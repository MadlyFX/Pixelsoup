#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/editor/effects_panel.hpp>
#include <mediaserver/editor/workspace_panels.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace assets = mediaserver::assets;
namespace compositor = mediaserver::compositor;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace render = mediaserver::render;
namespace scene = mediaserver::scene;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define UI_CHECK(...)                                                          \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] std::vector<editor::ElementInfo> elements() {
  editor::ElementInfo object{};
  object.node = doc::NodeId{7U};
  object.kind = editor::ElementKind::Object;
  editor::ElementInfo media{};
  media.node = doc::NodeId{9U};
  media.kind = editor::ElementKind::Media;
  editor::ElementInfo light{};
  light.node = doc::NodeId{11U};
  light.kind = editor::ElementKind::PointLight;
  return {object, media, light};
}

// The outliner's geometry: a 32-unit search bar, then list-height rows (the
// first row's centre is y=46), the eye at x=2..26 and the padlock at the
// right edge, a 30-unit footer naming the selection.
constexpr float scene_row(int index) {
  return 32.0F + editor::list_metrics::row_height *
                     (static_cast<float>(index) + 0.5F);
}

struct SceneHarness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::ScenePanel panel{gui::ui_id("scene.test")};
  std::vector<editor::ElementInfo> items{elements()};
  std::vector<doc::NodeId> selection;
  std::vector<mediaserver::output::ProjectionSurface> surfaces;
  doc::NodeId selected_surface{};
  static constexpr gui::Rect bounds{0.0F, 0.0F, 240.0F, 300.0F};

  std::optional<editor::SceneSelectionRequest>
  frame(float x, float y, bool down, gui::KeyMods mods = gui::KeyMods::None,
        std::string_view text = {}) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.set_mods(mods);
    input.add_text(text);
    ctx.new_frame();
    auto request = panel.draw(ctx, bounds, items, selection,
                              selection.empty() ? doc::NodeId{}
                                                : selection.back(),
                              false, editor::SceneSettingsTarget::None,
                              surfaces, selected_surface);
    // Menus are drawn - and so placed - by the host after every panel.
    ctx.draw_overlays(bounds);
    return request;
  }

  std::optional<editor::SceneSelectionRequest> click(float x, float y) {
    static_cast<void>(frame(x, y, false));
    static_cast<void>(frame(x, y, true));
    return frame(x, y, false);
  }

  // A right press and release, which opens a context menu.
  std::optional<editor::SceneSelectionRequest> right_click(float x, float y) {
    static_cast<void>(frame(x, y, false));
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, false);
    input.set_mouse_button(gui::MouseButton::Right, true);
    ctx.new_frame();
    auto request = panel.draw(ctx, bounds, items, selection,
                              selection.empty() ? doc::NodeId{}
                                                : selection.back(),
                              false, editor::SceneSettingsTarget::None,
                              surfaces, selected_surface);
    ctx.draw_overlays(bounds);
    input.set_mouse_button(gui::MouseButton::Right, false);
    static_cast<void>(frame(x, y, false));
    return request;
  }

  // Chooses entry `index` of a menu of `count` entries opened at `anchor_y`:
  // menus open 6 units below the pointer in menu_row_height rows, or flip
  // above it when they would leave the 300-unit window, never closer than 8
  // to its top.
  std::optional<editor::SceneSelectionRequest>
  choose(float anchor_y, std::size_t count, std::size_t index) {
    const float height = gui::menu_row_height * static_cast<float>(count);
    float top = anchor_y + 6.0F;
    if (top + height > bounds.height - 8.0F) {
      top = anchor_y - height - 6.0F;
    }
    top = std::max(top, 8.0F);
    const float y =
        top + gui::menu_row_height * (static_cast<float>(index) + 0.5F);
    static_cast<void>(frame(90.0F, y, false));
    static_cast<void>(frame(90.0F, y, true));
    return frame(90.0F, y, false);
  }
};

// The stage's LED surfaces follow the scene's elements. A click selects one
// on Stage; its padlock - at the right edge, like every row's - sets its lock;
// a locked surface's row offers only the padlock. No eye: x=2..26 is inert.
void led_surfaces_list_after_the_scene_with_a_lock() {
  SceneHarness harness;
  mediaserver::output::ProjectionSurface surface{};
  surface.node = doc::NodeId{40U};
  surface.name = "Main LED";
  surface.locked = false;
  harness.surfaces.push_back(surface);
  static_cast<void>(harness.frame(0.0F, 0.0F, false));

  const auto selected = harness.click(80.0F, scene_row(3));
  UI_CHECK(selected.has_value());
  UI_CHECK(selected->surface);
  UI_CHECK(selected->node == doc::NodeId{40U});
  UI_CHECK(!selected->lock_changed);

  const auto locked = harness.click(226.0F, scene_row(3));
  UI_CHECK(locked.has_value());
  UI_CHECK(locked->surface);
  UI_CHECK(locked->node == doc::NodeId{40U});
  UI_CHECK(locked->lock_changed);
  UI_CHECK(locked->locked);

  const auto eye = harness.click(14.0F, scene_row(3));
  UI_CHECK(!eye.has_value() || !eye->visibility_changed);

  harness.surfaces.back().locked = true;
  const auto inert = harness.click(80.0F, scene_row(3));
  UI_CHECK(!inert.has_value() || !inert->surface);
  const auto unlocked = harness.click(226.0F, scene_row(3));
  UI_CHECK(unlocked.has_value());
  UI_CHECK(unlocked->lock_changed);
  UI_CHECK(!unlocked->locked);
}

void a_scene_row_reports_the_clicked_node() {
  SceneHarness harness;
  static_cast<void>(harness.frame(80.0F, scene_row(0), false));
  static_cast<void>(harness.frame(80.0F, scene_row(0), true));
  const auto request = harness.frame(80.0F, scene_row(0), false);

  UI_CHECK(request.has_value());
  UI_CHECK(request->node == doc::NodeId{7U});
  UI_CHECK(request->action == editor::SelectionAction::Replace);
}

void a_ctrl_scene_click_requests_an_exact_selection_toggle() {
  SceneHarness harness;
  static_cast<void>(harness.frame(80.0F, scene_row(0), false));
  static_cast<void>(
      harness.frame(80.0F, scene_row(0), true, gui::KeyMods::Ctrl));
  const auto request =
      harness.frame(80.0F, scene_row(0), false, gui::KeyMods::Ctrl);

  UI_CHECK(request.has_value());
  UI_CHECK(request->action == editor::SelectionAction::Toggle);
  UI_CHECK(request->visible_order.empty());
}

void a_shift_scene_click_carries_the_visible_element_order() {
  SceneHarness harness;
  // The outliner is the scene's hierarchy, not a sort by kind, so the light
  // is simply the third top-level row.
  static_cast<void>(harness.frame(80.0F, scene_row(2), false));
  static_cast<void>(
      harness.frame(80.0F, scene_row(2), true, gui::KeyMods::Shift));
  const auto request =
      harness.frame(80.0F, scene_row(2), false, gui::KeyMods::Shift);

  UI_CHECK(request.has_value());
  UI_CHECK(request->node == doc::NodeId{11U});
  UI_CHECK(request->action == editor::SelectionAction::Range);
  UI_CHECK(request->visible_order ==
           std::vector<doc::NodeId>{doc::NodeId{7U}, doc::NodeId{9U},
                                    doc::NodeId{11U}});
}

void bare_scene_list_space_requests_a_clear() {
  SceneHarness harness;
  static_cast<void>(harness.frame(80.0F, 240.0F, false));
  const auto request = harness.frame(80.0F, 240.0F, true);

  UI_CHECK(request.has_value());
  UI_CHECK(request->action == editor::SelectionAction::Clear);
  UI_CHECK(request->node == doc::NodeId{});
}

void a_locked_scene_row_only_offers_its_unlock_control() {
  SceneHarness harness;
  harness.items.front().locked = true;

  static_cast<void>(harness.frame(80.0F, scene_row(0), false));
  static_cast<void>(harness.frame(80.0F, scene_row(0), true));
  UI_CHECK(!harness.frame(80.0F, scene_row(0), false).has_value());

  // The padlock sits at the row's right edge, x=214..238.
  constexpr float lock_x = 226.0F;
  static_cast<void>(harness.frame(lock_x, scene_row(0), false));
  static_cast<void>(harness.frame(lock_x, scene_row(0), true));
  const auto request = harness.frame(lock_x, scene_row(0), false);
  UI_CHECK(request.has_value());
  UI_CHECK(request->node == doc::NodeId{7U});
  UI_CHECK(request->lock_changed);
  UI_CHECK(!request->locked);
}

void scene_search_filters_the_hierarchy() {
  SceneHarness harness;
  // Focus the search well, then type. The filtered tree holds only the
  // matching point light, in the first row.
  static_cast<void>(harness.frame(70.0F, 16.0F, false));
  static_cast<void>(harness.frame(70.0F, 16.0F, true));
  static_cast<void>(harness.frame(70.0F, 16.0F, false));
  static_cast<void>(
      harness.frame(70.0F, 16.0F, false, gui::KeyMods::None, "point"));
  UI_CHECK(harness.panel.search() == "point");

  static_cast<void>(harness.frame(80.0F, scene_row(0), true));
  const auto request = harness.frame(80.0F, scene_row(0), false);
  UI_CHECK(request.has_value());
  UI_CHECK(request->node == doc::NodeId{11U});
}

void a_scene_group_is_visible_selectable_and_uses_its_authored_name() {
  SceneHarness harness;
  editor::ElementInfo group{};
  group.node = doc::NodeId{13U};
  group.kind = editor::ElementKind::Group;
  group.display_name = "Opening Look";
  harness.items.push_back(group);

  // An empty group is a top-level row of its own, after its siblings.
  static_cast<void>(harness.frame(80.0F, scene_row(3), false));
  static_cast<void>(harness.frame(80.0F, scene_row(3), true));
  const auto request = harness.frame(80.0F, scene_row(3), false);
  UI_CHECK(request.has_value());
  UI_CHECK(request->node == doc::NodeId{13U});
  UI_CHECK(request->action == editor::SelectionAction::Replace);
}

// Grouping lives in a row's right-click menu; the footer only names the
// selection. An object's menu reads Rename, Hide, Lock, Highlight Color...,
// Group Selection, Delete.
void scene_group_and_ungroup_live_in_the_row_menu() {
  SceneHarness footer;
  footer.selection = {doc::NodeId{7U}, doc::NodeId{9U}};
  // Where the footer's Group and Ungroup buttons used to be.
  UI_CHECK(!footer.click(126.0F, 285.0F).has_value());
  UI_CHECK(!footer.click(200.0F, 285.0F).has_value());

  SceneHarness group_harness;
  group_harness.selection = {doc::NodeId{7U}, doc::NodeId{9U}};
  static_cast<void>(group_harness.right_click(80.0F, scene_row(0)));
  const auto grouped = group_harness.choose(scene_row(0), 6U, 4U);
  UI_CHECK(grouped.has_value());
  UI_CHECK(grouped->structure ==
           editor::SceneSelectionRequest::StructureAction::Group);

  // A group's menu: Rename, Hide, Lock, Highlight Color..., Group, Ungroup,
  // New Group Inside, New Vehicle Process Group Inside, Delete.
  SceneHarness ungroup_harness;
  editor::ElementInfo group{};
  group.node = doc::NodeId{13U};
  group.kind = editor::ElementKind::Group;
  group.display_name = "Opening Look";
  ungroup_harness.items.push_back(group);
  ungroup_harness.selection = {group.node};
  static_cast<void>(ungroup_harness.right_click(80.0F, scene_row(3)));
  const auto ungrouped = ungroup_harness.choose(scene_row(3), 9U, 5U);
  UI_CHECK(ungrouped.has_value());
  UI_CHECK(ungrouped->structure ==
           editor::SceneSelectionRequest::StructureAction::Ungroup);
}

// A group's menu files a new, empty group inside it - a folder or a vehicle
// process group.
void a_scene_group_menu_creates_empty_groups_inside_it() {
  for (const bool vehicle : {false, true}) {
    SceneHarness harness;
    editor::ElementInfo group{};
    group.node = doc::NodeId{13U};
    group.kind = editor::ElementKind::Group;
    group.display_name = "Opening Look";
    harness.items.push_back(group);
    harness.selection = {group.node};
    static_cast<void>(harness.right_click(80.0F, scene_row(3)));
    const auto created =
        harness.choose(scene_row(3), 9U, vehicle ? 7U : 6U);
    UI_CHECK(created.has_value());
    UI_CHECK(created->structure ==
             editor::SceneSelectionRequest::StructureAction::Create);
    UI_CHECK(created->destination == doc::NodeId{13U});
    UI_CHECK(created->group_kind == (vehicle
                                         ? scene::GroupKind::VehicleProcess
                                         : scene::GroupKind::Folder));
  }
}

// A right-click on the list's empty space offers New Group and New Vehicle
// Process Group at the top level, and selects nothing.
void scene_empty_space_menu_creates_top_level_groups() {
  for (const bool vehicle : {false, true}) {
    SceneHarness harness;
    harness.selection = {doc::NodeId{9U}};
    const auto opened = harness.right_click(80.0F, 200.0F);
    UI_CHECK(!opened.has_value() ||
             opened->action != editor::SelectionAction::Replace);
    const auto created = harness.choose(200.0F, 2U, vehicle ? 1U : 0U);
    UI_CHECK(created.has_value());
    UI_CHECK(created->structure ==
             editor::SceneSelectionRequest::StructureAction::Create);
    UI_CHECK(created->destination == doc::NodeId{});
    UI_CHECK(created->group_kind == (vehicle
                                         ? scene::GroupKind::VehicleProcess
                                         : scene::GroupKind::Folder));
  }
}

// Highlight Color... opens the palette where the menu was: None, then Red,
// Orange, Yellow, Green, Cyan, Blue, Purple, Pink. The choice is reported for
// the selection; None clears it.
void a_scene_row_menu_picks_a_highlight_color() {
  for (const std::size_t entry : {3U, 0U}) {
    SceneHarness harness;
    harness.selection = {doc::NodeId{7U}};
    static_cast<void>(harness.right_click(80.0F, scene_row(0)));
    const auto opened = harness.choose(scene_row(0), 6U, 3U);
    UI_CHECK(!opened.has_value() || !opened->highlight_changed);
    // The palette opens at the pointer that chose Highlight Color...
    const float palette_anchor =
        scene_row(0) + 6.0F + gui::menu_row_height * 3.5F;
    const auto picked = harness.choose(palette_anchor, 9U, entry);
    UI_CHECK(picked.has_value());
    UI_CHECK(picked->highlight_changed);
    UI_CHECK(picked->highlight ==
             (entry == 0U ? std::optional<core::u8>{}
                          : std::optional<core::u8>{
                                static_cast<core::u8>(entry - 1U)}));
  }
}

// A double-click on a row's name opens it for renaming with the old name
// selected; typing replaces it and Enter reports the new name once.
void a_scene_row_renames_in_place() {
  SceneHarness harness;
  static_cast<void>(harness.frame(80.0F, scene_row(0), false));
  static_cast<void>(harness.frame(80.0F, scene_row(0), true));
  static_cast<void>(harness.frame(80.0F, scene_row(0), false));
  static_cast<void>(harness.frame(80.0F, scene_row(0), true));
  static_cast<void>(harness.frame(80.0F, scene_row(0), false));
  UI_CHECK(harness.panel.renaming());
  static_cast<void>(harness.frame(300.0F, 400.0F, false, gui::KeyMods::None,
                                  "Floor Riser"));
  harness.input.new_frame();
  harness.input.add_key(gui::Key::Enter);
  harness.ctx.new_frame();
  const auto renamed = harness.panel.draw(harness.ctx, harness.bounds,
                                          harness.items, harness.selection);
  UI_CHECK(renamed.has_value());
  UI_CHECK(renamed->node == doc::NodeId{7U});
  UI_CHECK(renamed->rename == std::optional<std::string>{"Floor Riser"});
  UI_CHECK(!harness.panel.renaming());
}

// The eye at a row's left edge is the element's own visibility, the same
// switch the Timeline's eye throws.
void a_scene_eye_hides_and_shows() {
  SceneHarness harness;
  static_cast<void>(harness.frame(14.0F, scene_row(1), false));
  static_cast<void>(harness.frame(14.0F, scene_row(1), true));
  const auto hidden = harness.frame(14.0F, scene_row(1), false);
  UI_CHECK(hidden.has_value());
  UI_CHECK(hidden->node == doc::NodeId{9U});
  UI_CHECK(hidden->visibility_changed);
  UI_CHECK(!hidden->visible);
}

// Dragging a row onto the middle of a group files it under the group; onto
// the edge of a sibling, it reorders beside it.
void scene_rows_drag_into_groups_and_between_siblings() {
  SceneHarness harness;
  editor::ElementInfo group{};
  group.node = doc::NodeId{13U};
  group.kind = editor::ElementKind::Group;
  group.display_name = "Rig";
  harness.items.push_back(group);

  static_cast<void>(harness.frame(80.0F, scene_row(0), false));
  static_cast<void>(harness.frame(80.0F, scene_row(0), true));
  static_cast<void>(harness.frame(80.0F, scene_row(2), true));
  static_cast<void>(harness.frame(80.0F, scene_row(3), true));
  const auto filed = harness.frame(80.0F, scene_row(3), false);
  UI_CHECK(filed.has_value());
  UI_CHECK(filed->structure ==
           editor::SceneSelectionRequest::StructureAction::MoveInto);
  UI_CHECK(filed->destination == group.node);
  UI_CHECK(filed->moved == std::vector<doc::NodeId>{doc::NodeId{7U}});

  static_cast<void>(harness.frame(80.0F, scene_row(0), false));
  static_cast<void>(harness.frame(80.0F, scene_row(0), true));
  static_cast<void>(harness.frame(80.0F, scene_row(1), true));
  const auto reordered = harness.frame(80.0F, scene_row(1) + 9.0F, false);
  UI_CHECK(reordered.has_value());
  UI_CHECK(!reordered->structure.has_value());
  UI_CHECK(reordered->node == doc::NodeId{7U});
  UI_CHECK(reordered->reorder_destination == doc::NodeId{9U});
  UI_CHECK(!reordered->reorder_before);
}

void an_effect_drag_reports_its_graph_drop_position_without_a_selection() {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::EffectsPanel panel{gui::ui_id("effects.drag.test")};
  constexpr gui::Rect bounds{0.0F, 0.0F, 240.0F, 180.0F};
  const auto frame = [&](float x, float y, bool down) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    ctx.new_frame();
    return panel.draw(ctx, bounds);
  };

  static_cast<void>(frame(80.0F, 90.0F, false));
  static_cast<void>(frame(80.0F, 90.0F, true));
  static_cast<void>(frame(430.0F, 270.0F, true));
  const auto dropped = frame(430.0F, 270.0F, false);
  UI_CHECK(dropped.has_value());
  UI_CHECK(dropped->target == doc::NodeId{});
  UI_CHECK(dropped->effect == scene::EffectKind::Brightness);
  UI_CHECK(dropped->screen_x == 430.0F);
  UI_CHECK(dropped->screen_y == 270.0F);
}

void clicking_an_effect_does_not_apply_it() {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::EffectsPanel panel{gui::ui_id("effects.saturation.test")};
  constexpr gui::Rect bounds{0.0F, 0.0F, 240.0F, 180.0F};
  const doc::NodeId selected{42U};
  const auto frame = [&](bool down) {
    input.new_frame();
    input.set_mouse_position(80.0F, 140.0F);
    input.set_mouse_button(gui::MouseButton::Left, down);
    ctx.new_frame();
    return panel.draw(ctx, bounds, selected);
  };

  static_cast<void>(frame(false));
  static_cast<void>(frame(true));
  const auto applied = frame(false);
  UI_CHECK(!applied.has_value());
}

void effect_search_filters_before_dragging() {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::EffectsPanel panel{gui::ui_id("effects.search.test")};
  constexpr gui::Rect bounds{0.0F, 0.0F, 240.0F, 200.0F};
  const auto frame = [&](float x, float y, bool down,
                         std::string_view text = {}) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.add_text(text);
    ctx.new_frame();
    return panel.draw(ctx, bounds);
  };

  static_cast<void>(frame(80.0F, 37.0F, false));
  static_cast<void>(frame(80.0F, 37.0F, true));
  static_cast<void>(frame(80.0F, 37.0F, false));
  static_cast<void>(frame(80.0F, 37.0F, false, "directional"));
  UI_CHECK(panel.search() == "directional");

  static_cast<void>(frame(80.0F, 90.0F, false));
  static_cast<void>(frame(80.0F, 90.0F, true));
  static_cast<void>(frame(430.0F, 270.0F, true));
  const auto dropped = frame(430.0F, 270.0F, false);
  UI_CHECK(dropped.has_value());
  UI_CHECK(dropped->effect == scene::EffectKind::DirectionalBlur);
}

void effect_drops_apply_only_over_stage_or_inspector() {
  constexpr gui::Rect stage{300.0F, 40.0F, 600.0F, 400.0F};
  constexpr gui::Rect inspector{920.0F, 40.0F, 260.0F, 600.0F};
  editor::EffectApplyRequest request{doc::NodeId{42U}, scene::EffectKind::Blur,
                                     1040.0F, 240.0F};

  UI_CHECK(
      editor::effect_drop_targets_selected_layer(request, &stage, &inspector));
  request.screen_x = 500.0F;
  request.screen_y = 200.0F;
  UI_CHECK(
      editor::effect_drop_targets_selected_layer(request, &stage, &inspector));
  request.screen_x = 120.0F;
  request.screen_y = 500.0F;
  UI_CHECK(
      !editor::effect_drop_targets_selected_layer(request, &stage, &inspector));

  // A drag with no selected target can never apply implicitly.
  request.target = {};
  UI_CHECK(
      !editor::effect_drop_targets_selected_layer(request, &stage, &inspector));
}

void an_effect_drop_target_strengthens_under_the_pointer() {
  constexpr gui::Rect target{300.0F, 40.0F, 260.0F, 600.0F};
  const auto strongest_cyan = [&](float x, float y) {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    input.new_frame();
    input.set_mouse_position(x, y);
    ctx.new_frame();
    editor::draw_effect_drop_target(ctx, target, "Hero clip");
    float strongest{};
    const gui::UiColor accent = ctx.theme().accent;
    for (const gui::UiVertex &vertex : list.vertices()) {
      if (vertex.r == accent.r && vertex.g == accent.g &&
          vertex.b == accent.b) {
        strongest = std::max(strongest, vertex.a);
      }
    }
    return strongest;
  };

  const float candidate = strongest_cyan(120.0F, 500.0F);
  const float hovered = strongest_cyan(420.0F, 240.0F);
  UI_CHECK(candidate > 0.25F && candidate < 0.50F);
  UI_CHECK(hovered > 0.95F);
}

[[nodiscard]] compositor::GraphNode
graph_node(core::u64 id, compositor::NodeKind kind, float x, float y) {
  compositor::GraphNode node{};
  node.node = doc::NodeId{id};
  node.kind = kind;
  node.label = std::string{compositor::node_kind_name(kind)};
  node.editor_x = x;
  node.editor_y = y;
  switch (kind) {
  case compositor::NodeKind::SceneLayer:
    node.parameters = compositor::SceneLayerParameters{doc::NodeId{1000U}};
    break;
  case compositor::NodeKind::Brightness:
    node.parameters = compositor::BrightnessParameters{1.25F};
    break;
  case compositor::NodeKind::Saturation:
    node.parameters = compositor::SaturationParameters{1.5F};
    break;
  case compositor::NodeKind::Levels:
    node.parameters = compositor::LevelsParameters{};
    break;
  case compositor::NodeKind::LiftGammaGain:
    node.parameters = compositor::LiftGammaGainParameters{};
    break;
  case compositor::NodeKind::ContrastPivot:
    node.parameters = compositor::ContrastPivotParameters{};
    break;
  case compositor::NodeKind::ColorMatrix:
    node.parameters = compositor::ColorMatrixParameters{};
    break;
  case compositor::NodeKind::WhiteBalance:
    node.parameters = compositor::WhiteBalanceParameters{};
    break;
  case compositor::NodeKind::Curves: {
    const auto parameters =
        scene::normalized_curves(compositor::CurvesParameters{});
    UI_CHECK(parameters.has_value());
    node.parameters = *parameters;
    break;
  }
  case compositor::NodeKind::FilmGrain:
    node.parameters = compositor::FilmGrainParameters{
        0.25F, 2.0F, 0.1F, 17U, scene::FilmGrainMode::Monochrome};
    break;
  case compositor::NodeKind::Blur:
    node.parameters = compositor::BlurParameters{12.0F};
    break;
  case compositor::NodeKind::BoxBlur:
    node.parameters = compositor::BoxBlurParameters{4U, 2U};
    break;
  case compositor::NodeKind::DirectionalBlur:
    node.parameters = compositor::DirectionalBlurParameters{16.0F, 0.125F};
    break;
  case compositor::NodeKind::ChromaKey:
    node.parameters = compositor::ChromaKeyParameters{};
    break;
  case compositor::NodeKind::LumaKey:
    node.parameters = compositor::LumaKeyParameters{};
    break;
  case compositor::NodeKind::Matte:
    node.parameters = compositor::MatteParameters{};
    break;
  case compositor::NodeKind::RadialArray:
    node.parameters = compositor::RadialArrayParameters{};
    break;
  case compositor::NodeKind::Composite:
    node.parameters = compositor::CompositeParameters{};
    break;
  case compositor::NodeKind::Output:
    node.parameters = compositor::OutputParameters{};
    break;
  }
  return node;
}

struct CompositorHarness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::CompositorPanel panel{gui::ui_id("compositor.edit.test")};
  static constexpr gui::Rect bounds{0.0F, 0.0F, 900.0F, 640.0F};

  editor::CompositorFrame frame(const compositor::CompositorGraph &graph,
                                float x, float y, bool down,
                                std::string_view text = {},
                                std::optional<gui::Key> key = std::nullopt,
                                gui::KeyMods mods = gui::KeyMods::None) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.set_mouse_button(gui::MouseButton::Middle, false);
    input.set_mods(mods);
    input.add_text(text);
    if (key.has_value()) {
      input.add_key(*key);
    }
    ctx.new_frame();
    return panel.draw(ctx, bounds, graph);
  }

  // A frame with the right button up or down, for context menus.
  editor::CompositorFrame secondary(const compositor::CompositorGraph &graph,
                                    float x, float y, bool down) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, false);
    input.set_mouse_button(gui::MouseButton::Middle, false);
    input.set_mouse_button(gui::MouseButton::Right, down);
    input.set_mods(gui::KeyMods::None);
    ctx.new_frame();
    auto result = panel.draw(ctx, bounds, graph);
    input.set_mouse_button(gui::MouseButton::Right, false);
    return result;
  }

  editor::CompositorFrame
  navigation_frame(const compositor::CompositorGraph &graph, float x, float y,
                   bool middle_down, float wheel = 0.0F) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, false);
    input.set_mouse_button(gui::MouseButton::Middle, middle_down);
    input.add_wheel(wheel);
    ctx.new_frame();
    return panel.draw(ctx, bounds, graph);
  }
};

void select_compositor_node(CompositorHarness &harness,
                            const compositor::CompositorGraph &graph) {
  static_cast<void>(harness.frame(graph, 100.0F, 75.0F, false));
  static_cast<void>(harness.frame(graph, 100.0F, 75.0F, true));
  static_cast<void>(harness.frame(graph, 100.0F, 75.0F, false));
}

[[nodiscard]] editor::CompositorFrame
commit_compositor_field(CompositorHarness &harness,
                        const compositor::CompositorGraph &graph, float x,
                        float y, std::string_view value) {
  static_cast<void>(harness.frame(graph, x, y, false));
  static_cast<void>(harness.frame(graph, x, y, true));
  const editor::CompositorFrame opened = harness.frame(graph, x, y, false);
  UI_CHECK(opened.editing);
  UI_CHECK(!opened.property_edit.has_value());
  const editor::CompositorFrame typed =
      harness.frame(graph, x, y, false, value);
  UI_CHECK(typed.editing);
  UI_CHECK(!typed.property_edit.has_value());
  return harness.frame(graph, x, y, false, {}, gui::Key::Enter);
}

void compositor_scene_layers_show_the_referenced_element_name() {
  editor::ElementInfo model{};
  model.node = doc::NodeId{1000U};
  model.kind = editor::ElementKind::Model;
  model.display_name = "Hero Robot";
  std::array<editor::ElementInfo, 1U> items{model};
  UI_CHECK(editor::compositor_scene_layer_detail(model.node, items) ==
           "Hero Robot");
  items.front().display_name = "Robot Closeup";
  UI_CHECK(editor::compositor_scene_layer_detail(model.node, items) ==
           "Robot Closeup");
  UI_CHECK(editor::compositor_scene_layer_detail(doc::NodeId{2000U}, items) ==
           "OBJECT #2000");
}

