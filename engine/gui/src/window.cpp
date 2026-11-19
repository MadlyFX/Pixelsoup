#include <mediaserver/gui/window.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

namespace mediaserver::gui {
namespace {

// Build each native tool cursor once from the same vector mark as its toolbar
// button. A white outline keeps the black mark legible over any clip image.
[[nodiscard]] SDL_Cursor* create_tool_cursor(Icon icon) {
    constexpr int size = 32;
    UiDrawList glyph;
    draw_icon(glyph, icon, 16.0F, 16.0F, UiColor{0.0F, 0.0F, 0.0F, 1.0F}, 1.35F);
    std::array<bool, size * size> ink{};
    const auto& vertices = glyph.vertices();
    const auto edge = [](const UiVertex& a, const UiVertex& b, float x, float y) {
        return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
    };
    for (std::size_t i = 0; i + 2U < vertices.size(); i += 3U) {
        const auto& a = vertices[i];
        const auto& b = vertices[i + 1U];
        const auto& c = vertices[i + 2U];
        if (edge(a, b, c.x, c.y) == 0.0F) continue;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const float px = static_cast<float>(x) + 0.5F;
                const float py = static_cast<float>(y) + 0.5F;
                const float ab = edge(a, b, px, py);
                const float bc = edge(b, c, px, py);
                const float ca = edge(c, a, px, py);
                if ((ab >= 0 && bc >= 0 && ca >= 0) ||
                    (ab <= 0 && bc <= 0 && ca <= 0)) {
                    ink[static_cast<std::size_t>(y * size + x)] = true;
                }
            }
        }
    }
    std::array<Uint8, size * size / 8> data{};
    std::array<Uint8, size * size / 8> mask{};
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const auto index = static_cast<std::size_t>(y * size + x);
            const auto bit = static_cast<Uint8>(0x80U >> (x % 8));
            if (ink[index]) data[index / 8U] |= bit;
            bool covered = false;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx >= 0 && nx < size && ny >= 0 && ny < size &&
                        ink[static_cast<std::size_t>(ny * size + nx)]) covered = true;
                }
            }
            if (covered) mask[index / 8U] |= bit;
        }
    }
    return SDL_CreateCursor(data.data(), mask.data(), size, size, 16, 16);
}

[[nodiscard]] std::optional<MouseButton> button_of(Uint8 sdl_button) noexcept {
    switch (sdl_button) {
    case SDL_BUTTON_LEFT:
        return MouseButton::Left;
    case SDL_BUTTON_MIDDLE:
        return MouseButton::Middle;
    case SDL_BUTTON_RIGHT:
        return MouseButton::Right;
    default:
        return std::nullopt;
    }
}

