#include "hardware_mapping_frame.hpp"

#include <mediaserver/core/output_limits.hpp>
#include <mediaserver/editor/hardware_sprites.hpp>
#include <mediaserver/gui/shapes.hpp>
#include <mediaserver/topology/hardware_map_schema.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mediaserver::editor {

using namespace mapping;
using namespace mapping::hardware;

std::string_view hardware_window_settings_source_text(
    topology::HardwareWindowMode mode) noexcept {
  return mode == topology::HardwareWindowMode::Inherit
             ? std::string_view{"USES LAUNCH SETTINGS"}
             : std::string_view{"USES PROJECT SETTINGS"};
}

IsoPoint project_isometric(const IsometricProjection &projection,
                           core::i32 grid_x, core::i32 grid_y) noexcept {
  const float x = static_cast<float>(grid_x);
  const float y = static_cast<float>(grid_y);
  return IsoPoint{projection.origin_x + (x - y) * projection.tile_width * 0.5F,
                  projection.origin_y +
                      (x + y) * projection.tile_height * 0.5F};
}

std::pair<core::i32, core::i32>
unproject_isometric(const IsometricProjection &projection, float screen_x,
                    float screen_y) noexcept {
  if (projection.tile_width <= 0.0F || projection.tile_height <= 0.0F) {
    return {0, 0};
  }
  const float horizontal =
      (screen_x - projection.origin_x) / (projection.tile_width * 0.5F);
  const float vertical =
      (screen_y - projection.origin_y) / (projection.tile_height * 0.5F);
  const float grid_x = (horizontal + vertical) * 0.5F;
  const float grid_y = (vertical - horizontal) * 0.5F;
  constexpr float low =
      static_cast<float>(std::numeric_limits<core::i32>::min());
  constexpr float high =
      static_cast<float>(std::numeric_limits<core::i32>::max());
  return {static_cast<core::i32>(std::lround(std::clamp(grid_x, low, high))),
          static_cast<core::i32>(std::lround(std::clamp(grid_y, low, high)))};
}

gui::Rect hardware_mapping_canvas(const gui::Rect &bounds) noexcept {
  // Between the palette on the left and the details column on the right.
  gui::Rect canvas = bounds;
  static_cast<void>(gui::cut_top(canvas, title_height));
  static_cast<void>(gui::cut_left(canvas, hardware_palette_width(bounds)));
  static_cast<void>(gui::cut_right(canvas, hardware_details_width(bounds)));
  return canvas;
}

IsometricProjection
hardware_mapping_projection(const gui::Rect &bounds) noexcept {
  const gui::Rect canvas = hardware_mapping_canvas(bounds);
  // Two screen units to a sprite pixel: the pixel art lands on whole pixels
  // at the default zoom.
  constexpr float tile_width = 64.0F;
  return IsometricProjection{canvas.x + canvas.width * 0.47F,
                             canvas.y + std::min(canvas.height * 0.30F, 150.0F),
                             tile_width, tile_width * 0.50F};
}

IsometricProjection
HardwareMappingPanel::view_projection(const gui::Rect &bounds) const noexcept {
  IsometricProjection projection = hardware_mapping_projection(bounds);
  projection.origin_x += view_.pan_x;
  projection.origin_y += view_.pan_y;
  projection.tile_width *= view_.zoom;
  projection.tile_height *= view_.zoom;
  return projection;
}

gui::Rect
hardware_palette_item_bounds(const gui::Rect &bounds,
                             topology::HardwareDeviceKind kind) noexcept {
  return hardware_palette_item_bounds(
      bounds, kind, topology::HardwareProcessorProtocol::Generic);
}

gui::Rect hardware_palette_item_bounds(
    const gui::Rect &bounds, topology::HardwareDeviceKind kind,
    topology::HardwareProcessorProtocol protocol) noexcept {
  return gui::list_row(palette_layout(bounds).cards,
                       palette_index(kind, protocol), palette_row_height, 0.0F,
                       palette_row_step - palette_row_height);
}

gui::Rect
hardware_sync_mode_button_bounds(const gui::Rect &bounds) noexcept {
  return palette_layout(bounds).sync_mode;
}

