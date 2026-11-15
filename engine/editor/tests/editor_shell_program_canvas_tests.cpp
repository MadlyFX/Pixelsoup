#include <mediaserver/editor/canvas_tool.hpp>
#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/editor/workspace_layout.hpp>
#include <mediaserver/gui/dock.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

// The Program view's 2D tools through the shipping shell: a real editor
// window, fed SDL events as the platform feeds it. Proves a box handle, the
// options bar and the tool keys reach the document as the intents the
// Director receives - the part the panel tests cannot see.
namespace {
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace scene = mediaserver::scene;
namespace doc = mediaserver::doc;

void check(bool value, const char *reason) {
  if (!value) throw std::runtime_error{reason};
}

[[nodiscard]] bool near(double a, double b, double tolerance = 0.02) {
  return std::abs(a - b) < tolerance;
}

void program_tools() {
  if (!editor::editor_available()) {
    std::puts("[skip] editor GPU backend unavailable");
    return;
  }
  // Program Output alone, so its place in the window is known.
  gui::DockLayout layout;
  const auto program_panel = gui::ui_id("editor.panel.program");
  static_cast<void>(layout.add(program_panel, "Program Output"));
  const auto workspace = editor::encode_workspace_layout(
      editor::WorkspaceLayoutState{layout.snapshot(), 1600U, 900U});
  check(workspace.has_value(), "encode Program workspace");
  editor::EditorConfig config;
  config.title = "Editor Program canvas regression";
  config.workspace_layout = workspace.value();
  auto created = editor::EditorShell::create(config);
  check(created.has_value(), "create editor");
  auto shell = std::move(created).value();

  // A flat two-by-two card facing a camera eight units back.
  scene::Scene show;
  show.id = scene::SceneId{100U};
  show.canvas = scene::Canvas{1920U, 1080U};
  show.camera.eye = scene::Vec3{0.0F, 0.0F, 8.0F};
  show.camera.target = scene::Vec3{};
  show.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
  editor::EditorTimeline timeline;
  timeline.last_frame = 600U;
  std::vector<scene::ChannelKeyframes> keyframes;
  editor::ElementInfo card;
  card.node = doc::NodeId{5U};
  card.kind = editor::ElementKind::Object;
  card.display_name = "Card";
  card.scale = scene::Vec3{2.0F, 2.0F, 1.0F};
  scene::MeshInstance mesh;
  mesh.source = card.node;
  mesh.mesh = {scene::Primitive::Quad, 1U};
  mesh.world = scene::scaling(card.scale);
  mesh.normal = scene::identity();
  show.instances.push_back(mesh);
  std::vector<editor::ElementInfo> elements{card};

  SDL_Window *window = nullptr;
  int count = 0;
  SDL_Window **windows = SDL_GetWindows(&count);
  for (int i = 0; i < count; ++i)
    if (std::string{SDL_GetWindowTitle(windows[i])} == config.title)
      window = windows[i];
  SDL_free(windows);
  check(window != nullptr, "find editor window");
  const SDL_WindowID window_id = SDL_GetWindowID(window);
  const float scale = SDL_GetWindowDisplayScale(window);
  const float input_scale = scale / SDL_GetWindowPixelDensity(window);
  int width = 0, height = 0;
  check(SDL_GetWindowSizeInPixels(window, &width, &height), "window extent");
  layout.solve(editor::editor_dock_area(static_cast<float>(width) / scale,
                                        static_cast<float>(height) / scale),
               {.divider = 6, .seam_reach = 4, .tab_bar = 32, .title_bar = 30,
                .edge = 0.25F});
  const gui::Rect panel = layout.content_of(layout.leaf_of(program_panel));
  check(panel.width > 300.0F && panel.height > 300.0F,
        "Program Output fills the workspace");

  // The panel's own furniture: an output bar and an options bar above the
  // picture, the tools down its left edge, a footer beneath.
  const gui::Rect options{panel.x, panel.y + 26.0F, panel.width, 26.0F};
  const gui::Rect pane{panel.x + editor::canvas_tool_palette_width,
                       panel.y + 52.0F,
                       panel.width - editor::canvas_tool_palette_width,
                       panel.height - 52.0F - 24.0F};
  // The 16:9 picture, fitted into that pane.
  float picture_width = pane.width;
  float picture_height = picture_width * 9.0F / 16.0F;
  if (picture_height > pane.height) {
    picture_height = pane.height;
    picture_width = picture_height * 16.0F / 9.0F;
  }
  const gui::Rect picture{pane.x + (pane.width - picture_width) * 0.5F,
                          pane.y + (pane.height - picture_height) * 0.5F,
                          picture_width, picture_height};
  // Where a point of the world lands in the window.
  const auto on_screen = [&](scene::Vec3 point) {
    const scene::CanvasProjection at =
        scene::project_to_canvas(show.camera, show.canvas, point);
    check(at.visible, "the point is in front of the camera");
    return gui::UiPoint{picture.x + at.x / 1920.0F * picture.width,
                        picture.y + at.y / 1080.0F * picture.height};
  };

  editor::EditorFrame last;
  std::vector<editor::EditIntent> edits;
  std::vector<editor::PropertyEdit> properties;
  std::vector<editor::MaskEditIntent> mask_edits;
  std::vector<editor::AddIntent> adds;
  std::vector<editor::GenericEdit> generics;
  editor::EditorSwitcherState switcher;
  const auto frame = [&] {
    last = shell->frame(show, timeline, keyframes, elements, {}, {}, {},
                        switcher);
    check(last.status == editor::EditorStatus::Running, "editor still running");
    edits.insert(edits.end(), last.edits.begin(), last.edits.end());
    properties.insert(properties.end(), last.property_edits.begin(),
                      last.property_edits.end());
    if (last.mask_edit.has_value()) mask_edits.push_back(*last.mask_edit);
    adds.insert(adds.end(), last.adds.begin(), last.adds.end());
    generics.insert(generics.end(), last.generic_edits.begin(),
                    last.generic_edits.end());
  };
  const auto move = [&](gui::UiPoint to) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.windowID = window_id;
    event.motion.x = to.x * input_scale;
    event.motion.y = to.y * input_scale;
    check(SDL_PushEvent(&event), "push motion");
    frame();
  };
  const auto button = [&](gui::UiPoint at, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.windowID = window_id;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = down;
    event.button.x = at.x * input_scale;
    event.button.y = at.y * input_scale;
    check(SDL_PushEvent(&event), "push button");
    frame();
  };
  const auto type = [&](const char *text) {
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.windowID = window_id;
    event.text.text = text;
    check(SDL_PushEvent(&event), "push text");
    frame();
  };
  const auto value_of =
      [&](scene::TransformChannel component) -> std::optional<double> {
    std::optional<double> value;
    for (const auto &edit : properties)
      if (edit.node == card.node && edit.component == component)
        value = edit.value;
    return value;
  };

