// The split-sum BRDF integration table.
//
// View-independent and environment-independent, so it is generated once when
// the renderer is built rather than per environment or per frame. x is
// N-dot-V, y is roughness; the output is the scale and bias applied to F0.
#include "ibl_common.hlsli"

cbuffer LutParameters : register(b0, space1) {
    uint2 lut_extent;
    uint sample_count;
    uint lut_padding;
};

RWTexture2D<float4> lut_target : register(u0);

// Smith's geometry term with the IBL k, which differs from the direct-light k.
float geometry_smith_ibl(float n_dot_v, float n_dot_l, float roughness) {
    const float k = (roughness * roughness) * 0.5F;
    const float gv = n_dot_v / (n_dot_v * (1.0F - k) + k);
    const float gl = n_dot_l / (n_dot_l * (1.0F - k) + k);
    return gv * gl;
}

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    if (thread.x >= lut_extent.x || thread.y >= lut_extent.y) {
        return;
    }
    // Texel centres, so the table is symmetric and never samples exactly zero
    // roughness at an edge where the GGX distribution degenerates.
    const float n_dot_v = max(
        (float(thread.x) + 0.5F) / float(lut_extent.x), 1.0e-4F);
    const float roughness = (float(thread.y) + 0.5F) / float(lut_extent.y);

    const float3 view = float3(
        sqrt(1.0F - n_dot_v * n_dot_v), 0.0F, n_dot_v);
    const float3 normal = float3(0.0F, 0.0F, 1.0F);

    float scale = 0.0F;
    float bias = 0.0F;
    // A fixed count in a fixed order. Two nodes integrate the same table.
    for (uint index = 0U; index < sample_count; ++index) {
        const float2 sample_point = ibl_hammersley(index, sample_count);
        const float3 half_vector =
            ibl_importance_sample_ggx(sample_point, normal, roughness);
        const float3 light =
            normalize(2.0F * dot(view, half_vector) * half_vector - view);
        const float n_dot_l = max(light.z, 0.0F);
        if (n_dot_l <= 0.0F) {
            continue;
        }
        const float n_dot_h = max(half_vector.z, 0.0F);
        const float v_dot_h = max(dot(view, half_vector), 0.0F);
        const float geometry =
            geometry_smith_ibl(n_dot_v, n_dot_l, roughness);
        const float visibility =
            geometry * v_dot_h / max(n_dot_h * n_dot_v, 1.0e-6F);
        const float fresnel = pow(1.0F - v_dot_h, 5.0F);
        scale += (1.0F - fresnel) * visibility;
        bias += fresnel * visibility;
    }
    const float inverse = 1.0F / float(sample_count);
    lut_target[thread.xy] =
        float4(scale * inverse, bias * inverse, 0.0F, 1.0F);
}
