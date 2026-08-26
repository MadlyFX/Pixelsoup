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

OutputDeployment resolve_output_deployment(
    const PlaybackOptions& options,
    const topology::HardwareMap& hardware,
    const net::ClusterMachineInfo* local_inventory) {
    OutputDeployment result{};
    result.node_index = options.node_index;

    const topology::HardwareDevice* server{};
    for (const topology::HardwareDevice& device : hardware.devices) {
        if (device.kind != topology::HardwareDeviceKind::RenderServer ||
            device.node_index != options.node_index) {
            continue;
        }
        if (server != nullptr) {
            result.issues.push_back(issue(
                OutputDeploymentIssueCode::DuplicateRenderServer, {}, {},
                "more than one Render Server owns this process node index"));
            return result;
        }
        server = &device;
    }
    if (server == nullptr) {
        // No authored server remains the explicit legacy launch contract.
        // A rejected/ambiguous document is handled by presenter_window_update
        // before this resolver is called.
        result.legacy_single_output = true;
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
        result.legacy_single_output = true;
        result.outputs.push_back(DeployedOutput{
            {}, topology::first_render_server_video_port,
            options.node_index, {},
            presenter_window_settings(options, hardware), server->name});
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

        PresenterWindowSettings window{};
        window.width = binding->width;
        window.height = binding->height;
        window.position_x = target->desktop_x;
        window.position_y = target->desktop_y;
        window.position_explicit = true;
        window.windowed = true;
        window.fullscreen =
            binding->window_mode == topology::HardwareWindowMode::Fullscreen;
        window.borderless =
            binding->window_mode == topology::HardwareWindowMode::Borderless;
        window.refresh_numerator = binding->refresh_numerator;
        window.refresh_denominator = binding->refresh_denominator;
        window.require_exact_display_mode = window.fullscreen;
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
