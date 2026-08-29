// Luma Key changes coverage only. Source colour is recovered from its
// premultiplied representation and premultiplied again by the final gated
// alpha, keeping transparent padding transparent at every mix.

Texture2D<float4> source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);
Texture2D<float4> matte_source : register(t1, space2);
SamplerState matte_sampler : register(s1, space2);

cbuffer LumaKeyComposeConstants : register(b0, space3) {
    float key_mix;
    float3 padding;
};

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(Input input) : SV_Target0 {
    const float4 premultiplied = source.Sample(source_sampler, input.uv);
    const float matte = saturate(matte_source.Sample(matte_sampler, input.uv).a);
    const float3 rgb = premultiplied.a > 0.0
        ? saturate(premultiplied.rgb / premultiplied.a)
        : 0.0.xxx;
    const float final_alpha = premultiplied.a * lerp(1.0, matte, key_mix);
    return float4(rgb * final_alpha, final_alpha);
}
