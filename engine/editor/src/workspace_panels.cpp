#include <charconv>
#include <mediaserver/editor/workspace_panels.hpp>

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/editor/mapping_panels.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <tinyfiledialogs.h>

namespace mediaserver::editor {

std::string compositor_scene_layer_detail(
    doc::NodeId scene_node, std::span<const ElementInfo> elements) {
  const auto element = std::find_if(
      elements.begin(), elements.end(),
      [scene_node](const ElementInfo &candidate) {
        return candidate.node == scene_node;
      });
  if (element != elements.end()) {
    return element_display_name(*element);
  }
  return "OBJECT #" + std::to_string(scene_node.value());
}

namespace {

namespace gui = mediaserver::gui;

constexpr float title_height = 25.0F;
constexpr float search_height = 25.0F;
constexpr float tree_row_height = 20.0F;
constexpr float section_row_height = 21.0F;
constexpr float inset = 6.0F;

[[nodiscard]] gui::UiId mix(gui::UiId base, std::string_view name,
                            core::u64 index = 0U) noexcept {
  core::u64 hash = base ^ 0x9E3779B97F4A7C15ULL;
  for (const char character : name) {
    hash ^= static_cast<core::u8>(character);
    hash *= 1099511628211ULL;
  }
  hash ^= index + 0x9E3779B9ULL;
  hash *= 1099511628211ULL;
  return hash;
}

[[nodiscard]] gui::UiColor with_alpha(gui::UiColor color,
                                      float opacity) noexcept {
  color.a = opacity;
  return color;
}

[[nodiscard]] gui::UiColor scale(gui::UiColor color, float amount) noexcept {
  color.r *= amount;
  color.g *= amount;
  color.b *= amount;
  return color;
}

void border(gui::UiContext &ctx, const gui::Rect &bounds, gui::UiColor color,
            float thickness = 1.0F) {
  auto &draw = ctx.draw_list();
  draw.add_rect(bounds.x, bounds.y, bounds.width, thickness, color);
  draw.add_rect(bounds.x, bounds.y + bounds.height - thickness, bounds.width,
                thickness, color);
  draw.add_rect(bounds.x, bounds.y, thickness, bounds.height, color);
  draw.add_rect(bounds.x + bounds.width - thickness, bounds.y, thickness,
                bounds.height, color);
}

void draw_circle(gui::UiContext &ctx, float centre_x, float centre_y,
                 float radius, gui::UiColor color) {
  constexpr std::size_t point_count = 12U;
  constexpr float tau = 6.28318530718F;
  std::array<gui::UiPoint, point_count> points{};
  for (std::size_t index = 0U; index < point_count; ++index) {
    const float angle =
        tau * static_cast<float>(index) / static_cast<float>(point_count);
    points[index] = gui::UiPoint{centre_x + std::cos(angle) * radius,
                                 centre_y + std::sin(angle) * radius};
  }
  ctx.draw_list().add_convex_poly(points, color);
}

[[nodiscard]] std::string element_name(const ElementInfo &element) {
  return element_display_name(element);
}

[[nodiscard]] std::string lowercase(std::string_view input) {
  std::string result;
  result.reserve(input.size());
  for (const char character : input) {
    if (character >= 'A' && character <= 'Z') {
      result.push_back(static_cast<char>(character - 'A' + 'a'));
    } else {
      result.push_back(character);
    }
  }
  return result;
}

[[nodiscard]] bool matches(std::string_view name, std::string_view query) {
  return query.empty() ||
         lowercase(name).find(lowercase(query)) != std::string::npos;
}

[[nodiscard]] bool selected(doc::NodeId node,
                            std::span<const doc::NodeId> selection) noexcept {
  return std::find(selection.begin(), selection.end(), node) != selection.end();
}

void panel_title(gui::UiContext &ctx, const gui::Rect &bounds,
                 std::string_view title, std::string_view trailing = {}) {
  const gui::Rect header{bounds.x, bounds.y, bounds.width,
                         std::min(bounds.height, title_height)};
  ctx.panel(header, ctx.theme().background);
  ctx.panel(
      gui::Rect{header.x, header.y + header.height - 1.0F, header.width, 1.0F},
      ctx.theme().divider);
  if (!title.empty()) {
    ctx.label(gui::Rect{header.x + 8.0F, header.y, header.width - 16.0F,
                        header.height},
              title, ctx.theme().text);
  }
  if (!trailing.empty()) {
    const float width = ctx.font_width(trailing);
    ctx.label(gui::Rect{header.x + header.width - width - 8.0F, header.y, width,
                        header.height},
              trailing, ctx.theme().text_muted);
  }
}

void search_field(gui::UiContext &ctx, gui::UiId id, const gui::Rect &bounds,
                  std::string &value, std::string_view placeholder) {
  const gui::Rect field{bounds.x + inset, bounds.y + 3.0F,
                        std::max(bounds.width - inset * 2.0F, 0.0F),
                        std::max(bounds.height - 6.0F, 0.0F)};
  static_cast<void>(ctx.text_field(id, field, value));
  if (value.empty() && ctx.focus() != id) {
    ctx.label(
        gui::Rect{field.x + 19.0F, field.y, field.width - 23.0F, field.height},
        placeholder, ctx.theme().text_muted);
  }
  // A small magnifier, built from the same primitives as the rest of the
  // chrome, keeps the search affordance readable with no icon dependency.
  draw_circle(ctx, field.x + 9.0F, field.y + field.height * 0.46F, 3.0F,
              ctx.theme().text_muted);
  draw_circle(ctx, field.x + 9.0F, field.y + field.height * 0.46F, 1.7F,
              ctx.theme().widget);
  ctx.draw_list().add_line(
      field.x + 11.0F, field.y + field.height * 0.46F + 2.0F, field.x + 14.0F,
      field.y + field.height * 0.46F + 5.0F, 1.0F, ctx.theme().text_muted);
}

struct LibraryItem final {
  std::string_view name;
  std::string_view detail;
  LibraryAddAction action;
  std::size_t category;
};

enum class LibraryCategory : std::size_t {
  Generators,
  Media,
  Models,
  Lights,
  Count,
};

constexpr std::array<std::string_view, 4U> library_category_names{
    "GENERATORS", "MEDIA", "MODELS", "LIGHTS"};

constexpr std::array<LibraryItem, 7U> library_items{{
    {"Box", "3D primitive", LibraryAddAction::Box, 0U},
    {"Plane", "2D surface", LibraryAddAction::Plane, 0U},
    {"Sphere", "3D primitive", LibraryAddAction::Sphere, 0U},
    {"Camera", "Tracked stage camera", LibraryAddAction::Camera, 0U},
    {"Directional Light", "Global sun", LibraryAddAction::DirectionalLight, 3U},
    {"Point Light", "Local emitter", LibraryAddAction::PointLight, 3U},
    // A practical: the lamp, window or screen an interior set is actually lit
    // by. Its own entry rather than a point the operator converts afterwards,
    // because the cone and its aim are the whole point of placing one.
    {"Spot Light", "Practical with a cone", LibraryAddAction::SpotLight, 3U},
}};

constexpr float asset_row_height = 48.0F;
constexpr float thumbnail_asset_row_height = 68.0F;
constexpr float thumbnail_preview_width = 96.0F;
constexpr float thumbnail_preview_aspect = 16.0F / 9.0F;
constexpr float import_row_height = 26.0F;
constexpr float empty_row_height = 24.0F;

[[nodiscard]] std::string_view
library_thumbnail_status(LibraryThumbnailState state) noexcept {
  switch (state) {
  case LibraryThumbnailState::Placeholder:
    return "THUMBNAIL";
  case LibraryThumbnailState::Queued:
    return "THUMBNAIL QUEUED";
  case LibraryThumbnailState::Generating:
    return "THUMBNAIL GENERATING";
  case LibraryThumbnailState::Ready:
    return "";
  case LibraryThumbnailState::Stale:
    return "THUMBNAIL STALE";
  case LibraryThumbnailState::Failed:
    return "THUMBNAIL FAILED";
  }
  return "THUMBNAIL";
}

[[nodiscard]] gui::UiColor
library_thumbnail_color(const gui::UiContext &ctx,
                        LibraryThumbnailState state) noexcept {
  switch (state) {
  case LibraryThumbnailState::Placeholder:
    return ctx.theme().text_muted;
  case LibraryThumbnailState::Queued:
    return ctx.theme().accent;
  case LibraryThumbnailState::Generating:
    return ctx.theme().keyframe;
  case LibraryThumbnailState::Ready:
    return ctx.theme().good;
  case LibraryThumbnailState::Stale:
    return ctx.theme().playhead;
  case LibraryThumbnailState::Failed:
    return ctx.theme().warning;
  }
  return ctx.theme().text_muted;
}

[[nodiscard]] std::string_view
library_proxy_reason_detail(assets::ProxyPlaybackReason reason) noexcept {
  switch (reason) {
  case assets::ProxyPlaybackReason::None:
    return "READY";
  case assets::ProxyPlaybackReason::SourcePolicy:
    return "SOURCE POLICY";
  case assets::ProxyPlaybackReason::SourceUnavailable:
    return "SOURCE UNAVAILABLE";
  case assets::ProxyPlaybackReason::InvalidConfiguration:
    return "INVALID CONFIGURATION";
  case assets::ProxyPlaybackReason::ProxyNotConfigured:
    return "NOT CONFIGURED";
  case assets::ProxyPlaybackReason::SourceFingerprintUnavailable:
    return "FINGERPRINT UNAVAILABLE";
  case assets::ProxyPlaybackReason::CacheObservationMissing:
    return "CACHE UNAVAILABLE";
  case assets::ProxyPlaybackReason::CacheKeyMismatch:
    return "CACHE KEY MISMATCH";
  case assets::ProxyPlaybackReason::CacheMissing:
    return "MISSING";
  case assets::ProxyPlaybackReason::CacheQueued:
    return "QUEUED";
  case assets::ProxyPlaybackReason::CacheGenerating:
    return "GENERATING";
  case assets::ProxyPlaybackReason::CacheStale:
    return "STALE";
  case assets::ProxyPlaybackReason::CacheFailed:
    return "FAILED";
  case assets::ProxyPlaybackReason::CacheCancelled:
    return "CANCELLED";
  case assets::ProxyPlaybackReason::FrameMapMissing:
    return "FRAME MAP MISSING";
  case assets::ProxyPlaybackReason::FrameMapKeyMismatch:
    return "FRAME MAP KEY MISMATCH";
  case assets::ProxyPlaybackReason::FrameMapInvalid:
    return "FRAME MAP INVALID";
  }
  return "UNKNOWN";
}

[[nodiscard]] std::string_view
library_proxy_issue_detail(LibraryProxyRuntimeIssue issue) noexcept {
  switch (issue) {
  case LibraryProxyRuntimeIssue::None:
    return {};
  case LibraryProxyRuntimeIssue::MissingAssetIdentity:
    return "ASSET ID MISSING";
  case LibraryProxyRuntimeIssue::AssetNotFound:
    return "ASSET NOT FOUND";
  case LibraryProxyRuntimeIssue::AssetKindMismatch:
    return "ASSET KIND MISMATCH";
  case LibraryProxyRuntimeIssue::PlacementSourceMismatch:
    return "PLACEMENT SOURCE MISMATCH";
  case LibraryProxyRuntimeIssue::AvailabilityMissing:
    return "SOURCE STATUS UNAVAILABLE";
  case LibraryProxyRuntimeIssue::CacheInspectionFailed:
    return "CACHE INSPECTION FAILED";
  case LibraryProxyRuntimeIssue::ArtifactLoadFailed:
    return "ARTIFACT LOAD FAILED";
  case LibraryProxyRuntimeIssue::ArtifactMetadataMismatch:
    return "SOURCE METADATA MISMATCH";
  case LibraryProxyRuntimeIssue::FrameMappingFailed:
    return "FRAME MAPPING FAILED";
  case LibraryProxyRuntimeIssue::MaterializationFailed:
    return "MATERIALIZATION FAILED";
  }
  return "UNKNOWN";
}

[[nodiscard]] gui::UiColor
library_proxy_runtime_color(const gui::UiContext &ctx,
                            LibraryProxyRuntimeState state) noexcept {
  switch (state) {
  case LibraryProxyRuntimeState::Source:
    return ctx.theme().text_muted;
  case LibraryProxyRuntimeState::ProxyReady:
    return ctx.theme().good;
  case LibraryProxyRuntimeState::PreferFallback:
    return ctx.theme().playhead;
  case LibraryProxyRuntimeState::RequiredUnready:
  case LibraryProxyRuntimeState::Failed:
    return ctx.theme().warning;
  }
  return ctx.theme().text_muted;
}

[[nodiscard]] gui::Rect intersect(const gui::Rect &a,
                                  const gui::Rect &b) noexcept {
  const float left = std::max(a.x, b.x);
  const float top = std::max(a.y, b.y);
  const float right = std::min(a.x + a.width, b.x + b.width);
  const float bottom = std::min(a.y + a.height, b.y + b.height);
  return gui::Rect{left, top, std::max(right - left, 0.0F),
                   std::max(bottom - top, 0.0F)};
}

[[nodiscard]] gui::Interaction clipped_grab(gui::UiContext &ctx, gui::UiId id,
                                            const gui::Rect &bounds,
                                            const gui::Rect &clip) noexcept {
  const gui::Rect hit = intersect(bounds, clip);
  return hit.width > 0.0F && hit.height > 0.0F ? ctx.grab(id, hit)
                                               : gui::Interaction{};
}

[[nodiscard]] std::string asset_detail(const assets::Asset &asset) {
  std::array<char, 72U> text{};
  int length = 0;
  if (const auto *const media = std::get_if<assets::MediaAsset>(&asset)) {
    if (media->dimensions.has_value() && media->frame_count.has_value()) {
      length = std::snprintf(text.data(), text.size(), "%u x %u | %u FR",
                             media->dimensions->width,
                             media->dimensions->height, *media->frame_count);
    } else if (media->dimensions.has_value()) {
      length =
          std::snprintf(text.data(), text.size(), "%u x %u",
                        media->dimensions->width, media->dimensions->height);
    } else if (media->frame_count.has_value()) {
      length = std::snprintf(text.data(), text.size(), "%u FRAMES",
                             *media->frame_count);
    } else {
      return "METADATA NOT PROBED";
    }
  } else {
    const auto &model = std::get<assets::ModelAsset>(asset);
    if (model.triangle_count.has_value() && model.material_count.has_value()) {
      length =
          std::snprintf(text.data(), text.size(), "%llu TRIS | %u MATS",
                        static_cast<unsigned long long>(*model.triangle_count),
                        *model.material_count);
    } else if (model.triangle_count.has_value()) {
      length =
          std::snprintf(text.data(), text.size(), "%llu TRIANGLES",
                        static_cast<unsigned long long>(*model.triangle_count));
    } else if (model.material_count.has_value()) {
      length = std::snprintf(text.data(), text.size(), "%u MATERIALS",
                             *model.material_count);
    } else if (model.bounds.has_value()) {
      return "BOUNDS STORED";
    } else {
      return "METADATA NOT PROBED";
    }
  }
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] std::string_view
library_proxy_label(assets::ProxyPlaybackPolicy policy) noexcept {
  switch (policy) {
  case assets::ProxyPlaybackPolicy::Source:
    return "SRC";
  case assets::ProxyPlaybackPolicy::PreferProxy:
    return "AUTO";
  case assets::ProxyPlaybackPolicy::RequireProxy:
    return "REQ";
  }
  return "?";
}

[[nodiscard]] const LibraryProxyRuntimeObservation *
library_proxy_runtime_observation(
    const assets::MediaAsset &asset,
    std::span<const LibraryProxyRuntimeObservation> observations) noexcept {
  const std::optional<assets::DerivativeKey> requested =
      assets::requested_proxy_key(asset);
  const auto found = std::find_if(
      observations.begin(), observations.end(),
      [&](const LibraryProxyRuntimeObservation &observation) {
        return observation.asset == asset.id && observation.key == requested;
      });
  return found == observations.end() ? nullptr : std::addressof(*found);
}

[[nodiscard]] bool asset_matches(const assets::Asset &asset,
                                 std::string_view detail,
                                 std::string_view query) {
  return matches(assets::asset_display_name(asset), query) ||
         matches(assets::asset_source(asset), query) ||
         matches(detail, query) ||
         matches(assets::asset_kind(asset) == assets::AssetKind::Media
                     ? std::string_view{"media video image"}
                     : std::string_view{"model 3d gltf obj"},
                 query);
}

void draw_library_icon(gui::UiContext &ctx, LibraryAddAction action,
                       const gui::Rect &bounds) {
  const gui::UiColor color =
      action == LibraryAddAction::DirectionalLight ||
              action == LibraryAddAction::PointLight ||
              action == LibraryAddAction::SpotLight
          ? ctx.theme().warning
          : (action == LibraryAddAction::Media ? ctx.theme().accent
                                               : ctx.theme().keyframe);
  auto &draw = ctx.draw_list();
  const float centre_x = bounds.x + bounds.width * 0.5F;
  const float centre_y = bounds.y + bounds.height * 0.5F;
  if (action == LibraryAddAction::DirectionalLight ||
      action == LibraryAddAction::PointLight ||
      action == LibraryAddAction::SpotLight) {
    draw_circle(ctx, centre_x, centre_y, 4.0F, color);
    draw.add_line(centre_x - 7.0F, centre_y, centre_x - 5.0F, centre_y, 1.0F,
                  color);
    draw.add_line(centre_x + 5.0F, centre_y, centre_x + 7.0F, centre_y, 1.0F,
                  color);
    return;
  }
  if (action == LibraryAddAction::Media) {
    draw.add_rect(bounds.x + 2.0F, bounds.y + 3.0F, bounds.width - 4.0F,
                  bounds.height - 6.0F, with_alpha(color, 0.24F));
    border(ctx,
           gui::Rect{bounds.x + 2.0F, bounds.y + 3.0F, bounds.width - 4.0F,
                     bounds.height - 6.0F},
           color);
    draw.add_triangle(centre_x - 2.0F, centre_y - 4.0F, centre_x - 2.0F,
                      centre_y + 4.0F, centre_x + 4.0F, centre_y, color);
    return;
  }
  if (action == LibraryAddAction::Model) {
    draw.add_line(bounds.x + 2.0F, bounds.y + bounds.height - 3.0F, centre_x,
                  bounds.y + 2.0F, 1.0F, color);
    draw.add_line(centre_x, bounds.y + 2.0F, bounds.x + bounds.width - 2.0F,
                  bounds.y + bounds.height - 3.0F, 1.0F, color);
    draw.add_line(bounds.x + 2.0F, bounds.y + bounds.height - 3.0F,
                  bounds.x + bounds.width - 2.0F,
                  bounds.y + bounds.height - 3.0F, 1.0F, color);
    return;
  }
  const gui::Rect cube{bounds.x + 3.0F, bounds.y + 3.0F, bounds.width - 6.0F,
                       bounds.height - 6.0F};
  draw.add_rect(cube.x, cube.y, cube.width, cube.height,
                with_alpha(color, 0.18F));
  border(ctx, cube, color);
  if (action == LibraryAddAction::Sphere) {
    draw_circle(ctx, centre_x, centre_y, cube.width * 0.46F, color);
    draw_circle(ctx, centre_x, centre_y, cube.width * 0.33F,
                ctx.theme().widget);
  } else if (action == LibraryAddAction::Plane) {
    draw.add_line(cube.x, cube.y + cube.height, cube.x + cube.width, cube.y,
                  1.0F, color);
  }
}

void draw_cpu_decode_warning(gui::UiContext &ctx, const gui::Rect &bounds) {
  auto &draw = ctx.draw_list();
  const gui::UiColor red = ctx.theme().warning;
  const gui::UiColor cutout = scale(ctx.theme().row, 0.72F);
  const float centre_x = bounds.x + bounds.width * 0.5F;
  draw.add_triangle(centre_x, bounds.y, bounds.x + bounds.width,
                    bounds.y + bounds.height, bounds.x,
                    bounds.y + bounds.height, red);
  draw.add_line(centre_x, bounds.y + 4.0F, centre_x,
                bounds.y + bounds.height - 4.5F, 1.5F, cutout);
  draw.add_rect(centre_x - 0.8F, bounds.y + bounds.height - 3.0F, 1.6F, 1.6F,
                cutout);
}

void draw_plus(gui::UiContext &ctx, float x, float y, gui::UiColor color) {
  ctx.draw_list().add_line(x - 3.0F, y, x + 3.0F, y, 1.0F, color);
  ctx.draw_list().add_line(x, y - 3.0F, x, y + 3.0F, 1.0F, color);
}

void draw_remove(gui::UiContext &ctx, float x, float y, gui::UiColor color) {
  ctx.draw_list().add_line(x - 3.0F, y - 3.0F, x + 3.0F, y + 3.0F, 1.0F, color);
  ctx.draw_list().add_line(x + 3.0F, y - 3.0F, x - 3.0F, y + 3.0F, 1.0F, color);
}

constexpr float compositor_toolbar_height = 26.0F;
// Node layout, in graph units at 100% zoom. Widths fit each node's own title
// and port names, and heights follow the editor font's line height, so no
// label is cut off whatever font or display scale the editor runs at.
constexpr float compositor_node_min_width = 176.0F;
constexpr float compositor_node_max_width = 360.0F;
constexpr float compositor_node_padding = 10.0F;
// From a port's dot to the start of its name.
constexpr float compositor_pin_inset = 13.0F;
// Between an input's name and an output's name on the same row.
constexpr float compositor_pin_label_gap = 18.0F;
constexpr float compositor_port_radius = 5.0F;
// Text sizes, as multiples of the UI font, before zoom.
constexpr float compositor_title_text = 1.0F;
constexpr float compositor_detail_text = 0.875F;
constexpr float compositor_pin_text = 0.875F;
// Screen-space reach of the pointer, independent of zoom: how near a wire
// end has to come to a compatible port to snap to it, and how near the
// pointer has to be to a wire to pick it.
constexpr float compositor_port_snap_radius = 22.0F;
constexpr float compositor_wire_hit_radius = 6.0F;
// A released move whose edit never arrives - refused by the Director, say -
// stops overriding the graph after this many frames.
constexpr core::u32 compositor_pending_move_frames = 120U;
constexpr float compositor_minimum_zoom = 0.35F;
constexpr float compositor_maximum_zoom = 2.5F;
constexpr float compositor_zoom_step = 1.18F;

enum class CompositorNodeCategory : core::u8 {
  Input,
  Color,
  Keying,
  Blur,
  Texture,
  Spatial,
  Combine,
  Count,
};

struct CompositorPaletteItem final {
  compositor::NodeKind kind;
  CompositorNodeCategory category;
  std::string_view search_terms;
};

constexpr std::array<std::string_view, 7U> compositor_category_names{{
    "INPUTS",
    "COLOR",
    "KEYING",
    "BLUR",
    "TEXTURE",
    "SPATIAL / 3D",
    "COMBINE & OUTPUT",
}};

constexpr std::array<CompositorPaletteItem, 20U> compositor_palette_items{{
    {compositor::NodeKind::SceneLayer, CompositorNodeCategory::Input,
     "source object timeline layer"},
    {compositor::NodeKind::Brightness, CompositorNodeCategory::Color,
     "exposure gain"},
    {compositor::NodeKind::Saturation, CompositorNodeCategory::Color,
     "colour color intensity"},
    {compositor::NodeKind::Levels, CompositorNodeCategory::Color,
     "tonal range"},
    {compositor::NodeKind::LiftGammaGain, CompositorNodeCategory::Color,
     "shadows midtones highlights"},
    {compositor::NodeKind::ContrastPivot, CompositorNodeCategory::Color,
     "contrast pivot"},
    {compositor::NodeKind::ColorMatrix, CompositorNodeCategory::Color,
     "channel mixer rgb matrix"},
    {compositor::NodeKind::WhiteBalance, CompositorNodeCategory::Color,
     "temperature tint"},
    {compositor::NodeKind::Curves, CompositorNodeCategory::Color,
     "curve tonal"},
    {compositor::NodeKind::ChromaKey, CompositorNodeCategory::Keying,
     "green screen colour key"},
    {compositor::NodeKind::LumaKey, CompositorNodeCategory::Keying,
     "luminance key"},
    {compositor::NodeKind::Matte, CompositorNodeCategory::Keying,
     "alpha luma mask"},
    {compositor::NodeKind::Blur, CompositorNodeCategory::Blur,
     "gaussian smooth"},
    {compositor::NodeKind::BoxBlur, CompositorNodeCategory::Blur, "box smooth"},
    {compositor::NodeKind::DirectionalBlur, CompositorNodeCategory::Blur,
     "motion angle"},
    {compositor::NodeKind::FilmGrain, CompositorNodeCategory::Texture,
     "noise texture"},
    {compositor::NodeKind::RadialArray, CompositorNodeCategory::Spatial,
     "array instances radial spatial 3d"},
    {compositor::NodeKind::LedCarProcess, CompositorNodeCategory::Spatial,
     "led virtual production car process plates volume reflection ceiling"},
    {compositor::NodeKind::Composite, CompositorNodeCategory::Combine,
     "merge combine layers"},
    {compositor::NodeKind::Output, CompositorNodeCategory::Combine,
     "program final render"},
}};

[[nodiscard]] constexpr std::size_t
category_index(CompositorNodeCategory category) noexcept {
  return static_cast<std::size_t>(category);
}

struct CompositorNodeVisual final {
  const compositor::GraphNode *node{};
  gui::Rect bounds;
};

struct CompositorPortVisual final {
  compositor::EdgeEndpoint endpoint;
  const compositor::PortDescriptor *descriptor{};
  // The dot's centre.
  float x{};
  float y{};
  // Its whole row on that side of the node: dot and name, so a press or a
  // drop does not have to find a ten-pixel circle.
  gui::Rect hit;
};

// Wires and ports are coloured by what flows through them, so an operator
// can see which ports accept a wire before trying. The palette stays cool:
// warm amber belongs to the playhead alone.
[[nodiscard]] gui::UiColor port_color(const gui::UiContext &,
                                      compositor::PortValueType type) noexcept {
  switch (type) {
  case compositor::PortValueType::Image:
    return gui::UiColor{0.380F, 0.741F, 0.949F, 1.0F};
  case compositor::PortValueType::Effect:
    return gui::UiColor{0.722F, 0.580F, 0.976F, 1.0F};
  case compositor::PortValueType::TransformSet:
    return gui::UiColor{0.412F, 0.808F, 0.698F, 1.0F};
  }
  return gui::UiColor{0.380F, 0.741F, 0.949F, 1.0F};
}

[[nodiscard]] gui::UiColor blend_color(gui::UiColor from, gui::UiColor to,
                                       float amount) noexcept {
  return gui::UiColor{from.r + (to.r - from.r) * amount,
                      from.g + (to.g - from.g) * amount,
                      from.b + (to.b - from.b) * amount,
                      from.a + (to.a - from.a) * amount};
}

// Each value type has its own port shape as well as its own colour, so the
// difference survives colour-blindness and a dimmed, incompatible port: an
// image is a circle, an effect a diamond, a transform set a square.
void draw_port_shape(gui::UiContext &ctx, compositor::PortValueType type,
                     float x, float y, float radius, gui::UiColor color) {
  switch (type) {
  case compositor::PortValueType::Image:
    draw_circle(ctx, x, y, radius, color);
    return;
  case compositor::PortValueType::Effect: {
    const float reach = radius * 1.25F;
    const std::array<gui::UiPoint, 4U> diamond{{{x, y - reach},
                                                {x + reach, y},
                                                {x, y + reach},
                                                {x - reach, y}}};
    ctx.draw_list().add_convex_poly(diamond, color);
    return;
  }
  case compositor::PortValueType::TransformSet: {
    const float half = radius * 0.9F;
    const std::array<gui::UiPoint, 4U> square{{{x - half, y - half},
                                               {x + half, y - half},
                                               {x + half, y + half},
                                               {x - half, y + half}}};
    ctx.draw_list().add_convex_poly(square, color);
    return;
  }
  }
}

// A connected port is filled; an open one is a ring showing the node behind.
void draw_port(gui::UiContext &ctx, compositor::PortValueType type, float x,
               float y, float radius, gui::UiColor color, bool filled,
               gui::UiColor hollow) {
  draw_port_shape(ctx, type, x, y, radius, color);
  if (!filled) {
    draw_port_shape(ctx, type, x, y,
                    std::max(radius - std::max(1.5F, radius * 0.4F), 0.5F),
                    hollow);
  }
}

// One accent per Add Node category, so a node reads as the same family it
// was picked from.
[[nodiscard]] gui::UiColor
compositor_category_color(const gui::UiContext &ctx,
                          CompositorNodeCategory category) noexcept {
  switch (category) {
  case CompositorNodeCategory::Input:
    return port_color(ctx, compositor::PortValueType::Image);
  case CompositorNodeCategory::Color:
    return gui::UiColor{0.890F, 0.502F, 0.706F, 1.0F};
  case CompositorNodeCategory::Keying:
    return gui::UiColor{0.471F, 0.808F, 0.471F, 1.0F};
  case CompositorNodeCategory::Blur:
    return gui::UiColor{0.553F, 0.635F, 0.976F, 1.0F};
  case CompositorNodeCategory::Texture:
    return gui::UiColor{0.722F, 0.671F, 0.600F, 1.0F};
  case CompositorNodeCategory::Spatial:
    return port_color(ctx, compositor::PortValueType::TransformSet);
  case CompositorNodeCategory::Combine:
  case CompositorNodeCategory::Count:
    break;
  }
  return ctx.theme().text;
}

[[nodiscard]] gui::UiColor
compositor_node_color(const gui::UiContext &ctx,
                      compositor::NodeKind kind) noexcept {
  const auto found =
      std::find_if(compositor_palette_items.begin(),
                   compositor_palette_items.end(),
                   [kind](const CompositorPaletteItem &item) noexcept {
                     return item.kind == kind;
                   });
  return compositor_category_color(ctx, found == compositor_palette_items.end()
                                            ? CompositorNodeCategory::Combine
                                            : found->category);
}

// `text` at `size` times the UI font, in graph units. Zoomed text is drawn
// from the nearest baked size at or below what it asks for, so the nominal
// scaled width bounds it; a size baked exactly is measured as drawn.
[[nodiscard]] float compositor_text_width(const gui::UiContext &ctx,
                                          std::string_view text, float size) {
  const float nominal = ctx.font_width(text) * size;
  const std::optional<float> baked = ctx.text_size_at_most(size);
  return baked.has_value() && *baked == size
             ? std::max(nominal, ctx.font_width(text, size))
             : nominal;
}

// A line of that text, rounded up to whole units so the rows below it stay
// on the pixel grid at 100% and a glyph's rounded baseline cannot spill out.
[[nodiscard]] float compositor_text_height(const gui::UiContext &ctx,
                                           float size) {
  const std::optional<float> baked = ctx.text_size_at_most(size);
  return std::ceil(baked.has_value() && *baked == size
                       ? std::max(ctx.line_height() * size,
                                  ctx.line_height(size))
                       : ctx.line_height() * size);
}

// Draws `text` at `size` times the UI font scaled by the graph's zoom, from
// the largest baked size that fits. Returns false, drawing nothing, when even
// the smallest baked text is too big - far zoomed out - so a caller can skip
// detail an operator could not read anyway.
bool compositor_label(gui::UiContext &ctx, const gui::Rect &bounds,
                      std::string_view text, gui::UiColor color, float size,
                      float zoom) {
  const std::optional<float> baked = ctx.text_size_at_most(size * zoom);
  if (!baked.has_value() || bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return false;
  }
  ctx.label(bounds, text, color, *baked);
  return true;
}

[[nodiscard]] CompositorNodeMetrics
compositor_node_metrics(const gui::UiContext &ctx) {
  const float title = compositor_text_height(ctx, compositor_title_text);
  const float detail = compositor_text_height(ctx, compositor_detail_text);
  const float pin = compositor_text_height(ctx, compositor_pin_text);
  return CompositorNodeMetrics{5.0F + title + detail + 3.0F,
                               std::max(22.0F, pin + 8.0F), 7.0F};
}

[[nodiscard]] float compositor_node_height(const CompositorNodeMetrics &metrics,
                                           compositor::NodeKind kind) noexcept {
  const std::size_t rows =
      std::max({compositor::input_ports(kind).size(),
                compositor::output_ports(kind).size(), std::size_t{1U}});
  return metrics.header_height +
         static_cast<float>(rows) * metrics.row_height + metrics.footer_height;
}

// Wide enough for the title, a Scene Layer's source name, and the longest
// input and output names side by side on one row. Other details carry live
// values and are left out on purpose: a node must not change width while
// its value is being scrubbed. Rounded up so a renamed source does not nudge
// the node by a pixel.
[[nodiscard]] float compositor_node_width(const gui::UiContext &ctx,
                                          compositor::NodeKind kind,
                                          std::string_view title,
                                          std::string_view source) {
  float widest_input{};
  for (const compositor::PortDescriptor &port : compositor::input_ports(kind)) {
    widest_input = std::max(
        widest_input, compositor_text_width(ctx, port.name, compositor_pin_text));
  }
  float widest_output{};
  for (const compositor::PortDescriptor &port :
       compositor::output_ports(kind)) {
    widest_output =
        std::max(widest_output,
                 compositor_text_width(ctx, port.name, compositor_pin_text));
  }
  const float pins =
      (widest_input > 0.0F ? compositor_pin_inset + widest_input
                           : compositor_node_padding) +
      compositor_pin_label_gap +
      (widest_output > 0.0F ? widest_output + compositor_pin_inset
                            : compositor_node_padding);
  const float heading =
      std::max(compositor_text_width(ctx, title, compositor_title_text),
               compositor_text_width(ctx, source, compositor_detail_text)) +
      compositor_node_padding * 2.0F;
  const float content = std::ceil(std::max(pins, heading) / 8.0F) * 8.0F;
  return std::clamp(content, compositor_node_min_width,
                    compositor_node_max_width);
}

[[nodiscard]] std::string compositor_node_detail(
    const compositor::GraphNode &node,
    std::span<const ElementInfo> elements) {
  std::array<char, 64U> text{};
  int length = 0;
  switch (node.kind) {
  case compositor::NodeKind::SceneLayer: {
    const auto *parameters =
        std::get_if<compositor::SceneLayerParameters>(&node.parameters);
    return compositor_scene_layer_detail(
        parameters != nullptr ? parameters->scene_node : doc::NodeId{},
        elements);
  }
  case compositor::NodeKind::Brightness: {
    const auto *parameters =
        std::get_if<compositor::BrightnessParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "AMOUNT %.2f",
        static_cast<double>(parameters != nullptr ? parameters->amount : 1.0F));
    break;
  }
  case compositor::NodeKind::Saturation: {
    const auto *parameters =
        std::get_if<compositor::SaturationParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "AMOUNT %.2f",
        static_cast<double>(parameters != nullptr ? parameters->amount : 1.0F));
    break;
  }
  case compositor::NodeKind::Levels: {
    const auto *parameters =
        std::get_if<compositor::LevelsParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "MASTER %.2f / %.2f / %.2f",
        static_cast<double>(
            parameters != nullptr ? parameters->master.input_black : 0.0F),
        static_cast<double>(parameters != nullptr ? parameters->master.gamma
                                                  : 1.0F),
        static_cast<double>(
            parameters != nullptr ? parameters->master.input_white : 1.0F));
    break;
  }
  case compositor::NodeKind::LiftGammaGain: {
    const auto *parameters =
        std::get_if<compositor::LiftGammaGainParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "L %.2f  G %.2f  GAIN %.2f",
        static_cast<double>(parameters != nullptr ? parameters->lift.x : 0.0F),
        static_cast<double>(parameters != nullptr ? parameters->gamma.x : 1.0F),
        static_cast<double>(parameters != nullptr ? parameters->gain.x : 1.0F));
    break;
  }
  case compositor::NodeKind::ContrastPivot: {
    const auto *parameters =
        std::get_if<compositor::ContrastPivotParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "CONTRAST %.2f  PIVOT %.2f",
        static_cast<double>(parameters != nullptr ? parameters->contrast
                                                  : 1.0F),
        static_cast<double>(parameters != nullptr ? parameters->pivot : 0.5F));
    break;
  }
  case compositor::NodeKind::ColorMatrix:
    return "3x3 RGB MATRIX + OFFSET";
  case compositor::NodeKind::WhiteBalance: {
    const auto *parameters =
        std::get_if<compositor::WhiteBalanceParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "%.0f K  TINT %.2f",
        static_cast<double>(parameters != nullptr
                                ? parameters->temperature_kelvin
                                : scene::d65_white_balance_temperature_kelvin),
        static_cast<double>(parameters != nullptr ? parameters->tint : 0.0F));
    break;
  }
  case compositor::NodeKind::Curves: {
    const auto *parameters =
        std::get_if<compositor::CurvesParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "MASTER %llu POINTS  RGB %u",
        static_cast<unsigned long long>(
            parameters != nullptr ? parameters->master.points.size() : 2U),
        static_cast<unsigned>(parameters != nullptr &&
                              (parameters->red.has_value() ||
                               parameters->green.has_value() ||
                               parameters->blue.has_value())));
    break;
  }
  case compositor::NodeKind::FilmGrain: {
    const auto *parameters =
        std::get_if<compositor::FilmGrainParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "AMOUNT %.2f  %s",
        static_cast<double>(parameters != nullptr ? parameters->amount : 0.0F),
        parameters != nullptr && parameters->mode == scene::FilmGrainMode::RGB
            ? "RGB"
            : "MONO");
    break;
  }
  case compositor::NodeKind::Blur: {
    const auto *parameters =
        std::get_if<compositor::BlurParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "RADIUS %.1f",
        static_cast<double>(parameters != nullptr ? parameters->radius : 0.0F));
    break;
  }
  case compositor::NodeKind::BoxBlur: {
    const auto *parameters =
        std::get_if<compositor::BoxBlurParameters>(&node.parameters);
    length = std::snprintf(text.data(), text.size(), "RADIUS %u  PASSES %u",
                           parameters != nullptr ? parameters->radius_px : 0U,
                           parameters != nullptr ? parameters->passes : 1U);
    break;
  }
  case compositor::NodeKind::DirectionalBlur: {
    const auto *parameters =
        std::get_if<compositor::DirectionalBlurParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "LENGTH %.1f  ANGLE %.3f",
        static_cast<double>(parameters != nullptr ? parameters->length_px
                                                  : 0.0F),
        static_cast<double>(parameters != nullptr ? parameters->angle_turns
                                                  : 0.0F));
    break;
  }
  case compositor::NodeKind::ChromaKey:
    return "COLOUR KEY + SPILL";
  case compositor::NodeKind::LumaKey:
    return "LUMINANCE KEY";
  case compositor::NodeKind::Matte: {
    const auto *parameters =
        std::get_if<compositor::MatteParameters>(&node.parameters);
    if (parameters == nullptr) {
      return "ALPHA MATTE";
    }
    return parameters->mode == compositor::MatteMode::Luma ? "LUMA MATTE"
                                                           : "ALPHA MATTE";
  }
  case compositor::NodeKind::RadialArray: {
    const auto *parameters =
        std::get_if<compositor::RadialArrayParameters>(&node.parameters);
    length = std::snprintf(text.data(), text.size(), "%u INSTANCES",
                           parameters != nullptr ? parameters->count : 0U);
    break;
  }
  case compositor::NodeKind::LedCarProcess: {
    const auto *parameters =
        std::get_if<compositor::LedCarProcessParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "8 WALL + CEILING  ARRAY %.2f",
        static_cast<double>(parameters != nullptr ? parameters->array_scale
                                                  : 4.0F));
    break;
  }
  case compositor::NodeKind::Composite:
    return "COMBINES IMAGE LAYERS";
  case compositor::NodeKind::Output:
    return "PROGRAM IMAGE";
  }
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] bool has_editable_parameters(compositor::NodeKind kind) noexcept {
  switch (kind) {
  case compositor::NodeKind::Brightness:
  case compositor::NodeKind::Saturation:
  case compositor::NodeKind::Levels:
  case compositor::NodeKind::LiftGammaGain:
  case compositor::NodeKind::ContrastPivot:
  case compositor::NodeKind::ColorMatrix:
  case compositor::NodeKind::WhiteBalance:
  case compositor::NodeKind::Curves:
  case compositor::NodeKind::FilmGrain:
  case compositor::NodeKind::Blur:
  case compositor::NodeKind::BoxBlur:
  case compositor::NodeKind::DirectionalBlur:
  case compositor::NodeKind::RadialArray:
  case compositor::NodeKind::LedCarProcess:
    return true;
  case compositor::NodeKind::ChromaKey:
  case compositor::NodeKind::LumaKey:
  case compositor::NodeKind::Matte:
  case compositor::NodeKind::SceneLayer:
  case compositor::NodeKind::Composite:
  case compositor::NodeKind::Output:
    return false;
  }
  return false;
}

