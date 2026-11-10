#include "output_deployment.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace mediaserver::app {
namespace {

[[nodiscard]] OutputDeploymentIssue issue(
    OutputDeploymentIssueCode code, doc::NodeId binding,
    topology::HardwareDisplayTargetKey target, std::string message) {
    return OutputDeploymentIssue{code, binding, target, std::move(message)};
}

// A desktop area is only comparable once the source mode is known. A probe
// that could not read one reports zero extent, and a zero extent matches
// nothing rather than matching everything.
[[nodiscard]] bool same_desktop_area(
    const net::ClusterDisplayTargetInfo& left,
    const net::ClusterDisplayTargetInfo& right) noexcept {
    return left.desktop_width != 0U && left.desktop_height != 0U &&
        left.desktop_x == right.desktop_x &&
        left.desktop_y == right.desktop_y &&
        left.desktop_width == right.desktop_width &&
        left.desktop_height == right.desktop_height;
}

[[nodiscard]] bool mirrors_another_active_target(
    const net::ClusterMachineInfo& inventory,
    const net::ClusterDisplayTargetInfo& target) noexcept {
    for (const auto& candidate : inventory.display_targets) {
        if (!candidate.active) {
            continue;
        }
        if (candidate.adapter_id == target.adapter_id &&
            candidate.target_id == target.target_id &&
            candidate.connector_instance == target.connector_instance) {
            continue;
        }
        if (same_desktop_area(candidate, target)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool valid_mode(
    const topology::HardwareOutputBinding& binding) noexcept {
    return binding.width > 0U && binding.height > 0U &&
        binding.width <= topology::max_hardware_output_width &&
        binding.height <= topology::max_hardware_output_height &&
        binding.refresh_numerator > 0U &&
        binding.refresh_denominator > 0U;
}

} // namespace

bool display_target_matches(
    const net::ClusterDisplayTargetInfo& observed,
    const topology::HardwareDisplayTargetKey& authored) noexcept {
    return observed.adapter_id == authored.adapter_id &&
        observed.target_id == authored.target_id &&
        observed.connector_instance == authored.connector_instance;
}

net::RuntimeOutputDeploymentState
runtime_output_deployment_state(const OutputDeployment& deployment) noexcept {
    using State = net::RuntimeOutputDeploymentState;
    if (deployment.issues.empty()) {
        return State::Ready;
    }
    switch (deployment.issues.front().code) {
    case OutputDeploymentIssueCode::NoBoundDisplay:
        return State::NoBoundDisplay;
    case OutputDeploymentIssueCode::DuplicateRenderServer:
        return State::DuplicateRenderServer;
    case OutputDeploymentIssueCode::InventoryUnavailable:
        return State::InventoryUnavailable;
    case OutputDeploymentIssueCode::MissingDisplayTarget:
        return State::MissingDisplayTarget;
    case OutputDeploymentIssueCode::DuplicateDisplayTarget:
        return State::DuplicateDisplayTarget;
    case OutputDeploymentIssueCode::InactiveDisplayTarget:
        return State::InactiveDisplayTarget;
    case OutputDeploymentIssueCode::InvalidOutputMode:
        return State::InvalidOutputMode;
    case OutputDeploymentIssueCode::MirroredDisplayTarget:
        return State::MirroredDisplayTarget;
    }
    return State::Unknown;
}

OutputDeployment resolve_output_deployment(
    const PlaybackOptions& options,
    const topology::HardwareMap& hardware,
    const net::ClusterMachineInfo* local_inventory) {
    OutputDeployment result{};
    result.node_index = options.node_index;

    // Only a cabled card owns a node index. A parked alternative with the
    // same index is ignored rather than making this output ambiguous.
    const topology::RenderServerLookup owner =
        topology::render_server_for_node(hardware, options.node_index);
    if (owner.duplicate) {
        result.issues.push_back(issue(
            OutputDeploymentIssueCode::DuplicateRenderServer, {}, {},
            "more than one Render Server owns this process node index"));
        return result;
    }
    const topology::HardwareDevice* const server = owner.device;
    if (server == nullptr) {
        // No card owns this node index: a command-line launch, drawn in the
        // window its arguments describe and fed by its own node index.
        result.outputs.push_back(DeployedOutput{
            {}, topology::first_render_server_video_port,
            options.node_index, {}, presenter_window_settings(options, {}),
            "launch output"});
        return result;
    }
    result.render_server = server->node;

    std::vector<const topology::HardwareOutputBinding*> bindings;
    for (const auto& binding : hardware.output_bindings) {
        if (binding.render_server == server->node) {
            bindings.push_back(&binding);
        }
    }
    if (bindings.empty()) {
        result.issues.push_back(issue(
            OutputDeploymentIssueCode::NoBoundDisplay, {}, {},
            "this Render Server has no display bound to an output"));
        return result;
    }

    using Availability = net::ClusterMachineInfoAvailability;
    if (local_inventory == nullptr ||
        (local_inventory->availability != Availability::Available &&
         local_inventory->availability != Availability::Partial) ||
        local_inventory->display_state !=
            net::ClusterDisplayEnumerationState::Available) {
        result.issues.push_back(issue(
            OutputDeploymentIssueCode::InventoryUnavailable, {}, {},
            "active display inventory is unavailable on this render machine"));
        return result;
    }

    std::set<topology::HardwareDisplayTargetKey> observed_once;
    for (const topology::HardwareOutputBinding* const binding : bindings) {
        if (!valid_mode(*binding)) {
            result.issues.push_back(issue(
                OutputDeploymentIssueCode::InvalidOutputMode, binding->node,
                binding->target,
                "saved output mode is outside presenter limits"));
            continue;
        }
        const net::ClusterDisplayTargetInfo* target{};
        bool duplicate{};
        for (const auto& candidate : local_inventory->display_targets) {
            if (!display_target_matches(candidate, binding->target)) {
                continue;
            }
            if (target != nullptr) {
                duplicate = true;
                break;
            }
            target = &candidate;
        }
        if (duplicate || !observed_once.insert(binding->target).second) {
            result.issues.push_back(issue(
                OutputDeploymentIssueCode::DuplicateDisplayTarget,
                binding->node, binding->target,
                "display target identity is duplicated"));
            continue;
        }
        if (target == nullptr || !target->available) {
            result.issues.push_back(issue(
                OutputDeploymentIssueCode::MissingDisplayTarget,
                binding->node, binding->target,
                "saved display target is not present"));
            continue;
        }
        if (!target->active) {
            result.issues.push_back(issue(
                OutputDeploymentIssueCode::InactiveDisplayTarget,
                binding->node, binding->target,
                "saved display target is present but not active"));
            continue;
        }
        // Two active targets occupying one desktop area are mirrored. Driving
        // either of them separately is not possible, so say so rather than
        // opening a window that lands on the other one.
        if (mirrors_another_active_target(*local_inventory, *target)) {
            result.issues.push_back(issue(
                OutputDeploymentIssueCode::MirroredDisplayTarget,
                binding->node, binding->target,
                "saved display target mirrors another display on this "
                "machine; extend the desktop to drive it on its own"));
            continue;
        }

        PresenterWindowSettings window{};
        window.width = binding->width;
        window.height = binding->height;
        window.position_x = target->desktop_x;
        window.position_y = target->desktop_y;
        window.position_explicit = true;
        window.display_bounds_x = target->desktop_x;
        window.display_bounds_y = target->desktop_y;
        window.display_bounds_width = target->desktop_width;
        window.display_bounds_height = target->desktop_height;
        window.windowed = true;
        window.fullscreen =
            binding->window_mode == topology::HardwareWindowMode::Fullscreen;
        window.borderless =
            binding->window_mode == topology::HardwareWindowMode::Borderless;
        window.refresh_numerator = binding->refresh_numerator;
        window.refresh_denominator = binding->refresh_denominator;
        window.require_exact_display_mode = window.fullscreen;
        window.output_bit_depth = binding->output_bit_depth;
        result.outputs.push_back(DeployedOutput{
            binding->node, binding->logical_port, binding->feed_index,
            binding->target, window,
            target->name.empty()
                ? "display " + std::to_string(target->target_id)
                : target->name});
    }
    return result;
}

} // namespace mediaserver::app
