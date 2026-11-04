#include "check.hpp"

#include <ajfx/actions.hpp>
#include <ajfx/builder.hpp>
#include <ajfx/cache.hpp>
#include <ajfx/rigid.hpp>
#include <ajfx/rigid_runner.hpp>
#include <ajfx/simulation.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Actions on a running effect: bodies placed before the first step and
// dropped mid-run, pushed, steered and removed; particles painted in; the
// contracts those keep (bit-identical reruns, seeks that land where
// stepping did, serials that leave the other spawns alone, budgets refused
// rather than grown); the runner revising from a tick; and the cache
// keyed by the performance. Expectations are independent of the code under
// test: a placed body stands where it was put, a pushed one moves the way
// it was pushed, a stepped simulation is compared with a seeking one.
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

Simulation make(const Effect& effect, std::shared_ptr<const Cache> cache = nullptr) {
    SimulationOptions options;
    options.rigid = g_solver.get();
    options.cache = std::move(cache);
    return must(Simulation::create(effect, 5U, std::move(options)));
}

// A stage: boxes an artist puts down, drops, pushes and takes away, on a
// floor, sized 0.2 to 0.4 unless an action says otherwise. Nothing but the
// actions bears a body.
Effect stage(std::uint32_t max_alive = 32U, std::uint32_t max_per_step = 8U) {
    EffectBuilder builder{"stage"};
    builder.emitter("boxes")
        .capacity(64U)
        .seed(11U)
        .spawn(SpawnActions{.max_per_step = max_per_step, .max_alive = max_alive})
        .init(Lifetime{.min = 60.0F, .max = 60.0F})
        .init(ShapeLocation{.shape = Shape::Point, .center = Vec3{9.0F, 9.0F, 9.0F}})
        .init(Velocity{.speed_min = 0.0F, .speed_max = 0.0F})
        .init(InitSize{.min = 0.2F, .max = 0.4F})
        .init(InitTumble{.speed_min = 0.0F, .speed_max = 0.0F})
        .update(Gravity{})
        .update(RigidPhysics{.shape = RigidShape::Box, .friction = 0.6F, .restitution = 0.1F})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 0.6F});
    return must(builder.build());
}

// The stage with dust that is painted in: a particle emitter born of
// actions alone, each particle starting where its action says, spread by
// its init modules.
Effect stage_with_dust() {
    Effect effect = stage();
    EffectBuilder builder{"stage"};
    builder.emitter("dust")
        .capacity(2048U)
        .spawn(SpawnActions{.max_per_step = 16U})
        .init(Lifetime{.min = 2.0F, .max = 2.0F})
        .init(ShapeLocation{.shape = Shape::Point, .center = Vec3{0.0F, 0.5F, 0.0F}})
        .init(Velocity{.direction = Vec3{0.0F, 1.0F, 0.0F}, .speed_min = 1.0F, .speed_max = 1.0F, .spread = 0.0F})
        .init(InitSize{.min = 0.05F, .max = 0.05F});
    Effect dust = must(builder.build());
    effect.emitters.push_back(dust.emitters[0]);
    must(validate(effect));
    return effect;
}

Action spawn_at(std::uint64_t tick, std::uint32_t serial, Vec3 position, Vec3 velocity = {}, float size = 0.0F,
                std::uint64_t release = 0U) {
    Action action;
    action.kind = ActionKind::Spawn;
    action.tick = tick;
    action.serial = serial;
    action.position = position;
    action.velocity = velocity;
    action.size = size;
    action.release = release;
    return action;
}

Action push_at(std::uint64_t tick, std::uint32_t target, Vec3 push, Vec3 twist = {}) {
    Action action;
    action.kind = ActionKind::Push;
    action.tick = tick;
    action.target = target;
    action.push = push;
    action.twist = twist;
    return action;
}

Action remove_at(std::uint64_t tick, std::uint32_t target) {
    Action action;
    action.kind = ActionKind::Remove;
    action.tick = tick;
    action.target = target;
    return action;
}

Action steer_at(std::uint64_t tick, std::uint32_t target, Vec3 to) {
    Action action;
    action.kind = ActionKind::Steer;
    action.tick = tick;
    action.target = target;
    action.position = to;
    action.gain = 8.0F;
    action.max_speed = 12.0F;
    return action;
}

Action blast_at(std::uint64_t tick, Vec3 center, float radius, float power) {
    Action action;
    action.kind = ActionKind::Blast;
    action.tick = tick;
    action.position = center;
    action.radius = radius;
    action.power = power;
    return action;
}