// Bits of CompositorPanel's collapsed Parameters sections.
constexpr core::u32 led_car_position_section = 0U;
constexpr core::u32 led_car_rotation_section = 1U;
constexpr core::u32 led_car_plates_section = 2U;
constexpr core::u32 led_car_reflection_section = 3U;
constexpr core::u32 levels_master_section = 4U;
constexpr core::u32 levels_red_section = 5U;
constexpr core::u32 levels_green_section = 6U;
constexpr core::u32 levels_blue_section = 7U;

// Every field label the Parameters panel shows for a node kind, so the label
// column can be sized to the longest before any row is drawn. A label left
// out here still draws; it is shortened with an ellipsis if it does not fit.
[[nodiscard]] std::span<const std::string_view>
compositor_parameter_labels(compositor::NodeKind kind) noexcept {
  static constexpr std::array<std::string_view, 1U> amount{{"AMOUNT"}};
  static constexpr std::array<std::string_view, 5U> levels{
      {"IN BLACK", "IN WHITE", "GAMMA", "OUT BLACK", "OUT WHITE"}};
  static constexpr std::array<std::string_view, 9U> lift_gamma_gain{
      {"LIFT R", "LIFT G", "LIFT B", "GAMMA R", "GAMMA G", "GAMMA B", "GAIN R",
       "GAIN G", "GAIN B"}};
  static constexpr std::array<std::string_view, 2U> contrast_pivot{
      {"CONTRAST", "PIVOT"}};
  static constexpr std::array<std::string_view, 12U> color_matrix{
      {"RED FROM R", "RED FROM G", "RED FROM B", "GREEN FROM R", "GREEN FROM G",
       "GREEN FROM B", "BLUE FROM R", "BLUE FROM G", "BLUE FROM B",
       "RED OFFSET", "GREEN OFFSET", "BLUE OFFSET"}};
  static constexpr std::array<std::string_view, 2U> white_balance{
      {"TEMPERATURE K", "TINT"}};
  static constexpr std::array<std::string_view, 2U> curves{
      {"LAST POINT X", "LAST POINT Y"}};
  static constexpr std::array<std::string_view, 5U> film_grain{
      {"AMOUNT", "PIXEL SCALE", "CHROMA", "SEED", "MODE"}};
  static constexpr std::array<std::string_view, 1U> blur{{"RADIUS"}};
  static constexpr std::array<std::string_view, 2U> box_blur{
      {"RADIUS PX", "PASSES"}};
  static constexpr std::array<std::string_view, 2U> directional_blur{
      {"LENGTH PX", "ANGLE TURNS"}};
  static constexpr std::array<std::string_view, 4U> radial_array{
      {"COUNT", "RADIUS", "START", "CENTER"}};
  static constexpr std::array<std::string_view, 11U> led_car{
      {"X", "Y", "Z", "YAW", "PITCH", "ROLL", "PLATE SCALE", "ARRAY SCALE",
       "HORIZONTAL BLEND", "HEIGHT", "SCALE"}};
  switch (kind) {
  case compositor::NodeKind::Brightness:
  case compositor::NodeKind::Saturation:
    return amount;
  case compositor::NodeKind::Levels:
    return levels;
  case compositor::NodeKind::LiftGammaGain:
    return lift_gamma_gain;
  case compositor::NodeKind::ContrastPivot:
    return contrast_pivot;
  case compositor::NodeKind::ColorMatrix:
    return color_matrix;
  case compositor::NodeKind::WhiteBalance:
    return white_balance;
  case compositor::NodeKind::Curves:
    return curves;
  case compositor::NodeKind::FilmGrain:
    return film_grain;
  case compositor::NodeKind::Blur:
    return blur;
  case compositor::NodeKind::BoxBlur:
    return box_blur;
  case compositor::NodeKind::DirectionalBlur:
    return directional_blur;
  case compositor::NodeKind::RadialArray:
    return radial_array;
  case compositor::NodeKind::LedCarProcess:
    return led_car;
  case compositor::NodeKind::SceneLayer:
  case compositor::NodeKind::ChromaKey:
  case compositor::NodeKind::LumaKey:
  case compositor::NodeKind::Matte:
  case compositor::NodeKind::Composite:
  case compositor::NodeKind::Output:
    break;
  }
  return {};
}

[[nodiscard]] float
parameter_panel_height(const compositor::GraphNode &node) noexcept {
  constexpr float header = 32.0F;
  constexpr float footer = 8.0F;
  constexpr float row = 26.0F;
  switch (node.kind) {
  case compositor::NodeKind::RadialArray:
    return header + footer + 4.0F * row;
  case compositor::NodeKind::LedCarProcess:
    // Eleven fields under four section headings.
    return header + footer + 15.0F * row;
  case compositor::NodeKind::Levels:
    // Five fields for each of four channel sections.
    return header + footer + 24.0F * row;
  case compositor::NodeKind::LiftGammaGain:
    return header + footer + 9.0F * row;
  case compositor::NodeKind::ContrastPivot:
    return header + footer + 2.0F * row;
  case compositor::NodeKind::ColorMatrix:
    return header + footer + 12.0F * row;
  case compositor::NodeKind::WhiteBalance:
    return header + footer + 2.0F * row;
  case compositor::NodeKind::Curves: {
    const auto *parameters =
        std::get_if<compositor::CurvesParameters>(&node.parameters);
    const bool has_interior =
        parameters != nullptr && parameters->master.points.size() > 2U;
    return header + footer + (has_interior ? 4.0F : 2.0F) * row;
  }
  case compositor::NodeKind::FilmGrain:
    return header + footer + 5.0F * row;
  case compositor::NodeKind::BoxBlur:
  case compositor::NodeKind::DirectionalBlur:
    return header + footer + 2.0F * row;
  case compositor::NodeKind::Brightness:
  case compositor::NodeKind::Saturation:
  case compositor::NodeKind::Blur:
  case compositor::NodeKind::ChromaKey:
  case compositor::NodeKind::LumaKey:
  case compositor::NodeKind::Matte:
  case compositor::NodeKind::SceneLayer:
  case compositor::NodeKind::Composite:
  case compositor::NodeKind::Output:
    return header + footer + row;
  }
  return header + footer + row;
}

[[nodiscard]] bool node_has_diagnostic(const compositor::CompositorGraph &graph,
                                       doc::NodeId node) noexcept {
  return std::any_of(
      graph.diagnostics.begin(), graph.diagnostics.end(),
      [node](const compositor::GraphDiagnostic &diagnostic) noexcept {
        return diagnostic.subject == node || diagnostic.related == node;
      });
}

[[nodiscard]] const CompositorPortVisual *
find_port(std::span<const CompositorPortVisual> ports,
          compositor::EdgeEndpoint endpoint) noexcept {
  const auto found =
      std::find_if(ports.begin(), ports.end(),
                   [endpoint](const CompositorPortVisual &port) noexcept {
                     return port.endpoint == endpoint;
                   });
  return found == ports.end() ? nullptr : std::addressof(*found);
}

[[nodiscard]] bool can_connect(const compositor::CompositorGraph &graph,
                               const CompositorPortVisual &from,
                               const CompositorPortVisual &to) noexcept {
  if (from.descriptor == nullptr || to.descriptor == nullptr ||
      from.descriptor->direction != compositor::PortDirection::Output ||
      to.descriptor->direction != compositor::PortDirection::Input ||
      from.descriptor->type != to.descriptor->type) {
    return false;
  }
  const bool duplicate =
      std::any_of(graph.edges.begin(), graph.edges.end(),
                  [&from, &to](const compositor::GraphEdge &edge) noexcept {
                    return edge.from == from.endpoint && edge.to == to.endpoint;
                  });
  const bool output_occupied =
      !from.descriptor->multiple &&
      std::any_of(graph.edges.begin(), graph.edges.end(),
                  [&from](const compositor::GraphEdge &edge) noexcept {
                    return edge.from == from.endpoint;
                  });
  // An occupied single-input port is intentionally connectable: the emitted
  // intent names its current edge so the authoring boundary can replace it in
  // one transaction. This is the natural graph-editor rewire gesture.
  return !duplicate && !output_occupied;
}

// Retained for the read-only scene-element compatibility adapter below. The
// graph overload uses the typed node/port visuals above.
struct NodeBox final {
  gui::Rect bounds;
  std::string title;
  std::string detail;
  gui::UiColor color;
  bool highlighted{};
};

void draw_node(gui::UiContext &ctx, const NodeBox &node) {
  const gui::UiColor body = node.highlighted
                                ? scale(ctx.theme().selection, 1.22F)
                                : scale(ctx.theme().panel, 1.08F);
  ctx.panel(node.bounds, body);
  border(ctx, node.bounds,
         node.highlighted ? ctx.theme().accent : ctx.theme().widget_hover);
  ctx.panel(gui::Rect{node.bounds.x + 1.0F, node.bounds.y + 1.0F,
                      node.bounds.width - 2.0F, 4.0F},
            node.color);
  ctx.label(gui::Rect{node.bounds.x + 9.0F, node.bounds.y + 7.0F,
                      node.bounds.width - 16.0F, 17.0F},
            node.title, ctx.theme().text);
  ctx.label(gui::Rect{node.bounds.x + 9.0F, node.bounds.y + 23.0F,
                      node.bounds.width - 16.0F, 16.0F},
            node.detail, ctx.theme().text_muted);
  draw_circle(ctx, node.bounds.x, node.bounds.y + node.bounds.height * 0.5F,
              3.0F, ctx.theme().keyframe);
  draw_circle(ctx, node.bounds.x + node.bounds.width,
              node.bounds.y + node.bounds.height * 0.5F, 3.0F, node.color);
}

// A wire leaves its output and enters its input horizontally, as a cubic
// whose reach grows with the distance it spans. A wire running backwards -
// to a node left of its source - reaches further out so it loops around
// rather than folding back over the nodes it joins.
struct WirePath final {
  static constexpr std::size_t max_points = 49U;
  std::array<gui::UiPoint, max_points> points{};
  std::size_t count{};

  [[nodiscard]] std::span<const gui::UiPoint> view() const noexcept {
    return std::span<const gui::UiPoint>{points.data(), count};
  }
};

[[nodiscard]] WirePath wire_path(float from_x, float from_y, float to_x,
                                 float to_y, float zoom) noexcept {
  const float dx = to_x - from_x;
  const float dy = to_y - from_y;
  float reach = std::clamp(std::abs(dx) * 0.5F, 40.0F * zoom, 220.0F * zoom);
  if (dx < 0.0F) {
    reach += std::min(std::abs(dy), std::abs(dx)) * 0.25F + 30.0F * zoom;
  }
  const gui::UiPoint p0{from_x, from_y};
  const gui::UiPoint p1{from_x + reach, from_y};
  const gui::UiPoint p2{to_x - reach, to_y};
  const gui::UiPoint p3{to_x, to_y};
  // The control polygon bounds the curve's length; about one segment per
  // ten pixels keeps the stroke smooth without wasting vertices.
  const auto distance = [](gui::UiPoint a, gui::UiPoint b) noexcept {
    return std::hypot(b.x - a.x, b.y - a.y);
  };
  const float length = distance(p0, p1) + distance(p1, p2) + distance(p2, p3);
  const std::size_t segments = std::clamp<std::size_t>(
      static_cast<std::size_t>(std::ceil(length / 10.0F)), 12U,
      WirePath::max_points - 1U);
  WirePath path;
  path.count = segments + 1U;
  for (std::size_t index = 0U; index <= segments; ++index) {
    const float t = static_cast<float>(index) / static_cast<float>(segments);
    const float u = 1.0F - t;
    const float a = u * u * u;
    const float b = 3.0F * u * u * t;
    const float c = 3.0F * u * t * t;
    const float d = t * t * t;
    path.points[index] =
        gui::UiPoint{a * p0.x + b * p1.x + c * p2.x + d * p3.x,
                     a * p0.y + b * p1.y + c * p2.y + d * p3.y};
  }
  return path;
}

void wire(gui::UiContext &ctx, float from_x, float from_y, float to_x,
          float to_y, gui::UiColor color, float thickness = 2.0F,
          float zoom = 1.0F) {
  const WirePath path = wire_path(from_x, from_y, to_x, to_y, zoom);
  ctx.draw_list().add_polyline(path.view(), thickness, color);
}

// The pointer's distance from the drawn curve, for picking a wire.
[[nodiscard]] float wire_distance(const WirePath &path, float x,
                                  float y) noexcept {
  float nearest = std::numeric_limits<float>::infinity();
  for (std::size_t index = 0U; index + 1U < path.count; ++index) {
    const gui::UiPoint a = path.points[index];
    const gui::UiPoint b = path.points[index + 1U];
    const float sx = b.x - a.x;
    const float sy = b.y - a.y;
    const float length_squared = sx * sx + sy * sy;
    const float t =
        length_squared > 0.0F
            ? std::clamp(((x - a.x) * sx + (y - a.y) * sy) / length_squared,
                         0.0F, 1.0F)
            : 0.0F;
    nearest = std::min(nearest, std::hypot(x - (a.x + sx * t),
                                           y - (a.y + sy * t)));
  }
  return nearest;
}

[[nodiscard]] std::string resolution_text(core::u32 width, core::u32 height) {
  std::array<char, 32U> output{};
  const int length =
      std::snprintf(output.data(), output.size(), "%u x %u", width, height);
  return std::string{output.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] std::string fps_text(float fps) {
  std::array<char, 16U> output{};
  const int length = std::snprintf(output.data(), output.size(), "%.0f",
                                   static_cast<double>(fps));
  return std::string{output.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] std::string frame_text(core::u64 frame) {
  std::array<char, 32U> output{};
  const int length = std::snprintf(output.data(), output.size(), "FRAME %08llu",
                                   static_cast<unsigned long long>(frame));
  return std::string{output.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

} // namespace

std::string_view compositor_diagnostic_message(
    const compositor::GraphDiagnostic &diagnostic) noexcept {
  switch (diagnostic.code) {
  case compositor::DiagnosticCode::NodesOverCapacity:
    return "Too many nodes; remove a node";
  case compositor::DiagnosticCode::EdgesOverCapacity:
    return "Too many connections; disconnect a wire";
  case compositor::DiagnosticCode::DanglingNode:
    return "A wire references a missing node";
  case compositor::DiagnosticCode::DanglingSceneReference:
    return "A Layer references a missing scene object";
  case compositor::DiagnosticCode::UnknownPort:
    return "A wire references a missing port";
  case compositor::DiagnosticCode::IncompatiblePorts:
    return "Port types do not match";
  case compositor::DiagnosticCode::DuplicateInput:
    return "An input already has a connection";
  case compositor::DiagnosticCode::MissingRequiredInput:
    return "A required input is not connected";
  case compositor::DiagnosticCode::MissingOutput:
    return "Add and connect one Output node";
  case compositor::DiagnosticCode::DuplicateOutput:
    return "Keep only one Output node";
  case compositor::DiagnosticCode::DuplicateSceneLayerReference:
    return "One active Scene Layer per source is supported";
  case compositor::DiagnosticCode::Cycle:
    return "Cycle detected; wires must flow toward Output";
  case compositor::DiagnosticCode::UnsupportedEffectOrder:
    return "Inline effects must run before spatial filters";
  case compositor::DiagnosticCode::DeadBranch:
    return "This branch does not reach Output";
  case compositor::DiagnosticCode::PassBudgetExceeded:
    return "Graph exceeds the authored pass budget";
  case compositor::DiagnosticCode::TextureBudgetExceeded:
    return "Graph exceeds the offscreen texture budget";
  case compositor::DiagnosticCode::TapBudgetExceeded:
    return "Graph exceeds the blur tap budget";
  }
  return "Graph is not ready";
}

std::string_view compositor_diagnostic_severity_label(
    const compositor::GraphDiagnostic &diagnostic) noexcept {
  return compositor::diagnostic_severity_name(diagnostic.severity);
}

void draw_compositor_drop_target(gui::UiContext &ctx, const gui::Rect &bounds,
                                 std::string_view item_name) {
  if (bounds.width <= 0.0F || bounds.height <= compositor_toolbar_height) {
    return;
  }
  const gui::Rect canvas{bounds.x, bounds.y + compositor_toolbar_height,
                         bounds.width,
                         bounds.height - compositor_toolbar_height};
  const bool hovered = canvas.contains(ctx.pointer_x(), ctx.pointer_y());
  const gui::UiColor color =
      with_alpha(ctx.theme().accent, hovered ? 1.0F : 0.36F);
  ctx.panel(canvas, with_alpha(ctx.theme().accent, hovered ? 0.10F : 0.025F));
  border(ctx, canvas, color, hovered ? 2.0F : 1.0F);

  const gui::Rect callout{
      canvas.x + 9.0F, canvas.y + 9.0F,
      std::min(std::max(canvas.width - 18.0F, 0.0F), 360.0F), 30.0F};
  if (callout.width <= 0.0F) {
    return;
  }
  ctx.panel(callout, hovered ? ctx.theme().widget_active : ctx.theme().header);
  border(ctx, callout, color, hovered ? 2.0F : 1.0F);
  ctx.panel(gui::Rect{callout.x, callout.y, 3.0F, callout.height}, color);
  std::string label{hovered ? "RELEASE TO ADD NODE  ·  "
                            : "DROP IN NODE GRAPH  ·  "};
  label += item_name.empty() ? "ITEM" : item_name;
  ctx.label(gui::Rect{callout.x + 10.0F, callout.y,
                      std::max(callout.width - 14.0F, 0.0F), callout.height},
            label, hovered ? ctx.theme().text : ctx.theme().text_muted);
}

std::string_view output_node_connection_label(
    const OutputMonitorState::Node &node) noexcept {
  using Connection = OutputMonitorState::Node::Connection;
  switch (node.connection) {
  case Connection::Live: return "LIVE";
  case Connection::CatchingUp: return "CATCHING UP";
  case Connection::Disconnected: return "CONNECTING";
  }
  return "CONNECTING";
}

std::string_view output_node_deployment_label(
    const OutputMonitorState::Node &node) noexcept {
  using OutputStatus = OutputMonitorState::Node::OutputStatus;
  switch (node.output) {
  case OutputStatus::Unknown:
  case OutputStatus::Deployed:
    return {};
  case OutputStatus::InventoryUnavailable:
    return "NO DISPLAY INVENTORY ON THAT MACHINE";
  case OutputStatus::InvalidMode:
    return "SAVED OUTPUT MODE IS OUT OF RANGE";
  case OutputStatus::DisplayMissing:
    return "BOUND DISPLAY IS NOT CONNECTED";
  case OutputStatus::DisplayInactive:
    return "BOUND DISPLAY IS CONNECTED BUT SWITCHED OFF";
  case OutputStatus::DisplayDuplicated:
    return "TWO DISPLAYS CLAIM THE SAME BINDING";
  case OutputStatus::DisplayMirrored:
    return "BOUND DISPLAY MIRRORS ANOTHER - EXTEND THE DESKTOP";
  case OutputStatus::DuplicateRenderServer:
    return "TWO RENDER SERVERS CLAIM THIS NODE INDEX";
  case OutputStatus::PresenterRefused:
    return "DISPLAY DOES NOT OFFER THE SAVED MODE";
  }
  return "OUTPUT NOT DEPLOYED";
}

std::string output_node_content_label(const OutputMonitorState::Node &node) {
  using ContentStatus = OutputMonitorState::Node::ContentStatus;
  switch (node.content) {
  case ContentStatus::NotRequired:
    return {};
  case ContentStatus::Checking:
    return "ASSETS CHECKING";
  case ContentStatus::Ready:
    return std::string{"ASSETS "} + std::to_string(node.assets_ready) + '/' +
           std::to_string(node.assets_required) + " READY";
  case ContentStatus::Missing:
    return std::string{"ASSETS MISSING "} +
           std::to_string(node.assets_missing) + " / STALE " +
           std::to_string(node.assets_mismatched);
  case ContentStatus::Error:
    return node.assets_verification_failed == 0U
               ? std::string{"ASSET CHECK ERROR"}
               : std::string{"ASSET VERIFY FAILED "} +
                     std::to_string(node.assets_verification_failed);
  }
  return "ASSET CHECK ERROR";
}

namespace {

// The Scene outliner is the document's hierarchy, not a sort by kind: groups
// hold their children, Blender collections hold theirs, and siblings stand in
// the same front-to-back order the Timeline's layer list shows them in.
[[nodiscard]] doc::NodeId outliner_parent(const ElementInfo &element) noexcept {
  return element.linked ? element.linked_parent
                        : element.parent_group.value_or(doc::NodeId{});
}

[[nodiscard]] bool outliner_lists(const ElementInfo &element) noexcept {
  // A track is a timeline header, and the environment is the Environment
  // settings row above the tree; listing either again would put one thing in
  // two places with two ways to select it.
  return element.kind != ElementKind::Track &&
         element.kind != ElementKind::Environment;
}

[[nodiscard]] bool outliner_orderable(const ElementInfo &element) noexcept {
  return element.kind == ElementKind::Object ||
         element.kind == ElementKind::Media ||
         element.kind == ElementKind::Model ||
         element.kind == ElementKind::Group || is_light(element.kind);
}

[[nodiscard]] bool outliner_groupable(const ElementInfo &element) noexcept {
  return !element.linked &&
         (element.kind == ElementKind::Object ||
          element.kind == ElementKind::Media ||
          element.kind == ElementKind::Model ||
          element.kind == ElementKind::Group);
}

struct OutlinerRow final {
  const ElementInfo *element{};
  std::size_t depth{};
  bool has_children{};
};

enum class SceneMenuCommand : core::u8 {
  Rename,
  ToggleVisible,
  ToggleLocked,
  Group,
  Ungroup,
  MoveOut,
  Delete,
};

constexpr float scene_toolbar_height = 32.0F;
constexpr float scene_footer_height = 30.0F;
constexpr float scene_drag_threshold = 5.0F;

} // namespace

void ScenePanel::begin_rename(gui::UiContext &ctx, const ElementInfo &element) {
  if (element.locked || element.linked) {
    return;
  }
  rename_.begin(ctx, element.node.value(), element_display_name(element));
}

std::optional<SceneSelectionRequest>
ScenePanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                 std::span<const ElementInfo> elements,
                 std::span<const doc::NodeId> selection, doc::NodeId primary,
                 bool show_settings, SceneSettingsTarget settings_selection,
                 std::span<const output::ProjectionSurface> surfaces,
                 doc::NodeId selected_surface) {
  std::optional<SceneSelectionRequest> request;
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    rename_.end_frame(ctx);
    return request;
  }
  const gui::Theme &theme = ctx.theme();
  const float pointer_x = ctx.pointer_x();
  const float pointer_y = ctx.pointer_y();
  if (ctx.pointer_pressed() || ctx.secondary_pointer_pressed()) {
    focused_ = bounds.contains(pointer_x, pointer_y);
  }

  ctx.push_clip(bounds);
  ctx.panel(bounds);

  const auto find = [&](doc::NodeId node) -> const ElementInfo * {
    for (const ElementInfo &element : elements) {
      if (element.node == node) {
        return &element;
      }
    }
    return nullptr;
  };

  // ---------------------------------------------------------------- tree
  std::map<doc::NodeId, std::vector<const ElementInfo *>> children;
  std::set<doc::NodeId> listed_nodes;
  std::size_t listed_count = 0U;
  for (const ElementInfo &element : elements) {
    if (outliner_lists(element)) {
      listed_nodes.insert(element.node);
      ++listed_count;
    }
  }
  for (const ElementInfo &element : elements) {
    if (!outliner_lists(element)) {
      continue;
    }
    const doc::NodeId parent = outliner_parent(element);
    children[parent != element.node && listed_nodes.contains(parent)
                 ? parent
                 : doc::NodeId{}]
        .push_back(&element);
  }
  for (auto &[parent, siblings] : children) {
    // Front first, as in the layer stack: a higher group_order is nearer the
    // camera and sits higher in the list. Unordered kinds (cameras) follow.
    std::stable_sort(siblings.begin(), siblings.end(),
                     [](const ElementInfo *left, const ElementInfo *right) {
                       const bool left_ordered = outliner_orderable(*left);
                       const bool right_ordered = outliner_orderable(*right);
                       if (left_ordered != right_ordered) {
                         return left_ordered;
                       }
                       return left_ordered &&
                              left->group_order > right->group_order;
                     });
  }

  // Reveal a newly selected element wherever it sits, the way the Timeline
  // opens to it.
  if (primary != revealed_) {
    revealed_ = primary;
    const ElementInfo *walk = find(primary);
    std::size_t guard = 0U;
    while (walk != nullptr && guard++ < elements.size()) {
      const doc::NodeId parent = outliner_parent(*walk);
      if (parent == doc::NodeId{} || parent == walk->node) {
        break;
      }
      collapsed_.erase(parent);
      walk = find(parent);
    }
  }

  // Search keeps a match's ancestors so its place in the hierarchy is never
  // lost, and opens every branch while it applies.
  std::set<doc::NodeId> matching;
  if (!search_.empty()) {
    for (const ElementInfo &element : elements) {
      if (!outliner_lists(element) ||
          (!matches(element_display_name(element), search_) &&
           !matches(kind_name(element), search_))) {
        continue;
      }
      const ElementInfo *walk = &element;
      std::size_t guard = 0U;
      while (walk != nullptr && guard++ <= elements.size() &&
             matching.insert(walk->node).second) {
        const doc::NodeId parent = outliner_parent(*walk);
        walk = parent == walk->node ? nullptr : find(parent);
      }
    }
  }

  std::vector<OutlinerRow> rows;
  rows.reserve(listed_count);
  {
    std::set<doc::NodeId> visited;
    const auto append = [&](const auto &self, const ElementInfo *element,
                            std::size_t depth) -> void {
      if (!visited.insert(element->node).second) {
        return;
      }
      if (!search_.empty() && !matching.contains(element->node)) {
        return;
      }
      const auto found = children.find(element->node);
      const bool has_children =
          found != children.end() && !found->second.empty();
      rows.push_back(OutlinerRow{element, depth, has_children});
      if (has_children &&
          (!search_.empty() || !collapsed_.contains(element->node))) {
        for (const ElementInfo *child : found->second) {
          self(self, child, depth + 1U);
        }
      }
    };
    if (const auto roots = children.find(doc::NodeId{});
        roots != children.end()) {
      for (const ElementInfo *root : roots->second) {
        append(append, root, 0U);
      }
    }
  }
  std::vector<doc::NodeId> visible_order;
  visible_order.reserve(rows.size());
  for (const OutlinerRow &row : rows) {
    if (!row.element->locked) {
      visible_order.push_back(row.element->node);
    }
  }
  std::vector<const output::ProjectionSurface *> surface_rows;
  surface_rows.reserve(surfaces.size());
  for (const output::ProjectionSurface &surface : surfaces) {
    if (search_.empty() || matches(surface.name, search_) ||
        matches("LED output physical surface", search_)) {
      surface_rows.push_back(&surface);
    }
  }

  // ------------------------------------------------------------- toolbar
  const gui::Rect toolbar{bounds.x, bounds.y, bounds.width,
                          scene_toolbar_height};
  ctx.panel(toolbar, theme.header);
  ctx.panel(gui::Rect{toolbar.x, toolbar.y + toolbar.height - 1.0F,
                      toolbar.width, 1.0F},
            theme.divider);
  std::array<char, 32U> count_text{};
  const int count_length = std::snprintf(
      count_text.data(), count_text.size(), "%llu",
      static_cast<unsigned long long>(listed_count));
  const std::string_view count{
      count_text.data(), static_cast<std::size_t>(std::max(count_length, 0))};
  const float count_width = ctx.font_width(count) + 12.0F;
  search_field(ctx, mix(id_, "scene.search"),
               gui::Rect{toolbar.x, toolbar.y + 2.0F,
                         std::max(toolbar.width - count_width, 0.0F),
                         toolbar.height - 4.0F},
               search_, "Search scene");
  ctx.label(gui::Rect{toolbar.x + toolbar.width - count_width + 2.0F,
                      toolbar.y, count_width, toolbar.height},
            count, theme.text_muted);
  ctx.tooltip(gui::Rect{toolbar.x + toolbar.width - count_width, toolbar.y,
                        count_width, toolbar.height},
              "Elements in this scene");

  // ---------------------------------------------------------------- list
  const gui::Rect list_bounds{
      bounds.x, toolbar.y + toolbar.height, bounds.width,
      std::max(bounds.height - toolbar.height - scene_footer_height, 0.0F)};
  constexpr float row_height = list_metrics::row_height;
  const std::size_t settings_rows = show_settings ? 2U : 0U;
  const float content_height =
      static_cast<float>(settings_rows + rows.size() + surface_rows.size()) *
          row_height +
      (show_settings ? 6.0F : 0.0F);
  const float maximum_scroll =
      std::max(content_height - list_bounds.height, 0.0F);
  if (list_bounds.contains(pointer_x, pointer_y)) {
    scroll_ -= ctx.wheel() * row_height * 3.0F;
  }
  scroll_ = std::clamp(scroll_, 0.0F, maximum_scroll);

  // Drag state from earlier frames: whether a press has travelled far enough
  // to be a move rather than a click.
  if (drag_node_ != doc::NodeId{}) {
    const ElementInfo *const dragged = find(drag_node_);
    if (dragged == nullptr || dragged->locked) {
      drag_node_ = doc::NodeId{};
      dragging_ = false;
    } else if (!dragging_ && ctx.pointer_down() &&
               std::fabs(pointer_x - drag_x_) + std::fabs(pointer_y - drag_y_) >
                   scene_drag_threshold) {
      dragging_ = true;
    }
  }
  const auto descends_from = [&](doc::NodeId node, doc::NodeId ancestor) {
    const ElementInfo *walk = find(node);
    std::size_t guard = 0U;
    while (walk != nullptr && guard++ <= elements.size()) {
      if (walk->node == ancestor) {
        return true;
      }
      const doc::NodeId parent = outliner_parent(*walk);
      if (parent == walk->node) {
        break;
      }
      walk = find(parent);
    }
    return false;
  };
  // What the moving rows are: the whole selection when the dragged row is
  // part of it, otherwise just that row.
  std::vector<doc::NodeId> moving;
  if (dragging_) {
    if (std::find(selection.begin(), selection.end(), drag_node_) !=
        selection.end()) {
      for (const doc::NodeId node : selection) {
        const ElementInfo *const element = find(node);
        if (element != nullptr && outliner_groupable(*element) &&
            !element->locked) {
          moving.push_back(node);
        }
      }
    }
    if (moving.empty()) {
      moving.push_back(drag_node_);
    }
  }
  const auto movable_into = [&](const ElementInfo &target) {
    if (target.kind != ElementKind::Group || target.linked || target.locked) {
      return false;
    }
    return std::none_of(moving.begin(), moving.end(), [&](doc::NodeId node) {
      const ElementInfo *const element = find(node);
      return element == nullptr || !outliner_groupable(*element) ||
             descends_from(target.node, node);
    });
  };
  enum class DropKind : core::u8 { None, Before, After, Into };
  DropKind drop{DropKind::None};
  const ElementInfo *drop_target{};
  float drop_line_y{};
  float drop_line_x{};

  ctx.push_clip(list_bounds);
  float y = list_bounds.y - scroll_;
  bool pointer_over_content = false;

  if (show_settings) {
    const std::array<std::pair<SceneSettingsTarget, std::string_view>, 2U>
        settings{{{SceneSettingsTarget::Renderer, "Renderer"},
                  {SceneSettingsTarget::Environment, "Environment"}}};
    for (const auto &[target, label] : settings) {
      const gui::Rect row{list_bounds.x, y, list_bounds.width, row_height};
      const gui::Interaction interaction =
          clipped_grab(ctx, mix(id_, label), row, list_bounds);
      pointer_over_content =
          pointer_over_content || row.contains(pointer_x, pointer_y);
      const bool chosen = settings_selection == target;
      if (chosen) {
        ctx.panel(row, theme.selection);
        ctx.panel(gui::Rect{row.x, row.y, 2.0F, row.height}, theme.accent);
      } else if (interaction.hovered) {
        ctx.panel(row, theme.widget_hover);
      }
      const float icon_x = row.x + list_metrics::toggle + 4.0F +
                           list_metrics::twirl_width + 8.0F;
      if (target == SceneSettingsTarget::Environment) {
        draw_kind_icon(ctx, ElementKind::Environment,
                       scene::LocalLightKind::Point, icon_x,
                       row.y + row.height * 0.5F,
                       element_kind_color(ElementKind::Environment));
      } else {
        // The renderer: a gear-like cog, drawn as a ring and teeth.
        const float cx = icon_x;
        const float cy = row.y + row.height * 0.5F;
        draw_filled_circle(ctx, cx, cy, 5.0F, theme.text_muted);
        draw_filled_circle(ctx, cx, cy, 2.2F,
                           chosen ? theme.selection : theme.panel);
        for (std::size_t tooth = 0U; tooth < 6U; ++tooth) {
          const float angle = 6.28318530718F * static_cast<float>(tooth) / 6.0F;
          ctx.draw_list().add_line(cx + std::cos(angle) * 4.0F,
                                   cy + std::sin(angle) * 4.0F,
                                   cx + std::cos(angle) * 7.0F,
                                   cy + std::sin(angle) * 7.0F, 2.0F,
                                   theme.text_muted);
        }
      }
      ctx.label(gui::Rect{icon_x + 14.0F, row.y,
                          std::max(row.width - icon_x - 14.0F + row.x, 0.0F),
                          row.height},
                label, chosen ? theme.text : scale(theme.text, 0.9F));
      if (interaction.clicked) {
        SceneSelectionRequest next{};
        next.settings = target;
        request = next;
      }
      y += row_height;
    }
    // A rule between the scene's settings and the things in it.
    ctx.panel(gui::Rect{list_bounds.x + 8.0F, y + 2.0F,
                        std::max(list_bounds.width - 16.0F, 0.0F), 1.0F},
              theme.divider);
    y += 6.0F;
  }

  const float tree_x = list_bounds.x + list_metrics::toggle + 4.0F;
  const gui::UiColor guide_color = with_alpha(theme.border, 0.9F);
  std::optional<std::size_t> menu_row;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const OutlinerRow &entry = rows[index];
    const ElementInfo &element = *entry.element;
    const gui::Rect row{list_bounds.x, y, list_bounds.width, row_height};
    y += row_height;
    if (row.y + row.height <= list_bounds.y ||
        row.y >= list_bounds.y + list_bounds.height) {
      continue;
    }
    const bool row_hovered = !ctx.pointer_blocked() &&
                             row.contains(pointer_x, pointer_y) &&
                             list_bounds.contains(pointer_x, pointer_y);
    pointer_over_content = pointer_over_content || row_hovered;
    const float centre_y = row.y + row.height * 0.5F;
    const float depth_x =
        tree_x + static_cast<float>(entry.depth) * list_metrics::indent;
    const gui::Rect eye_bounds{row.x + 2.0F, row.y, list_metrics::toggle,
                               row.height};
    const gui::Rect lock_bounds{row.x + row.width - list_metrics::toggle - 2.0F,
                                row.y, list_metrics::toggle, row.height};
    const gui::Rect twirl_bounds{depth_x, row.y, list_metrics::twirl_width,
                                 row.height};
    const float icon_x = depth_x + list_metrics::twirl_width + 8.0F;
    const float name_x = icon_x + 13.0F;
    std::string tag;
    if (element.linked_collection) {
      tag = element.linked_paused     ? "paused"
            : element.linked_pending  ? "updating"
            : element.linked_failed   ? "refused"
            : !element.linked_connected ? "offline"
                                        : "";
    } else if (element.linked) {
      tag = "Blender";
    }
    const float tag_width = tag.empty() ? 0.0F : ctx.font_width(tag) + 10.0F;
    const gui::Rect name_bounds{
        name_x, row.y,
        std::max(lock_bounds.x - name_x - tag_width - 4.0F, 0.0F), row.height};

    const bool is_selected =
        std::find(selection.begin(), selection.end(), element.node) !=
        selection.end();
    const bool is_drop_target_row =
        dragging_ && row_hovered && element.node != drag_node_;

    // Background: selection, hover, nothing - no zebra; the hierarchy is the
    // structure the eye follows, and alternating fills fight the guides.
    if (is_selected) {
      ctx.panel(row, element.node == primary ? scale(theme.selection, 1.2F)
                                              : theme.selection);
      if (element.node == primary) {
        ctx.panel(gui::Rect{row.x, row.y, 2.0F, row.height}, theme.accent);
      }
    } else if (row_hovered && !dragging_) {
      ctx.panel(row, with_alpha(theme.widget_hover, 0.7F));
    }
    draw_indent_guides(ctx, tree_x, row.y, row.height, entry.depth,
                       guide_color);

    // Visibility, far left like every compositor's eye column.
    if (row_flag_toggle(ctx, mix(id_, "scene.element.visible",
                                 element.node.value()),
                        eye_bounds, RowFlag::Visible, element.visible,
                        row_hovered)) {
      SceneSelectionRequest next{};
      next.node = element.node;
      next.visibility_changed = true;
      next.visible = !element.visible;
      request = std::move(next);
    }
    if (entry.has_children) {
      const gui::Interaction twirl = clipped_grab(
          ctx, mix(id_, "scene.twirl", element.node.value()), twirl_bounds,
          list_bounds);
      const bool open = !search_.empty() || !collapsed_.contains(element.node);
      draw_twirl(ctx, twirl_bounds.x + twirl_bounds.width * 0.5F, centre_y,
                 open, twirl.hovered ? theme.text : theme.text_muted);
      if (twirl.clicked && search_.empty()) {
        if (open) {
          collapsed_.insert(element.node);
        } else {
          collapsed_.erase(element.node);
        }
      }
    }
    const gui::UiColor kind_tint = element_kind_color(element);
    draw_element_icon(ctx, element, icon_x, centre_y,
                      element.effective_visible ? kind_tint
                                                : with_alpha(kind_tint, 0.4F));

    // The row's body: everything between the twirl and the lock.
    const gui::Rect body{icon_x - 8.0F, row.y,
                         std::max(lock_bounds.x - icon_x + 8.0F, 0.0F),
                         row.height};
    const bool renaming = rename_.editing(element.node.value());
    gui::Interaction interaction{};
    if (renaming) {
      if (auto renamed = rename_.draw(
              ctx, gui::Rect{name_bounds.x - 4.0F, row.y + 2.0F,
                             std::max(lock_bounds.x - name_bounds.x, 24.0F),
                             row.height - 4.0F})) {
        SceneSelectionRequest next{};
        next.node = element.node;
        next.rename = std::move(*renamed);
        request = std::move(next);
      }
    } else {
      if (!element.locked) {
        interaction = clipped_grab(
            ctx, mix(id_, "scene.element", element.node.value()), body,
            list_bounds);
      }
      const gui::UiColor name_color =
          element.locked ? theme.text_muted
                         : (!element.effective_visible
                                ? with_alpha(theme.text, 0.45F)
                                : (is_selected ? theme.text
                                               : scale(theme.text, 0.92F)));
      ctx.label(name_bounds, element_display_name(element), name_color);
      if (!tag.empty()) {
        ctx.label(gui::Rect{name_bounds.x + name_bounds.width + 4.0F, row.y,
                            tag_width, row.height},
                  tag,
                  element.linked_failed ? theme.warning
                                        : with_alpha(theme.text_muted, 0.85F));
      }
    }

    if (row_flag_toggle(
            ctx, mix(id_, "scene.element.lock", element.node.value()),
            lock_bounds, RowFlag::Locked, element.locked, row_hovered)) {
      SceneSelectionRequest next{};
      next.node = element.node;
      next.lock_changed = true;
      next.locked = !element.locked;
      request = std::move(next);
    }

    if (interaction.pressed && !element.linked && outliner_orderable(element)) {
      drag_node_ = element.node;
      drag_x_ = pointer_x;
      drag_y_ = pointer_y;
      dragging_ = false;
    }
    if (interaction.clicked && !dragging_) {
      SceneSelectionRequest next{};
      next.node = element.node;
      next.action = selection_action(ctx.ctrl_held(), ctx.shift_held(), true);
      if (next.action == SelectionAction::Range) {
        next.visible_order = visible_order;
      }
      const bool plain_click = next.action == SelectionAction::Replace;
      request = std::move(next);
      // A second click on the same name opens it for renaming.
      if (plain_click && name_bounds.contains(pointer_x, pointer_y) &&
          clicks_.double_click(element.node.value(), ctx.time_seconds()) &&
          !element.linked) {
        rename_.begin(ctx, element.node.value(),
                      element_display_name(element));
      }
    }
    if (row_hovered && ctx.secondary_pointer_pressed() && !element.locked) {
      menu_row = index;
    }

    // Where a drag would land if let go here.
    if (is_drop_target_row) {
      const float local = (pointer_y - row.y) / row.height;
      const bool into_band = local > 0.25F && local < 0.75F;
      if (into_band && movable_into(element)) {
        drop = DropKind::Into;
        drop_target = &element;
      } else if (outliner_orderable(element) && !element.linked &&
                 std::none_of(moving.begin(), moving.end(),
                              [&](doc::NodeId node) {
                                return descends_from(element.node, node);
                              })) {
        drop = local < 0.5F ? DropKind::Before : DropKind::After;
        drop_target = &element;
        drop_line_y = local < 0.5F ? row.y : row.y + row.height;
        drop_line_x = depth_x;
      }
      if (drop == DropKind::Into) {
        const gui::Rect target_row = row.inflated(-1.0F, -1.0F);
        ctx.panel(target_row, with_alpha(theme.accent, 0.18F));
        auto &draw = ctx.draw_list();
        draw.add_rect(target_row.x, target_row.y, target_row.width, 1.0F,
                      theme.accent);
        draw.add_rect(target_row.x, target_row.y + target_row.height - 1.0F,
                      target_row.width, 1.0F, theme.accent);
      }
    }
  }
  // The LED surfaces, after everything the scene holds. A click selects one
  // on Stage, as picking it there does; the padlock is its Feed Mapping lock.
  for (const output::ProjectionSurface *const surface : surface_rows) {
    const gui::Rect row{list_bounds.x, y, list_bounds.width, row_height};
    y += row_height;
    if (row.y + row.height <= list_bounds.y ||
        row.y >= list_bounds.y + list_bounds.height) {
      continue;
    }
    const bool row_hovered = !ctx.pointer_blocked() && !dragging_ &&
                             row.contains(pointer_x, pointer_y) &&
                             list_bounds.contains(pointer_x, pointer_y);
    pointer_over_content = pointer_over_content || row_hovered;
    const bool is_selected = selected_surface == surface->node;
    if (is_selected) {
      ctx.panel(row, scale(theme.selection, 1.2F));
      ctx.panel(gui::Rect{row.x, row.y, 2.0F, row.height}, theme.accent);
    } else if (row_hovered) {
      ctx.panel(row, with_alpha(theme.widget_hover, 0.7F));
    }
    const gui::Rect lock_bounds{row.x + row.width - list_metrics::toggle - 2.0F,
                                row.y, list_metrics::toggle, row.height};
    const float icon_x = tree_x + list_metrics::twirl_width + 8.0F;
    const float name_x = icon_x + 13.0F;
    draw_kind_icon(ctx, ElementKind::Track, scene::LocalLightKind::Point,
                   icon_x, row.y + row.height * 0.5F, theme.accent);
    const std::string_view tag{"LED"};
    const float tag_width = ctx.font_width(tag) + 10.0F;
    const gui::Rect name_bounds{
        name_x, row.y,
        std::max(lock_bounds.x - name_x - tag_width - 4.0F, 0.0F), row.height};
    ctx.label(name_bounds,
              surface->name.empty() ? std::string_view{"Surface"}
                                    : std::string_view{surface->name},
              surface->locked ? theme.text_muted
                              : (is_selected ? theme.text
                                             : scale(theme.text, 0.92F)));
    ctx.label(gui::Rect{name_bounds.x + name_bounds.width + 4.0F, row.y,
                        tag_width, row.height},
              tag, with_alpha(theme.text_muted, 0.85F));
    if (!surface->locked) {
      const gui::Interaction interaction = clipped_grab(
          ctx, mix(id_, "scene.surface", surface->node.value()),
          gui::Rect{icon_x - 8.0F, row.y,
                    std::max(lock_bounds.x - icon_x + 8.0F, 0.0F), row.height},
          list_bounds);
      if (interaction.clicked) {
        SceneSelectionRequest next{};
        next.node = surface->node;
        next.surface = true;
        request = std::move(next);
      }
    }
    if (row_flag_toggle(
            ctx, mix(id_, "scene.surface.lock", surface->node.value()),
            lock_bounds, RowFlag::Locked, surface->locked, row_hovered)) {
      SceneSelectionRequest next{};
      next.node = surface->node;
      next.surface = true;
      next.lock_changed = true;
      next.locked = !surface->locked;
      request = std::move(next);
    }
  }
  if (drop == DropKind::Before || drop == DropKind::After) {
    ctx.panel(gui::Rect{drop_line_x, drop_line_y - 1.0F,
                        std::max(list_bounds.x + list_bounds.width -
                                     drop_line_x - 4.0F,
                                 0.0F),
                        2.0F},
              theme.accent);
    draw_filled_circle(ctx, drop_line_x, drop_line_y, 3.0F, theme.accent);
  }
  if (!request.has_value() && ctx.pointer_pressed() &&
      list_bounds.contains(pointer_x, pointer_y) && !pointer_over_content) {
    SceneSelectionRequest clear{};
    clear.action = SelectionAction::Clear;
    request = std::move(clear);
  }
  if (rows.empty() && surface_rows.empty()) {
    ctx.label(gui::Rect{list_bounds.x + 12.0F, y + 4.0F,
                        std::max(list_bounds.width - 24.0F, 0.0F), row_height},
              search_.empty() ? "Nothing in this scene yet - add from Library"
                              : "No matches",
              theme.text_muted);
  }
  ctx.pop_clip();

  // The drop, on release: into a group, or beside a sibling - which is a
  // reorder when the two already share a parent, and a move otherwise.
  if (dragging_ && !ctx.pointer_down()) {
    if (drop_target != nullptr && drop != DropKind::None) {
      SceneSelectionRequest next{};
      if (drop == DropKind::Into) {
        next.structure = SceneSelectionRequest::StructureAction::MoveInto;
        next.destination = drop_target->node;
        next.moved = moving;
      } else {
        const ElementInfo *const dragged = find(drag_node_);
        const bool siblings =
            dragged != nullptr && moving.size() == 1U &&
            outliner_parent(*dragged) == outliner_parent(*drop_target);
        if (siblings) {
          next.node = drag_node_;
          next.reorder_destination = drop_target->node;
          next.reorder_before = drop == DropKind::Before;
        } else {
          next.structure = SceneSelectionRequest::StructureAction::MoveInto;
          next.destination = outliner_parent(*drop_target);
          next.moved = moving;
          next.reorder_destination = drop_target->node;
          next.reorder_before = drop == DropKind::Before;
        }
      }
      request = std::move(next);
    }
    drag_node_ = doc::NodeId{};
    dragging_ = false;
  } else if (!ctx.pointer_down()) {
    drag_node_ = doc::NodeId{};
    dragging_ = false;
  }
  if (dragging_) {
    // The rows riding under the pointer, so the eye can follow the move.
    const ElementInfo *const dragged = find(drag_node_);
    std::string label = dragged != nullptr ? element_display_name(*dragged)
                                           : std::string{};
    if (moving.size() > 1U) {
      label += " +" + std::to_string(moving.size() - 1U);
    }
    const float width = ctx.font_width(label) + 16.0F;
    const gui::Rect ghost{pointer_x + 12.0F, pointer_y - 10.0F, width, 20.0F};
    ctx.panel(ghost, with_alpha(theme.accent, 0.85F));
    ctx.label(gui::Rect{ghost.x + 8.0F, ghost.y, width - 8.0F, ghost.height},
              label, theme.text);
    ctx.set_cursor(gui::Cursor::Move);
  }

  // F2 renames the selected row while the outliner has the keyboard's
  // attention - the last panel clicked.
  if (focused_ && !ctx.typing() && ctx.key_pressed(gui::Key::Rename)) {
    if (const ElementInfo *const target = find(primary);
        target != nullptr && outliner_lists(*target)) {
      begin_rename(ctx, *target);
    }
  }

  // ------------------------------------------------------------- footer
  const gui::Rect footer{bounds.x,
                         bounds.y + bounds.height - scene_footer_height,
                         bounds.width, scene_footer_height};
  ctx.panel(footer, theme.header);
  ctx.panel(gui::Rect{footer.x, footer.y, footer.width, 1.0F}, theme.divider);
  const auto primary_element = find(primary);
  const bool can_group =
      !selection.empty() &&
      std::all_of(selection.begin(), selection.end(), [&](doc::NodeId node) {
        const ElementInfo *const found = find(node);
        return found != nullptr && !found->locked &&
               outliner_groupable(*found);
      });
  const bool can_ungroup = selection.size() == 1U &&
                           primary_element != nullptr &&
                           !primary_element->locked &&
                           !primary_element->linked &&
                           primary_element->visible &&
                           primary_element->kind == ElementKind::Group;
  constexpr float action_width = 70.0F;
  constexpr float action_gap = 4.0F;
  const gui::Rect ungroup_bounds{footer.x + footer.width - action_width - 5.0F,
                                 footer.y + 4.0F, action_width,
                                 std::max(footer.height - 8.0F, 0.0F)};
  const gui::Rect group_bounds{ungroup_bounds.x - action_width - action_gap,
                               ungroup_bounds.y, action_width,
                               ungroup_bounds.height};
  const auto structure_button = [&](gui::UiId button_id,
                                    const gui::Rect &button_bounds,
                                    std::string_view label,
                                    std::string_view tip, bool enabled) {
    const gui::Interaction interaction =
        enabled ? ctx.grab(button_id, button_bounds) : gui::Interaction{};
    ctx.panel(button_bounds,
              !enabled ? with_alpha(theme.widget, 0.5F)
                       : (interaction.held
                              ? theme.widget_active
                              : (interaction.hovered ? theme.widget_hover
                                                     : theme.widget)));
    border(ctx, button_bounds, enabled ? theme.border : theme.divider);
    const float text_width = ctx.font_width(label);
    ctx.label(gui::Rect{button_bounds.x +
                            std::max((button_bounds.width - text_width) * 0.5F,
                                     2.0F),
                        button_bounds.y, button_bounds.width,
                        button_bounds.height},
              label, enabled ? theme.text : theme.text_muted);
    ctx.tooltip(button_bounds, tip);
    return enabled && interaction.clicked;
  };
  if (structure_button(mix(id_, "scene.structure.group"), group_bounds,
                       "Group", "Put the selection in a new group",
                       can_group)) {
    SceneSelectionRequest next{};
    next.structure = SceneSelectionRequest::StructureAction::Group;
    request = std::move(next);
  }
  if (structure_button(mix(id_, "scene.structure.ungroup"), ungroup_bounds,
                       "Ungroup", "Dissolve the group, keeping its contents",
                       can_ungroup)) {
    SceneSelectionRequest next{};
    next.structure = SceneSelectionRequest::StructureAction::Ungroup;
    request = std::move(next);
  }
  std::string selection_text =
      selection.empty() ? std::string{"No selection"}
                        : (selection.size() == 1U && primary_element != nullptr
                               ? element_display_name(*primary_element)
                               : std::to_string(selection.size()) +
                                     " selected");
  ctx.label(gui::Rect{footer.x + 10.0F, footer.y,
                      std::max(group_bounds.x - footer.x - 16.0F, 0.0F),
                      footer.height},
            selection_text,
            selection.empty() ? theme.text_muted : theme.text);

  // ------------------------------------------------------ context menu
  const gui::UiId menu_id = mix(id_, "scene.menu");
  if (menu_row.has_value()) {
    const ElementInfo &element = *rows[*menu_row].element;
    menu_node_ = element.node;
    menu_entries_.clear();
    menu_commands_.clear();
    const auto add = [&](std::string label, SceneMenuCommand command) {
      menu_entries_.push_back(std::move(label));
      menu_commands_.push_back(static_cast<core::u8>(command));
    };
    const bool in_selection =
        std::find(selection.begin(), selection.end(), element.node) !=
        selection.end();
    if (!in_selection) {
      // A right press acts on what it is over, so the row becomes the
      // selection first - the menu then means exactly that row.
      SceneSelectionRequest next{};
      next.node = element.node;
      next.action = SelectionAction::Replace;
      request = std::move(next);
    }
    if (!element.linked) {
      add("Rename      F2", SceneMenuCommand::Rename);
    }
    add(element.visible ? "Hide" : "Show", SceneMenuCommand::ToggleVisible);
    add("Lock", SceneMenuCommand::ToggleLocked);
    const bool group_ready =
        outliner_groupable(element) &&
        (!in_selection || can_group);
    if (group_ready) {
      add(in_selection && selection.size() > 1U ? "Group Selection"
                                                : "Group",
          SceneMenuCommand::Group);
    }
    if (element.kind == ElementKind::Group && !element.linked &&
        element.visible) {
      add("Ungroup", SceneMenuCommand::Ungroup);
    }
    if (!element.linked && element.parent_group.has_value()) {
      add("Move Out of Group", SceneMenuCommand::MoveOut);
    }
    if (!element.linked_collection) {
      add("Delete", SceneMenuCommand::Delete);
    }
    ctx.open_context_menu(menu_id, pointer_x, pointer_y);
  }
  if (menu_node_ != doc::NodeId{}) {
    std::vector<std::string_view> labels(menu_entries_.begin(),
                                         menu_entries_.end());
    if (const auto chosen = ctx.context_menu(menu_id, labels)) {
      const ElementInfo *const element = find(menu_node_);
      if (element != nullptr && *chosen < menu_commands_.size()) {
        SceneSelectionRequest next{};
        next.node = element->node;
        switch (static_cast<SceneMenuCommand>(menu_commands_[*chosen])) {
        case SceneMenuCommand::Rename:
          begin_rename(ctx, *element);
          break;
        case SceneMenuCommand::ToggleVisible:
          next.visibility_changed = true;
          next.visible = !element->visible;
          request = std::move(next);
          break;
        case SceneMenuCommand::ToggleLocked:
          next.lock_changed = true;
          next.locked = !element->locked;
          request = std::move(next);
          break;
        case SceneMenuCommand::Group:
          next.structure = SceneSelectionRequest::StructureAction::Group;
          request = std::move(next);
          break;
        case SceneMenuCommand::Ungroup:
          next.structure = SceneSelectionRequest::StructureAction::Ungroup;
          request = std::move(next);
          break;
        case SceneMenuCommand::MoveOut: {
          next.structure = SceneSelectionRequest::StructureAction::MoveInto;
          const ElementInfo *const parent = find(*element->parent_group);
          next.destination = parent != nullptr
                                 ? outliner_parent(*parent)
                                 : doc::NodeId{};
          next.moved = {element->node};
          if (parent != nullptr) {
            next.reorder_destination = parent->node;
            next.reorder_before = false;
          }
          request = std::move(next);
          break;
        }
        case SceneMenuCommand::Delete:
          next.delete_selection = true;
          request = std::move(next);
          break;
        }
      }
      menu_node_ = doc::NodeId{};
    } else if (!ctx.menu_open(menu_id)) {
      menu_node_ = doc::NodeId{};
    }
  }

  rename_.end_frame(ctx);
  ctx.pop_clip();
  return request;
}

