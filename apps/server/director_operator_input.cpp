// Operator input authored at the first sealed frame of a batch: project
// load and save, transport, Preview, switcher, show timing and history.

#include "director_show_loop.hpp"

#include "director_compositor_authoring.hpp"
#include "director_documents.hpp"
#include "director_project_load.hpp"
#include "edit_coalescing_policy.hpp"
#include "playback_runtime.hpp"

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/compositor/compositor_model.hpp>
#include <mediaserver/compositor/compositor_schema.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/persist/project_file.hpp>
#include <mediaserver/persist/settings.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/linked_publication.hpp>
#include <mediaserver/scene/linked_scene.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_project.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/show/show_clock.hpp>
#include <mediaserver/show/switcher_author.hpp>
#include <mediaserver/show/switcher_authority.hpp>
#include <mediaserver/show/switcher_state.hpp>
#include <mediaserver/show/transport_authority.hpp>
#include <mediaserver/show/transport_state.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mediaserver::app {

EditCoalescingPlan *DirectorShowLoop::coalescing_for(core::u64 gesture) {
    const auto found = pending_editor_coalescing.find(gesture);
    return found == pending_editor_coalescing.end() ? nullptr
                                                    : &found->second;
}

void DirectorShowLoop::remember_recent_project(std::string_view path) {
    if (!recent_project_settings_file.has_value()) {
        return;
    }
    auto remembered = persist::save_recent_project_path(
        *recent_project_settings_file, native_path_from_utf8(path));
    if (!remembered && !remembered.error().committed) {
        report_recent_project_error("could not update", remembered.error());
    } else if (!remembered) {
        report_recent_project_error("updated without confirmed durability",
                                    remembered.error());
    }
    if (remembered || remembered.error().committed) {
        auto paths = persist::load_recent_project_paths(
            *recent_project_settings_file);
        if (paths) {
            recent_project_paths.clear();
            for (const auto &item : paths.value()) {
                recent_project_paths.push_back(native_path_to_utf8(item));
            }
        } else {
            report_recent_project_error("could not read", paths.error());
        }
    }
}

void DirectorShowLoop::clear_pending_project_authoring() {
    pending_edits.clear();
    pending_world_edits.clear();
    pending_history.reset();
    pending_seek.reset();
    pending_preview_seek.reset();
    preview_commands.clear();
    pending_range.reset();
    pending_show_rate.reset();
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
    pending_renames.clear();
    pending_highlights.clear();
    pending_timeline_edits.clear();
    pending_source_edits.clear();
    asset_viewer_request.reset();
    pending_group_edit.reset();
    pending_selection_set_edit.reset();
    pending_mask_edit.reset();
    pending_clip_paste.reset();
    pending_scene_duplicates.clear();
    clip_clipboard.clear();
    compositor_clipboard.clear();
    linked_clipboard.reset(); pending_linked_paste.reset();
    pending_key_paste.reset();
    key_clipboard.clear();
    pending_project_settings.reset();
    pending_properties.clear();
    pending_generics.clear();
    pending_locks.clear();
    pending_adds.clear();
    pending_asset_imports.clear();
    pending_asset_revisions.clear();
    pending_asset_deletes.clear();
    pending_asset_proxy_edits.clear();
    pending_asset_library_edits.clear();
    pending_source_replacements.clear();
    pending_effects.clear();
    pending_film_grain_effect_edits.clear();
    pending_effect_deletes.clear();
    pending_switcher.reset();
    pending_hardware_places.clear();
    pending_hardware_moves.clear();
    pending_hardware_links.clear();
    pending_hardware_properties.clear();
    pending_hardware_control_port.reset();
    pending_hardware_canvas.reset();
    pending_sync_policy.reset();
    pending_hardware_output_binding_edits.clear();
    pending_hardware_output_binding_deletes.clear();
    pending_hardware_deletes.clear();
    pending_feed_map_adds.clear();
    pending_feed_map_edits.clear();
    pending_feed_map_deletes.clear();
    pending_feed_projection_edits.clear();
    pending_feed_organize.clear();
    pending_feed_layout.reset();
    pending_feed_surface_adds.clear();
    pending_feed_surface_edits.clear();
    pending_feed_surface_deletes.clear();
    pending_compositor_adds.clear();
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
    pending_compositor_gesture = 0U;
    pending_deletes.clear();
    pending_delete_gesture = 0U;
    pending_editor_coalescing.clear();
    pending_editor_coalescing_close = false;
    editor_coalescing_policy.close();
    scheduler.end_coalescing();
}

// Entering Preview seeds its playhead from wherever the show currently is
// and freezes it there, so the operator starts from the picture they were
// just looking at and the show carries on without them.
void DirectorShowLoop::enter_preview(cmd::FrameNumber at) {
    rehearsal_mode = true;
    show::TransportAuthority seeded = transport;
    static_cast<void>(seeded.pause(at));
    preview_transport = seeded;
}

void DirectorShowLoop::leave_preview() noexcept {
    rehearsal_mode = false;
    preview_transport.reset();
}

