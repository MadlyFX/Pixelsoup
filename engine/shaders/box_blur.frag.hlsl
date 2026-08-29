// Exact separable box filter. The CPU records one horizontal and one vertical
// pass for every authored pass. Radius is an integer canvas-pixel distance, so
// each axis averages exactly (2 * radius + 1) equally weighted samples.

Texture2D<float4> source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

cbuffer BoxBlurConstants : register(b0, space3) {
    float2 texel_step;
    uint radius_px;
    uint padding;
};

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

static const int max_box_radius = 256;

float4 main(Input input) : SV_Target0 {
    const int radius = min((int)radius_px, max_box_radius);
    float4 result = source.Sample(source_sampler, input.uv);
    [loop]
    for (int offset = 1; offset <= max_box_radius; ++offset) {
        if (offset > radius) {
            break;
        }
        const float2 step = texel_step * float(offset);
        result += source.Sample(source_sampler, input.uv + step);
        result += source.Sample(source_sampler, input.uv - step);
    }
    return result / float(radius * 2 + 1);
}