void compositor_toolbar_reports_effect_add_intents() {
  const compositor::CompositorGraph graph;
  struct SearchCase final {
    std::string_view query;
    compositor::NodeKind kind;
  };
  constexpr std::array<SearchCase, 9U> searches{{
      {"exposure", compositor::NodeKind::Brightness},
      {"green screen", compositor::NodeKind::ChromaKey},
      {"gaussian", compositor::NodeKind::Blur},
      {"noise", compositor::NodeKind::FilmGrain},
      {"array", compositor::NodeKind::RadialArray},
      {"source object", compositor::NodeKind::SceneLayer},
      {"merge", compositor::NodeKind::Merge},
      {"stack", compositor::NodeKind::Composite},
      {"gradient", compositor::NodeKind::Background},
  }};
  for (const SearchCase &search : searches) {
    CompositorHarness harness;
    static_cast<void>(harness.frame(graph, 60.0F, 13.0F, false));
    static_cast<void>(harness.frame(graph, 60.0F, 13.0F, true));
    static_cast<void>(harness.frame(graph, 60.0F, 13.0F, false));
    static_cast<void>(harness.frame(graph, 80.0F, 42.0F, true));
    static_cast<void>(harness.frame(graph, 80.0F, 42.0F, false));
    static_cast<void>(harness.frame(graph, 80.0F, 42.0F, false, search.query));
    static_cast<void>(harness.frame(graph, 120.0F, 70.0F, false));
    static_cast<void>(harness.frame(graph, 120.0F, 70.0F, true));
    const editor::CompositorFrame result =
        harness.frame(graph, 120.0F, 70.0F, false);
    UI_CHECK(result.add_node.has_value());
    UI_CHECK(result.add_node->kind == search.kind);
    UI_CHECK(result.add_node->editor_x >= 0.0F);
    UI_CHECK(result.add_node->editor_y >= 0.0F);
    UI_CHECK(result.move_nodes.empty());
    UI_CHECK(!result.connect_ports.has_value());
  }
}

void compositor_output_creation_is_visible_and_unique() {
  const compositor::CompositorGraph empty;
  CompositorHarness add_harness;
  static_cast<void>(add_harness.frame(empty, 60.0F, 13.0F, false));
  static_cast<void>(add_harness.frame(empty, 60.0F, 13.0F, true));
  static_cast<void>(add_harness.frame(empty, 60.0F, 13.0F, false));
  static_cast<void>(add_harness.frame(empty, 80.0F, 42.0F, true));
  static_cast<void>(add_harness.frame(empty, 80.0F, 42.0F, false));
  static_cast<void>(add_harness.frame(empty, 80.0F, 42.0F, false, "output"));
  static_cast<void>(add_harness.frame(empty, 120.0F, 70.0F, false));
  static_cast<void>(add_harness.frame(empty, 120.0F, 70.0F, true));
  const editor::CompositorFrame added =
      add_harness.frame(empty, 120.0F, 70.0F, false);
  UI_CHECK(added.add_node.has_value());
  UI_CHECK(added.add_node->kind == compositor::NodeKind::Output);

  compositor::CompositorGraph configured;
  configured.nodes.push_back(
      graph_node(90U, compositor::NodeKind::Output, 320.0F, 60.0F));
  CompositorHarness disabled_harness;
  static_cast<void>(disabled_harness.frame(configured, 60.0F, 13.0F, false));
  static_cast<void>(disabled_harness.frame(configured, 60.0F, 13.0F, true));
  static_cast<void>(disabled_harness.frame(configured, 60.0F, 13.0F, false));
  static_cast<void>(disabled_harness.frame(configured, 80.0F, 42.0F, true));
  static_cast<void>(disabled_harness.frame(configured, 80.0F, 42.0F, false));
  static_cast<void>(
      disabled_harness.frame(configured, 80.0F, 42.0F, false, "output"));
  static_cast<void>(disabled_harness.frame(configured, 120.0F, 70.0F, false));
  static_cast<void>(disabled_harness.frame(configured, 120.0F, 70.0F, true));
  const editor::CompositorFrame rejected =
      disabled_harness.frame(configured, 120.0F, 70.0F, false);
  UI_CHECK(!rejected.add_node.has_value());
}

void selected_compositor_nodes_and_edges_return_delete_intents() {
  compositor::CompositorGraph node_graph;
  node_graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 60.0F, 40.0F));
  CompositorHarness node_harness;
  static_cast<void>(node_harness.frame(node_graph, 100.0F, 75.0F, false));
  const editor::CompositorFrame selected =
      node_harness.frame(node_graph, 100.0F, 75.0F, true);
  UI_CHECK(selected.selection ==
           editor::CompositorSelection{editor::CompositorSelectionKind::Node,
                                       doc::NodeId{1U}});
  static_cast<void>(node_harness.frame(node_graph, 100.0F, 75.0F, false));

  // Re-clicking the same node is surfaced so the shell can restore the graph
  // as the active Delete-key domain. (On its body: a second click on the
  // title would be a double-click, which renames.)
  const editor::CompositorFrame reselected =
      node_harness.frame(node_graph, 100.0F, 115.0F, true);
  UI_CHECK(reselected.selection ==
           editor::CompositorSelection{editor::CompositorSelectionKind::Node,
                                       doc::NodeId{1U}});
  static_cast<void>(node_harness.frame(node_graph, 100.0F, 115.0F, false));

  const editor::CompositorFrame node_deleted = node_harness.frame(
      node_graph, 100.0F, 115.0F, false, {}, gui::Key::Delete);
  UI_CHECK(node_deleted.remove ==
           editor::CompositorDeleteIntent{{editor::CompositorSelection{
               editor::CompositorSelectionKind::Node, doc::NodeId{1U}}}});
  UI_CHECK(node_deleted.consume_delete);
  const editor::CompositorFrame idle =
      node_harness.frame(node_graph, 100.0F, 75.0F, false);
  UI_CHECK(!idle.remove.has_value());
  UI_CHECK(!idle.consume_delete);

  // The visible toolbar action returns the same compact intent.
  constexpr float remove_x = 849.0F;
  constexpr float remove_y = 13.0F;
  static_cast<void>(node_harness.frame(node_graph, remove_x, remove_y, false));
  static_cast<void>(node_harness.frame(node_graph, remove_x, remove_y, true));
  const editor::CompositorFrame button_deleted =
      node_harness.frame(node_graph, remove_x, remove_y, false);
  UI_CHECK(button_deleted.remove ==
           editor::CompositorDeleteIntent{{editor::CompositorSelection{
               editor::CompositorSelectionKind::Node, doc::NodeId{1U}}}});

  compositor::CompositorGraph edge_graph;
  edge_graph.nodes.push_back(
      graph_node(10U, compositor::NodeKind::SceneLayer, 40.0F, 60.0F));
  edge_graph.nodes.push_back(
      graph_node(11U, compositor::NodeKind::Output, 320.0F, 60.0F));
  edge_graph.edges.push_back(compositor::GraphEdge{
      doc::NodeId{50U}, compositor::EdgeEndpoint{doc::NodeId{10U}, 0U},
      compositor::EdgeEndpoint{doc::NodeId{11U}, 0U}, 0});
  CompositorHarness edge_harness;
  constexpr float wire_x = 268.0F;
  constexpr float wire_y = 132.0F;
  static_cast<void>(edge_harness.frame(edge_graph, wire_x, wire_y, false));
  const editor::CompositorFrame edge_selected =
      edge_harness.frame(edge_graph, wire_x, wire_y, true);
  UI_CHECK(edge_selected.selection ==
           editor::CompositorSelection{editor::CompositorSelectionKind::Edge,
                                       doc::NodeId{50U}});
  static_cast<void>(edge_harness.frame(edge_graph, wire_x, wire_y, false));
  const editor::CompositorFrame disconnected = edge_harness.frame(
      edge_graph, wire_x, wire_y, false, {}, gui::Key::Delete);
  UI_CHECK(disconnected.remove ==
           editor::CompositorDeleteIntent{{editor::CompositorSelection{
               editor::CompositorSelectionKind::Edge, doc::NodeId{50U}}}});
  UI_CHECK(disconnected.consume_delete);
}

void led_car_process_returns_an_ordinary_node_delete_intent() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(7U, compositor::NodeKind::LedCarProcess, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  const editor::CompositorFrame deleted =
      harness.frame(graph, 100.0F, 75.0F, false, {}, gui::Key::Delete);
  UI_CHECK(deleted.remove ==
           editor::CompositorDeleteIntent{{editor::CompositorSelection{
               editor::CompositorSelectionKind::Node, doc::NodeId{7U}}}});
  UI_CHECK(deleted.consume_delete);
}

void the_required_output_node_cannot_be_deleted() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Output, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  const editor::CompositorFrame deleted =
      harness.frame(graph, 100.0F, 75.0F, false, {}, gui::Key::Delete);
  UI_CHECK(!deleted.remove.has_value());
  UI_CHECK(deleted.consume_delete);
}

void project_replacement_clears_compositor_document_state() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 60.0F, 40.0F));
  CompositorHarness harness;

  static_cast<void>(harness.frame(graph, 100.0F, 75.0F, false));
  static_cast<void>(harness.frame(graph, 100.0F, 75.0F, true));
  static_cast<void>(harness.frame(graph, 100.0F, 75.0F, false));
  UI_CHECK(harness.panel.selection() ==
           editor::CompositorSelection{editor::CompositorSelectionKind::Node,
                                       doc::NodeId{1U}});

  harness.panel.reset_project_state();
  UI_CHECK(harness.panel.selection() == editor::CompositorSelection{});
}

void compositor_navigation_zooms_under_the_pointer_and_pans_the_graph() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Output, 60.0F, 40.0F));
  CompositorHarness harness;
  constexpr float anchor_x = 450.0F;
  constexpr float anchor_y = 320.0F;
  constexpr float canvas_y = 26.0F;

  static_cast<void>(harness.navigation_frame(graph, anchor_x, anchor_y, false));
  static_cast<void>(
      harness.navigation_frame(graph, anchor_x, anchor_y, false, 2.0F));
  const float zoom = harness.panel.graph_zoom();
  const gui::UiPoint zoom_pan = harness.panel.graph_pan();
  UI_CHECK(zoom > 1.0F);
  UI_CHECK(std::abs((anchor_x - zoom_pan.x) / zoom - anchor_x) < 0.001F);
  UI_CHECK(std::abs((anchor_y - canvas_y - zoom_pan.y) / zoom -
                    (anchor_y - canvas_y)) < 0.001F);

  static_cast<void>(harness.navigation_frame(graph, anchor_x, anchor_y, true));
  static_cast<void>(harness.navigation_frame(graph, anchor_x + 120.0F,
                                             anchor_y + 60.0F, true));
  static_cast<void>(harness.navigation_frame(graph, anchor_x + 120.0F,
                                             anchor_y + 60.0F, false));
  const gui::UiPoint panned = harness.panel.graph_pan();
  UI_CHECK(std::abs(panned.x - (zoom_pan.x + 120.0F)) < 0.001F);
  UI_CHECK(std::abs(panned.y - (zoom_pan.y + 60.0F)) < 0.001F);

  const auto dropped = harness.panel.node_drop_position(
      anchor_x + 120.0F, anchor_y + 60.0F, 164.0F, 54.0F);
  UI_CHECK(dropped.has_value());
  UI_CHECK(std::abs(dropped->x -
                    ((anchor_x + 120.0F - panned.x) / zoom - 82.0F)) < 0.001F);
  UI_CHECK(std::abs(dropped->y -
                    ((anchor_y + 60.0F - canvas_y - panned.y) / zoom - 27.0F)) <
           0.001F);

  // Replacement picking follows the navigated, visible node geometry.
  const float node_x = panned.x + 60.0F * zoom;
  const float node_y = canvas_y + panned.y + 40.0F * zoom;
  const auto hit = harness.panel.node_at(graph, node_x + 30.0F, node_y + 20.0F);
  UI_CHECK(hit.has_value() && hit->first == doc::NodeId{1U});
  UI_CHECK(std::abs(hit->second.x - node_x) < 0.001F);
  UI_CHECK(std::abs(hit->second.y - node_y) < 0.001F);
  UI_CHECK(!harness.panel.node_at(graph, 10.0F, 13.0F).has_value());

  harness.panel.reset_project_state();
  UI_CHECK(harness.panel.graph_zoom() == 1.0F);
  UI_CHECK(harness.panel.graph_pan().x == 0.0F);
  UI_CHECK(harness.panel.graph_pan().y == 0.0F);
}

void compositor_replacement_picks_topmost_and_clips_feedback() {
  CompositorHarness harness;
  compositor::CompositorGraph graph;
  graph.nodes.push_back(graph_node(1U, compositor::NodeKind::SceneLayer, -30.0F, -20.0F));
  graph.nodes.push_back(graph_node(2U, compositor::NodeKind::Blur, -10.0F, -10.0F));
  static_cast<void>(harness.frame(graph, 50.0F, 40.0F, false));
  const auto hit = harness.panel.node_at(graph, 50.0F, 40.0F);
  UI_CHECK(hit.has_value() && hit->first == doc::NodeId{2U});
  UI_CHECK(hit->second.x == 0.0F && hit->second.y == 26.0F);
  // Blur's RADIUS row sits below its port: 35 + 2 * 22 + 7 = 86 tall, the
  // top 10 of it above the canvas.
  UI_CHECK(hit->second.width == 166.0F && hit->second.height == 76.0F);
  UI_CHECK(!harness.panel.node_at(graph, -1.0F, 40.0F).has_value());
  UI_CHECK(!harness.panel.node_at(graph, 20.0F, 20.0F).has_value());
}

void zoomed_compositor_node_dragging_still_authors_graph_coordinates() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Output, 60.0F, 40.0F));
  CompositorHarness harness;
  static_cast<void>(harness.navigation_frame(graph, 100.0F, 80.0F, false));
  static_cast<void>(
      harness.navigation_frame(graph, 100.0F, 80.0F, false, 2.0F));

  const float zoom = harness.panel.graph_zoom();
  const gui::UiPoint pan = harness.panel.graph_pan();
  const float node_x = pan.x + 60.0F * zoom;
  const float node_y = 26.0F + pan.y + 40.0F * zoom;
  const float press_x = node_x + 40.0F;
  const float press_y = node_y + 10.0F;
  static_cast<void>(harness.frame(graph, press_x, press_y, false));
  static_cast<void>(harness.frame(graph, press_x, press_y, true));
  const float release_x = press_x + 30.0F * zoom;
  const float release_y = press_y + 20.0F * zoom;
  static_cast<void>(harness.frame(graph, release_x, release_y, true));
  const editor::CompositorFrame moved =
      harness.frame(graph, release_x, release_y, false);
  UI_CHECK(moved.move_nodes.size() == 1U);
  UI_CHECK(std::abs(moved.move_nodes.front().editor_x - 90.0F) < 0.001F);
  UI_CHECK(std::abs(moved.move_nodes.front().editor_y - 60.0F) < 0.001F);
}

void dragging_a_compositor_node_reports_its_authored_position_on_release() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 60.0F, 40.0F));
  CompositorHarness harness;

  // Canvas starts at y=26. Pressing at (100,75) is 40px/9px into the
  // node. Releasing at (160,135) therefore authors local (120,100).
  static_cast<void>(harness.frame(graph, 100.0F, 75.0F, false));
  static_cast<void>(harness.frame(graph, 100.0F, 75.0F, true));
  const editor::CompositorFrame held =
      harness.frame(graph, 160.0F, 135.0F, true);
  UI_CHECK(held.move_nodes.empty());
  const editor::CompositorFrame result =
      harness.frame(graph, 160.0F, 135.0F, false);
  UI_CHECK(result.move_nodes.size() == 1U);
  UI_CHECK(result.move_nodes.front().node == doc::NodeId{1U});
  UI_CHECK(result.move_nodes.front().editor_x == 120.0F);
  UI_CHECK(result.move_nodes.front().editor_y == 100.0F);
}

void compatible_typed_ports_report_a_connection_intent() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::SceneLayer, 320.0F, 60.0F));
  CompositorHarness harness;

  // Both are effect ports. With the headless font a node is its minimum 176
  // wide, its header 35 tall and its port rows 22, so the first port centre
  // is 46 below the node top; the canvas begins at y=26.
  static_cast<void>(harness.frame(graph, 216.0F, 132.0F, false));
  static_cast<void>(harness.frame(graph, 216.0F, 132.0F, true));
  static_cast<void>(harness.frame(graph, 320.0F, 132.0F, true));
  const editor::CompositorFrame result =
      harness.frame(graph, 320.0F, 132.0F, false);
  UI_CHECK(result.connect_ports.has_value());
  UI_CHECK(result.connect_ports->from ==
           compositor::EdgeEndpoint{doc::NodeId{1U}, 0U});
  UI_CHECK(result.connect_ports->to ==
           compositor::EdgeEndpoint{doc::NodeId{2U}, 0U});
  UI_CHECK(!result.add_node.has_value());
  UI_CHECK(result.move_nodes.empty());
}

void dropping_on_a_single_input_reports_an_atomic_rewire() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::SceneLayer, 40.0F, 180.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::Composite, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(3U, compositor::NodeKind::Output, 320.0F, 60.0F));
  graph.edges.push_back(compositor::GraphEdge{
      doc::NodeId{50U}, compositor::EdgeEndpoint{doc::NodeId{1U}, 0U},
      compositor::EdgeEndpoint{doc::NodeId{3U}, 0U}, 0});
  CompositorHarness harness;

  // Composite IMAGE OUT replaces the existing Layer -> Output edge when it
  // is dropped on Output's occupied single IMAGE input.
  static_cast<void>(harness.frame(graph, 216.0F, 132.0F, false));
  static_cast<void>(harness.frame(graph, 216.0F, 132.0F, true));
  static_cast<void>(harness.frame(graph, 320.0F, 132.0F, true));
  const editor::CompositorFrame result =
      harness.frame(graph, 320.0F, 132.0F, false);

  UI_CHECK(result.connect_ports ==
           editor::CompositorConnectIntent{
               compositor::EdgeEndpoint{doc::NodeId{2U}, 0U},
               compositor::EdgeEndpoint{doc::NodeId{3U}, 0U},
               doc::NodeId{50U}});
}

void incompatible_ports_are_rejected_and_persisted_edges_draw_wires() {
  compositor::CompositorGraph incompatible;
  incompatible.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Blur, 40.0F, 60.0F));
  incompatible.nodes.push_back(
      graph_node(2U, compositor::NodeKind::Output, 320.0F, 60.0F));
  CompositorHarness incompatible_harness;
  static_cast<void>(
      incompatible_harness.frame(incompatible, 216.0F, 132.0F, false));
  static_cast<void>(
      incompatible_harness.frame(incompatible, 216.0F, 132.0F, true));
  static_cast<void>(
      incompatible_harness.frame(incompatible, 320.0F, 132.0F, true));
  const editor::CompositorFrame rejected =
      incompatible_harness.frame(incompatible, 320.0F, 132.0F, false);
  UI_CHECK(!rejected.connect_ports.has_value());

  compositor::CompositorGraph connected;
  connected.nodes.push_back(
      graph_node(10U, compositor::NodeKind::SceneLayer, 40.0F, 60.0F));
  connected.nodes.push_back(
      graph_node(11U, compositor::NodeKind::Output, 320.0F, 60.0F));
  CompositorHarness draw_harness;
  static_cast<void>(draw_harness.frame(connected, 0.0F, 0.0F, false));
  const std::size_t without_wire = draw_harness.list.vertices().size();
  connected.edges.push_back(compositor::GraphEdge{
      doc::NodeId{50U}, compositor::EdgeEndpoint{doc::NodeId{10U}, 0U},
      compositor::EdgeEndpoint{doc::NodeId{11U}, 0U}, 0});
  static_cast<void>(draw_harness.frame(connected, 0.0F, 0.0F, false));
  UI_CHECK(draw_harness.list.vertices().size() > without_wire);
}

void compositor_diagnostics_are_explained_in_plain_language() {
  constexpr std::array<compositor::DiagnosticCode, 15U> codes{{
      compositor::DiagnosticCode::NodesOverCapacity,
      compositor::DiagnosticCode::EdgesOverCapacity,
      compositor::DiagnosticCode::DanglingNode,
      compositor::DiagnosticCode::DanglingSceneReference,
      compositor::DiagnosticCode::UnknownPort,
      compositor::DiagnosticCode::IncompatiblePorts,
      compositor::DiagnosticCode::DuplicateInput,
      compositor::DiagnosticCode::MissingOutput,
      compositor::DiagnosticCode::DuplicateOutput,
      compositor::DiagnosticCode::DuplicateSceneLayerReference,
      compositor::DiagnosticCode::Cycle,
      compositor::DiagnosticCode::DeadBranch,
      compositor::DiagnosticCode::PassBudgetExceeded,
      compositor::DiagnosticCode::TextureBudgetExceeded,
      compositor::DiagnosticCode::TapBudgetExceeded,
  }};
  for (const compositor::DiagnosticCode code : codes) {
    const std::string_view message = editor::compositor_diagnostic_message(
        compositor::GraphDiagnostic{code, {}, {}, 0U});
    UI_CHECK(!message.empty());
    UI_CHECK(message.find("Graph is not ready") == std::string_view::npos);
  }
  UI_CHECK(editor::compositor_diagnostic_message(compositor::GraphDiagnostic{
               compositor::DiagnosticCode::IncompatiblePorts, {}, {}, 0U}) ==
           "A wire joins different kinds of port; filters go into FILTERS");
  UI_CHECK(editor::compositor_diagnostic_message(compositor::GraphDiagnostic{
               compositor::DiagnosticCode::MissingOutput, {}, {}, 0U}) ==
           "Add and connect one Output node");
  UI_CHECK(editor::compositor_diagnostic_message(compositor::GraphDiagnostic{
               compositor::DiagnosticCode::Cycle, {}, {}, 0U}) ==
           "Cycle detected; wires must flow toward Output");
  UI_CHECK(editor::compositor_diagnostic_message(compositor::GraphDiagnostic{
               compositor::DiagnosticCode::DuplicateSceneLayerReference,
               {},
               {},
               0U}) == "One active Scene Layer per source is supported");
  UI_CHECK(editor::compositor_diagnostic_message(compositor::GraphDiagnostic{
               compositor::DiagnosticCode::DeadBranch,
               {},
               {},
               0U,
               compositor::DiagnosticSeverity::Warning}) ==
           "This branch does not reach Output");
  UI_CHECK(
      editor::compositor_diagnostic_severity_label(compositor::GraphDiagnostic{
          compositor::DiagnosticCode::MissingOutput, {}, {}, 0U}) == "Error");
  UI_CHECK(
      editor::compositor_diagnostic_severity_label(compositor::GraphDiagnostic{
          compositor::DiagnosticCode::DeadBranch,
          {},
          {},
          0U,
          compositor::DiagnosticSeverity::Warning}) == "Warning");

  compositor::CompositorGraph ready;
  ready.nodes.push_back(
      graph_node(10U, compositor::NodeKind::SceneLayer, 180.0F, 90.0F));
  CompositorHarness ready_harness;
  static_cast<void>(ready_harness.frame(ready, 0.0F, 0.0F, false));
  const std::size_t without_guide = ready_harness.list.vertices().size();

  compositor::CompositorGraph invalid = ready;
  invalid.diagnostics.push_back(compositor::GraphDiagnostic{
      compositor::DiagnosticCode::MissingOutput, {}, {}, 0U});
  CompositorHarness invalid_harness;
  static_cast<void>(invalid_harness.frame(invalid, 0.0F, 0.0F, false));
  UI_CHECK(invalid_harness.list.vertices().size() > without_guide);

  // A parked node - kept, reaching no Output - is not a problem: it is
  // faded, one soft veil in the ground colour over its card, and raises no
  // guide and no outline. Everything it adds over a ready node is that veil.
  compositor::CompositorGraph parked = ready;
  parked.diagnostics.push_back(
      compositor::GraphDiagnostic{compositor::DiagnosticCode::DeadBranch,
                                  doc::NodeId{10U},
                                  {},
                                  0U,
                                  compositor::DiagnosticSeverity::Warning});
  CompositorHarness parked_harness;
  static_cast<void>(parked_harness.frame(parked, 0.0F, 0.0F, false));
  const gui::UiColor ground = parked_harness.ctx.theme().background;
  const auto veil_vertices = [&ground](const gui::UiDrawList &list) {
    return static_cast<std::size_t>(std::count_if(
        list.vertices().begin(), list.vertices().end(),
        [&ground](const gui::UiVertex &vertex) {
          return vertex.r == ground.r && vertex.g == ground.g &&
                 vertex.b == ground.b &&
                 (std::abs(vertex.a - 0.45F) < 1.0e-4F || vertex.a == 0.0F);
        }));
  };
  const std::size_t veil =
      veil_vertices(parked_harness.list) - veil_vertices(ready_harness.list);
  UI_CHECK(veil > 0U);
  UI_CHECK(parked_harness.list.vertices().size() == without_guide + veil);

  // A real warning still raises the guide.
  compositor::CompositorGraph warning = ready;
  warning.diagnostics.push_back(compositor::GraphDiagnostic{
      compositor::DiagnosticCode::PassBudgetExceeded, {}, {}, 0U,
      compositor::DiagnosticSeverity::Warning});
  CompositorHarness warning_harness;
  static_cast<void>(warning_harness.frame(warning, 0.0F, 0.0F, false));
  UI_CHECK(warning_harness.list.vertices().size() > without_guide);

  // An Output waiting for its image is not an error either: it is disabled
  // and faded, and the start guide says Program shows the scene meanwhile.
  compositor::CompositorGraph wired;
  wired.nodes.push_back(
      graph_node(20U, compositor::NodeKind::Output, 180.0F, 90.0F));
  compositor::CompositorGraph waiting = wired;
  waiting.node_metadata.push_back(compositor::GraphNodeMetadata{
      .node = doc::NodeId{20U}, .disabled = true});
  CompositorHarness wired_harness;
  static_cast<void>(wired_harness.frame(wired, 0.0F, 0.0F, false));
  CompositorHarness waiting_harness;
  static_cast<void>(waiting_harness.frame(waiting, 0.0F, 0.0F, false));
  // The guide's panel and outline, and the fade's quad.
  UI_CHECK(waiting_harness.list.vertices().size() >=
           wired_harness.list.vertices().size() + 36U);
}

// The values an operator reaches for most are on the node itself: pressing
// one chooses the node and edits the value, exactly as Parameters does.
void inline_node_values_edit_like_parameters() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(70U, compositor::NodeKind::Brightness, 60.0F, 40.0F));
  graph.nodes.push_back(
      graph_node(71U, compositor::NodeKind::BoxBlur, 60.0F, 200.0F));
  CompositorHarness harness;
  // Brightness at (60, 66), 176 wide: its EFFECT row, then AMOUNT at
  // y=123..145 with its field at x=139..223.
  static_cast<void>(harness.frame(graph, 180.0F, 134.0F, false));
  const editor::CompositorFrame pressed =
      harness.frame(graph, 180.0F, 134.0F, true);
  UI_CHECK(pressed.selection ==
           (editor::CompositorSelection{editor::CompositorSelectionKind::Node,
                                        doc::NodeId{70U}}));
  const editor::CompositorFrame opened =
      harness.frame(graph, 180.0F, 134.0F, false);
  UI_CHECK(opened.editing);
  UI_CHECK(opened.move_nodes.empty());
  static_cast<void>(harness.frame(graph, 180.0F, 134.0F, false, "1.75"));
  const editor::CompositorFrame committed = harness.frame(
      graph, 180.0F, 134.0F, false, {}, gui::Key::Enter);
  UI_CHECK(committed.property_edit.has_value());
  UI_CHECK(committed.property_edit->node == doc::NodeId{70U});
  UI_CHECK(committed.property_edit->property ==
           editor::CompositorProperty::BrightnessAmount);
  UI_CHECK(std::get<float>(committed.property_edit->value) == 1.75F);
  UI_CHECK(committed.move_nodes.empty());

  // A whole-number value is rounded and kept in range: Box Blur's PASSES
  // row is its second, y=305..327 below its top at 226.
  const editor::CompositorFrame passes =
      commit_compositor_field(harness, graph, 180.0F, 316.0F, "2.6");
  UI_CHECK(passes.property_edit.has_value());
  UI_CHECK(passes.property_edit->node == doc::NodeId{71U});
  UI_CHECK(passes.property_edit->property ==
           editor::CompositorProperty::BoxBlurPasses);
  UI_CHECK(std::get<core::u32>(passes.property_edit->value) == 3U);
  const editor::CompositorFrame clamped =
      commit_compositor_field(harness, graph, 180.0F, 316.0F, "400");
  UI_CHECK(clamped.property_edit.has_value());
  UI_CHECK(std::get<core::u32>(clamped.property_edit->value) ==
           compositor::max_box_blur_passes);
}

