#include "feed_map_support.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>

namespace mediaserver::editor::mapping::feed {

gui::Rect row_stripe(gui::Rect row) noexcept {
  return gui::inset(gui::cut_left(row, 5.0F),
                    gui::Padding{2.0F, 4.0F, 0.0F, 4.0F});
}

gui::Rect action_rule(gui::Rect action, float inset) noexcept {
  return gui::inset(gui::cut_left(action, 1.0F), 0.0F, inset);
}

[[nodiscard]] float feed_list_width(const gui::Rect &bounds) noexcept {
  if (bounds.width <= 0.0F) {
    return 0.0F;
  }
  // Wide enough at a full-size panel for a row's whole destination; the
  // narrow end is unchanged so a small panel keeps its work area.
  return std::min(std::clamp(bounds.width * 0.24F, 148.0F, 280.0F),
                  bounds.width * 0.46F);
}

[[nodiscard]] float feed_details_width(const gui::Rect &bounds) noexcept {
  if (bounds.width < 680.0F) {
    return 0.0F;
  }
  return std::min(std::clamp(bounds.width * 0.29F, 222.0F, 270.0F),
                  bounds.width * 0.34F);
}

[[nodiscard]] gui::UiColor feed_color(const gui::Theme &theme,
                                      core::u32 index) noexcept {
  constexpr std::array<gui::UiColor, 6U> colors{{
      {0.18F, 0.68F, 0.98F, 1.0F},
      {0.68F, 0.36F, 0.94F, 1.0F},
      {0.18F, 0.78F, 0.72F, 1.0F},
      {0.94F, 0.55F, 0.24F, 1.0F},
      {0.93F, 0.36F, 0.55F, 1.0F},
      {0.56F, 0.78F, 0.28F, 1.0F},
  }};
  static_cast<void>(theme);
  return colors[index % colors.size()];
}

[[nodiscard]] core::u32 feed_viewport_ordinal(
    const output::FeedMap &feed_map,
    const output::OutputMap &wanted) noexcept {
  core::u32 ordinal = 1U;
  for (const output::OutputMap &map : feed_map.maps_for(wanted.node_index)) {
    if (map.node == wanted.node) {
      return ordinal;
    }
    ++ordinal;
  }
  return ordinal;
}

[[nodiscard]] std::string feed_viewport_name(
    core::u32 window_index, core::u32 viewport_ordinal) {
  std::array<char, 48U> text{};
  const int length = std::snprintf(
      text.data(), text.size(), "WINDOW %02u  |  VIEWPORT %02u",
      window_index + 1U, viewport_ordinal);
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] std::string feed_dimensions(scene::CanvasRect source) {
  std::array<char, 48U> text{};
  const int length =
      std::snprintf(text.data(), text.size(), "%u,%u  %u x %u", source.x,
                    source.y, source.width, source.height);
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] std::string feed_viewport_short_name(core::u32 viewport_ordinal) {
  std::array<char, 24U> text{};
  const int length = std::snprintf(text.data(), text.size(), "VIEWPORT %02u",
                                   viewport_ordinal);
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] std::string feed_viewport_title(const output::OutputMap &map,
                                              core::u32 viewport_ordinal) {
  return map.name.empty() ? feed_viewport_name(map.node_index, viewport_ordinal)
                          : map.name;
}

[[nodiscard]] std::string feed_row_details(const output::OutputMap &map) {
  const std::string domain{
      map.projection == output::FeedProjection::PlanarSurface ? "PHYSICAL  "
                                                               : "CANVAS  "};
  return domain + feed_dimensions(map.viewport);
}

[[nodiscard]] std::string feed_row_tooltip(const output::OutputMap &map,
                                           std::string_view title,
                                           const output::FeedMap &feed_map) {
  std::string source;
  if (map.projection == output::FeedProjection::PlanarSurface) {
    const output::ProjectionSurface *const surface =
        feed_map.surface_for(map.surface);
    source = "surface " + (surface != nullptr ? surface->name
                                               : std::string{"(missing)"}) +
             " " + feed_dimensions(map.source);
  } else {
    source = "canvas " + feed_dimensions(map.source);
  }
  std::array<char, 24U> window{};
  const int length = std::snprintf(window.data(), window.size(), "window %02u",
                                   map.node_index + 1U);
  return std::string{title} + "  |  from " + source + "  |  to " +
         std::string{window.data(),
                     static_cast<std::size_t>(std::max(length, 0))} +
         " " + feed_dimensions(map.viewport);
}

[[nodiscard]] std::vector<doc::NodeId>
reordered(const std::vector<doc::NodeId> &nodes, doc::NodeId moved,
          doc::NodeId target, bool after) {
  std::vector<doc::NodeId> order;
  order.reserve(nodes.size());
  for (const doc::NodeId node : nodes) {
    if (node == moved) {
      continue;
    }
    if (node == target && !after) {
      order.push_back(moved);
    }
    order.push_back(node);
    if (node == target && after) {
      order.push_back(moved);
    }
  }
  return order;
}

[[nodiscard]] scene::Canvas feed_window_raster(
    core::u32 feed_index, scene::Canvas fallback,
    const topology::HardwareMap *hardware) noexcept {
  if (hardware == nullptr) {
    return fallback;
  }
  for (const topology::HardwareOutputBinding &binding :
       hardware->output_bindings) {
    if (binding.feed_index == feed_index && binding.width > 0U &&
        binding.height > 0U) {
      return scene::Canvas{binding.width, binding.height};
    }
  }
  for (const topology::HardwareDevice &device : hardware->devices) {
    if (device.kind == topology::HardwareDeviceKind::RenderServer &&
        device.node_index == feed_index && device.width > 0U &&
        device.height > 0U) {
      return scene::Canvas{device.width, device.height};
    }
  }
  return fallback;
}

[[nodiscard]] std::vector<FeedWindowLayout> feed_window_layouts(
    const output::FeedMap &feed_map, scene::Canvas fallback,
    const topology::HardwareMap *hardware) {
  std::vector<FeedWindowLayout> layouts;
  layouts.reserve(feed_map.maps.size());
  core::u32 offset = 0U;
  for (const output::OutputMap &map : feed_map.maps) {
    if (!layouts.empty() && layouts.back().node_index == map.node_index) {
      continue;
    }
    const scene::Canvas raster =
        feed_window_raster(map.node_index, fallback, hardware);
    layouts.push_back(FeedWindowLayout{map.node_index, raster, offset});
    if (hardware != nullptr) {
      const core::u64 next = static_cast<core::u64>(offset) + raster.width;
      offset =
          static_cast<core::u32>(std::min<core::u64>(next, UINT32_MAX));
    }
  }
  return layouts;
}

[[nodiscard]] const FeedWindowLayout *feed_window_layout_for(
    const std::vector<FeedWindowLayout> &layouts,
    core::u32 node_index) noexcept {
  const auto found = std::find_if(
      layouts.begin(), layouts.end(),
      [node_index](const FeedWindowLayout &layout) noexcept {
        return layout.node_index == node_index;
      });
  return found == layouts.end() ? nullptr : &*found;
}

[[nodiscard]] std::string default_surface_name(std::size_t count) {
  std::array<char, 48U> text{};
  const int length = std::snprintf(text.data(), text.size(), "LED Surface %u",
                                   static_cast<unsigned>(count + 1U));
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] std::string default_mesh_surface_name(std::size_t count) {
  std::array<char, 48U> text{};
  const int length = std::snprintf(text.data(), text.size(), "UV Mesh %u",
                                   static_cast<unsigned>(count + 1U));
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] std::string mesh_source_status(std::string_view source) {
  if (source.empty()) {
    return "NO UV MESH SELECTED";
  }
  const std::filesystem::path path{source};
  const std::string filename = path.filename().string();
  return filename.empty() ? std::string{source} : filename;
}

[[nodiscard]] output::OutputMap uv_preview_map(
    const output::OutputMap &selected,
    const output::ProjectionSurface &surface) {
  output::OutputMap map = selected;
  map.projection = output::FeedProjection::PlanarSurface;
  map.surface = surface.node;
  map.source = scene::whole(surface.geometry.raster);

  output::PhysicalSurface canonical = surface.geometry;
  canonical.center = scene::Vec3{};
  canonical.right = scene::Vec3{1.0F, 0.0F, 0.0F};
  canonical.up = scene::Vec3{0.0F, 1.0F, 0.0F};
  map.resolved_surface = output::ResolvedProjectionSurface{
      canonical,
      output::ProjectionSurfaceShape::ImportedMesh,
      surface.horizontal_arc_turns,
      surface.mesh_columns,
      surface.mesh_source,
      // Destination UVs are invariant under stage-space scale. Keeping that
      // out of this preview-only key avoids re-reading a file during a live
      // calibration scrub.
      1.0F,
      surface.mesh_asset};
  return map;
}

[[nodiscard]] output::PhysicalSurface
default_surface(scene::Canvas canvas) noexcept {
  output::PhysicalSurface surface{};
  surface.center = scene::Vec3{0.0F, 0.0F, -4.0F};
  surface.right = scene::Vec3{1.0F, 0.0F, 0.0F};
  surface.up = scene::Vec3{0.0F, 1.0F, 0.0F};
  surface.width = 8.0F;
  surface.height = canvas.width == 0U
                       ? 4.5F
                       : surface.width * static_cast<float>(canvas.height) /
                             static_cast<float>(canvas.width);
  surface.raster = canvas;
  return surface;
}

[[nodiscard]] output::PhysicalSurface
default_mesh_surface(scene::Canvas canvas) noexcept {
  output::PhysicalSurface surface = default_surface(canvas);
  // External projection pieces normally retain one shared modelling origin.
  // Starting at zero preserves those authored coordinates; operators can move
  // every piece together with the explicit origin/yaw/scale calibration.
  surface.center = scene::Vec3{};
  return surface;
}

[[nodiscard]] const output::ProjectionSurface *
surface_or_null(const output::FeedMap &map,
                std::optional<doc::NodeId> node) noexcept {
  return node.has_value() ? map.surface_for(*node) : nullptr;
}

[[nodiscard]] scene::CanvasRect move_feed_rect(scene::CanvasRect source,
                                               scene::Canvas canvas,
                                               core::i64 dx,
                                               core::i64 dy) noexcept {
  const core::u32 width = std::min(source.width, canvas.width);
  const core::u32 height = std::min(source.height, canvas.height);
  const core::i64 maximum_x = static_cast<core::i64>(canvas.width - width);
  const core::i64 maximum_y = static_cast<core::i64>(canvas.height - height);
  const core::i64 moved_x = std::clamp(static_cast<core::i64>(source.x) + dx,
                                       core::i64{0}, maximum_x);
  const core::i64 moved_y = std::clamp(static_cast<core::i64>(source.y) + dy,
                                       core::i64{0}, maximum_y);
  return scene::CanvasRect{static_cast<core::u32>(moved_x),
                           static_cast<core::u32>(moved_y), width, height};
}

[[nodiscard]] scene::CanvasRect resize_feed_rect(scene::CanvasRect source,
                                                 scene::Canvas canvas,
                                                 core::i64 dx,
                                                 core::i64 dy) noexcept {
  const core::u32 x = std::min(source.x, canvas.width - 1U);
  const core::u32 y = std::min(source.y, canvas.height - 1U);
  const core::i64 maximum_width = static_cast<core::i64>(canvas.width - x);
  const core::i64 maximum_height = static_cast<core::i64>(canvas.height - y);
  const core::i64 width = std::clamp(static_cast<core::i64>(source.width) + dx,
                                     core::i64{1}, maximum_width);
  const core::i64 height = std::clamp(
      static_cast<core::i64>(source.height) + dy, core::i64{1}, maximum_height);
  return scene::CanvasRect{x, y, static_cast<core::u32>(width),
                           static_cast<core::u32>(height)};
}


} // namespace mediaserver::editor::mapping::feed
