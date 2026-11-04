#include <mediaserver/ui/sdl_presenter.hpp>

#include <mediaserver/core/idle_report.hpp>
#include <mediaserver/render/compositor_scene_executor.hpp>
#include <mediaserver/render/icvfx.hpp>
#include <mediaserver/render/icvfx_stage.hpp>
#include <mediaserver/render/mapped_scene.hpp>
#include <mediaserver/render/output_transform.hpp>
#include <mediaserver/render/projection_mesh_warp.hpp>
#include <mediaserver/render/scene_mixer.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/ui/gpu_output_target.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

namespace mediaserver::ui {

struct SdlPresenter::Platform final {
    SDL_Window *window{};
    // The CPU path owns a renderer and a streaming texture. The GPU path
    // owns a device, a swapchain, and one offscreen target; the two are
    // mutually exclusive because both would claim the same window surface.
    SDL_Renderer *renderer{};
    SDL_Texture *texture{};
    SDL_WindowID window_id{};

    std::optional<rhi::Device> device;
    rhi::Swapchain swapchain;
    std::optional<GpuOutputTarget> output;
    // Present only when the 3D path was asked for. It owns a depth buffer and
    // the mesh arenas, so a node driving a 2D show does not pay for it.
    std::optional<render::CompositorSceneExecutor> scene_from;
    std::optional<render::CompositorSceneExecutor> scene_to;
    // A transition is a mix of two completed scene targets, never a blend of
    // their elements. Constructed with the scene path so the per-frame seam
    // has no conditional allocation and every scene presenter advertises the
    // same capability.
    std::optional<render::SceneMixer> scene_mixer;
    // The inner frustum: a second scene render at the tracked pose, composited
    // over the wall image before it reaches the display transform. Constructed
    // with the scene path so a tracked show never allocates one mid-frame; its
    // own inner render target is allocated lazily, the first time a plate is
    // actually drawn.
    std::optional<render::IcvfxStage> icvfx_from;
    std::optional<render::IcvfxStage> icvfx_to;
    // Present only for the output path: the feed-map crop and soft-edge blend
    // applied to the scene renderer's target on the way to the window.
    std::optional<render::OutputTransform> output_transform;
    std::optional<render::ProjectionMeshWarp> mesh_warp_from;
    std::optional<render::ProjectionMeshWarp> mesh_warp_to;
    // Imported projection geometry is published by bounded background work,
    // never read or statted by a frame. Program/Rehearsal share the last ready
    // immutable CPU mesh while retaining separate GPU warp targets. Failure is
    // sticky until the operator explicitly refreshes it or authored mapping
    // identity changes; no timer or frame-count work retries it.
    render::OutputProjectionCacheRegistry projection_meshes;
    // Borrowed from one of the targets above. Updated only after a frame has
    // been recorded successfully, so readback always inspects the texture the
    // swapchain was actually given (single scene, mixed scene, mapped output,
    // or test pattern).
    const rhi::Texture *last_presented{};
    rhi::SubmittedWork pending;
    core::FrameTiming frame_timing{};
    core::u64 pending_record_ns{}, pending_profile_digest{};
    core::u64 recording_sequence{};
    core::AssetPublicationGate asset_gate;
    core::AssetSetReadiness assets{};
    core::u64 uploaded_asset_digest{};
    core::u64 pending_asset_digest{};
    std::optional<rhi::Texture> visible_program;
    bool visible_program_valid{};
};

namespace {

namespace rhi = mediaserver::rhi;

// SDL owns one process-wide event queue and one video subsystem, so the
// lifetime of both is reference counted across presenters and every poll is
// dispatched by window id. Without that, a second window would silently steal
// the first window's close event.
std::mutex &registry_mutex() noexcept {
    static std::mutex mutex;
    return mutex;
}

std::vector<SdlPresenter *> &registry() {
    static std::vector<SdlPresenter *> presenters;
    return presenters;
}

std::size_t &video_users() noexcept {
    static std::size_t users = 0U;
    return users;
}

[[nodiscard]] bool acquire_video() {
    const std::lock_guard lock{registry_mutex()};
    if (video_users() == 0U && !SDL_Init(SDL_INIT_VIDEO)) {
        return false;
    }
    ++video_users();
    return true;
}

void release_video() noexcept {
    const std::lock_guard lock{registry_mutex()};
    if (video_users() == 0U) {
        return;
    }
    --video_users();
    if (video_users() == 0U) {
        SDL_Quit();
    }
}

[[nodiscard]] SDL_WindowFlags
window_flags(const SdlPresenterConfig &config) noexcept {
    // Shader/resource creation can take seconds. Keep the unpainted native
    // client area off the wall until a black image has been submitted.
    SDL_WindowFlags flags = SDL_WINDOW_HIDDEN;
    // With explicit placement, create a desktop window first so SDL can move
    // it onto the requested display before entering fullscreen. Creating it
    // fullscreen here lets the platform select the primary monitor first and
    // later SetWindowPosition calls may have no effect.
    if (config.fullscreen && !config.position_explicit) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }
    if (config.borderless) {
        flags |= SDL_WINDOW_BORDERLESS;
    }
    if (config.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    return flags;
}

[[nodiscard]] show::PresenterCommand
command_for(SDL_Scancode scancode) noexcept {
    switch (scancode) {
    case SDL_SCANCODE_SPACE:
        return show::PresenterCommand::TogglePlay;
    case SDL_SCANCODE_LEFT:
        return show::PresenterCommand::StepBack;
    case SDL_SCANCODE_RIGHT:
        return show::PresenterCommand::StepForward;
    case SDL_SCANCODE_DOWN:
        return show::PresenterCommand::JumpBack;
    case SDL_SCANCODE_UP:
        return show::PresenterCommand::JumpForward;
    case SDL_SCANCODE_HOME:
        return show::PresenterCommand::GoToStart;
    case SDL_SCANCODE_ESCAPE:
    case SDL_SCANCODE_Q:
        return show::PresenterCommand::Quit;
    default:
        break;
    }
    return show::PresenterCommand::None;
}

// Reads any colour target back and digests it. The same start-up self-check
// GpuOutputTarget::readback performs, but against a texture this file does not
// own - the scene renderer's target - so it lives here rather than being
// duplicated onto the renderer's interface. Not a frame path: it submits and
// blocks.
[[nodiscard]] show::PresenterReadback
digest_target(rhi::Device &device, const rhi::Texture &target) noexcept {
    const auto width = target.width();
    const auto height = target.height();
    const auto bytes = static_cast<core::u32>(width) * height * 4U;

    auto buffer = device.create_readback(bytes);
    if (!buffer) {
        return {};
    }
    auto acquired = device.acquire_commands();
    if (!acquired) {
        return {};
    }
    auto commands = std::move(acquired).value();
    if (!commands.download(target, buffer.value())) {
        return {};
    }
    auto submitted = device.submit(std::move(commands));
    if (!submitted || !std::move(submitted).value().wait()) {
        return {};
    }

    const auto pixels = buffer.value().map();
    if (pixels.size() < bytes) {
        return {};
    }
    const show::FrameBufferView view{
        std::span<core::u8>{const_cast<core::u8 *>(pixels.data()),
                            pixels.size()},
        width,
        height,
        width * 4U,
        show::PixelFormat::Rgba8888,
    };
    const show::PresenterReadback report{width, height,
                                         show::frame_buffer_digest(view)};
    buffer.value().unmap();
    return report;
}

[[nodiscard]] const rhi::Texture *
record_scene(render::CompositorSceneExecutor &executor,
             rhi::CommandList &commands, const scene::Scene &frame,
             scene::CanvasRect slice) noexcept {
    auto rendered = executor.execute(commands, frame, slice);
    return rendered ? rendered.value() : nullptr;
}

// The plate for one mapped output, composited over the wall image.
//
// Only planar surfaces. The inner frustum is a region of a *wall*, and a
// planar surface is the one projection policy that hands back an image still
// in surface-raster coordinates - a mesh-warped output has already been
// resampled into connector space by the time it gets here, and outlining a
// plate on it would need the complete surface mesh rather than the region this
// node owns. A curved wall therefore shows the outer frustum until that path
// exists, which is honest; drawing an approximately-placed plate would not be.
//
// Returns `scene_target` untouched whenever there is nothing to draw, so an
// untracked show and a node whose slice misses the plate both cost nothing.
[[nodiscard]] const rhi::Texture *
record_inner_frustum(std::optional<render::IcvfxStage> &stage,
                     rhi::CommandList &commands,
                     const scene::Scene &frame, const output::OutputMap &map,
                     const rhi::Texture *scene_target) noexcept {
    // Why this output is drawing no plate, said once per reason. See
    // core/idle_report.hpp: every condition below is a legitimate reason to
    // draw nothing, and from the wall they are all the same black rectangle.
    const auto once = [](const char *reason) noexcept {
        static_cast<void>(core::report_idle_once(
            "inner frustum: no plate on this output", reason));
    };
    if (scene_target == nullptr) {
        return scene_target;
    }
    if (!stage.has_value()) {
        once("the ICVFX stage was not created");
        return scene_target;
    }
    if (!frame.renderer_settings.icvfx.inner_frustum) {
        once("the show-wide inner frustum switch is off");
        return scene_target;
    }
    if (frame.tracked_cameras.empty()) {
        once("this scene has no tracked camera");
        return scene_target;
    }
    if (!map.perspective_ready()) {
        once("this output is not mapped to a planar LED surface");
        return scene_target;
    }
    if (map.resolved_surface->stage.anchor == output::StageAnchor::Camera) {
        // A tracked plate is framed on a wall that stands still in the world.
        // A stage carried by the scene camera has no such wall to frame it on.
        once("the LED stage follows the scene camera, so there is no fixed "
             "wall for a tracked plate");
        return scene_target;
    }
    // One region per tracked camera, in the scene's composite order, so the
    // stage draws the stack the same way round the scene named it.
    std::array<scene::InnerFrustumRegion, scene::max_tracked_cameras> regions{};
    std::size_t count = 0U;
    bool any = false;
    for (const scene::TrackedCameraView &view : frame.tracked_cameras) {
        if (count >= regions.size()) {
            break;
        }
        if (view.drawing()) {
            // The same eye the plate is rendered from - see
            // `tracked_stage_camera`.
            const scene::Camera tracked = render::tracked_stage_camera(view);
            // Sized by the camera's own calibrated lens when it has one.
            //
            // The plate rectangle used to come from a field of view the
            // Director derived from the *feed's* single sensor height and
            // fallback focal length - a one-point lens that does not follow
            // the zoom encoder at all - while the camera carried a calibrated
            // profile nobody sized anything with. Two lenses deciding one
            // plate is how a background slides against the set piece it is
            // meant to be locked to during a push-in.
            //
            // The lens-aware form also widens the rectangle to the distorted
            // field, which is what stops a wide lens photographing outer
            // frustum at the corners of its own plate. The render stays
            // undistorted; only this coverage is widened.
            const auto region = view.authored.lens_profile.has_value()
                ? output::inner_frustum_region(
                      map.resolved_surface->geometry, tracked,
                      *view.authored.lens_profile, view.encoders,
                      scene::clamped(view.icvfx).margin_px)
                : output::inner_frustum_region(
                      map.resolved_surface->geometry, tracked,
                      view.pose.aspect_ratio,
                      scene::clamped(view.icvfx).margin_px);
            // Both the rectangle above and the view the stage builds from it
            // are in this surface's raster, which is the space `map.source`
            // is already in.
            if (region) {
                regions[count] = region.value();
                any = any || region.value().valid;
            }
        }
        ++count;
    }
    if (!any) {
        // Past every authored gate and still nothing to draw. The two ways
        // that happens are worth telling apart, because one is a wire problem
        // and the other is a geometry problem, and from the wall they look the
        // same.
        bool drawing_any = false;
        bool pose_any = false;
        for (const scene::TrackedCameraView &view : frame.tracked_cameras) {
            drawing_any = drawing_any || view.drawing();
            pose_any = pose_any || view.pose.valid;
        }
        once(drawing_any
                 ? "the tracked frustum meets no part of this wall"
                 : (pose_any ? "the camera is not asking for a plate"
                             : "this node has no pose for its tracked camera"));
        return scene_target;
    }
    return stage->record(
        commands, frame, map.resolved_surface->geometry, map.source,
        std::span<const scene::InnerFrustumRegion>{regions.data(), count},
        *scene_target);
}

void include_media_readiness(const render::CompositorSceneExecutor &executor,
                             bool &complete, bool buffered_media) noexcept {
    complete = complete && (!buffered_media || executor.media_cache_stats().pending_requests == 0U)
        && executor.renderer_passes().geometry.texture_streaming.state == render::TextureStreamState::Ready
        && executor.renderer_passes().geometry.baked_ready;
}

// The plate's own media residency, which is a second scene render and can be
// waiting on content the outer frustum already has. Nothing to report until a
// plate has actually been drawn, because the inner renderer does not exist
// until then.
void include_icvfx_readiness(const std::optional<render::IcvfxStage> &stage,
                             bool &complete, bool buffered_media) noexcept {
    if (stage.has_value() && stage->inner() != nullptr) {
        include_media_readiness(*stage->inner(), complete, buffered_media);
    }
}

void include_icvfx_plates(const std::optional<render::IcvfxStage> &stage,
                          show::PresenterPlateStats &into) noexcept {
    if (!stage.has_value()) {
        return;
    }
    const render::IcvfxStats stats = stage->stats();
    into.drawn += stats.inner_frames_rendered;
    into.skipped += stats.inner_frames_skipped;
    into.refusals += stats.refusals;
}

[[nodiscard]] bool valid_map_identities(
    std::span<const output::OutputMap> maps) noexcept {
    if (maps.empty() || maps.size() > output::max_output_maps) {
        return false;
    }
    for (std::size_t index = 0U; index < maps.size(); ++index) {
        if (!maps[index].node) {
            return false;
        }
        for (std::size_t prior = 0U; prior < index; ++prior) {
            if (maps[prior].node == maps[index].node) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] scene::CanvasRect
whole_output(const render::OutputTransform &transform) noexcept {
    return scene::CanvasRect{0U, 0U, transform.width(), transform.height()};
}

[[nodiscard]] const rhi::Texture *
record_display_transform(render::OutputTransform &transform,
                         rhi::CommandList &commands, const rhi::Texture *source,
                         bool scene_linear, const scene::Scene &frame,
                         scene::CanvasRect region) noexcept {
    // The project's authored colour, taken once per frame before the draw so
    // a table can never change between two viewports of one window. `region`
    // is the part of the wall this window shows, which keys the dither to the
    // wall pixel rather than to this node's own raster.
    transform.adopt_color(frame.renderer_settings.color, scene::asset_generation_key(frame));
    if (source == nullptr ||
        !transform.render(commands, *source, whole_output(transform),
                          scene_linear, region)) {
        return nullptr;
    }
    return &transform.target();
}

[[nodiscard]] const rhi::Texture *
record_scene(render::CompositorSceneExecutor &executor,
             rhi::CommandList &commands,
             const compositor::CompositorGraph &graph,
             const scene::Scene &frame, scene::CanvasRect slice) noexcept;

[[nodiscard]] const rhi::Texture *
record_mapping_fault(rhi::CommandList &commands,
                     render::OutputTransform &output_transform) noexcept {
    // A missing or invalid calibration must not leave the connector showing
    // its previous successful frame. Submit an explicit black frame while the
    // public present() result remains Failed for health monitoring.
    rhi::Texture &target = output_transform.target();
    return commands.clear(target, rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}) ? &target
                                                                      : nullptr;
}

[[nodiscard]] const rhi::Texture *
record_scene(render::CompositorSceneExecutor &executor,
             rhi::CommandList &commands,
             const compositor::CompositorGraph &graph,
             const scene::Scene &frame, scene::CanvasRect slice) noexcept {
    // An incomplete graph stays editable without replacing Program. An
    // executable Output overrides only its reachable SceneLayers by default;
    // unrelated scene drawables pass through unless Output explicitly opts
    // into full-scene replacement.
    if (!graph.valid() || graph.plan.steps.empty()) {
        return record_scene(executor, commands, frame, slice);
    }
    const auto planned = render::plan_compositor_scene(graph, frame);
    if (!planned) {
        // A project-wide graph can legitimately refer to layers that belong to
        // the other scene in Program/Rehearsal. Such a graph is simply not the
        // compositor for this scene. Keep that scene visible through its
        // intrinsic layer/effect stack; structural graph errors still fail
        // loudly instead of rendering misleading pixels.
        if (planned.error() ==
            render::CompositorScenePlanError::MissingSceneLayer) {
            return record_scene(executor, commands, frame, slice);
        }
        return nullptr;
    }
    auto rendered = executor.execute(commands, graph, frame, slice);
    return rendered ? rendered.value() : nullptr;
}

} // namespace

SdlPresenter::SdlPresenter(show::PresenterSurface surface, Platform *platform,
                           bool accepts_commands, bool gpu, bool scene,
                           bool output, bool buffered_media) noexcept
    : surface_(std::move(surface)), platform_(platform),
      accepts_commands_(accepts_commands), gpu_(gpu), scene_(scene),
      output_(output), buffered_media_(buffered_media) {
    timing_.gpu = gpu;
}

bool gpu_output_available() noexcept {
    return GpuOutputTarget::shaders_available() && rhi::Device::available();
}

bool scene_output_available() noexcept {
    // The 3D path carries its own bytecode, so it can be absent while the 2D
    // one is present if the shader build was only half applied. Both questions
    // also need a device, which is what makes this more than a build check.
    return GpuOutputTarget::shaders_available() &&
           render::CompositorSceneExecutor::shaders_available() &&
           render::SceneMixer::shaders_available() &&
           render::OutputTransform::shaders_available() &&
           rhi::Device::available();
}

bool output_output_available() noexcept {
    // The output stage needs its own bytecode and a source to transform, so it
    // needs the scene path too. All of it also needs a device.
    return GpuOutputTarget::shaders_available() &&
           render::OutputTransform::shaders_available() &&
           render::ProjectionMeshWarp::shaders_available() &&
           render::CompositorSceneExecutor::shaders_available() &&
           render::SceneMixer::shaders_available() && rhi::Device::available();
}

// Everything the GPU path needs, built once. Returns false and leaves the
// platform block releasable on any refusal, so create() can fall through to
// its single cleanup path.
bool SdlPresenter::build_gpu_resources(
    Platform &platform, const SdlPresenterConfig &config) noexcept {
    if (!GpuOutputTarget::shaders_available()) {
        return false;
    }

    const auto open_backend = [&](rhi::BackendKind backend) {
        rhi::DeviceConfig device_config{};
        device_config.debug = false;
        device_config.backend = backend;
        auto created = rhi::Device::create(device_config);
        if (!created) {
            return false;
        }
        // Claim only after the Device has reached its final address. RHI
        // resources retain their owning Device pointer; moving the owner after
        // claim would leave the swapchain consulting a moved-from object.
        platform.device.emplace(std::move(created).value());
        const auto format = config.output_bit_depth == 10U ? rhi::TextureFormat::Rgb10A2Unorm
            : config.output_bit_depth == 16U ? rhi::TextureFormat::Rgba16Float : rhi::TextureFormat::Rgba8Unorm;
        auto claimed = platform.device->claim_window(platform.window, rhi::PresentMode::Immediate, format);
        if (!claimed) {
            platform.device.reset();
            return false;
        }
        platform.swapchain = std::move(claimed).value();
        return true;
    };
    // Scene/output windows prefer the native device that can honor Advanced.
    // A Basic-only machine remains supported by the SDL backend.
    if (config.scene && open_backend(rhi::BackendKind::D3d12)) {
        // Selected above.
    } else if (!open_backend(rhi::BackendKind::SdlGpu)) {
        return false;
    }

    auto output =
        GpuOutputTarget::create(*platform.device, config.width, config.height);
    if (!output) {
        return false;
    }
    platform.output.emplace(std::move(output).value());

    const rhi::TextureFormat working_format =
        platform.device->capabilities().backend == rhi::BackendKind::D3d12
            ? rhi::TextureFormat::Rgba16Float
            : rhi::TextureFormat::Rgba8Unorm;
    if (config.scene) {
        auto visible = platform.device->create_texture(rhi::TextureDesc{
            config.width, config.height, platform.swapchain.format(), true, true});
        if (!visible) return false;
        platform.visible_program.emplace(std::move(visible).value());
        const std::size_t decoded_cache_bytes =
            static_cast<std::size_t>(config.media_cache_mebibytes) * 1'024U *
            1'024U;
        const render::MediaCacheConfig media_cache =
            config.buffered_media
                ? render::MediaCacheConfig::buffered_output(
                      config.media_read_ahead_frames, decoded_cache_bytes)
                : render::MediaCacheConfig{};
        if (config.buffered_media) {
            render::configure_buffered_output_media_decode(
                render::MediaDecodeConfig::buffered_output(
                    config.media_read_ahead_frames, decoded_cache_bytes,
                    config.media_trail_frames));
        }
        auto from_renderer = render::CompositorSceneExecutor::create(
            *platform.device, config.width, config.height,
            render::RendererLimits{}, media_cache, working_format);
        if (!from_renderer) {
            return false;
        }
        platform.scene_from.emplace(std::move(from_renderer).value());

        auto to_renderer = render::CompositorSceneExecutor::create(
            *platform.device, config.width, config.height,
            render::RendererLimits{}, media_cache, working_format);
        if (!to_renderer) {
            return false;
        }
        platform.scene_to.emplace(std::move(to_renderer).value());

        auto mixer = render::SceneMixer::create(*platform.device, config.width,
                                                config.height, working_format);
        if (!mixer) {
            return false;
        }
        platform.scene_mixer.emplace(std::move(mixer).value());

        // An ICVFX-capable presenter is not a requirement: a show with no
        // tracked camera never asks this stage for anything, and a build with
        // no composite shader simply presents the outer frustum alone rather
        // than refusing to start.
        //
        // Two of them, for the same reason there are two scene executors and
        // two mesh warps. A transition cross-dissolves two complete wall
        // images, and each of those images needs its own plate composited into
        // it before the mix - the outgoing scene's plate over the outgoing
        // wall, the incoming scene's over the incoming one. Sharing one stage
        // would have the second call overwrite the compositor target the first
        // result still points at. The second stage costs only its two
        // compositor targets up front; its inner renderer is built on first
        // use, so a show that never transitions with a plate never pays for it.
        // Independently, and in that order. The two serve different frames, so
        // a second stage this device could not afford must cost the show its
        // transition plates and nothing else - dropping the first one with it
        // would take every plate off every wall to protect a cross-dissolve.
        if (!render::IcvfxStage::shaders_available()) {
            std::cerr << "inner frustum unavailable: no composite shader in "
                         "this build\n";
        } else {
            auto icvfx_from = render::IcvfxStage::create(
                *platform.device, config.width, config.height,
                render::RendererLimits{}, media_cache, working_format);
            if (icvfx_from) {
                platform.icvfx_from.emplace(std::move(icvfx_from).value());
            } else {
                // A node that cannot plate has to say so. Silently presenting
                // the outer frustum alone looks exactly like a show with no
                // tracking, and on a stage that is the wrong thing to believe.
                std::cerr << "inner frustum unavailable: stage refused ("
                          << static_cast<int>(icvfx_from.error()) << ")\n";
            }
            auto icvfx_to = render::IcvfxStage::create(
                *platform.device, config.width, config.height,
                render::RendererLimits{}, media_cache, working_format);
            if (icvfx_to) {
                platform.icvfx_to.emplace(std::move(icvfx_to).value());
            } else {
                std::cerr << "inner frustum: transition plates unavailable ("
                          << static_cast<int>(icvfx_to.error()) << ")\n";
            }
        }
    }

    if (config.output) {
        auto from_warp = render::ProjectionMeshWarp::create(
            *platform.device, config.width, config.height,
            render::ProjectionMeshWarpLimits{}, working_format);
        if (!from_warp) {
            return false;
        }
        platform.mesh_warp_from.emplace(std::move(from_warp).value());

        auto to_warp = render::ProjectionMeshWarp::create(
            *platform.device, config.width, config.height,
            render::ProjectionMeshWarpLimits{}, working_format);
        if (!to_warp) {
            return false;
        }
        platform.mesh_warp_to.emplace(std::move(to_warp).value());
    }
    // Every scene reaches an RGBA8 display boundary through this transform.
    // Advanced remains scene-linear RGBA16F through compositing, transition
    // mixing, and projection warp; its transfer function is applied exactly
    // once here. Basic is copied without an additional transfer conversion.
    if (config.scene) {
        auto transform = render::OutputTransform::create(
            *platform.device, config.width, config.height, false, platform.swapchain.format());
        if (!transform) {
            return false;
        }
        platform.output_transform.emplace(std::move(transform).value());
    }
    return true;
}

namespace {

// The display whose desktop bounds are exactly the ones the authored binding
// named. Exact because this is an identity check, not a search for something
// close: a display that merely contains the origin is the failure this
// replaces. Zero when the caller named no display, and zero when it named one
// this machine is not presenting - the caller distinguishes those by whether
// it supplied an extent.
[[nodiscard]] SDL_DisplayID display_for_bounds(
    core::i32 x, core::i32 y, core::u32 width, core::u32 height) noexcept {
    if (width == 0U || height == 0U) {
        return 0U;
    }
    int count{};
    SDL_DisplayID *const displays = SDL_GetDisplays(&count);
    SDL_DisplayID found = 0U;
    for (int index = 0; displays != nullptr && index < count; ++index) {
        SDL_Rect bounds{};
        if (!SDL_GetDisplayBounds(displays[index], &bounds)) {
            continue;
        }
        if (bounds.x == static_cast<int>(x) &&
            bounds.y == static_cast<int>(y) &&
            bounds.w == static_cast<int>(width) &&
            bounds.h == static_cast<int>(height)) {
            found = displays[index];
            break;
        }
    }
    SDL_free(displays);
    return found;
}

} // namespace

show::PresenterResult
SdlPresenter::create(const SdlPresenterConfig &config) noexcept {
    using Result = show::PresenterResult;

    auto surface = show::PresenterSurface::create(config.width, config.height);
    if (!surface) {
        return Result::failure(surface.error());
    }
    if ((config.output_bit_depth != 8U && config.output_bit_depth != 10U && config.output_bit_depth != 16U)
        || (config.output_bit_depth != 8U && !config.scene)) return Result::failure(show::PresenterError::InvalidConfiguration);
    if (config.require_exact_display_mode &&
        (!config.fullscreen || !config.position_explicit ||
         config.refresh_numerator == 0U || config.refresh_denominator == 0U)) {
        return Result::failure(show::PresenterError::InvalidConfiguration);
    }
    if (!acquire_video()) {
        return Result::failure(show::PresenterError::DisplayUnavailable);
    }

    auto *platform = new (std::nothrow) Platform{};
    if (platform == nullptr) {
        release_video();
        return Result::failure(show::PresenterError::AllocationFailed);
    }

    const auto fail = [&platform](show::PresenterError error) {
        // GPU resources borrow the device and the window, so they are released
        // before either. The swapchain must also let the window go before the
        // window is destroyed.
        platform->pending = rhi::SubmittedWork{};
        platform->output_transform.reset();
        platform->icvfx_to.reset();
        platform->icvfx_from.reset();
        platform->mesh_warp_to.reset();
        platform->mesh_warp_from.reset();
        platform->scene_mixer.reset();
        platform->scene_to.reset();
        platform->scene_from.reset();
        platform->output.reset();
        platform->swapchain = rhi::Swapchain{};
        platform->device.reset();
        if (platform->texture != nullptr) {
            SDL_DestroyTexture(platform->texture);
        }
        if (platform->renderer != nullptr) {
            SDL_DestroyRenderer(platform->renderer);
        }
        if (platform->window != nullptr) {
            SDL_DestroyWindow(platform->window);
        }
        delete platform;
        release_video();
        return Result::failure(error);
    };

    // The scene path is the GPU path plus a renderer; the output path is the
    // scene path plus the transform. Asking for a layer without the one beneath
    // it is a configuration mistake rather than something to interpret.
    if ((config.scene && !config.gpu) || (config.output && !config.scene) ||
        (config.buffered_media &&
         (!config.output || config.media_read_ahead_frames == 0U ||
          config.media_cache_mebibytes == 0U))) {
        return fail(show::PresenterError::InvalidConfiguration);
    }

    // Program must keep driving its connector while the operator works in
    // the editor. SDL's default minimizes exclusive fullscreen windows on
    // focus loss, which removes their swapchain and can throttle rendering.
    // This policy is process-wide and must remain active for every output.
    if (config.output &&
        !SDL_SetHintWithPriority(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0",
                                 SDL_HINT_OVERRIDE)) {
        return fail(show::PresenterError::DisplayUnavailable);
    }

    const bool defer_cpu_renderer =
        config.fullscreen && config.position_explicit;
    if (config.gpu || defer_cpu_renderer) {
        // The GPU path always claims the window directly. Explicit fullscreen
        // also delays the CPU renderer until placement is complete, so neither
        // renderer kind binds resources to the transient primary display.
        platform->window = SDL_CreateWindow(
            config.title.c_str(), static_cast<int>(config.width),
            static_cast<int>(config.height), window_flags(config));
    } else if (!SDL_CreateWindowAndRenderer(
                   config.title.c_str(), static_cast<int>(config.width),
                   static_cast<int>(config.height), window_flags(config),
                   &platform->window, &platform->renderer)) {
        return fail(show::PresenterError::DisplayUnavailable);
    }
    if (platform->window == nullptr) {
        return fail(show::PresenterError::DisplayUnavailable);
    }

    if (config.position_explicit) {
        if (!SDL_SetWindowPosition(platform->window, config.position_x,
                                   config.position_y)) {
            return fail(show::PresenterError::DisplayUnavailable);
        }
        if (!SDL_SyncWindow(platform->window)) {
            return fail(show::PresenterError::DisplayUnavailable);
        }
        // Which display this output drives is decided by the authored target,
        // not by where the window landed. A named display that is not here is
        // refused outright: it is the same binding that would otherwise open
        // silently on a neighbouring screen.
        const bool display_named = config.display_bounds_width != 0U &&
            config.display_bounds_height != 0U;
        SDL_DisplayID bound_display = 0U;
        if (display_named) {
            bound_display = display_for_bounds(
                config.display_bounds_x, config.display_bounds_y,
                config.display_bounds_width, config.display_bounds_height);
            if (bound_display == 0U) {
                return fail(show::PresenterError::DisplayTargetUnavailable);
            }
        }
        if (config.fullscreen && config.require_exact_display_mode) {
            if (config.refresh_numerator == 0U ||
                config.refresh_denominator == 0U) {
                return fail(show::PresenterError::InvalidConfiguration);
            }
            const SDL_DisplayID display =
                display_named ? bound_display
                              : SDL_GetDisplayForWindow(platform->window);
            int mode_count{};
            SDL_DisplayMode **const modes =
                display != 0U
                    ? SDL_GetFullscreenDisplayModes(display, &mode_count)
                    : nullptr;
            const SDL_DisplayMode *selected{};
            for (int index = 0; modes != nullptr && index < mode_count;
                 ++index) {
                const SDL_DisplayMode *const candidate = modes[index];
                if (candidate != nullptr &&
                    candidate->w == static_cast<int>(config.width) &&
                    candidate->h == static_cast<int>(config.height) &&
                    candidate->refresh_rate_numerator ==
                        static_cast<int>(config.refresh_numerator) &&
                    candidate->refresh_rate_denominator ==
                        static_cast<int>(config.refresh_denominator)) {
                    selected = candidate;
                    break;
                }
            }
            const bool selected_ok =
                selected != nullptr &&
                SDL_SetWindowFullscreenMode(platform->window, selected);
            SDL_free(modes);
            if (!selected_ok) {
                return fail(show::PresenterError::ExactDisplayModeUnavailable);
            }
        }
        if (config.fullscreen &&
            !SDL_SetWindowFullscreen(platform->window, true)) {
            return fail(show::PresenterError::DisplayUnavailable);
        }
        if (config.fullscreen && !SDL_SyncWindow(platform->window)) {
            return fail(show::PresenterError::DisplayUnavailable);
        }
        if (config.fullscreen && config.require_exact_display_mode) {
            const SDL_DisplayMode *const applied =
                SDL_GetWindowFullscreenMode(platform->window);
            if (applied == nullptr ||
                applied->w != static_cast<int>(config.width) ||
                applied->h != static_cast<int>(config.height) ||
                applied->refresh_rate_numerator !=
                    static_cast<int>(config.refresh_numerator) ||
                applied->refresh_rate_denominator !=
                    static_cast<int>(config.refresh_denominator)) {
                return fail(show::PresenterError::ExactDisplayModeUnavailable);
            }
        }
    }

    if (!config.gpu && platform->renderer == nullptr) {
        platform->renderer = SDL_CreateRenderer(platform->window, nullptr);
        if (platform->renderer == nullptr) {
            return fail(show::PresenterError::DisplayUnavailable);
        }
    }
    if (platform->renderer != nullptr) {
        // Presentation must never pace the show. The frame boundary owns
        // timing, and blocking on desktop refresh would beat against it.
        static_cast<void>(SDL_SetRenderVSync(platform->renderer, 0));
    }

    if (config.gpu) {
        if (!build_gpu_resources(*platform, config)) {
            return fail(show::PresenterError::DisplayUnavailable);
        }
    } else {
        platform->texture = SDL_CreateTexture(
            platform->renderer, SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STREAMING, static_cast<int>(config.width),
            static_cast<int>(config.height));
        if (platform->texture == nullptr) {
            return fail(show::PresenterError::DisplayUnavailable);
        }
    }
    platform->window_id = SDL_GetWindowID(platform->window);

    const auto present_black = [&]() -> bool {
        if (config.gpu) {
            auto acquired = platform->device->acquire_commands();
            if (!acquired) return false;
            auto commands = std::move(acquired).value();
            auto &black = platform->output->texture();
            if (!commands.clear(black, rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}))
                return false;
            const auto image = platform->swapchain.acquire(commands);
            if (image && !platform->swapchain.present(commands, image, black,
                                                     rhi::Filter::Nearest))
                return false;
            auto submitted = platform->device->submit(std::move(commands));
            // Startup only: complete the first paint before exposing a
            // window. No frame-recording path waits for GPU work here.
            if (!submitted || !std::move(submitted).value().wait()) return false;
            platform->last_presented = &black;
            return true;
        }
        return SDL_SetRenderDrawColor(platform->renderer, 0U, 0U, 0U, 255U) &&
            SDL_RenderClear(platform->renderer) &&
            SDL_RenderPresent(platform->renderer);
    };
    if (!present_black() || !SDL_ShowWindow(platform->window) ||
        !present_black()) {
        return fail(show::PresenterError::DisplayUnavailable);
    }