// The boxes' action source is the first (only) spawn module.
std::uint32_t box_index(std::uint32_t serial) {
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

bool same_particles(const EmitterState& a, const EmitterState& b) {
    return a.position == b.position && a.orientation == b.orientation && a.velocity == b.velocity &&
           a.angular_velocity == b.angular_velocity && a.age == b.age && a.spawn_index == b.spawn_index &&
           a.size == b.size && a.color == b.color && a.spawned == b.spawned && a.refused == b.refused &&
           a.held_until == b.held_until;
}

// A performance over 240 steps: two boxes placed at the start (one held
// until tick 40), two dropped mid-run, one pushed, one steered for a while,
// one taken away.
std::shared_ptr<ActionList> performance() {
    auto list = std::make_shared<ActionList>();
    list->add("boxes", spawn_at(0U, 1U, Vec3{0.0F, 0.15F, 0.0F}, {}, 0.3F));
    list->add("boxes", spawn_at(0U, 2U, Vec3{0.0F, 2.0F, 0.0F}, {}, 0.3F, 40U));
    list->add("boxes", spawn_at(30U, 3U, Vec3{1.0F, 1.5F, 0.0F}, Vec3{-1.0F, 0.0F, 0.0F}));
    list->add("boxes", spawn_at(31U, 4U, Vec3{-1.0F, 1.5F, 0.5F}, Vec3{1.0F, 2.0F, 0.0F}, 0.25F));
    list->add("boxes", push_at(90U, box_index(1U), Vec3{0.0F, 4.0F, 1.0F}, Vec3{3.0F, 0.0F, 0.0F}));
    for (std::uint64_t tick = 120U; tick < 180U; ++tick) {
        list->add("boxes", steer_at(tick, box_index(3U), Vec3{0.0F, 1.0F, -1.0F}));
    }
    list->add("boxes", blast_at(150U, Vec3{0.0F, 0.0F, 0.0F}, 2.0F, 4.0F));
    list->add("boxes", remove_at(200U, box_index(4U)));
    return list;
}

// ------------------------------------------------------------ placing and dropping

void placed_bodies_stand_before_the_first_step() {
    const Effect effect = stage();
    auto list = std::make_shared<ActionList>();
    list->add("boxes", spawn_at(0U, 7U, Vec3{0.5F, 2.0F, -0.5F}, {}, 0.3F, 30U));
    list->add("boxes", spawn_at(0U, 8U, Vec3{-0.5F, 0.15F, 0.5F}, {}, 0.3F));
    Simulation simulation = make(effect);
    simulation.reset(nullptr, list.get());

    // Both stand at tick 0, exactly where they were put, at the size given.
    const EmitterState& s = simulation.emitter(0);
    AJFX_CHECK(s.alive() == 2U);
    AJFX_CHECK(s.spawn_index[0] == box_index(7U) && s.spawn_index[1] == box_index(8U));
    AJFX_CHECK(s.position[0] == Vec3{0.5F, 2.0F, -0.5F});
    AJFX_CHECK(s.position[1] == Vec3{-0.5F, 0.15F, 0.5F});
    AJFX_CHECK(s.size[0] == 0.3F && s.size[1] == 0.3F);
    AJFX_CHECK(s.held_until.size() == 2U && s.held_until[0] == 30U && s.held_until[1] == 0U);

    // The held one hangs in the air until its release; the other rests on
    // the floor from the start.
    simulation.advance_to(29U, nullptr, list.get());
    AJFX_CHECK(simulation.emitter(0).position[0] == Vec3{0.5F, 2.0F, -0.5F});
    AJFX_CHECK(std::fabs(simulation.emitter(0).position[1].y - 0.15F) < 0.02F);
    simulation.advance_to(60U, nullptr, list.get());
    AJFX_CHECK(simulation.emitter(0).position[0].y < 1.5F);

    // The first step does not bear them again.
    AJFX_CHECK(simulation.emitter(0).alive() == 2U && simulation.emitter(0).spawned == 2U);
}

void dropped_bodies_take_their_pose_and_motion() {
    const Effect effect = stage();
    auto list = std::make_shared<ActionList>();
    list->add("boxes", spawn_at(5U, 1U, Vec3{1.0F, 3.0F, 0.0F}, Vec3{0.0F, 0.0F, 2.0F}, 0.3F));
    Simulation simulation = make(effect);
    simulation.advance_to(5U, nullptr, list.get());
    AJFX_CHECK(simulation.emitter(0).alive() == 0U);
    simulation.step(nullptr, list.get());
    {
        // Born at the end of its step, exactly where it was dropped, moving
        // as it was thrown: not where the init modules would have put it.
        const EmitterState& s = simulation.emitter(0);
        AJFX_CHECK(s.alive() == 1U);
        AJFX_CHECK(s.spawn_index[0] == box_index(1U));
        AJFX_CHECK(s.position[0] == Vec3{1.0F, 3.0F, 0.0F});
        AJFX_CHECK(s.velocity[0] == Vec3{0.0F, 0.0F, 2.0F});
        AJFX_CHECK(s.size[0] == 0.3F);
    }
    simulation.step(nullptr, list.get());
    {
        // One step of falling, carried along z.
        const EmitterState& s = simulation.emitter(0);
        AJFX_CHECK(std::fabs(s.position[0].x - 1.0F) < 1.0e-4F);
        AJFX_CHECK(s.position[0].y < 3.0F && s.position[0].y > 2.9F);
        AJFX_CHECK(s.position[0].z > 0.02F && s.position[0].z < 0.05F);
        AJFX_CHECK(std::fabs(s.velocity[0].z - 2.0F) < 1.0e-3F);
    }
}

void a_spawn_without_a_size_takes_its_init_modules() {
    const Effect effect = stage();
    auto list = std::make_shared<ActionList>();
    list->add("boxes", spawn_at(2U, 1U, Vec3{0.0F, 1.0F, 0.0F}));
    list->add("boxes", spawn_at(2U, 2U, Vec3{1.0F, 1.0F, 0.0F}));
    Simulation simulation = make(effect);
    simulation.advance_to(3U, nullptr, list.get());
    const EmitterState& s = simulation.emitter(0);
    AJFX_CHECK(s.alive() == 2U);
    for (std::size_t i = 0U; i < 2U; ++i) {
        AJFX_CHECK(s.size[i] >= 0.2F && s.size[i] <= 0.4F);
    }
    // Two serials, two draws.
    AJFX_CHECK(s.size[0] != s.size[1]);
}

// ------------------------------------------------------------ hands

void a_push_throws_and_a_steer_carries() {
    const Effect effect = stage();
    auto list = std::make_shared<ActionList>();
    list->add("boxes", spawn_at(0U, 1U, Vec3{0.0F, 0.15F, 0.0F}, {}, 0.3F));
    list->add("boxes", push_at(30U, box_index(1U), Vec3{0.0F, 5.0F, 0.0F}, Vec3{0.0F, 0.0F, 6.0F}));
    Simulation simulation = make(effect);
    simulation.advance_to(30U, nullptr, list.get());
    const Vec3 resting = simulation.emitter(0).position[0];
    AJFX_CHECK(length(simulation.emitter(0).velocity[0]) < 0.05F);
    simulation.step(nullptr, list.get());
    // Thrown up and set spinning, in the step the push was stamped with.
    AJFX_CHECK(simulation.emitter(0).velocity[0].y > 4.0F);
    AJFX_CHECK(simulation.emitter(0).angular_velocity[0].z > 4.0F);
    simulation.advance_to(45U, nullptr, list.get());
    AJFX_CHECK(simulation.emitter(0).position[0].y > resting.y + 0.5F);

    // Steered every step towards a point in the air, it goes there and
    // hangs; let go, it falls.
    auto hand = std::make_shared<ActionList>();
    hand->add("boxes", spawn_at(0U, 1U, Vec3{0.0F, 0.15F, 0.0F}, {}, 0.3F));
    const Vec3 to{1.5F, 1.2F, -0.8F};
    for (std::uint64_t tick = 10U; tick < 130U; ++tick) {
        hand->add("boxes", steer_at(tick, box_index(1U), to));
    }
    Simulation held = make(effect);
    held.advance_to(130U, nullptr, hand.get());
    AJFX_CHECK(length(held.emitter(0).position[0] - to) < 0.15F);
    held.advance_to(190U, nullptr, hand.get());
    AJFX_CHECK(held.emitter(0).position[0].y < 0.5F);
}

void a_removed_body_leaves_in_its_step() {
    const Effect effect = stage();
    auto list = std::make_shared<ActionList>();
    list->add("boxes", spawn_at(0U, 1U, Vec3{0.0F, 0.15F, 0.0F}, {}, 0.3F));
    list->add("boxes", spawn_at(0U, 2U, Vec3{1.0F, 0.15F, 0.0F}, {}, 0.3F));
    list->add("boxes", remove_at(20U, box_index(1U)));
    Simulation simulation = make(effect);
    simulation.advance_to(20U, nullptr, list.get());
    AJFX_CHECK(simulation.emitter(0).alive() == 2U);
    simulation.step(nullptr, list.get());
    AJFX_CHECK(simulation.emitter(0).alive() == 1U);
    AJFX_CHECK(simulation.emitter(0).spawn_index[0] == box_index(2U));
    // Its slot is free for the next body.
    auto more = std::make_shared<ActionList>(*list);
    more->add("boxes", spawn_at(25U, 3U, Vec3{2.0F, 1.0F, 0.0F}));
    simulation.advance_to(26U, nullptr, more.get());
    AJFX_CHECK(simulation.emitter(0).alive() == 2U);
    AJFX_CHECK(simulation.emitter(0).body[1] == 0U);
}

void a_blast_throws_what_it_reaches() {
    const Effect effect = stage();
    auto list = std::make_shared<ActionList>();
    list->add("boxes", spawn_at(0U, 1U, Vec3{0.5F, 0.15F, 0.0F}, {}, 0.3F));
    list->add("boxes", spawn_at(0U, 2U, Vec3{-1.5F, 0.15F, 0.0F}, {}, 0.3F));
    list->add("boxes", spawn_at(0U, 3U, Vec3{0.0F, 0.15F, 5.0F}, {}, 0.3F));
    list->add("boxes", blast_at(30U, Vec3{0.0F, 0.15F, 0.0F}, 3.0F, 10.0F));
    Simulation simulation = make(effect);
    simulation.advance_to(30U, nullptr, list.get());
    for (std::size_t i = 0U; i < 3U; ++i) {
        AJFX_CHECK(length(simulation.emitter(0).velocity[i]) < 0.05F);
    }
    simulation.step(nullptr, list.get());
    const EmitterState& s = simulation.emitter(0);
    const Vec3 near = s.velocity[*find(s, box_index(1U))];
    const Vec3 far = s.velocity[*find(s, box_index(2U))];
    const Vec3 out = s.velocity[*find(s, box_index(3U))];
    // Out from the centre, as hard as the blast is where each stands:
    // 10 * (1 - 0.5 / 3) and 10 * (1 - 1.5 / 3), less a step of the floor.
    AJFX_CHECK(near.x > 7.0F && near.x < 8.4F);
    AJFX_CHECK(far.x < -4.0F && far.x > -5.1F);
    // Beyond the radius, nothing.
    AJFX_CHECK(length(out) < 0.05F);
    // A blast is part of the performance's digest.
    ActionList without = *list;
    without.remove_if("boxes", [](const Action& a) { return a.kind == ActionKind::Blast; });
    AJFX_CHECK(without.digest("boxes") != list->digest("boxes"));
    Action weaker = blast_at(30U, Vec3{0.0F, 0.15F, 0.0F}, 3.0F, 9.0F);
    ActionList other = without;
    other.add("boxes", weaker);
    AJFX_CHECK(other.digest("boxes") != list->digest("boxes"));
}

// ------------------------------------------------------------ contracts

void reruns_are_bit_identical_and_seeks_agree() {
    const Effect effect = stage();
    const std::shared_ptr<ActionList> list = performance();
    Simulation a = make(effect);
    Simulation b = make(effect);
    a.reset(nullptr, list.get());
    b.reset(nullptr, list.get());
    for (std::uint64_t tick = 0U; tick < 240U; ++tick) {
        a.step(nullptr, list.get());
        b.step(nullptr, list.get());
    }
    AJFX_CHECK(a.state() == b.state());
    AJFX_CHECK(a.emitter(0).alive() == 3U);

    CheckpointRing ring{25U, 6U};
    Simulation seeker = make(effect);
    for (const std::uint64_t target : {150U, 50U, 240U, 35U, 95U}) {
        ring.seek(seeker, target, nullptr, list.get());
        Simulation straight = make(effect);
        straight.reset(nullptr, list.get());
        straight.advance_to(target, nullptr, list.get());
        AJFX_CHECK(seeker.tick() == target);
        AJFX_CHECK(seeker.state() == straight.state());
    }
}

void editing_past_actions_clears_the_checkpoints_after_them() {
    const Effect effect = stage();
    auto list = performance();
    CheckpointRing ring{25U, 8U};
    Simulation seeker = make(effect);
    ring.seek(seeker, 200U, nullptr, list.get());
    AJFX_CHECK(ring.size() == 8U);

    // A push added at tick 60: everything from 60 on is another performance.
    auto edited = std::make_shared<ActionList>(*list);
    edited->add("boxes", push_at(60U, box_index(1U), Vec3{2.0F, 3.0F, 0.0F}));
    ring.clear_from(60U);
    AJFX_CHECK(ring.size() == 2U); // 25 and 50 stand (0 was the oldest, dropped); 75 and on went
    ring.seek(seeker, 200U, nullptr, edited.get());
    Simulation straight = make(effect);
    straight.reset(nullptr, edited.get());
    straight.advance_to(200U, nullptr, edited.get());
    AJFX_CHECK(seeker.state() == straight.state());

    // Tick 0 placed the bodies: editing there drops the lot.
    ring.clear_from(0U);
    AJFX_CHECK(ring.size() == 0U);
}

void serials_keep_the_other_spawns_as_they_were() {
    const Effect effect = stage();
    auto list = std::make_shared<ActionList>();
    list->add("boxes", spawn_at(2U, 1U, Vec3{0.0F, 1.0F, 0.0F}));
    list->add("boxes", spawn_at(4U, 2U, Vec3{1.0F, 1.0F, 0.0F}));
    list->add("boxes", spawn_at(6U, 3U, Vec3{2.0F, 1.0F, 0.0F}));
    Simulation with = make(effect);
    with.advance_to(8U, nullptr, list.get());
    const EmitterState all = with.emitter(0);
    AJFX_CHECK(all.alive() == 3U);

    // The first spawn taken away: the other two are the same bodies, the
    // same sizes, from the same draws.
    list->remove_if("boxes", [](const Action& a) { return a.serial == 1U; });
    Simulation without = make(effect);
    without.advance_to(8U, nullptr, list.get());
    const EmitterState& rest = without.emitter(0);
    AJFX_CHECK(rest.alive() == 2U);
    for (const std::uint32_t serial : {2U, 3U}) {
        const std::optional<std::size_t> before = find(all, box_index(serial));
        const std::optional<std::size_t> after = find(rest, box_index(serial));
        AJFX_CHECK(before && after);
        AJFX_CHECK(all.size[*before] == rest.size[*after]);
        AJFX_CHECK(all.base_color[*before] == rest.base_color[*after]);
    }
}

void spawns_past_the_budget_are_refused_and_counted() {
    const Effect effect = stage(2U, 8U);
    auto list = std::make_shared<ActionList>();
    for (std::uint32_t serial = 1U; serial <= 3U; ++serial) {
        list->add("boxes", spawn_at(1U, serial, Vec3{static_cast<float>(serial), 1.0F, 0.0F}));
    }
    Simulation simulation = make(effect);
    simulation.advance_to(2U, nullptr, list.get());
    AJFX_CHECK(simulation.emitter(0).alive() == 2U);
    AJFX_CHECK(simulation.emitter(0).spawned == 3U);
    AJFX_CHECK(simulation.emitter(0).refused == 1U);
    // Room again once one is taken away.
    list->add("boxes", remove_at(5U, box_index(1U)));
    list->add("boxes", spawn_at(6U, 9U, Vec3{4.0F, 1.0F, 0.0F}));
    simulation.advance_to(7U, nullptr, list.get());
    AJFX_CHECK(simulation.emitter(0).alive() == 2U);
    AJFX_CHECK(find(simulation.emitter(0), box_index(9U)).has_value());

    // The step's cap, and a serial too large for a spawn index.
    const Effect narrow = stage(32U, 1U);
    auto crowd = std::make_shared<ActionList>();
    crowd->add("boxes", spawn_at(1U, 1U, Vec3{0.0F, 1.0F, 0.0F}));
    crowd->add("boxes", spawn_at(1U, 2U, Vec3{1.0F, 1.0F, 0.0F}));
    crowd->add("boxes", spawn_at(2U, 1U << 24U, Vec3{2.0F, 1.0F, 0.0F}));
    Simulation one_a_step = make(narrow);
    one_a_step.advance_to(3U, nullptr, crowd.get());
    AJFX_CHECK(one_a_step.emitter(0).alive() == 1U);
    AJFX_CHECK(one_a_step.emitter(0).refused == 2U);
}

void impossible_action_modules_are_refused() {
    EffectBuilder twice{"twice"};
    twice.emitter("boxes")
        .capacity(8U)
        .spawn(SpawnActions{})
        .spawn(SpawnActions{})
        .init(Lifetime{.min = 1.0F, .max = 1.0F})
        .update(RigidPhysics{});
    AJFX_CHECK(!twice.build().ok());

    EffectBuilder wide{"wide"};
    wide.emitter("boxes")
        .capacity(8U)
        .spawn(SpawnActions{.max_alive = 9U})
        .init(Lifetime{.min = 1.0F, .max = 1.0F})
        .update(RigidPhysics{});
    AJFX_CHECK(!wide.build().ok());

    EffectBuilder none{"none"};
    none.emitter("boxes").capacity(8U).spawn(SpawnActions{.max_per_step = 0U}).init(Lifetime{.min = 1.0F, .max = 1.0F});
    AJFX_CHECK(!none.build().ok());
}

// ------------------------------------------------------------ particles

void painted_particles_start_where_their_actions_say() {
    const Effect effect = stage_with_dust();
    auto list = std::make_shared<ActionList>();
    list->add("dust", spawn_at(3U, 1U, Vec3{1.0F, 1.0F, 1.0F}, Vec3{2.0F, 0.0F, 0.0F}));
    list->add("dust", spawn_at(3U, 2U, Vec3{-1.0F, 1.0F, 1.0F}));
    Simulation simulation = make(effect);
    simulation.advance_to(3U, nullptr, list.get());
    AJFX_CHECK(simulation.emitter(1).alive() == 0U);
    simulation.step(nullptr, list.get());
    const EmitterState& s = simulation.emitter(1);
    AJFX_CHECK(s.alive() == 2U);
    AJFX_CHECK(s.spawn_index[0] == spawn_index_of(0U, 1U) && s.spawn_index[1] == spawn_index_of(0U, 2U));
    // The action's point plus the init modules' offset, moved for the
    // part of the step it lived (nothing: born at the step's end), thrown
    // by the action beside its own launch.
    AJFX_CHECK(std::fabs(s.position[0].x - 1.0F) < 1.0e-5F && std::fabs(s.position[0].y - 1.5F) < 1.0e-5F);
    AJFX_CHECK(std::fabs(s.position[1].x + 1.0F) < 1.0e-5F);
    AJFX_CHECK(std::fabs(s.velocity[0].x - 2.0F) < 1.0e-5F && std::fabs(s.velocity[0].y - 1.0F) < 1.0e-5F);
    AJFX_CHECK(s.velocity[1].x == 0.0F);
    // The boxes took nothing from the dust's actions.
    AJFX_CHECK(simulation.emitter(0).alive() == 0U);
}

// ------------------------------------------------------------ the runner

void the_runner_revises_from_the_tick_that_changed() {
    const Effect effect = stage();
    RigidRunner runner{*g_solver,
                       RigidRunnerOptions{.lead = 0U, .checkpoint_interval = 20U, .checkpoint_count = 4U, .turn = 5U}};
    auto first = performance();
    std::unique_ptr<RigidTrack> track = runner.track(effect, 5U, {}, nullptr, first);

    const auto reference = [&](std::uint64_t tick, const ActionProvider* actions) {
        Simulation simulation = make(effect);
        simulation.reset(nullptr, actions);
        simulation.advance_to(tick, nullptr, actions);
        return simulation.emitter(0);
    };
    const auto check_at = [&](std::uint64_t tick, const ActionProvider* actions) {
        std::shared_ptr<const RigidFrame> frame = track->frame(tick);
        if (!frame) {
            runner.wait_idle();
            frame = track->frame(tick);
        }
        AJFX_CHECK(frame != nullptr && frame->tick == tick);
        AJFX_CHECK(same_particles(frame->emitters[0], reference(tick, actions)));
    };

    // Lead 0: the tick asked for is all that is worked out.
    check_at(0U, first.get());
    check_at(100U, first.get());
    runner.wait_idle();
    AJFX_CHECK(track->frame(101U) == nullptr);
    check_at(101U, first.get());

    // A push appended at a tick not yet stepped: nothing is thrown away.
    auto second = std::make_shared<ActionList>(*first);
    second->add("boxes", push_at(102U, box_index(1U), Vec3{0.0F, 3.0F, 0.0F}));
    track->set_actions(second, 102U);
    runner.wait_idle();
    AJFX_CHECK(track->frame(101U) != nullptr);
    check_at(103U, second.get());
    check_at(160U, second.get());

    // An edit in the past: what stands before it is kept, the rest is
    // worked out again and matches a straight run of the new performance.
    auto third = std::make_shared<ActionList>(*second);
    third->add("boxes", push_at(50U, box_index(1U), Vec3{1.0F, 2.0F, 0.0F}));
    track->set_actions(third, 50U);
    check_at(60U, third.get());
    check_at(160U, third.get());
    check_at(40U, third.get());
    AJFX_CHECK(!track->error());

    // A lead again, for playback.
    track->set_lead(10U);
    check_at(170U, third.get());
    runner.wait_idle();
    AJFX_CHECK(track->frame(175U) != nullptr);
}

// ------------------------------------------------------------ the cache

void a_cache_is_keyed_by_its_performance() {
    const Effect effect = stage();
    const std::shared_ptr<ActionList> list = performance();
    BakeSettings settings;
    settings.frames = 240U;
    settings.actions = list;
    SimulationOptions options;
    options.rigid = g_solver.get();
    const Baked baked = must(bake(effect, 5U, options, settings));
    AJFX_CHECK(baked.report.identity.emitters.size() == 1U);
    AJFX_CHECK(baked.report.identity.emitters[0].actions != 0U);
    AJFX_CHECK(baked.report.identity == cache_identity(effect, {}, 5U, {}, {}, {}, list.get()));

    // Played back with the same performance, the cache holds what the
    // simulation did, body for body.
    auto cache = std::make_shared<const Cache>(must(Cache::open(baked.bytes, effect, 5U, {}, {}, {}, list.get())));
    Simulation played = make(effect, cache);
    Simulation straight = make(effect);
    straight.reset(nullptr, list.get());
    for (std::uint64_t tick = 0U; tick < 240U; ++tick) {
        AJFX_CHECK(played.emitter(0).position == straight.emitter(0).position);
        AJFX_CHECK(played.emitter(0).spawn_index == straight.emitter(0).spawn_index);
        played.step();
        straight.step(nullptr, list.get());
    }

    // Another performance, or none: the cache is stale.
    auto other = std::make_shared<ActionList>(*list);
    other->add("boxes", push_at(10U, box_index(1U), Vec3{0.0F, 1.0F, 0.0F}));
    Result<Cache> stale = Cache::open(baked.bytes, effect, 5U, {}, {}, {}, other.get());
    AJFX_CHECK(!stale.ok() && stale.error().code == ErrorCode::StaleCache);
    Result<Cache> bare = Cache::open(baked.bytes, effect, 5U);
    AJFX_CHECK(!bare.ok() && bare.error().code == ErrorCode::StaleCache);

    // An empty performance is no performance: baked with one, the cache
    // opens without any, as a host that never acted has none.
    BakeSettings unacted = settings;
    unacted.actions = std::make_shared<ActionList>();
    unacted.frames = 60U;
    const Baked quiet = must(bake(effect, 5U, options, unacted));
    AJFX_CHECK(quiet.report.identity.emitters[0].actions == 0U);
    AJFX_CHECK(Cache::open(quiet.bytes, effect, 5U).ok());

    // A state with placed bodies round-trips, hold and all.
    Simulation placed = make(effect);
    placed.reset(nullptr, list.get());
    placed.advance_to(20U, nullptr, list.get());
    const SimulationState state = placed.state();
    const SimulationState back = must(read_state(write_state(state)));
    AJFX_CHECK(back == state);
    AJFX_CHECK(back.emitters[0].held_until == std::vector<std::uint64_t>{0U, 40U});
}

// What a delivered effect plays with: the cache holds the performance it
// was baked with, as far as its steps read it, and the seed, so a host that
// has neither plays it as it was performed.
void a_cache_carries_its_performance_and_seed() {
    const Effect effect = stage();
    const std::shared_ptr<ActionList> list = performance();
    // An act after the bake's last step: the cache is not of it.
    auto longer = std::make_shared<ActionList>(*list);
    longer->add("boxes", push_at(300U, box_index(1U), Vec3{0.0F, 5.0F, 0.0F}));
    BakeSettings settings;
    settings.frames = 240U;
    settings.actions = longer;
    SimulationOptions options;
    options.rigid = g_solver.get();
    const Baked baked = must(bake(effect, 77U, options, settings));

    // Opened as baked: its own seed, and the acts its steps read, the late
    // one left out.
    auto cache = std::make_shared<const Cache>(must(Cache::open_as_baked(baked.bytes, effect)));
    AJFX_CHECK(cache->identity().instance_seed == 77U);
    AJFX_CHECK(cache->performance()->size() == list->size());
    AJFX_CHECK(cache->performance()->digest("boxes") == list->digest("boxes"));
    AJFX_CHECK(cache->performance()->actions("boxes").back().tick == 200U);

    // Its performance steps the effect as the original does.
    Simulation straight = must(Simulation::create(effect, 77U, SimulationOptions{.rigid = g_solver.get()}));
    straight.reset(nullptr, list.get());
    straight.advance_to(239U, nullptr, list.get());
    Simulation played = must(Simulation::create(effect, 77U, SimulationOptions{.rigid = g_solver.get(), .cache = cache}));
    played.reset(nullptr, cache->performance().get());
    played.advance_to(239U, nullptr, cache->performance().get());
    AJFX_CHECK(played.emitter(0).position == straight.emitter(0).position);
    AJFX_CHECK(played.emitter(0).spawn_index == straight.emitter(0).spawn_index);

    // Studio's check opens it with the whole performance: the late act does
    // not make it stale, an act inside its steps does, and so does another
    // seed.
    AJFX_CHECK(Cache::open(baked.bytes, effect, 77U, {}, {}, {}, longer.get()).ok());
    AJFX_CHECK(Cache::open(baked.bytes, effect, 77U, {}, {}, {}, list.get()).ok());
    auto inside = std::make_shared<ActionList>(*list);
    inside->add("boxes", push_at(239U, box_index(1U), Vec3{0.0F, 5.0F, 0.0F}));
    const Result<Cache> stale = Cache::open(baked.bytes, effect, 77U, {}, {}, {}, inside.get());
    AJFX_CHECK(!stale.ok() && stale.error().code == ErrorCode::StaleCache);
    const Result<Cache> moved = Cache::open(baked.bytes, effect, 5U, {}, {}, {}, list.get());
    AJFX_CHECK(!moved.ok() && moved.error().code == ErrorCode::StaleCache);

    // Played as baked against an effect whose bodies changed: refused, with
    // the emitter named.
    Effect heavier = effect;
    std::get<RigidPhysics>(heavier.emitters[0].update[1]).density *= 2.0F;
    const Result<Cache> changed = Cache::open_as_baked(baked.bytes, heavier);
    AJFX_CHECK(!changed.ok() && changed.error().code == ErrorCode::StaleCache);
    AJFX_CHECK(changed.error().message.find("boxes") != std::string::npos);

    // A cache baked with no performance carries none.
    BakeSettings quiet = settings;
    quiet.actions = nullptr;
    quiet.frames = 30U;
    const Cache bare = must(Cache::open_as_baked(must(bake(effect, 3U, options, quiet)).bytes, effect));
    AJFX_CHECK(bare.performance() != nullptr && bare.performance()->empty());
    AJFX_CHECK(bare.identity().instance_seed == 3U);
}

// A runner track given a cache steps nothing: each frame is what the cache
// holds at its tick, bodies and the particle layers born of them, whatever
// order the ticks are asked in.
void a_cached_track_reads_what_the_cache_holds() {
    Effect effect = stage();
    EffectBuilder builder{"stage"};
    builder.emitter("boxes").capacity(4U).update(RigidPhysics{});
    builder.emitter("smoke")
        .capacity(4096U)
        .seed(13U)
        .spawn(SpawnFromBodies{.emitter = "boxes", .min_speed = 0.3F, .per_speed = 20.0F, .max_per_step = 8U})
        .init(Lifetime{.min = 1.0F, .max = 1.5F})
        .init(InitSize{.min = 0.05F, .max = 0.1F})
        .update(Gravity{.acceleration = Vec3{0.0F, 0.5F, 0.0F}});
    effect.emitters.push_back(must(builder.build()).emitters[1]);
    must(validate(effect));
    const std::shared_ptr<ActionList> list = performance();
    BakeSettings settings;
    settings.frames = 240U;
    settings.actions = list;
    const Baked baked = must(bake(effect, 9U, SimulationOptions{.rigid = g_solver.get()}, settings));
    auto cache = std::make_shared<const Cache>(must(Cache::open_as_baked(baked.bytes, effect)));
    AJFX_CHECK(cache->mode(0U) == CacheMode::Rigid && cache->mode(1U) == CacheMode::Particles);

    RigidRunner runner{*g_solver, RigidRunnerOptions{.lead = 4U, .turn = 3U}};
    std::unique_ptr<RigidTrack> track = runner.track(effect, 9U, {}, nullptr, cache->performance(), cache);
    const auto frame_at = [&](std::uint64_t tick) {
        std::shared_ptr<const RigidFrame> frame = track->frame(tick);
        while (!frame) {
            AJFX_CHECK(!track->error().has_value());
            runner.wait_idle();
            frame = track->frame(tick);
        }
        return frame;
    };
    std::size_t smoke = 0U;
    for (const std::uint64_t tick : {0U, 1U, 2U, 100U, 101U, 150U, 30U, 31U, 239U, 400U}) {
        const std::shared_ptr<const RigidFrame> frame = frame_at(tick);
        AJFX_CHECK(frame->tick == tick);
        AJFX_CHECK(same_particles(frame->emitters[0], cache->emitter_at(0U, tick)));
        AJFX_CHECK(same_particles(frame->emitters[1], cache->emitter_at(1U, tick)));
        smoke = std::max(smoke, frame->emitters[1].alive());
    }
    std::cout << "  up to " << smoke << " puffs of smoke read from the cache\n";
    AJFX_CHECK(smoke > 0U);

    // A rigid layer the cache leaves live cannot be read from it.
    const Baked live = must(bake(effect, 9U, SimulationOptions{.rigid = g_solver.get()},
                                 BakeSettings{.frames = 10U, .modes = {CacheMode::Live, CacheMode::Live},
                                              .actions = list}));
    auto none = std::make_shared<const Cache>(must(Cache::open_as_baked(live.bytes, effect)));
    std::unique_ptr<RigidTrack> refused = runner.track(effect, 9U, {}, nullptr, nullptr, none);
    AJFX_CHECK(refused->error().has_value() && refused->error()->message.find("boxes") != std::string::npos);
}

} // namespace

