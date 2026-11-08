// The Director console status lines.

#include "director_show_loop.hpp"

#include "cluster_observability.hpp"
#include "director_live_data.hpp"
#include "playback_runtime.hpp"
#include "tracker_listener.hpp"

#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/scene/scene.hpp>

#include <iostream>
#include <string>

namespace mediaserver::app {

void DirectorShowLoop::print_status(DirectorTick &tick) {
    auto &sealed_this_iteration = tick.sealed_this_iteration;
    const auto &control_frame = tick.control_frame;
    auto &input = tick.input;
    const auto &needs_scene = tick.needs_scene;
    auto &scene_frame = tick.scene_frame;
    auto &authored_scene_digest = tick.authored_scene_digest;
    auto &feed_map = tick.feed_map;
    since_status += sealed_this_iteration;
    if (sealed_this_iteration != 0U && since_status >= interval) {
        since_status = 0U;
        ClusterObservation cluster =
            observe_cluster(input.show_frame.value());
        cluster.diagnostics_recording = cluster_diagnostics.available();
        const std::string node_tracking_text =
            summarize_node_tracking(director_node_tracking);
        static_cast<void>(cluster_diagnostics.append(cluster));
        std::cout << "director show-frame=" << input.show_frame.value()
                  << " control-frame=" << control_frame.value()
                  << " transport=" << transport_label(transport.state())
                  << " sealed=" << sealed << " anchors=" << anchors
                  << " pacer-lag=" << cluster.pacer_lag_frames
                  << boundary_wake_text(boundary_lateness.take())
                  << " tracked=" << tracked.size()
                  // Poses replaced in a session's mailbox before that
                  // session drained them. Nonzero means some node's plate
                  // is running behind the others'.
                  // What the control frame - the clock the whole
                  // cohort keys to - is actually paced by.
                  << " clock="
                  << (director_clock.requires_reference
                          ? (director_clock.locked() ? "reference"
                                                     : "unreferenced")
                          : "system")
                  << " clock-unreferenced="
                  << director_clock.unreferenced_frames
                  << " poses-sent="
                  << cluster.sessions.sent_tracked_poses
                  << " poses-coalesced="
                  << barrier.tracked_poses_coalesced()
                  // What the cohort did with those poses, aggregated
                  // across every node that reported. Printed here as well
                  // as on the Live Data panel because a headless Director
                  // is how a stage is usually run, and a plate stuck on
                  // one node is invisible from the Director's own
                  // counters: they say only what was sent.
                  << " nodes-tracking=" << node_tracking_text
                  << " sessions=" << hub.active_subscriptions()
                  << " live=" << cluster.live_sessions
                  << " catching-up=" << cluster.catching_up_sessions
                  << " release-lag=" << cluster.release_lag_frames
                  << " heartbeat-timeouts="
                  << cluster.sessions.heartbeat_timeouts
                  << " released-through="
                  << barrier.released_through().through_frame.value();
        // The same scene digest the nodes report. Printed here so an
        // operator can confirm the Director and every node agree on the
        // replicated scene at the same show frame.
        if (needs_scene) {
            std::cout << " digest=" << authored_scene_digest;
        }
        // And the same feed-map digest, so the operator can confirm the map
        // itself is replicated, not just the scene.
        if (options.output) {
            std::cout << " feed=" << output::digest(feed_map);
        }
        if (presenter != nullptr) {
            std::cout << gpu_timing_text(presenter->timing());
        }
        // Camera tracking, for the Director with no editor attached.
        // A headless stage machine is exactly the deployment that
        // needs to see the tracker is alive, and it has no panel to
        // look at. Printed only once a source is authored, so an
        // untracked show says nothing rather than a row of zeroes.
        for (const scene::TrackedCameraView &reported :
             scene_frame.tracked_cameras) {
            const server::TrackerListenerStatus tracking =
                tracker_listeners.status(reported.source);
            if (!tracking.source.enabled &&
                tracking.state == server::TrackerListenerState::Idle) {
                continue;
            }
            // Named by camera, because a stage with two of them needs to
            // know which one went quiet.
            std::cout
                << " camera" << reported.camera.value() << "="
                << server::tracker_listener_state_name(tracking.state)
                << " poses=" << tracking.accepted << '/'
                << tracking.datagrams;
            if (const auto *entry =
                    director_pose_channels.find(reported.camera);
                entry != nullptr) {
                std::cout << " held="
                          << entry->channel.counters().held_frames;
            }
        }
        std::cout << '\n' << std::flush;
    }
}

// A full backlog seals nothing, so nothing the operator does is applied. Say
// so where the operator is looking - the console for a headless Director, the
// status bar for an editor - and name what the oldest frame is waiting for.
void DirectorShowLoop::report_backlog(ControlBacklogEvent event) {
    if (event == ControlBacklogEvent::None) {
        return;
    }
    if (event == ControlBacklogEvent::Recovered) {
        std::cerr << "Director resumed sealing control frames; operator "
                     "edits apply again\n";
        project_message = "EDITS RESUMED";
        return;
    }
    std::cerr << "Director stalled: " << tracked.size()
              << " control frames are unretired, so no frame is sealed and "
                 "operator edits are not applied. released-through="
              << barrier.released_through().through_frame.value();
    bool waiting_on_nodes = false;
    if (!tracked.empty()) {
        if (const auto oldest = barrier.snapshot(tracked.front())) {
            const net::DirectorFrameBarrierSummary &summary = oldest.value();
            waiting_on_nodes =
                summary.prepared + summary.lost < summary.members;
            std::cerr << " oldest-frame="
                      << summary.after.through_frame.value()
                      << " members=" << summary.members
                      << " prepared=" << summary.prepared
                      << " committed=" << summary.committed
                      << " lost=" << summary.lost;
        }
    }
    std::cerr << '\n' << std::flush;
    project_message =
        waiting_on_nodes
            ? "EDITS PAUSED - a render node stopped acknowledging frames; "
              "see Output Monitor"
            : "EDITS PAUSED - the Director could not finish a frame; see "
              "its console";
}

// Every output waits for the slowest node, up to the barrier's lag limit.
// A node that passes it is taken out of lockstep so the rest can move on, and
// that is a real change on the wall - its outputs now trail the others - so
// it is said once, by node, when it happens.
void DirectorShowLoop::report_lockstep_demotions() {
    const net::DirectorFrameBarrierObserverDiagnostics diagnostics =
        barrier.observer_diagnostics();
    if (diagnostics.lockstep_demotions == reported_lockstep_demotions) {
        return;
    }
    reported_lockstep_demotions = diagnostics.lockstep_demotions;
    std::cerr << "Director took node " << diagnostics.last_demoted_node
              << " out of lockstep: it held every output more than "
              << barrier.limits().max_cohort_lag_frames
              << " frames behind. The other outputs continue; it rejoins "
                 "once it catches up. lockstep-demotions="
              << diagnostics.lockstep_demotions << '\n';
    project_message = "NODE " + std::to_string(diagnostics.last_demoted_node) +
                      " FELL BEHIND - outputs no longer wait for it until it "
                      "catches up";
}

void DirectorShowLoop::report_stopping() {
    ClusterObservation final_cluster =
        observe_cluster(transport.frame_at(scheduler.sealed_through()).value());
    final_cluster.diagnostics_recording = cluster_diagnostics.available();
    static_cast<void>(cluster_diagnostics.append(final_cluster));
    std::cout << "director stopping"
              << " show-frame="
              << transport.frame_at(scheduler.sealed_through()).value()
              << " control-frame=" << scheduler.sealed_through().value()
              << " transport=" << transport_label(transport.state())
              << " sealed=" << sealed << " anchors=" << anchors
              << " pacer-lag=" << final_cluster.pacer_lag_frames
              << " tracked=" << tracked.size()
              << " present-failures=" << present_failures << " surface="
              << (options.edit
                      ? "editor"
                      : (presenter && presenter->windowed() ? "window"
                                                            : "headless"))
              << " released-through="
              << barrier.released_through().through_frame.value();
    if (options.edit) {
        std::cout << " editor-frame-calls=" << editor_frame_calls;
    }
    std::cout << '\n' << std::flush;
}

} // namespace mediaserver::app
