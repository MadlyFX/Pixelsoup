#include "director_scene_authoring.hpp"

#include "director_editor_view.hpp"
#include "plateset_authoring.hpp"
#include "director_key_authoring.hpp"
#include "model_source_authoring.hpp"

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/assets/asset_schema.hpp>
#include <mediaserver/assets/derivative.hpp>
#include <mediaserver/assets/library_edit.hpp>
#include <mediaserver/assets/proxy_policy.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/output/feed_map_schema.hpp>
#include <mediaserver/scene/linked_publication.hpp>
#include <mediaserver/scene/linked_scene.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <iostream>
#include <system_error>
#include <variant>

namespace mediaserver::app {

[[nodiscard]] std::string
group_edit_error_message(editor::GroupEditAction action,
                         scene::GroupEditError error) {
    const std::string operation =
        action == editor::GroupEditAction::Group      ? "GROUP"
        : action == editor::GroupEditAction::Create   ? "NEW GROUP"
        : action == editor::GroupEditAction::MoveInto ? "MOVE"
                                                      : "UNGROUP";
    std::string_view reason = "INVALID REQUEST";
    switch (error) {
    case scene::GroupEditError::InvalidScene:
        reason = "SCENE IS NOT ACTIVE";
        break;
    case scene::GroupEditError::EmptySelection:
        reason = "NO ELIGIBLE SELECTION";
        break;
    case scene::GroupEditError::DuplicateSelection:
        reason = "DUPLICATE SELECTION";
        break;
    case scene::GroupEditError::UnsupportedNode:
        reason = "UNSUPPORTED NODE";
        break;
    case scene::GroupEditError::CrossSceneSelection:
        reason = "CROSS-SCENE SELECTION";
        break;
    case scene::GroupEditError::AlreadyGrouped:
        reason = "SELECT TOP-LEVEL NODES";
        break;
    case scene::GroupEditError::Locked:
        reason = "SELECTION IS LOCKED";
        break;
    case scene::GroupEditError::InvalidName:
        reason = "INVALID NAME";
        break;
    case scene::GroupEditError::InvalidFirstId:
    case scene::GroupEditError::IdExhausted:
        reason = "NODE IDS EXHAUSTED";
        break;
    case scene::GroupEditError::GroupNotFound:
    case scene::GroupEditError::NotAGroup:
        reason = "GROUP NO LONGER EXISTS";
        break;
    case scene::GroupEditError::InvalidHierarchy:
        reason = "HIERARCHY NEEDS REPAIR";
        break;
    case scene::GroupEditError::AnimatedTransform:
        reason = "ANIMATED TRANSFORM CANNOT BE PRESERVED";
        break;
    case scene::GroupEditError::TransformNotRepresentable:
        reason = "WORLD TRANSFORM CANNOT BE PRESERVED";
        break;
    case scene::GroupEditError::VisibilityNotRepresentable:
        reason = "HIDDEN STATE CANNOT BE PRESERVED";
        break;
    case scene::GroupEditError::OrderOverflow:
        reason = "SIBLING ORDER EXHAUSTED";
        break;
    case scene::GroupEditError::AllocationFailed:
        reason = "OUT OF MEMORY";
        break;
    }
    return operation + " REJECTED - " + std::string{reason};
}

[[nodiscard]] std::string
mask_edit_error_message(scene::MaskAuthoringErrorCode code) {
    std::string_view reason = "INVALID REQUEST";
    switch (code) {
    case scene::MaskAuthoringErrorCode::InvalidScene:
    case scene::MaskAuthoringErrorCode::MissingTargetSceneOwner:
    case scene::MaskAuthoringErrorCode::CrossSceneTarget:
    case scene::MaskAuthoringErrorCode::CrossSceneMask:
    case scene::MaskAuthoringErrorCode::CrossScenePoint:
        reason = "SCENE OR TARGET CHANGED";
        break;
    case scene::MaskAuthoringErrorCode::TargetMissing:
    case scene::MaskAuthoringErrorCode::MaskNotFound:
    case scene::MaskAuthoringErrorCode::PointNotFound:
        reason = "TARGET NO LONGER EXISTS";
        break;
    case scene::MaskAuthoringErrorCode::UnsupportedTarget:
    case scene::MaskAuthoringErrorCode::NotAMask:
    case scene::MaskAuthoringErrorCode::NotAMaskPoint:
    case scene::MaskAuthoringErrorCode::PointMaskMismatch:
    case scene::MaskAuthoringErrorCode::NonBezierMask:
        reason = "TARGET IS INCOMPATIBLE";
        break;
    case scene::MaskAuthoringErrorCode::MaskCapacity:
    case scene::MaskAuthoringErrorCode::PointCapacity:
    case scene::MaskAuthoringErrorCode::InvalidFirstId:
    case scene::MaskAuthoringErrorCode::IdExhausted:
        reason = "MASK OR NODE LIMIT REACHED";
        break;
    case scene::MaskAuthoringErrorCode::InvalidName:
    case scene::MaskAuthoringErrorCode::NameTooLong:
        reason = "NAME IS INVALID";
        break;
    case scene::MaskAuthoringErrorCode::InvalidValue:
    case scene::MaskAuthoringErrorCode::InvalidShape:
    case scene::MaskAuthoringErrorCode::InvalidMask:
    case scene::MaskAuthoringErrorCode::InvalidPoint:
    case scene::MaskAuthoringErrorCode::InvalidMaskIndex:
    case scene::MaskAuthoringErrorCode::InvalidPointIndex:
    case scene::MaskAuthoringErrorCode::MinimumBezierPoints:
    case scene::MaskAuthoringErrorCode::InvalidFirstActiveCombine:
        reason = "MASK GEOMETRY OR ORDER IS INVALID";
        break;
    case scene::MaskAuthoringErrorCode::AllocationFailed:
        reason = "OUT OF MEMORY";
        break;
    }
    return "MASK EDIT REJECTED - " + std::string{reason};
}

[[nodiscard]] std::string
selection_set_error_message(editor::SelectionSetEditAction action,
                            const scene::SelectionSetError &error) {
    std::string_view operation = "SELECTION SET EDIT";
    switch (action) {
    case editor::SelectionSetEditAction::Create:
        operation = "SELECTION SET CREATE";
        break;
    case editor::SelectionSetEditAction::ReplaceMembers:
        operation = "SELECTION SET REPLACE";
        break;
    case editor::SelectionSetEditAction::Recall:
        operation = "SELECTION SET RECALL";
        break;
    case editor::SelectionSetEditAction::Rename:
        operation = "SELECTION SET RENAME";
        break;
    case editor::SelectionSetEditAction::Delete:
        operation = "SELECTION SET DELETE";
        break;
    }
    std::string_view reason = "INVALID REQUEST";
    switch (error.code) {
    case scene::SelectionSetErrorCode::InvalidScene:
        reason = "SCENE IS NOT ACTIVE";
        break;
    case scene::SelectionSetErrorCode::InvalidName:
        reason = "NAME IS EMPTY OR INVALID UTF-8";
        break;
    case scene::SelectionSetErrorCode::NameTooLong:
        reason = "NAME IS TOO LONG";
        break;
    case scene::SelectionSetErrorCode::EmptySelection:
        reason = "NO ELIGIBLE SELECTION";
        break;
    case scene::SelectionSetErrorCode::MemberCapacity:
        reason = "TOO MANY MEMBERS";
        break;
    case scene::SelectionSetErrorCode::InvalidFirstId:
    case scene::SelectionSetErrorCode::IdExhausted:
        reason = "NODE IDS EXHAUSTED";
        break;
    case scene::SelectionSetErrorCode::SetNotFound:
    case scene::SelectionSetErrorCode::NotASelectionSet:
        reason = "SET NO LONGER EXISTS";
        break;
    case scene::SelectionSetErrorCode::InvalidSet:
    case scene::SelectionSetErrorCode::InvalidReference:
        reason = "SET NEEDS REPAIR";
        break;
    case scene::SelectionSetErrorCode::CrossSceneSet:
        reason = "SET BELONGS TO ANOTHER SCENE";
        break;
    case scene::SelectionSetErrorCode::TargetMissing:
        reason = "A SELECTED TARGET NO LONGER EXISTS";
        break;
    case scene::SelectionSetErrorCode::UnsupportedTarget:
        reason = "SELECTION CONTAINS AN UNSUPPORTED TARGET";
        break;
    case scene::SelectionSetErrorCode::CrossSceneTarget:
        reason = "SELECTION CROSSES SCENES";
        break;
    case scene::SelectionSetErrorCode::TargetLocked:
        reason = "SELECTION CONTAINS A LOCKED TARGET";
        break;
    case scene::SelectionSetErrorCode::InvalidTarget:
        reason = "SELECTION TARGET NEEDS REPAIR";
        break;
    case scene::SelectionSetErrorCode::OrderOverflow:
        reason = "MEMBER ORDER EXHAUSTED";
        break;
    case scene::SelectionSetErrorCode::AllocationFailed:
        reason = "OUT OF MEMORY";
        break;
    }
    return std::string{operation} + " REJECTED - " + std::string{reason};
}

// Maps an editor transport request to the operator command the transport
// already understands, so timeline buttons and keyboard controls share one
// path.
[[nodiscard]] show::PresenterCommand
map_transport(editor::TransportRequest request) noexcept {
    switch (request) {
    case editor::TransportRequest::TogglePlay:
        return show::PresenterCommand::TogglePlay;
    case editor::TransportRequest::StepBack:
        return show::PresenterCommand::StepBack;
    case editor::TransportRequest::StepForward:
        return show::PresenterCommand::StepForward;
    case editor::TransportRequest::JumpBack:
        return show::PresenterCommand::JumpBack;
    case editor::TransportRequest::JumpForward:
        return show::PresenterCommand::JumpForward;
    case editor::TransportRequest::GoToStart:
        return show::PresenterCommand::GoToStart;
    case editor::TransportRequest::GoToEnd:
        return show::PresenterCommand::GoToEnd;
    case editor::TransportRequest::Go:
        return show::PresenterCommand::Go;
    case editor::TransportRequest::PlayReverse:
        return show::PresenterCommand::PlayReverse;
    case editor::TransportRequest::Pause:
        return show::PresenterCommand::Pause;
    case editor::TransportRequest::PlayForward:
        return show::PresenterCommand::PlayForward;
    case editor::TransportRequest::None:
        break;
    }
    return show::PresenterCommand::None;
}

// Authors one Inspector edit of a named property (a light field). Reads the
// node, sets the property, PutNode with the restoring inverse.
[[nodiscard]] bool schedule_generic_edit(
    cmd::DirectorScheduler &scheduler, const editor::GenericEdit &edit,
    cmd::FrameNumber apply_at, EditCoalescingPlan *coalescing,
    std::string* feedback) {
    if (edit.property.empty()) {
        return true;
    }
    const auto *const existing = scheduler.projected_document().get(edit.node);
    if (existing == nullptr) {
        return true;
    }
    doc::Node updated = *existing;
    if ((scene::is_linked_occurrence(*existing) || existing->type == scene::link_schema::instance_type) && edit.property.starts_with("link_")) {
        const auto& root = scheduler.projected_document();
        auto planned = scene::LinkedPlanResult::failure({edit.node, "Unknown linked edit"});
        if (edit.property == scene::link_schema::action && edit.text) {
            const std::pair<std::string_view, scene::LinkedEdit> actions[] = {
                {"hold", scene::LinkedEdit::Hold}, {"follow", scene::LinkedEdit::Follow},
                {"reset", scene::LinkedEdit::ResetPlacement}, {"hide", scene::LinkedEdit::Hide},
                {"visibility", scene::LinkedEdit::FollowVisibility}, {"material", scene::LinkedEdit::ResetMaterial},
                {"forget", scene::LinkedEdit::ForgetRemoved}, {"duplicate", scene::LinkedEdit::DuplicateLink},
                {"pause",scene::LinkedEdit::Pause},{"resume",scene::LinkedEdit::Resume},
                {"unlink",scene::LinkedEdit::Unlink},{"show",scene::LinkedEdit::Show}};
            for (const auto& [name, action] : actions)
                if (name == *edit.text) planned = scene::plan_linked_edit(root, edit.node, action);
        } else if (edit.property.starts_with("link_world_") || edit.property.starts_with("link_stage_")) {
            auto parsed = scene::read_linked_scene(root);
            const char axis = edit.property.back();
            if (existing->type == scene::link_schema::instance_type && edit.property.starts_with("link_stage_")
                && axis >= '0' && axis <= '2' && std::isfinite(edit.value)) {
                auto alignment = scene::identity();
                for (std::size_t i=0;i<16;++i) alignment.m[i] = static_cast<float>(read_transform(*existing,"link_alignment_"+std::to_string(i),alignment.m[i]));
                alignment.m[12+axis-'0'] = static_cast<float>(edit.value);
                planned = scene::plan_link_alignment(root,edit.node,alignment);
            } else if (parsed && parsed.value().occurrences.contains(edit.node) && axis >= '0' && axis <= '2' && std::isfinite(edit.value)) {
                const auto& placed = parsed.value().occurrences.at(edit.node);
                if (edit.property.starts_with("link_world_")) {
                    auto world = placed.world; world.m[12U + static_cast<std::size_t>(axis - '0')] = static_cast<float>(edit.value);
                    planned = scene::plan_linked_world(root, edit.node, world);
                } else {
                    auto attachment = *root.get(placed.instance);
                    attachment.properties["link_alignment_" + std::to_string(12 + axis - '0')] = edit.value;
                    // An identity alignment is serialized explicitly by the authoring plan.
                    planned = scene::plan_linked_change(root, root.set(std::move(attachment)));
                }
            }
        }
        if (!planned) {
            if (feedback) *feedback = "Linked scene: " + planned.error().message;
            return true;
        }
        const auto delta = doc::diff(root, planned.value());
        if (delta.changes.size() == 1 && delta.changes.front().after && coalescing) {
            const auto& change = delta.changes.front();
            return static_cast<bool>(schedule_editor_coalesced_edit(scheduler,
                cmd::PutNode{*change.after}, cmd::PutNode{*change.before}, *coalescing, apply_at));
        }
        return static_cast<bool>(scheduler.schedule_document_replace(planned.value(),
            cmd::UndoContextId{2U}, cmd::TransactionId{edit.gesture}, apply_at));
    }
    if (edit.property == scene::schema::animation_start_frame && edit.text.has_value()) {
        if (existing->type != scene::schema::model_type) return true;
        std::int64_t value{};
        const auto& text=*edit.text;
        const auto result=std::from_chars(text.data(),text.data()+text.size(),value);
        if (result.ec!=std::errc{} || result.ptr!=text.data()+text.size() || value<0) return true;
        updated.properties.insert_or_assign(edit.property,doc::PropertyValue{value});
    } else if (edit.property == scene::schema::animation_clip || edit.property == scene::schema::wind_period_frames) {
        if (existing->type != scene::schema::model_type || !std::isfinite(edit.value)) return true;
        const bool clip=edit.property==scene::schema::animation_clip;
        const auto value=static_cast<std::int64_t>(std::llround(std::clamp(edit.value,clip?0.0:2.0,clip?63.0:1000000.0)));
        updated.properties.insert_or_assign(edit.property,doc::PropertyValue{value});
    } else if (std::string_view{edit.property}.starts_with(scene::schema::particle_parameter_prefix)) {
        // A published parameter's base value: a key the scene can read and a
        // finite number in float range, or the node would become unreadable.
        const std::string_view key = std::string_view{edit.property}.substr(scene::schema::particle_parameter_prefix.size());
        if (existing->type != scene::schema::particle_effect_type || !scene::parse_particle_parameter_key(key)
            || !std::isfinite(edit.value) || std::abs(edit.value) > static_cast<double>(std::numeric_limits<float>::max()))
            return true;
        updated.properties.insert_or_assign(edit.property, doc::PropertyValue{edit.value});
    } else if (edit.property == scene::schema::particle_seed ||
               edit.property == scene::schema::particle_start_frame) {
        // Whole numbers, as the scene reads them: a fractional or negative
        // value would make the node unreadable.
        if (existing->type != scene::schema::particle_effect_type || !std::isfinite(edit.value)) return true;
        const double most = edit.property == scene::schema::particle_seed ? 4294967295.0 : 9007199254740992.0;
        updated.properties.insert_or_assign(edit.property,
            doc::PropertyValue{static_cast<std::int64_t>(std::llround(std::clamp(edit.value, 0.0, most)))});
    } else if (edit.property == scene::schema::source && edit.text.has_value()) {
        return schedule_model_source_edit(scheduler, edit.node, *edit.text,
            cmd::TransactionId{edit.gesture}, apply_at, coalescing);
    } else if (edit.property == scene::schema::media_resolution && edit.text.has_value()) {
        // Only a clip has a decoded size, and only the two named sizes are
        // readable; anything else would make the clip node unreadable. Full
        // is the absence of the property, so a project that never halves a
        // clip carries nothing new.
        if (existing->type != scene::schema::media_type) return true;
        if (*edit.text == scene::schema::media_resolution_half) {
            updated.properties.insert_or_assign(edit.property, doc::PropertyValue{*edit.text});
        } else if (*edit.text == scene::schema::media_resolution_full) {
            updated.properties.erase(edit.property);
        } else {
            return true;
        }
    } else if (edit.property == scene::schema::ies_source && edit.text.has_value()) {
        if (existing->type != scene::schema::light_type || edit.text->size() > 1024U) return true;
        updated.properties.insert_or_assign(edit.property, doc::PropertyValue{*edit.text});
    } else if ((edit.property == scene::schema::base_color_texture ||
                edit.property == scene::schema::metallic_roughness_texture ||
                edit.property == scene::schema::normal_texture ||
                edit.property == scene::schema::occlusion_texture ||
                edit.property == scene::schema::emissive_texture) && edit.text.has_value()) {
        if (existing->type != scene::schema::object_type || edit.text->size() > 1024U) return true;
        updated.properties.insert_or_assign(edit.property, doc::PropertyValue{*edit.text});
    } else if (edit.property == scene::schema::box_blur_radius) {
        if (!std::isfinite(edit.value)) {
            return true;
        }
        const auto radius = static_cast<std::int64_t>(std::llround(
            std::clamp(edit.value, 0.0,
                       static_cast<double>(scene::max_box_blur_radius_px))));
        updated.properties.insert_or_assign(edit.property,
                                            doc::PropertyValue{radius});
    } else if (edit.property == scene::schema::lane_raster_width ||
               edit.property == scene::schema::lane_raster_height) {
        if (!std::isfinite(edit.value)) {
            return true;
        }
        // A raster is whole pixels, and the scene model reads it as an
        // integer: writing 1920.0 as a double would make the track node
        // unreadable and the screen disappear.
        const auto extent = static_cast<std::int64_t>(std::llround(std::clamp(
            edit.value, 1.0, static_cast<double>(scene::max_raster_extent))));
        updated.properties.insert_or_assign(edit.property,
                                            doc::PropertyValue{extent});
    } else if (edit.property == scene::schema::lane_time_offset) {
        // Whole source frames, and a slot screen's alone: on any other track
        // the scene model refuses the node, and the track with it.
        if (!std::isfinite(edit.value) ||
            !existing->properties.contains(scene::schema::lane_parent)) {
            return true;
        }
        const auto frames = static_cast<std::int64_t>(std::llround(std::clamp(
            edit.value, -static_cast<double>(scene::max_lane_time_offset),
            static_cast<double>(scene::max_lane_time_offset))));
        if (frames == 0) {
            updated.properties.erase(edit.property);
        } else {
            updated.properties.insert_or_assign(edit.property,
                                                doc::PropertyValue{frames});
        }
    } else if (edit.property == scene::schema::two_sided || edit.property == scene::schema::use_color_temperature || edit.property == scene::schema::casts_shadow || edit.property == scene::schema::bake_static || edit.property == scene::schema::bake_indirect || edit.property == scene::schema::animation_enabled || edit.property == scene::schema::animation_loop) {
        // The Inspector speaks in doubles because most of what it edits is a
        // number. A boolean written as 1.0 would parse as absent, so the
        // translation happens here rather than by widening the reader.
        updated.properties.insert_or_assign(
            edit.property, doc::PropertyValue{edit.value >= 0.5});
    } else if (edit.property == scene::schema::light_kind) {
        // Point and spot are the same node type with a different `kind`, so
        // switching is a property edit rather than a different node.
        if (!std::isfinite(edit.value)) return true;
        constexpr std::array<std::string_view, 4> kinds{scene::schema::point_kind,
            scene::schema::spot_kind, scene::schema::rectangle_kind, scene::schema::sphere_kind};
        const auto kind_index = static_cast<std::size_t>(std::clamp(edit.value, 0.0, 3.0));
        updated.properties.insert_or_assign(edit.property, std::string{kinds[kind_index]});
        // Kind and dependent fields form one undoable edit. An old beam profile
        // or unsupported unit must not invalidate a newly selected area emitter.
        if (kind_index >= 2) {
            updated.properties.insert_or_assign(std::string{scene::schema::ies_source}, std::string{});
            updated.properties.insert_or_assign(std::string{scene::schema::casts_shadow}, false);
        }
        const auto unit_property = existing->properties.find(std::string{scene::schema::light_unit});
        const auto* unit_text = unit_property == existing->properties.end() ? nullptr : std::get_if<std::string>(&unit_property->second);
        const std::string_view current_unit = unit_text ? std::string_view{*unit_text} : scene::schema::unitless_unit;
        if ((kind_index >= 2 && current_unit == scene::schema::candela_unit)
            || (kind_index < 2 && current_unit == scene::schema::nits_unit))
            updated.properties.insert_or_assign(std::string{scene::schema::light_unit}, std::string{scene::schema::unitless_unit});
    } else if (edit.property == scene::schema::light_unit) {
        if (!std::isfinite(edit.value)) return true;
        const auto unit = static_cast<scene::LightUnit>(static_cast<int>(std::clamp(edit.value, 0.0, 4.0)));
        const auto kind = existing->properties.find(std::string{scene::schema::light_kind});
        const auto* kind_text = kind == existing->properties.end() ? nullptr
            : std::get_if<std::string>(&kind->second);
        const bool directional = kind_text == nullptr || *kind_text == scene::schema::directional_kind;
        const bool area = kind_text && (*kind_text == scene::schema::rectangle_kind || *kind_text == scene::schema::sphere_kind);
        if ((area && unit == scene::LightUnit::Candela) || (!area && unit == scene::LightUnit::Nits)) return true;
        if ((directional && unit != scene::LightUnit::Unitless && unit != scene::LightUnit::Lux)
            || (!directional && unit == scene::LightUnit::Lux)) return true;
        updated.properties.insert_or_assign(edit.property, doc::PropertyValue{std::string{
            unit == scene::LightUnit::Lumens ? scene::schema::lumens_unit
            : unit == scene::LightUnit::Candela ? scene::schema::candela_unit
            : unit == scene::LightUnit::Lux ? scene::schema::lux_unit
            : unit == scene::LightUnit::Nits ? scene::schema::nits_unit : scene::schema::unitless_unit}});
    } else if (edit.property == scene::schema::fill_mode) {
        // The Inspector sends the mode as FillMode's value; the document
        // spells it as a word, like the compositor's Background.
        if (existing->type != scene::schema::object_type ||
            !std::isfinite(edit.value)) {
            return true;
        }
        constexpr std::array<std::string_view, 3U> modes{
            scene::schema::solid_fill, scene::schema::linear_fill,
            scene::schema::radial_fill};
        const auto index = static_cast<std::size_t>(
            std::llround(std::clamp(edit.value, 0.0, 2.0)));
        updated.properties.insert_or_assign(
            edit.property, doc::PropertyValue{std::string{modes[index]}});
    } else if (edit.property.starts_with("fill_")) {
        // Every other fill key is a number. Clamped here to the range the
        // scene model reads, so a scrub past an end stops there rather than
        // writing a value the next read silently clamps anyway.
        if (existing->type != scene::schema::object_type ||
            !std::isfinite(edit.value)) {
            return true;
        }
        double value = edit.value;
        if (edit.property == scene::schema::fill_angle_turns) {
            value -= std::floor(value);
        } else if (edit.property == scene::schema::fill_center_x ||
                   edit.property == scene::schema::fill_center_y) {
            value = std::clamp(value,
                               -static_cast<double>(scene::max_fill_center),
                               static_cast<double>(scene::max_fill_center));
        } else if (edit.property == scene::schema::fill_size) {
            value = std::clamp(value, 0.01,
                               static_cast<double>(scene::max_fill_size));
        } else {
            value = std::clamp(value, 0.0, 1.0);
        }
        updated.properties.insert_or_assign(edit.property,
                                            doc::PropertyValue{value});
    } else if (edit.property == scene::schema::shadow_tier) {
        const int tier = static_cast<int>(std::llround(edit.value));
        updated.properties.insert_or_assign(
            edit.property,
            doc::PropertyValue{std::string{
                tier <= 0 ? scene::schema::small_tier
                          : (tier >= 2 ? scene::schema::large_tier
                                       : scene::schema::medium_tier)}});
    } else if (edit.property == scene::schema::blend_mode &&
               existing->type == scene::schema::timeline_lane_type) {
        // A track spells its blend as a mode name, not as the numeric
        // blend_mode every drawable uses. Translating here keeps the
        // Inspector generic and keeps the track with one authored blend
        // rather than two properties that can disagree.
        if (!std::isfinite(edit.value)) {
            return true;
        }
        const auto index = static_cast<int>(std::llround(edit.value));
        std::string_view mode = scene::schema::lane_normal_mode;
        if (index == 1) {
            mode = scene::schema::lane_add_mode;
        } else if (index == 2) {
            mode = scene::schema::lane_multiply_mode;
        } else if (index == 3) {
            mode = scene::schema::lane_screen_mode;
        } else if (index != 0) {
            return true;
        }
        updated.properties.insert_or_assign(
            std::string{scene::schema::lane_compositing_mode},
            doc::PropertyValue{std::string{mode}});
    } else if (edit.property == scene::schema::lane_aspect_locked) {
        updated.properties.insert_or_assign(
            edit.property, doc::PropertyValue{edit.value >= 0.5});
    } else if (edit.property == scene::schema::effect_enabled) {
        // Switching an effect off is not deleting it: the node, its parameters,
        // and its key channels all stay, and the scene model reads this as a
        // flag rather than as the double every scalar property uses.
        if (existing->type != scene::schema::effect_type) {
            return true;
        }
        updated.properties.insert_or_assign(
            edit.property, doc::PropertyValue{edit.value >= 0.5});
    } else if (edit.property == scene::schema::box_blur_passes) {
        if (!std::isfinite(edit.value)) {
            return true;
        }
        const auto passes = static_cast<std::int64_t>(std::llround(std::clamp(
            edit.value, 1.0, static_cast<double>(scene::max_box_blur_passes))));
        updated.properties.insert_or_assign(edit.property,
                                            doc::PropertyValue{passes});
    } else if (edit.property == scene::schema::chroma_key_morphology_px) {
        if (!std::isfinite(edit.value)) {
            return true;
        }
        const auto radius = static_cast<std::int64_t>(std::llround(std::clamp(
            edit.value,
            -static_cast<double>(scene::max_key_morphology_radius_px),
            static_cast<double>(scene::max_key_morphology_radius_px))));
        updated.properties.insert_or_assign(edit.property,
                                            doc::PropertyValue{radius});
    } else if (edit.property == scene::schema::luma_key_invert) {
        updated.properties.insert_or_assign(
            edit.property, doc::PropertyValue{edit.value >= 0.5});
    } else if (edit.property == scene::schema::camera_tracking_source) {
        // Binding a feed to a camera. Node ids are whole and small, and zero
        // is the honest way to say "none" - the same value an unbound camera
        // was authored with, so unbinding restores exactly that.
        if (existing->type != scene::schema::camera_type ||
            !std::isfinite(edit.value) || edit.value < 0.0) {
            return true;
        }
        const auto source =
            static_cast<std::int64_t>(std::llround(edit.value));
        const auto *const referenced = source == 0
            ? nullptr
            : scheduler.projected_document().get(
                  doc::NodeId{static_cast<core::u64>(source)});
        if (source != 0 &&
            (referenced == nullptr ||
             referenced->type != scene::schema::tracking_source_type)) {
            return true;
        }
        updated.properties.insert_or_assign(edit.property,
                                            doc::PropertyValue{source});
    } else if (edit.property == scene::schema::tracking_enabled) {
        if (existing->type != scene::schema::tracking_source_type) {
            return true;
        }
        updated.properties.insert_or_assign(
            edit.property, doc::PropertyValue{edit.value >= 0.5});
    } else if (edit.property == scene::schema::tracking_protocol) {
        // The Inspector cycles the protocol and reports which one it landed
        // on; the document spells it, because a project outlives the order
        // this enumeration happens to be in.
        if (existing->type != scene::schema::tracking_source_type ||
            !std::isfinite(edit.value)) {
            return true;
        }
        // The Inspector's choice index: FreeD, OpenTrackIO, NatNet.
        const std::string_view protocol = edit.value >= 1.5
            ? scene::schema::natnet_protocol
            : edit.value >= 0.5 ? scene::schema::opentrackio_protocol
                                : scene::schema::freed_protocol;
        updated.properties.insert_or_assign(
            edit.property, doc::PropertyValue{std::string{protocol}});
    } else if (edit.property == scene::schema::tracking_multicast_group &&
               edit.text.has_value()) {
        // Spelled as the operator typed it, once it parses as a group: the
        // model refuses a malformed one on load, so none may be written.
        std::array<core::u8, 4U> group{};
        if (existing->type != scene::schema::tracking_source_type ||
            !scene::parse_multicast_group(*edit.text, group)) {
            return true;
        }
        updated.properties.insert_or_assign(
            edit.property,
            doc::PropertyValue{scene::multicast_group_text(group)});
    } else if (edit.property == scene::schema::tracking_port ||
               edit.property == scene::schema::tracking_command_port ||
               edit.property == scene::schema::tracking_camera_id) {
        // A port and a camera id are whole numbers the model reads as
        // integers; writing 6301.0 as a double would make the feed
        // unreadable and the source silently stop listening.
        if (existing->type != scene::schema::tracking_source_type ||
            !std::isfinite(edit.value)) {
            return true;
        }
        // A FreeD camera id is one byte; a NatNet rigid body id is Motive's
        // streaming id, any positive 32-bit number.
        const auto protocol_property =
            existing->properties.find(std::string{scene::schema::tracking_protocol});
        const auto* const protocol_text =
            protocol_property == existing->properties.end()
                ? nullptr
                : std::get_if<std::string>(&protocol_property->second);
        const bool freed = protocol_text == nullptr ||
                           *protocol_text == scene::schema::freed_protocol;
        const double limit =
            edit.property == scene::schema::tracking_camera_id
                ? (freed ? 255.0 : 2147483647.0)
                : 65535.0;
        const auto whole = static_cast<std::int64_t>(
            std::llround(std::clamp(edit.value, 0.0, limit)));
        updated.properties.insert_or_assign(edit.property,
                                            doc::PropertyValue{whole});
    } else if (edit.property == scene::schema::tracking_sensor_height_mm ||
               edit.property == scene::schema::tracking_sensor_aspect ||
               edit.property == scene::schema::tracking_fallback_focal_mm) {
        if (existing->type != scene::schema::tracking_source_type ||
            !std::isfinite(edit.value) || edit.value <= 0.0) {
            return true;
        }
        updated.properties.insert_or_assign(edit.property,
                                            doc::PropertyValue{edit.value});
    } else if (edit.property == scene::schema::tracking_axes) {
        // Written as the schema's own text rather than a number, so a project
        // file says which convention it means instead of encoding it as 1.
        if (existing->type != scene::schema::tracking_source_type ||
            !std::isfinite(edit.value)) {
            return true;
        }
        const std::string_view axes = edit.value >= 0.5
            ? scene::schema::tracking_axes_z_up
            : scene::schema::tracking_axes_stage;
        updated.properties.insert_or_assign(
            edit.property, doc::PropertyValue{std::string{axes}});
    } else if (edit.property == scene::schema::tracking_origin_x ||
               edit.property == scene::schema::tracking_origin_y ||
               edit.property == scene::schema::tracking_origin_z ||
               edit.property == scene::schema::tracking_heading ||
               edit.property == scene::schema::tracking_scale) {
        // An origin may legitimately be negative and a heading may be any
        // angle, so these are only checked for finiteness here; the model
        // clamps each to its own range once, at its boundary.
        if (existing->type != scene::schema::tracking_source_type ||
            !std::isfinite(edit.value)) {
            return true;
        }
        updated.properties.insert_or_assign(edit.property,
                                            doc::PropertyValue{edit.value});
    } else if (edit.property == scene::schema::chroma_key_color_r ||
               edit.property == scene::schema::chroma_key_color_g ||
               edit.property == scene::schema::chroma_key_color_b ||
               edit.property == scene::schema::chroma_key_tolerance ||
               edit.property == scene::schema::chroma_key_softness ||
               edit.property == scene::schema::chroma_key_spill_suppression ||
               edit.property == scene::schema::luma_key_low_threshold ||
               edit.property == scene::schema::luma_key_high_threshold ||
               edit.property == scene::schema::luma_key_low_softness ||
               edit.property == scene::schema::luma_key_high_softness) {
        if (!std::isfinite(edit.value)) {
            return true;
        }
        double value = std::clamp(edit.value, 0.0, 1.0);
        const auto number_property = [&](std::string_view property,
                                         double fallback) {
            const auto found = existing->properties.find(std::string{property});
            if (found == existing->properties.end()) {
                return fallback;
            }
            const auto *const number = std::get_if<double>(&found->second);
            return number != nullptr && std::isfinite(*number)
                       ? std::clamp(*number, 0.0, 1.0)
                       : fallback;
        };
        if (edit.property == scene::schema::luma_key_low_threshold) {
            value = std::min(
                value,
                number_property(scene::schema::luma_key_high_threshold, 1.0));
        } else if (edit.property == scene::schema::luma_key_high_threshold) {
            value = std::max(
                value,
                number_property(scene::schema::luma_key_low_threshold, 0.0));
        }
        updated.properties.insert_or_assign(edit.property,
                                            doc::PropertyValue{value});
    } else {
        updated.properties.insert_or_assign(edit.property,
                                            doc::PropertyValue{edit.value});
    }
    if (scene::is_linked_occurrence(updated)) {
        auto validated = scene::plan_linked_change(scheduler.projected_document(), scheduler.projected_document().set(updated));
        if (!validated) { std::cerr << "Linked scene edit: " << validated.error().message << '\n'; return true; }
    }
    auto scheduled = coalescing != nullptr
                         ? schedule_editor_coalesced_edit(
                               scheduler, cmd::PutNode{std::move(updated)},
                               cmd::PutNode{*existing}, *coalescing, apply_at)
                         : scheduler.schedule_edit(
                               cmd::PutNode{std::move(updated)},
                               cmd::PutNode{*existing}, cmd::UndoContextId{2U},
                               cmd::TransactionId{edit.gesture}, apply_at);
    return static_cast<bool>(scheduled);
}

[[nodiscard]] bool schedule_lock_edit(cmd::DirectorScheduler &scheduler,
                                      const editor::LockEdit &edit,
                                      cmd::FrameNumber apply_at) {
    const auto *const existing = scheduler.projected_document().get(edit.node);
    if (existing == nullptr) {
        return true;
    }
    const bool surface = existing->type == output::schema::surface_type;
    const bool track = existing->type == scene::schema::timeline_lane_type;
    const bool scene_element = existing->type == scene::schema::object_type ||
                               existing->type == scene::schema::media_type ||
                               existing->type == scene::schema::model_type ||
                               existing->type == scene::schema::light_type;
    if (!surface && !track && !scene_element) {
        return true;
    }
    doc::Node updated = *existing;
    // A track keeps its lock under its own name, so locking a screen from the
    // Inspector and locking it from the timeline are one state.
    const std::string_view property =
        surface ? output::schema::locked
                : (track ? scene::schema::lane_locked : scene::schema::locked);
    updated.properties.insert_or_assign(std::string{property},
                                        doc::PropertyValue{edit.locked});
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, cmd::PutNode{*existing},
        cmd::UndoContextId{surface ? 4U : 2U}, cmd::TransactionId{edit.gesture},
        apply_at));
}

