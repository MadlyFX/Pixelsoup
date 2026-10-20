// The Director frame loop driver: pacing, sealing, operator input dispatch and
// the evaluations every phase shares.

#include "director_show_loop.hpp"

#include "asset_activation.hpp"
#include "cluster_observability.hpp"
#include "edit_coalescing_policy.hpp"
#include "playback_runtime.hpp"
#include "renderer_lockstep.hpp"

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/core/asset_publication.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/net/session_protocol.hpp>
#include <mediaserver/scene/lens.hpp>
#include <mediaserver/scene/linked_publication.hpp>
#include <mediaserver/scene/linked_scene.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/show/frame_boundary_driver.hpp>
#include <mediaserver/show/presenter.hpp>
#include <mediaserver/show/show_clock.hpp>
#include <mediaserver/show/switcher_author.hpp>
#include <mediaserver/show/switcher_state.hpp>
#include <mediaserver/show/transport_author.hpp>
#include <mediaserver/show/transport_state.hpp>
#include <mediaserver/sync/sync_policy.hpp>
#include <mediaserver/sync/sync_readiness.hpp>
#include <mediaserver/sync/sync_status.hpp>
#include <mediaserver/topology/hardware_map.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace mediaserver::app {

int DirectorShowLoop::run() {
    while (stop_requested == 0 && !closed) {
        DirectorTick tick{};
        switch (pace(tick)) {
        case PaceOutcome::Ready:
            break;
        case PaceOutcome::Retry:
            continue;
        case PaceOutcome::Failed:
            return 1;
        }
        if (const PhaseExit exit = seal_due_frames(tick)) {
            return *exit;
        }
        // Retire in order. A frame whose cohort has not finished stays, and so
        // does everything behind it, which is what bounds the backlog above.
        while (!tracked.empty() && barrier.retire(tracked.front())) {
            tracked.pop_front();
        }

        begin_frame(tick);
        if (const PhaseExit exit = evaluate_scene(tick)) {
            return *exit;
        }
        evaluate_feed_and_sync(tick);
        observe_asset_cohort(tick);
#if defined(MEDIASERVER_HAS_GUI)
        if (const PhaseExit exit = refresh_editor(tick)) {
            return *exit;
        }
#endif
        if (const PhaseExit exit = present_output(tick)) {
            return *exit;
        }
        print_status(tick);

        if (options.run_frames != 0U && sealed >= options.run_frames) {
            break;
        }
        auto next_wake = pacer.next_boundary_at();
#if defined(MEDIASERVER_HAS_GUI)
        if (editor) {
            next_wake = std::min(next_wake, next_editor_refresh);
        }
#endif
        sleep_until_boundary(next_wake);
    }
    report_stopping();
    return 0;
}

