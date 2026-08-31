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

struct InlineEffectOp {
    uint kind;
    uint parameter_offset;
    uint parameter_count;
    uint reserved;
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
StructuredBuffer<InlineEffectOp> inline_effects : register(t4, space2);
StructuredBuffer<float4> inline_effect_parameters : register(t5, space2);

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
    // xy: authored canvas slice origin. zw: exact show frame low/high uint32.
    uint4 grain_context;
    // xy: authored slice extent. zw: render-target extent.
    uint4 grain_extent;
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
    nointerpolation uint4 effect_range : TEXCOORD6;
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

float apply_levels_channel(float value, uint parameter_offset) {
    const float4 input_gamma_output_black =
        inline_effect_parameters[parameter_offset];
    const float output_white =
        inline_effect_parameters[parameter_offset + 1u].x;
    const float normalised = saturate(
        (value - input_gamma_output_black.x)
        / (input_gamma_output_black.y - input_gamma_output_black.x));
    return input_gamma_output_black.w
        + pow(normalised, 1.0 / input_gamma_output_black.z)
            * (output_white - input_gamma_output_black.w);
}

float3 apply_levels(float3 colour, uint parameter_offset) {
    // The canonical order is master first, then the independent R/G/B curves.
    colour.r = apply_levels_channel(colour.r, parameter_offset);
    colour.g = apply_levels_channel(colour.g, parameter_offset);
    colour.b = apply_levels_channel(colour.b, parameter_offset);
    colour.r = apply_levels_channel(colour.r, parameter_offset + 2u);
    colour.g = apply_levels_channel(colour.g, parameter_offset + 4u);
    colour.b = apply_levels_channel(colour.b, parameter_offset + 6u);
    return colour;
}

float3 apply_lift_gamma_gain(float3 colour, uint parameter_offset) {
    const float3 lift = inline_effect_parameters[parameter_offset].xyz;
    const float3 gamma = inline_effect_parameters[parameter_offset + 1u].xyz;
    const float3 gain = inline_effect_parameters[parameter_offset + 2u].xyz;
    return pow(max(colour + lift, 0.0), 1.0 / gamma) * gain;
}

float3 apply_contrast_pivot(float3 colour, uint parameter_offset) {
    const float2 parameters =
        inline_effect_parameters[parameter_offset].xy;
    return (colour - parameters.y) * parameters.x + parameters.y;
}

float3 apply_color_matrix(float3 colour, uint parameter_offset) {
    const float4 row0 = inline_effect_parameters[parameter_offset];
    const float4 row1 = inline_effect_parameters[parameter_offset + 1u];
    const float4 row2 = inline_effect_parameters[parameter_offset + 2u];
    return float3(
        dot(row0.xyz, colour) + row0.w,
        dot(row1.xyz, colour) + row1.w,
        dot(row2.xyz, colour) + row2.w);
}

float3 apply_white_balance(float3 colour, uint parameter_offset) {
    const float3 row0 = inline_effect_parameters[parameter_offset].xyz;
    const float3 row1 = inline_effect_parameters[parameter_offset + 1u].xyz;
    const float3 row2 = inline_effect_parameters[parameter_offset + 2u].xyz;
    return float3(
        dot(row0, colour), dot(row1, colour), dot(row2, colour));
}

float sample_curve_lut(float value, uint parameter_offset, uint lane) {
    const float position = saturate(value) * 1023.0;
    const uint lower = (uint)floor(position);
    const uint upper = min(lower + 1u, 1023u);
    const float fraction = position - (float)lower;
    return lerp(
        inline_effect_parameters[parameter_offset + lower][lane],
        inline_effect_parameters[parameter_offset + upper][lane], fraction);
}

float3 apply_curves(float3 colour, uint parameter_offset) {
    const float3 master = float3(
        sample_curve_lut(colour.r, parameter_offset, 0u),
        sample_curve_lut(colour.g, parameter_offset, 0u),
        sample_curve_lut(colour.b, parameter_offset, 0u));
    return float3(
        sample_curve_lut(master.r, parameter_offset, 1u),
        sample_curve_lut(master.g, parameter_offset, 2u),
        sample_curve_lut(master.b, parameter_offset, 3u));
}

// Integer-only avalanche/hash steps keep the random sequence independent of
// driver floating-point optimisations. The final 24-bit conversion is exactly
// representable and symmetric about zero over the complete hash range.
uint film_grain_mix(uint state, uint value) {
    state ^= value + 0x9e3779b9u + (state << 6u) + (state >> 2u);
    state ^= state >> 16u;
    state *= 0x7feb352du;
    state ^= state >> 15u;
    state *= 0x846ca68bu;
    state ^= state >> 16u;
    return state;
}

float film_grain_sample(uint seed, uint2 canvas_cell, uint channel) {
    uint hash = 0x811c9dc5u;
    hash = film_grain_mix(hash, seed);
    hash = film_grain_mix(hash, grain_context.z);
    hash = film_grain_mix(hash, grain_context.w);
    hash = film_grain_mix(hash, canvas_cell.x);
    hash = film_grain_mix(hash, canvas_cell.y);
    hash = film_grain_mix(hash, channel);
    return (float)(hash & 0x00ffffffu) * (2.0 / 16777215.0) - 1.0;
}

uint2 authored_canvas_pixel(float2 output_position) {
    const uint2 local_pixel = (uint2)floor(output_position);
    const float2 target_extent = max(float2(grain_extent.zw), 1.0);
    const float2 canvas_per_output =
        float2(grain_extent.xy) / target_extent;
    return grain_context.xy
        + (uint2)floor(float2(local_pixel) * canvas_per_output);
}

float3 apply_film_grain(
    float3 colour,
    uint parameter_offset,
    uint seed,
    float2 output_position) {
    const float4 parameters = inline_effect_parameters[parameter_offset];
    const uint2 canvas_pixel = authored_canvas_pixel(output_position);
    const uint2 canvas_cell =
        (uint2)floor(float2(canvas_pixel) / parameters.y);
    const float monochrome = film_grain_sample(seed, canvas_cell, 0u);
    float3 noise = monochrome.xxx;
    if (parameters.w > 0.5) {
        const float3 channels = float3(
            film_grain_sample(seed, canvas_cell, 1u),
            film_grain_sample(seed, canvas_cell, 2u),
            film_grain_sample(seed, canvas_cell, 3u));
        noise = lerp(noise, channels, parameters.z);
    }
    return colour + noise * parameters.x;
}

float3 apply_inline_effects(
    float3 colour, uint4 range, float2 output_position) {
    for (uint index = 0u; index < range.y; ++index) {
        const InlineEffectOp operation = inline_effects[range.x + index];
        if (operation.kind == 0u) { // EffectKind::Brightness
            colour *= inline_effect_parameters[operation.parameter_offset].x;
        } else if (operation.kind == 1u) { // EffectKind::Saturation
            const float luminance = dot(
                colour, float3(0.2126, 0.7152, 0.0722));
            colour = lerp(
                luminance.xxx,
                colour,
                inline_effect_parameters[operation.parameter_offset].x);
        } else if (operation.kind == 3u && operation.parameter_count == 8u) {
            colour = apply_levels(colour, operation.parameter_offset);
        } else if (operation.kind == 4u && operation.parameter_count == 3u) {
            colour = apply_lift_gamma_gain(
                colour, operation.parameter_offset);
        } else if (operation.kind == 5u && operation.parameter_count == 1u) {
            colour = apply_contrast_pivot(
                colour, operation.parameter_offset);
        } else if (operation.kind == 6u && operation.parameter_count == 3u) {
            colour = apply_color_matrix(
                colour, operation.parameter_offset);
        } else if (operation.kind == 7u && operation.parameter_count == 3u) {
            colour = apply_white_balance(
                colour, operation.parameter_offset);
        } else if (operation.kind == 8u && operation.parameter_count == 1024u) {
            colour = apply_curves(colour, operation.parameter_offset);
        } else if (operation.kind == 9u && operation.parameter_count == 1u) {
            colour = apply_film_grain(
                colour, operation.parameter_offset, operation.reserved,
                output_position);
        }
    }
    return colour;
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

    const float edge_distance = min(input.uv.x, 1.0 - input.uv.x);
    const float horizontal_coverage = input.layer_style.y > 0.0
        ? smoothstep(0.0, input.layer_style.y, edge_distance)
        : 1.0;
    const float opacity = saturate(
        input.layer_style.x * horizontal_coverage);
    // The program is already canonicalised on the CPU. The project's response
    // setting decides only where that authored order runs: scene-linear before
    // tone mapping for Exposure, or display-referred after encoding for Gain.
    const bool display_gain = counts.y != 0u;
    const float3 exposed = accumulated * ground_exposure.a;
    float3 display = display_gain
        ? encode_srgb(tonemap(max(exposed, 0.0)))
        : encode_srgb(tonemap(max(
              apply_inline_effects(
                  exposed, input.effect_range, input.position.xy), 0.0)));
    if (display_gain) {
        display = saturate(
            apply_inline_effects(
                display, input.effect_range, input.position.xy));
    }
    return float4(
        blend_source(display, opacity, input.layer_style.z), opacity);
}
