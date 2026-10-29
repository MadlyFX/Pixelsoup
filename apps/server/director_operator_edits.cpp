// Queued editor edits authored at the frame boundary, one phase per domain.

#include "director_show_loop.hpp"

#include "clip_clipboard.hpp"
#include "director_clip_authoring.hpp"
#include "plateset_authoring.hpp"
#include "director_compositor_authoring.hpp"
#include "director_effect_authoring.hpp"
#include "director_feed_map_authoring.hpp"
#include "director_hardware_authoring.hpp"
#include "director_key_authoring.hpp"
#include "director_scene_authoring.hpp"
#include "key_clipboard.hpp"
#include "scene_duplicate.hpp"
#include "source_range_edit.hpp"
#include "mask_edit_integration.hpp"
#include "project_asset_import.hpp"
#include "source_replacement_authoring.hpp"
#include "timeline_edit_integration.hpp"
#include "timeline_lane_authoring.hpp"
#include "timeline_marker_authoring.hpp"

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/compositor/compositor_model.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/editor/mapping_panels.hpp>
#include <mediaserver/editor/timeline_panel.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/key_edit.hpp>
#include <mediaserver/scene/linked_scene.hpp>
#include <mediaserver/scene/mask_authoring.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_project.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/scene/selection_set.hpp>
#include <mediaserver/topology/hardware_map.hpp>

#include <algorithm>
#include <iostream>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mediaserver::app {

PhaseExit DirectorShowLoop::apply_scene_edits(OperatorFrame &op) {
    const auto &frame = op.frame;
    auto &scene_edits_locked = op.scene_edits_locked;
    if (scene_edits_locked) {
        if (!pending_edits.empty() || !pending_world_edits.empty() || !pending_properties.empty() || !pending_generics.empty() || !pending_locks.empty())
            project_message="Scene edits wait for the running dissolve to finish.";
        pending_locks.clear();
        pending_edits.clear();
        pending_world_edits.clear();
        pending_keys.clear();
        pending_key_moves.clear();
        pending_key_duplicate.reset();
        pending_key_batch.reset();
        key_scale_preview.reset();
        pending_clip_timings.clear();
        pending_clip_splits.clear();
        pending_plateset_places.clear();
        pending_plateset_swaps.clear();
        pending_lane_edits.clear();
        pending_marker_edits.clear();
        pending_layer_orders.clear();
        pending_element_flags.clear();
        pending_renames.clear();
        pending_highlights.clear();
        pending_timeline_edits.clear();
        pending_source_edits.clear();
        pending_group_edit.reset();
        pending_selection_set_edit.reset();
        pending_mask_edit.reset();
        pending_clip_paste.reset();
        pending_scene_duplicates.clear();
        pending_key_paste.reset();
        pending_project_settings.reset();
        pending_properties.clear();
        pending_generics.clear();
        pending_adds.clear();
        pending_effects.clear();
        pending_film_grain_effect_edits.clear();
        pending_effect_deletes.clear();
        pending_deletes.clear();
        pending_compositor_adds.clear();
        pending_source_replacements.clear();
        pending_compositor_moves.clear();
        pending_compositor_clipboard.reset();
        pending_compositor_rename.reset();
        pending_compositor_comment.reset();
        pending_compositor_collapse.reset();
        pending_compositor_bypass.reset();
        pending_compositor_reorder.reset();
        pending_compositor_connect.reset();
        pending_compositor_rewire.reset();
        pending_compositor_waypoints.reset();
        pending_compositor_property.reset();
        pending_compositor_delete.reset();
        pending_history.reset();
    }
    for (const auto &edit : pending_locks) {
        if (!schedule_lock_edit(scheduler, edit, frame)) {
            std::cerr
                << "could not author an element lock for frame "
                << frame.value() << '\n';
            return 1;
        }
    }
    pending_locks.clear();
    for (const auto &edit : pending_edits) {
        if (!schedule_scene_edit(scheduler, edit, frame)) {
            std::cerr << "could not author a scene edit for frame "
                      << frame.value() << '\n';
            return 1;
        }
    }
    pending_edits.clear();
    for (const auto &edit : pending_world_edits) {
        if (!schedule_world_edit(scheduler, edit, frame)) {
            std::cerr << "could not author a linked placement for frame "
                      << frame.value() << '\n';
            return 1;
        }
    }
    pending_world_edits.clear();
    for (const auto &key : pending_keys) {
        if (!schedule_key_edit(scheduler, key, frame,
                               coalescing_for(key.gesture))) {
            std::cerr << "could not author a keyframe for frame "
                      << frame.value() << '\n';
            return 1;
        }
    }
    pending_keys.clear();
    return std::nullopt;
}

PhaseExit DirectorShowLoop::apply_asset_imports(OperatorFrame &op) {
    const auto &frame = op.frame;
    // Import is a live path reference plus a metadata-only source
    // revision. No media payload bytes are read on this loop.
    for (auto &imported : pending_asset_imports) {
        auto prepared =
            prepare_project_asset_import(std::move(imported.asset));
        if (!prepared) {
            discard_gesture_requests(pending_feed_surface_adds,
                                     imported.gesture);
            discard_gesture_requests(pending_feed_surface_edits,
                                     imported.gesture);
            const std::string_view label =
                project_asset_import_error_label(
                    prepared.error().code);
            project_message = "ASSET IMPORT FAILED - ";
            project_message.append(label);
            std::cerr << "asset import rejected: " << label << '\n';
            continue;
        }

        PendingAssetImportAuthoring authoring{};
        authoring.gesture = imported.gesture;
        try {
            extract_gesture_requests(pending_feed_surface_adds,
                                     imported.gesture,
                                     authoring.surface_adds);
            extract_gesture_requests(pending_feed_surface_edits,
                                     imported.gesture,
                                     authoring.surface_edits);
        } catch (const std::bad_alloc &) {
            discard_gesture_requests(pending_feed_surface_adds,
                                     imported.gesture);
            discard_gesture_requests(pending_feed_surface_edits,
                                     imported.gesture);
            project_message =
                "ASSET IMPORT FAILED - NOT ENOUGH MEMORY";
            std::cerr << "could not queue an asset import: "
                         "allocation failed\n";
            continue;
        }

        while (scheduler.projected_document().contains(
            doc::NodeId{next_new_node})) {
            ++next_new_node;
        }
        const doc::NodeId asset_id{next_new_node++};
        const editor::AssetImportIntent verified{
            std::move(prepared).value(), imported.gesture};
        if (!schedule_asset_import(scheduler, verified, asset_id,
                                   frame)) {
            project_message =
                "ASSET IMPORT FAILED - SCHEDULER REJECTED ASSET";
            std::cerr
                << "could not author a completed asset import "
                   "for frame "
                << frame.value() << '\n';
            continue;
        }

        bool companion_failed = false;
        for (const auto &request : authoring.surface_edits) {
            if (!schedule_feed_surface_edit(scheduler, request,
                                            frame)) {
                companion_failed = true;
                break;
            }
        }
        if (!companion_failed) {
            for (const auto &request : authoring.surface_adds) {
                while (scheduler.projected_document().contains(
                    doc::NodeId{next_new_node})) {
                    ++next_new_node;
                }
                const doc::NodeId surface_id{next_new_node++};
                if (!schedule_feed_surface_add(scheduler, request,
                                               surface_id, frame)) {
                    companion_failed = true;
                    break;
                }
                if (request.assign_map_node.has_value()) {
                    const editor::FeedProjectionEdit assignment{
                        *request.assign_map_node,
                        output::FeedProjection::PlanarSurface,
                        surface_id,
                        scene::whole(request.surface.raster),
                        request.gesture};
                    if (!schedule_feed_projection_edit(
                            scheduler, assignment, frame)) {
                        companion_failed = true;
                        break;
                    }
                }
            }
        }
        if (companion_failed) {
            project_message =
                "ASSET IMPORTED - SURFACE UPDATE FAILED";
            std::cerr << "could not author the projection surface "
                         "paired with an asset import for frame "
                      << frame.value() << '\n';
        } else {
            project_message = "ASSET IMPORTED";
        }
    }
    pending_asset_imports.clear();

    // Accepted changed sources. Fingerprinting is a stat; the editor's
    // workers already did the probing.
    std::size_t revised = 0U;
    std::size_t refused = 0U;
    for (auto &revision : pending_asset_revisions) {
        const doc::NodeId id = assets::asset_id(revision.asset);
        auto prepared = prepare_project_asset_import(std::move(revision.asset));
        if (!prepared) {
            ++refused;
            continue;
        }
        auto plan = plan_project_asset_revision(
            scheduler.projected_document(), id, prepared.value(),
            ProjectAssetRevisionMode::AcceptCurrentSource);
        if (!plan) {
            ++refused;
            continue;
        }
        if (!plan.value().modified) {
            continue;
        }
        if (!scheduler.schedule_edit(
                cmd::PutNode{std::move(plan.value().changed)},
                cmd::PutNode{std::move(plan.value().before)},
                cmd::UndoContextId{2U},
                cmd::TransactionId{revision.gesture}, frame)) {
            ++refused;
            continue;
        }
        ++revised;
    }
    if (revised > 0U || refused > 0U) {
        project_message =
            refused == 0U
                ? std::string{"ACCEPTED "} + std::to_string(revised) +
                      (revised == 1U ? " CHANGED SOURCE" : " CHANGED SOURCES")
                : std::string{"COULD NOT ACCEPT "} + std::to_string(refused) +
                      (refused == 1U ? " SOURCE" : " SOURCES") +
                      " - CHECK THE FILE IS STILL THERE";
    }
    pending_asset_revisions.clear();
    return std::nullopt;
}

