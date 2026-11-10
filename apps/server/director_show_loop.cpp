// The Director's two loops: the sealing thread (pacing, sealing, retiring),
// the show thread's driver and operator input dispatch, and the evaluations
// every phase shares.

#include "director_show_loop.hpp"

#include "asset_activation.hpp"
#include "cluster_observability.hpp"
#include "edit_coalescing_policy.hpp"
#include "playback_runtime.hpp"
#include "renderer_lockstep.hpp"
#include "tracked_camera_bridge.hpp"

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
#include <mediaserver/track/pose_channel.hpp>
#include <mediaserver/track/pose_slot.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace mediaserver::app {

int DirectorShowLoop::run() {
    // Stops and joins the sealing thread however the show thread leaves.
    struct Sealing final {
        DirectorShowLoop &loop;
        std::thread thread;
        ~Sealing() {
            loop.sealing_stopped.store(true, std::memory_order_relaxed);
            loop.seal_waiter.notify();
            if (thread.joinable()) {
                thread.join();
            }
        }
    };
    int exit_code = 1;
    {
        Sealing sealing{*this, {}};
        // Taken before the sealing thread exists and held, apart from where
        // the phases leave it, until this thread is done.
        const AuthorityGate::Show inside{authority_gate};
        // What start-up asked for - autoplay, the project to reopen - goes
        // onto the first control frame, before anything can seal it and
        // before the first editor frame, which builds every GPU resource
        // the editor has and can take seconds.
        if (const PhaseExit exit = author_operator_input()) {
            return *exit;
        }
        try {
            sealing.thread = std::thread{[this] { control_loop(); }};
        } catch (const std::system_error &error) {
            std::cerr << "could not start the Director sealing thread: "
                      << error.what() << '\n';
            return 1;
        }
        exit_code = show_loop();
    }
    if (exit_code != 0) {
        return exit_code;
    }
    // The editor only closes on a saved project or once the operator agreed
    // to give its changes up, so there is nothing left to offer back. Any
    // other way out of this loop leaves the recovery copy where it is.
    if (closed && editor && project_autosave != nullptr) {
        project_autosave->discard(project_path);
    }
    report_stopping();
    return 0;
}

// The sealing thread. Each wake: wait for the gate, pace, seal what is due,
// retire what the cohort finished, tell the show thread, sleep to the next
// boundary. Every fault it can meet ends the Director; it leaves the exit
// code - or the exception - in the notes for the show thread, which owns the
// way out and is where the process reports either.
void DirectorShowLoop::control_loop() {
    while (!sealing_stopped.load(std::memory_order_relaxed)) {
        Clock::time_point next_wake{};
        bool sealed_any = false;
        bool ended = false;
        {
            const Clock::time_point woke = Clock::now();
            const AuthorityGate::Seal inside{authority_gate};
            try {
                // Measured from the wake, so the figure is how late the loop
                // left its timer; the gate reports the wait that followed.
                boundary_lateness.observe(woke);
                ControlTick tick{};
                PhaseExit exit;
                switch (pace(tick)) {
                case PaceOutcome::Ready:
                    exit = seal_due_frames(tick);
                    break;
                case PaceOutcome::Retry:
                    break;
                case PaceOutcome::Failed:
                    exit = 1;
                    break;
                }
                // Retire in order. A frame whose cohort has not finished
                // stays, and so does everything behind it, which is what
                // bounds the backlog pace() watches.
                while (!tracked.empty() && barrier.retire(tracked.front())) {
                    tracked.pop_front();
                }
                sealed_any = tick.sealed != 0U;
                if (sealed_any && !exit.has_value()) {
                    sample_tracked_cameras(scheduler.sealed_through());
                }
                if (exit.has_value()) {
                    sealer_notes.exit = exit;
                    ended = true;
                } else if (options.run_frames != 0U &&
                           sealed >= options.run_frames) {
                    sealer_notes.finished = true;
                    ended = true;
                }
                next_wake = pacer.next_boundary_at();
                boundary_lateness.arm(next_wake);
            } catch (...) {
                sealer_notes.failure = std::current_exception();
                ended = true;
            }
        }
        if (sealed_any || ended) {
            frame_waiter.notify();
        }
        if (ended) {
            return;
        }
        // To the boundary, or for max_sleep when it is further off than
        // that. Only the show thread stopping the loop notifies this waiter.
        const Clock::time_point give_up = Clock::now() + max_sleep;
        while (!seal_waiter.wait_until_notified(next_wake, max_sleep)) {
            const Clock::time_point now = Clock::now();
            if (now >= next_wake || now >= give_up) {
                break;
            }
        }
    }
}

