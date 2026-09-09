// Output transform, pixel stage.
//
// This is the OUTPUT stage of the architecture plan (§1.2): it samples the
// node's source - the region of the shared canvas this node is responsible for,
// already rendered into a texture - onto the output raster.
//
// A straight sample on the display-referred path: this product drives LED
// walls, where adjacent nodes abut pixel-exact and there is no overlap to
// blend. On the scene-linear path it is also the colour boundary - the baked
// display transform composed with the authored show look, then an ordered
// dither to the output's own bit depth - both colour operations applied last,
// on the node that owns the output. Per-panel calibration stays downstream on
// the LED processor for this deployment.
//
// Register spaces follow the SDL_GPU DXIL convention, the same as the other
// pixel stages here: sampled textures and samplers at (t[n], space2) / (s[n],
// space2).

Texture2D<float4> source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);
#if defined(MEDIASERVER_LINEAR_OUTPUT)
// The baked display transform, as slices of a cube laid left to right.
//
// A 2D atlas rather than a 3D texture because the filtering below is done by
// hand: a hardware sampler's precision is not guaranteed identical between
// implementations, and the two halves of a wall are graded by two machines. With
// the interpolation manual, a 3D texture buys nothing and costs an RHI change.
Texture2D<float4> display_lut : register(t1, space2);
SamplerState display_lut_sampler : register(s1, space2);
#endif

cbuffer OutputConstants : register(b0, space3) {
    float2 viewport_origin;
    float2 viewport_size;
    // x: the table's per-axis size. y: the linear value that lands halfway
    // along it, which is what the shaper below divides by. Both come from the
    // bake, so the shader cannot disagree with the table it was handed.
    float2 display_lut_parameters;
    // xy: added to an output pixel's coordinate to give its absolute canvas
    // coordinate, which is what keys the dither - the wall pixel decides the
    // threshold, never the node that happens to own it. z: quantisation
    // steps of the target, or zero for no dither. w: unused.
    float4 dither_parameters;
};

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

#if defined(MEDIASERVER_LINEAR_OUTPUT)
// One texel of the table, addressed the way `pack_display_lut_atlas` wrote it:
// slice `b` occupies columns `b * size`.
float3 display_lut_texel(int3 grid, int size) {
    const int3 clamped = clamp(grid, int3(0, 0, 0), int3(size - 1, size - 1,
                                                         size - 1));
    const int2 texel = int2(clamped.z * size + clamped.x, clamped.y);
    return display_lut.Load(int3(texel, 0)).rgb;
}

// Scene-linear to display, through the baked table.
//
// The shaper is `x / (x + k)` - monotonic over the whole positive range and
// exactly invertible - so the table covers every value a scene can produce
// without a separate clamp for highlights. It is the same expression the bake
// inverted, and display_transform.hpp holds both so they cannot drift.
float3 sample_display_lut(float3 linear_color) {
    const int size = max((int)display_lut_parameters.x, 2);
    const float pivot = max(display_lut_parameters.y, 1.0e-6);
    const float3 positive = max(linear_color, 0.0);
    const float3 shaped = positive / (positive + pivot);
    const float3 position = saturate(shaped) * (float)(size - 1);
    const int3 base = min((int3)floor(position), int3(size - 2, size - 2,
                                                      size - 2));
    const float3 fraction = position - (float3)base;

    // Trilinear by hand, in a fixed order, for the reason given at the binding.
    const float3 c000 = display_lut_texel(base + int3(0, 0, 0), size);
    const float3 c100 = display_lut_texel(base + int3(1, 0, 0), size);
    const float3 c010 = display_lut_texel(base + int3(0, 1, 0), size);
    const float3 c110 = display_lut_texel(base + int3(1, 1, 0), size);
    const float3 c001 = display_lut_texel(base + int3(0, 0, 1), size);
    const float3 c101 = display_lut_texel(base + int3(1, 0, 1), size);
    const float3 c011 = display_lut_texel(base + int3(0, 1, 1), size);
    const float3 c111 = display_lut_texel(base + int3(1, 1, 1), size);
    const float3 x00 = lerp(c000, c100, fraction.x);
    const float3 x10 = lerp(c010, c110, fraction.x);
    const float3 x01 = lerp(c001, c101, fraction.x);
    const float3 x11 = lerp(c011, c111, fraction.x);
    return lerp(lerp(x00, x10, fraction.y), lerp(x01, x11, fraction.y),
                fraction.z);
}