std::string library_proxy_runtime_status(
    const LibraryProxyRuntimeObservation &observation) {
  if (observation.transcode_progress.has_value()) {
    const LibraryProxyTranscodeProgress &progress =
        *observation.transcode_progress;
    if (progress.queued) {
      return "PROXY QUEUED";
    }
    if (progress.total_frames == 0U) {
      return "TRANSCODING PROXY";
    }
    const core::u64 percent = std::min<core::u64>(
        100U,
        (std::min(progress.completed_frames, progress.total_frames) * 100U +
         progress.total_frames / 2U) /
            progress.total_frames);
    return "TRANSCODING PROXY " + std::to_string(percent) + "%";
  }
  switch (observation.state) {
  case LibraryProxyRuntimeState::Source:
    return "SOURCE";
  case LibraryProxyRuntimeState::ProxyReady:
    return "PROXY READY";
  case LibraryProxyRuntimeState::PreferFallback:
    return "SOURCE (PROXY " +
           std::string{library_proxy_reason_detail(observation.reason)} + ")";
  case LibraryProxyRuntimeState::RequiredUnready:
    return "REQUIRED - UNREADY (" +
           std::string{library_proxy_reason_detail(observation.reason)} + ")";
  case LibraryProxyRuntimeState::Failed: {
    const std::string_view issue =
        library_proxy_issue_detail(observation.issue);
    const std::string_view detail =
        issue.empty() ? library_proxy_reason_detail(observation.reason) : issue;
    return "PROXY FAILED (" + std::string{detail} + ")";
  }
  }
  return "PROXY FAILED (UNKNOWN)";
}

LibraryFrame LibraryPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                                const assets::AssetRegistry &registry) {
  return draw_impl(ctx, bounds, registry, {}, nullptr, {});
}

LibraryFrame
LibraryPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                   const assets::AssetRegistry &registry,
                   std::span<const LibraryThumbnailObservation> thumbnails,
                   LibraryThumbnailWindow window) {
  return draw_impl(ctx, bounds, registry, thumbnails, std::addressof(window),
                   {});
}

LibraryFrame LibraryPanel::draw(
    gui::UiContext &ctx, const gui::Rect &bounds,
    const assets::AssetRegistry &registry,
    std::span<const LibraryThumbnailObservation> thumbnails,
    LibraryThumbnailWindow window,
    std::span<const LibraryProxyRuntimeObservation> proxy_runtime) {
  return draw_impl(ctx, bounds, registry, thumbnails, std::addressof(window),
                   proxy_runtime);
}

LibraryFrame LibraryPanel::draw_impl(
    gui::UiContext &ctx, const gui::Rect &bounds,
    const assets::AssetRegistry &registry,
    std::span<const LibraryThumbnailObservation> thumbnails,
    const LibraryThumbnailWindow *thumbnail_window,
    std::span<const LibraryProxyRuntimeObservation> proxy_runtime) {
  LibraryFrame frame;
  if (thumbnail_window != nullptr) {
    frame.thumbnail_retain.emplace();
  }
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return frame;
  }

  const bool thumbnails_enabled = thumbnail_window != nullptr;
  const float current_asset_row_height =
      thumbnails_enabled ? thumbnail_asset_row_height : asset_row_height;

  ctx.push_clip(bounds);
  ctx.panel(bounds);
  std::array<char, 32U> asset_count{};
  const int asset_count_length =
      std::snprintf(asset_count.data(), asset_count.size(), "%llu ASSETS",
                    static_cast<unsigned long long>(registry.assets.size()));
  panel_title(ctx, bounds, "",
              std::string_view{
                  asset_count.data(),
                  static_cast<std::size_t>(std::max(asset_count_length, 0))});
  constexpr float rescan_width = 68.0F;
  const bool room_to_rescan = bounds.width > rescan_width * 2.0F;
  const gui::Rect search_bounds{bounds.x, bounds.y + title_height,
                                room_to_rescan ? bounds.width - rescan_width
                                               : bounds.width,
                                search_height};
  search_field(ctx, mix(id_, "library.search"), search_bounds, search_,
               "Search Assets and Generators...");
  if (room_to_rescan &&
      ctx.button(mix(id_, "library.rescan"),
                 gui::Rect{search_bounds.x + search_bounds.width + 2.0F,
                           search_bounds.y + 2.0F, rescan_width - 4.0F,
                           search_height - 4.0F},
                 "RESCAN")) {
    frame.rescan_assets = true;
  }
  const gui::Rect list_bounds{
      bounds.x, search_bounds.y + search_bounds.height, bounds.width,
      std::max(bounds.height - title_height - search_height - 18.0F, 0.0F)};

  struct AssetRow final {
    const assets::Asset *asset{};
    std::string detail;
  };

  constexpr std::size_t category_count =
      static_cast<std::size_t>(LibraryCategory::Count);
  constexpr std::size_t media_category =
      static_cast<std::size_t>(LibraryCategory::Media);
  constexpr std::size_t model_category =
      static_cast<std::size_t>(LibraryCategory::Models);

  std::array<bool, category_count> category_query{};
  for (std::size_t category = 0U; category < category_count; ++category) {
    category_query[category] =
        matches(library_category_names[category], search_);
  }

  std::array<std::vector<const LibraryItem *>, category_count> items;
  for (const LibraryItem &item : library_items) {
    if (category_query[item.category] || matches(item.name, search_) ||
        matches(item.detail, search_)) {
      items[item.category].push_back(&item);
    }
  }

  std::array<std::vector<AssetRow>, category_count> asset_rows;
  std::array<std::size_t, category_count> total_assets{};
  for (const assets::Asset &asset : registry.assets) {
    const std::size_t category =
        assets::asset_kind(asset) == assets::AssetKind::Media ? media_category
                                                              : model_category;
    ++total_assets[category];
    std::string detail = asset_detail(asset);
    if (category_query[category] || asset_matches(asset, detail, search_)) {
      asset_rows[category].push_back(
          AssetRow{std::addressof(asset), std::move(detail)});
    }
  }

  std::array<bool, category_count> import_visible{};
  import_visible[media_category] = category_query[media_category] ||
                                   matches("Import Media Video Image", search_);
  import_visible[model_category] =
      category_query[model_category] || matches("Import Model 3D", search_);

  std::array<bool, category_count> category_visible{};
  bool has_results = false;
  for (std::size_t category = 0U; category < category_count; ++category) {
    category_visible[category] = search_.empty() || category_query[category] ||
                                 !items[category].empty() ||
                                 !asset_rows[category].empty() ||
                                 import_visible[category];
    has_results = has_results || category_visible[category];
  }

  float content_height = 0.0F;
  for (std::size_t category = 0U; category < category_count; ++category) {
    if (!category_visible[category]) {
      continue;
    }
    content_height += section_row_height;
    if (category_open_[category]) {
      content_height +=
          tree_row_height * static_cast<float>(items[category].size());
      if (import_visible[category]) {
        content_height += import_row_height;
      }
      content_height += current_asset_row_height *
                        static_cast<float>(asset_rows[category].size());
      if (search_.empty() &&
          (category == media_category || category == model_category) &&
          total_assets[category] == 0U) {
        content_height += empty_row_height;
      }
    }
  }
  if (!has_results) {
    content_height = empty_row_height;
  }
  const float maximum_scroll =
      std::max(content_height - list_bounds.height, 0.0F);
  if (list_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
    scroll_ -= ctx.wheel() * tree_row_height * 2.0F;
  }
  scroll_ = std::clamp(scroll_, 0.0F, maximum_scroll);

  struct ThumbnailCandidate final {
    assets::DerivativeKey key;
    LibraryThumbnailState state{LibraryThumbnailState::Placeholder};
    bool visible{};
    float distance_from_view{};
  };
  std::vector<ThumbnailCandidate> thumbnail_candidates;
  if (thumbnails_enabled) {
    thumbnail_candidates.reserve(registry.assets.size());
  }
  const assets::DerivativeProfile *const thumbnail_profile =
      thumbnails_enabled
          ? assets::derivative_profile(
                assets::DerivativeProfileId::Thumbnail320x180Srgb)
          : nullptr;
  const auto thumbnail_observation =
      [thumbnails](const assets::DerivativeKey &key)
      -> const LibraryThumbnailObservation * {
    const auto found =
        std::find_if(thumbnails.begin(), thumbnails.end(),
                     [&key](const LibraryThumbnailObservation &observation) {
                       return observation.key == key;
                     });
    return found == thumbnails.end() ? nullptr : std::addressof(*found);
  };

  ctx.push_clip(list_bounds);
  float y = list_bounds.y - scroll_;
  if (!has_results) {
    ctx.label(gui::Rect{list_bounds.x + 8.0F, y,
                        std::max(list_bounds.width - 16.0F, 0.0F),
                        empty_row_height},
              "NO LIBRARY RESULTS", ctx.theme().text_muted);
  }

  for (std::size_t category = 0U; category < category_count; ++category) {
    if (!category_visible[category]) {
      continue;
    }
    const gui::Rect category_row{list_bounds.x, y, list_bounds.width,
                                 section_row_height};
    const gui::Interaction category_interaction = clipped_grab(
        ctx, mix(id_, "library.category", category), category_row, list_bounds);
    if (intersect(category_row, list_bounds).height > 0.0F) {
      ctx.panel(category_row, category_interaction.hovered
                                  ? ctx.theme().widget_hover
                                  : ctx.theme().header);
      draw_twirl(ctx, category_row.x + 11.0F,
                 category_row.y + category_row.height * 0.5F,
                 category_open_[category], ctx.theme().text_muted);
      ctx.label(gui::Rect{category_row.x + 22.0F, category_row.y,
                          std::max(category_row.width - 58.0F, 0.0F),
                          category_row.height},
                library_category_names[category], ctx.theme().text_muted);
      const std::size_t visible_count =
          items[category].size() + asset_rows[category].size();
      if (visible_count > 0U && category_row.width > 90.0F) {
        std::array<char, 16U> count{};
        const int count_length =
            std::snprintf(count.data(), count.size(), "%llu",
                          static_cast<unsigned long long>(visible_count));
        ctx.label(gui::Rect{category_row.x + category_row.width - 29.0F,
                            category_row.y, 21.0F, category_row.height},
                  std::string_view{
                      count.data(),
                      static_cast<std::size_t>(std::max(count_length, 0))},
                  ctx.theme().text_muted);
      }
    }
    if (category_interaction.clicked) {
      category_open_[category] = !category_open_[category];
    }
    y += section_row_height;
    if (!category_open_[category]) {
      continue;
    }

    std::size_t item_index = 0U;
    for (const LibraryItem *const item : items[category]) {
      const gui::Rect row{list_bounds.x, y, list_bounds.width, tree_row_height};
      const gui::Interaction interaction = clipped_grab(
          ctx, mix(id_, "library.item", static_cast<core::u64>(item->action)),
          row, list_bounds);
      if (intersect(row, list_bounds).height > 0.0F) {
        ctx.panel(row, interaction.hovered
                           ? scale(ctx.theme().widget_hover, 1.08F)
                           : (item_index % 2U == 0U ? ctx.theme().row
                                                    : ctx.theme().row_alt));
        const gui::Rect icon{row.x + 8.0F, row.y + 2.0F, 16.0F, 16.0F};
        draw_library_icon(ctx, item->action, icon);
        ctx.label(gui::Rect{row.x + 31.0F, row.y,
                            std::max(row.width * 0.54F, 0.0F), row.height},
                  item->name, ctx.theme().text);
        draw_plus(ctx, row.x + row.width - 12.0F, row.y + row.height * 0.5F,
                  interaction.hovered ? ctx.theme().accent
                                      : ctx.theme().text_muted);
      }
      if (interaction.clicked) {
        frame.add = item->action;
      }
      y += tree_row_height;
      ++item_index;
    }

    if (import_visible[category]) {
      const gui::Rect row{list_bounds.x, y, list_bounds.width,
                          import_row_height};
      const gui::Rect button{row.x + 7.0F, row.y + 3.0F,
                             std::max(row.width - 14.0F, 0.0F),
                             row.height - 6.0F};
      const gui::Interaction interaction = clipped_grab(
          ctx, mix(id_, "library.import", category), button, list_bounds);
      if (intersect(row, list_bounds).height > 0.0F) {
        ctx.panel(button, interaction.hovered
                              ? ctx.theme().widget_hover
                              : scale(ctx.theme().widget, 0.94F));
        border(ctx, button,
               interaction.hovered ? ctx.theme().accent : ctx.theme().divider);
        draw_plus(ctx, button.x + 10.0F, button.y + button.height * 0.5F,
                  interaction.hovered ? ctx.theme().accent
                                      : ctx.theme().text_muted);
        ctx.label(
            gui::Rect{button.x + 20.0F, button.y,
                      std::max(button.width - 25.0F, 0.0F), button.height},
            category == media_category ? "IMPORT MEDIA" : "IMPORT MODEL",
            interaction.hovered ? ctx.theme().text : ctx.theme().text_muted);
      }
      if (interaction.clicked) {
        frame.import_asset = category == media_category
                                 ? LibraryImportAction::Media
                                 : LibraryImportAction::Model;
      }
      y += import_row_height;
    }

    std::size_t asset_index = 0U;
    for (const AssetRow &entry : asset_rows[category]) {
      const gui::Rect row{list_bounds.x, y, list_bounds.width,
                          current_asset_row_height};
      const doc::NodeId asset_id = assets::asset_id(*entry.asset);
      const bool is_media =
          assets::asset_kind(*entry.asset) == assets::AssetKind::Media;
      const auto *const media = std::get_if<assets::MediaAsset>(entry.asset);
      const LibraryProxyRuntimeObservation *const proxy_observation =
          media != nullptr
              ? library_proxy_runtime_observation(*media, proxy_runtime)
              : nullptr;
      const auto decode_observation =
          std::find_if(proxy_runtime.begin(), proxy_runtime.end(),
                       [&](const LibraryProxyRuntimeObservation &observation) {
                         return observation.asset == asset_id;
                       });
      const bool cpu_decode_only =
          decode_observation != proxy_runtime.end() &&
          decode_observation->media_decode == LibraryMediaDecodeState::CpuOnly;
      std::optional<assets::DerivativeKey> thumbnail_key;
      const LibraryThumbnailObservation *observation = nullptr;
      LibraryThumbnailState thumbnail_state =
          LibraryThumbnailState::Placeholder;
      if (thumbnail_profile != nullptr && is_media) {
        const std::optional<assets::ContentFingerprint> &fingerprint =
            assets::asset_fingerprint(*entry.asset);
        if (fingerprint.has_value()) {
          const assets::DerivativeKey candidate_key =
              assets::make_derivative_key(asset_id, *fingerprint,
                                          *thumbnail_profile);
          if (assets::valid_derivative_key(candidate_key)) {
            thumbnail_key = candidate_key;
            observation = thumbnail_observation(*thumbnail_key);
            if (observation != nullptr) {
              thumbnail_state = observation->state;
            }
            const gui::Rect visible_row = intersect(row, list_bounds);
            const bool visible =
                visible_row.width > 0.0F && visible_row.height > 0.0F;
            float distance_from_view = 0.0F;
            if (!visible && row.y + row.height <= list_bounds.y) {
              distance_from_view = list_bounds.y - row.y - row.height;
            } else if (!visible) {
              distance_from_view = row.y - (list_bounds.y + list_bounds.height);
            }
            thumbnail_candidates.push_back(ThumbnailCandidate{
                *thumbnail_key, thumbnail_state, visible, distance_from_view});
          }
        }
      }
      const float action_width = std::min(28.0F, row.width);
      const gui::Rect content{
          row.x, row.y, std::max(row.width - action_width, 0.0F), row.height};
      const float remove_height = is_media ? row.height * 0.5F : row.height;
      const gui::Rect remove{content.x + content.width, row.y, action_width,
                             remove_height};
      const gui::Rect proxy{content.x + content.width, row.y + remove_height,
                            action_width,
                            std::max(row.height - remove_height, 0.0F)};
      const gui::Interaction interaction =
          clipped_grab(ctx, mix(id_, "library.asset", asset_id.value()),
                       content, list_bounds);
      const gui::Interaction remove_interaction =
          clipped_grab(ctx, mix(id_, "library.asset.remove", asset_id.value()),
                       remove, list_bounds);
      const gui::Interaction proxy_interaction =
          is_media
              ? clipped_grab(ctx,
                             mix(id_, "library.asset.proxy", asset_id.value()),
                             proxy, list_bounds)
              : gui::Interaction{};
      if (intersect(row, list_bounds).height > 0.0F) {
        ctx.panel(row, interaction.hovered || remove_interaction.hovered ||
                               proxy_interaction.hovered
                           ? scale(ctx.theme().widget_hover, 1.06F)
                           : (asset_index % 2U == 0U ? ctx.theme().row
                                                     : ctx.theme().row_alt));
        if (thumbnails_enabled) {
          float preview_width = std::min(thumbnail_preview_width,
                                         std::max(content.width - 14.0F, 0.0F));
          float preview_height = preview_width / thumbnail_preview_aspect;
          const float maximum_preview_height =
              std::max(row.height - 8.0F, 0.0F);
          if (preview_height > maximum_preview_height) {
            preview_height = maximum_preview_height;
            preview_width = preview_height * thumbnail_preview_aspect;
          }
          const gui::Rect preview{
              row.x + 6.0F,
              row.y + std::max((row.height - preview_height) * 0.5F, 0.0F),
              preview_width, preview_height};
          const gui::UiColor thumbnail_color =
              library_thumbnail_color(ctx, thumbnail_state);
          if (preview.width > 0.0F && preview.height > 0.0F) {
            ctx.panel(preview, scale(ctx.theme().track, 0.84F));
            if (observation != nullptr &&
                observation->state == LibraryThumbnailState::Ready &&
                observation->texture != nullptr) {
              ctx.image(preview, *observation->texture);
            } else if (preview.width >= 8.0F && preview.height >= 8.0F) {
              const float icon_size =
                  std::min({16.0F, preview.width, preview.height});
              const gui::Rect icon{
                  preview.x + (preview.width - icon_size) * 0.5F,
                  preview.y + (preview.height - icon_size) * 0.5F, icon_size,
                  icon_size};
              draw_library_icon(ctx,
                                is_media ? LibraryAddAction::Media
                                         : LibraryAddAction::Model,
                                icon);
            }
            border(ctx, preview, with_alpha(thumbnail_color, 0.78F));
          }

          const float text_x = preview.x + preview.width + 8.0F;
          const float text_width =
              std::max(content.x + content.width - text_x - 17.0F, 0.0F);
          ctx.label(gui::Rect{text_x, row.y + 2.0F, text_width, 16.0F},
                    assets::asset_display_name(*entry.asset), ctx.theme().text);
          ctx.label(gui::Rect{text_x, row.y + 18.0F, text_width, 14.0F},
                    entry.detail,
                    scale(ctx.theme().text_muted, 0.90F));
          ctx.label(gui::Rect{text_x, row.y + 33.0F, text_width, 14.0F},
                    "", ctx.theme().text_muted);
          std::string status{library_thumbnail_status(thumbnail_state)};
          if (proxy_observation != nullptr) {
            if (!status.empty()) {
              status += " | ";
            }
            status += library_proxy_runtime_status(*proxy_observation);
          } else if (media != nullptr) {
            if (!status.empty()) {
              status += " | ";
            }
            status += "PROXY ";
            status += library_proxy_label(media->proxy_playback_policy);
          }
          if (cpu_decode_only) {
            if (!status.empty()) {
              status += " | ";
            }
            status += "OUTPUT CPU DECODE ONLY";
          }
          if (!status.empty()) {
            ctx.label(
                gui::Rect{text_x, row.y + 49.0F, text_width, 15.0F}, status,
                proxy_observation != nullptr &&
                        proxy_observation->transcode_progress.has_value()
                    ? ctx.theme().accent
                : proxy_observation != nullptr
                    ? library_proxy_runtime_color(ctx, proxy_observation->state)
                    : thumbnail_color);
          }
        } else {
          const gui::Rect icon{row.x + 8.0F, row.y + 15.0F, 16.0F, 16.0F};
          draw_library_icon(
              ctx, is_media ? LibraryAddAction::Media : LibraryAddAction::Model,
              icon);
          ctx.label(gui::Rect{row.x + 31.0F, row.y + 1.0F,
                              std::max(content.width - 48.0F, 0.0F), 15.0F},
                    assets::asset_display_name(*entry.asset), ctx.theme().text);
          ctx.label(gui::Rect{row.x + 31.0F, row.y + 16.0F,
                              std::max(content.width - 48.0F, 0.0F), 15.0F},
                    entry.detail,
                    scale(ctx.theme().text_muted, 0.90F));
          ctx.label(gui::Rect{row.x + 31.0F, row.y + 31.0F,
                              std::max(content.width - 48.0F, 0.0F), 15.0F},
                    "", ctx.theme().text_muted);
        }
        ctx.tooltip(content, assets::asset_source(*entry.asset));
        if (cpu_decode_only) {
          draw_cpu_decode_warning(ctx,
                                  gui::Rect{content.x + content.width - 18.0F,
                                            row.y + 4.0F, 12.0F, 11.0F});
        }
        if (proxy_observation != nullptr &&
            proxy_observation->transcode_progress.has_value()) {
          const LibraryProxyTranscodeProgress &progress =
              *proxy_observation->transcode_progress;
          const float fraction =
              progress.total_frames == 0U
                  ? 0.0F
                  : std::clamp(static_cast<float>(progress.completed_frames) /
                                   static_cast<float>(progress.total_frames),
                               0.0F, 1.0F);
          const gui::Rect progress_track{
              content.x + 6.0F, row.y + row.height - 4.0F,
              std::max(content.width - 12.0F, 0.0F), 3.0F};
          ctx.panel(progress_track, scale(ctx.theme().track, 0.78F));
          if (fraction > 0.0F) {
            ctx.panel(gui::Rect{progress_track.x, progress_track.y,
                                progress_track.width * fraction,
                                progress_track.height},
                      ctx.theme().accent);
          }
        }
        draw_plus(
            ctx, content.x + content.width - 10.0F, row.y + row.height * 0.5F,
            interaction.hovered ? ctx.theme().accent : ctx.theme().text_muted);
        ctx.panel(gui::Rect{remove.x, row.y + 6.0F, 1.0F,
                            std::max(row.height - 12.0F, 0.0F)},
                  ctx.theme().divider);
        draw_remove(ctx, remove.x + remove.width * 0.5F,
                    remove.y + remove.height * 0.5F,
                    remove_interaction.hovered ? ctx.theme().warning
                                               : ctx.theme().text_muted);
        if (is_media) {
          ctx.panel(gui::Rect{proxy.x + 4.0F, proxy.y,
                              std::max(proxy.width - 8.0F, 0.0F), 1.0F},
                    ctx.theme().divider);
          ctx.label(proxy,
                    media != nullptr
                        ? library_proxy_label(media->proxy_playback_policy)
                        : std::string_view{"SRC"},
                    proxy_interaction.hovered ? ctx.theme().accent
                                              : ctx.theme().text_muted);
        }
      }
      if (interaction.pressed) {
        drag_asset_ = asset_id;
        drag_start_x_ = ctx.pointer_x();
        drag_start_y_ = ctx.pointer_y();
        drag_moved_ = false;
        drag_label_ = std::string{assets::asset_display_name(*entry.asset)};
      }
      if (interaction.held && drag_asset_ == asset_id) {
        const float dx = ctx.pointer_x() - drag_start_x_;
        const float dy = ctx.pointer_y() - drag_start_y_;
        drag_moved_ = drag_moved_ || dx * dx + dy * dy >= 16.0F;
        if (drag_moved_) {
          frame.dragging_asset = asset_id;
        }
      }
      if (interaction.released && drag_asset_ == asset_id) {
        if (drag_moved_) {
          frame.asset_drop =
              LibraryAssetDrop{asset_id, ctx.pointer_x(), ctx.pointer_y()};
        } else if (interaction.clicked) {
          frame.instantiate_asset = asset_id;
        }
        drag_asset_ = {};
        drag_moved_ = false;
        drag_label_.clear();
      }
      if (remove_interaction.clicked) {
        frame.remove_asset = asset_id;
        if (drag_asset_ == asset_id) {
          drag_asset_ = {};
          drag_moved_ = false;
          drag_label_.clear();
        }
      }
      if (proxy_interaction.clicked && media != nullptr) {
        LibraryProxyPolicyIntent intent{};
        intent.asset = asset_id;
        switch (media->proxy_playback_policy) {
        case assets::ProxyPlaybackPolicy::Source:
          intent.requested_profile = media->requested_proxy_profile.value_or(
              cpu_decode_only
                  ? assets::DerivativeProfileId::ProxySourceCfrHevcMain10
                  : assets::DerivativeProfileId::Proxy720pCfrH264);
          intent.policy = assets::ProxyPlaybackPolicy::PreferProxy;
          break;
        case assets::ProxyPlaybackPolicy::PreferProxy:
          intent.requested_profile = media->requested_proxy_profile.value_or(
              assets::DerivativeProfileId::Proxy720pCfrH264);
          intent.policy = assets::ProxyPlaybackPolicy::RequireProxy;
          break;
        case assets::ProxyPlaybackPolicy::RequireProxy:
          intent.requested_profile.reset();
          intent.policy = assets::ProxyPlaybackPolicy::Source;
          break;
        }
        frame.proxy_policy = intent;
      }
      y += current_asset_row_height;
      ++asset_index;
    }

    if (search_.empty() &&
        (category == media_category || category == model_category) &&
        total_assets[category] == 0U) {
      const gui::Rect row{list_bounds.x, y, list_bounds.width,
                          empty_row_height};
      if (intersect(row, list_bounds).height > 0.0F) {
        ctx.label(gui::Rect{row.x + 31.0F, row.y,
                            std::max(row.width - 39.0F, 0.0F), row.height},
                  category == media_category ? "NO MEDIA IMPORTED"
                                             : "NO MODELS IMPORTED",
                  scale(ctx.theme().text_muted, 0.82F));
      }
      y += empty_row_height;
    }
  }

  if (thumbnail_window != nullptr) {
    const std::size_t prefetch_limit = static_cast<std::size_t>(
        std::min(thumbnail_window->prefetch_asset_rows,
                 maximum_library_thumbnail_prefetch_rows));
    const std::size_t request_limit = static_cast<std::size_t>(
        std::min(thumbnail_window->maximum_request_intents,
                 maximum_library_thumbnail_request_intents));
    LibraryThumbnailRetainIntent &retain = *frame.thumbnail_retain;
    const auto retain_contains = [&retain](const assets::DerivativeKey &key) {
      return std::find(retain.keys.begin(), retain.keys.end(), key) !=
             retain.keys.end();
    };
    const auto requests_contain = [&frame](const assets::DerivativeKey &key) {
      return std::find_if(frame.thumbnail_requests.begin(),
                          frame.thumbnail_requests.end(),
                          [&key](const LibraryThumbnailRequestIntent &request) {
                            return request.key == key;
                          }) != frame.thumbnail_requests.end();
    };
    const auto append_candidate =
        [&frame, &retain, &retain_contains, &requests_contain,
         request_limit](const ThumbnailCandidate &candidate,
                        LibraryThumbnailRequestPriority priority) {
          if (!retain_contains(candidate.key)) {
            retain.keys.push_back(candidate.key);
          }
          const bool needs_request =
              candidate.state == LibraryThumbnailState::Placeholder ||
              candidate.state == LibraryThumbnailState::Stale;
          if (needs_request &&
              frame.thumbnail_requests.size() < request_limit &&
              !requests_contain(candidate.key)) {
            frame.thumbnail_requests.push_back(
                LibraryThumbnailRequestIntent{candidate.key, priority});
          }
        };

    std::vector<const ThumbnailCandidate *> prefetch_candidates;
    prefetch_candidates.reserve(thumbnail_candidates.size());
    for (const ThumbnailCandidate &candidate : thumbnail_candidates) {
      if (candidate.visible) {
        append_candidate(candidate, LibraryThumbnailRequestPriority::Visible);
      } else {
        prefetch_candidates.push_back(std::addressof(candidate));
      }
    }
    std::stable_sort(
        prefetch_candidates.begin(), prefetch_candidates.end(),
        [](const ThumbnailCandidate *left, const ThumbnailCandidate *right) {
          if (left->distance_from_view != right->distance_from_view) {
            return left->distance_from_view < right->distance_from_view;
          }
          return left->key.asset.value() < right->key.asset.value();
        });
    const std::size_t selected_prefetch =
        std::min(prefetch_limit, prefetch_candidates.size());
    for (std::size_t index = 0U; index < selected_prefetch; ++index) {
      append_candidate(*prefetch_candidates[index],
                       LibraryThumbnailRequestPriority::Prefetch);
    }
  }
  ctx.pop_clip();

  if (maximum_scroll > 0.0F && list_bounds.height > 0.0F) {
    const gui::Rect track{list_bounds.x + list_bounds.width - 4.0F,
                          list_bounds.y, 3.0F, list_bounds.height};
    ctx.panel(track, scale(ctx.theme().divider, 0.75F));
    const float thumb_height = std::max(
        list_bounds.height * list_bounds.height / content_height, 20.0F);
    const float thumb_range = std::max(list_bounds.height - thumb_height, 0.0F);
    const float thumb_y =
        list_bounds.y + thumb_range * (scroll_ / maximum_scroll);
    ctx.panel(gui::Rect{track.x, thumb_y, track.width, thumb_height},
              ctx.theme().text_muted);
  }

  const gui::Rect footer{bounds.x, bounds.y + bounds.height - 18.0F,
                         bounds.width, 18.0F};
  ctx.panel(footer, ctx.theme().background);
  ctx.panel(gui::Rect{footer.x, footer.y, footer.width, 1.0F},
            ctx.theme().divider);
  ctx.pop_clip();

  if (frame.dragging_asset.has_value()) {
    const float ghost_width = std::clamp(
        static_cast<float>(drag_label_.size()) * 7.0F + 28.0F, 108.0F, 220.0F);
    const gui::Rect ghost{ctx.pointer_x() + 12.0F, ctx.pointer_y() + 10.0F,
                          ghost_width, 24.0F};
    ctx.panel(ghost, with_alpha(ctx.theme().background, 0.94F));
    border(ctx, ghost, ctx.theme().accent);
    draw_plus(ctx, ghost.x + 10.0F, ghost.y + ghost.height * 0.5F,
              ctx.theme().accent);
    ctx.label(gui::Rect{ghost.x + 21.0F, ghost.y,
                        std::max(ghost.width - 25.0F, 0.0F), ghost.height},
              drag_label_, ctx.theme().text);
  }
  return frame;
}

