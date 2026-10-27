// Scene geometry, pixel stage. The metallic-roughness core and glTF material
// layers share direct lighting. Advanced adds scene-linear HDR, environment
// lighting, shadowing and occlusion; Basic encodes its display target here.
// Thin transmission samples a bounded opaque-background copy. Volume transport
// and a material graph remain outside this shader's contract.
//  * A managed colour pipeline. The tone map at the bottom is a display
//    transform, not colour management; OCIO replaces it, and the fact that it
//    is one clearly marked function is the point.
//
// Register spaces follow the SDL_GPU DXIL convention: a pixel stage's storage
// buffers live at (t[n], space2) and its uniform buffers at (b[n], space3).

static const float pi = 3.14159265358979323846;

#include "light.hlsli"
#include "ies.hlsli"

// One shadow-casting light's map. Only lights that got a tile appear here, so
// a light's `params.w` is an index into this table rather than a flag.
struct LightShadow {
    column_major float4x4 view_projection;
    // xy: tile origin in atlas texels. z: tile extent. w: one over the extent,
    // which is both the texel size for filtering and the depth a texel of
    // slope costs for the bias.
    float4 tile;
};

struct InlineEffectOp {
    uint kind;
    uint parameter_offset;
    uint parameter_count;
    uint reserved;
};

// Each material texture has its own authored sampler. Advanced shares two
// clamp samplers for every non-material texture, staying within sixteen slots.
Texture2D<float4> base_color_texture : register(t0, space2);
SamplerState base_color_sampler : register(s0, space2);
Texture2D<float4> metallic_roughness_texture : register(t1, space2);
SamplerState metallic_roughness_sampler : register(s1, space2);
Texture2D<float4> normal_texture : register(t2, space2);
SamplerState normal_sampler : register(s2, space2);
Texture2D<float4> occlusion_texture : register(t3, space2);
SamplerState occlusion_sampler : register(s3, space2);
Texture2D<float4> emissive_texture : register(t4, space2);
SamplerState emissive_sampler : register(s4, space2);
Texture2D<float4> clearcoat_texture : register(t5, space2);
SamplerState clearcoat_sampler : register(s5, space2);
Texture2D<float4> clearcoat_roughness_texture : register(t6, space2);
SamplerState clearcoat_roughness_sampler : register(s6, space2);
Texture2D<float4> clearcoat_normal_texture : register(t7, space2);
SamplerState clearcoat_normal_sampler : register(s7, space2);
Texture2D<float4> sheen_color_texture : register(t8, space2);
SamplerState sheen_color_sampler : register(s8, space2);
Texture2D<float4> sheen_roughness_texture : register(t9, space2);
SamplerState sheen_roughness_sampler : register(s9, space2);
Texture2D<float4> transmission_texture : register(t10, space2);
SamplerState transmission_sampler : register(s10, space2);
Texture2D<float4> material_specular_texture : register(t11, space2);
SamplerState material_specular_sampler : register(s11, space2);
Texture2D<float4> specular_color_texture : register(t12, space2);
SamplerState specular_color_sampler : register(s12, space2);
Texture2D<float4> anisotropy_texture : register(t13, space2);
SamplerState anisotropy_sampler : register(s13, space2);
#if defined(MEDIASERVER_SCENE_LINEAR)
TextureCube<float4> specular_texture : register(t14, space2);
SamplerState linear_clamp_sampler : register(s14, space2);
SamplerState nearest_clamp_sampler : register(s15, space2);
Texture2D<float> shadow_cascade0_texture : register(t15, space2);
Texture2D<float> shadow_cascade1_texture : register(t16, space2);
Texture2D<float> shadow_cascade2_texture : register(t17, space2);
Texture2D<float> shadow_cascade3_texture : register(t18, space2);
Texture2D<float4> rsm_normal_texture : register(t19, space2);
Texture2D<float4> rsm_flux_texture : register(t20, space2);
Texture2D<float4> irradiance_texture : register(t21, space2);
Texture2D<float4> brdf_texture : register(t22, space2);
Texture2D<float4> screen_occlusion_texture : register(t23, space2);
Texture2D<float> shadow_atlas_texture : register(t24, space2);
Texture2D<float4> transmission_background : register(t25, space2);
Texture2D<float> contact_depth_texture : register(t26, space2);
Texture2D<float> ies_atlas : register(t27, space2);
TextureCube<float4> reflection_probe0 : register(t28, space2);
TextureCube<float4> reflection_probe1 : register(t29, space2);
TextureCube<float4> reflection_probe2 : register(t30, space2);
TextureCube<float4> reflection_probe3 : register(t31, space2);
Texture2D<float4> planar_reflection : register(t32, space2);
StructuredBuffer<Light> lights : register(t33, space2);
StructuredBuffer<InlineEffectOp> inline_effects : register(t34, space2);
StructuredBuffer<float4> inline_effect_parameters : register(t35, space2);
StructuredBuffer<float4> ltc_tables : register(t36, space2);
StructuredBuffer<LightShadow> light_shadows : register(t37, space2);
StructuredBuffer<float4> baked_probes : register(t38, space2);
#else
Texture2D<float4> transmission_background : register(t14, space2);
Texture2D<float> ies_atlas : register(t15, space2);
StructuredBuffer<Light> lights : register(t16, space2);
StructuredBuffer<InlineEffectOp> inline_effects : register(t17, space2);
StructuredBuffer<float4> inline_effect_parameters : register(t18, space2);
StructuredBuffer<float4> ltc_tables : register(t19, space2);
#endif

