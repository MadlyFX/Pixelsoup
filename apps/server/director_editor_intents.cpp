// Editor intents queued for the next frame boundary.

#include "director_show_loop.hpp"

#include "director_documents.hpp"
#include "director_editor_view.hpp"
#include "director_key_authoring.hpp"
#include "director_scene_authoring.hpp"
#include "key_clipboard.hpp"

#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/editor/timeline_panel.hpp>
#include <mediaserver/scene/key_edit.hpp>
#include <mediaserver/scene/linked_scene.hpp>
#include <mediaserver/scene/selection_set.hpp>

#include <iostream>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mediaserver::app {

PhaseExit DirectorShowLoop::queue_editor_edits(
    editor::EditorFrame &editor_frame) {
    // Coalesce to the latest edit per node before the next boundary
    // authors them, so a drag is one command per frame, not per event.
    for (const auto &edit : editor_frame.edits) {
        bool merged = false;
        for (auto &queued : pending_edits) {
            if (queued.node == edit.node) {
                queued = edit;
                merged = true;
                break;
            }
        }
        if (!merged) {
            try {
                pending_edits.push_back(edit);
            } catch (const std::bad_alloc &) {
                std::cerr << "could not queue a scene edit\n";
                return 1;
            }
        }
    }
    for (const auto &edit : editor_frame.world_edits) {
        bool merged = false;
        for (auto &queued : pending_world_edits) {
            if (queued.node == edit.node) {
                queued = edit;
                merged = true;
                break;
            }
        }
        if (!merged) {
            try {
                pending_world_edits.push_back(edit);
            } catch (const std::bad_alloc &) {
                std::cerr << "could not queue a linked placement\n";
                return 1;
            }
        }
    }
    if (editor_frame.history.has_value()) {
        pending_history = editor_frame.history;
    }
    if (editor_frame.clip_clipboard.has_value()) {
        const editor::ClipClipboardRequest &request =
            *editor_frame.clip_clipboard;
        if (request.action == editor::ClipClipboardAction::Copy) {
            try {
                linked_clipboard.reset();
                doc::NodeId attachment{};
                bool all_linked = !request.selection.empty();
                for (const auto id : request.selection) {
                    const auto* n = scheduler.projected_document().get(id);
                    const auto instance = n && n->type == scene::link_schema::instance_type ? id
                        : n && scene::is_linked_occurrence(*n) ? doc::NodeId{static_cast<core::u64>(read_transform(*n,"link_instance",0))} : doc::NodeId{};
                    if (!instance.value() || (attachment.value() && attachment != instance)) all_linked = false;
                    attachment = instance;
                }
                if (all_linked) {
                    auto snapshot = scene::snapshot_link(scheduler.projected_document(),attachment);
                    if (snapshot) { linked_clipboard = snapshot.value(); project_message = "COPIED - independent collection snapshot; Paste into Preview"; }
                    else project_message = "COPY - " + snapshot.error().message;
                }
                static_cast<void>(clip_clipboard.copy(
                    scheduler.projected_document(), request.selection));
            } catch (const std::bad_alloc &) {
                std::cerr << "could not snapshot the clip clipboard\n";
                return 1;
            }
        } else {
            try {
                if (linked_clipboard) pending_linked_paste = request;
                else pending_clip_paste =
                    PendingClipPaste{request, clip_clipboard};
            } catch (const std::bad_alloc &) {
                std::cerr << "could not queue a clip paste\n";
                return 1;
            }
        }
    }
    if (editor_frame.key_clipboard.has_value()) {
        const editor::KeyClipboardRequest &request =
            *editor_frame.key_clipboard;
        try {
            if (request.action ==
                editor::TimelineKeyClipboardAction::Copy) {
                const std::vector<KeyAddress> selection =
                    key_addresses(request.selection);
                const KeyCopyReport report =
                    key_clipboard.copy(scheduler.projected_document(),
                                       request.scene, selection);
                project_message = key_operation_message(
                    "COPY", report.captured, "CAPTURED",
                    report.locked_targets, report.incompatible);
            } else {
                pending_key_paste =
                    PendingKeyPaste{request, key_clipboard};
            }
        } catch (const std::bad_alloc &) {
            std::cerr << "could not snapshot the key clipboard\n";
            return 1;
        }
    }
    if (editor_frame.key_duplicate.has_value()) {
        try {
            pending_key_duplicate = editor_frame.key_duplicate;
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue a key duplicate\n";
            return 1;
        }
    }
    if (editor_frame.key_scale_preview.has_value()) {
        const editor::KeyScalePreviewRequest &request =
            *editor_frame.key_scale_preview;
        const std::vector<scene::KeyAddress> selection =
            scene_key_addresses(request.preview.keys);
        auto planned =
            scene::plan_key_scale(scheduler.projected_document(),
                                  selection, request.preview.scale);
        editor::TimelineKeyScalePreview preview{};
        preview.signature = request.preview.signature;
        if (planned) {
            preview.valid = true;
            preview.selected = planned.value().report.selected;
            preview.applied = planned.value().report.applied;
            preview.skips = planned.value().report.skips.size();
            preview.collisions =
                planned.value().report.collisions.size();
        } else if (planned.error() ==
                   scene::KeyEditError::AllocationFailed) {
            std::cerr << "could not allocate a key scale preview\n";
            return 1;
        }
        key_scale_preview = preview;
    }
    if (editor_frame.key_batch.has_value()) {
        try {
            pending_key_batch = std::move(*editor_frame.key_batch);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue an atomic key edit\n";
            return 1;
        }
    }
    if (editor_frame.seek_to_frame) {
        (rehearsal_mode ? pending_preview_seek : pending_seek) =
            editor_frame.seek_to_frame;
    }
    if (editor_frame.range) {
        pending_range = editor_frame.range;
    }
    if (editor_frame.show_rate) {
        pending_show_rate = editor_frame.show_rate;
    }
    for (const auto &key : editor_frame.keys) {
        try {
            pending_keys.push_back(key);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue a keyframe\n";
            return 1;
        }
    }
    for (const auto &moved : editor_frame.key_moves) {
        try {
            pending_key_moves.push_back(moved);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue a keyframe move\n";
            return 1;
        }
    }
    if (editor_frame.project_settings.has_value()) {
        pending_project_settings = editor_frame.project_settings;
    }
    for (const auto &edit : editor_frame.clip_timings) {
        try {
            pending_clip_timings.push_back(edit);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue a clip placement\n";
            return 1;
        }
    }
    for (const auto &split : editor_frame.clip_splits) {
        try {
            pending_clip_splits.push_back(split);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue a clip split\n";
            return 1;
        }
    }
    pending_asset_rescan =
        pending_asset_rescan || editor_frame.rescan_assets;
    for (const auto &edit : editor_frame.element_flags) {
        try {
            pending_element_flags.push_back(edit);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue an element flag edit\n";
            return 1;
        }
    }
    for (const auto &edit : editor_frame.renames) {
        try {
            pending_renames.push_back(edit);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue a rename\n";
            return 1;
        }
    }
    for (const auto &edit : editor_frame.highlights) {
        try {
            pending_highlights.push_back(edit);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue a highlight\n";
            return 1;
        }
    }
    for (const auto &edit : editor_frame.lane_edits) {
        try {
            pending_lane_edits.push_back(edit);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue a timeline lane edit\n";
            return 1;
        }
    }
    for (const auto &edit : editor_frame.marker_edits) {
        try {
            pending_marker_edits.push_back(edit);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue a timeline marker edit\n";
            return 1;
        }
    }
    if (editor_frame.cues_armed.has_value()) {
        pending_cues_armed = editor_frame.cues_armed;
    }
    for (const auto &edit : editor_frame.layer_orders) {
        try {
            pending_layer_orders.push_back(edit);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue a layer reorder\n";
            return 1;
        }
    }
    for (const auto &edit : editor_frame.timeline_edits) {
        try {
            pending_timeline_edits.push_back(edit);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue an advanced timeline edit\n";
            return 1;
        }
    }
    if (editor_frame.group_edit.has_value()) {
        try {
            pending_group_edit = editor_frame.group_edit;
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue a Scene group edit\n";
            return 1;
        }
    }
    if (editor_frame.selection_set_edit.has_value()) {
        editor::SelectionSetEditIntent request =
            std::move(*editor_frame.selection_set_edit);
        if (request.action == editor::SelectionSetEditAction::Recall) {
            auto recalled = scene::recall_selection_set(
                scheduler.projected_document(), request.scene,
                request.set);
            if (!recalled) {
                project_message = selection_set_error_message(
                    request.action, recalled.error());
            } else if (!editor->apply_selection_set_recall(
                           request.scene, recalled.value().targets,
                           recalled.value().primary)) {
                project_message =
                    "SELECTION SET RECALL REJECTED - SCENE CHANGED "
                    "OR LOCAL MEMORY IS FULL";
            } else {
                project_message =
                    "SELECTION SET RECALLED - " +
                    std::to_string(recalled.value().targets.size()) +
                    " SELECTED";
                if (!recalled.value().skips.empty()) {
                    project_message +=
                        " / " +
                        std::to_string(recalled.value().skips.size()) +
                        " STALE OR INCOMPATIBLE SKIPPED";
                }
            }
        } else {
            try {
                pending_selection_set_edit = std::move(request);
            } catch (const std::bad_alloc &) {
                std::cerr << "could not queue a Selection Set edit\n";
                return 1;
            }
        }
    }
    if (editor_frame.mask_edit.has_value()) {
        try {
            pending_mask_edit = std::move(*editor_frame.mask_edit);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue a mask edit\n";
            return 1;
        }
    }
    for (const auto &edit : editor_frame.property_edits) {
        try {
            pending_properties.push_back(edit);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue an inspector edit\n";
            return 1;
        }
    }
    for (const auto &edit : editor_frame.generic_edits) {
        try {
            pending_generics.push_back(edit);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue an inspector edit\n";
            return 1;
        }
    }
    for (const auto &edit : editor_frame.lock_edits) {
        try {
            pending_locks.push_back(edit);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue an element lock\n";
            return 1;
        }
    }
    for (const auto &add : editor_frame.adds) {
        try {
            pending_adds.push_back(add);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue an element add\n";
            return 1;
        }
    }
    for (const auto &imported : editor_frame.asset_imports) {
        try {
            pending_asset_imports.push_back(imported);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue an asset import\n";
            return 1;
        }
    }
    for (const auto &revision : editor_frame.asset_revisions) {
        try {
            pending_asset_revisions.push_back(revision);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue an asset revision\n";
            return 1;
        }
    }
    for (const auto &request : editor_frame.asset_deletes) {
        try {
            pending_asset_deletes.push_back(request);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue an asset removal\n";
            return 1;
        }
    }
    for (const auto &request : editor_frame.source_replacements) {
        try {
            pending_source_replacements.push_back(request);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue a source replacement\n";
            return 1;
        }
    }
    for (const auto &request : editor_frame.asset_proxy_edits) {
        try {
            pending_asset_proxy_edits.push_back(request);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue an asset proxy policy\n";
            return 1;
        }
    }
    for (const auto &request : editor_frame.asset_library_edits) {
        try {
            pending_asset_library_edits.push_back(request);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue a Library edit\n";
            return 1;
        }
    }
    for (const auto &effect : editor_frame.effects) {
        try {
            pending_effects.push_back(effect);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue an effect\n";
            return 1;
        }
    }
    for (const auto &edit : editor_frame.film_grain_effect_edits) {
        try {
            pending_film_grain_effect_edits.push_back(edit);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue Film Grain parameters\n";
            return 1;
        }
    }
    for (const auto &effect : editor_frame.effect_deletes) {
        try {
            pending_effect_deletes.push_back(effect);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue an effect removal\n";
            return 1;
        }
    }
    if (editor_frame.switcher.has_value()) {
        pending_switcher = editor_frame.switcher;
    }
    if (editor_frame.publication_storage) pending_publication_storage=editor_frame.publication_storage;
    pending_publication=pending_publication || editor_frame.publish_linked_scene;
    pending_publication_cancel=pending_publication_cancel || editor_frame.cancel_linked_publication;
    pending_publication_rollback=pending_publication_rollback || editor_frame.rollback_linked_publication;
    return std::nullopt;
}

