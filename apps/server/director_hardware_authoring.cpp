#include "director_hardware_authoring.hpp"

#include "director_compositor_authoring.hpp"
#include "director_feed_map_authoring.hpp"

#include <mediaserver/core/output_limits.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/sync/sync_policy.hpp>
#include <mediaserver/sync/sync_policy_model.hpp>
#include <mediaserver/sync/sync_schema.hpp>
#include <mediaserver/topology/hardware_map_model.hpp>
#include <mediaserver/topology/hardware_map_schema.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace mediaserver::app {

[[nodiscard]] std::string_view
hardware_kind_property(topology::HardwareDeviceKind kind) noexcept {
    switch (kind) {
    case topology::HardwareDeviceKind::RenderServer:
        return topology::schema::render_server_kind;
    case topology::HardwareDeviceKind::OperatorMachine:
        return topology::schema::operator_machine_kind;
    case topology::HardwareDeviceKind::LedProcessor:
        return topology::schema::led_processor_kind;
    case topology::HardwareDeviceKind::LedWall:
        return topology::schema::led_wall_kind;
    }
    return topology::schema::render_server_kind;
}

[[nodiscard]] doc::Node
make_hardware_device(doc::NodeId id,
                     const editor::HardwarePlaceRequest &request,
                     core::u32 node_index) {
    doc::Node node{};
    node.id = id;
    node.type = std::string{topology::schema::device_type};
    node.properties.emplace(
        std::string{topology::schema::kind},
        doc::PropertyValue{std::string{hardware_kind_property(request.kind)}});
    // A machine found on the network names the card after itself.
    std::string name{topology::device_kind_name(request.kind)};
    if (request.machine.has_value() && !request.machine->name.empty()) {
        name = request.machine->name;
    }
    node.properties.emplace(std::string{topology::schema::name},
                            doc::PropertyValue{std::move(name)});
    node.properties.emplace(
        std::string{topology::schema::grid_x},
        doc::PropertyValue{static_cast<std::int64_t>(request.grid_x)});
    node.properties.emplace(
        std::string{topology::schema::grid_y},
        doc::PropertyValue{static_cast<std::int64_t>(request.grid_y)});
    node.properties.emplace(
        std::string{topology::schema::address},
        doc::PropertyValue{request.machine.has_value()
                               ? request.machine->address
                               : std::string{}});
    if (request.kind == topology::HardwareDeviceKind::RenderServer) {
        node.properties.emplace(
            std::string{topology::schema::lifecycle_endpoint},
            doc::PropertyValue{std::string{}});
        node.properties.emplace(
            std::string{topology::schema::node_index},
            doc::PropertyValue{static_cast<std::int64_t>(node_index)});
    }
    node.properties.emplace(std::string{topology::schema::width},
                            doc::PropertyValue{std::int64_t{1920}});
    node.properties.emplace(std::string{topology::schema::height},
                            doc::PropertyValue{std::int64_t{1080}});
    node.properties.emplace(std::string{topology::schema::refresh_numerator},
                            doc::PropertyValue{std::int64_t{60}});
    node.properties.emplace(std::string{topology::schema::refresh_denominator},
                            doc::PropertyValue{std::int64_t{1}});
    if (request.kind == topology::HardwareDeviceKind::RenderServer) {
        node.properties.emplace(std::string{topology::schema::window_mode},
                                doc::PropertyValue{std::string{
                                    topology::schema::inherit_window_mode}});
        node.properties.emplace(std::string{topology::schema::desktop_x},
                                doc::PropertyValue{std::int64_t{0}});
        node.properties.emplace(std::string{topology::schema::desktop_y},
                                doc::PropertyValue{std::int64_t{0}});
    }
    return node;
}

