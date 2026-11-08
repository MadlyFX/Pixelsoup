#include "viewport_mask_support.hpp"

#include <mediaserver/editor/gizmo.hpp>
#include <mediaserver/editor/light_guides.hpp>
#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/rect_cut.hpp>
#include <mediaserver/gui/shapes.hpp>

#include <mediaserver/scene/manipulate.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <tuple>
#include <utility>

namespace mediaserver::editor {

namespace {

// The projective map taking the unit square onto the source quad: canvas
// (x, y) = (r0.p, r1.p) / (r2.p) for p = (u, v, 1). A planar surface seen
// through a pinhole camera lands on screen exactly this way, and it is how the
// renderer traces a pixel back onto the layer, so the outline drawn here and
// the coverage rendered there follow the same surface. Interpolating the four
// corners bilinearly instead bends straight mask edges and drifts away from
// the rendered mask whenever the layer is seen in perspective.
using QuadProjective = std::array<double, 9U>;

[[nodiscard]] std::optional<QuadProjective> square_to_quad(
    const MaskViewportSourceMapping& mapping) noexcept {
    const auto& q = mapping.corners;
    for (const MaskViewportCanvasPoint& corner : q) {
        if (!std::isfinite(corner.x) || !std::isfinite(corner.y)) {
            return std::nullopt;
        }
    }
    const float area_twice =
        q[0U].x * q[1U].y - q[1U].x * q[0U].y +
        q[1U].x * q[2U].y - q[2U].x * q[1U].y +
        q[2U].x * q[3U].y - q[3U].x * q[2U].y +
        q[3U].x * q[0U].y - q[0U].x * q[3U].y;
    if (!std::isfinite(area_twice) || std::abs(area_twice) < 1.0e-4F) {
        return std::nullopt;
    }
    const double x0 = q[0U].x, y0 = q[0U].y, x1 = q[1U].x, y1 = q[1U].y;
    const double x2 = q[2U].x, y2 = q[2U].y, x3 = q[3U].x, y3 = q[3U].y;
    const double sx = x0 - x1 + x2 - x3;
    const double sy = y0 - y1 + y2 - y3;
    const double dx1 = x1 - x2, dx2 = x3 - x2;
    const double dy1 = y1 - y2, dy2 = y3 - y2;
    const double denominator = dx1 * dy2 - dx2 * dy1;
    if (!std::isfinite(denominator) || std::abs(denominator) < 1.0e-12) {
        return std::nullopt;
    }
    const double g = (sx * dy2 - dx2 * sy) / denominator;
    const double h = (dx1 * sy - sx * dy1) / denominator;
    const QuadProjective result{
        x1 - x0 + g * x1, x3 - x0 + h * x3, x0,
        y1 - y0 + g * y1, y3 - y0 + h * y3, y0,
        g, h, 1.0};
    for (const double value : result) {
        if (!std::isfinite(value)) {
            return std::nullopt;
        }
    }
    return result;
}

} // namespace

std::optional<gui::UiPoint> mask_normalized_to_viewport(
    const gui::Rect& image, scene::Canvas canvas,
    scene::MaskCoordinateSpace coordinate_space,
    const MaskViewportSourceMapping* source_mapping, float x,
    float y) noexcept {
    if (image.width <= 0.0F || image.height <= 0.0F ||
        canvas.width == 0U || canvas.height == 0U || !std::isfinite(x) ||
        !std::isfinite(y)) {
        return std::nullopt;
    }
    float canvas_x{};
    float canvas_y{};
    if (coordinate_space == scene::MaskCoordinateSpace::AuthoredCanvas) {
        canvas_x = x * static_cast<float>(canvas.width);
        canvas_y = y * static_cast<float>(canvas.height);
    } else {
        if (source_mapping == nullptr) {
            return std::nullopt;
        }
        const auto map = square_to_quad(*source_mapping);
        if (!map.has_value()) {
            return std::nullopt;
        }
        const auto& m = *map;
        const double u = x;
        const double v = y;
        const double depth = m[6U] * u + m[7U] * v + m[8U];
        // Beyond the plane's horizon: no point on the surface lands there.
        if (!(depth > 1.0e-9)) {
            return std::nullopt;
        }
        canvas_x = static_cast<float>((m[0U] * u + m[1U] * v + m[2U]) / depth);
        canvas_y = static_cast<float>((m[3U] * u + m[4U] * v + m[5U]) / depth);
        if (!std::isfinite(canvas_x) || !std::isfinite(canvas_y)) {
            return std::nullopt;
        }
    }
    return canvas_to_image(image, canvas, canvas_x, canvas_y);
}

std::optional<MaskViewportNormalizedPoint> mask_viewport_to_normalized(
    const gui::Rect& image, scene::Canvas canvas,
    scene::MaskCoordinateSpace coordinate_space,
    const MaskViewportSourceMapping* source_mapping, float pointer_x,
    float pointer_y) noexcept {
    if (image.width <= 0.0F || image.height <= 0.0F ||
        canvas.width == 0U || canvas.height == 0U ||
        !std::isfinite(pointer_x) || !std::isfinite(pointer_y)) {
        return std::nullopt;
    }
    const float canvas_x =
        (pointer_x - image.x) * static_cast<float>(canvas.width) / image.width;
    const float canvas_y =
        (pointer_y - image.y) * static_cast<float>(canvas.height) /
        image.height;
    if (coordinate_space == scene::MaskCoordinateSpace::AuthoredCanvas) {
        return MaskViewportNormalizedPoint{
            canvas_x / static_cast<float>(canvas.width),
            canvas_y / static_cast<float>(canvas.height)};
    }
    if (source_mapping == nullptr) {
        return std::nullopt;
    }
    const auto map = square_to_quad(*source_mapping);
    if (!map.has_value()) {
        return std::nullopt;
    }
    // The adjugate inverts the map up to scale, and scale cancels in the
    // division. Which side of the horizon the pointer is on is checked by
    // mapping the answer forward again.
    const auto& m = *map;
    const QuadProjective inverse{
        m[4U] * m[8U] - m[5U] * m[7U], m[2U] * m[7U] - m[1U] * m[8U],
        m[1U] * m[5U] - m[2U] * m[4U], m[5U] * m[6U] - m[3U] * m[8U],
        m[0U] * m[8U] - m[2U] * m[6U], m[2U] * m[3U] - m[0U] * m[5U],
        m[3U] * m[7U] - m[4U] * m[6U], m[1U] * m[6U] - m[0U] * m[7U],
        m[0U] * m[4U] - m[1U] * m[3U]};
    const double px = canvas_x;
    const double py = canvas_y;
    const double w = inverse[6U] * px + inverse[7U] * py + inverse[8U];
    if (!std::isfinite(w) || std::abs(w) < 1.0e-12) {
        return std::nullopt;
    }
    const double u = (inverse[0U] * px + inverse[1U] * py + inverse[2U]) / w;
    const double v = (inverse[3U] * px + inverse[4U] * py + inverse[5U]) / w;
    if (!std::isfinite(u) || !std::isfinite(v) ||
        !(m[6U] * u + m[7U] * v + m[8U] > 1.0e-9)) {
        return std::nullopt;
    }
    return MaskViewportNormalizedPoint{static_cast<float>(u),
                                       static_cast<float>(v)};
}

} // namespace mediaserver::editor