PaceOutcome DirectorShowLoop::pace(DirectorTick &tick) {
    auto &now = tick.now;
    auto &due = tick.due;
    now = Clock::now();
#if defined(MEDIASERVER_HAS_GUI)
    if (editor_coalescing_policy.expire(now)) {
        scheduler.end_coalescing();
        pending_editor_coalescing_close = true;
    }
#endif

    // The pacing source, derived from the same replicated policy a node
    // uses to pick its own. It lags a policy change by one iteration,
    // because the policy is evaluated further down this loop from the
    // document this iteration seals - which is a frame of latency on an
    // operator action, not a source of drift.
    director_clock.requires_reference =
        director_sync.policy.requires_external_reference();
    const show::VideoReferenceSample director_reference =
        director_clock.requires_reference ? read_director_video_reference()
                                          : show::VideoReferenceSample{};
    director_clock.reference =
        show_clock_reference_state(director_reference.state);

    core::u64 reference_boundaries = 0U;
    const bool paced_by_reference =
        director_clock.requires_reference &&
        director_reference.state == show::VideoReferenceState::Locked;
    if (paced_by_reference) {
        if (!director_reference_seen) {
            director_reference_seen = true;
        } else {
            // Unsigned subtraction intentionally implements the source's
            // documented modulo-2^32 counter wrap, exactly as the node
            // boundary driver does.
            reference_boundaries = static_cast<core::u32>(
                director_reference.frame_counter
                - director_last_reference_frame);
        }
        director_last_reference_frame = director_reference.frame_counter;
    } else {
        director_reference_seen = false;
    }

    // Referenced: advance by exactly the frames house reference counted.
    // Unreferenced: fall back to the system clock rather than holding.
    //
    // A node without a reference holds, and that is right for a node - the
    // previous frame stays on the wall. A Director that held would seal no
    // control frames at all, and the operator command that recovers from a
    // reference fault is itself a control frame. Holding would make the
    // fault unrecoverable without a restart. So the pacer keeps running and
    // the drift is reported instead of hidden - and the readiness gate
    // refuses Program on it, so falling back is never mistaken for being
    // locked.
    auto paced_tick = paced_by_reference
        ? pacer.advance_by(reference_boundaries, now)
        : pacer.advance_to(now);
    if (!paced_tick) {
        if (paced_tick.error() == show::ShowClockError::TimeOverflow) {
            static_cast<void>(pacer.seek(pacer.frame(), now));
            return PaceOutcome::Retry;
        }
        std::cerr << "Director frame pacer refused a time observation\n";
        return PaceOutcome::Failed;
    }

    due = std::min(
        pacer.frame().value() > scheduler.sealed_through().value()
            ? pacer.frame().value() - scheduler.sealed_through().value()
            : 0,
        max_director_catch_up);
    // End a catch-up batch on the activation, so the editor/output is
    // offered that exact snapshot. Remaining pacing debt stays queued.
    const auto asset_frame = core::asset_presentation_frame(
        observed_local_renderer.assets,
        {last_asset_commit_digest, last_asset_commit_frame, 0},
        scheduler.sealed_through().value());
    if (asset_frame > scheduler.sealed_through().value())
        due =
            std::min(due, asset_frame - scheduler.sealed_through().value());
    // A native file dialog can leave a large pacing debt. Keep catch-up
    // within the cohort queue's remaining capacity, then service node
    // acknowledgements below before sealing another batch. A full queue
    // is backpressure; it must not terminate the editor after Save As.
    due = std::min(due, static_cast<core::u64>(
        max_tracked_frames - std::min(tracked.size(), max_tracked_frames)));
    if (paced_by_reference) {
        director_clock.referenced_frames += due;
    } else if (director_clock.requires_reference) {
        director_clock.unreferenced_frames += due;
    }
    return PaceOutcome::Ready;
}

