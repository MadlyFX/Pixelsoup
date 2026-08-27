#include <mediaserver/sync/sync_group.hpp>

#include <algorithm>
#include <cstddef>
#include <map>
#include <utility>

namespace mediaserver::sync {
namespace {

using topology::HardwareDevice;
using topology::HardwareDeviceKind;
using topology::HardwareLink;
using topology::HardwareMap;
using topology::HardwareOutputBinding;
using topology::HardwareSignal;

[[nodiscard]] bool video_port(const HardwareDevice& device,
                              core::u8 port) noexcept {
    const topology::HardwarePort* const described =
        topology::find_port(device.kind, port);
    return described != nullptr && described->signal == HardwareSignal::Video;
}

// A disjoint set over the two kinds of thing a video cable can join: one
// render-server output socket, and one downstream device. Render servers are
// deliberately not whole-device elements, so two outputs of one machine
// driving two unrelated walls remain two synchronization groups.
class Sets final {
public:
    [[nodiscard]] std::size_t make() {
        parent_.push_back(parent_.size());
        return parent_.size() - 1U;
    }

    [[nodiscard]] std::size_t find(std::size_t element) noexcept {
        while (parent_[element] != element) {
            parent_[element] = parent_[parent_[element]];
            element = parent_[element];
        }
        return element;
    }

    void join(std::size_t left, std::size_t right) noexcept {
        const std::size_t left_root = find(left);
        const std::size_t right_root = find(right);
        if (left_root == right_root) {
            return;
        }
        // Lowest index wins so component representatives stay deterministic
        // across machines.
        if (left_root < right_root) {
            parent_[right_root] = left_root;
        } else {
            parent_[left_root] = right_root;
        }
    }

private:
    std::vector<std::size_t> parent_;
};

struct OutputKey final {
    doc::NodeId server{};
    core::u8 port{};

