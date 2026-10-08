#include "material_coverage.hlsli"
cbuffer CoverageConstants : register(b0, space3) { float4 coverage_sampling; };

// Reflective-shadow-map geometric normal. This pass uses the same pulled
// vertex stage, light view, batches, and reversed-depth test as the visibility
// map, so a texel's normal belongs to the surface whose depth reconstructs its
// virtual point-light position.

#include "scene_varyings.hlsli"

float4 main(Varyings input, bool front_face : SV_IsFrontFace) : SV_Target0 {
    material_coverage(input.uv, input.anisotropy.w, input.alpha, coverage_sampling.x, front_face);
    const float3 normal = normalize(input.world_normal) * (front_face ? 1.0 : -1.0);
    return float4(normal * 0.5 + 0.5, 1.0);
}
