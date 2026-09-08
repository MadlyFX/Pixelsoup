// Spatial antialiasing: an edge-directed blend in the FXAA family.
//
// This is the alternative to the temporal pass, not a degraded version of it.
// On a wall a camera is filming the two make genuinely different trades:
// temporal antialiasing resolves thin geometry far better, and pays for it
// with ghosting that gets integrated into the plate permanently and cannot be
// graded out. This softens an edge and forgets it immediately. A DP who has
// seen smear will take this one, so it is authored rather than inferred.
//
// Every tap is a Load and every branch is on values loaded the same way, so
// two nodes given the same pixels produce the same pixels. No sampler, no
// filtering, nothing whose last bit is an implementation's choice.

Texture2D<float4> source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

cbuffer SpatialAntialiasConstants : register(b0, space3) {
    // xy: target extent in pixels. z: relative contrast that counts as an
    // edge. w: absolute contrast floor, which keeps the pass off flat areas
    // where the only variation is quantisation.
    float4 aa_parameters;
};

float luma(float3 color) {
    // Rec.709. The pass is deciding what a human reads as an edge, so it
    // weights the channels the eye does.
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

int2 clamp_pixel(int2 pixel, int2 extent) {
    return clamp(pixel, int2(0, 0), extent - int2(1, 1));
}

float4 main(float4 position : SV_Position) : SV_Target {
    const int2 extent = int2(max(aa_parameters.xy, 1.0));
    const int2 pixel = clamp_pixel(int2(position.xy), extent);

    const float4 center = source.Load(int3(pixel, 0));
    const float4 north =
        source.Load(int3(clamp_pixel(pixel + int2(0, -1), extent), 0));
    const float4 south =
        source.Load(int3(clamp_pixel(pixel + int2(0, 1), extent), 0));
    const float4 west =
        source.Load(int3(clamp_pixel(pixel + int2(-1, 0), extent), 0));
    const float4 east =
        source.Load(int3(clamp_pixel(pixel + int2(1, 0), extent), 0));

    const float luma_center = luma(center.rgb);
    const float luma_north = luma(north.rgb);
    const float luma_south = luma(south.rgb);
    const float luma_west = luma(west.rgb);
    const float luma_east = luma(east.rgb);

    const float lowest = min(luma_center,
                             min(min(luma_north, luma_south),
                                 min(luma_west, luma_east)));
    const float highest = max(luma_center,
                              max(max(luma_north, luma_south),
                                  max(luma_west, luma_east)));
    const float range = highest - lowest;

    // Not an edge. Returning the source untouched rather than blending by a
    // tiny weight matters: this pass runs on every pixel of a wall, and a
    // uniform slight blur across flat areas is exactly the softness an
    // operator would blame the renderer for.
    if (range < max(aa_parameters.w, highest * aa_parameters.z)) {
        return center;
    }

    // Which way the edge runs. The second difference along each axis is large
    // across an edge and small along it.
    const float horizontal =
        abs(luma_west + luma_east - 2.0 * luma_center);
    const float vertical =
        abs(luma_north + luma_south - 2.0 * luma_center);
    const bool spans_horizontally = horizontal >= vertical;

    // Step toward the steeper side, so the blend crosses the edge rather than
    // running along it.
    const float near_side =
        spans_horizontally ? luma_west : luma_north;
    const float far_side = spans_horizontally ? luma_east : luma_south;
    const bool toward_far =
        abs(far_side - luma_center) >= abs(near_side - luma_center);
    const int2 step = spans_horizontally ? int2(1, 0) : int2(0, 1);
    const int2 neighbour_pixel =
        clamp_pixel(pixel + (toward_far ? step : -step), extent);
    const float4 neighbour = source.Load(int3(neighbour_pixel, 0));

    // Weighted by how much of an edge this actually is, so a hard silhouette
    // gets the full half-pixel blend and a faint gradient gets almost none.
    const float strength = saturate(range / max(highest, 1.0e-5));
    return lerp(center, 0.5 * (center + neighbour), strength);
}
