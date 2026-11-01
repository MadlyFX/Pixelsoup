#include "stage_overlay.hpp"

#include <mediaserver/gui/shapes.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace ajfx::studio {
namespace {

constexpr float two_pi = 6.28318530718F;
constexpr float handle_radius = 5.5F;
// The most line pieces Paths draws in a frame. The interface draws at most
// 262,144 vertices a frame and refuses the frame beyond that, so paths keep
// well inside it, six vertices a piece, whatever the effect.
constexpr std::size_t max_path_segments = 12000U;

float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

Vec3 normalized(Vec3 v, Vec3 fallback) {
    const float l = length(v);
    return l > 1.0e-6F ? v * (1.0F / l) : fallback;
}

// Where a ray meets the plane through `point` with `normal`, or nothing when
// it runs along the plane or away from it.
std::optional<Vec3> meet(Vec3 origin, Vec3 direction, Vec3 point, Vec3 normal) {
    const float facing = dot(direction, normal);
    if (std::fabs(facing) < 1.0e-3F) {
        return std::nullopt;
    }
    const float t = dot(point - origin, normal) / facing;
    if (!(t > 0.0F) || t > 1.0e4F) {
        return std::nullopt;
    }
    return origin + direction * t;
}

// A particle's colour as a line: HDR brought into range by its brightest
// channel, and its opacity held up enough to see a fading wisp's path.
gui::UiColor path_colour(const Color& c, float alpha) {
    const float peak = std::max({c.r, c.g, c.b, 1.0F});
    return gui::UiColor{c.r / peak, c.g / peak, c.b / peak, alpha * std::clamp(0.25F + c.a * 0.75F, 0.25F, 1.0F)};
}

} // namespace