// Authors one Inspector edit: set a single transform component's base property,
// PutNode with the inverse that restores it. The gesture groups a drag's edits.
[[nodiscard]] bool schedule_property_edit(
    cmd::DirectorScheduler &scheduler, const editor::PropertyEdit &edit,
    cmd::FrameNumber apply_at, EditCoalescingPlan *coalescing) {
    const auto name = edit.component == scene::PropertyChannel::FilmGrainAmount
                          ? scene::schema::amount
                          : transform_property(edit.component);
    if (name.empty()) {
        return true;
    }
    const auto *const existing = scheduler.projected_document().get(edit.node);
    if (existing == nullptr) {
        return true;
    }
    doc::Node updated = *existing;
    // Placement on a linked occurrence belongs to the link planner; compositing
    // channels such as opacity are ordinary shot properties on the node.
    const auto component = edit.component;
    const bool linked_placement = component == scene::TransformChannel::PositionX
        || component == scene::TransformChannel::PositionY || component == scene::TransformChannel::PositionZ
        || component == scene::TransformChannel::RotationYaw || component == scene::TransformChannel::RotationPitch
        || component == scene::TransformChannel::RotationRoll || component == scene::TransformChannel::ScaleX
        || component == scene::TransformChannel::ScaleY || component == scene::TransformChannel::ScaleZ;
    if (scene::is_linked_occurrence(*existing) && linked_placement) {
        auto parsed = scene::read_linked_scene(scheduler.projected_document());
        if (!parsed || !parsed.value().occurrences.contains(edit.node)) return true;
        auto world = parsed.value().occurrences.at(edit.node).world;
        if (component == scene::TransformChannel::PositionX) world.m[12] = static_cast<float>(edit.value);
        else if (component == scene::TransformChannel::PositionY) world.m[13] = static_cast<float>(edit.value);
        else if (component == scene::TransformChannel::PositionZ) world.m[14] = static_cast<float>(edit.value);
        else return true; // Affine rotation/scale editing is exposed by the link planner.
        auto planned = scene::plan_linked_world(scheduler.projected_document(), edit.node, world);
        if (!planned) { std::cerr << "Linked scene placement: " << planned.error().message << '\n'; return true; }
        updated = *planned.value().get(edit.node);
        const auto scheduled = coalescing
            ? schedule_editor_coalesced_edit(scheduler, cmd::PutNode{updated}, cmd::PutNode{*existing}, *coalescing, apply_at)
            : scheduler.schedule_edit(cmd::PutNode{updated}, cmd::PutNode{*existing}, cmd::UndoContextId{2U}, cmd::TransactionId{edit.gesture}, apply_at);
        return static_cast<bool>(scheduled);
    }
    if (edit.component == scene::PropertyChannel::BoxBlurRadius) {
        if (!std::isfinite(edit.value)) {
            return true;
        }
        const auto rounded = std::round(edit.value);
        const auto radius = static_cast<std::int64_t>(std::clamp(
            rounded, 0.0, static_cast<double>(scene::max_box_blur_radius_px)));
        updated.properties.insert_or_assign(std::string{name},
                                            doc::PropertyValue{radius});
    } else {
        updated.properties.insert_or_assign(std::string{name},
                                            doc::PropertyValue{edit.value});
    }
    auto scheduled = coalescing != nullptr
                         ? schedule_editor_coalesced_edit(
                               scheduler, cmd::PutNode{std::move(updated)},
                               cmd::PutNode{*existing}, *coalescing, apply_at)
                         : scheduler.schedule_edit(
                               cmd::PutNode{std::move(updated)},
                               cmd::PutNode{*existing}, cmd::UndoContextId{2U},
                               cmd::TransactionId{edit.gesture}, apply_at);
    return static_cast<bool>(scheduled);
}