cbuffer SceneConstants : register(b0, space3) {
    // w = 1: xyz is the eye. w = 0: an orthographic view, and xyz is the unit
    // direction toward the viewer, the same at every pixel.
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
    // x: how an editor view shades - 0 lit, 1 unlit, 2 lights only,
    // 3 wireframe. A show always renders lit.
    float4 view_mode;
    // Directional shadow cascades, deliberately independent of output slice:
    // near cascades are fit to sub-frusta of the authored full-canvas camera
    // and the last one is the full-scene fit, so two nodes select the same
    // cascade for the same world point. Only counts.z of them are valid.
    column_major float4x4 shadow_view_projection[4];
    // xy: reciprocal shadow extent. z: constant bias. w: slope bias. Both
    // biases are in shadow texels, so a tight near cascade is not pushed off
    // its receiver by an offset sized for the whole-scene map: that is what
    // would leave a contact shadow detached from the thing casting it.
    float4 shadow_parameters;
    // Reversed-depth offset of one texel in each cascade.
    float4 shadow_cascade_texel_depth;
    // Inverse of the last (full-scene) cascade, which is the RSM projection.
    column_major float4x4 shadow_inverse_view_projection;
    // x: RSM sample radius in UV; y: GI strength; z: minimum squared distance.
    float4 gi_parameters;
    // x: environment intensity, folded from the authored map_intensity. Zero
    // means no environment is resident and the hemisphere below stands in,
    // which is the ordinary case for a scene that authored none and must not
    // read as a failure.
    // y: how many mips the prefiltered chain has, so this shader can invert
    // the roughness-to-level mapping the prefilter used.
    float4 environment_parameters;
    column_major float4x4 lighting_inverse_view_projection;
    float4 lighting_canvas;
    float4 baked_minimum;
    float4 baked_maximum;
    float4 baked_dimensions;
    float4 texture_sampling;
    float4 shadow_quality;
    float4 contact_shadow;
    column_major float4x4 lighting_view_projection;
    float4 reflection_position[4];
    float4 reflection_extent[4];
    float4 reflection_parameters[4];
    float4 planar_parameters;

};

#if defined(MEDIASERVER_SCENE_LINEAR)
#include "baked_lighting.hlsli"
#include "reflections.hlsli"

// Irradiance from the nine projected coefficients.
//
// The coefficients are the environment projected onto the basis; turning them
// into irradiance needs the cosine-lobe convolution factors, which are the
// three constants below. Without them this returns radiance rather than
// irradiance and every diffuse surface is off by a band-dependent factor -
// which reads as a slightly wrong colour rather than as an obvious error.
float3 environment_irradiance(float3 n) {
    // Ramamoorthi and Hanrahan's Â terms for bands zero, one and two.
    const float a0 = 3.14159265;
    const float a1 = 2.09439510;
    const float a2 = 0.78539816;

    float basis[9];
    basis[0] = 0.282095;
    basis[1] = 0.488603 * n.y;
    basis[2] = 0.488603 * n.z;
    basis[3] = 0.488603 * n.x;
    basis[4] = 1.092548 * n.x * n.y;
    basis[5] = 1.092548 * n.y * n.z;
    basis[6] = 0.315392 * (3.0 * n.z * n.z - 1.0);
    basis[7] = 1.092548 * n.x * n.z;
    basis[8] = 0.546274 * (n.x * n.x - n.y * n.y);

    float3 result = irradiance_texture.Load(int3(0, 0, 0)).rgb * basis[0] * a0;
    [unroll]
    for (int band1 = 1; band1 < 4; ++band1) {
        result += irradiance_texture.Load(int3(band1, 0, 0)).rgb
            * (basis[band1] * a1);
    }
    [unroll]
    for (int band2 = 4; band2 < 9; ++band2) {
        result += irradiance_texture.Load(int3(band2, 0, 0)).rgb
            * (basis[band2] * a2);
    }
    return max(result, 0.0);
}

float3 diffuse_irradiance(float3 n, float3 position) {
    [branch] if (baked_dimensions.x > 0.0) return baked_irradiance(position, n);
    if (environment_parameters.x > 0.0) return environment_irradiance(n) * environment_parameters.x;
    return lerp(ground_exposure.rgb, sky_ambient.rgb, n.y * 0.5 + 0.5) * sky_ambient.a * pi;
}

