#include "timeline_internal.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

// The Timeline's ruler and marker strip. The ruler heads the layer list with
// search, Auto Key and a new track, reads in timecode or frames, carries the
// In/Out range bar and scrubs; the strip under it holds markers and cues,
// which select, drag, rename and convert in place.
namespace mediaserver::editor {
using namespace timeline;

void TimelinePanel::draw_ruler(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const gui::Rect &bounds = p.bounds;
  const EditorTimeline &timeline = p.input.timeline;
  TimelinePanelFrame &frame = p.frame;
  const float &list_width = p.layout.list_width;
  const gui::Rect &ruler = p.layout.ruler;
  const gui::Rect &strip = p.layout.strip;
  const gui::Rect &rows_bounds = p.layout.rows;
  const gui::Rect &graph = p.layout.graph;
  const auto frame_to_x = [&](double value) noexcept {
    return time_axis_.x(value, graph.x);
  };
  const auto x_to_frame = [&](float x) noexcept {
    return time_axis_.frame(x, graph.x);
  };
  const gui::Theme &theme = ctx.theme();
  auto &first = p.first;
  auto &last = p.last;
  auto &fps = p.fps;
  auto &shown_playhead = p.shown_playhead;

  // ---------------------------------------------------------------------
  // The ruler. Its left end heads the layer list: search, and a new track.
  // Over the graph it reads in timecode, carries the In/Out range as a bar
  // with a handle at each end, and scrubs everywhere else.
  p.search_id = mix(id_, "search");
  auto &search_id = p.search_id;
  ctx.panel(ruler, theme.header);
  ctx.panel(gui::Rect{ruler.x, ruler.y, list_width, ruler.height},
            theme.row_alt);
  const float auto_key_width = list_width >= 160.0F ? 72.0F : 0.0F;
  const gui::Rect search_bounds{ruler.x + 5.0F, ruler.y + 2.0F,
                                std::max(list_width - 37.0F - auto_key_width, 0.0F),
                                std::max(ruler.height - 4.0F, 0.0F)};
  static_cast<void>(ctx.text_field(search_id, search_bounds, search_));
  if (search_.empty() && ctx.focus() != search_id) {
    ctx.label(gui::Rect{search_bounds.x + 19.0F, search_bounds.y,
                        std::max(search_bounds.width - 23.0F, 0.0F),
                        search_bounds.height},
              "Search Timeline...", theme.text_muted);
  }
  const float search_cy = search_bounds.y + search_bounds.height * 0.46F;
  draw_circle(ctx, search_bounds.x + 9.0F, search_cy, 3.0F, theme.text_muted);
  draw_circle(ctx, search_bounds.x + 9.0F, search_cy, 1.7F, theme.widget);
  ctx.draw_list().add_line(search_bounds.x + 11.0F, search_cy + 2.0F,
                           search_bounds.x + 14.0F, search_cy + 5.0F, 1.0F,
                           theme.text_muted);
  if (auto_key_width > 0.0F &&
      text_toggle(ctx, mix(id_, "auto key"),
                  gui::Rect{ruler.x + list_width - 32.0F - auto_key_width,
                            ruler.y + 2.0F, auto_key_width - 3.0F,
                            std::max(ruler.height - 4.0F, 0.0F)},
                  "Auto Key", auto_key_,
                  "Auto Key (Shift+K): edits to animated properties create keys. "
                  "Off edits base values; existing animation still plays. "
                  "Use a property diamond to add or remove a key explicitly.")) {
    auto_key_ = !auto_key_;
  }
  if (list_width >= 60.0F &&
      icon_button(ctx, mix(id_, "create lane"),
                  gui::Rect{ruler.x + list_width - 29.0F, ruler.y + 2.0F,
                            icon_button_width,
                            std::max(ruler.height - 4.0F, 0.0F)},
                  gui::Icon::Plus, false, "New track", theme.accent)) {
    frame.lane_edits.push_back(
        TimelineLaneEdit{TimelineLaneAction::Create, {}, {}});
  }
  // The edge between the layer list and the lanes is a seam the operator
  // drags: long names want a wide list, long shows want wide lanes. A
  // double-click widens the list until every name on screen reads whole.
  {
    // Over the rows only: on the ruler and the marker strip the same edge is
    // where the In handle and a cue at frame zero are grabbed.
    const gui::Rect seam{graph.x - 5.0F, rows_bounds.y, 8.0F,
                         rows_bounds.height};
    const gui::Interaction drag = ctx.grab(mix(id_, "list seam"), seam);
    ctx.cursor_for(drag, gui::Cursor::ResizeHorizontal);
    const float widest =
        std::max(bounds.width - minimum_graph_width, minimum_list_width);
    if (drag.held) {
      list_width_ = std::clamp(ctx.pointer_x() - bounds.x, minimum_list_width,
                               widest);
    }
    if (drag.clicked &&
        clicks_.double_click(mix(id_, "list seam"), ctx.time_seconds())) {
      list_width_ = std::clamp(std::max(list_fit_width_, list_width),
                               minimum_list_width, widest);
    }
    const bool lit = drag.held || drag.hovered;
    ctx.panel(gui::Rect{graph.x - (lit ? 1.0F : 0.0F), ruler.y,
                        lit ? 2.0F : 1.0F,
                        ruler.height + strip.height + rows_bounds.height},
              drag.held ? theme.accent
                        : (drag.hovered ? theme.text_muted : theme.border));
    // A grip at the top of the seam says it moves.
    for (int dot = 0; dot < 3; ++dot) {
      ctx.panel(gui::Rect{graph.x - 1.0F,
                          rows_bounds.y + 6.0F + 4.0F * static_cast<float>(dot),
                          2.0F, 2.0F},
                lit ? theme.text : theme.text_muted);
    }
    ctx.tooltip(seam, "Drag to widen the track list. Double-click to fit "
                      "the names.");
  }

  // Edit points: where a cut, a range end, or a marker sits. Snapping and
  // Up/Down navigation share the one list.
  p.snap_tolerance = static_cast<core::i64>(std::max(
      1.0, std::ceil(8.0 / static_cast<double>(time_axis_.pixels_per_frame))));
  auto &snap_tolerance = p.snap_tolerance;
  // Where a razor pressed at `px` cuts: the frame under it, pulled onto the
  // playhead, a marker, or another cut when snapping.

  ctx.push_clip(gui::Rect{graph.x, ruler.y, graph.width, ruler.height});
  {
    // Labels drop the hours while the whole view is inside the first one,
    // which leaves room for a label every second at an ordinary zoom.
    const double view_right = x_to_frame(graph.x + graph.width);
    const bool first_hour =
        time_axis_.left_frame > -static_cast<double>(fps) * 3'600.0 &&
        view_right < static_cast<double>(fps) * 3'600.0;
    const auto tick_label = [&](core::i64 frame) {
      const std::string text = compact_timecode(
          timecode(p, static_cast<core::u64>(frame < 0 ? -frame : frame)),
          first_hour);
      return frame < 0 ? "-" + text : text;
    };
    const float minimum_spacing = std::max(
        56.0F,
        ctx.font_width(tick_label(static_cast<core::i64>(std::max(
            view_right, static_cast<double>(last))))) + 14.0F);
    const core::i64 step =
        ruler_.frames ? ruler_step(time_axis_.pixels_per_frame)
                      : timecode_step(time_axis_.pixels_per_frame, fps, minimum_spacing);
    // Minor ticks divide each labelled step into whole frames, with the
    // half-way tick taller where the division is even, as a tape measure
    // marks its halves.
    const core::i64 minor =
        ruler_subdivision(step, time_axis_.pixels_per_frame, 7.0F);
    const core::i64 visible_begin =
        static_cast<core::i64>(std::floor(time_axis_.left_frame));
    core::i64 tick = (visible_begin / step) * step;
    if (tick > visible_begin) {
      tick -= step;
    }
    for (std::size_t count = 0U; count < 512U; ++count, tick += step) {
      const float tick_x = frame_to_x(static_cast<double>(tick));
      if (tick_x > graph.x + graph.width) {
        break;
      }
      if (minor > 0) {
        const bool halves = (step / minor) % 2 == 0;
        for (core::i64 sub = tick + minor; sub < tick + step; sub += minor) {
          const float sub_x = frame_to_x(static_cast<double>(sub));
          if (sub_x >= graph.x) {
            const bool half = halves && (sub - tick) * 2 == step;
            const float tall = half ? 5.0F : 3.0F;
            ctx.draw_list().add_rect(sub_x, ruler.y + ruler.height - tall,
                                     1.0F, tall,
                                     alpha(theme.text_muted, half ? 0.8F : 0.5F));
          }
        }
      }
      if (tick_x < graph.x) {
        continue;
      }
      ctx.draw_list().add_rect(tick_x, ruler.y + ruler.height - 8.0F, 1.0F,
                               8.0F, theme.text_muted);
      ctx.label(gui::Rect{tick_x + 4.0F, ruler.y, minimum_spacing,
                          ruler.height - 5.0F},
                tick_label(tick), theme.text_muted);
    }
  }
  ctx.pop_clip();

  // The range bar and its handles. A handle drag previews locally and
  // reports once, on release, like every other drag here.
  p.shown_first = first;
  auto &shown_first = p.shown_first;
  p.shown_last = last;
  auto &shown_last = p.shown_last;
  {
    // Each handle sits on the frame it names, where the playhead stands on
    // that frame, so a fitted view shows both.
    const float in_x = frame_to_x(static_cast<double>(first));
    const float out_x = frame_to_x(static_cast<double>(last));
    // Each handle answers down the ruler's full height, not only on the
    // thin range bar it draws.
    const gui::Rect in_grip{in_x - range_handle_reach, ruler.y,
                            range_handle_reach * 2.0F, ruler.height};
    const gui::Rect out_grip{out_x - range_handle_reach, ruler.y,
                             range_handle_reach * 2.0F, ruler.height};
    ctx.push_clip(gui::Rect{graph.x, ruler.y, graph.width, ruler.height});
    const gui::Interaction in_it = ctx.grab(mix(id_, "range in"), in_grip);
    const gui::Interaction out_it = ctx.grab(mix(id_, "range out"), out_grip);
    ctx.pop_clip();
    if (in_it.pressed) {
      ruler_.range_drag = RangeHandle::In;
    } else if (out_it.pressed) {
      ruler_.range_drag = RangeHandle::Out;
    }
    const gui::Interaction &held = ruler_.range_drag == RangeHandle::In ? in_it : out_it;
    if (ruler_.range_drag != RangeHandle::None && held.held) {
      core::i64 wanted =
          static_cast<core::i64>(std::llround(x_to_frame(ctx.pointer_x())));
      if (snapping_ && !ctx.alt_held()) {
        std::vector<core::i64> targets = edit_points(p, doc::NodeId{}, true);
        targets.push_back(shown_playhead);
        if (ruler_.range_drag == RangeHandle::Out) {
          // A clip's exclusive end is the frame after its last; Out lands on
          // the last frame itself.
          for (core::i64 &target : targets) {
            target -= 1;
          }
          targets.push_back(shown_playhead);
        }
        if (const auto snapped = nearest_snap(wanted, targets, snap_tolerance)) {
          wanted = *snapped;
        }
      }
      wanted = std::max<core::i64>(wanted, 0);
      ruler_.range_drag_value = static_cast<core::u64>(wanted);
      if (ruler_.range_drag == RangeHandle::In) {
        shown_first = std::min(ruler_.range_drag_value, last);
      } else {
        shown_last = std::max(ruler_.range_drag_value, first);
      }
      frame.editing = true;
      if (held.released) {
        if (shown_first != first || shown_last != last) {
          frame.range = RangeRequest{shown_first, shown_last, timeline.loop};
        }
        ruler_.range_drag = RangeHandle::None;
      }
    } else if (ruler_.range_drag != RangeHandle::None && !held.held) {
      ruler_.range_drag = RangeHandle::None;
    }
    ctx.push_clip(gui::Rect{graph.x, ruler.y, graph.width, ruler.height});
    const float bar_left = frame_to_x(static_cast<double>(shown_first));
    const float bar_right = frame_to_x(static_cast<double>(shown_last));
    ctx.panel(gui::Rect{bar_left, ruler.y + ruler.height - 3.0F,
                        std::max(bar_right - bar_left, 1.0F), 3.0F},
              alpha(theme.accent, timeline.loop ? 0.85F : 0.5F));
    const auto handle = [&](float handle_x, bool in, bool hot) {
      const gui::UiColor color = hot ? theme.text : theme.accent;
      ctx.panel(gui::Rect{handle_x - (in ? 0.0F : 2.0F),
                          ruler.y + ruler.height - 9.0F, 2.0F, 9.0F},
                color);
      ctx.panel(gui::Rect{in ? handle_x : handle_x - 5.0F,
                          ruler.y + ruler.height - 9.0F, 5.0F, 2.0F},
                color);
    };
    handle(bar_left, true, in_it.hovered || ruler_.range_drag == RangeHandle::In);
    handle(bar_right, false, out_it.hovered || ruler_.range_drag == RangeHandle::Out);
    ctx.pop_clip();
  }

  // Scrubbing the ruler pauses/seeks through the shell. It follows the
  // pointer while held, including the release frame.
  const gui::Interaction ruler_it =
      ctx.grab(mix(id_, "ruler"),
               gui::Rect{graph.x, ruler.y, graph.width, ruler.height});
  if (ruler_it.held) {
    frame.seek_to_frame =
        rounded_frame(x_to_frame(ctx.pointer_x()), first, last);
  }

}

std::vector<core::i64>
TimelinePanel::edit_points(Pass &p,
                           doc::NodeId exclude_clip,
                           bool include_markers) {
  const std::span<const ElementInfo> elements = p.input.elements;
  const std::span<const scene::TimelineMarker> markers = p.input.markers;
  auto &first_i = p.first_i;
  auto &last_i = p.last_i;

  std::vector<core::i64> points{first_i, last_i + 1};
  for (const ElementInfo &element : elements) {
    if (element.node == exclude_clip || element.kind != ElementKind::Media ||
        !element.timing.placed()) {
      continue;
    }
    points.push_back(element.timing.start);
    points.push_back(element.timing.start + element.timing.duration);
  }
  if (include_markers) {
    for (const scene::TimelineMarker &marker : markers) {
      if (std::find(markers_.selected.begin(), markers_.selected.end(),
                    marker.id) == markers_.selected.end() ||
          markers_.drag == doc::NodeId{}) {
        points.push_back(marker.frame);
      }
    }
  }
  return points;
}

core::i64 TimelinePanel::razor_frame(Pass &p, float px) {
  gui::UiContext &ctx = p.ctx;
  const gui::Rect &graph = p.layout.graph;
  const auto x_to_frame = [&](float x) noexcept {
    return time_axis_.frame(x, graph.x);
  };
  auto &shown_playhead = p.shown_playhead;
  auto &snap_tolerance = p.snap_tolerance;

  core::i64 cut = static_cast<core::i64>(std::llround(x_to_frame(px)));
  if (snapping_ && !ctx.alt_held()) {
    std::vector<core::i64> targets = edit_points(p, doc::NodeId{}, true);
    targets.push_back(shown_playhead);
    if (const auto snapped = nearest_snap(cut, targets, snap_tolerance)) {
      cut = *snapped;
    }
  }
  return cut;
}

void TimelinePanel::draw_marker_strip(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const EditorTimeline &timeline = p.input.timeline;
  const std::span<const scene::TimelineMarker> markers = p.input.markers;
  TimelinePanelFrame &frame = p.frame;
  FrameScratch &scratch = p.scratch;
  const float &list_width = p.layout.list_width;
  const gui::Rect &strip = p.layout.strip;
  const gui::Rect &graph = p.layout.graph;
  const gui::Rect &strip_graph = p.layout.strip_graph;
  const auto frame_to_x = [&](double value) noexcept {
    return time_axis_.x(value, graph.x);
  };
  const auto x_to_frame = [&](float x) noexcept {
    return time_axis_.frame(x, graph.x);
  };
  const gui::Theme &theme = ctx.theme();
  auto &first = p.first;
  auto &last = p.last;
  auto &cue_numbers = p.cue_numbers;
  auto &any_cue = p.any_cue;
  auto &shown_playhead = p.shown_playhead;
  auto &snap_tolerance = p.snap_tolerance;

  // ---------------------------------------------------------------------
  // The marker strip. Its list end reports where the show stands against
  // its cues; over the graph each marker is a flag and each cue a numbered
  // tag, Loop cues bracketing the section they repeat.
  ctx.panel(gui::Rect{strip.x, strip.y, list_width, strip.height},
            theme.row_alt);
  ctx.panel(strip_graph, theme.background);
  ctx.panel(gui::Rect{strip.x, strip.y + strip.height - 1.0F, strip.width,
                      1.0F},
            theme.divider);
  if (any_cue) {
    std::string status;
    gui::UiColor status_color = theme.text_muted;
    if (!timeline.cues_armed) {
      status = "CUES OFF";
    } else if (timeline.held_cue != doc::NodeId{}) {
      status = "HOLD  " + cue_label(p, timeline.held_cue);
      status_color = theme.playhead;
    } else if (timeline.looping_cue != doc::NodeId{}) {
      status = "LOOP  " + cue_label(p, timeline.looping_cue);
      if (timeline.frames_to_loop_restart.has_value()) {
        status += "  again in " + seconds_label(p, *timeline.frames_to_loop_restart);
      }
      status_color = theme.accent;
    } else if (timeline.next_cue != doc::NodeId{}) {
      status = "NEXT  " + cue_label(p, timeline.next_cue);
      if (timeline.playing && timeline.frames_to_next_cue.has_value()) {
        status += "  " + seconds_label(p, *timeline.frames_to_next_cue);
      }
      status_color = theme.text;
    }
    ctx.label(gui::Rect{strip.x + 8.0F, strip.y,
                        std::max(list_width - 12.0F, 0.0F), strip.height},
              status, status_color);
  }

  // Where each marker draws this frame: a drag previews from the panel.
  struct MarkerHit final {
    std::size_t index{};
    gui::Rect bounds{};
  };
  std::vector<MarkerHit> marker_hits;
  marker_hits.reserve(markers.size());
  for (std::size_t index = 0U; index < markers.size(); ++index) {
    const scene::TimelineMarker &marker = markers[index];
    const float marker_x =
        frame_to_x(static_cast<double>(shown_marker_frame(marker)));
    std::string label = marker.is_cue() ? cue_tag(cue_numbers[index]) : "";
    if (!marker.name.empty()) {
      label += label.empty() ? marker.name : " " + marker.name;
    }
    const float tag_width =
        std::clamp(ctx.font_width(label) + 14.0F, 14.0F, 180.0F);
    marker_hits.push_back(MarkerHit{
        index, gui::Rect{marker_x - marker_hit_reach, strip.y,
                         tag_width + marker_hit_reach, strip.height - 1.0F}});
  }

  // Later markers draw over earlier ones, so they are offered the press
  // first.
  std::optional<std::size_t> pressed_marker;
  ctx.push_clip(strip_graph);
  for (auto hit = marker_hits.rbegin(); hit != marker_hits.rend(); ++hit) {
    const gui::Interaction interaction = ctx.grab(
        mix(id_, "marker", markers[hit->index].id.value()), hit->bounds);
    if (interaction.pressed) {
      pressed_marker = hit->index;
    }
    if (!scratch.right_marker.has_value() && ctx.secondary_pointer_pressed() &&
        hit->bounds.contains(ctx.pointer_x(), ctx.pointer_y()) &&
        strip_graph.contains(ctx.pointer_x(), ctx.pointer_y())) {
      scratch.right_marker = hit->index;
    }
    if (markers[hit->index].id == markers_.drag && interaction.held) {
      const float dx = ctx.pointer_x() - markers_.drag_start_x;
      markers_.drag_moved = markers_.drag_moved || std::fabs(dx) >= 3.0F;
      if (markers_.drag_moved) {
        const core::i64 pointer_frame =
            static_cast<core::i64>(std::llround(x_to_frame(ctx.pointer_x())));
        core::i64 delta = pointer_frame - markers_.drag_anchor;
        markers_.snap_guide.reset();
        const scene::TimelineMarker &dragged = markers[hit->index];
        if (snapping_ && !ctx.alt_held()) {
          std::vector<core::i64> targets = edit_points(p, doc::NodeId{}, true);
          targets.push_back(shown_playhead);
          if (const auto snapped =
                  nearest_snap(dragged.frame + delta, targets, snap_tolerance)) {
            delta = *snapped - dragged.frame;
            markers_.snap_guide = *snapped;
          }
        }
        // No selected marker may pass frame zero.
        for (const doc::NodeId id : markers_.selected) {
          if (const auto moving = marker_index(p, id)) {
            delta = std::max(delta, -markers[*moving].frame);
          }
        }
        markers_.drag_delta = delta;
      }
      frame.editing = true;
      if (interaction.released) {
        if (markers_.drag_moved && markers_.drag_delta != 0) {
          for (const doc::NodeId id : markers_.selected) {
            if (const auto moving = marker_index(p, id)) {
              TimelineMarkerEdit edit{TimelineMarkerAction::Update,
                                      markers[*moving]};
              edit.marker.frame += markers_.drag_delta;
              frame.marker_edits.push_back(std::move(edit));
            }
          }
        } else if (!markers_.drag_moved && interaction.clicked) {
          const double now = ctx.time_seconds();
          if (markers_.click == markers[hit->index].id &&
              now >= markers_.click_time &&
              now - markers_.click_time <= double_click_seconds) {
            // A second click names it. Opened on release, so the press that
            // began the click cannot close the field again.
            markers_.name_node = markers_.click;
            markers_.name_draft = markers[hit->index].name;
            ctx.focus_text_field(
                mix(id_, "marker name", markers_.name_node.value()),
                markers_.name_draft);
            markers_.click = doc::NodeId{};
          } else {
            // A click goes there, so the markers are a way around the show.
            frame.seek_to_frame =
                std::clamp(static_cast<core::u64>(std::max<core::i64>(
                               markers[hit->index].frame, 0)),
                           first, last);
            markers_.click = markers[hit->index].id;
            markers_.click_time = now;
          }
        }
        markers_.drag = doc::NodeId{};
        markers_.drag_moved = false;
        markers_.drag_delta = 0;
        markers_.snap_guide.reset();
      }
    }
  }
  ctx.pop_clip();
  if (pressed_marker.has_value()) {
    const scene::TimelineMarker &marker = markers[*pressed_marker];
    const auto found = std::find(markers_.selected.begin(),
                                 markers_.selected.end(), marker.id);
    if (ctx.ctrl_held()) {
      if (found != markers_.selected.end()) {
        markers_.selected.erase(found);
      } else {
        markers_.selected.push_back(marker.id);
      }
    } else if (ctx.shift_held()) {
      if (found == markers_.selected.end()) {
        markers_.selected.push_back(marker.id);
      }
    } else if (found == markers_.selected.end()) {
      markers_.selected.assign(1U, marker.id);
    }
    std::sort(markers_.selected.begin(), markers_.selected.end());
    // Choosing a marker leaves the key and layer domains, so Delete and the
    // clipboard aim at it rather than at whatever was selected before.
    keys_.selected.clear();
    if (std::find(markers_.selected.begin(), markers_.selected.end(),
                  marker.id) != markers_.selected.end()) {
      markers_.drag = marker.id;
      markers_.drag_start_x = ctx.pointer_press_x();
      markers_.drag_anchor =
          static_cast<core::i64>(std::llround(x_to_frame(ctx.pointer_press_x())));
      markers_.drag_delta = 0;
      markers_.drag_moved = false;
    }
  }

  // The empty strip scrubs like the ruler, and a double-click drops a marker
  // where it lands.
  const gui::Interaction strip_it =
      ctx.grab(mix(id_, "marker strip"), strip_graph);
  if (strip_it.pressed) {
    const double now = ctx.time_seconds();
    if (now >= markers_.strip_click_time &&
        now - markers_.strip_click_time <= double_click_seconds &&
        std::fabs(ctx.pointer_x() - markers_.strip_click_x) <= 4.0F) {
      create_marker(p, 
          static_cast<core::i64>(rounded_frame(x_to_frame(ctx.pointer_x()),
                                               0U, std::max(last, first))),
          scene::CueAction::None);
      markers_.strip_click_time = -1.0;
    } else {
      markers_.strip_click_time = now;
      markers_.strip_click_x = ctx.pointer_x();
    }
    markers_.selected.clear();
  }
  if (strip_it.held) {
    frame.seek_to_frame =
        rounded_frame(x_to_frame(ctx.pointer_x()), first, last);
  }

  // Draw: loop brackets under everything, then flags and tags.
  ctx.push_clip(strip_graph);
  for (std::size_t index = 0U; index < markers.size(); ++index) {
    const scene::TimelineMarker &marker = markers[index];
    if (marker.cue != scene::CueAction::Loop) {
      continue;
    }
    core::i64 section = signed_frame(first);
    for (std::size_t earlier = 0U; earlier < index; ++earlier) {
      if (markers[earlier].is_cue() && markers[earlier].frame < marker.frame &&
          markers[earlier].frame >= signed_frame(first)) {
        section = markers[earlier].frame;
      }
    }
    const float from = frame_to_x(static_cast<double>(section));
    const float to = frame_to_x(static_cast<double>(shown_marker_frame(marker)));
    const bool looping = timeline.looping_cue == marker.id;
    const gui::UiColor color = alpha(timeline_marker_color(marker.color),
                                     looping ? 0.95F : 0.45F);
    const float y = strip.y + strip.height - 4.0F;
    ctx.panel(gui::Rect{from, y, std::max(to - from, 1.0F), 1.0F}, color);
    ctx.panel(gui::Rect{from, y - 4.0F, 1.0F, 5.0F}, color);
    ctx.draw_list().add_triangle(from + 1.0F, y - 3.0F, from + 6.0F, y - 0.5F,
                                 from + 1.0F, y + 2.0F, color);
    // With no wait, a crossfade overlaps the end of the section: a ramp
    // over the frames that fade out, capped as the transport caps it.
    if (marker.loop_wait_ms == 0U && marker.loop_fade_ms != 0U) {
      const double rate =
          static_cast<double>(timeline.rate_numerator) /
          static_cast<double>(std::max(timeline.rate_denominator, 1U));
      const double length =
          static_cast<double>(shown_marker_frame(marker) - section + 1);
      const double fade =
          std::min(std::round(static_cast<double>(marker.loop_fade_ms) *
                              rate / 1'000.0),
                   std::floor(length / 2.0));
      if (fade >= 1.0) {
        const float ramp = frame_to_x(
            static_cast<double>(shown_marker_frame(marker)) - fade + 1.0);
        ctx.draw_list().add_triangle(ramp, y, to, y - 6.0F, to, y,
                                     alpha(color, 0.6F));
      }
    }
  }
  for (const MarkerHit &hit : marker_hits) {
    const scene::TimelineMarker &marker = markers[hit.index];
    const float marker_x =
        frame_to_x(static_cast<double>(shown_marker_frame(marker)));
    const bool chosen =
        std::find(markers_.selected.begin(), markers_.selected.end(),
                  marker.id) != markers_.selected.end();
    const bool held = timeline.held_cue == marker.id;
    const bool upcoming = timeline.next_cue == marker.id;
    gui::UiColor color = timeline_marker_color(marker.color);
    if (held) {
      color = theme.playhead;
    }
    const float top = strip.y + 2.0F;
    const float height = strip.height - 5.0F;
    if (marker.is_cue()) {
      // A cue is a chip carrying its number, filled so it reads from across
      // a stage.
      const gui::Rect chip{marker_x, top, hit.bounds.width - marker_hit_reach,
                           height};
      ctx.panel(chip, alpha(color, chosen ? 0.95F : 0.72F));
      ctx.panel(gui::Rect{marker_x, top, 2.0F, height + 3.0F}, color);
      if (chosen || upcoming) {
        draw_border(ctx, chip, chosen ? theme.text : alpha(theme.text, 0.6F));
      }
      gui::draw_icon(ctx.draw_list(),
                     marker.cue == scene::CueAction::Hold ? gui::Icon::Hold
                                                          : gui::Icon::Loop,
                     marker_x + 8.0F, top + height * 0.5F,
                     theme.background);
    } else {
      // A plain marker is a pennant on a staff.
      ctx.panel(gui::Rect{marker_x, top, 1.5F, height + 3.0F}, color);
      ctx.draw_list().add_triangle(marker_x + 1.5F, top, marker_x + 9.0F,
                                   top + 4.0F, marker_x + 1.5F, top + 8.0F,
                                   color);
      if (chosen) {
        ctx.panel(gui::Rect{marker_x - 1.0F, top + height + 1.0F,
                            hit.bounds.width - marker_hit_reach, 1.0F},
                  theme.text);
      }
    }
    if (markers_.name_node == marker.id) {
      continue;
    }
    std::string label = marker.is_cue() ? cue_tag(cue_numbers[hit.index]) : "";
    if (!marker.name.empty()) {
      label += label.empty() ? marker.name : " " + marker.name;
    }
    ctx.label(gui::Rect{marker_x + (marker.is_cue() ? 15.0F : 11.0F), top,
                        std::max(hit.bounds.width - marker_hit_reach -
                                     (marker.is_cue() ? 16.0F : 11.0F),
                                 0.0F),
                        height},
              label,
              marker.is_cue() ? theme.background
                              : (chosen ? theme.text : color));
    std::string tip = marker.is_cue()
                          ? (marker.cue == scene::CueAction::Hold
                                 ? "Hold cue "
                                 : "Loop cue ")
                          : "Marker ";
    tip += label;
    tip += " at ";
    tip += timecode(p, static_cast<core::u64>(std::max<core::i64>(marker.frame, 0)));
    if (marker.cue == scene::CueAction::Loop) {
      if (marker.loop_wait_ms != 0U) {
        tip += ", waits " + format_cue_seconds(marker.loop_wait_ms);
      }
      if (marker.loop_fade_ms != 0U) {
        tip += ", crossfades " + format_cue_seconds(marker.loop_fade_ms);
      }
    }
    if (!marker.note.empty()) {
      tip += " - ";
      tip += marker.note;
    }
    ctx.tooltip(hit.bounds, tip);
  }
  ctx.pop_clip();

  // The inline name field sits on its marker's tag.
  if (markers_.name_node != doc::NodeId{}) {
    if (const auto index = marker_index(p, markers_.name_node)) {
      const gui::UiId name_id =
          mix(id_, "marker name", markers_.name_node.value());
      const float field_x = std::clamp(
          frame_to_x(static_cast<double>(markers[*index].frame)), graph.x,
          std::max(graph.x, graph.x + graph.width - 160.0F));
      static_cast<void>(ctx.text_field(
          name_id,
          gui::Rect{field_x, strip.y + 1.0F, 160.0F, strip.height - 3.0F},
          markers_.name_draft));
      if (ctx.focus() != name_id) {
        if (markers_.name_draft != markers[*index].name &&
            markers_.name_draft.size() <= scene::max_timeline_marker_name_bytes) {
          TimelineMarkerEdit edit{TimelineMarkerAction::Update,
                                  markers[*index]};
          edit.marker.name = markers_.name_draft;
          frame.marker_edits.push_back(std::move(edit));
        }
        markers_.name_node = doc::NodeId{};
        markers_.name_draft.clear();
      } else {
        frame.editing = true;
      }
    }
  }

}

core::i64
TimelinePanel::shown_marker_frame(const scene::TimelineMarker &marker) const {
  if (markers_.drag_moved &&
      std::find(markers_.selected.begin(), markers_.selected.end(),
                marker.id) != markers_.selected.end()) {
    return std::max<core::i64>(marker.frame + markers_.drag_delta, 0);
  }
  return marker.frame;
}

} // namespace mediaserver::editor
