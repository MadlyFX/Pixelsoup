#include <mediaserver/render/particle_device.hpp>

#include <ajfx/builder.hpp>
#include <ajfx/d3d12.hpp>
#include <ajfx/gpu/simulation.hpp>
#include <ajfx/presets.hpp>
#include <ajfx/simulation.hpp>

#include <algorithm>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// ajfx on the media server's RHI. The particle kernels are the same bytecode
// whichever device runs them, so on one GPU the RHI and ajfx's own D3D12
// device must leave byte-identical particle records - and both must spawn and
// retire exactly what the CPU reference does.
namespace {

namespace core = mediaserver::core;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) + ": check failed: " + expression};
    }
}

#define PARTICLE_CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

std::unique_ptr<rhi::Device> g_rhi;
std::unique_ptr<ajfx::gpu::ShaderCompiler> g_compiler;
std::unique_ptr<ajfx::d3d12::Device> g_reference;

[[nodiscard]] std::unique_ptr<render::ParticleDevice> particle_device() {
    auto created = render::ParticleDevice::create(*g_rhi);
    PARTICLE_CHECK(created);
    return std::move(created).value();
}

[[nodiscard]] ajfx::gpu::GpuSimulation simulate_on(ajfx::gpu::Device& device, const ajfx::Effect& effect) {
    ajfx::Result<ajfx::gpu::KernelSet> kernels = ajfx::gpu::compile_kernels(effect, *g_compiler);
    PARTICLE_CHECK(kernels);
    ajfx::Result<ajfx::gpu::GpuSimulation> simulation =
        ajfx::gpu::GpuSimulation::create(device, effect, kernels.value(), 4U);
    if (!simulation) {
        std::cout << simulation.error().message << '\n';
    }
    PARTICLE_CHECK(simulation);
    return std::move(simulation).value();
}

[[nodiscard]] std::vector<std::uint8_t> records(ajfx::gpu::GpuSimulation& simulation) {
    ajfx::Result<std::vector<std::uint8_t>> bytes = simulation.read_particle_bytes(0U);
    if (!bytes) {
        std::cout << bytes.error().message << '\n';
    }
    PARTICLE_CHECK(bytes);
    return std::move(bytes).value();
}

void both_backends_leave_identical_particles() {
    for (const ajfx::Effect& effect : {ajfx::presets::smoke(), ajfx::presets::sparks(), ajfx::presets::confetti()}) {
        auto device = particle_device();
        ajfx::gpu::GpuSimulation on_rhi = simulate_on(*device, effect);
        ajfx::gpu::GpuSimulation on_reference = simulate_on(*g_reference, effect);
        on_rhi.advance_to(150U);
        on_reference.advance_to(150U);
        const std::vector<std::uint8_t> rhi_records = records(on_rhi);
        PARTICLE_CHECK(!rhi_records.empty());
        PARTICLE_CHECK(rhi_records == records(on_reference));

        // And the population is the CPU reference's exactly.
        ajfx::Result<ajfx::Simulation> cpu = ajfx::Simulation::create(effect, 4U);
        PARTICLE_CHECK(cpu);
        cpu.value().advance_to(150U);
        ajfx::Result<ajfx::EmitterState> read = on_rhi.read_emitter(0U);
        PARTICLE_CHECK(read);
        PARTICLE_CHECK(read.value().spawn_index == cpu.value().emitter(0U).spawn_index);
        PARTICLE_CHECK(read.value().age == cpu.value().emitter(0U).age);
        PARTICLE_CHECK(read.value().refused == 0U);
    }
}