// Which scene an authoring gesture belongs to. Preview edits
// the rehearsal scene; otherwise the operator is editing
// Program directly. A document with no switcher yet has one
// scene, and it is the lowest-id stage.
doc::NodeId DirectorShowLoop::scene_being_authored(const OperatorFrame &op) {
    const auto &projected_switcher = op.projected_switcher;
    if (projected_switcher.has_value()) {
        return rehearsal_mode
                   ? projected_switcher->rehearsal_scene
                   : projected_switcher->program_scene;
    }
    scene::SceneModel authored_scenes;
    authored_scenes.reset(scheduler.projected_document());
    const auto infos = authored_scenes.scenes();
    return infos.empty() ? doc::NodeId{} : infos.front().id;
}

PhaseExit DirectorShowLoop::apply_project_load(DirectorTick &tick,
                                               OperatorFrame &op) {
    auto &repace_after_seal = tick.repace_after_seal;
    const auto &frame = op.frame;
    auto &project_loaded_this_frame = op.project_loaded_this_frame;
    if (pending_project_load.has_value()) {
        const editor::ProjectFileRequest request =
            std::move(*pending_project_load);
        pending_project_load.reset();

        auto loaded =
            pending_preloaded_project.has_value()
                ? persist::ProjectLoadResult::success(
                      std::move(*pending_preloaded_project))
                : persist::load_project_file(
                      native_path_from_utf8(request.path));
        pending_preloaded_project.reset();
        if (!loaded) {
            project_message =
                project_file_failure("LOADING", loaded.error());
            std::cerr << "could not load project '" << request.path
                      << "' (code "
                      << static_cast<unsigned>(loaded.error().code)
                      << ')';
            if (loaded.error().system_error) {
                std::cerr << ": "
                          << loaded.error().system_error.message();
            }
            std::cerr << '\n';
        } else {
            doc::Root replacement = scene::without_publication_locks(scene::without_asset_commits(std::move(loaded).value()));
            std::size_t enriched_media_nodes{};
            try {
                MediaRateEnrichment enrichment =
                    enrich_media_frame_rates(replacement);
                replacement = std::move(enrichment.project);
                enriched_media_nodes = enrichment.updated_nodes;
            } catch (const std::bad_alloc &) {
                std::cerr << "could not enrich loaded media FPS "
                             "metadata: "
                             "allocation failed\n";
            } catch (const std::length_error &) {
                std::cerr << "could not enrich loaded media FPS "
                             "metadata: "
                             "project limits exceeded\n";
            }
            show::TransportAuthority replacement_transport =
                transport;
            const doc::Node *const raw_transport =
                replacement.get(show::transport_node_id);
            const auto loaded_transport =
                show::read_transport(replacement);
            const bool malformed_transport =
                raw_transport != nullptr &&
                std::string_view{raw_transport->type} ==
                    show::transport_node_type &&
                !loaded_transport.has_value();
            bool transport_valid = !malformed_transport;
            if (transport_valid && loaded_transport.has_value()) {
                transport_valid = restore_project_transport(
                    replacement_transport, *loaded_transport,
                    frame);
            } else if (transport_valid) {
                // A project predating persisted transport settings
                // gets the launch/default cadence (24 fps unless an
                // explicit --show-rate was supplied), never the
                // settings of whichever project happened to be
                // open.
                auto legacy_transport =
                    show::TransportAuthority::create(
                        clock_config(options), frame);
                if (legacy_transport) {
                    replacement_transport =
                        std::move(legacy_transport).value();
                } else {
                    transport_valid = false;
                }
            }
            if (!transport_valid) {
                project_message = "ERROR LOADING PROJECT TRANSPORT";
                std::cerr << "project transport state is invalid\n";
            } else {
                // Settle before staging so the preflight batch is
                // still exact when the ordinary end-of-frame
                // transport settlement runs below.
                static_cast<void>(
                    replacement_transport.settle(frame));
                // Re-anchor the saved playhead to this Director's
                // current control frame. This preserves range,
                // direction, state, and position without importing
                // a stale control-frame ordinal from another run.
                replacement =
                    replacement.set(show::make_transport_node(
                        replacement_transport.anchor()));
                if (!normalize_project_switcher(replacement,
                                                frame)) {
                    project_message =
                        "ERROR LOADING PROJECT SWITCHER";
                    std::cerr
                        << "project switcher state is invalid\n";
                } else {
                    auto staged = stage_project_load(
                        scheduler, replacement, frame, command_log,
                        frame_durability);
                    if (!staged) {
                        const auto &error = staged.error();
                        switch (error.code) {
                        case ProjectLoadStageErrorCode::
                            ScheduleFailed:
                            project_message =
                                "ERROR LOADING PROJECT: "
                                "SCHEDULER "
                                "REJECTED REPLACEMENT";
                            break;
                        case ProjectLoadStageErrorCode::
                            PrepareFailed:
                            project_message =
                                "ERROR LOADING PROJECT: "
                                "FRAME "
                                "COULD NOT BE PREPARED";
                            break;
                        case ProjectLoadStageErrorCode::
                            WirePreflightFailed:
                            project_message =
                                error.command_log_error
                                            .has_value() &&
                                        command_log_limit_error(
                                            *error
                                                 .command_log_error)
                                    ? "ERROR LOADING "
                                      "PROJECT: "
                                      "PROJECT "
                                      "EXCEEDS FRAME "
                                      "TRANSFER "
                                      "LIMITS"
                                    : "ERROR LOADING "
                                      "PROJECT: "
                                      "FRAME "
                                      "TRANSFER "
                                      "PREFLIGHT "
                                      "REJECTED";
                            break;
                        case ProjectLoadStageErrorCode::
                            WalPreflightFailed:
                            project_message =
                                error.command_log_error
                                            .has_value() &&
                                        command_log_limit_error(
                                            *error
                                                 .command_log_error)
                                    ? "ERROR LOADING "
                                      "PROJECT: "
                                      "PROJECT EXCEEDS "
                                      "WAL "
                                      "LIMITS"
                                    : "ERROR LOADING "
                                      "PROJECT: WAL "
                                      "PREFLIGHT "
                                      "REJECTED";
                            break;
                        case ProjectLoadStageErrorCode::AbortFailed:
                            project_message =
                                "ERROR LOADING PROJECT: "
                                "FRAME "
                                "PREFLIGHT FAILED";
                            break;
                        case ProjectLoadStageErrorCode::
                            AllocationFailed:
                            project_message =
                                "ERROR LOADING PROJECT: "
                                "NOT ENOUGH "
                                "MEMORY";
                            break;
                        }
                        std::cerr
                            << "could not stage loaded project "
                               "for "
                               "frame "
                            << frame.value() << " (code "
                            << static_cast<unsigned>(error.code)
                            << ")\n";
                    } else {
                        static_assert(
                            std::is_nothrow_move_assignable_v<
                                cmd::DirectorScheduler>);
                        scheduler = std::move(staged).value();
                        transport = replacement_transport;
                        repace_after_seal = transport.anchor().rate;
                        authored = transport.anchor();
                        saved_project_snapshot =
                            scheduler.projected_document();
                        project_explicitly_dirty =
                            enriched_media_nodes != 0U;
                        project_path = request.path;
                        // An untitled project leaves the recent
                        // project alone, and makes its own Blender
                        // file when it first opens one.
                        if (!project_path.empty()) {
                            remember_recent_project(project_path);
                        }
                        unsaved_blender_name.clear();
                        project_message =
                            project_path.empty() ? "NEW PROJECT"
                            : enriched_media_nodes == 0U
                                ? "PROJECT LOADED"
                                : "PROJECT LOADED - MEDIA "
                                  "FPS "
                                  "UPDATED (" +
                                      std::to_string(
                                          enriched_media_nodes) +
                                      ')';
                        if (enriched_media_nodes != 0U) {
                            std::cerr
                                << "enriched "
                                << enriched_media_nodes
                                << " loaded media node(s) with "
                                   "native FPS; save the project "
                                   "to persist metadata\n";
                        }
                        leave_preview();
                        next_new_node = 100'000U;
                        ++project_generation;
                        // Ids from the last project's RESCAN mean
                        // nothing in this one.
                        changed_library_sources.clear();
                        changed_library_assets.clear();
                        director_pose_predictors.reset();
                        pending_asset_commit.reset();
                        last_asset_commit_digest = 0;
                        last_asset_commit_frame = 0;
                        last_asset_commit_stage = doc::NodeId{};
                        // A replacement project may carry the
                        // same document version as the previous
                        // one, while project-relative asset
                        // resolution depends on the newly chosen
                        // project path. Force the runtime asset
                        // overlay to be rebuilt on the next draw.
                        assets_loaded = false;
                        clear_pending_project_authoring();
                        project_loaded_this_frame = true;
                    }
                }
            }
        }
    }
    return std::nullopt;
}

