// getenv is fine for reading one test switch.
#define _CRT_SECURE_NO_WARNINGS

#include "check.hpp"

#include <ajfx/builder.hpp>
#include <ajfx/d3d12.hpp>
#include <ajfx/gpu/renderer.hpp>
#include <ajfx/gpu/simulation.hpp>
#include <ajfx/gpu/sort.hpp>
#include <ajfx/mesh.hpp>
#include <ajfx/presets.hpp>
#include <ajfx/rigid.hpp>
#include <ajfx/rigid_runner.hpp>
#include <ajfx/simulation.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

// The GPU runtime's rigid physics emitters: simulated on the CPU by the
// reference's own code and written into the emitters' buffers, so the GPU
// holds exactly what the reference computes, beside emitters it simulates
// itself.
namespace {

using namespace ajfx;

std::unique_ptr<gpu::Device> g_device;
std::unique_ptr<gpu::ShaderCompiler> g_compiler;
std::unique_ptr<RigidSolver> g_solver;

// Tumbling blocks in a box beside sparks the GPU simulates.
Effect mixed() {
    EffectBuilder builder{"mixed"};
    builder.float_parameter("throw", 3.0F, 1.0F, 6.0F);
    builder.emitter("sparks")
        .capacity(512U)
        .spawn(SpawnRate{.rate = 60.0F})
        .init(Lifetime{.min = 1.0F, .max = 2.0F})
        .init(Velocity{.speed_min = 1.0F, .speed_max = 3.0F, .spread = 60.0F})
        .update(Gravity{});
    builder.emitter("blocks")
        .capacity(256U)
        .seed(2U)
        .spawn(SpawnRate{.rate = 80.0F})
        .init(Lifetime{.min = 1.0F, .max = 2.5F})
        .init(ShapeLocation{.shape = Shape::Sphere, .center = Vec3{0.0F, 1.0F, 0.0F}, .radius_min = 0.4F, .radius_max = 0.4F})
        .init(Velocity{.speed_min = 1.0F, .speed_max = bind("throw"), .spread = 180.0F})
        .init(InitColor{.a = Color{1.0F, 0.5F, 0.2F, 1.0F}, .b = Color{0.2F, 0.5F, 1.0F, 1.0F}})
        .init(InitSize{.min = 0.1F, .max = 0.2F})
        .init(InitTumble{})
        .update(Gravity{})
        .update(RigidPhysics{.shape = RigidShape::Box})
        .update(BoxCollision{.center = Vec3{0.0F, 1.0F, 0.0F}, .half_size = Vec3{1.0F, 1.0F, 1.0F}, .inside = true})
        .update(ColorOverLife{});
    return builder.build().value();
}

class Ramp final : public ParameterProvider {
public:
    void values_at(std::uint64_t tick, std::span<ParameterValue> values) const override {
        values[0][0] = 1.0F + static_cast<float>(tick % 100U) * 0.05F;
    }
};

gpu::GpuSimulation make_gpu(const Effect& effect, const MeshSet& meshes = {}) {
    const gpu::KernelSet kernels = gpu::compile_kernels(effect, *g_compiler).value();
    Result<gpu::GpuSimulation> simulation = gpu::GpuSimulation::create(
        *g_device, effect, kernels, 5U, SimulationOptions{.meshes = meshes, .rigid = g_solver.get()});
    if (!simulation) {
        std::cout << simulation.error().message << '\n';
    }
    AJFX_CHECK(simulation.ok());
    return std::move(simulation).value();
}

void the_gpu_holds_what_the_reference_simulates() {
    const Effect effect = mixed();
    const Ramp ramp;
    gpu::GpuSimulation gpu = make_gpu(effect);
    Simulation cpu = Simulation::create(effect, 5U, SimulationOptions{.rigid = g_solver.get()}).value();
    for (const std::uint64_t tick : {1U, 40U, 150U}) {
        gpu.advance_to(tick, &ramp);
        cpu.advance_to(tick, &ramp);
        const EmitterState g = gpu.read_emitter(1U).value();
        const EmitterState& c = cpu.emitter(1U);
        AJFX_CHECK(tick == 1U || c.alive() > 50U);
        AJFX_CHECK(g.alive() == c.alive() && g.spawned == c.spawned && g.refused == c.refused);
        AJFX_CHECK(g.position == c.position && g.orientation == c.orientation);
        AJFX_CHECK(g.velocity == c.velocity && g.angular_velocity == c.angular_velocity);
        AJFX_CHECK(g.age == c.age && g.lifetime == c.lifetime && g.spawn_index == c.spawn_index);
        AJFX_CHECK(g.color == c.color && g.size == c.size);
        // The sparks beside them run on the GPU as ever.
        AJFX_CHECK(gpu.read_emitter(0U).value().alive() == cpu.emitter(0U).alive());
    }
}

void checkpoints_put_rigid_emitters_back_exactly() {
    const Effect effect = mixed();
    gpu::GpuSimulation gpu = make_gpu(effect);
    gpu.advance_to(60U);
    Result<gpu::GpuCheckpoint> checkpoint = gpu.save();
    AJFX_CHECK(checkpoint.ok());
    gpu.advance_to(200U);
    const std::vector<std::uint8_t> straight = gpu.read_particle_bytes(1U).value();
    AJFX_CHECK(!straight.empty());
    gpu.restore(checkpoint.value());
    gpu.advance_to(200U);
    AJFX_CHECK(gpu.read_particle_bytes(1U).value() == straight);
    gpu.reset();
    AJFX_CHECK(gpu.read_emitter(1U).value().alive() == 0U);
    gpu.advance_to(200U);
    AJFX_CHECK(gpu.read_particle_bytes(1U).value() == straight);
}

void rubble_draws_its_chips() {
    const Effect effect = presets::rubble();
    const MeshSet meshes = presets::meshes();
    const gpu::Camera camera = gpu::Camera::look_at(Vec3{0.0F, 2.5F, 6.0F}, Vec3{0.0F, 0.5F, 0.0F},
                                                    Vec3{0.0F, 1.0F, 0.0F}, 1.0F, 1.0F, 0.1F, 100.0F);
    const auto draw = [&] {
        const gpu::KernelSet kernels = gpu::compile_kernels(effect, *g_compiler).value();
        gpu::GpuSimulation simulation = make_gpu(effect, meshes);
        gpu::ParticleRenderer renderer =
            gpu::ParticleRenderer::create(*g_device, effect, kernels, TextureSet{}, gpu::TextureFormat::Rgba8Unorm,
                                          meshes)
                .value();
        gpu::ParticleSorter sorter = gpu::ParticleSorter::create(*g_device, effect, kernels).value();
        const gpu::Texture target =
            g_device->create_render_target(128U, 128U, gpu::TextureFormat::Rgba8Unorm, "rubble").value();
        simulation.advance_to(180U);
        const std::array<float, 4> clear{0.0F, 0.0F, 0.0F, 0.0F};
        renderer.render(simulation, sorter, camera, target, &clear);
        std::vector<std::uint8_t> pixels = g_device->read_texture(target).value();
        g_device->destroy_texture(target);
        return pixels;
    };
    const std::vector<std::uint8_t> a = draw();
    std::size_t lit = 0U;
    for (std::size_t i = 3U; i < a.size(); i += 4U) {
        lit += a[i] != 0U ? 1U : 0U;
    }
    std::cout << "  " << lit << " of 16384 pixels covered\n";
    AJFX_CHECK(lit > 1000U);
    AJFX_CHECK(draw() == a);
}

// Snow falling on one crate: the crate is a body the CPU steps, and the snow
// is an emitter the GPU simulates, meeting that body through its own kernel.
Effect snow_on_a_crate() {
    EffectBuilder builder{"snow"};
    builder.emitter("crates")
        .capacity(8U)
        .spawn(SpawnPile{.count = 1U, .shape = PileShape::Box, .jitter = 0.0F, .release = 0.0F})
        .init(Lifetime{.min = 30.0F, .max = 30.0F})
        .init(InitSize{.min = 1.0F, .max = 1.0F})
        .update(RigidPhysics{.shape = RigidShape::Box, .friction = 0.9F, .restitution = 0.0F});
    EmitterBuilder& flakes = builder.emitter("snow");
    flakes.capacity(512U)
        .seed(11U)
        .spawn(SpawnRate{.rate = 60.0F})
        .init(Lifetime{.min = 8.0F, .max = 8.0F})
        .init(ShapeLocation{.shape = Shape::Disc, .center = Vec3{0.0F, 4.0F, 0.0F}, .radius_min = 0.0F,
                            .radius_max = 1.0F})
        .init(Velocity{.direction = Vec3{0.0F, -1.0F, 0.0F}, .speed_min = 0.8F, .speed_max = 1.2F})
        .init(InitSize{.min = 0.05F, .max = 0.05F})
        .update(Gravity{.acceleration = Vec3{0.0F, -2.0F, 0.0F}})
        .update(BodyCollision{.emitter = "crates", .restitution = 0.0F, .friction = 1.0F})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 1.0F});
    return builder.build().value();
}

