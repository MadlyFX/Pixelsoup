#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/editor/workspace_layout.hpp>

#include <SDL3/SDL.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace assets = mediaserver::assets;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace scene = mediaserver::scene;

void check(bool value, const char *reason) {
  if (!value) {
    throw std::runtime_error{reason};
  }
}

// The Asset Viewer through the shipping docked shell and real SDL input: a
// Library double-click opens the asset there and places nothing, the viewer
// asks the Director for the frame it shows, and with the viewer focused `,`
// cuts its range into the track the Timeline's targeting rule names.
void library_to_viewer_to_track() {
  if (!editor::editor_available()) {
    std::puts("[skip] editor GPU backend unavailable");
    return;
  }
  gui::DockLayout layout;
  const auto viewer_panel = gui::ui_id("editor.panel.asset-viewer");
  const auto library_panel = gui::ui_id("editor.panel.library");
  const auto timeline_panel = gui::ui_id("editor.panel.timeline");
  const auto viewer_leaf = layout.add(viewer_panel, "Asset Viewer");
  static_cast<void>(layout.dock(library_panel, "Library", viewer_leaf,
                                gui::DockSide::Left));
  static_cast<void>(layout.dock(timeline_panel, "Timeline", viewer_leaf,
                                gui::DockSide::Bottom));
  const auto workspace = editor::encode_workspace_layout(
      editor::WorkspaceLayoutState{layout.snapshot(), 1600U, 900U});
  check(workspace.has_value(), "encode viewer workspace");
  editor::EditorConfig config;
  config.title = "Asset Viewer workflow";
  config.workspace_layout = workspace.value();
  auto created = editor::EditorShell::create(config);
  check(created.has_value(), "create editor");
  auto shell = std::move(created).value();

  scene::Scene show;
  show.id = scene::SceneId{100U};
  editor::EditorTimeline timeline;
  timeline.last_frame = 600U;
  timeline.show_frame = 12U;
  timeline.rate_numerator = 25U;
  timeline.rate_denominator = 1U;
  const std::vector<scene::ChannelKeyframes> keys;

  // One clip on the Plates track, selected, so the track every selected
  // clip is on is where `,` lands.
  constexpr doc::NodeId plates{50U};
  constexpr doc::NodeId clip{31U};
  std::vector<editor::ElementInfo> elements(1U);
  elements[0U].node = clip;
  elements[0U].kind = editor::ElementKind::Media;
  elements[0U].display_name = "Earlier cut";
  elements[0U].source = "T:/other.mov";
  elements[0U].timing = scene::ClipTiming{0, 10, 0, 0, 0};
  elements[0U].timeline_lane = plates;
  elements[0U].sequence_count = 100U;
  const std::array<scene::TimelineLane, 1U> lanes{
      scene::TimelineLane{plates, show.id, 10, "Plates", false, false,
                          scene::BlendMode::Normal, false}};

  assets::AssetRegistry registry;
  assets::MediaAsset media;
  media.id = doc::NodeId{71U};
  media.display_name = "Viewer plate";
  media.source = "T:/viewer_plate.mov";
  media.frame_count = 48U;
  media.frame_rate = assets::MediaFrameRate{24U, 1U};
  media.dimensions = assets::PixelDimensions{1920U, 1080U};
  registry.assets.emplace_back(media);
  editor::EditorSwitcherState switcher;

  const auto frame = [&] {
    auto result = shell->frame(show, timeline, keys, elements, {}, {}, {},
                               switcher, registry, {}, {}, {}, {}, {}, {},
                               {}, {}, {}, lanes);
    check(result.status == editor::EditorStatus::Running, "editor running");
    return result;
  };
  static_cast<void>(frame());
  check(shell->apply_selection_set_recall(show.id, std::array{clip}, clip),
        "select the earlier clip");

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
  const auto library = layout.content_of(layout.leaf_of(library_panel));
  const auto viewer = layout.content_of(layout.leaf_of(viewer_panel));
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
  const auto type = [&](const char *text) {
    SDL_Event typed{};
    typed.type = SDL_EVENT_TEXT_INPUT;
    typed.text.windowID = window_id;
    typed.text.text = text;
    check(SDL_PushEvent(&typed), "type");
    return frame();
  };

  // Filter the Library to the one asset: its row is the first under the
  // column header.
  static_cast<void>(pointer(library.x + 75.0F, library.y + 37.0F, true));
  static_cast<void>(pointer(library.x + 75.0F, library.y + 37.0F, false));
  static_cast<void>(type("Viewer"));
  const float row_x = library.x + 80.0F;
  const float row_y = library.y + 86.0F;

  // Nothing is open yet, so the viewer asks for nothing.
  check(!frame().asset_viewer.has_value(), "an empty viewer requests nothing");

  // A double-click opens the asset in the viewer and places nothing.
  bool added = false;
  for (int click = 0; click < 2; ++click) {
    added = added || !pointer(row_x, row_y, true).adds.empty();
    added = added || !pointer(row_x, row_y, false).adds.empty();
  }
  check(!added, "a double-click does not place the asset");
  std::optional<editor::AssetViewerRequest> requested;
  for (int attempt = 0; attempt < 4 && !requested.has_value(); ++attempt) {
    requested = frame().asset_viewer;
  }
  check(requested.has_value(), "the viewer asks to see the opened asset");
  check(requested->asset == media.id && requested->source_frame == 0U,
        "the request names the asset at its first frame");

  // Focus the viewer and cut the whole source into the selected clip's track.
  const float picture_x = viewer.x + viewer.width * 0.5F;
  const float picture_y = viewer.y + 28.0F + 40.0F;
  static_cast<void>(pointer(picture_x, picture_y, true));
  static_cast<void>(pointer(picture_x, picture_y, false));
  const editor::EditorFrame cut = type(",");
  check(cut.source_edits.size() == 1U, "`,` cuts one range");
  const editor::SourceRangeEditIntent &edit = cut.source_edits.front();
  check(edit.asset == media.id && edit.lane == plates &&
            edit.scene == show.id,
        "the cut names the asset, the target track and the Stage scene");
  check(edit.mode == editor::SourceEditMode::Insert && edit.source_in == 0U &&
            edit.source_frames == 48U,
        "an unmarked source is cut whole, as an insert");
  check(edit.start == 12U && edit.show_rate == scene::MediaFrameRate{25U, 1U},
        "the cut lands at the Stage playhead at the show rate");
  check(edit.transaction != 0U, "the cut is its own undo step");
  check(cut.adds.empty() && cut.timeline_edits.empty(),
        "the cut is only the source edit");
  check(frame().source_edits.empty(), "one keystroke cuts once");

  // During a transition the cut is refused in the viewer.
  switcher.rehearsal_locked = true;
  check(type(".").source_edits.empty(), "a transition refuses the cut");
}
// A red cube, as the importer reads one: a model the thumbnail can be told
// apart from the floor and sky by colour alone.
[[nodiscard]] std::string write_red_cube() {
  const auto directory =
      std::filesystem::temp_directory_path() / "mediaserver_viewer_thumbnail";
  std::filesystem::create_directories(directory);
  {
    std::ofstream material{directory / "red.mtl", std::ios::trunc};
    material << "newmtl Red\nKd 0.85 0.05 0.05\nPr 0.6\nPm 0.0\n";
  }
  const auto path = directory / "cube.obj";
  std::ofstream model{path, std::ios::trunc};
  model << "mtllib red.mtl\n"
           "v -0.5 0 -0.5\nv 0.5 0 -0.5\nv 0.5 1 -0.5\nv -0.5 1 -0.5\n"
           "v -0.5 0 0.5\nv 0.5 0 0.5\nv 0.5 1 0.5\nv -0.5 1 0.5\n"
           "usemtl Red\n"
           "f 1 3 2\nf 1 4 3\nf 5 6 7\nf 5 7 8\nf 1 5 8\nf 1 8 4\n"
           "f 2 3 7\nf 2 7 6\nf 4 8 7\nf 4 7 3\nf 1 2 6\nf 1 6 5\n";
  model.close();
  check(static_cast<bool>(model), "write the cube");
  return path.generic_string();
}

