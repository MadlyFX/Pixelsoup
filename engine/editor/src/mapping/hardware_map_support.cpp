#include "hardware_map_support.hpp"

#include <mediaserver/core/output_limits.hpp>
#include <mediaserver/editor/hardware_sprites.hpp>
#include <mediaserver/gui/shapes.hpp>
#include <mediaserver/topology/hardware_map_schema.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace mediaserver::editor::mapping::hardware {

HardwareLinkFeedback link_feedback_for(PortTargetState state) noexcept {
  switch (state) {
  case PortTargetState::Compatible:
    return HardwareLinkFeedback::Compatible;
  case PortTargetState::Replaces:
    return HardwareLinkFeedback::Replaces;
  case PortTargetState::Incompatible:
    return HardwareLinkFeedback::Incompatible;
  case PortTargetState::Occupied:
    return HardwareLinkFeedback::Occupied;
  case PortTargetState::None:
    break;
  }
  return HardwareLinkFeedback::None;
}

[[nodiscard]] std::size_t
palette_index(topology::HardwareDeviceKind kind,
              topology::HardwareProcessorProtocol protocol) noexcept {
  for (std::size_t index = 0U; index < palette_entries.size(); ++index) {
    if (palette_entries[index].kind == kind &&
        palette_entries[index].protocol == protocol) {
      return index;
    }
  }
  return 0U;
}

[[nodiscard]] bool
runtime_is_current(const topology::HardwareRuntimeNode *runtime) noexcept {
  return runtime != nullptr && !runtime->stale &&
         runtime->reachability == topology::HardwareRuntimeReachability::Online;
}

[[nodiscard]] bool runtime_inventory_is_reported(
    const topology::HardwareRuntimeNode *runtime) noexcept {
  if (runtime == nullptr) {
    return false;
  }
  return runtime->inventory ==
             topology::HardwareRuntimeInventoryState::Available ||
         runtime->inventory == topology::HardwareRuntimeInventoryState::Partial;
}

[[nodiscard]] bool runtime_inventory_is_usable(
    const topology::HardwareRuntimeNode *runtime) noexcept {
  if (runtime == nullptr) {
    return false;
  }
  if (runtime_is_current(runtime) && runtime_inventory_is_reported(runtime)) {
    return true;
  }
  // A disconnected node may retain its authenticated last-good inventory.
  // The stale bit is the explicit permission to present that data as historic.
  return runtime->stale && (runtime_inventory_is_reported(runtime) ||
                            !runtime->video_outputs.empty());
}

[[nodiscard]] HardwareDevicePresentation hardware_presentation(
    const topology::HardwareMap &map, const topology::HardwareDevice &device,
    const topology::HardwareRuntimeSnapshot &runtime_snapshot) noexcept {
  HardwareDevicePresentation presentation{};
  presentation.runtime = runtime_snapshot.node(device.node);
  presentation.parked =
      device.kind == topology::HardwareDeviceKind::RenderServer &&
      !topology::render_server_active(map, device);
  const auto append = [&](topology::HardwarePort port,
                          const topology::HardwareVideoOutput *output,
                          RuntimePortState runtime_state) {
    if (presentation.port_count >= presentation.ports.size()) {
      return;
    }
    presentation.ports[presentation.port_count++] =
        PresentedHardwarePort{port, output, runtime_state};
    if (port.signal == topology::HardwareSignal::Video &&
        port.direction == topology::HardwarePortDirection::Output) {
      ++presentation.video_port_count;
    }
  };

  if (device.kind == topology::HardwareDeviceKind::LedProcessor) {
    // LED OUT, then every input worth showing in number order: input one,
    // inputs a cable reaches, and inputs the processor reports, named as
    // the processor names them.
    const topology::HardwareProcessorNode *const observed =
        runtime_snapshot.processor(device.node);
    presentation.processor = observed;
    if (const topology::HardwarePort *const out = topology::find_port(
            device.kind, topology::processor_led_output_port)) {
      append(*out, nullptr, RuntimePortState::Unknown);
    }
    std::array<bool,
               static_cast<std::size_t>(topology::max_processor_video_inputs) +
                   1U>
        visible{};
    visible[1U] = true;
    for (const topology::HardwareLink &link : map.links) {
      if (link.to.device == device.node) {
        visible[topology::processor_input_number(link.to.port)] = true;
      }
    }
    const bool reporting =
        observed != nullptr && !observed->stale &&
        observed->reachability == topology::HardwareRuntimeReachability::Online;
    if (observed != nullptr) {
      for (const topology::HardwareProcessorInput &input : observed->inputs) {
        if (input.number >= 1U &&
            input.number <= topology::max_processor_video_inputs) {
          visible[input.number] = true;
        }
      }
    }
    for (core::u8 number = 1U; number <= topology::max_processor_video_inputs;
         ++number) {
      if (!visible[number]) {
        continue;
      }
      const topology::HardwarePort *const description = topology::find_port(
          device.kind, topology::processor_input_port(number));
      if (description == nullptr) {
        continue;
      }
      topology::HardwarePort shown = *description;
      const topology::HardwareProcessorInput *const reported =
          observed != nullptr ? observed->input(number) : nullptr;
      if (reported != nullptr && !reported->name.empty()) {
        shown.name = reported->name;
      }
      const RuntimePortState state =
          reported != nullptr
              ? (reported->signal ? RuntimePortState::Active
                                  : RuntimePortState::Unknown)
              : (reporting && observed->all_inputs
                     ? RuntimePortState::Missing
                           : RuntimePortState::Unknown);
      append(shown, nullptr, state);
      if (presentation.port_count != 0U) {
        presentation.ports[presentation.port_count - 1U].input = reported;
      }
    }
    return presentation;
  }

  if (device.kind != topology::HardwareDeviceKind::RenderServer) {
    for (const topology::HardwarePort &port :
         topology::ports_for(device.kind)) {
      append(port, nullptr, RuntimePortState::Unknown);
    }
    return presentation;
  }

  if (const topology::HardwarePort *const control =
          topology::find_port(device.kind, 0U)) {
    append(*control, nullptr, RuntimePortState::Unknown);
  }

  std::array<bool,
             static_cast<std::size_t>(topology::last_render_server_video_port) +
                 1U>
      visible{};
  std::array<const topology::HardwareVideoOutput *,
             static_cast<std::size_t>(topology::last_render_server_video_port) +
                 1U>
      observed{};
  std::array<bool,
             static_cast<std::size_t>(topology::last_render_server_video_port) +
                 1U>
      bound_slots{};
  // Port one is the legacy authored slot. It remains available for an offline
  // project even when this Director has never observed the machine.
  visible[topology::first_render_server_video_port] = true;

  // Persistent deployment bindings own their logical slot. Discovery is
  // reconciled by stable target identity below; it never compacts a later
  // connector into a missing binding's port.
  for (const topology::HardwareOutputBinding &binding :
       map.output_bindings) {
    if (binding.render_server == device.node &&
        binding.logical_port >= topology::first_render_server_video_port &&
        binding.logical_port <= topology::last_render_server_video_port) {
      visible[binding.logical_port] = true;
      bound_slots[binding.logical_port] = true;
    }
  }

  core::u8 next_slot = topology::first_render_server_video_port;
  if (runtime_inventory_is_usable(presentation.runtime)) {
    for (const topology::HardwareVideoOutput &output :
         presentation.runtime->video_outputs) {
      if (!output.active) {
        continue;
      }
      ++presentation.active_video_output_count;
      core::u8 assigned{};
      for (const topology::HardwareOutputBinding &binding :
           map.output_bindings) {
        if (binding.render_server == device.node &&
            binding.target == output.target) {
          assigned = binding.logical_port;
          break;
        }
      }
      if (assigned == 0U) {
        while (next_slot <= topology::last_render_server_video_port &&
               (bound_slots[next_slot] || observed[next_slot] != nullptr)) {
          ++next_slot;
        }
        if (next_slot > topology::last_render_server_video_port) {
          continue;
        }
        assigned = next_slot++;
      }
      visible[assigned] = true;
      observed[assigned] = &output;
    }
  }
  for (const topology::HardwareLink &link : map.links) {
    if (link.from.device == device.node &&
        link.from.port >= topology::first_render_server_video_port &&
        link.from.port <= topology::last_render_server_video_port) {
      visible[link.from.port] = true;
    }
  }

  for (core::u8 slot = topology::first_render_server_video_port;
       slot <= topology::last_render_server_video_port; ++slot) {
    if (!visible[slot]) {
      continue;
    }
    const topology::HardwarePort *const description =
        topology::find_port(device.kind, slot);
    if (description == nullptr) {
      continue;
    }
    const topology::HardwareVideoOutput *const output = observed[slot];
    RuntimePortState state = RuntimePortState::Unknown;
    if (output != nullptr) {
      state = runtime_is_current(presentation.runtime)
                  ? RuntimePortState::Active
                  : RuntimePortState::Stale;
    } else if (presentation.runtime != nullptr &&
               presentation.runtime->inventory ==
                   topology::HardwareRuntimeInventoryState::Available &&
               (runtime_is_current(presentation.runtime) ||
                presentation.runtime->stale)) {
      state = RuntimePortState::Missing;
    }
    topology::HardwarePort displayed = *description;
    if (output != nullptr) {
      if (!output->connector.empty()) {
        displayed.name = output->connector;
      } else if (!output->name.empty()) {
        displayed.name = output->name;
      }
    }
    append(displayed, output, state);
  }
  return presentation;
}