// The chevron on a node's title collapses it to its title and the ports that
// are wired; hidden ports are no longer targets. The node keeps its shape
// while the toggle is on its way to the Director.
void a_collapsed_node_keeps_only_its_wired_ports() {
  compositor::CompositorGraph graph;
  // In id order, as the evaluated graph lists them.
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::SceneLayer, 300.0F, 40.0F));
  graph.nodes.push_back(
      graph_node(3U, compositor::NodeKind::Brightness, 60.0F, 40.0F));
  graph.nodes.push_back(
      graph_node(4U, compositor::NodeKind::Blur, 60.0F, 300.0F));
  graph.edges.push_back(compositor::GraphEdge{
      doc::NodeId{40U}, compositor::EdgeEndpoint{doc::NodeId{3U}, 0U},
      compositor::EdgeEndpoint{doc::NodeId{1U}, 0U}, 0});
  CompositorHarness harness;
  static_cast<void>(harness.frame(graph, 0.0F, 0.0F, false));
  // The Scene Layer at (300, 66): EFFECTS on row 0, FILTERS (port 2) on row
  // 1, INSTANCES on row 2.
  UI_CHECK(harness.panel.input_port_at(graph, 302.0F, 134.0F) ==
           (compositor::EdgeEndpoint{doc::NodeId{1U}, 2U}));
  const auto open = harness.panel.node_at(graph, 380.0F, 90.0F);
  UI_CHECK(open.has_value() && open->second.height == 108.0F);

  // Its toggle is at the right end of the title line, ending at x=472.
  static_cast<void>(harness.frame(graph, 465.0F, 78.0F, false));
  static_cast<void>(harness.frame(graph, 465.0F, 78.0F, true));
  const editor::CompositorFrame folded =
      harness.frame(graph, 465.0F, 78.0F, false);
  UI_CHECK(folded.collapse ==
           (editor::CompositorCollapseIntent{{doc::NodeId{1U}}, true}));
  UI_CHECK(folded.move_nodes.empty());
  UI_CHECK(!folded.selection.has_value());

  // Held until the graph says so, then as authored: the wired EFFECTS
  // input stays where it was, the rest go.
  for (int pass = 0; pass < 2; ++pass) {
    static_cast<void>(harness.frame(graph, 0.0F, 0.0F, false));
    const auto shown = harness.panel.node_at(graph, 380.0F, 90.0F);
    UI_CHECK(shown.has_value() && shown->second.height == 64.0F);
    UI_CHECK(harness.panel.input_port_at(graph, 302.0F, 112.0F) ==
             (compositor::EdgeEndpoint{doc::NodeId{1U}, 0U}));
    UI_CHECK(!harness.panel.input_port_at(graph, 302.0F, 134.0F).has_value());
    graph.nodes[0].collapsed = true;
  }

  // An unwired node collapses to its title alone, and opens again.
  static_cast<void>(harness.frame(graph, 225.0F, 338.0F, false));
  static_cast<void>(harness.frame(graph, 225.0F, 338.0F, true));
  const editor::CompositorFrame blur_folded =
      harness.frame(graph, 225.0F, 338.0F, false);
  UI_CHECK(blur_folded.collapse ==
           (editor::CompositorCollapseIntent{{doc::NodeId{4U}}, true}));
  static_cast<void>(harness.frame(graph, 0.0F, 0.0F, false));
  const auto title_only = harness.panel.node_at(graph, 100.0F, 340.0F);
  UI_CHECK(title_only.has_value() && title_only->second.height == 42.0F);
  graph.nodes[2].collapsed = true;
  static_cast<void>(harness.frame(graph, 225.0F, 338.0F, false));
  static_cast<void>(harness.frame(graph, 225.0F, 338.0F, true));
  const editor::CompositorFrame opened_again =
      harness.frame(graph, 225.0F, 338.0F, false);
  UI_CHECK(opened_again.collapse ==
           (editor::CompositorCollapseIntent{{doc::NodeId{4U}}, false}));
  static_cast<void>(harness.frame(graph, 0.0F, 0.0F, false));
  const auto reopened = harness.panel.node_at(graph, 100.0F, 340.0F);
  UI_CHECK(reopened.has_value() && reopened->second.height == 86.0F);
}

// An effect dropped on an effect wire joins that wire's input right after
// the wire's own effect; the input's wires are renumbered to make room.
void an_effect_wire_drop_goes_after_that_wires_effect() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::SceneLayer, 300.0F, 40.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::Brightness, 40.0F, 40.0F));
  graph.nodes.push_back(
      graph_node(3U, compositor::NodeKind::Saturation, 40.0F, 260.0F));
  // Equal orders: the input takes them by wire id, 20 then 21.
  graph.edges.push_back(compositor::GraphEdge{
      doc::NodeId{20U}, compositor::EdgeEndpoint{doc::NodeId{2U}, 0U},
      compositor::EdgeEndpoint{doc::NodeId{1U}, 0U}, 0});
  graph.edges.push_back(compositor::GraphEdge{
      doc::NodeId{21U}, compositor::EdgeEndpoint{doc::NodeId{3U}, 0U},
      compositor::EdgeEndpoint{doc::NodeId{1U}, 0U}, 0});
  CompositorHarness harness;
  static_cast<void>(harness.frame(graph, 0.0F, 0.0F, false));

  // Brightness's wire runs level from (216, 112) to (300, 112).
  const auto first = harness.panel.effect_wire_at(graph, 258.0F, 114.0F, compositor::PortValueType::Effect);
  UI_CHECK(first.has_value());
  UI_CHECK(first->edge == doc::NodeId{20U});
  UI_CHECK(first->input == (compositor::EdgeEndpoint{doc::NodeId{1U}, 0U}));
  UI_CHECK(first->order == 1);
  UI_CHECK(first->reorder ==
           (std::vector<std::pair<doc::NodeId, core::i64>>{
               {doc::NodeId{21U}, 2}}));
  UI_CHECK(first->after == "Brightness");
  UI_CHECK(!harness.panel.effect_wire_at(graph, 258.0F, 600.0F, compositor::PortValueType::Effect).has_value());

  // Gaps are closed up as room is made; after the last, it goes last.
  graph.edges[1].order = 4;
  static_cast<void>(harness.frame(graph, 0.0F, 0.0F, false));
  const auto spaced = harness.panel.effect_wire_at(graph, 258.0F, 114.0F, compositor::PortValueType::Effect);
  UI_CHECK(spaced.has_value() && spaced->order == 1);
  UI_CHECK(spaced->reorder ==
           (std::vector<std::pair<doc::NodeId, core::i64>>{
               {doc::NodeId{21U}, 2}}));
  graph.edges[1].order = 2;
  graph.edges[0].order = 7;
  static_cast<void>(harness.frame(graph, 0.0F, 0.0F, false));
  const auto last = harness.panel.effect_wire_at(graph, 258.0F, 114.0F, compositor::PortValueType::Effect);
  UI_CHECK(last.has_value() && last->order == 3);
  UI_CHECK(last->reorder ==
           (std::vector<std::pair<doc::NodeId, core::i64>>{
               {doc::NodeId{21U}, 0}, {doc::NodeId{20U}, 2}}));
}

// A layer chosen elsewhere brings its Scene Layer - or its Track's - into
// the middle of the view, without choosing it; one already in view stays put.
void revealing_an_element_brings_its_scene_layer_into_view() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::SceneLayer, 1500.0F, 900.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::SceneLayer, 60.0F, 40.0F));
  graph.nodes[1].parameters =
      compositor::SceneLayerParameters{doc::NodeId{3000U}};
  editor::ElementInfo clip{};
  clip.node = doc::NodeId{2000U};
  clip.kind = editor::ElementKind::Media;
  clip.timeline_lane = doc::NodeId{1000U};
  const std::array<editor::ElementInfo, 1U> elements{clip};
  CompositorHarness harness;
  const auto draw = [&](float x, float y) {
    harness.input.new_frame();
    harness.input.set_mouse_position(x, y);
    harness.input.set_mouse_button(gui::MouseButton::Left, false);
    harness.ctx.new_frame();
    return harness.panel.draw(harness.ctx, harness.bounds, graph, elements);
  };
  static_cast<void>(draw(0.0F, 0.0F));
  UI_CHECK(!harness.panel.node_at(graph, 450.0F, 333.0F).has_value());

  // The clip plays on Track 1000, which node 1 shows.
  harness.panel.reveal_element(clip.node, 0.0);
  const editor::CompositorFrame revealed = draw(0.0F, 0.0F);
  UI_CHECK(!revealed.selection.has_value());
  UI_CHECK(harness.panel.graph_zoom() == 1.0F);
  // The canvas is 900 x 614 below its toolbar, so its middle is (450, 333).
  const auto centred = harness.panel.node_at(graph, 450.0F, 333.0F);
  UI_CHECK(centred.has_value() && centred->first == doc::NodeId{1U});
  UI_CHECK(harness.panel.revealed_node(1.0) == doc::NodeId{1U});
  UI_CHECK(harness.panel.revealed_node(2.0) == doc::NodeId{});

  // Already in view, revealed again by its own element: only the outline
  // shows it.
  const gui::UiPoint pan = harness.panel.graph_pan();
  harness.panel.reveal_element(doc::NodeId{1000U}, 4.0);
  static_cast<void>(draw(0.0F, 0.0F));
  UI_CHECK(harness.panel.graph_pan().x == pan.x &&
           harness.panel.graph_pan().y == pan.y);
  UI_CHECK(harness.panel.revealed_node(5.0) == doc::NodeId{1U});
  // Nothing in the graph shows it: nothing happens.
  harness.panel.reveal_element(doc::NodeId{9999U}, 6.0);
  static_cast<void>(draw(0.0F, 0.0F));
  UI_CHECK(harness.panel.graph_pan().x == pan.x);
  UI_CHECK(harness.panel.revealed_node(6.5) == doc::NodeId{});
}

// D, the node menu and the Parameters button bypass the selection's
// bypassable nodes, or restore them once all are; the node shows it at once.
void bypass_toggles_the_selections_bypassable_nodes() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 60.0F, 40.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::Output, 60.0F, 300.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);
  // D, with the pointer over the canvas.
  const editor::CompositorFrame bypassed =
      harness.frame(graph, 400.0F, 400.0F, false, "d");
  UI_CHECK(bypassed.bypass ==
           (editor::CompositorBypassIntent{{doc::NodeId{1U}}, true}));
  UI_CHECK(!bypassed.property_edit.has_value());
  // Held until the graph reports it, then read from the graph: D restores.
  static_cast<void>(harness.frame(graph, 400.0F, 400.0F, false));
  graph.nodes[0].bypassed = true;
  static_cast<void>(harness.frame(graph, 400.0F, 400.0F, false));
  const editor::CompositorFrame restored =
      harness.frame(graph, 400.0F, 400.0F, false, "d");
  UI_CHECK(restored.bypass ==
           (editor::CompositorBypassIntent{{doc::NodeId{1U}}, false}));

  // The Parameters button ends 8 short of the panel's right edge, at the
  // top of the panel below the toolbar.
  graph.nodes[0].bypassed = false;
  static_cast<void>(harness.frame(graph, 885.0F, 40.0F, false));
  static_cast<void>(harness.frame(graph, 885.0F, 40.0F, false));
  static_cast<void>(harness.frame(graph, 885.0F, 40.0F, true));
  const editor::CompositorFrame clicked =
      harness.frame(graph, 885.0F, 40.0F, false);
  UI_CHECK(clicked.bypass ==
           (editor::CompositorBypassIntent{{doc::NodeId{1U}}, true}));

  // Output cannot be bypassed: with only it selected, D does nothing.
  CompositorHarness output;
  static_cast<void>(output.frame(graph, 100.0F, 335.0F, false));
  static_cast<void>(output.frame(graph, 100.0F, 335.0F, true));
  static_cast<void>(output.frame(graph, 100.0F, 335.0F, false));
  const editor::CompositorFrame ignored =
      output.frame(graph, 400.0F, 400.0F, false, "d");
  UI_CHECK(!ignored.bypass.has_value());
}

// Chroma Key, Luma Key and Matte are edited in Parameters like every other
// node: numbers scrub or type, switches flip, and a Luma Key's thresholds
// stop at each other.
void keyer_and_matte_parameters_emit_typed_edits() {
  // The first field's row is at y=69, each next one 26 below; x=820 is in
  // the field column.
  const auto field_y = [](int row) { return 69.0F + 26.0F * static_cast<float>(row); };
  {
    compositor::CompositorGraph graph;
    graph.nodes.push_back(
        graph_node(80U, compositor::NodeKind::ChromaKey, 60.0F, 40.0F));
    graph.nodes.back().parameters = compositor::ChromaKeyParameters{};
    CompositorHarness harness;
    select_compositor_node(harness, graph);
    // KEY COLOUR's swatch, then KEY RED, KEY GREEN.
    const editor::CompositorFrame green =
        commit_compositor_field(harness, graph, 820.0F, field_y(2), "0.75");
    UI_CHECK(green.property_edit.has_value());
    UI_CHECK(green.property_edit->node == doc::NodeId{80U});
    UI_CHECK(green.property_edit->property ==
             editor::CompositorProperty::ChromaKeyGreen);
    UI_CHECK(std::get<float>(green.property_edit->value) == 0.75F);
    // EDGE PX is signed and whole, and stops at the largest reach.
    const editor::CompositorFrame edge =
        commit_compositor_field(harness, graph, 820.0F, field_y(6), "-3.4");
    UI_CHECK(edge.property_edit.has_value());
    UI_CHECK(edge.property_edit->property ==
             editor::CompositorProperty::ChromaKeyEdgePx);
    UI_CHECK(std::get<float>(edge.property_edit->value) == -3.0F);
    const editor::CompositorFrame far =
        commit_compositor_field(harness, graph, 820.0F, field_y(6), "-500");
    UI_CHECK(std::get<float>(far.property_edit->value) ==
             -static_cast<float>(scene::max_key_morphology_radius_px));
  }
  {
    compositor::CompositorGraph graph;
    graph.nodes.push_back(
        graph_node(81U, compositor::NodeKind::LumaKey, 60.0F, 40.0F));
    compositor::LumaKeyParameters luma{};
    luma.low_threshold = 0.2F;
    luma.high_threshold = 0.6F;
    graph.nodes.back().parameters = luma;
    CompositorHarness harness;
    select_compositor_node(harness, graph);
    // LOW cannot pass HIGH.
    const editor::CompositorFrame low =
        commit_compositor_field(harness, graph, 820.0F, field_y(0), "0.9");
    UI_CHECK(low.property_edit.has_value());
    UI_CHECK(low.property_edit->property ==
             editor::CompositorProperty::LumaKeyLow);
    UI_CHECK(std::get<float>(low.property_edit->value) == 0.6F);
    // MATTE flips NORMAL to INVERTED.
    static_cast<void>(harness.frame(graph, 820.0F, field_y(4), false));
    static_cast<void>(harness.frame(graph, 820.0F, field_y(4), true));
    const editor::CompositorFrame inverted =
        harness.frame(graph, 820.0F, field_y(4), false);
    UI_CHECK(inverted.property_edit ==
             (editor::CompositorPropertyEditIntent{
                 doc::NodeId{81U}, editor::CompositorProperty::LumaKeyInvert,
                 editor::CompositorPropertyValue{core::u32{1U}}}));
  }
  {
    compositor::CompositorGraph graph;
    graph.nodes.push_back(
        graph_node(82U, compositor::NodeKind::Matte, 60.0F, 40.0F));
    graph.nodes.back().parameters = compositor::MatteParameters{};
    CompositorHarness harness;
    select_compositor_node(harness, graph);
    // MODE flips ALPHA to LUMA; CLAMP INPUT, on by default, turns off.
    static_cast<void>(harness.frame(graph, 820.0F, field_y(0), false));
    static_cast<void>(harness.frame(graph, 820.0F, field_y(0), true));
    const editor::CompositorFrame mode =
        harness.frame(graph, 820.0F, field_y(0), false);
    UI_CHECK(mode.property_edit ==
             (editor::CompositorPropertyEditIntent{
                 doc::NodeId{82U}, editor::CompositorProperty::MatteMode,
                 editor::CompositorPropertyValue{core::u32{1U}}}));
    static_cast<void>(harness.frame(graph, 820.0F, field_y(2), false));
    static_cast<void>(harness.frame(graph, 820.0F, field_y(2), true));
    const editor::CompositorFrame clamp =
        harness.frame(graph, 820.0F, field_y(2), false);
    UI_CHECK(clamp.property_edit ==
             (editor::CompositorPropertyEditIntent{
                 doc::NodeId{82U}, editor::CompositorProperty::MatteClampInput,
                 editor::CompositorPropertyValue{core::u32{0U}}}));
  }
}

// Merge and Background are edited in Parameters: opacity, the constant and
// a gradient's placement are numbers kept in range, and a gradient's angle
// wraps to one turn.
void merge_and_background_parameters_emit_typed_edits() {
  const auto field_y = [](int row) {
    return 69.0F + 26.0F * static_cast<float>(row);
  };
  {
    compositor::CompositorGraph graph;
    graph.nodes.push_back(
        graph_node(90U, compositor::NodeKind::Merge, 60.0F, 40.0F));
    graph.nodes.back().parameters = compositor::MergeParameters{
        compositor::MergeBlendMode::Multiply, 1.0F, 1.0F};
    CompositorHarness harness;
    select_compositor_node(harness, graph);
    // BLEND, then OPACITY and VALUE.
    const editor::CompositorFrame opacity =
        commit_compositor_field(harness, graph, 820.0F, field_y(1), "0.5");
    UI_CHECK(opacity.property_edit ==
             (editor::CompositorPropertyEditIntent{
                 doc::NodeId{90U}, editor::CompositorProperty::MergeOpacity,
                 editor::CompositorPropertyValue{0.5F}}));
    const editor::CompositorFrame value =
        commit_compositor_field(harness, graph, 820.0F, field_y(2), "40");
    UI_CHECK(value.property_edit ==
             (editor::CompositorPropertyEditIntent{
                 doc::NodeId{90U}, editor::CompositorProperty::MergeValue,
                 editor::CompositorPropertyValue{compositor::max_merge_value}}));
  }
  {
    compositor::CompositorGraph graph;
    graph.nodes.push_back(
        graph_node(91U, compositor::NodeKind::Background, 60.0F, 40.0F));
    compositor::BackgroundParameters linear{};
    linear.mode = compositor::BackgroundMode::Linear;
    graph.nodes.back().parameters = linear;
    CompositorHarness harness;
    select_compositor_node(harness, graph);
    // TYPE; COLOUR A's swatch and channels; COLOUR B's swatch and channels;
    // ANGLE TURNS, CENTER X, CENTER Y and SIZE.
    const editor::CompositorFrame alpha =
        commit_compositor_field(harness, graph, 820.0F, field_y(10), "0.25");
    UI_CHECK(alpha.property_edit ==
             (editor::CompositorPropertyEditIntent{
                 doc::NodeId{91U},
                 editor::CompositorProperty::BackgroundColorBA,
                 editor::CompositorPropertyValue{0.25F}}));
    const editor::CompositorFrame angle =
        commit_compositor_field(harness, graph, 820.0F, field_y(11), "1.25");
    UI_CHECK(angle.property_edit ==
             (editor::CompositorPropertyEditIntent{
                 doc::NodeId{91U},
                 editor::CompositorProperty::BackgroundAngleTurns,
                 editor::CompositorPropertyValue{0.25F}}));
    const editor::CompositorFrame size =
        commit_compositor_field(harness, graph, 820.0F, field_y(14), "0");
    UI_CHECK(size.property_edit ==
             (editor::CompositorPropertyEditIntent{
                 doc::NodeId{91U}, editor::CompositorProperty::BackgroundSize,
                 editor::CompositorPropertyValue{0.01F}}));

    // A solid colour has nothing to place: its last field is A's ALPHA.
    graph.nodes.back().parameters = compositor::BackgroundParameters{};
    const editor::CompositorFrame solid =
        commit_compositor_field(harness, graph, 820.0F, field_y(5), "0.5");
    UI_CHECK(solid.property_edit ==
             (editor::CompositorPropertyEditIntent{
                 doc::NodeId{91U},
                 editor::CompositorProperty::BackgroundColorAA,
                 editor::CompositorPropertyValue{0.5F}}));
  }
  UI_CHECK(editor::compositor_diagnostic_message(compositor::GraphDiagnostic{
               compositor::DiagnosticCode::MergeNestingTooDeep, {}, {}, 0U}) ==
           "Merges are nested more than 4 deep; flatten one");
  UI_CHECK(editor::compositor_diagnostic_message(compositor::GraphDiagnostic{
               compositor::DiagnosticCode::UnsupportedMatteTopology,
               {},
               {},
               0U}) ==
           "Matte needs a Scene Layer source and a Scene Layer or Background "
           "matte");
}

// Wires sharing an input are reordered from the wire's menu, or by dragging
// a wire's number onto another's. The whole input is renumbered at once.
void an_inputs_wires_are_reordered_by_menu_and_by_number() {
  compositor::CompositorGraph graph;
  // Both effects run level into the Scene Layer's EFFECTS input at
  // (300, 112), Saturation from further left, so the two wires lie along
  // one line and their numbers sit together 30 short of the input.
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 40.0F, 40.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::Saturation, -260.0F, 40.0F));
  graph.nodes.push_back(
      graph_node(10U, compositor::NodeKind::SceneLayer, 300.0F, 40.0F));
  graph.edges.push_back(compositor::GraphEdge{
      doc::NodeId{20U}, compositor::EdgeEndpoint{doc::NodeId{1U}, 0U},
      compositor::EdgeEndpoint{doc::NodeId{10U}, 0U}, 0});
  graph.edges.push_back(compositor::GraphEdge{
      doc::NodeId{21U}, compositor::EdgeEndpoint{doc::NodeId{2U}, 0U},
      compositor::EdgeEndpoint{doc::NodeId{10U}, 0U}, 1});
  const editor::CompositorReorderIntent swapped{
      {{doc::NodeId{21U}, 0}, {doc::NodeId{20U}, 1}}};
  {
    // The wire's menu: Add Reroute Point, Disconnect, then after a divider
    // Move Earlier and Move Later. Where the two lie together it opens for
    // the one drawn on top, 21, which is last and cannot move later.
    const auto choose = [&](std::size_t row) {
      CompositorHarness harness;
      static_cast<void>(harness.frame(graph, 250.0F, 112.0F, false));
      static_cast<void>(harness.secondary(graph, 250.0F, 112.0F, true));
      static_cast<void>(harness.secondary(graph, 250.0F, 112.0F, false));
      const float item_y = 112.0F + 4.0F + 23.0F * static_cast<float>(row) +
                           11.5F + (row >= 2U ? 7.0F : 0.0F);
      static_cast<void>(harness.frame(graph, 290.0F, item_y, false));
      static_cast<void>(harness.frame(graph, 290.0F, item_y, true));
      return harness.frame(graph, 290.0F, item_y, false);
    };
    UI_CHECK(choose(2U).reorder == swapped);
    UI_CHECK(!choose(3U).reorder.has_value());
  }
  {
    CompositorHarness harness;
    static_cast<void>(harness.frame(graph, 0.0F, 0.0F, false));
    static_cast<void>(harness.frame(graph, 270.0F, 112.0F, false));
    const editor::CompositorFrame pressed =
        harness.frame(graph, 270.0F, 112.0F, true);
    UI_CHECK(pressed.selection ==
             (editor::CompositorSelection{editor::CompositorSelectionKind::Edge,
                                          doc::NodeId{20U}}));
    static_cast<void>(harness.frame(graph, 280.0F, 120.0F, true));
    const editor::CompositorFrame dropped =
        harness.frame(graph, 271.0F, 113.0F, false);
    UI_CHECK(dropped.reorder == swapped);
    UI_CHECK(dropped.move_nodes.empty());
    UI_CHECK(!dropped.rewire.has_value());
  }
}

void brightness_parameter_typing_emits_one_typed_commit() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(70U, compositor::NodeKind::Brightness, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  const editor::CompositorFrame result =
      commit_compositor_field(harness, graph, 820.0F, 69.0F, "2.5");
  UI_CHECK(result.property_edit.has_value());
  UI_CHECK(result.property_edit->node == doc::NodeId{70U});
  UI_CHECK(result.property_edit->property ==
           editor::CompositorProperty::BrightnessAmount);
  UI_CHECK(std::get<float>(result.property_edit->value) == 2.5F);
  UI_CHECK(!result.editing);
  UI_CHECK(!result.add_node.has_value());
  UI_CHECK(result.move_nodes.empty());
  UI_CHECK(!result.connect_ports.has_value());
}

void compositor_parameter_nudges_report_full_semantic_identity() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(70U, compositor::NodeKind::Brightness, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  // Open the Amount field, then use a coarse keyboard repeat. It remains a
  // live numeric edit but carries enough identity for the Director's local
  // coalescing policy to distinguish it from every other stream.
  static_cast<void>(harness.frame(graph, 820.0F, 69.0F, false));
  static_cast<void>(harness.frame(graph, 820.0F, 69.0F, true));
  static_cast<void>(harness.frame(graph, 820.0F, 69.0F, false));
  const editor::CompositorFrame result = harness.frame(
      graph, 820.0F, 69.0F, false, {}, gui::Key::Up, gui::KeyMods::Shift);
  UI_CHECK(result.property_edit.has_value());
  UI_CHECK(result.property_edit->property ==
           editor::CompositorProperty::BrightnessAmount);
  UI_CHECK(std::abs(std::get<float>(result.property_edit->value) - 1.35F) <
           1.0e-5F);
  UI_CHECK(result.numeric_nudge.has_value());
  UI_CHECK(result.numeric_nudge->focus != gui::UiId{});
  UI_CHECK(
      result.numeric_nudge->property ==
      static_cast<core::u64>(editor::CompositorProperty::BrightnessAmount) +
          1U);
  UI_CHECK(result.numeric_nudge->direction ==
           editor::NumberFieldNudgeDirection::Increase);
  UI_CHECK(result.numeric_nudge->modifiers == gui::KeyMods::Shift);
}

void saturation_parameter_commits_are_clamped_to_four() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(73U, compositor::NodeKind::Saturation, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  const editor::CompositorFrame result =
      commit_compositor_field(harness, graph, 820.0F, 69.0F, "9.5");
  UI_CHECK(result.property_edit.has_value());
  UI_CHECK(result.property_edit->node == doc::NodeId{73U});
  UI_CHECK(result.property_edit->property ==
           editor::CompositorProperty::SaturationAmount);
  UI_CHECK(std::get<float>(result.property_edit->value) ==
           compositor::max_saturation);
  UI_CHECK(!result.editing);
}

void levels_parameter_typing_emits_the_exact_channel_property() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(74U, compositor::NodeKind::Levels, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  const editor::CompositorFrame result =
      commit_compositor_field(harness, graph, 820.0F, 99.0F, "0.25");
  UI_CHECK(result.property_edit.has_value());
  UI_CHECK(result.property_edit->node == doc::NodeId{74U});
  UI_CHECK(result.property_edit->property ==
           editor::CompositorProperty::LevelsMasterInputBlack);
  UI_CHECK(std::get<float>(result.property_edit->value) == 0.25F);
  UI_CHECK(!result.editing);
}

void lift_gamma_gain_fields_emit_typed_clamped_properties() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(75U, compositor::NodeKind::LiftGammaGain, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  struct FieldCase final {
    float y;
    editor::CompositorProperty property;
    std::string_view authored;
    float expected;
  };
  constexpr std::array<FieldCase, 9U> fields{{
      {69.0F, editor::CompositorProperty::LiftGammaGainLiftR, "-9", -4.0F},
      {95.0F, editor::CompositorProperty::LiftGammaGainLiftG, "9", 4.0F},
      {121.0F, editor::CompositorProperty::LiftGammaGainLiftB, "-2", -2.0F},
      {147.0F, editor::CompositorProperty::LiftGammaGainGammaR, "0", 0.01F},
      {173.0F, editor::CompositorProperty::LiftGammaGainGammaG, "99", 10.0F},
      {199.0F, editor::CompositorProperty::LiftGammaGainGammaB, "2", 2.0F},
      {225.0F, editor::CompositorProperty::LiftGammaGainGainR, "-1", 0.0F},
      {251.0F, editor::CompositorProperty::LiftGammaGainGainG, "99", 16.0F},
      {277.0F, editor::CompositorProperty::LiftGammaGainGainB, "2", 2.0F},
  }};

  for (const FieldCase &field : fields) {
    const editor::CompositorFrame result = commit_compositor_field(
        harness, graph, 820.0F, field.y, field.authored);
    UI_CHECK(result.property_edit.has_value());
    UI_CHECK(result.property_edit->node == doc::NodeId{75U});
    UI_CHECK(result.property_edit->property == field.property);
    UI_CHECK(std::get<float>(result.property_edit->value) == field.expected);
    UI_CHECK(!result.editing);
  }
}

void contrast_pivot_fields_emit_typed_clamped_properties() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(76U, compositor::NodeKind::ContrastPivot, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  const editor::CompositorFrame contrast =
      commit_compositor_field(harness, graph, 820.0F, 69.0F, "9");
  UI_CHECK(contrast.property_edit.has_value());
  UI_CHECK(contrast.property_edit->node == doc::NodeId{76U});
  UI_CHECK(contrast.property_edit->property ==
           editor::CompositorProperty::ContrastPivotContrast);
  UI_CHECK(std::get<float>(contrast.property_edit->value) == 4.0F);

  const editor::CompositorFrame pivot =
      commit_compositor_field(harness, graph, 820.0F, 95.0F, "-2");
  UI_CHECK(pivot.property_edit.has_value());
  UI_CHECK(pivot.property_edit->node == doc::NodeId{76U});
  UI_CHECK(pivot.property_edit->property ==
           editor::CompositorProperty::ContrastPivotPivot);
  UI_CHECK(std::get<float>(pivot.property_edit->value) == 0.0F);
}

