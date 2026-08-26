#include <mediaserver/output/perspective_mapping.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <utility>

namespace mediaserver::output {
namespace {

using CornersResult = core::Result<SurfaceCorners, PerspectiveMappingError>;
using ViewResult = core::Result<PerspectiveView, PerspectiveMappingError>;

[[nodiscard]] bool finite(float value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] bool finite(scene::Vec3 value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

[[nodiscard]] bool finite(scene::Vec4 value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z)
        && finite(value.w);
}

[[nodiscard]] bool finite(scene::Mat4 value) noexcept {
    for (const float component : value.m) {
        if (!finite(component)) {
            return false;
        }
    }
    return true;
}

struct SurfaceBasis final {
    scene::Vec3 right{};
    scene::Vec3 up{};
    scene::Vec3 normal{};
};

[[nodiscard]] SurfaceBasis basis_for(const PhysicalSurface& surface) noexcept {
    const scene::Vec3 right = scene::normalize(surface.right);
    const scene::Vec3 normal = scene::normalize(
        scene::cross(right, surface.up), scene::Vec3{});
    // Rebuilding up makes the evaluated surface rectangular even if authored
    // axes carry a little measurement skew. The normal preserves their
    // handedness, so the face direction remains an authored choice.
    const scene::Vec3 up = scene::cross(normal, right);
    return SurfaceBasis{right, up, normal};
}

[[nodiscard]] bool valid_basis(const PhysicalSurface& surface) noexcept {
    if (!finite(surface.right) || !finite(surface.up)) {
        return false;
    }
    const float right_length = scene::length(surface.right);
    const float up_length = scene::length(surface.up);
    const float area = scene::length(scene::cross(surface.right, surface.up));
    return finite(right_length) && finite(up_length) && finite(area)
        && right_length > 1.0e-20F && up_length > 1.0e-20F
        && area > 1.0e-20F;
}

// A physical coordinate measured from the surface centre. `pixel` is an
// absolute edge, never a region-local offset. This is the seam guarantee: both
// sides of a shared boundary invoke this exact expression with the same integer.
[[nodiscard]] float horizontal_edge(const PhysicalSurface& surface,
                                    core::u64 pixel) noexcept {
    return -surface.width * 0.5F
        + surface.width
            * (static_cast<float>(pixel)
               / static_cast<float>(surface.raster.width));
}

[[nodiscard]] float vertical_edge(const PhysicalSurface& surface,
                                  core::u64 pixel) noexcept {
    return surface.height * 0.5F
        - surface.height
            * (static_cast<float>(pixel)
               / static_cast<float>(surface.raster.height));
}

[[nodiscard]] scene::Vec3 point_on_surface(const PhysicalSurface& surface,
                                           const SurfaceBasis& basis,
                                           core::u64 x,
                                           core::u64 y) noexcept {
    return surface.center + basis.right * horizontal_edge(surface, x)
        + basis.up * vertical_edge(surface, y);
}

[[nodiscard]] scene::Mat4 projection_for(
    const scene::FrustumWindow& frustum) noexcept {
    const float width = frustum.right - frustum.left;
    const float height = frustum.top - frustum.bottom;
    const float depth_span = frustum.far_plane - frustum.near_plane;
    const float twice_near = frustum.near_plane + frustum.near_plane;

    scene::Mat4 projection{};
    projection.set(0U, 0U, twice_near / width);
    projection.set(
        0U, 2U, (frustum.right + frustum.left) / width);
    projection.set(1U, 1U, twice_near / height);
    projection.set(
        1U, 2U, (frustum.top + frustum.bottom) / height);
    // Match scene::view_for_slice: reversed depth maps near to one and far to
    // zero, for a greater-than depth test and a zero depth clear.
    projection.set(2U, 2U, frustum.near_plane / depth_span);
    projection.set(
        2U, 3U,
        (frustum.near_plane * frustum.far_plane) / depth_span);
    projection.set(3U, 2U, -1.0F);
    return projection;
}

} // namespace

bool valid(const PhysicalSurface& surface) noexcept {
    return finite(surface.center) && finite(surface.width)
        && finite(surface.height) && surface.width > 0.0F
        && surface.height > 0.0F && scene::valid(surface.raster)
        && valid_basis(surface);
}

CornersResult surface_corners(const PhysicalSurface& surface,
                              scene::CanvasRect region) noexcept {
    if (!valid(surface)) {
        return CornersResult::failure(
            PerspectiveMappingError::InvalidSurface);
    }
    if (!scene::contains(surface.raster, region)) {
        return CornersResult::failure(
            PerspectiveMappingError::RegionOutsideSurface);
    }

    const SurfaceBasis basis = basis_for(surface);
    const core::u64 right = static_cast<core::u64>(region.x)
        + static_cast<core::u64>(region.width);
    const core::u64 bottom = static_cast<core::u64>(region.y)
        + static_cast<core::u64>(region.height);

    const SurfaceCorners corners{
        point_on_surface(surface, basis, region.x, bottom),
        point_on_surface(surface, basis, right, bottom),
        point_on_surface(surface, basis, region.x, region.y),
        point_on_surface(surface, basis, right, region.y),
    };
    if (!finite(corners.lower_left) || !finite(corners.lower_right)
        || !finite(corners.upper_left) || !finite(corners.upper_right)) {
        return CornersResult::failure(
            PerspectiveMappingError::InvalidSurface);
    }
    return CornersResult::success(corners);
}

ViewResult perspective_view_for(const PhysicalSurface& surface,
                                scene::CanvasRect region,
                                scene::Vec3 eye,
                                float near_plane,
                                float far_plane) noexcept {
    if (!finite(eye)) {
        return ViewResult::failure(PerspectiveMappingError::InvalidEye);
    }
    if (!finite(near_plane) || !finite(far_plane) || !(near_plane > 0.0F)
        || !(far_plane > near_plane)) {
        return ViewResult::failure(
            PerspectiveMappingError::InvalidClipRange);
    }

    auto corners_result = surface_corners(surface, region);
    if (!corners_result) {
        return ViewResult::failure(corners_result.error());
    }
    const SurfaceCorners corners = std::move(corners_result).value();
    const SurfaceBasis basis = basis_for(surface);

    // `normal` points out of the authored front face. Its signed distance from
    // the surface to the eye must be positive. Refusing the reverse side keeps
    // horizontal orientation stable instead of silently mirroring a wall whose
    // axes were authored backwards.
    const float distance = scene::dot(basis.normal, eye - surface.center);
    if (!finite(distance) || !(distance > 1.0e-6F)) {
        return ViewResult::failure(
            PerspectiveMappingError::EyeNotInFront);
    }

    const scene::Vec3 to_lower_left = corners.lower_left - eye;
    const scene::Vec3 to_lower_right = corners.lower_right - eye;
    const scene::Vec3 to_upper_left = corners.upper_left - eye;
    const float near_scale = near_plane / distance;

    const scene::FrustumWindow frustum{
        scene::dot(basis.right, to_lower_left) * near_scale,
        scene::dot(basis.right, to_lower_right) * near_scale,
        scene::dot(basis.up, to_lower_left) * near_scale,
        scene::dot(basis.up, to_upper_left) * near_scale,
        near_plane,
        far_plane,
    };
    if (!finite(frustum.left) || !finite(frustum.right)
        || !finite(frustum.bottom) || !finite(frustum.top)
        || !(frustum.right > frustum.left)
        || !(frustum.top > frustum.bottom)) {
        // With a valid rectangular region this can only be numerical collapse
        // from physically unusable values, so surface validation is the most
        // useful category to expose to an authoring UI.
        return ViewResult::failure(
            PerspectiveMappingError::InvalidSurface);
    }

    const scene::Mat4 projection = projection_for(frustum);
    const scene::Mat4 view = scene::look_at(
        eye, eye - basis.normal, basis.up);
    return ViewResult::success(PerspectiveView{
        region,
        corners,
        frustum,
        scene::ViewMatrices{
            view, projection, scene::multiply(projection, view)},
        eye,
        -basis.normal,
        distance,
    });
}

scene::CanvasProjection project_through_surface(
    const PerspectiveView& view,
    scene::Vec3 world_point) noexcept {
    const scene::Vec4 clip = scene::transform(
        view.matrices.view_projection,
        scene::Vec4{world_point.x, world_point.y, world_point.z, 1.0F});
    const float ndc_x = clip.w != 0.0F ? clip.x / clip.w : 0.0F;
    const float ndc_y = clip.w != 0.0F ? clip.y / clip.w : 0.0F;
    const float depth = clip.w != 0.0F ? clip.z / clip.w : 0.0F;
    return scene::CanvasProjection{
        static_cast<float>(view.region.x)
            + (ndc_x * 0.5F + 0.5F)
                * static_cast<float>(view.region.width),
        static_cast<float>(view.region.y)
            + (0.5F - ndc_y * 0.5F)
                * static_cast<float>(view.region.height),
        depth,
        clip.w > 0.0F,
    };
}

bool valid(const ProjectionMesh& mesh) noexcept {
    if (mesh.vertices.empty() || mesh.indices.empty()
        || mesh.indices.size() % 3U != 0U) {
        return false;
    }
    for (const ProjectionMeshVertex& vertex : mesh.vertices) {
        if (!finite(vertex.position) || !finite(vertex.destination_u)
            || !finite(vertex.destination_v)) {
            return false;
        }
    }
    for (const core::u32 index : mesh.indices) {
        if (static_cast<std::size_t>(index) >= mesh.vertices.size()) {
            return false;
        }
    }
    return true;
}

core::Result<ProjectionMesh, PerspectiveMappingError>
horizontal_arc_mesh(const PhysicalSurface& surface, float total_arc_turns,
                    core::u32 columns) noexcept {
    return horizontal_arc_mesh(
        surface, total_arc_turns, columns, scene::whole(surface.raster));
}

core::Result<ProjectionMesh, PerspectiveMappingError>
horizontal_arc_mesh(const PhysicalSurface& surface, float total_arc_turns,
                    core::u32 columns, scene::CanvasRect region) noexcept {
    using Result = core::Result<ProjectionMesh, PerspectiveMappingError>;
    const float magnitude = std::abs(total_arc_turns);
    if (!valid(surface) || !finite(total_arc_turns)
        || !(magnitude > 1.0e-5F) || !(magnitude < 0.5F)
        || columns < min_horizontal_arc_columns
        || columns > max_horizontal_arc_columns
        || !scene::contains(surface.raster, region)) {
        return Result::failure(PerspectiveMappingError::InvalidMesh);
    }

    constexpr float two_pi = 6.28318530717958647692F;
    const float radius = surface.width / (two_pi * magnitude);
    if (!finite(radius) || !(radius > 0.0F)) {
        return Result::failure(PerspectiveMappingError::InvalidMesh);
    }
    const SurfaceBasis basis = basis_for(surface);
    const float bend_sign = total_arc_turns >= 0.0F ? 1.0F : -1.0F;
    const core::u64 scaled_columns =
        static_cast<core::u64>(columns) * region.width;
    const core::u32 region_columns = std::max(
        min_horizontal_arc_columns,
        static_cast<core::u32>(
            (scaled_columns + surface.raster.width - 1U)
            / surface.raster.width));
    const float top_fraction = static_cast<float>(region.y)
        / static_cast<float>(surface.raster.height);
    const core::u64 bottom_pixel = static_cast<core::u64>(region.y)
        + region.height;
    const float bottom_fraction = static_cast<float>(bottom_pixel)
        / static_cast<float>(surface.raster.height);

    try {
        ProjectionMesh mesh{};
        mesh.vertices.reserve(
            static_cast<std::size_t>(region_columns + 1U) * 2U);
        mesh.indices.reserve(static_cast<std::size_t>(region_columns) * 6U);
        for (core::u32 column = 0U; column <= region_columns; ++column) {
            const float u = static_cast<float>(column)
                / static_cast<float>(region_columns);
            float absolute_pixel{};
            if (column == 0U) {
                absolute_pixel = static_cast<float>(region.x);
            } else if (column == region_columns) {
                absolute_pixel = static_cast<float>(
                    static_cast<core::u64>(region.x) + region.width);
            } else {
                absolute_pixel = static_cast<float>(region.x)
                    + static_cast<float>(region.width) * u;
            }
            const float surface_u = absolute_pixel
                / static_cast<float>(surface.raster.width);
            const float theta = (surface_u - 0.5F) * magnitude;
            const float along =
                radius * scene::deterministic::sin_turns(theta);
            const float bow = bend_sign * radius
                * (1.0F - scene::deterministic::cos_turns(theta));
            const scene::Vec3 middle =
                surface.center + basis.right * along + basis.normal * bow;
            mesh.vertices.push_back(ProjectionMeshVertex{
                middle + basis.up *
                    (surface.height * (0.5F - top_fraction)),
                u, 0.0F});
            mesh.vertices.push_back(ProjectionMeshVertex{
                middle + basis.up *
                    (surface.height * (0.5F - bottom_fraction)),
                u, 1.0F});
        }
        for (core::u32 column = 0U; column < region_columns; ++column) {
            const core::u32 top_left = column * 2U;
            const core::u32 bottom_left = top_left + 1U;
            const core::u32 top_right = top_left + 2U;
            const core::u32 bottom_right = top_left + 3U;
            mesh.indices.insert(mesh.indices.end(),
                                {top_left, bottom_left, top_right,
                                 top_right, bottom_left, bottom_right});
        }
        return Result::success(std::move(mesh));
    } catch (const std::bad_alloc&) {
        return Result::failure(PerspectiveMappingError::AllocationFailed);
    }
}

core::Result<MeshPerspectiveView, PerspectiveMappingError>
mesh_perspective_view_for(const ProjectionMesh& mesh, scene::Vec3 eye,
                          float near_plane, float far_plane,
                          scene::Vec3 stage_up) noexcept {
    using Result =
        core::Result<MeshPerspectiveView, PerspectiveMappingError>;
    if (!valid(mesh)) {
        return Result::failure(PerspectiveMappingError::InvalidMesh);
    }
    if (!finite(eye) || !finite(stage_up)
        || !(scene::length(stage_up) > 1.0e-20F)) {
        return Result::failure(PerspectiveMappingError::InvalidEye);
    }
    if (!finite(near_plane) || !finite(far_plane) || !(near_plane > 0.0F)
        || !(far_plane > near_plane)) {
        return Result::failure(PerspectiveMappingError::InvalidClipRange);
    }

    scene::Vec3 minimum = mesh.vertices[mesh.indices.front()].position;
    scene::Vec3 maximum = minimum;
    for (const core::u32 index : mesh.indices) {
        const scene::Vec3 position = mesh.vertices[index].position;
        minimum.x = std::min(minimum.x, position.x);
        minimum.y = std::min(minimum.y, position.y);
        minimum.z = std::min(minimum.z, position.z);
        maximum.x = std::max(maximum.x, position.x);
        maximum.y = std::max(maximum.y, position.y);
        maximum.z = std::max(maximum.z, position.z);
    }
    const scene::Vec3 centre = (minimum + maximum) * 0.5F;
    const scene::Vec3 to_centre = centre - eye;
    if (!finite(to_centre) || !(scene::length(to_centre) > 1.0e-6F)) {
        return Result::failure(PerspectiveMappingError::DegenerateFrustum);
    }
    const scene::Vec3 forward = scene::normalize(to_centre);
    scene::Vec3 up = scene::normalize(stage_up);
    // A calibrated patch directly above/below the viewpoint needs a stable
    // fallback axis. Pick it deterministically; do not let look_at collapse.
    if (std::abs(scene::dot(forward, up)) > 0.999F) {
        up = std::abs(forward.z) < 0.999F
            ? scene::Vec3{0.0F, 0.0F, 1.0F}
            : scene::Vec3{1.0F, 0.0F, 0.0F};
    }
    const scene::Vec3 right = scene::normalize(scene::cross(forward, up));
    up = scene::normalize(scene::cross(right, forward));
    const scene::Mat4 view = scene::look_at(eye, eye + forward, up);

    float left = std::numeric_limits<float>::infinity();
    float right_edge = -std::numeric_limits<float>::infinity();
    float bottom = std::numeric_limits<float>::infinity();
    float top = -std::numeric_limits<float>::infinity();
    for (const core::u32 index : mesh.indices) {
        const scene::Vec3 position = mesh.vertices[index].position;
        const scene::Vec4 camera = scene::transform(
            view, scene::Vec4{position.x, position.y, position.z, 1.0F});
        const float depth = -camera.z;
        if (!finite(camera) || !(depth > 1.0e-6F)) {
            return Result::failure(PerspectiveMappingError::MeshBehindEye);
        }
        const float scale = near_plane / depth;
        const float x = camera.x * scale;
        const float y = camera.y * scale;
        left = std::min(left, x);
        right_edge = std::max(right_edge, x);
        bottom = std::min(bottom, y);
        top = std::max(top, y);
    }
    if (!finite(left) || !finite(right_edge) || !finite(bottom)
        || !finite(top) || !(right_edge > left) || !(top > bottom)) {
        return Result::failure(PerspectiveMappingError::DegenerateFrustum);
    }

    const scene::FrustumWindow frustum{left, right_edge, bottom, top,
                                        near_plane, far_plane};
    const scene::Mat4 projection = projection_for(frustum);
    const scene::Mat4 view_projection = scene::multiply(projection, view);
    if (!finite(view) || !finite(projection) || !finite(view_projection)) {
        return Result::failure(PerspectiveMappingError::DegenerateFrustum);
    }
    return Result::success(MeshPerspectiveView{
        frustum,
        scene::ViewMatrices{view, projection, view_projection},
        eye,
        forward,
        up,
    });
}

core::Result<std::vector<MeshWarpVertex>, PerspectiveMappingError>
mesh_warp_vertices(const ProjectionMesh& mesh,
                   const MeshPerspectiveView& view) noexcept {
    using Result = core::Result<std::vector<MeshWarpVertex>,
                                PerspectiveMappingError>;
    if (!valid(mesh)) {
        return Result::failure(PerspectiveMappingError::InvalidMesh);
    }
    if (!finite(view.eye) || !finite(view.forward) || !finite(view.up)
        || !finite(view.matrices.view)
        || !finite(view.matrices.projection)
        || !finite(view.matrices.view_projection)) {
        return Result::failure(PerspectiveMappingError::InvalidEye);
    }

    try {
        std::vector<MeshWarpVertex> resolved;
        resolved.reserve(mesh.vertices.size());
        for (const ProjectionMeshVertex& vertex : mesh.vertices) {
            const scene::Vec4 source = scene::transform(
                view.matrices.view_projection,
                scene::Vec4{vertex.position.x, vertex.position.y,
                            vertex.position.z, 1.0F});
            if (!finite(source)) {
                return Result::failure(PerspectiveMappingError::InvalidMesh);
            }
            resolved.push_back(MeshWarpVertex{
                scene::Vec4{vertex.destination_u * 2.0F - 1.0F,
                            1.0F - vertex.destination_v * 2.0F, 0.0F, 1.0F},
                source,
            });
        }
        return Result::success(std::move(resolved));
    } catch (const std::bad_alloc&) {
        return Result::failure(PerspectiveMappingError::AllocationFailed);
    }
}

scene::CanvasProjection project_through_mesh_warp(
    const ProjectionMesh& mesh,
    const MeshPerspectiveView& view,
    scene::Canvas destination,
    scene::Vec3 world_point) noexcept {
    if (!valid(mesh) || !scene::valid(destination)) {
        return scene::CanvasProjection{};
    }
    const scene::Vec4 target = scene::transform(
        view.matrices.view_projection,
        scene::Vec4{world_point.x, world_point.y, world_point.z, 1.0F});
    if (!(target.w > 0.0F) || !finite(target)) {
        return scene::CanvasProjection{};
    }
    const float target_x = target.x / target.w;
    const float target_y = target.y / target.w;
    const float depth = target.z / target.w;
    constexpr float inside_epsilon = 1.0e-4F;
    constexpr float determinant_epsilon = 1.0e-8F;
    const auto determinant = [](float a00, float a01, float a02, float a10,
                                float a11, float a12, float a20, float a21,
                                float a22) noexcept {
        return a00 * (a11 * a22 - a12 * a21) -
               a01 * (a10 * a22 - a12 * a20) +
               a02 * (a10 * a21 - a11 * a20);
    };

    for (std::size_t index = 0U; index < mesh.indices.size(); index += 3U) {
        const std::array<core::u32, 3U> triangle{
            mesh.indices[index], mesh.indices[index + 1U],
            mesh.indices[index + 2U]};
        std::array<scene::Vec4, 3U> clips{};
        std::array<float, 3U> equation_x{};
        std::array<float, 3U> equation_y{};
        for (std::size_t corner = 0U; corner < triangle.size(); ++corner) {
            const ProjectionMeshVertex& vertex = mesh.vertices[triangle[corner]];
            clips[corner] = scene::transform(
                view.matrices.view_projection,
                scene::Vec4{vertex.position.x, vertex.position.y,
                            vertex.position.z, 1.0F});
            equation_x[corner] = clips[corner].x - target_x * clips[corner].w;
            equation_y[corner] = clips[corner].y - target_y * clips[corner].w;
        }
        const float denominator = determinant(
            equation_x[0U], equation_x[1U], equation_x[2U],
            equation_y[0U], equation_y[1U], equation_y[2U], 1.0F, 1.0F,
            1.0F);
        if (!std::isfinite(denominator) ||
            std::abs(denominator) <= determinant_epsilon) {
            continue;
        }
        const float lambda0 = determinant(
            0.0F, equation_x[1U], equation_x[2U], 0.0F, equation_y[1U],
            equation_y[2U], 1.0F, 1.0F, 1.0F) /
                              denominator;
        const float lambda1 = determinant(
            equation_x[0U], 0.0F, equation_x[2U], equation_y[0U], 0.0F,
            equation_y[2U], 1.0F, 1.0F, 1.0F) /
                              denominator;
        const float lambda2 = 1.0F - lambda0 - lambda1;
        if (lambda0 < -inside_epsilon || lambda1 < -inside_epsilon ||
            lambda2 < -inside_epsilon) {
            continue;
        }
        const ProjectionMeshVertex& a = mesh.vertices[triangle[0U]];
        const ProjectionMeshVertex& b = mesh.vertices[triangle[1U]];
        const ProjectionMeshVertex& c = mesh.vertices[triangle[2U]];
        const float u = lambda0 * a.destination_u +
                        lambda1 * b.destination_u +
                        lambda2 * c.destination_u;
        const float v = lambda0 * a.destination_v +
                        lambda1 * b.destination_v +
                        lambda2 * c.destination_v;
        return scene::CanvasProjection{
            u * static_cast<float>(destination.width),
            v * static_cast<float>(destination.height), depth, true};
    }
    return scene::CanvasProjection{0.0F, 0.0F, depth, false};
}

} // namespace mediaserver::output
