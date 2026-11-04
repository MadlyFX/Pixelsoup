#include "stage_camera.hpp"

#include <mediaserver/gui/shapes.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace ajfx::studio {
namespace {

constexpr float two_pi = 6.28318530718F;
constexpr float handle_radius = 5.5F;

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

} // namespace

StageCamera::StageCamera(const CameraPose& pose, const gui::Rect& view) : view_{view} {
    eye_ = pose.eye;
    forward_ = normalized(pose.target - eye_, Vec3{0.0F, 0.0F, -1.0F});
    right_ = normalized(cross(forward_, Vec3{0.0F, 1.0F, 0.0F}), Vec3{1.0F, 0.0F, 0.0F});
    up_ = cross(right_, forward_);
    tan_half_ = std::tan(pose.field_of_view * two_pi * 0.5F);
    aspect_ = view.width / std::max(view.height, 1.0F);
    near_ = std::max(length(pose.target - eye_) * 0.01F, 0.01F);
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

std::array<float, 16> StageCamera::view_matrix() const noexcept {
    return {right_.x, up_.x, -forward_.x, 0.0F,
            right_.y, up_.y, -forward_.y, 0.0F,
            right_.z, up_.z, -forward_.z, 0.0F,
            -dot(right_, eye_), -dot(up_, eye_), dot(forward_, eye_), 1.0F};
}

std::array<float, 16> StageCamera::projection_matrix(float near_plane, float far_plane) const noexcept {
    const float f = 1.0F / tan_half_;
    return {f / aspect_, 0.0F, 0.0F, 0.0F,
            0.0F, f, 0.0F, 0.0F,
            0.0F, 0.0F, -(far_plane + near_plane) / (far_plane - near_plane), -1.0F,
            0.0F, 0.0F, -2.0F * far_plane * near_plane / (far_plane - near_plane), 0.0F};
}

float StageCamera::pixels_per_metre(Vec3 world) const {
    const float z = std::max(dot(world - eye_, forward_), near_);
    return view_.height * 0.5F / (z * tan_half_);
}

void HandleDrags::draw(gui::UiContext& ctx, const gui::Rect& view, const StageCamera& camera, Document& document,
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
            // The release frame still moves the handle to where it was let
            // go, but the drag ends with it.
            held_any = held_any || !interaction.released;
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
