#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/render/effect_execution.hpp>

#include <mediaserver/render/model_loader.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/scene_frag_dxil.h>
#include <mediaserver/shaders/scene_media_frag_dxil.h>
#include <mediaserver/shaders/scene_vert_dxil.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <utility>

namespace mediaserver::render {
namespace {

using Result = core::Result<SceneRenderer, RenderError>;

// ---------------------------------------------------------------------------
// The GPU-side records. Every one of these is described twice - here and in
// the HLSL - and a mismatch would be a silently wrong picture rather than a
// compile error, so each carries a size assertion and each field comment names
// its counterpart.

// scene.vert.hlsl: struct Vertex.
static_assert(sizeof(scene::MeshVertex) == 32U);

// scene.vert.hlsl: struct Instance. Matrices are stored as explicit columns on
// both sides, so nothing here transposes anything.
struct GpuInstance final {
    float world[16U]{};  // four columns of the object-to-world transform
    float normal[12U]{}; // three columns of the inverse-transpose basis
    float base_color[3U]{};
    float roughness{};
    float emissive[3U]{};
    float metallic{};
    // opacity, horizontal edge blend, BlendMode, depth-only flag
    float layer_style[4U]{};
    // first inline operation, operation count, DecodedPixelLayout, reserved
    core::u32 effect_range[4U]{};
};

static_assert(sizeof(GpuInstance) == 176U);
static_assert(alignof(GpuInstance) == 4U);
static_assert(static_cast<core::u8>(scene::BlendMode::Normal) == 0U);
static_assert(static_cast<core::u8>(scene::BlendMode::Add) == 1U);
static_assert(static_cast<core::u8>(scene::BlendMode::Multiply) == 2U);
static_assert(static_cast<core::u8>(scene::BlendMode::Screen) == 3U);

// scene.frag.hlsl / scene_media.frag.hlsl: an operation selects a typed slice
// of the separate float4 parameter buffer. Keeping parameters out of the
// instance record is what lets Levels carry all twenty values without fixed
// per-instance lanes.
struct GpuInlineEffectOp final {
    core::u32 kind{};
    core::u32 parameter_offset{};
    core::u32 parameter_count{};
    core::u32 reserved{};
};

struct GpuInlineEffectParameter final {
    float values[4U]{};
};

static_assert(sizeof(GpuInlineEffectOp) == 16U);
static_assert(sizeof(GpuInlineEffectParameter) == 16U);
static_assert(static_cast<core::u8>(scene::EffectKind::Brightness) == 0U);
static_assert(static_cast<core::u8>(scene::EffectKind::Saturation) == 1U);
static_assert(static_cast<core::u8>(scene::EffectKind::Levels) == 3U);
static_assert(
    static_cast<core::u8>(scene::EffectKind::LiftGammaGain) == 4U);
static_assert(
    static_cast<core::u8>(scene::EffectKind::ContrastPivot) == 5U);
static_assert(
    static_cast<core::u8>(scene::EffectKind::ColorMatrix) == 6U);
static_assert(
    static_cast<core::u8>(scene::EffectKind::WhiteBalance) == 7U);
static_assert(static_cast<core::u8>(scene::EffectKind::Curves) == 8U);
static_assert(static_cast<core::u8>(scene::EffectKind::FilmGrain) == 9U);

// scene.frag.hlsl: struct Light.
struct GpuLight final {
    float vector_kind[4U]{}; // xyz direction or position, w selects which
    float color_intensity[4U]{};
    float params[4U]{}; // x: range
};

static_assert(sizeof(GpuLight) == 48U);

// scene.vert.hlsl: cbuffer ViewConstants.
struct ViewConstants final {
    float view_projection[16U]{};
};

// scene.vert.hlsl: cbuffer BatchConstants.
struct BatchConstants final {
    core::u32 first_index{};
    core::u32 first_instance{};
    core::u32 reserved0{};
    core::u32 reserved1{};
};

static_assert(sizeof(BatchConstants) == 16U);

// scene.frag.hlsl: cbuffer SceneConstants.
struct SceneConstants final {
    float eye_position[4U]{};
    float sky_ambient[4U]{};
    float ground_exposure[4U]{};
    core::u32 counts[4U]{};
    // slice origin x/y, exact show frame low/high u32
    core::u32 grain_context[4U]{};
    // slice width/height, render-target width/height
    core::u32 grain_extent[4U]{};
};

static_assert(sizeof(SceneConstants) == 96U);

// ---------------------------------------------------------------------------

struct ShaderBlobs final {
    std::span<const core::u8> vertex;
    std::span<const core::u8> fragment;
    std::span<const core::u8> media_fragment;
    rhi::ShaderFormat format{rhi::ShaderFormat::Dxil};
    bool present{};
};

[[nodiscard]] ShaderBlobs shader_blobs() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return ShaderBlobs{
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_scene_vert_dxil),
            sizeof(mediaserver_scene_vert_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_scene_frag_dxil),
            sizeof(mediaserver_scene_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_scene_media_frag_dxil),
            sizeof(mediaserver_scene_media_frag_dxil)},
        rhi::ShaderFormat::Dxil,
        true,
    };
#else
    // No shader compiler was available when this was built, so there is no 3D
    // path to offer. Saying so plainly is the whole point of the flag.
    return ShaderBlobs{};
#endif
}

template <typename T>
void append(std::vector<core::u8>& into, const T& value) {
    const auto offset = into.size();
    into.resize(offset + sizeof(T));
    std::memcpy(into.data() + offset, &value, sizeof(T));
}

template <typename T>
[[nodiscard]] std::span<const core::u8> bytes_of(const T& value) noexcept {
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(&value), sizeof(T)};
}

[[nodiscard]] rhi::TextureDesc color_desc(core::u32 width, core::u32 height) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
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

[[nodiscard]] rhi::TextureDesc depth_desc(core::u32 width, core::u32 height) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Depth32Float;
    desc.depth_target = true;
    return desc;
}