gui::Rect hardware_control_port_field_bounds(const gui::Rect &bounds) noexcept {
  return palette_layout(bounds).control_port.field;
}

gui::Rect hardware_canvas_field_bounds(const gui::Rect &bounds,
                                       bool height) noexcept {
  const PaletteLayout layout = palette_layout(bounds);
  return (height ? layout.canvas_height : layout.canvas_width).field;
}

gui::Rect
hardware_discovery_strip_bounds(const gui::Rect &bounds,
                                const topology::HardwareDiscovery &discovery) noexcept {
  if (discovery.state == topology::HardwareDiscoveryState::Off &&
      !discovery.processor_search_available) {
    return {};
  }
  const gui::Rect canvas = hardware_mapping_canvas(bounds);
  const bool cards =
      !discovery.machines.empty() || !discovery.processors.empty();
  const float height =
      discovery_header_height + (cards ? discovery_card_height + 8.0F : 0.0F);
  if (canvas.width <= 0.0F ||
      canvas.height < height + discovery_minimum_view_height) {
    return {};
  }
  return gui::Rect{canvas.x, canvas.bottom() - height, canvas.width,
                   height};
}

gui::Rect
hardware_discovered_card_bounds(const gui::Rect &bounds,
                                const topology::HardwareDiscovery &discovery,
                                std::size_t index) noexcept {
  const gui::Rect strip = hardware_discovery_strip_bounds(bounds, discovery);
  if (strip.height <= discovery_header_height ||
      index >= discovery.machines.size() + discovery.processors.size()) {
    return {};
  }
  return gui::Rect{strip.x + 8.0F + static_cast<float>(index) *
                                        (discovery_card_width +
                                         discovery_card_gap),
                   strip.y + discovery_header_height, discovery_card_width,
                   discovery_card_height};
}

const topology::HardwareDevice *
hardware_discovered_device(const topology::HardwareMap &map,
                           const topology::HardwareDiscoveredMachine &machine) noexcept {
  for (const topology::HardwareDevice &device : map.devices) {
    if (device.kind != topology::HardwareDeviceKind::RenderServer ||
        device.node_index != machine.node_index) {
      continue;
    }
    if (local_address(device.address)) {
      if (machine.this_machine) {
        return &device;
      }
      continue;
    }
    if (device.address == machine.address ||
        std::find(machine.addresses.begin(), machine.addresses.end(),
                  device.address) != machine.addresses.end()) {
      return &device;
    }
  }
  return nullptr;
}

std::string
hardware_discovered_status_text(const topology::HardwareMap &map,
                                const topology::HardwareDiscovery &discovery,
                                const topology::HardwareDiscoveredMachine &machine) {
  return discovered_status(map, discovery, machine).text;
}

gui::Rect
hardware_device_bounds(const IsometricProjection &projection,
                       const topology::HardwareDevice &device) noexcept {
  return sprite_bounds(projection, device);
}

IsoPoint hardware_port_position(const IsometricProjection &projection,
                                const topology::HardwareDevice &device,
                                core::u8 port) noexcept {
  return presented_port_position(
      projection, device,
      hardware_presentation(topology::HardwareMap{}, device,
                            topology::HardwareRuntimeSnapshot{}),
      port);
}

gui::Rect hardware_presented_device_bounds(
    const IsometricProjection &projection, const topology::HardwareMap &map,
    const topology::HardwareDevice &device,
    const topology::HardwareRuntimeSnapshot &runtime) noexcept {
  const HardwareDevicePresentation presentation =
      hardware_presentation(map, device, runtime);
  return presented_device_bounds(projection, device, presentation);
}

IsoPoint hardware_presented_port_position(
    const IsometricProjection &projection, const topology::HardwareMap &map,
    const topology::HardwareDevice &device,
    const topology::HardwareRuntimeSnapshot &runtime, core::u8 port) noexcept {
  const HardwareDevicePresentation presentation =
      hardware_presentation(map, device, runtime);
  return presented_port_position(projection, device, presentation, port);
}

