// LTC GGX fit: Heitz, Dupuy, Hill and Neubelt, SIGGRAPH 2016.
// https://eheitzresearch.wordpress.com/415-2/ ; third_party/ltc/LICENSE.
// Polygon integration here explicitly clips each edge to the cosine horizon.
#include "area_shape.hlsli"

float4 ltc_lookup(float roughness, float nv, uint table) {
    const float2 location = float2(saturate(roughness), sqrt(1.0 - saturate(nv))) * 63.0;
    const uint2 lower = (uint2)floor(location), upper = min(lower + 1u, 63u);
    const float2 fraction = location - float2(lower);
    const uint base = table * 4096u;
    return lerp(lerp(ltc_tables[base + lower.y * 64u + lower.x],
                     ltc_tables[base + lower.y * 64u + upper.x], fraction.x),
                lerp(ltc_tables[base + upper.y * 64u + lower.x],
                     ltc_tables[base + upper.y * 64u + upper.x], fraction.x), fraction.y);
}

float ltc_edge(float3 a, float3 b) {
    const float cosine = clamp(dot(a, b), -1.0, 1.0);
    const float3 c = cross(a, b);
    const float sine = length(c);
    // atan2 stays finite near coincident vertices. Edges lie on the clipped
    // hemisphere, so antipodal ambiguity is limited to the horizon itself.
    return c.z * (atan2(sine, cosine) / max(sine, 1e-7)) * 0.1591549430918953;
}
void ltc_emit(float3 value, inout float3 first, inout float3 last, inout uint count, inout float sum) {
    const float squared = dot(value, value);
    if (squared <= 1e-20) return;
    const float3 direction = value * rsqrt(squared);
    if (count == 0u) first = direction;
    else sum += ltc_edge(last, direction);
    last = direction;
    ++count;
}
float3 ltc_transform(float3 direction, float3 t, float3 b, float3 n, float4 matrix) {
    const float3 local = float3(dot(t, direction), dot(b, direction), dot(n, direction));
    return float3(matrix.x * local.x + matrix.z * local.z, local.y,
                  matrix.y * local.x + matrix.w * local.z);
}

// Rectangles use four exact vertices. A sphere's visible tangent circle uses
// a fixed 32-sided polygon at every pixel/roughness/distance on every node.
float ltc_integral(Light light, float3 position, float3 n, float3 view, float4 matrix) {
    const float3 offset = light.vector - position;
    if (light.kind == LIGHT_RECTANGLE && light.spot.w < 0.5 && dot(-offset, light.spot.xyz) <= 0.0) return 0.0;
    float3 centre = offset;
    float3 u = light.area_u.xyz * light.area_u.w, v = light.area_v.xyz * light.area_v.w;
    uint vertices = 4u;
    if (light.kind == LIGHT_SPHERE) {
        const float d2 = dot(offset, offset), r2 = light.area_u.w * light.area_u.w;
        if (d2 <= r2) return 0.0; // outward emission; an interior receiver sees no emitting face
        const float3 axis = offset * rsqrt(d2);
        const float3 reference = abs(axis.x) < 0.9 ? float3(1,0,0) : float3(0,0,1);
        const float radius = light.area_u.w * sqrt(1.0 - r2 / d2);
        u = normalize(reference - axis * dot(reference, axis)) * radius;
        v = cross(axis, u);
        centre = offset * (1.0 - r2 / d2);
        vertices = 32u;
    }
    const float3 projected = view - n * dot(n, view);
    const float3 reference = abs(n.x) < 0.9 ? float3(1,0,0) : float3(0,0,1);
    const float3 t = dot(projected, projected) > 1e-8 ? normalize(projected)
        : normalize(reference - n * dot(reference, n));
    const float3 b = cross(n, t);
    float3 first = 0.0, last = 0.0;
    uint count = 0u;
    float sum = 0.0;
    [loop] for (uint i = 0u; i < vertices; ++i) {
        const uint next = (i + 1u) % vertices;
        const float2 ca = vertices == 4u ? area_quad_corners[i] : area_circle_corners[i];
        const float2 cb = vertices == 4u ? area_quad_corners[next] : area_circle_corners[next];
        const float3 a = ltc_transform(centre + ca.x * u + ca.y * v, t, b, n, matrix);
        const float3 z = ltc_transform(centre + cb.x * u + cb.y * v, t, b, n, matrix);
        const bool ain = a.z > 0.0, zin = z.z > 0.0;
        if (ain != zin) {
            const float3 intersection = lerp(a, z, a.z / (a.z - z.z));
            ltc_emit(float3(intersection.xy, 0.0), first, last, count, sum);
        }
        if (zin) ltc_emit(z, first, last, count, sum);
    }
    if (count < 3u) return 0.0;
    return saturate(abs(sum + ltc_edge(last, first)));
}

float3 area_surface_response(Light light, float3 position, float3 n, float3 view,
                             float3 albedo, float roughness, float metallic, MaterialLobes m) {
    const float fade = area_range_weight(light, position);
    if (fade <= 0.0) return 0.0;
    // The published fit is isotropic GGX. Anisotropic GGX/Charlie retain their
    // own authored BRDF through fixed emission quadrature, never the GGX fit.
    if (m.anisotropy > 0.0 || max_component(m.sheen) > 0.0) {
        float3 result = 0.0;
        [loop] for (uint i = 0u; i < 64u; ++i) {
            float3 l;
            const float solid_angle = area_sample(light, position, i, l);
            if (solid_angle > 0.0) result += extended_surface_response(n, view, l, albedo, roughness, metallic, m, false) * solid_angle;
        }
        return result * fade;
    }
    const float nv = saturate(dot(n, view));
    const float4 matrix = ltc_lookup(roughness, nv, 0u), amplitude = ltc_lookup(roughness, nv, 1u);
    const float3 f0 = lerp(m.dielectric_f0, albedo, metallic);
    const float weight = lerp(m.specular_weight, 1.0, metallic);
    const float3 fresnel = material_fresnel(f0, weight, nv);
    const float specular = ltc_integral(light, position, n, view, matrix);
    const float diffuse = ltc_integral(light, position, n, view, float4(1,0,0,1));
    return (specular * (f0 * amplitude.x + (weight - f0) * amplitude.y)
        + diffuse * albedo * (1.0 - fresnel) * (1.0 - metallic) * (1.0 - m.transmission)) * fade;
}
float3 area_coat_response(Light light, float3 position, float3 view, MaterialLobes m) {
    const float nv = saturate(dot(m.coat_normal, view));
    const float4 matrix = ltc_lookup(m.coat_roughness, nv, 0u);
    const float amplitude = ltc_lookup(m.coat_roughness, nv, 1u).x;
    // Coat Fresnel and strength are applied once by the existing layer blend.
    return (ltc_integral(light, position, m.coat_normal, view, matrix) * amplitude * area_range_weight(light, position)).xxx;
}