// The show thread, called inside the gate. It holds the gate for the whole
// iteration and gives it up where the phases say (drawing, presenting, file
// writes) and while it waits.
int DirectorShowLoop::show_loop() {
    while (stop_requested == 0 && !closed) {
        adopt_sealer_notes();
        if (sealer_notes.failure) {
            // Thrown again here, where main reports what stops the Director.
            std::rethrow_exception(
                std::exchange(sealer_notes.failure, nullptr));
        }
        if (sealer_notes.exit.has_value()) {
            return *sealer_notes.exit;
        }
        // Read before the frame is evaluated: once the last run frame has
        // been evaluated and presented below, there is nothing left to do.
        const bool last_frame_sealed = sealer_notes.finished;

        DirectorTick tick{};
        tick.now = Clock::now();
        sample_director_video_reference();
        begin_frame(tick);
        if (const PhaseExit exit = evaluate_scene(tick)) {
            return *exit;
        }
        evaluate_feed_and_sync(tick);
        observe_asset_cohort(tick);
        autosave_project(tick);
        if (const PhaseExit exit = refresh_editor(tick)) {
            return *exit;
        }
        if (const PhaseExit exit = present_output(tick)) {
            return *exit;
        }
        // After the editor and the presenter have said what the operator
        // asked for, so it reaches the very next control frame.
        if (const PhaseExit exit = author_operator_input()) {
            return *exit;
        }
        write_project_files();
        print_status(tick);

        if (last_frame_sealed) {
            break;
        }
        // The next thing worth waking for: a sealed frame, which the sealing
        // thread announces, or the editor's own refresh. A Director paced by
        // house reference also wakes at each boundary, because the sealing
        // thread paces from the sample this thread reads.
        Clock::time_point next_wake = Clock::time_point::max();
        if (editor) {
            next_wake = next_editor_refresh;
        }
        if (director_clock.requires_reference) {
            next_wake = std::min(next_wake, pacer.next_boundary_at());
        }
        const AuthorityGate::Away waiting{authority_gate};
        const Clock::time_point give_up = Clock::now() + max_sleep;
        while (!frame_waiter.wait_until_notified(next_wake, max_sleep)) {
            const Clock::time_point now = Clock::now();
            if (now >= next_wake || now >= give_up) {
                break;
            }
        }
    }
    return 0;
}

