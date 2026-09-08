#include <mediaserver/rhi/device.hpp>

#include <SDL3/SDL.h>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

namespace core = mediaserver::core;
namespace rhi = mediaserver::rhi;

void check(bool condition, const char *expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line) +
            ": check failed: " + expression};
    }
}

#define WINDOW_CHECK(...)                                                   \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] rhi::TextureDesc target_desc(core::u32 width,
                                            core::u32 height) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.color_target = true;
    desc.sampled = true;
    return desc;
}

void a_native_d3d12_frame_reaches_a_dxgi_swapchain() {
    WINDOW_CHECK(SDL_Init(SDL_INIT_VIDEO));
    SDL_Window *window = SDL_CreateWindow(
        "media-server native presentation test", 96, 64, 0U);
    if (window == nullptr) {
        SDL_Quit();
        throw std::runtime_error{"could not create the Win32 test window"};
    }

    try {
        rhi::DeviceConfig config{};
        config.backend = rhi::BackendKind::D3d12;
        auto opened = rhi::Device::create(config);
        WINDOW_CHECK(opened);
        rhi::Device device = std::move(opened).value();
        WINDOW_CHECK(device.capabilities().backend ==
                     rhi::BackendKind::D3d12);

        auto claimed =
            device.claim_window(window, rhi::PresentMode::Immediate);
        WINDOW_CHECK(claimed);
        rhi::Swapchain swapchain = std::move(claimed).value();

        int pixel_width{};
        int pixel_height{};
        WINDOW_CHECK(SDL_GetWindowSizeInPixels(
            window, &pixel_width, &pixel_height));
        WINDOW_CHECK(pixel_width > 0 && pixel_height > 0);
        auto target = device.create_texture(target_desc(
            static_cast<core::u32>(pixel_width),
            static_cast<core::u32>(pixel_height)));
        WINDOW_CHECK(target);

        auto acquired_commands = device.acquire_commands();
        WINDOW_CHECK(acquired_commands);
        auto commands = std::move(acquired_commands).value();
        WINDOW_CHECK(commands.clear(
            target.value(), rhi::Color{0.04F, 0.31F, 0.82F, 1.0F}));

        const rhi::SwapchainImage image = swapchain.acquire(commands);
        WINDOW_CHECK(image);
        WINDOW_CHECK(image.backend == rhi::BackendKind::D3d12);
        WINDOW_CHECK(image.width == static_cast<core::u32>(pixel_width));
        WINDOW_CHECK(image.height == static_cast<core::u32>(pixel_height));
        WINDOW_CHECK(image.native_handle());
        WINDOW_CHECK(image.native_handle().value != image.handle);
        WINDOW_CHECK(swapchain.present(
            commands, image, target.value(), rhi::Filter::Nearest));

        auto submitted = device.submit(std::move(commands));
        WINDOW_CHECK(submitted);
        WINDOW_CHECK(std::move(submitted).value().wait());
        device.wait_idle();
    } catch (...) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        throw;
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
}

} // namespace

int main() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        std::cout << "[skip] native D3D12 device unavailable\n";
        return 0;
    }
    try {
        a_native_d3d12_frame_reaches_a_dxgi_swapchain();
        std::cout << "[pass] native D3D12 frame reached a DXGI swapchain\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "[fail] " << error.what() << '\n';
        return 1;
    }
}
