#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/sync/sync_policy.hpp>
#include <mediaserver/topology/hardware_map.hpp>

#include <string>
#include <string_view>
#include <vector>

// Transient physical-timing observations.
//
// These values are never document properties, never contribute to a project
// digest, and never make a project dirty. Two independent producers fill them
// in and they are deliberately kept apart:
//
//  * The machine-local lifecycle agent reports the sync device: whether a
//    board exists, whether a house reference is present, whether the device
//    is actually using it, and the lock state and timing of each display
//    target. It can answer these while the render child is stopped.
//  * The render process reports what only it can know: whether its swapchains
//    joined the expected present barrier, and how many frames it presented,
//    missed, ran late, held, or repeated.
//
// Anything not measured stays Unknown/Unsupported. There is no default that
// means "probably locked".
namespace mediaserver::sync {

enum class SyncDeviceAvailability : core::u8 {
    // Nothing has reported yet.
    Unknown,
    // This platform/build has no vendor sync integration. Not a fault, but
    // never evidence of lock either.
    Unsupported,
    // The vendor interface answered and there is no sync device.
    Absent,
    Present,
    // The vendor interface failed. Fails closed like Absent, but is reported
    // separately so an operator can tell a driver problem from a missing card.
    Failed,
};

enum class SyncReferenceState : core::u8 {
    Unknown,
    // No reference signal at the sync device input.
    Absent,
    // A reference is present but the device is not disciplined to it.
    Present,
    // Present and actually in use as the timing source. Only this state
    // satisfies EXTERNAL GENLOCK.
    InUse,
    // Present but the device reports it as out of range or unstable.
    Unstable,
    Unsupported,
};

enum class SyncLockState : core::u8 {
    Unknown,
    Unlocked,
    Locked,
    Unsupported,
};

enum class PresentBarrierState : core::u8 {
    Unknown,
    // The presentation backend cannot join a vendor present barrier at all.
    Unsupported,
    NotJoined,
    Joining,
    Joined,
    // Joined earlier and dropped out. Distinguished from NotJoined because it
    // is a loss-of-sync event, not a configuration that never started.
    Left,
    Failed,
};

[[nodiscard]] std::string_view
sync_device_availability_name(SyncDeviceAvailability value) noexcept;
[[nodiscard]] std::string_view
sync_reference_state_name(SyncReferenceState value) noexcept;
[[nodiscard]] std::string_view sync_lock_state_name(SyncLockState value)
    noexcept;
[[nodiscard]] std::string_view
present_barrier_state_name(PresentBarrierState value) noexcept;

// One display target's observed lock and timing.
struct SyncOutputStatus final {
    topology::HardwareDisplayTargetKey target{};
    SyncLockState lock{SyncLockState::Unknown};
    // Zero fields mean the timing was not reported, which is not the same as
    // a zero-rate mode.
    SyncTiming timing{0U, 0U, 0U, 0U};
    bool timing_reported{};

    bool operator==(const SyncOutputStatus&) const = default;
};

// Render-runtime frame accounting for one node. These are cumulative since
// the render process started.
struct SyncFrameCounters final {
    core::u64 presented{};
    // Boundaries where no complete frame existed to present.
    core::u64 missed{};
    // Frames presented after their raster deadline.
    core::u64 late{};
    // Boundaries where the previous complete frame was deliberately held by
    // the group late-frame policy.
    core::u64 held{};
    // Frames the presentation backend showed again because no new one was
    // acquired.
    core::u64 repeated{};
    // Transitions out of a locked/joined state.
    core::u64 loss_of_sync_events{};

    bool operator==(const SyncFrameCounters&) const = default;
};

// Everything known about one configured render node's output timing.
struct NodeSyncStatus final {
    core::u32 node_index{};
    // Machine-agent half.
    SyncDeviceAvailability board{SyncDeviceAvailability::Unknown};
    SyncReferenceState reference{SyncReferenceState::Unknown};
    std::vector<SyncOutputStatus> outputs;
    // Render-runtime half.
    PresentBarrierState barrier{PresentBarrierState::Unknown};
    core::u32 barrier_group{};
    core::u32 barrier_id{};
    core::u32 barrier_joined_outputs{};
    core::u32 barrier_expected_outputs{};
    SyncFrameCounters counters{};
    // True when this sample came from a deliberately simulated provider. It
    // is carried all the way to the operator surface and can never satisfy a
    // strict policy: simulated evidence is not evidence.
    bool simulated{};
    // True when the machine agent answered at all this poll.
    bool machine_reported{};
    // True when the render runtime answered at all this poll.
    bool runtime_reported{};
    std::string detail;

    bool operator==(const NodeSyncStatus&) const = default;

    [[nodiscard]] core::u32 locked_outputs() const noexcept;
    [[nodiscard]] core::u32 unlocked_outputs() const noexcept;
};

struct SyncStatusSnapshot final {
    std::vector<NodeSyncStatus> nodes;

    [[nodiscard]] const NodeSyncStatus* node(core::u32 node_index)
        const noexcept;

    bool operator==(const SyncStatusSnapshot&) const = default;
};

} // namespace mediaserver::sync
