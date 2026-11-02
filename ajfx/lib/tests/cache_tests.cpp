#include "check.hpp"

#include <ajfx/builder.hpp>
#include <ajfx/cache.hpp>
#include <ajfx/package.hpp>
#include <ajfx/rigid.hpp>
#include <ajfx/simulation.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Caching and playback: saved state, rigid and particle caches, their
// identity, and the split between bake-time and playback-time parameters.
// Expectations are independent of the code under test: a cached layer is
// compared with the simulation it was taken from, step by step.
namespace {

using namespace ajfx;

std::unique_ptr<RigidSolver> g_solver;

template <class T>
T must(Result<T> result) {
    if (!result) {
        std::cout << "  " << result.error().message << '\n';
    }
    AJFX_CHECK(result.ok());
    return std::move(result).value();
}

Simulation make(const Effect& effect, std::shared_ptr<const Cache> cache = nullptr,
                const RigidSolver* solver = g_solver.get()) {
    SimulationOptions options;
    options.rigid = solver;
    options.cache = std::move(cache);
    return must(Simulation::create(effect, 7U, std::move(options)));
}

// Nine crates stacked, a bouncy ball dropped on them, crates that come apart
// into four slabs when hit hard, fading as they age; and snow that settles
// on whatever stands, crate or slab. The crates are what a cache holds and
// the snow what stays live, meeting the cached bodies.
// The crates' size is a bake-time parameter the cache depends on; the snow's
// rate one only the live snow reads (or, with `rate_bound` false, a constant).
Effect crates_in_the_snow(float threshold = 3.0F, bool rate_bound = true) {
    EffectBuilder builder{"crates_in_the_snow"};
    builder.float_parameter("snow_rate", 120.0F, 0.0F, 400.0F);
    builder.color_parameter("tint", Color{1.0F, 1.0F, 1.0F, 1.0F}, 0.0F, 4.0F).playback();
    builder.float_parameter("crate_size", 0.8F, 0.5F, 1.2F);
    builder.emitter("crates")
        .capacity(64U)
        .spawn(SpawnPile{.count = 9U, .shape = PileShape::Box, .width = 3.0F, .depth = 1.0F, .jitter = 0.0F,
                         .release = 0.0F, .body = 0U})
        .spawn(SpawnBurst{.time = 0.25F, .count = 1U, .body = 1U})
        .init(Lifetime{.min = 30.0F, .max = 30.0F})
        .init(ShapeLocation{.shape = Shape::Point, .center = Vec3{0.2F, 4.0F, 0.1F}})
        .init(Velocity{.speed_min = 0.0F, .speed_max = 0.0F})
        .init(InitSize{.min = bind("crate_size"), .max = bind("crate_size")})
        .init(InitColor{.a = Color{0.8F, 0.5F, 0.3F, 1.0F}, .b = Color{1.0F, 0.7F, 0.4F, 1.0F}})
        .update(Gravity{})
        .update(RigidPhysics{.shape = RigidShape::Box,
                             .friction = 0.7F,
                             .restitution = 0.05F,
                             .bodies = {RigidBodyKind{.shape = RigidShape::Sphere, .scale = 1.2F,
                                                      .restitution = 0.7F},
                                        RigidBodyKind{.shape = RigidShape::Box}}})
        .update(Break{.body = 0U,
                      .threshold = threshold,
                      .scatter = 1.0F,
                      .spin = 120.0F,
                      .pieces = {BreakPiece{.body = 2U, .offset = Vec3{-0.25F, 0.25F, 0.0F}, .size = 0.5F},
                                 BreakPiece{.body = 2U, .offset = Vec3{0.25F, 0.25F, 0.0F}, .size = 0.5F},
                                 BreakPiece{.body = 2U, .offset = Vec3{-0.25F, -0.25F, 0.0F}, .size = 0.5F},
                                 BreakPiece{.body = 2U, .offset = Vec3{0.25F, -0.25F, 0.0F}, .size = 0.5F}}})
        .update(ColorOverLife{.keys = {{0.0F, Color{1.0F, 1.0F, 1.0F, 1.0F}}, {1.0F, Color{0.2F, 0.2F, 0.2F, 1.0F}}}})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 0.8F});
    builder.emitter("snow")
        .capacity(4096U)
        .seed(11U)
        .spawn(rate_bound ? SpawnRate{.rate = bind("snow_rate")} : SpawnRate{.rate = 120.0F})
        .init(Lifetime{.min = 6.0F, .max = 8.0F})
        .init(ShapeLocation{.shape = Shape::Disc, .center = Vec3{0.0F, 5.0F, 0.0F}, .radius_min = 0.0F,
                            .radius_max = 2.0F})
        .init(Velocity{.direction = Vec3{0.0F, -1.0F, 0.0F}, .speed_min = 0.6F, .speed_max = 1.0F})
        .init(InitSize{.min = 0.04F, .max = 0.06F})
        .update(Gravity{.acceleration = Vec3{0.0F, -2.0F, 0.0F}})
        .update(BodyCollision{.emitter = "crates", .restitution = 0.0F, .friction = 1.0F})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 1.0F})
        .renderer(SpriteRenderer{.tint = bind("tint")});
    return must(builder.build());
}

