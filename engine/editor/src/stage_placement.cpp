#include <mediaserver/editor/stage_placement.hpp>


#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace mediaserver::editor {
namespace {

// A surface this close to level is a floor (or a ceiling); steeper is a wall.
constexpr float level_surface = 0.7F;
// Within this of an axis, a surface is treated as facing along it for snapping.
constexpr float axis_aligned = 0.999F;
// A flat screen hung on a wall stands this far off it, so the two never fight
// over the same depth.
constexpr float flat_standoff_m = 0.001F;

[[nodiscard]] scene::Turns heading(float x, float z) noexcept {
    return static_cast<scene::Turns>(std::atan2(x, z) /
                                     (2.0F * std::numbers::pi_v<float>));
}

[[nodiscard]] float snap_to(float value, float step) noexcept {
    return std::round(value / step) * step;
}

// The world direction a turn sends the element's +Z face to.
[[nodiscard]] scene::Vec3 facing_of(const scene::Mat4& turned) noexcept {
    return scene::Vec3{turned.at(0U, 2U), turned.at(1U, 2U), turned.at(2U, 2U)};
}

} // namespace

scene::ModelLocalBounds media_placement_bounds(float aspect) noexcept {
    const float width = aspect > 0.0F ? aspect : 16.0F / 9.0F;
    return scene::ModelLocalBounds{scene::Vec3{-width, -1.0F, 0.0F},
                                   scene::Vec3{width, 1.0F, 0.0F}};
}

std::optional<StagePlacement>
place_on_stage(const StagePlacementRequest& request) noexcept {
    const scene::Ray& ray = request.ray;
    StagePlacement placement{};
    placement.bounds = request.bounds;
    if (request.hit.has_value()) {
        placement.surface = StagePlacementSurface::Element;
        placement.element = request.hit->node;
        placement.contact = request.hit->position;
        placement.normal = request.hit->normal;
    } else if (ray.direction.y < -1.0e-6F && ray.origin.y > 0.0F &&
               -ray.origin.y / ray.direction.y <= request.camera.far_plane) {
        placement.surface = StagePlacementSurface::Floor;
        placement.contact = ray.origin +
                            ray.direction * (-ray.origin.y / ray.direction.y);
        placement.contact.y = 0.0F;
        placement.normal = scene::Vec3{0.0F, 1.0F, 0.0F};
    } else {
        const scene::Vec3 forward = scene::normalize(
            request.camera.target - request.camera.eye);
        const float facing = scene::dot(ray.direction, forward);
        if (!(facing > 1.0e-6F)) {
            return std::nullopt;
        }
        placement.surface = StagePlacementSurface::ViewPlane;
        placement.contact =
            ray.origin +
            ray.direction *
                (scene::dot(request.camera.target - ray.origin, forward) /
                 facing);
        placement.normal = -forward;
    }

    const scene::Vec3 n = placement.normal;
    if (request.snap && placement.surface != StagePlacementSurface::ViewPlane) {
        placement.contact = snap_contact(placement.contact, n, request.snap_move);
    }

    if (request.media) {
        if (placement.surface == StagePlacementSurface::Element &&
            std::abs(n.y) < level_surface) {
            // Flat against the wall, facing out of it.
            placement.rotation.x = heading(n.x, n.z);
        } else {
            const scene::Vec3 toward = request.camera.eye - placement.contact;
            if (toward.x * toward.x + toward.z * toward.z > 1.0e-8F) {
                scene::Turns yaw = heading(toward.x, toward.z);
                if (request.snap) {
                    yaw = snap_to(yaw, request.snap_turns);
                }
                placement.rotation.x = yaw;
            }
        }
    }

    if (placement.surface == StagePlacementSurface::ViewPlane) {
        placement.position = placement.contact;
        return placement;
    }

    const scene::Mat4 turned =
        scene::rotation(placement.rotation.x, placement.rotation.y,
                        placement.rotation.z);
    placement.position =
        resting_origin(placement.contact, n, turned, request.bounds);
    if (request.media && (request.bounds.maximum.z - request.bounds.minimum.z) <
                             1.0e-4F &&
        std::abs(scene::dot(n, facing_of(turned))) > 0.999F) {
        // Flat against the surface: a millimetre proud of it.
        placement.position = placement.position + n * flat_standoff_m;
    }
    return placement;
}

scene::Vec3 resting_origin(scene::Vec3 contact, scene::Vec3 normal,
                           const scene::Mat4& linear,
                           const scene::ModelLocalBounds& bounds) noexcept {
    float deepest = std::numeric_limits<float>::infinity();
    for (unsigned corner = 0U; corner < 8U; ++corner) {
        const scene::Vec3 local{
            (corner & 1U) != 0U ? bounds.maximum.x : bounds.minimum.x,
            (corner & 2U) != 0U ? bounds.maximum.y : bounds.minimum.y,
            (corner & 4U) != 0U ? bounds.maximum.z : bounds.minimum.z};
        const scene::Vec3 turned{
            linear.at(0U, 0U) * local.x + linear.at(0U, 1U) * local.y +
                linear.at(0U, 2U) * local.z,
            linear.at(1U, 0U) * local.x + linear.at(1U, 1U) * local.y +
                linear.at(1U, 2U) * local.z,
            linear.at(2U, 0U) * local.x + linear.at(2U, 1U) * local.y +
                linear.at(2U, 2U) * local.z};
        deepest = std::min(deepest, scene::dot(turned, normal));
    }
    return contact - normal * deepest;
}

scene::Vec3 snap_contact(scene::Vec3 contact, scene::Vec3 normal,
                         float step) noexcept {
    if (!(step > 0.0F) ||
        std::max({std::abs(normal.x), std::abs(normal.y), std::abs(normal.z)}) <
            axis_aligned) {
        return contact;
    }
    if (std::abs(normal.x) < axis_aligned) {
        contact.x = snap_to(contact.x, step);
    }
    if (std::abs(normal.y) < axis_aligned) {
        contact.y = snap_to(contact.y, step);
    }
    if (std::abs(normal.z) < axis_aligned) {
        contact.z = snap_to(contact.z, step);
    }
    return contact;
}

scene::Mat4 placement_world(const StagePlacement& placement) noexcept {
    return scene::multiply(scene::translation(placement.position),
                           scene::rotation(placement.rotation.x,
                                           placement.rotation.y,
                                           placement.rotation.z));
}

} // namespace mediaserver::editor
