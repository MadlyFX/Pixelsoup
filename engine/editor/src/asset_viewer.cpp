#include <mediaserver/editor/asset_viewer.hpp>

#include <mediaserver/editor/library_panel.hpp>
#include <mediaserver/scene/math.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace mediaserver::editor {
namespace {

// Far enough in front of the picture that no zoom puts the eye inside it.
constexpr float camera_distance = 10.0F;

[[nodiscard]] bool usable(float value) noexcept {
  return std::isfinite(value) && value > 0.0F;
}

} // namespace

core::u32 asset_viewer_frame_count(const assets::MediaAsset &asset) noexcept {
  return std::max(asset.frame_count.value_or(1U), 1U);
}

assets::PixelDimensions
asset_viewer_pixels(const assets::MediaAsset &asset) noexcept {
  if (asset.dimensions.has_value() && asset.dimensions->width != 0U &&
      asset.dimensions->height != 0U) {
    return *asset.dimensions;
  }
  return assets::PixelDimensions{1920U, 1080U};
}

float asset_viewer_aspect(const assets::MediaAsset &asset) noexcept {
  const assets::PixelDimensions pixels = asset_viewer_pixels(asset);
  return static_cast<float>(pixels.width) / static_cast<float>(pixels.height);
}

assets::MediaFrameRate
asset_viewer_rate(const assets::MediaAsset &asset,
                  scene::MediaFrameRate show_rate) noexcept {
  if (asset.frame_rate.has_value() && asset.frame_rate->numerator != 0U &&
      asset.frame_rate->denominator != 0U) {
    return *asset.frame_rate;
  }
  return assets::MediaFrameRate{std::max(show_rate.numerator, 1U),
                                std::max(show_rate.denominator, 1U)};
}

scene::Scene asset_viewer_media_scene(const assets::MediaAsset &asset,
                                      core::u32 source_frame,
                                      const scene::RendererSettings &project) {
  scene::Scene viewer{};
  viewer.renderer_settings.backend = project.backend;
  viewer.renderer_settings.quality = project.quality;
  viewer.renderer_settings.color = project.color;
  viewer.environment.sky_color = scene::Vec3{0.0F, 0.0F, 0.0F};
  viewer.environment.ground_color = scene::Vec3{0.0F, 0.0F, 0.0F};

  const assets::PixelDimensions pixels = asset_viewer_pixels(asset);
  viewer.canvas = scene::Canvas{pixels.width, pixels.height};
  viewer.camera.eye = scene::Vec3{0.0F, 0.0F, camera_distance};
  viewer.camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
  viewer.camera.near_plane = 0.1F;
  viewer.camera.far_plane = 100.0F;

  const core::u32 count = asset_viewer_frame_count(asset);
  scene::MediaInstance clip{};
  clip.world = scene::scaling(scene::Vec3{asset_viewer_aspect(asset), 1.0F, 1.0F});
  clip.normal = scene::normal_basis(clip.world);
  clip.source = asset.source;
  clip.frame_index = std::min(source_frame, count - 1U);
  clip.loop_first = 0U;
  clip.loop_count = count;
  clip.asset = asset.id;
  viewer.media.push_back(std::move(clip));
  return viewer;
}

float asset_viewer_fit_scale(assets::PixelDimensions source, float width,
                             float height) noexcept {
  if (!usable(width) || !usable(height) || source.width == 0U ||
      source.height == 0U) {
    return 1.0F;
  }
  return std::min(width / static_cast<float>(source.width),
                  height / static_cast<float>(source.height));
}

float asset_viewer_scale(const AssetViewerFraming &framing,
                         assets::PixelDimensions source, float width,
                         float height) noexcept {
  return usable(framing.scale) ? framing.scale
                               : asset_viewer_fit_scale(source, width, height);
}

float asset_viewer_view_height(const AssetViewerFraming &framing,
                               assets::PixelDimensions source, float width,
                               float height) noexcept {
  const float scale = asset_viewer_scale(framing, source, width, height);
  if (!usable(height) || source.height == 0U || !usable(scale)) {
    return 1.0F;
  }
  // The picture is one world unit tall and `source.height` pixels tall, so
  // one world unit fills source.height * scale screen pixels.
  return height / (static_cast<float>(source.height) * scale);
}

