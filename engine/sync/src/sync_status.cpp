#include <mediaserver/sync/sync_status.hpp>

namespace mediaserver::sync {

std::string_view
sync_device_availability_name(SyncDeviceAvailability value) noexcept {
    switch (value) {
    case SyncDeviceAvailability::Unknown:
        return "UNKNOWN";
    case SyncDeviceAvailability::Unsupported:
        return "UNSUPPORTED";
    case SyncDeviceAvailability::Absent:
        return "ABSENT";
    case SyncDeviceAvailability::Present:
        return "PRESENT";
    case SyncDeviceAvailability::Failed:
        return "FAILED";
    }
    return "UNKNOWN";
}

std::string_view sync_reference_state_name(SyncReferenceState value) noexcept {
    switch (value) {
    case SyncReferenceState::Unknown:
        return "UNKNOWN";
    case SyncReferenceState::Absent:
        return "ABSENT";
    case SyncReferenceState::Present:
        return "PRESENT";
    case SyncReferenceState::InUse:
        return "IN USE";
    case SyncReferenceState::Unstable:
        return "UNSTABLE";
    case SyncReferenceState::Unsupported:
        return "UNSUPPORTED";
    }
    return "UNKNOWN";
}

std::string_view sync_lock_state_name(SyncLockState value) noexcept {
    switch (value) {
    case SyncLockState::Unknown:
        return "UNKNOWN";
    case SyncLockState::Unlocked:
        return "UNLOCKED";
    case SyncLockState::Locked:
        return "LOCKED";
    case SyncLockState::Unsupported:
        return "UNSUPPORTED";
    }
    return "UNKNOWN";
}

std::string_view present_barrier_state_name(PresentBarrierState value)
    noexcept {
    switch (value) {
    case PresentBarrierState::Unknown:
        return "UNKNOWN";
    case PresentBarrierState::Unsupported:
        return "UNSUPPORTED";
    case PresentBarrierState::NotJoined:
        return "NOT JOINED";
    case PresentBarrierState::Joining:
        return "JOINING";
    case PresentBarrierState::Joined:
        return "JOINED";
    case PresentBarrierState::Left:
        return "LEFT";
    case PresentBarrierState::Failed:
        return "FAILED";
    }
    return "UNKNOWN";
}

core::u32 NodeSyncStatus::locked_outputs() const noexcept {
    core::u32 total{};
    for (const SyncOutputStatus& output : outputs) {
        if (output.lock == SyncLockState::Locked) {
            ++total;
        }
    }
    return total;
}

core::u32 NodeSyncStatus::unlocked_outputs() const noexcept {
    core::u32 total{};
    for (const SyncOutputStatus& output : outputs) {
        if (output.lock != SyncLockState::Locked) {
            ++total;
        }
    }
    return total;
}

const NodeSyncStatus* SyncStatusSnapshot::node(core::u32 node_index)
    const noexcept {
    for (const NodeSyncStatus& candidate : nodes) {
        if (candidate.node_index == node_index) {
            return &candidate;
        }
    }
    return nullptr;
}

} // namespace mediaserver::sync
