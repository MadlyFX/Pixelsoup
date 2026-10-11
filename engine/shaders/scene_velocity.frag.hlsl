#include "material_coverage.hlsli"
cbuffer CoverageConstants : register(b0, space3) { float4 coverage_sampling; };

#define MODEL_MOTION 1
#include "scene_varyings.hlsli"
#include "scene_motion_vector.hlsli"

float4 main(Varyings input, bool front_face : SV_IsFrontFace) : SV_Target0 {
    material_coverage(input.uv, input.anisotropy.w, input.alpha, coverage_sampling.x, front_face);
    // A depth-only instance participates in visibility but is not part of the
    // isolated layer's visible motion product. Its zero output still updates
    // auxiliary depth and prevents hidden geometry from claiming velocity.
    if (input.layer_style.w > 0.5) {
        return 0.0;
    }
    return scene_motion_vector(input);
}