std::optional<LibraryAddAction> LibraryPanel::draw(gui::UiContext &ctx,
                                                   const gui::Rect &bounds) {
  static const assets::AssetRegistry empty_registry;
  const LibraryFrame frame = draw(ctx, bounds, empty_registry);
  if (frame.add.has_value()) {
    return frame.add;
  }
  if (frame.import_asset == LibraryImportAction::Media) {
    return LibraryAddAction::Media;
  }
  if (frame.import_asset == LibraryImportAction::Model) {
    return LibraryAddAction::Model;
  }
  return std::nullopt;
}

std::optional<std::pair<doc::NodeId, gui::Rect>>
CompositorPanel::node_at(const compositor::CompositorGraph &graph,
                         float x, float y) const noexcept {
  if (!last_canvas_.contains(x, y)) return std::nullopt;
  for (auto it = graph.nodes.rbegin(); it != graph.nodes.rend(); ++it) {
    const bool pending =
        pending_move_.has_value() && pending_move_->node == it->node;
    const gui::UiPoint size = node_size(*it);
    const gui::Rect bounds{
        last_canvas_.x + pan_x_ + (pending ? pending_move_->x : it->editor_x) * zoom_,
        last_canvas_.y + pan_y_ + (pending ? pending_move_->y : it->editor_y) * zoom_,
        size.x * zoom_, size.y * zoom_};
    if (bounds.contains(x, y)) return std::pair{it->node, intersect(bounds, last_canvas_)};
  }
  return std::nullopt;
}

gui::UiPoint
CompositorPanel::node_size(const compositor::GraphNode &node) const noexcept {
  const auto found = std::lower_bound(
      node_sizes_.begin(), node_sizes_.end(), node.node,
      [](const NodeSize &size, doc::NodeId id) noexcept {
        return size.node < id;
      });
  if (found != node_sizes_.end() && found->node == node.node) {
    return gui::UiPoint{found->width, found->height};
  }
  return new_node_size(node.kind);
}

gui::UiPoint
CompositorPanel::new_node_size(compositor::NodeKind kind) const noexcept {
  const auto index = static_cast<std::size_t>(kind);
  if (index < new_node_sizes_.size() && new_node_sizes_[index].x > 0.0F) {
    return new_node_sizes_[index];
  }
  // Not drawn yet: the minimum width at the nominal 14-unit font.
  const CompositorNodeMetrics nominal{35.0F, 22.0F, 7.0F};
  return gui::UiPoint{compositor_node_min_width,
                      compositor_node_height(metrics_.row_height > 0.0F
                                                 ? metrics_
                                                 : nominal,
                                             kind)};
}

std::optional<compositor::EdgeEndpoint>
CompositorPanel::input_port_at(const compositor::CompositorGraph &graph,
                               float x, float y) const noexcept {
  if (!last_canvas_.contains(x, y)) return std::nullopt;
  // Topmost node first, as node_at() does, so the answer is the port the
  // operator can see.
  for (auto it = graph.nodes.rbegin(); it != graph.nodes.rend(); ++it) {
    const bool pending =
        pending_move_.has_value() && pending_move_->node == it->node;
    const gui::UiPoint size = node_size(*it);
    const float left =
        last_canvas_.x + pan_x_ + (pending ? pending_move_->x : it->editor_x) * zoom_;
    const float top =
        last_canvas_.y + pan_y_ + (pending ? pending_move_->y : it->editor_y) * zoom_;
    const gui::Rect bounds{left, top, size.x * zoom_, size.y * zoom_};
    const float reach = compositor_port_radius * zoom_ + 4.0F;
    const gui::Rect with_dots{bounds.x - reach, bounds.y,
                              bounds.width + reach, bounds.height};
    if (!with_dots.contains(x, y)) {
      continue;
    }
    const auto ports = compositor::input_ports(it->kind);
    for (std::size_t index = 0U; index < ports.size(); ++index) {
      const float row_top = top + (metrics_.header_height +
                                   metrics_.row_height *
                                       static_cast<float>(index)) * zoom_;
      const gui::Rect row{left - reach, row_top,
                          bounds.width * 0.5F + reach,
                          metrics_.row_height * zoom_};
      if (row.contains(x, y)) {
        return compositor::EdgeEndpoint{it->node, ports[index].index};
      }
    }
    if (bounds.contains(x, y)) {
      return std::nullopt; // over this node, but not over one of its inputs
    }
  }
  return std::nullopt;
}