[[nodiscard]] std::string
lens_calibration_property(core::u32 index, std::string_view suffix) {
    return std::string{scene::schema::lens_calibration_prefix}
        + std::to_string(index) + std::string{suffix};
}

// Keeps lens calibration on its camera node, beside the camera's fallback
// turn-FOV value. Encoder values are intentionally absent: they are stamped
// with each tracked pose and must never become a per-frame document edit.
void write_lens_profile(
    doc::Node& node, const std::optional<scene::LensProfile>& profile) {
    node.properties.insert_or_assign(
        std::string{scene::schema::lens_profile_enabled},
        doc::PropertyValue{profile.has_value()});
    if (!profile.has_value()) {
        return;
    }
    const scene::LensProfile lens = scene::clamped(*profile);
    const auto number = [&node](std::string_view key, float value) {
        node.properties.insert_or_assign(
            std::string{key}, doc::PropertyValue{static_cast<double>(value)});
    };
    number(scene::schema::lens_sensor_width_mm, lens.sensor_width_mm);
    number(scene::schema::lens_sensor_height_mm, lens.sensor_height_mm);
    number(scene::schema::lens_focal_length_mm, lens.focal_length_mm);
    number(scene::schema::lens_entrance_pupil_offset_m,
           lens.entrance_pupil_offset_m);
    number(scene::schema::lens_distortion_k1, lens.distortion.k1);
    number(scene::schema::lens_distortion_k2, lens.distortion.k2);
    number(scene::schema::lens_distortion_k3, lens.distortion.k3);
    number(scene::schema::lens_distortion_p1, lens.distortion.p1);
    number(scene::schema::lens_distortion_p2, lens.distortion.p2);
    node.properties.insert_or_assign(
        std::string{scene::schema::lens_calibration_count},
        doc::PropertyValue{static_cast<std::int64_t>(lens.calibration_count)});
    for (core::u32 index = 0U; index < lens.calibration_count; ++index) {
        const scene::LensCalibrationPoint& point = lens.calibration[index];
        number(lens_calibration_property(index,
                                         scene::schema::lens_calibration_zoom_suffix),
               point.zoom_encoder);
        number(lens_calibration_property(index,
                                         scene::schema::lens_calibration_focus_suffix),
               point.focus_encoder);
        number(lens_calibration_property(index,
                                         scene::schema::lens_calibration_focal_suffix),
               point.focal_length_mm);
        number(lens_calibration_property(index,
                                         scene::schema::lens_calibration_pupil_suffix),
               point.entrance_pupil_offset_m);
        number(lens_calibration_property(index,
                                         scene::schema::lens_calibration_k1_suffix),
               point.distortion.k1);
        number(lens_calibration_property(index,
                                         scene::schema::lens_calibration_k2_suffix),
               point.distortion.k2);
        number(lens_calibration_property(index,
                                         scene::schema::lens_calibration_k3_suffix),
               point.distortion.k3);
        number(lens_calibration_property(index,
                                         scene::schema::lens_calibration_p1_suffix),
               point.distortion.p1);
        number(lens_calibration_property(index,
                                         scene::schema::lens_calibration_p2_suffix),
               point.distortion.p2);
    }
}

