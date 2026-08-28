#include <mediaserver/editor/mapping_panels.hpp>

#include <mediaserver/render/projection_mesh_loader.hpp>
#include <mediaserver/topology/hardware_map_schema.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>

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

void border(gui::UiContext &ctx, const gui::Rect &bounds, gui::UiColor color,
            float thickness = 1.0F) {
  if (bounds.width <= 0.0F || bounds.height <= 0.0F || thickness <= 0.0F) {
    return;
  }
  auto &draw = ctx.draw_list();
  draw.add_rect(bounds.x, bounds.y, bounds.width, thickness, color);
  draw.add_rect(bounds.x, bounds.y + bounds.height - thickness, bounds.width,
                thickness, color);
  draw.add_rect(bounds.x, bounds.y, thickness, bounds.height, color);
  draw.add_rect(bounds.x + bounds.width - thickness, bounds.y, thickness,
                bounds.height, color);
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
  constexpr std::size_t count = 14U;
  constexpr float tau = 6.28318530718F;
  std::array<gui::UiPoint, count> points{};
  for (std::size_t index = 0U; index < count; ++index) {
    const float angle =
        tau * static_cast<float>(index) / static_cast<float>(count);
    points[index] = gui::UiPoint{x + std::cos(angle) * radius,
                                 y + std::sin(angle) * radius};
  }
  ctx.draw_list().add_convex_poly(points, color);
}

