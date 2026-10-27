#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/scene/lens.hpp>
#include <mediaserver/scene/math.hpp>

#include <array>
#include <optional>
#include <span>

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
    // Optional physical calibration. Its absence preserves the historic
    // turn-based camera path exactly; a calibrated profile is used by the
    // tracked inner-frustum path and never pre-distorts the rendered image.
    std::optional<LensProfile> lens_profile;
    float near_plane{0.1F};
    float far_plane{1000.0F};
    // Physical exposure controls are authored values. The Basic renderer may
    // ignore them, but carrying them in the replicated scene means an
    // Advanced renderer never has to infer exposure from local settings.
    float aperture_f_number{2.8F};
    // Physical focus fallback in metres; zero means unavailable.
    float focus_distance_m{};
    float shutter_seconds{1.0F / 48.0F};
    float iso{100.0F};
    float exposure_compensation_ev{};
    // Zero for the ordinary perspective camera. Positive makes the projection
    // orthographic: this is the world height the canvas spans, at every depth,
    // and the field of view is not used for the image. No document property
    // carries it - an authored camera is always perspective, so every node
    // evaluates zero here - and only the editor's Stage views set it. It is in
    // the scene digest all the same, and a renderer asked to draw a scene whose
    // own camera is orthographic refuses rather than guess (an orthographic
    // Stage view reaches the renderer as an explicit view instead).
    float orthographic_height{};

    bool operator==(const Camera&) const = default;
};

// True when the camera projects orthographically (see `orthographic_height`).
[[nodiscard]] constexpr bool orthographic(const Camera& camera) noexcept {
    return camera.orthographic_height > 0.0F;
}

// True when a projection matrix is orthographic: a constant clip w of one.
// What a renderer handed only matrices tests, because the passes that care
// (atmosphere, view-dependent shading) must agree with what rasterised.
[[nodiscard]] bool orthographic(const Mat4& projection) noexcept;

// World units one canvas pixel spans, `depth` units in front of the eye along
// the view: the same at every depth for an orthographic camera, growing with
// depth for a perspective one. Zero for an unusable camera or canvas height.
[[nodiscard]] float units_per_canvas_pixel(const Camera& camera, float depth,
                                           float canvas_height) noexcept;

// How far ahead of its eye a placed camera's target sits. A camera is aimed
// by its rotation; the target only names the view direction (and gives the
// tools that want a point of interest, such as a sun's gizmo, one).
inline constexpr float camera_aim_distance_m = 5.0F;

// Places a camera the way every other element is placed: at `position`,
// turned by `rotation(yaw, pitch, roll)` from looking down -Z with +Y up.
// Sets the eye, target and up vector; every other field is left alone.
void place_camera(Camera& camera, Vec3 position, Turns yaw, Turns pitch,
                  Turns roll) noexcept;

// The yaw, pitch and roll (turns, in that order) that `place_camera` needs to
// reproduce this camera's view direction and up vector. A degenerate camera
// reads as zero rotation.
[[nodiscard]] Vec3 camera_rotation(const Camera& camera) noexcept;

// The rigid placement `place_camera` describes, as a matrix: translation to
// the eye, +Y the camera's up and -Z where it looks. Unit scale, so the gizmo
// and a transform gesture see a camera exactly as they see any element.
[[nodiscard]] Mat4 camera_world(const Camera& camera) noexcept;

// Relative photographic exposure used by the Advanced renderer. The default
// camera is neutral (1.0), preserving today's unitless-light look while
// aperture, shutter duration, ISO, and compensation gain their proper stop
// relationships. The result is bounded to +/-24 stops and always finite.
[[nodiscard]] float camera_exposure_multiplier(const Camera& camera) noexcept;

[[nodiscard]] bool valid(Canvas canvas) noexcept;

// True when the rectangle is non-empty and lies wholly inside the canvas.
[[nodiscard]] bool contains(Canvas canvas, CanvasRect slice) noexcept;

// True when the camera describes a frustum that can actually be built: a
// positive near plane strictly inside the far plane, a field of view inside
// half a turn, and an eye that is not sitting on its own target. An
// orthographic height must be finite and not negative, and an orthographic
// camera has no lens profile: a calibrated lens is a perspective device.
[[nodiscard]] bool valid(const Camera& camera) noexcept;

// The near-plane window a slice sees, in view space, plus the depth range.
// For an orthographic camera the window is the same rectangle at every depth.
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

// A sub-pixel offset in **canvas** pixels, each component in [-0.5, 0.5).
//
// Canvas pixels rather than slice or target pixels: a node rendering a slice
// has to land on the same sub-pixel position the full-canvas render would have
// used, or two nodes sample the same show frame at different points and their
// temporal histories converge on different images. Everything that converts
// this to projection units therefore divides by the canvas extent, which every
// node agrees on, and never by its own.
struct SubpixelJitter final {
    float x{};
    float y{};

    bool operator==(const SubpixelJitter&) const = default;
};

// The radical inverse of `index` in `base` - the Halton sequence.
//
// Written out rather than taken from a library so the operation order is
// fixed: every node evaluates the same multiply-accumulate chain in the same
// sequence and gets the same bits. Bases below two return zero rather than
// looping forever.
[[nodiscard]] float halton(core::u64 index, core::u32 base) noexcept;

