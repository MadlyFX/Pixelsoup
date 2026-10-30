#include <mediaserver/render/compositor_scene_executor.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/scene/math.hpp>

#include <ajfx/builder.hpp>
#include <ajfx/d3d12.hpp>
#include <ajfx/package.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Particle effects placed in a scene: the renderer loads the package off the
// frame path, holds readiness until it can draw the effect at the exact show
// frame, and draws the same particles on a full canvas as on its slices -
// including a sprite straddling the slice boundary.
namespace {

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) + ": check failed: " + expression};
    }
}

#define SCENE_CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// Advanced targets are four half floats a pixel.
constexpr core::u32 stride = 8U;

std::unique_ptr<rhi::Device> g_device;
std::filesystem::path g_directory;

rhi::CommandList acquire() {
    auto result = g_device->acquire_commands();
    SCENE_CHECK(result);
    return std::move(result).value();
}

std::vector<core::u8> download(rhi::CommandList commands, const rhi::Texture& target) {
    auto readback = g_device->create_readback(target.width() * target.height() * stride);
    SCENE_CHECK(readback);
    SCENE_CHECK(commands.download(target, readback.value()));
    auto submitted = g_device->submit(std::move(commands));
    SCENE_CHECK(submitted);
    SCENE_CHECK(std::move(submitted).value().wait());
    const auto bytes = readback.value().map();
    std::vector<core::u8> result(bytes.begin(), bytes.end());
    readback.value().unmap();
    return result;
}

// One still sprite, a world unit across, at the effect's origin. With
// `published`, its size is the published "size" parameter instead.
std::string write_effect(const std::string& name, ajfx::gpu::ShaderCompiler& compiler,
                         ajfx::Color red = {1.0F, 0.0F, 0.0F, 1.0F}, bool published = false) {
    ajfx::EffectBuilder builder{"still"};
    if (published) {
        builder.float_parameter("size", 1.0F, 0.1F, 4.0F);
    }
    builder.emitter("e")
        .spawn(ajfx::SpawnBurst{.count = 1U})
        .init(ajfx::Lifetime{.min = 100.0F, .max = 100.0F})
        .init(ajfx::ShapeLocation{.shape = ajfx::Shape::Point})
        .init(ajfx::InitColor{.a = red, .b = red})
        .init(published ? ajfx::InitSize{.min = ajfx::bind("size"), .max = ajfx::bind("size")}
                        : ajfx::InitSize{.min = 1.0F, .max = 1.0F})
        .renderer(ajfx::SpriteRenderer{.blend = ajfx::BlendMode::Alpha});
    const ajfx::Effect effect = builder.build().value();
    const ajfx::Result<ajfx::gpu::KernelSet> kernels = ajfx::gpu::compile_kernels(effect, compiler);
    SCENE_CHECK(kernels);
    const ajfx::Result<std::vector<std::uint8_t>> package =
        ajfx::write_package(effect, ajfx::gpu::kernel_package_entries(effect, kernels.value()));
    SCENE_CHECK(package);
    const std::filesystem::path path = g_directory / name;
    std::ofstream out{path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(package.value().data()),
              static_cast<std::streamsize>(package.value().size()));
    SCENE_CHECK(out.good());
    return path.string();
}

scene::Scene scene_with(const std::string& source) {
    scene::Scene frame;
    frame.canvas = {128U, 64U};
    frame.camera.eye = {0, 0, 4};
    frame.camera.target = {0, 0, 0};
    frame.camera.field_of_view = scene::Turns{0.25F};
    frame.environment.ambient = 0.0F;
    frame.environment.sky_color = {0, 0, 0};
    frame.environment.ground_color = {0, 0, 0};
    frame.renderer_settings.backend = scene::RendererBackend::Advanced;
    frame.show_rate = {24U, 1U};
    frame.show_frame = 10U;
    scene::ParticleEffect effect;
    effect.world = scene::identity();
    effect.source = source;
    effect.source_node = doc::NodeId{7U};
    effect.seed = 5U;
    frame.particle_effects.push_back(effect);
    return frame;
}

render::RendererLimits limits() {
    render::RendererLimits limits;
    limits.max_instances = 8U;
    limits.max_vertices = 128U;
    limits.max_indices = 128U;
    limits.directional_shadow_resolution = 16U;
    limits.shadow_atlas.extent = 64U;
    return limits;
}

