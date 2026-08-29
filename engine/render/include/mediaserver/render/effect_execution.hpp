#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace mediaserver::render {

// Where an enabled, non-identity effect executes. This is a rendering
// contract, not an editor category: the planner uses it to decide whether the
// ordinary 3D scene may retain its shared depth buffer.
enum class EffectExecutionDomain : core::u8 {
    Inline,
    Geometry,
    IsolatedLayer,
    FullScene,
};

// The depth relationship an effect promises to preserve or produce.
enum class EffectDepthPolicy : core::u8 {
    SharedSceneDepth,
    TestAgainstSceneDepth,
    ProducesDepth,
    NoDepth,
};

// The concrete implementation route already present in the renderer. A new
// route must be implemented before a registry entry can truthfully name it.
enum class EffectProcessor : core::u8 {
    MaterialBrightness,
    MaterialSaturation,
    GaussianBlur,
    MaterialLevels,
    MaterialLiftGammaGain,
    MaterialContrastPivot,
    MaterialColorMatrix,
    MaterialWhiteBalance,
    MaterialCurves,
    MaterialFilmGrain,
    BoxBlur,
    DirectionalBlur,
    ChromaKey,
    LumaKey,
    Count,
};

struct EffectApplication final {
    bool enabled{};
    float value{};
    scene::LevelsParameters levels{};
    scene::LiftGammaGainParameters lift_gamma_gain{};
    scene::ContrastPivotParameters contrast_pivot{};
    scene::ColorMatrixParameters color_matrix{};
    scene::WhiteBalanceParameters white_balance{};
    const scene::CurvesParameters* curves{};
    scene::FilmGrainParameters film_grain{};
    scene::BoxBlurParameters box_blur{};
    scene::DirectionalBlurParameters directional_blur{};
    scene::ChromaKeyParameters chroma_key{
        scene::Vec3{0.0F, 1.0F, 0.0F}, 0.1F, 0.1F, 0, 0.0F, 0.0F};
    scene::LumaKeyParameters luma_key{
        0.0F, 1.0F, 0.0F, 0.0F, false, 0.0F};

    bool operator==(const EffectApplication&) const = default;
};

using ResolveEffectApplication = EffectApplication (*)(
    const scene::EffectStack&) noexcept;
using IsIdentityEffect = bool (*)(EffectApplication) noexcept;
using DisableEffect = void (*)(scene::EffectStack&) noexcept;
using ResolveEffectBounds = scene::EffectBoundsExpansion (*)(
    EffectApplication) noexcept;

struct EffectExecutionTraits final {
    scene::EffectKind kind{scene::EffectKind::Brightness};
    std::string_view name;
    EffectExecutionDomain domain{EffectExecutionDomain::Inline};
    EffectDepthPolicy depth{EffectDepthPolicy::SharedSceneDepth};
    EffectProcessor processor{EffectProcessor::MaterialBrightness};
    float identity_value{};
    bool changes_alpha{};
    bool expands_bounds{};
    bool reads_destination{};
    ResolveEffectApplication resolve{};
    IsIdentityEffect is_identity{};
    DisableEffect disable{};
    ResolveEffectBounds bounds{};

    bool operator==(const EffectExecutionTraits&) const = default;
};