// The split-sum ambient term: prefiltered environment times the integrated
// BRDF.
float3 environment_ambient(float3 normal, float3 view, float3 albedo,
                           float roughness, float metallic, float3 position, float2 pixel, float planar_weight) {


    // Diffuse. Dividing the irradiance by pi is what turns it back into
    // outgoing radiance for a Lambertian surface.
    const float3 diffuse =
        diffuse_irradiance(normal, position) / 3.14159265
        * albedo * (1.0 - metallic);

    // Specular. The level inverts the prefilter's own roughness-per-mip
    // mapping, which is linear precisely so this inversion is exact - see
    // EnvironmentPrefilter::mip_roughness.
    const float3 reflection = reflect(-view, normal);
    const float3 prefiltered = reflection_radiance(position, reflection, roughness, pixel, planar_weight);

    const float n_dot_v = saturate(dot(normal, view));
    const float2 integrated = brdf_texture.SampleLevel(
        linear_clamp_sampler, float2(n_dot_v, saturate(roughness)), 0.0).rg;
    // The usual dielectric base reflectance, lerped toward the albedo for a
    // metal - the same F0 the direct lighting above uses.
    const float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    const float3 specular = prefiltered * (f0 * integrated.x + integrated.y);

    return diffuse + specular;
}
#endif

#include "scene_varyings.hlsli"
#include "scene_media_coverage.hlsli"

// How much of what is behind this pixel the layer hides: its opacity, the
// horizontal edge feather, and the material's coverage when it blends.
float layer_opacity(Varyings input, float alpha_coverage) {
    const float edge_distance = min(input.uv.x, 1.0 - input.uv.x);
    const float horizontal_coverage = input.layer_style.y > 0.0
        ? smoothstep(0.0, input.layer_style.y, edge_distance)
        : 1.0;
    return saturate(input.layer_style.x * horizontal_coverage
        * (input.alpha.y > 1.5 ? alpha_coverage : 1.0));
}

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
    float metallic, bool diffuse_only) {
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

    return (diffuse + (diffuse_only ? float3(0,0,0) : specular)) * n_dot_l;
}

#include "material_lobes.hlsli"

#if defined(MEDIASERVER_SCENE_LINEAR)
static const float2 rsm_offsets[16] = {
    float2(-0.6134,  0.6175), float2( 0.1700, -0.0403),
    float2(-0.2994, -0.7919), float2( 0.6457,  0.4932),
    float2(-0.6518, -0.3625), float2( 0.4210, -0.7336),
    float2( 0.7997, -0.1686), float2(-0.0994,  0.1437),
    float2(-0.8172,  0.1051), float2( 0.3302,  0.7884),
    float2(-0.2186,  0.9100), float2( 0.0834, -0.9217),
    float2( 0.9175,  0.3301), float2(-0.9273, -0.1329),
    float2( 0.5421,  0.0528), float2(-0.3973,  0.3281)
};

#include "shadow_filter.hlsli"

// Point sample of one cascade map. The reflective-shadow-map pass reconstructs
// virtual point lights from the last cascade, which is the full-scene fit, so
// every node derives the same indirect field from the same depths.
float shadow_cascade_depth(uint index, float2 uv) {
    if (index == 0u) {
        return shadow_cascade0_texture.SampleLevel(
            nearest_clamp_sampler, uv, 0.0);
    }
    if (index == 1u) {
        return shadow_cascade1_texture.SampleLevel(
            nearest_clamp_sampler, uv, 0.0);
    }
    if (index == 2u) {
        return shadow_cascade2_texture.SampleLevel(
            nearest_clamp_sampler, uv, 0.0);
    }
    return shadow_cascade3_texture.SampleLevel(
        nearest_clamp_sampler, uv, 0.0);
}

