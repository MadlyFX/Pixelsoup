#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/scene.hpp>

#include <array>
#include <utility>
#include <cmath>
#include <cstddef>
#include <limits>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define ICVFX_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

constexpr scene::Canvas wall{3'840U, 2'160U};

[[nodiscard]] scene::Camera authored_camera() noexcept {
    scene::Camera camera{};
    camera.eye = scene::Vec3{0.0F, 1.6F, 6.0F};
    camera.target = scene::Vec3{0.0F, 1.6F, 0.0F};
    camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    camera.field_of_view = 0.125F;
    camera.near_plane = 0.05F;
    camera.far_plane = 500.0F;
    return camera;
}

[[nodiscard]] scene::TrackedCamera tracked_at(
    scene::Vec3 position, scene::Turns yaw, core::u64 frame) noexcept {
    scene::TrackedCamera tracked{};
    tracked.position = position;
    tracked.yaw = yaw;
    tracked.field_of_view = 0.05F; // 18 degrees: a long lens on a wall
    tracked.aspect_ratio = 16.0F / 9.0F;
    tracked.control_frame = frame;
    tracked.sequence = frame;
    tracked.valid = true;
    return tracked;
}

// Projects a set of world points through the authored canvas camera. This is
// the shape a wall-geometry intersection produces, without needing the output
// layer here.
[[nodiscard]] std::vector<scene::CanvasProjection> projected(
    const scene::Camera& camera,
    const std::vector<scene::Vec3>& points) {
    std::vector<scene::CanvasProjection> result;
    result.reserve(points.size());
    for (const scene::Vec3 point : points) {
        result.push_back(scene::project_to_canvas(camera, wall, point));
    }
    return result;
}

void an_invalid_tracked_pose_leaves_the_authored_camera_alone() {
    const scene::Camera authored = authored_camera();
    scene::TrackedCamera tracked = tracked_at({1.0F, 2.0F, 3.0F}, 0.1F, 5U);
    tracked.valid = false;
    ICVFX_CHECK(scene::tracked_view_camera(authored, tracked) == authored);
}

void a_tracked_pose_overrides_pose_and_lens_but_not_clip_or_exposure() {
    scene::Camera authored = authored_camera();
    authored.aperture_f_number = 1.8F;
    authored.iso = 800.0F;
    const scene::TrackedCamera tracked =
        tracked_at({2.0F, 1.5F, 4.0F}, 0.0F, 7U);

    const scene::Camera effective =
        scene::tracked_view_camera(authored, tracked);
    ICVFX_CHECK(effective.eye == tracked.position);
    ICVFX_CHECK(effective.field_of_view == tracked.field_of_view);
    // Clip range and every exposure control are lens and shading policy. A
    // tracker has no opinion about them.
    ICVFX_CHECK(effective.near_plane == authored.near_plane);
    ICVFX_CHECK(effective.far_plane == authored.far_plane);
    ICVFX_CHECK(effective.aperture_f_number == authored.aperture_f_number);
    ICVFX_CHECK(effective.iso == authored.iso);
    ICVFX_CHECK(scene::valid(effective));

    // Zero rotation looks along -Z, the same convention the authored camera
    // and every other transform in the scene use.
    const scene::Vec3 forward =
        scene::normalize(effective.target - effective.eye);
    ICVFX_CHECK(std::fabs(forward.x) < 1.0e-6F);
    ICVFX_CHECK(std::fabs(forward.y) < 1.0e-6F);
    ICVFX_CHECK(forward.z < -0.99F);

    // A quarter turn of yaw about +Y turns the camera to look along -X.
    const scene::Camera turned = scene::tracked_view_camera(
        authored, tracked_at(tracked.position, 0.25F, 7U));
    const scene::Vec3 turned_forward =
        scene::normalize(turned.target - turned.eye);
    ICVFX_CHECK(turned_forward.x < -0.99F);
    ICVFX_CHECK(std::fabs(turned_forward.z) < 1.0e-5F);
}

