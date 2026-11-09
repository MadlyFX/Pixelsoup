#include <mediaserver/output/feed_layout.hpp>

#include <mediaserver/core/output_limits.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/topology/hardware_signal.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <utility>

namespace mediaserver::output {
namespace {

// One cable from a render output: the window it shows, the part of that
// window the cable carries (all of it, or one tile of a shared output), and
// what it feeds.
struct Feed final {
  core::u32 window{};
  core::u32 x{};
  core::u32 y{};
  core::u32 width{};
  core::u32 height{};
  // The processor's VIDEO IN number, or zero for a wall fed directly.
  core::u8 input{};
};

// Everything one processor - or one wall cabled straight from a render
// output - receives.
struct Group final {
  const topology::HardwareDevice *device{};
  // The walls a processor's LED OUT drives, in map order; the first stands
  // for the group.
  std::vector<const topology::HardwareDevice *> walls;
  const topology::HardwareDevice *wall{};
  std::vector<Feed> feeds;
};

// Left to right as the map draws them: an isometric cell's screen x is its
// grid x minus its grid y.
[[nodiscard]] std::pair<core::i64, core::i64>
map_order(const topology::HardwareDevice &device) noexcept {
  return {static_cast<core::i64>(device.grid_x) - device.grid_y,
          static_cast<core::i64>(device.grid_x) + device.grid_y};
}

// Places rectangles left to right in rows no wider than the largest canvas.
class Shelf final {
public:
  [[nodiscard]] scene::CanvasRect place(core::u32 width, core::u32 height) {
    width = std::min(width, core::max_output_dimension);
    height = std::min(height, core::max_output_dimension);
    if (x_ != 0U && x_ + width > core::max_output_dimension) {
      x_ = 0U;
      y_ += row_;
      row_ = 0U;
    }
    const scene::CanvasRect placed{x_, y_, width, height};
    x_ += width;
    row_ = std::max(row_, height);
    right_ = std::max(right_, x_);
    bottom_ = std::max(bottom_, y_ + height);
    return placed;
  }

