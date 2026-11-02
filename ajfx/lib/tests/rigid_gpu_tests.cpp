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
#include <chrono>
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

// A crate, and a ball dropped on its lid: sparks where the ball hits, which
// glow and settle on the crate; smoke off whatever moves; and beside them an
// emitter whose two spawn modules look different, scoped by source. The
// births are scheduled on the CPU by the reference's own code; the kernels
// set them up and move them.
Effect sparks_and_smoke() {
    EffectBuilder builder{"sparks"};
    builder.emitter("pieces")
        .capacity(8U)
        .spawn(SpawnPile{.count = 1U, .shape = PileShape::Box, .jitter = 0.0F, .release = 0.0F})
        .spawn(SpawnBurst{.time = 0.5F, .count = 1U, .body = 1U})
        .init(Lifetime{.min = 30.0F, .max = 30.0F})
        .init(ShapeLocation{.shape = Shape::Point, .center = Vec3{0.15F, 3.0F, 0.1F}, .only = Scope{.sources = {1U}}})
        .init(InitSize{.min = 1.0F, .max = 1.0F, .only = Scope{.kinds = {0U}}})
        .init(InitSize{.min = 0.3F, .max = 0.3F, .only = Scope{.kinds = {1U}}})
        .update(Gravity{})
        .update(RigidPhysics{.shape = RigidShape::Box,
                             .friction = 0.9F,
                             .restitution = 0.0F,
                             .bodies = {RigidBodyKind{.shape = RigidShape::Sphere, .friction = 0.9F,
                                                      .restitution = 0.1F}}})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 0.9F});
    builder.emitter("sparks")
        .capacity(16384U)
        .seed(7U)
        .spawn(SpawnFromContacts{.emitter = "pieces",
                                 .min_speed = 1.0F,
                                 .per_speed = 6.0F,
                                 .max_per_step = 32U,
                                 .along_normal = 0.25F,
                                 .along_surface = 0.05F,
                                 .spread = 20.0F,
                                 .bodies = Scope{.kinds = {1U}}})
        .init(Lifetime{.min = 8.0F, .max = 8.0F})
        .init(InitSize{.min = 0.02F, .max = 0.02F})
        .init(InitColor{.a = Color{0.3F, 0.3F, 0.3F, 1.0F}, .b = Color{0.3F, 0.3F, 0.3F, 1.0F}})
        .update(Gravity{})
        .update(BodyCollision{.emitter = "pieces", .restitution = 0.0F, .friction = 1.0F})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 1.0F})
        .update(Heat{.start = 1.2F, .half_life = 0.4F, .glow = 3.0F});
    builder.emitter("smoke")
        .capacity(4096U)
        .seed(13U)
        .spawn(SpawnFromBodies{.emitter = "pieces", .min_speed = 0.3F, .per_speed = 20.0F, .max_per_step = 8U,
                               .surface = 1.0F, .inherit = 0.2F})
        .init(Lifetime{.min = 2.0F, .max = 3.0F})
        .init(InitSize{.min = 0.05F, .max = 0.1F})
        .update(Gravity{.acceleration = Vec3{0.0F, 0.5F, 0.0F}})
        .update(Drag{.coefficient = 1.0F})
        .update(ColorOverLife{});
    builder.emitter("two")
        .capacity(512U)
        .seed(17U)
        .spawn(SpawnRate{.rate = 40.0F})
        .spawn(SpawnRate{.rate = 40.0F})
        .init(Lifetime{.min = 2.0F, .max = 2.0F})
        .init(Velocity{.speed_min = 0.5F, .speed_max = 1.0F, .spread = 180.0F})
        .init(InitSize{.min = 0.2F, .max = 0.2F})
        .init(InitColor{.a = Color{1.0F, 0.0F, 0.0F, 1.0F}, .b = Color{1.0F, 0.2F, 0.0F, 1.0F},
                        .only = Scope{.sources = {0U}}})
        .init(InitColor{.a = Color{0.0F, 0.0F, 1.0F, 1.0F}, .b = Color{0.0F, 0.2F, 1.0F, 1.0F},
                        .only = Scope{.sources = {1U}}})
        .update(SizeOverLife{.keys = {{0.0F, 1.0F}, {1.0F, 0.25F}}, .only = Scope{.sources = {1U}}})
        .update(Heat{.start = 1.0F, .half_life = 0.5F, .glow = 2.0F, .only = Scope{.sources = {0U}}});
    return builder.build().value();
}

