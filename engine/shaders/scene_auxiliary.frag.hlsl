// Object identity, motion and world normals in one pass, for a device that
// writes several targets at once. Each product is exactly what its own
// single-target stage writes - scene_object_id, scene_velocity and
// scene_normal - under the same coverage, so the geometry is rasterized once
// instead of three times.
#include "material_coverage.hlsli"
cbuffer CoverageConstants : register(b0, space3) { float4 coverage_sampling; };

#define MODEL_MOTION 1
#include "scene_varyings.hlsli"
#include "scene_motion_vector.hlsli"
#include "octahedral.hlsli"

struct AuxiliaryProducts {
    uint object_id : SV_Target0;
    float4 velocity : SV_Target1;
    float2 normal : SV_Target2;
};

AuxiliaryProducts main(Varyings input, bool front_face : SV_IsFrontFace) {
    material_coverage(input.uv, input.anisotropy.w, input.alpha, coverage_sampling.x, front_face);
    AuxiliaryProducts products;
    // A depth-only instance participates in visibility but claims no
    // identity, motion or normal.
    if (input.layer_style.w > 0.5) {
        products.object_id = 0u;
        products.velocity = 0.0;
        products.normal = float2(0.0, 0.0);
        return products;
    }
    products.object_id = input.object_id;
    products.velocity = scene_motion_vector(input);
    products.normal = octahedral_encode(normalize(input.world_normal) * (front_face ? 1.0 : -1.0));
    return products;
}
