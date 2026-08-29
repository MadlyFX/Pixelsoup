// Luma-key band matte. Low and high softness extend outward from the ordered
// opaque interval; invert flips the complete matte after both gates combine.

Texture2D<float4> source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

cbuffer LumaKeyMatteConstants : register(b0, space3) {
    float low_threshold;
    float high_threshold;
    float low_softness;
    float high_softness;
    uint invert;
    uint3 padding;
};

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(Input input) : SV_Target0 {
    const float4 premultiplied = source.Sample(source_sampler, input.uv);
    const float3 rgb = premultiplied.a > 0.0
        ? saturate(premultiplied.rgb / premultiplied.a)
        : 0.0.xxx;
    const float y = dot(rgb, float3(0.2126, 0.7152, 0.0722));
    const float low_gate = low_softness == 0.0
        ? (y >= low_threshold ? 1.0 : 0.0)
        : smoothstep(low_threshold - low_softness, low_threshold, y);
    const float high_gate = high_softness == 0.0
        ? (y <= high_threshold ? 1.0 : 0.0)
        : 1.0 - smoothstep(high_threshold, high_threshold + high_softness, y);
    float matte = min(low_gate, high_gate);
    if (invert != 0) {
        matte = 1.0 - matte;
    }
    return matte.xxxx;
}
