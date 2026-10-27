#include "timeline_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <vector>

// The Timeline's foot and its clip-row edges. The navigator is the whole show
// in miniature with the part on screen as a thumb - drag it to scroll, drag
// an end to zoom with the other end held - and the layer list's end of it
// picks how tall every clip row is. A clip row's lower edge in the list sizes
// that row alone.
namespace mediaserver::editor {
using namespace timeline;

namespace {

// How far either side of a thumb end its grip answers.
constexpr float navigator_grip_reach = 5.0F;
// The thumb is never drawn thinner than this, so a view zoomed in on a long
// show can still be found and grabbed.
constexpr float navigator_minimum_thumb = 14.0F;

struct HeightChoice final {
  float height{};
  std::string_view label;
  std::string_view tip;
};

constexpr std::array<HeightChoice, 3U> height_choices{{
    {clip_row_height_small, "S", "Small clip rows"},
    {clip_row_height_medium, "M",
     "Medium clip rows: room for the filmstrip and waveform"},
    {clip_row_height_large, "L",
     "Large clip rows: the filmstrip above the waveform"},
}};

} // namespace

void TimelinePanel::resize_clip_rows(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const std::span<const ElementInfo> elements = p.input.elements;
  const gui::Rect &rows_bounds = p.layout.rows;
  const float list_width = p.layout.list_width;
  const std::vector<Row> &rows = p.built.rows;

  if (row_resize_.clip != doc::NodeId{} && !ctx.pointer_down()) {
    row_resize_ = RowResize{};
  }
  ctx.push_clip(gui::Rect{rows_bounds.x, rows_bounds.y, list_width,
                          rows_bounds.height});
  for (const Row &row : rows) {
    if (row.kind != Row::Kind::Layer || row.surface ||
        row.element_kind != ElementKind::Media) {
      continue;
    }
    const float bottom = rows_bounds.y + row.top - row_scroll_ + row.height;
    if (bottom < rows_bounds.y || bottom > rows_bounds.y + rows_bounds.height) {
      continue;
    }
    const ElementInfo *const element = find_element(elements, row.node);
    if (element == nullptr || !element->timing.placed()) {
      continue;
    }
    // The edge answers just either side of the rule between the rows, so
    // it never takes the row's own name or toggles from it.
    const gui::Interaction edge =
        ctx.grab(mix(id_, "clip row edge", row.node.value()),
                 gui::Rect{rows_bounds.x, bottom - 3.0F, list_width, 6.0F});
    ctx.cursor_for(edge, gui::Cursor::ResizeVertical);
    if (edge.pressed) {
      row_resize_ = RowResize{row.node, ctx.shift_held(),
                              ctx.pointer_press_y(), row.height};
    }
    const bool sizing = edge.held && row_resize_.clip == row.node;
    if (sizing) {
      const float height =
          std::clamp(row_resize_.start_height + ctx.pointer_y() -
                         row_resize_.start_y,
                     row_height, clip_row_height_maximum);
      if (row_resize_.all) {
        rows_state_.clip_row_height = height;
        rows_state_.clip_row_heights.clear();
      } else {
        rows_state_.clip_row_heights[row.node] = height;
      }
      p.frame.editing = true;
      if (edge.released) {
        row_resize_ = RowResize{};
      }
    }
    if (sizing || (edge.hovered && row_resize_.clip == doc::NodeId{})) {
      ctx.panel(gui::Rect{rows_bounds.x, bottom - 1.5F, list_width, 2.0F},
                sizing ? ctx.theme().accent : ctx.theme().text_muted);
    }
    ctx.tooltip(gui::Rect{rows_bounds.x, bottom - 3.0F, list_width, 6.0F},
                "Drag to size this clip row; Shift sizes every clip row");
  }
  ctx.pop_clip();
}

void TimelinePanel::draw_navigator(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const gui::Theme &theme = ctx.theme();
  const gui::Rect &navigator = p.layout.navigator;
  const gui::Rect &bar = p.layout.navigator_graph;
  const gui::Rect &graph = p.layout.graph;
  const float list_width = p.layout.list_width;
  if (navigator.height <= 0.0F) {
    return;
  }

  ctx.panel(navigator, theme.header);
  ctx.panel(gui::Rect{navigator.x, navigator.y, navigator.width, 1.0F},
            theme.divider);

  // The keyboard reference, where the hand finds it without a menu.
  if (list_width >= 40.0F &&
      text_toggle(ctx, mix(id_, "shortcut sheet"),
                  gui::Rect{navigator.x + 6.0F, navigator.y + 2.0F, 20.0F,
                            navigator.height - 3.0F},
                  "?", shortcut_sheet_open_,
                  "Timeline keyboard shortcuts (?)")) {
    shortcut_sheet_open_ = true;
  }

  // The layer list's end: every clip row's height, chosen at once. A row
  // sized alone by its edge rejoins the others when one is chosen.
  constexpr float choice_width = 22.0F;
  const float choices_width =
      choice_width * static_cast<float>(height_choices.size()) + 4.0F;
  const float caption_width = ctx.font_width("CLIP ROWS") + 12.0F;
  if (list_width >= choices_width + caption_width + 8.0F) {
    float x = navigator.x + list_width - choices_width - 4.0F;
    ctx.label(gui::Rect{x - caption_width, navigator.y + 1.0F, caption_width,
                        navigator.height - 1.0F},
              "CLIP ROWS", theme.text_muted);
    for (const HeightChoice &choice : height_choices) {
      const bool engaged =
          rows_state_.clip_row_heights.empty() &&
          std::fabs(rows_state_.clip_row_height - choice.height) < 0.5F;
      if (text_toggle(ctx, mix(id_, "clip row height", choice.label.front()),
                      gui::Rect{x, navigator.y + 2.0F, choice_width - 2.0F,
                                navigator.height - 3.0F},
                      choice.label, engaged, choice.tip)) {
        rows_state_.clip_row_height = choice.height;
        rows_state_.clip_row_heights.clear();
      }
      x += choice_width;
    }
  }
  if (bar.width <= 2.0F * navigator_minimum_thumb) {
    return;
  }

  // What the bar stands for: the show range, everything authored outside
  // it, and the view itself, so the thumb always lies on the bar. A drag
  // holds the span it began with, so the thumb tracks the pointer.
  const double view_left = time_axis_.left_frame;
  const double view_right =
      time_axis_.frame(graph.x + graph.width, graph.x);
  if (navigator_.drag == NavigatorDrag::None) {
    double begin = static_cast<double>(p.first);
    double end = static_cast<double>(p.last) + 1.0;
    for (const ElementInfo &element : p.input.elements) {
      if (element.kind == ElementKind::Media && element.timing.placed()) {
        begin = std::min(begin, static_cast<double>(element.timing.start));
        end = std::max(end, static_cast<double>(element.timing.start) +
                                static_cast<double>(element.timing.duration));
      }
    }
    for (const scene::ChannelKeyframes &channel : p.input.keyframes) {
      if (!channel.frames.empty()) {
        begin = std::min(begin, static_cast<double>(channel.frames.front()));
        end = std::max(end, static_cast<double>(channel.frames.back()) + 1.0);
      }
    }
    for (const scene::TimelineMarker &marker : p.input.markers) {
      end = std::max(end, static_cast<double>(marker.frame) + 1.0);
    }
    navigator_.begin = begin;
    navigator_.end = end;
  }
  const double begin = std::min(navigator_.begin, view_left);
  const double end = std::max(navigator_.end, view_right);
  const gui::Rect track{bar.x + 2.0F, bar.y + 3.0F,
                        std::max(bar.width - 4.0F, 1.0F),
                        std::max(bar.height - 5.0F, 1.0F)};
  const double scale =
      static_cast<double>(track.width) / std::max(end - begin, 1.0);
  const auto to_x = [&](double frame) noexcept {
    return track.x + static_cast<float>((frame - begin) * scale);
  };

  float thumb_left = to_x(view_left);
  float thumb_right = to_x(view_right);
  if (thumb_right - thumb_left < navigator_minimum_thumb) {
    const float centre = (thumb_left + thumb_right) * 0.5F;
    thumb_left = std::clamp(centre - navigator_minimum_thumb * 0.5F, track.x,
                            track.x + track.width - navigator_minimum_thumb);
    thumb_right = thumb_left + navigator_minimum_thumb;
  }

  // Grips first, so an end wins over the body it overlaps; the bare track
  // last, so a press beside the thumb jumps the view there.
  const gui::Interaction left_it = ctx.grab(
      mix(id_, "navigator left"),
      gui::Rect{thumb_left - navigator_grip_reach, bar.y,
                navigator_grip_reach * 2.0F, bar.height});
  const gui::Interaction right_it = ctx.grab(
      mix(id_, "navigator right"),
      gui::Rect{thumb_right - navigator_grip_reach, bar.y,
                navigator_grip_reach * 2.0F, bar.height});
  const gui::Interaction thumb_it = ctx.grab(
      mix(id_, "navigator thumb"),
      gui::Rect{thumb_left, bar.y, thumb_right - thumb_left, bar.height});
  const gui::Interaction track_it =
      ctx.grab(mix(id_, "navigator track"), bar);
  ctx.cursor_for(left_it, gui::Cursor::ResizeHorizontal);
  ctx.cursor_for(right_it, gui::Cursor::ResizeHorizontal);
  ctx.cursor_for(thumb_it, gui::Cursor::Move);

  const double span = view_right - view_left;
  const auto start = [&](NavigatorDrag drag) {
    navigator_.drag = drag;
    navigator_.start_x = ctx.pointer_press_x();
    navigator_.start_left = time_axis_.left_frame;
    navigator_.start_right = time_axis_.left_frame + span;
    navigator_.begin = begin;
    navigator_.end = end;
  };
  if (left_it.pressed) {
    start(NavigatorDrag::Left);
  } else if (right_it.pressed) {
    start(NavigatorDrag::Right);
  } else if (thumb_it.pressed) {
    start(NavigatorDrag::Pan);
  } else if (track_it.pressed) {
    // The view jumps to centre on the frame pressed, then follows the drag.
    const double pressed =
        begin + static_cast<double>(ctx.pointer_press_x() - track.x) / scale;
    time_axis_.left_frame = std::clamp(pressed - span * 0.5, begin,
                                       std::max(begin, end - span));
    start(NavigatorDrag::Pan);
  }
  const bool held = (navigator_.drag == NavigatorDrag::Left && left_it.held) ||
                    (navigator_.drag == NavigatorDrag::Right && right_it.held) ||
                    (navigator_.drag == NavigatorDrag::Pan &&
                     (thumb_it.held || track_it.held));
  if (navigator_.drag != NavigatorDrag::None && !held) {
    navigator_.drag = NavigatorDrag::None;
  } else if (held) {
    const double moved =
        static_cast<double>(ctx.pointer_x() - navigator_.start_x) / scale;
    const double held_begin = navigator_.begin;
    const double held_end = navigator_.end;
    const double narrowest =
        static_cast<double>(graph.width) /
        static_cast<double>(TimelineTimeAxis::maximum_pixels_per_frame);
    const auto zoom_to = [&](double left, double right) {
      time_axis_.pixels_per_frame = std::clamp(
          static_cast<float>(static_cast<double>(graph.width) /
                             std::max(right - left, 1e-6)),
          TimelineTimeAxis::minimum_pixels_per_frame,
          TimelineTimeAxis::maximum_pixels_per_frame);
    };
    switch (navigator_.drag) {
    case NavigatorDrag::Pan: {
      const double width = navigator_.start_right - navigator_.start_left;
      time_axis_.left_frame =
          std::clamp(navigator_.start_left + moved, held_begin,
                     std::max(held_begin, held_end - width));
      break;
    }
    case NavigatorDrag::Left: {
      const double left =
          std::clamp(navigator_.start_left + moved, held_begin,
                     navigator_.start_right - narrowest);
      zoom_to(left, navigator_.start_right);
      // The right end stays where it was, even where the zoom clamped.
      time_axis_.left_frame =
          navigator_.start_right - static_cast<double>(graph.width) /
                                       time_axis_.pixels_per_frame;
      break;
    }
    case NavigatorDrag::Right: {
      const double right =
          std::clamp(navigator_.start_right + moved,
                     navigator_.start_left + narrowest, held_end);
      zoom_to(navigator_.start_left, right);
      time_axis_.left_frame = navigator_.start_left;
      break;
    }
    case NavigatorDrag::None:
      break;
    }
    if (left_it.released || right_it.released || thumb_it.released ||
        track_it.released) {
      navigator_.drag = NavigatorDrag::None;
    }
  }

  // The show in miniature: the In/Out range, the thumb, then every placed
  // clip, the markers and the playhead over it, so the view never hides
  // what it is a view of.
  ctx.push_clip(bar);
  ctx.panel(track, theme.track);
  {
    const float in_x = to_x(static_cast<double>(p.first));
    const float out_x = to_x(static_cast<double>(p.last) + 1.0);
    ctx.panel(gui::Rect{in_x, track.y, std::max(out_x - in_x, 1.0F),
                        track.height},
              alpha(theme.accent, 0.16F));
  }
  const bool thumb_hot = navigator_.drag != NavigatorDrag::None ||
                         thumb_it.hovered || left_it.hovered ||
                         right_it.hovered;
  const gui::Rect thumb{thumb_left, bar.y + 2.0F, thumb_right - thumb_left,
                        bar.height - 3.0F};
  ctx.panel(thumb, thumb_hot ? theme.widget_hover : theme.widget);
  draw_border(ctx, thumb, thumb_hot ? theme.accent : theme.text_muted);
  const auto grip = [&](float x, bool hot) {
    ctx.panel(gui::Rect{x - 1.0F, thumb.y + 3.0F, 2.0F,
                        std::max(thumb.height - 6.0F, 1.0F)},
              hot ? theme.accent : theme.text);
  };
  grip(thumb_left + 4.0F,
       left_it.hovered || navigator_.drag == NavigatorDrag::Left);
  grip(thumb_right - 4.0F,
       right_it.hovered || navigator_.drag == NavigatorDrag::Right);
  for (const ElementInfo &element : p.input.elements) {
    if (element.kind != ElementKind::Media || !element.timing.placed()) {
      continue;
    }
    const float x0 = to_x(static_cast<double>(element.timing.start));
    const float x1 = to_x(static_cast<double>(element.timing.start) +
                          static_cast<double>(element.timing.duration));
    ctx.panel(gui::Rect{x0, track.y + track.height * 0.5F - 1.0F,
                        std::max(x1 - x0, 1.0F), 3.0F},
              alpha(theme.text_muted, 0.7F));
  }
  for (const scene::TimelineMarker &marker : p.input.markers) {
    ctx.panel(gui::Rect{to_x(static_cast<double>(marker.frame)), track.y,
                        1.0F, track.height},
              alpha(timeline_marker_color(marker.color), 0.8F));
  }
  ctx.panel(gui::Rect{to_x(static_cast<double>(p.input.timeline.show_frame)),
                      bar.y + 1.0F, 1.0F, bar.height - 2.0F},
            theme.playhead);
  ctx.pop_clip();
  ctx.tooltip(thumb,
              "Drag to scroll; drag either end to zoom. Click the bar to jump "
              "there");
}

} // namespace mediaserver::editor