void the_gpu_meets_the_same_bodies_as_the_reference() {
    const Effect effect = snow_on_a_crate();
    gpu::GpuSimulation gpu = make_gpu(effect);
    Simulation cpu = Simulation::create(effect, 5U, SimulationOptions{.rigid = g_solver.get()}).value();
    for (const std::uint64_t tick : {60U, 240U, 600U}) {
        gpu.advance_to(tick);
        cpu.advance_to(tick);
        const EmitterState read = gpu.read_emitter(1U).value();
        const EmitterState& reference = cpu.emitter(1U);
        AJFX_CHECK(read.alive() == reference.alive());
        AJFX_CHECK(read.spawned == reference.spawned);
        std::size_t apart = 0U;
        for (std::size_t i = 0U; i < read.alive(); ++i) {
            // The kernel and the reference do the same arithmetic in the same
            // order, so a flake resting on the crate agrees closely; a flake
            // still falling can differ by a step's worth of rounding.
            const Vec3 d{read.position[i].x - reference.position[i].x, read.position[i].y - reference.position[i].y,
                         read.position[i].z - reference.position[i].z};
            if (std::sqrt(dot(d, d)) > 1.0e-3F) {
                ++apart;
            }
        }
        // And they land on the crate's lid, which is what there is to agree
        // about: a flake that fell through would be a metre out.
        std::size_t on_the_lid = 0U;
        for (std::size_t i = 0U; i < read.alive(); ++i) {
            if (std::fabs(read.position[i].x) < 0.35F && std::fabs(read.position[i].z) < 0.35F &&
                read.position[i].y > 1.3F && read.position[i].y < 1.45F) {
                ++on_the_lid;
            }
        }
        std::cout << "  step " << tick << ": " << read.alive() << " flakes, " << apart << " apart from the reference, "
                  << on_the_lid << " resting on the crate\n";
        AJFX_CHECK(apart == 0U);
        if (tick >= 240U) {
            AJFX_CHECK(on_the_lid > 10U);
        }
    }
}

