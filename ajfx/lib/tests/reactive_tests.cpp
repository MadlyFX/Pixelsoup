#include "check.hpp"

#include <ajfx/actions.hpp>
#include <ajfx/builder.hpp>
#include <ajfx/cache.hpp>
#include <ajfx/rigid.hpp>
#include <ajfx/rigid_runner.hpp>
#include <ajfx/simulation.hpp>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// A world that answers: wind on bodies (BodyWind, RigidWorld::force) that
// carries light things off and leaves heavy ones be, gusting by the step;
// and a firm hand (ActionKind::Hold, RigidWorld::hold and let_go) that moves
// what it holds exactly where it is told, shoves what is in the way, and
// throws it as fast as it moved. Expectations are worked out by hand: where
// the hand was told to be, the speed of its path, which of two weights the
// same wind moves further; and replays are compared with themselves and
// with seeks.
namespace {

using namespace ajfx;

std::unique_ptr<RigidSolver> g_solver;

float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

template <class T>
T must(Result<T> result) {
    if (!result) {
        std::cout << "  " << result.error().message << '\n';
    }
    AJFX_CHECK(result.ok());
    return std::move(result).value();
}

Simulation make(const Effect& effect) {
    SimulationOptions options;
    options.rigid = g_solver.get();
    return must(Simulation::create(effect, 5U, std::move(options)));
}

// Boxes put down by actions on a floor. Kind 0 is the emitter's own (heavy,
// 2000 kg/m^3); kind 1 is light (20 kg/m^3, a cardboard box) and kind 2
// lighter still (10 kg/m^3), both otherwise alike. `winds` blow on them.
Effect stage(std::vector<BodyWind> winds = {}) {
    EffectBuilder builder{"stage"};
    auto& boxes = builder.emitter("boxes")
                      .capacity(32U)
                      .seed(3U)
                      .spawn(SpawnActions{.max_per_step = 8U, .max_alive = 16U})
                      .init(Lifetime{.min = 60.0F, .max = 60.0F})
                      .init(ShapeLocation{.shape = Shape::Point})
                      .init(Velocity{.speed_min = 0.0F, .speed_max = 0.0F})
                      .init(InitSize{.min = 0.3F, .max = 0.3F})
                      .init(InitTumble{.speed_min = 0.0F, .speed_max = 0.0F})
                      .update(Gravity{})
                      .update(RigidPhysics{.shape = RigidShape::Box,
                                           .density = 2000.0F,
                                           .friction = 0.6F,
                                           .restitution = 0.0F,
                                           .bodies = {RigidBodyKind{.shape = RigidShape::Box,
                                                                    .density = 20.0F,
                                                                    .friction = 0.6F,
                                                                    .restitution = 0.0F},
                                                      RigidBodyKind{.shape = RigidShape::Box,
                                                                    .density = 10.0F,
                                                                    .friction = 0.6F,
                                                                    .restitution = 0.0F}}})
                      .update(PlaneCollision{.restitution = 0.0F, .friction = 0.6F});
    for (BodyWind wind : winds) {
        boxes.update(wind);
    }
    return must(builder.build());
}

Action spawn_at(std::uint64_t tick, std::uint32_t serial, Vec3 position, std::uint32_t kind = 0U) {
    Action action;
    action.kind = ActionKind::Spawn;
    action.tick = tick;
    action.serial = serial;
    action.body = kind;
    action.position = position;
    action.size = 0.3F;
    return action;
}

Action hold_at(std::uint64_t tick, std::uint32_t target, Vec3 to) {
    Action action;
    action.kind = ActionKind::Hold;
    action.tick = tick;
    action.target = target;
    action.position = to;
    return action;
}

std::uint32_t box(std::uint32_t serial) {
    return spawn_index_of(0U, serial);
}

std::optional<std::size_t> find(const EmitterState& s, std::uint32_t spawn_index) {
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        if (s.spawn_index[i] == spawn_index) {
            return i;
        }
    }
    return std::nullopt;
}

Vec3 position_of(const Simulation& simulation, std::uint32_t serial) {
    const EmitterState& s = simulation.emitter(0);
    const std::optional<std::size_t> i = find(s, box(serial));
    AJFX_CHECK(i.has_value());
    return s.position[*i];
}

