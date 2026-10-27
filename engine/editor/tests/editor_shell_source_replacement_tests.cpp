#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/editor/workspace_layout.hpp>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace scene = mediaserver::scene;
namespace doc = mediaserver::doc;
namespace compositor = mediaserver::compositor;

void check(bool value, const char *reason) {
  if (!value) {
    throw std::runtime_error{reason};
  }
}

// Real SDL input through the shipping docked shell, including the selection
// carrier and screen-to-graph conversion that a panel-only test cannot see.
void source_replacement() {
  if (!editor::editor_available()) {
    std::puts("[skip] editor GPU backend unavailable");
    return;
  }
  gui::DockLayout layout;
  const auto timeline_panel = gui::ui_id("editor.panel.timeline");
  const auto compositor_panel = gui::ui_id("editor.panel.compositor");
  const auto library_panel = gui::ui_id("editor.panel.library");
  const auto timeline_leaf = layout.add(timeline_panel, "Timeline");
  static_cast<void>(layout.dock(compositor_panel, "Compositor", timeline_leaf,
                                gui::DockSide::Right));
  static_cast<void>(layout.dock(library_panel, "Library", timeline_leaf, gui::DockSide::Top));
  const auto workspace = editor::encode_workspace_layout(
      editor::WorkspaceLayoutState{layout.snapshot(), 1600U, 900U});
  check(workspace.has_value(), "encode side-by-side workspace");
  editor::EditorConfig config;
  config.title = "Source replacement regression";
  config.workspace_layout = workspace.value();
  auto created = editor::EditorShell::create(config);
  check(created.has_value(), "create editor");
  auto shell = std::move(created).value();
  scene::Scene show;
  show.id = scene::SceneId{100U};
  editor::EditorTimeline timeline;
  timeline.last_frame = 600U;
  const std::vector<scene::ChannelKeyframes> keys;
  std::vector<editor::ElementInfo> elements(3U);
  // Deliberately use node ids in a different order from their timeline order.
  elements[0U].node = doc::NodeId{27U};
  elements[1U].node = doc::NodeId{13U};
  elements[2U].node = doc::NodeId{41U};
  for (std::size_t i = 0U; i < elements.size(); ++i) {
    elements[i].kind = i == 1U ? editor::ElementKind::Model : editor::ElementKind::Media;
    elements[i].display_name = "Layer " + std::to_string(i);
    elements[i].group_order = static_cast<mediaserver::core::i32>(3U - i);
  }
  compositor::CompositorGraph graph;
  compositor::GraphNode input;
  input.node = doc::NodeId{201U};
  input.kind = compositor::NodeKind::SceneLayer;
  input.editor_x = 40.0F; input.editor_y = 100.0F;
  input.parameters = compositor::SceneLayerParameters{elements[0U].node};
  graph.nodes.push_back(input);
  input.node = doc::NodeId{202U};
  input.editor_y = 240.0F;
  input.parameters = compositor::SceneLayerParameters{elements[1U].node};
  graph.nodes.push_back(input);
  mediaserver::assets::AssetRegistry registry;
  mediaserver::assets::MediaAsset media;
  media.id = doc::NodeId{71U}; media.display_name = "Replacement video";
  media.source = "T:/replacement.mov";
  registry.assets.emplace_back(media);
  mediaserver::assets::ModelAsset model;
  model.id = doc::NodeId{81U}; model.display_name = "Replacement model";
  model.source = "T:/replacement.msmodel";
  registry.assets.emplace_back(model);
  editor::EditorSwitcherState switcher;

  const auto frame = [&] {
    auto result = shell->frame(show, timeline, keys, elements,
                              {}, {}, {}, switcher, registry, {}, graph);
    check(result.status == editor::EditorStatus::Running, "editor running");
    return result;
  };
  static_cast<void>(frame());
  SDL_Window *window = nullptr;
  int count = 0;
  SDL_Window **windows = SDL_GetWindows(&count);
  for (int i = 0; i < count; ++i) {
    if (std::string{SDL_GetWindowTitle(windows[i])} == config.title) {
      window = windows[i];
    }
  }
  SDL_free(windows);
  check(window != nullptr, "find editor window");
  const float scale = SDL_GetWindowDisplayScale(window);
  const float input_scale = scale / SDL_GetWindowPixelDensity(window);
  int width = 0;
  int height = 0;
  check(SDL_GetWindowSizeInPixels(window, &width, &height), "window extent");
  layout.solve({0, 34, static_cast<float>(width) / scale,
                static_cast<float>(height) / scale - 62},
               {.divider = 6, .seam_reach = 4, .tab_bar = 32,
                .title_bar = 30, .edge = 0.25F});
  const auto rows = layout.content_of(layout.leaf_of(timeline_panel));
  const auto canvas = layout.content_of(layout.leaf_of(compositor_panel));
  const auto library = layout.content_of(layout.leaf_of(library_panel));
  const auto window_id = SDL_GetWindowID(window);
  const auto pointer = [&](float x, float y, bool down) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.windowID = window_id;
    event.motion.x = x * input_scale;
    event.motion.y = y * input_scale;
    check(SDL_PushEvent(&event), "move pointer");
    event = {};
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.windowID = window_id;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = down;
    event.button.x = x * input_scale;
    event.button.y = y * input_scale;
    check(SDL_PushEvent(&event), "pointer button");
    return frame();
  };
  // Filter Library to the two assets, making source positions independent of
  // the built-in generator palette. The List view sorts them by name under
  // its column header: the model's row is 70..103, the video's 103..136.
  static_cast<void>(pointer(library.x + 75.0F, library.y + 37.0F, true));
  static_cast<void>(pointer(library.x + 75.0F, library.y + 37.0F, false));
  SDL_Event text{};
  text.type = SDL_EVENT_TEXT_INPUT; text.text.windowID = window_id;
  text.text.text = "Replacement";
  check(SDL_PushEvent(&text), "filter Library");
  static_cast<void>(frame());
  const float asset_x = library.x + 80.0F;
  const float media_y = library.y + 120.0F;
  const float model_y = library.y + 86.0F;
  const float row_x = rows.x + 110.0F;
  const float media_row = rows.y + 88.0F;
  const float model_row = rows.y + 112.0F;
  const float source_row = rows.y + 136.0F;
  const float graph_x = canvas.x + 110.0F;
  const float graph_media_y = canvas.y + 26.0F + 125.0F;
  const float graph_model_y = canvas.y + 26.0F + 265.0F;
  const auto drag = [&](float sx, float sy, float dx, float dy) {
    const auto pressed = pointer(sx, sy, true);
    check(pressed.source_replacements.empty(), "press does not replace");
    const auto hover = pointer(dx, dy, true);
    check(hover.source_replacements.empty() && hover.compositor_adds.empty() && hover.adds.empty(),
          "hover previews without authoring");
    return pointer(dx, dy, false);
  };
  const auto expect_replacement = [&](const editor::EditorFrame &result,
                                      doc::NodeId target, doc::NodeId asset, doc::NodeId layer) {
    check(result.source_replacements.size() == 1U, "one replacement on release");
    const auto &replacement = result.source_replacements.front();
    check(replacement.target == target && replacement.asset == asset &&
          replacement.layer == layer && replacement.scene == show.id && replacement.gesture != 0U,
          "replacement has exact target and source identity");
    check(result.adds.empty() && result.compositor_adds.empty() && result.deletes.empty() &&
          !result.compositor_delete.has_value(), "replacement preserves existing objects");
    check(frame().source_replacements.empty(), "release authors only once");
  };
  expect_replacement(drag(asset_x, media_y, row_x, media_row), elements[0U].node, media.id, {});
  expect_replacement(drag(asset_x, media_y, rows.x + rows.width - 20.0F, media_row),
                     elements[0U].node, media.id, {});
  expect_replacement(drag(asset_x, model_y, row_x, model_row), elements[1U].node, model.id, {});
  expect_replacement(drag(asset_x, media_y, graph_x, graph_media_y), elements[0U].node, media.id, {});
  expect_replacement(drag(asset_x, model_y, graph_x, graph_model_y), elements[1U].node, model.id, {});
  check(drag(asset_x, media_y, row_x, model_row).source_replacements.empty(),
        "video cannot replace a timeline model");
  check(drag(asset_x, model_y, row_x, media_row).source_replacements.empty(),
        "model cannot replace a timeline video");
  check(drag(asset_x, media_y, graph_x, graph_model_y).source_replacements.empty(),
        "video cannot replace a compositor model");
  check(drag(asset_x, model_y, graph_x, graph_media_y).source_replacements.empty(),
        "model cannot replace a compositor video");
  expect_replacement(drag(row_x, source_row, graph_x, graph_media_y), graph.nodes[0U].node,
                     {}, elements[2U].node);
  const auto incompatible_layer = drag(row_x, model_row, graph_x, graph_media_y);
  check(incompatible_layer.source_replacements.empty() && incompatible_layer.compositor_adds.empty(),
        "incompatible layer drop neither replaces nor adds a hidden input");
  elements[0U].locked = true;
  check(drag(asset_x, media_y, row_x, media_row).source_replacements.empty(), "locked row refuses replacement");
  check(drag(asset_x, media_y, graph_x, graph_media_y).source_replacements.empty(), "locked graph layer refuses replacement");
  elements[0U].locked = false;
  elements[0U].linked = true;
  check(drag(asset_x, media_y, row_x, media_row).source_replacements.empty(), "linked source remains Blender-owned");
  elements[0U].linked = false;
  switcher.rehearsal_locked = true;
  check(drag(asset_x, media_y, row_x, media_row).source_replacements.empty(), "locked scene refuses replacement");
  switcher.rehearsal_locked = false;
  check(drag(asset_x, media_y, library.x + library.width - 15.0F, library.y + library.height - 30.0F)
        .source_replacements.empty(), "release outside a replacement target does nothing");

}
} // namespace

int main() {
  try {
    source_replacement();
    std::puts("[pass] Library and Timeline source replacement through the editor shell");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "[fail] %s\n", error.what());
    return 1;
  }
}