void title(gui::UiContext &ctx, const gui::Rect &bounds, std::string_view name,
           std::string_view trailing) {
  const gui::Rect header{bounds.x, bounds.y, bounds.width,
                         std::min(bounds.height, title_height)};
  ctx.panel(header, ctx.theme().background);
  ctx.panel(
      gui::Rect{header.x, header.y + header.height - 1.0F, header.width, 1.0F},
      ctx.theme().divider);
  ctx.label(
      gui::Rect{header.x + 9.0F, header.y, header.width - 18.0F, header.height},
      name, ctx.theme().text);
  if (!trailing.empty()) {
    const float width = ctx.font_width(trailing);
    ctx.label(gui::Rect{header.x + header.width - width - 9.0F, header.y, width,
                        header.height},
              trailing, ctx.theme().text_muted);
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
  if (kind == topology::HardwareDeviceKind::LedWall) {
    const gui::Rect screen{x, y, 18.0F, 12.0F};
    draw.add_rect(screen.x, screen.y, screen.width, screen.height,
                  alpha(color, 0.20F));
    border(ctx, screen, color);
    draw.add_line(x + 6.0F, y, x + 6.0F, y + 12.0F, 1.0F, color);
    draw.add_line(x + 12.0F, y, x + 12.0F, y + 12.0F, 1.0F, color);
    return;
  }
  if (kind == topology::HardwareDeviceKind::LedProcessor) {
    draw.add_rect(x, y, 18.0F, 12.0F, alpha(color, 0.18F));
    border(ctx, gui::Rect{x, y, 18.0F, 12.0F}, color);
    draw.add_rect(x + 3.0F, y + 3.0F, 2.0F, 2.0F, color);
    draw.add_rect(x + 7.0F, y + 3.0F, 8.0F, 2.0F, color);
    draw.add_rect(x + 3.0F, y + 7.0F, 12.0F, 2.0F, alpha(color, 0.65F));
    return;
  }
  if (kind == topology::HardwareDeviceKind::OperatorMachine) {
    border(ctx, gui::Rect{x + 1.0F, y, 16.0F, 10.0F}, color);
    draw.add_line(x + 9.0F, y + 10.0F, x + 9.0F, y + 13.0F, 1.0F, color);
    draw.add_line(x + 5.0F, y + 13.0F, x + 13.0F, y + 13.0F, 1.0F, color);
    return;
  }
  draw.add_rect(x, y, 18.0F, 12.0F, alpha(color, 0.18F));
  border(ctx, gui::Rect{x, y, 18.0F, 12.0F}, color);
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
          : bounds.x + bounds.width;
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
  const gui::Rect face{bounds.x + 2.0F, bounds.y + 9.0F, bounds.width - 4.0F,
                       bounds.height - 11.0F};
  ctx.panel(face, dragging ? scale(body, 1.15F) : body);

  const std::array<gui::UiPoint, 4U> top{{
      {bounds.x + 2.0F, bounds.y + 9.0F},
      {bounds.x + 18.0F, bounds.y + 2.0F},
      {bounds.x + bounds.width - 2.0F, bounds.y + 2.0F},
      {bounds.x + bounds.width - 18.0F, bounds.y + 9.0F},
  }};
  ctx.draw_list().add_convex_poly(top, scale(body, 1.23F));
  ctx.draw_list().add_line(bounds.x + 18.0F, bounds.y + 2.0F,
                           bounds.x + bounds.width - 2.0F, bounds.y + 2.0F,
                           1.0F, alpha(color, 0.72F));
  border(ctx, face, selected ? ctx.theme().accent : ctx.theme().widget_hover,
         selected ? 2.0F : 1.0F);
  ctx.panel(gui::Rect{face.x + 1.0F, face.y + 1.0F, 3.0F, face.height - 2.0F},
            color);
  draw_device_icon(ctx, device.kind, face.x + 10.0F, face.y + 8.0F, color);
  // Text is clipped to the card face while port circles remain free to sit on
  // its outer edge. Runtime status and connector names are supplied by remote
  // machines and can legitimately be wider than this compact topology card.
  ctx.push_clip(face);
  ctx.label(gui::Rect{face.x + 34.0F, face.y + 3.0F, face.width - 39.0F, 18.0F},
            device.name, ctx.theme().text);
  const std::string status = runtime_card_status(presentation);
  ctx.label(
      gui::Rect{face.x + 10.0F, face.y + 20.0F, face.width - 15.0F, 15.0F},
      status, runtime_status_color(ctx.theme(), presentation));

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
      ctx.label(
          gui::Rect{face.x + 34.0F,
                    face.y + 33.0F +
                        static_cast<float>(ordinal) * hardware_port_row_step,
                    std::max(face.width - 43.0F, 0.0F), 15.0F},
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
    ctx.draw_list().add_triangle(point.x - 2.0F, point.y - 2.5F, point.x + 2.5F,
                                 point.y, point.x - 2.0F, point.y + 2.5F,
                                 ctx.theme().panel);
  }
}

void draw_wire(gui::UiContext &ctx, const IsoPoint &from, const IsoPoint &to,
               gui::UiColor color, bool selected) {
  const float middle_x = (from.x + to.x) * 0.5F;
  auto &draw = ctx.draw_list();
  const float width = selected ? 3.0F : 2.0F;
  draw.add_line(from.x, from.y, middle_x, from.y, width + 2.0F,
                alpha(ctx.theme().divider, 0.92F));
  draw.add_line(middle_x, from.y, middle_x, to.y, width + 2.0F,
                alpha(ctx.theme().divider, 0.92F));
  draw.add_line(middle_x, to.y, to.x, to.y, width + 2.0F,
                alpha(ctx.theme().divider, 0.92F));
  draw.add_line(from.x, from.y, middle_x, from.y, width, color);
  draw.add_line(middle_x, from.y, middle_x, to.y, width, color);
  draw.add_line(middle_x, to.y, to.x, to.y, width, color);
  const float direction = to.x >= middle_x ? 1.0F : -1.0F;
  ctx.draw_list().add_triangle(to.x, to.y, to.x - direction * 7.0F, to.y - 4.0F,
                               to.x - direction * 7.0F, to.y + 4.0F, color);
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
  return std::min(std::clamp(bounds.width * 0.24F, 148.0F, 196.0F),
                  bounds.width * 0.46F);
}

[[nodiscard]] float feed_details_width(const gui::Rect &bounds) noexcept {
  if (bounds.width < 680.0F) {
    return 0.0F;
  }
  return std::min(std::clamp(bounds.width * 0.29F, 222.0F, 270.0F),
                  bounds.width * 0.34F);
}

[[nodiscard]] std::optional<core::u32>
next_feed_node_index(const output::FeedMap &feed_map) noexcept {
  if (feed_map.maps.size() >= output::max_output_maps) {
    return std::nullopt;
  }
  for (core::u32 index = 0U; index < output::max_output_maps; ++index) {
    if (feed_map.map_for(index) == nullptr) {
      return index;
    }
  }
  return std::nullopt;
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

[[nodiscard]] std::string feed_name(core::u32 index) {
  std::array<char, 32U> text{};
  const int length =
      std::snprintf(text.data(), text.size(), "OUTPUT %02u", index + 1U);
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

[[nodiscard]] std::string feed_row_details(const output::OutputMap &map) {
  return std::string{map.projection == output::FeedProjection::PlanarSurface
                         ? "PHYSICAL  "
                         : "CANVAS  "} +
         feed_dimensions(map.source);
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

gui::Rect hardware_mapping_canvas(const gui::Rect &bounds) noexcept {
  const float palette_width = hardware_palette_width(bounds);
  const float details_width = hardware_details_width(bounds);
  return gui::Rect{bounds.x + palette_width, bounds.y + title_height,
                   std::max(bounds.width - palette_width - details_width, 0.0F),
                   std::max(bounds.height - title_height, 0.0F)};
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
  projection.origin_x += view_pan_x_;
  projection.origin_y += view_pan_y_;
  projection.tile_width *= view_zoom_;
  projection.tile_height *= view_zoom_;
  return projection;
}

gui::Rect
hardware_palette_item_bounds(const gui::Rect &bounds,
                             topology::HardwareDeviceKind kind) noexcept {
  const float palette_width = hardware_palette_width(bounds);
  return gui::Rect{bounds.x + 7.0F,
                   bounds.y + title_height + palette_header_height +
                       static_cast<float>(kind_index(kind)) * palette_row_step,
                   std::max(palette_width - 14.0F, 0.0F), palette_row_height};
}

gui::Rect
hardware_sync_mode_button_bounds(const gui::Rect &bounds) noexcept {
  const float palette_width = hardware_palette_width(bounds);
  const float setup_y = bounds.y + title_height + palette_header_height +
                        static_cast<float>(device_kinds.size()) *
                            palette_row_step +
                        4.0F;
  return gui::Rect{bounds.x + 9.0F, setup_y + 92.0F,
                   std::max(palette_width - 18.0F, 0.0F),
                   details_row_height - 2.0F};
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
                    bounds.y + bounds.height - 11.0F};
  }
  const float x =
      description->direction == topology::HardwarePortDirection::Input
          ? bounds.x
          : bounds.x + bounds.width;
  return IsoPoint{x, bounds.y + bounds.height - 13.0F};
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
    const gui::Rect remove_bounds{
        bounds.x + std::max(bounds.width - 130.0F, 0.0F), bounds.y + 3.0F,
        std::min(124.0F, bounds.width), 20.0F};
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

  // Navigation changes only this panel's screen-space projection. Device
  // placement and movement still unproject to the same integer document
  // grid, so no camera/view state leaks into topology authoring intents.
  if (ctx.middle_pointer_pressed()) {
    panning_ = canvas.contains(ctx.pointer_x(), ctx.pointer_y()) &&
               drag_kind_ == DragKind::None && !link_dragging_;
  }
  if (panning_ && ctx.middle_pointer_down()) {
    if (!ctx.middle_pointer_pressed()) {
      view_pan_x_ += ctx.pointer_dx();
      view_pan_y_ += ctx.pointer_dy();
    }
    frame.navigating = true;
  }
  if (ctx.middle_pointer_released()) {
    frame.navigating = frame.navigating || panning_;
    panning_ = false;
  }

  const float wheel = ctx.wheel();
  if (std::abs(wheel) > 0.0001F &&
      canvas.contains(ctx.pointer_x(), ctx.pointer_y()) &&
      drag_kind_ == DragKind::None && !link_dragging_) {
    const IsometricProjection old_projection = view_projection(bounds);
    const float old_zoom = view_zoom_;
    const float next_zoom = std::clamp(old_zoom * std::pow(1.15F, wheel),
                                       minimum_view_zoom, maximum_view_zoom);
    if (std::abs(next_zoom - old_zoom) > 0.0001F) {
      const float factor = next_zoom / old_zoom;
      const float next_origin_x =
          ctx.pointer_x() -
          (ctx.pointer_x() - old_projection.origin_x) * factor;
      const float next_origin_y =
          ctx.pointer_y() -
          (ctx.pointer_y() - old_projection.origin_y) * factor;
      const IsometricProjection base = hardware_mapping_projection(bounds);
      view_zoom_ = next_zoom;
      view_pan_x_ = next_origin_x - base.origin_x;
      view_pan_y_ = next_origin_y - base.origin_y;
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
  ctx.push_clip(canvas);
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
  const float palette_width = hardware_palette_width(bounds);
  const gui::Rect palette{bounds.x, bounds.y + title_height, palette_width,
                          std::max(bounds.height - title_height, 0.0F)};
  ctx.panel(palette, scale(ctx.theme().panel, 0.94F));
  ctx.panel(gui::Rect{palette.x + palette.width - 1.0F, palette.y, 1.0F,
                      palette.height},
            ctx.theme().divider);
  ctx.label(gui::Rect{palette.x + 9.0F, palette.y, palette.width - 18.0F,
                      palette_header_height},
            "DRAG TO PLACE", ctx.theme().text_muted);

  for (const topology::HardwareDeviceKind kind : device_kinds) {
    const gui::Rect item = hardware_palette_item_bounds(bounds, kind);
    const gui::Interaction interaction =
        ctx.grab(mix(id_, "hardware.palette", kind_index(kind)), item);
    ctx.panel(item, interaction.held
                        ? ctx.theme().selection
                        : (interaction.hovered ? ctx.theme().widget_hover
                                               : ctx.theme().row));
    border(ctx, item,
           interaction.held ? ctx.theme().accent : ctx.theme().divider);
    const gui::UiColor color = device_color(ctx.theme(), kind);
    draw_device_icon(ctx, kind, item.x + 10.0F, item.y + 9.0F, color);
    ctx.label(
        gui::Rect{item.x + 38.0F, item.y + 2.0F, item.width - 43.0F, 19.0F},
        topology::device_kind_name(kind), ctx.theme().text);
    ctx.label(
        gui::Rect{item.x + 38.0F, item.y + 20.0F, item.width - 43.0F, 16.0F},
        "CONFIGURED DEVICE", ctx.theme().text_muted);

    if (interaction.pressed) {
      drag_kind_ = DragKind::Palette;
      palette_kind_ = kind;
    }
    if (interaction.released && drag_kind_ == DragKind::Palette &&
        palette_kind_ == kind) {
      if (canvas.contains(ctx.pointer_x(), ctx.pointer_y())) {
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
  const float setup_y =
      palette.y + palette_header_height +
      static_cast<float>(device_kinds.size()) * palette_row_step + 4.0F;
  if (setup_y + 54.0F <= palette.y + palette.height) {
    ctx.panel(gui::Rect{palette.x + 7.0F, setup_y,
                        std::max(palette.width - 14.0F, 0.0F), 1.0F},
              ctx.theme().divider);
    ctx.label(gui::Rect{palette.x + 9.0F, setup_y + 4.0F,
                        std::max(palette.width - 18.0F, 0.0F), 19.0F},
              "PROJECT SETUP", ctx.theme().text_muted);
    const gui::Rect port_bounds{palette.x + 86.0F, setup_y + 26.0F,
                                std::max(palette.width - 95.0F, 0.0F),
                                details_row_height};
    ctx.label(
        gui::Rect{palette.x + 9.0F, setup_y + 26.0F, 74.0F, details_row_height},
        "Control port", ctx.theme().text_muted);
    double changed_port{};
    if (settings_numbers_.draw(
            ctx, mix(id_, "hardware.settings.control-port"), port_bounds, {},
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
    ctx.label(gui::Rect{palette.x + 9.0F, setup_y + 50.0F,
                        std::max(palette.width - 18.0F, 0.0F), 17.0F},
              setup_status, ctx.theme().text_muted);
  }

  // The project synchronization policy sits directly under the control port
  // because both are project settings, and because the sync groups this
  // policy governs are derived from the very cables drawn in this panel.
  const float sync_y = setup_y + 72.0F;
  const gui::Rect sync_mode_button = hardware_sync_mode_button_bounds(bounds);
  if (sync_y + 24.0F <= palette.y + palette.height) {
    const float rail_x = palette.x + 9.0F;
    const float rail_width = std::max(palette.width - 18.0F, 0.0F);
    ctx.panel(gui::Rect{palette.x + 7.0F, sync_y - 6.0F,
                        std::max(palette.width - 14.0F, 0.0F), 1.0F},
              ctx.theme().divider);
    ctx.label(gui::Rect{rail_x, sync_y - 2.0F, rail_width, 19.0F},
              "SYNC POLICY", ctx.theme().text_muted);

    sync::SyncPolicy edited = sync_policy;
    bool changed = false;
    const auto cycle_button = [&](std::string_view suffix, float y,
                                  std::string_view label,
                                  bool emphasised) -> bool {
      const gui::Rect button{sync_mode_button.x, y, sync_mode_button.width,
                             sync_mode_button.height};
      if (y + button.height > palette.y + palette.height) {
        return false;
      }
      const gui::Interaction interaction = ctx.grab(mix(id_, suffix), button);
      const gui::UiColor base = emphasised
                                    ? scale(ctx.theme().selection, 0.9F)
                                    : ctx.theme().widget;
      ctx.panel(button, interaction.hovered ? scale(base, 1.2F) : base);
      border(ctx, button, ctx.theme().divider);
      ctx.label(gui::Rect{button.x + 6.0F, button.y,
                          std::max(button.width - 12.0F, 0.0F), button.height},
                label,
                emphasised ? ctx.theme().text : ctx.theme().text_muted);
      return interaction.clicked;
    };

    std::string mode_label{"MODE: "};
    mode_label += sync::sync_mode_name(sync_policy.mode);
    if (cycle_button("hardware.sync.mode", sync_y + 20.0F, mode_label,
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

    float row_y = sync_y + 20.0F + details_row_height;
    const auto number_row = [&](std::string_view suffix,
                                std::string_view label, double value,
                                double minimum, double maximum,
                                double &result) -> bool {
      if (row_y + details_row_height > palette.y + palette.height) {
        return false;
      }
      ctx.label(gui::Rect{rail_x, row_y, 74.0F, details_row_height}, label,
                ctx.theme().text_muted);
      const gui::Rect field{rail_x + 77.0F, row_y,
                            std::max(rail_width - 77.0F, 0.0F),
                            details_row_height};
      double edited_value{};
      if (!sync_numbers_.draw(ctx, mix(id_, suffix), field, {}, value, 1.0,
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
      row_y += details_row_height;
      if (number_row("hardware.sync.height", "Height",
                     static_cast<double>(sync_policy.required_timing.height),
                     1.0, static_cast<double>(sync::max_sync_timing_height),
                     next)) {
        edited.required_timing.height =
            static_cast<core::u32>(std::llround(next));
        changed = true;
      }
      row_y += details_row_height;
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
      row_y += details_row_height;
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
      row_y += details_row_height;
      if (number_row("hardware.sync.group", "Swap group",
                     static_cast<double>(sync_policy.group), 1.0,
                     static_cast<double>(sync::max_sync_group_id), next)) {
        edited.group = static_cast<core::u32>(std::llround(next));
        changed = true;
      }
      row_y += details_row_height;
      if (number_row("hardware.sync.barrier", "Barrier",
                     static_cast<double>(sync_policy.barrier), 1.0,
                     static_cast<double>(sync::max_sync_barrier_id), next)) {
        edited.barrier = static_cast<core::u32>(std::llround(next));
        changed = true;
      }
      row_y += details_row_height + 2.0F;

      std::string failure_label{"ON FAILURE: "};
      failure_label += sync::sync_failure_policy_name(sync_policy.failure);
      if (cycle_button("hardware.sync.failure", row_y, failure_label,
                       sync_policy.failure ==
                           sync::SyncFailurePolicy::Strict)) {
        edited.failure = sync_policy.failure == sync::SyncFailurePolicy::Strict
            ? sync::SyncFailurePolicy::Degraded
            : sync::SyncFailurePolicy::Strict;
        changed = true;
      }
      row_y += details_row_height;

      std::string late_label{"LATE FRAME: "};
      late_label += sync::late_frame_policy_name(sync_policy.late_frame);
      if (cycle_button("hardware.sync.late-frame", row_y, late_label,
                       sync_policy.late_frame ==
                           sync::LateFramePolicy::HoldGroup)) {
        edited.late_frame =
            sync_policy.late_frame == sync::LateFramePolicy::HoldGroup
            ? sync::LateFramePolicy::AdvanceIndependently
            : sync::LateFramePolicy::HoldGroup;
        changed = true;
      }
      row_y += details_row_height + 2.0F;
    }

    if (row_y + 17.0F <= palette.y + palette.height) {
      const std::string_view sync_status =
          !sync_policy.authored
              ? std::string_view{"DEFAULT - EDIT TO SAVE"}
              : (sync_policy.requires_physical_sync()
                     ? std::string_view{"SAVED - VERIFIED IN OUTPUT MONITOR"}
                     : std::string_view{"SAVED - NO PHYSICAL TIMING CLAIM"});
      ctx.label(gui::Rect{rail_x, row_y, rail_width, 17.0F}, sync_status,
                ctx.theme().text_muted);
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


  ctx.push_clip(canvas);

  const auto input_at_pointer =
      [&]() -> std::optional<topology::HardwareEndpoint> {
    if (!canvas.contains(ctx.pointer_x(), ctx.pointer_y())) {
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
                    static_cast<int>(std::lround(view_zoom_ * 100.0F)));
  const std::string_view navigation_label{
      navigation_text.data(),
      static_cast<std::size_t>(std::max(navigation_length, 0))};
  const float navigation_width = ctx.font_width(navigation_label);
  ctx.label(gui::Rect{canvas.x + canvas.width - navigation_width - 10.0F,
                      canvas.y + canvas.height - 21.0F, navigation_width,
                      17.0F},
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

  if (map.devices.empty()) {
    ctx.label(gui::Rect{canvas.x + 16.0F, canvas.y + 34.0F,
                        canvas.width - 32.0F, 22.0F},
              "NO DEVICES CONFIGURED - DRAG ONE FROM THE PALETTE",
              ctx.theme().text_muted);
  }
  ctx.pop_clip();

  // Selection may have changed in the card/port pass above. The details rail
  // follows immediately and occupies space excluded from the grid, so its
  // controls never fight a device beneath them for pointer capture.
  const topology::HardwareDevice *const selected_device =
      selection_.kind == HardwareSelectionKind::Device
          ? map.device(selection_.node)
          : nullptr;
  sync_details(selected_device);
  const float details_width = hardware_details_width(bounds);
  if (details_width > 0.0F) {
    const gui::Rect details{bounds.x + bounds.width - details_width,
                            bounds.y + title_height, details_width,
                            std::max(bounds.height - title_height, 0.0F)};
    ctx.panel(details, scale(ctx.theme().panel, 0.96F));
    ctx.panel(gui::Rect{details.x, details.y, 1.0F, details.height},
              ctx.theme().divider);
    ctx.label(gui::Rect{details.x + 10.0F, details.y + 3.0F,
                        details.width - 20.0F, 22.0F},
              "DEVICE DETAILS", ctx.theme().text_muted);

    if (selected_device != nullptr && details_node_ == selected_device->node) {
      float y = details.y + 31.0F;
      const auto labelled_bounds = [&](std::string_view label) {
        ctx.label(gui::Rect{details.x + 9.0F, y, details_label_width,
                            details_row_height},
                  label, ctx.theme().text_muted);
        return gui::Rect{
            details.x + details_label_width + 9.0F, y,
            std::max(details.width - details_label_width - 18.0F, 0.0F),
            details_row_height};
      };
      const gui::UiId name_id =
          mix(id_, "hardware.details.name", details_node_.value());
      const gui::Rect name_bounds = labelled_bounds("Name");
      if (ctx.text_field(name_id, name_bounds, details_name_)) {
        details_name_dirty_ = true;
      }
      y += 28.0F;
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
        ctx.label(gui::Rect{address_bounds.x + 6.0F, address_bounds.y,
                            std::max(address_bounds.width - 12.0F, 0.0F),
                            address_bounds.height},
                  "LOCAL (127.0.0.1)", ctx.theme().text_muted);
      }
      y += 28.0F;
      if (render_server) {
        const gui::Rect target_bounds = labelled_bounds("Control");
        ctx.panel(target_bounds, ctx.theme().track);
        border(ctx, target_bounds, ctx.theme().divider);
        const std::optional<std::string> target =
            topology::lifecycle_endpoint_for(map, *selected_device);
        ctx.push_clip(target_bounds);
        ctx.label(gui::Rect{target_bounds.x + 6.0F, target_bounds.y,
                            std::max(target_bounds.width - 12.0F, 0.0F),
                            target_bounds.height},
                  target.has_value() ? std::string_view{*target}
                                     : std::string_view{"UNAVAILABLE"},
                  target.has_value() ? ctx.theme().text_muted
                                     : ctx.theme().warning);
        ctx.pop_clip();
        y += 27.0F;
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
        y += 27.0F;
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
        ctx.label(gui::Rect{details.x + 9.0F, y, details.width - 18.0F,
                            19.0F},
                  "OUTPUT DEPLOYMENT", ctx.theme().text_muted);
        y += 20.0F;
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
          const gui::Rect row{details.x + 9.0F, y,
                              details.width - 18.0F, 22.0F};
          if (ctx.button(mix(id_, "hardware.binding.select",
                             binding->node.value()),
                         row, label)) {
            details_output_binding_ = binding->node;
          }
          y += 24.0F;
        }
        ctx.label(gui::Rect{details.x + 9.0F, y,
                            details.width - 18.0F, 19.0F},
                  deployment_runtime == nullptr ||
                          !runtime_inventory_is_usable(deployment_runtime)
                      ? "START LISTENER, THEN WIRE VIDEO OUT"
                      : "WIRE VIDEO OUT TO CREATE OUTPUT",
                  deployment_runtime == nullptr ||
                          !runtime_inventory_is_usable(deployment_runtime)
                      ? ctx.theme().warning
                      : ctx.theme().text_muted);
        y += 22.0F;

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
            y += 27.0F;
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
          y += 27.0F;
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
            const gui::Rect repair{details.x + 9.0F, y,
                                   details.width - 18.0F, 22.0F};
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
            y += 25.0F;
          }
          ctx.label(gui::Rect{details.x + 9.0F, y,
                              details.width - 18.0F, 19.0F},
                    "DISCONNECT CABLE TO REMOVE OUTPUT",
                    ctx.theme().text_muted);
          y += 22.0F;
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
          y += 27.0F;
          const std::string_view settings_source =
              hardware_window_settings_source_text(
                  selected_device->window_mode);
          ctx.label(gui::Rect{details.x + details_label_width + 9.0F, y,
                              std::max(details.width - details_label_width -
                                           18.0F,
                                       0.0F),
                              18.0F},
                    settings_source,
                    selected_device->window_mode ==
                            topology::HardwareWindowMode::Inherit
                        ? ctx.theme().warning
                        : ctx.theme().text_muted);
          y += 20.0F;
          emit_signed_number("Desktop X", topology::schema::desktop_x, 5U,
                             selected_device->desktop_x);
          y += 27.0F;
          emit_signed_number("Desktop Y", topology::schema::desktop_y, 6U,
                             selected_device->desktop_y);
          y += 27.0F;
        }
      }
      if (!explicit_output_bindings) {
        const core::u32 maximum_width =
            render_server ? topology::max_hardware_output_width : 32'768U;
        const core::u32 maximum_height =
            render_server ? topology::max_hardware_output_height : 32'768U;
        emit_number("Width", topology::schema::width, 0U,
                    selected_device->width, 1U, maximum_width);
        y += 27.0F;
        emit_number("Height", topology::schema::height, 1U,
                    selected_device->height, 1U, maximum_height);
        y += 27.0F;
        emit_number("Refresh", topology::schema::refresh_numerator, 2U,
                    selected_device->refresh_numerator, 1U, 240'000U);
        y += 27.0F;
        emit_number("Rate div", topology::schema::refresh_denominator, 3U,
                    selected_device->refresh_denominator, 1U, 1001U);
      }

      if (render_server) {
        y += 30.0F;
        ctx.panel(gui::Rect{details.x + 9.0F, y, details.width - 18.0F, 1.0F},
                  ctx.theme().divider);
        y += 5.0F;
        ctx.label(gui::Rect{details.x + 9.0F, y, details.width - 18.0F, 19.0F},
                  "LIVE SYSTEM", ctx.theme().text_muted);
        y += 20.0F;

        const auto live_row = [&](std::string_view label,
                                  std::string_view value, gui::UiColor color) {
          if (y + 19.0F > details.y + details.height - 5.0F) {
            return false;
          }
          ctx.label(gui::Rect{details.x + 9.0F, y, details_label_width, 19.0F},
                    label, ctx.theme().text_muted);
          const gui::Rect value_bounds{
              details.x + details_label_width + 9.0F, y,
              std::max(details.width - details_label_width - 18.0F, 0.0F),
              19.0F};
          ctx.push_clip(value_bounds);
          ctx.label(value_bounds, value, color);
          ctx.pop_clip();
          y += 20.0F;
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
      ctx.label(gui::Rect{details.x + 10.0F, details.y + 34.0F,
                          details.width - 20.0F, 22.0F},
                "CABLE", ctx.theme().text);
      if (link != nullptr) {
        const std::string route = std::to_string(link->from.device.value()) +
                                  ":" + std::to_string(link->from.port) +
                                  "  ->  " +
                                  std::to_string(link->to.device.value()) +
                                  ":" + std::to_string(link->to.port);
        ctx.label(gui::Rect{details.x + 10.0F, details.y + 59.0F,
                            details.width - 20.0F, 22.0F},
                  route, ctx.theme().keyframe);
      }
    } else {
      ctx.label(gui::Rect{details.x + 10.0F, details.y + 35.0F,
                          details.width - 20.0F, 44.0F},
                "Select a device or cable", ctx.theme().text_muted);
    }
  }

  frame.dragging = drag_kind_ != DragKind::None || link_dragging_;
  frame.editing = frame.editing || settings_numbers_.live() ||
                  sync_numbers_.live();
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
      std::max(bounds.height - title_height - 52.0F, 0.0F)};
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
                                        core::u64 show_frame) {
  FeedMappingFrame frame{};
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return frame;
  }
  if (ctx.pointer_pressed()) {
    focused_ = bounds.contains(ctx.pointer_x(), ctx.pointer_y());
  }
  if (selected_node_index_.has_value() &&
      feed_map.map_for(*selected_node_index_) == nullptr) {
    selected_node_index_.reset();
    drag_kind_ = DragKind::None;
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
        selected_node_index_.has_value()
            ? feed_map.map_for(*selected_node_index_)
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
    mesh_browse_pending_ = false;
    surface_name_dirty_ = false;
  };
  sync_surface_details();

  ctx.push_clip(bounds);
  ctx.panel(bounds);
  const bool valid_selection =
      selected_node_index_.has_value() &&
      feed_map.map_for(*selected_node_index_) != nullptr;
  title(ctx, bounds, "FEED MAPPING",
        valid_selection
            ? std::string_view{}
            : (feed_map.maps.empty() ? std::string_view{"NOT CONFIGURED"}
                                     : std::string_view{"CONFIGURED"}));
  if (focused_ && valid_selection && !ctx.typing() &&
      !surface_numbers_.live() && ctx.key_pressed(gui::Key::Delete)) {
    frame.remove = FeedMapDeleteRequest{*selected_node_index_};
    frame.consume_delete = true;
  }

  const float list_width = feed_list_width(bounds);
  const gui::Rect content{bounds.x, bounds.y + title_height, bounds.width,
                          std::max(bounds.height - title_height, 0.0F)};
  const gui::Rect list{content.x, content.y, list_width, content.height};
  const gui::Rect work{content.x + list_width, content.y,
                       std::max(content.width - list_width, 0.0F),
                       content.height};
  ctx.panel(list, scale(ctx.theme().panel, 0.94F));
  ctx.panel(gui::Rect{list.x + list.width - 1.0F, list.y, 1.0F, list.height},
            ctx.theme().divider);
  ctx.label(gui::Rect{list.x + 9.0F, list.y, list.width - 18.0F,
                      feed_list_header_height},
            "OUTPUT REGIONS", ctx.theme().text_muted);

  const std::optional<core::u32> add_index = next_feed_node_index(feed_map);
  const bool can_add =
      canvas.width > 0U && canvas.height > 0U && add_index.has_value();
  const gui::Rect add_bounds{list.x + 6.0F, list.y + feed_list_header_height,
                             std::max(list.width - 12.0F, 0.0F),
                             feed_add_button_height};
  const gui::Interaction add_interaction =
      can_add ? ctx.grab(mix(id_, "feed.add"), add_bounds) : gui::Interaction{};
  ctx.panel(add_bounds,
            can_add ? (add_interaction.hovered ? ctx.theme().widget_hover
                                               : ctx.theme().widget)
                    : ctx.theme().track);
  border(ctx, add_bounds,
         can_add && add_interaction.hovered ? ctx.theme().accent
                                            : ctx.theme().divider);
  ctx.label(add_bounds,
            !add_index.has_value()
                ? std::string_view{"REGION LIMIT"}
                : (can_add ? std::string_view{"+ ADD REGION"}
                           : std::string_view{"CANVAS REQUIRED"}),
            can_add ? ctx.theme().text : ctx.theme().text_muted);
  if (add_interaction.clicked) {
    frame.add = FeedMapAddRequest{
        *add_index, scene::CanvasRect{0U, 0U, canvas.width, canvas.height}};
  }

  for (std::size_t index = 0U; index < feed_map.maps.size(); ++index) {
    const output::OutputMap &map = feed_map.maps[index];
    const gui::Rect row{list.x + 6.0F,
                        list.y + feed_list_header_height +
                            feed_add_button_step +
                            static_cast<float>(index) * feed_row_step,
                        std::max(list.width - 12.0F, 0.0F), feed_row_height};
    const float action_width = std::min(28.0F, row.width);
    const gui::Rect row_content{row.x, row.y,
                                std::max(row.width - action_width, 0.0F),
                                row.height};
    const gui::Rect remove{row_content.x + row_content.width, row.y,
                           action_width, row.height};
    const gui::Interaction interaction =
        ctx.grab(mix(id_, "feed.row", map.node_index), row_content);
    const gui::Interaction remove_interaction =
        ctx.grab(mix(id_, "feed.row.remove", map.node_index), remove);
    const bool selected = selected_node_index_ == map.node_index;
    ctx.panel(row,
              selected ? ctx.theme().selection
                       : (interaction.hovered || remove_interaction.hovered
                              ? ctx.theme().widget_hover
                              : ctx.theme().row));
    ctx.panel(gui::Rect{row.x + 2.0F, row.y + 4.0F, 3.0F, row.height - 8.0F},
              feed_color(ctx.theme(), map.node_index));
    ctx.label(gui::Rect{row.x + 10.0F, row.y + 1.0F,
                        std::max(row_content.width - 14.0F, 0.0F), 17.0F},
              feed_output_identity(map.node_index), ctx.theme().text);
    ctx.label(gui::Rect{row.x + 10.0F, row.y + 16.0F,
                        std::max(row_content.width - 14.0F, 0.0F), 15.0F},
              feed_row_details(map), ctx.theme().text_muted);
    ctx.panel(gui::Rect{remove.x, remove.y + 6.0F, 1.0F,
                        std::max(remove.height - 12.0F, 0.0F)},
              ctx.theme().divider);
    draw_remove(ctx, remove.x + remove.width * 0.5F,
                remove.y + remove.height * 0.5F,
                remove_interaction.hovered ? ctx.theme().warning
                                           : ctx.theme().text_muted);
    if (interaction.clicked) {
      selected_node_index_ = map.node_index;
      frame.selection = map.node_index;
      if (map.projection == output::FeedProjection::PlanarSurface &&
          feed_map.surface_for(map.surface) != nullptr) {
        selected_surface_node_ = map.surface;
        frame.surface_selection = map.surface;
      }
    }
    if (remove_interaction.clicked) {
      frame.remove = FeedMapDeleteRequest{map.node_index};
    }
  }

  float surface_y = list.y + feed_list_header_height + feed_add_button_step +
                    static_cast<float>(feed_map.maps.size()) * feed_row_step +
                    7.0F;
  ctx.label(gui::Rect{list.x + 9.0F, surface_y, list.width - 18.0F,
                      feed_details_label_height},
            "PROJECTION SURFACES", ctx.theme().text_muted);
  surface_y += 19.0F;
  const bool can_add_surface =
      scene::valid(canvas) &&
      feed_map.surfaces.size() < output::max_projection_surfaces;
  const gui::Rect add_surface_bounds{list.x + 6.0F, surface_y,
                                     std::max(list.width - 12.0F, 0.0F),
                                     feed_add_button_height};
  const gui::Interaction add_surface_interaction =
      can_add_surface
          ? ctx.grab(mix(id_, "feed.surface.add"), add_surface_bounds)
          : gui::Interaction{};
  ctx.panel(add_surface_bounds,
            can_add_surface
                ? (add_surface_interaction.hovered ? ctx.theme().widget_hover
                                                   : ctx.theme().widget)
                : ctx.theme().track);
  border(ctx, add_surface_bounds,
         can_add_surface && add_surface_interaction.hovered
             ? ctx.theme().accent
             : ctx.theme().divider);
  ctx.label(add_surface_bounds,
            can_add_surface ? std::string_view{"+ ADD SURFACE"}
                            : std::string_view{"SURFACE LIMIT"},
            can_add_surface ? ctx.theme().text : ctx.theme().text_muted);
  if (add_surface_interaction.clicked) {
    frame.surface_add =
        FeedSurfaceAddRequest{default_surface_name(feed_map.surfaces.size()),
                              default_surface(canvas)};
  }
  surface_y += feed_add_button_step;
  const gui::Rect import_mesh_bounds{list.x + 6.0F, surface_y,
                                     std::max(list.width - 12.0F, 0.0F),
                                     feed_add_button_height};
  const gui::Interaction import_mesh_interaction =
      can_add_surface
          ? ctx.grab(mix(id_, "feed.surface.import-mesh"), import_mesh_bounds)
          : gui::Interaction{};
  ctx.panel(import_mesh_bounds,
            can_add_surface
                ? (import_mesh_interaction.hovered ? ctx.theme().widget_hover
                                                   : ctx.theme().widget)
                : ctx.theme().track);
  border(ctx, import_mesh_bounds,
         can_add_surface && import_mesh_interaction.hovered
             ? ctx.theme().accent
             : ctx.theme().divider);
  ctx.label(import_mesh_bounds,
            can_add_surface ? std::string_view{"+ IMPORT UV MESH"}
                            : std::string_view{"SURFACE LIMIT"},
            can_add_surface ? ctx.theme().text : ctx.theme().text_muted);
  if (import_mesh_interaction.clicked) {
    FeedSurfaceAddRequest request{
        default_mesh_surface_name(feed_map.surfaces.size()),
        default_mesh_surface(canvas),
        0U,
        output::ProjectionSurfaceShape::ImportedMesh,
        0.25F,
        32U,
        {},
        1.0F};
    request.assign_node_index = selected_node_index_;
    frame.surface_add = std::move(request);
  }
  surface_y += feed_surface_row_step;
  for (const output::ProjectionSurface &surface : feed_map.surfaces) {
    const gui::Rect row{list.x + 6.0F, surface_y,
                        std::max(list.width - 12.0F, 0.0F),
                        feed_surface_row_height};
    const float remove_width = std::min(28.0F, row.width);
    const float lock_width =
        std::min(48.0F, std::max(row.width - remove_width, 0.0F));
    const float action_width = remove_width + lock_width;
    const gui::Rect row_content{row.x, row.y,
                                std::max(row.width - action_width, 0.0F),
                                row.height};
    const gui::Rect lock{row_content.x + row_content.width, row.y, lock_width,
                         row.height};
    const gui::Rect remove{lock.x + lock.width, row.y, remove_width,
                           row.height};
    const gui::Interaction interaction =
        surface.locked
            ? gui::Interaction{}
            : ctx.grab(mix(id_, "feed.surface.row", surface.node.value()),
                       row_content);
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
                                  remove_interaction.hovered
                              ? ctx.theme().widget_hover
                              : ctx.theme().row));
    ctx.panel(gui::Rect{row.x + 2.0F, row.y + 4.0F, 3.0F, row.height - 8.0F},
              selected ? ctx.theme().accent : ctx.theme().keyframe);
    ctx.label(gui::Rect{row.x + 10.0F, row.y,
                        std::max(row_content.width - 14.0F, 0.0F), row.height},
              surface.name.empty() ? std::string_view{"UNTITLED SURFACE"}
                                   : std::string_view{surface.name},
              surface.locked
                  ? ctx.theme().text_muted
                  : (selected ? ctx.theme().text : ctx.theme().text_muted));
    ctx.panel(gui::Rect{lock.x, lock.y + 5.0F, 1.0F,
                        std::max(lock.height - 10.0F, 0.0F)},
              ctx.theme().divider);
    ctx.label(lock, surface.locked ? std::string_view{"LOCKED"}
                                   : std::string_view{"LOCK"},
              surface.locked || lock_interaction.hovered
                  ? ctx.theme().warning
                  : ctx.theme().text_muted);
    ctx.panel(gui::Rect{remove.x, remove.y + 5.0F, 1.0F,
                        std::max(remove.height - 10.0F, 0.0F)},
              ctx.theme().divider);
    draw_remove(ctx, remove.x + remove.width * 0.5F,
                remove.y + remove.height * 0.5F,
                remove_interaction.hovered ? ctx.theme().warning
                                           : ctx.theme().text_muted);
    if (interaction.clicked) {
      selected_surface_node_ = surface.node;
      frame.surface_selection = surface.node;
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
    surface_y += feed_surface_row_step;
  }
  sync_surface_details();

  ctx.panel(work, scale(ctx.theme().background, 0.76F));
  const output::OutputMap *const selected_map =
      selected_node_index_.has_value() ? feed_map.map_for(*selected_node_index_)
                                       : nullptr;
  const output::ProjectionSurface *const mapping_surface =
      selected_map != nullptr &&
              selected_map->projection == output::FeedProjection::PlanarSurface
          ? feed_map.surface_for(selected_map->surface)
          : nullptr;
  const bool selected_wants_physical =
      selected_map != nullptr &&
      selected_map->projection == output::FeedProjection::PlanarSurface;
  const bool physical_domain = mapping_surface != nullptr;
  const bool uv_mesh_domain =
      mapping_surface != nullptr &&
      mapping_surface->shape == output::ProjectionSurfaceShape::ImportedMesh;
  if (uv_mesh_domain && drag_kind_ != DragKind::None) {
    drag_kind_ = DragKind::None;
  }
  // This is a destination-UV preview: stage origin, orientation, and uniform
  // scale cannot change it. Keying only the asset identity avoids parsing the
  // whole model again after every live calibration scrub.
  if (uv_mesh_domain &&
      (preview_mesh_node_ != mapping_surface->node ||
       preview_mesh_source_ != mapping_surface->mesh_source)) {
    preview_mesh_node_ = mapping_surface->node;
    preview_mesh_source_ = mapping_surface->mesh_source;
    preview_mesh_.reset();
    auto loaded = render::load_projection_mesh(
        mapping_surface->mesh_source, mapping_surface->geometry,
        mapping_surface->mesh_scale);
    if (loaded) {
      preview_mesh_.emplace(std::move(loaded).value());
    }
  }
  const scene::Canvas mapping_canvas =
      physical_domain ? mapping_surface->geometry.raster : canvas;
  const gui::Rect fitted = feed_mapping_canvas(bounds, mapping_canvas);
  if (fitted.width > 0.0F && fitted.height > 0.0F) {
    ctx.panel(fitted, gui::UiColor{0.018F, 0.023F, 0.034F, 1.0F});
    for (core::u32 division = 1U; division < 8U; ++division) {
      const float fraction = static_cast<float>(division) / 8.0F;
      ctx.draw_list().add_line(
          fitted.x + fitted.width * fraction, fitted.y,
          fitted.x + fitted.width * fraction, fitted.y + fitted.height,
          division == 4U ? 1.0F : 0.6F,
          alpha(ctx.theme().widget, division == 4U ? 0.64F : 0.34F));
      ctx.draw_list().add_line(
          fitted.x, fitted.y + fitted.height * fraction,
          fitted.x + fitted.width, fitted.y + fitted.height * fraction,
          division == 4U ? 1.0F : 0.6F,
          alpha(ctx.theme().widget, division == 4U ? 0.64F : 0.34F));
    }
    border(ctx, fitted, ctx.theme().widget_hover);
  }

  if (mapping_canvas.width > 0U && mapping_canvas.height > 0U &&
      fitted.width > 0.0F && fitted.height > 0.0F) {
    const float scale_factor =
        fitted.width / static_cast<float>(mapping_canvas.width);
    for (const output::OutputMap &map : feed_map.maps) {
      const bool in_domain =
          physical_domain
              ? map.projection == output::FeedProjection::PlanarSurface &&
                    map.surface == mapping_surface->node
              : !selected_wants_physical &&
                    map.projection == output::FeedProjection::CanvasSlice;
      if (!in_domain) {
        continue;
      }
      scene::CanvasRect source = map.source;
      if (drag_kind_ != DragKind::None && drag_node_index_ == map.node_index) {
        source = preview_;
      }
      gui::Rect rectangle =
          feed_map_screen_rect(fitted, mapping_canvas, source);
      const bool selected = selected_node_index_ == map.node_index;

      // The resize handle gets first refusal over the rectangle body.
      const gui::Rect handle{rectangle.x + rectangle.width - 6.0F,
                             rectangle.y + rectangle.height - 6.0F, 12.0F,
                             12.0F};
      gui::Interaction handle_interaction{};
      if (selected && !uv_mesh_domain) {
        handle_interaction =
            ctx.grab(mix(id_, "feed.resize", map.node_index), handle);
        if (handle_interaction.pressed) {
          drag_kind_ = DragKind::Resize;
          drag_node_index_ = map.node_index;
          drag_original_ = map.source;
          preview_ = map.source;
          drag_start_x_ = ctx.pointer_x();
          drag_start_y_ = ctx.pointer_y();
        }
        if (handle_interaction.held && drag_kind_ == DragKind::Resize &&
            drag_node_index_ == map.node_index) {
          const core::i64 dx = static_cast<core::i64>(
              std::llround((ctx.pointer_x() - drag_start_x_) / scale_factor));
          const core::i64 dy = static_cast<core::i64>(
              std::llround((ctx.pointer_y() - drag_start_y_) / scale_factor));
          preview_ = resize_feed_rect(drag_original_, mapping_canvas, dx, dy);
          source = preview_;
          rectangle = feed_map_screen_rect(fitted, mapping_canvas, source);
        }
        if (handle_interaction.released && drag_kind_ == DragKind::Resize &&
            drag_node_index_ == map.node_index) {
          if (preview_ != drag_original_) {
            frame.edit = FeedMapRectEdit{map.node_index, preview_};
          }
          drag_kind_ = DragKind::None;
        }
      }

      const gui::Interaction body_interaction =
          ctx.grab(mix(id_, "feed.rectangle", map.node_index), rectangle);
      if (body_interaction.pressed) {
        selected_node_index_ = map.node_index;
        frame.selection = map.node_index;
        if (!uv_mesh_domain) {
          drag_kind_ = DragKind::Move;
          drag_node_index_ = map.node_index;
          drag_original_ = map.source;
          preview_ = map.source;
          drag_start_x_ = ctx.pointer_x();
          drag_start_y_ = ctx.pointer_y();
        }
      }
      if (body_interaction.held && drag_kind_ == DragKind::Move &&
          drag_node_index_ == map.node_index) {
        const core::i64 dx = static_cast<core::i64>(
            std::llround((ctx.pointer_x() - drag_start_x_) / scale_factor));
        const core::i64 dy = static_cast<core::i64>(
            std::llround((ctx.pointer_y() - drag_start_y_) / scale_factor));
        preview_ = move_feed_rect(drag_original_, mapping_canvas, dx, dy);
        source = preview_;
        rectangle = feed_map_screen_rect(fitted, mapping_canvas, source);
      }
      if (body_interaction.released && drag_kind_ == DragKind::Move &&
          drag_node_index_ == map.node_index) {
        if (preview_ != drag_original_) {
          frame.edit = FeedMapRectEdit{map.node_index, preview_};
        }
        drag_kind_ = DragKind::None;
      }

      const gui::UiColor color = feed_color(ctx.theme(), map.node_index);
      ctx.panel(rectangle, alpha(color, selected ? 0.30F : 0.19F));
      border(ctx, rectangle, selected ? ctx.theme().accent : color,
             selected ? 2.0F : 1.0F);
      ctx.panel(gui::Rect{rectangle.x + 1.0F, rectangle.y + 1.0F,
                          std::max(rectangle.width - 2.0F, 0.0F), 18.0F},
                alpha(color, selected ? 0.78F : 0.56F));
      ctx.label(gui::Rect{rectangle.x + 7.0F, rectangle.y + 1.0F,
                          std::max(rectangle.width - 14.0F, 0.0F), 17.0F},
                feed_name(map.node_index), ctx.theme().text);
      if (selected && !uv_mesh_domain) {
        const gui::Rect current_handle{rectangle.x + rectangle.width - 5.0F,
                                       rectangle.y + rectangle.height - 5.0F,
                                       10.0F, 10.0F};
        ctx.panel(current_handle, ctx.theme().accent);
        border(ctx, current_handle, ctx.theme().background);
      }
    }

    if (uv_mesh_domain && preview_mesh_.has_value()) {
      const output::ProjectionMesh &mesh = *preview_mesh_;
      const std::size_t triangle_count = mesh.indices.size() / 3U;
      constexpr std::size_t max_preview_triangles = 1'024U;
      const std::size_t drawn = std::min(triangle_count, max_preview_triangles);
      const auto screen =
          [&fitted](const output::ProjectionMeshVertex &vertex) {
            return IsoPoint{fitted.x + vertex.destination_u * fitted.width,
                            fitted.y + vertex.destination_v * fitted.height};
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
      ctx.label(gui::Rect{fitted.x + 8.0F,
                          fitted.y + std::max(fitted.height - 24.0F, 0.0F),
                          std::max(fitted.width - 16.0F, 0.0F), 18.0F},
                "UV MESH PREVIEW UNAVAILABLE", ctx.theme().warning);
    }
  }

  if (feed_map.maps.empty()) {
    ctx.label(
        gui::Rect{work.x + 18.0F, work.y + 36.0F, work.width - 36.0F, 22.0F},
        "NO OUTPUT MAPS CONFIGURED", ctx.theme().text_muted);
    ctx.label(
        gui::Rect{work.x + 18.0F, work.y + 58.0F, work.width - 36.0F, 18.0F},
        "Add document-backed output regions to begin.", ctx.theme().text_muted);
  } else {
    std::array<char, 72U> canvas_text{};
    const int length = std::snprintf(
        canvas_text.data(), canvas_text.size(),
        uv_mesh_domain
            ? "UV0 CONNECTOR  %u x %u  |  REGION LOCKED"
            : (physical_domain ? "SURFACE RASTER  %u x %u"
                               : (selected_wants_physical
                                      ? "NO VALID SURFACE RASTER  %u x %u"
                                      : "SCENE CANVAS  %u x %u")),
        mapping_canvas.width, mapping_canvas.height);
    ctx.label(
        gui::Rect{work.x + 12.0F, work.y + 5.0F, work.width - 24.0F, 18.0F},
        std::string_view{canvas_text.data(),
                         static_cast<std::size_t>(std::max(length, 0))},
        ctx.theme().text_muted);
  }

  const float details_width = feed_details_width(bounds);
  if (details_width > 0.0F) {
    const gui::Rect details{bounds.x + bounds.width - details_width,
                            bounds.y + title_height, details_width,
                            std::max(bounds.height - title_height, 0.0F)};
    ctx.panel(details, scale(ctx.theme().panel, 0.96F));
    ctx.panel(gui::Rect{details.x, details.y, 1.0F, details.height},
              ctx.theme().divider);
    const gui::Rect projection_title{details.x + 10.0F, details.y + 3.0F,
                                     details.width - 20.0F, 20.0F};
    if (selected_map != nullptr) {
      ctx.label(projection_title,
                feed_output_identity(selected_map->node_index),
                ctx.theme().text);
    } else {
      ctx.label(projection_title, "OUTPUT PROJECTION", ctx.theme().text_muted);
    }

    float details_y = details.y + 28.0F;
    if (selected_map != nullptr) {
      ctx.label(gui::Rect{details.x + 10.0F, details_y, details.width - 20.0F,
                          feed_details_label_height},
                feed_runtime_binding(selected_map->node_index),
                ctx.theme().text_muted);
      details_y += 17.0F;
      const bool has_surfaces = !feed_map.surfaces.empty();
      const float mode_width = (details.width - 23.0F) * 0.5F;
      const gui::Rect canvas_mode{
          details.x + 10.0F, details_y,
          has_surfaces ? mode_width : details.width - 20.0F, 22.0F};
      const gui::Rect physical_mode{
          has_surfaces ? canvas_mode.x + mode_width + 3.0F : canvas_mode.x,
          has_surfaces ? details_y : details_y + 25.0F,
          has_surfaces ? mode_width : details.width - 20.0F, 22.0F};
      if (ctx.button(mix(id_, "feed.mode.canvas", selected_map->node_index),
                     canvas_mode, "CANVAS") &&
          selected_map->projection != output::FeedProjection::CanvasSlice) {
        const scene::CanvasRect source =
            scene::contains(canvas, selected_map->source)
                ? selected_map->source
                : scene::whole(canvas);
        frame.projection_edit =
            FeedProjectionEdit{selected_map->node_index,
                               output::FeedProjection::CanvasSlice,
                               {},
                               source};
      }
      if (has_surfaces &&
          ctx.button(mix(id_, "feed.mode.physical", selected_map->node_index),
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
            selected_map->node_index, output::FeedProjection::PlanarSurface,
            target->node, scene::whole(target->geometry.raster)};
      }
      if (!has_surfaces) {
        ctx.panel(physical_mode, ctx.theme().track);
        border(ctx, physical_mode, ctx.theme().divider);
        ctx.label(gui::Rect{physical_mode.x + 7.0F, physical_mode.y,
                            std::max(physical_mode.width - 14.0F, 0.0F),
                            physical_mode.height},
                  "NO SURFACES - ADD ONE AT LEFT", ctx.theme().text_muted);
      }
      border(ctx,
             selected_map->projection == output::FeedProjection::CanvasSlice
                 ? canvas_mode
                 : physical_mode,
             ctx.theme().accent, 1.5F);
      details_y += has_surfaces ? 26.0F : 51.0F;

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
      ctx.label(
          gui::Rect{details.x + 10.0F, details_y, details.width - 20.0F, 18.0F},
          status, status_color);
      details_y += 21.0F;

      ctx.label(gui::Rect{details.x + 10.0F, details_y, details.width - 20.0F,
                          feed_details_label_height},
                "SURFACE", ctx.theme().text_muted);
      details_y += 17.0F;
      const gui::Rect previous_surface{details.x + 10.0F, details_y, 24.0F,
                                       22.0F};
      const gui::Rect next_surface{details.x + details.width - 34.0F, details_y,
                                   24.0F, 22.0F};
      const gui::Rect surface_choice{
          previous_surface.x + previous_surface.width + 3.0F, details_y,
          std::max(next_surface.x - previous_surface.x -
                       previous_surface.width - 6.0F,
                   0.0F),
          22.0F};
      const output::ProjectionSurface *assigned =
          feed_map.surface_for(selected_map->surface);
      std::string_view assignment{"NO SURFACE"};
      if (assigned != nullptr) {
        assignment = assigned->name.empty()
                         ? std::string_view{"UNTITLED SURFACE"}
                         : std::string_view{assigned->name};
      } else if (selected_map->surface != doc::NodeId{}) {
        assignment = "MISSING SURFACE";
      }
      bool previous_clicked{};
      bool next_clicked{};
      if (has_surfaces) {
        previous_clicked = ctx.button(
            mix(id_, "feed.surface.previous", selected_map->node_index),
            previous_surface, "<");
        next_clicked =
            ctx.button(mix(id_, "feed.surface.next", selected_map->node_index),
                       next_surface, ">");
        next_clicked = ctx.button(mix(id_, "feed.surface.choice",
                                      selected_map->node_index),
                                  surface_choice, assignment) ||
                       next_clicked;
      } else {
        ctx.panel(previous_surface, ctx.theme().track);
        ctx.panel(surface_choice, ctx.theme().track);
        ctx.panel(next_surface, ctx.theme().track);
        ctx.label(surface_choice, assignment, ctx.theme().text_muted);
      }
      if ((previous_clicked || next_clicked) && has_surfaces) {
        std::size_t current = 0U;
        for (std::size_t index = 0U; index < feed_map.surfaces.size();
             ++index) {
          if (feed_map.surfaces[index].node == selected_map->surface) {
            current = index;
            break;
          }
        }
        const std::size_t selected_index =
            previous_clicked ? (current + feed_map.surfaces.size() - 1U) %
                                   feed_map.surfaces.size()
                             : (current + 1U) % feed_map.surfaces.size();
        const output::ProjectionSurface &target =
            feed_map.surfaces[selected_index];
        selected_surface_node_ = target.node;
        frame.surface_selection = target.node;
        frame.projection_edit = FeedProjectionEdit{
            selected_map->node_index, output::FeedProjection::PlanarSurface,
            target.node, scene::whole(target.geometry.raster)};
      }
      details_y += 29.0F;
    } else {
      ctx.label(
          gui::Rect{details.x + 10.0F, details_y, details.width - 20.0F, 40.0F},
          "Select an output region to choose its projection.",
          ctx.theme().text_muted);
      details_y += 73.0F;
    }

    sync_surface_details();
    ctx.panel(
        gui::Rect{details.x + 9.0F, details_y, details.width - 18.0F, 1.0F},
        ctx.theme().divider);
    details_y += 6.0F;
    ctx.label(
        gui::Rect{details.x + 10.0F, details_y, details.width - 20.0F, 20.0F},
        "SURFACE CALIBRATION", ctx.theme().text_muted);
    details_y += 22.0F;

    const output::ProjectionSurface *const details_surface =
        feed_map.surface_for(surface_details_node_);
    if (details_surface != nullptr) {
      const bool details_locked = details_surface->locked;
      if (details_locked) {
        ctx.label(gui::Rect{details.x + details.width - 78.0F,
                            details_y - 22.0F, 68.0F, 20.0F},
                  "LOCKED", ctx.theme().warning);
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
      const auto field_columns = [&](std::size_t count) {
        std::array<gui::Rect, 3U> fields{};
        const float gap = 3.0F;
        const float available =
            details.width - 20.0F - gap * static_cast<float>(count - 1U);
        const float width = available / static_cast<float>(count);
        for (std::size_t index = 0U; index < count; ++index) {
          fields[index] = gui::Rect{
              details.x + 10.0F + static_cast<float>(index) * (width + gap),
              details_y, width, 22.0F};
        }
        return fields;
      };
      const auto labelled_columns = [&](std::string_view label,
                                        std::size_t count) {
        constexpr float label_width = 50.0F;
        constexpr float gap = 3.0F;
        ctx.label(gui::Rect{details.x + 10.0F, details_y, label_width, 22.0F},
                  label, ctx.theme().text_muted);
        std::array<gui::Rect, 3U> fields{};
        const float start_x = details.x + 10.0F + label_width + gap;
        const float available = details.width - 20.0F - label_width - gap -
                                gap * static_cast<float>(count - 1U);
        const float width = available / static_cast<float>(count);
        for (std::size_t index = 0U; index < count; ++index) {
          fields[index] =
              gui::Rect{start_x + static_cast<float>(index) * (width + gap),
                        details_y, width, 22.0F};
        }
        return fields;
      };

      auto fields = field_columns(3U);
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
      border(ctx, selected_shape, ctx.theme().accent, 1.5F);
      details_y += 27.0F;

      const gui::UiId name_id =
          mix(id_, "feed.surface.name", surface_details_node_.value());
      fields = labelled_columns("NAME", 1U);
      if (ctx.text_field(name_id, fields[0U], surface_name_)) {
        surface_name_dirty_ = true;
      }
      details_y += 27.0F;

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
      details_y += 27.0F;

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
      details_y += 27.0F;

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
        details_y += 27.0F;
      }

      if (surface_shape_ == output::ProjectionSurfaceShape::ImportedMesh) {
        const std::string mesh_status =
            mesh_source_status(surface_mesh_source_);
        ctx.label(gui::Rect{details.x + 10.0F, details_y, details.width - 20.0F,
                            16.0F},
                  mesh_status,
                  surface_mesh_source_.empty() ? ctx.theme().warning
                                               : ctx.theme().text_muted);
        details_y += 16.0F;
        if (ctx.button(mix(id_, "feed.surface.mesh.browse",
                           surface_details_node_.value()),
                       gui::Rect{details.x + 10.0F, details_y,
                                 details.width - 20.0F, 22.0F},
                       surface_mesh_source_.empty() ? "CHOOSE UV MESH"
                                                    : "REPLACE UV MESH")) {
          mesh_browse_pending_ = true;
          // A replacement may deliberately overwrite the same path. Forget
          // the key as well as the decoded value so the next evaluated frame
          // re-reads those new contents instead of accepting a stale hit.
          preview_mesh_node_ = {};
          preview_mesh_source_.clear();
          preview_mesh_.reset();
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
        details_y += 27.0F;
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
        details_y += 27.0F;
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
        details_y += 27.0F;
      } else {
        ctx.label(gui::Rect{details.x + 10.0F, details_y, details.width - 20.0F,
                            16.0F},
                  "AUTHORED COORDINATES / SHARED ORIGIN",
                  ctx.theme().text_muted);
        details_y += 20.0F;
      }

      fields = labelled_columns("RASTER", 2U);
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
      details_y += 29.0F;

      if (surface_name_dirty_ && ctx.focus() != name_id) {
        emit_surface_edit();
        surface_name_dirty_ = false;
      }
      frame.editing = !details_locked &&
                      (surface_numbers_.live() || ctx.focus() == name_id);
      if (details_locked) {
        ctx.pop_pointer_block();
      }
    } else {
      ctx.label(
          gui::Rect{details.x + 10.0F, details_y, details.width - 20.0F, 40.0F},
          "Add or select a physical surface to calibrate it.",
          ctx.theme().text_muted);
    }
  }

  frame.dragging = drag_kind_ != DragKind::None;
  ctx.pop_clip();
  return frame;
}

} // namespace mediaserver::editor
