#include <mediaserver/editor/gizmo.hpp>

#include <mediaserver/gui/rect_cut.hpp>
#include <mediaserver/gui/shapes.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace mediaserver::editor {
namespace {

void line(gui::UiDrawList& list, gui::UiPoint a, gui::UiPoint b,
          float thickness, gui::UiColor color) {
    list.add_line(a.x, a.y, b.x, b.y, thickness, color);
}

// The three axis hues, in the order the X/Y/Z members of GizmoAxis run.
[[nodiscard]] std::array<gui::UiColor, 3U> axis_colors(
    const gui::Theme& theme) noexcept {
    return {theme.gizmo_axis_x, theme.gizmo_axis_y, theme.gizmo_axis_z};
}

[[nodiscard]] GizmoAxis axis_at(std::size_t index) noexcept {
    return static_cast<GizmoAxis>(static_cast<core::u8>(GizmoAxis::X) +
                                  static_cast<core::u8>(index));
}

}  // namespace

std::size_t axis_index(GizmoAxis axis) noexcept {
    return static_cast<std::size_t>(static_cast<core::u8>(axis) -
                                    static_cast<core::u8>(GizmoAxis::X));
}

gui::UiPoint canvas_to_image(const gui::Rect& image, scene::Canvas canvas,
                             float x, float y) noexcept {
    return gui::UiPoint{
        image.x + x * image.width / static_cast<float>(canvas.width),
        image.y + y * image.height / static_cast<float>(canvas.height)};
}

gui::UiPoint to_ui(const gui::Rect& image, scene::Canvas canvas,
                   const scene::CanvasProjection& point) noexcept {
    return canvas_to_image(image, canvas, point.x, point.y);
}

void square(gui::UiDrawList& list, gui::UiPoint centre, float size,
            gui::UiColor color) {
    const gui::Rect box = gui::around(centre.x, centre.y, size, size);
    list.add_rect(box.x, box.y, box.width, box.height, color);
}

void haloed_line(gui::UiDrawList& list, gui::UiPoint a, gui::UiPoint b,
                 float halo, float thickness, gui::UiColor color) {
    line(list, a, b, halo, overlay_halo);
    line(list, a, b, thickness, color);
}

void haloed_square(gui::UiDrawList& list, gui::UiPoint centre, float size,
                   float border, gui::UiColor color) {
    square(list, centre, size + border * 2.0F, overlay_halo);
    square(list, centre, size, color);
}

void haloed_ring(gui::UiDrawList& list, gui::UiPoint centre, float radius,
                 std::size_t segments, float halo, float thickness,
                 gui::UiColor color) {
    gui::UiPoint previous{centre.x + radius, centre.y};
    for (std::size_t index = 1U; index <= segments; ++index) {
        const float turns =
            static_cast<float>(index) / static_cast<float>(segments);
        const gui::UiPoint next{
            centre.x + scene::deterministic::cos_turns(turns) * radius,
            centre.y + scene::deterministic::sin_turns(turns) * radius};
        haloed_line(list, previous, next, halo, thickness, color);
        previous = next;
    }
}

void projected_line(gui::UiDrawList& list, const gui::Rect& image,
                    const scene::Scene& view, scene::Vec3 a, scene::Vec3 b,
                    float thickness, gui::UiColor color) {
    const auto projected_a =
        scene::project_to_canvas(view.camera, view.canvas, a);
    const auto projected_b =
        scene::project_to_canvas(view.camera, view.canvas, b);
    if (!projected_a.visible || !projected_b.visible) {
        return;
    }
    line(list, to_ui(image, view.canvas, projected_a),
         to_ui(image, view.canvas, projected_b), thickness, color);
}

void haloed_projected_line(gui::UiDrawList& list, const gui::Rect& image,
                           const scene::Scene& view, scene::Vec3 a,
                           scene::Vec3 b, float halo, float thickness,
                           gui::UiColor color) {
    projected_line(list, image, view, a, b, halo, overlay_halo);
    projected_line(list, image, view, a, b, thickness, color);
}

