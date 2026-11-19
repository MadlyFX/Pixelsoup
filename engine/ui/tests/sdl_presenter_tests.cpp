#include <mediaserver/rhi/device.hpp>
#include <mediaserver/render/compositor_scene_executor.hpp>
#include <mediaserver/render/icvfx_stage.hpp>
#include <mediaserver/render/output_transform.hpp>
#include <mediaserver/render/projection_mesh_warp.hpp>
#include <mediaserver/render/scene_mixer.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/ui/gpu_output_target.hpp>
#include <mediaserver/ui/sdl_presenter.hpp>

#include "../src/pointer_overlay_pass.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <span>
#include <vector>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace {

namespace cmd = mediaserver::cmd;
namespace core = mediaserver::core;
namespace show = mediaserver::show;
namespace rhi = mediaserver::rhi;
namespace render = mediaserver::render;
namespace scene = mediaserver::scene;
namespace output = mediaserver::output;
namespace ui = mediaserver::ui;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define UI_CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

constexpr core::u32 window_width = 128U;
constexpr core::u32 window_height = 96U;

[[nodiscard]] ui::SdlPresenterConfig config(
    std::string_view title = "presenter-test") {
    ui::SdlPresenterConfig created{};
    created.title = std::string{title};
    created.width = window_width;
    created.height = window_height;
    // A test window must not be resizable: a window manager that resized it
    // would scale the readback and make the comparison meaningless.
    created.resizable = false;
    created.accepts_commands = true;
    return created;
}

[[nodiscard]] show::TestPatternInput frame(core::u64 index) {
    show::TestPatternInput input{};
    input.show_frame = show::ShowFrame{index};
    input.control_frame = cmd::FrameNumber{index};
    input.rate = show::FrameRate{50U, 1U};
    input.state = show::TransportState::Playing;
    return input;
}

[[nodiscard]] std::unique_ptr<show::Presenter> open(
    const ui::SdlPresenterConfig& settings) {
    auto created = ui::SdlPresenter::create(settings);
    UI_CHECK(created.has_value());
    return std::move(created).value();
}

// Synthesises a key press addressed at one window. This is how the operator
// input path is exercised without a human at a keyboard.
void press(const show::Presenter& presenter, SDL_Scancode scancode) {
    // A static downcast, not a dynamic one: the engine libraries are built
    // without RTTI, and every presenter this test opens is an SdlPresenter.
    const auto& window = static_cast<const ui::SdlPresenter&>(presenter);

    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.type = SDL_EVENT_KEY_DOWN;
    event.key.windowID = static_cast<SDL_WindowID>(window.window_id());
    event.key.scancode = scancode;
    event.key.down = true;
    event.key.repeat = false;
    UI_CHECK(SDL_PushEvent(&event));
}

void a_malformed_surface_is_refused_before_touching_the_platform() {
    auto zero_width = config();
    zero_width.width = 0U;
    const auto refused = ui::SdlPresenter::create(zero_width);
    UI_CHECK(!refused.has_value());
    UI_CHECK(refused.error() == show::PresenterError::InvalidConfiguration);

    auto incomplete_mode = config();
    incomplete_mode.require_exact_display_mode = true;
    incomplete_mode.fullscreen = true;
    incomplete_mode.position_explicit = true;
    const auto missing_rate = ui::SdlPresenter::create(incomplete_mode);
    UI_CHECK(!missing_rate.has_value());
    UI_CHECK(missing_rate.error() ==
             show::PresenterError::InvalidConfiguration);

    incomplete_mode.refresh_numerator = 60U;
    incomplete_mode.refresh_denominator = 1U;
    incomplete_mode.fullscreen = false;
    const auto not_fullscreen = ui::SdlPresenter::create(incomplete_mode);
    UI_CHECK(!not_fullscreen.has_value());
    UI_CHECK(not_fullscreen.error() ==
             show::PresenterError::InvalidConfiguration);

    auto buffered_preview = config();
    buffered_preview.buffered_media = true;
    const auto no_output_stage = ui::SdlPresenter::create(buffered_preview);
    UI_CHECK(!no_output_stage.has_value());
    UI_CHECK(no_output_stage.error() ==
             show::PresenterError::InvalidConfiguration);
}