void circle(gui::UiContext &ctx, float x, float y, float radius,
            gui::UiColor color) {
  gui::add_disc(ctx.draw_list(), gui::UiPoint{x, y}, radius, color, 14U);
}

[[nodiscard]] float hardware_palette_width(const gui::Rect &bounds) noexcept {
  if (bounds.width <= 0.0F) {
    return 0.0F;
  }
  const float preferred = std::clamp(bounds.width * 0.22F, 150.0F, 210.0F);
  return std::min(preferred, bounds.width * 0.46F);
}

[[nodiscard]] float hardware_details_width(const gui::Rect &bounds) noexcept {
  if (bounds.width < 720.0F) {
    return 0.0F;
  }
  return std::min(std::clamp(bounds.width * 0.24F, 205.0F, 320.0F),
                  bounds.width * 0.30F);
}

[[nodiscard]] gui::Metrics palette_metrics() noexcept {
  gui::Metrics metrics{};
  metrics.inset = 9.0F;
  metrics.gap = 0.0F;
  metrics.control = details_row_height;
  metrics.row_gap = 0.0F;
  metrics.label_column = 77.0F;
  return metrics;
}

[[nodiscard]] PaletteLayout palette_layout(const gui::Rect &bounds) noexcept {
  PaletteLayout layout{};
  gui::Rect area = bounds;
  static_cast<void>(gui::cut_top(area, title_height));
  layout.column = gui::cut_left(area, hardware_palette_width(bounds));
  gui::Rect rest = layout.column;
  layout.header = gui::inset(gui::cut_top(rest, palette_header_height), 9.0F,
                             0.0F);
  layout.cards = gui::inset(
      gui::cut_top(rest, static_cast<float>(palette_entries.size()) *
                             palette_row_step),
      7.0F, 0.0F);
  // Rules run 2px wider than the rows on each side.
  const gui::Rect rule_span = gui::inset(layout.column, 7.0F, 0.0F);
  gui::FormLayout form{rest, palette_metrics()};
  form.space(4.0F);
  layout.setup_rule = gui::cell(rule_span, form.next(1.0F, 3.0F));
  layout.setup_heading = form.next(19.0F, 3.0F);
  layout.control_port = form.row();
  form.space(1.0F);
  layout.setup_status = form.next(17.0F, 3.0F);
  layout.canvas_width = form.row();
  layout.canvas_height = form.row();
  form.space(3.0F);
  layout.sync_rule = gui::cell(rule_span, form.next(1.0F, 3.0F));
  layout.sync_heading = form.next(19.0F, 3.0F);
  layout.sync_mode = form.next(details_row_height - 2.0F, 2.0F);
  layout.sync_rows = form.cursor();
  return layout;
}

[[nodiscard]] gui::UiColor
device_color(const gui::Theme &theme,
             topology::HardwareDeviceKind kind) noexcept {
  switch (kind) {
  case topology::HardwareDeviceKind::RenderServer:
    return theme.accent;
  case topology::HardwareDeviceKind::OperatorMachine:
    return theme.keyframe;
  case topology::HardwareDeviceKind::LedProcessor:
    return gui::UiColor{0.69F, 0.38F, 0.94F, 1.0F};
  case topology::HardwareDeviceKind::LedWall:
    return gui::UiColor{0.18F, 0.78F, 0.72F, 1.0F};
  }
  return theme.text_muted;
}

