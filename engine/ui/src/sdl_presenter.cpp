#include <mediaserver/ui/sdl_presenter.hpp>

#include <algorithm>
#include <mutex>
#include <new>
#include <span>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

namespace mediaserver::ui {

struct SdlPresenter::Platform final {
    SDL_Window* window{};
    SDL_Renderer* renderer{};
    SDL_Texture* texture{};
    SDL_WindowID window_id{};
};

namespace {

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

} // namespace

SdlPresenter::SdlPresenter(
    show::PresenterSurface surface,
    Platform* platform,
    bool accepts_commands) noexcept
    : surface_(std::move(surface)),
      platform_(platform),
      accepts_commands_(accepts_commands) {}

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

    if (!SDL_CreateWindowAndRenderer(
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
    static_cast<void>(SDL_SetRenderVSync(platform->renderer, 0));

    if (config.position_x >= 0 && config.position_y >= 0) {
        static_cast<void>(SDL_SetWindowPosition(
            platform->window, config.position_x, config.position_y));
    }

    platform->texture = SDL_CreateTexture(
        platform->renderer,
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STREAMING,
        static_cast<int>(config.width),
        static_cast<int>(config.height));
    if (platform->texture == nullptr) {
        return fail(show::PresenterError::DisplayUnavailable);
    }
    platform->window_id = SDL_GetWindowID(platform->window);

    std::unique_ptr<Presenter> presenter{new (std::nothrow) SdlPresenter(
        std::move(surface).value(), platform, config.accepts_commands)};
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
    return show::PresenterStatus::Presented;
}

} // namespace mediaserver::ui
