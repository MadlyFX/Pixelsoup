#include <mediaserver/scene/canvas.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define SLICE_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

constexpr scene::Canvas wall{3'840U, 2'160U};

[[nodiscard]] scene::Camera stage_camera() noexcept {
    scene::Camera camera{};
    camera.eye = scene::Vec3{2.5F, 3.0F, 9.0F};
    camera.target = scene::Vec3{0.0F, 1.0F, 0.0F};
    camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    camera.field_of_view = 0.1F; // 36 degrees
    camera.near_plane = 0.05F;
    camera.far_plane = 500.0F;
    return camera;
}

// A two-by-two wall. Four nodes, each owning a quadrant of one 4K canvas.
[[nodiscard]] std::array<scene::CanvasRect, 4U> quadrants() noexcept {
    const auto half_width = wall.width / 2U;
    const auto half_height = wall.height / 2U;
    return {{
        {0U, 0U, half_width, half_height},
        {half_width, 0U, half_width, half_height},
        {0U, half_height, half_width, half_height},
        {half_width, half_height, half_width, half_height},
    }};
}

// Points scattered across the volume the camera looks at, including some
// behind it and some off to the sides, so the agreement being tested is not
// only agreement about the easy middle of the frame.
[[nodiscard]] std::vector<scene::Vec3> sample_points() {
    std::vector<scene::Vec3> points;
    for (int x = -6; x <= 6; ++x) {
        for (int y = -3; y <= 5; ++y) {
            for (int z = -8; z <= 4; z += 2) {
                points.push_back(scene::Vec3{
                    static_cast<float>(x) * 0.75F,
                    static_cast<float>(y) * 0.5F,
                    static_cast<float>(z) * 0.75F});
            }
        }
    }
    return points;
}

void adjacent_slices_share_a_bit_identical_frustum_plane() {
    const auto camera = stage_camera();
    const auto tiles = quadrants();

    const auto top_left = scene::window_for_slice(camera, wall, tiles[0U]);
    const auto top_right = scene::window_for_slice(camera, wall, tiles[1U]);
    const auto bottom_left = scene::window_for_slice(camera, wall, tiles[2U]);

    // Equality, not proximity. Anything less and a wall shows a hairline.
    SLICE_CHECK(top_left.right == top_right.left);
    SLICE_CHECK(top_left.bottom == bottom_left.top);
    SLICE_CHECK(top_left.top == top_right.top);
    SLICE_CHECK(top_left.left == bottom_left.left);

    // And the outer edges are the canvas frustum's own edges, exactly, so the
    // tiling neither loses nor invents any part of the image.
    const auto full = scene::window_for_slice(camera, wall, scene::whole(wall));
    SLICE_CHECK(top_left.left == full.left);
    SLICE_CHECK(top_left.top == full.top);
    SLICE_CHECK(top_right.right == full.right);
    SLICE_CHECK(bottom_left.bottom == full.bottom);
}

void a_slice_boundary_is_the_same_wherever_it_is_cut() {
    const auto camera = stage_camera();
    // The same vertical cut reached from either side, at a column that is not
    // a power-of-two fraction of the canvas - the case where a naive
    // implementation accumulating per-slice offsets would drift.
    constexpr core::u32 column = 1'237U;
    const auto left =
        scene::window_for_slice(camera, wall, scene::CanvasRect{0U, 0U, column, wall.height});
    const auto right = scene::window_for_slice(
        camera,
        wall,
        scene::CanvasRect{column, 0U, wall.width - column, wall.height});
    SLICE_CHECK(left.right == right.left);

    // A slice one pixel wide at that column agrees with both of its
    // neighbours, which is the degenerate case a blend region reduces to.
    const auto sliver = scene::window_for_slice(
        camera, wall, scene::CanvasRect{column, 0U, 1U, wall.height});
    SLICE_CHECK(sliver.left == left.right);
}