std::array<scene::Vec3, 3U> world_axes_of(const scene::Mat4& world) noexcept {
    static constexpr std::array<scene::Vec3, 3U> fallback_axes{
        scene::Vec3{1.0F, 0.0F, 0.0F}, scene::Vec3{0.0F, 1.0F, 0.0F},
        scene::Vec3{0.0F, 0.0F, 1.0F}};
    std::array<scene::Vec3, 3U> axes{};
    for (std::size_t index = 0U; index < axes.size(); ++index) {
        axes[index] = scene::normalize(
            scene::Vec3{world.at(0U, static_cast<core::u32>(index)),
                        world.at(1U, static_cast<core::u32>(index)),
                        world.at(2U, static_cast<core::u32>(index))},
            fallback_axes[index]);
    }
    return axes;
}

GizmoGeometry gizmo_geometry(const gui::Rect& image, const scene::Scene& view,
                             scene::Vec3 origin_world,
                             const std::array<scene::Vec3, 3U>& axes,
                             bool rings) noexcept {
    GizmoGeometry result{};
    if (view.canvas.height == 0U || image.height <= 0.0F) {
        return result;
    }
    const scene::Vec3 forward =
        scene::normalize(view.camera.target - view.camera.eye);
    result.world_origin = origin_world;
    const float depth = scene::dot(origin_world - view.camera.eye, forward);
    if (!(depth > 0.0F)) {
        return result;
    }
    const float canvas_pixels = gizmo_length_pixels *
                                static_cast<float>(view.canvas.height) /
                                image.height;
    const float units_per_pixel = scene::units_per_canvas_pixel(
        view.camera, depth, static_cast<float>(view.canvas.height));
    result.world_length = units_per_pixel * canvas_pixels;

    const auto origin =
        scene::project_to_canvas(view.camera, view.canvas, origin_world);
    if (!origin.visible) {
        return result;
    }
    result.origin = to_ui(image, view.canvas, origin);
    result.world_axes = axes;
    for (std::size_t index = 0U; index < result.world_axes.size(); ++index) {
        const auto endpoint =
            scene::project_to_canvas(view.camera, view.canvas,
                                     origin_world + result.world_axes[index] *
                                                        result.world_length);
        if (!endpoint.visible) {
            return GizmoGeometry{};
        }
        result.canvas_dx[index] = endpoint.x - origin.x;
        result.canvas_dy[index] = endpoint.y - origin.y;
        result.ends[index] = to_ui(image, view.canvas, endpoint);
    }
    if (rings) {
        const float radius = result.world_length * 0.92F;
        for (std::size_t index = 0U; index < 3U; ++index) {
            const scene::Vec3 u = result.world_axes[(index + 1U) % 3U];
            const scene::Vec3 v = result.world_axes[(index + 2U) % 3U];
            for (std::size_t step = 0U; step <= gizmo_ring_segments; ++step) {
                const float turns = static_cast<float>(step) /
                                    static_cast<float>(gizmo_ring_segments);
                const scene::Vec3 offset =
                    (u * scene::deterministic::cos_turns(turns) +
                     v * scene::deterministic::sin_turns(turns)) *
                    radius;
                const auto point = scene::project_to_canvas(
                    view.camera, view.canvas, origin_world + offset);
                if (!point.visible) {
                    return GizmoGeometry{};
                }
                result.ring_points[index][step] =
                    to_ui(image, view.canvas, point);
                result.ring_front[index][step] =
                    scene::dot(offset, forward) <= radius * 0.02F;
            }
        }
        result.rings = true;
    }
    result.visible = true;
    return result;
}

GizmoGeometry gizmo_geometry(const gui::Rect& image, const scene::Scene& view,
                             scene::Vec3 world) noexcept {
    return gizmo_geometry(image, view, world, world_basis);
}

float distance_to_segment(float px, float py, gui::UiPoint a,
                          gui::UiPoint b) noexcept {
    const float vx = b.x - a.x;
    const float vy = b.y - a.y;
    const float length_squared = vx * vx + vy * vy;
    if (!(length_squared > 0.0F)) {
        return std::numeric_limits<float>::infinity();
    }
    const float t = std::clamp(
        ((px - a.x) * vx + (py - a.y) * vy) / length_squared, 0.18F, 1.12F);
    const float dx = px - (a.x + vx * t);
    const float dy = py - (a.y + vy * t);
    return std::sqrt(dx * dx + dy * dy);
}

