// Separable Gaussian blur, shared by the horizontal and vertical passes.
//
// The four bilinear samples are paired taps from a normalised nine-tap
// Gaussian kernel. Hardware linear filtering combines each adjacent pair, so
// the result has the same weights as nine point samples with five texture
// reads. `radius_px` scales the outer pair to the requested radius; the CPU
// supplies a one-axis texel step to select the horizontal or vertical pass.

Texture2D<float4> source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

cbuffer BlurConstants : register(b0, space3) {
    float2 texel_step;
    float radius_px;
    float padding;
};

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(Input input) : SV_Target0 {
    static const float centre_weight = 0.2270270270;
    static const float near_weight = 0.3162162162;
    static const float far_weight = 0.0702702703;
    static const float near_offset = 1.3846153846;
    static const float far_offset = 3.2307692308;

    const float2 scaled_step = texel_step * (radius_px / far_offset);
    float4 result = source.Sample(source_sampler, input.uv) * centre_weight;
    result += source.Sample(source_sampler,
                            input.uv + scaled_step * near_offset) * near_weight;
    result += source.Sample(source_sampler,
                            input.uv - scaled_step * near_offset) * near_weight;
    result += source.Sample(source_sampler,
                            input.uv + scaled_step * far_offset) * far_weight;
    result += source.Sample(source_sampler,
                            input.uv - scaled_step * far_offset) * far_weight;
    return result;
}
