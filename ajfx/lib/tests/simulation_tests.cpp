#include "check.hpp"

#include <ajfx/builder.hpp>
#include <ajfx/presets.hpp>
#include <ajfx/simulation.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using namespace ajfx;

Simulation make(const Effect& effect, std::uint32_t seed = 0U) {
    Result<Simulation> simulation = Simulation::create(effect, seed);
    AJFX_CHECK(simulation.ok());
    return std::move(simulation).value();
}

Effect one_emitter(EmitterBuilder& (*configure)(EmitterBuilder&), std::uint32_t capacity = 10000U) {
    EffectBuilder builder{"test"};
    configure(builder.emitter("e").capacity(capacity));
    Result<Effect> effect = builder.build();
    AJFX_CHECK(effect.ok());
    return std::move(effect).value();
}

bool near(float a, float b, float tolerance) {
    return std::fabs(a - b) <= tolerance;
}

// Sparks' rate parameter ramps up and wraps every four seconds: a pure
// function of the step, as the seek contract requires.
class RampingRate final : public ParameterProvider {
public:
    void values_at(std::uint64_t tick, std::span<ParameterValue> values) const override {
        values[0][0] = static_cast<float>(tick % 240U) * 4.0F;
    }
};

class HugeRate final : public ParameterProvider {
public:
    void values_at(std::uint64_t, std::span<ParameterValue> values) const override { values[0][0] = 1.0e6F; }
};

void reruns_are_identical_and_seeds_differ() {
    for (const Effect& effect : {presets::sparks(), presets::smoke(), presets::confetti(), presets::debris()}) {
        Simulation a = make(effect, 5U);
        Simulation b = make(effect, 5U);
        Simulation c = make(effect, 6U);
        a.advance_to(300U);
        b.advance_to(300U);
        c.advance_to(300U);
        AJFX_CHECK(a.emitter(0).alive() > 0U);
        AJFX_CHECK(a.state() == b.state());
        AJFX_CHECK(a.emitter(0).position != c.emitter(0).position);
        AJFX_CHECK(a.emitter(0).refused == 0U);
    }
}

void seeking_lands_on_the_stepped_state() {
    const RampingRate ramp;
    const Effect effect = presets::sparks();
    Simulation seeker = make(effect, 9U);
    CheckpointRing ring{60U, 8U};
    for (const std::uint64_t target : {900U, 300U, 700U, 450U, 451U, 0U, 1000U}) {
        ring.seek(seeker, target, &ramp);
        Simulation stepped = make(effect, 9U);
        stepped.advance_to(target, &ramp);
        AJFX_CHECK(seeker.tick() == target);
        AJFX_CHECK(seeker.state() == stepped.state());
    }
    AJFX_CHECK(ring.size() == 8U);
}

void rate_spawning_is_exact() {
    Simulation half = make(one_emitter([](EmitterBuilder& e) -> EmitterBuilder& {
        return e.spawn(SpawnRate{.rate = 30.0F}).init(Lifetime{.min = 100.0F, .max = 100.0F});
    }));
    half.advance_to(1U);
    AJFX_CHECK(half.emitter(0).spawned == 0U);
    half.advance_to(2U);
    AJFX_CHECK(half.emitter(0).spawned == 1U);
    // Born exactly at the end of the step.
    AJFX_CHECK(half.emitter(0).age[0] == 0.0F);
    half.advance_to(60U);
    AJFX_CHECK(half.emitter(0).spawned == 30U);

    // Ten births a step are spread evenly through it.
    Simulation dense = make(one_emitter([](EmitterBuilder& e) -> EmitterBuilder& {
        return e.spawn(SpawnRate{.rate = 600.0F}).init(Lifetime{.min = 10.0F, .max = 10.0F});
    }));
    dense.step();
    const EmitterState& s = dense.emitter(0);
    AJFX_CHECK(s.alive() == 10U);
    const float dt = dense.step_seconds();
    for (std::size_t i = 0U; i < 10U; ++i) {
        AJFX_CHECK(near(s.age[i], dt * static_cast<float>(9U - i) / 10.0F, 1.0e-7F));
        AJFX_CHECK(s.spawn_index[i] == i);
    }
}

void bursts_fire_on_their_step() {
    Simulation once = make(one_emitter([](EmitterBuilder& e) -> EmitterBuilder& {
        return e.spawn(SpawnBurst{.time = 0.5F, .count = 100U}).init(Lifetime{.min = 10.0F, .max = 10.0F});
    }));
    once.advance_to(30U); // steps 0..29 cover [0, 0.5)
    AJFX_CHECK(once.emitter(0).alive() == 0U);
    once.advance_to(31U);
    AJFX_CHECK(once.emitter(0).alive() == 100U);
    AJFX_CHECK(near(once.emitter(0).age[0], 1.0F / 60.0F, 1.0e-6F));
    once.advance_to(600U);
    AJFX_CHECK(once.emitter(0).spawned == 100U);

    Simulation limited = make(one_emitter([](EmitterBuilder& e) -> EmitterBuilder& {
        return e.spawn(SpawnBurst{.time = 0.0F, .count = 100U, .interval = 0.25F, .cycles = 3U})
            .init(Lifetime{.min = 10.0F, .max = 10.0F});
    }));
    limited.advance_to(600U);
    AJFX_CHECK(limited.emitter(0).spawned == 300U);

    Simulation forever = make(one_emitter([](EmitterBuilder& e) -> EmitterBuilder& {
        return e.spawn(SpawnBurst{.time = 0.0F, .count = 100U, .interval = 0.25F, .cycles = 0U})
            .init(Lifetime{.min = 0.1F, .max = 0.1F});
    }));
    forever.advance_to(60U); // bursts at 0, 0.25, 0.5 and 0.75
    AJFX_CHECK(forever.emitter(0).spawned == 400U);
    forever.advance_to(61U); // and 1.0
    AJFX_CHECK(forever.emitter(0).spawned == 500U);
}

void gravity_integrates_semi_implicitly() {
    Simulation falling = make(one_emitter([](EmitterBuilder& e) -> EmitterBuilder& {
        return e.spawn(SpawnBurst{.count = 1U})
            .init(Lifetime{.min = 100.0F, .max = 100.0F})
            .update(Gravity{.acceleration = Vec3{0.0F, -10.0F, 0.0F}});
    }));
    falling.advance_to(60U); // born in step 0, then 59 steps of falling
    const double dt = 1.0 / 60.0;
    const double k = 59.0;
    const EmitterState& s = falling.emitter(0);
    AJFX_CHECK(near(s.velocity[0].y, static_cast<float>(-10.0 * k * dt), 1.0e-4F));
    AJFX_CHECK(near(s.position[0].y, static_cast<float>(-10.0 * dt * dt * k * (k + 1.0) / 2.0), 1.0e-4F));
    AJFX_CHECK(s.position[0].x == 0.0F && s.position[0].z == 0.0F);
}

