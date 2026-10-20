#include <mediaserver/editor/timeline_panel.hpp>

#include <mediaserver/editor/cues_panel.hpp>

#include "timeline/timeline_internal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <tuple>

namespace mediaserver::editor {
using namespace timeline;

gui::UiColor timeline_marker_color(core::u8 index) noexcept {
  constexpr std::array<gui::UiColor, scene::timeline_marker_colors> palette{{
      {0.93F, 0.36F, 0.35F, 1.0F},
      {0.96F, 0.60F, 0.27F, 1.0F},
      {0.94F, 0.84F, 0.33F, 1.0F},
      {0.42F, 0.84F, 0.47F, 1.0F},
      {0.32F, 0.82F, 0.90F, 1.0F},
      {0.40F, 0.58F, 0.98F, 1.0F},
      {0.71F, 0.50F, 0.96F, 1.0F},
      {0.95F, 0.47F, 0.73F, 1.0F},
  }};
  return palette[index % palette.size()];
}

std::string_view timeline_marker_color_name(core::u8 index) noexcept {
  constexpr std::array<std::string_view, scene::timeline_marker_colors> names{
      {"Red", "Orange", "Yellow", "Green", "Cyan", "Blue", "Purple", "Pink"}};
  return names[index % names.size()];
}

std::string format_timecode(core::u64 frame, core::u32 rate_numerator,
                            core::u32 rate_denominator) {
  const core::u64 fps = nominal_rate(rate_numerator, rate_denominator);
  const core::u64 total_seconds = frame / fps;
  std::array<char, 48U> text{};
  const int length = std::snprintf(
      text.data(), text.size(), "%02llu:%02llu:%02llu:%02llu",
      static_cast<unsigned long long>(total_seconds / 3'600U),
      static_cast<unsigned long long>((total_seconds / 60U) % 60U),
      static_cast<unsigned long long>(total_seconds % 60U),
      static_cast<unsigned long long>(frame % fps));
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

std::optional<core::u64> parse_timecode(std::string_view text,
                                        core::u32 rate_numerator,
                                        core::u32 rate_denominator,
                                        core::u64 relative_to) {
  while (!text.empty() && text.front() == ' ') {
    text.remove_prefix(1U);
  }
  while (!text.empty() && text.back() == ' ') {
    text.remove_suffix(1U);
  }
  int sign = 0;
  if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
    sign = text.front() == '+' ? 1 : -1;
    text.remove_prefix(1U);
  }
  if (text.empty()) {
    return std::nullopt;
  }
  // Up to four fields, most significant first; the last is always frames.
  std::array<core::u64, 4U> fields{};
  std::size_t count = 0U;
  std::size_t begin = 0U;
  for (std::size_t index = 0U; index <= text.size(); ++index) {
    const bool end = index == text.size();
    const char c = end ? ':' : text[index];
    if (c == ':' || c == ';' || c == '.') {
      if (index == begin || count == fields.size()) {
        return std::nullopt;
      }
      core::u64 value{};
      const auto parsed =
          std::from_chars(text.data() + begin, text.data() + index, value);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + index) {
        return std::nullopt;
      }
      fields[count++] = value;
      begin = index + 1U;
    } else if (c < '0' || c > '9') {
      return std::nullopt;
    }
  }
  const core::u64 fps = nominal_rate(rate_numerator, rate_denominator);
  constexpr core::u64 limit =
      static_cast<core::u64>(std::numeric_limits<core::i64>::max());
  core::u64 frames{};
  if (count == 1U) {
    frames = fields[0U];
  } else {
    // Hours, minutes and seconds accumulate in base sixty; the last field is
    // always frames.
    core::u64 seconds{};
    for (std::size_t index = 0U; index + 1U < count; ++index) {
      if (seconds > limit / 60U) {
        return std::nullopt;
      }
      seconds = seconds * 60U + fields[index];
    }
    if (seconds > limit / fps) {
      return std::nullopt;
    }
    frames = seconds * fps + fields[count - 1U];
  }
  if (frames > limit) {
    return std::nullopt;
  }
  if (sign > 0) {
    return relative_to > limit - frames ? limit : relative_to + frames;
  }
  if (sign < 0) {
    return relative_to > frames ? relative_to - frames : 0U;
  }
  return frames;
}

bool TimelinePanel::selected(
    doc::NodeId node, std::span<const doc::NodeId> selection) const noexcept {
  return std::find(selection.begin(), selection.end(), node) != selection.end();
}

void TimelinePanel::clear_key_context() noexcept {
  keys_.selected.clear();
  keys_.dragging = false;
  keys_.duplicate = false;
  keys_.drag_keys.clear();
  keys_.drag_delta = 0;
  keys_.snap_guide.reset();
  key_tools_.open = false;
  key_tools_.scale_open = false;
  hidden_channels_.clear();
  active_channel_.reset();
  key_tools_.value_field = NumberField{};
  keys_.marquee_tracking = false;
  keys_.marquee_active = false;
  keys_.marquee_base.clear();
  keys_.lasso_points.clear();
  // Markers are scene content, so a Stage-scene switch leaves none of the
  // old scene's selected or half-dragged.
  markers_.selected.clear();
  markers_.drag = doc::NodeId{};
  markers_.drag_moved = false;
  markers_.drag_delta = 0;
  markers_.snap_guide.reset();
  markers_.name_node = doc::NodeId{};
  markers_.name_draft.clear();
  menu_entries_.clear();
}

void TimelinePanel::select_marker(doc::NodeId marker) {
  markers_.selected.assign(1U, marker);
}

void TimelinePanel::reset_project_state() noexcept {
  replacement_targets_.clear();
  time_axis_ = TimelineTimeAxis{};
  row_scroll_ = 0.0F;
  rows_state_.expanded_layers.clear();
  rows_state_.expanded_groups.clear();
  rows_state_.expanded_hierarchies.clear();
  rows_state_.hierarchy_selection = doc::NodeId{};
  selected_lane_ = doc::NodeId{};
  reorder_.lane_destination = doc::NodeId{};
  rename_.reset();
  clicks_.reset();
  search_.clear();
  range_fields_ = NumberField{};
  clear_key_context();
  keys_.drag_anchor_frame = 0;
  toolbar_.rate_menu_open = false;
  edit_tool_ = TimelineEditTool::Standard;
  ruler_.range_drag = RangeHandle::None;
  toolbar_.time_drag_id = gui::UiId{};
  toolbar_.time_drag_moved = false;
  toolbar_.time_edit_id = gui::UiId{};
  toolbar_.time_draft.clear();
  markers_.click = doc::NodeId{};
  timeline_focused_ = false;
  panning_ = false;
  keys_.marquee_start_x = 0.0F;
  keys_.marquee_start_y = 0.0F;
  keys_.marquee_shift = false;
  keys_.marquee_ctrl = false;
  reorder_.lane_node = doc::NodeId{};
  reorder_.lane_start_x = 0.0F;
  reorder_.lane_start_y = 0.0F;
  reorder_.lane_moved = false;
  reorder_.layer_node = doc::NodeId{};
  reorder_.layer_nodes.clear();
  reorder_.layer_start_x = 0.0F;
  reorder_.layer_start_y = 0.0F;
  reorder_.layer_moved = false;
  clip_.drag = ClipDrag::None;
  clip_.node = doc::NodeId{};
  clip_.peer = doc::NodeId{};
  clip_.origin = scene::ClipTiming{};
  clip_.proposed = scene::ClipTiming{};
  clip_.pending_edit.reset();
  clip_.pending_frames = 0U;
  clip_.status.clear();
  clip_.status_error = false;
  clip_.status_until = 0.0;
  previous_draw_time_ = 0.0;
  clip_.anchor = 0;
  clip_.delta = 0;
  clip_.start_x = 0.0F;
  clip_.start_y = 0.0F;
  clip_.release.reset();
  clip_.moved = false;
  clip_.drop_lane.reset();
  clip_.snap_guide.reset();
}

void TimelineTimeAxis::fit(double begin, double span, float width) noexcept {
  pixels_per_frame =
      std::clamp(static_cast<float>(static_cast<double>(width) / span),
                 minimum_pixels_per_frame, maximum_pixels_per_frame);
  left_frame = begin;
}

void TimelineTimeAxis::zoom_about(double anchor, float anchor_x, float origin,
                                  double factor) noexcept {
  pixels_per_frame = std::clamp(
      static_cast<float>(static_cast<double>(pixels_per_frame) * factor),
      minimum_pixels_per_frame, maximum_pixels_per_frame);
  left_frame = anchor - static_cast<double>(anchor_x - origin) /
                            static_cast<double>(pixels_per_frame);
}

TimelinePanelFrame
TimelinePanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                    const EditorTimeline &timeline,
                    std::span<const scene::ChannelKeyframes> keyframes,
                    std::span<const ElementInfo> elements,
                    std::span<const doc::NodeId> selection, doc::NodeId primary,
                    std::span<const scene::EffectInstance> effects,
                    std::span<const output::ProjectionSurface> surfaces,
                    doc::NodeId selected_surface,
                    std::span<const scene::TimelineLane> lanes,
                    std::optional<core::i64> clipboard_span,
                    std::optional<TimelineKeyScalePreview> key_scale_preview,
                    std::span<const TimelineFilmstripObservation> filmstrips,
                    std::span<const TimelineWaveformObservation> waveforms,
                    std::span<const scene::TimelineMarker> markers) {
  return draw(ctx, bounds,
              TimelineDrawInput{timeline, keyframes, elements, selection,
                                primary, effects, surfaces, selected_surface,
                                lanes, clipboard_span, key_scale_preview,
                                filmstrips, waveforms, markers});
}

TimelinePanelFrame TimelinePanel::draw(gui::UiContext &ctx,
                                       const gui::Rect &bounds,
                                       const TimelineDrawInput &input) {
  replacement_targets_.clear();
  Pass p{ctx, bounds, input};
  if (!open_pass(p)) {
    return std::move(p.frame);
  }
  prune_key_selection(p);
  navigate(p);
  request_derivatives(p);
  draw_toolbar(p);
  draw_ruler(p);
  draw_marker_strip(p);
  track_key_drag(p);
  resolve_clip_gesture(p);
  resolve_reorder_targets(p);
  draw_rows(p);
  resolve_track_drop(p);
  finish_marquee(p);
  draw_time_overlays(p);
  commit_key_drag(p);
  draw_key_tools(p);
  draw_rate_menu(p);
  draw_drag_feedback(p);
  handle_actions(p);
  close_pass(p);
  return std::move(p.frame);
}

