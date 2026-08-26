#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/scene/canvas.hpp>

#include <vector>

// Physical-screen perspective mapping.
//
// A feed-map rectangle answers which pixels a connector owns. A physical
// surface answers the separate question that matters on an LED stage: which
// window into the 3D world those pixels represent. This is the same primitive
// as nDisplay's planar "simple" projection policy: one rectangular screen in
// stage space and one view origin produce an off-axis frustum automatically.
// No operator-authored camera is required for each wall face.
//
// One surface may be split into any number of raster regions, so several ports
// or render nodes can share it without duplicating its physical calibration.
// Region edges are derived from absolute raster boundaries; two adjacent
// regions therefore produce bit-identical shared corners and frustum planes.
//
// This is intentionally the flat-screen policy. Curved and arbitrary walls
// belong to a mesh-backed policy beside it: one asymmetric render followed by
// a projective UV warp, as in modern nDisplay. They must not be disguised here
// as a manually-authored bank of narrow cameras or pseudo-planar columns.
namespace mediaserver::output {

struct PhysicalSurface final {
    // Centre of the LED face in scene/world space.
    scene::Vec3 center{0.0F, 0.0F, -1.0F};
    // Directions along increasing raster X and decreasing raster Y. They need
    // not be unit length; evaluation normalises and orthogonalises them. Their
    // handedness defines the front face: cross(right, up) points at the viewer.
    scene::Vec3 right{1.0F, 0.0F, 0.0F};
    scene::Vec3 up{0.0F, 1.0F, 0.0F};
    // Physical extents in the same world units as the scene.
    float width{1.0F};
    float height{1.0F};
    // Addressable pixel geometry of the complete logical surface. Output ports
    // partition this raster with scene::CanvasRect values.
    scene::Canvas raster{1U, 1U};

    bool operator==(const PhysicalSurface&) const = default;
};

// The four world-space corners of one raster region. Naming is physical, while
// CanvasRect remains top-left/+Y-down: upper_left corresponds to (x,y), and
// lower_right corresponds to (x+width,y+height).
struct SurfaceCorners final {
    scene::Vec3 lower_left{};
    scene::Vec3 lower_right{};
    scene::Vec3 upper_left{};
    scene::Vec3 upper_right{};

    bool operator==(const SurfaceCorners&) const = default;
};

enum class PerspectiveMappingError : core::u8 {
    InvalidSurface,
    RegionOutsideSurface,
    InvalidEye,
    InvalidClipRange,
    EyeNotInFront,
    InvalidMesh,
    MeshBehindEye,
    DegenerateFrustum,
    AllocationFailed,
};

struct PerspectiveView final {
    // The region of PhysicalSurface::raster this view renders.
    scene::CanvasRect region{};
    SurfaceCorners corners{};
    scene::FrustumWindow frustum{};
    scene::ViewMatrices matrices{};
    // Explicit because the renderer also needs them for PBR lighting and
    // deterministic translucent depth sorting; changing only the projection
    // matrix would make a side wall look geometrically right but shade and
    // composite from the old camera direction.
    scene::Vec3 eye{};
    scene::Vec3 forward{0.0F, 0.0F, -1.0F};
    // Perpendicular distance from the eye to the surface plane.
    float eye_distance{};

    bool operator==(const PerspectiveView&) const = default;
};

// True when the surface has finite, positive extents, a valid raster, and two
// finite non-parallel orientation axes.
[[nodiscard]] bool valid(const PhysicalSurface& surface) noexcept;

// Converts an absolute raster region to world-space corners. Adjacent regions
// that share an integer raster boundary return exactly equal corner positions
// on that boundary, not merely approximately equal ones.
[[nodiscard]] core::Result<SurfaceCorners, PerspectiveMappingError>
surface_corners(const PhysicalSurface& surface,
                scene::CanvasRect region) noexcept;

// Builds a right-handed, -Z-forward, reversed-depth generalized perspective
// projection for one surface region. The front face is determined by
// cross(surface.right, surface.up); an eye on or behind it is rejected.
[[nodiscard]] core::Result<PerspectiveView, PerspectiveMappingError>
perspective_view_for(const PhysicalSurface& surface,
                     scene::CanvasRect region,
                     scene::Vec3 eye,
                     float near_plane,
                     float far_plane) noexcept;

// Projection back into the complete surface raster. This deliberately has the
// same shape as scene::CanvasProjection: x/y are absolute surface pixels, so
// the whole-surface view and every partitioned view can be compared directly.
[[nodiscard]] scene::CanvasProjection project_through_surface(
    const PerspectiveView& view,
    scene::Vec3 world_point) noexcept;

// The geometry contract for nDisplay-style arbitrary-surface projection.
// Position is already in stage/world space. UV0 is destination space: (0,0)
// is the top-left of the connector image and (1,1) its bottom-right. Keeping
// that meaning explicit prevents a model's material UVs from accidentally
// becoming an LED routing map.
struct ProjectionMeshVertex final {
    scene::Vec3 position{};
    float destination_u{};
    float destination_v{};

