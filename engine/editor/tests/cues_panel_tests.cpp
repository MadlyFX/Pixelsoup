#include <mediaserver/editor/cues_panel.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The Cue Stack's proof, with no window: what it reports to the shell for a
// GO, a row, a cell, a key, and a footer button.
namespace {

namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace scene = mediaserver::scene;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define CUE_CHECK(...)                                                         \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] scene::TimelineMarker marker(std::uint64_t id, std::int64_t frame,
                                           scene::CueAction cue,
                                           std::string name = {}) {
  scene::TimelineMarker out{};
  out.id = doc::NodeId{id};
  out.scene = scene::SceneId{100U};
  out.frame = frame;
  out.name = std::move(name);
  out.cue = cue;
  return out;
}

// At 800x400 the header is y=0..30 with ARMED at x=736..792 and MARKERS at
// x=676..732; GO is x=8..268, y=36..88; rows start at y=114, 26px each; the
// name cell starts at x=66, the action cell at x=315, a Loop's WAIT at
// x=479..531 and FADE at x=535..587, and the footer buttons sit at
// y=373..395 from x=8.
struct Harness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::CuesPanel panel{gui::ui_id("cues")};
  editor::EditorTimeline timeline{50U, 50U, 0U, 200U, false, false, false};
  std::vector<scene::TimelineMarker> markers{
      marker(31U, 10, scene::CueAction::Hold, "Doors"),
      marker(32U, 20, scene::CueAction::None, "Note to self"),
      marker(33U, 90, scene::CueAction::Loop, "Vamp"),
  };
  std::vector<doc::NodeId> selected;
  gui::Rect bounds{0.0F, 0.0F, 800.0F, 400.0F};

  editor::CuesPanelFrame frame(float x, float y, bool down,
                               std::string_view text = {}) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    if (!text.empty()) {
      input.add_text(text);
    }
    ctx.new_frame();
    return panel.draw(ctx, bounds, timeline, markers, selected);
  }

  editor::CuesPanelFrame click(float x, float y) {
    static_cast<void>(frame(x, y, false));
    static_cast<void>(frame(x, y, true));
    return frame(x, y, false);
  }

  editor::CuesPanelFrame key(gui::Key pressed) {
    input.new_frame();
    input.set_mouse_button(gui::MouseButton::Left, false);
    input.add_key(pressed);
    ctx.new_frame();
    return panel.draw(ctx, bounds, timeline, markers, selected);
  }
};

void go_is_one_big_button() {
  Harness h;
  h.timeline.held_cue = doc::NodeId{31U};
  h.timeline.next_cue = doc::NodeId{33U};
  const editor::CuesPanelFrame went = h.click(138.0F, 62.0F);
  CUE_CHECK(went.transport == editor::TransportRequest::Go);
  CUE_CHECK(went.marker_edits.empty());
}

void the_list_holds_cues_in_show_order_and_markers_on_request() {
  Harness h;
  // Plain markers are navigation; the stack lists cues alone by default, so
  // the second row is the Loop.
  const editor::CuesPanelFrame second = h.click(150.0F, 153.0F);
  CUE_CHECK(second.selected == doc::NodeId{33U});

  const editor::CuesPanelFrame shown = h.click(700.0F, 15.0F);
  CUE_CHECK(h.panel.showing_markers());
  CUE_CHECK(!shown.selected.has_value());
  const editor::CuesPanelFrame plain = h.click(150.0F, 153.0F);
  CUE_CHECK(plain.selected == doc::NodeId{32U});
}

void cells_recolour_and_change_what_a_cue_does() {
  Harness h;
  const editor::CuesPanelFrame action = h.click(330.0F, 127.0F);
  CUE_CHECK(action.marker_edits.size() == 1U);
  CUE_CHECK(action.marker_edits.front().action ==
            editor::TimelineMarkerAction::Update);
  CUE_CHECK(action.marker_edits.front().marker.id == doc::NodeId{31U});
  CUE_CHECK(action.marker_edits.front().marker.cue == scene::CueAction::Loop);
  // Everything else about the cue travels unchanged.
  CUE_CHECK(action.marker_edits.front().marker.frame == 10);
  CUE_CHECK(action.marker_edits.front().marker.name == "Doors");

  const editor::CuesPanelFrame swatch = h.click(55.0F, 127.0F);
  CUE_CHECK(swatch.marker_edits.size() == 1U);
  CUE_CHECK(swatch.marker_edits.front().marker.color == 1U);
}

