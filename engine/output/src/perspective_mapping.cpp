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

struct MeshPlacementBasis final {
    scene::Vec3 center{};
    scene::Vec3 right{};
    scene::Vec3 up{};
    scene::Vec3 front{};
    float scale{1.0F};
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

[[nodiscard]] bool placement_basis(
    const ProjectionMeshPlacement& placement,
    MeshPlacementBasis& into) noexcept {
    if (!valid(placement.calibration) || !finite(placement.scale) ||
        !(placement.scale > 0.0F)) {
        return false;
    }
    into.center = placement.calibration.center;
    into.right = scene::normalize(placement.calibration.right);
    const scene::Vec3 rejected =
        placement.calibration.up -
        into.right * scene::dot(placement.calibration.up, into.right);
    if (!(scene::length(rejected) > 1.0e-8F)) {
        return false;
    }
    into.up = scene::normalize(rejected);
    into.front = scene::normalize(scene::cross(into.right, into.up));
    into.scale = placement.scale;
    return finite(into.center) && finite(into.right) && finite(into.up) &&
           finite(into.front);
}

[[nodiscard]] scene::Vec3 placed_position(
    scene::Vec3 local, const MeshPlacementBasis& placement) noexcept {
    const scene::Vec3 scaled = local * placement.scale;
    return placement.center + placement.right * scaled.x +
           placement.up * scaled.y + placement.front * scaled.z;
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

namespace {

// The wall-intersection half of an inner-frustum region, shared by every form
// of it.
//
// Factored because it was not: the lens-aware overload was a copy of this, so a
// correctness fix applied to one silently left the other behind - which is
// exactly what happened, and the copy kept the conflation between "faces the
// wall and overshoots" and "cannot see the wall at all" for a release after the
// original was fixed. One body, one behaviour.
[[nodiscard]] core::Result<scene::InnerFrustumRegion, PerspectiveMappingError>
region_from_corners(const PhysicalSurface& surface,
                    const scene::Camera& tracked,
                    const scene::FrustumCorners& corners,
                    core::u32 margin_px) noexcept {
    using RegionResult =
        core::Result<scene::InnerFrustumRegion, PerspectiveMappingError>;
    const SurfaceBasis basis = basis_for(surface);
    // The whole raster, marked clamped: the answer whenever the frustum faces
    // the wall but is not bounded by it. Wasteful, never wrong.
    const scene::InnerFrustumRegion whole_surface{
        scene::whole(surface.raster), true, true};
    // No plate at all. Distinct from the clamp above, and the distinction is
    // the whole point: a camera that cannot see this wall must draw nothing on
    // it. Answering "the whole wall" there is not conservative, it is wrong in
    // the expensive direction - it puts a full-wall plate up, and it tells the
    // operator the plate covers 100% of the wall at exactly the moment the
    // camera is pointing away from it.
    const scene::InnerFrustumRegion no_region{scene::CanvasRect{}, false,
                                              false};

    // `normal` points out of the authored front face, so the eye is in front
    // when its signed distance is positive. Behind the wall there is no plate,
    // for the same reason `perspective_view_for` refuses that eye rather than
    // silently mirroring the wall.
    const float eye_in_front =
        scene::dot(basis.normal, tracked.eye - surface.center);
    if (!finite(eye_in_front) || !(eye_in_front > 1.0e-6F)) {
        return RegionResult::success(no_region);
    }

    const float plane_offset =
        scene::dot(basis.normal, surface.center - tracked.eye);
    std::array<scene::CanvasProjection, 4U> hits{};
    // A frustum may straddle the wall's plane: some corner rays reach it and
    // some diverge. A straddle still has to be covered outward, so one missing
    // corner clamps to the whole raster. Every corner missing is the other
    // case - the camera is facing away - and draws nothing.
    std::size_t missed = 0U;
    for (std::size_t index = 0U; index < hits.size(); ++index) {
        // The far corners give the widest ray directions; the near corners lie
        // along the same four rays, so either would do.
        const scene::Vec3 direction = corners.points[index + 4U] - tracked.eye;
        const float along = scene::dot(basis.normal, direction);
        if (!finite(along) || along == 0.0F) {
            ++missed;
            continue;
        }
        const float distance = plane_offset / along;
        if (!finite(distance) || !(distance > 0.0F)) {
            ++missed;
            continue;
        }
        const scene::Vec3 hit = tracked.eye + direction * distance;
        if (!finite(hit)) {
            return RegionResult::success(whole_surface);
        }
        // The inverse of horizontal_edge/vertical_edge. Both nodes evaluate
        // this expression over the same replicated numbers.
        const scene::Vec3 local = hit - surface.center;
        const float across = scene::dot(local, basis.right);
        const float up = scene::dot(local, basis.up);
        hits[index] = scene::CanvasProjection{
            (across / surface.width + 0.5F)
                * static_cast<float>(surface.raster.width),
            (0.5F - up / surface.height)
                * static_cast<float>(surface.raster.height),
            0.0F,
            true,
        };
        if (!finite(hits[index].x) || !finite(hits[index].y)) {
            return RegionResult::success(whole_surface);
        }
    }

    if (missed == hits.size()) {
        return RegionResult::success(no_region);
    }
    if (missed != 0U) {
        return RegionResult::success(whole_surface);
    }
    return RegionResult::success(
        scene::inner_frustum_rect(surface.raster, hits, margin_px));
}

} // namespace

core::Result<scene::InnerFrustumRegion, PerspectiveMappingError>
inner_frustum_region(const PhysicalSurface& surface,
                     const scene::Camera& tracked,
                     float aspect_ratio,
                     core::u32 margin_px) noexcept {
    using RegionResult =
        core::Result<scene::InnerFrustumRegion, PerspectiveMappingError>;
    if (!valid(surface)) {
        return RegionResult::failure(
            PerspectiveMappingError::InvalidSurface);
    }
    if (!scene::valid(tracked)) {
        return RegionResult::failure(PerspectiveMappingError::InvalidEye);
    }
    if (!finite(aspect_ratio) || !(aspect_ratio > 0.0F)) {
        return RegionResult::failure(
            PerspectiveMappingError::DegenerateFrustum);
    }
    const scene::FrustumCorners corners =
        scene::frustum_corners(tracked, aspect_ratio);
    if (!corners.valid) {
        return RegionResult::failure(
            PerspectiveMappingError::DegenerateFrustum);
    }
    return region_from_corners(surface, tracked, corners, margin_px);
}

core::Result<scene::InnerFrustumRegion, PerspectiveMappingError>
inner_frustum_region(const PhysicalSurface& surface,
                     const scene::Camera& tracked,
                     const scene::LensProfile& profile,
                     const scene::LensEncoders& encoders,
                     core::u32 margin_px) noexcept {
    using RegionResult =
        core::Result<scene::InnerFrustumRegion, PerspectiveMappingError>;
    if (!valid(surface)) {
        return RegionResult::failure(PerspectiveMappingError::InvalidSurface);
    }
    // The calibrated body: sensor and focal length from the profile at these
    // encoders, widened to the conservative distorted extent. Both matter and
    // for different reasons - the profile is what makes the rectangle follow a
    // zoom, and the distorted extent is what stops a wide lens photographing
    // outer frustum at the corners of its own plate. The render itself stays
    // undistorted; only the coverage this sizes is widened.
    const scene::FrustumCorners corners = scene::frustum_corners_for_lens(
        tracked, profile, encoders, true);
    if (!corners.valid) {
        return RegionResult::failure(
            PerspectiveMappingError::DegenerateFrustum);
    }
    return region_from_corners(surface, tracked, corners, margin_px);
}

scene::InnerFrustumRegion inner_frustum_region(
    const ProjectionMesh& mesh,
    scene::Canvas destination,
    const scene::Camera& tracked,
    float aspect_ratio,
    core::u32 margin_px) noexcept {
    if (!valid(mesh) || !scene::valid(destination) || !scene::valid(tracked)
        || !finite(aspect_ratio) || !(aspect_ratio > 0.0F)) {
        return scene::InnerFrustumRegion{};
    }

    const scene::ViewMatrices view =
        scene::view_for_aspect(tracked, aspect_ratio);
    if (!finite(view.view_projection)) {
        return scene::InnerFrustumRegion{};
    }

    // One outside-flag per side clip plane, in clip space so a vertex behind
    // the eye classifies correctly instead of folding through the divide.
    // Near and far are deliberately absent: the camera physically sees the wall
    // whether or not the wall lies inside the render's depth range.
    struct Classified final {
        core::u8 outside{};
    };
    constexpr core::u8 outside_left = 1U << 0U;
    constexpr core::u8 outside_right = 1U << 1U;
    constexpr core::u8 outside_bottom = 1U << 2U;
    constexpr core::u8 outside_top = 1U << 3U;

    std::vector<Classified> classified;
    std::vector<scene::CanvasProjection> covered;
    try {
        classified.resize(mesh.vertices.size());
        covered.reserve(mesh.indices.size());
    } catch (const std::bad_alloc&) {
        return scene::InnerFrustumRegion{};
    }
    for (std::size_t index = 0U; index < mesh.vertices.size(); ++index) {
        const scene::Vec3 position = mesh.vertices[index].position;
        const scene::Vec4 clip = scene::transform(
            view.view_projection,
            scene::Vec4{position.x, position.y, position.z, 1.0F});
        core::u8 flags = 0U;
        if (!finite(clip)) {
            // An unusable vertex is inside every plane, which keeps it in the
            // bound rather than quietly shrinking the rectangle.
            classified[index] = Classified{0U};
            continue;
        }
        if (clip.x < -clip.w) {
            flags |= outside_left;
        }
        if (clip.x > clip.w) {
            flags |= outside_right;
        }
        if (clip.y < -clip.w) {
            flags |= outside_bottom;
        }
        if (clip.y > clip.w) {
            flags |= outside_top;
        }
        classified[index] = Classified{flags};
    }

    for (std::size_t triangle = 0U; triangle + 2U < mesh.indices.size();
         triangle += 3U) {
        const core::u32 a = mesh.indices[triangle];
        const core::u32 b = mesh.indices[triangle + 1U];
        const core::u32 c = mesh.indices[triangle + 2U];
        const core::u8 shared = static_cast<core::u8>(
            classified[a].outside & classified[b].outside
            & classified[c].outside);
        if (shared != 0U) {
            continue;
        }
        for (const core::u32 index : {a, b, c}) {
            const ProjectionMeshVertex& vertex = mesh.vertices[index];
            // Capacity was reserved for every index, so this cannot grow.
            covered.push_back(scene::CanvasProjection{
                vertex.destination_u * static_cast<float>(destination.width),
                vertex.destination_v * static_cast<float>(destination.height),
                0.0F,
                true,
            });
        }
    }
    if (covered.empty()) {
        return scene::InnerFrustumRegion{};
    }
    return scene::inner_frustum_rect(destination, covered, margin_px);
}

scene::InnerFrustumRegion inner_frustum_region(
    const ProjectionMesh& mesh,
    scene::Canvas destination,
    const scene::Camera& tracked,
    const scene::LensProfile& profile,
    const scene::LensEncoders& encoders,
    core::u32 margin_px) noexcept {
    if (!valid(mesh) || !scene::valid(destination)) {
        return scene::InnerFrustumRegion{};
    }
    const scene::FrustumWindow coverage = scene::distorted_window_for_lens(
        tracked, profile, encoders);
    if (!(coverage.right > coverage.left) || !(coverage.top > coverage.bottom)
        || !(coverage.near_plane > 0.0F)
        || !(coverage.far_plane > coverage.near_plane)) {
        return scene::InnerFrustumRegion{};
    }
    const scene::ViewMatrices view = scene::view_for_distorted_lens(
        tracked, profile, encoders);
    if (!finite(view.view_projection)) {
        return scene::InnerFrustumRegion{};
    }

    struct Classified final {
        core::u8 outside{};
    };
    constexpr core::u8 outside_left = 1U << 0U;
    constexpr core::u8 outside_right = 1U << 1U;
    constexpr core::u8 outside_bottom = 1U << 2U;
    constexpr core::u8 outside_top = 1U << 3U;
    std::vector<Classified> classified;
    std::vector<scene::CanvasProjection> covered;
    try {
        classified.resize(mesh.vertices.size());
        covered.reserve(mesh.indices.size());
    } catch (const std::bad_alloc&) {
        return scene::InnerFrustumRegion{};
    }
    for (std::size_t index = 0U; index < mesh.vertices.size(); ++index) {
        const scene::Vec3 position = mesh.vertices[index].position;
        const scene::Vec4 clip = scene::transform(
            view.view_projection,
            scene::Vec4{position.x, position.y, position.z, 1.0F});
        core::u8 flags = 0U;
        if (!finite(clip)) {
            classified[index] = Classified{};
            continue;
        }
        if (clip.x < -clip.w) {
            flags |= outside_left;
        }
        if (clip.x > clip.w) {
            flags |= outside_right;
        }
        if (clip.y < -clip.w) {
            flags |= outside_bottom;
        }
        if (clip.y > clip.w) {
            flags |= outside_top;
        }
        classified[index] = Classified{flags};
    }
    for (std::size_t triangle = 0U; triangle + 2U < mesh.indices.size();
         triangle += 3U) {
        const core::u32 a = mesh.indices[triangle];
        const core::u32 b = mesh.indices[triangle + 1U];
        const core::u32 c = mesh.indices[triangle + 2U];
        const core::u8 shared = static_cast<core::u8>(
            classified[a].outside & classified[b].outside
            & classified[c].outside);
        if (shared != 0U) {
            continue;
        }
        for (const core::u32 index : {a, b, c}) {
            const ProjectionMeshVertex& vertex = mesh.vertices[index];
            covered.push_back(scene::CanvasProjection{
                vertex.destination_u * static_cast<float>(destination.width),
                vertex.destination_v * static_cast<float>(destination.height),
                0.0F,
                true,
            });
        }
    }
    return covered.empty()
        ? scene::InnerFrustumRegion{}
        : scene::inner_frustum_rect(destination, covered, margin_px);
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

bool valid(const ProjectionMeshPlacement& placement) noexcept {
    MeshPlacementBasis unused{};
    return placement_basis(placement, unused);
}

scene::Vec3 place_projection_mesh_position(
    scene::Vec3 local_position,
    const ProjectionMeshPlacement& placement) noexcept {
    MeshPlacementBasis basis{};
    if (!finite(local_position) || !placement_basis(placement, basis)) {
        const float invalid = std::numeric_limits<float>::quiet_NaN();
        return scene::Vec3{invalid, invalid, invalid};
    }
    return placed_position(local_position, basis);
}

ProjectionMeshPlacement stage_mesh_placement(const StagePose& pose) noexcept {
    // A unit "surface" whose basis is the stage's own axes: placed_position
    // then reads origin + right*x + up*y + back*z, which is stage_point.
    PhysicalSurface frame{};
    frame.center = pose.origin;
    frame.right = pose.right;
    frame.up = pose.up;
    frame.width = 1.0F;
    frame.height = 1.0F;
    frame.raster = scene::Canvas{1U, 1U};
    return ProjectionMeshPlacement{frame, 1.0F};
}

core::Result<StagePose, PerspectiveMappingError>
camera_stage_pose(const scene::Camera& camera, scene::Vec3 eye) noexcept {
    using PoseResult = core::Result<StagePose, PerspectiveMappingError>;
    if (!finite(camera.eye) || !finite(camera.target) || !finite(camera.up)
        || !finite(eye)) {
        return PoseResult::failure(PerspectiveMappingError::InvalidStage);
    }
    const scene::Vec3 look = camera.target - camera.eye;
    const float look_length = scene::length(look);
    if (!finite(look_length) || !(look_length > 1.0e-12F)) {
        return PoseResult::failure(PerspectiveMappingError::InvalidStage);
    }
    const scene::Vec3 forward = look * (1.0F / look_length);
    const scene::Vec3 side = scene::cross(forward, camera.up);
    const float side_length = scene::length(side);
    if (!finite(side_length) || !(side_length > 1.0e-12F)) {
        // Looking straight along `up` leaves the roll undefined. Choosing one
        // would turn every face of the room by an amount nobody authored.
        return PoseResult::failure(PerspectiveMappingError::InvalidStage);
    }
    StagePose pose{};
    pose.right = side * (1.0F / side_length);
    pose.up = scene::cross(pose.right, forward);
    pose.back = -forward;
    pose.origin = camera.eye
        - (pose.right * eye.x + pose.up * eye.y + pose.back * eye.z);
    if (!finite(pose.origin) || !finite(pose.right) || !finite(pose.up)
        || !finite(pose.back)) {
        return PoseResult::failure(PerspectiveMappingError::InvalidStage);
    }
    return PoseResult::success(pose);
}

scene::Vec3 stage_point(const StagePose& pose, scene::Vec3 point) noexcept {
    return pose.origin + pose.right * point.x + pose.up * point.y
        + pose.back * point.z;
}

scene::Vec3 stage_direction(const StagePose& pose,
                            scene::Vec3 direction) noexcept {
    return pose.right * direction.x + pose.up * direction.y
        + pose.back * direction.z;
}

PhysicalSurface place_on_stage(const PhysicalSurface& surface,
                               const StagePose& pose) noexcept {
    PhysicalSurface placed = surface;
    placed.center = stage_point(pose, surface.center);
    placed.right = stage_direction(pose, surface.right);
    placed.up = stage_direction(pose, surface.up);
    return placed;
}

namespace {

// The horizontal normal of a pitch-zero face with the given right edge, which
// is the direction a positive pitch tips the top edge towards.
[[nodiscard]] scene::Vec3 level_normal(scene::Vec3 level_right) noexcept {
    return scene::Vec3{-level_right.z, 0.0F, level_right.x};
}

void set_yaw_and_pitch_basis(PhysicalSurface& surface, float yaw_turns,
                             SurfacePitchBasis pitch) noexcept {
    const float cosine = scene::deterministic::cos_turns(yaw_turns);
    const float sine = scene::deterministic::sin_turns(yaw_turns);
    surface.right = scene::Vec3{cosine, 0.0F, -sine};
    if (pitch.sine == 0.0F) {
        // Exactly the upright basis an untilted wall has always had (or its
        // half-turn, for a face hung upside down).
        surface.up = scene::Vec3{0.0F, pitch.cosine, 0.0F};
        return;
    }
    const scene::Vec3 normal = level_normal(surface.right);
    surface.up = scene::Vec3{normal.x * pitch.sine, pitch.cosine,
                             normal.z * pitch.sine};
}

} // namespace

void orient(PhysicalSurface& surface, float yaw_turns,
            float pitch_turns) noexcept {
    SurfacePitchBasis pitch{};
    if (pitch_turns != 0.0F) {
        pitch.cosine = scene::deterministic::cos_turns(pitch_turns);
        pitch.sine = scene::deterministic::sin_turns(pitch_turns);
    }
    set_yaw_and_pitch_basis(surface, yaw_turns, pitch);
}

SurfacePitchBasis surface_pitch_basis(const PhysicalSurface& surface) noexcept {
    const scene::Vec3 level = scene::Vec3{surface.right.x, 0.0F,
                                          surface.right.z};
    const float level_length = scene::length(level);
    if (!finite(level_length) || !(level_length > 1.0e-12F)) {
        return SurfacePitchBasis{};
    }
    const scene::Vec3 normal = level_normal(level * (1.0F / level_length));
    const float cosine = surface.up.y;
    const float sine = scene::dot(surface.up, normal);
    const float magnitude = std::sqrt(cosine * cosine + sine * sine);
    if (!finite(magnitude) || !(magnitude > 1.0e-12F)) {
        return SurfacePitchBasis{};
    }
    if (sine == 0.0F) {
        return SurfacePitchBasis{cosine < 0.0F ? -1.0F : 1.0F, 0.0F};
    }
    return SurfacePitchBasis{cosine / magnitude, sine / magnitude};
}

void set_yaw_keeping_pitch(PhysicalSurface& surface, float yaw_turns) noexcept {
    set_yaw_and_pitch_basis(surface, yaw_turns, surface_pitch_basis(surface));
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

[[nodiscard]] static core::Result<MeshPerspectiveView, PerspectiveMappingError>
mesh_perspective_view_for_impl(
    const ProjectionMesh& mesh, scene::Vec3 eye, float near_plane,
    float far_plane, scene::Vec3 stage_up,
    const ProjectionMeshPlacement* placement) noexcept {
    using Result =
        core::Result<MeshPerspectiveView, PerspectiveMappingError>;
    if (!valid(mesh)) {
        return Result::failure(PerspectiveMappingError::InvalidMesh);
    }
    MeshPlacementBasis placed{};
    if (placement != nullptr && !placement_basis(*placement, placed)) {
        return Result::failure(PerspectiveMappingError::InvalidSurface);
    }
    const auto position_for = [&](const ProjectionMeshVertex& vertex) noexcept {
        return placement != nullptr ? placed_position(vertex.position, placed)
                                    : vertex.position;
    };
    if (!finite(eye) || !finite(stage_up)
        || !(scene::length(stage_up) > 1.0e-20F)) {
        return Result::failure(PerspectiveMappingError::InvalidEye);
    }
    if (!finite(near_plane) || !finite(far_plane) || !(near_plane > 0.0F)
        || !(far_plane > near_plane)) {
        return Result::failure(PerspectiveMappingError::InvalidClipRange);
    }

    scene::Vec3 minimum = position_for(mesh.vertices[mesh.indices.front()]);
    if (!finite(minimum)) {
        return Result::failure(PerspectiveMappingError::InvalidMesh);
    }
    scene::Vec3 maximum = minimum;
    for (const core::u32 index : mesh.indices) {
        const scene::Vec3 position = position_for(mesh.vertices[index]);
        if (!finite(position)) {
            return Result::failure(PerspectiveMappingError::InvalidMesh);
        }
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
        const scene::Vec3 position = position_for(mesh.vertices[index]);
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

core::Result<MeshPerspectiveView, PerspectiveMappingError>
mesh_perspective_view_for(const ProjectionMesh& mesh, scene::Vec3 eye,
                          float near_plane, float far_plane,
                          scene::Vec3 stage_up) noexcept {
    return mesh_perspective_view_for_impl(mesh, eye, near_plane, far_plane,
                                          stage_up, nullptr);
}

core::Result<MeshPerspectiveView, PerspectiveMappingError>
mesh_perspective_view_for(const ProjectionMesh& local_mesh,
                          const ProjectionMeshPlacement& placement,
                          scene::Vec3 eye, float near_plane, float far_plane,
                          scene::Vec3 stage_up) noexcept {
    return mesh_perspective_view_for_impl(local_mesh, eye, near_plane,
                                          far_plane, stage_up, &placement);
}

[[nodiscard]] static core::Result<std::vector<MeshWarpVertex>,
                                  PerspectiveMappingError>
mesh_warp_vertices_impl(const ProjectionMesh& mesh,
                        const MeshPerspectiveView& view,
                        const ProjectionMeshPlacement* placement) noexcept {
    using Result = core::Result<std::vector<MeshWarpVertex>,
                                PerspectiveMappingError>;
    if (!valid(mesh)) {
        return Result::failure(PerspectiveMappingError::InvalidMesh);
    }
    MeshPlacementBasis placed{};
    if (placement != nullptr && !placement_basis(*placement, placed)) {
        return Result::failure(PerspectiveMappingError::InvalidSurface);
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
            const scene::Vec3 position =
                placement != nullptr
                    ? placed_position(vertex.position, placed)
                    : vertex.position;
            const scene::Vec4 source = scene::transform(
                view.matrices.view_projection,
                scene::Vec4{position.x, position.y, position.z, 1.0F});
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

core::Result<std::vector<MeshWarpVertex>, PerspectiveMappingError>
mesh_warp_vertices(const ProjectionMesh& mesh,
                   const MeshPerspectiveView& view) noexcept {
    return mesh_warp_vertices_impl(mesh, view, nullptr);
}

core::Result<std::vector<MeshWarpVertex>, PerspectiveMappingError>
mesh_warp_vertices(const ProjectionMesh& local_mesh,
                   const MeshPerspectiveView& view,
                   const ProjectionMeshPlacement& placement) noexcept {
    return mesh_warp_vertices_impl(local_mesh, view, &placement);
}

[[nodiscard]] static scene::CanvasProjection project_through_mesh_warp_impl(
    const ProjectionMesh& mesh,
    const MeshPerspectiveView& view,
    scene::Canvas destination,
    scene::Vec3 world_point,
    const ProjectionMeshPlacement* placement) noexcept {
    if (!valid(mesh) || !scene::valid(destination)) {
        return scene::CanvasProjection{};
    }
    MeshPlacementBasis placed{};
    if (placement != nullptr && !placement_basis(*placement, placed)) {
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
            const scene::Vec3 position =
                placement != nullptr
                    ? placed_position(vertex.position, placed)
                    : vertex.position;
            clips[corner] = scene::transform(
                view.matrices.view_projection,
                scene::Vec4{position.x, position.y, position.z, 1.0F});
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

scene::CanvasProjection project_through_mesh_warp(
    const ProjectionMesh& mesh, const MeshPerspectiveView& view,
    scene::Canvas destination, scene::Vec3 world_point) noexcept {
    return project_through_mesh_warp_impl(mesh, view, destination, world_point,
                                          nullptr);
}

scene::CanvasProjection project_through_mesh_warp(
    const ProjectionMesh& local_mesh, const MeshPerspectiveView& view,
    scene::Canvas destination, scene::Vec3 world_point,
    const ProjectionMeshPlacement& placement) noexcept {
    return project_through_mesh_warp_impl(local_mesh, view, destination,
                                          world_point, &placement);
}

} // namespace mediaserver::output
