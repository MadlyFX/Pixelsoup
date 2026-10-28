#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/editor/stage_view.hpp>
#include <mediaserver/editor/workspace_layout.hpp>
#include <mediaserver/scene/math.hpp>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

// A Library asset dragged onto the Stage through the shipping docked shell,
// with real SDL input: it rests on the surface under the pointer, a screen
// stands on its bottom edge and turns to face the Stage camera, and nothing is
// authored until the release. A Background generator dragged out the same way
// is placed like a screen.
namespace {
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace scene = mediaserver::scene;
namespace doc = mediaserver::doc;

void check(bool value, const char *reason) {
  if (!value) {
    throw std::runtime_error{reason};
  }
}

[[nodiscard]] bool near(float a, float b, float tolerance = 1.0e-3F) {
  return std::abs(a - b) <= tolerance;
}

void stage_drop() {
  if (!editor::editor_available()) {
    std::puts("[skip] editor GPU backend unavailable");
    return;
  }
  gui::DockLayout layout;
  const auto stage_panel = gui::ui_id("editor.panel.stage");
  const auto library_panel = gui::ui_id("editor.panel.library");
  const auto stage_leaf = layout.add(stage_panel, "Stage");
  static_cast<void>(layout.dock(library_panel, "Library", stage_leaf,
                                gui::DockSide::Left));
  const auto workspace = editor::encode_workspace_layout(
      editor::WorkspaceLayoutState{layout.snapshot(), 1600U, 900U});
  check(workspace.has_value(), "encode Library-and-Stage workspace");
  editor::EditorConfig config;
  config.title = "Stage drop regression";
  config.workspace_layout = workspace.value();
  auto created = editor::EditorShell::create(config);
  check(created.has_value(), "create editor");
  auto shell = std::move(created).value();

  scene::Scene show;
  show.id = scene::SceneId{100U};
  // A wide platform whose top is half a metre above the stage floor, locked
  // as set pieces usually are: it is still something to stand things on.
  scene::MeshInstance platform;
  platform.source = doc::NodeId{9U};
  platform.mesh = {scene::Primitive::Box, 1U};
  platform.world = scene::scaling(scene::Vec3{40.0F, 1.0F, 40.0F});
  platform.normal = scene::identity();
  platform.locked = true;
  show.instances.push_back(platform);
  editor::EditorTimeline timeline;
  timeline.last_frame = 600U;
  const std::vector<scene::ChannelKeyframes> keys;
  std::vector<editor::ElementInfo> elements(1U);
  elements[0U].node = platform.source;
  elements[0U].kind = editor::ElementKind::Object;
  elements[0U].display_name = "Platform";
  elements[0U].locked = true;

  mediaserver::assets::AssetRegistry registry;
  mediaserver::assets::MediaAsset media;
  media.id = doc::NodeId{71U};
  media.display_name = "Dropped video";
  media.source = "T:/dropped.mov";
  media.dimensions = mediaserver::assets::PixelDimensions{2000U, 1000U};
  registry.assets.emplace_back(media);
  mediaserver::assets::ModelAsset model;
  model.id = doc::NodeId{81U};
  model.display_name = "Dropped model";
  model.source = "T:/dropped.msmodel";
  registry.assets.emplace_back(model);
  editor::EditorSwitcherState switcher;

  const auto frame = [&] {
    auto result = shell->frame(show, timeline, keys, elements, {}, {}, {},
                               switcher, registry);
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
  layout.solve(editor::editor_dock_area(static_cast<float>(width) / scale,
                                          static_cast<float>(height) / scale),
               {.divider = 6, .seam_reach = 4, .tab_bar = 32,
                .title_bar = 30, .edge = 0.25F});
  const auto stage = layout.content_of(layout.leaf_of(stage_panel));
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
  // Just below the middle of the image, under the Stage toolbar: the
  // default free view looks down at the origin, so this is on the platform.
  const float drop_x = stage.x + stage.width * 0.5F;
  const float drop_y = stage.y + 29.0F + (stage.height - 29.0F) * 0.6F;
  const auto drag = [&](float sx, float sy) {
    check(pointer(sx, sy, true).adds.empty(), "press authors nothing");
    check(pointer(drop_x, drop_y, true).adds.empty(),
          "hovering the Stage previews without authoring");
    const auto released = pointer(drop_x, drop_y, false);
    check(frame().adds.empty(), "release authors once");
    return released;
  };
  const scene::Vec3 eye = editor::stage_eye(editor::StageOrbit{});

  // The Background generator, the fourth Generators row (92 + 3 x 22 ..),
  // is placed like a screen: it stands on the platform facing the camera.
  const auto background = drag(library.x + 80.0F, library.y + 169.0F);
  check(background.adds.size() == 1U, "the Background is added");
  const editor::AddIntent &card = background.adds.front();
  check(card.kind == editor::AddKind::Background,
        "the dropped generator is a Background");
  check(card.position.has_value() && card.rotation.has_value(),
        "a Stage drop places and turns the Background");
  check(near(card.position->y, 1.5F), "the Background stands on the platform");
  {
    const scene::Mat4 turned = scene::rotation(card.rotation->x, 0.0F, 0.0F);
    const scene::Vec3 face{turned.at(0U, 2U), 0.0F, turned.at(2U, 2U)};
    const scene::Vec3 toward = scene::normalize(scene::Vec3{
        eye.x - card.position->x, 0.0F, eye.z - card.position->z});
    check(scene::dot(face, toward) > 0.999F,
          "the Background faces the camera");
  }

  // Filter the Library to the two assets. The List view sorts them by name:
  // the model's row is 70..103, the video's 103..136.
  static_cast<void>(pointer(library.x + 75.0F, library.y + 37.0F, true));
  static_cast<void>(pointer(library.x + 75.0F, library.y + 37.0F, false));
  SDL_Event text{};
  text.type = SDL_EVENT_TEXT_INPUT;
  text.text.windowID = window_id;
  text.text.text = "Dropped";
  check(SDL_PushEvent(&text), "filter Library");
  static_cast<void>(frame());
  const float asset_x = library.x + 80.0F;
  const float media_y = library.y + 120.0F;
  const float model_y = library.y + 86.0F;

  const auto video = drag(asset_x, media_y);
  check(video.adds.size() == 1U, "the video is added");
  const editor::AddIntent &screen = video.adds.front();
  check(screen.kind == editor::AddKind::Media && screen.asset == media.id,
        "the dropped video, not another asset");
  check(screen.position.has_value() && screen.rotation.has_value(),
        "a Stage drop places and turns the screen");
  // Two metres tall, so its centre is a metre above the platform's top.
  check(near(screen.position->y, 1.5F), "the screen stands on the platform");
  check(near(screen.rotation->y, 0.0F) && near(screen.rotation->z, 0.0F),
        "the screen stays upright");
  const scene::Mat4 turned = scene::rotation(screen.rotation->x, 0.0F, 0.0F);
  const scene::Vec3 face{turned.at(0U, 2U), 0.0F, turned.at(2U, 2U)};
  const scene::Vec3 toward = scene::normalize(scene::Vec3{
      eye.x - screen.position->x, 0.0F, eye.z - screen.position->z});
  check(scene::dot(face, toward) > 0.999F, "the screen faces the camera");

  const auto set_piece = drag(asset_x, model_y);
  check(set_piece.adds.size() == 1U && set_piece.adds.front().asset == model.id,
        "the model is added");
  const editor::AddIntent &placed = set_piece.adds.front();
  // An undecodable model has no bounds: its origin sits on the platform, in
  // its authored orientation.
  check(placed.position.has_value() && near(placed.position->y, 0.5F),
        "the model's origin rests on the platform");
  check(!placed.rotation.has_value(), "the model keeps its orientation");

  // Without the platform, the stage floor carries it.
  show.instances.clear();
  elements.clear();
  static_cast<void>(frame());
  const auto on_floor = drag(asset_x, media_y);
  check(on_floor.adds.size() == 1U && on_floor.adds.front().position.has_value(),
        "the video is added on the floor");
  check(near(on_floor.adds.front().position->y, 1.0F),
        "the screen stands on the stage floor");
}
} // namespace

int main() {
  try {
    stage_drop();
    std::puts("[pass] a Library asset dropped on Stage rests on the surface under the pointer");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "[fail] %s\n", error.what());
    return 1;
  }
}