scene::Camera asset_viewer_camera(const AssetViewerFraming &framing,
                                  assets::PixelDimensions source, float width,
                                  float height) noexcept {
  scene::Camera camera{};
  const bool fit = !usable(framing.scale);
  const float x = fit ? 0.0F : framing.center_x;
  const float y = fit ? 0.0F : framing.center_y;
  camera.eye = scene::Vec3{x, y, camera_distance};
  camera.target = scene::Vec3{x, y, 0.0F};
  camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
  camera.near_plane = 0.1F;
  camera.far_plane = 100.0F;
  camera.orthographic_height =
      asset_viewer_view_height(framing, source, width, height);
  return camera;
}

AssetViewerPictureRect
asset_viewer_picture_rect(const AssetViewerFraming &framing,
                          assets::PixelDimensions source, float width,
                          float height) noexcept {
  const float view_height =
      asset_viewer_view_height(framing, source, width, height);
  if (!usable(width) || !usable(height) || !usable(view_height)) {
    return {};
  }
  const bool fit = !usable(framing.scale);
  const float center_x = fit ? 0.0F : framing.center_x;
  const float center_y = fit ? 0.0F : framing.center_y;
  const float units = height / view_height; // area units per world unit
  const float aspect = source.height == 0U
                           ? 16.0F / 9.0F
                           : static_cast<float>(source.width) /
                                 static_cast<float>(source.height);
  const float left = -aspect * 0.5F;
  const float top = 0.5F;
  return AssetViewerPictureRect{
      width * 0.5F + (left - center_x) * units,
      height * 0.5F - (top - center_y) * units, aspect * units, units};
}

AssetViewerFraming asset_viewer_zoom(const AssetViewerFraming &framing,
                                     assets::PixelDimensions source,
                                     float width, float height, float factor,
                                     float at_x, float at_y) noexcept {
  if (!usable(factor) || !usable(width) || !usable(height)) {
    return framing;
  }
  const bool fit = !usable(framing.scale);
  const float scale = asset_viewer_scale(framing, source, width, height);
  const float view_height =
      asset_viewer_view_height(framing, source, width, height);
  const float center_x = fit ? 0.0F : framing.center_x;
  const float center_y = fit ? 0.0F : framing.center_y;
  // The world point under the pointer, which the zoom keeps there.
  const float units = height / view_height;
  const float world_x = center_x + (at_x - width * 0.5F) / units;
  const float world_y = center_y - (at_y - height * 0.5F) / units;
  AssetViewerFraming zoomed{};
  zoomed.scale = std::clamp(scale * factor, 1.0F / 64.0F, 64.0F);
  const float zoomed_units =
      height / asset_viewer_view_height(zoomed, source, width, height);
  zoomed.center_x = world_x - (at_x - width * 0.5F) / zoomed_units;
  zoomed.center_y = world_y + (at_y - height * 0.5F) / zoomed_units;
  return zoomed;
}

AssetViewerFraming asset_viewer_pan(const AssetViewerFraming &framing,
                                    assets::PixelDimensions source,
                                    float width, float height, float dx,
                                    float dy) noexcept {
  if (!usable(width) || !usable(height)) {
    return framing;
  }
  const bool fit = !usable(framing.scale);
  AssetViewerFraming panned{};
  panned.scale = asset_viewer_scale(framing, source, width, height);
  const float units =
      height / asset_viewer_view_height(framing, source, width, height);
  panned.center_x = (fit ? 0.0F : framing.center_x) - dx / units;
  panned.center_y = (fit ? 0.0F : framing.center_y) + dy / units;
  return panned;
}

