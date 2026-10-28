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

} // namespace mediaserver::editor
