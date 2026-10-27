#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/editor/stage_view.hpp>
#include <mediaserver/editor/workspace_layout.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/math.hpp>

#include <SDL3/SDL.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

// The Stage's views through the shipping shell, with real SDL input: Alt+J
// turns the one view to Top and a click there picks what the plan shows under
// it; the view menu splits the Stage into Top, Front, Side and Perspective,
// and a click in each picks the element that view shows under the pointer.
// Each pick is observed as Alt+G clearing that element's position.
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

// Where `point` lands in `image`, through `camera` over a canvas of the
// image's shape. A canvas sixteen times the logical size keeps the integer
// extents' aspect within a sixteenth of a pixel of the image's.
[[nodiscard]] std::array<float, 2U> on_screen(const scene::Camera &camera,
                                              const gui::Rect &image,
                                              scene::Vec3 point) {
  const scene::Canvas canvas{
      static_cast<mediaserver::core::u32>(std::lround(image.width * 16.0F)),
      static_cast<mediaserver::core::u32>(std::lround(image.height * 16.0F))};
  const auto projected = scene::project_to_canvas(camera, canvas, point);
  check(projected.visible, "the element is in front of the view");
  return {image.x + projected.x / 16.0F, image.y + projected.y / 16.0F};
}

void stage_views() {
  if (!editor::editor_available()) {
    std::puts("[skip] editor GPU backend unavailable");
    return;
  }
  gui::DockLayout layout;
  const auto stage_panel = gui::ui_id("editor.panel.stage");
  static_cast<void>(layout.add(stage_panel, "Stage"));
  const auto workspace = editor::encode_workspace_layout(
      editor::WorkspaceLayoutState{layout.snapshot(), 1600U, 900U});
  check(workspace.has_value(), "encode Stage workspace");
  editor::EditorConfig config;
  config.title = "Stage views regression";
  config.workspace_layout = workspace.value();
  auto created = editor::EditorShell::create(config);
  check(created.has_value(), "create editor");
  auto shell = std::move(created).value();

  // Four boxes, each where one view can single it out: apart across the
  // floor, at different heights, and in front of and behind one another.
  struct Placed final {
    doc::NodeId node;
    scene::Vec3 centre;
  };
  const std::array<Placed, 4U> boxes{{
      {doc::NodeId{11U}, {-3.0F, 0.5F, -2.0F}},
      {doc::NodeId{12U}, {2.5F, 2.5F, 1.5F}},
      {doc::NodeId{13U}, {0.5F, -2.0F, 3.0F}},
      {doc::NodeId{14U}, {-1.5F, 3.0F, -3.5F}},
  }};
  scene::Scene show;
  show.id = scene::SceneId{100U};
  std::vector<editor::ElementInfo> elements;
  for (const Placed &box : boxes) {
    scene::MeshInstance mesh;
    mesh.source = box.node;
    mesh.mesh = {scene::Primitive::Box, 1U};
    mesh.world = scene::translation(box.centre);
    mesh.normal = scene::identity();
    show.instances.push_back(mesh);
    editor::ElementInfo element;
    element.node = box.node;
    element.kind = editor::ElementKind::Object;
    element.display_name = "Box";
    elements.push_back(element);
  }
  editor::EditorTimeline timeline;
  timeline.last_frame = 600U;
  const std::vector<scene::ChannelKeyframes> keys;
  mediaserver::assets::AssetRegistry registry;
  editor::EditorSwitcherState switcher;

  std::vector<editor::EditIntent> edits;
  const auto frame = [&] {
    auto result = shell->frame(show, timeline, keys, elements, {}, {}, {},
                               switcher, registry);
    check(result.status == editor::EditorStatus::Running, "editor running");
    edits.insert(edits.end(), result.edits.begin(), result.edits.end());
  };
  frame();
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
  const gui::Rect stage = layout.content_of(layout.leaf_of(stage_panel));
  const auto window_id = SDL_GetWindowID(window);

  const auto move = [&](float x, float y) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.windowID = window_id;
    event.motion.x = x * input_scale;
    event.motion.y = y * input_scale;
    check(SDL_PushEvent(&event), "move pointer");
    frame();
  };
  const auto click = [&](float x, float y) {
    move(x, y);
    for (const bool down : {true, false}) {
      SDL_Event event{};
      event.type =
          down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
      event.button.windowID = window_id;
      event.button.button = SDL_BUTTON_LEFT;
      event.button.down = down;
      event.button.x = x * input_scale;
      event.button.y = y * input_scale;
      check(SDL_PushEvent(&event), "pointer button");
      frame();
    }
  };
  const auto key = [&](SDL_Keycode code, SDL_Keymod mod) {
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
  // A click on the element's centre picks it; Alt+G then clears its
  // position, which names what was picked.
  const auto picks = [&](const scene::Camera &camera, const gui::Rect &image,
                         const Placed &box) {
    const auto at = on_screen(camera, image, box.centre);
    check(at[0] > image.x + 4.0F && at[0] < image.x + image.width - 4.0F &&
              at[1] > image.y + 32.0F &&
              at[1] < image.y + image.height - 4.0F,
          "the element is inside its view, clear of the view menu");
    // Nothing selected first, so no gizmo stands over the click.
    check(shell->apply_selection_set_recall(show.id, {}), "clear selection");
    frame();
    click(at[0], at[1]);
    edits.clear();
    key(SDLK_G, SDL_KMOD_LALT);
    return !edits.empty() && edits.back().node == box.node &&
           edits.back().position == scene::Vec3{};
  };

  constexpr float toolbar = 29.0F;
  const gui::Rect whole{stage.x, stage.y + toolbar, stage.width,
                        stage.height - toolbar};
  const scene::Camera authored{};
  editor::StageOrbit orbit{};

  // Alt+J over the Stage makes the one view a plan: a click picks the box the
  // plan shows there.
  move(whole.x + whole.width * 0.5F, whole.y + whole.height * 0.5F);
  key(SDLK_J, SDL_KMOD_LALT);
  orbit.projection = editor::StageProjection::Top;
  check(picks(editor::stage_camera_for(authored, orbit), whole, boxes[0]),
        "Alt+J shows Top, and a click there picks what it shows");
  // Alt+H, Front.
  key(SDLK_H, SDL_KMOD_LALT);
  orbit.projection = editor::StageProjection::Front;
  check(picks(editor::stage_camera_for(authored, orbit), whole, boxes[1]),
        "Alt+H shows Front");
  // Alt+P returns to the perspective view as it was.
  key(SDLK_P, SDL_KMOD_LALT);
  orbit.projection = editor::StageProjection::Perspective;
  check(picks(editor::stage_camera_for(authored, orbit), whole, boxes[3]),
        "Alt+P returns to Perspective");

  // The view menu, in the image's top-right corner, splits the Stage.
  const gui::Rect menu{whole.x + whole.width - 110.0F, whole.y + 6.0F, 104.0F,
                       22.0F};
  click(menu.x + menu.width * 0.5F, menu.y + menu.height * 0.5F);
  // The fifth entry, below the four projections.
  click(menu.x + 30.0F, menu.y + menu.height + 2.0F + 24.0F * 4.0F + 12.0F);
  frame();

  constexpr float gap = 2.0F;
  const float half_width = (whole.width - gap) * 0.5F;
  const float half_height = (whole.height - gap) * 0.5F;
  struct Quadrant final {
    gui::Rect image;
    editor::StageProjection projection;
    const Placed *box;
  };
  const std::array<Quadrant, 4U> quadrants{{
      {{whole.x, whole.y, half_width, half_height},
       editor::StageProjection::Top, &boxes[2]},
      {{whole.x + half_width + gap, whole.y, half_width, half_height},
       editor::StageProjection::Front, &boxes[0]},
      {{whole.x, whole.y + half_height + gap, half_width, half_height},
       editor::StageProjection::Side, &boxes[1]},
      {{whole.x + half_width + gap, whole.y + half_height + gap, half_width,
        half_height},
       editor::StageProjection::Perspective, &boxes[3]},
  }};
  for (const Quadrant &quadrant : quadrants) {
    orbit.projection = quadrant.projection;
    check(picks(editor::stage_camera_for(authored, orbit), quadrant.image,
                *quadrant.box),
          "each of the four views picks what it shows under the pointer");
  }
  // A view's own keys change that view alone: the Side view becomes a
  // second Top, and still picks through its new projection.
  const Quadrant &side = quadrants[2];
  move(side.image.x + side.image.width * 0.5F,
       side.image.y + side.image.height * 0.5F);
  key(SDLK_J, SDL_KMOD_LALT);
  orbit.projection = editor::StageProjection::Top;
  check(picks(editor::stage_camera_for(authored, orbit), side.image,
              boxes[0]),
        "a key over one view changes only that view");
  orbit.projection = editor::StageProjection::Front;
  check(picks(editor::stage_camera_for(authored, orbit),
              quadrants[1].image, boxes[1]),
        "the other views keep their projections");
}
} // namespace

int main() {
  try {
    stage_views();
    std::puts("[pass] each Stage view projects and picks through its own camera");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "[fail] %s\n", error.what());
    return 1;
  }
}