    std::unique_ptr<Presenter> presenter{new (std::nothrow) SdlPresenter(
        std::move(surface).value(), platform, config.accepts_commands,
        config.gpu, config.scene, config.output, config.buffered_media)};
    if (presenter != nullptr && config.gpu) {
        static_cast<SdlPresenter *>(presenter.get())->timing_.vsync_limited =
            platform->swapchain.present_mode() == rhi::PresentMode::Vsync;
    }
    if (presenter == nullptr) {
        return fail(show::PresenterError::AllocationFailed);
    }

    try {
        const std::lock_guard lock{registry_mutex()};
        registry().push_back(static_cast<SdlPresenter *>(presenter.get()));
    } catch (...) {
        // The presenter still owns the platform handles, so let its destructor
        // release them rather than unwinding them here.
        return Result::failure(show::PresenterError::AllocationFailed);
    }
    return Result::success(std::move(presenter));
}

SdlPresenter::~SdlPresenter() {
    {
        const std::lock_guard lock{registry_mutex()};
        auto &presenters = registry();
        presenters.erase(
            std::remove(presenters.begin(), presenters.end(), this),
            presenters.end());
    }

    if (platform_ != nullptr) {
        if (platform_->device) {
            platform_->device->wait_idle();
        }
        platform_->pending = rhi::SubmittedWork{};
        platform_->output_transform.reset();
        platform_->icvfx_to.reset();
        platform_->icvfx_from.reset();
        platform_->mesh_warp_to.reset();
        platform_->mesh_warp_from.reset();
        platform_->scene_mixer.reset();
        platform_->scene_to.reset();
        platform_->scene_from.reset();
        platform_->output.reset();
        platform_->swapchain = rhi::Swapchain{};
        platform_->device.reset();
        if (platform_->texture != nullptr) {
            SDL_DestroyTexture(platform_->texture);
        }
        if (platform_->renderer != nullptr) {
            SDL_DestroyRenderer(platform_->renderer);
        }
        if (platform_->window != nullptr) {
            SDL_DestroyWindow(platform_->window);
        }
        delete platform_;
        platform_ = nullptr;
    }
    release_video();
}

bool SdlPresenter::enqueue(show::PresenterCommand command) noexcept {
    if (command == show::PresenterCommand::None ||
        command_size_ == command_capacity) {
        return false;
    }
    const auto slot = (command_head_ + command_size_) % command_capacity;
    commands_[slot] = command;
    ++command_size_;
    return true;
}

namespace {

[[nodiscard]] show::PresenterBarrierState
as_presenter_barrier(rhi::PresentBarrierStatus status) noexcept {
    switch (status) {
    case rhi::PresentBarrierStatus::Unsupported:
        return show::PresenterBarrierState::Unsupported;
    case rhi::PresentBarrierStatus::NotJoined:
        return show::PresenterBarrierState::NotJoined;
    case rhi::PresentBarrierStatus::Joining:
        return show::PresenterBarrierState::Joining;
    case rhi::PresentBarrierStatus::Joined:
        return show::PresenterBarrierState::Joined;
    case rhi::PresentBarrierStatus::Left:
        return show::PresenterBarrierState::Left;
    case rhi::PresentBarrierStatus::Failed:
        return show::PresenterBarrierState::Failed;
    }
    return show::PresenterBarrierState::Unknown;
}

[[nodiscard]] show::PresenterVideoReference
as_presenter_video_reference(rhi::VideoReferenceSample sample) noexcept {
    switch (sample.state) {
    case rhi::VideoReferenceState::Unsupported:
        return {show::PresenterVideoReferenceState::Unsupported, 0U};
    case rhi::VideoReferenceState::Waiting:
        return {show::PresenterVideoReferenceState::Waiting, 0U};
    case rhi::VideoReferenceState::Locked:
        return {show::PresenterVideoReferenceState::Locked,
                sample.frame_counter};
    case rhi::VideoReferenceState::Lost:
        return {show::PresenterVideoReferenceState::Lost, 0U};
    case rhi::VideoReferenceState::Failed:
        return {show::PresenterVideoReferenceState::Failed, 0U};
    }
    return {};
}

} // namespace

void SdlPresenter::begin_document_stream() noexcept {
    if (platform_ != nullptr) {
        platform_->asset_gate.reset();
        platform_->assets.visible_commit = 0U;
    }
}

show::PresenterStatus SdlPresenter::hold() noexcept {
    // The window still has to breathe while its group holds, otherwise an
    // operator could not close an output that is deliberately not advancing.
    pump();
    if (closed_) {
        return show::PresenterStatus::Closed;
    }
    static_cast<void>(retire_pending());
    return show::PresenterStatus::Skipped;
}

show::PresenterBarrierState
SdlPresenter::apply_sync(const show::PresenterSyncRequest &request) noexcept {
    // Only the GPU path owns a swapchain. A CPU window presents through the
    // SDL renderer and can never be swap locked, so it says so rather than
    // reporting a state its path cannot have.
    if (!gpu_ || platform_ == nullptr || !platform_->device.has_value()) {
        sync_request_ = request;
        sync_state_ = show::PresenterBarrierState::Unsupported;
        return sync_state_;
    }
    const auto previous = sync_state_;
    sync_request_ = request;
    if (!request.enabled) {
        platform_->swapchain.leave_present_barrier();
        sync_state_ = show::PresenterBarrierState::NotJoined;
    } else {
        sync_state_ =
            as_presenter_barrier(platform_->swapchain.join_present_barrier(
                rhi::PresentBarrierDesc{request.group, request.barrier}));
    }
    if (previous == show::PresenterBarrierState::Joined &&
        sync_state_ != show::PresenterBarrierState::Joined) {
        ++sync_leave_events_;
    }
    return sync_state_;
}

show::PresenterSyncReport SdlPresenter::sync_report() const noexcept {
    show::PresenterSyncReport report{};
    report.group = sync_request_.group;
    report.barrier_id = sync_request_.barrier;
    // Frames the window was not ready for are repeats from the wall's point
    // of view: the previous image stayed on the panels.
    report.repeated = timing_.skipped_presents;
    report.leave_events = sync_leave_events_;
    if (!gpu_ || platform_ == nullptr || !platform_->device.has_value()) {
        report.barrier = show::PresenterBarrierState::Unsupported;
        return report;
    }
    // The backend is authoritative: a client can be dropped from a barrier
    // without this side ever asking to leave.
    report.barrier =
        as_presenter_barrier(platform_->swapchain.present_barrier_status());
    const auto statistics = platform_->swapchain.present_barrier_statistics();
    report.presents = statistics.presents;
    report.presents_in_sync = statistics.presents_in_sync;
    report.flips_in_sync = statistics.flips_in_sync;
    report.leave_events += statistics.leave_events;
    return report;
}

show::PresenterVideoReference SdlPresenter::video_reference() const noexcept {
    if (!gpu_ || platform_ == nullptr || !platform_->device.has_value()) {
        return {show::PresenterVideoReferenceState::Unsupported, 0U};
    }
    return as_presenter_video_reference(platform_->device->video_reference());
}

show::PresenterCommand SdlPresenter::take_command() noexcept {
    if (command_size_ == 0U) {
        return show::PresenterCommand::None;
    }
    const auto command = commands_[command_head_];
    command_head_ = (command_head_ + 1U) % command_capacity;
    --command_size_;
    return command;
}

void SdlPresenter::pump() noexcept {
    // One drain of the shared queue, dispatched by window. Locking only around
    // the snapshot keeps the dispatch itself off the registry mutex.
    std::vector<SdlPresenter *> peers;
    try {
        const std::lock_guard lock{registry_mutex()};
        peers = registry();
    } catch (...) {
        peers.clear();
    }

    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
            for (auto *peer : peers) {
                peer->closed_ = true;
            }
            closed_ = true;
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            for (auto *peer : peers) {
                if (peer->platform_ != nullptr &&
                    peer->platform_->window_id == event.window.windowID) {
                    peer->closed_ = true;
                }
            }
            break;
        case SDL_EVENT_KEY_DOWN: {
            if (event.key.repeat) {
                break;
            }
            const auto command = command_for(event.key.scancode);
            if (command == show::PresenterCommand::None) {
                break;
            }
            for (auto *peer : peers) {
                if (peer->platform_ == nullptr ||
                    peer->platform_->window_id != event.key.windowID) {
                    continue;
                }
                if (command == show::PresenterCommand::Quit) {
                    peer->closed_ = true;
                } else if (peer->accepts_commands_) {
                    static_cast<void>(peer->enqueue(command));
                }
            }
            break;
        }
        default:
            break;
        }
    }
}

