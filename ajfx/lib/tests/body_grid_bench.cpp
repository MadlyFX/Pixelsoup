// Cost of particles meeting another emitter's bodies (BodyCollision) and
// following their wakes (Wake), through the grid and meeting every body in
// turn: 2000 crates held in rows and 50000 flakes of snow falling on them,
// beside a few rolling balls that stir the snow. Both runs must end in the
// same bits, which the bench checks. Not a test:
//   ajfx_body_grid_bench [steps]

#include <ajfx/builder.hpp>
#include <ajfx/rigid.hpp>
#include <ajfx/simulation.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {

using namespace ajfx;
using Clock = std::chrono::steady_clock;

constexpr std::uint32_t crate_count = 2000U;
constexpr std::uint32_t flake_count = 50000U;

Effect crates_and_snow() {
    // Crates 0.5 across, 1.3 apart (1.5 times their reach), 50 by 40.
    const float d = 0.5F * std::sqrt(3.0F) * 1.5F;
    const float width = 50.0F * d;
    const float depth = 40.0F * d;
    EffectBuilder builder{"bench"};
    builder.emitter("crates")
        .capacity(crate_count)
        .spawn(SpawnPile{.count = crate_count,
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
        .capacity(16U)
        .seed(2U)
        .spawn(SpawnBurst{.count = 16U})
        .init(Lifetime{.min = 1000.0F, .max = 1000.0F})
        .init(ShapeLocation{.shape = Shape::Box, .center = Vec3{0.0F, 2.0F, 0.0F}, .size = Vec3{width, 0.0F, depth}})
        .init(Velocity{.direction = Vec3{1.0F, 0.0F, 0.0F}, .speed_min = 2.0F, .speed_max = 4.0F, .spread = 90.0F})
        .init(InitSize{.min = 0.8F, .max = 0.8F})
        .update(RigidPhysics{.shape = RigidShape::Sphere, .allow_sleep = false});
    builder.emitter("snow")
        .capacity(flake_count)
        .seed(3U)
        .spawn(SpawnBurst{.count = flake_count})
        .init(Lifetime{.min = 100.0F, .max = 100.0F})
        .init(ShapeLocation{.shape = Shape::Box, .center = Vec3{0.0F, 2.0F, 0.0F}, .size = Vec3{width, 2.0F, depth}})
        .init(Velocity{.direction = Vec3{0.0F, -1.0F, 0.0F}, .speed_min = 0.5F, .speed_max = 1.0F, .spread = 20.0F})
        .init(InitSize{.min = 0.02F, .max = 0.02F})
        .update(Gravity{.acceleration = Vec3{0.0F, -2.0F, 0.0F}})
        .update(Wake{.emitter = "balls", .strength = 6.0F, .radius = 4.0F})
        .update(BodyCollision{.emitter = "crates", .restitution = 0.0F, .friction = 1.0F})
        .update(BodyCollision{.emitter = "balls", .restitution = 0.0F, .friction = 0.2F})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 1.0F});
    return builder.build().value();
}

double run(const char* name, const Effect& effect, const RigidSolver& solver, bool broadphase, std::uint64_t steps,
           SimulationState& out) {
    Simulation simulation =
        Simulation::create(effect, 1U, SimulationOptions{.rigid = &solver, .body_broadphase = broadphase}).value();
    double total = 0.0;
    double slowest = 0.0;
    for (std::uint64_t i = 0U; i < steps; ++i) {
        const auto start = Clock::now();
        simulation.step();
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        total += ms;
        slowest = ms > slowest ? ms : slowest;
    }
    out = simulation.state();
    const double mean = total / static_cast<double>(steps);
    std::printf("%-22s %zu crates, %zu balls, %zu flakes: step mean %.2f ms, max %.2f ms\n", name,
                simulation.emitter(0).alive(), simulation.emitter(1).alive(), simulation.emitter(2).alive(), mean,
                slowest);
    return mean;
}

} // namespace

int main(int argc, char** argv) {
    const std::uint64_t steps = argc > 1 ? static_cast<std::uint64_t>(std::atoi(argv[1])) : 10U;
    const std::unique_ptr<RigidSolver> solver = rigid::make_jolt_solver();
    const Effect effect = crates_and_snow();
    SimulationState grid;
    SimulationState every;
    const double fast = run("through the grid", effect, *solver, true, steps, grid);
    const double slow = run("every body in turn", effect, *solver, false, steps, every);
    const bool same = grid == every;
    std::printf("%s; the grid is %.1f times as fast\n", same ? "the same bits" : "DIFFERENT RESULTS",
                fast > 0.0 ? slow / fast : 0.0);
    return same ? 0 : 1;
}