void DirectorShowLoop::apply_preview_transport(OperatorFrame &op) {
    const auto &frame = op.frame;
    auto &project_loaded_this_frame = op.project_loaded_this_frame;
    if (preview_transport.has_value()) {
        // Range, loop, and cadence are one show-wide setting, so
        // Preview follows the live transport for those. Everything
        // else about where Preview sits is local and authored
        // nowhere: no command, no anchor, no replication.
        const show::TransportAnchor &live = transport.anchor();
        const show::TransportAnchor &previewed =
            preview_transport->anchor();
        if (previewed.rate != live.rate) {
            static_cast<void>(
                preview_transport->set_rate(live.rate, frame));
        }
        if (previewed.first_frame != live.first_frame ||
            previewed.last_frame != live.last_frame ||
            previewed.loop != live.loop) {
            static_cast<void>(preview_transport->set_range(
                live.first_frame, live.last_frame, live.loop,
                frame));
        }
        if (previewed.cues_armed != live.cues_armed) {
            static_cast<void>(preview_transport->set_cues_armed(
                live.cues_armed, frame));
        }
        if (!project_loaded_this_frame) {
            for (const auto command : preview_commands) {
                apply_command(*preview_transport, command, frame);
            }
            if (pending_preview_seek.has_value()) {
                if (preview_transport->state() ==
                    show::TransportState::Playing) {
                    static_cast<void>(
                        preview_transport->pause(frame));
                }
                static_cast<void>(preview_transport->seek(
                    show::ShowFrame{
                        std::min(std::max(*pending_preview_seek,
                                          live.first_frame.value()),
                                 live.last_frame.value())},
                    frame));
            }
        }
    }
    preview_commands.clear();
    pending_preview_seek.reset();
}

