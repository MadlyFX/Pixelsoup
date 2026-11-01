#include <mediaserver/editor/gizmo.hpp>

#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

// The transform gizmo's geometry, with no panel, no window and no document:
// where a pivot and its three axes land in panel pixels, which handle a
// pointer is on, and which half of a rotate ring faces the camera.
//
// This is the half of the gizmo that a second host - ajfx Studio, placing
// props - leans on without ever building a scene graph, so it is checked
// against hand-computed projections rather than through a Stage frame.
namespace {

namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define GIZMO_CHECK(...)                                                       \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] bool near(float a, float b, float tolerance = 1.0e-3F) noexcept {
    return std::fabs(a - b) < tolerance;
}

// A camera eight units back along +z looking at the origin, on a 1920x1080
// canvas. The panel shows that canvas at exactly half size, so a canvas pixel
// is half a panel pixel and every expectation below can be worked out by hand.
[[nodiscard]] scene::Scene test_view() {
    scene::Scene view{};
    view.canvas = scene::Canvas{1920U, 1080U};
    view.camera.eye = scene::Vec3{0.0F, 0.0F, 8.0F};
    view.camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    view.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    return view;
}

constexpr gui::Rect test_image{100.0F, 50.0F, 960.0F, 540.0F};

// Where a world point is drawn in the panel, computed the long way round: the
// scene's own projection, then the image mapping. Any disagreement with the
// gizmo means it is projecting its axes through something else.
[[nodiscard]] gui::UiPoint expected_point(const scene::Scene& view,
                                          scene::Vec3 world) {
    const auto projected =
        scene::project_to_canvas(view.camera, view.canvas, world);
    GIZMO_CHECK(projected.visible);
    return gui::UiPoint{
        test_image.x + projected.x * test_image.width /
                           static_cast<float>(view.canvas.width),
        test_image.y + projected.y * test_image.height /
                           static_cast<float>(view.canvas.height)};
}

void a_gizmos_origin_and_axis_ends_project_where_the_camera_puts_them() {
    const scene::Scene view = test_view();
    // A pivot off the view axis, so an axis end that was quietly dropped or
    // measured from the wrong origin cannot pass by symmetry.
    const scene::Vec3 pivot{1.5F, -0.5F, 0.0F};
    const editor::GizmoGeometry gizmo =
        editor::gizmo_geometry(test_image, view, pivot);
    GIZMO_CHECK(gizmo.visible);
    GIZMO_CHECK(!gizmo.rings);
    GIZMO_CHECK(gizmo.world_origin == pivot);

    const gui::UiPoint origin = expected_point(view, pivot);
    GIZMO_CHECK(near(gizmo.origin.x, origin.x));
    GIZMO_CHECK(near(gizmo.origin.y, origin.y));

    // The default frame is the world basis, in X, Y, Z order.
    GIZMO_CHECK(gizmo.world_axes[0U] == scene::Vec3{1.0F, 0.0F, 0.0F});
    GIZMO_CHECK(gizmo.world_axes[1U] == scene::Vec3{0.0F, 1.0F, 0.0F});
    GIZMO_CHECK(gizmo.world_axes[2U] == scene::Vec3{0.0F, 0.0F, 1.0F});

    // An arrow is as long in the world as 72 panel pixels are at the pivot's
    // depth, which is what keeps the gizmo one size however far away it is.
    const float depth = scene::dot(
        pivot - view.camera.eye,
        scene::normalize(view.camera.target - view.camera.eye));
    GIZMO_CHECK(near(depth, 8.0F));
    const float expected_length =
        scene::units_per_canvas_pixel(view.camera, depth, 1080.0F) *
        editor::gizmo_length_pixels * 1080.0F / test_image.height;
    GIZMO_CHECK(near(gizmo.world_length, expected_length, 1.0e-4F));

    for (std::size_t index = 0U; index < 3U; ++index) {
        const gui::UiPoint end = expected_point(
            view, pivot + gizmo.world_axes[index] * gizmo.world_length);
        GIZMO_CHECK(near(gizmo.ends[index].x, end.x));
        GIZMO_CHECK(near(gizmo.ends[index].y, end.y));
    }
    // X runs right and Y runs up the panel, which is down in canvas pixels.
    GIZMO_CHECK(gizmo.ends[0U].x > gizmo.origin.x);
    GIZMO_CHECK(gizmo.ends[1U].y < gizmo.origin.y);

    // A local frame points along the element's own turned axes instead. A
    // quarter turn about world up swaps which of the element's axes lies
    // across the screen and which runs into it, so the arrow that shrinks to
    // nothing is the first rather than the third.
    const std::array<scene::Vec3, 3U> turned =
        editor::world_axes_of(scene::rotation(0.25F, 0.0F, 0.0F));
    const editor::GizmoGeometry local = editor::gizmo_geometry(
        test_image, view, scene::Vec3{0.0F, 0.0F, 0.0F}, turned);
    GIZMO_CHECK(local.visible);
    GIZMO_CHECK(near(std::fabs(local.world_axes[0U].z), 1.0F));
    GIZMO_CHECK(near(std::fabs(local.world_axes[2U].x), 1.0F));
    GIZMO_CHECK(near(local.ends[0U].x, local.origin.x, 1.0F));
    GIZMO_CHECK(near(local.ends[0U].y, local.origin.y, 1.0F));
    GIZMO_CHECK(std::fabs(local.ends[2U].x - local.origin.x) > 60.0F);
}