// Only the keys the UI acts on directly. Everything printable arrives as text
// input instead, which is what makes the editing path layout-independent: the
// operator's keyboard layout is SDL's problem, not the widget layer's.
[[nodiscard]] std::optional<Key> key_of(
    SDL_Keycode code, SDL_Keymod modifiers) noexcept {
    if ((modifiers & SDL_KMOD_CTRL) != 0U) {
        if (code == SDLK_RETURN || code == SDLK_KP_ENTER) {
            return Key::Take;
        }
        if (code == SDLK_Z) {
            return (modifiers & SDL_KMOD_SHIFT) != 0U ? Key::Redo : Key::Undo;
        }
        if (code == SDLK_Y) {
            return Key::Redo;
        }
        if (code == SDLK_C) {
            return Key::Copy;
        }
        if (code == SDLK_V) {
            return Key::Paste;
        }
        if (code == SDLK_A) {
            return Key::SelectAll;
        }
        if (code == SDLK_K || code == SDLK_B) {
            return Key::Split;
        }
        if (code == SDLK_D) {
            return Key::Duplicate;
        }
        if (code == SDLK_S) {
            return Key::Save;
        }
        if (code == SDLK_EQUALS || code == SDLK_PLUS || code == SDLK_KP_PLUS) {
            return Key::ZoomIn;
        }
        if (code == SDLK_MINUS || code == SDLK_KP_MINUS) {
            return Key::ZoomOut;
        }
        if (code == SDLK_0 || code == SDLK_KP_0) {
            return Key::ZoomReset;
        }
        if (code >= SDLK_1 && code <= SDLK_9) {
            return static_cast<Key>(static_cast<core::u8>(Key::SaveView1) +
                                    static_cast<core::u8>(code - SDLK_1));
        }
    }
    // Alt+G / Alt+R / Alt+S clear location, rotation and scale, as in
    // Blender, Alt+P / J / H / K choose a Stage view's projection and
    // Alt+4 / 3 / 6 / 2 its shading. Alt with a letter or digit produces no
    // text, so these arrive as keys and never reach a digit's view recall.
    // Ctrl excluded: AltGr arrives as Ctrl+Alt on Windows layouts.
    if ((modifiers & SDL_KMOD_ALT) != 0U &&
        (modifiers & SDL_KMOD_CTRL) == 0U) {
        if (code == SDLK_G) {
            return Key::ClearLocation;
        }
        if (code == SDLK_R) {
            return Key::ClearRotation;
        }
        if (code == SDLK_S) {
            return Key::ClearScale;
        }
        if (code == SDLK_P) {
            return Key::ViewPerspective;
        }
        if (code == SDLK_J) {
            return Key::ViewTop;
        }
        if (code == SDLK_H) {
            return Key::ViewFront;
        }
        if (code == SDLK_K) {
            return Key::ViewSide;
        }
        if (code == SDLK_4) {
            return Key::ShadeLit;
        }
        if (code == SDLK_3) {
            return Key::ShadeUnlit;
        }
        if (code == SDLK_6) {
            return Key::ShadeLightsOnly;
        }
        if (code == SDLK_2) {
            return Key::ShadeWireframe;
        }
        if (code == SDLK_Q) {
            return Key::TrimHeadToPlayhead;
        }
        if (code == SDLK_W) {
            return Key::TrimTailToPlayhead;
        }
    }
    switch (code) {
    case SDLK_BACKSPACE:
        return Key::Backspace;
    case SDLK_DELETE:
        return Key::Delete;
    case SDLK_LEFT:
        return Key::Left;
    case SDLK_RIGHT:
        return Key::Right;
    case SDLK_UP:
        return Key::Up;
    case SDLK_DOWN:
        return Key::Down;
    case SDLK_HOME:
        return Key::Home;
    case SDLK_END:
        return Key::End;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        return Key::Enter;
    case SDLK_TAB:
        return Key::Tab;
    case SDLK_ESCAPE:
        return Key::Escape;
    case SDLK_F2:
        return Key::Rename;
    case SDLK_F9:
        return Key::EasyEase;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<MoveKey> move_key_of(SDL_Scancode code) noexcept {
    switch (code) {
    case SDL_SCANCODE_W:
        return MoveKey::W;
    case SDL_SCANCODE_A:
        return MoveKey::A;
    case SDL_SCANCODE_S:
        return MoveKey::S;
    case SDL_SCANCODE_D:
        return MoveKey::D;
    case SDL_SCANCODE_Q:
        return MoveKey::Q;
    case SDL_SCANCODE_E:
        return MoveKey::E;
    case SDL_SCANCODE_V:
        return MoveKey::V;
    case SDL_SCANCODE_L:
        return MoveKey::L;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] KeyMods mods_of(SDL_Keymod mod) noexcept {
    KeyMods mods = KeyMods::None;
    if ((mod & SDL_KMOD_SHIFT) != 0U) {
        mods = mods | KeyMods::Shift;
    }
    if ((mod & SDL_KMOD_CTRL) != 0U) {
        mods = mods | KeyMods::Ctrl;
    }
    if ((mod & SDL_KMOD_ALT) != 0U) {
        mods = mods | KeyMods::Alt;
    }
    return mods;
}

} // namespace

bool translate_event(
    const void* sdl_event, InputState& input, float to_logical) {
    if (sdl_event == nullptr) {
        return true;
    }
    const auto& event = *static_cast<const SDL_Event*>(sdl_event);
    switch (event.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        return false;

    case SDL_EVENT_MOUSE_MOTION:
        input.set_mouse_position(
            event.motion.x * to_logical, event.motion.y * to_logical);
        input.add_mouse_motion(
            event.motion.xrel * to_logical, event.motion.yrel * to_logical);
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        // The position rides along with the button event, so a click that
        // arrives without a preceding motion still lands where it was aimed.
        input.set_mouse_position(
            event.button.x * to_logical, event.button.y * to_logical);
        const auto button = button_of(event.button.button);
        if (button.has_value()) {
            input.set_mouse_button(
                button.value(), event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        break;
    }

    case SDL_EVENT_MOUSE_WHEEL:
        input.add_wheel(event.wheel.y);
        break;

    case SDL_EVENT_TEXT_INPUT:
        if (event.text.text != nullptr) {
            input.add_text(event.text.text);
        }
        break;

    case SDL_EVENT_KEY_DOWN: {
        input.set_mods(mods_of(event.key.mod));
        if (const auto move = move_key_of(event.key.scancode)) {
            input.set_move_key(move.value(), true);
        }
        const auto key = key_of(event.key.key, event.key.mod);
        if (key.has_value()) {
            // Repeats are kept: holding Backspace should keep deleting.
            input.add_key(key.value());
        }
        if (key == Key::Paste) {
            // The clipboard arrives as ordinary typed text.
            //
            // A text field already inserts whatever `text()` holds at its
            // caret, so routing the paste through that path makes every field
            // in the application pasteable without any of them knowing what a
            // clipboard is - and keeps the widget layer free of the platform,
            // which is the reason it never handled Paste itself.
            //
            // The key still goes through as well: a paste with no field
            // focused is a document-level action, and the shell's global
            // shortcut stands down while anything is typing.
            char* const text = SDL_GetClipboardText();
            if (text != nullptr) {
                if (text[0] != 0) {
                    input.add_text(text);
                }
                SDL_free(text);
            }
        }
        break;
    }

    case SDL_EVENT_KEY_UP:
        input.set_mods(mods_of(event.key.mod));
        if (const auto move = move_key_of(event.key.scancode)) {
            input.set_move_key(move.value(), false);
        }
        break;

    case SDL_EVENT_DROP_BEGIN:
    case SDL_EVENT_DROP_POSITION:
        input.set_file_drag(true, event.drop.x * to_logical,
                            event.drop.y * to_logical);
        break;

    case SDL_EVENT_DROP_FILE:
        if (event.drop.data != nullptr && event.drop.data[0] != '\0') {
            input.add_dropped_file(event.drop.data, event.drop.x * to_logical,
                                   event.drop.y * to_logical);
        }
        break;

    case SDL_EVENT_DROP_COMPLETE:
        input.set_file_drag(false, event.drop.x * to_logical,
                            event.drop.y * to_logical);
        break;

    case SDL_EVENT_WINDOW_FOCUS_LOST:
        // The key and button ups go to whichever window has focus now, so
        // anything held here would otherwise stay held forever.
        input.release_held();
        break;

    default:
        break;
    }
    return true;
}

struct UiWindow::Impl final {
    // Declared before the resources that reference it so it outlives them:
    // members are destroyed in reverse order, and the teardown below leans on
    // that as well as doing the window/device dance explicitly.
    std::optional<rhi::Device> device;
    std::optional<rhi::Swapchain> swapchain;
    std::optional<UiRenderer> renderer;
    rhi::Texture target;
    // Textures the caller asked for (UiWindow::upload_texture), uploaded on
    // the next present. After the device, so they go before it.
    struct Extra final {
        std::unique_ptr<rhi::Texture> texture;
        std::vector<core::u8> pixels;
        bool uploaded{};
        bool dead{};
    };
    std::vector<Extra> extras;

    SDL_Window* window{};
    bool sdl_video{};
    // Physical drawable size, and the two factors that turn it into logical
    // units: what the display asks for and what the operator added.
    core::u32 width{};
    core::u32 height{};
    float display_scale{1.0F};
    float zoom{1.0F};
    // Window coordinates per physical pixel. One on Windows, where SDL works
    // in pixels; below one where the platform reports points.
    float pixel_density{1.0F};
    bool closed{};
    std::function<void(const void*)> event_tap;

    std::array<SDL_Cursor*, static_cast<std::size_t>(Cursor::Count)> cursors{};
    Cursor cursor{Cursor::Arrow};
    // Relative mouse mode, and where the cursor was when it began so it can
    // be put back there when it ends.
    bool pointer_locked{};
    float lock_x{};
    float lock_y{};

    // The atlas is copied rather than borrowed: uploading needs a command list,
    // so it happens on the next present, by which time the caller's FontAtlas
    // may be long gone.
    std::vector<core::u8> atlas_pixels;
    core::u32 atlas_width{};
    core::u32 atlas_height{};
    bool atlas_pending{};
    bool atlas_uploaded{};

    Uint64 start_ns{};

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() {
        if (device) device->wait_idle();
        // Order is load-bearing: the swapchain hands the window back to the
        // device, so both must still exist when it goes.
        target = rhi::Texture{};
        renderer.reset();
        swapchain.reset();
        for (SDL_Cursor*& shape : cursors) {
            if (shape != nullptr) {
                SDL_DestroyCursor(shape);
                shape = nullptr;
            }
        }
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
        device.reset();
        if (sdl_video) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }

    [[nodiscard]] float ui_scale() const noexcept {
        return display_scale * zoom;
    }

    // Re-reads the drawable size and the display's scale. Polled rather than
    // tracked through events: one query a frame is cheaper than getting the
    // event set right on every platform.
    void refresh_metrics() noexcept {
        int pixel_width = 0;
        int pixel_height = 0;
        static_cast<void>(
            SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height));
        width = static_cast<core::u32>(std::max(pixel_width, 0));
        height = static_cast<core::u32>(std::max(pixel_height, 0));
        const float reported = SDL_GetWindowDisplayScale(window);
        display_scale = std::isfinite(reported) && reported > 0.0F
            ? std::clamp(reported, 0.5F, 4.0F) : 1.0F;
        const float density = SDL_GetWindowPixelDensity(window);
        pixel_density = std::isfinite(density) && density > 0.0F ? density : 1.0F;
    }

    // Keeps the offscreen target the size of the drawable, remaking it when the
    // operator resizes the window.
    [[nodiscard]] bool ensure_target() {
        if (width == 0U || height == 0U) {
            return false;
        }
        if (target && target.width() == width && target.height() == height) {
            return true;
        }
        rhi::TextureDesc desc{};
        desc.width = width;
        desc.height = height;
        desc.format = rhi::TextureFormat::Rgba8Unorm;
        desc.color_target = true;
        // Sampled as well as drawn into: presenting blits this onto the
        // acquired image, and a blit reads its source through a sampler.
        desc.sampled = true;
        auto created = device->create_texture(desc);
        if (!created) {
            return false;
        }
        target = std::move(created).value();
        return true;
    }
};

UiWindow::UiWindow(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

UiWindow::UiWindow(UiWindow&&) noexcept = default;
UiWindow& UiWindow::operator=(UiWindow&&) noexcept = default;
UiWindow::~UiWindow() = default;

core::Result<UiWindow, WindowError> UiWindow::create(const WindowConfig& config) {
    using Result = core::Result<UiWindow, WindowError>;

    if (!UiRenderer::shaders_available()) {
        return Result::failure(WindowError::Unavailable);
    }
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        return Result::failure(WindowError::DisplayUnavailable);
    }

    auto impl = std::make_unique<Impl>();
    impl->sdl_video = true;

    // No high-density flag: on Windows the window works in physical pixels
    // either way, and the UI does its own scaling from the display's content
    // scale (see ui_scale()), which is what keeps text crisp at 150%.
    impl->window = SDL_CreateWindow(
        config.title.c_str(),
        static_cast<int>(config.width),
        static_cast<int>(config.height),
        SDL_WINDOW_RESIZABLE);
    if (impl->window == nullptr) {
        return Result::failure(WindowError::DisplayUnavailable);
    }
    if (config.logical_size) {
        const float scale = SDL_GetWindowDisplayScale(impl->window);
        if (std::isfinite(scale) && scale > 0.0F && scale != 1.0F) {
            const float clamped = std::clamp(scale, 0.5F, 4.0F);
            static_cast<void>(SDL_SetWindowSize(
                impl->window,
                static_cast<int>(std::lround(
                    static_cast<float>(config.width) * clamped)),
                static_cast<int>(std::lround(
                    static_cast<float>(config.height) * clamped))));
        }
    }
    if (config.position_x >= 0 && config.position_y >= 0) {
        static_cast<void>(SDL_SetWindowPosition(
            impl->window, config.position_x, config.position_y));
    }

    const rhi::PresentMode present_mode = config.vsync
        ? rhi::PresentMode::Vsync : rhi::PresentMode::Immediate;
    const auto open_backend = [&](rhi::BackendKind backend) {
        rhi::DeviceConfig device_config{};
        device_config.backend = backend;
        device_config.debug = config.debug;
        // An editor frame records the offscreen Program/Stage work and the UI
        // presentation as separate submissions on this same device. Keep two
        // complete frame pairs available on native D3D12; SDL_GPU exposes at
        // most three contexts, which still leaves one context beyond the two
        // submissions in the current frame. Acquisition remains non-blocking.
        device_config.frames_in_flight =
            backend == rhi::BackendKind::D3d12 ? 4U : 3U;
        auto device = rhi::Device::create(device_config);
        if (!device) {
            return false;
        }
        // Swapchain retains the owning Device address, so establish that
        // address before creating any resource from it.
        impl->device.emplace(std::move(device).value());
        auto swapchain = impl->device->claim_window(
            impl->window, present_mode);
        if (!swapchain) {
            impl->device.reset();
            return false;
        }
        impl->swapchain.emplace(std::move(swapchain).value());
        return true;
    };
    // The editor prefers the native renderer-capable device. A machine that
    // cannot create its D3D12 window remains a fully usable Basic editor on
    // SDL; renderer readiness still reports which path is actually active.
    if (!open_backend(rhi::BackendKind::D3d12) &&
        !open_backend(rhi::BackendKind::SdlGpu)) {
        return Result::failure(WindowError::DisplayUnavailable);
    }

    auto renderer =
        UiRenderer::create(*impl->device, rhi::TextureFormat::Rgba8Unorm);
    if (!renderer) {
        return Result::failure(WindowError::BackendFailure);
    }
    impl->renderer.emplace(std::move(renderer).value());

    // Without this SDL sends no text input events at all, and every text field
    // would silently ignore the keyboard.
    static_cast<void>(SDL_StartTextInput(impl->window));

    impl->refresh_metrics();
    impl->start_ns = SDL_GetTicksNS();

    return Result::success(UiWindow{std::move(impl)});
}

bool UiWindow::pump(InputState& input) {
    input.new_frame();
    input.set_pointer_locked(impl_->pointer_locked);

    // Events are in window coordinates; the UI wants logical units. Read the
    // scale first so a frame's pointer and its layout agree on one.
    impl_->refresh_metrics();
    const float to_logical = impl_->pixel_density / impl_->ui_scale();
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        if (impl_->event_tap) {
            impl_->event_tap(&event);
        }
        if (!translate_event(&event, input, to_logical)) {
            impl_->closed = true;
        }
    }
    impl_->refresh_metrics();

    return !impl_->closed;
}

void UiWindow::set_event_tap(std::function<void(const void*)> tap) {
    impl_->event_tap = std::move(tap);
}

void* UiWindow::native_window() const noexcept {
    return impl_->window;
}

void UiWindow::cancel_close_request() noexcept {
    impl_->closed = false;
}

core::u32 UiWindow::width() const noexcept {
    return static_cast<core::u32>(
        std::floor(static_cast<float>(impl_->width) / impl_->ui_scale()));
}

core::u32 UiWindow::height() const noexcept {
    return static_cast<core::u32>(
        std::floor(static_cast<float>(impl_->height) / impl_->ui_scale()));
}

core::u32 UiWindow::pixel_width() const noexcept {
    return impl_->width;
}

core::u32 UiWindow::pixel_height() const noexcept {
    return impl_->height;
}

float UiWindow::ui_scale() const noexcept {
    return impl_->ui_scale();
}

float UiWindow::display_scale() const noexcept {
    return impl_->display_scale;
}

void UiWindow::set_zoom(float zoom) noexcept {
    impl_->zoom = std::isfinite(zoom) ? std::clamp(zoom, 0.5F, 3.0F) : 1.0F;
}

float UiWindow::zoom() const noexcept {
    return impl_->zoom;
}

void UiWindow::set_cursor(Cursor cursor) noexcept {
    if (cursor == impl_->cursor) {
        return;
    }
    const auto index = static_cast<std::size_t>(cursor);
    if (index >= impl_->cursors.size()) {
        return;
    }
    SDL_Cursor*& shape = impl_->cursors[index];
    if (shape == nullptr) {
        SDL_SystemCursor system = SDL_SYSTEM_CURSOR_DEFAULT;
        switch (cursor) {
        case Cursor::Arrow:
            system = SDL_SYSTEM_CURSOR_DEFAULT;
            break;
        case Cursor::Hand:
            system = SDL_SYSTEM_CURSOR_POINTER;
            break;
        case Cursor::Text:
            system = SDL_SYSTEM_CURSOR_TEXT;
            break;
        case Cursor::ResizeHorizontal:
            system = SDL_SYSTEM_CURSOR_EW_RESIZE;
            break;
        case Cursor::ResizeVertical:
            system = SDL_SYSTEM_CURSOR_NS_RESIZE;
            break;
        case Cursor::ResizeDiagonal:
            system = SDL_SYSTEM_CURSOR_NWSE_RESIZE;
            break;
        case Cursor::Move:
            system = SDL_SYSTEM_CURSOR_MOVE;
            break;
        case Cursor::RippleTrim: shape = create_tool_cursor(Icon::Ripple); break;
        case Cursor::Roll: shape = create_tool_cursor(Icon::Roll); break;
        case Cursor::Slip: shape = create_tool_cursor(Icon::Slip); break;
        case Cursor::Slide: shape = create_tool_cursor(Icon::Slide); break;
        case Cursor::Razor: shape = create_tool_cursor(Icon::Razor); break;
        case Cursor::Hidden: {
            // Every bit clear in both data and mask: a cursor with no pixels.
            constexpr int size = 8;
            const std::array<Uint8, size * size / 8> blank{};
            shape = SDL_CreateCursor(blank.data(), blank.data(), size, size,
                                     0, 0);
            break;
        }
        case Cursor::Count: return;
        }
        if (shape == nullptr) shape = SDL_CreateSystemCursor(system);
    }
    if (shape != nullptr && SDL_SetCursor(shape)) {
        impl_->cursor = cursor;
    }
}

void UiWindow::set_pointer_locked(bool locked) noexcept {
    if (locked == impl_->pointer_locked || impl_->window == nullptr) {
        return;
    }
    if (locked) {
        static_cast<void>(SDL_GetMouseState(&impl_->lock_x, &impl_->lock_y));
        if (!SDL_SetWindowRelativeMouseMode(impl_->window, true)) {
            return;
        }
    } else {
        SDL_WarpMouseInWindow(impl_->window, impl_->lock_x, impl_->lock_y);
        static_cast<void>(SDL_SetWindowRelativeMouseMode(impl_->window, false));
    }
    impl_->pointer_locked = locked;
}

double UiWindow::time_seconds() const noexcept {
    const Uint64 now = SDL_GetTicksNS();
    return static_cast<double>(now - impl_->start_ns) / 1'000'000'000.0;
}

bool UiWindow::set_atlas(const FontAtlas& atlas) {
    const auto pixels = atlas.pixels();
    if (pixels.empty() || atlas.width() == 0U || atlas.height() == 0U) {
        return false;
    }
    impl_->atlas_pixels.assign(pixels.begin(), pixels.end());
    impl_->atlas_width = atlas.width();
    impl_->atlas_height = atlas.height();
    impl_->atlas_pending = true;
    return true;
}

const rhi::Texture* UiWindow::upload_texture(std::span<const core::u8> rgba, core::u32 width, core::u32 height) {
    if (rgba.size() != static_cast<std::size_t>(width) * height * 4U || width == 0U || height == 0U) {
        return nullptr;
    }
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.sampled = true;
    auto created = impl_->device->create_texture(desc);
    if (!created) {
        return nullptr;
    }
    UiWindow::Impl::Extra extra;
    extra.texture = std::make_unique<rhi::Texture>(std::move(created).value());
    extra.pixels.assign(rgba.begin(), rgba.end());
    impl_->extras.push_back(std::move(extra));
    return impl_->extras.back().texture.get();
}

void UiWindow::release_texture(const rhi::Texture* texture) {
    for (auto& extra : impl_->extras) {
        if (extra.texture.get() == texture) {
            extra.dead = true;
        }
    }
}

bool UiWindow::present(const UiDrawList& list, rhi::Color clear) {
    if (!impl_->ensure_target()) {
        return false;
    }
    if (std::any_of(impl_->extras.begin(), impl_->extras.end(), [](const auto& e) { return e.dead; })) {
        impl_->device->wait_idle();
        std::erase_if(impl_->extras, [](const auto& e) { return e.dead; });
    }
    if (impl_->atlas_pending && impl_->atlas_uploaded) {
        // A re-bake (the scale changed) replaces a texture earlier frames may
        // still be sampling. Rare enough to drain for rather than track.
        impl_->device->wait_idle();
    }

    auto acquired = impl_->device->acquire_commands();
    if (!acquired) {
        return false;
    }
    auto commands = std::move(acquired).value();

    if (impl_->atlas_pending) {
        impl_->atlas_pending = !impl_->renderer->upload_atlas(
            *impl_->device,
            commands,
            impl_->atlas_pixels,
            impl_->atlas_width,
            impl_->atlas_height);
        impl_->atlas_uploaded = impl_->atlas_uploaded || !impl_->atlas_pending;
    }

    for (auto& extra : impl_->extras) {
        if (!extra.uploaded && commands.upload(*extra.texture, extra.pixels)) {
            extra.uploaded = true;
            extra.pixels.clear();
            extra.pixels.shrink_to_fit();
        }
    }

    bool presented = false;
    if (impl_->renderer->render(
            *impl_->device,
            commands,
            impl_->target,
            clear,
            list,
            impl_->width,
            impl_->height)) {
        // An empty image is the documented "window is not showing" outcome -
        // minimised or occluded - so the list is still submitted and the frame
        // is simply skipped.
        const rhi::SwapchainImage image = impl_->swapchain->acquire(commands);
        if (image) {
            presented = impl_->swapchain->present(
                commands, image, impl_->target, rhi::Filter::Nearest);
        }
    }

    // A command list cannot be re-submitted or abandoned, so it goes either
    // way, whatever happened above.
    auto submitted = impl_->device->submit(std::move(commands));
    return presented && submitted.has_value();
}

rhi::Device& UiWindow::device() noexcept {
    return *impl_->device;
}

} // namespace mediaserver::gui
