#include "check.hpp"

#include <ajfx/builder.hpp>
#include <ajfx/presets.hpp>
#include <ajfx/simulation.hpp>

#include <cmath>
#include <cstdint>
#include <span>

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
    for (const Effect& effect : {presets::sparks(), presets::smoke(), presets::confetti()}) {
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

int main() {
    const ajfx::test::Case cases[] = {
        {"reruns_are_identical_and_seeds_differ", reruns_are_identical_and_seeds_differ},
        {"seeking_lands_on_the_stepped_state", seeking_lands_on_the_stepped_state},
        {"rate_spawning_is_exact", rate_spawning_is_exact},
        {"bursts_fire_on_their_step", bursts_fire_on_their_step},
        {"gravity_integrates_semi_implicitly", gravity_integrates_semi_implicitly},
        {"particles_die_in_order", particles_die_in_order},
        {"plane_collision_holds_or_kills", plane_collision_holds_or_kills},
        {"spheres_and_boxes_hold_particles_in_or_out", spheres_and_boxes_hold_particles_in_or_out},
        {"distance_fields_hold_particles_out_or_in", distance_fields_hold_particles_out_or_in},
        {"parameters_are_clamped_to_their_range", parameters_are_clamped_to_their_range},
        {"colour_and_size_follow_normalized_age", colour_and_size_follow_normalized_age},
        {"velocity_cone_respects_its_spread", velocity_cone_respects_its_spread},
        {"reset_and_restore_return_to_earlier_states", reset_and_restore_return_to_earlier_states},
        {"invalid_effects_do_not_simulate", invalid_effects_do_not_simulate},
    };
    return ajfx::test::run(cases);
}
