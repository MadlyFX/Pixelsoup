#include <mediaserver/render/icvfx.hpp>

#include <mediaserver/render/icvfx_stage.hpp>
#include <mediaserver/render/mapped_scene.hpp>

#include <mediaserver/output/perspective_mapping.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/scene.hpp>

#include <algorithm>
#include <chrono>
#include <thread>
#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Inner/outer frustum, from the renderer's side.
//
// The headless half of this suite proves the derivation: two nodes with
// different slices agree on the rectangle and on the feather weight, and a node
// whose slice misses the rectangle skips the second render entirely.
//
// The GPU half proves the composite contributes no seam of its own. It feeds
// byte-identical outer and inner content to a full-canvas composite and to two
// node composites, and requires the shared pixels to be equal *exactly*, not
// closely. That is the test the feature stands on: any drift in how a node
// converts its own pixel to a canvas coordinate lands as a visible step in the
// feather, halfway across a plate, during a take.
namespace {

namespace core = mediaserver::core;
namespace output = mediaserver::output;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;
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

constexpr scene::Canvas wall{128U, 96U};
constexpr scene::CanvasRect left_slice{0U, 0U, 64U, 96U};
constexpr scene::CanvasRect right_slice{64U, 0U, 64U, 96U};

const mediaserver::doc::NodeId tracked_node{11U};

[[nodiscard]] scene::Scene tracked_scene(
    bool enabled = true, core::u32 soft_edge = 8U) {
    scene::Scene value{};
    value.canvas = wall;
    value.camera.eye = scene::Vec3{0.0F, 0.0F, 6.0F};
    value.camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    value.camera_source = mediaserver::doc::NodeId{7U};
    value.renderer_settings.icvfx.inner_frustum = enabled;

    scene::TrackedCameraView view{};
    view.camera = tracked_node;
    view.source = mediaserver::doc::NodeId{3U};
    view.authored = value.camera;
    view.icvfx.inner_frustum = true;
    view.icvfx.margin_px = 16U;
    view.icvfx.soft_edge_px = soft_edge;
    view.pose.position = scene::Vec3{0.2F, 0.1F, 3.0F};
    view.pose.field_of_view = 0.05F;
    view.pose.aspect_ratio = 16.0F / 9.0F;
    view.pose.control_frame = 900U;
    view.pose.valid = true;
    value.tracked_cameras.push_back(view);
    return value;
}

// The one tracked camera these tests plan for.
[[nodiscard]] const scene::TrackedCameraView& only(
    const scene::Scene& value) noexcept {
    return value.tracked_cameras.front();
}

[[nodiscard]] scene::InnerFrustumRegion region(
    scene::CanvasRect rect, bool clamped = false) noexcept {
    return scene::InnerFrustumRegion{rect, true, clamped};
}

// The wall every plan here is derived against: a flat LED face at the origin
// whose raster is `wall`, eight by six world units, front face toward +Z where
// both the authored and the tracked eye sit. cross(right, up) is +Z, so an eye
// at positive Z is in front of it.
//
// The plate's view is built through this, not through the scene canvas, so the
// surface is as much a part of the fixture as the scene is.
const output::PhysicalSurface surface{
    scene::Vec3{0.0F, 0.0F, 0.0F}, scene::Vec3{1.0F, 0.0F, 0.0F},
    scene::Vec3{0.0F, 1.0F, 0.0F}, 8.0F, 6.0F, wall};

void an_unauthored_or_untracked_scene_plans_nothing() {
    const auto region_value = region(scene::CanvasRect{40U, 20U, 48U, 40U});

    auto unauthored = tracked_scene(false);
    ICVFX_CHECK(
        !render::plan_inner_frustum(
        unauthored, only(unauthored), surface, left_slice, region_value)
             .active);

    auto untracked = tracked_scene();
    untracked.tracked_cameras.front().pose.valid = false;
    ICVFX_CHECK(
        !render::plan_inner_frustum(
        untracked, only(untracked), surface, left_slice, region_value)
             .active);

    // A node with no region - the camera sees no part of this canvas - renders
    // the outer frustum alone rather than guessing at a rectangle.
    const auto tracked = tracked_scene();
    ICVFX_CHECK(!render::plan_inner_frustum(
        tracked, only(tracked), surface, left_slice, scene::InnerFrustumRegion{})
                     .active);
}

void a_node_whose_slice_misses_the_rectangle_skips_the_inner_render() {
    const auto value = tracked_scene();
    // Entirely inside the right node's half.
    const auto right_only = region(scene::CanvasRect{80U, 20U, 32U, 40U});

    const auto left = render::plan_inner_frustum(
        value, only(value), surface, left_slice, right_only);
    ICVFX_CHECK(left.active);
    ICVFX_CHECK(left.skipped);
    ICVFX_CHECK(left.covered == scene::CanvasRect{});
    // No target is allocated for a render that will not happen.
    ICVFX_CHECK(left.inner_width == 0U && left.inner_height == 0U);

    const auto right =
        render::plan_inner_frustum(
        value, only(value), surface, right_slice, right_only);
    ICVFX_CHECK(right.active);
    ICVFX_CHECK(!right.skipped);
    ICVFX_CHECK(right.covered == scene::CanvasRect{80U, 20U, 32U, 40U});
    // The inner render covers the whole rectangle, not this node's share of
    // it: two nodes splitting the rectangle must sample one image, or the
    // join between their halves would itself be a seam.
    ICVFX_CHECK(right.inner_width == 32U && right.inner_height == 40U);

    // And both nodes derived the same rectangle, which is the point.
    ICVFX_CHECK(left.inner_rect == right.inner_rect);
    ICVFX_CHECK(left.soft_edge_px == right.soft_edge_px);
}

void the_authored_resolution_scale_sizes_the_inner_target() {
    auto value = tracked_scene();
    const auto straddling = region(scene::CanvasRect{40U, 20U, 48U, 40U});

    value.tracked_cameras.front().icvfx.resolution_numerator = 3U;
    value.tracked_cameras.front().icvfx.resolution_denominator = 2U;
    const auto denser =
        render::plan_inner_frustum(
        value, only(value), surface, left_slice, straddling);
    ICVFX_CHECK(denser.inner_width == 72U);
    ICVFX_CHECK(denser.inner_height == 60U);

    // An authored scale outside the contract is clamped once, at the model
    // boundary, rather than guarded again here.
    value.tracked_cameras.front().icvfx.resolution_numerator = 99U;
    value.tracked_cameras.front().icvfx.resolution_denominator = 0U;
    const auto clamped =
        render::plan_inner_frustum(
        value, only(value), surface, left_slice, straddling);
    ICVFX_CHECK(clamped.inner_width
                == 48U * scene::max_icvfx_resolution_scale);
    ICVFX_CHECK(clamped.inner_height
                == 40U * scene::max_icvfx_resolution_scale);
    ICVFX_CHECK(clamped.inner_width <= render::max_inner_frustum_extent);
}

void the_feather_never_outgrows_the_rectangle() {
    // A feather wider than half the rectangle would never reach full strength
    // in the middle, so the plate would be a permanent cross-dissolve.
    const auto value = tracked_scene(true, 500U);
    const auto small = region(scene::CanvasRect{40U, 20U, 20U, 12U});
    const auto plan = render::plan_inner_frustum(
        value, only(value), surface, left_slice, small);
    ICVFX_CHECK(plan.active && !plan.skipped);
    ICVFX_CHECK(plan.soft_edge_px == 6U);
    // Full strength at the centre, nothing outside.
    ICVFX_CHECK(render::icvfx_composite_weight(plan, 50.0F, 26.0F) == 1.0F);
    ICVFX_CHECK(render::icvfx_composite_weight(plan, 39.0F, 26.0F) == 0.0F);
}

// The weight is what two nodes sharing the rectangle's boundary must agree on,
// so it is checked as a function of absolute canvas position and nothing else.
void the_composite_weight_is_a_function_of_canvas_position_alone() {
    const auto value = tracked_scene(true, 8U);
    const auto straddling = region(scene::CanvasRect{40U, 20U, 48U, 40U});
    const auto left = render::plan_inner_frustum(
        value, only(value), surface, left_slice, straddling);
    const auto right =
        render::plan_inner_frustum(
        value, only(value), surface, right_slice, straddling);
    ICVFX_CHECK(!left.skipped && !right.skipped);

    // Walk the whole rectangle, including across the node boundary at x=64.
    bool saw_partial = false;
    for (core::u32 x = 30U; x < 100U; ++x) {
        for (core::u32 y = 10U; y < 70U; ++y) {
            const float canvas_x = static_cast<float>(x) + 0.5F;
            const float canvas_y = static_cast<float>(y) + 0.5F;
            const float a =
                render::icvfx_composite_weight(left, canvas_x, canvas_y);
            const float b =
                render::icvfx_composite_weight(right, canvas_x, canvas_y);
            // Equality, not proximity. One ulp of disagreement here is a step
            // in the feather down the middle of the plate.
            ICVFX_CHECK(a == b);
            if (a > 0.0F && a < 1.0F) {
                saw_partial = true;
            }
        }
    }
    ICVFX_CHECK(saw_partial);

    // Outside, inside, and the ramp between.
    ICVFX_CHECK(render::icvfx_composite_weight(left, 39.0F, 40.0F) == 0.0F);
    ICVFX_CHECK(render::icvfx_composite_weight(left, 64.0F, 40.0F) == 1.0F);
    const float ramp = render::icvfx_composite_weight(left, 44.0F, 40.0F);
    ICVFX_CHECK(ramp > 0.0F && ramp < 1.0F);
    // A skipped node contributes nothing, whatever the position.
    const auto missed = render::plan_inner_frustum(
        value, only(value), surface, left_slice, region(scene::CanvasRect{80U, 20U, 32U, 40U}));
    ICVFX_CHECK(render::icvfx_composite_weight(missed, 90.0F, 40.0F) == 0.0F);
}

// A tracked camera moving or zooming must produce motion vectors, not a
// temporal-history reset. A pose-derived history identity would reset on every
// frame of every move and permanently defeat temporal antialiasing.
void the_inner_view_history_identity_is_stable_under_motion() {
    auto value = tracked_scene();
    const auto straddling = region(scene::CanvasRect{40U, 20U, 48U, 40U});
    const auto plan = render::plan_inner_frustum(
        value, only(value), surface, left_slice, straddling);
    const auto first = render::inner_frustum_view(value, only(value), surface, plan);
    ICVFX_CHECK(first.has_value());
    ICVFX_CHECK(first->history_identity != 0U);

    value.tracked_cameras.front().pose.position.x += 1.25F;
    value.tracked_cameras.front().pose.field_of_view = 0.08F;
    value.tracked_cameras.front().pose.control_frame += 1U;
    const auto moved = render::inner_frustum_view(value, only(value), surface, plan);
    ICVFX_CHECK(moved.has_value());
    ICVFX_CHECK(moved->history_identity == first->history_identity);
    // The view itself did move, or the check above would be vacuous.
    ICVFX_CHECK(!(moved->matrices == first->matrices));
    ICVFX_CHECK(moved->eye.x > first->eye.x);

    // Cutting to a different physical camera is a different view identity,
    // which is the one thing that should reset. The outer camera changing does
    // not: the plate is not drawn from it.
    auto recut = value;
    recut.tracked_cameras.front().camera = mediaserver::doc::NodeId{99U};
    ICVFX_CHECK(render::inner_frustum_view(recut, only(recut), surface, plan)
                    ->history_identity
                != first->history_identity);
    auto outer_cut = value;
    outer_cut.camera_source = mediaserver::doc::NodeId{99U};
    ICVFX_CHECK(render::inner_frustum_view(outer_cut, only(outer_cut), surface, plan)
                    ->history_identity
                == first->history_identity);
}

// The plate must register with the wall.
//
// This is the property the whole feature rests on, and it is stated in wall
// terms rather than in matrix terms deliberately: a point on the LED surface
// is at a fixed place on that surface, and moving the camera changes what is
// seen along the ray to it, never where that ray meets the panel. So every
// wall pixel the plate covers must come back to itself through the plate's
// own view.
//
// The tracked eye is pushed well off the authored axis, because that is the
// case the feature exists for and the case an approximation gets wrong: a view
// built by narrowing the camera's own frustum by the rectangle's fraction of
// the canvas agrees with this only when the camera happens to see the whole
// wall, and the difference between the two is exactly the parallax the plate
// is supposed to show.
void the_plate_registers_with_the_wall_from_a_displaced_eye() {
    auto value = tracked_scene();
    value.tracked_cameras.front().pose.position =
        scene::Vec3{1.6F, 0.9F, 2.5F};
    const scene::CanvasRect rect{40U, 20U, 48U, 40U};
    const auto plan = render::plan_inner_frustum(
        value, only(value), surface, left_slice, region(rect));
    ICVFX_CHECK(plan.active && !plan.skipped);
    const auto view =
        render::inner_frustum_view(value, only(value), surface, plan);
    ICVFX_CHECK(view.has_value());

    // The eye is the tracked one, not the authored one.
    ICVFX_CHECK(view->eye == value.tracked_cameras.front().pose.position);

    // Corners and centre of the plate, in absolute surface-raster pixels.
    const scene::Canvas plate{rect.width, rect.height};
    const std::array<std::pair<core::u32, core::u32>, 5U> samples{{
        {rect.x, rect.y},
        {rect.x + rect.width, rect.y},
        {rect.x, rect.y + rect.height},
        {rect.x + rect.width, rect.y + rect.height},
        {rect.x + rect.width / 2U, rect.y + rect.height / 2U},
    }};
    for (const auto& [px, py] : samples) {
        const auto corners = output::surface_corners(
            surface, scene::CanvasRect{px, py, 1U, 1U});
        ICVFX_CHECK(corners.has_value());
        const auto projected = scene::project_to_canvas(
            view->matrices, plate, corners.value().upper_left);
        ICVFX_CHECK(projected.visible);
        ICVFX_CHECK(std::abs(projected.x + static_cast<float>(rect.x)
                             - static_cast<float>(px))
                    < 0.05F);
        ICVFX_CHECK(std::abs(projected.y + static_cast<float>(rect.y)
                             - static_cast<float>(py))
                    < 0.05F);
    }

    // An inactive plan yields no view rather than a camera at the origin.
    const scene::Scene unauthored = tracked_scene(false);
    const auto idle = render::inner_frustum_view(
        unauthored, only(unauthored), surface, render::IcvfxPlan{});
    ICVFX_CHECK(!idle.has_value());

    // An eye behind the wall has no honest plate, and every node refuses it
    // identically because the pose that put it there is replicated.
    auto behind = tracked_scene();
    behind.tracked_cameras.front().pose.position =
        scene::Vec3{0.0F, 0.0F, -3.0F};
    const auto behind_plan = render::plan_inner_frustum(
        behind, only(behind), surface, left_slice, region(rect));
    ICVFX_CHECK(!render::inner_frustum_view(behind, only(behind), surface,
                                            behind_plan)
                     .has_value());
}

// The lens moves the eye; the wall builds the frustum.
//
// A real lens does not originate its rays at the tracked marker but at the
// entrance pupil, which slides along the optical axis as the lens zooms. That
// offset is the lens's whole contribution to the plate's view: once the eye is
// in the right place, where the plate's rays go is decided by the rectangle on
// the wall, not by the camera's own field of view. The lens still sizes that
// rectangle - that is `output::inner_frustum_region` - but it does not build
// the projection, and the render stays undistorted because the physical lens
// will distort whatever the wall shows.
void a_calibrated_lens_moves_the_inner_view_eye_to_the_entrance_pupil() {
    auto value = tracked_scene();
    const scene::CanvasRect rect{40U, 20U, 48U, 40U};
    scene::LensProfile profile{};
    profile.sensor_width_mm = 36.0F;
    profile.sensor_height_mm = 24.0F;
    profile.focal_length_mm = 25.0F;
    profile.calibration_count = 1U;
    profile.calibration[0U] = scene::LensCalibrationPoint{
        100.0F, 50.0F, 50.0F, 0.125F,
        scene::BrownConradyDistortion{0.2F, 0.0F, 0.0F, 0.0F, 0.0F}};
    // The lens belongs to the tracked camera, not to the outer one: they are
    // two different objects on the stage.
    value.camera.lens_profile = profile;
    value.tracked_cameras.front().authored.lens_profile = profile;
    value.tracked_cameras.front().encoders =
        scene::LensEncoders{100.0F, 50.0F, 0.0F, true, true, false};

    const auto plan = render::plan_inner_frustum(
        value, only(value), surface, left_slice, region(rect));
    const auto view =
        render::inner_frustum_view(value, only(value), surface, plan);
    ICVFX_CHECK(view.has_value());

    // The eye moved along the optical axis by the calibrated pupil offset and
    // by nothing else. The camera looks down -Z, so the pupil is nearer the
    // wall than the marker.
    const auto& pose = value.tracked_cameras.front().pose;
    ICVFX_CHECK(std::abs(view->eye.z - (pose.position.z - 0.125F)) < 1.0e-6F);
    ICVFX_CHECK(std::abs(view->eye.x - pose.position.x) < 1.0e-6F);
    ICVFX_CHECK(std::abs(view->eye.y - pose.position.y) < 1.0e-6F);

    // And the projection is the wall's, not the lens's: the same rectangle
    // viewed from the same pupil gives the same matrices whatever field of
    // view the camera's own frustum would have implied.
    auto longer = value;
    longer.tracked_cameras.front().pose.field_of_view = 0.02F;
    const auto longer_view =
        render::inner_frustum_view(longer, only(longer), surface, plan);
    ICVFX_CHECK(longer_view.has_value());
    ICVFX_CHECK(longer_view->matrices == view->matrices);

    // Registration still holds with the pupil offset applied.
    const scene::Canvas plate{rect.width, rect.height};
    const auto corners = output::surface_corners(
        surface, scene::CanvasRect{rect.x, rect.y, 1U, 1U});
    ICVFX_CHECK(corners.has_value());
    const auto projected = scene::project_to_canvas(
        view->matrices, plate, corners.value().upper_left);
    ICVFX_CHECK(projected.visible);
    ICVFX_CHECK(std::abs(projected.x) < 0.05F);
    ICVFX_CHECK(std::abs(projected.y) < 0.05F);
}

// The plan reaches the renderer through the wall geometry, so the two halves
// are exercised together at least once.
void a_wall_surface_drives_the_plan_end_to_end() {
    auto value = tracked_scene();
    value.canvas = scene::Canvas{3'840U, 2'160U};
    const output::PhysicalSurface wide_surface{
        scene::Vec3{0.0F, 0.0F, -4.0F},
        scene::Vec3{1.0F, 0.0F, 0.0F},
        scene::Vec3{0.0F, 1.0F, 0.0F},
        8.0F,
        4.5F,
        scene::Canvas{3'840U, 2'160U},
    };
    const scene::Camera tracked = scene::tracked_view_camera(
        value.camera, value.tracked_cameras.front().pose);
    const auto derived = output::inner_frustum_region(
        surface, tracked, value.tracked_cameras.front().pose.aspect_ratio,
        value.tracked_cameras.front().icvfx.margin_px);
    ICVFX_CHECK(derived.has_value() && derived.value().valid);

    const std::array<scene::CanvasRect, 4U> ports{{
        {0U, 0U, 960U, 2'160U},
        {960U, 0U, 960U, 2'160U},
        {1'920U, 0U, 960U, 2'160U},
        {2'880U, 0U, 960U, 2'160U},
    }};
    std::size_t rendering = 0U;
    std::size_t skipping = 0U;
    scene::CanvasRect shared{};
    for (std::size_t index = 0U; index < ports.size(); ++index) {
        const auto plan =
            render::plan_inner_frustum(
        value, only(value), wide_surface, ports[index], derived.value());
        ICVFX_CHECK(plan.active);
        if (index == 0U) {
            shared = plan.inner_rect;
        } else {
            ICVFX_CHECK(plan.inner_rect == shared);
        }
        if (plan.skipped) {
            ++skipping;
        } else {
            ++rendering;
        }
    }
    // A long lens on a wide wall: some nodes draw the plate, some do not, and
    // that is the entire performance argument for the feature.
    ICVFX_CHECK(rendering >= 1U);
    ICVFX_CHECK(skipping >= 1U);
}


// ---------------------------------------------------------------------------
// Mesh walls
// ---------------------------------------------------------------------------

// The fixture wall, bowed into a horizontal arc whose edges come toward the
// tracked eye. The complete mesh: UV0 spans the whole raster.
[[nodiscard]] output::ProjectionMesh arc_wall() {
    auto built = output::horizontal_arc_mesh(surface, 0.1F, 16U);
    check(built.has_value(), "arc mesh", __LINE__);
    return std::move(built).value();
}

// A plate on a curved wall registers with the wall, exactly as one on a flat
// wall does: every wall point under the plate rectangle is seen by the plate's
// own render, and the warp lands each of those points on its own raster pixel
// inside the rectangle. Stated in wall terms - surface pixels and the mesh
// vertices that carry them - not in terms of the matrices that produce it.
void a_mesh_wall_plate_registers_with_the_wall() {
    auto value = tracked_scene();
    value.tracked_cameras.front().pose.position = scene::Vec3{1.2F, 0.4F, 2.5F};
    const output::ProjectionMesh mesh = arc_wall();
    const render::IcvfxSurfaceMesh wall_mesh{&mesh, nullptr};
    const scene::TrackedCameraView& view = only(value);
    const scene::Camera tracked = render::tracked_stage_camera(view);

    const scene::InnerFrustumRegion found = render::inner_frustum_mesh_region(
        wall_mesh, surface.raster, tracked, view, view.icvfx.margin_px);
    ICVFX_CHECK(found.valid);
    ICVFX_CHECK(!found.clamped);
    // A long lens sees a part of the wall, offset toward the eye's +X.
    ICVFX_CHECK(found.rect.width < surface.raster.width);
    ICVFX_CHECK(found.rect.x + found.rect.width / 2U
                > surface.raster.width / 2U);

    const auto plan = render::plan_inner_frustum(value, view, surface,
                                                 right_slice, found);
    ICVFX_CHECK(plan.active && !plan.skipped);
    const auto built =
        render::inner_frustum_mesh_view(value, view, surface, wall_mesh, plan);
    ICVFX_CHECK(built.has_value());
    // Drawn from the tracked eye, with the plate's own stable history.
    ICVFX_CHECK(built.value().render.eye == tracked.eye);
    const auto flat = render::inner_frustum_view(value, view, surface, plan);
    ICVFX_CHECK(flat.has_value());
    ICVFX_CHECK(built.value().render.history_identity
                == flat->history_identity);
    ICVFX_CHECK(built.value().render.guard_band);
    // A curved wall has no single plane for plate depth of field.
    ICVFX_CHECK(!built.value().render.plate_lens.has_value());

    const output::MeshDestinationWindow window{surface.raster, plan.inner_rect};
    const auto warped = output::mesh_warp_vertices(
        mesh, built.value().mesh_view, nullptr, window);
    ICVFX_CHECK(warped.has_value());
    std::size_t under_plate = 0U;
    for (std::size_t index = 0U; index < mesh.vertices.size(); ++index) {
        const float px = mesh.vertices[index].destination_u
            * static_cast<float>(surface.raster.width);
        const bool inside_x = px >= static_cast<float>(plan.inner_rect.x)
            && px <= static_cast<float>(plan.inner_rect.x
                                        + plan.inner_rect.width);
        if (!inside_x) {
            continue;
        }
        ++under_plate;
        // The rib's wall points at the rectangle's top, middle and bottom
        // rows: seen by the plate render, in front of its eye. A rib runs the
        // full wall height, so its own end vertices are not - only the part
        // under the plate is.
        if (index % 2U == 0U) {
            const scene::Vec3 top = mesh.vertices[index].position;
            const scene::Vec3 bottom = mesh.vertices[index + 1U].position;
            for (const core::u32 row :
                 {plan.inner_rect.y,
                  plan.inner_rect.y + plan.inner_rect.height / 2U,
                  plan.inner_rect.y + plan.inner_rect.height}) {
                const float v = static_cast<float>(row)
                    / static_cast<float>(surface.raster.height);
                const scene::Vec3 point = top + (bottom - top) * v;
                const scene::Vec4 seen = scene::transform(
                    built.value().mesh_view.matrices.view_projection,
                    scene::Vec4{point.x, point.y, point.z, 1.0F});
                ICVFX_CHECK(seen.w > 0.0F);
                ICVFX_CHECK(std::abs(seen.x / seen.w) <= 1.0001F);
                ICVFX_CHECK(std::abs(seen.y / seen.w) <= 1.0001F);
            }
        }
        // And placed on its own pixel of the rectangle.
        const scene::Vec4 destination = warped.value()[index].destination_clip;
        const float expected_x = (px - static_cast<float>(plan.inner_rect.x))
            / static_cast<float>(plan.inner_rect.width) * 2.0F - 1.0F;
        ICVFX_CHECK(std::abs(destination.x - expected_x) < 1.0e-4F);
    }
    // The arc has ribs under the plate, or the loop above checked nothing.
    ICVFX_CHECK(under_plate >= 4U);
}

// The rest of a curved wall has no say in the plate's view.
//
// The arc here wraps far enough round that, seen from a tracked eye close to
// one end, its far end is behind that eye - no single render could see the
// whole wall. A plate at the near end must still be drawn, and a plate whose
// rectangle reaches the part behind the eye must be refused with that reason,
// on every node alike.
void only_the_wall_under_the_plate_constrains_its_view() {
    auto value = tracked_scene();
    output::PhysicalSurface wide = surface;
    wide.width = 24.0F;
    auto built = output::horizontal_arc_mesh(wide, 0.45F, 64U);
    ICVFX_CHECK(built.has_value());
    const output::ProjectionMesh mesh = std::move(built).value();
    const render::IcvfxSurfaceMesh wall_mesh{&mesh, nullptr};
    // Near the right end of the wall, just in front of it.
    const scene::Vec3 eye{4.5F, 0.0F, 1.5F};
    ICVFX_CHECK(!output::mesh_perspective_view_for(mesh, eye, 0.05F, 1'000.0F)
                     .has_value());

    value.tracked_cameras.front().pose.position = eye;
    render::IcvfxPlan plan{};
    plan.camera = only(value).camera;
    plan.active = true;
    plan.inner_rect = scene::CanvasRect{64U, 24U, 12U, 48U};
    plan.covered = plan.inner_rect;
    plan.inner_width = plan.inner_rect.width;
    plan.inner_height = plan.inner_rect.height;
    const auto near_plate = render::inner_frustum_mesh_view(
        value, only(value), wide, wall_mesh, plan);
    ICVFX_CHECK(near_plate.has_value());

    // The whole raster includes the far end of the arc, behind this eye.
    plan.inner_rect = scene::whole(wide.raster);
    const auto whole_wall = render::inner_frustum_mesh_view(
        value, only(value), wide, wall_mesh, plan);
    ICVFX_CHECK(!whole_wall.has_value());
    ICVFX_CHECK(whole_wall.error()
                == output::PerspectiveMappingError::MeshBehindEye);

    // An idle plan never yields a view, and no mesh never yields one either.
    ICVFX_CHECK(!render::inner_frustum_mesh_view(value, only(value), wide,
                                                 wall_mesh, render::IcvfxPlan{})
                     .has_value());
    ICVFX_CHECK(!render::inner_frustum_mesh_view(
                     value, only(value), wide, render::IcvfxSurfaceMesh{}, plan)
                     .has_value());
}

// An imported mesh keeps file-local vertices and is placed by its surface
// calibration. The plate rectangle over it must be the one a node would find
// over the same mesh already placed in the world - otherwise a UV-mesh wall
// would put its plate wherever the file's own origin happened to be.
void an_imported_mesh_is_placed_before_its_plate_is_found() {
    auto value = tracked_scene();
    const output::ProjectionMesh world = arc_wall();
    // The same geometry in "file" coordinates: centred on the origin, then
    // carried back to the fixture wall by a placement two units to its left
    // and one forward, which the tracked eye then has to account for.
    output::ProjectionMesh local = world;
    output::ProjectionMeshPlacement placement{};
    placement.calibration = surface;
    placement.calibration.center = scene::Vec3{-2.0F, 0.0F, 1.0F};
    output::ProjectionMesh placed = world;
    for (std::size_t index = 0U; index < world.vertices.size(); ++index) {
        placed.vertices[index].position = world.vertices[index].position
            + placement.calibration.center;
    }
    const scene::TrackedCameraView& view = only(value);
    const scene::Camera tracked = render::tracked_stage_camera(view);
    const auto from_placement = render::inner_frustum_mesh_region(
        render::IcvfxSurfaceMesh{&local, &placement}, surface.raster, tracked,
        view, view.icvfx.margin_px);
    const auto from_world = render::inner_frustum_mesh_region(
        render::IcvfxSurfaceMesh{&placed, nullptr}, surface.raster, tracked,
        view, view.icvfx.margin_px);
    const auto unplaced = render::inner_frustum_mesh_region(
        render::IcvfxSurfaceMesh{&local, nullptr}, surface.raster, tracked,
        view, view.icvfx.margin_px);
    ICVFX_CHECK(from_placement.valid && from_world.valid && unplaced.valid);
    ICVFX_CHECK(from_placement == from_world);
    // The wall moved left under a camera that did not, so its plate is found
    // further right in the wall's own raster than on the unplaced file.
    ICVFX_CHECK(from_placement.rect.x > unplaced.rect.x);
}

// ---------------------------------------------------------------------------
// A real wall: four imported UV-mesh surfaces forming one curved LED volume
// ---------------------------------------------------------------------------

// The live setup a plate went missing on: four externally split FBX patches,
// each its own surface sharing one calibration (origin 10.3 m down stage +Z,
// local +X along stage -Z, so the patches' front faces look along stage +X),
// each with the full 11520 x 1536 raster, and the tracked pose the Director
// was sending - stage space, feed origin already applied. The camera stands
// inside the volume looking down stage -Z into the curve; only the "node3"
// patch, which wraps round in front of it, is in its view.
#if defined(MEDIASERVER_FOUR_LED_WALL_FIXTURE)
[[nodiscard]] output::OutputMap four_wall_map(const char* file,
                                              core::u64 map_node) {
    output::ResolvedProjectionSurface patch_surface{};
    patch_surface.geometry.center = scene::Vec3{0.0F, 0.0F, 10.3F};
    patch_surface.geometry.right = scene::Vec3{0.0F, 0.0F, -1.0F};
    patch_surface.geometry.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    patch_surface.geometry.width = 8.0F;
    patch_surface.geometry.height = 1.06666672F;
    patch_surface.geometry.raster = scene::Canvas{11'520U, 1'536U};
    patch_surface.shape = output::ProjectionSurfaceShape::ImportedMesh;
    patch_surface.mesh_source =
        std::string{MEDIASERVER_FOUR_LED_WALL_FIXTURE} + "/" + file;
    patch_surface.mesh_scale = 1.0F;
    output::OutputMap map{};
    map.node = mediaserver::doc::NodeId{map_node};
    map.projection = output::FeedProjection::PlanarSurface;
    map.surface = mediaserver::doc::NodeId{map_node + 1000U};
    map.source = scene::whole(patch_surface.geometry.raster);
    map.viewport = scene::CanvasRect{0U, 0U, 2'880U, 1'536U};
    map.resolved_surface = std::move(patch_surface);
    return map;
}

[[nodiscard]] scene::Scene four_wall_frame() {
    scene::Scene frame{};
    frame.canvas = scene::Canvas{11'520U, 1'536U};
    frame.camera.eye = scene::Vec3{0.0F, 0.165F, 6.564F};
    frame.camera.target = scene::Vec3{0.0F, 0.165F, 5.564F};
    frame.renderer_settings.icvfx.inner_frustum = true;
    scene::TrackedCameraView view{};
    view.camera = mediaserver::doc::NodeId{1'202U};
    view.source = mediaserver::doc::NodeId{100'084U};
    view.authored = frame.camera;
    view.authored.near_plane = 0.1F;
    view.authored.far_plane = 1'000.0F;
    view.icvfx.inner_frustum = true;
    view.icvfx.margin_px = 24U;
    view.pose.position = scene::Vec3{-3.259F, 1.082F, 4.439F};
    view.pose.yaw = 5.38F / 360.0F;
    view.pose.pitch = 2.27F / 360.0F;
    view.pose.roll = 0.27F / 360.0F;
    view.pose.field_of_view = 21.83F / 360.0F;
    view.pose.aspect_ratio = 1.778F;
    view.pose.valid = true;
    frame.tracked_cameras.push_back(view);
    return frame;
}

void a_real_curved_wall_puts_the_plate_on_the_patch_the_camera_faces() {
    const scene::Scene frame = four_wall_frame();
    const scene::TrackedCameraView& view = only(frame);
    const scene::Camera tracked = render::tracked_stage_camera(view);
    // The camera looks down stage -Z with a slight pan left and tilt up.
    const scene::Vec3 forward = scene::normalize(tracked.target - tracked.eye);
    ICVFX_CHECK(forward.z < -0.99F && forward.x < 0.0F && forward.y > 0.0F);

    struct Patch final {
        const char* file;
        core::u64 map;
        bool seen;
    };
    constexpr std::array<Patch, 4U> patches{{
        {"node2.fbx", 100'014U, false},
        {"node3.fbx", 100'058U, true},
        {"node4.fbx", 100'059U, false},
        {"node5.fbx", 100'060U, false},
    }};
    for (const Patch& patch : patches) {
        const output::OutputMap map = four_wall_map(patch.file, patch.map);
        // Loaded exactly as a node loads it: the projection cache's
        // background worker, then the frozen projection the patch_wall warps with.
        render::OutputProjectionCache cache;
        static_cast<void>(cache.resolve(frame, map));
        ICVFX_CHECK(
            render::detail::wait_for_output_projection_cache_for_test(cache));
        const auto projection = cache.resolve(frame, map);
        ICVFX_CHECK(projection.has_value());
        ICVFX_CHECK(projection->surface_mesh != nullptr);
        ICVFX_CHECK(projection->mesh_placement.has_value());
        const render::IcvfxSurfaceMesh patch_wall{projection->surface_mesh.get(),
                                            &*projection->mesh_placement};
        const scene::Canvas raster = map.resolved_surface->geometry.raster;
        const scene::InnerFrustumRegion region =
            render::inner_frustum_mesh_region(patch_wall, raster, tracked, view,
                                              view.icvfx.margin_px);
        ICVFX_CHECK(region.valid == patch.seen);
        if (!patch.seen) {
            continue;
        }
        // A part of the patch, not all of it, and a fit rather than the
        // whole-patch_wall fallback: the camera's 21.8 degree view of a patch
        // about two metres away.
        ICVFX_CHECK(!region.clamped);
        ICVFX_CHECK(region.rect.width < raster.width / 2U);
        ICVFX_CHECK(region.rect.height < raster.height / 2U);
        // The rectangle the live pose put on this patch, pinned so a change
        // in placement, camera convention or raster shows up here first.
        ICVFX_CHECK(region.rect.x + 1U >= 3'959U && region.rect.x <= 3'960U);
        ICVFX_CHECK(region.rect.y + 1U >= 747U && region.rect.y <= 748U);
        ICVFX_CHECK(region.rect.width + 2U >= 3'931U
                    && region.rect.width <= 3'933U);
        ICVFX_CHECK(region.rect.height + 2U >= 614U
                    && region.rect.height <= 616U);

        // And the plate the stage would draw there fits from the tracked eye.
        const auto plan = render::plan_inner_frustum(
            frame, view, map.resolved_surface->geometry, map.source, region);
        ICVFX_CHECK(plan.active && !plan.skipped);
        const auto built = render::inner_frustum_mesh_view(
            frame, view, map.resolved_surface->geometry, patch_wall, plan);
        ICVFX_CHECK(built.has_value());
        ICVFX_CHECK(built.value().render.eye == tracked.eye);
    }
}
#endif

// ---------------------------------------------------------------------------
// GPU
// ---------------------------------------------------------------------------

[[nodiscard]] rhi::Device open_device() {
    rhi::DeviceConfig config{};
    config.debug = false;
    auto created = rhi::Device::create(config);
    check(created.has_value(), "device creation", __LINE__);
    return std::move(created).value();
}

// A deterministic image with variation in every channel and in both axes, so
// an equality check on the composite is checking something.
[[nodiscard]] std::vector<core::u8> pattern(
    core::u32 origin_x, core::u32 origin_y, core::u32 width, core::u32 height,
    core::u8 bias) {
    std::vector<core::u8> pixels(
        static_cast<std::size_t>(width) * height * 4U);
    for (core::u32 y = 0U; y < height; ++y) {
        for (core::u32 x = 0U; x < width; ++x) {
            const core::u32 canvas_x = origin_x + x;
            const core::u32 canvas_y = origin_y + y;
            const auto index =
                (static_cast<std::size_t>(y) * width + x) * 4U;
            pixels[index] = static_cast<core::u8>((canvas_x * 3U + bias) & 0xFFU);
            pixels[index + 1U] =
                static_cast<core::u8>((canvas_y * 5U + bias) & 0xFFU);
            pixels[index + 2U] = static_cast<core::u8>(
                ((canvas_x + canvas_y) * 2U + bias) & 0xFFU);
            pixels[index + 3U] = 0xFFU;
        }
    }
    return pixels;
}

[[nodiscard]] rhi::Texture uploaded_texture(
    rhi::Device& device, core::u32 width, core::u32 height,
    std::span<const core::u8> pixels) {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.sampled = true;
    auto texture = device.create_texture(desc);
    check(texture.has_value(), "texture creation", __LINE__);
    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "upload acquire", __LINE__);
    auto commands = std::move(acquired).value();
    check(commands.upload(texture.value(), pixels), "upload", __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "upload submit", __LINE__);
    check(std::move(submitted).value().wait(), "upload wait", __LINE__);
    return std::move(texture).value();
}

[[nodiscard]] std::vector<core::u8> composite(
    rhi::Device& device, const render::IcvfxPlan& plan,
    scene::CanvasRect slice, const rhi::Texture& outer,
    const rhi::Texture* inner) {
    auto created =
        render::IcvfxCompositor::create(device, slice.width, slice.height);
    check(created.has_value(), "compositor creation", __LINE__);
    auto compositor = std::move(created).value();

    const core::u32 bytes = slice.width * slice.height * 4U;
    auto readback = device.create_readback(bytes);
    check(readback.has_value(), "readback creation", __LINE__);
    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "composite acquire", __LINE__);
    auto commands = std::move(acquired).value();
    check(compositor.render(commands, plan, slice, outer, inner),
          "composite render", __LINE__);
    check(commands.download(compositor.target(), readback.value()),
          "composite download", __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "composite submit", __LINE__);
    check(std::move(submitted).value().wait(), "composite wait", __LINE__);

    const auto pixels = readback.value().map();
    check(pixels.size() >= bytes, "composite map", __LINE__);
    std::vector<core::u8> result(pixels.begin(), pixels.begin() + bytes);
    readback.value().unmap();
    return result;
}

// The stage: the second render plus the composite, as a presenter uses it.
//
// The unit tests above prove the plan and the composite in isolation. What
// this adds is the part a wall actually depends on - that asking for a plate
// produces a *different* image from not asking, that a skipping node gets its
// own texture back untouched rather than a copy, and that an untracked show
// costs nothing at all.
void the_stage_draws_a_plate_only_when_there_is_one_to_draw() {
    auto device = open_device();
    const auto value = tracked_scene(true, 10U);
    const auto inner_rect = scene::CanvasRect{40U, 20U, 48U, 40U};

    auto created = render::IcvfxStage::create(device, left_slice.width,
                                              left_slice.height);
    ICVFX_CHECK(created.has_value());
    auto stage = std::move(created).value();

    auto outer = uploaded_texture(
        device, left_slice.width, left_slice.height,
        pattern(left_slice.x, left_slice.y, left_slice.width,
                left_slice.height, 0U));

    const auto record = [&](const scene::Scene& frame,
                            const scene::InnerFrustumRegion& region) {
        auto acquired = device.acquire_commands();
        check(acquired.has_value(), "stage acquire", __LINE__);
        auto commands = std::move(acquired).value();
        const std::array<scene::InnerFrustumRegion, 1U> regions{region};
        const rhi::Texture* result = stage.record(
            commands, frame, surface, left_slice,
            std::span<const scene::InnerFrustumRegion>{regions}, outer);
        auto submitted = device.submit(std::move(commands));
        check(submitted.has_value(), "stage submit", __LINE__);
        check(std::move(submitted).value().wait(), "stage wait", __LINE__);
        return result;
    };

    // Untracked: the caller's own texture back, and no second renderer was
    // ever constructed.
    const auto untracked = tracked_scene(false);
    ICVFX_CHECK(record(untracked, region(inner_rect)) == &outer);
    ICVFX_CHECK(stage.inner() == nullptr);
    ICVFX_CHECK(!stage.last_plans().front().active);

    // A plate entirely inside the other node: skipped, same texture back,
    // still no second renderer. This is the performance argument for ICVFX
    // and it has to cost literally nothing.
    ICVFX_CHECK(record(value, region(scene::CanvasRect{80U, 20U, 32U, 40U}))
                == &outer);
    ICVFX_CHECK(stage.last_plans().front().skipped);
    ICVFX_CHECK(stage.inner() == nullptr);
    ICVFX_CHECK(stage.stats().inner_frames_rendered == 0U);

    // A plate this node owns: a different texture, and the second renderer
    // now exists.
    const rhi::Texture* const composited = record(value, region(inner_rect));
    ICVFX_CHECK(composited != nullptr);
    ICVFX_CHECK(composited != &outer);
    ICVFX_CHECK(stage.last_plans().front().active && !stage.last_plans().front().skipped);
    ICVFX_CHECK(stage.inner() != nullptr);
    ICVFX_CHECK(stage.stats().inner_frames_rendered == 1U);

    // The inner render target is sized from the canvas and the authored
    // density, never from the plate - a target that followed the plate would
    // reallocate every frame the camera moved.
    ICVFX_CHECK(stage.inner()->width() == value.canvas.width);
    ICVFX_CHECK(stage.inner()->height() == value.canvas.height);
    auto denser = value;
    denser.tracked_cameras.front().icvfx.resolution_numerator = 2U;
    denser.tracked_cameras.front().icvfx.resolution_denominator = 1U;
    ICVFX_CHECK(record(denser, region(inner_rect)) != nullptr);
    ICVFX_CHECK(stage.inner()->width() == value.canvas.width * 2U);

    // And a plate that moves does not resize anything.
    const core::u32 settled = stage.inner()->width();
    ICVFX_CHECK(record(denser, region(scene::CanvasRect{30U, 10U, 60U, 50U}))
                != nullptr);
    ICVFX_CHECK(stage.inner()->width() == settled);
}


[[nodiscard]] std::vector<core::u8> read_back(rhi::Device& device,
                                              const rhi::Texture& texture) {
    const core::u32 bytes = texture.width() * texture.height() * 4U;
    auto readback = device.create_readback(bytes);
    check(readback.has_value(), "readback creation", __LINE__);
    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "readback acquire", __LINE__);
    auto commands = std::move(acquired).value();
    check(commands.download(texture, readback.value()), "download", __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "readback submit", __LINE__);
    check(std::move(submitted).value().wait(), "readback wait", __LINE__);
    const auto pixels = readback.value().map();
    check(pixels.size() >= bytes, "readback map", __LINE__);
    std::vector<core::u8> result(pixels.begin(), pixels.begin() + bytes);
    readback.value().unmap();
    return result;
}

// The stage on a curved wall, as a presenter drives it for a mesh-warped
// output: the plate is rendered, warped into its rectangle and composited over
// the wall this node already drew. Outside the rectangle the wall is untouched
// to the byte; inside it, the plate replaces it.
void the_stage_draws_a_plate_on_a_mesh_wall() {
    auto device = open_device();
    const auto value = tracked_scene(true, 0U);
    const output::ProjectionMesh mesh = arc_wall();
    const render::IcvfxSurfaceMesh wall_mesh{&mesh, nullptr};
    const scene::TrackedCameraView& view = only(value);
    const scene::InnerFrustumRegion found = render::inner_frustum_mesh_region(
        wall_mesh, surface.raster, render::tracked_stage_camera(view), view,
        view.icvfx.margin_px);
    ICVFX_CHECK(found.valid);
    ICVFX_CHECK(found.rect.x < left_slice.width);

    auto created = render::IcvfxStage::create(device, left_slice.width,
                                              left_slice.height);
    ICVFX_CHECK(created.has_value());
    auto stage = std::move(created).value();
    // Saturated, so the plate (an empty scene, rendered dark) differs from it
    // everywhere it lands.
    std::vector<core::u8> bright(
        static_cast<std::size_t>(left_slice.width) * left_slice.height * 4U,
        0xFFU);
    auto outer = uploaded_texture(device, left_slice.width, left_slice.height,
                                  bright);

    const std::array<scene::InnerFrustumRegion, 1U> regions{found};
    const auto record = [&](const render::IcvfxSurfaceMesh& with) {
        auto acquired = device.acquire_commands();
        check(acquired.has_value(), "stage acquire", __LINE__);
        auto commands = std::move(acquired).value();
        const rhi::Texture* result = stage.record(
            commands, value, surface, with, left_slice,
            std::span<const scene::InnerFrustumRegion>{regions}, outer);
        auto submitted = device.submit(std::move(commands));
        check(submitted.has_value(), "stage submit", __LINE__);
        check(std::move(submitted).value().wait(), "stage wait", __LINE__);
        return result;
    };

    // No mesh held: the wall exactly as it was, and no second render.
    ICVFX_CHECK(record(render::IcvfxSurfaceMesh{}) == &outer);
    ICVFX_CHECK(stage.inner() == nullptr);

    const rhi::Texture* const composited = record(wall_mesh);
    ICVFX_CHECK(composited != nullptr);
    ICVFX_CHECK(composited != &outer);
    ICVFX_CHECK(stage.stats().inner_frames_rendered == 1U);

    const std::vector<core::u8> pixels = read_back(device, *composited);
    const auto at = [&](core::u32 x, core::u32 y) {
        return pixels.data()
            + (static_cast<std::size_t>(y) * left_slice.width + x) * 4U;
    };
    std::size_t outside = 0U;
    std::size_t replaced = 0U;
    for (core::u32 y = 0U; y < left_slice.height; ++y) {
        for (core::u32 x = 0U; x < left_slice.width; ++x) {
            const core::u8* pixel = at(x, y);
            const bool in_rect = x >= found.rect.x
                && x < found.rect.x + found.rect.width && y >= found.rect.y
                && y < found.rect.y + found.rect.height;
            if (!in_rect) {
                ++outside;
                ICVFX_CHECK(pixel[0] == 0xFFU && pixel[1] == 0xFFU
                            && pixel[2] == 0xFFU);
            } else if (pixel[0] != 0xFFU || pixel[1] != 0xFFU
                       || pixel[2] != 0xFFU) {
                ++replaced;
            }
        }
    }
    ICVFX_CHECK(outside > 0U);
    // Every pixel of the rectangle this node owns came from the plate.
    const core::u32 owned_width = left_slice.width - found.rect.x;
    ICVFX_CHECK(replaced
                == static_cast<std::size_t>(std::min(owned_width,
                                                     found.rect.width))
                    * found.rect.height);

    // A new asset generation: the plate's renderer re-opens every media
    // source before it draws anything. That plate is left off - the wall
    // under it untouched, not the output failed - and the stage says it is
    // owed, which the presenter turns into an incomplete frame.
    auto adopting = value;
    adopting.asset_refresh_generation = 7U;
    scene::MediaInstance clip{};
    clip.source = "mediaserver-icvfx-missing-plate-media.mov";
    adopting.media.push_back(clip);
    const auto record_adopting = [&]() {
        auto acquired = device.acquire_commands();
        check(acquired.has_value(), "stage acquire", __LINE__);
        auto commands = std::move(acquired).value();
        const rhi::Texture* result = stage.record(
            commands, adopting, surface, wall_mesh, left_slice,
            std::span<const scene::InnerFrustumRegion>{regions}, outer);
        auto submitted = device.submit(std::move(commands));
        check(submitted.has_value(), "stage submit", __LINE__);
        check(std::move(submitted).value().wait(), "stage wait", __LINE__);
        return result;
    };
    const auto rendered_before = stage.stats().inner_frames_rendered;
    ICVFX_CHECK(record_adopting() == &outer);
    ICVFX_CHECK(stage.plate_pending());
    ICVFX_CHECK(stage.stats().inner_frames_rendered == rendered_before);
    // Once adopted, the plate is drawn again and nothing is owed.
    bool adopted = false;
    for (int attempt = 0; attempt < 500 && !adopted; ++attempt) {
        const rhi::Texture* const again = record_adopting();
        ICVFX_CHECK(again != nullptr);
        adopted = !stage.plate_pending();
        if (!adopted) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }
    ICVFX_CHECK(adopted);
    ICVFX_CHECK(stage.stats().inner_frames_rendered > rendered_before);
}

#if defined(MEDIASERVER_FOUR_LED_WALL_FIXTURE)
// The real curved wall, plated every frame for hundreds of frames, on the
// backend a render node uses - D3D12, with its validation layer on.
//
// A node removed its GPU device within seconds of drawing this plate every
// frame. The plate renderer was a complete second scene renderer at the
// wall's full 11520 x 1536 raster - gigabytes of video memory for an output
// that shows that raster at a quarter of its width. It is now sized from the
// extent the output shows the wall at, which this test pins, and the device
// must still be alive after every frame.
void a_mesh_wall_plate_records_every_frame_on_d3d12() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        std::cout << "      (skipped: no D3D12 device)\n";
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    config.debug = true;
    auto opened = rhi::Device::create(config);
    ICVFX_CHECK(opened.has_value());
    rhi::Device device = std::move(opened).value();

    scene::Scene frame = four_wall_frame();
    const output::OutputMap map = four_wall_map("node3.fbx", 100'058U);
    render::OutputProjectionCache cache;
    static_cast<void>(cache.resolve(frame, map));
    ICVFX_CHECK(
        render::detail::wait_for_output_projection_cache_for_test(cache));
    const auto projection = cache.resolve(frame, map);
    ICVFX_CHECK(projection.has_value() && projection->surface_mesh);
    const render::IcvfxSurfaceMesh patch_wall{projection->surface_mesh.get(),
                                              &*projection->mesh_placement};
    const output::PhysicalSurface& patch_surface =
        map.resolved_surface->geometry;

    // An output window showing the patch's 11520 x 1536 raster in a
    // 1440 x 192 viewport.
    constexpr scene::Canvas shown{1'440U, 192U};
    auto created = render::IcvfxStage::create(device, shown.width,
                                              shown.height);
    ICVFX_CHECK(created.has_value());
    auto stage = std::move(created).value();
    std::vector<core::u8> grey(
        static_cast<std::size_t>(shown.width) * shown.height * 4U, 0x80U);
    auto outer = uploaded_texture(device, shown.width, shown.height, grey);

    constexpr core::u32 frames = 240U;
    for (core::u32 index = 0U; index < frames; ++index) {
        // A camera that moves a little every frame, as a tracked one does.
        frame.tracked_cameras.front().pose.position.x =
            -3.259F + 0.002F * static_cast<float>(index % 60U);
        frame.tracked_cameras.front().pose.control_frame = 1'000U + index;
        const scene::TrackedCameraView& view = only(frame);
        const scene::InnerFrustumRegion region =
            render::inner_frustum_mesh_region(
                patch_wall, patch_surface.raster,
                render::tracked_stage_camera(view), view,
                view.icvfx.margin_px);
        ICVFX_CHECK(region.valid);
        const std::array<scene::InnerFrustumRegion, 1U> regions{region};
        auto acquired = device.acquire_commands();
        ICVFX_CHECK(acquired.has_value());
        auto commands = std::move(acquired).value();
        const rhi::Texture* result = stage.record(
            commands, frame, patch_surface, patch_wall, map.source,
            std::span<const scene::InnerFrustumRegion>{regions}, outer,
            nullptr, shown);
        ICVFX_CHECK(result != nullptr);
        ICVFX_CHECK(result != &outer);
        auto submitted = device.submit(std::move(commands));
        ICVFX_CHECK(submitted.has_value());
        ICVFX_CHECK(std::move(submitted).value().wait());
        ICVFX_CHECK(device.lost_reason().empty());
    }
    ICVFX_CHECK(stage.stats().inner_frames_rendered == frames);
    ICVFX_CHECK(stage.stats().refusals == 0U);
    // Sized from what the output shows, not from the 11520 x 1536 raster.
    ICVFX_CHECK(stage.inner() != nullptr);
    ICVFX_CHECK(stage.inner()->width() == shown.width);
    ICVFX_CHECK(stage.inner()->height() == shown.height);
}
#endif

// The test the feature stands on.
//
// One full-canvas composite, and two node composites of the same canvas with
// the inner rectangle straddling the node boundary. Both nodes get their own
// outer image - an exact crop of the full one, so the inputs are identical by
// construction - and both sample the one shared inner render. Every pixel the
// full composite and the node composites have in common must be *equal*.
void a_two_node_composite_is_byte_exact_against_the_whole_canvas() {
    auto device = open_device();
    const auto value = tracked_scene(true, 10U);
    // Deliberately straddling x = 64, the boundary between the two nodes, and
    // wide enough that the feather crosses it too.
    const auto inner_rect = scene::CanvasRect{40U, 20U, 48U, 40U};
    const auto region_value = region(inner_rect);

    const auto whole = scene::whole(wall);
    const auto whole_plan =
        render::plan_inner_frustum(
        value, only(value), surface, whole, region_value);
    const auto left_plan =
        render::plan_inner_frustum(
        value, only(value), surface, left_slice, region_value);
    const auto right_plan =
        render::plan_inner_frustum(
        value, only(value), surface, right_slice, region_value);
    ICVFX_CHECK(whole_plan.active && !whole_plan.skipped);
    ICVFX_CHECK(!left_plan.skipped && !right_plan.skipped);
    ICVFX_CHECK(left_plan.inner_rect == whole_plan.inner_rect);
    ICVFX_CHECK(right_plan.inner_rect == whole_plan.inner_rect);
    ICVFX_CHECK(left_plan.soft_edge_px == whole_plan.soft_edge_px);

    auto outer_whole = uploaded_texture(
        device, wall.width, wall.height,
        pattern(0U, 0U, wall.width, wall.height, 0U));
    auto outer_left = uploaded_texture(
        device, left_slice.width, left_slice.height,
        pattern(left_slice.x, left_slice.y, left_slice.width,
                left_slice.height, 0U));
    auto outer_right = uploaded_texture(
        device, right_slice.width, right_slice.height,
        pattern(right_slice.x, right_slice.y, right_slice.width,
                right_slice.height, 0U));
    // One inner render, shared: two nodes splitting the rectangle must sample
    // the same image or the join between their halves is itself a seam.
    auto inner = uploaded_texture(
        device, whole_plan.inner_width, whole_plan.inner_height,
        pattern(0U, 0U, whole_plan.inner_width, whole_plan.inner_height,
                97U));

    const auto full =
        composite(device, whole_plan, whole, outer_whole, &inner);
    const auto left =
        composite(device, left_plan, left_slice, outer_left, &inner);
    const auto right =
        composite(device, right_plan, right_slice, outer_right, &inner);

    std::size_t differing = 0U;
    std::size_t inside_inner = 0U;
    std::size_t partial = 0U;
    for (core::u32 y = 0U; y < wall.height; ++y) {
        for (core::u32 x = 0U; x < wall.width; ++x) {
            const auto full_index =
                (static_cast<std::size_t>(y) * wall.width + x) * 4U;
            const bool is_left = x < left_slice.width;
            const auto& part = is_left ? left : right;
            const core::u32 part_x = is_left ? x : x - left_slice.width;
            const auto part_index =
                (static_cast<std::size_t>(y) * left_slice.width + part_x) * 4U;
            for (std::size_t channel = 0U; channel < 4U; ++channel) {
                if (full[full_index + channel] != part[part_index + channel]) {
                    ++differing;
                }
            }
            const float weight = render::icvfx_composite_weight(
                whole_plan, static_cast<float>(x) + 0.5F,
                static_cast<float>(y) + 0.5F);
            if (weight >= 1.0F) {
                ++inside_inner;
            } else if (weight > 0.0F) {
                ++partial;
            }
        }
    }
    ICVFX_CHECK(differing == 0U);
    // The comparison above proves something only if the composite actually
    // did something: a solid plate in the middle and a real ramp around it.
    ICVFX_CHECK(inside_inner > 0U);
    ICVFX_CHECK(partial > 0U);

    // And the composite really is the inner content where the weight is one,
    // and the outer content where it is zero.
    const auto sample = [&](const std::vector<core::u8>& image,
                            core::u32 width, core::u32 x, core::u32 y) {
        const auto index = (static_cast<std::size_t>(y) * width + x) * 4U;
        return std::array<core::u8, 4U>{image[index], image[index + 1U],
                                        image[index + 2U], image[index + 3U]};
    };
    const auto outer_pixels =
        pattern(0U, 0U, wall.width, wall.height, 0U);
    const auto corner = sample(full, wall.width, 4U, 4U);
    const auto corner_expected =
        sample(outer_pixels, wall.width, 4U, 4U);
    ICVFX_CHECK(corner == corner_expected);
    const auto centre = sample(full, wall.width, 64U, 40U);
    ICVFX_CHECK(centre != corner_expected);
}

// A node that skips the inner render must produce its outer content untouched,
// and must do it through the same pass as a node that draws one.
void a_skipped_node_passes_its_outer_render_through_unchanged() {
    auto device = open_device();
    const auto value = tracked_scene();
    // Entirely inside the right node.
    const auto region_value = region(scene::CanvasRect{80U, 20U, 32U, 40U});
    const auto plan =
        render::plan_inner_frustum(
        value, only(value), surface, left_slice, region_value);
    ICVFX_CHECK(plan.active && plan.skipped);

    const auto expected = pattern(left_slice.x, left_slice.y,
                                  left_slice.width, left_slice.height, 0U);
    auto outer = uploaded_texture(
        device, left_slice.width, left_slice.height, expected);
    // Null inner: a skipped node allocates and renders nothing for it.
    const auto composited =
        composite(device, plan, left_slice, outer, nullptr);
    ICVFX_CHECK(composited == expected);

    // A plan that needs an inner render and is handed none is refused and
    // counted, not rendered approximately.
    auto created = render::IcvfxCompositor::create(
        device, left_slice.width, left_slice.height);
    ICVFX_CHECK(created.has_value());
    auto compositor = std::move(created).value();
    auto acquired = device.acquire_commands();
    ICVFX_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();
    const auto needy = render::plan_inner_frustum(
        value, only(value), surface, right_slice, region_value);
    ICVFX_CHECK(needy.active && !needy.skipped);
    ICVFX_CHECK(
        !compositor.render(commands, needy, right_slice, outer, nullptr));
    ICVFX_CHECK(compositor.stats().refusals == 1U);
    ICVFX_CHECK(compositor.stats().composites == 0U);
    auto submitted = device.submit(std::move(commands));
    ICVFX_CHECK(submitted.has_value());
    ICVFX_CHECK(std::move(submitted).value().wait());
}

struct Case final {
    const char* name;
    void (*function)();
    bool needs_gpu{};
};

// Where the plate lands on the wall and what the plate contains are computed
// in two different places - the presenter derives the rectangle, the stage
// renders the image - and they must agree about which camera is the physical
// one. A bound camera with its own lens moves the eye; if only one of the two
// paths saw the binding, the plate would be correct and in the wrong place.
void one_camera_answers_where_the_plate_is_and_what_is_in_it() {
    auto scene_value = tracked_scene();
    scene::Camera bound{};
    bound.eye = scene::Vec3{9.0F, 9.0F, 9.0F};
    bound.near_plane = 0.25F;
    bound.far_plane = 400.0F;
    scene_value.tracked_cameras.front().authored = bound;

    const scene::Camera resolved =
        render::tracked_stage_camera(only(scene_value));
    // The pose owns the placement; the bound camera owns everything a tracker
    // has no opinion about.
    ICVFX_CHECK(resolved.eye
                == scene_value.tracked_cameras.front().pose.position);
    ICVFX_CHECK(resolved.near_plane == bound.near_plane);
    ICVFX_CHECK(resolved.far_plane == bound.far_plane);

    // And it is the same camera the plate is rendered with.
    const render::IcvfxPlan plan = render::plan_inner_frustum(
        scene_value, only(scene_value), surface, left_slice,
        region(scene::CanvasRect{40U, 20U, 48U, 40U}));
    ICVFX_CHECK(plan.active);
    ICVFX_CHECK(plan.camera == tracked_node);
    const auto view =
        render::inner_frustum_view(scene_value, only(scene_value), surface, plan);
    ICVFX_CHECK(view.has_value());
    ICVFX_CHECK(view->eye == resolved.eye);

    // Each camera keeps its own temporal history, so swapping which physical
    // camera is live is the cut it actually is rather than a continuation.
    auto second = scene_value;
    second.tracked_cameras.front().camera = mediaserver::doc::NodeId{12U};
    ICVFX_CHECK(render::inner_frustum_view(second, only(second), surface, plan)
                    ->history_identity
                != view->history_identity);

    // No pose is no override: the authored camera stands exactly where it was
    // authored rather than being moved to the origin.
    auto untracked = tracked_scene();
    untracked.tracked_cameras.front().pose.valid = false;
    ICVFX_CHECK(render::tracked_stage_camera(only(untracked)).eye
                == untracked.tracked_cameras.front().authored.eye);
}

// Two cameras looking at one wall. Both plates are drawn, in the order the
// scene named, and the one with the bigger render order ends up on top -
// which is the whole reason the order is part of the replicated scene.
void two_cameras_put_two_plates_on_the_wall_in_order() {
    auto value = tracked_scene();
    scene::TrackedCameraView second = value.tracked_cameras.front();
    second.camera = mediaserver::doc::NodeId{12U};
    second.icvfx.render_order = 5;
    value.tracked_cameras.push_back(second);

    const auto rect = region(scene::CanvasRect{40U, 20U, 48U, 40U});
    const auto lower = render::plan_inner_frustum(
        value, value.tracked_cameras[0U], surface, left_slice, rect);
    const auto upper = render::plan_inner_frustum(
        value, value.tracked_cameras[1U], surface, left_slice, rect);
    ICVFX_CHECK(lower.active && upper.active);
    ICVFX_CHECK(lower.camera == tracked_node);
    ICVFX_CHECK(upper.camera == mediaserver::doc::NodeId{12U});

    // A camera with its own inner frustum switched off draws nothing, while
    // its neighbour keeps drawing. That is a witness camera, not a fault.
    auto witness = value;
    witness.tracked_cameras[0U].icvfx.inner_frustum = false;
    ICVFX_CHECK(!render::plan_inner_frustum(
                     witness, witness.tracked_cameras[0U], surface, left_slice, rect)
                     .active);
    ICVFX_CHECK(render::plan_inner_frustum(
                    witness, witness.tracked_cameras[1U], surface, left_slice, rect)
                    .active);

    // The show-wide switch takes every plate off the wall at once, whatever
    // each camera asked for.
    auto dark = value;
    dark.renderer_settings.icvfx.inner_frustum = false;
    for (const scene::TrackedCameraView& camera : dark.tracked_cameras) {
        ICVFX_CHECK(
            !render::plan_inner_frustum(dark, camera, surface, left_slice, rect)
                 .active);
    }
}

#if defined(MEDIASERVER_FOUR_LED_WALL_FIXTURE)
constexpr std::size_t real_wall_cases = 1U;
#else
constexpr std::size_t real_wall_cases = 0U;
#endif

constexpr std::array<Case, 15U + real_wall_cases> cases{{
#if defined(MEDIASERVER_FOUR_LED_WALL_FIXTURE)
    {"a real curved wall puts the plate on the patch the camera faces",
     &a_real_curved_wall_puts_the_plate_on_the_patch_the_camera_faces, false},
#endif
    {"a mesh wall plate registers with the wall",
     &a_mesh_wall_plate_registers_with_the_wall, false},
    {"only the wall under the plate constrains its view",
     &only_the_wall_under_the_plate_constrains_its_view, false},
    {"an imported mesh is placed before its plate is found",
     &an_imported_mesh_is_placed_before_its_plate_is_found, false},
    {"one camera answers where the plate is and what is in it",
     &one_camera_answers_where_the_plate_is_and_what_is_in_it},
    {"two cameras put two plates on the wall in order",
     &two_cameras_put_two_plates_on_the_wall_in_order},
    {"an unauthored or untracked scene plans nothing",
     &an_unauthored_or_untracked_scene_plans_nothing, false},
    {"a node whose slice misses the rectangle skips the inner render",
     &a_node_whose_slice_misses_the_rectangle_skips_the_inner_render, false},
    {"the authored resolution scale sizes the inner target",
     &the_authored_resolution_scale_sizes_the_inner_target, false},
    {"the feather never outgrows the rectangle",
     &the_feather_never_outgrows_the_rectangle, false},
    {"the composite weight is a function of canvas position alone",
     &the_composite_weight_is_a_function_of_canvas_position_alone, false},
    {"the inner view history identity is stable under motion",
     &the_inner_view_history_identity_is_stable_under_motion, false},
    {"the plate registers with the wall from a displaced eye",
     &the_plate_registers_with_the_wall_from_a_displaced_eye, false},
    {"a calibrated lens moves the inner view eye to the entrance pupil",
     &a_calibrated_lens_moves_the_inner_view_eye_to_the_entrance_pupil, false},
    {"a wall surface drives the plan end to end",
     &a_wall_surface_drives_the_plan_end_to_end, false},
    {"a skipped node passes its outer render through unchanged",
     &a_skipped_node_passes_its_outer_render_through_unchanged, true},
}};

} // namespace

int main() {
    bool gpu = rhi::Device::available()
        && render::IcvfxCompositor::shaders_available()
        && render::IcvfxStage::shaders_available();
    if (gpu) {
        auto device = open_device();
        const auto probe = render::IcvfxCompositor::create(device, 4U, 4U);
        if (!probe
            && probe.error() == render::RenderError::ShaderFormatUnsupported) {
            gpu = false;
        }
    }
    if (!gpu) {
        std::cout << "[skip] icvfx composite unavailable on this machine; "
                     "running the headless derivation only\n";
    }

    std::size_t failures = 0U;
    std::size_t ran = 0U;
    for (const Case& entry : cases) {
        if (entry.needs_gpu && !gpu) {
            std::cout << "[skip] " << entry.name << '\n';
            continue;
        }
        ++ran;
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    if (gpu) {
        try {
            the_stage_draws_a_plate_only_when_there_is_one_to_draw();
            ++ran;
            std::cout << "[pass] the stage draws a plate only when there is "
                         "one to draw\n";
        } catch (const std::exception& error) {
            ++ran;
            ++failures;
            std::cerr << "[fail] the stage draws a plate only when there is "
                         "one to draw: "
                      << error.what() << '\n';
        }
#if defined(MEDIASERVER_FOUR_LED_WALL_FIXTURE)
        try {
            a_mesh_wall_plate_records_every_frame_on_d3d12();
            ++ran;
            std::cout << "[pass] a mesh wall plate records every frame on "
                         "d3d12\n";
        } catch (const std::exception& error) {
            ++ran;
            ++failures;
            std::cerr << "[fail] a mesh wall plate records every frame on "
                         "d3d12: "
                      << error.what() << '\n';
        }
#endif
        try {
            the_stage_draws_a_plate_on_a_mesh_wall();
            ++ran;
            std::cout << "[pass] the stage draws a plate on a mesh wall\n";
        } catch (const std::exception& error) {
            ++ran;
            ++failures;
            std::cerr << "[fail] the stage draws a plate on a mesh wall: "
                      << error.what() << '\n';
        }
        try {
            a_two_node_composite_is_byte_exact_against_the_whole_canvas();
            ++ran;
            std::cout << "[pass] a two-node composite is byte-exact against "
                         "the whole canvas\n";
        } catch (const std::exception& error) {
            ++ran;
            ++failures;
            std::cerr << "[fail] a two-node composite is byte-exact against "
                         "the whole canvas: "
                      << error.what() << '\n';
        }
    } else {
        std::cout << "[skip] a two-node composite is byte-exact against the "
                     "whole canvas\n";
    }

    std::cout << (ran - failures) << '/' << ran << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
