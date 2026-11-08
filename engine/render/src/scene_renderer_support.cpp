#include "scene_renderer_internal.hpp"

#include <mediaserver/shaders/output_vert_dxil.h>
#include <mediaserver/shaders/scene_coverage_frag_dxil.h>
#include <mediaserver/shaders/sky_frag_dxil.h>
#include <mediaserver/shaders/solid_vert_dxil.h>
#include <mediaserver/shaders/scene_frag_dxil.h>
#include <mediaserver/shaders/scene_linear_frag_dxil.h>
#include <mediaserver/shaders/scene_media_frag_dxil.h>
#include <mediaserver/shaders/scene_media_linear_frag_dxil.h>
#include <mediaserver/shaders/scene_media_normal_frag_dxil.h>
#include <mediaserver/shaders/scene_media_object_id_frag_dxil.h>
#include <mediaserver/shaders/scene_media_velocity_frag_dxil.h>
#include <mediaserver/shaders/scene_normal_frag_dxil.h>
#include <mediaserver/shaders/scene_object_id_frag_dxil.h>
#include <mediaserver/shaders/scene_rsm_flux_frag_dxil.h>
#include <mediaserver/shaders/scene_rsm_normal_frag_dxil.h>
#include <mediaserver/shaders/scene_shadow_frag_dxil.h>
#include <mediaserver/shaders/scene_velocity_frag_dxil.h>
#include <mediaserver/shaders/scene_auxiliary_frag_dxil.h>
#include <mediaserver/shaders/scene_media_auxiliary_frag_dxil.h>
#include <mediaserver/shaders/scene_vert_dxil.h>
#include <mediaserver/shaders/scene_motion_vert_dxil.h>

namespace mediaserver::render::scene_detail {

[[nodiscard]] ShaderBlobs shader_blobs() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return ShaderBlobs{
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_scene_vert_dxil),
            sizeof(mediaserver_scene_vert_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_scene_motion_vert_dxil),
            sizeof(mediaserver_scene_motion_vert_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_scene_frag_dxil),
            sizeof(mediaserver_scene_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_scene_linear_frag_dxil),
            sizeof(mediaserver_scene_linear_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_scene_media_frag_dxil),
            sizeof(mediaserver_scene_media_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_media_linear_frag_dxil),
            sizeof(mediaserver_scene_media_linear_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_media_object_id_frag_dxil),
            sizeof(mediaserver_scene_media_object_id_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_media_velocity_frag_dxil),
            sizeof(mediaserver_scene_media_velocity_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_media_normal_frag_dxil),
            sizeof(mediaserver_scene_media_normal_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_object_id_frag_dxil),
            sizeof(mediaserver_scene_object_id_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_velocity_frag_dxil),
            sizeof(mediaserver_scene_velocity_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_normal_frag_dxil),
            sizeof(mediaserver_scene_normal_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_auxiliary_frag_dxil),
            sizeof(mediaserver_scene_auxiliary_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_media_auxiliary_frag_dxil),
            sizeof(mediaserver_scene_media_auxiliary_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_shadow_frag_dxil),
            sizeof(mediaserver_scene_shadow_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_rsm_normal_frag_dxil),
            sizeof(mediaserver_scene_rsm_normal_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_rsm_flux_frag_dxil),
            sizeof(mediaserver_scene_rsm_flux_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_output_vert_dxil),
            sizeof(mediaserver_output_vert_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_scene_coverage_frag_dxil),
            sizeof(mediaserver_scene_coverage_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_solid_vert_dxil),
            sizeof(mediaserver_solid_vert_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_sky_frag_dxil),
            sizeof(mediaserver_sky_frag_dxil)},
        rhi::ShaderFormat::Dxil,
        true,
        true,
    };
#else
    // No shader compiler was available when this was built, so there is no 3D
    // path to offer. Saying so plainly is the whole point of the flag.
    return ShaderBlobs{};
#endif
}

[[nodiscard]] rhi::TextureDesc color_desc(
    core::u32 width,
    core::u32 height,
    rhi::TextureFormat format) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.color_target = true;
    // Sampled as well as drawn into, because presentation blits from it and a
    // readback test reads it.
    desc.sampled = true;
    return desc;
}

[[nodiscard]] rhi::TextureDesc sampled_desc(
    core::u32 width,
    core::u32 height) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.sampled = true;
    return desc;
}

[[nodiscard]] rhi::TextureDesc depth_desc(core::u32 width, core::u32 height,
                                          bool sampled) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Depth32Float;
    desc.depth_target = true;
    // The auxiliary depth is read by the occlusion pass. Reconstructing
    // position from anything else means reconstructing it from a derivative,
    // which spans two surfaces at exactly the discontinuities occlusion cares
    // about. The colour pass's own depth stays write-only.
    desc.sampled = sampled;
    return desc;
}