void color_matrix_fields_emit_exact_typed_properties() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(77U, compositor::NodeKind::ColorMatrix, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  struct FieldCase final {
    float y;
    editor::CompositorProperty property;
    std::string_view authored;
    float expected;
  };
  constexpr std::array<FieldCase, 12U> fields{{
      {69.0F, editor::CompositorProperty::ColorMatrixM00, "20", 16.0F},
      {95.0F, editor::CompositorProperty::ColorMatrixM01, "-20", -16.0F},
      {121.0F, editor::CompositorProperty::ColorMatrixM02, "0.2", 0.2F},
      {147.0F, editor::CompositorProperty::ColorMatrixM10, "0.3", 0.3F},
      {173.0F, editor::CompositorProperty::ColorMatrixM11, "1.1", 1.1F},
      {199.0F, editor::CompositorProperty::ColorMatrixM12, "0.4", 0.4F},
      {225.0F, editor::CompositorProperty::ColorMatrixM20, "0.5", 0.5F},
      {251.0F, editor::CompositorProperty::ColorMatrixM21, "0.6", 0.6F},
      {277.0F, editor::CompositorProperty::ColorMatrixM22, "1.2", 1.2F},
      {303.0F, editor::CompositorProperty::ColorMatrixOffsetR, "-0.1", -0.1F},
      {329.0F, editor::CompositorProperty::ColorMatrixOffsetG, "0.25", 0.25F},
      {355.0F, editor::CompositorProperty::ColorMatrixOffsetB, "20", 16.0F},
  }};

  for (const FieldCase &field : fields) {
    const editor::CompositorFrame result = commit_compositor_field(
        harness, graph, 820.0F, field.y, field.authored);
    UI_CHECK(result.property_edit.has_value());
    UI_CHECK(result.property_edit->node == doc::NodeId{77U});
    UI_CHECK(result.property_edit->property == field.property);
    UI_CHECK(std::get<float>(result.property_edit->value) == field.expected);
    UI_CHECK(!result.editing);
  }
}

void white_balance_fields_emit_typed_clamped_properties() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(78U, compositor::NodeKind::WhiteBalance, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  const editor::CompositorFrame temperature =
      commit_compositor_field(harness, graph, 820.0F, 69.0F, "18000");
  UI_CHECK(temperature.property_edit.has_value());
  UI_CHECK(temperature.property_edit->node == doc::NodeId{78U});
  UI_CHECK(temperature.property_edit->property ==
           editor::CompositorProperty::WhiteBalanceTemperatureKelvin);
  UI_CHECK(std::get<float>(temperature.property_edit->value) ==
           scene::max_white_balance_temperature_kelvin);

  const editor::CompositorFrame tint =
      commit_compositor_field(harness, graph, 820.0F, 95.0F, "-9");
  UI_CHECK(tint.property_edit.has_value());
  UI_CHECK(tint.property_edit->node == doc::NodeId{78U});
  UI_CHECK(tint.property_edit->property ==
           editor::CompositorProperty::WhiteBalanceTint);
  UI_CHECK(std::get<float>(tint.property_edit->value) ==
           scene::min_white_balance_tint);
}

void curves_add_and_last_point_edit_emit_one_typed_program() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(79U, compositor::NodeKind::Curves, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  static_cast<void>(harness.frame(graph, 840.0F, 69.0F, false));
  static_cast<void>(harness.frame(graph, 840.0F, 69.0F, true));
  const editor::CompositorFrame added =
      harness.frame(graph, 840.0F, 69.0F, false);
  UI_CHECK(added.property_edit.has_value());
  UI_CHECK(added.property_edit->property ==
           editor::CompositorProperty::CurvesParameters);
  const auto &added_parameters =
      std::get<compositor::CurvesParameters>(added.property_edit->value);
  UI_CHECK(added_parameters.master.points.size() == 3U);
  UI_CHECK(scene::curves_is_identity(added_parameters));

  graph.nodes[0U].parameters = added_parameters;
  const editor::CompositorFrame changed =
      commit_compositor_field(harness, graph, 820.0F, 147.0F, "0.72");
  UI_CHECK(changed.property_edit.has_value());
  UI_CHECK(changed.property_edit->property ==
           editor::CompositorProperty::CurvesParameters);
  const auto &changed_parameters =
      std::get<compositor::CurvesParameters>(changed.property_edit->value);
  UI_CHECK(changed_parameters.master.points.size() == 3U);
  UI_CHECK(std::abs(changed_parameters.master.points[1U].y - 0.72F) < 1.0e-6F);
  UI_CHECK(!scene::curves_is_identity(changed_parameters));
}

void film_grain_fields_preserve_exact_integer_and_mode_types() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(80U, compositor::NodeKind::FilmGrain, 60.0F, 40.0F));

  {
    CompositorHarness harness;
    select_compositor_node(harness, graph);
    const editor::CompositorFrame amount =
        commit_compositor_field(harness, graph, 820.0F, 69.0F, "2");
    UI_CHECK(amount.property_edit.has_value());
    UI_CHECK(amount.property_edit->property ==
             editor::CompositorProperty::FilmGrainAmount);
    UI_CHECK(std::get<float>(amount.property_edit->value) == 1.0F);
  }

  {
    CompositorHarness harness;
    select_compositor_node(harness, graph);
    const editor::CompositorFrame seed =
        commit_compositor_field(harness, graph, 820.0F, 147.0F, "4294967295");
    UI_CHECK(seed.property_edit.has_value());
    UI_CHECK(seed.property_edit->property ==
             editor::CompositorProperty::FilmGrainSeed);
    UI_CHECK(std::get<core::u32>(seed.property_edit->value) == 4'294'967'295U);
  }

  {
    CompositorHarness harness;
    select_compositor_node(harness, graph);
    static_cast<void>(harness.frame(graph, 820.0F, 173.0F, false));
    static_cast<void>(harness.frame(graph, 820.0F, 173.0F, true));
    const editor::CompositorFrame mode =
        harness.frame(graph, 820.0F, 173.0F, false);
    UI_CHECK(mode.property_edit.has_value());
    UI_CHECK(mode.property_edit->property ==
             editor::CompositorProperty::FilmGrainMode);
    UI_CHECK(std::get<core::u32>(mode.property_edit->value) == 1U);
  }
}

void blur_parameter_commits_are_clamped_to_the_model_contract() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(71U, compositor::NodeKind::Blur, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  const editor::CompositorFrame result =
      commit_compositor_field(harness, graph, 820.0F, 69.0F, "999");
  UI_CHECK(result.property_edit.has_value());
  UI_CHECK(result.property_edit->node == doc::NodeId{71U});
  UI_CHECK(result.property_edit->property ==
           editor::CompositorProperty::BlurRadius);
  UI_CHECK(std::get<float>(result.property_edit->value) ==
           compositor::max_blur_radius);
}

void radial_array_parameters_emit_count_radius_angle_and_center_types() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(72U, compositor::NodeKind::RadialArray, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);

  editor::CompositorFrame result =
      commit_compositor_field(harness, graph, 820.0F, 69.0F, "17");
  UI_CHECK(result.property_edit.has_value());
  UI_CHECK(result.property_edit->property ==
           editor::CompositorProperty::RadialCount);
  UI_CHECK(std::get<core::u32>(result.property_edit->value) == 17U);

  result = commit_compositor_field(harness, graph, 820.0F, 95.0F, "8.5");
  UI_CHECK(result.property_edit.has_value());
  UI_CHECK(result.property_edit->property ==
           editor::CompositorProperty::RadialRadius);
  UI_CHECK(std::get<float>(result.property_edit->value) == 8.5F);

  result = commit_compositor_field(harness, graph, 820.0F, 121.0F, "0.125");
  UI_CHECK(result.property_edit.has_value());
  UI_CHECK(result.property_edit->property ==
           editor::CompositorProperty::RadialStartAngleTurns);
  UI_CHECK(std::get<float>(result.property_edit->value) == 0.125F);

  result = commit_compositor_field(harness, graph, 740.0F, 147.0F, "-2.25");
  UI_CHECK(result.property_edit.has_value());
  UI_CHECK(result.property_edit->property ==
           editor::CompositorProperty::RadialCenterX);
  UI_CHECK(std::get<float>(result.property_edit->value) == -2.25F);

  result = commit_compositor_field(harness, graph, 855.0F, 147.0F, "3.5");
  UI_CHECK(result.property_edit.has_value());
  UI_CHECK(result.property_edit->property ==
           editor::CompositorProperty::RadialCenterY);
  UI_CHECK(std::get<float>(result.property_edit->value) == 3.5F);
}

// The authored move reaches the evaluated graph some frames after release.
// Until it does the node stays where it was let go, rather than snapping back
// to its old position for those frames and then jumping forward.
void a_released_node_holds_its_position_until_the_graph_reports_it() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 60.0F, 40.0F));
  // Old screen bounds (60, 66)-(236, 152); after the drag, (120, 126)-(296,
  // 212). Each point below is inside one and not the other, and in a header
  // rather than on a port or the collapse toggle.
  constexpr float old_only_x = 70.0F;
  constexpr float old_only_y = 80.0F;
  constexpr float new_only_x = 250.0F;
  constexpr float new_only_y = 140.0F;

  CompositorHarness harness;
  static_cast<void>(harness.frame(graph, 100.0F, 75.0F, false));
  static_cast<void>(harness.frame(graph, 100.0F, 75.0F, true));
  static_cast<void>(harness.frame(graph, 160.0F, 135.0F, true));
  const editor::CompositorFrame released =
      harness.frame(graph, 160.0F, 135.0F, false);
  UI_CHECK(released.move_nodes ==
           (std::vector<editor::CompositorMoveIntent>{
               {doc::NodeId{1U}, 120.0F, 100.0F}}));

  // The graph has not caught up: the node is drawn and picked where it was
  // dropped, and pressing there selects it.
  static_cast<void>(harness.frame(graph, 0.0F, 0.0F, false));
  UI_CHECK(harness.panel.node_at(graph, new_only_x, new_only_y).has_value());
  UI_CHECK(!harness.panel.node_at(graph, old_only_x, old_only_y).has_value());
  const editor::CompositorFrame pressed =
      harness.frame(graph, new_only_x, new_only_y, true);
  UI_CHECK(pressed.selection ==
           (editor::CompositorSelection{editor::CompositorSelectionKind::Node,
                                        doc::NodeId{1U}}));
  static_cast<void>(harness.frame(graph, new_only_x, new_only_y, false));

  // The edit arrives, normalized to a slightly different value: the graph's
  // own position takes over from then on.
  graph.nodes.front().editor_x = 121.0F;
  graph.nodes.front().editor_y = 100.0F;
  static_cast<void>(harness.frame(graph, 0.0F, 0.0F, false));
  UI_CHECK(harness.panel.node_at(graph, 296.5F, new_only_y).has_value());

  // An edit that never arrives - refused - lets go after a bounded wait.
  compositor::CompositorGraph refused;
  refused.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 60.0F, 40.0F));
  CompositorHarness refused_harness;
  static_cast<void>(refused_harness.frame(refused, 100.0F, 75.0F, false));
  static_cast<void>(refused_harness.frame(refused, 100.0F, 75.0F, true));
  static_cast<void>(refused_harness.frame(refused, 160.0F, 135.0F, true));
  static_cast<void>(refused_harness.frame(refused, 160.0F, 135.0F, false));
  for (int frame = 0; frame < 60; ++frame) {
    static_cast<void>(refused_harness.frame(refused, 0.0F, 0.0F, false));
  }
  UI_CHECK(refused_harness.panel.node_at(refused, new_only_x, new_only_y)
               .has_value());
  for (int frame = 0; frame < 90; ++frame) {
    static_cast<void>(refused_harness.frame(refused, 0.0F, 0.0F, false));
  }
  UI_CHECK(refused_harness.panel.node_at(refused, old_only_x, old_only_y)
               .has_value());
  UI_CHECK(!refused_harness.panel.node_at(refused, new_only_x, new_only_y)
                .has_value());
}

// A wire is a curve now, so it is picked where it is drawn: through the
// middle of its two ports, and not at the elbow a square wire used to turn.
void wires_are_picked_along_their_curve() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(10U, compositor::NodeKind::SceneLayer, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(11U, compositor::NodeKind::Output, 320.0F, 200.0F));
  graph.edges.push_back(compositor::GraphEdge{
      doc::NodeId{50U}, compositor::EdgeEndpoint{doc::NodeId{10U}, 0U},
      compositor::EdgeEndpoint{doc::NodeId{11U}, 0U}, 0});
  // Layer IMAGE OUT at (216, 132); Output's IMAGE input at (320, 272). A
  // curve leaving and entering horizontally is symmetric about the midpoint.
  constexpr float middle_x = (216.0F + 320.0F) * 0.5F;
  constexpr float middle_y = (132.0F + 272.0F) * 0.5F;

  CompositorHarness elbow;
  static_cast<void>(elbow.frame(graph, middle_x, 132.0F, false));
  const editor::CompositorFrame missed = elbow.frame(graph, middle_x, 132.0F, true);
  UI_CHECK(missed.selection == editor::CompositorSelection{});

  CompositorHarness middle;
  static_cast<void>(middle.frame(graph, middle_x, middle_y, false));
  const editor::CompositorFrame picked =
      middle.frame(graph, middle_x, middle_y + 3.0F, true);
  UI_CHECK(picked.selection ==
           (editor::CompositorSelection{editor::CompositorSelectionKind::Edge,
                                        doc::NodeId{50U}}));
  // The pressed wire keeps the pointer until release, wherever it wanders.
  static_cast<void>(middle.frame(graph, 600.0F, 500.0F, true));
  static_cast<void>(middle.frame(graph, 600.0F, 500.0F, false));
  const editor::CompositorFrame next = middle.frame(graph, 100.0F, 100.0F, true);
  UI_CHECK(next.selection ==
           (editor::CompositorSelection{editor::CompositorSelectionKind::Node,
                                        doc::NodeId{10U}}));
}

// Letting a wire go near a compatible port connects it: the operator does
// not have to land on the dot.
void a_wire_snaps_to_a_compatible_port_near_the_pointer() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::SceneLayer, 320.0F, 60.0F));
  // Brightness EFFECT OUT at (216, 132); the Layer's EFFECTS input dot at
  // (320, 132), its row target starting 9 to the left of the dot.
  const auto release_at = [&](float x, float y) {
    CompositorHarness harness;
    static_cast<void>(harness.frame(graph, 216.0F, 132.0F, false));
    static_cast<void>(harness.frame(graph, 216.0F, 132.0F, true));
    static_cast<void>(harness.frame(graph, x, y, true));
    return harness.frame(graph, x, y, false);
  };
  // 20 short of the dot and outside its row: within snapping reach.
  const editor::CompositorFrame near = release_at(300.0F, 132.0F);
  UI_CHECK(near.connect_ports ==
           (editor::CompositorConnectIntent{
               compositor::EdgeEndpoint{doc::NodeId{1U}, 0U},
               compositor::EdgeEndpoint{doc::NodeId{2U}, 0U}, std::nullopt}));
  // Well inside the row, far from the dot: the row is the target.
  const editor::CompositorFrame over_row = release_at(390.0F, 140.0F);
  UI_CHECK(over_row.connect_ports.has_value());
  UI_CHECK(over_row.connect_ports->to ==
           (compositor::EdgeEndpoint{doc::NodeId{2U}, 0U}));
  // 30 short: out of reach, so nothing connects.
  UI_CHECK(!release_at(290.0F, 132.0F).connect_ports.has_value());
  // The FILTERS row below takes filters only; an effect never snaps
  // to it, even with the pointer on it.
  UI_CHECK(!release_at(330.0F, 158.0F).connect_ports.has_value());
}

// A drop from another panel can name the input it lands on, from the whole
// row rather than the dot alone.
void an_input_ports_target_covers_its_whole_row() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::SceneLayer, 320.0F, 60.0F));
  CompositorHarness harness;
  static_cast<void>(harness.frame(graph, 0.0F, 0.0F, false));
  // Node (320, 86)-(496, 174): header to 121, EFFECTS row 121-143, FILTERS
  // (port 2) 143-165, INSTANCES (port 1) 165-187.
  UI_CHECK(harness.panel.input_port_at(graph, 314.0F, 132.0F) ==
           (compositor::EdgeEndpoint{doc::NodeId{2U}, 0U}));
  UI_CHECK(harness.panel.input_port_at(graph, 400.0F, 125.0F) ==
           (compositor::EdgeEndpoint{doc::NodeId{2U}, 0U}));
  UI_CHECK(harness.panel.input_port_at(graph, 330.0F, 150.0F) ==
           (compositor::EdgeEndpoint{doc::NodeId{2U}, 2U}));
  UI_CHECK(harness.panel.input_port_at(graph, 330.0F, 172.0F) ==
           (compositor::EdgeEndpoint{doc::NodeId{2U}, 1U}));
  UI_CHECK(!harness.panel.input_port_at(graph, 330.0F, 100.0F).has_value());
  UI_CHECK(!harness.panel.input_port_at(graph, 470.0F, 132.0F).has_value());
  UI_CHECK(!harness.panel.input_port_at(graph, 200.0F, 132.0F).has_value());
}

void the_parameters_panel_resizes_from_its_edge() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(70U, compositor::NodeKind::Brightness, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);
  UI_CHECK(harness.panel.parameter_panel_width() == 0.0F);

  // The panel's left edge is at 900 - 280. Press just on the graph side of
  // it - the seam reaches 3 either way - and drag it 100 to the left.
  static_cast<void>(harness.frame(graph, 618.0F, 400.0F, false));
  const editor::CompositorFrame pressed =
      harness.frame(graph, 618.0F, 400.0F, true);
  static_cast<void>(harness.frame(graph, 518.0F, 400.0F, true));
  const editor::CompositorFrame released =
      harness.frame(graph, 518.0F, 400.0F, false);
  UI_CHECK(harness.panel.parameter_panel_width() == 382.0F);
  UI_CHECK(released.move_nodes.empty());
  // Resizing is not a click on empty graph: the node stays selected.
  UI_CHECK(!pressed.selection.has_value());
  UI_CHECK(harness.panel.selection() ==
           (editor::CompositorSelection{editor::CompositorSelectionKind::Node,
                                        doc::NodeId{70U}}));

  // The field moved with the edge: x=600 was graph canvas and is now the
  // AMOUNT field.
  const editor::CompositorFrame typed =
      commit_compositor_field(harness, graph, 600.0F, 73.0F, "2.5");
  UI_CHECK(typed.property_edit.has_value());
  UI_CHECK(typed.property_edit->property ==
           editor::CompositorProperty::BrightnessAmount);

  // Dragged far left, it stops at its widest, still leaving the graph a
  // usable 260.
  static_cast<void>(harness.frame(graph, 518.0F, 400.0F, false));
  static_cast<void>(harness.frame(graph, 518.0F, 400.0F, true));
  static_cast<void>(harness.frame(graph, 10.0F, 400.0F, true));
  static_cast<void>(harness.frame(graph, 10.0F, 400.0F, false));
  UI_CHECK(harness.panel.parameter_panel_width() == 640.0F);
}

// Levels has four channels of five fields - taller than the panel. The list
// scrolls, so the last field is still reachable.
void long_parameter_lists_scroll_to_their_last_field() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(74U, compositor::NodeKind::Levels, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);
  // Rows start at y=62, 26 apart: BLUE OUT WHITE is row 23, at 660, below
  // the 640-high panel.
  static_cast<void>(harness.navigation_frame(graph, 820.0F, 300.0F, false, -5.0F));
  static_cast<void>(harness.frame(graph, 820.0F, 300.0F, false));
  // Scrolled to the end: the list is 632 tall in a 582 body, so 50 up.
  const editor::CompositorFrame last =
      commit_compositor_field(harness, graph, 820.0F, 621.0F, "0.75");
  UI_CHECK(last.property_edit.has_value());
  UI_CHECK(last.property_edit->property ==
           editor::CompositorProperty::LevelsBlueOutputWhite);
  UI_CHECK(std::get<float>(last.property_edit->value) == 0.75F);
}

void parameter_sections_collapse() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(74U, compositor::NodeKind::Levels, 60.0F, 40.0F));
  CompositorHarness harness;
  select_compositor_node(harness, graph);
  // Collapse MASTER: RED's heading moves up into the next row, and its
  // first field below that.
  static_cast<void>(harness.frame(graph, 820.0F, 73.0F, false));
  static_cast<void>(harness.frame(graph, 820.0F, 73.0F, true));
  static_cast<void>(harness.frame(graph, 820.0F, 73.0F, false));
  const editor::CompositorFrame red =
      commit_compositor_field(harness, graph, 820.0F, 125.0F, "0.125");
  UI_CHECK(red.property_edit.has_value());
  UI_CHECK(red.property_edit->property ==
           editor::CompositorProperty::LevelsRedInputBlack);
}

// The fault this layout replaces: titles and port names drawn in boxes
// shorter than the font, and nodes narrower than their names. With a real
// font, every glyph a node draws lies inside the clip it is drawn under, and
// every character of its title, source and port names is drawn - none was
// shortened to an ellipsis.
void node_text_fits_unclipped_in_a_real_font() {
  constexpr std::array<std::string_view, 2U> candidates{
      "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf"};
  constexpr std::array<float, 4U> sizes{0.5F, 0.75F, 0.875F, 1.25F};
  std::optional<gui::FontAtlas> font;
  for (const std::string_view path : candidates) {
    font = gui::FontAtlas::create(path, 14U, 1.0F, sizes);
    if (font.has_value()) {
      break;
    }
  }
  if (!font.has_value()) {
    std::cout << "[skip] no system font; node text fit needs real glyphs\n";
    return;
  }
  gui::InputState input;
  gui::UiDrawList list;
  list.set_white_texel(font->white_u(), font->white_v());
  gui::UiContext ctx{input, list, *font};
  editor::CompositorPanel panel{gui::ui_id("compositor.font.test")};

  editor::ElementInfo plate{};
  plate.node = doc::NodeId{1000U};
  plate.kind = editor::ElementKind::Media;
  plate.display_name = "Plate Front Driver Side 3/4 Long Lens";
  const std::array<editor::ElementInfo, 1U> elements{plate};
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::SceneLayer, 20.0F, 20.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::LedCarProcess, 420.0F, 20.0F));

  input.new_frame();
  input.set_mouse_position(890.0F, 630.0F);
  ctx.new_frame();
  static_cast<void>(panel.draw(ctx, CompositorHarness::bounds, graph, elements));

  const auto non_space = [](std::string_view text) {
    return static_cast<std::size_t>(
        std::count_if(text.begin(), text.end(),
                      [](char character) { return character != ' '; }));
  };
  std::size_t expected{};
  for (const compositor::GraphNode &node : graph.nodes) {
    expected += non_space(compositor::node_kind_name(node.kind));
    for (const auto &port : compositor::input_ports(node.kind)) {
      expected += non_space(port.name);
    }
    for (const auto &port : compositor::output_ports(node.kind)) {
      expected += non_space(port.name);
    }
  }
  expected += non_space(plate.display_name);
  expected += non_space("8 WALL + CEILING  ARRAY 4.00");

  // Glyphs sample the atlas away from its white texel; everything below the
  // toolbar belongs to the two nodes.
  std::size_t glyph_vertices{};
  for (const gui::UiBatch &batch : list.batches()) {
    for (core::u32 index = batch.first_vertex;
         index < batch.first_vertex + batch.vertex_count; ++index) {
      const gui::UiVertex &vertex = list.vertices()[index];
      if ((vertex.u == font->white_u() && vertex.v == font->white_v()) ||
          vertex.y < 26.0F) {
        continue;
      }
      ++glyph_vertices;
      UI_CHECK(vertex.x >= batch.clip_min_x - 0.01F &&
               vertex.x <= batch.clip_max_x + 0.01F);
      UI_CHECK(vertex.y >= batch.clip_min_y - 0.01F &&
               vertex.y <= batch.clip_max_y + 0.01F);
    }
  }
  UI_CHECK(glyph_vertices == expected * 6U);

  // Both nodes grew past the minimum to fit their names.
  UI_CHECK(panel.new_node_size(compositor::NodeKind::LedCarProcess).x > 176.0F);
  const auto layer = panel.node_at(graph, 30.0F, 50.0F);
  UI_CHECK(layer.has_value() && layer->second.width > 176.0F);
}

// Headless geometry, as in the tests above: nodes are 176 wide with a 35
// header and 22 rows, so input i's dot is at the node's left edge and
// 46 + 22i below its top; the canvas starts at y=26.

[[nodiscard]] compositor::GraphEdge graph_edge(core::u64 id, core::u64 from,
                                               core::u8 from_port, core::u64 to,
                                               core::u8 to_port) {
  return compositor::GraphEdge{
      doc::NodeId{id}, compositor::EdgeEndpoint{doc::NodeId{from}, from_port},
      compositor::EdgeEndpoint{doc::NodeId{to}, to_port}, 0};
}

// Press at one point, drag to another, let go; the result of the release.
[[nodiscard]] editor::CompositorFrame
drag_in_graph(CompositorHarness &harness,
              const compositor::CompositorGraph &graph, float from_x,
              float from_y, float to_x, float to_y,
              gui::KeyMods mods = gui::KeyMods::None) {
  static_cast<void>(
      harness.frame(graph, from_x, from_y, false, {}, std::nullopt, mods));
  static_cast<void>(
      harness.frame(graph, from_x, from_y, true, {}, std::nullopt, mods));
  static_cast<void>(harness.frame(graph, to_x, to_y, true, {}, std::nullopt, mods));
  return harness.frame(graph, to_x, to_y, false, {}, std::nullopt, mods);
}

// Letting a wire go on empty canvas opens the node search there, offering
// only nodes that can take the wire. The one chosen arrives connected, its
// input where the wire was let go.
void a_wire_let_go_on_empty_canvas_offers_nodes_that_take_it() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Blur, 40.0F, 60.0F));
  CompositorHarness harness;
  // Blur's FILTER output is at (216, 132).
  const editor::CompositorFrame released =
      drag_in_graph(harness, graph, 216.0F, 132.0F, 500.0F, 300.0F);
  UI_CHECK(!released.connect_ports.has_value());
  UI_CHECK(!released.add_node.has_value());
  static_cast<void>(harness.frame(graph, 500.0F, 300.0F, false));
  // "layer" matches Scene Layer and Composite; only Scene Layer has an input
  // a filter can feed: FILTERS, its second row.
  static_cast<void>(harness.frame(graph, 500.0F, 300.0F, false, "layer"));
  const editor::CompositorFrame chosen = harness.frame(
      graph, 500.0F, 300.0F, false, {}, gui::Key::Enter);
  UI_CHECK(chosen.add_node.has_value());
  UI_CHECK(chosen.add_node->kind == compositor::NodeKind::SceneLayer);
  UI_CHECK(chosen.add_node->editor_x == 500.0F);
  UI_CHECK(chosen.add_node->editor_y == 300.0F - 26.0F - 46.0F - 22.0F);
  UI_CHECK(chosen.add_node->links ==
           (std::vector<editor::CompositorAddLink>{
               {false, 2U, compositor::EdgeEndpoint{doc::NodeId{1U}, 0U}}}));
  // Taken, the search closes.
  UI_CHECK(!harness.frame(graph, 500.0F, 300.0F, false, {}, gui::Key::Enter)
                .add_node.has_value());

  // Escape closes it without adding anything.
  CompositorHarness escaped;
  static_cast<void>(
      drag_in_graph(escaped, graph, 216.0F, 132.0F, 500.0F, 300.0F));
  static_cast<void>(escaped.frame(graph, 500.0F, 300.0F, false));
  static_cast<void>(
      escaped.frame(graph, 500.0F, 300.0F, false, {}, gui::Key::Escape));
  UI_CHECK(!escaped.frame(graph, 500.0F, 300.0F, false, {}, gui::Key::Enter)
                .add_node.has_value());
}

// Tab over the canvas opens the same search with nothing attached; the node
// chosen is placed at the pointer.
void tab_opens_the_node_search_at_the_pointer() {
  const compositor::CompositorGraph graph;
  CompositorHarness harness;
  // A click on the graph gives it the keyboard.
  static_cast<void>(harness.frame(graph, 400.0F, 300.0F, true));
  static_cast<void>(harness.frame(graph, 400.0F, 300.0F, false));
  static_cast<void>(
      harness.frame(graph, 400.0F, 300.0F, false, {}, gui::Key::Tab));
  static_cast<void>(harness.frame(graph, 400.0F, 300.0F, false));
  static_cast<void>(harness.frame(graph, 400.0F, 300.0F, false, "gaussian"));
  const editor::CompositorFrame chosen = harness.frame(
      graph, 400.0F, 300.0F, false, {}, gui::Key::Enter);
  UI_CHECK(chosen.add_node.has_value());
  UI_CHECK(chosen.add_node->kind == compositor::NodeKind::Blur);
  UI_CHECK(chosen.add_node->editor_x == 400.0F);
  UI_CHECK(chosen.add_node->editor_y == 274.0F);
  UI_CHECK(chosen.add_node->links.empty());
}

