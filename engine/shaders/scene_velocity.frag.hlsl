#include "material_coverage.hlsli"
cbuffer CoverageConstants : register(b0, space3) { float4 coverage_sampling; };

#include "scene_varyings.hlsli"

float4 main(Varyings input, bool front_face : SV_IsFrontFace) : SV_Target0 {
    material_coverage(input.uv, input.anisotropy.w, input.alpha, coverage_sampling.x, front_face);
    // A depth-only instance participates in visibility but is not part of the
    // isolated layer's visible motion product. Its zero output still updates
    // auxiliary depth and prevents hidden geometry from claiming velocity.
    if (input.layer_style.w > 0.5) {
        return 0.0;
    }
    const float2 current = input.current_clip.xy
        / max(abs(input.current_clip.w), 1.0e-6);
    const float2 previous = input.previous_clip.xy
        / max(abs(input.previous_clip.w), 1.0e-6);
    // A canvas-relative UV delta: the clip pair is canvas-wide, so this is the
    // same number on every node rendering this wall pixel. Y is inverted
    // because render-target coordinates grow downward while clip-space Y grows
    // upward. taa_resolve.frag.hlsl scales it by the canvas extent.
    // Depth uses the same previous transform as motion, including rigid object
    // motion. Comparing current Z directly to old Z rejects a moving camera.
    return float4(input.motion_valid > 0.5 ? (current - previous) * float2(0.5, -0.5) : float2(0.0, 0.0),
        input.current_clip.z / max(abs(input.current_clip.w), 1.0e-6),
        input.previous_clip.w > 0.0
            ? input.previous_clip.z / input.previous_clip.w : -1.0);
}
