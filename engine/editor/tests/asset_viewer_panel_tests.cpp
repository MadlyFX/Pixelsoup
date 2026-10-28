#include <mediaserver/editor/asset_viewer.hpp>
#include <mediaserver/editor/asset_viewer_panel.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <optional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace assets = mediaserver::assets;
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

#define VIEWER_CHECK(...)                                                      \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] bool near(float actual, float wanted,
                        float tolerance = 1.0e-3F) noexcept {
  return std::fabs(actual - wanted) <= tolerance;
}

constexpr doc::NodeId plate_id{40U};
constexpr doc::NodeId other_id{41U};
constexpr doc::NodeId model_id{42U};
constexpr doc::NodeId track_id{7U};

[[nodiscard]] assets::MediaAsset plate() {
  assets::MediaAsset asset{};
  asset.id = plate_id;
  asset.display_name = "Driving Plate";
  asset.source = "D:/media/plate.mov";
  asset.dimensions = assets::PixelDimensions{3840U, 2160U};
  asset.frame_count = 240U;
  asset.frame_rate = assets::MediaFrameRate{24U, 1U};
  asset.start_timecode = "01:00:00:00";
  return asset;
}

[[nodiscard]] assets::AssetRegistry registry() {
  assets::AssetRegistry value{};
  value.assets.push_back(assets::Asset{plate()});
  assets::MediaAsset other = plate();
  other.id = other_id;
  other.display_name = "Second";
  other.source = "D:/media/second.mov";
  value.assets.push_back(assets::Asset{other});
  assets::ModelAsset model{};
  model.id = model_id;
  model.display_name = "Car";
  model.source = "D:/models/car.glb";
  value.assets.push_back(assets::Asset{model});
  return value;
}

struct Harness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::AssetViewerPanel panel{gui::ui_id("viewer")};
  assets::AssetRegistry assets = registry();
  std::optional<doc::NodeId> target{track_id};
  bool locked{};
  double time{};

  static constexpr gui::Rect bounds{0.0F, 0.0F, 800.0F, 500.0F};
  // Inside the picture area, which runs from below the 28-unit header to
  // above the scrub bar, transport and edit rows.
  static constexpr float picture_x = 400.0F;
  static constexpr float picture_y = 200.0F;

  editor::AssetViewerFrame frame(float x, float y, bool down,
                                 std::string_view text = {},
                                 std::optional<gui::Key> key = std::nullopt,
                                 gui::KeyMods mods = gui::KeyMods::None) {
    input.new_frame();
    input.set_mods(mods);
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.add_text(text);
    if (key.has_value()) {
      input.add_key(*key);
    }
    ctx.new_frame(time);
    editor::AssetViewerDrawInput in{};
    in.assets = &assets;
    in.show_rate = scene::MediaFrameRate{25U, 1U};
    in.target_lane = target;
    in.target_name = "Plates";
    in.edits_locked = locked;
    return panel.draw(ctx, bounds, in);
  }

  void click(float x, float y) {
    static_cast<void>(frame(x, y, false));
    static_cast<void>(frame(x, y, true));
    static_cast<void>(frame(x, y, false));
  }

  editor::AssetViewerFrame type(std::string_view text) {
    return frame(-1.0F, -1.0F, false, text);
  }
};

// ---------------------------------------------------------------- framing

void a_fit_shows_the_whole_picture_centred() {
  const assets::PixelDimensions source{3840U, 2160U};
  // A 1000 x 1000 area fits a 16:9 picture to its width.
  VIEWER_CHECK(near(editor::asset_viewer_fit_scale(source, 1000.0F, 1000.0F),
                    1000.0F / 3840.0F));
  const editor::AssetViewerPictureRect picture =
      editor::asset_viewer_picture_rect({}, source, 1000.0F, 1000.0F);
  VIEWER_CHECK(near(picture.x, 0.0F) && near(picture.width, 1000.0F));
  VIEWER_CHECK(near(picture.height, 562.5F));
  VIEWER_CHECK(near(picture.y, (1000.0F - 562.5F) * 0.5F));
  // The orthographic camera spans the area's height in picture heights.
  const scene::Camera camera =
      editor::asset_viewer_camera({}, source, 1000.0F, 1000.0F);
  VIEWER_CHECK(near(camera.orthographic_height, 1000.0F / 562.5F));
  VIEWER_CHECK(near(camera.eye.x, 0.0F) && camera.eye.z > camera.target.z);
}