void forces_follow_their_curves_over_life() {
    // Twice the acceleration all life long: twice the speed.
    Simulation doubled = make(one_emitter([](EmitterBuilder& e) -> EmitterBuilder& {
        return e.spawn(SpawnBurst{.count = 1U})
            .init(Lifetime{.min = 100.0F, .max = 100.0F})
            .update(Gravity{.acceleration = Vec3{0.0F, -10.0F, 0.0F}, .over_life = {{0.0F, 2.0F}, {1.0F, 2.0F}}});
    }));
    doubled.advance_to(60U);
    AJFX_CHECK(near(doubled.emitter(0).velocity[0].y, static_cast<float>(-20.0 * 59.0 / 60.0), 1.0e-4F));
    // A curve at zero from the start: no push at all.
    Simulation still = make(one_emitter([](EmitterBuilder& e) -> EmitterBuilder& {
        return e.spawn(SpawnBurst{.count = 1U})
            .init(Lifetime{.min = 100.0F, .max = 100.0F})
            .update(Gravity{.over_life = {{0.0F, 0.0F}, {0.5F, 0.0F}, {1.0F, 1.0F}}})
            .update(CurlNoise{.strength = 5.0F, .over_life = {{0.0F, 0.0F}, {0.5F, 0.0F}, {1.0F, 1.0F}}});
    }));
    still.advance_to(60U);
    AJFX_CHECK(still.emitter(0).velocity[0].y == 0.0F && still.emitter(0).velocity[0].x == 0.0F);
}

void drifting_turbulence_moves_its_field() {
    // Three particles born in step 0, at rest, swirled by step 1 (which
    // starts at 1/60 s): one in a field drifting up, one in a still field at
    // the same place, and one in the still field where the drifting field
    // has carried the same swirl. The last pair feel the same push.
    const Vec3 at{0.3F, 0.2F, -0.1F};
    const Vec3 drift{0.0F, 1.5F, 0.0F};
    const Vec3 carried = at - drift * static_cast<float>(1.0 / 60.0);
    const auto swirled = [](Vec3 position, Vec3 field_drift) {
        EffectBuilder builder{"swirl"};
        builder.emitter("e")
            .capacity(1U)
            .spawn(SpawnBurst{.count = 1U})
            .init(Lifetime{.min = 100.0F, .max = 100.0F})
            .init(ShapeLocation{.shape = Shape::Point, .center = position})
            .update(CurlNoise{.strength = 4.0F, .frequency = 2.0F, .seed = 3U, .drift = field_drift});
        Simulation simulation = make(builder.build().value());
        simulation.advance_to(2U);
        return simulation.emitter(0).velocity[0];
    };
    const Vec3 drifting = swirled(at, drift);
    const Vec3 still = swirled(carried, Vec3{});
    AJFX_CHECK(drifting.x == still.x && drifting.y == still.y && drifting.z == still.z);
    AJFX_CHECK(dot(drifting, drifting) > 0.0F);
    const Vec3 unmoved = swirled(at, Vec3{});
    AJFX_CHECK(unmoved.x != drifting.x || unmoved.y != drifting.y || unmoved.z != drifting.z);
}

void converge_pulls_towards_its_axis_below_its_height() {
    // One particle a metre out at the base, one a metre out above the
    // height where the pull has faded.
    EffectBuilder builder{"converge"};
    for (const float y : {0.0F, 3.0F}) {
        builder.emitter(y == 0.0F ? "low" : "high")
            .capacity(1U)
            .spawn(SpawnBurst{.count = 1U})
            .init(Lifetime{.min = 100.0F, .max = 100.0F})
            .init(ShapeLocation{.shape = Shape::Point, .center = Vec3{1.0F, y, 0.0F}})
            .update(Converge{.strength = 6.0F, .height = 2.0F});
    }
    Simulation simulation = make(builder.build().value());
    simulation.advance_to(20U);
    const Vec3 low = simulation.emitter(0).position[0];
    const Vec3 high = simulation.emitter(1).position[0];
    AJFX_CHECK(low.x < 0.9F && low.x > 0.0F && low.y == 0.0F && low.z == 0.0F);
    AJFX_CHECK(high.x == 1.0F && high.y == 3.0F);
}

void spin_turns_sprites_and_slows() {
    // A quarter turn a second, one way only: the turn is the spin times the
    // age, newborn's part step included.
    Simulation steady = make(one_emitter([](EmitterBuilder& e) -> EmitterBuilder& {
        return e.spawn(SpawnBurst{.count = 1U})
            .init(Lifetime{.min = 100.0F, .max = 100.0F})
            .init(InitSpin{.speed_min = 90.0F, .speed_max = 90.0F, .both_ways = false});
    }));
    steady.advance_to(60U);
    const EmitterState& s = steady.emitter(0);
    const float quarter = 3.14159265F / 2.0F;
    AJFX_CHECK(near(s.spin[0], quarter, 1.0e-6F));
    AJFX_CHECK(near(s.rotation[0], quarter * s.age[0], 1.0e-4F));
    // With drag, it loses that share of itself each second, as Drag does.
    Simulation slowing = make(one_emitter([](EmitterBuilder& e) -> EmitterBuilder& {
        return e.spawn(SpawnBurst{.count = 1U})
            .init(Lifetime{.min = 100.0F, .max = 100.0F})
            .init(InitSpin{.speed_min = 90.0F, .speed_max = 90.0F, .both_ways = false, .drag = 1.0F});
    }));
    slowing.advance_to(60U);
    AJFX_CHECK(near(slowing.emitter(0).spin[0], quarter * static_cast<float>(std::pow(1.0 / (1.0 + 1.0 / 60.0), 59.0)),
                    1.0e-5F));
    // Both ways round: some turn each way.
    Simulation both = make(one_emitter([](EmitterBuilder& e) -> EmitterBuilder& {
        return e.spawn(SpawnBurst{.count = 64U})
            .init(Lifetime{.min = 100.0F, .max = 100.0F})
            .init(InitSpin{.speed_min = 30.0F, .speed_max = 60.0F});
    }));
    both.advance_to(2U);
    std::size_t backwards = 0U;
    for (const float spin : both.emitter(0).spin) {
        backwards += spin < 0.0F ? 1U : 0U;
    }
    AJFX_CHECK(backwards > 16U && backwards < 48U);
}