bool TimelinePanel::open_pass(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const gui::Rect &bounds = p.bounds;
  const EditorTimeline &timeline = p.input.timeline;
  const std::span<const scene::ChannelKeyframes> keyframes = p.input.keyframes;
  const std::span<const ElementInfo> elements = p.input.elements;
  const std::span<const doc::NodeId> selection = p.input.selection;
  const std::span<const scene::EffectInstance> effects = p.input.effects;
  const std::span<const output::ProjectionSurface> surfaces = p.input.surfaces;
  const std::span<const scene::TimelineLane> lanes = p.input.lanes;
  const std::span<const scene::TimelineMarker> markers = p.input.markers;
  const doc::NodeId primary = p.input.primary;
  const std::optional<core::i64> &clipboard_span = p.input.clipboard_span;
  TimelinePanelFrame &frame = p.frame;
  const gui::Rect &graph = p.layout.graph;

  p.gesture_x = ctx.pointer_released()
      ? ctx.pointer_release_x() : ctx.pointer_x();
  p.gesture_y = ctx.pointer_released()
      ? ctx.pointer_release_y() : ctx.pointer_y();
  p.typing_at_start = ctx.typing();
  frame.filmstrip_retain = TimelineFilmstripRetainIntent{};
  frame.waveform_retain = TimelineWaveformRetainIntent{};
  ctx.panel(bounds, ctx.theme().panel);
  if (ctx.pointer_pressed()) {
    timeline_focused_ =
        bounds.contains(ctx.pointer_press_x(), ctx.pointer_press_y());
  }
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    range_fields_.cancel(ctx);
    return false;
  }
  ctx.push_clip(bounds);

  p.first = std::min(timeline.first_frame, timeline.last_frame);
  auto &first = p.first;
  p.last = std::max(timeline.first_frame, timeline.last_frame);
  auto &last = p.last;
  p.first_i = signed_frame(first);
  p.last_i = signed_frame(last);

  // The anchor must actually belong to the selection. A stale primary falls
  // back to the first selected element, exactly as the shell's viewport does.
  p.anchor = primary;
  auto &anchor = p.anchor;
  if (!selected(anchor, selection)) {
    anchor = selection.empty() ? doc::NodeId{} : selection.front();
  }
  const ElementInfo *const anchor_info = find_element(elements, anchor);

  // Canonical advanced-edit context. Controls only light up when the current
  // selection has the shape their planner requires; the Director validates
  // the same identities again against its projected document before it
  // authors anything.
  auto &selected_clips = p.selected_clips;
  selected_clips.reserve(selection.size());
  p.clip_selection_valid = !selection.empty();
  auto &clip_selection_valid = p.clip_selection_valid;
  for (const doc::NodeId id : selection) {
    const ElementInfo *const element = find_element(elements, id);
    if (element == nullptr || !editable_clip(*element) ||
        !exclusive_clip_end(element->timing).has_value()) {
      clip_selection_valid = false;
      continue;
    }
    selected_clips.push_back(element);
  }
  clip_selection_valid =
      clip_selection_valid && selected_clips.size() == selection.size();
  std::sort(selected_clips.begin(), selected_clips.end(),
            [](const ElementInfo *left, const ElementInfo *right) noexcept {
              if (left->timing.start != right->timing.start) {
                return left->timing.start < right->timing.start;
              }
              return left->node < right->node;
            });
  const bool same_selected_lane =
      clip_selection_valid &&
      std::all_of(selected_clips.begin(), selected_clips.end(),
                  [&](const ElementInfo *clip) noexcept {
                    return clip->timeline_lane ==
                           selected_clips.front()->timeline_lane;
                  });

  auto &target_lane = p.target_lane;
  const auto explicit_lane = std::find_if(
      lanes.begin(), lanes.end(), [&](const scene::TimelineLane &lane) {
        return !lane.implicit && lane.id == selected_lane_;
      });
  if (explicit_lane != lanes.end() && !explicit_lane->locked) {
    target_lane = explicit_lane->id;
  } else if (same_selected_lane) {
    target_lane = selected_clips.front()->timeline_lane;
  }
  p.paste_ready = clipboard_span.has_value() && *clipboard_span > 0 &&
                           target_lane.has_value();

  p.layout =
      timeline::layout_timeline(bounds, list_width_);
  // The part of the graph that is time rather than the ruler's own band.
  p.fps =
      nominal_rate(timeline.rate_numerator, timeline.rate_denominator);

  // Markers arrive in (frame, id) order. A cue's number is its place among
  // the cues alone, which is the number a stage manager calls.
  p.cue_numbers.assign(markers.size(), 0U);
  auto &cue_numbers = p.cue_numbers;
  auto &any_cue = p.any_cue;
  {
    std::size_t number = 0U;
    for (std::size_t index = 0U; index < markers.size(); ++index) {
      if (markers[index].is_cue()) {
        cue_numbers[index] = ++number;
        any_cue = true;
      }
    }
  }
  markers_.selected.erase(
      std::remove_if(markers_.selected.begin(), markers_.selected.end(),
                     [&](doc::NodeId id) { return !marker_index(p, id); }),
      markers_.selected.end());
  if (markers_.drag != doc::NodeId{} && !marker_index(p, markers_.drag)) {
    markers_.drag = doc::NodeId{};
    markers_.drag_moved = false;
    markers_.drag_delta = 0;
  }
  if (markers_.name_node != doc::NodeId{} && !marker_index(p, markers_.name_node)) {
    markers_.name_node = doc::NodeId{};
    markers_.name_draft.clear();
  }

  if (!time_axis_.initialized && graph.width > 0.0F) {
    const double span =
        std::max(static_cast<double>(last) - static_cast<double>(first), 1.0);
    time_axis_.fit(static_cast<double>(first), span, graph.width);
    time_axis_.initialized = true;
    if (anchor != doc::NodeId{} && anchor_info != nullptr &&
        !is_light(anchor_info->kind)) {
      rows_state_.expanded_layers.insert(anchor.value());
    }
  }
  if (time_axis_.pixels_per_frame <= 0.0F) {
    time_axis_.pixels_per_frame = 1.0F;
  }


  // The rows this frame lists, from the operator's twirls and search.
  p.built = build_rows(
      rows_state_,
      RowsInput{elements, lanes, surfaces, effects, keyframes,
                timeline.show_frame, search_, anchor},
      ctx.theme());


  return true;
}

std::string TimelinePanel::timecode(Pass &p, core::u64 value) {
  const EditorTimeline &timeline = p.input.timeline;

  if (ruler_.frames) {
    return std::to_string(value);
  }
  return format_timecode(value, timeline.rate_numerator,
                         timeline.rate_denominator);
}

std::optional<std::size_t>
TimelinePanel::marker_index(Pass &p, doc::NodeId id) {
  const std::span<const scene::TimelineMarker> markers = p.input.markers;

  for (std::size_t index = 0U; index < markers.size(); ++index) {
    if (markers[index].id == id) {
      return index;
    }
  }
  return std::nullopt;
}

void TimelinePanel::navigate(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const gui::Rect &bounds = p.bounds;
  const gui::Rect &ruler = p.layout.ruler;
  const gui::Rect &rows_bounds = p.layout.rows;
  const gui::Rect &graph = p.layout.graph;
  std::vector<Row> &rows = p.built.rows;
  const auto x_to_frame = [&](float x) noexcept {
    return time_axis_.frame(x, graph.x);
  };

  p.content_height = rows_extent(rows);
  auto &content_height = p.content_height;
  p.maximum_row_scroll =
      std::max(content_height - rows_bounds.height, 0.0F);
  auto &maximum_row_scroll = p.maximum_row_scroll;

  // Wheel gestures have the compositor convention: over the graph, wheel
  // zooms about the pointer and Shift+wheel pans. In curve mode Alt+wheel
  // zooms values and Ctrl+wheel pans values; over the layer list it scrolls
  // rows. Time and value axes therefore remain independently addressable.
  if (bounds.contains(ctx.pointer_x(), ctx.pointer_y()) &&
      ctx.pointer_y() >= ruler.y && ctx.wheel() != 0.0F) {
    if (graph_mode_ && ctx.pointer_x() >= graph.x && ctx.alt_held()) {
      // Hold the value under the pointer still, exactly as the time zoom below
      // holds the frame under it. A curve band is normalised inside its own
      // row, so the fixed point is where the pointer sits in that row's band:
      // with normal = 0.5 + deviation * zoom + pan, keeping normal fixed while
      // zoom scales by k means pan becomes pan * k + (normal - 0.5) * (1 - k).
      const float previous = curve_value_zoom_;
      curve_value_zoom_ = std::clamp(
          curve_value_zoom_ * static_cast<float>(std::pow(
                                  1.15, static_cast<double>(ctx.wheel()))),
          0.25F, 16.0F);
      const float local = ctx.pointer_y() - rows_bounds.y + row_scroll_;
      const std::optional<std::size_t> under = row_at(rows, local);
      const float band =
          (under.has_value() ? rows[*under].height : row_height) - 8.0F;
      if (previous > 0.0F && band > 0.0F &&
          ctx.pointer_y() >= rows_bounds.y) {
        const float row_top =
            rows_bounds.y - row_scroll_ +
            (under.has_value() ? rows[*under].top
                               : std::floor(local / row_height) * row_height);
        const float normal =
            std::clamp((ctx.pointer_y() - row_top - 4.0F) / band, 0.0F, 1.0F);
        const float ratio = curve_value_zoom_ / previous;
        curve_value_pan_ =
            std::clamp(curve_value_pan_ * ratio +
                           (normal - 0.5F) * (1.0F - ratio),
                       -4.0F, 4.0F);
      }
    } else if (graph_mode_ && ctx.pointer_x() >= graph.x && ctx.ctrl_held()) {
      curve_value_pan_ =
          std::clamp(curve_value_pan_ + ctx.wheel() * 0.08F, -4.0F, 4.0F);
    } else if (ctx.shift_held()) {
      time_axis_.pan(-(static_cast<double>(ctx.wheel()) * 80.0));
    } else if (ctx.pointer_x() >= graph.x) {
      const double under_pointer = x_to_frame(ctx.pointer_x());
      const double factor = std::pow(1.15, static_cast<double>(ctx.wheel()));
      time_axis_.zoom_about(under_pointer, ctx.pointer_x(), graph.x, factor);
    } else {
      row_scroll_ -= ctx.wheel() * row_height * 3.0F;
    }
  }

  const bool began_pan = ctx.middle_pointer_pressed();
  if (began_pan) {
    panning_ = graph.contains(ctx.pointer_x(), ctx.pointer_y());
    if (panning_) {
      timeline_focused_ = true;
    }
  }
  if (panning_ && ctx.middle_pointer_down() && !began_pan) {
    time_axis_.pan(-static_cast<double>(ctx.pointer_dx()));
    if (graph_mode_ && ctx.ctrl_held()) {
      curve_value_pan_ =
          std::clamp(curve_value_pan_ + ctx.pointer_dy() / 100.0F, -4.0F, 4.0F);
    } else {
      row_scroll_ -= ctx.pointer_dy();
    }
  }
  if (ctx.middle_pointer_released()) {
    panning_ = false;
  }
  // In/Out delimit playback, not the editable workspace. Signed placement
  // positions let an operator move a clip partly before frame zero without
  // trimming away its source; both ends of the workspace remain reachable.
  const double draw_delta = std::clamp(
      ctx.time_seconds() - previous_draw_time_, 0.0, 0.05);
  previous_draw_time_ = ctx.time_seconds();
  if (clip_.drag != ClipDrag::None && clip_.moved && ctx.pointer_down()) {
    constexpr float edge_zone = 24.0F;
    float velocity = 0.0F;
    if (ctx.pointer_x() < graph.x + edge_zone) {
      velocity = -std::clamp((graph.x + edge_zone - ctx.pointer_x()) /
                                edge_zone, 0.0F, 1.0F);
    } else if (ctx.pointer_x() > graph.x + graph.width - edge_zone) {
      velocity = std::clamp((ctx.pointer_x() - graph.x - graph.width +
                             edge_zone) / edge_zone, 0.0F, 1.0F);
    }
    time_axis_.pan(static_cast<double>(velocity) * 720.0 * draw_delta);
  }
  time_axis_.left_frame = std::clamp(time_axis_.left_frame,
      -static_cast<double>(std::numeric_limits<core::i64>::max() / 4),
      static_cast<double>(std::numeric_limits<core::i64>::max() / 4));
  row_scroll_ = std::clamp(row_scroll_, 0.0F, maximum_row_scroll);

}