// Sparks: a stream with every appearance a particle layer can have -- colour
// and size over life, spin, velocity-facing streaks -- and one particle in
// fifty fast enough to overrun a quantised step.
Effect sparks() {
    EffectBuilder builder{"sparks"};
    builder.emitter("sparks")
        .capacity(8192U)
        .seed(5U)
        .spawn(SpawnRate{.rate = 900.0F})
        .spawn(SpawnBurst{.time = 0.5F, .count = 400U, .interval = 1.0F, .cycles = 0U})
        .init(Lifetime{.min = 0.5F, .max = 2.5F})
        .init(ShapeLocation{.shape = Shape::Sphere, .radius_min = 0.0F, .radius_max = 0.2F})
        .init(Velocity{.speed_min = 1.0F, .speed_max = 6.0F, .spread = 60.0F})
        .init(InitColor{.a = Color{1.0F, 0.6F, 0.2F, 1.0F}, .b = Color{1.0F, 0.9F, 0.5F, 1.0F}})
        .init(InitSize{.min = 0.02F, .max = 0.05F})
        .init(InitRotation{})
        .init(InitSpin{.speed_min = 30.0F, .speed_max = 400.0F, .drag = 0.5F})
        .update(Gravity{})
        .update(Drag{.coefficient = 0.3F})
        .update(CurlNoise{.strength = 1.5F})
        .update(ColorOverLife{})
        .update(SizeOverLife{.keys = {{0.0F, 0.5F}, {0.3F, 1.0F}, {1.0F, 0.1F}}})
        .update(PlaneCollision{.restitution = 0.4F, .friction = 0.2F})
        .renderer(SpriteRenderer{.facing = Facing::Velocity, .stretch = 0.03F});
    builder.emitter("embers")
        .capacity(512U)
        .seed(6U)
        .spawn(SpawnRate{.rate = 40.0F})
        .init(Lifetime{.min = 2.0F, .max = 3.0F})
        .init(Velocity{.speed_min = 300.0F, .speed_max = 400.0F, .spread = 10.0F})
        .update(Gravity{});
    return must(builder.build());
}

BakeSettings settings(std::uint64_t frames, std::vector<CacheMode> modes = {},
                      std::vector<ParameterValue> values = {}) {
    BakeSettings out;
    out.frames = frames;
    out.modes = std::move(modes);
    out.values = std::move(values);
    out.keyframe_interval = 30U;
    out.checkpoint_interval = 60U;
    return out;
}

Baked bake_it(const Effect& effect, const BakeSettings& with) {
    return must(bake(effect, 7U, SimulationOptions{.rigid = g_solver.get()}, with));
}

std::shared_ptr<const Cache> open(const Effect& effect, const Baked& baked,
                                  std::span<const ParameterValue> values = {}) {
    return std::make_shared<const Cache>(must(Cache::open(baked.bytes, effect, 7U, values)));
}

float distance(Vec3 a, Vec3 b) {
    const Vec3 d = a - b;
    return std::sqrt(dot(d, d));
}