[[nodiscard]] bool
schedule_hardware_place(cmd::DirectorScheduler &scheduler,
                        const editor::HardwarePlaceRequest &request,
                        doc::NodeId id, cmd::FrameNumber apply_at) {
    // A second Render Server card starts on a free node index, so placing an
    // alternative configuration never collides with the one in use.
    // A machine found on the network keeps the node index its agent was
    // started with: that is the identity it will answer to.
    topology::HardwareMapModel model;
    model.reset(scheduler.projected_document());
    const topology::HardwareMap map = model.evaluate();
    const core::u32 node_index =
        request.machine.has_value() ? request.machine->node_index
                                    : topology::next_free_node_index(map);
    doc::Node device = make_hardware_device(id, request, node_index);
    // The show's first Render Server is its main node, so the Timeline has a
    // node playhead without a trip to the checkbox. Later cards start off.
    if (request.kind == topology::HardwareDeviceKind::RenderServer &&
        std::none_of(map.devices.begin(), map.devices.end(),
                     [](const topology::HardwareDevice &existing) noexcept {
                         return existing.kind ==
                                topology::HardwareDeviceKind::RenderServer;
                     })) {
        device.properties.insert_or_assign(
            std::string{topology::schema::main_node}, doc::PropertyValue{true});
    }
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(device)}, cmd::EraseNode{id},
        cmd::UndoContextId{3U}, cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] bool
schedule_hardware_move(cmd::DirectorScheduler &scheduler,
                       const editor::HardwareMoveRequest &request,
                       cmd::FrameNumber apply_at) {
    const doc::Node *const existing =
        scheduler.projected_document().get(request.device);
    if (existing == nullptr ||
        existing->type != topology::schema::device_type) {
        return true;
    }
    doc::Node updated = *existing;
    updated.properties.insert_or_assign(
        std::string{topology::schema::grid_x},
        doc::PropertyValue{static_cast<std::int64_t>(request.grid_x)});
    updated.properties.insert_or_assign(
        std::string{topology::schema::grid_y},
        doc::PropertyValue{static_cast<std::int64_t>(request.grid_y)});
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, cmd::PutNode{*existing},
        cmd::UndoContextId{3U}, cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] bool
schedule_hardware_link(cmd::DirectorScheduler &scheduler,
                       const editor::HardwareLinkRequest &request,
                       doc::NodeId id, cmd::FrameNumber apply_at) {
    constexpr core::u64 max_document_node =
        static_cast<core::u64>(std::numeric_limits<std::int64_t>::max());
    if (request.from.device.value() > max_document_node ||
        request.to.device.value() > max_document_node) {
        return true;
    }
    if (request.output_binding.has_value() &&
        (request.output_binding->render_server != request.from.device ||
         request.output_binding->logical_port != request.from.port ||
         request.output_region.width == 0U ||
         request.output_region.height == 0U)) {
        return true;
    }
    doc::Node node{};
    node.id = id;
    node.type = std::string{topology::schema::link_type};
    node.properties.emplace(std::string{topology::schema::from_device},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                request.from.device.value())});
    node.properties.emplace(
        std::string{topology::schema::from_port},
        doc::PropertyValue{static_cast<std::int64_t>(request.from.port)});
    node.properties.emplace(std::string{topology::schema::to_device},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                request.to.device.value())});
    node.properties.emplace(
        std::string{topology::schema::to_port},
        doc::PropertyValue{static_cast<std::int64_t>(request.to.port)});
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(node)}, cmd::EraseNode{id},
        cmd::UndoContextId{3U}, cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] bool
