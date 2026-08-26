// Program/Rehearsal transition over two completed scene textures.

Texture2D<float4> from_scene : register(t0, space2);
SamplerState from_sampler : register(s0, space2);
Texture2D<float4> to_scene : register(t1, space2);
SamplerState to_sampler : register(s1, space2);

cbuffer MixConstants : register(b0, space3) {
    float mix_amount;
    float3 padding;
};

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(Input input) : SV_Target0 {
    const float4 from_color = from_scene.Sample(from_sampler, input.uv);
    const float4 to_color = to_scene.Sample(to_sampler, input.uv);
    return lerp(from_color, to_color, saturate(mix_amount));
}