void particles_die_in_order() {
    Simulation steady = make(one_emitter([](EmitterBuilder& e) -> EmitterBuilder& {
        return e.spawn(SpawnRate{.rate = 60.0F}).init(Lifetime{.min = 0.25F, .max = 0.5F});
    }));
    steady.advance_to(600U);
    const EmitterState& s = steady.emitter(0);
    AJFX_CHECK(s.spawned == 600U);
    AJFX_CHECK(s.alive() >= 15U && s.alive() <= 31U);
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        AJFX_CHECK(s.age[i] < s.lifetime[i]);
        AJFX_CHECK(s.lifetime[i] >= 0.25F && s.lifetime[i] <= 0.5F);
        AJFX_CHECK(i == 0U || s.spawn_index[i] > s.spawn_index[i - 1U]);
    }
}

void plane_collision_holds_or_kills() {
    const auto thrown_down = [](bool kill) {
        EffectBuilder builder{"floor"};
        builder.emitter("e")
            .spawn(SpawnRate{.rate = 120.0F})
            .init(Lifetime{.min = 3.0F, .max = 3.0F})
            .init(ShapeLocation{.shape = Shape::Point, .center = Vec3{0.0F, 1.0F, 0.0F}})
            .init(Velocity{.direction = Vec3{0.2F, -1.0F, 0.0F}, .speed_min = 5.0F, .speed_max = 5.0F})
            .update(Gravity{})
            .update(PlaneCollision{.restitution = 0.0F, .kill = kill});
        return make(builder.build().value());
    };

    Simulation held = thrown_down(false);
    held.advance_to(120U);
    AJFX_CHECK(held.emitter(0).alive() == 240U);
    for (const Vec3& p : held.emitter(0).position) {
        AJFX_CHECK(p.y >= -1.0e-6F);
    }

    Simulation killed = thrown_down(true);
    killed.advance_to(120U);
    // Each particle reaches the floor within a few steps of birth.
    AJFX_CHECK(killed.emitter(0).spawned == 240U);
    AJFX_CHECK(killed.emitter(0).alive() < 40U);
    for (const Vec3& p : killed.emitter(0).position) {
        AJFX_CHECK(p.y >= 0.0F);
    }
}

// Particles flung in every direction from the origin at 3 units a second,
// under one collider.
Simulation flung(UpdateModule collider) {
    EffectBuilder builder{"flung"};
    builder.emitter("e")
        .capacity(400U)
        .spawn(SpawnRate{.rate = 120.0F})
        .init(Lifetime{.min = 3.0F, .max = 3.0F})
        .init(ShapeLocation{.shape = Shape::Point})
        .init(Velocity{.speed_min = 3.0F, .speed_max = 3.0F, .spread = 180.0F})
        .update(std::move(collider));
    Simulation simulation = make(builder.build().value());
    simulation.advance_to(180U);
    return simulation;
}

void spheres_and_boxes_hold_particles_in_or_out() {
    const float slack = 1.0e-5F;
    // Inside a sphere of radius 1: nothing escapes, though most have tried.
    const Simulation ball = flung(SphereCollision{.radius = 1.0F, .inside = true, .restitution = 0.8F});
    AJFX_CHECK(ball.emitter(0).alive() > 300U);
    for (const Vec3& p : ball.emitter(0).position) {
        AJFX_CHECK(dot(p, p) <= (1.0F + slack) * (1.0F + slack));
    }
    // Outside a sphere of radius 1 at (2, 0, 0): nothing inside it.
    const Simulation rock = flung(SphereCollision{.center = Vec3{2.0F, 0.0F, 0.0F}, .radius = 1.0F});
    for (const Vec3& p : rock.emitter(0).position) {
        const Vec3 d = p - Vec3{2.0F, 0.0F, 0.0F};
        AJFX_CHECK(dot(d, d) >= (1.0F - slack) * (1.0F - slack));
    }
    // Inside a box half a unit by one by two: every axis held.
    const Simulation crate = flung(BoxCollision{.half_size = Vec3{0.5F, 1.0F, 2.0F}, .inside = true});
    for (const Vec3& p : crate.emitter(0).position) {
        AJFX_CHECK(std::fabs(p.x) <= 0.5F + slack && std::fabs(p.y) <= 1.0F + slack && std::fabs(p.z) <= 2.0F + slack);
    }
    // Outside a box beside the origin: none within it.
    const Simulation wall = flung(BoxCollision{.center = Vec3{0.0F, 2.0F, 0.0F}, .half_size = Vec3{3.0F, 0.5F, 3.0F}});
    for (const Vec3& p : wall.emitter(0).position) {
        const bool within = std::fabs(p.x) < 3.0F - slack && std::fabs(p.y - 2.0F) < 0.5F - slack &&
                            std::fabs(p.z) < 3.0F - slack;
        AJFX_CHECK(!within);
    }
    // Killing instead: the escapees are gone, not held.
    const Simulation culled = flung(SphereCollision{.radius = 1.0F, .inside = true, .kill = true});
    AJFX_CHECK(culled.emitter(0).alive() < 60U);
    // Shapes must have size.
    EffectBuilder flat{"flat"};
    flat.emitter("e").update(BoxCollision{.half_size = Vec3{1.0F, 0.0F, 1.0F}});
    AJFX_CHECK(!flat.build().ok());
}

// A sphere of radius 1 at (2, 0, 0), as a field 0.1 apart around it.
FieldSet ball_field() {
    return FieldSet{{"ball", make_distance_field(41U, 41U, 41U, Vec3{0.0F, -2.0F, -2.0F}, 0.1F, [](Vec3 p) {
                                 const Vec3 d = p - Vec3{2.0F, 0.0F, 0.0F};
                                 return std::sqrt(dot(d, d)) - 1.0F;
                             }).value()}};
}

