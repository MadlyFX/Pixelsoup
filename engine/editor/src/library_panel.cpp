#include <mediaserver/editor/library_panel.hpp>

#include <mediaserver/gui/rect_cut.hpp>
#include <mediaserver/gui/shapes.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace mediaserver::editor {
namespace {

namespace gui = mediaserver::gui;

// Panel chrome, top to bottom: title, toolbar, the column header or folder
// path, the content, and the footer with the view switch and size slider.
constexpr float title_height = 25.0F;
constexpr float toolbar_height = 25.0F;
constexpr float subheader_height = 20.0F;
constexpr float footer_height = 22.0F;
constexpr float list_indent = 14.0F;
constexpr float twirl_width = 14.0F;
constexpr float grid_gap = 8.0F;
constexpr float minimum_column_width = 36.0F;
constexpr float drag_threshold_squared = 16.0F;
constexpr float thumbnail_aspect = 16.0F / 9.0F;
// Space between the Generators, the folders and the assets, so each group
// reads as its own block: wider at the top level than inside a folder.
constexpr float group_gap = 10.0F;
constexpr float nested_group_gap = 6.0F;
constexpr float scrollbar_size = 10.0F;
// A file that is no longer the revision the Library recorded: amber, the
// one warm colour in the palette that is not an error.
constexpr gui::UiColor changed_color{0.93F, 0.66F, 0.24F, 1.0F};

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

void draw_plus(gui::UiContext &ctx, float x, float y, gui::UiColor color) {
  ctx.draw_list().add_line(x - 3.0F, y, x + 3.0F, y, 1.0F, color);
  ctx.draw_list().add_line(x, y - 3.0F, x, y + 3.0F, 1.0F, color);
}

[[nodiscard]] std::string lowercase(std::string_view input) {
  std::string result;
  result.reserve(input.size());
  for (const char character : input) {
    result.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(character))));
  }
  return result;
}

[[nodiscard]] bool matches(std::string_view name, std::string_view query) {
  return query.empty() ||
         lowercase(name).find(lowercase(query)) != std::string::npos;
}

[[nodiscard]] bool visible(const gui::Rect &bounds) noexcept {
  return bounds.width > 0.0F && bounds.height > 0.0F;
}

[[nodiscard]] gui::Interaction clipped_grab(gui::UiContext &ctx, gui::UiId id,
                                            const gui::Rect &bounds,
                                            const gui::Rect &clip) noexcept {
  const gui::Rect hit = gui::intersection(bounds, clip);
  return visible(hit) ? ctx.grab(id, hit) : gui::Interaction{};
}

// `text` shortened with a trailing ".." until it fits `width`.
[[nodiscard]] std::string fit(const gui::UiContext &ctx, std::string_view text,
                              float width) {
  if (width <= 0.0F) {
    return {};
  }
  if (ctx.font_width(text) <= width) {
    return std::string{text};
  }
  std::string shortened{text};
  while (!shortened.empty()) {
    shortened.pop_back();
    if (ctx.font_width(shortened) + ctx.font_width("..") <= width) {
      return shortened + "..";
    }
  }
  return {};
}

struct LibraryItem final {
  std::string_view name;
  std::string_view detail;
  LibraryAddAction action;
};

constexpr std::array<LibraryItem, 8U> library_items{{
    {"Box", "3D primitive", LibraryAddAction::Box},
    {"Plane", "2D surface", LibraryAddAction::Plane},
    {"Sphere", "3D primitive", LibraryAddAction::Sphere},
    // The compositor's Background node as a card on Stage: a colour or a
    // gradient, edited with the same controls.
    {"Background", "Colour or gradient", LibraryAddAction::Background},
    {"Camera", "Tracked stage camera", LibraryAddAction::Camera},
    {"Directional Light", "Global sun", LibraryAddAction::DirectionalLight},
    {"Point Light", "Local emitter", LibraryAddAction::PointLight},
    // A practical: the lamp, window or screen an interior set is actually lit
    // by. Its own entry rather than a point the operator converts afterwards,
    // because the cone and its aim are the whole point of placing one.
    {"Spot Light", "Practical with a cone", LibraryAddAction::SpotLight},
}};

struct ColumnSpec final {
  std::string_view title;
  float width;
  bool shown;
  // Numbers read right-aligned, as in every spreadsheet and bin.
  bool numeric;
};

constexpr std::array<ColumnSpec, library_column_count> column_specs{{
    {"Name", 220.0F, true, false},
    {"Type", 74.0F, true, false},
    {"Resolution", 100.0F, true, true},
    {"Frame Rate", 84.0F, true, true},
    {"Duration", 88.0F, true, true},
    {"Start TC", 88.0F, true, true},
    {"End TC", 88.0F, true, true},
    {"Frames", 64.0F, false, true},
    {"Codec", 70.0F, true, false},
    {"Vertices", 80.0F, true, true},
    {"Triangles", 80.0F, true, true},
    {"Materials", 70.0F, false, true},
    {"Textures", 66.0F, false, true},
    {"Texture Size", 92.0F, true, true},
    {"File Size", 76.0F, true, true},
    {"Proxy", 52.0F, true, false},
    {"Status", 170.0F, true, false},
    {"Path", 280.0F, false, false},
}};

[[nodiscard]] const ColumnSpec &spec(LibraryColumn column) noexcept {
  return column_specs[static_cast<std::size_t>(column)];
}

[[nodiscard]] std::string grouped(core::u64 value) {
  std::string digits = std::to_string(value);
  for (std::size_t at = digits.size(); at > 3U; at -= 3U) {
    digits.insert(at - 3U, 1U, ',');
  }
  return digits;
}

[[nodiscard]] std::string file_size_text(core::u64 bytes) {
  constexpr std::array<std::string_view, 5U> units{
      {"B", "KB", "MB", "GB", "TB"}};
  double value = static_cast<double>(bytes);
  std::size_t unit = 0U;
  while (value >= 1024.0 && unit + 1U < units.size()) {
    value /= 1024.0;
    ++unit;
  }
  std::array<char, 32U> text{};
  const int length =
      unit == 0U
          ? std::snprintf(text.data(), text.size(), "%llu B",
                          static_cast<unsigned long long>(bytes))
          : std::snprintf(text.data(), text.size(),
                          value < 10.0 ? "%.2f %s" : "%.1f %s", value,
                          units[unit].data());
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] std::string rate_text(assets::MediaFrameRate rate) {
  if (rate.denominator == 0U) {
    return {};
  }
  std::array<char, 32U> text{};
  const int length = std::snprintf(
      text.data(), text.size(), "%.3f",
      static_cast<double>(rate.numerator) /
          static_cast<double>(rate.denominator));
  std::string result{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
  while (!result.empty() && result.back() == '0') {
    result.pop_back();
  }
  if (!result.empty() && result.back() == '.') {
    result.pop_back();
  }
  return result;
}

[[nodiscard]] std::string dimensions_text(assets::PixelDimensions size) {
  return std::to_string(size.width) + " x " + std::to_string(size.height);
}

[[nodiscard]] bool is_video(const assets::MediaAsset &media) noexcept {
  return media.frame_rate.has_value() && media.frame_count.has_value() &&
         *media.frame_count > 1U;
}

[[nodiscard]] core::u32 nominal_rate(assets::MediaFrameRate rate) noexcept {
  if (rate.denominator == 0U || rate.numerator == 0U) {
    return 0U;
  }
  return static_cast<core::u32>(
      std::llround(static_cast<double>(rate.numerator) /
                   static_cast<double>(rate.denominator)));
}

[[nodiscard]] bool drop_frame_rate(core::u32 nominal) noexcept {
  return nominal != 0U && nominal % 30U == 0U;
}

[[nodiscard]] bool drop_frame_text(std::string_view timecode) noexcept {
  return timecode.find(';') != std::string_view::npos ||
         timecode.find('.') != std::string_view::npos;
}

// The asset's first frame as a timecode frame number: its own timecode, or
// zero when the file carries none.
[[nodiscard]] std::optional<core::i64>
start_frame(const assets::MediaAsset &media) {
  if (!media.frame_rate.has_value()) {
    return std::nullopt;
  }
  if (media.start_timecode.empty()) {
    return core::i64{0};
  }
  return timecode_frames(media.start_timecode, *media.frame_rate);
}

// A sortable number for a column, or nothing when the column is text or the
// value is unknown (unknowns sort after every known value).
[[nodiscard]] std::optional<double> column_number(const assets::Asset &asset,
                                                  LibraryColumn column) {
  const auto *const media = std::get_if<assets::MediaAsset>(&asset);
  const auto *const model = std::get_if<assets::ModelAsset>(&asset);
  switch (column) {
  case LibraryColumn::Resolution:
    if (media != nullptr && media->dimensions.has_value()) {
      return static_cast<double>(media->dimensions->width) *
             static_cast<double>(media->dimensions->height);
    }
    return std::nullopt;
  case LibraryColumn::FrameRate:
    if (media != nullptr && media->frame_rate.has_value() &&
        media->frame_rate->denominator != 0U) {
      return static_cast<double>(media->frame_rate->numerator) /
             static_cast<double>(media->frame_rate->denominator);
    }
    return std::nullopt;
  case LibraryColumn::Duration:
    if (media != nullptr && media->frame_count.has_value()) {
      if (media->frame_rate.has_value() &&
          media->frame_rate->numerator != 0U) {
        return static_cast<double>(*media->frame_count) *
               static_cast<double>(media->frame_rate->denominator) /
               static_cast<double>(media->frame_rate->numerator);
      }
      return static_cast<double>(*media->frame_count);
    }
    return std::nullopt;
  case LibraryColumn::StartTimecode:
  case LibraryColumn::EndTimecode: {
    if (media == nullptr || !is_video(*media)) {
      return std::nullopt;
    }
    const std::optional<core::i64> start = start_frame(*media);
    if (!start.has_value()) {
      return std::nullopt;
    }
    return static_cast<double>(
        column == LibraryColumn::StartTimecode
            ? *start
            : *start + static_cast<core::i64>(*media->frame_count) - 1);
  }
  case LibraryColumn::Frames:
    if (media != nullptr && media->frame_count.has_value()) {
      return static_cast<double>(*media->frame_count);
    }
    return std::nullopt;
  case LibraryColumn::Vertices:
    if (model != nullptr && model->vertex_count.has_value()) {
      return static_cast<double>(*model->vertex_count);
    }
    return std::nullopt;
  case LibraryColumn::Triangles:
    if (model != nullptr && model->triangle_count.has_value()) {
      return static_cast<double>(*model->triangle_count);
    }
    return std::nullopt;
  case LibraryColumn::Materials:
    if (model != nullptr && model->material_count.has_value()) {
      return static_cast<double>(*model->material_count);
    }
    return std::nullopt;
  case LibraryColumn::Textures:
    if (model != nullptr && model->texture_count.has_value()) {
      return static_cast<double>(*model->texture_count);
    }
    return std::nullopt;
  case LibraryColumn::TextureSize:
    if (model != nullptr && model->largest_texture.has_value()) {
      return static_cast<double>(model->largest_texture->width) *
             static_cast<double>(model->largest_texture->height);
    }
    return std::nullopt;
  case LibraryColumn::FileSize:
    if (const auto &fingerprint = assets::asset_fingerprint(asset)) {
      return static_cast<double>(fingerprint->byte_length);
    }
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

// Every asset's searchable text: its name, source, kind and metadata.
[[nodiscard]] bool asset_matches(const assets::Asset &asset,
                                 std::string_view query) {
  if (query.empty()) {
    return true;
  }
  if (matches(assets::asset_display_name(asset), query) ||
      matches(assets::asset_source(asset), query)) {
    return true;
  }
  for (const LibraryColumn column :
       {LibraryColumn::Type, LibraryColumn::Resolution, LibraryColumn::Codec,
        LibraryColumn::StartTimecode}) {
    if (matches(library_column_text(asset, column), query)) {
      return true;
    }
  }
  return matches(assets::asset_kind(asset) == assets::AssetKind::Media
                     ? std::string_view{"media video image"}
                     : std::string_view{"model 3d gltf obj"},
                 query);
}

// A folder: a tab and a body, the shape every file browser uses.
void draw_folder_icon(gui::UiContext &ctx, const gui::Rect &bounds,
                      gui::UiColor color, bool open) {
  const float size = std::min(bounds.width, bounds.height);
  const float width = size;
  const float height = size * 0.78F;
  const float x = bounds.x + (bounds.width - width) * 0.5F;
  const float y = bounds.y + (bounds.height - height) * 0.5F;
  auto &draw = ctx.draw_list();
  draw.add_rect(x, y, width * 0.42F, height * 0.2F, color);
  draw.add_rect(x, y + height * 0.14F, width, height * 0.86F,
                with_alpha(color, open ? 0.55F : 0.85F));
  ctx.outline(gui::Rect{x, y + height * 0.14F, width, height * 0.86F}, color);
}

// The List and Grid switches: three bars, and four tiles.
void draw_view_icon(gui::UiContext &ctx, const gui::Rect &bounds,
                    LibraryView view, gui::UiColor color) {
  auto &draw = ctx.draw_list();
  const float x = bounds.x + bounds.width * 0.5F - 6.0F;
  const float y = bounds.y + bounds.height * 0.5F - 5.0F;
  if (view == LibraryView::List) {
    for (int line = 0; line < 3; ++line) {
      draw.add_rect(x, y + static_cast<float>(line) * 4.0F, 2.0F, 2.0F, color);
      draw.add_rect(x + 3.0F, y + static_cast<float>(line) * 4.0F, 9.0F, 2.0F,
                    color);
    }
    return;
  }
  for (int row = 0; row < 2; ++row) {
    for (int column = 0; column < 2; ++column) {
      draw.add_rect(x + static_cast<float>(column) * 7.0F,
                    y + static_cast<float>(row) * 6.0F, 5.0F, 4.0F, color);
    }
  }
}

enum class MenuCommand : core::u8 {
  View,
  Rename,
  NewFolder,
  NewFolderInside,
  MoveToTop,
  Remove,
  OpenFolder,
  DeleteFolder,
  AcceptChanged,
  AcceptAllChanged,
  ImportMedia,
  ImportModel,
  Rescan,
  ShowList,
  ShowGrid,
  // Followed by one command per column: show or hide it.
  ToggleColumn,
};

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
    gui::add_disc(ctx.draw_list(), gui::UiPoint{centre_x, centre_y},
                  4.0F, color);
    draw.add_line(centre_x - 7.0F, centre_y, centre_x - 5.0F, centre_y, 1.0F,
                  color);
    draw.add_line(centre_x + 5.0F, centre_y, centre_x + 7.0F, centre_y, 1.0F,
                  color);
    return;
  }
  if (action == LibraryAddAction::Media) {
    draw.add_rect(bounds.x + 2.0F, bounds.y + 3.0F, bounds.width - 4.0F,
                  bounds.height - 6.0F, with_alpha(color, 0.24F));
    ctx.outline(gui::Rect{bounds.x + 2.0F, bounds.y + 3.0F, bounds.width - 4.0F,
                     bounds.height - 6.0F},
                color);
    draw.add_triangle(centre_x - 2.0F, centre_y - 4.0F, centre_x - 2.0F,
                      centre_y + 4.0F, centre_x + 4.0F, centre_y, color);
    return;
  }
  if (action == LibraryAddAction::Model) {
    draw.add_line(bounds.x + 2.0F, bounds.bottom() - 3.0F, centre_x,
                  bounds.y + 2.0F, 1.0F, color);
    draw.add_line(centre_x, bounds.y + 2.0F, bounds.right() - 2.0F,
                  bounds.bottom() - 3.0F, 1.0F, color);
    draw.add_line(bounds.x + 2.0F, bounds.bottom() - 3.0F,
                  bounds.right() - 2.0F,
                  bounds.bottom() - 3.0F, 1.0F, color);
    return;
  }
  const gui::Rect cube{bounds.x + 3.0F, bounds.y + 3.0F, bounds.width - 6.0F,
                       bounds.height - 6.0F};
  draw.add_rect(cube.x, cube.y, cube.width, cube.height,
                with_alpha(color, 0.18F));
  ctx.outline(cube, color);
  if (action == LibraryAddAction::Sphere) {
    gui::add_disc(ctx.draw_list(), gui::UiPoint{centre_x, centre_y},
                  cube.width * 0.46F, color);
    gui::add_disc(ctx.draw_list(), gui::UiPoint{centre_x, centre_y},
                  cube.width * 0.33F, ctx.theme().widget);
  } else if (action == LibraryAddAction::Plane) {
    draw.add_line(cube.x, cube.bottom(), cube.right(), cube.y,
                  1.0F, color);
  } else if (action == LibraryAddAction::Background) {
    // A ramp: bands growing brighter down the card.
    for (int band = 0; band < 3; ++band) {
      const float height = cube.height / 3.0F;
      draw.add_rect(cube.x + 1.0F, cube.y + height * static_cast<float>(band),
                    cube.width - 2.0F, height,
                    with_alpha(color, 0.2F + 0.25F * static_cast<float>(band)));
    }
  }
}

void draw_cpu_decode_warning(gui::UiContext &ctx, const gui::Rect &bounds) {
  auto &draw = ctx.draw_list();
  const gui::UiColor red = ctx.theme().warning;
  const gui::UiColor cutout = scale(ctx.theme().row, 0.72F);
  const float centre_x = bounds.x + bounds.width * 0.5F;
  draw.add_triangle(centre_x, bounds.y, bounds.right(),
                    bounds.bottom(), bounds.x,
                    bounds.bottom(), red);
  draw.add_line(centre_x, bounds.y + 4.0F, centre_x,
                bounds.bottom() - 4.5F, 1.5F, cutout);
  draw.add_rect(centre_x - 0.8F, bounds.bottom() - 3.0F, 1.6F, 1.6F,
                cutout);
}


} // namespace