// Alt-click breaks wires: on an output, every wire leaving it; on an input,
// the wire arriving; on a wire, that wire.
void alt_click_breaks_wires() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(10U, compositor::NodeKind::SceneLayer, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(11U, compositor::NodeKind::Output, 320.0F, 60.0F));
  graph.edges.push_back(graph_edge(50U, 10U, 0U, 11U, 0U));
  const editor::CompositorRewireIntent broken{{doc::NodeId{50U}}, {}};
  const auto alt_click = [&](float x, float y) {
    CompositorHarness harness;
    static_cast<void>(harness.frame(graph, x, y, false, {}, std::nullopt,
                                    gui::KeyMods::Alt));
    return harness.frame(graph, x, y, true, {}, std::nullopt,
                         gui::KeyMods::Alt);
  };
  UI_CHECK(alt_click(216.0F, 132.0F).rewire == broken);
  const editor::CompositorFrame input = alt_click(330.0F, 132.0F);
  UI_CHECK(input.rewire == broken);
  UI_CHECK(input.move_nodes.empty());
  UI_CHECK(alt_click(268.0F, 132.0F).rewire == broken);
  {
    // The broken wire is gone from the next graph, and with it the control
    // the press captured. The panel must still take the next press.
    CompositorHarness harness;
    static_cast<void>(harness.frame(graph, 268.0F, 132.0F, false, {},
                                    std::nullopt, gui::KeyMods::Alt));
    UI_CHECK(harness.frame(graph, 268.0F, 132.0F, true, {}, std::nullopt,
                           gui::KeyMods::Alt)
                 .rewire == broken);
    compositor::CompositorGraph unwired = graph;
    unwired.edges.clear();
    static_cast<void>(harness.frame(unwired, 268.0F, 132.0F, false));
    static_cast<void>(harness.frame(unwired, 360.0F, 100.0F, false));
    UI_CHECK(harness.frame(unwired, 360.0F, 100.0F, true).selection ==
             (editor::CompositorSelection{editor::CompositorSelectionKind::Node,
                                          doc::NodeId{11U}}));
  }
  // Without Alt the same presses do what they always did.
  CompositorHarness plain;
  static_cast<void>(plain.frame(graph, 268.0F, 132.0F, false));
  UI_CHECK(!plain.frame(graph, 268.0F, 132.0F, true).rewire.has_value());
}

// Ctrl-drag lifts wires to move them: off an input, its wire's end moves to
// another input; off an output, its wires' start moves to another output.
// Let go on nothing, they are disconnected; put back, nothing changes.
void ctrl_drag_moves_wires_between_ports() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(10U, compositor::NodeKind::SceneLayer, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(12U, compositor::NodeKind::Composite, 320.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(11U, compositor::NodeKind::Output, 320.0F, 200.0F));
  graph.edges.push_back(graph_edge(50U, 10U, 0U, 12U, 0U));
  // Composite's LAYERS input at (320, 132); Output's IMAGE at (320, 272).
  {
    CompositorHarness harness;
    const editor::CompositorFrame moved = drag_in_graph(
        harness, graph, 330.0F, 132.0F, 330.0F, 272.0F, gui::KeyMods::Ctrl);
    UI_CHECK(moved.rewire ==
             (editor::CompositorRewireIntent{
                 {doc::NodeId{50U}},
                 {{compositor::EdgeEndpoint{doc::NodeId{10U}, 0U},
                   compositor::EdgeEndpoint{doc::NodeId{11U}, 0U}}}}));
    UI_CHECK(moved.move_nodes.empty());
  }
  {
    CompositorHarness harness;
    UI_CHECK(drag_in_graph(harness, graph, 330.0F, 132.0F, 700.0F, 500.0F,
                           gui::KeyMods::Ctrl)
                 .rewire ==
             (editor::CompositorRewireIntent{{doc::NodeId{50U}}, {}}));
  }
  {
    CompositorHarness harness;
    const editor::CompositorFrame put_back = drag_in_graph(
        harness, graph, 330.0F, 132.0F, 325.0F, 134.0F, gui::KeyMods::Ctrl);
    UI_CHECK(!put_back.rewire.has_value());
    UI_CHECK(!put_back.connect_ports.has_value());
  }

  compositor::CompositorGraph sources;
  sources.nodes.push_back(
      graph_node(10U, compositor::NodeKind::SceneLayer, 40.0F, 60.0F));
  sources.nodes.push_back(
      graph_node(13U, compositor::NodeKind::SceneLayer, 40.0F, 200.0F));
  sources.nodes.push_back(
      graph_node(11U, compositor::NodeKind::Output, 320.0F, 60.0F));
  sources.edges.push_back(graph_edge(50U, 10U, 0U, 11U, 0U));
  CompositorHarness harness;
  // From the first Layer's IMAGE output to the second's, at (216, 272).
  UI_CHECK(drag_in_graph(harness, sources, 216.0F, 132.0F, 216.0F, 272.0F,
                         gui::KeyMods::Ctrl)
               .rewire ==
           (editor::CompositorRewireIntent{
               {doc::NodeId{50U}},
               {{compositor::EdgeEndpoint{doc::NodeId{13U}, 0U},
                 compositor::EdgeEndpoint{doc::NodeId{11U}, 0U}}}}));
}

// A node with no wires of its own, dropped onto a wire it can carry, is
// spliced into it: the wire's source feeds the node, the node feeds the
// wire's old target, as one gesture with the move.
void a_free_node_dropped_on_a_wire_splices_into_it() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(10U, compositor::NodeKind::SceneLayer, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(11U, compositor::NodeKind::Output, 600.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(12U, compositor::NodeKind::Composite, 300.0F, 300.0F));
  graph.edges.push_back(graph_edge(50U, 10U, 0U, 11U, 0U));
  // The wire runs straight along y=132. Composite's header is grabbed 40 in
  // and 9 down, and let go 9 below the wire.
  CompositorHarness harness;
  const editor::CompositorFrame dropped =
      drag_in_graph(harness, graph, 340.0F, 335.0F, 400.0F, 141.0F);
  UI_CHECK(dropped.move_nodes ==
           (std::vector<editor::CompositorMoveIntent>{
               {doc::NodeId{12U}, 360.0F, 106.0F}}));
  UI_CHECK(dropped.rewire ==
           (editor::CompositorRewireIntent{
               {doc::NodeId{50U}},
               {{compositor::EdgeEndpoint{doc::NodeId{10U}, 0U},
                 compositor::EdgeEndpoint{doc::NodeId{12U}, 0U}},
                {compositor::EdgeEndpoint{doc::NodeId{12U}, 0U},
                 compositor::EdgeEndpoint{doc::NodeId{11U}, 0U}}}}));

  // Let go well away from the wire, it is only a move.
  CompositorHarness away;
  const editor::CompositorFrame moved =
      drag_in_graph(away, graph, 340.0F, 335.0F, 400.0F, 220.0F);
  UI_CHECK(moved.move_nodes.size() == 1U);
  UI_CHECK(!moved.rewire.has_value());

  // A node that already has a wire is not spliced in.
  compositor::CompositorGraph wired = graph;
  wired.nodes.push_back(
      graph_node(13U, compositor::NodeKind::SceneLayer, 40.0F, 400.0F));
  wired.edges.push_back(graph_edge(51U, 13U, 0U, 12U, 0U));
  CompositorHarness busy;
  UI_CHECK(!drag_in_graph(busy, wired, 340.0F, 335.0F, 400.0F, 141.0F)
                .rewire.has_value());
}

// Double-click a wire to add a reroute point; drag a point to move it;
// double-click a point to remove it. A routed wire is picked along its route.
void reroute_points_are_added_moved_and_removed() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(10U, compositor::NodeKind::SceneLayer, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(11U, compositor::NodeKind::Output, 600.0F, 60.0F));
  graph.edges.push_back(graph_edge(50U, 10U, 0U, 11U, 0U));
  {
    CompositorHarness harness;
    static_cast<void>(harness.frame(graph, 400.0F, 132.0F, false));
    const editor::CompositorFrame first =
        harness.frame(graph, 400.0F, 132.0F, true);
    UI_CHECK(!first.waypoints.has_value());
    static_cast<void>(harness.frame(graph, 400.0F, 132.0F, false));
    UI_CHECK(harness.frame(graph, 400.0F, 132.0F, true).waypoints ==
             (editor::CompositorWaypointIntent{doc::NodeId{50U},
                                               {{400.0F, 106.0F}}}));
  }

  compositor::CompositorGraph routed = graph;
  routed.edges.front().points = {{400.0F, 306.0F}};
  // Through (400, 332) on screen: each stretch is a curve symmetric about
  // its middle, so (308, 232) is on the first one and the straight path's
  // middle is not on either.
  {
    CompositorHarness harness;
    static_cast<void>(harness.frame(routed, 308.0F, 232.0F, false));
    UI_CHECK(harness.frame(routed, 308.0F, 232.0F, true).selection ==
             (editor::CompositorSelection{editor::CompositorSelectionKind::Edge,
                                          doc::NodeId{50U}}));
  }
  {
    CompositorHarness harness;
    static_cast<void>(harness.frame(routed, 408.0F, 132.0F, false));
    UI_CHECK(harness.frame(routed, 408.0F, 132.0F, true).selection ==
             editor::CompositorSelection{});
  }
  {
    CompositorHarness harness;
    UI_CHECK(drag_in_graph(harness, routed, 400.0F, 332.0F, 420.0F, 400.0F)
                 .waypoints ==
             (editor::CompositorWaypointIntent{doc::NodeId{50U},
                                               {{420.0F, 374.0F}}}));
  }
  {
    CompositorHarness harness;
    static_cast<void>(harness.frame(routed, 400.0F, 332.0F, false));
    static_cast<void>(harness.frame(routed, 400.0F, 332.0F, true));
    static_cast<void>(harness.frame(routed, 400.0F, 332.0F, false));
    UI_CHECK(harness.frame(routed, 400.0F, 332.0F, true).waypoints ==
             (editor::CompositorWaypointIntent{doc::NodeId{50U}, {}}));
  }
}

// A wire that would make a loop is refused, rather than authored and then
// reported by the graph.
void a_wire_that_would_make_a_loop_is_refused() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Composite, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::Composite, 320.0F, 60.0F));
  graph.edges.push_back(graph_edge(50U, 1U, 0U, 2U, 0U));
  CompositorHarness harness;
  const editor::CompositorFrame refused =
      drag_in_graph(harness, graph, 496.0F, 132.0F, 50.0F, 132.0F);
  UI_CHECK(!refused.connect_ports.has_value());
  UI_CHECK(!refused.rewire.has_value());
}

// What other panels ask before a drop: the compatible input under or near
// the pointer, the wire it would replace, and where a new node goes.
void drops_from_other_panels_find_compatible_inputs() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::SceneLayer, 320.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(11U, compositor::NodeKind::Output, 600.0F, 60.0F));
  graph.edges.push_back(graph_edge(50U, 2U, 0U, 11U, 0U));
  CompositorHarness harness;
  static_cast<void>(harness.frame(graph, 0.0F, 0.0F, false));

  const auto effects = harness.panel.port_drop_target(
      graph, 330.0F, 132.0F, compositor::PortValueType::Effect);
  UI_CHECK(effects.has_value());
  UI_CHECK(effects->endpoint == (compositor::EdgeEndpoint{doc::NodeId{2U}, 0U}));
  UI_CHECK(effects->x == 320.0F && effects->y == 132.0F);
  UI_CHECK(effects->multiple && !effects->occupied.has_value());
  // The row below takes array instances, and the effects dot is out of reach.
  UI_CHECK(!harness.panel
                .port_drop_target(graph, 330.0F, 158.0F,
                                  compositor::PortValueType::Effect)
                .has_value());
  const auto image = harness.panel.port_drop_target(
      graph, 610.0F, 132.0F, compositor::PortValueType::Image);
  UI_CHECK(image.has_value());
  UI_CHECK(image->endpoint == (compositor::EdgeEndpoint{doc::NodeId{11U}, 0U}));
  UI_CHECK(image->occupied == doc::NodeId{50U});
  UI_CHECK(!image->multiple);
  UI_CHECK(!harness.panel
                .port_drop_target(graph, 610.0F, 132.0F,
                                  compositor::PortValueType::Effect)
                .has_value());

  // A new Blur goes 70 left of the input with its output dot level with it.
  const gui::UiPoint at = harness.panel.node_position_feeding(
      compositor::NodeKind::Blur, *effects);
  UI_CHECK(at.x + 176.0F + 70.0F == 320.0F);
  UI_CHECK(at.y + 46.0F == 132.0F - 26.0F);
}

// A wire made by letting go on an input stays drawn until the graph reports
// it, and is not drawn twice once it has: the connection never blinks.
void a_new_wire_is_held_until_the_graph_reports_it() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::SceneLayer, 320.0F, 60.0F));
  CompositorHarness harness;
  UI_CHECK(drag_in_graph(harness, graph, 216.0F, 132.0F, 320.0F, 132.0F)
               .connect_ports.has_value());
  static_cast<void>(harness.frame(graph, 600.0F, 500.0F, false));
  const std::size_t held = harness.list.vertices().size();
  for (int frame = 0; frame < 125; ++frame) {
    static_cast<void>(harness.frame(graph, 600.0F, 500.0F, false));
  }
  const std::size_t refused = harness.list.vertices().size();
  UI_CHECK(held > refused);

  CompositorHarness arriving;
  UI_CHECK(drag_in_graph(arriving, graph, 216.0F, 132.0F, 320.0F, 132.0F)
               .connect_ports.has_value());
  compositor::CompositorGraph connected = graph;
  connected.edges.push_back(graph_edge(50U, 1U, 0U, 2U, 0U));
  static_cast<void>(arriving.frame(connected, 600.0F, 500.0F, false));
  const std::size_t arrived = arriving.list.vertices().size();
  for (int frame = 0; frame < 125; ++frame) {
    static_cast<void>(arriving.frame(connected, 600.0F, 500.0F, false));
  }
  UI_CHECK(arriving.list.vertices().size() == arrived);
}

// Clicks that build a selection: Ctrl toggles one node, Shift adds one.
void ctrl_and_shift_clicks_build_a_selection() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::Saturation, 300.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(3U, compositor::NodeKind::Blur, 40.0F, 300.0F));
  CompositorHarness harness;
  const auto click = [&](float x, float y, gui::KeyMods mods) {
    static_cast<void>(harness.frame(graph, x, y, true, {}, std::nullopt, mods));
    static_cast<void>(harness.frame(graph, x, y, false, {}, std::nullopt, mods));
  };
  const auto members = [&] {
    const auto span = harness.panel.selected_nodes();
    return std::vector<doc::NodeId>{span.begin(), span.end()};
  };
  // Bodies, below the titles: (80, 135), (340, 135), (80, 375).
  click(80.0F, 135.0F, gui::KeyMods::None);
  click(340.0F, 135.0F, gui::KeyMods::Ctrl);
  UI_CHECK(members() == (std::vector<doc::NodeId>{doc::NodeId{1U},
                                                  doc::NodeId{2U}}));
  UI_CHECK(harness.panel.selection().node == doc::NodeId{2U});
  click(80.0F, 135.0F, gui::KeyMods::Ctrl);
  UI_CHECK(members() == std::vector<doc::NodeId>{doc::NodeId{2U}});
  click(80.0F, 375.0F, gui::KeyMods::Shift);
  UI_CHECK(members() == (std::vector<doc::NodeId>{doc::NodeId{2U},
                                                  doc::NodeId{3U}}));
  // A plain click on one member of several picks it out on its own.
  click(80.0F, 375.0F, gui::KeyMods::None);
  UI_CHECK(members() == std::vector<doc::NodeId>{doc::NodeId{3U}});
}

// A rubber band dragged rightwards takes the nodes it wholly contains;
// leftwards, every node it touches. Shift adds, Ctrl takes away.
void a_rubber_band_selects_nodes() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::Saturation, 300.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(3U, compositor::NodeKind::Blur, 40.0F, 300.0F));
  // Screen bounds: 1 (40,86)-(216,150), 2 (300,86)-(476,150), 3 (40,326)-(216,390).
  CompositorHarness harness;
  const auto members = [&] {
    const auto span = harness.panel.selected_nodes();
    return std::vector<doc::NodeId>{span.begin(), span.end()};
  };
  static_cast<void>(
      drag_in_graph(harness, graph, 20.0F, 40.0F, 520.0F, 200.0F));
  UI_CHECK(members() == (std::vector<doc::NodeId>{doc::NodeId{1U},
                                                  doc::NodeId{2U}}));
  // Leftwards and only grazing node 1: it is touched, so it is taken.
  static_cast<void>(
      drag_in_graph(harness, graph, 260.0F, 200.0F, 100.0F, 140.0F));
  UI_CHECK(members() == std::vector<doc::NodeId>{doc::NodeId{1U}});
  static_cast<void>(drag_in_graph(harness, graph, 20.0F, 280.0F, 260.0F,
                                  420.0F, gui::KeyMods::Shift));
  UI_CHECK(members() == (std::vector<doc::NodeId>{doc::NodeId{1U},
                                                  doc::NodeId{3U}}));
  static_cast<void>(drag_in_graph(harness, graph, 20.0F, 40.0F, 260.0F,
                                  200.0F, gui::KeyMods::Ctrl));
  UI_CHECK(members() == std::vector<doc::NodeId>{doc::NodeId{3U}});
}

// Dragging one node of a selection moves all of it by the same amount, as
// one gesture; arrow keys nudge it.
void a_selection_moves_and_nudges_together() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::Saturation, 300.0F, 60.0F));
  CompositorHarness harness;
  static_cast<void>(
      drag_in_graph(harness, graph, 20.0F, 40.0F, 520.0F, 200.0F));
  const editor::CompositorFrame moved =
      drag_in_graph(harness, graph, 80.0F, 115.0F, 130.0F, 145.0F);
  UI_CHECK(moved.move_nodes ==
           (std::vector<editor::CompositorMoveIntent>{
               {doc::NodeId{1U}, 90.0F, 90.0F},
               {doc::NodeId{2U}, 350.0F, 90.0F}}));

  compositor::CompositorGraph settled = graph;
  settled.nodes[0U].editor_x = 90.0F;
  settled.nodes[0U].editor_y = 90.0F;
  settled.nodes[1U].editor_x = 350.0F;
  settled.nodes[1U].editor_y = 90.0F;
  static_cast<void>(harness.frame(settled, 700.0F, 500.0F, false));
  const editor::CompositorFrame right = harness.frame(
      settled, 700.0F, 500.0F, false, {}, gui::Key::Right);
  UI_CHECK(right.move_nodes ==
           (std::vector<editor::CompositorMoveIntent>{
               {doc::NodeId{1U}, 100.0F, 90.0F},
               {doc::NodeId{2U}, 360.0F, 90.0F}}));
  // A nudge says which way it went, so a run of them can be one undo step.
  UI_CHECK(right.numeric_nudge.has_value());
  UI_CHECK(right.numeric_nudge->direction ==
           editor::NumberFieldNudgeDirection::Increase);
  const editor::CompositorFrame fine =
      harness.frame(settled, 700.0F, 500.0F, false, {}, gui::Key::Up,
                    gui::KeyMods::Shift);
  // The first nudge is still on its way, so this one starts from it.
  UI_CHECK(fine.move_nodes ==
           (std::vector<editor::CompositorMoveIntent>{
               {doc::NodeId{1U}, 100.0F, 89.0F},
               {doc::NodeId{2U}, 360.0F, 89.0F}}));
  UI_CHECK(fine.numeric_nudge->property != right.numeric_nudge->property);
}

// Delete takes every selected node in one step, except Output.
void delete_takes_every_selected_node_but_output() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::Saturation, 300.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(3U, compositor::NodeKind::Output, 560.0F, 60.0F));
  CompositorHarness harness;
  static_cast<void>(
      drag_in_graph(harness, graph, 20.0F, 40.0F, 780.0F, 200.0F));
  UI_CHECK(harness.panel.selected_nodes().size() == 3U);
  const editor::CompositorFrame removed = harness.frame(
      graph, 700.0F, 500.0F, false, {}, gui::Key::Delete);
  UI_CHECK(removed.remove ==
           (editor::CompositorDeleteIntent{
               {{editor::CompositorSelectionKind::Node, doc::NodeId{1U}},
                {editor::CompositorSelectionKind::Node, doc::NodeId{2U}}}}));
  UI_CHECK(removed.consume_delete);
}

// Copy, Paste and Duplicate go to the Director, which keeps the copied
// nodes; what a paste adds is selected when it arrives.
void copy_paste_and_duplicate_ask_the_director() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::Saturation, 300.0F, 60.0F));
  CompositorHarness harness;
  static_cast<void>(
      drag_in_graph(harness, graph, 20.0F, 40.0F, 520.0F, 200.0F));
  const editor::CompositorFrame copied =
      harness.frame(graph, 400.0F, 300.0F, false, {}, gui::Key::Copy);
  UI_CHECK(copied.clipboard ==
           (editor::CompositorClipboardIntent{
               editor::CompositorClipboardAction::Copy,
               {doc::NodeId{1U}, doc::NodeId{2U}}}));
  const editor::CompositorFrame pasted =
      harness.frame(graph, 400.0F, 300.0F, false, {}, gui::Key::Paste);
  UI_CHECK(pasted.clipboard ==
           (editor::CompositorClipboardIntent{
               editor::CompositorClipboardAction::Paste, {}, 400.0F, 274.0F}));
  // The Director adds 90 and 91; they become the selection.
  compositor::CompositorGraph grown = graph;
  grown.nodes.push_back(
      graph_node(90U, compositor::NodeKind::Brightness, 400.0F, 274.0F));
  grown.nodes.push_back(
      graph_node(91U, compositor::NodeKind::Saturation, 660.0F, 274.0F));
  static_cast<void>(harness.frame(grown, 400.0F, 300.0F, false));
  const auto span = harness.panel.selected_nodes();
  UI_CHECK((std::vector<doc::NodeId>{span.begin(), span.end()}) ==
           (std::vector<doc::NodeId>{doc::NodeId{90U}, doc::NodeId{91U}}));
  const editor::CompositorFrame duplicated =
      harness.frame(grown, 400.0F, 300.0F, false, {}, gui::Key::Duplicate);
  UI_CHECK(duplicated.clipboard ==
           (editor::CompositorClipboardIntent{
               editor::CompositorClipboardAction::Duplicate,
               {doc::NodeId{90U}, doc::NodeId{91U}}, 32.0F, 32.0F}));
}

// F frames the selection, Home the whole graph.
void framing_brings_nodes_into_view() {
  // Composites have no Parameters, so the canvas keeps the panel's width.
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Composite, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::Composite, 1500.0F, 900.0F));
  CompositorHarness harness;
  static_cast<void>(harness.frame(graph, 80.0F, 115.0F, true));
  static_cast<void>(harness.frame(graph, 80.0F, 115.0F, false));
  static_cast<void>(harness.frame(graph, 450.0F, 333.0F, false, "f"));
  // The selected node is centred: the canvas middle is over it.
  const auto centred = harness.panel.node_at(graph, 450.0F, 333.0F);
  UI_CHECK(centred.has_value() && centred->first == doc::NodeId{1U});

  static_cast<void>(
      harness.frame(graph, 450.0F, 333.0F, false, {}, gui::Key::Home));
  // Both are on the canvas now.
  const float zoom = harness.panel.graph_zoom();
  const gui::UiPoint pan = harness.panel.graph_pan();
  for (const auto &[x, y] :
       std::array<std::pair<float, float>, 2U>{{{40.0F, 60.0F},
                                                {1500.0F, 900.0F}}}) {
    const float screen_x = pan.x + (x + 10.0F) * zoom;
    const float screen_y = 26.0F + pan.y + (y + 10.0F) * zoom;
    UI_CHECK(screen_x > 0.0F && screen_x < 900.0F);
    UI_CHECK(screen_y > 26.0F && screen_y < 640.0F);
    UI_CHECK(harness.panel.node_at(graph, screen_x, screen_y).has_value());
  }
}

// The right-click menu of a selection: its rows start 4 below its top, 23
// apart, with a 7-unit divider before Align and before Straighten.
[[nodiscard]] float context_row(float top, std::size_t row) {
  const float dividers = row >= 10U ? 14.0F : (row >= 4U ? 7.0F : 0.0F);
  return top + 4.0F + 23.0F * static_cast<float>(row) + 11.5F + dividers;
}

[[nodiscard]] editor::CompositorFrame
choose_from_context_menu(CompositorHarness &harness,
                         const compositor::CompositorGraph &graph, float x,
                         float y, std::size_t row) {
  static_cast<void>(harness.secondary(graph, x, y, true));
  static_cast<void>(harness.secondary(graph, x, y, false));
  const float item_y = context_row(y, row);
  static_cast<void>(harness.frame(graph, x + 40.0F, item_y, false));
  static_cast<void>(harness.frame(graph, x + 40.0F, item_y, true));
  return harness.frame(graph, x + 40.0F, item_y, false);
}

// The layout tools: align, distribute, straighten and arrange.
void layout_tools_place_nodes() {
  compositor::CompositorGraph row;
  row.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 40.0F, 60.0F));
  row.nodes.push_back(
      graph_node(2U, compositor::NodeKind::Saturation, 100.0F, 180.0F));
  row.nodes.push_back(
      graph_node(3U, compositor::NodeKind::Blur, 400.0F, 120.0F));
  {
    // Select all three, then Align Left (row 4) from node 1's menu.
    CompositorHarness harness;
    static_cast<void>(
        drag_in_graph(harness, row, 20.0F, 40.0F, 800.0F, 300.0F));
    UI_CHECK(choose_from_context_menu(harness, row, 80.0F, 115.0F, 4U)
                 .move_nodes ==
             (std::vector<editor::CompositorMoveIntent>{
                 {doc::NodeId{2U}, 40.0F, 180.0F},
                 {doc::NodeId{3U}, 40.0F, 120.0F}}));
  }
  {
    // Distribute Across (row 8): the 176-wide boxes span 40..576, leaving
    // gaps of (536 - 528) / 2 = 4, so the middle one starts at 220.
    CompositorHarness harness;
    static_cast<void>(
        drag_in_graph(harness, row, 20.0F, 40.0F, 800.0F, 300.0F));
    UI_CHECK(choose_from_context_menu(harness, row, 80.0F, 115.0F, 8U)
                 .move_nodes ==
             (std::vector<editor::CompositorMoveIntent>{
                 {doc::NodeId{2U}, 220.0F, 180.0F}}));
  }

  // Straighten (Q): a Composite fed from a Layer's IMAGE (row 0, 46 below
  // the Layer's top) moves so its LAYERS input (row 0) is level with it.
  compositor::CompositorGraph chain;
  chain.nodes.push_back(
      graph_node(10U, compositor::NodeKind::SceneLayer, 40.0F, 60.0F));
  chain.nodes.push_back(
      graph_node(11U, compositor::NodeKind::Composite, 320.0F, 200.0F));
  chain.nodes.push_back(
      graph_node(12U, compositor::NodeKind::Output, 100.0F, 400.0F));
  chain.edges.push_back(graph_edge(50U, 10U, 0U, 11U, 0U));
  chain.edges.push_back(graph_edge(51U, 11U, 0U, 12U, 0U));
  {
    CompositorHarness harness;
    static_cast<void>(harness.frame(chain, 360.0F, 255.0F, true));
    static_cast<void>(harness.frame(chain, 360.0F, 255.0F, false));
    UI_CHECK(harness.frame(chain, 360.0F, 255.0F, false, "q").move_nodes ==
             (std::vector<editor::CompositorMoveIntent>{
                 {doc::NodeId{11U}, 320.0F, 60.0F}}));
  }
  {
    // Arrange Graph (row 11, from a single node's menu): Output in the
    // rightmost column, Composite and Layer each 176 + 80 further left,
    // all along the top. The right edge stays at Composite's 320 + 176.
    CompositorHarness harness;
    const editor::CompositorFrame arranged =
        choose_from_context_menu(harness, chain, 360.0F, 255.0F, 11U);
    const float right = 320.0F + 176.0F;
    UI_CHECK(arranged.move_nodes ==
             (std::vector<editor::CompositorMoveIntent>{
                 {doc::NodeId{12U}, right - 176.0F, 60.0F},
                 {doc::NodeId{11U}, right - 176.0F - 256.0F, 60.0F},
                 {doc::NodeId{10U}, right - 176.0F - 512.0F, 60.0F}}));
  }
}

// A wire's menu disconnects it or adds a reroute point where it was
// clicked.
void a_wires_menu_disconnects_and_routes() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(10U, compositor::NodeKind::SceneLayer, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(11U, compositor::NodeKind::Output, 600.0F, 60.0F));
  graph.edges.push_back(graph_edge(50U, 10U, 0U, 11U, 0U));
  {
    CompositorHarness harness;
    static_cast<void>(harness.frame(graph, 400.0F, 132.0F, false));
    UI_CHECK(choose_from_context_menu(harness, graph, 400.0F, 132.0F, 1U)
                 .rewire ==
             (editor::CompositorRewireIntent{{doc::NodeId{50U}}, {}}));
  }
  {
    CompositorHarness harness;
    static_cast<void>(harness.frame(graph, 400.0F, 132.0F, false));
    UI_CHECK(choose_from_context_menu(harness, graph, 400.0F, 132.0F, 0U)
                 .waypoints ==
             (editor::CompositorWaypointIntent{doc::NodeId{50U},
                                               {{400.0F, 106.0F}}}));
  }
}

