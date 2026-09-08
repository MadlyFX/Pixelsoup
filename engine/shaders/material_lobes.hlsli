// glTF KHR_materials_clearcoat, sheen, specular, ior and anisotropy.
// No screen-space coordinates, random seeds or history enter these lobes.
struct MaterialLobes {
    float coat;
    float coat_roughness;
    float3 coat_normal;
    float3 sheen;
    float sheen_roughness;
    float transmission;
    float3 dielectric_f0;
    float specular_weight;
    float anisotropy;
    float3 tangent;
    float3 bitangent;
};

float max_component(float3 v) { return max(v.x, max(v.y, v.z)); }

float3 material_fresnel(float3 f0, float weight, float vh) {
    return f0 + (weight - f0) * pow(saturate(1.0 - vh), 5.0);
}

float sheen_distribution(float nh, float roughness) {
    const float inverse = 1.0 / max(roughness * roughness, 0.001);
    return (2.0 + inverse) * pow(max(1.0 - nh * nh, 0.0), inverse * 0.5)
        / (2.0 * pi);
}

float sheen_visibility(float nv, float nl) {
    return 1.0 / max(4.0 * (nl + nv - nl * nv), 0.0001);
}

float anisotropic_dv(float3 n, float3 v, float3 l, float3 h,
                     float alpha, MaterialLobes m) {
    const float ax = max(lerp(alpha, 1.0, m.anisotropy * m.anisotropy), 0.001);
    const float ay = alpha;
    const float3 projected = float3(dot(m.tangent, h) / ax,
        dot(m.bitangent, h) / ay, dot(n, h));
    const float denominator = dot(projected, projected);
    const float distribution = 1.0 / max(pi * ax * ay * denominator * denominator, 1e-7);
    const float nl = saturate(dot(n, l));
    const float nv = saturate(dot(n, v));
    const float lambda_v = nl * length(float3(ax * dot(m.tangent, v), ay * dot(m.bitangent, v), nv));
    const float lambda_l = nv * length(float3(ax * dot(m.tangent, l), ay * dot(m.bitangent, l), nl));
    return distribution * 0.5 / max(lambda_v + lambda_l, 1e-7);
}

float3 extended_surface_response(float3 n, float3 v, float3 l, float3 albedo,
                                  float roughness, float metallic, MaterialLobes m) {
    const float3 h = normalize(v + l);
    const float nl = saturate(dot(n, l));
    const float nv = saturate(dot(n, v));
    const float alpha = max(roughness * roughness, 0.001);
    const float3 f0 = lerp(m.dielectric_f0, albedo, metallic);
    const float3 f = material_fresnel(f0, lerp(m.specular_weight, 1.0, metallic), saturate(dot(v, h)));
    const float dv = m.anisotropy > 0.0
        ? anisotropic_dv(n, v, l, h, alpha, m)
        : distribution_ggx(saturate(dot(n, h)), alpha) * visibility_smith(nv + 1e-5, nl, alpha);
    const float3 diffuse = (1.0 - f) * (1.0 - metallic) * (1.0 - m.transmission) * albedo / pi;
    float3 result = (diffuse + f * dv) * nl;
    // Charlie distribution / Neubelt visibility for the velvet lobe.
    // A conservative albedo scale prevents a second lobe adding free energy.
    const float sheen_weight = max_component(m.sheen);
    if (sheen_weight > 0.0) {
        const float sheen_brdf = sheen_distribution(saturate(dot(n, h)), m.sheen_roughness)
            * sheen_visibility(nv, nl);
        result = result * (1.0 - sheen_weight)
            + m.sheen * sheen_brdf * nl;
    }
    return result;
}

float3 clearcoat_response(float3 v, float3 l, MaterialLobes m) {
    const float3 h = normalize(v + l);
    const float nv = saturate(dot(m.coat_normal, v));
    const float nl = saturate(dot(m.coat_normal, l));
    const float alpha = max(m.coat_roughness * m.coat_roughness, 0.001);
    // The layer weight/Fresnel is applied once, after base emission and IBL.
    return (distribution_ggx(saturate(dot(m.coat_normal, h)), alpha)
        * visibility_smith(nv + 1e-5, nl, alpha) * nl).xxx;
}

#if defined(MEDIASERVER_SCENE_LINEAR)
float3 extended_environment(float3 n, float3 v, float3 albedo,
                             float roughness, float metallic, MaterialLobes m, float3 position) {
    const float3 f0 = lerp(m.dielectric_f0, albedo, metallic);
    const float weight = lerp(m.specular_weight, 1.0, metallic);
    float3 reflection_normal = n;
    if (m.anisotropy > 0.0) {
        const float3 bent = cross(cross(m.bitangent, v), m.bitangent);
        if (dot(bent, bent) > 1e-8)
            reflection_normal = normalize(lerp(n, normalize(bent), m.anisotropy * (1.0 - roughness)));
    }
    const float3 reflected = specular_texture.SampleLevel(linear_clamp_sampler,
        reflect(-v, reflection_normal), roughness * max(environment_parameters.y - 1.0, 0.0)).rgb;
    const float2 integrated = brdf_texture.SampleLevel(linear_clamp_sampler,
        float2(saturate(dot(n, v)), roughness), 0.0).rg;
    float3 result = reflected * (f0 * integrated.x + weight * integrated.y) * environment_parameters.x
        + diffuse_irradiance(n, position) / pi * albedo * (1.0 - metallic) * (1.0 - m.transmission);
    const float sheen_weight = max_component(m.sheen);
    if (sheen_weight > 0.0) {
        // Fixed hemisphere quadrature: Charlie has no GGX split-sum LUT. This
        // samples the environment's unfiltered level in world space, identically
        // on every slice, instead of feeding Charlie roughness to a GGX LUT.
        float3 sheen_light = 0.0;
        [loop] for (uint i = 0u; i < 32u; ++i) {
            const float z = ((float)i + 0.5) / 32.0;
            const float phi = (float)i * 2.39996323;
            const float radial = sqrt(1.0 - z * z);
            const float3 l = m.tangent * (cos(phi) * radial)
                + m.bitangent * (sin(phi) * radial) + n * z;
            const float3 h = normalize(v + l);
            const float response = sheen_distribution(saturate(dot(n, h)), m.sheen_roughness)
                * sheen_visibility(saturate(dot(n, v)), z) * z;
            sheen_light += specular_texture.SampleLevel(linear_clamp_sampler, l, 0.0).rgb * response;
        }
        result = result * (1.0 - sheen_weight) + m.sheen * sheen_light * (2.0 * pi / 32.0) * environment_parameters.x;
    }
    return result;
}
#endif