// The GPU's particles against the reference's: born alike exactly, set up
// and moved within a thousandth.
std::size_t apart_from(const EmitterState& g, const EmitterState& c) {
    AJFX_CHECK(g.alive() == c.alive() && g.spawned == c.spawned);
    AJFX_CHECK(g.spawn_index == c.spawn_index && g.age == c.age);
    std::size_t apart = 0U;
    for (std::size_t i = 0U; i < g.alive(); ++i) {
        const Vec3 d = g.position[i] - c.position[i];
        const float colour = std::fabs(g.color[i].r - c.color[i].r) + std::fabs(g.color[i].g - c.color[i].g) +
                             std::fabs(g.color[i].b - c.color[i].b) + std::fabs(g.color[i].a - c.color[i].a);
        if (std::sqrt(dot(d, d)) > 1.0e-3F || colour > 1.0e-4F || std::fabs(g.size[i] - c.size[i]) > 1.0e-6F) {
            ++apart;
        }
    }
    return apart;
}

void contacts_and_bodies_bear_the_same_particles_on_the_gpu() {
    const Effect effect = sparks_and_smoke();
    gpu::GpuSimulation gpu = make_gpu(effect);
    Simulation cpu = Simulation::create(effect, 5U, SimulationOptions{.rigid = g_solver.get()}).value();
    for (const std::uint64_t tick : {40U, 90U, 200U}) {
        gpu.advance_to(tick);
        cpu.advance_to(tick);
        AJFX_CHECK(same(gpu.read_emitter(0U).value(), cpu.emitter(0U)));
        for (const std::size_t emitter : {1U, 2U, 3U}) {
            const std::size_t apart = apart_from(gpu.read_emitter(emitter).value(), cpu.emitter(emitter));
            std::cout << "  step " << tick << ", " << effect.emitters[emitter].name << ": "
                      << cpu.emitter(emitter).alive() << " alive, " << apart << " apart from the reference\n";
            AJFX_CHECK(apart == 0U);
        }
    }
    AJFX_CHECK(cpu.emitter(1U).spawned > 0U && cpu.emitter(2U).spawned > 0U);
}

void hosted_steps_bear_particles_from_the_runners_frames() {
    const Effect effect = sparks_and_smoke();
    const gpu::KernelSet kernels = gpu::compile_kernels(effect, *g_compiler).value();
    gpu::GpuSimulation gpu =
        gpu::GpuSimulation::create(*g_device, effect, kernels, 5U, SimulationOptions{}, gpu::RigidHosting::Host)
            .value();
    RigidRunner runner{*g_solver};
    std::unique_ptr<RigidTrack> track = runner.track(effect, 5U, {});
    Simulation cpu = Simulation::create(effect, 5U, SimulationOptions{.rigid = g_solver.get()}).value();
    while (gpu.tick() < 120U) {
        std::shared_ptr<const RigidFrame> frame = track->frame(gpu.tick() + 1U);
        while (!frame) {
            AJFX_CHECK(!track->error().has_value());
            runner.wait_idle();
            frame = track->frame(gpu.tick() + 1U);
        }
        gpu.step(nullptr, *frame);
    }
    cpu.advance_to(120U);
    AJFX_CHECK(same(gpu.read_emitter(0U).value(), cpu.emitter(0U)));
    for (const std::size_t emitter : {1U, 2U, 3U}) {
        AJFX_CHECK(apart_from(gpu.read_emitter(emitter).value(), cpu.emitter(emitter)) == 0U);
    }
    std::cout << "  " << cpu.emitter(1U).spawned << " sparks and " << cpu.emitter(2U).spawned
              << " puffs, born of the runner's frames as of the reference's own bodies\n";
    AJFX_CHECK(cpu.emitter(1U).spawned > 0U);
}

void contact_spawns_rerun_exactly_on_the_gpu() {
    const Effect effect = sparks_and_smoke();
    gpu::GpuSimulation a = make_gpu(effect);
    gpu::GpuSimulation b = make_gpu(effect);
    a.advance_to(150U);
    b.advance_to(150U);
    for (const std::size_t emitter : {1U, 2U, 3U}) {
        AJFX_CHECK(a.read_particle_bytes(emitter).value() == b.read_particle_bytes(emitter).value());
    }
    // A checkpoint taken before the ball lands resumes to the same bits.
    gpu::GpuSimulation c = make_gpu(effect);
    c.advance_to(20U);
    Result<gpu::GpuCheckpoint> checkpoint = c.save();
    AJFX_CHECK(checkpoint.ok());
    c.advance_to(90U);
    c.restore(checkpoint.value());
    c.advance_to(150U);
    for (const std::size_t emitter : {1U, 2U, 3U}) {
        AJFX_CHECK(c.read_particle_bytes(emitter).value() == a.read_particle_bytes(emitter).value());
    }
}

