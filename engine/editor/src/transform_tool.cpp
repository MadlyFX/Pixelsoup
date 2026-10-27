#include <mediaserver/editor/transform_tool.hpp>
#include <mediaserver/scene/raycast.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <utility>

namespace mediaserver::editor {
namespace {

constexpr double turns_per_radian = 0.5 / std::numbers::pi;

// A drawn world vector in the authored parent space, without the
// translation a point would pick up.
[[nodiscard]] scene::Vec3 direction_to_local(const scene::Mat4& world_to_local,
                                             scene::Vec3 vector) noexcept {
    return scene::transform_point(world_to_local, vector) -
           scene::transform_point(world_to_local, scene::Vec3{});
}

[[nodiscard]] float component(scene::Vec3 v, std::size_t index) noexcept {
    return index == 0U ? v.x : (index == 1U ? v.y : v.z);
}

[[nodiscard]] double nearest_turn(double value, double near) noexcept {
    return value + std::round(near - value);
}

} // namespace

float snap_to_step(float value, float step) noexcept {
    if (!(step > 0.0F) || !std::isfinite(value)) {
        return value;
    }
    return std::round(value / step) * step;
}

scene::Vec3 rotation_turns_of(const std::array<scene::Vec3, 3U>& axes,
                              scene::Vec3 near) noexcept {
    // scene::rotation is yaw about +Y, then pitch about +X, then roll about
    // -Z: R = Ry(yaw) Rx(pitch) Rz(-roll). Its third column is
    // (sin yaw cos pitch, -sin pitch, cos yaw cos pitch) and its second row
    // is (cos pitch sin(-roll), cos pitch cos(-roll), -sin pitch).
    const double sin_pitch =
        std::clamp(-static_cast<double>(axes[2U].y), -1.0, 1.0);
    const double pitch = std::asin(sin_pitch);
    const double cos_pitch = std::cos(pitch);

    double yaw = 0.0;
    double roll = 0.0;
    // Float columns leave a vertical pitch's cosine around 1e-4, not zero.
    if (cos_pitch > 1.0e-3) {
        yaw = std::atan2(static_cast<double>(axes[2U].x),
                         static_cast<double>(axes[2U].z));
        roll = -std::atan2(static_cast<double>(axes[0U].y),
                           static_cast<double>(axes[1U].y));
    } else {
        // Gimbal lock: yaw and roll turn about the same axis. Keep the roll
        // the element started with and give the rest to yaw.
        roll = static_cast<double>(near.z) / turns_per_radian;
        // With pitch at +/-90 degrees the first column is (cos, 0, -sin) of
        // yaw + roll or yaw - roll respectively.
        const double combined = std::atan2(-static_cast<double>(axes[0U].z),
                                           static_cast<double>(axes[0U].x));
        yaw = sin_pitch > 0.0 ? combined - roll : combined + roll;
    }

    const auto settle = [&](double y, double p, double r) {
        const scene::Vec3 candidate{
            static_cast<float>(nearest_turn(y * turns_per_radian, near.x)),
            static_cast<float>(nearest_turn(p * turns_per_radian, near.y)),
            static_cast<float>(nearest_turn(r * turns_per_radian, near.z))};
        const scene::Vec3 offset = candidate - near;
        return std::pair{candidate, scene::dot(offset, offset)};
    };
    const auto first = settle(yaw, pitch, roll);
    // The same orientation, reached with the pitch past vertical.
    const auto second = settle(yaw + std::numbers::pi,
                               std::numbers::pi - pitch,
                               roll + std::numbers::pi);
    return second.second < first.second ? second.first : first.first;
}

TransformResult apply_transform(const TransformStart& start,
                                const TransformDelta& delta) noexcept {
    TransformResult result{start.local_position, start.rotation, start.scale};
    const scene::Vec3 offset = start.world_position - delta.pivot;
    scene::Vec3 world_delta{};
    switch (delta.tool) {
    case TransformTool::Move:
        world_delta = delta.translation;
        break;
    case TransformTool::Rotate: {
        if (delta.turns == 0.0F) {
            break;
        }
        world_delta =
            scene::rotate_around(offset, delta.axis, delta.turns) - offset;
        const scene::Vec3 local_axis = scene::normalize(
            direction_to_local(start.world_to_local, delta.axis), delta.axis);
        const scene::Mat4 before = scene::rotation(
            start.rotation.x, start.rotation.y, start.rotation.z);
        std::array<scene::Vec3, 3U> axes{};
        for (std::size_t column = 0U; column < axes.size(); ++column) {
            const auto c = static_cast<core::u32>(column);
            axes[column] = scene::rotate_around(
                scene::Vec3{before.at(0U, c), before.at(1U, c),
                            before.at(2U, c)},
                local_axis, delta.turns);
        }
        result.rotation = rotation_turns_of(axes, start.rotation);
        break;
    }
    case TransformTool::Scale: {
        scene::Vec3 scaled{};
        for (std::size_t index = 0U; index < delta.basis.size(); ++index) {
            scaled = scaled + delta.basis[index] *
                                  (scene::dot(offset, delta.basis[index]) *
                                   component(delta.factors, index));
        }
        world_delta = scaled - offset;
        // Each of the element's own axes takes the factor of the gesture
        // axes it lies along: exact when they coincide, and a blend that
        // keeps the sign of a mirror when they do not.
        std::array<float, 3U> factor{};
        for (std::size_t own = 0U; own < factor.size(); ++own) {
            for (std::size_t index = 0U; index < delta.basis.size(); ++index) {
                const float along =
                    scene::dot(start.world_axes[own], delta.basis[index]);
                factor[own] += along * along * component(delta.factors, index);
            }
        }
        result.scale = scene::Vec3{start.scale.x * factor[0U],
                                   start.scale.y * factor[1U],
                                   start.scale.z * factor[2U]};
        break;
    }
    }
    result.position = start.local_position +
                      direction_to_local(start.world_to_local, world_delta);
    return result;
}

scene::Mat4 transformed_world(const scene::Mat4& world,
                              const TransformDelta& delta) noexcept {
    std::array<scene::Vec3, 3U> columns{};
    for (std::size_t column = 0U; column < columns.size(); ++column) {
        const auto c = static_cast<core::u32>(column);
        columns[column] =
            scene::Vec3{world.at(0U, c), world.at(1U, c), world.at(2U, c)};
    }
    const scene::Vec3 origin{world.at(0U, 3U), world.at(1U, 3U),
                             world.at(2U, 3U)};
    const scene::Vec3 offset = origin - delta.pivot;
    scene::Vec3 moved = origin;
    switch (delta.tool) {
    case TransformTool::Move:
        moved = origin + delta.translation;
        break;
    case TransformTool::Rotate:
        for (scene::Vec3& column : columns) {
            column = scene::rotate_around(column, delta.axis, delta.turns);
        }
        moved = delta.pivot +
                scene::rotate_around(offset, delta.axis, delta.turns);
        break;
    case TransformTool::Scale: {
        scene::Vec3 scaled{};
        for (std::size_t index = 0U; index < delta.basis.size(); ++index) {
            scaled = scaled + delta.basis[index] *
                                  (scene::dot(offset, delta.basis[index]) *
                                   component(delta.factors, index));
        }
        moved = delta.pivot + scaled;
        for (scene::Vec3& column : columns) {
            const scene::Vec3 axis = scene::normalize(column, scene::Vec3{});
            float factor = 0.0F;
            for (std::size_t index = 0U; index < delta.basis.size(); ++index) {
                const float along = scene::dot(axis, delta.basis[index]);
                factor += along * along * component(delta.factors, index);
            }
            column = column * factor;
        }
        break;
    }
    }
    scene::Mat4 result = world;
    for (std::size_t column = 0U; column < columns.size(); ++column) {
        const auto c = static_cast<core::u32>(column);
        result.set(0U, c, columns[column].x);
        result.set(1U, c, columns[column].y);
        result.set(2U, c, columns[column].z);
    }
    result.set(0U, 3U, moved.x);
    result.set(1U, 3U, moved.y);
    result.set(2U, 3U, moved.z);
    return result;
}

scene::Vec3 pointer_ray(const scene::Camera& camera, scene::Canvas canvas,
                        float canvas_x, float canvas_y) noexcept {
    const scene::Vec3 forward = scene::normalize(camera.target - camera.eye);
    if (canvas.height == 0U) {
        return forward;
    }
    const scene::Vec3 right = scene::normalize(scene::cross(forward, camera.up));
    const scene::Vec3 up = scene::cross(right, forward);
    const float units_per_pixel =
        2.0F * scene::deterministic::tan_half_turns(camera.field_of_view) /
        static_cast<float>(canvas.height);
    const float from_centre_x =
        canvas_x - static_cast<float>(canvas.width) * 0.5F;
    const float from_centre_y =
        canvas_y - static_cast<float>(canvas.height) * 0.5F;
    return forward + right * (from_centre_x * units_per_pixel) +
           up * (-from_centre_y * units_per_pixel);
}

std::optional<scene::Vec3>
plane_drag_translation(const scene::Camera& camera, scene::Canvas canvas,
                       scene::Vec3 origin, scene::Vec3 normal, float from_x,
                       float from_y, float to_x, float to_y) noexcept {
    const scene::Vec3 unit_normal = scene::normalize(normal, scene::Vec3{});
    if (scene::dot(unit_normal, unit_normal) == 0.0F) {
        return std::nullopt;
    }
    const auto hit = [&](float x, float y) -> std::optional<scene::Vec3> {
        // An orthographic view's rays are parallel and start on its eye
        // plane, not at the eye.
        scene::Vec3 start = camera.eye;
        scene::Vec3 ray = pointer_ray(camera, canvas, x, y);
        if (scene::orthographic(camera)) {
            const auto parallel = scene::canvas_ray(camera, canvas, x, y);
            if (!parallel.has_value()) {
                return std::nullopt;
            }
            start = parallel->origin;
            ray = parallel->direction;
        }
        const float facing = scene::dot(ray, unit_normal);
        const float ray_length = scene::length(ray);
        // Within about three degrees of edge-on the plane is a line on
        // screen and has no stable point under the pointer.
        if (!(ray_length > 0.0F) || std::abs(facing) < 0.05F * ray_length) {
            return std::nullopt;
        }
        const float distance =
            scene::dot(origin - start, unit_normal) / facing;
        if (!(distance > 0.0F) || !std::isfinite(distance)) {
            return std::nullopt;
        }
        return start + ray * distance;
    };
    const auto from = hit(from_x, from_y);
    const auto to = hit(to_x, to_y);
    if (!from.has_value() || !to.has_value()) {
        return std::nullopt;
    }
    return *to - *from;
}

float sweep_turns(float centre_x, float centre_y, float from_x, float from_y,
                  float to_x, float to_y, scene::Vec3 axis,
                  scene::Vec3 view_forward) noexcept {
    const double from_angle = std::atan2(static_cast<double>(from_y - centre_y),
                                         static_cast<double>(from_x - centre_x));
    const double to_angle = std::atan2(static_cast<double>(to_y - centre_y),
                                       static_cast<double>(to_x - centre_x));
    double swept = to_angle - from_angle;
    if (swept > std::numbers::pi) {
        swept -= 2.0 * std::numbers::pi;
    } else if (swept < -std::numbers::pi) {
        swept += 2.0 * std::numbers::pi;
    }
    // Pixels run y-down, so a growing angle is clockwise on screen. A
    // right-handed turn about an axis facing the viewer is anticlockwise.
    const double facing = scene::dot(axis, view_forward) < 0.0F ? 1.0 : -1.0;
    return static_cast<float>(-swept * facing * turns_per_radian);
}

bool NumericEntry::type(std::string_view text) {
    bool changed = false;
    for (const char c : text) {
        if (c >= '0' && c <= '9') {
            text_.push_back(c);
            changed = true;
        } else if ((c == '.' || c == ',') &&
                   text_.find('.') == std::string::npos) {
            text_.push_back('.');
            changed = true;
        } else if (c == '-') {
            negative_ = !negative_;
            changed = true;
        }
    }
    return changed;
}

void NumericEntry::backspace() noexcept {
    if (!text_.empty()) {
        text_.pop_back();
    } else {
        negative_ = false;
    }
}

std::optional<float> NumericEntry::value() const noexcept {
    if (text_.empty()) {
        return std::nullopt;
    }
    const float magnitude = std::strtof(text_.c_str(), nullptr);
    if (!std::isfinite(magnitude)) {
        return std::nullopt;
    }
    return negative_ ? -magnitude : magnitude;
}

std::string NumericEntry::display() const {
    return (negative_ ? std::string{"-"} : std::string{}) + text_;
}

} // namespace mediaserver::editor
