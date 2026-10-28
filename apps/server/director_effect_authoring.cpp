#include "director_effect_authoring.hpp"

#include "director_compositor_authoring.hpp"

#include <mediaserver/doc/node.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_project.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace mediaserver::app {

[[nodiscard]] doc::Node make_effect_node(doc::NodeId id,
                                         const editor::EffectIntent &effect) {
    doc::Node node{};
    node.id = id;
    node.type = std::string{scene::schema::effect_type};
    node.properties.emplace(
        std::string{scene::schema::effect_target},
        doc::PropertyValue{static_cast<std::int64_t>(effect.target.value())});
    std::string_view kind = scene::schema::brightness_kind;
    switch (effect.kind) {
    case scene::EffectKind::Brightness:
        kind = scene::schema::brightness_kind;
        break;
    case scene::EffectKind::Saturation:
        kind = scene::schema::saturation_kind;
        break;
    case scene::EffectKind::Blur:
        kind = scene::schema::blur_kind;
        break;
    case scene::EffectKind::BoxBlur:
        kind = scene::schema::box_blur_kind;
        break;
    case scene::EffectKind::DirectionalBlur:
        kind = scene::schema::directional_blur_kind;
        break;
    case scene::EffectKind::ChromaKey:
        kind = scene::schema::chroma_key_kind;
        break;
    case scene::EffectKind::LumaKey:
        kind = scene::schema::luma_key_kind;
        break;
    case scene::EffectKind::Levels:
        kind = scene::schema::levels_kind;
        break;
    case scene::EffectKind::LiftGammaGain:
        kind = scene::schema::lift_gamma_gain_kind;
        break;
    case scene::EffectKind::ContrastPivot:
        kind = scene::schema::contrast_pivot_kind;
        break;
    case scene::EffectKind::ColorMatrix:
        kind = scene::schema::color_matrix_kind;
        break;
    case scene::EffectKind::WhiteBalance:
        kind = scene::schema::white_balance_kind;
        break;
    case scene::EffectKind::Curves:
        kind = scene::schema::curves_kind;
        break;
    case scene::EffectKind::FilmGrain:
        kind = scene::schema::film_grain_kind;
        break;
    case scene::EffectKind::Count:
        break;
    }
    node.properties.emplace(std::string{scene::schema::effect_kind},
                            doc::PropertyValue{std::string{kind}});
    node.properties.emplace(std::string{scene::schema::effect_order},
                            doc::PropertyValue{std::int64_t{0}});
    node.properties.emplace(std::string{scene::schema::effect_enabled},
                            doc::PropertyValue{true});
    // The values a new effect starts with are scene::initial_effect()'s, the
    // same ones the Inspector's Reset to default returns a field to.
    const scene::EffectInstance initial = scene::initial_effect(effect.kind);
    const auto real = [](float value) {
        return doc::PropertyValue{static_cast<double>(value)};
    };
    const auto whole = [](auto value) {
        return doc::PropertyValue{static_cast<std::int64_t>(value)};
    };
    if (effect.kind == scene::EffectKind::Brightness) {
        node.properties.emplace(std::string{scene::schema::brightness},
                                real(initial.value));
    } else if (effect.kind == scene::EffectKind::Saturation) {
        node.properties.emplace(std::string{scene::schema::saturation},
                                real(initial.value));
    } else if (effect.kind == scene::EffectKind::Blur) {
        node.properties.emplace(std::string{scene::schema::blur_radius},
                                real(initial.value));
    } else if (effect.kind == scene::EffectKind::BoxBlur) {
        node.properties.emplace(std::string{scene::schema::box_blur_radius},
                                whole(initial.box_blur.radius_px));
        node.properties.emplace(std::string{scene::schema::box_blur_passes},
                                whole(initial.box_blur.passes));
    } else if (effect.kind == scene::EffectKind::DirectionalBlur) {
        node.properties.emplace(
            std::string{scene::schema::directional_blur_length},
            real(initial.directional_blur.length_px));
        node.properties.emplace(
            std::string{scene::schema::directional_blur_angle_turns},
            real(initial.directional_blur.angle_turns));
    } else if (effect.kind == scene::EffectKind::ChromaKey) {
        const scene::ChromaKeyParameters &key = initial.chroma_key;
        node.properties.emplace(std::string{scene::schema::chroma_key_color_r},
                                real(key.key_color.x));
        node.properties.emplace(std::string{scene::schema::chroma_key_color_g},
                                real(key.key_color.y));
        node.properties.emplace(std::string{scene::schema::chroma_key_color_b},
                                real(key.key_color.z));
        node.properties.emplace(
            std::string{scene::schema::chroma_key_tolerance},
            real(key.tolerance));
        node.properties.emplace(std::string{scene::schema::chroma_key_softness},
                                real(key.softness));
        node.properties.emplace(
            std::string{scene::schema::chroma_key_morphology_px},
            whole(key.morphology_px));
        node.properties.emplace(
            std::string{scene::schema::chroma_key_spill_suppression},
            real(key.spill_suppression));
        node.properties.emplace(std::string{scene::schema::chroma_key_mix},
                                real(key.mix));
    } else if (effect.kind == scene::EffectKind::LumaKey) {
        const scene::LumaKeyParameters &key = initial.luma_key;
        node.properties.emplace(
            std::string{scene::schema::luma_key_low_threshold},
            real(key.low_threshold));
        node.properties.emplace(
            std::string{scene::schema::luma_key_high_threshold},
            real(key.high_threshold));
        node.properties.emplace(
            std::string{scene::schema::luma_key_low_softness},
            real(key.low_softness));
        node.properties.emplace(
            std::string{scene::schema::luma_key_high_softness},
            real(key.high_softness));
        node.properties.emplace(std::string{scene::schema::luma_key_invert},
                                doc::PropertyValue{key.invert});
        node.properties.emplace(std::string{scene::schema::luma_key_mix},
                                real(key.mix));
    } else if (effect.kind == scene::EffectKind::Levels) {
        const auto add_channel =
            [&](std::string_view input_black, std::string_view input_white,
                std::string_view gamma, std::string_view output_black,
                std::string_view output_white) {
                node.properties.emplace(std::string{input_black},
                                        doc::PropertyValue{0.0});
                node.properties.emplace(std::string{input_white},
                                        doc::PropertyValue{1.0});
                node.properties.emplace(std::string{gamma},
                                        doc::PropertyValue{1.0});
                node.properties.emplace(std::string{output_black},
                                        doc::PropertyValue{0.0});
                node.properties.emplace(std::string{output_white},
                                        doc::PropertyValue{1.0});
            };
        add_channel(scene::schema::levels_master_input_black,
                    scene::schema::levels_master_input_white,
                    scene::schema::levels_master_gamma,
                    scene::schema::levels_master_output_black,
                    scene::schema::levels_master_output_white);
        add_channel(scene::schema::levels_red_input_black,
                    scene::schema::levels_red_input_white,
                    scene::schema::levels_red_gamma,
                    scene::schema::levels_red_output_black,
                    scene::schema::levels_red_output_white);
        add_channel(scene::schema::levels_green_input_black,
                    scene::schema::levels_green_input_white,
                    scene::schema::levels_green_gamma,
                    scene::schema::levels_green_output_black,
                    scene::schema::levels_green_output_white);
        add_channel(scene::schema::levels_blue_input_black,
                    scene::schema::levels_blue_input_white,
                    scene::schema::levels_blue_gamma,
                    scene::schema::levels_blue_output_black,
                    scene::schema::levels_blue_output_white);
    } else if (effect.kind == scene::EffectKind::LiftGammaGain) {
        node.properties.emplace(std::string{scene::schema::lift_r},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::lift_g},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::lift_b},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::gamma_r},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{scene::schema::gamma_g},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{scene::schema::gamma_b},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{scene::schema::gain_r},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{scene::schema::gain_g},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{scene::schema::gain_b},
                                doc::PropertyValue{1.0});
    } else if (effect.kind == scene::EffectKind::ContrastPivot) {
        node.properties.emplace(std::string{scene::schema::contrast},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{scene::schema::pivot},
                                doc::PropertyValue{0.5});
    } else if (effect.kind == scene::EffectKind::ColorMatrix) {
        node.properties.emplace(std::string{scene::schema::m00},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{scene::schema::m01},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::m02},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::m10},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::m11},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{scene::schema::m12},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::m20},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::m21},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::m22},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{scene::schema::offset_r},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::offset_g},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::offset_b},
                                doc::PropertyValue{0.0});
    } else if (effect.kind == scene::EffectKind::WhiteBalance) {
        node.properties.emplace(
            std::string{scene::schema::temperature_kelvin},
            doc::PropertyValue{static_cast<double>(
                scene::d65_white_balance_temperature_kelvin)});
        node.properties.emplace(std::string{scene::schema::tint},
                                doc::PropertyValue{0.0});
    } else if (effect.kind == scene::EffectKind::Curves) {
        node.properties.emplace(
            std::string{scene::schema::curve_master_prefix} + "0" +
                std::string{scene::schema::curve_x_suffix},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{scene::schema::curve_master_prefix} + "0" +
                std::string{scene::schema::curve_y_suffix},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{scene::schema::curve_master_prefix} + "1" +
                std::string{scene::schema::curve_x_suffix},
            doc::PropertyValue{1.0});
        node.properties.emplace(
            std::string{scene::schema::curve_master_prefix} + "1" +
                std::string{scene::schema::curve_y_suffix},
            doc::PropertyValue{1.0});
    } else if (effect.kind == scene::EffectKind::FilmGrain) {
        node.properties.emplace(std::string{scene::schema::amount},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::pixel_scale},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{scene::schema::chroma_ratio},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::seed},
                                doc::PropertyValue{std::int64_t{0}});
        node.properties.emplace(
            std::string{scene::schema::mode},
            doc::PropertyValue{std::string{scene::schema::monochrome_mode}});
    }
    return node;
}