CompositorFrame
CompositorPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                      const compositor::CompositorGraph &compositor_graph,
                      std::span<const ElementInfo> elements) {
  CompositorFrame frame;
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return frame;
  }
  if (ctx.pointer_pressed()) {
    focused_ = bounds.contains(ctx.pointer_x(), ctx.pointer_y());
  }
  if ((selection_.kind == CompositorSelectionKind::Node &&
       compositor_graph.node(selection_.node) == nullptr) ||
      (selection_.kind == CompositorSelectionKind::Edge &&
       compositor_graph.edge(selection_.node) == nullptr)) {
    selection_ = CompositorSelection{};
  }

  const gui::Rect toolbar{bounds.x, bounds.y, bounds.width,
                          std::min(bounds.height, compositor_toolbar_height)};
  const gui::Rect content{bounds.x, bounds.y + toolbar.height, bounds.width,
                          std::max(bounds.height - toolbar.height, 0.0F)};

  const compositor::GraphNode *parameter_node =
      selection_.kind == CompositorSelectionKind::Node
          ? compositor_graph.node(selection_.node)
          : nullptr;
  if (parameter_node != nullptr &&
      !has_editable_parameters(parameter_node->kind)) {
    parameter_node = nullptr;
  }
  if (parameter_node == nullptr) {
    if (parameter_node_.value() != 0U) {
      parameter_fields_.cancel(ctx);
      parameter_node_ = {};
    }
  } else if (parameter_node_ != parameter_node->node) {
    parameter_fields_.cancel(ctx);
    parameter_node_ = parameter_node->node;
    parameter_scroll_ = 0.0F;
    parameter_content_height_ = 0.0F;
  }

  // Every node's size this frame, from the font it is drawn in.
  metrics_ = compositor_node_metrics(ctx);
  for (const CompositorPaletteItem &item : compositor_palette_items) {
    new_node_sizes_[static_cast<std::size_t>(item.kind)] = gui::UiPoint{
        compositor_node_width(ctx, item.kind,
                              compositor::node_kind_name(item.kind), {}),
        compositor_node_height(metrics_, item.kind)};
  }
  node_sizes_.clear();
  node_sizes_.reserve(compositor_graph.nodes.size());
  for (const compositor::GraphNode &node : compositor_graph.nodes) {
    const std::string_view title =
        node.label.empty() ? compositor::node_kind_name(node.kind)
                           : std::string_view{node.label};
    const std::string source =
        node.kind == compositor::NodeKind::SceneLayer
            ? compositor_node_detail(node, elements)
            : std::string{};
    node_sizes_.push_back(NodeSize{
        node.node, compositor_node_width(ctx, node.kind, title, source),
        compositor_node_height(metrics_, node.kind)});
  }
  std::sort(node_sizes_.begin(), node_sizes_.end(),
            [](const NodeSize &left, const NodeSize &right) noexcept {
              return left.node < right.node;
            });

  // A released move keeps its position until the graph reports the edit.
  if (pending_move_.has_value()) {
    const compositor::GraphNode *const moved =
        compositor_graph.node(pending_move_->node);
    if (moved == nullptr || moved->editor_x != pending_move_->from_x ||
        moved->editor_y != pending_move_->from_y ||
        ++pending_move_->frames > compositor_pending_move_frames) {
      pending_move_.reset();
    }
  }

  gui::Rect canvas = content;
  gui::Rect parameter_bounds{};
  // The seam the operator drags to resize Parameters: its left edge beside
  // the graph, or its top edge when the panel is too narrow for both.
  gui::Rect parameter_splitter{};
  bool parameter_side = false;
  bool parameter_resize_pressed = false;
  constexpr float minimum_graph_width = 256.0F;
  constexpr float minimum_graph_height = 72.0F;
  constexpr float minimum_parameter_width = 200.0F;
  constexpr float default_parameter_width = 280.0F;
  constexpr float maximum_parameter_width = 640.0F;
  constexpr float minimum_parameter_height = 96.0F;
  constexpr float splitter_reach = 3.0F;
  if (parameter_node != nullptr) {
    parameter_side =
        content.width >= minimum_graph_width + minimum_parameter_width;
    if (parameter_side) {
      const float requested =
          parameter_width_ > 0.0F ? parameter_width_ : default_parameter_width;
      const float width = std::clamp(
          requested, minimum_parameter_width,
          std::clamp(content.width - minimum_graph_width,
                     minimum_parameter_width, maximum_parameter_width));
      canvas.width = std::max(content.width - width, 0.0F);
      parameter_bounds =
          gui::Rect{canvas.x + canvas.width, content.y, width, content.height};
      parameter_splitter =
          gui::Rect{parameter_bounds.x - splitter_reach, parameter_bounds.y,
                    splitter_reach * 2.0F, parameter_bounds.height};
    } else {
      const float available =
          std::max(content.height - minimum_graph_height, 0.0F);
      const float requested = parameter_height_ > 0.0F
                                  ? parameter_height_
                                  : parameter_panel_height(*parameter_node);
      const float height = std::min(
          std::max(requested, std::min(minimum_parameter_height, available)),
          available);
      canvas.height = std::max(content.height - height, 0.0F);
      parameter_bounds =
          gui::Rect{content.x, canvas.y + canvas.height, content.width, height};
      parameter_splitter =
          gui::Rect{parameter_bounds.x, parameter_bounds.y - splitter_reach,
                    parameter_bounds.width, splitter_reach * 2.0F};
    }
    // Claimed before the graph so a press on the seam resizes rather than
    // starting a node drag under it.
    const gui::Interaction resize = clipped_grab(
        ctx, mix(id_, "compositor.parameters.resize"), parameter_splitter,
        content);
    parameter_resize_pressed = resize.pressed;
    ctx.cursor_for(resize, parameter_side ? gui::Cursor::ResizeHorizontal
                                          : gui::Cursor::ResizeVertical);
    if (resize.held) {
      if (parameter_side) {
        parameter_width_ = std::clamp(
            content.x + content.width - ctx.pointer_x(),
            minimum_parameter_width,
            std::clamp(content.width - minimum_graph_width,
                       minimum_parameter_width, maximum_parameter_width));
      } else {
        parameter_height_ = std::clamp(
            content.y + content.height - ctx.pointer_y(),
            std::min(minimum_parameter_height,
                     std::max(content.height - minimum_graph_height, 0.0F)),
            std::max(content.height - minimum_graph_height, 0.0F));
      }
    }
  }
  last_canvas_ = canvas;
  const auto set_zoom = [&](float requested, float anchor_x,
                            float anchor_y) noexcept {
    const float next =
        std::clamp(requested, compositor_minimum_zoom, compositor_maximum_zoom);
    if (next == zoom_ || canvas.width <= 0.0F || canvas.height <= 0.0F) {
      return;
    }
    const float local_x = anchor_x - canvas.x;
    const float local_y = anchor_y - canvas.y;
    const float graph_x = (local_x - pan_x_) / zoom_;
    const float graph_y = (local_y - pan_y_) / zoom_;
    pan_x_ = local_x - graph_x * next;
    pan_y_ = local_y - graph_y * next;
    zoom_ = next;
  };

  ctx.push_clip(bounds);
  ctx.panel(bounds, scale(ctx.theme().background, 0.86F));
  ctx.panel(toolbar, ctx.theme().background);
  ctx.panel(gui::Rect{toolbar.x, toolbar.y + toolbar.height - 1.0F,
                      toolbar.width, 1.0F},
            ctx.theme().divider);
  const std::size_t output_count = static_cast<std::size_t>(std::count_if(
      compositor_graph.nodes.begin(), compositor_graph.nodes.end(),
      [](const compositor::GraphNode &node) noexcept {
        return node.kind == compositor::NodeKind::Output;
      }));
  // Toolbar buttons are as wide as their words, so none is cut short.
  const float button_height = std::max(toolbar.height - 6.0F, 0.0F);
  const auto button_width = [&ctx](std::string_view label, float minimum) {
    return std::max(minimum, std::ceil(ctx.font_width(label)) + 14.0F);
  };
  const std::string_view add_label =
      add_menu_open_ ? "ADD NODE  -" : "ADD NODE  +";
  const gui::Rect add_button{toolbar.x + 8.0F, toolbar.y + 3.0F,
                             button_width("ADD NODE  +", 94.0F), button_height};
  const gui::Interaction add_interaction =
      clipped_grab(ctx, mix(id_, "compositor.add_menu"), add_button, toolbar);
  ctx.panel(add_button, add_interaction.held ? ctx.theme().widget_active
                                             : (add_interaction.hovered
                                                    ? ctx.theme().widget_hover
                                                    : ctx.theme().widget));
  ctx.label(gui::Rect{add_button.x + 7.0F, add_button.y,
                      std::max(add_button.width - 7.0F, 0.0F),
                      add_button.height},
            add_label,
            add_interaction.hovered ? ctx.theme().text : ctx.theme().accent);
  if (add_interaction.clicked) {
    add_menu_open_ = !add_menu_open_;
    if (!add_menu_open_) {
      add_category_.reset();
      ctx.clear_focus();
    }
  }

  if (toolbar.width >= 520.0F) {
    const float navigation_x = add_button.x + add_button.width + 8.0F;
    const gui::Rect zoom_out{navigation_x, toolbar.y + 3.0F,
                             button_width("-", 22.0F), button_height};
    const gui::Rect zoom_value{zoom_out.x + zoom_out.width + 2.0F,
                               toolbar.y + 3.0F, button_width("250%", 48.0F),
                               button_height};
    const gui::Rect zoom_in{zoom_value.x + zoom_value.width + 2.0F,
                            toolbar.y + 3.0F, button_width("+", 22.0F),
                            button_height};
    const gui::Rect reset_view{zoom_in.x + zoom_in.width + 4.0F,
                               toolbar.y + 3.0F, button_width("RESET", 54.0F),
                               button_height};
    const float anchor_x = canvas.x + canvas.width * 0.5F;
    const float anchor_y = canvas.y + canvas.height * 0.5F;
    const auto navigation_button = [&](std::string_view salt,
                                       const gui::Rect &button,
                                       std::string_view label) {
      const gui::Interaction interaction =
          clipped_grab(ctx, mix(id_, salt), button, toolbar);
      ctx.panel(button, interaction.held
                            ? ctx.theme().widget_active
                            : (interaction.hovered ? ctx.theme().widget_hover
                                                   : ctx.theme().widget));
      const float label_inset =
          std::max((button.width - ctx.font_width(label)) * 0.5F, 0.0F);
      ctx.label(gui::Rect{button.x + label_inset, button.y,
                          std::max(button.width - label_inset, 0.0F),
                          button.height},
                label,
                interaction.hovered ? ctx.theme().text
                                    : ctx.theme().text_muted);
      return interaction.clicked;
    };
    if (navigation_button("compositor.zoom_out", zoom_out, "-")) {
      set_zoom(zoom_ / compositor_zoom_step, anchor_x, anchor_y);
    }
    std::array<char, 16U> zoom_text{};
    const int zoom_length =
        std::snprintf(zoom_text.data(), zoom_text.size(), "%.0f%%",
                      static_cast<double>(zoom_ * 100.0F));
    if (navigation_button("compositor.zoom_reset", zoom_value,
                          std::string_view{zoom_text.data(),
                                           static_cast<std::size_t>(
                                               std::max(zoom_length, 0))})) {
      set_zoom(1.0F, anchor_x, anchor_y);
    }
    if (navigation_button("compositor.zoom_in", zoom_in, "+")) {
      set_zoom(zoom_ * compositor_zoom_step, anchor_x, anchor_y);
    }
    if (navigation_button("compositor.view_reset", reset_view, "RESET")) {
      zoom_ = 1.0F;
      pan_x_ = 0.0F;
      pan_y_ = 0.0F;
    }
    // The navigation hint only where it cannot run into the graph status
    // and selection actions at the toolbar's other end.
    constexpr std::string_view hint = "WHEEL ZOOM  /  MIDDLE-DRAG PAN";
    if (toolbar.width >= 760.0F) {
      ctx.label(gui::Rect{reset_view.x + reset_view.width + 10.0F, toolbar.y,
                          std::ceil(ctx.font_width(hint)) + 2.0F,
                          toolbar.height},
                hint, ctx.theme().text_muted);
    }
  }

  const auto palette_matches = [&](const CompositorPaletteItem &item) {
    const std::string_view category =
        compositor_category_names[category_index(item.category)];
    return matches(compositor::node_kind_name(item.kind), node_search_) ||
           matches(category, node_search_) ||
           matches(item.search_terms, node_search_);
  };
  const auto palette_direct_matches = [&](const CompositorPaletteItem &item) {
    return matches(compositor::node_kind_name(item.kind), node_search_) ||
           matches(item.search_terms, node_search_);
  };
  std::size_t palette_item_rows{};
  if (!node_search_.empty()) {
    palette_item_rows = static_cast<std::size_t>(
        std::count_if(compositor_palette_items.begin(),
                      compositor_palette_items.end(), palette_matches));
  } else if (add_category_.has_value()) {
    palette_item_rows = static_cast<std::size_t>(std::count_if(
        compositor_palette_items.begin(), compositor_palette_items.end(),
        [&](const CompositorPaletteItem &item) {
          return category_index(item.category) == *add_category_;
        }));
  }
  constexpr float palette_search_height = 31.0F;
  constexpr float palette_row_height = 23.0F;
  const std::size_t palette_category_rows =
      node_search_.empty() ? compositor_category_names.size() : 0U;
  const float desired_palette_height =
      8.0F + palette_search_height +
      static_cast<float>(palette_category_rows + palette_item_rows) *
          palette_row_height;
  const float palette_width =
      std::min(292.0F, std::max(bounds.width - 16.0F, 0.0F));
  const gui::Rect palette_bounds{
      toolbar.x + 8.0F, content.y, palette_width,
      std::min(desired_palette_height,
               std::max(bounds.height - toolbar.height, 0.0F))};
  if (add_menu_open_ && ctx.pointer_pressed() &&
      !add_button.contains(ctx.pointer_x(), ctx.pointer_y()) &&
      !palette_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
    add_menu_open_ = false;
    add_category_.reset();
    ctx.clear_focus();
  }

  std::array<char, 64U> graph_status{};
  const bool invalid_output_count = output_count > 1U;
  const std::size_t error_count = static_cast<std::size_t>(std::count_if(
      compositor_graph.diagnostics.begin(), compositor_graph.diagnostics.end(),
      [](const compositor::GraphDiagnostic &diagnostic) noexcept {
        return diagnostic.severity == compositor::DiagnosticSeverity::Error;
      }));
  const std::size_t warning_count =
      compositor_graph.diagnostics.size() - error_count;
  int graph_status_length{};
  if (invalid_output_count) {
    graph_status_length =
        std::snprintf(graph_status.data(), graph_status.size(), "%llu OUTPUTS",
                      static_cast<unsigned long long>(output_count));
  } else if (compositor_graph.diagnostics.empty()) {
    graph_status_length = std::snprintf(
        graph_status.data(), graph_status.size(), "%llu NODES",
        static_cast<unsigned long long>(compositor_graph.nodes.size()));
  } else if (error_count != 0U && warning_count != 0U) {
    graph_status_length = std::snprintf(
        graph_status.data(), graph_status.size(), "%llu ERRORS / %llu WARNINGS",
        static_cast<unsigned long long>(error_count),
        static_cast<unsigned long long>(warning_count));
  } else if (error_count != 0U) {
    graph_status_length =
        std::snprintf(graph_status.data(), graph_status.size(), "%llu ERRORS",
                      static_cast<unsigned long long>(error_count));
  } else {
    graph_status_length =
        std::snprintf(graph_status.data(), graph_status.size(), "%llu WARNINGS",
                      static_cast<unsigned long long>(warning_count));
  }
  const std::string_view graph_status_text{
      graph_status.data(),
      static_cast<std::size_t>(std::max(graph_status_length, 0))};
  const float status_width = ctx.font_width(graph_status_text);
  const bool has_selection = selection_.kind != CompositorSelectionKind::None;
  const compositor::GraphNode *const selected_node =
      selection_.kind == CompositorSelectionKind::Node
          ? compositor_graph.node(selection_.node)
          : nullptr;
  const bool selected_required_output =
      selected_node != nullptr &&
      selected_node->kind == compositor::NodeKind::Output;
  const std::string_view remove_label =
      selection_.kind == CompositorSelectionKind::Edge
          ? std::string_view{"DISCONNECT"}
          : (selected_required_output ? std::string_view{"OUTPUT REQUIRED"}
                                      : std::string_view{"DELETE NODE"});
  const float remove_width = button_width(remove_label, 86.0F);
  const float trailing_control_width =
      has_selection ? remove_width + 12.0F : 0.0F;
  ctx.label(gui::Rect{toolbar.x + toolbar.width - status_width - 8.0F -
                          trailing_control_width,
                      toolbar.y, status_width, toolbar.height},
            graph_status_text,
            !invalid_output_count && compositor_graph.diagnostics.empty()
                ? ctx.theme().text_muted
                : ctx.theme().warning);
  if (has_selection) {
    const gui::Rect remove_bounds{
        toolbar.x + toolbar.width - remove_width - 8.0F, toolbar.y + 3.0F,
        remove_width, button_height};
    gui::Interaction interaction{};
    if (!selected_required_output) {
      interaction = clipped_grab(ctx, mix(id_, "compositor.remove"),
                                 remove_bounds, toolbar);
    }
    ctx.panel(remove_bounds,
              interaction.held ? ctx.theme().widget_active
                               : (interaction.hovered ? ctx.theme().widget_hover
                                                      : ctx.theme().widget));
    border(ctx, remove_bounds,
           interaction.hovered ? ctx.theme().warning : ctx.theme().divider);
    ctx.label(gui::Rect{remove_bounds.x + 7.0F, remove_bounds.y,
                        std::max(remove_bounds.width - 7.0F, 0.0F),
                        remove_bounds.height},
              remove_label,
              interaction.hovered ? ctx.theme().text : ctx.theme().text_muted);
    if (interaction.clicked && !selected_required_output) {
      frame.remove = CompositorDeleteIntent{selection_};
    }
  }
  if (focused_ && has_selection && !ctx.typing() &&
      ctx.key_pressed(gui::Key::Delete)) {
    if (!selected_required_output) {
      frame.remove = CompositorDeleteIntent{selection_};
    }
    frame.consume_delete = true;
  }

  const bool palette_pointer_blocked =
      add_menu_open_ &&
      palette_bounds.contains(ctx.pointer_x(), ctx.pointer_y());
  if (palette_pointer_blocked) {
    ctx.push_pointer_block();
  }

  if (canvas.width <= 0.0F || canvas.height <= 0.0F) {
    if (palette_pointer_blocked) {
      ctx.pop_pointer_block();
    }
    ctx.pop_clip();
    return frame;
  }

  if (canvas.contains(ctx.pointer_x(), ctx.pointer_y()) &&
      ctx.wheel() != 0.0F && dragging_node_ == doc::NodeId{} &&
      !connecting_from_.has_value()) {
    set_zoom(zoom_ * std::pow(compositor_zoom_step, ctx.wheel()),
             ctx.pointer_x(), ctx.pointer_y());
  }
  if (ctx.middle_pointer_pressed() &&
      canvas.contains(ctx.pointer_x(), ctx.pointer_y())) {
    panning_ = true;
  }
  if (panning_ && ctx.middle_pointer_down()) {
    pan_x_ += ctx.pointer_dx();
    pan_y_ += ctx.pointer_dy();
  }
  if (ctx.middle_pointer_released()) {
    panning_ = false;
  }

  std::vector<CompositorNodeVisual> nodes;
  std::vector<CompositorPortVisual> inputs;
  std::vector<CompositorPortVisual> outputs;
  nodes.reserve(compositor_graph.nodes.size());
  inputs.reserve(compositor_graph.nodes.size() * 2U);
  outputs.reserve(compositor_graph.nodes.size());

  const auto build_visuals = [&]() {
    nodes.clear();
    inputs.clear();
    outputs.clear();
    for (const compositor::GraphNode &node : compositor_graph.nodes) {
      float editor_x = node.editor_x;
      float editor_y = node.editor_y;
      if (dragging_node_ == node.node) {
        editor_x = drag_x_;
        editor_y = drag_y_;
      } else if (pending_move_.has_value() && pending_move_->node == node.node) {
        editor_x = pending_move_->x;
        editor_y = pending_move_->y;
      }
      const gui::UiPoint size = node_size(node);
      const gui::Rect node_bounds{canvas.x + pan_x_ + editor_x * zoom_,
                                  canvas.y + pan_y_ + editor_y * zoom_,
                                  size.x * zoom_, size.y * zoom_};
      nodes.push_back(CompositorNodeVisual{std::addressof(node), node_bounds});

      // A port's target is its half of the row - dot and name - reaching a
      // little past the node edge so the dot itself is inside it too.
      const float reach = compositor_port_radius * zoom_ + 4.0F;
      const auto append_ports =
          [&](std::span<const compositor::PortDescriptor> descriptors,
              std::vector<CompositorPortVisual> &destination, bool input) {
            for (std::size_t index = 0U; index < descriptors.size(); ++index) {
              const compositor::PortDescriptor &descriptor = descriptors[index];
              const float row_top =
                  node_bounds.y +
                  (metrics_.header_height +
                   metrics_.row_height * static_cast<float>(index)) *
                      zoom_;
              const float row_height = metrics_.row_height * zoom_;
              const float x =
                  input ? node_bounds.x : node_bounds.x + node_bounds.width;
              const gui::Rect hit =
                  input ? gui::Rect{node_bounds.x - reach, row_top,
                                    node_bounds.width * 0.5F + reach, row_height}
                        : gui::Rect{node_bounds.x + node_bounds.width * 0.5F,
                                    row_top, node_bounds.width * 0.5F + reach,
                                    row_height};
              destination.push_back(CompositorPortVisual{
                  compositor::EdgeEndpoint{node.node, descriptor.index},
                  std::addressof(descriptor), x, row_top + row_height * 0.5F,
                  hit});
            }
          };
      append_ports(compositor::input_ports(node.kind), inputs, true);
      append_ports(compositor::output_ports(node.kind), outputs, false);
    }
  };
  build_visuals();

  // While a wire is being drawn, the input it would connect to: the
  // compatible port whose row is under the pointer, or failing that the
  // nearest compatible dot within snapping reach.
  const auto snapped_input = [&]() -> const CompositorPortVisual * {
    if (!connecting_from_.has_value() ||
        !canvas.contains(ctx.pointer_x(), ctx.pointer_y())) {
      return nullptr;
    }
    const CompositorPortVisual *const from =
        find_port(outputs, *connecting_from_);
    if (from == nullptr) {
      return nullptr;
    }
    const CompositorPortVisual *best = nullptr;
    float best_rank = std::numeric_limits<float>::infinity();
    for (const CompositorPortVisual &to : inputs) {
      if (!can_connect(compositor_graph, *from, to)) {
        continue;
      }
      const float distance =
          std::hypot(ctx.pointer_x() - to.x, ctx.pointer_y() - to.y);
      const bool over_row = to.hit.contains(ctx.pointer_x(), ctx.pointer_y());
      if (!over_row && distance > compositor_port_snap_radius) {
        continue;
      }
      // A row under the pointer outranks any dot that is merely near.
      const float rank = over_row ? distance - 1.0e6F : distance;
      if (rank < best_rank) {
        best = std::addressof(to);
        best_rank = rank;
      }
    }
    return best;
  };

  bool selection_pressed = false;
  if (connecting_from_.has_value() && ctx.pointer_released()) {
    const CompositorPortVisual *const from =
        find_port(outputs, *connecting_from_);
    const CompositorPortVisual *const to = snapped_input();
    if (from != nullptr && to != nullptr) {
      std::optional<doc::NodeId> replace_edge;
      if (to->descriptor != nullptr && !to->descriptor->multiple) {
        const auto occupied = std::find_if(
            compositor_graph.edges.begin(), compositor_graph.edges.end(),
            [to](const compositor::GraphEdge &edge) noexcept {
              return edge.to == to->endpoint;
            });
        if (occupied != compositor_graph.edges.end()) {
          replace_edge = occupied->node;
        }
      }
      frame.connect_ports =
          CompositorConnectIntent{from->endpoint, to->endpoint, replace_edge};
    }
    connecting_from_.reset();
  }

  bool layout_changed = false;
  bool drag_released = false;
  for (const CompositorPortVisual &port : outputs) {
    const gui::Interaction interaction =
        clipped_grab(ctx,
                     mix(id_, "compositor.output_port",
                         port.endpoint.node.value() * 16U + port.endpoint.port),
                     port.hit, canvas);
    ctx.cursor_for(interaction, gui::Cursor::Hand);
    if (interaction.pressed) {
      connecting_from_ = port.endpoint;
      connecting_type_ = port.descriptor->type;
      selection_ = CompositorSelection{CompositorSelectionKind::Node,
                                       port.endpoint.node};
      frame.selection = selection_;
      selection_pressed = true;
    }
  }
  for (const CompositorNodeVisual &visual : nodes) {
    const gui::Interaction interaction = clipped_grab(
        ctx, mix(id_, "compositor.node", visual.node->node.value()),
        visual.bounds, canvas);
    if (interaction.pressed) {
      selection_ =
          CompositorSelection{CompositorSelectionKind::Node, visual.node->node};
      frame.selection = selection_;
      selection_pressed = true;
      dragging_node_ = visual.node->node;
      drag_x_ = (visual.bounds.x - canvas.x - pan_x_) / zoom_;
      drag_y_ = (visual.bounds.y - canvas.y - pan_y_) / zoom_;
      drag_offset_x_ = ctx.pointer_x() - visual.bounds.x;
      drag_offset_y_ = ctx.pointer_y() - visual.bounds.y;
    }
    if (interaction.held && dragging_node_ == visual.node->node) {
      ctx.set_cursor(gui::Cursor::Move);
      const float next_x =
          (ctx.pointer_x() - canvas.x - pan_x_ - drag_offset_x_) / zoom_;
      const float next_y =
          (ctx.pointer_y() - canvas.y - pan_y_ - drag_offset_y_) / zoom_;
      layout_changed = layout_changed || next_x != drag_x_ || next_y != drag_y_;
      drag_x_ = next_x;
      drag_y_ = next_y;
      if (interaction.released) {
        if (drag_x_ != visual.node->editor_x ||
            drag_y_ != visual.node->editor_y) {
          frame.move_node =
              CompositorMoveIntent{visual.node->node, drag_x_, drag_y_};
          pending_move_ =
              PendingMove{visual.node->node, drag_x_, drag_y_,
                          visual.node->editor_x, visual.node->editor_y, 0U};
        }
        drag_released = true;
      }
    }
  }
  if (layout_changed) {
    build_visuals();
  }

  // Each wire's curve, once, for both picking and drawing.
  struct EdgeVisual final {
    const compositor::GraphEdge *edge{};
    const CompositorPortVisual *from{};
    const CompositorPortVisual *to{};
    WirePath path;
  };
  std::vector<EdgeVisual> edges;
  edges.reserve(compositor_graph.edges.size());
  for (const compositor::GraphEdge &edge : compositor_graph.edges) {
    const CompositorPortVisual *const from = find_port(outputs, edge.from);
    const CompositorPortVisual *const to = find_port(inputs, edge.to);
    if (from == nullptr || to == nullptr || from->descriptor == nullptr ||
        to->descriptor == nullptr ||
        from->descriptor->type != to->descriptor->type) {
      continue;
    }
    edges.push_back(EdgeVisual{std::addressof(edge), from, to,
                               wire_path(from->x, from->y, to->x, to->y,
                                         zoom_)});
  }
  // The wire nearest the pointer, within reach and not under a node, which
  // draws over it. Every wire still takes part in the interaction each
  // frame, so the one that was pressed always sees its release.
  const EdgeVisual *hovered_edge = nullptr;
  const bool pointer_over_node = std::any_of(
      nodes.begin(), nodes.end(), [&ctx](const CompositorNodeVisual &visual) {
        return visual.bounds.contains(ctx.pointer_x(), ctx.pointer_y());
      });
  if (!connecting_from_.has_value() && dragging_node_ == doc::NodeId{} &&
      !pointer_over_node && canvas.contains(ctx.pointer_x(), ctx.pointer_y())) {
    float nearest = compositor_wire_hit_radius;
    for (const EdgeVisual &visual : edges) {
      const float distance =
          wire_distance(visual.path, ctx.pointer_x(), ctx.pointer_y());
      if (distance <= nearest) {
        nearest = distance;
        hovered_edge = std::addressof(visual);
      }
    }
  }
  for (const EdgeVisual &visual : edges) {
    const bool hovered = std::addressof(visual) == hovered_edge;
    const gui::Rect pick =
        hovered ? gui::Rect{ctx.pointer_x() - 1.0F, ctx.pointer_y() - 1.0F,
                            2.0F, 2.0F}
                : gui::Rect{};
    const gui::Interaction interaction = ctx.grab(
        mix(id_, "compositor.edge", visual.edge->node.value()), pick);
    ctx.cursor_for(interaction, gui::Cursor::Hand);
    if (interaction.pressed) {
      selection_ =
          CompositorSelection{CompositorSelectionKind::Edge, visual.edge->node};
      frame.selection = selection_;
      selection_pressed = true;
    }
  }
  if (ctx.pointer_pressed() &&
      canvas.contains(ctx.pointer_x(), ctx.pointer_y()) && !selection_pressed &&
      !parameter_resize_pressed) {
    selection_ = CompositorSelection{};
    frame.selection = selection_;
  }

  ctx.push_clip(canvas);
  ctx.panel(canvas, scale(ctx.theme().background, 0.84F));
  // A quiet drafting-grid texture identifies this as a spatial graph surface
  // without competing with wires or implying rigid row/column alignment.
  const float dot_spacing = 32.0F * zoom_;
  constexpr float dot_size = 1.5F;
  const gui::UiColor dot_color = with_alpha(ctx.theme().divider, 0.42F);
  float first_dot_x =
      canvas.x + std::fmod(pan_x_ + dot_spacing * 0.5F, dot_spacing);
  float first_dot_y =
      canvas.y + std::fmod(pan_y_ + dot_spacing * 0.5F, dot_spacing);
  if (first_dot_x < canvas.x) {
    first_dot_x += dot_spacing;
  }
  if (first_dot_y < canvas.y) {
    first_dot_y += dot_spacing;
  }
  for (float y = first_dot_y; y < canvas.y + canvas.height; y += dot_spacing) {
    for (float x = first_dot_x; x < canvas.x + canvas.width; x += dot_spacing) {
      ctx.draw_list().add_rect(x - dot_size * 0.5F, y - dot_size * 0.5F,
                               dot_size, dot_size, dot_color);
    }
  }

  // Guidance sits behind graph content: it remains available in empty/error
  // states, while nodes and wires always paint over it and therefore never
  // become obscured by instructional chrome.
  if (compositor_graph.nodes.empty() || !compositor_graph.diagnostics.empty()) {
    const bool diagnostic = !compositor_graph.diagnostics.empty();
    const gui::Rect guide{
        canvas.x + 8.0F, canvas.y + 8.0F,
        std::min(std::max(canvas.width - 16.0F, 0.0F), 660.0F), 28.0F};
    ctx.panel(guide, with_alpha(scale(ctx.theme().header, 0.92F), 0.94F));
    border(ctx, guide,
           diagnostic ? with_alpha(ctx.theme().warning, 0.72F)
                      : with_alpha(ctx.theme().accent, 0.42F));
    std::string title;
    if (diagnostic) {
      title = compositor_graph.diagnostics.front().severity ==
                      compositor::DiagnosticSeverity::Error
                  ? "GRAPH ERROR: "
                  : "GRAPH WARNING: ";
    } else {
      title = "START: ";
    }
    title += diagnostic ? compositor_diagnostic_message(
                              compositor_graph.diagnostics.front())
                        : std::string_view{"Add a Layer for each scene object"};
    ctx.label(gui::Rect{guide.x + 8.0F, guide.y + 3.0F,
                        std::max(guide.width - 16.0F, 0.0F), 22.0F},
              title, diagnostic ? ctx.theme().warning : ctx.theme().text_muted);
    ctx.tooltip(guide, title + " · Connect the output on the right of a node to an input on the left. Layers feed Composite, then Output.");
  }

  // Wires scale with the graph but never thin below what reads on screen.
  const float wire_width = std::clamp(2.0F * zoom_, 1.5F, 3.5F);
  const bool node_selected = selection_.kind == CompositorSelectionKind::Node;
  for (const EdgeVisual &visual : edges) {
    const bool selected =
        selection_ ==
        CompositorSelection{CompositorSelectionKind::Edge, visual.edge->node};
    // A selected node's own wires stand out from the rest of the graph.
    const bool attached = node_selected &&
                          (visual.edge->from.node == selection_.node ||
                           visual.edge->to.node == selection_.node);
    const bool hovered = std::addressof(visual) == hovered_edge;
    const gui::UiColor color = port_color(ctx, visual.from->descriptor->type);
    gui::UiColor stroke = with_alpha(color, 0.78F);
    float width = wire_width;
    if (selected) {
      stroke = ctx.theme().text;
      width = wire_width + 1.5F;
    } else if (hovered || attached) {
      stroke = blend_color(color, ctx.theme().text, hovered ? 0.45F : 0.2F);
      width = wire_width + (hovered ? 1.0F : 0.5F);
    }
    ctx.draw_list().add_polyline(visual.path.view(), width, stroke);
  }
  const CompositorPortVisual *const snapped = snapped_input();
  if (connecting_from_.has_value()) {
    const CompositorPortVisual *const from =
        find_port(outputs, *connecting_from_);
    if (from != nullptr) {
      // The wire lands on the port it would connect to, so the operator sees
      // the result before letting go.
      const float end_x = snapped != nullptr ? snapped->x : ctx.pointer_x();
      const float end_y = snapped != nullptr ? snapped->y : ctx.pointer_y();
      wire(ctx, from->x, from->y, end_x, end_y,
           port_color(ctx, connecting_type_), wire_width + 0.5F, zoom_);
    }
  }

  const auto input_connected = [&](compositor::EdgeEndpoint endpoint) {
    return std::any_of(compositor_graph.edges.begin(),
                       compositor_graph.edges.end(),
                       [endpoint](const compositor::GraphEdge &edge) noexcept {
                         return edge.to == endpoint;
                       });
  };
  const auto output_connected = [&](compositor::EdgeEndpoint endpoint) {
    return std::any_of(compositor_graph.edges.begin(),
                       compositor_graph.edges.end(),
                       [endpoint](const compositor::GraphEdge &edge) noexcept {
                         return edge.from == endpoint;
                       });
  };
  const CompositorPortVisual *const connecting_source =
      connecting_from_.has_value() ? find_port(outputs, *connecting_from_)
                                   : nullptr;
  const float node_title_height =
      compositor_text_height(ctx, compositor_title_text) * zoom_;
  const float node_detail_height =
      compositor_text_height(ctx, compositor_detail_text) * zoom_;
  const float port_radius = std::max(2.5F, compositor_port_radius * zoom_);

  for (const CompositorNodeVisual &visual : nodes) {
    const bool selected =
        selection_ ==
        CompositorSelection{CompositorSelectionKind::Node, visual.node->node};
    const bool diagnostic =
        node_has_diagnostic(compositor_graph, visual.node->node);
    const gui::UiColor kind = compositor_node_color(ctx, visual.node->kind);
    const gui::UiColor body = scale(ctx.theme().panel, 1.18F);

    // A soft drop shadow lifts the node off the grid.
    const float shadow = std::max(2.0F, 3.0F * zoom_);
    ctx.panel(gui::Rect{visual.bounds.x + shadow * 0.5F,
                        visual.bounds.y + shadow, visual.bounds.width,
                        visual.bounds.height},
              with_alpha(ctx.theme().background, 0.55F));
    ctx.panel(visual.bounds, body);

    const gui::Rect node_header{
        visual.bounds.x, visual.bounds.y, visual.bounds.width,
        std::min(metrics_.header_height * zoom_, visual.bounds.height)};
    ctx.panel(node_header, blend_color(ctx.theme().header, kind, 0.16F));
    ctx.panel(gui::Rect{node_header.x, node_header.y + node_header.height -
                                           std::max(1.0F, 2.0F * zoom_),
                        node_header.width, std::max(1.0F, 2.0F * zoom_)},
              with_alpha(kind, 0.85F));
    border(ctx, visual.bounds,
           diagnostic ? ctx.theme().warning
                      : (selected ? ctx.theme().accent : ctx.theme().border),
           selected || diagnostic ? std::max(1.0F, std::min(2.0F, 2.0F * zoom_))
                                  : 1.0F);

    const std::string_view title =
        visual.node->label.empty()
            ? compositor::node_kind_name(visual.node->kind)
            : std::string_view{visual.node->label};
    const float text_x = visual.bounds.x + compositor_node_padding * zoom_;
    const float text_width =
        std::max(visual.bounds.width - compositor_node_padding * 2.0F * zoom_,
                 0.0F);
    const gui::Rect title_bounds{text_x, visual.bounds.y + 5.0F * zoom_,
                                 text_width, node_title_height};
    if (compositor_label(ctx, title_bounds, title, ctx.theme().text,
                         compositor_title_text, zoom_)) {
      const std::string detail = compositor_node_detail(*visual.node, elements);
      static_cast<void>(compositor_label(
          ctx,
          gui::Rect{text_x, title_bounds.y + node_title_height, text_width,
                    node_detail_height},
          detail, ctx.theme().text_muted, compositor_detail_text, zoom_));
    }

    // Names share a row: an input on the left, an output on the right. Each
    // gets the room the other leaves, so the two never overlap.
    const auto port_label_width = [&](const CompositorPortVisual *port) {
      if (port == nullptr) {
        return 0.0F;
      }
      const std::optional<float> baked =
          ctx.text_size_at_most(compositor_pin_text * zoom_);
      return baked.has_value() ? ctx.font_width(port->descriptor->name, *baked)
                               : 0.0F;
    };
    const auto row_port = [&](const std::vector<CompositorPortVisual> &ports,
                              std::size_t row) -> const CompositorPortVisual * {
      std::size_t seen = 0U;
      for (const CompositorPortVisual &port : ports) {
        if (port.endpoint.node == visual.node->node && seen++ == row) {
          return std::addressof(port);
        }
      }
      return nullptr;
    };
    const float pin_inset = compositor_pin_inset * zoom_;
    const float gap = compositor_pin_label_gap * zoom_;
    const std::size_t rows =
        std::max(compositor::input_ports(visual.node->kind).size(),
                 compositor::output_ports(visual.node->kind).size());
    for (std::size_t row = 0U; row < rows; ++row) {
      const CompositorPortVisual *const input = row_port(inputs, row);
      const CompositorPortVisual *const output = row_port(outputs, row);
      const float row_height = metrics_.row_height * zoom_;
      const float input_width = port_label_width(input);
      const float output_width = port_label_width(output);
      const float right = visual.bounds.x + visual.bounds.width - pin_inset;
      if (input != nullptr) {
        const bool compatible = connecting_source != nullptr &&
                                can_connect(compositor_graph,
                                            *connecting_source, *input);
        const float limit =
            output != nullptr ? right - output_width - gap : right;
        static_cast<void>(compositor_label(
            ctx,
            gui::Rect{visual.bounds.x + pin_inset, input->y - row_height * 0.5F,
                      std::max(limit - visual.bounds.x - pin_inset, 0.0F),
                      row_height},
            input->descriptor->name,
            compatible ? ctx.theme().text : ctx.theme().text_muted,
            compositor_pin_text, zoom_));
      }
      if (output != nullptr) {
        const float left =
            input != nullptr ? visual.bounds.x + pin_inset + input_width + gap
                             : visual.bounds.x + pin_inset;
        const float width = std::min(output_width, std::max(right - left, 0.0F));
        static_cast<void>(compositor_label(
            ctx,
            gui::Rect{right - width, output->y - row_height * 0.5F, width,
                      row_height},
            output->descriptor->name, ctx.theme().text_muted,
            compositor_pin_text, zoom_));
      }
    }

    // Dots over names: a filled shape is connected, a ring is open, and an
    // open required input is drawn as the problem it is. While a wire is
    // being drawn, ports it cannot reach fade back.
    for (const CompositorPortVisual &port : inputs) {
      if (port.endpoint.node != visual.node->node) {
        continue;
      }
      const bool connected = input_connected(port.endpoint);
      const bool compatible =
          connecting_source != nullptr &&
          can_connect(compositor_graph, *connecting_source, port);
      const bool target = snapped == std::addressof(port);
      gui::UiColor color = port_color(ctx, port.descriptor->type);
      if (!connected && port.descriptor->required) {
        color = ctx.theme().warning;
      }
      if (connecting_source != nullptr && !compatible) {
        color = with_alpha(color, 0.3F);
      }
      const float radius =
          target ? port_radius + 2.0F
                 : (compatible ? port_radius + 1.0F : port_radius);
      if (target) {
        draw_port_shape(ctx, port.descriptor->type, port.x, port.y,
                        radius + 1.5F, ctx.theme().text);
      }
      draw_port(ctx, port.descriptor->type, port.x, port.y, radius, color,
                connected || target, body);
    }
    for (const CompositorPortVisual &port : outputs) {
      if (port.endpoint.node != visual.node->node) {
        continue;
      }
      const bool hovered = port.hit.contains(ctx.pointer_x(), ctx.pointer_y()) &&
                           !connecting_from_.has_value();
      const bool active = connecting_from_ == port.endpoint;
      const gui::UiColor color = port_color(ctx, port.descriptor->type);
      const float radius =
          hovered || active ? port_radius + 1.0F : port_radius;
      if (hovered || active) {
        draw_port_shape(ctx, port.descriptor->type, port.x, port.y,
                        radius + 1.5F, ctx.theme().text);
      }
      draw_port(ctx, port.descriptor->type, port.x, port.y, radius,
                connecting_source != nullptr && !active
                    ? with_alpha(color, 0.3F)
                    : color,
                output_connected(port.endpoint) || active, body);
    }
  }
  ctx.pop_clip();

  if (parameter_node != nullptr && parameter_bounds.width > 0.0F &&
      parameter_bounds.height > 0.0F) {
    ctx.push_clip(parameter_bounds);
    ctx.panel(parameter_bounds, scale(ctx.theme().panel, 0.98F));
    border(ctx, parameter_bounds, ctx.theme().divider);
    ctx.panel(gui::Rect{parameter_bounds.x, parameter_bounds.y,
                        parameter_bounds.width, 3.0F},
              compositor_node_color(ctx, parameter_node->kind));
    // The heading's lines are as tall as the font says, never clipped.
    const float heading_height = ctx.line_height();
    const float subheading_height =
        compositor_text_height(ctx, compositor_detail_text);
    const float header_height =
        std::max(32.0F, std::ceil(3.0F + heading_height + subheading_height));
    ctx.label(gui::Rect{parameter_bounds.x + 8.0F, parameter_bounds.y + 2.0F,
                        std::max(parameter_bounds.width - 16.0F, 0.0F),
                        heading_height},
              "PARAMETERS", ctx.theme().text);
    static_cast<void>(compositor_label(
        ctx,
        gui::Rect{parameter_bounds.x + 8.0F,
                  parameter_bounds.y + 2.0F + heading_height,
                  std::max(parameter_bounds.width - 16.0F, 0.0F),
                  subheading_height},
        compositor::node_kind_name(parameter_node->kind), ctx.theme().text_muted,
        compositor_detail_text, 1.0F));

    // The fields scroll beneath the heading. Their height is last frame's,
    // which is exact for every frame but the first after a selection - and
    // that frame starts at the top anyway.
    constexpr float row_height = 22.0F;
    constexpr float row_stride = 26.0F;
    constexpr float scrollbar_width = 8.0F;
    const gui::Rect parameter_body{
        parameter_bounds.x, parameter_bounds.y + header_height,
        parameter_bounds.width,
        std::max(parameter_bounds.height - header_height, 0.0F)};
    const float maximum_scroll =
        std::max(parameter_content_height_ - parameter_body.height, 0.0F);
    const bool scrollable = maximum_scroll > 0.0F;
    if (parameter_body.contains(ctx.pointer_x(), ctx.pointer_y())) {
      parameter_scroll_ -= ctx.wheel() * row_stride * 2.0F;
    }
    parameter_scroll_ = std::clamp(parameter_scroll_, 0.0F, maximum_scroll);
    const float content_x = parameter_body.x + 8.0F;
    const float content_width =
        std::max(parameter_body.width - 16.0F -
                     (scrollable ? scrollbar_width : 0.0F),
                 0.0F);
    const float content_top = parameter_body.y + 4.0F;
    float parameter_y = content_top - parameter_scroll_;

    // The label column fits this node's longest label. When the panel is
    // too narrow for that and a usable field, each label sits above its
    // field instead of being cut short.
    float widest_label{};
    for (const std::string_view label :
         compositor_parameter_labels(parameter_node->kind)) {
      widest_label = std::max(widest_label, ctx.font_width(label));
    }
    constexpr float minimum_field_width = 84.0F;
    const float inline_label_width = std::max(60.0F, widest_label + 12.0F);
    const bool stacked_labels =
        content_width - inline_label_width < minimum_field_width;
    const float label_width = stacked_labels ? 0.0F : inline_label_width;
    const float stacked_label_height = ctx.line_height() + 2.0F;
    ctx.push_clip(parameter_body);
    // One labelled field row; returns where the field goes.
    const auto next_field = [&](std::string_view label) {
      if (stacked_labels) {
        ctx.label(gui::Rect{content_x, parameter_y, content_width,
                            stacked_label_height},
                  label, ctx.theme().text_muted);
        parameter_y += stacked_label_height;
      }
      const gui::Rect row{content_x, parameter_y, content_width, row_height};
      parameter_y += row_stride;
      if (!stacked_labels) {
        ctx.label(gui::Rect{row.x, row.y, label_width, row.height}, label,
                  ctx.theme().text_muted);
      }
      return gui::Rect{row.x + label_width, row.y,
                       std::max(row.width - label_width, 0.0F), row.height};
    };
    // A row spanning the panel, for buttons and notes.
    const auto full_row = [&]() {
      const gui::Rect row{content_x, parameter_y, content_width, row_height};
      parameter_y += row_stride;
      return row;
    };
    // A collapsible heading; true while its fields are shown.
    const auto section = [&](std::string_view title, core::u32 bit) {
      const gui::Rect row = full_row();
      const core::u32 mask = core::u32{1U} << bit;
      const gui::Interaction interaction = ctx.grab(
          mix(id_, "compositor.parameters.section", bit), row);
      if (interaction.clicked) {
        collapsed_sections_ ^= mask;
      }
      const bool open = (collapsed_sections_ & mask) == 0U;
      ctx.panel(row, interaction.hovered ? ctx.theme().widget_hover
                                         : ctx.theme().header);
      draw_twirl(ctx, row.x + 9.0F, row.y + row.height * 0.5F, open,
                 interaction.hovered ? ctx.theme().accent
                                     : ctx.theme().text_muted);
      ctx.label(gui::Rect{row.x + 19.0F, row.y,
                          std::max(row.width - 23.0F, 0.0F), row.height},
                title,
                interaction.hovered ? ctx.theme().text : ctx.theme().text_muted);
      return open;
    };
    const auto field_id = [&](CompositorProperty property) noexcept {
      return mix(id_, "compositor.parameter",
                 parameter_node->node.value() * 16U +
                     static_cast<core::u64>(property));
    };
    const auto emit_float = [&](CompositorProperty property, double authored,
                                float current, float minimum, float maximum) {
      if (!std::isfinite(authored)) {
        return;
      }
      const float value =
          static_cast<float>(std::clamp(authored, static_cast<double>(minimum),
                                        static_cast<double>(maximum)));
      if (value != current) {
        frame.property_edit = CompositorPropertyEditIntent{
            parameter_node->node, property, CompositorPropertyValue{value}};
      }
    };
    const auto record_nudge = [&](CompositorProperty property, gui::UiId field,
                                  const NumberFieldEdit &edit) {
      if (edit.kind == NumberFieldEditKind::Nudge &&
          frame.property_edit.has_value() &&
          frame.property_edit->property == property) {
        frame.numeric_nudge =
            NumericNudge{field, static_cast<core::u64>(property) + 1U,
                         edit.direction, edit.modifiers};
      }
    };
    const auto scalar_field =
        [&](std::string_view label, CompositorProperty property, float value,
            double speed, float minimum, float maximum, int decimals) {
          const gui::Rect field = next_field(label);
          double authored = 0.0;
          NumberFieldEdit edit{};
          const gui::UiId numeric_id = field_id(property);
          if (parameter_fields_.draw(ctx, numeric_id, field, {},
                                     static_cast<double>(value), speed,
                                     authored, decimals, &edit)) {
            emit_float(property, authored, value, minimum, maximum);
            record_nudge(property, numeric_id, edit);
          }
        };
    const auto unsigned_field = [&](std::string_view label,
                                    CompositorProperty property,
                                    core::u32 value, double speed,
                                    core::u32 minimum, core::u32 maximum) {
      const gui::Rect field = next_field(label);
      double authored{};
      NumberFieldEdit edit{};
      const gui::UiId numeric_id = field_id(property);
      if (parameter_fields_.draw(ctx, numeric_id, field, {},
                                 static_cast<double>(value), speed, authored, 0,
                                 &edit) &&
          std::isfinite(authored)) {
        const core::u32 normalized = static_cast<core::u32>(
            std::clamp(std::round(authored), static_cast<double>(minimum),
                       static_cast<double>(maximum)));
        if (normalized != value) {
          frame.property_edit =
              CompositorPropertyEditIntent{parameter_node->node, property,
                                           CompositorPropertyValue{normalized}};
          record_nudge(property, numeric_id, edit);
        }
      }
    };

    switch (parameter_node->kind) {
    case compositor::NodeKind::Brightness: {
      const auto *parameters = std::get_if<compositor::BrightnessParameters>(
          &parameter_node->parameters);
      if (parameters != nullptr) {
        scalar_field("AMOUNT", CompositorProperty::BrightnessAmount,
                     parameters->amount, 0.01, 0.0F, compositor::max_brightness,
                     2);
      }
      break;
    }
    case compositor::NodeKind::Saturation: {
      const auto *parameters = std::get_if<compositor::SaturationParameters>(
          &parameter_node->parameters);
      if (parameters != nullptr) {
        scalar_field("AMOUNT", CompositorProperty::SaturationAmount,
                     parameters->amount, 0.01, 0.0F, compositor::max_saturation,
                     2);
      }
      break;
    }
    case compositor::NodeKind::Levels: {
      const auto *parameters = std::get_if<compositor::LevelsParameters>(
          &parameter_node->parameters);
      if (parameters == nullptr) {
        break;
      }
      using Property = CompositorProperty;
      // Each channel is its own section, so its labels need no prefix.
      const auto channel_fields =
          [&](std::string_view title, core::u32 section_bit,
              const scene::LevelsChannelParameters &channel,
              const std::array<Property, 5U> &properties) {
            if (!section(title, section_bit)) {
              return;
            }
            scalar_field("IN BLACK", properties[0U], channel.input_black,
                         0.005, 0.0F,
                         std::max(channel.input_white - 0.0001F, 0.0F), 3);
            scalar_field("IN WHITE", properties[1U], channel.input_white,
                         0.005, std::min(channel.input_black + 0.0001F, 1.0F),
                         1.0F, 3);
            scalar_field("GAMMA", properties[2U], channel.gamma, 0.01,
                         scene::min_levels_gamma, scene::max_levels_gamma, 3);
            scalar_field("OUT BLACK", properties[3U], channel.output_black,
                         0.005, 0.0F, 1.0F, 3);
            scalar_field("OUT WHITE", properties[4U], channel.output_white,
                         0.005, 0.0F, 1.0F, 3);
          };
      channel_fields(
          "MASTER", levels_master_section, parameters->master,
          {Property::LevelsMasterInputBlack, Property::LevelsMasterInputWhite,
           Property::LevelsMasterGamma, Property::LevelsMasterOutputBlack,
           Property::LevelsMasterOutputWhite});
      channel_fields("RED", levels_red_section, parameters->red,
                     {Property::LevelsRedInputBlack,
                      Property::LevelsRedInputWhite, Property::LevelsRedGamma,
                      Property::LevelsRedOutputBlack,
                      Property::LevelsRedOutputWhite});
      channel_fields(
          "GREEN", levels_green_section, parameters->green,
          {Property::LevelsGreenInputBlack, Property::LevelsGreenInputWhite,
           Property::LevelsGreenGamma, Property::LevelsGreenOutputBlack,
           Property::LevelsGreenOutputWhite});
      channel_fields("BLUE", levels_blue_section, parameters->blue,
                     {Property::LevelsBlueInputBlack,
                      Property::LevelsBlueInputWhite, Property::LevelsBlueGamma,
                      Property::LevelsBlueOutputBlack,
                      Property::LevelsBlueOutputWhite});
      break;
    }
    case compositor::NodeKind::LiftGammaGain: {
      const auto *parameters = std::get_if<compositor::LiftGammaGainParameters>(
          &parameter_node->parameters);
      if (parameters == nullptr) {
        break;
      }
      using Property = CompositorProperty;
      const auto channel_fields =
          [&scalar_field](std::string_view prefix, const scene::Vec3 &value,
                          const std::array<Property, 3U> &properties,
                          double speed, float minimum, float maximum) {
            const std::string red = std::string{prefix} + " R";
            const std::string green = std::string{prefix} + " G";
            const std::string blue = std::string{prefix} + " B";
            scalar_field(red, properties[0U], value.x, speed, minimum, maximum,
                         3);
            scalar_field(green, properties[1U], value.y, speed, minimum,
                         maximum, 3);
            scalar_field(blue, properties[2U], value.z, speed, minimum, maximum,
                         3);
          };
      channel_fields("LIFT", parameters->lift,
                     {Property::LiftGammaGainLiftR,
                      Property::LiftGammaGainLiftG,
                      Property::LiftGammaGainLiftB},
                     0.005, scene::min_lift_gamma_gain_lift,
                     scene::max_lift_gamma_gain_lift);
      channel_fields("GAMMA", parameters->gamma,
                     {Property::LiftGammaGainGammaR,
                      Property::LiftGammaGainGammaG,
                      Property::LiftGammaGainGammaB},
                     0.01, scene::min_lift_gamma_gain_gamma,
                     scene::max_lift_gamma_gain_gamma);
      channel_fields("GAIN", parameters->gain,
                     {Property::LiftGammaGainGainR,
                      Property::LiftGammaGainGainG,
                      Property::LiftGammaGainGainB},
                     0.01, scene::min_lift_gamma_gain_gain,
                     scene::max_lift_gamma_gain_gain);
      break;
    }
    case compositor::NodeKind::ContrastPivot: {
      const auto *parameters = std::get_if<compositor::ContrastPivotParameters>(
          &parameter_node->parameters);
      if (parameters != nullptr) {
        scalar_field("CONTRAST", CompositorProperty::ContrastPivotContrast,
                     parameters->contrast, 0.01,
                     scene::min_contrast_pivot_contrast,
                     scene::max_contrast_pivot_contrast, 3);
        scalar_field("PIVOT", CompositorProperty::ContrastPivotPivot,
                     parameters->pivot, 0.005, scene::min_contrast_pivot_pivot,
                     scene::max_contrast_pivot_pivot, 3);
      }
      break;
    }
    case compositor::NodeKind::ColorMatrix: {
      const auto *parameters = std::get_if<compositor::ColorMatrixParameters>(
          &parameter_node->parameters);
      if (parameters == nullptr) {
        break;
      }
      using Property = CompositorProperty;
      constexpr std::array<Property, 12U> properties{{
          Property::ColorMatrixM00,
          Property::ColorMatrixM01,
          Property::ColorMatrixM02,
          Property::ColorMatrixM10,
          Property::ColorMatrixM11,
          Property::ColorMatrixM12,
          Property::ColorMatrixM20,
          Property::ColorMatrixM21,
          Property::ColorMatrixM22,
          Property::ColorMatrixOffsetR,
          Property::ColorMatrixOffsetG,
          Property::ColorMatrixOffsetB,
      }};
      constexpr std::array<std::string_view, 12U> labels{{
          "RED FROM R",
          "RED FROM G",
          "RED FROM B",
          "GREEN FROM R",
          "GREEN FROM G",
          "GREEN FROM B",
          "BLUE FROM R",
          "BLUE FROM G",
          "BLUE FROM B",
          "RED OFFSET",
          "GREEN OFFSET",
          "BLUE OFFSET",
      }};
      const std::array<float, 12U> values{{
          parameters->matrix[0U],
          parameters->matrix[1U],
          parameters->matrix[2U],
          parameters->matrix[3U],
          parameters->matrix[4U],
          parameters->matrix[5U],
          parameters->matrix[6U],
          parameters->matrix[7U],
          parameters->matrix[8U],
          parameters->offset.x,
          parameters->offset.y,
          parameters->offset.z,
      }};
      for (std::size_t index = 0U; index < properties.size(); ++index) {
        scalar_field(labels[index], properties[index], values[index], 0.01,
                     scene::min_color_matrix_coefficient,
                     scene::max_color_matrix_coefficient, 3);
      }
      break;
    }
    case compositor::NodeKind::WhiteBalance: {
      const auto *parameters = std::get_if<compositor::WhiteBalanceParameters>(
          &parameter_node->parameters);
      if (parameters != nullptr) {
        scalar_field("TEMPERATURE K",
                     CompositorProperty::WhiteBalanceTemperatureKelvin,
                     parameters->temperature_kelvin, 25.0,
                     scene::min_white_balance_temperature_kelvin,
                     scene::max_white_balance_temperature_kelvin, 0);
        scalar_field("TINT", CompositorProperty::WhiteBalanceTint,
                     parameters->tint, 0.01, scene::min_white_balance_tint,
                     scene::max_white_balance_tint, 3);
      }
      break;
    }
    case compositor::NodeKind::Curves: {
      const auto *parameters = std::get_if<compositor::CurvesParameters>(
          &parameter_node->parameters);
      if (parameters == nullptr) {
        break;
      }
      const auto emit_curves = [&](compositor::CurvesParameters value) {
        const auto normalized = scene::normalized_curves(value);
        if (normalized.has_value() && *normalized != *parameters) {
          frame.property_edit = CompositorPropertyEditIntent{
              parameter_node->node, CompositorProperty::CurvesParameters,
              CompositorPropertyValue{*normalized}};
        }
      };
      const gui::Rect action_row = full_row();
      const float action_width = action_row.width * 0.5F;
      const gui::Rect reset{action_row.x, action_row.y, action_width,
                            action_row.height};
      const gui::Rect add{action_row.x + action_width, action_row.y,
                          action_width, action_row.height};
      const gui::Interaction reset_it = ctx.grab(
          mix(id_, "compositor.curves.reset", parameter_node->node.value()),
          reset);
      const gui::Interaction add_it = ctx.grab(
          mix(id_, "compositor.curves.add", parameter_node->node.value()), add);
      ctx.panel(reset, reset_it.hovered ? ctx.theme().widget_hover
                                        : ctx.theme().widget);
      ctx.panel(add,
                add_it.hovered ? ctx.theme().widget_hover : ctx.theme().widget);
      ctx.label(reset, "RESET",
                reset_it.hovered ? ctx.theme().accent : ctx.theme().text_muted);
      const bool can_add =
          parameters->master.points.size() < scene::max_curve_control_points;
      ctx.label(add, "+ MASTER POINT",
                add_it.hovered && can_add ? ctx.theme().accent
                                          : ctx.theme().text_muted);
      if (reset_it.clicked) {
        compositor::CurvesParameters reset_value{};
        if (const auto normalized = scene::normalized_curves(reset_value)) {
          emit_curves(*normalized);
        }
      }
      if (add_it.clicked && can_add && parameters->master.points.size() >= 2U) {
        compositor::CurvesParameters updated = *parameters;
        updated.lut.clear();
        const std::size_t endpoint = updated.master.points.size() - 1U;
        const scene::CurveControlPoint previous =
            updated.master.points[endpoint - 1U];
        updated.master.points[endpoint] = scene::CurveControlPoint{
            (previous.x + 1.0F) * 0.5F, (previous.y + 1.0F) * 0.5F};
        updated.master.points.push_back({1.0F, 1.0F});
        emit_curves(std::move(updated));
      }

      const std::string count =
          std::to_string(parameters->master.points.size()) +
          " master control points";
      ctx.label(full_row(), count, ctx.theme().text_muted);

      if (parameters->master.points.size() > 2U) {
        const std::size_t point = parameters->master.points.size() - 2U;
        const scene::CurveControlPoint current =
            parameters->master.points[point];
        const float minimum_x =
            parameters->master.points[point - 1U].x + 0.0001F;
        const float maximum_x =
            parameters->master.points[point + 1U].x - 0.0001F;
        const auto point_field = [&](std::string_view label, bool x_axis,
                                     float value, float minimum, float maximum,
                                     core::u64 salt) {
          const gui::Rect field = next_field(label);
          double authored{};
          NumberFieldEdit edit{};
          const gui::UiId numeric_id =
              mix(id_, "compositor.curves.point",
                  parameter_node->node.value() * 64U + salt);
          if (parameter_fields_.draw(ctx, numeric_id, field, {}, value, 0.005,
                                     authored, 3, &edit) &&
              std::isfinite(authored)) {
            const float normalized = static_cast<float>(
                std::clamp(authored, static_cast<double>(minimum),
                           static_cast<double>(maximum)));
            if (normalized != value) {
              compositor::CurvesParameters updated = *parameters;
              updated.lut.clear();
              if (x_axis) {
                updated.master.points[point].x = normalized;
              } else {
                updated.master.points[point].y = normalized;
              }
              emit_curves(std::move(updated));
              if (frame.property_edit.has_value() &&
                  edit.kind == NumberFieldEditKind::Nudge) {
                frame.numeric_nudge =
                    NumericNudge{numeric_id,
                                 static_cast<core::u64>(
                                     CompositorProperty::CurvesParameters) +
                                     1U,
                                 edit.direction, edit.modifiers};
              }
            }
          }
        };
        point_field("LAST POINT X", true, current.x, minimum_x, maximum_x, 1U);
        point_field("LAST POINT Y", false, current.y, 0.0F, 1.0F, 2U);
      }
      break;
    }
    case compositor::NodeKind::FilmGrain: {
      const auto *parameters = std::get_if<compositor::FilmGrainParameters>(
          &parameter_node->parameters);
      if (parameters == nullptr) {
        break;
      }
      scalar_field("AMOUNT", CompositorProperty::FilmGrainAmount,
                   parameters->amount, 0.005, scene::min_film_grain_amount,
                   scene::max_film_grain_amount, 3);
      scalar_field("PIXEL SCALE", CompositorProperty::FilmGrainPixelScale,
                   parameters->pixel_scale, 0.25,
                   scene::min_film_grain_pixel_scale,
                   scene::max_film_grain_pixel_scale, 2);
      scalar_field("CHROMA", CompositorProperty::FilmGrainChromaRatio,
                   parameters->chroma_ratio, 0.005,
                   scene::min_film_grain_chroma_ratio,
                   scene::max_film_grain_chroma_ratio, 3);

      const gui::Rect seed_field = next_field("SEED");
      double authored_seed{};
      NumberFieldEdit seed_edit{};
      const gui::UiId seed_id = field_id(CompositorProperty::FilmGrainSeed);
      if (parameter_fields_.draw(ctx, seed_id, seed_field, {},
                                 static_cast<double>(parameters->seed), 1.0,
                                 authored_seed, 0, &seed_edit) &&
          std::isfinite(authored_seed)) {
        const core::u32 value = static_cast<core::u32>(std::clamp(
            std::round(authored_seed), 0.0,
            static_cast<double>(std::numeric_limits<core::u32>::max())));
        if (value != parameters->seed) {
          frame.property_edit = CompositorPropertyEditIntent{
              parameter_node->node, CompositorProperty::FilmGrainSeed,
              CompositorPropertyValue{value}};
          record_nudge(CompositorProperty::FilmGrainSeed, seed_id, seed_edit);
        }
      }

      const gui::Rect mode_button = next_field("MODE");
      const gui::UiId mode_id = field_id(CompositorProperty::FilmGrainMode);
      const gui::Interaction mode_it = ctx.grab(mode_id, mode_button);
      ctx.panel(mode_button, mode_it.hovered ? ctx.theme().widget_hover
                                             : ctx.theme().widget);
      ctx.label(mode_button,
                parameters->mode == scene::FilmGrainMode::RGB ? "RGB"
                                                              : "MONOCHROME",
                mode_it.hovered ? ctx.theme().accent : ctx.theme().text);
      if (mode_it.clicked) {
        const core::u32 value =
            parameters->mode == scene::FilmGrainMode::RGB ? 0U : 1U;
        frame.property_edit = CompositorPropertyEditIntent{
            parameter_node->node, CompositorProperty::FilmGrainMode,
            CompositorPropertyValue{value}};
      }
      break;
    }
    case compositor::NodeKind::Blur: {
      const auto *parameters =
          std::get_if<compositor::BlurParameters>(&parameter_node->parameters);
      if (parameters != nullptr) {
        scalar_field("RADIUS", CompositorProperty::BlurRadius,
                     parameters->radius, 0.10, 0.0F,
                     compositor::max_blur_radius, 1);
      }
      break;
    }
    case compositor::NodeKind::BoxBlur: {
      const auto *parameters = std::get_if<compositor::BoxBlurParameters>(
          &parameter_node->parameters);
      if (parameters != nullptr) {
        unsigned_field("RADIUS PX", CompositorProperty::BoxBlurRadius,
                       parameters->radius_px, 1.0, 0U,
                       compositor::max_box_blur_radius);
        unsigned_field("PASSES", CompositorProperty::BoxBlurPasses,
                       parameters->passes, 1.0, 1U,
                       compositor::max_box_blur_passes);
      }
      break;
    }
    case compositor::NodeKind::DirectionalBlur: {
      const auto *parameters =
          std::get_if<compositor::DirectionalBlurParameters>(
              &parameter_node->parameters);
      if (parameters == nullptr) {
        break;
      }
      scalar_field("LENGTH PX", CompositorProperty::DirectionalBlurLength,
                   parameters->length_px, 0.10, 0.0F,
                   compositor::max_directional_blur_length, 2);

      const gui::Rect field = next_field("ANGLE TURNS");
      double authored{};
      NumberFieldEdit edit{};
      const CompositorProperty property =
          CompositorProperty::DirectionalBlurAngleTurns;
      const gui::UiId numeric_id = field_id(property);
      if (parameter_fields_.draw(ctx, numeric_id, field, {},
                                 static_cast<double>(parameters->angle_turns),
                                 0.001, authored, 3, &edit) &&
          std::isfinite(authored)) {
        double wrapped = authored - std::floor(authored);
        if (wrapped < 0.0) {
          wrapped += 1.0;
        }
        const float value = static_cast<float>(wrapped);
        if (value != parameters->angle_turns) {
          frame.property_edit = CompositorPropertyEditIntent{
              parameter_node->node, property, CompositorPropertyValue{value}};
          record_nudge(property, numeric_id, edit);
        }
      }
      break;
    }
    case compositor::NodeKind::RadialArray: {
      const auto *parameters = std::get_if<compositor::RadialArrayParameters>(
          &parameter_node->parameters);
      if (parameters == nullptr) {
        break;
      }

      const gui::Rect count_field = next_field("COUNT");
      double authored_count = 0.0;
      NumberFieldEdit count_edit{};
      const gui::UiId count_id = field_id(CompositorProperty::RadialCount);
      if (parameter_fields_.draw(ctx, count_id, count_field, {},
                                 static_cast<double>(parameters->count), 1.0,
                                 authored_count, 0, &count_edit) &&
          std::isfinite(authored_count)) {
        const double rounded = std::round(authored_count);
        const core::u32 value = static_cast<core::u32>(
            std::clamp(rounded, 1.0,
                       static_cast<double>(compositor::max_radial_instances)));
        if (value != parameters->count) {
          frame.property_edit = CompositorPropertyEditIntent{
              parameter_node->node, CompositorProperty::RadialCount,
              CompositorPropertyValue{value}};
          record_nudge(CompositorProperty::RadialCount, count_id, count_edit);
        }
      }

      scalar_field("RADIUS", CompositorProperty::RadialRadius,
                   parameters->radius, 0.05, 0.0F,
                   compositor::max_spatial_extent, 2);
      scalar_field("START", CompositorProperty::RadialStartAngleTurns,
                   parameters->start_angle, 0.001,
                   -compositor::max_spatial_extent,
                   compositor::max_spatial_extent, 3);

      const gui::Rect center_row = next_field("CENTER");
      const float gap = 4.0F;
      const float available = std::max(center_row.width - gap, 0.0F);
      const float field_width = available * 0.5F;
      const gui::Rect center_x_field{center_row.x, center_row.y, field_width,
                                     center_row.height};
      const gui::Rect center_y_field{center_x_field.x + field_width + gap,
                                     center_row.y, field_width,
                                     center_row.height};
      double authored_x = 0.0;
      NumberFieldEdit center_x_edit{};
      const gui::UiId center_x_id = field_id(CompositorProperty::RadialCenterX);
      if (parameter_fields_.draw(ctx, center_x_id, center_x_field, "X",
                                 static_cast<double>(parameters->center.x),
                                 0.05, authored_x, 2, &center_x_edit)) {
        emit_float(CompositorProperty::RadialCenterX, authored_x,
                   parameters->center.x, -compositor::max_spatial_extent,
                   compositor::max_spatial_extent);
        record_nudge(CompositorProperty::RadialCenterX, center_x_id,
                     center_x_edit);
      }
      double authored_y = 0.0;
      NumberFieldEdit center_y_edit{};
      const gui::UiId center_y_id = field_id(CompositorProperty::RadialCenterY);
      if (parameter_fields_.draw(ctx, center_y_id, center_y_field, "Y",
                                 static_cast<double>(parameters->center.y),
                                 0.05, authored_y, 2, &center_y_edit)) {
        emit_float(CompositorProperty::RadialCenterY, authored_y,
                   parameters->center.y, -compositor::max_spatial_extent,
                   compositor::max_spatial_extent);
        record_nudge(CompositorProperty::RadialCenterY, center_y_id,
                     center_y_edit);
      }
      break;
    }
    case compositor::NodeKind::LedCarProcess: {
      const auto *parameters = std::get_if<compositor::LedCarProcessParameters>(
          &parameter_node->parameters);
      if (parameters == nullptr) {
        break;
      }
      if (section("POSITION", led_car_position_section)) {
        scalar_field("X", CompositorProperty::LedCarPositionX,
                     parameters->position.x, 0.05,
                     -compositor::max_spatial_extent,
                     compositor::max_spatial_extent, 2);
        scalar_field("Y", CompositorProperty::LedCarPositionY,
                     parameters->position.y, 0.05,
                     -compositor::max_spatial_extent,
                     compositor::max_spatial_extent, 2);
        scalar_field("Z", CompositorProperty::LedCarPositionZ,
                     parameters->position.z, 0.05,
                     -compositor::max_spatial_extent,
                     compositor::max_spatial_extent, 2);
      }
      if (section("ROTATION (TURNS)", led_car_rotation_section)) {
        scalar_field("YAW", CompositorProperty::LedCarYawTurns,
                     parameters->yaw, 0.001, -compositor::max_spatial_extent,
                     compositor::max_spatial_extent, 3);
        scalar_field("PITCH", CompositorProperty::LedCarPitchTurns,
                     parameters->pitch, 0.001, -compositor::max_spatial_extent,
                     compositor::max_spatial_extent, 3);
        scalar_field("ROLL", CompositorProperty::LedCarRollTurns,
                     parameters->roll, 0.001, -compositor::max_spatial_extent,
                     compositor::max_spatial_extent, 3);
      }
      if (section("PLATES", led_car_plates_section)) {
        scalar_field("PLATE SCALE", CompositorProperty::LedCarPlateScale,
                     parameters->plate_scale, 0.05, 0.0F,
                     compositor::max_spatial_extent, 2);
        scalar_field("ARRAY SCALE", CompositorProperty::LedCarArrayScale,
                     parameters->array_scale, 0.05, 0.0F,
                     compositor::max_spatial_extent, 2);
        scalar_field("HORIZONTAL BLEND",
                     CompositorProperty::LedCarHorizontalBlend,
                     parameters->horizontal_blend, 0.005, 0.0F,
                     compositor::max_led_car_horizontal_blend, 3);
      }
      if (section("CEILING REFLECTION", led_car_reflection_section)) {
        scalar_field("HEIGHT", CompositorProperty::LedCarReflectionHeight,
                     parameters->reflection_height, 0.05,
                     -compositor::max_spatial_extent,
                     compositor::max_spatial_extent, 2);
        scalar_field("SCALE", CompositorProperty::LedCarReflectionScale,
                     parameters->reflection_scale, 0.05, 0.0F,
                     compositor::max_spatial_extent, 2);
      }
      break;
    }
    case compositor::NodeKind::SceneLayer:
    case compositor::NodeKind::ChromaKey:
    case compositor::NodeKind::LumaKey:
    case compositor::NodeKind::Matte:
    case compositor::NodeKind::Composite:
    case compositor::NodeKind::Output:
      break;
    }
    ctx.pop_clip();
    parameter_content_height_ =
        parameter_y + parameter_scroll_ - parameter_body.y + 4.0F;

    // A draggable thumb shows where the visible rows sit in the whole list.
    if (scrollable && parameter_body.height > 0.0F) {
      const gui::Rect track{
          parameter_body.x + parameter_body.width - scrollbar_width - 2.0F,
          parameter_body.y + 2.0F, scrollbar_width,
          std::max(parameter_body.height - 4.0F, 0.0F)};
      const float thumb_height = std::max(
          24.0F, track.height * parameter_body.height /
                     std::max(parameter_content_height_, 1.0F));
      const float travel = std::max(track.height - thumb_height, 0.0F);
      const gui::Rect thumb{
          track.x,
          track.y + (maximum_scroll > 0.0F
                         ? travel * parameter_scroll_ / maximum_scroll
                         : 0.0F),
          track.width, std::min(thumb_height, track.height)};
      const gui::Interaction drag =
          ctx.grab(mix(id_, "compositor.parameters.scroll"), thumb);
      if (drag.pressed) {
        parameter_thumb_grab_ = ctx.pointer_y() - thumb.y;
      }
      if (drag.held && travel > 0.0F) {
        parameter_scroll_ = std::clamp(
            (ctx.pointer_y() - parameter_thumb_grab_ - track.y) / travel *
                maximum_scroll,
            0.0F, maximum_scroll);
      }
      ctx.panel(track, with_alpha(ctx.theme().track, 0.8F));
      ctx.panel(thumb, drag.held ? ctx.theme().widget_active
                                 : (drag.hovered ? ctx.theme().widget_hover
                                                 : ctx.theme().widget));
    }
    ctx.pop_clip();
  }
  if (palette_pointer_blocked) {
    ctx.pop_pointer_block();
  }
  if (add_menu_open_) {
    ctx.push_clip(palette_bounds);
    ctx.panel(palette_bounds, scale(ctx.theme().panel, 1.04F));
    border(ctx, palette_bounds, ctx.theme().widget_hover);
    const gui::Rect palette_search{palette_bounds.x + 4.0F,
                                   palette_bounds.y + 4.0F,
                                   std::max(palette_bounds.width - 8.0F, 0.0F),
                                   palette_search_height - 4.0F};
    search_field(ctx, mix(id_, "compositor.node_search"), palette_search,
                 node_search_, "Search nodes and effects");

    float palette_y = palette_bounds.y + palette_search_height + 4.0F;
    const auto add_palette_node = [&](const CompositorPaletteItem &item,
                                      const gui::Rect &row, core::u64 salt) {
      const bool enabled =
          item.kind != compositor::NodeKind::Output || output_count == 0U;
      gui::Interaction interaction{};
      if (enabled) {
        interaction = ctx.grab(mix(id_, "compositor.palette_item", salt), row);
      }
      ctx.panel(row,
                enabled ? (interaction.held
                               ? ctx.theme().widget_active
                               : (interaction.hovered ? ctx.theme().widget_hover
                                                      : ctx.theme().row))
                        : ctx.theme().track);
      ctx.panel(gui::Rect{row.x, row.y, 3.0F, row.height},
                with_alpha(compositor_node_color(ctx, item.kind),
                           enabled ? 1.0F : 0.35F));
      std::string label{compositor::node_kind_name(item.kind)};
      if (!enabled) {
        label += "  ·  ALREADY ADDED";
      }
      ctx.label(gui::Rect{row.x + 10.0F, row.y,
                          std::max(row.width - 14.0F, 0.0F), row.height},
                label,
                enabled && interaction.hovered ? ctx.theme().text
                                               : ctx.theme().text_muted);
      if (!interaction.clicked) {
        return;
      }
      const float cascade =
          static_cast<float>(compositor_graph.nodes.size() % 4U) * 14.0F;
      const gui::UiPoint size = new_node_size(item.kind);
      const float node_height = size.y;
      const float node_width = size.x;
      frame.add_node = CompositorAddIntent{
          item.kind,
          (canvas.width * 0.5F - pan_x_) / zoom_ - node_width * 0.5F + cascade,
          (canvas.height * 0.5F - pan_y_) / zoom_ - node_height * 0.5F +
              cascade};
      add_menu_open_ = false;
      add_category_.reset();
      node_search_.clear();
      ctx.clear_focus();
    };

    if (node_search_.empty()) {
      for (std::size_t category = 0U;
           category < compositor_category_names.size(); ++category) {
        const gui::Rect row{palette_bounds.x + 4.0F, palette_y,
                            std::max(palette_bounds.width - 8.0F, 0.0F),
                            palette_row_height};
        const gui::Interaction interaction =
            ctx.grab(mix(id_, "compositor.palette_category", category), row);
        const bool open = add_category_ == static_cast<core::u8>(category);
        ctx.panel(row, interaction.hovered ? ctx.theme().widget_hover
                                           : ctx.theme().header);
        draw_twirl(ctx, row.x + 9.0F, row.y + row.height * 0.5F, open,
                   interaction.hovered ? ctx.theme().accent
                                       : ctx.theme().text_muted);
        ctx.label(gui::Rect{row.x + 19.0F, row.y,
                            std::max(row.width - 23.0F, 0.0F), row.height},
                  compositor_category_names[category],
                  interaction.hovered ? ctx.theme().text
                                      : ctx.theme().text_muted);
        if (interaction.clicked) {
          add_category_ =
              open ? std::optional<core::u8>{}
                   : std::optional<core::u8>{static_cast<core::u8>(category)};
        }
        palette_y += palette_row_height;
        if (!open) {
          continue;
        }
        for (std::size_t index = 0U; index < compositor_palette_items.size();
             ++index) {
          const CompositorPaletteItem &item = compositor_palette_items[index];
          if (category_index(item.category) != category) {
            continue;
          }
          const gui::Rect item_row{palette_bounds.x + 8.0F, palette_y,
                                   std::max(palette_bounds.width - 12.0F, 0.0F),
                                   palette_row_height};
          add_palette_node(item, item_row, index);
          palette_y += palette_row_height;
        }
      }
    } else {
      std::size_t match_count{};
      // Direct name/alias matches lead; category-only matches follow. Typing
      // "output" therefore puts Output before Composite even though both live
      // in the Combine & Output category.
      for (std::size_t pass = 0U; pass < 2U; ++pass) {
        for (std::size_t index = 0U; index < compositor_palette_items.size();
             ++index) {
          const CompositorPaletteItem &item = compositor_palette_items[index];
          const bool direct = palette_direct_matches(item);
          if (!palette_matches(item) || direct != (pass == 0U)) {
            continue;
          }
          const gui::Rect item_row{palette_bounds.x + 4.0F, palette_y,
                                   std::max(palette_bounds.width - 8.0F, 0.0F),
                                   palette_row_height};
          add_palette_node(item, item_row, index);
          palette_y += palette_row_height;
          ++match_count;
        }
      }
      if (match_count == 0U) {
        ctx.label(gui::Rect{palette_bounds.x + 10.0F, palette_y,
                            std::max(palette_bounds.width - 20.0F, 0.0F),
                            palette_row_height},
                  "NO NODES FOUND", ctx.theme().text_muted);
      }
    }
    ctx.pop_clip();
  }

  frame.editing = parameter_fields_.live();
  ctx.pop_clip();
  if (drag_released) {
    dragging_node_ = {};
  }
  return frame;
}

void CompositorPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                           std::span<const ElementInfo> elements,
                           doc::NodeId primary) const {
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return;
  }
  ctx.push_clip(bounds);
  ctx.panel(bounds, scale(ctx.theme().background, 0.86F));

  const float header_height = 20.0F;
  const gui::Rect header{bounds.x, bounds.y, bounds.width, header_height};
  ctx.panel(header, ctx.theme().background);
  ctx.label(gui::Rect{header.x + 8.0F, header.y, 68.0F, header.height}, "NODES",
            ctx.theme().text);
  ctx.label(gui::Rect{header.x + 76.0F, header.y, 52.0F, header.height}, "CUES",
            ctx.theme().text_muted);
  ctx.label(gui::Rect{header.x + 128.0F, header.y, 90.0F, header.height},
            "AUTOMATION", ctx.theme().text_muted);
  std::array<char, 24U> count{};
  const int count_length =
      std::snprintf(count.data(), count.size(), "%llu LAYERS",
                    static_cast<unsigned long long>(elements.size()));
  const std::string_view count_view{
      count.data(), static_cast<std::size_t>(std::max(count_length, 0))};
  const float count_width = ctx.font_width(count_view);
  ctx.label(gui::Rect{header.x + header.width - count_width - 8.0F, header.y,
                      count_width, header.height},
            count_view, ctx.theme().text_muted);
  ctx.panel(
      gui::Rect{header.x, header.y + header.height - 1.0F, header.width, 1.0F},
      ctx.theme().divider);

  const gui::Rect content{bounds.x, bounds.y + header_height, bounds.width,
                          std::max(bounds.height - header_height, 0.0F)};
  const float inputs_width = std::clamp(content.width * 0.19F, 120.0F, 180.0F);
  const gui::Rect inputs{content.x, content.y,
                         std::min(inputs_width, content.width), content.height};
  ctx.panel(inputs, ctx.theme().panel);
  ctx.panel(
      gui::Rect{inputs.x + inputs.width - 1.0F, inputs.y, 1.0F, inputs.height},
      ctx.theme().divider);
  ctx.label(
      gui::Rect{inputs.x + 8.0F, inputs.y + 1.0F, inputs.width - 16.0F, 19.0F},
      "SCENE LAYERS", ctx.theme().text_muted);

  float input_y = inputs.y + 23.0F;
  const std::size_t input_count = std::min<std::size_t>(elements.size(), 6U);
  for (std::size_t index = 0U; index < input_count; ++index) {
    const gui::Rect row{inputs.x + 7.0F, input_y,
                        std::max(inputs.width - 14.0F, 0.0F), 21.0F};
    ctx.panel(row, index % 2U == 0U ? ctx.theme().row : ctx.theme().row_alt);
    ctx.panel(gui::Rect{row.x + 4.0F, row.y + 6.0F, 3.0F, 9.0F},
              kind_color(elements[index].kind));
    ctx.label(gui::Rect{row.x + 12.0F, row.y, std::max(row.width - 17.0F, 0.0F),
                        row.height},
              element_name(elements[index]), ctx.theme().text_muted);
    input_y += 24.0F;
  }
  if (elements.empty()) {
    ctx.label(gui::Rect{inputs.x + 8.0F, input_y,
                        std::max(inputs.width - 16.0F, 0.0F), 36.0F},
              "No scene layers", ctx.theme().text_muted);
  }

  const gui::Rect graph{inputs.x + inputs.width, content.y,
                        std::max(content.width - inputs.width, 0.0F),
                        content.height};
  // A restrained engineering-paper grid gives an empty graph useful scale.
  for (float x = graph.x + 15.0F; x < graph.x + graph.width; x += 32.0F) {
    ctx.draw_list().add_line(x, graph.y, x, graph.y + graph.height, 1.0F,
                             with_alpha(ctx.theme().divider, 0.60F));
  }
  for (float y = graph.y + 15.0F; y < graph.y + graph.height; y += 32.0F) {
    ctx.draw_list().add_line(graph.x, y, graph.x + graph.width, y, 1.0F,
                             with_alpha(ctx.theme().divider, 0.60F));
  }

  if (graph.width < 450.0F || graph.height < 54.0F) {
    ctx.label(
        gui::Rect{graph.x + 8.0F, graph.y, graph.width - 16.0F, graph.height},
        "COMPOSITOR GRAPH", ctx.theme().text_muted);
    ctx.pop_clip();
    return;
  }

  constexpr float node_height = 44.0F;
  const float node_width = std::clamp(graph.width * 0.135F, 68.0F, 112.0F);
  const float centre_y = graph.y + graph.height * 0.52F;
  constexpr float graph_margin = 14.0F;
  const float column_gap = std::max(
      (graph.width - graph_margin * 2.0F - node_width * 5.0F) / 4.0F, 6.0F);
  const float left_x = graph.x + graph_margin;
  const float analyzer_x = left_x + node_width + column_gap;
  const float element_x = analyzer_x + node_width + column_gap;
  const float grade_x = element_x + node_width + column_gap;
  const float output_x = grade_x + node_width + column_gap;

  std::size_t active_effects = 0U;
  for (const ElementInfo &element : elements) {
    active_effects += element.style.effects.brightness_enabled ? 1U : 0U;
    active_effects += element.style.effects.saturation_enabled ? 1U : 0U;
    active_effects += element.style.effects.blur_enabled ? 1U : 0U;
  }
  std::array<char, 32U> effect_detail{};
  const int effect_length = std::snprintf(
      effect_detail.data(), effect_detail.size(), "%zu active", active_effects);
  const std::string effect_text{
      effect_detail.data(),
      static_cast<std::size_t>(std::max(effect_length, 0))};

  const NodeBox input_node{
      {left_x, centre_y - node_height * 0.5F, node_width, node_height},
      "Scene",
      "Authored layers",
      ctx.theme().accent,
      false};
  const NodeBox analyzer_node{
      {analyzer_x, centre_y - node_height * 0.5F, node_width, node_height},
      "Layer Stack",
      "Scene order",
      ctx.theme().keyframe,
      false};
  const NodeBox grade_node{
      {grade_x, centre_y - node_height * 0.5F, node_width, node_height},
      "Effects",
      effect_text,
      gui::UiColor{0.780F, 0.318F, 0.922F, 1.0F},
      false};
  const NodeBox output_node{
      {output_x, centre_y - node_height * 0.5F, node_width, node_height},
      "Program",
      "Preview",
      ctx.theme().good,
      false};

  std::vector<NodeBox> element_nodes;
  const std::size_t maximum_shown =
      graph.height >= 146.0F ? 3U : (graph.height >= 95.0F ? 2U : 1U);
  const std::size_t shown =
      std::min<std::size_t>(elements.size(), maximum_shown);
  element_nodes.reserve(std::max<std::size_t>(shown, 1U));
  if (shown == 0U) {
    element_nodes.push_back(NodeBox{
        {element_x, centre_y - node_height * 0.5F, node_width, node_height},
        "Scene Layers",
        "Empty",
        ctx.theme().text_muted,
        false});
  } else {
    const float total = static_cast<float>(shown) * node_height +
                        static_cast<float>(shown - 1U) * 7.0F;
    float y = centre_y - total * 0.5F;
    for (std::size_t index = 0U; index < shown; ++index) {
      const ElementInfo &element = elements[index];
      element_nodes.push_back(NodeBox{{element_x, y, node_width, node_height},
                                      element_name(element),
                                      std::string{kind_name(element.kind)},
                                      kind_color(element.kind),
                                      element.node == primary});
      y += node_height + 7.0F;
    }
  }

  wire(ctx, input_node.bounds.x + input_node.bounds.width,
       input_node.bounds.y + input_node.bounds.height * 0.5F,
       analyzer_node.bounds.x,
       analyzer_node.bounds.y + analyzer_node.bounds.height * 0.5F,
       ctx.theme().accent);
  for (const NodeBox &element : element_nodes) {
    wire(ctx, analyzer_node.bounds.x + analyzer_node.bounds.width,
         analyzer_node.bounds.y + analyzer_node.bounds.height * 0.5F,
         element.bounds.x, element.bounds.y + element.bounds.height * 0.5F,
         element.color);
    wire(ctx, element.bounds.x + element.bounds.width,
         element.bounds.y + element.bounds.height * 0.5F, grade_node.bounds.x,
         grade_node.bounds.y + grade_node.bounds.height * 0.5F, element.color);
  }
  wire(ctx, grade_node.bounds.x + grade_node.bounds.width,
       grade_node.bounds.y + grade_node.bounds.height * 0.5F,
       output_node.bounds.x,
       output_node.bounds.y + output_node.bounds.height * 0.5F,
       grade_node.color);

  draw_node(ctx, input_node);
  draw_node(ctx, analyzer_node);
  for (const NodeBox &element : element_nodes) {
    draw_node(ctx, element);
  }
  draw_node(ctx, grade_node);
  draw_node(ctx, output_node);
  ctx.pop_clip();
}

namespace {

// The Preferences dialog's fields, in the order they are laid out. Naming the
// members once keeps the layout, the hint text, and the override notice from
// drifting apart as fields are added.
struct PreferencesField final {
  std::string_view label;
  std::string_view hint;
  std::string EditorPreferences::*value;
  bool EditorPreferencesState::*overridden;
};

[[nodiscard]] const std::array<PreferencesField, 4U> &preferences_fields() {
  static const std::array<PreferencesField, 4U> fields{{
      {"Director listen", "ADDRESS:PORT render nodes connect to",
       &EditorPreferences::director_listen,
       &EditorPreferencesState::director_listen_overridden},
      {"Show ID", "32 hexadecimal characters, same on every node",
       &EditorPreferences::show_id,
       &EditorPreferencesState::show_id_overridden},
      {"Director epoch", "32 hexadecimal characters",
       &EditorPreferences::director_epoch,
       &EditorPreferencesState::director_epoch_overridden},
      {"Write-ahead log", "Director recovery journal",
       &EditorPreferences::wal_path,
       &EditorPreferencesState::wal_path_overridden},
  }};
  return fields;
}

constexpr float preferences_card_width = 620.0F;
// The category tabs keep each focused pane compact enough to keep the project
// dialog readable without hiding Apply/Cancel below a long settings form.
// Tall enough for the longest category - Renderer with every optional row
// shown plus the two lines reporting what ran - without clipping. A card taller
// than the window is cut to the window.
constexpr float project_settings_card_height = 700.0F;
constexpr float preferences_card_height = 780.0F;
constexpr float preferences_field_pitch = 46.0F;

[[nodiscard]] gui::Rect
preferences_card(const gui::Rect &bounds,
                 float height = project_settings_card_height) {
  const float x =
      bounds.x + std::max((bounds.width - preferences_card_width) * 0.5F, 0.0F);
  const float y = bounds.y + std::max((bounds.height - height) * 0.5F, 0.0F);
  return gui::Rect{std::floor(x), std::floor(y), preferences_card_width,
                   height};
}

[[nodiscard]] bool preferences_button(gui::UiContext &ctx, gui::UiId id,
                                      const gui::Rect &bounds,
                                      std::string_view label, bool enabled) {
  const gui::Interaction it =
      enabled ? ctx.grab(id, bounds) : gui::Interaction{};
  gui::UiColor body = !enabled ? ctx.theme().track
                               : (it.held && !it.released
                                      ? ctx.theme().widget_active
                                      : (it.hovered ? ctx.theme().widget_hover
                                                    : ctx.theme().widget));
  ctx.panel(bounds, body);
  border(ctx, bounds, enabled ? ctx.theme().header : ctx.theme().divider);
  const float text_width = ctx.font_width(label);
  ctx.label(
      gui::Rect{bounds.x + std::max((bounds.width - text_width) * 0.5F, 2.0F),
                bounds.y, bounds.width, bounds.height},
      label, enabled ? ctx.theme().text : ctx.theme().text_muted);
  return enabled && it.clicked;
}

} // namespace

void PreferencesPanel::open(const EditorPreferences &stored) {
  draft_ = stored;
  password_draft_.clear();
  password_edited_ = false;
  scroll_ = 0.0F;
  open_ = true;
}

void PreferencesPanel::close() noexcept { open_ = false; }

// What a pass is actually doing, in the operator's terms rather than the
// renderer's. `Refused` is the only one that is a fault; the rest are facts,
// and dressing a fact up as a warning trains people to ignore the colour.
[[nodiscard]] std::string_view pass_state_text(
    render::PassState state) noexcept {
  switch (state) {
  case render::PassState::NotAuthored:
    return "off";
  case render::PassState::Active:
    return "running";
  case render::PassState::BackendUnsupported:
    return "inert - needs Advanced";
  case render::PassState::Idle:
    return "idle";
  case render::PassState::Refused:
    return "REFUSED";
  }
  return "";
}

std::string_view environment_status_text(
    EnvironmentStatus status) noexcept {
  switch (status) {
  case EnvironmentStatus::NotAuthored:
    return "Empty: the two-colour hemisphere lights the scene.";
  case EnvironmentStatus::Loading:
    return "Reading the map.";
  case EnvironmentStatus::Failed:
    return "Could not read the HDR/EXR image or its colour-space tag.";
  case EnvironmentStatus::BackendUnsupported:
    // Names the selected backend rather than the authored one, because the
    // usual reason this appears is a project that authored Advanced and had it
    // refused - most often for requesting a pass that does not exist yet, which
    // the Renderer page offers to clear.
    return "Ignored: the Advanced renderer is not running for this project.";
  case EnvironmentStatus::NoStage:
    return "This scene has no stage node, so there is nowhere to store it.";
  case EnvironmentStatus::Ready:
    return "Lighting the scene and drawing as the sky.";
  }
  return "Empty: the two-colour hemisphere lights the scene.";
}

std::string_view color_status_text(ColorStatus status) noexcept {
  switch (status) {
  case ColorStatus::Loading:
    return "Baking the table on the residency worker.";
  case ColorStatus::Ready:
    return "Baked and live on the output.";
  case ColorStatus::Failed:
    return "Look could not be read. 3D .cube only; the transform is live "
           "without it.";
  case ColorStatus::BackendUnsupported:
    return "Ignored: colour is applied on the scene-linear path, and this "
           "project is running Basic rather than Advanced.";
  }
  return "Baked and live on the output.";
}

ColorStatus color_status_for(render::DisplayLutState state) noexcept {
  switch (state) {
  case render::DisplayLutState::Ready:
    return ColorStatus::Ready;
  case render::DisplayLutState::Failed:
    return ColorStatus::Failed;
  case render::DisplayLutState::Unseen:
  case render::DisplayLutState::Pending:
  case render::DisplayLutState::Busy:
    return ColorStatus::Loading;
  }
  return ColorStatus::Loading;
}

void ProjectSettingsPanel::open(
    const scene::EffectSettings &settings,
    const scene::RendererSettings &renderer,
    const std::optional<scene::LensProfile> &lens_profile,
    const scene::Environment &environment) {
  draft_ = scene::clamped(settings);
  renderer_draft_ = scene::clamped(renderer);
  environment_draft_ = environment;
  lens_draft_ =
      lens_profile.has_value()
          ? std::optional<scene::LensProfile>{scene::clamped(*lens_profile)}
          : std::nullopt;
  category_ = Category::Tracking;
  open_ = true;
}

