#pragma once

#include <mediaserver/scene/canvas.hpp>

namespace mediaserver::editor {

struct StageOrbit final {
    scene::Vec3 target{};
    scene::Turns yaw{0.10F};
    scene::Turns pitch{0.06F};
    float distance{9.0F};
};

// Orbit changes the viewpoint, not the scene's photographic exposure. Rebuild
// from the evaluated Stage camera every frame so animated exposure and scene
// switches remain consistent with the image that will go to Program.
[[nodiscard]] inline scene::Camera stage_camera_for(
    const scene::Camera& authored, const StageOrbit& orbit) noexcept {
    scene::Camera camera = authored;
    const float cp = scene::deterministic::cos_turns(orbit.pitch);
    const float sp = scene::deterministic::sin_turns(orbit.pitch);
    const float cy = scene::deterministic::cos_turns(orbit.yaw);
    const float sy = scene::deterministic::sin_turns(orbit.yaw);
    camera.target = orbit.target;
    camera.eye = orbit.target + scene::Vec3{orbit.distance * cp * sy,
                                           orbit.distance * sp,
                                           orbit.distance * cp * cy};
    camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    camera.field_of_view = 0.14F;
    camera.near_plane = 0.05F;
    camera.far_plane = 400.0F;
    // The free view uses its own field of view, not a tracked lens calibration.
    camera.lens_profile.reset();
    return camera;
}

} // namespace mediaserver::editor