PhaseExit DirectorShowLoop::apply_key_requests(OperatorFrame &op) {
    const auto &frame = op.frame;
    // A mixed surface drag can emit animated-axis keys and one
    // masked base edit for the remaining axes. Schedule that base
    // immediately after its keys so their shared scene transaction
    // stays on top and undoes atomically.
    for (const auto &request : pending_feed_surface_edits) {
        if (!schedule_feed_surface_edit(scheduler, request,
                                        frame)) {
            std::cerr
                << "could not edit a projection surface for frame "
                << frame.value() << '\n';
            return 1;
        }
    }
    pending_feed_surface_edits.clear();
    for (const auto &moved : pending_key_moves) {
        if (!schedule_key_move(scheduler, moved, frame)) {
            std::cerr
                << "could not author a keyframe move for frame "
                << frame.value() << '\n';
            return 1;
        }
    }
    pending_key_moves.clear();
    if (pending_key_batch.has_value()) {
        const editor::KeyBatchRequest request =
            std::move(*pending_key_batch);
        pending_key_batch.reset();
        const std::vector<scene::KeyAddress> selection =
            scene_key_addresses(request.edit.keys);
        scene::KeyEditResult planned = [&]() {
            switch (request.edit.action) {
            case editor::TimelineKeyBatchAction::Move:
                return scene::plan_key_move(
                    scheduler.projected_document(), selection,
                    request.edit.delta);
            case editor::TimelineKeyBatchAction::Duplicate:
                return scene::plan_key_duplicate(
                    scheduler.projected_document(), selection,
                    request.edit.delta);
            case editor::TimelineKeyBatchAction::Delete:
                return scene::plan_key_delete(
                    scheduler.projected_document(), selection);
            case editor::TimelineKeyBatchAction::Scale:
                return scene::plan_key_scale(
                    scheduler.projected_document(), selection,
                    request.edit.scale);
            case editor::TimelineKeyBatchAction::Ease:
                return scene::plan_key_ease(
                    scheduler.projected_document(), selection,
                    request.edit.ease);
            case editor::TimelineKeyBatchAction::Tangent:
                if (selection.size() != 1U) {
                    return scene::KeyEditResult::failure(
                        scene::KeyEditError::InvalidTangent);
                }
                return scene::plan_key_tangent(
                    scheduler.projected_document(), selection.front(),
                    request.edit.tangent);
            }
            return scene::KeyEditResult::failure(
                scene::KeyEditError::InvalidScale);
        }();
        if (!planned) {
            switch (planned.error()) {
            case scene::KeyEditError::InvalidScale:
                project_message = "KEY SCALE REJECTED - "
                                  "DENOMINATOR MUST BE POSITIVE";
                break;
            case scene::KeyEditError::IntegerOverflow:
                project_message = "KEY EDIT REJECTED - DESTINATION "
                                  "FRAME OVERFLOW";
                break;
            case scene::KeyEditError::InvalidTangent:
                project_message = "KEY TANGENT REJECTED - SLOPES MUST BE "
                                  "FINITE AND WEIGHTS POSITIVE";
                break;
            case scene::KeyEditError::AllocationFailed:
                std::cerr << "could not allocate an atomic key "
                             "edit plan\n";
                return 1;
            }
        } else {
            scene::KeyEditPlan plan = std::move(planned).value();
            const doc::Delta delta = doc::diff(
                scheduler.projected_document(), plan.after);
            if (!delta.changes.empty()) {
                auto scheduled =
                    scheduler.schedule_document_replace(
                        plan.after, cmd::UndoContextId{2U},
                        cmd::TransactionId{request.transaction},
                        frame);
                if (!scheduled) {
                    std::cerr << "could not author an atomic key "
                                 "edit for frame "
                              << frame.value() << '\n';
                    return 1;
                }
            }
            std::vector<editor::TimelineKeyAddress> selection_after;
            selection_after.reserve(plan.selection_after.size());
            for (const scene::KeyRecord &key :
                 plan.selection_after) {
                selection_after.push_back(
                    editor::TimelineKeyAddress{key.address.target,
                                               key.address.property,
                                               key.address.frame});
            }
            if (!editor->apply_key_edit_selection(
                    request.scene, selection_after)) {
                project_message = "KEY EDIT APPLIED - LOCAL "
                                  "SELECTION COULD NOT BE UPDATED";
            } else {
                project_message = key_batch_message(
                    request.edit.action, plan.report);
            }
        }
    }
    if (pending_key_duplicate.has_value()) {
        try {
            const std::vector<KeyAddress> selection =
                key_addresses(pending_key_duplicate->selection);
            auto planned = duplicate_keys(
                scheduler.projected_document(),
                pending_key_duplicate->scene, selection,
                pending_key_duplicate->delta);
            if (planned) {
                const KeyEditReport &report =
                    planned.value().report;
                if (report.applied != 0U) {
                    auto scheduled =
                        scheduler.schedule_document_replace(
                            planned.value().document,
                            cmd::UndoContextId{2U},
                            cmd::TransactionId{
                                pending_key_duplicate->transaction},
                            frame);
                    if (!scheduled) {
                        std::cerr
                            << "could not author a key duplicate "
                               "for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                project_message = key_operation_message(
                    "DUPLICATE", report.applied, "APPLIED",
                    report.locked_targets, report.incompatible);
            } else {
                project_message =
                    "KEY DUPLICATE REJECTED - FRAME OUT OF RANGE";
            }
        } catch (const std::bad_alloc &) {
            std::cerr << "could not plan a key duplicate\n";
            return 1;
        }
        pending_key_duplicate.reset();
    }
    if (pending_key_paste.has_value()) {
        try {
            auto planned = pending_key_paste->snapshot.paste(
                scheduler.projected_document(),
                pending_key_paste->request.scene,
                pending_key_paste->request.playhead);
            if (planned) {
                const KeyEditReport &report =
                    planned.value().report;
                if (report.applied != 0U) {
                    auto scheduled =
                        scheduler.schedule_document_replace(
                            planned.value().document,
                            cmd::UndoContextId{2U},
                            cmd::TransactionId{
                                pending_key_paste->request
                                    .transaction},
                            frame);
                    if (!scheduled) {
                        std::cerr
                            << "could not author a key paste for "
                               "frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                project_message = key_operation_message(
                    "PASTE", report.applied, "APPLIED",
                    report.locked_targets, report.incompatible);
            } else if (planned.error() == KeyPasteError::Empty) {
                project_message = "KEY PASTE - CLIPBOARD EMPTY";
            } else {
                project_message =
                    "KEY PASTE REJECTED - FRAME OUT OF RANGE";
            }
        } catch (const std::bad_alloc &) {
            std::cerr << "could not plan a key paste\n";
            return 1;
        }
        pending_key_paste.reset();
    }
    return std::nullopt;
}

PhaseExit DirectorShowLoop::apply_clip_requests(OperatorFrame &op) {
    const auto &frame = op.frame;
    for (const auto &edit : pending_clip_timings) {
        if (!schedule_clip_timing(scheduler, edit, frame)) {
            std::cerr
                << "could not author a clip placement for frame "
                << frame.value() << '\n';
            return 1;
        }
    }
    pending_clip_timings.clear();
    for (const auto &edit : pending_layer_orders) {
        const scene::LayerOrderPlacement placement =
            edit.placement == editor::TimelineLayerPlacement::Before
                ? scene::LayerOrderPlacement::Before
                : scene::LayerOrderPlacement::After;
        auto planned = scene::plan_reorder_layer(
            scheduler.projected_document(), edit.scene, edit.layer,
            edit.destination, placement);
        if (!planned) {
            if (planned.error() ==
                scene::LayerOrderError::AllocationFailed) {
                std::cerr
                    << "could not allocate a layer reorder plan\n";
                return 1;
            }
            project_message = "LAYER REORDER REJECTED";
            continue;
        }
        const doc::Delta delta = doc::diff(
            scheduler.projected_document(), planned.value().after);
        if (!delta.changes.empty()) {
            auto scheduled = scheduler.schedule_document_replace(
                planned.value().after, cmd::UndoContextId{2U},
                cmd::TransactionId{edit.gesture}, frame);
            if (!scheduled) {
                std::cerr
                    << "could not author a layer reorder for frame "
                    << frame.value() << '\n';
                return 1;
            }
        }
    }
    pending_layer_orders.clear();
    for (const auto &split : pending_clip_splits) {
        while (scheduler.projected_document().contains(
            doc::NodeId{next_new_node})) {
            ++next_new_node;
        }
        const doc::NodeId id{next_new_node++};
        if (!schedule_clip_split(scheduler, split, id, frame,
                scene::MediaFrameRate{transport.anchor().rate.numerator,
                                       transport.anchor().rate.denominator})) {
            std::cerr << "could not split a clip for frame "
                      << frame.value() << '\n';
            return 1;
        }
    }
    pending_clip_splits.clear();
    const scene::MediaFrameRate show_rate{transport.anchor().rate.numerator,
                                          transport.anchor().rate.denominator};
    for (const auto &request : pending_plateset_places) {
        PlatesetPlacementRequest place{};
        place.scene = request.scene;
        place.plateset = request.plateset;
        place.start = request.start;
        place.show_rate = show_rate;
        place.rig = request.rig == editor::PlatesetRigChoice::Replace
                        ? PlatesetRigChoice::Replace
                        : (request.rig == editor::PlatesetRigChoice::NewRig
                               ? PlatesetRigChoice::NewRig
                               : PlatesetRigChoice::Auto);
        while (scheduler.projected_document().contains(
            doc::NodeId{next_new_node})) {
            ++next_new_node;
        }
        auto planned = plan_plateset_placement(scheduler.projected_document(),
                                               place, next_new_node);
        if (!planned) {
            project_message = plateset_authoring_error_text(planned.error());
            continue;
        }
        next_new_node = std::max(next_new_node, planned.value().next_node);
        if (!scheduler.schedule_document_replace(
                planned.value().document, cmd::UndoContextId{2U},
                cmd::TransactionId{request.gesture}, frame)) {
            std::cerr << "could not place a plateset for frame "
                      << frame.value() << '\n';
            return 1;
        }
        project_message =
            planned.value().skipped.empty()
                ? std::string{"PLATESET PLACED"}
                : "PLATESET PLACED; " +
                      std::to_string(planned.value().skipped.size()) +
                      " SOCKETS KEPT THEIR WIRES";
    }
    pending_plateset_places.clear();
    for (const auto &request : pending_plateset_swaps) {
        auto planned = plan_plateset_swap(scheduler.projected_document(),
                                          request.clip, request.plateset);
        if (!planned) {
            project_message = plateset_authoring_error_text(planned.error());
            continue;
        }
        if (!scheduler.schedule_document_replace(
                planned.value().document, cmd::UndoContextId{2U},
                cmd::TransactionId{request.gesture}, frame)) {
            std::cerr << "could not swap a take for frame " << frame.value()
                      << '\n';
            return 1;
        }
        project_message =
            planned.value().unmatched.empty()
                ? std::string{"TAKE SWAPPED"}
                : "TAKE SWAPPED; " +
                      std::to_string(planned.value().unmatched.size()) +
                      " PLATES HAD NO MATCH AND STAYED";
    }
    pending_plateset_swaps.clear();
    return std::nullopt;
}

PhaseExit DirectorShowLoop::apply_element_and_lane_requests(OperatorFrame &op) {
    const auto &frame = op.frame;
    // Mute and lock are one boolean on the element's own node,
    // so they need no planner - but they do need the same undo
    // grouping, which the shared gesture id gives them.
    for (const auto &edit : pending_element_flags) {
        const auto *const existing =
            scheduler.projected_document().get(edit.element);
        if (existing == nullptr) {
            // Deleted under the click. Benign.
            continue;
        }
        doc::Node updated = *existing;
        const bool track =
            existing->type == scene::schema::timeline_lane_type;
        const bool showing =
            edit.flag == editor::TimelineElementFlag::Visible;
        // A track spells both of these differently and spells
        // visibility inside out - it is muted rather than hidden.
        // Translating here keeps one control in the timeline and
        // one in the Inspector writing the same state instead of
        // each owning half of it.
        updated.properties.insert_or_assign(
            std::string{track
                            ? (showing ? scene::schema::lane_muted
                                       : scene::schema::lane_locked)
                            : (showing ? scene::schema::visible
                                       : scene::schema::locked)},
            doc::PropertyValue{track && showing ? !edit.value
                                                : edit.value});
        if (updated == *existing) {
            continue;
        }
        auto scheduled = scheduler.schedule_edit(
            cmd::PutNode{std::move(updated)},
            cmd::PutNode{*existing}, cmd::UndoContextId{2U},
            cmd::TransactionId{edit.gesture}, frame);
        if (!scheduled) {
            std::cerr
                << "could not author an element flag for frame "
                << frame.value() << '\n';
            return 1;
        }
    }
    pending_element_flags.clear();
    // A rename is one string on the node's own record. Each node
    // type keeps its name under its own property, so the one
    // in-place field in every list writes the right one.
    for (const auto &edit : pending_renames) {
        const auto *const existing =
            scheduler.projected_document().get(edit.node);
        if (existing == nullptr || edit.name.empty()) {
            continue;
        }
        const std::string_view property =
            existing->type == scene::schema::stage_type
                ? scene::schema::scene_name
            : existing->type == scene::schema::timeline_lane_type
                ? scene::schema::lane_name
            : existing->type == scene::schema::timeline_marker_type
                ? scene::schema::marker_name
                : scene::schema::group_name;
        doc::Node updated = *existing;
        updated.properties.insert_or_assign(
            std::string{property}, doc::PropertyValue{edit.name});
        if (updated == *existing) {
            continue;
        }
        auto scheduled = scheduler.schedule_edit(
            cmd::PutNode{std::move(updated)},
            cmd::PutNode{*existing}, cmd::UndoContextId{2U},
            cmd::TransactionId{edit.gesture}, frame);
        if (!scheduled) {
            std::cerr << "could not author a rename for frame "
                      << frame.value() << '\n';
            return 1;
        }
    }
    pending_renames.clear();
    // A Scene panel highlight is one palette index on each node's own
    // record, every selected node in one undo step; None removes it.
    for (const auto &edit : pending_highlights) {
        if (edit.color.has_value() &&
            *edit.color >= scene::timeline_marker_colors) {
            continue;
        }
        doc::Root after = scheduler.projected_document();
        bool changed = false;
        for (const doc::NodeId node : edit.nodes) {
            const auto *const existing = after.get(node);
            if (existing == nullptr || scene::is_linked_occurrence(*existing)) {
                continue;
            }
            doc::Node updated = *existing;
            if (edit.color.has_value()) {
                updated.properties.insert_or_assign(
                    std::string{scene::schema::outliner_color},
                    doc::PropertyValue{static_cast<std::int64_t>(*edit.color)});
            } else if (const auto found = updated.properties.find(
                           scene::schema::outliner_color);
                       found != updated.properties.end()) {
                updated.properties.erase(found);
            }
            if (updated == *existing) {
                continue;
            }
            after = after.set(std::move(updated));
            changed = true;
        }
        if (!changed) {
            continue;
        }
        auto scheduled = scheduler.schedule_document_replace(
            after, cmd::UndoContextId{2U},
            cmd::TransactionId{edit.gesture}, frame);
        if (!scheduled) {
            std::cerr << "could not author a highlight for frame "
                      << frame.value() << '\n';
            return 1;
        }
    }
    pending_highlights.clear();
    for (const auto &edit : pending_lane_edits) {
        LaneAuthorAction action{};
        switch (edit.action) {
        case editor::TimelineLaneAction::Create:
            action = LaneAuthorAction::Create;
            break;
        case editor::TimelineLaneAction::Rename:
            action = LaneAuthorAction::Rename;
            break;
        case editor::TimelineLaneAction::MoveEarlier:
            action = LaneAuthorAction::MoveEarlier;
            break;
        case editor::TimelineLaneAction::MoveLater:
            action = LaneAuthorAction::MoveLater;
            break;
        case editor::TimelineLaneAction::MoveTo:
            action = LaneAuthorAction::MoveTo;
            break;
        case editor::TimelineLaneAction::ToggleMute:
            action = LaneAuthorAction::ToggleMute;
            break;
        case editor::TimelineLaneAction::ToggleLock:
            action = LaneAuthorAction::ToggleLock;
            break;
        case editor::TimelineLaneAction::ToggleSyncLock:
            action = LaneAuthorAction::ToggleSyncLock;
            break;
        case editor::TimelineLaneAction::DeleteEmpty:
            action = LaneAuthorAction::DeleteEmpty;
            break;
        case editor::TimelineLaneAction::DeleteClips:
            action = LaneAuthorAction::DeleteClips;
            break;
        case editor::TimelineLaneAction::DeleteMoveClips:
            action = LaneAuthorAction::DeleteMoveClips;
            break;
        case editor::TimelineLaneAction::Split:
            action = LaneAuthorAction::Split;
            break;
        }
        auto planned = plan_lane_authoring(
            scheduler.projected_document(),
            LaneAuthorRequest{
                action, edit.scene, edit.lane, edit.destination, edit.name,
                edit.frame,
                scene::MediaFrameRate{transport.anchor().rate.numerator,
                                      transport.anchor().rate.denominator}},
            next_new_node);
        if (!planned) {
            // Stale selection and a lane becoming non-empty are
            // benign operator races. The planner has not authored
            // anything, so history and redo remain untouched.
            switch (planned.error()) {
            case LaneAuthorError::NonEmptyLane:
                project_message = "TRACK DELETE REJECTED - TRACK IS NOT EMPTY";
                break;
            case LaneAuthorError::Locked:
                project_message =
                    "TRACK SPLIT REJECTED - UNLOCK THE TRACK AND ITS CLIPS";
                break;
            case LaneAuthorError::NothingToSplit:
                project_message = "TRACK SPLIT REJECTED - CUT WHERE THE "
                                  "TRACK HAS CLIPS ON BOTH SIDES";
                break;
            case LaneAuthorError::LaneLimit:
                project_message =
                    "TRACK EDIT REJECTED - THE SCENE HOLDS THE MOST TRACKS";
                break;
            default:
                project_message = "TRACK EDIT REJECTED";
                break;
            }
            continue;
        }
        const doc::Delta delta =
            doc::diff(scheduler.projected_document(),
                      planned.value().document);
        if (!delta.changes.empty()) {
            auto scheduled = scheduler.schedule_document_replace(
                planned.value().document, cmd::UndoContextId{2U},
                cmd::TransactionId{edit.gesture}, frame);
            if (!scheduled) {
                std::cerr
                    << "could not author a timeline lane for frame "
                    << frame.value() << '\n';
                return 1;
            }
        }
        next_new_node = planned.value().next_node;
    }
    pending_lane_edits.clear();
    for (const auto &edit : pending_marker_edits) {
        MarkerAuthorAction action{};
        switch (edit.action) {
        case editor::TimelineMarkerAction::Create:
            action = MarkerAuthorAction::Create;
            break;
        case editor::TimelineMarkerAction::Update:
            action = MarkerAuthorAction::Update;
            break;
        case editor::TimelineMarkerAction::Delete:
            action = MarkerAuthorAction::Delete;
            break;
        }
        auto planned = plan_marker_authoring(
            scheduler.projected_document(),
            MarkerAuthorRequest{action, edit.scene, edit.marker},
            next_new_node);
        if (!planned) {
            // A cue deleted on one surface while another still
            // showed it is a benign race; nothing was authored.
            project_message =
                planned.error() == MarkerAuthorError::MarkerLimit
                    ? "CUE REJECTED - THE SCENE HOLDS THE MOST "
                      "MARKERS IT CAN"
                    : "MARKER EDIT REJECTED";
            continue;
        }
        const doc::Delta delta =
            doc::diff(scheduler.projected_document(),
                      planned.value().document);
        if (!delta.changes.empty()) {
            auto scheduled = scheduler.schedule_document_replace(
                planned.value().document, cmd::UndoContextId{2U},
                cmd::TransactionId{edit.gesture}, frame);
            if (!scheduled) {
                std::cerr
                    << "could not author a timeline marker for "
                       "frame "
                    << frame.value() << '\n';
                return 1;
            }
        }
        next_new_node = planned.value().next_node;
    }
    pending_marker_edits.clear();
    return std::nullopt;
}

PhaseExit DirectorShowLoop::apply_group_selection_and_mask_requests(OperatorFrame &op) {
    const auto &frame = op.frame;
    if (pending_group_edit.has_value()) {
        const editor::GroupEditIntent request =
            std::move(*pending_group_edit);
        pending_group_edit.reset();
        core::Result<scene::GroupEditPlan, scene::GroupEditError>
            planned =
                request.action == editor::GroupEditAction::Group
                    ? scene::plan_group_nodes(
                          scheduler.projected_document(),
                          request.scene, request.selection,
                          doc::NodeId{next_new_node}, "Group")
                : request.action == editor::GroupEditAction::Create
                    ? scene::plan_create_group(
                          scheduler.projected_document(),
                          request.scene,
                          request.group != doc::NodeId{}
                              ? std::optional<doc::NodeId>{request.group}
                              : std::nullopt,
                          doc::NodeId{next_new_node},
                          request.kind == scene::GroupKind::VehicleProcess
                              ? "Vehicle Process"
                              : "Group",
                          request.kind)
                : request.action ==
                        editor::GroupEditAction::MoveInto
                    ? scene::plan_move_into_group(
                          scheduler.projected_document(),
                          request.scene, request.selection,
                          request.group != doc::NodeId{}
                              ? std::optional<doc::NodeId>{
                                    request.group}
                              : std::nullopt,
                          request.anchor != doc::NodeId{}
                              ? std::optional<doc::NodeId>{
                                    request.anchor}
                              : std::nullopt,
                          request.before)
                    : scene::plan_ungroup_nodes(
                          scheduler.projected_document(),
                          request.scene, request.group);
        if (!planned) {
            project_message = group_edit_error_message(
                request.action, planned.error());
        } else {
            const doc::Delta delta =
                doc::diff(scheduler.projected_document(),
                          planned.value().after);
            if (!delta.changes.empty()) {
                auto scheduled =
                    scheduler.schedule_document_replace(
                        planned.value().after,
                        cmd::UndoContextId{2U},
                        cmd::TransactionId{request.gesture}, frame);
                if (!scheduled) {
                    std::cerr << "could not author a Scene group "
                                 "edit for frame "
                              << frame.value() << '\n';
                    return 1;
                }
            }
            if (request.action == editor::GroupEditAction::Group) {
                next_new_node =
                    std::max(next_new_node,
                             planned.value().group.value() + 1U);
                project_message = "GROUP CREATED";
            } else if (request.action == editor::GroupEditAction::Create) {
                next_new_node =
                    std::max(next_new_node,
                             planned.value().group.value() + 1U);
                project_message =
                    request.kind == scene::GroupKind::VehicleProcess
                        ? "VEHICLE PROCESS GROUP CREATED"
                        : "GROUP CREATED";
            } else if (request.action ==
                       editor::GroupEditAction::MoveInto) {
                project_message = request.group != doc::NodeId{}
                                      ? "MOVED INTO GROUP"
                                      : "MOVED TO TOP LEVEL";
            } else {
                project_message =
                    "GROUP REMOVED; CHILDREN PRESERVED";
            }
        }
    }
    if (pending_selection_set_edit.has_value()) {
        const editor::SelectionSetEditIntent request =
            std::move(*pending_selection_set_edit);
        pending_selection_set_edit.reset();
        const scene::SelectionSetPlanResult planned = [&]() {
            switch (request.action) {
            case editor::SelectionSetEditAction::Create:
                return scene::plan_create_selection_set(
                    scheduler.projected_document(), request.scene,
                    request.name, request.members,
                    doc::NodeId{next_new_node});
            case editor::SelectionSetEditAction::ReplaceMembers:
                return scene::plan_replace_selection_set(
                    scheduler.projected_document(), request.scene,
                    request.set, request.members,
                    doc::NodeId{next_new_node});
            case editor::SelectionSetEditAction::Rename:
                return scene::plan_rename_selection_set(
                    scheduler.projected_document(), request.scene,
                    request.set, request.name);
            case editor::SelectionSetEditAction::Delete:
                return scene::plan_delete_selection_set(
                    scheduler.projected_document(), request.scene,
                    request.set);
            case editor::SelectionSetEditAction::Recall:
                return scene::SelectionSetPlanResult::failure(
                    scene::SelectionSetError{
                        scene::SelectionSetErrorCode::InvalidSet,
                        request.set});
            }
            return scene::SelectionSetPlanResult::failure(
                scene::SelectionSetError{
                    scene::SelectionSetErrorCode::InvalidSet,
                    request.set});
        }();
        if (!planned) {
            project_message = selection_set_error_message(
                request.action, planned.error());
        } else {
            const doc::Delta delta =
                doc::diff(scheduler.projected_document(),
                          planned.value().after);
            if (!delta.changes.empty()) {
                auto scheduled =
                    scheduler.schedule_document_replace(
                        planned.value().after,
                        cmd::UndoContextId{2U},
                        cmd::TransactionId{request.gesture}, frame);
                if (!scheduled) {
                    std::cerr << "could not author a Selection Set "
                                 "edit for frame "
                              << frame.value() << '\n';
                    return 1;
                }
            }
            if (request.action ==
                    editor::SelectionSetEditAction::Create ||
                request.action == editor::SelectionSetEditAction::
                                      ReplaceMembers) {
                for (const doc::Node &node : planned.value().puts) {
                    next_new_node = std::max(next_new_node,
                                             node.id.value() + 1U);
                }
            }
            switch (request.action) {
            case editor::SelectionSetEditAction::Create:
                project_message =
                    "SELECTION SET SAVED - " +
                    std::to_string(planned.value().members.size()) +
                    " MEMBERS";
                break;
            case editor::SelectionSetEditAction::ReplaceMembers:
                project_message =
                    "SELECTION SET REPLACED - " +
                    std::to_string(planned.value().members.size()) +
                    " MEMBERS";
                break;
            case editor::SelectionSetEditAction::Rename:
                project_message = "SELECTION SET RENAMED";
                break;
            case editor::SelectionSetEditAction::Delete:
                project_message = "SELECTION SET DELETED";
                break;
            case editor::SelectionSetEditAction::Recall:
                break;
            }
        }
    }
    if (pending_mask_edit.has_value()) {
        const editor::MaskEditIntent request =
            std::move(*pending_mask_edit);
        pending_mask_edit.reset();
        auto planned = plan_mask_edit_integration(
            scheduler.projected_document(), request, next_new_node);
        if (!planned) {
            if (planned.error().code ==
                scene::MaskAuthoringErrorCode::AllocationFailed) {
                std::cerr
                    << "could not allocate a mask authoring plan\n";
                return 1;
            }
            project_message =
                mask_edit_error_message(planned.error().code);
        } else {
            const doc::Delta delta =
                doc::diff(scheduler.projected_document(),
                          planned.value().document);
            if (!delta.changes.empty()) {
                auto scheduled =
                    scheduler.schedule_document_replace(
                        planned.value().document,
                        cmd::UndoContextId{2U},
                        cmd::TransactionId{request.transaction},
                        frame);
                if (!scheduled) {
                    std::cerr
                        << "could not author a mask edit for frame "
                        << frame.value() << '\n';
                    return 1;
                }
            }
            next_new_node = planned.value().next_node;
            project_message = "MASK EDIT APPLIED";
        }
    }
    return std::nullopt;
}

PhaseExit DirectorShowLoop::apply_timeline_edits(OperatorFrame &op) {
    const auto &frame = op.frame;
    for (const editor::TimelineEditIntent &request :
         pending_timeline_edits) {
        scene::SceneModel current_model;
        current_model.reset(scheduler.projected_document());
        const std::vector<scene::ClipPlacement> current_clips =
            current_model.clips(request.scene);
        auto planned = plan_timeline_edit_integration(
            scheduler.projected_document(), current_clips, request,
            clip_clipboard, next_new_node);
        if (!planned) {
            switch (planned.error()) {
            case TimelineEditIntegrationError::
                ClipboardIncompatible:
                project_message =
                    "TIMELINE EDIT REJECTED - CLIPBOARD "
                    "MUST "
                    "CONTAIN ONE SOURCE TRACK";
                break;
            case TimelineEditIntegrationError::
                TargetLaneUnavailable:
                project_message = "TIMELINE EDIT REJECTED - TARGET "
                                  "TRACK IS LOCKED OR STALE";
                break;
            case TimelineEditIntegrationError::TimelineRejected:
            case TimelineEditIntegrationError::AuthoringRejected:
                project_message = "TIMELINE EDIT REJECTED - CLIPS "
                                  "OR HANDLES CHANGED";
                break;
            case TimelineEditIntegrationError::PasteRejected:
                project_message = "TIMELINE EDIT REJECTED - "
                                  "CLIPBOARD PASTE FAILED";
                break;
            case TimelineEditIntegrationError::AllocationFailed:
                std::cerr << "could not allocate an advanced "
                             "timeline edit plan\n";
                return 1;
            }
            continue;
        }
        const doc::Delta delta =
            doc::diff(scheduler.projected_document(),
                      planned.value().document);
        if (!delta.changes.empty()) {
            auto scheduled = scheduler.schedule_document_replace(
                planned.value().document, cmd::UndoContextId{2U},
                cmd::TransactionId{request.transaction}, frame);
            if (!scheduled) {
                std::cerr << "could not author an advanced "
                             "timeline edit for frame "
                          << frame.value() << '\n';
                return 1;
            }
        }
        next_new_node = planned.value().next_node;
        project_message = "TIMELINE EDIT APPLIED";
    }
    pending_timeline_edits.clear();
    // The Asset Viewer's ranges: each a whole-source Library add of its asset,
    // trimmed to the range and pasted the way a clipboard Insert or
    // Overwrite is, then selected so the next keystroke acts on it.
    for (const editor::SourceRangeEditIntent &request : pending_source_edits) {
        const assets::Asset *const found =
            director_asset_registry.asset(request.asset);
        const auto *const media =
            found != nullptr ? std::get_if<assets::MediaAsset>(found) : nullptr;
        if (media == nullptr) {
            project_message =
                "ASSET VIEWER EDIT REJECTED - ASSET IS NOT IN THE LIBRARY";
            continue;
        }
        editor::AddIntent add{};
        add.kind = editor::AddKind::Media;
        add.source = media->source;
        add.count = std::max(media->frame_count.value_or(1U), 1U);
        add.gesture = request.transaction;
        add.aspect =
            media->dimensions.has_value() && media->dimensions->height != 0U
                ? static_cast<float>(media->dimensions->width) /
                      static_cast<float>(media->dimensions->height)
                : 16.0F / 9.0F;
        add.asset = media->id;
        add.scene = request.scene;
        add.frame_rate = media->frame_rate;
        const show::FrameRate show_rate{request.show_rate.numerator,
                                        request.show_rate.denominator};
        doc::Node placement = make_added_node(
            doc::NodeId{1U}, add, show_rate, scene::IcvfxCameraSettings{});
        scene::SceneModel current_model;
        current_model.reset(scheduler.projected_document());
        const std::vector<scene::ClipPlacement> current_clips =
            current_model.clips(request.scene);
        auto planned = plan_source_range_edit(
            scheduler.projected_document(), current_clips, *media, request,
            std::move(placement), next_new_node);
        if (!planned) {
            switch (planned.error()) {
            case SourceRangeEditError::NotProbed:
                project_message =
                    "ASSET VIEWER EDIT REJECTED - SOURCE NOT PROBED";
                break;
            case SourceRangeEditError::RangeOutsideSource:
                project_message =
                    "ASSET VIEWER EDIT REJECTED - RANGE OUTSIDE THE SOURCE";
                break;
            case SourceRangeEditError::TargetLaneUnavailable:
                project_message = "ASSET VIEWER EDIT REJECTED - TARGET "
                                  "TRACK IS LOCKED OR STALE";
                break;
            case SourceRangeEditError::TimelineRejected:
                project_message = "ASSET VIEWER EDIT REJECTED - CLIPS "
                                  "OR HANDLES CHANGED";
                break;
            case SourceRangeEditError::AllocationFailed:
                std::cerr << "could not allocate an Asset Viewer edit plan\n";
                return 1;
            }
            continue;
        }
        const doc::Delta delta =
            doc::diff(scheduler.projected_document(), planned.value().document);
        if (!delta.changes.empty()) {
            auto scheduled = scheduler.schedule_document_replace(
                planned.value().document, cmd::UndoContextId{2U},
                cmd::TransactionId{request.transaction}, frame);
            if (!scheduled) {
                std::cerr << "could not author an Asset Viewer edit for "
                             "frame "
                          << frame.value() << '\n';
                return 1;
            }
            history_descriptions.name(
                scene_undo_context, cmd::TransactionId{request.transaction},
                request.mode == editor::SourceEditMode::Insert
                    ? "Insert from Asset Viewer"
                : request.mode == editor::SourceEditMode::Overwrite
                    ? "Overwrite from Asset Viewer"
                    : "Append from Asset Viewer");
        }
        next_new_node = planned.value().next_node;
        std::vector<doc::NodeId> placed;
        for (const doc::Node &created : planned.value().created) {
            if (created.type == scene::schema::media_type) {
                placed.push_back(created.id);
            }
        }
        static_cast<void>(editor->apply_selection_set_recall(
            request.scene, placed,
            placed.empty() ? std::nullopt
                           : std::optional<doc::NodeId>{placed.front()}));
        project_message =
            request.mode == editor::SourceEditMode::Insert
                ? "ASSET VIEWER RANGE INSERTED"
            : request.mode == editor::SourceEditMode::Overwrite
                ? "ASSET VIEWER RANGE OVERWRITTEN"
                : "ASSET VIEWER RANGE APPENDED";
    }
    pending_source_edits.clear();
    if (pending_clip_paste.has_value()) {
        auto planned = pending_clip_paste->snapshot.paste(
            scheduler.projected_document(),
            pending_clip_paste->request.scene,
            pending_clip_paste->request.playhead, next_new_node);
        if (planned) {
            auto scheduled = scheduler.schedule_document_replace(
                planned.value().document, cmd::UndoContextId{2U},
                cmd::TransactionId{
                    pending_clip_paste->request.transaction},
                frame);
            if (!scheduled) {
                std::cerr
                    << "could not author a clip paste for frame "
                    << frame.value() << '\n';
                return 1;
            }
            history_descriptions.name(
                scene_undo_context,
                cmd::TransactionId{pending_clip_paste->request.transaction},
                "Paste clips");
            next_new_node = planned.value().next_node;
        } else if (planned.error() != ClipPasteError::Empty &&
                   planned.error() !=
                       ClipPasteError::InvalidTargetScene) {
            std::cerr
                << "ignored invalid clip clipboard paste (code "
                << static_cast<unsigned>(planned.error()) << ")\n";
        }
        pending_clip_paste.reset();
    }
    for (const editor::SceneDuplicateRequest &request :
         pending_scene_duplicates) {
        // In place, before the drag that carries the originals away lands.
        auto planned = duplicate_in_place(scheduler.projected_document(),
                                          request.selection, next_new_node);
        if (!planned.has_value()) {
            continue;
        }
        if (!scheduler.schedule_document_replace(
                planned->document, cmd::UndoContextId{2U},
                cmd::TransactionId{request.transaction}, frame)) {
            std::cerr << "could not author a duplicate for frame "
                      << frame.value() << '\n';
            return 1;
        }
        history_descriptions.name(
            scene_undo_context, cmd::TransactionId{request.transaction},
            request.selection.size() == 1U
                ? std::string{"Duplicate 1 item"}
                : "Duplicate " + std::to_string(request.selection.size()) +
                      " items");
        next_new_node = planned->next_node;
    }
    pending_scene_duplicates.clear();
    if (pending_project_settings.has_value()) {
        while (scheduler.projected_document().contains(
            doc::NodeId{next_new_node})) {
            ++next_new_node;
        }
        const doc::NodeId id{next_new_node};
        if (!schedule_project_settings(
                scheduler, *pending_project_settings, id, frame)) {
            std::cerr
                << "could not author project settings for frame "
                << frame.value() << '\n';
            return 1;
        }
        if (pending_project_settings->node == doc::NodeId{}) {
            ++next_new_node;
        }
        pending_project_settings.reset();
    }
    return std::nullopt;
}

PhaseExit DirectorShowLoop::apply_property_and_effect_requests(OperatorFrame &op) {
    const auto &frame = op.frame;
    for (const auto &edit : pending_properties) {
        if (!schedule_property_edit(scheduler, edit, frame,
                                    coalescing_for(edit.gesture))) {
            std::cerr
                << "could not author an inspector edit for frame "
                << frame.value() << '\n';
            return 1;
        }
    }
    pending_properties.clear();
    for (const auto &edit : pending_generics) {
        if (!schedule_generic_edit(scheduler, edit, frame,
                                   coalescing_for(edit.gesture), &project_message)) {
            std::cerr
                << "could not author an inspector edit for frame "
                << frame.value() << '\n';
            return 1;
        }
    }
    pending_generics.clear();
    const auto fresh_id = [&]() {
        while (scheduler.projected_document().contains(
            doc::NodeId{next_new_node})) {
            ++next_new_node;
        }
        return doc::NodeId{next_new_node++};
    };
    for (const auto &add : pending_adds) {
        const doc::NodeId id = fresh_id();
        // An asset dropped on a Compositor port lands in the graph's history,
        // with the Scene Layer and wire made for it, so one Undo in the
        // graph takes back everything the drop did.
        const cmd::UndoContextId context{add.graph_layer.has_value() ? 6U
                                                                     : 2U};
        if (!schedule_add(
                scheduler, add, id, frame,
                transport.anchor().rate,
                scene_model.renderer_settings()
                    .icvfx.camera_defaults, context)) {
            std::cerr
                << "could not author an element add for frame "
                << frame.value() << '\n';
            return 1;
        }
        if (add.graph_layer.has_value()) {
            editor::CompositorAddIntent layer = *add.graph_layer;
            layer.kind = compositor::NodeKind::SceneLayer;
            layer.scene_node = id;
            const doc::NodeId node = fresh_id();
            std::vector<doc::NodeId> edge_ids;
            edge_ids.reserve(layer.links.size());
            for (std::size_t index = 0U; index < layer.links.size(); ++index) {
                edge_ids.push_back(fresh_id());
            }
            if (!schedule_compositor_add_linked(
                    scheduler, layer, node, scene_being_authored(op),
                    edge_ids, add.gesture, frame)) {
                std::cerr << "could not wire a dropped asset for frame "
                          << frame.value() << '\n';
                return 1;
            }
        }
    }
    pending_adds.clear();
    for (const auto &request : pending_asset_deletes) {
        if (!schedule_asset_delete(scheduler, request, frame)) {
            std::cerr << "could not delete an asset for frame "
                      << frame.value() << '\n';
            return 1;
        }
    }
    pending_asset_deletes.clear();
    for (const auto &request : pending_source_replacements) {
        if (!schedule_source_replacement(scheduler, request, frame)) {
            std::cerr << "could not replace an object source for frame "
                      << frame.value() << '\n';
            return 1;
        }
    }
    pending_source_replacements.clear();
    for (const auto &request : pending_asset_proxy_edits) {
        if (!schedule_asset_proxy_edit(scheduler, request, frame)) {
            std::cerr << "could not author an asset proxy policy "
                         "for frame "
                      << frame.value() << '\n';
            return 1;
        }
    }
    pending_asset_proxy_edits.clear();
    for (const auto &request : pending_asset_library_edits) {
        doc::NodeId folder{};
        if (request.edit.kind == assets::LibraryEdit::Kind::CreateFolder ||
            request.edit.kind ==
                assets::LibraryEdit::Kind::CreatePlateset) {
            while (scheduler.projected_document().contains(
                doc::NodeId{next_new_node})) {
                ++next_new_node;
            }
            folder = doc::NodeId{next_new_node++};
        }
        if (!schedule_asset_library_edit(
                scheduler, request, folder, frame,
                scene::MediaFrameRate{transport.anchor().rate.numerator,
                                      transport.anchor().rate.denominator})) {
            std::cerr << "could not author a Library edit for frame "
                      << frame.value() << '\n';
            return 1;
        }
    }
    pending_asset_library_edits.clear();
    for (const auto &effect : pending_effects) {
        while (scheduler.projected_document().contains(
            doc::NodeId{next_new_node})) {
            ++next_new_node;
        }
        const doc::NodeId id{next_new_node++};
        if (!schedule_effect(scheduler, effect, id, frame)) {
            std::cerr << "could not author an effect for frame "
                      << frame.value() << '\n';
            return 1;
        }
    }
    pending_effects.clear();
    for (const auto &request : pending_film_grain_effect_edits) {
        if (!schedule_film_grain_effect_edit(
                scheduler, request, frame,
                coalescing_for(request.gesture))) {
            std::cerr << "could not author Film Grain parameters "
                         "for frame "
                      << frame.value() << '\n';
            return 1;
        }
    }
    pending_film_grain_effect_edits.clear();
    for (const auto &request : pending_effect_deletes) {
        if (!schedule_effect_delete(scheduler, request, frame)) {
            std::cerr << "could not delete an effect for frame "
                      << frame.value() << '\n';
            return 1;
        }
    }
    pending_effect_deletes.clear();
    return std::nullopt;
}

PhaseExit DirectorShowLoop::apply_hardware_requests(OperatorFrame &op) {
    const auto &frame = op.frame;
    for (const auto &request : pending_hardware_places) {
        while (scheduler.projected_document().contains(
            doc::NodeId{next_new_node})) {
            ++next_new_node;
        }
        const doc::NodeId id{next_new_node++};
        if (!schedule_hardware_place(scheduler, request, id,
                                     frame)) {
            std::cerr
                << "could not author a hardware device for frame "
                << frame.value() << '\n';
            return 1;
        }
    }
    pending_hardware_places.clear();
    for (const auto &request : pending_hardware_moves) {
        if (!schedule_hardware_move(scheduler, request, frame)) {
            std::cerr
                << "could not author a hardware move for frame "
                << frame.value() << '\n';
            return 1;
        }
    }
    pending_hardware_moves.clear();
    for (const auto &edit : pending_hardware_properties) {
        if (!schedule_hardware_property(scheduler, edit, frame)) {
            std::cerr
                << "could not author hardware properties for frame "
                << frame.value() << '\n';
            return 1;
        }
    }
    pending_hardware_properties.clear();
    if (pending_hardware_control_port.has_value()) {
        while (scheduler.projected_document().contains(
            doc::NodeId{next_new_node})) {
            ++next_new_node;
        }
        const doc::NodeId id{next_new_node++};
        if (!schedule_hardware_control_port(
                scheduler, *pending_hardware_control_port, id,
                frame)) {
            std::cerr << "could not author hardware control "
                         "settings for frame "
                      << frame.value() << '\n';
            return 1;
        }
        pending_hardware_control_port.reset();
    }
    if (pending_hardware_canvas.has_value()) {
        if (!schedule_hardware_canvas(scheduler, *pending_hardware_canvas,
                                      frame)) {
            std::cerr << "could not author the scene canvas for frame "
                      << frame.value() << '\n';
            return 1;
        }
        pending_hardware_canvas.reset();
    }
    if (pending_sync_policy.has_value()) {
        while (scheduler.projected_document().contains(
            doc::NodeId{next_new_node})) {
            ++next_new_node;
        }
        const doc::NodeId id{next_new_node++};
        if (!schedule_sync_policy(scheduler, *pending_sync_policy,
                                  id, frame)) {
            std::cerr << "could not author the sync policy for "
                         "frame "
                      << frame.value() << '\n';
            return 1;
        }
        pending_sync_policy.reset();
    }
    for (const auto &edit : pending_hardware_output_binding_edits) {
        while (scheduler.projected_document().contains(
            doc::NodeId{next_new_node})) {
            ++next_new_node;
        }
        const doc::NodeId id{next_new_node++};
        if (!schedule_hardware_output_binding(scheduler, edit, id,
                                              frame)) {
            std::cerr << "could not author an output binding for "
                         "frame "
                      << frame.value() << '\n';
            return 1;
        }
    }
    pending_hardware_output_binding_edits.clear();
    for (const auto &request :
         pending_hardware_output_binding_deletes) {
        if (!schedule_hardware_output_binding_delete(
                scheduler, request, frame)) {
            std::cerr << "could not remove an output binding for "
                         "frame "
                      << frame.value() << '\n';
            return 1;
        }
    }
    pending_hardware_output_binding_deletes.clear();
    for (const auto &request : pending_hardware_links) {
        if (request.output_binding.has_value()) {
            const topology::HardwareOutputBinding &binding =
                *request.output_binding;
            if (binding.node == doc::NodeId{}) {
                while (scheduler.projected_document().contains(
                    doc::NodeId{next_new_node})) {
                    ++next_new_node;
                }
                const doc::NodeId binding_id{next_new_node++};
                if (!schedule_hardware_output_binding(
                        scheduler,
                        editor::HardwareOutputBindingEdit{
                            binding, request.gesture},
                        binding_id, frame)) {
                    std::cerr << "could not bind a wired hardware "
                                 "output for frame "
                              << frame.value() << '\n';
                    return 1;
                }
            }
            if (find_feed_map_node(scheduler.projected_document(),
                                   binding.feed_index) == nullptr) {
                while (scheduler.projected_document().contains(
                    doc::NodeId{next_new_node})) {
                    ++next_new_node;
                }
                const doc::NodeId feed_id{next_new_node++};
                const editor::FeedMapAddRequest feed{
                    binding.feed_index, request.output_region,
                    scene::whole(scene::Canvas{binding.width,
                                               binding.height}),
                    request.gesture};
                if (!schedule_feed_map_add(
                        scheduler, feed, feed_id, frame,
                        cmd::UndoContextId{3U})) {
                    std::cerr << "could not create a region for a "
                                 "wired hardware "
                                 "output at frame "
                              << frame.value() << '\n';
                    return 1;
                }
            }
        }
        while (scheduler.projected_document().contains(
            doc::NodeId{next_new_node})) {
            ++next_new_node;
        }
        const doc::NodeId id{next_new_node++};
        if (!schedule_hardware_link(scheduler, request, id,
                                    frame)) {
            std::cerr
                << "could not author a hardware link for frame "
                << frame.value() << '\n';
            return 1;
        }
    }
    pending_hardware_links.clear();
    for (const auto &request : pending_hardware_deletes) {
        if (!schedule_hardware_delete(scheduler, request, frame)) {
            std::cerr << "could not delete hardware for frame "
                      << frame.value() << '\n';
            return 1;
        }
    }
    pending_hardware_deletes.clear();
    return std::nullopt;
}

PhaseExit DirectorShowLoop::apply_feed_map_requests(OperatorFrame &op) {
    const auto &frame = op.frame;
    for (const auto &request : pending_feed_surface_adds) {
        while (scheduler.projected_document().contains(
            doc::NodeId{next_new_node})) {
            ++next_new_node;
        }
        const doc::NodeId id{next_new_node++};
        if (!schedule_feed_surface_add(scheduler, request, id,
                                       frame)) {
            std::cerr
                << "could not add a projection surface for frame "
                << frame.value() << '\n';
            return 1;
        }
        if (request.assign_map_node.has_value()) {
            const editor::FeedProjectionEdit assignment{
                *request.assign_map_node,
                output::FeedProjection::PlanarSurface, id,
                scene::whole(request.surface.raster),
                request.gesture};
            if (!schedule_feed_projection_edit(scheduler,
                                               assignment, frame)) {
                std::cerr << "could not assign a UV projection "
                             "mesh for frame "
                          << frame.value() << '\n';
                return 1;
            }
        }
    }
    pending_feed_surface_adds.clear();
    for (const auto &request : pending_feed_map_adds) {
        while (scheduler.projected_document().contains(
            doc::NodeId{next_new_node})) {
            ++next_new_node;
        }
        const doc::NodeId id{next_new_node++};
        if (!schedule_feed_map_add(scheduler, request, id, frame)) {
            std::cerr << "could not add a feed map for frame "
                      << frame.value() << '\n';
            return 1;
        }
    }
    pending_feed_map_adds.clear();
    for (const auto &edit : pending_feed_map_edits) {
        while (scheduler.projected_document().contains(
            doc::NodeId{next_new_node})) {
            ++next_new_node;
        }
        const doc::NodeId id{next_new_node++};
        if (!schedule_feed_map_edit(scheduler, edit, id, frame)) {
            std::cerr
                << "could not author a feed map edit for frame "
                << frame.value() << '\n';
            return 1;
        }
    }
    pending_feed_map_edits.clear();
    for (const auto &edit : pending_feed_projection_edits) {
        if (!schedule_feed_projection_edit(scheduler, edit,
                                           frame)) {
            std::cerr
                << "could not edit a feed projection for frame "
                << frame.value() << '\n';
            return 1;
        }
    }
    pending_feed_projection_edits.clear();
    for (const auto &request : pending_feed_organize) {
        while (scheduler.projected_document().contains(
            doc::NodeId{next_new_node})) {
            ++next_new_node;
        }
        const doc::NodeId id{next_new_node};
        if (request.action == editor::FeedOrganizeAction::Duplicate) {
            ++next_new_node;
        }
        if (!schedule_feed_organize(scheduler, request, id, frame)) {
            std::cerr << "could not organize the feed map for frame "
                      << frame.value() << '\n';
            return 1;
        }
    }
    pending_feed_organize.clear();
    for (const auto &request : pending_feed_surface_deletes) {
        if (!schedule_feed_surface_delete(scheduler, request,
                                          frame)) {
            std::cerr << "could not delete a projection surface "
                         "for frame "
                      << frame.value() << '\n';
            return 1;
        }
    }
    pending_feed_surface_deletes.clear();
    for (const auto &request : pending_feed_map_deletes) {
        if (!schedule_feed_map_delete(scheduler, request, frame)) {
            std::cerr << "could not delete a feed map for frame "
                      << frame.value() << '\n';
            return 1;
        }
    }
    pending_feed_map_deletes.clear();
    return std::nullopt;
}

PhaseExit DirectorShowLoop::apply_compositor_and_delete_requests(OperatorFrame &op) {
    const auto &frame = op.frame;
    const auto authoring_scene = [&]() -> doc::NodeId {
        return scene_being_authored(op);
    };
    const auto fresh_id = [&]() {
        while (scheduler.projected_document().contains(
            doc::NodeId{next_new_node})) {
            ++next_new_node;
        }
        return doc::NodeId{next_new_node++};
    };
    // Ids for wires authored together. Each is taken before any is
    // scheduled, so the counter - not the document - keeps them distinct.
    const auto fresh_ids = [&](std::size_t count) {
        std::vector<doc::NodeId> ids;
        ids.reserve(count);
        for (std::size_t index = 0U; index < count; ++index) {
            ids.push_back(fresh_id());
        }
        return ids;
    };
    for (const auto &request : pending_compositor_adds) {
        const doc::NodeId id = fresh_id();
        const std::vector<doc::NodeId> edge_ids =
            fresh_ids(request.links.size());
        if (!schedule_compositor_add_linked(
                scheduler, request, id, authoring_scene(), edge_ids,
                pending_compositor_gesture, frame)) {
            std::cerr
                << "could not add a compositor node for frame "
                << frame.value() << '\n';
            return 1;
        }
    }
    pending_compositor_adds.clear();
    for (const auto &move : pending_compositor_moves) {
        if (!schedule_compositor_move(
                scheduler, move, pending_compositor_gesture, frame,
                coalescing_for(pending_compositor_gesture))) {
            std::cerr
                << "could not move a compositor node for frame "
                << frame.value() << '\n';
            return 1;
        }
    }
    pending_compositor_moves.clear();
    if (pending_compositor_rename.has_value()) {
        if (!schedule_compositor_rename(scheduler, *pending_compositor_rename,
                                        pending_compositor_gesture, frame)) {
            std::cerr << "could not rename a compositor node for frame "
                      << frame.value() << '\n';
            return 1;
        }
        pending_compositor_rename.reset();
    }
    if (pending_compositor_reorder.has_value()) {
        if (!schedule_compositor_reorder(scheduler, *pending_compositor_reorder,
                                         pending_compositor_gesture, frame)) {
            std::cerr << "could not reorder compositor wires for frame "
                      << frame.value() << '\n';
            return 1;
        }
        pending_compositor_reorder.reset();
    }
    if (pending_compositor_bypass.has_value()) {
        if (!schedule_compositor_bypass(scheduler, *pending_compositor_bypass,
                                        pending_compositor_gesture, frame)) {
            std::cerr << "could not bypass compositor nodes for frame "
                      << frame.value() << '\n';
            return 1;
        }
        pending_compositor_bypass.reset();
    }
    if (pending_compositor_collapse.has_value()) {
        if (!schedule_compositor_collapse(scheduler,
                                          *pending_compositor_collapse,
                                          pending_compositor_gesture, frame)) {
            std::cerr << "could not collapse compositor nodes for frame "
                      << frame.value() << '\n';
            return 1;
        }
        pending_compositor_collapse.reset();
    }
    if (pending_compositor_comment.has_value()) {
        const doc::NodeId id = pending_compositor_comment->node != doc::NodeId{}
                                   ? pending_compositor_comment->node
                                   : fresh_id();
        if (!schedule_compositor_comment(scheduler, *pending_compositor_comment,
                                         id, authoring_scene(),
                                         pending_compositor_gesture, frame)) {
            std::cerr << "could not author a compositor comment for frame "
                      << frame.value() << '\n';
            return 1;
        }
        pending_compositor_comment.reset();
    }
    if (pending_compositor_clipboard.has_value()) {
        const editor::CompositorClipboardIntent request =
            std::move(*pending_compositor_clipboard);
        pending_compositor_clipboard.reset();
        const doc::Root &root = scheduler.projected_document();
        if (request.action == editor::CompositorClipboardAction::Copy) {
            compositor_clipboard =
                compositor::snapshot_graph_selection(root, request.nodes);
        } else {
            const std::vector<doc::Node> snapshot =
                request.action == editor::CompositorClipboardAction::Duplicate
                    ? compositor::snapshot_graph_selection(root, request.nodes)
                    : compositor_clipboard;
            float dx = request.x;
            float dy = request.y;
            if (request.action == editor::CompositorClipboardAction::Paste) {
                const auto [left, top] = compositor_snapshot_origin(snapshot);
                dx = request.x - left;
                dy = request.y - top;
            }
            // Graph objects with no owner of their own belong to the
            // lowest-id stage, as everywhere else.
            scene::SceneModel stages;
            stages.reset(root);
            const auto scenes = stages.scenes();
            const doc::NodeId legacy_owner =
                scenes.empty() ? doc::NodeId{} : scenes.front().id;
            auto planned = compositor::plan_paste_graph(
                root, snapshot, authoring_scene(), legacy_owner, dx, dy,
                doc::NodeId{std::max<core::u64>(next_new_node, 1U)});
            if (planned && !schedule_compositor_paste(
                               scheduler, planned.value().puts,
                               pending_compositor_gesture, frame,
                               next_new_node)) {
                std::cerr << "could not paste compositor nodes for frame "
                          << frame.value() << '\n';
                return 1;
            }
            if (planned) {
                history_descriptions.name(
                    compositor_undo_context,
                    cmd::TransactionId{pending_compositor_gesture},
                    request.action ==
                            editor::CompositorClipboardAction::Duplicate
                        ? "Duplicate nodes"
                        : "Paste nodes");
            }
        }
    }
    if (pending_compositor_property.has_value()) {
        if (!schedule_compositor_property(
                scheduler, *pending_compositor_property,
                pending_compositor_gesture, frame,
                coalescing_for(pending_compositor_gesture))) {
            std::cerr << "could not edit a compositor parameter "
                         "for frame "
                      << frame.value() << '\n';
            return 1;
        }
        pending_compositor_property.reset();
    }
    if (pending_compositor_connect.has_value()) {
        while (scheduler.projected_document().contains(
            doc::NodeId{next_new_node})) {
            ++next_new_node;
        }
        const doc::NodeId id{next_new_node++};
        if (!schedule_compositor_connect(
                scheduler, *pending_compositor_connect, id,
                pending_compositor_gesture, frame)) {
            std::cerr
                << "could not connect compositor nodes for frame "
                << frame.value() << '\n';
            return 1;
        }
        pending_compositor_connect.reset();
    }
    if (pending_compositor_rewire.has_value()) {
        const std::vector<doc::NodeId> edge_ids =
            fresh_ids(pending_compositor_rewire->add_edges.size());
        if (!schedule_compositor_rewire(scheduler, *pending_compositor_rewire,
                                        edge_ids, pending_compositor_gesture,
                                        frame)) {
            std::cerr << "could not rewire compositor nodes for frame "
                      << frame.value() << '\n';
            return 1;
        }
        pending_compositor_rewire.reset();
    }
    if (pending_compositor_waypoints.has_value()) {
        if (!schedule_compositor_waypoints(scheduler,
                                           *pending_compositor_waypoints,
                                           pending_compositor_gesture, frame)) {
            std::cerr << "could not route a compositor wire for frame "
                      << frame.value() << '\n';
            return 1;
        }
        pending_compositor_waypoints.reset();
    }
    if (pending_compositor_delete.has_value()) {
        if (!schedule_compositor_delete(
                scheduler, *pending_compositor_delete,
                pending_compositor_gesture, frame)) {
            std::cerr
                << "could not delete a compositor item for frame "
                << frame.value() << '\n';
            return 1;
        }
        pending_compositor_delete.reset();
    }
    pending_compositor_gesture = 0U;
    for (const auto node : pending_deletes) {
        if (!schedule_delete(scheduler, node,
                             pending_delete_gesture, frame)) {
            std::cerr
                << "could not author an element delete for frame "
                << frame.value() << '\n';
            return 1;
        }
    }
    pending_deletes.clear();
    return std::nullopt;
}

} // namespace mediaserver::app
