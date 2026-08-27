// Scene geometry, pixel stage. A metallic-roughness surface response with a
// hemisphere ambient term, tone mapped and encoded for an 8-bit target.
//
// This is deliberately a small, correct core rather than a broad one: a
// Trowbridge-Reitz distribution, a Smith height-correlated visibility term, a
// Schlick Fresnel, and a Lambert diffuse. It is the same vocabulary the
// ported passes will speak, so the material a document author writes today
// keeps its meaning when the shading model grows underneath it.
//
// What is not here, and where it goes:
//
//  * Shadows, ambient occlusion, and any global illumination. Those arrive
//    with the ray-traced and probe passes rather than as additions to this.
//  * Normal maps and IBL. Normal maps require an authored tangent basis and
//    IBL requires filtered cubemaps; inventing either in this material slice
//    would make imported assets look plausible but wrong.
//  * glTF alpha modes. Layer opacity remains the one compositing authority
//    until MASK/BLEND become explicit scene material state.
//  * A managed colour pipeline. The tone map at the bottom is a display
//    transform, not colour management; OCIO replaces it, and the fact that it
//    is one clearly marked function is the point.
//
// Register spaces follow the SDL_GPU DXIL convention: a pixel stage's storage
// buffers live at (t[n], space2) and its uniform buffers at (b[n], space3).

static const float pi = 3.14159265358979323846;

struct Light {
    // xyz is a direction of travel when w is 0, a world position when w is 1.
    float4 vector_kind;
    float4 color_intensity;
    // x: range, beyond which a positional light contributes nothing.
    float4 params;
};

// SDL_GPU requires sampled textures first in the fragment stage's shared
// t-register range, followed by storage buffers. All three maps are bound for
// every lit batch; a missing map is a factor-preserving white texture.
Texture2D<float4> base_color_texture : register(t0, space2);
SamplerState base_color_sampler : register(s0, space2);
Texture2D<float4> metallic_roughness_texture : register(t1, space2);
SamplerState metallic_roughness_sampler : register(s1, space2);
Texture2D<float4> emissive_texture : register(t2, space2);
SamplerState emissive_sampler : register(s2, space2);
StructuredBuffer<Light> lights : register(t3, space2);

cbuffer SceneConstants : register(b0, space3) {
    float4 eye_position;
    // rgb: the colour of the sky half of the ambient hemisphere.
    // a: overall ambient strength.
    float4 sky_ambient;
    // rgb: the ground half. a: exposure, applied before the tone map.
    float4 ground_exposure;
    // x: number of lights. y: the project's brightness curve, 0 for a
    // scene-referred exposure and 1 for a display-referred gain.
    uint4 counts;
};

struct Varyings {
    float4 position : SV_Position;
    float3 world_position : TEXCOORD0;
    float3 world_normal : TEXCOORD1;
    nointerpolation float4 base_color_roughness : TEXCOORD2;
    nointerpolation float4 emissive_metallic : TEXCOORD3;
    // Unused by lit shading, but declared so this stage's input signature
    // matches the vertex stage's output exactly - the pipeline links the two by
    // full signature, not by the subset a stage happens to read.
    float2 uv : TEXCOORD4;
    nointerpolation float4 layer_style : TEXCOORD5;
};

// Multiply and Screen need different fixed-function factors from ordinary
// straight-alpha source-over. Prepare exactly the source colour those factors
// consume; Normal and Add keep straight colour and use alpha as their weight.
float3 blend_source(float3 colour, float opacity, float mode) {
    if (mode > 1.5 && mode < 2.5) { // BlendMode::Multiply
        return lerp(float3(1.0, 1.0, 1.0), colour, opacity);
    }
    if (mode > 2.5) { // BlendMode::Screen
        return colour * opacity;
    }
    return colour;
}

float distribution_ggx(float n_dot_h, float alpha) {
    const float alpha2 = alpha * alpha;
    const float denominator = n_dot_h * n_dot_h * (alpha2 - 1.0) + 1.0;
    return alpha2 / max(pi * denominator * denominator, 1e-7);
}

float visibility_smith(float n_dot_v, float n_dot_l, float alpha) {
    // Height-correlated Smith, already divided by the 4*NdotV*NdotL the
    // specular term would otherwise carry.
    const float alpha2 = alpha * alpha;
    const float v = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - alpha2) + alpha2);
    const float l = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - alpha2) + alpha2);
    return 0.5 / max(v + l, 1e-7);
}

float3 fresnel_schlick(float3 f0, float v_dot_h) {
    const float factor = pow(saturate(1.0 - v_dot_h), 5.0);
    return f0 + (1.0 - f0) * factor;
}

float3 surface_response(
    float3 normal,
    float3 view,
    float3 light,
    float3 albedo,
    float roughness,
    float metallic) {
    const float3 halfway = normalize(view + light);
    const float n_dot_l = saturate(dot(normal, light));
    const float n_dot_v = saturate(dot(normal, view)) + 1e-5;
    const float n_dot_h = saturate(dot(normal, halfway));
    const float v_dot_h = saturate(dot(view, halfway));

    // Roughness is authored perceptually and squared into the distribution's
    // alpha, which is the convention every DCC package the content comes from
    // already uses.
    const float alpha = max(roughness * roughness, 1e-3);

    const float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    const float3 fresnel = fresnel_schlick(f0, v_dot_h);
    const float3 specular =
        fresnel * distribution_ggx(n_dot_h, alpha)
        * visibility_smith(n_dot_v, n_dot_l, alpha);
    // Energy that was not reflected specularly is available to scatter, and a
    // metal has none of it.
    const float3 diffuse = (1.0 - fresnel) * (1.0 - metallic) * albedo / pi;

    return (diffuse + specular) * n_dot_l;
}