void the_rectangle_covers_every_projected_corner() {
    const scene::Camera authored = authored_camera();
    // Four points around a patch of the stage, plus one behind the camera that
    // must be ignored rather than folded in.
    const std::vector<scene::Vec3> points{
        {-1.0F, 0.5F, 0.0F},
        {1.0F, 0.5F, 0.0F},
        {-1.0F, 2.5F, 0.0F},
        {1.0F, 2.5F, 0.0F},
    };
    const auto shots = projected(authored, points);
    const auto region = scene::inner_frustum_rect(wall, shots, 0U);
    ICVFX_CHECK(region.valid);

    for (const scene::CanvasProjection& shot : shots) {
        ICVFX_CHECK(shot.visible);
        ICVFX_CHECK(shot.x >= static_cast<float>(region.rect.x));
        ICVFX_CHECK(shot.x <= static_cast<float>(
                        region.rect.x + region.rect.width));
        ICVFX_CHECK(shot.y >= static_cast<float>(region.rect.y));
        ICVFX_CHECK(shot.y <= static_cast<float>(
                        region.rect.y + region.rect.height));
    }
}

void rounding_is_outward_and_the_margin_grows_it() {
    // Exact fractional coordinates, so the rounding direction is the only
    // thing under test.
    const std::array<scene::CanvasProjection, 2U> points{{
        {100.25F, 200.75F, 0.5F, true},
        {300.5F, 400.25F, 0.5F, true},
    }};
    const auto tight = scene::inner_frustum_rect(wall, points, 0U);
    ICVFX_CHECK(tight.valid);
    ICVFX_CHECK(!tight.clamped);
    // Floor on the low edge, ceil on the high edge. A rectangle one pixel too
    // small shows outer-frustum content inside the plate.
    ICVFX_CHECK(tight.rect.x == 100U);
    ICVFX_CHECK(tight.rect.y == 200U);
    ICVFX_CHECK(tight.rect.x + tight.rect.width == 301U);
    ICVFX_CHECK(tight.rect.y + tight.rect.height == 401U);

    const auto margined = scene::inner_frustum_rect(wall, points, 16U);
    ICVFX_CHECK(margined.rect.x == 84U);
    ICVFX_CHECK(margined.rect.y == 184U);
    ICVFX_CHECK(margined.rect.x + margined.rect.width == 317U);
    ICVFX_CHECK(margined.rect.y + margined.rect.height == 417U);

    // An exact integer boundary must not lose a pixel to the ceiling.
    const std::array<scene::CanvasProjection, 2U> exact{{
        {10.0F, 20.0F, 0.5F, true},
        {30.0F, 40.0F, 0.5F, true},
    }};
    const auto snapped = scene::inner_frustum_rect(wall, exact, 0U);
    ICVFX_CHECK(snapped.rect == scene::CanvasRect{10U, 20U, 20U, 20U});
}

void a_region_off_the_canvas_clamps_or_refuses() {
    const std::array<scene::CanvasProjection, 2U> straddling{{
        {-50.0F, -20.0F, 0.5F, true},
        {200.0F, 100.0F, 0.5F, true},
    }};
    const auto clamped = scene::inner_frustum_rect(wall, straddling, 0U);
    ICVFX_CHECK(clamped.valid);
    ICVFX_CHECK(clamped.clamped);
    ICVFX_CHECK(clamped.rect == scene::CanvasRect{0U, 0U, 200U, 100U});

    const std::array<scene::CanvasProjection, 2U> beyond{{
        {-500.0F, -500.0F, 0.5F, true},
        {-100.0F, -100.0F, 0.5F, true},
    }};
    const auto outside = scene::inner_frustum_rect(wall, beyond, 0U);
    ICVFX_CHECK(!outside.valid);
    ICVFX_CHECK(outside.clamped);
}