schedule_hardware_property(cmd::DirectorScheduler &scheduler,
                           const editor::HardwarePropertyEdit &edit,
                           cmd::FrameNumber apply_at) {
    const bool allowed =
        edit.property == topology::schema::name ||
        edit.property == topology::schema::address ||
        edit.property == topology::schema::lifecycle_endpoint ||
        edit.property == topology::schema::node_index ||
        edit.property == topology::schema::width ||
        edit.property == topology::schema::height ||
        edit.property == topology::schema::refresh_numerator ||
        edit.property == topology::schema::refresh_denominator ||
        edit.property == topology::schema::window_mode ||
        edit.property == topology::schema::desktop_x ||
        edit.property == topology::schema::desktop_y ||
        edit.property == topology::schema::main_node;
    const doc::Node *const existing =
        scheduler.projected_document().get(edit.device);
    if (!allowed || existing == nullptr ||
        existing->type != topology::schema::device_type) {
        return true;
    }
    const bool lifecycle_property =
        edit.property == topology::schema::lifecycle_endpoint ||
        edit.property == topology::schema::node_index ||
        edit.property == topology::schema::window_mode ||
        edit.property == topology::schema::desktop_x ||
        edit.property == topology::schema::desktop_y ||
        edit.property == topology::schema::main_node;
    const auto kind =
        existing->properties.find(std::string{topology::schema::kind});
    const auto *const kind_text = kind != existing->properties.end()
                                      ? std::get_if<std::string>(&kind->second)
                                      : nullptr;
    if (lifecycle_property &&
        (kind_text == nullptr ||
         *kind_text != topology::schema::render_server_kind)) {
        return true;
    }
    if (edit.property == topology::schema::main_node &&
        std::get_if<bool>(&edit.value) == nullptr) {
        return true;
    }
    // One main node per show: choosing one clears every other in the same
    // transaction, so a single undo puts the previous choice back.
    if (edit.property == topology::schema::main_node &&
        std::get<bool>(edit.value)) {
        const doc::Delta contents =
            doc::diff(doc::Root{}, scheduler.projected_document());
        for (const doc::NodeChange &change : contents.changes) {
            const doc::Node *const other = change.after.get();
            if (other == nullptr || other->id == edit.device ||
                other->type != topology::schema::device_type) {
                continue;
            }
            const auto flag = other->properties.find(
                std::string{topology::schema::main_node});
            if (flag == other->properties.end() ||
                std::get_if<bool>(&flag->second) == nullptr ||
                !std::get<bool>(flag->second)) {
                continue;
            }
            doc::Node cleared = *other;
            cleared.properties.insert_or_assign(
                std::string{topology::schema::main_node},
                doc::PropertyValue{false});
            if (!scheduler.schedule_edit(
                    cmd::PutNode{std::move(cleared)}, cmd::PutNode{*other},
                    cmd::UndoContextId{3U}, cmd::TransactionId{edit.gesture},
                    apply_at)) {
                return false;
            }
        }
    }
    doc::Node updated = *existing;
    updated.properties.insert_or_assign(edit.property, edit.value);
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, cmd::PutNode{*existing},
        cmd::UndoContextId{3U}, cmd::TransactionId{edit.gesture}, apply_at));
}

[[nodiscard]] bool
schedule_hardware_control_port(cmd::DirectorScheduler &scheduler,
                               const editor::HardwareControlPortEdit &edit,
                               doc::NodeId allocated_id,
                               cmd::FrameNumber apply_at) {
    if (edit.base_port == 0U ||
        edit.base_port > topology::max_hardware_control_base_port) {
        return true;
    }

    const doc::Root &root = scheduler.projected_document();
    const doc::Node *existing{};
    if (edit.settings != doc::NodeId{}) {
        existing = root.get(edit.settings);
        // A stale request from a replaced project must not create a second
        // settings object or edit a node whose identity was reused.
        if (existing == nullptr ||
            existing->type != topology::schema::settings_type) {
            return true;
        }
    } else {
        // The panel asks to create when its evaluated map has no provenance.
        // Re-check the authoritative projected document at the authoring
        // boundary and update the lowest-id settings node if one arrived in
        // the meantime, preserving the model's deterministic winner.
        const doc::Delta contents = doc::diff(doc::Root{}, root);
        for (const doc::NodeChange &change : contents.changes) {
            if (!change.after ||
                change.after->type != topology::schema::settings_type) {
                continue;
            }
            if (existing == nullptr || change.after->id < existing->id) {
                existing = change.after.get();
            }
        }
    }

    doc::Node updated{};
    updated.id = existing != nullptr ? existing->id : allocated_id;
    updated.type = std::string{topology::schema::settings_type};
    if (existing != nullptr) {
        updated = *existing;
    }
    updated.properties.insert_or_assign(
        std::string{topology::schema::control_base_port},
        doc::PropertyValue{static_cast<std::int64_t>(edit.base_port)});
    cmd::Payload inverse = existing != nullptr
                               ? cmd::Payload{cmd::PutNode{*existing}}
                               : cmd::Payload{cmd::EraseNode{allocated_id}};
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, std::move(inverse),
        cmd::UndoContextId{3U}, cmd::TransactionId{edit.gesture}, apply_at));
}