  // A few frames for Program's first picture, then the card selected.
  for (int warm = 0; warm < 8; ++warm) frame();
  const doc::NodeId selected[]{card.node};
  check(shell->apply_selection_set_recall(show.id, selected), "select card");
  frame();
  frame();

  // The card's bottom-right corner, dragged half a card further right and a
  // quarter further down: the box resizes against its top-left corner.
  const gui::UiPoint corner = on_screen(scene::Vec3{1.0F, -1.0F, 0.0F});
  const gui::UiPoint beyond = on_screen(scene::Vec3{2.0F, -1.5F, 0.0F});
  move(corner);
  edits.clear();
  properties.clear();
  button(corner, true);
  move(beyond);
  button(beyond, false);
  check(value_of(scene::TransformChannel::ScaleX).has_value(),
        "a corner drag authors scale");
  check(near(*value_of(scene::TransformChannel::ScaleX), 3.0),
        "half a card wider is three units across");
  check(near(*value_of(scene::TransformChannel::ScaleY), 2.5),
        "a quarter taller is two and a half units down");
  check(near(*value_of(scene::TransformChannel::ScaleZ), 1.0),
        "a flat card keeps its depth");
  check(!edits.empty() && edits.back().node == card.node,
        "the card's middle moves as its far corner stays");
  check(near(edits.back().position.x, 0.5) &&
            near(edits.back().position.y, -0.25) &&
            near(edits.back().position.z, 0.0),
        "the top-left corner has not moved");
  for (const auto &edit : properties)
    check(edit.gesture == properties.front().gesture,
          "one drag is one undo step");
  for (const auto &edit : edits)
    check(edit.gesture == properties.front().gesture,
          "its move belongs to the same step");

