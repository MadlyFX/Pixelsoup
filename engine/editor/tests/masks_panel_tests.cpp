#include <mediaserver/editor/masks_panel.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <array>
#include <cmath>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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

#define MASKS_PANEL_CHECK(...)                                                 \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] scene::MaskPoint point(mediaserver::core::u64 id,
                                     mediaserver::core::i64 order, float x,
                                     float y) {
  scene::MaskPoint result{};
  result.source = doc::NodeId{id};
  result.scene = doc::NodeId{1U};
  result.mask = doc::NodeId{22U};
  result.order = order;
  result.x = x;
  result.y = y;
  result.in_x = x;
  result.in_y = y;
  result.out_x = x;
  result.out_y = y;
  return result;
}

[[nodiscard]] scene::MaskInstance mask(mediaserver::core::u64 id,
                                       scene::MaskKind kind,
                                       mediaserver::core::i64 order,
                                       std::string name) {
  scene::MaskInstance result{};
  result.source = doc::NodeId{id};
  result.scene = doc::NodeId{1U};
  result.name = std::move(name);
  result.target = doc::NodeId{10U};
  result.kind = kind;
  result.combine = id == 20U ? scene::MaskCombine::Add
                             : scene::MaskCombine::Subtract;
  result.order = order;
  result.enabled = true;
  result.feather_canvas_px = 4.0F;
  result.expansion_canvas_px = -2.0F;
  result.opacity = 0.75F;
  result.bounds = scene::MaskBounds{0.1F, 0.2F, 0.9F, 0.8F};
  return result;
}

template <typename Intent>
[[nodiscard]] const Intent *intent_as(
    const editor::MasksPanelFrame &frame) {
  if (!frame.intent.has_value()) {
    return nullptr;
  }
  return std::get_if<Intent>(&*frame.intent);
}

struct Harness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::MasksPanel panel{gui::ui_id("masks.test")};
  editor::MasksPanelTarget target{
      doc::NodeId{1U}, doc::NodeId{10U},
      scene::MaskCoordinateSpace::SourceLayer};
  std::vector<scene::MaskInstance> masks;
  std::vector<scene::MaskDiagnostic> diagnostics;
  editor::MasksPanelSelection selection{
      doc::NodeId{1U}, doc::NodeId{10U}, doc::NodeId{20U}, {}};
  bool authoring_enabled{true};
  double time{};
  static constexpr gui::Rect bounds{0.0F, 0.0F, 420.0F, 430.0F};

  Harness() {
    masks.push_back(mask(20U, scene::MaskKind::Rectangle, 0, "Rectangle 1"));
    masks.push_back(mask(21U, scene::MaskKind::Ellipse, 1, "Ellipse 1"));
    scene::MaskInstance path =
        mask(22U, scene::MaskKind::Bezier, 2, "Path 1");
    path.points = {
        point(100U, 0, 0.0F, 0.0F), point(101U, 1, 1.0F, 0.0F),
        point(102U, 2, 1.0F, 1.0F), point(103U, 3, 0.0F, 1.0F)};
    masks.push_back(std::move(path));
    diagnostics.push_back(scene::MaskDiagnostic{
        scene::MaskDiagnosticKind::MalformedPath, doc::NodeId{22U},
        doc::NodeId{10U}});
  }

  editor::MasksPanelFrame frame(float x, float y, bool down,
                                gui::KeyMods modifiers = gui::KeyMods::None,
                                std::string_view typed = {}) {
    time += 0.05;
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.set_mods(modifiers);
    input.add_text(typed);
    ctx.new_frame(time);
    auto result = panel.draw(ctx, bounds, target, masks, diagnostics, selection,
                             authoring_enabled);
    if (result.selection_changed) {
      selection = result.selection;
    }
    return result;
  }

  editor::MasksPanelFrame click(
      float x, float y,
      gui::KeyMods modifiers = gui::KeyMods::None) {
    static_cast<void>(frame(x, y, false, modifiers));
    static_cast<void>(frame(x, y, true, modifiers));
    return frame(x, y, false, modifiers);
  }

  editor::MasksPanelFrame drag(float from_x, float y, float to_x) {
    static_cast<void>(frame(from_x, y, false));
    static_cast<void>(frame(from_x, y, true));
    auto result = frame(to_x, y, true);
    static_cast<void>(frame(to_x, y, false));
    return result;
  }

  // Row reordering is a vertical gesture, so it needs both axes.
  editor::MasksPanelFrame drag_to(float from_x, float from_y, float to_x,
                                  float to_y) {
    static_cast<void>(frame(from_x, from_y, false));
    static_cast<void>(frame(from_x, from_y, true));
    auto result = frame(to_x, to_y, true);
    static_cast<void>(frame(to_x, to_y, false));
    return result;
  }
};