int main() {
    g_solver = rigid::make_jolt_solver();
    const ajfx::test::Case cases[] = {
        {"placed_bodies_stand_before_the_first_step", placed_bodies_stand_before_the_first_step},
        {"dropped_bodies_take_their_pose_and_motion", dropped_bodies_take_their_pose_and_motion},
        {"a_spawn_without_a_size_takes_its_init_modules", a_spawn_without_a_size_takes_its_init_modules},
        {"a_push_throws_and_a_steer_carries", a_push_throws_and_a_steer_carries},
        {"a_removed_body_leaves_in_its_step", a_removed_body_leaves_in_its_step},
        {"a_blast_throws_what_it_reaches", a_blast_throws_what_it_reaches},
        {"reruns_are_bit_identical_and_seeks_agree", reruns_are_bit_identical_and_seeks_agree},
        {"editing_past_actions_clears_the_checkpoints_after_them",
         editing_past_actions_clears_the_checkpoints_after_them},
        {"serials_keep_the_other_spawns_as_they_were", serials_keep_the_other_spawns_as_they_were},
        {"spawns_past_the_budget_are_refused_and_counted", spawns_past_the_budget_are_refused_and_counted},
        {"impossible_action_modules_are_refused", impossible_action_modules_are_refused},
        {"painted_particles_start_where_their_actions_say", painted_particles_start_where_their_actions_say},
        {"the_runner_revises_from_the_tick_that_changed", the_runner_revises_from_the_tick_that_changed},
        {"a_cache_is_keyed_by_its_performance", a_cache_is_keyed_by_its_performance},
        {"a_cache_carries_its_performance_and_seed", a_cache_carries_its_performance_and_seed},
        {"a_cached_track_reads_what_the_cache_holds", a_cached_track_reads_what_the_cache_holds},
    };
    const int result = ajfx::test::run(cases);
    g_solver.reset();
    return result;
}
