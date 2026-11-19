#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/output/perspective_mapping.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/scene.hpp>

#include <optional>

// Inner/outer frustum, from the renderer's side.
//
// The wall shows two things at once. Everywhere the physical camera cannot see,
// it shows the authored view - the **outer frustum** - which the camera does
// not record directly but which lights the subject and appears in every
// reflection on the stage. Inside the rectangle the camera *can* see, it shows
// the **inner frustum**: the same scene rendered from the tracked camera's real
// viewpoint, so the geometry shifts against itself as the camera moves and the
// wall reads as a set rather than a picture of one.
//
// Two things live here. `IcvfxPlan` is the per-node derivation: what rectangle,
// which view, and - the part that pays for the whole feature - whether this
// node has to render the inner frustum at all. `IcvfxCompositor` is the GPU
// pass that puts the second render over the first.
//
// The plan is pure, headless, and derived only from replicated state. That is
// not tidiness: a node that derived any part of it from its own slice would
// disagree with its neighbour about where the plate ends, and the disagreement
// would land as a hard cut down the middle of a take.
namespace mediaserver::render {

// What one node must do about one camera's inner frustum this frame.
struct IcvfxPlan final {
    // The camera this plan is for, so a node compositing several plates can
    // never mix up which target belongs to which.
    doc::NodeId camera{};
    // False when this camera's feed is not live, the show or the camera
    // authored no inner frustum, or the tracked camera sees no part of the
    // surface. The node then renders the outer frustum alone here, exactly as
    // it did before ICVFX existed.
    bool active{};
    // The inner-frustum rectangle in absolute pixels of the surface raster
    // this output shows - the same space as `slice` and as the region the
    // caller derived. Identical on every node in the cohort, bit for bit.
    //
    // Surface raster and not scene canvas: the rectangle is found by
    // projecting the tracked frustum onto a physical wall, and a wall carries
    // its own raster which a multi-wall show has no reason to make equal to
    // the canvas.
    scene::CanvasRect inner_rect{};
    // The part of `inner_rect` that falls inside this node's slice. Empty when
    // the node does not see the inner frustum at all.
    scene::CanvasRect covered{};
    // True when this node's slice misses the inner rectangle entirely, so it
    // skips the second render. This is the performance argument for ICVFX and
    // it is a rectangle intersection over replicated values, never a heuristic.
    bool skipped{};
    // The rectangle reached a raster edge. Expected whenever the camera looks
    // past the end of the wall.
    bool clamped{};
    // Render target extent for the inner frustum, after the authored
    // resolution scale. Zero when inactive or skipped.
    core::u32 inner_width{};
    core::u32 inner_height{};
    // Feather width in surface-raster pixels, already clamped against the
    // rectangle.
    core::u32 soft_edge_px{};

