// Local radiance uses an authored world-space influence, reduced in document
// node order. Neither the slice nor camera distance selects a probe.
float3 reflection_radiance(float3 position, float3 direction, float roughness,
                           float2 pixel, float planar_weight) {
    float3 local = 0.0;
    float total = 0.0;
    [unroll] for (uint i = 0; i < 4; ++i) {
        if (reflection_parameters[i].w == 0.0) continue;
        const float3 p = position - reflection_position[i].xyz;
        const float3 extent = reflection_extent[i].xyz;
        const bool sphere = reflection_position[i].w > 0.0;
        const float edge = sphere ? length(p) / extent.x : max(max(abs(p.x) / extent.x, abs(p.y) / extent.y), abs(p.z) / extent.z);
        const float weight = saturate((1.0 - edge) / reflection_parameters[i].x);
        if (weight <= 0.0) continue;
        float distance;
        if (sphere) {
            const float b = dot(p, direction);
            distance = -b + sqrt(max(b * b + extent.x * extent.x - dot(p, p), 0.0));
        } else {
            const float3 sign_direction = float3(direction.x >= 0.0 ? 1.0 : -1.0, direction.y >= 0.0 ? 1.0 : -1.0, direction.z >= 0.0 ? 1.0 : -1.0);
            const float3 exits = (sign_direction * extent - p) / (sign_direction * max(abs(direction), 1e-8));
            distance = min(min(exits.x, exits.y), exits.z);
        }
        const float3 corrected = p + direction * distance;
        const float level = saturate(roughness) * (reflection_parameters[i].z - 1.0);
        float3 radiance;
        if (i == 0) radiance = reflection_probe0.SampleLevel(linear_clamp_sampler, corrected, level).rgb;
        else if (i == 1) radiance = reflection_probe1.SampleLevel(linear_clamp_sampler, corrected, level).rgb;
        else if (i == 2) radiance = reflection_probe2.SampleLevel(linear_clamp_sampler, corrected, level).rgb;
        else radiance = reflection_probe3.SampleLevel(linear_clamp_sampler, corrected, level).rgb;
        local += radiance * (weight * reflection_parameters[i].y);
        total += weight;
    }
    const float3 environment = specular_texture.SampleLevel(linear_clamp_sampler, direction,
        saturate(roughness) * max(environment_parameters.y - 1.0, 0.0)).rgb * environment_parameters.x;
    const float3 blended = local / max(total, 1.0) + environment * (1.0 - saturate(total));
    // On the authored plane, P and its mirror project to the same pixel.
    // Integer loads preserve that identity at an adjacent slice boundary.
    return planar_weight > 0.0 ? lerp(blended, planar_reflection.Load(int3((int2)pixel, 0)).rgb,
        planar_parameters.x * planar_weight) : blended;
}