// Every field a cached body promises, bit for bit.
bool same_bodies(const EmitterState& cached, const EmitterState& simulated) {
    if (cached.alive() != simulated.alive() || cached.spawned != simulated.spawned ||
        cached.refused != simulated.refused) {
        return false;
    }
    for (std::size_t i = 0U; i < simulated.alive(); ++i) {
        if (cached.spawn_index[i] != simulated.spawn_index[i] || cached.position[i] != simulated.position[i] ||
            cached.orientation[i] != simulated.orientation[i] || cached.age[i] != simulated.age[i] ||
            cached.lifetime[i] != simulated.lifetime[i] || cached.color[i] != simulated.color[i] ||
            cached.size[i] != simulated.size[i] || cached.body_kind[i] != simulated.body_kind[i]) {
            return false;
        }
    }
    return true;
}

// Counts the steps a seek takes: a provider is asked once a step.
class CountingDefaults final : public ParameterProvider {
public:
    void values_at(std::uint64_t, std::span<ParameterValue>) const override { ++asked; }
    mutable std::uint64_t asked{0U};
};

// ------------------------------------------------------------ B1: state on disk

void saved_state_restores_bit_for_bit() {
    const Effect effect = crates_in_the_snow();
    Simulation straight = make(effect);
    std::vector<std::vector<std::uint8_t>> saved;
    for (std::uint64_t tick = 0U; tick <= 150U; ++tick) {
        if (tick > 0U) {
            straight.step();
        }
        const SimulationState& state = straight.state();
        std::vector<std::uint8_t> bytes = write_state(state);
        // Deterministic: the same state writes the same bytes.
        AJFX_CHECK(write_state(state) == bytes);
        const SimulationState back = must(read_state(bytes));
        AJFX_CHECK(back == state);
        saved.push_back(std::move(bytes));
    }
    // Bodies, pieces and the snow on them are all there by step 150.
    AJFX_CHECK(straight.emitter(0U).alive() > 9U);
    AJFX_CHECK(straight.emitter(1U).alive() > 100U);

    // Restored from disk into a fresh simulation, every saved step carries on
    // exactly as the run it was saved from.
    for (const std::uint64_t from : {0U, 20U, 31U, 77U, 120U}) {
        Simulation restored = make(effect);
        restored.restore(must(read_state(saved[from])));
        Simulation reference = make(effect);
        reference.advance_to(from);
        for (std::uint64_t tick = from; tick < 150U; ++tick) {
            restored.step();
            reference.step();
        }
        AJFX_CHECK(restored.state() == reference.state());
        AJFX_CHECK(write_state(restored.state()) == saved[150U]);
    }

    // And after a seek: back to a restored step, then on.
    Simulation seeking = make(effect);
    CheckpointRing ring{30U, 8U};
    ring.seek(seeking, 140U);
    seeking.restore(must(read_state(saved[45U])));
    ring.clear();
    ring.seek(seeking, 150U);
    AJFX_CHECK(write_state(seeking.state()) == saved[150U]);
}

void damaged_state_is_refused_with_a_reason() {
    Simulation simulation = make(crates_in_the_snow());
    simulation.advance_to(40U);
    const std::vector<std::uint8_t> bytes = write_state(simulation.state());

    std::vector<std::uint8_t> damaged = bytes;
    damaged[damaged.size() / 2U] ^= 0x10U;
    Result<SimulationState> refused = read_state(damaged);
    AJFX_CHECK(!refused && refused.error().code == ErrorCode::MalformedCache);

    std::vector<std::uint8_t> truncated(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() / 3U));
    refused = read_state(truncated);
    AJFX_CHECK(!refused && refused.error().code == ErrorCode::MalformedCache);

    AJFX_CHECK(read_state(std::vector<std::uint8_t>{'n', 'o', 'p', 'e'}).error().code == ErrorCode::MalformedCache);

    // A newer version, with a checksum that matches, is refused for its
    // version rather than read as if it were this one.
    std::vector<std::uint8_t> newer(bytes.begin(), bytes.end() - 8);
    newer[4] = static_cast<std::uint8_t>(state_format_version + 1U);
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const std::uint8_t byte : newer) {
        hash = (hash ^ byte) * 0x100000001b3ULL;
    }
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        newer.push_back(static_cast<std::uint8_t>(hash >> shift));
    }
    refused = read_state(newer);
    AJFX_CHECK(!refused && refused.error().code == ErrorCode::UnsupportedVersion);
}

// ------------------------------------------------------------ B2: a rigid cache