void a_double_click_on_the_name_renames_in_place() {
  Harness h;
  static_cast<void>(h.click(120.0F, 127.0F));
  static_cast<void>(h.click(120.0F, 127.0F));
  const editor::CuesPanelFrame typing = h.frame(0.0F, 0.0F, false, " A");
  CUE_CHECK(typing.editing);
  CUE_CHECK(typing.marker_edits.empty());
  const editor::CuesPanelFrame committed = h.key(gui::Key::Enter);
  CUE_CHECK(committed.marker_edits.size() == 1U);
  CUE_CHECK(committed.marker_edits.front().marker.name == "Doors A");
  // The Enter that committed the name is not also a GO.
  CUE_CHECK(committed.transport == editor::TransportRequest::None);
}

void a_double_click_elsewhere_on_a_row_goes_to_the_cue() {
  Harness h;
  static_cast<void>(h.click(420.0F, 153.0F));
  const editor::CuesPanelFrame went = h.click(420.0F, 153.0F);
  CUE_CHECK(went.seek_to_frame == std::optional<std::uint64_t>{90U});
}

void space_and_enter_are_go_and_delete_removes_the_chosen_cue() {
  Harness h;
  h.selected = {doc::NodeId{33U}};
  // Focus the panel without hitting a control.
  static_cast<void>(h.click(600.0F, 300.0F));
  const editor::CuesPanelFrame space = h.frame(600.0F, 300.0F, false, " ");
  CUE_CHECK(space.transport == editor::TransportRequest::Go);
  const editor::CuesPanelFrame enter = h.key(gui::Key::Enter);
  CUE_CHECK(enter.transport == editor::TransportRequest::Go);
  const editor::CuesPanelFrame up = h.key(gui::Key::Up);
  CUE_CHECK(up.selected == doc::NodeId{31U});
  const editor::CuesPanelFrame removed = h.key(gui::Key::Delete);
  CUE_CHECK(removed.consume_delete);
  CUE_CHECK(removed.marker_edits.size() == 1U);
  CUE_CHECK(removed.marker_edits.front().action ==
            editor::TimelineMarkerAction::Delete);
  CUE_CHECK(removed.marker_edits.front().marker.id == doc::NodeId{33U});
}

void the_footer_adds_cues_at_the_playhead_and_the_header_arms_them() {
  Harness h;
  const editor::CuesPanelFrame hold = h.click(18.0F, 384.0F);
  CUE_CHECK(hold.marker_edits.size() == 1U);
  CUE_CHECK(hold.marker_edits.front().action ==
            editor::TimelineMarkerAction::Create);
  CUE_CHECK(hold.marker_edits.front().marker.frame == 50);
  CUE_CHECK(hold.marker_edits.front().marker.cue == scene::CueAction::Hold);

  const editor::CuesPanelFrame loop = h.click(42.0F, 384.0F);
  CUE_CHECK(loop.marker_edits.size() == 1U);
  CUE_CHECK(loop.marker_edits.front().marker.cue == scene::CueAction::Loop);

  const editor::CuesPanelFrame plain = h.click(66.0F, 384.0F);
  CUE_CHECK(plain.marker_edits.size() == 1U);
  CUE_CHECK(plain.marker_edits.front().marker.cue == scene::CueAction::None);
  CUE_CHECK(plain.marker_edits.front().marker.name == "Marker 2");

  const editor::CuesPanelFrame disarmed = h.click(760.0F, 15.0F);
  CUE_CHECK(disarmed.cues_armed == std::optional<bool>{false});
}

