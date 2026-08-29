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
        EFFECT_CHECK(traits.bounds != nullptr);
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
        case render::EffectProcessor::MaterialSaturation:
        case render::EffectProcessor::MaterialLevels:
        case render::EffectProcessor::MaterialLiftGammaGain:
        case render::EffectProcessor::MaterialContrastPivot:
        case render::EffectProcessor::MaterialColorMatrix:
        case render::EffectProcessor::MaterialWhiteBalance:
        case render::EffectProcessor::MaterialCurves:
        case render::EffectProcessor::MaterialFilmGrain:
            processor_supported =
                traits.domain == render::EffectExecutionDomain::Inline;
            break;
        case render::EffectProcessor::GaussianBlur:
        case render::EffectProcessor::BoxBlur:
        case render::EffectProcessor::DirectionalBlur:
        case render::EffectProcessor::ChromaKey:
        case render::EffectProcessor::LumaKey:
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
    effects.saturation_enabled = true;
    effects.saturation = 1.0F;
    effects.blur_enabled = true;
    effects.blur_radius = 0.0F;

    EFFECT_CHECK(render::effect_is_identity(
        scene::EffectKind::Brightness, effects));
    EFFECT_CHECK(render::effect_is_identity(
        scene::EffectKind::Saturation, effects));
    EFFECT_CHECK(render::effect_is_identity(scene::EffectKind::Blur, effects));
    EFFECT_CHECK(!render::effect_stack_requires_isolation(effects));
    EFFECT_CHECK(render::effect_stack_preserves_hard_occlusion(effects));

    effects.blur_radius = std::numeric_limits<float>::quiet_NaN();
    EFFECT_CHECK(render::effect_is_identity(scene::EffectKind::Blur, effects));
    EFFECT_CHECK(!render::effect_stack_requires_isolation(effects));
    EFFECT_CHECK(render::effect_stack_preserves_hard_occlusion(effects));

    for (const float invalid : {
             -0.01F, scene::max_saturation + 0.01F,
             std::numeric_limits<float>::infinity(),
             std::numeric_limits<float>::quiet_NaN()}) {
        effects.saturation = invalid;
        EFFECT_CHECK(render::effect_is_identity(
            scene::EffectKind::Saturation, effects));
        EFFECT_CHECK(!render::effect_stack_requires_isolation(effects));
        EFFECT_CHECK(render::effect_stack_preserves_hard_occlusion(effects));
    }
}

void inline_and_isolated_effects_follow_their_declared_depth_rules() {
    scene::EffectStack effects{};
    effects.brightness_enabled = true;
    effects.brightness = 2.0F;
    effects.brightness_product_valid = true;
    effects.brightness_product = 2.0;
    effects.saturation_enabled = true;
    effects.saturation = 0.0F;
    effects.saturation_product_valid = true;
    effects.saturation_product = 0.0;
    EFFECT_CHECK(!render::effect_is_identity(
        scene::EffectKind::Brightness, effects));
    EFFECT_CHECK(!render::effect_is_identity(
        scene::EffectKind::Saturation, effects));
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
    EFFECT_CHECK(effects.saturation_enabled);
    EFFECT_CHECK(effects.saturation == 0.0F);
    EFFECT_CHECK(!effects.blur_enabled);
    EFFECT_CHECK(effects.blur_radius == 0.0F);
    EFFECT_CHECK(!render::effect_stack_requires_isolation(effects));
    EFFECT_CHECK(render::effect_stack_preserves_hard_occlusion(effects));

    // Resetting Inline removes both inline processors and nothing from the
    // isolated domain.
    effects.blur_enabled = true;
    effects.blur_radius = 6.0F;
    render::disable_effects_in_domain(
        effects, render::EffectExecutionDomain::Inline);
    EFFECT_CHECK(!effects.brightness_enabled);
    EFFECT_CHECK(effects.brightness == 1.0F);
    EFFECT_CHECK(!effects.brightness_product_valid);
    EFFECT_CHECK(effects.brightness_product == 1.0);
    EFFECT_CHECK(!effects.saturation_enabled);
    EFFECT_CHECK(effects.saturation == 1.0F);
    EFFECT_CHECK(!effects.saturation_product_valid);
    EFFECT_CHECK(effects.saturation_product == 1.0);
    EFFECT_CHECK(effects.blur_enabled);
    EFFECT_CHECK(effects.blur_radius == 6.0F);
}

