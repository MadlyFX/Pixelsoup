#include <mediaserver/render/effect_execution.hpp>

#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

namespace render = mediaserver::render;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define EFFECT_CHECK(...)                                                      \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

void every_effect_kind_has_one_complete_contract() {
    const auto registry = render::all_effect_execution_traits();
    EFFECT_CHECK(registry.size() == render::effect_kind_count);
    for (std::size_t index = 0U; index < registry.size(); ++index) {
        const render::EffectExecutionTraits& traits = registry[index];
        const auto kind = static_cast<scene::EffectKind>(index);
        EFFECT_CHECK(traits.kind == kind);
        EFFECT_CHECK(!traits.name.empty());
        EFFECT_CHECK(traits.resolve != nullptr);
        EFFECT_CHECK(traits.is_identity != nullptr);
        EFFECT_CHECK(traits.disable != nullptr);
        EFFECT_CHECK(render::effect_traits_for(kind) == &traits);

        // Identity is part of the contract, not an effect-specific planner
        // special case. Disabled and enabled-at-identity must both be inert.
        EFFECT_CHECK(traits.is_identity(
            render::EffectApplication{false, traits.identity_value + 1.0F}));
        EFFECT_CHECK(traits.is_identity(
            render::EffectApplication{true, traits.identity_value}));

        bool processor_supported{};
        switch (traits.processor) {
        case render::EffectProcessor::MaterialBrightness:
            processor_supported =
                traits.domain == render::EffectExecutionDomain::Inline;
            break;
        case render::EffectProcessor::GaussianBlur:
            processor_supported =
                traits.domain ==
                render::EffectExecutionDomain::IsolatedLayer;
            break;
        case render::EffectProcessor::Count:
            break;
        }
        EFFECT_CHECK(processor_supported);
        EFFECT_CHECK(render::effect_processor_is_implemented(traits));
    }
}

void identity_effects_never_change_the_render_plan_contract() {
    scene::EffectStack effects{};
    effects.brightness_enabled = true;
    effects.brightness = 1.0F;
    effects.blur_enabled = true;
    effects.blur_radius = 0.0F;

    EFFECT_CHECK(render::effect_is_identity(
        scene::EffectKind::Brightness, effects));
    EFFECT_CHECK(render::effect_is_identity(scene::EffectKind::Blur, effects));
    EFFECT_CHECK(!render::effect_stack_requires_isolation(effects));
    EFFECT_CHECK(render::effect_stack_preserves_hard_occlusion(effects));

    effects.blur_radius = std::numeric_limits<float>::quiet_NaN();
    EFFECT_CHECK(render::effect_is_identity(scene::EffectKind::Blur, effects));
    EFFECT_CHECK(!render::effect_stack_requires_isolation(effects));
    EFFECT_CHECK(render::effect_stack_preserves_hard_occlusion(effects));
}

void inline_and_isolated_effects_follow_their_declared_depth_rules() {
    scene::EffectStack effects{};
    effects.brightness_enabled = true;
    effects.brightness = 2.0F;
    EFFECT_CHECK(!render::effect_is_identity(
        scene::EffectKind::Brightness, effects));
    EFFECT_CHECK(!render::effect_stack_requires_isolation(effects));
    EFFECT_CHECK(render::effect_stack_preserves_hard_occlusion(effects));

    effects.blur_enabled = true;
    effects.blur_radius = 6.0F;
    EFFECT_CHECK(!render::effect_is_identity(scene::EffectKind::Blur, effects));
    EFFECT_CHECK(render::effect_stack_requires_isolation(effects));
    EFFECT_CHECK(!render::effect_stack_preserves_hard_occlusion(effects));
    EFFECT_CHECK(render::effect_stack_is_executable(effects));

    render::disable_effects_in_domain(
        effects, render::EffectExecutionDomain::IsolatedLayer);
    EFFECT_CHECK(effects.brightness_enabled);
    EFFECT_CHECK(std::abs(effects.brightness - 2.0F) < 0.001F);
    EFFECT_CHECK(!effects.blur_enabled);
    EFFECT_CHECK(effects.blur_radius == 0.0F);
    EFFECT_CHECK(!render::effect_stack_requires_isolation(effects));
    EFFECT_CHECK(render::effect_stack_preserves_hard_occlusion(effects));
}

void invalid_effect_kinds_are_never_silently_treated_as_identity() {
    scene::EffectStack effects{};
    EFFECT_CHECK(render::effect_traits_for(scene::EffectKind::Count) == nullptr);
    EFFECT_CHECK(!render::resolve_effect(scene::EffectKind::Count, effects)
                      .has_value());
    EFFECT_CHECK(!render::effect_is_identity(scene::EffectKind::Count, effects));
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr Case cases[]{
    {"every effect kind has one complete contract",
     &every_effect_kind_has_one_complete_contract},
    {"identity effects never change the render plan contract",
     &identity_effects_never_change_the_render_plan_contract},
    {"inline and isolated effects follow their declared depth rules",
     &inline_and_isolated_effects_follow_their_declared_depth_rules},
    {"invalid effect kinds are never silently treated as identity",
     &invalid_effect_kinds_are_never_silently_treated_as_identity},
};

} // namespace

int main() {
    std::size_t failures{};
    for (const Case& test : cases) {
        try {
            test.function();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << std::size(cases) - failures << '/' << std::size(cases)
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