    bool operator==(const IcvfxPlan&) const = default;
};

// The largest inner-frustum target a node will allocate, in pixels on a side.
// Renderers allocate at construction, so a plan past this bound is refused and
// counted rather than absorbed into an allocation mid-recording.
inline constexpr core::u32 max_inner_frustum_extent = 16'384U;

// Derives this node's plan.
//
// `region` comes from the wall geometry - `output::inner_frustum_region` for a
// flat surface or a projection mesh - and is already outward-rounded and
// clamped to `surface.raster`. It is passed in rather than derived here because
// the rectangle must be computed once per frame for the cohort rather than once
// per node output.
//
// `surface` supplies both halves of the frame of reference: its `raster` is the
// space `region` and `slice` are expressed in, and its geometry is what the
// view is later built through. Passing the surface rather than a bare canvas is
// what keeps those two from drifting apart.
//
// `slice` is the only per-node input, and it is used for exactly one thing:
// deciding whether this node intersects the rectangle. Nothing about the
// rectangle itself, the view, or the feather depends on it.
[[nodiscard]] IcvfxPlan plan_inner_frustum(
    const scene::Scene& scene,
    const scene::TrackedCameraView& view,
    const output::PhysicalSurface& surface,
    scene::CanvasRect slice,
    const scene::InnerFrustumRegion& region) noexcept;

// One tracked camera with this frame's pose and lens encoders applied.
//
// One definition, called by everything that needs to know where a physical
// camera is: the region its plate occupies on the wall and the view that plate
// is rendered with must come from the same eye, or the plate would be drawn
// correctly in the wrong place.
//
// The authored half comes from the camera node the feed is bound to, not from
// the outer camera: they are different objects on the stage, and the tracked
// one carries its own clip range, exposure, and - the part that actually shows
// - its own lens.
[[nodiscard]] scene::Camera
tracked_stage_camera(const scene::TrackedCameraView& view) noexcept;

// The view the inner frustum renders with.
//
// It is the generalized off-axis perspective from the tracked eye through the
// inner rectangle *on the wall* - `output::perspective_view_for`, the same
// construction the outer frustum is built with in `mapped_scene`, differing
// only in which eye and which region of the surface it is given.
//
// That it goes through the wall is the whole point. The plate has to show what
// the camera would see along the ray to each wall pixel it covers, and only the
// surface geometry says where those pixels are in the world. Narrowing the
// camera's own frustum by the rectangle's fraction of a canvas would agree with
// this only when the camera happens to see the entire wall, and the difference
// between the two is exactly the parallax the feature exists to produce.
//
// Empty when the plan is inactive or skipped, or when the surface refuses the
// eye - behind the wall, coplanar with it, or a degenerate rectangle. An empty
// answer means "no plate this frame", and it is reached identically on every
// node because every input to it is replicated. It is an optional rather than
// a default-constructed view so that no caller can render one by accident.
//
// `history_identity` is deliberately *stable*: it names "the inner frustum of
// this scene", not the pose. A tracked camera moving or zooming must produce
// motion vectors, not a temporal-history reset; a pose-derived identity would
// reset history on every frame of every move and permanently defeat temporal
// antialiasing. This is the same discipline output-map views already use.
[[nodiscard]] std::optional<SceneRenderView> inner_frustum_view(
    const scene::Scene& scene, const scene::TrackedCameraView& view,
    const output::PhysicalSurface& surface, const IcvfxPlan& plan) noexcept;

// The wall a plate is drawn on when it is not flat: a horizontal arc or an
// imported UV mesh.
//
// `mesh` is the **complete** surface mesh, whose UV0 spans the surface's whole
// raster - never one output's region mesh - so the plate rectangle, the view
// and the warp are the same on every node. `placement` is null for a mesh
// already in stage space (a generated arc) and carries the calibration for an
// imported mesh, whose cached vertices stay file-local.
struct IcvfxSurfaceMesh final {
    const output::ProjectionMesh* mesh{};
    const output::ProjectionMeshPlacement* placement{};
};

// The plate rectangle on a mesh wall: the mesh form of
// `output::inner_frustum_region` over the complete surface mesh, sized by the
// camera's calibrated lens when it has one and by the sensor aspect when it
// does not - the same choice the flat-wall path makes. One definition, called
// by the nodes that draw the plate and by the Director that reports it, so the
// two can never describe different rectangles. `tracked` is
// `tracked_stage_camera(view)`. Invalid when no mesh is given.
[[nodiscard]] scene::InnerFrustumRegion inner_frustum_mesh_region(
    const IcvfxSurfaceMesh& mesh, scene::Canvas raster,
    const scene::Camera& tracked, const scene::TrackedCameraView& view,
    core::u32 margin_px) noexcept;

// The plate view on a mesh wall, and the fitted mesh view the plate is then
// warped through.
struct IcvfxMeshView final {
    SceneRenderView render{};
    output::MeshPerspectiveView mesh_view{};
};

// The mesh-wall counterpart of `inner_frustum_view`.
//
// A curved wall has no single rectangle to build an off-axis frustum through,
// so the plate is the same two steps the outer frustum on that wall already
// is: one perspective render from the tracked eye, fitted to the triangles of
// the surface under the plate rectangle (`output::MeshDestinationWindow`), and
// a projective warp of that render through the same triangles into the
// rectangle. The warped plate then covers `plan.inner_rect` exactly as a flat
// wall's plate does, and composites through the same `IcvfxCompositor`.
//
// Every input is replicated, so a refusal is reached on every node alike. The
// error says which: MeshBehindEye when part of the wall under the plate is
// behind the tracked eye, DegenerateFrustum when no triangle lies under the
// rectangle or the plan is not drawing, InvalidMesh / InvalidSurface for
// unusable geometry or placement, InvalidClipRange / InvalidEye for the
// tracked camera.
//
// No plate depth of field: `plate_lens` describes a flat wall's plane and a
// curved wall has none, so it is left empty and the plate renders sharp. The
// caller reports that when depth of field was authored.
[[nodiscard]] core::Result<IcvfxMeshView, output::PerspectiveMappingError>
inner_frustum_mesh_view(const scene::Scene& scene,
                        const scene::TrackedCameraView& view,
                        const output::PhysicalSurface& surface,
                        const IcvfxSurfaceMesh& mesh,
                        const IcvfxPlan& plan) noexcept;

// Frame-path counters. Reading them performs no query, allocation, or wait.
struct IcvfxStats final {
    core::u64 frames_planned{};
    core::u64 inner_frames_rendered{};
    // Frames where this node's slice missed the inner rectangle. On a wide
    // wall with a long lens this is most of them, which is the point.
    core::u64 inner_frames_skipped{};
    core::u64 composites{};
    // Plans refused because the inner target would exceed the bound, or
    // because the compositor could not accept them. Counted, never absorbed.
    core::u64 refusals{};

