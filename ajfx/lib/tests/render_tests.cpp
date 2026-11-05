// getenv is fine for reading one test switch.
#define _CRT_SECURE_NO_WARNINGS

#include "check.hpp"

#include <ajfx/builder.hpp>
#include <ajfx/d3d12.hpp>
#include <ajfx/gpu/renderer.hpp>
#include <ajfx/presets.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using namespace ajfx;

constexpr std::uint32_t size = 64U;

std::unique_ptr<gpu::Device> g_device;
std::unique_ptr<gpu::ShaderCompiler> g_compiler;

struct Pixel final {
    int r, g, b, a;
};

class Frame final {
public:
    explicit Frame(std::vector<std::uint8_t> bytes) : bytes_{std::move(bytes)} {}
    [[nodiscard]] Pixel at(std::uint32_t x, std::uint32_t y) const {
        const std::size_t i = (static_cast<std::size_t>(y) * size + x) * 4U;
        return {bytes_[i], bytes_[i + 1U], bytes_[i + 2U], bytes_[i + 3U]};
    }
    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const { return bytes_; }
    [[nodiscard]] std::size_t lit() const {
        std::size_t count = 0U;
        for (std::size_t i = 3U; i < bytes_.size(); i += 4U) {
            count += bytes_[i] != 0U ? 1U : 0U;
        }
        return count;
    }

private:
    std::vector<std::uint8_t> bytes_;
};

// Steps an effect, draws it into a fresh 64x64 RGBA8 target cleared to
// transparent black under `light`, and reads the pixels back.
Frame render(const Effect& effect, std::uint64_t steps, const gpu::Camera& camera, const TextureSet& textures = {},
             const gpu::SceneLight& light = gpu::SceneLight{}) {
    const gpu::KernelSet kernels = gpu::compile_kernels(effect, *g_compiler).value();
    gpu::GpuSimulation simulation = gpu::GpuSimulation::create(*g_device, effect, kernels).value();
    Result<gpu::ParticleRenderer> created =
        gpu::ParticleRenderer::create(*g_device, effect, kernels, textures, gpu::TextureFormat::Rgba8Unorm);
    if (!created) {
        std::cout << created.error().message << std::endl;
    }
    AJFX_CHECK(created.ok());
    gpu::ParticleRenderer renderer = std::move(created).value();
    const gpu::Texture target =
        g_device->create_render_target(size, size, gpu::TextureFormat::Rgba8Unorm, "test target").value();
    gpu::ParticleSorter sorter = gpu::ParticleSorter::create(*g_device, effect, kernels).value();
    simulation.advance_to(steps);
    const std::array<float, 4> clear{0.0F, 0.0F, 0.0F, 0.0F};
    renderer.render(simulation, sorter, camera, target, &clear, light);
    Result<std::vector<std::uint8_t>> pixels = g_device->read_texture(target);
    if (!pixels) {
        std::cout << pixels.error().message << '\n';
    }
    AJFX_CHECK(pixels.ok());
    g_device->destroy_texture(target);
    return Frame{std::move(pixels).value()};
}

// One burst of `count` particles at `position`, size `diameter`, never moving.
Effect still(std::uint32_t count, Vec3 position, float diameter, Color color, BlendMode blend) {
    EffectBuilder builder{"still"};
    builder.emitter("e")
        .spawn(SpawnBurst{.count = count})
        .init(Lifetime{.min = 10.0F, .max = 10.0F})
        .init(ShapeLocation{.shape = Shape::Point, .center = position})
        .init(InitColor{.a = color, .b = color})
        .init(InitSize{.min = diameter, .max = diameter})
        .renderer(SpriteRenderer{.blend = blend});
    return builder.build().value();
}

// Orthographic, 4 world units across 64 pixels: 16 pixels per unit.
const gpu::Camera ortho = gpu::Camera::orthographic(Vec3{}, 2.0F, 2.0F);

void a_sprite_is_a_soft_disc_where_its_particle_is() {
    const Frame frame = render(still(1U, Vec3{}, 1.0F, Color{1.0F, 0.0F, 0.0F, 1.0F}, BlendMode::Alpha), 1U, ortho);
    // The pixel at (32, 32) has its centre 1/32 unit from the particle in x
    // and y: uv = 1/16, coverage = 1 - 2/256.
    const Pixel centre = frame.at(32U, 32U);
    AJFX_CHECK(centre.r >= 251 && centre.r <= 254 && centre.g == 0 && centre.b == 0 && centre.a == centre.r);
    // Six pixels right: x = 13/32 unit, uv.x = 13/16; coverage = 1 - (169 + 1) / 256 = 0.336.
    const Pixel edge = frame.at(38U, 32U);
    AJFX_CHECK(edge.r >= 83 && edge.r <= 88);
    // Ten pixels right is outside the half-unit quad.
    AJFX_CHECK(frame.at(42U, 32U).a == 0);
    AJFX_CHECK(frame.at(0U, 0U).a == 0);
    // The disc is about pi * 8^2 pixels.
    AJFX_CHECK(frame.lit() > 180U && frame.lit() < 220U);
}

void blend_modes_combine_overlapping_particles() {
    const Color grey{0.25F, 0.25F, 0.25F, 1.0F};
    // Additive: two quarter-grey particles sum to about half.
    const Pixel added = render(still(2U, Vec3{}, 1.0F, grey, BlendMode::Additive), 1U, ortho).at(32U, 32U);
    AJFX_CHECK(added.r >= 124 && added.r <= 128);
    // Alpha: the second, nearly opaque, mostly covers the first.
    const Pixel over = render(still(2U, Vec3{}, 1.0F, grey, BlendMode::Alpha), 1U, ortho).at(32U, 32U);
    AJFX_CHECK(over.r >= 62 && over.r <= 65 && over.a >= 254);
    // Premultiplied: the colour is used as given, so half-alpha white adds
    // 0.5 of white over what is below.
    const Pixel premultiplied =
        render(still(1U, Vec3{}, 1.0F, Color{0.5F, 0.5F, 0.5F, 0.5F}, BlendMode::Premultiplied), 1U, ortho).at(32U, 32U);
    AJFX_CHECK(premultiplied.r >= 125 && premultiplied.r <= 128 && premultiplied.a >= 125 && premultiplied.a <= 128);
}

