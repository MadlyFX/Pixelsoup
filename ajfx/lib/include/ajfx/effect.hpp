#pragma once

#include <ajfx/modules.hpp>
#include <ajfx/parameters.hpp>
#include <ajfx/result.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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
    // The effect's blasts, each set once and named by every module that
    // answers it (Shockwave, and Break::blast_threshold through it).
    std::vector<Blast> blasts;

    friend bool operator==(const Effect&, const Effect&) = default;
};

// The blast called `name`, if the effect has one.
[[nodiscard]] const Blast* find_blast(const Effect& effect, std::string_view name) noexcept;

// A particle's spawn index says which spawn module bore it and which of that
// module's births it was: the module's place in the emitter's spawn stack in
// the top 8 bits, its own count of births in the low 24. Each module counts
// its own (EmitterState::spawn_counts), so adding a spawn module after the
// others leaves their particles' random draws, and so the particles
// themselves, exactly as they were. The pieces broken bodies become count
// as one more source of their own, `piece_spawn_source`.
// - **Unique:** no two particles of an emitter alive together share one,
//   unless one module has made 16,777,216 births while the other lived.
// - **Order:** within one module's particles it rises with birth; between
//   modules it says nothing of age. Birth order is the particles' order in
//   the emitter (EmitterState), not their spawn indices.
inline constexpr std::uint32_t spawn_count_bits = 24U;
inline constexpr std::uint32_t spawn_count_mask = (1U << spawn_count_bits) - 1U;
// Spawn modules one emitter may hold: every source but the pieces'.
inline constexpr std::uint32_t max_spawn_modules = 255U;
inline constexpr std::uint32_t piece_spawn_source = 255U;

[[nodiscard]] constexpr std::uint32_t spawn_index_of(std::uint32_t source, std::uint64_t count) noexcept {
    return (source << spawn_count_bits) | (static_cast<std::uint32_t>(count) & spawn_count_mask);
}

// Which spawn module bore the particle (its place in the spawn stack), or
// piece_spawn_source for a piece of a broken body.
[[nodiscard]] constexpr std::uint32_t spawn_source(std::uint32_t spawn_index) noexcept {
    return spawn_index >> spawn_count_bits;
}

// Whether a particle with `spawn_index`, a body of `kind` (0 for a particle
// that is no body), is in `scope` (Scope): every list that is not empty
// names it.
[[nodiscard]] inline bool in_scope(const Scope& scope, std::uint32_t spawn_index, std::uint32_t kind) noexcept {
    const auto names = [](const std::vector<std::uint32_t>& list, std::uint32_t value) {
        for (const std::uint32_t entry : list) {
            if (entry == value) {
                return true;
            }
        }
        return false;
    };
    return (scope.sources.empty() || names(scope.sources, spawn_source(spawn_index))) &&
           (scope.kinds.empty() || names(scope.kinds, kind));
}

// Contacts one rigid physics emitter's world reports a step for what the
// effect itself reads of them (Break, SpawnFromContacts), at least: the
// hardest this many, or the emitter's capacity if that is more. Past it the
// softest are dropped and counted (RigidWorld::contacts_dropped), never the
// hardest.
inline constexpr std::uint32_t max_rigid_contacts = 4096U;

// The slowest contact anything in `effect` reads of emitter `index`'s
// contacts: its own Break modules' thresholds and the min_speed of every
// SpawnFromContacts naming it. None when nothing reads them. The world
// collects contacts down to this whatever its host asked for.
[[nodiscard]] std::optional<float> contact_floor(const Effect& effect, std::size_t index);

// Checks names, ranges, parameter bindings, module ids and that each
// emitter's worst case fits its capacity. Builders and package readers call
// this; an Effect that passed it is safe to simulate.
[[nodiscard]] Result<bool> validate(const Effect& effect);

// The most particles the emitter can have alive at once with every bound
// parameter anywhere in its range. validate() refuses an emitter whose bound
// exceeds its capacity. Returns UINT64_MAX when an input is unbounded.
[[nodiscard]] std::uint64_t particle_bound(const Effect& effect, const Emitter& emitter);

// Which particles carry the emitter's lights: those whose spawn index is a
// multiple of this. It spreads `light_count` lights over the most particles
// the emitter can have alive (particle_bound, else its capacity), so a light
// stays with its particle for life and at most `light_count` are lit at once.
// Spawn indices count per spawn module (spawn_index_of), so an emitter with
// several picks within each module's births and may light up to one more for
// each module beyond the first. One when the emitter has no lights.
[[nodiscard]] std::uint32_t light_spacing(const Effect& effect, const Emitter& emitter);

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
    Orientation = 1U << 7U,
    AngularVelocity = 1U << 8U,
    Spin = 1U << 9U,
};

[[nodiscard]] std::uint32_t required_attributes(const Emitter& emitter);

// The emitter's RigidPhysics module, if it has one: its particles are then
// a rigid-body engine's bodies, simulated on the CPU (see rigid_solver.hpp).
[[nodiscard]] const RigidPhysics* rigid_physics(const Emitter& emitter) noexcept;

// Whether the emitter draws a model per body kind: it draws meshes and at
// least one of its body kinds names a model of its own (RigidBodyKind::draw).
// Its records then carry each body's kind, and a renderer draws every body by
// the model of the kind it is.
[[nodiscard]] bool models_by_kind(const Emitter& emitter) noexcept;
[[nodiscard]] bool has_rigid_physics(const Effect& effect) noexcept;
// The effect's rigid physics emitters alone, in order. Each emitter's
// randomness is its own, so they simulate there exactly as within the whole.
[[nodiscard]] Effect rigid_physics_part(const Effect& effect);

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