void a_rigid_cache_plays_what_was_simulated() {
    const Effect effect = crates_in_the_snow();
    constexpr std::uint64_t frames = 240U;
    const Baked baked = bake_it(effect, settings(frames));
    // The crates depend on their size; the live snow's rate is its own.
    AJFX_CHECK(baked.report.identity.bake.size() == 1U);
    AJFX_CHECK(baked.report.identity.bake[0].name == "crate_size");
    AJFX_CHECK(baked.report.layers[0].mode == CacheMode::Rigid);
    AJFX_CHECK(baked.report.layers[1].mode == CacheMode::Live);
    AJFX_CHECK(baked.report.layers[1].bytes == 0U);
    // And so no checkpoints: the snow's state there would be one rate's.
    AJFX_CHECK(baked.report.checkpoint_bytes == 0U);
    const std::shared_ptr<const Cache> cache = open(effect, baked);

    // The cached crates need no solver: nothing in the playing simulation
    // runs rigid physics.
    Simulation played = make(effect, cache, nullptr);
    Simulation simulated = make(effect);
    std::uint32_t most = 0U;
    bool broke = false;
    for (std::uint64_t tick = 0U; tick < frames; ++tick) {
        if (tick > 0U) {
            played.step();
            simulated.step();
        }
        AJFX_CHECK(same_bodies(played.emitter(0U), simulated.emitter(0U)));
        // The live snow met the cached bodies exactly where it met the
        // simulated ones, so every flake is where it was, to the bit.
        AJFX_CHECK(played.emitter(1U) == simulated.emitter(1U));
        // Random access lands on the same frame as playing through.
        if (tick % 37U == 0U) {
            AJFX_CHECK(same_bodies(cache->emitter_at(0U, tick), simulated.emitter(0U)));
        }
        most = std::max(most, static_cast<std::uint32_t>(simulated.emitter(0U).alive()));
        broke = broke || simulated.emitter(0U).alive() > 10U;
    }
    AJFX_CHECK(broke);
    AJFX_CHECK(baked.report.layers[0].peak_count == most);
    // Each body costs 36 bytes a step, and a few bytes more once.
    std::cout << "  rigid layer: " << baked.report.layers[0].bytes << " bytes over " << frames << " steps, up to "
              << most << " bodies, peak " << baked.report.layers[0].peak_bytes_per_second << " bytes/s\n";
    AJFX_CHECK(baked.report.layers[0].bytes < static_cast<std::uint64_t>(frames) * (most * 36U + 64U) + 64U * 60U);

    // Seeking about, from the cache's own checkpoints, lands where playing
    // through did.
    Simulation reference = make(effect);
    std::vector<SimulationState> truth;
    for (std::uint64_t tick = 0U; tick < frames; ++tick) {
        if (tick > 0U) {
            reference.step();
        }
        if (tick % 23U == 0U) {
            truth.push_back(reference.state());
        }
    }
    CheckpointRing ring{30U, 4U};
    for (const std::uint64_t target : {207U, 46U, 230U, 0U, 115U, 161U}) {
        ring.seek(played, target);
        const SimulationState& want = truth[target / 23U];
        AJFX_CHECK(played.tick() == target);
        AJFX_CHECK(same_bodies(played.emitter(0U), want.emitters[0]));
        AJFX_CHECK(played.emitter(1U) == want.emitters[1]);
    }

    // Past the end the bodies hold their last frame.
    Simulation beyond = make(effect, cache, nullptr);
    beyond.advance_to(frames + 20U);
    AJFX_CHECK(beyond.beyond_cache());
    AJFX_CHECK(same_bodies(beyond.emitter(0U), cache->emitter_at(0U, frames - 1U)));
}

void cached_checkpoints_make_a_far_seek_a_read() {
    // Nothing outside the cache steers the live snow, so the cache can hold
    // its checkpoints.
    const Effect effect = crates_in_the_snow(3.0F, false);
    const Baked baked = bake_it(effect, settings(600U));
    AJFX_CHECK(baked.report.checkpoint_bytes > 0U);
    Simulation played = make(effect, open(effect, baked), nullptr);
    CheckpointRing ring{30U, 4U};
    CountingDefaults counting;
    ring.seek(played, 590U, &counting);
    // From the checkpoint at 540, not from the start.
    AJFX_CHECK(played.tick() == 590U);
    std::cout << "  a seek to 590 stepped " << counting.asked << " times\n";
    AJFX_CHECK(counting.asked <= 51U);
    Simulation simulated = make(effect);
    simulated.advance_to(590U);
    AJFX_CHECK(played.emitter(1U) == simulated.emitter(1U));
    AJFX_CHECK(same_bodies(played.emitter(0U), simulated.emitter(0U)));
}