void draws_follow_the_live_count() {
    EffectBuilder builder{"brief"};
    builder.emitter("e")
        .spawn(SpawnBurst{.count = 5U})
        .init(Lifetime{.min = 0.1F, .max = 0.1F})
        .init(ShapeLocation{.shape = Shape::Point})
        .init(InitSize{.min = 1.0F, .max = 1.0F});
    const Effect effect = builder.build().value();
    AJFX_CHECK(render(effect, 1U, ortho).lit() > 100U);
    // After their tenth of a second the instance count is zero: nothing draws.
    AJFX_CHECK(render(effect, 30U, ortho).lit() == 0U);
}

void perspective_puts_particles_where_the_projection_says() {
    const gpu::Camera camera =
        gpu::Camera::look_at(Vec3{0.0F, 0.0F, 5.0F}, Vec3{}, Vec3{0.0F, 1.0F, 0.0F}, 1.0471976F, 1.0F, 0.1F, 100.0F);
    // A point one unit right of the target, five units away with a 60 degree
    // view: x_ndc = (1 / tan 30) / 5 = 0.3464, pixel (0.3464 + 1) * 32 = 43.1.
    const Frame frame = render(still(1U, Vec3{1.0F, 0.0F, 0.0F}, 0.2F, Color{}, BlendMode::Alpha), 1U, camera);
    AJFX_CHECK(frame.at(43U, 32U).a > 0 || frame.at(43U, 31U).a > 0);
    AJFX_CHECK(frame.at(32U, 32U).a == 0);
    AJFX_CHECK(frame.at(21U, 32U).a == 0); // not mirrored
    // And one unit up appears above the centre (smaller row).
    const Frame above = render(still(1U, Vec3{0.0F, 1.0F, 0.0F}, 0.2F, Color{}, BlendMode::Alpha), 1U, camera);
    AJFX_CHECK(above.at(32U, 20U).a > 0 || above.at(31U, 20U).a > 0);
}

void velocity_aligned_sprites_stretch_along_their_motion() {
    // A particle moving right at 10 units a second, spanning 0.1 s of motion:
    // the quad's half-length along x is 0.5 + 10 * 0.1 / 2 = 1 unit, its
    // half-width across 0.5.
    EffectBuilder builder{"streak"};
    builder.emitter("e")
        .spawn(SpawnBurst{.count = 1U})
        .init(Lifetime{.min = 10.0F, .max = 10.0F})
        .init(ShapeLocation{.shape = Shape::Point})
        .init(Velocity{.direction = Vec3{1.0F, 0.0F, 0.0F}, .speed_min = 10.0F, .speed_max = 10.0F})
        .init(InitSize{.min = 1.0F, .max = 1.0F})
        .renderer(SpriteRenderer{.facing = Facing::Velocity, .stretch = 0.1F});
    const Effect effect = builder.build().value();
    // After one step it has moved 1/6 unit right; follow it with the camera.
    const Frame frame = render(effect, 1U, gpu::Camera::orthographic(Vec3{1.0F / 6.0F, 0.0F, 0.0F}, 2.0F, 2.0F));
    // 16 pixels per unit: lit 12 pixels along x either way, not 10 across,
    // and nothing past 16 along.
    AJFX_CHECK(frame.at(32U + 12U, 32U).a > 0 && frame.at(32U - 13U, 32U).a > 0);
    AJFX_CHECK(frame.at(32U, 32U + 10U).a == 0 && frame.at(32U, 32U - 11U).a == 0);
    AJFX_CHECK(frame.at(32U + 17U, 32U).a == 0 && frame.at(32U - 18U, 32U).a == 0);
}

void alpha_particles_draw_back_to_front() {
    // Two particles drift towards the camera: the first, born earlier, is
    // nearer and blue by then; the second, just born, is red. Birth order
    // would draw the red one over; sorted, the nearer blue one is on top.
    const auto pair = [](bool sort) {
        EffectBuilder builder{"pair"};
        builder.emitter("e")
            .spawn(SpawnBurst{.count = 1U, .interval = 0.3F, .cycles = 2U})
            .init(Lifetime{.min = 1.0F, .max = 1.0F})
            .init(ShapeLocation{.shape = Shape::Point})
            .init(Velocity{.direction = Vec3{0.0F, 0.0F, 1.0F}})
            .init(InitSize{.min = 1.0F, .max = 1.0F})
            .update(ColorOverLife{.keys = {{0.0F, Color{1.0F, 0.0F, 0.0F, 1.0F}}, {0.25F, Color{0.0F, 0.0F, 1.0F, 1.0F}}}})
            .renderer(SpriteRenderer{.blend = BlendMode::Alpha, .sort = sort});
        return builder.build().value();
    };
    // Twenty steps: the first is a third of a second old, the second 1/30.
    const Pixel sorted = render(pair(true), 20U, ortho).at(32U, 32U);
    AJFX_CHECK(sorted.b >= 240 && sorted.r <= 10);
    const Pixel unsorted = render(pair(false), 20U, ortho).at(32U, 32U);
    AJFX_CHECK(unsorted.r >= 200 && unsorted.b <= 40);
}

