#include <mediaserver/render/projection_mesh_loader.hpp>

#include <mediaserver/render/model_loader.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <new>
#include <utility>

namespace mediaserver::render {
namespace {

using Result = core::Result<output::ProjectionMesh, ProjectionMeshLoadError>;

[[nodiscard]] bool finite(scene::Vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

[[nodiscard]] bool finite(float value) noexcept { return std::isfinite(value); }

struct CalibrationBasis final {
    scene::Vec3 right{};
    scene::Vec3 up{};
    scene::Vec3 front{};
};

[[nodiscard]] bool calibration_basis(const output::PhysicalSurface& calibration,
                                     CalibrationBasis& into) noexcept {
    if (!output::valid(calibration)) {
        return false;
    }
    into.right = scene::normalize(calibration.right);
    const scene::Vec3 rejected =
        calibration.up - into.right * scene::dot(calibration.up, into.right);
    if (!(scene::length(rejected) > 1.0e-8F)) {
        return false;
    }
    into.up = scene::normalize(rejected);
    into.front = scene::normalize(scene::cross(into.right, into.up));
    return finite(into.right) && finite(into.up) && finite(into.front);
}

[[nodiscard]] bool valid_limits(ProjectionMeshLoadLimits limits) noexcept {
    return limits.max_vertices >= 3U && limits.max_indices >= 3U;
}

[[nodiscard]] bool uv_in_connector(float value) noexcept {
    constexpr float tolerance = 1.0e-5F;
    return finite(value) && value >= -tolerance && value <= 1.0F + tolerance;
}

[[nodiscard]] float uv_area_twice(const scene::MeshVertex& a,
                                  const scene::MeshVertex& b,
                                  const scene::MeshVertex& c) noexcept {
    return (b.u - a.u) * (c.v - a.v) - (b.v - a.v) * (c.u - a.u);
}

} // namespace

Result load_projection_mesh(const std::string& source,
                            const output::PhysicalSurface& calibration,
                            float scale, ProjectionMeshLoadLimits limits) {
    if (source.empty()) {
        return Result::failure(ProjectionMeshLoadError::InvalidSource);
    }
    CalibrationBasis basis{};
    if (!calibration_basis(calibration, basis) || !finite(scale) ||
        !(scale > 0.0F) || scale > max_projection_mesh_scale) {
        return Result::failure(ProjectionMeshLoadError::InvalidCalibration);
    }
    if (!valid_limits(limits)) {
        return Result::failure(ProjectionMeshLoadError::CapacityExceeded);
    }

    ModelAsset asset{};
    try {
        if (!load_model(source, asset)) {
            return Result::failure(ProjectionMeshLoadError::ModelLoadFailed);
        }
    } catch (const std::bad_alloc&) {
        return Result::failure(ProjectionMeshLoadError::AllocationFailed);
    }
    if (asset.mesh.vertices.size() > limits.max_vertices ||
        asset.mesh.indices.size() > limits.max_indices) {
        return Result::failure(ProjectionMeshLoadError::CapacityExceeded);
    }
    if (asset.mesh.vertices.empty() || asset.mesh.indices.empty() ||
        asset.mesh.indices.size() % 3U != 0U) {
        return Result::failure(ProjectionMeshLoadError::InvalidGeometry);
    }
    if (asset.vertex_metadata.size() != asset.mesh.vertices.size() ||
        !std::all_of(asset.vertex_metadata.begin(), asset.vertex_metadata.end(),
                     [](const ModelVertexMetadata& metadata) noexcept {
                         return metadata.has_uv0;
                     })) {
        return Result::failure(ProjectionMeshLoadError::MissingOrDegenerateUv);
    }

    for (const scene::MeshVertex& vertex : asset.mesh.vertices) {
        if (!finite(vertex.position)) {
            return Result::failure(ProjectionMeshLoadError::InvalidGeometry);
        }
        if (!uv_in_connector(vertex.u) || !uv_in_connector(vertex.v)) {
            return Result::failure(ProjectionMeshLoadError::UvOutsideConnector);
        }
    }
    for (std::size_t triangle = 0U; triangle < asset.mesh.indices.size();
         triangle += 3U) {
        const core::u32 ia = asset.mesh.indices[triangle];
        const core::u32 ib = asset.mesh.indices[triangle + 1U];
        const core::u32 ic = asset.mesh.indices[triangle + 2U];
        if (static_cast<std::size_t>(ia) >= asset.mesh.vertices.size() ||
            static_cast<std::size_t>(ib) >= asset.mesh.vertices.size() ||
            static_cast<std::size_t>(ic) >= asset.mesh.vertices.size()) {
            return Result::failure(ProjectionMeshLoadError::InvalidGeometry);
        }
        const scene::MeshVertex& a = asset.mesh.vertices[ia];
        const scene::MeshVertex& b = asset.mesh.vertices[ib];
        const scene::MeshVertex& c = asset.mesh.vertices[ic];
        if (!(scene::length(scene::cross(b.position - a.position,
                                         c.position - a.position)) > 1.0e-8F)) {
            return Result::failure(ProjectionMeshLoadError::InvalidGeometry);
        }
        if (!(std::abs(uv_area_twice(a, b, c)) > 1.0e-8F)) {
            return Result::failure(
                ProjectionMeshLoadError::MissingOrDegenerateUv);
        }
    }

    try {
        output::ProjectionMesh mesh{};
        mesh.vertices.reserve(asset.mesh.vertices.size());
        mesh.indices = asset.mesh.indices;
        for (const scene::MeshVertex& vertex : asset.mesh.vertices) {
            const scene::Vec3 local = vertex.position * scale;
            const scene::Vec3 position =
                calibration.center + basis.right * local.x +
                basis.up * local.y + basis.front * local.z;
            if (!finite(position)) {
                return Result::failure(
                    ProjectionMeshLoadError::InvalidGeometry);
            }
            mesh.vertices.push_back(output::ProjectionMeshVertex{
                position, std::clamp(vertex.u, 0.0F, 1.0F),
                std::clamp(vertex.v, 0.0F, 1.0F)});
        }
        if (!output::valid(mesh)) {
            return Result::failure(ProjectionMeshLoadError::InvalidGeometry);
        }
        return Result::success(std::move(mesh));
    } catch (const std::bad_alloc&) {
        return Result::failure(ProjectionMeshLoadError::AllocationFailed);
    }
}

} // namespace mediaserver::render