std::optional<IsoPoint> hardware_cable_point(
    const IsometricProjection &projection, const topology::HardwareMap &map,
    const topology::HardwareRuntimeSnapshot &runtime, doc::NodeId link) noexcept {
  const topology::HardwareLink *const cable = map.link(link);
  if (cable == nullptr) {
    return std::nullopt;
  }
  const topology::HardwareDevice *const from = map.device(cable->from.device);
  const topology::HardwareDevice *const to = map.device(cable->to.device);
  if (from == nullptr || to == nullptr) {
    return std::nullopt;
  }
  const HardwareDevicePresentation from_presentation =
      hardware_presentation(map, *from, runtime);
  const HardwareDevicePresentation to_presentation =
      hardware_presentation(map, *to, runtime);
  const CableRoute route = cable_route(
      projection,
      presented_port_position(projection, *from, from_presentation,
                              cable->from.port),
      cable_ground(projection, *from),
      port_ordinal(from_presentation, cable->from.port,
                   topology::HardwarePortDirection::Output),
      sprite_bounds(projection, *from),
      presented_port_position(projection, *to, to_presentation,
                              cable->to.port),
      cable_ground(projection, *to),
      port_ordinal(to_presentation, cable->to.port,
                   topology::HardwarePortDirection::Input),
      sprite_bounds(projection, *to));
  return route.points[3U];
}

std::string
hardware_runtime_status_text(const topology::HardwareMap &map,
                             const topology::HardwareDevice &device,
                             const topology::HardwareRuntimeSnapshot &runtime) {
  return runtime_card_status(hardware_presentation(map, device, runtime));
}

HardwareMappingFrame
HardwareMappingPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                           const topology::HardwareMap &map) {
  return draw(ctx, bounds, map, topology::HardwareRuntimeSnapshot{});
}

HardwareMappingFrame
HardwareMappingPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                           const topology::HardwareMap &map,
                           const topology::HardwareRuntimeSnapshot &runtime) {
  return draw(ctx, bounds, map, runtime, sync::SyncPolicy{});
}

HardwareMappingFrame
HardwareMappingPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                           const topology::HardwareMap &map,
                           const topology::HardwareRuntimeSnapshot &runtime,
                           const sync::SyncPolicy &sync_policy) {
  return draw(ctx, bounds, map, runtime, sync_policy, scene::Canvas{});
}

topology::HardwareDevice HardwareMappingPanel::Frame::displayed(
    const topology::HardwareDevice &original) const {
  topology::HardwareDevice device = original;
  const bool active_preview = panel.drag_kind_ == DragKind::Device &&
                              panel.drag_device_ == original.node;
  const bool released_preview =
      result.move.has_value() && result.move->device == original.node;
  if (active_preview || released_preview) {
    device.grid_x = panel.preview_x_;
    device.grid_y = panel.preview_y_;
  }
  return device;
}

HardwareDevicePresentation HardwareMappingPanel::Frame::presentation_for(
    const topology::HardwareDevice &device) const {
  return hardware_presentation(map, device, runtime);
}

const topology::HardwareSignalCheck *
HardwareMappingPanel::Frame::check_for(doc::NodeId link) const noexcept {
  for (const topology::HardwareSignalCheck &check : signal_checks) {
    if (check.link == link) {
      return &check;
    }
  }
  return nullptr;
}

std::optional<CableRoute>
HardwareMappingPanel::Frame::route_for(const topology::HardwareLink &link) const {
  const topology::HardwareDevice *const from = map.device(link.from.device);
  const topology::HardwareDevice *const to = map.device(link.to.device);
  if (from == nullptr || to == nullptr ||
      topology::find_port(from->kind, link.from.port) == nullptr) {
    return std::nullopt;
  }
  const topology::HardwareDevice displayed_from = displayed(*from);
  const topology::HardwareDevice displayed_to = displayed(*to);
  const HardwareDevicePresentation from_presentation =
      presentation_for(displayed_from);
  const HardwareDevicePresentation to_presentation =
      presentation_for(displayed_to);
  return cable_route(
      projection,
      presented_port_position(projection, displayed_from, from_presentation,
                              link.from.port),
      cable_ground(projection, displayed_from),
      port_ordinal(from_presentation, link.from.port,
                   topology::HardwarePortDirection::Output),
      sprite_bounds(projection, displayed_from),
      presented_port_position(projection, displayed_to, to_presentation,
                              link.to.port),
      cable_ground(projection, displayed_to),
      port_ordinal(to_presentation, link.to.port,
                   topology::HardwarePortDirection::Input),
      sprite_bounds(projection, displayed_to));
}