void one_to_one_is_one_source_pixel_per_screen_pixel() {
  const assets::PixelDimensions source{3840U, 2160U};
  const editor::AssetViewerFraming actual{1.0F, 0.0F, 0.0F};
  const editor::AssetViewerPictureRect picture =
      editor::asset_viewer_picture_rect(actual, source, 1000.0F, 500.0F);
  VIEWER_CHECK(near(picture.width, 3840.0F) && near(picture.height, 2160.0F));
  VIEWER_CHECK(near(picture.x + picture.width * 0.5F, 500.0F));
  VIEWER_CHECK(near(editor::asset_viewer_view_height(actual, source, 1000.0F,
                                                     500.0F),
                    500.0F / 2160.0F));
}

void a_zoom_keeps_the_point_under_the_pointer() {
  const assets::PixelDimensions source{1920U, 1080U};
  const float width = 900.0F;
  const float height = 600.0F;
  const editor::AssetViewerPictureRect before =
      editor::asset_viewer_picture_rect({}, source, width, height);
  const float at_x = 300.0F;
  const float at_y = 250.0F;
  const float u = (at_x - before.x) / before.width;
  const float v = (at_y - before.y) / before.height;
  const editor::AssetViewerFraming zoomed =
      editor::asset_viewer_zoom({}, source, width, height, 2.0F, at_x, at_y);
  VIEWER_CHECK(near(zoomed.scale,
                    2.0F * editor::asset_viewer_fit_scale(source, width, height)));
  const editor::AssetViewerPictureRect after =
      editor::asset_viewer_picture_rect(zoomed, source, width, height);
  VIEWER_CHECK(near(after.width, before.width * 2.0F, 1.0e-2F));
  VIEWER_CHECK(near(after.x + u * after.width, at_x, 1.0e-2F));
  VIEWER_CHECK(near(after.y + v * after.height, at_y, 1.0e-2F));

  // A pan moves the picture by exactly the drag.
  const editor::AssetViewerFraming panned = editor::asset_viewer_pan(
      zoomed, source, width, height, 37.0F, -12.0F);
  const editor::AssetViewerPictureRect moved =
      editor::asset_viewer_picture_rect(panned, source, width, height);
  VIEWER_CHECK(near(moved.x - after.x, 37.0F, 1.0e-2F));
  VIEWER_CHECK(near(moved.y - after.y, -12.0F, 1.0e-2F));
}

// ---------------------------------------------------------------- timecode

void timecode_counts_from_the_sources_own_start() {
  const assets::MediaAsset asset = plate();
  VIEWER_CHECK(editor::asset_viewer_timecode(asset, {25U, 1U}, 30U) ==
               "01:00:01:06");
  // Drop-frame skips frames 00 and 01 at each minute but every tenth.
  assets::MediaAsset ntsc = plate();
  ntsc.frame_rate = assets::MediaFrameRate{30'000U, 1'001U};
  ntsc.start_timecode = "00:00:59;29";
  VIEWER_CHECK(editor::asset_viewer_timecode(ntsc, {25U, 1U}, 1U) ==
               "00:01:00;02");
  // A sequence without a rate counts at the show's.
  assets::MediaAsset sequence = plate();
  sequence.frame_rate.reset();
  sequence.start_timecode.clear();
  VIEWER_CHECK(editor::asset_viewer_timecode(sequence, {25U, 1U}, 26U) ==
               "00:00:01:01");
}

// ---------------------------------------------------------------- scene

void the_viewer_scene_is_one_clip_at_the_requested_frame() {
  scene::RendererSettings project{};
  project.backend = scene::RendererBackend::Advanced;
  project.bloom_strength = 0.8F;
  project.taau = true;
  const scene::Scene viewer =
      editor::asset_viewer_media_scene(plate(), 117U, project);
  VIEWER_CHECK(viewer.media.size() == 1U);
  VIEWER_CHECK(viewer.models.empty() && viewer.instances.empty());
  const scene::MediaInstance &clip = viewer.media.front();
  VIEWER_CHECK(clip.source == "D:/media/plate.mov");
  VIEWER_CHECK(clip.frame_index == 117U);
  VIEWER_CHECK(clip.loop_first == 0U && clip.loop_count == 240U);
  VIEWER_CHECK(clip.asset == plate_id);
  // One unit tall, as wide as the picture.
  VIEWER_CHECK(near(clip.world.m[0], 16.0F / 9.0F));
  VIEWER_CHECK(near(clip.world.m[5], 1.0F));
  // The project's backend reaches it; the wall's look effects do not.
  VIEWER_CHECK(viewer.renderer_settings.backend ==
               scene::RendererBackend::Advanced);
  VIEWER_CHECK(viewer.renderer_settings.bloom_strength == 0.0F);
  VIEWER_CHECK(!viewer.renderer_settings.taau);
  VIEWER_CHECK(viewer.environment.sky_color.x == 0.0F);
  VIEWER_CHECK(viewer.camera.orthographic_height == 0.0F);
  // A request past the last frame shows the last frame.
  VIEWER_CHECK(editor::asset_viewer_media_scene(plate(), 999U, project)
                   .media.front()
                   .frame_index == 239U);
}

