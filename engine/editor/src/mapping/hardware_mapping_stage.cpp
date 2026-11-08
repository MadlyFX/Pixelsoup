// The Hardware Mapping canvas: the floor and cables, port, device and
// cable interaction, the stage drawn back to front, and the cable in hand.

#include "hardware_mapping_frame.hpp"

#include <mediaserver/core/output_limits.hpp>
#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/rect_cut.hpp>
#include <mediaserver/topology/hardware_map_schema.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mediaserver::editor {

using namespace mapping;
using namespace mapping::hardware;

void HardwareMappingPanel::draw_map_floor(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const topology::HardwareMap &map = f.map;
  const topology::HardwareRuntimeSnapshot &runtime = f.runtime;
  const gui::Rect &canvas = f.canvas;
  const gui::Rect &view = f.view;
  const IsometricProjection &projection = f.projection;
  ctx.panel(canvas, scale(ctx.theme().background, 0.84F));

  // What each cable into a monitored processor carries.
  f.signal_checks = topology::check_processor_signals(map, runtime);

  // The floor lights the cell a drag would put something on, and rings the
  // selected device's.
  std::optional<std::pair<core::i32, core::i32>> drop_cell;
  if (drag_kind_ == DragKind::Device) {
    drop_cell = std::pair{preview_x_, preview_y_};
  } else if ((drag_kind_ == DragKind::Palette ||
              drag_kind_ == DragKind::Discovered ||
              drag_kind_ == DragKind::DiscoveredProcessor) &&
             view.contains(ctx.pointer_x(), ctx.pointer_y())) {
    drop_cell = unproject_isometric(projection, ctx.pointer_x(), ctx.pointer_y());
  }
  std::optional<std::pair<core::i32, core::i32>> selected_cell;
  if (selection_.kind == HardwareSelectionKind::Device) {
    if (const topology::HardwareDevice *const chosen =
            map.device(selection_.node)) {
      const topology::HardwareDevice shown = f.displayed(*chosen);
      selected_cell = std::pair{shown.grid_x, shown.grid_y};
    }
  }
  ctx.push_clip(view);
  draw_floor(ctx, projection, map, drop_cell, selected_cell);
  // Cables lie on the floor, behind the devices they run between.
  for (const topology::HardwareLink &link : map.links) {
    if (const std::optional<CableRoute> route = f.route_for(link)) {
      draw_cable(ctx, *route, art_pixel(projection), f.cable_color(link),
                 selection_ ==
                     HardwareSelection{HardwareSelectionKind::Link, link.node},
                 CablePart::Floor);
    }
  }

  ctx.pop_clip();
}

