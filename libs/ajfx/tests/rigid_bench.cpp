// Cost of rigid physics emitters: step time and checkpoint size for the
// rubble preset and for thousands of chips, plus a digest of the final state
// so two builds or two machines can be compared bit for bit. Not a test:
//   ajfx_rigid_bench [threads]

#include <ajfx/builder.hpp>
#include <ajfx/presets.hpp>
#include <ajfx/rigid.hpp>
#include <ajfx/simulation.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

using namespace ajfx;
using Clock = std::chrono::steady_clock;

std::uint64_t digest(const SimulationState& state) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    const auto add = [&hash](const void* data, std::size_t bytes) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0U; i < bytes; ++i) {
            hash = (hash ^ p[i]) * 0x100000001b3ULL;
        }
    };
    for (const EmitterState& s : state.emitters) {
        add(s.position.data(), s.position.size() * sizeof(Vec3));
        add(s.orientation.data(), s.orientation.size() * sizeof(Quat));
        add(s.solver_state.data(), s.solver_state.size());
    }
    return hash;
}

// `count` chips flung up from a disc onto the floor, all at once.
Effect heap(std::uint32_t count) {
    EffectBuilder builder{"heap"};
    builder.emitter("chips")
        .capacity(count)
        .spawn(SpawnBurst{.count = count})
        .init(Lifetime{.min = 100.0F, .max = 100.0F})
        .init(ShapeLocation{.shape = Shape::Box, .center = Vec3{0.0F, 2.0F, 0.0F}, .size = Vec3{4.0F, 3.0F, 4.0F}})
        .init(Velocity{.speed_min = 1.0F, .speed_max = 5.0F, .spread = 60.0F})
        .init(InitSize{.min = 0.08F, .max = 0.2F})
        .init(InitTumble{.speed_min = 90.0F, .speed_max = 720.0F})
        .update(Gravity{})
        .update(RigidPhysics{.density = 2400.0F, .friction = 0.7F, .restitution = 0.15F})
        .update(PlaneCollision{.restitution = 0.1F, .friction = 0.8F})
        .renderer(SpriteRenderer{.geometry = Geometry::Mesh, .mesh = "chip"});
    return builder.build().value();
}

void run(const char* name, const Effect& effect, const RigidSolver& solver, std::uint64_t steps) {
    Simulation simulation =
        Simulation::create(effect, 1U, SimulationOptions{.meshes = presets::meshes(), .rigid = &solver}).value();
    std::vector<double> ms;
    for (std::uint64_t i = 0U; i < steps; ++i) {
        const auto start = Clock::now();
        simulation.step();
        ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    const auto save = Clock::now();
    const SimulationState& state = simulation.state();
    const double save_ms = std::chrono::duration<double, std::milli>(Clock::now() - save).count();
    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    double total = 0.0;
    for (const double v : ms) {
        total += v;
    }
    std::printf("%-6s %5zu bodies: step mean %.3f ms, p95 %.3f ms, max %.3f ms; checkpoint %zu bytes, saved in %.2f ms; "
                "digest 0x%016llx\n",
                name, simulation.emitter(0).alive(), total / static_cast<double>(ms.size()),
                sorted[sorted.size() * 95U / 100U], sorted.back(), state.emitters[0].solver_state.size(), save_ms,
                static_cast<unsigned long long>(digest(state)));
}

} // namespace

int main(int argc, char** argv) {
    const std::uint32_t threads = argc > 1 ? static_cast<std::uint32_t>(std::atoi(argv[1])) : 1U;
    const std::unique_ptr<RigidSolver> solver = rigid::make_jolt_solver(rigid::JoltOptions{.threads = threads});
    std::printf("threads %u\n", threads);
    run("rubble", presets::rubble(), *solver, 360U);
    run("heap", heap(4000U), *solver, 360U);
    return 0;
}
