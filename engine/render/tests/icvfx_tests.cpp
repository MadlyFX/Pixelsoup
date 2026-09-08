#include <mediaserver/render/icvfx.hpp>

#include <mediaserver/render/icvfx_stage.hpp>

#include <mediaserver/output/perspective_mapping.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/scene.hpp>

#include <algorithm>
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

constexpr std::array<Case, 12U> cases{{
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
