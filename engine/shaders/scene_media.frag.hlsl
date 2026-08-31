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
// Register spaces follow the SDL_GPU DXIL convention. Slot zero is either an
// RGBA clip or P010 luma; slot one is P010 chroma and ignored for RGBA clips.

Texture2D<float4> primary_plane : register(t0, space2);
SamplerState primary_sampler : register(s0, space2);
Texture2D<float2> chroma_plane : register(t1, space2);
SamplerState chroma_sampler : register(s1, space2);

struct InlineEffectOp {
    uint kind;
    uint parameter_offset;
    uint parameter_count;
    uint reserved;
};

StructuredBuffer<InlineEffectOp> inline_effects : register(t2, space2);
StructuredBuffer<float4> inline_effect_parameters : register(t3, space2);

// The same block the lit stage receives, so both agree about the project's
// brightness curve. Only the curve selector is read here: a clip carries no
// surface response and is not lit.
cbuffer SceneConstants : register(b0, space3) {
    float4 eye_position;
    float4 sky_ambient;
    float4 ground_exposure;
    // y: 0 for a scene-referred exposure, 1 for a display-referred gain.
    uint4 counts;
    uint4 grain_context;
    uint4 grain_extent;
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
    nointerpolation uint4 effect_range : TEXCOORD6;
};

float4 sample_clip(Input input) {
    // DecodedPixelLayout::Rgba8.
    if (input.effect_range.z == 0u) {
        return primary_plane.Sample(primary_sampler, input.uv);
    }

    // P010 stores ten significant bits in the high bits of each R16/RG16
    // texel. UNORM sampling reconstructs approximately code / 1023. The scene
    // consumes encoded RGB, matching the old intermediate conversion texture.
    const float y_code = primary_plane.Sample(primary_sampler, input.uv).r;
    const float2 uv_code = chroma_plane.Sample(chroma_sampler, input.uv);
    const bool limited = input.effect_range.z == 1u;
    const float y_offset = limited ? 64.0 / 1023.0 : 0.0;
    const float y_scale = limited ? 1023.0 / 876.0 : 1.0;
    const float chroma_scale = limited ? 1023.0 / 896.0 : 1.0;
    const float y = (y_code - y_offset) * y_scale;
    const float2 chroma =
        (uv_code - 512.0 / 1023.0) * chroma_scale;
    const float3 encoded_rgb = float3(
        y + 1.5748 * chroma.y,
        y - 0.187324 * chroma.x - 0.468124 * chroma.y,
        y + 1.8556 * chroma.x);
    return float4(saturate(encoded_rgb), 1.0);
}

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
        if (operation.kind == 0u) {
            colour *= inline_effect_parameters[operation.parameter_offset].x;
        } else if (operation.kind == 1u) {
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

float4 main(Input input) : SV_Target0 {
    const float4 clip = sample_clip(input);
    if (input.layer_style.w > 0.5) {
        // Drawn for depth alone, on behalf of another compositor layer. Texels
        // the clip does not cover must not occlude anything, so they leave the
        // depth buffer alone rather than writing a transparent silhouette.
        if (clip.a < depth_only_alpha_cutoff) {
            discard;
        }
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    const float edge_distance = min(input.uv.x, 1.0 - input.uv.x);
    const float horizontal_coverage = input.layer_style.y > 0.0
        ? smoothstep(0.0, input.layer_style.y, edge_distance)
        : 1.0;
    const float opacity = saturate(
        input.layer_style.x * clip.a * horizontal_coverage);
    // A display-referred gain scales the encoded picture directly. A
    // scene-referred exposure has to linearise first, or a clip and a lit
    // surface set to the same brightness would not match.
    const bool display_gain = counts.y != 0u;
    const float3 response_source = display_gain
        ? saturate(clip.rgb)
        : media_decode_srgb(saturate(clip.rgb));
    const float3 effected =
        apply_inline_effects(
            response_source, input.effect_range, input.position.xy);
    const float3 colour = display_gain
        ? saturate(effected)
        : saturate(media_encode_srgb(max(effected, 0.0)));
    return float4(
        blend_source(colour, opacity, input.layer_style.z), opacity);
}
