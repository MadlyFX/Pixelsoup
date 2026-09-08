#include <mediaserver/render/froxel_grid.hpp>

#include <array>
#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace core = mediaserver::core;
namespace render = mediaserver::render;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define FROXEL_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] scene::Camera test_camera() {
    scene::Camera camera{};
    camera.eye = scene::Vec3{0.0F, 1.5F, 4.0F};
    camera.target = scene::Vec3{0.0F, 0.5F, 0.0F};
    camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    camera.field_of_view = 0.125F;
    camera.near_plane = 0.1F;
    camera.far_plane = 200.0F;
    return camera;
}

// A canvas pixel falls in the same froxel whichever node is asking.
//
// This is the property the whole file exists for, and it is asserted on the
// partition rather than on rendered pixels because that is where it lives. A
// node that cut the volume by its own rectangle would light a shared world
// point with two different integrations, and the seam would move whenever the
// camera did.
void a_canvas_pixel_lands_in_one_froxel() {
    const scene::Canvas canvas{2560U, 720U};
    const render::FroxelGridLimits limits{};

    // Every column boundary is a function of the canvas and the grid, never of
    // where a node happens to start.
    for (core::u32 x = 0U; x < canvas.width; x += 7U) {
        const core::u32 column =
            render::froxel_column_of(canvas, x, limits.width);
        FROXEL_CHECK(column < limits.width);
        // Monotonic, and it never runs off the end.
        if (x > 0U) {
            FROXEL_CHECK(column
                         >= render::froxel_column_of(canvas, x - 7U,
                                                     limits.width));
        }
    }
    FROXEL_CHECK(render::froxel_column_of(canvas, 0U, limits.width) == 0U);
    FROXEL_CHECK(render::froxel_column_of(canvas, canvas.width - 1U,
                                          limits.width)
                 == limits.width - 1U);
    FROXEL_CHECK(render::froxel_row_of(canvas, canvas.height - 1U,
                                       limits.height)
                 == limits.height - 1U);
}

// The depth partition inverts exactly, including on its own boundaries.
//
// A point that lands on a boundary must not fall in one cell for one node and
// the next cell for its neighbour. Round-tripping the boundary itself is the
// case that would otherwise be off by one, and it is the case a scene puts
// geometry on constantly - a wall at a round distance is not unusual.
void the_depth_partition_inverts() {
    constexpr float near_plane = 0.1F;
    constexpr float far_plane = 200.0F;
    constexpr core::u32 depth = 64U;

    FROXEL_CHECK(render::froxel_slice_depth(near_plane, far_plane, 0U, depth)
                 == near_plane);
    FROXEL_CHECK(render::froxel_slice_depth(near_plane, far_plane, depth, depth)
                 == far_plane);

    float previous = near_plane;
    for (core::u32 index = 1U; index <= depth; ++index) {
        const float boundary =
            render::froxel_slice_depth(near_plane, far_plane, index, depth);
        // Strictly increasing, or two slices describe the same volume and one
        // of them is dead.
        FROXEL_CHECK(boundary > previous);
        // The start of slice `index` belongs to slice `index`, and a hair
        // before it belongs to the one before.
        FROXEL_CHECK(render::froxel_slice_of(near_plane, far_plane, boundary,
                                             depth)
                     >= index - 1U);
        const float inside = (previous + boundary) * 0.5F;
        FROXEL_CHECK(render::froxel_slice_of(near_plane, far_plane, inside,
                                             depth)
                     == index - 1U);
        previous = boundary;
    }

    // Outside the range clamps rather than wrapping. A point behind the camera
    // is not in the last slice.
    FROXEL_CHECK(render::froxel_slice_of(near_plane, far_plane, -5.0F, depth)
                 == 0U);
    FROXEL_CHECK(render::froxel_slice_of(near_plane, far_plane, 1.0e9F, depth)
                 == depth - 1U);

    // Slices are denser near the camera, which is the entire reason for the
    // warp: half the slices sit in well under half the distance.
    const float middle =
        render::froxel_slice_depth(near_plane, far_plane, depth / 2U, depth);
    FROXEL_CHECK(middle < (near_plane + far_plane) * 0.5F);
}