void distance_fields_hold_particles_out_or_in() {
    const auto run = [](bool inside, Vec3 from) {
        EffectBuilder builder{"field"};
        builder.emitter("e")
            .capacity(400U)
            .spawn(SpawnRate{.rate = 120.0F})
            .init(Lifetime{.min = 3.0F, .max = 3.0F})
            .init(ShapeLocation{.shape = Shape::Point, .center = from})
            .init(Velocity{.speed_min = 3.0F, .speed_max = 3.0F, .spread = 180.0F})
            .update(SdfCollision{.field = "ball", .inside = inside, .restitution = 0.6F});
        Simulation simulation = Simulation::create(builder.build().value(), 0U, ball_field()).value();
        simulation.advance_to(180U);
        return simulation;
    };
    const FieldSet ball = ball_field();
    const DistanceField& field = ball.begin()->second;
    // Thrown at the ball from beside it: none end up inside, to the field's
    // own resolution.
    const Simulation around = run(false, Vec3{0.5F, 0.0F, 0.0F});
    for (const Vec3& p : around.emitter(0).position) {
        if (const std::optional<FieldSample> sample = sample_field(field, p)) {
            AJFX_CHECK(sample->distance >= -0.02F);
        }
    }
    // Thrown from its centre, held in: none end up outside.
    const Simulation held = run(true, Vec3{2.0F, 0.0F, 0.0F});
    AJFX_CHECK(held.emitter(0).alive() > 300U);
    for (const Vec3& p : held.emitter(0).position) {
        const std::optional<FieldSample> sample = sample_field(field, p);
        AJFX_CHECK(sample && sample->distance <= 0.02F);
    }
}

// Forty chips, 0.1 to 0.3 units across, thrown up and tumbling onto a
// floor that bounces and grips them.
Simulation debris(float rest_speed) {
    EffectBuilder builder{"debris"};
    builder.emitter("e")
        .capacity(64U)
        .spawn(SpawnBurst{.count = 40U})
        .init(Lifetime{.min = 20.0F, .max = 20.0F})
        .init(ShapeLocation{.shape = Shape::Sphere, .center = Vec3{0.0F, 1.0F, 0.0F}, .radius_min = 0.3F, .radius_max = 0.3F})
        .init(Velocity{.speed_min = 2.0F, .speed_max = 5.0F, .spread = 60.0F})
        .init(InitSize{.min = 0.1F, .max = 0.3F})
        .init(InitTumble{.speed_min = 90.0F, .speed_max = 720.0F})
        .update(Gravity{})
        .update(RigidBody{.angular_drag = 5.0F, .rest_speed = rest_speed})
        .update(PlaneCollision{.restitution = 0.4F, .friction = 0.8F});
    return make(builder.build().value());
}

float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

void debris_settles_on_the_floor() {
    Simulation settled = debris(0.2F);
    settled.advance_to(600U);
    const EmitterState rest = settled.emitter(0);
    AJFX_CHECK(rest.alive() == 40U);
    for (std::size_t i = 0U; i < rest.alive(); ++i) {
        // Resting on the floor as a ball half its size across: on it, not
        // in it, and not bouncing.
        AJFX_CHECK(rest.position[i].y == rest.size[i] * 0.5F);
        AJFX_CHECK(rest.velocity[i].y == 0.0F);
        // Its roll has died away.
        AJFX_CHECK(length(rest.velocity[i]) < 1.0e-3F && length(rest.angular_velocity[i]) < 1.0e-2F);
        const Quat q = rest.orientation[i];
        AJFX_CHECK(near(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w, 1.0F, 1.0e-5F));
    }
    // A step on, nothing has risen or sunk.
    settled.step();
    for (std::size_t i = 0U; i < rest.alive(); ++i) {
        AJFX_CHECK(settled.emitter(0).position[i].y == rest.position[i].y);
    }
    // With no rest speed every contact bounces a little, for ever: each step
    // ends with the chips rising.
    Simulation buzzing = debris(0.0F);
    buzzing.advance_to(600U);
    for (const Vec3& v : buzzing.emitter(0).velocity) {
        AJFX_CHECK(v.y > 0.0F);
    }
}

// One ball 0.2 across on the floor, sliding along X at `speed` or dropped
// spinning, under a floor that grips fully.
Simulation ball(float speed, float spin) {
    EffectBuilder builder{"ball"};
    builder.emitter("e")
        .spawn(SpawnBurst{.count = 1U})
        .init(Lifetime{.min = 10.0F, .max = 10.0F})
        .init(ShapeLocation{.shape = Shape::Point, .center = Vec3{0.0F, 0.1F, 0.0F}})
        .init(Velocity{.direction = Vec3{1.0F, 0.0F, 0.0F}, .speed_min = speed, .speed_max = speed})
        .init(InitSize{.min = 0.2F, .max = 0.2F})
        .init(InitTumble{.speed_min = spin, .speed_max = spin, .random_orientation = false})
        .update(Gravity{})
        .update(RigidBody{.angular_drag = 0.0F})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 1.0F});
    return make(builder.build().value());
}

// One flat chip like the ball above, on a floor of the given friction.
Simulation chip(float speed, float spin, float friction) {
    EffectBuilder builder{"chip"};
    builder.emitter("e")
        .spawn(SpawnBurst{.count = 1U})
        .init(Lifetime{.min = 10.0F, .max = 10.0F})
        .init(ShapeLocation{.shape = Shape::Point, .center = Vec3{0.0F, 0.1F, 0.0F}})
        .init(Velocity{.direction = Vec3{1.0F, 0.0F, 0.0F}, .speed_min = speed, .speed_max = speed})
        .init(InitSize{.min = 0.2F, .max = 0.2F})
        .init(InitTumble{.speed_min = spin, .speed_max = spin})
        .update(Gravity{})
        .update(RigidBody{.angular_drag = 0.0F, .roundness = 0.0F})
        .update(PlaneCollision{.restitution = 0.0F, .friction = friction});
    return make(builder.build().value());
}

void friction_slows_a_chip_by_its_weight() {
    // Resting on the floor, each step's contact pushes back gravity's
    // 9.81 / 60, and friction f takes f / (1 - f) of that off the slide.
    for (const float friction : {0.25F, 0.5F}) {
        Simulation sliding = chip(5.0F, 0.0F, friction);
        sliding.advance_to(10U);
        const float before = sliding.emitter(0).velocity[0].x;
        sliding.advance_to(20U);
        const EmitterState& s = sliding.emitter(0);
        const float lost = 10.0F * 9.81F / 60.0F * friction / (1.0F - friction);
        AJFX_CHECK(near(before - s.velocity[0].x, lost, 1.0e-3F));
        // Flat, it slides without rolling.
        AJFX_CHECK(length(s.angular_velocity[0]) == 0.0F);
        // And comes to a stop.
        sliding.advance_to(120U);
        AJFX_CHECK(sliding.emitter(0).velocity[0].x == 0.0F);
    }
    // Full friction grips at its first touch, however light.
    Simulation stuck = chip(3.0F, 0.0F, 1.0F);
    stuck.advance_to(2U);
    AJFX_CHECK(stuck.emitter(0).velocity[0].x == 0.0F);

    // A spinning chip lands and stops tumbling, even on ice.
    Simulation spun = chip(0.0F, 720.0F, 0.0F);
    spun.advance_to(1U);
    AJFX_CHECK(length(spun.emitter(0).angular_velocity[0]) > 12.0F);
    spun.advance_to(2U);
    AJFX_CHECK(length(spun.emitter(0).angular_velocity[0]) == 0.0F);
}

