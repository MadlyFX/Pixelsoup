// Coverage-aware world normals for timeline media.
//
// Same rule as the other media auxiliaries: the colour pass is premultiplied
// and may rasterize a quad whose sampled texel contributes nothing, so an
// exact-zero coverage must leave the nearer visible surface's normal alone
// rather than claiming the whole rectangular card.

Texture2D<float4> primary_plane : register(t0, space2);
SamplerState primary_sampler : register(s0, space2);
Texture2D<float2> chroma_plane : register(t1, space2);
SamplerState chroma_sampler : register(s1, space2);

#include "scene_media_coverage.hlsli"
#include "scene_media_sampling.hlsli"
#include "octahedral.hlsli"

struct Input {
    float4 position : SV_Position;
    float3 world_position : TEXCOORD0;
    float3 world_normal : TEXCOORD1;
    nointerpolation float4 base_color_roughness : TEXCOORD2;
    nointerpolation float4 emissive_metallic : TEXCOORD3;
    float2 uv : TEXCOORD4;
    nointerpolation float4 layer_style : TEXCOORD5;
    nointerpolation uint4 effect_range : TEXCOORD6;
    nointerpolation uint object_id : TEXCOORD7;
    float4 current_clip : TEXCOORD8;
    float4 previous_clip : TEXCOORD9;
    float4 world_tangent : TEXCOORD10;
    nointerpolation float4 material_properties : TEXCOORD11;
};

// Chroma-keyed sources carry their coverage in the key, not in an alpha
// channel, so a keyed clip is fully covered here and the key decides.
float sampled_alpha(Input input) {
    const MediaFootprint footprint = media_footprint(input.uv);
    return input.effect_range.z == 0u
        ? media_sample_rgba(primary_plane, primary_sampler, footprint).a
        : 1.0;
}

float2 main(Input input) : SV_Target0 {
    const float coverage = media_composed_coverage(
        sampled_alpha(input), input.uv, input.layer_style);
    if (input.layer_style.w > 0.5) {
        if (coverage < media_depth_only_coverage_cutoff) {
            discard;
        }
        return float2(0.0, 0.0);
    }
    if (coverage <= 0.0) {
        discard;
    }
    return octahedral_encode(normalize(input.world_normal));
}