// ------------------------------------------------------------ B3: particles

void a_particle_cache_stays_within_its_quantum() {
    const Effect effect = sparks();
    constexpr std::uint64_t frames = 180U;
    BakeSettings with = settings(frames, {CacheMode::Particles, CacheMode::Particles});
    const Baked baked = must(bake(effect, 7U, SimulationOptions{}, with));
    const std::shared_ptr<const Cache> cache = open(effect, baked);

    Simulation played = make(effect, cache, nullptr);
    Simulation simulated = make(effect);
    std::uint64_t raw = 0U;
    std::uint64_t escaped = 0U;
    for (std::uint64_t tick = 0U; tick < frames; ++tick) {
        if (tick > 0U) {
            played.step();
            simulated.step();
        }
        for (std::size_t e = 0U; e < 2U; ++e) {
            const EmitterState& got = played.emitter(e);
            const EmitterState& want = simulated.emitter(e);
            raw += want.alive() * 32U;
            AJFX_CHECK(got.alive() == want.alive());
            AJFX_CHECK(got.spawned == want.spawned);
            AJFX_CHECK(got.spawn_index == want.spawn_index);
            // Worked out again, not stored, and exact.
            AJFX_CHECK(got.age == want.age);
            AJFX_CHECK(got.color == want.color);
            AJFX_CHECK(got.size == want.size);
            const bool key = tick % with.keyframe_interval == 0U;
            for (std::size_t i = 0U; i < want.alive(); ++i) {
                const float off = distance(got.position[i], want.position[i]);
                // Half a quantum on each axis, never drifting.
                AJFX_CHECK(off <= with.position_step * 0.87F + 1.0e-5F);
                AJFX_CHECK(!key || got.position[i] == want.position[i]);
                if (e == 1U && got.position[i] == want.position[i]) {
                    ++escaped;
                }
                if (e == 0U) {
                    // Velocity at half precision, where the sparks face it.
                    const float speed = std::sqrt(dot(want.velocity[i], want.velocity[i]));
                    AJFX_CHECK(distance(got.velocity[i], want.velocity[i]) <= speed * 2.0e-3F + 1.0e-4F);
                    const float turn = std::remainder(got.rotation[i] - want.rotation[i],
                                                      2.0F * 3.14159265F);
                    AJFX_CHECK(std::fabs(turn) <= 1.0e-4F + 2.0F * 3.14159265F / 65536.0F);
                }
            }
        }
        if (tick % 41U == 0U) {
            const EmitterState at = cache->emitter_at(0U, tick);
            AJFX_CHECK(at.position == played.emitter(0U).position);
        }
    }
    // Embers move 5 to 7 units a step, far past a 16-bit step of positions,
    // and are kept exactly instead.
    AJFX_CHECK(escaped > 0U);
    std::cout << "  particle cache: " << baked.report.bytes << " bytes against " << raw
              << " as 32-byte records; peak " << baked.report.peak_bytes_per_second << " bytes/s\n";
    // Sparks are the costly case: a streak faces its velocity and a sprite
    // spins, so both are kept beside the position (14 bytes a particle).
    AJFX_CHECK(baked.report.bytes * 4U < raw * 3U);
}

// ------------------------------------------------------------ identity

