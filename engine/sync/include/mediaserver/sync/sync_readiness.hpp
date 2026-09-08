#pragma once

#include <mediaserver/sync/sync_group.hpp>
#include <mediaserver/sync/sync_policy.hpp>
#include <mediaserver/sync/sync_status.hpp>

#include <span>
#include <string_view>

// The Program readiness gate for physical output timing.
//
// Program is allowed to claim it is online only when every one of these is
// proven at once:
//
//   1. The exact show frame is ready and assets are adopted (owned upstream,
//      passed in here as already-decided booleans).
//   2. Every configured render node is live.
//   3. The authored synchronization groups have compatible display timing.
//   4. Where the policy asks for it, a sync device exists, the reference is
//      present and actually in use, and every group output reports locked.
//   5. Every expected swapchain has joined the expected present barrier.
//
// Until all of that holds the operator surface reports the specific reason -
// SYNC UNVERIFIED, REFERENCE LOST, SWAP BARRIER INCOMPLETE, TIMING MISMATCH -
// rather than a generic green state.
namespace mediaserver::sync {

enum class SyncReadinessCode : core::u8 {
    // Everything the policy asks for is proven.
    Ready,
    // The policy makes no physical timing claim, so there is nothing to
    // prove. Reported separately from Ready so the surface can say UNSYNCED
    // instead of implying measured lock.
    Unsynced,
    // Upstream show-state gates are not satisfied yet.
    ShowFrameNotReady,
    AssetsNotAdopted,
    RenderNodesMissing,
    // The Program renderer cohort has a missing/stale profile, lacks a
    // backend, shader set, self-check, or feature capability, or selected
    // different Basic/Advanced backends.
    RendererUnavailable,
    // The Hardware Map does not describe any synchronized output.
    NoConfiguredOutputs,
    // Group members disagree about raster or refresh, or disagree with the
    // policy's required timing.
    TimingMismatch,
    // A cabled output has no display binding, so it has no target to lock.
    OutputsUnbound,
    // No node has reported physical timing at all.
    Unverified,
    SyncDeviceMissing,
    ReferenceLost,
    ReferenceNotInUse,
    OutputsUnlocked,
    // Joined nothing yet. Ordinary during startup, and a fault only if it
    // persists - so it is the one barrier state worth waiting on rather than
    // acting on.
    SwapBarrierIncomplete,
    // This build, driver, or adapter has no present barrier at all. Nothing on
    // the stage fixes it: it is a different build or different hardware.
    SwapBarrierUnsupported,
    // Joined, but to a different group or barrier than the policy authorises,
    // or with fewer outputs than the node was expected to bring. The operator
    // fixes this by editing the project, which is why it must not read as the
    // same fault as a driver that cannot do it at all.
    SwapBarrierMisconfigured,
    // A client was dropped after joining. It was working and now is not, which
    // is a different event from never having joined.
    SwapBarrierLost,
    // Still joined, but the driver's own in-sync count is falling behind its
    // present count. The wall is drifting apart while nothing has failed, and
    // that reads as a hardware fault in the room long before anything reports
    // an error.
    SwapBarrierOutOfSync,
    // The local ShowClock is not receiving the external frame source required
    // by the authored policy, so it is deliberately holding rather than
    // silently falling back to steady_clock.
    ShowClockReferenceUnavailable,
    ShowClockReferenceLost,
    // The Director is pacing the control frame on its own system clock while
    // the policy asks for an external reference.
    //
    // Its own code because it is the one fault every node reports as healthy.
    // Each node can be locked to house reference, its barrier joined and its
    // outputs in sync, and the cohort still drifts - because the clock they are
    // all keyed to is this machine's steady_clock. An operator reading a wall
    // of green node rows has no way to see it, which is exactly why it must
    // stop Program rather than only appear in a log.
    DirectorClockUnreferenced,
    // Every fact came from a simulated provider.
    Simulated,
};

// The exact operator wording for a gate result. Deliberately not a generic
// "offline"/"online" pair.
[[nodiscard]] std::string_view
sync_readiness_headline(SyncReadinessCode code) noexcept;

struct SyncReadinessInput final {
    const SyncPolicy* policy{};
    const SyncTopology* topology{};
    std::span<const NodeSyncStatus> nodes{};
    bool exact_show_frame_ready{true};
    bool assets_adopted{true};
    // Every configured render node holds a live session at the current
    // project generation.
    bool render_nodes_live{true};
    // Already accounts for authored fallback policy and cohort parity. An
    // optional fallback is ready only when every Program renderer selected
    // the same fallback backend.
    bool renderer_ready{true};
    // The Director's own control-frame pacer, which is not a node and does not
    // appear in `nodes`. Defaulted true so a caller that has not wired it up
    // reads exactly as this gate did before the field existed.
    bool director_clock_referenced{true};
};

struct SyncReadiness final {
    // True when Program may be promoted. Under a Degraded failure policy this
    // can be true while `code` still names an unproven condition; `degraded`
    // then says so and the surface must keep warning.
    bool satisfied{};
    bool degraded{};
    // True only when the physical claim is actually proven by non-simulated
    // evidence. This is the only value allowed to turn a timing line green.
    bool verified{};
    // The first unmet condition overall, including the upstream show-state
    // gates. This is what an operator must fix next to promote Program.
    SyncReadinessCode code{SyncReadinessCode::Unverified};
    // The physical-timing verdict alone, evaluated independently of the
    // upstream gates. A timing line must report this one: "assets not
    // adopted" is a true reason Program is held, but it is not a statement
    // about raster boundaries and must not appear as one.
    SyncReadinessCode physical{SyncReadinessCode::Unverified};
    core::u32 expected_outputs{};
    core::u32 locked_outputs{};
    core::u32 barrier_joined_outputs{};
    core::u64 barrier_presents{};
    core::u64 barrier_presents_in_sync{};
    core::u32 groups{};
    core::u32 reporting_nodes{};

    bool operator==(const SyncReadiness&) const = default;

    [[nodiscard]] std::string_view headline() const noexcept {
        return sync_readiness_headline(code);
    }
    [[nodiscard]] std::string_view physical_headline() const noexcept {
        return sync_readiness_headline(physical);
    }
};

[[nodiscard]] SyncReadiness
evaluate_sync_readiness(const SyncReadinessInput& input) noexcept;

} // namespace mediaserver::sync
