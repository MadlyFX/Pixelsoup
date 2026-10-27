#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/scene.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <limits>
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

void physical_camera_controls_change_relative_exposure_by_stops() {
    scene::Camera camera{};
    SLICE_CHECK(scene::camera_exposure_multiplier(camera) == 1.0F);

    camera.shutter_seconds *= 2.0F;
    SLICE_CHECK(scene::camera_exposure_multiplier(camera) == 2.0F);
    camera.shutter_seconds *= 0.5F;

    camera.iso *= 2.0F;
    SLICE_CHECK(scene::camera_exposure_multiplier(camera) == 2.0F);
    camera.iso *= 0.5F;

    camera.exposure_compensation_ev = 1.0F;
    SLICE_CHECK(scene::camera_exposure_multiplier(camera) == 2.0F);
    camera.exposure_compensation_ev = -1.0F;
    SLICE_CHECK(scene::camera_exposure_multiplier(camera) == 0.5F);

    camera = scene::Camera{};
    camera.aperture_f_number *= 2.0F;
    SLICE_CHECK(scene::camera_exposure_multiplier(camera) == 0.25F);

    camera.aperture_f_number = 0.0F;
    SLICE_CHECK(scene::camera_exposure_multiplier(camera) == 1.0F);
}

[[nodiscard]] scene::LensProfile calibrated_lens() noexcept {
    scene::LensProfile lens{};
    lens.sensor_width_mm = 36.0F;
    lens.sensor_height_mm = 24.0F;
    lens.focal_length_mm = 50.0F;
    lens.entrance_pupil_offset_m = 0.125F;
    return lens;
}

void a_physical_lens_derives_its_window_from_sensor_and_focal_length() {
    auto camera = stage_camera();
    camera.near_plane = 0.1F;
    const scene::LensProfile lens = calibrated_lens();
    const auto window = scene::window_for_lens(camera, lens, {});

    // 0.1 * (36 / 2) / 50 and 0.1 * (24 / 2) / 50 respectively.
    SLICE_CHECK(std::abs(window.left + 0.036F) <= 1.0e-7F);
    SLICE_CHECK(std::abs(window.right - 0.036F) <= 1.0e-7F);
    SLICE_CHECK(std::abs(window.bottom + 0.024F) <= 1.0e-7F);
    SLICE_CHECK(std::abs(window.top - 0.024F) <= 1.0e-7F);

    scene::LensProfile table = lens;
    table.calibration_count = 4U;
    table.calibration[0U] = scene::LensCalibrationPoint{0.0F, 0.0F, 25.0F,
                                                         0.00F, {0.00F}};
    table.calibration[1U] = scene::LensCalibrationPoint{100.0F, 0.0F, 45.0F,
                                                         0.04F, {0.04F}};
    table.calibration[2U] = scene::LensCalibrationPoint{0.0F, 100.0F, 35.0F,
                                                         0.08F, {0.08F}};
    table.calibration[3U] = scene::LensCalibrationPoint{100.0F, 100.0F, 55.0F,
                                                         0.12F, {0.12F}};
    const scene::LensEncoders encoders{50.0F, 50.0F, 0.0F, true, true, false};
    const scene::EvaluatedLens evaluated = scene::evaluate_lens(table, encoders);
    SLICE_CHECK(evaluated.calibrated);
    SLICE_CHECK(std::abs(evaluated.focal_length_mm - 40.0F) <= 1.0e-6F);
    SLICE_CHECK(std::abs(evaluated.entrance_pupil_offset_m - 0.06F)
                <= 1.0e-6F);
    SLICE_CHECK(std::abs(evaluated.distortion.k1 - 0.06F) <= 1.0e-6F);
}

void lens_derived_adjacent_slices_still_share_their_exact_edge() {
    const auto camera = stage_camera();
    const scene::LensProfile lens = calibrated_lens();
    const auto tiles = quadrants();
    const scene::LensEncoders encoders{2'048.0F, 4'096.0F, 0.0F, true, true,
                                       false};
    const auto left = scene::window_for_slice(camera, wall, tiles[0U], lens,
                                               encoders);
    const auto right = scene::window_for_slice(camera, wall, tiles[1U], lens,
                                                encoders);
    const auto lower = scene::window_for_slice(camera, wall, tiles[2U], lens,
                                                encoders);
    SLICE_CHECK(left.right == right.left);
    SLICE_CHECK(left.bottom == lower.top);
}

