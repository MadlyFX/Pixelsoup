// RHI conformance probe: samples an explicit mip without colour conversion.
Texture2D<float4> source_texture : register(t0, space2);
SamplerState source_sampler : register(s0, space2);
cbuffer Constants : register(b0, space3) { float4 parameters; };
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    const float2 at = parameters.y == 0.0 ? uv : uv * parameters.yz - 1.0;
    return parameters.w > 0.5
        ? source_texture.SampleBias(source_sampler, at, parameters.x)
        : source_texture.SampleLevel(source_sampler, at, parameters.x);
}
