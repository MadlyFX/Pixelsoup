#include "director_live_data.hpp"

#include <mediaserver/output/perspective_mapping.hpp>
#include <mediaserver/scene/lens.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>

namespace mediaserver::app {

// Resolves every tracked camera's pose for one control frame and writes it back
// onto the scene, so the renderer draws what the channels are holding.
//
// Resolve once for the main scene on each host. Other views peek at the same
// answer so diagnostics count control frames, rather than the number of views.
void inject_tracked_poses(scene::Scene &scene,
                          TrackedCameraChannels &channels,
                          core::u64 control_frame, bool advance) {
    for (scene::TrackedCameraView &view : scene.tracked_cameras) {
        TrackedCameraChannels::Entry &entry = channels.open(view.camera);
        const track::PoseForFrame answer =
            advance ? entry.channel.resolve(control_frame)
                    : entry.channel.peek(control_frame);
        view.pose = server::evaluated_tracked_camera(answer, entry.lens,
                                                     view.calibration);
        view.encoders = server::evaluated_lens_encoders(answer);
    }
}

// The plate rectangle *and* whether it is the whole-wall fallback. Returning
// only the rectangle threw the distinction away at the one place the operator
// could have been told about it, and a clamped plate is not a detail: it costs
// every pixel of the wall.
[[nodiscard]] std::optional<scene::InnerFrustumRegion> director_inner_frustum_rect(
    const scene::Scene &frame, const scene::TrackedCameraView &view,
    const output::FeedMap &feed_map) {
    if (!view.drawing() || !frame.renderer_settings.icvfx.inner_frustum ||
        feed_map.stage.anchor == output::StageAnchor::Camera) {
        // Nodes draw no plate on a stage carried by the scene camera; see the
        // presenter's matching refusal.
        return std::nullopt;
    }
    const scene::Camera tracked = scene::tracked_view_camera(
        view.authored, view.pose, view.encoders);
    for (const output::ProjectionSurface &surface : feed_map.surfaces) {
        if (surface.shape != output::ProjectionSurfaceShape::Planar) {
            continue;
        }
        // The same lens the wall sizes its plate with. This readout is the
        // operator's only view of where the plate is, so deriving it from a
        // different lens than the node uses would report a rectangle no wall
        // is drawing - which is how item 11.1 was found.
        const auto region = view.authored.lens_profile.has_value()
            ? output::inner_frustum_region(
                  surface.geometry, tracked, *view.authored.lens_profile,
                  view.encoders, scene::clamped(view.icvfx).margin_px)
            : output::inner_frustum_region(
                  surface.geometry, tracked, view.pose.aspect_ratio,
                  scene::clamped(view.icvfx).margin_px);
        if (region && region.value().valid) {
            return region.value();
        }
    }
    return std::nullopt;
}

// What the Live Data panel reports: one row per tracked camera, in the order
// their plates are composited.
// One field's worth of what every node reported about tracking.
//
// Compact on purpose: the status line is scanned, not parsed, and the question
// an operator asks of it is "is any node not drawing what the others are". So
// each node contributes one token - its index and its state - and the counters
// behind that live on the Live Data panel.
//
// "none" means no node has reported at all, which is different from every node
// reporting that it has no pose, and reads differently here.
[[nodiscard]] std::string summarize_node_tracking(
    std::span<const editor::NodeTrackingObservation> nodes) {
    if (nodes.empty()) {
        return "none";
    }
    std::string text;
    for (const editor::NodeTrackingObservation &node : nodes) {
        if (!text.empty()) {
            text.push_back(',');
        }
        std::array<char, 24U> index{};
        const auto written = std::snprintf(
            index.data(), index.size(), "%llu:",
            static_cast<unsigned long long>(node.node_index));
        if (written > 0) {
            text.append(index.data(),
                        static_cast<std::size_t>(written));
        }
        switch (editor::node_tracking_report(node)) {
        case editor::NodeTrackingReport::Silent:
            text += "silent";
            break;
        case editor::NodeTrackingReport::NoPose:
            text += "no-pose";
            break;
        case editor::NodeTrackingReport::NotPlanned:
            text += "no-plate";
            break;
        case editor::NodeTrackingReport::NoSlice:
            text += "no-slice";
            break;
        case editor::NodeTrackingReport::Drawing:
            text += "drawing";
            break;
        case editor::NodeTrackingReport::DrawingBehind:
            text += "behind";
            break;
        case editor::NodeTrackingReport::DrawingSlow:
            text += "dropping";
            break;
        }
    }
    return text;
}

[[nodiscard]] editor::LiveDataState director_live_data(
    const server::TrackerListenerPool &listeners,
    const TrackedCameraChannels &channels,
    const server::DirectorPosePredictors &predictors,
    const scene::Scene &frame,
    const output::FeedMap &feed_map,
    core::u64 control_frame,
    core::u64 show_frame,
    std::vector<editor::NodeTrackingObservation> nodes) {
    editor::LiveDataState live{};
    live.nodes = std::move(nodes);
    live.icvfx = frame.renderer_settings.icvfx;
    live.canvas = frame.canvas;
    live.show_frame = show_frame;
    live.cameras.reserve(frame.tracked_cameras.size());

    for (const scene::TrackedCameraView &view : frame.tracked_cameras) {
        const server::TrackerListenerStatus status =
            listeners.status(view.source);
        editor::TrackedCameraObservation observed{};
        observed.camera = view.camera;
        observed.icvfx = view.icvfx;
        {
            std::array<char, 48U> name{};
            const int length =
                std::snprintf(name.data(), name.size(), "CAMERA %llu",
                              static_cast<unsigned long long>(
                                  view.camera.value()));
            observed.name.assign(
                name.data(),
                static_cast<std::size_t>(std::max(length, 0)));
        }
        observed.tracking.state =
            static_cast<editor::TrackingLinkState>(
                static_cast<core::u8>(status.state));
        observed.tracking.source = status.source;
        observed.tracking.datagrams = status.datagrams;
        observed.tracking.accepted = status.accepted;
        observed.tracking.truncated = status.truncated;
        observed.tracking.unrecognized = status.unrecognized;
        observed.tracking.checksum_failures = status.checksum_failures;
        observed.tracking.invalid_values = status.invalid_values;
        observed.tracking.foreign_camera = status.foreign_camera;
        observed.tracking.awaiting_handshake = status.awaiting_handshake;
        observed.tracking.untracked = status.untracked;
        observed.tracking.server_description = status.server_description;
        observed.tracking.last_error = status.last_error;
        if (const auto* predictor = predictors.find(view.camera)) {
            auto prediction = predictor->telemetry();
            prediction.raw_position = scene::stage_position_from_tracker(
                prediction.raw_position, view.calibration);
            prediction.output_position = scene::stage_position_from_tracker(
                prediction.output_position, view.calibration);
            prediction.raw_angles.x = scene::stage_pan_from_tracker(
                prediction.raw_angles.x, view.calibration);
            prediction.output_angles.x = scene::stage_pan_from_tracker(
                prediction.output_angles.x, view.calibration);
            prediction.next_error.position_metres *= view.calibration.scale;
            prediction.latency_error.position_metres *= view.calibration.scale;
            observed.tracking.prediction = prediction;
            if (prediction.state == scene::PosePredictionState::Refused)
                observed.tracking.last_error = std::string{predictor->refusal_reason()};
        }

        // The pose the wall is actually drawing, not whatever last arrived on
        // the socket: the scene already carries the channel's answer for this
        // frame, so the numbers and the rectangle cannot disagree.
        if (view.pose.valid) {
            observed.tracking.has_pose = true;
            observed.tracking.position = view.pose.position;
            observed.tracking.yaw = view.pose.yaw;
            observed.tracking.pitch = view.pose.pitch;
            observed.tracking.roll = view.pose.roll;
            observed.tracking.field_of_view = view.pose.field_of_view;
            observed.tracking.tracker_sequence = view.pose.sequence;
        }
        if (const auto *entry = channels.find(view.camera); entry != nullptr) {
            observed.tracking.poses_sent = entry->accepted;
            observed.tracking.held_frames = entry->channel.counters().held_frames;
            observed.tracking.longest_hold =
                entry->channel.counters().longest_hold;
        }
        if (const auto region =
                director_inner_frustum_rect(frame, view, feed_map)) {
            observed.inner_rect = region->rect;
            observed.inner_rect_clamped = region->clamped;
        }
        live.cameras.push_back(std::move(observed));
    }
    static_cast<void>(control_frame);
    return live;
}

} // namespace mediaserver::app