[[nodiscard]] GpuInstance to_gpu(const scene::MeshInstance& instance,
                                 core::u32 first_effect,
                                 core::u32 effect_count,
                                 bool depth_only,
                                 DecodedPixelLayout media_layout,
                                 const scene::Mat4* previous_world) noexcept {
    GpuInstance record{};
    for (core::u32 index = 0U; index < 16U; ++index) {
        record.world[index] = instance.world.m[index];
        record.previous_world[index] = previous_world == nullptr
            ? instance.world.m[index]
            : previous_world->m[index];
    }
    // The first twelve elements of a column-major 4x4 are exactly its first
    // three columns, and the fourth element of each is zero for a basis, so
    // this is a copy rather than a gather.
    for (core::u32 index = 0U; index < 12U; ++index) {
        record.normal[index] = instance.normal.m[index];
    }
    record.base_color[0U] = instance.material.base_color.x;
    record.base_color[1U] = instance.material.base_color.y;
    record.base_color[2U] = instance.material.base_color.z;
    record.roughness = instance.material.roughness;
    record.emissive[0U] = instance.material.emissive.x
        * instance.material.emissive_strength;
    record.emissive[1U] = instance.material.emissive.y
        * instance.material.emissive_strength;
    record.emissive[2U] = instance.material.emissive.z
        * instance.material.emissive_strength;
    record.metallic = instance.material.metallic;
    record.material_properties[0U] =
        std::isfinite(instance.material.normal_scale)
        ? std::max(instance.material.normal_scale, 0.0F)
        : 1.0F;
    record.material_properties[1U] =
        std::isfinite(instance.material.occlusion_strength)
        ? std::clamp(instance.material.occlusion_strength, 0.0F, 1.0F)
        : 1.0F;
    record.coat[0U] = instance.material.clearcoat;
    record.coat[1U] = instance.material.clearcoat_roughness;
    record.coat[2U] = instance.material.clearcoat_normal_scale;
    record.coat[3U] = instance.material.transmission;
    record.sheen[0U] = instance.material.sheen_color.x;
    record.sheen[1U] = instance.material.sheen_color.y;
    record.sheen[2U] = instance.material.sheen_color.z;
    record.sheen[3U] = instance.material.sheen_roughness;
    record.specular[0U] = instance.material.specular_color.x;
    record.specular[1U] = instance.material.specular_color.y;
    record.specular[2U] = instance.material.specular_color.z;
    record.specular[3U] = instance.material.specular;
    record.anisotropy[0U] = instance.material.anisotropy_strength;
    record.anisotropy[1U] = instance.material.anisotropy_rotation;
    record.anisotropy[2U] = instance.material.ior;
    record.anisotropy[3U] = instance.material.base_alpha;
    record.alpha[0U] = instance.material.alpha_cutoff;
    record.alpha[1U] = static_cast<float>(instance.material.alpha_mode);
    record.alpha[2U] = instance.material.double_sided ? 1.0F : 0.0F;
    record.layer_style[0U] = std::isfinite(instance.style.opacity)
        ? std::clamp(instance.style.opacity, 0.0F, 1.0F)
        : 1.0F;
    record.layer_style[1U] = std::isfinite(instance.style.horizontal_blend)
        ? std::clamp(instance.style.horizontal_blend, 0.0F, 0.5F)
        : 0.0F;
    switch (instance.style.blend) {
    case scene::BlendMode::Normal:
    case scene::BlendMode::Add:
    case scene::BlendMode::Multiply:
    case scene::BlendMode::Screen:
        record.layer_style[2U] = static_cast<float>(
            static_cast<core::u8>(instance.style.blend));
        break;
    default:
        record.layer_style[2U] = 0.0F;
        break;
    }
    // A depth-only instance stands in for geometry another compositor layer
    // owns. The fragment stage returns a fully transparent black for it, so it
    // contributes depth and nothing else; the flag rather than an opacity of
    // zero because zero opacity means "not drawn at all" everywhere else.
    record.layer_style[3U] = depth_only ? 1.0F : 0.0F;
    record.effect_range[0U] = depth_only ? 0U : first_effect;
    record.effect_range[1U] = depth_only ? 0U : effect_count;
    record.effect_range[2U] = static_cast<core::u32>(media_layout);
    const core::u64 source = instance.source.value();
    core::u32 object_id = static_cast<core::u32>(source)
        ^ static_cast<core::u32>(source >> 32U);
    object_id ^= object_id >> 16U;
    object_id *= 0x7FEB352DU;
    object_id ^= object_id >> 15U;
    object_id *= 0x846CA68BU;
    object_id ^= object_id >> 16U;
    record.effect_range[3U] = object_id == 0U ? 1U : object_id;
    // Blur is intentionally absent. It needs an offscreen source and a
    // separable compositor pass; treating a radius as a pointwise colour tweak
    // would claim an effect the renderer did not perform.
    return record;
}

[[nodiscard]] std::optional<SurfaceFillProgram> surface_fill_program(
    const scene::MeshInstance& instance) noexcept {
    if (!instance.material.fill.has_value()) {
        return std::nullopt;
    }
    const scene::Fill& fill = *instance.material.fill;
    const auto column_length = [&](std::size_t column) noexcept {
        const float x = instance.world.m[column * 4U];
        const float y = instance.world.m[column * 4U + 1U];
        const float z = instance.world.m[column * 4U + 2U];
        const float length = std::sqrt(x * x + y * y + z * z);
        return std::isfinite(length) ? length : 0.0F;
    };
    float width = 1.0F;
    float height = 1.0F;
    if (instance.mesh.primitive == scene::Primitive::Plane) {
        width = column_length(0U);
        height = column_length(2U);
    } else if (instance.mesh.primitive == scene::Primitive::Quad) {
        width = column_length(0U);
        height = column_length(1U);
    }
    SurfaceFillProgram program{};
    auto& ramp = program.parameters[0U].values;
    auto& placement = program.parameters[1U].values;
    ramp[0U] = static_cast<float>(static_cast<core::u8>(fill.mode));
    placement[0U] = fill.center_x * width;
    placement[1U] = fill.center_y * height;
    placement[2U] = width;
    placement[3U] = height;
    if (fill.mode == scene::FillMode::Linear) {
        const float x = scene::deterministic::cos_turns(fill.angle);
        const float y = scene::deterministic::sin_turns(fill.angle);
        // The surface's extent along the ramp, so a size of 1 runs exactly
        // from one edge to the opposite one.
        const float length =
            (std::abs(x) * width + std::abs(y) * height) * fill.size;
        ramp[1U] = length > 0.0F ? 1.0F / length : 0.0F;
        ramp[2U] = x;
        ramp[3U] = y;
    } else if (fill.mode == scene::FillMode::Radial) {
        const float radius =
            fill.size * 0.5F * std::sqrt(width * width + height * height);
        ramp[1U] = radius > 0.0F ? 1.0F / radius : 0.0F;
    }
    const auto color = [](const scene::FillColor& value,
                          GpuInlineEffectParameter& into) noexcept {
        into.values[0U] = value.r;
        into.values[1U] = value.g;
        into.values[2U] = value.b;
        into.values[3U] = value.a;
    };
    color(fill.color_a, program.parameters[2U]);
    color(fill.mode == scene::FillMode::Solid ? fill.color_a : fill.color_b,
          program.parameters[3U]);
    return program;
}

