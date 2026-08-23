// Instanced quad, pixel stage.
//
// One coverage atlas serves both kinds of instance the pattern draws. A glyph
// cell points at its 5x7 bitmap; a solid rectangle points at a texel that is
// fully covered. That is what lets the entire frame be a single draw call
// rather than one pass for shapes and another for text.
//
// Register spaces follow the SDL_GPU DXIL convention: a pixel stage's sampled
// textures and their samplers live at (t[n], space2) and (s[n], space2).

Texture2D<float4> atlas : register(t0, space2);
SamplerState atlas_sampler : register(s0, space2);

struct Input {
    float4 position : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

float4 main(Input input) : SV_Target0 {
    // The atlas stores coverage, not colour, so only one channel is read and
    // the instance's own colour supplies the rest.
    const float coverage = atlas.Sample(atlas_sampler, input.uv).r;
    return float4(input.color.rgb, input.color.a * coverage);
}