// The canvas is a per-scene property, but it is authored once for the whole
// wall: every stage is written in the same transaction, so one undo restores
// them all and a Program transition never meets two different canvases.
[[nodiscard]] bool
schedule_hardware_canvas(cmd::DirectorScheduler &scheduler,
                         const editor::HardwareCanvasEdit &edit,
                         cmd::FrameNumber apply_at) {
    if (!scene::valid(edit.canvas) ||
        edit.canvas.width > core::max_output_dimension ||
        edit.canvas.height > core::max_output_dimension) {
        return true;
    }
    std::vector<doc::Node> stages;
    const doc::Delta contents =
        doc::diff(doc::Root{}, scheduler.projected_document());
    for (const doc::NodeChange &change : contents.changes) {
        if (change.after &&
            change.after->type == scene::schema::stage_type) {
            stages.push_back(*change.after);
        }
    }
    for (const doc::Node &existing : stages) {
        doc::Node updated = existing;
        updated.properties.insert_or_assign(
            std::string{scene::schema::canvas_width},
            doc::PropertyValue{static_cast<std::int64_t>(edit.canvas.width)});
        updated.properties.insert_or_assign(
            std::string{scene::schema::canvas_height},
            doc::PropertyValue{
                static_cast<std::int64_t>(edit.canvas.height)});
        if (updated == existing) {
            continue;
        }
        if (!scheduler.schedule_edit(
                cmd::PutNode{std::move(updated)}, cmd::PutNode{existing},
                cmd::UndoContextId{3U}, cmd::TransactionId{edit.gesture},
                apply_at)) {
            return false;
        }
    }
    return true;
}

// Authors the project-wide synchronization policy exactly like every other
// document edit, so it replicates, journals, and undoes through the same
// paths. A zero policy provenance creates sync.policy; an existing node is
// replaced whole, which keeps a half-edited raster from ever being visible.
[[nodiscard]] bool
schedule_sync_policy(cmd::DirectorScheduler &scheduler,
                     const editor::HardwareSyncPolicyEdit &edit,
                     doc::NodeId allocated_id, cmd::FrameNumber apply_at) {
    if (!sync::valid_policy(edit.value)) {
        return true;
    }

    const doc::Root &root = scheduler.projected_document();
    const doc::Node *existing{};
    if (edit.policy != doc::NodeId{}) {
        existing = root.get(edit.policy);
        // A stale request from a replaced project must not create a second
        // policy object or edit a node whose identity was reused.
        if (existing == nullptr ||
            existing->type != sync::schema::policy_type) {
            return true;
        }
    } else {
        // Re-check the authoritative projected document at the authoring
        // boundary and update the lowest-id policy node if one arrived in the
        // meantime, preserving the model deterministic winner.
        const doc::Delta contents = doc::diff(doc::Root{}, root);
        for (const doc::NodeChange &change : contents.changes) {
            if (!change.after ||
                change.after->type != sync::schema::policy_type) {
                continue;
            }
            if (existing == nullptr || change.after->id < existing->id) {
                existing = change.after.get();
            }
        }
    }

    doc::Node updated{};
    updated.id = existing != nullptr ? existing->id : allocated_id;
    sync::write_sync_policy_properties(updated, edit.value);
    cmd::Payload inverse = existing != nullptr
                               ? cmd::Payload{cmd::PutNode{*existing}}
                               : cmd::Payload{cmd::EraseNode{allocated_id}};
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, std::move(inverse),
        cmd::UndoContextId{3U}, cmd::TransactionId{edit.gesture}, apply_at));
}

[[nodiscard]] std::string_view
hardware_window_mode_property(topology::HardwareWindowMode mode) noexcept {
    switch (mode) {
    case topology::HardwareWindowMode::Inherit:
        return topology::schema::inherit_window_mode;
    case topology::HardwareWindowMode::Windowed:
        return topology::schema::windowed_window_mode;
    case topology::HardwareWindowMode::Borderless:
        return topology::schema::borderless_window_mode;
    case topology::HardwareWindowMode::Fullscreen:
        return topology::schema::fullscreen_window_mode;
    }
    return topology::schema::fullscreen_window_mode;
}