namespace detail {

[[nodiscard]] inline EffectApplication resolve_brightness(
    const scene::EffectStack& effects) noexcept {
    for (const scene::InlineEffectOperation& operation :
         effects.inline_operations) {
        if (operation.kind == scene::EffectKind::Brightness
            && std::isfinite(operation.value)
            && operation.value != 1.0F) {
            return EffectApplication{true, operation.value, {}};
        }
    }
    return EffectApplication{effects.brightness_enabled, effects.brightness};
}

[[nodiscard]] inline EffectApplication resolve_blur(
    const scene::EffectStack& effects) noexcept {
    for (const scene::SpatialEffectOperation& operation :
         effects.spatial_operations) {
        if (operation.kind == scene::EffectKind::Blur &&
            std::isfinite(operation.value) && operation.value > 0.0F) {
            EffectApplication result{};
            result.enabled = true;
            result.value = operation.value;
            return result;
        }
    }
    return EffectApplication{effects.blur_enabled, effects.blur_radius};
}

[[nodiscard]] inline EffectApplication resolve_box_blur(
    const scene::EffectStack& effects) noexcept {
    for (const scene::SpatialEffectOperation& operation :
         effects.spatial_operations) {
        if (operation.kind == scene::EffectKind::BoxBlur &&
            !scene::box_blur_is_identity(operation.box_blur)) {
            EffectApplication result{};
            result.enabled = true;
            result.box_blur = operation.box_blur;
            return result;
        }
    }
    return EffectApplication{};
}

[[nodiscard]] inline EffectApplication resolve_directional_blur(
    const scene::EffectStack& effects) noexcept {
    for (const scene::SpatialEffectOperation& operation :
         effects.spatial_operations) {
        if (operation.kind == scene::EffectKind::DirectionalBlur &&
            !scene::directional_blur_is_identity(
                operation.directional_blur)) {
            EffectApplication result{};
            result.enabled = true;
            result.directional_blur = operation.directional_blur;
            return result;
        }
    }
    return EffectApplication{};
}

[[nodiscard]] inline EffectApplication resolve_chroma_key(
    const scene::EffectStack& effects) noexcept {
    for (const scene::SpatialEffectOperation& operation :
         effects.spatial_operations) {
        if (operation.kind == scene::EffectKind::ChromaKey &&
            !scene::chroma_key_is_identity(operation.chroma_key)) {
            EffectApplication result{};
            result.enabled = true;
            result.chroma_key = operation.chroma_key;
            return result;
        }
    }
    return EffectApplication{};
}

[[nodiscard]] inline EffectApplication resolve_luma_key(
    const scene::EffectStack& effects) noexcept {
    for (const scene::SpatialEffectOperation& operation :
         effects.spatial_operations) {
        if (operation.kind == scene::EffectKind::LumaKey &&
            !scene::luma_key_is_identity(operation.luma_key)) {
            EffectApplication result{};
            result.enabled = true;
            result.luma_key = operation.luma_key;
            return result;
        }
    }
    return EffectApplication{};
}

[[nodiscard]] inline EffectApplication resolve_saturation(
    const scene::EffectStack& effects) noexcept {
    for (const scene::InlineEffectOperation& operation :
         effects.inline_operations) {
        if (operation.kind == scene::EffectKind::Saturation
            && scene::normalized_saturation(operation.value) != 1.0F) {
            return EffectApplication{true, operation.value, {}};
        }
    }
    return EffectApplication{effects.saturation_enabled, effects.saturation};
}

[[nodiscard]] inline EffectApplication resolve_levels(
    const scene::EffectStack& effects) noexcept {
    for (const scene::InlineEffectOperation& operation :
         effects.inline_operations) {
        if (operation.kind == scene::EffectKind::Levels
            && !scene::levels_is_identity(operation.levels)) {
            return EffectApplication{true, 1.0F, operation.levels};
        }
    }
    return EffectApplication{false, 1.0F, {}};
}

[[nodiscard]] inline EffectApplication resolve_lift_gamma_gain(
    const scene::EffectStack& effects) noexcept {
    for (const scene::InlineEffectOperation& operation :
         effects.inline_operations) {
        if (operation.kind == scene::EffectKind::LiftGammaGain
            && !scene::lift_gamma_gain_is_identity(
                operation.lift_gamma_gain)) {
            return EffectApplication{
                true, 1.0F, {}, operation.lift_gamma_gain};
        }
    }
    return EffectApplication{false, 1.0F, {}, {}};
}

[[nodiscard]] inline EffectApplication resolve_contrast_pivot(
    const scene::EffectStack& effects) noexcept {
    for (const scene::InlineEffectOperation& operation :
         effects.inline_operations) {
        if (operation.kind == scene::EffectKind::ContrastPivot
            && !scene::contrast_pivot_is_identity(
                operation.contrast_pivot)) {
            return EffectApplication{
                true, 1.0F, {}, {}, operation.contrast_pivot};
        }
    }
    return EffectApplication{false, 1.0F, {}, {}, {}};
}

[[nodiscard]] inline EffectApplication resolve_color_matrix(
    const scene::EffectStack& effects) noexcept {
    for (const scene::InlineEffectOperation& operation :
         effects.inline_operations) {
        if (operation.kind == scene::EffectKind::ColorMatrix
            && !scene::color_matrix_is_identity(operation.color_matrix)) {
            return EffectApplication{
                true, 1.0F, {}, {}, {}, operation.color_matrix};
        }
    }
    return EffectApplication{false, 1.0F, {}, {}, {}, {}};
}

[[nodiscard]] inline EffectApplication resolve_white_balance(
    const scene::EffectStack& effects) noexcept {
    for (const scene::InlineEffectOperation& operation :
         effects.inline_operations) {
        if (operation.kind == scene::EffectKind::WhiteBalance
            && !scene::white_balance_is_identity(operation.white_balance)) {
            return EffectApplication{
                true, 1.0F, {}, {}, {}, {}, operation.white_balance};
        }
    }
    return EffectApplication{false, 1.0F, {}, {}, {}, {}, {}};
}

[[nodiscard]] inline EffectApplication resolve_curves(
    const scene::EffectStack& effects) noexcept {
    for (const scene::InlineEffectOperation& operation :
         effects.inline_operations) {
        if (operation.kind == scene::EffectKind::Curves
            && !scene::curves_is_identity(operation.curves)) {
            return EffectApplication{
                true, 1.0F, {}, {}, {}, {}, {}, &operation.curves};
        }
    }
    return EffectApplication{false, 1.0F, {}, {}, {}, {}, {}, nullptr};
}

[[nodiscard]] inline EffectApplication resolve_film_grain(
    const scene::EffectStack& effects) noexcept {
    for (const scene::InlineEffectOperation& operation :
         effects.inline_operations) {
        if (operation.kind == scene::EffectKind::FilmGrain
            && !scene::film_grain_is_identity(operation.film_grain)) {
            EffectApplication result{};
            result.enabled = true;
            result.value = 1.0F;
            result.film_grain = operation.film_grain;
            return result;
        }
    }
    return EffectApplication{};
}

[[nodiscard]] inline bool brightness_is_identity(
    EffectApplication effect) noexcept {
    return !effect.enabled || !std::isfinite(effect.value) ||
        effect.value == 1.0F;
}

[[nodiscard]] inline bool blur_is_identity(
    EffectApplication effect) noexcept {
    return !effect.enabled || !std::isfinite(effect.value) ||
        effect.value <= 0.0F;
}

[[nodiscard]] inline bool saturation_is_identity(
    EffectApplication effect) noexcept {
    return !effect.enabled ||
        scene::normalized_saturation(effect.value) == 1.0F;
}

[[nodiscard]] inline bool levels_is_identity(
    EffectApplication effect) noexcept {
    return !effect.enabled || scene::levels_is_identity(effect.levels);
}

[[nodiscard]] inline bool lift_gamma_gain_is_identity(
    EffectApplication effect) noexcept {
    return !effect.enabled
        || scene::lift_gamma_gain_is_identity(effect.lift_gamma_gain);
}

[[nodiscard]] inline bool contrast_pivot_is_identity(
    EffectApplication effect) noexcept {
    return !effect.enabled
        || scene::contrast_pivot_is_identity(effect.contrast_pivot);
}

[[nodiscard]] inline bool color_matrix_is_identity(
    EffectApplication effect) noexcept {
    return !effect.enabled
        || scene::color_matrix_is_identity(effect.color_matrix);
}

[[nodiscard]] inline bool white_balance_is_identity(
    EffectApplication effect) noexcept {
    return !effect.enabled
        || scene::white_balance_is_identity(effect.white_balance);
}

[[nodiscard]] inline bool curves_is_identity(
    EffectApplication effect) noexcept {
    return !effect.enabled || effect.curves == nullptr
        || scene::curves_is_identity(*effect.curves);
}

[[nodiscard]] inline bool film_grain_is_identity(
    EffectApplication effect) noexcept {
    return !effect.enabled
        || scene::film_grain_is_identity(effect.film_grain);
}

[[nodiscard]] inline bool box_blur_is_identity(
    EffectApplication effect) noexcept {
    return !effect.enabled || scene::box_blur_is_identity(effect.box_blur);
}

[[nodiscard]] inline bool directional_blur_is_identity(
    EffectApplication effect) noexcept {
    return !effect.enabled ||
        scene::directional_blur_is_identity(effect.directional_blur);
}

[[nodiscard]] inline bool chroma_key_is_identity(
    EffectApplication effect) noexcept {
    return !effect.enabled ||
        scene::chroma_key_is_identity(effect.chroma_key);
}

[[nodiscard]] inline bool luma_key_is_identity(
    EffectApplication effect) noexcept {
    return !effect.enabled || scene::luma_key_is_identity(effect.luma_key);
}

[[nodiscard]] inline scene::EffectBoundsExpansion no_bounds(
    EffectApplication) noexcept {
    return {};
}

[[nodiscard]] inline scene::EffectBoundsExpansion gaussian_blur_bounds(
    EffectApplication effect) noexcept {
    if (blur_is_identity(effect)) {
        return {};
    }
    return scene::EffectBoundsExpansion{effect.value, effect.value};
}

[[nodiscard]] inline scene::EffectBoundsExpansion box_blur_bounds(
    EffectApplication effect) noexcept {
    return effect.enabled ? scene::box_blur_bounds(effect.box_blur)
                          : scene::EffectBoundsExpansion{};
}

[[nodiscard]] inline scene::EffectBoundsExpansion directional_blur_bounds(
    EffectApplication effect) noexcept {
    return effect.enabled
        ? scene::directional_blur_bounds(effect.directional_blur)
        : scene::EffectBoundsExpansion{};
}

inline void remove_inline_kind(scene::EffectStack& effects,
                               scene::EffectKind kind) noexcept {
    std::erase_if(effects.inline_operations,
                  [kind](const scene::InlineEffectOperation& operation) {
                      return operation.kind == kind;
                  });
}

inline void disable_brightness(scene::EffectStack& effects) noexcept {
    effects.brightness_enabled = false;
    effects.brightness = 1.0F;
    effects.brightness_product_valid = false;
    effects.brightness_product = 1.0;
    remove_inline_kind(effects, scene::EffectKind::Brightness);
}

inline void disable_blur(scene::EffectStack& effects) noexcept {
    effects.blur_enabled = false;
    effects.blur_radius = 0.0F;
    std::erase_if(effects.spatial_operations,
                  [](const scene::SpatialEffectOperation& operation) {
                      return operation.kind == scene::EffectKind::Blur;
                  });
}

inline void disable_box_blur(scene::EffectStack& effects) noexcept {
    std::erase_if(effects.spatial_operations,
                  [](const scene::SpatialEffectOperation& operation) {
                      return operation.kind == scene::EffectKind::BoxBlur;
                  });
}

inline void disable_directional_blur(scene::EffectStack& effects) noexcept {
    std::erase_if(effects.spatial_operations,
                  [](const scene::SpatialEffectOperation& operation) {
                      return operation.kind ==
                          scene::EffectKind::DirectionalBlur;
                  });
}

inline void disable_chroma_key(scene::EffectStack& effects) noexcept {
    std::erase_if(effects.spatial_operations,
                  [](const scene::SpatialEffectOperation& operation) {
                      return operation.kind == scene::EffectKind::ChromaKey;
                  });
}

inline void disable_luma_key(scene::EffectStack& effects) noexcept {
    std::erase_if(effects.spatial_operations,
                  [](const scene::SpatialEffectOperation& operation) {
                      return operation.kind == scene::EffectKind::LumaKey;
                  });
}

inline void disable_saturation(scene::EffectStack& effects) noexcept {
    effects.saturation_enabled = false;
    effects.saturation = 1.0F;
    effects.saturation_product_valid = false;
    effects.saturation_product = 1.0;
    remove_inline_kind(effects, scene::EffectKind::Saturation);
}

inline void disable_levels(scene::EffectStack& effects) noexcept {
    remove_inline_kind(effects, scene::EffectKind::Levels);
}

inline void disable_lift_gamma_gain(scene::EffectStack& effects) noexcept {
    remove_inline_kind(effects, scene::EffectKind::LiftGammaGain);
}

inline void disable_contrast_pivot(scene::EffectStack& effects) noexcept {
    remove_inline_kind(effects, scene::EffectKind::ContrastPivot);
}

inline void disable_color_matrix(scene::EffectStack& effects) noexcept {
    remove_inline_kind(effects, scene::EffectKind::ColorMatrix);
}

inline void disable_white_balance(scene::EffectStack& effects) noexcept {
    remove_inline_kind(effects, scene::EffectKind::WhiteBalance);
}

inline void disable_curves(scene::EffectStack& effects) noexcept {
    remove_inline_kind(effects, scene::EffectKind::Curves);
}

inline void disable_film_grain(scene::EffectStack& effects) noexcept {
    remove_inline_kind(effects, scene::EffectKind::FilmGrain);
}

} // namespace detail

