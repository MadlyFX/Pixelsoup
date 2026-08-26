#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/editor/effects_panel.hpp>
#include <mediaserver/editor/workspace_panels.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>

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

struct SceneHarness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::ScenePanel panel{gui::ui_id("scene.test")};
  std::vector<editor::ElementInfo> items{elements()};
  std::vector<doc::NodeId> selection;
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
    return panel.draw(ctx, bounds, items, selection);
  }
};

void a_scene_row_reports_the_clicked_node() {
  SceneHarness harness;
  static_cast<void>(harness.frame(80.0F, 101.0F, false));
  static_cast<void>(harness.frame(80.0F, 101.0F, true));
  const auto request = harness.frame(80.0F, 101.0F, false);

  UI_CHECK(request.has_value());
  UI_CHECK(request->node == doc::NodeId{7U});
  UI_CHECK(!request->toggle);
}

void a_modified_scene_click_requests_a_selection_toggle() {
  SceneHarness harness;
  static_cast<void>(harness.frame(80.0F, 101.0F, false));
  static_cast<void>(harness.frame(80.0F, 101.0F, true, gui::KeyMods::Ctrl));
  const auto request = harness.frame(80.0F, 101.0F, false, gui::KeyMods::Ctrl);

  UI_CHECK(request.has_value());
  UI_CHECK(request->toggle);
}

void scene_search_filters_the_hierarchy() {
  SceneHarness harness;
  // Focus the search well, then type. The filtered tree contains only the
  // Lights group and its matching point-light row at the original first-row
  // coordinate.
  static_cast<void>(harness.frame(70.0F, 37.0F, false));
  static_cast<void>(harness.frame(70.0F, 37.0F, true));
  static_cast<void>(harness.frame(70.0F, 37.0F, false));
  static_cast<void>(
      harness.frame(70.0F, 37.0F, false, gui::KeyMods::None, "point"));
  UI_CHECK(harness.panel.search() == "point");

  static_cast<void>(harness.frame(80.0F, 101.0F, true));
  const auto request = harness.frame(80.0F, 101.0F, false);
  UI_CHECK(request.has_value());
  UI_CHECK(request->node == doc::NodeId{11U});
}

struct LibraryHarness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::LibraryPanel panel{gui::ui_id("library.test")};
  static constexpr gui::Rect bounds{0.0F, 0.0F, 260.0F, 320.0F};

  std::optional<editor::LibraryAddAction> frame(float x, float y, bool down) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    ctx.new_frame();
    return panel.draw(ctx, bounds);
  }
};

[[nodiscard]] assets::AssetRegistry project_assets() {
  assets::MediaAsset media{};
  media.id = doc::NodeId{20U};
  media.display_name = "Opening Plate";
  media.source = "media/opening_plate.exr";
  media.dimensions = assets::PixelDimensions{3840U, 2160U};
  media.frame_count = 900U;

  assets::ModelAsset model{};
  model.id = doc::NodeId{30U};
  model.display_name = "Hero Car";
  model.source = "models/hero_car.glb";
  model.triangle_count = 128'000U;
  model.material_count = 7U;

  assets::AssetRegistry registry;
  registry.assets.emplace_back(std::move(media));
  registry.assets.emplace_back(std::move(model));
  return registry;
}

struct AssetLibraryHarness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::LibraryPanel panel{gui::ui_id("library.assets.test")};
  gui::Rect bounds{0.0F, 0.0F, 360.0F, 520.0F};

  editor::LibraryFrame frame(const assets::AssetRegistry &registry, float x,
                             float y, bool down, std::string_view text = {},
                             float wheel = 0.0F) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.add_text(text);
    input.add_wheel(wheel);
    ctx.new_frame();
    return panel.draw(ctx, bounds, registry);
  }
};

void a_library_item_reports_an_add_action() {
  LibraryHarness harness;
  // Generator heading is 50..71, and Box is the first dense item row.
  static_cast<void>(harness.frame(80.0F, 81.0F, false));
  static_cast<void>(harness.frame(80.0F, 81.0F, true));
  const auto request = harness.frame(80.0F, 81.0F, false);

  UI_CHECK(request.has_value());
  UI_CHECK(request.value() == editor::LibraryAddAction::Box);
}

