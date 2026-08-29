// Applies the completed mask field to a premultiplied isolated layer. Scaling
// the complete float4 gates alpha and already-premultiplied colour together;
// transparent source padding therefore remains transparent even when a mask
// has positive expansion or inverted coverage.

Texture2D<float4> source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);
Texture2D<float4> mask_source : register(t1, space2);
SamplerState mask_sampler : register(s1, space2);

cbuffer MaskComposeConstants : register(b0, space3) {
    float4 reserved;
};

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(Input input) : SV_Target0 {
    const float4 premultiplied = source.Sample(source_sampler, input.uv);
    const float coverage = saturate(
        mask_source.Sample(mask_sampler, input.uv).r);
    return premultiplied * coverage;
}