PhaseExit DirectorShowLoop::queue_editor_requests(
    DirectorTick &tick, editor::EditorFrame &editor_frame) {
    auto &stage_scene_frame = tick.stage_scene_frame;
    try {
        pending_hardware_places.insert(
            pending_hardware_places.end(),
            editor_frame.hardware_places.begin(),
            editor_frame.hardware_places.end());
        pending_hardware_moves.insert(
            pending_hardware_moves.end(),
            editor_frame.hardware_moves.begin(),
            editor_frame.hardware_moves.end());
        pending_hardware_links.insert(
            pending_hardware_links.end(),
            editor_frame.hardware_links.begin(),
            editor_frame.hardware_links.end());
        pending_hardware_deletes.insert(
            pending_hardware_deletes.end(),
            editor_frame.hardware_deletes.begin(),
            editor_frame.hardware_deletes.end());
        pending_hardware_properties.insert(
            pending_hardware_properties.end(),
            editor_frame.hardware_property_edits.begin(),
            editor_frame.hardware_property_edits.end());
        pending_hardware_output_binding_edits.insert(
            pending_hardware_output_binding_edits.end(),
            editor_frame.hardware_output_binding_edits.begin(),
            editor_frame.hardware_output_binding_edits.end());
        pending_hardware_output_binding_deletes.insert(
            pending_hardware_output_binding_deletes.end(),
            editor_frame.hardware_output_binding_deletes.begin(),
            editor_frame.hardware_output_binding_deletes.end());
        pending_feed_map_adds.insert(pending_feed_map_adds.end(),
                                     editor_frame.feed_map_adds.begin(),
                                     editor_frame.feed_map_adds.end());
        pending_feed_map_edits.insert(
            pending_feed_map_edits.end(),
            editor_frame.feed_map_edits.begin(),
            editor_frame.feed_map_edits.end());
        pending_feed_map_deletes.insert(
            pending_feed_map_deletes.end(),
            editor_frame.feed_map_deletes.begin(),
            editor_frame.feed_map_deletes.end());
        pending_feed_projection_edits.insert(
            pending_feed_projection_edits.end(),
            editor_frame.feed_projection_edits.begin(),
            editor_frame.feed_projection_edits.end());
        pending_feed_organize.insert(pending_feed_organize.end(),
                                     editor_frame.feed_organize.begin(),
                                     editor_frame.feed_organize.end());
        pending_feed_surface_adds.insert(
            pending_feed_surface_adds.end(),
            editor_frame.feed_surface_adds.begin(),
            editor_frame.feed_surface_adds.end());
        pending_feed_surface_edits.insert(
            pending_feed_surface_edits.end(),
            editor_frame.feed_surface_edits.begin(),
            editor_frame.feed_surface_edits.end());
        pending_feed_surface_deletes.insert(
            pending_feed_surface_deletes.end(),
            editor_frame.feed_surface_deletes.begin(),
            editor_frame.feed_surface_deletes.end());
    } catch (const std::bad_alloc &) {
        std::cerr << "could not queue a mapping edit\n";
        return 1;
    }
    if (editor_frame.hardware_control_port_edit.has_value()) {
        pending_hardware_control_port =
            editor_frame.hardware_control_port_edit;
    }
    if (editor_frame.sync_policy_edit.has_value()) {
        pending_sync_policy = editor_frame.sync_policy_edit;
    }
    if (editor_frame.hardware_canvas_edit.has_value()) {
        pending_hardware_canvas = editor_frame.hardware_canvas_edit;
    }
    if (!editor_frame.compositor_adds.empty()) {
        pending_compositor_adds.insert(pending_compositor_adds.end(),
            editor_frame.compositor_adds.begin(),
            editor_frame.compositor_adds.end());
        pending_compositor_gesture = editor_frame.compositor_gesture;
    }
    if (!editor_frame.compositor_moves.empty()) {
        pending_compositor_moves = editor_frame.compositor_moves;
        pending_compositor_gesture = editor_frame.compositor_gesture;
    }
    if (editor_frame.compositor_clipboard.has_value()) {
        pending_compositor_clipboard = editor_frame.compositor_clipboard;
        pending_compositor_gesture = editor_frame.compositor_gesture;
    }
    if (editor_frame.compositor_rename.has_value()) {
        pending_compositor_rename = editor_frame.compositor_rename;
        pending_compositor_gesture = editor_frame.compositor_gesture;
    }
    if (editor_frame.compositor_reorder.has_value()) {
        pending_compositor_reorder = editor_frame.compositor_reorder;
        pending_compositor_gesture = editor_frame.compositor_gesture;
    }
    if (editor_frame.compositor_bypass.has_value()) {
        pending_compositor_bypass = editor_frame.compositor_bypass;
        pending_compositor_gesture = editor_frame.compositor_gesture;
    }
    if (editor_frame.compositor_collapse.has_value()) {
        pending_compositor_collapse = editor_frame.compositor_collapse;
        pending_compositor_gesture = editor_frame.compositor_gesture;
    }
    if (editor_frame.compositor_comment.has_value()) {
        pending_compositor_comment = editor_frame.compositor_comment;
        pending_compositor_gesture = editor_frame.compositor_gesture;
    }
    if (editor_frame.compositor_connect.has_value()) {
        pending_compositor_connect = editor_frame.compositor_connect;
        pending_compositor_gesture = editor_frame.compositor_gesture;
    }
    if (editor_frame.compositor_rewire.has_value()) {
        pending_compositor_rewire = editor_frame.compositor_rewire;
        pending_compositor_gesture = editor_frame.compositor_gesture;
    }
    if (editor_frame.compositor_waypoints.has_value()) {
        pending_compositor_waypoints = editor_frame.compositor_waypoints;
        pending_compositor_gesture = editor_frame.compositor_gesture;
    }
    if (editor_frame.compositor_property_edit.has_value()) {
        pending_compositor_property =
            editor_frame.compositor_property_edit;
        pending_compositor_gesture = editor_frame.compositor_gesture;
    }
    if (editor_frame.compositor_delete.has_value()) {
        pending_compositor_delete = editor_frame.compositor_delete;
        pending_compositor_gesture = editor_frame.compositor_gesture;
    }
    if (!editor_frame.deletes.empty()) {
        try {
            pending_deletes.insert(pending_deletes.end(),
                                   editor_frame.deletes.begin(),
                                   editor_frame.deletes.end());
            pending_delete_gesture = editor_frame.delete_gesture;
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue an element delete\n";
            return 1;
        }
    }
    if (editor_frame.transport != editor::TransportRequest::None) {
        try {
            // In Preview the timeline drives Preview's own playhead.
            // The show keeps running underneath, untouched.
            (rehearsal_mode ? preview_commands : operator_commands)
                .push_back(map_transport(editor_frame.transport));
        } catch (const std::bad_alloc &) {
            std::cerr << "could not queue a transport command\n";
            return 1;
        }
    }
    // Linking needs a control frame (it may author the scene's
    // association); disconnecting only talks to Blender.
    if (!editor_frame.connect_blender.empty()) deferred_blender_connect = editor_frame.connect_blender;
    if (!editor_frame.resync_blender.empty()) deferred_blender_resync = editor_frame.resync_blender;
    if (editor_frame.open_in_blender) deferred_blender_open = *editor_frame.open_in_blender;
    if (editor_frame.disconnect_blender_scene) blender_links.disconnect_scene(*editor_frame.disconnect_blender_scene);
    if (!editor_frame.disconnect_blender.empty()) blender_links.disconnect_instance(editor_frame.disconnect_blender);
    if (editor_frame.stop_scene_link) {
        deferred_blender_connect.clear(); blender_connection_error.clear();
        blender_links.disconnect_scene(stage_scene_frame.id);
    }
    if (editor_frame.cancel_linked_scene_import) {
        linked_ingest.cancel(); project_message = "LINK IMPORT CANCELLED";
    }
    if (editor_frame.import_linked_scene) {
        if (!rehearsal_mode) project_message = "LINK IMPORT - switch to PREVIEW before importing";
        else {
            linked_ingest_base = scheduler.projected_document();
            linked_ingest_target = stage_scene_frame.id;
            linked_ingest_project = project_generation;
            linked_ingest_gesture = editor_frame.linked_scene_import_gesture;
            linked_ingest.submit(editor_frame.import_linked_scene->path);
            project_message = "STAGING LINKED SCENE - import again to supersede, or File > Cancel Linked Scene Import";
        }
    }
    if (editor_frame.load_project.has_value() ||
        editor_frame.new_project) {
        blender_links.stop_all();
        deferred_blender_connect.clear(); deferred_blender_resync.clear();
        deferred_blender_open.reset();
        linked_ingest.cancel();
        pending_project_save.reset();
        // An explicit Open supersedes the startup recent-project
        // candidate, including its already-decoded Root. Never pair a
        // newly chosen path with stale preloaded project contents.
        pending_preloaded_project.reset();
        if (editor_frame.new_project) {
            // A new project takes the ordinary load path with its
            // document already built and no file behind it.
            pending_preloaded_project = new_project_document();
            pending_project_load = editor::ProjectFileRequest{};
            project_message = "STARTING NEW PROJECT...";
        } else {
            pending_project_load = std::move(editor_frame.load_project);
            project_message = "LOADING PROJECT...";
        }
    } else if (editor_frame.save_project.has_value()) {
        // Explicit Save means "save this current project", not "load
        // the remembered project at the next boundary first".
        pending_project_load.reset();
        pending_preloaded_project.reset();
        pending_project_save = std::move(editor_frame.save_project);
        project_message = "SAVING PROJECT...";
    }
    return std::nullopt;
}

} // namespace mediaserver::app