[[nodiscard]] bool compile_inline_program(
    const scene::EffectStack& effects,
    const scene::EffectSettings& settings,
    core::u32 max_operations,
    core::u32 max_parameters,
    core::u32 max_curve_luts,
    core::u32& curve_luts_used,
    std::vector<core::u8>& operation_bytes,
    std::vector<core::u8>& parameter_bytes,
    CompiledInlineRange& range,
    const std::optional<SurfaceFillProgram>& fill) {
    const std::size_t operation_start = operation_bytes.size();
    const std::size_t parameter_start = parameter_bytes.size();
    const core::u32 curve_lut_start = curve_luts_used;
    range.first = static_cast<core::u32>(
        operation_start / sizeof(GpuInlineEffectOp));
    range.count = 0U;

    const auto rollback = [&]() noexcept {
        operation_bytes.resize(operation_start);
        parameter_bytes.resize(parameter_start);
        curve_luts_used = curve_lut_start;
        range = CompiledInlineRange{};
    };
    const auto room_for = [&](core::u32 parameters) noexcept {
        const std::size_t held_operations =
            operation_bytes.size() / sizeof(GpuInlineEffectOp);
        const std::size_t held_parameters =
            parameter_bytes.size() / sizeof(GpuInlineEffectParameter);
        return held_operations < max_operations
            && held_parameters + parameters <= max_parameters;
    };
    const auto emit_scalar = [&](scene::EffectKind kind, float value) {
        if (!room_for(1U)) {
            return false;
        }
        const core::u32 parameter_offset = static_cast<core::u32>(
            parameter_bytes.size() / sizeof(GpuInlineEffectParameter));
        GpuInlineEffectParameter parameter{};
        parameter.values[0U] = value;
        append(parameter_bytes, parameter);
        append(operation_bytes,
               GpuInlineEffectOp{static_cast<core::u32>(kind),
                                 parameter_offset, 1U, 0U});
        ++range.count;
        return true;
    };
    const auto emit_levels = [&](const scene::LevelsParameters& authored) {
        const auto normalized = scene::normalized_levels(authored);
        if (!normalized.has_value() || !room_for(8U)) {
            return false;
        }
        const core::u32 parameter_offset = static_cast<core::u32>(
            parameter_bytes.size() / sizeof(GpuInlineEffectParameter));
        const scene::LevelsChannelParameters* const channels[]{
            &normalized->master, &normalized->red, &normalized->green,
            &normalized->blue};
        for (const scene::LevelsChannelParameters* const channel : channels) {
            GpuInlineEffectParameter first{};
            first.values[0U] = channel->input_black;
            first.values[1U] = channel->input_white;
            first.values[2U] = channel->gamma;
            first.values[3U] = channel->output_black;
            append(parameter_bytes, first);
            GpuInlineEffectParameter second{};
            second.values[0U] = channel->output_white;
            append(parameter_bytes, second);
        }
        append(operation_bytes,
               GpuInlineEffectOp{
                   static_cast<core::u32>(scene::EffectKind::Levels),
                   parameter_offset, 8U, 0U});
        ++range.count;
        return true;
    };
    const auto emit_lift_gamma_gain =
        [&](const scene::LiftGammaGainParameters& authored) {
            const auto normalized = scene::normalized_lift_gamma_gain(authored);
            if (!normalized.has_value() || !room_for(3U)) {
                return false;
            }
            const core::u32 parameter_offset = static_cast<core::u32>(
                parameter_bytes.size() / sizeof(GpuInlineEffectParameter));
            for (const scene::Vec3 vector : {
                     normalized->lift, normalized->gamma,
                     normalized->gain}) {
                GpuInlineEffectParameter parameter{};
                parameter.values[0U] = vector.x;
                parameter.values[1U] = vector.y;
                parameter.values[2U] = vector.z;
                append(parameter_bytes, parameter);
            }
            append(operation_bytes,
                   GpuInlineEffectOp{
                       static_cast<core::u32>(
                           scene::EffectKind::LiftGammaGain),
                       parameter_offset, 3U, 0U});
            ++range.count;
            return true;
        };
    const auto emit_contrast_pivot =
        [&](const scene::ContrastPivotParameters& authored) {
            const auto normalized = scene::normalized_contrast_pivot(authored);
            if (!normalized.has_value() || !room_for(1U)) {
                return false;
            }
            const core::u32 parameter_offset = static_cast<core::u32>(
                parameter_bytes.size() / sizeof(GpuInlineEffectParameter));
            GpuInlineEffectParameter parameter{};
            parameter.values[0U] = normalized->contrast;
            parameter.values[1U] = normalized->pivot;
            append(parameter_bytes, parameter);
            append(operation_bytes,
                   GpuInlineEffectOp{
                       static_cast<core::u32>(
                           scene::EffectKind::ContrastPivot),
                       parameter_offset, 1U, 0U});
            ++range.count;
            return true;
        };
    const auto emit_color_matrix =
        [&](const scene::ColorMatrixParameters& authored) {
            const auto normalized = scene::normalized_color_matrix(authored);
            if (!normalized.has_value() || !room_for(3U)) {
                return false;
            }
            const core::u32 parameter_offset = static_cast<core::u32>(
                parameter_bytes.size() / sizeof(GpuInlineEffectParameter));
            const float offsets[]{normalized->offset.x, normalized->offset.y,
                                  normalized->offset.z};
            for (std::size_t row = 0U; row < 3U; ++row) {
                GpuInlineEffectParameter parameter{};
                parameter.values[0U] = normalized->matrix[row * 3U];
                parameter.values[1U] = normalized->matrix[row * 3U + 1U];
                parameter.values[2U] = normalized->matrix[row * 3U + 2U];
                parameter.values[3U] = offsets[row];
                append(parameter_bytes, parameter);
            }
            append(operation_bytes,
                   GpuInlineEffectOp{
                       static_cast<core::u32>(
                           scene::EffectKind::ColorMatrix),
                       parameter_offset, 3U, 0U});
            ++range.count;
            return true;
        };
    const auto emit_white_balance =
        [&](const scene::WhiteBalanceParameters& authored) {
            const auto normalized = scene::normalized_white_balance(authored);
            if (!normalized.has_value() || !room_for(3U)) {
                return false;
            }
            const core::u32 parameter_offset = static_cast<core::u32>(
                parameter_bytes.size() / sizeof(GpuInlineEffectParameter));
            for (std::size_t row = 0U; row < 3U; ++row) {
                GpuInlineEffectParameter parameter{};
                parameter.values[0U] = normalized->matrix[row * 3U];
                parameter.values[1U] = normalized->matrix[row * 3U + 1U];
                parameter.values[2U] = normalized->matrix[row * 3U + 2U];
                append(parameter_bytes, parameter);
            }
            append(operation_bytes,
                   GpuInlineEffectOp{
                       static_cast<core::u32>(
                           scene::EffectKind::WhiteBalance),
                       parameter_offset, 3U, 0U});
            ++range.count;
            return true;
        };
    const auto emit_curves = [&](const scene::CurvesParameters& authored) {
        if (!scene::curves_lut_is_canonical(authored)
            || curve_luts_used >= max_curve_luts
            || !room_for(scene::curve_lut_entries)) {
            return false;
        }
        const core::u32 parameter_offset = static_cast<core::u32>(
            parameter_bytes.size() / sizeof(GpuInlineEffectParameter));
        for (const scene::Vec4 sample : authored.lut) {
            GpuInlineEffectParameter parameter{};
            parameter.values[0U] = sample.x;
            parameter.values[1U] = sample.y;
            parameter.values[2U] = sample.z;
            parameter.values[3U] = sample.w;
            append(parameter_bytes, parameter);
        }
        append(operation_bytes,
               GpuInlineEffectOp{
                   static_cast<core::u32>(scene::EffectKind::Curves),
                   parameter_offset, scene::curve_lut_entries, 0U});
        ++range.count;
        ++curve_luts_used;
        return true;
    };
    const auto emit_film_grain =
        [&](const scene::FilmGrainParameters& authored) {
            const auto normalized = scene::normalized_film_grain(authored);
            if (!normalized.has_value() || !room_for(1U)) {
                return false;
            }
            const core::u32 parameter_offset = static_cast<core::u32>(
                parameter_bytes.size() / sizeof(GpuInlineEffectParameter));
            GpuInlineEffectParameter parameter{};
            parameter.values[0U] = normalized->amount;
            parameter.values[1U] = normalized->pixel_scale;
            parameter.values[2U] = normalized->chroma_ratio;
            parameter.values[3U] =
                normalized->mode == scene::FilmGrainMode::RGB ? 1.0F : 0.0F;
            append(parameter_bytes, parameter);
            append(operation_bytes,
                   GpuInlineEffectOp{
                       static_cast<core::u32>(scene::EffectKind::FilmGrain),
                       parameter_offset, 1U, normalized->seed});
            ++range.count;
            return true;
        };

    if (fill.has_value()) {
        if (!room_for(4U)) {
            rollback();
            return false;
        }
        const core::u32 parameter_offset = static_cast<core::u32>(
            parameter_bytes.size() / sizeof(GpuInlineEffectParameter));
        for (const GpuInlineEffectParameter& parameter : fill->parameters) {
            append(parameter_bytes, parameter);
        }
        append(operation_bytes,
               GpuInlineEffectOp{surface_fill_inline_kind, parameter_offset,
                                 4U, 0U});
        ++range.count;
    }

    if (effects.inline_operations.empty()) {
        // Hand-built/legacy Scene values already carry response-shaped scalar
        // values. They form the historical Brightness-then-Saturation program.
        if (effects.brightness_enabled && std::isfinite(effects.brightness)
            && effects.brightness != 1.0F
            && !emit_scalar(scene::EffectKind::Brightness,
                            std::max(effects.brightness, 0.0F))) {
            rollback();
            return false;
        }
        const float saturation =
            scene::normalized_saturation(effects.saturation);
        if (effects.saturation_enabled && saturation != 1.0F
            && !emit_scalar(scene::EffectKind::Saturation, saturation)) {
            rollback();
            return false;
        }
        return true;
    }

    double brightness_product = 1.0;
    double saturation_product = 1.0;
    bool brightness_seen{};
    bool saturation_seen{};
    const auto flush_segment = [&]() {
        if (brightness_seen) {
            const float brightness = scene::brightness_from_product(
                brightness_product, settings.brightness_gamma);
            if (brightness != 1.0F
                && !emit_scalar(scene::EffectKind::Brightness, brightness)) {
                return false;
            }
        }
        if (saturation_seen) {
            const float saturation =
                scene::saturation_from_product(saturation_product);
            if (saturation != 1.0F
                && !emit_scalar(scene::EffectKind::Saturation, saturation)) {
                return false;
            }
        }
        brightness_product = 1.0;
        saturation_product = 1.0;
        brightness_seen = false;
        saturation_seen = false;
        return true;
    };

    for (const scene::InlineEffectOperation& operation :
         effects.inline_operations) {
        switch (operation.kind) {
        case scene::EffectKind::Brightness:
            brightness_seen = true;
            brightness_product = scene::composed_brightness_product(
                brightness_product, static_cast<double>(operation.value));
            break;
        case scene::EffectKind::Saturation:
            saturation_seen = true;
            saturation_product = scene::composed_saturation_product(
                saturation_product, static_cast<double>(operation.value));
            break;
        case scene::EffectKind::Levels:
            if (!flush_segment()) {
                rollback();
                return false;
            }
            if (!scene::levels_is_identity(operation.levels)
                && !emit_levels(operation.levels)) {
                rollback();
                return false;
            }
            break;
        case scene::EffectKind::LiftGammaGain:
            if (!flush_segment()) {
                rollback();
                return false;
            }
            if (!scene::lift_gamma_gain_is_identity(
                    operation.lift_gamma_gain)
                && !emit_lift_gamma_gain(operation.lift_gamma_gain)) {
                rollback();
                return false;
            }
            break;
        case scene::EffectKind::ContrastPivot:
            if (!flush_segment()) {
                rollback();
                return false;
            }
            if (!scene::contrast_pivot_is_identity(
                    operation.contrast_pivot)
                && !emit_contrast_pivot(operation.contrast_pivot)) {
                rollback();
                return false;
            }
            break;
        case scene::EffectKind::ColorMatrix:
            if (!flush_segment()) {
                rollback();
                return false;
            }
            if (!scene::color_matrix_is_identity(operation.color_matrix)
                && !emit_color_matrix(operation.color_matrix)) {
                rollback();
                return false;
            }
            break;
        case scene::EffectKind::WhiteBalance:
            if (!flush_segment()) {
                rollback();
                return false;
            }
            if (!scene::white_balance_is_identity(operation.white_balance)
                && !emit_white_balance(operation.white_balance)) {
                rollback();
                return false;
            }
            break;
        case scene::EffectKind::Curves:
            if (!flush_segment()) {
                rollback();
                return false;
            }
            if (!scene::curves_is_identity(operation.curves)
                && !emit_curves(operation.curves)) {
                rollback();
                return false;
            }
            break;
        case scene::EffectKind::FilmGrain:
            if (!flush_segment()) {
                rollback();
                return false;
            }
            if (!scene::film_grain_is_identity(operation.film_grain)
                && !emit_film_grain(operation.film_grain)) {
                rollback();
                return false;
            }
            break;
        case scene::EffectKind::Blur:
        case scene::EffectKind::BoxBlur:
        case scene::EffectKind::DirectionalBlur:
        case scene::EffectKind::ChromaKey:
        case scene::EffectKind::LumaKey:
        case scene::EffectKind::Count:
            rollback();
            return false;
        }
    }
    if (!flush_segment()) {
        rollback();
        return false;
    }
    return true;
}