void HardwareMappingPanel::interact_ports(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const topology::HardwareMap &map = f.map;
  HardwareMappingFrame &frame = f.result;
  const IsometricProjection &projection = f.projection;
  frame.link_feedback = link_notice_;
  if (pending_link_.has_value()) {
    const std::optional<topology::HardwareEndpoint> hovered_input =
        f.input_at_pointer();
    if (hovered_input.has_value()) {
      frame.link_feedback = link_feedback_for(f.target_state(*hovered_input));
    } else {
      frame.link_feedback = link_dragging_
                                ? HardwareLinkFeedback::Dragging
                                : HardwareLinkFeedback::AwaitingTarget;
    }
  }

  // Ports receive interaction before card bodies so a cable endpoint never
  // starts a device move by accident. Outputs retain capture while the
  // pointer crosses the canvas; the compatible input is resolved
  // geometrically on release because UiContext intentionally prevents a
  // second widget from becoming hot during an active drag.
  for (const topology::HardwareDevice &original : map.devices) {
    topology::HardwareDevice device = f.displayed(original);
    const HardwareDevicePresentation presentation = f.presentation_for(device);
    for (std::size_t position = 0U; position < presentation.port_count;
         ++position) {
      const topology::HardwarePort &port = presentation.ports[position].port;
      const IsoPoint point =
          presented_port_position(projection, device, presentation, port.index);
      const gui::Rect hit{point.x - port_hit_radius, point.y - port_hit_radius,
                          port_hit_radius * 2.0F, port_hit_radius * 2.0F};
      const gui::Interaction interaction = ctx.grab(
          gui::ui_id(id_, "hardware.port", original.node.value() * 64U + port.index),
          hit);
      const topology::HardwareEndpoint endpoint{original.node, port.index};
      if (port.direction == topology::HardwarePortDirection::Output) {
        if (interaction.pressed) {
          const HardwareSelection chosen{HardwareSelectionKind::Device,
                                         original.node};
          selection_ = chosen;
          frame.selection = chosen;
          link_press_cancels_ = pending_link_ == endpoint;
          pending_link_ = endpoint;
          link_dragging_ = true;
          link_drag_moved_ = false;
          link_drag_start_x_ = ctx.pointer_x();
          link_drag_start_y_ = ctx.pointer_y();
          link_notice_ = HardwareLinkFeedback::None;
          frame.link_feedback = HardwareLinkFeedback::Dragging;
        }
        if (interaction.held && link_dragging_ && pending_link_ == endpoint) {
          link_drag_moved_ = link_drag_moved_ ||
                             std::hypot(ctx.pointer_x() - link_drag_start_x_,
                                        ctx.pointer_y() - link_drag_start_y_) >=
                                 link_drag_threshold;
          if (link_drag_moved_) {
            const std::optional<topology::HardwareEndpoint> target =
                f.input_at_pointer();
            frame.link_feedback = target.has_value()
                                      ? link_feedback_for(f.target_state(*target))
                                      : HardwareLinkFeedback::Dragging;
          }
        }
        if (interaction.released && link_dragging_ &&
            pending_link_ == endpoint) {
          if (link_drag_moved_) {
            const std::optional<topology::HardwareEndpoint> target =
                f.input_at_pointer();
            if (target.has_value()) {
              f.attempt_link(*target);
            } else {
              pending_link_.reset();
              link_dragging_ = false;
              link_drag_moved_ = false;
              link_notice_ = HardwareLinkFeedback::Missed;
              frame.link_feedback = HardwareLinkFeedback::Missed;
            }
          } else {
            link_dragging_ = false;
            if (link_press_cancels_) {
              pending_link_.reset();
              link_notice_ = HardwareLinkFeedback::None;
              frame.link_feedback = HardwareLinkFeedback::None;
            } else {
              frame.link_feedback = HardwareLinkFeedback::AwaitingTarget;
            }
          }
        }
        continue;
      }
      if (interaction.clicked) {
        const HardwareSelection chosen{HardwareSelectionKind::Device,
                                       original.node};
        selection_ = chosen;
        frame.selection = chosen;
        if (pending_link_.has_value() && !link_dragging_) {
          f.attempt_link(endpoint);
        }
      }
    }
  }
}

void HardwareMappingPanel::move_devices(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const topology::HardwareMap &map = f.map;
  HardwareMappingFrame &frame = f.result;
  const IsometricProjection &projection = f.projection;
  doc::NodeId &hovered_device = f.hovered_device;
  // Devices move in grid space and author once on release.
  for (const topology::HardwareDevice &original : map.devices) {
    const topology::HardwareDevice device = f.displayed(original);
    const HardwareDevicePresentation presentation = f.presentation_for(device);
    const gui::Rect device_bounds =
        presented_device_bounds(projection, device, presentation);
    const gui::Interaction interaction = ctx.grab(
        gui::ui_id(id_, "hardware.device", original.node.value()), device_bounds);
    if (interaction.hovered) {
      hovered_device = original.node;
    }
    if (interaction.pressed) {
      const HardwareSelection chosen{HardwareSelectionKind::Device,
                                     original.node};
      selection_ = chosen;
      frame.selection = chosen;
      drag_kind_ = DragKind::Device;
      drag_device_ = original.node;
      drag_original_x_ = original.grid_x;
      drag_original_y_ = original.grid_y;
      preview_x_ = original.grid_x;
      preview_y_ = original.grid_y;
      const IsoPoint anchor =
          project_isometric(projection, original.grid_x, original.grid_y);
      drag_offset_x_ = ctx.pointer_x() - anchor.x;
      drag_offset_y_ = ctx.pointer_y() - anchor.y;
    }
    if (interaction.held && drag_kind_ == DragKind::Device &&
        drag_device_ == original.node) {
      const auto [grid_x, grid_y] =
          unproject_isometric(projection, ctx.pointer_x() - drag_offset_x_,
                              ctx.pointer_y() - drag_offset_y_);
      preview_x_ = grid_x;
      preview_y_ = grid_y;
    }
    if (interaction.released && drag_kind_ == DragKind::Device &&
        drag_device_ == original.node) {
      if (preview_x_ != drag_original_x_ || preview_y_ != drag_original_y_) {
        frame.move = HardwareMoveRequest{original.node, preview_x_, preview_y_};
      }
      drag_kind_ = DragKind::None;
      drag_device_ = {};
    }
  }
}

