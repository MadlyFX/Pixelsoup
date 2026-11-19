// The laser pointer over the Stage and Program views: the dot that stands in
// for the system pointer while L is held, the strokes the buttons draw, and
// every mark a view can show.

#include "viewport_support.hpp"

#include <mediaserver/scene/raycast.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace mediaserver::editor {
namespace {

// A stroke takes a new point once the pointer has moved this far, in panel
// units: smooth at the speed a hand draws, and well inside the budget.
constexpr float laser_step = 2.5F;
// How far along a canvas pixel's line of sight a point is placed before a
// projected view carries it. Any distance in front of the eye lands on the
// same pixel; this stays clear of the near plane.
constexpr float sight_distance = 10.0F;

[[nodiscard]] std::optional<scene::Vec3>
sight_point(const scene::Scene& view, float x, float y) noexcept {
    const auto ray = scene::canvas_ray(view.camera, view.canvas, x, y);
    if (!ray.has_value()) {
        return std::nullopt;
    }
    return ray->origin + ray->direction * sight_distance;
}

} // namespace

std::optional<scene::CanvasProjection>
ViewportPanel::laser_canvas_to_view(const scene::Scene& view,
                                    const ViewportProjection* projection,
                                    float x, float y) noexcept {
    if (projection == nullptr || !projection->project) {
        return scene::CanvasProjection{x, y, 0.0F, true};
    }
    const auto world = sight_point(view, x, y);
    if (!world.has_value()) {
        return std::nullopt;
    }
    const scene::CanvasProjection mapped = projection->project(*world);
    if (!mapped.visible || !std::isfinite(mapped.x) ||
        !std::isfinite(mapped.y)) {
        return std::nullopt;
    }
    return mapped;
}

std::optional<scene::Vec3>
ViewportPanel::laser_view_to_canvas(const scene::Scene& view,
                                    const ViewportProjection& projection,
                                    float x, float y,
                                    scene::Vec3 guess) noexcept {
    // A warped output has no inverse to hand, but its forward projection is
    // smooth, so Newton's method finds the canvas pixel from a guess - the
    // last answer, as the pointer moves only a little between frames. A
    // slice settles in one step.
    const auto at = [&](float cx, float cy) {
        return laser_canvas_to_view(view, &projection, cx, cy);
    };
    float cx = guess.x;
    float cy = guess.y;
    const float limit = static_cast<float>(
        std::max(view.canvas.width, view.canvas.height));
    for (int iteration = 0; iteration < 16; ++iteration) {
        const auto here = at(cx, cy);
        if (!here.has_value()) {
            return std::nullopt;
        }
        const float ex = here->x - x;
        const float ey = here->y - y;
        if (std::fabs(ex) < 0.05F && std::fabs(ey) < 0.05F) {
            return scene::Vec3{cx, cy, 0.0F};
        }
        constexpr float h = 1.0F;
        const auto across = at(cx + h, cy);
        const auto down = at(cx, cy + h);
        if (!across.has_value() || !down.has_value()) {
            return std::nullopt;
        }
        const float a = (across->x - here->x) / h;
        const float b = (down->x - here->x) / h;
        const float c = (across->y - here->y) / h;
        const float d = (down->y - here->y) / h;
        const float det = a * d - b * c;
        if (!std::isfinite(det) || std::fabs(det) < 1e-9F) {
            return std::nullopt;
        }
        float step_x = (d * ex - b * ey) / det;
        float step_y = (a * ey - c * ex) / det;
        // A wild first step off a poor guess is reined in, not followed.
        const float length = std::hypot(step_x, step_y);
        if (length > limit * 0.25F) {
            step_x *= limit * 0.25F / length;
            step_y *= limit * 0.25F / length;
        }
        cx -= step_x;
        cy -= step_y;
    }
    const auto here = at(cx, cy);
    if (here.has_value() && std::fabs(here->x - x) < 0.5F &&
        std::fabs(here->y - y) < 0.5F) {
        return scene::Vec3{cx, cy, 0.0F};
    }
    return std::nullopt;
}

