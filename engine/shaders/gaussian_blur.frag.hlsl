// Separable Gaussian blur, shared by the horizontal and vertical passes.
//
// The kernel is built here rather than baked in, because its width and its
// sigma are project settings: a facility that wants a soft, wide defocus and
// one that wants a tight bloom need different kernels from the same authored
// radius. `half_taps` samples are taken on each side of the centre, evenly
// spaced out to `radius_px`, and weighted by a Gaussian of the given sigma.
// The weights are normalised by their own sum, so truncating the tail dims
// nothing - it only narrows the response.
//
// The CPU supplies a one-axis texel step to select the horizontal or vertical
// pass, and never lets `half_taps` exceed the loop bound below.

Texture2D<float4> source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

cbuffer BlurConstants : register(b0, space3) {
    float2 texel_step;
    float radius_px;
    float sigma_px;
    float half_taps;
    float3 padding;
};

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// (max_blur_taps - 1) / 2 in scene.hpp. A compile-time bound is required: the
// loop has to be unrollable and the shader must never depend on a uniform for
// its worst case.
static const int max_half_taps = 16;

float4 main(Input input) : SV_Target0 {
    const int half_count = clamp((int)half_taps, 1, max_half_taps);
    const float sigma = max(sigma_px, 1e-4);
    const float denominator = 2.0 * sigma * sigma;

    float4 result = source.Sample(source_sampler, input.uv);
    float total = 1.0;
    [loop]
    for (int index = 1; index <= max_half_taps; ++index) {
        if (index > half_count) {
            break;
        }
        const float offset =
            (float(index) / float(half_count)) * radius_px;
        const float weight = exp(-(offset * offset) / denominator);
        const float2 step = texel_step * offset;
        result += source.Sample(source_sampler, input.uv + step) * weight;
        result += source.Sample(source_sampler, input.uv - step) * weight;
        total += 2.0 * weight;
    }
    return result / total;
}
