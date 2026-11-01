#pragma once

#include <ajfx/rigid_solver.hpp>

#include <cstdint>
#include <memory>

// ajfx::rigid: rigid physics emitters run by Jolt Physics, built for
// cross-platform determinism (see third_party/jolt/README.md). Link
// ajfx::rigid and hand the solver to Simulation or GpuSimulation through
// SimulationOptions::rigid. No Jolt header is visible here.
namespace ajfx::rigid {

struct JoltOptions final {
    // Threads that step each world, the calling thread included. Results are
    // the same at any count.
    std::uint32_t threads{1U};
};

// A solver whose worlds step on its own thread pool. Its worlds must be
// stepped from one thread at a time, and it must outlive them.
[[nodiscard]] std::unique_ptr<RigidSolver> make_jolt_solver(JoltOptions options = {});

} // namespace ajfx::rigid