[[nodiscard]] scene::BlendMode valid_blend(scene::BlendMode blend) noexcept {
    switch (blend) {
    case scene::BlendMode::Normal:
    case scene::BlendMode::Add:
    case scene::BlendMode::Multiply:
    case scene::BlendMode::Screen:
        return blend;
    }
    return scene::BlendMode::Normal;
}

[[nodiscard]] float opacity(const scene::LayerStyle& style) noexcept {
    return std::isfinite(style.opacity)
        ? std::clamp(style.opacity, 0.0F, 1.0F)
        : 1.0F;
}

[[nodiscard]] bool needs_compositing(const scene::LayerStyle& style) noexcept {
    return opacity(style) < 1.0F
        || (std::isfinite(style.horizontal_blend) &&
            style.horizontal_blend > 0.0F)
        || valid_blend(style.blend) != scene::BlendMode::Normal;
}

[[nodiscard]] std::size_t blend_index(scene::BlendMode blend) noexcept {
    return static_cast<std::size_t>(
        static_cast<core::u8>(valid_blend(blend)));
}

[[nodiscard]] rhi::ColorBlend to_rhi_blend(
    scene::BlendMode blend,
    bool premultiplied) noexcept {
    switch (valid_blend(blend)) {
    case scene::BlendMode::Add:
        return premultiplied ? rhi::ColorBlend::PremultipliedAdd
                             : rhi::ColorBlend::Add;
    case scene::BlendMode::Multiply:
        return rhi::ColorBlend::Multiply;
    case scene::BlendMode::Screen:
        return rhi::ColorBlend::Screen;
    case scene::BlendMode::Normal:
        break;
    }
    return premultiplied ? rhi::ColorBlend::PremultipliedNormal
                         : rhi::ColorBlend::Normal;
}

