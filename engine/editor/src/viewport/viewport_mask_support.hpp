#pragma once

// Geometry and drawing helpers for the viewport's mask overlay: hit radii,
// mask paths in screen space, and the edits a gesture turns into. Internal
// to the editor library.

#include "viewport_support.hpp"

#include <mediaserver/editor/masks_panel.hpp>

#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace mediaserver::editor::viewport {

inline constexpr float mask_point_hit_pixels = 8.0F;
inline constexpr float mask_handle_hit_pixels = 7.0F;
inline constexpr float mask_shape_handle_hit_pixels = 10.0F;
inline constexpr float mask_path_hit_pixels = 7.0F;
inline constexpr float mask_minimum_drag = 1.0e-5F;
inline constexpr std::size_t mask_curve_steps = 16U;
inline constexpr std::size_t mask_ellipse_steps = 64U;
inline constexpr gui::UiColor mask_outline{0.20F, 0.92F, 1.0F, 1.0F};
inline constexpr gui::UiColor mask_secondary{0.24F, 0.70F, 0.88F, 0.78F};
inline constexpr gui::UiColor mask_handle{0.96F, 0.82F, 0.28F, 1.0F};
inline constexpr gui::UiColor mask_feather{0.26F, 0.68F, 1.0F, 0.18F};
inline constexpr gui::UiColor mask_expansion{0.96F, 0.56F, 0.20F, 0.26F};

struct MaskPathVertex final {
    MaskViewportNormalizedPoint normalized{};
    gui::UiPoint screen{};
    std::size_t insertion_index{};
};

struct MaskPath final {
    std::vector<MaskPathVertex> vertices;
};

struct MaskPathHit final {
    float distance{std::numeric_limits<float>::infinity()};
    MaskViewportNormalizedPoint normalized{};
    std::size_t insertion_index{};
};

struct MaskPick final {
    MaskViewportHit hit{MaskViewportHit::None};
    const scene::MaskInstance* mask{};
    std::optional<doc::NodeId> point;
    std::size_t insertion_index{};
    scene::MaskPointGeometry insertion_geometry{};
};

[[nodiscard]] MaskViewportNormalizedPoint mask_coordinate_extent(
    scene::Canvas canvas, const MaskViewportInput& input) noexcept;

[[nodiscard]] MaskViewportNormalizedPoint rotate_shape_point(
    MaskViewportNormalizedPoint point, const scene::MaskBounds& bounds,
    float turns, MaskViewportNormalizedPoint extent) noexcept;

[[nodiscard]] MaskViewportNormalizedPoint unrotate_shape_point(
    MaskViewportNormalizedPoint point, const scene::MaskBounds& bounds,
    float turns, MaskViewportNormalizedPoint extent) noexcept;

[[nodiscard]] const MaskViewportSourceMapping* source_mapping(
    const MaskViewportInput& input) noexcept;

[[nodiscard]] std::optional<gui::UiPoint> mask_screen_point(
    const gui::Rect& image, scene::Canvas canvas,
    const MaskViewportInput& input,
    MaskViewportNormalizedPoint point) noexcept;

[[nodiscard]] std::optional<MaskViewportNormalizedPoint> mask_normalized_point(
    const gui::Rect& image, scene::Canvas canvas,
    const MaskViewportInput& input, float x, float y) noexcept;

[[nodiscard]] MaskPath build_mask_path(
    const scene::MaskInstance& mask, const gui::Rect& image,
    scene::Canvas canvas, const MaskViewportInput& input);

void draw_mask_path(gui::UiDrawList& list, const MaskPath& path,
                    float thickness, gui::UiColor color);

[[nodiscard]] float point_distance(gui::UiPoint a, float x,
                                   float y) noexcept;

[[nodiscard]] MaskPathHit closest_path_point(const MaskPath& path, float x,
                                             float y) noexcept;

[[nodiscard]] bool inside_mask(const scene::MaskInstance& mask,
                               MaskViewportNormalizedPoint point,
                               scene::Canvas canvas,
                               const MaskViewportInput& input) noexcept;

[[nodiscard]] const scene::MaskPoint* mask_point(
    const scene::MaskInstance& mask, doc::NodeId id) noexcept;

[[nodiscard]] scene::MaskPoint* mask_point(scene::MaskInstance& mask,
                                           doc::NodeId id) noexcept;

[[nodiscard]] std::pair<float, float> clamp_translation(
    const scene::MaskInstance& mask, float dx, float dy,
    std::optional<doc::NodeId> only_point = std::nullopt) noexcept;

void translate_mask(scene::MaskInstance& mask, float dx, float dy,
                    std::optional<doc::NodeId> only_point = std::nullopt);

[[nodiscard]] std::pair<float, float> constrained_delta(float dx, float dy,
                                                        bool shift) noexcept;

[[nodiscard]] MaskViewportNormalizedPoint constrained_handle(
    MaskViewportNormalizedPoint anchor, MaskViewportNormalizedPoint value,
    bool shift) noexcept;

[[nodiscard]] scene::MaskPointPatch point_patch(
    const scene::MaskPoint& before, const scene::MaskPoint& after);

[[nodiscard]] bool empty_patch(const scene::MaskPointPatch& patch) noexcept;

void draw_mask_point(gui::UiDrawList& list, gui::UiPoint point,
                     gui::UiColor color, float size = 7.0F);

} // namespace mediaserver::editor::viewport