float3 directional_rsm_indirect(
    float3 world_position,
    float3 receiver_normal) {
    const uint far_cascade = min(counts.z, 4u) - 1u;
    const float4 receiver_clip = mul(
        shadow_view_projection[far_cascade], float4(world_position, 1.0));
    if (!(receiver_clip.w > 0.0)) {
        return 0.0;
    }
    const float2 receiver_ndc = receiver_clip.xy / receiver_clip.w;
    const float2 receiver_uv = float2(
        receiver_ndc.x * 0.5 + 0.5,
        0.5 - receiver_ndc.y * 0.5);
    if (receiver_uv.x <= 0.0 || receiver_uv.x >= 1.0
        || receiver_uv.y <= 0.0 || receiver_uv.y >= 1.0) {
        return 0.0;
    }

    float3 indirect = 0.0;
    // A loop rather than unrolled, like the shadow kernel rows: a smaller
    // lit stage keeps more pixels in flight for these dependent fetches.
    [loop]
    for (uint index = 0u; index < 16u; ++index) {
        const float2 sample_uv =
            receiver_uv + rsm_offsets[index] * gi_parameters.x;
        if (sample_uv.x <= 0.0 || sample_uv.x >= 1.0
            || sample_uv.y <= 0.0 || sample_uv.y >= 1.0) {
            continue;
        }
        const float depth = shadow_cascade_depth(far_cascade, sample_uv);
        if (!(depth > 0.0)) {
            continue;
        }
        const float4 clip = float4(
            sample_uv.x * 2.0 - 1.0,
            1.0 - sample_uv.y * 2.0,
            depth,
            1.0);
        const float4 reconstructed = mul(
            shadow_inverse_view_projection, clip);
        if (!(abs(reconstructed.w) > 1e-6)) {
            continue;
        }
        const float3 vpl_position =
            reconstructed.xyz / reconstructed.w;
        const float3 to_vpl = vpl_position - world_position;
        const float distance_squared = dot(to_vpl, to_vpl);
        if (!(distance_squared > gi_parameters.z)) {
            continue;
        }
        const float3 direction = to_vpl * rsqrt(distance_squared);
        const float3 vpl_normal = normalize(
            rsm_normal_texture.SampleLevel(
                nearest_clamp_sampler, sample_uv, 0.0).xyz * 2.0 - 1.0);
        const float receiver_facing = saturate(dot(
            receiver_normal, direction));
        const float emitter_facing = saturate(dot(
            vpl_normal, -direction));
        const float geometry = receiver_facing * emitter_facing
            / (1.0 + distance_squared * distance_squared);
        const float radial = 1.0 - saturate(length(rsm_offsets[index]));
        const float weight = geometry * radial * radial;
        indirect += rsm_flux_texture.SampleLevel(
            nearest_clamp_sampler, sample_uv, 0.0).rgb * weight;
    }
    return indirect * (gi_parameters.y / 16.0);
}
#endif

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

// A Background surface's fill (scene_renderer.cpp: SurfaceFillOp), carried as
// an inline operation that apply_inline_effects ignores. The ramp is measured
// on the CPU in the surface's world size, so this only places the pixel and
// interpolates - the same arithmetic as compositor_merge.frag.hlsl generate().
// Returns linear colour and straight alpha; white and opaque without a fill.
static const uint surface_fill_kind = 32u;

float4 surface_fill(uint4 range, float2 uv) {
    for (uint index = 0u; index < range.y; ++index) {
        const InlineEffectOp operation = inline_effects[range.x + index];
        if (operation.kind != surface_fill_kind
            || operation.parameter_count != 4u) {
            continue;
        }
        const float4 ramp = inline_effect_parameters[operation.parameter_offset];
        const float4 placement =
            inline_effect_parameters[operation.parameter_offset + 1u];
        const float4 color_a =
            inline_effect_parameters[operation.parameter_offset + 2u];
        const float4 color_b =
            inline_effect_parameters[operation.parameter_offset + 3u];
        const float2 position = uv * placement.zw;
        float t = 0.0;
        if (ramp.x > 0.5 && ramp.x < 1.5) { // FillMode::Linear
            t = dot(position - placement.xy, ramp.zw) * ramp.y + 0.5;
        } else if (ramp.x > 1.5) { // FillMode::Radial
            t = length(position - placement.xy) * ramp.y;
        }
        t = saturate(t);
        // Premultiplied interpolation, so a transparent end contributes no
        // colour of its own to the ramp.
        const float4 mixed = lerp(float4(color_a.rgb * color_a.a, color_a.a),
                                  float4(color_b.rgb * color_b.a, color_b.a), t);
        const float alpha = saturate(mixed.a);
        const float3 straight = alpha > 1e-6 ? saturate(mixed.rgb / alpha)
                                             : float3(0.0, 0.0, 0.0);
        return float4(decode_srgb(straight), alpha);
    }
    return float4(1.0, 1.0, 1.0, 1.0);
}

#include "area_lighting.hlsli"