// A fire feeding a grid of air with smoke and heat, and motes riding the
// air: every module one the GPU reproduces to the bit, and no vorticity
// confinement, so the grids may be compared exactly.
[[nodiscard]] ajfx::Effect plume() {
    ajfx::EffectBuilder builder{"plume"};
    builder.volume(ajfx::Volume{.name = "air",
                                .origin = ajfx::Vec3{-1.0F, 0.0F, -1.0F},
                                .cell_size = 0.25F,
                                .nx = 8U,
                                .ny = 12U,
                                .nz = 8U,
                                .buoyancy = 2.0F,
                                .weight = 0.1F,
                                .pressure_iterations = 12U,
                                .density_dissipation = 0.2F,
                                .temperature_dissipation = 0.5F,
                                .velocity_dissipation = 0.1F});
    builder.emitter("fire")
        .capacity(2000U)
        .seed(3U)
        .spawn(ajfx::SpawnRate{.rate = 300.0F})
        .init(ajfx::Lifetime{.min = 0.4F, .max = 0.8F})
        .init(ajfx::ShapeLocation{.shape = ajfx::Shape::Box, .center = ajfx::Vec3{0.0F, 0.2F, 0.0F},
                                  .size = ajfx::Vec3{0.5F, 0.1F, 0.5F}})
        .init(ajfx::Velocity{.direction = ajfx::Vec3{0.0F, 1.0F, 0.0F}, .speed_min = 0.5F, .speed_max = 1.0F})
        .update(ajfx::VolumeSource{.volume = "air", .density = 4.0F, .temperature = 3.0F, .velocity = 0.5F});
    builder.emitter("motes")
        .capacity(512U)
        .seed(5U)
        .spawn(ajfx::SpawnBurst{.count = 256U})
        .init(ajfx::Lifetime{.min = 10.0F, .max = 10.0F})
        .init(ajfx::ShapeLocation{.shape = ajfx::Shape::Box, .center = ajfx::Vec3{0.0F, 1.5F, 0.0F},
                                  .size = ajfx::Vec3{1.6F, 1.0F, 1.6F}})
        .update(ajfx::VolumeAdvect{.volume = "air", .strength = 6.0F, .scale_min = 0.5F, .scale_max = 1.5F});
    return builder.build().value();
}

[[nodiscard]] ajfx::VolumeState grid(ajfx::gpu::GpuSimulation& simulation) {
    ajfx::Result<ajfx::VolumeState> cells = simulation.read_volume(0U);
    if (!cells) {
        std::cout << cells.error().message << '\n';
    }
    PARTICLE_CHECK(cells);
    return std::move(cells).value();
}

void grid_volumes_step_identically_on_both_backends() {
    const ajfx::Effect effect = plume();
    auto device = particle_device();
    ajfx::gpu::GpuSimulation on_rhi = simulate_on(*device, effect);
    ajfx::gpu::GpuSimulation on_reference = simulate_on(*g_reference, effect);
    on_rhi.advance_to(90U);
    on_reference.advance_to(90U);
    // The grid the RHI's kernels left is the reference device's to the bit,
    // and so are the motes it carried.
    const ajfx::VolumeState smoke = grid(on_rhi);
    PARTICLE_CHECK(smoke.cells() == 8U * 12U * 8U);
    PARTICLE_CHECK(std::any_of(smoke.density.begin(), smoke.density.end(), [](float d) { return d > 0.0F; }));
    PARTICLE_CHECK(smoke == grid(on_reference));
    ajfx::Result<std::vector<std::uint8_t>> motes = on_rhi.read_particle_bytes(1U);
    ajfx::Result<std::vector<std::uint8_t>> reference_motes = on_reference.read_particle_bytes(1U);
    PARTICLE_CHECK(motes && reference_motes && !motes.value().empty() && motes.value() == reference_motes.value());
    // And the grid is the CPU reference's exactly.
    ajfx::Result<ajfx::Simulation> cpu = ajfx::Simulation::create(effect, 4U);
    PARTICLE_CHECK(cpu);
    cpu.value().advance_to(90U);
    PARTICLE_CHECK(smoke == cpu.value().volume(0U));
    // The grid for drawing resolves to an RHI buffer holding that many cells.
    const ajfx::gpu::VolumeBuffers cells = on_rhi.volume_buffers(0U);
    PARTICLE_CHECK(device->buffer(cells.buffer) != nullptr);
    PARTICLE_CHECK(cells.nx == 8U && cells.ny == 12U && cells.nz == 8U && cells.cell_size == 0.25F);
}