// A model's Library thumbnail is drawn on the editor's own device: queued by
// key, it comes back a few frames later as 320 x 180 pixels with the model
// in the middle, and a file that cannot load comes back with no picture.
void model_thumbnails_draw_on_the_editors_device() {
  if (!editor::editor_available()) {
    std::puts("[skip] editor GPU backend unavailable");
    return;
  }
  editor::EditorConfig config;
  config.title = "Model thumbnail";
  auto created = editor::EditorShell::create(config);
  check(created.has_value(), "create editor");
  auto shell = std::move(created).value();
  scene::Scene show;
  show.id = scene::SceneId{100U};
  editor::EditorTimeline timeline;
  timeline.last_frame = 600U;
  const std::vector<scene::ChannelKeyframes> keys;
  const std::vector<editor::ElementInfo> elements;
  const assets::AssetRegistry registry;
  editor::EditorSwitcherState switcher;
  const auto frame = [&] {
    auto result = shell->frame(show, timeline, keys, elements, {}, {}, {},
                               switcher, registry, {}, {}, {}, {}, {}, {},
                               {}, {}, {});
    check(result.status == editor::EditorStatus::Running, "editor running");
  };

  const assets::DerivativeProfile *const profile = assets::derivative_profile(
      assets::DerivativeProfileId::ModelTurntable320x180Srgb);
  check(profile != nullptr, "the model thumbnail profile exists");
  const auto key_for = [&](core::u64 id) {
    assets::ContentFingerprint fingerprint{};
    fingerprint.byte_length = id;
    fingerprint.sha256.bytes[0U] = static_cast<core::u8>(id);
    return assets::make_derivative_key(doc::NodeId{id}, fingerprint, *profile);
  };
  const assets::DerivativeKey cube = key_for(88U);
  const assets::DerivativeKey missing = key_for(89U);
  const core::u32 room = shell->model_thumbnail_room();
  check(room != 0U, "there is room for a thumbnail");
  check(shell->queue_model_thumbnail(cube, write_red_cube()), "queue the cube");
  check(shell->queue_model_thumbnail(
            missing, "T:/mediaserver-no-such-folder/missing.obj"),
        "queue the missing model");
  check(shell->model_thumbnail_room() == room - 2U, "both take room");

  std::vector<editor::RenderedModelThumbnail> drawn;
  for (int attempt = 0; attempt < 1200 && drawn.size() < 2U; ++attempt) {
    frame();
    for (editor::RenderedModelThumbnail &done : shell->take_model_thumbnails()) {
      drawn.push_back(std::move(done));
    }
  }
  check(drawn.size() == 2U, "both thumbnails are answered");
  const auto answer = [&](const assets::DerivativeKey &key)
      -> const editor::RenderedModelThumbnail & {
    for (const editor::RenderedModelThumbnail &done : drawn) {
      if (done.key == key) {
        return done;
      }
    }
    throw std::runtime_error{"a queued key was not answered"};
  };
  check(answer(missing).rgba.empty(), "a missing model has no picture");
  const std::vector<core::u8> &pixels = answer(cube).rgba;
  check(pixels.size() == 320U * 180U * 4U, "the picture is 320 x 180 RGBA");
  const std::size_t centre = (90U * 320U + 160U) * 4U;
  check(pixels[centre] > pixels[centre + 1U] + 40 &&
            pixels[centre] > pixels[centre + 2U] + 40,
        "the red cube is in the middle of the picture");
  const std::size_t corner = (4U * 320U + 4U) * 4U;
  check(pixels[corner] < pixels[corner + 1U] + 40,
        "the corner is sky, not the model");
  check(shell->model_thumbnail_room() == room, "the room comes back");
}
} // namespace

int main() {
  try {
    library_to_viewer_to_track();
    std::puts("[pass] Library double-click opens the Asset Viewer, which cuts "
              "into the target track");
    model_thumbnails_draw_on_the_editors_device();
    std::puts("[pass] a model's thumbnail is drawn on the editor's device");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "[fail] %s\n", error.what());
    return 1;
  }
}
