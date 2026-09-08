// Coverage-aware object identity for timeline media.
//
// The colour pass is premultiplied and may therefore rasterize a quad whose
// sampled texel contributes no colour at all.  Object identity is categorical,
// so an exact-zero source coverage must leave the nearer visible surface's ID
// untouched rather than claiming the whole rectangular card.

Texture2D<float4> primary_plane : register(t0, space2);
SamplerState primary_sampler : register(s0, space2);
Texture2D<float2> chroma_plane : register(t1, space2);
SamplerState chroma_sampler : register(s1, space2);

#include "scene_media_coverage.hlsli"

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

float sampled_alpha(Input input) {
    // DecodedPixelLayout::Rgba8. P010 is an opaque video plane and therefore
    // has no authored alpha channel to sample.
    return input.effect_range.z == 0u
        ? primary_plane.Sample(primary_sampler, input.uv).a
        : 1.0;
}

float visible_coverage(Input input) {
    return media_composed_coverage(
        sampled_alpha(input), input.uv, input.layer_style);
}

uint main(Input input) : SV_Target0 {
    const float coverage = visible_coverage(input);
    if (input.layer_style.w > 0.5) {
        // Match the colour pass's hard depth-only silhouette exactly. A soft
        // or translucent source cannot write a fractional depth value.
        if (coverage < media_depth_only_coverage_cutoff) {
            discard;
        }
        return 0u;
    }
    // Media has no authored alpha-mask cutoff. Any nonzero premultiplied
    // coverage is visible; only an exact zero leaves ID and depth untouched.
    if (coverage <= 0.0) {
        discard;
    }
    return input.object_id;
}
