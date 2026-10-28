#include <mediaserver/editor/mapping_panels.hpp>

#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/rect_cut.hpp>
#include <mediaserver/gui/shapes.hpp>

#include <mediaserver/core/output_limits.hpp>
#include <mediaserver/topology/hardware_map_schema.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mediaserver::editor {
namespace {

namespace gui = mediaserver::gui;
namespace render = mediaserver::render;
namespace topology = mediaserver::topology;

constexpr float title_height = 26.0F;
constexpr float palette_header_height = 28.0F;
constexpr float palette_row_height = 42.0F;
constexpr float palette_row_step = 47.0F;
constexpr float device_width = 160.0F;
constexpr float device_height = 56.0F;
constexpr float details_label_width = 72.0F;
constexpr float details_row_height = 23.0F;
constexpr float port_radius = 5.0F;
constexpr float port_hit_radius = 14.0F;
constexpr float hardware_port_row_step = 15.0F;
constexpr float link_drag_threshold = 4.0F;
constexpr float minimum_view_zoom = 0.45F;
constexpr float maximum_view_zoom = 2.50F;
constexpr float feed_list_header_height = 30.0F;
constexpr float feed_add_button_height = 22.0F;
constexpr float feed_add_button_step = 28.0F;
constexpr float feed_row_height = 34.0F;
constexpr float feed_row_step = 37.0F;
constexpr float feed_surface_row_height = 28.0F;
constexpr float feed_surface_row_step = 31.0F;
constexpr float feed_details_label_height = 16.0F;

constexpr std::array<topology::HardwareDeviceKind, 4U> device_kinds{{
    topology::HardwareDeviceKind::RenderServer,
    topology::HardwareDeviceKind::OperatorMachine,
    topology::HardwareDeviceKind::LedProcessor,
    topology::HardwareDeviceKind::LedWall,
}};

enum class PortTargetState : core::u8 {
  None,
  Compatible,
  Incompatible,
  Occupied,
};

enum class RuntimePortState : core::u8 {
  Unknown,
  Active,
  Stale,
  Missing,
};

struct PresentedHardwarePort final {
  topology::HardwarePort port{};
  const topology::HardwareVideoOutput *output{};
  RuntimePortState runtime_state{RuntimePortState::Unknown};
};

struct HardwareDevicePresentation final {
  std::array<PresentedHardwarePort,
             static_cast<std::size_t>(topology::max_hardware_video_outputs) +
                 1U>
      ports{};
  std::size_t port_count{};
  core::u8 video_port_count{};
  core::u8 active_video_output_count{};
  const topology::HardwareRuntimeNode *runtime{};
  // A Render Server no cable touches: kept on the map, not in the show.
  bool parked{};