[[nodiscard]] float camera_depth(
    const scene::Mat4& world,
    scene::Vec3 local_center,
    scene::Vec3 eye,
    scene::Vec3 forward) noexcept {
    forward = scene::normalize(forward, scene::Vec3{0.0F, 0.0F, -1.0F});
    const float result = scene::dot(
        scene::transform_point(world, local_center) - eye, forward);
    return std::isfinite(result) ? result : 0.0F;
}

[[nodiscard]] ModelBounds primitive_bounds(scene::Primitive primitive) noexcept {
    switch (primitive) {
    case scene::Primitive::Quad:
        return ModelBounds{scene::Vec3{-0.5F, -0.5F, 0.0F},
                           scene::Vec3{0.5F, 0.5F, 0.0F}};
    case scene::Primitive::Plane:
        return ModelBounds{scene::Vec3{-0.5F, 0.0F, -0.5F},
                           scene::Vec3{0.5F, 0.0F, 0.5F}};
    case scene::Primitive::Box:
    case scene::Primitive::Sphere:
    case scene::Primitive::Cylinder:
        break;
    }
    return unit_bounds;
}

[[nodiscard]] float component(scene::Vec3 value, std::size_t axis) noexcept {
    return axis == 0U ? value.x : axis == 1U ? value.y : value.z;
}

[[nodiscard]] bool finite(scene::Vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y)
        && std::isfinite(value.z);
}

[[nodiscard]] bool finite(const scene::Mat4& value) noexcept {
    return std::all_of(value.m.begin(), value.m.end(), [](float element) {
        return std::isfinite(element);
    });
}

