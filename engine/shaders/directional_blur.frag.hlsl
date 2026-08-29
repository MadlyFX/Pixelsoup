// Centred one-line directional gather. The line's authored length is split
// into (taps - 1) equal canvas-pixel intervals. Both endpoints participate.

Texture2D<float4> source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

cbuffer DirectionalBlurConstants : register(b0, space3) {
    float2 texel_step;
    float2 start_offset;
    uint taps;
    uint3 padding;
};

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

static const int max_directional_taps = 65;

float4 main(Input input) : SV_Target0 {
    const int count = clamp((int)taps, 3, max_directional_taps);
    float4 result = 0.0;
    [loop]
    for (int index = 0; index < max_directional_taps; ++index) {
        if (index >= count) {
            break;
        }
        result += source.Sample(
            source_sampler,
            input.uv + start_offset + texel_step * float(index));
    }
    return result / float(count);
}