std::string asset_viewer_timecode(const assets::MediaAsset &asset,
                                  scene::MediaFrameRate show_rate,
                                  core::u64 frame) {
  const assets::MediaFrameRate rate = asset_viewer_rate(asset, show_rate);
  core::i64 start = 0;
  bool drop = false;
  if (!asset.start_timecode.empty()) {
    if (const auto parsed = timecode_frames(asset.start_timecode, rate)) {
      start = *parsed;
      drop = asset.start_timecode.find(';') != std::string::npos ||
             asset.start_timecode.find('.') != std::string::npos;
    }
  }
  return timecode_text(start + static_cast<core::i64>(frame), rate, drop);
}

namespace {

// The look-dev props' render-history keys, apart from any document id.
constexpr core::u64 viewer_prop_identity = 0x4C4F'4F4B'4445'5600ULL;

[[nodiscard]] scene::Mat4 box_world(scene::Vec3 centre, scene::Vec3 size) {
  return scene::multiply(scene::translation(centre), scene::scaling(size));
}

void add_prop(scene::Scene &into, scene::Primitive primitive,
              const scene::Mat4 &world, scene::Vec3 colour, float roughness,
              core::u64 index) {
  scene::MeshInstance prop{};
  prop.mesh = scene::MeshKey{primitive, primitive == scene::Primitive::Sphere
                                            ? 32U
                                            : primitive == scene::Primitive::Cylinder
                                                  ? 32U
                                                  : 1U};
  prop.world = world;
  prop.normal = scene::normal_basis(world);
  prop.material.base_color = colour;
  prop.material.roughness = roughness;
  prop.source = doc::NodeId{viewer_prop_identity + index};
  prop.bake_static = false;
  into.instances.push_back(std::move(prop));
}

// `direction` turned by `yaw` about +Y, as the rig turns.
[[nodiscard]] scene::Vec3 turned(scene::Vec3 direction, scene::Turns yaw) {
  const float c = scene::deterministic::cos_turns(yaw);
  const float s = scene::deterministic::sin_turns(yaw);
  return scene::normalize(scene::Vec3{direction.x * c + direction.z * s,
                                      direction.y,
                                      -direction.x * s + direction.z * c});
}

[[nodiscard]] scene::DirectionalLight studio_light(scene::Vec3 towards,
                                                   scene::Vec3 colour,
                                                   float intensity,
                                                   scene::Turns yaw) {
  scene::DirectionalLight light{};
  // A directional light names the way its light travels: away from it.
  light.direction = -turned(towards, yaw);
  light.color = colour;
  light.intensity = intensity;
  light.bake_indirect = false;
  return light;
}

} // namespace

WorldBounds
asset_viewer_model_bounds(const assets::ModelAsset &asset,
                          const std::optional<WorldBounds> &loaded) noexcept {
  const auto usable_box = [](const WorldBounds &box) {
    const scene::Vec3 size = box.maximum - box.minimum;
    return std::isfinite(size.x) && std::isfinite(size.y) &&
           std::isfinite(size.z) && size.x >= 0.0F && size.y >= 0.0F &&
           size.z >= 0.0F;
  };
  if (loaded.has_value() && usable_box(*loaded)) {
    return *loaded;
  }
  if (asset.bounds.has_value()) {
    const assets::ModelBounds &b = *asset.bounds;
    const WorldBounds imported{
        scene::Vec3{static_cast<float>(b.minimum_x),
                    static_cast<float>(b.minimum_y),
                    static_cast<float>(b.minimum_z)},
        scene::Vec3{static_cast<float>(b.maximum_x),
                    static_cast<float>(b.maximum_y),
                    static_cast<float>(b.maximum_z)}};
    if (usable_box(imported)) {
      return imported;
    }
  }
  return WorldBounds{scene::Vec3{-0.5F, 0.0F, -0.5F},
                     scene::Vec3{0.5F, 1.0F, 0.5F}};
}

AssetViewerDimensions
asset_viewer_dimensions(const WorldBounds &bounds) noexcept {
  AssetViewerDimensions dimensions{};
  dimensions.width = bounds.maximum.x - bounds.minimum.x;
  dimensions.height = bounds.maximum.y - bounds.minimum.y;
  dimensions.depth = bounds.maximum.z - bounds.minimum.z;
  // The pivot is the model's origin; measured from the middle of its base.
  const scene::Vec3 base{(bounds.minimum.x + bounds.maximum.x) * 0.5F,
                         bounds.minimum.y,
                         (bounds.minimum.z + bounds.maximum.z) * 0.5F};
  dimensions.pivot_offset = scene::Vec3{} - base;
  return dimensions;
}