PhaseExit DirectorShowLoop::seal_due_frames(DirectorTick &tick) {
    const auto &now = tick.now;
    auto &due = tick.due;
    auto &sealed_this_iteration = tick.sealed_this_iteration;
    auto &repace_after_seal = tick.repace_after_seal;
    for (core::u64 index = 0U; index < due; ++index) {
        if (tracked.size() >= max_tracked_frames) {
            std::cerr << "Director cohort backlog reached "
                      << tracked.size()
                      << " unretired frames; refusing to seal further\n";
            return 1;
        }

        const auto through = scheduler.sealed_through().value();
        if (through == std::numeric_limits<core::u64>::max()) {
            std::cerr << "Director control frame numbering is exhausted\n";
            return 1;
        }
        const cmd::FrameNumber frame{through + 1U};

        // Operator input is applied to the first frame of this iteration,
        // because an anchor has to name the exact control frame it takes
        // effect on.
        if (index == 0U) {
            if (const PhaseExit exit =
                    apply_operator_input(tick, frame, through)) {
                return exit;
            }
        }
        // Cues act on the frame after the one that presented them, so
        // both playheads settle on every frame, not once per iteration.
        if (auto settled = transport.settle(frame);
            settled.has_value() && transport.cue_status(frame).held != 0U) {
            std::cout << "cue hold at show-frame="
                      << settled->anchor_show_frame.value()
                      << " control-frame=" << frame.value() << '\n';
        }
        if (preview_transport.has_value()) {
            static_cast<void>(preview_transport->settle(frame));
        }

        // The anchor travels as an ordinary command inside a sealed frame,
        // so every node derives the position from the same revision. An
        // unchanged anchor authors nothing, so a playing show costs the
        // document no edits at all -- which is what removes the per-frame
        // durable write behind it.
        if (!authored || *authored != transport.anchor()) {
            const auto command = show::schedule_transport_anchor(
                scheduler, transport.anchor(), frame,
                cmd::UndoContextId{1U}, cmd::TransactionId{frame.value()});
            if (!command) {
                std::cerr
                    << "could not author the transport anchor for frame "
                    << frame.value() << " (code "
                    << static_cast<unsigned>(command.error()) << ")\n";
                return 1;
            }
            authored = transport.anchor();
            ++anchors;
            // Re-anchoring inside an outstanding permit's lead changes the
            // show frame its activation presents. Restate it in this same
            // control frame, so no node compares against the stale one.
            const doc::Node* const permitted =
                last_asset_commit_stage == doc::NodeId{}
                    ? nullptr
                    : scheduler.projected_document().get(
                          last_asset_commit_stage);
            if (permitted != nullptr &&
                permitted->type == scene::schema::stage_type) {
                const core::AssetCommit permit{
                    scene::publication::integer(
                        permitted, scene::schema::asset_commit_digest),
                    scene::publication::integer(
                        permitted, scene::schema::asset_commit_control),
                    scene::publication::integer(
                        permitted, scene::schema::asset_commit_show)};
                if (const auto restated = restated_permit_show_frame(
                        permit, transport.anchor(), frame.value())) {
                    doc::Node updated = *permitted;
                    updated.properties.insert_or_assign(
                        std::string{scene::schema::asset_commit_show},
                        doc::PropertyValue{
                            static_cast<core::i64>(*restated)});
                    if (!scheduler.schedule_edit(
                            cmd::PutNode{std::move(updated)},
                            cmd::PutNode{*permitted},
                            cmd::UndoContextId{0x4153534554ULL},
                            cmd::TransactionId{frame.value()}, frame)) {
                        std::cerr
                            << "could not restate the asset cohort permit\n";
                        return 1;
                    }
                }
            }
        }

        if (pending_asset_commit && (index + 1 == due || repace_after_seal)) {
            const auto [stage_id, digest] = *pending_asset_commit;
            const doc::Node* existing = scheduler.projected_document().get(stage_id);
            if (existing && existing->type == scene::schema::stage_type) {
                doc::Node committed = *existing;
                const auto anchor = show::read_transport(scheduler.projected_document());
                const cmd::FrameNumber activation{frame.value() + core::asset_commit_lead_frames};
                const core::u64 show_frame = anchor ? show::derive_show_frame(*anchor, activation).value() : 0;
                committed.properties.insert_or_assign(std::string{scene::schema::asset_commit_digest}, doc::PropertyValue{std::bit_cast<core::i64>(digest)});
                committed.properties.insert_or_assign(std::string{scene::schema::asset_commit_control}, doc::PropertyValue{static_cast<core::i64>(activation.value())});
                committed.properties.insert_or_assign(std::string{scene::schema::asset_commit_show}, doc::PropertyValue{static_cast<core::i64>(show_frame)});
                if (!scheduler.schedule_edit(cmd::PutNode{std::move(committed)}, cmd::PutNode{*existing},
                    cmd::UndoContextId{0x4153534554ULL}, cmd::TransactionId{frame.value()}, frame)) {
                    std::cerr << "could not schedule asset cohort commit\n";
                    return 1;
                }
                last_asset_commit_frame = activation.value();
                last_asset_commit_digest = digest;
                last_asset_commit_stage = stage_id;
                const auto switcher=show::read_switcher(scheduler.projected_document());
                if (switcher && scene::publication::render_scene(scheduler.projected_document(),switcher->program_scene) == stage_id
                    && stage_id != switcher->program_scene) {
                    const auto* active=scheduler.projected_document().get(stage_id);
#if defined(MEDIASERVER_HAS_GUI)
                    publication_activation=activation.value();
                    publication_message="Publication scheduled at control frame "+std::to_string(activation.value())+", show frame "+std::to_string(show_frame)+".";
#endif
                    std::cout << "linked-publication permit revision=" << scene::publication::text(active,scene::publication::revision)
                              << " control=" << activation.value() << " show=" << show_frame << " digest=" << digest << '\n';
                }
            }
            pending_asset_commit.reset();
        }

        if (pending_asset_rescan && index == 0) {
            scene::SceneModel assets_scene;
            assets_scene.reset(scheduler.projected_document());
            const auto scenes = assets_scene.scenes();
            if (!scenes.empty()) {
                const auto stage_id = doc::NodeId{scenes.front().id.value()};
                if (const auto* existing = scheduler.projected_document().get(stage_id)) {
                    doc::Node refreshed = *existing;
                    refreshed.properties.insert_or_assign(std::string{scene::schema::asset_refresh_generation}, doc::PropertyValue{static_cast<core::i64>(frame.value())});
                    if (!scheduler.schedule_edit(cmd::PutNode{std::move(refreshed)},cmd::PutNode{*existing},
                        cmd::UndoContextId{0x4153534554ULL},cmd::TransactionId{frame.value()},frame)) return 1;
                }
            }
        }

        if (auto activated=show::activate_linked_publication(scheduler,frame); !activated) {
            std::cerr << "could not activate linked publication at its permit frame\n"; return 1;
        } else if (activated.value()) {
            std::cout << "linked-publication scene-switch control=" << frame.value() << '\n';
        }
        auto sealed_frame = barrier.seal_and_track(hub, frame);
        if (!sealed_frame) {
            std::cerr << "Director could not seal frame " << frame.value()
                      << " (code "
                      << static_cast<unsigned>(sealed_frame.error().code)
                      << ")\n";
            return 1;
        }
        tracked.push_back(sealed_frame.value().barrier);
        ++sealed;
        ++sealed_this_iteration;
        if (repace_after_seal.has_value()) {
            // `due` was calculated at the old cadence. Once the boundary
            // carrying the new rate is sealed, discard the remaining old-
            // rate debt and make the next boundary one complete period at
            // the new exact rational rate after this same observation.
            if (!pacer.seek(show::ShowFrame{frame.value()}, now) ||
                !pacer.reconfigure_rate(*repace_after_seal, now)) {
                std::cerr << "could not re-anchor the Director show rate\n";
                return 1;
            }
            interval = status_interval(options, *repace_after_seal);
            since_status = 0U;
            break;
        }
    }
    return std::nullopt;
}

