#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/scene/math.hpp>

// The canvas, the slice, and the projection that ties them together.
//
// This is the geometric half of "one scene, many outputs". The cohort agrees
// on a single image - the canvas - which no machine necessarily renders in
// full. Each node owns a rectangle of it and renders exactly that rectangle at
// its own output resolution.
//
// The camera is authored against the canvas and only against the canvas. A
// node never adjusts it. What a node does instead is narrow the camera's
// frustum to its own rectangle, which is an off-axis perspective projection:
// the near-plane window is a sub-rectangle of the full near-plane window,
// while the eye point and the near and far planes are untouched.
//
// Why that is the right construction rather than, say, rendering the whole
// canvas and cropping:
//
//  * The seam between two adjacent slices is bit-exact, not merely close. Two
//    nodes sharing an edge evaluate the identical float expression for it -
//    the same integer pixel column over the same integer canvas width - so
//    there is no rounding for a seam to open up in. The tests assert equality
//    of the shared plane, not proximity.
//  * The construction is exact rather than approximate: the slice frustum is a
//    subset of the canvas frustum. A world point therefore lands on the same
//    canvas pixel whichever frustum projects it, to within the rounding of two
//    different but algebraically equal matrix products - well under a
//    thousandth of a pixel, which the tests bound.
//  * Each node pays only for its own pixels. A four-node wall renders four
//    quarter-resolution frames, not four full ones.
//
// Nothing here knows about warp, soft-edge blend, or a calibration LUT. Those
// are the feed-mapping layer's job and sit downstream of this: they reshape
// the rendered slice on its way to a connector, while this decides what is in
// the slice in the first place. Overlapping slices are therefore legal and
// expected - a blend region is two nodes rendering the same canvas pixels -
// and nothing below rejects them.
namespace mediaserver::scene {

// The single image the cohort agrees on, in pixels. Every camera and every
// slice is expressed against this and nothing else.
struct Canvas final {
    core::u32 width{};
    core::u32 height{};

    bool operator==(const Canvas&) const = default;
};

// A node's rectangle of the canvas. Origin is the top-left of the canvas and
// +y runs down, which is the convention an operator laying out a wall in a
// feed map already has in their head.
struct CanvasRect final {
    core::u32 x{};
    core::u32 y{};
    core::u32 width{};
    core::u32 height{};

    bool operator==(const CanvasRect&) const = default;
};

// Right-handed, -Z forward. Authored against the canvas, never against a
// slice.
//
// The field of view is the full vertical angle in turns, and it applies to the
// canvas height. A slice narrower or shorter than the canvas therefore sees
// proportionally less, which is the whole point.
struct Camera final {
    Vec3 eye{0.0F, 0.0F, 0.0F};
    Vec3 target{0.0F, 0.0F, -1.0F};
    Vec3 up{0.0F, 1.0F, 0.0F};
    Turns field_of_view{0.125F}; // a quarter of a right angle: 45 degrees
    float near_plane{0.1F};
    float far_plane{1000.0F};

    bool operator==(const Camera&) const = default;
};

[[nodiscard]] bool valid(Canvas canvas) noexcept;

// True when the rectangle is non-empty and lies wholly inside the canvas.
[[nodiscard]] bool contains(Canvas canvas, CanvasRect slice) noexcept;

// True when the camera describes a frustum that can actually be built: a
// positive near plane strictly inside the far plane, a field of view inside
// half a turn, and an eye that is not sitting on its own target.
[[nodiscard]] bool valid(const Camera& camera) noexcept;

// The near-plane window a slice sees, in view space, plus the depth range.
//
// Exposed rather than kept inside the projection because it is the quantity
// two neighbouring slices have to agree on *exactly*, so it needs to be
// something a test can compare and a diagnostic can print. Frustum culling and
// the feed-mapping layer will both want it too.
struct FrustumWindow final {
    float left{};
    float right{};
    float bottom{};
    float top{};
    float near_plane{};
    float far_plane{};

    bool operator==(const FrustumWindow&) const = default;
};

// Returns a zeroed window for an invalid camera, canvas, or slice.
//
// Adjacent slices produce a bit-identical shared edge: node A's `right` is the
// same expression over the same integers as node B's `left`, because both are
// formed from the pixel boundary they share divided by the canvas width. There
// is no accumulation and no per-node origin, so there is nothing for a seam to
// open up in.
[[nodiscard]] FrustumWindow window_for_slice(
    const Camera& camera,
    Canvas canvas,
    CanvasRect slice) noexcept;

// The three matrices a view needs, kept together because computing one without
// the others is never what a caller wants and because the product is the thing
// actually uploaded.
struct ViewMatrices final {
    Mat4 view{};
    Mat4 projection{};
    Mat4 view_projection{};

    bool operator==(const ViewMatrices&) const = default;
};

// The projection for one slice of the canvas.
//
// Depth is reversed: the near plane maps to 1 and the far plane to 0. Floating
// point depth buffers have their precision bunched near zero, and reversing
// puts that precision where the geometry is far away and z-fighting would
// otherwise start. It costs nothing here and saves a rewrite later, so it is
// the default rather than an optimisation to reach for. The consequence for
// everything downstream is that the depth clear is 0 and the depth test is
// greater-than, not less-than.
//
// Returns identity matrices for an invalid camera, canvas, or slice. Callers
// validate first; this refuses to produce a matrix with an infinity in it.
[[nodiscard]] ViewMatrices view_for_slice(
    const Camera& camera,
    Canvas canvas,
    CanvasRect slice) noexcept;

// The whole canvas as a slice. The reference view: what a single machine
// rendering the entire image would use, and what the cohort's slices must
// reconstruct between them.
[[nodiscard]] CanvasRect whole(Canvas canvas) noexcept;

// Where a world point lands on the canvas, in canvas pixels, along with the
// reversed-depth value that decides what occludes it.
//
// `visible` is false when the point is at or behind the eye plane, where the
// perspective divide has no meaning. The pixel coordinates are then not
// usable; they are still returned so a caller can print them while debugging.
struct CanvasProjection final {
    float x{};
    float y{};
    float depth{};
    bool visible{};

    bool operator==(const CanvasProjection&) const = default;
};

// Projects through the full-canvas frustum. This is the reference answer that
// the per-slice path has to agree with.
[[nodiscard]] CanvasProjection project_to_canvas(
    const Camera& camera,
    Canvas canvas,
    Vec3 world_point) noexcept;

// Projects with an already-resolved view. This is the editor/picking seam for
// asymmetric physical-output cameras: reconstructing a symmetric Camera from
// an off-axis matrix would make the visible pixels and the hit geometry drift.
[[nodiscard]] CanvasProjection project_to_canvas(
    const ViewMatrices& matrices,
    Canvas canvas,
    Vec3 world_point) noexcept;

// Projects through one slice's frustum and converts the result back into
// canvas pixels. Agrees with `project_to_canvas` to floating-point precision
// for any point the slice can see, which is the property that makes a wall of
// slices one continuous image rather than a set of nearly-aligned ones.
[[nodiscard]] CanvasProjection project_through_slice(
    const Camera& camera,
    Canvas canvas,
    CanvasRect slice,
    Vec3 world_point) noexcept;

} // namespace mediaserver::scene
