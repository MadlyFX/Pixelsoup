#pragma once

// Hardware Map command authoring: devices, links, properties, sync policy
// and output bindings.

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/mapping_panels.hpp>
#include <mediaserver/topology/hardware_map.hpp>

#include <string_view>

namespace mediaserver::app {

[[nodiscard]] std::string_view
hardware_kind_property(topology::HardwareDeviceKind kind) noexcept;

[[nodiscard]] doc::Node
make_hardware_device(doc::NodeId id,
                     const editor::HardwarePlaceRequest &request,
                     core::u32 node_index);

[[nodiscard]] bool
schedule_hardware_place(cmd::DirectorScheduler &scheduler,
                        const editor::HardwarePlaceRequest &request,
                        doc::NodeId id, cmd::FrameNumber apply_at);

[[nodiscard]] bool
schedule_hardware_move(cmd::DirectorScheduler &scheduler,
                       const editor::HardwareMoveRequest &request,
                       cmd::FrameNumber apply_at);

[[nodiscard]] bool
schedule_hardware_link(cmd::DirectorScheduler &scheduler,
                       const editor::HardwareLinkRequest &request,
                       doc::NodeId id, cmd::FrameNumber apply_at);

[[nodiscard]] bool
schedule_hardware_property(cmd::DirectorScheduler &scheduler,
                           const editor::HardwarePropertyEdit &edit,
                           cmd::FrameNumber apply_at);

[[nodiscard]] bool
schedule_hardware_control_port(cmd::DirectorScheduler &scheduler,
                               const editor::HardwareControlPortEdit &edit,
                               doc::NodeId allocated_id,
                               cmd::FrameNumber apply_at);

[[nodiscard]] bool
schedule_sync_policy(cmd::DirectorScheduler &scheduler,
                     const editor::HardwareSyncPolicyEdit &edit,
                     doc::NodeId allocated_id, cmd::FrameNumber apply_at);

[[nodiscard]] std::string_view
hardware_window_mode_property(topology::HardwareWindowMode mode) noexcept;

[[nodiscard]] doc::Node
make_hardware_output_binding(doc::NodeId id,
                             const topology::HardwareOutputBinding &binding);

[[nodiscard]] bool
schedule_hardware_output_binding(cmd::DirectorScheduler &scheduler,
                                 const editor::HardwareOutputBindingEdit &edit,
                                 doc::NodeId allocated_id,
                                 cmd::FrameNumber apply_at);

[[nodiscard]] bool schedule_hardware_output_binding_delete(
    cmd::DirectorScheduler &scheduler,
    const editor::HardwareOutputBindingDelete &request,
    cmd::FrameNumber apply_at);

[[nodiscard]] bool
schedule_hardware_delete(cmd::DirectorScheduler &scheduler,
                         const editor::HardwareDeleteRequest &request,
                         cmd::FrameNumber apply_at);

} // namespace mediaserver::app