void DirectorShowLoop::prepare_authoring_context(OperatorFrame &op) {
    const auto &frame = op.frame;
    auto &projected_switcher = op.projected_switcher;
    auto &scene_edits_locked = op.scene_edits_locked;
    projected_switcher =
        show::read_switcher(scheduler.projected_document());
    if (projected_switcher.has_value() &&
        show::derive_switcher_progress(*projected_switcher, frame)
            .active) {
        scene::SceneModel projected_scenes;
        projected_scenes.reset(scheduler.projected_document());
        const auto infos = projected_scenes.scenes();
        const auto from = std::find_if(
            infos.begin(), infos.end(),
            [&](const scene::SceneInfo &info) {
                return info.id ==
                       projected_switcher->transition_from;
            });
        const auto to = std::find_if(
            infos.begin(), infos.end(),
            [&](const scene::SceneInfo &info) {
                return info.id == projected_switcher->transition_to;
            });
        scene_edits_locked =
            from != infos.end() && to != infos.end() &&
            show::dissolve_canvases_compatible(from->canvas,
                                               to->canvas);
    }
}

PhaseExit DirectorShowLoop::apply_switcher_request(OperatorFrame &op) {
    const auto &frame = op.frame;
    if (pending_switcher.has_value()) {
        const editor::SwitcherRequest request = *pending_switcher;
        pending_switcher.reset();
        const cmd::UndoContextId switcher_context{5U};
        const cmd::TransactionId switcher_transaction{
            frame.value()};

        scene::SceneModel project_scenes;
        project_scenes.reset(scheduler.projected_document());
        const std::vector<scene::SceneInfo> scenes =
            project_scenes.scenes();
        std::optional<show::SwitcherAnchor> anchor =
            show::read_switcher(scheduler.projected_document());

        const auto scene_for =
            [&](scene::SceneId id) -> const scene::SceneInfo * {
            const auto found =
                std::find_if(scenes.begin(), scenes.end(),
                             [id](const scene::SceneInfo &info) {
                                 return info.id == id;
                             });
            return found == scenes.end() ? nullptr : &*found;
        };
        const auto dissolve_pair_compatible =
            [&](scene::SceneId program, scene::SceneId rehearsal) {
                const scene::SceneInfo *const program_info =
                    scene_for(program);
                const scene::SceneInfo *const rehearsal_info =
                    scene_for(rehearsal);
                return program_info != nullptr &&
                       rehearsal_info != nullptr &&
                       show::dissolve_canvases_compatible(
                           program_info->canvas,
                           rehearsal_info->canvas);
            };

        const auto author_switcher =
            [&](const show::SwitcherAnchor &next) {
                return static_cast<bool>(
                    show::schedule_switcher_anchor(
                        scheduler, next, frame, switcher_context,
                        switcher_transaction));
            };

        // A scene copy has to bring its composite with it. The
        // scene layer owns the drawables; the compositor graph owns
        // how they are stacked and treated, and a copy that lost
        // the graph would look nothing like the scene it came from
        // the moment it was promoted.
        const scene::SceneId legacy_graph_owner =
            anchor.has_value()
                ? anchor->program_scene
                : (!scenes.empty() ? scenes.front().id
                                   : scene::SceneId{});
        const auto author_graph_clone =
            [&](const scene::SceneClonePlan &plan) -> bool {
            if (legacy_graph_owner == scene::SceneId{}) {
                return true;
            }
            auto planned = compositor::plan_clone_graph(
                scheduler.projected_document(), plan.source,
                plan.destination, legacy_graph_owner, plan.remap,
                doc::NodeId{next_new_node});
            if (!planned) {
                return false;
            }
            const compositor::GraphClonePlan graph =
                std::move(planned).value();
            const bool cloned_output = std::any_of(
                graph.puts.begin(), graph.puts.end(),
                [](const doc::Node &node) {
                    return node.type ==
                           compositor::schema::output_type;
                });
            for (const doc::Node &node : graph.puts) {
                if (!scheduler.schedule_edit(
                        cmd::PutNode{node}, cmd::EraseNode{node.id},
                        switcher_context, switcher_transaction,
                        frame)) {
                    return false;
                }
            }
            for (const auto &[source, destination] : graph.remap) {
                static_cast<void>(source);
                next_new_node = std::max(next_new_node,
                                         destination.value() + 1U);
            }
            if (!cloned_output) {
                while (scheduler.projected_document().contains(
                    doc::NodeId{next_new_node})) {
                    ++next_new_node;
                }
                const doc::Node output =
                    default_compositor_output_node(
                        doc::NodeId{next_new_node++},
                        plan.destination);
                if (!scheduler.schedule_edit(
                        cmd::PutNode{output},
                        cmd::EraseNode{output.id}, switcher_context,
                        switcher_transaction, frame)) {
                    return false;
                }
            }
            return true;
        };

        if (request.action ==
            editor::SwitcherAction::ToggleRehearse) {
            if (rehearsal_mode) {
                leave_preview();
            } else if (anchor.has_value()) {
                enter_preview(frame);
            } else if (!scenes.empty()) {
                auto clone = scene::plan_clone_scene(
                    scheduler.projected_document(),
                    scenes.front().id, doc::NodeId{next_new_node});
                if (!clone) {
                    std::cerr << "could not clone Program into "
                                 "Rehearsal\n";
                    return 1;
                }
                scene::SceneClonePlan plan =
                    std::move(clone).value();
                for (doc::Node &node : plan.puts) {
                    if (node.id == plan.destination) {
                        const std::string rehearsal_name =
                            scenes.front().name.empty()
                                ? "Rehearsal"
                                : scenes.front().name +
                                      " Rehearsal";
                        node.properties.insert_or_assign(
                            std::string{scene::schema::scene_name},
                            doc::PropertyValue{rehearsal_name});
                    }
                    if (!scheduler.schedule_edit(
                            cmd::PutNode{node},
                            cmd::EraseNode{node.id},
                            switcher_context, switcher_transaction,
                            frame)) {
                        std::cerr
                            << "could not author Rehearsal clone\n";
                        return 1;
                    }
                }
                for (const auto &[source, destination] :
                     plan.remap) {
                    static_cast<void>(source);
                    next_new_node = std::max(
                        next_new_node, destination.value() + 1U);
                }
                if (!author_graph_clone(plan)) {
                    std::cerr << "could not author the Rehearsal "
                                 "compositor graph\n";
                    return 1;
                }
                auto switcher_created =
                    show::SwitcherAuthority::create(
                        plan.source, plan.destination, frame);
                if (!switcher_created ||
                    !author_switcher(std::move(switcher_created)
                                         .value()
                                         .anchor())) {
                    std::cerr << "could not author "
                                 "Program/Rehearsal slots\n";
                    return 1;
                }
                enter_preview(frame);
            }
        } else if (request.action == editor::SwitcherAction::Swap) {
            // Swap promotes the scene the operator has been
            // rehearsing. Its conditions belong to the switcher
            // itself - a Preview scene, no transition already
            // running, and for a dissolve one shared canvas - plus
            // the STRICT synchronization hold the surface reports.
            // Whether render nodes hold the media is reported on
            // the Output Monitor, not enforced here: distributing
            // content to them is an external concern in this build.
            if (anchor && !scene::publication::text(scheduler.projected_document().get(anchor->rehearsal_scene),scene::publication::revision).empty()) {
                pending_publication=true;
            } else if (program_sync_held()) {
                project_message =
                    std::string{director_sync.readiness.headline()};
            } else if (anchor.has_value() &&
                       !show::derive_switcher_progress(*anchor,
                                                       frame)
                            .active &&
                       (request.transition_control_frames == 0U ||
                        dissolve_pair_compatible(
                            anchor->program_scene,
                            anchor->rehearsal_scene))) {
                auto authority = show::SwitcherAuthority::create(
                    anchor->program_scene, anchor->rehearsal_scene,
                    frame);
                auto swapped =
                    authority
                        ? std::move(authority).value().swap(
                              frame,
                              request.transition_control_frames)
                        : core::Result<show::SwitcherAnchor,
                                       show::SwitcherCommandError>::
                              failure(show::SwitcherCommandError::
                                          InvalidScene);
                if (!swapped ||
                    !author_switcher(std::move(swapped).value())) {
                    std::cerr << "could not swap Program and "
                                 "Rehearsal\n";
                    return 1;
                }
                // Preview now holds what Program was showing.
                // Re-seed its playhead so the operator picks up
                // from the picture that just came off air.
                if (rehearsal_mode) {
                    enter_preview(frame);
                }
            }
        } else if (request.action ==
                   editor::SwitcherAction::SelectRehearsal) {
            const scene::SceneId program =
                anchor.has_value()
                    ? anchor->program_scene
                    : (!scenes.empty() ? scenes.front().id
                                       : scene::SceneId{});
            // Preview accepts any scene. Whether the two canvases
            // allow a dissolve is decided when the operator asks
            // for one, not when they choose what to work on.
            if (request.scene != scene::publication::render_scene(scheduler.projected_document(),program) && request.scene != program &&
                (!anchor.has_value() ||
                 !show::derive_switcher_progress(*anchor, frame)
                      .active)) {
                auto authority = show::SwitcherAuthority::create(
                    program,
                    anchor.has_value() ? anchor->rehearsal_scene
                                       : request.scene,
                    frame);
                std::optional<show::SwitcherAnchor> selected;
                if (authority) {
                    auto state = std::move(authority).value();
                    if (!anchor.has_value() ||
                        request.scene == anchor->rehearsal_scene) {
                        selected = state.anchor();
                    } else {
                        auto changed = state.set_rehearsal(
                            request.scene, frame);
                        if (changed) {
                            selected = std::move(changed).value();
                        }
                    }
                }
                if (!selected.has_value() ||
                    !author_switcher(*selected)) {
                    std::cerr
                        << "could not select Rehearsal scene\n";
                    return 1;
                }
                enter_preview(frame);
            }
        } else if (request.action ==
                       editor::SwitcherAction::DuplicateScene &&
                   (!anchor.has_value() ||
                    !show::derive_switcher_progress(*anchor, frame)
                         .active)) {
            const scene::SceneInfo *const source_info =
                scene_for(request.scene);
            if (source_info != nullptr) {
                auto cloned = scene::plan_clone_scene(
                    scheduler.projected_document(), request.scene,
                    doc::NodeId{next_new_node});
                if (!cloned) {
                    std::cerr << "could not plan a scene copy\n";
                    return 1;
                }
                scene::SceneClonePlan plan =
                    std::move(cloned).value();
                for (doc::Node &node : plan.puts) {
                    if (node.id == plan.destination) {
                        const std::string copy_name =
                            source_info->name.empty()
                                ? "Scene Copy"
                                : source_info->name + " Copy";
                        node.properties.insert_or_assign(
                            std::string{scene::schema::scene_name},
                            doc::PropertyValue{copy_name});
                    }
                    if (!scheduler.schedule_edit(
                            cmd::PutNode{node},
                            cmd::EraseNode{node.id},
                            switcher_context, switcher_transaction,
                            frame)) {
                        std::cerr
                            << "could not author a scene copy\n";
                        return 1;
                    }
                }
                for (const auto &[source, destination] :
                     plan.remap) {
                    static_cast<void>(source);
                    next_new_node = std::max(
                        next_new_node, destination.value() + 1U);
                }
                if (!author_graph_clone(plan)) {
                    std::cerr << "could not author the scene copy "
                                 "compositor graph\n";
                    return 1;
                }
                const scene::SceneId program =
                    anchor.has_value()
                        ? anchor->program_scene
                        : (!scenes.empty() ? scenes.front().id
                                           : request.scene);
                auto copied_switcher =
                    show::SwitcherAuthority::create(
                        program, plan.destination, frame);
                if (!copied_switcher ||
                    !author_switcher(std::move(copied_switcher)
                                         .value()
                                         .anchor())) {
                    std::cerr
                        << "could not assign the scene copy to "
                           "Rehearsal\n";
                    return 1;
                }
                enter_preview(frame);
            }
        } else if (request.action ==
                       editor::SwitcherAction::CreateScene &&
                   (!anchor.has_value() ||
                    !show::derive_switcher_progress(*anchor, frame)
                         .active)) {
            const scene::SceneId existing_program =
                anchor.has_value()
                    ? anchor->program_scene
                    : (!scenes.empty() ? scenes.front().id
                                       : scene::SceneId{});
            const scene::SceneInfo *const program_info =
                scene_for(existing_program);
            const scene::Canvas created_canvas =
                program_info != nullptr
                    ? program_info->canvas
                    : scene::Canvas{1'920U, 1'080U};
            while (scheduler.projected_document().contains(
                doc::NodeId{next_new_node})) {
                ++next_new_node;
            }
            const scene::SceneId created_scene{next_new_node++};
            while (scheduler.projected_document().contains(
                doc::NodeId{next_new_node})) {
                ++next_new_node;
            }
            const doc::NodeId camera_id{next_new_node++};
            while (scheduler.projected_document().contains(
                doc::NodeId{next_new_node})) {
                ++next_new_node;
            }
            const doc::NodeId output_id{next_new_node++};
            doc::Node stage{};
            stage.id = created_scene;
            stage.type = std::string{scene::schema::stage_type};
            // A scene the artist added in Blender keeps its name
            // and is linked to that Blender scene from the start.
            const auto from_blender = std::exchange(blender_scene_creating, std::nullopt);
            stage.properties.emplace(
                std::string{scene::schema::scene_name},
                doc::PropertyValue{from_blender
                    ? from_blender->name
                    : "Scene " + std::to_string(scenes.size() + 1U)});
            if (from_blender) {
                stage.properties.emplace(std::string{scene::schema::blender_link_source},
                    doc::PropertyValue{from_blender->source});
                if (!from_blender->file.empty())
                    stage.properties.emplace(std::string{scene::schema::blender_file},
                        doc::PropertyValue{from_blender->file});
            }
            stage.properties.emplace(
                std::string{scene::schema::canvas_width},
                doc::PropertyValue{static_cast<std::int64_t>(
                    created_canvas.width)});
            stage.properties.emplace(
                std::string{scene::schema::canvas_height},
                doc::PropertyValue{static_cast<std::int64_t>(
                    created_canvas.height)});
            doc::Node camera{};
            camera.id = camera_id;
            camera.type = std::string{scene::schema::camera_type};
            camera.properties.emplace(
                std::string{scene::schema::scene_id},
                doc::PropertyValue{static_cast<std::int64_t>(
                    created_scene.value())});
            // A scene's default camera starts locked so a stray drag cannot move it.
            camera.properties.emplace(std::string{scene::schema::locked},
                                      doc::PropertyValue{true});
            camera.properties.emplace(
                std::string{scene::schema::position_z},
                doc::PropertyValue{6.0});
            const doc::Node output = default_compositor_output_node(
                output_id, created_scene);
            for (const doc::Node &node : {stage, camera, output}) {
                if (!scheduler.schedule_edit(
                        cmd::PutNode{node}, cmd::EraseNode{node.id},
                        switcher_context, switcher_transaction,
                        frame)) {
                    std::cerr << "could not create a scene\n";
                    return 1;
                }
            }
            const scene::SceneId program =
                existing_program != scene::SceneId{}
                    ? existing_program
                    : created_scene;
            if (program != created_scene) {
                auto scene_switcher =
                    show::SwitcherAuthority::create(
                        program, created_scene, frame);
                if (!scene_switcher ||
                    !author_switcher(std::move(scene_switcher)
                                         .value()
                                         .anchor())) {
                    std::cerr << "could not assign the new scene "
                                 "to Rehearsal\n";
                    return 1;
                }
                enter_preview(frame);
            }
        } else if (request.action ==
                       editor::SwitcherAction::DeleteScene &&
                   (!anchor.has_value() ||
                    !show::derive_switcher_progress(*anchor, frame)
                         .active)) {
            const scene::SceneId program =
                anchor.has_value()
                    ? anchor->program_scene
                    : (!scenes.empty() ? scenes.front().id
                                       : scene::SceneId{});
            const auto* active=scheduler.projected_document().get(program);
            const bool retained=request.scene.value() == scene::publication::integer(active,scene::publication::candidate)
                || request.scene.value() == scene::publication::integer(active,scene::publication::rollback)
                || request.scene.value() == scene::publication::integer(
                    scheduler.projected_document().get(scene::publication::render_scene(scheduler.projected_document(),program)),scene::publication::previous);
            if (retained) publication_message="This scene is retained for publication or rollback.";
            // Protect active, preparing and rollback revisions.
            if (request.scene != program && !retained &&
                scene_for(request.scene) != nullptr) {
                auto planned = scene::plan_delete_scene(
                    scheduler.projected_document(), request.scene);
                if (!planned) {
                    std::cerr << "could not plan scene deletion\n";
                    return 1;
                }
                if (anchor.has_value() &&
                    anchor->rehearsal_scene == request.scene) {
                    const auto fallback = std::find_if(
                        scenes.begin(), scenes.end(),
                        [&](const scene::SceneInfo &info) {
                            return info.id != program &&
                                   info.id != request.scene;
                        });
                    if (fallback != scenes.end()) {
                        auto replacement =
                            show::SwitcherAuthority::create(
                                program, fallback->id, frame);
                        if (!replacement ||
                            !author_switcher(std::move(replacement)
                                                 .value()
                                                 .anchor())) {
                            std::cerr
                                << "could not replace the "
                                   "deleted Rehearsal scene\n";
                            return 1;
                        }
                        if (rehearsal_mode) {
                            enter_preview(frame);
                        }
                    } else {
                        if (!schedule_erase_node(
                                scheduler, show::switcher_node_id,
                                switcher_context,
                                switcher_transaction, frame)) {
                            std::cerr << "could not remove the "
                                         "empty Rehearsal slot\n";
                            return 1;
                        }
                        leave_preview();
                    }
                }
                for (const doc::NodeId id :
                     planned.value().erases) {
                    if (!schedule_erase_node(
                            scheduler, id, switcher_context,
                            switcher_transaction, frame)) {
                        std::cerr << "could not delete scene\n";
                        return 1;
                    }
                }
            }
        }
    }
    return std::nullopt;
}