void loop_timing_reads_and_writes_as_seconds() {
  constexpr std::uint32_t most = 60'000U;
  CUE_CHECK(editor::parse_cue_seconds("2", most) ==
            std::optional<std::uint32_t>{2'000U});
  CUE_CHECK(editor::parse_cue_seconds("2.5s", most) ==
            std::optional<std::uint32_t>{2'500U});
  CUE_CHECK(editor::parse_cue_seconds(" 1.25 S ", most) ==
            std::optional<std::uint32_t>{1'250U});
  CUE_CHECK(editor::parse_cue_seconds("250ms", most) ==
            std::optional<std::uint32_t>{250U});
  CUE_CHECK(editor::parse_cue_seconds("0", most) ==
            std::optional<std::uint32_t>{0U});
  CUE_CHECK(!editor::parse_cue_seconds("", most));
  // Past the most a cue allows reads as the most; nonsense reads as nothing.
  CUE_CHECK(editor::parse_cue_seconds("120", most) ==
            std::optional<std::uint32_t>{most});
  CUE_CHECK(!editor::parse_cue_seconds("-1", most));
  CUE_CHECK(!editor::parse_cue_seconds("soon", most));
  CUE_CHECK(!editor::parse_cue_seconds("1.5x", most));
  CUE_CHECK(editor::format_cue_seconds(0U) == "-");
  CUE_CHECK(editor::format_cue_seconds(1'500U) == "1.5s");
  CUE_CHECK(editor::format_cue_seconds(250U) == "0.25s");
  CUE_CHECK(editor::format_cue_seconds(1'234U) == "1.234s");
}

void a_loop_wait_scrubs_and_lands_once() {
  Harness h;
  // Ten steps of four pixels: a second.
  static_cast<void>(h.frame(500.0F, 153.0F, false));
  static_cast<void>(h.frame(500.0F, 153.0F, true));
  const editor::CuesPanelFrame scrubbing = h.frame(540.0F, 153.0F, true);
  CUE_CHECK(scrubbing.editing);
  CUE_CHECK(scrubbing.marker_edits.empty());
  const editor::CuesPanelFrame landed = h.frame(540.0F, 153.0F, false);
  CUE_CHECK(landed.marker_edits.size() == 1U);
  CUE_CHECK(landed.marker_edits.front().action ==
            editor::TimelineMarkerAction::Update);
  CUE_CHECK(landed.marker_edits.front().marker.id == doc::NodeId{33U});
  CUE_CHECK(landed.marker_edits.front().marker.loop_wait_ms == 1'000U);
  CUE_CHECK(landed.marker_edits.front().marker.loop_fade_ms == 0U);
  CUE_CHECK(landed.marker_edits.front().marker.name == "Vamp");

  // Scrubbed back below nothing it stops at nothing, and a plain click
  // only chooses the row.
  h.markers[2U].loop_wait_ms = 300U;
  static_cast<void>(h.frame(500.0F, 153.0F, false));
  static_cast<void>(h.frame(500.0F, 153.0F, true));
  static_cast<void>(h.frame(380.0F, 153.0F, true));
  const editor::CuesPanelFrame floored = h.frame(380.0F, 153.0F, false);
  CUE_CHECK(floored.marker_edits.size() == 1U);
  CUE_CHECK(floored.marker_edits.front().marker.loop_wait_ms == 0U);
  const editor::CuesPanelFrame chosen = h.click(500.0F, 153.0F);
  CUE_CHECK(chosen.marker_edits.empty());
  CUE_CHECK(chosen.selected == doc::NodeId{33U});
}

void a_double_click_types_a_loop_crossfade() {
  Harness h;
  static_cast<void>(h.click(560.0F, 153.0F));
  static_cast<void>(h.click(560.0F, 153.0F));
  const editor::CuesPanelFrame typing = h.frame(0.0F, 0.0F, false, "0.5");
  CUE_CHECK(typing.editing);
  CUE_CHECK(typing.marker_edits.empty());
  const editor::CuesPanelFrame committed = h.key(gui::Key::Enter);
  CUE_CHECK(committed.marker_edits.size() == 1U);
  CUE_CHECK(committed.marker_edits.front().marker.loop_fade_ms == 500U);
  CUE_CHECK(committed.transport == editor::TransportRequest::None);

  // Until the document shows it, the next edit builds on the one sent: a
  // wait set straight after keeps the crossfade.
  static_cast<void>(h.click(500.0F, 153.0F));
  static_cast<void>(h.click(500.0F, 153.0F));
  static_cast<void>(h.frame(0.0F, 0.0F, false, "2"));
  const editor::CuesPanelFrame waited = h.key(gui::Key::Enter);
  CUE_CHECK(waited.marker_edits.size() == 1U);
  CUE_CHECK(waited.marker_edits.front().marker.loop_wait_ms == 2'000U);
  CUE_CHECK(waited.marker_edits.front().marker.loop_fade_ms == 500U);

  // Enter on an empty field changes nothing.
  static_cast<void>(h.click(560.0F, 153.0F));
  static_cast<void>(h.click(560.0F, 153.0F));
  CUE_CHECK(h.key(gui::Key::Enter).marker_edits.empty());

  // A Hold has no timing to set: its row is just a row there.
  const editor::CuesPanelFrame hold = h.click(500.0F, 127.0F);
  CUE_CHECK(hold.marker_edits.empty());
  CUE_CHECK(hold.selected == doc::NodeId{31U});
}

void a_narrow_panel_sets_the_chosen_loop_in_a_bar() {
  Harness h;
  h.bounds = gui::Rect{0.0F, 0.0F, 300.0F, 400.0F};
  // Too narrow for the columns, and nothing chosen: no timing anywhere.
  const editor::CuesPanelFrame idle = h.click(40.0F, 354.0F);
  CUE_CHECK(idle.marker_edits.empty());

  // Choosing the Loop puts its timing in a bar above the footer, y=340..368,
  // WAIT first from x=14.
  h.selected = {doc::NodeId{33U}};
  static_cast<void>(h.frame(40.0F, 354.0F, false));
  static_cast<void>(h.frame(40.0F, 354.0F, true));
  static_cast<void>(h.frame(80.0F, 354.0F, true));
  const editor::CuesPanelFrame landed = h.frame(80.0F, 354.0F, false);
  CUE_CHECK(landed.marker_edits.size() == 1U);
  CUE_CHECK(landed.marker_edits.front().marker.id == doc::NodeId{33U});
  CUE_CHECK(landed.marker_edits.front().marker.loop_wait_ms == 1'000U);

  // A chosen Hold has no timing, so no bar.
  h.selected = {doc::NodeId{31U}};
  static_cast<void>(h.frame(40.0F, 354.0F, false));
  static_cast<void>(h.frame(40.0F, 354.0F, true));
  static_cast<void>(h.frame(80.0F, 354.0F, true));
  CUE_CHECK(h.frame(80.0F, 354.0F, false).marker_edits.empty());
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 11U> cases{{
    {"GO is one big button", &go_is_one_big_button},
    {"the list holds cues in show order and markers on request",
     &the_list_holds_cues_in_show_order_and_markers_on_request},
    {"cells recolour and change what a cue does",
     &cells_recolour_and_change_what_a_cue_does},
    {"a double-click on the name renames in place",
     &a_double_click_on_the_name_renames_in_place},
    {"a double-click elsewhere on a row goes to the cue",
     &a_double_click_elsewhere_on_a_row_goes_to_the_cue},
    {"Space and Enter are GO and Delete removes the chosen cue",
     &space_and_enter_are_go_and_delete_removes_the_chosen_cue},
    {"the footer adds cues at the playhead and the header arms them",
     &the_footer_adds_cues_at_the_playhead_and_the_header_arms_them},
    {"loop timing reads and writes as seconds",
     &loop_timing_reads_and_writes_as_seconds},
    {"a loop wait scrubs and lands once", &a_loop_wait_scrubs_and_lands_once},
    {"a double-click types a loop crossfade",
     &a_double_click_types_a_loop_crossfade},
    {"a narrow panel sets the chosen loop in a bar",
     &a_narrow_panel_sets_the_chosen_loop_in_a_bar},
}};

} // namespace

int main() {
  std::size_t failures = 0U;
  for (const Case &entry : cases) {
    try {
      entry.function();
      std::cout << "[pass] " << entry.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
    }
  }
  std::cout << (cases.size() - failures) << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
