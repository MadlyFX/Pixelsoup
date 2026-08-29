Texture2D<float4> SourceTexture : register(t0, space2);
Texture2D<float4> MatteTexture : register(t1, space2);
SamplerState SourceSampler : register(s0, space2);
SamplerState MatteSampler : register(s1, space2);

cbuffer MatteParameters : register(b0, space3) {
    uint Mode;
    uint Invert;
    uint ClampInput;
    uint Padding;
};

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(Input input) : SV_Target0 {
    const float4 source = SourceTexture.Sample(SourceSampler, input.uv);
    const float4 matte = MatteTexture.Sample(MatteSampler, input.uv);

    float coverage = saturate(matte.a);
    if (Mode == 1U) {
        float3 straightMatte = 0.0F;
        if (matte.a > 1.0e-8F) {
            straightMatte = matte.rgb / matte.a;
            if (ClampInput != 0U) {
                straightMatte = saturate(straightMatte);
            }
        }
        const float luma = dot(straightMatte,
                               float3(0.2126F, 0.7152F, 0.0722F));
        coverage = saturate(luma * matte.a);
    }
    if (Invert != 0U) {
        coverage = 1.0F - coverage;
    }

    const float finalAlpha = saturate(source.a) * coverage;
    const float3 straightSource = source.a > 1.0e-8F
        ? source.rgb / source.a
        : 0.0F;
    return float4(straightSource * finalAlpha, finalAlpha);
}