void an_axis_seen_nearly_end_on_is_not_a_handle() {
    const scene::Scene view = test_view();
    const scene::Vec3 pivot{0.0F, 0.0F, 0.0F};
    const editor::GizmoGeometry gizmo =
        editor::gizmo_geometry(test_image, view, pivot);
    GIZMO_CHECK(gizmo.visible);

    // Z points straight at the camera here: it is drawn, but a click on the
    // pivot must not be read as a drag along a direction the operator cannot
    // see. The short-axis guard drops it before any distance is measured.
    const float z_length = std::hypot(gizmo.ends[2U].x - gizmo.origin.x,
                                      gizmo.ends[2U].y - gizmo.origin.y);
    GIZMO_CHECK(z_length < 18.0F);
    GIZMO_CHECK(editor::hit_gizmo(gizmo, gizmo.ends[2U].x,
                                  gizmo.ends[2U].y) == editor::GizmoAxis::None);
    // Right on the pivot, where all three handles meet, nothing is picked
    // either: X and Y are more than their hit radius away along their length,
    // and Z is gone.
    GIZMO_CHECK(editor::hit_gizmo(gizmo, gizmo.origin.x, gizmo.origin.y) ==
                editor::GizmoAxis::None);
    // The two axes that do project are still handles, each along its own arm.
    GIZMO_CHECK(editor::hit_gizmo(gizmo,
                                  (gizmo.origin.x + gizmo.ends[0U].x) * 0.5F,
                                  (gizmo.origin.y + gizmo.ends[0U].y) * 0.5F) ==
                editor::GizmoAxis::X);
    GIZMO_CHECK(editor::hit_gizmo(gizmo,
                                  (gizmo.origin.x + gizmo.ends[1U].x) * 0.5F,
                                  (gizmo.origin.y + gizmo.ends[1U].y) * 0.5F) ==
                editor::GizmoAxis::Y);
}

void two_axes_that_project_close_together_pick_the_nearer_one() {
    // A frame whose first two axes are 10 degrees apart on screen: the kind of
    // near-edge-on view where picking the first match rather than the nearest
    // would hand a drag to the wrong axis.
    const scene::Scene view = test_view();
    const float tilt = 10.0F / 360.0F;
    const std::array<scene::Vec3, 3U> axes{
        scene::Vec3{1.0F, 0.0F, 0.0F},
        scene::Vec3{scene::deterministic::cos_turns(tilt),
                    scene::deterministic::sin_turns(tilt), 0.0F},
        scene::Vec3{0.0F, 0.0F, 1.0F}};
    const editor::GizmoGeometry gizmo =
        editor::gizmo_geometry(test_image, view, scene::Vec3{0.0F, 0.0F, 0.0F},
                               axes);
    GIZMO_CHECK(gizmo.visible);

    // Both arms are long enough to be handles, and they are close enough that
    // a point on one is within the hit radius of the other.
    const gui::UiPoint on_x{(gizmo.origin.x + gizmo.ends[0U].x) * 0.5F,
                            (gizmo.origin.y + gizmo.ends[0U].y) * 0.5F};
    const gui::UiPoint on_y{(gizmo.origin.x + gizmo.ends[1U].x) * 0.5F,
                            (gizmo.origin.y + gizmo.ends[1U].y) * 0.5F};
    GIZMO_CHECK(editor::distance_to_segment(on_x.x, on_x.y, gizmo.origin,
                                            gizmo.ends[1U]) <
                editor::gizmo_hit_pixels);
    GIZMO_CHECK(editor::distance_to_segment(on_y.x, on_y.y, gizmo.origin,
                                            gizmo.ends[0U]) <
                editor::gizmo_hit_pixels);

    // Each point still picks the arm it is actually on - Y wins over the X it
    // overlaps, which only a nearest-wins search gets right.
    GIZMO_CHECK(editor::hit_gizmo(gizmo, on_x.x, on_x.y) ==
                editor::GizmoAxis::X);
    GIZMO_CHECK(editor::hit_gizmo(gizmo, on_y.x, on_y.y) ==
                editor::GizmoAxis::Y);
}