void a_cache_that_does_not_belong_is_refused() {
    const Effect effect = crates_in_the_snow();
    const std::vector<ParameterValue> values{to_value(200.0F), to_value(Color{1.0F, 1.0F, 1.0F, 1.0F}),
                                             to_value(0.9F)};
    const Baked baked = bake_it(effect, settings(30U, {}, values));
    AJFX_CHECK(Cache::open(baked.bytes, effect, 7U, values).ok());
    AJFX_CHECK(baked.report.identity == cache_identity(effect, {}, 7U, values));
    AJFX_CHECK(baked.report.identity.emitters.size() == 1U);
    AJFX_CHECK(baked.report.identity.emitters[0].emitter == "crates");

    const auto refused = [&](const Effect& with, std::uint32_t seed, std::span<const ParameterValue> at) {
        Result<Cache> opened = Cache::open(baked.bytes, with, seed, at);
        if (opened) {
            return false;
        }
        std::cout << "  refused: " << opened.error().message << '\n';
        return opened.error().code == ErrorCode::StaleCache;
    };
    // Another seed, value of a parameter the crates read, or crates.
    AJFX_CHECK(refused(effect, 8U, values));
    AJFX_CHECK(refused(effect, 7U, {})); // the crates' default size, not 0.9
    AJFX_CHECK(refused(crates_in_the_snow(4.0F), 7U, values));
    Effect gone = effect;
    gone.emitters[0].name = "boxes";
    gone.emitters[1].update[1] = BodyCollision{.id = 99U, .emitter = "boxes", .restitution = 0.0F, .friction = 1.0F};
    AJFX_CHECK(refused(gone, 7U, values));

    // Not what it is: a playback-time tint, the live snow's own rate, the
    // live snow itself, or how the crates look.
    const std::vector<ParameterValue> other{to_value(50.0F), to_value(Color{0.2F, 0.4F, 1.0F, 0.5F}),
                                            to_value(0.9F)};
    AJFX_CHECK(Cache::open(baked.bytes, effect, 7U, other).ok());
    Effect restyled = effect;
    restyled.emitters[1].update[0] = Gravity{.id = 50U, .acceleration = Vec3{0.0F, -3.0F, 0.0F}};
    restyled.emitters[0].renderer.blend = BlendMode::Additive;
    restyled.emitters[0].renderer.light_count = 4U;
    const Result<Cache> still = Cache::open(baked.bytes, restyled, 7U, values);
    AJFX_CHECK(still.ok());
    // Its checkpoints described the old snow, so they are not used.
    AJFX_CHECK(!still.value().has_checkpoints());

    // Damage.
    std::vector<std::uint8_t> damaged = baked.bytes;
    damaged[damaged.size() / 2U] ^= 1U;
    AJFX_CHECK(Cache::open(damaged, effect, 7U, values).error().code == ErrorCode::MalformedCache);
    // A simulation will not take a cache opened for another effect.
    Effect renamed = effect;
    renamed.name = "elsewhere";
    SimulationOptions options;
    options.rigid = g_solver.get();
    options.cache = open(effect, baked, values);
    AJFX_CHECK(Simulation::create(renamed, 7U, options).error().code == ErrorCode::StaleCache);
}

void a_cache_outlives_edits_to_what_it_does_not_hold() {
    // Bake the crates, then change the live snow: the cache still plays,
    // and the crates the new snow lands on are the very ones simulated.
    const Effect effect = crates_in_the_snow(3.0F, false);
    const Baked baked = bake_it(effect, settings(120U));
    AJFX_CHECK(baked.report.checkpoint_bytes > 0U);
    Effect heavier = effect;
    heavier.emitters[1].update[0] = Gravity{.id = 50U, .acceleration = Vec3{0.0F, -4.0F, 0.0F}};
    AJFX_CHECK(validate(heavier).ok());
    const std::shared_ptr<const Cache> cache = open(heavier, baked);
    AJFX_CHECK(!cache->has_checkpoints());
    Simulation played = make(heavier, cache, nullptr);
    Simulation simulated = make(heavier);
    for (std::uint64_t tick = 0U; tick < 119U; ++tick) {
        played.step();
        simulated.step();
        AJFX_CHECK(played.emitter(1U) == simulated.emitter(1U));
        AJFX_CHECK(same_bodies(played.emitter(0U), simulated.emitter(0U)));
    }
}

class Values final : public ParameterProvider {
public:
    explicit Values(std::vector<ParameterValue> values) : values_{std::move(values)} {}
    void values_at(std::uint64_t, std::span<ParameterValue> v) const override {
        std::copy(values_.begin(), values_.end(), v.begin());
    }

private:
    std::vector<ParameterValue> values_;
};