gui::UiColor
HardwareMappingPanel::Frame::cable_color(const topology::HardwareLink &link) const {
  const topology::HardwareDevice *const from = map.device(link.from.device);
  const topology::HardwarePort *const port =
      from != nullptr ? topology::find_port(from->kind, link.from.port)
                      : nullptr;
  const topology::HardwareSignalCheck *const check = check_for(link.node);
  if (check != nullptr &&
      check->issue != topology::HardwareSignalIssue::None) {
    return ctx.theme().caution;
  }
  return port != nullptr ? signal_color(ctx.theme(), port->signal)
                         : ctx.theme().text_muted;
}

std::optional<topology::HardwareEndpoint>
HardwareMappingPanel::Frame::input_at_pointer() const {
  if (!view.contains(ctx.pointer_x(), ctx.pointer_y())) {
    return std::nullopt;
  }
  std::optional<topology::HardwareEndpoint> nearest;
  float nearest_distance = port_hit_radius * port_hit_radius;
  for (const topology::HardwareDevice &original : map.devices) {
    const topology::HardwareDevice device = displayed(original);
    const HardwareDevicePresentation presentation = presentation_for(device);
    for (std::size_t position = 0U; position < presentation.port_count;
         ++position) {
      const topology::HardwarePort &port = presentation.ports[position].port;
      if (port.direction != topology::HardwarePortDirection::Input) {
        continue;
      }
      const IsoPoint point = presented_port_position(
          projection, device, presentation, port.index);
      const float dx = ctx.pointer_x() - point.x;
      const float dy = ctx.pointer_y() - point.y;
      const float distance = dx * dx + dy * dy;
      if (distance <= nearest_distance) {
        nearest_distance = distance;
        nearest = topology::HardwareEndpoint{original.node, port.index};
      }
    }
  }
  return nearest;
}

PortTargetState HardwareMappingPanel::Frame::target_state(
    topology::HardwareEndpoint endpoint) const {
  const std::optional<topology::HardwareEndpoint> &pending_link =
      panel.pending_link_;
  if (!pending_link.has_value()) {
    return PortTargetState::None;
  }
  const topology::HardwareDevice *const from = map.device(pending_link->device);
  const topology::HardwareDevice *const to = map.device(endpoint.device);
  if (from == nullptr || to == nullptr ||
      !topology::compatible(*from, pending_link->port, *to, endpoint.port)) {
    return PortTargetState::Incompatible;
  }
  // A Render Server video output may feed several inputs: an NVIDIA Mosaic
  // surface is one output to Windows but one connector per processor. A
  // processor's LED OUT may drive several walls, one per screen it runs.
  // Every other output drives one cable.
  const bool fans_out =
      (from->kind == topology::HardwareDeviceKind::RenderServer &&
       pending_link->port >= topology::first_render_server_video_port &&
       pending_link->port <= topology::last_render_server_video_port) ||
      (from->kind == topology::HardwareDeviceKind::LedProcessor &&
       pending_link->port == topology::processor_led_output_port);
  const bool output_occupied =
      !fans_out && std::any_of(map.links.begin(), map.links.end(),
                               [&](const topology::HardwareLink &link) {
                                 return link.from == *pending_link;
                               });
  if (output_occupied) {
    return PortTargetState::Occupied;
  }
  // Re-patching one device's cable to another of its outputs replaces it
  // in place; another device's cable is never pulled out by a drop.
  const topology::HardwareLink *const existing = link_into(map, endpoint);
  if (existing == nullptr) {
    return PortTargetState::Compatible;
  }
  if (existing->from == *pending_link) {
    return PortTargetState::Occupied;
  }
  return existing->from.device == pending_link->device
             ? PortTargetState::Replaces
             : PortTargetState::Occupied;
}

