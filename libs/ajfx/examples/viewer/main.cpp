// ajfx_viewer: plays an ajfx effect on the D3D12 reference backend.
//
//   ajfx_viewer [--preset sparks|smoke|confetti] [--package file.ajfx]
//               [--capture out.png --seconds 2.5 --size 960x540]
//               [--export file.ajfx]
//
// With --capture it renders one frame at the given time to a PNG and exits;
// otherwise it opens a window. --export writes the effect, with precompiled
// kernels, as a package. A package carrying kernels is played without
// compiling anything, as a host such as the media server does.
//
// Window controls: drag to orbit, wheel to zoom, Space pause, Left/Right
// seek a second, Home restart, 1/2/3 switch preset, Esc quit.

// sscanf and atof are fine for parsing a handful of arguments.
#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include "../common/playback.hpp"
#include "png.hpp"

#include <ajfx/d3d12.hpp>
#include <ajfx/gpu/renderer.hpp>
#include <ajfx/package.hpp>
#include <ajfx/presets.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ajfx;

using samples::background;
using samples::Orbit;
using samples::Player;
using samples::preset;
using samples::read_file;

int fail(const std::string& message) {
    std::fprintf(stderr, "ajfx_viewer: %s\n", message.c_str());
    return 1;
}

// ------------------------------------------------------------ window

struct Window final {
    HWND handle{nullptr};
    Orbit orbit;
    bool dragging{false};
    POINT last{};
    bool resized{true};
    std::vector<int> keys;
};

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
        return 0;
    case WM_SIZE:
        if (window != nullptr) {
            window->resized = true;
        }
        return 0;
    case WM_LBUTTONDOWN:
        window->dragging = true;
        window->last = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP:
        window->dragging = false;
        ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE:
        if (window != nullptr && window->dragging) {
            const POINT now{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            window->orbit.yaw -= static_cast<float>(now.x - window->last.x) * 0.01F;
            window->orbit.pitch = std::clamp(window->orbit.pitch + static_cast<float>(now.y - window->last.y) * 0.01F,
                                             -1.4F, 1.4F);
            window->last = now;
        }
        return 0;
    case WM_MOUSEWHEEL:
        window->orbit.distance =
            std::clamp(window->orbit.distance * (GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 0.9F : 1.1F), 1.0F, 100.0F);
        return 0;
    case WM_KEYDOWN:
        window->keys.push_back(static_cast<int>(wparam));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

int run_window(d3d12::Device& device, gpu::ShaderCompiler* compiler, std::unique_ptr<Player> player) {
    Window window;
    WNDCLASSW type{};
    type.lpfnWndProc = window_proc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // the arrow
    type.lpszClassName = L"ajfx_viewer";
    RegisterClassW(&type);
    window.handle = CreateWindowExW(0, type.lpszClassName, L"ajfx", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                                    CW_USEDEFAULT, 1280, 760, nullptr, nullptr, type.hInstance, &window);
    if (window.handle == nullptr) {
        return fail("could not create a window");
    }

    gpu::Texture target;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    bool paused = false;
    double time = 0.0;
    auto previous = std::chrono::steady_clock::now();
    MSG message{};
    while (true) {
        while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                device.destroy_texture(target);
                (void)device.flush();
                return 0;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - previous).count();
        previous = now;

        for (const int key : std::exchange(window.keys, {})) {
            if (key == VK_ESCAPE) {
                DestroyWindow(window.handle);
            } else if (key == VK_SPACE) {
                paused = !paused;
            } else if (key == VK_LEFT) {
                time = std::max(0.0, time - 1.0);
            } else if (key == VK_RIGHT) {
                time += 1.0;
            } else if (key == VK_HOME) {
                time = 0.0;
            } else if (key >= '1' && key <= '3') {
                const char* names[] = {"sparks", "smoke", "confetti"};
                Result<std::unique_ptr<Player>> next =
                    Player::create(device, compiler, *preset(names[key - '1']), nullptr);
                if (next) {
                    player = std::move(next).value();
                    time = 0.0;
                }
            }
        }
        if (!paused) {
            time += std::min(elapsed, 0.1);
        }

        RECT client{};
        GetClientRect(window.handle, &client);
        const auto w = static_cast<std::uint32_t>(std::max<LONG>(client.right, 1));
        const auto h = static_cast<std::uint32_t>(std::max<LONG>(client.bottom, 1));
        if (w != width || h != height) {
            device.destroy_texture(target);
            target = device.create_render_target(w, h, gpu::TextureFormat::Rgba8Unorm, "viewer").value();
            width = w;
            height = h;
        }

        const std::uint32_t rate = player->effect().simulation_rate;
        const auto step_start = std::chrono::steady_clock::now();
        player->seek(static_cast<std::uint64_t>(time * rate));
        const gpu::Camera camera = window.orbit.camera(static_cast<float>(w) / static_cast<float>(h));
        player->render(camera, target, &background);
        const Result<bool> shown = device.present(window.handle, target);
        if (!shown) {
            return fail(shown.error().message);
        }
        const double frame_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - step_start).count();

        static double title_timer = 0.0;
        title_timer += elapsed;
        if (title_timer > 0.25) {
            title_timer = 0.0;
            wchar_t title[256];
            swprintf(title, std::size(title), L"ajfx - %hs - %.2f s%ls - step %llu - %.2f ms a frame",
                     player->effect().name.c_str(), time, paused ? L" (paused)" : L"",
                     static_cast<unsigned long long>(player->simulation().tick()), frame_ms);
            SetWindowTextW(window.handle, title);
        }
    }
}