    bool operator==(const ProjectionMeshVertex&) const = default;
};

struct ProjectionMesh final {
    std::vector<ProjectionMeshVertex> vertices;
    std::vector<core::u32> indices;

    bool operator==(const ProjectionMesh&) const = default;
};

inline constexpr core::u32 min_horizontal_arc_columns = 2U;
inline constexpr core::u32 max_horizontal_arc_columns = 512U;

// Generates the common virtual-production case directly from measured setup
// values: a vertically straight LED wall with one circular horizontal bend.
// `surface.width` is arc length, not chord length; UV0 still spans the complete
// logical surface raster. Zero bend is intentionally rejected here—use the
// exact planar policy for that case.
[[nodiscard]] core::Result<ProjectionMesh, PerspectiveMappingError>
horizontal_arc_mesh(const PhysicalSurface& surface,
                    float total_arc_turns,
                    core::u32 columns) noexcept;

// Builds one connector's absolute raster region and remaps that region to
// destination UV 0..1. Adjacent regions evaluate their shared absolute pixel
// edge through the same expression, preserving the same physical seam while
// each node receives a tight frustum and a full-size connector image.
[[nodiscard]] core::Result<ProjectionMesh, PerspectiveMappingError>
horizontal_arc_mesh(const PhysicalSurface& surface,
                    float total_arc_turns,
                    core::u32 columns,
                    scene::CanvasRect region) noexcept;

// One asymmetric scene render that encloses an arbitrary physical patch. The
// subsequent warp rasterizes ProjectionMesh in destination UV space and
// projectively samples the scene render using source_clip. This is one camera
// plus one warp, not one camera per column of curved LED.
struct MeshPerspectiveView final {
    scene::FrustumWindow frustum{};
    scene::ViewMatrices matrices{};
    scene::Vec3 eye{};
    scene::Vec3 forward{0.0F, 0.0F, -1.0F};
    scene::Vec3 up{0.0F, 1.0F, 0.0F};

    bool operator==(const MeshPerspectiveView&) const = default;
};

struct MeshWarpVertex final {
    // Homogeneous connector position. x/y are derived from destination UV0;
    // z=0 and w=1 let a raster pipeline draw the physical mesh directly into
    // the output target without a second camera transform.
    scene::Vec4 destination_clip{};
    // Physical position projected by MeshPerspectiveView. A warp shader must
    // interpolate all four components and divide xyz by w in the fragment
    // stage before converting NDC to source texture UV.
    scene::Vec4 source_clip{};

    bool operator==(const MeshWarpVertex&) const = default;
};

[[nodiscard]] bool valid(const ProjectionMesh& mesh) noexcept;

// Fits one generalized perspective render to every indexed mesh vertex. The
// default stage-up axis is suitable for vertical LED walls; callers may supply
// another calibrated up direction. Patches spanning behind the eye are
// rejected and should be split, because no single perspective frustum can
// represent them honestly.
[[nodiscard]] core::Result<MeshPerspectiveView, PerspectiveMappingError>
mesh_perspective_view_for(const ProjectionMesh& mesh,
                          scene::Vec3 eye,
                          float near_plane,
                          float far_plane,
                          scene::Vec3 stage_up =
                              scene::Vec3{0.0F, 1.0F, 0.0F}) noexcept;

// Resolves the two coordinate spaces consumed by the GPU mesh-warp pass.
// Indices remain those in ProjectionMesh; the returned vector is parallel to
// mesh.vertices and deterministic for identical input bits.
[[nodiscard]] core::Result<std::vector<MeshWarpVertex>,
                           PerspectiveMappingError>
mesh_warp_vertices(const ProjectionMesh& mesh,
                   const MeshPerspectiveView& view) noexcept;

// Projects a world point through the same projective UV warp as the output
// shader. This is the inverse lookup the editor needs for picking and outlines:
// the returned x/y are pixels in the final connector image, after curvature or
// imported-UV distortion, rather than pixels in the intermediate scene render.
[[nodiscard]] scene::CanvasProjection project_through_mesh_warp(
    const ProjectionMesh& mesh,
    const MeshPerspectiveView& view,
    scene::Canvas destination,
    scene::Vec3 world_point) noexcept;

} // namespace mediaserver::output