PhaseExit DirectorShowLoop::apply_operator_input(DirectorTick &tick,
                                                 cmd::FrameNumber frame,
                                                 core::u64 through) {
    OperatorFrame op{frame, through};
    auto &project_loaded_this_frame = op.project_loaded_this_frame;
    if (autoplay_pending) {
        autoplay_pending = false;
        if (!transport.play(frame)) {
            std::cerr << "could not start the Director transport\n";
            return 1;
        }
    }
#if defined(MEDIASERVER_HAS_GUI)
    if (const PhaseExit exit = apply_project_load(tick, op)) {
        return exit;
    }
#endif
    if (!project_loaded_this_frame) {
        for (const auto command : operator_commands) {
            apply_command(transport, command, frame);
        }
    }
    operator_commands.clear();
#if defined(MEDIASERVER_HAS_GUI)
    apply_preview_transport(op);
    prepare_authoring_context(op);
    if (const PhaseExit exit = apply_publication_requests(op)) {
        return exit;
    }
    if (const PhaseExit exit = apply_blender_links(op)) {
        return exit;
    }
    if (const PhaseExit exit = apply_scene_edits(op)) {
        return exit;
    }
    if (const PhaseExit exit = apply_asset_imports(op)) {
        return exit;
    }
    if (const PhaseExit exit = apply_key_requests(op)) {
        return exit;
    }
    if (const PhaseExit exit = apply_clip_requests(op)) {
        return exit;
    }
    if (const PhaseExit exit = apply_element_and_lane_requests(op)) {
        return exit;
    }
    if (const PhaseExit exit = apply_group_selection_and_mask_requests(op)) {
        return exit;
    }
    if (const PhaseExit exit = apply_timeline_edits(op)) {
        return exit;
    }
    if (const PhaseExit exit = apply_property_and_effect_requests(op)) {
        return exit;
    }
    if (const PhaseExit exit = apply_hardware_requests(op)) {
        return exit;
    }
    if (const PhaseExit exit = apply_feed_map_requests(op)) {
        return exit;
    }
    if (const PhaseExit exit = apply_compositor_and_delete_requests(op)) {
        return exit;
    }
    if (const PhaseExit exit = apply_switcher_request(op)) {
        return exit;
    }
    if (const PhaseExit exit = apply_show_timing_requests(tick, op)) {
        return exit;
    }
    if (const PhaseExit exit = apply_history_request(op)) {
        return exit;
    }
    if (const PhaseExit exit = apply_project_save(op)) {
        return exit;
    }
    pending_editor_coalescing.clear();
    if (pending_editor_coalescing_close) {
        scheduler.end_coalescing();
        pending_editor_coalescing_close = false;
    }
#else
    static_cast<void>(tick);
#endif
    return std::nullopt;
}