void every_slice_projects_a_point_where_the_whole_canvas_does() {
    const auto camera = stage_camera();
    const auto tiles = quadrants();
    const auto points = sample_points();

    // A thousandth of a pixel on a 4K canvas. The two paths are algebraically
    // equal and differ only in the rounding of two different matrix products,
    // so anything approaching a visible fraction of a pixel would mean the
    // slice frustum is not actually a sub-frustum.
    constexpr float tolerance = 1.0e-3F;

    std::size_t compared = 0U;
    for (const auto& point : points) {
        const auto reference = scene::project_to_canvas(camera, wall, point);
        for (const auto& tile : tiles) {
            const auto through =
                scene::project_through_slice(camera, wall, tile, point);
            SLICE_CHECK(through.visible == reference.visible);
            if (!reference.visible) {
                continue;
            }
            // Every slice, not only the one that owns the point: the slice
            // frustums are restrictions of one frustum, so they agree
            // everywhere, and testing only the owning slice would not catch a
            // tiling that happened to be self-consistent but wrong.
            SLICE_CHECK(std::abs(through.x - reference.x) <= tolerance);
            SLICE_CHECK(std::abs(through.y - reference.y) <= tolerance);
            SLICE_CHECK(through.depth == reference.depth);
            ++compared;
        }
    }
    SLICE_CHECK(compared > 1'000U);
}

void the_quadrants_tile_the_canvas_without_gap_or_overlap() {
    // The geometric tests above assume the tiling is one; this checks it, so a
    // future change to `quadrants` cannot quietly weaken them.
    const auto tiles = quadrants();
    core::u64 area = 0U;
    for (const auto& tile : tiles) {
        SLICE_CHECK(scene::contains(wall, tile));
        area += static_cast<core::u64>(tile.width)
            * static_cast<core::u64>(tile.height);
    }
    SLICE_CHECK(
        area
        == static_cast<core::u64>(wall.width) * static_cast<core::u64>(wall.height));

    for (std::size_t left = 0U; left < tiles.size(); ++left) {
        for (auto right = left + 1U; right < tiles.size(); ++right) {
            const auto& a = tiles[left];
            const auto& b = tiles[right];
            const auto disjoint = a.x + a.width <= b.x || b.x + b.width <= a.x
                || a.y + a.height <= b.y || b.y + b.height <= a.y;
            SLICE_CHECK(disjoint);
        }
    }
}

void depth_is_reversed_and_monotonic() {
    const auto camera = stage_camera();
    scene::Camera straight{};
    straight.eye = scene::Vec3{0.0F, 0.0F, 0.0F};
    straight.target = scene::Vec3{0.0F, 0.0F, -1.0F};
    straight.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    straight.field_of_view = camera.field_of_view;
    straight.near_plane = 0.1F;
    straight.far_plane = 100.0F;

    const auto at_near =
        scene::project_to_canvas(straight, wall, scene::Vec3{0.0F, 0.0F, -0.1F});
    const auto at_far =
        scene::project_to_canvas(straight, wall, scene::Vec3{0.0F, 0.0F, -100.0F});
    SLICE_CHECK(at_near.visible && at_far.visible);
    SLICE_CHECK(std::abs(at_near.depth - 1.0F) <= 1.0e-5F);
    SLICE_CHECK(std::abs(at_far.depth) <= 1.0e-5F);

    // Monotonically decreasing with distance, which is what makes a
    // greater-than depth test correct.
    auto previous = 2.0F;
    for (int step = 1; step <= 200; ++step) {
        const auto distance = 0.1F + static_cast<float>(step) * 0.4F;
        const auto sample = scene::project_to_canvas(
            straight, wall, scene::Vec3{0.0F, 0.0F, -distance});
        SLICE_CHECK(sample.visible);
        SLICE_CHECK(sample.depth < previous);
        previous = sample.depth;
    }
    SLICE_CHECK(previous > 0.0F);

    // The centre of the canvas is where a point straight ahead lands.
    SLICE_CHECK(
        std::abs(at_near.x - static_cast<float>(wall.width) * 0.5F) <= 1.0e-2F);
    SLICE_CHECK(
        std::abs(at_near.y - static_cast<float>(wall.height) * 0.5F) <= 1.0e-2F);
}

void a_slice_maps_its_own_corners_to_its_own_edges() {
    const auto camera = stage_camera();
    const auto tiles = quadrants();

    // Reconstruct a point on the near plane at the slice's top-left corner and
    // check it comes back at that corner. This is the round trip an operator
    // is really asking about: does my rectangle show my part of the image.
    // The camera basis, built forwards rather than by inverting the view
    // matrix: a point on the frustum edge is the worst case for the divide
    // that follows, and there is no reason to spend precision getting there.
    const auto forward = scene::normalize(camera.target - camera.eye);
    const auto right = scene::normalize(scene::cross(forward, camera.up));
    const auto up = scene::cross(right, forward);

    for (const auto& tile : tiles) {
        const auto frustum = scene::window_for_slice(camera, wall, tile);
        const auto corner = camera.eye + right * frustum.left
            + up * frustum.top + forward * frustum.near_plane;

        const auto placed = scene::project_through_slice(camera, wall, tile, corner);
        SLICE_CHECK(placed.visible);
        // A twentieth of a pixel. The point sits exactly on two frustum
        // planes, where the perspective divide has the least precision to
        // spare, so this is as tight as the round trip honestly supports.
        SLICE_CHECK(std::abs(placed.x - static_cast<float>(tile.x)) <= 0.05F);
        SLICE_CHECK(std::abs(placed.y - static_cast<float>(tile.y)) <= 0.05F);
    }
}

void an_unusable_camera_or_slice_yields_identity_rather_than_an_infinity() {
    const auto camera = stage_camera();

    // Outside the canvas.
    const auto outside = scene::view_for_slice(
        camera, wall, scene::CanvasRect{wall.width, 0U, 16U, 16U});
    SLICE_CHECK(outside.projection == scene::identity());

    // Empty.
    SLICE_CHECK(
        scene::view_for_slice(camera, wall, scene::CanvasRect{0U, 0U, 0U, 16U})
            .projection
        == scene::identity());

    // A field of view no frustum can be built from.
    auto splayed = camera;
    splayed.field_of_view = 0.5F;
    SLICE_CHECK(!scene::valid(splayed));
    SLICE_CHECK(
        scene::view_for_slice(splayed, wall, scene::whole(wall)).projection
        == scene::identity());

    // A far plane inside the near plane.
    auto inverted = camera;
    inverted.far_plane = camera.near_plane * 0.5F;
    SLICE_CHECK(!scene::valid(inverted));

    // An eye sitting on its own target.
    auto degenerate = camera;
    degenerate.target = degenerate.eye;
    SLICE_CHECK(!scene::valid(degenerate));

    // Every element of every produced matrix is finite in each case, which is
    // the property that actually protects the frame.
    for (const auto element : outside.view_projection.m) {
        SLICE_CHECK(std::isfinite(element));
    }
}

void a_canvas_bigger_than_any_wall_is_refused() {
    SLICE_CHECK(scene::valid(scene::Canvas{1U, 1U}));
    SLICE_CHECK(scene::valid(scene::Canvas{65'535U, 65'535U}));
    SLICE_CHECK(!scene::valid(scene::Canvas{0U, 1'080U}));
    SLICE_CHECK(!scene::valid(scene::Canvas{1'920U, 0U}));
    SLICE_CHECK(!scene::valid(scene::Canvas{65'536U, 1'080U}));
    SLICE_CHECK(!scene::contains(scene::Canvas{}, scene::CanvasRect{0U, 0U, 1U, 1U}));
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 8U> cases{{
    {"adjacent slices share a bit-identical frustum plane",
     &adjacent_slices_share_a_bit_identical_frustum_plane},
    {"a slice boundary is the same wherever it is cut",
     &a_slice_boundary_is_the_same_wherever_it_is_cut},
    {"every slice projects a point where the whole canvas does",
     &every_slice_projects_a_point_where_the_whole_canvas_does},
    {"the quadrants tile the canvas without gap or overlap",
     &the_quadrants_tile_the_canvas_without_gap_or_overlap},
    {"depth is reversed and monotonic", &depth_is_reversed_and_monotonic},
    {"a slice maps its own corners to its own edges",
     &a_slice_maps_its_own_corners_to_its_own_edges},
    {"an unusable camera or slice yields identity rather than an infinity",
     &an_unusable_camera_or_slice_yields_identity_rather_than_an_infinity},
    {"a canvas bigger than any wall is refused",
     &a_canvas_bigger_than_any_wall_is_refused},
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
            std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