// Camera tracking, at the frame boundary and on the thread that owns it.
//
// This is the Director's one sample per frame per camera: each authored
// feed's pose slot is read without ever waiting on the receive thread, and the
// result is stamped with the exact control frame it applies to plus the
// authored latency compensation.
//
// Sampling here rather than beside the editor matters twice over. A headless
// Director tracks, so the stage does not depend on an operator window being
// open. And the frame a pose is stamped for is the frame it was sampled on,
// which is what makes the authored compensation a fixed offset rather than an
// approximate one - which it would not be if the sample waited for the show
// thread to finish drawing.
//
// Which cameras follow which feed comes from the frame the show thread last
// evaluated (tracking_plan). The show thread injects the accepted poses when
// it evaluates this control frame, after they were replicated here; the other
// way round would draw and digest last frame's stack while the nodes drew
// this frame's.
void DirectorShowLoop::sample_tracked_cameras(cmd::FrameNumber control_frame) {
    const std::vector<scene::TrackingSource> &tracking_sources =
        tracking_plan.sources;
    const show::FrameRate rate = transport.anchor().rate;
    director_pose_predictors.set_cadence(rate.numerator, rate.denominator);
    director_pose_predictors.retain(tracking_plan.cameras);
    std::array<std::optional<track::PoseSample>, scene::max_tracking_sources>
        source_samples{};
    for (std::size_t index = 0;
         index < tracking_sources.size() && index < source_samples.size();
         ++index) {
        source_samples[index] =
            tracker_listeners.sample(tracking_sources[index].node);
    }
    const core::u32 latency_frames =
        scene::clamped(tracking_plan.icvfx).latency_frames;
    net::TrackedCameraFrame replicated{};
    // The frame these poses are *for*, which is the sampled frame plus the
    // authored compensation. Sending the raw frame here instead - which this
    // did - meant every node applied the pose on arrival and the compensation
    // described nothing: the wire field's own comment says "after latency
    // compensation", and only the Director's local channel ever saw the
    // compensated value. A node cannot recompute it, because the compensation
    // is show state it is not required to have evaluated yet.
    const core::u64 compensated =
        control_frame.value() <=
                std::numeric_limits<core::u64>::max() - latency_frames
            ? control_frame.value() + latency_frames
            : control_frame.value();
    // Never backwards. Lowering the compensation mid-show would otherwise
    // stamp the next pose for an earlier frame than the last one, which every
    // node's channel refuses as stale - and a node that refuses every pose
    // holds the one it has, so the plate freezes exactly when an operator is
    // adjusting the thing meant to fix it. Clamping costs a few frames of
    // extra latency while the raw frame catches up, and then it is exact
    // again.
    //
    // Keep the last stamp during the clamp; nothing may invent a new one.
    // Pausing the show still advances control time and tracking.
    replicated.control_frame =
        std::max(compensated, last_replicated_pose_frame);
    for (const scene::TrackedCameraView &view : tracking_plan.cameras) {
        auto *predictor = director_pose_predictors.find(view.camera);
        const auto source = std::find_if(
            tracking_sources.begin(), tracking_sources.end(),
            [&](const auto &candidate) {
                return candidate.node == view.source;
            });
        if (!predictor || source == tracking_sources.end()) {
            continue;
        }
        predictor->configure(view.source, source->settings,
                             tracking_plan.icvfx);
        const auto &sampled = source_samples[static_cast<std::size_t>(
            source - tracking_sources.begin())];
        const auto stamped = predictor->sample(
            sampled, view.camera, control_frame.value(),
            replicated.control_frame,
            scene::tracked_field_of_view(
                source->settings,
                sampled ? sampled->pose.focal_length_mm : 0.0F),
            source->settings.sensor_aspect_ratio);
        if (!stamped.has_value()) {
            continue;
        }
        auto &entry = director_pose_channels.open(view.camera);
        entry.lens = server::tracked_camera_lens(stamped->sample);
        // Prediction already used this final wire stamp as its horizon. The
        // Director and nodes accept the same answer.
        if (entry.channel.accept(server::stamped_pose(
                stamped->sample, replicated.control_frame)) ==
            track::PoseChannelOutcome::Accepted) {
            ++entry.accepted;
            ++director_poses_sent;
            replicated.samples.push_back(stamped->sample);
        }
    }
    if (!replicated.samples.empty()) {
        last_replicated_pose_frame = replicated.control_frame;
        // Ascending camera order, which is what the wire contract asks for: a
        // node then applies the stack without sorting it, and a duplicated
        // camera is refused rather than silently overriding its own earlier
        // pose.
        std::sort(replicated.samples.begin(), replicated.samples.end(),
                  [](const net::TrackedCameraSample &left,
                     const net::TrackedCameraSample &right) {
                      return left.camera_node < right.camera_node;
                  });
        // Fanned out to every live session here, so every node is given
        // identical bits. This is the one place poses leave the Director, and
        // they leave once.
        static_cast<void>(barrier.publish_tracked_pose(replicated));
    }
}

void DirectorShowLoop::adopt_sealer_notes() {
    if (sealer_notes.project_message.has_value()) {
        project_message = std::move(*sealer_notes.project_message);
        sealer_notes.project_message.reset();
    }
    if (sealer_notes.publication_message.has_value()) {
        publication_message = std::move(*sealer_notes.publication_message);
        sealer_notes.publication_message.reset();
    }
}

