#include <mediaserver/editor/workspace_panels.hpp>

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/editor/mapping_panels.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <tinyfiledialogs.h>

namespace mediaserver::editor {
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

void draw_twirl(gui::UiContext &ctx, float centre_x, float centre_y, bool open,
                gui::UiColor color) {
  auto &draw = ctx.draw_list();
  if (open) {
    draw.add_triangle(centre_x - 3.5F, centre_y - 2.0F, centre_x + 3.5F,
                      centre_y - 2.0F, centre_x, centre_y + 2.5F, color);
    return;
  }
  draw.add_triangle(centre_x - 2.0F, centre_y - 3.5F, centre_x + 2.5F, centre_y,
                    centre_x - 2.0F, centre_y + 3.5F, color);
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
  if (!element.display_name.empty()) {
    return element.display_name;
  }
  std::array<char, 64U> output{};
  const std::string_view kind = kind_name(element.kind);
  const int length =
      std::snprintf(output.data(), output.size(), "%.*s_%03llu",
                    static_cast<int>(kind.size()), kind.data(),
                    static_cast<unsigned long long>(element.node.value()));
  return std::string{output.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
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

enum class SceneGroup : core::u8 {
  Visuals,
  Media,
  Lights,
  Groups,
};

[[nodiscard]] SceneGroup group_for(ElementKind kind) noexcept {
  if (kind == ElementKind::Media) {
    return SceneGroup::Media;
  }
  if (is_light(kind)) {
    return SceneGroup::Lights;
  }
  if (kind == ElementKind::Group) {
    return SceneGroup::Groups;
  }
  return SceneGroup::Visuals;
}

constexpr std::array<std::string_view, 4U> scene_group_names{
    "VISUALS", "MEDIA", "LIGHTS", "GROUPS"};

[[nodiscard]] std::size_t group_index(SceneGroup group) noexcept {
  return static_cast<std::size_t>(group);
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

void draw_scene_kind_icon(gui::UiContext &ctx, const ElementInfo &element,
                          float x, float y) {
  const gui::UiColor color = kind_color(element.kind);
  auto &draw = ctx.draw_list();
  if (is_light(element.kind)) {
    draw_circle(ctx, x + 5.0F, y + 5.0F, 3.0F, color);
    for (std::size_t ray = 0U; ray < 4U; ++ray) {
      constexpr std::array<gui::UiPoint, 4U> directions{
          {{0.0F, -1.0F}, {1.0F, 0.0F}, {0.0F, 1.0F}, {-1.0F, 0.0F}}};
      const gui::UiPoint direction = directions[ray];
      draw.add_line(x + 5.0F + direction.x * 4.5F,
                    y + 5.0F + direction.y * 4.5F,
                    x + 5.0F + direction.x * 6.5F,
                    y + 5.0F + direction.y * 6.5F, 1.0F, color);
    }
    return;
  }
  if (element.kind == ElementKind::Media) {
    draw.add_rect(x, y + 1.0F, 10.0F, 8.0F, with_alpha(color, 0.28F));
    border(ctx, gui::Rect{x, y + 1.0F, 10.0F, 8.0F}, color);
    draw.add_triangle(x + 3.0F, y + 3.0F, x + 3.0F, y + 7.0F, x + 7.0F,
                      y + 5.0F, color);
    return;
  }
  if (element.kind == ElementKind::Group) {
    draw.add_rect(x, y + 2.0F, 10.0F, 7.0F, with_alpha(color, 0.22F));
    border(ctx, gui::Rect{x, y + 2.0F, 10.0F, 7.0F}, color);
    draw.add_rect(x + 1.0F, y, 5.0F, 3.0F, with_alpha(color, 0.32F));
    return;
  }
  draw.add_rect(x + 1.0F, y + 1.0F, 8.0F, 8.0F, with_alpha(color, 0.22F));
  border(ctx, gui::Rect{x + 1.0F, y + 1.0F, 8.0F, 8.0F}, color);
  draw.add_line(x + 1.0F, y + 1.0F, x + 4.0F, y - 1.0F, 1.0F, color);
  draw.add_line(x + 9.0F, y + 1.0F, x + 6.0F, y - 1.0F, 1.0F, color);
  draw.add_line(x + 4.0F, y - 1.0F, x + 6.0F, y - 1.0F, 1.0F, color);
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
constexpr float compositor_node_width = 164.0F;
constexpr float compositor_node_header_height = 26.0F;
constexpr float compositor_port_row_height = 20.0F;
constexpr float compositor_port_hit_radius = 7.0F;
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
  float x{};
  float y{};
  gui::Rect hit;
};

[[nodiscard]] gui::UiColor port_color(const gui::UiContext &ctx,
                                      compositor::PortValueType type) noexcept {
  switch (type) {
  case compositor::PortValueType::Image:
    return ctx.theme().accent;
  case compositor::PortValueType::Effect:
    return gui::UiColor{0.780F, 0.318F, 0.922F, 1.0F};
  case compositor::PortValueType::TransformSet:
    return ctx.theme().playhead;
  }
  return ctx.theme().text_muted;
}

[[nodiscard]] gui::UiColor
compositor_node_color(const gui::UiContext &ctx,
                      compositor::NodeKind kind) noexcept {
  switch (kind) {
  case compositor::NodeKind::SceneLayer:
    return ctx.theme().good;
  case compositor::NodeKind::Brightness:
    return ctx.theme().keyframe;
  case compositor::NodeKind::Saturation:
    return gui::UiColor{0.965F, 0.380F, 0.600F, 1.0F};
  case compositor::NodeKind::Levels:
    return gui::UiColor{0.330F, 0.850F, 0.680F, 1.0F};
  case compositor::NodeKind::LiftGammaGain:
    return gui::UiColor{0.940F, 0.620F, 0.240F, 1.0F};
  case compositor::NodeKind::ContrastPivot:
    return gui::UiColor{0.720F, 0.480F, 0.980F, 1.0F};
  case compositor::NodeKind::ColorMatrix:
    return gui::UiColor{0.260F, 0.820F, 0.880F, 1.0F};
  case compositor::NodeKind::WhiteBalance:
    return gui::UiColor{0.980F, 0.700F, 0.420F, 1.0F};
  case compositor::NodeKind::Curves:
    return gui::UiColor{0.520F, 0.880F, 0.980F, 1.0F};
  case compositor::NodeKind::FilmGrain:
    return gui::UiColor{0.760F, 0.720F, 0.580F, 1.0F};
  case compositor::NodeKind::Blur:
    return gui::UiColor{0.780F, 0.318F, 0.922F, 1.0F};
  case compositor::NodeKind::BoxBlur:
    return gui::UiColor{0.260F, 0.820F, 0.880F, 1.0F};
  case compositor::NodeKind::DirectionalBlur:
    return gui::UiColor{0.620F, 0.500F, 0.980F, 1.0F};
  case compositor::NodeKind::ChromaKey:
    return gui::UiColor{0.300F, 0.900F, 0.430F, 1.0F};
  case compositor::NodeKind::LumaKey:
    return gui::UiColor{0.780F, 0.820F, 0.880F, 1.0F};
  case compositor::NodeKind::Matte:
    return gui::UiColor{0.520F, 0.720F, 0.980F, 1.0F};
  case compositor::NodeKind::RadialArray:
    return ctx.theme().playhead;
  case compositor::NodeKind::LedCarProcess:
    return gui::UiColor{0.180F, 0.760F, 0.960F, 1.0F};
  case compositor::NodeKind::Composite:
    return ctx.theme().accent;
  case compositor::NodeKind::Output:
    return ctx.theme().good;
  }
  return ctx.theme().text_muted;
}

[[nodiscard]] float compositor_node_height(compositor::NodeKind kind) noexcept {
  const std::size_t rows =
      std::max({compositor::input_ports(kind).size(),
                compositor::output_ports(kind).size(), std::size_t{1U}});
  return compositor_node_header_height +
         static_cast<float>(rows) * compositor_port_row_height + 8.0F;
}

[[nodiscard]] constexpr float
compositor_node_width_for(compositor::NodeKind kind) noexcept {
  // Directional plate names are operational labels, not abbreviations. Give
  // the purpose-built setup enough room to keep every socket unambiguous.
  return kind == compositor::NodeKind::LedCarProcess ? 252.0F
                                                     : compositor_node_width;
}

[[nodiscard]] std::string
compositor_node_detail(const compositor::GraphNode &node) {
  std::array<char, 64U> text{};
  int length = 0;
  switch (node.kind) {
  case compositor::NodeKind::SceneLayer: {
    const auto *parameters =
        std::get_if<compositor::SceneLayerParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "OBJECT #%llu",
        static_cast<unsigned long long>(
            parameters != nullptr ? parameters->scene_node.value() : 0U));
    break;
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

[[nodiscard]] float
parameter_panel_height(const compositor::GraphNode &node) noexcept {
  constexpr float header = 32.0F;
  constexpr float footer = 8.0F;
  constexpr float row = 26.0F;
  switch (node.kind) {
  case compositor::NodeKind::RadialArray:
    return header + footer + 4.0F * row;
  case compositor::NodeKind::LedCarProcess:
    return header + footer + 11.0F * row;
  case compositor::NodeKind::Levels:
    return header + footer + 20.0F * row;
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

void wire(gui::UiContext &ctx, float from_x, float from_y, float to_x,
          float to_y, gui::UiColor color, float thickness = 1.5F) {
  const float middle_x = (from_x + to_x) * 0.5F;
  auto &draw = ctx.draw_list();
  draw.add_line(from_x, from_y, middle_x, from_y, thickness, color);
  draw.add_line(middle_x, from_y, middle_x, to_y, thickness, color);
  draw.add_line(middle_x, to_y, to_x, to_y, thickness, color);
}

[[nodiscard]] std::array<gui::Rect, 3U>
wire_hit_bounds(float from_x, float from_y, float to_x, float to_y) noexcept {
  constexpr float radius = 5.0F;
  const float middle_x = (from_x + to_x) * 0.5F;
  const auto horizontal = [](float left, float right, float y) noexcept {
    constexpr float hit_radius = 5.0F;
    const float minimum = std::min(left, right);
    return gui::Rect{minimum - hit_radius, y - hit_radius,
                     std::abs(right - left) + hit_radius * 2.0F,
                     hit_radius * 2.0F};
  };
  const float minimum_y = std::min(from_y, to_y);
  return std::array<gui::Rect, 3U>{
      horizontal(from_x, middle_x, from_y),
      gui::Rect{middle_x - radius, minimum_y - radius, radius * 2.0F,
                std::abs(to_y - from_y) + radius * 2.0F},
      horizontal(middle_x, to_x, to_y)};
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

std::optional<SceneSelectionRequest>
ScenePanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                 std::span<const ElementInfo> elements,
                 std::span<const doc::NodeId> selection, doc::NodeId primary) {
  std::optional<SceneSelectionRequest> request;
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return request;
  }

  ctx.push_clip(bounds);
  ctx.panel(bounds);
  // A track is a screen in the scene, but its row is the timeline header it
  // already has. Listing it here as well would put the same thing in two
  // places with two ways to select it, and neither would be the obvious one.
  const auto listed = [](const ElementInfo &element) noexcept {
    return element.kind != ElementKind::Track;
  };
  const auto listed_elements = static_cast<std::size_t>(
      std::count_if(elements.begin(), elements.end(), listed));
  std::array<char, 32U> count_text{};
  const int count_length =
      std::snprintf(count_text.data(), count_text.size(), "%llu ITEMS",
                    static_cast<unsigned long long>(listed_elements));
  panel_title(
      ctx, bounds, "",
      std::string_view{count_text.data(),
                       static_cast<std::size_t>(std::max(count_length, 0))});

  const gui::Rect search_bounds{bounds.x, bounds.y + title_height, bounds.width,
                                search_height};
  search_field(ctx, mix(id_, "scene.search"), search_bounds, search_,
               "Search Scene...");

  std::array<std::vector<const ElementInfo *>, 4U> groups;
  for (const ElementInfo &element : elements) {
    if (!listed(element)) {
      continue;
    }
    const std::string name = element_name(element);
    if (matches(name, search_) || matches(kind_name(element.kind), search_)) {
      groups[group_index(group_for(element.kind))].push_back(&element);
    }
  }

  std::vector<doc::NodeId> visible_order;
  if (root_open_) {
    visible_order.reserve(elements.size());
    for (std::size_t group = 0U; group < groups.size(); ++group) {
      if ((groups[group].empty() && !search_.empty()) || !group_open_[group]) {
        continue;
      }
      for (const ElementInfo *const element : groups[group]) {
        if (!element->locked) {
          visible_order.push_back(element->node);
        }
      }
    }
  }

  constexpr float scene_footer_height = 22.0F;
  const gui::Rect list_bounds{bounds.x, search_bounds.y + search_bounds.height,
                              bounds.width,
                              std::max(bounds.height - title_height -
                                           search_height - scene_footer_height,
                                       0.0F)};
  float content_height = tree_row_height;
  if (root_open_) {
    for (std::size_t group = 0U; group < groups.size(); ++group) {
      if (groups[group].empty() && !search_.empty()) {
        continue;
      }
      content_height += section_row_height;
      if (group_open_[group]) {
        content_height +=
            tree_row_height * static_cast<float>(groups[group].size());
      }
    }
  }
  const float maximum_scroll =
      std::max(content_height - list_bounds.height, 0.0F);
  if (list_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
    scroll_ -= ctx.wheel() * tree_row_height * 2.0F;
  }
  scroll_ = std::clamp(scroll_, 0.0F, maximum_scroll);

  ctx.push_clip(list_bounds);
  float y = list_bounds.y - scroll_;
  const gui::Rect root_row{list_bounds.x, y, list_bounds.width,
                           tree_row_height};
  const gui::Interaction root_interaction =
      ctx.grab(mix(id_, "scene.root"), root_row);
  bool pointer_over_content =
      root_row.contains(ctx.pointer_x(), ctx.pointer_y());
  ctx.panel(root_row, root_interaction.hovered ? ctx.theme().widget_hover
                                               : ctx.theme().header);
  draw_twirl(ctx, root_row.x + 11.0F, root_row.y + root_row.height * 0.5F,
             root_open_, ctx.theme().text_muted);
  ctx.label(gui::Rect{root_row.x + 22.0F, root_row.y, root_row.width - 28.0F,
                      root_row.height},
            "SCENE", ctx.theme().text);
  if (root_interaction.clicked) {
    root_open_ = !root_open_;
  }
  y += tree_row_height;

  if (root_open_) {
    for (std::size_t group = 0U; group < groups.size(); ++group) {
      if (groups[group].empty() && !search_.empty()) {
        continue;
      }
      const gui::Rect group_row{list_bounds.x, y, list_bounds.width,
                                section_row_height};
      pointer_over_content =
          pointer_over_content ||
          group_row.contains(ctx.pointer_x(), ctx.pointer_y());
      const gui::Interaction group_interaction =
          ctx.grab(mix(id_, "scene.group", group), group_row);
      ctx.panel(group_row, group_interaction.hovered ? ctx.theme().widget_hover
                                                     : ctx.theme().row_alt);
      draw_twirl(ctx, group_row.x + 24.0F,
                 group_row.y + group_row.height * 0.5F, group_open_[group],
                 ctx.theme().text_muted);
      ctx.label(gui::Rect{group_row.x + 35.0F, group_row.y,
                          group_row.width - 69.0F, group_row.height},
                scene_group_names[group], ctx.theme().text_muted);
      std::array<char, 24U> group_count{};
      const int group_length =
          std::snprintf(group_count.data(), group_count.size(), "%llu",
                        static_cast<unsigned long long>(groups[group].size()));
      ctx.label(
          gui::Rect{group_row.x + group_row.width - 28.0F, group_row.y, 22.0F,
                    group_row.height},
          std::string_view{group_count.data(),
                           static_cast<std::size_t>(std::max(group_length, 0))},
          ctx.theme().text_muted);
      if (group_interaction.clicked) {
        group_open_[group] = !group_open_[group];
      }
      y += section_row_height;

      if (!group_open_[group]) {
        continue;
      }
      std::size_t row_index = 0U;
      for (const ElementInfo *const element : groups[group]) {
        const gui::Rect row{list_bounds.x, y, list_bounds.width,
                            tree_row_height};
        pointer_over_content = pointer_over_content ||
                               row.contains(ctx.pointer_x(), ctx.pointer_y());
        const float lock_width = std::min(48.0F, row.width);
        const gui::Rect row_content{
            row.x, row.y, std::max(row.width - lock_width, 0.0F), row.height};
        const gui::Rect lock_bounds{row_content.x + row_content.width, row.y,
                                    lock_width, row.height};
        const gui::UiId row_id =
            mix(id_, "scene.element", element->node.value());
        const gui::Interaction interaction =
            element->locked
                ? gui::Interaction{}
                : clipped_grab(ctx, row_id, row_content, list_bounds);
        const gui::Interaction lock_interaction = clipped_grab(
            ctx, mix(id_, "scene.element.lock", element->node.value()),
            lock_bounds, list_bounds);
        const bool is_selected = selected(element->node, selection);
        gui::UiColor row_color =
            row_index % 2U == 0U ? ctx.theme().row : ctx.theme().row_alt;
        if (is_selected) {
          row_color = element->node == primary
                          ? scale(ctx.theme().selection, 1.22F)
                          : ctx.theme().selection;
        } else if (interaction.hovered || lock_interaction.hovered) {
          row_color = ctx.theme().widget_hover;
        }
        ctx.panel(row, row_color);
        if (element->node == primary) {
          ctx.panel(gui::Rect{row.x, row.y, 2.0F, row.height},
                    ctx.theme().accent);
        }
        draw_scene_kind_icon(ctx, *element, row.x + 10.0F, row.y + 5.0F);
        const std::string name = element_name(*element);
        ctx.label(
            gui::Rect{row.x + 27.0F, row.y,
                      std::max(row_content.width - 31.0F, 0.0F), row.height},
            name,
            element->locked ? ctx.theme().text_muted
                            : (is_selected ? ctx.theme().text
                                           : scale(ctx.theme().text, 0.88F)));
        ctx.panel(gui::Rect{row_content.x + row_content.width - 5.0F,
                            row.y + 6.0F, 3.0F, 8.0F},
                  kind_color(element->kind));
        ctx.panel(gui::Rect{lock_bounds.x, lock_bounds.y + 4.0F, 1.0F,
                            std::max(lock_bounds.height - 8.0F, 0.0F)},
                  ctx.theme().divider);
        ctx.label(lock_bounds,
                  element->locked ? std::string_view{"LOCKED"}
                                  : std::string_view{"LOCK"},
                  element->locked || lock_interaction.hovered
                      ? ctx.theme().warning
                      : ctx.theme().text_muted);
        if (interaction.clicked) {
          SceneSelectionRequest next{};
          next.node = element->node;
          next.action =
              selection_action(ctx.ctrl_held(), ctx.shift_held(), true);
          if (next.action == SelectionAction::Range) {
            next.visible_order = visible_order;
          }
          request = std::move(next);
        }
        if (lock_interaction.clicked) {
          SceneSelectionRequest next{};
          next.node = element->node;
          next.lock_changed = true;
          next.locked = !element->locked;
          request = std::move(next);
        }
        y += tree_row_height;
        ++row_index;
      }
    }
  }
  if (!request.has_value() && ctx.pointer_pressed() &&
      list_bounds.contains(ctx.pointer_x(), ctx.pointer_y()) &&
      !pointer_over_content) {
    SceneSelectionRequest clear{};
    clear.action = SelectionAction::Clear;
    request = std::move(clear);
  }
  ctx.pop_clip();

  const gui::Rect footer{bounds.x,
                         bounds.y + bounds.height - scene_footer_height,
                         bounds.width, scene_footer_height};
  ctx.panel(footer, ctx.theme().background);
  ctx.panel(gui::Rect{footer.x, footer.y, footer.width, 1.0F},
            ctx.theme().divider);
  const auto selected_element = [&](doc::NodeId node) {
    return std::find_if(elements.begin(), elements.end(),
                        [node](const ElementInfo &element) noexcept {
                          return element.node == node;
                        });
  };
  const bool can_group =
      !selection.empty() &&
      std::all_of(selection.begin(), selection.end(), [&](doc::NodeId node) {
        const auto found = selected_element(node);
        return found != elements.end() && !found->locked &&
               !found->parent_group.has_value() &&
               (found->kind == ElementKind::Object ||
                found->kind == ElementKind::Media ||
                found->kind == ElementKind::Model ||
                found->kind == ElementKind::Group);
      });
  const auto primary_element = selected_element(primary);
  const bool can_ungroup =
      selection.size() == 1U && primary_element != elements.end() &&
      !primary_element->locked && primary_element->visible &&
      primary_element->kind == ElementKind::Group;
  constexpr float action_width = 58.0F;
  constexpr float action_gap = 3.0F;
  const gui::Rect ungroup_bounds{footer.x + footer.width - action_width - 4.0F,
                                 footer.y + 2.0F, action_width,
                                 std::max(footer.height - 4.0F, 0.0F)};
  const gui::Rect group_bounds{ungroup_bounds.x - action_width - action_gap,
                               ungroup_bounds.y, action_width,
                               ungroup_bounds.height};
  const auto structure_button = [&](gui::UiId button_id,
                                    const gui::Rect &button_bounds,
                                    std::string_view label, bool enabled) {
    const gui::Interaction interaction =
        enabled ? ctx.grab(button_id, button_bounds) : gui::Interaction{};
    ctx.panel(button_bounds,
              !enabled ? ctx.theme().track
                       : (interaction.held
                              ? ctx.theme().widget_active
                              : (interaction.hovered ? ctx.theme().widget_hover
                                                     : ctx.theme().widget)));
    border(ctx, button_bounds,
           enabled ? ctx.theme().header : ctx.theme().divider);
    ctx.label(button_bounds, label,
              enabled ? ctx.theme().text : ctx.theme().text_muted);
    return enabled && interaction.clicked;
  };
  if (structure_button(mix(id_, "scene.structure.group"), group_bounds, "GROUP",
                       can_group)) {
    SceneSelectionRequest next{};
    next.structure = SceneSelectionRequest::StructureAction::Group;
    request = std::move(next);
  }
  if (structure_button(mix(id_, "scene.structure.ungroup"), ungroup_bounds,
                       "UNGROUP", can_ungroup)) {
    SceneSelectionRequest next{};
    next.structure = SceneSelectionRequest::StructureAction::Ungroup;
    request = std::move(next);
  }
  ctx.label(gui::Rect{footer.x + 7.0F, footer.y,
                      std::max(group_bounds.x - footer.x - 11.0F, 0.0F),
                      footer.height},
            selection.empty() ? "NO SELECTION" : "SELECTION ACTIVE",
            selection.empty() ? ctx.theme().text_muted : ctx.theme().accent);
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
        if (row.width > 170.0F) {
          ctx.label(gui::Rect{row.x + row.width * 0.58F, row.y,
                              std::max(row.width * 0.30F, 0.0F), row.height},
                    item->detail, ctx.theme().text_muted);
        }
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
                    assets::asset_source(*entry.asset),
                    scale(ctx.theme().text_muted, 0.90F));
          ctx.label(gui::Rect{text_x, row.y + 33.0F, text_width, 14.0F},
                    entry.detail, ctx.theme().text_muted);
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
                    assets::asset_source(*entry.asset),
                    scale(ctx.theme().text_muted, 0.90F));
          ctx.label(gui::Rect{row.x + 31.0F, row.y + 31.0F,
                              std::max(content.width - 48.0F, 0.0F), 15.0F},
                    entry.detail, ctx.theme().text_muted);
        }
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

CompositorFrame
CompositorPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                      const compositor::CompositorGraph &compositor_graph) {
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
  }

  gui::Rect canvas = content;
  gui::Rect parameter_bounds{};
  if (parameter_node != nullptr) {
    constexpr float preferred_width = 224.0F;
    constexpr float minimum_graph_width = 256.0F;
    const float required_height = parameter_panel_height(*parameter_node);
    if (content.width >= minimum_graph_width + 190.0F) {
      const float width =
          std::clamp(content.width * 0.28F, 190.0F, preferred_width);
      canvas.width = std::max(content.width - width, 0.0F);
      parameter_bounds =
          gui::Rect{canvas.x + canvas.width, content.y, width, content.height};
    } else {
      const float height =
          std::min(required_height, std::max(content.height - 72.0F, 0.0F));
      canvas.height = std::max(content.height - height, 0.0F);
      parameter_bounds =
          gui::Rect{content.x, canvas.y + canvas.height, content.width, height};
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
  const gui::Rect add_button{toolbar.x + 8.0F, toolbar.y + 3.0F, 94.0F,
                             std::max(toolbar.height - 6.0F, 0.0F)};
  const gui::Interaction add_interaction =
      clipped_grab(ctx, mix(id_, "compositor.add_menu"), add_button, toolbar);
  ctx.panel(add_button, add_interaction.held ? ctx.theme().widget_active
                                             : (add_interaction.hovered
                                                    ? ctx.theme().widget_hover
                                                    : ctx.theme().widget));
  ctx.label(add_button, add_menu_open_ ? "ADD NODE  −" : "ADD NODE  +",
            add_interaction.hovered ? ctx.theme().text : ctx.theme().accent);
  if (add_interaction.clicked) {
    add_menu_open_ = !add_menu_open_;
    if (!add_menu_open_) {
      add_category_.reset();
      ctx.clear_focus();
    }
  }

  if (toolbar.width >= 520.0F) {
    const float navigation_x = toolbar.x + 110.0F;
    const gui::Rect zoom_out{navigation_x, toolbar.y + 3.0F, 22.0F,
                             std::max(toolbar.height - 6.0F, 0.0F)};
    const gui::Rect zoom_value{navigation_x + 24.0F, toolbar.y + 3.0F, 48.0F,
                               std::max(toolbar.height - 6.0F, 0.0F)};
    const gui::Rect zoom_in{navigation_x + 74.0F, toolbar.y + 3.0F, 22.0F,
                            std::max(toolbar.height - 6.0F, 0.0F)};
    const gui::Rect reset_view{navigation_x + 100.0F, toolbar.y + 3.0F, 54.0F,
                               std::max(toolbar.height - 6.0F, 0.0F)};
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
      ctx.label(button, label,
                interaction.hovered ? ctx.theme().text
                                    : ctx.theme().text_muted);
      return interaction.clicked;
    };
    if (navigation_button("compositor.zoom_out", zoom_out, "−")) {
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
    ctx.label(
        gui::Rect{navigation_x + 160.0F, toolbar.y, 146.0F, toolbar.height},
        "WHEEL ZOOM  ·  MMB PAN", ctx.theme().text_muted);
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
  const float trailing_control_width = has_selection ? 98.0F : 0.0F;
  ctx.label(gui::Rect{toolbar.x + toolbar.width - status_width - 8.0F -
                          trailing_control_width,
                      toolbar.y, status_width, toolbar.height},
            graph_status_text,
            !invalid_output_count && compositor_graph.diagnostics.empty()
                ? ctx.theme().text_muted
                : ctx.theme().warning);
  if (has_selection) {
    const gui::Rect remove_bounds{toolbar.x + toolbar.width - 94.0F,
                                  toolbar.y + 3.0F, 86.0F,
                                  std::max(toolbar.height - 6.0F, 0.0F)};
    const std::string_view label =
        selection_.kind == CompositorSelectionKind::Edge
            ? std::string_view{"DISCONNECT"}
            : (selected_required_output ? std::string_view{"OUTPUT REQUIRED"}
                                        : std::string_view{"DELETE NODE"});
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
    ctx.label(remove_bounds, label,
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
      const bool dragging = dragging_node_ == node.node;
      const float editor_x = dragging ? drag_x_ : node.editor_x;
      const float editor_y = dragging ? drag_y_ : node.editor_y;
      const float node_width = compositor_node_width_for(node.kind);
      const gui::Rect node_bounds{canvas.x + pan_x_ + editor_x * zoom_,
                                  canvas.y + pan_y_ + editor_y * zoom_,
                                  node_width * zoom_,
                                  compositor_node_height(node.kind) * zoom_};
      nodes.push_back(CompositorNodeVisual{std::addressof(node), node_bounds});

      const auto append_ports =
          [&](std::span<const compositor::PortDescriptor> descriptors,
              std::vector<CompositorPortVisual> &destination, bool input) {
            for (std::size_t index = 0U; index < descriptors.size(); ++index) {
              const compositor::PortDescriptor &descriptor = descriptors[index];
              const float x =
                  input ? node_bounds.x : node_bounds.x + node_bounds.width;
              const float y = node_bounds.y +
                              compositor_node_header_height * zoom_ +
                              compositor_port_row_height * zoom_ *
                                  (static_cast<float>(index) + 0.5F);
              const float hit_radius =
                  std::max(5.0F, compositor_port_hit_radius * zoom_);
              destination.push_back(CompositorPortVisual{
                  compositor::EdgeEndpoint{node.node, descriptor.index},
                  std::addressof(descriptor), x, y,
                  gui::Rect{x - hit_radius, y - hit_radius, hit_radius * 2.0F,
                            hit_radius * 2.0F}});
            }
          };
      append_ports(compositor::input_ports(node.kind), inputs, true);
      append_ports(compositor::output_ports(node.kind), outputs, false);
    }
  };
  build_visuals();

  bool selection_pressed = false;
  if (connecting_from_.has_value() && ctx.pointer_released()) {
    const CompositorPortVisual *const from =
        find_port(outputs, *connecting_from_);
    if (from != nullptr) {
      for (const CompositorPortVisual &to : inputs) {
        if (to.hit.contains(ctx.pointer_x(), ctx.pointer_y()) &&
            can_connect(compositor_graph, *from, to)) {
          std::optional<doc::NodeId> replace_edge;
          if (to.descriptor != nullptr && !to.descriptor->multiple) {
            const auto occupied = std::find_if(
                compositor_graph.edges.begin(), compositor_graph.edges.end(),
                [&to](const compositor::GraphEdge &edge) noexcept {
                  return edge.to == to.endpoint;
                });
            if (occupied != compositor_graph.edges.end()) {
              replace_edge = occupied->node;
            }
          }
          frame.connect_ports = CompositorConnectIntent{
              from->endpoint, to.endpoint, replace_edge};
          break;
        }
      }
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
    const float port_gutter =
        std::max(5.0F, compositor_port_hit_radius * zoom_);
    const gui::Rect move_hit{
        visual.bounds.x + port_gutter, visual.bounds.y,
        std::max(visual.bounds.width - port_gutter * 2.0F, 0.0F),
        visual.bounds.height};
    const gui::Interaction interaction = clipped_grab(
        ctx, mix(id_, "compositor.node", visual.node->node.value()), move_hit,
        canvas);
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
        }
        drag_released = true;
      }
    }
  }
  if (layout_changed) {
    build_visuals();
  }

  for (const compositor::GraphEdge &edge : compositor_graph.edges) {
    const CompositorPortVisual *const from = find_port(outputs, edge.from);
    const CompositorPortVisual *const to = find_port(inputs, edge.to);
    if (from == nullptr || to == nullptr || from->descriptor == nullptr ||
        to->descriptor == nullptr ||
        from->descriptor->type != to->descriptor->type) {
      continue;
    }
    const auto hit_bounds = wire_hit_bounds(from->x, from->y, to->x, to->y);
    for (std::size_t segment = 0U; segment < hit_bounds.size(); ++segment) {
      const gui::Interaction interaction =
          clipped_grab(ctx,
                       mix(id_, "compositor.edge",
                           edge.node.value() * hit_bounds.size() + segment),
                       hit_bounds[segment], canvas);
      if (interaction.pressed) {
        selection_ =
            CompositorSelection{CompositorSelectionKind::Edge, edge.node};
        frame.selection = selection_;
        selection_pressed = true;
      }
    }
  }
  if (ctx.pointer_pressed() &&
      canvas.contains(ctx.pointer_x(), ctx.pointer_y()) && !selection_pressed) {
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
        std::min(std::max(canvas.width - 16.0F, 0.0F), 660.0F), 40.0F};
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
                        std::max(guide.width - 16.0F, 0.0F), 17.0F},
              title, diagnostic ? ctx.theme().warning : ctx.theme().text_muted);
    ctx.label(gui::Rect{guide.x + 8.0F, guide.y + 20.0F,
                        std::max(guide.width - 16.0F, 0.0F), 17.0F},
              "FLOW: EFFECT / RADIAL -> LAYER -> COMP -> OUTPUT  |  "
              "DRAG RIGHT OUT TO LEFT IN",
              ctx.theme().text_muted);
  }

  for (const compositor::GraphEdge &edge : compositor_graph.edges) {
    const CompositorPortVisual *const from = find_port(outputs, edge.from);
    const CompositorPortVisual *const to = find_port(inputs, edge.to);
    if (from == nullptr || to == nullptr || from->descriptor == nullptr ||
        to->descriptor == nullptr ||
        from->descriptor->type != to->descriptor->type) {
      continue;
    }
    const bool selected =
        selection_ ==
        CompositorSelection{CompositorSelectionKind::Edge, edge.node};
    wire(ctx, from->x, from->y, to->x, to->y,
         selected ? ctx.theme().text
                  : with_alpha(port_color(ctx, from->descriptor->type), 0.82F),
         selected ? 3.0F : 1.5F);
  }
  if (connecting_from_.has_value()) {
    const CompositorPortVisual *const from =
        find_port(outputs, *connecting_from_);
    if (from != nullptr) {
      wire(ctx, from->x, from->y, ctx.pointer_x(), ctx.pointer_y(),
           port_color(ctx, connecting_type_));
    }
  }

  for (const CompositorNodeVisual &visual : nodes) {
    const bool selected =
        selection_ ==
        CompositorSelection{CompositorSelectionKind::Node, visual.node->node};
    const bool diagnostic =
        node_has_diagnostic(compositor_graph, visual.node->node);
    ctx.panel(visual.bounds, selected ? scale(ctx.theme().selection, 1.18F)
                                      : scale(ctx.theme().panel, 1.06F));
    border(ctx, visual.bounds,
           diagnostic
               ? ctx.theme().warning
               : (selected ? ctx.theme().accent : ctx.theme().widget_hover));
    const gui::UiColor kind = compositor_node_color(ctx, visual.node->kind);
    const float header_inset = std::max(1.0F, zoom_);
    const gui::Rect node_header{
        visual.bounds.x + header_inset, visual.bounds.y + header_inset,
        std::max(visual.bounds.width - header_inset * 2.0F, 0.0F),
        std::max(compositor_node_header_height * zoom_ - header_inset * 2.0F,
                 0.0F)};
    ctx.panel(node_header, scale(ctx.theme().header, 1.08F));
    ctx.panel(gui::Rect{node_header.x, node_header.y,
                        std::max(1.5F, 3.0F * zoom_), node_header.height},
              kind);
    const std::string_view title =
        visual.node->label.empty()
            ? compositor::node_kind_name(visual.node->kind)
            : std::string_view{visual.node->label};
    if (zoom_ >= 0.9F) {
      ctx.label(gui::Rect{node_header.x + 9.0F * zoom_, node_header.y,
                          std::max(node_header.width - 17.0F * zoom_, 0.0F),
                          13.0F},
                title, ctx.theme().text);
      const std::string detail = compositor_node_detail(*visual.node);
      ctx.label(
          gui::Rect{node_header.x + 9.0F * zoom_, node_header.y + 12.0F * zoom_,
                    std::max(node_header.width - 17.0F * zoom_, 0.0F), 12.0F},
          detail, ctx.theme().text_muted);
    } else if (zoom_ >= 0.62F) {
      ctx.label(gui::Rect{node_header.x + 7.0F, node_header.y,
                          std::max(node_header.width - 12.0F, 0.0F),
                          node_header.height},
                title, ctx.theme().text);
    }

    for (const CompositorPortVisual &port : inputs) {
      if (port.endpoint.node != visual.node->node) {
        continue;
      }
      const bool compatible = connecting_from_.has_value() &&
                              port.descriptor->type == connecting_type_;
      const bool hovered = port.hit.contains(ctx.pointer_x(), ctx.pointer_y());
      const gui::UiColor color = port_color(ctx, port.descriptor->type);
      const float port_radius = std::max(2.0F, 4.5F * zoom_);
      draw_circle(ctx, port.x, port.y,
                  compatible && hovered ? port_radius + 1.5F : port_radius,
                  compatible && hovered ? ctx.theme().text : color);
      draw_circle(ctx, port.x, port.y, std::max(1.0F, 2.0F * zoom_),
                  ctx.theme().panel);
      if (zoom_ >= 0.85F) {
        const std::string input_label =
            "IN  " + std::string{port.descriptor->name};
        ctx.label(gui::Rect{port.x + 9.0F * zoom_, port.y - 8.0F,
                            std::max(visual.bounds.width * 0.48F, 0.0F), 16.0F},
                  input_label,
                  compatible ? ctx.theme().text : ctx.theme().text_muted);
      }
    }
    for (const CompositorPortVisual &port : outputs) {
      if (port.endpoint.node != visual.node->node) {
        continue;
      }
      const bool hovered = port.hit.contains(ctx.pointer_x(), ctx.pointer_y());
      const bool active = connecting_from_ == port.endpoint;
      const gui::UiColor color = port_color(ctx, port.descriptor->type);
      const float port_radius = std::max(2.0F, 4.5F * zoom_);
      draw_circle(ctx, port.x, port.y,
                  hovered || active ? port_radius + 1.0F : port_radius,
                  active ? ctx.theme().text : color);
      draw_circle(ctx, port.x, port.y, std::max(1.0F, 2.0F * zoom_),
                  ctx.theme().panel);
      if (zoom_ >= 0.85F) {
        const std::string output_label =
            std::string{port.descriptor->name} + "  OUT";
        const float label_width = ctx.font_width(output_label);
        ctx.label(gui::Rect{port.x - label_width - 9.0F * zoom_, port.y - 8.0F,
                            label_width, 16.0F},
                  output_label, ctx.theme().text_muted);
      }
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
    ctx.label(gui::Rect{parameter_bounds.x + 8.0F, parameter_bounds.y + 3.0F,
                        std::max(parameter_bounds.width - 16.0F, 0.0F), 15.0F},
              "PARAMETERS", ctx.theme().text);
    ctx.label(gui::Rect{parameter_bounds.x + 8.0F, parameter_bounds.y + 17.0F,
                        std::max(parameter_bounds.width - 16.0F, 0.0F), 13.0F},
              compositor::node_kind_name(parameter_node->kind),
              ctx.theme().text_muted);

    float parameter_y = parameter_bounds.y + 32.0F;
    const float label_width =
        std::clamp(parameter_bounds.width * 0.34F, 60.0F, 82.0F);
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
          const gui::Rect row{parameter_bounds.x + 8.0F, parameter_y,
                              std::max(parameter_bounds.width - 16.0F, 0.0F),
                              22.0F};
          parameter_y += 26.0F;
          ctx.label(gui::Rect{row.x, row.y, label_width, row.height}, label,
                    ctx.theme().text_muted);
          const gui::Rect field{row.x + label_width, row.y,
                                std::max(row.width - label_width, 0.0F),
                                row.height};
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
      const gui::Rect row{parameter_bounds.x + 8.0F, parameter_y,
                          std::max(parameter_bounds.width - 16.0F, 0.0F),
                          22.0F};
      parameter_y += 26.0F;
      ctx.label(gui::Rect{row.x, row.y, label_width, row.height}, label,
                ctx.theme().text_muted);
      const gui::Rect field{row.x + label_width, row.y,
                            std::max(row.width - label_width, 0.0F),
                            row.height};
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
      const auto channel_fields =
          [&](std::string_view prefix,
              const scene::LevelsChannelParameters &channel,
              const std::array<Property, 5U> &properties) {
            const std::string input_black = std::string{prefix} + " IN BLACK";
            const std::string input_white = std::string{prefix} + " IN WHITE";
            const std::string gamma = std::string{prefix} + " GAMMA";
            const std::string output_black = std::string{prefix} + " OUT BLACK";
            const std::string output_white = std::string{prefix} + " OUT WHITE";
            scalar_field(input_black, properties[0U], channel.input_black,
                         0.005, 0.0F,
                         std::max(channel.input_white - 0.0001F, 0.0F), 3);
            scalar_field(input_white, properties[1U], channel.input_white,
                         0.005, std::min(channel.input_black + 0.0001F, 1.0F),
                         1.0F, 3);
            scalar_field(gamma, properties[2U], channel.gamma, 0.01,
                         scene::min_levels_gamma, scene::max_levels_gamma, 3);
            scalar_field(output_black, properties[3U], channel.output_black,
                         0.005, 0.0F, 1.0F, 3);
            scalar_field(output_white, properties[4U], channel.output_white,
                         0.005, 0.0F, 1.0F, 3);
          };
      channel_fields(
          "MASTER", parameters->master,
          {Property::LevelsMasterInputBlack, Property::LevelsMasterInputWhite,
           Property::LevelsMasterGamma, Property::LevelsMasterOutputBlack,
           Property::LevelsMasterOutputWhite});
      channel_fields("RED", parameters->red,
                     {Property::LevelsRedInputBlack,
                      Property::LevelsRedInputWhite, Property::LevelsRedGamma,
                      Property::LevelsRedOutputBlack,
                      Property::LevelsRedOutputWhite});
      channel_fields(
          "GREEN", parameters->green,
          {Property::LevelsGreenInputBlack, Property::LevelsGreenInputWhite,
           Property::LevelsGreenGamma, Property::LevelsGreenOutputBlack,
           Property::LevelsGreenOutputWhite});
      channel_fields("BLUE", parameters->blue,
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
      const gui::Rect action_row{parameter_bounds.x + 8.0F, parameter_y,
                                 std::max(parameter_bounds.width - 16.0F, 0.0F),
                                 22.0F};
      parameter_y += 26.0F;
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
      ctx.label(gui::Rect{parameter_bounds.x + 8.0F, parameter_y,
                          std::max(parameter_bounds.width - 16.0F, 0.0F),
                          22.0F},
                count, ctx.theme().text_muted);
      parameter_y += 26.0F;

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
          const gui::Rect row{parameter_bounds.x + 8.0F, parameter_y,
                              std::max(parameter_bounds.width - 16.0F, 0.0F),
                              22.0F};
          parameter_y += 26.0F;
          ctx.label(gui::Rect{row.x, row.y, label_width, row.height}, label,
                    ctx.theme().text_muted);
          const gui::Rect field{row.x + label_width, row.y,
                                std::max(row.width - label_width, 0.0F),
                                row.height};
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

      const gui::Rect seed_row{parameter_bounds.x + 8.0F, parameter_y,
                               std::max(parameter_bounds.width - 16.0F, 0.0F),
                               22.0F};
      parameter_y += 26.0F;
      ctx.label(gui::Rect{seed_row.x, seed_row.y, label_width, seed_row.height},
                "SEED", ctx.theme().text_muted);
      const gui::Rect seed_field{seed_row.x + label_width, seed_row.y,
                                 std::max(seed_row.width - label_width, 0.0F),
                                 seed_row.height};
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

      const gui::Rect mode_row{parameter_bounds.x + 8.0F, parameter_y,
                               std::max(parameter_bounds.width - 16.0F, 0.0F),
                               22.0F};
      parameter_y += 26.0F;
      ctx.label(gui::Rect{mode_row.x, mode_row.y, label_width, mode_row.height},
                "MODE", ctx.theme().text_muted);
      const gui::Rect mode_button{mode_row.x + label_width, mode_row.y,
                                  std::max(mode_row.width - label_width, 0.0F),
                                  mode_row.height};
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

      const gui::Rect row{parameter_bounds.x + 8.0F, parameter_y,
                          std::max(parameter_bounds.width - 16.0F, 0.0F),
                          22.0F};
      parameter_y += 26.0F;
      ctx.label(gui::Rect{row.x, row.y, label_width, row.height}, "ANGLE TURNS",
                ctx.theme().text_muted);
      const gui::Rect field{row.x + label_width, row.y,
                            std::max(row.width - label_width, 0.0F),
                            row.height};
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

      const gui::Rect count_row{parameter_bounds.x + 8.0F, parameter_y,
                                std::max(parameter_bounds.width - 16.0F, 0.0F),
                                22.0F};
      parameter_y += 26.0F;
      ctx.label(
          gui::Rect{count_row.x, count_row.y, label_width, count_row.height},
          "COUNT", ctx.theme().text_muted);
      const gui::Rect count_field{count_row.x + label_width, count_row.y,
                                  std::max(count_row.width - label_width, 0.0F),
                                  count_row.height};
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

      const gui::Rect center_row{parameter_bounds.x + 8.0F, parameter_y,
                                 std::max(parameter_bounds.width - 16.0F, 0.0F),
                                 22.0F};
      ctx.label(
          gui::Rect{center_row.x, center_row.y, label_width, center_row.height},
          "CENTER", ctx.theme().text_muted);
      const float gap = 4.0F;
      const float available =
          std::max(center_row.width - label_width - gap, 0.0F);
      const float field_width = available * 0.5F;
      const gui::Rect center_x_field{center_row.x + label_width, center_row.y,
                                     field_width, center_row.height};
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
      scalar_field("POSITION X", CompositorProperty::LedCarPositionX,
                   parameters->position.x, 0.05,
                   -compositor::max_spatial_extent,
                   compositor::max_spatial_extent, 2);
      scalar_field("POSITION Y", CompositorProperty::LedCarPositionY,
                   parameters->position.y, 0.05,
                   -compositor::max_spatial_extent,
                   compositor::max_spatial_extent, 2);
      scalar_field("POSITION Z", CompositorProperty::LedCarPositionZ,
                   parameters->position.z, 0.05,
                   -compositor::max_spatial_extent,
                   compositor::max_spatial_extent, 2);
      scalar_field("YAW", CompositorProperty::LedCarYawTurns, parameters->yaw,
                   0.001, -compositor::max_spatial_extent,
                   compositor::max_spatial_extent, 3);
      scalar_field("PITCH", CompositorProperty::LedCarPitchTurns,
                   parameters->pitch, 0.001, -compositor::max_spatial_extent,
                   compositor::max_spatial_extent, 3);
      scalar_field("ROLL", CompositorProperty::LedCarRollTurns,
                   parameters->roll, 0.001, -compositor::max_spatial_extent,
                   compositor::max_spatial_extent, 3);
      scalar_field("PLATE SCALE", CompositorProperty::LedCarPlateScale,
                   parameters->plate_scale, 0.05, 0.0F,
                   compositor::max_spatial_extent, 2);
      scalar_field("ARRAY SCALE", CompositorProperty::LedCarArrayScale,
                   parameters->array_scale, 0.05, 0.0F,
                   compositor::max_spatial_extent, 2);
      scalar_field(
          "REFLECTION HEIGHT", CompositorProperty::LedCarReflectionHeight,
          parameters->reflection_height, 0.05, -compositor::max_spatial_extent,
          compositor::max_spatial_extent, 2);
      scalar_field("REFLECTION SCALE",
                   CompositorProperty::LedCarReflectionScale,
                   parameters->reflection_scale, 0.05, 0.0F,
                   compositor::max_spatial_extent, 2);
      scalar_field("HORIZONTAL BLEND",
                   CompositorProperty::LedCarHorizontalBlend,
                   parameters->horizontal_blend, 0.005, 0.0F,
                   compositor::max_led_car_horizontal_blend, 3);
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
      const float node_height = compositor_node_height(item.kind);
      const float node_width = compositor_node_width_for(item.kind);
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

[[nodiscard]] const std::array<PreferencesField, 5U> &preferences_fields() {
  static const std::array<PreferencesField, 5U> fields{{
      {"Director listen", "ADDRESS:PORT render nodes connect to",
       &EditorPreferences::director_listen,
       &EditorPreferencesState::director_listen_overridden},
      {"Show ID", "32 hexadecimal characters, same on every node",
       &EditorPreferences::show_id,
       &EditorPreferencesState::show_id_overridden},
      {"Director epoch", "32 hexadecimal characters",
       &EditorPreferences::director_epoch,
       &EditorPreferencesState::director_epoch_overridden},
      {"Lifecycle secret", "32-256 byte credential file on this machine",
       &EditorPreferences::lifecycle_secret_file,
       &EditorPreferencesState::lifecycle_secret_file_overridden},
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
    return "Could not be read. Radiance .hdr only; OpenEXR is not read yet.";
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
  category_ = Category::Renderer;
  environment_scroll_ = 0.0F;
  environment_content_height_ = 0.0F;
  geometry_scroll_ = 0.0F;
  geometry_content_height_ = 0.0F;
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
  // Six categories share the row. The divisor is the count, not a constant
  // that happened to equal it - adding a seventh without changing this would
  // leave the last tab off the card, and the test that clicks it by position
  // would fail somewhere that does not name the cause.
  constexpr float category_count = 8.0F;
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
  tab(Category::Renderer, "project.tab.renderer", "RENDERER", 0U);
  tab(Category::Image, "project.tab.image", "IMAGE", 1U);
  // Colour sits beside Image: both are how the finished picture is treated,
  // and the output stage is where the picture last changes before the wall.
  tab(Category::Color, "project.tab.color", "COLOUR", 2U);
  tab(Category::Environment, "project.tab.environment", "ENVIRON.", 3U);
  tab(Category::Tracking, "project.tab.tracking", "TRACKING", 4U);
  tab(Category::Lens, "project.tab.lens", "LENS", 5U);
  tab(Category::Geometry, "project.tab.geometry", "GEOMETRY", 6U);
  tab(Category::Shadows, "project.tab.shadows", "SHADOWS", 7U);
  ctx.panel(gui::Rect{card.x + 14.0F, tabs_y + tab_height + 7.0F,
                      card.width - 28.0F, 1.0F},
            ctx.theme().divider);

  const gui::Rect content_bounds{
      card.x, tabs_y + tab_height + 8.0F, card.width,
      std::max(buttons_y - 8.0F - (tabs_y + tab_height + 8.0F), 0.0F)};
  ctx.push_clip(content_bounds);

  constexpr float label_width = 150.0F;
  constexpr float value_width = 66.0F;
  constexpr float row_pitch = 44.0F;
  float y = content_bounds.y;
  if (category_ == Category::Geometry) {
    const float maximum = std::max(geometry_content_height_ - content_bounds.height, 0.0F);
    if (content_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) geometry_scroll_ -= ctx.wheel() * 36.0F;
    geometry_scroll_ = std::clamp(geometry_scroll_, 0.0F, maximum);
    y -= geometry_scroll_;
    if (maximum > 0.0F) {
      const float thumb = content_bounds.height * content_bounds.height / geometry_content_height_;
      ctx.panel(gui::Rect{card.x + card.width - 5.0F,
                content_bounds.y + geometry_scroll_ / maximum * (content_bounds.height - thumb),
                3.0F, thumb}, ctx.theme().divider);
    }
  }


  const auto section = [&](std::string_view title) {
    ctx.label(gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 18.0F}, title,
              ctx.theme().accent);
    y += 22.0F;
  };
  // A slider plus the exact number it is holding. The number matters: these
  // are values an operator copies between shows, and a bare handle position is
  // not something anyone can write down.
  const auto scalar_row = [&](std::string_view label, float &value, float min,
                              float max, const char *format,
                              std::string_view note) {
    ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 22.0F}, label,
              ctx.theme().text);
    const gui::Rect track{card.x + 14.0F + label_width, y,
                          card.width - 28.0F - label_width - value_width - 8.0F,
                          22.0F};
    static_cast<void>(ctx.slider(mix(id_, label), track, value, min, max));
    std::array<char, 32U> text{};
    const int length = std::snprintf(text.data(), text.size(), format,
                                     static_cast<double>(value));
    ctx.label(gui::Rect{track.x + track.width + 8.0F, y, value_width, 22.0F},
              std::string_view{text.data(),
                               static_cast<std::size_t>(std::max(length, 0))},
              ctx.theme().keyframe);
    ctx.label(gui::Rect{card.x + 14.0F + label_width, y + 21.0F,
                        card.width - 28.0F - label_width, 16.0F},
              note, ctx.theme().text_muted);
    y += row_pitch;
  };

  if (category_ == Category::Shadows) {
    section("SHADOW STABILITY");
    if (renderer_draft_.backend == scene::RendererBackend::Advanced) {
      scalar_row("Cascade blend", renderer_draft_.shadow_blend_band, 0.0F, 0.25F, "%.3f",
                 "Fraction of each map blended into the next cascade.");
      scalar_row("Shadow filter radius", renderer_draft_.shadow_filter_radius, 0.0F, 4.0F, "%.1f px",
                 "Fixed 5 by 5 filter; sun size adds softness.");
      scalar_row("Contact shadow length", renderer_draft_.contact_shadow_length, 0.0F, 10.0F, "%.3f m",
                 "Depth ray to the first sun and shadowed spots. Zero is off.");
      if (renderer_draft_.contact_shadow_length > 0.0F) {
        scalar_row("Contact thickness", renderer_draft_.contact_shadow_thickness, 0.0F, 1.0F, "%.3f m",
                   "Occluder thickness. Screen reach is limited to 32 px.");
      }
    } else {
      ctx.label(gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 22.0F},
                "Select Advanced in Renderer to use shadows.", ctx.theme().text_muted);
    }
  }

  if (category_ == Category::Renderer) {
    section("RENDERER");
    const bool advanced =
        renderer_draft_.backend == scene::RendererBackend::Advanced;
    ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 22.0F}, "Backend",
              ctx.theme().text);
    const float renderer_x = card.x + 14.0F + label_width;
    if (preferences_button(ctx, mix(id_, "renderer.basic"),
                           gui::Rect{renderer_x, y, 116.0F, 22.0F}, "BASIC",
                           advanced)) {
      renderer_draft_.backend = scene::RendererBackend::Basic;
    }
    if (preferences_button(ctx, mix(id_, "renderer.advanced"),
                           gui::Rect{renderer_x + 124.0F, y, 116.0F, 22.0F},
                           "ADVANCED", !advanced)) {
      renderer_draft_.backend = scene::RendererBackend::Advanced;
    }
    ctx.label(gui::Rect{renderer_x, y + 21.0F, card.width - 28.0F - label_width,
                        16.0F},
              !advanced
                  ? "Proven SDL GPU renderer. The passes below need Advanced "
                    "and do nothing here."
                  : state.passes.fell_back()
                  ? "Authored Advanced; RUNNING BASIC. Everything below is "
                    "inert. Set Availability to REQUIRED to see why."
                  : "Capability checked on every render node.",
              ctx.theme().text_muted);
    y += row_pitch;


    if (renderer_draft_.backend == scene::RendererBackend::Advanced) {
      ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 22.0F},
                "Availability", ctx.theme().text);
      if (preferences_button(ctx, mix(id_, "renderer.optional"),
                             gui::Rect{renderer_x, y, 116.0F, 22.0F},
                             "OPTIONAL", renderer_draft_.required)) {
        renderer_draft_.required = false;
      }
      if (preferences_button(ctx, mix(id_, "renderer.required"),
                             gui::Rect{renderer_x + 124.0F, y, 116.0F, 22.0F},
                             "REQUIRED", !renderer_draft_.required)) {
        renderer_draft_.required = true;
      }
      ctx.label(gui::Rect{renderer_x, y + 21.0F,
                          card.width - 28.0F - label_width, 16.0F},
                renderer_draft_.required
                    ? "Program stays unavailable until every node proves it."
                    : "Unsupported nodes report an explicit Basic fallback.",
                ctx.theme().text_muted);
      y += row_pitch;

      ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 22.0F}, "Profile",
                ctx.theme().text);
      constexpr float profile_width = 96.0F;
      const auto profile_button = [&](std::string_view id,
                                      std::string_view label,
                                      scene::RendererQuality quality, float x) {
        if (preferences_button(ctx, mix(id_, id),
                               gui::Rect{x, y, profile_width, 22.0F}, label,
                               renderer_draft_.quality != quality)) {
          renderer_draft_.quality = quality;
        }
      };
      profile_button("renderer.interactive", "INTERACTIVE",
                     scene::RendererQuality::Interactive, renderer_x);
      profile_button("renderer.balanced", "BALANCED",
                     scene::RendererQuality::Balanced,
                     renderer_x + profile_width + 8.0F);
      profile_button("renderer.cinematic", "CINEMATIC",
                     scene::RendererQuality::Cinematic,
                     renderer_x + (profile_width + 8.0F) * 2.0F);
      // Temporal antialiasing is deliberately not here any more. The escape
      // hatch exists to clear requests for passes that do not exist; the TAA
      // resolve does, so clearing it would be throwing away a working feature
      // rather than unblocking the renderer.
      const bool future_passes_requested =
          renderer_draft_.ray_tracing || renderer_draft_.ddgi ||
          renderer_draft_.restir || renderer_draft_.denoising;
      if (future_passes_requested) {
        constexpr float raster_button_width = 104.0F;
        const gui::Rect raster_button{card.x + card.width - 14.0F -
                                          raster_button_width,
                                      y + 24.0F, raster_button_width, 18.0F};
        ctx.label(gui::Rect{renderer_x, y + 21.0F,
                            std::max(raster_button.x - renderer_x - 8.0F, 0.0F),
                            18.0F},
                  "FUTURE PASSES REQUESTED", ctx.theme().warning);
        if (preferences_button(ctx, mix(id_, "renderer.raster_only"),
                               raster_button, "USE RASTER", true)) {
          // Advanced raster already includes deterministic directional shadows
          // and bounded RSM GI. Clearing these authored future-pass requests is
          // an explicit project edit; the readiness gate never ignores them.
          renderer_draft_.ray_tracing = false;
          renderer_draft_.ddgi = false;
          renderer_draft_.restir = false;
          renderer_draft_.denoising = false;
        }
      } else {
        ctx.label(gui::Rect{renderer_x, y + 21.0F,
                            card.width - 28.0F - label_width, 16.0F},
                  "Current raster: directional shadows and bounded RSM GI.",
                  ctx.theme().text_muted);
      }
      y += row_pitch;

      // Both of these change what every node draws, so they are project state
      // beside the backend rather than a per-machine quality knob.
      float cascades = static_cast<float>(renderer_draft_.shadow_cascades);
      scalar_row("Shadow cascades", cascades, 0.0F,
                 static_cast<float>(scene::max_shadow_cascades), "%.0f",
                 "Directional shadow maps, near to far. Zero draws none.");
      renderer_draft_.shadow_cascades = static_cast<core::u32>(cascades + 0.5F);

      scalar_row("Bounced light", renderer_draft_.gi_strength, 0.0F,
                 scene::max_gi_strength, "%.2f",
                 "Strength of the one-bounce diffuse term. Zero is off.");

      // Antialiasing is two authored booleans and one decision. The buttons
      // set them so that only one can be on, which keeps what the operator
      // sees and what `scene::antialiasing_for` decides the same thing.
      ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 22.0F},
                "Antialiasing", ctx.theme().text);
      constexpr float aa_width = 96.0F;
      const auto aa_button = [&](std::string_view id, std::string_view label,
                                 scene::AntialiasingMode mode, float x) {
        if (preferences_button(
                ctx, mix(id_, id), gui::Rect{x, y, aa_width, 22.0F}, label,
                scene::antialiasing_for(renderer_draft_) != mode)) {
          renderer_draft_.taau = mode == scene::AntialiasingMode::Temporal;
          renderer_draft_.spatial_antialiasing =
              mode == scene::AntialiasingMode::Spatial;
        }
      };
      aa_button("renderer.aa_none", "NONE", scene::AntialiasingMode::None,
                renderer_x);
      aa_button("renderer.aa_spatial", "SPATIAL",
                scene::AntialiasingMode::Spatial,
                renderer_x + aa_width + 8.0F);
      aa_button("renderer.aa_temporal", "TEMPORAL",
                scene::AntialiasingMode::Temporal,
                renderer_x + (aa_width + 8.0F) * 2.0F);
      ctx.label(gui::Rect{renderer_x, y + 21.0F,
                          card.width - 28.0F - label_width, 16.0F},
                scene::antialiasing_for(renderer_draft_)
                        == scene::AntialiasingMode::Temporal
                    ? "Depth-aware history for thin geometry and contact shadows."
                    : scene::antialiasing_for(renderer_draft_)
                            == scene::AntialiasingMode::Spatial
                        ? "Softens edges only. Cannot ghost."
                        : "Geometry edges are left as hard steps.",
                ctx.theme().text_muted);
      y += row_pitch;

      if (renderer_draft_.taau) {
        float period =
            static_cast<float>(renderer_draft_.taa_sequence_length);
        scalar_row("Jitter samples", period,
                   static_cast<float>(scene::min_taa_sequence_length),
                   static_cast<float>(scene::max_taa_sequence_length), "%.0f",
                   "Sub-pixel positions before the sequence repeats.");
        renderer_draft_.taa_sequence_length =
            static_cast<core::u32>(period + 0.5F);
      }

      // Screen-space occlusion. Zero radius is the authored off, which is why
      // this is a slider from zero rather than a toggle plus a value: the two
      // would be two ways to say the same thing and could disagree.
      scalar_row("Occlusion radius", renderer_draft_.occlusion_radius, 0.0F,
                 8.0F, "%.2f",
                 "World units searched for contact. Zero is off.");
      if (renderer_draft_.occlusion_radius > 0.0F) {
        scalar_row("Occlusion strength", renderer_draft_.occlusion_strength,
                   0.0F, scene::max_occlusion_strength, "%.2f",
                   "How dark a fully occluded crease goes.");
      }

      // Bloom. One control, because the chain's depth belongs to the canvas -
      // a per-node radius is the one thing that would make two halves of the
      // wall disagree, so it is not offered.
      scalar_row("Bloom", renderer_draft_.bloom_strength, 0.0F,
                 scene::max_bloom_strength, "%.2f",
                 "How much highlight spill is added back. Zero is off.");

    // What the renderer actually did with all of this, as opposed to what the
    // controls above ask for.
    //
    // The two are not the same thing and nothing on screen distinguishes them:
    // a pass the selected backend cannot run, a pass that refused, and a pass
    // that ran and did nothing visible all produce the same frame. Every one of
    // these has been silently absent at some point - bloom was authored at full
    // strength for an afternoon while the compositor read a texture written
    // before it ran.
    //
    // Two lines rather than a row per pass: the card has room for two, and a
    // list of six mostly-fine states buries the one that is not.
    {
      const render::RendererPassStatus &passes = state.passes;
      const bool fell_back = passes.fell_back();
      ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 22.0F}, "Running",
                ctx.theme().text);
      ctx.label(
          gui::Rect{renderer_x, y, card.width - 28.0F - label_width, 22.0F},
          passes.selected == scene::RendererBackend::Advanced ? "ADVANCED"
                                                              : "BASIC",
          fell_back ? ctx.theme().warning : ctx.theme().keyframe);

      struct PassRow final {
        std::string_view label;
        render::PassState state;
        // Why it refused or is idle in this view.
        std::string_view reason;
      };
      const std::array<PassRow, 9U> pass_rows{{
          {"plate depth of field", passes.plate_dof, passes.plate_dof_reason},
          {"object motion blur", passes.motion_blur, passes.motion_blur_reason},
          {"shadows", passes.shadows, {}},
          {"bounced light", passes.bounced_light, {}},
          {"temporal AA", passes.temporal_antialiasing, {}},
          {"spatial AA", passes.spatial_antialiasing, {}},
          {"occlusion", passes.occlusion, {}},
          {"bloom", passes.bloom, passes.bloom_refusal},
          // Authored in the Environment category rather than this one, and
          // reported here anyway: an operator asking "is anything actually
          // running" wants one list, not one per tab.
          {"fog", passes.fog, passes.fog_refusal},
      }};
      std::array<char, 200U> active{};
      std::array<char, 200U> idle{};
      int active_length = std::snprintf(active.data(), active.size(), "running:");
      int idle_length = std::snprintf(
          idle.data(), idle.size(), "%s:",
          fell_back ? "inert until Advanced is selected" : "NOT RUNNING");
      const auto append = [](std::array<char, 200U> &buffer, int &length,
                             std::string_view what, bool first) {
        if (length < 0) {
          return;
        }
        const int written = std::snprintf(
            buffer.data() + length,
            buffer.size() - static_cast<std::size_t>(length),
            first ? " %.*s" : ", %.*s", static_cast<int>(what.size()),
            what.data());
        if (written > 0) {
          length += written;
        }
      };
      bool any_active = false;
      bool any_idle = false;
      for (const PassRow &row : pass_rows) {
        if (row.state == render::PassState::Active) {
          append(active, active_length, row.label, !any_active);
          any_active = true;
        } else if (row.state != render::PassState::NotAuthored &&
                   row.state != render::PassState::Idle) {
          append(idle, idle_length, row.label, !any_idle);
          any_idle = true;
        }
      }
      if (any_active) {
        ctx.label(gui::Rect{renderer_x, y + 21.0F,
                            card.width - 28.0F - label_width, 16.0F},
                  std::string_view{active.data(),
                                   static_cast<std::size_t>(
                                       std::max(active_length, 0))},
                  ctx.theme().text_muted);
      }
      y += any_active ? row_pitch : 24.0F;
      if (any_idle) {
        // The only line here that is a fault, and the only one in the warning
        // colour. If everything authored is running, nothing is shown.
        ctx.label(gui::Rect{renderer_x, y, card.width - 28.0F - label_width,
                            16.0F},
                  std::string_view{idle.data(), static_cast<std::size_t>(
                                                    std::max(idle_length, 0))},
                  ctx.theme().warning);
        y += 20.0F;
        // And why, for the passes that can say. A counter tells an operator
        // that bloom stopped; only this tells them the slice has to start on
        // a 64-pixel boundary, which is the difference between a fault and a
        // fix. One line per reason, across the card's full width.
        for (const PassRow &row : pass_rows) {
          if (row.state != render::PassState::Refused || row.reason.empty()) {
            continue;
          }
          std::array<char, 200U> line{};
          const int written = std::snprintf(
              line.data(), line.size(), "%.*s: %.*s",
              static_cast<int>(row.label.size()), row.label.data(),
              static_cast<int>(row.reason.size()), row.reason.data());
          ctx.label(gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 16.0F},
                    std::string_view{line.data(), static_cast<std::size_t>(
                                                      std::max(written, 0))},
                    ctx.theme().warning);
          y += 18.0F;
        }
      }
      // A plate-only pass is normally idle in this ordinary editor view.
      // Explain that scope without presenting it as a renderer fault.
      for (const PassRow &row : pass_rows) {
        if (row.state != render::PassState::Idle) {
          continue;
        }
        std::array<char, 200U> line{};
        const int written = std::snprintf(
            line.data(), line.size(), "%.*s: idle%s%.*s",
            static_cast<int>(row.label.size()), row.label.data(),
            row.reason.empty() ? "" : " - ",
            static_cast<int>(row.reason.size()), row.reason.empty() ? "" : row.reason.data());
        ctx.label(gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 16.0F},
                  std::string_view{line.data(), std::min(line.size() - 1U,
                      static_cast<std::size_t>(std::max(written, 0)))},
                  ctx.theme().text_muted);
        y += 18.0F;
      }
    }
    }
  }

  if (category_ == Category::Geometry) {
    section("GEOMETRY");
    const auto geometry_toggle = [&](std::string_view id, std::string_view label,
                                     bool& value, std::string_view note) {
      ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 22.0F}, label, ctx.theme().text);
      const float x = card.x + 14.0F + label_width;
      if (preferences_button(ctx, mix(id_, id), gui::Rect{x, y, 116.0F, 22.0F},
                             value ? "ON" : "OFF", true)) value = !value;
      ctx.label(gui::Rect{x, y + 21.0F, card.width - label_width - 28.0F, 16.0F},
                note, ctx.theme().text_muted);
      y += row_pitch;
    };
    geometry_toggle("geometry.culling", "Frustum culling", renderer_draft_.frustum_culling,
                    "Skips geometry outside the canvas. Shadow casters stay.");
    geometry_toggle("geometry.instancing", "Instancing", renderer_draft_.geometry_instancing,
                    "Draws repeated mesh and model parts together.");
    geometry_toggle("geometry.lod", "Model LOD", renderer_draft_.model_lod,
                    "Uses authored levels by full-canvas pixel size.");
    if (renderer_draft_.backend == scene::RendererBackend::Advanced) {
      section("TEXTURE RESIDENCY");
      geometry_toggle("geometry.streaming", "Mip streaming", renderer_draft_.texture_streaming,
                      "Selects the same texture detail on every output node.");
      float budget = static_cast<float>(renderer_draft_.texture_budget_mib);
      scalar_row("Texture memory", budget, 1.0F, 512.0F, "%.0f MiB",
                 "Includes replacement headroom; half holds the working set.");
      renderer_draft_.texture_budget_mib = static_cast<core::u32>(budget + 0.5F);
      if (renderer_draft_.texture_streaming) {
        float bias = static_cast<float>(renderer_draft_.texture_mip_bias);
        scalar_row("Mip bias", bias, 0.0F, 12.0F, "%.0f",
                   "0 follows canvas size. Higher values use softer textures.");
        renderer_draft_.texture_mip_bias = static_cast<core::u32>(bias + 0.5F);
      }
    }
    section("TEXTURE SAMPLING");
    ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 22.0F}, "Anisotropy", ctx.theme().text);
    constexpr std::array<core::u32, 5U> sampling_levels{1U, 2U, 4U, 8U, 16U};
    constexpr std::array<std::string_view, 5U> sampling_labels{"1x", "2x", "4x", "8x", "16x"};
    for (std::size_t i = 0U; i < sampling_levels.size(); ++i) {
      if (preferences_button(ctx, mix(id_, sampling_labels[i]),
            gui::Rect{card.x + 14.0F + label_width + static_cast<float>(i) * 42.0F, y, 38.0F, 22.0F},
            sampling_labels[i], renderer_draft_.texture_anisotropy != sampling_levels[i]))
        renderer_draft_.texture_anisotropy = sampling_levels[i];
    }
    ctx.label(gui::Rect{card.x + 14.0F + label_width, y + 21.0F, card.width - label_width - 28.0F, 16.0F},
              "Sharper grazing surfaces. Authored nearest filters stay nearest.", ctx.theme().text_muted);
    y += row_pitch;
    scalar_row("Sampling LOD bias", renderer_draft_.texture_lod_bias, -4.0F, 4.0F, "%.2f",
               "Negative sharpens; positive softens the resident texture mips.");
    section("FILTER GUARD");
    ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 22.0F}, "Guard limit", ctx.theme().text);
    constexpr std::array<core::u32, 4U> guard_limits{256U, 512U, 1024U, 2048U};
    constexpr std::array<std::string_view, 4U> guard_labels{"256 px", "512 px", "1024 px", "2048 px"};
    for (std::size_t i = 0U; i < guard_limits.size(); ++i) {
      if (preferences_button(ctx, mix(id_, guard_labels[i]),
          gui::Rect{card.x + 14.0F + label_width + static_cast<float>(i) * 72.0F, y, 68.0F, 22.0F},
          guard_labels[i], renderer_draft_.slice_apron_limit != guard_limits[i]))
        renderer_draft_.slice_apron_limit = guard_limits[i];
    }
    ctx.label(gui::Rect{card.x + 14.0F + label_width, y + 21.0F, card.width - label_width - 28.0F, 16.0F},
              "Use at least 512 px for temporal AA, occlusion and plate bloom.", ctx.theme().text_muted);
    y += row_pitch;
    section("OBJECT MOTION BLUR");
    geometry_toggle("geometry.motion_blur", "Object motion blur", renderer_draft_.motion_blur,
                    "Uses camera shutter duration. Camera movement stays sharp.");
    if (renderer_draft_.motion_blur) {
      float radius = static_cast<float>(renderer_draft_.motion_blur_radius);
      scalar_row("Maximum radius", radius, 0.0F, static_cast<float>(scene::max_motion_blur_radius), "%.0f px",
                 "Centred streak support. Zero is off; camera Properties sets Shutter s.");
      renderer_draft_.motion_blur_radius = static_cast<core::u32>(radius + 0.5F);
    }
    section("PLATE DEPTH OF FIELD");
    geometry_toggle("geometry.plate_dof", "Plate depth of field", renderer_draft_.plate_depth_of_field,
                    "Tracked focus and iris, with physical wall blur subtracted.");
    if (renderer_draft_.plate_depth_of_field) {
      float radius = static_cast<float>(renderer_draft_.plate_dof_radius);
      scalar_row("Plate maximum radius", radius, 0.0F, static_cast<float>(scene::max_plate_dof_radius), "%.0f px",
                 "Plate pixels. Camera Focus m and Aperture f/ supply missing lens values.");
      renderer_draft_.plate_dof_radius = static_cast<core::u32>(radius + 0.5F);
    }
    section("MODEL LEVELS");
    const auto note = [&](std::string_view text) {
      ctx.label(gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 20.0F}, text, ctx.theme().text_muted);
      y += 22.0F;
    };
    note("Import the finest model with its .lod companion already beside it.");
    note("Example: set.glb.lod starts with MSLOD 1, then: 256 \"set-low.glb\"");
    note("Each next line has a smaller pixel threshold and a quoted relative model path.");
    note("Up to eight levels. After editing files, restart the application to reload.");
    note("Mapped views and tracked plates retain geometry beyond the authored camera.");
    y += 8.0F;
    section("LAST RENDER");
    const auto& geometry = state.passes.geometry;
    std::array<char, 180U> summary{};
    std::snprintf(summary.data(), summary.size(), "%u visible / %u culled; %u draws (%u instanced)",
                  geometry.visible_geometry, geometry.culled_geometry,
                  geometry.batches, geometry.instanced_batches);
    note(summary.data());
    std::snprintf(summary.data(), summary.size(), "%u models with levels; %u using lower detail; %u refused instances",
                  geometry.lod_models, geometry.lod_reductions, geometry.dropped_instances);
    note(summary.data());
    const auto& textures = geometry.texture_streaming;
    if (textures.textures != 0U || textures.state != render::TextureStreamState::Ready) {
      std::snprintf(summary.data(), summary.size(), "%u textures (%u reduced); %.1f MiB resident / %.1f MiB allocated",
                    textures.textures, textures.reduced,
                    static_cast<double>(textures.resident_bytes) / 1048576.0,
                    static_cast<double>(textures.allocated_bytes) / 1048576.0);
      note(summary.data());
      switch (textures.state) {
      case render::TextureStreamState::Ready: break;
      case render::TextureStreamState::Loading: note("Texture mips are loading. Complete textures stay in use."); break;
      case render::TextureStreamState::CapacityRefused: note("Texture tails exceed capacity. Increase texture memory or reduce the set."); break;
      case render::TextureStreamState::AllocationFailed: note("Texture allocation failed; output held. Reload the source after freeing memory."); break;
      case render::TextureStreamState::UploadFailed: note("Texture upload failed; output held. Reload the source to retry."); break;
      case render::TextureStreamState::Busy: note("Texture replacement is waiting for GPU resources; output held."); break;
      }
    }
    if (geometry.models_pending != 0U) note("Model data is loading.");
    if (geometry.models_failed != 0U) note("Model load failed. Check the source and companion, then restart.");
    geometry_content_height_ = y + geometry_scroll_ - content_bounds.y;
  }

  if (category_ == Category::Image) {
    section("GAUSSIAN BLUR");
    float taps = static_cast<float>(draft_.blur_taps);
    scalar_row("Kernel taps", taps, static_cast<float>(scene::min_blur_taps),
               static_cast<float>(scene::max_blur_taps), "%.0f",
               "Samples per axis. Wider is smoother and costs more.");
    draft_.blur_taps = static_cast<core::u32>(taps + 0.5F);
    scalar_row("Sigma / radius", draft_.blur_sigma_ratio,
               scene::min_blur_sigma_ratio, scene::max_blur_sigma_ratio, "%.2f",
               "Low reads as a box blur; high truncates the Gaussian's tail.");
    float passes = static_cast<float>(draft_.blur_passes);
    scalar_row("Passes", passes, 1.0F,
               static_cast<float>(scene::max_blur_passes), "%.0f",
               "Repeat the separable pair. Smooths a very wide radius.");
    draft_.blur_passes = static_cast<core::u32>(passes + 0.5F);

    y += 4.0F;
    section("DIRECTIONAL BLUR");
    float directional_taps = static_cast<float>(draft_.directional_blur_taps);
    scalar_row("Line taps", directional_taps,
               static_cast<float>(scene::min_directional_blur_taps),
               static_cast<float>(scene::max_directional_blur_taps), "%.0f",
               "Odd samples across the complete centred line kernel.");
    core::u32 rounded_directional_taps =
        static_cast<core::u32>(directional_taps + 0.5F);
    if ((rounded_directional_taps & 1U) == 0U) {
      rounded_directional_taps = std::min(rounded_directional_taps + 1U,
                                          scene::max_directional_blur_taps);
    }
    draft_.directional_blur_taps = rounded_directional_taps;

    y += 4.0F;
    section("BRIGHTNESS");
    const bool gain = draft_.brightness_curve == scene::BrightnessCurve::Gain;
    ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 22.0F}, "Curve",
              ctx.theme().text);
    const float curve_x = card.x + 14.0F + label_width;
    if (preferences_button(ctx, mix(id_, "curve.exposure"),
                           gui::Rect{curve_x, y, 116.0F, 22.0F}, "EXPOSURE",
                           gain)) {
      draft_.brightness_curve = scene::BrightnessCurve::Exposure;
    }
    if (preferences_button(ctx, mix(id_, "curve.gain"),
                           gui::Rect{curve_x + 124.0F, y, 116.0F, 22.0F},
                           "GAIN", !gain)) {
      draft_.brightness_curve = scene::BrightnessCurve::Gain;
    }
    ctx.label(
        gui::Rect{curve_x, y + 21.0F, card.width - 28.0F - label_width, 16.0F},
        gain ? "Scales the finished picture. Clips, like a mixer's gain."
             : "Scales light before the tone map. Highlights roll off.",
        ctx.theme().text_muted);
    y += row_pitch;
    scalar_row("Response gamma", draft_.brightness_gamma,
               scene::min_brightness_gamma, scene::max_brightness_gamma, "%.2f",
               "The exponent the effect's multiplier is raised to.");
  }

  // Colour at the output boundary. Project state like everything else here:
  // two nodes grading the two halves of one wall differently is a seam nothing
  // downstream can hide.
  if (category_ == Category::Color) {
    section("DISPLAY TRANSFORM");
    scene::ColorSettings &color = renderer_draft_.color;
    const bool srgb =
        color.display_transform == scene::DisplayTransform::Srgb;
    ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 22.0F}, "Transform",
              ctx.theme().text);
    const float color_x = card.x + 14.0F + label_width;
    if (preferences_button(ctx, mix(id_, "color.srgb"),
                           gui::Rect{color_x, y, 116.0F, 22.0F}, "SRGB",
                           !srgb)) {
      color.display_transform = scene::DisplayTransform::Srgb;
    }
    if (preferences_button(ctx, mix(id_, "color.aces"),
                           gui::Rect{color_x + 124.0F, y, 116.0F, 22.0F},
                           "ACES APPROX", srgb)) {
      color.display_transform = scene::DisplayTransform::AcesApproximate;
    }
    ctx.label(gui::Rect{color_x, y + 21.0F, card.width - 28.0F - label_width,
                        16.0F},
              srgb ? "Straight sRGB encode. Highlights clip at white; the "
                     "reference."
                   : "Filmic shoulder in AP1, then the sRGB encode. "
                     "Highlights roll off.",
              ctx.theme().text_muted);
    y += row_pitch;

    section("SHOW LOOK");
    ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 22.0F}, "Look LUT",
              ctx.theme().text);
    const gui::Rect look_field{color_x, y,
                               std::max(card.width - 28.0F - label_width, 1.0F),
                               22.0F};
    static_cast<void>(ctx.text_field(mix(id_, "color.look"), look_field,
                                     color.show_lut_source));
    if (color.show_lut_source.empty() &&
        ctx.focus() != mix(id_, "color.look")) {
      ctx.label(gui::Rect{look_field.x + 6.0F, look_field.y,
                          look_field.width - 12.0F, look_field.height},
                "Path to a .cube, applied after the transform",
                ctx.theme().text_muted);
    }
    // What the output is doing with the *applied* colour, not with this
    // draft. The two differ while a table bakes, and permanently when the
    // path is wrong - which is the case that otherwise looks identical to no
    // look at all.
    const bool color_healthy = state.color_status == ColorStatus::Ready ||
                               state.color_status == ColorStatus::Loading;
    ctx.label(gui::Rect{color_x, y + 21.0F, card.width - 28.0F - label_width,
                        16.0F},
              state.color_status == ColorStatus::Ready
                  ? (state.renderer.color.show_lut_source.empty()
                         ? "Transform baked and live on the output."
                         : "Transform and look baked; live on the output.")
                  : color_status_text(state.color_status),
              color_healthy ? ctx.theme().text_muted : ctx.theme().warning);
    y += row_pitch;

    section("OUTPUT");
    static_cast<void>(ctx.checkbox(
        mix(id_, "color.dither"),
        gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 24.0F}, color.dither,
        "Dither to the 8-bit output"));
    ctx.label(gui::Rect{card.x + 14.0F, y + 24.0F, card.width - 28.0F, 16.0F},
              "A fixed pattern keyed to the wall pixel, so every node rounds "
              "the pixel it shares the same way.",
              ctx.theme().text_muted);
    y += row_pitch;
  }

    if (category_ == Category::Color) {
      section("PHOTOMETRIC LIGHTING");
      ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 22.0F},
                "Light scale", ctx.theme().text);
      const gui::Rect field{card.x + 14.0F + label_width, y,
                            card.width - 28.0F - label_width, 22.0F};
      double edited_scale{};
      if (tracking_fields_.draw(ctx, mix(id_, "lighting.scale"), field, "",
              renderer_draft_.photometric_scale, 0.00001, edited_scale, 6)
          && std::isfinite(edited_scale)) {
        renderer_draft_.photometric_scale = static_cast<float>(
            std::clamp(edited_scale, 0.000001, 1.0));
      }
      ctx.label(gui::Rect{field.x, y + 21.0F, field.width, 16.0F},
                "Default 0.003142: 1,000 lux puts an 18% card near mid-grey.",
                ctx.theme().text_muted);
      y += row_pitch;
    }

  if (category_ == Category::Environment) {
    if (preferences_button(ctx, mix(id_, "environment.sky-page"),
        gui::Rect{card.x + 14.0F, y + 6.0F, 140.0F, 24.0F}, "SKY AND FOG", baked_page_)) baked_page_ = false;
    if (preferences_button(ctx, mix(id_, "environment.baked-page"),
        gui::Rect{card.x + 162.0F, y + 6.0F, 160.0F, 24.0F}, "BAKED LIGHTING", !baked_page_)) baked_page_ = true;
    y += 38.0F;
  }
  if (category_ == Category::Environment && baked_page_) {
    const float top = y;
    const float height = std::max(content_bounds.height - 38.0F, 1.0F);
    const float maximum_scroll = std::max(baked_content_height_ - height, 0.0F);
    if (content_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) baked_scroll_ -= ctx.wheel() * 36.0F;
    baked_scroll_ = std::clamp(baked_scroll_, 0.0F, maximum_scroll);
    ctx.push_clip(gui::Rect{card.x, top, card.width, height});
    y -= baked_scroll_;
    auto& baked = environment_draft_.baked;
    section("DIFFUSE LIGHTING BAKE");
    static_cast<void>(ctx.checkbox(mix(id_, "baked.enabled"),
        gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 24.0F}, baked.enabled, "Use baked diffuse lighting"));
    y += 30.0F;
    ctx.label(gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 20.0F},
        state.bake_status.empty() ? (state.renderer.backend == scene::RendererBackend::Basic
            ? "Select Advanced in Renderer to use a bake."
            : !state.environment.baked.enabled ? "Bake the Stage scene at its current frame."
            : state.environment.baked.source.empty() ? "Create a bake to light this scene."
            : state.passes.geometry.baked_ready ? "Baked field resident; direct lights remain live."
            : "Program waiting for the baked field to become resident.") : state.bake_status,
        ctx.theme().text_muted);
    y += 28.0F;
    ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 22.0F}, "Output base path", ctx.theme().text);
    static_cast<void>(ctx.text_field(mix(id_, "baked.destination"),
        gui::Rect{card.x + 14.0F + label_width, y, card.width - 28.0F - label_width, 22.0F}, bake_destination_));
    y += 30.0F;
    if (preferences_button(ctx, mix(id_, "baked.start"), gui::Rect{card.x + 14.0F, y, 160.0F, 26.0F},
        baked.source.empty() ? "BAKE LIGHTING" : "REBAKE LIGHTING",
        !state.bake_running && !bake_destination_.empty() && state.stage_node != doc::NodeId{} && state.renderer.backend == scene::RendererBackend::Advanced)) {
        result.bake = scene::clamped(baked);
        result.bake_destination = bake_destination_;
    }
    if (state.bake_running && preferences_button(ctx, mix(id_, "baked.cancel"),
        gui::Rect{card.x + 184.0F, y, 100.0F, 26.0F}, "STOP BAKE", true)) result.cancel_bake = true;
    y += 36.0F;
    scalar_row("Strength", baked.strength, 0.0F, 4.0F, "%.2f", "Diffuse ambient only. Environment reflections remain live.");
    section("PROBE VOLUME IN METRES");
    const auto coordinate_row = [&](std::string_view label, std::string_view id, scene::Vec3& value) {
      ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 24.0F}, label, ctx.theme().text);
      const float field_width = (card.width - 36.0F - label_width) / 3.0F;
      for (unsigned axis = 0; axis < 3; ++axis) {
        float& component = axis == 0 ? value.x : axis == 1 ? value.y : value.z;
        double edited{};
        if (tracking_fields_.draw(ctx, mix(id_, id, static_cast<core::u64>(axis + 1)),
            gui::Rect{card.x + 14.0F + label_width + static_cast<float>(axis) * (field_width + 3.0F), y, field_width, 24.0F},
            axis == 0 ? "X" : axis == 1 ? "Y" : "Z", component, 0.05, edited, 2) && std::isfinite(edited)) component = static_cast<float>(edited);
      }
      y += 32.0F;
    };
    coordinate_row("Minimum", "baked.min", baked.minimum);
    coordinate_row("Maximum", "baked.max", baked.maximum);
    const auto integer_row = [&](std::string_view label, core::u32& value, float lo, float hi, std::string_view note) {
      float draft = static_cast<float>(value); scalar_row(label, draft, lo, hi, "%.0f", note); value = static_cast<core::u32>(draft + 0.5F);
    };
    integer_row("Probes X", baked.count_x, 2, 16, "Uniform grid. Up to 4096 probes per field.");
    integer_row("Probes Y", baked.count_y, 2, 16, "Keep probes in the empty space around surfaces.");
    integer_row("Probes Z", baked.count_z, 2, 16, "Tighter spacing preserves more bounce detail.");
    integer_row("Samples", baked.samples, 64, 4096, "More paths reduce offline integration error.");
    integer_row("Bounces", baked.bounces, 1, 16, "Diffuse path depth. Relighting static content requires a rebake.");
    baked = scene::clamped(baked);
    baked_content_height_ = y + baked_scroll_ - top;
    ctx.pop_clip();
  }
  if (category_ == Category::Environment && !baked_page_) {
    const float top = y;
    const float height = std::max(content_bounds.height - 38.0F, 1.0F);
    const gui::Rect page_bounds{card.x, top, card.width, height};
    const float maximum_scroll = std::max(environment_content_height_ - height, 0.0F);
    if (page_bounds.contains(ctx.pointer_x(), ctx.pointer_y()))
      environment_scroll_ -= ctx.wheel() * 36.0F;
    environment_scroll_ = std::clamp(environment_scroll_, 0.0F, maximum_scroll);
    ctx.push_clip(page_bounds);
    y -= environment_scroll_;
    section("IMAGE-BASED LIGHTING");
    ctx.label(gui::Rect{card.x + 14.0F, y, card.width - 28.0F, 18.0F},
              "An equirectangular radiance map lights the scene and draws as "
              "the sky.",
              ctx.theme().text_muted);
    y += 22.0F;

    ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 22.0F}, "Map",
              ctx.theme().text);
    const gui::Rect map_field{card.x + 14.0F + label_width, y,
                              std::max(card.width - 28.0F - label_width, 1.0F),
                              22.0F};
    static_cast<void>(ctx.text_field(mix(id_, "environment.map"), map_field,
                                     environment_draft_.map_source));
    if (environment_draft_.map_source.empty()
        && ctx.focus() != mix(id_, "environment.map")) {
        ctx.label(gui::Rect{map_field.x + 6.0F, map_field.y,
                            map_field.width - 12.0F, map_field.height},
                  "Path to a .hdr", ctx.theme().text_muted);
    }
    y += 21.0F;
    // What the renderer is doing with it, not what this dialog holds. The two
    // differ for as long as a decode is in flight, and they differ permanently
    // when the path is wrong - which is the case that otherwise looks
    // identical to no map at all.
    const bool healthy = state.environment_status == EnvironmentStatus::Ready
        || state.environment_status == EnvironmentStatus::NotAuthored
        || state.environment_status == EnvironmentStatus::Loading;
    ctx.label(gui::Rect{card.x + 14.0F + label_width, y,
                        card.width - 28.0F - label_width, 16.0F},
              environment_status_text(state.environment_status),
              healthy ? ctx.theme().text_muted : ctx.theme().warning);
    y += row_pitch - 21.0F;

    scalar_row("Intensity", environment_draft_.map_intensity, 0.0F, 8.0F,
               "%.2f",
               "Multiplies the map. One is the radiance the file carries.");
    scalar_row("Rotation", environment_draft_.map_rotation, -1.0F, 1.0F,
               "%.3f",
               "Turns about the stage's up axis. Spins the environment "
               "without re-exporting it.");

    // Atmosphere, beside the sky it scatters. Content rather than renderer
    // policy, which is why it lives in this category and not the Renderer one.
    section("ATMOSPHERE");
    scalar_row("Fog density", environment_draft_.fog_density, 0.0F,
               scene::max_fog_density, "%.3f",
               "Optical depth per 10 metres. Zero turns fog off.");
    if (environment_draft_.fog_density > 0.0F) {
      ctx.label(gui::Rect{card.x + 14.0F, y, label_width, 22.0F},
                "Background distance", ctx.theme().text);
      double edited_distance{};
      if (tracking_fields_.draw(ctx, mix(id_, "environment.background-distance"),
              gui::Rect{card.x + 14.0F + label_width, y,
                        card.width - 28.0F - label_width, 22.0F},
              "m", environment_draft_.fog_background_distance, 0.1,
              edited_distance, 2) && std::isfinite(edited_distance)) {
        environment_draft_.fog_background_distance = static_cast<float>(
            std::clamp(edited_distance, 0.0,
                       static_cast<double>(scene::max_fog_background_distance)));
      }
      ctx.label(gui::Rect{card.x + 14.0F + label_width, y + 21.0F,
                          card.width - 28.0F - label_width, 16.0F},
                "Fog depth in metres. 0 = far clip. Try 5-10 indoors.",
                ctx.theme().text_muted);
      y += row_pitch;
      scalar_row("Fog anisotropy", environment_draft_.fog_anisotropy,
                 -scene::max_fog_anisotropy, scene::max_fog_anisotropy, "%.2f",
                 "Forward scattering. Positive haloes a light aimed at the "
                 "camera.");
      // Where the air thins. Zero falloff is uniform air, which is what every
      // scene had before these existed and what the default keeps.
      scalar_row("Fog height", environment_draft_.fog_height,
                 -10.0F, 10.0F, "%.2f",
                 "World height the air is densest at and below.");
      scalar_row("Height falloff", environment_draft_.fog_height_falloff,
                 0.0F, scene::max_fog_height_falloff, "%.2f",
                 "How fast the air thins above that height. Zero fills the "
                 "room.");
      scalar_row("Fog red", environment_draft_.fog_color.x, 0.0F, 1.0F, "%.3f",
                 "The colour the air returns, not the colour it removes.");
      scalar_row("Fog green", environment_draft_.fog_color.y, 0.0F, 1.0F,
                 "%.3f", "Near-white is air; warmer is dust or haze.");
      scalar_row("Fog blue", environment_draft_.fog_color.z, 0.0F, 1.0F,
                 "%.3f", "Scattering albedo, per channel.");
    }
    environment_content_height_ = y + environment_scroll_ - top;
    if (maximum_scroll > 0.0F) {
      const float thumb = height * height / environment_content_height_;
      ctx.panel(gui::Rect{card.x + card.width - 5.0F,
                         top + environment_scroll_ / maximum_scroll * (height - thumb),
                         3.0F, thumb}, ctx.theme().divider);
    }
    ctx.pop_clip();
  }

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
                 "Frames a pose is stamped ahead, covering camera and wall.");

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
      draft_ != scene::EffectSettings{} ||
          renderer_draft_ != scene::RendererSettings{} ||
          environment_draft_ != scene::Environment{} ||
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
    draft_ = scene::EffectSettings{};
    renderer_draft_ = scene::RendererSettings{};
    environment_draft_ = scene::Environment{};
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
  constexpr float scroll_content_height = 700.0F;
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

  const bool changed = draft_ != state.stored;
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
  }
  if (saved || cancelled || dismissed) {
    open_ = false;
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
  ctx.push_clip(bounds);
  ctx.panel(bounds);
  panel_title(ctx, bounds, "", frame_text(state.frame));

  const float table_y = bounds.y + title_height;
  constexpr float column_height = 18.0F;
  const gui::Rect columns{bounds.x, table_y, bounds.width, column_height};
  ctx.panel(columns, ctx.theme().header);
  const float name_x = columns.x + 8.0F;
  const float resolution_x = columns.x + columns.width * 0.39F;
  const float fps_x = columns.x + columns.width * 0.65F;
  const float status_x = columns.x + columns.width * 0.76F;
  ctx.label(gui::Rect{name_x, columns.y, resolution_x - name_x - 4.0F,
                      columns.height},
            "OUTPUT", ctx.theme().text_muted);
  ctx.label(gui::Rect{resolution_x, columns.y, fps_x - resolution_x - 4.0F,
                      columns.height},
            "RESOLUTION", ctx.theme().text_muted);
  ctx.label(
      gui::Rect{fps_x, columns.y, status_x - fps_x - 4.0F, columns.height},
      "FPS", ctx.theme().text_muted);
  ctx.label(gui::Rect{status_x, columns.y,
                      columns.x + columns.width - status_x - 4.0F,
                      columns.height},
            "STATUS", ctx.theme().text_muted);

  using ClusterHealth = OutputMonitorState::Cluster::Health;
  const bool cluster_visible =
      state.cluster.health != ClusterHealth::Unmonitored ||
      state.cluster.configured_nodes != 0U;
  const float conformance_height = state.cluster.conformance_active
      ? (conformance_expanded_ ? 528.0F : 46.0F) : 0.0F;
  const float base_program_height = cluster_visible ? 56.0F : 20.0F;
  const float program_row_height = base_program_height + conformance_height;
  const std::string primary_resolution =
      resolution_text(state.width, state.height);
  const std::string fps = fps_text(state.frames_per_second);
  constexpr float footer_height = 18.0F;
  const gui::Rect list_bounds{
      bounds.x, columns.y + columns.height, bounds.width,
      std::max(bounds.y + bounds.height - footer_height -
                   (columns.y + columns.height),
               0.0F)};
  // A third row per output only when the project actually claims physical
  // synchronization. An UNSYNCED show has nothing to report there and should
  // not pay the vertical space for an empty line.
  const float node_base_height = state.cluster.sync_required ? 56.0F : 40.0F;
  const float node_row_height = node_base_height + (state.cluster.conformance_active
      ? (conformance_expanded_ ? 478.0F : 54.0F) : 0.0F);
  const float content_height =
      program_row_height +
      node_row_height * static_cast<float>(state.nodes.size());
  const float maximum_scroll =
      std::max(content_height - list_bounds.height, 0.0F);
  if (list_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
    // Expanded evidence is much taller than the default dock. Keep individual
    // metadata and pass rows reachable with the wheel instead of skipping a node.
    scroll_ -= ctx.wheel() * (conformance_expanded_ ? 36.0F : node_row_height);
  }
  scroll_ = std::clamp(scroll_, 0.0F, maximum_scroll);
  float y = list_bounds.y - scroll_;
  ctx.push_clip(list_bounds);
  const gui::Rect program_row{bounds.x, y, bounds.width, program_row_height};
  ctx.panel(program_row, ctx.theme().row);
  ctx.label(
      gui::Rect{name_x, program_row.y, resolution_x - name_x - 4.0F, 20.0F},
      "PROGRAM PREVIEW", ctx.theme().text);
  ctx.label(gui::Rect{resolution_x, program_row.y, fps_x - resolution_x - 4.0F,
                      20.0F},
            primary_resolution, ctx.theme().text_muted);
  ctx.label(gui::Rect{fps_x, program_row.y, status_x - fps_x - 4.0F, 20.0F},
            fps, ctx.theme().text_muted);
  const gui::UiColor program_color =
      state.playing ? ctx.theme().good : ctx.theme().text_muted;
  draw_circle(ctx, status_x + 5.0F, program_row.y + 10.0F, 2.5F, program_color);
  ctx.label(gui::Rect{status_x + 12.0F, program_row.y, 50.0F, 20.0F},
            state.playing ? "PLAY" : "PAUSE", program_color);
  if (cluster_visible) {
    const auto health_label = [](ClusterHealth health) -> std::string_view {
      switch (health) {
      case ClusterHealth::Healthy:
        return "CONTROL HEALTHY";
      case ClusterHealth::Degraded:
        return "CONTROL DEGRADED";
      case ClusterHealth::Faulted:
        return "CONTROL FAULT";
      case ClusterHealth::Unmonitored:
        return "CONTROL UNMONITORED";
      }
      return "CONTROL FAULT";
    };
    const gui::UiColor health_color =
        state.cluster.health == ClusterHealth::Healthy
            ? ctx.theme().good
            : (state.cluster.health == ClusterHealth::Faulted
                   ? ctx.theme().warning
                   : ctx.theme().keyframe);
    std::string control{health_label(state.cluster.health)};
    control += " | LIVE ";
    control += std::to_string(state.cluster.live_sessions);
    control += '/';
    control += std::to_string(state.cluster.configured_nodes);
    if (state.cluster.catching_up_sessions != 0U) {
      control += " | CATCHING UP ";
      control += std::to_string(state.cluster.catching_up_sessions);
    }
    control += " | RELEASE LAG ";
    control += std::to_string(state.cluster.release_lag_frames);
    control += " | PACER LAG ";
    control += std::to_string(state.cluster.pacer_lag_frames);
    if (state.cluster.renderer_active) {
      control += " | ";
      control += state.cluster.renderer_headline;
      if (state.cluster.conformance_difference.reason != core::ConformanceMismatch::None) {
        control += ": ";
        control += core::conformance_detail(state.cluster.conformance_difference);
      }
      if (state.cluster.renderer_satisfied) {
        control +=
            state.cluster.renderer_selected_advanced ? " ADVANCED" : " BASIC";
        if (state.cluster.renderer_fallback) {
          control += " FALLBACK";
        }
      } else if (state.cluster.renderer_failure_local) {
        control += " EDITOR";
      } else {
        control += " NODE ";
        control += std::to_string(state.cluster.renderer_failure_node);
      }
    }
    const gui::Rect control_bounds{program_row.x + 8.0F, program_row.y + 20.0F,
                                   std::max(program_row.width - 16.0F, 0.0F),
                                   18.0F};
    ctx.push_clip(control_bounds);
    ctx.label(control_bounds, control,
              state.cluster.renderer_active && !state.cluster.renderer_satisfied
                  ? ctx.theme().warning
                  : health_color);
    ctx.pop_clip();

    // Physical timing is reported as three separate facts on one line: the
    // authored policy, exactly what the cluster can prove about it, and how
    // much of the group is actually locked and joined. A green control state
    // never changes any of them.
    const bool timing_verified = state.cluster.sync_satisfied &&
                                 !state.cluster.sync_degraded &&
                                 (!state.cluster.sync_required ||
                                  (state.cluster.external_time_sync_verified &&
                                   state.cluster.hardware_frame_lock_verified));
    std::string timing{"SYNC "};
    timing += state.cluster.sync_mode;
    timing += " | ";
    timing += state.cluster.sync_headline;
    if (state.cluster.sync_required) {
      timing += " | LOCKED ";
      timing += std::to_string(state.cluster.sync_locked_outputs);
      timing += '/';
      timing += std::to_string(state.cluster.sync_expected_outputs);
      timing += " | BARRIER ";
      timing += std::to_string(state.cluster.barrier_joined_outputs);
      timing += '/';
      timing += std::to_string(state.cluster.sync_expected_outputs);
      if (state.cluster.sync_groups != 0U) {
        timing += " | GROUPS ";
        timing += std::to_string(state.cluster.sync_groups);
      }
      if (state.cluster.sync_configuration_issues != 0U) {
        timing += " | CONFIG ISSUES ";
        timing += std::to_string(state.cluster.sync_configuration_issues);
      }
      if (state.cluster.sync_degraded) {
        timing += " | DEGRADED PROMOTION";
      }
    }
    const core::u64 incidents =
        state.cluster.heartbeat_timeouts + state.cluster.session_failures +
        state.cluster.barrier_rejections + state.cluster.present_failures;
    if (incidents != 0U) {
      timing += " | INCIDENTS ";
      timing += std::to_string(incidents);
    }
    timing += state.cluster.diagnostics_recording ? " | DIAG LOG ON"
                                                  : " | DIAG LOG OFF";
    const gui::Rect timing_bounds{program_row.x + 8.0F, program_row.y + 38.0F,
                                  std::max(program_row.width - 16.0F, 0.0F),
                                  18.0F};
    ctx.push_clip(timing_bounds);
    ctx.label(timing_bounds, timing,
              timing_verified
                  ? ctx.theme().good
                  : (state.cluster.sync_required ? ctx.theme().warning
                                                 : ctx.theme().text_muted));
    ctx.pop_clip();
  }
  if (state.cluster.conformance_active) {
    float cy = program_row.y + base_program_height;
    if (ctx.button(mix(id_, "conformance.check"),
        gui::Rect{program_row.x + 8.0F, cy, 168.0F, 20.0F}, "Check render cohort"))
      frame.check_render_conformance = true;
    if (ctx.button(mix(id_, "conformance.details"),
        gui::Rect{program_row.x + 184.0F, cy, 100.0F, 20.0F}, conformance_expanded_ ? "Hide passes" : "Pass details"))
      conformance_expanded_ = !conformance_expanded_;
    const auto& proof = state.cluster.conformance;
    std::string summary;
    switch (proof.state) {
    case core::ConformanceState::Unknown: summary = "Render check awaiting a Program device"; break;
    case core::ConformanceState::Running: summary = "Rendering fixed conformance scene on this machine"; break;
    case core::ConformanceState::Failed: summary = "Local render check failed: "; summary += core::conformance_detail({core::ConformanceMismatch::Failed, proof.failed_pass}); break;
    case core::ConformanceState::Passed: summary = "Local render check passed | "; summary += proof.strict_float ? "IEEE strict" : "experimental relaxed"; break;
    }
    ctx.label(gui::Rect{program_row.x + 8.0F, cy + 23.0F, program_row.width - 16.0F, 20.0F}, summary, ctx.theme().text_muted);
    if (conformance_expanded_) {
      cy += 46.0F;
      const auto version = proof.adapter.driver_version;
      ctx.label(gui::Rect{program_row.x + 8.0F, cy, program_row.width - 16.0F, 20.0F},
                proof.adapter.name.data(), ctx.theme().text_muted);
      cy += 20.0F;
      const std::string adapter_text = "Driver " +
          std::to_string(version >> 48U) + "." + std::to_string((version >> 32U) & 65535U) + "." +
          std::to_string((version >> 16U) & 65535U) + "." + std::to_string(version & 65535U);
      ctx.label(gui::Rect{program_row.x + 8.0F, cy, program_row.width - 16.0F, 20.0F}, adapter_text, ctx.theme().text_muted);
      cy += 20.0F;
      constexpr char hex[] = "0123456789abcdef";
      std::string bundle;
      for (auto byte : proof.shader_bundle) { bundle += hex[byte >> 4U]; bundle += hex[byte & 15U]; }
      ctx.label(gui::Rect{program_row.x + 8.0F, cy, program_row.width - 16.0F, 20.0F}, "Shader SHA-256", ctx.theme().text_muted);
      cy += 20.0F;
      for (std::size_t offset = 0; offset < bundle.size(); offset += 32U) {
        ctx.label(gui::Rect{program_row.x + 8.0F, cy, program_row.width - 16.0F, 20.0F},
                  std::string_view{bundle}.substr(offset, 32U), ctx.theme().text_muted);
        cy += 20.0F;
      }
      for (std::size_t i = 0; i < core::conformance_pass_count; ++i) {
        std::string row{core::conformance_pass_names[i]}; row += "  ";
        for (int shift = 60; shift >= 0; shift -= 4) row += hex[(proof.passes[i] >> shift) & 15U];
        const bool differs = static_cast<std::size_t>(state.cluster.conformance_difference.pass) == i;
        if (differs) row += "  FIRST DIFFERENCE";
        ctx.label(gui::Rect{program_row.x + 8.0F, cy, program_row.width - 16.0F, 18.0F}, row,
                  differs ? ctx.theme().warning : ctx.theme().text_muted);
        cy += 18.0F;
      }
    }
  }
  y += program_row_height;

  using NodeStatus = OutputMonitorState::Node::Status;
  using ContentStatus = OutputMonitorState::Node::ContentStatus;
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
      return "RUNNING";
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
      return ctx.theme().good;
    case NodeStatus::Starting:
    case NodeStatus::Stopping:
    case NodeStatus::Probing:
    case NodeStatus::Busy:
      return ctx.theme().keyframe;
    case NodeStatus::Faulted:
    case NodeStatus::AuthenticationFailed:
    case NodeStatus::ProtocolError:
    case NodeStatus::ConfigurationMismatch:
    case NodeStatus::InvalidConfiguration:
    case NodeStatus::DuplicateNodeIndex:
      return ctx.theme().warning;
    case NodeStatus::Disabled:
    case NodeStatus::Stopped:
    case NodeStatus::Unreachable:
      return ctx.theme().text_muted;
    }
    return ctx.theme().text_muted;
  };
  using NodeSync = OutputMonitorState::Node::SyncStatus;
  const auto sync_label = [](NodeSync status) -> std::string_view {
    switch (status) {
    case NodeSync::Unsynced:
      return "SYNC UNSYNCED";
    case NodeSync::Unverified:
      return "SYNC UNVERIFIED";
    case NodeSync::SyncDeviceMissing:
      return "SYNC DEVICE MISSING";
    case NodeSync::ReferenceLost:
      return "REFERENCE LOST";
    case NodeSync::ReferenceNotInUse:
      return "REFERENCE NOT IN USE";
    case NodeSync::Unlocked:
      return "OUTPUTS UNLOCKED";
    case NodeSync::BarrierIncomplete:
      return "SWAP BARRIER INCOMPLETE";
    case NodeSync::BarrierLost:
      return "SWAP BARRIER LOST";
    case NodeSync::ShowClockReferenceUnavailable:
      return "SHOW CLOCK REFERENCE UNAVAILABLE";
    case NodeSync::ShowClockReferenceLost:
      return "SHOW CLOCK REFERENCE LOST";
    case NodeSync::Simulated:
      return "SYNC SIMULATED";
    case NodeSync::Locked:
      return "SYNC LOCKED";
    }
    return "SYNC UNVERIFIED";
  };
  const auto sync_color = [&](NodeSync status) {
    switch (status) {
    case NodeSync::Locked:
      return ctx.theme().good;
    case NodeSync::Simulated:
    case NodeSync::Unverified:
      return ctx.theme().keyframe;
    case NodeSync::SyncDeviceMissing:
    case NodeSync::ReferenceLost:
    case NodeSync::ReferenceNotInUse:
    case NodeSync::Unlocked:
    case NodeSync::BarrierIncomplete:
    case NodeSync::BarrierLost:
    case NodeSync::ShowClockReferenceUnavailable:
    case NodeSync::ShowClockReferenceLost:
      return ctx.theme().warning;
    case NodeSync::Unsynced:
      return ctx.theme().text_muted;
    }
    return ctx.theme().text_muted;
  };
  const auto content_color = [&](ContentStatus status) {
    switch (status) {
    case ContentStatus::Ready:
      return ctx.theme().good;
    case ContentStatus::Checking:
      return ctx.theme().keyframe;
    case ContentStatus::Missing:
    case ContentStatus::Error:
      return ctx.theme().warning;
    case ContentStatus::NotRequired:
      return ctx.theme().text_muted;
    }
    return ctx.theme().text_muted;
  };

  for (std::size_t index = 0U; index < state.nodes.size(); ++index) {
    const OutputMonitorState::Node &node = state.nodes[index];
    const gui::Rect row{bounds.x, y, bounds.width, node_row_height};
    ctx.panel(row, index % 2U == 0U ? ctx.theme().row_alt : ctx.theme().row);
    const std::string identity =
        feed_output_identity(node.node_index) + "  " + node.name;
    const gui::Rect identity_bounds{name_x, row.y, resolution_x - name_x - 4.0F,
                                    20.0F};
    ctx.push_clip(identity_bounds);
    ctx.label(identity_bounds, identity, ctx.theme().text);
    ctx.pop_clip();
    ctx.label(
        gui::Rect{resolution_x, row.y, fps_x - resolution_x - 4.0F, 20.0F},
        resolution_text(node.width, node.height), ctx.theme().text_muted);
    ctx.label(gui::Rect{fps_x, row.y, status_x - fps_x - 4.0F, 20.0F}, fps,
              ctx.theme().text_muted);
    const gui::UiColor node_status_color = status_color(node.status);
    draw_circle(ctx, status_x + 5.0F, row.y + 10.0F, 2.5F, node_status_color);
    ctx.label(gui::Rect{status_x + 12.0F, row.y,
                        std::max(row.x + row.width - status_x - 16.0F, 0.0F),
                        20.0F},
              status_label(node.status), node_status_color);

    float button_right = row.x + row.width - 4.0F;
    const bool row_visible = intersect(row, list_bounds).height > 0.0F;
    const auto action_button = [&](std::string_view suffix,
                                   std::string_view label, float width,
                                   OutputLifecycleAction action,
                                   bool enabled = true) {
      button_right -= width;
      const gui::Rect button{button_right, row.y + 20.0F, width - 3.0F, 17.0F};
      const gui::UiId button_id =
          mix(id_, suffix,
              node.device.value() * 8U + static_cast<core::u64>(action));
      const bool clicked =
          enabled && row_visible && ctx.button(button_id, button, label);
      if (!enabled) {
        ctx.panel(button, scale(ctx.theme().widget, 0.65F));
        border(ctx, button, ctx.theme().divider);
        ctx.label(button, label, scale(ctx.theme().text_muted, 0.65F));
      }
      if (clicked && !frame.lifecycle.has_value()) {
        frame.lifecycle = OutputLifecycleRequest{node.device, node.node_index,
                                                 node.endpoint, action};
      }
      button_right -= 3.0F;
    };
    switch (node.status) {
    case NodeStatus::Stopped:
    case NodeStatus::Faulted:
      action_button("output.lifecycle.start", "START", 52.0F,
                    OutputLifecycleAction::Start, node.content_ready());
      break;
    case NodeStatus::Running:
      action_button("output.lifecycle.stop", "STOP", 47.0F,
                    OutputLifecycleAction::Stop);
      action_button("output.lifecycle.restart", "RESTART", 64.0F,
                    OutputLifecycleAction::Restart, node.content_ready());
      break;
    case NodeStatus::Unreachable:
    case NodeStatus::AuthenticationFailed:
    case NodeStatus::ProtocolError:
    case NodeStatus::ConfigurationMismatch:
      action_button("output.lifecycle.status", "RETRY", 52.0F,
                    OutputLifecycleAction::Status);
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
    const float detail_width = std::max(button_right - name_x - 4.0F, 0.0F);
    const std::string asset_status = output_node_content_label(node);
    std::string detail;
    if (!asset_status.empty()) {
      detail = asset_status;
      if (!node.content_detail.empty()) {
        detail += " - ";
        detail += node.content_detail;
      }
    } else {
      detail = node.detail.empty() ? node.endpoint : node.detail;
    }
    const gui::Rect detail_bounds{name_x, row.y + 20.0F, detail_width, 17.0F};
    ctx.push_clip(detail_bounds);
    ctx.label(detail_bounds, detail,
              asset_status.empty() ? ctx.theme().text_muted
                                   : content_color(node.content));
    ctx.pop_clip();
    if (state.cluster.sync_required) {
      std::string sync_line{sync_label(node.sync)};
      sync_line += " | LOCKED ";
      sync_line += std::to_string(node.sync_locked_outputs);
      sync_line += '/';
      sync_line += std::to_string(node.sync_expected_outputs);
      sync_line += " | BARRIER ";
      sync_line += std::to_string(node.barrier_joined_outputs);
      sync_line += '/';
      sync_line += std::to_string(node.barrier_expected_outputs);
      sync_line += " | PRESENT SYNC ";
      sync_line += std::to_string(node.barrier_presents_in_sync);
      sync_line += '/';
      sync_line += std::to_string(node.barrier_presents);
      sync_line += " | SHOW CLOCK ";
      sync_line += node.show_clock_reference;
      if (node.show_clock_reference == "LOCKED") {
        sync_line += " #";
        sync_line += std::to_string(node.show_clock_reference_frame);
      }
      sync_line += " | MISSED ";
      sync_line += std::to_string(node.missed_frames);
      sync_line += " LATE ";
      sync_line += std::to_string(node.late_frames);
      sync_line += " HELD ";
      sync_line += std::to_string(node.held_frames);
      sync_line += " REPEATED ";
      sync_line += std::to_string(node.repeated_frames);
      if (node.loss_of_sync_events != 0U) {
        sync_line += " | SYNC LOSS ";
        sync_line += std::to_string(node.loss_of_sync_events);
      }
      if (!node.sync_detail.empty()) {
        sync_line += " - ";
        sync_line += node.sync_detail;
      }
      const gui::Rect sync_bounds{
          name_x, row.y + 37.0F,
          std::max(row.x + row.width - name_x - 12.0F, 0.0F), 17.0F};
      ctx.push_clip(sync_bounds);
      ctx.label(sync_bounds, sync_line, sync_color(node.sync));
      ctx.pop_clip();
    }
    if (state.cluster.conformance_active) {
      const auto& proof = node.conformance;
      const auto difference = core::compare_render_conformance(state.cluster.conformance, proof);
      float cy = row.y + node_base_height;
      std::string summary = difference.reason == core::ConformanceMismatch::None ? "Render check matches" : "Render check: ";
      if (difference.reason != core::ConformanceMismatch::None) summary += core::conformance_detail(difference);
      ctx.label(gui::Rect{name_x, cy, row.width - 16.0F, 18.0F}, summary,
          difference.reason == core::ConformanceMismatch::None ? ctx.theme().good : ctx.theme().warning);
      const auto version = proof.adapter.driver_version;
      ctx.label(gui::Rect{name_x, cy + 18.0F, row.width - 16.0F, 18.0F},
                proof.adapter.name.data(), ctx.theme().text_muted);
      const std::string adapter_text = "Driver " +
          std::to_string(version >> 48U) + "." + std::to_string((version >> 32U) & 65535U) + "." +
          std::to_string((version >> 16U) & 65535U) + "." + std::to_string(version & 65535U);
      ctx.label(gui::Rect{name_x, cy + 36.0F, row.width - 16.0F, 18.0F}, adapter_text, ctx.theme().text_muted);
      if (conformance_expanded_) {
        cy += 54.0F;
        constexpr char hex[] = "0123456789abcdef";
        std::string bundle;
        for (auto byte : proof.shader_bundle) { bundle += hex[byte >> 4U]; bundle += hex[byte & 15U]; }
        ctx.label(gui::Rect{name_x, cy, row.width - 16.0F, 20.0F}, "Shader SHA-256", ctx.theme().text_muted); cy += 20.0F;
        for (std::size_t offset = 0; offset < bundle.size(); offset += 32U) {
          ctx.label(gui::Rect{name_x, cy, row.width - 16.0F, 20.0F},
                    std::string_view{bundle}.substr(offset, 32U), ctx.theme().text_muted);
          cy += 20.0F;
        }
        for (std::size_t i = 0; i < core::conformance_pass_count; ++i) {
          std::string pass{core::conformance_pass_names[i]}; pass += "  ";
          for (int shift = 60; shift >= 0; shift -= 4) pass += hex[(proof.passes[i] >> shift) & 15U];
          const bool differs = proof.passes[i] != state.cluster.conformance.passes[i];
          ctx.label(gui::Rect{name_x, cy, row.width - 16.0F, 18.0F}, pass, differs ? ctx.theme().warning : ctx.theme().text_muted);
          cy += 18.0F;
        }
      }
    }
    y += node_row_height;
  }
  ctx.pop_clip();

  if (maximum_scroll > 0.0F && list_bounds.height > 8.0F) {
    const float thumb_height = std::max(
        list_bounds.height * list_bounds.height / content_height, 12.0F);
    const float thumb_range = std::max(list_bounds.height - thumb_height, 0.0F);
    const float thumb_y =
        list_bounds.y + thumb_range * (scroll_ / maximum_scroll);
    ctx.panel(gui::Rect{list_bounds.x + list_bounds.width - 3.0F, thumb_y, 2.0F,
                        thumb_height},
              ctx.theme().text_muted);
  }
  const gui::Rect footer{bounds.x, bounds.y + bounds.height - footer_height,
                         bounds.width, footer_height};
  ctx.panel(footer, ctx.theme().background);
  ctx.panel(gui::Rect{footer.x, footer.y, footer.width, 1.0F},
            ctx.theme().divider);
  const auto actionable = [&](OutputLifecycleAction action) {
    return static_cast<std::size_t>(std::count_if(
        state.nodes.begin(), state.nodes.end(), [action](const auto &node) {
          using Status = OutputMonitorState::Node::Status;
          const bool configured = node.status != Status::Disabled &&
                                  node.status != Status::InvalidConfiguration &&
                                  node.status != Status::DuplicateNodeIndex;
          return configured && (action == OutputLifecycleAction::Stop ||
                                node.content_ready());
        }));
  };
  constexpr float batch_width = 68.0F;
  float batch_right = footer.x + footer.width - 3.0F;
  const auto batch_button = [&](std::string_view suffix, std::string_view label,
                                OutputLifecycleAction action, bool caution) {
    batch_right -= batch_width;
    const gui::Rect button{batch_right, footer.y + 1.0F, batch_width - 3.0F,
                           footer.height - 2.0F};
    if (actionable(action) > 0U) {
      const gui::Interaction interaction = ctx.grab(mix(id_, suffix), button);
      const gui::UiColor base =
          caution ? scale(ctx.theme().warning, 0.36F) : ctx.theme().widget;
      ctx.panel(button, interaction.hovered ? scale(base, 1.24F) : base);
      border(ctx, button, caution ? ctx.theme().warning : ctx.theme().divider);
      ctx.label(button, label,
                caution ? ctx.theme().warning : ctx.theme().text_muted);
      if (!frame.lifecycle.has_value() && interaction.clicked) {
        frame.lifecycle = OutputLifecycleRequest{{}, 0U, {}, action, true};
      }
    } else {
      ctx.panel(button, scale(ctx.theme().widget, 0.65F));
      ctx.label(button, label, scale(ctx.theme().text_muted, 0.65F));
    }
    batch_right -= 2.0F;
  };
  batch_button("output.lifecycle.stop-all", "STOP ALL",
               OutputLifecycleAction::Stop, true);
  batch_button("output.lifecycle.restart-all", "RESTART ALL",
               OutputLifecycleAction::Restart, false);
  batch_button("output.lifecycle.start-all", "START ALL",
               OutputLifecycleAction::Start, false);
  const gui::Rect footer_text{footer.x + 8.0F, footer.y,
                              std::max(batch_right - footer.x - 12.0F, 0.0F),
                              footer.height};
  ctx.push_clip(footer_text);
  ctx.label(footer_text,
            state.nodes.empty()
                ? "Add Render Servers in Hardware Mapping"
                : "Lifecycle targets are configured per Render Server",
            ctx.theme().text_muted);
  ctx.pop_clip();
  ctx.pop_clip();
  return frame;
}