// How fast the ball's surface moves where it touches the floor.
Vec3 contact_velocity(const EmitterState& s) {
    return s.velocity[0] + cross(s.angular_velocity[0], Vec3{0.0F, -s.size[0] * 0.5F, 0.0F});
}

void friction_turns_sliding_into_rolling() {
    // Born at step 0, it first meets the floor in step 1. A solid ball
    // gripped fully keeps 5/7 of its speed and rolls: its surface stops
    // where it touches, so it spins at speed / radius about -Z.
    Simulation sliding = ball(2.0F, 0.0F);
    sliding.advance_to(2U);
    const EmitterState& s = sliding.emitter(0);
    AJFX_CHECK(near(s.velocity[0].x, 2.0F * 5.0F / 7.0F, 1.0e-5F));
    AJFX_CHECK(near(s.angular_velocity[0].z, -2.0F * 5.0F / 7.0F / 0.1F, 1.0e-4F));
    AJFX_CHECK(length(contact_velocity(s)) < 1.0e-5F);
    // It turns that way, a step's worth at a time.
    sliding.advance_to(3U);
    const Quat q = sliding.emitter(0).orientation[0];
    AJFX_CHECK(q.x == 0.0F && q.y == 0.0F && q.z < 0.0F);
    const float turned = 2.0F * std::atan2(-q.z, q.w);
    AJFX_CHECK(near(turned, 2.0F * 5.0F / 7.0F / 0.1F / 60.0F, 0.003F));
    // Rolling, nothing slips, so nothing slows it.
    sliding.advance_to(60U);
    AJFX_CHECK(near(sliding.emitter(0).velocity[0].x, 2.0F * 5.0F / 7.0F, 1.0e-4F));

    // Dropped spinning, the grip throws it along: it leaves rolling too.
    Simulation spun = ball(0.0F, 720.0F);
    spun.advance_to(2U);
    const EmitterState& t = spun.emitter(0);
    const Vec3 along{t.velocity[0].x, 0.0F, t.velocity[0].z};
    AJFX_CHECK(length(along) > 0.1F);
    AJFX_CHECK(length(contact_velocity(t)) < 1.0e-4F);
}

void tumbles_start_uniformly() {
    const auto tumbling = [](bool random_orientation) {
        EffectBuilder builder{"tumble"};
        builder.emitter("e")
            .capacity(4096U)
            .spawn(SpawnBurst{.count = 4000U})
            .init(Lifetime{.min = 10.0F, .max = 10.0F})
            .init(InitTumble{.speed_min = 90.0F, .speed_max = 180.0F, .random_orientation = random_orientation})
            .update(RigidBody{});
        Simulation simulation = make(builder.build().value());
        simulation.advance_to(1U);
        return simulation;
    };
    const Simulation random = tumbling(true);
    const EmitterState& s = random.emitter(0);
    AJFX_CHECK(s.alive() == 4000U);
    double mean[4]{};
    double square[4]{};
    Vec3 axes{};
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        const Quat q = s.orientation[i];
        AJFX_CHECK(near(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w, 1.0F, 1.0e-5F));
        const float c[4] = {q.x, q.y, q.z, q.w};
        for (int k = 0; k < 4; ++k) {
            mean[k] += c[k] / 4000.0;
            square[k] += c[k] * c[k] / 4000.0;
        }
        // A quarter to a half turn a second, about any axis.
        const float speed = length(s.angular_velocity[i]);
        AJFX_CHECK(speed >= 1.5707F && speed <= 3.1416F);
        axes += s.angular_velocity[i] * (1.0F / speed / 4000.0F);
    }
    // A uniform rotation's components average zero, and each squared a
    // quarter.
    for (int k = 0; k < 4; ++k) {
        AJFX_CHECK(std::fabs(mean[k]) < 0.03 && std::fabs(square[k] - 0.25) < 0.02);
    }
    AJFX_CHECK(length(axes) < 0.05F);
    // Without a random orientation, all start unturned.
    for (const Quat& q : tumbling(false).emitter(0).orientation) {
        AJFX_CHECK(q == Quat{});
    }
}

void parameters_are_clamped_to_their_range() {
    EffectBuilder builder{"clamp"};
    builder.float_parameter("rate", 10.0F, 0.0F, 120.0F);
    builder.emitter("e").spawn(SpawnRate{.rate = bind("rate")}).init(Lifetime{.min = 0.5F, .max = 0.5F});
    Simulation simulation = make(builder.build().value());
    const HugeRate huge;
    simulation.advance_to(60U, &huge);
    // Two births a step at the clamped 120 per second.
    AJFX_CHECK(simulation.emitter(0).spawned == 120U);
    AJFX_CHECK(simulation.emitter(0).refused == 0U);
}

void colour_and_size_follow_normalized_age() {
    Simulation simulation = make(one_emitter([](EmitterBuilder& e) -> EmitterBuilder& {
        return e.spawn(SpawnBurst{.count = 4U})
            .init(Lifetime{.min = 1.0F, .max = 1.0F})
            .init(InitColor{.a = Color{0.5F, 0.5F, 0.5F, 1.0F}, .b = Color{0.5F, 0.5F, 0.5F, 1.0F}})
            .init(InitSize{.min = 2.0F, .max = 2.0F})
            .update(ColorOverLife{.keys = {{0.0F, Color{1.0F, 1.0F, 1.0F, 1.0F}}, {1.0F, Color{0.0F, 0.0F, 0.0F, 0.0F}}}})
            .update(SizeOverLife{.keys = {{0.0F, 1.0F}, {0.5F, 3.0F}, {1.0F, 3.0F}}});
    }));
    for (const std::uint64_t tick : {1U, 15U, 30U, 45U}) {
        simulation.advance_to(tick);
        const EmitterState& s = simulation.emitter(0);
        const float t = s.age[0] / s.lifetime[0];
        AJFX_CHECK(near(s.color[0].r, 0.5F * (1.0F - t), 1.0e-6F));
        AJFX_CHECK(near(s.color[0].a, 1.0F - t, 1.0e-6F));
        const float curve = t < 0.5F ? 1.0F + 4.0F * t : 3.0F;
        AJFX_CHECK(near(s.size[0], 2.0F * curve, 1.0e-5F));
    }
}