void shape_tools_emit_explicit_target_space_and_familiar_modifiers() {
  Harness harness;
  const auto rectangle = harness.click(70.0F, 39.0F);
  const auto *rectangle_intent =
      intent_as<editor::MaskCreateIntent>(rectangle);
  MASKS_PANEL_CHECK(rectangle_intent != nullptr);
  MASKS_PANEL_CHECK(rectangle_intent->scene == doc::NodeId{1U});
  MASKS_PANEL_CHECK(rectangle_intent->target == doc::NodeId{10U});
  MASKS_PANEL_CHECK(rectangle_intent->coordinate_space ==
                    scene::MaskCoordinateSpace::SourceLayer);
  MASKS_PANEL_CHECK(rectangle_intent->tool ==
                    editor::MaskCreateTool::Rectangle);

  const auto ellipse = harness.click(
      210.0F, 39.0F, gui::KeyMods::Shift | gui::KeyMods::Alt);
  const auto *ellipse_intent = intent_as<editor::MaskCreateIntent>(ellipse);
  MASKS_PANEL_CHECK(ellipse_intent != nullptr);
  MASKS_PANEL_CHECK(ellipse_intent->tool == editor::MaskCreateTool::Ellipse);
  MASKS_PANEL_CHECK(ellipse_intent->modifiers.constrain_aspect);
  MASKS_PANEL_CHECK(ellipse_intent->modifiers.from_center);

  harness.target.target = doc::NodeId{11U};
  harness.target.coordinate_space =
      scene::MaskCoordinateSpace::AuthoredCanvas;
  const auto pen = harness.click(350.0F, 39.0F);
  const auto *pen_intent = intent_as<editor::MaskCreateIntent>(pen);
  MASKS_PANEL_CHECK(pen_intent != nullptr);
  MASKS_PANEL_CHECK(pen_intent->target == doc::NodeId{11U});
  MASKS_PANEL_CHECK(pen_intent->coordinate_space ==
                    scene::MaskCoordinateSpace::AuthoredCanvas);
  MASKS_PANEL_CHECK(pen_intent->tool == editor::MaskCreateTool::Bezier);

  harness.authoring_enabled = false;
  MASKS_PANEL_CHECK(!harness.click(70.0F, 39.0F).intent.has_value());
}

// A box, sphere, model or group has no flat surface for a mask. No shape tool
// makes one there, but masks already on it can still be turned off, so an
// operator can clear what the render cannot draw.
void a_target_without_a_flat_surface_makes_no_new_mask() {
  Harness harness;
  harness.target.flat_surface = false;
  harness.selection.mask = {};
  MASKS_PANEL_CHECK(!harness.click(70.0F, 39.0F).intent.has_value());
  MASKS_PANEL_CHECK(!harness.click(210.0F, 39.0F).intent.has_value());
  MASKS_PANEL_CHECK(!harness.click(350.0F, 39.0F).intent.has_value());

  const auto toggled = harness.click(16.0F, 91.0F);
  const auto *enabled = intent_as<editor::MaskEnabledIntent>(toggled);
  MASKS_PANEL_CHECK(enabled != nullptr);
  MASKS_PANEL_CHECK(enabled->mask == doc::NodeId{21U});
  MASKS_PANEL_CHECK(!enabled->enabled);

  harness.target.flat_surface = true;
  MASKS_PANEL_CHECK(
      intent_as<editor::MaskCreateIntent>(harness.click(70.0F, 39.0F)) !=
      nullptr);
}

