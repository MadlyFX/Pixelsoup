#include <mediaserver/editor/asset_viewer.hpp>
#include <mediaserver/editor/asset_viewer_panel.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/scene/manipulate.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <memory>
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
namespace render = mediaserver::render;
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
  // A model asks for nothing the Director renders.
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

// With no track to aim at a cut lands outside every track; the Timeline names
// no target only when it aims at a locked one.
void a_cut_at_a_locked_track_or_during_a_transition_is_refused() {
  Harness harness;
  harness.panel.open(plate_id);
  harness.click(Harness::picture_x, Harness::picture_y);
  harness.target = doc::NodeId{};
  VIEWER_CHECK(harness.type(",").source_edit ==
               editor::AssetViewerSourceEdit{editor::SourceEditMode::Insert,
                                             0U, 240U});
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

// ---------------------------------------------------------------- models

[[nodiscard]] editor::WorldBounds box(scene::Vec3 minimum, scene::Vec3 maximum) {
  return editor::WorldBounds{minimum, maximum};
}

[[nodiscard]] bool near3(scene::Vec3 a, scene::Vec3 b,
                         float tolerance = 1.0e-3F) noexcept {
  return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) &&
         near(a.z, b.z, tolerance);
}

[[nodiscard]] assets::ModelAsset car() {
  assets::ModelAsset model{};
  model.id = model_id;
  model.display_name = "Car";
  model.source = "D:/models/car.glb";
  return model;
}

// Three parts, the second with two textures, and two clips.
[[nodiscard]] std::shared_ptr<const render::ModelPackageFacts> car_facts() {
  render::ModelPackageFacts facts{};
  facts.vertices = 900U;
  facts.triangles = 600U;
  for (int index = 0; index < 3; ++index) {
    render::ModelPartFacts part{};
    part.triangles = 200U;
    part.has_material = true;
    facts.parts.push_back(part);
  }
  facts.parts[1U].textures.push_back(render::ModelPartTexture{
      0U, 2048U, 2048U, render::ModelTextureEncoding::Bc7});
  facts.materials = 3U;
  facts.clips.push_back(render::ModelClipFacts{"Idle", 2.0F});
  facts.clips.push_back(render::ModelClipFacts{"Drive", 1.5F});
  facts.bounds = render::ModelBounds{scene::Vec3{-1.0F, 0.0F, -2.0F},
                                     scene::Vec3{1.0F, 1.5F, 2.0F}};
  return std::make_shared<const render::ModelPackageFacts>(std::move(facts));
}

void model_bounds_prefer_the_package_and_measure_from_the_base() {
  assets::ModelAsset model = car();
  // Nothing known: a 1 m cube standing on the origin.
  const editor::WorldBounds unknown =
      editor::asset_viewer_model_bounds(model, std::nullopt);
  VIEWER_CHECK(near3(unknown.minimum, scene::Vec3{-0.5F, 0.0F, -0.5F}));
  VIEWER_CHECK(near3(unknown.maximum, scene::Vec3{0.5F, 1.0F, 0.5F}));
  // The Library's import, then the loaded package over it.
  model.bounds = assets::ModelBounds{-2.0, -1.0, -3.0, 2.0, 1.0, 3.0};
  const editor::WorldBounds imported =
      editor::asset_viewer_model_bounds(model, std::nullopt);
  VIEWER_CHECK(near3(imported.maximum, scene::Vec3{2.0F, 1.0F, 3.0F}));
  const editor::WorldBounds loaded = editor::asset_viewer_model_bounds(
      model, box(scene::Vec3{0.0F, 0.0F, 0.0F}, scene::Vec3{4.0F, 3.0F, 2.0F}));
  VIEWER_CHECK(near3(loaded.maximum, scene::Vec3{4.0F, 3.0F, 2.0F}));

  // 4 m wide, 2 m deep, 3 m tall, with its origin at a corner of the base:
  // the pivot sits 2 m left and 1 m behind the base's middle.
  const editor::AssetViewerDimensions size =
      editor::asset_viewer_dimensions(loaded);
  VIEWER_CHECK(near(size.width, 4.0F) && near(size.depth, 2.0F) &&
               near(size.height, 3.0F));
  VIEWER_CHECK(near3(size.pivot_offset, scene::Vec3{-2.0F, 0.0F, -1.0F}));
  // Standing on its centre, the pivot is at the base's middle.
  VIEWER_CHECK(near3(editor::asset_viewer_dimensions(unknown).pivot_offset,
                     scene::Vec3{}));

  // Home looks at the middle of the box from the front right, a little
  // above, far enough back to see all of it.
  const editor::StageOrbit home = editor::asset_viewer_model_home(loaded);
  VIEWER_CHECK(near3(home.target, scene::Vec3{2.0F, 1.5F, 1.0F}));
  VIEWER_CHECK(home.yaw > 0.0F && home.pitch > 0.0F);
  const float radius = scene::length(scene::Vec3{2.0F, 1.5F, 1.0F});
  VIEWER_CHECK(home.distance > radius);
  VIEWER_CHECK(home.projection == editor::StageProjection::Perspective);
}