    auto operator<=>(const OutputKey&) const = default;
};

[[nodiscard]] SyncTiming device_timing(const HardwareDevice& device) noexcept {
    return SyncTiming{device.width, device.height, device.refresh_numerator,
                      device.refresh_denominator};
}

[[nodiscard]] SyncTiming binding_timing(const HardwareOutputBinding& binding)
    noexcept {
    return SyncTiming{binding.width, binding.height,
                      binding.refresh_numerator, binding.refresh_denominator};
}

} // namespace

std::string_view
sync_group_diagnostic_message(SyncGroupDiagnosticCode code) noexcept {
    switch (code) {
    case SyncGroupDiagnosticCode::RasterMismatch:
        return "output raster differs from the rest of its sync group";
    case SyncGroupDiagnosticCode::RefreshMismatch:
        return "output refresh rate differs from the rest of its sync group";
    case SyncGroupDiagnosticCode::UnboundOutput:
        return "cabled video output has no display binding to lock";
    case SyncGroupDiagnosticCode::UncabledBinding:
        return "output binding is not cabled to a processor or wall";
    case SyncGroupDiagnosticCode::DuplicateTarget:
        return "two outputs in one sync group name the same display target";
    case SyncGroupDiagnosticCode::PolicyTimingMismatch:
        return "sync group timing differs from the project sync policy";
    }
    return "sync group configuration error";
}

std::size_t SyncTopology::member_count() const noexcept {
    std::size_t total{};
    for (const SyncGroup& group : groups) {
        total += group.members.size();
    }
    return total;
}

const SyncGroup* SyncTopology::group_for(doc::NodeId render_server,
                                         core::u8 logical_port)
    const noexcept {
    for (const SyncGroup& group : groups) {
        for (const SyncGroupMember& member : group.members) {
            if (member.render_server == render_server &&
                member.logical_port == logical_port) {
                return &group;
            }
        }
    }
    return nullptr;
}

SyncTopology derive_sync_topology(const HardwareMap& map,
                                  const SyncPolicy& policy) {
    SyncTopology topology{};

    Sets sets;
    std::map<OutputKey, std::size_t> outputs;
    std::map<doc::NodeId, std::size_t> devices;

    const auto device_element = [&](doc::NodeId id) -> std::size_t {
        const auto found = devices.find(id);
        if (found != devices.end()) {
            return found->second;
        }
        const std::size_t element = sets.make();
        devices.emplace(id, element);
        return element;
    };
    const auto output_element = [&](OutputKey key) -> std::size_t {
        const auto found = outputs.find(key);
        if (found != outputs.end()) {
            return found->second;
        }
        const std::size_t element = sets.make();
        outputs.emplace(key, element);
        return element;
    };

    for (const HardwareLink& link : map.links) {
        const HardwareDevice* const from = map.device(link.from.device);
        const HardwareDevice* const to = map.device(link.to.device);
        if (from == nullptr || to == nullptr ||
            !video_port(*from, link.from.port) ||
            !video_port(*to, link.to.port)) {
            continue;
        }
        const std::size_t downstream = device_element(link.to.device);
        if (from->kind == HardwareDeviceKind::RenderServer) {
            sets.join(
                output_element(OutputKey{link.from.device, link.from.port}),
                downstream);
            continue;
        }
        sets.join(device_element(link.from.device), downstream);
    }

    // Bindings are indexed by socket so a member can carry its display
    // identity and exact authored timing.
    std::map<OutputKey, const HardwareOutputBinding*> bound;
    for (const HardwareOutputBinding& binding : map.output_bindings) {
        bound.emplace(OutputKey{binding.render_server, binding.logical_port},
                      &binding);
    }

    // Members per component root. The map already guarantees a deterministic
    // (render server, port) ordering, and the ordered containers preserve it.
    std::map<std::size_t, std::vector<SyncGroupMember>> by_root;
    for (const auto& [key, element] : outputs) {
        const HardwareDevice* const server = map.device(key.server);
        if (server == nullptr ||
            server->kind != HardwareDeviceKind::RenderServer) {
            continue;
        }
        SyncGroupMember member{};
        member.render_server = key.server;
        member.node_index = server->node_index;
        member.logical_port = key.port;
        member.cabled = true;
        const auto binding = bound.find(key);
        if (binding != bound.end()) {
            member.binding = binding->second->node;
            member.feed_index = binding->second->feed_index;
            member.target = binding->second->target;
            member.timing = binding_timing(*binding->second);
        } else {
            member.feed_index = server->node_index;
            member.timing = device_timing(*server);
        }
        by_root[sets.find(element)].push_back(std::move(member));
    }

    // Downstream devices per component, so the operator surface can name what
    // a group actually drives.
    std::map<std::size_t, std::vector<doc::NodeId>> downstream_by_root;
    for (const auto& [id, element] : devices) {
        downstream_by_root[sets.find(element)].push_back(id);
    }

    topology.groups.reserve(by_root.size());
    for (auto& [root, members] : by_root) {
        SyncGroup group{};
        group.members = std::move(members);
        const auto downstream = downstream_by_root.find(root);
        if (downstream != downstream_by_root.end()) {
            group.downstream = downstream->second;
        }
        topology.groups.push_back(std::move(group));
    }
    std::sort(topology.groups.begin(), topology.groups.end(),
              [](const SyncGroup& left, const SyncGroup& right) noexcept {
                  const SyncGroupMember& first = left.members.front();
                  const SyncGroupMember& second = right.members.front();
                  if (first.render_server != second.render_server) {
                      return first.render_server < second.render_server;
                  }
                  return first.logical_port < second.logical_port;
              });

    for (core::u32 index = 0U;
         index < static_cast<core::u32>(topology.groups.size()); ++index) {
        SyncGroup& group = topology.groups[index];
        group.index = index;
        group.timing = group.members.front().timing;
        for (const SyncGroupMember& member : group.members) {
            if (member.binding.value() == 0U) {
                topology.diagnostics.push_back(SyncGroupDiagnostic{
                    SyncGroupDiagnosticCode::UnboundOutput, index,
                    member.render_server, {}, member.logical_port});
            }
            if (!same_raster(member.timing, group.timing)) {
                group.timing_consistent = false;
                topology.diagnostics.push_back(SyncGroupDiagnostic{
                    SyncGroupDiagnosticCode::RasterMismatch, index,
                    member.render_server, member.binding,
                    member.logical_port});
            }
            if (!same_refresh(member.timing, group.timing)) {
                group.timing_consistent = false;
                topology.diagnostics.push_back(SyncGroupDiagnostic{
                    SyncGroupDiagnosticCode::RefreshMismatch, index,
                    member.render_server, member.binding,
                    member.logical_port});
            }
        }
        for (std::size_t later = 1U; later < group.members.size(); ++later) {
            const SyncGroupMember& member = group.members[later];
            if (!member.target.valid()) {
                continue;
            }
            for (std::size_t earlier = 0U; earlier < later; ++earlier) {
                if (group.members[earlier].target == member.target) {
                    topology.diagnostics.push_back(SyncGroupDiagnostic{
                        SyncGroupDiagnosticCode::DuplicateTarget, index,
                        member.render_server, member.binding,
                        member.logical_port});
                    break;
                }
            }
        }
        if (policy.requires_physical_sync() &&
            !same_timing(group.timing, policy.required_timing)) {
            topology.diagnostics.push_back(SyncGroupDiagnostic{
                SyncGroupDiagnosticCode::PolicyTimingMismatch, index,
                group.members.front().render_server,
                group.members.front().binding,
                group.members.front().logical_port});
        }
        for (const SyncGroupMember& member : group.members) {
            topology.node_indices.push_back(member.node_index);
        }
    }

    for (const HardwareOutputBinding& binding : map.output_bindings) {
        if (outputs.find(OutputKey{binding.render_server,
                                   binding.logical_port}) != outputs.end()) {
            continue;
        }
        topology.diagnostics.push_back(SyncGroupDiagnostic{
            SyncGroupDiagnosticCode::UncabledBinding, no_sync_group_index,
            binding.render_server, binding.node, binding.logical_port});
    }

    std::sort(topology.node_indices.begin(), topology.node_indices.end());
    topology.node_indices.erase(
        std::unique(topology.node_indices.begin(),
                    topology.node_indices.end()),
        topology.node_indices.end());
    return topology;
}

} // namespace mediaserver::sync