void HardwareMappingPanel::Frame::attempt_link(
    topology::HardwareEndpoint endpoint) {
  std::optional<topology::HardwareEndpoint> &pending_link = panel.pending_link_;
  const PortTargetState state = target_state(endpoint);
  result.link_feedback = link_feedback_for(state);
  if ((state == PortTargetState::Compatible ||
       state == PortTargetState::Replaces) &&
      pending_link.has_value()) {
    HardwareLinkRequest request{*pending_link, endpoint};
    if (const topology::HardwareLink *const replaced =
            link_into(map, endpoint);
        state == PortTargetState::Replaces && replaced != nullptr) {
      request.replaces = replaced->node;
    }
    const topology::HardwareDevice *const source =
        map.device(pending_link->device);
    if (source != nullptr &&
        source->kind == topology::HardwareDeviceKind::RenderServer) {
      if (const topology::HardwareOutputBinding *const existing =
              map.output_binding_for(source->node, pending_link->port)) {
        request.output_binding = *existing;
      } else {
        const HardwareDevicePresentation presentation =
            presentation_for(*source);
        const PresentedHardwarePort *const shown =
            presentation.find(pending_link->port);
        const topology::HardwareVideoOutput *const observed =
            shown != nullptr && shown->runtime_state == RuntimePortState::Active
                ? shown->output
                : nullptr;
        if (observed != nullptr && observed->active && observed->available &&
            observed->target.valid()) {
          request.output_binding = topology::HardwareOutputBinding{
              {}, source->node, pending_link->port, 0U, observed->target,
              observed->width, observed->height,
              observed->refresh_numerator,
              std::max(observed->refresh_denominator, core::u32{1U}),
              topology::HardwareWindowMode::Fullscreen};
        }
      }
    }
    result.link = std::move(request);
    panel.link_notice_ = HardwareLinkFeedback::None;
  } else {
    result.incompatible_link_attempted = true;
    panel.link_notice_ = result.link_feedback;
  }
  pending_link.reset();
  panel.link_dragging_ = false;
  panel.link_drag_moved_ = false;
}

void HardwareMappingPanel::Frame::sync_details(
    const topology::HardwareDevice *device) {
  const doc::NodeId next = device != nullptr ? device->node : doc::NodeId{};
  if (next == panel.details_node_) {
    return;
  }
  // A click that changes selection also commits the text field it left.
  // Emit that commit before replacing the local buffer.
  if (panel.details_node_ != doc::NodeId{} && panel.details_name_dirty_) {
    result.property_edit = HardwarePropertyEdit{
        panel.details_node_, std::string{topology::schema::name},
        doc::PropertyValue{panel.details_name_}};
  } else if (panel.details_node_ != doc::NodeId{} &&
             panel.details_address_dirty_) {
    result.property_edit = HardwarePropertyEdit{
        panel.details_node_, std::string{topology::schema::address},
        doc::PropertyValue{panel.details_address_}};
  }
  panel.details_numbers_.cancel(ctx);
  ctx.clear_focus();
  panel.details_node_ = next;
  panel.details_name_ = device != nullptr ? device->name : std::string{};
  panel.details_address_ = device != nullptr ? device->address : std::string{};
  panel.details_name_dirty_ = false;
  panel.details_address_dirty_ = false;
}

void HardwareMappingPanel::Frame::select_device(
    const topology::HardwareDevice &device) {
  const HardwareSelection chosen{HardwareSelectionKind::Device, device.node};
  panel.selection_ = chosen;
  result.selection = chosen;
}

HardwareMappingFrame
HardwareMappingPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                           const topology::HardwareMap &map,
                           const topology::HardwareRuntimeSnapshot &runtime,
                           const sync::SyncPolicy &sync_policy,
                           scene::Canvas scene_canvas) {
  Frame f{*this, ctx, bounds, map, runtime, sync_policy, scene_canvas};
  if (!begin_frame(f)) {
    return f.result;
  }
  navigate(f);
  draw_map_floor(f);
  draw_palette(f);
  draw_project_setup(f);
  draw_sync_policy(f);

  ctx.push_clip(f.view);
  interact_ports(f);
  move_devices(f);
  pick_cables(f);
  draw_stage(f);
  draw_cable_prompt(f);
  draw_navigation_hint(f);
  ctx.pop_clip();

  drop_discovered(f, draw_discovery_strip(f));
  draw_details(f);

  HardwareMappingFrame &frame = f.result;
  frame.dragging = drag_kind_ != DragKind::None || link_dragging_;
  frame.editing = frame.editing || settings_numbers_.live() ||
                  canvas_numbers_.live() || sync_numbers_.live() ||
                  link_numbers_.live();
  ctx.pop_clip();
  return frame;
}

