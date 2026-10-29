#pragma once

#include <ajfx/effect.hpp>
#include <ajfx/math.hpp>
#include <ajfx/parameters.hpp>
#include <ajfx/result.hpp>

#include <cstddef>
#include <cstdint>
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
    std::vector<std::uint32_t> spawn_index;

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

class Simulation final {
public:
    // Fails if the effect does not validate.
    [[nodiscard]] static Result<Simulation> create(Effect effect, std::uint32_t instance_seed = 0U);

    [[nodiscard]] const Effect& effect() const noexcept { return effect_; }
    [[nodiscard]] std::uint64_t tick() const noexcept { return state_.tick; }
    [[nodiscard]] float step_seconds() const noexcept { return step_seconds_; }

    // Advances one step. Without a provider every parameter is its default.
    void step(const ParameterProvider* parameters = nullptr);
    // Steps until tick() == target; does nothing if target is behind.
    void advance_to(std::uint64_t target, const ParameterProvider* parameters = nullptr);
    // Back to step 0 with no particles.
    void reset();

    [[nodiscard]] const SimulationState& state() const noexcept { return state_; }
    [[nodiscard]] const EmitterState& emitter(std::size_t index) const { return state_.emitters[index]; }
    // Replaces the state, e.g. with a checkpoint taken from this simulation.
    void restore(const SimulationState& state) { state_ = state; }

private:
    Simulation(Effect effect, std::uint32_t instance_seed);

    void step_emitter(std::size_t index, std::span<const ParameterValue> values);

    Effect effect_;
    std::uint32_t instance_seed_{0U};
    float step_seconds_{0.0F};
    SimulationState state_;
    std::vector<ParameterValue> values_;
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