// Comment boxes: the title bar moves the box and the nodes wholly inside
// it, the corner resizes it, C draws one around the selection, and a
// double-click on the title renames it.
void comment_boxes_carry_their_nodes() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::Composite, 600.0F, 60.0F));
  graph.comments.push_back(compositor::GraphComment{
      doc::NodeId{70U}, {}, "Plates", 20.0F, 20.0F, 400.0F, 300.0F, 2U});
  // Title bar: screen (20, 46)-(420, 70). Corner: up to (420, 346).
  {
    CompositorHarness harness;
    const editor::CompositorFrame moved =
        drag_in_graph(harness, graph, 100.0F, 55.0F, 130.0F, 95.0F);
    UI_CHECK(moved.comment ==
             (editor::CompositorCommentIntent{doc::NodeId{70U}, 50.0F, 60.0F,
                                              400.0F, 300.0F, "Plates", 2U}));
    UI_CHECK(moved.move_nodes ==
             (std::vector<editor::CompositorMoveIntent>{
                 {doc::NodeId{1U}, 70.0F, 100.0F}}));
  }
  {
    CompositorHarness harness;
    const editor::CompositorFrame resized =
        drag_in_graph(harness, graph, 414.0F, 340.0F, 464.0F, 360.0F);
    UI_CHECK(resized.comment ==
             (editor::CompositorCommentIntent{doc::NodeId{70U}, 20.0F, 20.0F,
                                              450.0F, 320.0F, "Plates", 2U}));
    UI_CHECK(resized.move_nodes.empty());
  }
  {
    CompositorHarness harness;
    static_cast<void>(harness.frame(graph, 640.0F, 115.0F, true));
    static_cast<void>(harness.frame(graph, 640.0F, 115.0F, false));
    const editor::CompositorFrame boxed =
        harness.frame(graph, 640.0F, 115.0F, false, "c");
    UI_CHECK(boxed.comment.has_value());
    UI_CHECK(boxed.comment->node == doc::NodeId{});
    // Around node 2's (600, 60, 176, 64) with room for its title.
    UI_CHECK(boxed.comment->x < 600.0F && boxed.comment->y < 60.0F);
    UI_CHECK(boxed.comment->x + boxed.comment->width > 776.0F);
    UI_CHECK(boxed.comment->y + boxed.comment->height > 124.0F);
  }
  {
    CompositorHarness harness;
    static_cast<void>(harness.frame(graph, 100.0F, 55.0F, true));
    static_cast<void>(harness.frame(graph, 100.0F, 55.0F, false));
    static_cast<void>(harness.frame(graph, 100.0F, 55.0F, true));
    static_cast<void>(harness.frame(graph, 100.0F, 55.0F, false));
    static_cast<void>(harness.frame(graph, 100.0F, 55.0F, false));
    static_cast<void>(harness.frame(graph, 100.0F, 55.0F, false, "Car plates"));
    UI_CHECK(harness.frame(graph, 100.0F, 55.0F, false, {}, gui::Key::Enter)
                 .rename ==
             (editor::CompositorRenameIntent{doc::NodeId{70U}, "Car plates"}));
  }
}

// Double-click a node's title, or press F2, to type a new name; Escape
// leaves it as it was.
void node_titles_are_renamed_in_place() {
  // Screen (60, 66)-(236, 130): title down to 101, body below.
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 60.0F, 40.0F));
  {
    CompositorHarness harness;
    static_cast<void>(harness.frame(graph, 100.0F, 75.0F, true));
    static_cast<void>(harness.frame(graph, 100.0F, 75.0F, false));
    static_cast<void>(harness.frame(graph, 100.0F, 75.0F, true));
    static_cast<void>(harness.frame(graph, 100.0F, 75.0F, false));
    static_cast<void>(harness.frame(graph, 100.0F, 75.0F, false));
    static_cast<void>(harness.frame(graph, 100.0F, 75.0F, false, "Key light"));
    UI_CHECK(harness.frame(graph, 100.0F, 75.0F, false, {}, gui::Key::Enter)
                 .rename ==
             (editor::CompositorRenameIntent{doc::NodeId{1U}, "Key light"}));
  }
  {
    CompositorHarness harness;
    static_cast<void>(harness.frame(graph, 100.0F, 115.0F, true));
    static_cast<void>(harness.frame(graph, 100.0F, 115.0F, false));
    static_cast<void>(
        harness.frame(graph, 100.0F, 115.0F, false, {}, gui::Key::Rename));
    static_cast<void>(harness.frame(graph, 100.0F, 115.0F, false));
    static_cast<void>(harness.frame(graph, 100.0F, 115.0F, false, "Gone"));
    // F2 opened the field: what was typed went into it, not the graph.
    UI_CHECK(!harness.frame(graph, 100.0F, 115.0F, false, {}, gui::Key::Escape)
                  .rename.has_value());
    UI_CHECK(!harness.frame(graph, 100.0F, 115.0F, false, {}, gui::Key::Enter)
                  .rename.has_value());
  }
}

// The minimap shows the whole graph and recentres the view where it is
// pressed.
void the_minimap_recentres_the_view() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Brightness, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::Saturation, 3000.0F, 2000.0F));
  CompositorHarness harness;
  // The MAP toggle follows RESET in the toolbar, at x 268..312.
  static_cast<void>(harness.frame(graph, 290.0F, 13.0F, true));
  static_cast<void>(harness.frame(graph, 290.0F, 13.0F, false));
  const gui::UiPoint before = harness.panel.graph_pan();
  // The map sits in the canvas's bottom-right corner, 200 by 130; its
  // bottom-right reaches toward the far node.
  static_cast<void>(harness.frame(graph, 880.0F, 620.0F, true));
  static_cast<void>(harness.frame(graph, 880.0F, 620.0F, false));
  const gui::UiPoint after = harness.panel.graph_pan();
  UI_CHECK(after.x < before.x - 1000.0F);
  UI_CHECK(after.y < before.y - 1000.0F);
  // Nothing was selected or moved by pressing it.
  UI_CHECK(harness.panel.selected_nodes().empty());
}

void workspace_panels_draw_scene_relevant_content() {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  const std::vector<editor::ElementInfo> items{elements()};

  editor::CompositorPanel compositor{gui::ui_id("compositor.test")};
  input.new_frame();
  ctx.new_frame();
  compositor.draw(ctx, gui::Rect{0.0F, 0.0F, 900.0F, 190.0F}, items,
                  doc::NodeId{9U});
  UI_CHECK(list.vertices().size() > 300U);

  editor::OutputMonitorPanel monitor{gui::ui_id("monitor.test")};
  input.new_frame();
  ctx.new_frame();
  static_cast<void>(monitor.draw(
      ctx, gui::Rect{0.0F, 0.0F, 430.0F, 150.0F},
      editor::OutputMonitorState{240U, 3840U, 1080U, 60.0F, true}));
  // The monitor deliberately contains only the real Program output now; it
  // must still paint structured chrome and a truthful live state without
  // relying on fabricated destination rows or meters.
  UI_CHECK(list.vertices().size() > 50U);
  bool has_live_green = false;
  for (const gui::UiVertex &vertex : list.vertices()) {
    has_live_green = has_live_green || (vertex.g > 0.75F && vertex.r < 0.40F);
  }
  UI_CHECK(has_live_green);
}

struct OutputMonitorHarness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::OutputMonitorPanel panel{gui::ui_id("monitor.lifecycle.test")};
  static constexpr gui::Rect bounds{0.0F, 0.0F, 430.0F, 150.0F};
  gui::Rect panel_bounds{bounds};

  editor::OutputMonitorFrame frame(float x, float y, bool down,
                                   const editor::OutputMonitorState &state,
                                   float wheel = 0.0F) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.add_wheel(wheel);
    ctx.new_frame();
    return panel.draw(ctx, panel_bounds, state);
  }
};

void output_monitor_requests_conformance_without_a_lifecycle_action() {
  OutputMonitorHarness harness;
  editor::OutputMonitorState state{};
  state.cluster.conformance_active = true;
  harness.panel_bounds.height = 240.0F;
  // The cohort check is the Program row's own action, at its right edge.
  constexpr float x = 409.0F;
  constexpr float y = 68.0F;
  static_cast<void>(harness.frame(x, y, false, state));
  static_cast<void>(harness.frame(x, y, true, state));
  const auto clicked = harness.frame(x, y, false, state);
  UI_CHECK(clicked.check_render_conformance);
  UI_CHECK(!clicked.lifecycle.has_value());
  UI_CHECK(!harness.frame(x, y, false, state).check_render_conformance);
  state.cluster.conformance_active = false;
  static_cast<void>(harness.frame(x, y, true, state));
  UI_CHECK(!harness.frame(x, y, false, state).check_render_conformance);
}

void output_monitor_emits_one_fixed_lifecycle_intent() {
  OutputMonitorHarness harness;
  const editor::OutputMonitorState::Node node{
      doc::NodeId{50U},
      "Render A",
      "127.0.0.1:47840",
      3U,
      1920U,
      1080U,
      editor::OutputMonitorState::Node::Status::Running,
      4U,
      {}};
  editor::OutputMonitorState state{240U,  3840U, 1080U,
                                   60.0F, true,  std::span{&node, 1U}};
  static_cast<void>(harness.frame(400.0F, 92.0F, false, state));
  static_cast<void>(harness.frame(400.0F, 92.0F, true, state));
  const auto released = harness.frame(400.0F, 92.0F, false, state);
  UI_CHECK(released.lifecycle.has_value());
  UI_CHECK(released.lifecycle->device == doc::NodeId{50U});
  UI_CHECK(released.lifecycle->node_index == 3U);
  UI_CHECK(released.lifecycle->endpoint == "127.0.0.1:47840");
  UI_CHECK(released.lifecycle->action == editor::OutputLifecycleAction::Stop);
}

void duplicate_node_indices_have_no_ambiguous_controls() {
  OutputMonitorHarness harness;
  const editor::OutputMonitorState::Node node{
      doc::NodeId{50U},
      "Render A",
      "127.0.0.1:47840",
      3U,
      1920U,
      1080U,
      editor::OutputMonitorState::Node::Status::DuplicateNodeIndex,
      0U,
      {}};
  editor::OutputMonitorState state{240U,  3840U, 1080U,
                                   60.0F, true,  std::span{&node, 1U}};
  static_cast<void>(harness.frame(400.0F, 92.0F, false, state));
  static_cast<void>(harness.frame(400.0F, 92.0F, true, state));
  const auto released = harness.frame(400.0F, 92.0F, false, state);
  UI_CHECK(!released.lifecycle.has_value());
}

void missing_cluster_assets_block_start_but_keep_status_visible() {
  OutputMonitorHarness harness;
  editor::OutputMonitorState::Node node{
      doc::NodeId{50U},
      "Render A",
      "127.0.0.1:47840",
      3U,
      1920U,
      1080U,
      editor::OutputMonitorState::Node::Status::Stopped,
      0U,
      {}};
  node.content = editor::OutputMonitorState::Node::ContentStatus::Missing;
  node.assets_required = 2U;
  node.assets_ready = 1U;
  node.assets_missing = 1U;
  node.content_detail = "one required object is absent";
  editor::OutputMonitorState state{240U,  3840U, 1080U,
                                   60.0F, true,  std::span{&node, 1U}};
  static_cast<void>(harness.frame(400.0F, 92.0F, false, state));
  static_cast<void>(harness.frame(400.0F, 92.0F, true, state));
  const auto released = harness.frame(400.0F, 92.0F, false, state);
  UI_CHECK(!released.lifecycle.has_value());
  UI_CHECK(harness.list.vertices().size() > 50U);

  // Offline media keeps its warning but has a renderer fallback, so the
  // shipping Start control must remain reachable.
  node.missing_media_only = true;
  node.content_detail = "offline media will display placeholders";
  static_cast<void>(harness.frame(400.0F, 92.0F, true, state));
  const auto offline_start = harness.frame(400.0F, 92.0F, false, state);
  UI_CHECK(offline_start.lifecycle.has_value());
  UI_CHECK(offline_start.lifecycle->action == editor::OutputLifecycleAction::Start);
}

void content_labels_distinguish_missing_stale_and_verification_failures() {
  editor::OutputMonitorState::Node node{};
  node.content = editor::OutputMonitorState::Node::ContentStatus::Missing;
  node.assets_missing = 2U;
  node.assets_mismatched = 3U;
  UI_CHECK(editor::output_node_content_label(node) ==
           "ASSETS MISSING 2 / STALE 3");

  node.content = editor::OutputMonitorState::Node::ContentStatus::Error;
  node.assets_verification_failed = 4U;
  UI_CHECK(editor::output_node_content_label(node) == "ASSET VERIFY FAILED 4");
}

// A node whose displays never opened reports nothing for its renderer or its
// assets, so every other row on it reads like a slow load. The output status
// is the one field that separates "not driving its displays" from "not
// finished yet", and each physical fault has to stay distinguishable: an
// operator sent to check the assets when a display is mirrored looks in the
// wrong place entirely.
void output_failures_are_named_and_distinct_from_asset_failures() {
  using OutputStatus = editor::OutputMonitorState::Node::OutputStatus;
  editor::OutputMonitorState::Node node{};
  // Nothing reported is not a failure, and is never evidence of success.
  UI_CHECK(!node.output_failed());
  UI_CHECK(editor::output_node_deployment_label(node).empty());

  node.output = OutputStatus::Deployed;
  UI_CHECK(!node.output_failed());
  UI_CHECK(editor::output_node_deployment_label(node).empty());

  node.output = OutputStatus::DisplayMirrored;
  UI_CHECK(node.output_failed());
  const auto mirrored = editor::output_node_deployment_label(node);
  UI_CHECK(mirrored == "BOUND DISPLAY MIRRORS ANOTHER - EXTEND THE DESKTOP");

  node.output = OutputStatus::PresenterRefused;
  const auto refused = editor::output_node_deployment_label(node);
  UI_CHECK(refused == "DISPLAY DOES NOT OFFER THE SAVED MODE");
  UI_CHECK(refused != mirrored);

  node.output = OutputStatus::DisplayMissing;
  UI_CHECK(editor::output_node_deployment_label(node) !=
           "BOUND DISPLAY IS CONNECTED BUT SWITCHED OFF");
  node.output = OutputStatus::DisplayInactive;
  UI_CHECK(editor::output_node_deployment_label(node) ==
           "BOUND DISPLAY IS CONNECTED BUT SWITCHED OFF");

  // A display fault says nothing about the assets, and the asset label must
  // not start describing one.
  node.content = editor::OutputMonitorState::Node::ContentStatus::Ready;
  node.assets_required = 2U;
  node.assets_ready = 2U;
  UI_CHECK(editor::output_node_content_label(node) == "ASSETS 2/2 READY");
}

void process_running_is_distinct_from_editor_control() {
  editor::OutputMonitorState::Node node{};
  node.status = editor::OutputMonitorState::Node::Status::Running;
  UI_CHECK(editor::output_node_connection_label(node) == "CONNECTING");
  node.connection = editor::OutputMonitorState::Node::Connection::CatchingUp;
  UI_CHECK(editor::output_node_connection_label(node) == "CATCHING UP");
  node.connection = editor::OutputMonitorState::Node::Connection::Live;
  UI_CHECK(editor::output_node_connection_label(node) == "LIVE");
}

void output_monitor_scroll_reaches_later_cluster_nodes() {
  OutputMonitorHarness harness;
  editor::OutputMonitorState state{240U, 3840U, 1080U, 60.0F, true};
  std::vector<editor::OutputMonitorState::Node> nodes;
  for (core::u32 index = 0U; index < 4U; ++index) {
    nodes.push_back(editor::OutputMonitorState::Node{
        doc::NodeId{60U + index},
        "Render",
        "127.0.0.1:47840",
        index,
        1920U,
        1080U,
        editor::OutputMonitorState::Node::Status::Stopped,
        0U,
        {}});
  }
  state.nodes = nodes;
  // Scroll the list to its lower bound, then click Start on the last row,
  // which then sits at the bottom of the list, y=86..118.
  static_cast<void>(harness.frame(400.0F, 90.0F, false, state, -10.0F));
  static_cast<void>(harness.frame(409.0F, 102.0F, false, state));
  static_cast<void>(harness.frame(409.0F, 102.0F, true, state));
  const auto released = harness.frame(409.0F, 102.0F, false, state);
  UI_CHECK(released.lifecycle.has_value());
  UI_CHECK(released.lifecycle->device == doc::NodeId{63U});
  UI_CHECK(released.lifecycle->node_index == 3U);
  UI_CHECK(released.lifecycle->action == editor::OutputLifecycleAction::Start);
}

void output_monitor_emits_explicit_cluster_relaunch() {
  OutputMonitorHarness harness;
  const editor::OutputMonitorState::Node node{
      doc::NodeId{50U},
      "Render A",
      "127.0.0.1:47840",
      3U,
      1920U,
      1080U,
      editor::OutputMonitorState::Node::Status::Running,
      4U,
      {}};
  editor::OutputMonitorState state{240U,  3840U, 1080U,
                                   60.0F, true,  std::span{&node, 1U}};
  // Footer buttons run right to left: Stop all, Restart all, Start all.
  // With the glyphless test font each is its icon and padding, 44 wide:
  // Restart all is x=330..374.
  static_cast<void>(harness.frame(352.0F, 134.0F, false, state));
  static_cast<void>(harness.frame(352.0F, 134.0F, true, state));
  const auto released = harness.frame(352.0F, 134.0F, false, state);
  UI_CHECK(released.lifecycle.has_value());
  UI_CHECK(released.lifecycle->all);
  UI_CHECK(released.lifecycle->device == doc::NodeId{});
  UI_CHECK(released.lifecycle->endpoint.empty());
  UI_CHECK(released.lifecycle->action ==
           editor::OutputLifecycleAction::Restart);
}

// Stopping every output takes the wall dark, so Stop all asks twice: the
// first press only arms it, the second stops.
void output_monitor_stop_all_asks_before_stopping() {
  OutputMonitorHarness harness;
  const editor::OutputMonitorState::Node node{
      doc::NodeId{50U},
      "Render A",
      "127.0.0.1:47840",
      3U,
      1920U,
      1080U,
      editor::OutputMonitorState::Node::Status::Running,
      4U,
      {}};
  editor::OutputMonitorState state{240U,  3840U, 1080U,
                                   60.0F, true,  std::span{&node, 1U}};
  // Stop all is the rightmost footer button, x=380..424.
  const auto press = [&] {
    static_cast<void>(harness.frame(402.0F, 134.0F, false, state));
    static_cast<void>(harness.frame(402.0F, 134.0F, true, state));
    return harness.frame(402.0F, 134.0F, false, state);
  };
  const auto armed = press();
  UI_CHECK(!armed.lifecycle.has_value());
  const auto stopped = press();
  UI_CHECK(stopped.lifecycle.has_value());
  UI_CHECK(stopped.lifecycle->all);
  UI_CHECK(stopped.lifecycle->action == editor::OutputLifecycleAction::Stop);
  // And it disarms: the next press arms again rather than stopping.
  UI_CHECK(!press().lifecycle.has_value());
}

// An expanded render-server row offers its log whatever the process is
// doing. Opening it asks the agent at once, never twice while a read is on
// its way, again on a timer while it stays open, and not at all once closed.
void output_monitor_opens_a_node_log_and_refreshes_it() {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::OutputMonitorPanel panel{gui::ui_id("monitor.log.test")};
  const gui::Rect bounds{0.0F, 0.0F, 640.0F, 600.0F};
  editor::OutputMonitorState::Node node{
      doc::NodeId{70U},
      "Render A",
      "127.0.0.1:47840",
      2U,
      1920U,
      1080U,
      editor::OutputMonitorState::Node::Status::Faulted,
      4U,
      {}};
  editor::OutputMonitorState state{240U,  3840U, 1080U,
                                   60.0F, true,  std::span{&node, 1U}};
  double now = 10.0;
  const auto frame = [&](float x, float y, bool down) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    ctx.new_frame(now);
    return panel.draw(ctx, bounds, state);
  };
  const auto click = [&](float x, float y) {
    static_cast<void>(frame(x, y, false));
    static_cast<void>(frame(x, y, true));
    return frame(x, y, false);
  };
  // The row (y 84..116) opens its details; a faulted row has two lines of
  // them (endpoint, status), so the log toggle sits at 118 + 40 = 158..180.
  UI_CHECK(!click(120.0F, 100.0F).lifecycle.has_value());
  constexpr float toggle_x = 80.0F;
  constexpr float toggle_y = 169.0F;
  const auto opened = click(toggle_x, toggle_y);
  UI_CHECK(!opened.lifecycle.has_value());
  // The next frame draws the log and asks for it.
  const auto asked = frame(500.0F, 590.0F, false);
  UI_CHECK(asked.lifecycle.has_value());
  UI_CHECK(asked.lifecycle->action ==
           editor::OutputLifecycleAction::FetchLog);
  UI_CHECK(asked.lifecycle->device == doc::NodeId{70U});
  UI_CHECK(asked.lifecycle->node_index == 2U);
  UI_CHECK(asked.lifecycle->endpoint == "127.0.0.1:47840");

  // While the read is on its way, nothing more is asked, even later.
  node.log_fetching = true;
  now += 10.0;
  UI_CHECK(!frame(500.0F, 590.0F, false).lifecycle.has_value());
  UI_CHECK(editor::output_node_log_summary(node) == "Reading the log");

  auto log = std::make_shared<editor::OutputNodeLog>();
  log->status = editor::OutputNodeLog::Status::Available;
  log->fetched_at = "12:00:10";
  log->previous_run_lines = 1U;
  for (int index = 0; index < 40; ++index) {
    log->lines.push_back("line " + std::to_string(index));
  }
  log->lines.front() = "NODE 2 FELL BEHIND";
  node.log = log;
  node.log_fetching = false;
  UI_CHECK(editor::output_node_log_summary(node) ==
           "Read at 12:00:10 - 40 lines, 1 from the previous run");
  const std::size_t before = list.vertices().size();
  static_cast<void>(frame(500.0F, 590.0F, false));
  // Lines are drawn (the empty test atlas still emits quads for glyph runs,
  // so a drawn log adds geometry).
  UI_CHECK(list.vertices().size() >= before);

  // Still open: a few seconds on, it asks again.
  now += editor::OutputMonitorPanel::log_refresh_seconds + 0.1;
  const auto refreshed = frame(500.0F, 590.0F, false);
  UI_CHECK(refreshed.lifecycle.has_value());
  UI_CHECK(refreshed.lifecycle->action ==
           editor::OutputLifecycleAction::FetchLog);

  // Closed, it never asks again.
  const auto closed = click(toggle_x, toggle_y);
  UI_CHECK(!closed.lifecycle.has_value() ||
           closed.lifecycle->action !=
               editor::OutputLifecycleAction::FetchLog);
  now += 60.0;
  UI_CHECK(!frame(500.0F, 590.0F, false).lifecycle.has_value());

  // The wording of the other outcomes.
  auto failed = std::make_shared<editor::OutputNodeLog>();
  failed->status = editor::OutputNodeLog::Status::Failed;
  failed->detail = "lifecycle listener did not answer in time";
  node.log = failed;
  UI_CHECK(editor::output_node_log_summary(node) ==
           "Could not read the log - lifecycle listener did not answer in "
           "time");
  auto none = std::make_shared<editor::OutputNodeLog>();
  none->status = editor::OutputNodeLog::Status::Unavailable;
  node.log = none;
  UI_CHECK(editor::output_node_log_summary(node) == "No log on this machine");
  node.log.reset();
  UI_CHECK(editor::output_node_log_summary(node) == "Log not read yet");
}

void output_monitor_cluster_health_keeps_node_controls_reachable() {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::OutputMonitorPanel panel{gui::ui_id("monitor.cluster.health.test")};
  constexpr gui::Rect bounds{0.0F, 0.0F, 430.0F, 190.0F};
  const editor::OutputMonitorState::Node node{
      doc::NodeId{50U},
      "Render A",
      "127.0.0.1:47840",
      3U,
      1920U,
      1080U,
      editor::OutputMonitorState::Node::Status::Running,
      4U,
      {}};
  editor::OutputMonitorState state{240U,  3840U, 1080U,
                                   60.0F, true,  std::span{&node, 1U}};
  state.cluster.health = editor::OutputMonitorState::Cluster::Health::Healthy;
  state.cluster.configured_nodes = 1U;
  state.cluster.live_sessions = 1U;
  state.cluster.diagnostics_recording = true;

  const auto frame = [&](bool down) {
    input.new_frame();
    input.set_mouse_position(409.0F, 100.0F);
    input.set_mouse_button(gui::MouseButton::Left, down);
    ctx.new_frame();
    return panel.draw(ctx, bounds, state);
  };
  static_cast<void>(frame(false));
  static_cast<void>(frame(true));
  const auto released = frame(false);
  UI_CHECK(released.lifecycle.has_value());
  UI_CHECK(released.lifecycle->action == editor::OutputLifecycleAction::Stop);
}

// The Preferences dialog is modal and covers the whole window, so its scrim
// competes with its own controls for the pointer. gui::UiContext resolves a
// frame first-wins, which once made every control in the card unreachable.
struct PreferencesHarness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::PreferencesPanel panel{gui::ui_id("preferences.test")};
  editor::EditorPreferencesState state;
  static constexpr gui::Rect bounds{0.0F, 0.0F, 1600.0F, 900.0F};

  // The card is centred in `bounds`; these mirror the panel's own layout.
  static constexpr float card_width = 620.0F;
  static constexpr float card_height = 780.0F;
  static constexpr float card_x = (1600.0F - card_width) * 0.5F;
  static constexpr float card_y = (900.0F - card_height) * 0.5F;

  // The form's rhythm with the default metrics: 24px controls 6px apart, a
  // 132px label column, 14px in from the card's sides. Each section's rule
  // sits 10px below the row above it and its heading 10px below the rule.
  // The footer buttons are 84px wide, 6px apart, 6px up from the card's
  // bottom edge.
  [[nodiscard]] static gui::Rect cancel_button() {
    return gui::Rect{card_x + card_width - 14.0F - 84.0F - 6.0F - 84.0F,
                     card_y + card_height - 30.0F, 84.0F, 24.0F};
  }
  [[nodiscard]] static gui::Rect save_button() {
    return gui::Rect{card_x + card_width - 14.0F - 84.0F,
                     card_y + card_height - 30.0F, 84.0F, 24.0F};
  }
  [[nodiscard]] static gui::Rect first_field() {
    return gui::Rect{card_x + 14.0F + 132.0F, card_y + 66.0F,
                     card_width - 28.0F - 132.0F, 24.0F};
  }
  [[nodiscard]] static gui::Rect next_to_source_checkbox() {
    return gui::Rect{card_x + 14.0F, card_y + 402.0F, card_width - 28.0F,
                     24.0F};
  }
  [[nodiscard]] static gui::Rect clear_proxy_cache_button() {
    return gui::Rect{card_x + 14.0F, card_y + 522.0F, 132.0F, 24.0F};
  }
  [[nodiscard]] static gui::Rect low_latency_preset() {
    const float width = (card_width - 28.0F - 18.0F) / 4.0F;
    return gui::Rect{card_x + 14.0F, card_y + 591.0F, width, 24.0F};
  }
  [[nodiscard]] static gui::Rect balanced_transcode_priority() {
    const float start = card_x + 14.0F + 132.0F;
    const float width = (card_x + card_width - 14.0F - start - 12.0F) / 3.0F;
    return gui::Rect{start + width + 6.0F, card_y + 492.0F, width, 24.0F};
  }
  // RENDER NODES: the password well, then its CLEAR button.
  [[nodiscard]] static gui::Rect password_field() {
    return gui::Rect{card_x + 146.0F, card_y + 309.0F, 370.0F, 24.0F};
  }
  [[nodiscard]] static gui::Rect clear_password_button() {
    return gui::Rect{card_x + card_width - 14.0F - 84.0F, card_y + 309.0F,
                     84.0F, 24.0F};
  }
  // The form is 771px tall (its last line, the status message, ends 765px
  // below the content's top, plus the 6px margin) in a 710px viewport
  // between the 34px header and the 36px footer: 61px of scroll.
  static constexpr float max_scroll = 771.0F - 710.0F;

  editor::PreferencesFrame wheel(float notches) {
    input.new_frame();
    input.set_mouse_position(card_x + card_width * 0.5F, card_y + 300.0F);
    input.add_wheel(notches);
    ctx.new_frame();
    return panel.draw(ctx, bounds, state);
  }

  editor::PreferencesFrame frame(float x, float y, bool down,
                                 std::string_view text = {}) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.add_text(text);
    ctx.new_frame();
    return panel.draw(ctx, bounds, state);
  }

  // Press and release over one point, which is what gui::UiContext turns into
  // a click.
  editor::PreferencesFrame click(const gui::Rect &target) {
    const float x = target.x + target.width * 0.5F;
    const float y = target.y + target.height * 0.5F;
    static_cast<void>(frame(x, y, false));
    static_cast<void>(frame(x, y, true));
    return frame(x, y, false);
  }
};