StageCamera::StageCamera(const Orbit& orbit, const gui::Rect& view) : view_{view} {
    const float yaw = orbit.yaw * two_pi;
    const float pitch = orbit.pitch * two_pi;
    const Vec3 offset{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    const Vec3 target{orbit.target.x, orbit.target.y, orbit.target.z};
    eye_ = target + offset * orbit.distance;
    forward_ = normalized(target - eye_, Vec3{0.0F, 0.0F, -1.0F});
    right_ = normalized(cross(forward_, Vec3{0.0F, 1.0F, 0.0F}), Vec3{1.0F, 0.0F, 0.0F});
    up_ = cross(right_, forward_);
    tan_half_ = std::tan(preview_field_of_view * two_pi * 0.5F);
    aspect_ = view.width / std::max(view.height, 1.0F);
    near_ = std::max(orbit.distance * 0.01F, 0.01F);
}

std::optional<gui::UiPoint> StageCamera::project(Vec3 world) const {
    const Vec3 d = world - eye_;
    const float z = dot(d, forward_);
    if (z < near_) {
        return std::nullopt;
    }
    const float x = dot(d, right_) / (z * tan_half_ * aspect_);
    const float y = dot(d, up_) / (z * tan_half_);
    return gui::UiPoint{view_.x + (x + 1.0F) * 0.5F * view_.width, view_.y + (1.0F - y) * 0.5F * view_.height};
}

void StageCamera::ray(float x, float y, Vec3& origin, Vec3& direction) const {
    const float nx = (x - view_.x) / std::max(view_.width, 1.0F) * 2.0F - 1.0F;
    const float ny = 1.0F - (y - view_.y) / std::max(view_.height, 1.0F) * 2.0F;
    origin = eye_;
    direction = normalized(forward_ + right_ * (nx * tan_half_ * aspect_) + up_ * (ny * tan_half_), forward_);
}

float StageCamera::pixels_per_metre(Vec3 world) const {
    const float z = std::max(dot(world - eye_, forward_), near_);
    return view_.height * 0.5F / (z * tan_half_);
}

void StageOverlay::draw(gui::UiContext& ctx, const gui::Rect& view, const Orbit& orbit, Document& document,
                        const OverlayInputs& inputs) {
    const StageCamera camera{orbit, view};
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
        draw_handles(ctx, view, camera, document, *inputs.guides);
    } else {
        drag_.reset();
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

void StageOverlay::draw_handles(gui::UiContext& ctx, const gui::Rect& view, const StageCamera& camera, Document& document,
                                const GuideSet& guides) {
    const gui::Theme& theme = ctx.theme();
    gui::UiDrawList& list = ctx.draw_list();
    bool held_any = false;
    for (const Handle& handle : guides.handles) {
        // Off the picture a handle cannot be reached, unless it is the one
        // being dragged.
        const std::optional<gui::UiPoint> at = camera.project(handle.at);
        const bool dragged = drag_ && drag_->handle.block == handle.block && drag_->handle.control == handle.control &&
                             drag_->handle.end == handle.end;
        if (!at || (!view.contains(at->x, at->y) && !dragged)) {
            continue;
        }
        const gui::Rect hit{at->x - 10.0F, at->y - 10.0F, 20.0F, 20.0F};
        const gui::UiId id = part_id(handle.block, handle.control, 60U + static_cast<std::uint32_t>(handle.end));
        const gui::Interaction interaction = ctx.grab(id, hit);
        ctx.cursor_for(interaction, gui::Cursor::Move);

        Vec3 origin{};
        Vec3 direction{};
        camera.ray(ctx.pointer_x(), ctx.pointer_y(), origin, direction);
        const Vec3 up{0.0F, 1.0F, 0.0F};
        // The plane a drag moves in, through the handle.
        const auto plane_hit = [&](const Handle& h, bool vertical) -> std::optional<Vec3> {
            switch (h.motion) {
            case HandleMotion::Floor:
                if (!vertical) {
                    return meet(origin, direction, h.at, up);
                }
                [[fallthrough]];
            case HandleMotion::Height: {
                // Upright, turned to face the camera.
                const Vec3 facing = normalized(Vec3{camera.forward().x, 0.0F, camera.forward().z}, Vec3{0.0F, 0.0F, 1.0F});
                const std::optional<Vec3> p = meet(origin, direction, h.at, facing);
                return p ? std::optional<Vec3>{Vec3{h.at.x, p->y, h.at.z}} : std::nullopt;
            }
            case HandleMotion::Heading:
                return meet(origin, direction, h.origin, up);
            case HandleMotion::Radius:
                return meet(origin, direction, h.origin, camera.forward());
            case HandleMotion::Corner:
                return meet(origin, direction, h.at, camera.forward());
            }
            return std::nullopt;
        };

        if (interaction.pressed) {
            const bool vertical = ctx.shift_held();
            const std::optional<Vec3> p = plane_hit(handle, vertical);
            Vec3 grab{};
            if (p && handle.motion != HandleMotion::Radius && handle.motion != HandleMotion::Heading) {
                grab = handle.at - *p;
            }
            drag_ = Drag{handle, grab, vertical};
        }
        const bool mine = drag_ && drag_->handle.block == handle.block && drag_->handle.control == handle.control &&
                          drag_->handle.end == handle.end;
        if (interaction.held && mine) {
            held_any = true;
            // Shift changes the plane mid-drag: the grab is taken afresh so
            // the handle stays under the pointer.
            const bool vertical = ctx.shift_held() && handle.motion == HandleMotion::Floor;
            if (vertical != drag_->vertical) {
                const std::optional<Vec3> p = plane_hit(handle, vertical);
                drag_->grab = p ? handle.at - *p : Vec3{};
                drag_->vertical = vertical;
            }
            if (const std::optional<Vec3> p = plane_hit(handle, vertical)) {
                Vec3 world = *p + drag_->grab;
                if (handle.motion == HandleMotion::Floor) {
                    // Across the floor keeps the height; up and down keeps the place.
                    world = vertical ? Vec3{handle.at.x, world.y, handle.at.z} : Vec3{world.x, handle.at.y, world.z};
                }
                if (std::optional<Value> value = handle_value(document, handle, world)) {
                    if (const auto place = document.find_block(handle.block)) {
                        Block& block = place->layer->blocks[place->index];
                        if (block.find(handle.control) == nullptr || !(*block.find(handle.control) == *value)) {
                            block.set(handle.control, std::move(*value));
                        }
                    }
                }
            }
        }

        const float grow = ctx.ease(part_id(handle.block, handle.control, 70U + static_cast<std::uint32_t>(handle.end)),
                                    interaction.hovered || interaction.held ? 1.0F : 0.0F);
        const float radius = handle_radius + 1.5F * grow;
        gui::add_disc(list, *at, radius + 1.5F, theme.background, 16U);
        if (handle.motion == HandleMotion::Floor) {
            gui::add_disc(list, *at, radius, theme.accent, 16U);
        } else {
            gui::add_diamond(list, *at, radius + 0.5F, theme.accent);
        }
        std::string tip = handle.label;
        switch (handle.motion) {
        case HandleMotion::Floor: tip += ": drag across the floor, hold Shift to raise or lower"; break;
        case HandleMotion::Height: tip += ": drag up or down"; break;
        case HandleMotion::Radius: tip += ": drag in or out"; break;
        case HandleMotion::Heading: tip += ": drag round to turn"; break;
        case HandleMotion::Corner: tip += ": drag the corner"; break;
        }
        ctx.tooltip(hit, tip);
    }
    if (!held_any) {
        drag_.reset();
    }
}

} // namespace ajfx::studio