// Two slices cover the canvas grid exactly as one whole-canvas render does.
void two_slices_cover_the_same_grid() {
    const scene::Canvas canvas{2560U, 720U};
    const render::FroxelGridLimits limits{};
    const scene::Camera camera = test_camera();

    const auto whole = render::plan_froxel_grid(
        canvas, scene::CanvasRect{0U, 0U, canvas.width, canvas.height}, camera,
        limits);
    FROXEL_CHECK(whole.has_value());
    FROXEL_CHECK(whole.value().coverage
                 == render::FroxelCoverage{0U, 0U, limits.width,
                                           limits.height});

    const core::u32 half = canvas.width / 2U;
    const auto left = render::plan_froxel_grid(
        canvas, scene::CanvasRect{0U, 0U, half, canvas.height}, camera, limits);
    const auto right = render::plan_froxel_grid(
        canvas, scene::CanvasRect{half, 0U, half, canvas.height}, camera,
        limits);
    FROXEL_CHECK(left.has_value());
    FROXEL_CHECK(right.has_value());

    // The depth range is the camera's, so it is identical on both - a slice
    // narrows the frustum's window and never its near or far plane.
    FROXEL_CHECK(left.value().near_plane == whole.value().near_plane);
    FROXEL_CHECK(left.value().far_plane == whole.value().far_plane);
    FROXEL_CHECK(right.value().near_plane == whole.value().near_plane);
    FROXEL_CHECK(right.value().far_plane == whole.value().far_plane);

    // Together the two halves cover every column, and they meet without a gap.
    // They may overlap by the one froxel that straddles the seam, which is
    // correct: both integrate it, identically, and each keeps the pixels that
    // land in its own rectangle.
    FROXEL_CHECK(left.value().coverage.x == 0U);
    const core::u32 left_end =
        left.value().coverage.x + left.value().coverage.width;
    FROXEL_CHECK(right.value().coverage.x <= left_end);
    FROXEL_CHECK(right.value().coverage.x + right.value().coverage.width
                 == limits.width);
    FROXEL_CHECK(left.value().coverage.height == limits.height);
    FROXEL_CHECK(right.value().coverage.height == limits.height);

    // And every canvas pixel of each half maps into that half's coverage.
    for (core::u32 x = 0U; x < half; x += 13U) {
        const core::u32 column =
            render::froxel_column_of(canvas, x, limits.width);
        FROXEL_CHECK(column >= left.value().coverage.x);
        FROXEL_CHECK(column < left_end);
    }
    for (core::u32 x = half; x < canvas.width; x += 13U) {
        const core::u32 column =
            render::froxel_column_of(canvas, x, limits.width);
        FROXEL_CHECK(column >= right.value().coverage.x);
        FROXEL_CHECK(column < right.value().coverage.x
                                  + right.value().coverage.width);
    }
}

// A degenerate grid or camera is refused rather than silently emptied.
void a_degenerate_grid_is_refused() {
    const scene::Canvas canvas{1920U, 1080U};
    const scene::Camera camera = test_camera();

    render::FroxelGridLimits empty{};
    empty.depth = 0U;
    const auto no_depth = render::plan_froxel_grid(
        canvas, scene::CanvasRect{0U, 0U, canvas.width, canvas.height}, camera,
        empty);
    FROXEL_CHECK(!no_depth);
    FROXEL_CHECK(no_depth.error() == render::FroxelGridError::InvalidExtent);

    scene::Camera inverted = camera;
    inverted.far_plane = inverted.near_plane;
    const auto no_range = render::plan_froxel_grid(
        canvas, scene::CanvasRect{0U, 0U, canvas.width, canvas.height},
        inverted, render::FroxelGridLimits{});
    FROXEL_CHECK(!no_range);
    FROXEL_CHECK(no_range.error() == render::FroxelGridError::InvalidCamera);

    const auto outside = render::plan_froxel_grid(
        canvas, scene::CanvasRect{0U, 0U, canvas.width + 1U, canvas.height},
        camera, render::FroxelGridLimits{});
    FROXEL_CHECK(!outside);
    FROXEL_CHECK(outside.error() == render::FroxelGridError::InvalidSlice);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 4U> cases{{
    {"a canvas pixel lands in one froxel", &a_canvas_pixel_lands_in_one_froxel},
    {"the depth partition inverts", &the_depth_partition_inverts},
    {"two slices cover the same grid", &two_slices_cover_the_same_grid},
    {"a degenerate grid is refused", &a_degenerate_grid_is_refused},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const auto& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
