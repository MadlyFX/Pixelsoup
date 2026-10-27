#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/scene/canvas.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace mediaserver::editor {

// How a Stage view projects. Top, Front and Side are orthographic and look
// straight down -Y, along -Z and along -X; they keep the orbit's yaw and pitch
// untouched, so returning to Perspective finds the view as it was left.
enum class StageProjection : core::u8 {
    Perspective,
    Top,
    Front,
    Side,
};

[[nodiscard]] constexpr bool stage_orthographic(
    StageProjection projection) noexcept {
    return projection != StageProjection::Perspective;
}

// The Stage's free view: a pivot, the direction the eye sits in from it, and
// how far away. Every navigation gesture is a change to these values, so
// orbiting after flying turns about a point straight ahead of the eye. In an
// orthographic view `distance` is the zoom: the view shows the height a
// perspective view would show at the pivot from that far away.
struct StageOrbit final {
    scene::Vec3 target{};
    scene::Turns yaw{0.10F};
    scene::Turns pitch{0.06F};
    float distance{9.0F};
    StageProjection projection{StageProjection::Perspective};

    bool operator==(const StageOrbit&) const = default;
};

inline constexpr scene::Turns stage_field_of_view = 0.14F;
// Short of straight up or down, where yaw would lose its meaning.
inline constexpr scene::Turns stage_pitch_limit = 0.24F;
inline constexpr float stage_minimum_distance = 0.5F;
inline constexpr float stage_maximum_distance = 500.0F;
// Turning rate for every rotating gesture, in turns per pixel of travel.
inline constexpr float stage_turn_per_pixel = 0.0015F;
// How far behind the pivot an orthographic view's eye stands, so geometry on
// either side of the pivot along the view stays between its near and far
// planes. Half the Stage's far plane: nothing within 200 m is clipped.
inline constexpr float stage_orthographic_standoff = 200.0F;
// Fly speed in scene units per second, as the right-drag wheel sets it.
inline constexpr float stage_default_fly_speed = 4.0F;
inline constexpr float stage_minimum_fly_speed = 0.05F;
inline constexpr float stage_maximum_fly_speed = 200.0F;

// Where the eye sits relative to the pivot: `distance` along yaw and pitch.
[[nodiscard]] inline scene::Vec3 stage_eye_offset(
    const StageOrbit& orbit) noexcept {
    const float cp = scene::deterministic::cos_turns(orbit.pitch);
    const float sp = scene::deterministic::sin_turns(orbit.pitch);
    const float cy = scene::deterministic::cos_turns(orbit.yaw);
    const float sy = scene::deterministic::sin_turns(orbit.yaw);
    return scene::Vec3{orbit.distance * cp * sy, orbit.distance * sp,
                       orbit.distance * cp * cy};
}

// The direction a Stage view looks along, and its screen right and up.
struct StageViewAxes final {
    scene::Vec3 forward{};
    scene::Vec3 right{};
    scene::Vec3 up{};
};