// ---------------------------------------------------------------- panel

void an_opened_asset_is_requested_and_nothing_else() {
  Harness harness;
  VIEWER_CHECK(!harness.type({}).request.has_value());
  harness.panel.open(plate_id);
  const editor::AssetViewerFrame shown = harness.type({});
  VIEWER_CHECK(shown.request == editor::AssetViewerRequest{plate_id, 0U});
  VIEWER_CHECK(shown.surface_width > 0U && shown.surface_height > 0U);
  // A model opens in the panel but is not something it renders.
  harness.panel.open(model_id);
  VIEWER_CHECK(!harness.type({}).request.has_value());
}

void transport_keys_work_only_with_focus() {
  Harness harness;
  harness.panel.open(plate_id);
  // Not focused: L does nothing.
  static_cast<void>(harness.type("l"));
  VIEWER_CHECK(!harness.panel.playing());
  harness.click(Harness::picture_x, Harness::picture_y);
  VIEWER_CHECK(harness.panel.focused());
  // L plays at the source's own 24 fps from where it stands.
  static_cast<void>(harness.type("l"));
  VIEWER_CHECK(harness.panel.playing());
  harness.time += 0.5;
  VIEWER_CHECK(harness.type({}).request->source_frame == 12U);
  // K stops; Right steps one frame and Shift+Right a second.
  static_cast<void>(harness.type("k"));
  VIEWER_CHECK(!harness.panel.playing());
  static_cast<void>(harness.frame(-1.0F, -1.0F, false, {}, gui::Key::Right));
  VIEWER_CHECK(harness.panel.frame() == 13U);
  static_cast<void>(harness.frame(-1.0F, -1.0F, false, {}, gui::Key::Right,
                                  gui::KeyMods::Shift));
  VIEWER_CHECK(harness.panel.frame() == 37U);
  // J plays backwards and stops at the first frame.
  static_cast<void>(harness.type("j"));
  harness.time += 10.0;
  VIEWER_CHECK(harness.type({}).request->source_frame == 0U);
  VIEWER_CHECK(!harness.panel.playing());
  // A press elsewhere takes the focus away.
  harness.click(900.0F, 900.0F);
  VIEWER_CHECK(!harness.panel.focused());
  static_cast<void>(harness.type("l"));
  VIEWER_CHECK(!harness.panel.playing());
}

void marks_and_comma_cut_the_range_into_the_target_track() {
  Harness harness;
  harness.panel.open(plate_id, 48U);
  harness.click(Harness::picture_x, Harness::picture_y);
  static_cast<void>(harness.type("i"));
  harness.panel.open(plate_id, 71U);
  // Re-opening the same asset at a frame keeps the panel's focus but resets
  // its marks, as opening any asset does.
  VIEWER_CHECK(!harness.panel.mark_in().has_value());
  static_cast<void>(harness.type("i"));
  static_cast<void>(harness.frame(-1.0F, -1.0F, false, {}, gui::Key::Right));
  static_cast<void>(harness.frame(-1.0F, -1.0F, false, {}, gui::Key::Right));
  static_cast<void>(harness.type("o"));
  VIEWER_CHECK(harness.panel.mark_in() == 71U);
  VIEWER_CHECK(harness.panel.mark_out() == 73U);
  const editor::AssetViewerFrame inserted = harness.type(",");
  VIEWER_CHECK(inserted.source_edit ==
               editor::AssetViewerSourceEdit{editor::SourceEditMode::Insert,
                                             71U, 3U});
  const editor::AssetViewerFrame overwritten = harness.type(".");
  VIEWER_CHECK(overwritten.source_edit ==
               editor::AssetViewerSourceEdit{
                   editor::SourceEditMode::Overwrite, 71U, 3U});
  // Home goes to In, End to Out.
  static_cast<void>(harness.frame(-1.0F, -1.0F, false, {}, gui::Key::End));
  VIEWER_CHECK(harness.panel.frame() == 73U);
  static_cast<void>(harness.frame(-1.0F, -1.0F, false, {}, gui::Key::Home));
  VIEWER_CHECK(harness.panel.frame() == 71U);
}

void without_marks_the_whole_source_is_cut() {
  Harness harness;
  harness.panel.open(plate_id, 100U);
  harness.click(Harness::picture_x, Harness::picture_y);
  VIEWER_CHECK(harness.type(",").source_edit ==
               editor::AssetViewerSourceEdit{editor::SourceEditMode::Insert,
                                             0U, 240U});
}