void the_look_dev_scene_places_lights_floor_and_figure() {
  const assets::ModelAsset model = car();
  const editor::WorldBounds bounds =
      box(scene::Vec3{-1.0F, -0.5F, -2.0F}, scene::Vec3{1.0F, 1.0F, 2.0F});
  scene::RendererSettings project{};
  project.backend = scene::RendererBackend::Advanced;
  project.texture_budget_mib = 1536U;
  project.bloom_strength = 0.7F;
  editor::AssetViewerModelView view{};
  view.asset = model_id;
  view.orbit = editor::asset_viewer_model_home(bounds);

  const scene::Scene studio =
      editor::asset_viewer_model_scene(model, view, bounds, project, nullptr);
  // The project's backend and budget carry over; its look does not.
  VIEWER_CHECK(studio.renderer_settings.backend ==
               scene::RendererBackend::Advanced);
  VIEWER_CHECK(studio.renderer_settings.texture_budget_mib == 1536U);
  VIEWER_CHECK(studio.renderer_settings.bloom_strength == 0.0F);
  // One model at the origin, drawn from its own file, never baked static.
  VIEWER_CHECK(studio.models.size() == 1U);
  VIEWER_CHECK(studio.models.front().source == "D:/models/car.glb");
  VIEWER_CHECK(studio.models.front().source_node == model_id);
  VIEWER_CHECK(!studio.models.front().bake_static);
  VIEWER_CHECK(!studio.models.front().animation.enabled);
  VIEWER_CHECK(near3(scene::transform_point(studio.models.front().world,
                                            scene::Vec3{1.0F, 0.0F, 0.0F}),
                     scene::Vec3{1.0F, 0.0F, 0.0F}));
  // Three lights, key first and brightest, all shining downwards.
  VIEWER_CHECK(studio.directional_lights.size() == 3U);
  for (const scene::DirectionalLight &light : studio.directional_lights) {
    VIEWER_CHECK(light.direction.y < 0.0F);
    VIEWER_CHECK(light.intensity <= studio.directional_lights.front().intensity);
  }
  VIEWER_CHECK(!studio.environment.baked.enabled &&
               studio.environment.fog_density == 0.0F);
  // The camera is the orbit.
  VIEWER_CHECK(near3(studio.camera.target, view.orbit.target));
  // The floor faces up at the model's base; the figure stands 1.8 m tall on
  // it beside the model, clear of it.
  const auto floor = std::find_if(
      studio.instances.begin(), studio.instances.end(), [](const auto &prop) {
        return prop.mesh.primitive == scene::Primitive::Plane;
      });
  VIEWER_CHECK(floor != studio.instances.end());
  VIEWER_CHECK(near(scene::translation_of(floor->world).y, -0.5F));
  float figure_top = 0.0F;
  float figure_x = 0.0F;
  for (const scene::MeshInstance &prop : studio.instances) {
    if (prop.mesh.primitive == scene::Primitive::Sphere) {
      const scene::Vec3 centre = scene::translation_of(prop.world);
      figure_top = std::max(figure_top, centre.y + 0.2F);
      figure_x = centre.x;
    }
  }
  VIEWER_CHECK(near(figure_top, 1.3F));
  VIEWER_CHECK(figure_x - 0.2F > bounds.maximum.x);

  // Without floor or figure, the model is all there is.
  view.look.floor = false;
  view.look.figure = false;
  VIEWER_CHECK(editor::asset_viewer_model_scene(model, view, bounds, project,
                                                nullptr)
                   .instances.empty());
  // A thumbnail's floor without its grid is the plane alone.
  view.look.floor = true;
  view.look.grid = false;
  const scene::Scene plain =
      editor::asset_viewer_model_scene(model, view, bounds, project, nullptr);
  VIEWER_CHECK(plain.instances.size() == 1U &&
               plain.instances.front().mesh.primitive ==
                   scene::Primitive::Plane);
  VIEWER_CHECK(studio.instances.size() > plain.instances.size() + 3U);
  view.look.grid = true;

  // Turning the rig turns every light; the turntable turns the model.
  view.look.rig_yaw = 0.25F;
  view.turntable = 0.25F;
  const scene::Scene turned =
      editor::asset_viewer_model_scene(model, view, bounds, project, nullptr);
  VIEWER_CHECK(!near3(turned.directional_lights.front().direction,
                      studio.directional_lights.front().direction));
  VIEWER_CHECK(near(turned.directional_lights.front().direction.y,
                    studio.directional_lights.front().direction.y));
  const scene::Vec3 side = scene::transform_point(turned.models.front().world,
                                                  scene::Vec3{1.0F, 0.0F, 0.0F});
  VIEWER_CHECK(near(side.x, 0.0F) && near(std::abs(side.z), 1.0F));

  // A clip plays on the viewer's own clock.
  view.clip = 1U;
  view.loop = false;
  view.animation_frame = 90U;
  const scene::Scene animated =
      editor::asset_viewer_model_scene(model, view, bounds, project, nullptr);
  VIEWER_CHECK(animated.models.front().animation.enabled);
  VIEWER_CHECK(animated.models.front().animation.clip == 1U);
  VIEWER_CHECK(!animated.models.front().animation.loop);
  VIEWER_CHECK(animated.show_frame == 90U);
  VIEWER_CHECK(animated.show_rate ==
               (scene::MediaFrameRate{editor::asset_viewer_animation_rate, 1U}));
}