  [[nodiscard]] const PresentedHardwarePort *
  find(core::u8 index) const noexcept {
    for (std::size_t position = 0U; position < port_count; ++position) {
      if (ports[position].port.index == index) {
        return &ports[position];
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::size_t video_ordinal(core::u8 index) const noexcept {
    std::size_t ordinal{};
    for (std::size_t position = 0U; position < port_count; ++position) {
      const topology::HardwarePort &port = ports[position].port;
      if (port.signal != topology::HardwareSignal::Video ||
          port.direction != topology::HardwarePortDirection::Output) {
        continue;
      }
      if (port.index == index) {
        return ordinal;
      }
      ++ordinal;
    }
    return 0U;
  }
};

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

[[nodiscard]] gui::UiId mix(gui::UiId base, std::string_view name,
                            core::u64 index = 0U) noexcept {
  core::u64 hash = base ^ 0x9E3779B97F4A7C15ULL;
  for (const char character : name) {
    hash ^= static_cast<core::u8>(character);
    hash *= 1099511628211ULL;
  }
  hash ^= index + 0x9E3779B9ULL;
  hash *= 1099511628211ULL;
  return hash;
}

[[nodiscard]] gui::UiColor alpha(gui::UiColor color, float opacity) noexcept {
  color.a = opacity;
  return color;
}

[[nodiscard]] gui::UiColor scale(gui::UiColor color, float amount) noexcept {
  color.r = std::clamp(color.r * amount, 0.0F, 1.0F);
  color.g = std::clamp(color.g * amount, 0.0F, 1.0F);
  color.b = std::clamp(color.b * amount, 0.0F, 1.0F);
  return color;
}

void draw_remove(gui::UiContext &ctx, float x, float y,
                 gui::UiColor color) {
  ctx.draw_list().add_line(x - 3.0F, y - 3.0F, x + 3.0F, y + 3.0F, 1.0F,
                           color);
  ctx.draw_list().add_line(x + 3.0F, y - 3.0F, x - 3.0F, y + 3.0F, 1.0F,
                           color);
}

void circle(gui::UiContext &ctx, float x, float y, float radius,
            gui::UiColor color) {
  gui::add_disc(ctx.draw_list(), gui::UiPoint{x, y}, radius, color, 14U);
}

void title(gui::UiContext &ctx, const gui::Rect &bounds, std::string_view name,
           std::string_view trailing) {
  gui::Rect header = bounds;
  header = gui::cut_top(header, title_height);
  ctx.panel(header, ctx.theme().background);
  gui::Rect rule = header;
  ctx.panel(gui::cut_bottom(rule, 1.0F), ctx.theme().divider);
  gui::Rect text = gui::inset(header, 9.0F, 0.0F);
  ctx.label(text, name, ctx.theme().text);
  if (!trailing.empty()) {
    ctx.label(gui::cut_right(text, ctx.font_width(trailing)), trailing,
              ctx.theme().text_muted);
  }
}

[[nodiscard]] float hardware_palette_width(const gui::Rect &bounds) noexcept {
  if (bounds.width <= 0.0F) {
    return 0.0F;
  }
  const float preferred = std::clamp(bounds.width * 0.22F, 150.0F, 190.0F);
  return std::min(preferred, bounds.width * 0.46F);
}

[[nodiscard]] float hardware_details_width(const gui::Rect &bounds) noexcept {
  if (bounds.width < 720.0F) {
    return 0.0F;
  }
  return std::min(std::clamp(bounds.width * 0.24F, 205.0F, 250.0F),
                  bounds.width * 0.30F);
}

[[nodiscard]] std::size_t
kind_index(topology::HardwareDeviceKind kind) noexcept {
  for (std::size_t index = 0U; index < device_kinds.size(); ++index) {
    if (device_kinds[index] == kind) {
      return index;
    }
  }
  return 0U;
}

// The palette's settings rows: 23px rows that touch, a 77px label column,
// 9px in from the palette's sides.
[[nodiscard]] gui::Metrics palette_metrics() noexcept {
  gui::Metrics metrics{};
  metrics.inset = 9.0F;
  metrics.gap = 0.0F;
  metrics.control = details_row_height;
  metrics.row_gap = 0.0F;
  metrics.label_column = 77.0F;
  return metrics;
}

// The palette column under the title, top to bottom: the DRAG TO PLACE
// header, a card per device kind, then PROJECT SETUP - the control port and
// the scene canvas - and SYNC POLICY. Drawing and the bounds that tests and
// hit-tests ask for both read the rows from here.
struct PaletteLayout final {
  gui::Rect column;
  gui::Rect header;
  // Card `i` is list_row(cards, i, palette_row_height, 0, card gap).
  gui::Rect cards;
  gui::Rect setup_rule;
  gui::Rect setup_heading;
  gui::FormRow control_port;
  gui::Rect setup_status;
  gui::FormRow canvas_width;
  gui::FormRow canvas_height;
  gui::Rect sync_rule;
  gui::Rect sync_heading;
  gui::Rect sync_mode;
  // Where the sync policy's own rows begin.
  float sync_rows{};
};

[[nodiscard]] PaletteLayout palette_layout(const gui::Rect &bounds) noexcept {
  PaletteLayout layout{};
  gui::Rect area = bounds;
  static_cast<void>(gui::cut_top(area, title_height));
  layout.column = gui::cut_left(area, hardware_palette_width(bounds));
  gui::Rect rest = layout.column;
  layout.header = gui::inset(gui::cut_top(rest, palette_header_height), 9.0F,
                             0.0F);
  layout.cards = gui::inset(
      gui::cut_top(rest, static_cast<float>(device_kinds.size()) *
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

void draw_hardware_grid(gui::UiContext &ctx,
                        const IsometricProjection &projection) {
  auto &draw = ctx.draw_list();
  const gui::UiColor faint = alpha(ctx.theme().widget, 0.42F);
  const gui::UiColor major = alpha(ctx.theme().widget_hover, 0.60F);
  for (core::i32 row = -9; row <= 9; ++row) {
    const IsoPoint a = project_isometric(projection, -9, row);
    const IsoPoint b = project_isometric(projection, 9, row);
    draw.add_line(a.x, a.y, b.x, b.y, row == 0 ? 1.2F : 0.7F,
                  row == 0 ? major : faint);
  }
  for (core::i32 column = -9; column <= 9; ++column) {
    const IsoPoint a = project_isometric(projection, column, -9);
    const IsoPoint b = project_isometric(projection, column, 9);
    draw.add_line(a.x, a.y, b.x, b.y, column == 0 ? 1.2F : 0.7F,
                  column == 0 ? major : faint);
  }
}

[[nodiscard]] float presented_device_height(
    const topology::HardwareDevice &device,
    const HardwareDevicePresentation &presentation) noexcept {
  if (device.kind != topology::HardwareDeviceKind::RenderServer ||
      presentation.video_port_count <= 1U) {
    return device_height;
  }
  return device_height +
         static_cast<float>(presentation.video_port_count - 1U) *
             hardware_port_row_step;
}

[[nodiscard]] gui::Rect presented_device_bounds(
    const IsometricProjection &projection,
    const topology::HardwareDevice &device,
    const HardwareDevicePresentation &presentation) noexcept {
  const IsoPoint point =
      project_isometric(projection, device.grid_x, device.grid_y);
  return gui::Rect{point.x - device_width * 0.5F,
                   point.y - device_height * 0.60F, device_width,
                   presented_device_height(device, presentation)};
}

[[nodiscard]] IsoPoint
presented_port_position(const IsometricProjection &projection,
                        const topology::HardwareDevice &device,
                        const HardwareDevicePresentation &presentation,
                        core::u8 port_index) noexcept {
  const gui::Rect bounds =
      presented_device_bounds(projection, device, presentation);
  const PresentedHardwarePort *const shown = presentation.find(port_index);
  const topology::HardwarePort *const description =
      shown != nullptr ? &shown->port
                       : topology::find_port(device.kind, port_index);
  if (description == nullptr) {
    return IsoPoint{bounds.x + bounds.width * 0.5F,
                    bounds.y + device_height - 13.0F};
  }
  const float x =
      description->direction == topology::HardwarePortDirection::Input
          ? bounds.x
          : bounds.right();
  float y = bounds.y + device_height - 13.0F;
  if (device.kind == topology::HardwareDeviceKind::RenderServer &&
      description->signal == topology::HardwareSignal::Video &&
      description->direction == topology::HardwarePortDirection::Output) {
    y += static_cast<float>(presentation.video_ordinal(port_index)) *
         hardware_port_row_step;
  }
  return IsoPoint{x, y};
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

[[nodiscard]] constexpr std::string_view
window_mode_label(topology::HardwareWindowMode mode) noexcept {
  switch (mode) {
  case topology::HardwareWindowMode::Inherit:
    return "INHERIT";
  case topology::HardwareWindowMode::Windowed:
    return "WINDOWED";
  case topology::HardwareWindowMode::Borderless:
    return "BORDERLESS";
  case topology::HardwareWindowMode::Fullscreen:
    return "FULLSCREEN";
  }
  return "INHERIT";
}

[[nodiscard]] constexpr std::string_view
window_mode_property(topology::HardwareWindowMode mode) noexcept {
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
  return topology::schema::inherit_window_mode;
}

[[nodiscard]] constexpr topology::HardwareWindowMode
next_window_mode(topology::HardwareWindowMode mode) noexcept {
  switch (mode) {
  case topology::HardwareWindowMode::Inherit:
    return topology::HardwareWindowMode::Windowed;
  case topology::HardwareWindowMode::Windowed:
    return topology::HardwareWindowMode::Borderless;
  case topology::HardwareWindowMode::Borderless:
    return topology::HardwareWindowMode::Fullscreen;
  case topology::HardwareWindowMode::Fullscreen:
    return topology::HardwareWindowMode::Inherit;
  }
  return topology::HardwareWindowMode::Inherit;
}

void draw_device(gui::UiContext &ctx, const IsometricProjection &projection,
                 const topology::HardwareDevice &device,
                 const HardwareDevicePresentation &presentation, bool selected,
                 bool dragging) {
  const gui::Rect bounds =
      presented_device_bounds(projection, device, presentation);
  const gui::UiColor color = device_color(ctx.theme(), device.kind);
  const gui::UiColor body = selected ? scale(ctx.theme().selection, 1.22F)
                                     : scale(ctx.theme().panel, 1.08F);
  ctx.panel(bounds, alpha(ctx.theme().divider, 0.72F));
  // The face sits under a 9-deep bevel, 2 in from the card's other edges.
  const gui::Rect face =
      gui::inset(bounds, gui::Padding{2.0F, 9.0F, 2.0F, 2.0F});
  ctx.panel(face, dragging ? scale(body, 1.15F) : body);

  // The bevel: the card's top face receding from the face's top edge.
  const float back = bounds.y + 2.0F;
  const std::array<gui::UiPoint, 4U> top{{
      {face.x, face.y},
      {bounds.x + 18.0F, back},
      {face.right(), back},
      {bounds.right() - 18.0F, face.y},
  }};
  ctx.draw_list().add_convex_poly(top, scale(body, 1.23F));
  ctx.draw_list().add_line(bounds.x + 18.0F, back, face.right(), back, 1.0F,
                           alpha(color, 0.72F));
  ctx.outline(face, selected ? ctx.theme().accent : ctx.theme().widget_hover,
              selected ? 2.0F : 1.0F);
  {
    gui::Rect stripe = gui::inset(face, 1.0F);
    ctx.panel(gui::cut_left(stripe, 3.0F), color);
  }
  draw_device_icon(ctx, device.kind, face.x + 10.0F, face.y + 8.0F, color);
  // Text is clipped to the card face while port circles remain free to sit on
  // its outer edge. Runtime status and connector names are supplied by remote
  // machines and can legitimately be wider than this compact topology card.
  ctx.push_clip(face);
  // The name beside the icon, the status under both, and a server's
  // outputs listed under the name.
  gui::Rect name = gui::inset(face, gui::Padding{34.0F, 3.0F, 5.0F, 0.0F});
  ctx.label(gui::cut_top(name, 18.0F), device.name, ctx.theme().text);
  const std::string status = runtime_card_status(presentation);
  gui::Rect status_line =
      gui::inset(face, gui::Padding{10.0F, 20.0F, 5.0F, 0.0F});
  ctx.label(gui::cut_top(status_line, 15.0F), status,
            runtime_status_color(ctx.theme(), presentation));
  const gui::Rect outputs =
      gui::inset(face, gui::Padding{34.0F, 33.0F, 9.0F, 0.0F});

  if (device.kind == topology::HardwareDeviceKind::RenderServer) {
    for (std::size_t position = 0U; position < presentation.port_count;
         ++position) {
      const PresentedHardwarePort &shown = presentation.ports[position];
      if (shown.port.signal != topology::HardwareSignal::Video ||
          shown.port.direction != topology::HardwarePortDirection::Output) {
        continue;
      }
      const std::size_t ordinal = presentation.video_ordinal(shown.port.index);
      std::string label = "OUT " + std::to_string(shown.port.index) + "  " +
                          std::string{shown.port.name};
      if (shown.runtime_state == RuntimePortState::Missing) {
        label += "  MISSING";
      } else if (shown.runtime_state == RuntimePortState::Stale) {
        label += "  LAST SEEN";
      }
      const gui::UiColor label_color =
          shown.runtime_state == RuntimePortState::Missing
              ? ctx.theme().warning
              : (shown.runtime_state == RuntimePortState::Active
                     ? ctx.theme().text_muted
                     : scale(ctx.theme().text_muted, 0.82F));
      ctx.label(gui::list_row(outputs, ordinal, 15.0F, 0.0F,
                              hardware_port_row_step - 15.0F),
                label, label_color);
    }
  }
  ctx.pop_clip();
}

void draw_port(gui::UiContext &ctx, const IsoPoint &point,
               const topology::HardwarePort &port, bool pending,
               PortTargetState target_state,
               RuntimePortState runtime_state = RuntimePortState::Unknown) {
  const gui::UiColor base_color = signal_color(ctx.theme(), port.signal);
  const gui::UiColor color = runtime_state == RuntimePortState::Missing
                                 ? ctx.theme().warning
                                 : ((runtime_state == RuntimePortState::Stale)
                                        ? scale(base_color, 0.48F)
                                        : base_color);
  const bool targeted = target_state != PortTargetState::None;
  const gui::UiColor target_color =
      target_state == PortTargetState::Compatible
          ? ctx.theme().good
          : (target_state == PortTargetState::None ? color
                                                   : ctx.theme().warning);
  circle(ctx, point.x, point.y, pending ? 8.0F : (targeted ? 7.0F : 6.0F),
         alpha(ctx.theme().divider, 0.98F));
  circle(ctx, point.x, point.y, targeted ? port_radius + 1.0F : port_radius,
         target_color);
  if (port.direction == topology::HardwarePortDirection::Input) {
    circle(ctx, point.x, point.y, 2.1F, ctx.theme().panel);
    if (target_state == PortTargetState::Occupied) {
      ctx.draw_list().add_line(point.x - 3.0F, point.y - 3.0F, point.x + 3.0F,
                               point.y + 3.0F, 1.2F, ctx.theme().warning);
      ctx.draw_list().add_line(point.x + 3.0F, point.y - 3.0F, point.x - 3.0F,
                               point.y + 3.0F, 1.2F, ctx.theme().warning);
    }
  } else {
    gui::add_arrowhead(ctx.draw_list(), gui::UiPoint{point.x + 2.5F, point.y},
                       gui::UiPoint{1.0F, 0.0F}, 4.5F, 2.5F, ctx.theme().panel);
  }
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

[[nodiscard]] bool wire_hit(const IsoPoint &from, const IsoPoint &to, float x,
                            float y) noexcept {
  const float middle_x = (from.x + to.x) * 0.5F;
  constexpr float tolerance = 6.0F;
  return point_segment_distance(x, y, from.x, from.y, middle_x, from.y) <=
             tolerance ||
         point_segment_distance(x, y, middle_x, from.y, middle_x, to.y) <=
             tolerance ||
         point_segment_distance(x, y, middle_x, to.y, to.x, to.y) <= tolerance;
}

[[nodiscard]] bool
input_occupied(const topology::HardwareMap &map,
               topology::HardwareEndpoint endpoint) noexcept {
  for (const topology::HardwareLink &link : map.links) {
    if (link.to == endpoint) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::string hardware_count_text(std::size_t count) {
  std::array<char, 32U> text{};
  const int length = std::snprintf(text.data(), text.size(), "%llu CONFIGURED",
                                   static_cast<unsigned long long>(count));
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] float feed_list_width(const gui::Rect &bounds) noexcept {
  if (bounds.width <= 0.0F) {
    return 0.0F;
  }
  // Wide enough at a full-size panel for a row's whole destination; the
  // narrow end is unchanged so a small panel keeps its work area.
  return std::min(std::clamp(bounds.width * 0.24F, 148.0F, 280.0F),
                  bounds.width * 0.46F);
}

[[nodiscard]] float feed_details_width(const gui::Rect &bounds) noexcept {
  if (bounds.width < 680.0F) {
    return 0.0F;
  }
  return std::min(std::clamp(bounds.width * 0.29F, 222.0F, 270.0F),
                  bounds.width * 0.34F);
}

[[nodiscard]] gui::UiColor feed_color(const gui::Theme &theme,
                                      core::u32 index) noexcept {
  constexpr std::array<gui::UiColor, 6U> colors{{
      {0.18F, 0.68F, 0.98F, 1.0F},
      {0.68F, 0.36F, 0.94F, 1.0F},
      {0.18F, 0.78F, 0.72F, 1.0F},
      {0.94F, 0.55F, 0.24F, 1.0F},
      {0.93F, 0.36F, 0.55F, 1.0F},
      {0.56F, 0.78F, 0.28F, 1.0F},
  }};
  static_cast<void>(theme);
  return colors[index % colors.size()];
}

[[nodiscard]] core::u32 feed_viewport_ordinal(
    const output::FeedMap &feed_map,
    const output::OutputMap &wanted) noexcept {
  core::u32 ordinal = 1U;
  for (const output::OutputMap &map : feed_map.maps_for(wanted.node_index)) {
    if (map.node == wanted.node) {
      return ordinal;
    }
    ++ordinal;
  }
  return ordinal;
}

[[nodiscard]] std::string feed_viewport_name(
    core::u32 window_index, core::u32 viewport_ordinal) {
  std::array<char, 48U> text{};
  const int length = std::snprintf(
      text.data(), text.size(), "WINDOW %02u  |  VIEWPORT %02u",
      window_index + 1U, viewport_ordinal);
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] std::string feed_dimensions(scene::CanvasRect source) {
  std::array<char, 48U> text{};
  const int length =
      std::snprintf(text.data(), text.size(), "%u,%u  %u x %u", source.x,
                    source.y, source.width, source.height);
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

// An unnamed viewport inside its window's frame, where the window is shown.
[[nodiscard]] std::string feed_viewport_short_name(core::u32 viewport_ordinal) {
  std::array<char, 24U> text{};
  const int length = std::snprintf(text.data(), text.size(), "VIEWPORT %02u",
                                   viewport_ordinal);
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

// The operator's name for a viewport, or its window and position there.
[[nodiscard]] std::string feed_viewport_title(const output::OutputMap &map,
                                              core::u32 viewport_ordinal) {
  return map.name.empty() ? feed_viewport_name(map.node_index, viewport_ordinal)
                          : map.name;
}

[[nodiscard]] std::string feed_row_details(const output::OutputMap &map) {
  const std::string domain{
      map.projection == output::FeedProjection::PlanarSurface ? "PHYSICAL  "
                                                               : "CANVAS  "};
  return domain + feed_dimensions(map.viewport);
}

// Everything a row abbreviates, for the hover: where the picture comes from
// and where it lands.
[[nodiscard]] std::string feed_row_tooltip(const output::OutputMap &map,
                                           std::string_view title,
                                           const output::FeedMap &feed_map) {
  std::string source;
  if (map.projection == output::FeedProjection::PlanarSurface) {
    const output::ProjectionSurface *const surface =
        feed_map.surface_for(map.surface);
    source = "surface " + (surface != nullptr ? surface->name
                                               : std::string{"(missing)"}) +
             " " + feed_dimensions(map.source);
  } else {
    source = "canvas " + feed_dimensions(map.source);
  }
  std::array<char, 24U> window{};
  const int length = std::snprintf(window.data(), window.size(), "window %02u",
                                   map.node_index + 1U);
  return std::string{title} + "  |  from " + source + "  |  to " +
         std::string{window.data(),
                     static_cast<std::size_t>(std::max(length, 0))} +
         " " + feed_dimensions(map.viewport);
}

// `nodes` with `moved` taken out and put back just before or after `target`.
[[nodiscard]] std::vector<doc::NodeId>
reordered(const std::vector<doc::NodeId> &nodes, doc::NodeId moved,
          doc::NodeId target, bool after) {
  std::vector<doc::NodeId> order;
  order.reserve(nodes.size());
  for (const doc::NodeId node : nodes) {
    if (node == moved) {
      continue;
    }
    if (node == target && !after) {
      order.push_back(moved);
    }
    order.push_back(node);
    if (node == target && after) {
      order.push_back(moved);
    }
  }
  return order;
}

struct FeedWindowLayout final {
  core::u32 node_index{};
  scene::Canvas raster{};
  core::u32 offset_x{};
};

[[nodiscard]] scene::Canvas feed_window_raster(
    core::u32 feed_index, scene::Canvas fallback,
    const topology::HardwareMap *hardware) noexcept {
  if (hardware == nullptr) {
    return fallback;
  }
  for (const topology::HardwareOutputBinding &binding :
       hardware->output_bindings) {
    if (binding.feed_index == feed_index && binding.width > 0U &&
        binding.height > 0U) {
      return scene::Canvas{binding.width, binding.height};
    }
  }
  for (const topology::HardwareDevice &device : hardware->devices) {
    if (device.kind == topology::HardwareDeviceKind::RenderServer &&
        device.node_index == feed_index && device.width > 0U &&
        device.height > 0U) {
      return scene::Canvas{device.width, device.height};
    }
  }
  return fallback;
}

[[nodiscard]] std::vector<FeedWindowLayout> feed_window_layouts(
    const output::FeedMap &feed_map, scene::Canvas fallback,
    const topology::HardwareMap *hardware) {
  std::vector<FeedWindowLayout> layouts;
  layouts.reserve(feed_map.maps.size());
  core::u32 offset = 0U;
  for (const output::OutputMap &map : feed_map.maps) {
    if (!layouts.empty() && layouts.back().node_index == map.node_index) {
      continue;
    }
    const scene::Canvas raster =
        feed_window_raster(map.node_index, fallback, hardware);
    layouts.push_back(FeedWindowLayout{map.node_index, raster, offset});
    if (hardware != nullptr) {
      const core::u64 next = static_cast<core::u64>(offset) + raster.width;
      offset =
          static_cast<core::u32>(std::min<core::u64>(next, UINT32_MAX));
    }
  }
  return layouts;
}

[[nodiscard]] const FeedWindowLayout *feed_window_layout_for(
    const std::vector<FeedWindowLayout> &layouts,
    core::u32 node_index) noexcept {
  const auto found = std::find_if(
      layouts.begin(), layouts.end(),
      [node_index](const FeedWindowLayout &layout) noexcept {
        return layout.node_index == node_index;
      });
  return found == layouts.end() ? nullptr : &*found;
}

[[nodiscard]] std::string default_surface_name(std::size_t count) {
  std::array<char, 48U> text{};
  const int length = std::snprintf(text.data(), text.size(), "LED Surface %u",
                                   static_cast<unsigned>(count + 1U));
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] std::string default_mesh_surface_name(std::size_t count) {
  std::array<char, 48U> text{};
  const int length = std::snprintf(text.data(), text.size(), "UV Mesh %u",
                                   static_cast<unsigned>(count + 1U));
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] std::string mesh_source_status(std::string_view source) {
  if (source.empty()) {
    return "NO UV MESH SELECTED";
  }
  const std::filesystem::path path{source};
  const std::string filename = path.filename().string();
  return filename.empty() ? std::string{source} : filename;
}

[[nodiscard]] output::OutputMap uv_preview_map(
    const output::OutputMap &selected,
    const output::ProjectionSurface &surface) {
  output::OutputMap map = selected;
  map.projection = output::FeedProjection::PlanarSurface;
  map.surface = surface.node;
  map.source = scene::whole(surface.geometry.raster);

  output::PhysicalSurface canonical = surface.geometry;
  canonical.center = scene::Vec3{};
  canonical.right = scene::Vec3{1.0F, 0.0F, 0.0F};
  canonical.up = scene::Vec3{0.0F, 1.0F, 0.0F};
  map.resolved_surface = output::ResolvedProjectionSurface{
      canonical,
      output::ProjectionSurfaceShape::ImportedMesh,
      surface.horizontal_arc_turns,
      surface.mesh_columns,
      surface.mesh_source,
      // Destination UVs are invariant under stage-space scale. Keeping that
      // out of this preview-only key avoids re-reading a file during a live
      // calibration scrub.
      1.0F,
      surface.mesh_asset};
  return map;
}

[[nodiscard]] output::PhysicalSurface
default_surface(scene::Canvas canvas) noexcept {
  output::PhysicalSurface surface{};
  surface.center = scene::Vec3{0.0F, 0.0F, -4.0F};
  surface.right = scene::Vec3{1.0F, 0.0F, 0.0F};
  surface.up = scene::Vec3{0.0F, 1.0F, 0.0F};
  surface.width = 8.0F;
  surface.height = canvas.width == 0U
                       ? 4.5F
                       : surface.width * static_cast<float>(canvas.height) /
                             static_cast<float>(canvas.width);
  surface.raster = canvas;
  return surface;
}

[[nodiscard]] output::PhysicalSurface
default_mesh_surface(scene::Canvas canvas) noexcept {
  output::PhysicalSurface surface = default_surface(canvas);
  // External projection pieces normally retain one shared modelling origin.
  // Starting at zero preserves those authored coordinates; operators can move
  // every piece together with the explicit origin/yaw/scale calibration.
  surface.center = scene::Vec3{};
  return surface;
}

[[nodiscard]] const output::ProjectionSurface *
surface_or_null(const output::FeedMap &map,
                std::optional<doc::NodeId> node) noexcept {
  return node.has_value() ? map.surface_for(*node) : nullptr;
}

[[nodiscard]] scene::CanvasRect move_feed_rect(scene::CanvasRect source,
                                               scene::Canvas canvas,
                                               core::i64 dx,
                                               core::i64 dy) noexcept {
  const core::u32 width = std::min(source.width, canvas.width);
  const core::u32 height = std::min(source.height, canvas.height);
  const core::i64 maximum_x = static_cast<core::i64>(canvas.width - width);
  const core::i64 maximum_y = static_cast<core::i64>(canvas.height - height);
  const core::i64 moved_x = std::clamp(static_cast<core::i64>(source.x) + dx,
                                       core::i64{0}, maximum_x);
  const core::i64 moved_y = std::clamp(static_cast<core::i64>(source.y) + dy,
                                       core::i64{0}, maximum_y);
  return scene::CanvasRect{static_cast<core::u32>(moved_x),
                           static_cast<core::u32>(moved_y), width, height};
}

[[nodiscard]] scene::CanvasRect resize_feed_rect(scene::CanvasRect source,
                                                 scene::Canvas canvas,
                                                 core::i64 dx,
                                                 core::i64 dy) noexcept {
  const core::u32 x = std::min(source.x, canvas.width - 1U);
  const core::u32 y = std::min(source.y, canvas.height - 1U);
  const core::i64 maximum_width = static_cast<core::i64>(canvas.width - x);
  const core::i64 maximum_height = static_cast<core::i64>(canvas.height - y);
  const core::i64 width = std::clamp(static_cast<core::i64>(source.width) + dx,
                                     core::i64{1}, maximum_width);
  const core::i64 height = std::clamp(
      static_cast<core::i64>(source.height) + dy, core::i64{1}, maximum_height);
  return scene::CanvasRect{x, y, static_cast<core::u32>(width),
                           static_cast<core::u32>(height)};
}

} // namespace

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

namespace {

constexpr float discovery_header_height = 24.0F;
constexpr float discovery_card_width = 214.0F;
constexpr float discovery_card_height = 52.0F;
constexpr float discovery_card_gap = 6.0F;
// The strip gives way rather than leave the map less room than this.
constexpr float discovery_minimum_view_height = 140.0F;

enum class DiscoveredTone : core::u8 {
  Add,
  OnMap,
  Warning,
};

struct DiscoveredStatus final {
  std::string text;
  DiscoveredTone tone{DiscoveredTone::Add};
  // What a click selects instead of adding another card: the machine's own
  // card, or the one already using its node index.
  const topology::HardwareDevice *existing{};
};

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

// With no Render Server on the map yet nothing depends on the project's
// control range, so the first machine placed may move it to where that
// machine actually listens.
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
    return {"ON MAP - CLICK TO SELECT", DiscoveredTone::OnMap, placed};
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
  return {"DRAG OR CLICK TO ADD", DiscoveredTone::Add, nullptr};
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

// The first cell clear of every card, in rows below the origin: where a click
// puts a detected machine. Cards are compared in screen-aligned grid units -
// across (x - y) and down (x + y) - each half a tile.
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

} // namespace

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
  const float tile_width = std::clamp(canvas.width * 0.082F, 52.0F, 72.0F);
  return IsometricProjection{canvas.x + canvas.width * 0.47F,
                             canvas.y + std::min(canvas.height * 0.25F, 112.0F),
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
  return gui::list_row(palette_layout(bounds).cards, kind_index(kind),
                       palette_row_height, 0.0F,
                       palette_row_step - palette_row_height);
}

gui::Rect
hardware_sync_mode_button_bounds(const gui::Rect &bounds) noexcept {
  return palette_layout(bounds).sync_mode;
}

gui::Rect hardware_canvas_field_bounds(const gui::Rect &bounds,
                                       bool height) noexcept {
  const PaletteLayout layout = palette_layout(bounds);
  return (height ? layout.canvas_height : layout.canvas_width).field;
}

gui::Rect
hardware_discovery_strip_bounds(const gui::Rect &bounds,
                                const topology::HardwareDiscovery &discovery) noexcept {
  if (discovery.state == topology::HardwareDiscoveryState::Off) {
    return {};
  }
  const gui::Rect canvas = hardware_mapping_canvas(bounds);
  const float height =
      discovery_header_height +
      (discovery.machines.empty() ? 0.0F : discovery_card_height + 8.0F);
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
      index >= discovery.machines.size()) {
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
  const IsoPoint point =
      project_isometric(projection, device.grid_x, device.grid_y);
  return gui::Rect{point.x - device_width * 0.5F,
                   point.y - device_height * 0.60F, device_width,
                   device_height};
}

IsoPoint hardware_port_position(const IsometricProjection &projection,
                                const topology::HardwareDevice &device,
                                core::u8 port) noexcept {
  const gui::Rect bounds = hardware_device_bounds(projection, device);
  const topology::HardwarePort *const description =
      topology::find_port(device.kind, port);
  if (description == nullptr) {
    return IsoPoint{bounds.x + bounds.width * 0.5F,
                    bounds.bottom() - 11.0F};
  }
  const float x =
      description->direction == topology::HardwarePortDirection::Input
          ? bounds.x
          : bounds.right();
  return IsoPoint{x, bounds.bottom() - 13.0F};
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

HardwareMappingFrame
HardwareMappingPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                           const topology::HardwareMap &map,
                           const topology::HardwareRuntimeSnapshot &runtime,
                           const sync::SyncPolicy &sync_policy,
                           scene::Canvas scene_canvas) {
  HardwareMappingFrame frame{};
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return frame;
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

  const auto sync_details = [&](const topology::HardwareDevice *device) {
    const doc::NodeId next = device != nullptr ? device->node : doc::NodeId{};
    if (next == details_node_) {
      return;
    }
    // A click that changes selection also commits the text field it left.
    // Emit that commit before replacing the local buffer.
    if (details_node_ != doc::NodeId{} && details_name_dirty_) {
      frame.property_edit = HardwarePropertyEdit{
          details_node_, std::string{topology::schema::name},
          doc::PropertyValue{details_name_}};
    } else if (details_node_ != doc::NodeId{} && details_address_dirty_) {
      frame.property_edit = HardwarePropertyEdit{
          details_node_, std::string{topology::schema::address},
          doc::PropertyValue{details_address_}};
    }
    details_numbers_.cancel(ctx);
    ctx.clear_focus();
    details_node_ = next;
    details_name_ = device != nullptr ? device->name : std::string{};
    details_address_ = device != nullptr ? device->address : std::string{};
    details_name_dirty_ = false;
    details_address_dirty_ = false;
  };
  sync_details(selection_.kind == HardwareSelectionKind::Device
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
    if (ctx.button(mix(id_, "hardware.remove"), remove_bounds, label)) {
      frame.remove = HardwareDeleteRequest{selection_};
    }
  }
  if (focused_ && valid_selection && !ctx.typing() &&
      ctx.key_pressed(gui::Key::Delete)) {
    frame.remove = HardwareDeleteRequest{selection_};
    frame.consume_delete = true;
  }

  const gui::Rect canvas = hardware_mapping_canvas(bounds);
  // Machines found on the network sit in a strip over the bottom of the
  // canvas. The projection still belongs to the whole canvas, so the strip
  // never moves a card, but the map is navigated and edited only in `view`.
  const gui::Rect strip =
      hardware_discovery_strip_bounds(bounds, runtime.discovery);
  const gui::Rect view{canvas.x, canvas.y, canvas.width,
                       std::max(canvas.height - strip.height, 0.0F)};

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
  const auto displayed = [this,
                          &frame](const topology::HardwareDevice &original) {
    topology::HardwareDevice device = original;
    const bool active_preview =
        drag_kind_ == DragKind::Device && drag_device_ == original.node;
    const bool released_preview =
        frame.move.has_value() && frame.move->device == original.node;
    if (active_preview || released_preview) {
      device.grid_x = preview_x_;
      device.grid_y = preview_y_;
    }
    return device;
  };
  const auto presentation_for = [&](const topology::HardwareDevice &device) {
    return hardware_presentation(map, device, runtime);
  };
  ctx.panel(canvas, scale(ctx.theme().background, 0.84F));
  ctx.push_clip(view);
  draw_hardware_grid(ctx, projection);

  // Existing authored cables sit behind the cards.
  for (const topology::HardwareLink &link : map.links) {
    const topology::HardwareDevice *const from = map.device(link.from.device);
    const topology::HardwareDevice *const to = map.device(link.to.device);
    if (from == nullptr || to == nullptr) {
      continue;
    }
    const topology::HardwarePort *const port =
        topology::find_port(from->kind, link.from.port);
    if (port == nullptr) {
      continue;
    }
    const topology::HardwareDevice displayed_from = displayed(*from);
    const topology::HardwareDevice displayed_to = displayed(*to);
    const HardwareDevicePresentation from_presentation =
        presentation_for(displayed_from);
    const HardwareDevicePresentation to_presentation =
        presentation_for(displayed_to);
    draw_wire(ctx,
              presented_port_position(projection, displayed_from,
                                      from_presentation, link.from.port),
              presented_port_position(projection, displayed_to, to_presentation,
                                      link.to.port),
              signal_color(ctx.theme(), port->signal),
              selection_ ==
                  HardwareSelection{HardwareSelectionKind::Link, link.node});
  }

  ctx.pop_clip();

  // Palette: these are authored device kinds, not pretend discovered hosts.
  const PaletteLayout layout = palette_layout(bounds);
  const gui::Rect palette = layout.column;
  ctx.panel(palette, scale(ctx.theme().panel, 0.94F));
  {
    gui::Rect edge = palette;
    ctx.panel(gui::cut_right(edge, 1.0F), ctx.theme().divider);
  }
  ctx.label(layout.header, "DRAG TO PLACE", ctx.theme().text_muted);
  // Whether a palette row fits above the palette's bottom edge.
  const auto fits = [&](const gui::Rect &row) {
    return row.bottom() <= palette.bottom();
  };

  for (const topology::HardwareDeviceKind kind : device_kinds) {
    const gui::Rect item = hardware_palette_item_bounds(bounds, kind);
    const gui::Interaction interaction =
        ctx.grab(mix(id_, "hardware.palette", kind_index(kind)), item);
    ctx.panel(item, interaction.held
                        ? ctx.theme().selection
                        : (interaction.hovered ? ctx.theme().widget_hover
                                               : ctx.theme().row));
    ctx.outline(item,
                interaction.held ? ctx.theme().accent : ctx.theme().divider);
    const gui::UiColor color = device_color(ctx.theme(), kind);
    draw_device_icon(ctx, kind, item.x + 10.0F, item.y + 9.0F, color);
    // The kind's name over what it is, right of the icon.
    const gui::Rect text = gui::inset(item, gui::Padding{38.0F, 0.0F, 5.0F, 0.0F});
    ctx.label(gui::inset(text, gui::Padding{0.0F, 2.0F, 0.0F, 21.0F}),
              topology::device_kind_name(kind), ctx.theme().text);
    ctx.label(gui::inset(text, gui::Padding{0.0F, 20.0F, 0.0F, 6.0F}),
              "CONFIGURED DEVICE", ctx.theme().text_muted);

    if (interaction.pressed) {
      drag_kind_ = DragKind::Palette;
      palette_kind_ = kind;
    }
    if (interaction.released && drag_kind_ == DragKind::Palette &&
        palette_kind_ == kind) {
      if (view.contains(ctx.pointer_x(), ctx.pointer_y())) {
        const auto [grid_x, grid_y] =
            unproject_isometric(projection, ctx.pointer_x(), ctx.pointer_y());
        frame.place = HardwarePlaceRequest{kind, grid_x, grid_y};
      }
      drag_kind_ = DragKind::None;
    }
  }

  // One lifecycle port range governs the project. Render Servers contribute
  // their host and node index; asking operators to retype host:port on every
  // card made a wall configuration both repetitive and easy to diverge.
  if (fits(layout.control_port.field)) {
    ctx.panel(layout.setup_rule, ctx.theme().divider);
    ctx.label(layout.setup_heading, "PROJECT SETUP", ctx.theme().text_muted);
    ctx.label(layout.control_port.label, "Control port",
              ctx.theme().text_muted);
    double changed_port{};
    if (settings_numbers_.draw(
            ctx, mix(id_, "hardware.settings.control-port"),
            layout.control_port.field, {},
            static_cast<double>(map.control.base_port), 1.0, changed_port, 0)) {
      const auto rounded = static_cast<core::i64>(std::llround(std::clamp(
          changed_port, 1.0,
          static_cast<double>(topology::max_hardware_control_base_port))));
      frame.control_port_edit = HardwareControlPortEdit{
          map.control.node, static_cast<core::u32>(rounded)};
    }
    const std::string_view setup_status =
        map.control.authored ? std::string_view{"SAVED PROJECT RANGE"}
                             : std::string_view{"DEFAULT - EDIT TO SAVE"};
    ctx.label(layout.setup_status, setup_status, ctx.theme().text_muted);
  }

  // The canvas every scene renders, which canvas feed mappings crop. Its
  // rows are reserved even without a scene so the sync controls below never
  // move under the pointer when one appears.
  if (scene::valid(scene_canvas)) {
    const auto canvas_row = [&](bool height) {
      const gui::FormRow row =
          height ? layout.canvas_height : layout.canvas_width;
      if (!fits(row.field)) {
        return;
      }
      ctx.label(row.label, height ? "Canvas H" : "Canvas W",
                ctx.theme().text_muted);
      const core::u32 current =
          height ? scene_canvas.height : scene_canvas.width;
      double edited{};
      if (!canvas_numbers_.draw(
              ctx,
              mix(id_, height ? "hardware.settings.canvas-height"
                              : "hardware.settings.canvas-width"),
              row.field, "px", static_cast<double>(current), 1.0, edited, 0)) {
        return;
      }
      const auto rounded = static_cast<core::u32>(std::llround(std::clamp(
          edited, 1.0, static_cast<double>(core::max_output_dimension))));
      scene::Canvas next = frame.canvas_edit.has_value()
                               ? frame.canvas_edit->canvas
                               : scene_canvas;
      (height ? next.height : next.width) = rounded;
      if (next != scene_canvas) {
        frame.canvas_edit = HardwareCanvasEdit{next};
      }
    };
    canvas_row(false);
    canvas_row(true);
  }

  // The project synchronization policy sits directly under the control port
  // because both are project settings, and because the sync groups this
  // policy governs are derived from the very cables drawn in this panel.
  if (fits(layout.sync_mode)) {
    ctx.panel(layout.sync_rule, ctx.theme().divider);
    ctx.label(layout.sync_heading, "SYNC POLICY", ctx.theme().text_muted);

    sync::SyncPolicy edited = sync_policy;
    bool changed = false;
    // The policy's rows continue down the palette from the MODE button.
    gui::Metrics rows_metrics = palette_metrics();
    gui::FormLayout rows{
        gui::Rect{palette.x, layout.sync_rows, palette.width,
                  std::max(palette.bottom() - layout.sync_rows, 0.0F)},
        rows_metrics};
    const auto cycle_button = [&](std::string_view suffix,
                                  const gui::Rect &button,
                                  std::string_view label,
                                  bool emphasised) -> bool {
      if (!fits(button)) {
        return false;
      }
      const gui::Interaction interaction = ctx.grab(mix(id_, suffix), button);
      const gui::UiColor base = emphasised
                                    ? scale(ctx.theme().selection, 0.9F)
                                    : ctx.theme().widget;
      ctx.panel(button, interaction.hovered ? scale(base, 1.2F) : base);
      ctx.outline(button, ctx.theme().divider);
      ctx.label(gui::inset(button, 6.0F, 0.0F), label,
                emphasised ? ctx.theme().text : ctx.theme().text_muted);
      return interaction.clicked;
    };

    std::string mode_label{"MODE: "};
    mode_label += sync::sync_mode_name(sync_policy.mode);
    if (cycle_button("hardware.sync.mode", layout.sync_mode, mode_label,
                     sync_policy.requires_physical_sync())) {
      switch (sync_policy.mode) {
      case sync::SyncMode::Unsynced:
        edited.mode = sync::SyncMode::Framelock;
        break;
      case sync::SyncMode::Framelock:
        edited.mode = sync::SyncMode::ExternalGenlock;
        break;
      case sync::SyncMode::ExternalGenlock:
        edited.mode = sync::SyncMode::Unsynced;
        break;
      }
      changed = true;
    }

    const auto number_row = [&](std::string_view suffix,
                                std::string_view label, double value,
                                double minimum, double maximum,
                                double &result) -> bool {
      const gui::FormRow row = rows.row();
      if (!fits(row.field)) {
        return false;
      }
      ctx.label(row.label, label, ctx.theme().text_muted);
      double edited_value{};
      if (!sync_numbers_.draw(ctx, mix(id_, suffix), row.field, {}, value, 1.0,
                              edited_value, 0)) {
        return false;
      }
      result = std::clamp(edited_value, minimum, maximum);
      return true;
    };

    if (sync_policy.requires_physical_sync()) {
      double next{};
      if (number_row("hardware.sync.width", "Width",
                     static_cast<double>(sync_policy.required_timing.width),
                     1.0, static_cast<double>(sync::max_sync_timing_width),
                     next)) {
        edited.required_timing.width =
            static_cast<core::u32>(std::llround(next));
        changed = true;
      }
      if (number_row("hardware.sync.height", "Height",
                     static_cast<double>(sync_policy.required_timing.height),
                     1.0, static_cast<double>(sync::max_sync_timing_height),
                     next)) {
        edited.required_timing.height =
            static_cast<core::u32>(std::llround(next));
        changed = true;
      }
      if (number_row(
              "hardware.sync.rate-num", "Rate num",
              static_cast<double>(
                  sync_policy.required_timing.refresh_numerator),
              1.0, static_cast<double>(sync::max_sync_refresh_numerator),
              next)) {
        edited.required_timing.refresh_numerator =
            static_cast<core::u32>(std::llround(next));
        changed = true;
      }
      if (number_row(
              "hardware.sync.rate-den", "Rate den",
              static_cast<double>(
                  sync_policy.required_timing.refresh_denominator),
              1.0, static_cast<double>(sync::max_sync_refresh_denominator),
              next)) {
        edited.required_timing.refresh_denominator =
            static_cast<core::u32>(std::llround(next));
        changed = true;
      }
      if (number_row("hardware.sync.group", "Swap group",
                     static_cast<double>(sync_policy.group), 1.0,
                     static_cast<double>(sync::max_sync_group_id), next)) {
        edited.group = static_cast<core::u32>(std::llround(next));
        changed = true;
      }
      if (number_row("hardware.sync.barrier", "Barrier",
                     static_cast<double>(sync_policy.barrier), 1.0,
                     static_cast<double>(sync::max_sync_barrier_id), next)) {
        edited.barrier = static_cast<core::u32>(std::llround(next));
        changed = true;
      }
      rows.space(2.0F);

      std::string failure_label{"ON FAILURE: "};
      failure_label += sync::sync_failure_policy_name(sync_policy.failure);
      if (cycle_button("hardware.sync.failure",
                       rows.next(details_row_height - 2.0F, 2.0F),
                       failure_label,
                       sync_policy.failure ==
                           sync::SyncFailurePolicy::Strict)) {
        edited.failure = sync_policy.failure == sync::SyncFailurePolicy::Strict
            ? sync::SyncFailurePolicy::Degraded
            : sync::SyncFailurePolicy::Strict;
        changed = true;
      }

      std::string late_label{"LATE FRAME: "};
      late_label += sync::late_frame_policy_name(sync_policy.late_frame);
      if (cycle_button("hardware.sync.late-frame",
                       rows.next(details_row_height - 2.0F, 4.0F), late_label,
                       sync_policy.late_frame ==
                           sync::LateFramePolicy::HoldGroup)) {
        edited.late_frame =
            sync_policy.late_frame == sync::LateFramePolicy::HoldGroup
            ? sync::LateFramePolicy::AdvanceIndependently
            : sync::LateFramePolicy::HoldGroup;
        changed = true;
      }
    }

    const gui::Rect status = rows.next(17.0F, 0.0F);
    if (fits(status)) {
      const std::string_view sync_status =
          !sync_policy.authored
              ? std::string_view{"DEFAULT - EDIT TO SAVE"}
              : (sync_policy.requires_physical_sync()
                     ? std::string_view{"SAVED - VERIFIED IN OUTPUT MONITOR"}
                     : std::string_view{"SAVED - NO PHYSICAL TIMING CLAIM"});
      ctx.label(status, sync_status, ctx.theme().text_muted);
    }

    if (changed) {
      // Authoring always writes a complete, valid policy value, so a
      // half-edited raster can never reach the document and widen what the
      // project claims about physical timing.
      edited.authored = true;
      if (sync::valid_policy(edited)) {
        frame.sync_policy_edit =
            HardwareSyncPolicyEdit{sync_policy.node, edited};
      }
    }
  }

  ctx.push_clip(view);

  const auto input_at_pointer =
      [&]() -> std::optional<topology::HardwareEndpoint> {
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
  };
  const auto target_state = [&](topology::HardwareEndpoint endpoint) {
    if (!pending_link_.has_value()) {
      return PortTargetState::None;
    }
    const topology::HardwareDevice *const from =
        map.device(pending_link_->device);
    const topology::HardwareDevice *const to = map.device(endpoint.device);
    if (from == nullptr || to == nullptr ||
        !topology::compatible(*from, pending_link_->port, *to, endpoint.port)) {
      return PortTargetState::Incompatible;
    }
    const bool output_occupied = std::any_of(
        map.links.begin(), map.links.end(),
        [&](const topology::HardwareLink &link) {
          return link.from == *pending_link_;
        });
    if (output_occupied) {
      return PortTargetState::Occupied;
    }
    return input_occupied(map, endpoint) ? PortTargetState::Occupied
                                         : PortTargetState::Compatible;
  };
  const auto feedback_for = [](PortTargetState state) {
    switch (state) {
    case PortTargetState::Compatible:
      return HardwareLinkFeedback::Compatible;
    case PortTargetState::Incompatible:
      return HardwareLinkFeedback::Incompatible;
    case PortTargetState::Occupied:
      return HardwareLinkFeedback::Occupied;
    case PortTargetState::None:
      break;
    }
    return HardwareLinkFeedback::None;
  };
  const auto attempt_link = [&](topology::HardwareEndpoint endpoint) {
    const PortTargetState state = target_state(endpoint);
    frame.link_feedback = feedback_for(state);
    if (state == PortTargetState::Compatible && pending_link_.has_value()) {
      HardwareLinkRequest request{*pending_link_, endpoint};
      const topology::HardwareDevice *const source =
          map.device(pending_link_->device);
      if (source != nullptr &&
          source->kind == topology::HardwareDeviceKind::RenderServer) {
        if (const topology::HardwareOutputBinding *const existing =
                map.output_binding_for(source->node, pending_link_->port)) {
          request.output_binding = *existing;
        } else {
          const HardwareDevicePresentation presentation =
              presentation_for(*source);
          const PresentedHardwarePort *const shown =
              presentation.find(pending_link_->port);
          const topology::HardwareVideoOutput *const observed =
              shown != nullptr && shown->runtime_state == RuntimePortState::Active
                  ? shown->output
                  : nullptr;
          if (observed != nullptr && observed->active && observed->available &&
              observed->target.valid()) {
            request.output_binding = topology::HardwareOutputBinding{
                {}, source->node, pending_link_->port, 0U, observed->target,
                observed->width, observed->height,
                observed->refresh_numerator,
                std::max(observed->refresh_denominator, core::u32{1U}),
                topology::HardwareWindowMode::Fullscreen};
          }
        }
      }
      frame.link = std::move(request);
      link_notice_ = HardwareLinkFeedback::None;
    } else {
      frame.incompatible_link_attempted = true;
      link_notice_ = frame.link_feedback;
    }
    pending_link_.reset();
    link_dragging_ = false;
    link_drag_moved_ = false;
  };

  frame.link_feedback = link_notice_;
  if (pending_link_.has_value()) {
    const std::optional<topology::HardwareEndpoint> hovered_input =
        input_at_pointer();
    if (hovered_input.has_value()) {
      frame.link_feedback = feedback_for(target_state(*hovered_input));
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
    topology::HardwareDevice device = displayed(original);
    const HardwareDevicePresentation presentation = presentation_for(device);
    for (std::size_t position = 0U; position < presentation.port_count;
         ++position) {
      const topology::HardwarePort &port = presentation.ports[position].port;
      const IsoPoint point =
          presented_port_position(projection, device, presentation, port.index);
      const gui::Rect hit{point.x - port_hit_radius, point.y - port_hit_radius,
                          port_hit_radius * 2.0F, port_hit_radius * 2.0F};
      const gui::Interaction interaction = ctx.grab(
          mix(id_, "hardware.port", original.node.value() * 64U + port.index),
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
                input_at_pointer();
            frame.link_feedback = target.has_value()
                                      ? feedback_for(target_state(*target))
                                      : HardwareLinkFeedback::Dragging;
          }
        }
        if (interaction.released && link_dragging_ &&
            pending_link_ == endpoint) {
          if (link_drag_moved_) {
            const std::optional<topology::HardwareEndpoint> target =
                input_at_pointer();
            if (target.has_value()) {
              attempt_link(*target);
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
          attempt_link(endpoint);
        }
      }
    }
  }

  // Device cards move in grid space and author once on release.
  for (const topology::HardwareDevice &original : map.devices) {
    topology::HardwareDevice device = displayed(original);
    const HardwareDevicePresentation presentation = presentation_for(device);
    const gui::Rect device_bounds =
        presented_device_bounds(projection, device, presentation);
    const gui::Interaction interaction = ctx.grab(
        mix(id_, "hardware.device", original.node.value()), device_bounds);
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
      device.grid_x = preview_x_;
      device.grid_y = preview_y_;
    }
    if (interaction.released && drag_kind_ == DragKind::Device &&
        drag_device_ == original.node) {
      if (preview_x_ != drag_original_x_ || preview_y_ != drag_original_y_) {
        frame.move = HardwareMoveRequest{original.node, preview_x_, preview_y_};
      }
      drag_kind_ = DragKind::None;
      drag_device_ = {};
    }

    draw_device(ctx, projection, device, presentation,
                selection_ == HardwareSelection{HardwareSelectionKind::Device,
                                                original.node},
                drag_kind_ == DragKind::Device &&
                    drag_device_ == original.node);
  }

  // Draw ports last so their small targets remain readable over each card.
  for (const topology::HardwareDevice &original : map.devices) {
    topology::HardwareDevice device = displayed(original);
    const HardwareDevicePresentation presentation = presentation_for(device);
    for (std::size_t position = 0U; position < presentation.port_count;
         ++position) {
      const PresentedHardwarePort &shown = presentation.ports[position];
      const topology::HardwarePort &port = shown.port;
      const topology::HardwareEndpoint endpoint{original.node, port.index};
      const PortTargetState state =
          pending_link_.has_value() &&
                  port.direction == topology::HardwarePortDirection::Input
              ? target_state(endpoint)
              : PortTargetState::None;
      draw_port(
          ctx,
          presented_port_position(projection, device, presentation, port.index),
          port, pending_link_ == endpoint, state, shown.runtime_state);
    }
  }

  // Link hit testing follows the foreground controls, so ports and devices
  // win where geometry overlaps a cable endpoint.
  for (const topology::HardwareLink &link : map.links) {
    const topology::HardwareDevice *const from = map.device(link.from.device);
    const topology::HardwareDevice *const to = map.device(link.to.device);
    if (from == nullptr || to == nullptr) {
      continue;
    }
    const topology::HardwareDevice displayed_from = displayed(*from);
    const topology::HardwareDevice displayed_to = displayed(*to);
    const HardwareDevicePresentation from_presentation =
        presentation_for(displayed_from);
    const HardwareDevicePresentation to_presentation =
        presentation_for(displayed_to);
    const IsoPoint from_point = presented_port_position(
        projection, displayed_from, from_presentation, link.from.port);
    const IsoPoint to_point = presented_port_position(
        projection, displayed_to, to_presentation, link.to.port);
    const bool hit =
        wire_hit(from_point, to_point, ctx.pointer_x(), ctx.pointer_y());
    const gui::Rect hit_bounds =
        hit ? gui::Rect{ctx.pointer_x() - 1.0F, ctx.pointer_y() - 1.0F, 2.0F,
                        2.0F}
            : gui::Rect{};
    const gui::Interaction interaction =
        ctx.grab(mix(id_, "hardware.link", link.node.value()), hit_bounds);
    if (interaction.clicked) {
      const HardwareSelection chosen{HardwareSelectionKind::Link, link.node};
      selection_ = chosen;
      frame.selection = chosen;
    }
  }

  if (pending_link_.has_value()) {
    const topology::HardwareDevice *const from =
        map.device(pending_link_->device);
    if (from != nullptr) {
      const topology::HardwareDevice displayed_from = displayed(*from);
      const HardwareDevicePresentation from_presentation =
          presentation_for(displayed_from);
      const IsoPoint start = presented_port_position(
          projection, displayed_from, from_presentation, pending_link_->port);
      const gui::UiColor wire_color =
          frame.link_feedback == HardwareLinkFeedback::Compatible
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

  if (drag_kind_ == DragKind::Palette) {
    const auto [grid_x, grid_y] =
        unproject_isometric(projection, ctx.pointer_x(), ctx.pointer_y());
    topology::HardwareDevice ghost{};
    ghost.kind = palette_kind_;
    ghost.name = std::string{topology::device_kind_name(palette_kind_)};
    ghost.grid_x = grid_x;
    ghost.grid_y = grid_y;
    const HardwareDevicePresentation ghost_presentation =
        presentation_for(ghost);
    draw_device(ctx, projection, ghost, ghost_presentation, true, true);
  }
  if (drag_kind_ == DragKind::Discovered) {
    const auto [grid_x, grid_y] =
        unproject_isometric(projection, ctx.pointer_x(), ctx.pointer_y());
    topology::HardwareDevice ghost{};
    ghost.kind = topology::HardwareDeviceKind::RenderServer;
    ghost.name = discovered_drag_.hostname;
    ghost.address = discovered_drag_.address;
    ghost.node_index = discovered_drag_.node_index;
    ghost.grid_x = grid_x;
    ghost.grid_y = grid_y;
    draw_device(ctx, projection, ghost, presentation_for(ghost), true, true);
  }

  if (map.devices.empty()) {
    ctx.label(gui::Rect{canvas.x + 16.0F, canvas.y + 34.0F,
                        canvas.width - 32.0F, 22.0F},
              "NO DEVICES CONFIGURED - DRAG ONE FROM THE PALETTE",
              ctx.theme().text_muted);
  }
  ctx.pop_clip();

  // Render machines found on the network. A card drags onto the map like a
  // palette entry, or a click adds it at a free spot; either way the new
  // Render Server already carries the machine's name, address, and node
  // index. A machine already on the map selects its card instead.
  const auto place_discovered = [&](const topology::HardwareDiscoveredMachine
                                        &machine,
                                    core::i32 grid_x, core::i32 grid_y) {
    HardwarePlaceRequest request{topology::HardwareDeviceKind::RenderServer,
                                 grid_x, grid_y};
    request.machine =
        HardwarePlaceMachine{machine.hostname, machine.address,
                             machine.node_index};
    frame.place = std::move(request);
    if (const std::optional<core::u32> base =
            adopted_control_base(map, machine)) {
      frame.control_port_edit =
          HardwareControlPortEdit{map.control.node, *base};
    }
  };
  const auto select_device = [&](const topology::HardwareDevice &device) {
    const HardwareSelection chosen{HardwareSelectionKind::Device, device.node};
    selection_ = chosen;
    frame.selection = chosen;
  };
  const topology::HardwareDiscovery &discovery = runtime.discovery;
  std::optional<gui::Interaction> dragged;
  if (strip.height > 0.0F) {
    ctx.panel(strip, scale(ctx.theme().panel, 0.90F));
    ctx.panel(gui::Rect{strip.x, strip.y, strip.width, 1.0F},
              ctx.theme().divider);
    ctx.push_clip(strip);
    constexpr std::string_view heading{"DETECTED ON NETWORK"};
    const float heading_width = ctx.font_width(heading);
    ctx.label(gui::Rect{strip.x + 9.0F, strip.y + 3.0F, heading_width, 19.0F},
              heading, ctx.theme().text_muted);
    std::string summary;
    gui::UiColor summary_color = ctx.theme().text_muted;
    if (discovery.state == topology::HardwareDiscoveryState::Unavailable) {
      summary = "UNAVAILABLE";
      if (!discovery.detail.empty()) {
        summary += " - " + discovery.detail;
      }
      summary_color = ctx.theme().warning;
    } else if (discovery.machines.empty()) {
      summary = "SEARCHING - NONE FOUND. RENDER MACHINES ANSWER ON UDP " +
                std::to_string(discovery.port);
    } else {
      summary = std::to_string(discovery.machines.size()) +
                (discovery.machines.size() == 1U ? " MACHINE" : " MACHINES") +
                " - DRAG ONE ONTO THE MAP";
    }
    ctx.label(gui::Rect{strip.x + heading_width + 21.0F, strip.y + 3.0F,
                        std::max(strip.width - heading_width - 30.0F, 0.0F),
                        19.0F},
              summary, summary_color);

    const float step = discovery_card_width + discovery_card_gap;
    const float content =
        static_cast<float>(discovery.machines.size()) * step + 10.0F;
    if (std::abs(wheel) > 0.0001F &&
        strip.contains(ctx.pointer_x(), ctx.pointer_y()) &&
        drag_kind_ == DragKind::None) {
      discovered_scroll_ -= wheel * step;
    }
    const float max_scroll = std::max(content - strip.width, 0.0F);
    discovered_scroll_ = std::clamp(discovered_scroll_, 0.0F, max_scroll);
    // Arrows at either end when cards run past the strip. They are grabbed
    // ahead of the cards beneath them, which would otherwise take the press.
    constexpr float arrow_width = 20.0F;
    const float cards_y = strip.y + discovery_header_height;
    const gui::Rect left_arrow{strip.x, cards_y, arrow_width,
                               discovery_card_height};
    const gui::Rect right_arrow{strip.right() - arrow_width, cards_y,
                                arrow_width, discovery_card_height};
    const bool can_scroll_left =
        !discovery.machines.empty() && discovered_scroll_ > 0.5F;
    const bool can_scroll_right =
        !discovery.machines.empty() && discovered_scroll_ < max_scroll - 0.5F;
    gui::Interaction left_interaction{};
    gui::Interaction right_interaction{};
    if (can_scroll_left) {
      left_interaction =
          ctx.grab(mix(id_, "hardware.discovered.left"), left_arrow);
      if (left_interaction.clicked) {
        discovered_scroll_ = std::max(discovered_scroll_ - step, 0.0F);
      }
    }
    if (can_scroll_right) {
      right_interaction =
          ctx.grab(mix(id_, "hardware.discovered.right"), right_arrow);
      if (right_interaction.clicked) {
        discovered_scroll_ = std::min(discovered_scroll_ + step, max_scroll);
      }
    }

    for (std::size_t index = 0U; index < discovery.machines.size(); ++index) {
      const topology::HardwareDiscoveredMachine &machine =
          discovery.machines[index];
      gui::Rect card = hardware_discovered_card_bounds(bounds, discovery, index);
      card.x -= discovered_scroll_;
      const core::u64 key = discovered_key(machine);
      const gui::Interaction interaction =
          ctx.grab(mix(id_, "hardware.discovered", key), card);
      const DiscoveredStatus status =
          discovered_status(map, discovery, machine);
      if (interaction.pressed) {
        if (status.tone == DiscoveredTone::OnMap) {
          select_device(*status.existing);
        } else {
          drag_kind_ = DragKind::Discovered;
          discovered_drag_ = machine;
          discovered_press_x_ = ctx.pointer_x();
          discovered_press_y_ = ctx.pointer_y();
        }
      }
      const bool dragging_this = drag_kind_ == DragKind::Discovered &&
                                 discovered_key(discovered_drag_) == key;
      if (dragging_this) {
        dragged = interaction;
      }
      if (card.x > strip.right() || card.right() < strip.x) {
        continue;
      }
      const bool on_map = status.tone == DiscoveredTone::OnMap;
      ctx.panel(card, dragging_this
                          ? ctx.theme().selection
                          : (interaction.hovered ? ctx.theme().widget_hover
                                                 : ctx.theme().row));
      ctx.outline(card,
                  on_map ? ctx.theme().good
                    : (dragging_this ? ctx.theme().accent
                                     : ctx.theme().divider));
      draw_device_icon(
          ctx, topology::HardwareDeviceKind::RenderServer, card.x + 10.0F,
          card.y + 9.0F,
          on_map ? ctx.theme().text_muted
                 : device_color(ctx.theme(),
                                topology::HardwareDeviceKind::RenderServer));
      const float text_x = card.x + 37.0F;
      const float text_width = std::max(card.width - 42.0F, 0.0F);
      ctx.label(gui::Rect{text_x, card.y + 2.0F, text_width, 17.0F},
                machine.hostname.empty() ? machine.address : machine.hostname,
                on_map ? ctx.theme().text_muted : ctx.theme().text);
      std::string where = machine.address + "  NODE " +
                          std::to_string(machine.node_index);
      if (machine.this_machine) {
        where += "  THIS PC";
      }
      ctx.label(gui::Rect{text_x, card.y + 18.0F, text_width, 15.0F}, where,
                ctx.theme().text_muted);
      ctx.label(gui::Rect{text_x, card.y + 33.0F, text_width, 15.0F},
                status.text,
                status.tone == DiscoveredTone::OnMap
                    ? ctx.theme().good
                    : (status.tone == DiscoveredTone::Warning
                           ? ctx.theme().warning
                           : ctx.theme().text_muted));
    }
    const auto draw_arrow = [&](const gui::Rect &area,
                                const gui::Interaction &interaction,
                                bool pointing_right) {
      ctx.panel(area, interaction.hovered ? ctx.theme().widget_hover
                                          : scale(ctx.theme().panel, 0.90F));
      ctx.outline(area, ctx.theme().divider);
      const float middle_x = area.x + area.width * 0.5F;
      const float middle_y = area.y + area.height * 0.5F;
      const float tip = pointing_right ? 3.0F : -3.0F;
      auto &draw = ctx.draw_list();
      draw.add_line(middle_x - tip, middle_y - 6.0F, middle_x + tip, middle_y,
                    1.5F, ctx.theme().text);
      draw.add_line(middle_x + tip, middle_y, middle_x - tip, middle_y + 6.0F,
                    1.5F, ctx.theme().text);
    };
    if (can_scroll_left) {
      draw_arrow(left_arrow, left_interaction, false);
    }
    if (can_scroll_right) {
      draw_arrow(right_arrow, right_interaction, true);
    }
    ctx.pop_clip();
  }
  if (drag_kind_ == DragKind::Discovered) {
    // A machine that stopped answering, or a strip that gave way, still has
    // the pointer captured: keep reading that capture so its release lands.
    if (!dragged.has_value()) {
      dragged = ctx.grab(
          mix(id_, "hardware.discovered", discovered_key(discovered_drag_)),
          gui::Rect{});
    }
    if (dragged->released) {
      const float moved = std::hypot(ctx.pointer_x() - discovered_press_x_,
                                     ctx.pointer_y() - discovered_press_y_);
      if (view.contains(ctx.pointer_x(), ctx.pointer_y())) {
        const auto [grid_x, grid_y] =
            unproject_isometric(projection, ctx.pointer_x(), ctx.pointer_y());
        place_discovered(discovered_drag_, grid_x, grid_y);
      } else if (dragged->clicked && moved < link_drag_threshold * 2.0F) {
        const DiscoveredStatus status =
            discovered_status(map, discovery, discovered_drag_);
        if (status.existing != nullptr) {
          select_device(*status.existing);
        } else {
          const auto [grid_x, grid_y] = free_grid_cell(map);
          place_discovered(discovered_drag_, grid_x, grid_y);
        }
      }
      drag_kind_ = DragKind::None;
    } else if (!dragged->held) {
      drag_kind_ = DragKind::None;
    }
  }

  // Selection may have changed in the card/port pass above. The details rail
  // follows immediately and occupies space excluded from the grid, so its
  // controls never fight a device beneath them for pointer capture.
  const topology::HardwareDevice *const selected_device =
      selection_.kind == HardwareSelectionKind::Device
          ? map.device(selection_.node)
          : nullptr;
  sync_details(selected_device);
  gui::Rect details = bounds;
  static_cast<void>(gui::cut_top(details, title_height));
  details = gui::cut_right(details, hardware_details_width(bounds));
  if (details.width > 0.0F) {
    ctx.panel(details, scale(ctx.theme().panel, 0.96F));
    {
      gui::Rect edge = details;
      ctx.panel(gui::cut_left(edge, 1.0F), ctx.theme().divider);
      gui::Rect heading = gui::inset(details, gui::Padding{10.0F, 3.0F, 10.0F, 0.0F});
      ctx.label(gui::cut_top(heading, 22.0F), "DEVICE DETAILS",
                ctx.theme().text_muted);
    }

    if (selected_device != nullptr && details_node_ == selected_device->node) {
      // Labelled rows 23px tall and 4px apart under the heading, a 72px
      // label column, 9px in from the column's sides.
      gui::Metrics m = ctx.theme().metrics;
      m.inset = 9.0F;
      m.gap = 31.0F;
      m.control = details_row_height;
      m.row_gap = 4.0F;
      m.label_column = details_label_width;
      gui::FormLayout form{details, m};
      const auto labelled_bounds = [&](std::string_view label) {
        const gui::FormRow row = form.row();
        ctx.label(row.label, label, ctx.theme().text_muted);
        return row.field;
      };
      // A line of text across the column, `height` tall and `gap` above
      // whatever follows.
      const auto line = [&](float height, float gap) {
        return form.next(height, gap);
      };
      const gui::UiId name_id =
          mix(id_, "hardware.details.name", details_node_.value());
      const gui::Rect name_bounds = labelled_bounds("Name");
      if (ctx.text_field(name_id, name_bounds, details_name_)) {
        details_name_dirty_ = true;
      }
      const gui::UiId address_id =
          mix(id_, "hardware.details.address", details_node_.value());
      const bool render_server =
          selected_device->kind == topology::HardwareDeviceKind::RenderServer;
      const gui::Rect address_bounds =
          labelled_bounds(render_server ? "Host IP" : "Address");
      if (ctx.text_field(address_id, address_bounds, details_address_)) {
        details_address_dirty_ = true;
      }
      if (render_server && details_address_.empty() &&
          ctx.focus() != address_id) {
        ctx.label(gui::inset(address_bounds, 6.0F, 0.0F),
                  "LOCAL (127.0.0.1)", ctx.theme().text_muted);
      }
      if (render_server) {
        const gui::Rect target_bounds = labelled_bounds("Control");
        ctx.panel(target_bounds, ctx.theme().track);
        ctx.outline(target_bounds, ctx.theme().divider);
        const std::optional<std::string> target =
            topology::lifecycle_endpoint_for(map, *selected_device);
        ctx.push_clip(target_bounds);
        ctx.label(gui::inset(target_bounds, 6.0F, 0.0F),
                  target.has_value() ? std::string_view{*target}
                                     : std::string_view{"UNAVAILABLE"},
                  target.has_value() ? ctx.theme().text_muted
                                     : ctx.theme().warning);
        ctx.pop_clip();
      }

      const auto emit_number = [&](std::string_view label,
                                   std::string_view property, core::u64 salt,
                                   core::u32 value, core::u32 minimum,
                                   core::u32 maximum) {
        const gui::Rect field = labelled_bounds(label);
        double changed{};
        if (!details_numbers_.draw(ctx,
                                   mix(id_, "hardware.details.number",
                                       details_node_.value() * 8U + salt),
                                   field, {}, static_cast<double>(value), 1.0,
                                   changed, 0)) {
          return;
        }
        const auto rounded = static_cast<core::i64>(
            std::llround(std::clamp(changed, static_cast<double>(minimum),
                                    static_cast<double>(maximum))));
        frame.property_edit = HardwarePropertyEdit{
            details_node_, std::string{property}, doc::PropertyValue{rounded}};
      };
      const auto emit_signed_number = [&](std::string_view label,
                                          std::string_view property,
                                          core::u64 salt, core::i32 value) {
        const gui::Rect field = labelled_bounds(label);
        double changed{};
        if (!details_numbers_.draw(ctx,
                                   mix(id_, "hardware.details.number",
                                       details_node_.value() * 8U + salt),
                                   field, {}, static_cast<double>(value), 1.0,
                                   changed, 0)) {
          return;
        }
        const auto rounded = static_cast<core::i64>(std::llround(std::clamp(
            changed, static_cast<double>(std::numeric_limits<core::i32>::min()),
            static_cast<double>(std::numeric_limits<core::i32>::max()))));
        frame.property_edit = HardwarePropertyEdit{
            details_node_, std::string{property}, doc::PropertyValue{rounded}};
      };
      bool explicit_output_bindings = false;
      if (render_server) {
        emit_number("Node index", topology::schema::node_index, 4U,
                    selected_device->node_index, 0U,
                    topology::max_hardware_node_index);
        {
          // Whose playhead the Timeline draws beside the editor's.
          const gui::Rect main_bounds = labelled_bounds("Timeline");
          bool main = selected_device->main_node;
          if (ctx.checkbox(mix(id_, "hardware.details.main-node",
                               details_node_.value()),
                           main_bounds, main, "Main node")) {
            frame.property_edit = HardwarePropertyEdit{
                details_node_, std::string{topology::schema::main_node},
                doc::PropertyValue{main}};
          }
          ctx.tooltip(main_bounds,
                      "Show this node's playhead on the Timeline. One node "
                      "per show; choosing it clears the others.");
        }
        std::vector<const topology::HardwareOutputBinding *> bindings;
        for (const topology::HardwareOutputBinding &binding :
             map.output_bindings) {
          if (binding.render_server == selected_device->node) {
            bindings.push_back(&binding);
          }
        }
        explicit_output_bindings = !bindings.empty();
        if (details_output_binding_ != doc::NodeId{} &&
            map.output_binding(details_output_binding_) == nullptr) {
          details_output_binding_ = {};
        }

        const topology::HardwareRuntimeNode *const deployment_runtime =
            runtime.node(selected_device->node);
        const auto target_for = [&](const topology::HardwareDisplayTargetKey &key)
            -> const topology::HardwareVideoOutput * {
          if (deployment_runtime == nullptr ||
              !runtime_inventory_is_usable(deployment_runtime)) {
            return nullptr;
          }
          for (const topology::HardwareVideoOutput &candidate :
               deployment_runtime->video_outputs) {
            if (candidate.target == key) {
              return &candidate;
            }
          }
          return nullptr;
        };
        const auto next_unbound_target = [&]()
            -> const topology::HardwareVideoOutput * {
          if (deployment_runtime == nullptr ||
              !runtime_inventory_is_usable(deployment_runtime)) {
            return nullptr;
          }
          for (const topology::HardwareVideoOutput &candidate :
               deployment_runtime->video_outputs) {
            const bool bound = std::any_of(
                bindings.begin(), bindings.end(),
                [&](const topology::HardwareOutputBinding *binding) {
                  return binding->target == candidate.target;
                });
            if (candidate.active && candidate.available &&
                candidate.target.valid() && !bound) {
              return &candidate;
            }
          }
          return nullptr;
        };
        ctx.label(line(19.0F, 1.0F), "OUTPUT DEPLOYMENT",
                  ctx.theme().text_muted);
        for (const topology::HardwareOutputBinding *binding : bindings) {
          const topology::HardwareVideoOutput *const observed =
              target_for(binding->target);
          const bool current = observed != nullptr && observed->active &&
              observed->available;
          const bool mode_matches = !current ||
              binding->window_mode !=
                  topology::HardwareWindowMode::Fullscreen ||
              (observed->width == binding->width &&
               observed->height == binding->height &&
               observed->refresh_numerator == binding->refresh_numerator &&
               std::max(observed->refresh_denominator, core::u32{1U}) ==
                   binding->refresh_denominator);
          const std::string label =
              "PORT " + std::to_string(binding->logical_port) +
              "  FEED " + std::to_string(binding->feed_index) +
              (!current ? "  MISSING"
                        : (mode_matches ? "  READY" : "  MODE MISMATCH"));
          const gui::Rect row = line(22.0F, 2.0F);
          if (ctx.button(mix(id_, "hardware.binding.select",
                             binding->node.value()),
                         row, label)) {
            details_output_binding_ = binding->node;
          }
        }
        ctx.label(line(19.0F, 3.0F),
                  deployment_runtime == nullptr ||
                          !runtime_inventory_is_usable(deployment_runtime)
                      ? "START LISTENER, THEN WIRE VIDEO OUT"
                      : "WIRE VIDEO OUT TO CREATE OUTPUT",
                  deployment_runtime == nullptr ||
                          !runtime_inventory_is_usable(deployment_runtime)
                      ? ctx.theme().warning
                      : ctx.theme().text_muted);

        const topology::HardwareOutputBinding *const selected_binding =
            map.output_binding(details_output_binding_);
        if (selected_binding != nullptr &&
            selected_binding->render_server == selected_device->node) {
          topology::HardwareOutputBinding changed_binding = *selected_binding;
          const auto emit_binding_number =
              [&](std::string_view label, core::u64 salt, core::u32 value,
                  core::u32 minimum, core::u32 maximum,
                  core::u32 &destination) {
            const gui::Rect field = labelled_bounds(label);
            double changed{};
            if (details_numbers_.draw(
                    ctx,
                    mix(id_, "hardware.binding.number",
                        selected_binding->node.value() * 8U + salt),
                    field, {}, static_cast<double>(value), 1.0, changed, 0)) {
              destination = static_cast<core::u32>(std::llround(std::clamp(
                  changed, static_cast<double>(minimum),
                  static_cast<double>(maximum))));
              frame.output_binding_edit = HardwareOutputBindingEdit{
                  changed_binding};
            }
          };
          emit_binding_number("Feed", 0U, selected_binding->feed_index, 0U,
                              topology::max_hardware_node_index,
                              changed_binding.feed_index);
          const gui::Rect binding_mode = labelled_bounds("Mode");
          if (ctx.button(mix(id_, "hardware.binding.mode",
                             selected_binding->node.value()),
                         binding_mode,
                         window_mode_label(selected_binding->window_mode))) {
            switch (selected_binding->window_mode) {
            case topology::HardwareWindowMode::Windowed:
              changed_binding.window_mode =
                  topology::HardwareWindowMode::Borderless;
              break;
            case topology::HardwareWindowMode::Borderless:
              changed_binding.window_mode =
                  topology::HardwareWindowMode::Fullscreen;
              break;
            case topology::HardwareWindowMode::Fullscreen:
            case topology::HardwareWindowMode::Inherit:
              changed_binding.window_mode =
                  topology::HardwareWindowMode::Windowed;
              break;
            }
            frame.output_binding_edit = HardwareOutputBindingEdit{
                changed_binding};
          }
          const auto precision_field = labelled_bounds("Precision");
          if (ctx.button(mix(id_, "hardware.binding.precision", selected_binding->node.value()),
              precision_field, selected_binding->output_bit_depth == 10U ? "10-BIT RGB"
                : selected_binding->output_bit_depth == 16U ? "16-FLOAT LINEAR" : "8-BIT RGB")) {
            changed_binding.output_bit_depth = selected_binding->output_bit_depth == 8U ? 10U
                : selected_binding->output_bit_depth == 10U ? 16U : 8U;
            frame.output_binding_edit = HardwareOutputBindingEdit{changed_binding};
          }
          emit_binding_number("Width", 1U, selected_binding->width, 1U,
                              topology::max_hardware_output_width,
                              changed_binding.width);
          emit_binding_number("Height", 2U, selected_binding->height, 1U,
                              topology::max_hardware_output_height,
                              changed_binding.height);
          emit_binding_number("Refresh", 3U,
                              selected_binding->refresh_numerator, 1U,
                              240'000U,
                              changed_binding.refresh_numerator);
          emit_binding_number("Rate div", 4U,
                              selected_binding->refresh_denominator, 1U,
                              1001U, changed_binding.refresh_denominator);

          const topology::HardwareVideoOutput *const replacement =
              next_unbound_target();
          if (target_for(selected_binding->target) == nullptr &&
              replacement != nullptr) {
            const gui::Rect repair = line(22.0F, 3.0F);
            if (ctx.button(mix(id_, "hardware.binding.repair",
                               selected_binding->node.value()),
                           repair, "REPAIR WITH NEXT ACTIVE OUTPUT")) {
              changed_binding.target = replacement->target;
              changed_binding.width = replacement->width;
              changed_binding.height = replacement->height;
              changed_binding.refresh_numerator =
                  replacement->refresh_numerator;
              changed_binding.refresh_denominator =
                  std::max(replacement->refresh_denominator, 1U);
              frame.output_binding_edit = HardwareOutputBindingEdit{
                  changed_binding};
            }
          }
          ctx.label(line(19.0F, 3.0F), "DISCONNECT CABLE TO REMOVE OUTPUT",
                    ctx.theme().text_muted);
        }

        if (!explicit_output_bindings) {
          const gui::Rect mode_bounds = labelled_bounds("Window");
          if (ctx.button(
                  mix(id_, "hardware.details.window-mode",
                      details_node_.value()),
                  mode_bounds,
                  window_mode_label(selected_device->window_mode))) {
            const topology::HardwareWindowMode next =
                next_window_mode(selected_device->window_mode);
            frame.property_edit = HardwarePropertyEdit{
                details_node_, std::string{topology::schema::window_mode},
                doc::PropertyValue{std::string{window_mode_property(next)}}};
          }
          const std::string_view settings_source =
              hardware_window_settings_source_text(
                  selected_device->window_mode);
          ctx.label(form.note(), settings_source,
                    selected_device->window_mode ==
                            topology::HardwareWindowMode::Inherit
                        ? ctx.theme().warning
                        : ctx.theme().text_muted);
          emit_signed_number("Desktop X", topology::schema::desktop_x, 5U,
                             selected_device->desktop_x);
          emit_signed_number("Desktop Y", topology::schema::desktop_y, 6U,
                             selected_device->desktop_y);
        }
      }
      if (!explicit_output_bindings) {
        const core::u32 maximum_width =
            render_server ? topology::max_hardware_output_width : 32'768U;
        const core::u32 maximum_height =
            render_server ? topology::max_hardware_output_height : 32'768U;
        emit_number("Width", topology::schema::width, 0U,
                    selected_device->width, 1U, maximum_width);
        emit_number("Height", topology::schema::height, 1U,
                    selected_device->height, 1U, maximum_height);
        emit_number("Refresh", topology::schema::refresh_numerator, 2U,
                    selected_device->refresh_numerator, 1U, 240'000U);
        emit_number("Rate div", topology::schema::refresh_denominator, 3U,
                    selected_device->refresh_denominator, 1U, 1001U);
      }

      if (render_server) {
        form.space(3.0F);
        ctx.panel(line(1.0F, 4.0F), ctx.theme().divider);
        ctx.label(line(19.0F, 1.0F), "LIVE SYSTEM", ctx.theme().text_muted);

        const auto live_row = [&](std::string_view label,
                                  std::string_view value, gui::UiColor color) {
          if (form.cursor() + 19.0F > details.bottom() - 5.0F) {
            return false;
          }
          gui::Rect value_bounds = line(19.0F, 1.0F);
          ctx.label(gui::cut_left(value_bounds, details_label_width), label,
                    ctx.theme().text_muted);
          ctx.label(value_bounds, value, color);
          return true;
        };

        const HardwareDevicePresentation live_presentation =
            hardware_presentation(map, *selected_device, runtime);
        const topology::HardwareRuntimeNode *const live =
            live_presentation.runtime;
        if (live == nullptr) {
          static_cast<void>(
              live_row("Status", "UNMONITORED", ctx.theme().text_muted));
          static_cast<void>(
              live_row("Outputs", "OUTPUTS UNKNOWN", ctx.theme().text_muted));
          static_cast<void>(live_row("Info", "No authenticated telemetry",
                                     ctx.theme().text_muted));
        } else {
          const std::string status = runtime_card_status(live_presentation);
          static_cast<void>(
              live_row("Status", status,
                       runtime_status_color(ctx.theme(), live_presentation)));
          if (!live->hostname.empty()) {
            static_cast<void>(
                live_row("Host", live->hostname, ctx.theme().text));
          }
          if (!live->operating_system.empty()) {
            static_cast<void>(
                live_row("OS", live->operating_system, ctx.theme().text_muted));
          }
          if (live->logical_processor_count != 0U) {
            const std::string cpu =
                std::to_string(live->logical_processor_count) + " logical";
            static_cast<void>(live_row("CPU", cpu, ctx.theme().text_muted));
          }
          if (live->total_memory_bytes != 0U) {
            const std::string memory = capacity_text(live->total_memory_bytes);
            static_cast<void>(live_row("RAM", memory, ctx.theme().text_muted));
          }

          constexpr std::size_t maximum_gpu_rows = 2U;
          const std::size_t gpu_rows =
              std::min(live->gpu_adapters.size(), maximum_gpu_rows);
          for (std::size_t index = 0U; index < gpu_rows; ++index) {
            const topology::HardwareGpuAdapter &adapter =
                live->gpu_adapters[index];
            std::string gpu =
                adapter.name.empty() ? "Unknown GPU" : adapter.name;
            if (adapter.dedicated_video_memory_bytes != 0U) {
              gpu +=
                  " | " + capacity_text(adapter.dedicated_video_memory_bytes);
            }
            if (adapter.software) {
              gpu += " | SOFTWARE";
            } else if (adapter.remote) {
              gpu += " | REMOTE";
            }
            if (!live_row(index == 0U ? "GPU" : "", gpu,
                          adapter.software ? ctx.theme().warning
                                           : ctx.theme().text_muted)) {
              break;
            }
          }
          if (live->gpu_adapters.size() > gpu_rows) {
            const std::string more =
                "+" + std::to_string(live->gpu_adapters.size() - gpu_rows) +
                " more adapters";
            static_cast<void>(live_row("", more, ctx.theme().text_muted));
          }

          const std::string outputs = runtime_output_status(live_presentation);
          const bool output_warning =
              live->inventory ==
                  topology::HardwareRuntimeInventoryState::Failed ||
              live->inventory ==
                  topology::HardwareRuntimeInventoryState::Unsupported;
          static_cast<void>(live_row("Outputs", outputs,
                                     output_warning ? ctx.theme().warning
                                                    : ctx.theme().text_muted));
          constexpr std::size_t maximum_output_rows = 2U;
          std::size_t shown_outputs{};
          if (runtime_inventory_is_usable(live)) {
            for (const topology::HardwareVideoOutput &output :
                 live->video_outputs) {
              if (!output.active || shown_outputs >= maximum_output_rows) {
                continue;
              }
              const std::string mode = output_mode_text(output);
              if (!live_row("", mode, ctx.theme().text_muted)) {
                break;
              }
              ++shown_outputs;
            }
          }
          if (live_presentation.active_video_output_count > shown_outputs) {
            const std::string more =
                "+" +
                std::to_string(live_presentation.active_video_output_count -
                               shown_outputs) +
                " more outputs";
            static_cast<void>(live_row("", more, ctx.theme().text_muted));
          }
        }
      }

      if (details_name_dirty_ && ctx.focus() != name_id) {
        frame.property_edit = HardwarePropertyEdit{
            details_node_, std::string{topology::schema::name},
            doc::PropertyValue{details_name_}};
        details_name_dirty_ = false;
      } else if (details_address_dirty_ && ctx.focus() != address_id) {
        frame.property_edit = HardwarePropertyEdit{
            details_node_, std::string{topology::schema::address},
            doc::PropertyValue{details_address_}};
        details_address_dirty_ = false;
      }
      frame.editing = details_numbers_.live() || ctx.focus() == name_id ||
                      ctx.focus() == address_id;
    } else if (selection_.kind == HardwareSelectionKind::Link) {
      const topology::HardwareLink *const link = map.link(selection_.node);
      gui::Rect lines =
          gui::inset(details, gui::Padding{10.0F, 34.0F, 10.0F, 0.0F});
      ctx.label(gui::cut_top(lines, 22.0F), "CABLE", ctx.theme().text);
      static_cast<void>(gui::cut_top(lines, 3.0F));
      if (link != nullptr) {
        const std::string route = std::to_string(link->from.device.value()) +
                                  ":" + std::to_string(link->from.port) +
                                  "  ->  " +
                                  std::to_string(link->to.device.value()) +
                                  ":" + std::to_string(link->to.port);
        ctx.label(gui::cut_top(lines, 22.0F), route, ctx.theme().keyframe);
      }
    } else {
      gui::Rect message =
          gui::inset(details, gui::Padding{10.0F, 35.0F, 10.0F, 0.0F});
      ctx.label(gui::cut_top(message, 44.0F), "Select a device or cable",
                ctx.theme().text_muted);
    }
  }

  frame.dragging = drag_kind_ != DragKind::None || link_dragging_;
  frame.editing = frame.editing || settings_numbers_.live() ||
                  canvas_numbers_.live() || sync_numbers_.live();
  ctx.pop_clip();
  return frame;
}

gui::Rect fit_feed_canvas(const gui::Rect &area,
                          scene::Canvas canvas) noexcept {
  if (area.width <= 0.0F || area.height <= 0.0F || canvas.width == 0U ||
      canvas.height == 0U) {
    return gui::Rect{area.x + area.width * 0.5F, area.y + area.height * 0.5F,
                     0.0F, 0.0F};
  }
  const float scale_factor =
      std::min(area.width / static_cast<float>(canvas.width),
               area.height / static_cast<float>(canvas.height));
  const float width = static_cast<float>(canvas.width) * scale_factor;
  const float height = static_cast<float>(canvas.height) * scale_factor;
  return gui::Rect{area.x + (area.width - width) * 0.5F,
                   area.y + (area.height - height) * 0.5F, width, height};
}

gui::Rect feed_mapping_canvas(const gui::Rect &bounds,
                              scene::Canvas canvas) noexcept {
  const float list_width = feed_list_width(bounds);
  const float details_width = feed_details_width(bounds);
  const gui::Rect area{
      bounds.x + list_width + 22.0F, bounds.y + title_height + 30.0F,
      std::max(bounds.width - list_width - details_width - 44.0F, 0.0F),
      std::max(bounds.height - title_height - 132.0F, 0.0F)};
  return fit_feed_canvas(area, canvas);
}

gui::Rect feed_map_screen_rect(const gui::Rect &fitted_canvas,
                               scene::Canvas canvas,
                               scene::CanvasRect source) noexcept {
  if (canvas.width == 0U || canvas.height == 0U ||
      fitted_canvas.width <= 0.0F || fitted_canvas.height <= 0.0F) {
    return gui::Rect{fitted_canvas.x, fitted_canvas.y, 0.0F, 0.0F};
  }
  const float scale_x = fitted_canvas.width / static_cast<float>(canvas.width);
  const float scale_y =
      fitted_canvas.height / static_cast<float>(canvas.height);
  return gui::Rect{fitted_canvas.x + static_cast<float>(source.x) * scale_x,
                   fitted_canvas.y + static_cast<float>(source.y) * scale_y,
                   static_cast<float>(source.width) * scale_x,
                   static_cast<float>(source.height) * scale_y};
}

std::string feed_output_identity(core::u32 node_index) {
  std::array<char, 64U> text{};
  const int length = std::snprintf(
      text.data(), text.size(), "FEED %02u  FEED INDEX %u",
      node_index + 1U, node_index);
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

std::string feed_runtime_binding(core::u32 node_index) {
  std::array<char, 96U> text{};
  const int length = std::snprintf(
      text.data(), text.size(),
      "ASSIGN IN HARDWARE MAP  |  FEED INDEX %u", node_index);
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

FeedMappingFrame FeedMappingPanel::draw(gui::UiContext &ctx,
                                        const gui::Rect &bounds,
                                        scene::Canvas canvas,
                                        const output::FeedMap &feed_map,
                                        core::u64 show_frame,
                                        const topology::HardwareMap *hardware) {
  FeedMappingFrame frame{};
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    rectangle_numbers_.cancel(ctx);
    return frame;
  }
  if (ctx.pointer_pressed()) {
    focused_ = bounds.contains(ctx.pointer_x(), ctx.pointer_y());
  }
  if (selected_map_node_.has_value() &&
      feed_map.map_node(*selected_map_node_) == nullptr) {
    selected_map_node_.reset();
    drag_kind_ = DragKind::None;
    rectangle_numbers_.cancel(ctx);
  }
  if (selected_surface_node_.has_value() &&
      feed_map.surface_for(*selected_surface_node_) == nullptr) {
    selected_surface_node_.reset();
    surface_details_node_ = {};
    surface_details_show_frame_ = 0U;
    surface_name_.clear();
    surface_name_dirty_ = false;
    mesh_browse_pending_ = false;
    surface_numbers_.cancel(ctx);
  }
  if (!selected_surface_node_.has_value() && !feed_map.surfaces.empty()) {
    const output::OutputMap *const selected_map =
        selected_map_node_.has_value()
            ? feed_map.map_node(*selected_map_node_)
            : nullptr;
    const output::ProjectionSurface *const assigned =
        selected_map != nullptr && selected_map->projection ==
                                       output::FeedProjection::PlanarSurface
            ? feed_map.surface_for(selected_map->surface)
            : nullptr;
    selected_surface_node_ =
        assigned != nullptr ? assigned->node : feed_map.surfaces.front().node;
  }

  const auto sync_surface_details = [&] {
    const output::ProjectionSurface *const surface =
        surface_or_null(feed_map, selected_surface_node_);
    const doc::NodeId next = surface != nullptr ? surface->node : doc::NodeId{};
    if (next == surface_details_node_) {
      if (surface != nullptr && surface->name != surface_name_ &&
          !surface_name_dirty_ &&
          ctx.focus() != mix(id_, "feed.surface.name", next.value())) {
        surface_name_ = surface->name;
      }
      const auto camera_field = mix(id_, "feed.surface.camera-inverse", next.value());
      if (surface != nullptr && surface->color != surface_color_observed_
          && !surface_numbers_.live() && ctx.focus() != camera_field) {
        // Follow undo/redo without reverting a local gesture while its command
        // is still travelling to the replicated document.
        surface_color_ = surface->color;
        surface_color_observed_ = surface->color;
      }
      // Native mesh browsing is resolved synchronously by EditorShell but the
      // resulting document reaches this value panel on a later frame. Reconcile
      // that one externally-filled value without re-basing ordinary live
      // numeric scrubs on the lagging document.
      if (mesh_browse_pending_ && surface != nullptr &&
          surface->shape == output::ProjectionSurfaceShape::ImportedMesh &&
          !surface->mesh_source.empty() &&
          surface->mesh_source != surface_mesh_source_) {
        surface_name_ = surface->name;
        surface_geometry_ = surface->geometry;
        surface_shape_ = surface->shape;
        surface_horizontal_arc_turns_ = surface->horizontal_arc_turns;
        surface_mesh_columns_ = surface->mesh_columns;
        surface_mesh_source_ = surface->mesh_source;
        surface_mesh_scale_ = surface->mesh_scale;
        surface_details_show_frame_ = show_frame;
        surface_name_dirty_ = false;
        mesh_browse_pending_ = false;
      }
      if (surface != nullptr && show_frame != surface_details_show_frame_ &&
          !surface_numbers_.live()) {
        // The feed model supplies evaluated centre/yaw at the playhead. Follow
        // timeline motion while idle, but never re-base a live scrub on a
        // document value that may still be a few replicated frames behind.
        surface_geometry_.center = surface->geometry.center;
        surface_geometry_.right = surface->geometry.right;
        surface_geometry_.up = surface->geometry.up;
        surface_details_show_frame_ = show_frame;
      }
      return;
    }
    if (surface_details_node_ != doc::NodeId{} && surface_name_dirty_ &&
        !(surface_shape_ == output::ProjectionSurfaceShape::ImportedMesh &&
          surface_mesh_source_.empty())) {
      frame.surface_edit = FeedSurfaceEditRequest{
          surface_details_node_, surface_name_,
          surface_geometry_,     0U,
          surface_shape_,        surface_horizontal_arc_turns_,
          surface_mesh_columns_, surface_mesh_source_,
          surface_mesh_scale_,   FeedSurfaceTransform::None};
    }
    surface_numbers_.cancel(ctx);
    ctx.clear_focus();
    surface_details_node_ = next;
    surface_details_show_frame_ = show_frame;
    surface_name_ = surface != nullptr ? surface->name : std::string{};
    surface_geometry_ =
        surface != nullptr ? surface->geometry : output::PhysicalSurface{};
    surface_shape_ = surface != nullptr
                         ? surface->shape
                         : output::ProjectionSurfaceShape::Planar;
    surface_horizontal_arc_turns_ =
        surface != nullptr ? surface->horizontal_arc_turns : 0.25F;
    surface_mesh_columns_ = surface != nullptr ? surface->mesh_columns : 32U;
    surface_mesh_source_ =
        surface != nullptr ? surface->mesh_source : std::string{};
    surface_mesh_scale_ = surface != nullptr ? surface->mesh_scale : 1.0F;
    surface_color_ = surface != nullptr ? surface->color : scene::WallColorSettings{};
    surface_color_observed_ = surface_color_;
    mesh_browse_pending_ = false;
    surface_name_dirty_ = false;
  };
  sync_surface_details();

  ctx.push_clip(bounds);
  ctx.panel(bounds);
  const bool valid_selection =
      selected_map_node_.has_value() &&
      feed_map.map_node(*selected_map_node_) != nullptr;
  title(ctx, bounds, "FEED MAPPING",
        valid_selection
            ? std::string_view{}
            : (feed_map.maps.empty() ? std::string_view{"NOT CONFIGURED"}
                                     : std::string_view{"CONFIGURED"}));
  if (focused_ && valid_selection && !ctx.typing() &&
      !surface_numbers_.live() && !rectangle_numbers_.live() &&
      ctx.key_pressed(gui::Key::Delete)) {
    frame.remove = FeedMapDeleteRequest{*selected_map_node_};
    frame.consume_delete = true;
  }

  gui::Rect content = bounds;
  static_cast<void>(gui::cut_top(content, title_height));
  const gui::Rect list = gui::cut_left(content, feed_list_width(bounds));
  const gui::Rect work = content;
  ctx.panel(list, scale(ctx.theme().panel, 0.94F));
  {
    gui::Rect edge = list;
    ctx.panel(gui::cut_right(edge, 1.0F), ctx.theme().divider);
  }
  // Down the list column: buttons and rows 6px in from its sides, headings
  // 3px further in.
  gui::Metrics column_metrics = ctx.theme().metrics;
  column_metrics.inset = 6.0F;
  column_metrics.gap = 0.0F;
  gui::FormLayout list_column{list, column_metrics};
  ctx.label(gui::inset(list_column.next(feed_list_header_height, 0.0F), 3.0F, 0.0F),
            "WINDOW VIEWPORTS", ctx.theme().text_muted);
  // A full-width button that adds to the list, dimmed when it cannot.
  const auto add_button = [&](gui::UiId id, const gui::Rect &button,
                              std::string_view label, bool enabled) {
    const gui::Interaction interaction =
        enabled ? ctx.grab(id, button) : gui::Interaction{};
    ctx.panel(button, enabled ? (interaction.hovered ? ctx.theme().widget_hover
                                                     : ctx.theme().widget)
                              : ctx.theme().track);
    ctx.outline(button, enabled && interaction.hovered ? ctx.theme().accent
                                                       : ctx.theme().divider);
    ctx.label(button, label,
              enabled ? ctx.theme().text : ctx.theme().text_muted);
    return interaction.clicked;
  };
  // The colour bar down a row's left edge.
  const auto row_stripe = [](gui::Rect row) {
    return gui::inset(gui::cut_left(row, 5.0F),
                      gui::Padding{2.0F, 4.0F, 0.0F, 4.0F});
  };
  // The rule left of a row's action button.
  const auto action_rule = [](gui::Rect action, float inset) {
    return gui::inset(gui::cut_left(action, 1.0F), 0.0F, inset);
  };

  const output::OutputMap *const selected_viewport =
      selected_map_node_.has_value()
          ? feed_map.map_node(*selected_map_node_)
          : nullptr;
  std::optional<core::u32> add_index;
  if (selected_viewport != nullptr) {
    add_index = selected_viewport->node_index;
  } else if (hardware != nullptr && !hardware->output_bindings.empty()) {
    add_index = hardware->output_bindings.front().feed_index;
  } else if (!feed_map.maps.empty()) {
    add_index = feed_map.maps.front().node_index;
  } else {
    add_index = 0U;
  }
  const bool below_limit = feed_map.maps.size() < output::max_output_maps;
  const bool can_add = canvas.width > 0U && canvas.height > 0U && below_limit;
  const gui::Rect add_bounds = list_column.next(
      feed_add_button_height, feed_add_button_step - feed_add_button_height);
  if (add_button(mix(id_, "feed.add"), add_bounds,
                 !below_limit
                     ? std::string_view{"VIEWPORT LIMIT"}
                     : (can_add ? std::string_view{"+ ADD VIEWPORT"}
                                : std::string_view{"CANVAS REQUIRED"}),
                 can_add)) {
    const scene::Canvas window =
        feed_window_raster(*add_index, canvas, hardware);
    const scene::CanvasRect source =
        selected_viewport != nullptr
            ? selected_viewport->source
            : scene::CanvasRect{0U, 0U, canvas.width, canvas.height};
    scene::CanvasRect viewport =
        selected_viewport != nullptr ? selected_viewport->viewport
                                     : scene::whole(window);
    core::u32 rightmost = 0U;
    for (const output::OutputMap &map : feed_map.maps_for(*add_index)) {
      rightmost = std::max(rightmost, map.viewport.x + map.viewport.width);
    }
    if (rightmost <= window.width && viewport.width <= window.width - rightmost) {
      viewport.x = rightmost;
    }
    frame.add = FeedMapAddRequest{
        *add_index, source, viewport};
  }

  // A surface is assigned by dragging it out of the list onto the output it
  // drives. Both views of an output take the drop - its row here and its
  // rectangle in the work area - so the gesture ends wherever the operator is
  // already looking. Targets are collected as they are drawn and resolved once,
  // after the whole panel has laid out.
  if (surface_drag_node_ != doc::NodeId{} &&
      feed_map.surface_for(surface_drag_node_) == nullptr) {
    surface_drag_node_ = {};
    surface_drag_moved_ = false;
  }
  // The threshold is measured here rather than in the row loop below, so the
  // targets light up on the frame the drag starts moving and not the one after.
  if (surface_drag_node_ != doc::NodeId{} && ctx.pointer_down()) {
    const float dx = ctx.pointer_x() - surface_drag_start_x_;
    const float dy = ctx.pointer_y() - surface_drag_start_y_;
    surface_drag_moved_ = surface_drag_moved_ || dx * dx + dy * dy >= 16.0F;
  }
  const bool surface_drag_active =
      surface_drag_node_ != doc::NodeId{} && surface_drag_moved_;
  std::vector<std::pair<gui::Rect, doc::NodeId>> surface_drop_targets;
  surface_drop_targets.reserve(feed_map.maps.size() * 2U);

  // A viewport row dragged up or down its window's rows reorders it there,
  // which is also its compositing order. Only its own window's rows are
  // targets: moving between windows is a placement change, not a reorder.
  struct ViewportRow final {
    gui::Rect bounds;
    doc::NodeId node;
    core::u32 window{};
  };
  std::vector<ViewportRow> viewport_rows;
  viewport_rows.reserve(feed_map.maps.size());
  if (row_drag_node_ != doc::NodeId{} &&
      feed_map.map_node(row_drag_node_) == nullptr) {
    row_drag_node_ = {};
    row_drag_moved_ = false;
  }
  if (row_drag_node_ != doc::NodeId{} && ctx.pointer_down()) {
    row_drag_moved_ = row_drag_moved_ ||
                      std::abs(ctx.pointer_y() - row_drag_start_y_) >= 4.0F;
  }
  // F2 names whichever row was clicked last, as in every layer list.
  if (focused_ && !ctx.typing() && !rename_.active() &&
      ctx.key_pressed(gui::Key::Rename)) {
    if (const output::OutputMap *const map = feed_map.map_node(rename_row_)) {
      rename_.begin(ctx, map->node.value(),
                    feed_viewport_title(*map,
                                        feed_viewport_ordinal(feed_map, *map)));
    } else if (const output::ProjectionSurface *const surface =
                   feed_map.surface_for(rename_row_);
               surface != nullptr && !surface->locked) {
      rename_.begin(ctx, surface->node.value(), surface->name);
    }
  }

  for (std::size_t index = 0U; index < feed_map.maps.size(); ++index) {
    const output::OutputMap &map = feed_map.maps[index];
    const core::u32 viewport_ordinal =
        feed_viewport_ordinal(feed_map, map);
    const std::string title = feed_viewport_title(map, viewport_ordinal);
    const gui::Rect row =
        list_column.next(feed_row_height, feed_row_step - feed_row_height);
    // The row's name and details, then DUPLICATE and REMOVE at its end.
    const float action_width = std::min(26.0F, row.width * 0.25F);
    gui::Rect row_content = row;
    const gui::Rect remove = gui::cut_right(row_content, action_width);
    const gui::Rect duplicate = gui::cut_right(row_content, action_width);
    const bool renaming = rename_.editing(map.node.value());
    const gui::Interaction interaction =
        renaming ? gui::Interaction{}
                 : ctx.grab(mix(id_, "feed.row", map.node.value()),
                            row_content);
    const gui::Interaction duplicate_interaction = ctx.grab(
        mix(id_, "feed.row.duplicate", map.node.value()), duplicate);
    const gui::Interaction remove_interaction =
        ctx.grab(mix(id_, "feed.row.remove", map.node.value()), remove);
    const bool selected = selected_map_node_ == map.node;
    ctx.panel(row,
              selected ? ctx.theme().selection
                       : (interaction.hovered || remove_interaction.hovered ||
                                  duplicate_interaction.hovered
                              ? ctx.theme().widget_hover
                              : ctx.theme().row));
    ctx.panel(row_stripe(row), feed_color(ctx.theme(), map.node_index));
    const gui::Rect title_bounds = gui::inset(
        row_content, gui::Padding{10.0F, 1.0F, 4.0F, row.height - 18.0F});
    if (renaming) {
      if (auto name = rename_.draw(ctx, title_bounds)) {
        frame.organize = FeedOrganizeRequest{FeedOrganizeAction::Rename,
                                             map.node, std::move(*name)};
      }
    } else {
      ctx.label(title_bounds, title, ctx.theme().text);
    }
    ctx.label(gui::inset(row_content,
                         gui::Padding{10.0F, 16.0F, 4.0F, row.height - 31.0F}),
              feed_row_details(map), ctx.theme().text_muted);
    ctx.tooltip(row_content, feed_row_tooltip(map, title, feed_map));
    ctx.panel(action_rule(duplicate, 6.0F), ctx.theme().divider);
    gui::draw_icon(ctx.draw_list(), gui::Icon::Duplicate,
                   duplicate.center_x(), duplicate.center_y(),
                   duplicate_interaction.hovered ? ctx.theme().text
                                                 : ctx.theme().text_muted);
    ctx.tooltip(duplicate, "Duplicate viewport");
    draw_remove(ctx, remove.center_x(), remove.center_y(),
                remove_interaction.hovered ? ctx.theme().warning
                                           : ctx.theme().text_muted);
    if (interaction.pressed) {
      row_drag_node_ = map.node;
      row_drag_start_y_ = ctx.pointer_y();
      row_drag_moved_ = false;
    }
    if (interaction.clicked) {
      selected_map_node_ = map.node;
      frame.selection = map.node;
      rename_row_ = map.node;
      if (map.projection == output::FeedProjection::PlanarSurface &&
          feed_map.surface_for(map.surface) != nullptr) {
        selected_surface_node_ = map.surface;
        frame.surface_selection = map.surface;
      }
      if (clicks_.double_click(map.node.value(), ctx.time_seconds())) {
        rename_.begin(ctx, map.node.value(), title);
      }
    }
    if (duplicate_interaction.clicked) {
      frame.organize =
          FeedOrganizeRequest{FeedOrganizeAction::Duplicate, map.node};
    }
    if (remove_interaction.clicked) {
      frame.remove = FeedMapDeleteRequest{map.node};
    }
    viewport_rows.push_back(ViewportRow{row, map.node, map.node_index});
    if (surface_drag_active) {
      surface_drop_targets.emplace_back(row, map.node);
      ctx.outline(row,
                  row.contains(ctx.pointer_x(), ctx.pointer_y())
                 ? ctx.theme().accent
                 : alpha(ctx.theme().accent, 0.35F),
                  row.contains(ctx.pointer_x(), ctx.pointer_y()) ? 2.0F : 1.0F);
    }
  }

  if (row_drag_node_ != doc::NodeId{}) {
    const output::OutputMap *const dragged = feed_map.map_node(row_drag_node_);
    if (row_drag_moved_ && dragged != nullptr) {
      for (const ViewportRow &target : viewport_rows) {
        if (target.window != dragged->node_index ||
            !target.bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
          continue;
        }
        const bool after =
            ctx.pointer_y() >= target.bounds.y + target.bounds.height * 0.5F;
        // Two thick, a pixel clear of the edge it lands on.
        ctx.panel(gui::rule_y(target.bounds,
                              after ? target.bounds.bottom() + 2.0F
                                    : target.bounds.y - 1.0F,
                              2.0F),
                  ctx.theme().accent);
        if (!ctx.pointer_down() && target.node != dragged->node) {
          std::vector<doc::NodeId> current;
          for (const output::OutputMap &map :
               feed_map.maps_for(dragged->node_index)) {
            current.push_back(map.node);
          }
          frame.organize = FeedOrganizeRequest{
              FeedOrganizeAction::Reorder, dragged->node, {},
              reordered(current, dragged->node, target.node, after)};
          if (frame.organize->order == current) {
            frame.organize.reset();
          }
        }
        break;
      }
    }
    if (!ctx.pointer_down()) {
      row_drag_node_ = {};
      row_drag_moved_ = false;
    }
  }

  list_column.space(7.0F);
  ctx.label(gui::inset(list_column.next(feed_details_label_height, 3.0F), 3.0F,
                       0.0F),
            "PROJECTION SURFACES", ctx.theme().text_muted);
  const bool can_add_surface =
      scene::valid(canvas) &&
      feed_map.surfaces.size() < output::max_projection_surfaces;
  if (add_button(mix(id_, "feed.surface.add"),
                 list_column.next(feed_add_button_height,
                             feed_add_button_step - feed_add_button_height),
                 can_add_surface ? std::string_view{"+ ADD SURFACE"}
                                 : std::string_view{"SURFACE LIMIT"},
                 can_add_surface)) {
    frame.surface_add =
        FeedSurfaceAddRequest{default_surface_name(feed_map.surfaces.size()),
                              default_surface(canvas)};
  }
  if (add_button(mix(id_, "feed.surface.import-mesh"),
                 list_column.next(feed_add_button_height,
                             feed_surface_row_step - feed_add_button_height),
                 can_add_surface ? std::string_view{"+ IMPORT UV MESH"}
                                 : std::string_view{"SURFACE LIMIT"},
                 can_add_surface)) {
    FeedSurfaceAddRequest request{
        default_mesh_surface_name(feed_map.surfaces.size()),
        default_mesh_surface(canvas),
        0U,
        output::ProjectionSurfaceShape::ImportedMesh,
        0.25F,
        32U,
        {},
        1.0F};
    request.assign_map_node = selected_map_node_;
    frame.surface_add = std::move(request);
  }
  // Collected for the drag resolution below: a surface dropped on another
  // surface row reorders the list instead of assigning it to an output.
  std::vector<std::pair<gui::Rect, doc::NodeId>> surface_rows;
  surface_rows.reserve(feed_map.surfaces.size());
  for (const output::ProjectionSurface &surface : feed_map.surfaces) {
    const gui::Rect row = list_column.next(
        feed_surface_row_height,
        feed_surface_row_step - feed_surface_row_height);
    // The name, then DUPLICATE, LOCK and REMOVE at the row's end.
    gui::Rect row_content = row;
    const gui::Rect remove = gui::cut_right(row_content, 26.0F);
    const gui::Rect lock = gui::cut_right(row_content, 26.0F);
    const gui::Rect duplicate = gui::cut_right(row_content, 26.0F);
    const bool renaming = rename_.editing(surface.node.value());
    const gui::Interaction interaction =
        surface.locked || renaming
            ? gui::Interaction{}
            : ctx.grab(mix(id_, "feed.surface.row", surface.node.value()),
                       row_content);
    const gui::Interaction duplicate_interaction = ctx.grab(
        mix(id_, "feed.surface.row.duplicate", surface.node.value()),
        duplicate);
    const gui::Interaction lock_interaction = ctx.grab(
        mix(id_, "feed.surface.row.lock", surface.node.value()), lock);
    const gui::Interaction remove_interaction =
        surface.locked
            ? gui::Interaction{}
            : ctx.grab(mix(id_, "feed.surface.row.remove", surface.node.value()),
                       remove);
    const bool selected = selected_surface_node_ == surface.node;
    ctx.panel(row,
              selected ? ctx.theme().selection
                       : (interaction.hovered || lock_interaction.hovered ||
                                  remove_interaction.hovered ||
                                  duplicate_interaction.hovered
                              ? ctx.theme().widget_hover
                              : ctx.theme().row));
    ctx.panel(row_stripe(row),
              selected ? ctx.theme().accent : ctx.theme().keyframe);
    const gui::Rect name_bounds =
        gui::inset(row_content, gui::Padding{10.0F, 0.0F, 4.0F, 0.0F});
    if (renaming) {
      if (auto name = rename_.draw(ctx, name_bounds)) {
        frame.organize = FeedOrganizeRequest{FeedOrganizeAction::Rename,
                                             surface.node, std::move(*name)};
      }
    } else {
      ctx.label(name_bounds,
                surface.name.empty() ? std::string_view{"UNTITLED SURFACE"}
                                     : std::string_view{surface.name},
                surface.locked
                    ? ctx.theme().text_muted
                    : (selected ? ctx.theme().text : ctx.theme().text_muted));
    }
    ctx.tooltip(row_content, surface.locked
                                 ? std::string_view{"Unlock to rename or move"}
                                 : std::string_view{surface.name});
    ctx.panel(action_rule(duplicate, 5.0F), ctx.theme().divider);
    gui::draw_icon(ctx.draw_list(), gui::Icon::Duplicate,
                   duplicate.center_x(), duplicate.center_y(),
                   duplicate_interaction.hovered ? ctx.theme().text
                                                 : ctx.theme().text_muted);
    ctx.tooltip(duplicate, "Duplicate surface");
    ctx.panel(action_rule(lock, 5.0F), ctx.theme().divider);
    gui::draw_padlock_glyph(ctx.draw_list(), lock.center_x(),
                            lock.center_y(), surface.locked,
                            surface.locked || lock_interaction.hovered
                                ? ctx.theme().warning
                                : ctx.theme().text_muted);
    ctx.tooltip(lock, surface.locked ? std::string_view{"Unlock surface"}
                                     : std::string_view{"Lock surface"});
    ctx.panel(action_rule(remove, 5.0F), ctx.theme().divider);
    draw_remove(ctx, remove.center_x(), remove.center_y(),
                remove_interaction.hovered ? ctx.theme().warning
                                           : ctx.theme().text_muted);
    if (interaction.pressed) {
      surface_drag_node_ = surface.node;
      surface_drag_start_x_ = ctx.pointer_x();
      surface_drag_start_y_ = ctx.pointer_y();
      surface_drag_moved_ = false;
    }
    if (interaction.clicked) {
      selected_surface_node_ = surface.node;
      frame.surface_selection = surface.node;
      rename_row_ = surface.node;
      if (clicks_.double_click(surface.node.value(), ctx.time_seconds())) {
        rename_.begin(ctx, surface.node.value(), surface.name);
      }
    }
    if (duplicate_interaction.clicked) {
      frame.organize =
          FeedOrganizeRequest{FeedOrganizeAction::Duplicate, surface.node};
    }
    if (lock_interaction.clicked) {
      frame.surface_lock =
          FeedSurfaceLockRequest{surface.node, !surface.locked};
      if (!surface.locked) {
        selected_surface_node_ = {};
      }
    }
    if (remove_interaction.clicked) {
      frame.surface_remove = FeedSurfaceDeleteRequest{surface.node};
    }
    surface_rows.emplace_back(row, surface.node);
  }
  rename_.end_frame(ctx);
  sync_surface_details();

  ctx.panel(work, scale(ctx.theme().background, 0.76F));
  const output::OutputMap *const selected_map =
      selected_map_node_.has_value() ? feed_map.map_node(*selected_map_node_)
                                     : nullptr;
  const output::ProjectionSurface *const mapping_surface =
      selected_map != nullptr &&
              selected_map->projection == output::FeedProjection::PlanarSurface
          ? feed_map.surface_for(selected_map->surface)
          : nullptr;
  const bool uv_mesh_domain =
      mapping_surface != nullptr &&
      mapping_surface->shape == output::ProjectionSurfaceShape::ImportedMesh;
  // This is a destination-UV preview: stage origin, orientation, and uniform
  // scale cannot change it. Observation/admission is nonblocking; an exact-key
  // stale mesh remains visible while an explicit replacement loads.
  preview_mesh_ = uv_mesh_domain && selected_map != nullptr
                      ? preview_projection_cache_.resolve_mesh(
                            uv_preview_map(*selected_map, *mapping_surface))
                      : std::shared_ptr<const output::ProjectionMesh>{};
  const std::vector<FeedWindowLayout> windows =
      feed_window_layouts(feed_map, canvas, hardware);
  scene::Canvas mapping_canvas{};
  for (const FeedWindowLayout &window : windows) {
    const core::u64 right = static_cast<core::u64>(window.offset_x) +
                            window.raster.width;
    mapping_canvas.width = static_cast<core::u32>(
        std::min<core::u64>(std::max<core::u64>(mapping_canvas.width, right),
                            UINT32_MAX));
    mapping_canvas.height = std::max(mapping_canvas.height,
                                     window.raster.height);
  }
  const gui::Rect fitted = feed_mapping_canvas(bounds, mapping_canvas);
  if (mapping_canvas.width > 0U && mapping_canvas.height > 0U &&
      fitted.width > 0.0F && fitted.height > 0.0F) {
    const float scale_factor =
        fitted.width / static_cast<float>(mapping_canvas.width);
    ctx.panel(fitted, gui::UiColor{0.012F, 0.016F, 0.025F, 1.0F});
    for (const FeedWindowLayout &window : windows) {
      const scene::CanvasRect window_rect{window.offset_x, 0U,
                                          window.raster.width,
                                          window.raster.height};
      const gui::Rect screen =
          feed_map_screen_rect(fitted, mapping_canvas, window_rect);
      ctx.panel(screen, gui::UiColor{0.018F, 0.023F, 0.034F, 1.0F});
      for (core::u32 division = 1U; division < 4U; ++division) {
        const float fraction = static_cast<float>(division) / 4.0F;
        ctx.draw_list().add_line(
            screen.x + screen.width * fraction, screen.y,
            screen.x + screen.width * fraction, screen.bottom(),
            division == 2U ? 1.0F : 0.6F,
            alpha(ctx.theme().widget, division == 2U ? 0.64F : 0.34F));
        ctx.draw_list().add_line(
            screen.x, screen.y + screen.height * fraction,
            screen.right(), screen.y + screen.height * fraction,
            division == 2U ? 1.0F : 0.6F,
            alpha(ctx.theme().widget, division == 2U ? 0.64F : 0.34F));
      }
      ctx.outline(screen, ctx.theme().widget_hover);
    }

    for (const output::OutputMap &map : feed_map.maps) {
      const core::u32 viewport_ordinal =
          feed_viewport_ordinal(feed_map, map);
      const FeedWindowLayout *const window =
          feed_window_layout_for(windows, map.node_index);
      if (window == nullptr) {
        continue;
      }
      scene::CanvasRect viewport = map.viewport;
      if (drag_kind_ != DragKind::None && drag_node_ == map.node) {
        viewport = preview_;
      }
      const scene::CanvasRect placed{
          window->offset_x + viewport.x, viewport.y, viewport.width,
          viewport.height};
      gui::Rect rectangle =
          feed_map_screen_rect(fitted, mapping_canvas, placed);
      const bool selected = selected_map_node_ == map.node;

      // The resize handle gets first refusal over the rectangle body.
      const gui::Rect handle{rectangle.right() - 6.0F,
                             rectangle.bottom() - 6.0F, 12.0F,
                             12.0F};
      gui::Interaction handle_interaction{};
      if (selected) {
        handle_interaction =
            ctx.grab(mix(id_, "feed.resize", map.node.value()), handle);
        if (handle_interaction.pressed) {
          drag_kind_ = DragKind::Resize;
          drag_node_ = map.node;
          drag_original_ = map.viewport;
          preview_ = drag_original_;
          drag_start_x_ = ctx.pointer_x();
          drag_start_y_ = ctx.pointer_y();
        }
        if (handle_interaction.held && drag_kind_ == DragKind::Resize &&
            drag_node_ == map.node) {
          const core::i64 dx = static_cast<core::i64>(
              std::llround((ctx.pointer_x() - drag_start_x_) / scale_factor));
          const core::i64 dy = static_cast<core::i64>(
              std::llround((ctx.pointer_y() - drag_start_y_) / scale_factor));
          preview_ = resize_feed_rect(drag_original_, window->raster, dx, dy);
          viewport = preview_;
          rectangle = feed_map_screen_rect(
              fitted, mapping_canvas,
              scene::CanvasRect{window->offset_x + viewport.x, viewport.y,
                                viewport.width, viewport.height});
        }
        if (handle_interaction.released && drag_kind_ == DragKind::Resize &&
            drag_node_ == map.node) {
          if (preview_ != drag_original_) {
            frame.edit = FeedMapRectEdit{map.node, preview_};
          }
          drag_kind_ = DragKind::None;
        }
      }

      const gui::Interaction body_interaction =
          ctx.grab(mix(id_, "feed.rectangle", map.node.value()), rectangle);
      if (body_interaction.pressed) {
        selected_map_node_ = map.node;
        frame.selection = map.node;
        drag_kind_ = DragKind::Move;
        drag_node_ = map.node;
        drag_original_ = map.viewport;
        preview_ = drag_original_;
        drag_start_x_ = ctx.pointer_x();
        drag_start_y_ = ctx.pointer_y();
      }
      if (body_interaction.held && drag_kind_ == DragKind::Move &&
          drag_node_ == map.node) {
        const core::i64 dx = static_cast<core::i64>(
            std::llround((ctx.pointer_x() - drag_start_x_) / scale_factor));
        const core::i64 dy = static_cast<core::i64>(
            std::llround((ctx.pointer_y() - drag_start_y_) / scale_factor));
        preview_ = move_feed_rect(drag_original_, window->raster, dx, dy);
        viewport = preview_;
        rectangle = feed_map_screen_rect(
            fitted, mapping_canvas,
            scene::CanvasRect{window->offset_x + viewport.x, viewport.y,
                              viewport.width, viewport.height});
      }
      if (body_interaction.released && drag_kind_ == DragKind::Move &&
          drag_node_ == map.node) {
        if (preview_ != drag_original_) {
          frame.edit = FeedMapRectEdit{map.node, preview_};
        }
        drag_kind_ = DragKind::None;
      }

      const gui::UiColor color = feed_color(ctx.theme(), map.node_index);
      ctx.panel(rectangle, alpha(color, selected ? 0.30F : 0.19F));
      ctx.outline(rectangle, selected ? ctx.theme().accent : color,
                  selected ? 2.0F : 1.0F);
      if (surface_drag_active) {
        const bool under_pointer =
            rectangle.contains(ctx.pointer_x(), ctx.pointer_y());
        surface_drop_targets.emplace_back(rectangle, map.node);
        ctx.panel(rectangle,
                  alpha(ctx.theme().accent, under_pointer ? 0.26F : 0.10F));
        ctx.outline(rectangle,
                    under_pointer ? ctx.theme().accent
                             : alpha(ctx.theme().accent, 0.45F),
                    under_pointer ? 2.0F : 1.0F);
      }
      // A title band across its top, inside the outline.
      gui::Rect title_band = gui::inset(rectangle, 1.0F);
      title_band.height = 18.0F;
      ctx.panel(title_band, alpha(color, selected ? 0.78F : 0.56F));
      ctx.label(gui::inset(title_band, gui::Padding{6.0F, 0.0F, 6.0F, 1.0F}),
                map.name.empty() ? feed_viewport_short_name(viewport_ordinal)
                                 : map.name,
                ctx.theme().text);
      ctx.tooltip(rectangle,
                  feed_row_tooltip(map,
                                   feed_viewport_title(map, viewport_ordinal),
                                   feed_map));
      if (selected) {
        const gui::Rect current_handle =
            gui::around(rectangle.right(), rectangle.bottom(), 10.0F, 10.0F);
        ctx.panel(current_handle, ctx.theme().accent);
        ctx.outline(current_handle, ctx.theme().background);
      }
    }

    if (uv_mesh_domain && preview_mesh_ != nullptr && selected_map != nullptr) {
      const output::ProjectionMesh &mesh = *preview_mesh_;
      const std::size_t triangle_count = mesh.indices.size() / 3U;
      constexpr std::size_t max_preview_triangles = 1'024U;
      const std::size_t drawn = std::min(triangle_count, max_preview_triangles);
      gui::Rect viewport_screen{};
      for (const FeedWindowLayout &window : windows) {
        if (window.node_index != selected_map->node_index) {
          continue;
        }
        const scene::CanvasRect viewport = selected_map->viewport;
        viewport_screen = feed_map_screen_rect(
            fitted, mapping_canvas,
            scene::CanvasRect{window.offset_x + viewport.x, viewport.y,
                              viewport.width, viewport.height});
        break;
      }
      const auto screen = [&viewport_screen](
                              const output::ProjectionMeshVertex &vertex) {
        return IsoPoint{
            viewport_screen.x + vertex.destination_u * viewport_screen.width,
            viewport_screen.y + vertex.destination_v * viewport_screen.height};
      };
      for (std::size_t preview_index = 0U; preview_index < drawn;
           ++preview_index) {
        const std::size_t triangle = preview_index * triangle_count / drawn;
        const std::size_t first = triangle * 3U;
        const IsoPoint a = screen(mesh.vertices[mesh.indices[first]]);
        const IsoPoint b = screen(mesh.vertices[mesh.indices[first + 1U]]);
        const IsoPoint c = screen(mesh.vertices[mesh.indices[first + 2U]]);
        const gui::UiColor wire = alpha(ctx.theme().accent, 0.44F);
        ctx.draw_list().add_line(a.x, a.y, b.x, b.y, 0.75F, wire);
        ctx.draw_list().add_line(b.x, b.y, c.x, c.y, 0.75F, wire);
        ctx.draw_list().add_line(c.x, c.y, a.x, a.y, 0.75F, wire);
      }
    } else if (uv_mesh_domain) {
      gui::Rect foot = gui::inset(fitted, gui::Padding{8.0F, 0.0F, 8.0F, 6.0F});
      ctx.label(gui::cut_bottom(foot, 18.0F), "UV MESH PREVIEW UNAVAILABLE",
                ctx.theme().warning);
    }
  }

  if (feed_map.maps.empty()) {
    gui::Rect message =
        gui::inset(work, gui::Padding{18.0F, 36.0F, 18.0F, 0.0F});
    ctx.label(gui::cut_top(message, 22.0F), "NO OUTPUT VIEWPORTS CONFIGURED",
              ctx.theme().text_muted);
    ctx.label(gui::cut_top(message, 18.0F),
              "Add a viewport inside an application window to begin.",
              ctx.theme().text_muted);
  } else {
    std::array<char, 72U> canvas_text{};
    const int length = std::snprintf(
        canvas_text.data(), canvas_text.size(),
        "OUTPUT WINDOWS  %u x %u  |  VIEWPORTS INSIDE FIXED RASTERS",
        mapping_canvas.width, mapping_canvas.height);
    gui::Rect heading = gui::inset(work, gui::Padding{12.0F, 5.0F, 12.0F, 0.0F});
    ctx.label(
        gui::cut_top(heading, 18.0F),
        std::string_view{canvas_text.data(),
                         static_cast<std::size_t>(std::max(length, 0))},
        ctx.theme().text_muted);
  }

  const float details_width = feed_details_width(bounds);
  const gui::Rect rectangle_controls{
      work.x + 12.0F, bounds.bottom() - 78.0F,
      std::max(work.width - details_width - 24.0F, 0.0F), 72.0F};
  if (selected_map != nullptr && rectangle_controls.width >= 240.0F &&
      content.height >= 180.0F) {
    if (rectangle_mesh_domain_ != uv_mesh_domain) {
      rectangle_numbers_.cancel(ctx);
      rectangle_mesh_domain_ = uv_mesh_domain;
    }
    const auto draw_rectangle = [&](std::string_view label,
                                     std::string_view domain,
                                     scene::CanvasRect original,
                                     scene::Canvas extent, float y) {
      scene::CanvasRect edited = original;
      // The extent bounds every field below, so it is stated beside them: a
      // crop that stops short is otherwise a silent clamp against a raster
      // authored somewhere else in the panel.
      std::array<char, 40U> bound{};
      const int bound_length =
          std::snprintf(bound.data(), bound.size(), "  |  WITHIN %u x %u",
                        extent.width, extent.height);
      ctx.label(gui::Rect{rectangle_controls.x, y,
                          rectangle_controls.width, 17.0F},
                std::string{label} +
                    std::string{bound.data(), static_cast<std::size_t>(
                                                  std::max(bound_length, 0))},
                ctx.theme().text_muted);
      const float field_width = (rectangle_controls.width - 9.0F) / 4.0F;
      const std::array<std::string_view, 4U> names{"X", "Y", "W", "H"};
      const std::array<core::u32, 4U> values{
          original.x, original.y, original.width, original.height};
      bool changed = false;
      for (std::size_t index = 0U; index < values.size(); ++index) {
        double value{};
        const std::string key = std::string{domain} + "." +
                                std::string{names[index]};
        if (!rectangle_numbers_.draw(
                ctx, mix(id_, key, selected_map->node.value()),
                gui::Rect{rectangle_controls.x +
                              static_cast<float>(index) * (field_width + 3.0F),
                          y + 17.0F, field_width, 20.0F},
                names[index], static_cast<double>(values[index]), 1.0,
                value, 0)) continue;
        // Preserve the other rectangle components when moving. A source
        // crop and a destination viewport are independent authored values.
        const auto bounded = [&](core::u32 minimum, core::u32 maximum) {
          return static_cast<core::u32>(std::clamp(
              std::round(value), static_cast<double>(minimum),
              static_cast<double>(std::max(minimum, maximum))));
        };
        switch (index) {
        case 0U: edited.x = bounded(0U, extent.width -
                     std::min(original.width, extent.width)); break;
        case 1U: edited.y = bounded(0U, extent.height -
                     std::min(original.height, extent.height)); break;
        case 2U: edited.width = bounded(1U, extent.width -
                     std::min(original.x, extent.width)); break;
        case 3U: edited.height = bounded(1U, extent.height -
                     std::min(original.y, extent.height)); break;
        default: break;
        }
        changed = true;
      }
      return changed ? std::optional{edited} : std::nullopt;
    };
    const auto viewport = draw_rectangle(
        "OUTPUT WINDOW  /  DESTINATION PIXELS", "feed.viewport",
        selected_map->viewport,
        feed_window_raster(selected_map->node_index, canvas, hardware),
        rectangle_controls.y);
    if (viewport) frame.edit = FeedMapRectEdit{selected_map->node, *viewport};
    if (uv_mesh_domain) {
      ctx.label(gui::Rect{rectangle_controls.x, rectangle_controls.y + 39.0F,
                         rectangle_controls.width, 20.0F},
                "UV MESH USES THE FULL SURFACE RASTER", ctx.theme().text_muted);
    } else {
      const auto source = draw_rectangle(
          selected_map->projection == output::FeedProjection::PlanarSurface
              ? "LED SURFACE  /  SOURCE PIXELS" : "CANVAS  /  SOURCE PIXELS",
          "feed.source", selected_map->source,
          mapping_surface != nullptr ? mapping_surface->geometry.raster : canvas,
          rectangle_controls.y + 39.0F);
      if (source) frame.projection_edit = FeedProjectionEdit{
          selected_map->node, selected_map->projection, selected_map->surface,
          *source};
    }
    frame.editing = frame.editing || rectangle_numbers_.live();
  } else {
    // A resize or replicated deletion can remove the active field without
    // another draw call to release its keyboard focus.
    rectangle_numbers_.cancel(ctx);
  }
  if (details_width > 0.0F) {
    gui::Rect details = bounds;
    static_cast<void>(gui::cut_top(details, title_height));
    details = gui::cut_right(details, details_width);
    ctx.panel(details, scale(ctx.theme().panel, 0.96F));
    {
      gui::Rect edge = details;
      ctx.panel(gui::cut_left(edge, 1.0F), ctx.theme().divider);
    }
    gui::Rect title_strip =
        gui::inset(details, gui::Padding{10.0F, 3.0F, 10.0F, 0.0F});
    const gui::Rect projection_title = gui::cut_top(title_strip, 20.0F);
    if (selected_map != nullptr) {
      ctx.label(projection_title,
                feed_output_identity(selected_map->node_index),
                ctx.theme().text);
    } else {
      ctx.label(projection_title, "OUTPUT PROJECTION", ctx.theme().text_muted);
    }

    // Rows down the column 10px in from its sides; each call says how tall
    // the next strip is and how far below it the one after starts.
    gui::Metrics details_metrics = ctx.theme().metrics;
    details_metrics.inset = 10.0F;
    details_metrics.gap = 28.0F;
    gui::FormLayout details_column{details, details_metrics};
    constexpr float field_height = 22.0F;
    constexpr float field_gap = 3.0F;
    if (selected_map != nullptr) {
      ctx.label(details_column.next(feed_details_label_height, 1.0F),
                feed_runtime_binding(selected_map->node_index),
                ctx.theme().text_muted);
      const bool has_surfaces = !feed_map.surfaces.empty();
      const gui::Rect modes = details_column.next(field_height, 4.0F);
      const gui::Rect canvas_mode =
          has_surfaces ? gui::column(modes, 0U, 2U, field_gap) : modes;
      const gui::Rect physical_mode = gui::column(modes, 1U, 2U, field_gap);
      if (ctx.button(mix(id_, "feed.mode.canvas", selected_map->node.value()),
                     canvas_mode, "CANVAS") &&
          selected_map->projection != output::FeedProjection::CanvasSlice) {
        const scene::CanvasRect source =
            scene::contains(canvas, selected_map->source)
                ? selected_map->source
                : scene::whole(canvas);
        frame.projection_edit =
            FeedProjectionEdit{selected_map->node,
                               output::FeedProjection::CanvasSlice,
                               {},
                               source};
      }
      if (has_surfaces &&
          ctx.button(mix(id_, "feed.mode.physical", selected_map->node.value()),
                     physical_mode, "PHYSICAL") &&
          selected_map->projection != output::FeedProjection::PlanarSurface) {
        const output::ProjectionSurface *target =
            surface_or_null(feed_map, selected_surface_node_);
        if (target == nullptr) {
          target = &feed_map.surfaces.front();
        }
        selected_surface_node_ = target->node;
        frame.surface_selection = target->node;
        frame.projection_edit = FeedProjectionEdit{
            selected_map->node, output::FeedProjection::PlanarSurface,
            target->node, scene::whole(target->geometry.raster)};
      }
      if (selected_map->projection == output::FeedProjection::CanvasSlice ||
          !has_surfaces) {
        ctx.outline(canvas_mode, ctx.theme().accent, 1.5F);
      } else {
        ctx.outline(physical_mode, ctx.theme().accent, 1.5F);
      }

      std::string_view status{"SHARED CANVAS CROP"};
      gui::UiColor status_color = ctx.theme().text_muted;
      if (selected_map->projection == output::FeedProjection::PlanarSurface) {
        if (selected_map->perspective_ready()) {
          status = uv_mesh_domain
                       ? std::string_view{"CALIBRATED UV SURFACE - READY"}
                       : std::string_view{"CALIBRATED WORLD SURFACE - READY"};
          status_color = ctx.theme().good;
        } else if (feed_map.surface_for(selected_map->surface) == nullptr) {
          status = "WORLD SURFACE - MISSING ASSIGNMENT";
          status_color = ctx.theme().warning;
        } else {
          status = "WORLD SURFACE - REGION OUTSIDE RASTER";
          status_color = ctx.theme().warning;
        }
      }
      ctx.label(details_column.next(18.0F, 3.0F), status, status_color);

    // A project with no surfaces authored has no surface row: a heading over an
    // empty well is a container invented to hold an exception.
    if (has_surfaces) {
      ctx.label(details_column.next(feed_details_label_height, 1.0F), "SURFACE",
                ctx.theme().text_muted);
      // A surface reaches an output by being dragged onto it, so this row
      // states the surface that is on this one and takes the drop as well. It
      // is not a stepper: cycling blindly through a wall's surfaces to find
      // the right one is the gesture nobody could decide on.
      const gui::Rect surface_choice = details_column.next(field_height, 7.0F);
      const output::ProjectionSurface *const assigned =
          feed_map.surface_for(selected_map->surface);
      const gui::Interaction assignment_interaction =
          assigned != nullptr
              ? ctx.grab(mix(id_, "feed.surface.choice",
                             selected_map->node.value()),
                         surface_choice)
              : gui::Interaction{};
      ctx.panel(surface_choice, assignment_interaction.hovered
                                    ? ctx.theme().widget_hover
                                    : ctx.theme().track);
      const gui::Rect assignment_label = gui::inset(surface_choice, 7.0F, 0.0F);
      if (assigned != nullptr) {
        ctx.label(assignment_label,
                  assigned->name.empty()
                      ? std::string_view{"UNTITLED SURFACE"}
                      : std::string_view{assigned->name},
                  ctx.theme().text);
        if (assignment_interaction.clicked) {
          selected_surface_node_ = assigned->node;
          frame.surface_selection = assigned->node;
        }
      } else if (selected_map->surface != doc::NodeId{}) {
        ctx.label(assignment_label, "MISSING SURFACE", ctx.theme().warning);
      } else {
        ctx.label(assignment_label, "DRAG A SURFACE HERE",
                  ctx.theme().text_muted);
      }
      if (surface_drag_active) {
        surface_drop_targets.emplace_back(surface_choice, selected_map->node);
        ctx.outline(surface_choice,
                    surface_choice.contains(ctx.pointer_x(), ctx.pointer_y())
                   ? ctx.theme().accent
                   : alpha(ctx.theme().accent, 0.45F),
                    1.0F);
      }
    }
    } else {
      ctx.label(details_column.next(40.0F, 33.0F),
                "Select an output region to choose its projection.",
                ctx.theme().text_muted);
    }

    sync_surface_details();
    // The rule runs a pixel wider than the rows on each side.
    ctx.panel(gui::cell(gui::inset(details, 9.0F, 0.0F), details_column.next(1.0F, 5.0F)),
              ctx.theme().divider);
    const gui::Rect calibration_heading = details_column.next(20.0F, 2.0F);
    ctx.label(calibration_heading, "SURFACE CALIBRATION",
              ctx.theme().text_muted);

    const output::ProjectionSurface *const details_surface =
        feed_map.surface_for(surface_details_node_);
    if (details_surface != nullptr) {
      const bool details_locked = details_surface->locked;
      if (details_locked) {
        gui::Rect locked_strip = calibration_heading;
        ctx.label(gui::cut_right(locked_strip, 68.0F), "LOCKED",
                  ctx.theme().warning);
        ctx.push_pointer_block();
      }
      const auto emit_surface_edit = [&](FeedSurfaceTransform transform =
                                             FeedSurfaceTransform::None) {
        if (details_locked) {
          return;
        }
        if (surface_shape_ == output::ProjectionSurfaceShape::ImportedMesh &&
            surface_mesh_source_.empty()) {
          return;
        }
        if (frame.surface_edit.has_value()) {
          transform |= frame.surface_edit->transform_components;
        }
        frame.surface_edit = FeedSurfaceEditRequest{
            surface_details_node_, surface_name_,
            surface_geometry_,     0U,
            surface_shape_,        surface_horizontal_arc_turns_,
            surface_mesh_columns_, surface_mesh_source_,
            surface_mesh_scale_,   transform};
      };
      const auto draw_float = [&](gui::UiId field_id, const gui::Rect &field,
                                  std::string_view axis, float &value,
                                  double speed, double minimum, double maximum,
                                  int decimals,
                                  FeedSurfaceTransform transform =
                                      FeedSurfaceTransform::None) {
        double changed{};
        if (!surface_numbers_.draw(ctx, field_id, field, axis,
                                   static_cast<double>(value), speed, changed,
                                   decimals)) {
          return;
        }
        value = static_cast<float>(std::clamp(changed, minimum, maximum));
        emit_surface_edit(transform);
      };
      // `count` fields across the next row, `gap_after` above the row
      // after it - first behind a label, for labelled_columns.
      const auto field_columns = [&](std::size_t count, float gap_after) {
        const gui::Rect strip = details_column.next(field_height, gap_after);
        std::array<gui::Rect, 3U> fields{};
        for (std::size_t index = 0U; index < count; ++index) {
          fields[index] = gui::column(strip, index, count, field_gap);
        }
        return fields;
      };
      const auto labelled_columns = [&](std::string_view label,
                                        std::size_t count,
                                        float gap_after = 5.0F) {
        constexpr float label_width = 50.0F;
        gui::Rect strip = details_column.next(field_height, gap_after);
        ctx.label(gui::cut_left(strip, label_width), label,
                  ctx.theme().text_muted);
        static_cast<void>(gui::cut_left(strip, field_gap));
        std::array<gui::Rect, 3U> fields{};
        for (std::size_t index = 0U; index < count; ++index) {
          fields[index] = gui::column(strip, index, count, field_gap);
        }
        return fields;
      };

      const auto page_fields = field_columns(2U, 7.0F);
      if (ctx.button(mix(id_, "feed.surface.geometry-page"), page_fields[0], "GEOMETRY")) surface_color_page_ = false;
      if (ctx.button(mix(id_, "feed.surface.color-page"), page_fields[1], "COLOUR")) surface_color_page_ = true;
      const gui::UiId name_id = mix(id_, "feed.surface.name", surface_details_node_.value());
      const gui::UiId camera_id = mix(id_, "feed.surface.camera-inverse", surface_details_node_.value());
      if (!surface_color_page_) {
      auto fields = field_columns(3U, 5.0F);
      const gui::Rect planar_shape = fields[0U];
      const gui::Rect arc_shape = fields[1U];
      const gui::Rect mesh_shape = fields[2U];
      if (ctx.button(mix(id_, "feed.surface.shape.planar",
                         surface_details_node_.value()),
                     planar_shape, "FLAT") &&
          surface_shape_ != output::ProjectionSurfaceShape::Planar) {
        surface_shape_ = output::ProjectionSurfaceShape::Planar;
        emit_surface_edit();
      }
      if (ctx.button(
              mix(id_, "feed.surface.shape.arc", surface_details_node_.value()),
              arc_shape, "ARC") &&
          surface_shape_ != output::ProjectionSurfaceShape::HorizontalArc) {
        surface_shape_ = output::ProjectionSurfaceShape::HorizontalArc;
        emit_surface_edit();
      }
      if (ctx.button(mix(id_, "feed.surface.shape.mesh",
                         surface_details_node_.value()),
                     mesh_shape, "UV MESH") &&
          surface_shape_ != output::ProjectionSurfaceShape::ImportedMesh) {
        surface_shape_ = output::ProjectionSurfaceShape::ImportedMesh;
        // An imported surface is not document-valid until it has a UV mesh.
        // Keep the local choice visible so the explicit chooser appears, but
        // never publish a broken surface while the source is still empty.
        if (!surface_mesh_source_.empty()) {
          emit_surface_edit();
        }
      }
      const gui::Rect selected_shape =
          surface_shape_ == output::ProjectionSurfaceShape::Planar
              ? planar_shape
              : (surface_shape_ == output::ProjectionSurfaceShape::HorizontalArc
                     ? arc_shape
                     : mesh_shape);
      ctx.outline(selected_shape, ctx.theme().accent, 1.5F);

      fields = labelled_columns("NAME", 1U);
      if (ctx.text_field(name_id, fields[0U], surface_name_)) {
        surface_name_dirty_ = true;
      }

      fields = labelled_columns(
          surface_shape_ == output::ProjectionSurfaceShape::ImportedMesh
              ? std::string_view{"ORIGIN"}
              : std::string_view{"CENTER"},
          3U);
      draw_float(
          mix(id_, "feed.surface.center.x", surface_details_node_.value()),
          fields[0U], "X", surface_geometry_.center.x, 0.01, -1'000'000.0,
          1'000'000.0, 3, FeedSurfaceTransform::PositionX);
      draw_float(
          mix(id_, "feed.surface.center.y", surface_details_node_.value()),
          fields[1U], "Y", surface_geometry_.center.y, 0.01, -1'000'000.0,
          1'000'000.0, 3, FeedSurfaceTransform::PositionY);
      draw_float(
          mix(id_, "feed.surface.center.z", surface_details_node_.value()),
          fields[2U], "Z", surface_geometry_.center.z, 0.01, -1'000'000.0,
          1'000'000.0, 3, FeedSurfaceTransform::PositionZ);

      constexpr double pi = 3.14159265358979323846;
      double yaw_degrees =
          std::atan2(-static_cast<double>(surface_geometry_.right.z),
                     static_cast<double>(surface_geometry_.right.x)) *
          180.0 / pi;
      double changed_yaw{};
      fields = labelled_columns("YAW", 1U);
      if (surface_numbers_.draw(
              ctx, mix(id_, "feed.surface.yaw", surface_details_node_.value()),
              fields[0U], "YAW", yaw_degrees, 0.25, changed_yaw, 1)) {
        const double radians =
            std::clamp(changed_yaw, -3600.0, 3600.0) * pi / 180.0;
        surface_geometry_.right =
            scene::Vec3{static_cast<float>(std::cos(radians)), 0.0F,
                        static_cast<float>(-std::sin(radians))};
        surface_geometry_.up = scene::Vec3{0.0F, 1.0F, 0.0F};
        emit_surface_edit(FeedSurfaceTransform::Yaw);
      }

      if (surface_shape_ == output::ProjectionSurfaceShape::HorizontalArc) {
        fields = labelled_columns("ARC", 2U);
        double bend_degrees =
            static_cast<double>(surface_horizontal_arc_turns_) * 360.0;
        double changed_bend{};
        if (surface_numbers_.draw(
                ctx,
                mix(id_, "feed.surface.arc", surface_details_node_.value()),
                fields[0U], "DEG", bend_degrees, 0.25, changed_bend, 1)) {
          changed_bend = std::clamp(changed_bend, -179.0, 179.0);
          if (std::abs(changed_bend) < 0.1) {
            changed_bend = changed_bend < 0.0 ? -0.1 : 0.1;
          }
          surface_horizontal_arc_turns_ =
              static_cast<float>(changed_bend / 360.0);
          emit_surface_edit();
        }
        double changed_columns{};
        if (surface_numbers_.draw(
                ctx,
                mix(id_, "feed.surface.columns", surface_details_node_.value()),
                fields[1U], "COL", static_cast<double>(surface_mesh_columns_),
                1.0, changed_columns, 0)) {
          surface_mesh_columns_ =
              static_cast<core::u32>(std::llround(std::clamp(
                  changed_columns,
                  static_cast<double>(output::min_horizontal_arc_columns),
                  static_cast<double>(output::max_horizontal_arc_columns))));
          emit_surface_edit();
        }
      }

      if (surface_shape_ == output::ProjectionSurfaceShape::ImportedMesh) {
        const std::string mesh_status =
            mesh_source_status(surface_mesh_source_);
        ctx.label(details_column.next(16.0F, 0.0F), mesh_status,
                  surface_mesh_source_.empty() ? ctx.theme().warning
                                               : ctx.theme().text_muted);
        if (ctx.button(mix(id_, "feed.surface.mesh.browse",
                           surface_details_node_.value()),
                       details_column.next(field_height, 5.0F),
                       surface_mesh_source_.empty() ? "CHOOSE UV MESH"
                                                    : "REPLACE UV MESH")) {
          mesh_browse_pending_ = true;
          // A replacement may deliberately overwrite the same path. Record a
          // new generation without doing any file work on this UI frame; the
          // prior exact-key UV mesh remains visible until publication.
          if (!surface_mesh_source_.empty()) {
            static_cast<void>(
                preview_projection_cache_.refresh_imported_mesh());
          }
          frame.mesh_browse = FeedSurfaceEditRequest{
              surface_details_node_,
              surface_name_,
              surface_geometry_,
              0U,
              output::ProjectionSurfaceShape::ImportedMesh,
              surface_horizontal_arc_turns_,
              surface_mesh_columns_,
              surface_mesh_source_,
              surface_mesh_scale_,
              FeedSurfaceTransform::None};
        }
        fields = labelled_columns("SCALE", 1U);
        double changed_scale{};
        if (surface_numbers_.draw(ctx,
                                  mix(id_, "feed.surface.mesh.scale",
                                      surface_details_node_.value()),
                                  fields[0U], "S",
                                  static_cast<double>(surface_mesh_scale_),
                                  0.01, changed_scale, 3)) {
          surface_mesh_scale_ = static_cast<float>(
              std::clamp(changed_scale, 0.001,
                         static_cast<double>(output::max_imported_mesh_scale)));
          if (!surface_mesh_source_.empty()) {
            emit_surface_edit();
          }
        }
      }

      if (surface_shape_ != output::ProjectionSurfaceShape::ImportedMesh) {
        fields = labelled_columns("SIZE", 2U);
        draw_float(
            mix(id_, "feed.surface.width", surface_details_node_.value()),
            fields[0U], "W", surface_geometry_.width, 0.01, 0.01, 1'000'000.0,
            3);
        draw_float(
            mix(id_, "feed.surface.height", surface_details_node_.value()),
            fields[1U], "H", surface_geometry_.height, 0.01, 0.01, 1'000'000.0,
            3);
      } else {
        ctx.label(details_column.next(16.0F, 4.0F),
                  "AUTHORED COORDINATES / SHARED ORIGIN",
                  ctx.theme().text_muted);
      }

      fields = labelled_columns("RASTER", 2U, 7.0F);
      const auto draw_raster = [&](gui::UiId field_id, const gui::Rect &field,
                                   std::string_view axis, core::u32 &value) {
        double changed{};
        if (!surface_numbers_.draw(ctx, field_id, field, axis,
                                   static_cast<double>(value), 1.0, changed,
                                   0)) {
          return;
        }
        value = static_cast<core::u32>(
            std::llround(std::clamp(changed, 1.0, 131'072.0)));
        emit_surface_edit();
      };
      draw_raster(
          mix(id_, "feed.surface.raster.width", surface_details_node_.value()),
          fields[0U], "W", surface_geometry_.raster.width);
      draw_raster(
          mix(id_, "feed.surface.raster.height", surface_details_node_.value()),
          fields[1U], "H", surface_geometry_.raster.height);

      } else {
        bool changed = ctx.checkbox(mix(id_, "feed.surface.color-enabled", surface_details_node_.value()),
            details_column.next(24.0F, 8.0F), surface_color_.enabled,
            "Enable compensation");
        ctx.label(details_column.next(20.0F, 4.0F), "WALL PRIMARIES (LINEAR)",
                  ctx.theme().text_muted);
        constexpr std::array<std::string_view,3> rows{"RED", "GREEN", "BLUE"};
        for (std::size_t row = 0; row < 3U; ++row) {
          const auto cells = labelled_columns(rows[row], 3U);
          for (std::size_t column = 0; column < 3U; ++column) {
            const std::size_t index = row * 3U + column;
            double value{};
            if (surface_numbers_.draw(ctx, mix(id_, "feed.surface.primary", surface_details_node_.value() * 9U + index),
                cells[column], {}, surface_color_.primaries[index], 0.001, value, 4)) {
              surface_color_.primaries[index] = static_cast<float>(std::clamp(value, -16.0, 16.0)); changed = true;
            }
          }
        }
        details_column.space(10.0F);
        ctx.label(details_column.next(20.0F, 3.0F), "CAMERA IDT INVERSE (.cube)",
                  ctx.theme().text_muted);
        changed |= ctx.text_field(camera_id, details_column.next(24.0F, 8.0F),
                                  surface_color_.camera_inverse_lut_source);
        ctx.label(details_column.next(18.0F, 0.0F), "Measured for this wall / camera.",
                  ctx.theme().text_muted);
        ctx.label(details_column.next(18.0F, 0.0F), "Applied after the show look.",
                  ctx.theme().text_muted);
        if (changed) {
          emit_surface_edit();
          if (frame.surface_edit) frame.surface_edit->color = surface_color_;
        }
      }
      if (surface_name_dirty_ && ctx.focus() != name_id) {
        emit_surface_edit();
        surface_name_dirty_ = false;
      }
      frame.editing = frame.editing || (!details_locked &&
                      (surface_numbers_.live() || ctx.focus() == name_id || ctx.focus() == camera_id));
      if (details_locked) {
        ctx.pop_pointer_block();
      }
    } else {
      ctx.label(details_column.next(40.0F, 0.0F),
                "Add or select a physical surface to calibrate it.",
                ctx.theme().text_muted);
    }
  }

  // One resolution point for the whole panel: every target has been laid out by
  // now, so a release names exactly one output and the assignment is a single
  // document edit.
  if (surface_drag_node_ != doc::NodeId{}) {
    const output::ProjectionSurface *const dragged =
        feed_map.surface_for(surface_drag_node_);
    bool over_surface_list = false;
    if (surface_drag_active && dragged != nullptr) {
      for (const auto &[row_bounds, node] : surface_rows) {
        if (!row_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
          continue;
        }
        over_surface_list = true;
        const bool after =
            ctx.pointer_y() >= row_bounds.y + row_bounds.height * 0.5F;
        ctx.panel(gui::rule_y(row_bounds,
                              after ? row_bounds.bottom() + 2.0F
                                    : row_bounds.y - 1.0F,
                              2.0F),
                  ctx.theme().accent);
        if (!ctx.pointer_down() && node != dragged->node) {
          std::vector<doc::NodeId> current;
          current.reserve(feed_map.surfaces.size());
          for (const output::ProjectionSurface &surface : feed_map.surfaces) {
            current.push_back(surface.node);
          }
          frame.organize = FeedOrganizeRequest{
              FeedOrganizeAction::Reorder, dragged->node, {},
              reordered(current, dragged->node, node, after)};
          if (frame.organize->order == current) {
            frame.organize.reset();
          }
        }
        break;
      }
    }
    if (surface_drag_active && dragged != nullptr) {
      for (const auto &[target_bounds, map_node] : surface_drop_targets) {
        if (over_surface_list ||
            !target_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
          continue;
        }
        if (!ctx.pointer_down()) {
          selected_map_node_ = map_node;
          selected_surface_node_ = dragged->node;
          frame.selection = map_node;
          frame.surface_selection = dragged->node;
          frame.projection_edit = FeedProjectionEdit{
              map_node, output::FeedProjection::PlanarSurface, dragged->node,
              scene::whole(dragged->geometry.raster)};
        }
        break;
      }
      const gui::Rect ghost{ctx.pointer_x() + 12.0F, ctx.pointer_y() + 8.0F,
                            152.0F, 22.0F};
      ctx.panel(ghost, ctx.theme().widget_active);
      gui::Rect edge = ghost;
      ctx.panel(gui::cut_left(edge, 3.0F), ctx.theme().accent);
      ctx.label(gui::inset(ghost, gui::Padding{9.0F, 0.0F, 3.0F, 0.0F}),
                dragged->name.empty() ? std::string_view{"UNTITLED SURFACE"}
                                      : std::string_view{dragged->name},
                ctx.theme().text);
    }
    if (!ctx.pointer_down()) {
      surface_drag_node_ = {};
      surface_drag_moved_ = false;
    }
  }

  frame.dragging = drag_kind_ != DragKind::None || surface_drag_active ||
                   (row_drag_node_ != doc::NodeId{} && row_drag_moved_);
  frame.editing = frame.editing || rename_.active();
  ctx.pop_clip();
  return frame;
}

bool detail::wait_for_feed_mapping_projection_cache_for_test(
    FeedMappingPanel &panel) noexcept {
  return render::detail::wait_for_output_projection_cache_for_test(
      panel.preview_projection_cache_);
}

} // namespace mediaserver::editor
