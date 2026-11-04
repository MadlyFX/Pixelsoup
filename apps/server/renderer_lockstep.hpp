#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/net/session_protocol.hpp>
#include <mediaserver/scene/scene.hpp>

#include <string_view>

namespace mediaserver::server {

enum class RendererLockstepCode : core::u8 {
    Inactive,
    Ready,
    ObservationMissing,
    ProfileMismatch,
    RendererUnavailable,
    SelectionMismatch,
    TemporalHistoryMismatch,
    ConformancePending,
    ConformanceFailed,
    ConformanceMismatch,
    FrameBudgetExceeded,
    FrameTimingPending,
    VideoMemoryShort,
};

[[nodiscard]] std::string_view renderer_lockstep_headline(
    RendererLockstepCode code) noexcept;

// Allocation-free fold over the Program renderer cohort. The Director starts
// one value from the exact authored settings, then observes its local editor
// and every configured output node. Once failed it remains failed for that
// tick; a later healthy member cannot conceal the first unsafe condition.
struct RendererLockstep final {
    RendererLockstepCode code{RendererLockstepCode::Inactive};
    core::u64 profile_digest{};
    net::RuntimeRendererBackend requested{
        net::RuntimeRendererBackend::Unknown};
    net::RuntimeRendererBackend selected{
        net::RuntimeRendererBackend::Unknown};
    core::u32 failing_node{};
    bool local_failure{};
    bool require_requested_selection{};
    bool require_temporal_agreement{};
    core::u32 frame_budget_us{};
    bool hold_over_budget{true};
    core::u64 temporal_restarts{};
    core::u64 temporal_restart_frame{};
    core::u64 conformance_request{};
    core::RenderConformance conformance{};
    core::ConformanceComparison conformance_difference{};
    // Whether the Director's own renderer feeds a physical output. When it
    // only previews, its GPU running short of memory holds nothing: Program
    // is rendered by the nodes, and a laptop running the show must not stop
    // a wall its render nodes can draw.
    bool local_drives_output{};
    // VideoMemoryShort: what the failing node still needs and has free.
    core::u64 video_memory_needed{};
    core::u64 video_memory_free{};
    // The Director's preview reported VideoMemoryShort without holding.
    core::u64 preview_memory_needed{};
    core::u64 preview_memory_free{};

    [[nodiscard]] bool satisfied() const noexcept {
        if (!hold_over_budget && (code == RendererLockstepCode::FrameBudgetExceeded
            || code == RendererLockstepCode::FrameTimingPending)) return true;
        return code == RendererLockstepCode::Inactive ||
               code == RendererLockstepCode::Ready;
    }
    // Renderer evidence is an independent gate. A higher-priority sync
    // headline (e.g. a missing node) must not hide it in an unsynced show.
    [[nodiscard]] bool holds_program(bool physical_sync_required,
                                     bool physical_sync_satisfied) const noexcept {
        return !satisfied() || (physical_sync_required && !physical_sync_satisfied);
    }
};

[[nodiscard]] RendererLockstep begin_renderer_lockstep(
    const scene::RendererSettings& settings,
    bool active) noexcept;

void observe_renderer_lockstep(
    RendererLockstep& state,
    const net::RuntimeRendererReadiness* observation,
    core::u32 node_index,
    bool local) noexcept;

} // namespace mediaserver::server
