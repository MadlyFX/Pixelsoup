// Measures the device-local memory one renderer object holds, as the video
// memory manager counts it for this process. One case per process, so nothing
// a previous case left resident is counted against the next.
//
//   mediaserver_render_vram_benchmark <object> <width> <height> [planar=1] [show=advanced] [count=1] [frames=8]
//
// Objects: `basic` and `advanced` are a lone SceneRenderer in that mode,
// `hybrid` is a HybridSceneRenderer as the compositor builds it, and
// `executor` is a CompositorSceneExecutor in the presenter's working format.
// `planar` sets RendererLimits::planar_reflections. `show` is the backend the
// minimal scene authors (basic or advanced). `count` builds that many of the
// object on one device, the way a presenter builds two executors and the
// editor three.
//
// Prints the idle device, the growth after construction, and the growth after
// `frames` recorded frames of a one-box, one-sun scene. Built but not
// registered with CTest.
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <dxgi1_4.h>
#pragma comment(lib, "dxgi.lib")
#endif

#include <mediaserver/render/compositor_scene_executor.hpp>
#include <mediaserver/render/hybrid_scene_renderer.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/scene.hpp>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;

namespace {

struct Vram final {
    double usage_mib{-1.0};
    double budget_mib{-1.0};
};

// Device-local memory this process holds, summed over adapters.
[[nodiscard]] Vram process_vram() {
    Vram result{};
#if defined(_WIN32)
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return result;
    UINT64 usage = 0U;
    UINT64 budget = 0U;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT index = 0U; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        IDXGIAdapter3* queried = nullptr;
        if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&queried)))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (SUCCEEDED(queried->QueryVideoMemoryInfo(0U, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                usage += info.CurrentUsage;
                budget += info.Budget;
            }
            queried->Release();
        }
        adapter->Release();
    }
    factory->Release();
    result.usage_mib = static_cast<double>(usage) / 1048576.0;
    result.budget_mib = static_cast<double>(budget) / 1048576.0;
#endif
    return result;
}

[[nodiscard]] scene::Scene minimal_scene(core::u32 width, core::u32 height, bool advanced) {
    scene::Scene scn{};
    scn.canvas = scene::Canvas{width, height};
    scn.show_rate = {50U, 1U};
    scn.camera.eye = scene::Vec3{0.0F, 2.0F, 6.0F};
    scn.camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    scn.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    scn.camera.field_of_view = 0.15F;
    scn.camera.near_plane = 0.1F;
    scn.camera.far_plane = 50.0F;
    scn.environment.sky_color = scene::Vec3{0.3F, 0.35F, 0.45F};
    scn.environment.ground_color = scene::Vec3{0.1F, 0.1F, 0.1F};
    scn.environment.ambient = 0.6F;
    scn.renderer_settings.backend = advanced ? scene::RendererBackend::Advanced : scene::RendererBackend::Basic;
    scene::DirectionalLight sun{};
    sun.direction = scene::Vec3{-0.4F, -1.0F, -0.3F};
    sun.color = scene::Vec3{1.0F, 0.97F, 0.9F};
    sun.intensity = 3.0F;
    scn.directional_lights.push_back(sun);
    scene::MeshInstance box{};
    box.source = doc::NodeId{900000U};
    box.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    box.world = scene::scaling({1.0F, 1.0F, 1.0F});
    box.normal = scene::normal_basis(box.world);
    box.material.base_color = scene::Vec3{0.5F, 0.5F, 0.55F};
    scn.instances.push_back(box);
    return scn;
}

