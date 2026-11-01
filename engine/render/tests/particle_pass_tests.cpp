#include <mediaserver/render/particle_pass.hpp>

#include <ajfx/builder.hpp>
#include <ajfx/d3d12.hpp>
#include <ajfx/presets.hpp>
#include <ajfx/rigid.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The particle pass draws a simulated effect where the camera says, tests it
// against the scene's depth, follows the show frame exactly, and holds while
// a seek is still catching up rather than drawing particles from the wrong
// moment.
namespace {

namespace core = mediaserver::core;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) + ": check failed: " + expression};
    }
}

#define PASS_CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

constexpr core::u32 size = 64U;

std::unique_ptr<rhi::Device> g_device;
std::unique_ptr<ajfx::gpu::ShaderCompiler> g_compiler;
std::unique_ptr<ajfx::d3d12::Device> g_reference;

[[nodiscard]] scene::Mat4 identity() {
    scene::Mat4 m;
    for (core::u32 i = 0U; i < 4U; ++i) {
        m.set(i, i, 1.0F);
    }
    return m;
}

[[nodiscard]] scene::Mat4 translation(float x, float y, float z) {
    scene::Mat4 m = identity();
    m.set(0U, 3U, x);
    m.set(1U, 3U, y);
    m.set(2U, 3U, z);
    return m;
}

// Looking down -Z from the origin: 4 world units across the 64 pixels, so 16
// pixels a unit, every particle at reversed depth 0.5.
[[nodiscard]] scene::ViewMatrices orthographic() {
    scene::ViewMatrices view;
    view.view = identity();
    view.projection = identity();
    view.projection.set(0U, 0U, 0.5F);
    view.projection.set(1U, 1U, 0.5F);
    view.projection.set(2U, 2U, 0.0F);
    view.projection.set(2U, 3U, 0.5F);
    view.view_projection = view.projection;
    return view;
}

// One still particle of `diameter` at the origin.
[[nodiscard]] ajfx::Effect still(float diameter, ajfx::Color color, ajfx::BlendMode blend) {
    ajfx::EffectBuilder builder{"still"};
    builder.emitter("e")
        .spawn(ajfx::SpawnBurst{.count = 1U})
        .init(ajfx::Lifetime{.min = 100.0F, .max = 100.0F})
        .init(ajfx::ShapeLocation{.shape = ajfx::Shape::Point})
        .init(ajfx::InitColor{.a = color, .b = color})
        .init(ajfx::InitSize{.min = diameter, .max = diameter})
        .renderer(ajfx::SpriteRenderer{.blend = blend});
    return builder.build().value();
}

[[nodiscard]] render::ParticlePass make_pass(render::ParticleSettings settings = {}) {
    auto pass = render::ParticlePass::create(*g_device, rhi::TextureFormat::Rgba8Unorm, settings);
    if (!pass) {
        std::cout << pass.error().message << '\n';
    }
    PASS_CHECK(pass);
    return std::move(pass).value();
}

[[nodiscard]] render::ParticleInstance make_instance(render::ParticlePass& pass, const ajfx::Effect& effect,
                                                     core::u32 seed = 3U,
                                                     const ajfx::TextureSet& textures = ajfx::presets::textures()) {
    const ajfx::gpu::KernelSet kernels = ajfx::gpu::compile_kernels(effect, *g_compiler).value();
    auto instance = pass.create_instance(effect, kernels, textures, ajfx::FieldSet{}, ajfx::MeshSet{}, seed);
    if (!instance) {
        std::cout << instance.error().message << '\n';
    }
    PASS_CHECK(instance);
    return std::move(instance).value();
}

struct Frame final {
    std::vector<core::u8> pixels;
    [[nodiscard]] core::u8 red(core::u32 x, core::u32 y) const {
        return pixels[(static_cast<std::size_t>(y) * size + x) * 4U];
    }
    [[nodiscard]] core::u8 green(core::u32 x, core::u32 y) const {
        return pixels[(static_cast<std::size_t>(y) * size + x) * 4U + 1U];
    }
    [[nodiscard]] core::u8 blue(core::u32 x, core::u32 y) const {
        return pixels[(static_cast<std::size_t>(y) * size + x) * 4U + 2U];
    }
    [[nodiscard]] core::u8 alpha(core::u32 x, core::u32 y) const {
        return pixels[(static_cast<std::size_t>(y) * size + x) * 4U + 3U];
    }
};

// Advances to `frame` of a 24 fps show (one call) and draws into fresh
// targets cleared to transparent black and the given depth.
[[nodiscard]] Frame render_frame(render::ParticlePass& pass, render::ParticleInstance& instance, core::u64 frame,
                                 const scene::Mat4& placement, float depth_clear = 0.0F) {
    rhi::TextureDesc color_desc{};
    color_desc.width = size;
    color_desc.height = size;
    color_desc.format = rhi::TextureFormat::Rgba8Unorm;
    color_desc.color_target = true;
    color_desc.sampled = true;
    rhi::TextureDesc depth_desc{};
    depth_desc.width = size;
    depth_desc.height = size;
    depth_desc.format = rhi::TextureFormat::Depth32Float;
    depth_desc.depth_target = true;
    auto color = g_device->create_texture(color_desc);
    auto depth = g_device->create_texture(depth_desc);
    auto readback = g_device->create_readback(size * size * 4U);
    PASS_CHECK(color && depth && readback);

    auto acquired = g_device->acquire_commands();
    PASS_CHECK(acquired);
    rhi::CommandList commands = std::move(acquired).value();
    PASS_CHECK(pass.advance(commands, instance, frame, 24U, 1U));
    render::ParticleTargets targets{};
    targets.color = &color.value();
    targets.depth = &depth.value();
    targets.load = false;
    targets.clear = rhi::Color{0.0F, 0.0F, 0.0F, 0.0F};
    targets.depth_clear = depth_clear;
    PASS_CHECK(pass.draw(commands, instance, orthographic(), placement, targets));
    PASS_CHECK(commands.download(color.value(), readback.value()));
    auto submitted = g_device->submit(std::move(commands));
    PASS_CHECK(submitted);
    PASS_CHECK(std::move(submitted).value().wait());
    PASS_CHECK(!pass.take_error());

    const auto mapped = readback.value().map();
    Frame out{std::vector<core::u8>(mapped.begin(), mapped.begin() + size * size * 4U)};
    readback.value().unmap();
    return out;
}

void a_particle_draws_where_the_camera_sees_it() {
    render::ParticlePass pass = make_pass();
    render::ParticleInstance instance =
        make_instance(pass, still(1.0F, ajfx::Color{1.0F, 0.0F, 0.0F, 1.0F}, ajfx::BlendMode::Alpha));
    const Frame frame = render_frame(pass, instance, 1U, identity());
    // (32, 32) is 1/32 unit from the particle each way: coverage 1 - 2/256.
    PASS_CHECK(frame.red(32U, 32U) >= 251U && frame.alpha(32U, 32U) >= 251U);
    // Six pixels right: uv.x = 13/16, coverage 0.336.
    PASS_CHECK(frame.red(38U, 32U) >= 83U && frame.red(38U, 32U) <= 88U);
    PASS_CHECK(frame.alpha(42U, 32U) == 0U && frame.alpha(0U, 0U) == 0U);

    // Placed one unit right, it lands 16 pixels right.
    render::ParticleInstance moved =
        make_instance(pass, still(1.0F, ajfx::Color{1.0F, 0.0F, 0.0F, 1.0F}, ajfx::BlendMode::Alpha));
    const Frame shifted = render_frame(pass, moved, 1U, translation(1.0F, 0.0F, 0.0F));
    PASS_CHECK(shifted.red(48U, 32U) >= 251U);
    PASS_CHECK(shifted.alpha(32U, 32U) == 0U);
}

void the_scene_depth_hides_particles_behind_it() {
    render::ParticlePass pass = make_pass();
    render::ParticleInstance instance =
        make_instance(pass, still(1.0F, ajfx::Color{1.0F, 0.0F, 0.0F, 1.0F}, ajfx::BlendMode::Alpha));
    // Reversed depth: a scene surface at 1 (the near plane) covers the
    // particle at 0.5; the far plane at 0 does not.
    PASS_CHECK(render_frame(pass, instance, 1U, identity(), 1.0F).alpha(32U, 32U) == 0U);
    PASS_CHECK(render_frame(pass, instance, 1U, identity(), 0.0F).alpha(32U, 32U) >= 251U);
}

void additive_particles_add_and_cover_as_they_brighten() {
    render::ParticlePass pass = make_pass();
    ajfx::EffectBuilder builder{"pair"};
    builder.emitter("e")
        .spawn(ajfx::SpawnBurst{.count = 2U})
        .init(ajfx::Lifetime{.min = 100.0F, .max = 100.0F})
        .init(ajfx::ShapeLocation{.shape = ajfx::Shape::Point})
        .init(ajfx::InitColor{.a = ajfx::Color{0.25F, 0.25F, 0.25F, 1.0F}, .b = ajfx::Color{0.25F, 0.25F, 0.25F, 1.0F}})
        .init(ajfx::InitSize{.min = 1.0F, .max = 1.0F})
        .renderer(ajfx::SpriteRenderer{.blend = ajfx::BlendMode::Additive});
    render::ParticleInstance instance = make_instance(pass, builder.build().value());
    const Frame frame = render_frame(pass, instance, 1U, identity());
    // Two quarter-grey particles add to about half. Each covers a quarter, as
    // bright as it is, accumulating like over: 1 - 0.75^2 = 0.4375.
    PASS_CHECK(frame.red(32U, 32U) >= 124U && frame.red(32U, 32U) <= 128U);
    PASS_CHECK(frame.alpha(32U, 32U) >= 108U && frame.alpha(32U, 32U) <= 112U);
    PASS_CHECK(frame.alpha(2U, 2U) == 0U);
}

