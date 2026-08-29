#include <mediaserver/editor/selection_sets_panel.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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

#define PANEL_CHECK(...)                                                       \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

struct Harness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::SelectionSetsPanel panel{gui::ui_id("selection-sets.test")};
  std::vector<scene::SelectionSetSummary> sets{
      scene::SelectionSetSummary{doc::NodeId{100U}, doc::NodeId{1U}, "Opening",
                                 3U, 1U},
      scene::SelectionSetSummary{doc::NodeId{200U}, doc::NodeId{1U}, "Finale",
                                 2U, 0U},
  };
  doc::NodeId selected{};
  std::size_t scene_selection_count{2U};
  bool authoring_enabled{true};
  double time{};
  static constexpr gui::Rect bounds{0.0F, 0.0F, 320.0F, 260.0F};

  editor::SelectionSetPanelFrame frame(float x, float y, bool down,
                                       std::string_view text = {}) {
    time += 0.05;
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.add_text(text);
    ctx.new_frame(time);
    auto result = panel.draw(ctx, bounds, sets, selected, scene_selection_count,
                             authoring_enabled);
    if (result.selection_changed) {
      selected = result.selected;
    }
    return result;
  }

  editor::SelectionSetPanelFrame click(float x, float y) {
    static_cast<void>(frame(x, y, false));
    static_cast<void>(frame(x, y, true));
    return frame(x, y, false);
  }
};

void row_selection_is_a_distinct_typed_id() {
  Harness harness;
  const auto result = harness.click(80.0F, 63.0F);
  PANEL_CHECK(result.selection_changed);
  PANEL_CHECK(result.selected == doc::NodeId{100U});
  PANEL_CHECK(harness.selected == doc::NodeId{100U});
  PANEL_CHECK(!result.intent.has_value());
  PANEL_CHECK(harness.panel.edit_name() == "Opening");
}

void double_click_and_recall_button_emit_recall_without_scene_edits() {
  Harness harness;
  static_cast<void>(harness.click(80.0F, 63.0F));
  const auto doubled = harness.click(80.0F, 63.0F);
  PANEL_CHECK(doubled.intent.has_value());
  PANEL_CHECK(doubled.intent->action ==
              editor::SelectionSetPanelAction::Recall);
  PANEL_CHECK(doubled.intent->set == doc::NodeId{100U});

  // The dedicated button is the second footer action.
  const auto button = harness.click(120.0F, 244.0F);
  PANEL_CHECK(button.intent.has_value());
  PANEL_CHECK(button.intent->action == editor::SelectionSetPanelAction::Recall);
  PANEL_CHECK(button.intent->set == doc::NodeId{100U});
}

void save_and_replace_require_a_nonempty_scene_selection() {
  Harness harness;
  harness.scene_selection_count = 0U;
  PANEL_CHECK(!harness.click(278.0F, 38.0F).intent.has_value());
  harness.scene_selection_count = 2U;
  const auto saved = harness.click(278.0F, 38.0F);
  PANEL_CHECK(saved.intent.has_value());
  PANEL_CHECK(saved.intent->action == editor::SelectionSetPanelAction::SaveNew);
  PANEL_CHECK(saved.intent->name == "Selection Set");

  static_cast<void>(harness.click(80.0F, 63.0F));
  const auto replaced = harness.click(40.0F, 244.0F);
  PANEL_CHECK(replaced.intent.has_value());
  PANEL_CHECK(replaced.intent->action ==
              editor::SelectionSetPanelAction::ReplaceMembers);
  PANEL_CHECK(replaced.intent->set == doc::NodeId{100U});
}

void rename_and_delete_carry_only_the_selected_set_domain() {
  Harness harness;
  static_cast<void>(harness.click(80.0F, 86.0F));
  const auto renamed = harness.click(200.0F, 244.0F);
  PANEL_CHECK(renamed.intent.has_value());
  PANEL_CHECK(renamed.intent->action ==
              editor::SelectionSetPanelAction::Rename);
  PANEL_CHECK(renamed.intent->set == doc::NodeId{200U});
  PANEL_CHECK(renamed.intent->name == "Finale");

  const auto deleted = harness.click(280.0F, 244.0F);
  PANEL_CHECK(deleted.intent.has_value());
  PANEL_CHECK(deleted.intent->action ==
              editor::SelectionSetPanelAction::Delete);
  PANEL_CHECK(deleted.intent->set == doc::NodeId{200U});
}

void a_stale_panel_selection_clears_without_fabricating_an_action() {
  Harness harness;
  harness.selected = doc::NodeId{999U};
  const auto result = harness.frame(0.0F, 0.0F, false);
  PANEL_CHECK(result.selection_changed);
  PANEL_CHECK(result.selected == doc::NodeId{});
  PANEL_CHECK(!result.intent.has_value());
  PANEL_CHECK(harness.selected == doc::NodeId{});
}

void transition_lock_disables_authoring_but_keeps_recall_local() {
  Harness harness;
  harness.authoring_enabled = false;
  PANEL_CHECK(!harness.click(278.0F, 38.0F).intent.has_value());
  static_cast<void>(harness.click(80.0F, 63.0F));
  PANEL_CHECK(!harness.click(40.0F, 244.0F).intent.has_value());
  PANEL_CHECK(!harness.click(200.0F, 244.0F).intent.has_value());
  PANEL_CHECK(!harness.click(280.0F, 244.0F).intent.has_value());
  const auto recalled = harness.click(120.0F, 244.0F);
  PANEL_CHECK(recalled.intent.has_value());
  PANEL_CHECK(recalled.intent->action ==
              editor::SelectionSetPanelAction::Recall);
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 6U> cases{{
    {"row selection is a distinct typed id",
     &row_selection_is_a_distinct_typed_id},
    {"double click and recall button emit recall without scene edits",
     &double_click_and_recall_button_emit_recall_without_scene_edits},
    {"save and replace require a nonempty scene selection",
     &save_and_replace_require_a_nonempty_scene_selection},
    {"rename and delete carry only the selected set domain",
     &rename_and_delete_carry_only_the_selected_set_domain},
    {"a stale panel selection clears without fabricating an action",
     &a_stale_panel_selection_clears_without_fabricating_an_action},
    {"transition lock disables authoring but keeps recall local",
     &transition_lock_disables_authoring_but_keeps_recall_local},
}};

} // namespace

int main() {
  std::size_t failures{};
  for (const Case &test : cases) {
    try {
      std::cout << "[run ] " << test.name << std::endl;
      test.function();
      std::cout << "[pass] " << test.name << '\n';
    } catch (const std::exception &exception) {
      ++failures;
      std::cerr << "[fail] " << test.name << ": " << exception.what() << '\n';
    }
  }
  std::cout << (cases.size() - failures) << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
