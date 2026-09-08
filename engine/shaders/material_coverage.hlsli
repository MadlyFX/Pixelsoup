#ifndef MEDIASERVER_MATERIAL_COVERAGE_HLSLI
#define MEDIASERVER_MATERIAL_COVERAGE_HLSLI

Texture2D<float4> base_color_texture : register(t0, space2);
SamplerState base_color_sampler : register(s0, space2);

// Shared by shadow, RSM, depth/normal, identity and velocity replays. Alpha is
// linear data; use the material's own UV, sampler, opacity and MASK threshold.
void material_coverage(float2 uv, float opacity, float4 alpha, float lod_bias,
                       bool front_face) {
    if (!front_face && alpha.z < 0.5) discard;
    if (alpha.y > 0.5 && alpha.y < 1.5) {
        const float coverage = saturate(opacity * base_color_texture.SampleBias(
            base_color_sampler, uv, lod_bias).a);
        if (coverage < alpha.x) discard;
    }
}
#endif