void the_stage_or_an_image_can_light_the_model() {
  const assets::ModelAsset model = car();
  const editor::WorldBounds bounds =
      box(scene::Vec3{-1.0F, 0.0F, -1.0F}, scene::Vec3{1.0F, 1.0F, 1.0F});
  scene::Scene stage{};
  stage.environment.sky_color = scene::Vec3{0.3F, 0.1F, 0.05F};
  stage.environment.fog_density = 0.4F;
  stage.environment.baked.enabled = true;
  scene::DirectionalLight sun{};
  sun.intensity = 7.5F;
  stage.directional_lights.push_back(sun);
  stage.camera.aperture_f_number = 11.0F;
  stage.camera.iso = 400.0F;
  editor::AssetViewerModelView view{};
  view.asset = model_id;
  view.orbit = editor::asset_viewer_model_home(bounds);
  view.look.environment = editor::AssetViewerEnvironment::Stage;
  const scene::Scene staged =
      editor::asset_viewer_model_scene(model, view, bounds, {}, &stage);
  VIEWER_CHECK(staged.environment.sky_color == stage.environment.sky_color);
  VIEWER_CHECK(staged.directional_lights.size() == 1U);
  VIEWER_CHECK(staged.directional_lights.front().intensity == 7.5F);
  VIEWER_CHECK(staged.camera.aperture_f_number == 11.0F);
  VIEWER_CHECK(staged.camera.iso == 400.0F);
  // The room's air and baked light stay in the room.
  VIEWER_CHECK(staged.environment.fog_density == 0.0F);
  VIEWER_CHECK(!staged.environment.baked.enabled);
  // With no Stage scene, the studio lights it.
  VIEWER_CHECK(editor::asset_viewer_model_scene(model, view, bounds, {}, nullptr)
                   .directional_lights.size() == 3U);

  view.look.environment = editor::AssetViewerEnvironment::Hdri;
  view.look.hdri_source = "D:/hdri/hall.exr";
  view.look.rig_yaw = 0.125F;
  const scene::Scene image =
      editor::asset_viewer_model_scene(model, view, bounds, {}, &stage);
  VIEWER_CHECK(image.environment.map_source == "D:/hdri/hall.exr");
  VIEWER_CHECK(image.environment.map_rotation == 0.125F);
  VIEWER_CHECK(image.directional_lights.empty());
}

