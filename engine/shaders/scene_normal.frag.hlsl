#include "material_coverage.hlsli"
cbuffer CoverageConstants : register(b0, space3) { float4 coverage_sampling; };

// World-space normals as an auxiliary product.
//
// Ambient occlusion needs normals and Advanced did not produce any. The
// alternative is reconstructing them from depth, which is cheaper and wrong in
// exactly the place occlusion matters most: at a depth discontinuity the
// derivative spans two surfaces and the reconstructed normal belongs to
// neither, so a crease - the thing AO exists to darken - gets a normal that
// points nowhere near either wall of it.
//
// Octahedral into RG16F. Screen-space reflections will want this target too.

#include "octahedral.hlsli"

#include "scene_varyings.hlsli"

float2 main(Varyings input, bool front_face : SV_IsFrontFace) : SV_Target0 {
    material_coverage(input.uv, input.anisotropy.w, input.alpha, coverage_sampling.x, front_face);
    // A depth-only instance participates in visibility but claims no product,
    // exactly as it claims no velocity and no identity.
    if (input.layer_style.w > 0.5) {
        return float2(0.0, 0.0);
    }
    return octahedral_encode(normalize(input.world_normal) * (front_face ? 1.0 : -1.0));
}