core::u32 SdlPresenter::window_id() const noexcept {
    return platform_ == nullptr ? 0U
                                : static_cast<core::u32>(platform_->window_id);
}

rhi::BackendKind SdlPresenter::backend() const noexcept {
    if (platform_ == nullptr || !platform_->device) {
        return rhi::BackendKind::SdlGpu;
    }
    return platform_->device->capabilities().backend;
}

render::RendererReadiness SdlPresenter::render_readiness() const noexcept {
    if (platform_ == nullptr || !platform_->scene_from) {
        return {};
    }
    return platform_->scene_from->renderer_readiness();
}

bool SdlPresenter::refresh_output_projection_mesh() noexcept {
    return platform_ != nullptr && output_ &&
           platform_->projection_meshes.refresh_imported_meshes();
}

bool SdlPresenter::refresh_model_source(
    const std::string& source) noexcept {
    return platform_ != nullptr && platform_->scene_from.has_value()
        && platform_->scene_from->refresh_model_source(source);
}

render::OutputProjectionCacheStatus
SdlPresenter::output_projection_mesh_status() const noexcept {
    return platform_ != nullptr && output_
               ? platform_->projection_meshes.status()
               : render::OutputProjectionCacheStatus{};
}

show::PresenterRendererReadiness
SdlPresenter::renderer_readiness() const noexcept {
    show::PresenterRendererReadiness result{};
    if (platform_ == nullptr || !platform_->scene_from) {
        return result;
    }
    const render::RendererReadiness observed =
        platform_->scene_from->renderer_readiness();
    result.profile_digest = platform_->scene_from->renderer_profile_digest();
    if (platform_->device) result.adapter = platform_->device->adapter_identity();
    switch (observed.code) {
    case render::RendererReadinessCode::Ready:
        result.state = show::PresenterRendererReadinessState::Ready;
        break;
    case render::RendererReadinessCode::BasicFallback:
        result.state = show::PresenterRendererReadinessState::BasicFallback;
        break;
    case render::RendererReadinessCode::MissingCapabilities:
        result.state =
            show::PresenterRendererReadinessState::MissingCapabilities;
        break;
    case render::RendererReadinessCode::RequestedFeaturesUnavailable:
        result.state = show::PresenterRendererReadinessState::
            AdvancedImplementationUnavailable;
        break;
    case render::RendererReadinessCode::AdvancedImplementationUnavailable:
        result.state = show::PresenterRendererReadinessState::
            AdvancedImplementationUnavailable;
        break;
    case render::RendererReadinessCode::InvalidPolicy:
    // No authored camera is orthographic; one that arrives is a policy fault.
    case render::RendererReadinessCode::OrthographicCamera:
        result.state = show::PresenterRendererReadinessState::InvalidPolicy;
        break;
    case render::RendererReadinessCode::VideoMemoryShort:
        result.state = show::PresenterRendererReadinessState::VideoMemoryShort;
        result.video_memory_needed = observed.video_memory_needed;
        result.video_memory_free = observed.video_memory_free;
        break;
    }
    result.requested = observed.requested;
    result.selected = observed.selected;
    result.missing_capabilities = observed.missing_capabilities;
    result.satisfied = observed.satisfied;
    result.fallback = observed.fallback;
    result.assets = platform_->assets;
    result.timing = platform_->frame_timing;
    const auto include_temporal = [&](const render::CompositorSceneExecutor* executor) {
        if (!executor || executor->renderer_profile_digest() != result.profile_digest) return;
        if (platform_->scene_from && executor->temporal_document_version()
            != platform_->scene_from->temporal_document_version()) return;
        result.temporal_restarts = std::max(result.temporal_restarts, executor->temporal_restarts());
        result.temporal_restart_frame = std::max(result.temporal_restart_frame, executor->temporal_restart_frame());
    };
    include_temporal(platform_->scene_from ? &*platform_->scene_from : nullptr);
    include_temporal(platform_->scene_to ? &*platform_->scene_to : nullptr);
    include_temporal(platform_->icvfx_from ? platform_->icvfx_from->inner() : nullptr);
    include_temporal(platform_->icvfx_to ? platform_->icvfx_to->inner() : nullptr);
    return result;
}

