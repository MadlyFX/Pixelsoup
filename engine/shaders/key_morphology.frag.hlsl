// One axis of square key-matte morphology. The CPU records horizontal then
// vertical passes. Coverage beyond the allocated canvas is transparent, not a
// clamped copy of the edge pixel.

Texture2D<float4> matte_source : register(t0, space2);
SamplerState matte_sampler : register(s0, space2);

cbuffer KeyMorphologyConstants : register(b0, space3) {
    float2 texel_step;
    uint radius_px;
    uint dilate;
};

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

static const int max_key_morphology_radius = 64;

float sample_matte(float2 uv) {
    if (any(uv < 0.0) || any(uv >= 1.0)) {
        return 0.0;
    }
    return matte_source.Sample(matte_sampler, uv).a;
}

float4 main(Input input) : SV_Target0 {
    const int radius = min((int)radius_px, max_key_morphology_radius);
    float selected = sample_matte(input.uv);
    [loop]
    for (int offset = 1; offset <= max_key_morphology_radius; ++offset) {
        if (offset > radius) {
            break;
        }
        const float2 delta = texel_step * float(offset);
        const float lower = sample_matte(input.uv - delta);
        const float upper = sample_matte(input.uv + delta);
        if (dilate != 0) {
            selected = max(selected, max(lower, upper));
        } else {
            selected = min(selected, min(lower, upper));
        }
    }
    return selected.xxxx;
}