void velocity_cone_respects_its_spread() {
    Simulation narrow = make(one_emitter([](EmitterBuilder& e) -> EmitterBuilder& {
        return e.spawn(SpawnBurst{.count = 500U})
            .init(Lifetime{.min = 10.0F, .max = 10.0F})
            .init(Velocity{.direction = Vec3{1.0F, 0.0F, 0.0F}, .speed_min = 2.0F, .speed_max = 3.0F, .spread = 10.0F});
    }));
    narrow.step();
    const float cos_spread = std::cos(10.0F * 3.14159265F / 180.0F);
    for (const Vec3& v : narrow.emitter(0).velocity) {
        const float speed = std::sqrt(dot(v, v));
        AJFX_CHECK(speed >= 2.0F - 1.0e-5F && speed <= 3.0F + 1.0e-5F);
        AJFX_CHECK(v.x / speed >= cos_spread - 1.0e-5F);
    }

    Simulation everywhere = make(one_emitter([](EmitterBuilder& e) -> EmitterBuilder& {
        return e.spawn(SpawnBurst{.count = 500U})
            .init(Lifetime{.min = 10.0F, .max = 10.0F})
            .init(Velocity{.spread = 180.0F});
    }));
    everywhere.step();
    std::size_t downward = 0U;
    for (const Vec3& v : everywhere.emitter(0).velocity) {
        downward += v.y < 0.0F ? 1U : 0U;
    }
    AJFX_CHECK(downward > 200U && downward < 300U);

    // A hollow cone: between 30 and 40 degrees off +X, filling that band.
    Simulation crown = make(one_emitter([](EmitterBuilder& e) -> EmitterBuilder& {
        return e.spawn(SpawnBurst{.count = 500U})
            .init(Lifetime{.min = 10.0F, .max = 10.0F})
            .init(Velocity{.direction = Vec3{1.0F, 0.0F, 0.0F}, .spread = 40.0F, .spread_min = 30.0F});
    }));
    crown.step();
    const float cos_outer = std::cos(40.0F * 3.14159265F / 180.0F);
    const float cos_inner = std::cos(30.0F * 3.14159265F / 180.0F);
    const float cos_middle = std::cos(35.0F * 3.14159265F / 180.0F);
    std::size_t outer_half = 0U;
    for (const Vec3& v : crown.emitter(0).velocity) {
        const float c = v.x / std::sqrt(dot(v, v));
        AJFX_CHECK(c <= cos_inner + 1.0e-5F && c >= cos_outer - 1.0e-5F);
        outer_half += c < cos_middle ? 1U : 0U;
    }
    AJFX_CHECK(outer_half > 200U && outer_half < 300U);
}

void shapes_give_each_particle_its_own_radius() {
    // A shell whose radius is picked between 1 and 2: every particle on its
    // own sphere in that band, the band filled.
    Simulation shell = make(one_emitter([](EmitterBuilder& e) -> EmitterBuilder& {
        return e.spawn(SpawnBurst{.count = 600U})
            .init(Lifetime{.min = 10.0F, .max = 10.0F})
            .init(ShapeLocation{.shape = Shape::Sphere, .radius_min = 1.0F, .radius_max = 2.0F, .surface_only = true});
    }));
    shell.step();
    std::size_t outer = 0U;
    for (const Vec3& p : shell.emitter(0).position) {
        const float r = std::sqrt(dot(p, p));
        AJFX_CHECK(r >= 1.0F - 1.0e-5F && r <= 2.0F + 1.0e-5F);
        outer += r > 1.5F ? 1U : 0U;
    }
    AJFX_CHECK(outer > 240U && outer < 360U);

    // Equal ends are a crisp disc's edge, exactly as one radius was.
    Simulation ring = make(one_emitter([](EmitterBuilder& e) -> EmitterBuilder& {
        return e.spawn(SpawnBurst{.count = 100U})
            .init(Lifetime{.min = 10.0F, .max = 10.0F})
            .init(ShapeLocation{.shape = Shape::Disc, .radius_min = 0.75F, .radius_max = 0.75F, .surface_only = true});
    }));
    ring.step();
    for (const Vec3& p : ring.emitter(0).position) {
        AJFX_CHECK(near(std::sqrt(p.x * p.x + p.z * p.z), 0.75F, 1.0e-5F) && p.y == 0.0F);
    }
}

void brightness_is_picked_apart_from_the_colour() {
    Simulation simulation = make(one_emitter([](EmitterBuilder& e) -> EmitterBuilder& {
        return e.spawn(SpawnBurst{.count = 400U})
            .init(Lifetime{.min = 10.0F, .max = 10.0F})
            .init(InitColor{.a = Color{1.0F, 0.5F, 0.0F, 0.8F},
                            .b = Color{1.0F, 0.5F, 0.0F, 0.8F},
                            .brightness_min = 2.0F,
                            .brightness_max = 6.0F});
    }));
    simulation.step();
    std::size_t bright = 0U;
    for (const Color& c : simulation.emitter(0).color) {
        // Brightness scales red, green and blue together, never opacity.
        AJFX_CHECK(c.r >= 2.0F - 1.0e-5F && c.r <= 6.0F + 1.0e-5F);
        AJFX_CHECK(near(c.g, c.r * 0.5F, 1.0e-5F) && c.b == 0.0F && c.a == 0.8F);
        bright += c.r > 4.0F ? 1U : 0U;
    }
    AJFX_CHECK(bright > 150U && bright < 250U);
}