// The cube model at half its size, so a body drawn as it covers a quarter of
// the pixels a body drawn as the cube does.
MeshData half_cube_mesh() {
    const MeshData cube = cube_mesh();
    std::vector<MeshVertex> vertices = cube.vertices;
    for (MeshVertex& vertex : vertices) {
        vertex.position = Vec3{vertex.position.x * 0.5F, vertex.position.y * 0.5F, vertex.position.z * 0.5F};
    }
    return make_mesh(std::move(vertices), cube.indices).value();
}

void body_kinds_draw_their_own_models_in_depth_order() {
    // Two bodies of one emitter drawn as two models, one in front of the
    // other. The first born is kind 0, drawn as the emitter's own half cube,
    // and is nearer; the second is kind 1, drawn as the whole cube, and is
    // behind it. Colour by age tells them apart: the older red, the younger
    // green.
    //
    // Drawing model by model would paint kind 0's model first and kind 1's
    // over it, hiding the nearer body behind the farther one. Every model of
    // an emitter is in one pair of buffers, so one draw covers them all and
    // the order is still the sort's: the near body stays on top.
    EffectBuilder builder{"models"};
    builder.emitter("pieces")
        .capacity(8U)
        .spawn(SpawnBurst{.time = 0.0F, .count = 1U, .body = 0U})
        .spawn(SpawnBurst{.time = 0.5F, .count = 1U, .body = 1U})
        .init(Lifetime{.min = 10.0F, .max = 10.0F})
        .init(ShapeLocation{.shape = Shape::Point})
        .init(Velocity{.direction = Vec3{0.0F, 0.0F, 1.0F}, .speed_min = 1.0F, .speed_max = 1.0F})
        .init(InitSize{.min = 1.0F, .max = 1.0F})
        .update(RigidPhysics{.shape = RigidShape::Sphere,
                             .bodies = {RigidBodyKind{.shape = RigidShape::Sphere, .draw = "cube"}}})
        .update(ColorOverLife{.keys = {{0.0F, Color{0.0F, 1.0F, 0.0F, 1.0F}},
                                       {0.03F, Color{0.0F, 1.0F, 0.0F, 1.0F}},
                                       {0.05F, Color{1.0F, 0.0F, 0.0F, 1.0F}},
                                       {1.0F, Color{1.0F, 0.0F, 0.0F, 1.0F}}}})
        .renderer(SpriteRenderer{.geometry = Geometry::Mesh, .mesh = "half"});
    const Effect effect = builder.build().value();
    AJFX_CHECK(models_by_kind(effect.emitters[0]));
    const MeshSet meshes{{"half", half_cube_mesh()}, {"cube", cube_mesh()}};
    // A model a body kind is drawn as has to be in the package.
    AJFX_CHECK(!check_meshes(effect, MeshSet{{"half", half_cube_mesh()}}).ok());

    const gpu::KernelSet kernels = gpu::compile_kernels(effect, *g_compiler).value();
    gpu::GpuSimulation simulation = make_gpu(effect, meshes);
    gpu::ParticleRenderer renderer =
        gpu::ParticleRenderer::create(*g_device, effect, kernels, TextureSet{}, gpu::TextureFormat::Rgba8Unorm,
                                      meshes)
            .value();
    gpu::ParticleSorter sorter = gpu::ParticleSorter::create(*g_device, effect, kernels).value();
    const gpu::Texture target =
        g_device->create_render_target(64U, 64U, gpu::TextureFormat::Rgba8Unorm, "models").value();
    // 0.6 s: the first at z = 0.6, the second at z = 0.1, one behind the
    // other, both still centred on the view's axis.
    simulation.advance_to(36U);
    const EmitterState state = simulation.read_emitter(0U).value();
    AJFX_CHECK(state.alive() == 2U && state.position[0].z > state.position[1].z + 0.4F);
    AJFX_CHECK(state.color[0].r == 1.0F && state.color[1].g == 1.0F);
    const std::array<float, 4> clear{0.0F, 0.0F, 0.0F, 0.0F};
    // Orthographic, 4 world units across 64 pixels: 16 pixels per unit.
    renderer.render(simulation, sorter, gpu::Camera::orthographic(Vec3{}, 2.0F, 2.0F), target, &clear);
    const std::vector<std::uint8_t> pixels = g_device->read_texture(target).value();
    g_device->destroy_texture(target);
    const auto at = [&pixels](std::uint32_t x, std::uint32_t y) {
        const std::size_t i = (static_cast<std::size_t>(y) * 64U + x) * 4U;
        return std::array<std::uint8_t, 4>{pixels[i], pixels[i + 1U], pixels[i + 2U], pixels[i + 3U]};
    };
    // The middle is the near body's: the half cube, half a unit across, so
    // four pixels either side of the centre, and red for its age.
    const auto middle = at(32U, 32U);
    std::cout << "  the middle is (" << static_cast<int>(middle[0]) << ", " << static_cast<int>(middle[1]) << ")\n";
    AJFX_CHECK(middle[0] >= 250U && middle[1] == 0U);
    // Six pixels out is past the half cube and within the whole one, which
    // is the farther body: green.
    const auto ring = at(38U, 32U);
    AJFX_CHECK(ring[1] >= 250U && ring[0] == 0U);
    // Ten pixels out is past both.
    AJFX_CHECK(at(42U, 32U)[3] == 0U);
}