void TimelinePanel::request_derivatives(Pass &p) {
  const std::span<const ElementInfo> elements = p.input.elements;
  TimelinePanelFrame &frame = p.frame;
  const gui::Rect &rows_bounds = p.layout.rows;
  std::vector<Row> &rows = p.built.rows;

  // The panel owns visibility, so it is the only layer that can truthfully
  // distinguish visible work from adjacent-row prefetch. Keys are still
  // deduplicated by full derivative identity: two clips borrowing one Asset
  // revision schedule and retain one atlas.
  TimelineFilmstripRetainIntent &filmstrip_retain = *frame.filmstrip_retain;
  const auto append_filmstrip_candidate =
      [&](const TimelineFilmstripObservation &observation,
          TimelineFilmstripRequestPriority priority) {
        if (std::find(filmstrip_retain.keys.begin(),
                      filmstrip_retain.keys.end(), observation.key) ==
            filmstrip_retain.keys.end()) {
          filmstrip_retain.keys.push_back(observation.key);
        }
        const bool needs_request =
            observation.state == assets::DerivativeState::Missing ||
            observation.state == assets::DerivativeState::Stale ||
            observation.state == assets::DerivativeState::Cancelled;
        if (!needs_request) {
          return;
        }
        auto existing = std::find_if(
            frame.filmstrip_requests.begin(), frame.filmstrip_requests.end(),
            [&](const TimelineFilmstripRequestIntent &request) noexcept {
              return request.key == observation.key;
            });
        if (existing != frame.filmstrip_requests.end()) {
          if (priority == TimelineFilmstripRequestPriority::Visible) {
            existing->priority = priority;
          }
          return;
        }
        if (frame.filmstrip_requests.size() < maximum_filmstrip_requests) {
          frame.filmstrip_requests.push_back(
              TimelineFilmstripRequestIntent{observation.key, priority});
        }
      };
  TimelineWaveformRetainIntent &waveform_retain = *frame.waveform_retain;
  const auto append_waveform_candidate =
      [&](const TimelineWaveformObservation &observation,
          TimelineWaveformRequestPriority priority) {
        if (std::find(waveform_retain.keys.begin(), waveform_retain.keys.end(),
                      observation.key) == waveform_retain.keys.end()) {
          waveform_retain.keys.push_back(observation.key);
        }
        const bool needs_request =
            observation.state == assets::DerivativeState::Missing ||
            observation.state == assets::DerivativeState::Stale ||
            observation.state == assets::DerivativeState::Cancelled;
        if (!needs_request) {
          return;
        }
        auto existing = std::find_if(
            frame.waveform_requests.begin(), frame.waveform_requests.end(),
            [&](const TimelineWaveformRequestIntent &request) noexcept {
              return request.key == observation.key;
            });
        if (existing != frame.waveform_requests.end()) {
          if (priority == TimelineWaveformRequestPriority::Visible) {
            existing->priority = priority;
          }
          return;
        }
        if (frame.waveform_requests.size() < maximum_waveform_requests) {
          frame.waveform_requests.push_back(
              TimelineWaveformRequestIntent{observation.key, priority});
        }
      };
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const Row &row = rows[index];
    if (row.kind != Row::Kind::Layer || row.surface ||
        row.element_kind != ElementKind::Media) {
      continue;
    }
    const ElementInfo *const element = find_element(elements, row.node);
    const TimelineFilmstripObservation *const observation =
        element != nullptr && element->timing.placed()
            ? filmstrip_for_clip(p, row.node)
            : nullptr;
    const TimelineWaveformObservation *const waveform =
        element != nullptr && element->timing.placed()
            ? waveform_for_clip(p, row.node)
            : nullptr;
    if (observation == nullptr && waveform == nullptr) {
      continue;
    }
    const float row_top =
        rows_bounds.y + row.top - row_scroll_;
    const float row_bottom = row_top + row.height;
    const bool visible = row_bottom > rows_bounds.y &&
                         row_top < rows_bounds.y + rows_bounds.height;
    std::size_t distance{};
    if (!visible) {
      const float gap = row_bottom <= rows_bounds.y
                            ? rows_bounds.y - row_bottom
                            : row_top - (rows_bounds.y + rows_bounds.height);
      distance = static_cast<std::size_t>(
          std::floor(std::max(gap, 0.0F) / row_height)) +
                 1U;
    }
    if (observation != nullptr &&
        (visible || distance <= filmstrip_prefetch_rows)) {
      append_filmstrip_candidate(
          *observation,
          visible ? TimelineFilmstripRequestPriority::Visible
                  : TimelineFilmstripRequestPriority::Prefetch);
    }
    if (waveform != nullptr &&
        (visible || distance <= waveform_prefetch_rows)) {
      append_waveform_candidate(
          *waveform,
          visible ? TimelineWaveformRequestPriority::Visible
                  : TimelineWaveformRequestPriority::Prefetch);
    }
  }

}

const TimelineFilmstripObservation *
TimelinePanel::filmstrip_for_clip(Pass &p, doc::NodeId clip) noexcept {
  const std::span<const TimelineFilmstripObservation> filmstrips = p.input.filmstrips;

  const auto found = std::find_if(
      filmstrips.begin(), filmstrips.end(),
      [clip](const TimelineFilmstripObservation &observation) noexcept {
        return observation.clip == clip;
      });
  return found != filmstrips.end() ? std::addressof(*found) : nullptr;
}

const TimelineWaveformObservation *
TimelinePanel::waveform_for_clip(Pass &p, doc::NodeId clip) noexcept {
  const std::span<const TimelineWaveformObservation> waveforms = p.input.waveforms;

  const auto found = std::find_if(
      waveforms.begin(), waveforms.end(),
      [clip](const TimelineWaveformObservation &observation) noexcept {
        return observation.clip == clip;
      });
  return found != waveforms.end() ? std::addressof(*found) : nullptr;
}