// The Director's own reference, from its own presenter.
//
// A Director with no output window has none, and reports Unsupported
// rather than pretending: an editor workstation is not a genlocked output,
// and neither is a headless Director on a machine with no card in the
// house chain. That is a real deployment and the honest answer is to say
// the cadence is unreferenced, not to invent one.
show::VideoReferenceSample DirectorShowLoop::read_director_video_reference() noexcept {
    if (presenter == nullptr) {
        return show::VideoReferenceSample{
            show::VideoReferenceState::Unsupported, 0U};
    }
    const show::PresenterVideoReference observed =
        presenter->video_reference();
    show::VideoReferenceSample mapped{};
    mapped.frame_counter = observed.frame_counter;
    switch (observed.state) {
    case show::PresenterVideoReferenceState::Unsupported:
        mapped.state = show::VideoReferenceState::Unsupported;
        break;
    case show::PresenterVideoReferenceState::Waiting:
        mapped.state = show::VideoReferenceState::Waiting;
        break;
    case show::PresenterVideoReferenceState::Locked:
        mapped.state = show::VideoReferenceState::Locked;
        break;
    case show::PresenterVideoReferenceState::Lost:
        mapped.state = show::VideoReferenceState::Lost;
        break;
    case show::PresenterVideoReferenceState::Failed:
        mapped.state = show::VideoReferenceState::Failed;
        break;
    case show::PresenterVideoReferenceState::Unknown:
        mapped.state = show::VideoReferenceState::Unknown;
        break;
    }
    return mapped;
}

server::RendererLockstep DirectorShowLoop::evaluate_renderer_lockstep(
    const scene::RendererSettings &source,
    const topology::HardwareMap &hardware) noexcept {
    const scene::RendererSettings settings = scene::clamped(source);
    server::RendererLockstep lockstep =
        server::begin_renderer_lockstep(
            settings,
            options.scene || options.output || options.edit);

    lockstep.hold_over_budget = director_sync.policy.late_frame == sync::LateFramePolicy::HoldGroup;
    lockstep.conformance_request = conformance_request;
#if defined(MEDIASERVER_HAS_GUI)
    if (observed_local_renderer.selected == net::RuntimeRendererBackend::Advanced) {
        const auto adapter = observed_local_renderer.conformance.adapter;
        director_conformance.poll(adapter, conformance_request, observed_local_renderer.conformance);
    }
#endif
    // The Director's editor or scene/output preview is itself a
    // Program renderer. Its most recent actual record establishes the
    // backend every remote output must match. A busy GPU may leave
    // this one frame stale; policy edits then hold rather than probing
    // or waiting on the frame path.
    if (options.edit || options.scene || options.output) {
        server::observe_renderer_lockstep(
            lockstep,
            &observed_local_renderer,
            0U,
            true);
    }

    // Only cabled cards are cohort members: a parked alternative on
    // the map never reports, and must not hold every output.
    for (const topology::HardwareDevice *const device :
         topology::cluster_render_servers(hardware)) {
        net::RuntimeRendererReadiness remote{};
        const bool reported = barrier.runtime_renderer_readiness(
            device->node_index, remote);
        server::observe_renderer_lockstep(
            lockstep,
            reported ? &remote : nullptr,
            device->node_index,
            false);
    }
    return lockstep;
}

// One place where every physical-timing condition is combined, so the
// Program gate, the operator surface, and the diagnostic stream can never
// disagree about why an output is or is not ready.
sync::SyncReadiness DirectorShowLoop::evaluate_sync_gate(bool assets_adopted,
                                                         bool renderer_ready) {
    sync::SyncReadinessInput gate{};
    gate.policy = &director_sync.policy;
    gate.topology = &director_sync.topology;
    gate.nodes = director_sync.status.nodes;
    gate.exact_show_frame_ready = true;
    gate.assets_adopted = assets_adopted;
    gate.render_nodes_live =
        std::all_of(director_sync.topology.node_indices.begin(),
                    director_sync.topology.node_indices.end(),
                    [&](core::u32 node_index) {
                        const sync::NodeSyncStatus *const node =
                            director_sync.status.node(node_index);
                        return node != nullptr && node->runtime_reported;
                    });
    gate.renderer_ready = renderer_ready;
    // Not a node fact, and the one this gate could not see: every node can
    // be locked while the clock they are all keyed to free-runs here.
    gate.director_clock_referenced = !director_clock.drifting();
    return sync::evaluate_sync_readiness(gate);
}