show::PresenterPlateStats SdlPresenter::plates() const noexcept {
    show::PresenterPlateStats stats{};
    if (platform_ == nullptr) {
        return stats;
    }
    // Both stages, summed: a graph transition draws the plate through the
    // second one, and reporting only the first would show a node's plate count
    // stop dead at a swap it actually rendered through.
    include_icvfx_plates(platform_->icvfx_from, stats);
    include_icvfx_plates(platform_->icvfx_to, stats);
    return stats;
}

show::PresenterReadback SdlPresenter::readback() noexcept {
    if (platform_ == nullptr) {
        return {};
    }

    if (gpu_) {
        // Read the offscreen target rather than the window. It is what was
        // composited, it is available whether or not the window is showing,
        // and it is the surface real output hardware will take later.
        auto &platform = *platform_;
        if (!platform.device) {
            return {};
        }
        if (platform.last_presented == nullptr) {
            return {};
        }
        return digest_target(*platform.device, *platform.last_presented);
    }

    SDL_Surface *captured = SDL_RenderReadPixels(platform_->renderer, nullptr);
    if (captured == nullptr) {
        return {};
    }

    SDL_Surface *rgba = captured;
    if (captured->format != SDL_PIXELFORMAT_RGBA32) {
        rgba = SDL_ConvertSurface(captured, SDL_PIXELFORMAT_RGBA32);
    }

    show::PresenterReadback report{};
    if (rgba != nullptr && rgba->pixels != nullptr && rgba->w > 0 &&
        rgba->h > 0 && rgba->pitch > 0) {
        const auto bytes = static_cast<std::size_t>(rgba->pitch) *
                           static_cast<std::size_t>(rgba->h);
        const show::FrameBufferView view{
            std::span<core::u8>{static_cast<core::u8 *>(rgba->pixels), bytes},
            static_cast<core::u32>(rgba->w),
            static_cast<core::u32>(rgba->h),
            static_cast<core::u32>(rgba->pitch),
            show::PixelFormat::Rgba8888,
        };
        report.width = view.width;
        report.height = view.height;
        report.digest = show::frame_buffer_digest(view);
    }

    if (rgba != nullptr && rgba != captured) {
        SDL_DestroySurface(rgba);
    }
    SDL_DestroySurface(captured);
    return report;
}

