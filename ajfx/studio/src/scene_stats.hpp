#pragma once
#include "compile.hpp"
#include "scene.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ajfx::studio {
// What the scene costs, counted for the artist before a budget is hit: the
// authored items, what they compile to, and how close each fixed limit is.
struct SceneStats final {
    std::size_t objects{};
    std::size_t systems{};
    std::size_t forces{};
    std::size_t groups{};
    std::size_t relationships{};
    // The most forces acting on any one system, of max_forces_per_system.
    std::size_t forces_on_busiest{};
    // From the compiled effect; 0 without one.
    std::uint32_t particle_capacity{};
    // The most particles alive at once through the loop, when it was traced
    // on the CPU; 0 when it was not.
    std::uint64_t peak_particles{};
    bool traced{false};
    std::uint32_t body_capacity{}; // loose bodies the world may hold at once
    std::size_t props{};           // fixed and animated things standing in it
    std::size_t body_kinds{};      // of ajfx::max_rigid_body_kinds
    std::size_t blasts{};          // of ajfx::max_blasts
    std::uint64_t state_bytes{};   // particle state at capacity
    // Smoke grids: how many of ajfx::max_volumes, their cells in all, the
    // most in one grid of ajfx::max_volume_cells, and the GPU memory their
    // working buffers take (about 108 bytes a cell).
    std::size_t grids{};
    std::uint64_t grid_cells{};
    std::uint64_t largest_grid_cells{};
    std::uint64_t grid_bytes{};
    // Limits within reach, each one short line: "Body kinds: 230 of 256".
    std::vector<std::string> near_limits;
    friend bool operator==(const SceneStats &, const SceneStats &) = default;
};

[[nodiscard]] SceneStats scene_stats(const Scene &scene, const Compiled *compiled,
                                     const std::map<std::uint32_t, std::uint64_t> *peak_alive);
// The statistics as short lines for a panel or the viewport, most useful first.
[[nodiscard]] std::vector<std::string> stat_lines(const SceneStats &stats);
} // namespace ajfx::studio
