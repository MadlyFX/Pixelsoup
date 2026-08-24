#include <mediaserver/gui/window.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <optional>
#include <vector>

namespace mediaserver::gui {
namespace {

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
[[nodiscard]] std::optional<Key> key_of(SDL_Keycode code) noexcept {
    switch (code) {
    case SDLK_BACKSPACE:
        return Key::Backspace;
    case SDLK_DELETE:
        return Key::Delete;
    case SDLK_LEFT:
        return Key::Left;
    case SDLK_RIGHT:
        return Key::Right;
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

bool translate_event(const void* sdl_event, InputState& input) {
    if (sdl_event == nullptr) {
        return true;
    }
    const auto& event = *static_cast<const SDL_Event*>(sdl_event);
    switch (event.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        return false;

    case SDL_EVENT_MOUSE_MOTION:
        input.set_mouse_position(event.motion.x, event.motion.y);
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        // The position rides along with the button event, so a click that
        // arrives without a preceding motion still lands where it was aimed.
        input.set_mouse_position(event.button.x, event.button.y);
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
        const auto key = key_of(event.key.key);
        if (key.has_value()) {
            // Repeats are kept: holding Backspace should keep deleting.
            input.add_key(key.value());
        }
        break;
    }

    case SDL_EVENT_KEY_UP:
        input.set_mods(mods_of(event.key.mod));
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

    SDL_Window* window{};
    bool sdl_video{};
    core::u32 width{};
    core::u32 height{};
    bool closed{};

    // The atlas is copied rather than borrowed: uploading needs a command list,
    // so it happens on the next present, by which time the caller's FontAtlas
    // may be long gone.
    std::vector<core::u8> atlas_pixels;
    core::u32 atlas_width{};
    core::u32 atlas_height{};
    bool atlas_pending{};

    Uint64 start_ns{};

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() {
        // Order is load-bearing: the swapchain hands the window back to the
        // device, so both must still exist when it goes.
        target = rhi::Texture{};
        renderer.reset();
        swapchain.reset();
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
        device.reset();
        if (sdl_video) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
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

    // No high-density flag: the UI's coordinate space is the window's pixels,
    // so a pointer position needs no scaling to become a widget coordinate. A
    // real DPI story means baking the atlas at a scale and multiplying layout
    // units, which is a step of its own rather than a flag here.
    impl->window = SDL_CreateWindow(
        config.title.c_str(),
        static_cast<int>(config.width),
        static_cast<int>(config.height),
        SDL_WINDOW_RESIZABLE);
    if (impl->window == nullptr) {
        return Result::failure(WindowError::DisplayUnavailable);
    }

    auto device = rhi::Device::create(rhi::DeviceConfig{});
    if (!device) {
        return Result::failure(WindowError::Unavailable);
    }
    impl->device.emplace(std::move(device).value());

    auto swapchain = impl->device->claim_window(
        impl->window,
        config.vsync ? rhi::PresentMode::Vsync : rhi::PresentMode::Immediate);
    if (!swapchain) {
        return Result::failure(WindowError::DisplayUnavailable);
    }
    impl->swapchain.emplace(std::move(swapchain).value());

    auto renderer =
        UiRenderer::create(*impl->device, rhi::TextureFormat::Rgba8Unorm);
    if (!renderer) {
        return Result::failure(WindowError::BackendFailure);
    }
    impl->renderer.emplace(std::move(renderer).value());

    // Without this SDL sends no text input events at all, and every text field
    // would silently ignore the keyboard.
    static_cast<void>(SDL_StartTextInput(impl->window));

    int pixel_width = 0;
    int pixel_height = 0;
    static_cast<void>(
        SDL_GetWindowSizeInPixels(impl->window, &pixel_width, &pixel_height));
    impl->width = static_cast<core::u32>(std::max(pixel_width, 0));
    impl->height = static_cast<core::u32>(std::max(pixel_height, 0));
    impl->start_ns = SDL_GetTicksNS();

    return Result::success(UiWindow{std::move(impl)});
}

bool UiWindow::pump(InputState& input) {
    input.new_frame();

    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        if (!translate_event(&event, input)) {
            impl_->closed = true;
        }
    }

    // Polled rather than tracked through resize events: one query a frame is
    // cheaper than getting the event set right on every platform.
    int pixel_width = 0;
    int pixel_height = 0;
    static_cast<void>(
        SDL_GetWindowSizeInPixels(impl_->window, &pixel_width, &pixel_height));
    impl_->width = static_cast<core::u32>(std::max(pixel_width, 0));
    impl_->height = static_cast<core::u32>(std::max(pixel_height, 0));

    return !impl_->closed;
}

core::u32 UiWindow::width() const noexcept {
    return impl_->width;
}

core::u32 UiWindow::height() const noexcept {
    return impl_->height;
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

bool UiWindow::present(const UiDrawList& list, rhi::Color clear) {
    if (!impl_->ensure_target()) {
        return false;
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
