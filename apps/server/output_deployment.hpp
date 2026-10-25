#pragma once

#include "output_window_config.hpp"

#include <mediaserver/net/cluster_lifecycle.hpp>

#include <string>
#include <vector>

namespace mediaserver::app {

enum class OutputDeploymentIssueCode : core::u8 {
    MissingRenderServer,
    DuplicateRenderServer,
    InventoryUnavailable,
    MissingDisplayTarget,
    DuplicateDisplayTarget,
    InactiveDisplayTarget,
    InvalidOutputMode,
    // The saved target is active but shares its desktop area with another
    // active target: the machine is mirroring rather than extending. The two
    // are one desktop as far as any window is concerned, so this binding
    // cannot be driven on its own and an exclusive mode would be applied to
    // whichever display the toolkit picked. Refused with its own reason
    // instead, because "mirroring" is a thing the operator can go and change.
    MirroredDisplayTarget,
};

struct OutputDeploymentIssue final {
    OutputDeploymentIssueCode code{OutputDeploymentIssueCode::MissingRenderServer};
    doc::NodeId binding{};
    topology::HardwareDisplayTargetKey target;
    std::string message;

    bool operator==(const OutputDeploymentIssue&) const = default;
};

struct DeployedOutput final {
    doc::NodeId binding{};
    core::u8 logical_port{topology::first_render_server_video_port};
    core::u32 feed_index{};
    topology::HardwareDisplayTargetKey target;
    PresenterWindowSettings window;
    std::string display_name;

    bool operator==(const DeployedOutput&) const = default;
};

struct OutputDeployment final {
    doc::NodeId render_server{};
    core::u32 node_index{};
    // Legacy projects have no explicit bindings and retain one CLI/device
    // presenter. New projects contain one entry per real display target.
    bool legacy_single_output{};
    std::vector<DeployedOutput> outputs;
    std::vector<OutputDeploymentIssue> issues;

    [[nodiscard]] bool ready() const noexcept { return issues.empty(); }
    bool operator==(const OutputDeployment&) const = default;
};

[[nodiscard]] bool display_target_matches(
    const net::ClusterDisplayTargetInfo& observed,
    const topology::HardwareDisplayTargetKey& authored) noexcept;

// The reportable state for a resolved deployment: Ready, or the reason the
// first issue gives. The first issue is the one an operator fixes first -
// later ones are frequently consequences of it - and one reason that travels
// is worth more than a list that does not.
[[nodiscard]] net::RuntimeOutputDeploymentState
runtime_output_deployment_state(const OutputDeployment& deployment) noexcept;

// Resolves only the RenderServer whose process identity equals
// options.node_index. Active display discovery is an input, never project
// authoring; a missing target therefore produces a deterministic issue while
// leaving the saved binding untouched for repair.
[[nodiscard]] OutputDeployment resolve_output_deployment(
    const PlaybackOptions& options,
    const topology::HardwareMap& hardware,
    const net::ClusterMachineInfo* local_inventory);

} // namespace mediaserver::app