// Two particles drifting along +Z: the first, born earlier, is further along
// and blue by then; the second, just born, is red.
[[nodiscard]] ajfx::Effect drifting_pair(bool sort) {
    ajfx::EffectBuilder builder{"pair"};
    builder.emitter("e")
        .spawn(ajfx::SpawnBurst{.count = 1U, .interval = 0.3F, .cycles = 2U})
        .init(ajfx::Lifetime{.min = 1.0F, .max = 1.0F})
        .init(ajfx::ShapeLocation{.shape = ajfx::Shape::Point})
        .init(ajfx::Velocity{.direction = ajfx::Vec3{0.0F, 0.0F, 1.0F}})
        .init(ajfx::InitSize{.min = 1.0F, .max = 1.0F})
        .update(ajfx::ColorOverLife{.keys = {{0.0F, ajfx::Color{1.0F, 0.0F, 0.0F, 1.0F}},
                                             {0.25F, ajfx::Color{0.0F, 0.0F, 1.0F, 1.0F}}}})
        .renderer(ajfx::SpriteRenderer{.blend = ajfx::BlendMode::Alpha, .sort = sort});
    return builder.build().value();
}

void alpha_particles_draw_back_to_front_in_the_placed_effect() {
    // Show frame 8 is a third of a second: the first is at z = 1/3, the
    // second at z = 1/30. The camera looks down -Z, so the first is nearer
    // and, sorted, drawn last; birth order would draw the red one over it.
    // Twenty steps: a budget that reaches them in one frame.
    render::ParticlePass pass = make_pass(render::ParticleSettings{.step_budget = 64U});
    render::ParticleInstance sorted = make_instance(pass, drifting_pair(true));
    const Frame front = render_frame(pass, sorted, 8U, identity());
    PASS_CHECK(front.blue(32U, 32U) >= 240U && front.red(32U, 32U) <= 10U);
    render::ParticleInstance unsorted = make_instance(pass, drifting_pair(false));
    const Frame birth = render_frame(pass, unsorted, 8U, identity());
    PASS_CHECK(birth.red(32U, 32U) >= 200U && birth.blue(32U, 32U) <= 40U);
    // Placed turned half way round, the effect's +Z points away from the
    // camera: the first is now the further, and the red one is in front.
    scene::Mat4 turned = identity();
    turned.set(0U, 0U, -1.0F);
    turned.set(2U, 2U, -1.0F);
    render::ParticleInstance placed = make_instance(pass, drifting_pair(true));
    const Frame behind = render_frame(pass, placed, 8U, turned);
    PASS_CHECK(behind.red(32U, 32U) >= 200U && behind.blue(32U, 32U) <= 40U);
}

