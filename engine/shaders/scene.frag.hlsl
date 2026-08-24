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

StructuredBuffer<Light> lights : register(t0, space2);

cbuffer SceneConstants : register(b0, space3) {
    float4 eye_position;
    // rgb: the colour of the sky half of the ambient hemisphere.
    // a: overall ambient strength.
    float4 sky_ambient;
    // rgb: the ground half. a: exposure, applied before the tone map.
    float4 ground_exposure;
    uint4 counts; // x: number of lights
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
};

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

float4 main(Varyings input) : SV_Target0 {
    const float3 normal = normalize(input.world_normal);
    const float3 view = normalize(eye_position.xyz - input.world_position);
    const float3 albedo = input.base_color_roughness.rgb;
    const float roughness = saturate(input.base_color_roughness.a);
    const float metallic = saturate(input.emissive_metallic.a);

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
    accumulated += input.emissive_metallic.rgb;

    const float3 exposed = accumulated * ground_exposure.a;
    return float4(encode_srgb(tonemap(exposed)), 1.0);
}