inline constexpr std::size_t effect_kind_count =
    static_cast<std::size_t>(scene::EffectKind::Count);

// Indexed by EffectKind. Keep this declaration aggregate-only: its size and
// index validation below are the compile-time gate that every new effect must
// pass before any renderer can be built.
inline constexpr std::array<EffectExecutionTraits, effect_kind_count>
    effect_execution_registry{{
        {scene::EffectKind::Brightness, "Brightness",
         EffectExecutionDomain::Inline, EffectDepthPolicy::SharedSceneDepth,
         EffectProcessor::MaterialBrightness, 1.0F, false, false, false,
         &detail::resolve_brightness, &detail::brightness_is_identity,
         &detail::disable_brightness, &detail::no_bounds},
        {scene::EffectKind::Saturation, "Saturation",
         EffectExecutionDomain::Inline, EffectDepthPolicy::SharedSceneDepth,
         EffectProcessor::MaterialSaturation, 1.0F, false, false, false,
         &detail::resolve_saturation, &detail::saturation_is_identity,
         &detail::disable_saturation, &detail::no_bounds},
        {scene::EffectKind::Blur, "Blur", EffectExecutionDomain::IsolatedLayer,
         EffectDepthPolicy::TestAgainstSceneDepth,
         EffectProcessor::GaussianBlur, 0.0F, true, true, false,
         &detail::resolve_blur, &detail::blur_is_identity,
         &detail::disable_blur, &detail::gaussian_blur_bounds},
        {scene::EffectKind::Levels, "Levels", EffectExecutionDomain::Inline,
         EffectDepthPolicy::SharedSceneDepth, EffectProcessor::MaterialLevels,
         1.0F, false, false, false, &detail::resolve_levels,
         &detail::levels_is_identity, &detail::disable_levels,
         &detail::no_bounds},
        {scene::EffectKind::LiftGammaGain, "Lift/Gamma/Gain",
         EffectExecutionDomain::Inline, EffectDepthPolicy::SharedSceneDepth,
         EffectProcessor::MaterialLiftGammaGain, 1.0F, false, false, false,
         &detail::resolve_lift_gamma_gain, &detail::lift_gamma_gain_is_identity,
         &detail::disable_lift_gamma_gain, &detail::no_bounds},
        {scene::EffectKind::ContrastPivot, "Contrast/Pivot",
         EffectExecutionDomain::Inline, EffectDepthPolicy::SharedSceneDepth,
         EffectProcessor::MaterialContrastPivot, 1.0F, false, false, false,
         &detail::resolve_contrast_pivot, &detail::contrast_pivot_is_identity,
         &detail::disable_contrast_pivot, &detail::no_bounds},
        {scene::EffectKind::ColorMatrix, "Colour Matrix",
         EffectExecutionDomain::Inline, EffectDepthPolicy::SharedSceneDepth,
         EffectProcessor::MaterialColorMatrix, 1.0F, false, false, false,
         &detail::resolve_color_matrix, &detail::color_matrix_is_identity,
         &detail::disable_color_matrix, &detail::no_bounds},
        {scene::EffectKind::WhiteBalance, "White Balance",
         EffectExecutionDomain::Inline, EffectDepthPolicy::SharedSceneDepth,
         EffectProcessor::MaterialWhiteBalance, 1.0F, false, false, false,
         &detail::resolve_white_balance, &detail::white_balance_is_identity,
         &detail::disable_white_balance, &detail::no_bounds},
        {scene::EffectKind::Curves, "Curves", EffectExecutionDomain::Inline,
         EffectDepthPolicy::SharedSceneDepth, EffectProcessor::MaterialCurves,
         1.0F, false, false, false, &detail::resolve_curves,
         &detail::curves_is_identity, &detail::disable_curves,
         &detail::no_bounds},
        {scene::EffectKind::FilmGrain, "Film Grain / Noise",
         EffectExecutionDomain::Inline, EffectDepthPolicy::SharedSceneDepth,
         EffectProcessor::MaterialFilmGrain, 0.0F, false, false, false,
         &detail::resolve_film_grain, &detail::film_grain_is_identity,
         &detail::disable_film_grain, &detail::no_bounds},
        {scene::EffectKind::BoxBlur, "Box Blur",
         EffectExecutionDomain::IsolatedLayer,
         EffectDepthPolicy::TestAgainstSceneDepth, EffectProcessor::BoxBlur,
         0.0F, true, true, false, &detail::resolve_box_blur,
         &detail::box_blur_is_identity, &detail::disable_box_blur,
         &detail::box_blur_bounds},
        {scene::EffectKind::DirectionalBlur, "Directional Blur",
         EffectExecutionDomain::IsolatedLayer,
         EffectDepthPolicy::TestAgainstSceneDepth,
         EffectProcessor::DirectionalBlur, 0.0F, true, true, false,
         &detail::resolve_directional_blur,
         &detail::directional_blur_is_identity,
         &detail::disable_directional_blur, &detail::directional_blur_bounds},
        {scene::EffectKind::ChromaKey, "Chroma Key",
         EffectExecutionDomain::IsolatedLayer,
         EffectDepthPolicy::TestAgainstSceneDepth,
         EffectProcessor::ChromaKey, 0.0F, true, false, false,
         &detail::resolve_chroma_key, &detail::chroma_key_is_identity,
         &detail::disable_chroma_key, &detail::no_bounds},
        {scene::EffectKind::LumaKey, "Luma Key",
         EffectExecutionDomain::IsolatedLayer,
         EffectDepthPolicy::TestAgainstSceneDepth,
         EffectProcessor::LumaKey, 0.0F, true, false, false,
         &detail::resolve_luma_key, &detail::luma_key_is_identity,
         &detail::disable_luma_key, &detail::no_bounds},
    }};