void entrance_pupil_moves_only_along_the_tracked_optical_axis() {
    scene::Camera authored = stage_camera();
    authored.lens_profile = calibrated_lens();
    scene::TrackedCamera tracked{};
    tracked.position = scene::Vec3{1.25F, -0.5F, 4.0F};
    tracked.field_of_view = 0.1F;
    tracked.aspect_ratio = 16.0F / 9.0F;
    tracked.valid = true;

    const scene::Camera effective =
        scene::tracked_view_camera(authored, tracked, {});
    SLICE_CHECK(effective.eye.x == tracked.position.x);
    SLICE_CHECK(effective.eye.y == tracked.position.y);
    SLICE_CHECK(std::abs(effective.eye.z - (tracked.position.z - 0.125F))
                <= 1.0e-6F);
    SLICE_CHECK(std::abs(scene::length(effective.target - effective.eye) - 1.0F)
                <= 1.0e-6F);
}

void distortion_expands_coverage_without_changing_the_rendered_projection() {
    auto camera = stage_camera();
    camera.near_plane = 0.1F;
    scene::LensProfile lens = calibrated_lens();
    lens.distortion.k1 = 0.2F;
    const scene::LensCoordinate edge =
        scene::distort_brown_conrady(scene::LensCoordinate{1.0F, 0.0F},
                                     lens.distortion);
    const scene::DistortedLensExtent extent =
        scene::distorted_lens_extent(lens.distortion);
    SLICE_CHECK(extent.valid);
    SLICE_CHECK(edge.x <= extent.right);
    SLICE_CHECK(edge.x >= extent.left);

    const auto render_window = scene::window_for_lens(camera, lens, {});
    const auto coverage_window =
        scene::distorted_window_for_lens(camera, lens, {});
    SLICE_CHECK(coverage_window.left <= render_window.left);
    SLICE_CHECK(coverage_window.right >= render_window.right);
    SLICE_CHECK(coverage_window.bottom <= render_window.bottom);
    SLICE_CHECK(coverage_window.top >= render_window.top);
}

void no_lens_profile_preserves_the_legacy_tracked_camera_path_exactly() {
    const scene::Camera authored = stage_camera();
    scene::TrackedCamera tracked{};
    tracked.position = scene::Vec3{2.0F, 1.0F, 3.0F};
    tracked.yaw = 0.125F;
    tracked.pitch = -0.03125F;
    tracked.roll = 0.015625F;
    tracked.field_of_view = 0.08F;
    tracked.aspect_ratio = 1.5F;
    tracked.valid = true;

    const scene::Camera legacy = scene::tracked_view_camera(authored, tracked);
    const scene::Camera lens_aware =
        scene::tracked_view_camera(authored, tracked, scene::LensEncoders{});
    SLICE_CHECK(legacy == lens_aware);
    SLICE_CHECK(scene::view_for_slice(legacy, wall, scene::whole(wall))
                == scene::view_for_slice(lens_aware, wall, scene::whole(wall)));
}

// A plan view of a 1200 x 900 canvas, ten metres tall, looking down -Z from
// ten metres back. Distinct from the default camera in every field it uses.
[[nodiscard]] scene::Camera plan_camera() noexcept {
    scene::Camera camera{};
    camera.eye = scene::Vec3{1.0F, 2.0F, 10.0F};
    camera.target = scene::Vec3{1.0F, 2.0F, 0.0F};
    camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    camera.near_plane = 0.5F;
    camera.far_plane = 40.5F;
    camera.orthographic_height = 10.0F;
    return camera;
}

