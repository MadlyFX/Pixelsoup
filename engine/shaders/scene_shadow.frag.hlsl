// Reversed depth, with exactly the cutout policy used by the camera products.
#include "material_coverage.hlsli"
cbuffer CoverageConstants : register(b0, space3) { float4 coverage_sampling; };
#include "scene_varyings.hlsli"
float main(Varyings input, bool front_face : SV_IsFrontFace) : SV_Target0 {
    material_coverage(input.uv, input.anisotropy.w, input.alpha, coverage_sampling.x, front_face);
    return input.position.z;
}
