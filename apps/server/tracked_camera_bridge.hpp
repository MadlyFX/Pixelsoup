#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/net/session_protocol.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/track/pose_channel.hpp>
#include <mediaserver/track/tracked_pose.hpp>

#include <mediaserver/doc/ids.hpp>

#include <optional>

// The two ends of the tracking channel, and the arithmetic between them.
//
// `engine/track` knows nothing about a wire or a scene. `engine/net` knows
// nothing about a tracker or a camera. `engine/scene` knows nothing about
// either. That separation is deliberate - it is what keeps a tracker read
// structurally impossible on a node - and this is the one place the three meet,
// which is the process that already links all three.
//
// Both directions are pure functions of their inputs, so both are testable, and
// the one piece of arithmetic that matters - latency compensation - is pinned
// by a test rather than living inline in a frame loop.
namespace mediaserver::server {

// Director side: stamps a tracker sample for the control frame it should apply
// to, and names the camera it drives.
//
// The control frame and not the show frame. A parked timeline stops advancing
// the show frame, so every sample would carry the same stamp, the channel would
// report every one of them as a repeat, and the wall would freeze at whichever
// pose happened to arrive first - exactly when an operator is lining up a shot
// and most wants it live. The control frame is the replication clock the frame
// barrier already holds nodes together on.
//
// The compensation is authored project state in frames, applied here, once, by
// the Director. Never machine-local and never adaptive: a node measuring its own
// latency would compensate differently from its neighbour, and the two halves of
// the plate would then show different moments of the same move.
//
// `field_of_view_turns` is the vertical angle the Director resolved from the
// tracker's focal length and the authored sensor height. It is resolved
// upstream so every node receives one already-decided angle rather than
// re-deriving it from a lens table each might hold a different version of.
//
// Absent when the pose or the resulting frustum is unusable - a tracker fault,
// a lens the profile cannot resolve - in which case nothing is sent and every
// node holds. Holding is always better than swinging the wall.
struct StampedTrackerSample final {
    net::TrackedCameraSample sample{};
    // The control frame the sample applies to, after compensation. Carried
    // beside rather than inside the sample because one message stamps the
    // whole stack of cameras with one frame.
    core::u64 control_frame{};

    bool operator==(const StampedTrackerSample&) const = default;
};

[[nodiscard]] std::optional<StampedTrackerSample> stamped_tracker_sample(
    const track::TrackedPose& pose,
    doc::NodeId camera_node,
    core::u64 sampled_control_frame,
    core::u32 latency_frames,
    float field_of_view_turns,
    float aspect_ratio) noexcept;

// Node side: the wire form as something a `track::PoseChannel` can hold.
[[nodiscard]] track::StampedPose stamped_pose(
    const net::TrackedCameraSample& sample, core::u64 control_frame) noexcept;

// Director side: the same, for a sample it has just stamped itself.
[[nodiscard]] track::StampedPose stamped_pose(
    const StampedTrackerSample& stamped) noexcept;

// The sensor aspect and lens angle a wire sample carried, which the pose value
// itself has no field for. Kept beside the channel so a node reconstructs the
// evaluated camera from replicated numbers rather than from local settings.
struct TrackedCameraLens final {
    float field_of_view_turns{0.125F};
    float aspect_ratio{16.0F / 9.0F};

    bool operator==(const TrackedCameraLens&) const = default;
};

[[nodiscard]] TrackedCameraLens tracked_camera_lens(
    const net::TrackedCameraSample& sample) noexcept;

// The frame-stamped controls that pair with the pose. This is deliberately a
// separate value from `TrackedCameraLens`: sensor calibration is authored in a
// camera profile, while encoder positions change every frame.
[[nodiscard]] scene::LensEncoders tracked_camera_encoders(
    const net::TrackedCameraSample& sample) noexcept;

// Node side: what a channel answer means for the scene being evaluated.
//
// A held answer produces a *valid* tracked camera carrying the held pose and
// marked `held`, because holding is what every node does and they must all draw
// the same thing. An answer of None - nothing ever received - produces an
// invalid value, and the wall then shows the authored view everywhere.
[[nodiscard]] scene::TrackedCamera evaluated_tracked_camera(
    const track::PoseForFrame& answer,
    const TrackedCameraLens& lens,
    const scene::TrackerCalibration& calibration = {}) noexcept;

[[nodiscard]] scene::LensEncoders evaluated_lens_encoders(
    const track::PoseForFrame& answer) noexcept;

} // namespace mediaserver::server