StageOrbit asset_viewer_model_home(const WorldBounds &bounds) noexcept {
  StageOrbit orbit{};
  orbit.yaw = 0.09F;
  orbit.pitch = 0.05F;
  return stage_focus(orbit, bounds);
}

scene::Scene asset_viewer_model_scene(const assets::ModelAsset &asset,
                                      const AssetViewerModelView &view,
                                      const WorldBounds &bounds,
                                      const scene::RendererSettings &project,
                                      const scene::Scene *stage) {
  scene::Scene viewer{};
  viewer.renderer_settings.backend = project.backend;
  viewer.renderer_settings.quality = project.quality;
  viewer.renderer_settings.color = project.color;
  viewer.renderer_settings.texture_budget_mib = project.texture_budget_mib;
  viewer.renderer_settings.texture_streaming = project.texture_streaming;
  viewer.renderer_settings.texture_mip_bias = project.texture_mip_bias;
  viewer.renderer_settings.photometric_scale = project.photometric_scale;
  viewer.show_rate = scene::MediaFrameRate{asset_viewer_animation_rate, 1U};
  viewer.show_frame = view.animation_frame;

  // The camera: the orbit over a neutral one, or over the Stage's for its
  // exposure when the Stage lights the model.
  scene::Camera base{};
  if (view.look.environment == AssetViewerEnvironment::Stage &&
      stage != nullptr) {
    base.aperture_f_number = stage->camera.aperture_f_number;
    base.shutter_seconds = stage->camera.shutter_seconds;
    base.iso = stage->camera.iso;
    base.exposure_compensation_ev = stage->camera.exposure_compensation_ev;
  }
  viewer.camera = stage_camera_for(base, view.orbit);

  switch (view.look.environment) {
  case AssetViewerEnvironment::Stage:
    if (stage != nullptr) {
      viewer.environment = stage->environment;
      viewer.directional_lights = stage->directional_lights;
      break;
    }
    [[fallthrough]];
  case AssetViewerEnvironment::Studio:
    viewer.environment.sky_color = scene::Vec3{0.20F, 0.21F, 0.23F};
    viewer.environment.ground_color = scene::Vec3{0.08F, 0.08F, 0.08F};
    viewer.environment.ambient = 1.0F;
    viewer.directional_lights.push_back(
        studio_light(scene::Vec3{-0.55F, 0.75F, 0.60F},
                     scene::Vec3{1.0F, 0.96F, 0.90F}, 1.6F, view.look.rig_yaw));
    viewer.directional_lights.push_back(
        studio_light(scene::Vec3{0.80F, 0.30F, 0.55F},
                     scene::Vec3{0.86F, 0.91F, 1.0F}, 0.45F, view.look.rig_yaw));
    viewer.directional_lights.push_back(
        studio_light(scene::Vec3{0.20F, 0.65F, -0.90F},
                     scene::Vec3{1.0F, 1.0F, 1.0F}, 1.0F, view.look.rig_yaw));
    break;
  case AssetViewerEnvironment::Hdri:
    viewer.environment.sky_color = scene::Vec3{};
    viewer.environment.ground_color = scene::Vec3{};
    viewer.environment.map_source = view.look.hdri_source;
    viewer.environment.map_intensity = 1.0F;
    viewer.environment.map_rotation = view.look.rig_yaw;
    break;
  }
  // The viewer is not a room: no baked transport, mirror or air.
  viewer.environment.baked.enabled = false;
  viewer.environment.planar.enabled = false;
  viewer.environment.fog_density = 0.0F;
  if (view.look.environment == AssetViewerEnvironment::Stage &&
      stage != nullptr && viewer.environment.physical_sky.enabled) {
    viewer.environment.map_source.clear();
  }

  const scene::Vec3 size = bounds.maximum - bounds.minimum;
  const float largest = std::max({size.x, size.y, size.z, 0.01F});
  // The floor meets the model's base, as a drop on the Stage rests it on a
  // surface; the turntable turns about the vertical, so the base stays put.
  const float ground =
      std::isfinite(bounds.minimum.y) ? bounds.minimum.y : 0.0F;
  if (view.look.floor) {
    // A floor reaching three model-widths each way, ruled every metre (or
    // every 10 cm or 10 m for a small or large model), with the lines
    // through the pivot drawn red along X and blue along Z, as on the Stage.
    const float step = largest < 0.5F ? 0.1F : largest < 50.0F ? 1.0F : 10.0F;
    const int lines = std::clamp(
        static_cast<int>(std::ceil(std::max(largest * 3.0F, 2.5F) / step)), 2,
        40);
    const float reach = static_cast<float>(lines) * step;
    add_prop(viewer, scene::Primitive::Plane,
             box_world(scene::Vec3{0.0F, ground, 0.0F},
                       scene::Vec3{reach * 2.0F, 1.0F, reach * 2.0F}),
             scene::Vec3{0.24F, 0.24F, 0.24F}, 0.85F, 0U);
    const float width = step * 0.02F;
    const float lift = step * 0.002F;
    core::u64 index = 1U;
    if (view.look.grid) {
      for (int line = -lines; line <= lines; ++line) {
        const float at = static_cast<float>(line) * step;
        const bool axis = line == 0;
        add_prop(viewer, scene::Primitive::Box,
                 box_world(scene::Vec3{0.0F, ground + lift * 0.5F, at},
                           scene::Vec3{reach * 2.0F, lift, width}),
                 axis ? scene::Vec3{0.50F, 0.10F, 0.10F}
                      : scene::Vec3{0.12F, 0.12F, 0.12F},
                 0.9F, index++);
        add_prop(viewer, scene::Primitive::Box,
                 box_world(scene::Vec3{at, ground + lift * 0.5F, 0.0F},
                           scene::Vec3{width, lift, reach * 2.0F}),
                 axis ? scene::Vec3{0.10F, 0.18F, 0.50F}
                      : scene::Vec3{0.12F, 0.12F, 0.12F},
                 0.9F, index++);
      }
    }
  }
  if (view.look.figure) {
    // A 1.8 m capsule, 40 cm across, standing on the floor a little to the
    // model's right.
    constexpr float radius = 0.2F;
    const float x = std::max(bounds.maximum.x, 0.0F) + 0.4F + radius;
    const scene::Vec3 colour{0.50F, 0.50F, 0.48F};
    add_prop(viewer, scene::Primitive::Cylinder,
             box_world(scene::Vec3{x, ground + 0.9F, 0.0F},
                       scene::Vec3{radius * 2.0F, 1.8F - radius * 2.0F,
                                   radius * 2.0F}),
             colour, 0.6F, 200U);
    add_prop(viewer, scene::Primitive::Sphere,
             box_world(scene::Vec3{x, ground + radius, 0.0F},
                       scene::Vec3{radius * 2.0F, radius * 2.0F, radius * 2.0F}),
             colour, 0.6F, 201U);
    add_prop(viewer, scene::Primitive::Sphere,
             box_world(scene::Vec3{x, ground + 1.8F - radius, 0.0F},
                       scene::Vec3{radius * 2.0F, radius * 2.0F, radius * 2.0F}),
             colour, 0.6F, 202U);
  }

  scene::ModelInstance model{};
  model.world = scene::rotation(view.turntable, 0.0F, 0.0F);
  model.normal = scene::normal_basis(model.world);
  model.source = asset.source;
  // The Library id stands in for the one placement the viewer draws.
  model.source_node = asset.id;
  model.bake_static = false;
  model.animation.enabled = view.clip.has_value();
  model.animation.clip = view.clip.value_or(0U);
  model.animation.loop = view.loop;
  model.animation.start_frame = 0U;
  viewer.models.push_back(std::move(model));
  return viewer;
}

} // namespace mediaserver::editor