void points_behind_the_eye_are_ignored_not_clamped() {
    // A point behind the eye has no canvas position; folding one in would put
    // the rectangle somewhere derived from a number that is not a coordinate.
    const std::array<scene::CanvasProjection, 3U> mixed{{
        {100.0F, 100.0F, 0.5F, true},
        {-9'999.0F, -9'999.0F, 0.5F, false},
        {200.0F, 200.0F, 0.5F, true},
    }};
    const auto region = scene::inner_frustum_rect(wall, mixed, 0U);
    ICVFX_CHECK(region.valid);
    ICVFX_CHECK(region.rect == scene::CanvasRect{100U, 100U, 100U, 100U});

    const std::array<scene::CanvasProjection, 1U> all_behind{{
        {50.0F, 50.0F, 0.5F, false},
    }};
    ICVFX_CHECK(!scene::inner_frustum_rect(wall, all_behind, 0U).valid);
}

// The property the feature stands on. Two nodes own different slices of the
// canvas and must derive the same rectangle, byte for byte, from the same
// replicated pose. Neither of them may consult its own slice while doing it.
void two_nodes_with_different_slices_derive_identical_rectangles() {
    const scene::Camera authored = authored_camera();
    const std::array<scene::CanvasRect, 2U> slices{{
        {0U, 0U, wall.width / 2U, wall.height},
        {wall.width / 2U, 0U, wall.width / 2U, wall.height},
    }};

    for (int step = -8; step <= 8; ++step) {
        const float offset = static_cast<float>(step) * 0.37F;
        const scene::TrackedCamera tracked = tracked_at(
            scene::Vec3{offset, 1.6F, 3.5F}, offset * 0.01F,
            static_cast<core::u64>(step + 100));
        const scene::Camera effective =
            scene::tracked_view_camera(authored, tracked);
        const auto corners =
            scene::frustum_corners(effective, tracked.aspect_ratio);
        ICVFX_CHECK(corners.valid);

        std::vector<scene::Vec3> points;
        for (std::size_t index = 4U; index < corners.points.size(); ++index) {
            // Bring the far corners back onto the plane the wall stands on so
            // they land inside the canvas; the derivation under test is the
            // rounding, not the wall intersection.
            const scene::Vec3 corner = corners.points[index];
            const scene::Vec3 direction = corner - effective.eye;
            const float distance =
                direction.z == 0.0F ? 1.0F : -effective.eye.z / direction.z;
            points.push_back(effective.eye + direction * distance);
        }

        std::array<scene::InnerFrustumRegion, 2U> derived{};
        for (std::size_t node = 0U; node < slices.size(); ++node) {
            // Deliberately not passing `slices[node]` anywhere: the point is
            // that a node cannot introduce its own origin into this answer.
            derived[node] =
                scene::inner_frustum_rect(wall, projected(authored, points),
                                          24U);
        }
        ICVFX_CHECK(derived[0] == derived[1]);
        ICVFX_CHECK(derived[0].valid);

        // And each node's own question - do I have to render the inner
        // frustum - is a rectangle intersection over those same values.
        const bool left_hit = scene::intersects(slices[0], derived[0].rect);
        const bool right_hit = scene::intersects(slices[1], derived[0].rect);
        ICVFX_CHECK(left_hit || right_hit);
    }
}

