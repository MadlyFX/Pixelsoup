#pragma once

#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/rect_cut.hpp>

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
// Kept plateset moments, while shown, get a thinner band under the markers,
// so each can say what it is beside its diamond without crowding a flag.
inline constexpr float moments_height = 18.0F;
// The cache bar's thickness along the top of the marker strip.
inline constexpr float playback_cache_bar_height = 3.0F;
// The zoom/scroll bar along the panel's foot: the whole show in miniature,
// with the part on screen as a thumb whose ends zoom and whose middle pans.
inline constexpr float navigator_height = 16.0F;
// Every row - layer, track, or property - is one height, so a toggle or a
// key diamond is as easy to hit as the name beside it. Denser than the other
// lists, since a show's layers and their keys must fit on one screen; clip
// rows have their own S/M/L sizes for a larger target.
inline constexpr float row_height = 24.0F;
// The layer list's limits while the operator drags its edge.
inline constexpr float minimum_list_width = 200.0F;
inline constexpr float minimum_graph_width = 160.0F;

struct TimelineLayout final {
  // The layer list's width this frame.
  float list_width{};
  gui::Rect toolbar;
  gui::Rect ruler;
  gui::Rect strip;
  // The moments band under it; no height while moments are hidden.
  gui::Rect moments;
  // The scrolling rows, list and lanes together, under the strip.
  gui::Rect rows;
  // Time: the lanes plus the ruler and strip above them.
  gui::Rect graph;
  // The part of the graph that is the marker strip.
  gui::Rect strip_graph;
  gui::Rect moments_graph;
  // The foot band, full width, and the part of it under the lanes.
  gui::Rect navigator;
  gui::Rect navigator_graph;
};

// `operator_list_width` is the width the operator last dragged the list to,
// or zero to follow the panel's own width.
[[nodiscard]] inline TimelineLayout
layout_timeline(const gui::Rect &bounds, float operator_list_width,
                bool moments_shown = false) noexcept {
  TimelineLayout out{};
  const float preferred_list =
      operator_list_width > 0.0F
          ? operator_list_width
          : std::clamp(bounds.width * 0.26F, 250.0F, 340.0F);
  out.list_width = std::min(
      std::max(preferred_list, std::min(minimum_list_width, bounds.width)),
      std::max(bounds.width - minimum_graph_width, 0.0F));
  gui::Rect rest = bounds;
  out.toolbar = gui::cut_top(rest, toolbar_height);
  out.ruler = gui::cut_top(rest, ruler_height);
  out.strip = gui::cut_top(rest, strip_height);
  out.moments = gui::cut_top(rest, moments_shown ? moments_height : 0.0F);
  out.navigator = gui::cut_bottom(rest, navigator_height);
  out.rows = rest;
  // Time runs right of the list, from the ruler down to the rows' foot; the
  // strip and the navigator each have their part of it.
  gui::Rect time = gui::span_y(bounds, out.ruler.y, out.rows.bottom());
  static_cast<void>(gui::cut_left(time, out.list_width));
  out.graph = time;
  out.strip_graph = gui::cell(out.graph, out.strip);
  out.moments_graph = gui::cell(out.graph, out.moments);
  out.navigator_graph = gui::cell(out.graph, out.navigator);
  return out;
}

} // namespace mediaserver::editor::timeline