void rows_share_only_mask_selection_and_enabled_intents() {
  Harness harness;
  harness.selection.mask = {};
  const auto selected = harness.click(110.0F, 91.0F);
  MASKS_PANEL_CHECK(selected.selection_changed);
  MASKS_PANEL_CHECK(selected.selection.scene == doc::NodeId{1U});
  MASKS_PANEL_CHECK(selected.selection.target == doc::NodeId{10U});
  MASKS_PANEL_CHECK(selected.selection.mask == doc::NodeId{21U});
  MASKS_PANEL_CHECK(!selected.selection.point.has_value());

  const auto toggled = harness.click(16.0F, 91.0F);
  const auto *enabled = intent_as<editor::MaskEnabledIntent>(toggled);
  MASKS_PANEL_CHECK(enabled != nullptr);
  MASKS_PANEL_CHECK(enabled->scene == doc::NodeId{1U});
  MASKS_PANEL_CHECK(enabled->target == doc::NodeId{10U});
  MASKS_PANEL_CHECK(enabled->mask == doc::NodeId{21U});
  MASKS_PANEL_CHECK(!enabled->enabled);

  harness.selection.mask = doc::NodeId{999U};
  const auto stale = harness.frame(0.0F, 0.0F, false);
  MASKS_PANEL_CHECK(stale.selection_changed);
  MASKS_PANEL_CHECK(stale.selection.mask == doc::NodeId{});
  MASKS_PANEL_CHECK(!stale.selection.point.has_value());
}

void detail_controls_emit_rename_toggle_combine_and_numeric_types() {
  Harness harness;
  const auto inverted = harness.click(30.0F, 199.0F);
  const auto *invert = intent_as<editor::MaskInvertIntent>(inverted);
  MASKS_PANEL_CHECK(invert != nullptr);
  MASKS_PANEL_CHECK(invert->mask == doc::NodeId{20U});
  MASKS_PANEL_CHECK(invert->invert);

  const auto difference = harness.click(380.0F, 199.0F);
  const auto *combine = intent_as<editor::MaskCombineIntent>(difference);
  MASKS_PANEL_CHECK(combine != nullptr);
  MASKS_PANEL_CHECK(combine->combine == scene::MaskCombine::Difference);

  const auto opacity = harness.drag(60.0F, 226.0F, 80.0F);
  const auto *opacity_edit =
      intent_as<editor::MaskScalarEditIntent>(opacity);
  MASKS_PANEL_CHECK(opacity_edit != nullptr);
  MASKS_PANEL_CHECK(opacity_edit->property ==
                    editor::MaskScalarProperty::Opacity);
  MASKS_PANEL_CHECK(std::abs(opacity_edit->value - 0.85F) < 1.0e-5F);
  MASKS_PANEL_CHECK(opacity_edit->edit.kind ==
                    editor::NumberFieldEditKind::Delta);

  const auto feather = harness.drag(190.0F, 226.0F, 210.0F);
  const auto *feather_edit =
      intent_as<editor::MaskScalarEditIntent>(feather);
  MASKS_PANEL_CHECK(feather_edit != nullptr);
  MASKS_PANEL_CHECK(feather_edit->property ==
                    editor::MaskScalarProperty::FeatherCanvasPx);
  MASKS_PANEL_CHECK(std::abs(feather_edit->value - 9.0F) < 1.0e-5F);

  const auto expansion = harness.drag(330.0F, 226.0F, 350.0F);
  const auto *expansion_edit =
      intent_as<editor::MaskScalarEditIntent>(expansion);
  MASKS_PANEL_CHECK(expansion_edit != nullptr);
  MASKS_PANEL_CHECK(expansion_edit->property ==
                    editor::MaskScalarProperty::ExpansionCanvasPx);
  MASKS_PANEL_CHECK(std::abs(expansion_edit->value - 3.0F) < 1.0e-5F);

  const auto left = harness.drag(40.0F, 253.0F, 60.0F);
  const auto *bounds = intent_as<editor::MaskBoundsEditIntent>(left);
  MASKS_PANEL_CHECK(bounds != nullptr);
  MASKS_PANEL_CHECK(bounds->component == editor::MaskBoundsComponent::Left);
  MASKS_PANEL_CHECK(std::abs(bounds->bounds.left - 0.2F) < 1.0e-5F);
}