// Narkowicz's ACES fit. A display transform standing in for the managed
// pipeline, not a substitute for it.
float3 tonemap(float3 colour) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((colour * (a * colour + b)) / (colour * (c * colour + d) + e));
}

float3 encode_srgb(float3 linear_colour) {
    // The target is an unorm texture rather than an sRGB one, so the transfer
    // curve is applied here. The piecewise standard, not a 2.2 power: the two
    // differ visibly in the shadows, which is where an operator grading a
    // stage spends their attention.
    const float3 low = linear_colour * 12.92;
    const float3 high = 1.055 * pow(max(linear_colour, 1e-5), 1.0 / 2.4) - 0.055;
    return lerp(low, high, step(0.0031308, linear_colour));
}

float3 decode_srgb(float3 encoded_colour) {
    const float3 low = encoded_colour / 12.92;
    const float3 high = pow(
        max((encoded_colour + 0.055) / 1.055, 0.0), 2.4);
    return lerp(low, high, step(0.04045, encoded_colour));
}

float4 main(Varyings input) : SV_Target0 {
    // A depth-only instance belongs to a different compositor layer and is
    // drawn here only so that this layer is occluded by it. Depth is still
    // written - there is no discard - but nothing reaches the colour target,
    // and a fully transparent black is also inert under every blend equation
    // the layer composite can apply afterwards.
    if (input.layer_style.w > 0.5) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float3 normal = normalize(input.world_normal);
    const float3 view = normalize(eye_position.xyz - input.world_position);
    const float4 sampled_base =
        base_color_texture.Sample(base_color_sampler, input.uv);
    const float4 sampled_metallic_roughness = metallic_roughness_texture.Sample(
        metallic_roughness_sampler, input.uv);
    const float3 sampled_emissive = decode_srgb(
        emissive_texture.Sample(emissive_sampler, input.uv).rgb);
    const float3 albedo = input.base_color_roughness.rgb *
        decode_srgb(sampled_base.rgb);
    // glTF packs roughness in G and metallic in B. These are data channels,
    // sampled linearly rather than passed through the colour transfer curve.
    const float roughness = saturate(
        input.base_color_roughness.a * sampled_metallic_roughness.g);
    const float metallic = saturate(
        input.emissive_metallic.a * sampled_metallic_roughness.b);
    const float3 emissive =
        input.emissive_metallic.rgb * sampled_emissive;

    float3 accumulated = float3(0.0, 0.0, 0.0);
    for (uint index = 0; index < counts.x; ++index) {
        const Light light = lights[index];
        float3 to_light;
        float attenuation = 1.0;

        if (light.vector_kind.w > 0.5) {
            const float3 offset = light.vector_kind.xyz - input.world_position;
            const float distance = length(offset);
            const float range = max(light.params.x, 1e-3);
            if (distance >= range) {
                continue;
            }
            to_light = offset / max(distance, 1e-5);
            // Inverse square, windowed so the light actually reaches zero at
            // its stated range. A light that never quite ends cannot be culled
            // and cannot be reasoned about by whoever placed it.
            const float falloff = saturate(1.0 - (distance / range));
            attenuation = falloff * falloff / max(distance * distance, 1e-4);
        } else {
            to_light = -normalize(light.vector_kind.xyz);
        }

        const float3 radiance =
            light.color_intensity.rgb * light.color_intensity.a * attenuation;
        accumulated += surface_response(
            normal, view, to_light, albedo, roughness, metallic) * radiance;
    }

    // A two-colour hemisphere standing in for image-based lighting: enough
    // that an unlit face reads as being in shadow rather than as a hole in the
    // frame, and cheap enough not to matter.
    const float hemisphere = normal.y * 0.5 + 0.5;
    const float3 ambient =
        lerp(ground_exposure.rgb, sky_ambient.rgb, hemisphere) * sky_ambient.a;
    accumulated += ambient * albedo;
    accumulated += emissive;

    const float opacity = saturate(input.layer_style.x);
    // The multiplier is already shaped by the project's brightness gamma on
    // the CPU, where it is part of the replicated scene. What is left to decide
    // here is *where* it applies: before the tone map, which is what changing
    // exposure physically does and lets a highlight roll off, or after it,
    // which is the hard gain a vision mixer applies and which clips.
    const float brightness = max(input.layer_style.y, 0.0);
    const bool display_gain = counts.y != 0u;
    const float3 exposed =
        accumulated * ground_exposure.a * (display_gain ? 1.0 : brightness);
    float3 display = encode_srgb(tonemap(max(exposed, 0.0)));
    if (display_gain) {
        display = saturate(display * brightness);
    }
    return float4(
        blend_source(display, opacity, input.layer_style.z), opacity);
}
