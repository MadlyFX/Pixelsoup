#include <mediaserver/render/froxel_grid.hpp>

#include <algorithm>
#include <cmath>

namespace mediaserver::render {
namespace {

// A froxel column covers `canvas.width / limits.width` pixels, and the division
// is done in the numerator so the boundary between two columns is the same
// expression from either side - the same discipline `window_for_slice` uses for
// the frustum edge two nodes share.
[[nodiscard]] core::u32 cell_of(core::u32 position, core::u32 extent,
                                core::u32 cells) noexcept {
    if (extent == 0U || cells == 0U) {
        return 0U;
    }
    const core::u64 scaled =
        static_cast<core::u64>(position) * static_cast<core::u64>(cells);
    const auto index = static_cast<core::u32>(scaled / extent);
    return std::min(index, cells - 1U);
}

} // namespace

float froxel_slice_depth(float near_plane, float far_plane, core::u32 index,
                         core::u32 depth) noexcept {
    if (!(far_plane > near_plane) || depth == 0U) {
        return near_plane;
    }
    const core::u32 clamped = std::min(index, depth);
    const float fraction =
        static_cast<float>(clamped) / static_cast<float>(depth);
    return near_plane + (far_plane - near_plane) * fraction * fraction;
}

core::u32 froxel_slice_of(float near_plane, float far_plane, float view_depth,
                          core::u32 depth) noexcept {
    if (!(far_plane > near_plane) || depth == 0U) {
        return 0U;
    }
    if (!(view_depth > near_plane)) {
        return 0U;
    }
    if (view_depth >= far_plane) {
        return depth - 1U;
    }
    // The inverse of the warp above: t = sqrt((z - near) / (far - near)), and
    // the slice is floor(t * depth). `std::sqrt` is correctly rounded by IEEE
    // 754 - it is the one transcendental-looking operation that is actually
    // exact to the last bit on every conforming implementation, which is why it
    // is allowed here where a `pow` would not be.
    const float fraction = (view_depth - near_plane) / (far_plane - near_plane);
    const float warped = std::sqrt(std::max(fraction, 0.0F));
    const auto index =
        static_cast<core::u32>(warped * static_cast<float>(depth));
    return std::min(index, depth - 1U);
}

core::u32 froxel_column_of(scene::Canvas canvas, core::u32 canvas_x,
                           core::u32 width) noexcept {
    return cell_of(canvas_x, canvas.width, width);
}

core::u32 froxel_row_of(scene::Canvas canvas, core::u32 canvas_y,
                        core::u32 height) noexcept {
    return cell_of(canvas_y, canvas.height, height);
}

core::Result<FroxelGrid, FroxelGridError> plan_froxel_grid(
    scene::Canvas canvas, scene::CanvasRect render, const scene::Camera& camera,
    const FroxelGridLimits& limits) noexcept {
    using Result = core::Result<FroxelGrid, FroxelGridError>;
    if (!scene::valid(canvas)) {
        return Result::failure(FroxelGridError::InvalidCanvas);
    }
    if (!scene::contains(canvas, render) || render.width == 0U
        || render.height == 0U) {
        return Result::failure(FroxelGridError::InvalidSlice);
    }
    if (!scene::valid(camera) || !(camera.far_plane > camera.near_plane)) {
        return Result::failure(FroxelGridError::InvalidCamera);
    }
    if (limits.width == 0U || limits.height == 0U || limits.depth == 0U) {
        return Result::failure(FroxelGridError::InvalidExtent);
    }

    FroxelGrid grid{};
    grid.limits = limits;
    // The camera's own planes, which are canvas-wide by construction: a slice
    // narrows the frustum's *window*, never its depth range.
    grid.near_plane = camera.near_plane;
    grid.far_plane = camera.far_plane;

    // The froxels this node covers, widened outward to whole cells. A froxel
    // straddling the slice edge is integrated by both neighbours - identically,
    // because the cell is the canvas's - and each keeps the part of it that
    // lands in its own pixels.
    const core::u32 first_column =
        froxel_column_of(canvas, render.x, limits.width);
    const core::u32 last_column =
        froxel_column_of(canvas, render.x + render.width - 1U, limits.width);
    const core::u32 first_row = froxel_row_of(canvas, render.y, limits.height);
    const core::u32 last_row =
        froxel_row_of(canvas, render.y + render.height - 1U, limits.height);
    grid.coverage = FroxelCoverage{first_column, first_row,
                                   last_column - first_column + 1U,
                                   last_row - first_row + 1U};
    return Result::success(grid);
}

} // namespace mediaserver::render