[[nodiscard]] editor::EditorPreferences stored_preferences() {
  editor::EditorPreferences preferences{};
  preferences.director_listen = "0.0.0.0:45060";
  preferences.show_id = "0000000000000001000000000000000B";
  return preferences;
}

void cancel_closes_the_preferences_dialog() {
  PreferencesHarness harness;
  harness.state.stored = stored_preferences();
  harness.panel.open(harness.state.stored);
  UI_CHECK(harness.panel.is_open());

  const auto result = harness.click(PreferencesHarness::cancel_button());
  UI_CHECK(!result.save.has_value());
  UI_CHECK(!harness.panel.is_open());
}

void cancel_discards_edits_so_reopening_shows_the_stored_values() {
  PreferencesHarness harness;
  harness.state.stored = stored_preferences();
  harness.panel.open(harness.state.stored);

  // Focus the first field and type into it, then cancel.
  static_cast<void>(harness.click(PreferencesHarness::first_field()));
  static_cast<void>(harness.frame(0.0F, 0.0F, false, "9"));
  UI_CHECK(harness.panel.draft() != harness.state.stored);
  static_cast<void>(harness.click(PreferencesHarness::cancel_button()));
  UI_CHECK(!harness.panel.is_open());

  harness.panel.open(harness.state.stored);
  UI_CHECK(harness.panel.draft() == harness.state.stored);
}

void save_reports_the_edited_values_and_closes() {
  PreferencesHarness harness;
  harness.state.stored = stored_preferences();
  harness.panel.open(harness.state.stored);

  static_cast<void>(harness.click(PreferencesHarness::first_field()));
  static_cast<void>(harness.frame(0.0F, 0.0F, false, "9"));
  const auto result = harness.click(PreferencesHarness::save_button());
  UI_CHECK(result.save.has_value());
  UI_CHECK(result.save->director_listen !=
           harness.state.stored.director_listen);
  UI_CHECK(result.save->show_id == harness.state.stored.show_id);
  UI_CHECK(!harness.panel.is_open());
}

void save_stays_disabled_until_something_actually_changes() {
  PreferencesHarness harness;
  harness.state.stored = stored_preferences();
  harness.panel.open(harness.state.stored);

  // Nothing edited yet, so Save must neither fire nor dismiss the card.
  const auto result = harness.click(PreferencesHarness::save_button());
  UI_CHECK(!result.save.has_value());
  UI_CHECK(harness.panel.is_open());
}

void proxy_storage_mode_is_part_of_the_saved_draft() {
  PreferencesHarness harness;
  harness.state.stored = stored_preferences();
  harness.panel.open(harness.state.stored);

  static_cast<void>(
      harness.click(PreferencesHarness::next_to_source_checkbox()));
  UI_CHECK(harness.panel.draft().proxy_cache_next_to_source);
  const auto result = harness.click(PreferencesHarness::save_button());
  UI_CHECK(result.save.has_value());
  UI_CHECK(result.save->proxy_cache_next_to_source);
}

void clear_proxy_cache_reports_without_saving_or_closing() {
  PreferencesHarness harness;
  harness.state.stored = stored_preferences();
  harness.panel.open(harness.state.stored);

  const auto result =
      harness.click(PreferencesHarness::clear_proxy_cache_button());
  UI_CHECK(result.clear_proxy_cache);
  UI_CHECK(!result.save.has_value());
  UI_CHECK(harness.panel.is_open());
}

void playback_preset_is_part_of_the_saved_machine_draft() {
  PreferencesHarness harness;
  harness.state.stored = stored_preferences();
  harness.panel.open(harness.state.stored);

  static_cast<void>(harness.click(PreferencesHarness::low_latency_preset()));
  UI_CHECK(harness.panel.draft().output_buffer_frames == 3U);
  UI_CHECK(harness.panel.draft().output_read_ahead_frames == 3U);
  const auto result = harness.click(PreferencesHarness::save_button());
  UI_CHECK(result.save.has_value());
  UI_CHECK(result.save->output_buffer_frames == 3U);
  UI_CHECK(result.save->output_read_ahead_frames == 3U);
}

// The form measures its own height, so the wheel scrolls exactly as far as the
// last line needs and no further - not to a constant that has to be grown by
// hand every time a setting is added.
void preferences_scroll_stops_at_the_end_of_the_form() {
  PreferencesHarness harness;
  harness.state.stored = stored_preferences();
  harness.panel.open(harness.state.stored);
  static_cast<void>(harness.frame(0.0F, 0.0F, false));

  static_cast<void>(harness.wheel(-40.0F));
  gui::Rect preset = PreferencesHarness::low_latency_preset();
  preset.y -= PreferencesHarness::max_scroll;
  static_cast<void>(harness.click(preset));
  UI_CHECK(harness.panel.draft().output_buffer_frames == 3U);
  UI_CHECK(harness.panel.draft().output_read_ahead_frames == 3U);

  // Reopening starts back at the top.
  static_cast<void>(harness.click(PreferencesHarness::cancel_button()));
  harness.panel.open(harness.state.stored);
  static_cast<void>(harness.click(PreferencesHarness::low_latency_preset()));
  UI_CHECK(harness.panel.draft().output_buffer_frames == 3U);
}

void transcode_priority_is_part_of_the_saved_machine_draft() {
  PreferencesHarness harness;
  harness.state.stored = stored_preferences();
  harness.panel.open(harness.state.stored);

  static_cast<void>(
      harness.click(PreferencesHarness::balanced_transcode_priority()));
  UI_CHECK(harness.panel.draft().proxy_transcode_priority ==
           editor::EditorPreferences::TranscodePriority::Balanced);
  const auto result = harness.click(PreferencesHarness::save_button());
  UI_CHECK(result.save.has_value());
  UI_CHECK(result.save->proxy_transcode_priority ==
           editor::EditorPreferences::TranscodePriority::Balanced);
}

// The render node password is typed, never shown back, and reported with
// the save only when the operator touched it: a new password, or empty for
// none. Untouched, the stored one stays.
void a_render_node_password_is_typed_cleared_and_saved() {
  PreferencesHarness harness;
  harness.state.stored = stored_preferences();
  harness.panel.open(harness.state.stored);
  static_cast<void>(harness.click(PreferencesHarness::password_field()));
  static_cast<void>(harness.frame(0.0F, 0.0F, false, "hunter2"));
  UI_CHECK(harness.panel.draft().render_node_password);
  auto typed = harness.click(PreferencesHarness::save_button());
  UI_CHECK(typed.save.has_value());
  UI_CHECK(typed.save->render_node_password);
  UI_CHECK(typed.render_node_password == std::optional<std::string>{"hunter2"});

  // Set before: CLEAR removes it.
  harness.state.stored.render_node_password = true;
  harness.panel.open(harness.state.stored);
  static_cast<void>(
      harness.click(PreferencesHarness::clear_password_button()));
  UI_CHECK(!harness.panel.draft().render_node_password);
  auto cleared = harness.click(PreferencesHarness::save_button());
  UI_CHECK(cleared.save.has_value());
  UI_CHECK(!cleared.save->render_node_password);
  UI_CHECK(cleared.render_node_password == std::optional<std::string>{""});

  // Untouched: another change saves, the password is not reported.
  harness.panel.open(harness.state.stored);
  static_cast<void>(
      harness.click(PreferencesHarness::next_to_source_checkbox()));
  auto other = harness.click(PreferencesHarness::save_button());
  UI_CHECK(other.save.has_value());
  UI_CHECK(other.save->render_node_password);
  UI_CHECK(!other.render_node_password.has_value());
}

void only_a_click_outside_the_card_dismisses_it() {
  PreferencesHarness harness;
  harness.state.stored = stored_preferences();
  harness.panel.open(harness.state.stored);

  // The card's own background is inert: a stray click inside must not throw
  // away whatever the operator has typed so far.
  static_cast<void>(harness.click(gui::Rect{
      PreferencesHarness::card_x + 20.0F,
      PreferencesHarness::card_y + PreferencesHarness::card_height - 70.0F,
      10.0F, 10.0F}));
  UI_CHECK(harness.panel.is_open());

  static_cast<void>(harness.click(gui::Rect{4.0F, 4.0F, 10.0F, 10.0F}));
  UI_CHECK(!harness.panel.is_open());
}

// The Project Settings dialog shares Preferences' modal card geometry, and
// unlike Preferences its result is a document edit.
struct ProjectSettingsHarness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::ProjectSettingsPanel panel{gui::ui_id("project-settings.test")};
  editor::ProjectSettingsState state;
  static constexpr gui::Rect bounds{0.0F, 0.0F, 1600.0F, 900.0F};
  static constexpr float card_width = 620.0F;
  static constexpr float card_height = 700.0F;
  static constexpr float card_x = (1600.0F - card_width) * 0.5F;
  static constexpr float card_y = (900.0F - card_height) * 0.5F;

  // The default metrics: under the 34px header, the intro caption 6px down
  // (18px tall, 2px after) and the 24px tabs 6px apart; the fixed part ends
  // 6px below them, then a 1px rule, then the scrolling pane, whose first
  // row is 6px down. Rows are 24px with 6px between, notes 18px with 2px
  // after, 14px in from the card's sides, fields after a 132px label column.
  static constexpr float tab_y = card_y + 34.0F + 6.0F + 18.0F + 2.0F;
  static constexpr float content_y = tab_y + 24.0F + 6.0F + 1.0F + 6.0F;
  static constexpr float tab_width = (card_width - 28.0F - 6.0F) / 2.0F;

  // DEFAULTS on the left; CANCEL and APPLY on the right, 6px apart; all 84px
  // wide and 6px up from the card's bottom edge.
  [[nodiscard]] static gui::Rect defaults_button() {
    return gui::Rect{card_x + 14.0F, card_y + card_height - 30.0F, 84.0F,
                     24.0F};
  }
  [[nodiscard]] static gui::Rect cancel_button() {
    return gui::Rect{card_x + card_width - 14.0F - 84.0F - 6.0F - 84.0F,
                     card_y + card_height - 30.0F, 84.0F, 24.0F};
  }
  [[nodiscard]] static gui::Rect apply_button() {
    return gui::Rect{card_x + card_width - 14.0F - 84.0F,
                     card_y + card_height - 30.0F, 84.0F, 24.0F};
  }
  [[nodiscard]] static gui::Rect category_tab(std::size_t index) {
    return gui::Rect{card_x + 14.0F +
                         static_cast<float>(index) * (tab_width + 6.0F),
                     tab_y, tab_width, 24.0F};
  }
  [[nodiscard]] static gui::Rect tracking_tab() { return category_tab(0U); }
  [[nodiscard]] static gui::Rect lens_tab() { return category_tab(1U); }
  // LENS CALIBRATION heading, then the Projection row's first choice.
  [[nodiscard]] static gui::Rect calibrated_lens_button() {
    return gui::Rect{card_x + 14.0F + 132.0F, content_y + 24.0F, 116.0F,
                     24.0F};
  }
  // After the Projection row and its note: two columns 14px apart, each a
  // 66px label, the track, a 6px gap and a 48px value. The track is 18px
  // tall, centred in its row.
  [[nodiscard]] static gui::Rect lens_sensor_width_track() {
    const float y = content_y + 24.0F + 30.0F + 20.0F;
    const float column = (card_width - 28.0F - 14.0F) * 0.5F;
    return gui::Rect{card_x + 14.0F + 66.0F, y + 3.0F,
                     column - 66.0F - 48.0F - 6.0F, 18.0F};
  }
  // INNER FRUSTUM heading, then the enable checkbox; Latency and its note,
  // then the prediction checkbox.
  [[nodiscard]] static gui::Rect inner_frustum_checkbox() {
    return gui::Rect{card_x + 14.0F, content_y + 24.0F, card_width - 28.0F,
                     24.0F};
  }
  [[nodiscard]] static gui::Rect predict_checkbox() {
    return gui::Rect{card_x + 14.0F, content_y + 24.0F + 30.0F + 30.0F + 20.0F,
                     card_width - 28.0F, 24.0F};
  }

  editor::ProjectSettingsFrame frame(float x, float y, bool down) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    ctx.new_frame();
    return panel.draw(ctx, bounds, state);
  }

  editor::ProjectSettingsFrame click(const gui::Rect &target) {
    const float x = target.x + target.width * 0.5F;
    const float y = target.y + target.height * 0.5F;
    static_cast<void>(frame(x, y, false));
    static_cast<void>(frame(x, y, true));
    return frame(x, y, false);
  }
};

// Tests for the docked, immediately authored scene controls. A frame adopts
// emitted values just as the Director does, so subsequent edits must preserve
// earlier changes and keep the correct node's base values.
struct LiveSettingsHarness {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input,list,font};
  editor::SceneSettingsPanel panel;
  editor::ProjectSettingsState state;
  editor::SceneSettingsTarget target{editor::SceneSettingsTarget::Renderer};
  std::vector<scene::ChannelKeyframes> keys;
  std::vector<editor::ElementInfo> elements;
  mediaserver::core::u64 time{};
  gui::Rect bounds{0,0,330,2000};
  LiveSettingsHarness() {state.stage_node=doc::NodeId{10};state.node=doc::NodeId{20};}
  editor::SceneSettingsFrame draw() {
    auto result=panel.draw(ctx,bounds,target,state,keys,time,elements);
    ctx.draw_overlays(bounds);
    if(result.project.apply) {
      state.renderer=result.project.apply->renderer;state.settings=result.project.apply->settings;
      state.environment=result.project.apply->environment;state.lens_profile=result.project.apply->lens_profile;
    }
    return result;
  }
  editor::SceneSettingsFrame frame(float x,float y,bool down) {
    input.new_frame();input.set_mouse_position(x,y);input.set_mouse_button(gui::MouseButton::Left,down);ctx.new_frame();return draw();
  }
  editor::SceneSettingsFrame click(float y,float x=200) {
    static_cast<void>(frame(x,y+11,false));static_cast<void>(frame(x,y+11,true));return frame(x,y+11,false);
  }
  editor::SceneSettingsFrame key(gui::Key key) {
    input.new_frame();input.add_key(key);ctx.new_frame();return draw();
  }
  editor::SceneSettingsFrame enter(float y,std::string_view value) {
    static_cast<void>(click(y));input.new_frame();input.add_text(value);ctx.new_frame();static_cast<void>(draw());return key(gui::Key::Enter);
  }
  editor::SceneSettingsFrame choose(float y,std::size_t from,std::size_t to) {
    static_cast<void>(click(y));
    while(from<to){static_cast<void>(key(gui::Key::Down));++from;}
    while(from>to){static_cast<void>(key(gui::Key::Up));--from;}
    return key(gui::Key::Enter);
  }
  void advanced_section(std::size_t section) {
    state.renderer.backend=scene::RendererBackend::Advanced;
    static_cast<void>(click(58,30));
    static_cast<void>(click(58.0F+40.0F*static_cast<float>(section),30));
  }
  void environment_section(std::size_t section) {
    target=editor::SceneSettingsTarget::Environment;
    state.renderer.backend=scene::RendererBackend::Advanced;
    static_cast<void>(click(58,30));
    static_cast<void>(click(58.0F+40.0F*static_cast<float>(section),30));
  }
};

void project_settings_apply_reports_the_edited_values() {
  ProjectSettingsHarness h;h.panel.open(h.state.settings,h.state.renderer);
  UI_CHECK(!h.click(ProjectSettingsHarness::apply_button()).apply);
  static_cast<void>(h.click(ProjectSettingsHarness::inner_frustum_checkbox()));
  auto result=h.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(result.apply && result.apply->renderer.icvfx.inner_frustum!=h.state.renderer.icvfx.inner_frustum);
  UI_CHECK(!h.panel.is_open());
}

void scene_file_fields_request_the_matching_picker_without_editing() {
  LiveSettingsHarness environment;
  environment.target = editor::SceneSettingsTarget::Environment;
  environment.state.renderer.backend = scene::RendererBackend::Advanced;
  environment.state.environment.map_source = "C:/lighting/stage.exr";
  const auto map = environment.click(154, 285);
  UI_CHECK(map.file_picker && map.file_picker->kind == editor::FilePickerKind::EnvironmentMap);
  UI_CHECK(map.file_picker->current == "C:/lighting/stage.exr");
  UI_CHECK(!map.project.apply);

  LiveSettingsHarness renderer;
  renderer.advanced_section(2);
  renderer.state.renderer.color.show_lut_source = "C:/color/show.cube";
  const auto lut = renderer.click(234, 285);
  UI_CHECK(lut.file_picker && lut.file_picker->kind == editor::FilePickerKind::ShowLut);
  UI_CHECK(lut.file_picker->current == "C:/color/show.cube");
  UI_CHECK(!lut.project.apply);

  LiveSettingsHarness bake;
  bake.environment_section(2);
  bake.panel.set_bake_destination("C:/exports/lighting");
  const auto folder = bake.click(618, 285);
  UI_CHECK(folder.file_picker && folder.file_picker->kind == editor::FilePickerKind::BakeFolder);
  UI_CHECK(folder.file_picker->current == "C:/exports/lighting");
  UI_CHECK(!folder.project.apply && !folder.project.bake);
}

void baked_lighting_page_starts_cancels_and_preserves_the_authored_field() {
  LiveSettingsHarness h;h.state.environment.baked.enabled=true;h.state.environment.baked.source="lighting.msgi";
  h.state.environment.baked.content="1234567890abcdef";h.state.environment.baked.count_x=3;
  h.environment_section(2);
  h.state.renderer.backend=scene::RendererBackend::Basic;
  UI_CHECK(!h.click(650.0).project.bake);
  h.state.renderer.backend=scene::RendererBackend::Advanced;
  auto bake=h.click(650.0);
  UI_CHECK(bake.project.bake && *bake.project.bake==h.state.environment.baked && !bake.project.bake_destination.empty());
  h.state.bake_running=true;auto cancel=h.click(650.0);UI_CHECK(cancel.project.cancel_bake && !cancel.project.bake);
}

void reflection_targets_choose_scene_objects_without_truncating_ids() {
  LiveSettingsHarness h;
  editor::ElementInfo floor{};
  floor.node = doc::NodeId{0xF123456789ABCDEFULL};
  floor.kind = editor::ElementKind::Object;
  floor.display_name = "Stage floor";
  h.elements.push_back(floor);
  h.state.environment.planar.enabled = true;
  h.state.environment.planar.receiver = doc::NodeId{999U};
  h.state.environment.baked.reflections[0].enabled = true;
  h.environment_section(3);
  UI_CHECK(h.state.environment.planar.receiver == doc::NodeId{999U});
  const auto receiver = h.choose(242.0, 2, 1);
  UI_CHECK(receiver.project.apply);
  UI_CHECK(receiver.project.apply->environment.planar.receiver == floor.node);
  const auto probe = h.choose(530.0, 0, 1);
  UI_CHECK(probe.project.apply);
  UI_CHECK(probe.project.apply->environment.baked.reflections[0].source == floor.node);
}

void project_settings_cancel_discards_the_draft() {
  ProjectSettingsHarness harness;
  harness.state.settings = scene::EffectSettings{};
  harness.panel.open(harness.state.settings, harness.state.renderer);

  static_cast<void>(harness.click(ProjectSettingsHarness::inner_frustum_checkbox()));
  const auto cancelled = harness.click(ProjectSettingsHarness::cancel_button());
  UI_CHECK(!cancelled.apply.has_value());
  UI_CHECK(!harness.panel.is_open());

  // Reopening seeds from the project again, so the discarded edit is gone.
  harness.panel.open(harness.state.settings, harness.state.renderer);
  const auto reapplied = harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(!reapplied.apply.has_value());
}

void project_settings_defaults_restores_the_built_in_values() {
  ProjectSettingsHarness h;h.state.renderer.icvfx.latency_frames=4;
  h.state.renderer.bloom_strength=0.75F;h.state.environment.fog_density=0.2F;
  h.panel.open(h.state.settings,h.state.renderer,{},h.state.environment);
  static_cast<void>(h.click(ProjectSettingsHarness::defaults_button()));
  auto result=h.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(result.apply && result.apply->renderer.icvfx==scene::IcvfxSettings{});
  UI_CHECK(result.apply->renderer.bloom_strength==0.75F && result.apply->environment.fog_density==0.2F);
}

void advanced_renderer_required_is_an_explicit_project_edit() {
  LiveSettingsHarness h;
  auto backend=h.choose(90.0,0,1);UI_CHECK(backend.project.apply && h.state.renderer.backend==scene::RendererBackend::Advanced);
  auto required=h.click(122.0);UI_CHECK(required.project.apply && h.state.renderer.required);
  static_cast<void>(h.choose(154.0,1,2));UI_CHECK(h.state.renderer.quality==scene::RendererQuality::Cinematic);
}

void raster_only_clears_only_unavailable_future_pass_requests() {
  LiveSettingsHarness h;h.state.renderer.backend=scene::RendererBackend::Advanced;
  h.state.renderer.ray_tracing=true;h.state.renderer.ddgi=true;h.state.renderer.restir=true;h.state.renderer.denoising=true;
  h.state.renderer.taau=true;h.state.renderer.motion_blur=true;
  auto result=h.click(218.0);UI_CHECK(result.project.apply);
  UI_CHECK(!h.state.renderer.ray_tracing && !h.state.renderer.ddgi && !h.state.renderer.restir && !h.state.renderer.denoising);
  UI_CHECK(h.state.renderer.taau && h.state.renderer.motion_blur);
}

// Occlusion is authored as a radius, and zero is the off.
//
// A toggle beside a value would be two ways to say the same thing, and the
// renderer only reads one of them - so the panel offers exactly the control
// the renderer obeys.
void occlusion_is_authored_as_a_radius() {
  LiveSettingsHarness h;h.advanced_section(1);
  auto result=h.enter(258.0,"2.5");UI_CHECK(result.project.apply && h.state.renderer.occlusion_radius==2.5F);
  static_cast<void>(h.enter(258.0,"0"));UI_CHECK(h.state.renderer.occlusion_radius==0.0F);
}

// Antialiasing is one choice, authored, with the jitter period behind it.
//
// The panel stores two booleans and the renderer reads one decision, so the
// thing worth asserting is that they cannot disagree: no click can leave both
// set, and what the buttons show is what `antialiasing_for` returns.
void antialiasing_is_one_authored_choice() {
  LiveSettingsHarness h;h.advanced_section(1);
  static_cast<void>(h.choose(130.0,0,2));UI_CHECK(h.state.renderer.taau && !h.state.renderer.spatial_antialiasing);
  static_cast<void>(h.enter(162.0,"33"));UI_CHECK(h.state.renderer.taa_sequence_length==33);
  static_cast<void>(h.choose(130.0,2,1));UI_CHECK(!h.state.renderer.taau && h.state.renderer.spatial_antialiasing);
  static_cast<void>(h.choose(130.0,1,0));UI_CHECK(scene::antialiasing_for(h.state.renderer)==scene::AntialiasingMode::None);
}

void geometry_controls_emit_replicated_settings() {
  LiveSettingsHarness h;h.advanced_section(4);
  static_cast<void>(h.click(250.0));static_cast<void>(h.click(282.0));static_cast<void>(h.click(314.0));
  UI_CHECK(!h.state.renderer.frustum_culling && !h.state.renderer.geometry_instancing && !h.state.renderer.model_lod);
}

void object_motion_controls_apply_and_cancel() {
  LiveSettingsHarness h;h.advanced_section(1);
  static_cast<void>(h.click(162.0));UI_CHECK(h.state.renderer.motion_blur);
  static_cast<void>(h.enter(194.0,"16"));UI_CHECK(h.state.renderer.motion_blur_radius==16);
  static_cast<void>(h.click(194.0));h.input.new_frame();h.input.add_text("9");h.ctx.new_frame();static_cast<void>(h.draw());
  static_cast<void>(h.key(gui::Key::Escape));UI_CHECK(h.state.renderer.motion_blur_radius==16);
}

void plate_dof_controls_apply_and_cancel() {
  LiveSettingsHarness h;h.advanced_section(1);
  static_cast<void>(h.click(194.0));UI_CHECK(h.state.renderer.plate_depth_of_field);
  static_cast<void>(h.enter(226.0,"14"));UI_CHECK(h.state.renderer.plate_dof_radius==14);
  static_cast<void>(h.click(194.0));UI_CHECK(!h.state.renderer.plate_depth_of_field);
}

void texture_sampling_controls_apply_and_cancel() {
  LiveSettingsHarness h;h.advanced_section(4);
  static_cast<void>(h.choose(442.0,0,3));UI_CHECK(h.state.renderer.texture_anisotropy==8);
  static_cast<void>(h.enter(474.0,"-0.75"));UI_CHECK(h.state.renderer.texture_lod_bias==-0.75F);
}

void texture_streaming_controls_emit_authored_policy() {
  LiveSettingsHarness h;h.advanced_section(4);
  static_cast<void>(h.enter(378.0,"256"));static_cast<void>(h.enter(410.0,"2"));
  UI_CHECK(h.state.renderer.texture_budget_mib==256 && h.state.renderer.texture_mip_bias==2);
  // The field stops at the renderer's envelope rather than accepting a value it would cut.
  static_cast<void>(h.enter(378.0,"4096"));UI_CHECK(h.state.renderer.texture_budget_mib==1024);
  static_cast<void>(h.click(346.0));UI_CHECK(!h.state.renderer.texture_streaming);
}

void shadow_cascades_and_bounce_are_authored_beside_the_backend() {
  LiveSettingsHarness h;h.advanced_section(3);
  static_cast<void>(h.enter(210.0,"3"));UI_CHECK(h.state.renderer.shadow_cascades==3);
  static_cast<void>(h.enter(210.0,"0"));UI_CHECK(h.state.renderer.shadow_cascades==0);
}

void calibrated_lens_controls_author_their_profile() {
  ProjectSettingsHarness harness;
  harness.state.renderer.backend = scene::RendererBackend::Advanced;
  harness.state.camera_node = doc::NodeId{99U};
  harness.panel.open(harness.state.settings, harness.state.renderer,
                     harness.state.lens_profile);

  static_cast<void>(harness.click(ProjectSettingsHarness::lens_tab()));
  static_cast<void>(
      harness.click(ProjectSettingsHarness::calibrated_lens_button()));
  static_cast<void>(
      harness.click(ProjectSettingsHarness::lens_sensor_width_track()));
  const auto applied = harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(applied.apply.has_value());
  UI_CHECK(applied.apply->lens_profile.has_value());
  UI_CHECK(applied.apply->lens_profile->sensor_width_mm > 90.0F);
  UI_CHECK(applied.apply->lens_profile->sensor_width_mm < 110.0F);
}

// The page authors the show-wide plate policy. Where a tracker listens is not
// here: that is a property of one feed, authored on the feed's own node beside
// the camera it drives.
void the_tracking_category_authors_the_inner_frustum_policy() {
  ProjectSettingsHarness harness;
  harness.state.renderer.icvfx.camera_defaults.margin_px = 32U;
  harness.state.renderer.icvfx.latency_frames = 3U;
  harness.state.renderer.icvfx.prediction_window = 9U;
  harness.state.renderer.icvfx.jitter_filter_percent = 63U;
  harness.panel.open(harness.state.settings, harness.state.renderer,
                     harness.state.lens_profile);

  static_cast<void>(harness.click(ProjectSettingsHarness::tracking_tab()));
  static_cast<void>(
      harness.click(ProjectSettingsHarness::inner_frustum_checkbox()));
  const gui::Rect predict = ProjectSettingsHarness::predict_checkbox();
  static_cast<void>(harness.click(predict));
  const auto applied = harness.click(ProjectSettingsHarness::apply_button());
  UI_CHECK(applied.apply.has_value());
  UI_CHECK(applied.apply->renderer.icvfx.inner_frustum);
  UI_CHECK(applied.apply->renderer.icvfx.camera_defaults.margin_px == 32U);
  UI_CHECK(applied.apply->renderer.icvfx.latency_frames == 3U);
  UI_CHECK(applied.apply->renderer.icvfx.pose_prediction);
  UI_CHECK(applied.apply->renderer.icvfx.prediction_window == 9U);
  UI_CHECK(applied.apply->renderer.icvfx.jitter_filter_percent == 63U);
  // Cancel is a draft-only operation, including prediction settings.
  harness.panel.open(harness.state.settings, applied.apply->renderer);
  static_cast<void>(harness.click(ProjectSettingsHarness::tracking_tab()));
  static_cast<void>(harness.click(predict));
  const auto canceled = harness.click(ProjectSettingsHarness::cancel_button());
  UI_CHECK(!canceled.apply);
}

struct Case final {
  const char *name;
  void (*function)();
};

// Map edits are immediate and preserve the other scene environment values.
void the_environment_category_authors_the_lighting_map() {
  LiveSettingsHarness h;h.target=editor::SceneSettingsTarget::Environment;h.state.renderer.backend=scene::RendererBackend::Advanced;
  static_cast<void>(h.click(154.0));h.input.new_frame();h.input.add_text("studio.hdr");h.ctx.new_frame();auto result=h.draw();
  UI_CHECK(result.project.apply && h.state.environment.map_source=="studio.hdr");
  static_cast<void>(h.key(gui::Key::Enter));static_cast<void>(h.enter(186.0,"2.5"));static_cast<void>(h.enter(218.0,"0.25"));
  UI_CHECK(h.state.environment.map_intensity==2.5F && h.state.environment.map_rotation==0.25F);
}