ProgramAssetStatus
program_asset_status(core::AssetSetReadiness local, core::AssetCommit permit,
                     core::u64 control_frame, bool required,
                     const OutputMonitorState::Cluster &cluster,
                     std::string_view loading_detail) {
  if (!required || (local.ready && local.digest == 0))
    return {};
  if (local.ready && local.visible_commit != 0) {
    if (cluster.asset_cohort.members <= 1 || cluster.asset_cohort.visible)
      return {};
    return {true,
            "CONFIRMING OUTPUT UPDATE",
            "Preview updated. Waiting for all output nodes to confirm the same "
            "update."};
  }
  if (!local.ready)
    return {true, "PREPARING PROGRAM", std::string{loading_detail}};
  if (cluster.asset_waiting_node)
    return {true,
            "WAITING FOR OUTPUT NODE " +
                std::to_string(*cluster.asset_waiting_node),
            std::to_string(cluster.asset_cohort.ready_members) + "/" +
                std::to_string(cluster.asset_cohort.members) +
                " renderers ready. Check Output Monitor and Hardware Map."};
  if (permit.digest == local.digest && permit.control_frame != 0) {
    if (permit.control_frame >= control_frame)
      return {true, "UPDATING PROGRAM",
              "Assets ready. Switching in " +
                  std::to_string(permit.control_frame - control_frame) +
                  " control frames."};
    return {true, "PROGRAM ACTIVATION MISSED",
            cluster.asset_cohort.some_visible
                ? "An output missed activation. Program holds the last "
                  "complete image."
                : "Program holds the last complete image while activation is "
                  "retried."};
  }
  return {true, "WAITING TO UPDATE PROGRAM",
          "Assets ready. Waiting for synchronized activation."};
}

} // namespace mediaserver::editor