void reorder_is_a_row_drag_and_delete_stays_target_local() {
  Harness harness;
  // Dragging the first row onto the second steps the stack one place, the way
  // a Timeline track header does. No up/down buttons are involved.
  const auto down = harness.drag_to(110.0F, 66.0F, 110.0F, 92.0F);
  const auto *moved = intent_as<editor::MaskReorderIntent>(down);
  MASKS_PANEL_CHECK(moved != nullptr);
  MASKS_PANEL_CHECK(moved->scene == doc::NodeId{1U});
  MASKS_PANEL_CHECK(moved->target == doc::NodeId{10U});
  MASKS_PANEL_CHECK(moved->mask == doc::NodeId{20U});
  MASKS_PANEL_CHECK(moved->destination_index == 1U);

  // Dragging the last row upward steps the other way.
  const auto up = harness.drag_to(110.0F, 118.0F, 110.0F, 92.0F);
  const auto *raised = intent_as<editor::MaskReorderIntent>(up);
  MASKS_PANEL_CHECK(raised != nullptr);
  MASKS_PANEL_CHECK(raised->mask == doc::NodeId{22U});
  MASKS_PANEL_CHECK(raised->destination_index == 1U);

  // A drag that never leaves its own row reorders nothing.
  MASKS_PANEL_CHECK(!intent_as<editor::MaskReorderIntent>(
      harness.drag_to(110.0F, 66.0F, 130.0F, 70.0F)));

  harness.selection.mask = doc::NodeId{20U};
  const auto deleted = harness.click(390.0F, 66.0F);
  const auto *erase = intent_as<editor::MaskDeleteIntent>(deleted);
  MASKS_PANEL_CHECK(erase != nullptr);
  MASKS_PANEL_CHECK(erase->mask == doc::NodeId{20U});

  harness.authoring_enabled = false;
  MASKS_PANEL_CHECK(!harness.drag_to(110.0F, 66.0F, 110.0F, 92.0F)
                         .intent.has_value());
  MASKS_PANEL_CHECK(!harness.click(390.0F, 66.0F).intent.has_value());
  const auto selection = harness.click(110.0F, 91.0F);
  MASKS_PANEL_CHECK(selection.selection_changed);
  MASKS_PANEL_CHECK(selection.selection.mask == doc::NodeId{21U});
}

void a_second_click_on_a_selected_row_renames_it() {
  Harness harness;
  // With no name open the row is not a text field, so typing authors nothing.
  MASKS_PANEL_CHECK(!harness
                         .frame(110.0F, 92.0F, false, gui::KeyMods::None, "Z")
                         .intent.has_value());

  // The first click only selects, exactly as it does on a track header.
  static_cast<void>(harness.click(110.0F, 92.0F));
  MASKS_PANEL_CHECK(harness.selection.mask == doc::NodeId{21U});

  // The second opens the name, and typing authors the rename with no button
  // to press afterwards.
  static_cast<void>(harness.click(110.0F, 92.0F));
  const auto typed =
      harness.frame(110.0F, 92.0F, false, gui::KeyMods::None, "Z");
  const auto *rename = intent_as<editor::MaskRenameIntent>(typed);
  MASKS_PANEL_CHECK(rename != nullptr);
  MASKS_PANEL_CHECK(rename->scene == doc::NodeId{1U});
  MASKS_PANEL_CHECK(rename->target == doc::NodeId{10U});
  MASKS_PANEL_CHECK(rename->mask == doc::NodeId{21U});
  MASKS_PANEL_CHECK(rename->name == "Ellipse 1Z");
  MASKS_PANEL_CHECK(harness.panel.edit_name() == "Ellipse 1Z");

  // Selecting another row closes the open name.
  static_cast<void>(harness.click(110.0F, 66.0F));
  MASKS_PANEL_CHECK(!harness
                         .frame(110.0F, 66.0F, false, gui::KeyMods::None, "Q")
                         .intent.has_value());
}