struct ModelHarness final {
  Harness base;
  editor::AssetViewerModelInput model{};

  ModelHarness() {
    base.assets.assets.clear();
    base.assets.assets.push_back(assets::Asset{plate()});
    base.assets.assets.push_back(assets::Asset{car()});
  }

  editor::AssetViewerFrame frame(float x, float y, bool down,
                                 std::string_view text = {},
                                 std::optional<gui::Key> key = std::nullopt,
                                 gui::KeyMods mods = gui::KeyMods::None,
                                 gui::MouseButton button = gui::MouseButton::Left) {
    gui::InputState &input = base.input;
    input.new_frame();
    input.set_mods(mods);
    input.set_mouse_position(x, y);
    input.set_mouse_button(button, down);
    input.add_text(text);
    if (key.has_value()) {
      input.add_key(*key);
    }
    base.ctx.new_frame(base.time);
    editor::AssetViewerDrawInput in{};
    in.assets = &base.assets;
    in.model = &model;
    return base.panel.draw(base.ctx, Harness::bounds, in);
  }

  void click(float x, float y) {
    static_cast<void>(frame(x, y, false));
    static_cast<void>(frame(x, y, true));
    static_cast<void>(frame(x, y, false));
  }

  editor::AssetViewerFrame type(std::string_view text) {
    return frame(-1.0F, -1.0F, false, text);
  }

  // The info area's tab buttons, as the panel lays them out.
  [[nodiscard]] float tab_x(std::size_t index) const {
    const std::array<std::string, 3U> names{
        "Details", "Parts  3", "Animation  2"};
    float x = 6.0F;
    for (std::size_t at = 0U; at < index; ++at) {
      x += base.ctx.font_width(names[at]) + 20.0F + 4.0F;
    }
    return x + 4.0F;
  }
};

// The 800 x 500 panel keeps 160 units below its picture for the info area,
// whose tabs take its top 26 and whose rows start 4 below them.
constexpr float info_tab_y = 340.0F + 13.0F;
constexpr float first_row_y = 370.0F;

void a_model_opens_framed_and_asks_for_its_view() {
  ModelHarness harness;
  harness.base.panel.open(model_id);
  const editor::AssetViewerFrame loading = harness.type({});
  VIEWER_CHECK(!loading.request.has_value());
  VIEWER_CHECK(loading.model_view.has_value());
  VIEWER_CHECK(loading.model_view->asset == model_id);
  VIEWER_CHECK(loading.model_view->shading == mediaserver::render::ViewShading::Lit);
  VIEWER_CHECK(loading.surface_width > 0U && loading.surface_height > 0U);
  const editor::WorldBounds cube =
      editor::asset_viewer_model_bounds(car(), std::nullopt);
  VIEWER_CHECK(loading.model_view->orbit == editor::asset_viewer_model_home(cube));
  // The package's own box arrives and, untouched, the view re-frames to it.
  const editor::WorldBounds loaded =
      box(scene::Vec3{-3.0F, 0.0F, -3.0F}, scene::Vec3{3.0F, 4.0F, 3.0F});
  harness.model.state = editor::AssetViewerModelState::Ready;
  harness.model.bounds = loaded;
  VIEWER_CHECK(harness.type({}).model_view->orbit ==
               editor::asset_viewer_model_home(loaded));
  // Media still opens as media.
  harness.base.panel.open(plate_id);
  const editor::AssetViewerFrame media = harness.type({});
  VIEWER_CHECK(media.request.has_value() && !media.model_view.has_value());

  // A projection mesh opens showing its UV mapping.
  assets::ModelAsset mesh = car();
  mesh.id = doc::NodeId{43U};
  mesh.projection_mesh = true;
  harness.base.assets.assets.push_back(assets::Asset{mesh});
  harness.base.panel.open(mesh.id);
  VIEWER_CHECK(harness.type({}).model_view->shading ==
               mediaserver::render::ViewShading::UvChecker);
}