void HardwareMappingPanel::pick_cables(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const topology::HardwareMap &map = f.map;
  HardwareMappingFrame &frame = f.result;
  // Cable hit testing follows the devices, so a port or a device wins where
  // it overlaps a cable.
  for (const topology::HardwareLink &link : map.links) {
    const std::optional<CableRoute> route = f.route_for(link);
    const bool hit = route.has_value() &&
                     cable_hit(*route, ctx.pointer_x(), ctx.pointer_y());
    const gui::Rect hit_bounds =
        hit ? gui::Rect{ctx.pointer_x() - 1.0F, ctx.pointer_y() - 1.0F, 2.0F,
                        2.0F}
            : gui::Rect{};
    const gui::Interaction interaction =
        ctx.grab(gui::ui_id(id_, "hardware.link", link.node.value()), hit_bounds);
    if (interaction.clicked) {
      const HardwareSelection chosen{HardwareSelectionKind::Link, link.node};
      selection_ = chosen;
      frame.selection = chosen;
    }
  }
}

void HardwareMappingPanel::draw_stage(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const topology::HardwareMap &map = f.map;
  const gui::Rect &view = f.view;
  const IsometricProjection &projection = f.projection;
  doc::NodeId &hovered_device = f.hovered_device;
  // The stage, drawn back to front so a nearer device stands in front of one
  // behind it: every sprite in one run, then the cables' risers up the faces,
  // the sockets, and last the name tags.
  struct Shown final {
    topology::HardwareDevice device;
    HardwareDevicePresentation presentation;
    doc::NodeId node{};
    float opacity{1.0F};
  };
  std::vector<Shown> shown;
  shown.reserve(map.devices.size() + 1U);
  for (const topology::HardwareDevice &original : map.devices) {
    topology::HardwareDevice device = f.displayed(original);
    if (drag_kind_ == DragKind::Device && drag_device_ == original.node) {
      device.grid_x = preview_x_;
      device.grid_y = preview_y_;
    }
    HardwareDevicePresentation presentation = f.presentation_for(device);
    shown.push_back(Shown{std::move(device), presentation, original.node,
                          drag_kind_ == DragKind::Device &&
                                  drag_device_ == original.node
                              ? 0.85F
                              : 1.0F});
  }
  // What a palette or network drag would place, at the cell under the
  // pointer.
  if ((drag_kind_ == DragKind::Palette || drag_kind_ == DragKind::Discovered ||
       drag_kind_ == DragKind::DiscoveredProcessor) &&
      view.contains(ctx.pointer_x(), ctx.pointer_y())) {
    const auto [grid_x, grid_y] =
        unproject_isometric(projection, ctx.pointer_x(), ctx.pointer_y());
    topology::HardwareDevice ghost{};
    if (drag_kind_ == DragKind::Palette) {
      ghost.kind = palette_kind_;
      ghost.protocol = palette_protocol_;
      ghost.name = std::string{palette_entries[palette_index(
                                                   palette_kind_,
                                                   palette_protocol_)]
                                   .name};
    } else if (drag_kind_ == DragKind::Discovered) {
      ghost.kind = topology::HardwareDeviceKind::RenderServer;
      ghost.name = discovered_drag_.hostname;
      ghost.address = discovered_drag_.address;
      ghost.node_index = discovered_drag_.node_index;
    } else {
      ghost.kind = topology::HardwareDeviceKind::LedProcessor;
      ghost.protocol = discovered_processor_drag_.protocol;
      ghost.name = discovered_processor_drag_.name;
      ghost.address = discovered_processor_drag_.address;
    }
    ghost.grid_x = grid_x;
    ghost.grid_y = grid_y;
    HardwareDevicePresentation presentation = f.presentation_for(ghost);
    shown.push_back(Shown{std::move(ghost), presentation, doc::NodeId{}, 0.6F});
  }
  std::stable_sort(shown.begin(), shown.end(),
                   [](const Shown &left, const Shown &right) {
                     const core::i64 left_depth =
                         static_cast<core::i64>(left.device.grid_x) +
                         left.device.grid_y;
                     const core::i64 right_depth =
                         static_cast<core::i64>(right.device.grid_x) +
                         right.device.grid_y;
                     return left_depth < right_depth;
                   });
  for (const Shown &entry : shown) {
    draw_device_sprite(ctx, projection, entry.device, sprites_, entry.opacity);
  }
  const float px = art_pixel(projection);
  for (const topology::HardwareLink &link : map.links) {
    if (const std::optional<CableRoute> route = f.route_for(link)) {
      draw_cable(ctx, *route, px, f.cable_color(link),
                 selection_ ==
                     HardwareSelection{HardwareSelectionKind::Link, link.node},
                 CablePart::Risers);
      const topology::HardwareSignalCheck *const check = f.check_for(link.node);
      if (check != nullptr &&
          check->issue != topology::HardwareSignalIssue::None) {
        const IsoPoint &corner = route->points[3U];
        draw_signal_badge(ctx, corner, ctx.theme().caution);
        ctx.tooltip(gui::Rect{corner.x - 9.0F, corner.y - 9.0F, 18.0F, 16.0F},
                    topology::signal_check_text(*check));
      }
    }
  }
  for (const Shown &entry : shown) {
    if (entry.node == doc::NodeId{}) {
      continue;
    }
    const bool selected =
        selection_ == HardwareSelection{HardwareSelectionKind::Device, entry.node};
    const bool labelled = selected || hovered_device == entry.node;
    for (std::size_t position = 0U; position < entry.presentation.port_count;
         ++position) {
      const PresentedHardwarePort &port_shown = entry.presentation.ports[position];
      const topology::HardwarePort &port = port_shown.port;
      const topology::HardwareEndpoint endpoint{entry.node, port.index};
      const PortTargetState state =
          pending_link_.has_value() &&
                  port.direction == topology::HardwarePortDirection::Input
              ? f.target_state(endpoint)
              : PortTargetState::None;
      const IsoPoint point = presented_port_position(
          projection, entry.device, entry.presentation, port.index);
      draw_socket(ctx, point, port, px, pending_link_ == endpoint, state,
                  port_shown.runtime_state);
      // Port names beside the sockets of the device in hand, and of every
      // input a cable could end on.
      if (labelled || state == PortTargetState::Compatible ||
          state == PortTargetState::Replaces) {
        std::string label{port.name};
        if (entry.device.kind == topology::HardwareDeviceKind::RenderServer &&
            port.direction == topology::HardwarePortDirection::Output) {
          label = "OUT " + std::to_string(port.index) + "  " + label;
        } else if (entry.device.kind ==
                       topology::HardwareDeviceKind::LedProcessor &&
                   port.direction == topology::HardwarePortDirection::Input &&
                   port_shown.input != nullptr) {
          label = "IN " +
                  std::to_string(topology::processor_input_number(port.index)) +
                  "  " + label;
        }
        if (port_shown.runtime_state == RuntimePortState::Missing) {
          label += "  MISSING";
        } else if (port_shown.runtime_state == RuntimePortState::Stale) {
          label += "  LAST SEEN";
        }
        const float width = std::min(ctx.font_width(label) + 12.0F, 220.0F);
        const bool input =
            port.direction == topology::HardwarePortDirection::Input;
        const gui::Rect tag{input ? point.x - 9.0F - width : point.x + 9.0F,
                            point.y - 7.0F, width, 14.0F};
        ctx.rounded(tag, alpha(ctx.theme().track, 0.90F), tag.height * 0.5F);
        ctx.push_clip(tag);
        ctx.label(gui::inset(tag, 6.0F, 0.0F), label,
                  port_shown.runtime_state == RuntimePortState::Missing
                      ? ctx.theme().warning
                      : ctx.theme().text_muted);
        ctx.pop_clip();
      }
    }
  }
  for (const Shown &entry : shown) {
    const gui::Rect sprite = sprite_bounds(projection, entry.device);
    std::string status;
    gui::UiColor light = ctx.theme().text_muted;
    switch (entry.device.kind) {
    case topology::HardwareDeviceKind::RenderServer:
      status = runtime_card_status(entry.presentation);
      light = runtime_status_color(ctx.theme(), entry.presentation);
      break;
    case topology::HardwareDeviceKind::LedProcessor:
      status = processor_status_text(entry.device, entry.presentation.processor);
      light = processor_status_color(ctx.theme(), entry.device,
                                     entry.presentation.processor);
      break;
    case topology::HardwareDeviceKind::OperatorMachine:
    case topology::HardwareDeviceKind::LedWall:
      break;
    }
    const bool selected =
        entry.node != doc::NodeId{} &&
        selection_ == HardwareSelection{HardwareSelectionKind::Device, entry.node};
    draw_nameplate(ctx, sprite, entry.device.name, light, status, selected,
                   entry.node != doc::NodeId{} && hovered_device == entry.node);
  }
}