ProjectSettingsFrame
ProjectSettingsPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                           const ProjectSettingsState &state) {
  ProjectSettingsFrame result{};
  if (!open_) {
    return result;
  }
  const gui::Rect card = preferences_card(
      bounds, std::min(project_settings_card_height, bounds.height));
  const float buttons_y = card.y + card.height - 38.0F;
  ctx.panel(bounds, gui::UiColor{0.0F, 0.0F, 0.0F, 0.55F});
  ctx.panel(card, ctx.theme().background);
  border(ctx, card, ctx.theme().divider);
  ctx.push_clip(card);

  const gui::Rect header{card.x, card.y, card.width, 34.0F};
  ctx.panel(header, ctx.theme().header);
  ctx.label(gui::Rect{header.x + 14.0F, header.y, header.width - 14.0F,
                      header.height},
            "PROJECT SETTINGS", ctx.theme().text);
  ctx.panel(
      gui::Rect{card.x, header.y + header.height - 1.0F, card.width, 1.0F},
      ctx.theme().divider);
  ctx.label(gui::Rect{card.x + 14.0F, header.y + header.height + 6.0F,
                      card.width - 28.0F, 18.0F},
            "Saved in the project. Applies to every scene and every node.",
            ctx.theme().text_muted);

  // A focused category is much quicker to audit in a dark control room than a
  // very tall scrolling form. Keep each tab visible and leave Apply/Cancel
  // fixed at the bottom of the modal.
  constexpr float tab_height = 24.0F;
  constexpr float tab_gap = 6.0F;
  const float tabs_y = header.y + header.height + 30.0F;
  // Tracking setup and lens calibration share the modal; scene controls live
  // in the Inspector.
  constexpr float category_count = 2.0F;
  const float tab_width =
      (card.width - 28.0F - tab_gap * (category_count - 1.0F))
      / category_count;
  const auto tab = [&](Category category, std::string_view id,
                       std::string_view label, std::size_t index) {
    const gui::Rect tab_bounds{
        card.x + 14.0F + static_cast<float>(index) * (tab_width + tab_gap),
        tabs_y, tab_width, tab_height};
    const bool selected = category_ == category;
    const gui::Interaction interaction = ctx.grab(mix(id_, id), tab_bounds);
    const gui::UiColor body =
        selected ? ctx.theme().selection
                 : (interaction.held && !interaction.released
                        ? ctx.theme().widget_active
                        : (interaction.hovered ? ctx.theme().widget_hover
                                               : ctx.theme().widget));
    ctx.panel(tab_bounds, body);
    border(ctx, tab_bounds,
           selected ? ctx.theme().accent : ctx.theme().divider);
    const float text_width = ctx.font_width(label);
    ctx.label(
        gui::Rect{tab_bounds.x +
                      std::max((tab_bounds.width - text_width) * 0.5F, 2.0F),
                  tab_bounds.y, tab_bounds.width, tab_bounds.height},
        label, selected ? ctx.theme().text : ctx.theme().text_muted);
    if (interaction.clicked && !selected) {
      // A numeric field can own the keyboard while it is being typed. Its
      // pane is disappearing, so explicitly give that focus back first.
      tracking_fields_.cancel(ctx);
      category_ = category;
    }
  };
  tab(Category::Tracking, "project.tab.tracking", "TRACKING", 0U);
  tab(Category::Lens, "project.tab.lens", "LENS", 1U);
  ctx.panel(gui::Rect{card.x + 14.0F, tabs_y + tab_height + 7.0F,
                      card.width - 28.0F, 1.0F},
            ctx.theme().divider);

  const gui::Rect content_bounds{
      card.x, tabs_y + tab_height + 8.0F, card.width,
      std::max(buttons_y - 8.0F - (tabs_y + tab_height + 8.0F), 0.0F)};
  ctx.push_clip(content_bounds);

  constexpr float label_width = 150.0F;
  constexpr float row_pitch = 44.0F;
  float y = content_bounds.y;
  const auto section = [&](std::string_view title) {
    ctx.label(gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 18.0F}, title,
              ctx.theme().accent);
    y += 22.0F;
  };

  if (category_ == Category::Tracking) {
    section("INNER FRUSTUM");
    static_cast<void>(ctx.checkbox(
        mix(id_, "icvfx.enabled"),
        gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 24.0F},
        renderer_draft_.icvfx.inner_frustum,
        "Draw tracked cameras' plates on the wall"));
    y += 34.0F;

    const auto unsigned_row = [&](std::string_view label, std::string_view id,
                                  core::u32 &value, core::u32 maximum,
                                  std::string_view note) {
      ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 22.0F}, label,
                ctx.theme().text);
      const gui::Rect field{card.x + 14.0F + label_width, y,
                            card.width - 28.0F - label_width, 22.0F};
      double edited{};
      if (tracking_fields_.draw(ctx, mix(id_, id), field, "",
                                static_cast<double>(value), 1.0, edited, 0) &&
          std::isfinite(edited)) {
        const double bounded =
            std::clamp(edited, 0.0, static_cast<double>(maximum));
        value = static_cast<core::u32>(std::llround(bounded));
      }
      ctx.label(gui::Rect{field.x, y + 21.0F, field.width, 16.0F}, note,
                ctx.theme().text_muted);
      y += row_pitch;
    };
    unsigned_row("Latency", "icvfx.latency",
                 renderer_draft_.icvfx.latency_frames,
                 scene::max_icvfx_latency_frames,
                 "Control frames ahead; prediction estimates the pose at this stamp.");
    static_cast<void>(ctx.checkbox(mix(id_, "icvfx.predict"),
        gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 24.0F},
        renderer_draft_.icvfx.pose_prediction,
        "Predict camera motion on the Director"));
    y += 30.0F;
    unsigned_row("Fit samples", "icvfx.prediction-window",
                 renderer_draft_.icvfx.prediction_window,
                 scene::max_pose_prediction_window,
                 "2-16 fresh samples. Longer fits smooth more and react slower.");
    renderer_draft_.icvfx.prediction_window =
        std::max(renderer_draft_.icvfx.prediction_window, 2U);
    unsigned_row("Jitter filter %", "icvfx.jitter-filter",
                 renderer_draft_.icvfx.jitter_filter_percent, 100U,
                 "0 keeps raw motion; 100 uses the fitted pose. See Live Data errors.");

    y += 4.0F;
    section("NEW CAMERA DEFAULTS");
    unsigned_row("Margin", "icvfx.margin",
                 renderer_draft_.icvfx.camera_defaults.margin_px,
                 scene::max_icvfx_margin_px,
                 "Pixels of plate drawn beyond the camera's own frustum.");
    unsigned_row("Soft edge", "icvfx.soft-edge",
                 renderer_draft_.icvfx.camera_defaults.soft_edge_px,
                 scene::max_icvfx_soft_edge_px,
                 "Width of the fade from plate to the outer frustum.");
    unsigned_row("Plate scale", "icvfx.scale",
                 renderer_draft_.icvfx.camera_defaults.resolution_numerator,
                 scene::max_icvfx_resolution_scale,
                 "Plate resolution numerator; the denominator is below.");
    unsigned_row("Plate divisor", "icvfx.divisor",
                 renderer_draft_.icvfx.camera_defaults.resolution_denominator,
                 scene::max_icvfx_resolution_scale,
                 "Plate resolution denominator. Equal values render 1:1.");
    ctx.label(gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 16.0F},
              "A camera keeps its own values once added; edit them in the "
              "Inspector.",
              ctx.theme().text_muted);
    y += 20.0F;
  }

  // Lens controls deliberately author only calibration. Zoom/focus/iris are
  // sampled controls carried beside the tracked pose, so making them sliders in
  // this dialog would tempt an operator to put live frame state in the project.
  if (category_ == Category::Lens) {
    section("LENS CALIBRATION");
    const bool camera_available = state.camera_node != doc::NodeId{};
    const bool lens_enabled = lens_draft_.has_value();
    ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 22.0F}, "Projection",
              camera_available ? ctx.theme().text : ctx.theme().text_muted);
    const float lens_x = card.x + 14.0F + label_width;
    if (preferences_button(ctx, mix(id_, "lens.calibrated"),
                           gui::Rect{lens_x, y, 116.0F, 22.0F}, "CALIBRATED",
                           camera_available && !lens_enabled)) {
      lens_draft_ = scene::LensProfile{};
    }
    if (preferences_button(ctx, mix(id_, "lens.legacy"),
                           gui::Rect{lens_x + 124.0F, y, 116.0F, 22.0F},
                           "LEGACY FOV", camera_available && lens_enabled)) {
      lens_draft_.reset();
    }
    ctx.label(
        gui::Rect{lens_x, y + 21.0F, card.width - 28.0F - label_width, 16.0F},
        camera_available
            ? "Sensor and distortion are calibration; live encoders are status."
            : "No active stage camera is available for lens calibration.",
        ctx.theme().text_muted);
    y += row_pitch;

    if (lens_draft_.has_value()) {
      scene::LensProfile &lens = *lens_draft_;
      constexpr float lens_gap = 10.0F;
      constexpr float lens_label_width = 66.0F;
      constexpr float lens_value_width = 48.0F;
      constexpr float lens_row_pitch = 26.0F;
      const float lens_column_width = (card.width - 28.0F - lens_gap) * 0.5F;
      const auto lens_scalar = [&](std::string_view label, float &value,
                                   float minimum, float maximum,
                                   const char *format, float x) {
        ctx.label(gui::Rect{x, y, lens_label_width, 18.0F}, label,
                  ctx.theme().text);
        const gui::Rect track{x + lens_label_width + 2.0F, y,
                              lens_column_width - lens_label_width -
                                  lens_value_width - 6.0F,
                              18.0F};
        static_cast<void>(
            ctx.slider(mix(id_, label), track, value, minimum, maximum));
        std::array<char, 24U> text{};
        const int length = std::snprintf(text.data(), text.size(), format,
                                         static_cast<double>(value));
        ctx.label(
            gui::Rect{track.x + track.width + 4.0F, y, lens_value_width, 18.0F},
            std::string_view{text.data(),
                             static_cast<std::size_t>(std::max(length, 0))},
            ctx.theme().keyframe);
      };
      const auto lens_row = [&](std::string_view left_label, float &left_value,
                                float left_minimum, float left_maximum,
                                const char *left_format,
                                std::string_view right_label,
                                float &right_value, float right_minimum,
                                float right_maximum, const char *right_format) {
        lens_scalar(left_label, left_value, left_minimum, left_maximum,
                    left_format, card.x + 14.0F);
        lens_scalar(right_label, right_value, right_minimum, right_maximum,
                    right_format,
                    card.x + 14.0F + lens_column_width + lens_gap);
        y += lens_row_pitch;
      };
      lens_row("Sensor W", lens.sensor_width_mm,
               scene::min_lens_sensor_extent_mm,
               scene::max_lens_sensor_extent_mm, "%.1f", "Sensor H",
               lens.sensor_height_mm, scene::min_lens_sensor_extent_mm,
               scene::max_lens_sensor_extent_mm, "%.1f");
      lens_row("Focal", lens.focal_length_mm, scene::min_lens_focal_length_mm,
               scene::max_lens_focal_length_mm, "%.1f", "Pupil m",
               lens.entrance_pupil_offset_m, -scene::max_lens_pupil_offset_m,
               scene::max_lens_pupil_offset_m, "%.3f");
      lens_row("K1", lens.distortion.k1,
               -scene::max_lens_distortion_coefficient,
               scene::max_lens_distortion_coefficient, "%.3f", "K2",
               lens.distortion.k2, -scene::max_lens_distortion_coefficient,
               scene::max_lens_distortion_coefficient, "%.3f");
      lens_row("K3", lens.distortion.k3,
               -scene::max_lens_distortion_coefficient,
               scene::max_lens_distortion_coefficient, "%.3f", "P1",
               lens.distortion.p1, -scene::max_lens_distortion_coefficient,
               scene::max_lens_distortion_coefficient, "%.3f");
      lens_scalar(
          "P2", lens.distortion.p2, -scene::max_lens_distortion_coefficient,
          scene::max_lens_distortion_coefficient, "%.3f", card.x + 14.0F);
      std::array<char, 48U> calibration_text{};
      const int calibration_length =
          std::snprintf(calibration_text.data(), calibration_text.size(),
                        "Imported table: %u points", lens.calibration_count);
      ctx.label(gui::Rect{card.x + 14.0F + lens_column_width + lens_gap, y,
                          lens_column_width, 18.0F},
                std::string_view{
                    calibration_text.data(),
                    static_cast<std::size_t>(std::max(calibration_length, 0))},
                ctx.theme().text_muted);
      y += lens_row_pitch;
    }
  }
  ctx.pop_clip();

  draft_ = scene::clamped(draft_);
  renderer_draft_ = scene::clamped(renderer_draft_);
  if (lens_draft_.has_value()) {
    lens_draft_ = scene::clamped(*lens_draft_);
  }

  const bool changed = draft_ != state.settings ||
                       renderer_draft_ != state.renderer ||
                       environment_draft_ != state.environment ||
                       lens_draft_ != state.lens_profile;
  const bool reset = preferences_button(
      ctx, mix(id_, "project.reset"),
      gui::Rect{card.x + 14.0F, buttons_y, 84.0F, 26.0F}, "DEFAULTS",
      renderer_draft_.icvfx != scene::IcvfxSettings{} ||
          lens_draft_.has_value());
  const bool cancelled = preferences_button(
      ctx, mix(id_, "project.cancel"),
      gui::Rect{card.x + card.width - 190.0F, buttons_y, 84.0F, 26.0F},
      "CANCEL", true);
  const bool applied = preferences_button(
      ctx, mix(id_, "project.apply"),
      gui::Rect{card.x + card.width - 98.0F, buttons_y, 84.0F, 26.0F}, "APPLY",
      changed);
  ctx.pop_clip();

  // Registered last, so it catches only presses no control in the card wanted.
  const gui::Interaction scrim = ctx.grab(mix(id_, "project.scrim"), bounds);
  const bool dismissed =
      scrim.clicked && !card.contains(ctx.pointer_x(), ctx.pointer_y());

  if (reset) {
    tracking_fields_.cancel(ctx);
    renderer_draft_.icvfx = scene::IcvfxSettings{};
    lens_draft_.reset();
    return result;
  }
  if (applied) {
    result.apply = ProjectSettingsValues{
        draft_, renderer_draft_, environment_draft_, lens_draft_};
  }
  if (applied || cancelled || dismissed) {
    tracking_fields_.cancel(ctx);
    open_ = false;
  }
  return result;
}

PreferencesFrame PreferencesPanel::draw(gui::UiContext &ctx,
                                        const gui::Rect &bounds,
                                        const EditorPreferencesState &state) {
  PreferencesFrame result{};
  if (!open_) {
    return result;
  }
  const gui::Rect card = preferences_card(
      bounds, std::min(preferences_card_height, bounds.height));
  const float buttons_y = card.y + card.height - 38.0F;

  // Dim the workspace. This is the drawing half only; the scrim's hit test is
  // registered at the end of this function, because a full-screen grab placed
  // here would claim the pointer before any control in the card could.
  ctx.panel(bounds, gui::UiColor{0.0F, 0.0F, 0.0F, 0.55F});
  ctx.panel(card, ctx.theme().background);
  border(ctx, card, ctx.theme().divider);
  ctx.push_clip(card);

  const gui::Rect header{card.x, card.y, card.width, 34.0F};
  ctx.panel(header, ctx.theme().header);
  ctx.label(gui::Rect{header.x + 14.0F, header.y, header.width - 14.0F,
                      header.height},
            "PREFERENCES", ctx.theme().text);
  ctx.panel(
      gui::Rect{card.x, header.y + header.height - 1.0F, card.width, 1.0F},
      ctx.theme().divider);
  const gui::Rect content_bounds{
      card.x, header.y + header.height, card.width,
      std::max(buttons_y - 6.0F - (header.y + header.height), 0.0F)};
  constexpr float scroll_content_height = 760.0F;
  const float maximum_scroll =
      std::max(scroll_content_height - content_bounds.height, 0.0F);
  if (content_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
    scroll_ -= ctx.wheel() * 36.0F;
  }
  scroll_ = std::clamp(scroll_, 0.0F, maximum_scroll);
  ctx.push_clip(content_bounds);

  ctx.label(gui::Rect{card.x + 14.0F, header.y + header.height + 6.0F - scroll_,
                      card.width - 28.0F, 18.0F},
            "This machine only. Never saved in a project or sent to a node.",
            ctx.theme().text_muted);

  float y = header.y + header.height + 32.0F - scroll_;
  constexpr float label_width = 132.0F;
  for (const PreferencesField &field : preferences_fields()) {
    ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 24.0F}, field.label,
              ctx.theme().text);
    const gui::Rect input{card.x + 14.0F + label_width, y,
                          card.width - 28.0F - label_width, 24.0F};
    const gui::UiId field_id = mix(id_, field.label);
    static_cast<void>(ctx.text_field(field_id, input, draft_.*(field.value)));
    if ((draft_.*(field.value)).empty() && ctx.focus() != field_id) {
      ctx.label(gui::Rect{input.x + 6.0F, input.y,
                          std::max(input.width - 12.0F, 0.0F), input.height},
                field.hint, ctx.theme().text_muted);
    }
    // Naming what the process is really using matters more than what is
    // stored: without it, an argument-driven launch looks like the settings
    // are being ignored at random.
    if (state.*(field.overridden)) {
      std::string note{"in use this run: "};
      note.append(state.active.*(field.value));
      note.append("  (command line)");
      ctx.label(gui::Rect{input.x + 2.0F, y + 26.0F, input.width - 2.0F, 16.0F},
                note, ctx.theme().warning);
    }
    y += preferences_field_pitch;
  }

  // ------------------------------------------------------- render nodes
  // The password Output Monitor controls render machines with. It is typed,
  // never shown back, and applies the moment it is saved.
  y += 4.0F;
  ctx.panel(gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 1.0F},
            ctx.theme().divider);
  y += 10.0F;
  ctx.label(gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 18.0F},
            "RENDER NODES", ctx.theme().text);
  y += 22.0F;
  {
    ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 24.0F}, "Password",
              ctx.theme().text);
    const gui::Rect clear{card.x + card.width - 90.0F, y, 76.0F, 24.0F};
    const gui::Rect input{
        card.x + 14.0F + label_width, y,
        std::max(clear.x - 8.0F - (card.x + 14.0F + label_width), 0.0F),
        24.0F};
    const gui::UiId password_id = mix(id_, "preferences.render-node-password");
    if (ctx.password_field(password_id, input, password_draft_)) {
      password_edited_ = true;
      draft_.render_node_password = !password_draft_.empty();
    }
    if (password_draft_.empty() && ctx.focus() != password_id) {
      ctx.label(gui::Rect{input.x + 6.0F, input.y,
                          std::max(input.width - 12.0F, 0.0F), input.height},
                draft_.render_node_password ? "Set - type to replace"
                                            : "None",
                ctx.theme().text_muted);
    }
    const bool clearable = draft_.render_node_password;
    if (preferences_button(ctx, mix(id_, "preferences.render-node-password.clear"),
                           clear, "CLEAR", clearable)) {
      password_draft_.clear();
      password_edited_ = true;
      draft_.render_node_password = false;
    }
    y += 28.0F;
    const std::string_view note =
        state.render_node_password_overridden
            ? "This run uses the password given on the command line."
        : draft_.render_node_password
            ? "Give each render machine the same password in its setup "
              "window. Applies when saved."
            : "No password: any Media Server on this network can start and "
              "stop render machines. Applies when saved.";
    ctx.label(gui::Rect{input.x + 2.0F, y, card.width - 28.0F - label_width,
                        16.0F},
              note,
              state.render_node_password_overridden ||
                      !draft_.render_node_password
                  ? ctx.theme().warning
                  : ctx.theme().text_muted);
    y += 24.0F;
  }

  y += 4.0F;
  ctx.panel(gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 1.0F},
            ctx.theme().divider);
  y += 10.0F;
  ctx.label(gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 18.0F}, "PROXIES",
            ctx.theme().text);
  y += 22.0F;
  static_cast<void>(ctx.checkbox(
      mix(id_, "preferences.proxy.next-to-source"),
      gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 24.0F},
      draft_.proxy_cache_next_to_source, "Store next to source media"));
  y += 30.0F;

  ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 24.0F}, "Cache folder",
            ctx.theme().text);
  const gui::Rect browse{card.x + card.width - 90.0F, y, 76.0F, 24.0F};
  const gui::Rect proxy_input{
      card.x + 14.0F + label_width, y,
      std::max(browse.x - 8.0F - (card.x + 14.0F + label_width), 0.0F), 24.0F};
  const bool custom_folder_enabled = !draft_.proxy_cache_next_to_source;
  if (custom_folder_enabled) {
    const gui::UiId folder_id = mix(id_, "preferences.proxy.cache-directory");
    static_cast<void>(
        ctx.text_field(folder_id, proxy_input, draft_.proxy_cache_directory));
    if (draft_.proxy_cache_directory.empty() && ctx.focus() != folder_id) {
      ctx.label(gui::Rect{proxy_input.x + 6.0F, proxy_input.y,
                          std::max(proxy_input.width - 12.0F, 0.0F),
                          proxy_input.height},
                "Built-in per-user folder", ctx.theme().text_muted);
    }
  } else {
    ctx.panel(proxy_input, ctx.theme().track);
    ctx.label(gui::Rect{proxy_input.x + 6.0F, proxy_input.y,
                        std::max(proxy_input.width - 12.0F, 0.0F),
                        proxy_input.height},
              "Resolved separately for each source", ctx.theme().text_muted);
  }
  if (preferences_button(ctx, mix(id_, "preferences.proxy.browse"), browse,
                         "BROWSE", custom_folder_enabled)) {
    const char *const selected =
        tinyfd_selectFolderDialog("Select Proxy Cache Folder",
                                  draft_.proxy_cache_directory.empty()
                                      ? ""
                                      : draft_.proxy_cache_directory.c_str());
    if (selected != nullptr) {
      draft_.proxy_cache_directory = selected;
    }
  }
  y += 32.0F;

  const auto integer_slider_row = [&](std::string_view label,
                                      std::string_view id, core::u32 &value,
                                      core::u32 minimum, core::u32 maximum,
                                      std::string_view suffix) {
    ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 24.0F}, label,
              ctx.theme().text);
    const float value_width = 92.0F;
    const gui::Rect track{card.x + 14.0F + label_width, y + 6.0F,
                          card.width - 28.0F - label_width - value_width,
                          12.0F};
    float slider_value = static_cast<float>(value);
    if (ctx.slider(mix(id_, id), track, slider_value,
                   static_cast<float>(minimum), static_cast<float>(maximum))) {
      value = static_cast<core::u32>(std::lround(slider_value));
      value = std::clamp(value, minimum, maximum);
    }
    std::string exact = std::to_string(value);
    exact.push_back(' ');
    exact.append(suffix);
    ctx.label(
        gui::Rect{track.x + track.width + 8.0F, y, value_width - 8.0F, 24.0F},
        exact, ctx.theme().text_muted);
    y += 30.0F;
  };

  const auto power_of_two_slider_row = [&](std::string_view label,
                                           std::string_view id,
                                           core::u32 &value, core::u32 base,
                                           core::u32 maximum,
                                           std::string_view suffix) {
    const float minimum_power = 0.0F;
    const float maximum_power =
        std::log2(static_cast<float>(maximum) / static_cast<float>(base));
    float power = std::log2(static_cast<float>(std::max(value, base)) /
                            static_cast<float>(base));
    ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 24.0F}, label,
              ctx.theme().text);
    const float value_width = 92.0F;
    const gui::Rect track{card.x + 14.0F + label_width, y + 6.0F,
                          card.width - 28.0F - label_width - value_width,
                          12.0F};
    if (ctx.slider(mix(id_, id), track, power, minimum_power, maximum_power)) {
      const auto exponent = static_cast<core::u32>(std::lround(power));
      value = std::min(base << exponent, maximum);
    }
    std::string exact = std::to_string(value);
    exact.push_back(' ');
    exact.append(suffix);
    ctx.label(
        gui::Rect{track.x + track.width + 8.0F, y, value_width - 8.0F, 24.0F},
        exact, ctx.theme().text_muted);
    y += 30.0F;
  };

  power_of_two_slider_row("Cache limit", "preferences.proxy.cache-limit",
                          draft_.proxy_cache_gibibytes, 1U, 1'024U, "GiB");

  ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 24.0F},
            "Transcode priority", ctx.theme().text);
  const float priority_x = card.x + 14.0F + label_width;
  constexpr float priority_gap = 6.0F;
  const float priority_width =
      (card.x + card.width - 14.0F - priority_x - priority_gap * 2.0F) / 3.0F;
  using Priority = EditorPreferences::TranscodePriority;
  const auto priority_button = [&](std::string_view key, std::string_view label,
                                   Priority value, float x) {
    if (preferences_button(ctx, mix(id_, key),
                           gui::Rect{x, y, priority_width, 24.0F}, label,
                           draft_.proxy_transcode_priority != value)) {
      draft_.proxy_transcode_priority = value;
    }
  };
  priority_button("preferences.proxy.priority.background", "BACKGROUND",
                  Priority::Background, priority_x);
  priority_button("preferences.proxy.priority.balanced", "BALANCED",
                  Priority::Balanced,
                  priority_x + priority_width + priority_gap);
  priority_button("preferences.proxy.priority.fast", "FAST", Priority::Fast,
                  priority_x + (priority_width + priority_gap) * 2.0F);
  y += 32.0F;

  result.clear_proxy_cache = preferences_button(
      ctx, mix(id_, "preferences.proxy.clear"),
      gui::Rect{card.x + 14.0F, y, 118.0F, 26.0F}, "CLEAR CACHE", true);
  ctx.label(gui::Rect{card.x + 142.0F, y, card.width - 156.0F, 26.0F},
            "Clears the cache currently in use, not this unsaved draft.",
            ctx.theme().text_muted);
  y += 36.0F;

  ctx.panel(gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 1.0F},
            ctx.theme().divider);
  y += 10.0F;
  ctx.label(gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 18.0F},
            "OUTPUT PLAYBACK", ctx.theme().text);
  y += 22.0F;

  struct BufferPreset final {
    std::string_view key;
    std::string_view label;
    core::u32 buffer;
    core::u32 read_ahead;
  };
  constexpr std::array<BufferPreset, 4U> presets{{
      {"low", "LOW LATENCY", 3U, 3U},
      {"balanced", "BALANCED", 8U, 8U},
      {"smooth", "SMOOTH", 12U, 12U},
      {"reliable", "RELIABLE", 24U, 24U},
  }};
  constexpr float preset_gap = 6.0F;
  const float preset_width = (card.width - 28.0F - preset_gap * 3.0F) / 4.0F;
  for (std::size_t index = 0U; index < presets.size(); ++index) {
    const BufferPreset &preset = presets[index];
    const bool selected = draft_.output_buffer_frames == preset.buffer &&
                          draft_.output_read_ahead_frames == preset.read_ahead;
    if (preferences_button(ctx, mix(id_, preset.key),
                           gui::Rect{card.x + 14.0F +
                                         static_cast<float>(index) *
                                             (preset_width + preset_gap),
                                     y, preset_width, 24.0F},
                           preset.label, !selected)) {
      draft_.output_buffer_frames = preset.buffer;
      draft_.output_read_ahead_frames = preset.read_ahead;
    }
  }
  y += 32.0F;

  integer_slider_row("Output buffer", "preferences.playback.buffer",
                     draft_.output_buffer_frames, 0U, 60U, "frames");
  integer_slider_row("Read-ahead", "preferences.playback.read-ahead",
                     draft_.output_read_ahead_frames, 1U, 60U, "frames");
  power_of_two_slider_row("Decoded cache", "preferences.playback.cache",
                          draft_.playback_cache_mebibytes, 256U, 32'768U,
                          "MiB");
  integer_slider_row("Trail", "preferences.playback.trail",
                     draft_.playback_trail_frames, 0U, 16U, "frames");

  ctx.label(gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 18.0F},
            "More buffering smooths output at the cost of latency and memory.",
            ctx.theme().text_muted);
  y += 20.0F;
  ctx.label(gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 18.0F},
            "Saved settings apply the next time this program starts.",
            ctx.theme().text_muted);
  y += 20.0F;
  if (!state.message.empty()) {
    ctx.label(gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 18.0F},
              state.message, ctx.theme().text);
  }
  ctx.pop_clip();

  // Retyping a set password is a change even though "set" is unchanged.
  const bool changed = draft_ != state.stored ||
                       (password_edited_ && !password_draft_.empty());
  const bool cancelled = preferences_button(
      ctx, mix(id_, "preferences.cancel"),
      gui::Rect{card.x + card.width - 190.0F, buttons_y, 84.0F, 26.0F},
      "CANCEL", true);
  const bool saved = preferences_button(
      ctx, mix(id_, "preferences.save"),
      gui::Rect{card.x + card.width - 98.0F, buttons_y, 84.0F, 26.0F}, "SAVE",
      changed);
  ctx.pop_clip();

  // Last, so it only catches presses no control in the card wanted. A click on
  // the card's own background lands here too and deliberately does nothing;
  // only a click outside dismisses.
  const gui::Interaction scrim =
      ctx.grab(mix(id_, "preferences.scrim"), bounds);
  const bool dismissed =
      scrim.clicked && !card.contains(ctx.pointer_x(), ctx.pointer_y());

  if (saved) {
    result.save = draft_;
    if (password_edited_) {
      result.render_node_password = password_draft_;
    }
  }
  if (saved || cancelled || dismissed) {
    open_ = false;
    password_draft_.clear();
    password_edited_ = false;
  }
  return result;
}

