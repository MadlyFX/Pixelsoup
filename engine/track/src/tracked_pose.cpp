#include <mediaserver/track/tracked_pose.hpp>

#include <cmath>

namespace mediaserver::track {

bool valid(const TrackedPose& pose) noexcept {
    if (pose.protocol == TrackerProtocol::None) {
        return false;
    }
    if (!std::isfinite(pose.x) || !std::isfinite(pose.y)
        || !std::isfinite(pose.z)) {
        return false;
    }
    if (!std::isfinite(pose.pan) || !std::isfinite(pose.tilt)
        || !std::isfinite(pose.roll)) {
        return false;
    }
    // Zero is the explicit "no encoder" form for each lens field, so only a
    // negative or non-finite reported value is a fault.
    if (!std::isfinite(pose.focal_length_mm) || pose.focal_length_mm < 0.0F) {
        return false;
    }
    if (!std::isfinite(pose.focus_distance_m) || pose.focus_distance_m < 0.0F) {
        return false;
    }
    if (!std::isfinite(pose.aperture_f_number)
        || pose.aperture_f_number < 0.0F) {
        return false;
    }
    // FreeD's 24-bit encoder is the largest supported raw domain. Sources
    // that report physical controls are far below the same bound. Refusing an
    // out-of-range value here keeps corrupted controls out of the replicated
    // frame channel before a profile can interpret them.
    constexpr float max_encoder = 16'777'215.0F;
    const auto valid_encoder = [max_encoder](float value) noexcept {
        return std::isfinite(value) && value >= 0.0F && value <= max_encoder;
    };
    return valid_encoder(pose.zoom_encoder)
        && valid_encoder(pose.focus_encoder)
        && valid_encoder(pose.iris_encoder);
}

} // namespace mediaserver::track
