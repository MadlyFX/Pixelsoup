#pragma once

#include <ajfx/effect.hpp>
#include <ajfx/math.hpp>
#include <ajfx/simulation.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// The CPU reference of a grid volume (Volume): what particles deposit into
// it and sample from it, and the step that moves it on. Every function here
// has an HLSL twin in shaders/ajfx_volume.hlsli and ajfx_volume.hlsl that
// runs the same operations in the same order, so the two grids agree to the
// bit (vorticity confinement aside, whose square roots and divisions round as
// the GPU rounds them).
namespace ajfx::detail {

// What one step of a grid is a function of, worked out once from the volume
// and the step: the GPU is handed these very floats, so it works with the
// same products and reciprocals the reference does.
struct VolumeStep final {
    std::uint32_t nx{0U};
    std::uint32_t ny{0U};
    std::uint32_t nz{0U};
    std::uint32_t cells{0U};
    Vec3 origin{};
    float inv_cell{1.0F};
    float buoyancy_dt{0.0F};
    float weight_dt{0.0F};
    // vorticity * cell * dt
    float confinement{0.0F};
    float inv_two_cells{0.5F};
    float cell_squared{1.0F};
    // dt / cell: how many cells a unit of velocity carries in a step.
    float advect_scale{0.0F};
    float keep_density{1.0F};
    float keep_temperature{1.0F};
    float keep_velocity{1.0F};
    std::uint32_t pressure_iterations{0U};
};

[[nodiscard]] VolumeStep volume_step(const Volume& volume, float dt);

// An empty grid of the volume's size.
[[nodiscard]] VolumeState empty_volume(const Volume& volume);

// This step's deposits, five fixed-point sums a cell: density, temperature,
// then the three parts of the momentum (volume_deposit_quantum).
struct VolumeDeposits final {
    std::vector<std::int64_t> sums;

    void clear() noexcept;
    [[nodiscard]] bool any() const noexcept { return deposited; }
    bool deposited{false};
};

// The cell `p` is in, or none outside the grid.
[[nodiscard]] std::optional<std::uint32_t> volume_cell(const VolumeStep& grid, Vec3 p) noexcept;

// A deposit as the integer it is summed as: rounded to the nearest quantum,
// clamped to what an int32 holds.
[[nodiscard]] std::int32_t quantize_deposit(float amount) noexcept;
// A cell's sum back as a float, before the quantum: the two 32-bit words of
// the sum converted and added as the GPU converts and adds them.
[[nodiscard]] float decode_deposit(std::int64_t sum) noexcept;

void deposit(VolumeDeposits& deposits, std::uint32_t cell, float density, float temperature, Vec3 momentum);

// The velocity the grid holds at `p` (the effect's space): trilinear between
// the centres of the cells around it, clamped to the grid's edge; none when
// `p` is outside the grid's box.
[[nodiscard]] std::optional<Vec3> sample_volume_velocity(const VolumeStep& grid, const VolumeState& state, Vec3 p);

// Buffers a step writes into, kept between steps so a step allocates
// nothing; none of it is state.
struct VolumeScratch final {
    VolumeState next;
    std::vector<Vec3> curl;
    std::vector<float> magnitude;
    std::vector<float> divergence;
    std::vector<float> pressure[2];
};

// One step of the grid: inject, buoyancy, vorticity confinement, advection,
// pressure projection, dissipation (see Volume). `state` is the grid before
// and after; `deposits` are the step's, cleared on the way out.
void step_volume(const VolumeStep& grid, VolumeDeposits& deposits, VolumeState& state, VolumeScratch& scratch);

} // namespace ajfx::detail
