#pragma once

#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <mediaserver/core/types.hpp>

#include <array>
#include <cstddef>

// The transform gizmo drawn over a 3D view: three axis arrows out of a pivot,
// one rotate ring per axis, scale boxes at the arrow ends, and the outer
// screen-space ring that turns about the view axis or scales uniformly. That
// is Blender's arrangement, which is what an operator who models already
// knows.
//
// Geometry, hit-testing and drawing only. Nothing here knows what is being
// manipulated: a caller hands over a pivot and a basis in world space, asks
// which handle a pointer is on, and draws. What a drag then resolves to lives
// beside this in transform_tool.hpp. Neither names a document, a selection or
// a scene graph, so a second host - ajfx Studio, placing props in a physics
// scene - gets this gizmo rather than a worse second one.
namespace mediaserver::editor {

// An axis arrow's length and how far from one a click still counts, both in
// panel pixels, so a gizmo is the same size however far away its pivot is.
constexpr float gizmo_length_pixels = 72.0F;
constexpr float gizmo_hit_pixels = 9.0F;
// The outer view ring, likewise fixed in pixels, outside the axis handles.
constexpr float view_ring_radius_pixels = 86.0F;
constexpr float view_ring_hit_pixels = 6.0F;
// The square every gizmo draws at its pivot.
constexpr float gizmo_hub_size = 6.0F;
// How many straight strokes a ring is drawn and hit-tested as.
constexpr std::size_t gizmo_ring_segments = 48U;

// The dark backing an overlay stroke sits on. A thin bright line over a wider
// near-black one reads over any picture - a white wall, a black-out, a busy
// clip - which is the only reason a one-pixel axis is findable at all.
constexpr gui::UiColor overlay_halo{0.0F, 0.02F, 0.03F, 0.88F};

// Which handle owns the current drag. Screen is the direct-on-object drag;
// X/Y/Z are the gizmo's axis arrows, rings or scale handles for the active
// tool; View is the outer screen-space ring that rotates about the view axis
// or scales uniformly.
enum class GizmoAxis : core::u8 {
    None,
    Screen,
    X,
    Y,
    Z,
    // Surface calibration has a dedicated, screen-space ring for rotation
    // about stage-up.
    Yaw,
    View,
};

// X, Y or Z as 0, 1 or 2. Only meaningful for those three.
[[nodiscard]] std::size_t axis_index(GizmoAxis axis) noexcept;

// Where a point in the authored canvas's pixels is drawn in `image`, the
// rectangle the canvas is shown in.
[[nodiscard]] gui::UiPoint canvas_to_image(const gui::Rect& image,
                                           scene::Canvas canvas, float x,
                                           float y) noexcept;
[[nodiscard]] gui::UiPoint to_ui(const gui::Rect& image, scene::Canvas canvas,
                                 const scene::CanvasProjection& point) noexcept;

// A `size` square centred on a point: a handle, a pivot, a measured point.
void square(gui::UiDrawList& list, gui::UiPoint centre, float size,
            gui::UiColor color);

// A stroke over a wider dark halo, so it reads over any picture.
void haloed_line(gui::UiDrawList& list, gui::UiPoint a, gui::UiPoint b,
                 float halo, float thickness, gui::UiColor color);

// The same over a dark square `border` wider on every side.
void haloed_square(gui::UiDrawList& list, gui::UiPoint centre, float size,
                   float border, gui::UiColor color);

// A circle of `segments` straight strokes round `centre`, each over its
// halo. Fixed in pixels, so a gizmo's rings stay one size at any distance.
void haloed_ring(gui::UiDrawList& list, gui::UiPoint centre, float radius,
                 std::size_t segments, float halo, float thickness,
                 gui::UiColor color);

// A world-space segment, drawn only while both ends are in front of the
// camera, and the same over its halo.
void projected_line(gui::UiDrawList& list, const gui::Rect& image,
                    const scene::Scene& view, scene::Vec3 a, scene::Vec3 b,
                    float thickness, gui::UiColor color);
void haloed_projected_line(gui::UiDrawList& list, const gui::Rect& image,
                           const scene::Scene& view, scene::Vec3 a,
                           scene::Vec3 b, float halo, float thickness,
                           gui::UiColor color);

// One gizmo projected for one frame: everything the hit tests and the drawing
// need, worked out once. `visible` is false when the pivot is behind the
// camera or any handle leaves the frustum, in which case nothing is drawn and
// nothing is hit - a half-projected gizmo would be a lie about where its axes
// point.
struct GizmoGeometry final {
    bool visible{};
    gui::UiPoint origin{};
    scene::Vec3 world_origin{};
    std::array<gui::UiPoint, 3U> ends{};
    std::array<float, 3U> canvas_dx{};
    std::array<float, 3U> canvas_dy{};
    std::array<scene::Vec3, 3U> world_axes{};
    float world_length{};
    // Rotate rings: one circle per axis in the plane it is normal to, and
    // whether each point faces the camera. Only the near half is a handle,
    // as in Blender - the far half sits behind the element.
    bool rings{};
    std::array<std::array<gui::UiPoint, gizmo_ring_segments + 1U>, 3U>
        ring_points{};
    std::array<std::array<bool, gizmo_ring_segments + 1U>, 3U> ring_front{};
};

// The three normalized columns of a world matrix: an element's own axes, which
// is what a local-frame gizmo points along. Falls back to the world basis
// axis-by-axis for a degenerate column.
[[nodiscard]] std::array<scene::Vec3, 3U> world_axes_of(
    const scene::Mat4& world) noexcept;

constexpr std::array<scene::Vec3, 3U> world_basis{
    scene::Vec3{1.0F, 0.0F, 0.0F}, scene::Vec3{0.0F, 1.0F, 0.0F},
    scene::Vec3{0.0F, 0.0F, 1.0F}};

// Projects a gizmo at `origin_world` with `axes` into `image`. Pass `rings`
// for the rotate tool, which needs the per-axis circles as well.
[[nodiscard]] GizmoGeometry gizmo_geometry(
    const gui::Rect& image, const scene::Scene& view,
    scene::Vec3 origin_world, const std::array<scene::Vec3, 3U>& axes,
    bool rings = false) noexcept;
// The same on the world basis: the global frame, the gizmo's default.
[[nodiscard]] GizmoGeometry gizmo_geometry(const gui::Rect& image,
                                           const scene::Scene& view,
                                           scene::Vec3 world) noexcept;

// Distance from a point to a segment. The first reaches a little past both
// ends, which is what an axis arrow wants: its arrowhead and the pixels just
// short of its pivot are part of the handle. The second is the exact segment,
// for a ring's many short chords.
[[nodiscard]] float distance_to_segment(float px, float py, gui::UiPoint a,
                                        gui::UiPoint b) noexcept;
[[nodiscard]] float distance_to_full_segment(float px, float py, gui::UiPoint a,
                                             gui::UiPoint b) noexcept;

// Which axis arrow a pointer is on, nearest first. An axis projecting to
// almost nothing - seen end-on - is skipped: it has no direction to drag
// along, and leaving it in would make a pointer near the pivot pick whichever
// edge-on axis happened to be closest.
[[nodiscard]] GizmoAxis hit_gizmo(const GizmoGeometry& gizmo, float pointer_x,
                                  float pointer_y) noexcept;
// The outer white ring Blender's rotate and scale gizmos share: rotation about
// the view axis, or uniform scale. Fixed in pixels, outside the axis handles.
[[nodiscard]] bool hit_view_ring(const GizmoGeometry& gizmo, float pointer_x,
                                 float pointer_y) noexcept;
// A rotate ring's near half, then the view ring.
[[nodiscard]] GizmoAxis hit_rotate_gizmo(const GizmoGeometry& gizmo,
                                         float pointer_x,
                                         float pointer_y) noexcept;
// A scale axis, then the view ring for uniform scale.
[[nodiscard]] GizmoAxis hit_scale_gizmo(const GizmoGeometry& gizmo,
                                        float pointer_x,
                                        float pointer_y) noexcept;

// The move gizmo: an arrow per axis, with the hovered or dragged one lit.
void draw_gizmo(gui::UiDrawList& list, const gui::Theme& theme,
                const GizmoGeometry& gizmo, GizmoAxis hovered,
                GizmoAxis active);
void draw_view_ring(gui::UiDrawList& list, const gui::Theme& theme,
                    const GizmoGeometry& gizmo, bool emphasized);
void draw_rotate_gizmo(gui::UiDrawList& list, const gui::Theme& theme,
                       const GizmoGeometry& gizmo, GizmoAxis hovered,
                       GizmoAxis active);
void draw_scale_gizmo(gui::UiDrawList& list, const gui::Theme& theme,
                      const GizmoGeometry& gizmo, GizmoAxis hovered,
                      GizmoAxis active);

}  // namespace mediaserver::editor