bool HardwareMappingPanel::begin_frame(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const gui::Rect &bounds = f.bounds;
  const topology::HardwareMap &map = f.map;
  HardwareMappingFrame &frame = f.result;
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return false;
  }
  if (ctx.pointer_pressed()) {
    focused_ = bounds.contains(ctx.pointer_x(), ctx.pointer_y());
    link_notice_ = HardwareLinkFeedback::None;
  }
  if (ctx.middle_pointer_pressed()) {
    focused_ = bounds.contains(ctx.pointer_x(), ctx.pointer_y());
  }

  if ((selection_.kind == HardwareSelectionKind::Device &&
       map.device(selection_.node) == nullptr) ||
      (selection_.kind == HardwareSelectionKind::Link &&
       map.link(selection_.node) == nullptr)) {
    selection_ = HardwareSelection{};
  }
  if (drag_kind_ == DragKind::Device && map.device(drag_device_) == nullptr) {
    drag_kind_ = DragKind::None;
    drag_device_ = {};
  }
  if (pending_link_.has_value()) {
    const topology::HardwareDevice *const pending_device =
        map.device(pending_link_->device);
    const topology::HardwarePort *const pending_port =
        pending_device == nullptr
            ? nullptr
            : topology::find_port(pending_device->kind, pending_link_->port);
    if (pending_port == nullptr ||
        pending_port->direction != topology::HardwarePortDirection::Output) {
      pending_link_.reset();
      link_dragging_ = false;
    }
  }
  if (!ctx.typing() && ctx.key_pressed(gui::Key::Escape) &&
      pending_link_.has_value()) {
    pending_link_.reset();
    link_dragging_ = false;
    link_notice_ = HardwareLinkFeedback::None;
  }

  f.sync_details(selection_.kind == HardwareSelectionKind::Device
                   ? map.device(selection_.node)
                   : nullptr);

  ctx.push_clip(bounds);
  ctx.panel(bounds, ctx.theme().panel);
  const bool valid_selection =
      (selection_.kind == HardwareSelectionKind::Device &&
       map.device(selection_.node) != nullptr) ||
      (selection_.kind == HardwareSelectionKind::Link &&
       map.link(selection_.node) != nullptr);
  const std::string configured_count = hardware_count_text(map.devices.size());
  title(ctx, bounds, "HARDWARE MAPPING",
        valid_selection ? std::string_view{}
                        : std::string_view{configured_count});
  if (valid_selection) {
    gui::Rect title_strip = bounds;
    title_strip = gui::inset(gui::cut_top(title_strip, title_height),
                             gui::Padding{0.0F, 3.0F, 6.0F, 3.0F});
    const gui::Rect remove_bounds = gui::cut_right(title_strip, 124.0F);
    const std::string_view label =
        selection_.kind == HardwareSelectionKind::Device
            ? std::string_view{"REMOVE DEVICE"}
            : std::string_view{"DISCONNECT CABLE"};
    if (ctx.button(gui::ui_id(id_, "hardware.remove"), remove_bounds, label)) {
      frame.remove = HardwareDeleteRequest{selection_};
    }
  }
  if (focused_ && valid_selection && !ctx.typing() &&
      ctx.key_pressed(gui::Key::Delete)) {
    frame.remove = HardwareDeleteRequest{selection_};
    frame.consume_delete = true;
  }
  return true;
}