[[nodiscard]] inline StageViewAxes stage_view_axes(
    const StageOrbit& orbit) noexcept {
    switch (orbit.projection) {
    case StageProjection::Top:
        // North (-Z) at the top of the screen, +X to the right.
        return {{0.0F, -1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}};
    case StageProjection::Front:
        return {{0.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}};
    case StageProjection::Side:
        return {{-1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, {0.0F, 1.0F, 0.0F}};
    case StageProjection::Perspective:
        break;
    }
    const scene::Vec3 forward =
        -stage_eye_offset(orbit) * (1.0F / orbit.distance);
    const scene::Vec3 right{scene::deterministic::cos_turns(orbit.yaw), 0.0F,
                            -scene::deterministic::sin_turns(orbit.yaw)};
    return {forward, right, scene::cross(right, forward)};
}

[[nodiscard]] inline scene::Vec3 stage_eye(const StageOrbit& orbit) noexcept {
    if (stage_orthographic(orbit.projection)) {
        return orbit.target -
               stage_view_axes(orbit).forward * stage_orthographic_standoff;
    }
    return orbit.target + stage_eye_offset(orbit);
}

// The world height an orthographic Stage view shows: what the perspective view
// shows at its pivot, so switching between them keeps the scale on screen.
[[nodiscard]] inline float stage_orthographic_height(
    const StageOrbit& orbit) noexcept {
    return 2.0F * orbit.distance *
           scene::deterministic::tan_half_turns(stage_field_of_view);
}

// Orbit changes the viewpoint, not the scene's photographic exposure. Rebuild
// from the evaluated Stage camera every frame so animated exposure and scene
// switches remain consistent with the image that will go to Program.
[[nodiscard]] inline scene::Camera stage_camera_for(
    const scene::Camera& authored, const StageOrbit& orbit) noexcept {
    scene::Camera camera = authored;
    camera.target = orbit.target;
    camera.eye = stage_eye(orbit);
    camera.up = stage_orthographic(orbit.projection)
                    ? stage_view_axes(orbit).up
                    : scene::Vec3{0.0F, 1.0F, 0.0F};
    camera.field_of_view = stage_field_of_view;
    camera.near_plane = 0.05F;
    camera.far_plane = 400.0F;
    // The free view uses its own field of view, not a tracked lens calibration.
    camera.lens_profile.reset();
    camera.orthographic_height = stage_orthographic(orbit.projection)
                                     ? stage_orthographic_height(orbit)
                                     : 0.0F;
    return camera;
}

// The Unreal Editor's perspective-viewport mouse navigation, chosen from the
// buttons and modifiers held right now so a gesture can change mid-drag the
// way it does there (adding the left button to a right drag pans).
enum class StageNavigation : core::u8 {
    None,
    // Left drag on bare canvas: forward and back across the ground, turning.
    Walk,
    // Right drag: turn in place. W A S D Q E fly while it is held.
    Look,
    // Middle drag or Alt+left drag: turn about the pivot.
    Orbit,
    // Left+right drag: the camera moves the way the mouse does.
    Pan,
    // Alt+middle or Shift+middle drag: the scene follows the mouse.
    Track,
    // Alt+right drag: toward and away from the pivot.
    Dolly,
};

struct StageNavigationButtons final {
    bool left{};
    bool middle{};
    bool right{};
    bool alt{};
    bool shift{};
};

[[nodiscard]] constexpr StageNavigation stage_navigation_for(
    StageNavigationButtons held) noexcept {
    if (held.alt) {
        if (held.right) {
            return StageNavigation::Dolly;
        }
        if (held.middle) {
            return StageNavigation::Track;
        }
        return held.left ? StageNavigation::Orbit : StageNavigation::None;
    }
    if (held.left && held.right) {
        return StageNavigation::Pan;
    }
    if (held.middle) {
        return held.shift ? StageNavigation::Track : StageNavigation::Orbit;
    }
    if (held.right) {
        return StageNavigation::Look;
    }
    return held.left ? StageNavigation::Walk : StageNavigation::None;
}

// One frame of navigation input. Pointer travel is raw motion in logical
// pixels (positive right and down); `move` is the held fly keys as camera
// axes - x right (D/A), y world up (E/Q), z forward (W/S).
struct StageNavigationStep final {
    StageNavigation gesture{StageNavigation::None};
    float dx{};
    float dy{};
    scene::Vec3 move{};
    float seconds{};
    float fly_speed{stage_default_fly_speed};
    // World units one pixel spans at the pivot's depth, so a pan or track keeps
    // whatever is at the pivot under the pointer.
    float units_per_pixel{};
};

// World units per viewport pixel at the pivot, for an image `height` pixels
// tall. The same expression is an orthographic view's scale everywhere.
[[nodiscard]] inline float stage_units_per_pixel(const StageOrbit& orbit,
                                                 float height) noexcept {
    if (!(height > 0.0F)) {
        return 0.0F;
    }
    return 2.0F * orbit.distance *
           scene::deterministic::tan_half_turns(stage_field_of_view) / height;
}

// Moves the eye along the view by `amount` units, carrying the pivot ahead
// once the eye reaches it, so the wheel can keep travelling forward.
// An orthographic view zooms instead: it has nowhere to travel to.
[[nodiscard]] inline StageOrbit stage_dolly(StageOrbit orbit,
                                            float amount) noexcept {
    if (stage_orthographic(orbit.projection)) {
        orbit.distance = std::clamp(orbit.distance - amount,
                                    stage_minimum_distance,
                                    stage_maximum_distance);
        return orbit;
    }
    const scene::Vec3 forward = -stage_eye_offset(orbit) * (1.0F / orbit.distance);
    const float distance = orbit.distance - amount;
    if (distance < stage_minimum_distance) {
        orbit.target = orbit.target + forward * (stage_minimum_distance - distance);
        orbit.distance = stage_minimum_distance;
    } else {
        orbit.distance = std::min(distance, stage_maximum_distance);
    }
    return orbit;
}

// One wheel notch outside a fly: a tenth of the way to the pivot, at least a
// tenth of a unit.
[[nodiscard]] inline StageOrbit stage_wheel_dolly(StageOrbit orbit,
                                                  float wheel) noexcept {
    return stage_dolly(orbit, wheel * std::max(orbit.distance * 0.1F, 0.1F));
}

// The wheel during a right-drag fly sets its speed, each notch a quarter.
[[nodiscard]] inline float stage_fly_speed_after(float speed,
                                                 float wheel) noexcept {
    return std::clamp(speed * std::pow(1.25F, wheel), stage_minimum_fly_speed,
                      stage_maximum_fly_speed);
}

// An axis-aligned world-space box, as the Stage frames a selection.
// The Stage floor grid's spacing for a view `reach` metres from what it looks
// at: a power of ten, about a quarter of the reach, so zooming out widens the
// grid a decade at a time and a close view gets centimetres.
[[nodiscard]] inline float stage_grid_step(float reach) noexcept {
    constexpr std::array<float, 6U> steps{0.01F, 0.1F, 1.0F,
                                          10.0F, 100.0F, 1000.0F};
    std::size_t chosen = 0U;
    while (chosen + 1U < steps.size() && steps[chosen + 1U] <= reach * 0.25F) {
        ++chosen;
    }
    return steps[chosen];
}

struct WorldBounds final {
    scene::Vec3 minimum{};
    scene::Vec3 maximum{};
};

// F: the pivot moves to the middle of `bounds` and the eye backs off along the
// current view until the whole box fits, whichever way it is turned.
[[nodiscard]] inline StageOrbit stage_focus(StageOrbit orbit,
                                            const WorldBounds& bounds) noexcept {
    const scene::Vec3 half = (bounds.maximum - bounds.minimum) * 0.5F;
    const float radius = std::max(scene::length(half), 0.25F);
    orbit.target = bounds.minimum + half;
    // An orthographic view only has to show the box across its height, not
    // keep it inside a cone.
    const float reach =
        stage_orthographic(orbit.projection)
            ? radius * 1.1F /
                  scene::deterministic::tan_half_turns(stage_field_of_view)
            : radius * 1.1F /
                  scene::deterministic::sin_turns(stage_field_of_view * 0.5F);
    orbit.distance =
        std::clamp(reach, stage_minimum_distance, stage_maximum_distance);
    return orbit;
}

// An orthographic view pans and zooms; it never turns. Every drag carries the
// scene with the pointer, Alt+right drag and the wheel zoom, and the fly keys
// pan (A D, Q E) and zoom (W S).
[[nodiscard]] inline StageOrbit navigate_stage_orthographic(
    StageOrbit orbit, const StageNavigationStep& step) noexcept {
    const StageViewAxes axes = stage_view_axes(orbit);
    switch (step.gesture) {
    case StageNavigation::None:
        return orbit;
    case StageNavigation::Dolly:
        return stage_dolly(orbit,
                           (step.dx - step.dy) * orbit.distance * 0.005F);
    case StageNavigation::Look:
        orbit.target = orbit.target +
                       (axes.right * step.move.x + axes.up * step.move.y) *
                           (step.fly_speed * step.seconds);
        orbit = stage_dolly(orbit, step.move.z * step.fly_speed * step.seconds);
        [[fallthrough]];
    case StageNavigation::Walk:
    case StageNavigation::Orbit:
    case StageNavigation::Pan:
    case StageNavigation::Track:
        orbit.target = orbit.target +
                       axes.right * (-step.dx * step.units_per_pixel) +
                       axes.up * (step.dy * step.units_per_pixel);
        return orbit;
    }
    return orbit;
}

[[nodiscard]] inline StageOrbit navigate_stage(
    StageOrbit orbit, const StageNavigationStep& step) noexcept {
    if (stage_orthographic(orbit.projection)) {
        return navigate_stage_orthographic(orbit, step);
    }
    const float cy = scene::deterministic::cos_turns(orbit.yaw);
    const float sy = scene::deterministic::sin_turns(orbit.yaw);
    const scene::Vec3 forward =
        -stage_eye_offset(orbit) * (1.0F / orbit.distance);
    const scene::Vec3 right{cy, 0.0F, -sy};
    const scene::Vec3 up = scene::cross(right, forward);
    // Turning about the eye keeps it where it is and swings the pivot round;
    // turning about the pivot carries the eye.
    const auto turn = [&](float dx, float dy, bool about_eye) {
        const scene::Vec3 eye = stage_eye(orbit);
        orbit.yaw -= dx * stage_turn_per_pixel;
        orbit.pitch = std::clamp(orbit.pitch + dy * stage_turn_per_pixel,
                                 -stage_pitch_limit, stage_pitch_limit);
        if (about_eye) {
            orbit.target = eye - stage_eye_offset(orbit);
        }
    };
    switch (step.gesture) {
    case StageNavigation::None:
        break;
    case StageNavigation::Walk: {
        // Pushing the mouse away walks forward along the ground.
        const scene::Vec3 ground{-sy, 0.0F, -cy};
        orbit.target = orbit.target +
                       ground * (-step.dy * step.fly_speed * 0.0125F);
        turn(step.dx, 0.0F, true);
        break;
    }
    case StageNavigation::Look: {
        turn(step.dx, step.dy, true);
        // Fly along the view as it is after this frame's turn.
        const scene::Vec3 look =
            -stage_eye_offset(orbit) * (1.0F / orbit.distance);
        const scene::Vec3 strafe{scene::deterministic::cos_turns(orbit.yaw),
                                 0.0F,
                                 -scene::deterministic::sin_turns(orbit.yaw)};
        const scene::Vec3 travel = strafe * step.move.x +
                                   scene::Vec3{0.0F, 1.0F, 0.0F} * step.move.y +
                                   look * step.move.z;
        orbit.target = orbit.target + travel * (step.fly_speed * step.seconds);
        break;
    }
    case StageNavigation::Orbit:
        turn(step.dx, step.dy, false);
        break;
    case StageNavigation::Pan:
        orbit.target = orbit.target + right * (step.dx * step.units_per_pixel) +
                       up * (-step.dy * step.units_per_pixel);
        break;
    case StageNavigation::Track:
        orbit.target = orbit.target + right * (-step.dx * step.units_per_pixel) +
                       up * (step.dy * step.units_per_pixel);
        break;
    case StageNavigation::Dolly:
        orbit = stage_dolly(orbit, (step.dx - step.dy) * orbit.distance * 0.005F);
        break;
    }
    return orbit;
}

} // namespace mediaserver::editor