// Renders until the package has loaded (on the cache's worker) and the
// simulation has caught up with the frame, a step budget at a time.
std::vector<core::u8> settle(render::SceneRenderer& renderer, const scene::Scene& frame, scene::CanvasRect slice) {
    for (int attempt = 0; attempt < 500; ++attempt) {
        rhi::CommandList commands = acquire();
        SCENE_CHECK(renderer.render(commands, frame, slice));
        std::vector<core::u8> image = download(std::move(commands), renderer.target());
        const render::RenderStats& stats = renderer.stats();
        if (stats.particles_behind != 0U) {
            // Held, not drawn early, and not ready.
            SCENE_CHECK(stats.particles_drawn == 0U && !stats.assets.ready);
        }
        if (stats.particles_pending == 0U && stats.particles_behind == 0U) {
            return image;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    throw std::runtime_error{"the particle package never loaded"};
}

bool lit(const std::vector<core::u8>& image, core::u32 width, core::u32 x, core::u32 y) {
    const std::size_t at = (static_cast<std::size_t>(y) * width + x) * stride;
    return std::any_of(image.begin() + static_cast<std::ptrdiff_t>(at),
                       image.begin() + static_cast<std::ptrdiff_t>(at + 6U), [](core::u8 byte) { return byte != 0U; });
}

void effects_draw_alike_on_the_full_canvas_and_its_slices(ajfx::gpu::ShaderCompiler& compiler) {
    const scene::Scene frame = scene_with(write_effect("still.ajfx", compiler));
    auto full = render::SceneRenderer::create(*g_device, 128U, 64U, limits(), render::SceneRendererMode::AdvancedLinear);
    auto left = render::SceneRenderer::create(*g_device, 64U, 64U, limits(), render::SceneRendererMode::AdvancedLinear);
    auto right = render::SceneRenderer::create(*g_device, 64U, 64U, limits(), render::SceneRendererMode::AdvancedLinear);
    SCENE_CHECK(full && left && right);

    // Frame 10 at 24 fps is 25 steps at 60 a second: more than one frame's
    // budget, so the first frame that can simulate holds.
    const std::vector<core::u8> image = settle(full.value(), frame, {0U, 0U, 128U, 64U});
    const render::RenderStats& stats = full.value().stats();
    SCENE_CHECK(stats.particles_drawn == 1U);
    SCENE_CHECK(stats.particles_failed == 0U && stats.particles_behind == 0U && stats.particle_issue.empty());
    SCENE_CHECK(stats.assets.ready);
    // The sprite straddles the canvas centre, where the slices meet; the
    // corners stay black.
    SCENE_CHECK(lit(image, 128U, 63U, 32U) && lit(image, 128U, 64U, 32U));
    SCENE_CHECK(!lit(image, 128U, 2U, 2U) && !lit(image, 128U, 125U, 61U));

    const std::vector<core::u8> a = settle(left.value(), frame, {0U, 0U, 64U, 64U});
    const std::vector<core::u8> b = settle(right.value(), frame, {64U, 0U, 64U, 64U});
    SCENE_CHECK(left.value().stats().particles_drawn == 1U && right.value().stats().particles_drawn == 1U);
    for (core::u32 y = 0U; y < 64U; ++y) {
        const auto row = image.begin() + static_cast<std::ptrdiff_t>(y) * 128 * stride;
        const auto half = static_cast<std::ptrdiff_t>(64U * stride);
        SCENE_CHECK(std::equal(a.begin() + y * half, a.begin() + (y + 1U) * half, row));
        SCENE_CHECK(std::equal(b.begin() + y * half, b.begin() + (y + 1U) * half, row + half));
    }

    // Rendering the same frame again draws the same picture.
    rhi::CommandList commands = acquire();
    SCENE_CHECK(full.value().render(commands, frame, {0U, 0U, 128U, 64U}));
    SCENE_CHECK(download(std::move(commands), full.value().target()) == image);
    g_device->wait_idle();
}

void unstarted_missing_and_unsupported_effects(ajfx::gpu::ShaderCompiler& compiler) {
    const std::string source = write_effect("held.ajfx", compiler);
    auto renderer = render::SceneRenderer::create(*g_device, 64U, 64U, limits(), render::SceneRendererMode::AdvancedLinear);
    SCENE_CHECK(renderer);

    // Not started yet: nothing drawn, nothing missing.
    scene::Scene frame = scene_with(source);
    frame.canvas = {64U, 64U};
    frame.particle_effects[0].start_frame = 100U;
    rhi::CommandList commands = acquire();
    SCENE_CHECK(renderer.value().render(commands, frame, {0U, 0U, 64U, 64U}));
    static_cast<void>(download(std::move(commands), renderer.value().target()));
    SCENE_CHECK(renderer.value().stats().particles_drawn == 0U && renderer.value().stats().particles_pending == 0U);
    SCENE_CHECK(renderer.value().stats().assets.ready);

    // A package that cannot be read fails readiness with the reason.
    frame.particle_effects[0].start_frame = 0U;
    frame.particle_effects[0].source = (g_directory / "missing.ajfx").string();
    static_cast<void>(settle(renderer.value(), frame, {0U, 0U, 64U, 64U}));
    SCENE_CHECK(renderer.value().stats().particles_failed == 1U);
    SCENE_CHECK(renderer.value().stats().particle_issue.find("missing.ajfx") != std::string::npos);
    SCENE_CHECK(!renderer.value().stats().assets.ready);

    // The Basic renderer cannot run particles and says so.
    auto basic = render::SceneRenderer::create(*g_device, 64U, 64U, limits(), render::SceneRendererMode::BasicDisplay);
    SCENE_CHECK(basic);
    frame.particle_effects[0].source = source;
    frame.renderer_settings.backend = scene::RendererBackend::Basic;
    commands = acquire();
    SCENE_CHECK(basic.value().render(commands, frame, {0U, 0U, 64U, 64U}));
    static_cast<void>(download(std::move(commands), basic.value().target()));
    SCENE_CHECK(basic.value().stats().particles_unsupported == 1U);
    SCENE_CHECK(!basic.value().stats().particle_issue.empty() && !basic.value().stats().assets.ready);
    g_device->wait_idle();
}

void a_reexported_package_is_adopted_at_the_next_asset_generation(ajfx::gpu::ShaderCompiler& compiler) {
    const std::string source = write_effect("swap.ajfx", compiler);
    scene::Scene frame = scene_with(source);
    frame.canvas = {64U, 64U};
    frame.asset_refresh_generation = 1U;
    auto renderer = render::SceneRenderer::create(*g_device, 64U, 64U, limits(), render::SceneRendererMode::AdvancedLinear);
    SCENE_CHECK(renderer);
    const std::vector<core::u8> red = settle(renderer.value(), frame, {0U, 0U, 64U, 64U});
    SCENE_CHECK(renderer.value().stats().particles_drawn == 1U);

    // Exported again in place, green. Until the asset generation moves on,
    // every node keeps drawing the package it has.
    static_cast<void>(write_effect("swap.ajfx", compiler, ajfx::Color{0.0F, 1.0F, 0.0F, 1.0F}));
    for (int frames = 0; frames < 10; ++frames) {
        rhi::CommandList commands = acquire();
        SCENE_CHECK(renderer.value().render(commands, frame, {0U, 0U, 64U, 64U}));
        SCENE_CHECK(download(std::move(commands), renderer.value().target()) == red);
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    // A rescan advances it: the old package draws (and readiness holds) until
    // the new one is read, then the new one does - as a fresh renderer sees it.
    frame.asset_refresh_generation = 2U;
    std::vector<core::u8> image = red;
    for (int attempt = 0; attempt < 500 && image == red; ++attempt) {
        rhi::CommandList commands = acquire();
        SCENE_CHECK(renderer.value().render(commands, frame, {0U, 0U, 64U, 64U}));
        image = download(std::move(commands), renderer.value().target());
        SCENE_CHECK(renderer.value().stats().particles_drawn + renderer.value().stats().particles_behind
                        + renderer.value().stats().particles_pending == 1U);
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    image = settle(renderer.value(), frame, {0U, 0U, 64U, 64U});
    SCENE_CHECK(image != red);
    auto fresh = render::SceneRenderer::create(*g_device, 64U, 64U, limits(), render::SceneRendererMode::AdvancedLinear);
    SCENE_CHECK(fresh);
    SCENE_CHECK(settle(fresh.value(), frame, {0U, 0U, 64U, 64U}) == image);
    g_device->wait_idle();
}

std::size_t lit_pixels(const std::vector<core::u8>& image, core::u32 width, core::u32 height) {
    std::size_t count = 0U;
    for (core::u32 y = 0U; y < height; ++y) {
        for (core::u32 x = 0U; x < width; ++x) {
            count += lit(image, width, x, y) ? 1U : 0U;
        }
    }
    return count;
}

void published_parameters_drive_the_simulation(ajfx::gpu::ShaderCompiler& compiler) {
    scene::Scene frame = scene_with(write_effect("sized.ajfx", compiler, {1.0F, 0.0F, 0.0F, 1.0F}, true));
    frame.canvas = {64U, 64U};
    const auto render = [&](const scene::Scene& shown) {
        auto renderer = render::SceneRenderer::create(*g_device, 64U, 64U, limits(), render::SceneRendererMode::AdvancedLinear);
        SCENE_CHECK(renderer);
        return settle(renderer.value(), shown, {0U, 0U, 64U, 64U});
    };
    const std::vector<core::u8> fallback = render(frame);

    // A value the node authors is the size every particle is born with.
    scene::Scene larger = frame;
    larger.particle_effects[0].parameters = {scene::ParticleParameter{"size.0", 2.0, {}}};
    const std::vector<core::u8> doubled = render(larger);
    SCENE_CHECK(lit_pixels(doubled, 64U, 64U) > 3U * lit_pixels(fallback, 64U, 64U));

    // A keyed component is the curve at each step's own time: flat at 2, it
    // is the same picture as the value 2; a key the package does not publish
    // is ignored.
    scene::Scene keyed = frame;
    scene::ParticleParameter curve{"size.0", 0.5, {}};
    curve.curve.component = scene::PropertyChannel::ParticleParameter;
    curve.curve.parameter = "size.0";
    curve.curve.frames = {0, 20};
    curve.curve.values = {2.0, 2.0};
    curve.curve.interpolations = {scene::KeyInterpolation::Linear, scene::KeyInterpolation::Linear};
    curve.curve.tangents = {std::nullopt, std::nullopt};
    keyed.particle_effects[0].parameters = {curve, scene::ParticleParameter{"wind.1", 3.0, {}}};
    SCENE_CHECK(render(keyed) == doubled);

    // Changing a value changes the effect's past: the same renderer simulates
    // again from the start and draws what a fresh one does.
    auto renderer = render::SceneRenderer::create(*g_device, 64U, 64U, limits(), render::SceneRendererMode::AdvancedLinear);
    SCENE_CHECK(renderer);
    SCENE_CHECK(settle(renderer.value(), frame, {0U, 0U, 64U, 64U}) == fallback);
    SCENE_CHECK(settle(renderer.value(), larger, {0U, 0U, 64U, 64U}) == doubled);
    g_device->wait_idle();
}

void effects_alone_reach_the_composited_picture(ajfx::gpu::ShaderCompiler& compiler) {
    // The editor's Stage and every output render through the compositor. A
    // scene holding only effects still has something to draw, and each
    // effect draws once, in the pass that holds it.
    const scene::Scene frame = scene_with(write_effect("composited.ajfx", compiler));
    auto executor = render::CompositorSceneExecutor::create(*g_device, 128U, 64U, limits(), {},
                                                            rhi::TextureFormat::Rgba16Float);
    SCENE_CHECK(executor);
    std::vector<core::u8> image;
    for (int attempt = 0; attempt < 500; ++attempt) {
        rhi::CommandList commands = acquire();
        auto result = executor.value().execute(commands, frame, scene::whole(frame.canvas));
        SCENE_CHECK(result);
        image = download(std::move(commands), *result.value());
        const render::RenderStats& stats = executor.value().renderer_passes().geometry;
        if (stats.particles_pending == 0U && stats.particles_behind == 0U) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    SCENE_CHECK(executor.value().renderer_passes().geometry.particles_drawn == 1U);
    SCENE_CHECK(lit(image, 128U, 63U, 32U) && lit(image, 128U, 64U, 32U));
    SCENE_CHECK(!lit(image, 128U, 2U, 2U));
    g_device->wait_idle();
}

} // namespace

int main() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        std::cout << "skipped: no D3D12 device\n";
        return 77;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto device = rhi::Device::create(config);
    ajfx::Result<std::unique_ptr<ajfx::gpu::ShaderCompiler>> compiler = ajfx::d3d12::create_shader_compiler();
    if (!device || !compiler || !device.value().capabilities().advanced_raster()) {
        std::cout << "skipped: no Advanced D3D12 device or shader compiler\n";
        return 77;
    }
    g_device = std::make_unique<rhi::Device>(std::move(device).value());
    g_directory = std::filesystem::temp_directory_path() / "mediaserver-scene-particles";
    std::filesystem::create_directories(g_directory);

    struct Case final {
        const char* name;
        void (*run)(ajfx::gpu::ShaderCompiler&);
    };
    const Case cases[] = {
        {"effects_draw_alike_on_the_full_canvas_and_its_slices", effects_draw_alike_on_the_full_canvas_and_its_slices},
        {"unstarted_missing_and_unsupported_effects", unstarted_missing_and_unsupported_effects},
        {"a_reexported_package_is_adopted_at_the_next_asset_generation",
         a_reexported_package_is_adopted_at_the_next_asset_generation},
        {"published_parameters_drive_the_simulation", published_parameters_drive_the_simulation},
        {"effects_alone_reach_the_composited_picture", effects_alone_reach_the_composited_picture},
    };
    int failures = 0;
    for (const Case& test : cases) {
        try {
            test.run(*compiler.value());
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    g_device.reset();
    std::error_code ignored;
    std::filesystem::remove_all(g_directory, ignored);
    return failures == 0 ? 0 : 1;
}