[[nodiscard]] bool schedule_effect(cmd::DirectorScheduler &scheduler,
                                   const editor::EffectIntent &effect,
                                   doc::NodeId id, cmd::FrameNumber apply_at) {
    if (effect.kind == scene::EffectKind::Count ||
        !scheduler.projected_document().contains(effect.target) ||
        effect.target.value() >
            static_cast<core::u64>(std::numeric_limits<std::int64_t>::max())) {
        return true;
    }
    auto scheduled = scheduler.schedule_edit(
        cmd::PutNode{make_effect_node(id, effect)}, cmd::EraseNode{id},
        cmd::UndoContextId{2U}, cmd::TransactionId{effect.gesture}, apply_at);
    return static_cast<bool>(scheduled);
}

[[nodiscard]] bool schedule_film_grain_effect_edit(
    cmd::DirectorScheduler &scheduler,
    const editor::FilmGrainEffectEditIntent &request, cmd::FrameNumber apply_at,
    EditCoalescingPlan *coalescing) {
    const doc::Node *const existing =
        scheduler.projected_document().get(request.effect);
    const auto normalized = scene::normalized_film_grain(request.parameters);
    if (existing == nullptr || existing->type != scene::schema::effect_type ||
        !normalized.has_value()) {
        return true;
    }
    const auto kind = existing->properties.find(scene::schema::effect_kind);
    if (kind == existing->properties.end() ||
        std::get_if<std::string>(&kind->second) == nullptr ||
        *std::get_if<std::string>(&kind->second) !=
            scene::schema::film_grain_kind) {
        return true;
    }

    doc::Node changed = *existing;
    changed.properties.insert_or_assign(
        std::string{scene::schema::amount},
        doc::PropertyValue{static_cast<double>(normalized->amount)});
    changed.properties.insert_or_assign(
        std::string{scene::schema::pixel_scale},
        doc::PropertyValue{static_cast<double>(normalized->pixel_scale)});
    changed.properties.insert_or_assign(
        std::string{scene::schema::chroma_ratio},
        doc::PropertyValue{static_cast<double>(normalized->chroma_ratio)});
    changed.properties.insert_or_assign(
        std::string{scene::schema::seed},
        doc::PropertyValue{static_cast<std::int64_t>(normalized->seed)});
    changed.properties.insert_or_assign(
        std::string{scene::schema::mode},
        doc::PropertyValue{
            std::string{normalized->mode == scene::FilmGrainMode::RGB
                            ? scene::schema::rgb_mode
                            : scene::schema::monochrome_mode}});

    if (coalescing != nullptr) {
        return static_cast<bool>(schedule_editor_coalesced_edit(
            scheduler, cmd::PutNode{std::move(changed)},
            cmd::PutNode{*existing}, *coalescing, apply_at));
    }
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(changed)}, cmd::PutNode{*existing},
        cmd::UndoContextId{2U}, cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] bool
schedule_effect_delete(cmd::DirectorScheduler &scheduler,
                       const editor::EffectDeleteIntent &request,
                       cmd::FrameNumber apply_at) {
    const auto planned = scene::plan_delete_effect(
        scheduler.projected_document(), request.effect);
    if (!planned) {
        // A stale UI request is harmless. Allocation failure is different: do
        // not partially author a cascade whose complete dependency list could
        // not be produced.
        return planned.error() !=
               scene::EffectDeletePlanError::AllocationFailed;
    }

    const cmd::UndoContextId context{2U};
    const cmd::TransactionId transaction{request.gesture};
    for (const doc::NodeId node : planned.value().erases) {
        if (!schedule_erase_node(scheduler, node, context, transaction,
                                 apply_at)) {
            return false;
        }
    }
    return true;
}

} // namespace mediaserver::app
