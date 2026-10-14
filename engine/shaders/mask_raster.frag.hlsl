// Ordered scene-mask rasterization. Each fullscreen pass evaluates one active
// mask at exact pixel centres. The first Add operation seeds coverage; later
// passes sample the previous UNORM coverage and apply the authored fold.
//
// Rectangle and ellipse use analytic signed-distance functions. Closed cubic
// Bezier segments use eight deterministic de Casteljau chords each; CPU and
// GPU share this fixed construction so adaptive floating-point branch choices
// can never diverge between output nodes.

Texture2D<float4> accumulated_source : register(t0, space2);
SamplerState accumulated_sampler : register(s0, space2);

static const uint max_mask_points = 256u;
static const uint bezier_subdivisions = 8u;

cbuffer MaskRasterConstants : register(b0, space3) {
    // Rows of the projective map from (output u, output v, 1) to mask
    // coordinates times a positive depth; w is unused.
    float4 mapping[3];
    // xy: normalized-coordinate extent in authored canvas pixels;
    // z: feather pixels; w: signed expansion pixels.
    float4 geometry;
    // x: opacity; y: MaskKind; z: MaskCombine; w: invert.
    float4 operation;
    // x: point count; y: first-operation seed flag.
    uint4 counts;
    // Two float4 records per point: (position.xy, in_handle.xy), then
    // (out_handle.xy, 0, 0). Handles are absolute positions.
    float4 point_records[max_mask_points * 2u];
};

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float cross_2d(float2 left, float2 right) {
    return left.x * right.y - left.y * right.x;
}

float distance_sq_to_segment(
    float2 sample_position, float2 start, float2 finish) {
    const float2 direction = finish - start;
    const float denominator = dot(direction, direction);
    const float parameter = denominator > 0.0
        ? saturate(dot(sample_position - start, direction) / denominator)
        : 0.0;
    const float2 delta = sample_position - (start + direction * parameter);
    return dot(delta, delta);
}

void accumulate_segment(
    float2 sample_position,
    float2 start,
    float2 finish,
    inout float minimum_distance_sq,
    inout int winding) {
    minimum_distance_sq = min(
        minimum_distance_sq,
        distance_sq_to_segment(sample_position, start, finish));
    if (start.y <= sample_position.y) {
        if (finish.y > sample_position.y
            && cross_2d(finish - start, sample_position - start) > 0.0) {
            ++winding;
        }
    } else if (finish.y <= sample_position.y
               && cross_2d(
                   finish - start, sample_position - start) < 0.0) {
        --winding;
    }
}

float2 point_position(uint index) {
    return point_records[index * 2u].xy * geometry.xy;
}

float2 point_in_handle(uint index) {
    return point_records[index * 2u].zw * geometry.xy;
}

float2 point_out_handle(uint index) {
    return point_records[index * 2u + 1u].xy * geometry.xy;
}

float2 cubic_point(
    float2 p0, float2 p1, float2 p2, float2 p3, float parameter) {
    const float2 a = lerp(p0, p1, parameter);
    const float2 b = lerp(p1, p2, parameter);
    const float2 c = lerp(p2, p3, parameter);
    const float2 d = lerp(a, b, parameter);
    const float2 e = lerp(b, c, parameter);
    return lerp(d, e, parameter);
}

float rectangle_distance(float2 sample_position) {
    const float2 lower = point_records[0u].xy * geometry.xy;
    const float2 upper = point_records[0u].zw * geometry.xy;
    const float2 centre = (lower + upper) * 0.5;
    const float2 half_extent = (upper - lower) * 0.5;
    const float angle = point_records[1u].x * 6.28318530717958647692;
    const float cosine = cos(angle);
    const float sine = sin(angle);
    const float2 held = sample_position - centre;
    const float2 local = float2(
        cosine * held.x + sine * held.y,
        -sine * held.x + cosine * held.y);
    const float2 delta = abs(local) - half_extent;
    return length(max(delta, 0.0.xx)) + min(max(delta.x, delta.y), 0.0);
}

