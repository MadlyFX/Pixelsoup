#include "timeline_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

// The Timeline's toolbar: transport, the playhead readout and show rate, the
// pointer tools, the view toggles, marker and cue creation, and the cue
// status, GO and arm at the right edge. Its rate menu draws later in the
// frame, above the rows.
namespace mediaserver::editor {
using namespace timeline;

void TimelinePanel::draw_toolbar(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const gui::Rect &bounds = p.bounds;
  const EditorTimeline &timeline = p.input.timeline;
  const std::span<const scene::ChannelKeyframes> keyframes = p.input.keyframes;
  const std::span<const scene::TimelineLane> lanes = p.input.lanes;
  TimelinePanelFrame &frame = p.frame;
  FrameScratch &scratch = p.scratch;
  const gui::Rect &toolbar = p.layout.toolbar;
  auto &first = p.first;
  auto &last = p.last;
  auto &cue_numbers = p.cue_numbers;
  auto &any_cue = p.any_cue;

  // ---------------------------------------------------------------------
  // Toolbar. Left to right: transport, where the show is, its rate, the
  // pointer tools, view toggles, and the two things the timeline creates at
  // the playhead. GO and the cue arm sit hard against the right edge, where a
  // hand running a live show finds them without reading; exact In/Out entry
  // takes the space between when there is room for it.
  const gui::Theme &theme = ctx.theme();
  ctx.panel(toolbar, theme.panel);
  {
    gui::Rect rule = toolbar;
    ctx.panel(gui::cut_bottom(rule, 1.0F), theme.divider);
  }
  constexpr float button_height = 22.0F;
  const bool show_key_tools = !keyframes.empty() || !keys_.selected.empty();

  // One strip of controls 6px in from the toolbar's edges: GO and the cue
  // arm from its end, the left group from its start, and the In/Out range in
  // whatever room is left between them.
  gui::Rect bar = gui::inset(toolbar, gui::Padding{6.0F, 6.0F, 6.0F, 0.0F});
  bar = gui::cut_top(bar, button_height);
  gui::Rect go_button{};
  gui::Rect armed_button{};
  if (any_cue) {
    go_button = gui::cut_right(bar, 100.0F);
    static_cast<void>(gui::cut_right(bar, 4.0F));
    armed_button = gui::cut_right(bar, icon_button_width);
    static_cast<void>(gui::cut_right(bar, 8.0F));
  }
  // Whether a control `width` wide fits before the right-hand group, and
  // the next control's place. A control that does not fit still takes its
  // place, so everything after it is dropped too.
  const auto fits = [&](float width) noexcept { return width <= bar.width; };
  const auto place = [&](float width, float gap_after) {
    const gui::Rect at = gui::cut_left(bar, width);
    static_cast<void>(gui::cut_left(bar, gap_after));
    return at;
  };

  // A time field: drag sideways to scrub a frame per pixel, click to type a
  // timecode or a frame count. Reports a value on every frame a drag changes
  // it, and once when typed text is committed.
  const auto time_field = [&](gui::UiId id, const gui::Rect &field,
                              std::string_view caption, core::u64 value,
                              core::u64 maximum,
                              gui::UiColor color) -> std::optional<core::u64> {
    const gui::UiId text_id = mix(id, "text");
    std::optional<core::u64> result;
    if (toolbar_.time_edit_id == id) {
      static_cast<void>(ctx.text_field(text_id, field, toolbar_.time_draft));
      if (ctx.focus() != text_id) {
        // Enter, Tab, or a press elsewhere commits; Escape has already put
        // the original text back, which parses to the value unchanged.
        if (const auto parsed =
                parse_timecode(toolbar_.time_draft, timeline.rate_numerator,
                               timeline.rate_denominator, value);
            parsed.has_value() && std::min(*parsed, maximum) != value) {
          result = std::min(*parsed, maximum);
        }
        toolbar_.time_edit_id = gui::UiId{};
        toolbar_.time_draft.clear();
      }
      return result;
    }
    const gui::Interaction interaction = ctx.grab(id, field);
    if (interaction.pressed) {
      toolbar_.time_drag_id = id;
      toolbar_.time_drag_start_x = ctx.pointer_press_x();
      toolbar_.time_drag_origin = value;
      toolbar_.time_drag_moved = false;
    }
    if (toolbar_.time_drag_id == id && interaction.held) {
      const float dx = ctx.pointer_x() - toolbar_.time_drag_start_x;
      toolbar_.time_drag_moved = toolbar_.time_drag_moved || std::fabs(dx) >= 3.0F;
      if (toolbar_.time_drag_moved) {
        const core::i64 moved =
            signed_frame(toolbar_.time_drag_origin) + std::llround(dx);
        const core::u64 next = static_cast<core::u64>(std::clamp<core::i64>(
            moved, 0, signed_frame(maximum)));
        if (next != value) {
          result = next;
        }
      }
      if (interaction.released) {
        if (!toolbar_.time_drag_moved && interaction.clicked) {
          toolbar_.time_edit_id = id;
          toolbar_.time_draft = timecode(p, value);
          ctx.focus_text_field(text_id, toolbar_.time_draft);
        }
        toolbar_.time_drag_id = gui::UiId{};
        toolbar_.time_drag_moved = false;
      }
    }
    // A soft well with a lit dot in the time's own colour.
    ctx.rounded(field, interaction.hovered ? theme.widget_hover : theme.track,
                field.height * 0.5F);
    draw_status_dot(ctx, field.x + 6.0F, field.center_y(), color, 2.5F);
    // The caption, then the time.
    gui::Rect text = gui::inset(field, gui::Padding{14.0F, 0.0F, 2.0F, 0.0F});
    if (!caption.empty()) {
      gui::Rect caption_cell = text;
      ctx.label(gui::cut_left(caption_cell, 28.0F), caption, theme.text_muted);
      static_cast<void>(gui::cut_left(text, ctx.font_width(caption) + 6.0F));
    }
    ctx.label(text, timecode(p, value), color);
    ctx.tooltip(field, "Drag to scrub, click to type a timecode or frame");
    return result;
  };

  const auto transport_icon = [&](std::string_view name, float width,
                                  gui::Icon icon, TransportRequest request,
                                  std::string_view tip, bool engaged = false) {
    const bool fitted = fits(width);
    const gui::Rect at = place(width, 2.0F);
    // Play lights green while the show runs, like the show bar's Program.
    if (fitted && icon_button(ctx, mix(id_, name), at, icon, engaged, tip,
                              name == "play" ? theme.good : theme.playhead)) {
      frame.transport = request;
    }
  };
  // An icon button in the left group, `gap_after` short of the next.
  const auto bar_icon = [&](std::string_view name, gui::Icon icon,
                            bool engaged, std::string_view tip,
                            float gap_after = 2.0F) {
    const bool fitted = fits(icon_button_width);
    const gui::Rect at = place(icon_button_width, gap_after);
    return fitted && icon_button(ctx, mix(id_, name), at, icon, engaged, tip,
                                 theme.accent);
  };
  const auto separate = [&] {
    static_cast<void>(gui::cut_left(bar, 2.0F));
    draw_separator(ctx, bar.x, toolbar);
    static_cast<void>(gui::cut_left(bar, 4.0F));
  };
  transport_icon("to start", icon_button_width, gui::Icon::ToStart,
                 TransportRequest::GoToStart, "Go to In (Home)");
  transport_icon("step back", icon_button_width, gui::Icon::StepBack,
                 TransportRequest::StepBack,
                 "Back one frame (Left), one second (Shift+Left)");
  transport_icon("play", 30.0F,
                 timeline.playing ? gui::Icon::Pause : gui::Icon::Play,
                 TransportRequest::TogglePlay,
                 "Play forwards / Pause (Space); J, K and L shuttle",
                 timeline.playing);
  transport_icon("step forward", icon_button_width, gui::Icon::StepForward,
                 TransportRequest::StepForward,
                 "Forward one frame (Right), one second (Shift+Right)");
  transport_icon("to end", icon_button_width, gui::Icon::ToEnd,
                 TransportRequest::GoToEnd, "Go to Out (End)");
  static_cast<void>(gui::cut_left(bar, 2.0F));
  if (bar_icon("loop", gui::Icon::Loop, timeline.loop, "Loop the In/Out range",
               8.0F)) {
    frame.range = RangeRequest{first, last, !timeline.loop};
  }

  // Where the Stage playhead is. Typing a time goes there; dragging scrubs.
  constexpr float readout_width = 110.0F;
  {
    const bool fitted = fits(readout_width);
    const gui::Rect readout = place(readout_width, 4.0F);
    if (fitted) {
      if (const auto moved = time_field(
              mix(id_, "playhead time"), readout, {}, timeline.show_frame,
              std::max(last, timeline.show_frame) + 1'000'000U,
              theme.playhead)) {
        frame.seek_to_frame = *moved;
      }
    }
  }

  constexpr float rate_width = 70.0F;
  gui::Rect rate_button{};
  auto &rate_popup = p.rate_popup;
  const bool rate_fits = fits(rate_width);
  const gui::Rect rate_slot = place(rate_width, 4.0F);
  if (rate_fits) {
    rate_button = rate_slot;
    if (ctx.button(mix(id_, "show rate"), rate_button, rate_label(timeline))) {
      toolbar_.rate_menu_open = !toolbar_.rate_menu_open;
      key_tools_.open = false;
    }
    constexpr float popup_width = 104.0F;
    constexpr float choice_height = 21.0F;
    rate_popup = gui::Rect{rate_button.x, toolbar.bottom() - 2.0F,
                           popup_width,
                           8.0F + choice_height *
                                      static_cast<float>(common_rates.size())};
    if (toolbar_.rate_menu_open && ctx.pointer_pressed() &&
        !rate_button.contains(ctx.pointer_x(), ctx.pointer_y()) &&
        !rate_popup.contains(ctx.pointer_x(), ctx.pointer_y())) {
      toolbar_.rate_menu_open = false;
    }
    if (toolbar_.rate_menu_open &&
        rate_popup.contains(ctx.pointer_x(), ctx.pointer_y())) {
      // Process and draw the menu after the timeline. Until then, mask
      // the graph underneath it so choosing a rate cannot also scrub.
      ctx.push_pointer_block();
      scratch.popup_pointer_blocked = true;
    }
  } else {
    toolbar_.rate_menu_open = false;
  }

  // The pointer tools. Each acts on the clip it is pressed on.
  struct ToolChoice final {
    TimelineEditTool tool;
    gui::Icon icon;
    std::string_view name;
    std::string_view tip;
  };
  constexpr std::array<ToolChoice, 6U> tool_choices{{
      {TimelineEditTool::Standard, gui::Icon::Select, "tool select",
       "Selection: move, trim and fade clips (V)"},
      {TimelineEditTool::RippleTrim, gui::Icon::Ripple, "tool ripple",
       "Ripple Trim: trim an edge and pull what follows along (B)"},
      {TimelineEditTool::Roll, gui::Icon::Roll, "tool roll",
       "Roll: move the cut between two butted clips (N)"},
      {TimelineEditTool::Slip, gui::Icon::Slip, "tool slip",
       "Slip: change the source frames a clip shows (Y)"},
      {TimelineEditTool::Slide, gui::Icon::Slide, "tool slide",
       "Slide: move a clip between its neighbours (U)"},
      {TimelineEditTool::Razor, gui::Icon::Razor, "tool razor",
       "Razor: cut a clip where clicked; Shift cuts every track (C)"},
  }};
  draw_separator(ctx, bar.x, toolbar);
  static_cast<void>(gui::cut_left(bar, 4.0F));
  for (const ToolChoice &choice : tool_choices) {
    // The enlarged marks still fit the existing width, keeping transport and
    // editing controls reachable at the same compact panel sizes; they stand
    // taller than the strip, centred in the toolbar.
    constexpr float tool_width = icon_button_width;
    constexpr float tool_height = button_height * edit_tool_scale;
    const bool fitted = fits(tool_width);
    const gui::Rect at = gui::centered(gui::cell(place(tool_width, 2.0F), toolbar),
                                       tool_width, tool_height);
    if (fitted &&
        icon_button(ctx, mix(id_, choice.name), at, choice.icon,
                    edit_tool_ == choice.tool, choice.tip, theme.accent,
                    edit_tool_scale)) {
      edit_tool_ = choice.tool;
    }
  }
  separate();

  if (bar_icon("snap", gui::Icon::Snap, snapping_,
               "Snapping to the playhead, cuts, keys and markers (S)")) {
    snapping_ = !snapping_;
  }
  if (bar_icon("fit", gui::Icon::Fit, false, "Zoom to fit the range (\\)")) {
    zoom_to_fit(p);
  }
  constexpr float curves_width = 52.0F;
  const bool curves_fit = fits(curves_width);
  const gui::Rect curves_toggle = place(curves_width, 2.0F);
  if (curves_fit &&
      text_toggle(ctx, mix(id_, "graph mode"), curves_toggle, "Curves",
                  graph_mode_,
                  "Draw animated values as curves instead of keys")) {
    graph_mode_ = !graph_mode_;
    // Entering Curves shows every value the graph will draw.
    if (graph_mode_) {
      graph_->request_fit();
    }
  }

  // Key selection tools live behind one button, shown only when there is
  // animation to use them on.
  constexpr float keys_width = 46.0F;
  gui::Rect key_tools_button{};
  if (show_key_tools && fits(keys_width)) {
    key_tools_button = place(keys_width, 2.0F);
    if (text_toggle(ctx, mix(id_, "key tools"), key_tools_button, "Keys",
                    key_tools_.open,
                    "Key selection, value and time-scale tools")) {
      key_tools_.open = !key_tools_.open;
      toolbar_.rate_menu_open = false;
      if (!key_tools_.open) {
        key_tools_.scale_open = false;
      }
    }
  } else {
    key_tools_.open = false;
    key_tools_.scale_open = false;
  }
  const float key_popup_width = std::min(338.0F, bounds.width);
  p.key_tools_popup = gui::Rect{
      std::clamp(key_tools_button.x, bounds.x,
                 std::max(bounds.x, bounds.right() - key_popup_width)),
      toolbar.bottom() - 2.0F, key_popup_width, 224.0F};
  auto &key_tools_popup = p.key_tools_popup;
  p.scale_numerator_id = mix(id_, "scale numerator");
  auto &scale_numerator_id = p.scale_numerator_id;
  p.scale_denominator_id = mix(id_, "scale denominator");
  auto &scale_denominator_id = p.scale_denominator_id;
  p.scale_user_anchor_id = mix(id_, "scale user anchor");
  auto &scale_user_anchor_id = p.scale_user_anchor_id;
  if (key_tools_.open && ctx.pointer_pressed() &&
      !key_tools_button.contains(ctx.pointer_x(), ctx.pointer_y()) &&
      !key_tools_popup.contains(ctx.pointer_x(), ctx.pointer_y())) {
    if (ctx.focus() == scale_numerator_id ||
        ctx.focus() == scale_denominator_id ||
        ctx.focus() == scale_user_anchor_id) {
      ctx.clear_focus();
    }
    key_tools_.open = false;
    key_tools_.scale_open = false;
  }
  if (key_tools_.open &&
      key_tools_popup.contains(ctx.pointer_x(), ctx.pointer_y())) {
    ctx.push_pointer_block();
    scratch.key_popup_pointer_blocked = true;
  }
  separate();

  // New markers take the next number and a colour that says what they are:
  // plain markers green, holds red, loops purple. The operator recolours and
  // renames from the marker itself.
  // The playhead an operator just put somewhere is where the next key acts,
  // even before the Director's answer has come back round: a click on the
  // ruler followed at once by Q must cue the frame clicked, not the one the
  // playhead was leaving.
  if (requested_playhead_.has_value() &&
      (timeline.show_frame == *requested_playhead_ || timeline.playing ||
       requested_playhead_frames_ == 0U)) {
    requested_playhead_.reset();
  } else if (requested_playhead_.has_value()) {
    --requested_playhead_frames_;
  }
  p.acting_frame =
      requested_playhead_.value_or(timeline.show_frame);
  auto &acting_frame = p.acting_frame;
  p.playhead = signed_frame(acting_frame);
  auto &playhead = p.playhead;
  // Snapping pulls onto the playhead the operator can see.
  p.shown_playhead = signed_frame(timeline.show_frame);
  if (bar_icon("add marker", gui::Icon::Marker, false,
               "Add a marker at the playhead (M)")) {
    create_marker(p, playhead, scene::CueAction::None);
  }
  if (bar_icon("add cue", gui::Icon::Hold, false,
               "Add a hold cue at the playhead (Q); Shift+Q adds a loop")) {
    create_marker(p, playhead, scene::CueAction::Hold);
  }
  // Playback's own state, after what the operator authors: the cache bar
  // and the render nodes' playheads.
  if (bar_icon("playback overlay", gui::Icon::Cache, playback_overlay_,
               "Show the frames this machine has decoded ready (green "
               "bar) and where each render node is playing (white "
               "playheads)",
               0.0F)) {
    playback_overlay_ = !playback_overlay_;
  }
  if (!p.input.moments.empty() || !moments_visible_) {
    if (bar_icon("plateset moments", gui::Icon::Moments, moments_visible_,
                 "Show the moments kept from placed platesets - stops, "
                 "turns, bumps - where their footage plays them",
                 0.0F)) {
      moments_visible_ = !moments_visible_;
    }
  }

  // Exact In/Out entry, right-aligned before GO, when what is left holds
  // both fields - each wide enough for its caption and the longest value it
  // can show, so the range never reads as "IN 00:00:0...".
  const float range_field_width =
      std::max(96.0F, 8.0F + ctx.font_width("OUT") + 6.0F +
                          ctx.font_width(timecode(p, last)) + 8.0F);
  const bool show_range = bar.width >= range_field_width * 2.0F + 12.0F;
  gui::Rect in_field{};
  gui::Rect out_field{};
  if (show_range) {
    out_field = gui::cut_right(bar, range_field_width);
    static_cast<void>(gui::cut_right(bar, 4.0F));
    in_field = gui::cut_right(bar, range_field_width);
  }
  if (show_range) {
    if (const auto entered = time_field(mix(id_, "range first"), in_field,
                                        "IN", first, last, theme.text)) {
      frame.range = RangeRequest{*entered, std::max(last, *entered),
                                 timeline.loop};
    }
    if (const auto entered =
            time_field(mix(id_, "range last"), out_field, "OUT", last,
                       static_cast<core::u64>(
                           std::numeric_limits<core::i64>::max()),
                       theme.text)) {
      frame.range = RangeRequest{std::min(first, *entered), *entered,
                                 timeline.loop};
    }
  }

  // The cue being acted on, and the one GO reaches next.
  if (any_cue) {
    if (icon_button(ctx, mix(id_, "cues armed"), armed_button, gui::Icon::Hold,
                    timeline.cues_armed,
                    timeline.cues_armed
                        ? "Cues armed: the show stops and loops on cues"
                        : "Cues off: the show plays straight through them",
                    timeline_marker_color(0U))) {
      frame.cues_armed = !timeline.cues_armed;
    }
    // GO does something when the show is held, looping, or stopped short of
    // a cue; while it runs towards one, the button counts down instead.
    const bool running = timeline.playing && timeline.looping_cue == doc::NodeId{};
    const gui::Interaction go = ctx.grab(mix(id_, "go"), go_button);
    if (go.clicked) {
      frame.transport = TransportRequest::Go;
    }
    const gui::UiColor go_color =
        timeline.held_cue != doc::NodeId{} ? theme.caution : theme.cue;
    const float go_radius = go_button.height * 0.5F;
    if (!running) {
      ctx.glow(go_button, go_color, go_radius, go.hovered ? 10.0F : 7.0F);
    }
    ctx.rounded(go_button,
                running ? (go.hovered ? theme.widget_hover : theme.widget)
                        : alpha(go_color,
                                (go.held && !go.released)
                                    ? 0.55F
                                    : (go.hovered ? 0.40F : 0.28F)),
                go_radius);
    if (!running) {
      ctx.rounded_outline(go_button, go_color, go_radius);
    }
    std::string go_text;
    if (running) {
      go_text = timeline.next_cue != doc::NodeId{}
                    ? cue_tag(cue_numbers[marker_index(p, timeline.next_cue)
                                              .value_or(0U)]) +
                          " in " +
                          seconds_label(p, timeline.frames_to_next_cue.value_or(0U))
                    : "RUNNING";
    } else if (timeline.looping_cue != doc::NodeId{}) {
      go_text = "GO  release";
    } else if (timeline.next_cue != doc::NodeId{}) {
      go_text = "GO  " + cue_tag(cue_numbers[marker_index(p, timeline.next_cue)
                                                 .value_or(0U)]);
    } else {
      go_text = "GO";
    }
    ctx.label(gui::inset(go_button, gui::Padding{9.0F, 0.0F, 3.0F, 0.0F}),
              go_text, running ? theme.text_muted : theme.text);
    ctx.tooltip(go_button,
                "GO (Enter): run from a hold, release a loop, or play to the "
                "next cue");
  }

  // A track that went away under the panel takes its selection with it.
  if (selected_lane_ != doc::NodeId{} &&
      std::none_of(lanes.begin(), lanes.end(),
                   [&](const scene::TimelineLane &lane) {
                     return !lane.implicit && lane.id == selected_lane_;
                   })) {
    selected_lane_ = doc::NodeId{};
    reorder_.lane_destination = doc::NodeId{};
  }

}

void TimelinePanel::zoom_to_fit(Pass &p) noexcept {
  const gui::Rect &graph = p.layout.graph;
  auto &first = p.first;
  auto &last = p.last;

  const double span =
      std::max(static_cast<double>(last) - static_cast<double>(first), 1.0);
  time_axis_.fit(static_cast<double>(first), span, graph.width);
}

void TimelinePanel::create_marker(Pass &p, core::i64 at, scene::CueAction cue) {
  const std::span<const scene::TimelineMarker> markers = p.input.markers;
  TimelinePanelFrame &frame = p.frame;

  TimelineMarkerEdit edit{};
  edit.action = TimelineMarkerAction::Create;
  edit.marker.frame = std::max<core::i64>(at, 0);
  edit.marker.cue = cue;
  if (cue == scene::CueAction::None) {
    const auto plain = static_cast<std::size_t>(std::count_if(
        markers.begin(), markers.end(),
        [](const scene::TimelineMarker &marker) noexcept {
          return !marker.is_cue();
        }));
    edit.marker.name = "Marker " + std::to_string(plain + 1U);
    edit.marker.color = 3U;
  } else {
    edit.marker.color = cue == scene::CueAction::Hold ? 0U : 6U;
  }
  frame.marker_edits.push_back(std::move(edit));
}

std::string TimelinePanel::cue_label(Pass &p, doc::NodeId id) {
  const std::span<const scene::TimelineMarker> markers = p.input.markers;
  auto &cue_numbers = p.cue_numbers;

  std::string label;
  if (const auto index = marker_index(p, id); index.has_value()) {
    label = cue_tag(cue_numbers[*index]);
    if (!markers[*index].name.empty()) {
      label += ' ';
      label += markers[*index].name;
    }
  }
  return label;
}

std::string TimelinePanel::seconds_label(Pass &p, core::u64 frames) {
  const EditorTimeline &timeline = p.input.timeline;

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

void TimelinePanel::draw_rate_menu(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const EditorTimeline &timeline = p.input.timeline;
  TimelinePanelFrame &frame = p.frame;
  FrameScratch &scratch = p.scratch;
  auto &rate_popup = p.rate_popup;

  if (scratch.popup_pointer_blocked) {
    ctx.pop_pointer_block();
  }
  if (toolbar_.rate_menu_open) {
    draw_hanging_popup(ctx, rate_popup);
    const gui::Rect choices = gui::inset(rate_popup, 4.0F);
    for (std::size_t index = 0U; index < common_rates.size(); ++index) {
      const RateChoice &choice = common_rates[index];
      const gui::Rect row = gui::list_row(choices, index, 21.0F);
      const gui::Interaction interaction =
          ctx.grab(mix(id_, "show rate choice", index), row);
      ctx.panel(row, same_rate(timeline, choice.rate)
                         ? ctx.theme().selection
                         : (interaction.hovered ? ctx.theme().widget_hover
                                                : ctx.theme().row));
      ctx.label(gui::inset(row, gui::Padding{7.0F, 0.0F, 0.0F, 0.0F}),
                choice.label,
                same_rate(timeline, choice.rate) ? ctx.theme().text
                                                 : ctx.theme().text_muted);
      if (interaction.clicked) {
        if (!same_rate(timeline, choice.rate)) {
          frame.show_rate = choice.rate;
        }
        toolbar_.rate_menu_open = false;
      }
    }
  }

}

} // namespace mediaserver::editor