void forces_scale_each_particle_by_its_own_factor() {
    // Each particle falls at its own share of gravity, picked once.
    Simulation varied = make(one_emitter([](EmitterBuilder& e) -> EmitterBuilder& {
        return e.spawn(SpawnBurst{.count = 300U})
            .init(Lifetime{.min = 100.0F, .max = 100.0F})
            .update(Gravity{.acceleration = Vec3{0.0F, -10.0F, 0.0F}, .scale_min = 0.5F, .scale_max = 1.5F});
    }));
    varied.advance_to(60U);
    const EmitterState& s = varied.emitter(0);
    const float full = static_cast<float>(-10.0 * 59.0 / 60.0);
    std::size_t heavy = 0U;
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        const float share = s.velocity[i].y / full;
        AJFX_CHECK(share >= 0.5F - 1.0e-4F && share <= 1.5F + 1.0e-4F);
        heavy += share > 1.0F ? 1U : 0U;
    }
    AJFX_CHECK(heavy > 100U && heavy < 200U);
    // Kept for life: a second later each particle still falls at its share.
    const float share = s.velocity[7].y / full;
    varied.advance_to(120U);
    const float later = static_cast<float>(-10.0 * 119.0 / 60.0);
    AJFX_CHECK(near(varied.emitter(0).velocity[7].y / later, share, 1.0e-4F));

    // Drag slows each particle by its own coefficient; equal ends are the
    // module's own.
    const auto dragged = [](float scale_min, float scale_max) {
        Effect effect = one_emitter([](EmitterBuilder& e) -> EmitterBuilder& {
            return e.spawn(SpawnBurst{.count = 200U})
                .init(Lifetime{.min = 100.0F, .max = 100.0F})
                .init(Velocity{.direction = Vec3{1.0F, 0.0F, 0.0F}, .speed_min = 4.0F, .speed_max = 4.0F})
                .update(Drag{.coefficient = 2.0F});
        });
        std::get<Drag>(effect.emitters[0].update[0]).scale_min = scale_min;
        std::get<Drag>(effect.emitters[0].update[0]).scale_max = scale_max;
        Simulation run = make(effect);
        run.advance_to(30U);
        return run;
    };
    const Simulation plain = dragged(1.0F, 1.0F);
    const Simulation spread = dragged(0.25F, 4.0F);
    float slowest = 4.0F;
    float fastest = 0.0F;
    for (std::size_t i = 0U; i < plain.emitter(0).alive(); ++i) {
        AJFX_CHECK(plain.emitter(0).velocity[i].x == plain.emitter(0).velocity[0].x);
        slowest = std::min(slowest, spread.emitter(0).velocity[i].x);
        fastest = std::max(fastest, spread.emitter(0).velocity[i].x);
    }
    AJFX_CHECK(slowest < plain.emitter(0).velocity[0].x && fastest > plain.emitter(0).velocity[0].x);
}

void reset_and_restore_return_to_earlier_states() {
    Simulation simulation = make(presets::smoke());
    simulation.advance_to(100U);
    const SimulationState at_100 = simulation.state();
    simulation.advance_to(200U);
    simulation.restore(at_100);
    simulation.advance_to(200U);
    Simulation straight = make(presets::smoke());
    straight.advance_to(200U);
    AJFX_CHECK(simulation.state() == straight.state());

    simulation.reset();
    AJFX_CHECK(simulation.tick() == 0U && simulation.emitter(0).alive() == 0U && simulation.emitter(0).spawned == 0U);
}

void invalid_effects_do_not_simulate() {
    Effect effect = presets::sparks();
    effect.emitters[0].capacity = 1U;
    const Result<Simulation> simulation = Simulation::create(effect);
    AJFX_CHECK(!simulation.ok() && simulation.error().code == ErrorCode::BudgetExceeded);
}

} // namespace

// Sparks with every per-particle random draw an emitter can make, and, when
// `second`, another spawn module after the first, with modules of its own
// kind: the first module's particles must not notice it.
Effect sparks_with_a_second_source(bool second) {
    EffectBuilder builder{"sources"};
    EmitterBuilder& e = builder.emitter("e");
    e.capacity(4000U)
        .seed(12U)
        .spawn(SpawnRate{.id = 1U, .rate = 70.0F})
        .init(Lifetime{.id = 2U, .min = 0.5F, .max = 2.0F})
        .init(ShapeLocation{.id = 3U, .shape = Shape::Sphere, .radius_min = 0.2F, .radius_max = 1.0F})
        .init(Velocity{.id = 4U, .speed_min = 1.0F, .speed_max = 3.0F, .spread = 60.0F})
        .init(InitColor{.id = 5U, .a = Color{1.0F, 0.2F, 0.1F, 1.0F}, .b = Color{0.1F, 0.2F, 1.0F, 1.0F}})
        .init(InitSize{.id = 6U, .min = 0.05F, .max = 0.2F})
        .update(Gravity{.id = 7U, .scale_min = 0.5F, .scale_max = 1.5F})
        .update(Drag{.id = 8U, .coefficient = 0.4F, .scale_min = 0.5F, .scale_max = 2.0F})
        .update(ColorOverLife{.id = 9U});
    if (second) {
        e.spawn(SpawnBurst{.id = 10U, .time = 0.25F, .count = 40U, .interval = 0.5F, .cycles = 0U});
    }
    return builder.build().value();
}

void adding_a_spawn_module_leaves_the_others_alone() {
    Simulation alone = make(sparks_with_a_second_source(false), 4U);
    Simulation beside = make(sparks_with_a_second_source(true), 4U);
    for (const std::uint64_t tick : {1U, 20U, 61U, 150U, 300U}) {
        alone.advance_to(tick);
        beside.advance_to(tick);
        const EmitterState& a = alone.emitter(0);
        const EmitterState& b = beside.emitter(0);
        AJFX_CHECK(a.alive() > 0U);
        // The second module's particles are there, and are its own.
        std::size_t from_burst = 0U;
        std::size_t j = 0U;
        for (std::size_t i = 0U; i < b.alive(); ++i) {
            if (spawn_source(b.spawn_index[i]) == 1U) {
                ++from_burst;
                continue;
            }
            AJFX_CHECK(spawn_source(b.spawn_index[i]) == 0U);
            // The first module's particles, in their order, are exactly the
            // ones it makes alone: same spawn index, same draws, same bits.
            AJFX_CHECK(j < a.alive());
            AJFX_CHECK(b.spawn_index[i] == a.spawn_index[j]);
            AJFX_CHECK(b.position[i] == a.position[j] && b.velocity[i] == a.velocity[j]);
            AJFX_CHECK(b.age[i] == a.age[j] && b.lifetime[i] == a.lifetime[j]);
            AJFX_CHECK(b.color[i] == a.color[j] && b.size[i] == a.size[j]);
            ++j;
        }
        AJFX_CHECK(j == a.alive());
        AJFX_CHECK(tick < 15U || from_burst > 0U);
        // Each source counts its own births.
        AJFX_CHECK(b.spawn_counts.size() == 3U && a.spawn_counts.size() == 2U);
        AJFX_CHECK(b.spawn_counts[0] == a.spawn_counts[0]);
        AJFX_CHECK(b.spawned == b.spawn_counts[0] + b.spawn_counts[1]);
    }
    // A seek over both sources lands on the stepped state, counts and all.
    CheckpointRing ring{40U, 4U};
    Simulation seeker = make(sparks_with_a_second_source(true), 4U);
    for (const std::uint64_t target : {170U, 35U, 290U, 81U}) {
        ring.seek(seeker, target);
        Simulation straight = make(sparks_with_a_second_source(true), 4U);
        straight.advance_to(target);
        AJFX_CHECK(seeker.state() == straight.state());
    }
}

