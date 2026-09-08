Texture2D<float4> scene_colour : register(t0, space2);
SamplerState scene_sampler : register(s0, space2);

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float main(Input input) : SV_Target0 {
    return saturate(scene_colour.Sample(scene_sampler, input.uv).a);
}