void what_reaches_the_display_is_what_was_rasterised() {
    auto presenter = open(config());
    UI_CHECK(presenter->windowed());

    std::vector<core::u8> black(window_width * window_height * 4U, 0U);
    for (std::size_t i = 3U; i < black.size(); i += 4U) black[i] = 255U;
    const show::FrameBufferView expected{
        black, window_width, window_height, window_width * 4U,
        show::PixelFormat::Rgba8888};
    UI_CHECK(presenter->readback().digest == show::frame_buffer_digest(expected));
    UI_CHECK(presenter->hold() == show::PresenterStatus::Skipped);
    UI_CHECK(presenter->readback().digest == show::frame_buffer_digest(expected));

    UI_CHECK(presenter->present(frame(1'234U)) == show::PresenterStatus::Presented);

    // The whole path is checked here, not just the rasteriser: the readback
    // comes from the renderer, so a wrong pixel order, a wrong pitch, or a
    // failed upload all show up as a digest mismatch.
    const auto presented = presenter->readback();
    UI_CHECK(static_cast<bool>(presented));
    UI_CHECK(presented.width == window_width);
    UI_CHECK(presented.height == window_height);
    UI_CHECK(presented.digest == show::frame_buffer_digest(presenter->surface()));
}

void every_frame_reaches_the_display_in_turn() {
    auto presenter = open(config());

    core::u64 previous = 0U;
    for (core::u64 index = 100U; index < 105U; ++index) {
        UI_CHECK(presenter->present(frame(index)) == show::PresenterStatus::Presented);
        const auto presented = presenter->readback();
        UI_CHECK(static_cast<bool>(presented));
        UI_CHECK(
            presented.digest
            == show::frame_buffer_digest(presenter->surface()));
        UI_CHECK(presented.digest != previous);
        previous = presented.digest;
    }
}

void two_windows_present_independently() {
    auto first = open(config("presenter-test-a"));
    auto second = open(config("presenter-test-b"));

    UI_CHECK(first->present(frame(7U)) == show::PresenterStatus::Presented);
    UI_CHECK(second->present(frame(8U)) == show::PresenterStatus::Presented);

    const auto left = first->readback();
    const auto right = second->readback();
    UI_CHECK(static_cast<bool>(left));
    UI_CHECK(static_cast<bool>(right));
    UI_CHECK(left.digest == show::frame_buffer_digest(first->surface()));
    UI_CHECK(right.digest == show::frame_buffer_digest(second->surface()));
    // Different show frames must not collapse to the same picture, which is
    // the property the whole two-node comparison rests on.
    UI_CHECK(left.digest != right.digest);

    // Closing one window must not disturb the other; the event queue is shared
    // and each presenter is addressed by its own window id.
    first.reset();
    UI_CHECK(second->present(frame(9U)) == show::PresenterStatus::Presented);
}

void an_operator_window_reports_transport_keys_in_order() {
    auto presenter = open(config());
    UI_CHECK(presenter->present(frame(1U)) == show::PresenterStatus::Presented);
    UI_CHECK(presenter->take_command() == show::PresenterCommand::None);

    press(*presenter, SDL_SCANCODE_SPACE);
    press(*presenter, SDL_SCANCODE_RIGHT);
    press(*presenter, SDL_SCANCODE_LEFT);
    press(*presenter, SDL_SCANCODE_UP);
    press(*presenter, SDL_SCANCODE_DOWN);
    press(*presenter, SDL_SCANCODE_HOME);
    // The queue is drained by presenting, which is where the shell pumps.
    UI_CHECK(presenter->present(frame(2U)) == show::PresenterStatus::Presented);

    UI_CHECK(presenter->take_command() == show::PresenterCommand::TogglePlay);
    UI_CHECK(presenter->take_command() == show::PresenterCommand::StepForward);
    UI_CHECK(presenter->take_command() == show::PresenterCommand::StepBack);
    UI_CHECK(presenter->take_command() == show::PresenterCommand::JumpForward);
    UI_CHECK(presenter->take_command() == show::PresenterCommand::JumpBack);
    UI_CHECK(presenter->take_command() == show::PresenterCommand::GoToStart);
    UI_CHECK(presenter->take_command() == show::PresenterCommand::None);
}

void a_quit_key_closes_the_output_rather_than_queueing() {
    auto presenter = open(config());
    UI_CHECK(presenter->present(frame(1U)) == show::PresenterStatus::Presented);

    press(*presenter, SDL_SCANCODE_ESCAPE);
    UI_CHECK(presenter->present(frame(2U)) == show::PresenterStatus::Closed);
    UI_CHECK(presenter->take_command() == show::PresenterCommand::None);
    // Closing latches: a later boundary must not resume presenting.
    UI_CHECK(presenter->present(frame(3U)) == show::PresenterStatus::Closed);
}

void an_output_window_queues_no_operator_commands_by_default() {
    auto settings = config();
    settings.accepts_commands = false;
    auto presenter = open(settings);

    press(*presenter, SDL_SCANCODE_SPACE);
    UI_CHECK(presenter->present(frame(1U)) == show::PresenterStatus::Presented);
    UI_CHECK(presenter->take_command() == show::PresenterCommand::None);

    // Quit is not a transport command, so it still closes an input-inert
    // output window.
    press(*presenter, SDL_SCANCODE_ESCAPE);
    UI_CHECK(presenter->present(frame(2U)) == show::PresenterStatus::Closed);
}

void the_video_subsystem_is_released_with_the_last_window() {
    // Repeated open/close cycles must not leak the subsystem reference count,
    // which would eventually refuse a window.
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto presenter = open(config());
        UI_CHECK(presenter->present(frame(42U)) == show::PresenterStatus::Presented);
    }
    auto presenter = open(config());
    UI_CHECK(presenter->present(frame(43U)) == show::PresenterStatus::Presented);
}

struct Case final {
    const char* name;
    void (*function)();
};


// ---------------------------------------------------------------------------
// The GPU output path.
//
// The offscreen target is deliberately separable from the window, so these run
// on a build machine with a GPU and no display. Only the swapchain needs a real
// window, and that is checked separately where one exists.

[[nodiscard]] bool gpu_render_testable() {
    return ui::GpuOutputTarget::shaders_available() && rhi::Device::available();
}

// A real window is required to claim a swapchain, and SDL's dummy video driver
// cannot back one. Asked once, by trying: sniffing the driver name would be a
// guess about which drivers can present, and the answer this needs is simply
// whether this one did.
[[nodiscard]] bool gpu_window_testable() {
    static const bool testable = [] {
        if (!gpu_render_testable()) {
            return false;
        }
        auto settings = config("gpu-probe");
        settings.gpu = true;
        const auto created = ui::SdlPresenter::create(settings);
        return created.has_value();
    }();
    return testable;
}

[[nodiscard]] scene::Scene solid_scene(scene::Vec3 colour) {
    scene::Scene created{};
    created.canvas = scene::Canvas{window_width, window_height};
    created.environment.sky_color = colour;
    created.environment.ground_color = colour;
    return created;
}

void scene_capability_includes_the_transition_compositor() {
    UI_CHECK(
        ui::scene_output_available()
        == (ui::GpuOutputTarget::shaders_available()
            && render::CompositorSceneExecutor::shaders_available()
            && render::SceneMixer::shaders_available()
            && render::OutputTransform::shaders_available()
            && rhi::Device::available()));
    UI_CHECK(
        ui::output_output_available()
        == (ui::GpuOutputTarget::shaders_available()
            && render::OutputTransform::shaders_available()
            && render::ProjectionMeshWarp::shaders_available()
            && render::CompositorSceneExecutor::shaders_available()
            && render::SceneMixer::shaders_available()
            && rhi::Device::available()));
}

void a_scene_presenter_dissolves_completed_scene_targets() {
    if (!gpu_window_testable() || !ui::scene_output_available()) {
        std::cout << "      (skipped: needs scene shaders and a GPU swapchain)\n";
        return;
    }

    auto settings = config("scene-transition");
    settings.gpu = true;
    settings.scene = true;
    auto presenter = open(settings);

    const auto from = solid_scene(scene::Vec3{0.08F, 0.20F, 0.75F});
    const auto to = solid_scene(scene::Vec3{0.90F, 0.12F, 0.18F});
    const auto slice = scene::whole(from.canvas);
    const auto present_mix = [&presenter, &from, &to, slice](float mix) {
        const auto status = presenter->present(from, to, mix, slice);
        UI_CHECK(
            status == show::PresenterStatus::Presented
            || status == show::PresenterStatus::Skipped);
        const auto result = presenter->readback();
        UI_CHECK(static_cast<bool>(result));
        return result.digest;
    };

    const auto first = present_mix(0.0F);
    const auto middle = present_mix(0.5F);
    const auto last = present_mix(1.0F);
    UI_CHECK(first != last);
    UI_CHECK(middle != first);
    UI_CHECK(middle != last);

    // The original entry point still presents directly through the first
    // renderer; introducing transitions must not change its result.
    const auto status = presenter->present(from, slice);
    UI_CHECK(
        status == show::PresenterStatus::Presented
        || status == show::PresenterStatus::Skipped);
    UI_CHECK(presenter->readback().digest == first);
}

void one_output_window_presents_multiple_viewports() {
    if (!gpu_window_testable() || !ui::output_output_available()) {
        std::cout << "      (skipped: needs output shaders and a GPU swapchain)\n";
        return;
    }
    auto settings = config("multi-viewport-output");
    settings.gpu = true;
    settings.scene = true;
    settings.output = true;
    auto presenter = open(settings);

    const auto bright = solid_scene(scene::Vec3{0.7F, 0.25F, 0.08F});
    std::array<output::OutputMap, 2U> maps{};
    maps[0U].node = mediaserver::doc::NodeId{1U};
    maps[0U].source = scene::whole(bright.canvas);
    maps[0U].viewport =
        scene::CanvasRect{0U, 0U, window_width / 3U, window_height};
    maps[1U].node = mediaserver::doc::NodeId{2U};
    maps[1U].source = scene::whole(bright.canvas);
    maps[1U].viewport = scene::CanvasRect{
        window_width - window_width / 3U, 0U, window_width / 3U,
        window_height};

    const auto one = presenter->present(bright, maps[0U]);
    UI_CHECK(one == show::PresenterStatus::Presented ||
             one == show::PresenterStatus::Skipped);
    const core::u64 one_digest = presenter->readback().digest;
    const auto both = presenter->present(
        bright, std::span<const output::OutputMap>{maps});
    UI_CHECK(both == show::PresenterStatus::Presented ||
             both == show::PresenterStatus::Skipped);
    const auto readback = presenter->readback();
    UI_CHECK(static_cast<bool>(readback));
    UI_CHECK(readback.digest != one_digest);
}

// A canvas pixel lands in a slice map's viewport by the slice's own offset
// and scale, the same arithmetic the output transform crops by.
void a_pointer_lands_in_a_slice_by_its_offset_and_scale() {
    scene::Scene frame{};
    frame.canvas = scene::Canvas{1920U, 1080U};
    ui::PointerMapView map{};
    map.source = scene::CanvasRect{960U, 0U, 960U, 1080U};
    map.viewport = scene::CanvasRect{100U, 50U, 480U, 540U};
    const auto at = ui::pointer_to_window(
        frame, map, show::PointerPoint{1200.0F, 540.0F});
    UI_CHECK(at.has_value());
    UI_CHECK(at->x == 100.0F + 120.0F && at->y == 50.0F + 270.0F);
    // Left of the slice: still placed, off its viewport, for the clip.
    const auto left = ui::pointer_to_window(
        frame, map, show::PointerPoint{0.0F, 0.0F});
    UI_CHECK(left.has_value() && left->x < 100.0F);
    // A surface map with no view to project through shows nothing.
    map.projection = output::FeedProjection::PlanarSurface;
    UI_CHECK(!ui::pointer_to_window(frame, map,
                                    show::PointerPoint{1200.0F, 540.0F})
                  .has_value());
}

// The laser pointer reaches the window over every map, and taking it away
// gives back exactly the frame without it.
void the_laser_pointer_is_drawn_over_the_output() {
    if (!gpu_window_testable() || !ui::output_output_available()) {
        std::cout << "      (skipped: needs output shaders and a GPU swapchain)\n";
        return;
    }
    auto settings = config("laser-pointer-output");
    settings.gpu = true;
    settings.scene = true;
    settings.output = true;
    auto presenter = open(settings);

    const auto bright = solid_scene(scene::Vec3{0.1F, 0.3F, 0.6F});
    std::array<output::OutputMap, 2U> maps{};
    maps[0U].node = mediaserver::doc::NodeId{1U};
    maps[0U].source = scene::CanvasRect{0U, 0U, bright.canvas.width / 2U,
                                        bright.canvas.height};
    maps[0U].viewport =
        scene::CanvasRect{0U, 0U, window_width / 2U, window_height};
    maps[1U].node = mediaserver::doc::NodeId{2U};
    maps[1U].source =
        scene::CanvasRect{bright.canvas.width / 2U, 0U,
                          bright.canvas.width / 2U, bright.canvas.height};
    maps[1U].viewport = scene::CanvasRect{window_width / 2U, 0U,
                                          window_width / 2U, window_height};
    const auto present = [&] {
        const auto status = presenter->present(
            bright, std::span<const output::OutputMap>{maps});
        UI_CHECK(status == show::PresenterStatus::Presented ||
                 status == show::PresenterStatus::Skipped);
        const auto readback = presenter->readback();
        UI_CHECK(static_cast<bool>(readback));
        return readback.digest;
    };
    const core::u64 plain = present();

    show::PointerOverlay overlay{};
    overlay.dot = show::PointerPoint{
        static_cast<float>(bright.canvas.width) * 0.25F,
        static_cast<float>(bright.canvas.height) * 0.5F};
    presenter->set_pointer_overlay(&overlay);
    const core::u64 dot_left = present();
    UI_CHECK(dot_left != plain);

    // A line across the seam: drawn into both maps.
    overlay.dot.reset();
    overlay.points = {
        {static_cast<float>(bright.canvas.width) * 0.3F,
         static_cast<float>(bright.canvas.height) * 0.4F},
        {static_cast<float>(bright.canvas.width) * 0.7F,
         static_cast<float>(bright.canvas.height) * 0.6F}};
    overlay.stroke_ends = {2U};
    const core::u64 line = present();
    UI_CHECK(line != plain && line != dot_left);

    // Off the canvas altogether: nothing to see.
    overlay.points.clear();
    overlay.stroke_ends.clear();
    overlay.dot = show::PointerPoint{-50'000.0F, -50'000.0F};
    UI_CHECK(present() == plain);

    // Cleared, and taken away: the frame as it was.
    overlay.dot.reset();
    UI_CHECK(present() == plain);
    presenter->set_pointer_overlay(nullptr);
    UI_CHECK(present() == plain);
}

void buffered_output_defers_a_scene_with_pending_media() {
    if (!gpu_window_testable() || !ui::output_output_available()) {
        std::cout << "      (skipped: needs output shaders and a GPU swapchain)\n";
        return;
    }
    auto settings = config("buffered-media-output");
    settings.gpu = true;
    settings.scene = true;
    settings.output = true;
    settings.buffered_media = true;
    auto presenter = open(settings);

    scene::Scene pending = solid_scene(scene::Vec3{0.2F, 0.3F, 0.4F});
    pending.camera.eye = scene::Vec3{0.0F, 0.0F, 3.0F};
    pending.camera.target = scene::Vec3{};
    pending.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    pending.camera.field_of_view = 0.125F;
    pending.camera.near_plane = 0.1F;
    pending.camera.far_plane = 100.0F;
    scene::MediaInstance clip{};
    clip.mesh = scene::MeshKey{scene::Primitive::Quad, 1U};
    clip.world = scene::scaling(scene::Vec3{3.0F, 3.0F, 1.0F});
    clip.normal = scene::normal_basis(clip.world);
    clip.source_node = mediaserver::doc::NodeId{77U};
    clip.source = (std::filesystem::temp_directory_path() /
                   "mediaserver-pending-buffer-test.mp4").string();
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove(clip.source, ignored));
    pending.media.push_back(std::move(clip));

    output::OutputMap map{};
    map.node = mediaserver::doc::NodeId{71U};
    map.source = scene::whole(pending.canvas);
    map.viewport = scene::whole(pending.canvas);
    UI_CHECK(presenter->present(pending, map) ==
             show::PresenterStatus::Deferred);
    UI_CHECK(presenter->timing().media_deferred_frames == 1U);
}

// The shipping output loop presents through the compositor-graph overload, so
// that is the overload the plate has to reach. It did not for a long time: the
// plate was composited only on the graph-less path, which nothing outside these
// tests calls, and an authored inner frustum therefore never arrived on a wall
// however correctly the rest of it was derived.
//
// Stated as a pixel difference rather than a call count, because that is the
// property that matters: turning the show-wide inner frustum on has to change
// what the output shows.
void the_compositor_graph_path_composites_the_inner_frustum() {
    if (!gpu_window_testable() || !ui::output_output_available() ||
        !render::IcvfxStage::shaders_available()) {
        std::cout << "      (skipped: needs ICVFX and output shaders)\n";
        return;
    }

    auto settings = config("icvfx-graph-path");
    settings.gpu = true;
    settings.scene = true;
    settings.output = true;
    auto presenter = open(settings);

    // A wall the size of the window, square on to the camera, with the tracked
    // camera nearer it than the authored one so the plate is a smaller region.
    output::ResolvedProjectionSurface resolved{};
    resolved.shape = output::ProjectionSurfaceShape::Planar;
    resolved.geometry.center = scene::Vec3{0.0F, 0.0F, 0.0F};
    resolved.geometry.right = scene::Vec3{1.0F, 0.0F, 0.0F};
    resolved.geometry.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    resolved.geometry.width = 8.0F;
    resolved.geometry.height = 6.0F;
    resolved.geometry.raster = scene::Canvas{window_width, window_height};

    output::OutputMap map{};
    map.node = mediaserver::doc::NodeId{91U};
    map.projection = output::FeedProjection::PlanarSurface;
    map.source = scene::CanvasRect{0U, 0U, window_width, window_height};
    map.viewport = scene::CanvasRect{0U, 0U, window_width, window_height};
    map.surface = mediaserver::doc::NodeId{92U};
    map.resolved_surface = resolved;

    auto tracked = solid_scene(scene::Vec3{0.15F, 0.35F, 0.65F});
    tracked.camera.eye = scene::Vec3{0.0F, 0.0F, 6.0F};
    tracked.camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    tracked.camera_source = mediaserver::doc::NodeId{7U};
    // A lit box, so the plate's viewpoint actually changes the picture. A flat
    // sky would composite identically from either eye and the test would pass
    // whether or not the plate was drawn.
    scene::MeshInstance box{};
    box.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    box.world = scene::scaling(scene::Vec3{1.5F, 1.5F, 1.5F});
    box.normal = scene::normal_basis(box.world);
    box.material.base_color = scene::Vec3{0.9F, 0.7F, 0.2F};
    tracked.instances.push_back(box);

    scene::TrackedCameraView view{};
    view.camera = mediaserver::doc::NodeId{31U};
    view.source = mediaserver::doc::NodeId{32U};
    view.authored = tracked.camera;
    view.icvfx.inner_frustum = true;
    view.icvfx.margin_px = 4U;
    view.icvfx.soft_edge_px = 2U;
    view.pose.position = scene::Vec3{1.2F, 0.6F, 2.0F};
    view.pose.field_of_view = 0.08F;
    view.pose.aspect_ratio = 16.0F / 9.0F;
    view.pose.control_frame = 1'200U;
    view.pose.valid = true;
    tracked.tracked_cameras.push_back(view);

    const mediaserver::compositor::CompositorGraph graph{};
    const auto maps = std::span<const output::OutputMap>{&map, 1U};

    auto without = tracked;
    without.renderer_settings.icvfx.inner_frustum = false;
    const auto off_status = presenter->present(without, graph, maps);
    UI_CHECK(off_status == show::PresenterStatus::Presented ||
             off_status == show::PresenterStatus::Skipped);
    const core::u64 outer_only = presenter->readback().digest;

    auto with = tracked;
    with.renderer_settings.icvfx.inner_frustum = true;
    const auto on_status = presenter->present(with, graph, maps);
    UI_CHECK(on_status == show::PresenterStatus::Presented ||
             on_status == show::PresenterStatus::Skipped);
    UI_CHECK(presenter->readback().digest != outer_only);

    // And taking it off again returns the wall to the outer frustum, so the
    // difference above is the plate rather than any first-frame settling.
    const auto again_status = presenter->present(without, graph, maps);
    UI_CHECK(again_status == show::PresenterStatus::Presented ||
             again_status == show::PresenterStatus::Skipped);
    UI_CHECK(presenter->readback().digest == outer_only);
}

void an_unavailable_projection_mesh_blacks_the_output_and_reports_failure() {
    if (!gpu_window_testable() || !ui::output_output_available()) {
        std::cout << "      (skipped: needs output shaders and a GPU swapchain)\n";
        return;
    }

    auto settings = config("projection-mesh-fault");
    settings.gpu = true;
    settings.scene = true;
    settings.output = true;
    auto presenter = open(settings);
    auto& output_presenter = static_cast<ui::SdlPresenter&>(*presenter);
    const auto wait_for_projection_state =
        [&](render::OutputProjectionCacheState expected) {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds{10};
            while (std::chrono::steady_clock::now() < deadline) {
                if (output_presenter.output_projection_mesh_status().state ==
                    expected) {
                    return true;
                }
                std::this_thread::yield();
            }
            return false;
        };

    output::OutputMap canvas_map{};
    canvas_map.node = mediaserver::doc::NodeId{81U};
    canvas_map.source = scene::CanvasRect{0U, 0U, window_width, window_height};
    canvas_map.viewport =
        scene::CanvasRect{0U, 0U, window_width, window_height};

    const auto black_scene = solid_scene(scene::Vec3{});
    const auto black_status = presenter->present(black_scene, canvas_map);
    UI_CHECK(black_status == show::PresenterStatus::Presented ||
             black_status == show::PresenterStatus::Skipped);
    const core::u64 black = presenter->readback().digest;

    const auto bright_scene = solid_scene(scene::Vec3{0.8F, 0.2F, 0.1F});
    const auto bright_status = presenter->present(bright_scene, canvas_map);
    UI_CHECK(bright_status == show::PresenterStatus::Presented ||
             bright_status == show::PresenterStatus::Skipped);
    UI_CHECK(presenter->readback().digest != black);

    std::error_code cleanup_error{};
    const auto missing = std::filesystem::temp_directory_path() /
                         "mediaserver-missing-projection-mesh.obj";
    static_cast<void>(std::filesystem::remove(missing, cleanup_error));

    output::ResolvedProjectionSurface imported{};
    imported.geometry.raster = scene::Canvas{window_width, window_height};
    imported.shape = output::ProjectionSurfaceShape::ImportedMesh;
    imported.mesh_source = missing.string();

    output::OutputMap broken{};
    broken.node = mediaserver::doc::NodeId{92U};
    broken.source = scene::whole(imported.geometry.raster);
    broken.projection = output::FeedProjection::PlanarSurface;
    broken.surface = mediaserver::doc::NodeId{91U};
    broken.resolved_surface = std::move(imported);
    broken.viewport =
        scene::CanvasRect{0U, 0U, window_width, window_height};

    UI_CHECK(presenter->present(bright_scene, broken) ==
             show::PresenterStatus::Failed);
    UI_CHECK(presenter->readback().digest == black);
    UI_CHECK(
        wait_for_projection_state(render::OutputProjectionCacheState::Failed));

    {
        std::ofstream created{missing, std::ios::binary | std::ios::trunc};
        UI_CHECK(static_cast<bool>(created));
        created << "v -0.8 -0.7 0\n"
                   "v 0.8 -0.7 0\n"
                   "v 0 0.8 0\n"
                   "vt 0 1\n"
                   "vt 1 1\n"
                   "vt 0.5 0\n"
                   "f 1/1 2/2 3/3\n";
    }
    // Mere appearance or replacement of bytes is not a frame-path event. The
    // failed generation remains sticky and every mapped frame stays black.
    for (std::size_t attempt = 0U; attempt < 4U; ++attempt) {
        UI_CHECK(presenter->present(bright_scene, broken) ==
                 show::PresenterStatus::Failed);
        UI_CHECK(output_presenter.output_projection_mesh_status().state ==
                 render::OutputProjectionCacheState::Failed);
        UI_CHECK(presenter->readback().digest == black);
    }

    UI_CHECK(output_presenter.refresh_output_projection_mesh());
    UI_CHECK(
        wait_for_projection_state(render::OutputProjectionCacheState::Ready));
    bool recovered = false;
    for (std::size_t attempt = 0U; attempt < 16U && !recovered; ++attempt) {
        const auto status = presenter->present(bright_scene, broken);
        if (status == show::PresenterStatus::Presented ||
            status == show::PresenterStatus::Skipped) {
            recovered = presenter->readback().digest != black;
        }
        std::this_thread::yield();
    }
    UI_CHECK(recovered);
    cleanup_error.clear();
    static_cast<void>(std::filesystem::remove(missing, cleanup_error));
}

[[nodiscard]] std::array<core::u8, 4U> sample(
    const show::FrameBufferView& view,
    core::u32 x,
    core::u32 y) {
    const auto offset = static_cast<std::size_t>(y) * view.stride
        + static_cast<std::size_t>(x) * 4U;
    return {
        view.pixels[offset],
        view.pixels[offset + 1U],
        view.pixels[offset + 2U],
        view.pixels[offset + 3U],
    };
}

// Renders one frame into an offscreen target and hands back the pixels.
[[nodiscard]] std::vector<core::u8> render_gpu_frame(
    rhi::Device& device,
    ui::GpuOutputTarget& output,
    const show::TestPatternInput& input) {
    auto commands = device.acquire_commands();
    UI_CHECK(commands.has_value());
    auto list = std::move(commands).value();
    UI_CHECK(output.record(list, input));

    const auto bytes = static_cast<core::u32>(output.width()) * output.height() * 4U;
    auto readback = device.create_readback(bytes);
    UI_CHECK(readback.has_value());
    UI_CHECK(list.download(output.texture(), readback.value()));

    auto submitted = device.submit(std::move(list));
    UI_CHECK(submitted.has_value());
    UI_CHECK(std::move(submitted).value().wait());

    const auto mapped = readback.value().map();
    UI_CHECK(mapped.size() >= bytes);
    std::vector<core::u8> pixels(mapped.begin(), mapped.begin() + bytes);
    readback.value().unmap();
    return pixels;
}

// The GPU path has to draw the frame the CPU path describes. Comparing whole
// digests would be a promise about rasterisation rules that neither API makes,
// so this compares the landmarks the pattern exists to convey - the tint, the
// border, the inner field - plus the overall agreement rate.
void the_gpu_path_draws_the_frame_the_cpu_path_describes() {
    if (!gpu_render_testable()) {
        std::cout << "      (skipped: no GPU or no shader bytecode)\n";
        return;
    }

    auto device = rhi::Device::create(rhi::DeviceConfig{});
    UI_CHECK(device.has_value());
    auto output = ui::GpuOutputTarget::create(
        device.value(), window_width, window_height);
    UI_CHECK(output.has_value());

    constexpr core::u64 index = 321U;
    const auto pixels =
        render_gpu_frame(device.value(), output.value(), frame(index));

    auto reference = show::PresenterSurface::create(window_width, window_height);
    UI_CHECK(reference.has_value());
    UI_CHECK(show::render_test_pattern(reference.value().view(), frame(index)));

    const show::FrameBufferView rendered{
        std::span<core::u8>{const_cast<core::u8*>(pixels.data()), pixels.size()},
        window_width,
        window_height,
        window_width * 4U,
        show::PixelFormat::Rgba8888,
    };
    const auto& expected = reference.value().view();

    // Flat regions must agree exactly: a wrong channel order, a wrong clip
    // transform, or a missing quad all show up here.
    const auto border = window_height / 64U == 0U ? 4U : window_height / 64U;
    UI_CHECK(
        sample(rendered, window_width / 2U, border / 2U)
        == sample(expected, window_width / 2U, border / 2U));
    UI_CHECK(
        sample(rendered, window_width / 2U, window_height - border / 2U - 1U)
        == sample(expected, window_width / 2U, window_height - border / 2U - 1U));
    // The node tint is behind the border, so an interior point of the inner
    // field proves the field quad landed on top of it.
    UI_CHECK(
        sample(rendered, border + 2U, window_height / 2U)
        == sample(expected, border + 2U, window_height / 2U));

    // And the frame as a whole: text antialiasing rules are the only place the
    // two can legitimately differ, and that is a small minority of pixels.
    std::size_t matching = 0U;
    const auto total = static_cast<std::size_t>(window_width) * window_height;
    for (core::u32 y = 0U; y < window_height; ++y) {
        for (core::u32 x = 0U; x < window_width; ++x) {
            if (sample(rendered, x, y) == sample(expected, x, y)) {
                ++matching;
            }
        }
    }
    UI_CHECK(matching * 100U >= total * 97U);
}

// Two targets given the same frame must produce identical pixels, which is the
// property two nodes showing one wall depend on.
void two_gpu_targets_render_one_frame_identically() {
    if (!gpu_render_testable()) {
        std::cout << "      (skipped: no GPU or no shader bytecode)\n";
        return;
    }

    auto device = rhi::Device::create(rhi::DeviceConfig{});
    UI_CHECK(device.has_value());
    auto left =
        ui::GpuOutputTarget::create(device.value(), window_width, window_height);
    auto right =
        ui::GpuOutputTarget::create(device.value(), window_width, window_height);
    UI_CHECK(left.has_value());
    UI_CHECK(right.has_value());

    const auto input = frame(9'876U);
    const auto first = render_gpu_frame(device.value(), left.value(), input);
    const auto second = render_gpu_frame(device.value(), right.value(), input);
    UI_CHECK(first == second);

    // And a different frame must actually differ, so the comparison above is
    // not passing on two identically blank targets.
    const auto other = render_gpu_frame(device.value(), right.value(), frame(9'877U));
    UI_CHECK(other != first);
}

void the_gpu_path_reports_per_frame_submission_time() {
    if (!gpu_window_testable()) {
        std::cout << "      (skipped: needs a real window for a swapchain)\n";
        return;
    }

    auto settings = config("gpu-timing");
    settings.gpu = true;
    auto presenter = open(settings);
    UI_CHECK(presenter->timing().gpu);
    UI_CHECK(presenter->timing().frames == 0U);

    for (core::u64 index = 0U; index < 8U; ++index) {
        const auto status = presenter->present(frame(index));
        UI_CHECK(
            status == show::PresenterStatus::Presented
            || status == show::PresenterStatus::Skipped);
    }

    const auto timing = presenter->timing();
    UI_CHECK(timing.gpu);
    UI_CHECK(timing.frames == 8U);
    // Every frame after the first retires the one before it, so by now a real
    // submission has been measured.
    UI_CHECK(timing.last_submit_to_observed_ns > 0U);
    UI_CHECK(
        timing.peak_submit_to_observed_ns >= timing.last_submit_to_observed_ns);
    // The exact number: time actually spent inside present().
    UI_CHECK(timing.last_present_ns > 0U);
    UI_CHECK(timing.peak_present_ns >= timing.last_present_ns);

    const auto presented = presenter->readback();
    UI_CHECK(static_cast<bool>(presented));
    UI_CHECK(presented.digest != 0U);
}

// The point of the whole slice: the frame path stops uploading pixels. The
// instance count does not depend on resolution, so the gap widens with it.
void the_gpu_frame_path_uploads_instances_rather_than_pixels() {
    std::array<show::PatternQuad, show::max_pattern_quads> quads{};
    const auto small =
        show::build_pattern_quads(frame(4'321U), 640U, 360U, quads);
    const auto large =
        show::build_pattern_quads(frame(4'321U), 3'840U, 2'160U, quads);

    UI_CHECK(small.count > 0U);
    UI_CHECK(!small.truncated);
    UI_CHECK(!large.truncated);

    const auto instance_bytes = large.count * sizeof(show::PatternQuad);
    const auto surface_bytes = static_cast<std::size_t>(3'840U) * 2'160U * 4U;
    // Three orders of magnitude at 4K, and the CPU path would send the whole
    // surface every frame per output.
    UI_CHECK(instance_bytes * 1'000U < surface_bytes);
}

void a_gpu_request_is_refused_rather_than_downgraded() {
    // The refusal only has a meaning where the GPU path cannot be provided,
    // which is exactly the headless lane: no swapchain, so no windowed output.
    if (gpu_window_testable()) {
        return;
    }
    auto settings = config("gpu-refused");
    settings.gpu = true;
    const auto refused = ui::SdlPresenter::create(settings);
    UI_CHECK(!refused.has_value());
    UI_CHECK(refused.error() == show::PresenterError::DisplayUnavailable);
}

constexpr std::array<Case, 21U> cases{{
    {"malformed surface refused",
     &a_malformed_surface_is_refused_before_touching_the_platform},
    {"display shows what was rasterised",
     &what_reaches_the_display_is_what_was_rasterised},
    {"each frame reaches the display", &every_frame_reaches_the_display_in_turn},
    {"two windows are independent", &two_windows_present_independently},
    {"operator keys in order",
     &an_operator_window_reports_transport_keys_in_order},
    {"quit closes the output", &a_quit_key_closes_the_output_rather_than_queueing},
    {"output windows ignore transport keys",
     &an_output_window_queues_no_operator_commands_by_default},
    {"video subsystem is released",
     &the_video_subsystem_is_released_with_the_last_window},
    {"the GPU path draws the frame the CPU path describes",
     &the_gpu_path_draws_the_frame_the_cpu_path_describes},
    {"two GPU targets render one frame identically",
     &two_gpu_targets_render_one_frame_identically},
    {"scene capability includes the transition compositor",
     &scene_capability_includes_the_transition_compositor},
    {"scene presenters dissolve completed scene targets",
     &a_scene_presenter_dissolves_completed_scene_targets},
    {"one output window presents multiple viewports",
     &one_output_window_presents_multiple_viewports},
    {"a pointer lands in a slice by its offset and scale",
     &a_pointer_lands_in_a_slice_by_its_offset_and_scale},
    {"the laser pointer is drawn over the output",
     &the_laser_pointer_is_drawn_over_the_output},
    {"buffered output defers a scene with pending media",
     &buffered_output_defers_a_scene_with_pending_media},
    {"the compositor graph path composites the inner frustum",
     &the_compositor_graph_path_composites_the_inner_frustum},
    {"unavailable projection meshes black the output",
     &an_unavailable_projection_mesh_blacks_the_output_and_reports_failure},
    {"the GPU path reports per-frame submission time",
     &the_gpu_path_reports_per_frame_submission_time},
    {"the GPU frame path uploads instances rather than pixels",
     &the_gpu_frame_path_uploads_instances_rather_than_pixels},
    {"a GPU request is refused rather than downgraded",
     &a_gpu_request_is_refused_rather_than_downgraded},
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
