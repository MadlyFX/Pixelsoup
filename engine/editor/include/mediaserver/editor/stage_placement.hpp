#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/manipulate.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/raycast.hpp>

#include <optional>

// Where a Library asset dropped on the Stage lands: on the surface under the
// pointer, resting on it rather than half inside it.
//
// The rule is the one a set dresser expects. The new element sits on what the
// pointer is over - a set piece, a screen, a floor - with its bounds touching
// the surface and not crossing it. With nothing under the pointer it rests on
// the stage floor (y = 0, where the grid is drawn); looking up at empty sky it
// falls back to the depth of the camera's point of interest. A screen dropped
// on a wall hangs flat against it facing out; anywhere else it stands upright
// and turns to face the camera. Models keep their authored orientation.
namespace mediaserver::editor {

enum class StagePlacementSurface : core::u8 {
    Element,
    Floor,
    ViewPlane,
};

struct StagePlacementRequest final {
    // The pointer's line of sight, from `scene::canvas_ray`.
    scene::Ray ray{};
    // What it meets first, from `ViewportPanel::surface_at`.
    std::optional<scene::SurfaceHit> hit;
    scene::Camera camera{};
    // The new element's local bounds at its default scale. A media clip's
    // two-metre quad is `media_placement_bounds`; an unloaded model is a point.
    scene::ModelLocalBounds bounds{scene::Vec3{}, scene::Vec3{}};
    bool media{};
    // Snapping steps the contact point across an axis-aligned surface and a
    // screen's turn towards the camera, by the Stage's chosen increments.
    bool snap{};
    float snap_move{1.0F};
    scene::Turns snap_turns{5.0F / 360.0F};
};

struct StagePlacement final {
    // The new element's authored position and yaw/pitch/roll in turns.
    scene::Vec3 position{};
    scene::Vec3 rotation{};
    // Where it touches the surface, and the surface's direction there.
    scene::Vec3 contact{};
    scene::Vec3 normal{0.0F, 1.0F, 0.0F};
    StagePlacementSurface surface{StagePlacementSurface::ViewPlane};
    doc::NodeId element{};
    scene::ModelLocalBounds bounds{scene::Vec3{}, scene::Vec3{}};

    bool operator==(const StagePlacement&) const = default;
};

// The quad `make_added_node` gives a new media clip: two metres tall and as
// wide as the source's aspect ratio.
[[nodiscard]] scene::ModelLocalBounds
media_placement_bounds(float aspect) noexcept;

[[nodiscard]] std::optional<StagePlacement>
place_on_stage(const StagePlacementRequest& request) noexcept;

// The origin that rests `bounds` on the surface at `contact` facing `normal`:
// the corner deepest along the normal touches it. `linear` turns and scales
// the bounds as the element's world does; its translation is ignored.
[[nodiscard]] scene::Vec3
resting_origin(scene::Vec3 contact, scene::Vec3 normal,
               const scene::Mat4& linear,
               const scene::ModelLocalBounds& bounds) noexcept;

// `contact` stepped to `step` across a surface that faces along a world
// axis, and left alone along that axis; a sloped surface has no grid to
// step across and keeps it as it is.
[[nodiscard]] scene::Vec3 snap_contact(scene::Vec3 contact, scene::Vec3 normal,
                                       float step) noexcept;

// The placement's transform, for drawing its bounds where it will land.
[[nodiscard]] scene::Mat4 placement_world(const StagePlacement& placement) noexcept;

} // namespace mediaserver::editor
