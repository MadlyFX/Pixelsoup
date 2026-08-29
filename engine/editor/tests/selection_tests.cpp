#include <mediaserver/editor/selection.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define UI_CHECK(...)                                                          \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

void membership_is_unique_and_sorted_without_losing_primary() {
  editor::NodeSelection selection;
  selection.toggle(doc::NodeId{30U});
  selection.toggle(doc::NodeId{10U});
  selection.toggle(doc::NodeId{20U});

  constexpr std::array expected{doc::NodeId{10U}, doc::NodeId{20U},
                                doc::NodeId{30U}};
  UI_CHECK(std::ranges::equal(selection.members(), expected));
  UI_CHECK(selection.primary() == doc::NodeId{20U});
}

void ctrl_toggle_removes_exactly_one_member() {
  editor::NodeSelection selection;
  selection.replace(doc::NodeId{20U});
  selection.toggle(doc::NodeId{10U});
  selection.toggle(doc::NodeId{30U});
  selection.toggle(doc::NodeId{10U});

  constexpr std::array expected{doc::NodeId{20U}, doc::NodeId{30U}};
  UI_CHECK(std::ranges::equal(selection.members(), expected));
  UI_CHECK(selection.primary() == doc::NodeId{30U});

  selection.toggle(doc::NodeId{30U});
  UI_CHECK(selection.members().size() == 1U);
  UI_CHECK(selection.members().front() == doc::NodeId{20U});
  UI_CHECK(selection.primary() == doc::NodeId{20U});
}

void shift_range_uses_visible_order_but_stores_canonical_membership() {
  editor::NodeSelection selection;
  selection.replace(doc::NodeId{30U});
  constexpr std::array visible{doc::NodeId{30U}, doc::NodeId{10U},
                               doc::NodeId{40U}, doc::NodeId{20U}};

  selection.apply(editor::SelectionAction::Range, doc::NodeId{40U}, visible);
  constexpr std::array first_expected{doc::NodeId{10U}, doc::NodeId{30U},
                                      doc::NodeId{40U}};
  UI_CHECK(std::ranges::equal(selection.members(), first_expected));
  UI_CHECK(selection.primary() == doc::NodeId{40U});

  // The original anchor remains stable while the clicked primary moves.
  selection.apply(editor::SelectionAction::Range, doc::NodeId{20U}, visible);
  constexpr std::array second_expected{doc::NodeId{10U}, doc::NodeId{20U},
                                       doc::NodeId{30U}, doc::NodeId{40U}};
  UI_CHECK(std::ranges::equal(selection.members(), second_expected));
  UI_CHECK(selection.primary() == doc::NodeId{20U});
}

void a_hidden_range_anchor_falls_back_to_the_clicked_row() {
  editor::NodeSelection selection;
  selection.replace(doc::NodeId{90U});
  constexpr std::array visible{doc::NodeId{10U}, doc::NodeId{20U}};
  selection.apply(editor::SelectionAction::Range, doc::NodeId{20U}, visible);

  UI_CHECK(selection.members().size() == 1U);
  UI_CHECK(selection.primary() == doc::NodeId{20U});
}

void modifier_resolution_never_conflates_ctrl_with_shift() {
  UI_CHECK(editor::selection_action(true, false, true) ==
           editor::SelectionAction::Toggle);
  UI_CHECK(editor::selection_action(false, true, true) ==
           editor::SelectionAction::Range);
  UI_CHECK(editor::selection_action(false, true, false) ==
           editor::SelectionAction::Replace);
  UI_CHECK(editor::selection_action(true, true, true) ==
           editor::SelectionAction::Range);
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 5U> cases{{
    {"membership is unique and sorted without losing primary",
     &membership_is_unique_and_sorted_without_losing_primary},
    {"Ctrl toggle removes exactly one member",
     &ctrl_toggle_removes_exactly_one_member},
    {"Shift range uses visible order but stores canonical membership",
     &shift_range_uses_visible_order_but_stores_canonical_membership},
    {"a hidden range anchor falls back to the clicked row",
     &a_hidden_range_anchor_falls_back_to_the_clicked_row},
    {"modifier resolution never conflates Ctrl with Shift",
     &modifier_resolution_never_conflates_ctrl_with_shift},
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
