#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/scene/scene.hpp>

namespace mediaserver::render {

enum class HistorySurface : core::u8 {
    Program,
    Preview,
    Stage,
    TransitionFrom,
    TransitionTo,
    Offscreen,
};

// Complete identity of temporal state. No pointer or wall-clock value enters
// it, so every node can reach the same reset/hold decision independently.
struct HistoryKey final {
    scene::SceneId scene{};
    doc::Version document_version{};
    HistorySurface surface{HistorySurface::Offscreen};
    core::u64 surface_id{};
    // The authored projection domain is part of history identity. Two output
    // nodes may have equal local target extents while rendering different
    // canvas regions; reusing temporal state after either mapping changes
    // would manufacture motion at the new slice boundary.
    scene::Canvas canvas{};
    scene::CanvasRect slice{};
    core::u32 width{};
    core::u32 height{};
    doc::NodeId camera_source{};
    core::u64 camera_digest{};
    // Zero for the scene camera's ordinary slice projection. Physical and
    // other explicit views carry a stable mapping identity. Animated matrices
    // are deliberately excluded; document_version resets calibration edits.
    core::u64 view_digest{};
    core::u64 profile_digest{};
    scene::MediaFrameRate motion_rate{};
    float object_shutter{};
    std::optional<core::u64> temporal_document_digest;

    bool operator==(const HistoryKey&) const = default;
};

enum class HistoryDisposition : core::u8 {
    Initialize,
    Advance,
    Hold,
    Reset,
};

struct HistoryDecision final {
    HistoryDisposition disposition{HistoryDisposition::Initialize};
    core::u64 previous_show_frame{};
    core::u64 show_frame{};
    core::u64 reset_count{};

    bool operator==(const HistoryDecision&) const = default;
};

// A tiny state machine owned by one Program/Preview/Stage/transition surface.
// Repeating N is a hold: it must not advance jitter, reservoirs, probes, or
// denoiser moments. Any discontinuity resets instead of trying to infer intent.
class TemporalHistory final {
public:
    [[nodiscard]] HistoryDecision observe(
        const HistoryKey& key,
        core::u64 show_frame,
        core::u64 topology_digest,
        core::u64 visual_digest = 0U) noexcept;

    void reset() noexcept;

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] core::u64 reset_count() const noexcept {
        return reset_count_;
    }

private:
    HistoryKey key_{};
    core::u64 show_frame_{};
    core::u64 topology_digest_{};
    core::u64 visual_digest_{};
    core::u64 reset_count_{};
    bool initialized_{};
};

// Counter-based deterministic sample source. Absolute canvas coordinates are
// mandatory; slice-local coordinates would create different noise at a seam.
[[nodiscard]] core::u64 deterministic_sample_bits(
    core::u64 show_frame,
    core::u64 stable_identity,
    core::u32 canvas_x,
    core::u32 canvas_y,
    core::u32 dimension) noexcept;

[[nodiscard]] float deterministic_sample_unit(
    core::u64 show_frame,
    core::u64 stable_identity,
    core::u32 canvas_x,
    core::u32 canvas_y,
    core::u32 dimension) noexcept;

[[nodiscard]] core::u64 camera_history_digest(
    const scene::Camera& camera) noexcept;
[[nodiscard]] core::u64 view_history_digest(
    const scene::ViewMatrices& matrices,
    scene::Vec3 eye,
    scene::Vec3 forward) noexcept;
[[nodiscard]] core::u64 renderer_profile_digest(
    const scene::RendererSettings& settings) noexcept;

} // namespace mediaserver::render