[[nodiscard]] doc::Node
make_hardware_output_binding(doc::NodeId id,
                             const topology::HardwareOutputBinding &binding) {
    doc::Node node{};
    node.id = id;
    node.type = std::string{topology::schema::output_binding_type};
    node.properties.emplace(std::string{topology::schema::render_server},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                binding.render_server.value())});
    node.properties.emplace(
        std::string{topology::schema::logical_port},
        doc::PropertyValue{static_cast<std::int64_t>(binding.logical_port)});
    node.properties.emplace(
        std::string{topology::schema::feed_index},
        doc::PropertyValue{static_cast<std::int64_t>(binding.feed_index)});
    node.properties.emplace(
        std::string{topology::schema::display_adapter_id},
        doc::PropertyValue{
            topology::format_display_adapter_id(binding.target.adapter_id)});
    node.properties.emplace(std::string{topology::schema::display_target_id},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                binding.target.target_id)});
    node.properties.emplace(std::string{topology::schema::connector_instance},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                binding.target.connector_instance)});
    node.properties.emplace(
        std::string{topology::schema::width},
        doc::PropertyValue{static_cast<std::int64_t>(binding.width)});
    node.properties.emplace(
        std::string{topology::schema::height},
        doc::PropertyValue{static_cast<std::int64_t>(binding.height)});
    node.properties.emplace(std::string{topology::schema::refresh_numerator},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                binding.refresh_numerator)});
    node.properties.emplace(std::string{topology::schema::refresh_denominator},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                binding.refresh_denominator)});
    node.properties.emplace(std::string{topology::schema::output_bit_depth},
        doc::PropertyValue{static_cast<std::int64_t>(binding.output_bit_depth)});
    node.properties.emplace(
        std::string{topology::schema::window_mode},
        doc::PropertyValue{
            std::string{hardware_window_mode_property(binding.window_mode)}});
    return node;
}

[[nodiscard]] bool
schedule_hardware_output_binding(cmd::DirectorScheduler &scheduler,
                                 const editor::HardwareOutputBindingEdit &edit,
                                 doc::NodeId allocated_id,
                                 cmd::FrameNumber apply_at) {
    const topology::HardwareOutputBinding &binding = edit.binding;
    if (binding.render_server == doc::NodeId{} ||
        binding.logical_port < topology::first_render_server_video_port ||
        binding.logical_port > topology::last_render_server_video_port ||
        binding.feed_index > topology::max_hardware_node_index ||
        !binding.target.valid() || binding.width == 0U ||
        binding.width > topology::max_hardware_output_width ||
        binding.height == 0U ||
        binding.height > topology::max_hardware_output_height ||
        binding.refresh_numerator == 0U || binding.refresh_denominator == 0U ||
        binding.window_mode == topology::HardwareWindowMode::Inherit) {
        return true;
    }

    const doc::Root &root = scheduler.projected_document();
    topology::HardwareMapModel model;
    model.reset(root);
    const topology::HardwareMap map = model.evaluate();
    const topology::HardwareDevice *const server =
        map.device(binding.render_server);
    if (server == nullptr ||
        server->kind != topology::HardwareDeviceKind::RenderServer) {
        return true;
    }
    const doc::NodeId id =
        binding.node != doc::NodeId{} ? binding.node : allocated_id;
    const doc::Node *const existing = root.get(id);
    if (existing != nullptr &&
        existing->type != topology::schema::output_binding_type) {
        return true;
    }
    for (const topology::HardwareOutputBinding &other : map.output_bindings) {
        if (other.node == id || other.render_server != binding.render_server) {
            continue;
        }
        if (other.logical_port == binding.logical_port ||
            other.feed_index == binding.feed_index ||
            other.target == binding.target) {
            return true;
        }
    }
    cmd::Payload inverse = existing != nullptr
                               ? cmd::Payload{cmd::PutNode{*existing}}
                               : cmd::Payload{cmd::EraseNode{id}};
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{make_hardware_output_binding(id, binding)},
        std::move(inverse), cmd::UndoContextId{3U},
        cmd::TransactionId{edit.gesture}, apply_at));
}

[[nodiscard]] bool schedule_hardware_output_binding_delete(
    cmd::DirectorScheduler &scheduler,
    const editor::HardwareOutputBindingDelete &request,
    cmd::FrameNumber apply_at) {
    const doc::Node *const existing =
        scheduler.projected_document().get(request.binding);
    if (existing == nullptr ||
        existing->type != topology::schema::output_binding_type) {
        return true;
    }
    topology::HardwareMapModel model;
    model.reset(scheduler.projected_document());
    const topology::HardwareMap map = model.evaluate();
    const topology::HardwareOutputBinding *const binding =
        map.output_binding(request.binding);
    if (binding == nullptr) {
        return true;
    }
    const bool feed_is_shared =
        std::any_of(map.output_bindings.begin(), map.output_bindings.end(),
                    [&](const topology::HardwareOutputBinding &other) {
                        return other.node != binding->node &&
                               other.feed_index == binding->feed_index;
                    });
    if (!feed_is_shared) {
        for (const doc::Node *const feed : find_feed_map_nodes(
                 scheduler.projected_document(), binding->feed_index)) {
            if (!schedule_erase_node(
                    scheduler, feed->id, cmd::UndoContextId{3U},
                    cmd::TransactionId{request.gesture}, apply_at)) {
                return false;
            }
        }
    }
    return schedule_erase_node(scheduler, request.binding,
                               cmd::UndoContextId{3U},
                               cmd::TransactionId{request.gesture}, apply_at);
}

