// GPU contract fixture for the production shadow filters, fed analytic maps.
Texture2D<float> shadow_cascade0_texture : register(t0, space2);
Texture2D<float> shadow_cascade1_texture : register(t1, space2);
#define shadow_cascade2_texture shadow_cascade1_texture
#define shadow_cascade3_texture shadow_cascade1_texture
Texture2D<float> contact_depth_texture : register(t2, space2);
Texture2D<float> shadow_atlas_texture : register(t3, space2);
struct LightShadow { column_major float4x4 view_projection; float4 tile; };
cbuffer ProbeConstants : register(b0, space3) {
    column_major float4x4 shadow_view_projection[4];
    float4 shadow_cascade_texel_depth;
    float4 shadow_parameters;
    float4 shadow_quality;
    column_major float4x4 lighting_view_projection;
    column_major float4x4 lighting_inverse_view_projection;
    float4 lighting_canvas;
    float4 contact_shadow;
    float4 eye_position;
    uint4 grain_extent;
    uint4 counts;
    LightShadow light_shadows[1];
    float4 probe;
};
#include "shadow_filter.hlsli"
float4 main(float4 pixel : SV_Position) : SV_Target0 {
    const float x = pixel.x / float(grain_extent.z) * 2.0 - 1.0;
    const float3 position = float3(x, 0.0, probe.y + probe.z * x);
    const float3 normal = normalize(float3(-probe.z, 0.0, 1.0));
    float visibility;
    if (probe.x < 0.5) visibility = directional_shadow_visibility(position, normal, float3(0, 0, -1));
    else if (probe.x < 1.5) visibility = spot_shadow_visibility(0u, position, normal, 1.0);
    else visibility = contact_shadow_visibility(position, normal, normalize(float3(1, 0, 1)), contact_shadow.x);
    return float4(visibility, visibility, visibility, 1.0);
}
