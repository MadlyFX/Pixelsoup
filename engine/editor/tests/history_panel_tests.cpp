#include <mediaserver/editor/history_panel.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <array>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// The History strip's proof, with no window: which request a click on a step,
// Start, Undo or Redo hands the shell, when an on-air jump asks twice, and
// which history it shows.
namespace {

namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define HISTORY_CHECK(...)                                                     \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] editor::HistoryStep step(std::uint64_t id, bool applied,
                                       std::vector<doc::NodeId> nodes = {}) {
  editor::HistoryStep out{};
  out.step = id;
  out.label = "Step " + std::to_string(id);
  out.applied = applied;
  out.nodes = std::move(nodes);
  return out;
}

// With the empty test font every label measures zero. At 800x60 the row is
// y=16..44; Hide is x=8..36, Scene x=40..64, Compositor x=68..92, Undo
// x=102..130, Redo x=132..160, and the steps start at x=170, 40 wide with 10
// between: Start x=170..210, then x=220, 270, 320.
constexpr float row = 30.0F;
constexpr float hide_x = 22.0F;
constexpr float scene_x = 52.0F;
constexpr float compositor_x = 80.0F;
constexpr float undo_x = 116.0F;
constexpr float redo_x = 146.0F;
constexpr float start_x = 190.0F;
[[nodiscard]] constexpr float step_x(std::size_t index) noexcept {
  return 240.0F + 50.0F * static_cast<float>(index);
}

struct Harness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::HistoryPanel panel{gui::ui_id("history")};
  editor::HistoryState state{};
  editor::HistoryContext active{editor::HistoryContext::Scene};
  bool on_air{};
  double now{};
  gui::Rect bounds{0.0F, 0.0F, 800.0F, 60.0F};

  Harness() {
    // Two applied steps and one undone, in each context with distinct ids.
    state.scene.steps = {step(11U, true, {doc::NodeId{501U}}),
                         step(12U, true, {doc::NodeId{502U}, doc::NodeId{503U}}),
                         step(13U, false)};
    state.scene.revision = 7U;
    state.compositor.steps = {step(21U, true), step(22U, true),
                              step(23U, true)};
    state.compositor.revision = 9U;
  }

  editor::HistoryPanelFrame frame(float x, float y, bool down) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    ctx.new_frame(now);
    return panel.draw(ctx, bounds, state, active, on_air);
  }

  editor::HistoryPanelFrame click(float x, float y) {
    static_cast<void>(frame(x, y, false));
    static_cast<void>(frame(x, y, true));
    return frame(x, y, false);
  }
};

void a_jump_crosses_every_step_between_here_and_there() {
  const Harness h;
  const editor::HistoryContextState &scene = h.state.scene;
  HISTORY_CHECK(editor::history_jump(scene, std::nullopt) ==
                editor::HistoryJump{0U, 2U});
  HISTORY_CHECK(editor::history_jump(scene, 0U) == editor::HistoryJump{11U, 1U});
  HISTORY_CHECK(editor::history_jump(scene, 1U) == editor::HistoryJump{12U, 0U});
  HISTORY_CHECK(editor::history_jump(scene, 2U) == editor::HistoryJump{13U, 1U});
  HISTORY_CHECK(editor::history_jump(scene, 3U) == editor::HistoryJump{});
}

void clicking_a_step_asks_to_jump_there() {
  Harness h;
  const auto back = h.click(start_x, row);
  HISTORY_CHECK(back.request.has_value());
  HISTORY_CHECK(back.request->action == editor::HistoryAction::Jump);
  HISTORY_CHECK(back.request->context == editor::HistoryContext::Scene);
  HISTORY_CHECK(back.request->target_step == 0U);
  HISTORY_CHECK(back.request->transaction_count == 2U);

  const auto forward = h.click(step_x(2U), row);
  HISTORY_CHECK(forward.request.has_value());
  HISTORY_CHECK(forward.request->target_step == 13U);
  HISTORY_CHECK(forward.request->transaction_count == 1U);

  // The current step is where the history already is.
  HISTORY_CHECK(!h.click(step_x(1U), row).request.has_value());
}

