#include <mediaserver/ui/sdl_presenter.hpp>

#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/ui/gpu_output_target.hpp>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <new>
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
    std::optional<render::SceneRenderer> scene;
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

[[nodiscard]] SDL_WindowFlags window_flags(const SdlPresenterConfig& config) noexcept {
    SDL_WindowFlags flags = 0U;
    if (config.fullscreen) {
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

[[nodiscard]] show::PresenterCommand command_for(SDL_Scancode scancode) noexcept {
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
[[nodiscard]] show::PresenterReadback digest_target(
    rhi::Device& device,
    const rhi::Texture& target) noexcept {
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
        std::span<core::u8>{const_cast<core::u8*>(pixels.data()), pixels.size()},
        width,
        height,
        width * 4U,
        show::PixelFormat::Rgba8888,
    };
    const show::PresenterReadback report{
        width, height, show::frame_buffer_digest(view)};
    buffer.value().unmap();
    return report;
}

} // namespace

SdlPresenter::SdlPresenter(
    show::PresenterSurface surface,
    Platform* platform,
    bool accepts_commands,
    bool gpu,
    bool scene) noexcept
    : surface_(std::move(surface)),
      platform_(platform),
      accepts_commands_(accepts_commands),
      gpu_(gpu),
      scene_(scene) {
    timing_.gpu = gpu;
}

bool gpu_output_available() noexcept {
    return GpuOutputTarget::shaders_available() && rhi::Device::available();
}

bool scene_output_available() noexcept {
    // The 3D path carries its own bytecode, so it can be absent while the 2D
    // one is present if the shader build was only half applied. Both questions
    // also need a device, which is what makes this more than a build check.
    return render::SceneRenderer::shaders_available() && rhi::Device::available();
}

// Everything the GPU path needs, built once. Returns false and leaves the
// platform block releasable on any refusal, so create() can fall through to
// its single cleanup path.
bool SdlPresenter::build_gpu_resources(
    Platform& platform,
    const SdlPresenterConfig& config) noexcept {
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
        auto renderer = render::SceneRenderer::create(
            *platform.device, config.width, config.height);
        if (!renderer) {
            return false;
        }
        platform.scene.emplace(std::move(renderer).value());
    }
    return true;
}

