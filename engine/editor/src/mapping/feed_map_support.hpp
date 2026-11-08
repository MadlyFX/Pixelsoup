#pragma once

// Layout, naming and geometry helpers behind the Feed Mapping panel.
// Internal to the editor library.

#include <mediaserver/editor/mapping_panels.hpp>

#include "mapping_shared.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mediaserver::editor::mapping::feed {

inline constexpr float feed_list_header_height = 30.0F;
inline constexpr float feed_add_button_height = 22.0F;
inline constexpr float feed_add_button_step = 28.0F;
inline constexpr float feed_row_height = 34.0F;
inline constexpr float feed_row_step = 37.0F;
inline constexpr float feed_surface_row_height = 28.0F;
inline constexpr float feed_surface_row_step = 31.0F;
inline constexpr float feed_details_label_height = 16.0F;

// The colour bar down a row's left edge.
[[nodiscard]] gui::Rect row_stripe(gui::Rect row) noexcept;

// The rule left of a row's action button.
[[nodiscard]] gui::Rect action_rule(gui::Rect action, float inset) noexcept;

[[nodiscard]] float feed_list_width(const gui::Rect &bounds) noexcept;

[[nodiscard]] float feed_details_width(const gui::Rect &bounds) noexcept;

[[nodiscard]] gui::UiColor feed_color(const gui::Theme &theme,
                                      core::u32 index) noexcept;

[[nodiscard]] core::u32 feed_viewport_ordinal(
    const output::FeedMap &feed_map,
    const output::OutputMap &wanted) noexcept;

[[nodiscard]] std::string feed_viewport_name(
    core::u32 window_index, core::u32 viewport_ordinal);

[[nodiscard]] std::string feed_dimensions(scene::CanvasRect source);

// An unnamed viewport inside its window's frame, where the window is shown.
[[nodiscard]] std::string feed_viewport_short_name(core::u32 viewport_ordinal);

// The operator's name for a viewport, or its window and position there.
[[nodiscard]] std::string feed_viewport_title(const output::OutputMap &map,
                                              core::u32 viewport_ordinal);

[[nodiscard]] std::string feed_row_details(const output::OutputMap &map);

// Everything a row abbreviates, for the hover: where the picture comes from
// and where it lands.
[[nodiscard]] std::string feed_row_tooltip(const output::OutputMap &map,
                                           std::string_view title,
                                           const output::FeedMap &feed_map);

// `nodes` with `moved` taken out and put back just before or after `target`.
[[nodiscard]] std::vector<doc::NodeId>
reordered(const std::vector<doc::NodeId> &nodes, doc::NodeId moved,
          doc::NodeId target, bool after);

struct FeedWindowLayout final {
  core::u32 node_index{};
  scene::Canvas raster{};
  core::u32 offset_x{};
};

[[nodiscard]] scene::Canvas feed_window_raster(
    core::u32 feed_index, scene::Canvas fallback,
    const topology::HardwareMap *hardware) noexcept;

[[nodiscard]] std::vector<FeedWindowLayout> feed_window_layouts(
    const output::FeedMap &feed_map, scene::Canvas fallback,
    const topology::HardwareMap *hardware);

[[nodiscard]] const FeedWindowLayout *feed_window_layout_for(
    const std::vector<FeedWindowLayout> &layouts,
    core::u32 node_index) noexcept;

[[nodiscard]] std::string default_surface_name(std::size_t count);

[[nodiscard]] std::string default_mesh_surface_name(std::size_t count);

[[nodiscard]] std::string mesh_source_status(std::string_view source);

[[nodiscard]] output::OutputMap uv_preview_map(
    const output::OutputMap &selected,
    const output::ProjectionSurface &surface);

[[nodiscard]] output::PhysicalSurface
default_surface(scene::Canvas canvas) noexcept;

[[nodiscard]] output::PhysicalSurface
default_mesh_surface(scene::Canvas canvas) noexcept;

[[nodiscard]] const output::ProjectionSurface *
surface_or_null(const output::FeedMap &map,
                std::optional<doc::NodeId> node) noexcept;

[[nodiscard]] scene::CanvasRect move_feed_rect(scene::CanvasRect source,
                                               scene::Canvas canvas,
                                               core::i64 dx,
                                               core::i64 dy) noexcept;

[[nodiscard]] scene::CanvasRect resize_feed_rect(scene::CanvasRect source,
                                                 scene::Canvas canvas,
                                                 core::i64 dx,
                                                 core::i64 dy) noexcept;


} // namespace mediaserver::editor::mapping::feed