void a_live_layer_beside_a_cache_follows_its_own_parameters() {
    const Effect effect = crates_in_the_snow();
    const Color white{1.0F, 1.0F, 1.0F, 1.0F};
    const Baked baked = bake_it(effect, settings(150U, {}, {to_value(120.0F), to_value(white), to_value(1.0F)}));
    // Played at another snow rate, and asking for other crates: the snow
    // follows its own rate, and the crates are the cache's, size 1, which
    // the snow lands on.
    const Values asked{{to_value(40.0F), to_value(white), to_value(0.6F)}};
    const Values truth{{to_value(40.0F), to_value(white), to_value(1.0F)}};
    Simulation played = make(effect, open(effect, baked, std::vector{to_value(40.0F), to_value(white), to_value(1.0F)}),
                             nullptr);
    Simulation simulated = make(effect);
    played.reset(&asked);
    simulated.reset(&truth);
    for (std::uint64_t tick = 0U; tick < 149U; ++tick) {
        played.step(&asked);
        simulated.step(&truth);
        AJFX_CHECK(played.emitter(1U) == simulated.emitter(1U));
        AJFX_CHECK(same_bodies(played.emitter(0U), simulated.emitter(0U)));
    }
    // Fewer flakes than the baked rate would have made.
    Simulation baked_rate = make(effect);
    baked_rate.advance_to(149U);
    AJFX_CHECK(played.emitter(1U).spawned < baked_rate.emitter(1U).spawned);
}

// ------------------------------------------------------------ B4: timing

void playback_parameters_never_reach_the_simulation() {
    // A playback-time parameter bound to something the simulation reads.
    EffectBuilder into_simulation{"wrong"};
    into_simulation.float_parameter("rate", 10.0F, 0.0F, 100.0F).playback();
    into_simulation.emitter("e").spawn(SpawnRate{.rate = bind("rate")}).init(Lifetime{});
    Result<Effect> refused = into_simulation.build();
    AJFX_CHECK(!refused);
    std::cout << "  refused: " << refused.error().message << '\n';
    AJFX_CHECK(refused.error().message.find("playback-time") != std::string::npos);

    // A bake-time parameter bound to a draw input.
    EffectBuilder into_drawing{"wrong"};
    into_drawing.float_parameter("glow", 1.0F, 0.0F, 4.0F);
    into_drawing.emitter("e").spawn(SpawnRate{}).init(Lifetime{}).renderer(
        SpriteRenderer{.brightness = bind("glow")});
    refused = into_drawing.build();
    AJFX_CHECK(!refused);
    AJFX_CHECK(refused.error().message.find("bake-time") != std::string::npos);

    // Negative draw inputs.
    EffectBuilder negative{"wrong"};
    negative.emitter("e").spawn(SpawnRate{}).init(Lifetime{}).renderer(SpriteRenderer{.opacity = -1.0F});
    AJFX_CHECK(!negative.build());

    // Timing and draw inputs survive a package round trip; an effect that
    // uses neither writes no word of them, so its bytes are what they were.
    const Effect effect = crates_in_the_snow();
    const std::vector<std::uint8_t> package = must(write_package(effect));
    const Effect back = must(read_package(package));
    AJFX_CHECK(back == effect);
    AJFX_CHECK(back.parameters[1].timing == ParameterTiming::Playback);
    const std::string plain = effect_to_json(sparks());
    AJFX_CHECK(plain.find("timing") == std::string::npos);
    AJFX_CHECK(plain.find("tint") == std::string::npos);
    AJFX_CHECK(plain.find("size_scale") == std::string::npos);
    AJFX_CHECK(effect_from_json(plain).ok());
    std::string unknown = effect_to_json(effect);
    const std::size_t at = unknown.find("\"playback\"");
    AJFX_CHECK(at != std::string::npos);
    unknown.replace(at, 10U, "\"sometimes\"");
    AJFX_CHECK(!effect_from_json(unknown));

    // What a renderer multiplies by follows the playback value.
    std::vector<ParameterValue> values = parameter_values(effect, nullptr, 0U);
    AJFX_CHECK(draw_adjustment(effect, 1U, values) == DrawAdjustment{});
    values[1] = to_value(Color{0.5F, 0.25F, 2.0F, 0.5F});
    const DrawAdjustment tinted = draw_adjustment(effect, 1U, values);
    AJFX_CHECK(tinted.color == (Color{0.5F, 0.25F, 2.0F, 0.5F}));
    EmitterState particles;
    particles.color = {Color{1.0F, 1.0F, 1.0F, 1.0F}};
    particles.size = {0.1F};
    adjust_for_drawing(DrawAdjustment{Color{0.5F, 1.0F, 1.0F, 0.25F}, 2.0F}, particles);
    AJFX_CHECK(particles.color[0] == (Color{0.5F, 1.0F, 1.0F, 0.25F}));
    AJFX_CHECK(particles.size[0] == 0.2F);
}