show::PresenterStatus
SdlPresenter::present(const show::TestPatternInput &frame) noexcept {
    pump();
    if (closed_) {
        return show::PresenterStatus::Closed;
    }
    if (platform_ == nullptr) {
        return show::PresenterStatus::Failed;
    }
    return gpu_ ? present_gpu(frame) : present_cpu(frame);
}

bool SdlPresenter::retire_pending() noexcept {
    if (platform_ == nullptr) {
        return true;
    }
    auto &platform = *platform_;
    if (platform.pending) {
        if (!platform.pending.complete()) {
            return false;
        }
        platform.frame_timing = platform.pending.frame_timing();
        platform.frame_timing.sequence = platform.recording_sequence;
        platform.frame_timing.cpu_record_ns = platform.pending_record_ns;
        platform.frame_timing.profile_digest = platform.pending_profile_digest;
        const auto barrier_stats = platform.swapchain.present_barrier_statistics();
        platform.frame_timing.presents = barrier_stats.presents;
        platform.frame_timing.presents_in_sync = barrier_stats.presents_in_sync;
        platform.frame_timing.flips_in_sync = barrier_stats.flips_in_sync;
        if (platform.scene_from) platform.scene_from->observe_completed_timing(platform.frame_timing);
        if (platform.scene_to) platform.scene_to->observe_completed_timing(platform.frame_timing);
        const auto elapsed = static_cast<core::u64>(
            platform.pending.submit_to_complete().count());
        timing_.last_submit_to_observed_ns = elapsed;
        timing_.peak_submit_to_observed_ns =
            std::max(timing_.peak_submit_to_observed_ns, elapsed);
        if (platform.pending_asset_digest != 0) platform.uploaded_asset_digest = platform.pending_asset_digest;
        platform.pending_asset_digest = 0;
        platform.pending = rhi::SubmittedWork{};
    }
    return true;
}

