// Screen-space ambient occlusion, horizon search.
//
// With image-based lighting landed, ambient became a bright omnidirectional
// term that reaches into creases, under furniture and into the contact between
// an object and the floor. Without occlusion every object starts to float, and
// on a volume a prop that hovers a centimetre above the floor is the single
// most obvious thing a camera records. This is what stops the environment from
// flattening the image; the two are close to a package deal.
//
// The baked `Material::occlusion_texture` path already handles detail inside
// one asset. What no baked texture can know is occlusion *between* separate
// objects, which is exactly what this computes.

#include "deterministic_hash.hlsli"
#include "octahedral.hlsli"

// Compute binds in the default space, unlike the graphics passes: textures at
// t0, their samplers at s0, storage at u0, and the constant buffer in space1.
// The samplers are declared to match the pipeline's table and deliberately
// unused - every read below is a Load, because a filtered tap would make the
// occlusion depend on an implementation's filtering precision.
Texture2D<float> scene_depth : register(t0);
SamplerState depth_sampler : register(s0);
Texture2D<float2> scene_normal : register(t1);
SamplerState normal_sampler : register(s1);
RWTexture2D<float> occlusion : register(u0);

cbuffer OcclusionConstants : register(b0, space1) {
    // Clip to world, for reconstructing a position per pixel.
    column_major float4x4 inverse_view_projection;
    // xy: target extent in pixels. zw: this slice's origin in absolute canvas
    // pixels, so the sample rotation below is keyed to the wall rather than to
    // whichever part of it this node happens to own.
    float4 occlusion_extent;
    // x: world-space radius. y: strength. z: low 32 bits of the show frame.
    // w: high 32 bits.
    float4 occlusion_parameters;
};

// Fixed. Both counts are compiled into the loop bounds precisely so that no
// node can decide to take fewer samples than its neighbour under load - an
// adaptive quality dial is a seam, not an optimisation.
static const uint occlusion_directions = 8u;
static const uint occlusion_steps = 6u;
static const float pi = 3.14159265;

float3 world_from_depth(float2 uv, float depth) {
    // Reversed depth: the near plane is one. The projection already encodes
    // that, so nothing here has to know about it beyond passing the value
    // through unmodified.
    const float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    const float4 world = mul(inverse_view_projection, clip);
    return world.xyz / (abs(world.w) > 1.0e-12 ? world.w : 1.0e-12);
}

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    const int2 extent = int2(max(occlusion_extent.xy, 1.0));
    const int2 pixel = int2(thread.xy);
    if (pixel.x >= extent.x || pixel.y >= extent.y) {
        return;
    }

    const float depth = scene_depth.Load(int3(pixel, 0));
    // Nothing was drawn here. Background is unoccluded, and writing one rather
    // than skipping keeps the target fully defined for whatever reads it.
    if (depth <= 0.0) {
        occlusion[pixel] = 1.0;
        return;
    }

    const float2 extent_f = float2(extent);
    const float2 uv = (float2(pixel) + 0.5) / extent_f;
    const float3 position = world_from_depth(uv, depth);
    const float3 normal = octahedral_decode(scene_normal.Load(int3(pixel, 0)));

    // The sample rotation, from absolute canvas coordinates and the exact show
    // frame. Two nodes asked about the same wall pixel on the same frame get
    // the same rotation, so the two halves of a crease are sampled the same
    // way and the seam through it does not move.
    const uint2 canvas_pixel = uint2(pixel) + uint2(occlusion_extent.zw);
    uint state = 0x811c9dc5u;
    state = deterministic_mix(state, canvas_pixel.x);
    state = deterministic_mix(state, canvas_pixel.y);
    state = deterministic_mix(state, asuint(occlusion_parameters.z));
    state = deterministic_mix(state, asuint(occlusion_parameters.w));
    const float rotation = deterministic_unit(state);

    const float radius = max(occlusion_parameters.x, 1.0e-4);
    // How many pixels that world radius spans at this depth, measured rather
    // than approximated: reconstruct the pixel next door at the same depth and
    // see how far it moved in world space. A fixed pixel radius would occlude
    // a distant wall as strongly as a near one, and an approximation from the
    // depth value alone would be a different approximation on a node whose
    // slice has a different aspect.
    const float3 neighbour =
        world_from_depth(uv + float2(1.0 / extent_f.x, 0.0), depth);
    const float world_per_pixel = max(length(neighbour - position), 1.0e-6);
    // Clamped to the apron the slice reserves for this pass. A search that
    // reached further would read past the padded region, and on a node that is
    // the edge of the world rather than more scene - it would occlude against
    // whatever the border happened to hold, along the seam.
    const float screen_radius = clamp(radius / world_per_pixel, 2.0, 32.0);

    float visibility = 0.0;
    [unroll] for (uint direction = 0u; direction < occlusion_directions;
                  ++direction) {
        const float angle =
            (float(direction) + rotation) * (2.0 * pi / float(occlusion_directions));
        const float2 step_direction = float2(cos(angle), sin(angle));
        float horizon = 0.0;
        [unroll] for (uint step = 1u; step <= occlusion_steps; ++step) {
            const float2 tap = float2(pixel) + 0.5
                + step_direction * (screen_radius * float(step)
                                    / float(occlusion_steps));
            const int2 tap_pixel =
                clamp(int2(tap), int2(0, 0), extent - int2(1, 1));
            const float tap_depth = scene_depth.Load(int3(tap_pixel, 0));
            if (tap_depth <= 0.0) {
                continue;
            }
            const float2 tap_uv = (float2(tap_pixel) + 0.5) / extent_f;
            const float3 tap_position = world_from_depth(tap_uv, tap_depth);
            const float3 delta = tap_position - position;
            const float distance = length(delta);
            if (distance < 1.0e-5 || distance > radius) {
                continue;
            }
            // How far above the horizon this sample sits, faded out with
            // distance so a wall across the room does not occlude a desk.
            const float elevation = dot(normal, delta / distance);
            const float falloff = saturate(1.0 - distance / radius);
            horizon = max(horizon, elevation * falloff);
        }
        visibility += horizon;
    }

    const float occluded = saturate(visibility / float(occlusion_directions));
    occlusion[pixel] = saturate(1.0 - occluded * occlusion_parameters.y);
}
