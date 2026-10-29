#include <mediaserver/output/feed_layout.hpp>

#include <mediaserver/core/output_limits.hpp>
#include <mediaserver/output/feed_map.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <utility>

namespace mediaserver::output {
namespace {

// One cabled render output: the window it shows and what it feeds.
struct Feed final {
  core::u32 window{};
  core::u32 width{};
  core::u32 height{};
  // The processor's VIDEO IN number, or zero for a wall fed directly.
  core::u8 input{};
};

// Everything one processor - or one wall cabled straight from a render
// output - receives.
struct Group final {
  const topology::HardwareDevice *device{};
  // The wall a processor's LED OUT drives, when it is cabled.
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
      feed.width = binding->width;
      feed.height = binding->height;
    } else if (link.from.port == topology::first_render_server_video_port) {
      // A card without bindings outputs its single legacy window.
      feed.window = from->node_index;
      feed.width = from->width;
      feed.height = from->height;
    } else {
      plan.notes.push_back(from->name + " output " +
                           std::to_string(link.from.port) +
                           " has no output binding, so it is left out");
      continue;
    }
    Group &group = groups[to->node];
    group.device = to;
    group.feeds.push_back(feed);
  }

  // Which wall each processor drives.
  for (auto &[node, group] : groups) {
    for (const topology::HardwareLink &link : map.links) {
      if (link.from.device != node) {
        continue;
      }
      const topology::HardwareDevice *const target = map.device(link.to.device);
      if (target != nullptr &&
          target->kind == topology::HardwareDeviceKind::LedWall) {
        group.wall = target;
        break;
      }
    }
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
          const scene::CanvasRect in_window =
              clamp_to(crop, feed->width, feed->height);
          if (!(in_window == crop)) {
            plan.notes.push_back(input_label(observed, layer.input) +
                                 " takes more than its " +
                                 std::to_string(feed->width) + "x" +
                                 std::to_string(feed->height) +
                                 " window; the viewport is cut to fit");
          }
          const scene::CanvasRect on_screen = clamp_to(
              scene::CanvasRect{layer.screen.x, layer.screen.y,
                                layer.screen.width, layer.screen.height},
              origin.width, origin.height);
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
        device.protocol != topology::HardwareProcessorProtocol::Generic) {
      plan.notes.push_back(device.name +
                           " has not reported its screens, so its inputs "
                           "are laid side by side");
    }
    core::u32 width{};
    core::u32 height{};
    for (const Feed &feed : group->feeds) {
      width += feed.width;
      height = std::max(height, feed.height);
    }
    const scene::CanvasRect origin = shelf.place(width, height);
    plan.screens.push_back(FeedLayoutScreen{wall_name, origin, false});
    core::u32 x = origin.x;
    for (const Feed &feed : group->feeds) {
      const std::string label =
          feed.input == 0U ? wall_name
                           : wall_name + " - " + input_label(observed, feed.input);
      add_viewport(FeedLayoutViewport{
          feed.window, label, scene::CanvasRect{x, origin.y, feed.width, feed.height},
          scene::CanvasRect{0U, 0U, feed.width, feed.height}});
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
