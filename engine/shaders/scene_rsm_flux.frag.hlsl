// Reflective-shadow-map diffuse flux for the first directional light. This is
// deliberately camera independent and non-temporal: every output node derives
// the same bounded virtual point-light field from the complete scene.

static const float pi = 3.14159265358979323846;

#include "material_coverage.hlsli"

cbuffer RsmFluxConstants : register(b0, space3) {
    // xyz is direction of light travel; w is intensity.
    float4 direction_intensity;
    float4 light_color;
};

#include "scene_varyings.hlsli"

float3 decode_srgb(float3 encoded_colour) {
    const float3 low = encoded_colour / 12.92;
    const float3 high = pow(
        max((encoded_colour + 0.055) / 1.055, 0.0), 2.4);
    return lerp(low, high, step(0.04045, encoded_colour));
}

float4 main(Varyings input, bool front_face : SV_IsFrontFace) : SV_Target0 {
    material_coverage(input.uv, input.anisotropy.w, input.alpha, light_color.w, front_face);
    if (input.layer_style.w > 0.5) {
        return 0.0;
    }
    const float3 normal = normalize(input.world_normal);
    const float n_dot_l = saturate(dot(
        normal, -normalize(direction_intensity.xyz)));
    const float3 albedo = input.base_color_roughness.rgb
        * decode_srgb(base_color_texture.SampleBias(
              base_color_sampler, input.uv, light_color.w).rgb);
    const float diffuse = 1.0 - saturate(input.emissive_metallic.w);
    const float3 direct_flux = albedo * diffuse * n_dot_l
        * light_color.rgb * direction_intensity.w / pi;
    // Authored emissive factor participates even before the remaining
    // emissive texture view is added to this compact RSM pass.
    return float4(direct_flux + input.emissive_metallic.rgb, 1.0);
}
