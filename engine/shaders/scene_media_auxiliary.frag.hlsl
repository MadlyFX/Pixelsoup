// Coverage-aware identity, motion and normals for timeline media in one pass,
// for a device that writes several targets at once. Each product is exactly
// what scene_media_object_id, scene_media_velocity and scene_media_normal
// write, under the one visibility equation they share.

Texture2D<float4> primary_plane : register(t0, space2);
SamplerState primary_sampler : register(s0, space2);
Texture2D<float2> chroma_plane : register(t1, space2);
SamplerState chroma_sampler : register(s1, space2);

#include "scene_media_coverage.hlsli"
#include "scene_motion_vector.hlsli"
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
    nointerpolation float motion_valid : TEXCOORD17;
};

struct AuxiliaryProducts {
    uint object_id : SV_Target0;
    float4 velocity : SV_Target1;
    float2 normal : SV_Target2;
};

// Chroma-keyed sources carry their coverage in the key, not in an alpha
// channel, so a keyed clip is fully covered here and the key decides.
float sampled_alpha(Input input) {
    return input.effect_range.z == 0u
        ? primary_plane.Sample(primary_sampler, input.uv).a
        : 1.0;
}

AuxiliaryProducts main(Input input) {
    const float coverage = media_composed_coverage(
        sampled_alpha(input), input.uv, input.layer_style);
    AuxiliaryProducts products;
    if (input.layer_style.w > 0.5) {
        // Match the colour pass's hard depth-only silhouette exactly; the
        // occluder claims no product of its own.
        if (coverage < media_depth_only_coverage_cutoff) {
            discard;
        }
        products.object_id = 0u;
        products.velocity = 0.0;
        products.normal = float2(0.0, 0.0);
        return products;
    }
    // Any nonzero premultiplied coverage is visible; only an exact zero
    // leaves the nearer surface's products and depth untouched.
    if (coverage <= 0.0) {
        discard;
    }
    products.object_id = input.object_id;
    products.velocity = motion_vector(input.current_clip, input.previous_clip, input.motion_valid);
    products.normal = octahedral_encode(normalize(input.world_normal));
    return products;
}