void emitters_interleave_by_depth() {
    // A grey smoke disc four units across at z = 0 fills the view, drawn
    // between two additive red sparks: one in front of it at x = -0.5
    // (pixel 24), emitted first, and one behind it at x = 0.5 (pixel 40),
    // emitted last. Emitter order would hide the near spark and show the far
    // one; by depth the near one shows and the far one is hidden.
    EffectBuilder builder{"interleaved"};
    const auto spark = [&](const char* name, Vec3 at) {
        const Color red{1.0F, 0.0F, 0.0F, 1.0F};
        builder.emitter(name)
            .spawn(SpawnBurst{.count = 1U})
            .init(Lifetime{.min = 10.0F, .max = 10.0F})
            .init(ShapeLocation{.shape = Shape::Point, .center = at})
            .init(InitColor{.a = red, .b = red})
            .init(InitSize{.min = 0.5F, .max = 0.5F})
            .renderer(SpriteRenderer{.blend = BlendMode::Additive, .sort = false});
    };
    spark("near", Vec3{-0.5F, 0.0F, 0.5F});
    const Color grey{0.25F, 0.25F, 0.25F, 1.0F};
    builder.emitter("smoke")
        .spawn(SpawnBurst{.count = 1U})
        .init(Lifetime{.min = 10.0F, .max = 10.0F})
        .init(ShapeLocation{.shape = Shape::Point})
        .init(InitColor{.a = grey, .b = grey})
        .init(InitSize{.min = 4.0F, .max = 4.0F})
        .renderer(SpriteRenderer{.blend = BlendMode::Alpha});
    spark("far", Vec3{0.5F, 0.0F, -0.5F});
    const Effect effect = builder.build().value();
    for (std::size_t e = 0U; e < 3U; ++e) {
        AJFX_CHECK(gpu::interleaved(effect, e));
    }
    const Frame frame = render(effect, 1U, ortho);
    // The smoke at x = 0.5 unit is a quarter of the way out: coverage
    // 1 - 1/16, so a premultiplied 0.234 grey. In front of it the spark adds
    // its full red; behind it the spark shows through the last 1/16 only.
    const Pixel near = frame.at(24U, 32U);
    AJFX_CHECK(near.r >= 250 && near.g >= 57 && near.g <= 62);
    const Pixel far = frame.at(40U, 32U);
    AJFX_CHECK(far.r >= 70 && far.r <= 80 && far.g >= 57 && far.g <= 62);
    // An effect with one such emitter, or with only additive ones, draws
    // as it always has.
    Effect sparks = effect;
    sparks.emitters.erase(sparks.emitters.begin() + 1);
    AJFX_CHECK(!gpu::interleaved(sparks, 0U) && !gpu::interleaved(sparks, 1U));
    AJFX_CHECK(!gpu::interleaved(still(1U, Vec3{}, 1.0F, grey, BlendMode::Alpha), 0U));
}