void TimelinePanel::draw_rows(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const EditorTimeline &timeline = p.input.timeline;
  const std::span<const scene::ChannelKeyframes> keyframes = p.input.keyframes;
  const std::span<const ElementInfo> elements = p.input.elements;
  const std::span<const doc::NodeId> selection = p.input.selection;
  const std::span<const output::ProjectionSurface> surfaces = p.input.surfaces;
  const doc::NodeId selected_surface = p.input.selected_surface;
  TimelinePanelFrame &frame = p.frame;
  FrameScratch &scratch = p.scratch;
  const float &list_width = p.layout.list_width;
  const gui::Rect &rows_bounds = p.layout.rows;
  const gui::Rect &graph = p.layout.graph;
  std::vector<Row> &rows = p.built.rows;
  const std::vector<doc::NodeId> &visible_order = p.built.visible_order;
  const auto frame_to_x = [&](double value) noexcept {
    return time_axis_.x(value, graph.x);
  };
  const auto x_to_frame = [&](float x) noexcept {
    return time_axis_.frame(x, graph.x);
  };
  auto &first = p.first;
  auto &last = p.last;
  auto &first_i = p.first_i;
  auto &last_i = p.last_i;
  auto &anchor = p.anchor;

  std::vector<std::size_t> track_numbers(rows.size(), 0U);
  {
    std::size_t number = 0U;
    for (std::size_t index = 0U; index < rows.size(); ++index) {
      if (rows[index].kind == Row::Kind::LaneHeader) {
        track_numbers[index] = ++number;
      }
    }
  }
  // A right press on a row opens that row's menu once the rows are drawn.

  ctx.push_clip(rows_bounds);
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const Row &row = rows[index];
    const float y = rows_bounds.y + row.top - row_scroll_;
    if (y + row.height <= rows_bounds.y ||
        y >= rows_bounds.y + rows_bounds.height) {
      continue;
    }
    if (row.kind == Row::Kind::Layer && !row.surface) {
      const float top = std::max(y, rows_bounds.y);
      const float bottom = std::min(y + row.height, rows_bounds.y + rows_bounds.height);
      replacement_targets_.push_back({row.node,
          {rows_bounds.x, top, rows_bounds.width, bottom - top}});
    }
    const float centre_y = y + row.height * 0.5F;
    std::optional<ChannelIdentity> row_channel_identity;
    if (row.kind == Row::Kind::Component || row.kind == Row::Kind::Property) {
      row_channel_identity = ChannelIdentity{
          row.kind == Row::Kind::Property ? row.property_target : row.node,
          row.kind == Row::Kind::Property
              ? row.property
              : group_channel(row.group, row.component)};
    }
    const bool is_selected =
        row.kind == Row::Kind::LaneHeader
            ? row.node == selected_lane_
            : (row.surface ? row.node == selected_surface
                           : selected(row.node, selection));
    const bool layer_like =
        row.kind == Row::Kind::Layer || row.kind == Row::Kind::LaneHeader;
    const gui::Theme &row_theme = ctx.theme();
    const bool row_hovered =
        !ctx.pointer_blocked() &&
        rows_bounds.contains(ctx.pointer_x(), ctx.pointer_y()) &&
        ctx.pointer_y() >= y && ctx.pointer_y() < y + row.height;
    gui::UiColor background{};
    if (is_selected && layer_like) {
      background = row_theme.selection;
    } else if (is_selected) {
      // The selected layer's own properties: tinted, not flooded, so the
      // selection still reads as one row.
      background = mix_colors(row_theme.track, row_theme.selection, 0.35F);
    } else if (row.kind == Row::Kind::LaneHeader) {
      background = row_theme.header;
    } else if (layer_like) {
      background = row_theme.row;
    } else {
      // Property rows sit a shade below the layer they open from, so the eye
      // reads them as that layer's contents rather than as more layers.
      background = row_theme.track;
    }
    ctx.panel(gui::Rect{rows_bounds.x, y, rows_bounds.width, row.height},
              background);
    if (!is_selected && row_hovered && layer_like) {
      ctx.panel(gui::Rect{rows_bounds.x, y, list_width, row.height},
                alpha(row_theme.widget_hover, 0.6F));
    }
    // Every layer is ruled off across the whole panel; property rows only
    // across the lanes, so a twirled-open layer still reads as one block.
    ctx.panel(gui::Rect{layer_like ? rows_bounds.x : graph.x,
                        y + row.height - 1.0F,
                        layer_like ? rows_bounds.width : graph.width, 1.0F},
              row_theme.divider);
    if (layer_like && !row.surface) {
      // The row's own colour down its left edge: the kind for a layer, the
      // track's for a track. Selection takes the accent.
      ctx.panel(gui::Rect{rows_bounds.x, y, 3.0F, row.height - 1.0F},
                is_selected ? row_theme.accent
                            : alpha(row.color, row.locked ? 0.35F : 0.85F));
    }

    const float tree_x = rows_bounds.x + list_metrics::toggle + 6.0F;
    const float twirl_x =
        tree_x + static_cast<float>(row.indent) * list_metrics::indent;
    draw_indent_guides(ctx, tree_x, y, row.height, row.indent,
                       alpha(row_theme.border, 0.9F));
    const core::u64 row_group = group_id(row.node, row.group);
    const bool open = row.hierarchy
        ? (!search_.empty() || rows_state_.expanded_hierarchies.contains(row.node))
        : ((row.kind == Row::Kind::Layer ||
            row.kind == Row::Kind::PropertiesHeader)
               ? rows_state_.expanded_layers.contains(row.node.value())
               : (row.kind == Row::Kind::Group &&
                  rows_state_.expanded_groups.contains(row_group)));
    if (row.expandable) {
      const bool twirl_hovered =
          row_hovered && ctx.pointer_x() >= twirl_x &&
          ctx.pointer_x() < twirl_x + list_metrics::twirl_width;
      draw_twirl(ctx, twirl_x + list_metrics::twirl_width * 0.5F, centre_y,
                 open, twirl_hovered ? row_theme.text : row_theme.text_muted);
    }
    // Left of this a press opens or closes the row; right of it, it means
    // the row itself.
    const float content_x = twirl_x + list_metrics::twirl_width + 2.0F;
    float name_x = content_x;
    gui::Rect name_bounds{content_x, y,
                          std::max(graph.x - content_x - 48.0F, 0.0F),
                          row.height};
    const float lock_x = graph.x - list_metrics::toggle - 4.0F;
    const gui::Rect eye_bounds{rows_bounds.x + 4.0F, y, list_metrics::toggle,
                               row.height};
    const gui::Rect lock_bounds{lock_x, y, list_metrics::toggle, row.height};
    if (row.kind == Row::Kind::LaneHeader) {
      // Tracks are numbered top-down, the way they are called across a
      // stage: "kill track two".
      const std::string number = std::to_string(track_numbers[index]);
      const float badge_width = std::max(18.0F, ctx.font_width(number) + 8.0F);
      const gui::Rect badge{content_x + 2.0F, centre_y - 8.0F, badge_width,
                            16.0F};
      ctx.panel(badge, row.muted ? alpha(row_theme.text_muted, 0.6F)
                                 : row.color);
      ctx.label(gui::Rect{badge.x + std::max((badge.width -
                                              ctx.font_width(number)) *
                                                 0.5F,
                                             0.0F),
                          badge.y, badge.width, badge.height},
                number, row_theme.background);
      name_x = badge.x + badge.width + 8.0F;
      name_bounds = gui::Rect{name_x, y, std::max(lock_x - name_x - 4.0F, 0.0F),
                              row.height};
      if (rename_.editing(row.node.value())) {
        // While a track's name is open the shell holds one lane gesture and
        // keeps the keyboard's shortcuts away from the field.
        frame.lane_editing = true;
        if (auto renamed = rename_.draw(
                ctx, gui::Rect{name_bounds.x - 4.0F, y + 2.0F,
                               name_bounds.width + 4.0F, row.height - 4.0F})) {
          frame.lane_edits.push_back(TimelineLaneEdit{
              TimelineLaneAction::Rename, row.node, std::move(*renamed)});
        }
      } else {
        ctx.label(name_bounds, row.label,
                  row.muted ? row_theme.text_muted : row_theme.text);
      }
      // Muting a track hides it, so its switch is the eye every layer has;
      // the toggles double as the state readout, so no badge repeats them.
      if (row_flag_toggle(ctx, mix(id_, "track mute", row.node.value()),
                          eye_bounds, RowFlag::Visible, !row.muted,
                          row_hovered)) {
        frame.lane_edits.push_back(TimelineLaneEdit{
            TimelineLaneAction::ToggleMute, row.node, {}});
      }
      if (row_flag_toggle(ctx, mix(id_, "track lock", row.node.value()),
                          lock_bounds, RowFlag::Locked, row.locked,
                          row_hovered)) {
        frame.lane_edits.push_back(TimelineLaneEdit{
            TimelineLaneAction::ToggleLock, row.node, {}});
      }
    } else if (row.kind == Row::Kind::Layer) {
      const ElementInfo *const row_element =
          row.surface ? nullptr : find_element(elements, row.node);
      const float icon_cx = content_x + 8.0F;
      if (row_element != nullptr) {
        draw_element_icon(ctx, *row_element, icon_cx, centre_y,
                          row_element->effective_visible
                              ? row.color
                              : alpha(row.color, 0.4F));
      } else {
        draw_kind_icon(ctx, ElementKind::Track, scene::LocalLightKind::Point,
                       icon_cx, centre_y, row.color);
      }
      name_x = icon_cx + 13.0F;
      // Everything the timeline lists can be switched off and protected, not
      // only the clips: a light or an LED surface is as worth muting while
      // working as a plate is. A surface is physical stage geometry rather
      // than a scene node, so it has no eye - but it takes the same padlock,
      // the one its Feed Mapping lock and Stage picking already honour. The
      // environment is always there.
      const bool has_toggles =
          row.surface || row_element == nullptr ||
          row_element->kind != ElementKind::Environment;
      // A surface keeps its badge, just inside the padlock.
      const float name_right =
          (has_toggles ? lock_x : graph.x) - (row.surface ? 60.0F : 0.0F) -
          (has_toggles ? 0.0F : 64.0F);
      name_bounds = gui::Rect{name_x, y,
                              std::max(name_right - name_x - 6.0F, 0.0F),
                              row.height};
      if (rename_.editing(row.node.value())) {
        if (auto renamed = rename_.draw(
                ctx, gui::Rect{name_bounds.x - 4.0F, y + 2.0F,
                               name_bounds.width + 4.0F, row.height - 4.0F})) {
          frame.renames.push_back(TimelineRename{row.node, std::move(*renamed)});
        }
      } else {
        const bool shown =
            row_element == nullptr || row_element->effective_visible;
        ctx.label(name_bounds, row.label,
                  row.locked ? row_theme.text_muted
                             : (shown ? row_theme.text
                                      : alpha(row_theme.text, 0.45F)));
      }
      if (row.surface) {
        ctx.label(gui::Rect{lock_x - 60.0F, y, 56.0F, row.height}, "SURFACE",
                  row_theme.text_muted);
        if (row_flag_toggle(ctx, mix(id_, "surface lock", row.node.value()),
                            lock_bounds, RowFlag::Locked, row.locked,
                            row_hovered)) {
          frame.element_flags.push_back(TimelineElementFlagEdit{
              row.node, TimelineElementFlag::Locked, !row.locked});
        }
      } else if (has_toggles) {
        const bool hidden = !row.element_visible;
        if (row_flag_toggle(ctx, mix(id_, "element mute", row.node.value()),
                            eye_bounds, RowFlag::Visible, row.element_visible,
                            row_hovered)) {
          frame.element_flags.push_back(TimelineElementFlagEdit{
              row.node, TimelineElementFlag::Visible, hidden});
        }
        if (row_flag_toggle(ctx, mix(id_, "element lock", row.node.value()),
                            lock_bounds, RowFlag::Locked, row.locked,
                            row_hovered)) {
          frame.element_flags.push_back(TimelineElementFlagEdit{
              row.node, TimelineElementFlag::Locked, !row.locked});
        }
      }
    } else {
      // A property: named in the colour its keys and curve are drawn in, so
      // the list and the lanes read as one table.
      name_bounds = gui::Rect{content_x, y,
                              std::max(graph.x - content_x - 48.0F, 0.0F),
                              row.height};
      ctx.label(name_bounds, row.label,
                row.kind == Row::Kind::Component ||
                        row.kind == Row::Kind::Property
                    ? channel_color(row_channel_identity->property)
                    : row.color);
    }

    const float interaction_y = std::max(y, rows_bounds.y);
    const float interaction_bottom =
        std::min(y + row.height, rows_bounds.y + rows_bounds.height);
    const float interaction_height =
        std::max(interaction_bottom - interaction_y, 0.0F);

    if (row_channel_identity.has_value()) {
      const bool channel_hidden =
          hidden_channels_.contains(*row_channel_identity);
      const gui::Rect visibility_bounds{
          graph.x - 41.0F,
          interaction_y + std::max((interaction_height - 14.0F) * 0.5F, 0.0F),
          14.0F, std::min(interaction_height, 14.0F)};
      const gui::Interaction visibility = ctx.grab(
          mix(id_, "channel visibility", static_cast<core::u64>(index)),
          visibility_bounds);
      ctx.panel(visibility_bounds,
                channel_hidden ? alpha(ctx.theme().track, 0.82F)
                               : channel_color(row_channel_identity->property));
      if (channel_hidden) {
        ctx.draw_list().add_line(visibility_bounds.x, visibility_bounds.y,
                                 visibility_bounds.x + visibility_bounds.width,
                                 visibility_bounds.y + visibility_bounds.height,
                                 1.0F, ctx.theme().warning);
      }
      if (visibility.clicked) {
        active_channel_ = *row_channel_identity;
        if (channel_hidden) {
          hidden_channels_.erase(*row_channel_identity);
        } else {
          hidden_channels_.insert(*row_channel_identity);
          keys_.selected.erase(
              std::remove_if(keys_.selected.begin(), keys_.selected.end(),
                             [&](const SelectedKey &key) {
                               return key.target ==
                                          row_channel_identity->target &&
                                      key.component ==
                                          row_channel_identity->property;
                             }),
              keys_.selected.end());
          keys_.drag_keys.erase(
              std::remove_if(keys_.drag_keys.begin(), keys_.drag_keys.end(),
                             [&](const SelectedKey &key) {
                               return key.target ==
                                          row_channel_identity->target &&
                                      key.component ==
                                          row_channel_identity->property;
                             }),
              keys_.drag_keys.end());
          if (keys_.drag_keys.empty()) {
            keys_.dragging = false;
            keys_.duplicate = false;
          }
        }
      }
    }

    // Property-local key controls: the group diamond keys all three axes;
    // a component diamond keys only that channel. A filled diamond means
    // there is already a key at the playhead, while a hollow bright one
    // means the channel is animated elsewhere.
    if (!row.locked && (row.kind == Row::Kind::Group ||
                        row.kind == Row::Kind::Component ||
                        row.kind == Row::Kind::Property)) {
      const ElementInfo *const row_element = find_element(elements, row.node);
      const output::ProjectionSurface *const row_surface =
          row.surface ? find_surface(surfaces, row.node) : nullptr;
      if ((row_surface != nullptr) || row_element != nullptr) {
        bool animated = false;
        bool keyed_here = true;
        const core::i64 show_frame = signed_frame(timeline.show_frame);
        const core::u8 first_component =
            row.kind == Row::Kind::Property
                ? static_cast<core::u8>(row.property)
                : static_cast<core::u8>(group_channel(
                      row.group,
                      row.kind == Row::Kind::Component ? row.component : 0U));
        const core::u8 component_count =
            row.kind == Row::Kind::Group ? row.component_count : 1U;
        const doc::NodeId key_target =
            row.kind == Row::Kind::Property ? row.property_target : row.node;
        for (core::u8 offset = 0U; offset < component_count; ++offset) {
          const auto component =
              static_cast<scene::TransformChannel>(first_component + offset);
          const scene::ChannelKeyframes *const authored =
              channel_for(keyframes, key_target, component);
          animated =
              animated || (authored != nullptr && !authored->frames.empty());
          keyed_here =
              keyed_here && authored != nullptr &&
              std::find(authored->frames.begin(), authored->frames.end(),
                        show_frame) != authored->frames.end();
        }

        const gui::Rect key_bounds{
            graph.x - 22.0F,
            interaction_y + std::max((interaction_height - 18.0F) * 0.5F, 0.0F),
            18.0F, std::min(interaction_height, 18.0F)};
        if (key_button(ctx, mix(id_, "row key", static_cast<core::u64>(index)),
                       key_bounds, animated, keyed_here, background)) {
          for (core::u8 offset = 0U; offset < component_count; ++offset) {
            const auto component =
                static_cast<scene::TransformChannel>(first_component + offset);
            frame.keys.push_back(TimelineKeyEdit{
                key_target, component, timeline.show_frame,
                row.kind == Row::Kind::Property
                    ? row.property_value
                    : (row_surface != nullptr
                           ? surface_component_value(*row_surface, component)
                           : component_value(*row_element, component)),
                false, keyed_here});
          }
        }
      }
    }

    // A header being renamed gives its width to the text field; a header that
    // is merely selected stays fully grabbable, because selecting a track is
    // now the ordinary thing a click does to it.
    const float label_width =
        rename_.editing(row.node.value())
            ? std::max(name_x - rows_bounds.x - 4.0F, 0.0F)
            : list_width;
    const gui::Interaction label_it =
        ctx.grab(mix(id_, "row", static_cast<core::u64>(index)),
                 gui::Rect{rows_bounds.x, interaction_y, label_width,
                           interaction_height});
    if (label_it.pressed && row.kind == Row::Kind::LaneHeader && !row.locked) {
      reorder_.lane_node = row.node;
      capture_layer_drag(p, row.node);
      reorder_.lane_start_x = ctx.pointer_press_x();
      reorder_.lane_start_y = ctx.pointer_press_y();
      reorder_.lane_moved = false;
    } else if (label_it.pressed && row.kind == Row::Kind::Layer &&
               !row.surface &&
               !(row.expandable && ctx.pointer_press_x() < content_x)) {
      const ElementInfo *const row_element = find_element(elements, row.node);
      if (row_element != nullptr && orderable_layer(*row_element) &&
          !row.locked) {
        keys_.selected.clear();
        keys_.drag_keys.clear();
        keys_.dragging = false;
        keys_.duplicate = false;
        reorder_.layer_node = row.node;
        const bool preserve_selection = selected(row.node, selection);
        capture_layer_drag(p, row.node);
        reorder_.layer_start_x = ctx.pointer_press_x();
        reorder_.layer_start_y = ctx.pointer_press_y();
        reorder_.layer_moved = false;
        keys_.marquee_tracking = false;
        keys_.marquee_active = false;
        keys_.lasso_points.clear();
        // Keep a selected group intact until a click is released. Modifier
        // clicks also select on release, so Ctrl toggles exactly once.
        if (!preserve_selection && !ctx.ctrl_held() && !ctx.shift_held()) {
          TimelineSelectionRequest next{};
          next.node = row.node;
          next.action = SelectionAction::Replace;
          frame.selection = std::move(next);
        }
      }
    }
    const bool dragged_header_click =
        row.kind == Row::Kind::LaneHeader && row.node == reorder_.lane_node &&
        reorder_.lane_moved;
    const bool dragged_layer_click =
        row.kind == Row::Kind::Layer && row.node == scratch.released_layer_drag_node &&
        scratch.released_layer_drag_moved;
    if (label_it.clicked && !dragged_header_click && !dragged_layer_click) {
      if (row.kind == Row::Kind::LaneHeader &&
          !(row.expandable && ctx.pointer_x() < content_x)) {
        selected_lane_ = row.node;
        reorder_.lane_destination = doc::NodeId{};
        keys_.selected.clear();
        keys_.drag_keys.clear();
        keys_.dragging = false;
        keys_.duplicate = false;
        // A track is a screen, so selecting one has to reach the Inspector
        // like selecting anything else does. Without this the header would
        // highlight and the Inspector would keep showing whatever was
        // selected before, which is the shape of a control that does nothing.
        if (!row.locked) {
          TimelineSelectionRequest next{};
          next.node = row.node;
          next.action =
              selection_action(ctx.ctrl_held(), ctx.shift_held(), true);
          if (next.action == SelectionAction::Range) {
            next.visible_order = visible_order;
          }
          frame.selection = std::move(next);
        }
        // A second click on the same name opens it for editing, focused
        // with the whole name selected so typing replaces it.
        if (!row.locked && !ctx.ctrl_held() && !ctx.shift_held() &&
            name_bounds.contains(ctx.pointer_x(), ctx.pointer_y()) &&
            clicks_.double_click(row.node.value(), ctx.time_seconds())) {
          rename_.begin(ctx, row.node.value(), row.label);
        }
        continue;
      }
      const bool on_twirl = row.expandable && ctx.pointer_x() < content_x;
      if (on_twirl) {
        if (row.hierarchy) {
          if (open) {
            rows_state_.expanded_hierarchies.erase(row.node);
          } else {
            rows_state_.expanded_hierarchies.insert(row.node);
          }
        } else if (row.kind == Row::Kind::Layer ||
                   row.kind == Row::Kind::PropertiesHeader) {
          if (open) {
            rows_state_.expanded_layers.erase(row.node.value());
          } else {
            rows_state_.expanded_layers.insert(row.node.value());
          }
        } else if (open) {
          rows_state_.expanded_groups.erase(row_group);
        } else {
          rows_state_.expanded_groups.insert(row_group);
        }
      } else if (!row.locked) {
        // A layer-row click leaves the key-selection domain. This
        // keeps a later Ctrl+C/Ctrl+V aimed at the newly selected
        // clip/layer rather than at stale Timeline diamonds.
        keys_.selected.clear();
        keys_.drag_keys.clear();
        keys_.dragging = false;
        keys_.duplicate = false;
        TimelineSelectionRequest next{};
        next.node = row.node;
        next.action =
            selection_action(ctx.ctrl_held(), ctx.shift_held(), !row.surface);
        if (next.action == SelectionAction::Range) {
          next.visible_order = visible_order;
        }
        next.surface = row.surface;
        const bool plain_click = next.action == SelectionAction::Replace;
        frame.selection = std::move(next);
        // A second click on a layer's name renames it, as in the outliner.
        if (row.kind == Row::Kind::Layer && !row.surface && plain_click &&
            name_bounds.contains(ctx.pointer_x(), ctx.pointer_y()) &&
            clicks_.double_click(row.node.value(), ctx.time_seconds())) {
          const ElementInfo *const element = find_element(elements, row.node);
          if (element != nullptr && !element->linked &&
              element->kind != ElementKind::Environment) {
            rename_.begin(ctx, row.node.value(), row.label);
          }
        }
      }
    }

    // The time lane: a clip bar for a placed media layer, a translucent
    // authored-span bar for every other layer, aggregate diamonds for
    // groups, and draggable diamonds for component channels.
    ctx.push_clip(gui::Rect{graph.x, y, graph.width, row.height});
    // Filled in for a placed clip row, and consulted by the lane
    // interaction below so that a press on the bar becomes a clip gesture
    // rather than a scrub.
    const ElementInfo *clip_element = nullptr;
    scene::ClipTiming clip_timing{};
    gui::Rect clip_bar{};
    float clip_fade_in_x{};
    float clip_fade_out_x{};
    if (row.kind == Row::Kind::Layer && !row.surface) {
      const ElementInfo *const layer_element = find_element(elements, row.node);
      if (layer_element != nullptr &&
          layer_element->kind == ElementKind::Media &&
          layer_element->timing.placed()) {
        clip_element = layer_element;
        clip_timing =
            clip_.drag != ClipDrag::None && clip_.node == row.node
                ? clip_.proposed
                : (clip_.pending_edit.has_value() &&
                           clip_.pending_edit->clip == row.node
                       ? clip_.pending_edit->timing
                       : layer_element->timing);
      }
    }
    if (clip_element != nullptr) {
      const float x0 = frame_to_x(static_cast<double>(clip_timing.start));
      const float x1 = frame_to_x(
          static_cast<double>(clip_timing.start + clip_timing.duration));
      clip_bar =
          gui::Rect{x0, y + 3.0F, std::max(x1 - x0, 2.0F), row.height - 6.0F};
      const bool live =
          clip_.drag != ClipDrag::None && clip_.node == row.node;
      ctx.panel(clip_bar, alpha(row.color, live ? 0.42F : 0.30F));
      bool drew_filmstrip = false;
      if (const TimelineFilmstripObservation *const observation =
              filmstrip_for_clip(p, row.node);
          observation != nullptr &&
          observation->state == assets::DerivativeState::Ready &&
          observation->texture != nullptr) {
        float u0 = 0.0F;
        float u1 = 1.0F;
        bool looped = false;
        const scene::MediaFrameRate source_mapping_show_rate{
            timeline.rate_numerator, timeline.rate_denominator};
        if (clip_element->sequence_count > 1U &&
            clip_timing.source_in >= 0 && clip_timing.duration > 0) {
          const auto mapped = scene::source_ordinal_delta(
              clip_timing.duration - 1, source_mapping_show_rate,
              clip_element->source_frame_rate);
          const core::i64 maximum_source =
              static_cast<core::i64>(clip_element->sequence_count - 1U);
          if (mapped && mapped.value() >= 0 &&
              clip_timing.source_in <= maximum_source - mapped.value()) {
            const auto atlas_center = [maximum_source](core::i64 ordinal) {
              const double normalized = static_cast<double>(ordinal) /
                                        static_cast<double>(maximum_source);
              return static_cast<float>(
                  (normalized * 7.0 + 0.5) / 8.0);
            };
            u0 = atlas_center(clip_timing.source_in);
            u1 = atlas_center(clip_timing.source_in + mapped.value());
            constexpr float one_tile = 1.0F / 8.0F;
            if (u1 - u0 < one_tile) {
              const float midpoint = (u0 + u1) * 0.5F;
              u0 = std::clamp(midpoint - one_tile * 0.5F, 0.0F,
                              1.0F - one_tile);
              u1 = u0 + one_tile;
            }
          } else {
            looped = true;
          }
        }
        const gui::UiColor tint{1.0F, 1.0F, 1.0F, live ? 0.62F : 0.78F};
        if (looped) {
          // Long placements repeat the source. Sample the existing eight-thumb
          // atlas at visible timeline positions, with a fixed per-row budget.
          // Neither a very long placement nor many loops increases this work.
          const float begin = std::max(clip_bar.x, graph.x);
          const float end = std::min(clip_bar.x + clip_bar.width,
                                     graph.x + graph.width);
          if (end > begin) {
            const unsigned tiles = static_cast<unsigned>(std::clamp(
                std::ceil((end - begin) / 72.0F), 1.0F, 32.0F));
            const float width = (end - begin) / static_cast<float>(tiles);
            for (unsigned tile = 0U; tile < tiles; ++tile) {
              const float x = begin + static_cast<float>(tile) * width;
              const double offset = std::clamp(
                  x_to_frame(x + width * 0.5F) -
                      static_cast<double>(clip_timing.start),
                  0.0, static_cast<double>(clip_timing.duration - 1));
              const core::i64 show_offset =
                  offset >= static_cast<double>(clip_timing.duration - 1)
                      ? clip_timing.duration - 1
                      : static_cast<core::i64>(offset);
              const auto phase = scene::advance_source_phase(
                  clip_timing.source_in, show_offset,
                  source_mapping_show_rate, clip_element->source_frame_rate,
                  clip_element->sequence_count, scene::SourcePhaseRounding::Floor);
              if (!phase) {
                continue;
              }
              const auto thumbnail = static_cast<unsigned>(std::llround(
                  static_cast<double>(phase.value()) * 7.0 /
                  static_cast<double>(clip_element->sequence_count - 1U)));
              const float left_uv = static_cast<float>(thumbnail) / 8.0F;
              ctx.draw_list().add_image_region(
                  x, clip_bar.y, width, clip_bar.height, *observation->texture,
                  left_uv, 0.0F, left_uv + 0.125F, 1.0F, tint);
            }
          }
        } else {
          ctx.draw_list().add_image_region(
              clip_bar.x, clip_bar.y, clip_bar.width, clip_bar.height,
              *observation->texture, u0, 0.0F, u1, 1.0F, tint);
        }
        drew_filmstrip = true;
      }
      if (const TimelineWaveformObservation *const waveform =
              waveform_for_clip(p, row.node);
          waveform != nullptr &&
          waveform->state == assets::DerivativeState::Ready &&
          waveform->bins.size() == assets::waveform_peak_rms_bins &&
          clip_element->sequence_count > 0U && clip_timing.duration > 0) {
        const float begin = std::max(clip_bar.x, graph.x) + 1.0F;
        const float end =
            std::min(clip_bar.x + clip_bar.width, graph.x + graph.width) - 1.0F;
        const float half_height = std::max(clip_bar.height * 0.42F, 1.0F);
        const scene::MediaFrameRate show_frame_rate{timeline.rate_numerator,
                                                     timeline.rate_denominator};
        const core::i64 maximum_source = static_cast<core::i64>(
            std::min<core::u64>(
                clip_element->sequence_count - 1U,
                static_cast<core::u64>(std::numeric_limits<core::i64>::max())));
        for (float x = begin; x <= end; x += 2.0F) {
          const double offset = std::clamp(
              std::floor(x_to_frame(x)) - static_cast<double>(clip_timing.start),
              0.0, static_cast<double>(clip_timing.duration - 1));
          const core::i64 show_offset =
              offset >= static_cast<double>(clip_timing.duration - 1)
                  ? clip_timing.duration - 1
                  : static_cast<core::i64>(offset);
          const auto source_phase = scene::advance_source_phase(
              clip_timing.source_in, show_offset, show_frame_rate,
              clip_element->source_frame_rate, clip_element->sequence_count,
              scene::SourcePhaseRounding::Floor);
          if (!source_phase) {
            continue;
          }
          const core::i64 source_ordinal = source_phase.value();
          const double normalized =
              maximum_source > 0
                  ? static_cast<double>(source_ordinal) /
                        static_cast<double>(maximum_source)
                  : 0.0;
          const std::size_t bin = std::min<std::size_t>(
              static_cast<std::size_t>(std::floor(
                  normalized * static_cast<double>(waveform->bins.size()))),
              waveform->bins.size() - 1U);
          const assets::WaveformPeakRms sample = waveform->bins[bin];
          const float peak_height = sample.peak * half_height;
          const float rms_height = sample.rms * half_height;
          ctx.panel(gui::Rect{x, centre_y - rms_height, 1.0F,
                              std::max(rms_height * 2.0F, 1.0F)},
                    alpha(ctx.theme().keyframe,
                          drew_filmstrip ? 0.68F : 0.76F));
          ctx.panel(gui::Rect{x, centre_y - peak_height, 1.0F, 1.0F},
                    alpha(ctx.theme().text, 0.82F));
          ctx.panel(gui::Rect{x, centre_y + peak_height, 1.0F, 1.0F},
                    alpha(ctx.theme().text, 0.82F));
        }
      }
      ctx.panel(gui::Rect{clip_bar.x, clip_bar.y, clip_bar.width, 1.0F},
                alpha(row.color, 0.85F));
      ctx.panel(gui::Rect{clip_bar.x, clip_bar.y + clip_bar.height - 1.0F,
                          clip_bar.width, 1.0F},
                alpha(row.color, 0.85F));
      const float grab = edge_grab_width(clip_bar.width);
      ctx.panel(gui::Rect{clip_bar.x, clip_bar.y, grab, clip_bar.height},
                alpha(row.color, 0.68F));
      ctx.panel(gui::Rect{clip_bar.x + clip_bar.width - grab, clip_bar.y, grab,
                          clip_bar.height},
                alpha(row.color, 0.68F));

      // The ramps, drawn the way an editor draws them: a diagonal from
      // the bar's foot at the cut to its head where the clip is fully in.
      clip_fade_in_x = frame_to_x(
          static_cast<double>(clip_timing.start + clip_timing.fade_in));
      clip_fade_out_x = frame_to_x(static_cast<double>(
          clip_timing.start + clip_timing.duration - clip_timing.fade_out));
      const gui::UiColor ramp = alpha(ctx.theme().keyframe, 0.9F);
      if (clip_timing.fade_in > 0) {
        ctx.draw_list().add_line(clip_bar.x, clip_bar.y + clip_bar.height,
                                 clip_fade_in_x, clip_bar.y, 1.0F, ramp);
      }
      if (clip_timing.fade_out > 0) {
        ctx.draw_list().add_line(clip_fade_out_x, clip_bar.y,
                                 clip_bar.x + clip_bar.width,
                                 clip_bar.y + clip_bar.height, 1.0F, ramp);
      }
      // The fade handles ride the bar's top edge as round knobs. They grow
      // and light under the pointer, and answer a press several units away,
      // so a crossfade can be pulled without hunting for a pixel.
      const auto fade_handle = [&](float handle_x, bool engaged) {
        const bool near =
            edit_tool_ == TimelineEditTool::Standard &&
            !ctx.pointer_blocked() && clip_.drag == ClipDrag::None &&
            std::fabs(ctx.pointer_x() - handle_x) <= fade_handle_reach &&
            ctx.pointer_y() >= clip_bar.y - 4.0F &&
            ctx.pointer_y() <= clip_bar.y + fade_handle_reach;
        const bool lit = engaged || near;
        draw_filled_circle(ctx, handle_x, clip_bar.y + 1.0F,
                           lit ? 6.0F : 4.5F,
                           alpha(ctx.theme().background, 0.85F));
        draw_filled_circle(ctx, handle_x, clip_bar.y + 1.0F,
                           lit ? 4.5F : 3.0F,
                           lit ? ctx.theme().accent : ramp);
        if (near) {
          ctx.set_cursor(gui::Cursor::ResizeHorizontal);
          ctx.tooltip(gui::Rect{handle_x - fade_handle_reach, clip_bar.y - 4.0F,
                                fade_handle_reach * 2.0F,
                                fade_handle_reach + 4.0F},
                      "Drag to set the fade");
        }
      };
      fade_handle(clip_fade_in_x, clip_.drag == ClipDrag::FadeIn &&
                                      clip_.node == row.node);
      fade_handle(clip_fade_out_x, clip_.drag == ClipDrag::FadeOut &&
                                       clip_.node == row.node);

      // Every editor names its clips on the bar. The name rides on a dark
      // backing so it reads over any filmstrip, and stays pinned to the
      // visible start of a bar scrolled half off the left edge.
      const float name_left =
          std::max(clip_bar.x, graph.x) + 6.0F + edge_grab_width(clip_bar.width);
      const float name_right = clip_bar.x + clip_bar.width - 6.0F;
      if (name_right - name_left >= 24.0F) {
        std::string name = row.label;
        if (clip_.drag == ClipDrag::Slip && clip_.node == row.node) {
          name += "  Source " + std::to_string(clip_timing.source_in);
        }
        if (!clip_element->on_air) {
          name += "  off air";
        }
        const float text_width =
            std::min(ctx.font_width(name) + 8.0F, name_right - name_left);
        ctx.panel(gui::Rect{name_left - 4.0F, clip_bar.y + 2.0F, text_width,
                            clip_bar.height - 4.0F},
                  alpha(ctx.theme().background, 0.55F));
        ctx.label(gui::Rect{name_left, clip_bar.y, name_right - name_left,
                            clip_bar.height},
                  name,
                  clip_element->on_air ? ctx.theme().text
                                       : ctx.theme().text_muted);
      }
      if (selected(row.node, selection)) {
        draw_border(ctx, clip_bar, ctx.theme().text, 1.5F);
      }
      // What a press here would do, shown before it is done: the trim edge
      // under a Selection pointer lights, and the razor draws its cut.
      if (!row.locked &&
          clip_bar.contains(ctx.pointer_x(), ctx.pointer_y()) &&
          clip_.drag == ClipDrag::None && !ctx.pointer_blocked()) {
        const float px = ctx.pointer_x();
        if (edit_tool_ == TimelineEditTool::Razor) {
          const float cut_x =
              frame_to_x(static_cast<double>(razor_frame(p, px)));
          ctx.panel(gui::Rect{cut_x - 0.5F, clip_bar.y - 2.0F, 1.5F,
                              clip_bar.height + 4.0F},
                    ctx.theme().warning);
        } else if (edit_tool_ == TimelineEditTool::Standard) {
          const bool on_fade =
              ctx.pointer_y() <= clip_bar.y + fade_handle_reach &&
              (std::fabs(px - clip_fade_in_x) <= fade_handle_reach ||
               std::fabs(px - clip_fade_out_x) <= fade_handle_reach);
          if (on_fade) {
            // The handle itself shows the hover.
          } else if (px <= clip_bar.x + grab) {
            ctx.panel(gui::Rect{clip_bar.x, clip_bar.y, grab, clip_bar.height},
                      row.color);
            ctx.set_cursor(gui::Cursor::ResizeHorizontal);
          } else if (px >= clip_bar.x + clip_bar.width - grab) {
            ctx.panel(gui::Rect{clip_bar.x + clip_bar.width - grab,
                                clip_bar.y, grab, clip_bar.height},
                      row.color);
            ctx.set_cursor(gui::Cursor::ResizeHorizontal);
          } else {
            ctx.set_cursor(gui::Cursor::Move);
          }
        }
      }
    } else if (row.kind == Row::Kind::Layer) {
      // A layer with no placement is present for the whole show: a quiet
      // band in its colour, so it reads as "always on" rather than as a clip.
      const float x0 = frame_to_x(static_cast<double>(first_i));
      const float x1 = frame_to_x(static_cast<double>(last_i));
      const gui::Rect clip{x0, y + 5.0F, std::max(x1 - x0, 1.0F),
                           row.height - 10.0F};
      ctx.panel(clip, alpha(row.color, row.element_visible ? 0.16F : 0.06F));
      ctx.panel(gui::Rect{clip.x, clip.y, clip.width, 1.0F},
                alpha(row.color, row.element_visible ? 0.45F : 0.18F));
      if (row.element_kind == ElementKind::Media) {
        const float begin = std::max(clip.x, graph.x);
        const float end = std::min(clip.x + clip.width, graph.x + graph.width);
        std::size_t sample = 0U;
        for (float x = begin + 2.0F; x < end - 1.0F; x += 5.0F) {
          const core::u64 pattern =
              row.node.value() * 17U + sample * 29U + sample * sample;
          const float amplitude = 2.0F + static_cast<float>(pattern % 7U);
          ctx.panel(gui::Rect{x, centre_y - amplitude * 0.5F, 1.0F, amplitude},
                    alpha(row.color, 0.78F));
          ++sample;
        }
      }
    }

    const auto draw_frames = [&](const std::vector<core::i64> *frames,
                                 gui::UiColor color) {
      if (frames == nullptr) {
        return;
      }
      for (const core::i64 key : *frames) {
        draw_diamond(ctx, frame_to_x(static_cast<double>(key)), centre_y,
                     key_radius, color);
      }
    };

    const scene::ChannelKeyframes *component_channel = nullptr;
    const std::vector<core::i64> *component_frames = nullptr;
    std::vector<float> component_x;
    std::vector<float> component_y;
    scene::TransformChannel channel{};
    bool component_visible = true;
    gui::UiColor component_color = ctx.theme().keyframe;
    if (row.kind == Row::Kind::Component || row.kind == Row::Kind::Property) {
      channel = row_channel_identity->property;
      const doc::NodeId channel_target = row_channel_identity->target;
      component_visible = !hidden_channels_.contains(*row_channel_identity);
      component_color = channel_color(channel);
      component_channel = component_visible
                              ? channel_for(keyframes, channel_target, channel)
                              : nullptr;
      component_frames =
          component_channel != nullptr ? &component_channel->frames : nullptr;
      if (component_frames != nullptr) {
        component_x.reserve(component_frames->size());
        component_y.reserve(component_frames->size());

        double minimum_value = std::numeric_limits<double>::infinity();
        double maximum_value = -std::numeric_limits<double>::infinity();
        const bool has_values = component_channel->values.size() ==
                                component_channel->frames.size();
        if (graph_mode_ && has_values) {
          for (const double value : component_channel->values) {
            if (std::isfinite(value)) {
              minimum_value = std::min(minimum_value, value);
              maximum_value = std::max(maximum_value, value);
            }
          }
        }
        const double value_span = maximum_value - minimum_value;
        const auto value_y = [&](std::size_t key_index) noexcept {
          if (!graph_mode_ || !has_values || !std::isfinite(value_span) ||
              value_span <= 1.0e-12) {
            return centre_y;
          }
          const double value = component_channel->values[key_index];
          if (!std::isfinite(value)) {
            return centre_y;
          }
          const double normal = 0.5 +
                                (((maximum_value - value) / value_span) - 0.5) *
                                    static_cast<double>(curve_value_zoom_) +
                                static_cast<double>(curve_value_pan_);
          return y + 4.0F + static_cast<float>(normal) * (row.height - 8.0F);
        };

        if (graph_mode_ && has_values && std::isfinite(value_span) &&
            value_span > 1.0e-12) {
          ctx.draw_list().add_line(graph.x, centre_y, graph.x + graph.width,
                                   centre_y, 1.0F,
                                   alpha(component_color, 0.18F));
          std::array<char, 32U> maximum_label{};
          std::array<char, 32U> minimum_label{};
          const int maximum_length =
              std::snprintf(maximum_label.data(), maximum_label.size(), "%.3g",
                            maximum_value);
          const int minimum_length =
              std::snprintf(minimum_label.data(), minimum_label.size(), "%.3g",
                            minimum_value);
          ctx.label(gui::Rect{graph.x + 3.0F, y, std::min(graph.width, 52.0F),
                              row.height * 0.5F},
                    std::string_view{
                        maximum_label.data(),
                        static_cast<std::size_t>(std::max(maximum_length, 0))},
                    alpha(component_color, 0.68F));
          ctx.label(gui::Rect{graph.x + 3.0F, centre_y,
                              std::min(graph.width, 52.0F), row.height * 0.5F},
                    std::string_view{
                        minimum_label.data(),
                        static_cast<std::size_t>(std::max(minimum_length, 0))},
                    alpha(component_color, 0.68F));
        }

        float previous_x = 0.0F;
        float previous_y = centre_y;
        bool have_previous = false;
        for (std::size_t key_index = 0U; key_index < component_frames->size();
             ++key_index) {
          const core::i64 key_frame = (*component_frames)[key_index];
          const SelectedKey identity{channel_target, channel, key_frame};
          const auto moving =
              std::find(keys_.drag_keys.begin(), keys_.drag_keys.end(), identity);
          const bool moving_key = keys_.dragging && moving != keys_.drag_keys.end();
          const core::i64 shown_frame = moving_key && !keys_.duplicate
                                            ? key_frame + keys_.drag_delta
                                            : key_frame;
          const float key_x = frame_to_x(static_cast<double>(shown_frame));
          const float key_y = value_y(key_index);
          if (graph_mode_ && have_previous) {
            const auto interpolation =
                component_channel->interpolations.size() ==
                        component_channel->frames.size()
                    ? component_channel->interpolations[key_index - 1U]
                    : scene::KeyInterpolation::Linear;
            if (interpolation == scene::KeyInterpolation::Hold) {
              ctx.draw_list().add_line(previous_x, previous_y, key_x,
                                       previous_y, 1.0F,
                                       alpha(component_color, 0.72F));
              ctx.draw_list().add_line(key_x, previous_y, key_x, key_y, 1.0F,
                                       alpha(component_color, 0.72F));
            } else if (interpolation == scene::KeyInterpolation::Cubic) {
              float curve_x = previous_x;
              float curve_y = previous_y;
              constexpr std::size_t samples = 8U;
              for (std::size_t sample = 1U; sample <= samples; ++sample) {
                const float t =
                    static_cast<float>(sample) / static_cast<float>(samples);
                const float eased = t * t * (3.0F - 2.0F * t);
                const float next_x = previous_x + (key_x - previous_x) * t;
                const float next_y = previous_y + (key_y - previous_y) * eased;
                ctx.draw_list().add_line(curve_x, curve_y, next_x, next_y, 1.0F,
                                         alpha(component_color, 0.72F));
                curve_x = next_x;
                curve_y = next_y;
              }
            } else {
              ctx.draw_list().add_line(previous_x, previous_y, key_x, key_y,
                                       1.0F, alpha(component_color, 0.72F));
            }
          }
          have_previous = true;
          previous_x = key_x;
          previous_y = key_y;
          component_x.push_back(key_x);
          component_y.push_back(key_y);

          const bool is_key_selected = key_selected(identity);
          if (is_key_selected) {
            draw_diamond(ctx, key_x, key_y, key_radius + 2.0F,
                         alpha(ctx.theme().accent, 0.50F));
          }
          draw_diamond(ctx, key_x, key_y,
                       is_key_selected ? key_radius + 0.5F : key_radius,
                       is_key_selected ? ctx.theme().playhead
                                       : component_color);
          if (moving_key && keys_.duplicate && keys_.drag_delta != 0) {
            const float duplicate_x =
                frame_to_x(static_cast<double>(key_frame + keys_.drag_delta));
            draw_diamond(ctx, duplicate_x, key_y, key_radius + 0.5F,
                         ctx.theme().accent);
          }
          if (!row.locked) {
            scratch.visible_keys.push_back(FrameScratch::VisibleKeyPoint{identity, key_x, key_y});
          }
        }
      }
    } else if (row.kind == Row::Kind::Group) {
      for (core::u8 component = 0U; component < row.component_count;
           ++component) {
        const auto aggregate = group_channel(row.group, component);
        if (hidden_channels_.contains(ChannelIdentity{row.node, aggregate})) {
          continue;
        }
        draw_frames(frames_for(keyframes, row.node, aggregate),
                    alpha(channel_color(aggregate), 0.58F));
      }
    }

    const gui::Interaction lane_it = ctx.grab(
        mix(id_, "lane", static_cast<core::u64>(index)),
        gui::Rect{graph.x, interaction_y, graph.width, interaction_height});
    if (ctx.secondary_pointer_pressed() && interaction_height > 0.0F &&
        ctx.pointer_y() >= interaction_y &&
        ctx.pointer_y() < interaction_y + interaction_height &&
        ctx.pointer_x() >= rows_bounds.x &&
        ctx.pointer_x() < rows_bounds.x + rows_bounds.width) {
      scratch.right_row = index;
    }
    // The razor cuts where it lands and starts no drag; Shift cuts every
    // unlocked clip under that frame, on every track.
    // A native event batch may already contain motion or release after the
    // down edge. Classify the gesture where it began, not at that endpoint.
    const float press_x = ctx.pointer_press_x();
    const float press_y = ctx.pointer_press_y();
    const bool razor_press = !row.locked && lane_it.pressed &&
                             clip_element != nullptr &&
                             edit_tool_ == TimelineEditTool::Razor &&
                             clip_bar.contains(press_x, press_y);
    if (razor_press) {
      clip_.press_consumed = true;
      const core::i64 cut = razor_frame(p, press_x);
      for (const ElementInfo &element : elements) {
        if (!editable_clip(element) ||
            (element.node != row.node && !ctx.shift_held())) {
          continue;
        }
        if (cut > element.timing.start && cut <= element.timing.last_frame()) {
          frame.clip_splits.push_back(
              TimelineClipSplit{element.node, cut});
        }
      }
    } else if (!row.locked && lane_it.pressed && clip_element != nullptr &&
               (clip_bar.contains(press_x, press_y) ||
                (edit_tool_ == TimelineEditTool::Standard &&
                 press_y >= clip_bar.y - 4.0F &&
                 press_y <= clip_bar.y + fade_handle_reach &&
                 (std::fabs(press_x - clip_fade_in_x) <= fade_handle_reach ||
                  std::fabs(press_x - clip_fade_out_x) <= fade_handle_reach)))) {
      clip_.press_consumed = true;
      keys_.selected.clear();
      keys_.drag_keys.clear();
      keys_.dragging = false;
      keys_.duplicate = false;
      markers_.selected.clear();
      const float grab = edge_grab_width(clip_bar.width);
      const float px = press_x;
      const float midpoint = clip_bar.x + clip_bar.width * 0.5F;
      clip_.peer = doc::NodeId{};
      doc::NodeId dragged = row.node;
      scene::ClipTiming dragged_timing = clip_timing;
      if (edit_tool_ == TimelineEditTool::RippleTrim) {
        clip_.drag =
            px < midpoint ? ClipDrag::RippleTrimHead : ClipDrag::RippleTrimTail;
      } else if (edit_tool_ == TimelineEditTool::Roll) {
        // Roll the cut nearest the press: the clip butted against this
        // one's head on the left half, its tail on the right half, falling
        // back to whichever neighbour exists.
        const auto neighbour = [&](bool before) -> const ElementInfo * {
          for (const ElementInfo &candidate : elements) {
            if (candidate.node == row.node || !editable_clip(candidate) ||
                candidate.timeline_lane != clip_element->timeline_lane) {
              continue;
            }
            const core::i64 candidate_end =
                candidate.timing.start + candidate.timing.duration;
            if (before ? candidate_end == clip_timing.start
                       : candidate.timing.start ==
                             clip_timing.start + clip_timing.duration) {
              return &candidate;
            }
          }
          return nullptr;
        };
        const ElementInfo *left_of = neighbour(true);
        const ElementInfo *right_of = neighbour(false);
        const bool head = px < midpoint;
        const ElementInfo *peer = head ? (left_of ? left_of : right_of)
                                       : (right_of ? right_of : left_of);
        if (peer != nullptr) {
          clip_.drag = ClipDrag::Roll;
          // The earlier clip leads, so the request reads left to right.
          const bool peer_first = peer->timing.start < clip_timing.start;
          dragged = peer_first ? peer->node : row.node;
          clip_.peer = peer_first ? row.node : peer->node;
          dragged_timing = peer_first ? peer->timing : clip_timing;
        } else {
          // Nothing butts against this clip, so there is no cut to roll.
          clip_.drag = ClipDrag::None;
          clip_.press_consumed = true;
        }
      } else if (edit_tool_ == TimelineEditTool::Slip) {
        clip_.drag = ClipDrag::Slip;
      } else if (edit_tool_ == TimelineEditTool::Slide) {
        clip_.drag = ClipDrag::Slide;
      } else {
        // Standard placement editing. Fade handles sit on the bar's
        // top edge and take precedence over the trim strips.
        const bool on_top = press_y <= clip_bar.y + fade_handle_reach;
        const float to_in = std::fabs(px - clip_fade_in_x);
        const float to_out = std::fabs(px - clip_fade_out_x);
        if (on_top && to_in <= fade_handle_reach && to_in <= to_out) {
          clip_.drag = ClipDrag::FadeIn;
        } else if (on_top && to_out <= fade_handle_reach) {
          clip_.drag = ClipDrag::FadeOut;
        } else if (px <= clip_bar.x + grab) {
          clip_.drag = ClipDrag::TrimIn;
        } else if (px >= clip_bar.x + clip_bar.width - grab) {
          clip_.drag = ClipDrag::TrimOut;
        } else {
          clip_.drag = ClipDrag::Move;
        }
      }
      clip_.node = clip_.drag != ClipDrag::None ? dragged : doc::NodeId{};
      clip_.origin = dragged_timing;
      clip_.proposed = dragged_timing;
      clip_.status.clear();
      clip_.status_error = false;
      constexpr core::i64 edit_limit = std::numeric_limits<core::i64>::max() / 4;
      if (dragged_timing.start < -edit_limit || dragged_timing.start > edit_limit ||
          dragged_timing.duration > edit_limit || dragged_timing.source_in > edit_limit) {
        // Imported documents can carry a wider range than the interactive
        // workspace. Refuse arithmetic beyond that range rather than wrap it.
        clip_.drag = ClipDrag::None;
        clip_.node = doc::NodeId{};
        clip_.status = "EDIT UNAVAILABLE  /  Clip timing exceeds the editable range";
        clip_.status_error = true;
        clip_.status_until = ctx.time_seconds() + 3.0;
      }
      clip_.anchor = static_cast<core::i64>(std::llround(x_to_frame(px)));
      clip_.delta = 0;
      clip_.start_x = press_x;
      clip_.start_y = press_y;
      // A whole gesture can be captured after the proposal phase above.
      // Preserve its up position until the next draw resolves it, even if
      // later motion in this batch or the next one moves the free pointer.
      clip_.release = lane_it.released
          ? std::optional{gui::UiPoint{ctx.pointer_release_x(),
                                       ctx.pointer_release_y()}}
          : std::nullopt;
      clip_.moved = false;
      clip_.drop_lane.reset();
      clip_.snap_guide.reset();
      keys_.marquee_tracking = false;
      keys_.marquee_active = false;
      keys_.lasso_points.clear();
      TimelineSelectionRequest next{};
      next.node = row.node;
      next.action = selection_action(ctx.ctrl_held(), ctx.shift_held(), true);
      if (next.action == SelectionAction::Range) {
        next.visible_order = visible_order;
      }
      frame.selection = std::move(next);
    } else if (lane_it.pressed) {
      bool hit_key = false;
      if ((row.kind == Row::Kind::Component ||
           row.kind == Row::Kind::Property) &&
          component_frames != nullptr) {
        for (std::size_t key_index = 0U; key_index < component_frames->size();
             ++key_index) {
          const core::i64 key_frame = (*component_frames)[key_index];
          const float dx = component_x[key_index] - press_x;
          const float dy = component_y[key_index] - press_y;
          if (dx * dx + dy * dy <= key_hit_radius * key_hit_radius) {
            const SelectedKey hit{row.kind == Row::Kind::Property
                                      ? row.property_target
                                      : row.node,
                                  channel, key_frame};
            active_channel_ = ChannelIdentity{hit.target, hit.component};
            bool begin_drag = true;
            const auto selected_it =
                std::find(keys_.selected.begin(), keys_.selected.end(), hit);
            if (ctx.ctrl_held()) {
              if (selected_it != keys_.selected.end()) {
                keys_.selected.erase(selected_it);
                begin_drag = false;
              } else {
                keys_.selected.push_back(hit);
              }
            } else if (ctx.shift_held()) {
              if (selected_it == keys_.selected.end()) {
                keys_.selected.push_back(hit);
              }
            } else if (selected_it == keys_.selected.end()) {
              keys_.selected.assign(1U, hit);
            }
            canonicalize_key_selection();

            if (begin_drag) {
              keys_.drag_keys.clear();
              for (const SelectedKey &selected_key : keys_.selected) {
                const auto *const authored = frames_for(
                    keyframes, selected_key.target, selected_key.component);
                if (authored != nullptr &&
                    std::find(authored->begin(), authored->end(),
                              selected_key.frame) != authored->end()) {
                  keys_.drag_keys.push_back(selected_key);
                }
              }
              if (keys_.drag_keys.empty()) {
                keys_.drag_keys.push_back(hit);
              }
              keys_.dragging = true;
              // Alt is latched at press: releasing the modifier
              // before the pointer does not turn a duplicate into
              // a destructive retime halfway through the gesture.
              keys_.duplicate = ctx.alt_held();
              keys_.drag_anchor_frame = key_frame;
              keys_.drag_delta = 0;
              keys_.marquee_tracking = false;
              keys_.marquee_active = false;
              keys_.lasso_points.clear();
            }
            hit_key = true;
            markers_.selected.clear();
            break;
          }
        }
      }
      if (!hit_key && !ctx.ctrl_held() && !ctx.shift_held()) {
        TimelineSelectionRequest clear{};
        clear.action = SelectionAction::Clear;
        frame.selection = std::move(clear);
      }
      if (!hit_key && !row.locked) {
        keys_.marquee_base = keys_.selected;
        keys_.marquee_shift = ctx.shift_held();
        keys_.marquee_ctrl = ctx.ctrl_held();
        if (!keys_.marquee_shift && !keys_.marquee_ctrl) {
          keys_.selected.clear();
        }
        keys_.marquee_tracking = true;
        keys_.marquee_active = false;
        keys_.marquee_start_x = press_x;
        keys_.marquee_start_y = press_y;
        keys_.lasso_points.clear();
        if (keys_.lasso_mode) {
          keys_.lasso_points.push_back(
              gui::UiPoint{keys_.marquee_start_x, keys_.marquee_start_y});
        }
        frame.seek_to_frame =
            rounded_frame(x_to_frame(ctx.pointer_x()), first, last);
      } else if (row.locked) {
        frame.seek_to_frame =
            rounded_frame(x_to_frame(ctx.pointer_x()), first, last);
      }
    }
    if (lane_it.held && !keys_.dragging && !keys_.marquee_active &&
        clip_.drag == ClipDrag::None && !clip_.press_consumed &&
        reorder_.layer_node == doc::NodeId{} &&
        reorder_.lane_node == doc::NodeId{}) {
      frame.seek_to_frame =
          rounded_frame(x_to_frame(ctx.pointer_x()), first, last);
    }
    ctx.pop_clip();
  }

  ctx.pop_clip();
  rename_.end_frame(ctx);

  // F2 renames the selected layer or track while the timeline has the
  // operator's attention.
  if (timeline_focused_ && !ctx.typing() && ctx.key_pressed(gui::Key::Rename)) {
    for (const Row &row : rows) {
      if (row.kind == Row::Kind::LaneHeader && row.node == selected_lane_ &&
          !row.locked) {
        rename_.begin(ctx, row.node.value(), row.label);
        break;
      }
      if (row.kind == Row::Kind::Layer && !row.surface && row.node == anchor &&
          !row.locked) {
        const ElementInfo *const element = find_element(elements, row.node);
        if (element != nullptr && !element->linked &&
            element->kind != ElementKind::Environment) {
          rename_.begin(ctx, row.node.value(), row.label);
        }
        break;
      }
    }
  }

}