[[nodiscard]] gui::UiColor
signal_color(const gui::Theme &theme,
             topology::HardwareSignal signal) noexcept {
  return signal == topology::HardwareSignal::Network
             ? theme.keyframe
             : gui::UiColor{0.76F, 0.36F, 0.94F, 1.0F};
}

[[nodiscard]] std::string port_label(topology::HardwareDeviceKind kind,
                                     core::u8 index) {
  const topology::HardwarePort *const port = topology::find_port(kind, index);
  return port == nullptr ? std::string{"PORT"} : std::string{port->name};
}

void draw_device_icon(gui::UiContext &ctx, topology::HardwareDeviceKind kind,
                      float x, float y, gui::UiColor color) {
  auto &draw = ctx.draw_list();
  // Each fits an 18 by 13 box from (x, y).
  const gui::Rect box{x, y, 18.0F, 12.0F};
  if (kind == topology::HardwareDeviceKind::LedWall) {
    // A screen of three panels.
    ctx.panel(box, alpha(color, 0.20F));
    ctx.outline(box, color);
    draw.add_line(x + 6.0F, y, x + 6.0F, box.bottom(), 1.0F, color);
    draw.add_line(x + 12.0F, y, x + 12.0F, box.bottom(), 1.0F, color);
    return;
  }
  if (kind == topology::HardwareDeviceKind::OperatorMachine) {
    // A monitor on its stand.
    ctx.outline(gui::Rect{x + 1.0F, y, 16.0F, 10.0F}, color);
    draw.add_line(x + 9.0F, y + 10.0F, x + 9.0F, y + 13.0F, 1.0F, color);
    draw.add_line(x + 5.0F, y + 13.0F, x + 13.0F, y + 13.0F, 1.0F, color);
    return;
  }
  // A processor or a server: a box with a status light and two bars.
  ctx.panel(box, alpha(color, 0.18F));
  ctx.outline(box, color);
  draw.add_rect(x + 3.0F, y + 3.0F, 2.0F, 2.0F, color);
  draw.add_rect(x + 7.0F, y + 3.0F, 8.0F, 2.0F, color);
  draw.add_rect(x + 3.0F, y + 7.0F, 12.0F, 2.0F, alpha(color, 0.65F));
}

[[nodiscard]] HardwareSprite
sprite_for(const topology::HardwareDevice &device) noexcept {
  switch (device.kind) {
  case topology::HardwareDeviceKind::RenderServer:
    return HardwareSprite::RenderServer;
  case topology::HardwareDeviceKind::OperatorMachine:
    return HardwareSprite::OperatorMachine;
  case topology::HardwareDeviceKind::LedProcessor:
    switch (device.protocol) {
    case topology::HardwareProcessorProtocol::NovastarCoex:
      return HardwareSprite::ProcessorNovastar;
    case topology::HardwareProcessorProtocol::BromptonTessera:
      return HardwareSprite::ProcessorBrompton;
    case topology::HardwareProcessorProtocol::Generic:
      break;
    }
    return HardwareSprite::Processor;
  case topology::HardwareDeviceKind::LedWall:
    return HardwareSprite::LedWall;
  }
  return HardwareSprite::RenderServer;
}

[[nodiscard]] float art_pixel(const IsometricProjection &projection) noexcept {
  return projection.tile_width / 32.0F;
}

[[nodiscard]] IsoPoint project_point(const IsometricProjection &projection,
                                     float x, float y) noexcept {
  return IsoPoint{projection.origin_x + (x - y) * projection.tile_width * 0.5F,
                  projection.origin_y + (x + y) * projection.tile_height * 0.5F};
}

[[nodiscard]] gui::Rect
sprite_bounds(const IsometricProjection &projection,
              const topology::HardwareDevice &device) noexcept {
  const HardwareSpriteFrame &frame = hardware_sprite_frame(sprite_for(device));
  const float px = art_pixel(projection);
  const IsoPoint ground =
      project_isometric(projection, device.grid_x, device.grid_y);
  return gui::Rect{ground.x - static_cast<float>(frame.anchor_x) * px,
                   ground.y - static_cast<float>(frame.anchor_y) * px,
                   static_cast<float>(frame.width) * px,
                   static_cast<float>(frame.height) * px};
}

[[nodiscard]] std::size_t
port_ordinal(const HardwareDevicePresentation &presentation, core::u8 index,
             topology::HardwarePortDirection direction) noexcept {
  std::size_t ordinal{};
  for (std::size_t position = 0U; position < presentation.port_count;
       ++position) {
    const topology::HardwarePort &port = presentation.ports[position].port;
    if (port.direction != direction) {
      continue;
    }
    if (port.index == index) {
      return ordinal;
    }
    ++ordinal;
  }
  return ordinal;
}

[[nodiscard]] IsoPoint
presented_port_position(const IsometricProjection &projection,
                        const topology::HardwareDevice &device,
                        const HardwareDevicePresentation &presentation,
                        core::u8 port_index) noexcept {
  const gui::Rect sprite = sprite_bounds(projection, device);
  const PresentedHardwarePort *const shown = presentation.find(port_index);
  const topology::HardwarePort *const description =
      shown != nullptr ? &shown->port
                       : topology::find_port(device.kind, port_index);
  if (description == nullptr) {
    return IsoPoint{sprite.x + sprite.width * 0.5F,
                    sprite.y + sprite.height * 0.5F};
  }
  const HardwareSpriteFrame &frame = hardware_sprite_frame(sprite_for(device));
  const float px = art_pixel(projection);
  const bool input =
      description->direction == topology::HardwarePortDirection::Input;
  const HardwareSpriteColumn &column = input ? frame.inputs : frame.outputs;
  const auto ordinal = static_cast<float>(
      port_ordinal(presentation, port_index, description->direction));
  // A pixel outside the face, so a socket reads as plugged into its edge.
  const float x =
      sprite.x + (static_cast<float>(column.x) + (input ? -1.0F : 1.0F)) * px;
  const float y = sprite.y +
                  (static_cast<float>(column.top) + port_inset_pixels) * px +
                  ordinal * port_step_pixels * px;
  return IsoPoint{x, y};
}

[[nodiscard]] gui::Rect presented_device_bounds(
    const IsometricProjection &projection,
    const topology::HardwareDevice &device,
    const HardwareDevicePresentation &presentation) noexcept {
  gui::Rect bounds = sprite_bounds(projection, device);
  float lowest = bounds.bottom();
  for (std::size_t position = 0U; position < presentation.port_count;
       ++position) {
    const IsoPoint port = presented_port_position(
        projection, device, presentation, presentation.ports[position].port.index);
    lowest = std::max(lowest, port.y + port_radius + 2.0F);
  }
  bounds.height = lowest - bounds.y;
  return bounds;
}