void undo_and_redo_step_once_and_only_when_they_can() {
  Harness h;
  const auto undo = h.click(undo_x, row);
  HISTORY_CHECK(undo.request.has_value());
  HISTORY_CHECK(undo.request->action == editor::HistoryAction::Undo);
  HISTORY_CHECK(undo.request->transaction_count == 1U);
  const auto redo = h.click(redo_x, row);
  HISTORY_CHECK(redo.request.has_value());
  HISTORY_CHECK(redo.request->action == editor::HistoryAction::Redo);

  // Everything applied: nothing to redo.
  h.active = editor::HistoryContext::Compositor;
  HISTORY_CHECK(!h.click(redo_x, row).request.has_value());
  const auto compositor_undo = h.click(undo_x, row);
  HISTORY_CHECK(compositor_undo.request.has_value());
  HISTORY_CHECK(compositor_undo.request->context ==
                editor::HistoryContext::Compositor);
}

void an_on_air_jump_of_several_steps_asks_twice() {
  Harness h;
  h.on_air = true;
  // One step on air goes straight through.
  HISTORY_CHECK(h.click(step_x(2U), row).request.has_value());

  // Two steps arm first, then go on the second click.
  HISTORY_CHECK(!h.click(start_x, row).request.has_value());
  const auto confirmed = h.click(start_x, row);
  HISTORY_CHECK(confirmed.request.has_value());
  HISTORY_CHECK(confirmed.request->transaction_count == 2U);

  // An arm left too long lapses, and the next click arms again.
  HISTORY_CHECK(!h.click(start_x, row).request.has_value());
  h.now = 5.0;
  HISTORY_CHECK(!h.click(start_x, row).request.has_value());
  HISTORY_CHECK(h.click(start_x, row).request.has_value());

  // Off air the same jump is one click.
  h.on_air = false;
  HISTORY_CHECK(h.click(start_x, row).request.has_value());
}

void the_strip_follows_the_last_edit_unless_pinned() {
  Harness h;
  HISTORY_CHECK(h.panel.shown(editor::HistoryContext::Scene) ==
                editor::HistoryContext::Scene);
  static_cast<void>(h.click(compositor_x, row));
  HISTORY_CHECK(h.panel.shown(editor::HistoryContext::Scene) ==
                editor::HistoryContext::Compositor);
  // Pinned: the compositor's Start crosses its three steps.
  const auto pinned = h.click(start_x, row);
  HISTORY_CHECK(pinned.request.has_value());
  HISTORY_CHECK(pinned.request->context == editor::HistoryContext::Compositor);
  HISTORY_CHECK(pinned.request->transaction_count == 3U);

  // A click on the pinned context lets go; the strip follows again.
  static_cast<void>(h.click(compositor_x, row));
  HISTORY_CHECK(h.panel.shown(editor::HistoryContext::Scene) ==
                editor::HistoryContext::Scene);
  static_cast<void>(h.click(scene_x, row));
  HISTORY_CHECK(h.panel.shown(editor::HistoryContext::Compositor) ==
                editor::HistoryContext::Scene);
  h.panel.reset();
  HISTORY_CHECK(h.panel.shown(editor::HistoryContext::Compositor) ==
                editor::HistoryContext::Compositor);
}

void hovering_a_step_reports_what_it_touched() {
  Harness h;
  const auto hovered = h.frame(step_x(1U), row, false);
  HISTORY_CHECK(hovered.hovered_nodes ==
                std::vector{doc::NodeId{502U}, doc::NodeId{503U}});
  HISTORY_CHECK(h.frame(start_x, row, false).hovered_nodes.empty());
  HISTORY_CHECK(h.frame(700.0F, row, false).hovered_nodes.empty());
}

void the_chevron_asks_to_hide_the_strip() {
  Harness h;
  const auto hidden = h.click(hide_x, row);
  HISTORY_CHECK(hidden.hide);
  HISTORY_CHECK(!hidden.request.has_value());
  HISTORY_CHECK(!h.click(start_x, row).hide);

  // While hidden, the status bar's chip is the same toggle pointing up.
  const gui::Rect chip{8.0F, 0.0F, 90.0F, 24.0F};
  const auto toggle = [&](bool down) {
    h.input.new_frame();
    h.input.set_mouse_position(30.0F, 12.0F);
    h.input.set_mouse_button(gui::MouseButton::Left, down);
    h.ctx.new_frame();
    return editor::history_toggle(h.ctx, gui::ui_id("reveal"), chip, false,
                                  "History");
  };
  HISTORY_CHECK(!toggle(false));
  HISTORY_CHECK(!toggle(true));
  HISTORY_CHECK(toggle(false));
}