[[nodiscard]] bool schedule_camera_lens_profile(
    cmd::DirectorScheduler& scheduler, const editor::ProjectSettingsEdit& edit,
    cmd::FrameNumber apply_at) {
    if (edit.camera_node == doc::NodeId{}) {
        return !edit.lens_profile.has_value();
    }
    const doc::Node* const existing =
        scheduler.projected_document().get(edit.camera_node);
    if (existing == nullptr || existing->type != scene::schema::camera_type) {
        return false;
    }
    doc::Node updated = *existing;
    write_lens_profile(updated, edit.lens_profile);
    if (updated == *existing) {
        return true;
    }
    auto scheduled = scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, cmd::PutNode{*existing},
        cmd::UndoContextId{2U}, cmd::TransactionId{edit.gesture}, apply_at);
    return static_cast<bool>(scheduled);
}

// Authors the project-wide effect settings. Editing an existing node keeps its
// id so the undo restores exactly what was there; a project that never opened
// the dialog has no node at all, and one is created rather than assumed - the
// same provenance rule the synchronization policy follows. A camera-owned lens
// calibration shares the gesture but remains a separate document node.
[[nodiscard]] bool
schedule_project_settings(cmd::DirectorScheduler &scheduler,
                          const editor::ProjectSettingsEdit &edit,
                          doc::NodeId new_id, cmd::FrameNumber apply_at) {
    if (edit.camera_node == doc::NodeId{} && edit.lens_profile.has_value()) {
        return false;
    }
    if (edit.camera_node != doc::NodeId{}) {
        const doc::Node* const camera =
            scheduler.projected_document().get(edit.camera_node);
        if (camera == nullptr || camera->type != scene::schema::camera_type) {
            return false;
        }
    }
    const scene::EffectSettings settings = scene::clamped(edit.settings);
    doc::Node node{};
    node.type = std::string{scene::schema::settings_type};
    node.properties.emplace(
        std::string{scene::schema::blur_taps},
        doc::PropertyValue{static_cast<std::int64_t>(settings.blur_taps)});
    node.properties.emplace(
        std::string{scene::schema::blur_sigma_ratio},
        doc::PropertyValue{static_cast<double>(settings.blur_sigma_ratio)});
    node.properties.emplace(
        std::string{scene::schema::blur_passes},
        doc::PropertyValue{static_cast<std::int64_t>(settings.blur_passes)});
    node.properties.emplace(std::string{scene::schema::directional_blur_taps},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                settings.directional_blur_taps)});
    node.properties.emplace(
        std::string{scene::schema::brightness_curve},
        doc::PropertyValue{std::string{settings.brightness_curve ==
                                               scene::BrightnessCurve::Gain
                                           ? scene::schema::gain_curve
                                           : scene::schema::exposure_curve}});
    node.properties.emplace(
        std::string{scene::schema::brightness_gamma},
        doc::PropertyValue{static_cast<double>(settings.brightness_gamma)});
    node.properties.emplace(
        std::string{scene::schema::graph_texture_budget_gb},
        doc::PropertyValue{
            static_cast<double>(settings.graph_texture_budget_gb)});

    // The lighting environment lives on the stage node rather than on the
    // settings node this function builds, so it is written as its own update.
    // A zero stage node means the document has none and there is nothing to
    // write to - this dialog does not create one.
    if (edit.stage_node != doc::NodeId{}) {
        const doc::Node* const existing_stage =
            scheduler.projected_document().get(edit.stage_node);
        if (existing_stage != nullptr
            && existing_stage->type == scene::schema::stage_type) {
            doc::Node stage = *existing_stage;
            for (const auto& [key, value] : scene::planar_reflection_properties(edit.environment.planar))
                stage.properties.insert_or_assign(key, value);
            for (const auto& [key, value] : scene::baked_lighting_properties(edit.environment.baked))
                stage.properties.insert_or_assign(key, value);
            stage.properties.insert_or_assign(
                std::string{scene::schema::environment_map},
                doc::PropertyValue{edit.environment.map_source});
            stage.properties.insert_or_assign(
                std::string{scene::schema::environment_map_intensity},
                doc::PropertyValue{
                    static_cast<double>(edit.environment.map_intensity)});
            stage.properties.insert_or_assign(
                std::string{scene::schema::environment_map_rotation},
                doc::PropertyValue{
                    static_cast<double>(edit.environment.map_rotation)});
            stage.properties.insert_or_assign(
                std::string{scene::schema::sky_r},
                doc::PropertyValue{
                    static_cast<double>(edit.environment.sky_color.x)});
            stage.properties.insert_or_assign(
                std::string{scene::schema::sky_g},
                doc::PropertyValue{
                    static_cast<double>(edit.environment.sky_color.y)});
            stage.properties.insert_or_assign(
                std::string{scene::schema::sky_b},
                doc::PropertyValue{
                    static_cast<double>(edit.environment.sky_color.z)});
            stage.properties.insert_or_assign(
                std::string{scene::schema::environment_fog_density},
                doc::PropertyValue{
                    static_cast<double>(edit.environment.fog_density)});
            stage.properties.insert_or_assign(
                std::string{scene::schema::fog_r},
                doc::PropertyValue{
                    static_cast<double>(edit.environment.fog_color.x)});
            stage.properties.insert_or_assign(
                std::string{scene::schema::fog_g},
                doc::PropertyValue{
                    static_cast<double>(edit.environment.fog_color.y)});
            stage.properties.insert_or_assign(
                std::string{scene::schema::fog_b},
                doc::PropertyValue{
                    static_cast<double>(edit.environment.fog_color.z)});
            stage.properties.insert_or_assign(
                std::string{scene::schema::environment_fog_anisotropy},
                doc::PropertyValue{
                    static_cast<double>(edit.environment.fog_anisotropy)});
            stage.properties.insert_or_assign(
                std::string{scene::schema::environment_fog_height},
                doc::PropertyValue{
                    static_cast<double>(edit.environment.fog_height)});
            stage.properties.insert_or_assign(
                std::string{scene::schema::environment_fog_height_falloff},
                doc::PropertyValue{static_cast<double>(
                    edit.environment.fog_height_falloff)});
            stage.properties.insert_or_assign(
                std::string{scene::schema::environment_fog_background_distance},
                doc::PropertyValue{static_cast<double>(
                    edit.environment.fog_background_distance)});
            const auto sky = scene::clamped(edit.environment.physical_sky);
            stage.properties.insert_or_assign(std::string{scene::schema::sky_physical_enabled}, doc::PropertyValue{sky.enabled});
            stage.properties.insert_or_assign(std::string{scene::schema::sky_time_of_day}, doc::PropertyValue{static_cast<double>(sky.time_of_day)});
            stage.properties.insert_or_assign(std::string{scene::schema::sky_latitude}, doc::PropertyValue{static_cast<double>(sky.latitude)});
            stage.properties.insert_or_assign(std::string{scene::schema::sky_north_rotation}, doc::PropertyValue{static_cast<double>(sky.north_rotation)});
            stage.properties.insert_or_assign(std::string{scene::schema::sky_haze}, doc::PropertyValue{static_cast<double>(sky.haze)});
            stage.properties.insert_or_assign(std::string{scene::schema::sky_solar_illuminance}, doc::PropertyValue{static_cast<double>(sky.solar_illuminance)});
            stage.properties.insert_or_assign(std::string{scene::schema::sky_angular_radius}, doc::PropertyValue{static_cast<double>(sky.angular_radius)});
            stage.properties.insert_or_assign(std::string{scene::schema::sky_ground_albedo}, doc::PropertyValue{static_cast<double>(sky.ground_albedo)});
            if (!scheduler.schedule_edit(
                    cmd::PutNode{std::move(stage)},
                    cmd::PutNode{*existing_stage}, cmd::UndoContextId{2U},
                    cmd::TransactionId{edit.gesture}, apply_at)) {
                return false;
            }
        }
    }

    const scene::RendererSettings renderer = scene::clamped(edit.renderer);
    // Emitted by the scene model, beside the parser that reads it back.
    scene::write_renderer_settings(node, renderer);

    const doc::Node *const existing =
        edit.node == doc::NodeId{}
            ? nullptr
            : scheduler.projected_document().get(edit.node);
    node.id = existing != nullptr ? edit.node : new_id;
    if (existing != nullptr && node == *existing) {
        return schedule_camera_lens_profile(scheduler, edit, apply_at);
    }
    const cmd::Payload inverse = existing != nullptr
                                     ? cmd::Payload{cmd::PutNode{*existing}}
                                     : cmd::Payload{cmd::EraseNode{new_id}};
    auto scheduled = scheduler.schedule_edit(
        cmd::PutNode{std::move(node)}, inverse, cmd::UndoContextId{2U},
        cmd::TransactionId{edit.gesture}, apply_at);
    return static_cast<bool>(scheduled)
        && schedule_camera_lens_profile(scheduler, edit, apply_at);
}