void the_model_view_orbits_frames_and_turns() {
  ModelHarness harness;
  harness.model.state = editor::AssetViewerModelState::Ready;
  harness.model.bounds =
      box(scene::Vec3{-1.0F, 0.0F, -1.0F}, scene::Vec3{1.0F, 2.0F, 1.0F});
  harness.base.panel.open(model_id);
  const editor::StageOrbit home = harness.type({}).model_view->orbit;
  // A left drag orbits about the pivot.
  static_cast<void>(harness.frame(400.0F, 200.0F, false));
  static_cast<void>(harness.frame(400.0F, 200.0F, true));
  const editor::AssetViewerFrame orbited = harness.frame(460.0F, 180.0F, true);
  static_cast<void>(harness.frame(460.0F, 180.0F, false));
  VIEWER_CHECK(orbited.model_view->orbit.yaw != home.yaw);
  VIEWER_CHECK(orbited.model_view->orbit.pitch != home.pitch);
  VIEWER_CHECK(near3(orbited.model_view->orbit.target, home.target));
  VIEWER_CHECK(harness.base.panel.focused());
  // Navigated, a new box no longer re-frames it; Home does.
  harness.model.bounds =
      box(scene::Vec3{-5.0F, 0.0F, -5.0F}, scene::Vec3{5.0F, 5.0F, 5.0F});
  VIEWER_CHECK(harness.type({}).model_view->orbit == orbited.model_view->orbit);
  const editor::AssetViewerFrame homed =
      harness.frame(-1.0F, -1.0F, false, {}, gui::Key::Home);
  VIEWER_CHECK(homed.model_view->orbit ==
               editor::asset_viewer_model_home(*harness.model.bounds));
  VIEWER_CHECK(homed.consumed_keys);
  // The wheel dollies in.
  harness.base.input.new_frame();
  harness.base.input.set_mouse_position(400.0F, 200.0F);
  harness.base.input.add_wheel(1.0F);
  harness.base.ctx.new_frame(harness.base.time);
  editor::AssetViewerDrawInput in{};
  in.assets = &harness.base.assets;
  in.model = &harness.model;
  const editor::AssetViewerFrame dollied =
      harness.base.panel.draw(harness.base.ctx, Harness::bounds, in);
  VIEWER_CHECK(dollied.model_view->orbit.distance <
               homed.model_view->orbit.distance);
  // Shift+drag turns the lights, not the view.
  static_cast<void>(harness.frame(400.0F, 200.0F, false, {}, std::nullopt,
                                  gui::KeyMods::Shift));
  static_cast<void>(harness.frame(400.0F, 200.0F, true, {}, std::nullopt,
                                  gui::KeyMods::Shift));
  const editor::AssetViewerFrame lit = harness.frame(
      480.0F, 200.0F, true, {}, std::nullopt, gui::KeyMods::Shift);
  static_cast<void>(harness.frame(480.0F, 200.0F, false));
  VIEWER_CHECK(lit.model_view->look.rig_yaw != 0.0F);
  VIEWER_CHECK(lit.model_view->orbit == dollied.model_view->orbit);
  // T starts the turntable: one turn in twenty seconds.
  static_cast<void>(harness.type("t"));
  VIEWER_CHECK(harness.base.panel.turntable());
  harness.base.time += 0.2;
  static_cast<void>(harness.type({}));
  harness.base.time += 0.2;
  const editor::AssetViewerFrame turning = harness.type({});
  VIEWER_CHECK(near(turning.model_view->turntable, 0.02F, 1.0e-4F));
  // F frames the model again.
  VIEWER_CHECK(harness.type("f").model_view->orbit.target ==
               editor::stage_focus(lit.model_view->orbit, *harness.model.bounds)
                   .target);
}