constexpr scene::Canvas plan_canvas{1'200U, 900U};

void an_orthographic_camera_maps_world_extent_without_perspective() {
    const scene::Camera camera = plan_camera();
    SLICE_CHECK(scene::valid(camera) && scene::orthographic(camera));
    // Ten metres over 900 pixels, so 13.333 m across 1200: the top edge is
    // five metres above the target and the right edge 6.667 m beside it.
    const auto top_right = scene::project_to_canvas(
        camera, plan_canvas, scene::Vec3{1.0F + 20.0F / 3.0F, 7.0F, 0.0F});
    SLICE_CHECK(std::abs(top_right.x - 1200.0F) <= 1.0e-2F);
    SLICE_CHECK(std::abs(top_right.y) <= 1.0e-2F);
    // No foreshortening: the same offset lands on the same pixel at any depth.
    for (const float z : {9.0F, 0.0F, -12.0F, -30.0F}) {
        const auto sample = scene::project_to_canvas(
            camera, plan_canvas, scene::Vec3{3.5F, 0.5F, z});
        SLICE_CHECK(sample.visible);
        SLICE_CHECK(std::abs(sample.x - (600.0F + 2.5F * 90.0F)) <= 1.0e-2F);
        SLICE_CHECK(std::abs(sample.y - (450.0F + 1.5F * 90.0F)) <= 1.0e-2F);
    }
    // Reversed and linear: near is one, far is zero, and halfway is a half.
    const auto depth_at = [&](float distance) {
        return scene::project_to_canvas(camera, plan_canvas,
                                        scene::Vec3{1.0F, 2.0F,
                                                    10.0F - distance})
            .depth;
    };
    SLICE_CHECK(std::abs(depth_at(0.5F) - 1.0F) <= 1.0e-6F);
    SLICE_CHECK(std::abs(depth_at(40.5F)) <= 1.0e-6F);
    SLICE_CHECK(std::abs(depth_at(20.5F) - 0.5F) <= 1.0e-6F);
    // The perspective camera at the same pose foreshortens the far point.
    scene::Camera perspective = camera;
    perspective.orthographic_height = 0.0F;
    const auto far_point = scene::project_to_canvas(
        perspective, plan_canvas, scene::Vec3{3.5F, 0.5F, -30.0F});
    SLICE_CHECK(far_point.x < 600.0F + 2.5F * 90.0F - 10.0F);
    // Both matrices read as what they are.
    SLICE_CHECK(scene::orthographic(
        scene::view_for_slice(camera, plan_canvas, scene::whole(plan_canvas))
            .projection));
    SLICE_CHECK(!scene::orthographic(
        scene::view_for_slice(perspective, plan_canvas,
                              scene::whole(plan_canvas))
            .projection));
}

void orthographic_slices_tile_and_share_their_edges() {
    const scene::Camera camera = plan_camera();
    const scene::CanvasRect left{0U, 0U, 700U, 900U};
    const scene::CanvasRect right{700U, 0U, 500U, 900U};
    const auto a = scene::window_for_slice(camera, plan_canvas, left);
    const auto b = scene::window_for_slice(camera, plan_canvas, right);
    SLICE_CHECK(a.right == b.left);
    // Every slice puts a point where the whole canvas does.
    for (const scene::Vec3 point :
         {scene::Vec3{-4.0F, 5.5F, -3.0F}, scene::Vec3{2.2F, -1.0F, 4.0F},
          scene::Vec3{6.0F, 0.0F, -20.0F}}) {
        const auto whole =
            scene::project_to_canvas(camera, plan_canvas, point);
        const scene::CanvasRect owner = whole.x < 700.0F ? left : right;
        const auto sliced =
            scene::project_through_slice(camera, plan_canvas, owner, point);
        SLICE_CHECK(std::abs(sliced.x - whole.x) <= 1.0e-3F);
        SLICE_CHECK(std::abs(sliced.y - whole.y) <= 1.0e-3F);
        SLICE_CHECK(std::abs(sliced.depth - whole.depth) <= 1.0e-6F);
    }
}

void an_orthographic_jitter_slides_by_the_same_subpixel_either_way() {
    const scene::Camera camera = plan_camera();
    const scene::SubpixelJitter jitter{0.25F, -0.375F};
    const scene::ViewMatrices from_window = scene::view_for_slice(
        camera, plan_canvas, scene::whole(plan_canvas), jitter);
    const scene::ViewMatrices from_matrix = scene::jittered(
        scene::view_for_slice(camera, plan_canvas,
                              scene::whole(plan_canvas)),
        jitter, plan_canvas.width, plan_canvas.height);
    for (core::u32 index = 0U; index < 16U; ++index) {
        SLICE_CHECK(std::abs(from_matrix.projection.m[index] -
                             from_window.projection.m[index]) <= 1.0e-6F);
    }
    const scene::Vec3 probe{2.0F, 3.0F, -5.0F};
    const auto plain = scene::project_to_canvas(camera, plan_canvas, probe);
    const auto shifted =
        scene::project_to_canvas(from_matrix, plan_canvas, probe);
    // The same directions the perspective form has: x against the window,
    // y with it.
    SLICE_CHECK(std::abs((shifted.x - plain.x) + jitter.x) <= 1.0e-3F);
    SLICE_CHECK(std::abs((shifted.y - plain.y) - jitter.y) <= 1.0e-3F);
}

void an_orthographic_camera_is_checked_and_digested() {
    scene::Camera camera = plan_camera();
    SLICE_CHECK(std::abs(scene::units_per_canvas_pixel(camera, 1.0F, 900.0F) -
                         10.0F / 900.0F) <= 1.0e-7F);
    SLICE_CHECK(scene::units_per_canvas_pixel(camera, 1.0F, 900.0F) ==
                scene::units_per_canvas_pixel(camera, 30.0F, 900.0F));
    scene::Camera perspective = camera;
    perspective.orthographic_height = 0.0F;
    perspective.field_of_view = 0.125F;
    SLICE_CHECK(std::abs(scene::units_per_canvas_pixel(perspective, 30.0F,
                                                       900.0F) -
                         30.0F * scene::units_per_canvas_pixel(
                                     perspective, 1.0F, 900.0F)) <= 1.0e-5F);

    // An orthographic box keeps one cross-section from near to far.
    const auto corners = scene::frustum_corners(camera, 4.0F / 3.0F);
    SLICE_CHECK(corners.valid);
    SLICE_CHECK(std::abs(corners.points[0].x - corners.points[4].x) <=
                1.0e-5F);
    SLICE_CHECK(std::abs(corners.points[3].y - corners.points[7].y) <=
                1.0e-5F);
    SLICE_CHECK(std::abs(corners.points[3].y - 7.0F) <= 1.0e-5F);

    // A negative or non-finite height, or a calibrated lens, is unusable.
    scene::Camera negative = camera;
    negative.orthographic_height = -1.0F;
    SLICE_CHECK(!scene::valid(negative));
    scene::Camera infinite = camera;
    infinite.orthographic_height = std::numeric_limits<float>::infinity();
    SLICE_CHECK(!scene::valid(infinite));
    scene::Camera lensed = camera;
    lensed.lens_profile = scene::LensProfile{};
    SLICE_CHECK(scene::valid(*lensed.lens_profile));
    SLICE_CHECK(!scene::valid(lensed));

    // In the scene digest when set; absent, the digest is the historical one.
    scene::Scene base{};
    base.camera = perspective;
    scene::Scene ortho = base;
    ortho.camera.orthographic_height = 10.0F;
    scene::Scene taller = ortho;
    taller.camera.orthographic_height = 12.5F;
    SLICE_CHECK(scene::digest(base) != scene::digest(ortho));
    SLICE_CHECK(scene::digest(ortho) != scene::digest(taller));
    scene::Scene again = base;
    SLICE_CHECK(scene::digest(base) == scene::digest(again));
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 18U> cases{{
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
    {"physical camera controls change relative exposure by stops",
     &physical_camera_controls_change_relative_exposure_by_stops},
    {"a physical lens derives its window from sensor and focal length",
     &a_physical_lens_derives_its_window_from_sensor_and_focal_length},
    {"lens-derived adjacent slices still share their exact edge",
     &lens_derived_adjacent_slices_still_share_their_exact_edge},
    {"entrance pupil moves only along the tracked optical axis",
     &entrance_pupil_moves_only_along_the_tracked_optical_axis},
    {"distortion expands coverage without changing the rendered projection",
     &distortion_expands_coverage_without_changing_the_rendered_projection},
    {"no lens profile preserves the legacy tracked camera path exactly",
     &no_lens_profile_preserves_the_legacy_tracked_camera_path_exactly},
    {"an orthographic camera maps world extent without perspective",
     &an_orthographic_camera_maps_world_extent_without_perspective},
    {"orthographic slices tile and share their edges",
     &orthographic_slices_tile_and_share_their_edges},
    {"an orthographic jitter slides by the same subpixel either way",
     &an_orthographic_jitter_slides_by_the_same_subpixel_either_way},
    {"an orthographic camera is checked and digested",
     &an_orthographic_camera_is_checked_and_digested},
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
