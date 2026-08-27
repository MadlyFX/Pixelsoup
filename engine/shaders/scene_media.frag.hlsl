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

// The same block the lit stage receives, so both agree about the project's
// brightness curve. Only the curve selector is read here: a clip carries no
// surface response and is not lit.
cbuffer SceneConstants : register(b0, space3) {
    float4 eye_position;
    float4 sky_ambient;
    float4 ground_exposure;
    // y: 0 for a scene-referred exposure, 1 for a display-referred gain.
    uint4 counts;
};

// A clip's file is sRGB-encoded, so a scene-referred multiply has to happen in
// linear light. These are the same two transfer functions the lit stage uses.
float3 media_decode_srgb(float3 encoded) {
    const float3 low = encoded / 12.92;
    const float3 high = pow(max((encoded + 0.055) / 1.055, 0.0), 2.4);
    return lerp(low, high, step(0.04045, encoded));
}

float3 media_encode_srgb(float3 linear_colour) {
    const float3 low = linear_colour * 12.92;
    const float3 high =
        1.055 * pow(max(linear_colour, 1e-5), 1.0 / 2.4) - 0.055;
    return lerp(low, high, step(0.0031308, linear_colour));
}

// The full set of varyings the shared vertex stage emits, so this stage's input
// signature matches its output exactly. Only the texture coordinate is used.
struct Input {
    float4 position : SV_Position;
    float3 world_position : TEXCOORD0;
    float3 world_normal : TEXCOORD1;
    nointerpolation float4 base_color_roughness : TEXCOORD2;
    nointerpolation float4 emissive_metallic : TEXCOORD3;
    float2 uv : TEXCOORD4;
    nointerpolation float4 layer_style : TEXCOORD5;
};

float3 blend_source(float3 colour, float opacity, float mode) {
    if (mode > 1.5 && mode < 2.5) { // BlendMode::Multiply
        return lerp(float3(1.0, 1.0, 1.0), colour, opacity);
    }
    if (mode > 2.5) { // BlendMode::Screen
        return colour * opacity;
    }
    return colour;
}

// A clip's alpha decides whether it occludes. Half coverage is the cutoff a
// depth prepass conventionally uses; a soft edge cannot write a hard depth, so
// claiming one either way would be a guess.
static const float depth_only_alpha_cutoff = 0.5;

float4 main(Input input) : SV_Target0 {
    const float4 clip = source.Sample(source_sampler, input.uv);
    if (input.layer_style.w > 0.5) {
        // Drawn for depth alone, on behalf of another compositor layer. Texels
        // the clip does not cover must not occlude anything, so they leave the
        // depth buffer alone rather than writing a transparent silhouette.
        if (clip.a < depth_only_alpha_cutoff) {
            discard;
        }
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    const float opacity = saturate(input.layer_style.x * clip.a);
    const float brightness = max(input.layer_style.y, 0.0);
    // A display-referred gain scales the encoded picture directly. A
    // scene-referred exposure has to linearise first, or a clip and a lit
    // surface set to the same brightness would not match.
    const float3 colour = counts.y != 0u
        ? saturate(clip.rgb * brightness)
        : saturate(media_encode_srgb(
              media_decode_srgb(saturate(clip.rgb)) * brightness));
    return float4(
        blend_source(colour, opacity, input.layer_style.z), opacity);
}