Vec3 velocity_of(const Simulation& simulation, std::uint32_t serial) {
    const EmitterState& s = simulation.emitter(0);
    const std::optional<std::size_t> i = find(s, box(serial));
    AJFX_CHECK(i.has_value());
    return s.velocity[*i];
}

// Where the hand is at `tick`: from (-1.5, 0.25, 0) at step 20 to
// (1.5, 0.25, 0) at step 80, evenly. 3 m in a second: 3 m/s along x, low
// enough that what it holds meets a box standing on the floor.
Vec3 hand_at(std::uint64_t tick) {
    const float t = static_cast<float>(tick - 20U) / 60.0F;
    return Vec3{-1.5F + 3.0F * t, 0.25F, 0.0F};
}

// A box held firmly and carried along x through a heavy box standing in its
// way, then let go at step 80.
std::shared_ptr<ActionList> carried() {
    auto list = std::make_shared<ActionList>();
    list->add("boxes", spawn_at(0U, 1U, Vec3{-1.5F, 0.15F, 0.0F}, 1U));
    list->add("boxes", spawn_at(0U, 2U, Vec3{0.0F, 0.15F, 0.0F}, 1U));
    for (std::uint64_t tick = 20U; tick <= 80U; ++tick) {
        list->add("boxes", hold_at(tick, box(1U), hand_at(tick)));
    }
    return list;
}

// ------------------------------------------------------------ a firm hand

void a_firm_hold_goes_exactly_where_it_is_told() {
    const Effect effect = stage();
    const std::shared_ptr<ActionList> list = carried();
    Simulation simulation = make(effect);
    simulation.reset(nullptr, list.get());
    simulation.advance_to(20U, nullptr, list.get());
    const Vec3 standing = position_of(simulation, 2U);
    for (std::uint64_t tick = 20U; tick <= 80U; ++tick) {
        simulation.step(nullptr, list.get());
        // Held at step t, it stands where the hand is at t by t + 1, through
        // the box in its way: nothing pushes a firm hand back.
        const Vec3 at = position_of(simulation, 1U);
        AJFX_CHECK(length(at - hand_at(tick)) < 1.0e-3F);
    }
    // The other box was in the way and is shoved along.
    AJFX_CHECK(position_of(simulation, 2U).x > standing.x + 0.3F);
    // Let go after step 80: moving at the speed the hand moved, 3 m/s
    // along x, and from then on the world's again: it settles on the floor
    // downstream of where it was let go.
    const Vec3 thrown = velocity_of(simulation, 1U);
    AJFX_CHECK(std::fabs(thrown.x - 3.0F) < 0.01F);
    simulation.step(nullptr, list.get());
    std::cout << "  a step after letting go it moves at " << velocity_of(simulation, 1U).x << " m/s\n";
    AJFX_CHECK(velocity_of(simulation, 1U).x > 0.5F);
    simulation.advance_to(200U, nullptr, list.get());
    std::cout << "  settled at " << position_of(simulation, 1U).x << ", " << position_of(simulation, 1U).y << '\n';
    AJFX_CHECK(std::fabs(position_of(simulation, 1U).y - 0.15F) < 0.02F);
    AJFX_CHECK(position_of(simulation, 1U).x > hand_at(80U).x);
}

void a_steer_is_stopped_where_a_hold_is_not() {
    // The same path steered rather than held: the box in the way stops it,
    // so it falls behind the hand. This is what a firm hand is for.
    const Effect effect = stage();
    auto steered = std::make_shared<ActionList>();
    steered->add("boxes", spawn_at(0U, 1U, Vec3{-1.5F, 0.15F, 0.0F}, 1U));
    steered->add("boxes", spawn_at(0U, 2U, Vec3{0.0F, 0.15F, 0.0F}, 0U));
    auto held = std::make_shared<ActionList>(*steered);
    for (std::uint64_t tick = 20U; tick <= 80U; ++tick) {
        Action steer = hold_at(tick, box(1U), hand_at(tick));
        steer.kind = ActionKind::Steer;
        steer.gain = 10.0F;
        steer.max_speed = 20.0F;
        steered->add("boxes", steer);
        held->add("boxes", hold_at(tick, box(1U), hand_at(tick)));
    }
    Simulation a = make(effect);
    a.reset(nullptr, steered.get());
    a.advance_to(81U, nullptr, steered.get());
    Simulation b = make(effect);
    b.reset(nullptr, held.get());
    b.advance_to(81U, nullptr, held.get());
    std::cout << "  steered ends " << length(position_of(a, 1U) - hand_at(80U)) << " m from the hand, held "
              << length(position_of(b, 1U) - hand_at(80U)) << " m\n";
    AJFX_CHECK(length(position_of(a, 1U) - hand_at(80U)) > 0.3F);
    AJFX_CHECK(length(position_of(b, 1U) - hand_at(80U)) < 1.0e-3F);
}