void invalid_effect_kinds_are_never_silently_treated_as_identity() {
    scene::EffectStack effects{};
    EFFECT_CHECK(render::effect_traits_for(scene::EffectKind::Count) == nullptr);
    EFFECT_CHECK(!render::resolve_effect(scene::EffectKind::Count, effects)
                      .has_value());
    EFFECT_CHECK(!render::effect_is_identity(scene::EffectKind::Count, effects));
}

void levels_is_typed_inline_and_invalid_programs_fail_closed() {
    scene::EffectStack effects{};
    scene::LevelsParameters levels{};
    levels.master.gamma = 2.0F;
    effects.inline_operations.push_back(scene::InlineEffectOperation{
        scene::EffectKind::Levels, 1.0F, levels});

    const auto application =
        render::resolve_effect(scene::EffectKind::Levels, effects);
    EFFECT_CHECK(application.has_value());
    EFFECT_CHECK(application->enabled);
    EFFECT_CHECK(application->levels == levels);
    EFFECT_CHECK(!render::effect_is_identity(
        scene::EffectKind::Levels, effects));
    EFFECT_CHECK(!render::effect_stack_requires_isolation(effects));
    EFFECT_CHECK(render::effect_stack_preserves_hard_occlusion(effects));
    EFFECT_CHECK(render::effect_stack_is_executable(effects));

    effects.inline_operations.front().levels.master.input_black = 1.0F;
    effects.inline_operations.front().levels.master.input_white = 1.0F;
    EFFECT_CHECK(!render::effect_stack_is_executable(effects));

    render::disable_effects_in_domain(
        effects, render::EffectExecutionDomain::Inline);
    EFFECT_CHECK(effects.inline_operations.empty());
}

void lift_gamma_gain_is_typed_inline_and_resettable() {
    scene::EffectStack effects{};
    scene::LiftGammaGainParameters parameters{};
    parameters.lift.x = 0.1F;
    parameters.gamma.y = 2.0F;
    parameters.gain.z = 1.5F;
    effects.inline_operations.push_back(scene::InlineEffectOperation{
        scene::EffectKind::LiftGammaGain, 1.0F, {}, parameters});

    const auto application = render::resolve_effect(
        scene::EffectKind::LiftGammaGain, effects);
    EFFECT_CHECK(application.has_value());
    EFFECT_CHECK(application->enabled);
    EFFECT_CHECK(application->lift_gamma_gain == parameters);
    EFFECT_CHECK(!render::effect_is_identity(
        scene::EffectKind::LiftGammaGain, effects));
    EFFECT_CHECK(!render::effect_stack_requires_isolation(effects));
    EFFECT_CHECK(render::effect_stack_preserves_hard_occlusion(effects));
    EFFECT_CHECK(render::effect_stack_is_executable(effects));

    effects.inline_operations.front().lift_gamma_gain.gamma.x =
        std::numeric_limits<float>::quiet_NaN();
    EFFECT_CHECK(!render::effect_stack_is_executable(effects));

    render::disable_effects_in_domain(
        effects, render::EffectExecutionDomain::Inline);
    EFFECT_CHECK(effects.inline_operations.empty());
}

