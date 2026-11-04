#include "stage_camera.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace ajfx::studio {
namespace {

constexpr float two_pi = 6.28318530718F;

float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

Vec3 normalized(Vec3 v, Vec3 fallback) {
    const float l = length(v);
    return l > 1.0e-6F ? v * (1.0F / l) : fallback;
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

} // namespace ajfx::studio