  // The Hand tool (H over the picture): the same drag over the card moves
  // the view and authors nothing.
  const gui::UiPoint middle = on_screen(scene::Vec3{});
  move(middle);
  type("h");
  edits.clear();
  properties.clear();
  button(middle, true);
  move(gui::UiPoint{middle.x + 50.0F, middle.y + 20.0F});
  button(gui::UiPoint{middle.x + 50.0F, middle.y + 20.0F}, false);
  check(edits.empty() && properties.empty(), "the Hand tool authors nothing");

  // F fits the picture again and V takes Select back: a drag on the card
  // itself then moves it.
  type("f");
  type("v");
  move(middle);
  edits.clear();
  button(middle, true);
  move(gui::UiPoint{middle.x + 40.0F, middle.y});
  button(gui::UiPoint{middle.x + 40.0F, middle.y}, false);
  check(!edits.empty(), "a drag on the card moves it");
  check(edits.back().position.x > 0.05 &&
            near(edits.back().position.y, 0.0, 1.0e-3),
        "to the right, and no other way");

  // The options bar's X, scrubbed thirty units right: the card moves thirty
  // canvas pixels along X and not at all in Y.
  const double pixels_per_unit =
      (scene::project_to_canvas(show.camera, show.canvas,
                                scene::Vec3{1.0F, 0.0F, 0.0F})
           .x -
       scene::project_to_canvas(show.camera, show.canvas, scene::Vec3{}).x);
  const gui::UiPoint field{options.x + 8.0F + 39.0F, options.y + 13.0F};
  move(field);
  edits.clear();
  properties.clear();
  button(field, true);
  move(gui::UiPoint{field.x + 30.0F, field.y});
  button(gui::UiPoint{field.x + 30.0F, field.y}, false);
  check(!edits.empty() && edits.back().node == card.node,
        "the X field moves the card");
  check(near(edits.back().position.x, 30.0 / pixels_per_unit, 0.005),
        "thirty canvas pixels along X");
  check(near(edits.back().position.y, 0.0, 1.0e-3), "and none along Y");
  check(properties.empty(), "a move authors no scale or rotation");

  // Align, at the right of the options bar: with one layer selected, Align
  // left puts its left side on the picture's left edge, and moves it no
  // other way.
  const gui::UiPoint align_button{options.right() - 114.0F, options.y + 13.0F};
  const gui::UiPoint align_left{options.right() - 120.0F, options.y + 40.0F};
  move(align_button);
  edits.clear();
  properties.clear();
  button(align_button, true);
  button(align_button, false);
  move(align_left);
  button(align_left, true);
  button(align_left, false);
  frame();
  check(!edits.empty() && edits.back().node == card.node,
        "Align left moves the card");
  // The card is two units wide, and the picture 1920 pixels.
  check(near(edits.back().position.x, 1.0 - 960.0 / pixels_per_unit, 0.005),
        "its left side is on the picture's left edge");
  check(near(edits.back().position.y, 0.0, 1.0e-3) && properties.empty(),
        "and nothing else about it changed");
  for (const auto &edit : edits)
    check(edit.gesture == edits.front().gesture, "one choice is one undo step");

  // The Gradient tool (G): the card is given a ramp from left to right,
  // whose end is the middle of its right-hand side. Dragged to the middle
  // of the bottom, the ramp runs top to bottom - its angle a quarter turn,
  // its middle and length as they were - and the card does not move.
  scene::Fill ramp;
  ramp.mode = scene::FillMode::Linear;
  show.instances[0U].material.fill = ramp;
  move(middle);
  type("g");
  const gui::UiPoint ramp_end = on_screen(scene::Vec3{1.0F, 0.0F, 0.0F});
  const gui::UiPoint ramp_down = on_screen(scene::Vec3{0.0F, -1.0F, 0.0F});
  move(ramp_end);
  edits.clear();
  properties.clear();
  generics.clear();
  button(ramp_end, true);
  move(ramp_down);
  button(ramp_down, false);
  check(!generics.empty(), "dragging the ramp's end edits the fill");
  std::optional<double> angle;
  for (const auto &edit : generics) {
    check(edit.node == card.node, "the selected card's fill");
    check(edit.gesture == generics.front().gesture,
          "one drag is one undo step");
    check(edit.property != scene::schema::fill_center_x &&
              edit.property != scene::schema::fill_center_y,
          "its middle stays where it was");
    if (edit.property == scene::schema::fill_angle_turns) angle = edit.value;
  }
  check(angle.has_value() && near(*angle, 0.25, 0.005),
        "the ramp now runs top to bottom");
  check(edits.empty() && properties.empty(),
        "the Gradient tool moves no layer");
  show.instances[0U].material.fill.reset();