void a_cut_without_a_target_or_during_a_transition_is_refused() {
  Harness harness;
  harness.panel.open(plate_id);
  harness.click(Harness::picture_x, Harness::picture_y);
  harness.target.reset();
  VIEWER_CHECK(!harness.type(",").source_edit.has_value());
  harness.target = track_id;
  harness.locked = true;
  VIEWER_CHECK(!harness.type(".").source_edit.has_value());
  harness.locked = false;
  VIEWER_CHECK(harness.type(".").source_edit.has_value());
}

void the_picture_drags_out_carrying_its_range() {
  Harness harness;
  harness.panel.open(plate_id, 10U, std::pair{10U, 33U});
  static_cast<void>(
      harness.frame(Harness::picture_x, Harness::picture_y, false));
  static_cast<void>(harness.frame(Harness::picture_x, Harness::picture_y, true));
  const editor::AssetViewerFrame held = harness.frame(950.0F, 640.0F, true);
  VIEWER_CHECK(held.dragging);
  VIEWER_CHECK(harness.panel.dragging());
  const editor::AssetViewerFrame released = harness.frame(950.0F, 640.0F, false);
  VIEWER_CHECK(released.drop.has_value());
  VIEWER_CHECK(released.drop->asset == plate_id);
  VIEWER_CHECK(released.drop->source_in == 10U);
  VIEWER_CHECK(released.drop->source_frames == 24U);
  VIEWER_CHECK(released.drop->screen_x == 950.0F);
  // Let go inside the viewer, it is no drop at all.
  static_cast<void>(
      harness.frame(Harness::picture_x, Harness::picture_y, false));
  static_cast<void>(harness.frame(Harness::picture_x, Harness::picture_y, true));
  static_cast<void>(harness.frame(600.0F, 300.0F, true));
  VIEWER_CHECK(!harness.frame(600.0F, 300.0F, false).drop.has_value());
}

void the_wheel_zooms_about_the_pointer_and_fit_resets() {
  Harness harness;
  harness.panel.open(plate_id);
  static_cast<void>(harness.type({}));
  VIEWER_CHECK(harness.panel.framing().scale == 0.0F);
  harness.input.new_frame();
  harness.input.set_mouse_position(Harness::picture_x, Harness::picture_y);
  harness.input.add_wheel(2.0F);
  harness.ctx.new_frame(harness.time);
  editor::AssetViewerDrawInput in{};
  in.assets = &harness.assets;
  static_cast<void>(harness.panel.draw(harness.ctx, Harness::bounds, in));
  VIEWER_CHECK(harness.panel.framing().scale > 0.0F);
  harness.click(Harness::picture_x, Harness::picture_y);
  static_cast<void>(harness.type("f"));
  VIEWER_CHECK(harness.panel.framing().scale == 0.0F);
}

void a_project_change_forgets_the_asset() {
  Harness harness;
  harness.panel.open(plate_id);
  harness.panel.open(other_id);
  VIEWER_CHECK(harness.panel.asset() == other_id);
  harness.panel.reset_project_state();
  VIEWER_CHECK(harness.panel.asset() == doc::NodeId{});
  VIEWER_CHECK(!harness.type({}).request.has_value());
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 13U> cases{{
    {"a fit shows the whole picture centred",
     &a_fit_shows_the_whole_picture_centred},
    {"1:1 is one source pixel per screen pixel",
     &one_to_one_is_one_source_pixel_per_screen_pixel},
    {"a zoom keeps the point under the pointer",
     &a_zoom_keeps_the_point_under_the_pointer},
    {"timecode counts from the source's own start",
     &timecode_counts_from_the_sources_own_start},
    {"the viewer scene is one clip at the requested frame",
     &the_viewer_scene_is_one_clip_at_the_requested_frame},
    {"an opened asset is requested and nothing else",
     &an_opened_asset_is_requested_and_nothing_else},
    {"transport keys work only with focus",
     &transport_keys_work_only_with_focus},
    {"marks and comma cut the range into the target track",
     &marks_and_comma_cut_the_range_into_the_target_track},
    {"without marks the whole source is cut",
     &without_marks_the_whole_source_is_cut},
    {"a cut without a target or during a transition is refused",
     &a_cut_without_a_target_or_during_a_transition_is_refused},
    {"the picture drags out carrying its range",
     &the_picture_drags_out_carrying_its_range},
    {"the wheel zooms about the pointer and Fit resets",
     &the_wheel_zooms_about_the_pointer_and_fit_resets},
    {"a project change forgets the asset",
     &a_project_change_forgets_the_asset},
}};

} // namespace

int main() {
  std::size_t failures{};
  for (const Case &test : cases) {
    try {
      test.function();
      std::cout << "[pass] " << test.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << test.name << ": " << error.what() << '\n';
    }
  }
  std::cout << (cases.size() - failures) << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
