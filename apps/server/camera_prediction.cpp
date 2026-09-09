#include "camera_prediction.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mediaserver::server {
namespace {
using Components = std::array<double, 6U>;
[[nodiscard]] Components components(const track::TrackedPose& pose) noexcept {
    return {pose.x, pose.y, pose.z, pose.pan, pose.tilt, pose.roll};
}
[[nodiscard]] track::TrackedPose with_components(
    track::TrackedPose pose, const Components& value) noexcept {
    pose.x = static_cast<float>(value[0]);
    pose.y = static_cast<float>(value[1]);
    pose.z = static_cast<float>(value[2]);
    pose.pan = static_cast<float>(scene::deterministic::shortest_turn(value[3]));
    pose.tilt = static_cast<float>(scene::deterministic::shortest_turn(value[4]));
    pose.roll = static_cast<float>(scene::deterministic::shortest_turn(value[5]));
    return pose;
}
}

void CameraPosePredictor::clear_fit() noexcept {
    count_ = 0;
    for (auto& forecast : forecasts_) {
        if (forecast.pending) ++telemetry_.latency_error.missed;
        forecast.pending = false;
    }
    if (next_frame_.pending) ++telemetry_.next_error.missed;
    next_frame_.pending = false;
    telemetry_.fit_samples = 0;
}

void CameraPosePredictor::restart_fit() noexcept {
    clear_fit();
    if (configured_) ++telemetry_.resets;
    telemetry_.state = scene::PosePredictionState::Waiting;
}

void CameraPosePredictor::configure(doc::NodeId source,
    const scene::TrackingSourceSettings& settings,
    const scene::IcvfxSettings& policy) noexcept {
    const auto bounded = scene::clamped(policy);
    const bool changed = !configured_ || source_ != source
        || source_settings_ != settings
        || policy_.pose_prediction != bounded.pose_prediction
        || policy_.prediction_window != bounded.prediction_window
        || policy_.jitter_filter_percent != bounded.jitter_filter_percent
        || policy_.latency_frames != bounded.latency_frames;
    if (!changed) return;
    clear_fit();
    if (configured_) ++telemetry_.resets;
    // Keep the publication fence when an operator tunes the filter: the old
    // socket sample must not become a fresh measurement under a new policy.
    if (source_ != source) seen_publication_ = false;
    policy_ = bounded;
    source_ = source;
    source_settings_ = settings;
    configured_ = true;
    telemetry_.state = scene::PosePredictionState::Waiting;
}

void CameraPosePredictor::measure(Forecast& forecast, core::u64 frame,
    const Components& raw, scene::PosePredictionError& error) noexcept {
    if (!forecast.pending || forecast.frame > frame) return;
    forecast.pending = false;
    if (forecast.frame != frame) {
        ++error.missed;
        return;
    }
    double squared_distance = 0.0;
    double angle = 0.0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double difference = forecast.value[axis] - raw[axis];
        squared_distance += difference * difference;
        angle = std::max(angle, std::abs(scene::deterministic::shortest_turn(
            forecast.value[axis + 3] - raw[axis + 3])));
    }
    error.position_metres = std::sqrt(squared_distance);
    error.angle_turns = angle;
    error.control_frame = frame;
    ++error.samples;
}