// ---------------------------------------------------------------- the grid

gpu::GpuSimulation make_gpu_grid(const Effect& effect, const gpu::KernelSet& kernels, bool broadphase) {
    Result<gpu::GpuSimulation> simulation = gpu::GpuSimulation::create(
        *g_device, effect, kernels, 5U, SimulationOptions{.rigid = g_solver.get(), .body_broadphase = broadphase});
    if (!simulation) {
        std::cout << simulation.error().message << '\n';
    }
    AJFX_CHECK(simulation.ok());
    return std::move(simulation).value();
}

// `crates` boxes 0.5 across in rows 1.3 apart, held still; `flakes` of snow
// falling on them from just above; `chips` rigid chips (contact radius half
// their size) among them; and four balls rolling through the snow, stirring
// it with their wakes and pushing it aside.
Effect crates_snow_and_balls(std::uint32_t crates, std::uint32_t flakes, std::uint32_t chips) {
    const float d = 0.5F * std::sqrt(3.0F) * 1.5F;
    const auto columns = static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<float>(crates) * 1.25F)));
    const float width = static_cast<float>(columns) * d;
    const float depth = std::ceil(static_cast<float>(crates) / static_cast<float>(columns)) * d;
    EffectBuilder builder{"grid"};
    builder.emitter("crates")
        .capacity(crates)
        .spawn(SpawnPile{.count = crates,
                         .shape = PileShape::Box,
                         .width = width,
                         .depth = depth,
                         .spacing = 1.5F,
                         .jitter = 0.0F,
                         .release = 1000.0F})
        .init(Lifetime{.min = 1000.0F, .max = 1000.0F})
        .init(InitSize{.min = 0.5F, .max = 0.5F})
        .update(RigidPhysics{.shape = RigidShape::Box, .friction = 0.9F, .restitution = 0.0F});
    builder.emitter("balls")
        .capacity(4U)
        .seed(2U)
        .spawn(SpawnBurst{.count = 4U})
        .init(Lifetime{.min = 1000.0F, .max = 1000.0F})
        .init(ShapeLocation{.shape = Shape::Box, .center = Vec3{0.0F, 1.6F, 0.0F},
                            .size = Vec3{0.5F * width, 0.0F, 0.5F * depth}})
        .init(Velocity{.direction = Vec3{1.0F, 0.0F, 0.0F}, .speed_min = 2.0F, .speed_max = 4.0F, .spread = 90.0F})
        .init(InitSize{.min = 0.8F, .max = 0.8F})
        .update(RigidPhysics{.shape = RigidShape::Sphere, .allow_sleep = false});
    builder.emitter("snow")
        .capacity(flakes)
        .seed(3U)
        .spawn(SpawnBurst{.count = flakes})
        .init(Lifetime{.min = 100.0F, .max = 100.0F})
        .init(ShapeLocation{.shape = Shape::Box, .center = Vec3{0.0F, 1.5F, 0.0F}, .size = Vec3{width, 1.0F, depth}})
        .init(Velocity{.direction = Vec3{0.0F, -1.0F, 0.0F}, .speed_min = 0.5F, .speed_max = 1.0F, .spread = 20.0F})
        .init(InitSize{.min = 0.02F, .max = 0.02F})
        .update(Gravity{.acceleration = Vec3{0.0F, -2.0F, 0.0F}})
        .update(Wake{.emitter = "balls", .strength = 6.0F, .radius = 4.0F})
        .update(BodyCollision{.emitter = "crates", .restitution = 0.0F, .friction = 1.0F})
        .update(BodyCollision{.emitter = "balls", .restitution = 0.0F, .friction = 0.2F})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 1.0F});
    builder.emitter("chips")
        .capacity(chips)
        .seed(5U)
        .spawn(SpawnBurst{.count = chips})
        .init(Lifetime{.min = 100.0F, .max = 100.0F})
        .init(ShapeLocation{.shape = Shape::Box, .center = Vec3{0.0F, 1.5F, 0.0F}, .size = Vec3{width, 0.4F, depth}})
        .init(Velocity{.speed_min = 1.0F, .speed_max = 3.0F, .spread = 60.0F})
        .init(InitSize{.min = 0.06F, .max = 0.2F})
        .init(InitTumble{})
        .update(Gravity{})
        .update(RigidBody{.contact_scale = 0.5F})
        .update(BodyCollision{.emitter = "crates", .restitution = 0.2F, .friction = 0.6F})
        .update(PlaneCollision{.restitution = 0.2F, .friction = 0.6F});
    return builder.build().value();
}

