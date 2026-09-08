#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/scene/scene.hpp>

// The froxel grid: a view-frustum-aligned partition of the volume in front of
// the camera, which is where scattered light is accumulated.
//
// Same shape of problem as the bloom chain, and the same answer. A froxel is a
// *cell*, and two nodes holding different rectangles of one canvas will cut the
// volume into different cells unless they are made not to - so a world point
// would sit in one node's cell and its neighbour's, be lit by two different
// integrations, and put a discontinuity down the wall that moves whenever the
// camera does. The grid therefore belongs to the **authored full-canvas camera
// frustum**, and a node evaluates the part of it its slice covers. The plan is
// explicit that this is not optional and it is right.
//
// The x and y partition is the canvas divided into columns and rows; the depth
// partition is a fixed warp of the near-to-far range. None of it is derived
// from the node's own rectangle, which is the whole point.
namespace mediaserver::render {

enum class FroxelGridError : core::u8 {
    InvalidCanvas,
    InvalidSlice,
    InvalidCamera,
    InvalidExtent,
};

struct FroxelGridLimits final {
    // The plan's 160x90x64. Replicated policy rather than a per-machine dial:
    // two nodes on different grids is the failure this file exists to prevent,
    // so this is not something a node may lower under load.
    core::u32 width{160U};
    core::u32 height{90U};
    core::u32 depth{64U};

    bool operator==(const FroxelGridLimits&) const = default;
};

// The rows of the grid a node actually evaluates: the columns its slice covers,
// widened to whole froxels because a froxel is either integrated or it is not.
struct FroxelCoverage final {
    core::u32 x{};
    core::u32 y{};
    core::u32 width{};
    core::u32 height{};

    bool operator==(const FroxelCoverage&) const = default;
};

struct FroxelGrid final {
    FroxelGridLimits limits{};
    // The canvas-wide frustum this grid partitions, never the slice's.
    float near_plane{};
    float far_plane{};
    FroxelCoverage coverage{};

    bool operator==(const FroxelGrid&) const = default;
};

// Where slice `index` of the depth partition begins, in view-space units.
//
// Quadratic in the slice fraction: `near + (far - near) * t * t`. Not the
// exponential distribution the references describe, and the reason is the same
// one that decided the bloom kernels and the display transform - an exponential
// needs a transcendental, and a transcendental is exactly the arithmetic this
// renderer does not let two machines evaluate independently. A quadratic warp
// is one multiply, it concentrates slices near the camera where the eye is, and
// every node computes the same number from the same three floats.
[[nodiscard]] float froxel_slice_depth(float near_plane, float far_plane,
                                       core::u32 index,
                                       core::u32 depth) noexcept;

// The depth slice a view-space distance falls in. The inverse of the above, and
// asserted to be so: a point that lands on a boundary must not be in two cells
// depending on which node asked.
[[nodiscard]] core::u32 froxel_slice_of(float near_plane, float far_plane,
                                        float view_depth,
                                        core::u32 depth) noexcept;

// The froxel column and row a canvas pixel falls in. Keyed to the canvas, so a
// node holding half the wall and a node holding all of it agree.
[[nodiscard]] core::u32 froxel_column_of(scene::Canvas canvas,
                                         core::u32 canvas_x,
                                         core::u32 width) noexcept;
[[nodiscard]] core::u32 froxel_row_of(scene::Canvas canvas, core::u32 canvas_y,
                                      core::u32 height) noexcept;

// The grid a node rendering `render` out of `canvas` should evaluate.
//
// `render` is the padded rectangle, as everywhere else: the froxels covering
// the apron are integrated too, because the apply pass reads them and the crop
// happens afterwards.
[[nodiscard]] core::Result<FroxelGrid, FroxelGridError> plan_froxel_grid(
    scene::Canvas canvas, scene::CanvasRect render, const scene::Camera& camera,
    const FroxelGridLimits& limits) noexcept;

} // namespace mediaserver::render
