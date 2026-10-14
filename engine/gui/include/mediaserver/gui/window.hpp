#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/gui/ui_renderer.hpp>
#include <mediaserver/rhi/device.hpp>

#include <memory>
#include <string>

// The window the bespoke UI lives in: an SDL3 window claimed by the RHI, an
// event pump that fills an InputState, and a present path that draws a frame's
// UiDrawList and puts it on screen.
//
// This is the only file in the GUI module that knows a window toolkit exists,
// and it does not say so in its interface - SDL appears nowhere below. That is
// what keeps the widget and layout layers headless: they are driven by an
// InputState a test can fill by hand just as well as this pump can.
//
// The UI draws into an offscreen colour target and the swapchain blits that
// onto the acquired image, the same shape the output path uses. The extra blit
// buys a target whose format the pipeline is built against once, independent of
// whatever the platform hands back for the window.
namespace mediaserver::gui {

enum class WindowError : core::u8 {
    // No GPU this build can drive - the same first-class outcome the RHI
    // reports, so a machine without one fails clearly rather than crashing.
    Unavailable,
    // SDL has no video subsystem, or the window itself could not be made.
    DisplayUnavailable,
    AllocationFailed,
    // The backend refused something it had accepted the inputs for.
    BackendFailure,
};

struct WindowConfig final {
    std::string title{"Media Server"};
    core::u32 width{1600U};
    core::u32 height{900U};
    // Negative leaves placement to the window manager. The operator editor
    // uses this to sit beside a physical output during development.
    core::i32 position_x{-1};
    core::i32 position_y{-1};
    // Read width and height as logical units: the window opens at that size
    // times the display's content scale, so a small fixed-layout window is the
    // same size to the eye on a 100% and a 150% display. Off, they are
    // physical pixels - what a restored workspace asks for.
    bool logical_size{};
    // The operator UI is not an output: nothing downstream is paced by it, and
    // an idle editor should not spin the GPU. So it waits for the display,
    // which is the opposite of what an output node asks for.
    bool vsync{true};
};

// Applies one SDL event to `input`. `sdl_event` is an SDL_Event*, declared as
// void* so this header stays free of the platform layer - the same trick
// rhi::Device::claim_window uses for the window handle.
//
// Returns false when the event asks the window to close, so the pump can stop.
// Exposed rather than hidden inside the pump because event translation is the
// one part of windowing worth testing on its own: a test can synthesize events
// and assert what the widget layer would see, with no display anywhere.
//
// `to_logical` converts the event's window coordinates into the UI's logical
// units; the pump passes the inverse of the UI scale.
[[nodiscard]] bool translate_event(
    const void* sdl_event, InputState& input, float to_logical = 1.0F);
// Operator-invoked copy; no clipboard work occurs during ordinary frames.
[[nodiscard]] bool copy_text_to_clipboard(const std::string& text);

class UiWindow final {
public:
    [[nodiscard]] static core::Result<UiWindow, WindowError> create(
        const WindowConfig& config);

    UiWindow(const UiWindow&) = delete;
    UiWindow& operator=(const UiWindow&) = delete;
    UiWindow(UiWindow&&) noexcept;
    UiWindow& operator=(UiWindow&&) noexcept;
    ~UiWindow();

    // Drains the OS queue into `input`, calling InputState::new_frame() first
    // so this frame's edges read against the last. Returns false once the
    // operator has asked to close the window.
    [[nodiscard]] bool pump(InputState& input);

    // A close request only latches this wrapper; SDL has not destroyed the
    // native window yet. An editor that asks about unsaved work can clear that
    // latch when the operator cancels the close.
    void cancel_close_request() noexcept;

    // The drawable size in logical units, which is the UI's coordinate space:
    // the pixel size divided by ui_scale(). Pointer positions arrive in the
    // same units, so a widget never sees a physical pixel.
    [[nodiscard]] core::u32 width() const noexcept;
    [[nodiscard]] core::u32 height() const noexcept;
    // The drawable size in physical pixels - what the window was created at,
    // and what a restored workspace asks for again.
    [[nodiscard]] core::u32 pixel_width() const noexcept;
    [[nodiscard]] core::u32 pixel_height() const noexcept;

    // Physical pixels per logical unit: the display's content scale (150% on
    // a typical 4K desktop) times the operator's own zoom. Polled every pump,
    // so dragging the window to another monitor, or changing the Windows
    // setting, takes effect on the next frame. The draw list must be set to
    // this, and the font atlas baked for it.
    [[nodiscard]] float ui_scale() const noexcept;
    // What the operating system asks for on the window's current display.
    [[nodiscard]] float display_scale() const noexcept;
    // A factor on top of the display's own scale, for an operator who wants
    // the whole interface larger or smaller than the desktop default. Clamped
    // so the editor can never be zoomed into something unusable.
    void set_zoom(float zoom) noexcept;
    [[nodiscard]] float zoom() const noexcept;

    // Shows `cursor` until the next call. Cheap to call every frame; the
    // platform cursor only changes when the shape does.
    void set_cursor(Cursor cursor) noexcept;

    // Hides the cursor and holds it in place while raw motion keeps arriving,
    // for a camera drag that must not stop at the edge of the screen. Unlocking
    // puts the cursor back where the lock began. Cheap to call every frame.
    void set_pointer_locked(bool locked) noexcept;

    // Seconds since the window was created. Feeds UiContext::new_frame(), which
    // uses it for the caret blink.
    [[nodiscard]] double time_seconds() const noexcept;

    // Hands the glyph atlas to the renderer. Call once after create(); calling
    // it again re-uploads, which is what a re-baked atlas needs.
    [[nodiscard]] bool set_atlas(const FontAtlas& atlas);

    // Draws `list` over `clear` and presents it. Returns true when the frame
    // reached the screen; false is a skipped present - a minimised or occluded
    // window - not a failure the caller must handle.
    [[nodiscard]] bool present(const UiDrawList& list, rhi::Color clear);

    // The device the UI shares with the engine, so a viewport panel can be a
    // texture the scene renderer already drew into rather than a copy.
    [[nodiscard]] rhi::Device& device() noexcept;

private:
    struct Impl;
    explicit UiWindow(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace mediaserver::gui
