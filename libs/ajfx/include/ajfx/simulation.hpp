#pragma once

#include <ajfx/effect.hpp>
#include <ajfx/field.hpp>
#include <ajfx/math.hpp>
#include <ajfx/mesh.hpp>
#include <ajfx/parameters.hpp>
#include <ajfx/result.hpp>
#include <ajfx/rigid_solver.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

// The CPU reference simulator.
//
// It defines what an effect does. The GPU runtime is tested against it, and a
// host without a GPU path can run it directly. Its state after step N is a
// pure function of the effect, the instance seed and the parameter values at
// steps 0..N-1: the same build of the library produces the same bits on every
// machine, and reaching step N by seeking gives the same bits as stepping.
namespace ajfx {

// Supplies parameter values per step. Values must be a pure function of the
// step for seeking to reproduce playback; a host derives them from its
// timeline, never from wall-clock time. `values` arrives holding the
// defaults, in declaration order; results are clamped to each range.
class ParameterProvider {
public:
    virtual ~ParameterProvider() = default;
    virtual void values_at(std::uint64_t tick, std::span<ParameterValue> values) const = 0;
};

// Particles of one emitter, oldest first (ascending spawn index).
struct EmitterState final {
    std::vector<Vec3> position;
    std::vector<Vec3> velocity;
    std::vector<float> age;
    std::vector<float> lifetime;
    std::vector<Color> base_color;
    std::vector<Color> color;
    std::vector<float> base_size;
    std::vector<float> size;
    std::vector<float> rotation; // radians
    std::vector<Quat> orientation;
    std::vector<Vec3> angular_velocity; // radians per second, in the effect's space
    std::vector<float> spin;            // a sprite's, radians per second
    std::vector<std::uint32_t> spawn_index;
    // Rigid physics emitters only: each particle's slot in the emitter's
    // world, and the world's own state (see RigidWorld::save), which
    // Simulation::state brings up to date.
    std::vector<std::uint32_t> body;
    std::vector<std::uint8_t> solver_state;

    // Births so far, including refused ones; the low 32 bits of the count at
    // birth are a particle's spawn index.
    std::uint64_t spawned{0U};
    // Births refused because the emitter was full. Zero for any effect that
    // passed validation; reported rather than hidden if it ever is not.
    std::uint64_t refused{0U};
    // Fractional births carried between steps, one per SpawnRate module.
    std::vector<double> rate_carry;

    [[nodiscard]] std::size_t alive() const noexcept { return position.size(); }
    friend bool operator==(const EmitterState&, const EmitterState&) = default;
};

struct SimulationState final {
    std::uint64_t tick{0U};
    std::vector<EmitterState> emitters;
    friend bool operator==(const SimulationState&, const SimulationState&) = default;
};

// What a simulation needs beyond the effect, from its package and its host.
struct SimulationOptions final {
    // The distance fields its colliders use.
    FieldSet fields;
    // The meshes its rigid physics hulls wrap.
    MeshSet meshes;
    // Runs its rigid physics emitters (ajfx::rigid::make_jolt_solver); must
    // outlive the simulation. Effects without them need none.
    const RigidSolver* rigid{nullptr};
};

class Simulation final {
public:
    // Fails if the effect does not validate, or with MissingField when
    // `fields` lacks a distance field the effect collides with.
    [[nodiscard]] static Result<Simulation> create(Effect effect, std::uint32_t instance_seed = 0U,
                                                   FieldSet fields = {});
    // As above; also fails with MissingMesh when a rigid physics hull's mesh
    // is missing, RigidSolverMissing when the effect has rigid physics and no
    // solver was given, and RigidSolverFailure when the solver refuses a
    // world.
    [[nodiscard]] static Result<Simulation> create(Effect effect, std::uint32_t instance_seed,
                                                   SimulationOptions options);

    Simulation(Simulation&&) noexcept;
    Simulation& operator=(Simulation&&) noexcept;
    ~Simulation();

    [[nodiscard]] const Effect& effect() const noexcept { return effect_; }
    [[nodiscard]] std::uint64_t tick() const noexcept { return state_.tick; }
    [[nodiscard]] float step_seconds() const noexcept { return step_seconds_; }

    // Advances one step. Without a provider every parameter is its default.
    void step(const ParameterProvider* parameters = nullptr);
    // Steps until tick() == target; does nothing if target is behind.
    void advance_to(std::uint64_t target, const ParameterProvider* parameters = nullptr);
    // Back to step 0 with no particles.
    void reset();

    // The whole state, rigid physics solver state included: saving that
    // costs a little, so call it for checkpoints rather than every step.
    [[nodiscard]] const SimulationState& state() const;
    // One emitter's particles. A rigid physics emitter's `solver_state` may
    // be out of date here; state() brings it up to date.
    [[nodiscard]] const EmitterState& emitter(std::size_t index) const { return state_.emitters[index]; }
    // Replaces the state with one this simulation's state() returned (a
    // checkpoint), rebuilding rigid physics worlds from it.
    void restore(const SimulationState& state);

private:
    struct RigidEmitter final {
        RigidWorldDesc desc;
        std::unique_ptr<RigidWorld> world;
    };

    Simulation(Effect effect, std::uint32_t instance_seed, FieldSet fields);

    void step_emitter(std::size_t index, std::span<const ParameterValue> values);
    // A fresh world for each rigid physics emitter, holding the bodies its
    // particles in state_ have (none after a reset).
    void rebuild_worlds();

    Effect effect_;
    std::uint32_t instance_seed_{0U};
    float step_seconds_{0.0F};
    mutable SimulationState state_;
    std::vector<ParameterValue> values_;
    FieldSet fields_;
    const RigidSolver* solver_{nullptr};
    std::vector<RigidEmitter> rigid_;
    mutable bool solver_state_stale_{false};
};

// Snapshots taken every `interval` steps, so a seek resimulates at most one
// interval rather than from the start. A seek lands on exactly the state
// stepping would have produced.
class CheckpointRing final {
public:
    CheckpointRing(std::uint64_t interval, std::size_t capacity);

    // Moves `simulation` to `target`, forwards or backwards, from the nearest
    // usable checkpoint (or its current state when that is closer), and
    // records checkpoints passed on the way.
    void seek(Simulation& simulation, std::uint64_t target, const ParameterProvider* parameters = nullptr);

    [[nodiscard]] std::size_t size() const noexcept { return checkpoints_.size(); }
    // Must be called when the effect, instance seed or parameter history
    // changes; checkpoints from before describe a different timeline.
    void clear() noexcept { checkpoints_.clear(); }

private:
    void record(const Simulation& simulation);

    std::uint64_t interval_;
    std::size_t capacity_;
    std::vector<SimulationState> checkpoints_;
};

} // namespace ajfx