void holds_replay_and_seek_exactly() {
    const Effect effect = stage();
    const std::shared_ptr<ActionList> list = carried();
    Simulation a = make(effect);
    Simulation b = make(effect);
    a.reset(nullptr, list.get());
    b.reset(nullptr, list.get());
    a.advance_to(160U, nullptr, list.get());
    b.advance_to(160U, nullptr, list.get());
    AJFX_CHECK(a.state() == b.state());

    // Checkpoints every 7 steps land inside the hold, on the step after it
    // and on the release itself (77, 84; 81 is the step that lets go).
    CheckpointRing ring{7U, 32U};
    Simulation seeker = make(effect);
    for (const std::uint64_t target : {50U, 81U, 82U, 160U, 30U, 85U, 21U}) {
        ring.seek(seeker, target, nullptr, list.get());
        Simulation straight = make(effect);
        straight.reset(nullptr, list.get());
        straight.advance_to(target, nullptr, list.get());
        AJFX_CHECK(seeker.tick() == target);
        AJFX_CHECK(seeker.state() == straight.state());
    }
    // Restored from a checkpoint taken while it was held, it plays on as
    // the world that held it did.
    Simulation from = make(effect);
    from.reset(nullptr, list.get());
    from.advance_to(63U, nullptr, list.get());
    const SimulationState saved = from.state();
    Simulation restored = make(effect);
    restored.restore(saved);
    restored.advance_to(120U, nullptr, list.get());
    from.advance_to(120U, nullptr, list.get());
    AJFX_CHECK(restored.state() == from.state());
}

void a_hold_travels_in_a_cache() {
    const Effect effect = stage();
    const std::shared_ptr<ActionList> list = carried();
    BakeSettings settings;
    settings.frames = 120U;
    settings.actions = list;
    const Baked baked = must(bake(effect, 5U, SimulationOptions{.rigid = g_solver.get()}, settings));
    const Cache cache = must(Cache::open_as_baked(baked.bytes, effect));
    AJFX_CHECK(cache.performance()->digest("boxes") == list->digest("boxes"));
    std::size_t holds = 0U;
    for (const Action& action : cache.performance()->actions("boxes")) {
        holds += action.kind == ActionKind::Hold ? 1U : 0U;
    }
    AJFX_CHECK(holds == 61U);
}

// ------------------------------------------------------------ wind

void wind_carries_light_bodies_off_and_leaves_heavy_ones() {
    const Effect effect = stage({BodyWind{.id = 50U, .velocity = Vec3{12.0F, 0.0F, 0.0F}, .drag = 0.6F}});
    auto list = std::make_shared<ActionList>();
    list->add("boxes", spawn_at(0U, 1U, Vec3{0.0F, 0.15F, 0.0F}, 0U));  // 2000 kg/m^3: 54 kg
    list->add("boxes", spawn_at(0U, 2U, Vec3{0.0F, 0.15F, 2.0F}, 1U));  // 20 kg/m^3: 0.54 kg
    list->add("boxes", spawn_at(0U, 3U, Vec3{0.0F, 0.15F, -2.0F}, 2U)); // 10 kg/m^3: 0.27 kg
    Simulation simulation = make(effect);
    simulation.reset(nullptr, list.get());
    simulation.advance_to(120U, nullptr, list.get());
    // 12 m/s of air on 0.09 m^2 pushes 0.6 * 0.09 * 144 = 7.8 N: past the
    // light boxes' grip (0.6 * 0.54 kg * 9.81 = 3.2 N), nowhere near the
    // heavy one's (318 N).
    const float heavy = position_of(simulation, 1U).x;
    const float light = position_of(simulation, 2U).x;
    const float lighter = position_of(simulation, 3U).x;
    std::cout << "  after 2 s: heavy " << heavy << " m, light " << light << " m, lighter " << lighter << " m\n";
    AJFX_CHECK(std::fabs(heavy) < 0.01F);
    AJFX_CHECK(light > 1.0F);
    // The same force on half the mass carries it further.
    AJFX_CHECK(lighter > light);
    // Downwind, never against it.
    AJFX_CHECK(velocity_of(simulation, 2U).x > 0.0F && velocity_of(simulation, 2U).x <= 12.0F);
}

