#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/timeline_panel.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/scene/scene.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The Cue Stack: the show's cues as a list an operator runs from.
//
// The Timeline is where cues are placed; this is where they are called. The
// panel is built for the person standing at the desk during a take: one big
// GO, a line that says what the show is doing and what comes next, and the
// cues in show order with the held one, the looping one, and the next one
// marked so they can be found without reading. Everything it changes is the
// same marker edit the Timeline reports, so the two views never disagree.
namespace mediaserver::editor {

struct CuesPanelFrame final {
  std::vector<TimelineMarkerEdit> marker_edits;
  TransportRequest transport{TransportRequest::None};
  std::optional<core::u64> seek_to_frame;
  std::optional<bool> cues_armed;
  // A row was chosen; the shell mirrors it into the Timeline's marker
  // selection so both views point at the same cue.
  std::optional<doc::NodeId> selected;
  // A name or note field owns the keyboard.
  bool editing{};
  // Delete was aimed at the selected cue rather than at the scene selection.
  bool consume_delete{};
};

// A Loop's wait or crossfade as an operator types it: "2", "2.5", "2.5s",
// "250ms", or "0" for none. A duration past `maximum_ms` reads as the
// maximum; empty text, text that is not a duration, or a negative one reads
// as nothing.
[[nodiscard]] std::optional<core::u32>
parse_cue_seconds(std::string_view text, core::u32 maximum_ms) noexcept;
// "1.5s", "0.25s", or "-" for none.
[[nodiscard]] std::string format_cue_seconds(core::u32 milliseconds);

class CuesPanel final {
public:
  explicit CuesPanel(gui::UiId id) noexcept : id_(id) {}

  // `selected` is the Timeline's marker selection, so a cue chosen on the
  // timeline is the one highlighted here.
  [[nodiscard]] CuesPanelFrame
  draw(gui::UiContext &ctx, const gui::Rect &bounds,
       const EditorTimeline &timeline,
       std::span<const scene::TimelineMarker> markers,
       std::span<const doc::NodeId> selected);

  // A scene or project switch: no field may stay open on a cue that is gone.
  void reset() noexcept;

  [[nodiscard]] bool showing_markers() const noexcept {
    return show_markers_;
  }

private:
  gui::UiId id_{};
  float scroll_{};
  // Plain markers are navigation, not cues; they join the list on request.
  bool show_markers_{};
  bool focused_{};
  doc::NodeId name_node_{};
  std::string name_draft_;
  doc::NodeId note_node_{};
  std::string note_draft_;
  doc::NodeId click_node_{};
  double click_time_{};
  // A Loop's WAIT and FADE cells: a drag scrubs and lands once on release,
  // a double-click types seconds.
  enum class TimingField : core::u8 { None, Wait, Fade };
  doc::NodeId timing_node_{};
  TimingField timing_field_{TimingField::None};
  std::string timing_draft_;
  doc::NodeId scrub_node_{};
  TimingField scrub_field_{TimingField::None};
  float scrub_origin_x_{};
  core::u32 scrub_origin_ms_{};
  core::u32 scrub_ms_{};
  bool scrubbing_{};
  doc::NodeId timing_click_node_{};
  TimingField timing_click_field_{TimingField::None};
  double timing_click_time_{};
  // The last version of each cue this panel sent, until the document shows
  // it. Every edit carries the whole cue, so one made before the previous
  // edit has come back would otherwise put back what that one changed; the
  // list builds on what was sent, and shows it, until then.
  struct SentMarker final {
    scene::TimelineMarker marker;
    core::u32 draws_left{};
  };
  std::vector<SentMarker> sent_;
};

} // namespace mediaserver::editor