void expand_directional_shadow_bounds(
    DirectionalShadowBounds& bounds,
    const scene::Mat4& world,
    scene::Vec3 local_minimum,
    scene::Vec3 local_maximum) noexcept {
    scene::Vec3 candidate_minimum{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    scene::Vec3 candidate_maximum{
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()};
    for (core::u32 corner = 0U; corner < 8U; ++corner) {
        const scene::Vec3 local{
            (corner & 1U) != 0U ? local_maximum.x : local_minimum.x,
            (corner & 2U) != 0U ? local_maximum.y : local_minimum.y,
            (corner & 4U) != 0U ? local_maximum.z : local_minimum.z};
        const scene::Vec3 point = scene::transform_point(world, local);
        if (!finite(point)) {
            return;
        }
        candidate_minimum.x = std::min(candidate_minimum.x, point.x);
        candidate_minimum.y = std::min(candidate_minimum.y, point.y);
        candidate_minimum.z = std::min(candidate_minimum.z, point.z);
        candidate_maximum.x = std::max(candidate_maximum.x, point.x);
        candidate_maximum.y = std::max(candidate_maximum.y, point.y);
        candidate_maximum.z = std::max(candidate_maximum.z, point.z);
    }
    bounds.minimum.x = std::min(bounds.minimum.x, candidate_minimum.x);
    bounds.minimum.y = std::min(bounds.minimum.y, candidate_minimum.y);
    bounds.minimum.z = std::min(bounds.minimum.z, candidate_minimum.z);
    bounds.maximum.x = std::max(bounds.maximum.x, candidate_maximum.x);
    bounds.maximum.y = std::max(bounds.maximum.y, candidate_maximum.y);
    bounds.maximum.z = std::max(bounds.maximum.z, candidate_maximum.z);
    ++bounds.casters;
}

[[nodiscard]] bool directional_shadow_matrix(
    const DirectionalShadowBounds& bounds,
    const scene::DirectionalLight& light,
    core::u32 resolution,
    scene::Mat4& into,
    scene::Mat4& inverse_into,
    float& texel_depth) noexcept {
    if (bounds.casters == 0U || resolution == 0U
        || !finite(bounds.minimum) || !finite(bounds.maximum)) {
        return false;
    }
    const scene::Vec3 center =
        (bounds.minimum + bounds.maximum) * 0.5F;
    const scene::Vec3 half =
        (bounds.maximum - bounds.minimum) * 0.5F;
    const float radius = std::max(scene::length(half), 0.5F);
    const scene::Vec3 direction = scene::normalize(
        light.direction, scene::Vec3{0.0F, -1.0F, 0.0F});
    const scene::Vec3 up = std::abs(scene::dot(
        direction, scene::Vec3{0.0F, 1.0F, 0.0F})) > 0.95F
        ? scene::Vec3{0.0F, 0.0F, 1.0F}
        : scene::Vec3{0.0F, 1.0F, 0.0F};
    const scene::Vec3 eye = center - direction * (radius * 2.0F + 1.0F);
    const scene::Mat4 view = scene::look_at(eye, center, up);

    scene::Vec3 light_min{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    scene::Vec3 light_max{
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()};
    for (core::u32 corner = 0U; corner < 8U; ++corner) {
        const scene::Vec3 world{
            (corner & 1U) != 0U ? bounds.maximum.x : bounds.minimum.x,
            (corner & 2U) != 0U ? bounds.maximum.y : bounds.minimum.y,
            (corner & 4U) != 0U ? bounds.maximum.z : bounds.minimum.z};
        const scene::Vec3 point = scene::transform_point(view, world);
        light_min.x = std::min(light_min.x, point.x);
        light_min.y = std::min(light_min.y, point.y);
        light_min.z = std::min(light_min.z, point.z);
        light_max.x = std::max(light_max.x, point.x);
        light_max.y = std::max(light_max.y, point.y);
        light_max.z = std::max(light_max.z, point.z);
    }

    const float margin = std::max(radius * 0.05F, 0.05F);
    float left = light_min.x - margin;
    float right = light_max.x + margin;
    float bottom = light_min.y - margin;
    float top = light_max.y + margin;
    const float width = right - left;
    const float height = top - bottom;
    if (!(width > 0.0F) || !(height > 0.0F)) {
        return false;
    }

    // Snap the full-scene projection to shadow texels. Camera and output-slice
    // motion therefore cannot make adjacent nodes choose different shadow
    // samples for the same authored world point.
    const float texel_x = width / static_cast<float>(resolution);
    const float texel_y = height / static_cast<float>(resolution);
    const float center_x = (left + right) * 0.5F;
    const float center_y = (bottom + top) * 0.5F;
    const float snapped_x = std::floor(center_x / texel_x + 0.5F) * texel_x;
    const float snapped_y = std::floor(center_y / texel_y + 0.5F) * texel_y;
    left += snapped_x - center_x;
    right += snapped_x - center_x;
    bottom += snapped_y - center_y;
    top += snapped_y - center_y;

    const float near_plane = std::max(0.01F, -light_max.z - margin);
    const float far_plane = std::max(near_plane + 0.01F, -light_min.z + margin);
    const float depth_span = far_plane - near_plane;
    scene::Mat4 projection = scene::identity();
    projection.set(0U, 0U, 2.0F / (right - left));
    projection.set(0U, 3U, -(right + left) / (right - left));
    projection.set(1U, 1U, 2.0F / (top - bottom));
    projection.set(1U, 3U, -(top + bottom) / (top - bottom));
    // Reversed [0,1] depth for a right-handed view whose forward axis is -Z.
    projection.set(2U, 2U, 1.0F / depth_span);
    projection.set(2U, 3U, far_plane / depth_span);
    into = scene::multiply(projection, view);

    scene::Mat4 inverse_projection = scene::identity();
    inverse_projection.set(0U, 0U, (right - left) * 0.5F);
    inverse_projection.set(0U, 3U, (right + left) * 0.5F);
    inverse_projection.set(1U, 1U, (top - bottom) * 0.5F);
    inverse_projection.set(1U, 3U, (top + bottom) * 0.5F);
    inverse_projection.set(2U, 2U, depth_span);
    inverse_projection.set(2U, 3U, -far_plane);
    inverse_into = scene::multiply(
        scene::invert_affine(view), inverse_projection);
    texel_depth = std::max(texel_x, texel_y) / depth_span;
    return finite(into) && finite(inverse_into)
        && std::isfinite(texel_depth);
}

[[nodiscard]] float cascade_split(
    float near_plane, float far_plane, core::u32 index,
    core::u32 count) noexcept {
    if (count == 0U) {
        return far_plane;
    }
    const float ratio =
        static_cast<float>(index) / static_cast<float>(count);
    const float logarithmic =
        near_plane * std::pow(far_plane / near_plane, ratio);
    const float uniform = near_plane + (far_plane - near_plane) * ratio;
    constexpr float lambda = 0.7F;
    return lambda * logarithmic + (1.0F - lambda) * uniform;
}

[[nodiscard]] float cascade_far_distance(
    const scene::Camera& camera,
    const DirectionalShadowBounds& bounds,
    float near_distance,
    float far_plane) noexcept {
    if (bounds.casters == 0U) {
        return std::max(far_plane, near_distance * 2.0F);
    }
    (void)camera;
    const float reach = std::max(near_distance * 2.0F,
        scene::length(bounds.maximum - bounds.minimum) * 2.0F);
    return std::clamp(reach, near_distance * 2.0F,
                      std::max(far_plane, near_distance * 2.0F));
}

[[nodiscard]] bool camera_cascade_volume(
    const scene::Camera& camera,
    const scene::FrustumWindow& window,
    float near_distance,
    float far_distance,
    CascadeVolume& into) noexcept {
    if (!(window.near_plane > 0.0F) || !(far_distance > near_distance)) {
        return false;
    }
    const scene::Vec3 forward =
        scene::normalize(camera.target - camera.eye);
    const scene::Vec3 right =
        scene::normalize(scene::cross(forward, camera.up));
    const scene::Vec3 up = scene::cross(right, forward);
    if (!finite(forward) || !finite(right) || !finite(up)) {
        return false;
    }
    std::array<scene::Vec3, 8U> corners{};
    core::u32 written{};
    for (const float distance : {near_distance, far_distance}) {
        const float scale = distance / window.near_plane;
        for (const float x : {window.left * scale, window.right * scale}) {
            for (const float y :
                 {window.bottom * scale, window.top * scale}) {
                corners[written++] = camera.eye + forward * distance
                    + right * x + up * y;
            }
        }
    }
    scene::Vec3 center{};
    for (const scene::Vec3& corner : corners) {
        if (!finite(corner)) {
            return false;
        }
        center = center + corner;
    }
    center = center * 0.125F;
    float radius{};
    for (const scene::Vec3& corner : corners) {
        radius = std::max(radius, scene::length(corner - center));
    }
    if (!finite(center) || !std::isfinite(radius) || !(radius > 0.0F)) {
        return false;
    }
    into.center = center;
    into.radius = std::ceil(radius * 16.0F) / 16.0F;
    return true;
}

[[nodiscard]] bool cascade_shadow_matrix(
    const DirectionalShadowBounds& bounds,
    const CascadeVolume& volume,
    const scene::DirectionalLight& light,
    core::u32 resolution,
    scene::Mat4& into,
    float& texel_depth) noexcept {
    if (bounds.casters == 0U || resolution == 0U || !finite(volume.center)
        || !std::isfinite(volume.radius) || !(volume.radius > 0.0F)) {
        return false;
    }
    const scene::Vec3 direction = scene::normalize(
        light.direction, scene::Vec3{0.0F, -1.0F, 0.0F});
    const scene::Vec3 up = std::abs(scene::dot(
        direction, scene::Vec3{0.0F, 1.0F, 0.0F})) > 0.95F
        ? scene::Vec3{0.0F, 0.0F, 1.0F}
        : scene::Vec3{0.0F, 1.0F, 0.0F};
    const scene::Vec3 scene_center = (bounds.minimum + bounds.maximum) * 0.5F;
    const float scene_radius = std::max(scene::length(bounds.maximum - bounds.minimum), 1.0F);
    const scene::Vec3 eye = scene_center - direction * (scene_radius * 2.0F + 1.0F);
    const scene::Mat4 view = scene::look_at(eye, scene_center, up);
    const scene::Vec3 light_center =
        scene::transform_point(view, volume.center);
    if (!finite(light_center)) {
        return false;
    }

    const float texel =
        (volume.radius * 2.0F) / static_cast<float>(resolution);
    if (!(texel > 0.0F)) {
        return false;
    }
    const float snapped_x =
        std::floor(light_center.x / texel + 0.5F) * texel;
    const float snapped_y =
        std::floor(light_center.y / texel + 0.5F) * texel;
    const float left = snapped_x - volume.radius;
    const float right = snapped_x + volume.radius;
    const float bottom = snapped_y - volume.radius;
    const float top = snapped_y + volume.radius;

    float scene_min_z = std::numeric_limits<float>::max();
    float scene_max_z = std::numeric_limits<float>::lowest();
    for (core::u32 corner = 0U; corner < 8U; ++corner) {
        const scene::Vec3 world{
            (corner & 1U) != 0U ? bounds.maximum.x : bounds.minimum.x,
            (corner & 2U) != 0U ? bounds.maximum.y : bounds.minimum.y,
            (corner & 4U) != 0U ? bounds.maximum.z : bounds.minimum.z};
        const scene::Vec3 point = scene::transform_point(view, world);
        if (!finite(point)) {
            return false;
        }
        scene_min_z = std::min(scene_min_z, point.z);
        scene_max_z = std::max(scene_max_z, point.z);
    }
    // View space looks down -Z, so the largest z is the closest to the light.
    const float near_z =
        scene_max_z;
    const float far_z = scene_min_z;
    const float margin = std::max(scene_radius * 0.05F, 0.05F);
    const float near_plane = std::max(0.01F, -near_z - margin);
    const float far_plane = std::max(near_plane + 0.01F, -far_z + margin);
    const float depth_span = far_plane - near_plane;

    scene::Mat4 projection = scene::identity();
    projection.set(0U, 0U, 2.0F / (right - left));
    projection.set(0U, 3U, -(right + left) / (right - left));
    projection.set(1U, 1U, 2.0F / (top - bottom));
    projection.set(1U, 3U, -(top + bottom) / (top - bottom));
    projection.set(2U, 2U, 1.0F / depth_span);
    projection.set(2U, 3U, far_plane / depth_span);
    into = scene::multiply(projection, view);
    texel_depth = texel / depth_span;
    return finite(into) && std::isfinite(texel_depth);
}

[[nodiscard]] bool media_quad_outside_view(
    const scene::Mat4& world,
    const scene::Mat4& view_projection,
    const scene::EffectStack& effects,
    const scene::EffectSettings& effect_settings) noexcept {
    const scene::EffectBoundsExpansion expansion =
        effect_stack_bounds(effects, effect_settings);
    if (expansion.x > 0.0F || expansion.y > 0.0F) {
        // A blur can contribute pixels beyond the geometric quad. Until its
        // expanded bounds are projected, decoding it is the conservative
        // choice.
        return false;
    }

    constexpr std::array<scene::Vec4, 4U> corners{{
        {-0.5F, -0.5F, 0.0F, 1.0F},
        {0.5F, -0.5F, 0.0F, 1.0F},
        {-0.5F, 0.5F, 0.0F, 1.0F},
        {0.5F, 0.5F, 0.0F, 1.0F},
    }};
    std::array<scene::Vec4, corners.size()> clip{};
    const scene::Mat4 clip_from_local =
        scene::multiply(view_projection, world);
    for (std::size_t index = 0U; index < corners.size(); ++index) {
        clip[index] = scene::transform(clip_from_local, corners[index]);
        const scene::Vec4 point = clip[index];
        if (!std::isfinite(point.x) || !std::isfinite(point.y)
            || !std::isfinite(point.z) || !std::isfinite(point.w)) {
            return false;
        }
    }

    const auto all_outside = [&clip](auto&& predicate) noexcept {
        return std::all_of(clip.begin(), clip.end(), predicate);
    };
    // Direct3D clip space is -w..w in X/Y and 0..w in Z. This renderer uses
    // reversed depth, but near and far still occupy those same two Z planes.
    return all_outside([](scene::Vec4 p) { return p.x < -p.w; })
        || all_outside([](scene::Vec4 p) { return p.x > p.w; })
        || all_outside([](scene::Vec4 p) { return p.y < -p.w; })
        || all_outside([](scene::Vec4 p) { return p.y > p.w; })
        || all_outside([](scene::Vec4 p) { return p.z < 0.0F; })
        || all_outside([](scene::Vec4 p) { return p.z > p.w; });
}

[[nodiscard]] bool outside_shadow_projection(
    const scene::Mat4& view_projection,
    const scene::Mat4& world,
    const ModelBounds& bounds) noexcept {
    const scene::Mat4 clip_from_local = scene::multiply(view_projection, world);
    bool left = true, right = true, below = true, above = true;
    for (unsigned corner = 0U; corner < 8U; ++corner) {
        const scene::Vec4 p = scene::transform(clip_from_local, scene::Vec4{
            (corner & 1U) != 0U ? bounds.maximum.x : bounds.minimum.x,
            (corner & 2U) != 0U ? bounds.maximum.y : bounds.minimum.y,
            (corner & 4U) != 0U ? bounds.maximum.z : bounds.minimum.z, 1.0F});
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.w)) {
            return false;
        }
        const float slack = 1.0e-3F * std::abs(p.w) + 1.0e-5F;
        left = left && p.x < -p.w - slack;
        right = right && p.x > p.w + slack;
        below = below && p.y < -p.w - slack;
        above = above && p.y > p.w + slack;
    }
    return left || right || below || above;
}