void contrast_pivot_is_typed_inline_and_resettable() {
    scene::EffectStack effects{};
    const scene::ContrastPivotParameters parameters{1.5F, 0.35F};
    effects.inline_operations.push_back(scene::InlineEffectOperation{
        scene::EffectKind::ContrastPivot, 1.0F, {}, {}, parameters});

    const auto application = render::resolve_effect(
        scene::EffectKind::ContrastPivot, effects);
    EFFECT_CHECK(application.has_value());
    EFFECT_CHECK(application->enabled);
    EFFECT_CHECK(application->contrast_pivot == parameters);
    EFFECT_CHECK(!render::effect_is_identity(
        scene::EffectKind::ContrastPivot, effects));
    EFFECT_CHECK(!render::effect_stack_requires_isolation(effects));
    EFFECT_CHECK(render::effect_stack_preserves_hard_occlusion(effects));
    EFFECT_CHECK(render::effect_stack_is_executable(effects));

    effects.inline_operations.front().contrast_pivot =
        scene::ContrastPivotParameters{1.0F, 0.9F};
    EFFECT_CHECK(render::effect_is_identity(
        scene::EffectKind::ContrastPivot, effects));
    EFFECT_CHECK(render::effect_stack_is_executable(effects));

    effects.inline_operations.front().contrast_pivot.pivot =
        std::numeric_limits<float>::quiet_NaN();
    EFFECT_CHECK(!render::effect_stack_is_executable(effects));

    render::disable_effects_in_domain(
        effects, render::EffectExecutionDomain::Inline);
    EFFECT_CHECK(effects.inline_operations.empty());
}

void color_matrix_is_typed_inline_and_resettable() {
    scene::EffectStack effects{};
    scene::ColorMatrixParameters parameters{};
    parameters.matrix[1U] = 0.25F;
    parameters.offset.z = 0.1F;
    effects.inline_operations.push_back(scene::InlineEffectOperation{
        scene::EffectKind::ColorMatrix, 1.0F, {}, {}, {}, parameters});

    const auto application = render::resolve_effect(
        scene::EffectKind::ColorMatrix, effects);
    EFFECT_CHECK(application.has_value());
    EFFECT_CHECK(application->enabled);
    EFFECT_CHECK(application->color_matrix == parameters);
    EFFECT_CHECK(!render::effect_is_identity(
        scene::EffectKind::ColorMatrix, effects));
    EFFECT_CHECK(!render::effect_stack_requires_isolation(effects));
    EFFECT_CHECK(render::effect_stack_preserves_hard_occlusion(effects));
    EFFECT_CHECK(render::effect_stack_is_executable(effects));

    effects.inline_operations.front().color_matrix.matrix[8U] =
        std::numeric_limits<float>::quiet_NaN();
    EFFECT_CHECK(!render::effect_stack_is_executable(effects));

    render::disable_effects_in_domain(
        effects, render::EffectExecutionDomain::Inline);
    EFFECT_CHECK(effects.inline_operations.empty());
}