void TimelinePanel::draw_time_overlays(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const gui::Rect &bounds = p.bounds;
  const EditorTimeline &timeline = p.input.timeline;
  const std::span<const scene::TimelineMarker> markers = p.input.markers;
  const gui::Rect &ruler = p.layout.ruler;
  const gui::Rect &strip = p.layout.strip;
  const gui::Rect &rows_bounds = p.layout.rows;
  const gui::Rect &graph = p.layout.graph;
  const auto frame_to_x = [&](double value) noexcept {
    return time_axis_.x(value, graph.x);
  };
  auto &content_height = p.content_height;
  auto &maximum_row_scroll = p.maximum_row_scroll;
  auto &shown_first = p.shown_first;
  auto &shown_last = p.shown_last;

  // Over the rows: what lies outside the In/Out range is dimmed rather than
  // hidden, cues and markers run their line down through the clips they are
  // timed against, then the snap guide and the playhead on top of all of it.
  const gui::Rect time_area{graph.x, ruler.y, graph.width,
                            ruler.height + strip.height + rows_bounds.height};
  const gui::Rect lanes_area{graph.x, strip.y, graph.width,
                             strip.height + rows_bounds.height};
  ctx.push_clip(lanes_area);
  {
    const float in_x = frame_to_x(static_cast<double>(shown_first));
    const float out_x = frame_to_x(static_cast<double>(shown_last) + 1.0);
    const gui::UiColor dim = alpha(ctx.theme().background, 0.45F);
    if (in_x > graph.x) {
      ctx.panel(gui::Rect{graph.x, lanes_area.y, in_x - graph.x,
                          lanes_area.height},
                dim);
    }
    if (out_x < graph.x + graph.width) {
      ctx.panel(gui::Rect{out_x, lanes_area.y,
                          graph.x + graph.width - out_x, lanes_area.height},
                dim);
    }
    for (const scene::TimelineMarker &marker : markers) {
      const float marker_x =
          frame_to_x(static_cast<double>(shown_marker_frame(marker)));
      if (marker_x < graph.x || marker_x > graph.x + graph.width) {
        continue;
      }
      const gui::UiColor color =
          timeline.held_cue == marker.id
              ? ctx.theme().playhead
              : timeline_marker_color(marker.color);
      ctx.panel(gui::Rect{marker_x, rows_bounds.y, 1.0F, rows_bounds.height},
                alpha(color, marker.is_cue() ? 0.45F : 0.22F));
    }
  }
  ctx.pop_clip();
  ctx.push_clip(time_area);
  const float playhead_x = frame_to_x(static_cast<double>(timeline.show_frame));
  const std::optional<core::i64> guide =
      keys_.dragging
          ? keys_.snap_guide
          : (clip_.drag != ClipDrag::None
                 ? clip_.snap_guide
                 : (markers_.drag_moved ? markers_.snap_guide
                                       : std::optional<core::i64>{}));
  if (guide.has_value()) {
    const float guide_x = frame_to_x(static_cast<double>(*guide));
    if (guide_x >= graph.x && guide_x <= graph.x + graph.width) {
      ctx.draw_list().add_line(guide_x, ruler.y, guide_x,
                               rows_bounds.y + rows_bounds.height, 1.0F,
                               alpha(ctx.theme().accent, 0.72F));
    }
  }
  if (playhead_x >= graph.x && playhead_x <= graph.x + graph.width) {
    ctx.draw_list().add_line(playhead_x, ruler.y, playhead_x,
                             rows_bounds.y + rows_bounds.height, 1.5F,
                             ctx.theme().playhead);
    // A shield-shaped head on the ruler: a handle the eye finds first.
    const std::array<gui::UiPoint, 5U> head{{
        {playhead_x - 5.5F, ruler.y + 1.0F},
        {playhead_x + 5.5F, ruler.y + 1.0F},
        {playhead_x + 5.5F, ruler.y + 9.0F},
        {playhead_x, ruler.y + 14.0F},
        {playhead_x - 5.5F, ruler.y + 9.0F},
    }};
    ctx.draw_list().add_convex_poly(head, ctx.theme().playhead);
  }
  ctx.pop_clip();

  // A slim non-interactive scroll thumb is enough here: wheel owns row
  // navigation, and the indicator makes a clipped layer stack obvious.
  if (maximum_row_scroll > 0.0F && rows_bounds.height > 0.0F) {
    const float fraction = rows_bounds.height / content_height;
    const float thumb_height = std::max(rows_bounds.height * fraction, 20.0F);
    const float travel = std::max(rows_bounds.height - thumb_height, 0.0F);
    const float position =
        maximum_row_scroll > 0.0F ? row_scroll_ / maximum_row_scroll : 0.0F;
    ctx.push_clip(rows_bounds);
    ctx.panel(gui::Rect{bounds.x + bounds.width - 4.0F,
                        rows_bounds.y + travel * position, 3.0F, thumb_height},
              ctx.theme().widget_hover);
    ctx.pop_clip();
  }

}

