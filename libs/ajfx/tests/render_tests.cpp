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
// transparent black, and reads the pixels back.
Frame render(const Effect& effect, std::uint64_t steps, const gpu::Camera& camera) {
    const gpu::KernelSet kernels = gpu::compile_kernels(effect, *g_compiler).value();
    gpu::GpuSimulation simulation = gpu::GpuSimulation::create(*g_device, effect, kernels).value();
    gpu::ParticleRenderer renderer =
        gpu::ParticleRenderer::create(*g_device, kernels, gpu::TextureFormat::Rgba8Unorm).value();
    const gpu::Texture target =
        g_device->create_render_target(size, size, gpu::TextureFormat::Rgba8Unorm, "test target").value();
    gpu::ParticleSorter sorter = gpu::ParticleSorter::create(*g_device, effect, kernels).value();
    simulation.advance_to(steps);
    const std::array<float, 4> clear{0.0F, 0.0F, 0.0F, 0.0F};
    renderer.render(simulation, sorter, camera, target, &clear);
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

void frames_are_reproducible() {
    const gpu::Camera camera =
        gpu::Camera::look_at(Vec3{0.0F, 1.5F, 6.0F}, Vec3{0.0F, 1.5F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F}, 1.0F, 1.0F,
                             0.1F, 100.0F);
    // Smoke's large puffs cover much of the frame; confetti's specks are
    // smaller than a pixel here, so only some of them land on pixel centres.
    for (const Effect& effect : {presets::smoke(), presets::confetti()}) {
        const Frame a = render(effect, 120U, camera);
        const Frame b = render(effect, 120U, camera);
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
        {"frames_are_reproducible", frames_are_reproducible},
    };
    const int result = ajfx::test::run(cases);
    g_compiler.reset();
    g_device.reset();
    return result;
}