[[nodiscard]] GpuInstance to_gpu(const scene::MeshInstance& instance,
                                 core::u32 first_effect,
                                 core::u32 effect_count,
                                 bool depth_only = false,
                                 DecodedPixelLayout media_layout =
                                     DecodedPixelLayout::Rgba8) noexcept {
    GpuInstance record{};
    for (core::u32 index = 0U; index < 16U; ++index) {
        record.world[index] = instance.world.m[index];
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
    record.emissive[0U] = instance.material.emissive.x;
    record.emissive[1U] = instance.material.emissive.y;
    record.emissive[2U] = instance.material.emissive.z;
    record.metallic = instance.material.metallic;
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
    // Blur is intentionally absent. It needs an offscreen source and a
    // separable compositor pass; treating a radius as a pointwise colour tweak
    // would claim an effect the renderer did not perform.
    return record;
}

struct CompiledInlineRange final {
    core::u32 first{};
    core::u32 count{};
};

[[nodiscard]] bool compile_inline_program(
    const scene::EffectStack& effects,
    const scene::EffectSettings& settings,
    core::u32 max_operations,
    core::u32 max_parameters,
    core::u32 max_curve_luts,
    core::u32& curve_luts_used,
    std::vector<core::u8>& operation_bytes,
    std::vector<core::u8>& parameter_bytes,
    CompiledInlineRange& range) {
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

[[nodiscard]] rhi::ColorBlend to_rhi_blend(scene::BlendMode blend) noexcept {
    switch (valid_blend(blend)) {
    case scene::BlendMode::Add:
        return rhi::ColorBlend::Add;
    case scene::BlendMode::Multiply:
        return rhi::ColorBlend::Multiply;
    case scene::BlendMode::Screen:
        return rhi::ColorBlend::Screen;
    case scene::BlendMode::Normal:
        break;
    }
    return rhi::ColorBlend::Normal;
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

[[nodiscard]] bool finite(scene::Vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y)
        && std::isfinite(value.z);
}

[[nodiscard]] bool finite(const scene::Mat4& value) noexcept {
    return std::all_of(value.m.begin(), value.m.end(), [](float element) {
        return std::isfinite(element);
    });
}

// A media surface is the unit XY quad generated by Primitive::Quad. Test its
// four corners in homogeneous clip space so quads crossing the eye or a
// frustum plane remain visible. This only returns true after one complete
// separating plane is proven; malformed transforms therefore fail open.
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

} // namespace

bool SceneRenderer::shaders_available() noexcept {
    return shader_blobs().present;
}

SceneRenderer::SceneRenderer(
    rhi::Device& device,
    RendererLimits limits,
    rhi::Texture color,
    rhi::Texture depth,
    rhi::GraphicsPipeline pipeline,
    std::array<rhi::GraphicsPipeline, 4U> blended_pipelines,
    std::array<rhi::GraphicsPipeline, 4U> media_blended_pipelines,
    rhi::GraphicsPipeline media_depth_pipeline,
    rhi::Sampler media_sampler,
    rhi::Texture material_fallback,
    rhi::StorageBuffer vertices,
    rhi::StorageBuffer indices,
    rhi::StorageBuffer instances,
    rhi::StorageBuffer lights,
    rhi::StorageBuffer inline_effect_ops,
    rhi::StorageBuffer inline_effect_parameters) noexcept
    : device_(&device),
      limits_(limits),
      color_(std::move(color)),
      depth_(std::move(depth)),
      pipeline_(std::move(pipeline)),
      blended_pipelines_(std::move(blended_pipelines)),
      media_blended_pipelines_(std::move(media_blended_pipelines)),
      media_depth_pipeline_(std::move(media_depth_pipeline)),
      media_sampler_(std::move(media_sampler)),
      material_fallback_(std::move(material_fallback)),
      vertex_buffer_(std::move(vertices)),
      index_buffer_(std::move(indices)),
      instance_buffer_(std::move(instances)),
      light_buffer_(std::move(lights)),
      inline_effect_op_buffer_(std::move(inline_effect_ops)),
      inline_effect_parameter_buffer_(std::move(inline_effect_parameters)) {}

Result SceneRenderer::create(
    rhi::Device& device,
    core::u32 width,
    core::u32 height,
    const RendererLimits& limits) {
    const auto blobs = shader_blobs();
    if (!blobs.present) {
        return Result::failure(RenderError::ShaderFormatUnsupported);
    }
    if (width == 0U || height == 0U || limits.max_instances == 0U
        || limits.max_vertices == 0U || limits.max_indices == 0U
        || limits.max_lights == 0U || limits.max_inline_effect_ops == 0U
        || limits.max_inline_curve_luts == 0U
        || limits.max_inline_effect_parameter_vectors == 0U) {
        return Result::failure(RenderError::InvalidConfiguration);
    }
    if (!device.supports(blobs.format)) {
        return Result::failure(RenderError::ShaderFormatUnsupported);
    }

    auto color = device.create_texture(color_desc(width, height));
    if (!color) {
        return Result::failure(RenderError::AllocationFailed);
    }
    auto depth = device.create_texture(depth_desc(width, height));
    if (!depth) {
        return Result::failure(RenderError::AllocationFailed);
    }

    rhi::ShaderDesc vertex{};
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.format = blobs.format;
    vertex.bytecode = blobs.vertex;
    // Vertices, indices, and instances; two uniform slots, the view and the
    // per-batch offsets.
    vertex.storage_buffer_count = 3U;
    vertex.uniform_buffer_count = 2U;

    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = blobs.format;
    fragment.bytecode = blobs.fragment;
    fragment.sampler_count = 3U;
    fragment.storage_buffer_count = 3U;
    fragment.uniform_buffer_count = 1U;

    rhi::DepthState depth_state{};
    depth_state.enabled = true;
    depth_state.write = true;
    // Reversed depth: the scene layer's projection puts the near plane at one.
    depth_state.greater = true;

    auto pipeline = device.create_pipeline(
        vertex,
        fragment,
        rhi::TextureFormat::Rgba8Unorm,
        rhi::ColorBlend::Disabled,
        depth_state);
    if (!pipeline) {
        return Result::failure(
            pipeline.error() == rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }

    // Media uses the same pulled vertex stage and binds two source slots. RGBA
    // clips use the first and ignore the fallback second slot; P010 clips bind
    // luma and chroma directly and convert in this fragment stage. It always
    // goes through a compositing pipeline because source pixels may have alpha.
    rhi::ShaderDesc media_fragment{};
    media_fragment.stage = rhi::ShaderStage::Fragment;
    media_fragment.format = blobs.format;
    media_fragment.bytecode = blobs.media_fragment;
    media_fragment.sampler_count = 2U;
    media_fragment.storage_buffer_count = 2U;
    // The media stage reads the same scene constants as the lit stage, for the
    // one field both have to agree about: the project's brightness curve.
    media_fragment.uniform_buffer_count = 1U;

    rhi::DepthState composite_depth = depth_state;
    composite_depth.write = false;
    constexpr std::array<scene::BlendMode, 4U> blend_modes{
        scene::BlendMode::Normal,
        scene::BlendMode::Add,
        scene::BlendMode::Multiply,
        scene::BlendMode::Screen};
    std::array<rhi::GraphicsPipeline, 4U> blended_pipelines{};
    std::array<rhi::GraphicsPipeline, 4U> media_blended_pipelines{};
    for (std::size_t index = 0U; index < blend_modes.size(); ++index) {
        auto blended = device.create_pipeline(
            vertex,
            fragment,
            rhi::TextureFormat::Rgba8Unorm,
            to_rhi_blend(blend_modes[index]),
            composite_depth);
        if (!blended) {
            return Result::failure(
                blended.error() == rhi::RhiError::ShaderFormatUnsupported
                    ? RenderError::ShaderFormatUnsupported
                    : RenderError::BackendFailure);
        }
        blended_pipelines[index] = std::move(blended).value();

        auto media_blended = device.create_pipeline(
            vertex,
            media_fragment,
            rhi::TextureFormat::Rgba8Unorm,
            to_rhi_blend(blend_modes[index]),
            composite_depth);
        if (!media_blended) {
            return Result::failure(
                media_blended.error() == rhi::RhiError::ShaderFormatUnsupported
                    ? RenderError::ShaderFormatUnsupported
                    : RenderError::BackendFailure);
        }
        media_blended_pipelines[index] = std::move(media_blended).value();
    }
    // A clip that occludes an isolated layer needs depth without colour. The
    // ordinary source-over factors leave the destination untouched at the zero
    // alpha the depth-only branch of the media stage returns, so this is the
    // blended pipeline with depth writes turned back on.
    auto media_depth_pipeline = device.create_pipeline(
        vertex,
        media_fragment,
        rhi::TextureFormat::Rgba8Unorm,
        rhi::ColorBlend::Normal,
        depth_state);
    if (!media_depth_pipeline) {
        return Result::failure(
            media_depth_pipeline.error() ==
                    rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }
    // Linear, so a clip at one resolution resamples cleanly onto a quad of any
    // size on screen.
    auto media_sampler = device.create_sampler(rhi::Filter::Linear);
    if (!media_sampler) {
        return Result::failure(RenderError::AllocationFailed);
    }
    auto material_fallback = device.create_texture(sampled_desc(1U, 1U));
    if (!material_fallback) {
        return Result::failure(RenderError::AllocationFailed);
    }

    auto vertices = device.create_storage_buffer(
        limits.max_vertices * static_cast<core::u32>(sizeof(scene::MeshVertex)));
    auto indices = device.create_storage_buffer(
        limits.max_indices * static_cast<core::u32>(sizeof(core::u32)));
    auto instances = device.create_storage_buffer(
        limits.max_instances * static_cast<core::u32>(sizeof(GpuInstance)));
    auto lights = device.create_storage_buffer(
        limits.max_lights * static_cast<core::u32>(sizeof(GpuLight)));
    auto inline_effect_ops = device.create_storage_buffer(
        limits.max_inline_effect_ops
        * static_cast<core::u32>(sizeof(GpuInlineEffectOp)));
    auto inline_effect_parameters = device.create_storage_buffer(
        limits.max_inline_effect_parameter_vectors
        * static_cast<core::u32>(sizeof(GpuInlineEffectParameter)));
    if (!vertices || !indices || !instances || !lights
        || !inline_effect_ops || !inline_effect_parameters) {
        return Result::failure(RenderError::AllocationFailed);
    }

    SceneRenderer renderer{
        device,
        limits,
        std::move(color).value(),
        std::move(depth).value(),
        std::move(pipeline).value(),
        std::move(blended_pipelines),
        std::move(media_blended_pipelines),
        std::move(media_depth_pipeline).value(),
        std::move(media_sampler).value(),
        std::move(material_fallback).value(),
        std::move(vertices).value(),
        std::move(indices).value(),
        std::move(instances).value(),
        std::move(lights).value(),
        std::move(inline_effect_ops).value(),
        std::move(inline_effect_parameters).value()};

    // Every per-frame arena is reserved to its cap here so that the frame path
    // never grows one. The mesh arenas are not: they grow as geometry becomes
    // resident, which happens on a document change rather than on a frame.
    renderer.instance_scratch_.reserve(
        static_cast<std::size_t>(limits.max_instances) * sizeof(GpuInstance));
    renderer.light_scratch_.reserve(
        static_cast<std::size_t>(limits.max_lights) * sizeof(GpuLight));
    renderer.inline_effect_op_scratch_.reserve(
        static_cast<std::size_t>(limits.max_inline_effect_ops)
        * sizeof(GpuInlineEffectOp));
    renderer.inline_effect_parameter_scratch_.reserve(
        static_cast<std::size_t>(limits.max_inline_effect_parameter_vectors)
        * sizeof(GpuInlineEffectParameter));
    renderer.inline_program_cache_.reserve(limits.max_instances);
    renderer.batch_uniforms_.reserve(
        static_cast<std::size_t>(limits.max_instances) * sizeof(BatchConstants));
    renderer.batches_.reserve(limits.max_instances);
    renderer.batch_textures_.reserve(
        static_cast<std::size_t>(limits.max_instances) * 3U);
    renderer.composite_draws_.reserve(limits.max_instances);
    renderer.depth_only_media_.reserve(scene::max_scene_media);
    return Result::success(std::move(renderer));
}

bool SceneRenderer::resize(
    rhi::Device& device,
    core::u32 width,
    core::u32 height) noexcept {
    if (width == 0U || height == 0U) {
        return false;
    }
    if (color_.width() == width && color_.height() == height) {
        return true;
    }
    auto color = device.create_texture(color_desc(width, height));
    if (!color) {
        return false;
    }
    auto depth = device.create_texture(depth_desc(width, height));
    if (!depth) {
        return false;
    }
    // Both replaced together or neither: a colour target that outlived its
    // depth buffer would fail every subsequent draw.
    color_ = std::move(color).value();
    depth_ = std::move(depth).value();
    return true;
}

bool SceneRenderer::append_mesh(
    const scene::MeshData& mesh,
    MeshRange& range) {
    const auto vertex_stride = sizeof(scene::MeshVertex);
    const auto resident_vertices =
        static_cast<core::u64>(vertex_arena_.size() / vertex_stride);
    const auto resident_indices =
        static_cast<core::u64>(index_arena_.size() / sizeof(core::u32));

    if (resident_vertices + mesh.vertices.size() > limits_.max_vertices
        || resident_indices + mesh.indices.size() > limits_.max_indices) {
        // Out of arena. Reported through `dropped_instances` rather than as a
        // failed frame: one over-tessellated object or heavy model should not
        // black out an output that is otherwise correct.
        return false;
    }

    const auto vertex_offset = vertex_arena_.size();
    vertex_arena_.resize(
        vertex_offset + mesh.vertices.size() * vertex_stride);
    std::memcpy(
        vertex_arena_.data() + vertex_offset,
        mesh.vertices.data(),
        mesh.vertices.size() * vertex_stride);

    const auto first_index = static_cast<core::u32>(resident_indices);
    const auto base_vertex = static_cast<core::u32>(resident_vertices);
    const auto index_offset = index_arena_.size();
    index_arena_.resize(
        index_offset + mesh.indices.size() * sizeof(core::u32));
    auto* const written =
        reinterpret_cast<core::u32*>(index_arena_.data() + index_offset);
    for (std::size_t position = 0U; position < mesh.indices.size();
         ++position) {
        // Indices are made absolute on the way in, so a batch is a range in
        // the index arena and nothing else - no base-vertex argument, and no
        // per-backend disagreement about whether one is applied.
        written[position] = base_vertex + mesh.indices[position];
    }

    range = MeshRange{
        first_index, static_cast<core::u32>(mesh.indices.size())};
    geometry_dirty_ = true;
    return true;
}

bool SceneRenderer::ensure_resident(scene::MeshKey key) {
    const auto normal_key = scene::normalized(key);
    if (resident_.find(normal_key) != resident_.end()) {
        return true;
    }
    if (!scene::build_mesh(normal_key, mesh_scratch_)) {
        return false;
    }
    MeshRange range{};
    if (!append_mesh(mesh_scratch_, range)) {
        return false;
    }
    resident_.emplace(normal_key, range);
    return true;
}

bool SceneRenderer::ensure_model_resident(
    const std::string& source,
    rhi::CommandList& commands) {
    if (resident_models_.find(source) != resident_models_.end()) {
        return true;
    }
    if (!load_model(source, model_scratch_)) {
        return false;
    }
    MeshRange asset_range{};
    if (!append_mesh(model_scratch_.mesh, asset_range)) {
        return false;
    }
    ResidentModel resident{};
    resident.bounds = model_scratch_.bounds;
    resident.textures.reserve(model_scratch_.textures.size());
    for (const ModelTexture& source_texture : model_scratch_.textures) {
        rhi::Texture texture{};
        if (source_texture.valid()) {
            auto created = device_->create_texture(sampled_desc(
                source_texture.width, source_texture.height));
            if (created) {
                texture = std::move(created).value();
                if (commands.upload(texture, source_texture.rgba)) {
                    stats_.upload_bytes += source_texture.rgba.size();
                } else {
                    texture = rhi::Texture{};
                }
            }
        }
        resident.textures.push_back(std::move(texture));
    }
    resident.parts.reserve(model_scratch_.parts.size());
    for (const auto& source_part : model_scratch_.parts) {
        resident.parts.push_back(ResidentModelPart{
            MeshRange{
                asset_range.first_index + source_part.first_index,
            source_part.index_count},
            source_part.material,
            source_part.has_material,
            source_part.base_color_texture,
            source_part.metallic_roughness_texture,
            source_part.emissive_texture});
    }
    resident_models_.emplace(source, std::move(resident));
    return true;
}

bool SceneRenderer::render(
    rhi::CommandList& commands,
    const scene::Scene& scene,
    scene::CanvasRect slice) {
    return render(commands, scene, slice, SceneRenderOptions{});
}

bool SceneRenderer::render(
    rhi::CommandList& commands,
    const scene::Scene& scene,
    scene::CanvasRect slice,
    const SceneRenderOptions& options) {
    media_cache_.begin_frame();
    struct EndMediaFrame final {
        MediaCache* cache{};
        ~EndMediaFrame() { cache->end_frame(); }
    } end_media_frame{&media_cache_};
    return render(commands, scene, slice, options, media_cache_);
}

bool SceneRenderer::render(
    rhi::CommandList& commands,
    const scene::Scene& scene,
    scene::CanvasRect slice,
    const SceneRenderOptions& options,
    MediaCache& media_cache) {
    stats_ = RenderStats{};

    if (!scene::valid(scene.camera) ||
        (!options.view.has_value() && !scene::contains(scene.canvas, slice))) {
        return false;
    }

    SceneRenderView render_view{};
    if (options.view.has_value()) {
        render_view = *options.view;
        if (!finite(render_view.eye) || !finite(render_view.forward) ||
            !(scene::length(render_view.forward) > 1.0e-20F) ||
            !finite(render_view.matrices.view) ||
            !finite(render_view.matrices.projection) ||
            !finite(render_view.matrices.view_projection)) {
            return false;
        }
        render_view.forward = scene::normalize(render_view.forward);
    } else {
        render_view.matrices =
            scene::view_for_slice(scene.camera, scene.canvas, slice);
        render_view.eye = scene.camera.eye;
        render_view.forward = scene::normalize(
            scene.camera.target - scene.camera.eye,
            scene::Vec3{0.0F, 0.0F, -1.0F});
    }

    if (!material_fallback_uploaded_) {
        constexpr std::array<core::u8, 4U> white{255U, 255U, 255U, 255U};
        if (!commands.upload(material_fallback_, white)) {
            return false;
        }
        material_fallback_uploaded_ = true;
        stats_.upload_bytes += white.size();
    }

    instance_scratch_.clear();
    light_scratch_.clear();
    inline_effect_op_scratch_.clear();
    inline_effect_parameter_scratch_.clear();
    inline_program_cache_.clear();
    batch_uniforms_.clear();
    batches_.clear();
    batch_textures_.clear();
    composite_draws_.clear();
    depth_only_media_.clear();
    core::u32 curve_luts_used{};

    core::u32 next_ordinal = 0U;
    const auto held_instances = [&]() noexcept {
        return static_cast<std::size_t>(stats_.instances)
            + composite_draws_.size() + depth_only_media_.size();
    };
    const std::array<const rhi::Texture*, 3U> fallback_material_textures{
        &material_fallback_, &material_fallback_, &material_fallback_};
    const auto append_material_textures =
        [&](const std::array<const rhi::Texture*, 3U>& textures) {
        for (const rhi::Texture* const texture : textures) {
            batch_textures_.push_back(
                rhi::SampledTexture{texture, &media_sampler_});
            }
        };
    const auto inline_range_for =
        [&](doc::NodeId source,
            const scene::EffectStack& effects,
            InlineProgramRange& range) {
            if (source.value() != 0U) {
                const auto found = std::find_if(
                    inline_program_cache_.begin(),
                    inline_program_cache_.end(),
                    [source](const InlineProgramCacheEntry& entry) noexcept {
                        return entry.source == source;
                    });
                if (found != inline_program_cache_.end()) {
                    range = found->range;
                    return true;
                }
            }

            CompiledInlineRange compiled{};
            if (!compile_inline_program(
                    effects,
                    scene.effect_settings,
                    limits_.max_inline_effect_ops,
                    limits_.max_inline_effect_parameter_vectors,
                    limits_.max_inline_curve_luts,
                    curve_luts_used,
                    inline_effect_op_scratch_,
                    inline_effect_parameter_scratch_,
                    compiled)) {
                return false;
            }
            range = InlineProgramRange{compiled.first, compiled.count};
            if (source.value() != 0U) {
                inline_program_cache_.push_back(
                    InlineProgramCacheEntry{source, range});
            }
            return true;
        };
    struct SiblingOrder final {
        std::optional<doc::NodeId> parent;
        core::i64 order{};
    };
    const auto queue_composite = [&](const MeshRange& range,
                                     const scene::MeshInstance& instance,
                                     ResolvedMediaFrame media,
                                     const std::array<const rhi::Texture*, 3U>&
                                         material_textures,
                                     float depth,
                                     doc::NodeId source,
                                     std::optional<core::u64> timeline_order =
                                         std::nullopt,
                                     std::optional<SiblingOrder> sibling_order =
                                         std::nullopt) {
        if (held_instances() >= limits_.max_instances) {
            ++stats_.dropped_instances;
            return false;
        }
        CompositeDraw draw{};
        draw.range = range;
        draw.instance = instance;
        draw.material_textures = material_textures;
        draw.media = media;
        draw.blend = valid_blend(instance.style.blend);
        draw.depth = depth;
        draw.source_order = timeline_order.value_or(source.value());
        if (sibling_order.has_value()) {
            draw.sibling_ordered = true;
            draw.sibling_parent = sibling_order->parent;
            draw.sibling_order = sibling_order->order;
        }
        draw.ordinal = next_ordinal;
        ++next_ordinal;
        composite_draws_.push_back(draw);
        return true;
    };

    // Opaque instances retain the mesh-grouped fast path. Composited instances
    // are only queued here, so they do not interrupt the contiguous opaque
    // instance slots even when the evaluated scene interleaves the two.
    const MeshRange* open_range = nullptr;
    core::u32 open_first_instance = 0U;
    core::u32 open_count = 0U;
    scene::MeshKey open_key{};

    const auto close_batch = [&]() {
        if (open_range == nullptr || open_count == 0U) {
            return;
        }
        batches_.push_back(
            rhi::DrawBatch{open_range->index_count, open_count});
        append_material_textures(fallback_material_textures);
        append(
            batch_uniforms_,
            BatchConstants{open_range->first_index, open_first_instance, 0U, 0U});
        stats_.triangles += (open_range->index_count / 3U) * open_count;
        open_range = nullptr;
        open_count = 0U;
    };

    for (const auto& instance : scene.instances) {
        if (opacity(instance.style) <= 0.0F) {
            continue;
        }
        if (held_instances() >= limits_.max_instances) {
            ++stats_.dropped_instances;
            continue;
        }
        const auto key = scene::normalized(instance.mesh);
        if (!ensure_resident(key)) {
            ++stats_.dropped_instances;
            continue;
        }
        const auto found = resident_.find(key);
        if (found == resident_.end() || found->second.index_count == 0U) {
            ++stats_.dropped_instances;
            continue;
        }

        // Occluders join the batched depth-writing pass whatever their own
        // compositing style says, because the only thing wanted from them is
        // the depth their silhouette would have written.
        const bool instance_depth_only = options.depth_only(instance.source);
        if (!instance_depth_only && needs_compositing(instance.style)) {
            queue_composite(
                found->second,
                instance,
                ResolvedMediaFrame{},
                fallback_material_textures,
                camera_depth(instance.world, scene::Vec3{}, render_view.eye,
                             render_view.forward),
                instance.source,
                std::nullopt,
                SiblingOrder{instance.parent_group, instance.group_order});
            continue;
        }

        if (open_range == nullptr || !(key == open_key)) {
            close_batch();
            open_key = key;
            open_range = &found->second;
            open_first_instance = stats_.instances;
        }
        InlineProgramRange effect_range{};
        if (!instance_depth_only
            && !inline_range_for(
                instance.source, instance.style.effects, effect_range)) {
            return false;
        }
        append(
            instance_scratch_,
            to_gpu(
                instance,
                effect_range.first,
                effect_range.count,
                instance_depth_only));
        ++open_count;
        ++stats_.instances;
        if (instance_depth_only) {
            ++stats_.depth_only_instances;
        }
    }
    close_batch();

    // Imported models upload once per asset while retaining a batch per source
    // material part. Their transformed asset-bounds centre supplies one stable
    // sort depth for every part of an instance.
    for (const auto& model : scene.models) {
        if (opacity(model.style) <= 0.0F) {
            continue;
        }
        const std::string& model_source = model.source;
        if (model_source.empty() ||
            !ensure_model_resident(model_source, commands)) {
            ++stats_.dropped_instances;
            continue;
        }
        const auto found = resident_models_.find(model_source);
        if (found == resident_models_.end() || found->second.parts.empty()) {
            ++stats_.dropped_instances;
            continue;
        }
        const bool model_depth_only = options.depth_only(model.source_node);
        const auto local_center =
            (found->second.bounds.minimum + found->second.bounds.maximum) * 0.5F;
        const float depth = camera_depth(model.world, local_center,
                                         render_view.eye,
                                         render_view.forward);
        const auto texture_for = [&](std::optional<core::u32> index)
            -> const rhi::Texture* {
            if (index && *index < found->second.textures.size()) {
                const auto& texture = found->second.textures[*index];
                if (texture) {
                    return &texture;
                }
            }
            return &material_fallback_;
        };
        for (const auto& part : found->second.parts) {
            if (held_instances() >= limits_.max_instances) {
                ++stats_.dropped_instances;
                continue;
            }
            if (part.range.index_count == 0U) {
                ++stats_.dropped_instances;
                continue;
            }

            scene::MeshInstance proxy{};
            proxy.world = model.world;
            proxy.normal = model.normal;
            // Source-assigned material factors are authoritative. The authored
            // model material is used only for an unassigned primitive; an
            // explicit future override therefore needs its own state bit rather
            // than making today's default gray erase imported appearance.
            proxy.material = part.has_material ? part.material : model.material;
            proxy.source = model.source_node;
            proxy.style = model.style;
            const std::array<const rhi::Texture*, 3U> material_textures{
                texture_for(part.base_color_texture),
                texture_for(part.metallic_roughness_texture),
                texture_for(part.emissive_texture)};

            if (!model_depth_only && needs_compositing(model.style)) {
                queue_composite(
                    part.range,
                    proxy,
                    ResolvedMediaFrame{},
                    material_textures,
                    depth,
                    model.source_node,
                    std::nullopt,
                    SiblingOrder{model.parent_group, model.group_order});
                continue;
            }

            const auto slot = stats_.instances;
            InlineProgramRange effect_range{};
            if (!model_depth_only
                && !inline_range_for(
                    model.source_node, model.style.effects, effect_range)) {
                return false;
            }
            append(
                instance_scratch_,
                to_gpu(
                    proxy,
                    effect_range.first,
                    effect_range.count,
                    model_depth_only));
            batches_.push_back(rhi::DrawBatch{part.range.index_count, 1U});
            append_material_textures(material_textures);
            append(
                batch_uniforms_,
                BatchConstants{part.range.first_index, slot, 0U, 0U});
            stats_.triangles += part.range.index_count / 3U;
            ++stats_.instances;
            if (model_depth_only) {
                ++stats_.depth_only_instances;
            }
        }
    }

    // A media frame can contain alpha even when its layer opacity is one, so it
    // always joins the compositing queue. That is the only honest treatment
    // without asset metadata guaranteeing every texel is opaque.
    for (const auto& clip : scene.media) {
        if (opacity(clip.style) <= 0.0F) {
            continue;
        }
        if (clip.mesh.primitive == scene::Primitive::Quad
            && media_quad_outside_view(
                clip.world, render_view.matrices.view_projection,
                clip.style.effects, scene.effect_settings)) {
            ++stats_.culled_media;
            continue;
        }
        if (held_instances() >= limits_.max_instances) {
            ++stats_.dropped_instances;
            continue;
        }
        const auto key = scene::normalized(clip.mesh);
        if (!ensure_resident(key)) {
            continue;
        }
        const auto found = resident_.find(key);
        if (found == resident_.end() || found->second.index_count == 0U) {
            continue;
        }
        const std::string& media_source = clip.source;
        if (media_source.empty()) {
            continue;
        }
        const ResolvedMediaFrame media_frame = media_cache.resolve(
            *device_, commands, media_source, clip.frame_index,
            MediaLoopWindow{clip.loop_first, clip.loop_count},
            clip.source_node.value());
        if (!media_frame) {
            // The file could not be loaded: show nothing for this clip rather
            // than failing the whole frame.
            continue;
        }

        scene::MeshInstance proxy{};
        proxy.mesh = clip.mesh;
        proxy.world = clip.world;
        proxy.normal = clip.normal;
        proxy.source = clip.source_node;
        proxy.style = clip.style;
        if (options.depth_only(clip.source_node)) {
            CompositeDraw draw{};
            draw.range = found->second;
            draw.instance = proxy;
            draw.media = media_frame;
            draw.ordinal = next_ordinal;
            ++next_ordinal;
            depth_only_media_.push_back(draw);
            continue;
        }
        queue_composite(
            found->second,
            proxy,
            media_frame,
            fallback_material_textures,
            camera_depth(clip.world, scene::Vec3{}, render_view.eye,
                         render_view.forward),
            clip.source_node,
            clip.compositing_order,
            clip.timeline_lane == doc::NodeId{}
                ? std::optional<SiblingOrder>{SiblingOrder{
                      clip.parent_group, clip.group_order}}
                : std::nullopt);
    }
    // Clips that are about to cut in. Named after the drawing pass, so a frame
    // the show needs now is always queued ahead of a frame it needs shortly.
    for (const auto& upcoming : scene.media_prewarm) {
        if (upcoming.source.empty()) {
            continue;
        }
        media_cache.prewarm(
            upcoming.source,
            upcoming.frame_index,
            upcoming.source_node.value());
    }
    // Back-to-front for colour correctness, then the layer's own order and
    // the collection ordinal for a total order. The latter two make equal-depth
    // layers deterministic even though procedural instances arrived grouped
    // by mesh rather than by document order. Media clips carry their lane and
    // clip order in place of a node id, so clips sharing one surface - equal
    // depth to the bit - stack the way the timeline stacks them, while clips on
    // surfaces at different depths occlude by where they actually are. Depth
    // has to stay the primary key: these draws do not write depth, so drawing
    // order is the only thing deciding which one survives.
    std::sort(
        composite_draws_.begin(),
        composite_draws_.end(),
        [](const CompositeDraw& left, const CompositeDraw& right) noexcept {
            if (left.depth != right.depth) {
                return left.depth > right.depth;
            }
            if (left.sibling_ordered && right.sibling_ordered
                && left.sibling_parent == right.sibling_parent
                && left.sibling_order != right.sibling_order) {
                return left.sibling_order < right.sibling_order;
            }
            if (left.source_order != right.source_order) {
                return left.source_order < right.source_order;
            }
            return left.ordinal < right.ordinal;
        });
    // Depth-only clips are recorded before every destination-reading draw, so
    // their instance slots are claimed first and in document order.
    for (auto& draw : depth_only_media_) {
        draw.first_instance = stats_.instances;
        append(
            instance_scratch_,
            to_gpu(draw.instance, 0U, 0U, true, draw.media.layout));
        stats_.triangles += draw.range.index_count / 3U;
        ++stats_.instances;
        ++stats_.depth_only_instances;
    }
    for (auto& draw : composite_draws_) {
        draw.first_instance = stats_.instances;
        InlineProgramRange effect_range{};
        if (!inline_range_for(
                draw.instance.source,
                draw.instance.style.effects,
                effect_range)) {
            return false;
        }
        draw.first_effect = effect_range.first;
        draw.effect_count = effect_range.count;
        append(
            instance_scratch_,
            to_gpu(
                draw.instance, draw.first_effect, draw.effect_count, false,
                draw.media.layout));
        stats_.triangles += draw.range.index_count / 3U;
        ++stats_.instances;
        if (draw.media) {
            ++stats_.media;
        }
    }

    stats_.batches = static_cast<core::u32>(
        batches_.size() + depth_only_media_.size() + composite_draws_.size());
    stats_.meshes_resident = static_cast<core::u32>(
        resident_.size() + resident_models_.size());

    const auto push_light = [&](const float vector[3U],
                                float kind,
                                scene::Vec3 color,
                                float intensity,
                                float range) {
        const auto held = static_cast<core::u32>(light_scratch_.size() / sizeof(GpuLight));
        if (held >= limits_.max_lights) {
            ++stats_.dropped_lights;
            return;
        }
        GpuLight record{};
        record.vector_kind[0U] = vector[0U];
        record.vector_kind[1U] = vector[1U];
        record.vector_kind[2U] = vector[2U];
        record.vector_kind[3U] = kind;
        record.color_intensity[0U] = color.x;
        record.color_intensity[1U] = color.y;
        record.color_intensity[2U] = color.z;
        record.color_intensity[3U] = intensity;
        record.params[0U] = range;
        append(light_scratch_, record);
    };

    for (const auto& light : scene.directional_lights) {
        const float vector[3U]{light.direction.x, light.direction.y, light.direction.z};
        push_light(vector, 0.0F, light.color, light.intensity, 0.0F);
    }
    for (const auto& light : scene.point_lights) {
        const float vector[3U]{light.position.x, light.position.y, light.position.z};
        push_light(vector, 1.0F, light.color, light.intensity, light.range);
    }

    if (geometry_dirty_) {
        if (!commands.upload(vertex_buffer_, vertex_arena_)
            || !commands.upload(index_buffer_, index_arena_)) {
            return false;
        }
        geometry_dirty_ = false;
        stats_.geometry_uploaded = true;
        stats_.upload_bytes += vertex_arena_.size() + index_arena_.size();
    }
    if (!commands.upload(instance_buffer_, instance_scratch_)
        || !commands.upload(light_buffer_, light_scratch_)
        || !commands.upload(
            inline_effect_op_buffer_, inline_effect_op_scratch_)
        || !commands.upload(
            inline_effect_parameter_buffer_,
            inline_effect_parameter_scratch_)) {
        return false;
    }
    stats_.inline_effect_ops = static_cast<core::u32>(
        inline_effect_op_scratch_.size() / sizeof(GpuInlineEffectOp));
    stats_.upload_bytes += instance_scratch_.size() + light_scratch_.size()
        + inline_effect_op_scratch_.size()
        + inline_effect_parameter_scratch_.size();

    ViewConstants view_constants{};
    for (core::u32 index = 0U; index < 16U; ++index) {
        view_constants.view_projection[index] =
            render_view.matrices.view_projection.m[index];
    }

    SceneConstants scene_constants{};
    scene_constants.eye_position[0U] = render_view.eye.x;
    scene_constants.eye_position[1U] = render_view.eye.y;
    scene_constants.eye_position[2U] = render_view.eye.z;
    scene_constants.eye_position[3U] = 1.0F;
    scene_constants.sky_ambient[0U] = scene.environment.sky_color.x;
    scene_constants.sky_ambient[1U] = scene.environment.sky_color.y;
    scene_constants.sky_ambient[2U] = scene.environment.sky_color.z;
    scene_constants.sky_ambient[3U] = scene.environment.ambient;
    scene_constants.ground_exposure[0U] = scene.environment.ground_color.x;
    scene_constants.ground_exposure[1U] = scene.environment.ground_color.y;
    scene_constants.ground_exposure[2U] = scene.environment.ground_color.z;
    // Exposure is fixed at one until there is a camera model to derive it
    // from. It belongs with lumens and shutter angle, not with the surround.
    scene_constants.ground_exposure[3U] = 1.0F;
    scene_constants.counts[0U] =
        static_cast<core::u32>(light_scratch_.size() / sizeof(GpuLight));
    scene_constants.counts[1U] =
        scene.effect_settings.brightness_curve == scene::BrightnessCurve::Gain
            ? 1U
            : 0U;
    scene_constants.grain_context[0U] = slice.x;
    scene_constants.grain_context[1U] = slice.y;
    scene_constants.grain_context[2U] =
        static_cast<core::u32>(scene.show_frame & 0xFFFF'FFFFULL);
    scene_constants.grain_context[3U] =
        static_cast<core::u32>(scene.show_frame >> 32U);
    scene_constants.grain_extent[0U] = slice.width;
    scene_constants.grain_extent[1U] = slice.height;
    scene_constants.grain_extent[2U] = width();
    scene_constants.grain_extent[3U] = height();

    const rhi::StorageBuffer* const vertex_bindings[]{
        &vertex_buffer_, &index_buffer_, &instance_buffer_};
    const rhi::StorageBuffer* const lit_fragment_bindings[]{
        &light_buffer_,
        &inline_effect_op_buffer_,
        &inline_effect_parameter_buffer_};
    const rhi::StorageBuffer* const media_fragment_bindings[]{
        &inline_effect_op_buffer_, &inline_effect_parameter_buffer_};

    rhi::PulledDraw draw{};
    draw.pipeline = &pipeline_;
    draw.vertex_buffers = vertex_bindings;
    draw.fragment_buffers = lit_fragment_bindings;
    draw.vertex_uniform = bytes_of(view_constants);
    draw.fragment_uniform = bytes_of(scene_constants);
    draw.batch_uniforms = batch_uniforms_;
    draw.batch_uniform_stride = static_cast<core::u32>(sizeof(BatchConstants));
    draw.batches = batches_;
    draw.fragment_textures = batch_textures_;
    draw.fragment_textures_per_batch = 3U;
    draw.store_depth =
        !composite_draws_.empty() || !depth_only_media_.empty();

    // The ordinary clear is the sky, so an empty scene still reads as a frame.
    // Isolated compositor layers instead retain transparent uncovered pixels.
    // Depth clears to zero because the projection is reversed.
    const rhi::Color clear = options.transparent_background
        ? rhi::Color{0.0F, 0.0F, 0.0F, 0.0F}
        : rhi::Color{
              scene.environment.sky_color.x,
              scene.environment.sky_color.y,
              scene.environment.sky_color.z,
              1.0F};
    if (!commands.draw_pulled(color_, clear, depth_, 0.0F, draw)) {
        return false;
    }

    // Clips that occlude this pass, one depth-writing alpha-tested pass each.
    // They run before any composited layer so every later draw is tested
    // against the complete depth of the frame rather than part of it.
    for (std::size_t index = 0U; index < depth_only_media_.size(); ++index) {
        const auto& occluder = depth_only_media_[index];
        const rhi::DrawBatch occluder_batch{occluder.range.index_count, 1U};
        const BatchConstants occluder_offsets{
            occluder.range.first_index, occluder.first_instance, 0U, 0U};
        rhi::PulledDraw occluder_draw{};
        occluder_draw.pipeline = &media_depth_pipeline_;
        occluder_draw.vertex_buffers = vertex_bindings;
        occluder_draw.vertex_uniform = bytes_of(view_constants);
        const std::array<rhi::SampledTexture, 2U> occluder_sources{{
            rhi::SampledTexture{occluder.media.primary, &media_sampler_},
            rhi::SampledTexture{
                occluder.media.chroma != nullptr
                    ? occluder.media.chroma
                    : &material_fallback_,
                &media_sampler_},
        }};
        occluder_draw.fragment_textures = occluder_sources;
        occluder_draw.fragment_buffers = media_fragment_bindings;
        occluder_draw.fragment_uniform = bytes_of(scene_constants);
        occluder_draw.batch_uniforms = bytes_of(occluder_offsets);
        occluder_draw.batch_uniform_stride =
            static_cast<core::u32>(sizeof(BatchConstants));
        occluder_draw.batches =
            std::span<const rhi::DrawBatch>{&occluder_batch, 1U};
        occluder_draw.load = true;
        occluder_draw.store_depth = index + 1U < depth_only_media_.size() ||
            !composite_draws_.empty();
        if (!commands.draw_pulled(
                color_, clear, depth_, 0.0F, occluder_draw)) {
            return false;
        }
    }

    // Destination-reading layers are intentionally one batch per pass. That
    // preserves their sorted order while allowing every layer to choose its
    // own fixed-function blend equation and, for media, its own texture.
    for (std::size_t index = 0U; index < composite_draws_.size(); ++index) {
        const auto& composite = composite_draws_[index];
        const rhi::DrawBatch composite_batch{
            composite.range.index_count, 1U};
        const BatchConstants composite_offsets{
            composite.range.first_index,
            composite.first_instance,
            0U,
            0U};
        rhi::PulledDraw composite_draw{};
        const auto pipeline_index = blend_index(composite.blend);
        composite_draw.pipeline = composite.media
            ? &media_blended_pipelines_[pipeline_index]
            : &blended_pipelines_[pipeline_index];
        composite_draw.vertex_buffers = vertex_bindings;
        composite_draw.vertex_uniform = bytes_of(view_constants);
        std::array<rhi::SampledTexture, 3U> material_bindings{};
        std::array<rhi::SampledTexture, 2U> media_bindings{};
        if (!composite.media) {
            material_bindings = std::array<rhi::SampledTexture, 3U>{
                rhi::SampledTexture{
                    composite.material_textures[0U], &media_sampler_},
                rhi::SampledTexture{
                    composite.material_textures[1U], &media_sampler_},
                rhi::SampledTexture{
                    composite.material_textures[2U], &media_sampler_}};
            composite_draw.fragment_textures = material_bindings;
            composite_draw.fragment_buffers = lit_fragment_bindings;
            composite_draw.fragment_uniform = bytes_of(scene_constants);
        } else {
            media_bindings = std::array<rhi::SampledTexture, 2U>{{
                rhi::SampledTexture{
                    composite.media.primary, &media_sampler_},
                rhi::SampledTexture{
                    composite.media.chroma != nullptr
                        ? composite.media.chroma
                        : &material_fallback_,
                    &media_sampler_},
            }};
            composite_draw.fragment_textures = media_bindings;
            composite_draw.fragment_buffers = media_fragment_bindings;
            composite_draw.fragment_uniform = bytes_of(scene_constants);
        }
        composite_draw.batch_uniforms = bytes_of(composite_offsets);
        composite_draw.batch_uniform_stride =
            static_cast<core::u32>(sizeof(BatchConstants));
        composite_draw.batches =
            std::span<const rhi::DrawBatch>{&composite_batch, 1U};
        composite_draw.load = true;
        composite_draw.store_depth = index + 1U < composite_draws_.size();
        if (!commands.draw_pulled(
                color_, clear, depth_, 0.0F, composite_draw)) {
            return false;
        }
    }
    return true;
}

} // namespace mediaserver::render
