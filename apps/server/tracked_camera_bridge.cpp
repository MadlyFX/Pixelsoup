#include "tracked_camera_bridge.hpp"

#include <cmath>
#include <limits>

namespace mediaserver::server {

std::optional<StampedTrackerSample> stamped_tracker_sample(
    const track::TrackedPose& pose,
    doc::NodeId camera_node,
    core::u64 sampled_control_frame,
    core::u32 latency_frames,
    float field_of_view_turns,
    float aspect_ratio) noexcept {
    if (!track::valid(pose) || camera_node.value() == 0U) {
        return std::nullopt;
    }
    const core::u64 compensation = static_cast<core::u64>(latency_frames);
    if (sampled_control_frame
        > std::numeric_limits<core::u64>::max() - compensation) {
        return std::nullopt;
    }

    StampedTrackerSample stamped{};
    // Forward, not backward: the pose sampled now is the pose the wall should
    // be showing by the time this frame has crossed the wire, been rendered,
    // and been presented.
    stamped.control_frame = sampled_control_frame + compensation;
    net::TrackedCameraSample& sample = stamped.sample;
    sample.camera_node = camera_node.value();
    sample.position_x = pose.x;
    sample.position_y = pose.y;
    sample.position_z = pose.z;
    sample.yaw_turns = pose.pan;
    sample.pitch_turns = pose.tilt;
    sample.roll_turns = pose.roll;
    sample.field_of_view_turns = field_of_view_turns;
    sample.aspect_ratio = aspect_ratio;
    sample.tracker_sequence = pose.sequence;
    sample.camera_id = pose.camera_id;
    sample.focal_length_mm = pose.focal_length_mm;
    sample.focus_distance_m = pose.focus_distance_m;
    sample.aperture_f_number = pose.aperture_f_number;
    sample.zoom_encoder = pose.zoom_encoder;
    sample.focus_encoder = pose.focus_encoder;
    sample.iris_encoder = pose.iris_encoder;
    sample.zoom_encoder_valid = pose.zoom_encoder_valid;
    sample.focus_encoder_valid = pose.focus_encoder_valid;
    sample.iris_encoder_valid = pose.iris_encoder_valid;
    // The same check the codec applies, made here so an unusable lens or a
    // corrupt pose costs nothing on the wire and every node simply holds.
    if (!net::valid(sample)) {
        return std::nullopt;
    }
    return stamped;
}

track::StampedPose stamped_pose(
    const net::TrackedCameraSample& sample, core::u64 control_frame) noexcept {
    track::TrackedPose pose{};
    // The wire form carries no protocol, because by this point the Director has
    // already decided which tracker the show is using and every node must use
    // that decision rather than one of its own.
    pose.protocol = track::TrackerProtocol::OpenTrackIO;
    pose.x = sample.position_x;
    pose.y = sample.position_y;
    pose.z = sample.position_z;
    pose.pan = sample.yaw_turns;
    pose.tilt = sample.pitch_turns;
    pose.roll = sample.roll_turns;
    pose.focal_length_mm = sample.focal_length_mm;
    pose.focus_distance_m = sample.focus_distance_m;
    pose.aperture_f_number = sample.aperture_f_number;
    pose.zoom_encoder = sample.zoom_encoder;
    pose.focus_encoder = sample.focus_encoder;
    pose.iris_encoder = sample.iris_encoder;
    pose.zoom_encoder_valid = sample.zoom_encoder_valid;
    pose.focus_encoder_valid = sample.focus_encoder_valid;
    pose.iris_encoder_valid = sample.iris_encoder_valid;
    pose.sequence = sample.tracker_sequence;
    pose.camera_id = sample.camera_id;
    return track::StampedPose{pose, control_frame};
}

track::StampedPose stamped_pose(const StampedTrackerSample& stamped) noexcept {
    return stamped_pose(stamped.sample, stamped.control_frame);
}

TrackedCameraLens tracked_camera_lens(
    const net::TrackedCameraSample& sample) noexcept {
    return TrackedCameraLens{sample.field_of_view_turns, sample.aspect_ratio};
}

scene::LensEncoders tracked_camera_encoders(
    const net::TrackedCameraSample& sample) noexcept {
    return scene::clamped(scene::LensEncoders{
        sample.zoom_encoder,
        sample.focus_encoder,
        sample.iris_encoder,
        sample.zoom_encoder_valid,
        sample.focus_encoder_valid,
        sample.iris_encoder_valid,
    });
}

scene::TrackedCamera evaluated_tracked_camera(
    const track::PoseForFrame& answer,
    const TrackedCameraLens& lens,
    const scene::TrackerCalibration& calibration) noexcept {
    scene::TrackedCamera tracked{};
    if (answer.state == track::PoseHold::None) {
        return tracked;
    }
    // Mapped into stage space here, at the one boundary where a tracker's
    // numbers become the scene's. Everything downstream - the plate rectangle,
    // the plate's view, the viewport gizmo, the digest - then deals only in
    // stage coordinates and needs to know nothing about how the tracker was
    // surveyed. The calibration is replicated, so every node maps the same
    // datagram to the same stage pose.
    tracked.position = scene::stage_position_from_tracker(
        scene::Vec3{answer.pose.x, answer.pose.y, answer.pose.z}, calibration);
    tracked.yaw = scene::stage_pan_from_tracker(answer.pose.pan, calibration);
    tracked.pitch = answer.pose.tilt;
    tracked.roll = answer.pose.roll;
    tracked.field_of_view = lens.field_of_view_turns;
    tracked.aspect_ratio = lens.aspect_ratio;
    tracked.focal_length_mm = answer.pose.focal_length_mm;
    tracked.focus_distance_m = answer.pose.focus_distance_m;
    tracked.aperture_f_number = answer.pose.aperture_f_number;
    // The frame the pose was *stamped* for, not the frame being drawn. Two
    // nodes holding the same pose must digest the same value, and what they
    // agree on is which pose they are holding.
    tracked.control_frame = answer.stamped_frame;
    tracked.sequence = answer.pose.sequence;
    tracked.held = answer.state == track::PoseHold::Held;
    tracked.valid = true;
    return tracked;
}

scene::LensEncoders evaluated_lens_encoders(
    const track::PoseForFrame& answer) noexcept {
    if (answer.state == track::PoseHold::None) {
        return scene::LensEncoders{};
    }
    return scene::clamped(scene::LensEncoders{
        answer.pose.zoom_encoder,
        answer.pose.focus_encoder,
        answer.pose.iris_encoder,
        answer.pose.zoom_encoder_valid,
        answer.pose.focus_encoder_valid,
        answer.pose.iris_encoder_valid,
    });
}

} // namespace mediaserver::server