// A default node for a newly-added element. Objects sit at the origin with a
// neutral material; lights carry the same defaults the demo scene uses. The
// operator refines them through the Inspector afterwards.
// How many show frames a newly-placed clip occupies. A sequence or a video
// gets its own length converted into show frames exactly - a 24 fps clip on a
// 50 fps show is longer in show frames than it is in its own - and a still,
// which has no length of its own, gets the five seconds an editor conventional-
// ly gives one. Rounded up so the last source frame is always reachable.
[[nodiscard]] std::int64_t
placed_clip_duration(const editor::AddIntent &add,
                     show::FrameRate show_rate) noexcept {
    const auto count = static_cast<std::int64_t>(std::max(add.count, 1U));
    const auto show_numerator =
        static_cast<std::int64_t>(std::max(show_rate.numerator, core::u32{1}));
    const auto show_denominator = static_cast<std::int64_t>(
        std::max(show_rate.denominator, core::u32{1}));
    if (count <= 1) {
        constexpr std::int64_t still_seconds = 5;
        return std::max(
            (still_seconds * show_numerator + show_denominator - 1) /
                show_denominator,
            std::int64_t{1});
    }
    if (!add.frame_rate.has_value() || add.frame_rate->numerator == 0U ||
        add.frame_rate->denominator == 0U) {
        return count;
    }
    const auto source_numerator =
        static_cast<std::int64_t>(add.frame_rate->numerator);
    const auto source_denominator =
        static_cast<std::int64_t>(add.frame_rate->denominator);
    const std::int64_t numerator = count * show_numerator * source_denominator;
    const std::int64_t denominator = show_denominator * source_numerator;
    return std::max((numerator + denominator - 1) / denominator,
                    std::int64_t{1});
}