// Four solid 8x8 cells in a 16x16 texture: red, green, blue, white.
[[nodiscard]] ajfx::TextureSet cells() {
    const std::uint8_t colours[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
    std::vector<std::uint8_t> pixels;
    for (std::uint32_t y = 0U; y < 16U; ++y) {
        for (std::uint32_t x = 0U; x < 16U; ++x) {
            const std::uint8_t* c = colours[(y / 8U) * 2U + x / 8U];
            pixels.insert(pixels.end(), {c[0], c[1], c[2], 255});
        }
    }
    return ajfx::TextureSet{{"cells", ajfx::make_texture(16U, 16U, pixels).value()}};
}

void flipbooks_play_their_cells_over_each_life() {
    ajfx::EffectBuilder builder{"flipbook"};
    builder.emitter("e")
        .spawn(ajfx::SpawnBurst{.count = 1U})
        .init(ajfx::Lifetime{.min = 0.8F, .max = 0.8F})
        .init(ajfx::ShapeLocation{.shape = ajfx::Shape::Point})
        .init(ajfx::InitSize{.min = 1.0F, .max = 1.0F})
        .renderer(ajfx::SpriteRenderer{.texture = "cells", .flipbook_columns = 2U, .flipbook_rows = 2U});
    const ajfx::Effect effect = builder.build().value();
    render::ParticlePass pass = make_pass(render::ParticleSettings{.step_budget = 64U});
    // 0.8 s over four cells; show frames 1, 7 and 12 are steps 2, 17 and 30.
    const auto at = [&](core::u64 frame) {
        render::ParticleInstance instance = make_instance(pass, effect, 3U, cells());
        return render_frame(pass, instance, frame, identity());
    };
    const Frame red = at(1U);
    PASS_CHECK(red.red(32U, 32U) >= 250U && red.green(32U, 32U) <= 5U && red.alpha(32U, 32U) >= 250U);
    const Frame green = at(7U);
    PASS_CHECK(green.green(32U, 32U) >= 250U && green.red(32U, 32U) <= 5U);
    const Frame blue = at(12U);
    PASS_CHECK(blue.blue(32U, 32U) >= 250U && blue.green(32U, 32U) <= 5U);

    // A texture the effect names but was not given is refused, by name.
    const ajfx::gpu::KernelSet kernels = ajfx::gpu::compile_kernels(effect, *g_compiler).value();
    auto missing = pass.create_instance(effect, kernels, ajfx::TextureSet{}, ajfx::FieldSet{}, ajfx::MeshSet{}, 3U);
    PASS_CHECK(!missing && missing.error().code == render::ParticleErrorCode::InvalidEffect &&
               missing.error().message.find("texture 'cells'") != std::string::npos);
}

// Advances to `frame` and draws with a velocity target that starts at 7 in
// every channel, returning its pixels as floats, four a pixel.
[[nodiscard]] std::vector<float> render_motion(render::ParticlePass& pass, render::ParticleInstance& instance,
                                               core::u64 frame, bool valid) {
    rhi::TextureDesc color_desc{};
    color_desc.width = size;
    color_desc.height = size;
    color_desc.format = rhi::TextureFormat::Rgba8Unorm;
    color_desc.color_target = true;
    color_desc.sampled = true;
    rhi::TextureDesc velocity_desc = color_desc;
    velocity_desc.format = rhi::TextureFormat::Rgba32Float;
    rhi::TextureDesc depth_desc{};
    depth_desc.width = size;
    depth_desc.height = size;
    depth_desc.format = rhi::TextureFormat::Depth32Float;
    depth_desc.depth_target = true;
    auto color = g_device->create_texture(color_desc);
    auto velocity = g_device->create_texture(velocity_desc);
    auto depth = g_device->create_texture(depth_desc);
    auto readback = g_device->create_readback(size * size * 16U);
    PASS_CHECK(color && velocity && depth && readback);

    auto acquired = g_device->acquire_commands();
    PASS_CHECK(acquired);
    rhi::CommandList commands = std::move(acquired).value();
    PASS_CHECK(pass.advance(commands, instance, frame, 24U, 1U));
    // The scene's motion, standing in: 7 everywhere.
    std::vector<float> sevens(static_cast<std::size_t>(size) * size * 4U, 7.0F);
    PASS_CHECK(commands.upload(velocity.value(),
                               std::span<const core::u8>{reinterpret_cast<const core::u8*>(sevens.data()),
                                                         sevens.size() * sizeof(float)}));
    const auto view = orthographic();
    render::ParticleTargets targets{};
    targets.color = &color.value();
    targets.depth = &depth.value();
    targets.load = false;
    targets.clear = rhi::Color{0.0F, 0.0F, 0.0F, 0.0F};
    targets.velocity = &velocity.value();
    targets.canvas_view_projection = view.view_projection;
    targets.previous_view_projection = view.view_projection;
    targets.motion_seconds = 1.0F / 24.0F;
    targets.motion_valid = valid;
    PASS_CHECK(pass.draw(commands, instance, view, identity(), targets));
    PASS_CHECK(pass.draw_motion(commands, instance, view, identity(), targets));
    PASS_CHECK(commands.download(velocity.value(), readback.value()));
    auto submitted = g_device->submit(std::move(commands));
    PASS_CHECK(submitted);
    PASS_CHECK(std::move(submitted).value().wait());
    PASS_CHECK(!pass.take_error());
    const auto mapped = readback.value().map();
    std::vector<float> out(static_cast<std::size_t>(size) * size * 4U);
    std::memcpy(out.data(), mapped.data(), out.size() * sizeof(float));
    readback.value().unmap();
    return out;
}

void moving_particles_write_their_motion() {
    // One opaque particle a world unit across, moving +X at 1 unit a second.
    // The orthographic camera maps a unit to half the clip range, so a 24th
    // of a second is 0.5 / 24 in clip space and half that in UV.
    ajfx::EffectBuilder builder{"mover"};
    builder.emitter("e")
        .spawn(ajfx::SpawnBurst{.count = 1U})
        .init(ajfx::Lifetime{.min = 100.0F, .max = 100.0F})
        .init(ajfx::ShapeLocation{.shape = ajfx::Shape::Point})
        .init(ajfx::Velocity{.direction = ajfx::Vec3{1.0F, 0.0F, 0.0F}})
        .init(ajfx::InitSize{.min = 1.0F, .max = 1.0F});
    const ajfx::Effect effect = builder.build().value();
    render::ParticlePass pass = make_pass();
    render::ParticleInstance instance = make_instance(pass, effect);
    // Two steps in, the particle has moved a thirtieth of a unit: still over
    // the centre pixel (32, 32).
    const std::vector<float> moving = render_motion(pass, instance, 1U, true);
    const auto at = [](const std::vector<float>& pixels, core::u32 x, core::u32 y, core::u32 channel) {
        return pixels[(static_cast<std::size_t>(y) * size + x) * 4U + channel];
    };
    PASS_CHECK(std::abs(at(moving, 32U, 32U, 0U) - 0.25F / 24.0F) < 1.0e-5F);
    PASS_CHECK(at(moving, 32U, 32U, 1U) == 0.0F);
    // Current and expected previous depth: the projection puts everything at
    // 0.5.
    PASS_CHECK(at(moving, 32U, 32U, 2U) == 0.5F && at(moving, 32U, 32U, 3U) == 0.5F);
    // The disc's fringe, under half covered, and the space around it keep
    // the scene's motion.
    PASS_CHECK(at(moving, 39U, 32U, 0U) == 7.0F && at(moving, 2U, 2U, 0U) == 7.0F);
    // With no previous frame there is no motion, only depth.
    render::ParticleInstance fresh = make_instance(pass, effect);
    const std::vector<float> still = render_motion(pass, fresh, 1U, false);
    PASS_CHECK(at(still, 32U, 32U, 0U) == 0.0F && at(still, 32U, 32U, 2U) == 0.5F);
}

// One still particle drawn over a scene whose depth (the velocity product's
// z) is `scene_depth` everywhere, or with no scene depth at all. The view is
// orthographic with a real depth ramp: reversed depth 0.5 + 0.1 z, so the
// particle (z = 0) is at 0.5 and a scene depth of 0.4 is one unit behind it.
[[nodiscard]] Frame render_soft(render::ParticlePass& pass, float soft_distance, std::optional<float> scene_depth) {
    render::ParticleInstance instance = make_instance(
        pass, [&] {
            ajfx::EffectBuilder builder{"soft"};
            builder.emitter("e")
                .spawn(ajfx::SpawnBurst{.count = 1U})
                .init(ajfx::Lifetime{.min = 100.0F, .max = 100.0F})
                .init(ajfx::ShapeLocation{.shape = ajfx::Shape::Point})
                .init(ajfx::InitColor{.a = ajfx::Color{1.0F, 0.0F, 0.0F, 1.0F}, .b = ajfx::Color{1.0F, 0.0F, 0.0F, 1.0F}})
                .init(ajfx::InitSize{.min = 1.0F, .max = 1.0F})
                .renderer(ajfx::SpriteRenderer{.blend = ajfx::BlendMode::Alpha, .soft_distance = soft_distance});
            return builder.build().value();
        }());
    rhi::TextureDesc color_desc{};
    color_desc.width = size;
    color_desc.height = size;
    color_desc.format = rhi::TextureFormat::Rgba8Unorm;
    color_desc.color_target = true;
    color_desc.sampled = true;
    rhi::TextureDesc scene_desc = color_desc;
    scene_desc.format = rhi::TextureFormat::Rgba32Float;
    rhi::TextureDesc depth_desc{};
    depth_desc.width = size;
    depth_desc.height = size;
    depth_desc.format = rhi::TextureFormat::Depth32Float;
    depth_desc.depth_target = true;
    auto color = g_device->create_texture(color_desc);
    auto scene = g_device->create_texture(scene_desc);
    auto depth = g_device->create_texture(depth_desc);
    auto readback = g_device->create_readback(size * size * 4U);
    PASS_CHECK(color && scene && depth && readback);

    auto acquired = g_device->acquire_commands();
    PASS_CHECK(acquired);
    rhi::CommandList commands = std::move(acquired).value();
    PASS_CHECK(pass.advance(commands, instance, 1U, 24U, 1U));
    std::vector<float> product(static_cast<std::size_t>(size) * size * 4U, 0.0F);
    for (std::size_t at = 2U; at < product.size(); at += 4U) {
        product[at] = scene_depth.value_or(0.0F);
    }
    PASS_CHECK(commands.upload(scene.value(),
                               std::span<const core::u8>{reinterpret_cast<const core::u8*>(product.data()),
                                                         product.size() * sizeof(float)}));
    scene::ViewMatrices view = orthographic();
    view.projection.set(2U, 2U, 0.1F);
    view.view_projection = view.projection;
    render::ParticleTargets targets{};
    targets.color = &color.value();
    targets.depth = &depth.value();
    targets.load = false;
    targets.clear = rhi::Color{0.0F, 0.0F, 0.0F, 0.0F};
    targets.scene_depth = scene_depth ? &scene.value() : nullptr;
    PASS_CHECK(pass.draw(commands, instance, view, identity(), targets));
    PASS_CHECK(commands.download(color.value(), readback.value()));
    auto submitted = g_device->submit(std::move(commands));
    PASS_CHECK(submitted);
    PASS_CHECK(std::move(submitted).value().wait());
    PASS_CHECK(!pass.take_error());
    const auto mapped = readback.value().map();
    Frame out{std::vector<core::u8>(mapped.begin(), mapped.begin() + size * size * 4U)};
    readback.value().unmap();
    return out;
}

void soft_particles_fade_as_they_near_the_scene() {
    render::ParticlePass pass = make_pass();
    // Hard edges, or no scene to fade against: the whole sprite.
    const core::u8 whole = render_soft(pass, 0.0F, 0.4F).red(32U, 32U);
    PASS_CHECK(whole >= 251U);
    PASS_CHECK(render_soft(pass, 2.0F, std::nullopt).red(32U, 32U) == whole);
    // A surface one unit behind, fading over two: half.
    const Frame half = render_soft(pass, 2.0F, 0.4F);
    PASS_CHECK(half.red(32U, 32U) >= 124U && half.red(32U, 32U) <= 128U);
    PASS_CHECK(half.alpha(32U, 32U) >= 124U && half.alpha(32U, 32U) <= 128U);
    // Fading over half a unit, a unit away is past the fade: whole.
    PASS_CHECK(render_soft(pass, 0.5F, 0.4F).red(32U, 32U) == whole);
    // Where nothing was drawn behind (cleared depth), the sprite stays whole.
    PASS_CHECK(render_soft(pass, 2.0F, 0.0F).red(32U, 32U) == whole);
}

void distance_fields_reach_the_simulation() {
    // Particles thrown down onto a field floor at y = -0.5: once the first
    // advance has uploaded it, none fall through.
    const ajfx::FieldSet fields{{"floor", ajfx::make_distance_field(9U, 9U, 9U, ajfx::Vec3{-4.0F, -4.0F, -4.0F}, 1.0F,
                                                                     [](ajfx::Vec3 p) { return p.y + 0.5F; })
                                              .value()}};
    ajfx::EffectBuilder builder{"floored"};
    builder.emitter("e")
        .spawn(ajfx::SpawnRate{.rate = 60.0F})
        .init(ajfx::Lifetime{.min = 2.0F, .max = 2.0F})
        .init(ajfx::ShapeLocation{.shape = ajfx::Shape::Point})
        .init(ajfx::Velocity{.direction = ajfx::Vec3{0.0F, -1.0F, 0.0F}, .speed_min = 2.0F, .speed_max = 2.0F})
        .update(ajfx::SdfCollision{.field = "floor", .restitution = 0.0F});
    const ajfx::Effect effect = builder.build().value();
    const ajfx::gpu::KernelSet kernels = ajfx::gpu::compile_kernels(effect, *g_compiler).value();
    render::ParticlePass pass = make_pass(render::ParticleSettings{.step_budget = 256U});
    auto created = pass.create_instance(effect, kernels, ajfx::TextureSet{}, fields, ajfx::MeshSet{}, 3U);
    PASS_CHECK(created);
    render::ParticleInstance instance = std::move(created).value();
    static_cast<void>(render_frame(pass, instance, 36U, identity()));
    const ajfx::Result<ajfx::EmitterState> state = instance.simulation().read_emitter(0U);
    PASS_CHECK(state && state.value().alive() > 60U);
    for (const ajfx::Vec3& p : state.value().position) {
        PASS_CHECK(p.y >= -0.5F - 1.0e-4F);
    }
    // Without the field the effect is refused, by name.
    auto missing = pass.create_instance(effect, kernels, ajfx::TextureSet{}, ajfx::FieldSet{}, ajfx::MeshSet{}, 3U);
    PASS_CHECK(!missing && missing.error().message.find("field 'floor'") != std::string::npos);
}

// A red unit cube at the origin, turned `degrees` about Y; with `behind`, a
// green sprite emitter too, drawn after it, a fifth of a unit behind its
// front face.
[[nodiscard]] ajfx::Effect cube_effect(float degrees, bool behind) {
    ajfx::EffectBuilder builder{"cube"};
    builder.emitter("box")
        .spawn(ajfx::SpawnBurst{.count = 1U})
        .init(ajfx::Lifetime{.min = 10.0F, .max = 10.0F})
        .init(ajfx::ShapeLocation{.shape = ajfx::Shape::Point})
        .init(ajfx::InitColor{.a = ajfx::Color{1.0F, 0.0F, 0.0F, 1.0F}, .b = ajfx::Color{1.0F, 0.0F, 0.0F, 1.0F}})
        .init(ajfx::InitSize{.min = 1.0F, .max = 1.0F})
        .init(ajfx::InitRotation{.min = degrees, .max = degrees})
        .renderer(ajfx::SpriteRenderer{.geometry = ajfx::Geometry::Mesh, .mesh = "cube"});
    if (behind) {
        builder.emitter("dot")
            .spawn(ajfx::SpawnBurst{.count = 1U})
            .init(ajfx::Lifetime{.min = 10.0F, .max = 10.0F})
            .init(ajfx::ShapeLocation{.shape = ajfx::Shape::Point, .center = ajfx::Vec3{0.0F, 0.0F, 0.3F}})
            .init(ajfx::InitColor{.a = ajfx::Color{0.0F, 1.0F, 0.0F, 1.0F}, .b = ajfx::Color{0.0F, 1.0F, 0.0F, 1.0F}})
            .init(ajfx::InitSize{.min = 0.5F, .max = 0.5F});
    }
    return builder.build().value();
}

// Draws `instance` at show frame 1 with the orthographic view given a depth
// ramp (reversed depth 0.5 + 0.1 z), which solid meshes need to hide their
// far side; the flat test projection puts everything at one depth.
[[nodiscard]] Frame render_with_depth(render::ParticlePass& pass, render::ParticleInstance& instance) {
    scene::ViewMatrices ramp = orthographic();
    ramp.projection.set(2U, 2U, 0.1F);
    ramp.view_projection = ramp.projection;
    rhi::TextureDesc color_desc{};
    color_desc.width = size;
    color_desc.height = size;
    color_desc.format = rhi::TextureFormat::Rgba8Unorm;
    color_desc.color_target = true;
    color_desc.sampled = true;
    rhi::TextureDesc depth_desc{};
    depth_desc.width = size;
    depth_desc.height = size;
    depth_desc.format = rhi::TextureFormat::Depth32Float;
    depth_desc.depth_target = true;
    auto color = g_device->create_texture(color_desc);
    auto depth = g_device->create_texture(depth_desc);
    auto readback = g_device->create_readback(size * size * 4U);
    PASS_CHECK(color && depth && readback);
    auto acquired = g_device->acquire_commands();
    PASS_CHECK(acquired);
    rhi::CommandList commands = std::move(acquired).value();
    PASS_CHECK(pass.advance(commands, instance, 1U, 24U, 1U));
    render::ParticleTargets targets{};
    targets.color = &color.value();
    targets.depth = &depth.value();
    targets.load = false;
    targets.clear = rhi::Color{0.0F, 0.0F, 0.0F, 0.0F};
    PASS_CHECK(pass.draw(commands, instance, ramp, identity(), targets));
    PASS_CHECK(commands.download(color.value(), readback.value()));
    auto submitted = g_device->submit(std::move(commands));
    PASS_CHECK(submitted);
    PASS_CHECK(std::move(submitted).value().wait());
    PASS_CHECK(!pass.take_error());
    const auto mapped = readback.value().map();
    Frame out{std::vector<core::u8>(mapped.begin(), mapped.begin() + size * size * 4U)};
    readback.value().unmap();
    return out;
}

void mesh_particles_draw_solid_meshes() {
    render::ParticlePass pass = make_pass();
    const ajfx::MeshSet meshes{{"cube", ajfx::cube_mesh()}};
    const auto instance_of = [&](const ajfx::Effect& effect) {
        const ajfx::gpu::KernelSet kernels = ajfx::gpu::compile_kernels(effect, *g_compiler).value();
        auto created = pass.create_instance(effect, kernels, ajfx::TextureSet{}, ajfx::FieldSet{}, meshes, 3U);
        PASS_CHECK(created);
        return std::move(created).value();
    };
    // A solid square a unit (16 pixels) across, its front face lit full on.
    render::ParticleInstance square = instance_of(cube_effect(0.0F, false));
    const Frame flat = render_with_depth(pass, square);
    PASS_CHECK(flat.red(32U, 32U) >= 254U && flat.alpha(32U, 32U) >= 254U);
    PASS_CHECK(flat.red(39U, 39U) >= 254U && flat.alpha(41U, 32U) == 0U);
    // Turned 45 degrees: faces lit about 0.81, half a diagonal either side.
    render::ParticleInstance turned = instance_of(cube_effect(45.0F, false));
    const Frame corner = render_with_depth(pass, turned);
    PASS_CHECK(corner.red(36U, 32U) >= 203U && corner.red(36U, 32U) <= 210U);
    PASS_CHECK(corner.alpha(42U, 32U) >= 254U && corner.alpha(45U, 32U) == 0U);
    // The cube writes depth: a sprite drawn after it but behind its front
    // face stays hidden.
    render::ParticleInstance pair = instance_of(cube_effect(0.0F, true));
    const Frame hidden = render_with_depth(pass, pair);
    PASS_CHECK(hidden.red(32U, 32U) >= 254U && hidden.green(32U, 32U) == 0U);
}

void tumbling_meshes_turn_by_their_orientation() {
    // A red unit cube tumbling about a random axis, as debris does.
    ajfx::EffectBuilder builder{"tumbling"};
    builder.emitter("box")
        .spawn(ajfx::SpawnBurst{.count = 1U})
        .init(ajfx::Lifetime{.min = 10.0F, .max = 10.0F})
        .init(ajfx::ShapeLocation{.shape = ajfx::Shape::Point})
        .init(ajfx::InitColor{.a = ajfx::Color{1.0F, 0.0F, 0.0F, 1.0F}, .b = ajfx::Color{1.0F, 0.0F, 0.0F, 1.0F}})
        .init(ajfx::InitSize{.min = 1.0F, .max = 1.0F})
        .init(ajfx::InitTumble{.speed_min = 200.0F, .speed_max = 200.0F})
        .update(ajfx::RigidBody{.angular_drag = 0.0F})
        .renderer(ajfx::SpriteRenderer{.geometry = ajfx::Geometry::Mesh, .mesh = "cube"});
    const ajfx::Effect effect = builder.build().value();
    const ajfx::gpu::KernelSet kernels = ajfx::gpu::compile_kernels(effect, *g_compiler).value();
    render::ParticlePass pass = make_pass();
    std::vector<std::size_t> areas;
    // Two placements, two orientations.
    for (const core::u32 seed : {3U, 4U}) {
        auto created = pass.create_instance(effect, kernels, ajfx::TextureSet{}, ajfx::FieldSet{},
                                            ajfx::MeshSet{{"cube", ajfx::cube_mesh()}}, seed);
        PASS_CHECK(created);
        render::ParticleInstance instance = std::move(created).value();
        const Frame frame = render_with_depth(pass, instance);
        // Seen along Z, a unit cube covers |x.z| + |y.z| + |z.z| square
        // units, where x, y and z are its turned axes: 256 pixels each.
        const ajfx::Quat q = instance.simulation().read_emitter(0U).value().orientation[0];
        const float xz = 2.0F * (q.x * q.z - q.w * q.y);
        const float yz = 2.0F * (q.y * q.z + q.w * q.x);
        const float zz = 1.0F - 2.0F * (q.x * q.x + q.y * q.y);
        const float expected = 256.0F * (std::fabs(xz) + std::fabs(yz) + std::fabs(zz));
        std::size_t lit = 0U;
        for (core::u32 y = 0U; y < size; ++y) {
            for (core::u32 x = 0U; x < size; ++x) {
                lit += frame.alpha(x, y) != 0U ? 1U : 0U;
            }
        }
        std::cout << "  seed " << seed << ": " << lit << " pixels, expected " << expected << '\n';
        PASS_CHECK(std::fabs(static_cast<float>(lit) - expected) < 0.05F * expected);
        PASS_CHECK(frame.red(32U, 32U) >= 80U);
        areas.push_back(lit);
    }
    PASS_CHECK(areas[0] != areas[1]);
}

// Records `show_frame` of `instance` and its lights from slot `first`, and
// returns `slots` light records, 32 floats each.
std::vector<float> lights_at(render::ParticlePass& pass, render::ParticleInstance& instance, core::u64 show_frame,
                             const scene::Mat4& placed, core::u32 first, core::u32 slots) {
    auto lights = g_device->create_writable_buffer(render::max_particle_lights * 128U);
    auto readback = g_device->create_readback((first + slots) * 128U);
    PASS_CHECK(lights && readback);
    auto acquired = g_device->acquire_commands();
    PASS_CHECK(acquired);
    rhi::CommandList commands = std::move(acquired).value();
    PASS_CHECK(pass.advance(commands, instance, show_frame, 24U, 1U));
    PASS_CHECK(pass.write_lights(commands, instance, placed, lights.value(), first));
    PASS_CHECK(commands.download(lights.value(), 0U, (first + slots) * 128U, readback.value()));
    auto submitted = g_device->submit(std::move(commands));
    PASS_CHECK(submitted);
    PASS_CHECK(std::move(submitted).value().wait());
    PASS_CHECK(!pass.take_error());
    std::vector<float> words((first + slots) * 32U);
    const auto mapped = readback.value().map();
    std::memcpy(words.data(), mapped.data(), words.size() * sizeof(float));
    readback.value().unmap();
    return words;
}

void particle_lights_are_written_for_the_lit_pass() {
    // Ten particles flung out with four lights: the bound of ten spread over
    // four lights puts one on every third particle by birth - 0, 3, 6 and 9 -
    // in slots 0 to 3. A second emitter with one particle and three lights
    // fills two slots with nothing.
    ajfx::EffectBuilder builder{"lamps"};
    builder.emitter("many")
        .spawn(ajfx::SpawnBurst{.count = 10U})
        .init(ajfx::Lifetime{.min = 10.0F, .max = 10.0F})
        .init(ajfx::ShapeLocation{.shape = ajfx::Shape::Sphere, .radius_min = 2.0F, .radius_max = 2.0F})
        .init(ajfx::InitColor{.a = ajfx::Color{1.0F, 0.5F, 0.25F, 0.5F}, .b = ajfx::Color{1.0F, 0.5F, 0.25F, 0.5F}})
        .renderer(ajfx::SpriteRenderer{.light_count = 4U,
                                       .light_intensity_min = 3.0F,
                                       .light_intensity_max = 3.0F,
                                       .light_range_min = 2.5F,
                                       .light_range_max = 2.5F});
    builder.emitter("one")
        .spawn(ajfx::SpawnBurst{.count = 1U})
        .init(ajfx::Lifetime{.min = 10.0F, .max = 10.0F})
        .init(ajfx::ShapeLocation{.shape = ajfx::Shape::Point})
        .renderer(ajfx::SpriteRenderer{.light_count = 3U});
    const ajfx::Effect effect = builder.build().value();
    PASS_CHECK(ajfx::light_spacing(effect, effect.emitters[0]) == 3U);
    render::ParticlePass pass = make_pass();
    render::ParticleInstance instance = make_instance(pass, effect);
    PASS_CHECK(pass.light_count(instance) == 7U);

    scene::Mat4 placed = identity();
    placed.set(0U, 3U, 2.0F); // two units along X
    // From slot 3, as if an earlier effect held 0 to 2.
    const std::vector<float> words = lights_at(pass, instance, 1U, placed, 3U, 7U);
    const ajfx::EmitterState many = instance.simulation().read_emitter(0U).value();
    PASS_CHECK(many.alive() == 10U);
    const auto slot = [&](core::u32 index) { return std::span<const float>{words.data() + index * 32U, 32U}; };
    for (core::u32 k = 0U; k < 4U; ++k) {
        const std::span<const float> light = slot(3U + k);
        PASS_CHECK(many.spawn_index[3U * k] == 3U * k);
        const ajfx::Vec3 p = many.position[3U * k];
        PASS_CHECK(light[0] == p.x + 2.0F && light[1] == p.y && light[2] == p.z);
        std::uint32_t kind = 0U;
        std::memcpy(&kind, &light[3], 4U);
        PASS_CHECK(kind == 1U); // a point light
        // Colour by alpha, then the intensity; the range; no shadow row; no
        // profile.
        PASS_CHECK(light[4] == 0.5F && light[5] == 0.25F && light[6] == 0.125F && light[7] == 3.0F);
        PASS_CHECK(light[8] == 2.5F && light[11] == -1.0F && light[19] == -1.0F);
    }
    // The second emitter: its one particle, then two slots of nothing.
    PASS_CHECK(slot(7U)[7] == 1.0F && slot(8U)[7] == 0.0F && slot(9U)[7] == 0.0F);
    // Slots before `first_slot` are left alone.
    PASS_CHECK(slot(0U)[7] == 0.0F && slot(2U)[0] == 0.0F);
}

void particle_lights_stay_with_their_particles() {
    // A stream whose particles live different lengths, so births and deaths
    // keep changing which particles are alive. Each light belongs to one
    // particle for its life: its slot follows from the particle's spawn
    // index, its intensity and reach are picked for it within their ranges,
    // and its own colour and intensity follow the particle's age.
    ajfx::EffectBuilder builder{"stream"};
    builder.emitter("e")
        .spawn(ajfx::SpawnRate{.rate = 30.0F})
        .init(ajfx::Lifetime{.min = 0.3F, .max = 1.5F})
        .init(ajfx::ShapeLocation{.shape = ajfx::Shape::Sphere, .radius_min = 2.0F, .radius_max = 2.0F})
        .renderer(ajfx::SpriteRenderer{
            .light_count = 4U,
            .light_intensity_min = 1.0F,
            .light_intensity_max = 3.0F,
            .light_range_min = 2.0F,
            .light_range_max = 4.0F,
            .light_own_color = true,
            .light_colors = {{0.0F, ajfx::Color{1.0F, 0.0F, 0.0F, 1.0F}}, {1.0F, ajfx::Color{0.0F, 0.0F, 1.0F, 0.5F}}},
            .light_curve = {{0.0F, 0.0F}, {0.5F, 1.0F}, {1.0F, 0.0F}}});
    const ajfx::Effect effect = builder.build().value();
    const core::u32 spacing = ajfx::light_spacing(effect, effect.emitters[0]);
    PASS_CHECK(spacing > 1U);
    render::ParticlePass pass = make_pass(render::ParticleSettings{.step_budget = 256U});
    render::ParticleInstance instance = make_instance(pass, effect);

    // Which particle each slot lit, by spawn index, at two show frames a
    // quarter of a second apart.
    std::array<std::optional<std::uint32_t>, 4U> earlier{};
    std::array<std::optional<std::uint32_t>, 4U> later{};
    std::vector<float> reaches;
    for (const core::u64 frame : {48U, 54U}) {
        const std::vector<float> words = lights_at(pass, instance, frame, identity(), 0U, 4U);
        const ajfx::EmitterState state = instance.simulation().read_emitter(0U).value();
        std::size_t carriers = 0U;
        for (std::size_t i = 0U; i < state.alive(); ++i) {
            carriers += state.spawn_index[i] % spacing == 0U ? 1U : 0U;
        }
        PASS_CHECK(carriers <= 4U);
        std::size_t lit = 0U;
        for (core::u32 k = 0U; k < 4U; ++k) {
            const float* light = words.data() + k * 32U;
            // The live carrier that owns slot k, if any.
            std::optional<std::size_t> owner;
            for (std::size_t i = 0U; i < state.alive(); ++i) {
                const std::uint32_t s = state.spawn_index[i];
                if (s % spacing == 0U && (s / spacing) % 4U == k) {
                    owner = i;
                }
            }
            if (!owner) {
                PASS_CHECK(light[7] == 0.0F);
                continue;
            }
            ++lit;
            const std::size_t i = *owner;
            const ajfx::Vec3 p = state.position[i];
            PASS_CHECK(light[0] == p.x && light[1] == p.y && light[2] == p.z);
            const float t = std::clamp(state.age[i] / state.lifetime[i], 0.0F, 1.0F);
            // Red to half-strength blue, by alpha.
            const float alpha = 1.0F - 0.5F * t;
            PASS_CHECK(std::fabs(light[4] - (1.0F - t) * alpha) < 1.0e-4F);
            PASS_CHECK(std::fabs(light[6] - t * alpha) < 1.0e-4F && light[5] == 0.0F);
            const float curve = t <= 0.5F ? t * 2.0F : (1.0F - t) * 2.0F;
            PASS_CHECK(light[7] >= 1.0F * curve - 1.0e-4F && light[7] <= 3.0F * curve + 1.0e-4F);
            PASS_CHECK(light[8] >= 2.0F && light[8] <= 4.0F);
            reaches.push_back(light[8]);
            (frame == 48U ? earlier : later)[k] = state.spawn_index[i];
        }
        PASS_CHECK(lit == carriers && lit != 0U);
    }
    // A particle lit at both frames kept its slot, and so its light.
    bool kept = false;
    for (core::u32 k = 0U; k < 4U; ++k) {
        for (core::u32 j = 0U; j < 4U; ++j) {
            if (earlier[k] && later[j] && *earlier[k] == *later[j]) {
                PASS_CHECK(k == j);
                kept = true;
            }
        }
    }
    PASS_CHECK(kept);
    // Each particle picks its own reach.
    PASS_CHECK(std::adjacent_find(reaches.begin(), reaches.end(), std::not_equal_to<>{}) != reaches.end());
    g_device->wait_idle();
}

void ribbons_join_the_particles_in_birth_order() {
    // Particles born every half second, drifting +X at a unit a second; at
    // show frame 48 (two seconds) they are a quarter unit across at about
    // x = 0, 0.5, 1 and 1.5. Half way between two (x = 0.75, pixel 44) only
    // a ribbon draws.
    const auto trail = [](ajfx::Geometry geometry) {
        ajfx::EffectBuilder builder{"trail"};
        builder.emitter("e")
            .spawn(ajfx::SpawnRate{.rate = 2.0F})
            .init(ajfx::Lifetime{.min = 10.0F, .max = 10.0F})
            .init(ajfx::ShapeLocation{.shape = ajfx::Shape::Point})
            .init(ajfx::Velocity{.direction = ajfx::Vec3{1.0F, 0.0F, 0.0F}})
            .init(ajfx::InitSize{.min = 0.25F, .max = 0.25F})
            .renderer(ajfx::SpriteRenderer{.geometry = geometry});
        return builder.build().value();
    };
    render::ParticlePass pass = make_pass(render::ParticleSettings{.step_budget = 256U});
    render::ParticleInstance sprites = make_instance(pass, trail(ajfx::Geometry::Sprites));
    render::ParticleInstance ribbon = make_instance(pass, trail(ajfx::Geometry::Ribbon));
    const Frame dots = render_frame(pass, sprites, 48U, identity());
    const Frame strip = render_frame(pass, ribbon, 48U, identity());
    PASS_CHECK(dots.alpha(44U, 32U) == 0U);
    // A quarter unit (four pixels) wide and soft across: 15/16 a quarter of
    // the way out, 7/16 at three quarters, nothing past the edge.
    PASS_CHECK(strip.alpha(44U, 32U) >= 237U && strip.alpha(44U, 32U) <= 241U);
    PASS_CHECK(strip.alpha(44U, 33U) >= 109U && strip.alpha(44U, 33U) <= 114U && strip.alpha(44U, 34U) == 0U);
}

[[nodiscard]] std::vector<std::uint8_t> reference_records(const ajfx::Effect& effect, core::u64 step) {
    const ajfx::gpu::KernelSet kernels = ajfx::gpu::compile_kernels(effect, *g_compiler).value();
    ajfx::gpu::GpuSimulation reference = ajfx::gpu::GpuSimulation::create(*g_reference, effect, kernels, 3U).value();
    reference.advance_to(step);
    return reference.read_particle_bytes(0U).value();
}

void seeks_hold_until_caught_up_then_match_playing_through() {
    render::ParticleSettings settings;
    settings.step_budget = 16U;
    settings.checkpoint_interval = 120U;
    settings.checkpoint_count = 4U;
    render::ParticlePass pass = make_pass(settings);
    const ajfx::Effect effect = ajfx::presets::smoke();
    render::ParticleInstance instance = make_instance(pass, effect);

    const auto advance = [&](core::u64 frame) {
        auto acquired = g_device->acquire_commands();
        PASS_CHECK(acquired);
        rhi::CommandList commands = std::move(acquired).value();
        const bool reached = pass.advance(commands, instance, frame, 24U, 1U);
        if (!reached) {
            // Behind: drawing is refused.
            rhi::Texture* none = nullptr;
            PASS_CHECK(!pass.draw(commands, instance, orthographic(), identity(), render::ParticleTargets{none, none}));
        }
        auto submitted = g_device->submit(std::move(commands));
        PASS_CHECK(submitted);
        PASS_CHECK(std::move(submitted).value().wait());
        PASS_CHECK(!pass.take_error());
        return reached;
    };

    // Playing: every 24 fps frame is two or three 60 Hz steps, within budget.
    for (core::u64 frame = 0U; frame <= 48U; ++frame) {
        PASS_CHECK(advance(frame));
    }
    PASS_CHECK(instance.step() == 120U);

    // A jump to 10 s (step 600) is 480 steps away: 30 frames of 16.
    int frames = 1;
    while (!advance(240U)) {
        ++frames;
        PASS_CHECK(frames < 100);
    }
    PASS_CHECK(frames == 30);
    PASS_CHECK(instance.simulation().read_particle_bytes(0U).value() == reference_records(effect, 600U));

    // Back to 250: the checkpoint at 240 is ten steps away, so one frame.
    PASS_CHECK(advance(100U));
    PASS_CHECK(instance.simulation().read_particle_bytes(0U).value() == reference_records(effect, 250U));
    // Forward to 490 reuses the checkpoint at 480 rather than stepping 240.
    PASS_CHECK(advance(196U));
    PASS_CHECK(instance.simulation().read_particle_bytes(0U).value() == reference_records(effect, 490U));
}

// 120 balls heaped on a floor and held for a quarter of a second.
[[nodiscard]] ajfx::Effect rigid_heap() {
    ajfx::EffectBuilder builder{"heap"};
    builder.emitter("stones")
        .capacity(160U)
        .seed(7U)
        .spawn(ajfx::SpawnPile{.count = 120U, .jitter = 0.25F, .release = 0.25F})
        .init(ajfx::Lifetime{.min = 30.0F, .max = 30.0F})
        .init(ajfx::InitSize{.min = 0.1F, .max = 0.2F})
        .init(ajfx::InitColor{.a = ajfx::Color{0.4F, 0.4F, 0.4F, 1.0F}, .b = ajfx::Color{0.7F, 0.6F, 0.5F, 1.0F}})
        .update(ajfx::Gravity{})
        .update(ajfx::RigidPhysics{.shape = ajfx::RigidShape::Sphere, .friction = 0.7F})
        .update(ajfx::PlaneCollision{.friction = 0.8F});
    return builder.build().value();
}

void rigid_physics_holds_until_its_thread_has_the_step() {
    render::ParticleSettings settings;
    settings.rigid_lead = 12U;
    render::ParticlePass pass = make_pass(settings);
    const ajfx::Effect effect = rigid_heap();
    render::ParticleInstance instance = make_instance(pass, effect);
    const std::unique_ptr<ajfx::RigidSolver> solver = ajfx::rigid::make_jolt_solver();

    const auto advance = [&](core::u64 frame) {
        auto acquired = g_device->acquire_commands();
        PASS_CHECK(acquired);
        rhi::CommandList commands = std::move(acquired).value();
        const bool reached = pass.advance(commands, instance, frame, 24U, 1U);
        if (!reached) {
            rhi::Texture* none = nullptr;
            PASS_CHECK(!pass.draw(commands, instance, orthographic(), identity(), render::ParticleTargets{none, none}));
        }
        auto submitted = g_device->submit(std::move(commands));
        PASS_CHECK(submitted);
        PASS_CHECK(std::move(submitted).value().wait());
        PASS_CHECK(!pass.take_error());
        return reached;
    };
    // Frames held before `frame` is drawn.
    const auto reach = [&](core::u64 frame) {
        int held = 0;
        while (!advance(frame)) {
            ++held;
            PASS_CHECK(held < 5000);
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return held;
    };
    const auto matches = [&](core::u64 step) {
        ajfx::Simulation reference =
            ajfx::Simulation::create(effect, 3U, ajfx::SimulationOptions{.rigid = solver.get()}).value();
        reference.advance_to(step);
        const ajfx::EmitterState drawn = instance.simulation().read_emitter(0U).value();
        const ajfx::EmitterState& expected = reference.emitter(0U);
        return drawn.alive() == expected.alive() && drawn.position == expected.position &&
               drawn.orientation == expected.orientation && drawn.velocity == expected.velocity &&
               drawn.color == expected.color && drawn.size == expected.size;
    };

    // The first frame waits for the thread to build the pile, which stands
    // at the start.
    const int first = reach(0U);
    PASS_CHECK(instance.step() == 0U && matches(0U));
    PASS_CHECK(instance.simulation().read_emitter(0U).value().alive() == 120U);
    // Playing two seconds, the thread works ahead of the frames.
    int playing = 0;
    for (core::u64 frame = 1U; frame <= 48U; ++frame) {
        playing += reach(frame);
    }
    PASS_CHECK(instance.step() == 120U && matches(120U));
    // Back to the start: the pile stands again, exactly.
    const int back = reach(0U);
    PASS_CHECK(matches(0U));
    std::cout << "  held " << first << " frames to start, " << playing << " while playing, " << back
              << " going back\n";
}

// A rigid physics emitter holding one cube prop that collides as `shape` and
// moves as `motion`, with its single particle parked ten units below the
// camera's view so only the prop's own pixels land in the frame. The prop
// stands a unit left of the effect's origin; a Driven one is moved a unit to
// its right over the first second. Either shape draws the model itself, so
// the pixels are the same whichever the artist collides as.
[[nodiscard]] ajfx::Effect cube_prop(ajfx::PropMotion motion, ajfx::PropShape shape) {
    ajfx::RigidProp cube{};
    cube.mesh = "cube";
    cube.shape = shape;
    cube.motion = motion;
    cube.position = ajfx::Vec3{-1.0F, 0.0F, 0.0F};
    cube.size = 1.0F;
    cube.to_position = ajfx::Vec3{1.0F, 0.0F, 0.0F};
    cube.move_from = 0.0F;
    cube.move_over = 1.0F;

    ajfx::EffectBuilder builder{"prop"};
    builder.emitter("debris")
        .capacity(8U)
        .spawn(ajfx::SpawnBurst{.count = 1U})
        .init(ajfx::Lifetime{.min = 30.0F, .max = 30.0F})
        .init(ajfx::ShapeLocation{.shape = ajfx::Shape::Point, .center = ajfx::Vec3{0.0F, -10.0F, 0.0F}})
        .init(ajfx::InitSize{.min = 0.1F, .max = 0.1F})
        .update(ajfx::RigidPhysics{.shape = ajfx::RigidShape::Sphere, .props = {cube}});
    return builder.build().value();
}

// One recording that only advances `instance` towards `frame`, submitted on
// its own: whether the pass reached it.
[[nodiscard]] bool advance_once(render::ParticlePass& pass, render::ParticleInstance& instance, core::u64 frame) {
    auto acquired = g_device->acquire_commands();
    PASS_CHECK(acquired);
    rhi::CommandList commands = std::move(acquired).value();
    const bool reached = pass.advance(commands, instance, frame, 24U, 1U);
    auto submitted = g_device->submit(std::move(commands));
    PASS_CHECK(submitted);
    PASS_CHECK(std::move(submitted).value().wait());
    PASS_CHECK(!pass.take_error());
    return reached;
}

// Advances to `frame` of a 24 fps show, waiting for the rigid physics thread
// to reach it, then draws with the depth ramp solid meshes need.
[[nodiscard]] Frame render_prop_frame(render::ParticlePass& pass, render::ParticleInstance& instance,
                                      core::u64 frame) {
    scene::ViewMatrices ramp = orthographic();
    ramp.projection.set(2U, 2U, 0.1F);
    ramp.view_projection = ramp.projection;
    // Each attempt is its own recording, as a frame path's would be: the pass
    // refuses to draw until the tick's props have landed.
    for (int held = 0; !advance_once(pass, instance, frame); ++held) {
        PASS_CHECK(held < 5000);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    rhi::TextureDesc color_desc{};
    color_desc.width = size;
    color_desc.height = size;
    color_desc.format = rhi::TextureFormat::Rgba8Unorm;
    color_desc.color_target = true;
    color_desc.sampled = true;
    rhi::TextureDesc depth_desc{};
    depth_desc.width = size;
    depth_desc.height = size;
    depth_desc.format = rhi::TextureFormat::Depth32Float;
    depth_desc.depth_target = true;
    auto color = g_device->create_texture(color_desc);
    auto depth = g_device->create_texture(depth_desc);
    auto readback = g_device->create_readback(size * size * 4U);
    PASS_CHECK(color && depth && readback);
    auto acquired = g_device->acquire_commands();
    PASS_CHECK(acquired);
    rhi::CommandList commands = std::move(acquired).value();
    PASS_CHECK(pass.advance(commands, instance, frame, 24U, 1U));
    render::ParticleTargets targets{};
    targets.color = &color.value();
    targets.depth = &depth.value();
    targets.load = false;
    targets.clear = rhi::Color{0.0F, 0.0F, 0.0F, 0.0F};
    PASS_CHECK(pass.draw(commands, instance, ramp, identity(), targets));
    PASS_CHECK(commands.download(color.value(), readback.value()));
    auto submitted = g_device->submit(std::move(commands));
    PASS_CHECK(submitted);
    PASS_CHECK(std::move(submitted).value().wait());
    PASS_CHECK(!pass.take_error());
    const auto mapped = readback.value().map();
    Frame out{std::vector<core::u8>(mapped.begin(), mapped.begin() + size * size * 4U)};
    readback.value().unmap();
    return out;
}

// The leftmost and rightmost columns of row 32 the prop covered.
[[nodiscard]] std::pair<int, int> lit_columns(const Frame& frame) {
    int from = -1;
    int to = -1;
    for (core::u32 x = 0U; x < size; ++x) {
        if (frame.alpha(x, 32U) != 0U) {
            from = from < 0 ? static_cast<int>(x) : from;
            to = static_cast<int>(x);
        }
    }
    return {from, to};
}

// The cube model at half its size, so a body drawn as it covers a quarter of
// the pixels a body drawn as the cube does.
[[nodiscard]] ajfx::MeshData half_cube_mesh() {
    const ajfx::MeshData cube = ajfx::cube_mesh();
    std::vector<ajfx::MeshVertex> vertices = cube.vertices;
    for (ajfx::MeshVertex& vertex : vertices) {
        vertex.position = ajfx::Vec3{vertex.position.x * 0.5F, vertex.position.y * 0.5F, vertex.position.z * 0.5F};
    }
    return ajfx::make_mesh(std::move(vertices), cube.indices).value();
}

// A rigid physics emitter whose two body kinds are drawn as two different
// models: the emitter's own cube for kind 0, the half cube for kind 1. One
// body of each stands still, a little either side of the origin, so each
// model's pixels land on its own half of the frame. Nothing pulls on them and
// the two spheres do not touch, so they stay where the piles put them.
[[nodiscard]] ajfx::Effect two_models_in_one_emitter() {
    ajfx::EffectBuilder builder{"models"};
    builder.emitter("pieces")
        .capacity(4U)
        .spawn(ajfx::SpawnPile{.count = 1U,
                               .shape = ajfx::PileShape::Box,
                               .center = ajfx::Vec3{-0.5F, -0.25F, 0.0F},
                               .jitter = 0.0F,
                               .release = 0.0F,
                               .body = 0U})
        .spawn(ajfx::SpawnPile{.count = 1U,
                               .shape = ajfx::PileShape::Box,
                               .center = ajfx::Vec3{0.5F, -0.25F, 0.0F},
                               .jitter = 0.0F,
                               .release = 0.0F,
                               .body = 1U})
        .init(ajfx::Lifetime{.min = 30.0F, .max = 30.0F})
        .init(ajfx::InitColor{.a = ajfx::Color{1.0F, 0.0F, 0.0F, 1.0F}, .b = ajfx::Color{1.0F, 0.0F, 0.0F, 1.0F}})
        .init(ajfx::InitSize{.min = 0.5F, .max = 0.5F})
        .update(ajfx::RigidPhysics{
            .shape = ajfx::RigidShape::Sphere,
            .bodies = {ajfx::RigidBodyKind{.shape = ajfx::RigidShape::Sphere, .draw = "half"}}})
        .renderer(ajfx::SpriteRenderer{.geometry = ajfx::Geometry::Mesh, .mesh = "cube"});
    return builder.build().value();
}

void body_kinds_draw_as_their_own_models() {
    const ajfx::MeshSet meshes{{"cube", ajfx::cube_mesh()}, {"half", half_cube_mesh()}};
    const ajfx::Effect effect = two_models_in_one_emitter();
    // A kind that names a model is what asks the package for it.
    PASS_CHECK(!ajfx::check_meshes(effect, ajfx::MeshSet{{"cube", ajfx::cube_mesh()}}).ok());
    const ajfx::gpu::KernelSet kernels = ajfx::gpu::compile_kernels(effect, *g_compiler).value();
    render::ParticlePass pass = make_pass();
    auto created = pass.create_instance(effect, kernels, ajfx::TextureSet{}, ajfx::FieldSet{}, meshes, 3U);
    PASS_CHECK(created);
    render::ParticleInstance instance = std::move(created).value();
    const Frame frame = render_prop_frame(pass, instance, 12U);

    // Both bodies stand where their piles put them, one either side.
    const auto read = instance.simulation().read_emitter(0U);
    PASS_CHECK(read.ok());
    const ajfx::EmitterState& particles = read.value();
    // Read back from the records the GPU holds, which carry no body kind:
    // which kind each particle is is the simulation's, and tested there.
    PASS_CHECK(particles.alive() == 2U);
    PASS_CHECK(particles.position[0].x < -0.2F && particles.position[1].x > 0.2F);
    for (std::size_t b = 0U; b < 2U; ++b) {
        PASS_CHECK(std::fabs(particles.position[b].y) < 0.5F);
    }

    // The orthographic test view spans four world units across a frame of
    // `size`, so a world unit is size/4 pixels (a unit cube covers 256 of
    // them, as the cube tests above count). A cube of side 0.5 seen face on
    // covers that squared; the half cube is half the side, a quarter of the
    // pixels. Each lands on its own side of the middle.
    const auto side = static_cast<core::u32>(0.5F * static_cast<float>(size) / 4.0F);
    const std::size_t whole = static_cast<std::size_t>(side) * side;
    std::size_t left = 0U;
    std::size_t right = 0U;
    for (core::u32 y = 0U; y < size; ++y) {
        for (core::u32 x = 0U; x < size; ++x) {
            if (frame.alpha(x, y) == 0U) {
                continue;
            }
            if (x < size / 2U) {
                ++left;
            } else {
                ++right;
            }
        }
    }
    std::cout << "  the cube covered " << left << " pixels (expected " << whole << "), the half cube " << right
              << " (expected " << whole / 4U << ")\n";
    PASS_CHECK(left >= whole - side && left <= whole + side);
    PASS_CHECK(right >= whole / 4U - side && right <= whole / 4U + side);
}

void props_draw_where_they_stand() {
    const ajfx::MeshSet meshes{{"cube", ajfx::cube_mesh()}};
    const auto instance_of = [&](render::ParticlePass& pass, const ajfx::Effect& effect) {
        const ajfx::gpu::KernelSet kernels = ajfx::gpu::compile_kernels(effect, *g_compiler).value();
        auto created = pass.create_instance(effect, kernels, ajfx::TextureSet{}, ajfx::FieldSet{}, meshes, 3U);
        if (!created) {
            std::cout << created.error().message << '\n';
        }
        PASS_CHECK(created);
        return std::move(created).value();
    };

    // A fixed cube prop, a unit (16 pixels) across, standing a unit left of
    // the effect's origin: 16 pixels left of the middle, so columns 8 to 23
    // and rows 24 to 39. It carries no colour of its own, and its front face
    // squarely faces the camera's light, so it is solid white.
    render::ParticlePass pass = make_pass();
    render::ParticleInstance fixed =
        instance_of(pass, cube_prop(ajfx::PropMotion::Fixed, ajfx::PropShape::Mesh));
    const Frame standing = render_prop_frame(pass, fixed, 1U);
    PASS_CHECK(standing.red(16U, 32U) >= 254U && standing.green(16U, 32U) >= 254U &&
               standing.blue(16U, 32U) >= 254U && standing.alpha(16U, 32U) >= 254U);
    PASS_CHECK(standing.alpha(8U, 24U) >= 254U && standing.alpha(23U, 39U) >= 254U);
    PASS_CHECK(standing.alpha(7U, 32U) == 0U && standing.alpha(24U, 32U) == 0U);
    PASS_CHECK(standing.alpha(16U, 23U) == 0U && standing.alpha(16U, 40U) == 0U);
    // The emitter's one particle is ten units below: nothing else drew.
    PASS_CHECK(lit_columns(standing) == std::pair{8, 23});
    PASS_CHECK(standing.alpha(32U, 32U) == 0U && standing.alpha(48U, 32U) == 0U);

    // Drawing the same show frame again gives the same bytes: the pose comes
    // from the rigid frame for that tick and nothing else.
    const Frame again = render_prop_frame(pass, fixed, 1U);
    PASS_CHECK(again.pixels == standing.pixels);

    // A driven prop is moved a unit right over the first second. Held at one
    // show frame it does not budge, by the byte; a later frame finds it
    // further right, and coming back to the first frame finds it exactly
    // where it was.
    render::ParticleInstance driven =
        instance_of(pass, cube_prop(ajfx::PropMotion::Driven, ajfx::PropShape::Hull));
    const Frame start = render_prop_frame(pass, driven, 0U);
    PASS_CHECK(lit_columns(start) == std::pair{8, 23});
    PASS_CHECK(render_prop_frame(pass, driven, 0U).pixels == start.pixels);
    const Frame moved = render_prop_frame(pass, driven, 12U);
    const auto [from, to] = lit_columns(moved);
    PASS_CHECK(from > 8 && to > 23 && to - from == 15);
    PASS_CHECK(render_prop_frame(pass, driven, 12U).pixels == moved.pixels);
    PASS_CHECK(render_prop_frame(pass, driven, 0U).pixels == start.pixels);
}

// Advances to `frame`, waiting for the rigid physics thread, then draws the
// colour and the motion into a velocity target that starts at 7 in every
// channel, as render_motion does for particles.
[[nodiscard]] std::vector<float> render_prop_motion(render::ParticlePass& pass, render::ParticleInstance& instance,
                                                    core::u64 frame, bool valid) {
    for (int held = 0; !advance_once(pass, instance, frame); ++held) {
        PASS_CHECK(held < 5000);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    rhi::TextureDesc color_desc{};
    color_desc.width = size;
    color_desc.height = size;
    color_desc.format = rhi::TextureFormat::Rgba8Unorm;
    color_desc.color_target = true;
    color_desc.sampled = true;
    rhi::TextureDesc velocity_desc = color_desc;
    velocity_desc.format = rhi::TextureFormat::Rgba32Float;
    rhi::TextureDesc depth_desc{};
    depth_desc.width = size;
    depth_desc.height = size;
    depth_desc.format = rhi::TextureFormat::Depth32Float;
    depth_desc.depth_target = true;
    auto color = g_device->create_texture(color_desc);
    auto velocity = g_device->create_texture(velocity_desc);
    auto depth = g_device->create_texture(depth_desc);
    auto readback = g_device->create_readback(size * size * 16U);
    PASS_CHECK(color && velocity && depth && readback);

    auto acquired = g_device->acquire_commands();
    PASS_CHECK(acquired);
    rhi::CommandList commands = std::move(acquired).value();
    PASS_CHECK(pass.advance(commands, instance, frame, 24U, 1U));
    std::vector<float> sevens(static_cast<std::size_t>(size) * size * 4U, 7.0F);
    PASS_CHECK(commands.upload(velocity.value(),
                               std::span<const core::u8>{reinterpret_cast<const core::u8*>(sevens.data()),
                                                         sevens.size() * sizeof(float)}));
    const auto view = orthographic();
    render::ParticleTargets targets{};
    targets.color = &color.value();
    targets.depth = &depth.value();
    targets.load = false;
    targets.clear = rhi::Color{0.0F, 0.0F, 0.0F, 0.0F};
    targets.velocity = &velocity.value();
    targets.canvas_view_projection = view.view_projection;
    targets.previous_view_projection = view.view_projection;
    targets.motion_seconds = 1.0F / 24.0F;
    targets.motion_valid = valid;
    PASS_CHECK(pass.draw(commands, instance, view, identity(), targets));
    PASS_CHECK(pass.draw_motion(commands, instance, view, identity(), targets));
    PASS_CHECK(commands.download(velocity.value(), readback.value()));
    auto submitted = g_device->submit(std::move(commands));
    PASS_CHECK(submitted);
    PASS_CHECK(std::move(submitted).value().wait());
    PASS_CHECK(!pass.take_error());
    const auto mapped = readback.value().map();
    std::vector<float> out(static_cast<std::size_t>(size) * size * 4U);
    std::memcpy(out.data(), mapped.data(), out.size() * sizeof(float));
    readback.value().unmap();
    return out;
}

void props_write_their_motion() {
    const ajfx::MeshSet meshes{{"cube", ajfx::cube_mesh()}};
    const auto instance_of = [&](render::ParticlePass& pass, const ajfx::Effect& effect) {
        const ajfx::gpu::KernelSet kernels = ajfx::gpu::compile_kernels(effect, *g_compiler).value();
        auto created = pass.create_instance(effect, kernels, ajfx::TextureSet{}, ajfx::FieldSet{}, meshes, 3U);
        PASS_CHECK(created);
        return std::move(created).value();
    };
    const auto at = [](const std::vector<float>& pixels, core::u32 x, core::u32 y, core::u32 channel) {
        return pixels[(static_cast<std::size_t>(y) * size + x) * 4U + channel];
    };
    render::ParticlePass pass = make_pass();

    // A fixed prop never moves, so it writes no motion at all, only the
    // depth the flat test projection puts everything at. Its pixels still
    // replace the scene's motion where it covers them, as a sprite's do.
    render::ParticleInstance fixed =
        instance_of(pass, cube_prop(ajfx::PropMotion::Fixed, ajfx::PropShape::Mesh));
    const std::vector<float> standing = render_prop_motion(pass, fixed, 1U, true);
    PASS_CHECK(at(standing, 16U, 32U, 0U) == 0.0F && at(standing, 16U, 32U, 1U) == 0.0F);
    PASS_CHECK(at(standing, 16U, 32U, 2U) == 0.5F && at(standing, 16U, 32U, 3U) == 0.5F);
    PASS_CHECK(at(standing, 40U, 32U, 0U) == 7.0F && at(standing, 2U, 2U, 0U) == 7.0F);

    // A driven prop moves by the velocity its pose reports, exactly as a
    // particle moves by its own. It travels two units over the first second,
    // smoothed at both ends (t*t*(3-2t)), so at show frame 12 -- half a
    // second, step 30 of 60 -- it is at its fastest: 2 * 6t(1-t) = 3 units a
    // second. The camera maps a unit to half the clip range, and a 24th of a
    // second of that is half again in UV: 0.25 * 3 / 24. The solver reports
    // the step's own difference rather than that instant's derivative, which
    // is the whole of the tolerance.
    render::ParticleInstance driven =
        instance_of(pass, cube_prop(ajfx::PropMotion::Driven, ajfx::PropShape::Hull));
    const std::vector<float> moving = render_prop_motion(pass, driven, 12U, true);
    float covered = 0.0F;
    core::u32 column = size;
    for (core::u32 x = 0U; x < size; ++x) {
        if (at(moving, x, 32U, 0U) != 7.0F) {
            column = column == size ? x : column;
            covered += 1.0F;
        }
    }
    PASS_CHECK(covered == 16.0F && column > 8U);
    std::cout << "  driven prop motion x " << at(moving, column + 8U, 32U, 0U) << ", expected "
              << 0.25F * 3.0F / 24.0F << '\n';
    PASS_CHECK(std::abs(at(moving, column + 8U, 32U, 0U) - 0.25F * 3.0F / 24.0F) < 1.0e-4F);
    PASS_CHECK(at(moving, column + 8U, 32U, 1U) == 0.0F);
    PASS_CHECK(at(moving, column + 8U, 32U, 2U) == 0.5F);

    // With no previous frame there is nothing to move from: depth only.
    render::ParticleInstance fresh =
        instance_of(pass, cube_prop(ajfx::PropMotion::Driven, ajfx::PropShape::Hull));
    const std::vector<float> first = render_prop_motion(pass, fresh, 12U, false);
    PASS_CHECK(at(first, column + 8U, 32U, 0U) == 0.0F && at(first, column + 8U, 32U, 2U) == 0.5F);
}

void devices_without_particles_are_refused() {
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::SdlGpu;
    auto sdl = rhi::Device::create(config);
    if (sdl) {
        auto refused = render::ParticlePass::create(sdl.value(), rhi::TextureFormat::Rgba8Unorm);
        PASS_CHECK(!refused && refused.error().code == render::ParticleErrorCode::Unsupported);
    }
}

} // namespace

int main() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        std::cout << "skipped: no D3D12 device\n";
        return 77;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
#if !defined(NDEBUG)
    config.debug = true;
#endif
    auto device = rhi::Device::create(config);
    ajfx::Result<std::unique_ptr<ajfx::gpu::ShaderCompiler>> compiler = ajfx::d3d12::create_shader_compiler();
    ajfx::Result<std::unique_ptr<ajfx::d3d12::Device>> reference = ajfx::d3d12::create_device();
    if (!device || !compiler || !reference) {
        std::cout << "skipped: " << (!compiler ? compiler.error().message : "no D3D12 device") << '\n';
        return 77;
    }
    g_device = std::make_unique<rhi::Device>(std::move(device).value());
    g_compiler = std::move(compiler).value();
    g_reference = std::move(reference).value();

    struct Case final {
        const char* name;
        void (*run)();
    };
    const Case cases[] = {
        {"a_particle_draws_where_the_camera_sees_it", a_particle_draws_where_the_camera_sees_it},
        {"the_scene_depth_hides_particles_behind_it", the_scene_depth_hides_particles_behind_it},
        {"additive_particles_add_and_cover_as_they_brighten", additive_particles_add_and_cover_as_they_brighten},
        {"alpha_particles_draw_back_to_front_in_the_placed_effect",
         alpha_particles_draw_back_to_front_in_the_placed_effect},
        {"flipbooks_play_their_cells_over_each_life", flipbooks_play_their_cells_over_each_life},
        {"moving_particles_write_their_motion", moving_particles_write_their_motion},
        {"soft_particles_fade_as_they_near_the_scene", soft_particles_fade_as_they_near_the_scene},
        {"ribbons_join_the_particles_in_birth_order", ribbons_join_the_particles_in_birth_order},
        {"distance_fields_reach_the_simulation", distance_fields_reach_the_simulation},
        {"mesh_particles_draw_solid_meshes", mesh_particles_draw_solid_meshes},
        {"tumbling_meshes_turn_by_their_orientation", tumbling_meshes_turn_by_their_orientation},
        {"particle_lights_are_written_for_the_lit_pass", particle_lights_are_written_for_the_lit_pass},
        {"particle_lights_stay_with_their_particles", particle_lights_stay_with_their_particles},
        {"seeks_hold_until_caught_up_then_match_playing_through",
         seeks_hold_until_caught_up_then_match_playing_through},
        {"rigid_physics_holds_until_its_thread_has_the_step", rigid_physics_holds_until_its_thread_has_the_step},
        {"body_kinds_draw_as_their_own_models", body_kinds_draw_as_their_own_models},
        {"props_draw_where_they_stand", props_draw_where_they_stand},
        {"props_write_their_motion", props_write_their_motion},
        {"devices_without_particles_are_refused", devices_without_particles_are_refused},
    };
    int failures = 0;
    for (const Case& test : cases) {
        try {
            test.run();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    g_reference.reset();
    g_compiler.reset();
    g_device.reset();
    return failures == 0 ? 0 : 1;
}