  [[nodiscard]] scene::Canvas extent() const noexcept {
    return scene::Canvas{right_, std::min(bottom_, core::max_output_dimension)};
  }

private:
  core::u32 x_{};
  core::u32 y_{};
  core::u32 row_{};
  core::u32 right_{};
  core::u32 bottom_{};
};

[[nodiscard]] scene::CanvasRect
clamp_to(scene::CanvasRect rect, core::u32 width, core::u32 height) noexcept {
  rect.x = std::min(rect.x, width);
  rect.y = std::min(rect.y, height);
  rect.width = std::min(rect.width, width - rect.x);
  rect.height = std::min(rect.height, height - rect.y);
  return rect;
}

[[nodiscard]] std::string input_label(const topology::HardwareProcessorNode *node,
                                      core::u8 input) {
  if (node != nullptr) {
    if (const topology::HardwareProcessorInput *const shown =
            node->input(input)) {
      if (!shown->name.empty()) {
        return shown->name;
      }
    }
  }
  return "input " + std::to_string(input);
}

} // namespace

FeedLayoutPlan plan_feed_layout(const topology::HardwareMap &map,
                                const topology::HardwareRuntimeSnapshot &runtime) {
  FeedLayoutPlan plan{};
  std::map<doc::NodeId, Group> groups;

  for (const topology::HardwareLink &link : map.links) {
    const topology::HardwareDevice *const from = map.device(link.from.device);
    const topology::HardwareDevice *const to = map.device(link.to.device);
    if (from == nullptr || to == nullptr ||
        from->kind != topology::HardwareDeviceKind::RenderServer) {
      continue;
    }
    const topology::HardwarePort *const port =
        topology::find_port(from->kind, link.from.port);
    if (port == nullptr || port->signal != topology::HardwareSignal::Video) {
      continue;
    }
    Feed feed{};
    if (to->kind == topology::HardwareDeviceKind::LedProcessor) {
      feed.input = topology::processor_input_number(link.to.port);
      if (feed.input == 0U) {
        continue;
      }
    } else if (to->kind != topology::HardwareDeviceKind::LedWall) {
      continue;
    }
    if (const topology::HardwareOutputBinding *const binding =
            map.output_binding_for(from->node, link.from.port)) {
      feed.window = binding->feed_index;
    } else if (link.from.port == topology::first_render_server_video_port) {
      // A card without bindings outputs its single legacy window.
      feed.window = from->node_index;
    } else {
      plan.notes.push_back(from->name + " output " +
                           std::to_string(link.from.port) +
                           " has no output binding, so it is left out");
      continue;
    }
    const topology::HardwareOutputRegion carried =
        topology::link_output_region(map, link, runtime).region;
    feed.x = carried.x;
    feed.y = carried.y;
    feed.width = carried.width;
    feed.height = carried.height;
    Group &group = groups[to->node];
    group.device = to;
    group.feeds.push_back(feed);
  }

  // Which walls each processor drives.
  for (auto &[node, group] : groups) {
    for (const topology::HardwareLink &link : map.links) {
      if (link.from.device != node) {
        continue;
      }
      const topology::HardwareDevice *const target = map.device(link.to.device);
      if (target != nullptr &&
          target->kind == topology::HardwareDeviceKind::LedWall) {
        group.walls.push_back(target);
      }
    }
    std::stable_sort(group.walls.begin(), group.walls.end(),
                     [](const topology::HardwareDevice *left,
                        const topology::HardwareDevice *right) {
                       return map_order(*left) < map_order(*right);
                     });
    group.wall = group.walls.empty() ? nullptr : group.walls.front();
    std::stable_sort(group.feeds.begin(), group.feeds.end(),
                     [](const Feed &left, const Feed &right) {
                       return left.input < right.input;
                     });
  }

  std::vector<const Group *> ordered;
  for (const auto &[node, group] : groups) {
    ordered.push_back(&group);
  }
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const Group *left, const Group *right) {
                     const auto &l = left->wall != nullptr ? *left->wall
                                                           : *left->device;
                     const auto &r = right->wall != nullptr ? *right->wall
                                                            : *right->device;
                     return map_order(l) < map_order(r);
                   });

  Shelf shelf;
  const auto add_viewport = [&](FeedLayoutViewport viewport) {
    if (plan.viewports.size() >= max_output_maps ||
        viewport.source.width == 0U || viewport.source.height == 0U ||
        viewport.viewport.width == 0U || viewport.viewport.height == 0U) {
      return;
    }
    plan.viewports.push_back(std::move(viewport));
  };

  for (const Group *group : ordered) {
    const topology::HardwareDevice &device = *group->device;
    const std::string wall_name =
        group->wall != nullptr ? group->wall->name : device.name;
    const topology::HardwareProcessorNode *const observed =
        device.kind == topology::HardwareDeviceKind::LedProcessor
            ? runtime.processor(device.node)
            : nullptr;
    const bool reported =
        observed != nullptr && !observed->screens.empty() &&
        (observed->reachability ==
             topology::HardwareRuntimeReachability::Online ||
         observed->stale);
    const auto feed_for = [&](core::u8 input) -> const Feed * {
      for (const Feed &feed : group->feeds) {
        if (feed.input == input) {
          return &feed;
        }
      }
      return nullptr;
    };

    if (reported) {
      std::vector<bool> shown(group->feeds.size(), false);
      for (const topology::HardwareProcessorScreen &screen : observed->screens) {
        if (screen.width == 0U || screen.height == 0U) {
          continue;
        }
        // One screen is its wall; several keep the processor's names for
        // them, which the wall cards it proposed were given.
        const std::string screen_name =
            observed->screens.size() == 1U || screen.name.empty()
                ? wall_name
                : screen.name;
        const scene::CanvasRect origin = shelf.place(screen.width, screen.height);
        plan.screens.push_back(FeedLayoutScreen{screen_name, origin, true});
        for (const topology::HardwareProcessorLayer &layer : screen.layers) {
          const Feed *const feed = feed_for(layer.input);
          if (feed == nullptr) {
            if (layer.input != 0U) {
              plan.notes.push_back(screen_name + " shows " +
                                   input_label(observed, layer.input) +
                                   ", which no render output feeds");
            }
            continue;
          }
          shown[static_cast<std::size_t>(feed - group->feeds.data())] = true;
          const scene::CanvasRect crop{layer.source.x, layer.source.y,
                                       layer.source.width, layer.source.height};
          scene::CanvasRect in_tile =
              clamp_to(crop, feed->width, feed->height);
          if (!(in_tile == crop)) {
            plan.notes.push_back(input_label(observed, layer.input) +
                                 " takes more than its " +
                                 std::to_string(feed->width) + "x" +
                                 std::to_string(feed->height) +
                                 " signal; the viewport is cut to fit");
          }
          const scene::CanvasRect placed{layer.screen.x, layer.screen.y,
                                         layer.screen.width,
                                         layer.screen.height};
          const scene::CanvasRect on_screen =
              clamp_to(placed, origin.width, origin.height);
          // A layer reaching past its screen shows only the part on the
          // panels - a 3840x2160 input on a 2880x1536 wall lights its top
          // left 2880x1536 - so the window viewport is that part of the
          // source, scaled as the layer scales it.
          if (!(on_screen == placed) && on_screen.width != 0U &&
              on_screen.height != 0U) {
            const auto part = [](core::u32 source, core::u32 offset,
                                 core::u32 shown, core::u32 whole) {
              const auto scaled = [&](core::u32 value) {
                return static_cast<core::u32>(
                    (static_cast<core::u64>(value) * source + whole / 2U) /
                    whole);
              };
              return std::pair{scaled(offset), scaled(shown)};
            };
            const auto [dx, width] =
                part(in_tile.width, on_screen.x - placed.x, on_screen.width,
                     placed.width);
            const auto [dy, height] =
                part(in_tile.height, on_screen.y - placed.y, on_screen.height,
                     placed.height);
            in_tile = scene::CanvasRect{in_tile.x + dx, in_tile.y + dy, width,
                                        height};
          }
          const scene::CanvasRect in_window{feed->x + in_tile.x,
                                            feed->y + in_tile.y, in_tile.width,
                                            in_tile.height};
          const scene::CanvasRect target{origin.x + on_screen.x,
                                         origin.y + on_screen.y,
                                         on_screen.width, on_screen.height};
          add_viewport(FeedLayoutViewport{
              feed->window,
              screen_name + " - " + input_label(observed, layer.input),
              target, in_window});
        }
      }
      for (std::size_t index = 0U; index < group->feeds.size(); ++index) {
        if (!shown[index]) {
          plan.notes.push_back(
              wall_name + ": " + input_label(observed, group->feeds[index].input) +
              " is cabled but not on any screen");
        }
      }
      continue;
    }

    // Nothing reported: the windows side by side, each shown whole.
    if (device.kind == topology::HardwareDeviceKind::LedProcessor &&
        device.protocol == topology::HardwareProcessorProtocol::BromptonTessera) {
      plan.notes.push_back(device.name +
                           " reports no screen layout over IP control, so its "
                           "inputs are laid side by side");
    } else if (device.kind == topology::HardwareDeviceKind::LedProcessor &&
               device.protocol != topology::HardwareProcessorProtocol::Generic) {
      plan.notes.push_back(device.name +
                           " has not reported its screens, so its inputs "
                           "are laid side by side");
    }
    // A processor with one input and one wall smaller than its signal shows
    // the top left of it, pixel for pixel, as processors do unless told
    // otherwise. The wall card's size says how much.
    const topology::HardwareDevice *const sized = group->wall;
    std::vector<Feed> feeds = group->feeds;
    if (sized != nullptr && group->walls.size() <= 1U && feeds.size() == 1U &&
        sized->width != 0U && sized->height != 0U &&
        (sized->width < feeds.front().width ||
         sized->height < feeds.front().height)) {
      Feed &feed = feeds.front();
      feed.width = std::min(feed.width, sized->width);
      feed.height = std::min(feed.height, sized->height);
      plan.notes.push_back(sized->name + " takes the top left " +
                           std::to_string(feed.width) + "x" +
                           std::to_string(feed.height) +
                           " of its signal, the size on its card");
    }
    core::u32 width{};
    core::u32 height{};
    for (const Feed &feed : feeds) {
      width += feed.width;
      height = std::max(height, feed.height);
    }
    const scene::CanvasRect origin = shelf.place(width, height);
    plan.screens.push_back(FeedLayoutScreen{wall_name, origin, false});
    core::u32 x = origin.x;
    for (const Feed &feed : feeds) {
      const std::string label =
          feed.input == 0U ? wall_name
                           : wall_name + " - " + input_label(observed, feed.input);
      add_viewport(FeedLayoutViewport{
          feed.window, label, scene::CanvasRect{x, origin.y, feed.width, feed.height},
          scene::CanvasRect{feed.x, feed.y, feed.width, feed.height}});
      x += feed.width;
    }
  }

  plan.canvas = shelf.extent();
  for (FeedLayoutViewport &viewport : plan.viewports) {
    viewport.source =
        clamp_to(viewport.source, plan.canvas.width, plan.canvas.height);
  }
  std::erase_if(plan.viewports, [](const FeedLayoutViewport &viewport) {
    return viewport.source.width == 0U || viewport.source.height == 0U;
  });
  for (const FeedLayoutViewport &viewport : plan.viewports) {
    if (std::find(plan.windows.begin(), plan.windows.end(), viewport.window) ==
        plan.windows.end()) {
      plan.windows.push_back(viewport.window);
    }
  }
  std::sort(plan.windows.begin(), plan.windows.end());
  return plan;
}

} // namespace mediaserver::output