void HardwareMappingPanel::draw_cable_prompt(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const topology::HardwareMap &map = f.map;
  HardwareMappingFrame &frame = f.result;
  const gui::Rect &canvas = f.canvas;
  const IsometricProjection &projection = f.projection;
  if (pending_link_.has_value()) {
    const topology::HardwareDevice *const from =
        map.device(pending_link_->device);
    if (from != nullptr) {
      const topology::HardwareDevice displayed_from = f.displayed(*from);
      const HardwareDevicePresentation from_presentation =
          f.presentation_for(displayed_from);
      const IsoPoint start = presented_port_position(
          projection, displayed_from, from_presentation, pending_link_->port);
      const gui::UiColor wire_color =
          frame.link_feedback == HardwareLinkFeedback::Compatible ||
                  frame.link_feedback == HardwareLinkFeedback::Replaces
              ? ctx.theme().good
              : ((frame.link_feedback == HardwareLinkFeedback::Incompatible ||
                  frame.link_feedback == HardwareLinkFeedback::Occupied)
                     ? ctx.theme().warning
                     : ctx.theme().keyframe);
      draw_wire(ctx, start, IsoPoint{ctx.pointer_x(), ctx.pointer_y()},
                wire_color, false);
      std::string label;
      switch (frame.link_feedback) {
      case HardwareLinkFeedback::Compatible:
        label = "RELEASE TO CONNECT";
        break;
      case HardwareLinkFeedback::Replaces:
        label = "RELEASE TO MOVE THE CABLE HERE";
        break;
      case HardwareLinkFeedback::Incompatible:
        label = "INCOMPATIBLE SIGNAL";
        break;
      case HardwareLinkFeedback::Occupied:
        label = "INPUT ALREADY CONNECTED";
        break;
      case HardwareLinkFeedback::AwaitingTarget:
        label = "CLICK OR DRAG TO A GREEN INPUT";
        break;
      default:
        label = "DRAG " + port_label(from->kind, pending_link_->port) +
                " TO AN INPUT";
        break;
      }
      ctx.label(gui::Rect{canvas.x + 10.0F, canvas.y + 7.0F,
                          canvas.width - 20.0F, 18.0F},
                label, wire_color);
    }
  } else if (link_notice_ != HardwareLinkFeedback::None) {
    std::string_view notice;
    switch (link_notice_) {
    case HardwareLinkFeedback::Incompatible:
      notice = "CABLE NOT CREATED - INCOMPATIBLE SIGNAL";
      break;
    case HardwareLinkFeedback::Occupied:
      notice = "CABLE NOT CREATED - INPUT ALREADY CONNECTED";
      break;
    case HardwareLinkFeedback::Missed:
      notice = "CABLE NOT CREATED - DROP ON AN INPUT PORT";
      break;
    default:
      break;
    }
    if (!notice.empty()) {
      ctx.label(gui::Rect{canvas.x + 10.0F, canvas.y + 7.0F,
                          canvas.width - 20.0F, 18.0F},
                notice, ctx.theme().warning);
    }
  }
}

