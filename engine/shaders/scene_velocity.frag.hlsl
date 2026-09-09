#include "material_coverage.hlsli"
cbuffer CoverageConstants : register(b0, space3) { float4 coverage_sampling; };

#define MODEL_MOTION 1
#include "scene_varyings.hlsli"

float4 main(Varyings input, bool front_face : SV_IsFrontFace) : SV_Target0 {
    material_coverage(input.uv, input.anisotropy.w, input.alpha, coverage_sampling.x, front_face);
    // A depth-only instance participates in visibility but is not part of the
    // isolated layer's visible motion product. Its zero output still updates
    // auxiliary depth and prevents hidden geometry from claiming velocity.
    if (input.layer_style.w > 0.5) {
        return 0.0;
    }
    if (input.motion_viewport.z>0.0) {
        // Hardware interpolator rounding can depend on viewport origin. A
        // deforming triangle exposes it when nearby endpoints are subtracted.
        // Solve homogeneous barycentrics at the common canvas pixel instead.
        // Flat original clip coordinates also work for near-plane clipping.
        const float2 pixel=(input.position.xy-input.motion_viewport.xy)*input.motion_viewport.zw;
        const float2 sample_ndc=pixel*float2(2,-2)+float2(-1,1);
        const float3 x=float3(input.motion_raster0.x-sample_ndc.x*input.motion_raster0.w,
            input.motion_raster1.x-sample_ndc.x*input.motion_raster1.w,input.motion_raster2.x-sample_ndc.x*input.motion_raster2.w);
        const float3 y=float3(input.motion_raster0.y-sample_ndc.y*input.motion_raster0.w,
            input.motion_raster1.y-sample_ndc.y*input.motion_raster1.w,input.motion_raster2.y-sample_ndc.y*input.motion_raster2.w);
        float3 weight=cross(x,y);
        weight/=weight.x+weight.y+weight.z;
        input.current_clip=input.motion_current0*weight.x+input.motion_current1*weight.y+input.motion_current2*weight.z;
        input.previous_clip=input.motion_previous0*weight.x+input.motion_previous1*weight.y+input.motion_previous2*weight.z;
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