bool same(const EmitterState& g, const EmitterState& c) {
    return g.alive() == c.alive() && g.spawned == c.spawned && g.position == c.position &&
           g.orientation == c.orientation && g.velocity == c.velocity && g.angular_velocity == c.angular_velocity &&
           g.age == c.age && g.spawn_index == c.spawn_index && g.color == c.color && g.size == c.size;
}

void piles_stand_on_the_gpu_from_the_start() {
    EffectBuilder builder{"heap"};
    builder.emitter("stones")
        .capacity(128U)
        .spawn(SpawnPile{.count = 80U, .jitter = 0.3F, .release = 0.34F})
        .init(Lifetime{.min = 20.0F, .max = 20.0F})
        .init(InitSize{.min = 0.1F, .max = 0.2F})
        .init(InitColor{.a = Color{0.4F, 0.4F, 0.4F, 1.0F}, .b = Color{0.6F, 0.5F, 0.4F, 1.0F}})
        .init(InitTumble{})
        .update(Gravity{})
        .update(RigidPhysics{.shape = RigidShape::Box})
        .update(PlaneCollision{});
    const Effect effect = builder.build().value();
    gpu::GpuSimulation gpu = make_gpu(effect);
    Simulation cpu = Simulation::create(effect, 5U, SimulationOptions{.rigid = g_solver.get()}).value();
    // Before any step the buffers hold the pile.
    const EmitterState start = gpu.read_emitter(0U).value();
    AJFX_CHECK(start.alive() == 80U && same(start, cpu.emitter(0U)));
    gpu.advance_to(90U);
    cpu.advance_to(90U);
    AJFX_CHECK(same(gpu.read_emitter(0U).value(), cpu.emitter(0U)));
    AJFX_CHECK(cpu.emitter(0U).position != start.position);
    gpu.reset();
    AJFX_CHECK(same(gpu.read_emitter(0U).value(), start));
}