[[nodiscard]] consteval bool effect_execution_registry_complete() noexcept {
  if (effect_execution_registry.size() != effect_kind_count) {
    return false;
  }
  for (std::size_t index = 0U; index < effect_execution_registry.size();
       ++index) {
    const EffectExecutionTraits &traits = effect_execution_registry[index];
    if (static_cast<std::size_t>(traits.kind) != index || traits.name.empty() ||
        traits.resolve == nullptr || traits.is_identity == nullptr ||
        traits.disable == nullptr || traits.bounds == nullptr ||
        traits.processor == EffectProcessor::Count) {
      return false;
    }
  }
  return true;
}

static_assert(
    effect_execution_registry_complete(),
    "Every scene::EffectKind must have a complete execution registry entry");

[[nodiscard]] constexpr std::span<const EffectExecutionTraits>
all_effect_execution_traits() noexcept {
    return effect_execution_registry;
}

[[nodiscard]] constexpr const EffectExecutionTraits*
effect_traits_for(scene::EffectKind kind) noexcept {
    const std::size_t index = static_cast<std::size_t>(kind);
    if (index >= effect_execution_registry.size()) {
        return nullptr;
    }
    const EffectExecutionTraits& traits = effect_execution_registry[index];
    return traits.kind == kind ? &traits : nullptr;
}