namespace mediaserver::editor::viewport {

[[nodiscard]] MaskViewportNormalizedPoint mask_coordinate_extent(
    scene::Canvas canvas, const MaskViewportInput& input) noexcept {
    if (input.target.coordinate_space ==
            scene::MaskCoordinateSpace::AuthoredCanvas ||
        !input.source_layer_mapping.has_value()) {
        return {static_cast<float>(canvas.width),
                static_cast<float>(canvas.height)};
    }
    // The renderer's unit for this scene, not the corners' spacing: a mask
    // measured against Program Output's crop or warp would rotate and
    // feather differently from the one rendered.
    const scene::MaskCoordinateExtent& extent =
        input.source_layer_mapping->extent;
    return {std::max(extent.width, 1.0F), std::max(extent.height, 1.0F)};
}

[[nodiscard]] MaskViewportNormalizedPoint rotate_shape_point(
    MaskViewportNormalizedPoint point, const scene::MaskBounds& bounds,
    float turns, MaskViewportNormalizedPoint extent) noexcept {
    constexpr float tau = 6.28318530717958647692F;
    const float centre_x = (bounds.left + bounds.right) * 0.5F;
    const float centre_y = (bounds.top + bounds.bottom) * 0.5F;
    const float dx = (point.x - centre_x) * extent.x;
    const float dy = (point.y - centre_y) * extent.y;
    const float angle = turns * tau;
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    return {centre_x + (cosine * dx - sine * dy) / extent.x,
            centre_y + (sine * dx + cosine * dy) / extent.y};
}

[[nodiscard]] MaskViewportNormalizedPoint unrotate_shape_point(
    MaskViewportNormalizedPoint point, const scene::MaskBounds& bounds,
    float turns, MaskViewportNormalizedPoint extent) noexcept {
    return rotate_shape_point(point, bounds, -turns, extent);
}

[[nodiscard]] const MaskViewportSourceMapping* source_mapping(
    const MaskViewportInput& input) noexcept {
    return input.source_layer_mapping.has_value()
        ? &*input.source_layer_mapping
        : nullptr;
}

[[nodiscard]] std::optional<gui::UiPoint> mask_screen_point(
    const gui::Rect& image, scene::Canvas canvas,
    const MaskViewportInput& input,
    MaskViewportNormalizedPoint point) noexcept {
    return mask_normalized_to_viewport(
        image, canvas, input.target.coordinate_space, source_mapping(input),
        point.x, point.y);
}

[[nodiscard]] std::optional<MaskViewportNormalizedPoint> mask_normalized_point(
    const gui::Rect& image, scene::Canvas canvas,
    const MaskViewportInput& input, float x, float y) noexcept {
    return mask_viewport_to_normalized(
        image, canvas, input.target.coordinate_space, source_mapping(input), x,
        y);
}

namespace {

[[nodiscard]] MaskViewportNormalizedPoint cubic(
    MaskViewportNormalizedPoint p0, MaskViewportNormalizedPoint p1,
    MaskViewportNormalizedPoint p2, MaskViewportNormalizedPoint p3,
    float t) noexcept {
    const float one_minus = 1.0F - t;
    const float a = one_minus * one_minus * one_minus;
    const float b = 3.0F * one_minus * one_minus * t;
    const float c = 3.0F * one_minus * t * t;
    const float d = t * t * t;
    return MaskViewportNormalizedPoint{
        a * p0.x + b * p1.x + c * p2.x + d * p3.x,
        a * p0.y + b * p1.y + c * p2.y + d * p3.y};
}

} // namespace

[[nodiscard]] MaskPath build_mask_path(
    const scene::MaskInstance& mask, const gui::Rect& image,
    scene::Canvas canvas, const MaskViewportInput& input) {
    MaskPath path{};
    const MaskViewportNormalizedPoint coordinate_extent =
        mask_coordinate_extent(canvas, input);
    const auto append = [&](MaskViewportNormalizedPoint normalized,
                            std::size_t insertion_index) {
        if (mask.kind != scene::MaskKind::Bezier &&
            mask.rotation_turns != 0.0F) {
            normalized = rotate_shape_point(
                normalized, mask.bounds, mask.rotation_turns,
                coordinate_extent);
        }
        const auto screen = mask_screen_point(image, canvas, input, normalized);
        if (screen.has_value()) {
            path.vertices.push_back(
                MaskPathVertex{normalized, *screen, insertion_index});
        }
    };
    switch (mask.kind) {
    case scene::MaskKind::Rectangle:
        append({mask.bounds.left, mask.bounds.top}, 0U);
        append({mask.bounds.right, mask.bounds.top}, 0U);
        append({mask.bounds.right, mask.bounds.bottom}, 0U);
        append({mask.bounds.left, mask.bounds.bottom}, 0U);
        append({mask.bounds.left, mask.bounds.top}, 0U);
        break;
    case scene::MaskKind::Ellipse: {
        constexpr float tau = 6.28318530717958647692F;
        const float centre_x = (mask.bounds.left + mask.bounds.right) * 0.5F;
        const float centre_y = (mask.bounds.top + mask.bounds.bottom) * 0.5F;
        const float radius_x = (mask.bounds.right - mask.bounds.left) * 0.5F;
        const float radius_y = (mask.bounds.bottom - mask.bounds.top) * 0.5F;
        for (std::size_t index = 0U; index <= mask_ellipse_steps; ++index) {
            const float angle = tau * static_cast<float>(index) /
                                static_cast<float>(mask_ellipse_steps);
            append({centre_x + std::cos(angle) * radius_x,
                    centre_y + std::sin(angle) * radius_y},
                   0U);
        }
        break;
    }
    case scene::MaskKind::Bezier:
        if (mask.points.size() < 2U) {
            break;
        }
        for (std::size_t segment = 0U; segment < mask.points.size();
             ++segment) {
            const scene::MaskPoint& first = mask.points[segment];
            const scene::MaskPoint& second =
                mask.points[(segment + 1U) % mask.points.size()];
            const MaskViewportNormalizedPoint p0{first.x, first.y};
            const MaskViewportNormalizedPoint p1{first.out_x, first.out_y};
            const MaskViewportNormalizedPoint p2{second.in_x, second.in_y};
            const MaskViewportNormalizedPoint p3{second.x, second.y};
            const std::size_t begin = segment == 0U ? 0U : 1U;
            for (std::size_t step = begin; step <= mask_curve_steps; ++step) {
                append(cubic(p0, p1, p2, p3,
                             static_cast<float>(step) /
                                 static_cast<float>(mask_curve_steps)),
                       segment + 1U);
            }
        }
        break;
    }
    return path;
}

void draw_mask_path(gui::UiDrawList& list, const MaskPath& path,
                    float thickness, gui::UiColor color) {
    if (path.vertices.size() < 2U) {
        return;
    }
    for (std::size_t index = 1U; index < path.vertices.size(); ++index) {
        line(list, path.vertices[index - 1U].screen,
             path.vertices[index].screen, thickness, color);
    }
}

[[nodiscard]] float point_distance(gui::UiPoint a, float x,
                                   float y) noexcept {
    const float dx = a.x - x;
    const float dy = a.y - y;
    return std::sqrt(dx * dx + dy * dy);
}

[[nodiscard]] MaskPathHit closest_path_point(const MaskPath& path, float x,
                                             float y) noexcept {
    MaskPathHit closest{};
    for (std::size_t index = 1U; index < path.vertices.size(); ++index) {
        const MaskPathVertex& a = path.vertices[index - 1U];
        const MaskPathVertex& b = path.vertices[index];
        const float dx = b.screen.x - a.screen.x;
        const float dy = b.screen.y - a.screen.y;
        const float squared = dx * dx + dy * dy;
        const float t = squared > 0.0F
            ? std::clamp(((x - a.screen.x) * dx + (y - a.screen.y) * dy) /
                             squared,
                         0.0F, 1.0F)
            : 0.0F;
        const float px = a.screen.x + dx * t;
        const float py = a.screen.y + dy * t;
        const float distance = std::sqrt((x - px) * (x - px) +
                                         (y - py) * (y - py));
        if (distance < closest.distance) {
            closest.distance = distance;
            closest.normalized = MaskViewportNormalizedPoint{
                a.normalized.x + (b.normalized.x - a.normalized.x) * t,
                a.normalized.y + (b.normalized.y - a.normalized.y) * t};
            closest.insertion_index = b.insertion_index;
        }
    }
    return closest;
}

[[nodiscard]] bool inside_mask(const scene::MaskInstance& mask,
                               MaskViewportNormalizedPoint point,
                               scene::Canvas canvas,
                               const MaskViewportInput& input) noexcept {
    if (mask.kind != scene::MaskKind::Bezier &&
        mask.rotation_turns != 0.0F) {
        point = unrotate_shape_point(
            point, mask.bounds, mask.rotation_turns,
            mask_coordinate_extent(canvas, input));
    }
    switch (mask.kind) {
    case scene::MaskKind::Rectangle:
        return point.x >= mask.bounds.left && point.x <= mask.bounds.right &&
               point.y >= mask.bounds.top && point.y <= mask.bounds.bottom;
    case scene::MaskKind::Ellipse: {
        const float radius_x = (mask.bounds.right - mask.bounds.left) * 0.5F;
        const float radius_y = (mask.bounds.bottom - mask.bounds.top) * 0.5F;
        if (radius_x <= 0.0F || radius_y <= 0.0F) {
            return false;
        }
        const float centre_x = (mask.bounds.left + mask.bounds.right) * 0.5F;
        const float centre_y = (mask.bounds.top + mask.bounds.bottom) * 0.5F;
        const float x = (point.x - centre_x) / radius_x;
        const float y = (point.y - centre_y) / radius_y;
        return x * x + y * y <= 1.0F;
    }
    case scene::MaskKind::Bezier: {
        if (mask.points.size() < 3U) {
            return false;
        }
        bool inside{};
        std::size_t previous = mask.points.size() - 1U;
        for (std::size_t index = 0U; index < mask.points.size(); ++index) {
            const scene::MaskPoint& a = mask.points[index];
            const scene::MaskPoint& b = mask.points[previous];
            const bool crosses = (a.y > point.y) != (b.y > point.y);
            if (crosses) {
                const float intersect =
                    (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x;
                if (point.x < intersect) {
                    inside = !inside;
                }
            }
            previous = index;
        }
        return inside;
    }
    }
    return false;
}

[[nodiscard]] const scene::MaskPoint* mask_point(
    const scene::MaskInstance& mask, doc::NodeId id) noexcept {
    const auto found = std::find_if(
        mask.points.begin(), mask.points.end(),
        [id](const scene::MaskPoint& point) noexcept {
            return point.source == id;
        });
    return found == mask.points.end() ? nullptr : &*found;
}

[[nodiscard]] scene::MaskPoint* mask_point(scene::MaskInstance& mask,
                                           doc::NodeId id) noexcept {
    const auto found = std::find_if(
        mask.points.begin(), mask.points.end(),
        [id](const scene::MaskPoint& point) noexcept {
            return point.source == id;
        });
    return found == mask.points.end() ? nullptr : &*found;
}

[[nodiscard]] std::pair<float, float> clamp_translation(
    const scene::MaskInstance& mask, float dx, float dy,
    std::optional<doc::NodeId> only_point) noexcept {
    float minimum_x = 1.0F;
    float minimum_y = 1.0F;
    float maximum_x = 0.0F;
    float maximum_y = 0.0F;
    const auto include = [&](float x, float y) {
        minimum_x = std::min(minimum_x, x);
        minimum_y = std::min(minimum_y, y);
        maximum_x = std::max(maximum_x, x);
        maximum_y = std::max(maximum_y, y);
    };
    if (mask.kind != scene::MaskKind::Bezier) {
        include(mask.bounds.left, mask.bounds.top);
        include(mask.bounds.right, mask.bounds.bottom);
    } else {
        for (const scene::MaskPoint& point : mask.points) {
            if (only_point.has_value() && point.source != *only_point) {
                continue;
            }
            include(point.x, point.y);
            include(point.in_x, point.in_y);
            include(point.out_x, point.out_y);
        }
    }
    dx = std::clamp(dx, -minimum_x, 1.0F - maximum_x);
    dy = std::clamp(dy, -minimum_y, 1.0F - maximum_y);
    return {dx, dy};
}

void translate_mask(scene::MaskInstance& mask, float dx, float dy,
                    std::optional<doc::NodeId> only_point) {
    if (mask.kind != scene::MaskKind::Bezier) {
        mask.bounds.left += dx;
        mask.bounds.right += dx;
        mask.bounds.top += dy;
        mask.bounds.bottom += dy;
        return;
    }
    for (scene::MaskPoint& point : mask.points) {
        if (only_point.has_value() && point.source != *only_point) {
            continue;
        }
        point.x += dx;
        point.y += dy;
        point.in_x += dx;
        point.in_y += dy;
        point.out_x += dx;
        point.out_y += dy;
    }
}

[[nodiscard]] std::pair<float, float> constrained_delta(float dx, float dy,
                                                        bool shift) noexcept {
    if (!shift) {
        return {dx, dy};
    }
    if (std::abs(dx) >= std::abs(dy)) {
        return {dx, 0.0F};
    }
    return {0.0F, dy};
}

[[nodiscard]] MaskViewportNormalizedPoint constrained_handle(
    MaskViewportNormalizedPoint anchor, MaskViewportNormalizedPoint value,
    bool shift) noexcept {
    if (shift) {
        constexpr float eighth_turn = 0.78539816339744830962F;
        const float dx = value.x - anchor.x;
        const float dy = value.y - anchor.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length > 0.0F) {
            const float angle = std::round(std::atan2(dy, dx) / eighth_turn) *
                                eighth_turn;
            value.x = anchor.x + std::cos(angle) * length;
            value.y = anchor.y + std::sin(angle) * length;
        }
    }
    value.x = std::clamp(value.x, 0.0F, 1.0F);
    value.y = std::clamp(value.y, 0.0F, 1.0F);
    return value;
}

[[nodiscard]] scene::MaskPointPatch point_patch(
    const scene::MaskPoint& before, const scene::MaskPoint& after) {
    scene::MaskPointPatch patch{};
    if (before.x != after.x) {
        patch.x = after.x;
    }
    if (before.y != after.y) {
        patch.y = after.y;
    }
    if (before.in_x != after.in_x) {
        patch.in_x = after.in_x;
    }
    if (before.in_y != after.in_y) {
        patch.in_y = after.in_y;
    }
    if (before.out_x != after.out_x) {
        patch.out_x = after.out_x;
    }
    if (before.out_y != after.out_y) {
        patch.out_y = after.out_y;
    }
    return patch;
}

[[nodiscard]] bool empty_patch(const scene::MaskPointPatch& patch) noexcept {
    return !patch.x.has_value() && !patch.y.has_value() &&
           !patch.in_x.has_value() && !patch.in_y.has_value() &&
           !patch.out_x.has_value() && !patch.out_y.has_value();
}

void draw_mask_point(gui::UiDrawList& list, gui::UiPoint point,
                     gui::UiColor color, float size) {
    square(list, point, size, color);
}

} // namespace mediaserver::editor::viewport
