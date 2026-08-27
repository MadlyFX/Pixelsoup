#include <mediaserver/sync/sync_readiness.hpp>

#include <algorithm>

namespace mediaserver::sync {
namespace {

[[nodiscard]] bool configuration_diagnostic(SyncGroupDiagnosticCode code)
    noexcept {
    switch (code) {
    case SyncGroupDiagnosticCode::RasterMismatch:
    case SyncGroupDiagnosticCode::RefreshMismatch:
    case SyncGroupDiagnosticCode::PolicyTimingMismatch:
    case SyncGroupDiagnosticCode::DuplicateTarget:
        return true;
    case SyncGroupDiagnosticCode::UnboundOutput:
    case SyncGroupDiagnosticCode::UncabledBinding:
        return false;
    }
    return true;
}

[[nodiscard]] const NodeSyncStatus* find_node(
    std::span<const NodeSyncStatus> nodes, core::u32 node_index) noexcept {
    for (const NodeSyncStatus& candidate : nodes) {
        if (candidate.node_index == node_index) {
            return &candidate;
        }
    }
    return nullptr;
}

[[nodiscard]] const SyncOutputStatus* find_output(
    const NodeSyncStatus& node,
    const topology::HardwareDisplayTargetKey& target) noexcept {
    if (!target.valid()) {
        return nullptr;
    }
    for (const SyncOutputStatus& candidate : node.outputs) {
        if (candidate.target == target) {
            return &candidate;
        }
    }
    return nullptr;
}

// The physical-timing verdict alone. It is deliberately independent of the
// upstream show-state gates: a timing line must be able to say what the
// hardware is doing even while Program is held for an unrelated reason.
[[nodiscard]] SyncReadinessCode evaluate_physical(
    const SyncPolicy& policy, const SyncTopology& topology,
    std::span<const NodeSyncStatus> nodes, SyncReadiness& result) noexcept {
    if (!policy.requires_physical_sync()) {
        return SyncReadinessCode::Unsynced;
    }
    if (topology.groups.empty()) {
        return SyncReadinessCode::NoConfiguredOutputs;
    }

    bool unbound = false;
    for (const SyncGroupDiagnostic& diagnostic : topology.diagnostics) {
        if (configuration_diagnostic(diagnostic.code)) {
            return SyncReadinessCode::TimingMismatch;
        }
        unbound = true;
    }
    if (unbound) {
        return SyncReadinessCode::OutputsUnbound;
    }

    bool any_reported = false;
    bool any_simulated = false;
    bool board_unknown = false;
    bool board_missing = false;
    bool reference_lost = false;
    bool reference_idle = false;
    bool barrier_unknown = false;
    bool barrier_incomplete = false;

    for (const core::u32 node_index : topology.node_indices) {
        const NodeSyncStatus* const node = find_node(nodes, node_index);
        if (node == nullptr ||
            (!node->machine_reported && !node->runtime_reported)) {
            board_unknown = true;
            barrier_unknown = true;
            continue;
        }
        any_reported = true;
        any_simulated = any_simulated || node->simulated;

        switch (node->board) {
        case SyncDeviceAvailability::Present:
            break;
        case SyncDeviceAvailability::Absent:
        case SyncDeviceAvailability::Failed:
            board_missing = true;
            break;
        case SyncDeviceAvailability::Unknown:
        case SyncDeviceAvailability::Unsupported:
            board_unknown = true;
            break;
        }

        if (policy.requires_external_reference()) {
            switch (node->reference) {
            case SyncReferenceState::InUse:
                break;
            case SyncReferenceState::Present:
                reference_idle = true;
                break;
            case SyncReferenceState::Absent:
            case SyncReferenceState::Unstable:
                reference_lost = true;
                break;
            case SyncReferenceState::Unknown:
            case SyncReferenceState::Unsupported:
                board_unknown = true;
                break;
            }
        }

        switch (node->barrier) {
        case PresentBarrierState::Joined:
            if (node->barrier_group != policy.group ||
                node->barrier_id != policy.barrier ||
                node->barrier_joined_outputs <
                    node->barrier_expected_outputs) {
                barrier_incomplete = true;
            } else {
                result.barrier_joined_outputs += node->barrier_joined_outputs;
            }
            break;
        case PresentBarrierState::Unsupported:
        case PresentBarrierState::NotJoined:
        case PresentBarrierState::Joining:
        case PresentBarrierState::Left:
        case PresentBarrierState::Failed:
            barrier_incomplete = true;
            break;
        case PresentBarrierState::Unknown:
            barrier_unknown = true;
            break;
        }
    }

    for (const SyncGroup& group : topology.groups) {
        for (const SyncGroupMember& member : group.members) {
            const NodeSyncStatus* const node =
                find_node(nodes, member.node_index);
            if (node == nullptr) {
                continue;
            }
            const SyncOutputStatus* const output =
                find_output(*node, member.target);
            if (output != nullptr && output->lock == SyncLockState::Locked) {
                ++result.locked_outputs;
            }
        }
    }

    // Ordered from the most fundamental missing fact to the most specific, so
    // the verdict names the first thing an operator has to fix.
    if (!any_reported) {
        return SyncReadinessCode::Unverified;
    }
    if (board_missing) {
        return SyncReadinessCode::SyncDeviceMissing;
    }
    if (board_unknown) {
        return SyncReadinessCode::Unverified;
    }
    if (reference_lost) {
        return SyncReadinessCode::ReferenceLost;
    }
    if (reference_idle) {
        return SyncReadinessCode::ReferenceNotInUse;
    }
    if (result.locked_outputs < result.expected_outputs) {
        return SyncReadinessCode::OutputsUnlocked;
    }
    if (barrier_unknown) {
        return SyncReadinessCode::Unverified;
    }
    if (barrier_incomplete) {
        return SyncReadinessCode::SwapBarrierIncomplete;
    }
    if (any_simulated) {
        // A simulated provider can describe the whole workflow but proves
        // nothing about photons. It never satisfies a strict policy.
        return SyncReadinessCode::Simulated;
    }
    return SyncReadinessCode::Ready;
}

} // namespace

std::string_view sync_readiness_headline(SyncReadinessCode code) noexcept {
    switch (code) {
    case SyncReadinessCode::Ready:
        return "SYNC VERIFIED";
    case SyncReadinessCode::Unsynced:
        return "SYNC POLICY UNSYNCED";
    case SyncReadinessCode::ShowFrameNotReady:
        return "SHOW FRAME NOT READY";
    case SyncReadinessCode::AssetsNotAdopted:
        return "ASSETS NOT ADOPTED";
    case SyncReadinessCode::RenderNodesMissing:
        return "RENDER NODES MISSING";
    case SyncReadinessCode::NoConfiguredOutputs:
        return "NO SYNCHRONIZED OUTPUTS CONFIGURED";
    case SyncReadinessCode::TimingMismatch:
        return "DISPLAY TIMING MISMATCH";
    case SyncReadinessCode::OutputsUnbound:
        return "SYNC GROUP OUTPUTS UNBOUND";
    case SyncReadinessCode::Unverified:
        return "SYNC UNVERIFIED";
    case SyncReadinessCode::SyncDeviceMissing:
        return "SYNC DEVICE MISSING";
    case SyncReadinessCode::ReferenceLost:
        return "REFERENCE LOST";
    case SyncReadinessCode::ReferenceNotInUse:
        return "REFERENCE NOT IN USE";
    case SyncReadinessCode::OutputsUnlocked:
        return "OUTPUTS UNLOCKED";
    case SyncReadinessCode::SwapBarrierIncomplete:
        return "SWAP BARRIER INCOMPLETE";
    case SyncReadinessCode::Simulated:
        return "SYNC SIMULATED";
    }
    return "SYNC UNVERIFIED";
}

SyncReadiness evaluate_sync_readiness(const SyncReadinessInput& input)
    noexcept {
    SyncReadiness result{};
    if (input.policy == nullptr || input.topology == nullptr) {
        result.code = SyncReadinessCode::Unverified;
        result.physical = SyncReadinessCode::Unverified;
        return result;
    }
    const SyncPolicy& policy = *input.policy;
    const SyncTopology& topology = *input.topology;
    result.groups = static_cast<core::u32>(topology.groups.size());
    result.expected_outputs = static_cast<core::u32>(topology.member_count());
    for (const NodeSyncStatus& node : input.nodes) {
        if (node.machine_reported || node.runtime_reported) {
            ++result.reporting_nodes;
        }
    }

    result.physical =
        evaluate_physical(policy, topology, input.nodes, result);
    const bool physical_proven = result.physical == SyncReadinessCode::Ready ||
                                 result.physical == SyncReadinessCode::Unsynced;
    // A physical-sync failure is the only thing the failure policy relaxes.
    const bool physical_satisfied =
        physical_proven || policy.failure == SyncFailurePolicy::Degraded;
    // Set before the upstream gates return: whether the hardware is locked is
    // a fact about the hardware, not about whether Program may be promoted.
    result.verified = result.physical == SyncReadinessCode::Ready;

    // The upstream show-state gates are never relaxed by the sync failure
    // policy. Degraded is a decision about unproven physical timing, not
    // permission to promote a frame the cluster has not adopted.
    if (!input.exact_show_frame_ready) {
        result.code = SyncReadinessCode::ShowFrameNotReady;
        return result;
    }
    if (!input.assets_adopted) {
        result.code = SyncReadinessCode::AssetsNotAdopted;
        return result;
    }
    if (!input.render_nodes_live) {
        result.code = SyncReadinessCode::RenderNodesMissing;
        return result;
    }

    result.code = result.physical;
    result.satisfied = physical_satisfied;
    result.degraded = physical_satisfied && !physical_proven;
    return result;
}

} // namespace mediaserver::sync
