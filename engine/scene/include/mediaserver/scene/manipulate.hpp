#pragma once

#include <mediaserver/doc/ids.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <functional>
#include <optional>
#include <vector>

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
[[nodiscard]] Vec3 screen_drag_to_translation(const Camera& camera,
                                              Canvas canvas, Vec3 world_point,
                                              float drag_x,
                                              float drag_y) noexcept;

// The front-most media clip whose projected quad contains the canvas point, or
// null when the point hits none. Uses the same forward projection the renderer
// draws with, so what an operator clicks is what they see.
[[nodiscard]] const MediaInstance* pick_media(const Scene& scene, Canvas canvas,
                                              float canvas_x,
                                              float canvas_y) noexcept;

// The result of editor scene picking. `position` is the point the transform
// gizmo anchors to; `depth` is reversed depth, so the larger value is nearer.
// Directional lights have an editor-only anchor derived from their direction.
struct ScenePick final {
    doc::NodeId node{};
    Vec3 position{};
    float depth{};
    // Part of the element is behind the camera: a floor, a wall, a room the
    // camera stands in. It is listed so a deep select can reach it, but it
    // never wins a plain click - on Stage that press belongs to the camera.
    // Only a pick that knows the camera (the Canvas forms) finds these.
    bool crosses_camera{};

    bool operator==(const ScenePick&) const = default;
};

// Local-space bounds supplied by a geometry owner without making Scene depend
// on that owner. The default is the historical unit-cube proxy, so callers
// that do not know imported geometry retain the existing picking behaviour.
struct ModelLocalBounds final {
    Vec3 minimum{-0.5F, -0.5F, -0.5F};
    Vec3 maximum{0.5F, 0.5F, 0.5F};

    bool operator==(const ModelLocalBounds&) const = default;
};

// Returns imported bounds for one evaluated model. Missing or malformed bounds
// deliberately fall back to ModelLocalBounds' unit cube.
using ModelBoundsProvider =
    std::function<std::optional<ModelLocalBounds>(const ModelInstance&)>;

// Projects world geometry into the pixels the operator is actually viewing.
// The default viewport uses Camera/Canvas; mapped Program outputs supply an
// asymmetric or UV-warp-aware projector through the same picking vocabulary.
using SceneProjector = std::function<CanvasProjection(Vec3)>;

[[nodiscard]] Vec3 screen_drag_to_translation(
    Vec3 world_point, Vec3 view_forward, Vec3 view_up,
    const SceneProjector& projector, float drag_x, float drag_y) noexcept;

// Picks all visible editor element kinds: procedural objects, media quads,
// imported models, and both light kinds. Geometry uses its projected unit
// bounds (the same local space the scene renderer instances), while light
// icons get a small canvas-pixel target. The front-most hit wins, skipping an
// element that crosses the camera.
[[nodiscard]] std::optional<ScenePick>
pick_scene(const Scene& scene, Canvas canvas, float canvas_x, float canvas_y,
           float light_radius = 12.0F, bool include_lights = true);

// Bounds-aware form for an editor or other geometry-owning caller. Scene stays
// importer-agnostic: the callback is the only seam across that boundary.
[[nodiscard]] std::optional<ScenePick>
pick_scene(const Scene& scene, Canvas canvas, float canvas_x, float canvas_y,
           const ModelBoundsProvider& model_bounds,
           float light_radius = 12.0F, bool include_lights = true);

// Every editor element under the canvas point, front-to-back. One result per
// document node, with node-id as a deterministic equal-depth tie breaker. This
// powers viewport deep select while `pick_scene` remains the nearest shortcut.
// The Canvas forms clip faces at the camera's near plane, so an element that
// crosses the camera is listed too, marked `crosses_camera`; the projector
// form has no camera to clip against and leaves such elements out.
[[nodiscard]] std::vector<ScenePick>
pick_scene_all(const Scene& scene, Canvas canvas, float canvas_x,
               float canvas_y, float light_radius = 12.0F,
               bool include_lights = true);

[[nodiscard]] std::vector<ScenePick>
pick_scene_all(const Scene& scene, Canvas canvas, float canvas_x,
               float canvas_y, const ModelBoundsProvider& model_bounds,
               float light_radius = 12.0F, bool include_lights = true);

[[nodiscard]] std::vector<ScenePick>
pick_scene_all(const Scene& scene, float canvas_x, float canvas_y,
               const SceneProjector& projector,
               const ModelBoundsProvider& model_bounds = {},
               float light_radius = 12.0F, bool include_lights = true);

// A stable world-space place to draw and pick a directional-light icon. A sun
// has direction but no authored position, so its marker sits near the world
// origin without following the camera or adding a transform to the scene.
[[nodiscard]] Vec3
directional_light_anchor(const Scene& scene,
                         const DirectionalLight& light) noexcept;

// The media clip a node produced this frame, or null. Lets the editor hold a
// selection by node id and find its current transform each frame.
[[nodiscard]] const MediaInstance* media_for(const Scene& scene,
                                             doc::NodeId node) noexcept;

// Whether an evaluated scene element is locked against editor selection and
// manipulation. Unknown nodes are not locked.
[[nodiscard]] bool element_locked(const Scene& scene,
                                  doc::NodeId node) noexcept;

// The world transform of any movable element a node produced this frame - a
// group, clip, object, model, or point light (whose position becomes a
// translation) - or nullopt. Lets the editor manipulate a selection by node
// id regardless of its kind. Returned by value because a point light has a
// position but no stored matrix. Directional lights have no position and are
// absent.
[[nodiscard]] std::optional<Mat4> world_for(const Scene& scene,
                                            doc::NodeId node) noexcept;

// The world-space translation component of a transform - an element's position.
[[nodiscard]] Vec3 translation_of(const Mat4& transform) noexcept;

} // namespace mediaserver::scene
