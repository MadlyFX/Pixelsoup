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

TimelinePanel::TimelinePanel(gui::UiId id)
    : id_(id), rename_(id ^ 0x54494D454E414D45ULL),
      graph_(std::make_unique<timeline::TimelineGraphView>()) {}

TimelinePanel::~TimelinePanel() = default;

float TimelinePanel::curve_value_zoom() const noexcept {
  return graph_->zoom();
}

float TimelinePanel::curve_value_pan() const noexcept { return graph_->pan(); }

bool TimelinePanel::channel_visible(
    doc::NodeId target, scene::PropertyChannel property) const noexcept {
  return graph_->visible(ChannelIdentity{target, property});
}

bool TimelinePanel::selected(
    doc::NodeId node, std::span<const doc::NodeId> selection) const noexcept {
  return std::find(selection.begin(), selection.end(), node) != selection.end();
}

void TimelinePanel::clear_key_context() noexcept {
  keys_.selected.clear();
  handle_drag_ = HandleDrag{};
  keys_.dragging = false;
  keys_.duplicate = false;
  keys_.drag_keys.clear();
  keys_.drag_delta = 0;
  keys_.snap_guide.reset();
  key_tools_.open = false;
  key_tools_.scale_open = false;
  graph_->reset();
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
  // Rows sized alone belong to that project's clips; every clip row's
  // height is a view preference and stays.
  rows_state_.clip_row_heights.clear();
  row_resize_ = RowResize{};
  navigator_ = NavigatorState{};
  reveal_node_ = doc::NodeId{};
  reveal_scroll_ = false;
  reveal_until_ = 0.0;
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
  // The shortcut sheet is modal: nothing under it answers the pointer.
  if (shortcut_sheet_open_) {
    ctx.push_pointer_block();
  }
  prune_key_selection(p);
  navigate(p);
  request_derivatives(p);
  draw_toolbar(p);
  draw_ruler(p);
  draw_marker_strip(p);
  resize_clip_rows(p);
  track_key_drag(p);
  resolve_clip_gesture(p);
  resolve_reorder_targets(p);
  draw_rows(p);
  draw_navigator(p);
  resolve_track_drop(p);
  finish_marquee(p);
  draw_time_overlays(p);
  commit_key_drag(p);
  draw_key_tools(p);
  draw_rate_menu(p);
  draw_drag_feedback(p);
  handle_actions(p);
  draw_shortcut_sheet(p);
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


  // A row pointed at from another panel is listed: its track open, and
  // every group above it.
  if (reveal_scroll_) {
    if (const ElementInfo *const element = find_element(elements, reveal_node_)) {
      if (element->timeline_lane != doc::NodeId{}) {
        rows_state_.collapsed_lanes.erase(element->timeline_lane);
      }
      const ElementInfo *above = element;
      for (std::size_t depth = 0U; above != nullptr && depth < elements.size();
           ++depth) {
        const doc::NodeId parent = layer_parent(*above);
        if (parent == doc::NodeId{}) {
          break;
        }
        rows_state_.expanded_hierarchies.insert(parent);
        above = find_element(elements, parent);
      }
    }
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
      // holds the frame under it.
      const gui::Rect area = graph_area(p);
      timeline::ValueAxis &axis = graph_->axis();
      const float anchor_y = std::clamp(ctx.pointer_y(), area.y,
                                        area.y + area.height);
      axis.zoom_about(axis.value(anchor_y, area),
                      std::pow(1.15, static_cast<double>(ctx.wheel())));
    } else if (graph_mode_ && ctx.pointer_x() >= graph.x && ctx.ctrl_held()) {
      // A notch moves the values by a twelfth of the graph's height.
      const gui::Rect area = graph_area(p);
      graph_->axis().pan(ctx.wheel() * area.height / 12.0F, area);
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
      graph_->axis().pan(ctx.pointer_dy(), graph_area(p));
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
  // Scrolled to once, and only when it is not already in view.
  if (reveal_scroll_) {
    reveal_scroll_ = false;
    for (const Row &row : rows) {
      if ((row.kind == Row::Kind::Layer || row.kind == Row::Kind::LaneHeader) &&
          row.node == reveal_node_) {
        if (row.top < row_scroll_ ||
            row.top + row.height > row_scroll_ + rows_bounds.height) {
          row_scroll_ = std::clamp(
              row.top - (rows_bounds.height - row.height) * 0.5F, 0.0F,
              maximum_row_scroll);
        }
        break;
      }
    }
  }

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
  // The dimming runs up through the ruler too, so the played range reads
  // from the top of the panel as well as across the clips.
  ctx.push_clip(time_area);
  {
    const float in_x = frame_to_x(static_cast<double>(shown_first));
    const float out_x = frame_to_x(static_cast<double>(shown_last) + 1.0);
    const gui::UiColor dim = alpha(ctx.theme().background, 0.45F);
    if (in_x > graph.x) {
      ctx.panel(gui::Rect{graph.x, time_area.y, in_x - graph.x,
                          time_area.height},
                dim);
    }
    if (out_x < graph.x + graph.width) {
      ctx.panel(gui::Rect{out_x, time_area.y,
                          graph.x + graph.width - out_x, time_area.height},
                dim);
    }
  }
  ctx.pop_clip();
  ctx.push_clip(lanes_area);
  {
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
  if (playback_overlay_) {
    draw_playback_overlay(p);
  }
  if (playhead_x >= graph.x && playhead_x <= graph.x + graph.width) {
    // A flag on the ruler carrying the time it stands on, as Sequencer's
    // does: the eye finds the playhead and reads it in one place. It stays
    // whole at either edge of the lanes, its point still on the frame.
    const std::string label = timecode(p, timeline.show_frame);
    const float flag_width = ctx.font_width(label) + 10.0F;
    const float flag_height = std::max(ruler.height - 8.0F, 6.0F);
    const float flag_x =
        std::clamp(playhead_x - flag_width * 0.5F, graph.x,
                   std::max(graph.x, graph.x + graph.width - flag_width));
    const gui::Rect flag{flag_x, ruler.y + 1.0F, flag_width, flag_height};
    ctx.draw_list().add_line(playhead_x, flag.y + flag.height, playhead_x,
                             rows_bounds.y + rows_bounds.height, 1.5F,
                             ctx.theme().playhead);
    ctx.panel(flag, ctx.theme().playhead);
    const std::array<gui::UiPoint, 3U> point{{
        {playhead_x - 4.5F, flag.y + flag.height},
        {playhead_x + 4.5F, flag.y + flag.height},
        {playhead_x, flag.y + flag.height + 5.0F},
    }};
    ctx.draw_list().add_convex_poly(point, ctx.theme().playhead);
    ctx.label(gui::Rect{flag.x + 5.0F, flag.y, flag.width - 6.0F,
                        flag.height},
              label, ctx.theme().background);
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

void TimelinePanel::draw_playback_overlay(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const EditorTimeline &timeline = p.input.timeline;
  const gui::Rect &ruler = p.layout.ruler;
  const gui::Rect &strip = p.layout.strip;
  const gui::Rect &rows_bounds = p.layout.rows;
  const gui::Rect &graph = p.layout.graph;
  const gui::Theme &theme = ctx.theme();
  const auto frame_to_x = [&](double value) noexcept {
    return time_axis_.x(value, graph.x);
  };
  const float graph_right = graph.x + graph.width;

  // Each buffered run covers its frames edge to edge, so a single frame is
  // as wide as the frame is at this zoom - and never thinner than a pixel.
  for (const TimelineFrameRun &run : timeline.buffered) {
    const float left = std::max(frame_to_x(static_cast<double>(run.first)),
                                graph.x);
    const float right = std::min(
        frame_to_x(static_cast<double>(run.last) + 1.0), graph_right);
    if (right < graph.x || left > graph_right) {
      continue;
    }
    ctx.panel(gui::Rect{left, strip.y, std::max(right - left, 1.0F),
                        playback_cache_bar_height},
              theme.good);
  }

  // A node's playhead is thinner than the editor's and stops short of its
  // shield, so the two read apart where they nearly coincide. Its tag on
  // the ruler's lower edge carries the node's name and lag.
  for (const TimelineNodePlayhead &node : timeline.node_playheads) {
    const float node_x = frame_to_x(static_cast<double>(node.show_frame));
    if (node_x < graph.x || node_x > graph_right) {
      continue;
    }
    ctx.draw_list().add_line(node_x, ruler.y + ruler.height - 7.0F, node_x,
                             rows_bounds.y + rows_bounds.height, 1.0F,
                             alpha(theme.keyframe, 0.85F));
    const std::array<gui::UiPoint, 3U> tag{{
        {node_x - 4.0F, ruler.y + ruler.height - 7.0F},
        {node_x + 4.0F, ruler.y + ruler.height - 7.0F},
        {node_x, ruler.y + ruler.height - 1.0F},
    }};
    ctx.draw_list().add_convex_poly(tag, theme.keyframe);
    std::string tip = node.name.empty()
                          ? "Render node " + std::to_string(node.node_index)
                          : node.name;
    tip += " is presenting " + timecode(p, node.show_frame);
    if (node.latency_frames > 0U) {
      tip += ", " + std::to_string(node.latency_frames) +
             (node.latency_frames == 1U ? " frame" : " frames") +
             " behind what it has received";
    }
    ctx.tooltip(gui::Rect{node_x - 5.0F, ruler.y + ruler.height - 8.0F,
                          10.0F, 8.0F},
                tip);
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