    bool operator==(const IcvfxStats&) const = default;
};

// The GPU pass: inner over outer, with the feather.
//
// It reads two completed textures and writes a third, which keeps the composite
// independent of how either was drawn. Both inputs are sampled, so neither is
// modified and either may be reused by another output on the same frame.
class IcvfxCompositor final {
public:
    [[nodiscard]] static bool shaders_available() noexcept;

    [[nodiscard]] static core::Result<IcvfxCompositor, RenderError>
    create(rhi::Device& device, core::u32 width, core::u32 height,
           rhi::TextureFormat format = rhi::TextureFormat::Rgba8Unorm);

    IcvfxCompositor(const IcvfxCompositor&) = delete;
    IcvfxCompositor& operator=(const IcvfxCompositor&) = delete;
    IcvfxCompositor(IcvfxCompositor&&) = default;
    IcvfxCompositor& operator=(IcvfxCompositor&&) = default;
    ~IcvfxCompositor() = default;

    [[nodiscard]] rhi::Texture& target() noexcept { return color_; }
    [[nodiscard]] const rhi::Texture& target() const noexcept {
        return color_;
    }

    [[nodiscard]] bool resize(rhi::Device& device, core::u32 width,
                              core::u32 height) noexcept;

    // Composites one node's slice. `slice` is this node's rectangle of the
    // canvas and the plan carries the rectangle, feather, and skip decision.
    //
    // An inactive or skipped plan copies `outer` through unchanged rather than
    // taking a different path, so a node that skips the inner render and a node
    // that draws it produce their shared pixels with the same instruction
    // sequence. `inner` may be null exactly when the plan does not need it.
    [[nodiscard]] bool render(rhi::CommandList& commands,
                              const IcvfxPlan& plan,
                              scene::CanvasRect slice,
                              const rhi::Texture& outer,
                              const rhi::Texture* inner) noexcept;

    [[nodiscard]] const IcvfxStats& stats() const noexcept { return stats_; }

private:
    IcvfxCompositor(rhi::Texture color, rhi::GraphicsPipeline pipeline,
                    rhi::Sampler sampler) noexcept;

    rhi::Texture color_;
    rhi::GraphicsPipeline pipeline_;
    rhi::Sampler sampler_;
    IcvfxStats stats_{};
};

// The composite weight the shader applies, evaluated on the CPU.
//
// Exposed because it is the quantity two nodes sharing the inner rectangle's
// boundary must agree on exactly, so it needs to be something a test can
// compare rather than something only a GPU can produce. `canvas_x`/`canvas_y`
// are absolute canvas pixel centres.
[[nodiscard]] float icvfx_composite_weight(
    const IcvfxPlan& plan, float canvas_x, float canvas_y) noexcept;

} // namespace mediaserver::render