PhaseExit DirectorShowLoop::apply_show_timing_requests(DirectorTick &tick,
                                                       OperatorFrame &op) {
    auto &repace_after_seal = tick.repace_after_seal;
    const auto &frame = op.frame;
    if (pending_cues_armed.has_value()) {
        static_cast<void>(
            transport.set_cues_armed(*pending_cues_armed, frame));
        project_explicitly_dirty = true;
        pending_cues_armed.reset();
    }
    if (pending_range) {
        // Set the show length and loop, then let the ordinary
        // anchor authoring below replicate the new bounds.
        static_cast<void>(transport.set_range(
            show::ShowFrame{pending_range->first},
            show::ShowFrame{pending_range->last},
            pending_range->loop, frame));
        project_explicitly_dirty = true;
        pending_range.reset();
    }
    if (pending_show_rate.has_value()) {
        const show::FrameRate requested{
            pending_show_rate->numerator,
            pending_show_rate->denominator};
        if (!transport.set_rate(requested, frame)) {
            std::cerr
                << "could not apply the requested show rate\n";
            return 1;
        }
        repace_after_seal = requested;
        project_explicitly_dirty = true;
        pending_show_rate.reset();
    }
    if (pending_seek) {
        // Scrubbing is a paused operation, like the keyboard step
        // and jump commands: pause, then seek to the dragged frame,
        // which may lie outside In/Out. The new anchor replicates
        // like any other transport change.
        if (transport.state() == show::TransportState::Playing) {
            static_cast<void>(transport.pause(frame));
        }
        static_cast<void>(
            transport.seek(show::ShowFrame{*pending_seek}, frame));
        pending_seek.reset();
    }
    return std::nullopt;
}

