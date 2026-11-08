#include <mediaserver/editor/cues_panel.hpp>

#include <mediaserver/gui/rect_cut.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <system_error>

namespace mediaserver::editor {
namespace {

namespace gui = mediaserver::gui;
namespace scene = mediaserver::scene;

constexpr float header_height = 30.0F;
constexpr float go_band_height = 64.0F;
constexpr float columns_height = 20.0F;
constexpr float row_height = 26.0F;
constexpr float footer_height = 32.0F;
constexpr float timing_width = 52.0F;
constexpr double double_click_seconds = 0.35;
// A scrubbed WAIT or FADE moves a tenth of a second for every four pixels,
// a hundredth with Shift held.
constexpr float scrub_pixels_per_step = 4.0F;
constexpr double scrub_step_ms = 100.0;
constexpr double fine_scrub_step_ms = 10.0;
// How long a sent edit stands in for the document before the panel gives
// up on it: a few seconds of drawing, far past any healthy round trip.
constexpr core::u32 sent_marker_draws = 240U;

[[nodiscard]] gui::UiColor alpha(gui::UiColor color, float opacity) noexcept {
  color.a = opacity;
  return color;
}

[[nodiscard]] std::string_view action_word(scene::CueAction cue) noexcept {
  switch (cue) {
  case scene::CueAction::Hold:
    return "HOLD";
  case scene::CueAction::Loop:
    return "LOOP";
  case scene::CueAction::None:
    break;
  }
  return "MARK";
}

[[nodiscard]] std::string seconds_text(core::u64 frames,
                                       const EditorTimeline &timeline) {
  const double rate =
      static_cast<double>(timeline.rate_numerator) /
      static_cast<double>(std::max(timeline.rate_denominator, 1U));
  std::array<char, 32U> text{};
  const int length = std::snprintf(
      text.data(), text.size(), "%.1fs",
      rate > 0.0 ? static_cast<double>(frames) / rate : 0.0);
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] std::string_view trimmed(std::string_view text) noexcept {
  const auto first = text.find_first_not_of(" \t");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t");
  return text.substr(first, last - first + 1U);
}

[[nodiscard]] bool ends_with_unit(std::string_view text,
                                  std::string_view unit) noexcept {
  if (text.size() < unit.size()) {
    return false;
  }
  const std::string_view tail = text.substr(text.size() - unit.size());
  for (std::size_t index = 0U; index < unit.size(); ++index) {
    if (std::tolower(static_cast<unsigned char>(tail[index])) != unit[index]) {
      return false;
    }
  }
  return true;
}

} // namespace

std::optional<core::u32> parse_cue_seconds(std::string_view text,
                                           core::u32 maximum_ms) noexcept {
  text = trimmed(text);
  if (text.empty()) {
    return std::nullopt;
  }
  double scale = 1'000.0;
  if (ends_with_unit(text, "ms")) {
    scale = 1.0;
    text.remove_suffix(2U);
  } else if (ends_with_unit(text, "s")) {
    text.remove_suffix(1U);
  }
  text = trimmed(text);
  double value{};
  const char *const end = text.data() + text.size();
  const auto [stopped, error] = std::from_chars(text.data(), end, value);
  // `!(value >= 0)` also turns away a NaN.
  if (text.empty() || error != std::errc{} || stopped != end ||
      !(value >= 0.0)) {
    return std::nullopt;
  }
  const double milliseconds = std::round(value * scale);
  return milliseconds >= static_cast<double>(maximum_ms)
             ? maximum_ms
             : static_cast<core::u32>(milliseconds);
}

std::string format_cue_seconds(core::u32 milliseconds) {
  if (milliseconds == 0U) {
    return "-";
  }
  const char *const pattern = milliseconds % 100U == 0U  ? "%.1fs"
                              : milliseconds % 10U == 0U ? "%.2fs"
                                                         : "%.3fs";
  std::array<char, 32U> text{};
  const int length =
      std::snprintf(text.data(), text.size(), pattern,
                    static_cast<double>(milliseconds) / 1'000.0);
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

void CuesPanel::reset() noexcept {
  scroll_.reset();
  name_node_ = doc::NodeId{};
  name_draft_.clear();
  note_node_ = doc::NodeId{};
  note_draft_.clear();
  click_node_ = doc::NodeId{};
  click_time_ = 0.0;
  timing_node_ = doc::NodeId{};
  timing_field_ = TimingField::None;
  timing_draft_.clear();
  scrub_node_ = doc::NodeId{};
  scrub_field_ = TimingField::None;
  scrubbing_ = false;
  timing_click_node_ = doc::NodeId{};
  timing_click_field_ = TimingField::None;
  timing_click_time_ = 0.0;
  sent_.clear();
}

CuesPanelFrame CuesPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                               const EditorTimeline &timeline,
                               std::span<const scene::TimelineMarker> incoming,
                               std::span<const doc::NodeId> selected) {
  CuesPanelFrame frame{};
  // What the document says, with each edit this panel sent and the
  // document does not show yet laid over it.
  std::vector<scene::TimelineMarker> overlaid{incoming.begin(), incoming.end()};
  for (auto sent = sent_.begin(); sent != sent_.end();) {
    const auto found =
        std::find_if(overlaid.begin(), overlaid.end(),
                     [&](const scene::TimelineMarker &marker) {
                       return marker.id == sent->marker.id;
                     });
    if (found == overlaid.end() || *found == sent->marker ||
        sent->draws_left == 0U) {
      sent = sent_.erase(sent);
      continue;
    }
    *found = sent->marker;
    --sent->draws_left;
    ++sent;
  }
  const std::span<const scene::TimelineMarker> markers{overlaid};
  const auto remember_sent = [&]() {
    for (const TimelineMarkerEdit &edit : frame.marker_edits) {
      const auto found = std::find_if(
          sent_.begin(), sent_.end(), [&](const SentMarker &sent) {
            return sent.marker.id == edit.marker.id;
          });
      if (edit.action == TimelineMarkerAction::Delete) {
        if (found != sent_.end()) {
          sent_.erase(found);
        }
      } else if (edit.action == TimelineMarkerAction::Update) {
        if (found != sent_.end()) {
          *found = SentMarker{edit.marker, sent_marker_draws};
        } else {
          sent_.push_back(SentMarker{edit.marker, sent_marker_draws});
        }
      }
    }
  };
  const gui::Theme &theme = ctx.theme();
  const bool typing_at_start = ctx.typing();
  ctx.panel(bounds, theme.panel);
  if (ctx.pointer_pressed()) {
    focused_ = bounds.contains(ctx.pointer_x(), ctx.pointer_y());
  }
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return frame;
  }
  ctx.push_clip(bounds);

  // Cue numbers count cues alone, in show order; plain markers keep their
  // place in the list without taking a number.
  std::vector<std::size_t> numbers(markers.size(), 0U);
  std::size_t cue_count{};
  for (std::size_t index = 0U; index < markers.size(); ++index) {
    if (markers[index].is_cue()) {
      numbers[index] = ++cue_count;
    }
  }
  const auto find = [&](doc::NodeId id) -> std::optional<std::size_t> {
    for (std::size_t index = 0U; index < markers.size(); ++index) {
      if (markers[index].id == id) {
        return index;
      }
    }
    return std::nullopt;
  };
  const auto tag = [&](std::size_t index) {
    return markers[index].is_cue() ? "Q" + std::to_string(numbers[index])
                                   : std::string{"M"};
  };
  const auto describe = [&](doc::NodeId id) {
    std::string text;
    if (const auto index = find(id)) {
      text = tag(*index);
      if (!markers[*index].name.empty()) {
        text += "  ";
        text += markers[*index].name;
      }
    }
    return text;
  };
  const auto timecode = [&](core::i64 at) {
    return format_timecode(static_cast<core::u64>(std::max<core::i64>(at, 0)),
                           timeline.rate_numerator, timeline.rate_denominator);
  };
  if (name_node_ != doc::NodeId{} && !find(name_node_)) {
    name_node_ = doc::NodeId{};
    name_draft_.clear();
  }
  if (note_node_ != doc::NodeId{} && !find(note_node_)) {
    note_node_ = doc::NodeId{};
    note_draft_.clear();
  }
  if (timing_node_ != doc::NodeId{} && !find(timing_node_)) {
    timing_node_ = doc::NodeId{};
    timing_field_ = TimingField::None;
    timing_draft_.clear();
  }
  if (scrub_node_ != doc::NodeId{} && !find(scrub_node_)) {
    scrub_node_ = doc::NodeId{};
    scrub_field_ = TimingField::None;
    scrubbing_ = false;
  }

  std::vector<std::size_t> listed;
  for (std::size_t index = 0U; index < markers.size(); ++index) {
    if (show_markers_ || markers[index].is_cue()) {
      listed.push_back(index);
    }
  }
  const auto is_selected = [&](doc::NodeId id) {
    return std::find(selected.begin(), selected.end(), id) != selected.end();
  };

  // --- Header: the panel's name, then the two switches it owns.
  gui::Rect area = bounds;
  const gui::Rect header = gui::cut_top(area, header_height);
  ctx.panel(header, theme.header);
  {
    gui::Rect title = gui::inset(header, 10.0F, 0.0F);
    ctx.label(gui::cut_left(title, 120.0F), "CUE STACK", theme.text_muted);
  }
  {
    // Laid in from the right edge, the first switch outermost.
    gui::Rect switches = gui::inset(header, 8.0F, 5.0F);
    const auto toggle = [&](std::string_view name, std::string_view label,
                            bool engaged, std::string_view tip,
                            gui::UiColor color) {
      const gui::Rect button = gui::cut_right(
          switches, std::max(ctx.font_width(label) + 20.0F, 56.0F));
      static_cast<void>(gui::cut_right(switches, 4.0F));
      const gui::Interaction it = ctx.grab(gui::ui_id(id_, name), button);
      ctx.panel(button, engaged ? alpha(color, 0.25F)
                                : (it.hovered ? theme.widget_hover
                                              : theme.widget));
      if (engaged) {
        gui::Rect underline = button;
        ctx.panel(gui::cut_bottom(underline, 2.0F), color);
      }
      ctx.label(gui::inset(button, gui::Padding{10.0F, 0.0F, 2.0F, 0.0F}),
                label, engaged ? theme.text : theme.text_muted);
      ctx.tooltip(button, tip);
      return it.clicked;
    };
    if (toggle("armed", timeline.cues_armed ? "ARMED" : "CUES OFF",
               timeline.cues_armed,
               "Armed, the show stops on Hold cues and repeats Loop "
               "sections; off, it plays straight through",
               timeline_marker_color(0U))) {
      frame.cues_armed = !timeline.cues_armed;
    }
    if (toggle("markers", "MARKERS", show_markers_,
               "List plain markers among the cues", theme.accent)) {
      show_markers_ = !show_markers_;
    }
  }

  // --- GO, and the two lines an operator reads before pressing it.
  const gui::Rect band = gui::cut_top(area, go_band_height);
  ctx.panel(band, theme.row_alt);
  const bool any_cue = cue_count > 0U;
  const bool running =
      timeline.playing && timeline.looping_cue == doc::NodeId{};
  gui::Rect band_content = gui::inset(band, 8.0F, 6.0F);
  const gui::Rect go = gui::cut_left(
      band_content, std::clamp(band.width * 0.38F, 120.0F, 260.0F));
  {
    const gui::Interaction it = ctx.grab(gui::ui_id(id_, "go"), go);
    if (it.clicked) {
      frame.transport = TransportRequest::Go;
    }
    const gui::UiColor color =
        timeline.held_cue != doc::NodeId{} ? theme.playhead : theme.good;
    const bool live = any_cue && !running;
    ctx.panel(go, live ? alpha(color, (it.held && !it.released)
                                          ? 0.6F
                                          : (it.hovered ? 0.42F : 0.3F))
                       : (it.hovered ? theme.widget_hover : theme.widget));
    ctx.outline(go, live ? color : theme.divider, live ? 2.0F : 1.0F);
    // GO across the top half, what it will do across the bottom.
    const float half = go.height * 0.5F;
    ctx.label_centered(
        gui::inset(go, gui::Padding{0.0F, 4.0F, 0.0F, half - 4.0F}), "GO",
        live ? theme.text : theme.text_muted);
    std::string under;
    if (timeline.looping_cue != doc::NodeId{}) {
      under = "release loop";
    } else if (timeline.next_cue != doc::NodeId{}) {
      under = describe(timeline.next_cue);
    }
    ctx.label_centered(gui::inset(go, gui::Padding{6.0F, half, 6.0F, 4.0F}),
                       under, live ? theme.text : theme.text_muted);
    ctx.tooltip(go, "GO (Space or Enter here): run from a hold, release a "
                    "loop, or play to the next cue");
  }
  {
    gui::Rect lines = gui::inset(band_content,
                                 gui::Padding{14.0F, 2.0F, 0.0F, 0.0F});
    constexpr float line_height = 22.0F;
    std::string state;
    gui::UiColor state_color = theme.text_muted;
    if (!any_cue) {
      state = "Q on the Timeline adds a hold cue at the playhead";
    } else if (!timeline.cues_armed) {
      state = "CUES OFF  playing straight through";
    } else if (timeline.held_cue != doc::NodeId{}) {
      state = "HOLDING  " + describe(timeline.held_cue);
      state_color = theme.playhead;
    } else if (timeline.looping_cue != doc::NodeId{}) {
      state = "LOOPING  " + describe(timeline.looping_cue);
      if (timeline.frames_to_loop_restart.has_value()) {
        state += "   again in " +
                 seconds_text(*timeline.frames_to_loop_restart, timeline);
      }
      state_color = theme.accent;
    } else if (timeline.playing) {
      state = "RUNNING";
      state_color = theme.good;
    } else {
      state = "STOPPED";
    }
    ctx.label(gui::cut_top(lines, line_height), state, state_color);
    static_cast<void>(gui::cut_top(lines, 2.0F));
    if (any_cue && timeline.cues_armed &&
        timeline.next_cue != doc::NodeId{}) {
      std::string next = "NEXT  " + describe(timeline.next_cue);
      if (timeline.playing && timeline.frames_to_next_cue.has_value()) {
        next += "   in " + seconds_text(*timeline.frames_to_next_cue, timeline);
      } else if (const auto index = find(timeline.next_cue)) {
        next += "   at " + timecode(markers[*index].frame);
      }
      ctx.label(gui::cut_top(lines, line_height), next, theme.text);
    }
  }

  // --- Columns: laid across the heading once; every row takes its cells
  // from them. The right 30px is the delete button's lane.
  const gui::Rect columns = gui::cut_top(area, columns_height);
  ctx.panel(columns, theme.header);
  constexpr float column_gap = 6.0F;
  constexpr float narrow_gap = 4.0F;
  gui::Rect lanes = gui::inset(columns, gui::Padding{10.0F, 0.0F, 30.0F, 0.0F});
  const gui::Rect number_column = gui::cut_left(lanes, 36.0F);
  static_cast<void>(gui::cut_left(lanes, narrow_gap));
  const gui::Rect swatch_column = gui::cut_left(lanes, 10.0F);
  static_cast<void>(gui::cut_left(lanes, column_gap));
  // The name takes a share of what the fixed columns leave.
  const float flexible = std::max(lanes.width - 60.0F - 104.0F, 60.0F);
  const gui::Rect name_column =
      gui::cut_left(lanes, std::max(flexible * 0.45F, 60.0F));
  static_cast<void>(gui::cut_left(lanes, column_gap));
  const gui::Rect action_column = gui::cut_left(lanes, 54.0F);
  static_cast<void>(gui::cut_left(lanes, column_gap));
  const gui::Rect time_column = gui::cut_left(lanes, 100.0F);
  static_cast<void>(gui::cut_left(lanes, narrow_gap));
  // A Loop's timing sits beside its time and the note gives up the room. A
  // panel too narrow for it shows the chosen Loop's timing in a bar above
  // the footer instead, so it is never out of reach.
  const gui::Rect without_timing = lanes;
  const gui::Rect wait_column = gui::cut_left(lanes, timing_width);
  static_cast<void>(gui::cut_left(lanes, narrow_gap));
  const gui::Rect fade_column = gui::cut_left(lanes, timing_width);
  static_cast<void>(gui::cut_left(lanes, narrow_gap));
  const bool timing_columns = fade_column.width >= timing_width;
  const gui::Rect note_column = timing_columns ? lanes : without_timing;
  const auto heading = [&](const gui::Rect &column, std::string_view text) {
    ctx.label(gui::cell(column, columns), text, theme.text_muted);
  };
  heading(number_column, "#");
  heading(name_column, "NAME");
  heading(action_column, "ACTION");
  heading(time_column, "TIME");
  if (timing_columns) {
    heading(wait_column, "WAIT");
    heading(fade_column, "FADE");
  }
  std::optional<std::size_t> timing_subject;
  if (!timing_columns) {
    for (const std::size_t index : listed) {
      if (markers[index].cue == scene::CueAction::Loop &&
          is_selected(markers[index].id)) {
        timing_subject = index;
        break;
      }
    }
  }
  heading(note_column, "NOTE");

  // A Loop's WAIT and FADE: a drag scrubs and lands once on release, a
  // click chooses the cue, and a double-click types seconds.
  const auto timing_cell = [&](const scene::TimelineMarker &marker,
                               TimingField field, const gui::Rect &cell,
                               const gui::Interaction &it, core::u32 current,
                               core::u32 maximum, std::string_view field_name,
                               std::string_view tip) {
    std::optional<core::u32> commit;
    const bool scrubbed_here =
        scrub_node_ == marker.id && scrub_field_ == field;
    if (it.pressed) {
      scrub_node_ = marker.id;
      scrub_field_ = field;
      scrub_origin_x_ = ctx.pointer_x();
      scrub_origin_ms_ = current;
      scrub_ms_ = current;
      scrubbing_ = false;
    }
    if (it.held && scrub_node_ == marker.id && scrub_field_ == field) {
      const float moved = ctx.pointer_x() - scrub_origin_x_;
      if (!scrubbing_ && std::abs(moved) >= 3.0F) {
        scrubbing_ = true;
      }
      if (scrubbing_) {
        const double steps = std::round(moved / scrub_pixels_per_step);
        const double wanted = std::clamp(
            static_cast<double>(scrub_origin_ms_) +
                steps * (ctx.shift_held() ? fine_scrub_step_ms
                                          : scrub_step_ms),
            0.0, static_cast<double>(maximum));
        scrub_ms_ = static_cast<core::u32>(wanted);
        frame.editing = true;
      }
    }
    if (it.released && scrub_node_ == marker.id && scrub_field_ == field) {
      if (scrubbing_) {
        if (scrub_ms_ != current) {
          commit = scrub_ms_;
        }
      } else if (it.clicked) {
        frame.selected = marker.id;
        const double now = ctx.time_seconds();
        const bool second = timing_click_node_ == marker.id &&
                            timing_click_field_ == field &&
                            now >= timing_click_time_ &&
                            now - timing_click_time_ <= double_click_seconds;
        if (second) {
          timing_node_ = marker.id;
          timing_field_ = field;
          // Empty, so what is typed replaces the value; Enter on nothing
          // leaves it as it was.
          timing_draft_.clear();
          ctx.focus_text_field(gui::ui_id(id_, field_name, marker.id.value()),
                               timing_draft_);
          timing_click_node_ = doc::NodeId{};
        } else {
          timing_click_node_ = marker.id;
          timing_click_field_ = field;
          timing_click_time_ = now;
        }
      }
      scrub_node_ = doc::NodeId{};
      scrub_field_ = TimingField::None;
      scrubbing_ = false;
    }
    if (timing_node_ == marker.id && timing_field_ == field) {
      const gui::UiId typed = gui::ui_id(id_, field_name, marker.id.value());
      static_cast<void>(ctx.text_field(typed, cell, timing_draft_));
      if (ctx.focus() != typed) {
        const auto parsed = parse_cue_seconds(timing_draft_, maximum);
        if (parsed.has_value() && *parsed != current) {
          commit = parsed;
        }
        timing_node_ = doc::NodeId{};
        timing_field_ = TimingField::None;
        timing_draft_.clear();
      } else {
        frame.editing = true;
      }
      return commit;
    }
    const core::u32 shown =
        scrubbed_here && scrubbing_ && !commit.has_value() ? scrub_ms_
        : commit.has_value()                               ? *commit
                                                           : current;
    ctx.panel(cell, it.held ? theme.widget_active
                            : (it.hovered ? theme.widget_hover
                                          : theme.widget));
    ctx.label_centered(gui::inset(cell, 2.0F, 0.0F), format_cue_seconds(shown),
                       shown == 0U ? theme.text_muted : theme.accent);
    ctx.tooltip(cell, tip);
    return commit;
  };
  const auto loop_timing = [&](const scene::TimelineMarker &marker,
                               const gui::Rect &wait_cell,
                               const gui::Interaction &wait_it,
                               const gui::Rect &fade_cell,
                               const gui::Interaction &fade_it) {
    const auto wait = timing_cell(
        marker, TimingField::Wait, wait_cell, wait_it, marker.loop_wait_ms,
        scene::max_loop_wait_ms, "wait field",
        "How long the show holds the loop's last frame before going round. "
        "Drag to adjust (Shift for fine), double-click to type");
    const auto fade = timing_cell(
        marker, TimingField::Fade, fade_cell, fade_it, marker.loop_fade_ms,
        scene::max_loop_fade_ms, "fade field",
        "Crossfade into the next pass: from the held frame after a wait, or "
        "overlapping the end of the section without one. Drag to adjust "
        "(Shift for fine), double-click to type");
    if (wait.has_value() || fade.has_value()) {
      TimelineMarkerEdit edit{TimelineMarkerAction::Update, marker};
      edit.marker.loop_wait_ms = wait.value_or(marker.loop_wait_ms);
      edit.marker.loop_fade_ms = fade.value_or(marker.loop_fade_ms);
      frame.marker_edits.push_back(std::move(edit));
    }
  };

  // --- Rows, scrolled by the wheel, between the columns and the footer - and
  // the chosen Loop's timing bar, when the columns have no room for it.
  const gui::Rect footer = gui::cut_bottom(area, footer_height);
  constexpr float timing_bar_height = 28.0F;
  const gui::Rect timing_strip =
      timing_subject.has_value() ? gui::cut_bottom(area, timing_bar_height)
                                 : gui::Rect{};
  const gui::Rect list = area;
  const float scroll = scroll_.begin(ctx, list, row_height * 2.0F);
  for (std::size_t row = 0U; row < listed.size(); ++row) {
    const std::size_t index = listed[row];
    const scene::TimelineMarker &marker = markers[index];
    const gui::Rect bar = gui::list_row(list, row, row_height, scroll);
    if (bar.bottom() <= list.y || bar.y >= list.bottom()) {
      continue;
    }
    const bool chosen = is_selected(marker.id);
    const bool held = timeline.held_cue == marker.id;
    const bool looping = timeline.looping_cue == marker.id;
    const bool upcoming = timeline.next_cue == marker.id;
    const bool past = !held && !looping &&
                      marker.frame < static_cast<core::i64>(timeline.show_frame);
    ctx.panel(bar, chosen ? theme.selection
                          : (row % 2U == 0U ? theme.row : theme.row_alt));
    {
      // The state stripe down the row's left edge.
      gui::Rect stripe = bar;
      stripe = gui::cut_left(stripe, 3.0F);
      if (held) {
        ctx.panel(bar, alpha(theme.playhead, 0.22F));
        ctx.panel(stripe, theme.playhead);
      } else if (looping) {
        ctx.panel(bar, alpha(theme.accent, 0.2F));
        ctx.panel(stripe, theme.accent);
      } else if (upcoming && timeline.cues_armed) {
        ctx.panel(stripe, theme.good);
      }
    }
    const gui::UiColor text =
        past ? theme.text_muted : theme.text;

    // The swatch recolours, the action word cycles, and the x deletes;
    // everything else on the row selects it, and a double-click goes there.
    constexpr float swatch_size = 10.0F;
    const gui::Rect swatch = gui::centered(gui::cell(swatch_column, bar),
                                           swatch_size, swatch_size);
    const gui::Rect action =
        gui::inset(gui::cell(action_column, bar), 0.0F, 3.0F);
    gui::Rect remove_lane = bar;
    remove_lane = gui::cut_right(remove_lane, 26.0F);
    const gui::Rect remove =
        gui::inset(gui::cut_left(remove_lane, 18.0F), 0.0F, 4.0F);
    const gui::Rect name_cell = gui::cell(name_column, bar);
    const gui::Rect note_cell = gui::cell(note_column, bar);
    const gui::Interaction swatch_it =
        ctx.grab(gui::ui_id(id_, "swatch", marker.id.value()), swatch);
    const gui::Interaction action_it =
        ctx.grab(gui::ui_id(id_, "action", marker.id.value()), action);
    const bool hovered_row = bar.contains(ctx.pointer_x(), ctx.pointer_y());
    const gui::Interaction remove_it =
        hovered_row || chosen
            ? ctx.grab(gui::ui_id(id_, "remove", marker.id.value()), remove)
            : gui::Interaction{};
    const bool loop_cue = marker.cue == scene::CueAction::Loop;
    const gui::Rect wait_cell =
        gui::inset(gui::cell(wait_column, bar), 0.0F, 3.0F);
    const gui::Rect fade_cell =
        gui::inset(gui::cell(fade_column, bar), 0.0F, 3.0F);
    const gui::Interaction wait_it =
        loop_cue && timing_columns
            ? ctx.grab(gui::ui_id(id_, "wait", marker.id.value()), wait_cell)
            : gui::Interaction{};
    const gui::Interaction fade_it =
        loop_cue && timing_columns
            ? ctx.grab(gui::ui_id(id_, "fade", marker.id.value()), fade_cell)
            : gui::Interaction{};
    const gui::Interaction row_it =
        ctx.grab(gui::ui_id(id_, "row", marker.id.value()), bar);
    if (swatch_it.clicked) {
      TimelineMarkerEdit edit{TimelineMarkerAction::Update, marker};
      edit.marker.color = static_cast<core::u8>(
          (marker.color + 1U) % scene::timeline_marker_colors);
      frame.marker_edits.push_back(std::move(edit));
    }
    if (action_it.clicked) {
      TimelineMarkerEdit edit{TimelineMarkerAction::Update, marker};
      switch (marker.cue) {
      case scene::CueAction::Hold:
        edit.marker.cue = scene::CueAction::Loop;
        break;
      case scene::CueAction::Loop:
        edit.marker.cue =
            show_markers_ ? scene::CueAction::None : scene::CueAction::Hold;
        break;
      case scene::CueAction::None:
        edit.marker.cue = scene::CueAction::Hold;
        break;
      }
      frame.marker_edits.push_back(std::move(edit));
    }
    if (remove_it.clicked) {
      frame.marker_edits.push_back(
          TimelineMarkerEdit{TimelineMarkerAction::Delete, marker});
    }
    // Rows act on release, so a field opened by a double-click is not
    // closed again by the press that opened it.
    if (row_it.clicked) {
      frame.selected = marker.id;
      const double now = ctx.time_seconds();
      const bool second = click_node_ == marker.id && now >= click_time_ &&
                          now - click_time_ <= double_click_seconds;
      if (second && name_cell.contains(ctx.pointer_x(), ctx.pointer_y())) {
        name_node_ = marker.id;
        name_draft_ = marker.name;
        ctx.focus_text_field(gui::ui_id(id_, "name", marker.id.value()), name_draft_);
      } else if (second &&
                 note_cell.contains(ctx.pointer_x(), ctx.pointer_y())) {
        note_node_ = marker.id;
        note_draft_ = marker.note;
        ctx.focus_text_field(gui::ui_id(id_, "note", marker.id.value()), note_draft_);
      } else if (second) {
        frame.seek_to_frame =
            static_cast<core::u64>(std::max<core::i64>(marker.frame, 0));
      }
      click_node_ = second ? doc::NodeId{} : marker.id;
      click_time_ = now;
    }

    ctx.label(gui::cell(number_column, bar), tag(index),
              marker.is_cue() ? text : theme.text_muted);
    ctx.panel(swatch, timeline_marker_color(marker.color));
    if (swatch_it.hovered) {
      ctx.outline(swatch, theme.text);
    }
    if (name_node_ == marker.id) {
      const gui::UiId field = gui::ui_id(id_, "name", marker.id.value());
      static_cast<void>(ctx.text_field(
          field, gui::inset(name_cell, 0.0F, 3.0F), name_draft_));
      if (ctx.focus() != field) {
        if (name_draft_ != marker.name &&
            name_draft_.size() <= scene::max_timeline_marker_name_bytes) {
          TimelineMarkerEdit edit{TimelineMarkerAction::Update, marker};
          edit.marker.name = name_draft_;
          frame.marker_edits.push_back(std::move(edit));
        }
        name_node_ = doc::NodeId{};
        name_draft_.clear();
      } else {
        frame.editing = true;
      }
    } else {
      ctx.label(name_cell, marker.name, text);
    }
    ctx.panel(action, action_it.hovered ? theme.widget_hover : theme.widget);
    ctx.label(gui::inset(action, gui::Padding{8.0F, 0.0F, 2.0F, 0.0F}),
              action_word(marker.cue),
              marker.cue == scene::CueAction::Hold
                  ? timeline_marker_color(0U)
                  : (marker.cue == scene::CueAction::Loop
                         ? theme.accent
                         : theme.text_muted));
    ctx.tooltip(action, "Click to change what the cue does");
    std::string time = timecode(marker.frame);
    if (upcoming && timeline.playing && timeline.frames_to_next_cue) {
      time = "in " + seconds_text(*timeline.frames_to_next_cue, timeline);
    }
    ctx.label(gui::cell(time_column, bar), time,
              upcoming ? theme.good : text);

    if (loop_cue && timing_columns) {
      loop_timing(marker, wait_cell, wait_it, fade_cell, fade_it);
    }
    if (note_node_ == marker.id) {
      const gui::UiId field = gui::ui_id(id_, "note", marker.id.value());
      static_cast<void>(ctx.text_field(
          field, gui::inset(note_cell, 0.0F, 3.0F), note_draft_));
      if (ctx.focus() != field) {
        if (note_draft_ != marker.note &&
            note_draft_.size() <= scene::max_timeline_marker_note_bytes) {
          TimelineMarkerEdit edit{TimelineMarkerAction::Update, marker};
          edit.marker.note = note_draft_;
          frame.marker_edits.push_back(std::move(edit));
        }
        note_node_ = doc::NodeId{};
        note_draft_.clear();
      } else {
        frame.editing = true;
      }
    } else {
      ctx.label(note_cell, marker.note, theme.text_muted);
    }
    if (hovered_row || chosen) {
      ctx.panel(remove, remove_it.hovered ? theme.warning : theme.widget);
      const float cx = remove.center_x();
      const float cy = remove.center_y();
      ctx.draw_list().add_line(cx - 3.5F, cy - 3.5F, cx + 3.5F, cy + 3.5F,
                               1.5F, theme.text);
      ctx.draw_list().add_line(cx - 3.5F, cy + 3.5F, cx + 3.5F, cy - 3.5F,
                               1.5F, theme.text);
      ctx.tooltip(remove, marker.is_cue() ? "Delete cue" : "Delete marker");
    }
  }
  scroll_.end(ctx, static_cast<float>(listed.size()) * row_height);

  // --- The chosen Loop's timing, when the columns do not fit.
  if (timing_subject.has_value()) {
    const scene::TimelineMarker &marker = markers[*timing_subject];
    ctx.panel(timing_strip, theme.row_alt);
    gui::Rect line = gui::inset(timing_strip, 8.0F, 0.0F);
    const auto caption = [&](std::string_view text) {
      ctx.label(gui::cut_left(line, ctx.font_width(text) + 6.0F), text,
                theme.text_muted);
    };
    caption(tag(*timing_subject) + " WAIT");
    const gui::Rect wait_cell =
        gui::inset(gui::cut_left(line, timing_width), 0.0F, 4.0F);
    static_cast<void>(gui::cut_left(line, 8.0F));
    caption("FADE");
    const gui::Rect fade_cell =
        gui::inset(gui::cut_left(line, timing_width), 0.0F, 4.0F);
    const gui::Interaction wait_it =
        ctx.grab(gui::ui_id(id_, "wait", marker.id.value()), wait_cell);
    const gui::Interaction fade_it =
        ctx.grab(gui::ui_id(id_, "fade", marker.id.value()), fade_cell);
    loop_timing(marker, wait_cell, wait_it, fade_cell, fade_it);
  }

  // --- Footer: creation at the playhead, the one thing here with nothing to
  // drag.
  ctx.panel(footer, theme.header);
  {
    gui::Rect buttons = gui::inset(footer, 8.0F, 5.0F);
    const auto add = [&](std::string_view name, std::string_view label,
                         scene::CueAction cue, core::u8 color) {
      const float width = ctx.font_width(label) + 20.0F;
      if (width > buttons.width) {
        return;
      }
      const gui::Rect button = gui::cut_left(buttons, width);
      static_cast<void>(gui::cut_left(buttons, 4.0F));
      if (ctx.button(gui::ui_id(id_, name), button, label)) {
        TimelineMarkerEdit edit{};
        edit.action = TimelineMarkerAction::Create;
        edit.marker.frame = static_cast<core::i64>(timeline.show_frame);
        edit.marker.cue = cue;
        edit.marker.color = color;
        if (cue == scene::CueAction::None) {
          const auto plain = static_cast<std::size_t>(std::count_if(
              markers.begin(), markers.end(),
              [](const scene::TimelineMarker &held) noexcept {
                return !held.is_cue();
              }));
          edit.marker.name = "Marker " + std::to_string(plain + 1U);
        }
        frame.marker_edits.push_back(std::move(edit));
      }
    };
    add("add hold", "+ HOLD CUE", scene::CueAction::Hold, 0U);
    add("add loop", "+ LOOP CUE", scene::CueAction::Loop, 6U);
    add("add marker", "+ MARKER", scene::CueAction::None, 3U);
  }

  // --- Keyboard: the desk's habits. Space and Enter are GO here, as on any
  // cue stack; arrows walk the list; Delete removes the chosen cue.
  if (focused_ && !frame.editing && !typing_at_start && !ctx.typing()) {
    const std::string_view typed = ctx.shortcut_text();
    if (ctx.key_pressed(gui::Key::Enter) ||
        typed.find(' ') != std::string_view::npos) {
      frame.transport = TransportRequest::Go;
    }
    const auto current = std::find_if(
        listed.begin(), listed.end(),
        [&](std::size_t index) { return is_selected(markers[index].id); });
    if (!listed.empty() && ctx.key_pressed(gui::Key::Down)) {
      frame.selected =
          markers[current == listed.end() || std::next(current) == listed.end()
                      ? (current == listed.end() ? listed.front()
                                                 : *current)
                      : *std::next(current)]
              .id;
    } else if (!listed.empty() && ctx.key_pressed(gui::Key::Up)) {
      frame.selected =
          markers[current == listed.end() || current == listed.begin()
                      ? (current == listed.end() ? listed.back() : *current)
                      : *std::prev(current)]
              .id;
    }
    if ((ctx.key_pressed(gui::Key::Delete) ||
         ctx.key_pressed(gui::Key::Backspace)) &&
        current != listed.end()) {
      frame.marker_edits.push_back(
          TimelineMarkerEdit{TimelineMarkerAction::Delete, markers[*current]});
      frame.consume_delete = true;
    }
  }

  ctx.pop_clip();
  remember_sent();
  return frame;
}

} // namespace mediaserver::editor