// One authoring pass per control frame, onto the frame the sealing thread
// seals next. An anchor has to name the exact control frame it takes effect
// on, and inside the gate that frame cannot be sealed underneath the pass.
PhaseExit DirectorShowLoop::author_operator_input() {
    const core::u64 through = scheduler.sealed_through().value();
    if (through == std::numeric_limits<core::u64>::max()) {
        // The sealing thread reports this; there is no frame to author onto.
        return std::nullopt;
    }
    const cmd::FrameNumber frame{through + 1U};
    if (operator_input_frame == frame) {
        return std::nullopt;
    }
    operator_input_frame = frame;
    return apply_operator_input(frame, through);
}

PaceOutcome DirectorShowLoop::pace(ControlTick &tick) {
    auto &now = tick.now;
    auto &due = tick.due;
    now = Clock::now();
    if (editor_coalescing_policy.expire(now)) {
        scheduler.end_coalescing();
        pending_editor_coalescing_close = true;
    }

    // The pacing source, derived from the same replicated policy a node
    // uses to pick its own. It lags a policy change by a frame or so,
    // because the policy is evaluated by the show thread from the document
    // this thread seals - which is latency on an operator action, not a
    // source of drift.
    director_clock.requires_reference =
        director_sync.policy.requires_external_reference();
    const show::VideoReferenceSample director_reference =
        director_clock.requires_reference ? director_video_reference
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
    // A machine that stalled can leave a large pacing debt. Keep catch-up
    // within the barrier's remaining records, then service node
    // acknowledgements before sealing another batch. The barrier takes
    // a lagging node out of lockstep well before its records run out, so a
    // full barrier is a fault, not a slow node: it is reported, and it must
    // not terminate the editor after Save As.
    const std::size_t capacity = barrier.limits().max_open_frames;
    due = std::min(due, static_cast<core::u64>(
        capacity - std::min(tracked.size(), capacity)));
    // A fixed-length run seals exactly its frames, however late this woke.
    if (options.run_frames != 0U) {
        due = std::min(due, options.run_frames > sealed
                                ? options.run_frames - sealed
                                : core::u64{0U});
    }
    report_backlog(backlog_watch.observe(tracked.size() >= capacity, now));
    report_lockstep_demotions();
    if (paced_by_reference) {
        director_clock.referenced_frames += due;
    } else if (director_clock.requires_reference) {
        director_clock.unreferenced_frames += due;
    }
    return PaceOutcome::Ready;
}

PhaseExit DirectorShowLoop::seal_due_frames(ControlTick &tick) {
    const auto &now = tick.now;
    auto &due = tick.due;
    auto &sealed_this_iteration = tick.sealed;
    for (core::u64 index = 0U; index < due; ++index) {
        if (tracked.size() >= barrier.limits().max_open_frames) {
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

        // Whatever the operator asked for is already scheduled onto this
        // frame: the show thread authored it (author_operator_input) while
        // this was the next frame to seal.
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
            // An operator's anchor also travels ahead of the frame stream, so
            // a cluster held behind on frames stops, pauses or seeks now
            // rather than when it reaches this frame. The show's own anchors
            // - a cue, the end - play through in order and stay in the stream.
            if (!authored->scheduled &&
                barrier.publish_priority_transport(*authored) != 0U &&
                session_service != nullptr) {
                session_service->wake_workers();
            }
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
                    publication_activation=activation.value();
                    sealer_notes.publication_message="Publication scheduled at control frame "+std::to_string(activation.value())+", show frame "+std::to_string(show_frame)+".";
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
            // One press does both halves: the generation above tells every
            // node to look at its files again, and the Director looks at its
            // own once this frame is evaluated. The request is spent here, on
            // the frame that carries it, so neither half can be skipped by
            // which kind of wake happened to come first.
            asset_check_requested = true;
            pending_asset_rescan = false;
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
            repace_after_seal.reset();
            break;
        }
    }
    return std::nullopt;
}

