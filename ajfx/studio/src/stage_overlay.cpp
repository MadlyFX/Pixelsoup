#include "stage_overlay.hpp"

#include <mediaserver/gui/shapes.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace ajfx::studio {
namespace {

constexpr float two_pi = 6.28318530718F;
// The most line pieces Paths draws in a frame. The interface draws at most
// 262,144 vertices a frame and refuses the frame beyond that, so paths keep
// well inside it, six vertices a piece, whatever the effect.
constexpr std::size_t max_path_segments = 12000U;

// A particle's colour as a line: HDR brought into range by its brightest
// channel, and its opacity held up enough to see a fading wisp's path.
gui::UiColor path_colour(const Color& c, float alpha) {
    const float peak = std::max({c.r, c.g, c.b, 1.0F});
    return gui::UiColor{c.r / peak, c.g / peak, c.b / peak, alpha * std::clamp(0.25F + c.a * 0.75F, 0.25F, 1.0F)};
}

} // namespace

CameraPose camera_pose(const Orbit& orbit) {
    const float yaw = orbit.yaw * two_pi;
    const float pitch = orbit.pitch * two_pi;
    const Vec3 offset{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    const Vec3 target{orbit.target.x, orbit.target.y, orbit.target.z};
    return CameraPose{.eye = target + offset * orbit.distance,
                      .target = target,
                      .field_of_view = preview_field_of_view};
}

void StageOverlay::draw(gui::UiContext& ctx, const gui::Rect& view, const Orbit& orbit, Document& document,
                        const OverlayInputs& inputs) {
    const StageCamera camera{camera_pose(orbit), view};
    gui::UiDrawList& list = ctx.draw_list();
    list.push_clip(view.x, view.y, view.width, view.height);
    const bool any_path = inputs.analysis != nullptr && inputs.layer != 0U &&
                          std::any_of(inputs.analysis->paths.begin(), inputs.analysis->paths.end(),
                                      [&](const ParticlePath& path) { return path.layer == inputs.layer; });
    if (inputs.show_paths && any_path) {
        // The picture is veiled a little so the paths read over bright glow.
        ctx.panel(view, gui::fade(ctx.theme().background, 0.35F));
        draw_paths(ctx, camera, inputs);
    }
    if (inputs.show_guides && inputs.guides != nullptr) {
        draw_guides(ctx, camera, *inputs.guides);
    }
    list.pop_clip();
    if (inputs.show_handles && inputs.guides != nullptr) {
        handles_.draw(ctx, view, camera, document, *inputs.guides);
    } else {
        handles_.reset();
    }
}

void StageOverlay::draw_paths(gui::UiContext& ctx, const StageCamera& camera, const OverlayInputs& inputs) {
    gui::UiDrawList& list = ctx.draw_list();
    const Analysis& analysis = *inputs.analysis;
    const double step = static_cast<double>(std::max<std::uint64_t>(analysis.path_step, 1U));
    // Every path is thinned by one stride so all of them fit the budget.
    // Only the working layer's: the rest would bury what is being tuned.
    std::size_t total = 0U;
    for (const ParticlePath& path : analysis.paths) {
        if (path.layer == inputs.layer) {
            total += path.points.size() > 1U ? path.points.size() - 1U : 0U;
        }
    }
    const std::size_t stride = std::max<std::size_t>(1U, (total + max_path_segments - 1U) / max_path_segments);
    for (const ParticlePath& path : analysis.paths) {
        if (path.layer != inputs.layer) {
            continue;
        }
        // The whole flight, faint, each piece in the particle's colour there.
        const std::size_t last = path.points.size() - 1U;
        std::optional<gui::UiPoint> from = camera.project(path.points.front());
        for (std::size_t i = 0U; i < last;) {
            const std::size_t next = std::min(i + stride, last);
            const std::optional<gui::UiPoint> to = camera.project(path.points[next]);
            if (from && to) {
                list.add_line(from->x, from->y, to->x, to->y, 1.0F, path_colour(path.colors[i], 0.55F));
            }
            from = to;
            i = next;
        }
        // Where the particle is now, if it is alive at the playhead.
        const double at = (inputs.frame - static_cast<double>(path.birth)) / step;
        if (at >= 0.0 && at <= static_cast<double>(path.points.size() - 1U)) {
            const auto k = static_cast<std::size_t>(at);
            const std::size_t next = std::min(k + 1U, path.points.size() - 1U);
            const float f = static_cast<float>(at - static_cast<double>(k));
            const Vec3 p = path.points[k] + (path.points[next] - path.points[k]) * f;
            if (const std::optional<gui::UiPoint> screen = camera.project(p)) {
                gui::add_disc(list, *screen, 2.2F, path_colour(path.colors[k], 1.0F), 8U);
            }
        }
    }
}

void StageOverlay::draw_guides(gui::UiContext& ctx, const StageCamera& camera, const GuideSet& guides) {
    gui::UiDrawList& list = ctx.draw_list();
    const gui::Theme& theme = ctx.theme();
    std::vector<gui::UiPoint> points;
    for (const Guide& guide : guides.guides) {
        // The block being worked on in the accent; the rest quietly.
        const gui::UiColor ink = guide.focused ? theme.accent : gui::fade(theme.text, 0.22F);
        for (const GuideStroke& stroke : guide.strokes) {
            const gui::UiColor colour = stroke.faint ? gui::fade(ink, ink.a * 0.45F) : ink;
            const float thickness = guide.focused && !stroke.faint ? 1.6F : 1.0F;
            points.clear();
            const auto flush = [&] {
                if (points.size() >= 2U) {
                    list.add_polyline(points, thickness, colour);
                }
                points.clear();
            };
            for (const Vec3& world : stroke.points) {
                if (const std::optional<gui::UiPoint> p = camera.project(world)) {
                    points.push_back(*p);
                } else {
                    flush();
                }
            }
            if (stroke.closed && !stroke.points.empty() && points.size() == stroke.points.size()) {
                points.push_back(points.front());
            }
            if (stroke.arrow && points.size() >= 2U) {
                const gui::UiPoint tip = points.back();
                const gui::UiPoint from = points[points.size() - 2U];
                const gui::UiPoint direction{tip.x - from.x, tip.y - from.y};
                if (direction.x != 0.0F || direction.y != 0.0F) {
                    gui::add_arrowhead(list, tip, direction, guide.kind == GuideKind::Field ? 5.0F : 8.0F,
                                       guide.kind == GuideKind::Field ? 2.5F : 4.0F, colour);
                }
            }
            flush();
        }
    }
}

} // namespace ajfx::studio