[[nodiscard]] float cable_ground(const IsometricProjection &projection,
                                 const topology::HardwareDevice &device) noexcept {
  return project_isometric(projection, device.grid_x, device.grid_y).y +
         4.0F * art_pixel(projection);
}

[[nodiscard]] CableRoute
cable_route(const IsometricProjection &projection, IsoPoint from,
            float from_ground, std::size_t from_lane,
            const gui::Rect &from_device, IsoPoint to, float to_ground,
            std::size_t to_lane, const gui::Rect &to_device) noexcept {
  const float px = art_pixel(projection);
  const float out_lane = 3.0F * static_cast<float>(from_lane) * px;
  const float in_lane = 3.0F * static_cast<float>(to_lane) * px;
  const IsoPoint out{from.x + 3.0F * px + out_lane, from.y};
  const IsoPoint down{out.x, std::max(from_ground + out_lane * 0.5F, from.y)};
  const IsoPoint in{to.x - 3.0F * px - in_lane, to.y};
  const IsoPoint up{in.x, std::max(to_ground + in_lane * 0.5F, to.y)};
  // Along grid x (2, 1) and then grid y (-2, 1), or the other way about:
  // whichever turns clear of both devices, so the turn - where a cable is
  // picked and its warning drawn - is never hidden behind one.
  const float dx = up.x - down.x;
  const float dy = up.y - down.y;
  const float along_x = (dx * 0.5F + dy) * 0.5F;
  const float along_y = (dy - dx * 0.5F) * 0.5F;
  const IsoPoint x_first{down.x + 2.0F * along_x, down.y + along_x};
  const IsoPoint y_first{down.x - 2.0F * along_y, down.y + along_y};
  const auto covered = [&](const IsoPoint &point) {
    return from_device.contains(point.x, point.y) ||
           to_device.contains(point.x, point.y);
  };
  const IsoPoint corner =
      covered(x_first) && !covered(y_first) ? y_first : x_first;
  return CableRoute{{from, out, down, corner, up, in, to}};
}

[[nodiscard]] std::string
runtime_output_status(const HardwareDevicePresentation &presentation) {
  const topology::HardwareRuntimeNode *const runtime = presentation.runtime;
  if (runtime == nullptr) {
    return "OUTPUTS UNKNOWN";
  }
  const bool retained =
      runtime->stale && (runtime_inventory_is_reported(runtime) ||
                         presentation.active_video_output_count != 0U);
  if (retained) {
    return std::to_string(presentation.active_video_output_count) +
           (runtime->inventory ==
                    topology::HardwareRuntimeInventoryState::Partial
                ? "+ LAST OUT"
                : " LAST OUT");
  }
  if (runtime->inventory ==
      topology::HardwareRuntimeInventoryState::Unsupported) {
    return "OUTPUTS UNSUPPORTED";
  }
  if (runtime->inventory == topology::HardwareRuntimeInventoryState::Failed) {
    return "OUTPUTS ERROR";
  }
  if (!runtime_is_current(runtime)) {
    return "OUTPUTS UNKNOWN";
  }
  switch (runtime->inventory) {
  case topology::HardwareRuntimeInventoryState::Available:
    return std::to_string(presentation.active_video_output_count) +
           " ACTIVE OUT";
  case topology::HardwareRuntimeInventoryState::Partial:
    return std::to_string(presentation.active_video_output_count) +
           "+ ACTIVE OUT";
  case topology::HardwareRuntimeInventoryState::Unsupported:
    return "OUTPUTS UNSUPPORTED";
  case topology::HardwareRuntimeInventoryState::Failed:
    return "OUTPUTS ERROR";
  case topology::HardwareRuntimeInventoryState::Unknown:
    return "OUTPUTS UNKNOWN";
  }
  return "OUTPUTS UNKNOWN";
}

[[nodiscard]] std::string
runtime_card_status(const HardwareDevicePresentation &presentation) {
  const topology::HardwareRuntimeNode *const runtime = presentation.runtime;
  if (presentation.parked) {
    return "PARKED | NOT CABLED | NOT IN SHOW";
  }
  if (runtime == nullptr ||
      runtime->reachability ==
          topology::HardwareRuntimeReachability::Unmonitored) {
    return "CONFIGURED | UNMONITORED | " + runtime_output_status(presentation);
  }
  if (runtime->reachability ==
      topology::HardwareRuntimeReachability::ConfigurationError) {
    return "CONFIG ERROR | UNMONITORED | " +
           runtime_output_status(presentation);
  }
  if (runtime->reachability == topology::HardwareRuntimeReachability::Probing) {
    return "CONFIGURED | CHECKING | " + runtime_output_status(presentation);
  }

  const bool offline =
      runtime->stale ||
      runtime->reachability == topology::HardwareRuntimeReachability::Offline;
  std::string status = offline ? "OFFLINE" : "ONLINE";
  if (!offline) {
    switch (runtime->process) {
    case topology::HardwareRuntimeProcessState::Running:
      status += " | RUNNING";
      break;
    case topology::HardwareRuntimeProcessState::Stopped:
      status += " | STOPPED";
      break;
    case topology::HardwareRuntimeProcessState::Starting:
      status += " | STARTING";
      break;
    case topology::HardwareRuntimeProcessState::Stopping:
      status += " | STOPPING";
      break;
    case topology::HardwareRuntimeProcessState::Faulted:
      status += " | FAULTED";
      break;
    case topology::HardwareRuntimeProcessState::Unknown:
      break;
    }
  }
  status += " | " + runtime_output_status(presentation);
  return status;
}

[[nodiscard]] gui::UiColor
runtime_status_color(const gui::Theme &theme,
                     const HardwareDevicePresentation &presentation) noexcept {
  const topology::HardwareRuntimeNode *const runtime = presentation.runtime;
  if (runtime == nullptr ||
      runtime->reachability ==
          topology::HardwareRuntimeReachability::Unmonitored) {
    return theme.text_muted;
  }
  if (runtime->reachability == topology::HardwareRuntimeReachability::Online &&
      !runtime->stale) {
    return runtime->process == topology::HardwareRuntimeProcessState::Running
               ? theme.good
               : theme.keyframe;
  }
  return runtime->reachability == topology::HardwareRuntimeReachability::Probing
             ? theme.keyframe
             : theme.warning;
}

