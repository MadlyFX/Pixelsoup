// Merge composition for the render-side compositor executor: one image lands
// on the accumulator below it with a blend mode and an opacity.
//
// `above` is either a completed premultiplied texture or, for a Background,
// generated here from the authored canvas position of each pixel. Before it
// lands it may be blended against a constant - a Merge with nothing wired to
// its LAYERS - inside its own coverage, so transparency stays transparent.
//
// Normal, Add, Multiply and Screen are the same arithmetic as
// compositor_layer.frag.hlsl.

Texture2D<float4> below_texture : register(t0, space2);
SamplerState below_sampler : register(s0, space2);
Texture2D<float4> above_texture : register(t1, space2);
SamplerState above_sampler : register(s1, space2);

cbuffer MergeConstants : register(b0, space3) {
    uint blend_mode;
    float opacity;
    uint scene_linear;
    // 0 samples above_texture; 1 generates a Background.
    uint source;

    uint constant_blend;
    float constant_value;
    float constant_opacity;
    uint constant_enabled;

    // 0 solid, 1 linear, 2 radial.
    uint gradient;
    // Reciprocal of the ramp length (linear) or radius (radial), in canvas
    // pixels.
    float inv_extent;
    float2 direction;

    // Canvas pixels.
    float2 center;
    // Canvas position = canvas_origin + target pixel * canvas_step.
    float2 canvas_origin;

    float2 canvas_step;
    float2 padding;

    // Straight, display-referred colours.
    float4 color_a;
    float4 color_b;
};

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float3 blend_colour(uint mode, float3 backdrop, float3 source_colour) {
    if (mode == 1u) { // Add
        return scene_linear != 0u ? backdrop + source_colour
                                  : saturate(backdrop + source_colour);
    }
    if (mode == 2u) { // Multiply
        return backdrop * source_colour;
    }
    if (mode == 3u) { // Screen
        return backdrop + source_colour - backdrop * source_colour;
    }
    if (mode == 4u) { // Subtract
        return max(backdrop - source_colour, 0.0);
    }
    if (mode == 5u) { // Overlay: multiply the shadows, screen the highlights
        const float3 low = 2.0 * backdrop * source_colour;
        const float3 high =
            1.0 - 2.0 * (1.0 - backdrop) * (1.0 - source_colour);
        return lerp(low, high, step(0.5, backdrop));
    }
    if (mode == 6u) { // Lighten
        return max(backdrop, source_colour);
    }
    if (mode == 7u) { // Darken
        return min(backdrop, source_colour);
    }
    if (mode == 8u) { // Difference
        return abs(backdrop - source_colour);
    }
    return source_colour; // Normal
}

float3 finish(float3 colour) {
    // Physical sky and photometric lights can exceed display white. Preserve
    // that energy until the output transform.
    return scene_linear != 0u ? clamp(colour, 0.0, 65504.0) : saturate(colour);
}

// The same sRGB transfer the media shaders use: an operator's colour is a
// display colour, so the Advanced path works on its linear light.
float3 decode_srgb(float3 encoded) {
    const float3 low = encoded / 12.92;
    const float3 high = pow((encoded + 0.055) / 1.055, 2.4);
    return lerp(low, high, step(0.04045, encoded));
}

float4 generate(float2 pixel) {
    const float2 canvas = canvas_origin + pixel * canvas_step;
    float t = 0.0;
    if (gradient == 1u) {
        t = dot(canvas - center, direction) * inv_extent + 0.5;
    } else if (gradient == 2u) {
        t = length(canvas - center) * inv_extent;
    }
    t = saturate(t);
    // Premultiplied interpolation, so a transparent end contributes no
    // colour of its own to the ramp.
    const float4 from = float4(color_a.rgb * color_a.a, color_a.a);
    const float4 to = float4(color_b.rgb * color_b.a, color_b.a);
    const float4 mixed = lerp(from, to, t);
    const float alpha = saturate(mixed.a);
    const float3 straight = alpha > 1e-6 ? saturate(mixed.rgb / alpha)
                                         : float3(0.0, 0.0, 0.0);
    const float3 colour =
        scene_linear != 0u ? decode_srgb(straight) : straight;
    return float4(colour * alpha, alpha);
}

float4 blend_constant(float4 image) {
    const float alpha = saturate(image.a);
    if (alpha <= 1e-6) {
        return image;
    }
    const float3 colour = image.rgb / alpha;
    const float3 blended = blend_colour(
        constant_blend, colour, float3(constant_value, constant_value,
                                       constant_value));
    return float4(finish(lerp(colour, blended, constant_opacity)) * alpha,
                  image.a);
}

float4 main(Input input) : SV_Target0 {
    const float4 below = below_texture.Sample(below_sampler, input.uv);
    float4 above = source == 1u
        ? generate(input.position.xy)
        : above_texture.Sample(above_sampler, input.uv);
    if (constant_enabled != 0u) {
        above = blend_constant(above);
    }
    const float below_alpha = saturate(below.a);
    const float raw_alpha = saturate(above.a);
    const float above_alpha = raw_alpha * opacity;
    const float3 backdrop = below_alpha > 1e-6
        ? below.rgb / below_alpha
        : float3(0.0, 0.0, 0.0);
    const float3 source_colour = raw_alpha > 1e-6
        ? above.rgb / raw_alpha
        : float3(0.0, 0.0, 0.0);
    const float3 blended = blend_colour(blend_mode, backdrop, source_colour);

    // W3C source-over compositing with a separable blend function, as in
    // compositor_layer.frag.hlsl, with the layer's coverage scaled by opacity.
    const float3 colour =
        above_alpha * (1.0 - below_alpha) * source_colour
        + above_alpha * below_alpha * blended
        + (1.0 - above_alpha) * below_alpha * backdrop;
    const float alpha = above_alpha + below_alpha * (1.0 - above_alpha);
    return float4(finish(colour), alpha);
}
