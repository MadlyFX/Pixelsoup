// getenv is fine for reading one test switch.
#define _CRT_SECURE_NO_WARNINGS

#include "check.hpp"

#include <ajfx/builder.hpp>
#include <ajfx/d3d12.hpp>
#include <ajfx/gpu/simulation.hpp>
#include <ajfx/presets.hpp>
#include <ajfx/simulation.hpp>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>

namespace {

using namespace ajfx;

std::unique_ptr<gpu::Device> g_device;
std::unique_ptr<gpu::ShaderCompiler> g_compiler;

gpu::GpuSimulation make_gpu(const Effect& effect, std::uint32_t seed = 0U) {
    Result<gpu::KernelSet> kernels = gpu::compile_kernels(effect, *g_compiler);
    if (!kernels) {
        std::cout << kernels.error().message << '\n';
    }
    AJFX_CHECK(kernels.ok());
    Result<gpu::GpuSimulation> simulation = gpu::GpuSimulation::create(*g_device, effect, kernels.value(), seed);
    if (!simulation) {
        std::cout << simulation.error().message << '\n';
    }
    AJFX_CHECK(simulation.ok());
    return std::move(simulation).value();
}

Simulation make_cpu(const Effect& effect, std::uint32_t seed = 0U) {
    Result<Simulation> simulation = Simulation::create(effect, seed);
    AJFX_CHECK(simulation.ok());
    return std::move(simulation).value();
}

EmitterState read(gpu::GpuSimulation& simulation, std::size_t emitter = 0U) {
    Result<EmitterState> state = simulation.read_emitter(emitter);
    if (!state) {
        std::cout << state.error().message << '\n';
    }
    AJFX_CHECK(state.ok());
    return std::move(state).value();
}

std::vector<std::uint8_t> bytes(gpu::GpuSimulation& simulation, std::size_t emitter = 0U) {
    Result<std::vector<std::uint8_t>> read = simulation.read_particle_bytes(emitter);
    AJFX_CHECK(read.ok());
    return std::move(read).value();
}

bool close(float a, float b, float tolerance) {
    return std::fabs(a - b) <= tolerance * (1.0F + std::fabs(a));
}

bool close(Vec3 a, Vec3 b, float tolerance) {
    return close(a.x, b.x, tolerance) && close(a.y, b.y, tolerance) && close(a.z, b.z, tolerance);
}

bool close(Color a, Color b, float tolerance) {
    return close(a.r, b.r, tolerance) && close(a.g, b.g, tolerance) && close(a.b, b.b, tolerance) &&
           close(a.a, b.a, tolerance);
}

// Births, deaths, ages and lifetimes are integer or add-and-multiply results
// and must match exactly; anything through sin, cos, sqrt or a division may
// differ in the last bits and is compared with a tolerance. Returns the
// fraction of particles whose every attribute is within it.
float compare(const EmitterState& cpu, const EmitterState& gpu, float tolerance) {
    AJFX_CHECK(cpu.alive() == gpu.alive());
    AJFX_CHECK(cpu.spawned == gpu.spawned);
    AJFX_CHECK(gpu.refused == 0U);
    AJFX_CHECK(cpu.spawn_index == gpu.spawn_index);
    AJFX_CHECK(cpu.age == gpu.age);
    AJFX_CHECK(cpu.lifetime == gpu.lifetime);
    std::size_t within = 0U;
    for (std::size_t i = 0U; i < cpu.alive(); ++i) {
        within += close(cpu.position[i], gpu.position[i], tolerance) &&
                          close(cpu.velocity[i], gpu.velocity[i], tolerance) &&
                          close(cpu.color[i], gpu.color[i], tolerance) && close(cpu.size[i], gpu.size[i], tolerance) &&
                          close(cpu.rotation[i], gpu.rotation[i], tolerance)
                      ? 1U
                      : 0U;
    }
    return cpu.alive() == 0U ? 1.0F : static_cast<float>(within) / static_cast<float>(cpu.alive());
}

// Every module but curl noise, whose turbulence amplifies last-bit
// differences: here every particle must match the reference.
Effect smooth_effect() {
    EffectBuilder builder{"smooth"};
    builder.float_parameter("rate", 90.0F, 0.0F, 200.0F);
    builder.emitter("box")
        .capacity(2000U)
        .seed(4U)
        .spawn(SpawnRate{.rate = bind("rate")})
        .spawn(SpawnBurst{.time = 0.5F, .count = 64U, .interval = 0.5F, .cycles = 0U})
        .init(Lifetime{.min = 0.5F, .max = 1.5F})
        .init(ShapeLocation{.shape = Shape::Box, .center = Vec3{1.0F, 2.0F, 3.0F}, .size = Vec3{2.0F, 1.0F, 0.5F},
                            .surface_only = true})
        .init(Velocity{.direction = Vec3{0.3F, 1.0F, 0.2F}, .speed_min = 1.0F, .speed_max = 2.0F, .spread = 40.0F})
        .init(InitColor{.a = Color{1.0F, 0.5F, 0.25F, 1.0F}, .b = Color{0.0F, 0.5F, 1.0F, 0.5F}})
        .init(InitSize{.min = 0.1F, .max = 0.3F})
        .init(InitRotation{})
        .update(Gravity{})
        .update(Drag{.coefficient = 0.5F})
        .update(Vortex{.center = Vec3{1.0F, 0.0F, 3.0F}, .strength = 2.0F})
        .update(Attractor{.position = Vec3{1.0F, 4.0F, 3.0F}, .strength = 3.0F, .radius = 5.0F})
        .update(ColorOverLife{})
        .update(SizeOverLife{.keys = {{0.0F, 0.5F}, {0.3F, 1.5F}, {1.0F, 0.0F}}});
    builder.emitter("sphere")
        .capacity(1000U)
        .seed(5U)
        .spawn(SpawnRate{.rate = 60.0F})
        .init(Lifetime{.min = 2.0F, .max = 2.0F})
        .init(ShapeLocation{.shape = Shape::Sphere, .radius = 0.5F})
        .init(Velocity{.spread = 180.0F});
    builder.emitter("disc")
        .capacity(1000U)
        .seed(6U)
        .spawn(SpawnRate{.rate = 30.0F})
        .init(Lifetime{.min = 1.0F, .max = 3.0F})
        .init(ShapeLocation{.shape = Shape::Disc, .radius = 2.0F});
    Result<Effect> effect = builder.build();
    AJFX_CHECK(effect.ok());
    return std::move(effect).value();
}

class Ramp final : public ParameterProvider {
public:
    void values_at(std::uint64_t tick, std::span<ParameterValue> values) const override {
        values[0][0] = static_cast<float>(tick % 90U) * 2.0F;
    }
};

void gpu_matches_the_cpu_reference() {
    const Effect effect = smooth_effect();
    const Ramp ramp;
    Simulation cpu = make_cpu(effect, 3U);
    gpu::GpuSimulation gpu = make_gpu(effect, 3U);
    for (const std::uint64_t tick : {1U, 31U, 90U, 150U}) {
        cpu.advance_to(tick, &ramp);
        gpu.advance_to(tick, &ramp);
        for (std::size_t e = 0U; e < effect.emitters.size(); ++e) {
            // At step 1 the ramp has the rate at zero and the disc's first
            // birth is a step away; after that every emitter has particles.
            AJFX_CHECK(tick == 1U || cpu.emitter(e).alive() > 0U);
            AJFX_CHECK(compare(cpu.emitter(e), read(gpu, e), 1.0e-3F) == 1.0F);
        }
    }
}

void presets_match_the_reference_within_tolerance() {
    for (const Effect& effect : {presets::sparks(), presets::smoke(), presets::confetti()}) {
        Simulation cpu = make_cpu(effect, 8U);
        gpu::GpuSimulation gpu = make_gpu(effect, 8U);
        cpu.advance_to(120U);
        gpu.advance_to(120U);
        const float within = compare(cpu.emitter(0), read(gpu), 1.0e-2F);
        std::cout << "  " << effect.name << ": " << cpu.emitter(0).alive() << " particles, " << within * 100.0F
                  << "% within tolerance\n";
        AJFX_CHECK(within >= 0.98F);
    }
}

void reruns_are_bit_identical() {
    for (const Effect& effect : {presets::smoke(), presets::sparks()}) {
        gpu::GpuSimulation a = make_gpu(effect, 1U);
        gpu::GpuSimulation b = make_gpu(effect, 1U);
        a.advance_to(200U);
        b.advance_to(200U);
        const std::vector<std::uint8_t> first = bytes(a);
        AJFX_CHECK(!first.empty());
        AJFX_CHECK(first == bytes(b));
    }
}

void checkpoints_and_reset_reproduce_the_timeline() {
    const Effect effect = presets::smoke();
    gpu::GpuSimulation simulation = make_gpu(effect, 2U);
    simulation.advance_to(100U);
    Result<gpu::GpuCheckpoint> checkpoint = simulation.save();
    AJFX_CHECK(checkpoint.ok());
    AJFX_CHECK(checkpoint.value().tick() == 100U);
    simulation.advance_to(180U);
    const std::vector<std::uint8_t> straight = bytes(simulation);

    simulation.restore(checkpoint.value());
    AJFX_CHECK(simulation.tick() == 100U);
    simulation.advance_to(180U);
    AJFX_CHECK(bytes(simulation) == straight);

    simulation.reset();
    AJFX_CHECK(simulation.tick() == 0U);
    AJFX_CHECK(read(simulation).alive() == 0U);
    simulation.advance_to(180U);
    AJFX_CHECK(bytes(simulation) == straight);
}

void compaction_spans_many_blocks() {
    // Tens of thousands of particles with scattered lifetimes: deaths land in
    // every 4096-particle block, and survivors must keep birth order.
    EffectBuilder builder{"crowd"};
    builder.emitter("crowd")
        .capacity(70000U)
        .spawn(SpawnRate{.rate = 20000.0F})
        .init(Lifetime{.min = 0.2F, .max = 3.0F})
        .init(Velocity{.spread = 180.0F})
        .update(Gravity{});
    const Effect effect = builder.build().value();
    Simulation cpu = make_cpu(effect);
    gpu::GpuSimulation gpu = make_gpu(effect);
    cpu.advance_to(200U);
    gpu.advance_to(200U);
    AJFX_CHECK(cpu.emitter(0).alive() > 4U * 4096U);
    AJFX_CHECK(compare(cpu.emitter(0), read(gpu), 1.0e-3F) == 1.0F);
}

void buffers_expose_the_live_particles() {
    gpu::GpuSimulation simulation = make_gpu(presets::sparks());
    simulation.advance_to(30U);
    const gpu::EmitterBuffers buffers = simulation.emitter_buffers(0U);
    AJFX_CHECK(buffers.particles.valid() && buffers.counters.valid());
    AJFX_CHECK(buffers.layout.stride == 72U && buffers.capacity == 4096U);
    Result<std::vector<std::uint8_t>> counters = g_device->read_buffer(buffers.counters, 0U, 4U);
    AJFX_CHECK(counters.ok());
    std::uint32_t alive = 0U;
    std::memcpy(&alive, counters.value().data(), 4U);
    AJFX_CHECK(alive == read(simulation).alive());
}

} // namespace

int main() {
    // Hardware first, then WARP. AJFX_TEST_WARP=1 forces WARP;
    // AJFX_TEST_DEBUG_LAYER=1 fails any flush the debug layer objects to.
    const char* force_warp = std::getenv("AJFX_TEST_WARP");
    const bool debug_layer = std::getenv("AJFX_TEST_DEBUG_LAYER") != nullptr;
    Result<std::unique_ptr<gpu::Device>> device =
        d3d12::create_device({.warp = force_warp != nullptr, .debug_layer = debug_layer});
    if (!device && force_warp == nullptr) {
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
        {"gpu_matches_the_cpu_reference", gpu_matches_the_cpu_reference},
        {"presets_match_the_reference_within_tolerance", presets_match_the_reference_within_tolerance},
        {"reruns_are_bit_identical", reruns_are_bit_identical},
        {"checkpoints_and_reset_reproduce_the_timeline", checkpoints_and_reset_reproduce_the_timeline},
        {"compaction_spans_many_blocks", compaction_spans_many_blocks},
        {"buffers_expose_the_live_particles", buffers_expose_the_live_particles},
    };
    const int result = ajfx::test::run(cases);
    g_compiler.reset();
    g_device.reset();
    return result;
}