// Renderer conformance gates Program independently of physical-sync policy
// and of whichever other condition currently owns the sync headline.
bool DirectorShowLoop::program_sync_held() noexcept {
    return director_renderer_lockstep.holds_program(
        director_sync.policy.requires_physical_sync(),
        director_sync.readiness.satisfied);
}

ClusterObservation DirectorShowLoop::observe_cluster(core::u64 show_frame) {
    ClusterObservation observation{};
    const auto barrier_diagnostics = barrier.observer_diagnostics();
    observation.unix_time_ms = static_cast<core::u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    observation.control_frame = scheduler.sealed_through().value();
    observation.show_frame = show_frame;
    const std::size_t observed_sessions =
        barrier_diagnostics.live_sessions +
        barrier_diagnostics.catching_up_sessions;
    const core::u32 observed_node_count =
        static_cast<core::u32>(std::min<std::size_t>(
            observed_sessions, std::numeric_limits<core::u32>::max()));
    if (!cluster_configuration_authored) {
        configured_cluster_nodes =
            std::max(configured_cluster_nodes, observed_node_count);
    }
    observation.configured_nodes = configured_cluster_nodes;
    observation.live_sessions = static_cast<core::u32>(
        std::min<std::size_t>(barrier_diagnostics.live_sessions,
                              std::numeric_limits<core::u32>::max()));
    observation.catching_up_sessions = static_cast<core::u32>(
        std::min<std::size_t>(barrier_diagnostics.catching_up_sessions,
                              std::numeric_limits<core::u32>::max()));
    const core::u64 released =
        barrier.released_through().through_frame.value();
    observation.release_lag_frames =
        observation.control_frame > released
            ? observation.control_frame - released
            : 0U;
    observation.pacer_lag_frames =
        pacer_lag(pacer, observation.control_frame);
    observation.open_barriers =
        static_cast<core::u64>(barrier.open_frames());
    observation.present_failures = present_failures;
    if (session_service != nullptr) {
        observation.sessions = session_service->counters();
    }
    observation.barrier = barrier_diagnostics;
    observation.diagnostics_recording = cluster_diagnostics.available();
    observation.sync_mode =
        std::string{sync::sync_mode_name(director_sync.policy.mode)};
    observation.sync_state =
        std::string{director_sync.readiness.physical_headline()};
    observation.program_state =
        std::string{director_sync.readiness.headline()};
    observation.sync_required =
        director_sync.policy.requires_physical_sync();
    observation.sync_satisfied = director_sync.readiness.satisfied;
    observation.sync_degraded = director_sync.readiness.degraded;
    observation.sync_groups = director_sync.readiness.groups;
    observation.sync_expected_outputs =
        director_sync.readiness.expected_outputs;
    observation.sync_locked_outputs =
        director_sync.readiness.locked_outputs;
    observation.sync_barrier_joined_outputs =
        director_sync.readiness.barrier_joined_outputs;
    observation.sync_configuration_issues =
        static_cast<core::u32>(director_sync.topology.diagnostics.size());
    for (const sync::NodeSyncStatus &node : director_sync.status.nodes) {
        observation.sync_simulated =
            observation.sync_simulated || node.simulated;
        observation.sync_missed_frames += node.counters.missed;
        observation.sync_late_frames += node.counters.late;
        observation.sync_held_frames += node.counters.held;
        observation.sync_repeated_frames += node.counters.repeated;
        observation.sync_loss_events += node.counters.loss_of_sync_events;
    }
    // Physical timing is proven, never assumed. Only a non-simulated
    // verdict that satisfied a policy demanding it may set these.
    observation.hardware_frame_lock_verified =
        director_sync.readiness.verified;
    observation.external_time_sync_verified =
        director_sync.readiness.verified &&
        director_sync.policy.requires_external_reference();
    observation.health = cluster_control_health(observation);
    return observation;
}

} // namespace mediaserver::app