void HardwareMappingPanel::navigate(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const gui::Rect &bounds = f.bounds;
  const topology::HardwareMap &map = f.map;
  const topology::HardwareRuntimeSnapshot &runtime = f.runtime;
  HardwareMappingFrame &frame = f.result;
  const gui::Rect canvas = hardware_mapping_canvas(bounds);
  // Machines found on the network sit in a strip over the bottom of the
  // canvas. The projection still belongs to the whole canvas, so the strip
  // never moves a card, but the map is navigated and edited only in `view`.
  const gui::Rect strip =
      hardware_discovery_strip_bounds(bounds, runtime.discovery);
  const gui::Rect view{canvas.x, canvas.y, canvas.width,
                       std::max(canvas.height - strip.height, 0.0F)};

  // Centre the stage on its devices, zoomed out until every one fits with
  // its name tag, when the panel opens or the operator asks.
  if (frame_pending_ && view.width > 1.0F && view.height > 1.0F) {
    frame_pending_ = false;
    if (!map.devices.empty()) {
      const IsometricProjection base = hardware_mapping_projection(bounds);
      IsometricProjection unit = base;
      unit.origin_x = 0.0F;
      unit.origin_y = 0.0F;
      float left = std::numeric_limits<float>::max();
      float top = std::numeric_limits<float>::max();
      float right = std::numeric_limits<float>::lowest();
      float bottom = std::numeric_limits<float>::lowest();
      for (const topology::HardwareDevice &device : map.devices) {
        const gui::Rect sprite = sprite_bounds(unit, device);
        left = std::min(left, sprite.x);
        top = std::min(top, sprite.y - 34.0F);
        right = std::max(right, sprite.right());
        bottom = std::max(bottom, sprite.bottom());
      }
      float zoom = std::clamp(
          std::min({1.0F, view.width * 0.9F / std::max(right - left, 1.0F),
                    view.height * 0.85F / std::max(bottom - top, 1.0F)}),
          minimum_view_zoom, maximum_view_zoom);
      // Near enough to the full size is the full size, where every sprite
      // pixel lands on whole screen pixels.
      if (zoom > 0.9F) {
        zoom = 1.0F;
      }
      view_.zoom = zoom;
      view_.pan_x = view.x + view.width * 0.5F - base.origin_x -
                    zoom * (left + right) * 0.5F;
      view_.pan_y = view.y + view.height * 0.5F - base.origin_y -
                    zoom * (top + bottom) * 0.5F;
    }
  }

  // Navigation changes only this panel's screen-space projection. Device
  // placement and movement still unproject to the same integer document
  // grid, so no camera/view state leaks into topology authoring intents.
  if (ctx.middle_pointer_pressed()) {
    panning_ = view.contains(ctx.pointer_x(), ctx.pointer_y()) &&
               drag_kind_ == DragKind::None && !link_dragging_;
  }
  if (panning_ && ctx.middle_pointer_down()) {
    if (!ctx.middle_pointer_pressed()) {
      view_.pan_x += ctx.pointer_dx();
      view_.pan_y += ctx.pointer_dy();
    }
    frame.navigating = true;
  }
  if (ctx.middle_pointer_released()) {
    frame.navigating = frame.navigating || panning_;
    panning_ = false;
  }

  const float wheel = ctx.wheel();
  if (std::abs(wheel) > 0.0001F &&
      view.contains(ctx.pointer_x(), ctx.pointer_y()) &&
      drag_kind_ == DragKind::None && !link_dragging_) {
    const float next_zoom = std::clamp(view_.zoom * std::pow(1.15F, wheel),
                                       minimum_view_zoom, maximum_view_zoom);
    if (std::abs(next_zoom - view_.zoom) > 0.0001F) {
      // The view's canvas is the default projection's origin, so the grid
      // point under the pointer stays there.
      const IsometricProjection base = hardware_mapping_projection(bounds);
      view_.zoom_about(gui::Rect{base.origin_x, base.origin_y, 0.0F, 0.0F},
                       ctx.pointer_x(), ctx.pointer_y(), next_zoom);
      frame.navigating = true;
    }
  }

  const IsometricProjection projection = view_projection(bounds);
  if (drag_kind_ == DragKind::Device) {
    const auto [grid_x, grid_y] =
        unproject_isometric(projection, ctx.pointer_x() - drag_offset_x_,
                            ctx.pointer_y() - drag_offset_y_);
    preview_x_ = grid_x;
    preview_y_ = grid_y;
  }
  f.canvas = canvas;
  f.strip = strip;
  f.view = view;
  f.wheel = wheel;
  f.projection = projection;
}

} // namespace mediaserver::editor