[[nodiscard]] gui::UiColor sprite_tint(const gui::Theme &theme,
                                       float opacity) noexcept {
  const float level = std::clamp(theme.text.g / 0.89F, 0.35F, 1.0F);
  return gui::UiColor{level, level, level, opacity};
}

[[nodiscard]] std::string
processor_status_text(const topology::HardwareDevice &device,
                      const topology::HardwareProcessorNode *observed) {
  if (device.protocol == topology::HardwareProcessorProtocol::Generic) {
    return "Generic, not monitored";
  }
  if (device.address.empty()) {
    return "Set its IP address to monitor it";
  }
  if (observed == nullptr ||
      observed->reachability == topology::HardwareRuntimeReachability::Probing) {
    return "Checking " + device.address;
  }
  const std::string who =
      !observed->model.empty()
          ? observed->model
          : std::string{topology::processor_protocol_name(device.protocol)};
  switch (observed->reachability) {
  case topology::HardwareRuntimeReachability::Online: {
    // What it is doing to the picture outranks that it is answering.
    const std::string attention = observed->attention();
    return attention.empty() ? "Online, " + who
                             : "Online, " + who + ": " + attention;
  }
  case topology::HardwareRuntimeReachability::ConfigurationError:
    return "Refused: " + observed->detail;
  case topology::HardwareRuntimeReachability::Offline:
    return observed->stale ? "Offline, last report kept"
                           : "Offline: " + observed->detail;
  default:
    break;
  }
  return "Not monitored";
}

[[nodiscard]] gui::UiColor
processor_status_color(const gui::Theme &theme,
                       const topology::HardwareDevice &device,
                       const topology::HardwareProcessorNode *observed) noexcept {
  if (device.protocol == topology::HardwareProcessorProtocol::Generic ||
      device.address.empty()) {
    return theme.text_muted;
  }
  if (observed == nullptr ||
      observed->reachability == topology::HardwareRuntimeReachability::Probing) {
    return theme.caution;
  }
  if (observed->reachability !=
      topology::HardwareRuntimeReachability::Online) {
    return theme.warning;
  }
  return observed->attention().empty() ? theme.good : theme.caution;
}

void draw_sprite_thumbnail(gui::UiContext &ctx, const gui::Rect &area,
                           const topology::HardwareDevice &device,
                           const rhi::Texture *sheet, float opacity) {
  const HardwareSpriteFrame &frame = hardware_sprite_frame(sprite_for(device));
  if (sheet == nullptr) {
    draw_device_icon(ctx, device.kind, area.x + (area.width - 18.0F) * 0.5F,
                     area.y + (area.height - 13.0F) * 0.5F,
                     alpha(device_color(ctx.theme(), device.kind), opacity));
    return;
  }
  const float fit = std::min(area.width / static_cast<float>(frame.width),
                             area.height / static_cast<float>(frame.height));
  const float width = static_cast<float>(frame.width) * fit;
  const float height = static_cast<float>(frame.height) * fit;
  const auto sheet_width = static_cast<float>(hardware_sprite_sheet_width());
  const auto sheet_height = static_cast<float>(hardware_sprite_sheet_height());
  ctx.draw_list().add_sprite(
      area.x + (area.width - width) * 0.5F,
      area.y + (area.height - height) * 0.5F, width, height, *sheet,
      static_cast<float>(frame.x) / sheet_width,
      static_cast<float>(frame.y) / sheet_height,
      static_cast<float>(frame.x + frame.width) / sheet_width,
      static_cast<float>(frame.y + frame.height) / sheet_height,
      sprite_tint(ctx.theme(), opacity));
}

