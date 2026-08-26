// Projective physical-mesh warp, fragment stage.

Texture2D<float4> scene_render : register(t0, space2);
SamplerState scene_sampler : register(s0, space2);

struct Input {
    float4 position : SV_Position;
    float4 source_clip : TEXCOORD0;
};

float4 main(Input input) : SV_Target0 {
    if (!(input.source_clip.w > 0.0)) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    const float2 ndc = input.source_clip.xy / input.source_clip.w;
    const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    // A fitted frustum puts valid mesh fragments inside this range. Saturate
    // only absorbs sub-ulp raster interpolation at the edge; it does not turn
    // an invalid behind-eye patch into plausible pixels.
    return scene_render.Sample(scene_sampler, saturate(uv));
}
