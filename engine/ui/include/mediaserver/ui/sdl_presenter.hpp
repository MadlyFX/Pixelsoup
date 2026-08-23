#pragma once

#include <array>
#include <cstddef>
#include <string>

#include <mediaserver/core/types.hpp>
#include <mediaserver/show/presenter.hpp>

namespace mediaserver::ui {

struct SdlPresenterConfig final {
    std::string title{"media-server"};
    core::u32 width{960U};
    core::u32 height{540U};
    // Window placement, so a Director preview and its node outputs can be
    // arranged without a window manager.
    core::i32 position_x{-1};
    core::i32 position_y{-1};
    bool fullscreen{};
    bool borderless{};
    bool resizable{true};
    // Only an operator surface reads the keyboard. A node output window is
    // deliberately input-inert: what it shows is replicated state.
    bool accepts_commands{};
};

// SDL3 window output for the playback shell.
//
// The pattern is rasterised into a CPU surface and uploaded to a streaming
// texture each frame. That is deliberately the simple path: it keeps the
// presented pixels readable for a digest, it is the CPU fallback the plan
// calls for, and it leaves an obvious seam where the Vulkan presentation
// replaces the upload without touching the clock or the boundary driver.
//
// SDL is initialised on first use and shut down when the last presenter is
// destroyed, so a headless role never initialises a video subsystem.
class SdlPresenter final : public show::Presenter {
public:
    [[nodiscard]] static show::PresenterResult create(
        const SdlPresenterConfig& config) noexcept;

    ~SdlPresenter() override;

    [[nodiscard]] show::PresenterStatus present(
        const show::TestPatternInput& frame) noexcept override;
    [[nodiscard]] const show::FrameBufferView& surface() const noexcept override {
        return surface_.view();
    }
    [[nodiscard]] show::PresenterCommand take_command() noexcept override;
    [[nodiscard]] bool windowed() const noexcept override { return true; }
    [[nodiscard]] show::PresenterReadback readback() noexcept override;

    // Platform window identifier. Exposed so a diagnostic or test can address
    // this exact window on the shared event queue.
    [[nodiscard]] core::u32 window_id() const noexcept;

private:
    struct Platform;

    SdlPresenter(
        show::PresenterSurface surface,
        Platform* platform,
        bool accepts_commands) noexcept;

    void pump() noexcept;
    [[nodiscard]] bool enqueue(show::PresenterCommand command) noexcept;

    static constexpr std::size_t command_capacity = 16U;

    show::PresenterSurface surface_;
    Platform* platform_{};
    bool accepts_commands_{};
    bool closed_{};
    std::array<show::PresenterCommand, command_capacity> commands_{};
    std::size_t command_head_{};
    std::size_t command_size_{};
};

} // namespace mediaserver::ui
