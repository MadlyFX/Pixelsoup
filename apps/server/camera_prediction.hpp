#pragma once

#include "tracked_camera_bridge.hpp"
#include <mediaserver/track/pose_slot.hpp>

#include <array>
#include <span>

namespace mediaserver::server {

// Used only by the Director. PoseChannel and the node receive path remain
// hold-only. All storage is fixed; a fit consumes at most sixteen samples.
class CameraPosePredictor final {
public:
    // Discontinuities restart the fit while keeping the consumed-publication
    // fence. A cadence edit cannot turn a held socket value into a new sample.
    void restart_fit() noexcept;
    void configure(doc::NodeId source, const scene::TrackingSourceSettings& settings,
                   const scene::IcvfxSettings& policy) noexcept;
    [[nodiscard]] std::optional<StampedTrackerSample> sample(
        const std::optional<track::PoseSample>& raw, doc::NodeId camera,
        core::u64 control_frame, core::u64 target_frame,
        float field_of_view, float aspect_ratio) noexcept;
    [[nodiscard]] const scene::PosePredictionTelemetry& telemetry() const noexcept {
        return telemetry_;
    }
    [[nodiscard]] std::string_view refusal_reason() const noexcept { return refusal_reason_; }
private:
    using Components = std::array<double, 6U>;
    struct History final { core::u64 frame{}; Components value{}; };
    struct Forecast final {
        core::u64 frame{};
        Components value{};
        bool pending{};
    };
    void clear_fit() noexcept;
    void measure(Forecast& forecast, core::u64 frame, const Components& raw,
                 scene::PosePredictionError& error) noexcept;
    std::array<History, scene::max_pose_prediction_window> history_{};
    std::array<Forecast, scene::max_icvfx_latency_frames + 1U> forecasts_{};
    Forecast next_frame_{};
    std::size_t count_{};
    scene::IcvfxSettings policy_{};
    scene::TrackingSourceSettings source_settings_{};
    doc::NodeId source_{};
    bool configured_{};
    bool seen_control_{};
    core::u64 last_control_{};
    bool seen_publication_{};
    core::u64 last_publication_{};
    track::TrackedPose last_raw_{};
    scene::PosePredictionTelemetry telemetry_{};
    std::string_view refusal_reason_{};
};

// Retires removed/rebound cameras before assigning slots. Reordering the plate
// stack never changes a fit; repeated scene edits cannot grow this bank.
class DirectorPosePredictors final {
public:
    void retain(std::span<const scene::TrackedCameraView> views) noexcept;
    void set_cadence(core::u32 numerator, core::u32 denominator) noexcept;
    [[nodiscard]] CameraPosePredictor* find(doc::NodeId camera) noexcept;
    [[nodiscard]] const CameraPosePredictor* find(doc::NodeId camera) const noexcept;
    void reset() noexcept;
private:
    struct Entry final { doc::NodeId camera{}; CameraPosePredictor predictor{}; };
    std::array<Entry, scene::max_tracked_cameras> entries_{};
    core::u32 rate_numerator_{};
    core::u32 rate_denominator_{};
};

} // namespace mediaserver::server