PhaseExit DirectorShowLoop::apply_operator_input(cmd::FrameNumber frame,
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
    if (const PhaseExit exit = apply_project_load(op)) {
        return exit;
    }
    if (!project_loaded_this_frame) {
        for (const auto command : operator_commands) {
            apply_command(transport, command, frame);
        }
    }
    operator_commands.clear();
    apply_preview_transport(op);
    prepare_authoring_context(op);
    // Every phase from here to the save authors what the operator, Blender or
    // a publication asked for. One the document refuses is a request that did
    // not happen, not a reason to stop: the phase reports it, what it had
    // queued is dropped, and the show and the rest of the project carry on.
    if (apply_publication_requests(op)) {
        refuse_operator_request("PUBLISH REQUEST", frame);
    }
    if (apply_blender_links(op)) {
        refuse_operator_request("BLENDER LINK UPDATE", frame);
    }
    if (apply_scene_edits(op)) {
        refuse_operator_request("SCENE EDIT", frame);
    }
    if (apply_asset_imports(op)) {
        refuse_operator_request("LIBRARY EDIT", frame);
    }
    if (apply_key_requests(op)) {
        refuse_operator_request("KEYFRAME EDIT", frame);
    }
    if (apply_clip_requests(op)) {
        refuse_operator_request("CLIP EDIT", frame);
    }
    if (apply_element_and_lane_requests(op)) {
        refuse_operator_request("TRACK EDIT", frame);
    }
    if (apply_group_selection_and_mask_requests(op)) {
        refuse_operator_request("GROUP OR MASK EDIT", frame);
    }
    if (apply_timeline_edits(op)) {
        refuse_operator_request("TIMELINE EDIT", frame);
    }
    if (apply_property_and_effect_requests(op)) {
        refuse_operator_request("PROPERTY EDIT", frame);
    }
    if (apply_hardware_requests(op)) {
        refuse_operator_request("HARDWARE MAP EDIT", frame);
    }
    if (apply_feed_map_requests(op)) {
        refuse_operator_request("FEED MAP EDIT", frame);
    }
    if (apply_compositor_and_delete_requests(op)) {
        refuse_operator_request("COMPOSITOR EDIT", frame);
    }
    if (apply_switcher_request(op)) {
        refuse_operator_request("SCENE REQUEST", frame);
    }
    if (apply_show_timing_requests(op)) {
        refuse_operator_request("SHOW RATE CHANGE", frame);
    }
    if (apply_history_request(op)) {
        refuse_operator_request("UNDO OR REDO", frame);
    }
    if (const PhaseExit exit = apply_project_save(op)) {
        return exit;
    }
    pending_editor_coalescing.clear();
    if (pending_editor_coalescing_close) {
        scheduler.end_coalescing();
        pending_editor_coalescing_close = false;
    }
    return std::nullopt;
}

// The Director's own reference, from its own presenter.
//
// A Director with no output window has none, and reports Unsupported
// rather than pretending: an editor workstation is not a genlocked output,
// and neither is a headless Director on a machine with no card in the
// house chain. That is a real deployment and the honest answer is to say
// the cadence is unreferenced, not to invent one.
//
// Read here, on the show thread, because the presenter belongs to it and may
// be in the middle of a present when the sealing thread paces. The sealing
// thread advances by the frames counted between two of these samples, so a
// referenced Director still seals in step with the show thread's wakes; the
// system-clock Director, which is every editor, does not depend on it.
void DirectorShowLoop::sample_director_video_reference() noexcept {
    if (presenter == nullptr) {
        director_video_reference = show::VideoReferenceSample{
            show::VideoReferenceState::Unsupported, 0U};
        return;
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
    director_video_reference = mapped;
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
    lockstep.local_drives_output = options.output;
    if (observed_local_renderer.selected == net::RuntimeRendererBackend::Advanced) {
        const auto adapter = observed_local_renderer.conformance.adapter;
        director_conformance.poll(adapter, conformance_request, observed_local_renderer.conformance);
    }
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
    observation.control_stalled = backlog_watch.stalled();
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