// Basic draws none of the environment page, so it shows why and a way out
// instead of fields; a physical sky left on stays reachable to turn off.
void basic_renderer_hides_the_environment_fields() {
  LiveSettingsHarness h;h.target=editor::SceneSettingsTarget::Environment;
  h.state.environment.map_source="studio.hdr";
  h.state.environment.fog_density=0.25F;
  // Where Advanced has the physical sky switch, Basic has nothing to toggle.
  UI_CHECK(!h.click(90.0).project.apply && !h.state.environment.physical_sky.enabled);
  h.state.environment.physical_sky.enabled=true;
  bool switched=false,turned_off=false;
  for(float y=44.0F;y<700.0F;y+=6.0F) {
    const auto frame=h.click(y);
    UI_CHECK(!frame.file_picker && frame.keys.empty());
    if(!frame.project.apply) continue;
    if(h.state.renderer.backend==scene::RendererBackend::Advanced) {
      switched=true;h.state.renderer.backend=scene::RendererBackend::Basic;
    } else {
      UI_CHECK(!h.state.environment.physical_sky.enabled);turned_off=true;
    }
    // The hidden values are kept for when Advanced returns.
    UI_CHECK(h.state.environment.map_source=="studio.hdr" && h.state.environment.fog_density==0.25F);
  }
  UI_CHECK(switched && turned_off);
  // On Advanced the same page offers the sky switch again.
  h.state.renderer.backend=scene::RendererBackend::Advanced;
  static_cast<void>(h.click(90.0));
  UI_CHECK(h.state.environment.physical_sky.enabled);
}

// Physical sky values use the same property key controls as fog and maps.
void physical_sky_controls_apply_and_author_a_time_key() {
  LiveSettingsHarness h;h.target=editor::SceneSettingsTarget::Environment;h.state.renderer.backend=scene::RendererBackend::Advanced;
  static_cast<void>(h.click(90.0));UI_CHECK(h.state.environment.physical_sky.enabled);
  static_cast<void>(h.enter(122.0,"16.5"));
  auto result=h.click(122.0,306);UI_CHECK(result.keys.size()==1 && result.keys[0].channel==scene::PropertyChannel::SkyTimeOfDay && result.keys[0].value==16.5);
  UI_CHECK(!result.keys[0].erase);
}

void environment_keys_preserve_interpolation_and_authored_base() {
  LiveSettingsHarness h;h.environment_section(1);
  scene::ChannelKeyframes channel{};channel.target=h.state.stage_node;channel.component=scene::PropertyChannel::FogDensity;
  channel.frames={0,60};channel.values={0.0,0.6};h.keys={channel};h.time=30;
  auto key=h.click(130.0,306);UI_CHECK(key.keys.size()==1 && std::abs(key.keys[0].value-0.3)<1e-6 && !key.keys[0].erase);
  h.time=60;auto erased=h.click(130.0,306);UI_CHECK(erased.keys.size()==1 && erased.keys[0].erase);
  h.time=30;auto edited=h.enter(130.0,"0.4");UI_CHECK(edited.keys.size()==1 && !edited.project.apply);
  UI_CHECK(h.state.environment.fog_density==0.0F);
}

// An environment key added between weighted Cubic keys keeps the value
// playback shows there, tangents and weights included.
void environment_keys_sample_weighted_cubic_tangents() {
  LiveSettingsHarness h;h.environment_section(1);
  scene::ChannelKeyframes channel{};channel.target=h.state.stage_node;channel.component=scene::PropertyChannel::FogDensity;
  channel.frames={0,32};channel.values={0.1,0.2};
  channel.interpolations={scene::KeyInterpolation::Cubic,scene::KeyInterpolation::Cubic};
  channel.tangents={scene::KeyTangent{scene::KeyTangentMode::Broken,0.0,0.02,{},4.0},
                    scene::KeyTangent{scene::KeyTangentMode::Broken,-0.005,0.0,12.0,{}}};
  h.keys={channel};h.time=13;
  auto key=h.click(130.0,306);
  UI_CHECK(key.keys.size()==1 && std::abs(key.keys[0].value-0.2025)<1e-6 && !key.keys[0].erase);
}

// Small physical-lighting scales retain their precision in numeric fields.
void photometric_scale_accepts_precise_numeric_input() {
  LiveSettingsHarness h;h.advanced_section(1);
  auto result=h.enter(322.0,"0.0062831853");UI_CHECK(result.project.apply);
  UI_CHECK(h.state.renderer.photometric_scale==0.0062831853F);
}

void colour_is_authored_from_its_own_tab() {
  LiveSettingsHarness h;h.advanced_section(2);
  static_cast<void>(h.choose(170.0,1,0));static_cast<void>(h.click(266.0));
  UI_CHECK(h.state.renderer.color.display_transform==scene::DisplayTransform::Srgb && !h.state.renderer.color.dither);
  static_cast<void>(h.choose(170.0,0,2));UI_CHECK(h.state.renderer.color.display_transform==scene::DisplayTransform::Aces13);
  UI_CHECK(!h.state.renderer.color.dither && h.state.renderer.backend==scene::RendererBackend::Advanced);
}

// Every way the authored colour can fail to reach the wall has its own words,
// and the residency states map onto them without a gap.
void the_colour_status_names_each_reason() {
  UI_CHECK(editor::color_status_text(editor::ColorStatus::Loading) !=
           editor::color_status_text(editor::ColorStatus::Ready));
  UI_CHECK(editor::color_status_text(editor::ColorStatus::Failed) !=
           editor::color_status_text(editor::ColorStatus::Ready));
  UI_CHECK(editor::color_status_text(editor::ColorStatus::BackendUnsupported)
               .find("Basic") != std::string_view::npos);
  UI_CHECK(editor::color_status_text(editor::ColorStatus::Failed)
               .find("cube") != std::string_view::npos);
  UI_CHECK(editor::color_status_for(render::DisplayLutState::Ready) ==
           editor::ColorStatus::Ready);
  UI_CHECK(editor::color_status_for(render::DisplayLutState::Failed) ==
           editor::ColorStatus::Failed);
  UI_CHECK(editor::color_status_for(render::DisplayLutState::Pending) ==
           editor::ColorStatus::Loading);
  UI_CHECK(editor::color_status_for(render::DisplayLutState::Unseen) ==
           editor::ColorStatus::Loading);
  UI_CHECK(editor::color_status_for(render::DisplayLutState::Busy) ==
           editor::ColorStatus::Loading);
}

void the_environment_status_names_each_reason() {
  UI_CHECK(editor::environment_status_text(
               editor::EnvironmentStatus::NotAuthored)
           != editor::environment_status_text(
               editor::EnvironmentStatus::Failed));
  UI_CHECK(editor::environment_status_text(editor::EnvironmentStatus::Failed)
           != editor::environment_status_text(
               editor::EnvironmentStatus::BackendUnsupported));
  UI_CHECK(editor::environment_status_text(editor::EnvironmentStatus::Loading)
           != editor::environment_status_text(
               editor::EnvironmentStatus::Ready));
  // The two that need an operator to do something say so; the two that are
  // working or merely waiting do not.
  UI_CHECK(editor::environment_status_text(
                   editor::EnvironmentStatus::BackendUnsupported)
                   .find("Advanced") != std::string_view::npos);
  UI_CHECK(editor::environment_status_text(editor::EnvironmentStatus::Failed)
                   .find("EXR") != std::string_view::npos);
  // A project with nowhere to store the map is its own reason: the field
  // simply reverts, which looks exactly like never having typed one.
  UI_CHECK(editor::environment_status_text(editor::EnvironmentStatus::NoStage)
           != editor::environment_status_text(
               editor::EnvironmentStatus::NotAuthored));
  for (const auto status :
       {editor::EnvironmentStatus::NotAuthored,
        editor::EnvironmentStatus::Loading, editor::EnvironmentStatus::Failed,
        editor::EnvironmentStatus::BackendUnsupported,
        editor::EnvironmentStatus::NoStage,
        editor::EnvironmentStatus::Ready}) {
    UI_CHECK(!editor::environment_status_text(status).empty());
  }
}

void stage_status_names_what_particle_effects_are_waiting_for() {
  const auto status = [](auto set) {
    mediaserver::render::RenderStats geometry{};
    geometry.assets.ready = false;
    set(geometry);
    return editor::stage_asset_status(geometry, 0U, mediaserver::render::DisplayLutState::Ready);
  };
  // Refusals are failures with what to do about them.
  auto shown = status([](auto& g) { g.particles_unsupported = 1U; });
  UI_CHECK(!shown.loading && shown.headline == "STAGE ASSET LOAD FAILED");
  UI_CHECK(shown.detail.find("Advanced renderer") != std::string::npos);
  shown = status([](auto& g) { g.particles_failed = 2U; });
  UI_CHECK(!shown.loading && shown.detail.find("Rescan in Library") != std::string::npos);
  // Loading and catching up are waits.
  shown = status([](auto& g) { g.particles_pending = 3U; });
  UI_CHECK(shown.loading && shown.detail == "Loading particle effects: 3 pending.");
  shown = status([](auto& g) { g.particles_behind = 1U; });
  UI_CHECK(shown.loading && shown.detail.find("catching up") != std::string::npos);
}

void program_publication_status_reports_the_actual_wait() {
  editor::OutputMonitorState::Cluster cluster{};
  core::AssetSetReadiness local{19, false, 0};
  auto status = editor::program_asset_status(local, {}, 7, true, cluster, "Loading baked lighting");
  UI_CHECK(status.waiting && status.detail == "Loading baked lighting");
  local.ready = true;
  cluster.asset_cohort.observe(&local);
  cluster.asset_cohort.observe(nullptr);
  cluster.asset_waiting_node = 3;
  status = editor::program_asset_status(local, {}, 7, true, cluster, {});
  UI_CHECK(status.headline == "WAITING FOR OUTPUT NODE 3");
  UI_CHECK(status.detail.starts_with("1/2 renderers ready"));
  cluster.asset_waiting_node.reset();
  status = editor::program_asset_status(local, {19, 10, 42}, 7, true, cluster, {});
  UI_CHECK(status.detail.find("3 control frames") != std::string::npos);
  UI_CHECK(status.routine);
  status = editor::program_asset_status(local, {19, 10, 42}, 11, true, cluster, {});
  UI_CHECK(status.headline == "PROGRAM ACTIVATION MISSED");
  UI_CHECK(status.detail.find("retried") != std::string::npos);
  // A missed activation is the only wait the monitor raises at once.
  UI_CHECK(!status.routine);
  local.visible_commit = 10;
  status = editor::program_asset_status(local, {19, 10, 42}, 11, true, cluster, {});
  UI_CHECK(status.headline == "CONFIRMING OUTPUT UPDATE" && status.routine);
  const auto expired = 10 + core::asset_commit_confirmation_frames;
  UI_CHECK(editor::program_asset_status(local, {19, 10, 42}, expired - 1, true, cluster, {}).headline == "CONFIRMING OUTPUT UPDATE");
  status = editor::program_asset_status(local, {19, 10, 42}, expired, true, cluster, {});
  UI_CHECK(status.waiting && status.headline == "OUTPUT ACTIVATION MISSED");
  UI_CHECK(status.detail.find("retried") != std::string::npos);
  UI_CHECK(editor::program_asset_status(local, {19, 10, 42}, expired + 10000, true, cluster, {}).headline == "OUTPUT ACTIVATION MISSED");
  status = editor::program_asset_status(local, {19, expired + 31, 42}, expired + 1, true, cluster, {});
  UI_CHECK(status.headline == "RETRYING OUTPUT UPDATE" && !status.routine);
  UI_CHECK(status.detail.find("30 control frames") != std::string::npos);
  cluster.asset_waiting_node = 3;
  UI_CHECK(editor::program_asset_status(local, {19, 10, 42}, expired, true, cluster, {}).headline == "WAITING FOR OUTPUT NODE 3");
  cluster.asset_waiting_node.reset();
  UI_CHECK(editor::program_asset_status(local, {}, expired, true, cluster, {}).headline == "WAITING TO UPDATE PROGRAM");
  cluster.asset_cohort.visible = true;
  UI_CHECK(!editor::program_asset_status(local, {19, 10, 42}, 11, true, cluster, {}).waiting);
}

void shadow_stability_controls_apply_and_reopen() {
  LiveSettingsHarness h;h.advanced_section(3);
  static_cast<void>(h.enter(242.0,"0.2"));static_cast<void>(h.enter(274.0,"3.5"));static_cast<void>(h.enter(306.0,"0.4"));static_cast<void>(h.enter(338.0,"0.05"));
  UI_CHECK(h.state.renderer.shadow_blend_band==0.2F && h.state.renderer.shadow_filter_radius==3.5F);
  UI_CHECK(h.state.renderer.contact_shadow_length==0.4F && h.state.renderer.contact_shadow_thickness==0.05F);
}

void linked_tree_preserves_hierarchy_collapse_and_range() {
  SceneHarness h; h.items.clear();
  editor::ElementInfo collection; collection.node=doc::NodeId{500}; collection.linked=true;
  collection.linked_collection=true; collection.linked_instance=collection.node; collection.kind=editor::ElementKind::Group;
  collection.display_name="Set";
  auto parent=collection; parent.node=doc::NodeId{502}; parent.linked_collection=false; parent.linked_parent=collection.node; parent.display_name="Parent";
  auto child=parent; child.node=doc::NodeId{501}; child.linked_parent=parent.node; child.display_name="Prop";
  h.items={child,collection,parent};
  // Collection, its object, and the object's part: rows 0, 1 and 2.
  static_cast<void>(h.frame(80,scene_row(2),false)); static_cast<void>(h.frame(80,scene_row(2),true,gui::KeyMods::Shift));
  auto selected=h.frame(80,scene_row(2),false,gui::KeyMods::Shift);
  UI_CHECK(selected && selected->node==child.node);
  UI_CHECK(selected->visible_order==std::vector<doc::NodeId>{collection.node,parent.node,child.node});
  // The collection's twirl (x=28..44) closes it without selecting anything.
  static_cast<void>(h.frame(36,scene_row(0),true)); UI_CHECK(!h.frame(36,scene_row(0),false));
  static_cast<void>(h.frame(80,scene_row(2),false));
  selected=h.frame(80,scene_row(2),true);
  UI_CHECK(selected && selected->action==editor::SelectionAction::Clear);
  static_cast<void>(h.frame(80,scene_row(0),false)); static_cast<void>(h.frame(80,scene_row(0),true));
  selected=h.frame(80,scene_row(0),false); UI_CHECK(selected && selected->node==collection.node);
}

constexpr std::array<Case, 138U> cases{{
    {"a scene row renames in place", &a_scene_row_renames_in_place},
    {"a scene eye hides and shows", &a_scene_eye_hides_and_shows},
    {"scene rows drag into groups and between siblings",
     &scene_rows_drag_into_groups_and_between_siblings},
    {"linked tree preserves hierarchy collapse and range", &linked_tree_preserves_hierarchy_collapse_and_range},
    {"reflection targets choose scene objects without truncating IDs",
     &reflection_targets_choose_scene_objects_without_truncating_ids},
    {"object motion controls apply and cancel", &object_motion_controls_apply_and_cancel},
    {"plate depth of field controls apply and cancel", &plate_dof_controls_apply_and_cancel},
    {"shadow stability controls apply and reopen", &shadow_stability_controls_apply_and_reopen},
    {"photometric scale accepts precise numeric input", &photometric_scale_accepts_precise_numeric_input},
    {"program publication reports the actual wait", &program_publication_status_reports_the_actual_wait},
    {"Stage status names what particle effects are waiting for", &stage_status_names_what_particle_effects_are_waiting_for},
    {"baked lighting page actions and authored field", &baked_lighting_page_starts_cancels_and_preserves_the_authored_field},
    {"the environment status names each reason",
     &the_environment_status_names_each_reason},
    {"colour is authored from its own tab",
     &colour_is_authored_from_its_own_tab},
    {"the colour status names each reason",
     &the_colour_status_names_each_reason},
    {"the environment category authors the lighting map",
     &the_environment_category_authors_the_lighting_map},
    {"physical sky controls apply and author time keys", &physical_sky_controls_apply_and_author_a_time_key},
    {"environment keys preserve interpolation and authored base",
     &environment_keys_preserve_interpolation_and_authored_base},
    {"environment keys sample weighted cubic tangents",
     &environment_keys_sample_weighted_cubic_tangents},
    {"a scene row reports the clicked node",
     &a_scene_row_reports_the_clicked_node},
    {"LED surfaces list after the scene with a lock",
     &led_surfaces_list_after_the_scene_with_a_lock},
    {"a Ctrl scene click requests an exact selection toggle",
     &a_ctrl_scene_click_requests_an_exact_selection_toggle},
    {"a Shift scene click carries the visible element order",
     &a_shift_scene_click_carries_the_visible_element_order},
    {"bare Scene list space requests a clear",
     &bare_scene_list_space_requests_a_clear},
    {"a locked scene row only offers its unlock control",
     &a_locked_scene_row_only_offers_its_unlock_control},
    {"scene search filters the hierarchy", &scene_search_filters_the_hierarchy},
    {"a Scene group is visible selectable and uses its authored name",
     &a_scene_group_is_visible_selectable_and_uses_its_authored_name},
    {"Scene Group and Ungroup live in the row menu",
     &scene_group_and_ungroup_live_in_the_row_menu},
    {"a Scene group menu creates empty groups inside it",
     &a_scene_group_menu_creates_empty_groups_inside_it},
    {"Scene empty space menu creates top level groups",
     &scene_empty_space_menu_creates_top_level_groups},
    {"a Scene row menu picks a highlight color",
     &a_scene_row_menu_picks_a_highlight_color},
    {"an effect drag reports its graph drop position without a selection",
     &an_effect_drag_reports_its_graph_drop_position_without_a_selection},
    {"clicking an effect does not apply it",
     &clicking_an_effect_does_not_apply_it},
    {"effect search filters before dragging",
     &effect_search_filters_before_dragging},
    {"effect drops apply only over Stage or Inspector",
     &effect_drops_apply_only_over_stage_or_inspector},
    {"an effect drop target strengthens under the pointer",
     &an_effect_drop_target_strengthens_under_the_pointer},
    {"Compositor Scene Layers show the referenced element name",
     &compositor_scene_layers_show_the_referenced_element_name},
    {"compositor toolbar reports effect add intents",
     &compositor_toolbar_reports_effect_add_intents},
    {"compositor output creation is visible and unique",
     &compositor_output_creation_is_visible_and_unique},
    {"selected compositor nodes and edges return delete intents",
     &selected_compositor_nodes_and_edges_return_delete_intents},
    {"LED Car Process returns an ordinary node delete intent",
     &led_car_process_returns_an_ordinary_node_delete_intent},
    {"the required Output node cannot be deleted",
     &the_required_output_node_cannot_be_deleted},
    {"project replacement clears compositor document state",
     &project_replacement_clears_compositor_document_state},
    {"Compositor navigation zooms under the pointer and pans the graph",
     &compositor_navigation_zooms_under_the_pointer_and_pans_the_graph},
    {"zoomed Compositor node dragging still authors graph coordinates",
     &zoomed_compositor_node_dragging_still_authors_graph_coordinates},
    {"dragging a compositor node reports its authored position on release",
     &dragging_a_compositor_node_reports_its_authored_position_on_release},
    {"compatible typed ports report a connection intent",
     &compatible_typed_ports_report_a_connection_intent},
    {"Compositor replacement picks the topmost node and clips feedback",
     &compositor_replacement_picks_topmost_and_clips_feedback},
    {"a released node holds its position until the graph reports it",
     &a_released_node_holds_its_position_until_the_graph_reports_it},
    {"wires are picked along their curve",
     &wires_are_picked_along_their_curve},
    {"a wire snaps to a compatible port near the pointer",
     &a_wire_snaps_to_a_compatible_port_near_the_pointer},
    {"an input port's target covers its whole row",
     &an_input_ports_target_covers_its_whole_row},
    {"the Parameters panel resizes from its edge",
     &the_parameters_panel_resizes_from_its_edge},
    {"long parameter lists scroll to their last field",
     &long_parameter_lists_scroll_to_their_last_field},
    {"parameter sections collapse",
     &parameter_sections_collapse},
    {"node text fits unclipped in a real font",
     &node_text_fits_unclipped_in_a_real_font},
    {"a wire let go on empty canvas offers nodes that take it",
     &a_wire_let_go_on_empty_canvas_offers_nodes_that_take_it},
    {"Tab opens the node search at the pointer",
     &tab_opens_the_node_search_at_the_pointer},
    {"Alt-click breaks wires", &alt_click_breaks_wires},
    {"Ctrl-drag moves wires between ports",
     &ctrl_drag_moves_wires_between_ports},
    {"a free node dropped on a wire splices into it",
     &a_free_node_dropped_on_a_wire_splices_into_it},
    {"reroute points are added, moved and removed",
     &reroute_points_are_added_moved_and_removed},
    {"a wire that would make a loop is refused",
     &a_wire_that_would_make_a_loop_is_refused},
    {"drops from other panels find compatible inputs",
     &drops_from_other_panels_find_compatible_inputs},
    {"a new wire is held until the graph reports it",
     &a_new_wire_is_held_until_the_graph_reports_it},
    {"ctrl and shift clicks build a selection",
     &ctrl_and_shift_clicks_build_a_selection},
    {"a rubber band selects nodes",
     &a_rubber_band_selects_nodes},
    {"a selection moves and nudges together",
     &a_selection_moves_and_nudges_together},
    {"delete takes every selected node but output",
     &delete_takes_every_selected_node_but_output},
    {"copy paste and duplicate ask the director",
     &copy_paste_and_duplicate_ask_the_director},
    {"framing brings nodes into view",
     &framing_brings_nodes_into_view},
    {"layout tools place nodes",
     &layout_tools_place_nodes},
    {"a wires menu disconnects and routes",
     &a_wires_menu_disconnects_and_routes},
    {"comment boxes carry their nodes",
     &comment_boxes_carry_their_nodes},
    {"node titles are renamed in place",
     &node_titles_are_renamed_in_place},
    {"the minimap recentres the view",
     &the_minimap_recentres_the_view},
    {"dropping on a single input reports an atomic rewire",
     &dropping_on_a_single_input_reports_an_atomic_rewire},
    {"incompatible ports are rejected and persisted edges draw wires",
     &incompatible_ports_are_rejected_and_persisted_edges_draw_wires},
    {"compositor diagnostics are explained in plain language",
     &compositor_diagnostics_are_explained_in_plain_language},
    {"inline node values edit like Parameters",
     &inline_node_values_edit_like_parameters},
    {"a collapsed node keeps only its wired ports",
     &a_collapsed_node_keeps_only_its_wired_ports},
    {"an effect wire drop goes after that wire's effect",
     &an_effect_wire_drop_goes_after_that_wires_effect},
    {"revealing an element brings its Scene Layer into view",
     &revealing_an_element_brings_its_scene_layer_into_view},
    {"bypass toggles the selection's bypassable nodes",
     &bypass_toggles_the_selections_bypassable_nodes},
    {"keyer and Matte parameters emit typed edits",
     &keyer_and_matte_parameters_emit_typed_edits},
    {"Merge and Background parameters emit typed edits",
     &merge_and_background_parameters_emit_typed_edits},
    {"an input's wires are reordered by menu and by number",
     &an_inputs_wires_are_reordered_by_menu_and_by_number},
    {"brightness parameter typing emits one typed commit",
     &brightness_parameter_typing_emits_one_typed_commit},
    {"compositor parameter nudges report full semantic identity",
     &compositor_parameter_nudges_report_full_semantic_identity},
    {"Saturation parameter commits are clamped to four",
     &saturation_parameter_commits_are_clamped_to_four},
    {"Levels parameter typing emits the exact channel property",
     &levels_parameter_typing_emits_the_exact_channel_property},
    {"Lift Gamma Gain fields emit typed clamped properties",
     &lift_gamma_gain_fields_emit_typed_clamped_properties},
    {"Contrast Pivot fields emit typed clamped properties",
     &contrast_pivot_fields_emit_typed_clamped_properties},
    {"Color Matrix fields emit exact typed properties",
     &color_matrix_fields_emit_exact_typed_properties},
    {"White Balance fields emit typed clamped properties",
     &white_balance_fields_emit_typed_clamped_properties},
    {"Curves add and last point edit emit one typed program",
     &curves_add_and_last_point_edit_emit_one_typed_program},
    {"Film Grain fields preserve exact integer and mode types",
     &film_grain_fields_preserve_exact_integer_and_mode_types},
    {"blur parameter commits are clamped to the model contract",
     &blur_parameter_commits_are_clamped_to_the_model_contract},
    {"radial array parameters emit count radius angle and center types",
     &radial_array_parameters_emit_count_radius_angle_and_center_types},
    {"workspace panels draw scene relevant content",
     &workspace_panels_draw_scene_relevant_content},
    {"conformance control", &output_monitor_requests_conformance_without_a_lifecycle_action},
        {"output monitor emits fixed lifecycle intent",
     &output_monitor_emits_one_fixed_lifecycle_intent},
    {"duplicate node indices disable lifecycle controls",
     &duplicate_node_indices_have_no_ambiguous_controls},
    {"missing cluster assets block start but keep status visible",
     &missing_cluster_assets_block_start_but_keep_status_visible},
    {"running process needs an editor connection", &process_running_is_distinct_from_editor_control},
    {"content labels distinguish missing stale and verification failures",
     &content_labels_distinguish_missing_stale_and_verification_failures},
    {"output failures are named and distinct from asset failures",
     &output_failures_are_named_and_distinct_from_asset_failures},
    {"output monitor scroll reaches later cluster nodes",
     &output_monitor_scroll_reaches_later_cluster_nodes},
    {"output monitor emits explicit cluster relaunch",
     &output_monitor_emits_explicit_cluster_relaunch},
    {"output monitor stop all asks before stopping",
     &output_monitor_stop_all_asks_before_stopping},
    {"output monitor opens a node log and refreshes it",
     &output_monitor_opens_a_node_log_and_refreshes_it},
    {"output monitor cluster health keeps node controls reachable",
     &output_monitor_cluster_health_keeps_node_controls_reachable},
    {"project settings apply reports the edited values",
     &project_settings_apply_reports_the_edited_values},
    {"scene file fields request matching pickers without editing",
     &scene_file_fields_request_the_matching_picker_without_editing},
    {"project settings cancel discards the draft",
     &project_settings_cancel_discards_the_draft},
    {"project settings defaults restores the built-in values",
     &project_settings_defaults_restores_the_built_in_values},
    {"advanced renderer required is an explicit project edit",
     &advanced_renderer_required_is_an_explicit_project_edit},
    {"raster only clears only unavailable future pass requests",
     &raster_only_clears_only_unavailable_future_pass_requests},
    {"texture sampling controls apply and cancel", &texture_sampling_controls_apply_and_cancel},
    {"texture streaming controls emit authored policy", &texture_streaming_controls_emit_authored_policy},
    {"geometry controls emit replicated settings", &geometry_controls_emit_replicated_settings},
    {"basic renderer hides the environment fields",
     &basic_renderer_hides_the_environment_fields},
    {"shadow cascades and bounce are authored beside the backend",
     &shadow_cascades_and_bounce_are_authored_beside_the_backend},
    {"antialiasing is one authored choice",
     &antialiasing_is_one_authored_choice},
    {"occlusion is authored as a radius",
     &occlusion_is_authored_as_a_radius},
    {"calibrated lens controls author their profile",
     &calibrated_lens_controls_author_their_profile},
    {"the tracking category authors the inner frustum policy",
     &the_tracking_category_authors_the_inner_frustum_policy},
    {"cancel closes the preferences dialog",
     &cancel_closes_the_preferences_dialog},
    {"a render node password is typed cleared and saved",
     &a_render_node_password_is_typed_cleared_and_saved},
    {"cancel discards edits so reopening shows the stored values",
     &cancel_discards_edits_so_reopening_shows_the_stored_values},
    {"save reports the edited values and closes",
     &save_reports_the_edited_values_and_closes},
    {"save stays disabled until something actually changes",
     &save_stays_disabled_until_something_actually_changes},
    {"proxy storage mode is part of the saved draft",
     &proxy_storage_mode_is_part_of_the_saved_draft},
    {"clear proxy cache reports without saving or closing",
     &clear_proxy_cache_reports_without_saving_or_closing},
    {"playback preset is part of the saved machine draft",
     &playback_preset_is_part_of_the_saved_machine_draft},
    {"preferences scroll stops at the end of the form",
     &preferences_scroll_stops_at_the_end_of_the_form},
    {"transcode priority is part of the saved machine draft",
     &transcode_priority_is_part_of_the_saved_machine_draft},
    {"only a click outside the card dismisses it",
     &only_a_click_outside_the_card_dismisses_it},
}};

} // namespace

int main() {
  std::size_t failures = 0U;
  for (const Case &entry : cases) {
    try {
      entry.function();
      // Flushed, so a test that never returns is named by the last line.
      std::cout << "[pass] " << entry.name << std::endl;
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
    }
  }
  std::cout << (cases.size() - failures) << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