[[nodiscard]] core::u64 digest_bytes(core::u64 digest, std::span<const core::u8> bytes) noexcept {
    std::size_t offset = 0U;
    for (; offset + sizeof(core::u64) <= bytes.size(); offset += sizeof(core::u64)) {
        core::u64 word{};
        std::memcpy(&word, bytes.data() + offset, sizeof(word));
        digest = mix_digest(digest, word);
    }
    core::u64 tail{};
    if (offset < bytes.size()) {
        std::memcpy(&tail, bytes.data() + offset, bytes.size() - offset);
    }
    return mix_digest(digest, tail ^ (static_cast<core::u64>(bytes.size()) << 56U));
}

[[nodiscard]] core::u64 unmatched_digest() noexcept {
    static std::atomic<core::u64> next{1U};
    return mix_digest(0x554E4D41544348ULL, next.fetch_add(1U, std::memory_order_relaxed));
}

[[nodiscard]] bool resolve_render_view(
    const scene::Scene& scene, scene::CanvasRect slice,
    const SceneRenderOptions& options, SceneRenderView& out) noexcept {
    if (!options.view.has_value()) {
        if (!scene::contains(scene.canvas, slice)) {
            return false;
        }
        // The window is rebuilt around the offset rather than the matrix
        // shifted afterwards, because the conversion to projection units then
        // divides by the canvas extent every node agrees on. Two adjacent
        // slices add the same number to the edge they share and it stays
        // bit-identical.
        out.matrices = scene::view_for_slice(scene.camera, scene.canvas, slice,
                                             options.jitter);
        // The whole canvas under the same camera and the same jitter. Every
        // node derives this identically because nothing about the slice enters
        // it, which is exactly why motion is expressed against it.
        out.canvas_view_projection =
            scene::view_for_slice(scene.camera, scene.canvas,
                                  scene::whole(scene.canvas), options.jitter)
                .view_projection;
        out.eye = scene.camera.eye;
        out.forward = scene::normalize(scene.camera.target - scene.camera.eye,
                                       scene::Vec3{0.0F, 0.0F, -1.0F});
        return true;
    }
    out = *options.view;
    if (!finite(out.eye) || !finite(out.forward)
        || !(scene::length(out.forward) > 1.0e-20F)
        || !finite(out.matrices.view) || !finite(out.matrices.projection)
        || !finite(out.matrices.view_projection)) {
        return false;
    }
    // An explicit view has already resolved its window, so the offset is
    // applied to the matrix in that view's own target pixels. The two forms
    // are asserted equivalent in the scene math suite.
    out.matrices = scene::jittered(out.matrices, options.jitter, slice.width,
                                   slice.height);
    // An explicit view has already resolved its own window, so there is no
    // wider canvas to project against and the two are the same matrix.
    if (!options.full_canvas_view) out.canvas_view_projection = out.matrices.view_projection;
    out.forward = scene::normalize(out.forward);
    return true;
}


} // namespace mediaserver::render::scene_detail