void a_part_row_isolates_and_a_clip_row_plays() {
  ModelHarness harness;
  harness.model.state = editor::AssetViewerModelState::Ready;
  harness.model.facts = car_facts();
  harness.base.panel.open(model_id);
  static_cast<void>(harness.type({}));
  // The Parts tab, then its second row.
  harness.click(harness.tab_x(1U), info_tab_y);
  harness.click(400.0F, first_row_y + 23.0F + 11.0F);
  VIEWER_CHECK(harness.base.panel.isolated_part() == 1U);
  VIEWER_CHECK(harness.type({}).model_view->part == 1U);
  // Clicked again, every part shows.
  harness.click(400.0F, first_row_y + 23.0F + 11.0F);
  VIEWER_CHECK(!harness.base.panel.isolated_part().has_value());

  // The Animation tab: below its transport, the bind pose, then each clip.
  harness.click(harness.tab_x(2U), info_tab_y);
  const float drive_y = first_row_y + 30.0F + 23.0F * 2.0F + 11.0F;
  harness.click(400.0F, drive_y);
  VIEWER_CHECK(harness.base.panel.clip() == 1U);
  harness.base.time += 1.0;
  const editor::AssetViewerFrame playing = harness.type({});
  VIEWER_CHECK(playing.model_view->clip == 1U);
  VIEWER_CHECK(playing.model_view->animation_frame ==
               editor::asset_viewer_animation_rate);
  // The bind pose row clears it.
  harness.click(400.0F, first_row_y + 30.0F + 11.0F);
  VIEWER_CHECK(!harness.base.panel.clip().has_value());
  VIEWER_CHECK(harness.type({}).model_view->animation_frame == 0U);
}

void an_hdri_lights_the_model_once_chosen() {
  ModelHarness harness;
  harness.base.panel.open(model_id);
  harness.base.panel.set_hdri({});
  VIEWER_CHECK(harness.base.panel.look().environment ==
               editor::AssetViewerEnvironment::Studio);
  harness.base.panel.set_hdri("D:/hdri/hall.exr");
  const editor::AssetViewerFrame lit = harness.type({});
  VIEWER_CHECK(lit.model_view->look.environment ==
               editor::AssetViewerEnvironment::Hdri);
  VIEWER_CHECK(lit.model_view->look.hdri_source == "D:/hdri/hall.exr");
  // A new project forgets it.
  harness.base.panel.reset_project_state();
  VIEWER_CHECK(harness.base.panel.look().environment ==
               editor::AssetViewerEnvironment::Studio);
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

constexpr std::array<Case, 20U> cases{{
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
    {"a cut at a locked track or during a transition is refused",
     &a_cut_at_a_locked_track_or_during_a_transition_is_refused},
    {"the picture drags out carrying its range",
     &the_picture_drags_out_carrying_its_range},
    {"the wheel zooms about the pointer and Fit resets",
     &the_wheel_zooms_about_the_pointer_and_fit_resets},
    {"a project change forgets the asset",
     &a_project_change_forgets_the_asset},
    {"model bounds prefer the package and measure from the base",
     &model_bounds_prefer_the_package_and_measure_from_the_base},
    {"the look-dev scene places lights, floor and figure",
     &the_look_dev_scene_places_lights_floor_and_figure},
    {"the Stage or an image can light the model",
     &the_stage_or_an_image_can_light_the_model},
    {"a model opens framed and asks for its view",
     &a_model_opens_framed_and_asks_for_its_view},
    {"the model view orbits, frames and turns",
     &the_model_view_orbits_frames_and_turns},
    {"a part row isolates and a clip row plays",
     &a_part_row_isolates_and_a_clip_row_plays},
    {"an HDRI lights the model once chosen",
     &an_hdri_lights_the_model_once_chosen},
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