void library_filter_finds_and_instantiates_a_stable_asset() {
  AssetLibraryHarness harness;
  const assets::AssetRegistry registry = project_assets();

  static_cast<void>(harness.frame(registry, 70.0F, 37.0F, false));
  static_cast<void>(harness.frame(registry, 70.0F, 37.0F, true));
  static_cast<void>(harness.frame(registry, 70.0F, 37.0F, false));
  static_cast<void>(harness.frame(registry, 70.0F, 37.0F, false, "car"));
  UI_CHECK(harness.panel.search() == "car");

  // With this filter only MODELS remains: heading 50..71, asset 71..119.
  static_cast<void>(harness.frame(registry, 120.0F, 90.0F, true));
  const editor::LibraryFrame result =
      harness.frame(registry, 120.0F, 90.0F, false);
  UI_CHECK(result.instantiate_asset == doc::NodeId{30U});
  UI_CHECK(!result.import_asset.has_value());
  UI_CHECK(!result.add.has_value());
}

void library_imports_are_separate_persistent_asset_intents() {
  const assets::AssetRegistry empty;

  AssetLibraryHarness media_harness;
  // GENERATORS ends at 131; MEDIA's import button occupies 155..175.
  static_cast<void>(media_harness.frame(empty, 110.0F, 165.0F, false));
  static_cast<void>(media_harness.frame(empty, 110.0F, 165.0F, true));
  const editor::LibraryFrame media =
      media_harness.frame(empty, 110.0F, 165.0F, false);
  UI_CHECK(media.import_asset == editor::LibraryImportAction::Media);
  UI_CHECK(!media.instantiate_asset.has_value());
  UI_CHECK(!media.add.has_value());

  AssetLibraryHarness model_harness;
  // MEDIA's honest empty state ends at 202; MODELS' button is 226..246.
  static_cast<void>(model_harness.frame(empty, 110.0F, 235.0F, false));
  static_cast<void>(model_harness.frame(empty, 110.0F, 235.0F, true));
  const editor::LibraryFrame model =
      model_harness.frame(empty, 110.0F, 235.0F, false);
  UI_CHECK(model.import_asset == editor::LibraryImportAction::Model);
  UI_CHECK(!model.instantiate_asset.has_value());
  UI_CHECK(!model.add.has_value());
}

void an_existing_media_row_returns_its_asset_id() {
  AssetLibraryHarness harness;
  const assets::AssetRegistry registry = project_assets();

  // MEDIA heading follows the generators; its import button ends at 178 and
  // the persisted Opening Plate row occupies 178..226.
  static_cast<void>(harness.frame(registry, 120.0F, 200.0F, false));
  static_cast<void>(harness.frame(registry, 120.0F, 200.0F, true));
  const editor::LibraryFrame result =
      harness.frame(registry, 120.0F, 200.0F, false);
  UI_CHECK(result.instantiate_asset == doc::NodeId{20U});
  UI_CHECK(!result.import_asset.has_value());
  UI_CHECK(!result.add.has_value());
}

void an_imported_asset_row_has_a_separate_remove_action() {
  AssetLibraryHarness harness;
  const assets::AssetRegistry registry = project_assets();

  // The persisted media row occupies 178..226. Its rightmost action column is
  // reserved for removal, so it cannot also instantiate or begin a drag.
  static_cast<void>(harness.frame(registry, 346.0F, 200.0F, false));
  static_cast<void>(harness.frame(registry, 346.0F, 200.0F, true));
  const editor::LibraryFrame result =
      harness.frame(registry, 346.0F, 200.0F, false);
  UI_CHECK(result.remove_asset == doc::NodeId{20U});
  UI_CHECK(!result.instantiate_asset.has_value());
  UI_CHECK(!result.dragging_asset.has_value());
  UI_CHECK(!result.asset_drop.has_value());
}