[[nodiscard]] bool
schedule_hardware_delete(cmd::DirectorScheduler &scheduler,
                         const editor::HardwareDeleteRequest &request,
                         cmd::FrameNumber apply_at) {
    if (request.target.kind == editor::HardwareSelectionKind::None ||
        request.target.node == doc::NodeId{}) {
        return true;
    }
    const doc::Root &root = scheduler.projected_document();
    const doc::Node *const target = root.get(request.target.node);
    if (target == nullptr) {
        return true;
    }
    const doc::NodeId target_id = target->id;
    const std::string target_type = target->type;
    const cmd::UndoContextId context{3U};
    const cmd::TransactionId transaction{request.gesture};
    if (request.target.kind == editor::HardwareSelectionKind::Link) {
        if (target_type != topology::schema::link_type) {
            return true;
        }
        topology::HardwareMapModel model;
        model.reset(root);
        const topology::HardwareMap map = model.evaluate();
        if (const topology::HardwareLink *const link = map.link(target_id)) {
            if (const topology::HardwareOutputBinding *const binding =
                    map.output_binding_for(link->from.device,
                                           link->from.port)) {
                if (!schedule_hardware_output_binding_delete(
                        scheduler,
                        editor::HardwareOutputBindingDelete{binding->node,
                                                            request.gesture},
                        apply_at)) {
                    return false;
                }
            }
        }
        return schedule_erase_node(scheduler, target_id, context, transaction,
                                   apply_at);
    }
    if (target_type != topology::schema::device_type) {
        return true;
    }

    try {
        std::vector<doc::NodeId> incident;
        std::vector<core::u32> feeds;
        topology::HardwareMapModel model;
        model.reset(root);
        const topology::HardwareMap hardware = model.evaluate();
        for (const topology::HardwareOutputBinding &binding :
             hardware.output_bindings) {
            if (binding.render_server == target_id) {
                feeds.push_back(binding.feed_index);
            }
        }
        const doc::Delta contents = doc::diff(doc::Root{}, root);
        for (const doc::NodeChange &change : contents.changes) {
            if (!change.after) {
                continue;
            }
            if (change.after->type == topology::schema::output_binding_type) {
                const auto server = node_reference(
                    *change.after, topology::schema::render_server);
                if (server && *server == target_id) {
                    incident.push_back(change.after->id);
                }
                continue;
            }
            if (change.after->type != topology::schema::link_type) {
                continue;
            }
            const auto from =
                node_reference(*change.after, topology::schema::from_device);
            const auto to =
                node_reference(*change.after, topology::schema::to_device);
            if ((from && *from == target_id) || (to && *to == target_id)) {
                incident.push_back(change.after->id);
            }
        }
        for (const core::u32 feed_index : feeds) {
            const bool shared = std::any_of(
                hardware.output_bindings.begin(),
                hardware.output_bindings.end(),
                [&](const topology::HardwareOutputBinding &binding) {
                    return binding.render_server != target_id &&
                           binding.feed_index == feed_index;
                });
            if (!shared) {
                for (const doc::Node *const feed : find_feed_map_nodes(
                         scheduler.projected_document(), feed_index)) {
                    if (!schedule_erase_node(scheduler, feed->id, context,
                                             transaction, apply_at)) {
                        return false;
                    }
                }
            }
        }
        for (const doc::NodeId link : incident) {
            if (!schedule_erase_node(scheduler, link, context, transaction,
                                     apply_at)) {
                return false;
            }
        }
        return schedule_erase_node(scheduler, target_id, context, transaction,
                                   apply_at);
    } catch (const std::bad_alloc &) {
        return false;
    }
}

} // namespace mediaserver::app
