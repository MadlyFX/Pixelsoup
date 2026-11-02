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
#include <map>
#include <optional>
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

class Cache;

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
    // world, which kind of body it is (0 is the emitter's own, the rest are
    // RigidPhysics::bodies in order), and the world's own state (see
    // RigidWorld::save), which Simulation::state brings up to date.
    std::vector<std::uint32_t> body;
    std::vector<std::uint32_t> body_kind;
    // Which bodies are held to each other (Glue), by slot, in the order
    // they were joined. A join goes when either of its bodies does, or when
    // a step pulls it apart.
    std::vector<RigidJoin> joins;
    std::vector<std::uint8_t> solver_state;
    // Rigid physics emitters only: where each of the emitter's props stands,
    // in the order RigidPhysics lists them, so a host can draw them. A prop
    // is not a particle: it is here from the first step and never dies.
    std::vector<RigidBodyState> props;

    // Births so far, including refused ones and the pieces broken bodies
    // became.
    std::uint64_t spawned{0U};
    // Births so far by where they came from: one count per spawn module, in
    // stack order, then the pieces broken bodies became. A particle's spawn
    // index is spawn_index_of(its source, that count at its birth), so each
    // module's particles draw the same random values whatever the other
    // modules do. Refused births count, as they do in `spawned`.
    std::vector<std::uint64_t> spawn_counts;
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

// Contacts a host wants its rigid physics emitters to report, step by step
// (Simulation::contacts). Finding them costs, so without this nothing is
// collected and nothing is reported.
struct ContactOptions final {
    // Only pairs closing at least this fast (units per second) are reported.
    // This is what bounds how many there are: bodies at rest close at
    // nothing, so a settled pile reports none however large it is. Leaving
    // it at zero asks for every touch, resting ones included.
    float min_speed{0.0F};
    // At most this many per emitter per step; the rest are counted
    // (Simulation::contacts_dropped) rather than quietly left out.
    std::uint32_t limit{256U};
};

// One body of a rigid physics emitter, as another emitter's particles meet
// it (BodyCollision) or are stirred by it (Wake): the ball or box its size
// describes, turned as the body is, moving as it is. Gathered once a step,
// in body order, by `body_colliders`.
struct BodyCollider final {
    Vec3 position{};
    Quat orientation{};
    // Half the body's size: a box's half extent on every axis, a ball's
    // radius.
    float half{0.5F};
    bool box{false};
    Vec3 velocity{};
    // Which of its emitter's spawn modules bore it and what kind of body it
    // is, for a Wake's `bodies` scope.
    std::uint32_t spawn_index{0U};
    std::uint32_t kind{0U};

    friend bool operator==(const BodyCollider&, const BodyCollider&) = default;
};

// The live bodies of `state`, shaped as `physics` describes their kinds, for
// an emitter whose particles collide with them (BodyCollision).
[[nodiscard]] std::vector<BodyCollider> body_colliders(const RigidPhysics& physics, const EmitterState& state,
                                                       Geometry geometry);

// What a simulation needs beyond the effect, from its package and its host.
struct SimulationOptions final {
    // The distance fields its colliders use.
    FieldSet fields;
    // The meshes its rigid physics hulls wrap.
    MeshSet meshes;
    // Runs its rigid physics emitters (ajfx::rigid::make_jolt_solver); must
    // outlive the simulation. Effects without them need none.
    const RigidSolver* rigid{nullptr};
    // Asks its rigid physics emitters to report what their bodies hit.
    // Whatever is asked, a world also collects what the effect itself reads
    // of its contacts (contact_floor), and the effect reads the same
    // contacts either way.
    std::optional<ContactOptions> contacts;
    // For an effect that is a part of a larger one (rigid_physics_part): per
    // emitter of the part, the slowest contact the rest of the larger effect
    // reads of it (contact_floor there), so the part's worlds collect what
    // the whole needs. RigidRunner and GpuSimulation fill it; a whole effect
    // leaves it empty and works it out itself.
    std::vector<std::optional<float>> contact_floors;
    // Plays the emitters this cache holds rather than simulating them
    // (cache.hpp), from a Cache opened for this effect. A cached rigid
    // emitter needs no solver; live emitters step as they would beside the
    // simulated ones, meeting the same bodies. Bake-time parameters take the
    // values the cache was baked with, whatever a provider says, since those
    // are what the cache is; playback-time ones follow the provider.
    std::shared_ptr<const Cache> cache;
    // Whether particles find the bodies they meet (BodyCollision) and are
    // stirred by (Wake) through a grid the bodies are sorted into each step.
    // Off, each particle looks at every body in turn: the same result to
    // the bit, which is what this switch is for proving, at the cost of
    // every particle times every body.
    bool body_broadphase{true};
};

// The parameters' values at `tick`: defaults, then `provider`'s, clamped to
// each range, in declaration order -- what a step sees.
[[nodiscard]] std::vector<ParameterValue> parameter_values(const Effect& effect, const ParameterProvider* provider,
                                                           std::uint64_t tick);

// What a renderer multiplies an emitter's particles by as it draws them: the
// renderer's draw inputs (SpriteRenderer::tint and the rest) resolved
// against parameter values. The same for simulated and cached particles,
// since it applies after both.
struct DrawAdjustment final {
    // Colour by r, g and b (tint times brightness), alpha by a (tint's alpha
    // times opacity).
    Color color{};
    float size{1.0F};

