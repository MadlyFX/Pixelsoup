#pragma once

#include <mediaserver/doc/ids.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <optional>

// Screen-space manipulation of a scene, in the operator's terms.
//
// In virtual production the direction you know is "camera left", not a
// world-space vector, so an edit is expressed as a drag in a view and resolved
// into a world-space move here. This layer is pure and headless: it takes a
// camera, a canvas, and pixels, and returns geometry - no GPU, no UI toolkit,
// no document. The editor shell drives it and turns its results into ordinary
// document commands; keeping the math here is what lets it be tested to the
// pixel with no window open.
//
// The load-bearing simplification: no unproject. Picking uses the forward
// projection the renderer already agrees with (`project_to_canvas`), and a drag
// is a *delta* - a translation parallel to the image plane at the object's
// depth - so it needs the camera basis and a depth-scaled pixel size, not the
// inverse of the projection matrix.
namespace mediaserver::scene {

// The world-space translation a screen drag maps to, at `world_point`'s depth,
// parallel to the image plane. A rightward drag moves the point along the
// camera's right axis ("camera left" is a leftward drag); a downward drag moves
// it down. `drag_x`/`drag_y` are in canvas pixels, y running down. Returns zero
// for a degenerate camera or a point at or behind the eye, where a screen drag
// has no depth to act at.
[[nodiscard]] Vec3 screen_drag_to_translation(
    const Camera& camera,
    Canvas canvas,
    Vec3 world_point,
    float drag_x,
    float drag_y) noexcept;

// The front-most media clip whose projected quad contains the canvas point, or
// null when the point hits none. Uses the same forward projection the renderer
// draws with, so what an operator clicks is what they see.
[[nodiscard]] const MediaInstance* pick_media(
    const Scene& scene,
    Canvas canvas,
    float canvas_x,
    float canvas_y) noexcept;

// The media clip a node produced this frame, or null. Lets the editor hold a
// selection by node id and find its current transform each frame.
[[nodiscard]] const MediaInstance* media_for(
    const Scene& scene,
    doc::NodeId node) noexcept;

// The world transform of any movable element a node produced this frame - a
// clip, object, model, or point light (whose position becomes a translation) -
// or nullopt. Lets the editor manipulate a selection by node id regardless of
// its kind. Returned by value because a point light has a position but no stored
// matrix. Directional lights have no position and are absent.
[[nodiscard]] std::optional<Mat4> world_for(
    const Scene& scene,
    doc::NodeId node) noexcept;

// The world-space translation component of a transform - an element's position.
[[nodiscard]] Vec3 translation_of(const Mat4& transform) noexcept;

} // namespace mediaserver::scene
