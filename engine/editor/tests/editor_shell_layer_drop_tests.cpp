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
void layer_drop() {
  if (!editor::editor_available()) {
    std::puts("[skip] editor GPU backend unavailable");
    return;
  }
  gui::DockLayout layout;
  const auto timeline_panel = gui::ui_id("editor.panel.timeline");
  const auto compositor_panel = gui::ui_id("editor.panel.compositor");
  const auto timeline_leaf = layout.add(timeline_panel, "Timeline");
  static_cast<void>(layout.dock(compositor_panel, "Compositor", timeline_leaf,
                                gui::DockSide::Right));
  const auto workspace = editor::encode_workspace_layout(
      editor::WorkspaceLayoutState{layout.snapshot(), 1600U, 900U});
  check(workspace.has_value(), "encode side-by-side workspace");
  editor::EditorConfig config;
  config.title = "Timeline group drop regression";
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
    elements[i].kind = editor::ElementKind::Object;
    elements[i].display_name = "Layer " + std::to_string(i);
    elements[i].group_order = static_cast<mediaserver::core::i32>(3U - i);
  }
  compositor::CompositorGraph graph;
  const auto frame = [&] {
    auto result = shell->frame(show, timeline, keys, elements,
                              {}, {}, {}, {}, {}, {}, graph);
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
  const auto modifier = [&](SDL_Keymod mods) {
    SDL_SetModState(mods);
    SDL_Event event{};
    event.type = mods == SDL_KMOD_NONE ? SDL_EVENT_KEY_UP : SDL_EVENT_KEY_DOWN;
    event.key.windowID = window_id;
    event.key.key = SDLK_LCTRL;
    event.key.mod = mods;
    event.key.down = mods != SDL_KMOD_NONE;
    check(SDL_PushEvent(&event), "selection modifier");
    static_cast<void>(frame());
  };
  // First row centre is 88px below the panel top; each following row is 24px.
  const float x = rows.x + 80.0F;
  static_cast<void>(pointer(x, rows.y + 88.0F, true));
  static_cast<void>(pointer(x, rows.y + 88.0F, false));
  modifier(SDL_KMOD_CTRL);
  static_cast<void>(pointer(x, rows.y + 112.0F, true));
  static_cast<void>(pointer(x, rows.y + 112.0F, false));
  modifier(SDL_KMOD_NONE);

  const float drop_x = canvas.x + canvas.width * 0.5F;
  const float drop_y = canvas.y + 180.0F;
  const auto drag = [&] {
    check(pointer(x, rows.y + 88.0F, true).compositor_adds.empty(),
          "press does not author nodes");
    check(pointer(drop_x, drop_y, true).compositor_adds.empty(),
          "drag previews without authoring");
    return pointer(drop_x, drop_y, false);
  };
  const auto dropped = drag();
  check(dropped.compositor_adds.size() == 2U, "drop creates both selected layers");
  check(dropped.compositor_gesture != 0U, "batch has one undo gesture");
  const auto &first = dropped.compositor_adds[0U];
  const auto &second = dropped.compositor_adds[1U];
  check(first.scene_node == elements[0U].node &&
            second.scene_node == elements[1U].node,
        "batch follows timeline order");
  check(first.kind == compositor::NodeKind::SceneLayer &&
            second.kind == compositor::NodeKind::SceneLayer,
        "batch contains Scene Layer nodes");
  check(std::abs(first.editor_x - (drop_x - canvas.x - 82.0F)) < 0.1F &&
            std::abs(first.editor_y - (180.0F - 26.0F - 27.0F)) < 0.1F,
        "first node is centred on the graph drop");
  check(second.editor_x == first.editor_x &&
            second.editor_y == first.editor_y + 70.0F,
        "new nodes have separate positions");
  check(frame().compositor_adds.empty(), "release authors once");

  compositor::GraphNode existing;
  existing.node = doc::NodeId{201U};
  existing.parameters = compositor::SceneLayerParameters{elements[0U].node};
  graph.nodes.push_back(existing);
  const auto partial = drag();
  check(partial.compositor_adds.size() == 1U &&
            partial.compositor_adds.front().scene_node == elements[1U].node,
        "existing graph input is skipped without losing the other layer");
  existing.node = doc::NodeId{202U};
  existing.parameters = compositor::SceneLayerParameters{elements[1U].node};
  graph.nodes.push_back(existing);
  check(drag().compositor_adds.empty(), "repeated group drop skips duplicates");
  static_cast<void>(pointer(x, rows.y + 88.0F, true));
  static_cast<void>(pointer(rows.x + rows.width - 10.0F, rows.y + 250.0F, true));
  check(pointer(rows.x + rows.width - 10.0F, rows.y + 250.0F, false)
            .compositor_adds.empty(), "release outside graph does not add nodes");
}
} // namespace

int main() {
  try {
    layer_drop();
    std::puts("[pass] selected timeline layers drop through the editor shell");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "[fail] %s\n", error.what());
    return 1;
  }
}
