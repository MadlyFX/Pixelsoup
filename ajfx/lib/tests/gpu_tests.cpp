// getenv is fine for reading one test switch.
#define _CRT_SECURE_NO_WARNINGS

#include "check.hpp"

#include <ajfx/builder.hpp>
#include <ajfx/d3d12.hpp>
#include <ajfx/gpu/simulation.hpp>
#include <ajfx/gpu/sort.hpp>
#include <ajfx/presets.hpp>
#include <ajfx/simulation.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
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

bool close(Quat a, Quat b, float tolerance) {
    return close(a.x, b.x, tolerance) && close(a.y, b.y, tolerance) && close(a.z, b.z, tolerance) &&
           close(a.w, b.w, tolerance);
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
                          close(cpu.rotation[i], gpu.rotation[i], tolerance) &&
                          close(cpu.orientation[i], gpu.orientation[i], tolerance) &&
                          close(cpu.angular_velocity[i], gpu.angular_velocity[i], tolerance) &&
                          close(cpu.spin[i], gpu.spin[i], tolerance)
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
        .init(Velocity{.direction = Vec3{0.3F, 1.0F, 0.2F}, .speed_min = 1.0F, .speed_max = 2.0F, .spread = 40.0F,
                       .spread_min = 15.0F})
        .init(InitColor{.a = Color{1.0F, 0.5F, 0.25F, 1.0F},
                        .b = Color{0.0F, 0.5F, 1.0F, 0.5F},
                        .brightness_min = 0.5F,
                        .brightness_max = 3.0F})
        .init(InitSize{.min = 0.1F, .max = 0.3F})
        .init(InitRotation{})
        .init(InitSpin{.speed_min = 20.0F, .speed_max = 200.0F, .drag = 0.7F})
        .update(Gravity{.over_life = {{0.0F, 1.5F}, {0.4F, 0.5F}, {1.0F, 0.0F}}, .scale_min = 0.5F, .scale_max = 1.5F})
        .update(Drag{.coefficient = 0.5F, .scale_min = 0.2F, .scale_max = 3.0F})
        .update(Converge{.center = Vec3{1.0F, 0.0F, 3.0F}, .strength = 4.0F, .height = 3.0F, .scale_min = 0.0F})
        .update(Vortex{.center = Vec3{1.0F, 0.0F, 3.0F}, .strength = 2.0F, .scale_min = -1.0F, .scale_max = 2.0F})
        .update(Attractor{.position = Vec3{1.0F, 4.0F, 3.0F}, .strength = 3.0F, .radius = 5.0F, .scale_max = 2.0F})
        .update(ColorOverLife{})
        .update(SizeOverLife{.keys = {{0.0F, 0.5F}, {0.3F, 1.5F}, {1.0F, 0.0F}}});
    builder.emitter("sphere")
        .capacity(1000U)
        .seed(5U)
        .spawn(SpawnRate{.rate = 60.0F})
        .init(Lifetime{.min = 2.0F, .max = 2.0F})
        .init(ShapeLocation{.shape = Shape::Sphere, .radius_min = 0.2F, .radius_max = 0.5F})
        .init(Velocity{.spread = 180.0F})
        .update(Drag{.coefficient = 0.8F});
    // Colliders: particles bouncing inside a box, around a sphere.
    builder.emitter("caged")
        .capacity(1000U)
        .seed(7U)
        .spawn(SpawnRate{.rate = 120.0F})
        .init(Lifetime{.min = 2.0F, .max = 2.0F})
        .init(ShapeLocation{.shape = Shape::Point, .center = Vec3{0.2F, 0.3F, 0.1F}})
        .init(Velocity{.speed_min = 2.0F, .speed_max = 3.0F, .spread = 180.0F})
        .update(SphereCollision{.center = Vec3{0.9F, 0.0F, 0.0F}, .radius = 0.4F, .restitution = 0.7F})
        .update(BoxCollision{.half_size = Vec3{1.5F, 1.0F, 1.2F}, .inside = true, .restitution = 0.6F,
                             .friction = 0.2F});
    builder.emitter("disc")
        .capacity(1000U)
        .seed(6U)
        .spawn(SpawnRate{.rate = 30.0F})
        .init(Lifetime{.min = 1.0F, .max = 3.0F})
        .init(ShapeLocation{.shape = Shape::Disc, .radius_min = 1.0F, .radius_max = 2.0F, .surface_only = true});
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

void rolling_turbulence_matches_the_reference_within_tolerance() {
    // Flames: curl noise that drifts up with the plume and grows with age,
    // buoyancy that cools, air drawn in at the base, sprites that spin.
    EffectBuilder builder{"flames"};
    builder.emitter("e")
        .capacity(256U)
        .seed(9U)
        .spawn(SpawnRate{.rate = 90.0F})
        .init(Lifetime{.min = 0.6F, .max = 1.1F})
        .init(ShapeLocation{.shape = Shape::Disc, .radius_min = 0.22F, .radius_max = 0.22F})
        .init(Velocity{.speed_min = 0.2F, .speed_max = 0.5F, .spread = 15.0F})
        .init(InitRotation{})
        .init(InitSpin{.speed_min = 40.0F, .speed_max = 160.0F, .drag = 0.6F})
        .update(Gravity{.acceleration = Vec3{0.0F, 5.0F, 0.0F}, .over_life = {{0.0F, 1.6F}, {1.0F, 0.2F}}})
        .update(Converge{.strength = 8.0F, .height = 0.7F})
        .update(CurlNoise{.strength = 1.6F,
                          .frequency = 1.0F / 0.3F,
                          .evolution = 0.6F,
                          .seed = 11U,
                          .drift = Vec3{0.0F, 1.6F, 0.0F},
                          .over_life = {{0.0F, 0.15F}, {0.5F, 1.0F}, {1.0F, 1.4F}}})
        .update(Drag{.coefficient = 1.2F});
    const Effect effect = builder.build().value();
    Simulation cpu = make_cpu(effect, 8U);
    gpu::GpuSimulation gpu = make_gpu(effect, 8U);
    cpu.advance_to(150U);
    gpu.advance_to(150U);
    AJFX_CHECK(cpu.emitter(0).alive() > 40U);
    const float within = compare(cpu.emitter(0), read(gpu), 1.0e-2F);
    std::cout << "  flames: " << cpu.emitter(0).alive() << " particles, " << within * 100.0F
              << "% within tolerance\n";
    AJFX_CHECK(within >= 0.98F);
}

void presets_match_the_reference_within_tolerance() {
    for (const Effect& effect : {presets::sparks(), presets::smoke(), presets::confetti(), presets::debris()}) {
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

void distance_fields_collide_as_the_reference_does() {
    // Particles bouncing inside a ball described by a field offset from
    // the effect's origin, and off a field floor.
    const FieldSet fields{
        {"ball", make_distance_field(17U, 17U, 17U, Vec3{-1.6F, -1.6F, -1.6F}, 0.2F,
                                     [](Vec3 p) { return std::sqrt(dot(p, p)) - 1.2F; })
                     .value()},
        {"floor", make_distance_field(9U, 9U, 9U, Vec3{-4.0F, -4.0F, -4.0F}, 1.0F,
                                      [](Vec3 p) { return p.y + 0.5F; })
                      .value()}};
    EffectBuilder builder{"fields"};
    builder.emitter("e")
        .capacity(800U)
        .seed(9U)
        .spawn(SpawnRate{.rate = 200.0F})
        .init(Lifetime{.min = 2.0F, .max = 3.0F})
        .init(ShapeLocation{.shape = Shape::Point, .center = Vec3{0.3F, 0.5F, 0.0F}})
        .init(Velocity{.speed_min = 1.0F, .speed_max = 3.0F, .spread = 180.0F})
        .update(Gravity{})
        .update(SdfCollision{.field = "floor", .restitution = 0.4F, .friction = 0.3F})
        .update(SdfCollision{.field = "ball", .offset = Vec3{0.3F, 0.5F, 0.0F}, .inside = true, .restitution = 0.7F});
    const Effect effect = builder.build().value();
    Simulation cpu = Simulation::create(effect, 2U, fields).value();
    const gpu::KernelSet kernels = gpu::compile_kernels(effect, *g_compiler).value();
    gpu::GpuSimulation gpu = gpu::GpuSimulation::create(*g_device, effect, kernels, 2U, fields).value();
    for (const std::uint64_t tick : {1U, 40U, 120U, 200U}) {
        cpu.advance_to(tick);
        gpu.advance_to(tick);
        // Exactly alike until contacts pile up. Then a particle on the rim
        // where the floor meets the ball can meet one collider a step before
        // the other on one side of a last-bit difference (the random spread's
        // sin and cos are not correctly rounded), as sparks on their floor do.
        AJFX_CHECK(compare(cpu.emitter(0), read(gpu), 1.0e-3F) >= (tick <= 40U ? 1.0F : 0.98F));
    }
    AJFX_CHECK(cpu.emitter(0).alive() > 300U);
    // A field the effect names but was not given is refused.
    const Result<gpu::GpuSimulation> missing =
        gpu::GpuSimulation::create(*g_device, effect, kernels, 2U, FieldSet{{"ball", fields.at("ball")}});
    AJFX_CHECK(!missing.ok() && missing.error().code == ErrorCode::MissingField);
}

void rigid_bodies_match_the_reference() {
    // Tumbling chips thrown up inside a box, onto its floor and a ball.
    EffectBuilder builder{"debris"};
    builder.emitter("chips")
        .capacity(700U)
        .seed(11U)
        .spawn(SpawnRate{.rate = 150.0F})
        .init(Lifetime{.min = 3.0F, .max = 4.0F})
        .init(ShapeLocation{.shape = Shape::Sphere, .center = Vec3{0.0F, 1.0F, 0.0F}, .radius_min = 0.2F, .radius_max = 0.2F})
        .init(Velocity{.speed_min = 2.0F, .speed_max = 4.0F, .spread = 70.0F})
        .init(InitSize{.min = 0.05F, .max = 0.2F})
        .init(InitTumble{.speed_min = 90.0F, .speed_max = 720.0F})
        .update(Gravity{})
        .update(RigidBody{.angular_drag = 1.0F, .roundness = 0.4F})
        .update(PlaneCollision{.restitution = 0.5F, .friction = 0.7F})
        .update(BoxCollision{.half_size = Vec3{1.5F, 3.0F, 1.5F}, .inside = true, .restitution = 0.5F,
                             .friction = 0.3F})
        .update(SphereCollision{.center = Vec3{0.8F, 0.0F, 0.0F}, .radius = 0.5F, .restitution = 0.6F,
                                .friction = 0.5F});
    const Effect effect = builder.build().value();
    Simulation cpu = make_cpu(effect, 4U);
    gpu::GpuSimulation gpu = make_gpu(effect, 4U);
    for (const std::uint64_t tick : {1U, 30U, 90U, 180U}) {
        cpu.advance_to(tick);
        gpu.advance_to(tick);
        const float within = compare(cpu.emitter(0), read(gpu), 1.0e-3F);
        std::cout << "  tick " << tick << ": " << cpu.emitter(0).alive() << " chips, " << within * 100.0F
                  << "% within tolerance\n";
        // Alike until chips pile on the floor, where a last-bit difference
        // can put a contact on either side of the rest speed.
        AJFX_CHECK(within >= (tick <= 30U ? 1.0F : 0.98F));
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

gpu::ParticleSorter make_sorter(const Effect& effect) {
    Result<gpu::KernelSet> kernels = gpu::compile_kernels(effect, *g_compiler);
    AJFX_CHECK(kernels.ok());
    Result<gpu::ParticleSorter> sorter = gpu::ParticleSorter::create(*g_device, effect, kernels.value());
    if (!sorter) {
        std::cout << sorter.error().message << std::endl;
    }
    AJFX_CHECK(sorter.ok());
    return std::move(sorter).value();
}

// The drawing order a sort produced, for `alive` particles.
std::vector<std::uint32_t> order(gpu::Buffer buffer, std::size_t alive) {
    AJFX_CHECK(buffer.valid());
    if (alive == 0U) {
        AJFX_CHECK(g_device->flush().ok());
        return {};
    }
    Result<std::vector<std::uint8_t>> read = g_device->read_buffer(buffer, 0U, alive * 4U);
    AJFX_CHECK(read.ok());
    std::vector<std::uint32_t> out(alive);
    std::memcpy(out.data(), read.value().data(), alive * 4U);
    return out;
}

Effect burst(std::uint32_t count, Shape shape) {
    EffectBuilder builder{"burst"};
    builder.emitter("e")
        .capacity(std::max(count, 1U))
        .spawn(SpawnBurst{.count = count})
        .init(Lifetime{.min = 10.0F, .max = 10.0F})
        .init(ShapeLocation{.shape = shape, .radius_min = 5.0F, .radius_max = 5.0F});
    return builder.build().value();
}

void depth_sort_orders_far_to_near_at_block_edges() {
    // Counts either side of the 4096-particle block, and many blocks.
    const Vec3 axis{0.3F, -0.5F, 0.8F};
    for (const std::uint32_t count : {1U, 4095U, 4096U, 4097U, 20000U}) {
        const Effect effect = burst(count, Shape::Sphere);
        gpu::GpuSimulation simulation = make_gpu(effect);
        gpu::ParticleSorter sorter = make_sorter(effect);
        simulation.step();
        const std::vector<std::uint32_t> drawn = order(sorter.sort(simulation, 0U, axis), count);
        const EmitterState state = read(simulation);
        AJFX_CHECK(state.alive() == count);

        // Every record once.
        std::vector<std::uint32_t> sorted = drawn;
        std::sort(sorted.begin(), sorted.end());
        std::vector<std::uint32_t> every(count);
        std::iota(every.begin(), every.end(), 0U);
        AJFX_CHECK(sorted == every);
        // Farthest along the axis first, to the sort's resolution of one
        // part in 32768.
        const auto depth = [&](std::uint32_t i) { return dot(state.position[i], axis); };
        for (std::size_t k = 1U; k < drawn.size(); ++k) {
            const float before = depth(drawn[k - 1U]);
            const float after = depth(drawn[k]);
            AJFX_CHECK(before >= after - std::fabs(after) / 16384.0F);
        }
    }
}

void equal_depths_keep_birth_order() {
    // Every particle at the origin: one key, so a stable sort changes nothing,
    // across two blocks.
    const Effect effect = burst(5000U, Shape::Point);
    gpu::GpuSimulation simulation = make_gpu(effect);
    gpu::ParticleSorter sorter = make_sorter(effect);
    simulation.step();
    std::vector<std::uint32_t> birth(5000U);
    std::iota(birth.begin(), birth.end(), 0U);
    AJFX_CHECK(order(sorter.sort(simulation, 0U, Vec3{0.0F, 0.0F, 1.0F}), 5000U) == birth);
}

void sorting_is_reproducible_and_seek_invariant() {
    const Effect effect = presets::smoke();
    const Vec3 axis{0.0F, 0.6F, -0.8F};
    gpu::GpuSimulation first = make_gpu(effect, 3U);
    gpu::ParticleSorter first_sorter = make_sorter(effect);
    first.advance_to(150U);
    const std::size_t alive = read(first).alive();
    AJFX_CHECK(alive > 10U);
    const std::vector<std::uint32_t> straight = order(first_sorter.sort(first, 0U, axis), alive);

    // Another simulation and sorter, reaching the same step by a checkpoint.
    gpu::GpuSimulation second = make_gpu(effect, 3U);
    gpu::ParticleSorter second_sorter = make_sorter(effect);
    second.advance_to(100U);
    Result<gpu::GpuCheckpoint> checkpoint = second.save();
    AJFX_CHECK(checkpoint.ok());
    second.advance_to(170U);
    static_cast<void>(second_sorter.sort(second, 0U, axis));
    second.restore(checkpoint.value());
    second.advance_to(150U);
    AJFX_CHECK(order(second_sorter.sort(second, 0U, axis), alive) == straight);
}

void only_order_dependent_emitters_sort() {
    // Additive sparks opt out; an empty emitter sorts to nothing.
    const Effect sparks = presets::sparks();
    AJFX_CHECK(!gpu::sorts(sparks.emitters[0]));
    gpu::GpuSimulation simulation = make_gpu(sparks);
    gpu::ParticleSorter sorter = make_sorter(sparks);
    simulation.advance_to(10U);
    AJFX_CHECK(!sorter.sort(simulation, 0U, Vec3{0.0F, 0.0F, 1.0F}).valid());

    const Effect smoke = presets::smoke();
    AJFX_CHECK(gpu::sorts(smoke.emitters[0]));
    gpu::GpuSimulation empty = make_gpu(smoke);
    gpu::ParticleSorter empty_sorter = make_sorter(smoke);
    AJFX_CHECK(order(empty_sorter.sort(empty, 0U, Vec3{0.0F, 0.0F, 1.0F}), 0U).empty());
}

} // namespace

// Sparks from two spawn modules thrown by a blast as they fly: each newborn's
// spawn index comes from its own module's count, on the GPU as on the CPU,
// and the shockwave throws every particle as the reference does, each by its
// own factor.
Effect blasted_sparks() {
    EffectBuilder builder{"blasted"};
    builder.blast(Blast{.name = "boom", .center = Vec3{0.3F, 0.5F, -0.2F}, .power = 8.0F, .radius = 2.5F,
                        .time = 0.5F});
    builder.emitter("sparks")
        .capacity(2000U)
        .seed(6U)
        .spawn(SpawnRate{.rate = 120.0F})
        .spawn(SpawnBurst{.time = 0.2F, .count = 50U, .interval = 0.3F, .cycles = 0U})
        .init(Lifetime{.min = 1.0F, .max = 2.0F})
        .init(ShapeLocation{.shape = Shape::Sphere, .radius_min = 0.0F, .radius_max = 1.5F})
        .init(Velocity{.speed_min = 0.1F, .speed_max = 0.5F, .spread = 180.0F})
        .update(Shockwave{.blast = "boom", .scale_min = 0.25F, .scale_max = 1.75F})
        .update(Drag{.coefficient = 1.0F});
    return builder.build().value();
}

void shockwaves_match_the_reference() {
    const Effect effect = blasted_sparks();
    Simulation cpu = make_cpu(effect, 5U);
    gpu::GpuSimulation gpu = make_gpu(effect, 5U);
    for (const std::uint64_t tick : {12U, 30U, 31U, 45U, 90U}) {
        cpu.advance_to(tick);
        gpu.advance_to(tick);
        const EmitterState& c = cpu.emitter(0);
        const EmitterState g = read(gpu);
        AJFX_CHECK(compare(c, g, 1.0e-3F) == 1.0F);
        // Both sources are among the live particles once the bursts begin.
        bool burst = false;
        for (const std::uint32_t index : g.spawn_index) {
            burst = burst || spawn_source(index) == 1U;
        }
        AJFX_CHECK(tick < 13U || burst);
    }
    // The blast happened: right after it, particles near its centre fly far
    // faster than anything the launch or drag gives.
    Simulation probe = make_cpu(effect, 5U);
    probe.advance_to(31U);
    float fastest = 0.0F;
    for (const Vec3& v : probe.emitter(0).velocity) {
        fastest = std::max(fastest, std::sqrt(dot(v, v)));
    }
    AJFX_CHECK(fastest > 2.0F);

    // Reruns are bit-identical, and a checkpoint taken before the blast
    // finds it again.
    gpu::GpuSimulation a = make_gpu(effect, 5U);
    gpu::GpuSimulation b = make_gpu(effect, 5U);
    a.advance_to(20U);
    Result<gpu::GpuCheckpoint> before = a.save();
    AJFX_CHECK(before.ok());
    a.advance_to(60U);
    b.advance_to(60U);
    const std::vector<std::uint8_t> straight = bytes(a);
    AJFX_CHECK(!straight.empty() && straight == bytes(b));
    a.restore(before.value());
    a.advance_to(60U);
    AJFX_CHECK(bytes(a) == straight);
}

int main() {
    // Hardware first, then WARP. AJFX_TEST_WARP=1 forces WARP;
    // AJFX_TEST_DEBUG_LAYER=1 fails any flush the debug layer objects to.
    const char* force_warp = std::getenv("AJFX_TEST_WARP");
    const bool debug_layer = std::getenv("AJFX_TEST_DEBUG_LAYER") != nullptr;
    Result<std::unique_ptr<d3d12::Device>> device =
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
        {"shockwaves_match_the_reference", shockwaves_match_the_reference},
        {"distance_fields_collide_as_the_reference_does", distance_fields_collide_as_the_reference_does},
        {"rigid_bodies_match_the_reference", rigid_bodies_match_the_reference},
        {"presets_match_the_reference_within_tolerance", presets_match_the_reference_within_tolerance},
        {"rolling_turbulence_matches_the_reference_within_tolerance",
         rolling_turbulence_matches_the_reference_within_tolerance},
        {"reruns_are_bit_identical", reruns_are_bit_identical},
        {"checkpoints_and_reset_reproduce_the_timeline", checkpoints_and_reset_reproduce_the_timeline},
        {"compaction_spans_many_blocks", compaction_spans_many_blocks},
        {"buffers_expose_the_live_particles", buffers_expose_the_live_particles},
        {"depth_sort_orders_far_to_near_at_block_edges", depth_sort_orders_far_to_near_at_block_edges},
        {"equal_depths_keep_birth_order", equal_depths_keep_birth_order},
        {"sorting_is_reproducible_and_seek_invariant", sorting_is_reproducible_and_seek_invariant},
        {"only_order_dependent_emitters_sort", only_order_dependent_emitters_sort},
    };
    const int result = ajfx::test::run(cases);
    g_compiler.reset();
    g_device.reset();
    return result;
}