// Four solid 8x8 cells in a 16x16 texture: red, green, blue, white.
TextureSet cells() {
    const std::uint8_t colours[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
    std::vector<std::uint8_t> pixels;
    for (std::uint32_t y = 0U; y < 16U; ++y) {
        for (std::uint32_t x = 0U; x < 16U; ++x) {
            const std::uint8_t* c = colours[(y / 8U) * 2U + x / 8U];
            pixels.insert(pixels.end(), {c[0], c[1], c[2], 255});
        }
    }
    return TextureSet{{"cells", make_texture(16U, 16U, pixels).value()}};
}

Effect flipbook(bool blend) {
    EffectBuilder builder{"flipbook"};
    builder.emitter("e")
        .spawn(SpawnBurst{.count = 1U})
        .init(Lifetime{.min = 0.8F, .max = 0.8F})
        .init(ShapeLocation{.shape = Shape::Point})
        .init(InitSize{.min = 1.0F, .max = 1.0F})
        .renderer(SpriteRenderer{.texture = "cells", .flipbook_columns = 2U, .flipbook_rows = 2U,
                                 .flipbook_blend = blend});
    return builder.build().value();
}

void flipbooks_play_their_cells_over_each_life() {
    // A 0.8 s life over four cells: 0.2 s each.
    const auto colour = [](bool blend, std::uint64_t steps) {
        return render(flipbook(blend), steps, ortho, cells()).at(32U, 32U);
    };
    const Pixel first = colour(false, 3U);   // 0.05 s: red
    AJFX_CHECK(first.r >= 250 && first.g <= 5 && first.b <= 5 && first.a >= 250);
    const Pixel second = colour(false, 18U); // 0.3 s: green
    AJFX_CHECK(second.g >= 250 && second.r <= 5);
    const Pixel third = colour(false, 30U);  // 0.5 s: blue
    AJFX_CHECK(third.b >= 250 && third.r <= 5 && third.g <= 5);
    // Cross-fading: at 0.1 s, half way through the first cell, half red and
    // half green.
    const Pixel fading = colour(true, 6U);
    AJFX_CHECK(fading.r >= 100 && fading.r <= 155 && fading.g >= 100 && fading.g <= 155);
    // An effect naming a texture it was not given is refused.
    const gpu::KernelSet kernels = gpu::compile_kernels(flipbook(false), *g_compiler).value();
    const Result<gpu::ParticleRenderer> missing = gpu::ParticleRenderer::create(
        *g_device, flipbook(false), kernels, TextureSet{}, gpu::TextureFormat::Rgba8Unorm);
    AJFX_CHECK(!missing.ok() && missing.error().code == ErrorCode::MissingTexture);
}

// Particles born every half second at the origin, drifting +X at a unit a
// second: after two seconds, dots a quarter unit across at about x = 0, 0.5,
// 1 and 1.5, eight pixels apart in the orthographic view.
Effect trail(Geometry geometry) {
    EffectBuilder builder{"trail"};
    builder.emitter("e")
        .spawn(SpawnRate{.rate = 2.0F})
        .init(Lifetime{.min = 10.0F, .max = 10.0F})
        .init(ShapeLocation{.shape = Shape::Point})
        .init(Velocity{.direction = Vec3{1.0F, 0.0F, 0.0F}})
        .init(InitSize{.min = 0.25F, .max = 0.25F})
        .renderer(SpriteRenderer{.geometry = geometry});
    return builder.build().value();
}

void ribbons_join_the_particles_in_birth_order() {
    // Half way between two particles (x = 0.75, pixel 44): a gap between
    // sprites, filled by the ribbon.
    const Frame sprites = render(trail(Geometry::Sprites), 120U, ortho);
    const Frame ribbon = render(trail(Geometry::Ribbon), 120U, ortho);
    AJFX_CHECK(sprites.at(44U, 32U).a == 0);
    // A quarter unit (four pixels) wide about the line between rows 31 and
    // 32, soft across like the disc: 1 - side^2 at a quarter of the way out
    // (rows 31, 32) is 15/16, at three quarters (rows 30, 33) 7/16, and row
    // 34 is outside.
    AJFX_CHECK(ribbon.at(44U, 32U).a >= 237 && ribbon.at(44U, 32U).a <= 241);
    AJFX_CHECK(ribbon.at(44U, 33U).a >= 109 && ribbon.at(44U, 33U).a <= 114 && ribbon.at(44U, 34U).a == 0);
    // Ribbons are never depth-sorted, and face the camera.
    AJFX_CHECK(!gpu::sorts(trail(Geometry::Ribbon).emitters[0]));
    Effect turned = trail(Geometry::Ribbon);
    turned.emitters[0].renderer.facing = Facing::Velocity;
    AJFX_CHECK(!validate(turned).ok());
}

// One red unit cube at the origin, turned `degrees` about Y.
Effect cube(float degrees) {
    EffectBuilder builder{"cube"};
    builder.emitter("e")
        .spawn(SpawnBurst{.count = 1U})
        .init(Lifetime{.min = 10.0F, .max = 10.0F})
        .init(ShapeLocation{.shape = Shape::Point})
        .init(InitColor{.a = Color{1.0F, 0.0F, 0.0F, 1.0F}, .b = Color{1.0F, 0.0F, 0.0F, 1.0F}})
        .init(InitSize{.min = 1.0F, .max = 1.0F})
        .init(InitRotation{.min = degrees, .max = degrees})
        .renderer(SpriteRenderer{.geometry = Geometry::Mesh, .mesh = "cube"});
    return builder.build().value();
}

void mesh_particles_draw_their_mesh() {
    const MeshSet meshes{{"cube", cube_mesh()}};
    const auto draw = [&](float degrees) {
        const Effect effect = cube(degrees);
        const gpu::KernelSet kernels = gpu::compile_kernels(effect, *g_compiler).value();
        gpu::GpuSimulation simulation = gpu::GpuSimulation::create(*g_device, effect, kernels).value();
        gpu::ParticleRenderer renderer = gpu::ParticleRenderer::create(*g_device, effect, kernels, TextureSet{},
                                                                       gpu::TextureFormat::Rgba8Unorm, meshes)
                                             .value();
        gpu::ParticleSorter sorter = gpu::ParticleSorter::create(*g_device, effect, kernels).value();
        const gpu::Texture target =
            g_device->create_render_target(size, size, gpu::TextureFormat::Rgba8Unorm, "test target").value();
        simulation.advance_to(1U);
        const std::array<float, 4> clear{0.0F, 0.0F, 0.0F, 0.0F};
        renderer.render(simulation, sorter, ortho, target, &clear);
        Frame frame{g_device->read_texture(target).value()};
        g_device->destroy_texture(target);
        return frame;
    };
    // Facing the camera: a solid square a unit (16 pixels) across, its front
    // face lit full on, where a sprite would be a soft disc.
    const Frame square = draw(0.0F);
    AJFX_CHECK(square.at(32U, 32U).r >= 254 && square.at(32U, 32U).a >= 254);
    AJFX_CHECK(square.at(39U, 32U).r >= 254 && square.at(39U, 39U).r >= 254);
    AJFX_CHECK(square.at(41U, 32U).a == 0);
    // Turned 45 degrees about Y: two faces, each lit 0.35 + 0.65 cos 45,
    // about 0.81, half a diagonal (11 pixels) either side.
    const Frame turned = draw(45.0F);
    AJFX_CHECK(turned.at(36U, 32U).r >= 203 && turned.at(36U, 32U).r <= 210);
    AJFX_CHECK(turned.at(42U, 32U).a >= 254 && turned.at(45U, 32U).a == 0);
    // A mesh the effect draws but was not given is refused.
    const gpu::KernelSet kernels = gpu::compile_kernels(cube(0.0F), *g_compiler).value();
    const Result<gpu::ParticleRenderer> missing = gpu::ParticleRenderer::create(
        *g_device, cube(0.0F), kernels, TextureSet{}, gpu::TextureFormat::Rgba8Unorm);
    AJFX_CHECK(!missing.ok() && missing.error().code == ErrorCode::MissingMesh);
}

void mesh_particles_draw_back_to_front() {
    // Two cubes 0.4 across, the second born half a second after the first,
    // both moving toward the camera: the older is nearer, yet drawn first by
    // birth order. Colour by age tells them apart: the older red, the younger
    // green. Without a depth buffer, only a sort keeps the near one on top.
    EffectBuilder builder{"pair"};
    builder.emitter("e")
        .spawn(SpawnBurst{.time = 0.0F, .count = 1U})
        .spawn(SpawnBurst{.time = 0.5F, .count = 1U})
        .init(Lifetime{.min = 10.0F, .max = 10.0F})
        .init(ShapeLocation{.shape = Shape::Point})
        .init(Velocity{.direction = Vec3{0.0F, 0.0F, 1.0F}, .speed_min = 1.0F, .speed_max = 1.0F})
        .init(InitSize{.min = 0.4F, .max = 0.4F})
        .update(ColorOverLife{.keys = {{0.0F, Color{0.0F, 1.0F, 0.0F, 1.0F}},
                                       {0.03F, Color{0.0F, 1.0F, 0.0F, 1.0F}},
                                       {0.05F, Color{1.0F, 0.0F, 0.0F, 1.0F}},
                                       {1.0F, Color{1.0F, 0.0F, 0.0F, 1.0F}}}})
        .renderer(SpriteRenderer{.geometry = Geometry::Mesh, .mesh = "cube"});
    const Effect sorted = builder.build().value();
    const MeshSet meshes{{"cube", cube_mesh()}};
    const auto draw = [&](const Effect& effect) {
        const gpu::KernelSet kernels = gpu::compile_kernels(effect, *g_compiler).value();
        gpu::GpuSimulation simulation = gpu::GpuSimulation::create(*g_device, effect, kernels).value();
        gpu::ParticleRenderer renderer = gpu::ParticleRenderer::create(*g_device, effect, kernels, TextureSet{},
                                                                       gpu::TextureFormat::Rgba8Unorm, meshes)
                                             .value();
        gpu::ParticleSorter sorter = gpu::ParticleSorter::create(*g_device, effect, kernels).value();
        const gpu::Texture target =
            g_device->create_render_target(size, size, gpu::TextureFormat::Rgba8Unorm, "test target").value();
        // 0.6 s: the first at z = 0.6, the second at z = 0.1, one behind the
        // other.
        simulation.advance_to(36U);
        const EmitterState state = simulation.read_emitter(0U).value();
        AJFX_CHECK(state.alive() == 2U && state.position[0].z > state.position[1].z + 0.4F);
        AJFX_CHECK(state.color[0].r == 1.0F && state.color[1].g == 1.0F);
        const std::array<float, 4> clear{0.0F, 0.0F, 0.0F, 0.0F};
        renderer.render(simulation, sorter, ortho, target, &clear);
        const Pixel centre = Frame{g_device->read_texture(target).value()}.at(32U, 32U);
        g_device->destroy_texture(target);
        return centre;
    };
    AJFX_CHECK(gpu::sorts(sorted.emitters[0]));
    const Pixel near = draw(sorted);
    AJFX_CHECK(near.r >= 250 && near.g == 0);
    // Opting out of the sort draws them in birth order: the far one on top.
    Effect unsorted = sorted;
    unsorted.emitters[0].renderer.sort = false;
    const Pixel far = draw(unsorted);
    AJFX_CHECK(far.g >= 250 && far.r == 0);
}

void tumbling_meshes_turn_by_their_orientation() {
    // A red unit cube tumbling at 200 degrees a second about a random axis.
    EffectBuilder builder{"tumbling"};
    builder.emitter("e")
        .spawn(SpawnBurst{.count = 1U})
        .init(Lifetime{.min = 10.0F, .max = 10.0F})
        .init(ShapeLocation{.shape = Shape::Point})
        .init(InitColor{.a = Color{1.0F, 0.0F, 0.0F, 1.0F}, .b = Color{1.0F, 0.0F, 0.0F, 1.0F}})
        .init(InitSize{.min = 1.0F, .max = 1.0F})
        .init(InitTumble{.speed_min = 200.0F, .speed_max = 200.0F})
        .update(RigidBody{.angular_drag = 0.0F})
        .renderer(SpriteRenderer{.geometry = Geometry::Mesh, .mesh = "cube"});
    const Effect effect = builder.build().value();
    const MeshSet meshes{{"cube", cube_mesh()}};
    const gpu::KernelSet kernels = gpu::compile_kernels(effect, *g_compiler).value();
    gpu::GpuSimulation simulation = gpu::GpuSimulation::create(*g_device, effect, kernels).value();
    gpu::ParticleRenderer renderer =
        gpu::ParticleRenderer::create(*g_device, effect, kernels, TextureSet{}, gpu::TextureFormat::Rgba8Unorm, meshes)
            .value();
    gpu::ParticleSorter sorter = gpu::ParticleSorter::create(*g_device, effect, kernels).value();
    const gpu::Texture target =
        g_device->create_render_target(size, size, gpu::TextureFormat::Rgba8Unorm, "test target").value();
    std::vector<std::size_t> areas;
    for (const std::uint64_t tick : {1U, 20U, 40U}) {
        simulation.advance_to(tick);
        const std::array<float, 4> clear{0.0F, 0.0F, 0.0F, 0.0F};
        renderer.render(simulation, sorter, ortho, target, &clear);
        const Frame frame{g_device->read_texture(target).value()};
        // Seen along Z, a unit cube covers |x.z| + |y.z| + |z.z| square
        // units, where x, y and z are its turned axes: 256 pixels each.
        const Quat q = simulation.read_emitter(0U).value().orientation[0];
        const float xz = 2.0F * (q.x * q.z - q.w * q.y);
        const float yz = 2.0F * (q.y * q.z + q.w * q.x);
        const float zz = 1.0F - 2.0F * (q.x * q.x + q.y * q.y);
        const float expected = 256.0F * (std::fabs(xz) + std::fabs(yz) + std::fabs(zz));
        std::cout << "  tick " << tick << ": " << frame.lit() << " pixels, expected " << expected << '\n';
        AJFX_CHECK(std::fabs(static_cast<float>(frame.lit()) - expected) < 0.05F * expected);
        AJFX_CHECK(frame.at(32U, 32U).r >= 80);
        areas.push_back(frame.lit());
    }
    // It is turning.
    AJFX_CHECK(areas[0] != areas[1] && areas[1] != areas[2]);
    g_device->destroy_texture(target);
}

// One still white particle a unit across, lit as `renderer` says.
Effect lit_still(SpriteRenderer renderer) {
    EffectBuilder builder{"lit"};
    builder.emitter("e")
        .spawn(SpawnBurst{.count = 1U})
        .init(Lifetime{.min = 10.0F, .max = 10.0F})
        .init(ShapeLocation{.shape = Shape::Point})
        .init(InitSize{.min = 1.0F, .max = 1.0F})
        .renderer(renderer);
    return builder.build().value();
}

// No hemisphere: only the directional light, travelling `direction`.
gpu::SceneLight sun(Vec3 direction) {
    return gpu::SceneLight{.direction = direction, .color = Vec3{1.0F, 1.0F, 1.0F}, .sky = Vec3{}, .ground = Vec3{}};
}

void lit_sprites_take_the_light_from_its_side() {
    // A full sphere's normal, no wrap: the sprite turns from the camera at
    // its centre to the edge's direction at its rim.
    const Effect effect = lit_still(SpriteRenderer{.blend = BlendMode::Alpha, .lighting = Lighting::Lit,
                                                   .curvature = 1.0F, .wrap = 0.0F, .ambient = 0.0F});
    // Light from the camera (travelling away from it, down -Z): the centre
    // faces it fully, so it is as bright as an unlit particle's.
    const Frame from_camera = render(effect, 1U, ortho, {}, sun(Vec3{0.0F, 0.0F, -1.0F}));
    const Pixel centre = from_camera.at(32U, 32U);
    AJFX_CHECK(centre.r >= 251 && centre.g == centre.r && centre.b == centre.r && centre.a >= 251);
    // Light from the right (travelling -X). Six pixels right of the centre,
    // uv.x = 13/16: the normal leans 13/16 right, and the coverage there is
    // 0.336, so the premultiplied red is 0.8125 * 0.336 = 0.273. Seven pixels
    // left (the same 13/16, the other way) the normal leans away: nothing, under coverage alone.
    const Frame from_right = render(effect, 1U, ortho, {}, sun(Vec3{-1.0F, 0.0F, 0.0F}));
    const Pixel right = from_right.at(38U, 32U);
    const Pixel left = from_right.at(25U, 32U);
    AJFX_CHECK(right.r >= 66 && right.r <= 73 && right.a >= 83 && right.a <= 88);
    AJFX_CHECK(left.r <= 2 && left.a >= 83 && left.a <= 88);
    // The centre pixel is 1/32 unit right of the particle, so its normal
    // leans 1/16 right, square to the light but for that: 1/16 of it.
    AJFX_CHECK(from_right.at(32U, 32U).r >= 13 && from_right.at(32U, 32U).r <= 19);
    // Wrapped all the way, the light reaches round the terminator at half
    // strength: the far side is lit now, still less than the near side.
    Effect wrapped = effect;
    wrapped.emitters[0].renderer.wrap = 1.0F;
    const Frame wrap = render(wrapped, 1U, ortho, {}, sun(Vec3{-1.0F, 0.0F, 0.0F}));
    AJFX_CHECK(wrap.at(25U, 32U).r >= 20 && wrap.at(25U, 32U).r < wrap.at(38U, 32U).r);
    // The hemisphere alone, grey sky and black ground, taken at `ambient`:
    // the front faces sideways to it and takes half the sky (0.25), and the
    // centre pixel's slight lean right (a quarter of the sky) and up (all
    // of it) adds a little: 0.265 of white, under 0.992 coverage.
    Effect ambient = effect;
    ambient.emitters[0].renderer.ambient = 1.0F;
    const gpu::SceneLight hemisphere{.color = Vec3{}, .sky = Vec3{0.5F, 0.5F, 0.5F}, .ground = Vec3{}};
    const Pixel under_sky = render(ambient, 1U, ortho, {}, hemisphere).at(32U, 32U);
    AJFX_CHECK(under_sky.r >= 64 && under_sky.r <= 70 && under_sky.a >= 251);
    // Unlit particles ignore the light altogether.
    const Effect unlit = lit_still(SpriteRenderer{.blend = BlendMode::Alpha});
    AJFX_CHECK(render(unlit, 1U, ortho, {}, sun(Vec3{-1.0F, 0.0F, 0.0F})).at(32U, 32U).r >= 251);
}

// Six-way maps, one cell each: `right` takes light from the right only (and
// `behind` from behind only), opaque everywhere.
TextureSet six_way_maps() {
    std::vector<std::uint8_t> right;
    std::vector<std::uint8_t> behind;
    for (std::uint32_t i = 0U; i < 16U; ++i) {
        right.insert(right.end(), {255, 0, 0, 255});
        behind.insert(behind.end(), {0, 0, 255, 255});
    }
    return TextureSet{{"right", make_texture(4U, 4U, right, ColorSpace::Linear).value()},
                      {"behind", make_texture(4U, 4U, behind, ColorSpace::Linear).value()}};
}

void six_way_maps_weigh_the_light_by_their_texels() {
    const Effect effect = lit_still(SpriteRenderer{.blend = BlendMode::Alpha, .texture = "right",
                                                   .lighting = Lighting::SixWay, .lightmap = "behind",
                                                   .wrap = 0.0F, .ambient = 0.0F});
    // Light from the right: the first map's red channel takes all of it,
    // and the maps are light, not colour, so the white particle stays white.
    const Pixel lit = render(effect, 1U, ortho, six_way_maps(), sun(Vec3{-1.0F, 0.0F, 0.0F})).at(32U, 32U);
    AJFX_CHECK(lit.r >= 251 && lit.g >= 251 && lit.b >= 251 && lit.a >= 251);
    // From the left: neither map takes it; covered but dark.
    const Pixel dark = render(effect, 1U, ortho, six_way_maps(), sun(Vec3{1.0F, 0.0F, 0.0F})).at(32U, 32U);
    AJFX_CHECK(dark.r <= 2 && dark.a >= 251);
    // From behind the sprite (travelling towards the camera): the second
    // map's blue channel.
    const Pixel back = render(effect, 1U, ortho, six_way_maps(), sun(Vec3{0.0F, 0.0F, 1.0F})).at(32U, 32U);
    AJFX_CHECK(back.r >= 251 && back.a >= 251);
    // A six-way emitter whose second map is missing is refused by name.
    TextureSet half = six_way_maps();
    half.erase("behind");
    const gpu::KernelSet kernels = gpu::compile_kernels(effect, *g_compiler).value();
    const Result<gpu::ParticleRenderer> missing =
        gpu::ParticleRenderer::create(*g_device, effect, kernels, half, gpu::TextureFormat::Rgba8Unorm);
    AJFX_CHECK(!missing.ok() && missing.error().code == ErrorCode::MissingTexture &&
               missing.error().message.find("'behind'") != std::string::npos);
}

// A grid two units each way about the origin (8 cells of a quarter unit),
// drawn as smoke of `extinction` and `albedo`, and a still red sprite half
// a unit across in front of it when `with_sprite`.
Effect slab(float extinction, float albedo, bool with_sprite) {
    EffectBuilder builder{"slab"};
    builder.volume(Volume{.name = "smoke",
                          .origin = Vec3{-1.0F, -1.0F, -1.0F},
                          .cell_size = 0.25F,
                          .nx = 8U,
                          .ny = 8U,
                          .nz = 8U,
                          .extinction = extinction,
                          .albedo = albedo,
                          .color = Vec3{1.0F, 0.5F, 0.25F}});
    if (with_sprite) {
        builder.emitter("dot")
            .spawn(SpawnBurst{.count = 1U})
            .init(Lifetime{.min = 10.0F, .max = 10.0F})
            .init(ShapeLocation{.shape = Shape::Point, .center = Vec3{0.0F, 0.0F, 1.5F}})
            .init(InitColor{.a = Color{1.0F, 0.0F, 0.0F, 1.0F}, .b = Color{1.0F, 0.0F, 0.0F, 1.0F}})
            .init(InitSize{.min = 0.5F, .max = 0.5F})
            .renderer(SpriteRenderer{.blend = BlendMode::Alpha});
    }
    return builder.build().value();
}

// Draws `effect` with its grid filled with a uniform `density`, under
// `light`, without stepping.
Frame render_slab(const Effect& effect, float density, const gpu::SceneLight& light) {
    const gpu::KernelSet kernels = gpu::compile_kernels(effect, *g_compiler).value();
    gpu::GpuSimulation simulation = gpu::GpuSimulation::create(*g_device, effect, kernels).value();
    gpu::ParticleRenderer renderer =
        gpu::ParticleRenderer::create(*g_device, effect, kernels, TextureSet{}, gpu::TextureFormat::Rgba8Unorm).value();
    gpu::ParticleSorter sorter = gpu::ParticleSorter::create(*g_device, effect, kernels).value();
    VolumeState cells;
    cells.nx = cells.ny = cells.nz = 8U;
    cells.density.assign(512U, density);
    cells.temperature.assign(512U, 0.0F);
    cells.velocity.assign(512U, Vec3{});
    simulation.write_volume(0U, cells);
    if (!effect.emitters.empty()) {
        simulation.advance_to(1U);
    }
    const gpu::Texture target =
        g_device->create_render_target(size, size, gpu::TextureFormat::Rgba8Unorm, "test target").value();
    const std::array<float, 4> clear{0.0F, 0.0F, 0.0F, 0.0F};
    renderer.render(simulation, sorter, ortho, target, &clear, light);
    Frame frame{g_device->read_texture(target).value()};
    g_device->destroy_texture(target);
    return frame;
}

void volumes_raymarch_a_slab_of_smoke() {
    // A uniform slab two units deep of density 0.6 at extinction 1.5: the
    // light through it is exp(-1.8) = 0.165, so it covers 0.835 of the pixel
    // whatever the step count, as every step through uniform smoke takes the
    // same share. The light travels with the view (down -Z) from the near
    // face; the single scattering towards the camera is the march's own sum,
    // in 64 steps of ds along the ray, each lit through 8 steps of its own
    // back to the face, which for uniform smoke is exactly exp(-1.5 * 0.6 *
    // t) at depth t.
    const float density = 0.6F;
    const float extinction = 1.5F;
    const float albedo = 0.8F;
    const double sigma = static_cast<double>(density) * extinction;
    const double ds = 2.0 / 64.0;
    const double through = std::exp(-sigma * ds);
    double transmittance = 1.0;
    double scattered = 0.0;
    for (int k = 0; k < 64; ++k) {
        const double t = (k + 0.5) * ds;
        scattered += transmittance * (1.0 - through) * albedo * std::exp(-sigma * t);
        transmittance *= through;
    }
    const auto expect = [](int value, double wanted, int slack) {
        const int rounded = static_cast<int>(std::floor(wanted * 255.0 + 0.5));
        return value >= rounded - slack && value <= rounded + slack;
    };
    const Frame lit = render_slab(slab(extinction, albedo, false), density, sun(Vec3{0.0F, 0.0F, -1.0F}));
    const Pixel centre = lit.at(32U, 32U);
    AJFX_CHECK(expect(centre.a, 1.0 - std::exp(-1.8), 2));
    AJFX_CHECK(expect(centre.a, 1.0 - transmittance, 2));
    // The colour tints what is scattered: red takes all, green half, blue a
    // quarter of it.
    AJFX_CHECK(expect(centre.r, scattered, 3));
    AJFX_CHECK(expect(centre.g, scattered * 0.5, 3));
    AJFX_CHECK(expect(centre.b, scattered * 0.25, 3));
    // The closed form of that sum, with the midpoint rule's small error.
    AJFX_CHECK(std::fabs(scattered - albedo * 0.5 * (1.0 - std::exp(-2.0 * sigma * 2.0))) < 0.01);
    // Every pixel in the slab's square reads the same; outside it nothing.
    AJFX_CHECK(lit.at(20U, 44U).a == centre.a && lit.at(44U, 20U).r == centre.r);
    AJFX_CHECK(lit.at(14U, 32U).a == 0 && lit.at(32U, 49U).a == 0 && lit.at(0U, 0U).a == 0);
    // Lit from behind (the light travelling towards the camera) the far
    // samples are brightest and most hidden: darker than lit from the front.
    const Pixel from_behind = render_slab(slab(extinction, albedo, false), density, sun(Vec3{0.0F, 0.0F, 1.0F})).at(32U, 32U);
    AJFX_CHECK(from_behind.a == centre.a && from_behind.r < centre.r - 20);
    // Absorption alone: the same cover and no light scattered.
    const Pixel dark = render_slab(slab(extinction, 0.0F, false), density, sun(Vec3{0.0F, 0.0F, -1.0F})).at(32U, 32U);
    AJFX_CHECK(expect(dark.a, 1.0 - std::exp(-1.8), 2) && dark.r == 0 && dark.g == 0);
    // Twice the density, twice the depth of smoke.
    const Pixel thick = render_slab(slab(extinction, 0.0F, false), 2.0F * density, sun(Vec3{0.0F, 0.0F, -1.0F})).at(32U, 32U);
    AJFX_CHECK(expect(thick.a, 1.0 - std::exp(-3.6), 2));
    // An empty grid draws nothing at all.
    AJFX_CHECK(render_slab(slab(extinction, albedo, false), 0.0F, sun(Vec3{0.0F, 0.0F, -1.0F})).lit() == 0U);
    // The particles draw over the smoke: the red disc in front covers it at
    // its centre, and the smoke shows beside it.
    const Frame over = render_slab(slab(extinction, albedo, true), density, sun(Vec3{0.0F, 0.0F, -1.0F}));
    AJFX_CHECK(over.at(32U, 32U).r >= 250 && over.at(32U, 32U).g <= 2 && over.at(32U, 32U).a >= 254);
    AJFX_CHECK(over.at(44U, 20U).r == centre.r && over.at(44U, 20U).a == centre.a);
    // Frame after frame, the same frame.
    AJFX_CHECK(render_slab(slab(extinction, albedo, true), density, sun(Vec3{0.0F, 0.0F, -1.0F})).bytes() ==
               over.bytes());
}

void frames_are_reproducible() {
    const gpu::Camera camera =
        gpu::Camera::look_at(Vec3{0.0F, 1.5F, 6.0F}, Vec3{0.0F, 1.5F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F}, 1.0F, 1.0F,
                             0.1F, 100.0F);
    // Smoke's large puffs cover much of the frame; confetti's specks are
    // smaller than a pixel here, so only some of them land on pixel centres.
    for (const Effect& effect : {presets::smoke(), presets::confetti()}) {
        const Frame a = render(effect, 120U, camera, presets::textures());
        const Frame b = render(effect, 120U, camera, presets::textures());
        AJFX_CHECK(a.lit() > (effect.name == "smoke" ? 200U : 0U));
        AJFX_CHECK(a.bytes() == b.bytes());
    }
}

} // namespace