float distance_to_full_segment(float px, float py, gui::UiPoint a,
                               gui::UiPoint b) noexcept {
    const float vx = b.x - a.x;
    const float vy = b.y - a.y;
    const float length_squared = vx * vx + vy * vy;
    if (!(length_squared > 0.0F)) {
        return std::numeric_limits<float>::infinity();
    }
    const float t = std::clamp(
        ((px - a.x) * vx + (py - a.y) * vy) / length_squared, 0.0F, 1.0F);
    const float dx = px - (a.x + vx * t);
    const float dy = py - (a.y + vy * t);
    return std::sqrt(dx * dx + dy * dy);
}

GizmoAxis hit_gizmo(const GizmoGeometry& gizmo, float pointer_x,
                    float pointer_y) noexcept {
    if (!gizmo.visible) {
        return GizmoAxis::None;
    }
    GizmoAxis best = GizmoAxis::None;
    float best_distance = gizmo_hit_pixels;
    for (std::size_t index = 0U; index < gizmo.ends.size(); ++index) {
        const float screen_dx = gizmo.ends[index].x - gizmo.origin.x;
        const float screen_dy = gizmo.ends[index].y - gizmo.origin.y;
        if (screen_dx * screen_dx + screen_dy * screen_dy < 18.0F * 18.0F) {
            continue;
        }
        const float distance = distance_to_segment(
            pointer_x, pointer_y, gizmo.origin, gizmo.ends[index]);
        if (distance < best_distance) {
            best_distance = distance;
            best = axis_at(index);
        }
    }
    return best;
}

bool hit_view_ring(const GizmoGeometry& gizmo, float pointer_x,
                   float pointer_y) noexcept {
    if (!gizmo.visible) {
        return false;
    }
    const float dx = pointer_x - gizmo.origin.x;
    const float dy = pointer_y - gizmo.origin.y;
    return std::abs(std::sqrt(dx * dx + dy * dy) - view_ring_radius_pixels) <=
           view_ring_hit_pixels;
}

GizmoAxis hit_rotate_gizmo(const GizmoGeometry& gizmo, float pointer_x,
                           float pointer_y) noexcept {
    if (!gizmo.visible || !gizmo.rings) {
        return GizmoAxis::None;
    }
    GizmoAxis best = GizmoAxis::None;
    float best_distance = gizmo_hit_pixels;
    for (std::size_t index = 0U; index < 3U; ++index) {
        for (std::size_t step = 0U; step < gizmo_ring_segments; ++step) {
            if (!gizmo.ring_front[index][step] ||
                !gizmo.ring_front[index][step + 1U]) {
                continue;
            }
            const float distance = distance_to_full_segment(
                pointer_x, pointer_y, gizmo.ring_points[index][step],
                gizmo.ring_points[index][step + 1U]);
            if (distance < best_distance) {
                best_distance = distance;
                best = axis_at(index);
            }
        }
    }
    if (best == GizmoAxis::None && hit_view_ring(gizmo, pointer_x, pointer_y)) {
        best = GizmoAxis::View;
    }
    return best;
}

GizmoAxis hit_scale_gizmo(const GizmoGeometry& gizmo, float pointer_x,
                          float pointer_y) noexcept {
    const GizmoAxis axis = hit_gizmo(gizmo, pointer_x, pointer_y);
    if (axis != GizmoAxis::None) {
        return axis;
    }
    return hit_view_ring(gizmo, pointer_x, pointer_y) ? GizmoAxis::View
                                                      : GizmoAxis::None;
}

