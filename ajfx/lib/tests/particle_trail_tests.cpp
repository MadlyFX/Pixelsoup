#include "check.hpp"

#include <ajfx/builder.hpp>
#include <ajfx/cache.hpp>
#include <ajfx/package.hpp>
#include <ajfx/random.hpp>
#include <ajfx/simulation.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

// Particles shed by another emitter's moving particles (SpawnFromParticles):
// smoke trailing embers. Expectations are worked out here from the embers the
// simulation reports -- their speeds, their positions, the hash the module
// rolls -- not from the code that sheds the smoke.
namespace {

using namespace ajfx;

template <class T>
T must(Result<T> result) {
    if (!result) {
        std::cout << "  " << result.error().message << '\n';
    }
    AJFX_CHECK(result.ok());
    return std::move(result).value();
}

constexpr std::uint32_t instance_seed = 3U;

Simulation make(const Effect& effect) {
    return must(Simulation::create(effect, instance_seed));
}

float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

// Two dozen embers thrown up at once and slowed by drag until they hang,
// and the smoke they shed while they still move: twenty puffs a second for
// every unit a second of speed, none below half a unit a second. `cap` is
// the most the smoke sheds a step; `smoke_first` lists the smoke before the
// embers, which must change nothing but the emitters' places.
Effect embers_and_smoke(std::uint32_t cap = 64U, bool smoke_first = false, float loose_rate = 0.0F) {
    EffectBuilder builder{"embers"};
    const auto embers = [&builder]() {
        builder.emitter("embers")
            .capacity(64U)
            .seed(2U)
            .spawn(SpawnBurst{.time = 0.0F, .count = 24U})
            .init(Lifetime{.min = 6.0F, .max = 6.0F})
            .init(ShapeLocation{.shape = Shape::Sphere, .center = Vec3{0.0F, 1.0F, 0.0F}, .radius_min = 0.0F,
                                .radius_max = 0.2F})
            .init(Velocity{.direction = Vec3{0.0F, 1.0F, 0.0F}, .speed_min = 3.0F, .speed_max = 4.0F, .spread = 30.0F})
            .init(InitSize{.min = 0.05F, .max = 0.05F})
            .update(Drag{.coefficient = 2.0F});
    };
    const auto smoke = [&builder, cap, loose_rate]() {
        EmitterBuilder& e = builder.emitter("smoke").capacity(8192U).seed(7U);
        // Loose smoke that has nothing to do with the embers, listed first,
        // so the trail is always the second spawn module.
        e.spawn(SpawnRate{.rate = loose_rate});
        e.spawn(SpawnFromParticles{.emitter = "embers",
                                   .min_speed = 0.5F,
                                   .rate = 0.0F,
                                   .per_speed = 20.0F,
                                   .max_per_step = cap,
                                   .inherit = 0.3F})
            .init(Lifetime{.min = 2.0F, .max = 2.0F})
            .init(ShapeLocation{.shape = Shape::Sphere, .center = Vec3{0.0F, 1.0F, 0.0F}, .radius_min = 0.3F,
                                .radius_max = 0.3F, .only = Scope{.sources = {0U}}})
            .init(InitSize{.min = 0.1F, .max = 0.1F})
            .update(Gravity{.acceleration = Vec3{0.0F, 0.3F, 0.0F}});
    };
    if (smoke_first) {
        smoke();
        embers();
    } else {
        embers();
        smoke();
    }
    return must(builder.build());
}

std::size_t embers_of(const Effect& effect) {
    return effect.emitters[0].name == "embers" ? 0U : 1U;
}

// ------------------------------------------------------------ what is shed

void embers_leave_smoke_only_while_moving() {
    const Effect effect = embers_and_smoke();
    Simulation simulation = make(effect);
    std::uint64_t while_moving = 0U;
    std::uint64_t while_still = 0U;
    std::size_t still_steps = 0U;
    std::size_t moving_steps = 0U;
    for (std::uint64_t tick = 1U; tick <= 240U; ++tick) {
        const std::uint64_t before = simulation.emitter(1U).spawned;
        simulation.advance_to(tick);
        const EmitterState& embers = simulation.emitter(0U);
        const EmitterState& smoke = simulation.emitter(1U);
        float fastest = 0.0F;
        for (std::size_t i = 0U; i < embers.alive(); ++i) {
            fastest = std::max(fastest, length(embers.velocity[i]));
        }
        const std::uint64_t born = smoke.spawned - before;
        if (fastest < 0.5F) {
            while_still += born;
            ++still_steps;
        } else {
            while_moving += born;
            ++moving_steps;
        }
        // Every puff born this step sits on the step's stretch of an ember's
        // path: within the step's travel of where that ember is now.
        for (std::size_t i = 0U; i < smoke.alive(); ++i) {
            if (!(smoke.age[i] < simulation.step_seconds())) {
                continue;
            }
            float nearest = 1.0e9F;
            for (std::size_t j = 0U; j < embers.alive(); ++j) {
                nearest = std::min(nearest, length(smoke.position[i] - embers.position[j]));
            }
            AJFX_CHECK(nearest <= 4.0F * simulation.step_seconds() + 1.0e-4F);
            // Carried along a little: a third of the ember's speed, and a
            // puff of a still ember is never born.
            AJFX_CHECK(length(smoke.velocity[i]) >= 0.3F * 0.5F - 0.3F * simulation.step_seconds() * 0.3F);
        }
    }
    std::cout << "  " << while_moving << " puffs in " << moving_steps << " moving steps, " << while_still << " in "
              << still_steps << " still steps\n";
    AJFX_CHECK(while_moving > 100U);
    AJFX_CHECK(moving_steps > 30U && still_steps > 60U);
    AJFX_CHECK(while_still == 0U);
    // Shed smoke is the second module's, loose smoke the first's.
    const EmitterState& smoke = simulation.emitter(1U);
    for (std::size_t i = 0U; i < smoke.alive(); ++i) {
        AJFX_CHECK(spawn_source(smoke.spawn_index[i]) == 1U);
    }
}

// How many the trail sheds in a step, worked out from the embers as that
// step left them and the module's hash roll (detail::particle_births'
// contract), capped at `cap` in ember order.
std::uint64_t expected_births(const Effect& effect, const EmitterState& embers, std::uint64_t step,
                              std::uint32_t cap) {
    const Emitter& smoke = effect.emitters[1U - embers_of(effect)];
    const auto& trail = std::get<SpawnFromParticles>(smoke.spawn[1]);
    const float dt = 1.0F / static_cast<float>(effect.simulation_rate);
    const std::uint32_t seed = hash_mix(instance_seed, smoke.seed);
    const std::uint32_t key = hash_mix(static_cast<std::uint32_t>(step), static_cast<std::uint32_t>(step >> 32U));
    std::uint64_t total = 0U;
    for (std::size_t i = 0U; i < embers.alive() && total < cap; ++i) {
        const float speed = length(embers.velocity[i]);
        if (speed < trail.min_speed) {
            continue;
        }
        const float wanted = (trail.rate + trail.per_speed * speed) * dt;
        const float roll = particle_unit(seed, hash_mix(key, embers.spawn_index[i]), hash_mix(trail.id, 0x8000a71cU));
        const auto count = static_cast<std::uint64_t>(std::floor(wanted + roll));
        total += std::min<std::uint64_t>(count, cap - total);
    }
    return total;
}

void the_count_follows_the_hash_rounding_and_the_cap() {
    for (const std::uint32_t cap : {64U, 8U}) {
        const Effect effect = embers_and_smoke(cap);
        Simulation simulation = make(effect);
        std::uint64_t checked = 0U;
        std::uint64_t capped = 0U;
        double wanted = 0.0;
        std::uint64_t born_in_all = 0U;
        for (std::uint64_t tick = 1U; tick <= 90U; ++tick) {
            const std::uint64_t before = simulation.emitter(1U).spawned;
            simulation.advance_to(tick);
            const EmitterState& embers = simulation.emitter(0U);
            const std::uint64_t born = simulation.emitter(1U).spawned - before;
            AJFX_CHECK(born == expected_births(effect, embers, tick - 1U, cap));
            AJFX_CHECK(born <= cap);
            capped += born == cap ? 1U : 0U;
            for (std::size_t i = 0U; i < embers.alive(); ++i) {
                const float speed = length(embers.velocity[i]);
                if (speed >= 0.5F) {
                    wanted += 20.0 * speed / 60.0;
                }
            }
            born_in_all += born;
            ++checked;
        }
        std::cout << "  cap " << cap << ": " << born_in_all << " puffs in " << checked << " steps, " << capped
                  << " at the cap; " << wanted << " wanted by the rates\n";
        AJFX_CHECK(checked == 90U);
        if (cap == 8U) {
            AJFX_CHECK(capped > 20U);
        } else {
            // Rounding up or down by the roll keeps the mean: within a tenth.
            AJFX_CHECK(capped == 0U);
            AJFX_CHECK(std::fabs(static_cast<double>(born_in_all) - wanted) < 0.1 * wanted);
        }
    }
}

// ------------------------------------------------------------ determinism

void trails_rerun_and_seek_exactly() {
    const Effect effect = embers_and_smoke();
    Simulation a = make(effect);
    Simulation b = make(effect);
    for (const std::uint64_t tick : {1U, 40U, 90U, 240U}) {
        a.advance_to(tick);
        b.advance_to(tick);
        AJFX_CHECK(a.state() == b.state());
    }
    AJFX_CHECK(a.emitter(1U).spawned > 100U);
    CheckpointRing ring{30U, 8U};
    Simulation seeker = make(effect);
    for (const std::uint64_t target : {100U, 37U, 240U, 61U, 31U, 180U}) {
        ring.seek(seeker, target);
        Simulation straight = make(effect);
        straight.advance_to(target);
        AJFX_CHECK(seeker.tick() == target);
        AJFX_CHECK(seeker.state() == straight.state());
    }
    // A saved state round trip holds the smoke's counts exactly.
    const SimulationState state = must(read_state(write_state(a.state())));
    AJFX_CHECK(state == a.state());
}

void the_source_steps_first_whatever_the_order() {
    const Effect first = embers_and_smoke(64U, false);
    const Effect reversed = embers_and_smoke(64U, true);
    AJFX_CHECK(reversed.emitters[0].name == "smoke");
    const std::vector<std::size_t> order = step_order(reversed);
    AJFX_CHECK(order == std::vector<std::size_t>({1U, 0U}));
    AJFX_CHECK(step_order(first) == std::vector<std::size_t>({0U, 1U}));
    Simulation a = make(first);
    Simulation b = make(reversed);
    for (const std::uint64_t tick : {1U, 30U, 120U}) {
        a.advance_to(tick);
        b.advance_to(tick);
        AJFX_CHECK(a.emitter(0U) == b.emitter(1U));
        AJFX_CHECK(a.emitter(1U) == b.emitter(0U));
    }
    AJFX_CHECK(a.emitter(1U).spawned > 100U);
}

void adding_a_trail_leaves_the_rest_bit_identical() {
    const Effect with = embers_and_smoke(64U, false, 30.0F);
    Effect without = with;
    without.emitters[1].spawn.pop_back();
    AJFX_CHECK(validate(without).ok());
    Simulation a = make(with);
    Simulation b = make(without);
    for (const std::uint64_t tick : {6U, 60U, 120U}) {
        a.advance_to(tick);
        b.advance_to(tick);
        // The embers are untouched, and so is every loose puff: same spawn
        // indices, same draws, same places.
        AJFX_CHECK(a.emitter(0U) == b.emitter(0U));
        const EmitterState& x = a.emitter(1U);
        const EmitterState& y = b.emitter(1U);
        std::size_t loose = 0U;
        std::size_t j = 0U;
        for (std::size_t i = 0U; i < x.alive(); ++i) {
            if (spawn_source(x.spawn_index[i]) != 0U) {
                continue;
            }
            AJFX_CHECK(j < y.alive());
            AJFX_CHECK(x.spawn_index[i] == y.spawn_index[j] && x.position[i] == y.position[j] &&
                       x.age[i] == y.age[j]);
            ++j;
            ++loose;
        }
        AJFX_CHECK(j == y.alive());
        AJFX_CHECK(loose > 0U && x.alive() > loose);
    }
}

// ------------------------------------------------------------ packages, caches, refusals

void trails_round_trip_through_a_package() {
    const Effect effect = embers_and_smoke();
    const std::string json = effect_to_json(effect);
    AJFX_CHECK(json.find("\"spawn_from_particles\"") != std::string::npos);
    AJFX_CHECK(json.find("\"inherit\": 0.3") != std::string::npos || json.find("\"inherit\": 0.30000001") != std::string::npos);
    const Effect read = must(read_package(must(write_package(effect))));
    AJFX_CHECK(read == effect);
    // The smoke's digest follows the embers': what it sheds from is part of
    // what it does. The embers' own digest does not follow the smoke.
    Effect other = effect;
    std::get<Drag>(other.emitters[0].update[0]).coefficient = 3.0F;
    AJFX_CHECK(emitter_digest(effect, 1U) != emitter_digest(other, 1U));
    AJFX_CHECK(emitter_digest(effect, 0U) != emitter_digest(other, 0U));
    Effect retuned = effect;
    std::get<SpawnFromParticles>(retuned.emitters[1].spawn[1]).per_speed = 5.0F;
    AJFX_CHECK(emitter_digest(effect, 0U) == emitter_digest(retuned, 0U));
    AJFX_CHECK(emitter_digest(effect, 1U) != emitter_digest(retuned, 1U));
}

void impossible_trails_are_refused() {
    const Effect good = embers_and_smoke();
    AJFX_CHECK(validate(good).ok());
    const auto refused = [](const Effect& effect, std::string_view fragment) {
        const Result<bool> result = validate(effect);
        if (result.ok()) {
            std::cout << "  accepted, expecting " << fragment << '\n';
        } else if (result.error().message.find(fragment) == std::string::npos) {
            std::cout << "  " << result.error().message << "\n  expecting " << fragment << '\n';
        }
        AJFX_CHECK(!result.ok() && result.error().message.find(fragment) != std::string::npos);
    };
    {
        Effect e = good;
        std::get<SpawnFromParticles>(e.emitters[1].spawn[1]).emitter = "smoke";
        refused(e, "names its own emitter");
    }
    {
        Effect e = good;
        std::get<SpawnFromParticles>(e.emitters[1].spawn[1]).emitter = "nowhere";
        refused(e, "which this effect has none of");
    }
    {
        // Embers shedding from the smoke that sheds from them: a ring. The
        // embers get the room the cap asks for, so the ring is what fails.
        Effect e = good;
        e.emitters[0].capacity = 100000U;
        e.emitters[0].spawn.push_back(SpawnFromParticles{.id = 90U, .emitter = "smoke"});
        refused(e, "in a ring");
    }
    {
        // A longer ring, through a third emitter.
        Effect e = good;
        Emitter ash;
        ash.name = "ash";
        ash.capacity = 256U;
        ash.spawn.push_back(SpawnFromParticles{.id = 91U, .emitter = "smoke", .max_per_step = 1U});
        ash.init.push_back(Lifetime{.id = 92U});
        e.emitters.push_back(ash);
        AJFX_CHECK(validate(e).ok());
        AJFX_CHECK(step_order(e) == std::vector<std::size_t>({0U, 1U, 2U}));
        e.emitters[0].capacity = 100000U;
        e.emitters[0].spawn.push_back(SpawnFromParticles{.id = 93U, .emitter = "ash"});
        refused(e, "'ash', which sheds from 'smoke', which sheds from 'embers'");
    }
    {
        // Bodies are shed from by spawn_from_bodies.
        Effect e = good;
        e.emitters[0].update.push_back(RigidPhysics{.id = 94U, .shape = RigidShape::Sphere});
        refused(e, "'spawn_from_bodies' sheds from");
    }
    {
        // A rigid physics emitter bears no particles of its own.
        Effect e = good;
        e.emitters[1].update.push_back(RigidPhysics{.id = 95U, .shape = RigidShape::Sphere});
        refused(e, "rigid physics emitter's bodies are not born that way");
    }
    {
        Effect e = good;
        std::get<SpawnFromParticles>(e.emitters[1].spawn[1]).particles.kinds = {0U};
        refused(e, "names body kinds");
    }
    {
        Effect e = good;
        std::get<SpawnFromParticles>(e.emitters[1].spawn[1]).particles.sources = {3U};
        refused(e, "names spawn source 3");
    }
    {
        Effect e = good;
        std::get<SpawnFromParticles>(e.emitters[1].spawn[1]).per_speed = -1.0F;
        refused(e, "per_speed");
    }
    {
        Effect e = good;
        std::get<SpawnFromParticles>(e.emitters[1].spawn[1]).max_per_step = 0U;
        refused(e, "max_per_step");
    }
    {
        // The cap is what the capacity is proved against.
        Effect e = good;
        std::get<SpawnFromParticles>(e.emitters[1].spawn[1]).max_per_step = 4096U;
        refused(e, "particles alive but its capacity is");
    }
}

} // namespace

int main() {
    const ajfx::test::Case cases[] = {
        {"embers_leave_smoke_only_while_moving", embers_leave_smoke_only_while_moving},
        {"the_count_follows_the_hash_rounding_and_the_cap", the_count_follows_the_hash_rounding_and_the_cap},
        {"trails_rerun_and_seek_exactly", trails_rerun_and_seek_exactly},
        {"the_source_steps_first_whatever_the_order", the_source_steps_first_whatever_the_order},
        {"adding_a_trail_leaves_the_rest_bit_identical", adding_a_trail_leaves_the_rest_bit_identical},
        {"trails_round_trip_through_a_package", trails_round_trip_through_a_package},
        {"impossible_trails_are_refused", impossible_trails_are_refused},
    };
    return ajfx::test::run(cases);
}
