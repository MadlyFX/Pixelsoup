#pragma once

#include <ajfx/modules.hpp>
#include <ajfx/parameters.hpp>
#include <ajfx/result.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace ajfx {

// Largest capacity one emitter may declare.
inline constexpr std::uint32_t max_emitter_capacity = 1U << 24U;
inline constexpr std::uint32_t max_simulation_rate = 1000U;

struct Emitter final {
    std::string name;
    // Mixed with the instance seed a host supplies, so two placements of one
    // effect differ while each stays reproducible.
    std::uint32_t seed{0U};
    // Particles alive at once. Fixed: an emitter never grows while it runs.
    std::uint32_t capacity{1024U};
    std::vector<SpawnModule> spawn;
    std::vector<InitModule> init;
    std::vector<UpdateModule> update;
    SpriteRenderer renderer{};

    friend bool operator==(const Emitter&, const Emitter&) = default;
};

struct Effect final {
    std::string name;
    // Fixed simulation steps per second. A host maps its own time onto steps
    // with `tick_at`; the simulation never sees a variable delta.
    std::uint32_t simulation_rate{60U};
    std::vector<ParameterDesc> parameters;
    std::vector<Emitter> emitters;

    friend bool operator==(const Effect&, const Effect&) = default;
};

// Checks names, ranges, parameter bindings, module ids and that each
// emitter's worst case fits its capacity. Builders and package readers call
// this; an Effect that passed it is safe to simulate.
[[nodiscard]] Result<bool> validate(const Effect& effect);

// The most particles the emitter can have alive at once with every bound
// parameter anywhere in its range. validate() refuses an emitter whose bound
// exceeds its capacity. Returns UINT64_MAX when an input is unbounded.
[[nodiscard]] std::uint64_t particle_bound(const Effect& effect, const Emitter& emitter);

// Per-particle attributes an emitter's modules read or write. Position, age
// and lifetime are always present; the GPU runtime allocates only the rest
// that appear here.
enum class Attribute : std::uint32_t {
    Position = 1U << 0U,
    Velocity = 1U << 1U,
    Age = 1U << 2U,
    Lifetime = 1U << 3U,
    Color = 1U << 4U,
    Size = 1U << 5U,
    Rotation = 1U << 6U,
};

[[nodiscard]] std::uint32_t required_attributes(const Emitter& emitter);

[[nodiscard]] constexpr bool has_attribute(std::uint32_t mask, Attribute attribute) noexcept {
    return (mask & static_cast<std::uint32_t>(attribute)) != 0U;
}

// The simulation step reached at a host frame: floor(frame * sim_rate *
// rate_den / rate_num) for a host running at rate_num / rate_den frames per
// second, in exact integer arithmetic so every machine lands on the same step.
[[nodiscard]] constexpr std::uint64_t tick_at(
    std::uint64_t frame, std::uint32_t rate_num, std::uint32_t rate_den, std::uint32_t simulation_rate) noexcept {
    const std::uint64_t scale = static_cast<std::uint64_t>(rate_den) * simulation_rate;
    return (frame / rate_num) * scale + ((frame % rate_num) * scale) / rate_num;
}

} // namespace ajfx