show::PresenterStatus
SdlPresenter::present_recorded(const RecordFrame &record,
                               const bool *media_complete, const scene::Scene *asset_scene,
                               const scene::Scene *asset_partner) noexcept {
    auto &platform = *platform_;
    if (!platform.device) {
        return show::PresenterStatus::Failed;
    }
    const auto entered = std::chrono::steady_clock::now();
    const auto record_present = [this, entered]() noexcept {
        const auto spent = static_cast<core::u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - entered)
                .count());
        timing_.last_present_ns = spent;
        timing_.peak_present_ns = std::max(timing_.peak_present_ns, spent);
    };

    // No commands for this candidate have been recorded while the GPU is busy.
    // Reporting Skipped here would acknowledge an unrendered timeline position
    // and discard the consecutive-frame history that object motion blur needs.
    if (!retire_pending()) {
        record_present();
        return show::PresenterStatus::Deferred;
    }

    auto acquired = platform.device->acquire_commands();
    if (!acquired) {
        return show::PresenterStatus::Failed;
    }
    auto commands = std::move(acquired).value();

    // What differs between the test pattern and a scene is exactly this: what
    // gets recorded, and which texture the swapchain then blits. Everything
    // around it - retire, acquire, present, submit, time - is identical, and a
    // second copy of it would be a second definition of a presented frame.
    platform.last_presented = platform.visible_program_valid ? &*platform.visible_program : nullptr;
    if (asset_scene) platform.assets = {scene::asset_set_digest(*asset_scene), false, platform.asset_gate.visible_commit()};
    const auto cpu_record_begin = std::chrono::steady_clock::now();
    const rhi::Texture *presented = record(commands);
    platform.pending_record_ns = static_cast<core::u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - cpu_record_begin).count());
    platform.pending_profile_digest = asset_scene ? scene::renderer_settings_digest(asset_scene->renderer_settings) : 0U;
    ++platform.recording_sequence;
    if (presented == nullptr) {
        return show::PresenterStatus::Failed;
    }

    auto next_asset_gate = platform.asset_gate;
    bool assets_allowed = true;
    if (asset_scene) {
        // A dissolve within one scene - a Loop cue's crossfade - goes on
        // publishing the set already on screen, the one the frame it leaves
        // belongs to: a crossfade is not a reveal, and publishing the frames
        // it goes round to would hold the picture for an asset switch at the
        // very moment it should be mixing. Both sides still have to be ready,
        // and the permit is still matched on the frame the show is at.
        const bool within_scene =
            asset_partner != nullptr && asset_partner->id == asset_scene->id;
        const scene::Scene &gated = within_scene ? *asset_partner : *asset_scene;
        auto prepared = platform.scene_from->renderer_passes().geometry.assets;
        prepared.ready = prepared.ready && platform.scene_from->media_cache_stats().pending_requests == 0;
        if (asset_partner && platform.scene_to) {
            const auto other = platform.scene_to->renderer_passes().geometry.assets;
            if (!within_scene) {
                prepared.digest ^= other.digest + 0x9e3779b97f4a7c15ULL + (prepared.digest << 6) + (prepared.digest >> 2);
            }
            prepared.ready = prepared.ready && other.ready && platform.scene_to->media_cache_stats().pending_requests == 0;
        }
        if (platform.scene_from->scene_linear_output() && platform.output_transform)
            prepared.ready = prepared.ready && platform.output_transform->color_state() == render::DisplayLutState::Ready;
        prepared.ready = prepared.ready && asset_scene->publication_verified && (media_complete == nullptr || *media_complete);
        platform.pending_asset_digest = prepared.ready ? prepared.digest : 0;
        platform.assets = prepared;
        platform.assets.ready = prepared.ready && (prepared.digest == 0 || prepared.digest == platform.uploaded_asset_digest);
        platform.assets.visible_commit = platform.assets.digest == platform.asset_gate.visible_digest() ? platform.asset_gate.visible_commit() : 0;
        assets_allowed = next_asset_gate.present(platform.assets, gated.asset_commit_released ? gated.asset_commit : core::AssetCommit{},
            gated.control_frame, gated.show_frame, gated.asset_cohort_required);
    }
    if (!assets_allowed || (media_complete != nullptr && !*media_complete)) {
        // Mip uploads always defer presentation until the complete requested
        // set is ready; media uses the authored buffering policy. Keep the
        // already-recorded uploads and read-ahead work: discarding the
        // command list here would make the retry wait on the same GPU transfer.
        // The swapchain is deliberately untouched, so every plate on screen
        // remains from the previous complete scene.
        auto submitted = platform.device->submit(std::move(commands));
        if (!submitted) {
            return show::PresenterStatus::Failed;
        }
        platform.pending = std::move(submitted).value();
        ++timing_.frames;
        ++timing_.media_deferred_frames;
        record_present();
        return show::PresenterStatus::Deferred;
    }

    // Keep an immutable copy of the approved image for diagnostics as well as
    // the connector. Candidate renders continue while a replacement loads.
    if(asset_scene) {
        if(!platform.visible_program || platform.visible_program->width()!=presented->width() ||
            platform.visible_program->height()!=presented->height()) {
            auto visible=platform.device->create_texture(rhi::TextureDesc{presented->width(),presented->height(),presented->desc().format,true,true});
            if(!visible) return show::PresenterStatus::Failed;
            platform.last_presented=nullptr;
            platform.visible_program.emplace(std::move(visible).value());
            platform.visible_program_valid=false;
        }
        if(!commands.blit(*presented,*platform.visible_program,rhi::Filter::Nearest)) return show::PresenterStatus::Failed;
        presented=&*platform.visible_program;
    }

    const auto image = platform.swapchain.acquire(commands);
    auto status = show::PresenterStatus::Presented;
    if (image) {
        if (!platform.swapchain.present(commands, image, *presented,
                                        rhi::Filter::Linear)) {
            return show::PresenterStatus::Failed;
        }
    } else {
        // The window is not showing. The frame was still composited, which
        // keeps the offscreen target and the timing honest.
        ++timing_.skipped_presents;
        status = show::PresenterStatus::Skipped;
    }

    auto submitted = platform.device->submit(std::move(commands));
    if (!submitted) {
        return show::PresenterStatus::Failed;
    }
    platform.pending = std::move(submitted).value();
    platform.asset_gate = next_asset_gate;
    platform.last_presented = presented;
    if(asset_scene) platform.visible_program_valid=true;
    platform.assets.visible_commit = platform.assets.digest == platform.asset_gate.visible_digest() ? platform.asset_gate.visible_commit() : 0;
    ++timing_.frames;
    record_present();
    return status;
}

show::PresenterStatus
SdlPresenter::present_gpu(const show::TestPatternInput &frame) noexcept {
    auto &platform = *platform_;
    if (!platform.output) {
        return show::PresenterStatus::Failed;
    }
    return present_recorded(
        [&platform,
         &frame](rhi::CommandList &commands) noexcept -> const rhi::Texture * {
            if (!platform.output->record(commands, frame)) {
                return nullptr;
            }
            return &platform.output->texture();
        });
}

