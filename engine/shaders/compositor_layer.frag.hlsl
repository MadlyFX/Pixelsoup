// Premultiplied layer composition for the render-side compositor executor.
// Both inputs are completed transparent layer textures: `below` is the current
// accumulator and `above` is the isolated scene node after any spatial blur.

Texture2D<float4> below_texture : register(t0, space2);
SamplerState below_sampler : register(s0, space2);
Texture2D<float4> above_texture : register(t1, space2);
SamplerState above_sampler : register(s1, space2);

cbuffer CompositeConstants : register(b0, space3) {
    float blend_mode;
    float3 padding;
};

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float3 blend_colour(float3 backdrop, float3 source) {
    if (blend_mode > 0.5 && blend_mode < 1.5) { // Add
        return saturate(backdrop + source);
    }
    if (blend_mode > 1.5 && blend_mode < 2.5) { // Multiply
        return backdrop * source;
    }
    if (blend_mode > 2.5) { // Screen
        return backdrop + source - backdrop * source;
    }
    return source; // Normal
}

float4 main(Input input) : SV_Target0 {
    const float4 below = below_texture.Sample(below_sampler, input.uv);
    const float4 above = above_texture.Sample(above_sampler, input.uv);
    const float below_alpha = saturate(below.a);
    const float above_alpha = saturate(above.a);
    const float3 backdrop = below_alpha > 1e-6
        ? below.rgb / below_alpha
        : float3(0.0, 0.0, 0.0);
    const float3 source = above_alpha > 1e-6
        ? above.rgb / above_alpha
        : float3(0.0, 0.0, 0.0);
    const float3 blended = blend_colour(backdrop, source);

    // W3C source-over compositing with a separable blend function. The first
    // and third terms preserve a layer over transparency; the middle term is
    // where the two opaque colours actually interact.
    const float3 colour =
        above_alpha * (1.0 - below_alpha) * source
        + above_alpha * below_alpha * blended
        + (1.0 - above_alpha) * below_alpha * backdrop;
    const float alpha = above_alpha + below_alpha * (1.0 - above_alpha);
    return float4(saturate(colour), alpha);
}