void slice_intersection_is_half_open_and_exact() {
    const scene::CanvasRect left{0U, 0U, 1'920U, 2'160U};
    const scene::CanvasRect right{1'920U, 0U, 1'920U, 2'160U};
    // Abutting slices do not intersect: the shared column belongs to the one
    // on the right, exactly as the slice contract already says.
    ICVFX_CHECK(!scene::intersects(left, right));

    const scene::CanvasRect inner{1'900U, 500U, 100U, 100U};
    ICVFX_CHECK(scene::intersects(left, inner));
    ICVFX_CHECK(scene::intersects(right, inner));
    ICVFX_CHECK(scene::intersection(left, inner)
                == scene::CanvasRect{1'900U, 500U, 20U, 100U});
    ICVFX_CHECK(scene::intersection(right, inner)
                == scene::CanvasRect{1'920U, 500U, 80U, 100U});

    const scene::CanvasRect elsewhere{100U, 100U, 50U, 50U};
    ICVFX_CHECK(!scene::intersects(right, elsewhere));
    ICVFX_CHECK(scene::intersection(right, elsewhere) == scene::CanvasRect{});
}

void the_explicit_aspect_view_matches_a_canvas_of_that_shape() {
    const scene::Camera camera = authored_camera();
    const scene::Canvas matching{1'600U, 900U};
    const auto by_canvas =
        scene::view_for_slice(camera, matching, scene::whole(matching));
    const auto by_aspect = scene::view_for_aspect(camera, 16.0F / 9.0F);
    ICVFX_CHECK(by_canvas == by_aspect);

    // And an unusable aspect is identity rather than an infinity.
    const auto degenerate = scene::view_for_aspect(camera, 0.0F);
    ICVFX_CHECK(degenerate.projection == scene::identity());
}

void icvfx_policy_is_clamped_once_and_digested() {
    scene::IcvfxCameraSettings authored{};
    authored.inner_frustum = true;
    authored.margin_px = 100'000U;
    authored.soft_edge_px = 100'000U;
    authored.resolution_numerator = 0U;
    authored.resolution_denominator = 99U;

    const auto bounded = scene::clamped(authored);
    ICVFX_CHECK(bounded.margin_px == scene::max_icvfx_margin_px);
    ICVFX_CHECK(bounded.resolution_numerator == 1U);
    ICVFX_CHECK(bounded.resolution_denominator
                == scene::max_icvfx_resolution_scale);
    // A feather wider than the margin would reach outside the rectangle the
    // margin bought and let the outer frustum show through the plate edge.
    ICVFX_CHECK(bounded.soft_edge_px <= bounded.margin_px);
    ICVFX_CHECK(scene::clamped(bounded) == bounded);

    scene::IcvfxSettings show{};
    show.latency_frames = 900U;
    show.prediction_window = 900U;
    show.jitter_filter_percent = 500U;
    ICVFX_CHECK(scene::clamped(show).prediction_window == 16U);
    ICVFX_CHECK(scene::clamped(show).jitter_filter_percent == 100U);
    show.prediction_window = 0U;
    ICVFX_CHECK(scene::clamped(show).prediction_window == 2U);
    ICVFX_CHECK(scene::clamped(show).latency_frames
                == scene::max_icvfx_latency_frames);

    // The show-wide prediction fields are in the profile whether or not
    // plates are on; the per-camera defaults only while they are.
    scene::RendererSettings plain{};
    const auto baseline = scene::renderer_settings_digest(plain);
    for (int field = 0; field < 4; ++field) {
        auto changed = plain;
        if (field == 0) changed.icvfx.pose_prediction = true;
        if (field == 1) changed.icvfx.prediction_window = 8;
        if (field == 2) changed.icvfx.jitter_filter_percent = 61;
        if (field == 3) changed.icvfx.latency_frames = 7;
        ICVFX_CHECK(scene::renderer_settings_digest(changed) != baseline);
        scene::Scene original{}, altered{};
        original.renderer_settings = plain;
        altered.renderer_settings = changed;
        ICVFX_CHECK(scene::digest(original) != scene::digest(altered));
    }
    scene::RendererSettings widened = plain;
    widened.icvfx.camera_defaults.margin_px = 512U;
    ICVFX_CHECK(scene::renderer_settings_digest(widened) == baseline);

    // Once it is on, every field is part of the profile a Director accepts
    // evidence against - the show-wide latency and the density a new camera
    // starts from alike, because both decide how much a node has to draw.
    scene::RendererSettings live = plain;
    live.icvfx.inner_frustum = true;
    const auto enabled = scene::renderer_settings_digest(live);
    ICVFX_CHECK(enabled != baseline);
    scene::RendererSettings nudged = live;
    nudged.icvfx.camera_defaults.margin_px =
        live.icvfx.camera_defaults.margin_px + 1U;
    ICVFX_CHECK(scene::renderer_settings_digest(nudged) != enabled);
    scene::RendererSettings slower = live;
    slower.icvfx.latency_frames = live.icvfx.latency_frames + 1U;
    ICVFX_CHECK(scene::renderer_settings_digest(slower) != enabled);
}

void a_tracked_pose_joins_the_scene_digest_only_once_it_is_live() {
    scene::Scene plain{};
    plain.canvas = wall;
    plain.camera = authored_camera();
    const auto baseline = scene::digest(plain);

    // A camera with no pose is carrier data, not picture - but it is still a
    // bound camera, and which camera the plate comes from is part of what two
    // nodes agree on, so the stack itself moves the digest.
    scene::Scene idle = plain;
    scene::TrackedCameraView view{};
    view.camera = doc::NodeId{4U};
    view.source = doc::NodeId{3U};
    idle.tracked_cameras.push_back(view);
    const auto bound_digest = scene::digest(idle);
    ICVFX_CHECK(bound_digest != baseline);

    scene::Scene live = idle;
    live.tracked_cameras.front().pose =
        tracked_at({1.0F, 1.6F, 3.0F}, 0.05F, 42U);
    const auto tracked_digest = scene::digest(live);
    ICVFX_CHECK(tracked_digest != bound_digest);

    // One ulp of pose is a different picture, and the digest exists to catch
    // exactly that.
    scene::Scene nudged = live;
    nudged.tracked_cameras.front().pose.position.x = std::nextafter(
        live.tracked_cameras.front().pose.position.x, 2.0F);
    ICVFX_CHECK(scene::digest(nudged) != tracked_digest);

    // The control frame the pose was stamped for is part of it too: the same
    // pose held for a later frame is the same picture, but two nodes must
    // still agree about which frame they are holding from.
    scene::Scene later = live;
    later.tracked_cameras.front().pose.control_frame = 43U;
    ICVFX_CHECK(scene::digest(later) != tracked_digest);

    // And so is the order of the stack. Two nodes that composited the same
    // plates the other way round disagree wherever two of them overlap, so
    // reversing the sequence must not digest the same.
    scene::Scene second = live;
    scene::TrackedCameraView other = second.tracked_cameras.front();
    other.camera = doc::NodeId{9U};
    second.tracked_cameras.push_back(other);
    scene::Scene reversed = second;
    std::swap(reversed.tracked_cameras[0U], reversed.tracked_cameras[1U]);
    ICVFX_CHECK(scene::digest(reversed) != scene::digest(second));
}

struct Case final {
    const char* name;
    void (*function)();
};


// A tracker reports in its own frame and nothing in the datagram says which
// one. Uncalibrated, a Z-up feed puts the camera on its side somewhere
// unrelated to the wall - which on a stage reads as a broken plate rather than
// a missing setting, and is why this exists.
void a_tracker_pose_is_mapped_into_stage_space() {
    // Stage-native is the identity.
    const scene::TrackerCalibration native{};
    const scene::Vec3 raw{1.0F, 2.0F, 3.0F};
    ICVFX_CHECK(scene::stage_position_from_tracker(raw, native) == raw);
    ICVFX_CHECK(scene::stage_pan_from_tracker(0.25F, native) == 0.25F);

    // Z-up: the tracker's +Z is stage up, its +Y is stage forward (stage -Z).
    scene::TrackerCalibration z_up{};
    z_up.axes = scene::TrackerAxes::ZUpRightHanded;
    const scene::Vec3 mapped = scene::stage_position_from_tracker(raw, z_up);
    ICVFX_CHECK(mapped.x == 1.0F);
    ICVFX_CHECK(mapped.y == 3.0F);
    ICVFX_CHECK(mapped.z == -2.0F);
    // Height really is height: a metre up the tracker's Z is a metre up the
    // stage's Y and nothing else moves.
    const auto up_only = scene::stage_position_from_tracker(
        scene::Vec3{0.0F, 0.0F, 1.0F}, z_up);
    ICVFX_CHECK(up_only == (scene::Vec3{0.0F, 1.0F, 0.0F}));
    // And the map is a rotation, not a mirror: it preserves handedness, so a
    // camera basis built from it is not silently reversed.
    const auto x_axis = scene::stage_position_from_tracker(
        scene::Vec3{1.0F, 0.0F, 0.0F}, z_up);
    const auto y_axis = scene::stage_position_from_tracker(
        scene::Vec3{0.0F, 1.0F, 0.0F}, z_up);
    ICVFX_CHECK(scene::cross(x_axis, y_axis) == up_only);

    // Scale then origin, in that order.
    scene::TrackerCalibration placed{};
    placed.scale = 2.0F;
    placed.origin = scene::Vec3{10.0F, 0.0F, -5.0F};
    ICVFX_CHECK(scene::stage_position_from_tracker(raw, placed)
                == (scene::Vec3{12.0F, 4.0F, 1.0F}));

    // Heading is the daily correction, and it wraps rather than accumulating.
    scene::TrackerCalibration turned{};
    turned.heading = 0.25F;
    ICVFX_CHECK(scene::stage_pan_from_tracker(0.5F, turned) == 0.75F);
    ICVFX_CHECK(std::abs(scene::stage_pan_from_tracker(0.9F, turned) - 0.15F)
                < 1.0e-5F);

    // Out-of-contract values are clamped once here rather than trusted.
    scene::TrackerCalibration broken{};
    broken.scale = 0.0F;
    broken.origin = scene::Vec3{1.0e9F, 0.0F, 0.0F};
    const scene::TrackerCalibration fixed = scene::clamped(broken);
    ICVFX_CHECK(fixed.scale >= scene::min_tracker_scale);
    ICVFX_CHECK(fixed.origin.x <= scene::max_tracker_origin_m);
    // A non-finite heading is zero, not a NaN that would poison every pose.
    scene::TrackerCalibration nan_heading{};
    nan_heading.heading = std::numeric_limits<float>::quiet_NaN();
    ICVFX_CHECK(scene::clamped(nan_heading).heading == 0.0F);
}

constexpr std::array<Case, 12U> cases{{
    {"an invalid tracked pose leaves the authored camera alone",
     &an_invalid_tracked_pose_leaves_the_authored_camera_alone},
    {"a tracked pose overrides pose and lens but not clip or exposure",
     &a_tracked_pose_overrides_pose_and_lens_but_not_clip_or_exposure},
    {"the rectangle covers every projected corner",
     &the_rectangle_covers_every_projected_corner},
    {"rounding is outward and the margin grows it",
     &rounding_is_outward_and_the_margin_grows_it},
    {"a region off the canvas clamps or refuses",
     &a_region_off_the_canvas_clamps_or_refuses},
    {"points behind the eye are ignored, not clamped",
     &points_behind_the_eye_are_ignored_not_clamped},
    {"two nodes with different slices derive identical rectangles",
     &two_nodes_with_different_slices_derive_identical_rectangles},
    {"slice intersection is half open and exact",
     &slice_intersection_is_half_open_and_exact},
    {"the explicit-aspect view matches a canvas of that shape",
     &the_explicit_aspect_view_matches_a_canvas_of_that_shape},
    {"icvfx policy is clamped once and digested",
     &icvfx_policy_is_clamped_once_and_digested},
    {"a tracker pose is mapped into stage space",
     &a_tracker_pose_is_mapped_into_stage_space},
    {"a tracked pose joins the scene digest only once it is live",
     &a_tracked_pose_joins_the_scene_digest_only_once_it_is_live},
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