void wind_leaves_a_sleeping_heavy_body_asleep() {
    // A gentle wind on a heavy box at rest: it never wakes it, so the box
    // stays put to the bit and costs nothing.
    const Effect calm = stage();
    const Effect breezy = stage({BodyWind{.id = 50U, .velocity = Vec3{3.0F, 0.0F, 0.0F}, .drag = 0.6F}});
    auto list = std::make_shared<ActionList>();
    list->add("boxes", spawn_at(0U, 1U, Vec3{0.0F, 0.15F, 0.0F}, 0U));
    Simulation still = make(calm);
    Simulation blown = make(breezy);
    still.reset(nullptr, list.get());
    blown.reset(nullptr, list.get());
    still.advance_to(400U, nullptr, list.get());
    blown.advance_to(400U, nullptr, list.get());
    std::cout << "  heavy box at rest moved " << length(position_of(blown, 1U) - position_of(still, 1U))
              << " m in the breeze\n";
    AJFX_CHECK(length(position_of(blown, 1U) - position_of(still, 1U)) < 0.005F);
    AJFX_CHECK(length(velocity_of(blown, 1U)) < 1.0e-3F);
}

void gusts_rise_and_fall_by_the_step() {
    const BodyWind steady{.id = 7U, .velocity = Vec3{5.0F, 0.0F, 1.0F}};
    AJFX_CHECK(body_wind_at(steady, 0U, 60U) == steady.velocity);
    AJFX_CHECK(body_wind_at(steady, 12345U, 60U) == steady.velocity);
    const BodyWind gusty{.id = 7U, .velocity = Vec3{5.0F, 0.0F, 1.0F}, .gust = 0.4F, .gust_seconds = 1.0F};
    float lowest = 1.0e9F;
    float highest = 0.0F;
    for (std::uint64_t tick = 0U; tick < 600U; ++tick) {
        const Vec3 air = body_wind_at(gusty, tick, 60U);
        // The same way as the wind, its speed within 40% of it.
        AJFX_CHECK(std::fabs(air.z * 5.0F - air.x) < 1.0e-4F);
        AJFX_CHECK(air.x >= 5.0F * 0.6F - 1.0e-4F && air.x <= 5.0F * 1.4F + 1.0e-4F);
        lowest = std::min(lowest, air.x);
        highest = std::max(highest, air.x);
        // A function of the step: asked again, the same bits.
        AJFX_CHECK(body_wind_at(gusty, tick, 60U) == air);
        // Smooth: one step changes it little.
        if (tick > 0U) {
            AJFX_CHECK(std::fabs(body_wind_at(gusty, tick - 1U, 60U).x - air.x) < 0.2F);
        }
    }
    AJFX_CHECK(highest - lowest > 1.0F);
    // Another module's gusts are its own.
    const BodyWind other{.id = 8U, .velocity = gusty.velocity, .gust = 0.4F, .gust_seconds = 1.0F};
    AJFX_CHECK(body_wind_at(other, 90U, 60U) != body_wind_at(gusty, 90U, 60U));
}

void winds_replay_and_seek_exactly() {
    const Effect effect =
        stage({BodyWind{.id = 50U, .velocity = Vec3{10.0F, 0.0F, 2.0F}, .drag = 0.6F, .gust = 0.5F}});
    auto list = std::make_shared<ActionList>();
    for (std::uint32_t k = 0U; k < 6U; ++k) {
        list->add("boxes", spawn_at(0U, 1U + k, Vec3{0.4F * static_cast<float>(k), 0.15F, 0.0F}, k % 3U));
    }
    list->add("boxes", spawn_at(40U, 9U, Vec3{0.0F, 1.5F, 0.5F}, 1U));
    Simulation a = make(effect);
    Simulation b = make(effect);
    a.reset(nullptr, list.get());
    b.reset(nullptr, list.get());
    a.advance_to(200U, nullptr, list.get());
    b.advance_to(200U, nullptr, list.get());
    AJFX_CHECK(a.state() == b.state());
    CheckpointRing ring{16U, 16U};
    Simulation seeker = make(effect);
    for (const std::uint64_t target : {150U, 33U, 200U, 90U}) {
        ring.seek(seeker, target, nullptr, list.get());
        Simulation straight = make(effect);
        straight.reset(nullptr, list.get());
        straight.advance_to(target, nullptr, list.get());
        AJFX_CHECK(seeker.state() == straight.state());
    }
}