void an_asset_row_can_be_dragged_out_for_stage_placement() {
  AssetLibraryHarness harness;
  const assets::AssetRegistry registry = project_assets();

  static_cast<void>(harness.frame(registry, 120.0F, 200.0F, false));
  static_cast<void>(harness.frame(registry, 120.0F, 200.0F, true));
  const editor::LibraryFrame dragging =
      harness.frame(registry, 500.0F, 250.0F, true);
  UI_CHECK(dragging.dragging_asset == doc::NodeId{20U});
  UI_CHECK(!dragging.instantiate_asset.has_value());

  const editor::LibraryFrame dropped =
      harness.frame(registry, 500.0F, 250.0F, false);
  UI_CHECK(dropped.asset_drop.has_value());
  UI_CHECK(dropped.asset_drop->asset == doc::NodeId{20U});
  UI_CHECK(dropped.asset_drop->screen_x == 500.0F);
  UI_CHECK(dropped.asset_drop->screen_y == 250.0F);
  UI_CHECK(!dropped.instantiate_asset.has_value());
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

  static_cast<void>(frame(80.0F, 50.0F, false));
  static_cast<void>(frame(80.0F, 50.0F, true));
  static_cast<void>(frame(430.0F, 270.0F, true));
  const auto dropped = frame(430.0F, 270.0F, false);
  UI_CHECK(dropped.has_value());
  UI_CHECK(dropped->target == doc::NodeId{});
  UI_CHECK(dropped->effect == scene::EffectKind::Brightness);
  UI_CHECK(dropped->dragged);
  UI_CHECK(dropped->screen_x == 430.0F);
  UI_CHECK(dropped->screen_y == 270.0F);
}

void effect_drops_apply_only_over_stage_or_inspector() {
  constexpr gui::Rect stage{300.0F, 40.0F, 600.0F, 400.0F};
  constexpr gui::Rect inspector{920.0F, 40.0F, 260.0F, 600.0F};
  editor::EffectApplyRequest request{
      doc::NodeId{42U}, scene::EffectKind::Blur, 1040.0F, 240.0F, true};

  UI_CHECK(editor::effect_drop_targets_selected_layer(request, &stage,
                                                       &inspector));
  request.screen_x = 500.0F;
  request.screen_y = 200.0F;
  UI_CHECK(editor::effect_drop_targets_selected_layer(request, &stage,
                                                       &inspector));
  request.screen_x = 120.0F;
  request.screen_y = 500.0F;
  UI_CHECK(!editor::effect_drop_targets_selected_layer(request, &stage,
                                                        &inspector));

  // Clicking an effect retains the established apply-to-selection shortcut,
  // while a drag with no selected target can never apply implicitly.
  request.dragged = false;
  UI_CHECK(editor::effect_drop_targets_selected_layer(request, nullptr,
                                                       nullptr));
  request.target = {};
  UI_CHECK(!editor::effect_drop_targets_selected_layer(request, &stage,
                                                        &inspector));
}

void an_empty_registry_has_no_phantom_asset_rows() {
  AssetLibraryHarness harness;
  const assets::AssetRegistry empty;

  // This is the MEDIA empty-state row, not its explicit import button.
  static_cast<void>(harness.frame(empty, 110.0F, 190.0F, false));
  static_cast<void>(harness.frame(empty, 110.0F, 190.0F, true));
  const editor::LibraryFrame result =
      harness.frame(empty, 110.0F, 190.0F, false);
  UI_CHECK(result == editor::LibraryFrame{});
  UI_CHECK(!harness.list.vertices().empty());
}

