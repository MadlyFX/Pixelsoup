#pragma once

#include <ajfx/effect.hpp>
#include <ajfx/field.hpp>
#include <ajfx/mesh.hpp>
#include <ajfx/parameters.hpp>
#include <ajfx/result.hpp>
#include <ajfx/simulation.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Caching: a simulation worked out once, at whatever cost, and played back.
//
// Determinism is what makes a cache trustworthy: an effect's state at a step
// is a pure function of the effect, the instance seed and its bake-time
// parameter values, so a cache taken from those is the simulation, and a
// cache of anything else is not. The failure to watch is therefore
// staleness, and a cache carries its identity and is refused, with a reason,
// by anything it does not belong to.
//
// - **State on disk** (`write_state`, `read_state`): the whole
//   SimulationState, rigid solver state included, versioned and checked as
//   the package format is.
// - **The cache file** (`bake`, `Cache`): a sidecar beside its package
//   (`effect.ajfx` and `effect.ajfxcache`), never inside it. A package is an
//   asset identified by its content hash; a cache is taken per placement
//   seed and per bake-time value, re-taken without re-exporting, and taken
//   by Studio or by a farm alike, none of which should change that hash. It
//   holds, per emitter, nothing (the emitter stays
//   live), every step's bodies (a rigid layer) or quantised particles (a
//   particle layer), and whole-state checkpoints so that a far seek of the
//   live emitters is a read and at most one interval of stepping.
// - **Playback**: SimulationOptions::cache. A cached emitter is read rather
//   than stepped -- a cached rigid layer needs no solver at all -- and live
//   emitters around it step exactly as they would beside the simulated one,
//   colliding with the same bodies to the bit.
namespace ajfx {

// Version of the saved simulation state (write_state) this library reads and
// writes.
inline constexpr std::uint32_t state_format_version = 2U;
// Version of the cache file this library reads and writes.
inline constexpr std::uint32_t cache_format_version = 2U;

// The whole state as bytes: deterministic, so equal states give equal bytes.
[[nodiscard]] std::vector<std::uint8_t> write_state(const SimulationState& state);
// Fails with UnsupportedVersion for another version and MalformedCache for
// anything truncated, damaged or inconsistent.
[[nodiscard]] Result<SimulationState> read_state(std::span<const std::uint8_t> bytes);

// What emitter `index`'s simulation is a pure function of, as a hash: its
// modules and seed, the parts of its renderer the simulation reads (its
// geometry and mesh), the effect's simulation rate, the declarations of the
// bake-time parameters it reads, the contents of the meshes and fields it
// names (found in `meshes` and `fields`) and, for an emitter that collides
// with another's bodies or bears particles from them (SpawnFromContacts,
// SpawnFromBodies), that emitter's digest too. Its looks -- texture,
// blending, lights, draw inputs -- are not in it, nor is any other emitter.
[[nodiscard]] std::uint64_t emitter_digest(const Effect& effect, std::size_t index, const MeshSet& meshes = {},
                                           const FieldSet& fields = {});
// Every emitter's digest and every bake-time parameter's declaration
// together: what a whole-state checkpoint is a pure function of, beside the
// values.
[[nodiscard]] std::uint64_t simulation_digest(const Effect& effect, const MeshSet& meshes = {},
                                              const FieldSet& fields = {});

// A bake-time parameter's value as a cache was taken with it.
struct BakeValue final {
    std::string name;
    ParameterValue value{};

    friend bool operator==(const BakeValue&, const BakeValue&) = default;
};

// What a cache holds of one emitter.
enum class CacheMode : std::uint8_t {
    // Nothing: the emitter is simulated wherever the cache is played.
    Live,
    // Every step's bodies, exactly: pose, age, and once per body its kind,
    // size, life and colour. Only a rigid physics emitter. Its particles are
    // what other emitters collide with (BodyCollision), so they are kept to
    // the bit; they carry no velocities.
    Rigid,
    // A keyframe of every particle exactly at each keyframe interval, and
    // between them positions quantised to `position_step` (never more than
    // half a step out, and not drifting), with each newborn exact and
    // colour, size and age worked out again exactly. Velocity is kept, at
    // half precision, only where the renderer faces it; rotation and
    // orientation at 16 bits. Nothing reads another emitter's particles, so
    // nothing downstream depends on what was quantised.
    Particles,
};

[[nodiscard]] std::string_view cache_mode_name(CacheMode mode) noexcept;

// One emitter a cache holds, by name, and the digest it was baked from.
struct CachedEmitter final {
    std::string emitter;
    CacheMode mode{CacheMode::Rigid};
    std::uint64_t digest{0U};