void steps_record_into_the_renderers_list() {
    const ajfx::Effect effect = ajfx::presets::smoke();
    auto device = particle_device();
    ajfx::gpu::GpuSimulation simulation = simulate_on(*device, effect);

    // As a frame would: the renderer's list, submitted by the renderer.
    auto acquired = g_rhi->acquire_commands();
    PARTICLE_CHECK(acquired);
    rhi::CommandList commands = std::move(acquired).value();
    device->begin_recording(commands);
    simulation.advance_to(90U);
    // Flushing would submit someone else's list; it is refused.
    PARTICLE_CHECK(!device->flush());
    device->end_recording();
    PARTICLE_CHECK(!device->take_error());
    auto submitted = g_rhi->submit(std::move(commands));
    PARTICLE_CHECK(submitted);
    PARTICLE_CHECK(std::move(submitted).value().wait());

    ajfx::gpu::GpuSimulation reference = simulate_on(*g_reference, effect);
    reference.advance_to(90U);
    PARTICLE_CHECK(records(simulation) == records(reference));

    // The draw-facing view of the buffers resolves to RHI buffers.
    const ajfx::gpu::EmitterBuffers buffers = simulation.emitter_buffers(0U);
    PARTICLE_CHECK(device->buffer(buffers.particles) != nullptr);
    PARTICLE_CHECK(device->buffer(buffers.args) != nullptr);
}

void checkpoints_resume_exactly() {
    auto device = particle_device();
    ajfx::gpu::GpuSimulation simulation = simulate_on(*device, ajfx::presets::confetti());
    simulation.advance_to(50U);
    ajfx::Result<ajfx::gpu::GpuCheckpoint> saved = simulation.save();
    PARTICLE_CHECK(saved);
    simulation.advance_to(140U);
    const std::vector<std::uint8_t> straight = records(simulation);
    simulation.restore(saved.value());
    simulation.advance_to(140U);
    PARTICLE_CHECK(records(simulation) == straight);
}

void released_buffers_wait_for_the_gpu() {
    auto device = particle_device();
    auto acquired = g_rhi->acquire_commands();
    PARTICLE_CHECK(acquired);
    rhi::CommandList commands = std::move(acquired).value();
    device->begin_recording(commands);
    {
        ajfx::gpu::GpuSimulation simulation = simulate_on(*device, ajfx::presets::smoke());
        simulation.advance_to(30U);
        // Released while a list that uses them is still open: held, not freed.
    }
    PARTICLE_CHECK(device->retired() > 0U);
    device->end_recording();
    auto submitted = g_rhi->submit(std::move(commands));
    PARTICLE_CHECK(submitted);
    PARTICLE_CHECK(std::move(submitted).value().wait());

    // Once that work has completed, the next recording frees them.
    auto next = g_rhi->acquire_commands();
    PARTICLE_CHECK(next);
    rhi::CommandList later = std::move(next).value();
    device->begin_recording(later);
    device->end_recording();
    PARTICLE_CHECK(device->retired() == 0U);
}

void drawing_and_unsupported_devices_are_refused() {
    auto device = particle_device();
    PARTICLE_CHECK(!device->create_graphics_pipeline({}, "sprites"));
    PARTICLE_CHECK(!device->create_render_target(4U, 4U, ajfx::gpu::TextureFormat::Rgba8Unorm, "target"));

    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::SdlGpu;
    auto sdl = rhi::Device::create(config);
    if (sdl) {
        auto refused = render::ParticleDevice::create(sdl.value());
        PARTICLE_CHECK(!refused && refused.error() == rhi::RhiError::OperationUnsupported);
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
    g_rhi = std::make_unique<rhi::Device>(std::move(device).value());
    g_compiler = std::move(compiler).value();
    g_reference = std::move(reference).value();

    struct Case final {
        const char* name;
        void (*run)();
    };
    const Case cases[] = {
        {"both_backends_leave_identical_particles", both_backends_leave_identical_particles},
        {"grid_volumes_step_identically_on_both_backends", grid_volumes_step_identically_on_both_backends},
        {"steps_record_into_the_renderers_list", steps_record_into_the_renderers_list},
        {"checkpoints_resume_exactly", checkpoints_resume_exactly},
        {"released_buffers_wait_for_the_gpu", released_buffers_wait_for_the_gpu},
        {"drawing_and_unsupported_devices_are_refused", drawing_and_unsupported_devices_are_refused},
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
    g_rhi.reset();
    return failures == 0 ? 0 : 1;
}