[[nodiscard]] doc::Node make_added_node(
    doc::NodeId id, const editor::AddIntent &add, show::FrameRate show_rate,
    const scene::IcvfxCameraSettings &plate_defaults) {
    doc::Node node{};
    node.id = id;
    const auto kind = add.kind;
    const auto real = [](double value) { return doc::PropertyValue{value}; };
    const auto whole = [](std::int64_t value) {
        return doc::PropertyValue{value};
    };
    const auto text = [](std::string_view value) {
        return doc::PropertyValue{std::string{value}};
    };
    const auto add_links = [&]() {
        if (add.scene != scene::SceneId{}) {
            node.properties.emplace(
                std::string{scene::schema::scene_id},
                whole(static_cast<std::int64_t>(add.scene.value())));
        }
        if (add.asset != doc::NodeId{}) {
            node.properties.emplace(
                std::string{scene::schema::asset_id},
                whole(static_cast<std::int64_t>(add.asset.value())));
        }
        if (add.position.has_value()) {
            node.properties.insert_or_assign(
                std::string{scene::schema::position_x},
                real(static_cast<double>(add.position->x)));
            node.properties.insert_or_assign(
                std::string{scene::schema::position_y},
                real(static_cast<double>(add.position->y)));
            node.properties.insert_or_assign(
                std::string{scene::schema::position_z},
                real(static_cast<double>(add.position->z)));
        }
        if (add.rotation.has_value()) {
            node.properties.insert_or_assign(
                std::string{scene::schema::rotation_yaw},
                real(static_cast<double>(add.rotation->x)));
            node.properties.insert_or_assign(
                std::string{scene::schema::rotation_pitch},
                real(static_cast<double>(add.rotation->y)));
            node.properties.insert_or_assign(
                std::string{scene::schema::rotation_roll},
                real(static_cast<double>(add.rotation->z)));
        }
    };
    if (kind == editor::AddKind::Model) {
        node.type = std::string{scene::schema::model_type};
        node.properties.emplace(std::string{scene::schema::source},
                                text(add.source));
        add_links();
        return node;
    }
    if (kind == editor::AddKind::ParticleEffect) {
        // Seed zero and starting at the head of the show: the node id already
        // makes each placement's particles its own.
        node.type = std::string{scene::schema::particle_effect_type};
        node.properties.emplace(std::string{scene::schema::source},
                                text(add.source));
        add_links();
        return node;
    }
    if (kind == editor::AddKind::Media) {
        node.type = std::string{scene::schema::media_type};
        node.properties.emplace(std::string{scene::schema::source},
                                text(add.source));
        node.properties.emplace(std::string{scene::schema::sequence_start},
                                whole(0));
        node.properties.emplace(
            std::string{scene::schema::sequence_count},
            whole(static_cast<std::int64_t>(std::max(add.count, 1U))));
        if (add.frame_rate.has_value()) {
            node.properties.emplace(
                std::string{scene::schema::source_rate_numerator},
                whole(static_cast<std::int64_t>(add.frame_rate->numerator)));
            node.properties.emplace(
                std::string{scene::schema::source_rate_denominator},
                whole(static_cast<std::int64_t>(add.frame_rate->denominator)));
        }
        // Two metres tall, and as wide as the source's aspect ratio, so a clip
        // is never stretched.
        const double aspect =
            add.aspect > 0.0F ? static_cast<double>(add.aspect) : (16.0 / 9.0);
        node.properties.emplace(std::string{scene::schema::scale_x},
                                real(2.0 * aspect));
        node.properties.emplace(std::string{scene::schema::scale_y}, real(2.0));
        // A new clip lands placed rather than looping forever, so it appears on
        // the timeline as a bar the operator can move, trim, and cut. Its
        // trim, its ramps, and its start are all zero: an untouched placement
        // of the whole source at the head of the show.
        node.properties.emplace(std::string{scene::schema::clip_start},
                                whole(0));
        node.properties.emplace(std::string{scene::schema::clip_duration},
                                whole(placed_clip_duration(add, show_rate)));
        // The Asset Viewer's marked range, dragged out: where it starts, how
        // long it plays and which source frame it opens on.
        if (add.timing.has_value() && add.timing->duration > 0 &&
            add.timing->source_in >= 0) {
            node.properties.insert_or_assign(
                std::string{scene::schema::clip_start},
                whole(add.timing->start));
            node.properties.insert_or_assign(
                std::string{scene::schema::clip_duration},
                whole(add.timing->duration));
            node.properties.insert_or_assign(
                std::string{scene::schema::source_in},
                whole(add.timing->source_in));
        }
        add_links();
        return node;
    }
    if (kind == editor::AddKind::Camera) {
        node.type = std::string{scene::schema::camera_type};
        // Three metres back at eye height, tipped down towards the stage
        // origin: where a camera on a volume actually stands, so a new one
        // frames something rather than landing inside the floor.
        node.properties.emplace(std::string{scene::schema::position_y},
                                real(1.6));
        node.properties.emplace(std::string{scene::schema::position_z},
                                real(3.0));
        node.properties.emplace(
            std::string{scene::schema::rotation_pitch},
            real(std::atan2(-1.6, 3.0) / 6.283185307179586));
        node.properties.emplace(std::string{scene::schema::fov_turns},
                                real(0.0625));
        // Seeded from the project's new-camera defaults, once. They are
        // defaults and not policy: from here the camera carries its own
        // values, and editing the project's never reaches back and changes a
        // camera an operator already tuned.
        const scene::IcvfxCameraSettings plate = scene::clamped(plate_defaults);
        node.properties.emplace(
            std::string{scene::schema::camera_inner_frustum_margin_px},
            whole(static_cast<std::int64_t>(plate.margin_px)));
        node.properties.emplace(
            std::string{scene::schema::camera_inner_frustum_soft_edge_px},
            whole(static_cast<std::int64_t>(plate.soft_edge_px)));
        node.properties.emplace(
            std::string{
                scene::schema::camera_inner_frustum_scale_numerator},
            whole(static_cast<std::int64_t>(plate.resolution_numerator)));
        node.properties.emplace(
            std::string{
                scene::schema::camera_inner_frustum_scale_denominator},
            whole(static_cast<std::int64_t>(plate.resolution_denominator)));
        // A new camera draws a plate. An operator who adds one and binds a
        // feed has said what they want; making them find a second switch
        // afterwards would be the surprise.
        node.properties.emplace(
            std::string{scene::schema::camera_inner_frustum},
            doc::PropertyValue{true});
        add_links();
        return node;
    }
    if (kind == editor::AddKind::TrackingSource) {
        node.type = std::string{scene::schema::tracking_source_type};
        node.properties.emplace(
            std::string{scene::schema::tracking_source_name},
            text("Tracker"));
        // Authored off, with no port. An operator sets both, and until they do
        // the source is honestly not listening rather than half-configured.
        node.properties.emplace(std::string{scene::schema::tracking_enabled},
                                doc::PropertyValue{false});
        node.properties.emplace(std::string{scene::schema::tracking_protocol},
                                text(scene::schema::freed_protocol));
        add_links();
        return node;
    }
    if (kind == editor::AddKind::DirectionalLight ||
        kind == editor::AddKind::PointLight ||
        kind == editor::AddKind::SpotLight) {
        node.type = std::string{scene::schema::light_type};
        node.properties.emplace(std::string{scene::schema::color_r}, real(1.0));
        node.properties.emplace(std::string{scene::schema::color_g}, real(1.0));
        node.properties.emplace(std::string{scene::schema::color_b}, real(1.0));
        if (kind == editor::AddKind::PointLight ||
            kind == editor::AddKind::SpotLight) {
            const bool spot = kind == editor::AddKind::SpotLight;
            node.properties.emplace(
                std::string{scene::schema::light_kind},
                text(spot ? scene::schema::spot_kind : "point"));
            node.properties.emplace(std::string{scene::schema::position_y},
                                    real(3.0));
            node.properties.emplace(std::string{scene::schema::range},
                                    real(25.0));
            node.properties.emplace(std::string{scene::schema::intensity},
                                    real(12.0));
            if (spot) {
                // Pointing down, with a cone wide enough to read as a pool of
                // light rather than a pinprick. Casting by default: a spot
                // that lights through the wall it is aimed at is exactly what
                // this exists to stop, and an operator who cannot afford the
                // tile turns it off.
                node.properties.emplace(
                    std::string{scene::schema::direction_x}, real(0.0));
                node.properties.emplace(
                    std::string{scene::schema::direction_y}, real(-1.0));
                node.properties.emplace(
                    std::string{scene::schema::direction_z}, real(0.0));
                node.properties.emplace(
                    std::string{scene::schema::inner_cone_turns}, real(0.06));
                node.properties.emplace(
                    std::string{scene::schema::outer_cone_turns}, real(0.09));
                node.properties.emplace(
                    std::string{scene::schema::casts_shadow},
                    doc::PropertyValue{true});
                node.properties.emplace(
                    std::string{scene::schema::shadow_tier},
                    text(scene::schema::medium_tier));
            }
        } else {
            node.properties.emplace(std::string{scene::schema::light_kind},
                                    text("directional"));
            node.properties.emplace(std::string{scene::schema::direction_x},
                                    real(-0.3));
            node.properties.emplace(std::string{scene::schema::direction_y},
                                    real(-1.0));
            node.properties.emplace(std::string{scene::schema::direction_z},
                                    real(-0.4));
            node.properties.emplace(std::string{scene::schema::intensity},
                                    real(2.5));
        }
        add_links();
        return node;
    }

    node.type = std::string{scene::schema::object_type};
    if (kind == editor::AddKind::Background) {
        // A 16:9 card two metres tall standing on the floor, facing the
        // camera like a screen. Its colour is all emission over a black,
        // non-reflective base, so the fill reads as itself rather than as a
        // painted board picking up the lights; a gradient from a dark top to
        // a lighter horizon so a new one is visibly a gradient.
        node.properties.emplace(std::string{scene::schema::primitive},
                                text("quad"));
        node.properties.emplace(std::string{scene::schema::group_name},
                                text("Background"));
        node.properties.emplace(std::string{scene::schema::position_y},
                                real(1.0));
        node.properties.emplace(std::string{scene::schema::scale_x},
                                real(2.0 * 16.0 / 9.0));
        node.properties.emplace(std::string{scene::schema::scale_y}, real(2.0));
        for (const std::string_view channel :
             {scene::schema::color_r, scene::schema::color_g,
              scene::schema::color_b}) {
            node.properties.emplace(std::string{channel}, real(0.0));
        }
        for (const std::string_view channel :
             {scene::schema::emissive_r, scene::schema::emissive_g,
              scene::schema::emissive_b}) {
            node.properties.emplace(std::string{channel}, real(1.0));
        }
        node.properties.emplace(std::string{scene::schema::roughness},
                                real(1.0));
        node.properties.emplace(std::string{scene::schema::specular},
                                real(0.0));
        node.properties.emplace(std::string{scene::schema::double_sided},
                                doc::PropertyValue{true});
        node.properties.emplace(std::string{scene::schema::fill_mode},
                                text(scene::schema::linear_fill));
        const std::pair<std::string_view, double> fill[]{
            {scene::schema::fill_a_r, 0.10}, {scene::schema::fill_a_g, 0.12},
            {scene::schema::fill_a_b, 0.18}, {scene::schema::fill_a_a, 1.0},
            {scene::schema::fill_b_r, 0.46}, {scene::schema::fill_b_g, 0.50},
            {scene::schema::fill_b_b, 0.56}, {scene::schema::fill_b_a, 1.0},
            {scene::schema::fill_angle_turns, 0.25},
            {scene::schema::fill_center_x, 0.5},
            {scene::schema::fill_center_y, 0.5},
            {scene::schema::fill_size, 1.0}};
        for (const auto& [property, value] : fill) {
            node.properties.emplace(std::string{property}, real(value));
        }
        add_links();
        return node;
    }
    const std::string_view primitive =
        kind == editor::AddKind::Plane
            ? "plane"
            : (kind == editor::AddKind::Sphere ? "sphere" : "box");
    node.properties.emplace(std::string{scene::schema::primitive},
                            text(primitive));
    node.properties.emplace(std::string{scene::schema::color_r}, real(0.60));
    node.properties.emplace(std::string{scene::schema::color_g}, real(0.60));
    node.properties.emplace(std::string{scene::schema::color_b}, real(0.63));
    add_links();
    return node;
}