    friend bool operator==(const CachedEmitter&, const CachedEmitter&) = default;
};

// What a cache belongs to: the placement's instance seed, the simulation
// rate, each cached emitter's digest (emitter_digest), and the value of every
// bake-time parameter the cached emitters read, in declaration order.
//
// It is keyed by what the cached emitters' frames depend on, not by the
// package's bytes. A package changes when a texture, a kernel or a live layer
// does, and none of those changes a cached layer's frames: keyed by the
// package, every such edit would throw away a bake that is still exactly
// right. Two things follow from it:
// - Playback-time parameters never reach the simulation, so any value of
//   them plays any cache.
// - A bake-time parameter only live emitters read is theirs to follow: a
//   live layer beside a cached one keeps answering its own controls.
struct CacheIdentity final {
    std::uint32_t instance_seed{0U};
    std::uint32_t simulation_rate{0U};
    std::vector<CachedEmitter> emitters;
    std::vector<BakeValue> bake;

    friend bool operator==(const CacheIdentity&, const CacheIdentity&) = default;
};

// The modes a bake takes when it is given none: every rigid physics emitter
// cached (Rigid), the rest live, except an emitter bearing particles from
// cached bodies (SpawnFromContacts, SpawnFromBodies), which is cached with
// them as a particle layer, or, too large for one, leaves those bodies live.
[[nodiscard]] std::vector<CacheMode> default_cache_modes(const Effect& effect);

// The parameters emitter `index` depends on, by index, ascending: those its
// simulated inputs bind, and those of every rigid emitter whose bodies it
// collides with. Draw inputs are not in it.
[[nodiscard]] std::vector<std::size_t> parameters_read(const Effect& effect, std::size_t index);

// The identity a bake of `effect` with `modes` (empty for the default) takes,
// placed with `instance_seed` and run at `values` (the parameters' values in
// declaration order; missing ones take their defaults, and all are clamped
// to their ranges), with the meshes and fields it is played with.
[[nodiscard]] CacheIdentity cache_identity(const Effect& effect, std::span<const CacheMode> modes,
                                           std::uint32_t instance_seed, std::span<const ParameterValue> values = {},
                                           const MeshSet& meshes = {}, const FieldSet& fields = {});

// The most bodies a cached rigid layer may hold, and particles a cached
// particle layer: refused up front, before anything is stepped, by the
// emitter's capacity.
// - **Bodies:** the rigid engine's own cap. A body is 36 bytes a step, so a
//   full layer is 2.4 MB a step and about 140 MB a second at 60 Hz -- what
//   a node's local disk streams, which is where a cache is played from.
// - **Particles:** a quarter of a million. A particle that lives on costs 6
//   bytes a step at the least, so a full layer is 1.5 MB a step and about
//   90 MB a second at 60 Hz before births and keyframes. A layer bigger
//   than that is cheaper to simulate than to stream on any GPU that could
//   draw it (a million particles step in 1.9 ms on an RTX 3090), so it stays
//   live.
inline constexpr std::uint32_t max_cached_bodies = max_rigid_capacity;
inline constexpr std::uint32_t max_cached_particles = 1U << 18U;

// What playing a cache may cost: the bake is refused, naming the emitter
// that weighs most, if it comes out bigger. Both are measured, not estimated.
struct CacheBudget final {
    // The whole file.
    std::uint64_t bytes{4ULL << 30U};
    // The most read in any one second of playback, every cached emitter and
    // checkpoint together.
    std::uint64_t bytes_per_second{256ULL << 20U};
};

struct BakeSettings final {
    // Steps cached: ticks 0 to frames - 1. Past the last, a cached emitter
    // holds its last frame (Simulation::beyond_cache says when).
    std::uint64_t frames{0U};
    // What to cache of each emitter, in effect order. Empty caches every
    // rigid physics emitter (Rigid) and leaves the rest live.
    std::vector<CacheMode> modes;
    // The parameters' values the bake runs with, in declaration order
    // (missing ones take their defaults). The ones the identity holds are
    // what the cache is; the rest steer only the live emitters while baking.
    std::vector<ParameterValue> values;
    // A particle layer's exact keyframes, every this many steps.
    std::uint64_t keyframe_interval{60U};
    // Whole-state checkpoints every this many steps; 0 takes none. They are
    // left out, whatever this says, when a live emitter reads a bake-time
    // parameter the identity does not hold: its state at a checkpoint would
    // then describe one history of that parameter among many.
    std::uint64_t checkpoint_interval{120U};
    // A particle layer's position quantum, in effect units.
    float position_step{1.0F / 1024.0F};
    CacheBudget budget{};
    // Called after each step baked with the step reached; returning false
    // stops the bake, which then fails.
    std::function<bool(std::uint64_t)> progress;
};

// What one emitter of a cache costs to keep and to play.
struct CacheLayer final {
    // As the effect baked named it.
    std::string emitter;
    CacheMode mode{CacheMode::Live};
    std::uint64_t bytes{0U};
    // The most read in any one second of playback.
    std::uint64_t peak_bytes_per_second{0U};
    // The most particles (or bodies) any step held.
    std::uint32_t peak_count{0U};
};

struct BakeReport final {
    CacheIdentity identity;
    std::uint64_t bytes{0U};
    std::uint64_t peak_bytes_per_second{0U};
    std::uint64_t checkpoint_bytes{0U};
    std::vector<CacheLayer> layers;
};

struct Baked final {
    std::vector<std::uint8_t> bytes;
    BakeReport report;
};

// Simulates `effect` from step 0, as placed with `instance_seed`, with
// `settings.values`, and writes the cache. `options` are what
// Simulation::create takes: a solver is needed while baking whatever is
// cached, and the meshes and fields go into the cached emitters' digests.
// Fails as Simulation::create does; with InvalidEffect for a mode an emitter
// cannot take; and with BudgetExceeded for a layer over its count, or a
// cache over `settings.budget`, naming what does not fit.
[[nodiscard]] Result<Baked> bake(const Effect& effect, std::uint32_t instance_seed, SimulationOptions options,
                                 const BakeSettings& settings);

// A cache file, read and checked against what it will be played with.
// Decoding reads only the bytes it needs; a Cache is immutable and may be
// read from several threads.
class Cache final {
public:
    // Opens a cache to play `effect`, placed with `instance_seed`, at
    // parameter `values` (in declaration order; missing ones take their
    // defaults), with the meshes and fields it plays with. The cached
    // emitters are found by name. Refuses, with a reason, a cache that is
    // malformed (MalformedCache), of another version (UnsupportedVersion) or
    // that does not belong (StaleCache): another instance seed or rate, a
    // cached emitter that has changed or is gone, or another value of a
    // bake-time parameter a cached emitter reads.
    //
    // Its whole-state checkpoints are used only where the effect is the one
    // baked in every emitter, live ones too; otherwise seeks step from the
    // start as they would without a cache.
    [[nodiscard]] static Result<Cache> open(std::vector<std::uint8_t> bytes, const Effect& effect,
                                            std::uint32_t instance_seed, std::span<const ParameterValue> values = {},
                                            const MeshSet& meshes = {}, const FieldSet& fields = {});