void print(const char* phase, const Vram& now, const Vram& idle) {
    std::cout << std::fixed << std::setprecision(1) << phase << ": " << now.usage_mib << " MiB ("
              << (now.usage_mib - idle.usage_mib) << " over idle)\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: <basic|advanced|hybrid|executor> <width> <height> [planar=1] [show=advanced] [count=1] [frames=8]\n";
        return 2;
    }
    const std::string object = argv[1];
    const auto width = static_cast<core::u32>(std::stoul(argv[2]));
    const auto height = static_cast<core::u32>(std::stoul(argv[3]));
    const bool planar = argc > 4 ? std::string{argv[4]} != "0" : true;
    const bool advanced_show = argc > 5 ? std::string{argv[5]} != "basic" : true;
    const auto count = argc > 6 ? static_cast<core::u32>(std::stoul(argv[6])) : 1U;
    const auto frames = argc > 7 ? static_cast<core::u32>(std::stoul(argv[7])) : 8U;
    if (object != "basic" && object != "advanced" && object != "hybrid" && object != "executor") {
        std::cerr << "object must be basic, advanced, hybrid or executor\n";
        return 2;
    }
    if (!rhi::Device::available() || !render::SceneRenderer::shaders_available()) {
        std::cout << "[skip] no GPU or scene shaders\n";
        return 77;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    if (!opened) {
        std::cout << "[skip] no native D3D12 device\n";
        return 77;
    }
    auto device = std::move(opened).value();

    render::RendererLimits limits{};
    limits.planar_reflections = planar;
    const auto scn = minimal_scene(width, height, advanced_show);
    const auto slice = scene::whole(scn.canvas);

    std::cout << object << " " << width << "x" << height << " planar=" << planar
              << " show=" << (advanced_show ? "advanced" : "basic") << " count=" << count << "\n";
    const Vram idle = process_vram();
    std::cout << std::fixed << std::setprecision(1) << "idle device: " << idle.usage_mib
              << " MiB (budget " << idle.budget_mib << " MiB)\n";

    std::vector<render::SceneRenderer> scenes;
    std::vector<render::HybridSceneRenderer> hybrids;
    std::vector<render::CompositorSceneExecutor> executors;
    for (core::u32 index = 0U; index < count; ++index) {
        bool created = false;
        if (object == "basic" || object == "advanced") {
            auto made = render::SceneRenderer::create(device, width, height, limits,
                object == "advanced" ? render::SceneRendererMode::AdvancedLinear : render::SceneRendererMode::BasicDisplay);
            if (made) { scenes.push_back(std::move(made).value()); created = true; }
        } else if (object == "hybrid") {
            auto made = render::HybridSceneRenderer::create(device, width, height, limits,
                render::SceneEffectOwner::Compositor);
            if (made) { hybrids.push_back(std::move(made).value()); created = true; }
        } else {
            auto made = render::CompositorSceneExecutor::create(device, width, height, limits,
                render::MediaCacheConfig{}, rhi::TextureFormat::Rgba16Float);
            if (made) { executors.push_back(std::move(made).value()); created = true; }
        }
        if (!created) {
            std::cerr << "creation " << index << " failed\n";
            return 1;
        }
        const std::string phase = "after create #" + std::to_string(index + 1U);
        print(phase.c_str(), process_vram(), idle);
    }

    double peak = process_vram().usage_mib;
    bool all_recorded = true;
    for (core::u32 frame = 0U; frame < frames; ++frame) {
        auto acquired = device.acquire_commands();
        if (!acquired) { std::cerr << "acquire failed\n"; return 1; }
        auto commands = std::move(acquired).value();
        bool recorded = true;
        for (auto& renderer : scenes) recorded = renderer.render(commands, scn, slice) && recorded;
        for (auto& renderer : hybrids) recorded = renderer.render(commands, scn, slice) && recorded;
        for (auto& executor : executors) recorded = static_cast<bool>(executor.execute(commands, scn, slice)) && recorded;
        all_recorded = all_recorded && recorded;
        auto submitted = device.submit(std::move(commands));
        if (submitted) {
            auto work = std::move(submitted).value();
            static_cast<void>(work.wait());
        }
        const double now = process_vram().usage_mib;
        if (now > peak) peak = now;
        if (frame == 0U) print("after frame 1", process_vram(), idle);
    }
    const Vram settled = process_vram();
    print("after frames", settled, idle);
    std::cout << std::fixed << std::setprecision(1) << "peak sampled: " << peak << " MiB ("
              << (peak - idle.usage_mib) << " over idle)\n";
    std::cout << "frames recorded: " << (all_recorded ? "yes" : "NO") << "\n";
    for (const auto& executor : executors) {
        std::cout << "executor padded " << executor.padded_width() << "x" << executor.padded_height()
                  << ", selected " << (executor.renderer_readiness().selected == scene::RendererBackend::Advanced
                      ? "Advanced" : "Basic") << "\n";
    }
    for (auto& renderer : hybrids) {
        std::cout << "hybrid selected " << (renderer.readiness(scn.renderer_settings).selected
            == scene::RendererBackend::Advanced ? "Advanced" : "Basic") << "\n";
    }
    std::cout << std::fixed << std::setprecision(1) << "RESULT," << object << "," << width << "x" << height << ","
              << planar << "," << (advanced_show ? "advanced" : "basic") << "," << count << ","
              << (process_vram().usage_mib - idle.usage_mib) << "," << (peak - idle.usage_mib) << "\n";
    return all_recorded ? 0 : 3;
}