// Particles standing still in a ball of radius 4, and a blast of radius 3 at
// its centre half a second in.
Effect still_dust_and_a_blast(float scale_min, float scale_max) {
    EffectBuilder builder{"shockwave"};
    builder.blast(Blast{.name = "boom", .center = Vec3{0.5F, 1.0F, 0.0F}, .power = 6.0F, .radius = 3.0F, .time = 0.5F});
    builder.emitter("dust")
        .capacity(400U)
        .seed(3U)
        .spawn(SpawnBurst{.count = 300U})
        .init(Lifetime{.min = 10.0F, .max = 10.0F})
        .init(ShapeLocation{.shape = Shape::Sphere, .center = Vec3{0.5F, 1.0F, 0.0F}, .radius_min = 0.0F,
                            .radius_max = 4.0F})
        .update(Shockwave{.blast = "boom", .scale_min = scale_min, .scale_max = scale_max});
    return builder.build().value();
}

void a_shockwave_throws_particles_out_from_its_blast() {
    const Effect effect = still_dust_and_a_blast(1.0F, 1.0F);
    const Blast& blast = effect.blasts[0];
    AJFX_CHECK(blast_tick(blast, effect.simulation_rate) == 30U);
    Simulation simulation = make(effect, 2U);
    simulation.advance_to(30U);
    const EmitterState& s = simulation.emitter(0);
    const std::vector<Vec3> before = s.position;
    for (const Vec3& v : s.velocity) {
        AJFX_CHECK(v == Vec3{});
    }
    simulation.step();
    std::size_t thrown = 0U;
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        const Vec3 out = before[i] - blast.center;
        const float distance = std::sqrt(dot(out, out));
        const float speed = std::sqrt(dot(s.velocity[i], s.velocity[i]));
        if (distance >= blast.radius) {
            // Beyond its reach a blast touches nothing.
            AJFX_CHECK(s.velocity[i] == Vec3{});
            continue;
        }
        ++thrown;
        // Straight out from the centre, as hard as the blast is there.
        AJFX_CHECK(near(speed, blast.power * (1.0F - distance / blast.radius), 1.0e-4F));
        AJFX_CHECK(distance < 1.0e-3F || dot(s.velocity[i], out) > 0.999F * speed * distance);
    }
    AJFX_CHECK(thrown > 50U && thrown < s.alive());
    // Once: the next step leaves every velocity as it was.
    const std::vector<Vec3> after = s.velocity;
    simulation.step();
    AJFX_CHECK(s.velocity == after);

    // Each particle takes the blast by its own factor.
    Simulation scaled = make(still_dust_and_a_blast(0.0F, 2.0F), 2U);
    scaled.advance_to(31U);
    float slowest = 1.0e9F;
    float fastest = 0.0F;
    for (std::size_t i = 0U; i < scaled.emitter(0).alive(); ++i) {
        const Vec3 out = before[i] - blast.center;
        const float distance = std::sqrt(dot(out, out));
        if (distance < blast.radius * 0.8F) {
            const Vec3 v = scaled.emitter(0).velocity[i];
            const float ratio = std::sqrt(dot(v, v)) / (blast.power * (1.0F - distance / blast.radius));
            slowest = std::min(slowest, ratio);
            fastest = std::max(fastest, ratio);
        }
    }
    AJFX_CHECK(slowest < 0.5F && fastest > 1.5F && fastest <= 2.0F + 1.0e-4F);

    // A seek over the blast finds it in the same step.
    CheckpointRing ring{20U, 4U};
    Simulation seeker = make(effect, 2U);
    for (const std::uint64_t target : {45U, 29U, 31U, 60U, 30U}) {
        ring.seek(seeker, target);
        Simulation straight = make(effect, 2U);
        straight.advance_to(target);
        AJFX_CHECK(seeker.state() == straight.state());
    }
}

int main() {
    const ajfx::test::Case cases[] = {
        {"reruns_are_identical_and_seeds_differ", reruns_are_identical_and_seeds_differ},
        {"seeking_lands_on_the_stepped_state", seeking_lands_on_the_stepped_state},
        {"rate_spawning_is_exact", rate_spawning_is_exact},
        {"bursts_fire_on_their_step", bursts_fire_on_their_step},
        {"gravity_integrates_semi_implicitly", gravity_integrates_semi_implicitly},
        {"forces_follow_their_curves_over_life", forces_follow_their_curves_over_life},
        {"drifting_turbulence_moves_its_field", drifting_turbulence_moves_its_field},
        {"converge_pulls_towards_its_axis_below_its_height", converge_pulls_towards_its_axis_below_its_height},
        {"spin_turns_sprites_and_slows", spin_turns_sprites_and_slows},
        {"particles_die_in_order", particles_die_in_order},
        {"plane_collision_holds_or_kills", plane_collision_holds_or_kills},
        {"spheres_and_boxes_hold_particles_in_or_out", spheres_and_boxes_hold_particles_in_or_out},
        {"distance_fields_hold_particles_out_or_in", distance_fields_hold_particles_out_or_in},
        {"debris_settles_on_the_floor", debris_settles_on_the_floor},
        {"friction_turns_sliding_into_rolling", friction_turns_sliding_into_rolling},
        {"friction_slows_a_chip_by_its_weight", friction_slows_a_chip_by_its_weight},
        {"tumbles_start_uniformly", tumbles_start_uniformly},
        {"parameters_are_clamped_to_their_range", parameters_are_clamped_to_their_range},
        {"colour_and_size_follow_normalized_age", colour_and_size_follow_normalized_age},
        {"velocity_cone_respects_its_spread", velocity_cone_respects_its_spread},
        {"shapes_give_each_particle_its_own_radius", shapes_give_each_particle_its_own_radius},
        {"brightness_is_picked_apart_from_the_colour", brightness_is_picked_apart_from_the_colour},
        {"forces_scale_each_particle_by_its_own_factor", forces_scale_each_particle_by_its_own_factor},
        {"reset_and_restore_return_to_earlier_states", reset_and_restore_return_to_earlier_states},
        {"invalid_effects_do_not_simulate", invalid_effects_do_not_simulate},
        {"adding_a_spawn_module_leaves_the_others_alone", adding_a_spawn_module_leaves_the_others_alone},
        {"a_shockwave_throws_particles_out_from_its_blast", a_shockwave_throws_particles_out_from_its_blast},
    };
    return ajfx::test::run(cases);
}
