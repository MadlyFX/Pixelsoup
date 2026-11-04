#include "stage_overlay.hpp"

#include <cmath>

namespace ajfx::studio {
namespace {
constexpr float two_pi = 6.28318530718F;
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

} // namespace ajfx::studio
