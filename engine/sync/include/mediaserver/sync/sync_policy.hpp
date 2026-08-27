#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>

#include <string_view>

// The project-wide output synchronization policy.
//
// This is authored document state, not a measurement. It says what the show
// requires of physical output timing; whether the hardware actually meets it
// is a separate transient observation (sync_status.hpp) and a separate gate
// (sync_readiness.hpp). Keeping the two apart is the whole point: a saved
// project must never be able to claim that outputs are locked.
namespace mediaserver::sync {

enum class SyncMode : core::u8 {
    // No physical timing claim. Outputs free-run against the show clock and
    // the operator surface says so.
    Unsynced,
    // GPU output heads are expected to be frame-locked to each other and the
    // swapchains joined to a common present barrier. No external house
    // reference is required.
    Framelock,
    // Frame lock plus an external house reference (black burst / tri-level /
    // PTP-disciplined generator) that must be present and actually in use by
    // the sync device.
    ExternalGenlock,
};

// What happens when the required sync state cannot be proven. Strict refuses
// to promote Program; Degraded promotes but keeps the operator surface in a
// permanent warning state. Neither ever silently reports a green online state.
enum class SyncFailurePolicy : core::u8 {
    Degraded,
    Strict,
};

// What an output does when its frame is not ready at the raster boundary.
//
// For an LED wall the safe default is to repeat the previous complete frame
// across the whole synchronization group: one repeated frame everywhere is
// far less visible than one wall segment advancing while its neighbour does
// not. AdvanceIndependently is offered only for cases where each output is a
// genuinely independent surface.
enum class LateFramePolicy : core::u8 {
    HoldGroup,
    AdvanceIndependently,
};

// One exact output timing. Refresh is rational so 60 and 60000/1001 remain
// different values rather than both rounding to "60".
struct SyncTiming final {
    core::u32 width{1920U};
    core::u32 height{1080U};
    core::u32 refresh_numerator{60U};
    core::u32 refresh_denominator{1U};

    bool operator==(const SyncTiming&) const = default;

    [[nodiscard]] bool valid() const noexcept {
        return width != 0U && height != 0U && refresh_numerator != 0U &&
            refresh_denominator != 0U;
    }
};

[[nodiscard]] bool same_raster(const SyncTiming& left,
                               const SyncTiming& right) noexcept;
// Exact rational equality. 60/1 and 60000/1001 are deliberately not equal:
// mixing them is precisely the configuration that drifts.
[[nodiscard]] bool same_refresh(const SyncTiming& left,
                                const SyncTiming& right) noexcept;
[[nodiscard]] bool same_timing(const SyncTiming& left,
                               const SyncTiming& right) noexcept;
// Operator-facing "1920x1080 @ 60000/1001" text.
[[nodiscard]] std::string_view sync_mode_name(SyncMode mode) noexcept;
[[nodiscard]] std::string_view
sync_failure_policy_name(SyncFailurePolicy policy) noexcept;
[[nodiscard]] std::string_view
late_frame_policy_name(LateFramePolicy policy) noexcept;

inline constexpr core::u32 max_sync_group_id = 4'095U;
inline constexpr core::u32 max_sync_barrier_id = 4'095U;
inline constexpr core::u32 max_sync_timing_width = 7'680U;
inline constexpr core::u32 max_sync_timing_height = 4'320U;
// A rational refresh above this is not a display mode, it is a typo.
inline constexpr core::u32 max_sync_refresh_numerator = 1'000'000U;
inline constexpr core::u32 max_sync_refresh_denominator = 1'001'000U;

struct SyncPolicy final {
    // Zero/authored=false means the project has no sync.policy node. The
    // defaults below are then in force and the project stays UNSYNCED, which
    // is the only state that makes no physical claim.
    doc::NodeId node{};
    bool authored{};
    SyncMode mode{SyncMode::Unsynced};
    // The timing every output in every synchronization group must report.
    SyncTiming required_timing{};
    // Vendor swap-group and present-barrier identifiers. They are authored
    // rather than derived so a show can share a barrier with equipment this
    // application does not own.
    core::u32 group{1U};
    core::u32 barrier{1U};
    SyncFailurePolicy failure{SyncFailurePolicy::Strict};
    LateFramePolicy late_frame{LateFramePolicy::HoldGroup};

    bool operator==(const SyncPolicy&) const = default;

    // True when the policy asserts something about physical timing that must
    // be proven before Program can claim to be online.
    [[nodiscard]] bool requires_physical_sync() const noexcept {
        return mode != SyncMode::Unsynced;
    }
    [[nodiscard]] bool requires_external_reference() const noexcept {
        return mode == SyncMode::ExternalGenlock;
    }
};

[[nodiscard]] bool valid_policy(const SyncPolicy& policy) noexcept;

// FNV-1a over the authored policy in its deterministic field order.
[[nodiscard]] core::u64 digest(const SyncPolicy& policy) noexcept;

} // namespace mediaserver::sync