    Cache(Cache&&) noexcept;
    Cache& operator=(Cache&&) noexcept;
    ~Cache();

    [[nodiscard]] const CacheIdentity& identity() const noexcept;
    [[nodiscard]] const Effect& effect() const noexcept;
    // Steps held: ticks 0 to frames() - 1.
    [[nodiscard]] std::uint64_t frames() const noexcept;
    [[nodiscard]] std::uint64_t bytes() const noexcept;
    [[nodiscard]] const BakeReport& report() const noexcept;
    // What the cache holds of the playing effect's emitter `index`.
    [[nodiscard]] CacheMode mode(std::size_t index) const noexcept;
    // Whether its checkpoints describe the playing effect (see open).
    [[nodiscard]] bool has_checkpoints() const noexcept;
    [[nodiscard]] bool caches_anything() const noexcept;

    // Emitter `index` at `tick` (the last frame past the end), decoded from
    // the nearest keyframe. Empty for a live emitter.
    [[nodiscard]] EmitterState emitter_at(std::size_t index, std::uint64_t tick) const;
    // Moves `state`, emitter `index` as this cache decoded it at tick - 1,
    // on to `tick`: the cheap way to play forwards.
    void advance(std::size_t index, std::uint64_t tick, EmitterState& state) const;
    // The latest whole-state checkpoint at or before `tick`, if any: what
    // the effect was there, cached emitters as this cache decodes them.
    [[nodiscard]] std::optional<SimulationState> checkpoint_before(std::uint64_t tick) const;
    // Which tick that checkpoint is, without reading it.
    [[nodiscard]] std::optional<std::uint64_t> checkpoint_tick(std::uint64_t tick) const;

    struct Data;

private:
    explicit Cache(std::unique_ptr<Data> data);
    std::unique_ptr<Data> data_;
};

} // namespace ajfx