void draw_gizmo(gui::UiDrawList& list, const gui::Theme& theme,
                const GizmoGeometry& gizmo, GizmoAxis hovered,
                GizmoAxis active) {
    if (!gizmo.visible) {
        return;
    }
    const std::array<gui::UiColor, 3U> colors = axis_colors(theme);
    for (std::size_t index = 0U; index < gizmo.ends.size(); ++index) {
        const GizmoAxis axis = axis_at(index);
        const bool emphasized = hovered == axis || active == axis;
        const gui::UiColor color = emphasized ? theme.gizmo_hot : colors[index];
        const gui::UiPoint end = gizmo.ends[index];
        haloed_line(list, gizmo.origin, end, 5.0F, emphasized ? 3.2F : 2.2F,
                    color);
        // An axis seen end-on has no direction to point along.
        const gui::UiPoint along{end.x - gizmo.origin.x, end.y - gizmo.origin.y};
        if (std::hypot(along.x, along.y) > 1.0F) {
            gui::add_arrowhead(list, end, along, 10.0F, 4.5F, color);
        }
    }
    square(list, gizmo.origin, gizmo_hub_size, theme.gizmo_hub);
}

void draw_view_ring(gui::UiDrawList& list, const gui::Theme& theme,
                    const GizmoGeometry& gizmo, bool emphasized) {
    haloed_ring(list, gizmo.origin, view_ring_radius_pixels,
                gizmo_ring_segments, 4.0F, emphasized ? 2.6F : 1.6F,
                emphasized ? theme.gizmo_hot : theme.gizmo_view);
}

void draw_rotate_gizmo(gui::UiDrawList& list, const gui::Theme& theme,
                       const GizmoGeometry& gizmo, GizmoAxis hovered,
                       GizmoAxis active) {
    if (!gizmo.visible || !gizmo.rings) {
        return;
    }
    const std::array<gui::UiColor, 3U> colors = axis_colors(theme);
    for (std::size_t index = 0U; index < 3U; ++index) {
        const GizmoAxis axis = axis_at(index);
        // A drag on one ring hides the others, so the one being turned reads.
        if (active != GizmoAxis::None && active != axis) {
            continue;
        }
        const bool emphasized = hovered == axis || active == axis;
        const gui::UiColor color = emphasized ? theme.gizmo_hot : colors[index];
        const gui::UiColor faded{color.r, color.g, color.b, 0.28F};
        for (std::size_t step = 0U; step < gizmo_ring_segments; ++step) {
            const gui::UiPoint a = gizmo.ring_points[index][step];
            const gui::UiPoint b = gizmo.ring_points[index][step + 1U];
            if (gizmo.ring_front[index][step] &&
                gizmo.ring_front[index][step + 1U]) {
                haloed_line(list, a, b, 4.6F, emphasized ? 3.0F : 2.1F, color);
            } else if (active == GizmoAxis::None) {
                line(list, a, b, 1.2F, faded);
            }
        }
    }
    if (active == GizmoAxis::None || active == GizmoAxis::View) {
        draw_view_ring(list, theme, gizmo,
                       hovered == GizmoAxis::View || active == GizmoAxis::View);
    }
    square(list, gizmo.origin, gizmo_hub_size, theme.gizmo_hub);
}

void draw_scale_gizmo(gui::UiDrawList& list, const gui::Theme& theme,
                      const GizmoGeometry& gizmo, GizmoAxis hovered,
                      GizmoAxis active) {
    if (!gizmo.visible) {
        return;
    }
    const std::array<gui::UiColor, 3U> colors = axis_colors(theme);
    for (std::size_t index = 0U; index < gizmo.ends.size(); ++index) {
        const GizmoAxis axis = axis_at(index);
        const bool emphasized = hovered == axis || active == axis;
        const gui::UiColor color = emphasized ? theme.gizmo_hot : colors[index];
        haloed_line(list, gizmo.origin, gizmo.ends[index], 5.0F,
                    emphasized ? 3.2F : 2.2F, color);
        // A box at the end, as every scale gizmo marks its axes.
        haloed_square(list, gizmo.ends[index], 8.0F, 1.5F, color);
    }
    draw_view_ring(list, theme, gizmo,
                   hovered == GizmoAxis::View || active == GizmoAxis::View);
    square(list, gizmo.origin, gizmo_hub_size, theme.gizmo_hub);
}

}  // namespace mediaserver::editor