std::optional<StampedTrackerSample> CameraPosePredictor::sample(
    const std::optional<track::PoseSample>& raw, doc::NodeId camera,
    core::u64 control_frame, core::u64 target_frame,
    float field_of_view, float aspect_ratio) noexcept {
    using State = scene::PosePredictionState;
    if (seen_control_ && control_frame == last_control_) {
        // The editor can redraw several times per control frame. Preserve the
        // last meaningful state instead of flashing "held" between samples.
        return std::nullopt;
    }
    if (seen_control_ && control_frame < last_control_) {
        clear_fit();
        ++telemetry_.resets;
    }
    seen_control_ = true;
    last_control_ = control_frame;
    if (telemetry_.has_sample && target_frame <= telemetry_.target_frame) {
        telemetry_.state = State::Repeated;
        ++telemetry_.held;
        return std::nullopt;
    }
    if (!raw || (seen_publication_ && raw->publications == last_publication_)) {
        telemetry_.state = telemetry_.has_sample ? State::Held : State::Waiting;
        ++telemetry_.held;
        // No new extrapolation, filtering, or forecast evaluation on a hold.
        return std::nullopt;
    }
    if (seen_publication_ && raw->publications > last_publication_
        && raw->publications - last_publication_ > 1U) {
        telemetry_.coalesced += raw->publications - last_publication_ - 1U;
    }
    if (count_ != 0 && raw->pose.sequence == last_raw_.sequence
        && raw->pose.camera_id == last_raw_.camera_id
        && raw->pose.protocol == last_raw_.protocol) {
        last_publication_ = raw->publications;
        telemetry_.state = State::Held;
        ++telemetry_.held;
        return std::nullopt;
    }
    const bool source_restart = seen_publication_ && (
        raw->publications < last_publication_
        || raw->pose.sequence < last_raw_.sequence
        || raw->pose.camera_id != last_raw_.camera_id
        || raw->pose.protocol != last_raw_.protocol);
    last_publication_ = raw->publications;
    seen_publication_ = true;
    last_raw_ = raw->pose;
    const auto refuse = [&](std::string_view reason) -> std::optional<StampedTrackerSample> {
        clear_fit();
        ++telemetry_.refused;
        telemetry_.state = State::Refused;
        refusal_reason_ = reason;
        return std::nullopt;
    };
    // Validate before touching a fit. Overflow and a monotonic stamp clamp
    // beyond the authored bound are explicit refusals, never longer forecasts.
    if (!configured_) return refuse("Prediction has no configured tracking source");
    if (!source_settings_.enabled) return refuse("Prediction source is disabled");
    if (target_frame < control_frame
        || control_frame > std::numeric_limits<core::u64>::max() - policy_.latency_frames
        || target_frame - control_frame > scene::max_icvfx_latency_frames)
        return refuse("Prediction stamp overflows or exceeds the 16-frame horizon");
    if (!stamped_tracker_sample(raw->pose, camera, control_frame, 0, 0.125F, 1.0F))
        return refuse("Raw tracking pose is outside the replicated pose bounds");
    if (!stamped_tracker_sample(raw->pose, camera, control_frame, 0, field_of_view, aspect_ratio))
        return refuse("Tracked lens angle or aspect is invalid");

    Components value = components(raw->pose);
    if (source_restart || (count_ != 0
        && control_frame - history_[count_ - 1].frame > scene::max_pose_prediction_window)) {
        clear_fit();
        ++telemetry_.resets;
    }
    measure(next_frame_, control_frame, value, telemetry_.next_error);
    for (auto& forecast : forecasts_)
        measure(forecast, control_frame, value, telemetry_.latency_error);

    // Unwrap adjacent orientations before fitting. Normalize again only at
    // publication. A +179 to -179 degree pan is a two-degree move.
    if (count_ != 0) {
        for (std::size_t axis = 3; axis < 6; ++axis) {
            const double previous = history_[count_ - 1].value[axis];
            value[axis] = previous + scene::deterministic::shortest_turn(value[axis] - previous);
        }
    }
    if (count_ == policy_.prediction_window) {
        std::rotate(history_.begin(), history_.begin() + 1,
                    history_.begin() + static_cast<std::ptrdiff_t>(count_));
        --count_;
    }
    history_[count_++] = History{control_frame, value};
    telemetry_.fit_samples = static_cast<core::u32>(count_);
    Components filtered = value;
    Components velocity{};
    const bool fitting = policy_.pose_prediction || policy_.jitter_filter_percent != 0;
    if (fitting && count_ >= 2) {
        std::array<scene::deterministic::LinearMotionSample,
                   scene::max_pose_prediction_window> samples{};
        for (std::size_t axis = 0; axis < 6; ++axis) {
            for (std::size_t index = 0; index < count_; ++index) {
                samples[index] = {-static_cast<double>(control_frame - history_[index].frame),
                                  history_[index].value[axis]};
            }
            const auto fit = scene::deterministic::fit_linear_motion(
                std::span{samples}.first(count_));
            const double strength = static_cast<double>(policy_.jitter_filter_percent) / 100.0;
            filtered[axis] += (fit.present - value[axis]) * strength;
            velocity[axis] = fit.velocity;
        }
    }
    Components predicted = filtered;
    const double horizon = static_cast<double>(target_frame - control_frame);
    if (policy_.pose_prediction) {
        for (std::size_t axis = 0; axis < 6; ++axis)
            predicted[axis] += velocity[axis] * horizon;
    }
    // Preserve raw bits when disabled or warming; lens encoders always carry
    // the actual measurement rather than an extrapolated lens calibration.
    const auto output = fitting && count_ >= 2
        ? with_components(raw->pose, predicted) : raw->pose;
    auto stamped = stamped_tracker_sample(output, camera, target_frame, 0,
                                          field_of_view, aspect_ratio);
    if (!stamped) return refuse("Predicted pose exceeds the replicated pose bounds");
    refusal_reason_ = {};
    telemetry_.has_sample = true;
    telemetry_.raw_position = {raw->pose.x, raw->pose.y, raw->pose.z};
    telemetry_.raw_angles = {raw->pose.pan, raw->pose.tilt, raw->pose.roll};
    telemetry_.output_position = {output.x, output.y, output.z};
    telemetry_.output_angles = {output.pan, output.tilt, output.roll};
    telemetry_.sampled_frame = control_frame;
    telemetry_.target_frame = target_frame;
    telemetry_.state = !fitting ? State::Off : count_ < 2 ? State::Warming
        : policy_.pose_prediction && horizon > 0 ? State::Predicting : State::Filtering;
    if (telemetry_.state == State::Predicting) ++telemetry_.predicted;

    if (fitting && count_ >= 2) {
        if (control_frame != std::numeric_limits<core::u64>::max()) {
            Components next = filtered;
            for (std::size_t axis = 0; axis < 6; ++axis) next[axis] += velocity[axis];
            // Validate the wire-rounded result, which is what an operator is
            // tuning. The one-step error is available on the very next stamp.
            next_frame_ = {control_frame + 1, components(with_components(raw->pose, next)), true};
        }
        if (policy_.pose_prediction && horizon > 0) {
            auto& pending = forecasts_[static_cast<std::size_t>(target_frame % forecasts_.size())];
            if (pending.pending) ++telemetry_.latency_error.missed;
            pending = {target_frame, components(output), true};
        }
    }
    return stamped;
}

