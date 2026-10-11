#ifndef MEDIASERVER_SCENE_MOTION_VECTOR_HLSLI
#define MEDIASERVER_SCENE_MOTION_VECTOR_HLSLI

// The velocity product, shared by every stage that writes it so the equation
// cannot drift between independently compiled passes: canvas-relative UV
// motion in xy, current and expected previous reversed depth in zw.
float4 motion_vector(float4 current_clip, float4 previous_clip, float motion_valid) {
    const float2 current = current_clip.xy
        / max(abs(current_clip.w), 1.0e-6);
    const float2 previous = previous_clip.xy
        / max(abs(previous_clip.w), 1.0e-6);
    // A canvas-relative UV delta: the clip pair is canvas-wide, so this is the
    // same number on every node rendering this wall pixel. Y is inverted
    // because render-target coordinates grow downward while clip-space Y grows
    // upward. taa_resolve.frag.hlsl scales it by the canvas extent.
    // Depth uses the same previous transform as motion, including rigid object
    // motion. Comparing current Z directly to old Z rejects a moving camera.
    return float4(motion_valid > 0.5 ? (current - previous) * float2(0.5, -0.5) : float2(0.0, 0.0),
        current_clip.z / max(abs(current_clip.w), 1.0e-6),
        previous_clip.w > 0.0
            ? previous_clip.z / previous_clip.w : -1.0);
}

#if defined(MODEL_MOTION)
// Geometry motion, including deforming triangles, from the motion vertex
// stage's varyings.
float4 scene_motion_vector(Varyings input) {
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
    return motion_vector(input.current_clip, input.previous_clip, input.motion_valid);
}
#endif
#endif