void white_balance_is_typed_inline_and_resettable() {
    scene::EffectStack effects{};
    const auto parameters = scene::normalized_white_balance(
        scene::WhiteBalanceParameters{4'000.0F, 0.25F});
    EFFECT_CHECK(parameters.has_value());
    effects.inline_operations.push_back(scene::InlineEffectOperation{
        scene::EffectKind::WhiteBalance, 1.0F, {}, {}, {}, {}, *parameters});

    const auto application = render::resolve_effect(
        scene::EffectKind::WhiteBalance, effects);
    EFFECT_CHECK(application.has_value());
    EFFECT_CHECK(application->enabled);
    EFFECT_CHECK(application->white_balance == *parameters);
    EFFECT_CHECK(!render::effect_is_identity(
        scene::EffectKind::WhiteBalance, effects));
    EFFECT_CHECK(!render::effect_stack_requires_isolation(effects));
    EFFECT_CHECK(render::effect_stack_preserves_hard_occlusion(effects));
    EFFECT_CHECK(render::effect_stack_is_executable(effects));

    effects.inline_operations.front().white_balance.tint =
        std::numeric_limits<float>::quiet_NaN();
    EFFECT_CHECK(!render::effect_stack_is_executable(effects));

    render::disable_effects_in_domain(
        effects, render::EffectExecutionDomain::Inline);
    EFFECT_CHECK(effects.inline_operations.empty());
}

void curves_are_typed_inline_bounded_and_resettable() {
    scene::CurvesParameters authored{};
    authored.master.points = {
        {0.0F, 0.0F}, {0.5F, 0.25F}, {1.0F, 1.0F}};
    authored.blue = scene::CurveChannelParameters{{
        {0.0F, 0.0F}, {0.5F, 0.75F}, {1.0F, 1.0F}}};
    const auto parameters = scene::normalized_curves(authored);
    EFFECT_CHECK(parameters.has_value());
    EFFECT_CHECK(parameters->lut.size() == scene::curve_lut_entries);

    scene::EffectStack effects{};
    effects.inline_operations.push_back(scene::InlineEffectOperation{
        scene::EffectKind::Curves, 1.0F, {}, {}, {}, {}, {}, *parameters});
    const auto application = render::resolve_effect(
        scene::EffectKind::Curves, effects);
    EFFECT_CHECK(application.has_value());
    EFFECT_CHECK(application->enabled);
    EFFECT_CHECK(application->curves != nullptr);
    EFFECT_CHECK(*application->curves == *parameters);
    EFFECT_CHECK(!render::effect_is_identity(
        scene::EffectKind::Curves, effects));
    EFFECT_CHECK(!render::effect_stack_requires_isolation(effects));
    EFFECT_CHECK(render::effect_stack_preserves_hard_occlusion(effects));
    EFFECT_CHECK(render::effect_stack_is_executable(effects));

    effects.inline_operations.front().curves.master.points[1U].x = 0.0F;
    EFFECT_CHECK(!render::effect_stack_is_executable(effects));

    render::disable_effects_in_domain(
        effects, render::EffectExecutionDomain::Inline);
    EFFECT_CHECK(effects.inline_operations.empty());
}

void film_grain_is_typed_inline_deterministic_and_resettable() {
    const scene::FilmGrainParameters parameters{
        0.3F, 4.0F, 0.8F, 0xFEED'BEEFU,
        scene::FilmGrainMode::RGB};
    scene::InlineEffectOperation operation{};
    operation.kind = scene::EffectKind::FilmGrain;
    operation.film_grain = parameters;
    scene::EffectStack effects{};
    effects.inline_operations.push_back(operation);

    const auto application = render::resolve_effect(
        scene::EffectKind::FilmGrain, effects);
    EFFECT_CHECK(application.has_value());
    EFFECT_CHECK(application->enabled);
    EFFECT_CHECK(application->film_grain == parameters);
    EFFECT_CHECK(!render::effect_is_identity(
        scene::EffectKind::FilmGrain, effects));
    EFFECT_CHECK(!render::effect_stack_requires_isolation(effects));
    EFFECT_CHECK(render::effect_stack_preserves_hard_occlusion(effects));
    EFFECT_CHECK(render::effect_stack_is_executable(effects));

    effects.inline_operations.front().film_grain.amount = 0.0F;
    EFFECT_CHECK(render::effect_is_identity(
        scene::EffectKind::FilmGrain, effects));
    EFFECT_CHECK(render::effect_stack_is_executable(effects));

    effects.inline_operations.front().film_grain.amount = 0.3F;
    effects.inline_operations.front().film_grain.pixel_scale =
        std::numeric_limits<float>::quiet_NaN();
    EFFECT_CHECK(!render::effect_stack_is_executable(effects));

    render::disable_effects_in_domain(
        effects, render::EffectExecutionDomain::Inline);
    EFFECT_CHECK(effects.inline_operations.empty());
}

void spatial_blurs_are_typed_bounded_and_resettable() {
  for (const scene::EffectKind kind :
       {scene::EffectKind::BoxBlur, scene::EffectKind::DirectionalBlur}) {
    const render::EffectExecutionTraits *const traits =
        render::effect_traits_for(kind);
    EFFECT_CHECK(traits != nullptr);
    EFFECT_CHECK(traits->changes_alpha);
    EFFECT_CHECK(traits->expands_bounds);
    EFFECT_CHECK(traits->depth ==
                 render::EffectDepthPolicy::TestAgainstSceneDepth);
  }

  scene::EffectStack effects{};
  effects.spatial_operations.push_back(scene::SpatialEffectOperation{
      scene::EffectKind::BoxBlur, 0.0F, scene::BoxBlurParameters{2U, 3U}, {}});
  effects.spatial_operations.push_back(scene::SpatialEffectOperation{
      scene::EffectKind::DirectionalBlur,
      0.0F,
      {},
      scene::DirectionalBlurParameters{10.0F, 0.0F}});

  const auto box = render::resolve_effect(scene::EffectKind::BoxBlur, effects);
  const auto directional =
      render::resolve_effect(scene::EffectKind::DirectionalBlur, effects);
  EFFECT_CHECK(box.has_value() && box->enabled);
  EFFECT_CHECK(box->box_blur == (scene::BoxBlurParameters{2U, 3U}));
  EFFECT_CHECK(directional.has_value() && directional->enabled);
  EFFECT_CHECK(directional->directional_blur ==
               (scene::DirectionalBlurParameters{10.0F, 0.0F}));
  EFFECT_CHECK(render::effect_stack_requires_isolation(effects));
  EFFECT_CHECK(!render::effect_stack_preserves_hard_occlusion(effects));
  EFFECT_CHECK(render::effect_stack_is_executable(effects));

  const scene::EffectBoundsExpansion bounds =
      render::effect_stack_bounds(effects);
  EFFECT_CHECK(std::abs(bounds.x - 11.0F) <= 1.0e-5F);
  EFFECT_CHECK(std::abs(bounds.y - 6.0F) <= 1.0e-5F);
  EFFECT_CHECK(box->box_blur.radius_px * box->box_blur.passes == 6U);

  effects.spatial_operations.front().box_blur.passes = 0U;
  EFFECT_CHECK(!render::effect_stack_is_executable(effects));
  effects.spatial_operations.front().box_blur.passes = 3U;
  effects.spatial_operations.back().directional_blur.length_px =
      std::numeric_limits<float>::quiet_NaN();
  EFFECT_CHECK(!render::effect_stack_is_executable(effects));

  render::disable_effects_in_domain(
      effects, render::EffectExecutionDomain::IsolatedLayer);
  EFFECT_CHECK(effects.spatial_operations.empty());
  EFFECT_CHECK(!render::effect_stack_requires_isolation(effects));
}

void keyers_are_typed_alpha_changing_zero_bounds_and_resettable() {
    for (const scene::EffectKind kind :
         {scene::EffectKind::ChromaKey, scene::EffectKind::LumaKey}) {
        const render::EffectExecutionTraits* const traits =
            render::effect_traits_for(kind);
        EFFECT_CHECK(traits != nullptr);
        EFFECT_CHECK(traits->domain ==
                     render::EffectExecutionDomain::IsolatedLayer);
        EFFECT_CHECK(traits->depth ==
                     render::EffectDepthPolicy::TestAgainstSceneDepth);
        EFFECT_CHECK(traits->changes_alpha);
        EFFECT_CHECK(!traits->expands_bounds);
        EFFECT_CHECK(!traits->reads_destination);
    }

    scene::SpatialEffectOperation chroma{};
    chroma.kind = scene::EffectKind::ChromaKey;
    chroma.chroma_key = scene::ChromaKeyParameters{
        {0.0F, 1.0F, 0.0F}, 0.15F, 0.05F, 3, 0.5F, 0.75F};
    scene::SpatialEffectOperation luma{};
    luma.kind = scene::EffectKind::LumaKey;
    luma.luma_key = scene::LumaKeyParameters{
        0.2F, 0.8F, 0.1F, 0.1F, true, 0.5F};
    scene::EffectStack effects{};
    effects.spatial_operations = {chroma, luma};

    const auto resolved_chroma =
        render::resolve_effect(scene::EffectKind::ChromaKey, effects);
    const auto resolved_luma =
        render::resolve_effect(scene::EffectKind::LumaKey, effects);
    EFFECT_CHECK(resolved_chroma.has_value() && resolved_chroma->enabled);
    EFFECT_CHECK(resolved_chroma->chroma_key == chroma.chroma_key);
    EFFECT_CHECK(resolved_luma.has_value() && resolved_luma->enabled);
    EFFECT_CHECK(resolved_luma->luma_key == luma.luma_key);
    EFFECT_CHECK(render::effect_stack_is_executable(effects));
    EFFECT_CHECK(render::effect_stack_requires_isolation(effects));
    EFFECT_CHECK(!render::effect_stack_preserves_hard_occlusion(effects));
    EFFECT_CHECK(render::effect_stack_bounds(effects) ==
                 scene::EffectBoundsExpansion{});

    effects.spatial_operations.front().chroma_key.tolerance =
        std::numeric_limits<float>::quiet_NaN();
    EFFECT_CHECK(!render::effect_stack_is_executable(effects));

    effects.spatial_operations.clear();
    chroma.chroma_key.mix = 0.0F;
    chroma.chroma_key.tolerance =
        std::numeric_limits<float>::quiet_NaN();
    chroma.chroma_key.morphology_px =
        scene::max_key_morphology_radius_px + 1;
    effects.spatial_operations.push_back(chroma);
    EFFECT_CHECK(render::effect_stack_is_executable(effects));
    EFFECT_CHECK(render::effect_is_identity(
        scene::EffectKind::ChromaKey, effects));
    EFFECT_CHECK(!render::effect_stack_requires_isolation(effects));
    EFFECT_CHECK(render::effect_stack_bounds(effects) ==
                 scene::EffectBoundsExpansion{});

    scene::EffectStack wrong_carrier{};
    scene::InlineEffectOperation inline_key{};
    inline_key.kind = scene::EffectKind::ChromaKey;
    wrong_carrier.inline_operations.push_back(inline_key);
    EFFECT_CHECK(!render::effect_stack_is_executable(wrong_carrier));

    render::disable_effects_in_domain(
        effects, render::EffectExecutionDomain::IsolatedLayer);
    EFFECT_CHECK(effects.spatial_operations.empty());
}

struct Case final {
  const char *name;
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
    {"Levels is typed inline and invalid programs fail closed",
     &levels_is_typed_inline_and_invalid_programs_fail_closed},
    {"Lift Gamma Gain is typed inline and resettable",
     &lift_gamma_gain_is_typed_inline_and_resettable},
    {"Contrast Pivot is typed inline and resettable",
     &contrast_pivot_is_typed_inline_and_resettable},
    {"Color Matrix is typed inline and resettable",
     &color_matrix_is_typed_inline_and_resettable},
    {"White Balance is typed inline and resettable",
     &white_balance_is_typed_inline_and_resettable},
    {"Curves are typed inline bounded and resettable",
     &curves_are_typed_inline_bounded_and_resettable},
    {"Film Grain is typed inline deterministic and resettable",
     &film_grain_is_typed_inline_deterministic_and_resettable},
    {"spatial blurs are typed bounded and resettable",
     &spatial_blurs_are_typed_bounded_and_resettable},
    {"keyers are typed alpha changing zero bounds and resettable",
     &keyers_are_typed_alpha_changing_zero_bounds_and_resettable},
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