void a_long_history_scrolls_by_its_bar() {
  Harness h;
  // Thirty steps, the last five undone: 31 chips at 50 apiece overflow the
  // 622 units from x=170 to the card's inset right edge at x=792.
  h.state.scene.steps.clear();
  for (std::uint64_t id = 1U; id <= 30U; ++id) {
    h.state.scene.steps.push_back(step(id, id <= 25U));
  }
  h.state.scene.revision = 8U;
  constexpr float bar = 58.0F;
  // Press the track's left end: the thumb comes there and the strip starts
  // at Start, so Start is where it always is.
  static_cast<void>(h.frame(175.0F, bar, false));
  static_cast<void>(h.frame(175.0F, bar, true));
  static_cast<void>(h.frame(175.0F, bar, false));
  const auto start = h.click(start_x, row);
  HISTORY_CHECK(start.request.has_value());
  HISTORY_CHECK(start.request->target_step == 0U);
  HISTORY_CHECK(start.request->transaction_count == 25U);

  // Drag from there to the right end: the last step comes into view at the
  // card's right edge.
  static_cast<void>(h.frame(175.0F, bar, false));
  static_cast<void>(h.frame(175.0F, bar, true));
  static_cast<void>(h.frame(480.0F, bar, true));
  static_cast<void>(h.frame(791.0F, bar, true));
  static_cast<void>(h.frame(791.0F, bar, false));
  const auto last = h.click(775.0F, row);
  HISTORY_CHECK(last.request.has_value());
  HISTORY_CHECK(last.request->target_step == 30U);
  HISTORY_CHECK(last.request->transaction_count == 5U);
}

void a_short_history_has_no_bar() {
  Harness h;
  // Three steps fit; a press along the bottom edge does nothing and the
  // steps stay where they are.
  static_cast<void>(h.click(700.0F, 58.0F));
  const auto back = h.click(start_x, row);
  HISTORY_CHECK(back.request.has_value());
  HISTORY_CHECK(back.request->transaction_count == 2U);
}

void an_empty_history_offers_nothing() {
  Harness h;
  h.state = {};
  HISTORY_CHECK(!h.click(start_x, row).request.has_value());
  HISTORY_CHECK(!h.click(undo_x, row).request.has_value());
  HISTORY_CHECK(!h.click(redo_x, row).request.has_value());
}

struct Case final {
  const char *name;
  void (*run)();
};

constexpr std::array cases{
    Case{"a jump crosses every step between here and there",
         &a_jump_crosses_every_step_between_here_and_there},
    Case{"clicking a step asks to jump there",
         &clicking_a_step_asks_to_jump_there},
    Case{"undo and redo step once and only when they can",
         &undo_and_redo_step_once_and_only_when_they_can},
    Case{"an on-air jump of several steps asks twice",
         &an_on_air_jump_of_several_steps_asks_twice},
    Case{"the strip follows the last edit unless pinned",
         &the_strip_follows_the_last_edit_unless_pinned},
    Case{"hovering a step reports what it touched",
         &hovering_a_step_reports_what_it_touched},
    Case{"the chevron asks to hide the strip",
         &the_chevron_asks_to_hide_the_strip},
    Case{"a long history scrolls by its bar",
         &a_long_history_scrolls_by_its_bar},
    Case{"a short history has no bar", &a_short_history_has_no_bar},
    Case{"an empty history offers nothing", &an_empty_history_offers_nothing},
};

} // namespace

int main() {
  std::size_t failures{};
  for (const Case &test : cases) {
    try {
      test.run();
      std::cout << "[pass] " << test.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << test.name << ": " << error.what() << '\n';
    }
  }
  std::cout << cases.size() - failures << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