  // The Rectangle tool (R): a drag across the selected card is a mask of
  // it, in the card's own coordinates, and moves nothing.
  move(middle);
  type("r");
  const gui::UiPoint mask_from = on_screen(scene::Vec3{-0.5F, 0.5F, 0.0F});
  const gui::UiPoint mask_to = on_screen(scene::Vec3{0.5F, -0.5F, 0.0F});
  move(mask_from);
  edits.clear();
  properties.clear();
  button(mask_from, true);
  move(mask_to);
  button(mask_to, false);
  check(mask_edits.size() == 1U, "one drag draws one mask");
  const auto *const drawn =
      std::get_if<editor::MaskDrawIntent>(&mask_edits.front().request);
  check(drawn != nullptr && drawn->target == card.node &&
            drawn->scene == show.id,
        "the mask is drawn on the selected card");
  const auto *const rectangle =
      std::get_if<scene::RectangleMaskShape>(&drawn->shape);
  check(rectangle != nullptr, "a rectangle was drawn");
  check(near(rectangle->bounds.left, 0.25) &&
            near(rectangle->bounds.top, 0.25) &&
            near(rectangle->bounds.right, 0.75) &&
            near(rectangle->bounds.bottom, 0.75),
        "the middle half of the card, each way");
  check(edits.empty() && properties.empty() && adds.empty(),
        "drawing a mask moves and makes nothing else");

  // The Director authors that mask and asks for it to be selected. With
  // Select (V), Program then edits the mask rather than the card: a drag
  // on it moves the mask.
  scene::MaskInstance authored;
  authored.source = doc::NodeId{50U};
  authored.scene = show.id;
  authored.target = card.node;
  authored.kind = scene::MaskKind::Rectangle;
  authored.bounds = scene::MaskBounds{0.25F, 0.25F, 0.75F, 0.75F};
  show.masks.push_back(authored);
  check(shell->apply_mask_selection(show.id, card.node, authored.source),
        "select the drawn mask");
  move(middle);
  type("v");
  frame();
  mask_edits.clear();
  edits.clear();
  button(middle, true);
  move(gui::UiPoint{middle.x + 30.0F, middle.y});
  button(gui::UiPoint{middle.x + 30.0F, middle.y}, false);
  check(!mask_edits.empty(), "a drag on the selected mask edits the mask");
  const auto *const moved = std::get_if<editor::MaskViewportTranslateIntent>(
      &mask_edits.back().request);
  check(moved != nullptr && moved->mask == authored.source &&
            moved->delta_x > 0.01F && near(moved->delta_y, 0.0, 1.0e-3),
        "to the right, by the mask's own id");
  check(edits.empty() && properties.empty(),
        "the card under the mask stays where it is");

  // A click away from the mask goes back to the card: the same drag then
  // moves the card and leaves the mask alone.
  const gui::UiPoint away = on_screen(scene::Vec3{-3.0F, 0.0F, 0.0F});
  move(away);
  button(away, true);
  button(away, false);
  move(middle);
  mask_edits.clear();
  edits.clear();
  button(middle, true);
  move(gui::UiPoint{middle.x + 30.0F, middle.y});
  button(gui::UiPoint{middle.x + 30.0F, middle.y}, false);
  check(mask_edits.empty(), "the mask is no longer being edited");
  check(!edits.empty() && edits.back().position.x > 0.05,
        "the card moves again");
  show.masks.clear();