// Steps to `tick` and waits for the device: milliseconds a step.
double timed_steps(gpu::GpuSimulation& simulation, std::uint64_t tick) {
    const std::uint64_t from = simulation.tick();
    const auto start = std::chrono::steady_clock::now();
    simulation.advance_to(tick);
    AJFX_CHECK(simulation.read_particle_bytes(2U).ok());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return ms / static_cast<double>(tick - from);
}

void the_gpu_grid_meets_what_every_body_in_turn_meets() {
    const Effect effect = crates_snow_and_balls(1200U, 8000U, 1000U);
    const gpu::KernelSet kernels = gpu::compile_kernels(effect, *g_compiler).value();
    gpu::GpuSimulation grid = make_gpu_grid(effect, kernels, true);
    gpu::GpuSimulation every = make_gpu_grid(effect, kernels, false);
    Simulation cpu = Simulation::create(effect, 5U, SimulationOptions{.rigid = g_solver.get()}).value();
    for (const std::uint64_t tick : {20U, 60U}) {
        grid.advance_to(tick);
        every.advance_to(tick);
        cpu.advance_to(tick);
        for (const std::size_t emitter : {2U, 3U}) {
            AJFX_CHECK(grid.read_particle_bytes(emitter).value() == every.read_particle_bytes(emitter).value());
            const std::size_t apart = apart_from(grid.read_emitter(emitter).value(), cpu.emitter(emitter));
            std::cout << "  step " << tick << ", " << effect.emitters[emitter].name << ": "
                      << cpu.emitter(emitter).alive() << " alive, the same bits through the grid and without, "
                      << apart << " apart from the reference\n";
            // Snow settles, so it agrees closely; chips tumbling off each
            // other's rounding are only reported.
            if (emitter == 2U) {
                AJFX_CHECK(apart == 0U);
            }
        }
    }
    // A rerun, and a checkpoint resumed, land on the same bits.
    gpu::GpuSimulation again = make_gpu_grid(effect, kernels, true);
    again.advance_to(30U);
    Result<gpu::GpuCheckpoint> checkpoint = again.save();
    AJFX_CHECK(checkpoint.ok());
    again.advance_to(50U);
    again.restore(checkpoint.value());
    again.advance_to(60U);
    for (const std::size_t emitter : {2U, 3U}) {
        AJFX_CHECK(again.read_particle_bytes(emitter).value() == grid.read_particle_bytes(emitter).value());
    }
}

// 2000 crates and 50000 flakes: what a step costs through the grid and
// meeting every body in turn. Printed, not checked: GPUs differ.
void the_gpu_grid_cost() {
    const Effect effect = crates_snow_and_balls(2000U, 50000U, 2000U);
    const gpu::KernelSet kernels = gpu::compile_kernels(effect, *g_compiler).value();
    gpu::GpuSimulation grid = make_gpu_grid(effect, kernels, true);
    gpu::GpuSimulation every = make_gpu_grid(effect, kernels, false);
    // The first steps compile pipelines and settle; time the ones after.
    grid.advance_to(5U);
    every.advance_to(5U);
    AJFX_CHECK(grid.read_particle_bytes(2U).ok() && every.read_particle_bytes(2U).ok());
    const double fast = timed_steps(grid, 35U);
    const double slow = timed_steps(every, 35U);
    std::cout << "  2000 crates, 4 balls, 50000 flakes and 2000 chips: " << fast << " ms a step through the grid, "
              << slow << " ms meeting every body in turn (rigid physics on the CPU included in both)\n";
    AJFX_CHECK(grid.read_particle_bytes(2U).value() == every.read_particle_bytes(2U).value());
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
        {"contacts_and_bodies_bear_the_same_particles_on_the_gpu",
         contacts_and_bodies_bear_the_same_particles_on_the_gpu},
        {"hosted_steps_bear_particles_from_the_runners_frames", hosted_steps_bear_particles_from_the_runners_frames},
        {"contact_spawns_rerun_exactly_on_the_gpu", contact_spawns_rerun_exactly_on_the_gpu},
        {"the_gpu_grid_meets_what_every_body_in_turn_meets", the_gpu_grid_meets_what_every_body_in_turn_meets},
        {"the_gpu_grid_cost", the_gpu_grid_cost},
    };
    const int result = ajfx::test::run(cases);
    g_solver.reset();
    g_compiler.reset();
    g_device.reset();
    return result;
}