int main() {
    const bool debug_layer = std::getenv("AJFX_TEST_DEBUG_LAYER") != nullptr;
    Result<std::unique_ptr<d3d12::Device>> device = d3d12::create_device({.debug_layer = debug_layer});
    if (!device) {
        device = d3d12::create_device({.warp = true, .debug_layer = debug_layer});
    }
    Result<std::unique_ptr<gpu::ShaderCompiler>> compiler = d3d12::create_shader_compiler();
    if (!device || !compiler) {
        std::cout << "skipped: " << (!device ? device.error().message : compiler.error().message) << '\n';
        return 77;
    }
    g_device = std::move(device).value();
    g_compiler = std::move(compiler).value();
    std::cout << "adapter: " << g_device->adapter_name() << '\n';
    const ajfx::test::Case cases[] = {
        {"a_sprite_is_a_soft_disc_where_its_particle_is", a_sprite_is_a_soft_disc_where_its_particle_is},
        {"blend_modes_combine_overlapping_particles", blend_modes_combine_overlapping_particles},
        {"draws_follow_the_live_count", draws_follow_the_live_count},
        {"perspective_puts_particles_where_the_projection_says", perspective_puts_particles_where_the_projection_says},
        {"velocity_aligned_sprites_stretch_along_their_motion", velocity_aligned_sprites_stretch_along_their_motion},
        {"alpha_particles_draw_back_to_front", alpha_particles_draw_back_to_front},
        {"emitters_interleave_by_depth", emitters_interleave_by_depth},
        {"flipbooks_play_their_cells_over_each_life", flipbooks_play_their_cells_over_each_life},
        {"ribbons_join_the_particles_in_birth_order", ribbons_join_the_particles_in_birth_order},
        {"mesh_particles_draw_their_mesh", mesh_particles_draw_their_mesh},
        {"tumbling_meshes_turn_by_their_orientation", tumbling_meshes_turn_by_their_orientation},
        {"mesh_particles_draw_back_to_front", mesh_particles_draw_back_to_front},
        {"lit_sprites_take_the_light_from_its_side", lit_sprites_take_the_light_from_its_side},
        {"six_way_maps_weigh_the_light_by_their_texels", six_way_maps_weigh_the_light_by_their_texels},
        {"volumes_raymarch_a_slab_of_smoke", volumes_raymarch_a_slab_of_smoke},
        {"frames_are_reproducible", frames_are_reproducible},
    };
    const int result = ajfx::test::run(cases);
    g_compiler.reset();
    g_device.reset();
    return result;
}
