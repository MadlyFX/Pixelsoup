#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/sync/sync_policy.hpp>
#include <mediaserver/topology/hardware_map.hpp>

#include <span>
#include <string_view>
#include <vector>

// Synchronization groups derived from the authored Hardware Map.
//
// A group is the set of render-server video outputs that reach a common
// downstream video device. Two outputs of one machine that drive two
// unrelated walls are therefore two groups, while two machines feeding one
// processor are one. Nothing here is measured: this answers "which outputs
// must present the same frame on the same raster boundary", and it validates
// that the operator gave them compatible timing.
namespace mediaserver::sync {

// One render-server video output participating in a group.
struct SyncGroupMember final {
    doc::NodeId render_server{};
    // Zero when the cable exists but no persistent output binding does. Such
    // a member is reported and blocks a strict policy: an unbound port has no
    // display target to lock.
    doc::NodeId binding{};
    core::u32 node_index{};
    core::u8 logical_port{topology::first_render_server_video_port};
    core::u32 feed_index{};
    topology::HardwareDisplayTargetKey target{};
    SyncTiming timing{};
    // The port carries an authored video cable to a downstream device.
    bool cabled{};

    bool operator==(const SyncGroupMember&) const = default;
};

enum class SyncGroupDiagnosticCode : core::u8 {
    // Raster differs from the group's required timing.
    RasterMismatch,
    // Rational refresh differs from the group's required timing. 60/1 against
    // 60000/1001 is the classic silently-drifting configuration.
    RefreshMismatch,
    // A cabled render-server video port has no persistent output binding, so
    // no display target can be locked or verified.
    UnboundOutput,
    // An authored output binding is not cabled to anything downstream, so it
    // cannot be assigned to a group from the Hardware Map.
    UncabledBinding,
    // Two members of one group name the same display target.
    DuplicateTarget,
    // The group's timing does not match the policy's required timing.
    PolicyTimingMismatch,
};

struct SyncGroupDiagnostic final {
    SyncGroupDiagnosticCode code{SyncGroupDiagnosticCode::RasterMismatch};
    // Zero-based index into SyncTopology::groups, or max when the diagnostic
    // belongs to no group (an uncabled binding).
    core::u32 group_index{};
    doc::NodeId render_server{};
    doc::NodeId binding{};
    core::u8 logical_port{};

    bool operator==(const SyncGroupDiagnostic&) const = default;
};

[[nodiscard]] std::string_view
sync_group_diagnostic_message(SyncGroupDiagnosticCode code) noexcept;

struct SyncGroup final {
    // Deterministic identity derived from the lowest-ordered member, so the
    // same project produces the same group numbering on every machine.
    core::u32 index{};
    // The downstream devices the group's outputs reach, in node-id order.
    std::vector<doc::NodeId> downstream;
    std::vector<SyncGroupMember> members;
    // The timing every member must present. Taken from the policy when it is
    // authored and valid, otherwise from the lowest-ordered member.
    SyncTiming timing{};
    bool timing_consistent{true};

    bool operator==(const SyncGroup&) const = default;
};

struct SyncTopology final {
    std::vector<SyncGroup> groups;
    std::vector<SyncGroupDiagnostic> diagnostics;
    // Configured render output identities that belong to some group, sorted
    // and unique. This is what a Program gate has to see acknowledged.
    std::vector<core::u32> node_indices;

    [[nodiscard]] bool valid() const noexcept { return diagnostics.empty(); }
    [[nodiscard]] std::size_t member_count() const noexcept;
    [[nodiscard]] const SyncGroup* group_for(doc::NodeId render_server,
                                             core::u8 logical_port)
        const noexcept;

    bool operator==(const SyncTopology&) const = default;
};

inline constexpr core::u32 no_sync_group_index = 0xFFFF'FFFFU;

// Derives groups and validates their timing. The result is a pure function of
// the authored map and policy: no runtime discovery participates, so two
// machines evaluating the same project agree exactly.
[[nodiscard]] SyncTopology derive_sync_topology(
    const topology::HardwareMap& map, const SyncPolicy& policy);

} // namespace mediaserver::sync