// Creates one element: a PutNode whose inverse erases it, on the scene undo
// context, so an add undoes cleanly and replicates like any other edit.
[[nodiscard]] bool schedule_add(
    cmd::DirectorScheduler &scheduler, const editor::AddIntent &add,
    doc::NodeId id, cmd::FrameNumber apply_at, show::FrameRate show_rate,
    const scene::IcvfxCameraSettings &plate_defaults,
    cmd::UndoContextId undo_context) {
    auto scheduled = scheduler.schedule_edit(
        cmd::PutNode{make_added_node(id, add, show_rate, plate_defaults)},
        cmd::EraseNode{id}, undo_context,
        cmd::TransactionId{add.gesture}, apply_at);
    return static_cast<bool>(scheduled);
}

[[nodiscard]] bool
schedule_asset_import(cmd::DirectorScheduler &scheduler,
                      const editor::AssetImportIntent &request, doc::NodeId id,
                      cmd::FrameNumber apply_at) {
    ProjectAssetNodeUpsert upsert = make_project_asset_upsert(
        scheduler.projected_document(), request.asset, id);
    if (upsert.before.has_value()) {
        return static_cast<bool>(scheduler.schedule_edit(
            cmd::PutNode{std::move(upsert.after)},
            cmd::PutNode{std::move(upsert.before.value())},
            cmd::UndoContextId{4U}, cmd::TransactionId{request.gesture},
            apply_at));
    }
    const doc::NodeId target = upsert.after.id;
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(upsert.after)}, cmd::EraseNode{target},
        cmd::UndoContextId{4U}, cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] std::string_view
