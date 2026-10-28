#include "timeline_internal.hpp"

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

// The Timeline's keyboard editing beyond single-key commands: nudging what is
// selected by a frame with the active tool, trimming the selected clips to the
// playhead, and the reference sheet that lists every key the panel answers.
namespace mediaserver::editor {
using namespace timeline;

namespace {

struct Shortcut final {
  std::string_view keys;
  std::string_view action;
};

struct ShortcutGroup final {
  std::string_view title;
  std::span<const Shortcut> entries;
};

constexpr std::array<Shortcut, 11U> playback_keys{{
    {"Space", "Play forwards / pause"},
    {"J  K  L", "Play backwards, stop, play forwards"},
    {"Left  Right", "Step a frame (Shift: a second)"},
    {"Home  End", "Go to In / Out"},
    {"Up  Down", "Previous / next cut or marker"},
    {"[  ]", "Previous / next key on the selection"},
    {"Enter", "GO: run from a hold or to the next cue"},
    {"I  O", "Set In / Out at the playhead"},
    {"X", "Set In/Out around the selected clips"},
    {"M", "Add a marker, or name the one here"},
    {"F", "Match frame in the Asset Viewer"},
}};

constexpr std::array<Shortcut, 11U> editing_keys{{
    {"V B N Y U C", "Select, Ripple, Roll, Slip, Slide, Razor"},
    {"Ctrl+K", "Cut the selected clips at the playhead"},
    {"Alt+Q  Alt+W", "Trim head / tail to the playhead, ripple"},
    {"Alt+Left  Right", "Nudge with the tool (Shift: a second)"},
    {"Shift+Delete", "Ripple delete the selected clips"},
    {",  .", "Insert / overwrite the clipboard"},
    {"Q  Shift+Q", "Add a hold / loop cue"},
    {"F9", "Easy Ease (Shift: in, Ctrl: out)"},
    {"Ctrl+C  Ctrl+V", "Copy / paste the selected keys"},
    {"F2", "Rename the selected layer or track"},
    {"Delete", "Delete the selected keys or markers"},
}};

constexpr std::array<Shortcut, 5U> view_keys{{
    {"=  -", "Zoom in / out about the playhead"},
    {"\\", "Zoom to fit the range"},
    {"S", "Snapping on / off"},
    {"Shift+K", "Auto Key on / off"},
    {"?", "This sheet"},
}};

} // namespace

void TimelinePanel::nudge_selection(Pass &p, core::i64 delta) {
  TimelinePanelFrame &frame = p.frame;
  const std::span<const ElementInfo> elements = p.input.elements;

  // Keys first: a key selection is the most specific thing selected, and
  // moving keys never moves the clips they ride on.
  if (!keys_.selected.empty()) {
    TimelineKeyBatchEdit batch{};
    batch.action = TimelineKeyBatchAction::Move;
    batch.keys = selected_addresses();
    batch.delta = delta;
    frame.key_batch = std::move(batch);
    return;
  }
  if (p.selected_clips.empty()) {
    return;
  }
  // Ripple, Roll and Slide move what follows on the clip's track, so two
  // selected clips on one track would move it twice: the earliest on each
  // track is the one the tool acts on.
  std::vector<doc::NodeId> lanes_taken;
  const auto first_on_lane = [&](const ElementInfo &clip) {
    if (std::find(lanes_taken.begin(), lanes_taken.end(),
                  clip.timeline_lane) != lanes_taken.end()) {
      return false;
    }
    lanes_taken.push_back(clip.timeline_lane);
    return true;
  };
  for (const ElementInfo *clip : p.selected_clips) {
    switch (edit_tool_) {
    case TimelineEditTool::Standard: {
      scene::ClipTiming moved = clip->timing;
      moved.start += delta;
      frame.clip_edits.push_back(TimelineClipEdit{clip->node, moved, {}});
      break;
    }
    case TimelineEditTool::RippleTrim:
      if (first_on_lane(*clip)) {
        frame.timeline_edits.emplace_back(TimelineRippleTrimRequest{
            clip->node, scene::TimelineTrimEdge::Tail, delta});
      }
      break;
    case TimelineEditTool::Roll: {
      // The cut after the clip, with the clip that starts on it.
      const auto end = exclusive_clip_end(clip->timing);
      const auto next = std::find_if(
          elements.begin(), elements.end(), [&](const ElementInfo &other) {
            return other.node != clip->node && editable_clip(other) &&
                   other.timeline_lane == clip->timeline_lane &&
                   end.has_value() && other.timing.start == *end;
          });
      if (next != elements.end() && first_on_lane(*clip)) {
        frame.timeline_edits.emplace_back(
            TimelineRollRequest{clip->node, next->node, delta});
      }
      break;
    }
    case TimelineEditTool::Slip:
      frame.timeline_edits.emplace_back(TimelineSlipRequest{clip->node, delta});
      break;
    case TimelineEditTool::Slide:
      if (first_on_lane(*clip)) {
        frame.timeline_edits.emplace_back(
            TimelineSlideRequest{clip->node, delta});
      }
      break;
    case TimelineEditTool::Razor:
      break;
    }
  }
}

void TimelinePanel::trim_to_playhead(Pass &p, scene::TimelineTrimEdge edge) {
  TimelinePanelFrame &frame = p.frame;
  const core::i64 playhead = p.playhead;
  std::vector<doc::NodeId> lanes_taken;
  for (const ElementInfo *clip : p.selected_clips) {
    const auto end = exclusive_clip_end(clip->timing);
    // The playhead must fall inside the clip with something left on both
    // sides of it, or there is nothing to trim to.
    if (!end.has_value() || playhead <= clip->timing.start ||
        playhead >= *end ||
        std::find(lanes_taken.begin(), lanes_taken.end(),
                  clip->timeline_lane) != lanes_taken.end()) {
      continue;
    }
    lanes_taken.push_back(clip->timeline_lane);
    // Head: the frames before the playhead go and the clip keeps its start,
    // so what was under the playhead now starts there. Tail: the frames from
    // the playhead on go, so the clip ends just before it.
    const core::i64 delta = edge == scene::TimelineTrimEdge::Head
                                ? playhead - clip->timing.start
                                : playhead - *end;
    frame.timeline_edits.emplace_back(
        TimelineRippleTrimRequest{clip->node, edge, delta});
  }
  if (lanes_taken.empty()) {
    clip_.status = "TRIM TO PLAYHEAD  /  Select a clip the playhead is inside";
    clip_.status_error = true;
    clip_.status_until = p.ctx.time_seconds() + 3.0;
  }
}

void TimelinePanel::draw_shortcut_sheet(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  if (!shortcut_sheet_open_) {
    return;
  }
  ctx.pop_pointer_block();
  // Any press closes it, as does Escape; ? toggles it from the keyboard.
  if (ctx.pointer_pressed() || ctx.secondary_pointer_pressed() ||
      ctx.key_pressed(gui::Key::Escape)) {
    shortcut_sheet_open_ = false;
    return;
  }

  const gui::Theme &theme = ctx.theme();
  const std::array<ShortcutGroup, 3U> groups{{
      {"PLAYBACK AND RANGE", playback_keys},
      {"EDITING", editing_keys},
      {"VIEW", view_keys},
  }};
  constexpr float line = 20.0F;
  constexpr float column_width = 360.0F;
  constexpr float keys_width = 118.0F;
  // Two columns where they fit - playback on the left, editing and view on
  // the right - and one otherwise.
  const bool two_columns = p.bounds.width >= column_width * 2.0F + 48.0F;
  const auto group_height = [&](const ShortcutGroup &group) {
    return line * (static_cast<float>(group.entries.size()) + 1.5F);
  };
  const float left_height = group_height(groups[0]);
  const float right_height = group_height(groups[1]) + group_height(groups[2]);
  const float width = two_columns ? column_width * 2.0F + 32.0F
                                  : std::min(column_width + 16.0F,
                                             p.bounds.width - 16.0F);
  const float height =
      28.0F + (two_columns ? std::max(left_height, right_height)
                           : left_height + right_height) + 8.0F;
  const gui::Rect sheet{
      p.bounds.x + std::max((p.bounds.width - width) * 0.5F, 8.0F),
      p.bounds.y + std::max((p.bounds.height - height) * 0.5F, 8.0F),
      width, std::min(height, p.bounds.height - 16.0F)};

  ctx.panel(sheet, theme.background);
  ctx.outline(sheet, theme.border);
  ctx.label(gui::Rect{sheet.x + 12.0F, sheet.y + 4.0F, sheet.width - 24.0F,
                      line},
            "TIMELINE KEYS  -  press anywhere or Esc to close", theme.text);
  ctx.push_clip(sheet);
  float x = sheet.x + 12.0F;
  float y = sheet.y + 28.0F;
  for (std::size_t index = 0U; index < groups.size(); ++index) {
    if (two_columns && index == 1U) {
      x = sheet.x + 16.0F + column_width + 8.0F;
      y = sheet.y + 28.0F;
    }
    const ShortcutGroup &group = groups[index];
    ctx.label(gui::Rect{x, y, column_width, line}, group.title, theme.accent);
    y += line;
    for (const Shortcut &entry : group.entries) {
      ctx.label(gui::Rect{x, y, keys_width, line}, entry.keys, theme.playhead);
      ctx.label(gui::Rect{x + keys_width, y, column_width - keys_width, line},
                entry.action, theme.text_muted);
      y += line;
    }
    y += line * 0.5F;
  }
  ctx.pop_clip();
}

} // namespace mediaserver::editor