void bezier_points_edit_and_delete_the_point_the_stage_selected() {
  Harness harness;
  const auto selected_mask = harness.click(110.0F, 117.0F);
  MASKS_PANEL_CHECK(selected_mask.selection_changed);
  MASKS_PANEL_CHECK(selected_mask.selection.mask == doc::NodeId{22U});

  // Points are picked on the Stage, where they are. The panel takes that
  // selection as given and never offers a strip of numbered buttons for it.
  harness.selection.point = doc::NodeId{100U};
  const auto held = harness.frame(0.0F, 0.0F, false);
  MASKS_PANEL_CHECK(!held.selection_changed);
  MASKS_PANEL_CHECK(held.selection.point == doc::NodeId{100U});

  const auto edited = harness.drag(60.0F, 280.0F, 80.0F);
  const auto *point_edit = intent_as<editor::MaskPointEditIntent>(edited);
  MASKS_PANEL_CHECK(point_edit != nullptr);
  MASKS_PANEL_CHECK(point_edit->scene == doc::NodeId{1U});
  MASKS_PANEL_CHECK(point_edit->target == doc::NodeId{10U});
  MASKS_PANEL_CHECK(point_edit->mask == doc::NodeId{22U});
  MASKS_PANEL_CHECK(point_edit->point == doc::NodeId{100U});
  MASKS_PANEL_CHECK(point_edit->component == editor::MaskPointComponent::X);
  MASKS_PANEL_CHECK(point_edit->patch.x.has_value());
  MASKS_PANEL_CHECK(std::abs(*point_edit->patch.x - 0.1F) < 1.0e-5F);

  const auto inserted = harness.click(100.0F, 361.0F);
  const auto *insert = intent_as<editor::MaskPointInsertIntent>(inserted);
  MASKS_PANEL_CHECK(insert != nullptr);
  MASKS_PANEL_CHECK(insert->mask == doc::NodeId{22U});
  MASKS_PANEL_CHECK(insert->index == 1U);
  MASKS_PANEL_CHECK(std::abs(insert->geometry.x - 0.5F) < 1.0e-5F);
  MASKS_PANEL_CHECK(insert->geometry.y == 0.0F);

  const auto deleted = harness.click(320.0F, 361.0F);
  const auto *erase = intent_as<editor::MaskPointDeleteIntent>(deleted);
  MASKS_PANEL_CHECK(erase != nullptr);
  MASKS_PANEL_CHECK(erase->mask == doc::NodeId{22U});
  MASKS_PANEL_CHECK(erase->point == doc::NodeId{100U});

  harness.selection.point = doc::NodeId{999U};
  const auto stale = harness.frame(0.0F, 0.0F, false);
  MASKS_PANEL_CHECK(stale.selection_changed);
  MASKS_PANEL_CHECK(!stale.selection.point.has_value());
  MASKS_PANEL_CHECK(stale.selection.mask == doc::NodeId{22U});
}

void every_model_diagnostic_has_plain_operator_language() {
  constexpr std::array kinds{
      scene::MaskDiagnosticKind::MissingSceneOwner,
      scene::MaskDiagnosticKind::InvalidSceneOwner,
      scene::MaskDiagnosticKind::MissingTarget,
      scene::MaskDiagnosticKind::MissingTargetSceneOwner,
      scene::MaskDiagnosticKind::CrossSceneTarget,
      scene::MaskDiagnosticKind::InvalidTarget,
      scene::MaskDiagnosticKind::MissingMask,
      scene::MaskDiagnosticKind::CrossSceneMask,
      scene::MaskDiagnosticKind::MalformedPath,
      scene::MaskDiagnosticKind::DuplicatePointOrder,
      scene::MaskDiagnosticKind::InvalidFirstCombine,
      scene::MaskDiagnosticKind::MaskLimitExceeded,
      scene::MaskDiagnosticKind::PointLimitExceeded,
  };
  for (const scene::MaskDiagnosticKind kind : kinds) {
    const std::string_view message = editor::mask_diagnostic_message(kind);
    MASKS_PANEL_CHECK(!message.empty());
    MASKS_PANEL_CHECK(message != "MASK ERROR");
  }
  MASKS_PANEL_CHECK(editor::mask_diagnostic_message(
                        scene::MaskDiagnosticKind::MalformedPath) ==
                    "MALFORMED PATH");
  MASKS_PANEL_CHECK(editor::mask_diagnostic_message(
                        scene::MaskDiagnosticKind::InvalidFirstCombine) ==
                    "FIRST ACTIVE MASK MUST ADD");
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 8U> cases{{
    {"a target without a flat surface makes no new mask",
     &a_target_without_a_flat_surface_makes_no_new_mask},
    {"shape tools emit explicit target space and familiar modifiers",
     &shape_tools_emit_explicit_target_space_and_familiar_modifiers},
    {"rows share only mask selection and enabled intents",
     &rows_share_only_mask_selection_and_enabled_intents},
    {"detail controls emit rename toggle combine and numeric types",
     &detail_controls_emit_rename_toggle_combine_and_numeric_types},
    {"reorder is a row drag and delete stays target local",
     &reorder_is_a_row_drag_and_delete_stays_target_local},
    {"a second click on a selected row renames it",
     &a_second_click_on_a_selected_row_renames_it},
    {"bezier points edit and delete the point the stage selected",
     &bezier_points_edit_and_delete_the_point_the_stage_selected},
    {"every model diagnostic has plain operator language",
     &every_model_diagnostic_has_plain_operator_language},
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
      std::cerr << "[fail] " << test.name << ": " << exception.what()
                << '\n';
    }
  }
  std::cout << (cases.size() - failures) << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
