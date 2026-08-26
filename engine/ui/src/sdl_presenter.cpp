#include <mediaserver/ui/sdl_presenter.hpp>

#include <mediaserver/render/compositor_scene_executor.hpp>
#include <mediaserver/render/mapped_scene.hpp>
#include <mediaserver/render/output_transform.hpp>
#include <mediaserver/render/projection_mesh_warp.hpp>
#include <mediaserver/render/scene_mixer.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/ui/gpu_output_target.hpp>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

namespace mediaserver::ui {

struct SdlPresenter::Platform final {
    SDL_Window* window{};
    // The CPU path owns a renderer and a streaming texture. The GPU path
    // owns a device, a swapchain, and one offscreen target; the two are
    // mutually exclusive because both would claim the same window surface.
    SDL_Renderer* renderer{};
    SDL_Texture* texture{};
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
    // Present only for the output path: the feed-map crop and soft-edge blend
    // applied to the scene renderer's target on the way to the window.
    std::optional<render::OutputTransform> output_transform;
    std::optional<render::ProjectionMeshWarp> mesh_warp_from;
    std::optional<render::ProjectionMeshWarp> mesh_warp_to;
    // Imported projection geometry is decoded once per authored file revision,
    // not once per frame. Program/Rehearsal share the same physical patch and
    // can therefore share this immutable CPU mesh while retaining separate GPU
    // warp targets. Failed loads retry and a changed file fingerprint reloads.
    render::OutputProjectionCache imported_projection_mesh;
    // Borrowed from one of the targets above. Updated only after a frame has
    // been recorded successfully, so readback always inspects the texture the
    // swapchain was actually given (single scene, mixed scene, mapped output,
    // or test pattern).
    const rhi::Texture* last_presented{};
    rhi::SubmittedWork pending;
};

namespace {

namespace rhi = mediaserver::rhi;

// SDL owns one process-wide event queue and one video subsystem, so the
// lifetime of both is reference counted across presenters and every poll is
// dispatched by window id. Without that, a second window would silently steal
// the first window's close event.
std::mutex& registry_mutex() noexcept {
    static std::mutex mutex;
    return mutex;
}

std::vector<SdlPresenter*>& registry() {
    static std::vector<SdlPresenter*> presenters;
    return presenters;
}

std::size_t& video_users() noexcept {
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
window_flags(const SdlPresenterConfig& config) noexcept {
    SDL_WindowFlags flags = 0U;
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
digest_target(rhi::Device& device, const rhi::Texture& target) noexcept {
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
        std::span<core::u8>{const_cast<core::u8*>(pixels.data()),
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

[[nodiscard]] const rhi::Texture*
record_scene(render::CompositorSceneExecutor& executor,
             rhi::CommandList& commands, const scene::Scene& frame,
             scene::CanvasRect slice) noexcept {
    auto rendered = executor.execute(commands, frame, slice);
    return rendered ? rendered.value() : nullptr;
}

[[nodiscard]] const rhi::Texture*
record_scene(render::CompositorSceneExecutor& executor,
             rhi::CommandList& commands,
             const compositor::CompositorGraph& graph,
             const scene::Scene& frame, scene::CanvasRect slice) noexcept;

[[nodiscard]] const rhi::Texture*
record_mapping_fault(rhi::CommandList& commands,
                     render::OutputTransform& output_transform) noexcept {
    // A missing or invalid calibration must not leave the connector showing
    // its previous successful frame. Submit an explicit black frame while the
    // public present() result remains Failed for health monitoring.
    rhi::Texture& target = output_transform.target();
    return commands.clear(target, rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}) ? &target
                                                                      : nullptr;
}

[[nodiscard]] const rhi::Texture*
record_scene(render::CompositorSceneExecutor& executor,
             rhi::CommandList& commands,
             const compositor::CompositorGraph& graph,
             const scene::Scene& frame, scene::CanvasRect slice) noexcept {
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

SdlPresenter::SdlPresenter(show::PresenterSurface surface, Platform* platform,
                           bool accepts_commands, bool gpu, bool scene,
                           bool output) noexcept
    : surface_(std::move(surface)), platform_(platform),
      accepts_commands_(accepts_commands), gpu_(gpu), scene_(scene),
      output_(output) {
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
           render::SceneMixer::shaders_available() && rhi::Device::available();
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
    Platform& platform, const SdlPresenterConfig& config) noexcept {
    if (!GpuOutputTarget::shaders_available()) {
        return false;
    }

    rhi::DeviceConfig device_config{};
    device_config.debug = false;
    auto created = rhi::Device::create(device_config);
    if (!created) {
        return false;
    }
    platform.device.emplace(std::move(created).value());

    // A window that cannot back a swapchain is a hard refusal, not a quiet
    // downgrade: the caller asked for output on this window.
    auto claimed = platform.device->claim_window(platform.window);
    if (!claimed) {
        return false;
    }
    platform.swapchain = std::move(claimed).value();

    auto output =
        GpuOutputTarget::create(*platform.device, config.width, config.height);
    if (!output) {
        return false;
    }
    platform.output.emplace(std::move(output).value());

    if (config.scene) {
        auto from_renderer = render::CompositorSceneExecutor::create(
            *platform.device, config.width, config.height);
        if (!from_renderer) {
            return false;
        }
        platform.scene_from.emplace(std::move(from_renderer).value());

        auto to_renderer = render::CompositorSceneExecutor::create(
            *platform.device, config.width, config.height);
        if (!to_renderer) {
            return false;
        }
        platform.scene_to.emplace(std::move(to_renderer).value());

        auto mixer = render::SceneMixer::create(*platform.device, config.width,
                                                config.height);
        if (!mixer) {
            return false;
        }
        platform.scene_mixer.emplace(std::move(mixer).value());
    }

    if (config.output) {
        auto from_warp = render::ProjectionMeshWarp::create(
            *platform.device, config.width, config.height);
        if (!from_warp) {
            return false;
        }
        platform.mesh_warp_from.emplace(std::move(from_warp).value());

        auto to_warp = render::ProjectionMeshWarp::create(
            *platform.device, config.width, config.height);
        if (!to_warp) {
            return false;
        }
        platform.mesh_warp_to.emplace(std::move(to_warp).value());

        auto transform = render::OutputTransform::create(
            *platform.device, config.width, config.height);
        if (!transform) {
            return false;
        }
        platform.output_transform.emplace(std::move(transform).value());
    }
    return true;
}

show::PresenterResult
SdlPresenter::create(const SdlPresenterConfig& config) noexcept {
    using Result = show::PresenterResult;

    auto surface = show::PresenterSurface::create(config.width, config.height);
    if (!surface) {
        return Result::failure(surface.error());
    }
    if (!acquire_video()) {
        return Result::failure(show::PresenterError::DisplayUnavailable);
    }

    auto* platform = new (std::nothrow) Platform{};
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
    if ((config.scene && !config.gpu) || (config.output && !config.scene)) {
        release_video();
        return Result::failure(show::PresenterError::InvalidConfiguration);
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
        if (!SDL_SetWindowPosition(
                platform->window, config.position_x, config.position_y)) {
            return fail(show::PresenterError::DisplayUnavailable);
        }
        if (!SDL_SyncWindow(platform->window)) {
            return fail(show::PresenterError::DisplayUnavailable);
        }
        if (config.fullscreen &&
            !SDL_SetWindowFullscreen(platform->window, true)) {
            return fail(show::PresenterError::DisplayUnavailable);
        }
        if (config.fullscreen && !SDL_SyncWindow(platform->window)) {
            return fail(show::PresenterError::DisplayUnavailable);
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

    std::unique_ptr<Presenter> presenter{new (std::nothrow) SdlPresenter(
        std::move(surface).value(), platform, config.accepts_commands,
        config.gpu, config.scene, config.output)};
    if (presenter != nullptr && config.gpu) {
        static_cast<SdlPresenter*>(presenter.get())->timing_.vsync_limited =
            platform->swapchain.present_mode() == rhi::PresentMode::Vsync;
    }
    if (presenter == nullptr) {
        return fail(show::PresenterError::AllocationFailed);
    }

    try {
        const std::lock_guard lock{registry_mutex()};
        registry().push_back(static_cast<SdlPresenter*>(presenter.get()));
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
        auto& presenters = registry();
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
    std::vector<SdlPresenter*> peers;
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
            for (auto* peer : peers) {
                peer->closed_ = true;
            }
            closed_ = true;
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            for (auto* peer : peers) {
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
            for (auto* peer : peers) {
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

show::PresenterReadback SdlPresenter::readback() noexcept {
    if (platform_ == nullptr) {
        return {};
    }

    if (gpu_) {
        // Read the offscreen target rather than the window. It is what was
        // composited, it is available whether or not the window is showing,
        // and it is the surface real output hardware will take later.
        auto& platform = *platform_;
        if (!platform.device) {
            return {};
        }
        if (platform.last_presented == nullptr) {
            return {};
        }
        return digest_target(*platform.device, *platform.last_presented);
    }

    SDL_Surface* captured = SDL_RenderReadPixels(platform_->renderer, nullptr);
    if (captured == nullptr) {
        return {};
    }

    SDL_Surface* rgba = captured;
    if (captured->format != SDL_PIXELFORMAT_RGBA32) {
        rgba = SDL_ConvertSurface(captured, SDL_PIXELFORMAT_RGBA32);
    }

    show::PresenterReadback report{};
    if (rgba != nullptr && rgba->pixels != nullptr && rgba->w > 0 &&
        rgba->h > 0 && rgba->pitch > 0) {
        const auto bytes = static_cast<std::size_t>(rgba->pitch) *
                           static_cast<std::size_t>(rgba->h);
        const show::FrameBufferView view{
            std::span<core::u8>{static_cast<core::u8*>(rgba->pixels), bytes},
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
SdlPresenter::present(const show::TestPatternInput& frame) noexcept {
    pump();
    if (closed_) {
        return show::PresenterStatus::Closed;
    }
    if (platform_ == nullptr) {
        return show::PresenterStatus::Failed;
    }
    return gpu_ ? present_gpu(frame) : present_cpu(frame);
}

show::PresenterStatus
SdlPresenter::present_recorded(const RecordFrame& record) noexcept {
    auto& platform = *platform_;
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

    // Retire the previous submission first. With two frames in flight this is
    // one frame of overlap, and it is what makes the reported time a real
    // per-frame measurement rather than an average.
    if (platform.pending) {
        static_cast<void>(platform.pending.wait());
        const auto elapsed = static_cast<core::u64>(
            platform.pending.submit_to_complete().count());
        timing_.last_submit_to_observed_ns = elapsed;
        timing_.peak_submit_to_observed_ns =
            std::max(timing_.peak_submit_to_observed_ns, elapsed);
        platform.pending = rhi::SubmittedWork{};
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
    platform.last_presented = nullptr;
    const rhi::Texture* const presented = record(commands);
    if (presented == nullptr) {
        return show::PresenterStatus::Failed;
    }
    platform.last_presented = presented;

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
    ++timing_.frames;
    record_present();
    return status;
}

show::PresenterStatus
SdlPresenter::present_gpu(const show::TestPatternInput& frame) noexcept {
    auto& platform = *platform_;
    if (!platform.output) {
        return show::PresenterStatus::Failed;
    }
    return present_recorded(
        [&platform,
         &frame](rhi::CommandList& commands) noexcept -> const rhi::Texture* {
            if (!platform.output->record(commands, frame)) {
                return nullptr;
            }
            return &platform.output->texture();
        });
}

show::PresenterStatus
SdlPresenter::present(const mediaserver::scene::Scene& frame,
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
    auto& platform = *platform_;
    if (!platform.scene_from) {
        return show::PresenterStatus::Failed;
    }
    return present_recorded(
        [&platform, &frame,
         slice](rhi::CommandList& commands) noexcept -> const rhi::Texture* {
            // The renderer draws this node's slice of the shared canvas into
            // its own colour target; that target is what the swapchain blits.
            return record_scene(*platform.scene_from, commands, frame, slice);
        });
}

show::PresenterStatus
SdlPresenter::present(const mediaserver::scene::Scene& frame,
                      const mediaserver::compositor::CompositorGraph& graph,
                      mediaserver::scene::CanvasRect slice) noexcept {
    pump();
    if (closed_) {
        return show::PresenterStatus::Closed;
    }
    if (platform_ == nullptr || !scene_) {
        return show::PresenterStatus::Failed;
    }
    auto& platform = *platform_;
    if (!platform.scene_from) {
        return show::PresenterStatus::Failed;
    }
    return present_recorded(
        [&platform, &frame, &graph,
         slice](rhi::CommandList& commands) noexcept -> const rhi::Texture* {
            return record_scene(*platform.scene_from, commands, graph, frame,
                                slice);
        });
}

show::PresenterStatus
SdlPresenter::present(const mediaserver::scene::Scene& from,
                      const mediaserver::scene::Scene& to, float mix,
                      mediaserver::scene::CanvasRect slice) noexcept {
    pump();
    if (closed_) {
        return show::PresenterStatus::Closed;
    }
    if (platform_ == nullptr || !scene_) {
        return show::PresenterStatus::Failed;
    }
    auto& platform = *platform_;
    if (!platform.scene_from || !platform.scene_to || !platform.scene_mixer) {
        return show::PresenterStatus::Failed;
    }
    return present_recorded(
        [&platform, &from, &to, mix,
         slice](rhi::CommandList& commands) noexcept -> const rhi::Texture* {
            const rhi::Texture* const from_target =
                record_scene(*platform.scene_from, commands, from, slice);
            const rhi::Texture* const to_target =
                record_scene(*platform.scene_to, commands, to, slice);
            if (from_target == nullptr || to_target == nullptr ||
                !platform.scene_mixer->render(commands, *from_target,
                                              *to_target, mix)) {
                return nullptr;
            }
            return &platform.scene_mixer->target();
        });
}

show::PresenterStatus
SdlPresenter::present(const mediaserver::scene::Scene& from,
                      const mediaserver::scene::Scene& to, float mix,
                      const mediaserver::compositor::CompositorGraph& graph,
                      mediaserver::scene::CanvasRect slice) noexcept {
    pump();
    if (closed_) {
        return show::PresenterStatus::Closed;
    }
    if (platform_ == nullptr || !scene_) {
        return show::PresenterStatus::Failed;
    }
    auto& platform = *platform_;
    if (!platform.scene_from || !platform.scene_to || !platform.scene_mixer) {
        return show::PresenterStatus::Failed;
    }
    return present_recorded(
        [&platform, &from, &to, mix, &graph,
         slice](rhi::CommandList& commands) noexcept -> const rhi::Texture* {
            const rhi::Texture* const from_target = record_scene(
                *platform.scene_from, commands, graph, from, slice);
            const rhi::Texture* const to_target =
                record_scene(*platform.scene_to, commands, graph, to, slice);
            if (from_target == nullptr || to_target == nullptr ||
                !platform.scene_mixer->render(commands, *from_target,
                                              *to_target, mix)) {
                return nullptr;
            }
            return &platform.scene_mixer->target();
        });
}

show::PresenterStatus
SdlPresenter::present(const mediaserver::scene::Scene& frame,
                      const mediaserver::output::OutputMap& map) noexcept {
    pump();
    if (closed_) {
        return show::PresenterStatus::Closed;
    }
    // As with the scene path, a presenter built without the output stage
    // refuses rather than falling back to something that looks nothing like the
    // show it was asked for.
    if (platform_ == nullptr || !output_) {
        return show::PresenterStatus::Failed;
    }
    auto& platform = *platform_;
    if (!platform.scene_from || !platform.mesh_warp_from ||
        !platform.output_transform) {
        return show::PresenterStatus::Failed;
    }
    bool mapping_unavailable = false;
    const show::PresenterStatus status = present_recorded(
        [&platform, &frame, &mapping_unavailable,
         &map](rhi::CommandList& commands) noexcept -> const rhi::Texture* {
            // Two passes on one list: the renderer draws the map's source
            // rectangle of the canvas into the scene target, then the output
            // stage samples that target into its own. The output target is what
            // the swapchain blits.
            const rhi::Texture* const scene_target =
                render::record_output_mapped_scene(
                *platform.scene_from, *platform.mesh_warp_from,
                platform.imported_projection_mesh, commands, frame, map,
                mapping_unavailable);
            if (scene_target == nullptr) {
                return mapping_unavailable
                    ? record_mapping_fault(commands, *platform.output_transform)
                    : nullptr;
            }
            if (!platform.output_transform->render(commands, *scene_target)) {
                return nullptr;
            }
            return &platform.output_transform->target();
        });
    return mapping_unavailable ? show::PresenterStatus::Failed : status;
}

show::PresenterStatus
SdlPresenter::present(const mediaserver::scene::Scene& frame,
                      const mediaserver::compositor::CompositorGraph& graph,
                      const mediaserver::output::OutputMap& map) noexcept {
    pump();
    if (closed_) {
        return show::PresenterStatus::Closed;
    }
    if (platform_ == nullptr || !output_) {
        return show::PresenterStatus::Failed;
    }
    auto& platform = *platform_;
    if (!platform.scene_from || !platform.mesh_warp_from ||
        !platform.output_transform) {
        return show::PresenterStatus::Failed;
    }
    bool mapping_unavailable = false;
    const show::PresenterStatus status = present_recorded(
        [&platform, &frame, &graph, &mapping_unavailable,
         &map](rhi::CommandList& commands) noexcept -> const rhi::Texture* {
            const rhi::Texture* const scene_target =
                render::record_output_mapped_scene(
                *platform.scene_from, *platform.mesh_warp_from,
                platform.imported_projection_mesh, commands, graph, frame, map,
                mapping_unavailable);
            if (scene_target == nullptr) {
                return mapping_unavailable
                    ? record_mapping_fault(commands, *platform.output_transform)
                    : nullptr;
            }
            if (!platform.output_transform->render(commands, *scene_target)) {
                return nullptr;
            }
            return &platform.output_transform->target();
        });
    return mapping_unavailable ? show::PresenterStatus::Failed : status;
}

show::PresenterStatus
SdlPresenter::present(const mediaserver::scene::Scene& from,
                      const mediaserver::scene::Scene& to, float mix,
                      const mediaserver::output::OutputMap& map) noexcept {
    pump();
    if (closed_) {
        return show::PresenterStatus::Closed;
    }
    if (platform_ == nullptr || !output_) {
        return show::PresenterStatus::Failed;
    }
    auto& platform = *platform_;
    if (!platform.scene_from || !platform.scene_to || !platform.scene_mixer ||
        !platform.mesh_warp_from || !platform.mesh_warp_to ||
        !platform.output_transform) {
        return show::PresenterStatus::Failed;
    }
    bool mapping_unavailable = false;
    const show::PresenterStatus status = present_recorded(
        [&platform, &from, &to, mix, &mapping_unavailable,
         &map](rhi::CommandList& commands) noexcept -> const rhi::Texture* {
            const rhi::Texture* const from_target =
                render::record_output_mapped_scene(
                *platform.scene_from, *platform.mesh_warp_from,
                platform.imported_projection_mesh, commands, from, map,
                mapping_unavailable);
            const rhi::Texture* const to_target =
                render::record_output_mapped_scene(
                    *platform.scene_to, *platform.mesh_warp_to,
                    platform.imported_projection_mesh, commands, to, map,
                    mapping_unavailable);
            if (from_target == nullptr || to_target == nullptr) {
                return mapping_unavailable
                    ? record_mapping_fault(commands, *platform.output_transform)
                    : nullptr;
            }
            if (!platform.scene_mixer->render(commands, *from_target,
                                              *to_target, mix) ||
                !platform.output_transform->render(
                    commands, platform.scene_mixer->target())) {
                return nullptr;
            }
            return &platform.output_transform->target();
        });
    return mapping_unavailable ? show::PresenterStatus::Failed : status;
}

show::PresenterStatus
SdlPresenter::present(const mediaserver::scene::Scene& from,
                      const mediaserver::scene::Scene& to, float mix,
                      const mediaserver::compositor::CompositorGraph& graph,
                      const mediaserver::output::OutputMap& map) noexcept {
    pump();
    if (closed_) {
        return show::PresenterStatus::Closed;
    }
    if (platform_ == nullptr || !output_) {
        return show::PresenterStatus::Failed;
    }
    auto& platform = *platform_;
    if (!platform.scene_from || !platform.scene_to || !platform.scene_mixer ||
        !platform.mesh_warp_from || !platform.mesh_warp_to ||
        !platform.output_transform) {
        return show::PresenterStatus::Failed;
    }
    bool mapping_unavailable = false;
    const show::PresenterStatus status = present_recorded(
        [&platform, &from, &to, mix, &graph, &mapping_unavailable,
         &map](rhi::CommandList& commands) noexcept -> const rhi::Texture* {
            const rhi::Texture* const from_target =
                render::record_output_mapped_scene(
                *platform.scene_from, *platform.mesh_warp_from,
                platform.imported_projection_mesh, commands, graph, from, map,
                mapping_unavailable);
            const rhi::Texture* const to_target =
                render::record_output_mapped_scene(
                    *platform.scene_to, *platform.mesh_warp_to,
                    platform.imported_projection_mesh, commands, graph, to,
                    map, mapping_unavailable);
            if (from_target == nullptr || to_target == nullptr) {
                return mapping_unavailable
                    ? record_mapping_fault(commands, *platform.output_transform)
                    : nullptr;
            }
            if (!platform.scene_mixer->render(commands, *from_target,
                                              *to_target, mix) ||
                !platform.output_transform->render(
                    commands, platform.scene_mixer->target())) {
                return nullptr;
            }
            return &platform.output_transform->target();
        });
    return mapping_unavailable ? show::PresenterStatus::Failed : status;
}

show::PresenterStatus
SdlPresenter::present_cpu(const show::TestPatternInput& frame) noexcept {
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