show::PresenterStatus
SdlPresenter::present(const mediaserver::scene::Scene &frame,
                      mediaserver::scene::CanvasRect slice) noexcept {
    pump();
    if (closed_) {
        return show::PresenterStatus::Closed;
    }
    // A presenter built without the scene path refuses rather than falling back
    // to a test pattern: a node asked to show a scene it cannot render has a
    // configuration fault, and quietly showing a colour bar would hide it.
    if (platform_ == nullptr || !scene_) {
        return show::PresenterStatus::Failed;
    }
    auto &platform = *platform_;
    if (!platform.scene_from || !platform.output_transform) {
        return show::PresenterStatus::Failed;
    }
    bool media_complete = true;
    return present_recorded(
        [this, &platform, &frame, &media_complete,
         slice](rhi::CommandList &commands) noexcept -> const rhi::Texture * {
            // The renderer draws this node's slice of the shared canvas into
            // its own colour target; that target is what the swapchain blits.
            const rhi::Texture *const target =
                record_scene(*platform.scene_from, commands, frame, slice);
            include_media_readiness(*platform.scene_from, media_complete, buffered_media_);
            return record_display_transform(
                *platform.output_transform, commands, target,
                platform.scene_from->scene_linear_output(), frame, slice);
        },
        &media_complete, &frame);
}

show::PresenterStatus
SdlPresenter::present(const mediaserver::scene::Scene &frame,
                      const mediaserver::compositor::CompositorGraph &graph,
                      mediaserver::scene::CanvasRect slice) noexcept {
    pump();
    if (closed_) {
        return show::PresenterStatus::Closed;
    }
    if (platform_ == nullptr || !scene_) {
        return show::PresenterStatus::Failed;
    }
    auto &platform = *platform_;
    if (!platform.scene_from || !platform.output_transform) {
        return show::PresenterStatus::Failed;
    }
    bool media_complete = true;
    return present_recorded(
        [this, &platform, &frame, &graph, &media_complete,
         slice](rhi::CommandList &commands) noexcept -> const rhi::Texture * {
            const rhi::Texture *const target = record_scene(
                *platform.scene_from, commands, graph, frame, slice);
            include_media_readiness(*platform.scene_from, media_complete, buffered_media_);
            return record_display_transform(
                *platform.output_transform, commands, target,
                platform.scene_from->scene_linear_output(), frame, slice);
        },
        &media_complete, &frame);
}

show::PresenterStatus
SdlPresenter::present(const mediaserver::scene::Scene &from,
                      const mediaserver::scene::Scene &to, float mix,
                      mediaserver::scene::CanvasRect slice) noexcept {
    pump();
    if (closed_) {
        return show::PresenterStatus::Closed;
    }
    if (platform_ == nullptr || !scene_) {
        return show::PresenterStatus::Failed;
    }
    auto &platform = *platform_;
    if (!platform.scene_from || !platform.scene_to || !platform.scene_mixer ||
        !platform.output_transform) {
        return show::PresenterStatus::Failed;
    }
    bool media_complete = true;
    return present_recorded(
        [this, &platform, &from, &to, mix, &media_complete,
         slice](rhi::CommandList &commands) noexcept -> const rhi::Texture * {
            const rhi::Texture *const from_target =
                record_scene(*platform.scene_from, commands, from, slice);
            const rhi::Texture *const to_target =
                record_scene(*platform.scene_to, commands, to, slice);
            include_media_readiness(*platform.scene_from, media_complete, buffered_media_);
            include_media_readiness(*platform.scene_to, media_complete, buffered_media_);
            const bool scene_linear =
                platform.scene_from->scene_linear_output();
            if (from_target == nullptr || to_target == nullptr ||
                scene_linear != platform.scene_to->scene_linear_output() ||
                !platform.scene_mixer->render(commands, *from_target,
                                              *to_target, mix)) {
                return nullptr;
            }
            return record_display_transform(
                *platform.output_transform, commands,
                &platform.scene_mixer->target(), scene_linear, from, slice);
        },
        &media_complete, &from, &to);
}

show::PresenterStatus SdlPresenter::present(
    const mediaserver::scene::Scene &from, const mediaserver::scene::Scene &to,
    float mix, const mediaserver::compositor::CompositorGraph &from_graph,
    const mediaserver::compositor::CompositorGraph &to_graph,
    mediaserver::scene::CanvasRect slice) noexcept {
    pump();
    if (closed_) {
        return show::PresenterStatus::Closed;
    }
    if (platform_ == nullptr || !scene_) {
        return show::PresenterStatus::Failed;
    }
    auto &platform = *platform_;
    if (!platform.scene_from || !platform.scene_to || !platform.scene_mixer ||
        !platform.output_transform) {
        return show::PresenterStatus::Failed;
    }
    bool media_complete = true;
    return present_recorded(
        [this, &platform, &from, &to, mix, &from_graph, &to_graph, &media_complete,
         slice](rhi::CommandList &commands) noexcept -> const rhi::Texture * {
            const rhi::Texture *const from_target = record_scene(
                *platform.scene_from, commands, from_graph, from, slice);
            const rhi::Texture *const to_target =
                record_scene(*platform.scene_to, commands, to_graph, to, slice);
            include_media_readiness(*platform.scene_from, media_complete, buffered_media_);
            include_media_readiness(*platform.scene_to, media_complete, buffered_media_);
            const bool scene_linear =
                platform.scene_from->scene_linear_output();
            if (from_target == nullptr || to_target == nullptr ||
                scene_linear != platform.scene_to->scene_linear_output() ||
                !platform.scene_mixer->render(commands, *from_target,
                                              *to_target, mix)) {
                return nullptr;
            }
            return record_display_transform(
                *platform.output_transform, commands,
                &platform.scene_mixer->target(), scene_linear, from, slice);
        },
        &media_complete, &from, &to);
}

show::PresenterStatus
SdlPresenter::present(const mediaserver::scene::Scene &frame,
                      const mediaserver::output::OutputMap &map) noexcept {
    return present(frame, std::span<const output::OutputMap>{&map, 1U});
}

show::PresenterStatus SdlPresenter::present(
    const mediaserver::scene::Scene &frame,
    std::span<const mediaserver::output::OutputMap> maps) noexcept {
    pump();
    if (closed_) {
        return show::PresenterStatus::Closed;
    }
    // As with the scene path, a presenter built without the output stage
    // refuses rather than falling back to something that looks nothing like the
    // show it was asked for.
    if (platform_ == nullptr || !output_ || !valid_map_identities(maps)) {
        return show::PresenterStatus::Failed;
    }
    auto &platform = *platform_;
    if (!platform.scene_from || !platform.mesh_warp_from ||
        !platform.output_transform) {
        return show::PresenterStatus::Failed;
    }
    bool mapping_unavailable = false;
    bool media_complete = true;
    const show::PresenterStatus status = present_recorded(
        [this, &platform, &frame, &mapping_unavailable, &media_complete,
         maps](rhi::CommandList &commands) noexcept -> const rhi::Texture * {
            // The project's authored colour, once per frame and before any
            // viewport is drawn, so every map in this window grades through
            // the same table.
            platform.output_transform->adopt_color(
                frame.renderer_settings.color, scene::asset_generation_key(frame));
            for (const auto& mapped : maps) {
                if (mapped.resolved_surface && !platform.output_transform->adopt_wall_color(
                    mapped.surface, mapped.resolved_surface->color)) return nullptr;
            }
            if (!platform.output_transform->clear(commands)) {
                return nullptr;
            }
            for (const output::OutputMap &map : maps) {
                const rhi::Texture *scene_target =
                    render::record_output_mapped_scene(
                        *platform.scene_from, *platform.mesh_warp_from,
                        platform.projection_meshes, commands, frame, map,
                        mapping_unavailable);
                include_media_readiness(*platform.scene_from, media_complete, buffered_media_);
                if (scene_target == nullptr) {
                    return mapping_unavailable
                               ? record_mapping_fault(
                                     commands, *platform.output_transform)
                               : nullptr;
                }
                scene_target = record_inner_frustum(
                    platform.icvfx_from, commands, frame, map, scene_target);
                if (scene_target == nullptr) {
                    return nullptr;
                }
                include_icvfx_readiness(platform.icvfx_from, media_complete, buffered_media_);
                if (!platform.output_transform->composite(
                        commands, *scene_target, map.viewport,
                        platform.scene_from->scene_linear_output(),
                        map.source, map.surface)) {
                    return nullptr;
                }
            }
            return &platform.output_transform->target();
        },
        &media_complete, &frame);
    return mapping_unavailable ? show::PresenterStatus::Failed : status;
}

show::PresenterStatus
SdlPresenter::present(const mediaserver::scene::Scene &frame,
                      const mediaserver::compositor::CompositorGraph &graph,
                      const mediaserver::output::OutputMap &map) noexcept {
    return present(frame, graph, std::span<const output::OutputMap>{&map, 1U});
}