float ellipse_distance(float2 sample_position) {
    const float2 lower = point_records[0u].xy * geometry.xy;
    const float2 upper = point_records[0u].zw * geometry.xy;
    const float2 centre = (lower + upper) * 0.5;
    const float2 radius = max((upper - lower) * 0.5, 1.0e-6.xx);
    const float angle = point_records[1u].x * 6.28318530717958647692;
    const float cosine = cos(angle);
    const float sine = sin(angle);
    const float2 held = sample_position - centre;
    const float2 local = float2(
        cosine * held.x + sine * held.y,
        -sine * held.x + cosine * held.y);
    const float k0 = length(local / radius);
    const float k1 = length(local / (radius * radius));
    return k1 > 1.0e-8
        ? k0 * (k0 - 1.0) / k1
        : -min(radius.x, radius.y);
}

float bezier_distance(float2 sample_position) {
    float minimum_distance_sq = 3.402823466e+38;
    int winding = 0;
    [loop]
    for (uint index = 0u; index < counts.x; ++index) {
        const uint next = index + 1u == counts.x ? 0u : index + 1u;
        const float2 p0 = point_position(index);
        const float2 p1 = point_out_handle(index);
        const float2 p2 = point_in_handle(next);
        const float2 p3 = point_position(next);
        float2 start = p0;
        [unroll]
        for (uint sample = 1u; sample <= bezier_subdivisions; ++sample) {
            const float parameter =
                (float)sample / (float)bezier_subdivisions;
            const float2 finish = cubic_point(p0, p1, p2, p3, parameter);
            accumulate_segment(
                sample_position, start, finish,
                minimum_distance_sq, winding);
            start = finish;
        }
    }
    const float distance_to_path = sqrt(max(minimum_distance_sq, 0.0));
    return winding != 0 ? -distance_to_path : distance_to_path;
}

float operation_coverage(float2 output_uv) {
    // Mirrors the CPU reference term by term. A pixel whose depth is not
    // positive lies beyond the source plane's horizon: outside every shape.
    const float depth = mapping[2].x * output_uv.x
        + mapping[2].y * output_uv.y + mapping[2].z;
    float signed_distance = 3.402823466e+38;
    if (depth > 0.0) {
        const float2 mask_coordinate = float2(
            (mapping[0].x * output_uv.x + mapping[0].y * output_uv.y
                + mapping[0].z) / depth,
            (mapping[1].x * output_uv.x + mapping[1].y * output_uv.y
                + mapping[1].z) / depth);
        const float2 sample_position = mask_coordinate * geometry.xy;
        const uint kind = (uint)operation.y;
        if (kind == 0u) {
            signed_distance = rectangle_distance(sample_position);
        } else if (kind == 1u) {
            signed_distance = ellipse_distance(sample_position);
        } else {
            signed_distance = bezier_distance(sample_position);
        }
    }
    signed_distance -= geometry.w;

    float coverage = signed_distance <= 0.0 ? 1.0 : 0.0;
    if (geometry.z > 0.0) {
        const float parameter = saturate(
            (geometry.z - signed_distance) / (2.0 * geometry.z));
        coverage = parameter * parameter * (3.0 - 2.0 * parameter);
    }
    if (operation.w > 0.5) {
        coverage = 1.0 - coverage;
    }
    return saturate(coverage * operation.x);
}

float4 main(Input input) : SV_Target0 {
    const float held = counts.y != 0u
        ? 0.0
        : saturate(accumulated_source.Sample(
              accumulated_sampler, input.uv).r);
    const float coverage = operation_coverage(input.uv);
    float folded = coverage;
    if (counts.y == 0u) {
        const uint combine = (uint)operation.z;
        if (combine == 0u) {
            folded = max(held, coverage);
        } else if (combine == 1u) {
            folded = held * (1.0 - coverage);
        } else if (combine == 2u) {
            folded = min(held, coverage);
        } else {
            folded = abs(held - coverage);
        }
    }
    return saturate(folded).xxxx;
}
