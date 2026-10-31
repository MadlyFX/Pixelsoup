#include <mediaserver/render/compositor_scene_executor.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/scene/math.hpp>

#include <ajfx/builder.hpp>
#include <ajfx/d3d12.hpp>
#include <ajfx/package.hpp>
#include <ajfx/texture.hpp>

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

std::vector<core::u8> download(rhi::CommandList commands, const rhi::Texture& target,
                               core::u32 pixel_bytes = stride) {
    auto readback = g_device->create_readback(target.width() * target.height() * pixel_bytes);
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
                         ajfx::Color red = {1.0F, 0.0F, 0.0F, 1.0F}, bool published = false,
                         ajfx::BlendMode blend = ajfx::BlendMode::Alpha) {
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
        .renderer(ajfx::SpriteRenderer{.blend = blend});
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

// Three hundred puffs in a ball, each showing the "cells" flipbook cell its
// life has reached, sorted back to front. `textured` false leaves the texture
// out of the package.
std::string write_crowd(const std::string& name, ajfx::gpu::ShaderCompiler& compiler, bool textured = true) {
    const std::uint8_t colours[4][4] = {{255, 0, 0, 255}, {0, 255, 0, 160}, {0, 0, 255, 255}, {255, 255, 255, 96}};
    std::vector<std::uint8_t> pixels;
    for (std::uint32_t y = 0U; y < 16U; ++y) {
        for (std::uint32_t x = 0U; x < 16U; ++x) {
            const std::uint8_t* c = colours[(y / 8U) * 2U + x / 8U];
            pixels.insert(pixels.end(), {c[0], c[1], c[2], c[3]});
        }
    }
    ajfx::EffectBuilder builder{"crowd"};
    builder.emitter("puffs")
        .capacity(300U)
        .spawn(ajfx::SpawnBurst{.count = 300U})
        .init(ajfx::Lifetime{.min = 0.5F, .max = 3.0F})
        .init(ajfx::ShapeLocation{.shape = ajfx::Shape::Sphere, .radius = 1.2F})
        .init(ajfx::Velocity{.speed_min = 0.2F, .speed_max = 0.6F, .spread = 180.0F})
        .init(ajfx::InitSize{.min = 0.2F, .max = 0.5F})
        .renderer(ajfx::SpriteRenderer{.texture = "cells", .flipbook_columns = 2U, .flipbook_rows = 2U,
                                       .flipbook_blend = true});
    const ajfx::Effect effect = builder.build().value();
    const ajfx::Result<ajfx::gpu::KernelSet> kernels = ajfx::gpu::compile_kernels(effect, compiler);
    SCENE_CHECK(kernels);
    std::vector<ajfx::PackageEntry> extra = ajfx::gpu::kernel_package_entries(effect, kernels.value());
    if (textured) {
        extra.push_back({"textures/cells", ajfx::write_texture(ajfx::make_texture(16U, 16U, pixels).value())});
    }
    const ajfx::Result<std::vector<std::uint8_t>> package = ajfx::write_package(effect, std::move(extra));
    SCENE_CHECK(package);
    const std::filesystem::path path = g_directory / name;
    std::ofstream out{path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(package.value().data()),
              static_cast<std::streamsize>(package.value().size()));
    SCENE_CHECK(out.good());
    return path.string();
}

// One tiny white particle a unit in front of the origin, lighting the scene
// with `lights` lights (intensity 2, range 3).
std::string write_lamp(const std::string& name, ajfx::gpu::ShaderCompiler& compiler, std::uint32_t lights) {
    ajfx::EffectBuilder builder{"lamp"};
    builder.emitter("e")
        .capacity(64U)
        .spawn(ajfx::SpawnBurst{.count = 1U})
        .init(ajfx::Lifetime{.min = 100.0F, .max = 100.0F})
        .init(ajfx::ShapeLocation{.shape = ajfx::Shape::Point, .center = ajfx::Vec3{0.0F, 0.0F, 1.0F}})
        .init(ajfx::InitSize{.min = 0.001F, .max = 0.001F})
        .renderer(ajfx::SpriteRenderer{.light_count = lights, .light_intensity = 2.0F, .light_range = 3.0F});
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

// A half float's value.
float half_value(const std::vector<core::u8>& image, core::u32 width, core::u32 x, core::u32 y) {
    core::u16 bits = 0U;
    std::memcpy(&bits, image.data() + (static_cast<std::size_t>(y) * width + x) * stride, 2U);
    const int exponent = (bits >> 10) & 31;
    const float mantissa = static_cast<float>(bits & 1023);
    return exponent == 0 ? std::ldexp(mantissa, -24) : std::ldexp(1.0F + mantissa / 1024.0F, exponent - 15);
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

void sorted_textured_effects_draw_alike_on_slices(ajfx::gpu::ShaderCompiler& compiler) {
    // The sort axis and flipbook cells come from the camera and particle
    // state every node shares, so slices are pieces of the full canvas.
    const scene::Scene frame = scene_with(write_crowd("crowd.ajfx", compiler));
    auto full = render::SceneRenderer::create(*g_device, 128U, 64U, limits(), render::SceneRendererMode::AdvancedLinear);
    auto left = render::SceneRenderer::create(*g_device, 64U, 64U, limits(), render::SceneRendererMode::AdvancedLinear);
    auto right = render::SceneRenderer::create(*g_device, 64U, 64U, limits(), render::SceneRendererMode::AdvancedLinear);
    SCENE_CHECK(full && left && right);
    const std::vector<core::u8> image = settle(full.value(), frame, {0U, 0U, 128U, 64U});
    SCENE_CHECK(full.value().stats().particles_drawn == 1U && full.value().stats().particle_issue.empty());
    SCENE_CHECK(lit(image, 128U, 63U, 32U) && lit(image, 128U, 64U, 32U));
    const std::vector<core::u8> a = settle(left.value(), frame, {0U, 0U, 64U, 64U});
    const std::vector<core::u8> b = settle(right.value(), frame, {64U, 0U, 64U, 64U});
    for (core::u32 y = 0U; y < 64U; ++y) {
        const auto row = image.begin() + static_cast<std::ptrdiff_t>(y) * 128 * stride;
        const auto half = static_cast<std::ptrdiff_t>(64U * stride);
        SCENE_CHECK(std::equal(a.begin() + y * half, a.begin() + (y + 1U) * half, row));
        SCENE_CHECK(std::equal(b.begin() + y * half, b.begin() + (y + 1U) * half, row + half));
    }

    // With temporal antialiasing on, the sprites' motion lands in the
    // velocity target alike on the full canvas and its slices too: one flat
    // product a sprite, from the canvas-wide matrices. A frame further on,
    // so there is a previous frame to move from.
    scene::Scene moving = frame;
    moving.renderer_settings.taau = true;
    const auto velocity_of = [&](render::SceneRenderer& renderer, scene::CanvasRect slice) {
        for (const core::u64 show_frame : {10U, 11U}) {
            moving.show_frame = show_frame;
            static_cast<void>(settle(renderer, moving, slice));
        }
        rhi::CommandList commands = acquire();
        SCENE_CHECK(renderer.render(commands, moving, slice));
        return download(std::move(commands), renderer.velocity_target(), 16U);
    };
    const std::vector<core::u8> whole = velocity_of(full.value(), {0U, 0U, 128U, 64U});
    const std::vector<core::u8> va = velocity_of(left.value(), {0U, 0U, 64U, 64U});
    const std::vector<core::u8> vb = velocity_of(right.value(), {64U, 0U, 64U, 64U});
    for (core::u32 y = 0U; y < 64U; ++y) {
        const auto row = whole.begin() + static_cast<std::ptrdiff_t>(y) * 128 * 16;
        const auto half = static_cast<std::ptrdiff_t>(64U * 16U);
        SCENE_CHECK(std::equal(va.begin() + y * half, va.begin() + (y + 1U) * half, row));
        SCENE_CHECK(std::equal(vb.begin() + y * half, vb.begin() + (y + 1U) * half, row + half));
    }
    // Some sprite moved: a pixel with motion where the scene, still, has none.
    bool moved = false;
    for (std::size_t at = 0U; at + 16U <= whole.size() && !moved; at += 16U) {
        float motion[2]{};
        std::memcpy(motion, whole.data() + at, sizeof(motion));
        moved = motion[0] != 0.0F || motion[1] != 0.0F;
    }
    SCENE_CHECK(moved);

    // A package missing the texture its effect names fails, saying which.
    const scene::Scene bare = scene_with(write_crowd("bare.ajfx", compiler, false));
    for (int attempt = 0; attempt < 500 && full.value().stats().particles_failed == 0U; ++attempt) {
        rhi::CommandList commands = acquire();
        static_cast<void>(full.value().render(commands, bare, {0U, 0U, 128U, 64U}));
        static_cast<void>(download(std::move(commands), full.value().target()));
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    SCENE_CHECK(full.value().stats().particles_failed == 1U && full.value().stats().particles_drawn == 0U);
    SCENE_CHECK(full.value().stats().particle_issue.find("texture 'cells'") != std::string::npos);
    g_device->wait_idle();
}

void particle_lights_light_the_scene(ajfx::gpu::ShaderCompiler& compiler) {
    // A white wall at z = 0, with no other light and no ambient: black,
    // until a particle a unit in front of it lights it, brightest nearest.
    const auto lit_wall = [&](const std::string& source) {
        scene::Scene frame = scene_with(source);
        scene::MeshInstance wall;
        wall.source = doc::NodeId{2U};
        wall.mesh = {scene::Primitive::Quad, 1U};
        wall.world = scene::scaling({12.0F, 6.0F, 1.0F});
        wall.normal = scene::normal_basis(wall.world);
        wall.material.base_color = {1.0F, 1.0F, 1.0F};
        wall.material.specular = 0.0F;
        wall.material.roughness = 0.6F;
        frame.instances.push_back(wall);
        return frame;
    };
    auto full = render::SceneRenderer::create(*g_device, 128U, 64U, limits(), render::SceneRendererMode::AdvancedLinear);
    auto left = render::SceneRenderer::create(*g_device, 64U, 64U, limits(), render::SceneRendererMode::AdvancedLinear);
    auto right = render::SceneRenderer::create(*g_device, 64U, 64U, limits(), render::SceneRendererMode::AdvancedLinear);
    SCENE_CHECK(full && left && right);
    const scene::Scene dark = lit_wall(write_lamp("unlit.ajfx", compiler, 0U));
    const scene::Scene lamp = lit_wall(write_lamp("lamp.ajfx", compiler, 1U));
    const std::vector<core::u8> unlit = settle(full.value(), dark, {0U, 0U, 128U, 64U});
    const std::vector<core::u8> image = settle(full.value(), lamp, {0U, 0U, 128U, 64U});
    SCENE_CHECK(full.value().stats().particle_issue.empty());
    SCENE_CHECK(half_value(unlit, 128U, 70U, 32U) == 0.0F);
    const float near = half_value(image, 128U, 70U, 32U);
    const float far = half_value(image, 128U, 82U, 32U);
    SCENE_CHECK(near > 0.0F && far > 0.0F && near > far);
    // Every node lights its slice alike.
    const std::vector<core::u8> a = settle(left.value(), lamp, {0U, 0U, 64U, 64U});
    const std::vector<core::u8> b = settle(right.value(), lamp, {64U, 0U, 64U, 64U});
    for (core::u32 y = 0U; y < 64U; ++y) {
        const auto row = image.begin() + static_cast<std::ptrdiff_t>(y) * 128 * stride;
        const auto half = static_cast<std::ptrdiff_t>(64U * stride);
        SCENE_CHECK(std::equal(a.begin() + y * half, a.begin() + (y + 1U) * half, row));
        SCENE_CHECK(std::equal(b.begin() + y * half, b.begin() + (y + 1U) * half, row + half));
    }

    // Two placements asking for forty lights each: the second would pass the
    // budget, and is refused saying so.
    scene::Scene crowded = scene_with(write_lamp("forty.ajfx", compiler, 40U));
    scene::ParticleEffect second = crowded.particle_effects[0];
    second.source_node = doc::NodeId{8U};
    crowded.particle_effects.push_back(second);
    for (int attempt = 0; attempt < 500; ++attempt) {
        rhi::CommandList commands = acquire();
        static_cast<void>(full.value().render(commands, crowded, {0U, 0U, 128U, 64U}));
        static_cast<void>(download(std::move(commands), full.value().target()));
        if (full.value().stats().particles_failed != 0U) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    SCENE_CHECK(full.value().stats().particles_failed == 1U && full.value().stats().particles_drawn == 1U);
    SCENE_CHECK(full.value().stats().particle_issue.find("more than 64 lights") != std::string::npos);
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
    // effect draws once, in the pass that holds it. An additive effect over
    // nothing still reaches it: the layer composite divides colour by alpha,
    // and light with no coverage would vanish there.
    const scene::Scene frame = scene_with(
        write_effect("composited.ajfx", compiler, {1.0F, 0.0F, 0.0F, 1.0F}, false, ajfx::BlendMode::Additive));
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

void a_held_frame_with_temporal_antialiasing_shows_the_effect_once_ready(ajfx::gpu::ShaderCompiler& compiler) {
    // A paused show renders the same show frame over and over. With temporal
    // antialiasing on, a hold repeats the last resolve - but an effect whose
    // package loads, or whose simulation catches up, while the frame is held
    // is a new picture of that frame, and has to reach it without playback
    // moving. Once it has, the held picture stays exactly still.
    scene::Scene frame = scene_with(
        write_effect("held-taa.ajfx", compiler, {1.0F, 0.0F, 0.0F, 1.0F}, false, ajfx::BlendMode::Additive));
    frame.renderer_settings.taau = true;
    auto executor = render::CompositorSceneExecutor::create(*g_device, 128U, 64U, limits(), {},
                                                            rhi::TextureFormat::Rgba16Float);
    SCENE_CHECK(executor);
    const auto held = [&] {
        rhi::CommandList commands = acquire();
        auto result = executor.value().execute(commands, frame, scene::whole(frame.canvas));
        SCENE_CHECK(result);
        return download(std::move(commands), *result.value());
    };
    // The first frame cannot draw the effect yet: its package is still being
    // read, and frame 10 is more than one frame's simulation budget away.
    std::vector<core::u8> image = held();
    SCENE_CHECK(executor.value().renderer_passes().geometry.particles_drawn == 0U);
    SCENE_CHECK(!lit(image, 128U, 63U, 32U));
    for (int attempt = 0; attempt < 500; ++attempt) {
        image = held();
        const render::RenderStats& stats = executor.value().renderer_passes().geometry;
        if (stats.particles_pending == 0U && stats.particles_behind == 0U) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    const render::RendererPassStatus& passes = executor.value().renderer_passes();
    SCENE_CHECK(passes.temporal_antialiasing == render::PassState::Active);
    SCENE_CHECK(passes.geometry.particles_drawn == 1U && passes.geometry.particle_issue.empty());
    SCENE_CHECK(lit(image, 128U, 63U, 32U) && lit(image, 128U, 64U, 32U));
    SCENE_CHECK(!lit(image, 128U, 2U, 2U));
    // Nothing new is drawn after that: every further hold is the same image.
    for (int frames = 0; frames < 4; ++frames) {
        SCENE_CHECK(held() == image);
    }
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
        {"sorted_textured_effects_draw_alike_on_slices", sorted_textured_effects_draw_alike_on_slices},
        {"particle_lights_light_the_scene", particle_lights_light_the_scene},
        {"unstarted_missing_and_unsupported_effects", unstarted_missing_and_unsupported_effects},
        {"a_reexported_package_is_adopted_at_the_next_asset_generation",
         a_reexported_package_is_adopted_at_the_next_asset_generation},
        {"published_parameters_drive_the_simulation", published_parameters_drive_the_simulation},
        {"effects_alone_reach_the_composited_picture", effects_alone_reach_the_composited_picture},
        {"a_held_frame_with_temporal_antialiasing_shows_the_effect_once_ready",
         a_held_frame_with_temporal_antialiasing_shows_the_effect_once_ready},
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