[[nodiscard]] inline std::optional<EffectApplication>
resolve_effect(scene::EffectKind kind,
               const scene::EffectStack& effects) noexcept {
    const EffectExecutionTraits* const traits = effect_traits_for(kind);
    if (traits == nullptr) {
        return std::nullopt;
    }
    return traits->resolve(effects);
}

[[nodiscard]] inline bool effect_is_identity(
    scene::EffectKind kind, const scene::EffectStack& effects) noexcept {
    const EffectExecutionTraits* const traits = effect_traits_for(kind);
    return traits != nullptr && traits->is_identity(traits->resolve(effects));
}

[[nodiscard]] inline bool effect_stack_requires_isolation(
    const scene::EffectStack& effects) noexcept {
    for (const EffectExecutionTraits& traits : effect_execution_registry) {
        const EffectApplication application = traits.resolve(effects);
        if (!traits.is_identity(application) &&
            traits.domain == EffectExecutionDomain::IsolatedLayer) {
            return true;
        }
    }
    return false;
}

// This is the fail-closed inventory of processor routes the current renderer
// actually executes. A registry entry may describe a future contract before
// its processor exists, but enabling it must make planning fail rather than
// silently draw the source without the requested effect.
[[nodiscard]] constexpr bool effect_processor_is_implemented(
    const EffectExecutionTraits& traits) noexcept {
    switch (traits.processor) {
    case EffectProcessor::MaterialBrightness:
        return traits.kind == scene::EffectKind::Brightness &&
            traits.domain == EffectExecutionDomain::Inline;
    case EffectProcessor::MaterialSaturation:
        return traits.kind == scene::EffectKind::Saturation &&
            traits.domain == EffectExecutionDomain::Inline;
    case EffectProcessor::GaussianBlur:
        return traits.kind == scene::EffectKind::Blur &&
            traits.domain == EffectExecutionDomain::IsolatedLayer;
    case EffectProcessor::MaterialLevels:
        return traits.kind == scene::EffectKind::Levels &&
            traits.domain == EffectExecutionDomain::Inline;
    case EffectProcessor::MaterialLiftGammaGain:
        return traits.kind == scene::EffectKind::LiftGammaGain
            && traits.domain == EffectExecutionDomain::Inline;
    case EffectProcessor::MaterialContrastPivot:
        return traits.kind == scene::EffectKind::ContrastPivot
            && traits.domain == EffectExecutionDomain::Inline;
    case EffectProcessor::MaterialColorMatrix:
        return traits.kind == scene::EffectKind::ColorMatrix
            && traits.domain == EffectExecutionDomain::Inline;
    case EffectProcessor::MaterialWhiteBalance:
        return traits.kind == scene::EffectKind::WhiteBalance
            && traits.domain == EffectExecutionDomain::Inline;
    case EffectProcessor::MaterialCurves:
        return traits.kind == scene::EffectKind::Curves
            && traits.domain == EffectExecutionDomain::Inline;
    case EffectProcessor::MaterialFilmGrain:
        return traits.kind == scene::EffectKind::FilmGrain
            && traits.domain == EffectExecutionDomain::Inline;
    case EffectProcessor::BoxBlur:
        return traits.kind == scene::EffectKind::BoxBlur &&
            traits.domain == EffectExecutionDomain::IsolatedLayer;
    case EffectProcessor::DirectionalBlur:
        return traits.kind == scene::EffectKind::DirectionalBlur &&
            traits.domain == EffectExecutionDomain::IsolatedLayer;
    case EffectProcessor::ChromaKey:
        return traits.kind == scene::EffectKind::ChromaKey &&
            traits.domain == EffectExecutionDomain::IsolatedLayer;
    case EffectProcessor::LumaKey:
        return traits.kind == scene::EffectKind::LumaKey &&
            traits.domain == EffectExecutionDomain::IsolatedLayer;
    case EffectProcessor::Count:
        return false;
    }
    return false;
}

