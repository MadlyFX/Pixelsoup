// The 2D UI, pixel stage. Samples one atlas and tints by the vertex colour, so a
// single pipeline draws both text and solid fills: a glyph's coverage is in the
// atlas alpha (rgb = 1), and a solid fill samples a white texel in the same
// atlas, so tex * colour is the fill colour. Alpha blending is enabled on the
// pipeline.
//
// Clipping is done here because the RHI exposes no scissor: SV_Position is the
// pixel centre in the target, and a pixel outside this batch's clip rectangle is
// discarded. The rectangle is a flat varying carried from the vertex stage.
//
// Texture and sampler at (t0, space2) and (s0, space2), the textured-stage
// convention.

Texture2D<float4> atlas : register(t0, space2);
SamplerState atlas_sampler : register(s0, space2);

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
    nointerpolation float4 clip_rect : TEXCOORD2;
};

float4 main(Input input) : SV_Target0 {
    if (input.position.x < input.clip_rect.x
        || input.position.x > input.clip_rect.z
        || input.position.y < input.clip_rect.y
        || input.position.y > input.clip_rect.w) {
        discard;
    }
    const float4 sampled = atlas.Sample(atlas_sampler, input.uv);
    return sampled * input.color;
}