void TimelinePanel::close_pass(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  TimelinePanelFrame &frame = p.frame;
  FrameScratch &scratch = p.scratch;
  const gui::Rect &rows_bounds = p.layout.rows;
  const gui::Rect &graph = p.layout.graph;

  if (!ctx.pointer_down()) {
    clip_.press_consumed = false;
  }
  if (frame.seek_to_frame.has_value()) {
    requested_playhead_ = frame.seek_to_frame;
    // A seek the Director clamped or refused must not pin the keys to a
    // frame the playhead never reaches; half a second of frames is ample for
    // the round trip.
    requested_playhead_frames_ = 30U;
  }
  ctx.pop_clip();
  // Tool shapes apply to the editing surface, and stay engaged throughout a
  // captured clip gesture even when the pointer leaves the panel. Selection
  // keeps the more specific move, trim and fade feedback drawn above.
  if (!ctx.pointer_blocked() && !scratch.popup_pointer_blocked &&
      !scratch.key_popup_pointer_blocked &&
      (clip_.drag != ClipDrag::None ||
       (graph.contains(ctx.pointer_x(), ctx.pointer_y()) &&
        ctx.pointer_y() >= rows_bounds.y))) {
    if (edit_tool_ != TimelineEditTool::Standard) {
      ctx.set_cursor(tool_cursor(edit_tool_));
    } else if (clip_.drag != ClipDrag::None) {
      ctx.set_cursor(clip_.drag == ClipDrag::Move ? gui::Cursor::Move
                                               : gui::Cursor::ResizeHorizontal);
    }
  }
}

} // namespace mediaserver::editor