// ------------------------------------------------------------ budgets

void bakes_over_budget_are_refused() {
    const Effect effect = crates_in_the_snow();
    const SimulationOptions solver{.rigid = g_solver.get()};
    // The snow as a cached particle layer, in a cache allowed 20 KB.
    BakeSettings tight = settings(240U, {CacheMode::Rigid, CacheMode::Particles});
    tight.budget.bytes = 20000U;
    Result<Baked> refused = bake(effect, 7U, solver, tight);
    AJFX_CHECK(!refused && refused.error().code == ErrorCode::BudgetExceeded);
    std::cout << "  refused: " << refused.error().message << '\n';
    AJFX_CHECK(refused.error().message.find("weighs most") != std::string::npos);

    BakeSettings slow = settings(240U, {CacheMode::Rigid, CacheMode::Particles});
    slow.budget.bytes_per_second = 1000U;
    refused = bake(effect, 7U, solver, slow);
    AJFX_CHECK(!refused && refused.error().code == ErrorCode::BudgetExceeded);

    // A layer bigger than a cache may hold is refused before anything runs.
    EffectBuilder big{"big"};
    big.emitter("e").capacity(max_cached_particles + 1U).spawn(SpawnRate{}).init(Lifetime{});
    const Effect huge = must(big.build());
    refused = bake(huge, 7U, SimulationOptions{}, settings(10U, {CacheMode::Particles}));
    AJFX_CHECK(!refused && refused.error().code == ErrorCode::BudgetExceeded);

    // Modes an emitter cannot take.
    refused = bake(effect, 7U, solver, settings(10U, {CacheMode::Particles, CacheMode::Live}));
    AJFX_CHECK(!refused && refused.error().code == ErrorCode::InvalidEffect);
    refused = bake(effect, 7U, solver, settings(10U, {CacheMode::Rigid, CacheMode::Rigid}));
    AJFX_CHECK(!refused && refused.error().code == ErrorCode::InvalidEffect);

    // Stopped part way.
    BakeSettings stopped = settings(100U);
    stopped.progress = [](std::uint64_t tick) { return tick < 10U; };
    AJFX_CHECK(!bake(effect, 7U, solver, stopped));
}

void bakes_are_deterministic() {
    const Effect effect = crates_in_the_snow();
    const BakeSettings both = settings(120U, {CacheMode::Rigid, CacheMode::Particles});
    const Baked one = bake_it(effect, both);
    const std::unique_ptr<RigidSolver> four = rigid::make_jolt_solver(rigid::JoltOptions{.threads = 4U});
    const Baked other = must(bake(effect, 7U, SimulationOptions{.rigid = four.get()}, both));
    AJFX_CHECK(one.bytes == other.bytes);
}

} // namespace

int main() {
    g_solver = rigid::make_jolt_solver();
    const ajfx::test::Case cases[] = {
        {"saved_state_restores_bit_for_bit", saved_state_restores_bit_for_bit},
        {"damaged_state_is_refused_with_a_reason", damaged_state_is_refused_with_a_reason},
        {"a_rigid_cache_plays_what_was_simulated", a_rigid_cache_plays_what_was_simulated},
        {"cached_checkpoints_make_a_far_seek_a_read", cached_checkpoints_make_a_far_seek_a_read},
        {"a_particle_cache_stays_within_its_quantum", a_particle_cache_stays_within_its_quantum},
        {"a_cache_that_does_not_belong_is_refused", a_cache_that_does_not_belong_is_refused},
        {"a_cache_outlives_edits_to_what_it_does_not_hold", a_cache_outlives_edits_to_what_it_does_not_hold},
        {"a_live_layer_beside_a_cache_follows_its_own_parameters",
         a_live_layer_beside_a_cache_follows_its_own_parameters},
        {"playback_parameters_never_reach_the_simulation", playback_parameters_never_reach_the_simulation},
        {"bakes_over_budget_are_refused", bakes_over_budget_are_refused},
        {"bakes_are_deterministic", bakes_are_deterministic},
    };
    const int result = ajfx::test::run(cases);
    g_solver.reset();
    return result;
}