show::PresenterResult SdlPresenter::create(
    const SdlPresenterConfig& config) noexcept {
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
        platform->scene.reset();
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

    // The scene path is the GPU path plus a renderer; asking for one without
    // the other is a configuration mistake rather than something to interpret.
    if (config.scene && !config.gpu) {
        release_video();
        return Result::failure(show::PresenterError::InvalidConfiguration);
    }

    if (config.gpu) {
        // No renderer: SDL_GPU claims the window's surface itself, and
        // creating both would leave two owners of one swapchain.
        platform->window = SDL_CreateWindow(
            config.title.c_str(),
            static_cast<int>(config.width),
            static_cast<int>(config.height),
            window_flags(config));
        if (platform->window == nullptr) {
            return fail(show::PresenterError::DisplayUnavailable);
        }
    } else if (!SDL_CreateWindowAndRenderer(
                   config.title.c_str(),
                   static_cast<int>(config.width),
                   static_cast<int>(config.height),
                   window_flags(config),
                   &platform->window,
                   &platform->renderer)) {
        return fail(show::PresenterError::DisplayUnavailable);
    }

    // Presentation must never pace the show. The frame boundary owns timing,
    // and blocking on the display refresh would beat against it; real output
    // timing comes from genlock, not from the desktop compositor.
    if (platform->renderer != nullptr) {
        static_cast<void>(SDL_SetRenderVSync(platform->renderer, 0));
    }

    if (config.position_x >= 0 && config.position_y >= 0) {
        static_cast<void>(SDL_SetWindowPosition(
            platform->window, config.position_x, config.position_y));
    }

    if (config.gpu) {
        if (!build_gpu_resources(*platform, config)) {
            return fail(show::PresenterError::DisplayUnavailable);
        }
    } else {
        platform->texture = SDL_CreateTexture(
            platform->renderer,
            SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STREAMING,
            static_cast<int>(config.width),
            static_cast<int>(config.height));
        if (platform->texture == nullptr) {
            return fail(show::PresenterError::DisplayUnavailable);
        }
    }
    platform->window_id = SDL_GetWindowID(platform->window);

    std::unique_ptr<Presenter> presenter{new (std::nothrow) SdlPresenter(
        std::move(surface).value(),
        platform,
        config.accepts_commands,
        config.gpu,
        config.scene)};
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
        platform_->scene.reset();
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
    if (command == show::PresenterCommand::None
        || command_size_ == command_capacity) {
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
                if (peer->platform_ != nullptr
                    && peer->platform_->window_id == event.window.windowID) {
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
                if (peer->platform_ == nullptr
                    || peer->platform_->window_id != event.key.windowID) {
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
    return platform_ == nullptr
        ? 0U
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
        // A scene presenter draws into the renderer's own target, not the
        // test-pattern target, so it is that one a self-check must read.
        if (scene_ && platform.scene) {
            return digest_target(*platform.device, platform.scene->target());
        }
        if (!platform.output) {
            return {};
        }
        return platform.output->readback(*platform.device);
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
    if (rgba != nullptr && rgba->pixels != nullptr && rgba->w > 0
        && rgba->h > 0 && rgba->pitch > 0) {
        const auto bytes = static_cast<std::size_t>(rgba->pitch)
            * static_cast<std::size_t>(rgba->h);
        const show::FrameBufferView view{
            std::span<core::u8>{
                static_cast<core::u8*>(rgba->pixels), bytes},
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

show::PresenterStatus SdlPresenter::present(
    const show::TestPatternInput& frame) noexcept {
    pump();
    if (closed_) {
        return show::PresenterStatus::Closed;
    }
    if (platform_ == nullptr) {
        return show::PresenterStatus::Failed;
    }
    return gpu_ ? present_gpu(frame) : present_cpu(frame);
}

show::PresenterStatus SdlPresenter::present_recorded(
    const RecordFrame& record) noexcept {
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
    const rhi::Texture* const presented = record(commands);
    if (presented == nullptr) {
        return show::PresenterStatus::Failed;
    }

    const auto image = platform.swapchain.acquire(commands);
    auto status = show::PresenterStatus::Presented;
    if (image) {
        if (!platform.swapchain.present(
                commands, image, *presented, rhi::Filter::Linear)) {
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

show::PresenterStatus SdlPresenter::present_gpu(
    const show::TestPatternInput& frame) noexcept {
    auto& platform = *platform_;
    if (!platform.output) {
        return show::PresenterStatus::Failed;
    }
    return present_recorded(
        [&platform, &frame](rhi::CommandList& commands) noexcept
        -> const rhi::Texture* {
            if (!platform.output->record(commands, frame)) {
                return nullptr;
            }
            return &platform.output->texture();
        });
}

show::PresenterStatus SdlPresenter::present(
    const mediaserver::scene::Scene& frame,
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
    if (!platform.scene) {
        return show::PresenterStatus::Failed;
    }
    return present_recorded(
        [&platform, &frame, slice](rhi::CommandList& commands) noexcept
        -> const rhi::Texture* {
            // The renderer draws this node's slice of the shared canvas into
            // its own colour target; that target is what the swapchain blits.
            if (!platform.scene->render(commands, frame, slice)) {
                return nullptr;
            }
            return &platform.scene->target();
        });
}

show::PresenterStatus SdlPresenter::present_cpu(
    const show::TestPatternInput& frame) noexcept {
    if (!show::render_test_pattern(surface_.view(), frame)) {
        return show::PresenterStatus::Failed;
    }

    if (!SDL_UpdateTexture(
            platform_->texture,
            nullptr,
            surface_.pixels(),
            static_cast<int>(surface_.stride()))) {
        return show::PresenterStatus::Failed;
    }
    if (!SDL_RenderClear(platform_->renderer)
        || !SDL_RenderTexture(
            platform_->renderer, platform_->texture, nullptr, nullptr)
        || !SDL_RenderPresent(platform_->renderer)) {
        return show::PresenterStatus::Failed;
    }
    ++timing_.frames;
    return show::PresenterStatus::Presented;
}

} // namespace mediaserver::ui