OutputMonitorFrame OutputMonitorPanel::draw(gui::UiContext &ctx,
                                            const gui::Rect &bounds,
                                            const OutputMonitorState &state) {
  OutputMonitorFrame frame{};
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return frame;
  }
  const gui::Theme &theme = ctx.theme();
  ctx.push_clip(bounds);
  ctx.panel(bounds);

  using ClusterHealth = OutputMonitorState::Cluster::Health;
  using NodeStatus = OutputMonitorState::Node::Status;
  using ContentStatus = OutputMonitorState::Node::ContentStatus;
  using NodeSync = OutputMonitorState::Node::SyncStatus;

  const auto status_label = [](NodeStatus status) -> std::string_view {
    switch (status) {
    case NodeStatus::Disabled:
      return "CONTROL OFF";
    case NodeStatus::InvalidConfiguration:
      return "CONFIG ERROR";
    case NodeStatus::DuplicateNodeIndex:
      return "DUPLICATE INDEX";
    case NodeStatus::Probing:
      return "CHECKING";
    case NodeStatus::Stopped:
      return "STOPPED";
    case NodeStatus::Starting:
      return "STARTING";
    case NodeStatus::Running:
      return "LIVE";
    case NodeStatus::Stopping:
      return "STOPPING";
    case NodeStatus::Faulted:
      return "FAULTED";
    case NodeStatus::Unreachable:
      return "OFFLINE";
    case NodeStatus::AuthenticationFailed:
      return "AUTH ERROR";
    case NodeStatus::ProtocolError:
      return "PROTOCOL ERROR";
    case NodeStatus::ConfigurationMismatch:
      return "NODE MISMATCH";
    case NodeStatus::Busy:
      return "BUSY";
    }
    return "UNKNOWN";
  };
  const auto status_color = [&](NodeStatus status) {
    switch (status) {
    case NodeStatus::Running:
      return theme.good;
    case NodeStatus::Starting:
    case NodeStatus::Stopping:
    case NodeStatus::Probing:
    case NodeStatus::Busy:
      return theme.keyframe;
    case NodeStatus::Faulted:
    case NodeStatus::AuthenticationFailed:
    case NodeStatus::ProtocolError:
    case NodeStatus::ConfigurationMismatch:
    case NodeStatus::InvalidConfiguration:
    case NodeStatus::DuplicateNodeIndex:
      return theme.warning;
    case NodeStatus::Disabled:
    case NodeStatus::Stopped:
    case NodeStatus::Unreachable:
      return theme.text_muted;
    }
    return theme.text_muted;
  };
  const auto sync_label = [](NodeSync status) -> std::string_view {
    switch (status) {
    case NodeSync::Unsynced:
      return "UNSYNCED";
    case NodeSync::Unverified:
      return "UNVERIFIED";
    case NodeSync::SyncDeviceMissing:
      return "NO SYNC DEVICE";
    case NodeSync::ReferenceLost:
      return "REF LOST";
    case NodeSync::ReferenceNotInUse:
      return "REF UNUSED";
    case NodeSync::Unlocked:
      return "UNLOCKED";
    case NodeSync::BarrierIncomplete:
      return "BARRIER PARTIAL";
    case NodeSync::BarrierLost:
      return "BARRIER LOST";
    case NodeSync::ShowClockReferenceUnavailable:
      return "CLOCK REF N/A";
    case NodeSync::ShowClockReferenceLost:
      return "CLOCK REF LOST";
    case NodeSync::Simulated:
      return "SIMULATED";
    case NodeSync::Locked:
      return "LOCKED";
    }
    return "UNVERIFIED";
  };
  const auto sync_color = [&](NodeSync status) {
    switch (status) {
    case NodeSync::Locked:
      return theme.good;
    case NodeSync::Simulated:
    case NodeSync::Unverified:
      return theme.keyframe;
    case NodeSync::SyncDeviceMissing:
    case NodeSync::ReferenceLost:
    case NodeSync::ReferenceNotInUse:
    case NodeSync::Unlocked:
    case NodeSync::BarrierIncomplete:
    case NodeSync::BarrierLost:
    case NodeSync::ShowClockReferenceUnavailable:
    case NodeSync::ShowClockReferenceLost:
      return theme.warning;
    case NodeSync::Unsynced:
      return theme.text_muted;
    }
    return theme.text_muted;
  };
  const auto content_color = [&](ContentStatus status) {
    switch (status) {
    case ContentStatus::Ready:
      return theme.good;
    case ContentStatus::Checking:
      return theme.keyframe;
    case ContentStatus::Missing:
    case ContentStatus::Error:
      return theme.warning;
    case ContentStatus::NotRequired:
      return theme.text_muted;
    }
    return theme.text_muted;
  };
  const auto content_short = [](const OutputMonitorState::Node &node) {
    switch (node.content) {
    case ContentStatus::NotRequired:
      return std::string{};
    case ContentStatus::Checking:
      return std::string{"CHECKING"};
    case ContentStatus::Ready:
      return std::to_string(node.assets_ready) + "/" +
             std::to_string(node.assets_required);
    case ContentStatus::Missing:
      return "MISSING " +
             std::to_string(node.assets_missing + node.assets_mismatched);
    case ContentStatus::Error:
      return std::string{"VERIFY FAILED"};
    }
    return std::string{};
  };
  // One line of physical timing for a node, kept whole for its details and
  // its tooltip rather than run along the row.
  const auto node_sync_line = [&](const OutputMonitorState::Node &node) {
    std::string line{"Locked "};
    line += std::to_string(node.sync_locked_outputs) + "/" +
            std::to_string(node.sync_expected_outputs);
    line += "  Barrier " + std::to_string(node.barrier_joined_outputs) + "/" +
            std::to_string(node.barrier_expected_outputs);
    line += "  Present sync " + std::to_string(node.barrier_presents_in_sync) +
            "/" + std::to_string(node.barrier_presents);
    line += "  Show clock ";
    line += node.show_clock_reference;
    if (node.show_clock_reference == "LOCKED") {
      line += " #" + std::to_string(node.show_clock_reference_frame);
    }
    line += "  Missed " + std::to_string(node.missed_frames) + " late " +
            std::to_string(node.late_frames) + " held " +
            std::to_string(node.held_frames) + " repeated " +
            std::to_string(node.repeated_frames);
    if (node.loss_of_sync_events != 0U) {
      line += "  Sync loss " + std::to_string(node.loss_of_sync_events);
    }
    if (!node.sync_detail.empty()) {
      line += " - ";
      line += node.sync_detail;
    }
    return line;
  };

  // ------------------------------------------------------------ summary
  // The cluster at a glance: one chip per fact, coloured by how it stands,
  // each carrying its full wording in a tooltip. What used to be two run-on
  // lines of capitals is now something the eye reads in one pass.
  const gui::Rect summary{bounds.x, bounds.y, bounds.width, summary_height};
  ctx.panel(summary, theme.header);
  ctx.panel(gui::Rect{summary.x, summary.y + summary.height - 1.0F,
                      summary.width, 1.0F},
            theme.divider);
  const std::string frame_label = "Frame " + std::to_string(state.frame);
  const float frame_width = ctx.font_width(frame_label) + 14.0F;
  ctx.label(gui::Rect{summary.x + summary.width - frame_width, summary.y,
                      frame_width, summary.height},
            frame_label, theme.text_muted);
  float chip_x = summary.x + 8.0F;
  const float chip_limit = summary.x + summary.width - frame_width - 6.0F;
  const auto chip = [&](std::string_view text, gui::UiColor color,
                        std::string_view tip) {
    const float width = ctx.font_width(text) + 26.0F;
    if (chip_x + width > chip_limit) {
      return;
    }
    const gui::Rect body{chip_x, summary.y + 5.0F, width,
                         summary.height - 10.0F};
    ctx.panel(body, with_alpha(color, 0.10F));
    border(ctx, body, with_alpha(color, 0.40F));
    draw_status_dot(ctx, body.x + 10.0F, body.y + body.height * 0.5F, color,
                    3.0F);
    ctx.label(gui::Rect{body.x + 19.0F, body.y, width - 20.0F, body.height},
              text, theme.text);
    ctx.tooltip(body, tip);
    chip_x += width + 6.0F;
  };
  const bool cluster_visible =
      state.cluster.health != ClusterHealth::Unmonitored ||
      state.cluster.configured_nodes != 0U || state.cluster.renderer_active;
  if (!cluster_visible) {
    chip("Local only", theme.text_muted,
         "No render servers are configured; Program plays on this machine");
  } else {
    const gui::UiColor health_color =
        state.cluster.health == ClusterHealth::Healthy
            ? theme.good
            : (state.cluster.health == ClusterHealth::Faulted
                   ? theme.warning
                   : (state.cluster.health == ClusterHealth::Degraded
                          ? theme.keyframe
                          : theme.text_muted));
    const std::string_view health =
        state.cluster.health == ClusterHealth::Healthy    ? "Healthy"
        : state.cluster.health == ClusterHealth::Degraded ? "Degraded"
        : state.cluster.health == ClusterHealth::Faulted  ? "Fault"
                                                          : "Unmonitored";
    std::string control_tip{"Control "};
    control_tip += health;
    control_tip += " - " + std::to_string(state.cluster.live_sessions) +
                   " of " + std::to_string(state.cluster.configured_nodes) +
                   " render servers live";
    if (state.cluster.catching_up_sessions != 0U) {
      control_tip += ", " + std::to_string(state.cluster.catching_up_sessions) +
                     " catching up";
    }
    chip(std::string{health} + "  " +
             std::to_string(state.cluster.live_sessions) + "/" +
             std::to_string(state.cluster.configured_nodes) + " live",
         health_color, control_tip);
    // Next to the health chip so it is never the one squeezed off the row.
    if (state.cluster.control_stalled) {
      chip("Edits paused", theme.warning,
           "The Director has stopped sealing frames because the oldest one "
           "is still unfinished. Edits are not applied until it clears; "
           "the Director console names the frame and what it is waiting "
           "for.");
    }

    const bool timing_verified =
        state.cluster.sync_satisfied && !state.cluster.sync_degraded &&
        (!state.cluster.sync_required ||
         (state.cluster.external_time_sync_verified &&
          state.cluster.hardware_frame_lock_verified));
    std::string sync_tip{"Sync policy "};
    sync_tip += state.cluster.sync_mode;
    sync_tip += " - ";
    sync_tip += state.cluster.sync_headline;
    if (state.cluster.sync_required) {
      sync_tip += " - locked " +
                  std::to_string(state.cluster.sync_locked_outputs) + "/" +
                  std::to_string(state.cluster.sync_expected_outputs) +
                  ", barrier " +
                  std::to_string(state.cluster.barrier_joined_outputs) + "/" +
                  std::to_string(state.cluster.sync_expected_outputs);
      if (state.cluster.sync_groups != 0U) {
        sync_tip += ", " + std::to_string(state.cluster.sync_groups) + " groups";
      }
      if (state.cluster.sync_configuration_issues != 0U) {
        sync_tip += ", " +
                    std::to_string(state.cluster.sync_configuration_issues) +
                    " configuration issues";
      }
      if (state.cluster.sync_degraded) {
        sync_tip += ", promoted degraded";
      }
    }
    chip(std::string{"Sync "} + std::string{state.cluster.sync_mode},
         timing_verified ? theme.good
                         : (state.cluster.sync_required ? theme.warning
                                                        : theme.text_muted),
         sync_tip);

    if (state.cluster.renderer_active) {
      std::string renderer_tip{state.cluster.renderer_headline};
      if (state.cluster.conformance_difference.reason !=
          core::ConformanceMismatch::None) {
        renderer_tip += ": ";
        renderer_tip +=
            core::conformance_detail(state.cluster.conformance_difference);
      }
      std::string renderer_text;
      if (state.cluster.renderer_satisfied) {
        renderer_text = state.cluster.renderer_selected_advanced
                            ? "Advanced"
                            : "Basic";
        if (state.cluster.renderer_fallback) {
          renderer_text += " (fallback)";
        }
      } else {
        renderer_text = state.cluster.renderer_failure_local
                            ? "Renderer: editor"
                            : "Renderer: node " +
                                  std::to_string(
                                      state.cluster.renderer_failure_node);
      }
      chip(renderer_text,
           state.cluster.renderer_satisfied ? theme.good : theme.warning,
           renderer_tip);
    }
    if (state.cluster.release_lag_frames != 0U ||
        state.cluster.pacer_lag_frames != 0U) {
      const core::u64 lag = std::max(state.cluster.release_lag_frames,
                                     state.cluster.pacer_lag_frames);
      chip("Lag " + std::to_string(lag) + "f", theme.keyframe,
           "Release lag " + std::to_string(state.cluster.release_lag_frames) +
               " frames, pacer lag " +
               std::to_string(state.cluster.pacer_lag_frames) + " frames");
    }
    const core::u64 incidents =
        state.cluster.heartbeat_timeouts + state.cluster.session_failures +
        state.cluster.barrier_rejections + state.cluster.present_failures +
        state.cluster.lockstep_demotions;
    if (incidents != 0U) {
      chip(std::to_string(incidents) + " incidents", theme.warning,
           "Heartbeat timeouts " +
               std::to_string(state.cluster.heartbeat_timeouts) +
               ", session failures " +
               std::to_string(state.cluster.session_failures) +
               ", barrier rejections " +
               std::to_string(state.cluster.barrier_rejections) +
               ", present failures " +
               std::to_string(state.cluster.present_failures) +
               ", nodes dropped from lockstep for lagging " +
               std::to_string(state.cluster.lockstep_demotions));
    }
    if (state.cluster.diagnostics_recording) {
      chip("Recording", theme.warning, "Diagnostics log is recording");
    }
    if (state.cluster.control_unsecured && state.cluster.configured_nodes != 0U) {
      chip("No password", theme.warning,
           "Render machines are controlled without a password: any Media "
           "Server on this network can start and stop them. Set a render "
           "node password in Preferences, and give each render machine "
           "the same one in its setup window.");
    }
  }

  // ------------------------------------------------------------- columns
  const bool any_assets = std::any_of(
      state.nodes.begin(), state.nodes.end(),
      [](const OutputMonitorState::Node &node) {
        return node.content != ContentStatus::NotRequired;
      });
  constexpr float twirl_width = 22.0F;
  constexpr float resolution_width = 100.0F;
  constexpr float fps_width = 40.0F;
  constexpr float status_width = 100.0F;
  constexpr float assets_width = 96.0F;
  constexpr float sync_width = 108.0F;
  constexpr float actions_width = 62.0F;
  constexpr float minimum_name = 120.0F;
  float available = bounds.width - twirl_width - actions_width - 12.0F;
  const auto fits = [&](float width) {
    if (available - width >= minimum_name) {
      available -= width;
      return true;
    }
    return false;
  };
  const bool show_status = fits(status_width);
  const bool show_resolution = fits(resolution_width);
  const bool show_fps = fits(fps_width);
  const bool show_assets = any_assets && fits(assets_width);
  const bool show_sync = state.cluster.sync_required && fits(sync_width);
  float column_right = bounds.x + bounds.width - 6.0F - actions_width;
  const auto place = [&](bool shown, float width) {
    if (!shown) {
      return 0.0F;
    }
    column_right -= width;
    return column_right;
  };
  const float sync_x = place(show_sync, sync_width);
  const float assets_x = place(show_assets, assets_width);
  const float status_x = place(show_status, status_width);
  const float fps_x = place(show_fps, fps_width);
  const float resolution_x = place(show_resolution, resolution_width);
  const float name_x = bounds.x + twirl_width + 4.0F;
  const float name_width = std::max(column_right - name_x - 8.0F, 0.0F);
  const float actions_x = bounds.x + bounds.width - 6.0F - actions_width;

  const gui::Rect header{bounds.x, summary.y + summary.height, bounds.width,
                         header_height};
  ctx.panel(header, theme.panel);
  ctx.panel(gui::Rect{header.x, header.y + header.height - 1.0F, header.width,
                      1.0F},
            theme.divider);
  const auto heading = [&](bool shown, float x, float width,
                           std::string_view text) {
    if (shown) {
      ctx.label(gui::Rect{x, header.y, width, header.height}, text,
                theme.text_muted);
    }
  };
  heading(true, name_x, name_width, "OUTPUT");
  heading(show_resolution, resolution_x, resolution_width, "RESOLUTION");
  heading(show_fps, fps_x, fps_width, "FPS");
  heading(show_status, status_x, status_width, "STATUS");
  heading(show_assets, assets_x, assets_width, "ASSETS");
  heading(show_sync, sync_x, sync_width, "SYNC");

  // ----------------------------------------------------- row geometry
  // Details open under a row when it is clicked, so the evidence an operator
  // needs for one output never has to share a line with another's.
  const auto node_details = [&](const OutputMonitorState::Node &node) {
    std::vector<std::pair<std::string, std::string>> lines;
    lines.emplace_back("Endpoint", node.endpoint.empty() ? std::string{"-"}
                                                         : node.endpoint);
    std::string status{status_label(node.status)};
    if (!node.detail.empty()) {
      status += " - " + node.detail;
    }
    lines.emplace_back("Status", std::move(status));
    if (node.status == NodeStatus::Running) {
      std::string connection{output_node_connection_label(node)};
      if (node.connection == OutputMonitorState::Node::Connection::Disconnected) {
        connection += " - waiting for the node to connect automatically; "
                      "check its Director address and show ID if this persists";
      } else {
        connection += " - frame " + std::to_string(node.committed_frame) +
                      " / " + std::to_string(node.target_frame);
      }
      lines.emplace_back("Editor connection", std::move(connection));
    }
    // Above Assets deliberately. A node whose displays failed reports nothing
    // for its renderer and its assets, so those rows read like a slow load;
    // the operator should meet the actual reason first.
    if (node.output_failed()) {
      lines.emplace_back(
          "Output", std::string{output_node_deployment_label(node)} +
                        " - this node is not driving its displays");
    }
    if (node.content != ContentStatus::NotRequired) {
      std::string content = output_node_content_label(node);
      if (!node.content_detail.empty()) {
        content += " - " + node.content_detail;
      }
      lines.emplace_back("Assets", std::move(content));
    }
    if (state.cluster.sync_required) {
      lines.emplace_back(std::string{"Sync "} +
                             std::string{sync_label(node.sync)},
                         node_sync_line(node));
    }
    return lines;
  };
  constexpr float detail_line = 20.0F;
  const float timing_lines = state.cluster.conformance_active ? 3.0F : 0.0F;
  const auto node_height = [&](const OutputMonitorState::Node &node) {
    if (!expanded_.contains(node.device)) {
      return row_height;
    }
    return row_height + 8.0F +
           detail_line * (static_cast<float>(node_details(node).size()) +
                          timing_lines);
  };
  const float program_details_height =
      program_expanded_ && state.cluster.conformance_active
          ? 8.0F + 24.0F + (timing_expanded_ ? 2.0F + 1.0F +
                                                   static_cast<float>(
                                                       core::render_pass_count)
                                             : 2.0F) *
                                detail_line +
                24.0F + detail_line +
                (conformance_expanded_
                     ? detail_line * (4.0F + static_cast<float>(
                                                 core::conformance_pass_count))
                     : 0.0F)
          : 0.0F;
  const float program_height = row_height + program_details_height;

  const gui::Rect list_bounds{
      bounds.x, header.y + header.height, bounds.width,
      std::max(bounds.y + bounds.height - footer_height -
                   (header.y + header.height),
               0.0F)};
  float content_height = program_height;
  for (const OutputMonitorState::Node &node : state.nodes) {
    content_height += node_height(node);
  }
  const float maximum_scroll =
      std::max(content_height - list_bounds.height, 0.0F);
  if (list_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
    scroll_ -= ctx.wheel() * row_height * 2.0F;
  }
  scroll_ = std::clamp(scroll_, 0.0F, maximum_scroll);

  const auto status_pill = [&](float x, float centre_y, std::string_view text,
                               gui::UiColor color, bool filled,
                               std::string_view tip) {
    const gui::Rect pill = draw_pill(ctx, x, centre_y, text, color, filled);
    ctx.tooltip(pill, tip);
  };
  const auto row_cells = [&](const gui::Rect &row, std::string_view name,
                             std::string_view tag, std::string_view resolution,
                             std::string_view fps) {
    const float centre_y = row.y + row_height * 0.5F;
    const float tag_width = tag.empty() ? 0.0F : pill_width(ctx, tag) + 8.0F;
    const float name_text = std::min(ctx.font_width(name),
                                     std::max(name_width - tag_width, 0.0F));
    ctx.label(gui::Rect{name_x, row.y, name_text + 2.0F, row_height}, name,
              theme.text);
    if (!tag.empty() && name_width > tag_width + 40.0F) {
      static_cast<void>(draw_pill(ctx, name_x + name_text + 8.0F, centre_y,
                                  tag, theme.text_muted, false));
    }
    if (show_resolution) {
      ctx.label(gui::Rect{resolution_x, row.y, resolution_width, row_height},
                resolution, theme.text_muted);
    }
    if (show_fps) {
      ctx.label(gui::Rect{fps_x, row.y, fps_width, row_height}, fps,
                theme.text_muted);
    }
  };

  float y = list_bounds.y - scroll_;
  ctx.push_clip(list_bounds);

  // ---------------------------------------------------------- Program
  const std::string program_resolution =
      resolution_text(state.width, state.height);
  const std::string program_fps = fps_text(state.frames_per_second);
  {
    const gui::Rect row{bounds.x, y, bounds.width, program_height};
    const gui::Rect line{bounds.x, y, bounds.width, row_height};
    ctx.panel(row, theme.row);
    ctx.panel(gui::Rect{row.x, row.y + row.height - 1.0F, row.width, 1.0F},
              theme.divider);
    const float centre_y = line.y + row_height * 0.5F;
    if (state.cluster.conformance_active) {
      draw_twirl(ctx, bounds.x + 12.0F, centre_y, program_expanded_,
                 theme.text_muted);
    }
    row_cells(line, "Program", "PREVIEW", program_resolution, program_fps);
    if (show_status) {
      status_pill(status_x, centre_y, state.playing ? "PLAYING" : "PAUSED",
                  state.playing ? theme.good : theme.text_muted, state.playing,
                  state.playing ? "Program is playing" : "Program is paused");
    }
    if (state.cluster.conformance_active) {
      if (icon_button(ctx, mix(id_, "conformance.check"),
                      gui::Rect{actions_x + actions_width - 28.0F,
                                centre_y - 12.0F, 26.0F, 24.0F},
                      gui::Icon::Restart,
                      "Check the render cohort: every node renders the same "
                      "fixed scene and compares")) {
        frame.check_render_conformance = true;
      }
      const gui::Interaction toggle =
          clipped_grab(ctx, mix(id_, "program.details"),
                       gui::Rect{bounds.x, line.y,
                                 actions_x - bounds.x, row_height},
                       list_bounds);
      if (toggle.clicked) {
        program_expanded_ = !program_expanded_;
      }
    }
    if (program_details_height > 0.0F) {
      float dy = line.y + row_height + 4.0F;
      const float dx = name_x;
      const float dw = std::max(row.width - (dx - row.x) - 12.0F, 0.0F);
      if (ctx.button(mix(id_, "timing.details"),
                     gui::Rect{dx, dy, 160.0F, 20.0F},
                     timing_expanded_ ? "Hide pass costs" : "Pass costs")) {
        timing_expanded_ = !timing_expanded_;
      }
      dy += 24.0F;
      const core::FrameTiming &t = state.cluster.frame_timing;
      std::array<char, 256U> text{};
      if (t.sequence == 0U) {
        ctx.label(gui::Rect{dx, dy, dw, detail_line},
                  "Frame timing awaiting a completed submission",
                  theme.text_muted);
        dy += detail_line * 2.0F;
      } else {
        std::snprintf(text.data(), text.size(),
                      "CPU record %.3f ms   GPU %s%.3f ms   Upload %.2f MiB",
                      static_cast<double>(t.cpu_record_ns) / 1e6,
                      t.gpu_valid ? "" : "unavailable ",
                      static_cast<double>(t.gpu_ns) / 1e6,
                      static_cast<double>(t.upload_bytes) / 1048576.0);
        ctx.label(gui::Rect{dx, dy, dw, detail_line}, text.data(),
                  t.over_budget(state.cluster.frame_budget_us) ? theme.warning
                                                                : theme.text);
        dy += detail_line;
        std::snprintf(
            text.data(), text.size(),
            "Present %.3f ms   Barrier %llu/%llu presents, %llu flips   "
            "Budget %.3f ms%s",
            static_cast<double>(t.present_ns) / 1e6,
            static_cast<unsigned long long>(t.presents_in_sync),
            static_cast<unsigned long long>(t.presents),
            static_cast<unsigned long long>(t.flips_in_sync),
            static_cast<double>(state.cluster.frame_budget_us) / 1000.0,
            state.cluster.frame_budget_us == 0U ? " (report only)" : "");
        ctx.label(gui::Rect{dx, dy, dw, detail_line}, text.data(),
                  theme.text_muted);
        dy += detail_line;
      }
      if (timing_expanded_) {
        std::snprintf(text.data(), text.size(),
                      "Completed submission %llu   Dropped intervals %llu",
                      static_cast<unsigned long long>(t.sequence),
                      static_cast<unsigned long long>(t.query_overflows));
        ctx.label(gui::Rect{dx, dy, dw, detail_line}, text.data(),
                  theme.text_muted);
        dy += detail_line;
        for (std::size_t i = 0U; i < core::render_pass_count; ++i) {
          std::snprintf(text.data(), text.size(), "%.*s  %.3f ms",
                        static_cast<int>(core::render_pass_names[i].size()),
                        core::render_pass_names[i].data(),
                        static_cast<double>(t.pass_ns[i]) / 1e6);
          ctx.label(gui::Rect{dx + 12.0F, dy, dw - 12.0F, detail_line},
                    text.data(),
                    t.pass_ns[i] != 0U ? theme.text : theme.text_muted);
          dy += detail_line;
        }
      }
      if (ctx.button(mix(id_, "conformance.details"),
                     gui::Rect{dx, dy, 160.0F, 20.0F},
                     conformance_expanded_ ? "Hide pass digests"
                                           : "Pass digests")) {
        conformance_expanded_ = !conformance_expanded_;
      }
      dy += 24.0F;
      const auto &proof = state.cluster.conformance;
      std::string proof_summary;
      switch (proof.state) {
      case core::ConformanceState::Unknown:
        proof_summary = "Render check awaiting a Program device";
        break;
      case core::ConformanceState::Running:
        proof_summary = "Rendering the fixed conformance scene on this machine";
        break;
      case core::ConformanceState::Failed:
        proof_summary = "Local render check failed: ";
        proof_summary += core::conformance_detail(
            {core::ConformanceMismatch::Failed, proof.failed_pass});
        break;
      case core::ConformanceState::Passed:
        proof_summary = "Local render check passed - ";
        proof_summary += proof.strict_float ? "IEEE strict" : "experimental relaxed";
        break;
      }
      ctx.label(gui::Rect{dx, dy, dw, detail_line}, proof_summary,
                theme.text_muted);
      dy += detail_line;
      if (conformance_expanded_) {
        const auto version = proof.adapter.driver_version;
        ctx.label(gui::Rect{dx, dy, dw, detail_line},
                  proof.adapter.name.data(), theme.text_muted);
        dy += detail_line;
        ctx.label(gui::Rect{dx, dy, dw, detail_line},
                  "Driver " + std::to_string(version >> 48U) + "." +
                      std::to_string((version >> 32U) & 65535U) + "." +
                      std::to_string((version >> 16U) & 65535U) + "." +
                      std::to_string(version & 65535U),
                  theme.text_muted);
        dy += detail_line;
        constexpr char hex[] = "0123456789abcdef";
        std::string bundle;
        for (auto byte : proof.shader_bundle) {
          bundle += hex[byte >> 4U];
          bundle += hex[byte & 15U];
        }
        ctx.label(gui::Rect{dx, dy, dw, detail_line}, "Shader SHA-256 " + bundle,
                  theme.text_muted);
        dy += detail_line * 2.0F;
        for (std::size_t i = 0; i < core::conformance_pass_count; ++i) {
          std::string pass{core::conformance_pass_names[i]};
          pass += "  ";
          for (int shift = 60; shift >= 0; shift -= 4) {
            pass += hex[(proof.passes[i] >> shift) & 15U];
          }
          const bool differs =
              static_cast<std::size_t>(
                  state.cluster.conformance_difference.pass) == i;
          if (differs) {
            pass += "  first difference";
          }
          ctx.label(gui::Rect{dx + 12.0F, dy, dw - 12.0F, detail_line}, pass,
                    differs ? theme.warning : theme.text_muted);
          dy += detail_line;
        }
      }
    }
    y += program_height;
  }

  // ------------------------------------------------------- render servers
  for (std::size_t index = 0U; index < state.nodes.size(); ++index) {
    const OutputMonitorState::Node &node = state.nodes[index];
    const float height = node_height(node);
    const gui::Rect row{bounds.x, y, bounds.width, height};
    const gui::Rect line{bounds.x, y, bounds.width, row_height};
    y += height;
    if (row.y + row.height <= list_bounds.y ||
        row.y >= list_bounds.y + list_bounds.height) {
      continue;
    }
    const bool expanded = expanded_.contains(node.device);
    const bool hovered = !ctx.pointer_blocked() &&
                         line.contains(ctx.pointer_x(), ctx.pointer_y()) &&
                         list_bounds.contains(ctx.pointer_x(), ctx.pointer_y());
    ctx.panel(row, index % 2U == 0U ? theme.row_alt : theme.row);
    if (hovered) {
      ctx.panel(line, with_alpha(theme.widget_hover, 0.5F));
    }
    ctx.panel(gui::Rect{row.x, row.y + row.height - 1.0F, row.width, 1.0F},
              theme.divider);
    const float centre_y = line.y + row_height * 0.5F;
    draw_twirl(ctx, bounds.x + 12.0F, centre_y, expanded,
               hovered ? theme.text : theme.text_muted);
    const std::string identity = feed_output_identity(node.node_index);
    row_cells(line, node.name.empty() ? identity : node.name, identity,
              resolution_text(node.width, node.height), program_fps);

    const bool awaiting_connection = node.status == NodeStatus::Running &&
        node.connection != OutputMonitorState::Node::Connection::Live;
    // A live session whose outputs never opened is the case this row used to
    // read as healthy: the process is running and connected, and nothing it
    // reports afterwards is true of a display. Say OUTPUT FAILED instead, and
    // put the reason in the tooltip rather than only in the expanded detail.
    const bool output_failed = node.output_failed();
    const gui::UiColor node_color = output_failed ? theme.warning
        : awaiting_connection ? theme.warning
                              : status_color(node.status);
    std::string status_tip{status_label(node.status)};
    if (!node.detail.empty()) {
      status_tip += " - " + node.detail;
    }
    const auto displayed_status = output_failed
        ? std::string_view{"OUTPUT FAILED"}
        : node.status == NodeStatus::Running
            ? output_node_connection_label(node)
            : status_label(node.status);
    if (node.status == NodeStatus::Running) {
      status_tip += " - ";
      status_tip += displayed_status;
    }
    if (output_failed) {
      status_tip += " - ";
      status_tip += output_node_deployment_label(node);
    }
    if (show_status) {
      const bool faulted = node.status == NodeStatus::Faulted ||
                           node.status == NodeStatus::AuthenticationFailed ||
                           node.status == NodeStatus::ProtocolError ||
                           node.status == NodeStatus::ConfigurationMismatch ||
                           node.status == NodeStatus::InvalidConfiguration ||
                           node.status == NodeStatus::DuplicateNodeIndex;
      // Lit when live or broken - the two states worth seeing from across
      // the room - and outlined otherwise.
      status_pill(status_x, centre_y, displayed_status, node_color,
                  node.status == NodeStatus::Running || faulted, status_tip);
    }
    if (show_assets && node.content != ContentStatus::NotRequired) {
      std::string tip = output_node_content_label(node);
      if (!node.content_detail.empty()) {
        tip += " - " + node.content_detail;
      }
      status_pill(assets_x, centre_y, content_short(node),
                  content_color(node.content), false, tip);
    }
    if (show_sync) {
      status_pill(sync_x, centre_y, sync_label(node.sync),
                  sync_color(node.sync), false, node_sync_line(node));
    }

    // Lifecycle actions: the same icon in the same place on every row.
    const bool row_visible = intersect(line, list_bounds).height > 0.0F;
    float button_x = actions_x + actions_width;
    const auto action = [&](std::string_view suffix, gui::Icon icon,
                            std::string_view tip, OutputLifecycleAction kind,
                            bool enabled) {
      button_x -= 28.0F;
      const gui::Rect button{button_x, centre_y - 12.0F, 26.0F, 24.0F};
      const gui::UiId button_id = mix(
          id_, suffix, node.device.value() * 8U + static_cast<core::u64>(kind));
      if (row_visible &&
          icon_button(ctx, button_id, button, icon, tip, false, enabled) &&
          !frame.lifecycle.has_value()) {
        frame.lifecycle = OutputLifecycleRequest{node.device, node.node_index,
                                                 node.endpoint, kind};
      }
    };
    switch (node.status) {
    case NodeStatus::Stopped:
    case NodeStatus::Faulted:
      action("output.lifecycle.start", gui::Icon::Play,
             node.content_ready() ? "Start this output"
                                  : "Start - waiting for its assets",
             OutputLifecycleAction::Start, node.content_ready());
      break;
    case NodeStatus::Running:
      action("output.lifecycle.stop", gui::Icon::Stop, "Stop this output",
             OutputLifecycleAction::Stop, true);
      action("output.lifecycle.restart", gui::Icon::Restart,
             "Restart this output", OutputLifecycleAction::Restart,
             node.content_ready());
      break;
    case NodeStatus::Unreachable:
    case NodeStatus::AuthenticationFailed:
    case NodeStatus::ProtocolError:
    case NodeStatus::ConfigurationMismatch:
      action("output.lifecycle.status", gui::Icon::Restart,
             "Try to reach this output again", OutputLifecycleAction::Status,
             true);
      break;
    case NodeStatus::Disabled:
    case NodeStatus::InvalidConfiguration:
    case NodeStatus::DuplicateNodeIndex:
    case NodeStatus::Probing:
    case NodeStatus::Starting:
    case NodeStatus::Stopping:
    case NodeStatus::Busy:
      break;
    }
    // The rest of the row opens its details; drawn after the buttons so they
    // keep their presses.
    const gui::Interaction toggle = clipped_grab(
        ctx, mix(id_, "output.details", node.device.value()),
        gui::Rect{line.x, line.y, std::max(button_x - line.x - 2.0F, 0.0F),
                  row_height},
        list_bounds);
    if (toggle.clicked) {
      if (expanded) {
        expanded_.erase(node.device);
      } else {
        expanded_.insert(node.device);
      }
    }
    if (expanded) {
      float dy = line.y + row_height + 2.0F;
      const float dw = std::max(row.width - (name_x - row.x) - 12.0F, 0.0F);
      for (const auto &[key, value] : node_details(node)) {
        ctx.label(gui::Rect{name_x, dy, 110.0F, detail_line}, key,
                  theme.text_muted);
        ctx.label(gui::Rect{name_x + 116.0F, dy, std::max(dw - 116.0F, 0.0F),
                            detail_line},
                  value, theme.text);
        dy += detail_line;
      }
      if (state.cluster.conformance_active) {
        const core::FrameTiming &t = node.frame_timing;
        std::array<char, 256U> text{};
        std::snprintf(text.data(), text.size(),
                      "CPU %.3f ms   GPU %s%.3f ms   Present %.3f ms",
                      static_cast<double>(t.cpu_record_ns) / 1e6,
                      t.gpu_valid ? "" : "unavailable ",
                      static_cast<double>(t.gpu_ns) / 1e6,
                      static_cast<double>(t.present_ns) / 1e6);
        ctx.label(gui::Rect{name_x, dy, 110.0F, detail_line}, "Frame timing",
                  theme.text_muted);
        ctx.label(gui::Rect{name_x + 116.0F, dy, std::max(dw - 116.0F, 0.0F),
                            detail_line},
                  t.sequence == 0U ? std::string_view{"awaiting a submission"}
                                   : std::string_view{text.data()},
                  theme.text);
        dy += detail_line;
        const auto difference = core::compare_render_conformance(
            state.cluster.conformance, node.conformance);
        std::string check =
            difference.reason != core::ConformanceMismatch::None
                ? std::string{core::conformance_detail(difference)}
            : difference.adapter_differs
                ? std::string{"matches on another GPU / driver"}
                : std::string{"matches"};
        ctx.label(gui::Rect{name_x, dy, 110.0F, detail_line}, "Render check",
                  theme.text_muted);
        ctx.label(gui::Rect{name_x + 116.0F, dy, std::max(dw - 116.0F, 0.0F),
                            detail_line},
                  check,
                  difference.reason == core::ConformanceMismatch::None
                      ? theme.good
                      : theme.warning);
        dy += detail_line;
        ctx.label(gui::Rect{name_x, dy, 110.0F, detail_line}, "Adapter",
                  theme.text_muted);
        ctx.label(gui::Rect{name_x + 116.0F, dy, std::max(dw - 116.0F, 0.0F),
                            detail_line},
                  node.conformance.adapter.name.data(), theme.text_muted);
      }
    }
  }
  if (state.nodes.empty() && list_bounds.height > row_height * 2.0F) {
    ctx.label(gui::Rect{name_x, y + 6.0F, name_width + 200.0F, detail_line},
              "Add and cable a Render Server in Hardware Map to drive "
              "physical outputs",
              theme.text_muted);
  }
  ctx.pop_clip();

  if (maximum_scroll > 0.0F && list_bounds.height > 8.0F) {
    const float thumb_height = std::max(
        list_bounds.height * list_bounds.height / content_height, 16.0F);
    const float thumb_range = std::max(list_bounds.height - thumb_height, 0.0F);
    const float thumb_y =
        list_bounds.y + thumb_range * (scroll_ / maximum_scroll);
    ctx.panel(gui::Rect{list_bounds.x + list_bounds.width - 4.0F, thumb_y, 3.0F,
                        thumb_height},
              theme.widget_hover);
  }

  // -------------------------------------------------------------- footer
  const gui::Rect footer{bounds.x, bounds.y + bounds.height - footer_height,
                         bounds.width, footer_height};
  ctx.panel(footer, theme.header);
  ctx.panel(gui::Rect{footer.x, footer.y, footer.width, 1.0F}, theme.divider);
  const auto actionable = [&](OutputLifecycleAction action) {
    return static_cast<std::size_t>(std::count_if(
        state.nodes.begin(), state.nodes.end(), [action](const auto &node) {
          const bool configured = node.status != NodeStatus::Disabled &&
                                  node.status != NodeStatus::InvalidConfiguration &&
                                  node.status != NodeStatus::DuplicateNodeIndex;
          return configured && (action == OutputLifecycleAction::Stop ||
                                node.content_ready());
        }));
  };
  float batch_right = footer.x + footer.width - 6.0F;
  const auto batch_button = [&](std::string_view suffix, std::string_view label,
                                OutputLifecycleAction action, bool caution) {
    const float width = ctx.font_width(label) + 24.0F;
    batch_right -= width;
    const gui::Rect button{batch_right, footer.y + 4.0F, width,
                           footer.height - 8.0F};
    batch_right -= 6.0F;
    const bool enabled = actionable(action) > 0U;
    const gui::Interaction interaction =
        enabled ? ctx.grab(mix(id_, suffix), button) : gui::Interaction{};
    const gui::UiColor base =
        caution ? with_alpha(theme.warning, 0.16F) : theme.widget;
    ctx.panel(button, !enabled ? with_alpha(theme.widget, 0.5F)
                               : (interaction.held && !interaction.released
                                      ? theme.widget_active
                                      : (interaction.hovered
                                             ? (caution ? with_alpha(
                                                              theme.warning,
                                                              0.30F)
                                                        : theme.widget_hover)
                                             : base)));
    border(ctx, button,
           !enabled ? theme.divider
                    : (caution ? with_alpha(theme.warning, 0.7F) : theme.border));
    const float text_width = ctx.font_width(label);
    ctx.label(gui::Rect{button.x + (button.width - text_width) * 0.5F, button.y,
                        text_width + 2.0F, button.height},
              label,
              !enabled ? with_alpha(theme.text_muted, 0.6F)
                       : (caution ? theme.warning : theme.text));
    if (enabled && interaction.clicked && !frame.lifecycle.has_value()) {
      frame.lifecycle = OutputLifecycleRequest{{}, 0U, {}, action, true};
    }
  };
  if (!state.nodes.empty()) {
    batch_button("output.lifecycle.stop-all", "Stop all",
                 OutputLifecycleAction::Stop, true);
    batch_button("output.lifecycle.restart-all", "Restart all",
                 OutputLifecycleAction::Restart, false);
    batch_button("output.lifecycle.start-all", "Start all",
                 OutputLifecycleAction::Start, false);
  }
  const auto live = static_cast<std::size_t>(std::count_if(
      state.nodes.begin(), state.nodes.end(), [](const auto &node) {
        return node.status == NodeStatus::Running &&
            node.connection == OutputMonitorState::Node::Connection::Live;
      }));
  const std::string count =
      state.nodes.empty()
          ? std::string{"Program only"}
          : std::to_string(state.nodes.size()) +
                (state.nodes.size() == 1U ? " render server, "
                                          : " render servers, ") +
                std::to_string(live) + " live";
  ctx.label(gui::Rect{footer.x + 10.0F, footer.y,
                      std::max(batch_right - footer.x - 14.0F, 0.0F),
                      footer.height},
            count, theme.text_muted);
  ctx.pop_clip();
  return frame;
}

StageAssetStatus stage_asset_status(const render::RenderStats& geometry,
                                   core::u64 pending_media,
                                   render::DisplayLutState display) {
  using render::TextureStreamState;
  const auto failed = [](std::string detail) {
    return StageAssetStatus{false, "STAGE ASSET LOAD FAILED", std::move(detail)};
  };
  const auto loading = [](std::string detail) {
    return StageAssetStatus{true, "LOADING STAGE ASSETS...", std::move(detail)};
  };
  if (geometry.models_failed != 0U)
    return failed("A model could not load. Check its source path, then Rescan in Library.");
  if (geometry.model_integration_refusals != 0U)
    return failed("Model GPU resources are unavailable. Check Renderer diagnostics.");
  switch (geometry.texture_streaming.state) {
  case TextureStreamState::CapacityRefused:
    return failed("Material textures exceed the GPU texture budget. Check Renderer settings.");
  case TextureStreamState::AllocationFailed:
    return failed("GPU memory for material textures could not be allocated.");
  case TextureStreamState::UploadFailed:
    return failed("Material textures could not be uploaded to the GPU.");
  default: break;
  }
  if (!geometry.baked_ready && geometry.baked_state == render::ProbeCacheState::Failed)
    return failed("Baked lighting could not load. Check its path or rebake the scene.");
  if (display == render::DisplayLutState::Failed)
    return failed("The Stage display color transform could not load.");
  if (geometry.models_pending != 0U || geometry.models_stale != 0U)
    return loading("Preparing models: " +
        std::to_string(static_cast<core::u64>(geometry.models_pending) + geometry.models_stale) + " pending.");
  if (geometry.texture_streaming.state != TextureStreamState::Ready)
    return loading("Preparing material textures on the GPU.");
  if (!geometry.baked_ready)
    return loading("Loading baked lighting.");
  if (pending_media != 0U)
    return loading("Loading media: " + std::to_string(pending_media) + " pending.");
  if (display != render::DisplayLutState::Ready)
    return loading("Preparing the Stage display color transform.");
  if (!geometry.assets.ready)
    return loading("Preparing scene assets and GPU resources.");
  return {};
}

ProgramAssetStatus
program_asset_status(core::AssetSetReadiness local, core::AssetCommit permit,
                     core::u64 control_frame, bool required,
                     const OutputMonitorState::Cluster &cluster,
                     std::string_view loading_detail) {
  if (!required || (local.ready && local.digest == 0))
    return {};
  const bool local_visible = local.ready && local.visible_commit != 0;
  if (local_visible) {
    if (cluster.asset_cohort.members <= 1 || cluster.asset_cohort.visible)
      return {};
  }
  if (!local.ready)
    return {true, "PREPARING PROGRAM", std::string{loading_detail}, true};
  if (cluster.asset_waiting_node)
    return {true,
            "WAITING FOR OUTPUT NODE " +
                std::to_string(*cluster.asset_waiting_node),
            std::to_string(cluster.asset_cohort.ready_members) + "/" +
                std::to_string(cluster.asset_cohort.members) +
                " renderers ready. Check Output Monitor and Hardware Map.",
            true};
  if (permit.digest == local.digest && permit.control_frame != 0) {
    if (permit.control_frame >= control_frame) {
      const bool retry =
          local_visible && permit.control_frame > local.visible_commit;
      return {true, retry ? "RETRYING OUTPUT UPDATE" : "UPDATING PROGRAM",
              "Assets ready. Switching in " +
                  std::to_string(permit.control_frame - control_frame) +
                  " control frames.",
              !retry};
    }
    if (local_visible && !core::asset_commit_confirmation_expired(
                             permit.control_frame, control_frame))
      return {true, "CONFIRMING OUTPUT UPDATE",
              "Program updated. Waiting for all output nodes to confirm the "
              "same update.",
              true};
    return {true, local_visible ? "OUTPUT ACTIVATION MISSED"
                               : "PROGRAM ACTIVATION MISSED",
            "Waiting for activation to be retried. Renderers that missed "
            "activation hold their last complete image."};
  }
  return {true, "WAITING TO UPDATE PROGRAM",
          "Assets ready. Waiting for synchronized activation.", true};
}

} // namespace mediaserver::editor