std::string_view library_column_title(LibraryColumn column) {
  return column == LibraryColumn::Count ? std::string_view{}
                                        : spec(column).title;
}

std::optional<core::i64> timecode_frames(std::string_view timecode,
                                         assets::MediaFrameRate rate) {
  const core::u32 fps = nominal_rate(rate);
  if (fps == 0U) {
    return std::nullopt;
  }
  std::array<core::i64, 4U> fields{};
  std::size_t field = 0U;
  bool digits = false;
  for (const char character : timecode) {
    if (character >= '0' && character <= '9') {
      if (fields[field] > 1'000'000) {
        return std::nullopt;
      }
      fields[field] = fields[field] * 10 + (character - '0');
      digits = true;
    } else if (character == ':' || character == ';' || character == '.') {
      if (!digits || ++field >= fields.size()) {
        return std::nullopt;
      }
      digits = false;
    } else {
      return std::nullopt;
    }
  }
  if (field != 3U || !digits) {
    return std::nullopt;
  }
  const auto [hours, minutes, seconds, frames] = fields;
  if (minutes >= 60 || seconds >= 60 ||
      frames >= static_cast<core::i64>(fps)) {
    return std::nullopt;
  }
  const core::i64 nominal = static_cast<core::i64>(fps);
  core::i64 total = ((hours * 60 + minutes) * 60 + seconds) * nominal + frames;
  if (drop_frame_text(timecode) && drop_frame_rate(fps)) {
    const core::i64 dropped = nominal / 15;
    const core::i64 total_minutes = hours * 60 + minutes;
    total -= dropped * (total_minutes - total_minutes / 10);
  }
  return total;
}

std::string timecode_text(core::i64 frame, assets::MediaFrameRate rate,
                          bool drop_frame) {
  const core::u32 fps = nominal_rate(rate);
  if (fps == 0U) {
    return {};
  }
  const core::i64 nominal = static_cast<core::i64>(fps);
  const bool drop = drop_frame && drop_frame_rate(fps);
  // Timecode wraps at 24 hours; the frames in a real day at this rate.
  const core::i64 dropped = drop ? nominal / 15 : 0;
  const core::i64 per_ten_minutes = nominal * 600 - dropped * 9;
  const core::i64 day = per_ten_minutes * 6 * 24;
  frame %= day;
  if (frame < 0) {
    frame += day;
  }
  if (drop) {
    const core::i64 per_minute = nominal * 60 - dropped;
    const core::i64 tens = frame / per_ten_minutes;
    const core::i64 rest = frame % per_ten_minutes;
    frame += dropped * 9 * tens +
             (rest > dropped ? dropped * ((rest - dropped) / per_minute) : 0);
  }
  const core::i64 frames = frame % nominal;
  const core::i64 seconds = (frame / nominal) % 60;
  const core::i64 minutes = (frame / (nominal * 60)) % 60;
  const core::i64 hours = frame / (nominal * 3600);
  std::array<char, 32U> text{};
  const int length = std::snprintf(
      text.data(), text.size(), "%02lld:%02lld:%02lld%c%02lld",
      static_cast<long long>(hours), static_cast<long long>(minutes),
      static_cast<long long>(seconds), drop ? ';' : ':',
      static_cast<long long>(frames));
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

std::string library_column_text(const assets::Asset &asset,
                                LibraryColumn column) {
  const auto *const media = std::get_if<assets::MediaAsset>(&asset);
  const auto *const model = std::get_if<assets::ModelAsset>(&asset);
  const auto number = [](const auto &value) -> std::string {
    return value.has_value() ? grouped(static_cast<core::u64>(*value))
                             : std::string{};
  };
  switch (column) {
  case LibraryColumn::Name:
    return std::string{assets::asset_display_name(asset)};
  case LibraryColumn::Type:
    if (media != nullptr) {
      if (!media->frame_count.has_value()) {
        return "Media";
      }
      if (*media->frame_count <= 1U) {
        return "Still";
      }
      return media->frame_rate.has_value() ? "Video" : "Sequence";
    }
    return model->projection_mesh ? "Mesh" : "3D Model";
  case LibraryColumn::Resolution:
    return media != nullptr && media->dimensions.has_value()
               ? dimensions_text(*media->dimensions)
               : std::string{};
  case LibraryColumn::FrameRate:
    return media != nullptr && media->frame_rate.has_value() && is_video(*media)
               ? rate_text(*media->frame_rate)
               : std::string{};
  case LibraryColumn::Duration:
    if (media == nullptr || !media->frame_count.has_value() ||
        *media->frame_count <= 1U) {
      return {};
    }
    if (media->frame_rate.has_value()) {
      return timecode_text(static_cast<core::i64>(*media->frame_count),
                           *media->frame_rate,
                           drop_frame_text(media->start_timecode));
    }
    return grouped(*media->frame_count) + " fr";
  case LibraryColumn::StartTimecode:
  case LibraryColumn::EndTimecode: {
    if (media == nullptr || !is_video(*media)) {
      return {};
    }
    const std::optional<core::i64> start = start_frame(*media);
    if (!start.has_value()) {
      // A timecode this rate cannot hold: show what the file says.
      return column == LibraryColumn::StartTimecode ? media->start_timecode
                                                    : std::string{};
    }
    const core::i64 frame =
        column == LibraryColumn::StartTimecode
            ? *start
            : *start + static_cast<core::i64>(*media->frame_count) - 1;
    return timecode_text(frame, *media->frame_rate,
                         drop_frame_text(media->start_timecode));
  }
  case LibraryColumn::Frames:
    return media != nullptr ? number(media->frame_count) : std::string{};
  case LibraryColumn::Codec: {
    if (media == nullptr) {
      return {};
    }
    std::string codec = media->codec;
    for (char &character : codec) {
      character = static_cast<char>(
          std::toupper(static_cast<unsigned char>(character)));
    }
    return codec;
  }
  case LibraryColumn::Vertices:
    return model != nullptr ? number(model->vertex_count) : std::string{};
  case LibraryColumn::Triangles:
    return model != nullptr ? number(model->triangle_count) : std::string{};
  case LibraryColumn::Materials:
    return model != nullptr ? number(model->material_count) : std::string{};
  case LibraryColumn::Textures:
    return model != nullptr ? number(model->texture_count) : std::string{};
  case LibraryColumn::TextureSize:
    return model != nullptr && model->largest_texture.has_value()
               ? dimensions_text(*model->largest_texture)
               : std::string{};
  case LibraryColumn::FileSize: {
    const auto &fingerprint = assets::asset_fingerprint(asset);
    return fingerprint.has_value() ? file_size_text(fingerprint->byte_length)
                                   : std::string{};
  }
  case LibraryColumn::Proxy:
    return media != nullptr
               ? std::string{library_proxy_label(media->proxy_playback_policy)}
               : std::string{};
  case LibraryColumn::Status:
    return {};
  case LibraryColumn::Path:
    return std::string{assets::asset_source(asset)};
  case LibraryColumn::Count:
    return {};
  }
  return {};
}

std::optional<LibraryImportAction> library_import_kind(std::string_view path) {
  const std::size_t dot = path.find_last_of('.');
  const std::size_t slash = path.find_last_of("/\\");
  if (dot == std::string_view::npos ||
      (slash != std::string_view::npos && dot < slash)) {
    return std::nullopt;
  }
  const std::string extension = lowercase(path.substr(dot + 1U));
  constexpr std::array<std::string_view, 17U> media{
      {"mp4", "mov", "mkv", "webm", "avi", "mxf", "m4v", "mpg", "mpeg", "wmv",
       "png", "jpg", "jpeg", "bmp", "tga", "exr", "hdr"}};
  constexpr std::array<std::string_view, 4U> models{
      {"gltf", "glb", "obj", "msmodel"}};
  if (std::find(media.begin(), media.end(), extension) != media.end()) {
    return LibraryImportAction::Media;
  }
  if (std::find(models.begin(), models.end(), extension) != models.end()) {
    return LibraryImportAction::Model;
  }
  return std::nullopt;
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

LibraryPanel::LibraryPanel(gui::UiId id) noexcept
    : id_(id), rename_(id ^ 0x4C49425241525952ULL) {
  for (std::size_t column = 0U; column < library_column_count; ++column) {
    column_width_[column] = column_specs[column].width;
    column_visible_[column] = column_specs[column].shown;
  }
}

void LibraryPanel::set_icon_size(float size) noexcept {
  const float clamped = std::clamp(size, 0.0F, 1.0F);
  if (view_ == LibraryView::List) {
    list_zoom_ = clamped;
  } else {
    grid_zoom_ = clamped;
  }
}

void LibraryPanel::open_folder(doc::NodeId folder) noexcept {
  folder_ = folder;
  generators_open_ = false;
  scroll_ = 0.0F;
}

void LibraryPanel::set_column_visible(LibraryColumn column,
                                      bool visible) noexcept {
  if (column == LibraryColumn::Name || column == LibraryColumn::Count) {
    return;
  }
  column_visible_[static_cast<std::size_t>(column)] = visible;
}

void LibraryPanel::reset_project_state() noexcept {
  folder_ = {};
  generators_open_ = false;
  expanded_.clear();
  selection_.clear();
  anchor_ = {};
  drag_items_.clear();
  drag_primary_ = {};
  drag_moved_ = false;
  generator_drag_.reset();
  generator_drag_moved_ = false;
  pending_folder_name_.reset();
  known_folders_.clear();
  menu_open_ = false;
  scroll_ = 0.0F;
  rename_.reset();
}

LibraryFrame LibraryPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                                const assets::AssetRegistry &assets) {
  return draw_impl(ctx, bounds, assets, {}, nullptr, {});
}

LibraryFrame
LibraryPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                   const assets::AssetRegistry &assets,
                   std::span<const LibraryThumbnailObservation> thumbnails,
                   LibraryThumbnailWindow window) {
  return draw_impl(ctx, bounds, assets, thumbnails, &window, {});
}