void long_asset_lists_scroll_and_clip_interaction_to_the_list() {
  AssetLibraryHarness harness;
  harness.bounds = gui::Rect{0.0F, 0.0F, 300.0F, 220.0F};
  assets::AssetRegistry registry;
  for (core::u64 index = 0U; index < 30U; ++index) {
    assets::MediaAsset asset{};
    asset.id = doc::NodeId{100U + index};
    asset.display_name = "Plate " + std::to_string(index);
    asset.source = "media/plate_" + std::to_string(index) + ".exr";
    registry.assets.emplace_back(std::move(asset));
  }

  // Scroll to the end, then click the title. Rows whose logical coordinates
  // pass behind the title must neither paint nor receive that click.
  static_cast<void>(
      harness.frame(registry, 120.0F, 100.0F, false, {}, -100.0F));
  static_cast<void>(harness.frame(registry, 120.0F, 10.0F, true));
  const editor::LibraryFrame result =
      harness.frame(registry, 120.0F, 10.0F, false);
  UI_CHECK(result == editor::LibraryFrame{});
  UI_CHECK(harness.list.vertices().size() < 1'000U);
  for (const gui::UiBatch &batch : harness.list.batches()) {
    UI_CHECK(batch.clip_min_x >= harness.bounds.x);
    UI_CHECK(batch.clip_min_y >= harness.bounds.y);
    UI_CHECK(batch.clip_max_x <= harness.bounds.x + harness.bounds.width);
    UI_CHECK(batch.clip_max_y <= harness.bounds.y + harness.bounds.height);
  }
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
  case compositor::NodeKind::Blur:
    node.parameters = compositor::BlurParameters{12.0F};
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
  static constexpr gui::Rect bounds{0.0F, 0.0F, 900.0F, 320.0F};

  editor::CompositorFrame frame(const compositor::CompositorGraph &graph,
                                float x, float y, bool down,
                                std::string_view text = {},
                                std::optional<gui::Key> key = std::nullopt) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.add_text(text);
    if (key.has_value()) {
      input.add_key(*key);
    }
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

void compositor_toolbar_reports_effect_add_intents() {
  const compositor::CompositorGraph graph;
  struct ButtonCase final {
    float x;
    compositor::NodeKind kind;
  };
  constexpr std::array<ButtonCase, 3U> buttons{{
      {130.0F, compositor::NodeKind::Brightness},
      {198.0F, compositor::NodeKind::Blur},
      {265.0F, compositor::NodeKind::RadialArray},
  }};
  for (const ButtonCase &button : buttons) {
    CompositorHarness harness;
    static_cast<void>(harness.frame(graph, button.x, 13.0F, false));
    static_cast<void>(harness.frame(graph, button.x, 13.0F, true));
    const editor::CompositorFrame result =
        harness.frame(graph, button.x, 13.0F, false);
    UI_CHECK(result.add_node.has_value());
    UI_CHECK(result.add_node->kind == button.kind);
    UI_CHECK(result.add_node->editor_x >= 0.0F);
    UI_CHECK(result.add_node->editor_y >= 0.0F);
    UI_CHECK(!result.move_node.has_value());
    UI_CHECK(!result.connect_ports.has_value());
  }
}

void compositor_output_creation_is_visible_and_unique() {
  constexpr float output_x = 481.0F;
  constexpr float output_y = 13.0F;

  const compositor::CompositorGraph empty;
  CompositorHarness add_harness;
  static_cast<void>(add_harness.frame(empty, output_x, output_y, false));
  static_cast<void>(add_harness.frame(empty, output_x, output_y, true));
  const editor::CompositorFrame added =
      add_harness.frame(empty, output_x, output_y, false);
  UI_CHECK(added.add_node.has_value());
  UI_CHECK(added.add_node->kind == compositor::NodeKind::Output);

  compositor::CompositorGraph configured;
  configured.nodes.push_back(
      graph_node(90U, compositor::NodeKind::Output, 320.0F, 60.0F));
  CompositorHarness disabled_harness;
  static_cast<void>(
      disabled_harness.frame(configured, output_x, output_y, false));
  static_cast<void>(
      disabled_harness.frame(configured, output_x, output_y, true));
  const editor::CompositorFrame rejected =
      disabled_harness.frame(configured, output_x, output_y, false);
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
  // as the active Delete-key domain.
  const editor::CompositorFrame reselected =
      node_harness.frame(node_graph, 100.0F, 75.0F, true);
  UI_CHECK(reselected.selection ==
           editor::CompositorSelection{editor::CompositorSelectionKind::Node,
                                       doc::NodeId{1U}});
  static_cast<void>(node_harness.frame(node_graph, 100.0F, 75.0F, false));

  const editor::CompositorFrame node_deleted = node_harness.frame(
      node_graph, 100.0F, 75.0F, false, {}, gui::Key::Delete);
  UI_CHECK(node_deleted.remove ==
           editor::CompositorDeleteIntent{editor::CompositorSelection{
               editor::CompositorSelectionKind::Node, doc::NodeId{1U}}});
  UI_CHECK(node_deleted.consume_delete);
  const editor::CompositorFrame idle =
      node_harness.frame(node_graph, 100.0F, 75.0F, false);
  UI_CHECK(!idle.remove.has_value());
  UI_CHECK(!idle.consume_delete);

  // The visible toolbar action returns the same compact intent.
  constexpr float remove_x = 849.0F;
  constexpr float remove_y = 13.0F;
  static_cast<void>(
      node_harness.frame(node_graph, remove_x, remove_y, false));
  static_cast<void>(
      node_harness.frame(node_graph, remove_x, remove_y, true));
  const editor::CompositorFrame button_deleted =
      node_harness.frame(node_graph, remove_x, remove_y, false);
  UI_CHECK(button_deleted.remove ==
           editor::CompositorDeleteIntent{editor::CompositorSelection{
               editor::CompositorSelectionKind::Node, doc::NodeId{1U}}});

  compositor::CompositorGraph edge_graph;
  edge_graph.nodes.push_back(
      graph_node(10U, compositor::NodeKind::SceneLayer, 40.0F, 60.0F));
  edge_graph.nodes.push_back(
      graph_node(11U, compositor::NodeKind::Output, 320.0F, 60.0F));
  edge_graph.edges.push_back(compositor::GraphEdge{
      doc::NodeId{50U}, compositor::EdgeEndpoint{doc::NodeId{10U}, 0U},
      compositor::EdgeEndpoint{doc::NodeId{11U}, 0U}, 0});
  CompositorHarness edge_harness;
  constexpr float wire_x = 262.0F;
  constexpr float wire_y = 122.0F;
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
           editor::CompositorDeleteIntent{editor::CompositorSelection{
               editor::CompositorSelectionKind::Edge, doc::NodeId{50U}}});
  UI_CHECK(disconnected.consume_delete);
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
  UI_CHECK(!held.move_node.has_value());
  const editor::CompositorFrame result =
      harness.frame(graph, 160.0F, 135.0F, false);
  UI_CHECK(result.move_node.has_value());
  UI_CHECK(result.move_node->node == doc::NodeId{1U});
  UI_CHECK(result.move_node->editor_x == 120.0F);
  UI_CHECK(result.move_node->editor_y == 100.0F);
}

void compatible_typed_ports_report_a_connection_intent() {
  compositor::CompositorGraph graph;
  graph.nodes.push_back(
      graph_node(1U, compositor::NodeKind::Blur, 40.0F, 60.0F));
  graph.nodes.push_back(
      graph_node(2U, compositor::NodeKind::SceneLayer, 320.0F, 60.0F));
  CompositorHarness harness;

  // Both are EFFECT ports. Node width is 164; the first port centre is 36px
  // below the node top, and the canvas begins at y=26.
  static_cast<void>(harness.frame(graph, 204.0F, 122.0F, false));
  static_cast<void>(harness.frame(graph, 204.0F, 122.0F, true));
  static_cast<void>(harness.frame(graph, 320.0F, 122.0F, true));
  const editor::CompositorFrame result =
      harness.frame(graph, 320.0F, 122.0F, false);
  UI_CHECK(result.connect_ports.has_value());
  UI_CHECK(result.connect_ports->from ==
           compositor::EdgeEndpoint{doc::NodeId{1U}, 0U});
  UI_CHECK(result.connect_ports->to ==
           compositor::EdgeEndpoint{doc::NodeId{2U}, 0U});
  UI_CHECK(!result.add_node.has_value());
  UI_CHECK(!result.move_node.has_value());
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
  static_cast<void>(harness.frame(graph, 204.0F, 122.0F, false));
  static_cast<void>(harness.frame(graph, 204.0F, 122.0F, true));
  static_cast<void>(harness.frame(graph, 320.0F, 122.0F, true));
  const editor::CompositorFrame result =
      harness.frame(graph, 320.0F, 122.0F, false);

  UI_CHECK(result.connect_ports == editor::CompositorConnectIntent{
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
      incompatible_harness.frame(incompatible, 204.0F, 122.0F, false));
  static_cast<void>(
      incompatible_harness.frame(incompatible, 204.0F, 122.0F, true));
  static_cast<void>(
      incompatible_harness.frame(incompatible, 320.0F, 122.0F, true));
  const editor::CompositorFrame rejected =
      incompatible_harness.frame(incompatible, 320.0F, 122.0F, false);
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
  constexpr std::array<compositor::DiagnosticCode, 11U> codes{{
      compositor::DiagnosticCode::NodesOverCapacity,
      compositor::DiagnosticCode::EdgesOverCapacity,
      compositor::DiagnosticCode::DanglingNode,
      compositor::DiagnosticCode::DanglingSceneReference,
      compositor::DiagnosticCode::UnknownPort,
      compositor::DiagnosticCode::IncompatiblePorts,
      compositor::DiagnosticCode::DuplicateInput,
      compositor::DiagnosticCode::MissingRequiredInput,
      compositor::DiagnosticCode::MissingOutput,
      compositor::DiagnosticCode::DuplicateOutput,
      compositor::DiagnosticCode::Cycle,
  }};
  for (const compositor::DiagnosticCode code : codes) {
    const std::string_view message = editor::compositor_diagnostic_message(
        compositor::GraphDiagnostic{code, {}, {}, 0U});
    UI_CHECK(!message.empty());
    UI_CHECK(message.find("Graph is not ready") == std::string_view::npos);
  }
  UI_CHECK(editor::compositor_diagnostic_message(compositor::GraphDiagnostic{
               compositor::DiagnosticCode::MissingRequiredInput, {}, {}, 0U}) ==
           "A required input is not connected");
  UI_CHECK(editor::compositor_diagnostic_message(compositor::GraphDiagnostic{
               compositor::DiagnosticCode::IncompatiblePorts, {}, {}, 0U}) ==
           "Port types do not match");
  UI_CHECK(editor::compositor_diagnostic_message(compositor::GraphDiagnostic{
               compositor::DiagnosticCode::MissingOutput, {}, {}, 0U}) ==
           "Add and connect one Output node");
  UI_CHECK(editor::compositor_diagnostic_message(compositor::GraphDiagnostic{
               compositor::DiagnosticCode::Cycle, {}, {}, 0U}) ==
           "Cycle detected; wires must flow toward Output");

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
  UI_CHECK(!result.move_node.has_value());
  UI_CHECK(!result.connect_ports.has_value());
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

  result = commit_compositor_field(harness, graph, 790.0F, 147.0F, "-2.25");
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

  editor::OutputMonitorFrame frame(
      float x, float y, bool down,
      const editor::OutputMonitorState &state, float wheel = 0.0F) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.add_wheel(wheel);
    ctx.new_frame();
    return panel.draw(ctx, bounds, state);
  }
};

void output_monitor_emits_one_fixed_lifecycle_intent() {
  OutputMonitorHarness harness;
  const editor::OutputMonitorState::Node node{
      doc::NodeId{50U}, "Render A", "127.0.0.1:47840", 3U, 1920U, 1080U,
      editor::OutputMonitorState::Node::Status::Running, 4U, {}};
  editor::OutputMonitorState state{240U, 3840U, 1080U, 60.0F, true,
                                   std::span{&node, 1U}};
  static_cast<void>(harness.frame(400.0F, 92.0F, false, state));
  static_cast<void>(harness.frame(400.0F, 92.0F, true, state));
  const auto released = harness.frame(400.0F, 92.0F, false, state);
  UI_CHECK(released.lifecycle.has_value());
  UI_CHECK(released.lifecycle->device == doc::NodeId{50U});
  UI_CHECK(released.lifecycle->node_index == 3U);
  UI_CHECK(released.lifecycle->endpoint == "127.0.0.1:47840");
  UI_CHECK(released.lifecycle->action ==
           editor::OutputLifecycleAction::Stop);
}

void duplicate_node_indices_have_no_ambiguous_controls() {
  OutputMonitorHarness harness;
  const editor::OutputMonitorState::Node node{
      doc::NodeId{50U}, "Render A", "127.0.0.1:47840", 3U, 1920U, 1080U,
      editor::OutputMonitorState::Node::Status::DuplicateNodeIndex, 0U, {}};
  editor::OutputMonitorState state{240U, 3840U, 1080U, 60.0F, true,
                                   std::span{&node, 1U}};
  static_cast<void>(harness.frame(400.0F, 92.0F, false, state));
  static_cast<void>(harness.frame(400.0F, 92.0F, true, state));
  const auto released = harness.frame(400.0F, 92.0F, false, state);
  UI_CHECK(!released.lifecycle.has_value());
}

void missing_cluster_assets_block_start_but_keep_status_visible() {
  OutputMonitorHarness harness;
  editor::OutputMonitorState::Node node{
      doc::NodeId{50U}, "Render A", "127.0.0.1:47840", 3U, 1920U, 1080U,
      editor::OutputMonitorState::Node::Status::Stopped, 0U, {}};
  node.content =
      editor::OutputMonitorState::Node::ContentStatus::Missing;
  node.assets_required = 2U;
  node.assets_ready = 1U;
  node.assets_missing = 1U;
  node.content_detail = "one required object is absent";
  editor::OutputMonitorState state{240U, 3840U, 1080U, 60.0F, true,
                                   std::span{&node, 1U}};
  static_cast<void>(harness.frame(400.0F, 92.0F, false, state));
  static_cast<void>(harness.frame(400.0F, 92.0F, true, state));
  const auto released = harness.frame(400.0F, 92.0F, false, state);
  UI_CHECK(!released.lifecycle.has_value());
  UI_CHECK(harness.list.vertices().size() > 50U);
}

void output_monitor_scroll_reaches_later_cluster_nodes() {
  OutputMonitorHarness harness;
  editor::OutputMonitorState state{240U, 3840U, 1080U, 60.0F, true};
  std::vector<editor::OutputMonitorState::Node> nodes;
  for (core::u32 index = 0U; index < 4U; ++index) {
    nodes.push_back(editor::OutputMonitorState::Node{
        doc::NodeId{60U + index}, "Render", "127.0.0.1:47840", index,
        1920U, 1080U, editor::OutputMonitorState::Node::Status::Stopped, 0U,
        {}});
  }
  state.nodes = nodes;
  // Scroll the list to its lower bound, then click START on the last row.
  static_cast<void>(harness.frame(400.0F, 90.0F, false, state, -10.0F));
  static_cast<void>(harness.frame(400.0F, 120.0F, false, state));
  static_cast<void>(harness.frame(400.0F, 120.0F, true, state));
  const auto released = harness.frame(400.0F, 120.0F, false, state);
  UI_CHECK(released.lifecycle.has_value());
  UI_CHECK(released.lifecycle->device == doc::NodeId{63U});
  UI_CHECK(released.lifecycle->node_index == 3U);
  UI_CHECK(released.lifecycle->action == editor::OutputLifecycleAction::Start);
}

void output_monitor_emits_explicit_cluster_relaunch() {
  OutputMonitorHarness harness;
  const editor::OutputMonitorState::Node node{
      doc::NodeId{50U}, "Render A", "127.0.0.1:47840", 3U, 1920U, 1080U,
      editor::OutputMonitorState::Node::Status::Running, 4U, {}};
  editor::OutputMonitorState state{240U, 3840U, 1080U, 60.0F, true,
                                   std::span{&node, 1U}};
  static_cast<void>(harness.frame(320.0F, 140.0F, false, state));
  static_cast<void>(harness.frame(320.0F, 140.0F, true, state));
  const auto released = harness.frame(320.0F, 140.0F, false, state);
  UI_CHECK(released.lifecycle.has_value());
  UI_CHECK(released.lifecycle->all);
  UI_CHECK(released.lifecycle->device == doc::NodeId{});
  UI_CHECK(released.lifecycle->endpoint.empty());
  UI_CHECK(released.lifecycle->action ==
           editor::OutputLifecycleAction::Restart);
}

void output_monitor_cluster_health_keeps_node_controls_reachable() {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::OutputMonitorPanel panel{gui::ui_id("monitor.cluster.health.test")};
  constexpr gui::Rect bounds{0.0F, 0.0F, 430.0F, 190.0F};
  const editor::OutputMonitorState::Node node{
      doc::NodeId{50U}, "Render A", "127.0.0.1:47840", 3U, 1920U, 1080U,
      editor::OutputMonitorState::Node::Status::Running, 4U, {}};
  editor::OutputMonitorState state{240U, 3840U, 1080U, 60.0F, true,
                                   std::span{&node, 1U}};
  state.cluster.health =
      editor::OutputMonitorState::Cluster::Health::Healthy;
  state.cluster.configured_nodes = 1U;
  state.cluster.live_sessions = 1U;
  state.cluster.diagnostics_recording = true;

  const auto frame = [&](bool down) {
    input.new_frame();
    input.set_mouse_position(400.0F, 128.0F);
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

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 32U> cases{{
    {"a scene row reports the clicked node",
     &a_scene_row_reports_the_clicked_node},
    {"a modified scene click requests a selection toggle",
     &a_modified_scene_click_requests_a_selection_toggle},
    {"scene search filters the hierarchy", &scene_search_filters_the_hierarchy},
    {"a library item reports an add action",
     &a_library_item_reports_an_add_action},
    {"library filter finds and instantiates a stable asset",
     &library_filter_finds_and_instantiates_a_stable_asset},
    {"library imports are separate persistent asset intents",
     &library_imports_are_separate_persistent_asset_intents},
    {"an existing media row returns its asset id",
     &an_existing_media_row_returns_its_asset_id},
    {"an imported asset row has a separate remove action",
     &an_imported_asset_row_has_a_separate_remove_action},
    {"an asset row can be dragged out for stage placement",
     &an_asset_row_can_be_dragged_out_for_stage_placement},
    {"an effect drag reports its graph drop position without a selection",
     &an_effect_drag_reports_its_graph_drop_position_without_a_selection},
    {"effect drops apply only over Stage or Inspector",
     &effect_drops_apply_only_over_stage_or_inspector},
    {"an empty registry has no phantom asset rows",
     &an_empty_registry_has_no_phantom_asset_rows},
    {"long asset lists scroll and clip interaction to the list",
     &long_asset_lists_scroll_and_clip_interaction_to_the_list},
    {"compositor toolbar reports effect add intents",
     &compositor_toolbar_reports_effect_add_intents},
    {"compositor output creation is visible and unique",
     &compositor_output_creation_is_visible_and_unique},
    {"selected compositor nodes and edges return delete intents",
     &selected_compositor_nodes_and_edges_return_delete_intents},
    {"project replacement clears compositor document state",
     &project_replacement_clears_compositor_document_state},
    {"dragging a compositor node reports its authored position on release",
     &dragging_a_compositor_node_reports_its_authored_position_on_release},
    {"compatible typed ports report a connection intent",
     &compatible_typed_ports_report_a_connection_intent},
    {"dropping on a single input reports an atomic rewire",
     &dropping_on_a_single_input_reports_an_atomic_rewire},
    {"incompatible ports are rejected and persisted edges draw wires",
     &incompatible_ports_are_rejected_and_persisted_edges_draw_wires},
    {"compositor diagnostics are explained in plain language",
     &compositor_diagnostics_are_explained_in_plain_language},
    {"brightness parameter typing emits one typed commit",
     &brightness_parameter_typing_emits_one_typed_commit},
    {"blur parameter commits are clamped to the model contract",
     &blur_parameter_commits_are_clamped_to_the_model_contract},
    {"radial array parameters emit count radius angle and center types",
     &radial_array_parameters_emit_count_radius_angle_and_center_types},
    {"workspace panels draw scene relevant content",
     &workspace_panels_draw_scene_relevant_content},
    {"output monitor emits fixed lifecycle intent",
     &output_monitor_emits_one_fixed_lifecycle_intent},
    {"duplicate node indices disable lifecycle controls",
     &duplicate_node_indices_have_no_ambiguous_controls},
    {"missing cluster assets block start but keep status visible",
     &missing_cluster_assets_block_start_but_keep_status_visible},
    {"output monitor scroll reaches later cluster nodes",
     &output_monitor_scroll_reaches_later_cluster_nodes},
    {"output monitor emits explicit cluster relaunch",
     &output_monitor_emits_explicit_cluster_relaunch},
    {"output monitor cluster health keeps node controls reachable",
     &output_monitor_cluster_health_keeps_node_controls_reachable},
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