show::PresenterStatus SdlPresenter::present(
    const mediaserver::scene::Scene &frame,
    const mediaserver::compositor::CompositorGraph &graph,
    std::span<const mediaserver::output::OutputMap> maps) noexcept {
    pump();
    if (closed_) {
        return show::PresenterStatus::Closed;
    }
    if (platform_ == nullptr || !output_ || !valid_map_identities(maps)) {
        return show::PresenterStatus::Failed;
    }
    auto &platform = *platform_;
    if (!platform.scene_from || !platform.mesh_warp_from ||
        !platform.output_transform) {
        return show::PresenterStatus::Failed;
    }
    bool mapping_unavailable = false;
    bool media_complete = true;
    const show::PresenterStatus status = present_recorded(
        [this, &platform, &frame, &graph, &mapping_unavailable, &media_complete,
         maps](rhi::CommandList &commands) noexcept -> const rhi::Texture * {
            // The project's authored colour, once per frame and before any
            // viewport is drawn, so every map in this window grades through
            // the same table.
            platform.output_transform->adopt_color(
                frame.renderer_settings.color, scene::asset_generation_key(frame));
            for (const auto& mapped : maps) {
                if (mapped.resolved_surface && !platform.output_transform->adopt_wall_color(
                    mapped.surface, mapped.resolved_surface->color)) return nullptr;
            }
            if (!platform.output_transform->clear(commands)) {
                return nullptr;
            }
            for (const output::OutputMap &map : maps) {
                const rhi::Texture *scene_target =
                    render::record_output_mapped_scene(
                        *platform.scene_from, *platform.mesh_warp_from,
                        platform.projection_meshes, commands, graph,
                        frame, map, mapping_unavailable);
                include_media_readiness(*platform.scene_from, media_complete, buffered_media_);
                if (scene_target == nullptr) {
                    return mapping_unavailable
                               ? record_mapping_fault(
                                     commands, *platform.output_transform)
                               : nullptr;
                }
                // The plate goes over the composited wall image, exactly as it
                // does on the graph-less path. This is the shipping output
                // path - a node presents through the compositor graph - so
                // without this an authored inner frustum would never reach a
                // wall however correctly the rest of it was derived.
                scene_target = record_inner_frustum(
                    platform.icvfx_from, commands, frame, map, scene_target);
                if (scene_target == nullptr) {
                    return nullptr;
                }
                include_icvfx_readiness(platform.icvfx_from, media_complete, buffered_media_);
                if (!platform.output_transform->composite(
                        commands, *scene_target, map.viewport,
                        platform.scene_from->scene_linear_output(),
                        map.source, map.surface)) {
                    return nullptr;
                }
            }
            return &platform.output_transform->target();
        },
        &media_complete, &frame);
    return mapping_unavailable ? show::PresenterStatus::Failed : status;
}

show::PresenterStatus
SdlPresenter::present(const mediaserver::scene::Scene &from,
                      const mediaserver::scene::Scene &to, float mix,
                      const mediaserver::output::OutputMap &map) noexcept {
    return present(from, to, mix, std::span<const output::OutputMap>{&map, 1U});
}

show::PresenterStatus SdlPresenter::present(
    const mediaserver::scene::Scene &from, const mediaserver::scene::Scene &to,
    float mix, std::span<const mediaserver::output::OutputMap> maps) noexcept {
    pump();
    if (closed_) {
        return show::PresenterStatus::Closed;
    }
    if (platform_ == nullptr || !output_ || !valid_map_identities(maps)) {
        return show::PresenterStatus::Failed;
    }
    auto &platform = *platform_;
    if (!platform.scene_from || !platform.scene_to || !platform.scene_mixer ||
        !platform.mesh_warp_from || !platform.mesh_warp_to ||
        !platform.output_transform) {
        return show::PresenterStatus::Failed;
    }
    bool mapping_unavailable = false;
    bool media_complete = true;
    const show::PresenterStatus status = present_recorded(
        [this, &platform, &from, &to, mix, &mapping_unavailable, &media_complete,
         maps](rhi::CommandList &commands) noexcept -> const rhi::Texture * {
            // The project's authored colour, once per frame and before any
            // viewport is drawn, so every map in this window grades through
            // the same table.
            platform.output_transform->adopt_color(
                from.renderer_settings.color, scene::asset_generation_key(from));
            for (const auto& mapped : maps) {
                if (mapped.resolved_surface && !platform.output_transform->adopt_wall_color(
                    mapped.surface, mapped.resolved_surface->color)) return nullptr;
            }
            if (!platform.output_transform->clear(commands)) {
                return nullptr;
            }
            for (const output::OutputMap &map : maps) {
                const rhi::Texture *const from_target =
                    render::record_output_mapped_scene(
                        *platform.scene_from, *platform.mesh_warp_from,
                        platform.projection_meshes, commands, from, map,
                        mapping_unavailable);
                const rhi::Texture *const to_target =
                    render::record_output_mapped_scene(
                        *platform.scene_to, *platform.mesh_warp_to,
                        platform.projection_meshes, commands, to, map,
                        mapping_unavailable);
                include_media_readiness(*platform.scene_from, media_complete, buffered_media_);
                include_media_readiness(*platform.scene_to, media_complete, buffered_media_);
                if (from_target == nullptr || to_target == nullptr) {
                    return mapping_unavailable
                               ? record_mapping_fault(
                                     commands, *platform.output_transform)
                               : nullptr;
                }
                const bool scene_linear =
                    platform.scene_from->scene_linear_output();
                if (scene_linear != platform.scene_to->scene_linear_output() ||
                    !platform.scene_mixer->render(commands, *from_target,
                                                  *to_target, mix) ||
                    !platform.output_transform->composite(
                        commands, platform.scene_mixer->target(), map.viewport,
                        scene_linear, map.source, map.surface)) {
                    return nullptr;
                }
            }
            return &platform.output_transform->target();
        },
        &media_complete, &from, &to);
    return mapping_unavailable ? show::PresenterStatus::Failed : status;
}

show::PresenterStatus SdlPresenter::present(
    const mediaserver::scene::Scene &from, const mediaserver::scene::Scene &to,
    float mix, const mediaserver::compositor::CompositorGraph &from_graph,
    const mediaserver::compositor::CompositorGraph &to_graph,
    const mediaserver::output::OutputMap &map) noexcept {
    return present(from, to, mix, from_graph, to_graph,
                   std::span<const output::OutputMap>{&map, 1U});
}

show::PresenterStatus SdlPresenter::present(
    const mediaserver::scene::Scene &from, const mediaserver::scene::Scene &to,
    float mix, const mediaserver::compositor::CompositorGraph &from_graph,
    const mediaserver::compositor::CompositorGraph &to_graph,
    std::span<const mediaserver::output::OutputMap> maps) noexcept {
    pump();
    if (closed_) {
        return show::PresenterStatus::Closed;
    }
    if (platform_ == nullptr || !output_ || !valid_map_identities(maps)) {
        return show::PresenterStatus::Failed;
    }
    auto &platform = *platform_;
    if (!platform.scene_from || !platform.scene_to || !platform.scene_mixer ||
        !platform.mesh_warp_from || !platform.mesh_warp_to ||
        !platform.output_transform) {
        return show::PresenterStatus::Failed;
    }
    bool mapping_unavailable = false;
    bool media_complete = true;
    const show::PresenterStatus status = present_recorded(
        [this, &platform, &from, &to, mix, &from_graph, &to_graph,
         &mapping_unavailable, &media_complete,
         maps](rhi::CommandList &commands) noexcept -> const rhi::Texture * {
            // The project's authored colour, once per frame and before any
            // viewport is drawn, so every map in this window grades through
            // the same table.
            platform.output_transform->adopt_color(
                from.renderer_settings.color, scene::asset_generation_key(from));
            for (const auto& mapped : maps) {
                if (mapped.resolved_surface && !platform.output_transform->adopt_wall_color(
                    mapped.surface, mapped.resolved_surface->color)) return nullptr;
            }
            if (!platform.output_transform->clear(commands)) {
                return nullptr;
            }
            for (const output::OutputMap &map : maps) {
                const rhi::Texture *from_target =
                    render::record_output_mapped_scene(
                        *platform.scene_from, *platform.mesh_warp_from,
                        platform.projection_meshes, commands, from_graph,
                        from, map, mapping_unavailable);
                const rhi::Texture *to_target =
                    render::record_output_mapped_scene(
                        *platform.scene_to, *platform.mesh_warp_to,
                        platform.projection_meshes, commands, to_graph,
                        to, map, mapping_unavailable);
                include_media_readiness(*platform.scene_from, media_complete, buffered_media_);
                include_media_readiness(*platform.scene_to, media_complete, buffered_media_);
                if (from_target == nullptr || to_target == nullptr) {
                    return mapping_unavailable
                               ? record_mapping_fault(
                                     commands, *platform.output_transform)
                               : nullptr;
                }
                // Each side's plate over its own wall image, before the mix.
                // Mixing first and plating after would show one plate for two
                // scenes and cross-dissolve the wall behind it, which is a
                // different shot from the one either scene describes.
                from_target = record_inner_frustum(
                    platform.icvfx_from, commands, from, map, from_target);
                to_target = record_inner_frustum(
                    platform.icvfx_to, commands, to, map, to_target);
                if (from_target == nullptr || to_target == nullptr) {
                    return nullptr;
                }
                include_icvfx_readiness(platform.icvfx_from, media_complete, buffered_media_);
                include_icvfx_readiness(platform.icvfx_to, media_complete, buffered_media_);
                const bool scene_linear =
                    platform.scene_from->scene_linear_output();
                if (scene_linear != platform.scene_to->scene_linear_output() ||
                    !platform.scene_mixer->render(commands, *from_target,
                                                  *to_target, mix) ||
                    !platform.output_transform->composite(
                        commands, platform.scene_mixer->target(), map.viewport,
                        scene_linear, map.source, map.surface)) {
                    return nullptr;
                }
            }
            return &platform.output_transform->target();
        },
        &media_complete, &from, &to);
    return mapping_unavailable ? show::PresenterStatus::Failed : status;
}

show::PresenterStatus
SdlPresenter::present_cpu(const show::TestPatternInput &frame) noexcept {
    if (!show::render_test_pattern(surface_.view(), frame)) {
        return show::PresenterStatus::Failed;
    }

    if (!SDL_UpdateTexture(platform_->texture, nullptr, surface_.pixels(),
                           static_cast<int>(surface_.stride()))) {
        return show::PresenterStatus::Failed;
    }
    if (!SDL_RenderClear(platform_->renderer) ||
        !SDL_RenderTexture(platform_->renderer, platform_->texture, nullptr,
                           nullptr) ||
        !SDL_RenderPresent(platform_->renderer)) {
        return show::PresenterStatus::Failed;
    }
    ++timing_.frames;
    return show::PresenterStatus::Presented;
}

} // namespace mediaserver::ui