std::optional<LaserMark> ViewportPanel::run_laser(
    gui::UiContext& ctx, LaserPointer& laser, const gui::Rect& image,
    const scene::Scene& view, const ViewportProjection* projection,
    bool pointer_available) {
    if (image.width <= 0.0F || image.height <= 0.0F ||
        view.canvas.width == 0U || view.canvas.height == 0U) {
        laser_drawing_ = false;
        return std::nullopt;
    }
    const gui::Theme& theme = ctx.theme();
    gui::UiDrawList& list = ctx.draw_list();
    const scene::Canvas view_canvas =
        projection != nullptr ? projection->canvas : view.canvas;
    // One pixel of the picture on screen, so a line is as thick against the
    // picture as it will be on the wall.
    const float picture_scale =
        image.height / static_cast<float>(view_canvas.height);
    const float canvas_height = static_cast<float>(view.canvas.height);
    const float line_width =
        role_ == ViewportRole::Stage
            ? 3.0F
            : std::max(canvas_height *
                           show::pointer_line_width_per_canvas_height *
                           picture_scale,
                       2.0F);
    const float dot_radius =
        role_ == ViewportRole::Stage
            ? 6.0F
            : std::max(canvas_height *
                           show::pointer_dot_radius_per_canvas_height *
                           picture_scale,
                       5.0F);

    // Where a mark is on this view's screen, if this view can show it.
    const auto on_screen =
        [&](LaserSpace space, scene::Vec3 at) -> std::optional<gui::UiPoint> {
        if (space == LaserSpace::World) {
            const scene::CanvasProjection projected =
                projection != nullptr && projection->project
                    ? projection->project(at)
                    : scene::project_to_canvas(view.camera, view.canvas, at);
            if (!projected.visible) {
                return std::nullopt;
            }
            return to_ui(image, view_canvas, projected);
        }
        // A canvas mark belongs to the Program picture; the Stage's free
        // camera has no canvas of its own to put it on.
        if (role_ == ViewportRole::Stage) {
            return std::nullopt;
        }
        const auto mapped = laser_canvas_to_view(view, projection, at.x, at.y);
        if (!mapped.has_value()) {
            return std::nullopt;
        }
        return to_ui(image, view_canvas, *mapped);
    };

    // Where the pointer is pointing, as a mark this view keeps.
    const auto mark_at = [&](float px,
                             float py) -> std::optional<LaserMark> {
        const CanvasPoint point = to_canvas(image, view_canvas, px, py);
        if (role_ == ViewportRole::Stage) {
            if (const auto hit = surface_at(view, point.x, point.y)) {
                laser_depth_ = std::max(hit->distance, 0.05F);
                return LaserMark{LaserSpace::World, hit->position};
            }
            const auto ray =
                scene::canvas_ray(view.camera, view.canvas, point.x, point.y);
            if (!ray.has_value()) {
                return std::nullopt;
            }
            // Past every surface: the stage floor, else as deep as the last
            // mark, so a line drawn into the sky stays where it was drawn.
            if (ray->direction.y < -1e-4F && ray->origin.y > 0.0F) {
                const float along = -ray->origin.y / ray->direction.y;
                if (along < 10'000.0F) {
                    laser_depth_ = along;
                    return LaserMark{LaserSpace::World,
                                     ray->origin + ray->direction * along};
                }
            }
            return LaserMark{LaserSpace::World,
                             ray->origin + ray->direction * laser_depth_};
        }
        if (projection == nullptr || !projection->project) {
            return LaserMark{LaserSpace::Canvas,
                             scene::Vec3{point.x, point.y, 0.0F}};
        }
        const scene::Vec3 middle{static_cast<float>(view.canvas.width) * 0.5F,
                                 canvas_height * 0.5F, 0.0F};
        auto found = laser_view_to_canvas(
            view, *projection, point.x, point.y,
            laser_guess_valid_ ? laser_guess_ : middle);
        if (!found.has_value() && laser_guess_valid_) {
            found = laser_view_to_canvas(view, *projection, point.x, point.y,
                                         middle);
        }
        if (!found.has_value()) {
            laser_guess_valid_ = false;
            return std::nullopt;
        }
        laser_guess_ = *found;
        laser_guess_valid_ = true;
        return LaserMark{LaserSpace::Canvas, *found};
    };

    const float px = ctx.pointer_x();
    const float py = ctx.pointer_y();
    const bool over =
        laser.active() && pointer_available && image.contains(px, py);
    std::optional<LaserMark> pointing;
    if (over || laser_drawing_) {
        pointing = mark_at(px, py);
    }

    // The buttons. A stroke is this view's from its press to its release,
    // wherever the pointer goes meanwhile.
    if (!laser.active() || !laser.drawing()) {
        laser_drawing_ = false;
    }
    if (over && !laser_drawing_ && pointing.has_value()) {
        const bool left = ctx.pointer_pressed();
        const bool right = ctx.secondary_pointer_pressed();
        if ((left || right) &&
            laser.begin_stroke(left ? LaserInk::Fleeting : LaserInk::Kept,
                               *pointing)) {
            laser_drawing_ = true;
            laser_last_x_ = px;
            laser_last_y_ = py;
        }
    } else if (laser_drawing_) {
        const bool held = ctx.pointer_down() || ctx.secondary_pointer_down();
        if (held && pointing.has_value() &&
            pointing->space == laser.drawing_space() &&
            std::hypot(px - laser_last_x_, py - laser_last_y_) >= laser_step) {
            static_cast<void>(laser.extend(pointing->at));
            laser_last_x_ = px;
            laser_last_y_ = py;
        }
        if (!held) {
            laser.end_stroke();
            laser_drawing_ = false;
        }
    }

    // The marks, then the dot over them.
    ctx.push_clip(image);
    std::vector<gui::UiPoint> line;
    for (const LaserStroke& stroke : laser.strokes()) {
        line.clear();
        line.reserve(stroke.points.size());
        const auto flush = [&] {
            if (line.size() >= 2U) {
                list.add_polyline(line, line_width, theme.pointer);
            } else if (line.size() == 1U) {
                const float r = line_width;
                list.add_rounded_rect(line.front().x - r, line.front().y - r,
                                      r * 2.0F, r * 2.0F, r, theme.pointer);
            }
            line.clear();
        };
        for (const scene::Vec3& point : stroke.points) {
            const auto at = on_screen(stroke.space, point);
            if (!at.has_value()) {
                flush();
                continue;
            }
            line.push_back(*at);
        }
        flush();
    }
    std::optional<gui::UiPoint> dot_at;
    if (over) {
        // The dot is the pointer here: exactly under it, never a frame late.
        dot_at = gui::UiPoint{px, py};
        ctx.set_cursor(gui::Cursor::Hidden);
    } else if (laser.active() && laser.dot().has_value()) {
        dot_at = on_screen(laser.dot()->space, laser.dot()->at);
    }
    if (dot_at.has_value()) {
        const float r = dot_radius;
        list.add_rounded_glow(dot_at->x - r, dot_at->y - r, r * 2.0F, r * 2.0F,
                              r, r * 1.5F, gui::fade(theme.pointer, 0.55F));
        list.add_rounded_rect(dot_at->x - r, dot_at->y - r, r * 2.0F,
                              r * 2.0F, r, theme.pointer);
        const float core_radius = r * 0.45F;
        list.add_rounded_rect(dot_at->x - core_radius,
                              dot_at->y - core_radius, core_radius * 2.0F,
                              core_radius * 2.0F, core_radius,
                              gui::blend(theme.pointer, theme.playhead, 0.8F));
    }
    ctx.pop_clip();
    return over ? pointing : std::nullopt;
}

} // namespace mediaserver::editor
