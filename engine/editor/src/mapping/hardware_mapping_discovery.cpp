// The Hardware Mapping strip of machines and processors found on the
// network, and dropping one onto the map.

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

std::optional<gui::Interaction> HardwareMappingPanel::draw_discovery_strip(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const gui::Rect &bounds = f.bounds;
  const topology::HardwareMap &map = f.map;
  HardwareMappingFrame &frame = f.result;
  const gui::Rect &strip = f.strip;
  const float wheel = f.wheel;
  const topology::HardwareDiscovery &discovery = f.runtime.discovery;
  // Render machines found on the network. A card drags onto the map like a
  // palette entry, or a click adds it at a free spot; either way the new
  // Render Server already carries the machine's name, address, and node
  // index. A machine already on the map selects its card instead.
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
    } else if (discovery.machines.empty() && discovery.processors.empty()) {
      summary =
          discovery.state == topology::HardwareDiscoveryState::Searching
              ? "SEARCHING - NONE FOUND. RENDER MACHINES ANSWER ON UDP " +
                    std::to_string(discovery.port)
              : std::string{"NO RENDER MACHINE SEARCH IN THIS EDITOR"};
    } else {
      summary = std::to_string(discovery.machines.size()) +
                (discovery.machines.size() == 1U ? " MACHINE" : " MACHINES");
      if (!discovery.processors.empty()) {
        summary += ", " + std::to_string(discovery.processors.size()) +
                   (discovery.processors.size() == 1U ? " PROCESSOR"
                                                      : " PROCESSORS");
      }
    }
    // LED processors do not announce themselves: a search, on request,
    // tries their API port on every host of the local subnets.
    float search_width = 0.0F;
    if (discovery.processor_search_available) {
      search_width = 150.0F;
      const gui::Rect search{strip.right() - search_width - 6.0F,
                             strip.y + 2.0F, search_width,
                             discovery_header_height - 4.0F};
      if (pill_button(ctx, gui::ui_id(id_, "hardware.processors.search"), search,
                      gui::Icon::Search,
                      discovery.processor_search_running ? "Searching..."
                                                         : "Find processors",
                      "Search the local network for NovaStar and Brompton "
                      "processors: tries their API ports (8001 and 80) on "
                      "every host of this machine's subnets",
                      ButtonTone::Quiet,
                      !discovery.processor_search_running)) {
        frame.search_processors = true;
      }
      search_width += 12.0F;
    }
    ctx.label(gui::Rect{strip.x + heading_width + 21.0F, strip.y + 3.0F,
                        std::max(strip.width - heading_width - 30.0F -
                                     search_width,
                                 0.0F),
                        19.0F},
              summary, summary_color);

    const float step = discovery_card_width + discovery_card_gap;
    const float content =
        static_cast<float>(discovery.machines.size() +
                           discovery.processors.size()) *
            step +
        10.0F;
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
    const bool any_cards =
        !discovery.machines.empty() || !discovery.processors.empty();
    const bool can_scroll_left = any_cards && discovered_scroll_ > 0.5F;
    const bool can_scroll_right =
        any_cards && discovered_scroll_ < max_scroll - 0.5F;
    gui::Interaction left_interaction{};
    gui::Interaction right_interaction{};
    if (can_scroll_left) {
      left_interaction =
          ctx.grab(gui::ui_id(id_, "hardware.discovered.left"), left_arrow);
      if (left_interaction.clicked) {
        discovered_scroll_ = std::max(discovered_scroll_ - step, 0.0F);
      }
    }
    if (can_scroll_right) {
      right_interaction =
          ctx.grab(gui::ui_id(id_, "hardware.discovered.right"), right_arrow);
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
          ctx.grab(gui::ui_id(id_, "hardware.discovered", key), card);
      const DiscoveredStatus status =
          discovered_status(map, discovery, machine);
      if (interaction.pressed) {
        if (status.tone == DiscoveredTone::OnMap) {
          f.select_device(*status.existing);
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
      // Name and address, centred unless a warning takes a third line.
      const float text_x = card.x + 37.0F;
      const float text_width = std::max(card.width - 42.0F, 0.0F);
      const float text_y = card.y + (status.text.empty() ? 10.0F : 2.0F);
      ctx.label(gui::Rect{text_x, text_y, text_width, 17.0F},
                machine.hostname.empty() ? machine.address : machine.hostname,
                on_map ? ctx.theme().text_muted : ctx.theme().text);
      std::string where = machine.address + "  NODE " +
                          std::to_string(machine.node_index);
      if (machine.this_machine) {
        where += "  THIS PC";
      }
      ctx.label(gui::Rect{text_x, text_y + 16.0F, text_width, 15.0F}, where,
                ctx.theme().text_muted);
      if (!status.text.empty()) {
        ctx.label(gui::Rect{text_x, card.y + 33.0F, text_width, 15.0F},
                  status.text, ctx.theme().warning);
      }
    }
    // Processors the last search found, after the machines.
    for (std::size_t index = 0U; index < discovery.processors.size(); ++index) {
      const topology::HardwareDiscoveredProcessor &found =
          discovery.processors[index];
      gui::Rect card = hardware_discovered_card_bounds(
          bounds, discovery, discovery.machines.size() + index);
      card.x -= discovered_scroll_;
      const gui::Interaction interaction =
          ctx.grab(discovered_processor_id(id_, found), card);
      const topology::HardwareDevice *const placed =
          discovered_processor_device(map, found);
      if (interaction.pressed) {
        if (placed != nullptr) {
          f.select_device(*placed);
        } else {
          drag_kind_ = DragKind::DiscoveredProcessor;
          discovered_processor_drag_ = found;
          discovered_press_x_ = ctx.pointer_x();
          discovered_press_y_ = ctx.pointer_y();
        }
      }
      const bool dragging_this =
          drag_kind_ == DragKind::DiscoveredProcessor &&
          discovered_processor_drag_.address == found.address &&
          discovered_processor_drag_.protocol == found.protocol;
      if (dragging_this) {
        dragged = interaction;
      }
      if (card.x > strip.right() || card.right() < strip.x) {
        continue;
      }
      ctx.rounded(card,
                  dragging_this ? ctx.theme().selection
                                : (interaction.hovered ? ctx.theme().widget_hover
                                                       : ctx.theme().row),
                  ctx.theme().radius);
      ctx.rounded_outline(card,
                          placed != nullptr
                              ? ctx.theme().good
                              : (dragging_this ? ctx.theme().accent
                                               : ctx.theme().divider),
                          ctx.theme().radius);
      topology::HardwareDevice sample{};
      sample.kind = topology::HardwareDeviceKind::LedProcessor;
      sample.protocol = found.protocol;
      draw_sprite_thumbnail(
          ctx, gui::Rect{card.x + 3.0F, card.y + 4.0F, 34.0F, card.height - 8.0F},
          sample, sprites_, placed != nullptr ? 0.55F : 1.0F);
      const float text_x = card.x + 41.0F;
      const float text_width = std::max(card.width - 46.0F, 0.0F);
      ctx.push_clip(card);
      ctx.label(gui::Rect{text_x, card.y + 10.0F, text_width, 17.0F},
                found.name,
                placed != nullptr ? ctx.theme().text_muted : ctx.theme().text);
      ctx.label(gui::Rect{text_x, card.y + 26.0F, text_width, 15.0F},
                found.address + "  " +
                    std::string{topology::processor_protocol_name(found.protocol)},
                ctx.theme().text_muted);
      ctx.pop_clip();
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
  return dragged;
}

void HardwareMappingPanel::drop_discovered(Frame &f,
                                           std::optional<gui::Interaction> dragged) {
  gui::UiContext &ctx = f.ctx;
  const topology::HardwareMap &map = f.map;
  HardwareMappingFrame &frame = f.result;
  const gui::Rect &view = f.view;
  const IsometricProjection &projection = f.projection;
  const topology::HardwareDiscovery &discovery = f.runtime.discovery;
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
  if (drag_kind_ == DragKind::Discovered) {
    // A machine that stopped answering, or a strip that gave way, still has
    // the pointer captured: keep reading that capture so its release lands.
    if (!dragged.has_value()) {
      dragged = ctx.grab(
          gui::ui_id(id_, "hardware.discovered", discovered_key(discovered_drag_)),
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
          f.select_device(*status.existing);
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

  if (drag_kind_ == DragKind::DiscoveredProcessor) {
    // As for a machine: a processor that drops out of the list mid-drag
    // keeps the capture its card took.
    if (!dragged.has_value()) {
      dragged = ctx.grab(
          discovered_processor_id(id_, discovered_processor_drag_),
          gui::Rect{});
    }
    const auto place_processor = [&](core::i32 grid_x, core::i32 grid_y) {
      HardwarePlaceRequest request{topology::HardwareDeviceKind::LedProcessor,
                                   grid_x, grid_y};
      request.machine = HardwarePlaceMachine{discovered_processor_drag_.name,
                                             discovered_processor_drag_.address,
                                             0U};
      request.protocol = discovered_processor_drag_.protocol;
      frame.place = std::move(request);
    };
    if (dragged->released) {
      const float moved = std::hypot(ctx.pointer_x() - discovered_press_x_,
                                     ctx.pointer_y() - discovered_press_y_);
      if (view.contains(ctx.pointer_x(), ctx.pointer_y())) {
        const auto [grid_x, grid_y] =
            unproject_isometric(projection, ctx.pointer_x(), ctx.pointer_y());
        place_processor(grid_x, grid_y);
      } else if (dragged->clicked && moved < link_drag_threshold * 2.0F) {
        const auto [grid_x, grid_y] = free_grid_cell(map);
        place_processor(grid_x, grid_y);
      }
      drag_kind_ = DragKind::None;
    } else if (!dragged->held) {
      drag_kind_ = DragKind::None;
    }
  }
}

} // namespace mediaserver::editor
