// Evaluates the committed control frame after sealing: scene, feed map,
// hardware, sync policy and the asset cohort, then presents a non-editor
// Director surface.

#include "director_show_loop.hpp"

#include "director_live_data.hpp"
#include "output_playback_buffer.hpp"
#include "playback_media.hpp"
#include "playback_runtime.hpp"
#include "project_assets.hpp"
#include "proxy_playback.hpp"
#include "tracked_camera_bridge.hpp"

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/assets/derivative_profile_id.hpp>
#include <mediaserver/core/asset_publication.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/net/session_protocol.hpp>
#include <mediaserver/render/linked_stage_context.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/lens.hpp>
#include <mediaserver/scene/linked_publication.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/show/presenter.hpp>
#include <mediaserver/show/show_clock.hpp>
#include <mediaserver/show/switcher_state.hpp>
#include <mediaserver/show/transport_state.hpp>
#include <mediaserver/sync/sync_group.hpp>
#include <mediaserver/topology/hardware_map.hpp>
#include <mediaserver/track/pose_channel.hpp>
#include <mediaserver/track/pose_slot.hpp>

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace mediaserver::app {

void DirectorShowLoop::begin_frame(DirectorTick &tick) {
    auto &sealed_this_iteration = tick.sealed_this_iteration;
    auto &control_frame = tick.control_frame;
    auto &input = tick.input;
    auto &stage_show_frame = tick.stage_show_frame;
    auto &stage_playing = tick.stage_playing;
    auto &render_scene = tick.render_scene;
    auto &needs_scene = tick.needs_scene;
    control_frame = scheduler.sealed_through();
    input.show_frame = transport.frame_at(control_frame);
    input.control_frame = control_frame;
    input.rate = transport.anchor().rate;
    input.state = transport.state();
    input.node_index = options.node_index;
    input.held = sealed_this_iteration == 0U;
    // Where the Stage is looking. In Preview that is the operator's own
    // playhead, derived from the same control-frame stream but from a
    // Director-local anchor; otherwise it is exactly the show frame.
    stage_show_frame =
        preview_transport.has_value()
            ? preview_transport->frame_at(control_frame)
            : input.show_frame;
    stage_playing =
        preview_transport.has_value()
            ? preview_transport->state() == show::TransportState::Playing
            : transport.state() == show::TransportState::Playing;

    // The Director previews the whole canvas of the replicated scene, so it
    // shows what the wall shows between its nodes. It evaluates from its
    // own committed document rather than a mailbox.
    render_scene =
        presenter != nullptr && presenter->renders_scenes();
    // The scene is evaluated for the preview or the editor: both work on
    // the same committed scene at the show frame.
    needs_scene = render_scene || options.edit;
}

PhaseExit DirectorShowLoop::evaluate_scene(DirectorTick &tick) {
    const auto &control_frame = tick.control_frame;
    auto &input = tick.input;
    const auto &stage_show_frame = tick.stage_show_frame;
    const auto &needs_scene = tick.needs_scene;
    auto &scene_frame = tick.scene_frame;
    auto &authored_scene_digest = tick.authored_scene_digest;
    auto &stage_scene_frame = tick.stage_scene_frame;
    auto &transition_from_frame = tick.transition_from_frame;
    auto &transition_to_frame = tick.transition_to_frame;
    auto &scene_infos = tick.scene_infos;
    auto &switcher_anchor = tick.switcher_anchor;
    auto &switcher_progress = tick.switcher_progress;
    auto &loop_blend = tick.loop_blend;
    auto &compositor_graph = tick.compositor_graph;
    auto &program_compositor_graph = tick.program_compositor_graph;
    auto &transition_from_graph = tick.transition_from_graph;
    auto &transition_to_graph = tick.transition_to_graph;
    auto &stage_scene = tick.stage_scene;
    assets::AssetRegistry &asset_registry = director_asset_registry;
    if (needs_scene) {
        const auto &document = scheduler.sealed_document();
        if (!scene_loaded || document.version().value() != scene_version) {
            scene_model.reset(document);
            scene_version = document.version().value();
            scene_loaded = true;
        }
        if (!compositor_loaded ||
            document.version().value() != compositor_version) {
            compositor_model.reset(document);
            compositor_version = document.version().value();
            compositor_loaded = true;
        }
        if (!assets_loaded || document.version().value() != asset_version) {
            asset_model.reset(document);
            asset_registry = asset_model.evaluate();
            director_playback_asset_registry =
                options.edit ? editor_playback_registry(asset_registry)
                             : asset_registry;
            if (director_proxy_generator != nullptr) {
                // Only the editor's working derivatives belong on this
                // machine. Authored full-resolution output proxies are
                // generated independently by each output node.
                for (const assets::Asset &entry :
                     director_playback_asset_registry.assets) {
                    if (const auto *const media =
                            std::get_if<assets::MediaAsset>(&entry);
                        media != nullptr &&
                        media->requested_proxy_profile ==
                            assets::DerivativeProfileId::Proxy720pCfrH264) {
                        director_proxy_generator->request(*media);
                    }
                }
            }
            const core::u64 registry_digest =
                assets::digest(asset_registry);
            if (!assets_loaded ||
                registry_digest != asset_registry_digest) {
                director_proxy_playback.reset();
                asset_registry_digest = registry_digest;
            }
            const ProjectAssetManifest manifest =
                collect_project_asset_manifest(document, asset_registry);
            // The Director is one more machine that has to be able to
            // open these paths, and it answers the question the same way
            // a render node does.
            if (!assets_loaded ||
                manifest.digest != asset_manifest_digest) {
                asset_manifest_digest = manifest.digest;
                director_asset_availability = scan_project_assets(manifest);
            }
            asset_version = document.version().value();
            assets_loaded = true;
        } else if (pending_asset_rescan) {
            // The operator asked. This is the only path that reads a
            // source's bytes, and the only one that can report a content
            // mismatch: a show is rebuilt constantly and nothing on the
            // frame path is allowed to wait on a disk read.
            asset_model.reset(document);
            asset_registry = asset_model.evaluate();
            director_playback_asset_registry =
                options.edit ? editor_playback_registry(asset_registry)
                             : asset_registry;
            if (director_proxy_generator != nullptr) {
                for (const assets::Asset &entry :
                     director_playback_asset_registry.assets) {
                    if (const auto *const media =
                            std::get_if<assets::MediaAsset>(&entry);
                        media != nullptr &&
                        media->requested_proxy_profile ==
                            assets::DerivativeProfileId::Proxy720pCfrH264) {
                        director_proxy_generator->request(*media);
                    }
                }
            }
            director_asset_availability = check_project_assets(
                collect_project_asset_manifest(document, asset_registry));
            // Every Library file, used or not, that is no longer the
            // revision it recorded; the Library offers to accept it.
            changed_library_sources =
                find_changed_library_sources(asset_registry);
            project_message = director_asset_availability.ready()
                                  ? "ASSETS VERIFIED"
                                  : "ASSETS INCOMPLETE - SEE LIBRARY";
        }
        pending_asset_rescan = false;
        const scene::SceneTime time{input.show_frame.value(),
                                    input.rate.numerator,
                                    input.rate.denominator};
        // The show's repeat, for the warm-up across its wrap. Program only:
        // Preview runs its own playhead over its own range, and warming it
        // against the show's would name frames it is not heading for.
        const scene::ShowLoop show_loop{
            transport.anchor().first_frame.value(),
            transport.anchor().last_frame.value(), transport.anchor().loop};
        // Preview evaluates the scene it is rehearsing at its own playhead.
        // Program is evaluated at the show frame in the same iteration, so
        // scrubbing one never disturbs the other.
        const scene::SceneTime stage_time{stage_show_frame.value(),
                                          input.rate.numerator,
                                          input.rate.denominator};
        scene_infos = scene_model.scenes();
        switcher_anchor = show::read_switcher(document);
        // A graph object authored before graphs were scene-scoped carries
        // no owner. It used to apply to whatever Program was showing, so
        // that is the scene it is adopted into - which keeps the wall
        // looking exactly as it did when the project was saved.
        const scene::SceneId legacy_graph_owner =
            switcher_anchor.has_value()
                ? switcher_anchor->program_scene
                : (scene_infos.empty() ? scene::SceneId{}
                                       : scene_infos.front().id);
        if (switcher_anchor.has_value()) {
            switcher_progress = show::derive_switcher_progress(
                *switcher_anchor, control_frame);
            scene_frame = scene_model.evaluate(
                switcher_anchor->program_scene, time, show_loop);
            stage_scene = rehearsal_mode ? switcher_anchor->rehearsal_scene
                                         : switcher_anchor->program_scene;
            stage_scene_frame =
                scene_model.evaluate(stage_scene, stage_time);
            program_compositor_graph = compositor_model.evaluate(
                switcher_anchor->program_scene, legacy_graph_owner);
            compositor_graph = rehearsal_mode
                                   ? compositor_model.evaluate(
                                         stage_scene, legacy_graph_owner)
                                   : program_compositor_graph;
            if (switcher_progress.active) {
                transition_from_frame = scene_model.evaluate(
                    switcher_progress.from, time, show_loop);
                transition_to_frame = scene_model.evaluate(
                    switcher_progress.to, time, show_loop);
                transition_from_graph = compositor_model.evaluate(
                    switcher_progress.from, legacy_graph_owner);
                transition_to_graph = compositor_model.evaluate(
                    switcher_progress.to, legacy_graph_owner);
                if (!show::dissolve_canvases_compatible(
                        transition_from_frame.canvas,
                        transition_to_frame.canvas)) {
                    switcher_progress.active = false;
                }
            }
        } else {
            scene_frame = scene_model.evaluate(time, show_loop);
            stage_scene_frame = scene_frame;
            stage_scene = scene_frame.id;
            compositor_graph = compositor_model.evaluate(
                scene_frame.id, legacy_graph_owner);
            program_compositor_graph = compositor_graph;
        }

        if (switcher_anchor) {
            const auto publication_scene=scene::publication::render_scene(document,switcher_anchor->program_scene);
            if (publication_scene != switcher_anchor->program_scene) {
                scene_frame=scene_model.evaluate(publication_scene,time,show_loop);
                program_compositor_graph=compositor_model.evaluate(publication_scene,legacy_graph_owner);
            }
            if (publication_scene != switcher_anchor->program_scene || !scene_frame.publication_revision.empty()) {
                const auto keep=publication_scene != switcher_anchor->program_scene ? switcher_anchor->program_scene
                    : doc::NodeId{scene::publication::integer(document.get(publication_scene),scene::publication::rollback)};
                if (keep.value()) scene_frame.publication_retained_models=scene_model.evaluate(keep,time,show_loop).models;
            }
        }
#if defined(MEDIASERVER_HAS_GUI)
        scene_frame.publication_verified=director_publication_verification.observe(document,scene_frame.id);
#endif
        // The same scene at the frame the loop is leaving, as every node
        // evaluates it. A switcher dissolve owns the mix while one runs.
        if (!switcher_progress.active) {
            loop_blend = show::derive_transport_blend(transport.anchor(),
                                                      control_frame);
        }
        if (loop_blend.has_value()) {
            transition_from_frame = scene_model.evaluate(
                scene_frame.id,
                scene::SceneTime{loop_blend->from.value(),
                                 input.rate.numerator,
                                 input.rate.denominator},
                show_loop);
            transition_from_frame.publication_verified =
                scene_frame.publication_verified;
            transition_to_frame = scene_frame;
            transition_from_graph = program_compositor_graph;
            transition_to_graph = program_compositor_graph;
        }

        for (auto* candidate : {&scene_frame, &stage_scene_frame, &transition_from_frame, &transition_to_frame}) {
            candidate->asset_commit = scene_frame.asset_commit;
            candidate->asset_manifest_digest = asset_manifest_digest;
            const auto activation = candidate->asset_commit.control_frame;
            candidate->asset_commit_released = activation == 0 || (activation >= core::asset_commit_lead_frames &&
                barrier.released_through().through_frame.value() >= activation - core::asset_commit_lead_frames);
            candidate->control_frame = control_frame.value();
            candidate->asset_cohort_required = true;
        }

        // A Timeline edit being dragged: the Stage scene once more, at the
        // edit point and with the edit applied, for the Stage views to draw.
        // Program, the nodes and every panel keep the frames above.
        tick.stage_edit_frame.reset();
#if defined(MEDIASERVER_HAS_GUI)
        if (options.edit && stage_edit_request.has_value()) {
            tick.stage_edit_frame = scene_model.evaluate(
                stage_scene,
                scene::SceneTime{stage_edit_request->frame,
                                 input.rate.numerator,
                                 input.rate.denominator},
                {}, stage_edit_request->timings);
            scene::Scene &edited = *tick.stage_edit_frame;
            edited.asset_commit = stage_scene_frame.asset_commit;
            edited.asset_manifest_digest =
                stage_scene_frame.asset_manifest_digest;
            edited.asset_commit_released =
                stage_scene_frame.asset_commit_released;
            edited.control_frame = stage_scene_frame.control_frame;
            edited.asset_cohort_required =
                stage_scene_frame.asset_cohort_required;
        }
#endif

        // Camera tracking, at the frame boundary and not in the UI loop.
        //
        // This is the Director's one sample per frame per camera: each
        // authored feed's socket is reconfigured (idempotent while it is
        // unchanged), its pose slot is read without ever waiting on the
        // receive thread, and the result is stamped with the exact control
        // frame it applies to plus the authored latency compensation.
        //
        // Sampling here rather than beside the editor matters twice over.
        // A headless Director tracks, so the stage does not depend on an
        // operator window being open. And the frame a pose is stamped for
        // is the frame it was sampled on, which is what makes the authored
        // compensation a fixed offset rather than an approximate one.
        //
        // Sampled and replicated *before* the poses are injected below,
        // and it has to be that way round: injecting first would draw and
        // digest last frame's stack while the nodes drew this frame's.
        const auto tracking_sources = scene_model.tracking_sources();
        tracker_listeners.configure(tracking_sources);
        director_pose_predictors.set_cadence(input.rate.numerator, input.rate.denominator);
        director_pose_predictors.retain(scene_frame.tracked_cameras);
        std::array<std::optional<track::PoseSample>, scene::max_tracking_sources> source_samples{};
        for (std::size_t index = 0; index < tracking_sources.size() && index < source_samples.size(); ++index)
            source_samples[index] = tracker_listeners.sample(tracking_sources[index].node);
        {
            const core::u32 latency_frames =
                scene::clamped(scene_frame.renderer_settings.icvfx)
                    .latency_frames;
            net::TrackedCameraFrame replicated{};
            // The frame these poses are *for*, which is the sampled frame
            // plus the authored compensation. Sending the raw frame here
            // instead - which this did - meant every node applied the pose
            // on arrival and the compensation described nothing: the wire
            // field's own comment says "after latency compensation", and
            // only the Director's local channel ever saw the compensated
            // value. A node cannot recompute it, because the compensation
            // is show state it is not required to have evaluated yet.
            const core::u64 compensated =
                input.control_frame.value()
                <= std::numeric_limits<core::u64>::max() - latency_frames
                ? input.control_frame.value() + latency_frames
                : input.control_frame.value();
            // Never backwards. Lowering the compensation mid-show would
            // otherwise stamp the next pose for an earlier frame than the
            // last one, which every node's channel refuses as stale - and
            // a node that refuses every pose holds the one it has, so the
            // plate freezes exactly when an operator is adjusting the
            // thing meant to fix it. Clamping costs a few frames of extra
            // latency while the raw frame catches up, and then it is
            // exact again.
            //
            // Keep the last stamp during the clamp. Several editor loops
            // can share a control frame; none may invent a new stamp.
            // Pausing the show still advances control time and tracking.
            replicated.control_frame =
                std::max(compensated, last_replicated_pose_frame);
            for (const scene::TrackedCameraView &view :
                 scene_frame.tracked_cameras) {
                auto* predictor = director_pose_predictors.find(view.camera);
                const auto source = std::find_if(tracking_sources.begin(), tracking_sources.end(),
                    [&](const auto& candidate) { return candidate.node == view.source; });
                if (!predictor || source == tracking_sources.end()) continue;
                predictor->configure(view.source, source->settings,
                                     scene_frame.renderer_settings.icvfx);
                const auto& sampled = source_samples[static_cast<std::size_t>(
                    source - tracking_sources.begin())];
                const auto stamped = predictor->sample(
                    sampled, view.camera, input.control_frame.value(),
                    replicated.control_frame,
                    scene::tracked_field_of_view(source->settings,
                        sampled ? sampled->pose.focal_length_mm : 0.0F),
                    source->settings.sensor_aspect_ratio);
                if (!stamped.has_value()) {
                    continue;
                }
                auto &entry = director_pose_channels.open(view.camera);
                entry.lens = server::tracked_camera_lens(stamped->sample);
                // Prediction already used this final wire stamp as its
                // horizon. The Director and nodes accept the same answer.
                if (entry.channel.accept(server::stamped_pose(
                        stamped->sample, replicated.control_frame))
                    == track::PoseChannelOutcome::Accepted) {
                    ++entry.accepted;
                    ++director_poses_sent;
                    replicated.samples.push_back(stamped->sample);
                }
            }
            if (!replicated.samples.empty()) {
                last_replicated_pose_frame = replicated.control_frame;
                // Ascending camera order, which is what the wire contract
                // asks for: a node then applies the stack without sorting
                // it, and a duplicated camera is refused rather than
                // silently overriding its own earlier pose.
                std::sort(replicated.samples.begin(),
                          replicated.samples.end(),
                          [](const net::TrackedCameraSample &left,
                             const net::TrackedCameraSample &right) {
                              return left.camera_node < right.camera_node;
                          });
                // Fanned out to every live session here, so every node is
                // given identical bits. This is the one place poses leave
                // the Director, and they leave once.
                static_cast<void>(
                    barrier.publish_tracked_pose(replicated));
            }
        }

        // What the cohort is drawing this frame.
        //
        // Injected before the digest is taken, and it has to be: the
        // digest is the cross-node agreement check, and a Director that
        // digested a scene without the poses it just replicated would
        // disagree with every node that received them.
        // Resolve the accepted stream at this control frame. This retires
        // due samples and counts holds once, even when the editor redraws.
        inject_tracked_poses(scene_frame, director_pose_channels,
                             input.control_frame.value(), true);
        // Stage previews the same physical cameras Program is showing.
        stage_scene_frame.tracked_cameras = scene_frame.tracked_cameras;
        if (blender_links.any_connected())
            blender_links.sample(render::linked_stage_sample(stage_scene_frame,input.control_frame.value()));
        authored_scene_digest = scene::digest(scene_frame);

        std::vector<ProxyPlacementObservation> observations;
        bool proxies_ready = resolve_scene_proxies(
            director_proxy_playback, scene_frame,
            director_playback_asset_registry, director_asset_availability,
            observations);
        proxies_ready = resolve_scene_proxies(
                            director_proxy_playback, stage_scene_frame,
                            director_playback_asset_registry,
                            director_asset_availability, observations) &&
                        proxies_ready;
        if (switcher_progress.active || loop_blend.has_value()) {
            proxies_ready =
                resolve_scene_proxies(
                    director_proxy_playback, transition_from_frame,
                    director_playback_asset_registry,
                    director_asset_availability, observations) &&
                proxies_ready;
            proxies_ready =
                resolve_scene_proxies(
                    director_proxy_playback, transition_to_frame,
                    director_playback_asset_registry,
                    director_asset_availability, observations) &&
                proxies_ready;
        }
        report_proxy_status_changes("Director", observations,
                                    director_proxy_observations);
        director_proxy_observations = std::move(observations);
        static_cast<void>(proxies_ready);
    }
    return std::nullopt;
}

void DirectorShowLoop::evaluate_feed_and_sync(DirectorTick &tick) {
    const auto &control_frame = tick.control_frame;
    auto &input = tick.input;
    auto &scene_frame = tick.scene_frame;
    auto &feed_map = tick.feed_map;
    auto &hardware_map = tick.hardware_map;
    auto &asset_manifest = tick.asset_manifest;
    assets::AssetRegistry &asset_registry = director_asset_registry;
    if (options.edit) {
        const auto &document = scheduler.sealed_document();
        if (!feed_loaded || document.version().value() != feed_version) {
            feed_model.reset(document);
            feed_version = document.version().value();
            feed_loaded = true;
        }
        feed_map = feed_model.evaluate(
            scene::SceneTime{input.show_frame.value(), input.rate.numerator,
                             input.rate.denominator});
        if (!hardware_loaded ||
            document.version().value() != hardware_version) {
            hardware_model.reset(document);
            hardware_version = document.version().value();
            hardware_loaded = true;
        }
        hardware_map = hardware_model.evaluate();
        asset_manifest =
            collect_project_asset_manifest(document, asset_registry);
    }

    // The synchronization policy and the groups derived from the authored
    // cabling are pure document projections, so they are refreshed even on
    // a Director with no editor: the diagnostic stream should record what
    // the project asked for whether or not anyone is watching.
    {
        const doc::Root &sync_document = scheduler.sealed_document();
        if (!scene_loaded ||
            sync_document.version().value() != scene_version) {
            scene_model.reset(sync_document);
            scene_version = sync_document.version().value();
            scene_loaded = true;
        }
        // Each playhead acts on the cues of the scene it is playing:
        // Program on Program's, Preview on the one being rehearsed. The
        // sheets are cut when the document changes or Preview opens, and
        // count only arrivals from now on, so an edit made mid-show never
        // reaches back for a cue the show already passed.
        if (cue_version != scene_version ||
            cued_preview != preview_transport.has_value() ||
            cued_rate != transport.anchor().rate) {
            cue_version = scene_version;
            cued_preview = preview_transport.has_value();
            cued_rate = transport.anchor().rate;
            const auto switched = show::read_switcher(sync_document);
            const auto authored_scenes = scene_model.scenes();
            const scene::SceneId program_scene =
                switched.has_value()
                    ? switched->program_scene
                    : (authored_scenes.empty()
                           ? scene::legacy_scene_id
                           : authored_scenes.front().id);
            try {
                const auto program_cues = scene_cue_sheet(
                    scene_model, program_scene, cued_rate);
                static_cast<void>(
                    transport.set_cues(program_cues, control_frame));
                if (preview_transport.has_value()) {
                    const auto preview_cues = scene_cue_sheet(
                        scene_model,
                        switched.has_value() ? switched->rehearsal_scene
                                             : program_scene,
                        cued_rate);
                    static_cast<void>(preview_transport->set_cues(
                        preview_cues, control_frame));
                }
            } catch (const std::bad_alloc &) {
                // The transport keeps the sheet it had; the next document
                // change tries again.
                cue_version.reset();
            }
        }
        if (!director_sync_loaded ||
            sync_document.version().value() != director_sync_version) {
            director_sync_model.reset(sync_document);
            director_sync_version = sync_document.version().value();
            director_sync_loaded = true;
            director_sync.policy = director_sync_model.evaluate();
        }
        director_sync.topology =
            sync::derive_sync_topology(hardware_map, director_sync.policy);
        const scene::RendererSettings renderer =
            scene_frame.id != scene::SceneId{} ? scene_frame.renderer_settings : scene_model.renderer_settings();
        director_renderer_lockstep =
            evaluate_renderer_lockstep(renderer, hardware_map);
        director_sync.readiness =
            evaluate_sync_gate(
                true, director_renderer_lockstep.satisfied());

    }
}

void DirectorShowLoop::observe_asset_cohort(DirectorTick &tick) {
    const auto &control_frame = tick.control_frame;
    auto &hardware_map = tick.hardware_map;
    director_asset_cohort = {};
    asset_waiting_node.reset();
    if (options.edit || options.scene || options.output)
        director_asset_cohort.observe(&observed_local_renderer.assets);
    for (const topology::HardwareDevice *const member :
         topology::cluster_render_servers(hardware_map)) {
        net::RuntimeRendererReadiness reported{};
        const bool known = barrier.runtime_renderer_readiness(member->node_index, reported);
        if (!asset_waiting_node && (!known || !reported.assets.ready ||
            reported.assets.digest != observed_local_renderer.assets.digest))
            asset_waiting_node = member->node_index;
        director_asset_cohort.observe(known ? &reported.assets : nullptr);
    }
    if (director_renderer_lockstep.satisfied() &&
        (!director_sync.policy.requires_physical_sync() || director_sync.readiness.satisfied) && !pending_asset_commit &&
        director_asset_cohort.needs_commit(
            {last_asset_commit_digest, last_asset_commit_frame, 0},
            control_frame.value())) {
        const auto program = show::read_switcher(scheduler.sealed_document());
        const auto scenes = scene_model.scenes();
        const auto stage = program ? scene::publication::render_scene(scheduler.sealed_document(),program->program_scene) : scenes.empty() ? scene::SceneId{} : scenes.front().id;
        if (stage != scene::SceneId{}) pending_asset_commit = std::pair{doc::NodeId{stage.value()}, director_asset_cohort.digest};
    }
    if (logged_asset_cohort != director_asset_cohort) {
        std::cout << "asset-publication control-frame=" << control_frame.value()
                  << " ready=" << director_asset_cohort.ready_members << '/' << director_asset_cohort.members
                  << " reports=" << director_asset_cohort.reported_members
                  << " digest=" << director_asset_cohort.digest
                  << " activation=" << last_asset_commit_frame
                  << " visible=" << director_asset_cohort.visible_commit
                  << " all-visible=" << director_asset_cohort.visible << '\n';
        logged_asset_cohort = director_asset_cohort;
    }
}

PhaseExit DirectorShowLoop::present_output(DirectorTick &tick) {
    auto &input = tick.input;
    const auto &render_scene = tick.render_scene;
    auto &scene_frame = tick.scene_frame;
    auto &transition_from_frame = tick.transition_from_frame;
    auto &transition_to_frame = tick.transition_to_frame;
    auto &switcher_progress = tick.switcher_progress;
    auto &loop_blend = tick.loop_blend;
    auto &feed_map = tick.feed_map;
    auto &program_compositor_graph = tick.program_compositor_graph;
    auto &transition_from_graph = tick.transition_from_graph;
    auto &transition_to_graph = tick.transition_to_graph;
    auto &status = tick.status;
    auto &handled = tick.handled;
    if (!handled) {
        if (render_scene &&
            (switcher_progress.active || loop_blend.has_value())) {
            status = presenter->present(
                transition_from_frame, transition_to_frame,
                static_cast<float>(loop_blend.has_value()
                                       ? loop_blend->mix()
                                       : switcher_progress.mix()),
                transition_from_graph, transition_to_graph,
                scene::whole(scene_frame.canvas));
        } else {
            status = render_scene
                         ? presenter->present(
                               scene_frame, program_compositor_graph,
                               scene::whole(scene_frame.canvas))
                         : presenter->present(input);
        }
    }

    // Evaluated for its digest only, so the Director's status line can show
    // it agrees with the nodes' feed maps.
    if (options.output) {
        const auto &document = scheduler.sealed_document();
        if (!feed_loaded || document.version().value() != feed_version) {
            feed_model.reset(document);
            feed_version = document.version().value();
            feed_loaded = true;
        }
        feed_map = feed_model.evaluate(
            scene::SceneTime{input.show_frame.value(), input.rate.numerator,
                             input.rate.denominator});
    }

    switch (status) {
    case show::PresenterStatus::Closed:
        closed = true;
        break;
    case show::PresenterStatus::Failed:
        ++present_failures;
        break;
    case show::PresenterStatus::Deferred:
    case show::PresenterStatus::Presented:
    case show::PresenterStatus::Skipped:
        break;
    }

    if (!options.edit && presenter != nullptr &&
        presenter->renders_scenes()) {
        observed_local_renderer = runtime_renderer_readiness(
            presenter->renderer_readiness());
    }

    if (presenter != nullptr) {
        for (auto command = presenter->take_command();
             command != show::PresenterCommand::None;
             command = presenter->take_command()) {
            try {
                operator_commands.push_back(command);
            } catch (const std::bad_alloc &) {
                std::cerr
                    << "could not queue an operator transport command\n";
                return 1;
            }
        }
    }
    return std::nullopt;
}

} // namespace mediaserver::app