  // The Ellipse tool (E) on bare picture, with nothing selected: a new
  // card where it was drawn, the size drawn, born with an ellipse mask.
  move(away);
  button(away, true);
  button(away, false);
  type("e");
  const gui::UiPoint card_from = on_screen(scene::Vec3{-4.0F, 2.0F, 0.0F});
  const gui::UiPoint card_to = on_screen(scene::Vec3{-2.0F, 1.0F, 0.0F});
  move(card_from);
  adds.clear();
  mask_edits.clear();
  button(card_from, true);
  move(card_to);
  button(card_to, false);
  check(adds.size() == 1U && mask_edits.empty(),
        "bare picture takes a new card, not a mask on something else");
  const editor::AddIntent &made = adds.front();
  check(made.kind == editor::AddKind::Background && made.select &&
            made.scene == show.id && made.name == "Ellipse",
        "a Background card named for its shape, to be selected");
  check(made.scale.has_value() && near(made.scale->x, 2.0) &&
            near(made.scale->y, 1.0) && near(made.scale->z, 1.0),
        "two units wide and one tall, as drawn");
  check(made.position.has_value() && near(made.position->x, -3.0) &&
            near(made.position->y, 1.5) && near(made.position->z, 0.0),
        "standing where it was drawn, on the plane the camera looks at");
  check(made.rotation.has_value() && near(made.rotation->x, 0.0) &&
            near(made.rotation->y, 0.0) && near(made.rotation->z, 0.0),
        "facing the camera");
  check(made.mask.has_value() &&
            std::holds_alternative<scene::EllipseMaskShape>(*made.mask),
        "with the ellipse as its mask");

  // The Rectangle tool there needs no mask: the card is the rectangle.
  type("r");
  adds.clear();
  move(card_from);
  button(card_from, true);
  move(card_to);
  button(card_to, false);
  check(adds.size() == 1U && !adds.front().mask.has_value() &&
            adds.front().name == "Rectangle",
        "a drawn rectangle is the card itself");
  move(middle);
  type("v");

  // Preview: the Stage is on another scene, whose card stands a unit to the
  // right. Program still shows the scene on air, so a press where only the
  // Preview card's corner is finds nothing to resize.
  scene::Scene preview = show;
  preview.id = scene::SceneId{101U};
  preview.instances[0U].world = scene::multiply(
      scene::translation(scene::Vec3{1.0F, 0.0F, 0.0F}),
      scene::scaling(card.scale));
  elements[0U].position = scene::Vec3{1.0F, 0.0F, 0.0F};
  switcher.program_scene = show.id;
  switcher.stage_scene = &preview;
  frame();
  check(shell->apply_selection_set_recall(preview.id, selected),
        "select the Preview card");
  frame();
  const gui::UiPoint preview_corner = on_screen(scene::Vec3{2.0F, -1.0F, 0.0F});
  const gui::UiPoint preview_beyond = on_screen(scene::Vec3{3.0F, -1.5F, 0.0F});
  move(preview_corner);
  edits.clear();
  properties.clear();
  button(preview_corner, true);
  move(preview_beyond);
  button(preview_beyond, false);
  check(properties.empty(),
        "Program's own picture has no handle at the Preview card's corner");
  // That press landed on bare picture, which clears the selection.
  check(shell->apply_selection_set_recall(preview.id, selected),
        "select the Preview card again");

  // The view button goes round to the whole canvas, which follows the Stage
  // into Preview: now the same drag resizes the Preview card, against its
  // own top-left corner.
  const gui::UiPoint view_button{panel.x + 40.0F, panel.y + 13.0F};
  move(view_button);
  button(view_button, true);
  button(view_button, false);
  for (int warm = 0; warm < 12; ++warm) frame();
  move(preview_corner);
  edits.clear();
  properties.clear();
  button(preview_corner, true);
  move(preview_beyond);
  button(preview_beyond, false);
  check(value_of(scene::TransformChannel::ScaleX).has_value(),
        "the Canvas view authors into the Preview scene");
  check(near(*value_of(scene::TransformChannel::ScaleX), 3.0) &&
            near(*value_of(scene::TransformChannel::ScaleY), 2.5),
        "the Preview card resizes as Program's did");
  check(!edits.empty() && near(edits.back().position.x, 1.5) &&
            near(edits.back().position.y, -0.25),
        "from where the Preview card stands");

  // Back on an output, the view is Program's again.
  move(view_button);
  button(view_button, true);
  button(view_button, false);
  frame();
  move(preview_corner);
  edits.clear();
  properties.clear();
  button(preview_corner, true);
  move(preview_beyond);
  button(preview_beyond, false);
  check(properties.empty() && edits.empty(),
        "leaving the Canvas view leaves Preview alone");
}
} // namespace

int main() {
  try {
    program_tools();
    std::puts("[pass] Program canvas tools");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "[fail] %s\n", e.what());
    return 1;
  }
}