void HardwareMappingPanel::draw_navigation_hint(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const topology::HardwareMap &map = f.map;
  const gui::Rect &canvas = f.canvas;
  const gui::Rect &view = f.view;
  std::array<char, 64U> navigation_text{};
  const int navigation_length =
      std::snprintf(navigation_text.data(), navigation_text.size(),
                    "MMB PAN  |  WHEEL ZOOM  |  %d%%",
                    static_cast<int>(std::lround(view_.zoom * 100.0F)));
  const std::string_view navigation_label{
      navigation_text.data(),
      static_cast<std::size_t>(std::max(navigation_length, 0))};
  const float navigation_width = ctx.font_width(navigation_label);
  ctx.label(gui::Rect{view.right() - navigation_width - 10.0F,
                      view.bottom() - 21.0F, navigation_width, 17.0F},
            navigation_label, ctx.theme().text_muted);
  if (!map.devices.empty() &&
      icon_button(ctx, gui::ui_id(id_, "hardware.frame"),
                  gui::Rect{view.right() - navigation_width - 40.0F,
                            view.bottom() - 25.0F, 24.0F, 24.0F},
                  gui::Icon::Fit, "Frame every device")) {
    frame_pending_ = true;
  }

  if (map.devices.empty()) {
    ctx.label(gui::Rect{canvas.x + 16.0F, canvas.y + 34.0F,
                        canvas.width - 32.0F, 22.0F},
              "NO DEVICES CONFIGURED",
              ctx.theme().text_muted);
  }
}

} // namespace mediaserver::editor
