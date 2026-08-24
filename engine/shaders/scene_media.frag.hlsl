// Media clip, pixel stage.
//
// A clip is a texture on a screen-facing quad in the 3D scene. This stage shares
// the pulled vertex stage (scene.vert.hlsl) with the lit geometry - it reads a
// subset of the same varyings, taking only the texture coordinate - and samples
// the clip's current frame there.
//
// The clip is shown unlit: an image sequence is already a display-referred
// picture (sRGB-encoded in its file), so it passes straight through rather than
// being lit and tonemapped like procedural surfaces. Lighting and colour-managed
// exposure are a later increment.
//
// Register spaces follow the SDL_GPU DXIL convention, the same as the other
// textured stages: the sampled texture and its sampler live at (t0, space2) and
// (s0, space2).

Texture2D<float4> source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

// The full set of varyings the shared vertex stage emits, so this stage's input
// signature matches its output exactly. Only the texture coordinate is used.
struct Input {
    float4 position : SV_Position;
    float3 world_position : TEXCOORD0;
    float3 world_normal : TEXCOORD1;
    nointerpolation float4 base_color_roughness : TEXCOORD2;
    nointerpolation float4 emissive_metallic : TEXCOORD3;
    float2 uv : TEXCOORD4;
};

float4 main(Input input) : SV_Target0 {
    const float4 clip = source.Sample(source_sampler, input.uv);
    return float4(clip.rgb, 1.0);
}