void hosts_write_the_rigid_emitters_they_step() {
    const Effect effect = mixed();
    const gpu::KernelSet kernels = gpu::compile_kernels(effect, *g_compiler).value();
    // Hosted: no solver, and stepping leaves the blocks to the host.
    gpu::GpuSimulation gpu =
        gpu::GpuSimulation::create(*g_device, effect, kernels, 5U, SimulationOptions{}, gpu::RigidHosting::Host)
            .value();
    auto ramp = std::make_shared<Ramp>();
    RigidRunner runner{*g_solver};
    std::unique_ptr<RigidTrack> track = runner.track(effect, 5U, {}, ramp);
    Simulation cpu = Simulation::create(effect, 5U, SimulationOptions{.rigid = g_solver.get()}).value();
    for (const std::uint64_t tick : {90U, 30U}) {
        if (tick < gpu.tick()) {
            gpu.reset(ramp.get());
            cpu.reset(ramp.get());
        }
        gpu.advance_to(tick, ramp.get());
        cpu.advance_to(tick, ramp.get());
        std::shared_ptr<const RigidFrame> frame = track->frame(tick);
        if (!frame) {
            runner.wait_idle();
            frame = track->frame(tick);
        }
        AJFX_CHECK(frame != nullptr);
        gpu.write_rigid(1U, frame->emitters[1]);
        AJFX_CHECK(same(gpu.read_emitter(1U).value(), cpu.emitter(1U)));
        AJFX_CHECK(gpu.read_emitter(0U).value().alive() == cpu.emitter(0U).alive());
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
    g_solver = rigid::make_jolt_solver();
    std::cout << "adapter: " << g_device->adapter_name() << '\n';
    const ajfx::test::Case cases[] = {
        {"the_gpu_holds_what_the_reference_simulates", the_gpu_holds_what_the_reference_simulates},
        {"checkpoints_put_rigid_emitters_back_exactly", checkpoints_put_rigid_emitters_back_exactly},
        {"rubble_draws_its_chips", rubble_draws_its_chips},
        {"the_gpu_meets_the_same_bodies_as_the_reference", the_gpu_meets_the_same_bodies_as_the_reference},
        {"body_kinds_draw_their_own_models_in_depth_order", body_kinds_draw_their_own_models_in_depth_order},
        {"piles_stand_on_the_gpu_from_the_start", piles_stand_on_the_gpu_from_the_start},
        {"hosts_write_the_rigid_emitters_they_step", hosts_write_the_rigid_emitters_they_step},
    };
    const int result = ajfx::test::run(cases);
    g_solver.reset();
    g_compiler.reset();
    g_device.reset();
    return result;
}