PhaseExit DirectorShowLoop::apply_history_request(OperatorFrame &op) {
    const auto &frame = op.frame;
    if (pending_history.has_value()) {
        // Navigate the editor's most recently authored scene or
        // compositor context one whole gesture at a time, or jump to
        // a listed step. Every action authors fresh commands;
        // NoHistory only means that the requested direction has no
        // next step.
        const editor::HistoryRequest request = *pending_history;
        pending_history.reset();
        const cmd::UndoContextId history_context =
            undo_context_for(request.context);
        // Asset activation advances after a bake/probe edit. It
        // must survive Undo without conflicting with authored fields.
        constexpr std::array runtime_properties{
            scene::schema::asset_commit_digest,
            scene::schema::asset_commit_control,
            scene::schema::asset_commit_show};
        if (request.action == editor::HistoryAction::Jump) {
            std::vector<cmd::TransactionId> fresh;
            fresh.reserve(static_cast<std::size_t>(request.transaction_count));
            for (core::u64 offset = 0U; offset < request.transaction_count;
                 ++offset) {
                fresh.emplace_back(request.transaction + offset);
            }
            auto jumped = scheduler.schedule_history_jump(
                history_context, cmd::HistoryStepId{request.target_step},
                fresh, frame, runtime_properties);
            if (!jumped) {
                // The editor drew a history this frame may have moved
                // past (a retired step, a concurrent edit) or one that
                // conflicts with another context. Nothing was authored,
                // so the show carries on and the strip redraws.
                std::cerr << "history jump to step " << request.target_step
                          << " refused (code "
                          << static_cast<unsigned>(jumped.error()) << ")\n";
            }
            return std::nullopt;
        }
        const bool undo = request.action == editor::HistoryAction::Undo;
        auto navigated =
            undo ? scheduler.schedule_undo(
                       history_context,
                       cmd::TransactionId{request.transaction},
                       frame, runtime_properties)
                 : scheduler.schedule_redo(
                       history_context,
                       cmd::TransactionId{request.transaction},
                       frame, runtime_properties);
        if (!navigated && navigated.error() !=
                              cmd::DirectorUndoError::NoHistory) {
            std::cerr << "could not author a scene-edit "
                      << (undo ? "undo" : "redo") << " for frame "
                      << frame.value() << '\n';
            return 1;
        }
    }
    return std::nullopt;
}