project_asset_import_error_label(ProjectAssetImportErrorCode code) noexcept {
    switch (code) {
    case ProjectAssetImportErrorCode::InvalidSource:
        return "SOURCE IS NOT AN ABSOLUTE PATH";
    case ProjectAssetImportErrorCode::SourceMissing:
        return "SOURCE COULD NOT BE OPENED";
    case ProjectAssetImportErrorCode::FingerprintFailed:
        return "SOURCE METADATA COULD NOT BE READ";
    }
    return "UNKNOWN ERROR";
}

[[nodiscard]] bool
schedule_asset_delete(cmd::DirectorScheduler &scheduler,
                      const editor::AssetDeleteIntent &request,
                      cmd::FrameNumber apply_at) {
    const doc::Node *const existing =
        scheduler.projected_document().get(request.asset);
    if (existing == nullptr) {
        return true;
    }
    if (existing->type != assets::schema::media_type &&
        existing->type != assets::schema::model_type &&
        existing->type != assets::schema::projection_mesh_type &&
        existing->type != assets::schema::plateset_type &&
        existing->type != assets::schema::particle_effect_type) {
        // A stale or malformed UI request must never erase a non-asset node.
        return true;
    }
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::EraseNode{request.asset}, cmd::PutNode{*existing},
        // Library removal is a direct destructive UI action, so keep it in
        // the editor's globally reachable Undo context. Undo restores the
        // complete asset node without touching any scene placement.
        cmd::UndoContextId{2U}, cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] bool
schedule_asset_proxy_edit(cmd::DirectorScheduler &scheduler,
                          const editor::AssetProxyEditIntent &request,
                          cmd::FrameNumber apply_at) {
    const doc::Node *const existing =
        scheduler.projected_document().get(request.asset);
    if (existing == nullptr || existing->type != assets::schema::media_type ||
        !assets::valid_proxy_playback_policy(request.policy)) {
        return true;
    }
    doc::Node changed = *existing;
    if (request.policy == assets::ProxyPlaybackPolicy::Source) {
        changed.properties.erase(
            std::string{assets::schema::requested_proxy_profile});
    } else {
        if (!request.requested_profile.has_value() ||
            !assets::valid_requested_proxy_profile(
                *request.requested_profile)) {
            return true;
        }
        const assets::DerivativeProfile *const profile =
            assets::derivative_profile(*request.requested_profile);
        if (profile == nullptr) {
            return true;
        }
        changed.properties.insert_or_assign(
            std::string{assets::schema::requested_proxy_profile},
            doc::PropertyValue{std::string{profile->name}});
    }
    changed.properties.insert_or_assign(
        std::string{assets::schema::proxy_playback_policy},
        doc::PropertyValue{
            std::string{assets::proxy_playback_policy_name(request.policy)}});
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(changed)}, cmd::PutNode{*existing},
        cmd::UndoContextId{2U}, cmd::TransactionId{request.gesture}, apply_at));
}

bool schedule_asset_library_edit(
    cmd::DirectorScheduler &scheduler,
    const editor::AssetLibraryEditIntent &request, doc::NodeId new_folder,
    cmd::FrameNumber apply_at, scene::MediaFrameRate show_rate) {
    std::optional<doc::Root> planned = assets::plan_library_edit(
        scheduler.projected_document(), request.edit, new_folder);
    if (!planned.has_value()) {
        return true;
    }
    // Placed takes stay in step with their set: a plate moved to another
    // slot, a sync offset or the decode size reaches every take of it, in
    // the same undo step.
    if (request.edit.kind == assets::LibraryEdit::Kind::SetPlatesetSlot ||
        request.edit.kind == assets::LibraryEdit::Kind::SetPlatesetOffsets ||
        request.edit.kind ==
            assets::LibraryEdit::Kind::SetPlatesetHalfResolution) {
        planned = follow_plateset(std::move(*planned), request.edit.target,
                                  show_rate);
    }
    // Organising the Library is a direct UI action, undone from the same
    // globally reachable context as removing an asset.
    return static_cast<bool>(scheduler.schedule_document_replace(
        *planned, cmd::UndoContextId{2U},
        cmd::TransactionId{request.gesture}, apply_at));
}

// Deletes one element: an EraseNode whose inverse restores it. A missing node
// is a no-op. All of a multi-select delete share the gesture, so it undoes at
// once.
[[nodiscard]] bool schedule_delete(cmd::DirectorScheduler &scheduler,
                                   doc::NodeId node, core::u64 gesture,
                                   cmd::FrameNumber apply_at) {
    const auto *const existing = scheduler.projected_document().get(node);
    if (existing == nullptr) {
        return true;
    }
    // Collection removal is the explicit, asset-preserving Unlink operation.
    if (existing->type == scene::link_schema::instance_type) return true;
    // A Blender object is deleted in Blender too: writeback removes it there.
    if (scene::is_linked_occurrence(*existing)) {
        auto plan = scene::plan_linked_edit(scheduler.projected_document(), node, scene::LinkedEdit::Delete);
        if (!plan) return true;
        return static_cast<bool>(scheduler.schedule_document_replace(plan.value(),
            cmd::UndoContextId{2U}, cmd::TransactionId{gesture}, apply_at));
    }
    auto scheduled = scheduler.schedule_edit(
        cmd::EraseNode{node}, cmd::PutNode{*existing}, cmd::UndoContextId{2U},
        cmd::TransactionId{gesture}, apply_at);
    return static_cast<bool>(scheduled);
}

// Authors one operator move as an ordinary command: read the node's current
// authored state, set its position, and schedule a PutNode whose inverse
// restores the old one. A separate undo context from the transport's, so
// undoing a move never touches playback. The editor produced the position; this
// is the only place a document changes.
[[nodiscard]] bool schedule_scene_edit(cmd::DirectorScheduler &scheduler,
                                       const editor::EditIntent &edit,
                                       cmd::FrameNumber apply_at) {
    const auto *const existing = scheduler.projected_document().get(edit.node);
    if (existing == nullptr) {
        // The node is gone; there is nothing to move and nothing to fail.
        return true;
    }
    doc::Node updated = *existing;
    if (scene::is_linked_occurrence(*existing)) {
        auto parsed = scene::read_linked_scene(scheduler.projected_document());
        if (!parsed || !parsed.value().occurrences.contains(edit.node)) return true;
        auto world = parsed.value().occurrences.at(edit.node).world;
        world.m[12] = edit.position.x; world.m[13] = edit.position.y; world.m[14] = edit.position.z;
        auto plan = scene::plan_linked_world(scheduler.projected_document(), edit.node, world);
        if (!plan) return true;
        return static_cast<bool>(scheduler.schedule_edit(cmd::PutNode{*plan.value().get(edit.node)},
            cmd::PutNode{*existing}, cmd::UndoContextId{2U}, cmd::TransactionId{edit.gesture}, apply_at));
    }
    updated.properties.insert_or_assign(
        std::string{scene::schema::position_x},
        doc::PropertyValue{static_cast<double>(edit.position.x)});
    updated.properties.insert_or_assign(
        std::string{scene::schema::position_y},
        doc::PropertyValue{static_cast<double>(edit.position.y)});
    updated.properties.insert_or_assign(
        std::string{scene::schema::position_z},
        doc::PropertyValue{static_cast<double>(edit.position.z)});

    // The transaction is the drag gesture, not the frame: every sealed frame of
    // one continuous drag appends to the same transaction, so the clip moves on
    // the nodes each frame but the whole drag undoes as a single step. A fresh
    // gesture (the next drag) starts a new transaction and becomes the undo
    // top.
    auto scheduled = scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, cmd::PutNode{*existing},
        cmd::UndoContextId{2U}, cmd::TransactionId{edit.gesture}, apply_at);
    return static_cast<bool>(scheduled);
}

// Authors a viewport rotate, scale or clear on a Blender-linked object: its
// whole new world placement, planned as the occurrence's override exactly as a
// move is. Anything that is not a linked occurrence is placed by its own
// transform fields, which the editor edits directly instead.
[[nodiscard]] bool schedule_world_edit(cmd::DirectorScheduler &scheduler,
                                       const editor::WorldEdit &edit,
                                       cmd::FrameNumber apply_at) {
    const auto *const existing = scheduler.projected_document().get(edit.node);
    if (existing == nullptr || !scene::is_linked_occurrence(*existing)) {
        return true;
    }
    auto plan = scene::plan_linked_world(scheduler.projected_document(),
                                         edit.node, edit.world);
    if (!plan) {
        std::cerr << "Linked scene placement: " << plan.error().message << '\n';
        return true;
    }
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{*plan.value().get(edit.node)}, cmd::PutNode{*existing},
        cmd::UndoContextId{2U}, cmd::TransactionId{edit.gesture}, apply_at));
}

} // namespace mediaserver::app
