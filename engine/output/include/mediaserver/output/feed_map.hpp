#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/output/perspective_mapping.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/scene.hpp>

#include <optional>
#include <span>
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

// Where the stage that every surface is calibrated in stands. World is the
// virtual-production volume: surfaces are world coordinates and the camera
// moves among them. Camera carries the whole stage with the scene camera, so a
// wall, a ceiling and side walls keep facing where they physically face for
// the viewer while the camera flies - an immersive room or CAVE.
enum class StageAnchor : core::u8 {
    World,
    Camera,
};

struct StagePlacement final {
    StageAnchor anchor{StageAnchor::World};
    // Where the viewer stands, in stage coordinates. A camera-anchored stage
    // puts this point on the scene camera's eye, so the room is measured from
    // its own floor while the sweet spot moves independently of it. Unused by
    // a world-fixed stage, whose eye is the camera's.
    scene::Vec3 eye{};

    bool operator==(const StagePlacement&) const = default;
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
    // LED wall geometry is protected from accidental Stage selection and
    // manipulation unless the operator explicitly unlocks it in Feed Mapping.
    bool locked{};
    scene::WallColorSettings color;
    // Operator list position; see schema::list_order.
    core::i64 order{};

    bool operator==(const ProjectionSurface&) const = default;
};

struct ResolvedProjectionSurface final {
    PhysicalSurface geometry{};
    ProjectionSurfaceShape shape{ProjectionSurfaceShape::Planar};
    float horizontal_arc_turns{0.25F};
    core::u32 mesh_columns{32U};
    std::string mesh_source;
    float mesh_scale{1.0F};
    // Authored stable identity copied from ProjectionSurface.
    doc::NodeId mesh_asset{};
    scene::WallColorSettings color;
    // The project's stage, copied here so a presenter holding one map can
    // place its surface without reaching back for the whole feed map.
    StagePlacement stage;

    bool operator==(const ResolvedProjectionSurface&) const = default;
};

// One feed's binding: which image it renders and where that image lands in its
// application window.
//
// This lives in the document because it has to be replicated - a node that
// disagreed with the Director about either rectangle would render a correct
// image in the wrong place. A presenter selects its feed by the Hardware Map
// binding and never invents one. Source rectangles describe content/projection
// space; viewport rectangles independently describe placement inside the
// application's fixed window raster.
struct OutputMap final {
    // Stable identity of this viewport row. Several rows may target the same
    // logical output window, so the window index is not an object identity.
    doc::NodeId node{};
    core::u32 node_index{};
    // Operator label; empty shows the window/viewport ordinal.
    std::string name;
    // Position among the viewports of this window, which is also their
    // compositing order: later viewports draw over earlier ones.
    core::i64 order{};
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

    // Position and size inside the output application's fixed window raster.
    // Every map authors this explicitly. It is validated against the actual
    // window by the output transform, which leaves every pixel outside it
    // black.
    scene::CanvasRect viewport{};

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
    // Sorted by logical output-window index, list order, then stable document
    // id. Every
    // map in a same-index run is an independent viewport composited into that
    // one application window.
    std::vector<OutputMap> maps;

    // List order, then document id. Surfaces are project setup data and remain in
    // the evaluated value even when no feed currently references them.
    std::vector<ProjectionSurface> surfaces;

    // The project's stage. `stage_node` is the document node that authored
    // it, or zero for a project that never set one (a world-fixed stage).
    StagePlacement stage;
    doc::NodeId stage_node{};

    // Every viewport bound to one logical output window. The returned span is
    // empty when the window is unmapped.
    [[nodiscard]] std::span<const OutputMap>
    maps_for(core::u32 node_index) const noexcept;

    // Finds one viewport by its document identity. Editor mutations use this;
    // a window index is intentionally insufficient once a Mosaic window owns
    // several viewports.
    [[nodiscard]] const OutputMap* map_node(doc::NodeId node) const noexcept;

    // First viewport for a logical output. Useful only where presence or a
    // default preview is needed; presentation uses maps_for().
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
