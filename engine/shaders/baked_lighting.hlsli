// Fixed world-space interpolation. There is no view or node slice in this
// expression. CPU reference: sample_probe_irradiance.
float2 probe_visibility_moments(uint offset, float3 d) {
    d /= max(abs(d.x) + abs(d.y) + abs(d.z), 1e-8);
    if (d.z < 0.0) d.xy = (1.0 - abs(d.yx)) * float2(d.x >= 0.0 ? 1.0 : -1.0, d.y >= 0.0 ? 1.0 : -1.0);
    const float2 texel = (d.xy * 0.5 + 0.5) * 8.0 - 0.5;
    const int2 base = (int2)floor(texel);
    const float2 fraction = texel - floor(texel);
    float2 result = 0.0;
    [unroll] for (uint y = 0u; y < 2u; ++y) {
        [unroll] for (uint x = 0u; x < 2u; ++x) {
            int2 cell = base + int2(x, y);
            if (cell.x < 0 || cell.x > 7) { cell.x = clamp(cell.x, 0, 7); cell.y = 7 - cell.y; }
            if (cell.y < 0 || cell.y > 7) { cell.y = clamp(cell.y, 0, 7); cell.x = 7 - cell.x; }
            const uint index = (uint)(cell.y * 8 + cell.x);
            const float4 packed = baked_probes[offset + 9u + index / 2u];
            const float2 moments = (index & 1u) == 0u ? packed.xy : packed.zw;
            result += moments * (x != 0u ? fraction.x : 1.0 - fraction.x)
                              * (y != 0u ? fraction.y : 1.0 - fraction.y);
        }
    }
    return result;
}
float3 baked_irradiance(float3 position, float3 normal) {
    const uint3 dimensions = (uint3)baked_dimensions.xyz;
    const float3 extent = baked_maximum.xyz - baked_minimum.xyz;
    const float3 cell = clamp((position - baked_minimum.xyz) / extent * (dimensions - 1u), 0.0, (float3)(dimensions - 1u));
    const uint3 base = min((uint3)cell, dimensions - 2u);
    const float3 fraction = cell - base;
    float basis[9];
    basis[0] = 0.282095 * 3.14159265;
    basis[1] = 0.488603 * normal.y * 2.09439510;
    basis[2] = 0.488603 * normal.z * 2.09439510;
    basis[3] = 0.488603 * normal.x * 2.09439510;
    basis[4] = 1.092548 * normal.x * normal.y * 0.78539816;
    basis[5] = 1.092548 * normal.y * normal.z * 0.78539816;
    basis[6] = 0.315392 * (3.0 * normal.z * normal.z - 1.0) * 0.78539816;
    basis[7] = 1.092548 * normal.x * normal.z * 0.78539816;
    basis[8] = 0.546274 * (normal.x * normal.x - normal.y * normal.y) * 0.78539816;
    float3 result = 0.0;
    float total = 0.0;
    [unroll] for (uint z = 0u; z < 2u; ++z) {
        [unroll] for (uint y = 0u; y < 2u; ++y) {
            [unroll] for (uint x = 0u; x < 2u; ++x) {
                const uint3 grid = base + uint3(x, y, z);
                const uint offset = ((grid.z * dimensions.y + grid.y) * dimensions.x + grid.x) * 41u;
                const float3 origin = baked_minimum.xyz + extent * (float3)grid / (float3)(dimensions - 1u);
                const float3 delta = position + normal * 0.01 - origin;
                const float distance = length(delta);
                const float2 moments = probe_visibility_moments(offset, delta);
                const float variance = max(moments.y - moments.x * moments.x, 0.00001);
                const float excess = max(distance - moments.x - 0.01, 0.0);
                const float probability = variance / (variance + excess * excess);
                const float visibility = max(0.0, (probability - 0.1) / 0.9);
                const float3 weights = float3(x != 0u ? fraction.x : 1.0 - fraction.x,
                    y != 0u ? fraction.y : 1.0 - fraction.y, z != 0u ? fraction.z : 1.0 - fraction.z);
                const float weight = weights.x * weights.y * weights.z * visibility;
                float3 irradiance = 0.0;
                [unroll] for (uint k = 0u; k < 9u; ++k) irradiance += baked_probes[offset + k].rgb * basis[k];
                result += max(irradiance, 0.0) * weight;
                total += weight;
            }
        }
    }
    return total > 0.00001 ? result / total * baked_dimensions.w : 0.0;
}