void DirectorPosePredictors::retain(std::span<const scene::TrackedCameraView> views) noexcept {
    for (auto& entry : entries_) {
        if (std::none_of(views.begin(), views.end(), [&](const auto& view) {
            return view.camera == entry.camera;
        })) entry = {};
    }
    for (const auto& view : views) {
        if (find(view.camera)) continue;
        for (auto& entry : entries_) {
            if (entry.camera.value() == 0) { entry.camera = view.camera; break; }
        }
    }
}
CameraPosePredictor* DirectorPosePredictors::find(doc::NodeId camera) noexcept {
    if (camera.value() == 0) return nullptr;
    for (auto& entry : entries_) if (entry.camera == camera) return &entry.predictor;
    return nullptr;
}
const CameraPosePredictor* DirectorPosePredictors::find(doc::NodeId camera) const noexcept {
    if (camera.value() == 0) return nullptr;
    for (const auto& entry : entries_) if (entry.camera == camera) return &entry.predictor;
    return nullptr;
}
void DirectorPosePredictors::reset() noexcept { entries_ = {}; }

void DirectorPosePredictors::set_cadence(core::u32 numerator, core::u32 denominator) noexcept {
    if (rate_numerator_ == numerator && rate_denominator_ == denominator) return;
    for (auto& entry : entries_) entry.predictor.restart_fit();
    rate_numerator_ = numerator;
    rate_denominator_ = denominator;
}

} // namespace mediaserver::server
