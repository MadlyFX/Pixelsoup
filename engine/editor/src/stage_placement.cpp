#include <mediaserver/editor/stage_placement.hpp>

#include <mediaserver/editor/transform_tool.hpp>

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
    // Snapping steps across an axis-aligned surface, never off it along its
    // normal. A sloped surface has no grid to step across.
    if (request.snap && placement.surface != StagePlacementSurface::ViewPlane &&
        std::max({std::abs(n.x), std::abs(n.y), std::abs(n.z)}) >=
            axis_aligned) {
        if (std::abs(n.x) < axis_aligned) {
            placement.contact.x = snap_to(placement.contact.x, move_snap_step);
        }
        if (std::abs(n.y) < axis_aligned) {
            placement.contact.y = snap_to(placement.contact.y, move_snap_step);
        }
        if (std::abs(n.z) < axis_aligned) {
            placement.contact.z = snap_to(placement.contact.z, move_snap_step);
        }
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
                    yaw = snap_to(yaw, rotate_snap_step_turns);
                }
                placement.rotation.x = yaw;
            }
        }
    }

    if (placement.surface == StagePlacementSurface::ViewPlane) {
        placement.position = placement.contact;
        return placement;
    }

    // Rest the turned bounds on the surface: the corner deepest along the
    // normal touches it.
    const scene::Mat4 turned =
        scene::rotation(placement.rotation.x, placement.rotation.y,
                        placement.rotation.z);
    float deepest = std::numeric_limits<float>::infinity();
    float highest = -std::numeric_limits<float>::infinity();
    const scene::ModelLocalBounds& box = request.bounds;
    for (unsigned corner = 0U; corner < 8U; ++corner) {
        const scene::Vec3 point = scene::transform_point(
            turned,
            scene::Vec3{(corner & 1U) != 0U ? box.maximum.x : box.minimum.x,
                        (corner & 2U) != 0U ? box.maximum.y : box.minimum.y,
                        (corner & 4U) != 0U ? box.maximum.z : box.minimum.z});
        deepest = std::min(deepest, scene::dot(point, n));
        highest = std::max(highest, scene::dot(point, n));
    }
    placement.position = placement.contact - n * deepest;
    if (request.media && highest - deepest < 1.0e-4F) {
        placement.position = placement.position + n * flat_standoff_m;
    }
    return placement;
}

scene::Mat4 placement_world(const StagePlacement& placement) noexcept {
    return scene::multiply(scene::translation(placement.position),
                           scene::rotation(placement.rotation.x,
                                           placement.rotation.y,
                                           placement.rotation.z));
}

} // namespace mediaserver::editor