[[nodiscard]] std::string capacity_text(core::u64 bytes) {
  std::array<char, 48U> text{};
  constexpr double gibibyte = 1024.0 * 1024.0 * 1024.0;
  const int length = std::snprintf(text.data(), text.size(), "%.1f GiB",
                                   static_cast<double>(bytes) / gibibyte);
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] std::string
output_mode_text(const topology::HardwareVideoOutput &output) {
  std::array<char, 96U> text{};
  const double refresh =
      output.refresh_denominator == 0U
          ? 0.0
          : static_cast<double>(output.refresh_numerator) /
                static_cast<double>(output.refresh_denominator);
  const std::string_view name =
      !output.connector.empty()
          ? std::string_view{output.connector}
          : (!output.name.empty() ? std::string_view{output.name}
                                  : std::string_view{"DISPLAY"});
  const int length =
      std::snprintf(text.data(), text.size(), "%.*s  %ux%u @ %.2f",
                    static_cast<int>(std::min<std::size_t>(name.size(), 28U)),
                    name.data(), output.width, output.height, refresh);
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

void draw_device_sprite(gui::UiContext &ctx,
                        const IsometricProjection &projection,
                        const topology::HardwareDevice &device,
                        const rhi::Texture *sheet, float opacity) {
  const gui::Rect bounds = sprite_bounds(projection, device);
  const HardwareSpriteFrame &frame = hardware_sprite_frame(sprite_for(device));
  if (sheet != nullptr) {
    const auto sheet_width = static_cast<float>(hardware_sprite_sheet_width());
    const auto sheet_height = static_cast<float>(hardware_sprite_sheet_height());
    ctx.draw_list().add_sprite(
        bounds.x, bounds.y, bounds.width, bounds.height, *sheet,
        static_cast<float>(frame.x) / sheet_width,
        static_cast<float>(frame.y) / sheet_height,
        static_cast<float>(frame.x + frame.width) / sheet_width,
        static_cast<float>(frame.y + frame.height) / sheet_height,
        sprite_tint(ctx.theme(), opacity));
    return;
  }
  const float px = art_pixel(projection);
  const IsoPoint ground =
      project_isometric(projection, device.grid_x, device.grid_y);
  const float half = std::min(bounds.width * 0.30F, 24.0F * px);
  const float rise = std::max(bounds.height * 0.45F, 10.0F * px);
  const gui::UiColor color = alpha(device_color(ctx.theme(), device.kind), opacity);
  const std::array<gui::UiPoint, 4U> top{{{ground.x, ground.y - rise - half * 0.5F},
                                          {ground.x + half, ground.y - rise},
                                          {ground.x, ground.y - rise + half * 0.5F},
                                          {ground.x - half, ground.y - rise}}};
  const std::array<gui::UiPoint, 4U> left{{{ground.x - half, ground.y - rise},
                                           {ground.x, ground.y - rise + half * 0.5F},
                                           {ground.x, ground.y + half * 0.5F},
                                           {ground.x - half, ground.y}}};
  const std::array<gui::UiPoint, 4U> right{{{ground.x, ground.y - rise + half * 0.5F},
                                            {ground.x + half, ground.y - rise},
                                            {ground.x + half, ground.y},
                                            {ground.x, ground.y + half * 0.5F}}};
  ctx.draw_list().add_convex_poly(left, scale(color, 0.55F));
  ctx.draw_list().add_convex_poly(right, scale(color, 0.38F));
  ctx.draw_list().add_convex_poly(top, scale(color, 0.85F));
}

void draw_floor(gui::UiContext &ctx, const IsometricProjection &projection,
                const topology::HardwareMap &map,
                std::optional<std::pair<core::i32, core::i32>> highlight,
                std::optional<std::pair<core::i32, core::i32>> selected) {
  core::i32 min_x = -4;
  core::i32 max_x = 4;
  core::i32 min_y = -4;
  core::i32 max_y = 4;
  for (const topology::HardwareDevice &device : map.devices) {
    min_x = std::min(min_x, device.grid_x - 3);
    max_x = std::max(max_x, device.grid_x + 3);
    min_y = std::min(min_y, device.grid_y - 3);
    max_y = std::max(max_y, device.grid_y + 3);
  }
  if (highlight.has_value()) {
    min_x = std::min(min_x, highlight->first - 1);
    max_x = std::max(max_x, highlight->first + 1);
    min_y = std::min(min_y, highlight->second - 1);
    max_y = std::max(max_y, highlight->second + 1);
  }
  constexpr core::i32 reach = 48;
  min_x = std::max(min_x, -reach);
  min_y = std::max(min_y, -reach);
  max_x = std::min(max_x, reach);
  max_y = std::min(max_y, reach);

  auto &draw = ctx.draw_list();
  const gui::Theme &theme = ctx.theme();
  const float px = art_pixel(projection);
  const float depth = 6.0F * px;
  const auto cell = [&](float x, float y) {
    return std::array<gui::UiPoint, 4U>{{
        {project_point(projection, x - 0.5F, y - 0.5F).x,
         project_point(projection, x - 0.5F, y - 0.5F).y},
        {project_point(projection, x + 0.5F, y - 0.5F).x,
         project_point(projection, x + 0.5F, y - 0.5F).y},
        {project_point(projection, x + 0.5F, y + 0.5F).x,
         project_point(projection, x + 0.5F, y + 0.5F).y},
        {project_point(projection, x - 0.5F, y + 0.5F).x,
         project_point(projection, x - 0.5F, y + 0.5F).y},
    }};
  };
  // The platform's two front faces, under the near edges of the floor.
  const auto corner = [&](float x, float y) {
    const IsoPoint p = project_point(projection, x, y);
    return gui::UiPoint{p.x, p.y};
  };
  const float near_x = static_cast<float>(max_x) + 0.5F;
  const float near_y = static_cast<float>(max_y) + 0.5F;
  const float far_x = static_cast<float>(min_x) - 0.5F;
  const float far_y = static_cast<float>(min_y) - 0.5F;
  const gui::UiPoint left_far = corner(far_x, near_y);
  const gui::UiPoint near_corner = corner(near_x, near_y);
  const gui::UiPoint right_far = corner(near_x, far_y);
  const std::array<gui::UiPoint, 4U> left_face{{
      left_far, near_corner, {near_corner.x, near_corner.y + depth},
      {left_far.x, left_far.y + depth}}};
  const std::array<gui::UiPoint, 4U> right_face{{
      near_corner, right_far, {right_far.x, right_far.y + depth},
      {near_corner.x, near_corner.y + depth}}};
  draw.add_convex_poly(left_face, scale(theme.widget, 0.62F));
  draw.add_convex_poly(right_face, scale(theme.widget, 0.46F));

  const gui::UiColor light = scale(theme.widget, 0.92F);
  const gui::UiColor dark = scale(theme.widget, 0.80F);
  for (core::i32 y = min_y; y <= max_y; ++y) {
    for (core::i32 x = min_x; x <= max_x; ++x) {
      draw.add_convex_poly(cell(static_cast<float>(x), static_cast<float>(y)),
                           ((x + y) & 1) == 0 ? light : dark);
    }
  }
  // Seams between the tiles, quieter than the tiles themselves.
  const gui::UiColor seam = alpha(theme.divider, 0.65F);
  for (core::i32 y = min_y; y <= max_y + 1; ++y) {
    const gui::UiPoint a = corner(far_x, static_cast<float>(y) - 0.5F);
    const gui::UiPoint b = corner(near_x, static_cast<float>(y) - 0.5F);
    draw.add_line(a.x, a.y, b.x, b.y, 1.0F, seam);
  }
  for (core::i32 x = min_x; x <= max_x + 1; ++x) {
    const gui::UiPoint a = corner(static_cast<float>(x) - 0.5F, far_y);
    const gui::UiPoint b = corner(static_cast<float>(x) - 0.5F, near_y);
    draw.add_line(a.x, a.y, b.x, b.y, 1.0F, seam);
  }
  // A lit lip along the platform's near edges.
  draw.add_line(left_far.x, left_far.y, near_corner.x, near_corner.y, 1.0F,
                scale(theme.widget_hover, 1.15F));
  draw.add_line(near_corner.x, near_corner.y, right_far.x, right_far.y, 1.0F,
                scale(theme.widget_hover, 1.15F));

  if (selected.has_value()) {
    const auto ring = cell(static_cast<float>(selected->first),
                           static_cast<float>(selected->second));
    draw.add_convex_poly(ring, alpha(theme.accent, 0.18F));
    for (std::size_t index = 0U; index < ring.size(); ++index) {
      const gui::UiPoint &a = ring[index];
      const gui::UiPoint &b = ring[(index + 1U) % ring.size()];
      draw.add_line(a.x, a.y, b.x, b.y, std::max(1.0F, px), theme.accent);
    }
  }
  if (highlight.has_value()) {
    const auto lit = cell(static_cast<float>(highlight->first),
                          static_cast<float>(highlight->second));
    draw.add_convex_poly(lit, alpha(theme.accent, 0.30F));
  }
}

void draw_nameplate(gui::UiContext &ctx, const gui::Rect &sprite,
                    std::string_view name, gui::UiColor light,
                    std::string_view status, bool selected, bool hovered) {
  const std::string_view shown = name.empty() ? std::string_view{"Device"} : name;
  const float width = std::clamp(ctx.font_width(shown) + 26.0F, 56.0F, 210.0F);
  const gui::Rect plate{sprite.x + (sprite.width - width) * 0.5F,
                        sprite.y - 12.0F, width, 20.0F};
  ctx.rounded(plate, alpha(ctx.theme().panel, 0.94F), plate.height * 0.5F);
  ctx.rounded_outline(plate,
                      selected ? ctx.theme().accent : ctx.theme().border,
                      plate.height * 0.5F, selected ? 1.5F : 1.0F);
  draw_status_dot(ctx, plate.x + 10.0F, plate.y + plate.height * 0.5F, light);
  ctx.push_clip(plate);
  ctx.label(gui::inset(plate, gui::Padding{18.0F, 0.0F, 7.0F, 0.0F}), shown,
            ctx.theme().text);
  ctx.pop_clip();
  if ((selected || hovered) && !status.empty()) {
    const float status_width =
        std::min(ctx.font_width(status) + 16.0F, 300.0F);
    const gui::Rect chip{sprite.x + (sprite.width - status_width) * 0.5F,
                         plate.bottom() + 2.0F, status_width, 17.0F};
    ctx.rounded(chip, alpha(ctx.theme().track, 0.92F), chip.height * 0.5F);
    ctx.push_clip(chip);
    ctx.label(gui::inset(chip, 8.0F, 0.0F), status, ctx.theme().text_muted);
    ctx.pop_clip();
  }
}

void draw_socket(gui::UiContext &ctx, const IsoPoint &point,
                 const topology::HardwarePort &port, float px, bool pending,
                 PortTargetState target_state,
                 RuntimePortState runtime_state) {
  const gui::UiColor base_color = signal_color(ctx.theme(), port.signal);
  const gui::UiColor color = runtime_state == RuntimePortState::Missing
                                 ? ctx.theme().warning
                                 : ((runtime_state == RuntimePortState::Stale)
                                        ? scale(base_color, 0.48F)
                                        : base_color);
  const bool targeted = target_state != PortTargetState::None;
  const gui::UiColor rim =
      target_state == PortTargetState::Compatible ||
              target_state == PortTargetState::Replaces
          ? ctx.theme().good
          : (target_state == PortTargetState::None ? color
                                                   : ctx.theme().warning);
  const float half = std::max(3.0F, 2.5F * px) + (pending || targeted ? 1.0F : 0.0F);
  const gui::Rect jack{point.x - half, point.y - half * 0.8F, half * 2.0F,
                       half * 1.6F};
  if (targeted || pending) {
    ctx.draw_list().add_rounded_glow(jack.x, jack.y, jack.width, jack.height,
                                     2.0F, 5.0F,
                                     alpha(rim, ctx.theme().glow + 0.25F));
  }
  ctx.draw_list().add_rect(jack.x - 1.0F, jack.y - 1.0F, jack.width + 2.0F,
                           jack.height + 2.0F, alpha(ctx.theme().background, 0.95F));
  ctx.draw_list().add_rect(jack.x, jack.y, jack.width, jack.height, rim);
  const float inset = std::max(1.0F, px * 0.75F);
  ctx.draw_list().add_rect(jack.x + inset, jack.y + inset,
                           jack.width - inset * 2.0F,
                           jack.height - inset * 2.0F,
                           alpha(ctx.theme().background, 0.95F));
  if (target_state == PortTargetState::Occupied) {
    draw_remove(ctx, point.x, point.y, ctx.theme().warning);
  }
}

void draw_cable(gui::UiContext &ctx, const CableRoute &route, float px,
                gui::UiColor color, bool selected, CablePart part) {
  auto &draw = ctx.draw_list();
  const float width = std::max(2.0F, px * (selected ? 2.0F : 1.5F));
  const auto segment = [&](std::size_t from, std::size_t to) {
    const IsoPoint &a = route.points[from];
    const IsoPoint &b = route.points[to];
    draw.add_line(a.x, a.y, b.x, b.y, width + 2.0F,
                  alpha(ctx.theme().background, 0.92F));
    draw.add_line(a.x, a.y, b.x, b.y, width, color);
  };
  if (part == CablePart::Floor) {
    segment(2U, 3U);
    segment(3U, 4U);
    return;
  }
  segment(0U, 1U);
  segment(1U, 2U);
  segment(4U, 5U);
  segment(5U, 6U);
}

[[nodiscard]] float point_segment_distance(float px, float py, float x0,
                                           float y0, float x1,
                                           float y1) noexcept {
  const float dx = x1 - x0;
  const float dy = y1 - y0;
  const float length_squared = dx * dx + dy * dy;
  if (length_squared <= 0.0001F) {
    return std::hypot(px - x0, py - y0);
  }
  const float t = std::clamp(((px - x0) * dx + (py - y0) * dy) / length_squared,
                             0.0F, 1.0F);
  return std::hypot(px - (x0 + t * dx), py - (y0 + t * dy));
}

[[nodiscard]] bool cable_hit(const CableRoute &route, float x,
                             float y) noexcept {
  constexpr float tolerance = 6.0F;
  for (std::size_t index = 0U; index + 1U < route.points.size(); ++index) {
    const IsoPoint &a = route.points[index];
    const IsoPoint &b = route.points[index + 1U];
    if (point_segment_distance(x, y, a.x, a.y, b.x, b.y) <= tolerance) {
      return true;
    }
  }
  return false;
}

void draw_signal_badge(gui::UiContext &ctx, const IsoPoint &at,
                       gui::UiColor color) {
  auto &draw = ctx.draw_list();
  draw.add_triangle(at.x, at.y - 9.0F, at.x + 9.0F, at.y + 6.0F, at.x - 9.0F,
                    at.y + 6.0F, alpha(ctx.theme().background, 0.95F));
  draw.add_triangle(at.x, at.y - 7.0F, at.x + 7.0F, at.y + 5.0F, at.x - 7.0F,
                    at.y + 5.0F, color);
  draw.add_rect(at.x - 1.0F, at.y - 3.0F, 2.0F, 4.5F, ctx.theme().background);
  draw.add_rect(at.x - 1.0F, at.y + 2.5F, 2.0F, 1.5F, ctx.theme().background);
}

void draw_wire(gui::UiContext &ctx, const IsoPoint &from, const IsoPoint &to,
               gui::UiColor color, bool selected) {
  const float middle_x = (from.x + to.x) * 0.5F;
  auto &draw = ctx.draw_list();
  const float width = selected ? 3.0F : 2.0F;
  // Level out of the output, across at the midpoint, level into the input.
  const std::array<gui::UiPoint, 4U> elbow{
      {{from.x, from.y}, {middle_x, from.y}, {middle_x, to.y}, {to.x, to.y}}};
  for (const auto &[thickness, ink] :
       {std::pair{width + 2.0F, alpha(ctx.theme().divider, 0.92F)},
        std::pair{width, color}}) {
    for (std::size_t index = 0U; index + 1U < elbow.size(); ++index) {
      draw.add_line(elbow[index].x, elbow[index].y, elbow[index + 1U].x,
                    elbow[index + 1U].y, thickness, ink);
    }
  }
  gui::add_arrowhead(draw, gui::UiPoint{to.x, to.y},
                     gui::UiPoint{to.x >= middle_x ? 1.0F : -1.0F, 0.0F}, 7.0F,
                     4.0F, color);
}

[[nodiscard]] const topology::HardwareLink *
link_into(const topology::HardwareMap &map,
          topology::HardwareEndpoint endpoint) noexcept {
  for (const topology::HardwareLink &link : map.links) {
    if (link.to == endpoint) {
      return &link;
    }
  }
  return nullptr;
}

[[nodiscard]] std::string hardware_count_text(std::size_t count) {
  std::array<char, 32U> text{};
  const int length = std::snprintf(text.data(), text.size(), "%llu CONFIGURED",
                                   static_cast<unsigned long long>(count));
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] bool local_address(std::string_view address) noexcept {
  return address.empty() || address.starts_with("127.") || address == "::1";
}

[[nodiscard]] bool has_render_server(const topology::HardwareMap &map) noexcept {
  return std::any_of(map.devices.begin(), map.devices.end(),
                     [](const topology::HardwareDevice &device) {
                       return device.kind ==
                              topology::HardwareDeviceKind::RenderServer;
                     });
}

[[nodiscard]] std::optional<core::u32>
adopted_control_base(const topology::HardwareMap &map,
                     const topology::HardwareDiscoveredMachine &machine) noexcept {
  if (has_render_server(map) ||
      machine.control_port == map.control.base_port + machine.node_index ||
      machine.control_port <= machine.node_index) {
    return std::nullopt;
  }
  const core::u32 base = machine.control_port - machine.node_index;
  if (base > topology::max_hardware_control_base_port) {
    return std::nullopt;
  }
  return base;
}

[[nodiscard]] DiscoveredStatus
discovered_status(const topology::HardwareMap &map,
                  const topology::HardwareDiscovery &discovery,
                  const topology::HardwareDiscoveredMachine &machine) {
  if (const topology::HardwareDevice *const placed =
          hardware_discovered_device(map, machine)) {
    return {{}, DiscoveredTone::OnMap, placed};
  }
  for (const topology::HardwareDevice &device : map.devices) {
    if (device.kind == topology::HardwareDeviceKind::RenderServer &&
        device.node_index == machine.node_index) {
      return {"NODE " + std::to_string(machine.node_index) + " USED BY " +
                  (device.address.empty() ? std::string{"LOCAL"}
                                          : device.address),
              DiscoveredTone::Warning, &device};
    }
  }
  const core::u32 expected = map.control.base_port + machine.node_index;
  if (machine.control_port != expected &&
      !adopted_control_base(map, machine).has_value()) {
    return {"LISTENS ON " + std::to_string(machine.control_port) +
                ", MAP USES " + std::to_string(expected),
            DiscoveredTone::Warning, nullptr};
  }
  if (discovery.password_set && !machine.password_set) {
    return {"NO PASSWORD SET ON IT", DiscoveredTone::Warning, nullptr};
  }
  if (!discovery.password_set && machine.password_set) {
    return {"NEEDS THE RENDER NODE PASSWORD", DiscoveredTone::Warning,
            nullptr};
  }
  return {{}, DiscoveredTone::Add, nullptr};
}

[[nodiscard]] const topology::HardwareDevice *
discovered_processor_device(
    const topology::HardwareMap &map,
    const topology::HardwareDiscoveredProcessor &found) noexcept {
  const auto bare = [](std::string_view address) {
    const std::size_t colon = address.rfind(':');
    return address.size() > 5U && colon != std::string_view::npos &&
                   address.substr(colon) == ":8001" &&
                   address.find(':') == colon
               ? address.substr(0U, colon)
               : address;
  };
  for (const topology::HardwareDevice &device : map.devices) {
    if (device.kind == topology::HardwareDeviceKind::LedProcessor &&
        device.protocol == found.protocol && !device.address.empty() &&
        bare(device.address) == bare(found.address)) {
      return &device;
    }
  }
  return nullptr;
}

[[nodiscard]] gui::UiId
discovered_processor_id(gui::UiId panel,
                        const topology::HardwareDiscoveredProcessor &found) noexcept {
  return gui::ui_id(panel, found.address,
             0x50524f00U + static_cast<core::u64>(found.protocol));
}

[[nodiscard]] core::u64
discovered_key(const topology::HardwareDiscoveredMachine &machine) noexcept {
  core::u64 hash = 1469598103934665603ULL;
  const auto add = [&hash](core::u8 value) {
    hash ^= value;
    hash *= 1099511628211ULL;
  };
  for (const char character : machine.address) {
    add(static_cast<core::u8>(character));
  }
  add(0U);
  for (int shift = 0; shift < 32; shift += 8) {
    add(static_cast<core::u8>(machine.node_index >> shift));
  }
  return hash;
}

[[nodiscard]] std::pair<core::i32, core::i32>
free_grid_cell(const topology::HardwareMap &map) noexcept {
  for (core::i64 row = 0; row < 64; ++row) {
    for (core::i64 column = 0; column < 4; ++column) {
      const core::i64 x = 3 * (column + row);
      const core::i64 y = 3 * (row - column);
      const bool clear = std::none_of(
          map.devices.begin(), map.devices.end(),
          [&](const topology::HardwareDevice &device) {
            const core::i64 across =
                static_cast<core::i64>(device.grid_x) - device.grid_y;
            const core::i64 down =
                static_cast<core::i64>(device.grid_x) + device.grid_y;
            return std::llabs(across - (x - y)) < 6 &&
                   std::llabs(down - (x + y)) < 6;
          });
      if (clear) {
        return {static_cast<core::i32>(x), static_cast<core::i32>(y)};
      }
    }
  }
  return {0, 0};
}


} // namespace mediaserver::editor::mapping::hardware
