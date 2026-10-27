#pragma once

#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/layout.hpp>

#include <algorithm>

// Internal to the Timeline panel: the fixed bands it divides its bounds into.
// Every part of the panel draws inside one of these rectangles, so they are
// computed once per frame, before anything is drawn or hit-tested.
namespace mediaserver::editor::timeline {

inline constexpr float toolbar_height = 34.0F;
inline constexpr float ruler_height = 22.0F;
// Markers and cues get a band of their own under the ruler, so dragging one
// never competes with scrubbing and a name has room to be read.
inline constexpr float strip_height = 20.0F;
// The cache bar's thickness along the top of the marker strip.
inline constexpr float playback_cache_bar_height = 3.0F;
// The zoom/scroll bar along the panel's foot: the whole show in miniature,
// with the part on screen as a thumb whose ends zoom and whose middle pans.
inline constexpr float navigator_height = 16.0F;
// Every row - layer, track, or property - is a full list row high, so a
// toggle or a key diamond is as easy to hit as the name beside it.
inline constexpr float row_height = list_metrics::row_height;
// The layer list's limits while the operator drags its edge.
inline constexpr float minimum_list_width = 200.0F;
inline constexpr float minimum_graph_width = 160.0F;

struct TimelineLayout final {
  // The layer list's width this frame.
  float list_width{};
  gui::Rect toolbar;
  gui::Rect ruler;
  gui::Rect strip;
  // The scrolling rows, list and lanes together, under the strip.
  gui::Rect rows;
  // Time: the lanes plus the ruler and strip above them.
  gui::Rect graph;
  // The part of the graph that is the marker strip.
  gui::Rect strip_graph;
  // The foot band, full width, and the part of it under the lanes.
  gui::Rect navigator;
  gui::Rect navigator_graph;
};

// `operator_list_width` is the width the operator last dragged the list to,
// or zero to follow the panel's own width.
[[nodiscard]] inline TimelineLayout
layout_timeline(const gui::Rect &bounds, float operator_list_width) noexcept {
  TimelineLayout out{};
  const float preferred_list =
      operator_list_width > 0.0F
          ? operator_list_width
          : std::clamp(bounds.width * 0.26F, 250.0F, 340.0F);
  out.list_width = std::min(
      std::max(preferred_list, std::min(minimum_list_width, bounds.width)),
      std::max(bounds.width - minimum_graph_width, 0.0F));
  out.toolbar = gui::Rect{bounds.x, bounds.y, bounds.width, toolbar_height};
  out.ruler = gui::Rect{bounds.x, bounds.y + toolbar_height, bounds.width,
                        ruler_height};
  out.strip = gui::Rect{bounds.x, out.ruler.y + out.ruler.height, bounds.width,
                        strip_height};
  const float below_strip =
      std::max(bounds.height - toolbar_height - ruler_height - strip_height,
               0.0F);
  const float navigator = std::min(navigator_height, below_strip);
  out.rows = gui::Rect{bounds.x, out.strip.y + out.strip.height, bounds.width,
                       below_strip - navigator};
  out.navigator = gui::Rect{bounds.x, out.rows.y + out.rows.height,
                            bounds.width, navigator};
  out.graph = gui::Rect{bounds.x + out.list_width, out.ruler.y,
                        std::max(bounds.width - out.list_width, 0.0F),
                        out.ruler.height + out.strip.height + out.rows.height};
  out.strip_graph =
      gui::Rect{out.graph.x, out.strip.y, out.graph.width, out.strip.height};
  out.navigator_graph = gui::Rect{out.graph.x, out.navigator.y,
                                  out.graph.width, out.navigator.height};
  return out;
}

} // namespace mediaserver::editor::timeline