LibraryFrame LibraryPanel::draw(
    gui::UiContext &ctx, const gui::Rect &bounds,
    const assets::AssetRegistry &assets,
    std::span<const LibraryThumbnailObservation> thumbnails,
    LibraryThumbnailWindow window,
    std::span<const LibraryProxyRuntimeObservation> proxy_runtime) {
  return draw_impl(ctx, bounds, assets, thumbnails, &window, proxy_runtime);
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

void LibraryPanel::draw_drag_ghost(gui::UiContext &ctx) {
  if (!ghost_live_) {
    return;
  }
  ghost_live_ = false;
  const gui::Theme &theme = ctx.theme();
  // Behind the pointer rather than under it, so the drop target the
  // pointer is over stays readable.
  constexpr float image_width = 112.0F;
  constexpr float image_height = image_width / thumbnail_aspect;
  const gui::Rect image{ctx.pointer_x() + 14.0F, ctx.pointer_y() + 14.0F,
                        image_width, image_height};
  if (ghost_count_ > 1U) {
    // A stack: the cards underneath peek out behind the first.
    for (std::size_t layer = std::min<std::size_t>(ghost_count_ - 1U, 2U);
         layer > 0U; --layer) {
      const float offset = static_cast<float>(layer) * 4.0F;
      const gui::Rect card{image.x + offset, image.y - offset, image.width,
                           image.height};
      ctx.panel(card, with_alpha(theme.header, 0.85F));
      ctx.outline(card, with_alpha(theme.border, 0.9F));
    }
  }
  ctx.panel(image, with_alpha(scale(theme.track, 0.84F), 0.92F));
  if (ghost_texture_ != nullptr) {
    ctx.image(image, *ghost_texture_, gui::UiColor{1.0F, 1.0F, 1.0F, 0.82F});
  } else {
    const float icon = 26.0F;
    draw_library_icon(
        ctx, ghost_generator_.has_value()
                 ? *ghost_generator_
                 : (ghost_model_ ? LibraryAddAction::Model
                                 : LibraryAddAction::Media),
        gui::Rect{image.x + (image.width - icon) * 0.5F,
                  image.y + (image.height - icon) * 0.5F, icon, icon});
  }
  ctx.outline(image, theme.accent);
  const gui::Rect caption{image.x, image.bottom(), image.width, 18.0F};
  ctx.panel(caption, with_alpha(theme.background, 0.92F));
  ctx.label(gui::Rect{caption.x + 5.0F, caption.y, caption.width - 10.0F,
                      caption.height},
            fit(ctx, ghost_label_, caption.width - 10.0F), theme.text);
  if (ghost_count_ > 1U) {
    const std::string count = std::to_string(ghost_count_);
    const float width = std::max(ctx.font_width(count) + 10.0F, 18.0F);
    const gui::Rect badge{image.right() - width + 6.0F, image.y - 8.0F,
                          width, 16.0F};
    ctx.panel(badge, theme.accent);
    ctx.label(gui::Rect{badge.x + 5.0F, badge.y, badge.width - 5.0F,
                        badge.height},
              count, theme.background);
  }
}

LibraryFrame LibraryPanel::draw_impl(
    gui::UiContext &ctx, const gui::Rect &bounds,
    const assets::AssetRegistry &registry,
    std::span<const LibraryThumbnailObservation> thumbnails,
    const LibraryThumbnailWindow *thumbnail_window,
    std::span<const LibraryProxyRuntimeObservation> proxy_runtime) {
  LibraryFrame frame;
  ghost_live_ = false;
  ghost_texture_ = nullptr;
  ghost_generator_.reset();
  if (thumbnail_window != nullptr) {
    frame.thumbnail_retain.emplace();
  }
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return frame;
  }
  const gui::Theme &theme = ctx.theme();
  const float pointer_x = ctx.pointer_x();
  const float pointer_y = ctx.pointer_y();
  const bool pointer_inside = bounds.contains(pointer_x, pointer_y);
  if (ctx.pointer_pressed() || ctx.secondary_pointer_pressed()) {
    focused_ = pointer_inside;
  }
  const bool thumbnails_enabled = thumbnail_window != nullptr;
  const bool searching = !search_.empty();
  const bool list = view_ == LibraryView::List;

  // ------------------------------------------------------------ the model
  // The selection keeps only what still exists.
  std::erase_if(selection_, [&registry](doc::NodeId id) {
    return registry.asset(id) == nullptr && registry.folder(id) == nullptr;
  });
  if (folder_ != doc::NodeId{} && registry.folder(folder_) == nullptr) {
    folder_ = {};
  }
  const auto is_selected = [this](doc::NodeId id) {
    return std::find(selection_.begin(), selection_.end(), id) !=
           selection_.end();
  };
  // A folder this panel created has arrived: select it and name it.
  if (pending_folder_name_.has_value()) {
    for (const assets::AssetFolder &folder : registry.folders) {
      if (known_folders_.contains(folder.id) ||
          folder.display_name != *pending_folder_name_ ||
          folder.parent != pending_folder_parent_) {
        continue;
      }
      selection_.assign(1U, folder.id);
      anchor_ = folder.id;
      if (folder.parent != doc::NodeId{}) {
        expanded_.insert(folder.parent);
      }
      rename_.begin(ctx, folder.id.value(), folder.display_name);
      pending_folder_name_.reset();
      known_folders_.clear();
      break;
    }
  }

  // Each folder's contents, in the order the operator sorted them.
  const auto asset_less = [this](const assets::Asset *left,
                                 const assets::Asset *right) {
    const auto name_order = [&] {
      const std::string a = lowercase(assets::asset_display_name(*left));
      const std::string b = lowercase(assets::asset_display_name(*right));
      if (a != b) {
        return a < b ? -1 : 1;
      }
      return assets::asset_id(*left) < assets::asset_id(*right) ? -1 : 1;
    };
    int order = 0;
    if (sort_column_ == LibraryColumn::Name) {
      order = name_order();
    } else {
      const std::optional<double> a = column_number(*left, sort_column_);
      const std::optional<double> b = column_number(*right, sort_column_);
      if (a.has_value() || b.has_value()) {
        if (!a.has_value() || !b.has_value()) {
          // Unknown values stay at the end whichever way the column runs.
          return a.has_value();
        }
        if (*a != *b) {
          order = *a < *b ? -1 : 1;
        }
      } else {
        const std::string ta = lowercase(library_column_text(*left, sort_column_));
        const std::string tb =
            lowercase(library_column_text(*right, sort_column_));
        if (ta.empty() != tb.empty()) {
          return !ta.empty();
        }
        if (ta != tb) {
          order = ta < tb ? -1 : 1;
        }
      }
      if (order == 0) {
        return name_order() < 0;
      }
    }
    return sort_descending_ ? order > 0 : order < 0;
  };
  const auto folder_less = [this](const assets::AssetFolder *left,
                                  const assets::AssetFolder *right) {
    const std::string a = lowercase(left->display_name);
    const std::string b = lowercase(right->display_name);
    if (a != b) {
      const bool descending =
          sort_column_ == LibraryColumn::Name && sort_descending_;
      return descending ? a > b : a < b;
    }
    return left->id < right->id;
  };
  std::map<doc::NodeId, std::vector<const assets::AssetFolder *>> subfolders;
  std::map<doc::NodeId, std::vector<const assets::Asset *>> contents;
  for (const assets::AssetFolder &folder : registry.folders) {
    subfolders[folder.parent].push_back(std::addressof(folder));
  }
  for (const assets::Asset &asset : registry.assets) {
    contents[registry.folder_of(asset)].push_back(std::addressof(asset));
  }
  for (auto &[parent, held] : subfolders) {
    static_cast<void>(parent);
    std::sort(held.begin(), held.end(), folder_less);
  }
  for (auto &[parent, held] : contents) {
    static_cast<void>(parent);
    std::sort(held.begin(), held.end(), asset_less);
  }
  const auto child_count = [&](doc::NodeId folder) {
    const auto folders = subfolders.find(folder);
    const auto held = contents.find(folder);
    return (folders == subfolders.end() ? 0U : folders->second.size()) +
           (held == contents.end() ? 0U : held->second.size());
  };

  // What is shown, in order: generators, folders, assets.
  struct Entry final {
    enum class Kind : core::u8 { Generators, Generator, Folder, Asset };
    Kind kind{Kind::Asset};
    std::size_t depth{};
    const LibraryItem *item{};
    const assets::AssetFolder *folder{};
    const assets::Asset *asset{};
    doc::NodeId id{};
    // Space above this entry where one group gives way to the next.
    float gap{};
  };
  std::vector<Entry> entries;
  entries.reserve(registry.assets.size() + registry.folders.size() + 8U);
  std::vector<const LibraryItem *> generators;
  for (const LibraryItem &item : library_items) {
    if (!searching || matches(item.name, search_) ||
        matches(item.detail, search_) || matches("generators", search_)) {
      generators.push_back(std::addressof(item));
    }
  }
  const auto push_generators = [&](bool header, bool open,
                                   std::size_t depth) {
    if (generators.empty()) {
      return;
    }
    if (header) {
      entries.push_back(Entry{Entry::Kind::Generators, 0U});
    }
    if (open) {
      for (const LibraryItem *const item : generators) {
        entries.push_back(Entry{Entry::Kind::Generator, depth, item});
      }
    }
  };
  if (searching) {
    push_generators(list, list ? generators_expanded_ : true, list ? 1U : 0U);
    for (const assets::AssetFolder &folder : registry.folders) {
      if (matches(folder.display_name, search_)) {
        entries.push_back(Entry{Entry::Kind::Folder, 0U, nullptr,
                                std::addressof(folder), nullptr, folder.id});
      }
    }
    std::vector<const assets::Asset *> found;
    for (const assets::Asset &asset : registry.assets) {
      if (asset_matches(asset, search_)) {
        found.push_back(std::addressof(asset));
      }
    }
    std::sort(found.begin(), found.end(), asset_less);
    for (const assets::Asset *const asset : found) {
      entries.push_back(Entry{Entry::Kind::Asset, 0U, nullptr, nullptr, asset,
                              assets::asset_id(*asset)});
    }
  } else if (list) {
    push_generators(true, generators_expanded_, 1U);
    const auto emit = [&](const auto &self, doc::NodeId parent,
                          std::size_t depth) -> void {
      if (const auto folders = subfolders.find(parent);
          folders != subfolders.end()) {
        for (const assets::AssetFolder *const folder : folders->second) {
          entries.push_back(Entry{Entry::Kind::Folder, depth, nullptr, folder,
                                  nullptr, folder->id});
          if (expanded_.contains(folder->id) && depth < 32U) {
            self(self, folder->id, depth + 1U);
          }
        }
      }
      if (const auto held = contents.find(parent); held != contents.end()) {
        for (const assets::Asset *const asset : held->second) {
          entries.push_back(Entry{Entry::Kind::Asset, depth, nullptr, nullptr,
                                  asset, assets::asset_id(*asset)});
        }
      }
    };
    emit(emit, doc::NodeId{}, 0U);
  } else if (generators_open_) {
    push_generators(false, true, 0U);
  } else {
    if (folder_ == doc::NodeId{}) {
      push_generators(true, false, 0U);
    }
    if (const auto folders = subfolders.find(folder_);
        folders != subfolders.end()) {
      for (const assets::AssetFolder *const folder : folders->second) {
        entries.push_back(Entry{Entry::Kind::Folder, 0U, nullptr, folder,
                                nullptr, folder->id});
      }
    }
    if (const auto held = contents.find(folder_); held != contents.end()) {
      for (const assets::Asset *const asset : held->second) {
        entries.push_back(Entry{Entry::Kind::Asset, 0U, nullptr, nullptr,
                                asset, assets::asset_id(*asset)});
      }
    }
  }
  // Generators, folders and assets are three groups; a gap opens where one
  // gives way to another, and where an open folder's contents end.
  const auto group_of = [list](const Entry &entry) {
    switch (entry.kind) {
    case Entry::Kind::Generators:
      // In the Grid the Generators are a folder tile among the folders.
      return list ? 0 : 1;
    case Entry::Kind::Generator:
      return 0;
    case Entry::Kind::Folder:
      return 1;
    case Entry::Kind::Asset:
      return 2;
    }
    return 2;
  };
  for (std::size_t index = 1U; index < entries.size(); ++index) {
    const Entry &before = entries[index - 1U];
    Entry &entry = entries[index];
    const bool new_group = group_of(entry) != group_of(before) &&
                           entry.depth <= before.depth;
    if (new_group || entry.depth < before.depth) {
      entry.gap = entry.depth == 0U ? group_gap : nested_group_gap;
    }
  }

  // Shift ranges run over what the operator sees.
  std::vector<doc::NodeId> visible_order;
  for (const Entry &entry : entries) {
    if (entry.id != doc::NodeId{}) {
      visible_order.push_back(entry.id);
    }
  }

  // Changed sources still in this Library, in the order they are listed.
  std::vector<doc::NodeId> changed_shown;
  for (const doc::NodeId id : changed_) {
    if (registry.asset(id) != nullptr) {
      changed_shown.push_back(id);
    }
  }
  const auto is_changed = [&changed_shown](doc::NodeId id) {
    return std::find(changed_shown.begin(), changed_shown.end(), id) !=
           changed_shown.end();
  };

  // ------------------------------------------------------------ chrome
  ctx.push_clip(bounds);
  ctx.panel(bounds);
  gui::Rect regions = bounds;
  {
    const gui::Rect header = gui::cut_top(regions, title_height);
    ctx.panel(header, theme.background);
    gui::Rect rule = header;
    ctx.panel(gui::cut_bottom(rule, 1.0F), theme.divider);
    // Files that changed on disk: the one thing in the Library that needs
    // an answer, so it sits where it is seen.
    if (!changed_shown.empty()) {
      const std::string label =
          std::to_string(changed_shown.size()) +
          (changed_shown.size() == 1U ? " CHANGED SOURCE - ACCEPT"
                                      : " CHANGED SOURCES - ACCEPT ALL");
      gui::Rect chips = gui::inset(header, 6.0F, 3.0F);
      const gui::Rect chip =
          gui::cut_left(chips, ctx.font_width(label) + 16.0F);
      const gui::Interaction interaction =
          ctx.grab(mix(id_, "library.accept_all"), chip);
      ctx.panel(chip, interaction.hovered ? changed_color
                                          : scale(changed_color, 0.8F));
      ctx.label(gui::inset(chip, gui::Padding{8.0F, 0.0F, 0.0F, 0.0F}), label,
                theme.background);
      ctx.tooltip(chip, "These files changed on disk since they were imported. "
                        "Accept re-reads them and records the new revision; "
                        "one Undo puts the old one back.");
      if (interaction.clicked) {
        frame.accept_sources = changed_shown;
      }
    }
    std::string count = std::to_string(registry.assets.size()) + " ASSETS";
    if (!registry.folders.empty()) {
      count += " | " + std::to_string(registry.folders.size()) + " FOLDERS";
    }
    gui::Rect trailing = gui::inset(header, 8.0F, 0.0F);
    ctx.label(gui::cut_right(trailing, ctx.font_width(count)), count,
              theme.text_muted);
  }

  // The toolbar: search, a new folder, import and rescan.
  const gui::Rect toolbar = gui::cut_top(regions, toolbar_height);
  constexpr float folder_button_width = 26.0F;
  // Import is a word and its mark; Rescan a mark alone, named on hover.
  constexpr float import_width = 92.0F;
  constexpr float rescan_width = 34.0F;
  const bool room_for_import = bounds.width > 220.0F;
  const bool room_for_rescan = bounds.width > 300.0F;
  // The buttons after the search field, each a slot with its gap on the
  // right; cut from the right edge, last first.
  gui::Rect search_bounds = toolbar;
  const gui::Rect rescan_slot =
      room_for_rescan ? gui::cut_right(search_bounds, rescan_width)
                      : gui::Rect{};
  const gui::Rect import_slot =
      room_for_import ? gui::cut_right(search_bounds, import_width)
                      : gui::Rect{};
  const gui::Rect folder_slot =
      gui::cut_right(search_bounds, folder_button_width);
  const auto slot_button = [](const gui::Rect &slot, float gap) {
    return gui::inset(slot, gui::Padding{0.0F, 2.0F, gap, 2.0F});
  };
  {
    const gui::Rect field = gui::inset(search_bounds, 6.0F, 3.0F);
    const gui::UiId search_id = mix(id_, "library.search");
    if (search_box(ctx, search_id, field, search_,
                   "Search Assets and Generators...")) {
      scroll_ = 0.0F;
    }
  }
  const gui::UiId menu_id = mix(id_, "library.menu");
  bool open_import_menu = false;
  bool create_folder = false;
  {
    const gui::Rect button = slot_button(folder_slot, 2.0F);
    const gui::Interaction interaction =
        ctx.grab(mix(id_, "library.new_folder"), button);
    if (interaction.hovered) {
      ctx.rounded(button, theme.widget_hover, theme.radius);
    }
    draw_folder_icon(ctx, button.inflated(-5.0F, -5.0F),
                     interaction.hovered ? theme.text : theme.text_muted,
                     false);
    draw_plus(ctx, button.right() - 6.0F, button.y + 6.0F,
              theme.accent);
    ctx.tooltip(button, "New Folder");
    create_folder = interaction.clicked;
  }
  if (room_for_import) {
    // The panel's one coloured button: what it most wants pressed.
    if (pill_button(ctx, mix(id_, "library.import"),
                    slot_button(import_slot, 4.0F), gui::Icon::Import,
                    "Import", "Import media, models or a folder (Ctrl+I)",
                    ButtonTone::Primary)) {
      open_import_menu = true;
    }
  }
  if (room_for_rescan) {
    if (pill_button(ctx, mix(id_, "library.rescan"),
                    slot_button(rescan_slot, 4.0F), gui::Icon::Rescan, {},
                    "Rescan: look for changed and missing source files")) {
      frame.rescan_assets = true;
    }
  }

  const gui::Rect subheader = gui::cut_top(regions, subheader_height);
  const gui::Rect footer = gui::cut_bottom(regions, footer_height);
  const gui::Rect area = regions;

  // Geometry of one view or the other.
  const float list_thumb_height = std::round(16.0F + list_zoom_ * 64.0F);
  const float list_row_height = std::max(20.0F, list_thumb_height + 4.0F);
  const float tile_width = std::round(80.0F + grid_zoom_ * 176.0F);
  const float tile_image_height = std::round(tile_width / thumbnail_aspect);
  const float tile_height = tile_image_height + 36.0F;
  // Grid tiles need the width before a vertical scrollbar is known; one is
  // always reserved so the column count never flickers with it.
  const std::size_t grid_columns = static_cast<std::size_t>(std::max(
      1.0F, std::floor((area.width - scrollbar_size - grid_gap) /
                       (tile_width + grid_gap))));

  // The visible columns and their left edges, before horizontal scroll.
  std::vector<LibraryColumn> columns;
  float columns_width = 0.0F;
  for (std::size_t column = 0U; column < library_column_count; ++column) {
    if (column_visible_[column] || column == 0U) {
      columns.push_back(static_cast<LibraryColumn>(column));
      columns_width += column_width_[column];
    }
  }

  // Grid tiles, laid out once: each group starts a row of its own.
  std::vector<gui::UiPoint> tile_at;
  float content_height = 0.0F;
  if (list) {
    for (const Entry &entry : entries) {
      content_height += entry.gap + (entry.kind == Entry::Kind::Asset
                                         ? list_row_height
                                         : 22.0F);
    }
  } else {
    tile_at.reserve(entries.size());
    float x_index = 0.0F;
    float y = grid_gap;
    for (const Entry &entry : entries) {
      if (entry.gap > 0.0F && x_index > 0.0F) {
        x_index = 0.0F;
        y += tile_height + grid_gap + entry.gap;
      } else if (x_index >= static_cast<float>(grid_columns)) {
        x_index = 0.0F;
        y += tile_height + grid_gap;
      }
      tile_at.push_back(gui::UiPoint{
          grid_gap + x_index * (tile_width + grid_gap), y});
      x_index += 1.0F;
    }
    content_height = entries.empty() ? 0.0F : y + tile_height + grid_gap;
  }
  if (entries.empty()) {
    content_height = 40.0F;
  }
  // Scrollbars take their own strip beside the rows, never on top of them,
  // so a press there always reaches the bar.
  const bool needs_x =
      list && columns_width + 6.0F > area.width - scrollbar_size;
  const float view_height =
      std::max(area.height - (needs_x ? scrollbar_size : 0.0F), 0.0F);
  const bool needs_y = !list || content_height > view_height;
  // The rows, less the scrollbars' own strips.
  gui::Rect content = area;
  if (needs_x) {
    static_cast<void>(gui::cut_bottom(content, scrollbar_size));
  }
  if (needs_y) {
    static_cast<void>(gui::cut_right(content, scrollbar_size));
  }
  const float maximum_scroll_x =
      list ? std::max(columns_width - content.width + 6.0F, 0.0F) : 0.0F;
  const float maximum_scroll = std::max(content_height - content.height, 0.0F);
  if (content.contains(pointer_x, pointer_y) && ctx.wheel() != 0.0F) {
    if (ctx.ctrl_held()) {
      set_icon_size(icon_size() + ctx.wheel() * 0.05F);
    } else if (ctx.shift_held() && list) {
      scroll_x_ -= ctx.wheel() * 40.0F;
    } else {
      scroll_ -= ctx.wheel() * 40.0F;
    }
  }
  // A scrollbar: drag the thumb, or press the track to bring the thumb
  // there and keep dragging.
  const auto scrollbar = [&](std::string_view name, bool vertical,
                             float &offset, float maximum) {
    const gui::Rect track =
        vertical ? gui::Rect{content.right(), content.y,
                             scrollbar_size, content.height}
                 : gui::Rect{content.x, content.bottom(),
                             content.width, scrollbar_size};
    const float length = vertical ? track.height : track.width;
    const float total = length + maximum;
    const float thumb_length =
        std::min(std::max(length * length / std::max(total, 1.0F), 24.0F),
                 length);
    const float range = std::max(length - thumb_length, 0.0F);
    const float start = vertical ? track.y : track.x;
    const float along = vertical ? pointer_y : pointer_x;
    const gui::Interaction interaction = ctx.grab(mix(id_, name), track);
    float thumb_at =
        start + (maximum > 0.0F ? range * offset / maximum : 0.0F);
    if (interaction.pressed) {
      const float pressed_at =
          vertical ? ctx.pointer_press_y() : ctx.pointer_press_x();
      scrollbar_grip_ =
          pressed_at >= thumb_at && pressed_at <= thumb_at + thumb_length
              ? pressed_at - thumb_at
              : thumb_length * 0.5F;
    }
    if (interaction.held && range > 0.0F) {
      offset = std::clamp((along - scrollbar_grip_ - start) / range, 0.0F,
                          1.0F) *
               maximum;
      thumb_at = start + range * offset / maximum;
    }
    ctx.panel(track, scale(theme.divider, 0.75F));
    const gui::Rect thumb =
        vertical ? gui::Rect{track.x + 2.0F, thumb_at, track.width - 4.0F,
                             thumb_length}
                 : gui::Rect{thumb_at, track.y + 2.0F, thumb_length,
                             track.height - 4.0F};
    ctx.panel(thumb, interaction.held      ? theme.text
                     : interaction.hovered ? scale(theme.text_muted, 1.35F)
                                           : theme.text_muted);
  };
  scroll_ = std::clamp(scroll_, 0.0F, maximum_scroll);
  scroll_x_ = std::clamp(scroll_x_, 0.0F, maximum_scroll_x);
  // The Grid keeps the strip either way, so its columns stay put.
  if (needs_y && maximum_scroll > 0.0F) {
    scrollbar("library.scroll_y", true, scroll_, maximum_scroll);
  }
  if (needs_x) {
    scrollbar("library.scroll_x", false, scroll_x_, maximum_scroll_x);
  }

  // Thumbnails: every media asset in view (or near it) that has one.
  struct ThumbnailCandidate final {
    assets::DerivativeKey key;
    LibraryThumbnailState state{LibraryThumbnailState::Placeholder};
    bool visible{};
    float distance_from_view{};
  };
  std::vector<ThumbnailCandidate> thumbnail_candidates;
  const assets::DerivativeProfile *const thumbnail_profile =
      thumbnails_enabled
          ? assets::derivative_profile(
                assets::DerivativeProfileId::Thumbnail320x180Srgb)
          : nullptr;
  // A model's thumbnail is a rendered three-quarter view of its own profile.
  const assets::DerivativeProfile *const model_thumbnail_profile =
      thumbnails_enabled
          ? assets::derivative_profile(
                assets::DerivativeProfileId::ModelTurntable320x180Srgb)
          : nullptr;
  struct Thumbnail final {
    const LibraryThumbnailObservation *observation{};
    LibraryThumbnailState state{LibraryThumbnailState::Placeholder};
  };
  const auto thumbnail_for = [&](const assets::Asset &asset,
                                 const gui::Rect &where) -> Thumbnail {
    Thumbnail result;
    const assets::AssetKind kind = assets::asset_kind(asset);
    const assets::DerivativeProfile *const profile =
        kind == assets::AssetKind::Media ? thumbnail_profile
        : kind == assets::AssetKind::Model ||
                kind == assets::AssetKind::ProjectionMesh
            ? model_thumbnail_profile
            : nullptr;
    if (profile == nullptr) {
      return result;
    }
    const std::optional<assets::ContentFingerprint> &fingerprint =
        assets::asset_fingerprint(asset);
    if (!fingerprint.has_value()) {
      return result;
    }
    const assets::DerivativeKey key = assets::make_derivative_key(
        assets::asset_id(asset), *fingerprint, *profile);
    if (!assets::valid_derivative_key(key)) {
      return result;
    }
    const auto found =
        std::find_if(thumbnails.begin(), thumbnails.end(),
                     [&key](const LibraryThumbnailObservation &observation) {
                       return observation.key == key;
                     });
    if (found != thumbnails.end()) {
      result.observation = std::addressof(*found);
      result.state = found->state;
    }
    const bool in_view = visible(gui::intersection(where, content));
    float distance = 0.0F;
    if (!in_view && where.bottom() <= content.y) {
      distance = content.y - where.y - where.height;
    } else if (!in_view) {
      distance = where.y - (content.bottom());
    }
    thumbnail_candidates.push_back(
        ThumbnailCandidate{key, result.state, in_view, distance});
    return result;
  };

  // Machine-local proxy and decode truth for one media asset.
  struct Runtime final {
    const LibraryProxyRuntimeObservation *proxy{};
    bool cpu_decode_only{};
  };
  const auto runtime_for = [&](const assets::Asset &asset) {
    Runtime result;
    const auto *const media = std::get_if<assets::MediaAsset>(&asset);
    if (media == nullptr) {
      return result;
    }
    result.proxy = library_proxy_runtime_observation(*media, proxy_runtime);
    const auto decode =
        std::find_if(proxy_runtime.begin(), proxy_runtime.end(),
                     [&](const LibraryProxyRuntimeObservation &observation) {
                       return observation.asset == media->id;
                     });
    result.cpu_decode_only =
        decode != proxy_runtime.end() &&
        decode->media_decode == LibraryMediaDecodeState::CpuOnly;
    return result;
  };
  const auto status_text = [&](const assets::Asset &asset,
                               const Thumbnail &thumbnail,
                               const Runtime &runtime) {
    std::string status;
    const auto append = [&status](std::string_view part) {
      if (part.empty()) {
        return;
      }
      if (!status.empty()) {
        status += " | ";
      }
      status += part;
    };
    if (is_changed(assets::asset_id(asset))) {
      append("SOURCE CHANGED");
    }
    if (thumbnails_enabled &&
        assets::asset_kind(asset) == assets::AssetKind::Media &&
        assets::asset_fingerprint(asset).has_value()) {
      append(library_thumbnail_status(thumbnail.state));
    }
    if (runtime.proxy != nullptr) {
      append(library_proxy_runtime_status(*runtime.proxy));
    }
    if (runtime.cpu_decode_only) {
      append("OUTPUT CPU DECODE ONLY");
    }
    const auto *const media = std::get_if<assets::MediaAsset>(&asset);
    const auto *const model = std::get_if<assets::ModelAsset>(&asset);
    if ((media != nullptr && !media->frame_count.has_value() &&
         !media->dimensions.has_value()) ||
        (model != nullptr && !model->triangle_count.has_value() &&
         !model->bounds.has_value())) {
      append("METADATA NOT PROBED");
    }
    return status;
  };
  const auto status_color = [&](const assets::Asset &asset,
                                const Thumbnail &thumbnail,
                                const Runtime &runtime) {
    if (is_changed(assets::asset_id(asset))) {
      return changed_color;
    }
    if (runtime.proxy != nullptr) {
      return runtime.proxy->transcode_progress.has_value()
                 ? theme.accent
                 : library_proxy_runtime_color(ctx, runtime.proxy->state);
    }
    return thumbnail.state == LibraryThumbnailState::Ready
               ? theme.text_muted
               : library_thumbnail_color(ctx, thumbnail.state);
  };
  const auto proxy_intent = [&](const assets::MediaAsset &media,
                                bool cpu_decode_only) {
    LibraryProxyPolicyIntent intent{};
    intent.asset = media.id;
    switch (media.proxy_playback_policy) {
    case assets::ProxyPlaybackPolicy::Source:
      intent.requested_profile = media.requested_proxy_profile.value_or(
          cpu_decode_only ? assets::DerivativeProfileId::ProxySourceCfrHevcMain10
                          : assets::DerivativeProfileId::Proxy720pCfrH264);
      intent.policy = assets::ProxyPlaybackPolicy::PreferProxy;
      break;
    case assets::ProxyPlaybackPolicy::PreferProxy:
      intent.requested_profile = media.requested_proxy_profile.value_or(
          assets::DerivativeProfileId::Proxy720pCfrH264);
      intent.policy = assets::ProxyPlaybackPolicy::RequireProxy;
      break;
    case assets::ProxyPlaybackPolicy::RequireProxy:
      intent.requested_profile.reset();
      intent.policy = assets::ProxyPlaybackPolicy::Source;
      break;
    }
    return intent;
  };
  const auto draw_preview = [&](const assets::Asset &asset,
                                const gui::Rect &preview,
                                const Thumbnail &thumbnail) {
    if (!visible(preview)) {
      return;
    }
    const bool media = assets::asset_kind(asset) == assets::AssetKind::Media;
    ctx.panel(preview, scale(theme.track, 0.84F));
    if (thumbnail.observation != nullptr &&
        thumbnail.observation->state == LibraryThumbnailState::Ready &&
        thumbnail.observation->texture != nullptr) {
      ctx.image(preview, *thumbnail.observation->texture);
    } else if (preview.width >= 8.0F && preview.height >= 8.0F) {
      const float icon =
          std::min({std::max(16.0F, preview.height * 0.4F), preview.width,
                    preview.height});
      draw_library_icon(
          ctx, media ? LibraryAddAction::Media : LibraryAddAction::Model,
          gui::Rect{preview.x + (preview.width - icon) * 0.5F,
                    preview.y + (preview.height - icon) * 0.5F, icon, icon});
    }
    ctx.outline(preview,
                with_alpha(thumbnails_enabled
                          ? library_thumbnail_color(ctx, thumbnail.state)
                          : theme.border,
                      0.78F));
    if (is_changed(assets::asset_id(asset))) {
      // An amber corner: this file is not the revision the Library holds.
      const float size = std::clamp(preview.height * 0.35F, 6.0F, 18.0F);
      ctx.draw_list().add_triangle(
          preview.right() - size, preview.y,
          preview.right(), preview.y, preview.right(),
          preview.y + size, changed_color);
    }
  };

  // ------------------------------------------------------------ input
  // Selection gestures. A press on something unselected selects it at once,
  // so a drag carries what was pressed; a plain click on a member of a
  // larger selection narrows to it on release, once it is clear no drag
  // was meant.
  const auto press_item = [&](doc::NodeId id) {
    drag_pressed_selected_ = is_selected(id);
    if (ctx.shift_held() && anchor_ != doc::NodeId{}) {
      const auto from = std::find(visible_order.begin(), visible_order.end(),
                                  anchor_);
      const auto to =
          std::find(visible_order.begin(), visible_order.end(), id);
      if (from != visible_order.end() && to != visible_order.end()) {
        selection_.assign(std::min(from, to), std::max(from, to) + 1);
        return;
      }
    }
    if (ctx.ctrl_held()) {
      if (drag_pressed_selected_) {
        std::erase(selection_, id);
      } else {
        selection_.push_back(id);
      }
      anchor_ = id;
      return;
    }
    if (!drag_pressed_selected_) {
      selection_.assign(1U, id);
    }
    anchor_ = id;
  };
  const auto begin_drag = [&](doc::NodeId id) {
    drag_primary_ = id;
    drag_start_x_ = ctx.pointer_press_x();
    drag_start_y_ = ctx.pointer_press_y();
    drag_moved_ = false;
    drag_items_.clear();
    if (is_selected(id)) {
      // The pressed item leads, then the rest in the order they are shown.
      drag_items_.push_back(id);
      for (const doc::NodeId shown : visible_order) {
        if (shown != id && is_selected(shown)) {
          drag_items_.push_back(shown);
        }
      }
    }
  };
  // Where a drag of the selection would be filed if let go here.
  std::optional<doc::NodeId> drop_folder;
  const auto folder_accepts = [&](doc::NodeId folder) {
    return std::none_of(drag_items_.begin(), drag_items_.end(),
                        [&](doc::NodeId item) {
                          return registry.folder(item) != nullptr &&
                                 registry.folder_within(folder, item);
                        });
  };
  const bool dragging = !drag_items_.empty() && drag_moved_;
  const bool file_drag = ctx.file_drag_over() &&
                         bounds.contains(ctx.file_drag_x(), ctx.file_drag_y());
  const float target_x = file_drag ? ctx.file_drag_x() : pointer_x;
  const float target_y = file_drag ? ctx.file_drag_y() : pointer_y;
  std::optional<doc::NodeId> file_folder;
  const bool files_dropped = !ctx.dropped_files().empty() &&
                             bounds.contains(ctx.drop_x(), ctx.drop_y());
  const auto over = [&](const gui::Rect &rect, float x, float y) {
    return rect.contains(x, y) && content.contains(x, y);
  };

  std::optional<doc::NodeId> menu_on;
  bool menu_on_background = false;
  bool menu_on_header = false;
  const auto open_entry = [&](const Entry &entry) {
    if (entry.kind == Entry::Kind::Asset) {
      frame.view_asset = entry.id;
    } else if (entry.kind == Entry::Kind::Folder) {
      if (list) {
        if (!expanded_.erase(entry.id)) {
          expanded_.insert(entry.id);
        }
      } else {
        open_folder(entry.id);
      }
    } else if (entry.kind == Entry::Kind::Generators) {
      if (list) {
        generators_expanded_ = !generators_expanded_;
      } else {
        generators_open_ = true;
        scroll_ = 0.0F;
      }
    }
  };
  // One shown entry's pointer handling, shared by rows and tiles.
  // `inert` is a control inside the row - the Proxy chip - whose clicks
  // are its own and never select, drag or open the row.
  const auto handle = [&](const Entry &entry, const gui::Rect &hit,
                          const gui::Interaction &interaction,
                          const gui::Rect &twirl, const gui::Rect &inert) {
    const bool hovered_here = over(hit, pointer_x, pointer_y);
    if (entry.kind == Entry::Kind::Generator) {
      if (interaction.pressed &&
          library_generator_placeable(entry.item->action)) {
        generator_drag_ = entry.item->action;
        generator_drag_moved_ = false;
        drag_start_x_ = ctx.pointer_press_x();
        drag_start_y_ = ctx.pointer_press_y();
      }
      // A press that became a drag is not also a click.
      if (interaction.clicked && !generator_drag_moved_) {
        frame.add = entry.item->action;
      }
      return;
    }
    if (entry.kind == Entry::Kind::Generators) {
      if (interaction.clicked &&
          (list || clicks_.double_click(0xFFFFU, ctx.time_seconds()))) {
        open_entry(entry);
      }
      return;
    }
    if (interaction.pressed &&
        inert.contains(ctx.pointer_press_x(), ctx.pointer_press_y())) {
      drag_primary_ = {};
      return;
    }
    if (interaction.pressed) {
      if (list && entry.kind == Entry::Kind::Folder &&
          twirl.contains(pointer_x, pointer_y)) {
        open_entry(entry);
        drag_items_.clear();
        return;
      }
      press_item(entry.id);
      begin_drag(entry.id);
    }
    if (interaction.released && drag_primary_ == entry.id) {
      if (!drag_moved_ && interaction.clicked && !ctx.ctrl_held() &&
          !ctx.shift_held()) {
        if (drag_pressed_selected_ && selection_.size() > 1U) {
          selection_.assign(1U, entry.id);
        }
        if (clicks_.double_click(entry.id.value(), ctx.time_seconds())) {
          open_entry(entry);
        }
      }
    }
    if (hovered_here && ctx.secondary_pointer_pressed()) {
      menu_on = entry.id;
      if (!is_selected(entry.id)) {
        selection_.assign(1U, entry.id);
        anchor_ = entry.id;
      }
    }
    // A folder under a drag, or under files from the desktop, is where
    // they would go.
    const doc::NodeId into = entry.kind == Entry::Kind::Folder
                                 ? entry.id
                                 : (list && entry.asset != nullptr
                                        ? registry.folder_of(*entry.asset)
                                        : doc::NodeId{});
    const bool can_land = entry.kind == Entry::Kind::Folder || list;
    if (dragging && can_land && hovered_here && folder_accepts(into)) {
      drop_folder = into;
    }
    if ((file_drag || files_dropped) && can_land) {
      const float x = files_dropped ? ctx.drop_x() : target_x;
      const float y = files_dropped ? ctx.drop_y() : target_y;
      if (over(hit, x, y)) {
        file_folder = into;
      }
    }
  };

  // ------------------------------------------------------------ list view
  ctx.push_clip(content);
  const gui::Rect visible_content = content;
  if (entries.empty()) {
    ctx.label(gui::Rect{content.x + 10.0F, content.y + 6.0F,
                        std::max(content.width - 20.0F, 0.0F), 24.0F},
              searching          ? "NO LIBRARY RESULTS"
              : folder_ != doc::NodeId{} && !list
                  ? "THIS FOLDER IS EMPTY - DRAG ASSETS HERE"
                  : "NO ASSETS - IMPORT OR DROP FILES HERE",
              theme.text_muted);
  }
  bool over_row = false;
  if (list) {
    float y = content.y - scroll_;
    const float origin_x = content.x - scroll_x_;
    std::size_t stripe = 0U;
    for (const Entry &entry : entries) {
      // Rows alternate by position in the list, not on screen, so scrolling
      // never makes the stripes swap.
      const std::size_t row_number = stripe++;
      const bool asset_row = entry.kind == Entry::Kind::Asset;
      const float row_height = asset_row ? list_row_height : 22.0F;
      y += entry.gap;
      const gui::Rect row{content.x, y, content.width, row_height};
      y += row_height;
      over_row = over_row || over(row, pointer_x, pointer_y);
      const bool shown = visible(gui::intersection(row, visible_content));
      const gui::UiId row_id =
          entry.kind == Entry::Kind::Generator
              ? mix(id_, "library.generator",
                    static_cast<core::u64>(entry.item->action))
          : entry.kind == Entry::Kind::Generators
              ? mix(id_, "library.generators")
              : mix(id_, "library.row", entry.id.value());
      const gui::Interaction interaction =
          shown ? clipped_grab(ctx, row_id, row, visible_content)
                : gui::Interaction{};
      const float name_x = origin_x + 6.0F +
                           static_cast<float>(entry.depth) * list_indent;
      const gui::Rect twirl{name_x, row.y, twirl_width, row.height};
      gui::Rect proxy_chip{};
      if (asset_row &&
          std::holds_alternative<assets::MediaAsset>(*entry.asset)) {
        float x = origin_x + column_width_[0];
        for (std::size_t index = 1U; index < columns.size(); ++index) {
          const float width =
              column_width_[static_cast<std::size_t>(columns[index])];
          if (columns[index] == LibraryColumn::Proxy) {
            proxy_chip = gui::Rect{x + 4.0F, row.y + row.height * 0.5F - 8.0F,
                                   std::min(width - 8.0F, 44.0F), 16.0F};
            break;
          }
          x += width;
        }
      }
      handle(entry, row, interaction, twirl, proxy_chip);

      Thumbnail thumbnail;
      if (asset_row) {
        const float preview_height = list_thumb_height;
        const gui::Rect preview{
            name_x + twirl_width,
            row.y + (row.height - preview_height) * 0.5F,
            std::round(preview_height * thumbnail_aspect), preview_height};
        thumbnail = thumbnail_for(*entry.asset, preview);
      }
      if (!shown) {
        continue;
      }
      const bool selected_row =
          entry.id != doc::NodeId{} && is_selected(entry.id);
      gui::UiColor fill = row_number % 2U == 0U ? theme.row : theme.row_alt;
      if (entry.kind == Entry::Kind::Generators) {
        fill = theme.header;
      }
      if (selected_row) {
        fill = theme.selection;
      } else if (interaction.hovered) {
        fill = scale(theme.widget_hover, 1.04F);
      }
      ctx.panel(row, fill);
      if (drop_folder.has_value() && entry.kind == Entry::Kind::Folder &&
          *drop_folder == entry.id) {
        ctx.panel(row, with_alpha(theme.accent, 0.2F));
        ctx.outline(row, theme.accent);
      }

      // The Name column: depth, twirl, thumbnail or icon, name.
      const float name_right = origin_x + column_width_[0] - 4.0F;
      float text_x = name_x + twirl_width;
      if (entry.kind == Entry::Kind::Folder ||
          entry.kind == Entry::Kind::Generators) {
        const bool open = entry.kind == Entry::Kind::Folder
                              ? expanded_.contains(entry.id)
                              : generators_expanded_;
        draw_twirl(ctx, twirl.x + twirl.width * 0.5F,
                   row.y + row.height * 0.5F, open, theme.text_muted);
        draw_folder_icon(ctx, gui::Rect{text_x, row.y + 3.0F, 16.0F, 16.0F},
                         entry.kind == Entry::Kind::Generators
                             ? theme.keyframe
                             : theme.playhead,
                         open);
        text_x += 22.0F;
      } else if (entry.kind == Entry::Kind::Generator) {
        draw_library_icon(ctx, entry.item->action,
                          gui::Rect{text_x, row.y + 3.0F, 16.0F, 16.0F});
        text_x += 22.0F;
      } else {
        const float preview_height = list_thumb_height;
        const gui::Rect preview{text_x,
                                row.y + (row.height - preview_height) * 0.5F,
                                std::round(preview_height * thumbnail_aspect),
                                preview_height};
        draw_preview(*entry.asset, preview, thumbnail);
        text_x += preview.width + 7.0F;
      }
      const gui::Rect name_bounds{text_x, row.y,
                                  std::max(name_right - text_x, 0.0F),
                                  row.height};
      std::string name;
      if (entry.kind == Entry::Kind::Generators) {
        name = "Generators";
      } else if (entry.kind == Entry::Kind::Generator) {
        name = std::string{entry.item->name};
      } else if (entry.kind == Entry::Kind::Folder) {
        name = entry.folder->display_name;
      } else {
        name = std::string{assets::asset_display_name(*entry.asset)};
      }
      if (entry.id != doc::NodeId{} && rename_.editing(entry.id.value())) {
        if (auto renamed = rename_.draw(
                ctx, gui::Rect{name_bounds.x - 3.0F, row.y + 1.0F,
                               std::max(name_bounds.width, 80.0F),
                               std::min(row.height - 2.0F, 22.0F)})) {
          assets::LibraryEdit edit{};
          edit.kind = assets::LibraryEdit::Kind::Rename;
          edit.target = entry.id;
          edit.name = std::move(*renamed);
          frame.library_edits.push_back(std::move(edit));
        }
      } else {
        ctx.label(name_bounds, fit(ctx, name, name_bounds.width),
                  theme.text);
      }
      if (entry.kind == Entry::Kind::Folder ||
          entry.kind == Entry::Kind::Generators) {
        const std::size_t held = entry.kind == Entry::Kind::Folder
                                     ? child_count(entry.id)
                                     : library_items.size();
        const std::string count =
            std::to_string(held) + (held == 1U ? " item" : " items");
        const float x = origin_x + column_width_[0] + 6.0F;
        ctx.label(gui::Rect{x, row.y, ctx.font_width(count) + 4.0F,
                            row.height},
                  count, theme.text_muted);
      }
      if (entry.kind == Entry::Kind::Generator) {
        const float x = origin_x + column_width_[0] + 6.0F;
        ctx.label(gui::Rect{x, row.y, std::max(columns_width -
                                                   column_width_[0] - 12.0F,
                                               0.0F),
                            row.height},
                  entry.item->detail, theme.text_muted);
        draw_plus(ctx, std::min(origin_x + columns_width,
                                content.right()) -
                           12.0F,
                  row.y + row.height * 0.5F,
                  interaction.hovered ? theme.accent : theme.text_muted);
        continue;
      }
      if (!asset_row) {
        continue;
      }

      // The metadata columns.
      const Runtime runtime = runtime_for(*entry.asset);
      const auto *const media = std::get_if<assets::MediaAsset>(entry.asset);
      float x = origin_x + column_width_[0];
      for (std::size_t index = 1U; index < columns.size(); ++index) {
        const LibraryColumn column = columns[index];
        const float width = column_width_[static_cast<std::size_t>(column)];
        const gui::Rect cell{x + 6.0F, row.y, std::max(width - 12.0F, 0.0F),
                             row.height};
        x += width;
        if (cell.x > content.right() ||
            cell.right() < content.x) {
          continue;
        }
        if (column == LibraryColumn::Status) {
          float status_x = cell.x;
          if (runtime.cpu_decode_only) {
            draw_cpu_decode_warning(
                ctx, gui::Rect{status_x, row.y + row.height * 0.5F - 6.0F,
                               12.0F, 11.0F});
            status_x += 16.0F;
          }
          const std::string status =
              status_text(*entry.asset, thumbnail, runtime);
          ctx.label(gui::Rect{status_x, row.y,
                              std::max(cell.right() - status_x, 0.0F),
                              row.height},
                    fit(ctx, status, cell.right() - status_x),
                    status_color(*entry.asset, thumbnail, runtime));
          if (!status.empty()) {
            ctx.tooltip(cell, status);
          }
          if (runtime.proxy != nullptr &&
              runtime.proxy->transcode_progress.has_value()) {
            const LibraryProxyTranscodeProgress &progress =
                *runtime.proxy->transcode_progress;
            const float fraction =
                progress.total_frames == 0U
                    ? 0.0F
                    : std::clamp(
                          static_cast<float>(progress.completed_frames) /
                              static_cast<float>(progress.total_frames),
                          0.0F, 1.0F);
            const gui::Rect track{cell.x, row.bottom() - 4.0F,
                                  cell.width, 3.0F};
            ctx.panel(track, scale(theme.track, 0.78F));
            if (fraction > 0.0F) {
              ctx.panel(gui::Rect{track.x, track.y, track.width * fraction,
                                  track.height},
                        theme.accent);
            }
          }
          continue;
        }
        const std::string text = library_column_text(*entry.asset, column);
        if (column == LibraryColumn::Proxy && media != nullptr) {
          const gui::Rect chip = proxy_chip;
          const bool chip_hovered =
              over(chip, pointer_x, pointer_y) && !dragging;
          ctx.panel(chip, chip_hovered ? theme.widget_hover : theme.widget);
          ctx.label(gui::Rect{chip.x + 5.0F, chip.y, chip.width - 6.0F,
                              chip.height},
                    text, chip_hovered ? theme.accent : theme.text_muted);
          ctx.tooltip(chip, "Proxy playback: click for Source, Auto or "
                            "Require");
          if (interaction.clicked &&
              chip.contains(ctx.pointer_press_x(), ctx.pointer_press_y()) &&
              chip.contains(ctx.pointer_release_x(),
                            ctx.pointer_release_y())) {
            frame.proxy_policy =
                proxy_intent(*media, runtime.cpu_decode_only);
          }
          continue;
        }
        if (text.empty()) {
          continue;
        }
        const std::string shown_text = fit(ctx, text, cell.width);
        const float text_width = ctx.font_width(shown_text);
        const gui::Rect at =
            spec(column).numeric
                ? gui::Rect{cell.x + std::max(cell.width - text_width, 0.0F),
                            cell.y, text_width, cell.height}
                : cell;
        ctx.label(at, shown_text,
                  column == LibraryColumn::Codec ||
                          column == LibraryColumn::Type ||
                          column == LibraryColumn::Path
                      ? theme.text_muted
                      : theme.text);
        if (shown_text.size() != text.size()) {
          ctx.tooltip(cell, text);
        }
      }
      ctx.tooltip(name_bounds, assets::asset_source(*entry.asset));
    }
  } else {
    // ---------------------------------------------------------- grid view
    for (std::size_t index = 0U; index < entries.size(); ++index) {
      const Entry &entry = entries[index];
      const gui::Rect tile{content.x + tile_at[index].x,
                           content.y + tile_at[index].y - scroll_, tile_width,
                           tile_height};
      const gui::Rect image{tile.x + 4.0F, tile.y + 4.0F, tile.width - 8.0F,
                            tile_image_height - 8.0F};
      Thumbnail thumbnail;
      if (entry.kind == Entry::Kind::Asset) {
        thumbnail = thumbnail_for(*entry.asset, tile);
      }
      const bool shown = visible(gui::intersection(tile, visible_content));
      const gui::UiId tile_id =
          entry.kind == Entry::Kind::Generator
              ? mix(id_, "library.generator",
                    static_cast<core::u64>(entry.item->action))
          : entry.kind == Entry::Kind::Generators
              ? mix(id_, "library.generators")
              : mix(id_, "library.tile", entry.id.value());
      const gui::Interaction interaction =
          shown ? clipped_grab(ctx, tile_id, tile, visible_content)
                : gui::Interaction{};
      handle(entry, tile, interaction, gui::Rect{}, gui::Rect{});
      if (!shown) {
        continue;
      }
      const bool selected_tile =
          entry.id != doc::NodeId{} && is_selected(entry.id);
      if (selected_tile || interaction.hovered) {
        ctx.panel(tile, selected_tile ? theme.selection : theme.widget_hover);
      }
      const bool drop_here = drop_folder.has_value() &&
                             entry.kind == Entry::Kind::Folder &&
                             *drop_folder == entry.id;
      std::string name;
      std::string detail;
      if (entry.kind == Entry::Kind::Asset) {
        draw_preview(*entry.asset, image, thumbnail);
        name = std::string{assets::asset_display_name(*entry.asset)};
        if (assets::asset_kind(*entry.asset) == assets::AssetKind::Media) {
          detail = library_column_text(*entry.asset, LibraryColumn::Duration);
          const std::string resolution =
              library_column_text(*entry.asset, LibraryColumn::Resolution);
          if (!resolution.empty()) {
            detail += detail.empty() ? resolution : "  " + resolution;
          }
        } else {
          const std::string vertices =
              library_column_text(*entry.asset, LibraryColumn::Vertices);
          detail = vertices.empty() ? std::string{} : vertices + " verts";
          const std::string texture =
              library_column_text(*entry.asset, LibraryColumn::TextureSize);
          if (!texture.empty()) {
            detail += detail.empty() ? texture : "  " + texture;
          }
        }
        const Runtime runtime = runtime_for(*entry.asset);
        if (runtime.cpu_decode_only) {
          draw_cpu_decode_warning(
              ctx, gui::Rect{image.right() - 16.0F, image.y + 4.0F,
                             12.0F, 11.0F});
        }
        const auto *const media = std::get_if<assets::MediaAsset>(entry.asset);
        if (media != nullptr &&
            media->proxy_playback_policy != assets::ProxyPlaybackPolicy::Source) {
          const std::string_view label =
              library_proxy_label(media->proxy_playback_policy);
          const gui::Rect badge{image.x + 4.0F, image.y + 4.0F,
                                ctx.font_width(label) + 8.0F, 15.0F};
          ctx.panel(badge, with_alpha(theme.background, 0.8F));
          ctx.label(gui::Rect{badge.x + 4.0F, badge.y, badge.width - 4.0F,
                              badge.height},
                    label, status_color(*entry.asset, thumbnail, runtime));
        }
        ctx.tooltip(tile, assets::asset_source(*entry.asset));
      } else {
        ctx.panel(image, scale(theme.track, 0.9F));
        const float icon = std::min(image.width, image.height) * 0.5F;
        const gui::Rect icon_bounds{image.x + (image.width - icon) * 0.5F,
                                    image.y + (image.height - icon) * 0.5F,
                                    icon, icon};
        if (entry.kind == Entry::Kind::Generator) {
          draw_library_icon(ctx, entry.item->action, icon_bounds);
          name = std::string{entry.item->name};
          detail = std::string{entry.item->detail};
        } else if (entry.kind == Entry::Kind::Generators) {
          draw_folder_icon(ctx, icon_bounds, theme.keyframe, false);
          name = "Generators";
          detail = std::to_string(library_items.size()) + " items";
        } else {
          draw_folder_icon(ctx, icon_bounds, theme.playhead, drop_here);
          name = entry.folder->display_name;
          const std::size_t held = child_count(entry.id);
          detail = std::to_string(held) + (held == 1U ? " item" : " items");
        }
      }
      if (drop_here) {
        ctx.panel(tile, with_alpha(theme.accent, 0.2F));
        ctx.outline(tile, theme.accent);
      }
      const gui::Rect name_bounds{tile.x + 4.0F, image.bottom() + 3.0F,
                                  tile.width - 8.0F, 16.0F};
      if (entry.id != doc::NodeId{} && rename_.editing(entry.id.value())) {
        if (auto renamed = rename_.draw(ctx, name_bounds)) {
          assets::LibraryEdit edit{};
          edit.kind = assets::LibraryEdit::Kind::Rename;
          edit.target = entry.id;
          edit.name = std::move(*renamed);
          frame.library_edits.push_back(std::move(edit));
        }
      } else {
        ctx.label(name_bounds, fit(ctx, name, name_bounds.width), theme.text);
      }
      ctx.label(gui::Rect{name_bounds.x, name_bounds.y + 15.0F,
                          name_bounds.width, 14.0F},
                fit(ctx, detail, name_bounds.width), theme.text_muted);
    }
  }
  // Empty space: a background menu, a click that clears the selection, and
  // in List view somewhere to file things back at the top level.
  if (content.contains(pointer_x, pointer_y) && !ctx.pointer_blocked()) {
    if (ctx.secondary_pointer_pressed() && !menu_on.has_value()) {
      menu_on_background = true;
    }
    if (ctx.pointer_pressed() && ctx.hot() == gui::UiId{} &&
        !ctx.ctrl_held() && !ctx.shift_held()) {
      selection_.clear();
    }
    if (dragging && list && !drop_folder.has_value() && !over_row) {
      drop_folder = doc::NodeId{};
    }
  }
  if (files_dropped && !file_folder.has_value()) {
    file_folder = list ? doc::NodeId{} : folder_;
  }
  ctx.pop_clip();

  // ------------------------------------------------------------ header
  if (list) {
    ctx.panel(subheader, theme.header);
    ctx.panel(gui::Rect{subheader.x, subheader.bottom() - 1.0F,
                        subheader.width, 1.0F},
              theme.divider);
    ctx.push_clip(subheader);
    float x = subheader.x - scroll_x_;
    for (const LibraryColumn column : columns) {
      const std::size_t index = static_cast<std::size_t>(column);
      const float width = column_width_[index];
      const gui::Rect title{x, subheader.y, width, subheader.height};
      const gui::Rect sort_hit{x + 3.0F, subheader.y,
                               std::max(width - 7.0F, 0.0F), subheader.height};
      const gui::Rect edge{x + width - 4.0F, subheader.y, 7.0F,
                           subheader.height};
      x += width;
      const gui::Interaction sort =
          clipped_grab(ctx, mix(id_, "library.sort", index), sort_hit,
                       subheader);
      const gui::Interaction resize =
          clipped_grab(ctx, mix(id_, "library.resize", index), edge,
                       subheader);
      ctx.cursor_for(resize, gui::Cursor::ResizeHorizontal);
      if (resize.pressed) {
        resizing_ = column;
        resize_start_width_ = width;
      }
      if (resize.held && resizing_ == column) {
        column_width_[index] = std::max(
            minimum_column_width,
            resize_start_width_ + pointer_x - ctx.pointer_press_x());
      }
      if (resize.released) {
        resizing_.reset();
      }
      if (sort.clicked) {
        if (sort_column_ == column) {
          sort_descending_ = !sort_descending_;
        } else {
          sort_column_ = column;
          sort_descending_ = false;
        }
      }
      if (sort.hovered) {
        ctx.panel(title, theme.widget_hover);
      }
      const bool sorted = sort_column_ == column;
      const std::string label =
          fit(ctx, spec(column).title, width - (sorted ? 26.0F : 14.0F));
      ctx.label(gui::Rect{title.x + 6.0F, title.y, width - 12.0F,
                          title.height},
                label, sorted ? theme.text : theme.text_muted);
      if (sorted) {
        const float arrow_x = title.x + 10.0F + ctx.font_width(label);
        const float centre_y = title.y + title.height * 0.5F;
        if (sort_descending_) {
          ctx.draw_list().add_triangle(arrow_x, centre_y - 2.0F,
                                       arrow_x + 7.0F, centre_y - 2.0F,
                                       arrow_x + 3.5F, centre_y + 2.0F,
                                       theme.accent);
        } else {
          ctx.draw_list().add_triangle(arrow_x, centre_y + 2.0F,
                                       arrow_x + 3.5F, centre_y - 2.0F,
                                       arrow_x + 7.0F, centre_y + 2.0F,
                                       theme.accent);
        }
      }
      ctx.panel(gui::Rect{title.x + width - 1.0F, title.y + 4.0F, 1.0F,
                          title.height - 8.0F},
                theme.border);
    }
    if (subheader.contains(pointer_x, pointer_y) &&
        ctx.secondary_pointer_pressed()) {
      menu_on_header = true;
    }
    ctx.pop_clip();
  } else {
    // The folder path. Every step is a place to go, and a place to drop.
    ctx.panel(subheader, theme.header);
    ctx.panel(gui::Rect{subheader.x, subheader.bottom() - 1.0F,
                        subheader.width, 1.0F},
              theme.divider);
    ctx.push_clip(subheader);
    std::vector<std::pair<doc::NodeId, std::string>> path;
    if (searching) {
      path.emplace_back(doc::NodeId{}, "Search Results");
    } else if (generators_open_) {
      path.emplace_back(doc::NodeId{}, "Library");
      path.emplace_back(doc::NodeId{}, "Generators");
    } else {
      for (doc::NodeId walk = folder_; walk != doc::NodeId{};) {
        const assets::AssetFolder *const held = registry.folder(walk);
        if (held == nullptr || path.size() > 32U) {
          break;
        }
        path.emplace_back(held->id, held->display_name);
        walk = held->parent;
      }
      path.emplace_back(doc::NodeId{}, "Library");
      std::reverse(path.begin(), path.end());
    }
    float x = subheader.x + 6.0F;
    for (std::size_t step = 0U; step < path.size(); ++step) {
      const bool last = step + 1U == path.size();
      const float width = ctx.font_width(path[step].second) + 10.0F;
      const gui::Rect crumb{x, subheader.y + 1.0F, width,
                            subheader.height - 2.0F};
      const gui::Interaction interaction = clipped_grab(
          ctx, mix(id_, "library.crumb", step), crumb, subheader);
      const bool drop_here = dragging && !searching && !generators_open_ &&
                             crumb.contains(pointer_x, pointer_y) &&
                             folder_accepts(path[step].first);
      if (drop_here) {
        drop_folder = path[step].first;
        ctx.panel(crumb, with_alpha(theme.accent, 0.25F));
        ctx.outline(crumb, theme.accent);
      } else if (interaction.hovered && !last) {
        ctx.panel(crumb, theme.widget_hover);
      }
      ctx.label(gui::Rect{crumb.x + 5.0F, crumb.y, width - 5.0F, crumb.height},
                path[step].second, last ? theme.text : theme.text_muted);
      if (interaction.clicked && !last && !searching) {
        open_folder(path[step].first);
      }
      x += width;
      if (!last) {
        ctx.label(gui::Rect{x, subheader.y, 10.0F, subheader.height}, ">",
                  theme.text_muted);
        x += 12.0F;
      }
    }
    ctx.pop_clip();
  }

  // ------------------------------------------------------------ the drag
  if (!drag_items_.empty()) {
    if (ctx.pointer_down()) {
      const float dx = pointer_x - drag_start_x_;
      const float dy = pointer_y - drag_start_y_;
      drag_moved_ = drag_moved_ || dx * dx + dy * dy >= drag_threshold_squared;
    }
    std::vector<doc::NodeId> dragged_assets;
    for (const doc::NodeId item : drag_items_) {
      if (registry.asset(item) != nullptr) {
        dragged_assets.push_back(item);
      }
    }
    const bool released = !ctx.pointer_down();
    if (drag_moved_) {
      if (!dragged_assets.empty()) {
        frame.dragging_asset = dragged_assets.front();
        frame.dragging_assets = dragged_assets;
      }
      // The thumbnail of the asset the drag was started on rides with it.
      const doc::NodeId lead =
          dragged_assets.empty() ? drag_items_.front() : dragged_assets.front();
      ghost_live_ = !released;
      ghost_count_ = drag_items_.size();
      ghost_model_ = false;
      if (const assets::Asset *const asset = registry.asset(lead)) {
        ghost_label_ = std::string{assets::asset_display_name(*asset)};
        ghost_model_ =
            assets::asset_kind(*asset) != assets::AssetKind::Media;
        if (thumbnails_enabled && !ghost_model_) {
          if (const auto &fingerprint = assets::asset_fingerprint(*asset);
              fingerprint.has_value() && thumbnail_profile != nullptr) {
            const assets::DerivativeKey key = assets::make_derivative_key(
                lead, *fingerprint, *thumbnail_profile);
            const auto found = std::find_if(
                thumbnails.begin(), thumbnails.end(),
                [&key](const LibraryThumbnailObservation &observation) {
                  return observation.key == key &&
                         observation.state == LibraryThumbnailState::Ready;
                });
            if (found != thumbnails.end()) {
              ghost_texture_ = found->texture;
            }
          }
        }
      } else if (const assets::AssetFolder *const folder =
                     registry.folder(lead)) {
        ghost_label_ = folder->display_name;
      }
      if (released) {
        if (pointer_inside) {
          if (drop_folder.has_value()) {
            assets::LibraryEdit edit{};
            edit.kind = assets::LibraryEdit::Kind::Move;
            edit.destination = *drop_folder;
            edit.items = drag_items_;
            frame.library_edits.push_back(std::move(edit));
          }
        } else if (!dragged_assets.empty()) {
          frame.asset_drop = LibraryAssetDrop{
              dragged_assets.front(), ctx.pointer_release_x(),
              ctx.pointer_release_y(), dragged_assets};
          frame.dragging_asset.reset();
          frame.dragging_assets.clear();
        }
      }
    }
    if (released) {
      drag_items_.clear();
      drag_primary_ = {};
      drag_moved_ = false;
    }
  }

  // A generator carried out to Stage. Let go back inside the Library it is
  // nothing; outside, the shell places it where it landed.
  if (generator_drag_.has_value()) {
    if (ctx.pointer_down()) {
      const float dx = pointer_x - drag_start_x_;
      const float dy = pointer_y - drag_start_y_;
      generator_drag_moved_ = generator_drag_moved_ ||
                              dx * dx + dy * dy >= drag_threshold_squared;
    }
    const bool released = !ctx.pointer_down();
    if (generator_drag_moved_) {
      frame.dragging_generator = generator_drag_;
      const auto item = std::find_if(
          library_items.begin(), library_items.end(),
          [this](const LibraryItem &candidate) {
            return candidate.action == *generator_drag_;
          });
      ghost_live_ = !released;
      ghost_count_ = 1U;
      ghost_model_ = false;
      ghost_generator_ = generator_drag_;
      ghost_label_ = item != library_items.end() ? std::string{item->name}
                                                 : std::string{};
      if (released) {
        frame.dragging_generator.reset();
        if (!pointer_inside) {
          frame.generator_drop = LibraryGeneratorDrop{
              *generator_drag_, ctx.pointer_release_x(),
              ctx.pointer_release_y()};
        }
      }
    }
    if (released) {
      generator_drag_.reset();
      generator_drag_moved_ = false;
    }
  }

  // Files from the desktop: a frame round the panel while they are over it,
  // and an import into the folder they land on.
  if (file_drag) {
    ctx.outline(content.inflated(-1.0F, -1.0F), theme.accent, 2.0F);
    const std::string_view hint = "Drop to import into the Library";
    // Centred near the bottom of the rows.
    gui::Rect bottom = gui::inset(content, gui::Padding{0.0F, 0.0F, 0.0F, 8.0F});
    bottom = gui::cut_bottom(bottom, 22.0F);
    const gui::Rect badge =
        gui::centered(bottom, ctx.font_width(hint) + 16.0F, bottom.height);
    ctx.panel(badge, theme.accent);
    ctx.label_centered(badge, hint, theme.background);
  }
  if (files_dropped) {
    frame.import_files.assign(ctx.dropped_files().begin(),
                              ctx.dropped_files().end());
    frame.import_folder = file_folder.value_or(doc::NodeId{});
  }

  // ------------------------------------------------------------ footer
  ctx.panel(footer, theme.background);
  {
    gui::Rect rule = footer;
    ctx.panel(gui::cut_top(rule, 1.0F), theme.divider);
  }
  {
    // The view switches, the thumbnail size, and what is selected at the
    // right.
    gui::Rect strip = gui::inset(footer, gui::Padding{4.0F, 0.0F, 8.0F, 0.0F});
    for (const LibraryView choice : {LibraryView::List, LibraryView::Grid}) {
      const gui::Rect button =
          gui::inset(gui::cut_left(strip, 26.0F),
                     gui::Padding{0.0F, 2.0F, 2.0F, 2.0F});
      const gui::Interaction interaction = ctx.grab(
          mix(id_, "library.view", static_cast<core::u64>(choice)), button);
      const bool engaged = view_ == choice;
      if (engaged || interaction.hovered) {
        ctx.panel(button,
                  engaged ? theme.widget_active : theme.widget_hover);
      }
      draw_view_icon(ctx, button, choice,
                     engaged ? theme.text : theme.text_muted);
      ctx.tooltip(button, choice == LibraryView::List ? "List View"
                                                      : "Grid View");
      if (interaction.clicked && view_ != choice) {
        view_ = choice;
        scroll_ = 0.0F;
      }
    }
    static_cast<void>(gui::cut_left(strip, 6.0F));
    const float slider_width = std::min(110.0F, strip.width - 82.0F);
    if (slider_width > 30.0F) {
      // The thumbnail size, small to large, as in every bin's zoom slider.
      gui::Rect zoom = strip;
      draw_library_icon(ctx, LibraryAddAction::Media,
                        gui::Rect{zoom.x, footer.y + 7.0F, 9.0F, 9.0F});
      static_cast<void>(gui::cut_left(zoom, 12.0F));
      float size = icon_size();
      const gui::Rect slider =
          gui::inset(gui::cut_left(zoom, slider_width), 0.0F, 4.0F);
      if (ctx.slider(mix(id_, "library.zoom"), slider, size, 0.0F, 1.0F)) {
        set_icon_size(size);
      }
      ctx.tooltip(slider, "Thumbnail size (Ctrl+Wheel)");
      draw_library_icon(
          ctx, LibraryAddAction::Media,
          gui::Rect{slider.right() + 3.0F, footer.y + 4.0F, 14.0F,
                    14.0F});
    }
    std::string summary;
    if (selection_.size() == 1U) {
      if (const assets::Asset *const asset = registry.asset(selection_[0])) {
        summary = std::string{assets::asset_display_name(*asset)};
      } else if (const assets::AssetFolder *const folder =
                     registry.folder(selection_[0])) {
        summary = folder->display_name;
      }
    } else if (selection_.size() > 1U) {
      summary = std::to_string(selection_.size()) + " selected";
    }
    if (!summary.empty()) {
      const std::string shown = fit(ctx, summary, 140.0F);
      ctx.label(gui::cut_right(strip, ctx.font_width(shown)), shown,
                theme.text_muted);
    }
  }

  // ------------------------------------------------------------ keyboard
  const doc::NodeId new_folder_parent = [&] {
    if (!list) {
      return generators_open_ || searching ? doc::NodeId{} : folder_;
    }
    if (selection_.size() == 1U) {
      if (registry.folder(selection_[0]) != nullptr) {
        return selection_[0];
      }
      if (const assets::Asset *const asset = registry.asset(selection_[0])) {
        return registry.folder_of(*asset);
      }
    }
    return doc::NodeId{};
  }();
  const auto begin_rename_of = [&](doc::NodeId id) {
    if (const assets::Asset *const asset = registry.asset(id)) {
      rename_.begin(ctx, id.value(), assets::asset_display_name(*asset));
    } else if (const assets::AssetFolder *const folder = registry.folder(id)) {
      rename_.begin(ctx, id.value(), folder->display_name);
    }
  };
  const auto remove_selection = [&] {
    for (const doc::NodeId id : selection_) {
      if (registry.asset(id) != nullptr) {
        frame.remove_assets.push_back(id);
      } else if (registry.folder(id) != nullptr) {
        assets::LibraryEdit edit{};
        edit.kind = assets::LibraryEdit::Kind::DeleteFolder;
        edit.target = id;
        frame.library_edits.push_back(std::move(edit));
      }
    }
    selection_.clear();
  };
  if (focused_ && !ctx.typing() && !rename_.active()) {
    if (ctx.key_pressed(gui::Key::Rename) && selection_.size() == 1U) {
      begin_rename_of(selection_[0]);
    }
    if (ctx.key_pressed(gui::Key::Delete) && !selection_.empty()) {
      remove_selection();
      frame.consume_delete = true;
    }
    if (ctx.key_pressed(gui::Key::Enter)) {
      for (const doc::NodeId id : selection_) {
        if (registry.asset(id) != nullptr) {
          frame.view_asset = id;
          break;
        }
      }
    }
    if (ctx.key_pressed(gui::Key::Backspace) && !list) {
      if (generators_open_) {
        generators_open_ = false;
      } else if (const assets::AssetFolder *const folder =
                     registry.folder(folder_)) {
        open_folder(folder->parent);
      }
    }
  }

  // ------------------------------------------------------------ menus
  const auto add = [this](std::string label, MenuCommand command,
                          std::size_t extra = 0U) {
    menu_entries_.push_back(std::move(label));
    menu_commands_.push_back(
        static_cast<core::u8>(static_cast<std::size_t>(command) + extra));
  };
  const auto open_menu = [&](doc::NodeId target) {
    menu_target_ = target;
    menu_open_ = true;
    menu_entries_.clear();
    menu_commands_.clear();
  };
  if (menu_on.has_value()) {
    open_menu(*menu_on);
    if (registry.asset(*menu_on) != nullptr) {
      if (is_changed(*menu_on)) {
        const std::size_t accepting = static_cast<std::size_t>(
            std::count_if(selection_.begin(), selection_.end(), is_changed));
        add(accepting > 1U ? "Accept " + std::to_string(accepting) +
                                 " Changed Sources"
                           : std::string{"Accept Changed Source"},
            MenuCommand::AcceptChanged);
      }
      add("Open in Asset Viewer   Enter", MenuCommand::View);
      add("Rename           F2", MenuCommand::Rename);
      add("New Folder", MenuCommand::NewFolder);
      if (registry.folder_of(*registry.asset(*menu_on)) != doc::NodeId{}) {
        add("Move to Top Level", MenuCommand::MoveToTop);
      }
      add(selection_.size() > 1U ? "Remove Selected from Library   Del"
                                 : "Remove from Library   Del",
          MenuCommand::Remove);
    } else {
      add(list ? (expanded_.contains(*menu_on) ? "Collapse" : "Expand")
               : "Open",
          MenuCommand::OpenFolder);
      add("Rename           F2", MenuCommand::Rename);
      add("New Folder Inside", MenuCommand::NewFolderInside);
      if (const assets::AssetFolder *const folder = registry.folder(*menu_on);
          folder != nullptr && folder->parent != doc::NodeId{}) {
        add("Move to Top Level", MenuCommand::MoveToTop);
      }
      add("Delete Folder (keeps assets)", MenuCommand::DeleteFolder);
    }
    ctx.open_context_menu(menu_id, pointer_x, pointer_y);
  } else if (menu_on_header) {
    open_menu({});
    for (std::size_t column = 1U; column < library_column_count; ++column) {
      add(std::string{column_visible_[column] ? "[x] " : "[  ] "} +
              std::string{column_specs[column].title},
          MenuCommand::ToggleColumn, column);
    }
    ctx.open_context_menu(menu_id, pointer_x, pointer_y);
  } else if (menu_on_background || open_import_menu) {
    open_menu({});
    if (!open_import_menu) {
      add("New Folder", MenuCommand::NewFolder);
    }
    add("Import Media...", MenuCommand::ImportMedia);
    add("Import Model...", MenuCommand::ImportModel);
    if (!open_import_menu) {
      add("Rescan Sources", MenuCommand::Rescan);
      if (!changed_shown.empty()) {
        add("Accept All Changed Sources (" +
                std::to_string(changed_shown.size()) + ")",
            MenuCommand::AcceptAllChanged);
      }
      add(list ? "Show as Grid" : "Show as List",
          list ? MenuCommand::ShowGrid : MenuCommand::ShowList);
    }
    ctx.open_context_menu(menu_id,
                          open_import_menu ? import_slot.x - 2.0F : pointer_x,
                          open_import_menu ? toolbar.bottom() : pointer_y);
  }
  const auto request_folder = [&](doc::NodeId parent) {
    // A name no sibling has, so the new folder is easy to find.
    std::string name = "New Folder";
    for (int suffix = 2;; ++suffix) {
      const bool taken = std::any_of(
          registry.folders.begin(), registry.folders.end(),
          [&](const assets::AssetFolder &folder) {
            return folder.parent == parent && folder.display_name == name;
          });
      if (!taken || suffix > 999) {
        break;
      }
      name = "New Folder " + std::to_string(suffix);
    }
    assets::LibraryEdit edit{};
    edit.kind = assets::LibraryEdit::Kind::CreateFolder;
    edit.destination = parent;
    edit.name = name;
    frame.library_edits.push_back(std::move(edit));
    pending_folder_name_ = std::move(name);
    pending_folder_parent_ = parent;
    known_folders_.clear();
    for (const assets::AssetFolder &folder : registry.folders) {
      known_folders_.insert(folder.id);
    }
    if (parent != doc::NodeId{}) {
      expanded_.insert(parent);
    }
  };
  if (create_folder) {
    request_folder(new_folder_parent);
  }
  if (menu_open_) {
    std::vector<std::string_view> labels(menu_entries_.begin(),
                                         menu_entries_.end());
    if (const auto chosen = ctx.context_menu(menu_id, labels)) {
      menu_open_ = false;
      const std::size_t raw =
          *chosen < menu_commands_.size() ? menu_commands_[*chosen] : 255U;
      const std::size_t toggle_base =
          static_cast<std::size_t>(MenuCommand::ToggleColumn);
      if (raw >= toggle_base && raw < toggle_base + library_column_count) {
        const auto column = static_cast<LibraryColumn>(raw - toggle_base);
        set_column_visible(column, !column_visible(column));
      } else {
        const doc::NodeId target = menu_target_;
        switch (static_cast<MenuCommand>(raw)) {
        case MenuCommand::View:
          if (registry.asset(target) != nullptr) {
            frame.view_asset = target;
          }
          break;
        case MenuCommand::Rename:
          begin_rename_of(target);
          break;
        case MenuCommand::NewFolder:
          request_folder(target != doc::NodeId{} ? new_folder_parent
                         : list                  ? doc::NodeId{}
                                                 : folder_);
          break;
        case MenuCommand::NewFolderInside:
          request_folder(target);
          break;
        case MenuCommand::MoveToTop: {
          assets::LibraryEdit edit{};
          edit.kind = assets::LibraryEdit::Kind::Move;
          edit.items =
              is_selected(target) ? selection_ : std::vector{target};
          frame.library_edits.push_back(std::move(edit));
          break;
        }
        case MenuCommand::Remove:
          remove_selection();
          break;
        case MenuCommand::OpenFolder:
          if (list) {
            if (!expanded_.erase(target)) {
              expanded_.insert(target);
            }
          } else {
            open_folder(target);
          }
          break;
        case MenuCommand::DeleteFolder: {
          assets::LibraryEdit edit{};
          edit.kind = assets::LibraryEdit::Kind::DeleteFolder;
          edit.target = target;
          frame.library_edits.push_back(std::move(edit));
          break;
        }
        case MenuCommand::ImportMedia:
        case MenuCommand::ImportModel:
          frame.import_asset = static_cast<MenuCommand>(raw) ==
                                       MenuCommand::ImportMedia
                                   ? LibraryImportAction::Media
                                   : LibraryImportAction::Model;
          frame.import_folder = list ? new_folder_parent
                                     : (generators_open_ ? doc::NodeId{}
                                                         : folder_);
          break;
        case MenuCommand::AcceptChanged:
          if (is_selected(target)) {
            for (const doc::NodeId id : selection_) {
              if (is_changed(id)) {
                frame.accept_sources.push_back(id);
              }
            }
          } else if (is_changed(target)) {
            frame.accept_sources.push_back(target);
          }
          break;
        case MenuCommand::AcceptAllChanged:
          frame.accept_sources = changed_shown;
          break;
        case MenuCommand::Rescan:
          frame.rescan_assets = true;
          break;
        case MenuCommand::ShowList:
          view_ = LibraryView::List;
          break;
        case MenuCommand::ShowGrid:
          view_ = LibraryView::Grid;
          break;
        case MenuCommand::ToggleColumn:
          break;
        }
      }
    } else if (!ctx.menu_open(menu_id)) {
      menu_open_ = false;
    }
  }
  rename_.end_frame(ctx);

  // ------------------------------------------------------------ thumbnails
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
  return frame;
}

} // namespace mediaserver::editor