    friend bool operator==(const DrawAdjustment&, const DrawAdjustment&) = default;
};

[[nodiscard]] DrawAdjustment draw_adjustment(const Effect& effect, std::size_t emitter,
                                             std::span<const ParameterValue> values);
// Applies `adjustment` to a copy of particles about to be drawn: a host that
// draws from the CPU's state calls it on that copy, never on the
// simulation's own, which must stay a pure function of the step.
void adjust_for_drawing(const DrawAdjustment& adjustment, EmitterState& particles);

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
    // Back to step 0, holding only the bodies of its piles (SpawnPile), set
    // up with the parameters' values at step 0. Creating a simulation resets
    // it with the defaults; if a provider gives other values at step 0, the
    // first step sets the piles up again with those, so stepping from
    // creation and seeking agree.
    void reset(const ParameterProvider* parameters = nullptr);
    // At step 0, sets the piles up again if `parameters` give other values
    // there than they were set up with; elsewhere does nothing.
    void prepare(const ParameterProvider* parameters);

    // The whole state, rigid physics solver state included: saving that
    // costs a little, so call it for checkpoints rather than every step.
    [[nodiscard]] const SimulationState& state() const;
    // One emitter's particles. A rigid physics emitter's `solver_state` may
    // be out of date here; state() brings it up to date.
    [[nodiscard]] const EmitterState& emitter(std::size_t index) const { return state_.emitters[index]; }

    // What emitter `index`'s bodies hit in the last step, in
    // `contact_before`'s order, which is a pure function of the step: a
    // seek that lands on a step reports what stepping through it would
    // have. Empty for an emitter without rigid physics, when neither
    // SimulationOptions nor the effect itself (contact_floor) asks for
    // contacts, and for a step that has not run yet (a reset or a restore
    // reports none until the next step). Good until the next step.
    [[nodiscard]] std::span<const RigidContact> contacts(std::size_t index) const;
    // How many more that step found and did not report, the limit having
    // been full: the softest go first (contact_harder).
    [[nodiscard]] std::uint32_t contacts_dropped(std::size_t index) const;
    // Replaces the state with one this simulation's state() returned (a
    // checkpoint), rebuilding rigid physics worlds from it.
    void restore(const SimulationState& state);

    // The cache it plays, if any (SimulationOptions::cache).
    [[nodiscard]] const Cache* cache() const noexcept { return cache_.get(); }
    // Whether the step reached is past the end of its cache, where cached
    // emitters hold their last frame rather than going on.
    [[nodiscard]] bool beyond_cache() const noexcept;
    // The cache's latest checkpoint at or before `tick`, if it has one: a
    // far seek restores it rather than stepping there.
    [[nodiscard]] std::optional<SimulationState> cached_checkpoint(std::uint64_t tick) const;

private:
    struct RigidEmitter final {
        RigidWorldDesc desc;
        std::unique_ptr<RigidWorld> world;
        // The slowest contact the effect reads of this world, if it reads
        // any, and what it read of the last step (detail::effect_contacts).
        std::optional<float> floor;
        std::vector<RigidContact> read;
    };

    Simulation(Effect effect, std::uint32_t instance_seed, FieldSet fields);

    void step_emitter(std::size_t index, std::span<const ParameterValue> values);
    // The step's values: resolve_values, then the cache's bake-time values.
    void resolve(const ParameterProvider* parameters, std::uint64_t tick, std::vector<ParameterValue>& values) const;
    // Whether emitter `index` is read from the cache rather than simulated.
    [[nodiscard]] bool cached(std::size_t index) const noexcept;
    // Every cached emitter as the cache has it at the current tick.
    void load_cached();
    // A fresh world for each rigid physics emitter, holding the bodies its
    // particles in state_ have (none after a reset).
    void rebuild_worlds();
    // Step 0 with `values`: no particles but the piles'.
    void start(std::span<const ParameterValue> values);
    void prespawn(std::size_t index, std::span<const ParameterValue> values);
    // Whether the particle with `spawn_index` in emitter `index` is a pile
    // body still held at the current tick.
    [[nodiscard]] bool held(std::size_t index, std::uint32_t spawn_index) const;

    Effect effect_;
    std::uint32_t instance_seed_{0U};
    float step_seconds_{0.0F};
    mutable SimulationState state_;
    std::vector<ParameterValue> values_;
    FieldSet fields_;
    const RigidSolver* solver_{nullptr};
    // SimulationOptions::body_broadphase.
    bool body_broadphase_{true};
    std::vector<RigidEmitter> rigid_;
    mutable bool solver_state_stale_{false};
    bool has_piles_{false};
    // Props, like piles, stand in their worlds before step 0, so creating
    // the simulation has to start it once the worlds exist.
    bool has_props_{false};
    // The values step 0's piles were set up with.
    std::vector<ParameterValue> pile_values_;
    std::shared_ptr<const Cache> cache_;
    // Per parameter, the value the cache pins it to (bake-time ones only).
    std::vector<std::optional<ParameterValue>> pinned_;
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
