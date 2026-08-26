#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/output/perspective_mapping.hpp>
#include <mediaserver/scene/canvas.hpp>

#include <optional>
#include <string>
#include <vector>

// The evaluated feed map: what the output stage draws with, and the last place
// the answer is still a rectangle rather than a pixel.
//
// Like `scene::Scene`, a FeedMap is a value with no graph in it, and it exists
// to make one comparison possible: two nodes hold equal maps, or their regions
// of the wall will not line up. That comparison is `digest()`, and it is over
// the authored parameters - rectangles replicated from the document - not over
// any rendering of them, so it agrees across nodes exactly as the scene digest
// does and for the same reason.
namespace mediaserver::output {

// The projection policy is deliberately independent from output placement.
// CanvasSlice preserves the original shared-camera crop workflow. A
// PlanarSurface is the original serialized name for a physical-surface
// assignment. The referenced ProjectionSurface chooses either an exact planar
// frustum or a generated mesh warp; both derive their view from the scene
// camera's eye without silently creating a bank of cameras.
enum class FeedProjection : core::u8 {
    CanvasSlice,
    PlanarSurface,
};

// Friendly physical-surface constructors share one projection pipeline. A
// planar screen uses the exact rectangular frustum path; a horizontal arc is
// generated as a real indexed mesh and uses the projective warp path. Imported
// arbitrary meshes can join this enum without changing feed or hardware
// routing.
enum class ProjectionSurfaceShape : core::u8 {
    Planar,
    HorizontalArc,
    ImportedMesh,
};

// Imported calibration geometry is expressed in stage units after this
// multiplier is applied. Bound it before an asset loader sees it so a damaged
// project cannot manufacture infinities or unusably large world coordinates.
inline constexpr float max_imported_mesh_scale = 1'000'000.0F;

// A reusable physical LED face. Several outputs may reference distinct raster
// regions of one surface, which is how a wall wider than one connector stays
// one calibrated object rather than several nearly-matching camera setups.
struct ProjectionSurface final {
    doc::NodeId node{};
    std::string name;
    PhysicalSurface geometry{};
    ProjectionSurfaceShape shape{ProjectionSurfaceShape::Planar};
    // Total signed bend in turns. Positive bows the edges toward the authored
    // front face/viewer; negative bows them away. Used only by HorizontalArc.
    float horizontal_arc_turns{0.25F};
    core::u32 mesh_columns{32U};
    // External path (or a future asset URI) for an arbitrary calibrated mesh.
    // The asset loader owns format support; the output model owns only the
    // replicated identity and physical-unit conversion. Until project assets
    // are packaged, every render node must resolve this string identically.
    std::string mesh_source;
    float mesh_scale{1.0F};
    // First-class projection asset identity. Zero is the legacy/source-only
    // form and remains valid for old projects.
    doc::NodeId mesh_asset{};
    // Runtime-only verified path; see ResolvedProjectionSurface below.
    std::optional<std::string> resolved_mesh_source;

    [[nodiscard]] const std::string& render_mesh_source() const noexcept {
        return resolved_mesh_source.has_value() ? *resolved_mesh_source
                                                : mesh_source;
    }

    bool operator==(const ProjectionSurface&) const = default;
};

struct ResolvedProjectionSurface final {
    PhysicalSurface geometry{};
    ProjectionSurfaceShape shape{ProjectionSurfaceShape::Planar};
    float horizontal_arc_turns{0.25F};
    core::u32 mesh_columns{32U};
    std::string mesh_source;
    float mesh_scale{1.0F};
    // Runtime-only verified content-store resolution. nullopt uses the
    // authored path for legacy/external projects; empty means a managed mesh
    // is not ready and must not silently fall back to machine-local bytes.
    // output::digest intentionally ignores this machine-local value.
    std::optional<std::string> resolved_mesh_source;
    // Authored stable identity copied from ProjectionSurface. It is appended
    // after the runtime field to preserve existing aggregate initializers.
    doc::NodeId mesh_asset{};

    [[nodiscard]] const std::string& render_mesh_source() const noexcept {
        return resolved_mesh_source.has_value() ? *resolved_mesh_source
                                                : mesh_source;
    }

    bool operator==(const ResolvedProjectionSurface&) const = default;
};

// One node's binding: the canvas rectangle it outputs.
//
// This lives in the document because it has to be replicated - a node that
// disagreed with the Director about its own rectangle would render a correct
// image of the wrong region. A node selects its map by its own index and never
// invents one. Adjacent nodes' rectangles abut edge to edge with no overlap:
// this drives LED walls, where the seam is pixel-exact and there is nothing to
// blend.
struct OutputMap final {
    core::u32 node_index{};
    // CanvasSlice: the region of the scene canvas this node outputs.
    // PlanarSurface: the region of the referenced surface raster this node
    // owns. The same rectangle vocabulary makes port partitioning and the
    // editor's 2D feed layout reusable without conflating the two spaces.
    scene::CanvasRect source{};

    FeedProjection projection{FeedProjection::CanvasSlice};
    // Authored reference. It remains visible when the surface is missing so
    // the editor can diagnose and repair a dangling assignment.
    doc::NodeId surface{};
    // Resolved at evaluation. Empty means the physical assignment is missing
    // or its region falls outside the surface raster; presenters must refuse
    // it rather than fall back to a plausible but incorrect camera crop.
    std::optional<ResolvedProjectionSurface> resolved_surface;

    [[nodiscard]] bool perspective_ready() const noexcept {
        return projection == FeedProjection::PlanarSurface
            && resolved_surface.has_value();
    }

    bool operator==(const OutputMap&) const = default;
};

// A cap, so a runaway document is refused rather than absorbed. Generous
// against any real wall and far below what would matter.
inline constexpr core::u32 max_output_maps = 256U;
inline constexpr core::u32 max_projection_surfaces = 256U;

struct FeedMap final {
    // Sorted by node index. One binding per node; the lowest node id wins a
    // duplicate, decided in the model so every node resolves it the same way.
    std::vector<OutputMap> maps;

    // Stable document-id order. Surfaces are project setup data and remain in
    // the evaluated value even when no feed currently references them.
    std::vector<ProjectionSurface> surfaces;

    // The map bound to this node, or null when the document binds none. A node
    // with no map outputs nothing rather than guessing at the whole canvas:
    // an unmapped output should be black and obviously unmapped, exactly as an
    // unmapped scene slice is.
    [[nodiscard]] const OutputMap* map_for(core::u32 node_index) const noexcept;

    [[nodiscard]] const ProjectionSurface*
    surface_for(doc::NodeId node) const noexcept;
};

// FNV-1a over every map in node-index order. Hashes the authored parameters,
// not a rendering, so two nodes that agree on the document agree on this.
// Counts are hashed before contents so a map that is a prefix of another cannot
// collide with it.
[[nodiscard]] core::u64 digest(const FeedMap& feed_map) noexcept;

} // namespace mediaserver::output
