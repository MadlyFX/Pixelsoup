#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/output/feed_map.hpp>

#include <string>

// Imports an externally-authored LED projection patch.
//
// Each file represents one feed output. Vertex positions retain the external
// package's shared modelling coordinates, while UV0 is the destination image
// for that connector. Split pieces therefore stay aligned when they use the
// same origin/orientation/scale; they are deliberately not normalised around
// their individual bounds.
namespace mediaserver::render {

enum class ProjectionMeshLoadError : core::u8 {
    InvalidSource,
    ModelLoadFailed,
    InvalidCalibration,
    CapacityExceeded,
    InvalidGeometry,
    MissingOrDegenerateUv,
    UvOutsideConnector,
    AllocationFailed,
};

struct ProjectionMeshLoadLimits final {
    core::u32 max_vertices{65'536U};
    core::u32 max_indices{196'608U};

    bool operator==(const ProjectionMeshLoadLimits&) const = default;
};

inline constexpr float max_projection_mesh_scale =
    output::max_imported_mesh_scale;

// Decodes and validates immutable file-local geometry. This is the form used
// by OutputProjectionCache: calibration and mesh scale are deliberately not
// inputs, so animated placement cannot cause another file read.
[[nodiscard]] core::Result<output::ProjectionMesh,
                           ProjectionMeshLoadError>
load_projection_mesh_local(const std::string& source,
                           ProjectionMeshLoadLimits limits = {});

// `calibration.center` is the imported asset's stage-space origin. Local +X
// follows calibration.right, local +Y follows calibration.up, and local +Z
// follows cross(right, up), the surface's authored front direction. `scale`
// is uniform so externally split pieces cannot acquire different proportions.
// Width/height are not used to normalise the mesh.
[[nodiscard]] core::Result<output::ProjectionMesh,
                           ProjectionMeshLoadError>
load_projection_mesh(const std::string& source,
                     const output::PhysicalSurface& calibration,
                     float scale = 1.0F,
                     ProjectionMeshLoadLimits limits = {});

} // namespace mediaserver::render