[[nodiscard]] inline bool effect_stack_is_executable(
    const scene::EffectStack& effects) noexcept {
    for (const scene::InlineEffectOperation& operation :
         effects.inline_operations) {
        switch (operation.kind) {
        case scene::EffectKind::Brightness:
        case scene::EffectKind::Saturation:
        case scene::EffectKind::Levels:
        case scene::EffectKind::LiftGammaGain:
        case scene::EffectKind::ContrastPivot:
        case scene::EffectKind::ColorMatrix:
        case scene::EffectKind::WhiteBalance:
        case scene::EffectKind::Curves:
        case scene::EffectKind::FilmGrain:
            break;
        case scene::EffectKind::Blur:
        case scene::EffectKind::BoxBlur:
        case scene::EffectKind::DirectionalBlur:
        case scene::EffectKind::ChromaKey:
        case scene::EffectKind::LumaKey:
        case scene::EffectKind::Count:
            return false;
        }
    }
    for (const scene::SpatialEffectOperation& operation :
         effects.spatial_operations) {
        switch (operation.kind) {
        case scene::EffectKind::Blur:
            if (!std::isfinite(operation.value) || operation.value < 0.0F) {
                return false;
            }
            break;
        case scene::EffectKind::BoxBlur:
            if (!scene::normalized_box_blur(operation.box_blur).has_value()) {
                return false;
            }
            break;
        case scene::EffectKind::DirectionalBlur:
            if (!scene::normalized_directional_blur(
                    operation.directional_blur).has_value()) {
                return false;
            }
            break;
        case scene::EffectKind::ChromaKey:
            if (!scene::normalized_chroma_key(
                    operation.chroma_key).has_value()) {
                return false;
            }
            break;
        case scene::EffectKind::LumaKey:
            if (!scene::normalized_luma_key(
                    operation.luma_key).has_value()) {
                return false;
            }
            break;
        case scene::EffectKind::Brightness:
        case scene::EffectKind::Saturation:
        case scene::EffectKind::Levels:
        case scene::EffectKind::LiftGammaGain:
        case scene::EffectKind::ContrastPivot:
        case scene::EffectKind::ColorMatrix:
        case scene::EffectKind::WhiteBalance:
        case scene::EffectKind::Curves:
        case scene::EffectKind::FilmGrain:
        case scene::EffectKind::Count:
            return false;
        }
    }
    for (const EffectExecutionTraits& traits : effect_execution_registry) {
        const EffectApplication application = traits.resolve(effects);
        if (traits.kind == scene::EffectKind::Levels && application.enabled
            && !scene::normalized_levels(application.levels).has_value()) {
            return false;
        }
        if (traits.kind == scene::EffectKind::LiftGammaGain
            && application.enabled
            && !scene::normalized_lift_gamma_gain(
                    application.lift_gamma_gain).has_value()) {
            return false;
        }
        if (traits.kind == scene::EffectKind::ContrastPivot
            && application.enabled
            && !scene::normalized_contrast_pivot(
                    application.contrast_pivot).has_value()) {
            return false;
        }
        if (traits.kind == scene::EffectKind::ColorMatrix
            && application.enabled
            && !scene::normalized_color_matrix(
                    application.color_matrix).has_value()) {
            return false;
        }
        if (traits.kind == scene::EffectKind::WhiteBalance
            && application.enabled
            && !scene::normalized_white_balance(
                    application.white_balance).has_value()) {
            return false;
        }
        if (traits.kind == scene::EffectKind::Curves && application.enabled
            && (application.curves == nullptr
                || !scene::curves_lut_is_canonical(*application.curves))) {
            return false;
        }
        if (traits.kind == scene::EffectKind::FilmGrain
            && application.enabled
            && !scene::normalized_film_grain(
                    application.film_grain).has_value()) {
            return false;
        }
        if (traits.kind == scene::EffectKind::BoxBlur && application.enabled
            && !scene::normalized_box_blur(
                    application.box_blur).has_value()) {
            return false;
        }
        if (traits.kind == scene::EffectKind::DirectionalBlur
            && application.enabled
            && !scene::normalized_directional_blur(
                    application.directional_blur).has_value()) {
            return false;
        }
        if (traits.kind == scene::EffectKind::ChromaKey
            && application.enabled
            && !scene::normalized_chroma_key(
                    application.chroma_key).has_value()) {
            return false;
        }
        if (traits.kind == scene::EffectKind::LumaKey
            && application.enabled
            && !scene::normalized_luma_key(
                    application.luma_key).has_value()) {
            return false;
        }
        if (!traits.is_identity(application) &&
            !effect_processor_is_implemented(traits)) {
            return false;
        }
    }
    return true;
}