float4 main(Varyings input, bool front_face : SV_IsFrontFace) : SV_Target0 {
    // A depth-only instance belongs to a different compositor layer and is
    // drawn here only so that this layer is occluded by it. Depth is still
    // written - there is no discard - but nothing reaches the colour target,
    // and a fully transparent black is also inert under every blend equation
    // the layer composite can apply afterwards. A blended layer's own depth
    // pass (2) goes on to find its coverage first.
    if (input.layer_style.w > 0.5 && input.layer_style.w < 1.5) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    if (input.alpha.w < 0.0) front_face = !front_face;
    if (!front_face && input.alpha.z < 0.5) discard;
    const float face_sign = front_face ? 1.0 : -1.0;
    const float3 geometric_normal = normalize(input.world_normal) * face_sign;
    const float3 tangent = normalize(
        input.world_tangent.xyz
        - geometric_normal
            * dot(geometric_normal, input.world_tangent.xyz)) * face_sign;
    const float3 bitangent =
        cross(geometric_normal, tangent) * input.world_tangent.w * face_sign;
    // Two-sided shading reverses the complete tangent frame. Reversing only
    // N and B leaves normal-map X facing the wrong way on the back of a scan.
    float3 tangent_normal =
        normal_texture.SampleBias(normal_sampler, input.uv, texture_sampling.x).xyz * 2.0 - 1.0;
    if (input.material_properties.z > 0.5) tangent_normal.z = sqrt(saturate(1.0 - dot(tangent_normal.xy, tangent_normal.xy)));
    tangent_normal.xy *= input.material_properties.x;
    tangent_normal = normalize(tangent_normal);
    const float3 normal = normalize(
        tangent * tangent_normal.x
        + bitangent * tangent_normal.y
        + geometric_normal * tangent_normal.z);
    const float4 sampled_base =
        base_color_texture.SampleBias(base_color_sampler, input.uv, texture_sampling.x);
    const float4 sampled_metallic_roughness = metallic_roughness_texture.SampleBias(metallic_roughness_sampler, input.uv, texture_sampling.x);
    const float3 sampled_emissive = decode_srgb(
        emissive_texture.SampleBias(emissive_sampler, input.uv, texture_sampling.x).rgb);
    const float sampled_occlusion =
        occlusion_texture.SampleBias(occlusion_sampler, input.uv, texture_sampling.x).r;
    // Lights only: a neutral grey surface, so only the light on it shows.
    const bool lights_only = view_mode.x > 1.5 && view_mode.x < 2.5;
    const float4 fill = surface_fill(input.effect_range, input.uv);
    const float3 base_albedo = input.base_color_roughness.rgb *
        decode_srgb(sampled_base.rgb) * fill.rgb;
    const float3 albedo = lights_only ? float3(0.5, 0.5, 0.5) : base_albedo;
    // glTF packs roughness in G and metallic in B. These are data channels,
    // sampled linearly rather than passed through the colour transfer curve.
    float roughness = saturate(
        input.base_color_roughness.a * sampled_metallic_roughness.g);
#if MEDIASERVER_SCENE_LINEAR
    // Integrate subpixel geometric normal variation into the GGX width. Fine
    // derivatives use a fixed 2x2 quad, no time, random seed, or node-local state.
    const float3 normal_dx = ddx_fine(geometric_normal);
    const float3 normal_dy = ddy_fine(geometric_normal);
    const float normal_variance = min(0.18,
        dot(normal_dx, normal_dx) + dot(normal_dy, normal_dy));
    roughness = pow(saturate(pow(roughness, 4.0) + normal_variance), 0.25);
#endif
    const float metallic = lights_only ? 0.0 : saturate(
        input.emissive_metallic.a * sampled_metallic_roughness.b);
    const float3 emissive = lights_only ? float3(0.0, 0.0, 0.0)
        : input.emissive_metallic.rgb * sampled_emissive * fill.rgb;

    const float alpha_coverage =
        saturate(input.anisotropy.w * sampled_base.a * fill.a);
    if (input.alpha.y > 0.5 && input.alpha.y < 1.5 && alpha_coverage < input.alpha.x) discard;
    if (input.layer_style.w > 1.5) {
        // Depth for the pixels that hide what is behind them completely, so
        // a blended layer crossing another is hidden where it passes behind
        // it. Translucent pixels write nothing and stay ordered by the sort.
        if (layer_opacity(input, alpha_coverage)
                < blended_depth_coverage_cutoff) discard;
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    MaterialLobes m;
    m.coat = input.coat.x * clearcoat_texture.SampleBias(clearcoat_sampler, input.uv, texture_sampling.x).r;
    m.coat_roughness = input.coat.y * clearcoat_roughness_texture.SampleBias(clearcoat_roughness_sampler, input.uv, texture_sampling.x).g;
    float3 coat_normal = clearcoat_normal_texture.SampleBias(clearcoat_normal_sampler, input.uv, texture_sampling.x).xyz * 2.0 - 1.0;
    if (input.material_properties.w > 0.5) coat_normal.z = sqrt(saturate(1.0 - dot(coat_normal.xy, coat_normal.xy)));
    coat_normal.xy *= input.coat.z;
    m.coat_normal = normalize(tangent * coat_normal.x + bitangent * coat_normal.y + geometric_normal * coat_normal.z);
#if MEDIASERVER_SCENE_LINEAR
    m.coat_roughness = pow(saturate(pow(m.coat_roughness, 4.0) + normal_variance), 0.25);
#endif
    m.sheen = input.sheen.rgb * decode_srgb(sheen_color_texture.SampleBias(sheen_color_sampler, input.uv, texture_sampling.x).rgb);
    m.sheen_roughness = input.sheen.a * sheen_roughness_texture.SampleBias(sheen_roughness_sampler, input.uv, texture_sampling.x).a;
    m.transmission = input.coat.w * transmission_texture.SampleBias(transmission_sampler, input.uv, texture_sampling.x).r;
    m.specular_weight = input.specular.a * material_specular_texture.SampleBias(material_specular_sampler, input.uv, texture_sampling.x).a;
    const float ior_f0 = input.anisotropy.z == 1.5 ? 0.04
        : pow((input.anisotropy.z - 1.0) / (input.anisotropy.z + 1.0), 2.0);
    const float3 sampled_specular_color = specular_color_texture.SampleBias(specular_color_sampler, input.uv, texture_sampling.x).rgb;
    m.dielectric_f0 = min(ior_f0 * input.specular.rgb * decode_srgb(sampled_specular_color), 1.0) * m.specular_weight;
    const float3 aniso_map = anisotropy_texture.SampleBias(anisotropy_sampler, input.uv, texture_sampling.x).rgb;
    float2 direction = aniso_map.rg * 2.0 - 1.0;
    direction = dot(direction, direction) > 0.0001 ? normalize(direction) : float2(1.0, 0.0);
    const float sine = sin(input.anisotropy.y), cosine = cos(input.anisotropy.y);
    direction = float2(cosine * direction.x - sine * direction.y, sine * direction.x + cosine * direction.y);
    const float3 rotated_tangent = tangent * direction.x + bitangent * direction.y;
    m.tangent = normalize(rotated_tangent - normal * dot(normal, rotated_tangent));
    m.bitangent = cross(normal, m.tangent) * input.world_tangent.w;
    m.anisotropy = input.anisotropy.x * aniso_map.b;
    if (lights_only) {
        // A plain dielectric: no coat, sheen, transmission or anisotropy.
        m.coat = 0.0;
        m.sheen = float3(0.0, 0.0, 0.0);
        m.transmission = 0.0;
        m.anisotropy = 0.0;
        m.specular_weight = 1.0;
        m.dielectric_f0 = float3(0.04, 0.04, 0.04);
    }
    const bool extended_lighting = m.coat > 0.0 || max_component(m.sheen) > 0.0
        || m.transmission > 0.0 || m.anisotropy > 0.0 || m.specular_weight != 1.0
        || input.anisotropy.z != 1.5 || any(input.specular.rgb != 1.0)
        || any(sampled_specular_color != 1.0);
    // The hardware may interpolate world attributes with different last bits
    // after translating a viewport. Reconstruct lighting at the absolute canvas
    // pixel instead, using the projection all slices share. This also keeps the
    // opaque background consumed by transmission on the same lighting grid.
    const float2 canvas_pixel = input.position.xy * float2(grain_extent.xy)
        / max(float2(grain_extent.zw), 1.0) + lighting_canvas.zw;
    const float2 ndc = canvas_pixel / lighting_canvas.xy * float2(2.0, -2.0) + float2(-1.0, 1.0);
    const float4 reconstructed = mul(lighting_inverse_view_projection, float4(ndc, input.position.z, 1.0));
    const float3 world_position = reconstructed.xyz / reconstructed.w;
    // w = 0: an orthographic view, whose view vector is the same everywhere.
    const float3 view = eye_position.w > 0.5
        ? normalize(eye_position.xyz - world_position)
        : eye_position.xyz;
    float3 coat_light = 0.0;
    float3 accumulated = float3(0.0, 0.0, 0.0);
    for (uint index = 0; index < counts.x; ++index) {
        const Light light = lights[index];
        if (light.kind == LIGHT_RECTANGLE || light.kind == LIGHT_SPHERE) {
            if (light.ies.w < -1.5) continue;
            const float3 emission = light.color_intensity.rgb * light.color_intensity.a;
            accumulated += area_surface_response(light, world_position, normal, view, albedo, roughness, metallic, m) * emission;
            if (m.coat > 0.0) coat_light += area_coat_response(light, world_position, view, m) * emission;
            continue;
        }
        float3 to_light;
        float attenuation = 1.0;

        if (light.kind != LIGHT_DIRECTIONAL) {
            const float3 offset = light.vector - world_position;
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
            attenuation *= light_ies_weight(ies_atlas, light, -to_light);

            // The cone, for a spot.
            //
            // `to_light` runs from the surface toward the light, and the axis
            // runs away from the light, so the surface is inside the cone when
            // the axis agrees with the vector *from* the light.
            //
            // Every practical on a set is a spot of some kind - a lamp, a
            // window, a screen - and without this they all light like bare
            // bulbs, spilling in every direction including through the wall
            // behind them.
            if (light.kind == LIGHT_SPOT) {
                const float alignment = dot(-to_light, light.spot.xyz);
                // Photometric cones use smoothstep; the shared helper keeps
                // legacy unitless pixels unchanged, including fog scatter.
                const float cone = light_cone_weight(light, alignment);
                if (cone <= 0.0) {
                    continue;
                }
                attenuation *= cone;

                // And its shadow, if this light got an atlas tile. Without
                // this a practical lights straight through the wall it is
                // mounted on, which on an interior set is the first thing
                // anybody notices.
                //
                // Advanced only: the atlas and the shadow table are bound in
                // that branch alone, and Basic remains the reference path that
                // renders exactly what it always did.
#if defined(MEDIASERVER_SCENE_LINEAR)
                const int shadow_row = (int)light.params.w;
                if (shadow_row >= 0) {
                    attenuation *= spot_shadow_visibility(
                        (uint)shadow_row, world_position, geometric_normal,
                        saturate(dot(normal, to_light)));
                }
#endif
            }
        } else {
            to_light = -normalize(light.vector);
        }

        const float3 radiance =
            light.color_intensity.rgb * light.color_intensity.a * attenuation;
        float visibility = 1.0;
#if defined(MEDIASERVER_SCENE_LINEAR)
        if (counts.z != 0u && index == counts.w
            && light.kind == LIGHT_DIRECTIONAL) {
            visibility = directional_shadow_visibility(
                world_position, geometric_normal, light.vector);
        }
#endif
#if defined(MEDIASERVER_SCENE_LINEAR)
        if (contact_shadow.x > 0.0 && ((index == counts.w && light.kind == LIGHT_DIRECTIONAL)
            || (light.kind == LIGHT_SPOT && light.params.w >= 0.0))) {
            visibility *= contact_shadow_visibility(world_position, geometric_normal, to_light,
                light.kind == LIGHT_DIRECTIONAL ? contact_shadow.x : min(contact_shadow.x, length(light.vector - world_position)));
        }
#endif
        const bool diffuse_only = light.kind == LIGHT_DIRECTIONAL && light.params.x < 0.0;
        accumulated += (extended_lighting
            ? extended_surface_response(normal, view, to_light, albedo, roughness, metallic, m, diffuse_only)
            : surface_response(normal, view, to_light, albedo, roughness, metallic, diffuse_only)) * radiance * visibility;
        if (m.coat > 0.0 && !diffuse_only) coat_light += clearcoat_response(view, to_light, m) * radiance * visibility;
    }
#if defined(MEDIASERVER_SCENE_LINEAR)
    if (counts.z != 0u && gi_parameters.y > 0.0) {
        accumulated += directional_rsm_indirect(
            input.world_position, normal)
            * albedo * (1.0 - metallic) * (1.0 - m.transmission) / pi;
    }
#endif

    const float baked_occlusion = 1.0 + input.material_properties.y
        * (sampled_occlusion - 1.0);
#if defined(MEDIASERVER_SCENE_LINEAR)
    // Occlusion between separate objects, which no baked texture can know
    // about. Multiplied with the per-asset term rather than replacing it: the
    // two describe different scales of the same thing, and an asset's own
    // creases are still its own.
    //
    // Applied to the whole ambient term, specular included. That is not
    // physical - a mirror in a corner still sees the room - but it is what
    // stops a chrome prop from reading as unlit-and-floating, and the choice
    // matters far less than it being the *same* choice on every node.
    //
    // The tap is clamped to whatever is actually bound, because when no
    // occlusion was recorded that is the one-by-one white fallback. `Load`
    // does not clamp - an out-of-range read returns zero, and zero here is not
    // "no occlusion", it is "no ambient light at all". A scene with occlusion
    // switched off rendered completely black until this clamp existed.
    uint occlusion_width;
    uint occlusion_height;
    screen_occlusion_texture.GetDimensions(occlusion_width, occlusion_height);
    const int2 occlusion_pixel = clamp(
        (int2)input.position.xy, int2(0, 0),
        int2((int)occlusion_width - 1, (int)occlusion_height - 1));
    const float occlusion = baked_occlusion
        * screen_occlusion_texture.Load(int3(occlusion_pixel, 0)).r;
    if (environment_parameters.x > 0.0 || planar_parameters.y > 0.0 || input.alpha.w > 0.0) {
        // Image-based lighting. The diffuse half is nine coefficients
        // evaluated as a dot product; the specular half is one cubemap sample
        // and one table lookup. Both are constant-time regardless of how
        // complex the environment is, which is the entire point of
        // prefiltering it.
        accumulated += (extended_lighting
            ? extended_environment(normal, view, albedo, roughness, metallic, m, world_position, input.position.xy, max(input.alpha.w, 0.0))
            : environment_ambient(normal, view, albedo, roughness, metallic, world_position, input.position.xy, max(input.alpha.w, 0.0))) * occlusion;
        if (m.coat > 0.0) {
            const float2 integral = brdf_texture.SampleLevel(linear_clamp_sampler,
                float2(saturate(dot(m.coat_normal, view)), m.coat_roughness), 0.0).rg;
            coat_light += reflection_radiance(world_position, reflect(-view, m.coat_normal), m.coat_roughness,
                input.position.xy, max(input.alpha.w, 0.0)) * (integral.x + integral.y) * occlusion;
        }
    } else {
#endif
    // A two-colour hemisphere standing in for image-based lighting: enough
    // that an unlit face reads as being in shadow rather than as a hole in the
    // frame, and cheap enough not to matter.
    //
    // Still the fallback, and still exactly what it was: a scene that authored
    // no environment renders bit for bit as it did before any of this existed.
    const float hemisphere = normal.y * 0.5 + 0.5;
    const float3 ambient =
        lerp(ground_exposure.rgb, sky_ambient.rgb, hemisphere) * sky_ambient.a;
#if defined(MEDIASERVER_SCENE_LINEAR)
    accumulated += (baked_dimensions.x > 0.0
        ? baked_irradiance(input.world_position, normal) * (1.0 - metallic) / pi
        : ambient) * albedo * occlusion * (1.0 - m.transmission);
    }
#else
    accumulated += ambient * albedo * baked_occlusion * (1.0 - m.transmission);
#endif
    accumulated += emissive;
    if (m.transmission > 0.0 && metallic < 1.0) {
        uint width, height;
        transmission_background.GetDimensions(width, height);
        const int2 pixel = (int2)input.position.xy;
        // Bounded thin-surface microfacet blur. Eight canvas pixels at maximum
        // roughness; apron_requirements reserves the same support. Integer taps
        // avoid slice-relative UV round trips and cannot introduce a wall seam.
        const float radius = 8.0 * roughness * roughness * saturate(2.0 * (input.anisotropy.z - 1.0));
        const int2 step_px = (int2)round(radius * float2(grain_extent.zw) / max(float2(grain_extent.xy), 1.0));
        float3 background = 0.0;
        [unroll] for (int y = -1; y <= 1; ++y) {
            [unroll] for (int x = -1; x <= 1; ++x) {
                const int2 tap = clamp(pixel + int2(x, y) * step_px, int2(0, 0), int2(width, height) - 1);
                background += transmission_background.Load(int3(tap, 0)).rgb;
            }
        }
        background /= 9.0;
#if defined(MEDIASERVER_SCENE_LINEAR)
        background /= max(ground_exposure.a, 0.00001);
#else
        // Basic stores display-referred colour. Convert the copied scene back
        // to linear light, undoing its Reinhard response before relighting.
        background = decode_srgb(background);
        background = background / max(1.0 - background, 0.00001) / max(ground_exposure.a, 0.00001);
#endif
        const float3 fresnel = material_fresnel(m.dielectric_f0, m.specular_weight, saturate(dot(normal, view)));
        accumulated += background * albedo * (1.0 - fresnel) * (1.0 - metallic) * m.transmission;
    }
    if (m.coat > 0.0) {
        const float weight = m.coat * (0.04 + 0.96 * pow(1.0 - saturate(dot(m.coat_normal, view)), 5.0));
        accumulated = lerp(accumulated, coat_light, weight);
    }

    const float opacity = layer_opacity(input, alpha_coverage);
    // The program is already canonicalised on the CPU. The project's response
    // setting decides only where that authored order runs: scene-linear before
    // tone mapping for Exposure, or display-referred after encoding for Gain.
    const bool display_gain = counts.y != 0u;
    // Unlit and wireframe replace the lit result with the surface's own
    // colour, unexposed, so a base colour reads as itself.
    const bool unlit = view_mode.x > 0.5 && view_mode.x < 1.5;
    const bool wireframe = view_mode.x > 2.5;
    if (unlit) {
        accumulated = base_albedo + emissive;
    } else if (wireframe) {
        accumulated = 0.35 + 0.65 * base_albedo;
    }
    const float3 exposed =
        accumulated * (unlit || wireframe ? 1.0 : ground_exposure.a);
#if !defined(MEDIASERVER_SCENE_LINEAR)
    // Display-referred only. The Advanced path is scene-linear all the way to
    // the output stage, which owns the display transform - and this block used
    // to be computed there too and thrown away, which is a tone map per pixel
    // for nothing and an invitation to apply the transform twice.
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
#endif
#if defined(MEDIASERVER_SCENE_LINEAR)
    // The advanced working target is premultiplied scene-linear RGBA16F.
    // Display-referred effects are converted back to linear after evaluation;
    // the display transform itself remains an output-stage responsibility.
    float3 linear_output = display_gain
        ? decode_srgb(saturate(apply_inline_effects(
              encode_srgb(max(exposed, 0.0)), input.effect_range,
              input.position.xy)))
        : max(apply_inline_effects(
              exposed, input.effect_range, input.position.xy), 0.0);
    float3 linear_source =
        blend_source(linear_output, opacity, input.layer_style.z);
    if (input.layer_style.z < 1.5) { // Normal and Add use premultiplied factors.
        linear_source *= opacity;
    }
    return float4(linear_source, opacity);
#else
    return float4(
        blend_source(display, opacity, input.layer_style.z), opacity);
#endif
}