void the_runner_plays_wind_and_holds_as_the_simulation_does() {
    const Effect effect = stage({BodyWind{.id = 50U, .velocity = Vec3{12.0F, 0.0F, 0.0F}, .drag = 0.6F}});
    const std::shared_ptr<ActionList> list = carried();
    list->add("boxes", spawn_at(0U, 5U, Vec3{0.0F, 0.15F, 3.0F}, 2U));
    RigidRunner runner{*g_solver, RigidRunnerOptions{.lead = 4U, .turn = 3U}};
    std::unique_ptr<RigidTrack> track = runner.track(effect, 5U, {}, nullptr, list);
    for (const std::uint64_t tick : {10U, 50U, 81U, 120U}) {
        std::shared_ptr<const RigidFrame> frame = track->frame(tick);
        while (!frame) {
            AJFX_CHECK(!track->error().has_value());
            runner.wait_idle();
            frame = track->frame(tick);
        }
        Simulation straight = make(effect);
        straight.reset(nullptr, list.get());
        straight.advance_to(tick, nullptr, list.get());
        AJFX_CHECK(frame->emitters[0].position == straight.emitter(0).position);
        AJFX_CHECK(frame->emitters[0].velocity == straight.emitter(0).velocity);
    }
}

// ------------------------------------------------------------ refusals

void body_wind_is_refused_where_it_cannot_blow() {
    EffectBuilder builder{"sparks"};
    builder.emitter("sparks")
        .capacity(64U)
        .spawn(SpawnRate{.rate = 10.0F})
        .init(Lifetime{.min = 1.0F, .max = 1.0F})
        .update(BodyWind{.velocity = Vec3{1.0F, 0.0F, 0.0F}});
    const Result<Effect> particles = builder.build();
    AJFX_CHECK(!particles.ok());
    AJFX_CHECK(particles.error().message.find("body_wind") != std::string::npos);
    AJFX_CHECK(particles.error().message.find("rigid_physics") != std::string::npos);

    Effect negative = stage({BodyWind{.id = 50U, .drag = 0.6F}});
    std::get<BodyWind>(negative.emitters[0].update.back()).drag = -1.0F;
    const Result<bool> refused = validate(negative);
    AJFX_CHECK(!refused.ok() && refused.error().message.find("drag") != std::string::npos);
    Effect gusty = stage({BodyWind{.id = 50U, .gust = 0.5F}});
    std::get<BodyWind>(gusty.emitters[0].update.back()).gust = 1.5F;
    AJFX_CHECK(!validate(gusty).ok());
}

} // namespace

int main() {
    g_solver = rigid::make_jolt_solver();
    const ajfx::test::Case cases[] = {
        {"a_firm_hold_goes_exactly_where_it_is_told", a_firm_hold_goes_exactly_where_it_is_told},
        {"a_steer_is_stopped_where_a_hold_is_not", a_steer_is_stopped_where_a_hold_is_not},
        {"holds_replay_and_seek_exactly", holds_replay_and_seek_exactly},
        {"a_hold_travels_in_a_cache", a_hold_travels_in_a_cache},
        {"wind_carries_light_bodies_off_and_leaves_heavy_ones", wind_carries_light_bodies_off_and_leaves_heavy_ones},
        {"wind_leaves_a_sleeping_heavy_body_asleep", wind_leaves_a_sleeping_heavy_body_asleep},
        {"gusts_rise_and_fall_by_the_step", gusts_rise_and_fall_by_the_step},
        {"winds_replay_and_seek_exactly", winds_replay_and_seek_exactly},
        {"the_runner_plays_wind_and_holds_as_the_simulation_does",
         the_runner_plays_wind_and_holds_as_the_simulation_does},
        {"body_wind_is_refused_where_it_cannot_blow", body_wind_is_refused_where_it_cannot_blow},
    };
    const int result = ajfx::test::run(cases);
    g_solver.reset();
    return result;
}