// The jitter for one show frame, Halton(2, 3) over a sequence of
// `sequence_length` samples.
//
// Derived from the exact 64-bit show frame and nothing else - no frame
// counter, no wall clock, no per-node index - because those are the values
// that differ between nodes rendering the same frame. A repeated show frame is
// a hold, and a hold must reuse the offset rather than take the next sample;
// deriving from the show frame alone makes that true by construction instead
// of by a branch that could be forgotten.
//
// The sequence is offset by one so it starts at Halton's first sample rather
// than its degenerate zero, which would put the first frame of every sequence
// at the corner of the pixel instead of inside it. No sample is the exact
// centre - base two's first term is a half and base three's is a third - and
// that is correct: a temporal renderer wants distinct sub-pixel positions, and
// a project with temporal work switched off applies no jitter at all rather
// than relying on one sample happening to be zero.
[[nodiscard]] SubpixelJitter jitter_for_frame(
    core::u64 show_frame, core::u32 sequence_length) noexcept;

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

// The lens-aware form uses sensor dimensions and focal length rather than the
// turn-based field of view. It remains an undistorted projection: distortion
// affects coverage through `distorted_window_for_lens`, never the pixels sent
// to the wall.
[[nodiscard]] FrustumWindow window_for_slice(
    const Camera& camera,
    Canvas canvas,
    CanvasRect slice,
    const LensProfile& profile,
    const LensEncoders& encoders) noexcept;

// The jittered form. The offset is converted to projection units using the
// canvas extent and added to both edges of each axis, so the window is shifted
// rather than reshaped and the shared edge between two slices stays the same
// expression over the same integers. A zero jitter reproduces the unjittered
// window exactly.
[[nodiscard]] FrustumWindow window_for_slice(
    const Camera& camera,
    Canvas canvas,
    CanvasRect slice,
    SubpixelJitter jitter) noexcept;

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
// An orthographic camera gets the orthographic form of the same contract:
// clip w is one, depth is linear in view distance, still near 1 and far 0,
// and a slice is the same fraction of the canvas window as in perspective.
//
// Returns identity matrices for an invalid camera, canvas, or slice. Callers
// validate first; this refuses to produce a matrix with an infinity in it.
[[nodiscard]] ViewMatrices view_for_slice(
    const Camera& camera,
    Canvas canvas,
    CanvasRect slice) noexcept;
[[nodiscard]] ViewMatrices view_for_slice(
    const Camera& camera,
    Canvas canvas,
    CanvasRect slice,
    const LensProfile& profile,
    const LensEncoders& encoders) noexcept;

// The jittered form, for a temporally accumulating renderer.
[[nodiscard]] ViewMatrices view_for_slice(
    const Camera& camera,
    Canvas canvas,
    CanvasRect slice,
    SubpixelJitter jitter) noexcept;

// The view a spot light renders its shadow map from.
//
// A symmetric perspective down the cone axis, with the full cone angle as the
// field of view - the shadow map covers exactly what the light can reach, so
// none of the tile is spent on geometry the cone excludes anyway.
//
// Derived from the authored light alone. Nothing about the camera, the slice
// or the node enters it, which is what lets every node in a cohort rasterize
// the same map into the same tile.
[[nodiscard]] ViewMatrices spot_light_view(
    Vec3 position,
    Vec3 direction,
    Turns outer_cone,
    float near_plane,
    float far_plane) noexcept;

// The jittered form of an already-resolved viewpoint, for an output that
// builds its own asymmetric projection instead of taking a canvas slice.
//
// The offset is applied in that projection's own target pixels, which is the
// sub-pixel position its own antialiasing needs. The canvas-slice form above
// is the one to use whenever there is a canvas to be relative to; this exists
// for the physical and mapped views that have already resolved a window and
// have only a matrix left.
[[nodiscard]] ViewMatrices jittered(
    const ViewMatrices& matrices,
    SubpixelJitter jitter,
    core::u32 width,
    core::u32 height) noexcept;

// The near-plane window of a camera with an explicit aspect ratio, and the
// projection built from it.
//
// The canvas forms are the right ones for anything drawn onto the wall. These
// exist for the tracked camera, whose aspect is a physical sensor and has
// nothing to do with the shape of the canvas: asking `window_for_slice` for it
// would mean inventing a canvas with the sensor's proportions. The projection
// is built by the same expression as the slice form, so the two cannot drift.
//
// Returns a zeroed window and identity matrices for an unusable camera or a
// non-positive aspect ratio.
[[nodiscard]] FrustumWindow window_for_aspect(
    const Camera& camera, float aspect_ratio) noexcept;
[[nodiscard]] ViewMatrices view_for_aspect(
    const Camera& camera, float aspect_ratio) noexcept;

// Whole-sensor windows for a calibrated lens. The first is the projection used
// for rendering. The second is deliberately conservative and is used only to
// size the inner-frustum coverage for a lens that distorts what it sees.
[[nodiscard]] FrustumWindow window_for_lens(
    const Camera& camera,
    const LensProfile& profile,
    const LensEncoders& encoders) noexcept;