// Sequential spatial kernels add their support. This is the authored
// canvas-space allocation bound used by planning/diagnostics; it is independent
// of a machine's output slice or adapter limits.
[[nodiscard]] inline scene::EffectBoundsExpansion effect_stack_bounds(
    const scene::EffectStack& effects,
    const scene::EffectSettings& settings = {}) noexcept {
    scene::EffectBoundsExpansion result{};
    const scene::EffectSettings valid_settings = scene::clamped(settings);
    const auto add = [&result](scene::EffectBoundsExpansion value) noexcept {
        result.x += value.x;
        result.y += value.y;
    };
    if (effects.spatial_operations.empty()) {
        const EffectApplication gaussian = detail::resolve_blur(effects);
        if (!detail::blur_is_identity(gaussian)) {
            const float reach = gaussian.value *
                static_cast<float>(valid_settings.blur_passes);
            add(scene::EffectBoundsExpansion{reach, reach});
        }
        return result;
    }
    for (const scene::SpatialEffectOperation& operation :
         effects.spatial_operations) {
        switch (operation.kind) {
        case scene::EffectKind::Blur:
            if (std::isfinite(operation.value) && operation.value > 0.0F) {
                const float reach = operation.value *
                    static_cast<float>(valid_settings.blur_passes);
                add(scene::EffectBoundsExpansion{reach, reach});
            }
            break;
        case scene::EffectKind::BoxBlur:
            add(scene::box_blur_bounds(operation.box_blur));
            break;
        case scene::EffectKind::DirectionalBlur:
            add(scene::directional_blur_bounds(
                operation.directional_blur));
            break;
        case scene::EffectKind::ChromaKey:
        case scene::EffectKind::LumaKey:
            // Key morphology is source-alpha gated. It changes coverage only
            // where the source already has coverage and therefore cannot
            // expand the authored layer allocation.
            break;
        case scene::EffectKind::Brightness:
        case scene::EffectKind::Saturation:
        case scene::EffectKind::Levels:
        case scene::EffectKind::LiftGammaGain:
        case scene::EffectKind::ContrastPivot:
        case scene::EffectKind::ColorMatrix:
        case scene::EffectKind::WhiteBalance:
        case scene::EffectKind::Curves:
        case scene::EffectKind::FilmGrain:
        case scene::EffectKind::Count:
            break;
        }
    }
    return result;
}

// A source may stand in as hard depth for another compositor pass only when
// every active effect retains a crisp, depth-producing silhouette.
[[nodiscard]] inline bool effect_stack_preserves_hard_occlusion(
    const scene::EffectStack& effects) noexcept {
    for (const EffectExecutionTraits& traits : effect_execution_registry) {
        const EffectApplication application = traits.resolve(effects);
        if (traits.is_identity(application)) {
            continue;
        }
        const bool hard_depth =
            traits.depth == EffectDepthPolicy::SharedSceneDepth ||
            traits.depth == EffectDepthPolicy::ProducesDepth;
        if (!hard_depth || traits.changes_alpha || traits.expands_bounds) {
            return false;
        }
    }
    return true;
}

inline void disable_effects_in_domain(
    scene::EffectStack& effects, EffectExecutionDomain domain) noexcept {
    for (const EffectExecutionTraits& traits : effect_execution_registry) {
        if (traits.domain == domain) {
            traits.disable(effects);
        }
    }
}

} // namespace mediaserver::render
