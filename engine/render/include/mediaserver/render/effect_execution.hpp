#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/scene/scene.hpp>

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
    GaussianBlur,
    Count,
};

struct EffectApplication final {
    bool enabled{};
    float value{};

    bool operator==(const EffectApplication&) const = default;
};

using ResolveEffectApplication = EffectApplication (*)(
    const scene::EffectStack&) noexcept;
using IsIdentityEffect = bool (*)(EffectApplication) noexcept;
using DisableEffect = void (*)(scene::EffectStack&) noexcept;

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

    bool operator==(const EffectExecutionTraits&) const = default;
};

namespace detail {

[[nodiscard]] inline EffectApplication resolve_brightness(
    const scene::EffectStack& effects) noexcept {
    return EffectApplication{effects.brightness_enabled, effects.brightness};
}

[[nodiscard]] inline EffectApplication resolve_blur(
    const scene::EffectStack& effects) noexcept {
    return EffectApplication{effects.blur_enabled, effects.blur_radius};
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

inline void disable_brightness(scene::EffectStack& effects) noexcept {
    effects.brightness_enabled = false;
    effects.brightness = 1.0F;
}

inline void disable_blur(scene::EffectStack& effects) noexcept {
    effects.blur_enabled = false;
    effects.blur_radius = 0.0F;
}

} // namespace detail

inline constexpr std::size_t effect_kind_count =
    static_cast<std::size_t>(scene::EffectKind::Count);

// Indexed by EffectKind. Keep this declaration aggregate-only: its size and
// index validation below are the compile-time gate that every new effect must
// pass before any renderer can be built.
inline constexpr std::array<EffectExecutionTraits, effect_kind_count>
    effect_execution_registry{{
        {scene::EffectKind::Brightness,
         "Brightness",
         EffectExecutionDomain::Inline,
         EffectDepthPolicy::SharedSceneDepth,
         EffectProcessor::MaterialBrightness,
         1.0F,
         false,
         false,
         false,
         &detail::resolve_brightness,
         &detail::brightness_is_identity,
         &detail::disable_brightness},
        {scene::EffectKind::Blur,
         "Blur",
         EffectExecutionDomain::IsolatedLayer,
         EffectDepthPolicy::TestAgainstSceneDepth,
         EffectProcessor::GaussianBlur,
         0.0F,
         true,
         true,
         false,
         &detail::resolve_blur,
         &detail::blur_is_identity,
         &detail::disable_blur},
    }};

[[nodiscard]] consteval bool effect_execution_registry_complete() noexcept {
    if (effect_execution_registry.size() != effect_kind_count) {
        return false;
    }
    for (std::size_t index = 0U; index < effect_execution_registry.size();
         ++index) {
        const EffectExecutionTraits& traits =
            effect_execution_registry[index];
        if (static_cast<std::size_t>(traits.kind) != index ||
            traits.name.empty() || traits.resolve == nullptr ||
            traits.is_identity == nullptr || traits.disable == nullptr ||
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
    case EffectProcessor::GaussianBlur:
        return traits.kind == scene::EffectKind::Blur &&
            traits.domain == EffectExecutionDomain::IsolatedLayer;
    case EffectProcessor::Count:
        return false;
    }
    return false;
}

[[nodiscard]] inline bool effect_stack_is_executable(
    const scene::EffectStack& effects) noexcept {
    for (const EffectExecutionTraits& traits : effect_execution_registry) {
        const EffectApplication application = traits.resolve(effects);
        if (!traits.is_identity(application) &&
            !effect_processor_is_implemented(traits)) {
            return false;
        }
    }
    return true;
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
