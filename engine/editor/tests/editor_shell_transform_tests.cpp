#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/editor/workspace_layout.hpp>
#include <mediaserver/gui/dock.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/scene/math.hpp>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// The Stage's Blender keys through the shipping shell: a real editor window,
// fed SDL events the way the platform feeds it, authoring the same intents the
// Director receives. Proves G/R/S, axis keys, typed values, Enter/Escape and
// Alt+G reach the document as one undoable gesture each - the part the panel
// tests cannot see.
namespace {
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace scene = mediaserver::scene;
namespace doc = mediaserver::doc;

void check(bool value, const char* reason) {
  if (!value) throw std::runtime_error{reason};
}

[[nodiscard]] bool near(double a, double b) { return std::abs(a - b) < 1.0e-4; }

void transforms() {
  if (!editor::editor_available()) {
    std::puts("[skip] editor GPU backend unavailable");
    return;
  }
  editor::EditorConfig config;
  config.title = "Editor Stage transform regression";
  auto created = editor::EditorShell::create(config);
  check(created.has_value(), "create editor");
  auto shell = std::move(created).value();

  scene::Scene show;
  show.id = scene::SceneId{100U};
  editor::EditorTimeline timeline;
  timeline.last_frame = 600U;
  std::vector<scene::ChannelKeyframes> keyframes;
  editor::ElementInfo object;
  object.node = doc::NodeId{5U};
  object.kind = editor::ElementKind::Object;
  object.display_name = "Box";
  scene::MeshInstance mesh;
  mesh.source = object.node;
  mesh.mesh = {scene::Primitive::Box, 1U};
  mesh.world = mesh.normal = scene::identity();
  show.instances.push_back(mesh);
  std::vector<editor::ElementInfo> elements{object};

  SDL_Window* window = nullptr;
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  for (int i = 0; i < count; ++i)
    if (std::string{SDL_GetWindowTitle(windows[i])} == config.title) window = windows[i];
  SDL_free(windows);
  check(window != nullptr, "find editor window");
  const SDL_WindowID window_id = SDL_GetWindowID(window);

  // The starting workspace, which the first frame reports for saving, solved
  // as the shell solves it, says where Stage is.
  const editor::EditorFrame first = shell->frame(show, timeline, keyframes, elements);
  check(first.save_workspace_layout.has_value(), "first frame reports its workspace");
  gui::DockLayout layout;
  const auto starting = editor::decode_workspace_layout(*first.save_workspace_layout);
  check(starting.has_value(), "decode starting workspace");
  for (const auto& node : starting.value().docks.nodes)
    for (auto id : node.tabs) layout.register_panel(id, "Panel");
  for (const auto& closed : starting.value().docks.closed) layout.register_panel(closed.panel, "Panel");
  check(layout.restore(starting.value().docks), "restore starting workspace");
  const float scale = SDL_GetWindowDisplayScale(window) *
                      static_cast<float>(starting.value().ui_zoom_percent) / 100.0F;
  int width = 0, height = 0;
  check(SDL_GetWindowSizeInPixels(window, &width, &height), "window extent");
  layout.solve({0, 34, static_cast<float>(width) / scale, static_cast<float>(height) / scale - 62},
               {.divider = 6, .seam_reach = 4, .tab_bar = 32, .title_bar = 30, .edge = 0.25F});
  const gui::Rect stage = layout.content_of(layout.leaf_of(gui::ui_id("editor.panel.stage")));
  check(stage.width > 100.0F && stage.height > 100.0F, "Stage is in the starting workspace");
  const float input_scale = scale / SDL_GetWindowPixelDensity(window);
  // Below the Stage toolbar, inside the letterboxed image.
  const float over_x = stage.x + stage.width * 0.5F;
  const float over_y = stage.y + 29.0F + (stage.height - 29.0F) * 0.5F;

  editor::EditorFrame last;
  std::vector<editor::EditIntent> edits;
  std::vector<editor::PropertyEdit> properties;
  std::vector<editor::WorldEdit> worlds;
  const auto frame = [&] {
    last = shell->frame(show, timeline, keyframes, elements);
    check(last.status == editor::EditorStatus::Running, "editor still running");
    edits.insert(edits.end(), last.edits.begin(), last.edits.end());
    properties.insert(properties.end(), last.property_edits.begin(), last.property_edits.end());
    worlds.insert(worlds.end(), last.world_edits.begin(), last.world_edits.end());
  };
  const auto move = [&](float x, float y) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.windowID = window_id;
    event.motion.x = x * input_scale;
    event.motion.y = y * input_scale;
    check(SDL_PushEvent(&event), "push motion");
    frame();
  };
  const auto type = [&](const char* text) {
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.windowID = window_id;
    event.text.text = text;
    check(SDL_PushEvent(&event), "push text");
    frame();
  };
  const auto key = [&](SDL_Keycode code, SDL_Keymod mod = SDL_KMOD_NONE) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.windowID = window_id;
    event.key.key = code;
    event.key.mod = mod;
    event.key.down = true;
    check(SDL_PushEvent(&event), "push key");
    frame();
    event.type = SDL_EVENT_KEY_UP;
    event.key.down = false;
    event.key.mod = SDL_KMOD_NONE;
    check(SDL_PushEvent(&event), "release key");
    frame();
  };
  const auto value_of = [&](scene::TransformChannel component) -> std::optional<double> {
    std::optional<double> value;
    for (const auto& edit : properties)
      if (edit.node == object.node && edit.component == component) value = edit.value;
    return value;
  };

  frame();
  const doc::NodeId selected[]{object.node};
  check(shell->apply_selection_set_recall(show.id, selected), "select box");
  frame();
  move(over_x, over_y);
  frame();

  // G X 2 Enter: two metres along world X, one gesture from G to Enter.
  edits.clear();
  type("g");
  type("x");
  type("2");
  move(over_x + 40.0F, over_y + 25.0F);
  key(SDLK_RETURN);
  check(!edits.empty(), "G authors position edits");
  check(edits.back().node == object.node, "the box moves");
  check(edits.back().position == (scene::Vec3{2.0F, 0.0F, 0.0F}), "typed distance lands exactly on X");
  for (const auto& edit : edits)
    check(edit.gesture == edits.front().gesture, "G to Enter is one undo step");

  // G then Escape puts it back.
  edits.clear();
  move(over_x, over_y);
  type("g");
  move(over_x + 120.0F, over_y - 60.0F);
  check(!edits.empty() && edits.back().position != scene::Vec3{}, "G follows the pointer");
  key(SDLK_ESCAPE);
  check(edits.back().position == scene::Vec3{}, "Escape restores the start");

  // R Z 90 Enter: a quarter turn about world Z, which is a roll of -0.25.
  properties.clear();
  move(over_x, over_y);
  type("r");
  type("z");
  type("90");
  key(SDLK_RETURN);
  check(value_of(scene::TransformChannel::RotationRoll).has_value(), "R authors rotation");
  check(near(*value_of(scene::TransformChannel::RotationRoll), -0.25), "roll is a quarter turn");
  check(near(*value_of(scene::TransformChannel::RotationYaw), 0.0), "yaw untouched");
  check(near(*value_of(scene::TransformChannel::RotationPitch), 0.0), "pitch untouched");

  // S Shift+Z 3 Enter: X and Y three times, Z kept.
  properties.clear();
  type("s");
  type("Z");
  type("3");
  key(SDLK_RETURN);
  check(near(value_of(scene::TransformChannel::ScaleX).value_or(0.0), 3.0), "scale X");
  check(near(value_of(scene::TransformChannel::ScaleY).value_or(0.0), 3.0), "scale Y");
  check(near(value_of(scene::TransformChannel::ScaleZ).value_or(0.0), 1.0), "scale Z kept");

  // Alt+G clears position; Alt+S clears scale.
  edits.clear();
  properties.clear();
  key(SDLK_G, SDL_KMOD_LALT);
  check(!edits.empty() && edits.back().position == scene::Vec3{}, "Alt+G clears position");
  key(SDLK_S, SDL_KMOD_LALT);
  check(near(value_of(scene::TransformChannel::ScaleX).value_or(0.0), 1.0), "Alt+S clears scale");

  // A Blender-linked object is placed by one world matrix: R and S reach it as
  // that matrix, never as yaw/pitch/roll or scale fields it would ignore.
  editor::ElementInfo linked;
  linked.node = doc::NodeId{6U};
  linked.kind = editor::ElementKind::Model;
  linked.linked = true;
  linked.display_name = "Linked chair";
  scene::ModelInstance chair;
  chair.source_node = linked.node;
  chair.world = scene::translation(scene::Vec3{1.0F, 0.0F, 0.0F});
  show.models.push_back(chair);
  elements.push_back(linked);
  const doc::NodeId linked_selection[]{linked.node};
  check(shell->apply_selection_set_recall(show.id, linked_selection), "select linked chair");
  frame();
  move(over_x, over_y);
  properties.clear();
  worlds.clear();
  type("r");
  type("z");
  type("90");
  key(SDLK_RETURN);
  check(!worlds.empty() && worlds.back().node == linked.node, "R on a linked object authors its placement");
  const scene::Mat4 turned = worlds.back().world;
  // A quarter turn about +Z, about its own origin: X now points along +Y.
  check(near(turned.at(0U, 0U), 0.0) && near(turned.at(1U, 0U), 1.0), "linked X axis turned to +Y");
  check(near(turned.at(0U, 3U), 1.0) && near(turned.at(1U, 3U), 0.0), "linked origin stays put");
  for (const auto& edit : properties)
    check(edit.node != linked.node, "no transform fields for a linked object");
  worlds.clear();
  type("s");
  type("2");
  key(SDLK_RETURN);
  check(!worlds.empty() && near(worlds.back().world.at(2U, 2U), 2.0), "S on a linked object scales its placement");
  worlds.clear();
  key(SDLK_R, SDL_KMOD_LALT);
  check(!worlds.empty() && near(worlds.back().world.at(0U, 0U), 1.0), "Alt+R clears a linked rotation");

  // Off the Stage, G is not a transform.
  edits.clear();
  move(stage.x - 40.0F, stage.y - 20.0F);
  type("g");
  type("2");
  key(SDLK_RETURN);
  check(edits.empty(), "G away from Stage authors nothing");
}
} // namespace

int main() {
  try {
    transforms();
    std::puts("[pass] Stage Blender transform keys");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[fail] %s\n", e.what());
    return 1;
  }
}