// The 8x8 Bayer matrix, mirrored from `display_transform.cpp`: every value
// from 0 to 63 exactly once, so the threshold averages exactly one half and
// the quantisation below is unbiased. The GPU output test compares pixels
// against the C++ reference, which is what holds the two copies together.
static const float bayer_8x8[64] = {
     0.0, 32.0,  8.0, 40.0,  2.0, 34.0, 10.0, 42.0,
    48.0, 16.0, 56.0, 24.0, 50.0, 18.0, 58.0, 26.0,
    12.0, 44.0,  4.0, 36.0, 14.0, 46.0,  6.0, 38.0,
    60.0, 28.0, 52.0, 20.0, 62.0, 30.0, 54.0, 22.0,
     3.0, 35.0, 11.0, 43.0,  1.0, 33.0,  9.0, 41.0,
    51.0, 19.0, 59.0, 27.0, 49.0, 17.0, 57.0, 25.0,
    15.0, 47.0,  7.0, 39.0, 13.0, 45.0,  5.0, 37.0,
    63.0, 31.0, 55.0, 23.0, 61.0, 29.0, 53.0, 21.0,
};

// The dither threshold for an absolute canvas pixel, in [0, 1). Integer
// masking, so a coordinate left of the origin wraps rather than mirrors, and
// so the value depends on nothing a driver is free to reassociate.
float dither_threshold(int2 canvas_pixel) {
    const int2 cell = canvas_pixel & int2(7, 7);
    return (bayer_8x8[cell.y * 8 + cell.x] + 0.5) / 64.0;
}
#endif

float4 main(Input input) : SV_Target0 {
    const float2 local_uv = (input.uv - viewport_origin) / viewport_size;
    if (any(local_uv < 0.0) || any(local_uv >= 1.0)) {
        discard;
    }
    const float4 sampled = source.Sample(source_sampler, local_uv);
#if defined(MEDIASERVER_LINEAR_OUTPUT)
    const float alpha = saturate(sampled.a);
    const float3 straight = alpha > 1.0e-6
        ? max(sampled.rgb, 0.0) / alpha
        : float3(0.0, 0.0, 0.0);
    const float3 encoded = sample_display_lut(straight);
    float4 result = float4(encoded * alpha, alpha);
    if (dither_parameters.z > 0.0) {
        // SV_Position is the pixel centre, so the floor is the pixel; the
        // offset then puts it on the wall. Quantised on the premultiplied value
        // the target actually stores, so the stored code is exactly the one
        // chosen here rather than re-rounded by the format conversion.
        const int2 canvas_pixel =
            int2(floor(input.position.xy)) + int2(dither_parameters.xy);
        const float threshold = dither_threshold(canvas_pixel);
        const float levels = dither_parameters.z;
        result.rgb = floor(saturate(result.rgb) * levels + threshold) / levels;
    }
    // Float swapchains use linear Rec.709/scRGB; integer surfaces carry encoded SDR.
    if (dither_parameters.w > 0.5) {
        const float3 c = max(encoded, 0.0);
        result.rgb = lerp(c / 12.92, pow((c + 0.055) / 1.055, 2.4), step(0.04045, c)) * alpha;
    }
    return result;
#else
    if (dither_parameters.w > 0.5) {
        const float3 c = max(sampled.rgb, 0.0);
        return float4(lerp(c / 12.92, pow((c + 0.055) / 1.055, 2.4), step(0.04045, c)), sampled.a);
    }
    return sampled;
#endif
}