[[nodiscard]] FrustumWindow distorted_window_for_lens(
    const Camera& camera,
    const LensProfile& profile,
    const LensEncoders& encoders) noexcept;
[[nodiscard]] ViewMatrices view_for_lens(
    const Camera& camera,
    const LensProfile& profile,
    const LensEncoders& encoders) noexcept;
[[nodiscard]] ViewMatrices view_for_distorted_lens(
    const Camera& camera,
    const LensProfile& profile,
    const LensEncoders& encoders) noexcept;

// The whole canvas as a slice. The reference view: what a single machine
// rendering the entire image would use, and what the cohort's slices must
// reconstruct between them.
[[nodiscard]] CanvasRect whole(Canvas canvas) noexcept;

// Where a world point lands on the canvas, in canvas pixels, along with the
// reversed-depth value that decides what occludes it.
//
// `visible` is false when the point is at or behind the eye plane, where the
// perspective divide has no meaning; an orthographic projection has no divide
// and reports every point visible. The pixel coordinates are then not
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

// True when two rectangles share at least one pixel. Rectangles are half-open
// in x and y, so two slices that merely abut do not intersect.
[[nodiscard]] bool intersects(CanvasRect a, CanvasRect b) noexcept;

// The overlap of two rectangles, or a zero-size rectangle when they do not
// overlap. Computed in 64-bit so an out-of-canvas rectangle cannot wrap.
[[nodiscard]] CanvasRect intersection(CanvasRect a, CanvasRect b) noexcept;

// The inner frustum: the region of the canvas a tracked camera actually sees.
//
// The whole of ICVFX rests on this rectangle. Inside it the wall shows the
// scene rendered from the tracked camera's real viewpoint, and the physical
// camera therefore records correct parallax. Outside it the wall shows the
// authored view, which the camera does not see directly but which lights the
// subject and appears in every reflection on the stage.
//
// Every node in the cohort must derive a *bit-identical* rectangle from the
// replicated pose, or two nodes will disagree about where the plate ends and
// there will be a hard vertical cut down the middle of a take. So the
// derivation obeys the same discipline as `window_for_slice`: replicated
// inputs only, one expression, no accumulation, no per-node origin.
//
// Rounding is outward, and that direction is not arbitrary. A rectangle one
// pixel too large wastes a pixel column of inner-frustum render. A rectangle
// one pixel too small puts outer-frustum content inside the camera's view,
// which is a hard cut in the middle of the plate and unusable footage.

// The eight world-space corners of a camera's view frustum: the four on the
// near plane first, then the four on the far plane. Within each group the
// order is lower-left, lower-right, upper-left, upper-right.
//
// `aspect_ratio` is width over height. It is passed rather than derived from a
// canvas because the tracked camera's aspect is the physical camera's sensor,
// which has nothing to do with the shape of the wall.
struct FrustumCorners final {
    std::array<Vec3, 8U> points{};
    bool valid{};

    bool operator==(const FrustumCorners&) const = default;
};

[[nodiscard]] FrustumCorners frustum_corners(
    const Camera& camera, float aspect_ratio) noexcept;

// Builds corners for any explicit off-axis window. This is the shared seam for
// ordinary aspect cameras and calibrated distorted-coverage cameras.
[[nodiscard]] FrustumCorners frustum_corners(
    const Camera& camera, const FrustumWindow& window) noexcept;
[[nodiscard]] FrustumCorners frustum_corners_for_lens(
    const Camera& camera,
    const LensProfile& profile,
    const LensEncoders& encoders,
    bool distorted_coverage = false) noexcept;

// A derived inner-frustum rectangle and what had to be done to it.
struct InnerFrustumRegion final {
    CanvasRect rect{};
    // False when no usable rectangle exists: an invalid canvas, no projected
    // point in front of the eye, or a region that fell entirely outside the
    // canvas. A caller with an invalid region renders the outer frustum alone.
    bool valid{};
    // The region reached a canvas edge and was cut there. Expected whenever
    // the camera looks past the end of the wall; carried so a diagnostic can
    // distinguish it from a region that simply happens to be small.
    bool clamped{};

    bool operator==(const InnerFrustumRegion&) const = default;
};

// The outward-rounded canvas rectangle covering every visible projected point,
// grown by `margin_px` on each side and clipped to the canvas.
//
// Points behind the eye plane are ignored rather than clamped: their canvas
// coordinates are meaningless, and folding one in would produce a rectangle
// derived from a number that is not a position. When every point is behind the
// eye the region is invalid, which is the honest answer for a tracked camera
// pointing away from the wall.
//
// The margin covers what the projected corners alone do not: lens distortion
// pushing the real image outside the ideal frustum, one frame of tracker
// latency, and the soft edge the composite needs to hide the transition. It is
// authored, replicated, and identical on every node.
[[nodiscard]] InnerFrustumRegion inner_frustum_rect(
    Canvas canvas,
    std::span<const CanvasProjection> projected,
    core::u32 margin_px) noexcept;

} // namespace mediaserver::scene