PhaseExit DirectorShowLoop::apply_project_save(OperatorFrame &op) {
    const auto &frame = op.frame;
    if (pending_project_save.has_value()) {
        const editor::ProjectFileRequest request =
            std::move(*pending_project_save);
        pending_project_save.reset();
        doc::Root snapshot = scene::without_asset_commits(scheduler.projected_document());
        show::TransportAnchor saved_transport = transport.anchor();
        saved_transport.anchor_show_frame =
            transport.frame_at(frame);
        saved_transport.anchor_control_frame = frame;
        snapshot = snapshot.set(
            show::make_transport_node(saved_transport));
        auto saved = persist::save_project_file(
            native_path_from_utf8(request.path), snapshot);
        const bool installed = !saved && saved.error().committed;
        if (!saved && !installed) {
            project_message =
                project_file_failure("SAVING", saved.error());
            std::cerr << "could not save project '" << request.path
                      << "' (code "
                      << static_cast<unsigned>(saved.error().code)
                      << ')';
            if (saved.error().system_error) {
                std::cerr << ": "
                          << saved.error().system_error.message();
            }
            std::cerr << '\n';
        } else {
            saved_project_snapshot = scheduler.projected_document();
            project_explicitly_dirty = false;
            const bool asset_base_changed =
                project_path != request.path;
            project_path = request.path;
            // Save As changes the base of project-relative asset
            // paths even though authored content is unchanged.
            if (asset_base_changed) {
                assets_loaded = false;
            }
            remember_recent_project(project_path);
            project_message =
                installed ? "ERROR SAVE: DURABILITY NOT CONFIRMED"
                          : "PROJECT SAVED";
            if (installed) {
                std::cerr
                    << "project '" << request.path
                    << "' was replaced, but durable directory "
                       "sync failed";
                if (saved.error().system_error) {
                    std::cerr
                        << ": "
                        << saved.error().system_error.message();
                }
                std::cerr << '\n';
            }
        }
    }
    return std::nullopt;
}

} // namespace mediaserver::app