// ------------------------------------------------------------ main

struct Options final {
    std::string preset{"sparks"};
    std::string package;
    std::string capture;
    std::string export_path;
    double seconds{2.0};
    std::uint32_t width{960U};
    std::uint32_t height{540U};
    bool warp{false};
};

std::optional<Options> parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string{}; };
        if (arg == "--preset") {
            options.preset = next();
        } else if (arg == "--package") {
            options.package = next();
        } else if (arg == "--capture") {
            options.capture = next();
        } else if (arg == "--export") {
            options.export_path = next();
        } else if (arg == "--seconds") {
            options.seconds = std::atof(next().c_str());
        } else if (arg == "--size") {
            const std::string size = next();
            if (std::sscanf(size.c_str(), "%ux%u", &options.width, &options.height) != 2) {
                return std::nullopt;
            }
        } else if (arg == "--warp") {
            options.warp = true;
        } else {
            return std::nullopt;
        }
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    const std::optional<Options> options = parse(argc, argv);
    if (!options) {
        std::fprintf(stderr,
                     "usage: ajfx_viewer [--preset sparks|smoke|confetti] [--package file.ajfx]\n"
                     "                   [--capture out.png] [--seconds 2] [--size 960x540] [--export file.ajfx] [--warp]\n");
        return 2;
    }

    std::optional<std::vector<std::uint8_t>> package;
    std::optional<Effect> effect;
    if (!options->package.empty()) {
        package = read_file(options->package);
        if (!package) {
            return fail("cannot read " + options->package);
        }
        Result<Effect> read = read_package(*package);
        if (!read) {
            return fail(read.error().message);
        }
        effect = std::move(read).value();
    } else {
        effect = preset(options->preset);
        if (!effect) {
            return fail("unknown preset '" + options->preset + "'");
        }
    }

    Result<std::unique_ptr<d3d12::Device>> device = d3d12::create_device({.warp = options->warp});
    if (!device) {
        return fail(device.error().message);
    }
    std::printf("adapter: %s\n", std::string{device.value()->adapter_name()}.c_str());
    // The compiler is optional: a package with kernels plays without one.
    Result<std::unique_ptr<gpu::ShaderCompiler>> compiler = d3d12::create_shader_compiler();
    gpu::ShaderCompiler* compiler_ptr = compiler ? compiler.value().get() : nullptr;

    Result<std::unique_ptr<Player>> player =
        Player::create(*device.value(), compiler_ptr, *effect, package ? &*package : nullptr);
    if (!player) {
        return fail(player.error().message);
    }
    if (!player.value()->kernel_note().empty()) {
        std::printf("%s\n", player.value()->kernel_note().c_str());
    }

    if (!options->export_path.empty()) {
        const Effect& e = player.value()->effect();
        std::vector<PackageEntry> extra = gpu::kernel_package_entries(e, player.value()->kernels());
        for (PackageEntry& texture : texture_package_entries(player.value()->textures())) {
            extra.push_back(std::move(texture));
        }
        Result<std::vector<std::uint8_t>> bytes = write_package(e, std::move(extra));
        if (!bytes) {
            return fail(bytes.error().message);
        }
        std::ofstream out{options->export_path, std::ios::binary};
        out.write(reinterpret_cast<const char*>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
        std::printf("wrote %s (%zu bytes, kernels and textures included)\n", options->export_path.c_str(),
                    bytes.value().size());
    }

    if (options->capture.empty()) {
        if (!options->export_path.empty()) {
            return 0;
        }
        return run_window(*device.value(), compiler_ptr, std::move(player).value());
    }

    d3d12::Device& d = *device.value();
    Player& p = *player.value();
    const gpu::Texture target =
        d.create_render_target(options->width, options->height, gpu::TextureFormat::Rgba8Unorm, "capture").value();
    p.seek(static_cast<std::uint64_t>(options->seconds * p.effect().simulation_rate));
    const gpu::Camera camera =
        Orbit{}.camera(static_cast<float>(options->width) / static_cast<float>(options->height));
    p.render(camera, target, &background);
    Result<std::vector<std::uint8_t>> pixels = d.read_texture(target);
    if (!pixels) {
        return fail(pixels.error().message);
    }
    if (!viewer::write_png(options->capture, options->width, options->height, pixels.value())) {
        return fail("cannot write " + options->capture);
    }
    std::printf("wrote %s: %s at %.2f s, step %llu\n", options->capture.c_str(), p.effect().name.c_str(),
                options->seconds, static_cast<unsigned long long>(p.simulation().tick()));
    return 0;
}