void a_rotate_rings_samples_are_tagged_front_or_back() {
    const scene::Scene view = test_view();
    const scene::Vec3 pivot{0.0F, 0.0F, 0.0F};
    const editor::GizmoGeometry gizmo = editor::gizmo_geometry(
        test_image, view, pivot, editor::world_basis, true);
    GIZMO_CHECK(gizmo.visible);
    GIZMO_CHECK(gizmo.rings);

    const scene::Vec3 forward =
        scene::normalize(view.camera.target - view.camera.eye);
    const float radius = gizmo.world_length * 0.92F;
    for (std::size_t index = 0U; index < 3U; ++index) {
        const scene::Vec3 u = gizmo.world_axes[(index + 1U) % 3U];
        const scene::Vec3 v = gizmo.world_axes[(index + 2U) % 3U];
        std::size_t front = 0U;
        for (std::size_t step = 0U; step <= editor::gizmo_ring_segments;
             ++step) {
            const float turns =
                static_cast<float>(step) /
                static_cast<float>(editor::gizmo_ring_segments);
            const scene::Vec3 offset =
                (u * scene::deterministic::cos_turns(turns) +
                 v * scene::deterministic::sin_turns(turns)) *
                radius;
            // A sample faces the camera when it sits no further along the view
            // direction than the ring's own plane, with a small bias so the
            // two samples on the silhouette are not thrown away.
            const bool expected = scene::dot(offset, forward) <= radius * 0.02F;
            GIZMO_CHECK(gizmo.ring_front[index][step] == expected);
            // Said in this camera's terms, with no dot product: it looks
            // along -z, so a front sample is one the bias does not push
            // behind the pivot's own depth.
            const scene::Vec3 point = pivot + offset;
            GIZMO_CHECK(gizmo.ring_front[index][step] ==
                        (point.z >= -radius * 0.02F));
            if (gizmo.ring_front[index][step]) {
                ++front;
            }
        }
        // Each ring is about half a handle. The X and Y rings stand edge-on to
        // this camera, so both of their halves are tagged by the bias; the Z
        // ring lies in the screen plane and is a handle all the way round.
        GIZMO_CHECK(front >= editor::gizmo_ring_segments / 2U);
    }
    // The Z ring faces the camera entirely, so it is pickable anywhere on its
    // circumference, while the view ring sits further out and is not.
    const gui::UiPoint on_z_ring = gizmo.ring_points[2U][0U];
    GIZMO_CHECK(editor::hit_rotate_gizmo(gizmo, on_z_ring.x, on_z_ring.y) !=
                editor::GizmoAxis::None);
    GIZMO_CHECK(editor::hit_view_ring(
        gizmo, gizmo.origin.x + editor::view_ring_radius_pixels,
        gizmo.origin.y));
    GIZMO_CHECK(!editor::hit_view_ring(gizmo, gizmo.origin.x, gizmo.origin.y));
}

void a_pivot_behind_the_camera_has_no_gizmo() {
    const scene::Scene view = test_view();
    const editor::GizmoGeometry behind = editor::gizmo_geometry(
        test_image, view, scene::Vec3{0.0F, 0.0F, 20.0F});
    GIZMO_CHECK(!behind.visible);
    GIZMO_CHECK(editor::hit_gizmo(behind, 0.0F, 0.0F) ==
                editor::GizmoAxis::None);
    GIZMO_CHECK(editor::hit_rotate_gizmo(behind, 0.0F, 0.0F) ==
                editor::GizmoAxis::None);
    GIZMO_CHECK(editor::hit_scale_gizmo(behind, 0.0F, 0.0F) ==
                editor::GizmoAxis::None);
    GIZMO_CHECK(!editor::hit_view_ring(behind, 0.0F, 0.0F));
}

}  // namespace

int main() {
    struct Case final {
        const char* name;
        void (*run)();
    };
    const std::array cases{
        Case{"a_gizmos_origin_and_axis_ends_project_where_the_camera_puts_them",
             a_gizmos_origin_and_axis_ends_project_where_the_camera_puts_them},
        Case{"an_axis_seen_nearly_end_on_is_not_a_handle",
             an_axis_seen_nearly_end_on_is_not_a_handle},
        Case{"two_axes_that_project_close_together_pick_the_nearer_one",
             two_axes_that_project_close_together_pick_the_nearer_one},
        Case{"a_rotate_rings_samples_are_tagged_front_or_back",
             a_rotate_rings_samples_are_tagged_front_or_back},
        Case{"a_pivot_behind_the_camera_has_no_gizmo",
             a_pivot_behind_the_camera_has_no_gizmo},
    };
    int failures = 0;
    for (const Case& test : cases) {
        try {
            test.run();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
