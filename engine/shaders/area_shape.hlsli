#ifndef MEDIASERVER_AREA_SHAPE_HLSLI
#define MEDIASERVER_AREA_SHAPE_HLSLI

#include "area_directions.hlsli"

float area_range_weight(Light light, float3 position) {
    const float fade = saturate(1.0 - length(light.vector - position) / max(light.params.x, 0.001));
    return fade * fade;
}

// Fixed 8x8 emission quadrature for fog and non-GGX material lobes. Weight
// is solid angle; the BRDF/phase belongs to the caller. No random/time seed.
float area_sample(Light light, float3 position, uint index, out float3 to_light) {
    float3 source, source_normal;
    float sample_area;
    if (light.kind == LIGHT_SPHERE) {
        source_normal = area_sphere_directions[index];
        source = light.vector + light.area_u.w * source_normal;
        sample_area = 12.566370614359172 * light.area_u.w * light.area_u.w / 64.0;
    } else {
        const float2 uv = (float2(index % 8u, index / 8u) + 0.5) / 8.0 * 2.0 - 1.0;
        source = light.vector + light.area_u.xyz * (light.area_u.w * uv.x)
            + light.area_v.xyz * (light.area_v.w * uv.y);
        source_normal = light.spot.xyz;
        sample_area = 4.0 * light.area_u.w * light.area_v.w / 64.0;
    }
    const float3 delta = source - position;
    const float distance_squared = dot(delta, delta);
    to_light = delta * rsqrt(max(distance_squared, 1e-10));
    const float facing = dot(source_normal, -to_light);
    const float emission_cosine = light.kind == LIGHT_RECTANGLE && light.spot.w > 0.5
        ? abs(facing) : max(facing, 0.0);
    return emission_cosine * sample_area / max(distance_squared, 1e-6);
}
#endif
