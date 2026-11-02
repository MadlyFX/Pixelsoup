#include "check.hpp"

#include <ajfx/builder.hpp>
#include <ajfx/fracture.hpp>
#include <ajfx/mesh.hpp>
#include <ajfx/presets.hpp>
#include <ajfx/rigid.hpp>
#include <ajfx/rigid_runner.hpp>
#include <ajfx/simulation.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace ajfx;

std::unique_ptr<RigidSolver> g_solver;

float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

Simulation make(const Effect& effect, const RigidSolver* solver = g_solver.get(), MeshSet meshes = {}) {
    Result<Simulation> simulation =
        Simulation::create(effect, 3U, SimulationOptions{.meshes = std::move(meshes), .rigid = solver});
    if (!simulation) {
        std::cout << simulation.error().message << '\n';
    }
    AJFX_CHECK(simulation.ok());
    return std::move(simulation).value();
}

// Blocks 0.2 to 0.4 across poured from a column onto a floor, four every
// 1/30 s for 1.5 s, with nothing else to stop them: they can only rest on the
// floor or on each other.
Effect pile() {
    EffectBuilder builder{"pile"};
    builder.emitter("blocks")
        .capacity(256U)
        .spawn(SpawnBurst{.time = 0.0F, .count = 4U, .interval = 1.0F / 30.0F, .cycles = 45U})
        .init(Lifetime{.min = 30.0F, .max = 30.0F})
        .init(ShapeLocation{.shape = Shape::Disc, .center = Vec3{0.0F, 3.0F, 0.0F}, .radius_min = 0.4F, .radius_max = 0.4F})
        .init(Velocity{.direction = Vec3{0.0F, -1.0F, 0.0F}, .speed_min = 1.0F, .speed_max = 2.0F, .spread = 10.0F})
        .init(InitSize{.min = 0.2F, .max = 0.4F})
        .init(InitTumble{.speed_min = 0.0F, .speed_max = 180.0F})
        .update(Gravity{})
        .update(RigidPhysics{.shape = RigidShape::Box, .friction = 0.8F, .restitution = 0.1F})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 0.8F});
    return builder.build().value();
}

// Short-lived balls bouncing in a box, born and dying every step: the world
// gains and loses bodies all the time, in slots that come free.
Effect churn() {
    EffectBuilder builder{"churn"};
    builder.emitter("balls")
        .capacity(256U)
        .seed(9U)
        .spawn(SpawnRate{.rate = 90.0F})
        .init(Lifetime{.min = 0.8F, .max = 2.2F})
        .init(ShapeLocation{.shape = Shape::Sphere, .center = Vec3{0.0F, 1.0F, 0.0F}, .radius_min = 0.5F, .radius_max = 0.5F})
        .init(Velocity{.speed_min = 1.0F, .speed_max = 4.0F, .spread = 180.0F})
        .init(InitSize{.min = 0.1F, .max = 0.25F})
        .init(InitTumble{})
        .update(Gravity{})
        .update(Drag{.coefficient = 0.2F})
        .update(RigidPhysics{.shape = RigidShape::Box, .restitution = 0.4F})
        .update(BoxCollision{.center = Vec3{0.0F, 1.0F, 0.0F}, .half_size = Vec3{1.0F, 1.0F, 1.0F}, .inside = true,
                             .restitution = 0.5F, .friction = 0.3F})
        .update(SphereCollision{.center = Vec3{0.3F, 0.2F, 0.0F}, .radius = 0.3F})
        .update(ColorOverLife{});
    return builder.build().value();
}

void piles_rest_on_the_floor_and_each_other() {
    Simulation simulation = make(pile());
    simulation.advance_to(600U);
    const EmitterState& s = simulation.emitter(0);
    AJFX_CHECK(s.alive() == 180U);
    float highest = 0.0F;
    std::size_t still = 0U;
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        const float radius = 0.5F * s.size[i];
        // On the floor or above it, give or take the engine's slop.
        AJFX_CHECK(s.position[i].y >= radius - 0.03F);
        highest = std::max(highest, s.position[i].y - radius);
        still += length(s.velocity[i]) == 0.0F && length(s.angular_velocity[i]) == 0.0F ? 1U : 0U;
        // No block is inside another: their inscribed balls never overlap.
        for (std::size_t j = 0U; j < i; ++j) {
            const float apart = length(s.position[i] - s.position[j]);
            AJFX_CHECK(apart >= radius + 0.5F * s.size[j] - 0.03F);
        }
        const Quat q = s.orientation[i];
        AJFX_CHECK(std::fabs(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w - 1.0F) < 1.0e-4F);
    }
    // They pile: some rest on others, well above the floor.
    std::cout << "  highest block rests " << highest << " above the floor; " << still << " of " << s.alive()
              << " asleep\n";
    AJFX_CHECK(highest > 0.4F);
    // And they come to rest, asleep.
    AJFX_CHECK(still == s.alive());
}

void bodies_fall_when_what_held_them_up_goes() {
    // Two blocks half a unit across, dropped one a second after the other,
    // so the second comes to rest asleep on the first. Each lives three
    // seconds: when the first goes, the second must fall to the floor
    // rather than hang where it slept.
    EffectBuilder builder{"stack"};
    builder.emitter("blocks")
        .capacity(4U)
        .spawn(SpawnBurst{.time = 0.0F, .count = 1U})
        .spawn(SpawnBurst{.time = 1.0F, .count = 1U})
        .init(Lifetime{.min = 3.0F, .max = 3.0F})
        .init(ShapeLocation{.shape = Shape::Point, .center = Vec3{0.0F, 1.5F, 0.0F}})
        .init(InitSize{.min = 0.5F, .max = 0.5F})
        .update(Gravity{})
        .update(RigidPhysics{.shape = RigidShape::Box, .friction = 0.8F, .restitution = 0.0F})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 0.8F});
    Simulation simulation = make(builder.build().value());
    // At 2.9 s the second sleeps on the first, a block's height up.
    simulation.advance_to(174U);
    const EmitterState& s = simulation.emitter(0);
    AJFX_CHECK(s.alive() == 2U);
    AJFX_CHECK(length(s.velocity[1]) == 0.0F && std::fabs(s.position[1].y - 0.75F) < 0.03F);
    // At 3.6 s the first has gone and the second lies on the floor.
    simulation.advance_to(216U);
    AJFX_CHECK(s.alive() == 1U);
    std::cout << "  the second block came down to " << s.position[0].y << '\n';
    AJFX_CHECK(std::fabs(s.position[0].y - 0.25F) < 0.03F);
}

void reruns_and_thread_counts_agree() {
    const Effect effect = churn();
    const std::unique_ptr<RigidSolver> four = rigid::make_jolt_solver(rigid::JoltOptions{.threads = 4U});
    Simulation a = make(effect);
    Simulation b = make(effect);
    Simulation c = make(effect, four.get());
    for (const std::uint64_t tick : {1U, 45U, 120U, 300U}) {
        a.advance_to(tick);
        b.advance_to(tick);
        c.advance_to(tick);
        AJFX_CHECK(a.emitter(0).alive() > 0U || tick == 1U);
        AJFX_CHECK(!a.state().emitters[0].solver_state.empty());
        AJFX_CHECK(a.state() == b.state());
        AJFX_CHECK(a.state() == c.state());
    }
    // A different instance seed is a different explosion.
    Simulation other = Simulation::create(effect, 4U, SimulationOptions{.rigid = g_solver.get()}).value();
    other.advance_to(120U);
    AJFX_CHECK(other.emitter(0).position != a.emitter(0).position);
}

void seeking_lands_on_the_stepped_state() {
    const Effect effect = churn();
    Simulation stepped = make(effect);
    Simulation seeker = make(effect);
    CheckpointRing ring{30U, 8U};
    for (const std::uint64_t target : {200U, 95U, 260U, 31U, 299U, 140U}) {
        ring.seek(seeker, target);
        Simulation straight = make(effect);
        straight.advance_to(target);
        AJFX_CHECK(seeker.tick() == target);
        AJFX_CHECK(seeker.state() == straight.state());
    }
    // Every step after a restore matches, not just the ones checked.
    stepped.advance_to(100U);
    const SimulationState at_100 = stepped.state();
    stepped.advance_to(160U);
    Simulation restored = make(effect);
    restored.restore(at_100);
    Simulation straight = make(effect);
    straight.advance_to(100U);
    for (std::uint64_t tick = 101U; tick <= 160U; ++tick) {
        restored.step();
        straight.step();
        AJFX_CHECK(restored.emitter(0).position == straight.emitter(0).position);
        AJFX_CHECK(restored.emitter(0).orientation == straight.emitter(0).orientation);
    }
    AJFX_CHECK(restored.state() == stepped.state());
}

void births_deaths_and_colour_follow_the_reference() {
    // The same emitter without rigid physics: births, ages, lifetimes,
    // spawn indices and colour over life must not change; only the motion
    // is the engine's.
    const Effect rigid = churn();
    Effect plain = rigid;
    std::erase_if(plain.emitters[0].update,
                  [](const UpdateModule& m) { return std::holds_alternative<RigidPhysics>(m); });
    std::erase_if(plain.emitters[0].init, [](const InitModule& m) { return std::holds_alternative<InitTumble>(m); });
    Simulation a = make(rigid);
    Simulation b = make(plain);
    a.advance_to(170U);
    b.advance_to(170U);
    const EmitterState& r = a.emitter(0);
    const EmitterState& p = b.emitter(0);
    AJFX_CHECK(r.alive() == p.alive() && r.alive() > 100U);
    AJFX_CHECK(r.spawned == p.spawned && r.spawn_index == p.spawn_index);
    AJFX_CHECK(r.age == p.age && r.lifetime == p.lifetime && r.color == p.color && r.size == p.size);
    // Slots are unique among the living, and below capacity.
    std::vector<std::uint32_t> slots = r.body;
    std::sort(slots.begin(), slots.end());
    AJFX_CHECK(std::adjacent_find(slots.begin(), slots.end()) == slots.end());
    AJFX_CHECK(slots.back() < 256U);
    // Inside the box, give or take each body's size.
    for (std::size_t i = 0U; i < r.alive(); ++i) {
        const Vec3 d = r.position[i] - Vec3{0.0F, 1.0F, 0.0F};
        AJFX_CHECK(std::fabs(d.x) <= 1.05F && std::fabs(d.y) <= 1.05F && std::fabs(d.z) <= 1.05F);
    }
}

// The height of a rubble chip's lowest corner: the chip mesh's half extents
// are 0.5, 0.25 and 0.4 of its size.
float lowest_corner(const EmitterState& s, std::size_t i) {
    const Quat q = s.orientation[i];
    const float h[3] = {0.5F * s.size[i], 0.25F * s.size[i], 0.4F * s.size[i]};
    // How far up each of the chip's own axes points.
    const float up[3] = {2.0F * (q.x * q.y + q.w * q.z), 1.0F - 2.0F * (q.x * q.x + q.z * q.z),
                         2.0F * (q.y * q.z - q.w * q.x)};
    return s.position[i].y - (std::fabs(up[0]) * h[0] + std::fabs(up[1]) * h[1] + std::fabs(up[2]) * h[2]);
}

void hulls_wrap_the_renderers_mesh() {
    // The rubble preset's chips: hulls of the chip mesh, piling on the floor
    // and each other. Without the mesh, or without a solver, it is refused.
    const Effect effect = presets::rubble();
    AJFX_CHECK(validate(effect).ok());
    const Result<Simulation> no_mesh = Simulation::create(effect, 0U, SimulationOptions{.rigid = g_solver.get()});
    AJFX_CHECK(!no_mesh.ok() && no_mesh.error().code == ErrorCode::MissingMesh);
    const Result<Simulation> no_solver = Simulation::create(effect, 0U, SimulationOptions{.meshes = presets::meshes()});
    AJFX_CHECK(!no_solver.ok() && no_solver.error().code == ErrorCode::RigidSolverMissing);
    AJFX_CHECK(no_solver.error().message.find("'rubble'") != std::string::npos);

    Simulation rubble = make(effect, g_solver.get(), presets::meshes());
    rubble.advance_to(360U);
    const EmitterState& s = rubble.emitter(0);
    AJFX_CHECK(s.alive() == 400U);
    std::size_t on_floor = 0U;
    std::size_t piled = 0U;
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        // At rest, and touching the floor with the mesh as drawn: the hull
        // is the mesh, so no chip hovers above it or sinks in further than
        // the engine lets contacts overlap (its penetration slop, 0.02).
        AJFX_CHECK(length(s.velocity[i]) == 0.0F);
        const float low = lowest_corner(s, i);
        AJFX_CHECK(low > -0.025F);
        if (low < 0.02F) {
            ++on_floor;
        } else {
            ++piled;
        }
    }
    std::cout << "  " << on_floor << " chips lie on the floor, " << piled << " on other chips\n";
    AJFX_CHECK(on_floor > 100U && piled > 100U);
}

void unsupported_combinations_are_refused() {
    const auto refused = [](auto&& configure, const std::string& fragment) {
        EffectBuilder builder{"x"};
        EmitterBuilder& emitter = builder.emitter("rocks");
        emitter.init(Lifetime{}).update(RigidPhysics{});
        configure(emitter);
        const Result<Effect> effect = builder.build();
        if (effect.ok() || effect.error().message.find(fragment) == std::string::npos) {
            std::cout << "  expected '" << fragment << "', got: "
                      << (effect.ok() ? std::string{"accepted"} : effect.error().message) << '\n';
            return false;
        }
        return true;
    };
    AJFX_CHECK(refused([](EmitterBuilder& e) { e.update(SizeOverLife{}); }, "'size_over_life'"));
    AJFX_CHECK(refused([](EmitterBuilder& e) { e.update(CurlNoise{}); }, "the physics engine moves them"));
    AJFX_CHECK(refused([](EmitterBuilder& e) { e.update(SdfCollision{.field = "floor"}); }, "'sdf_collision'"));
    AJFX_CHECK(refused([](EmitterBuilder& e) { e.update(SphereCollision{.inside = true}); }, "keeps particles inside"));
    AJFX_CHECK(refused([](EmitterBuilder& e) { e.update(PlaneCollision{.kill = true}); }, "kills"));
    AJFX_CHECK(refused([](EmitterBuilder& e) { e.update(RigidBody{}); }, "choose the GPU tumble"));
    AJFX_CHECK(refused([](EmitterBuilder& e) { e.update(RigidPhysics{}); }, "more than one 'rigid_physics'"));
    AJFX_CHECK(refused([](EmitterBuilder& e) { e.init(InitSpin{}); }, "use 'tumble'"));
    AJFX_CHECK(refused([](EmitterBuilder& e) { e.capacity(max_rigid_capacity + 1U); }, "at most 65536"));

    // A bound collider names the parameter it is bound to.
    EffectBuilder bound{"x"};
    bound.vec3_parameter("where", Vec3{}, -5.0F, 5.0F);
    bound.emitter("rocks").init(Lifetime{}).update(RigidPhysics{}).update(BoxCollision{.center = bind("where")});
    const Result<Effect> moving = bound.build();
    AJFX_CHECK(!moving.ok() && moving.error().message.find("do not move") != std::string::npos);
    // A hull wraps the renderer's mesh; sprites have none.
    EffectBuilder hull{"x"};
    hull.emitter("rocks").init(Lifetime{}).update(RigidPhysics{.shape = RigidShape::Hull});
    const Result<Effect> no_mesh = hull.build();
    AJFX_CHECK(!no_mesh.ok() && no_mesh.error().message.find("draws none") != std::string::npos);
}

// A bucket standing on the floor with balls resting on its inside bottom.
// The bucket is a hollow prop: a compound of a disc and a ring of staves, so
// the balls are *inside* it. The convex hull of the same bucket would be a
// solid tub they could only sit on top of, which is the whole reason the
// hollow shapes are generated rather than wrapped.
//
// Measurements, all hand-computed and asserted against below: the bucket is
// 0.5 in radius and 0.8 tall with 0.05 walls, centred at y = 0.4, so its
// outside bottom sits on the floor, the top of its floor is at y = 0.05 and
// its inside radius is 0.45. The balls are 0.1 across.
constexpr float bucket_radius = 0.5F;
constexpr float bucket_height = 0.8F;
constexpr float bucket_wall = 0.05F;
constexpr float ball_size = 0.1F;
constexpr float bucket_floor_top = bucket_wall;
constexpr float bucket_inside = bucket_radius - bucket_wall;

Effect bucket_of_balls(PropMotion motion, float tip_degrees) {
    RigidProp bucket{};
    bucket.shape = PropShape::HollowCylinder;
    bucket.motion = motion;
    bucket.position = Vec3{0.0F, bucket_height * 0.5F, 0.0F};
    bucket.radius = bucket_radius;
    bucket.height = bucket_height;
    bucket.wall = bucket_wall;
    bucket.segments = 16U;
    bucket.density = 600.0F;
    bucket.friction = 0.7F;
    bucket.restitution = 0.05F;
    bucket.to_position = Vec3{0.0F, bucket_height * 0.5F, 0.0F};
    bucket.to_turn = Vec3{0.0F, 0.0F, tip_degrees};
    bucket.move_from = 0.5F;
    bucket.move_over = 0.5F;

    EffectBuilder builder{"bucket"};
    builder.emitter("balls")
        .capacity(64U)
        .spawn(SpawnPile{.count = 24U,
                         .shape = PileShape::Box,
                         .center = Vec3{0.0F, bucket_floor_top, 0.0F},
                         .width = 0.5F,
                         .depth = 0.5F,
                         .spacing = 1.0F,
                         .jitter = 0.0F,
                         .release = 0.0F})
        .init(Lifetime{.min = 30.0F, .max = 30.0F})
        .init(InitSize{.min = ball_size, .max = ball_size})
        .update(Gravity{})
        .update(RigidPhysics{.shape = RigidShape::Sphere,
                             .friction = 0.5F,
                             .restitution = 0.1F,
                             .props = {bucket}})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 0.8F});
    return builder.build().value();
}

// How far a ball's centre is from the bucket's upright axis.
float from_axis(Vec3 position) {
    return std::sqrt(position.x * position.x + position.z * position.z);
}

void balls_rest_inside_a_hollow_prop() {
    Simulation simulation = make(bucket_of_balls(PropMotion::Fixed, 0.0F));
    const EmitterState& s = simulation.emitter(0U);
    AJFX_CHECK(s.alive() == 24U);
    AJFX_CHECK(s.props.size() == 1U);
    // The props stand in the world before the first step, so a paused effect
    // at the start already shows the bucket where it was put.
    AJFX_CHECK(std::fabs(s.props[0].position.y - bucket_height * 0.5F) < 1.0e-4F);

    for (std::uint64_t step = 0U; step < 300U; ++step) {
        simulation.step();
    }
    const float ball_radius = ball_size * 0.5F;
    std::size_t resting = 0U;
    float highest = 0.0F;
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        // Inside the staves: a ball's centre cannot reach past the inside
        // wall less its own radius. 0.02 is Jolt's penetration slop.
        AJFX_CHECK(from_axis(s.position[i]) < bucket_inside - ball_radius + 0.02F);
        // On the bucket's floor, not through it and not under the bucket.
        AJFX_CHECK(s.position[i].y > bucket_floor_top + ball_radius - 0.02F);
        highest = std::max(highest, s.position[i].y);
        if (length(s.velocity[i]) < 0.01F) {
            ++resting;
        }
    }
    // They came to rest, and in a bucket 0.8 tall they cannot be stacked
    // higher than its rim.
    AJFX_CHECK(resting == s.alive());
    AJFX_CHECK(highest < bucket_height);
    // A fixed prop never moves, however much lands on it.
    AJFX_CHECK(std::fabs(s.props[0].position.y - bucket_height * 0.5F) < 1.0e-4F);
    AJFX_CHECK(length(s.props[0].velocity) == 0.0F);
}

void a_driven_prop_tips_what_it_holds() {
    // The same bucket, tipped past horizontal between 0.5 s and 1 s.
    Simulation simulation = make(bucket_of_balls(PropMotion::Driven, 115.0F));
    const EmitterState& s = simulation.emitter(0U);
    const Quat upright = s.props[0].orientation;
    for (std::uint64_t step = 0U; step < 360U; ++step) {
        simulation.step();
    }
    // The bucket turned, and a driven prop is moved by the effect alone: it
    // ends exactly where its move put it, whatever it shoved on the way.
    AJFX_CHECK(!(s.props[0].orientation == upright));
    AJFX_CHECK(std::fabs(s.props[0].position.y - bucket_height * 0.5F) < 1.0e-3F);

    const float ball_radius = ball_size * 0.5F;
    std::size_t spilled = 0U;
    std::size_t rolling = 0U;
    float highest = 0.0F;
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        // Everything is on the floor, and nothing fell through it.
        AJFX_CHECK(s.position[i].y > ball_radius - 0.03F);
        highest = std::max(highest, s.position[i].y);
        if (from_axis(s.position[i]) > bucket_radius) {
            ++spilled;
        }
        if (length(s.velocity[i]) >= 0.02F) {
            ++rolling;
        }
    }
    std::cout << "  spilled " << spilled << " of " << s.alive() << " clear of the bucket, highest " << highest
              << ", " << rolling << " still rolling slowly\n";
    // Every one is down on the floor, none still cradled: the bucket's inside
    // bottom was carried up to about its own centre, 0.4, while a ball lying
    // on the floor sits at 0.05 and one on top of another at 0.15. Balls are
    // not asserted to stop: a sphere on a plane keeps rolling, since rolling
    // resistance is not modelled (the pile tests use boxes, which do stop).
    AJFX_CHECK(highest < 0.2F);
    // Some made it clear of where the bucket stood.
    AJFX_CHECK(spilled > 0U);
}

void loose_props_fall_and_settle() {
    RigidProp crate{};
    crate.shape = PropShape::HollowBox;
    crate.motion = PropMotion::Loose;
    crate.position = Vec3{0.0F, 1.5F, 0.0F};
    crate.half_size = Vec3{0.3F, 0.3F, 0.3F};
    crate.wall = 0.04F;
    crate.density = 500.0F;
    crate.friction = 0.6F;
    crate.restitution = 0.05F;

    EffectBuilder builder{"crate"};
    builder.emitter("bits")
        .capacity(8U)
        .init(Lifetime{.min = 30.0F, .max = 30.0F})
        .update(Gravity{})
        .update(RigidPhysics{.shape = RigidShape::Sphere, .props = {crate}})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 0.8F});
    Simulation simulation = make(builder.build().value());
    const EmitterState& s = simulation.emitter(0U);
    AJFX_CHECK(std::fabs(s.props[0].position.y - 1.5F) < 1.0e-4F);
    for (std::uint64_t step = 0U; step < 300U; ++step) {
        simulation.step();
    }
    // A loose prop is an ordinary body: it fell and came to rest on the
    // floor, half its height up, and stopped.
    AJFX_CHECK(s.props[0].position.y < 1.5F);
    AJFX_CHECK(std::fabs(s.props[0].position.y - 0.3F) < 0.03F);
    AJFX_CHECK(length(s.props[0].velocity) < 0.01F);
}

void props_rerun_and_seek_exactly() {
    const Effect effect = bucket_of_balls(PropMotion::Driven, 115.0F);
    Simulation a = make(effect);
    Simulation b = make(effect);
    const std::unique_ptr<RigidSolver> four = rigid::make_jolt_solver(rigid::JoltOptions{.threads = 4U});
    Simulation c = make(effect, four.get());
    for (const std::uint64_t until : {1U, 45U, 90U, 200U}) {
        while (a.tick() < until) {
            a.step();
            b.step();
            c.step();
        }
        // The props' poses travel in the state, so this compares them too.
        AJFX_CHECK(a.state() == b.state());
        AJFX_CHECK(a.state() == c.state());
        AJFX_CHECK(!a.state().emitters[0].props.empty());
    }

    // A seek through the tip-over lands on the bits stepping there gives.
    Simulation seeker = make(effect);
    CheckpointRing ring{30U, 8U};
    for (const std::uint64_t target : {150U, 60U, 220U, 20U}) {
        ring.seek(seeker, target, nullptr);
        Simulation stepped = make(effect);
        while (stepped.tick() < target) {
            stepped.step();
        }
        AJFX_CHECK(seeker.state() == stepped.state());
    }
}

void impossible_props_are_refused() {
    const auto refused = [](const RigidProp& prop, const std::string& fragment) {
        EffectBuilder builder{"x"};
        builder.emitter("rocks").init(Lifetime{}).update(RigidPhysics{.props = {prop}});
        const Result<Effect> effect = builder.build();
        if (effect.ok() || effect.error().message.find(fragment) == std::string::npos) {
            std::cout << "  expected '" << fragment << "', got: "
                      << (effect.ok() ? std::string{"accepted"} : effect.error().message) << '\n';
            return false;
        }
        return true;
    };
    // A collision mesh can only stand still: it reports no mass, so with
    // assertions off a moving one would take an infinite inverse mass rather
    // than complain. Both ways of moving are refused, not just the obvious
    // one -- a driven mesh prop is the trap, since it looks harmless.
    for (const PropMotion motion : {PropMotion::Loose, PropMotion::Driven}) {
        RigidProp moving_mesh{};
        moving_mesh.mesh = "bucket";
        moving_mesh.shape = PropShape::Mesh;
        moving_mesh.motion = motion;
        AJFX_CHECK(refused(moving_mesh, "must be 'fixed'"));
    }
    // Standing still, the same prop is exactly what a mesh shape is for.
    RigidProp fixed_mesh{};
    fixed_mesh.mesh = "chip";
    fixed_mesh.shape = PropShape::Mesh;
    fixed_mesh.motion = PropMotion::Fixed;
    EffectBuilder standing{"x"};
    standing.emitter("rocks").init(Lifetime{}).update(RigidPhysics{.props = {fixed_mesh}});
    AJFX_CHECK(standing.build().ok());

    RigidProp no_mesh{};
    no_mesh.shape = PropShape::Hull;
    AJFX_CHECK(refused(no_mesh, "props[0].mesh"));

    RigidProp thick{};
    thick.shape = PropShape::HollowCylinder;
    thick.radius = 0.2F;
    thick.height = 0.5F;
    thick.wall = 0.3F;
    AJFX_CHECK(refused(thick, "thinner than the prop's radius"));

    RigidProp flat{};
    flat.shape = PropShape::Cylinder;
    flat.radius = 0.2F;
    flat.height = 0.0F;
    AJFX_CHECK(refused(flat, "props[0].height"));

    RigidProp tiny{};
    tiny.shape = PropShape::Box;
    tiny.size = 0.0F;
    AJFX_CHECK(refused(tiny, "props[0].size"));

    RigidProp coarse{};
    coarse.shape = PropShape::Pipe;
    coarse.radius = 0.3F;
    coarse.height = 1.0F;
    coarse.wall = 0.05F;
    coarse.segments = 2U;
    AJFX_CHECK(refused(coarse, "props[0].segments"));

    // A crowd of props is a budget, like a crowd of particles.
    EffectBuilder many{"x"};
    RigidProp one{};
    one.shape = PropShape::Sphere;
    one.radius = 0.1F;
    std::vector<RigidProp> crowd(max_rigid_props + 1U, one);
    many.emitter("rocks").init(Lifetime{}).update(RigidPhysics{.props = std::move(crowd)});
    const Result<Effect> crowded = many.build();
    AJFX_CHECK(!crowded.ok() && crowded.error().message.find("more props than") != std::string::npos);

    // A prop shaped by a mesh the package does not hold names that mesh.
    EffectBuilder missing{"x"};
    RigidProp hull{};
    hull.mesh = "bucket";
    hull.shape = PropShape::Hull;
    missing.emitter("rocks").init(Lifetime{}).update(RigidPhysics{.props = {hull}});
    const Result<Simulation> no_mesh_given =
        Simulation::create(missing.build().value(), 3U, SimulationOptions{.rigid = g_solver.get()});
    AJFX_CHECK(!no_mesh_given.ok() && no_mesh_given.error().code == ErrorCode::MissingMesh);
    AJFX_CHECK(no_mesh_given.error().message.find("prop shaped by mesh 'bucket'") != std::string::npos);
}

} // namespace

bool same_particles(const EmitterState& a, const EmitterState& b) {
    return a.position == b.position && a.orientation == b.orientation && a.velocity == b.velocity &&
           a.angular_velocity == b.angular_velocity && a.age == b.age && a.spawn_index == b.spawn_index &&
           a.size == b.size && a.color == b.color && a.spawned == b.spawned;
}

// 150 blocks 0.15 to `grain` across heaped on the floor, held until
// `release`. Their launch is ignored: a pile body starts still.
Effect heap(float release, float jitter) {
    EffectBuilder builder{"heap"};
    builder.float_parameter("grain", 0.2F, 0.15F, 0.4F);
    builder.emitter("stones")
        .capacity(256U)
        .seed(5U)
        .spawn(SpawnPile{.count = 150U,
                         .center = Vec3{0.5F, 0.0F, -0.25F},
                         .slope = 60.0F,
                         .jitter = jitter,
                         .release = release})
        .init(Lifetime{.min = 30.0F, .max = 30.0F})
        .init(Velocity{.speed_min = 3.0F, .speed_max = 3.0F})
        .init(InitSize{.min = 0.15F, .max = bind("grain")})
        .init(InitTumble{.speed_min = 90.0F, .speed_max = 180.0F})
        .update(Gravity{})
        .update(RigidPhysics{.shape = RigidShape::Box, .friction = 0.6F, .restitution = 0.1F})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 0.8F});
    return builder.build().value();
}

class Grain final : public ParameterProvider {
public:
    explicit Grain(float value) : value_{value} {}
    void values_at(std::uint64_t, std::span<ParameterValue> values) const override { values[0][0] = value_; }

private:
    float value_;
};

struct Spread final {
    float top{0.0F};
    float widest{0.0F};
};

Spread spread(const EmitterState& s, Vec3 center) {
    Spread out;
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        const Vec3 d = s.position[i] - center;
        out.top = std::max(out.top, d.y);
        out.widest = std::max(out.widest, std::sqrt(d.x * d.x + d.z * d.z));
    }
    return out;
}

void piles_stand_from_the_start_until_released() {
    const Vec3 center{0.5F, 0.0F, -0.25F};
    {
        // Without jitter the lattice is exact: no two bodies closer than the
        // spacing, which is the largest body's diagonal.
        Simulation simulation = make(heap(0.5F, 0.0F));
        const EmitterState& s = simulation.emitter(0);
        AJFX_CHECK(simulation.tick() == 0U);
        AJFX_CHECK(s.alive() == 150U && s.spawned == 150U);
        float largest = 0.0F;
        for (std::size_t i = 0U; i < s.alive(); ++i) {
            largest = std::max(largest, s.size[i]);
            AJFX_CHECK(s.age[i] == 0.0F);
            AJFX_CHECK(length(s.velocity[i]) == 0.0F && length(s.angular_velocity[i]) == 0.0F);
        }
        const float d = largest * std::sqrt(3.0F);
        float closest = 1.0e9F;
        for (std::size_t i = 0U; i < s.alive(); ++i) {
            for (std::size_t j = 0U; j < i; ++j) {
                closest = std::min(closest, length(s.position[i] - s.position[j]));
            }
            // A cone of 60 degrees: everything under the top, by its slope.
            AJFX_CHECK(s.position[i].y >= 0.5F * d - 1.0e-4F);
        }
        const Spread start = spread(s, center);
        std::cout << "  spacing " << d << ", closest " << closest << ", heap " << start.top << " high and "
                  << start.widest << " out\n";
        AJFX_CHECK(closest >= d * 0.9999F);
        AJFX_CHECK(start.top > 3.0F * d);
        AJFX_CHECK(start.widest < start.top);
    }

    Simulation simulation = make(heap(0.5F, 0.2F));
    const EmitterState& s = simulation.emitter(0);
    const std::vector<Vec3> at_start = s.position;
    const std::vector<Quat> turned = s.orientation;
    const Spread start = spread(s, center);
    // Held for 0.5 s: 30 steps leave every body exactly where it was.
    simulation.advance_to(30U);
    AJFX_CHECK(s.position == at_start && s.orientation == turned);
    // Let go, the heap slumps: lower, and no narrower.
    simulation.advance_to(150U);
    const Spread after = spread(s, center);
    std::cout << "  released: top " << start.top << " -> " << after.top << ", out " << start.widest << " -> "
              << after.widest << '\n';
    AJFX_CHECK(after.top < start.top - 0.2F);
    AJFX_CHECK(after.widest >= start.widest);
    AJFX_CHECK(s.alive() == 150U);
}

void piles_seek_and_follow_their_parameters() {
    const Effect effect = heap(0.25F, 0.2F);
    const Grain grain{0.3F};
    // Stepped from creation: the first step sets the pile up again with
    // the provider's value.
    Simulation stepped = make(effect);
    stepped.advance_to(200U, &grain);

    // Reset with the provider: the pile at step 0 already uses it.
    Simulation reset = make(effect);
    reset.reset(&grain);
    float largest = 0.0F;
    for (const float size : reset.emitter(0).size) {
        largest = std::max(largest, size);
    }
    AJFX_CHECK(largest > 0.2F && largest <= 0.3F);
    // Saved while held, restored into a fresh simulation, it carries on
    // through the release exactly.
    reset.advance_to(10U, &grain);
    const SimulationState held = reset.state();
    Simulation restored = make(effect);
    restored.restore(held);
    restored.advance_to(200U, &grain);
    AJFX_CHECK(same_particles(restored.emitter(0), stepped.emitter(0)));

    // Seeks back and forth across the release land on the same bits.
    Simulation seeking = make(effect);
    CheckpointRing ring{40U, 8U};
    ring.seek(seeking, 200U, &grain);
    ring.seek(seeking, 5U, &grain);
    ring.seek(seeking, 0U, &grain);
    // Back at the start, the pile is the provider's, not the defaults'.
    AJFX_CHECK(seeking.emitter(0).position != make(effect).emitter(0).position);
    ring.seek(seeking, 200U, &grain);
    AJFX_CHECK(same_particles(seeking.emitter(0), stepped.emitter(0)));
}

void piles_need_rigid_physics_and_room() {
    {
        EffectBuilder builder{"loose"};
        builder.emitter("stones")
            .capacity(64U)
            .spawn(SpawnPile{.count = 10U})
            .init(Lifetime{.min = 5.0F, .max = 5.0F})
            .update(Gravity{});
        const Result<Effect> built = builder.build();
        AJFX_CHECK(!built.ok());
        std::cout << "  " << built.error().message << '\n';
    }
    {
        EffectBuilder builder{"crowded"};
        builder.emitter("stones")
            .capacity(64U)
            .spawn(SpawnPile{.count = 100U})
            .init(Lifetime{.min = 5.0F, .max = 5.0F})
            .update(RigidPhysics{.shape = RigidShape::Sphere});
        const Result<Effect> built = builder.build();
        AJFX_CHECK(!built.ok());
        std::cout << "  " << built.error().message << '\n';
    }
    {
        // A box pile of 4 by 2 by however high, aligned when it has no
        // jitter.
        EffectBuilder builder{"wall"};
        builder.emitter("bricks")
            .capacity(64U)
            .spawn(SpawnPile{.count = 20U, .shape = PileShape::Box, .width = 2.0F, .depth = 1.0F, .jitter = 0.0F})
            .init(Lifetime{.min = 5.0F, .max = 5.0F})
            .init(InitSize{.min = 0.5F, .max = 0.5F})
            .update(RigidPhysics{.shape = RigidShape::Sphere});
        Simulation simulation = make(builder.build().value());
        const EmitterState& s = simulation.emitter(0);
        AJFX_CHECK(s.alive() == 20U);
        // Spheres reach their own size: 4 across, 2 deep, layers 0.5 apart.
        AJFX_CHECK(s.position[0] == (Vec3{-0.75F, 0.25F, -0.25F}));
        AJFX_CHECK(s.position[3] == (Vec3{0.75F, 0.25F, -0.25F}));
        AJFX_CHECK(s.position[4] == (Vec3{-0.75F, 0.25F, 0.25F}));
        AJFX_CHECK(s.position[8] == (Vec3{-0.75F, 0.75F, -0.25F}));
        AJFX_CHECK(s.position[19] == (Vec3{0.75F, 1.25F, -0.25F}));
    }
}

// Two emitters, the first not rigid: a track's frames keep the effect's
// emitter order and leave it empty.
Effect mixed() {
    Effect effect = heap(0.25F, 0.2F);
    Emitter sparks;
    sparks.name = "sparks";
    sparks.capacity = 16U;
    sparks.spawn.push_back(SpawnBurst{.id = 1U, .count = 4U});
    sparks.init.push_back(Lifetime{.id = 2U, .min = 1.0F, .max = 1.0F});
    effect.emitters.insert(effect.emitters.begin(), sparks);
    AJFX_CHECK(validate(effect).ok());
    return effect;
}

void runner_matches_the_simulation() {
    const Effect effect = mixed();
    RigidRunner runner{*g_solver, RigidRunnerOptions{.lead = 20U, .checkpoint_interval = 30U, .checkpoint_count = 3U,
                                                     .turn = 5U}};
    auto grain = std::make_shared<Grain>(0.3F);
    std::unique_ptr<RigidTrack> track = runner.track(effect, 3U, {}, grain);

    const auto reference = [&](std::uint64_t tick, const ParameterProvider* parameters) {
        Simulation simulation = make(effect);
        simulation.reset(parameters);
        simulation.advance_to(tick, parameters);
        return simulation.emitter(1);
    };
    const auto check_at = [&](std::uint64_t tick, const ParameterProvider* parameters) {
        std::shared_ptr<const RigidFrame> frame = track->frame(tick);
        if (!frame) {
            runner.wait_idle();
            frame = track->frame(tick);
        }
        AJFX_CHECK(frame != nullptr && frame->tick == tick);
        AJFX_CHECK(frame->emitters.size() == 2U && frame->emitters[0].alive() == 0U);
        AJFX_CHECK(same_particles(frame->emitters[1], reference(tick, parameters)));
    };

    // Nothing is worked out before it is asked for.
    AJFX_CHECK(track->frame(0U) == nullptr || track->frame(0U)->tick == 0U);
    check_at(0U, grain.get());
    check_at(45U, grain.get());
    // The lead: the next ticks are ready without waiting.
    runner.wait_idle();
    AJFX_CHECK(track->frame(46U) != nullptr);
    AJFX_CHECK(track->frame(65U) != nullptr);
    check_at(66U, grain.get());
    check_at(140U, grain.get());
    // Back, from a checkpoint, and forward again.
    check_at(20U, grain.get());
    check_at(140U, grain.get());
    AJFX_CHECK(!track->error());

    // New parameters drop the old past.
    auto fine = std::make_shared<Grain>(0.16F);
    track->set_parameters(fine);
    check_at(100U, fine.get());
}

void runner_keeps_serving_beside_a_failed_track() {
    RigidRunner runner{*g_solver};
    // A hull needs its mesh, and none was given.
    EffectBuilder builder{"chips"};
    builder.emitter("chips")
        .capacity(16U)
        .spawn(SpawnBurst{.count = 4U})
        .init(Lifetime{.min = 2.0F, .max = 2.0F})
        .update(RigidPhysics{})
        .renderer(SpriteRenderer{.geometry = Geometry::Mesh, .mesh = "chip"});
    std::unique_ptr<RigidTrack> broken = runner.track(builder.build().value(), 1U, {});
    std::unique_ptr<RigidTrack> good = runner.track(heap(0.0F, 0.2F), 1U, {});
    // One dropped while it works must not stall the runner.
    std::unique_ptr<RigidTrack> dropped = runner.track(heap(0.0F, 0.2F), 2U, {});
    AJFX_CHECK(broken->frame(10U) == nullptr);
    (void)good->frame(30U);
    (void)dropped->frame(600U);
    dropped.reset();
    runner.wait_idle();
    AJFX_CHECK(broken->frame(10U) == nullptr);
    AJFX_CHECK(broken->error().has_value() && broken->error()->code == ErrorCode::MissingMesh);
    std::cout << "  " << broken->error()->message << '\n';
    AJFX_CHECK(good->frame(30U) != nullptr && !good->error());
}

// ------------------------------------------------------------ contacts

Simulation with_contacts(const Effect& effect, ContactOptions contacts,
                         const RigidSolver* solver = g_solver.get()) {
    Result<Simulation> simulation =
        Simulation::create(effect, 3U, SimulationOptions{.rigid = solver, .contacts = contacts});
    if (!simulation) {
        std::cout << simulation.error().message << '\n';
    }
    AJFX_CHECK(simulation.ok());
    return std::move(simulation).value();
}

// One block half a unit across dropped on the floor, with nothing else in
// the world for it to hit.
Effect one_block(float from_height) {
    EffectBuilder builder{"drop"};
    builder.emitter("block")
        .capacity(2U)
        .spawn(SpawnBurst{.time = 0.0F, .count = 1U})
        .init(Lifetime{.min = 30.0F, .max = 30.0F})
        .init(ShapeLocation{.shape = Shape::Point, .center = Vec3{0.0F, from_height, 0.0F}})
        .init(InitSize{.min = 0.5F, .max = 0.5F})
        .update(Gravity{})
        .update(RigidPhysics{.shape = RigidShape::Box, .friction = 0.8F, .restitution = 0.0F})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 0.8F});
    return builder.build().value();
}

void contacts_report_the_hit_and_not_the_rest() {
    // From 1.5 up the block's underside falls 1.25 to the floor, arriving at
    // about 4.95 units a second.
    Simulation simulation = with_contacts(one_block(1.5F), ContactOptions{.min_speed = 0.5F});
    std::optional<RigidContact> hit;
    std::uint64_t hit_tick = 0U;
    std::uint64_t reporting = 0U;
    for (std::uint64_t tick = 1U; tick <= 180U; ++tick) {
        simulation.advance_to(tick);
        const std::span<const RigidContact> found = simulation.contacts(0U);
        if (found.empty()) {
            continue;
        }
        ++reporting;
        if (!hit) {
            hit = found.front();
            hit_tick = tick;
        }
    }
    AJFX_CHECK(hit.has_value());
    std::cout << "  it met the floor at step " << hit_tick << " at " << hit->speed
              << " units a second, reported in " << reporting << " step(s)\n";
    // The block is body 0 and the floor is the emitter's only static. The
    // body is the lower side, so the normal points from it to the floor.
    AJFX_CHECK(hit->a_is == RigidSide::Body && hit->a == 0U);
    AJFX_CHECK(hit->b_is == RigidSide::Static && hit->b == 0U);
    AJFX_CHECK(hit->began);
    AJFX_CHECK(hit->normal.y < -0.99F);
    AJFX_CHECK(std::fabs(hit->point.y) < 0.05F);
    AJFX_CHECK(hit->speed > 4.0F && hit->speed < 5.5F);
    // The hit is over in a few steps: the block then lies on the floor for
    // another two seconds, closing at nothing, and nothing is reported.
    AJFX_CHECK(reporting < 20U);
    AJFX_CHECK(simulation.contacts(0U).empty());
    // A simulation that did not ask collects nothing at all.
    Simulation quiet = make(one_block(1.5F));
    quiet.advance_to(180U);
    AJFX_CHECK(quiet.contacts(0U).empty());
}

void contacts_are_the_same_on_any_thread_count() {
    const Effect effect = pile();
    const ContactOptions options{.min_speed = 0.5F, .limit = 512U};
    const std::unique_ptr<RigidSolver> four = rigid::make_jolt_solver(rigid::JoltOptions{.threads = 4U});
    Simulation one_thread = with_contacts(effect, options);
    Simulation four_threads = with_contacts(effect, options, four.get());
    std::size_t total = 0U;
    for (std::uint64_t tick = 1U; tick <= 150U; ++tick) {
        one_thread.advance_to(tick);
        four_threads.advance_to(tick);
        const std::span<const RigidContact> a = one_thread.contacts(0U);
        const std::span<const RigidContact> b = four_threads.contacts(0U);
        AJFX_CHECK(std::is_sorted(a.begin(), a.end(), contact_before));
        AJFX_CHECK(a.size() == b.size());
        AJFX_CHECK(std::equal(a.begin(), a.end(), b.begin()));
        AJFX_CHECK(one_thread.contacts_dropped(0U) == 0U);
        total += a.size();
    }
    std::cout << "  " << total << " contacts over 150 steps, ordered, and the same on 1 and 4 threads\n";
    AJFX_CHECK(total > 50U);
}

void contacts_survive_a_seek() {
    const Effect effect = pile();
    const ContactOptions options{.min_speed = 0.5F, .limit = 512U};
    Simulation stepped = with_contacts(effect, options);
    stepped.advance_to(60U);
    const SimulationState at_60 = stepped.state();
    Simulation seeker = with_contacts(effect, options);
    seeker.restore(at_60);
    // A restore has taken no step, so it has nothing to report yet.
    AJFX_CHECK(seeker.contacts(0U).empty());
    std::size_t total = 0U;
    for (std::uint64_t tick = 61U; tick <= 150U; ++tick) {
        stepped.advance_to(tick);
        seeker.advance_to(tick);
        const std::span<const RigidContact> a = stepped.contacts(0U);
        const std::span<const RigidContact> b = seeker.contacts(0U);
        AJFX_CHECK(a.size() == b.size());
        AJFX_CHECK(std::equal(a.begin(), a.end(), b.begin()));
        total += a.size();
    }
    std::cout << "  " << total << " contacts matched step for step after a seek to 60\n";
    AJFX_CHECK(total > 30U);
}

void contacts_over_the_limit_are_counted() {
    // Every touch, resting ones included, from a pile still settling: far
    // more at a time than a limit of four.
    Simulation simulation = with_contacts(pile(), ContactOptions{.min_speed = 0.0F, .limit = 4U});
    std::uint32_t most_dropped = 0U;
    for (std::uint64_t tick = 1U; tick <= 150U; ++tick) {
        simulation.advance_to(tick);
        AJFX_CHECK(simulation.contacts(0U).size() <= 4U);
        most_dropped = std::max(most_dropped, simulation.contacts_dropped(0U));
    }
    std::cout << "  at its busiest the step had " << most_dropped << " contacts beyond the four reported\n";
    AJFX_CHECK(most_dropped > 0U);
    AJFX_CHECK(simulation.contacts(0U).size() == 4U);
}

void contacts_name_the_prop_that_was_hit() {
    // Balls stacked inside a fixed bucket and let go: they settle against
    // its floor, its staves and each other.
    Simulation simulation =
        with_contacts(bucket_of_balls(PropMotion::Fixed, 0.0F), ContactOptions{.min_speed = 0.0F, .limit = 512U});
    bool met_the_bucket = false;
    bool met_each_other = false;
    for (std::uint64_t tick = 1U; tick <= 120U; ++tick) {
        simulation.advance_to(tick);
        for (const RigidContact& contact : simulation.contacts(0U)) {
            met_the_bucket = met_the_bucket || (contact.b_is == RigidSide::Prop && contact.b == 0U);
            met_each_other = met_each_other || (contact.a_is == RigidSide::Body && contact.b_is == RigidSide::Body);
        }
    }
    AJFX_CHECK(met_the_bucket);
    AJFX_CHECK(met_each_other);
}

// ------------------------------------------------------------ body kinds

// A wall of nine crates with a ball dropped on it, in one world: the crates
// are the emitter's own kind (boxes that hardly bounce) and the ball is a
// kind of its own, a sphere whose bounce is `bounce`.
Effect crates_and_a_ball(float bounce) {
    EffectBuilder builder{"crates"};
    builder.emitter("pieces")
        .capacity(16U)
        .spawn(SpawnPile{.count = 9U,
                         .shape = PileShape::Box,
                         .width = 1.5F,
                         .depth = 1.5F,
                         .spacing = 1.0F,
                         .jitter = 0.0F,
                         .release = 0.0F,
                         .body = 0U})
        .spawn(SpawnBurst{.time = 0.1F, .count = 1U, .body = 1U})
        .init(Lifetime{.min = 30.0F, .max = 30.0F})
        .init(ShapeLocation{.shape = Shape::Point, .center = Vec3{0.0F, 2.0F, 0.0F}})
        .init(Velocity{.speed_min = 0.0F, .speed_max = 0.0F})
        .init(InitSize{.min = 0.25F, .max = 0.25F})
        .update(Gravity{})
        .update(RigidPhysics{.shape = RigidShape::Box,
                             .friction = 0.9F,
                             .restitution = 0.05F,
                             .bodies = {RigidBodyKind{.shape = RigidShape::Sphere,
                                                      .friction = 0.2F,
                                                      .restitution = bounce}}})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 0.9F});
    return builder.build().value();
}

// How low the ball got and how high it came back after that.
struct Bounce final {
    float lowest{0.0F};
    float rebound{0.0F};
};

Bounce ball_rebound(float bounce) {
    Simulation simulation = make(crates_and_a_ball(bounce));
    float lowest = 1.0e9F;
    float rebound = 0.0F;
    for (std::uint64_t tick = 1U; tick <= 300U; ++tick) {
        simulation.advance_to(tick);
        const EmitterState& s = simulation.emitter(0);
        // The pile's bodies are born first, so the ball is the tenth.
        if (s.alive() < 10U) {
            continue;
        }
        const float y = s.position[9].y;
        if (y < lowest) {
            lowest = y;
            rebound = 0.0F;
        }
        rebound = std::max(rebound, y - lowest);
    }
    return Bounce{lowest, rebound};
}

void body_kinds_mix_shapes_and_materials() {
    // One world, two kinds. The ball's own restitution decides its bounce,
    // although every crate it lands among hardly bounces at all.
    const Bounce bouncy = ball_rebound(0.9F);
    const Bounce dull = ball_rebound(0.05F);
    std::cout << "  a bouncy ball came back " << bouncy.rebound << " above its lowest point, a dull one " << dull.rebound
              << '\n';
    AJFX_CHECK(bouncy.rebound > 0.2F);
    AJFX_CHECK(bouncy.rebound > dull.rebound * 2.0F);

    // And the crates are still boxes: they lie flat on the floor, asleep,
    // rather than rolling as spheres of the same size would.
    Simulation simulation = make(crates_and_a_ball(0.9F));
    simulation.advance_to(300U);
    const EmitterState& s = simulation.emitter(0);
    AJFX_CHECK(s.alive() == 10U);
    AJFX_CHECK(s.body_kind.size() == 10U);
    for (std::size_t i = 0U; i < 9U; ++i) {
        AJFX_CHECK(s.body_kind[i] == 0U);
    }
    AJFX_CHECK(s.body_kind[9] == 1U);
    std::size_t resting = 0U;
    for (std::size_t i = 0U; i < 9U; ++i) {
        if (length(s.velocity[i]) == 0.0F) {
            ++resting;
        }
    }
    std::cout << "  " << resting << " of the nine crates lie still\n";
    AJFX_CHECK(resting >= 7U);
}

void body_kinds_rerun_and_seek_exactly() {
    const Effect effect = crates_and_a_ball(0.9F);
    const std::unique_ptr<RigidSolver> four = rigid::make_jolt_solver(rigid::JoltOptions{.threads = 4U});
    Simulation a = make(effect);
    Simulation b = make(effect);
    Simulation c = make(effect, four.get());
    for (const std::uint64_t tick : {1U, 40U, 120U, 300U}) {
        a.advance_to(tick);
        b.advance_to(tick);
        c.advance_to(tick);
        AJFX_CHECK(a.state() == b.state());
        AJFX_CHECK(a.state() == c.state());
    }
    // A seek rebuilds the world, each body back as the kind it was.
    Simulation stepped = make(effect);
    stepped.advance_to(90U);
    const SimulationState at_90 = stepped.state();
    Simulation seeker = make(effect);
    seeker.restore(at_90);
    AJFX_CHECK(seeker.state() == at_90);
    seeker.advance_to(300U);
    Simulation straight = make(effect);
    straight.advance_to(300U);
    AJFX_CHECK(seeker.state() == straight.state());
}

void impossible_body_kinds_are_refused() {
    const auto refused = [](const Result<Effect>& built, const std::string& fragment) {
        if (built.ok() || built.error().message.find(fragment) == std::string::npos) {
            std::cout << "  expected '" << fragment << "', got: "
                      << (built.ok() ? std::string{"accepted"} : built.error().message) << '\n';
            return false;
        }
        return true;
    };
    // A spawn module may only name a kind the emitter has.
    EffectBuilder missing{"x"};
    missing.emitter("e")
        .spawn(SpawnBurst{.count = 1U, .body = 2U})
        .init(Lifetime{})
        .update(RigidPhysics{.bodies = {RigidBodyKind{}}});
    AJFX_CHECK(refused(missing.build(), "spawns body kind 2, and rigid physics has 2"));

    // And naming one at all needs a physics world to make bodies in.
    EffectBuilder no_physics{"x"};
    no_physics.emitter("e").spawn(SpawnBurst{.count = 1U, .body = 1U}).init(Lifetime{});
    AJFX_CHECK(refused(no_physics.build(), "no 'rigid_physics' module to make bodies"));

    // A kind's material is held to what any other module's is.
    EffectBuilder slippery{"x"};
    slippery.emitter("e").init(Lifetime{}).update(RigidPhysics{.bodies = {RigidBodyKind{.friction = 1.5F}}});
    AJFX_CHECK(refused(slippery.build(), "bodies[0].friction"));

    // More kinds than an emitter may have.
    EffectBuilder crowded{"x"};
    std::vector<RigidBodyKind> many(max_rigid_body_kinds + 1U);
    crowded.emitter("e").init(Lifetime{}).update(RigidPhysics{.bodies = many});
    AJFX_CHECK(refused(crowded.build(), "more kinds of body than"));

    // A kind that collides as a hull and names no model, where the renderer
    // draws none either, has nothing to wrap.
    EffectBuilder nothing_to_wrap{"x"};
    nothing_to_wrap.emitter("e").init(Lifetime{}).update(
        RigidPhysics{.bodies = {RigidBodyKind{.shape = RigidShape::Hull}}});
    AJFX_CHECK(refused(nothing_to_wrap.build(), "collides as a hull and names no model"));

    // Naming one it can wrap is fine, and the mesh is then asked for by name.
    EffectBuilder named{"x"};
    named.emitter("e")
        .capacity(8U)
        .spawn(SpawnBurst{.count = 1U, .body = 1U})
        .init(Lifetime{})
        .update(RigidPhysics{.bodies = {RigidBodyKind{.shape = RigidShape::Hull, .mesh = "chip"}}});
    const Result<Effect> with_hull = named.build();
    AJFX_CHECK(with_hull.ok());
    const Result<Simulation> without_mesh =
        Simulation::create(with_hull.value(), 1U, SimulationOptions{.rigid = g_solver.get()});
    AJFX_CHECK(!without_mesh.ok() && without_mesh.error().code == ErrorCode::MissingMesh);
    std::cout << "  " << without_mesh.error().message << '\n';
    const Result<Simulation> with_mesh = Simulation::create(
        with_hull.value(), 1U, SimulationOptions{.meshes = presets::meshes(), .rigid = g_solver.get()});
    AJFX_CHECK(with_mesh.ok());
}

// ---------------------------------------------------------------- breaking

// The six panels a crate comes apart into, each a shard model's hull at a
// face of the parent, a little smaller than the face it was.
std::vector<BreakPiece> crate_panels(std::uint32_t shard_kind) {
    std::vector<BreakPiece> pieces;
    const Vec3 faces[6] = {{0.5F, 0.0F, 0.0F},  {-0.5F, 0.0F, 0.0F}, {0.0F, 0.5F, 0.0F},
                           {0.0F, -0.5F, 0.0F}, {0.0F, 0.0F, 0.5F},  {0.0F, 0.0F, -0.5F}};
    const Vec3 turns[6] = {{0.0F, 0.0F, 90.0F}, {0.0F, 0.0F, 90.0F}, {0.0F, 0.0F, 0.0F},
                           {0.0F, 0.0F, 0.0F},  {90.0F, 0.0F, 0.0F}, {90.0F, 0.0F, 0.0F}};
    for (std::size_t i = 0U; i < 6U; ++i) {
        pieces.push_back(BreakPiece{.body = shard_kind, .offset = faces[i], .turn = turns[i], .size = 0.4F});
    }
    return pieces;
}

// Two balls of one emitter dropped side by side, alike but for the size
// their kinds are: one at the particle's own size and one at `scale` of it.
// An emitter has one size at birth, so without a kind's own scale these two
// could only be the same ball twice.
Effect two_balls_of_different_kinds(float scale) {
    EffectBuilder builder{"scaled"};
    builder.emitter("balls")
        .capacity(8U)
        // A pile of one places its body at its own centre, so the two stand
        // apart and fall without meeting each other.
        .spawn(SpawnPile{.count = 1U, .center = Vec3{-1.0F, 1.0F, 0.0F}, .jitter = 0.0F, .release = 0.0F, .body = 1U})
        .spawn(SpawnPile{.count = 1U, .center = Vec3{1.0F, 1.0F, 0.0F}, .jitter = 0.0F, .release = 0.0F, .body = 2U})
        .init(Lifetime{.min = 30.0F, .max = 30.0F})
        .init(ShapeLocation{.shape = Shape::Point})
        .init(Velocity{.speed_min = 0.0F, .speed_max = 0.0F})
        .init(InitSize{.min = 0.4F, .max = 0.4F})
        .update(Gravity{})
        .update(RigidPhysics{.shape = RigidShape::Sphere,
                             .bodies = {RigidBodyKind{.shape = RigidShape::Sphere},
                                        RigidBodyKind{.shape = RigidShape::Sphere, .scale = scale}}})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 0.9F});
    return builder.build().value();
}

void a_kinds_own_size_decides_the_body_it_is() {
    // A ball rests with its middle a radius above the floor, and a sphere of
    // size s has radius s / 2. At size 0.4 the unscaled ball must come to
    // rest at 0.2 and the half-size one at 0.1, which is the whole claim:
    // the kind's scale reached the body the solver built.
    Simulation simulation = make(two_balls_of_different_kinds(0.5F));
    simulation.advance_to(240U);
    const EmitterState& s = simulation.emitter(0);
    AJFX_CHECK(s.alive() == 2U);
    const float full = s.position[0].y;
    const float half = s.position[1].y;
    std::cout << "  a ball of size 0.4 rests at " << full << ", the same ball at half the size at " << half << '\n';
    AJFX_CHECK(std::abs(full - 0.2F) < 0.01F);
    AJFX_CHECK(std::abs(half - 0.1F) < 0.01F);
    // And the two are twice one another, worked out from the poses rather
    // than from the numbers above.
    AJFX_CHECK(std::abs(full / half - 2.0F) < 0.1F);

    // A scale of 1 is the size the emitter gave, so both rest alike.
    Simulation plain = make(two_balls_of_different_kinds(1.0F));
    plain.advance_to(240U);
    const EmitterState& p = plain.emitter(0);
    AJFX_CHECK(std::abs(p.position[0].y - p.position[1].y) < 0.001F);
    AJFX_CHECK(std::abs(p.position[0].y - 0.2F) < 0.01F);
}

void kind_scales_rerun_and_seek_exactly() {
    const Effect effect = two_balls_of_different_kinds(0.35F);
    const std::unique_ptr<RigidSolver> four = rigid::make_jolt_solver(rigid::JoltOptions{.threads = 4U});
    Simulation a = make(effect);
    Simulation b = make(effect);
    Simulation c = make(effect, four.get());
    for (const std::uint64_t tick : {1U, 30U, 120U, 240U}) {
        a.advance_to(tick);
        b.advance_to(tick);
        c.advance_to(tick);
        AJFX_CHECK(a.state() == b.state());
        AJFX_CHECK(a.state() == c.state());
    }
    // A seek rebuilds the world, so each body has to come back the size its
    // kind says and not the size the emitter gave.
    Simulation stepped = make(effect);
    stepped.advance_to(60U);
    const SimulationState at_60 = stepped.state();
    Simulation seeker = make(effect);
    seeker.restore(at_60);
    AJFX_CHECK(seeker.state() == at_60);
    seeker.advance_to(240U);
    Simulation straight = make(effect);
    straight.advance_to(240U);
    AJFX_CHECK(seeker.state() == straight.state());
}

void impossible_kind_scales_are_refused() {
    const auto refused = [](const Result<Effect>& built, const std::string& fragment) {
        if (built.ok() || built.error().message.find(fragment) == std::string::npos) {
            std::cout << "  expected '" << fragment << "', got: "
                      << (built.ok() ? std::string{"accepted"} : built.error().message) << '\n';
            return false;
        }
        std::cout << "  " << built.error().message << '\n';
        return true;
    };
    // A body at no size has no shape to build, and an engine would answer
    // for it in its own way rather than complaining.
    for (const float scale : {0.0F, -1.0F, std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::quiet_NaN()}) {
        EffectBuilder builder{"x"};
        builder.emitter("e").init(Lifetime{}).update(
            RigidPhysics{.bodies = {RigidBodyKind{.shape = RigidShape::Sphere, .scale = scale}}});
        AJFX_CHECK(refused(builder.build(), "bodies[0].scale"));
    }
}

// A crate standing on the floor with a ball dropped on it from `height`. The
// crate is the emitter's own kind, the ball is a bouncy sphere, and the
// panels the crate breaks into are hulls of the chip model.
Effect crate_and_a_dropped_ball(float height, float threshold, std::uint32_t capacity) {
    EffectBuilder builder{"break"};
    builder.emitter("pieces")
        .capacity(capacity)
        .spawn(SpawnPile{.count = 1U, .shape = PileShape::Box, .jitter = 0.0F, .release = 0.0F, .body = 0U})
        .spawn(SpawnBurst{.time = 0.3F, .count = 1U, .body = 1U})
        .init(Lifetime{.min = 30.0F, .max = 30.0F})
        .init(ShapeLocation{.shape = Shape::Point, .center = Vec3{0.0F, height, 0.0F}})
        .init(Velocity{.speed_min = 0.0F, .speed_max = 0.0F})
        .init(InitSize{.min = 0.5F, .max = 0.5F})
        .update(Gravity{})
        .update(RigidPhysics{.shape = RigidShape::Box,
                             .friction = 0.7F,
                             .restitution = 0.05F,
                             .bodies = {RigidBodyKind{.shape = RigidShape::Sphere, .restitution = 0.8F},
                                        RigidBodyKind{.shape = RigidShape::Hull, .mesh = "chip"}}})
        .update(Break{.body = 0U,
                      .threshold = threshold,
                      .scatter = 1.0F,
                      .spin = 180.0F,
                      .pieces = crate_panels(2U)})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 0.8F});
    return builder.build().value();
}

Simulation breaking(const Effect& effect, const RigidSolver* solver = g_solver.get()) {
    Result<Simulation> simulation =
        Simulation::create(effect, 3U, SimulationOptions{.meshes = presets::meshes(), .rigid = solver});
    if (!simulation) {
        std::cout << simulation.error().message << '\n';
    }
    AJFX_CHECK(simulation.ok());
    return std::move(simulation).value();
}

void a_hit_crate_comes_apart_into_its_pieces() {
    Simulation simulation = breaking(crate_and_a_dropped_ball(4.0F, 4.0F, 32U));
    // The crate stands and the ball is on its way: two bodies, no pieces.
    simulation.advance_to(30U);
    const EmitterState& s = simulation.emitter(0);
    AJFX_CHECK(s.alive() == 2U);
    AJFX_CHECK(s.body_kind[0] == 0U && s.body_kind[1] == 1U);
    const Vec3 crate = s.position[0];

    // It breaks when the ball reaches it, into six panels and no more.
    std::uint64_t broke_at = 0U;
    for (std::uint64_t tick = 31U; tick <= 240U && broke_at == 0U; ++tick) {
        simulation.advance_to(tick);
        if (s.alive() != 2U) {
            broke_at = tick;
        }
    }
    AJFX_CHECK(broke_at > 0U);
    std::cout << "  the crate came apart at step " << broke_at << " into " << s.alive() - 1U << " pieces\n";
    AJFX_CHECK(s.alive() == 7U);
    // The ball is the oldest left, then the panels, every one a shard.
    AJFX_CHECK(s.body_kind[0] == 1U);
    for (std::size_t i = 1U; i < s.alive(); ++i) {
        AJFX_CHECK(s.body_kind[i] == 2U);
        // A panel starts where its face of the crate was, a quarter of a
        // unit out at most, and is smaller than the crate.
        const Vec3 from{s.position[i].x - crate.x, s.position[i].y - crate.y, s.position[i].z - crate.z};
        AJFX_CHECK(length(from) < 0.5F);
        AJFX_CHECK(s.size[i] < 0.5F);
    }
    // The pieces are new particles, and nothing was refused.
    AJFX_CHECK(s.spawned == 8U && s.refused == 0U);

    // They settle on the floor rather than sinking through it or buzzing.
    simulation.advance_to(600U);
    AJFX_CHECK(s.alive() == 7U);
    float lowest = 1.0e9F;
    std::size_t still = 0U;
    for (std::size_t i = 1U; i < s.alive(); ++i) {
        lowest = std::min(lowest, s.position[i].y);
        if (length(s.velocity[i]) == 0.0F) {
            ++still;
        }
    }
    std::cout << "  the lowest panel rests at " << lowest << ", " << still << " of six lie still\n";
    AJFX_CHECK(lowest > -0.05F);
    AJFX_CHECK(still >= 4U);

    // A gentler landing leaves it whole: the same shot with a threshold no
    // impact reaches breaks nothing at all.
    Simulation unbroken = breaking(crate_and_a_dropped_ball(4.0F, 40.0F, 32U));
    unbroken.advance_to(600U);
    AJFX_CHECK(unbroken.emitter(0).alive() == 2U);
    AJFX_CHECK(unbroken.emitter(0).refused == 0U);
}

void breaking_reruns_and_seeks_exactly() {
    const Effect effect = crate_and_a_dropped_ball(4.0F, 4.0F, 32U);
    const std::unique_ptr<RigidSolver> four = rigid::make_jolt_solver(rigid::JoltOptions{.threads = 4U});
    Simulation a = breaking(effect);
    Simulation b = breaking(effect);
    Simulation c = breaking(effect, four.get());
    for (const std::uint64_t tick : {1U, 60U, 120U, 400U}) {
        a.advance_to(tick);
        b.advance_to(tick);
        c.advance_to(tick);
        AJFX_CHECK(a.state() == b.state());
        AJFX_CHECK(a.state() == c.state());
    }
    AJFX_CHECK(a.emitter(0).alive() == 7U);

    // A seek over the break lands on what stepping through it gave: the
    // pieces' poses, and the hashed scatter and spin they left with.
    CheckpointRing ring{30U, 8U};
    for (const std::uint64_t target : {90U, 45U, 300U, 61U, 400U, 120U}) {
        Simulation seeker = breaking(effect);
        ring.seek(seeker, target);
        Simulation straight = breaking(effect);
        straight.advance_to(target);
        AJFX_CHECK(seeker.tick() == target);
        AJFX_CHECK(seeker.state() == straight.state());
    }
}

void a_break_with_no_room_is_refused_whole() {
    // Six panels need six places. With the crate and the ball holding two of
    // six, five come free at most, so the crate cannot break and says so.
    Simulation simulation = breaking(crate_and_a_dropped_ball(4.0F, 4.0F, 6U));
    simulation.advance_to(600U);
    const EmitterState& s = simulation.emitter(0);
    std::cout << "  " << s.alive() << " bodies left, " << s.refused << " pieces refused\n";
    AJFX_CHECK(s.alive() == 2U);
    AJFX_CHECK(s.body_kind[0] == 0U);
    AJFX_CHECK(s.refused >= 6U && s.refused % 6U == 0U);
    // One more place is all it needs: with seven the same shot breaks.
    Simulation roomier = breaking(crate_and_a_dropped_ball(4.0F, 4.0F, 7U));
    roomier.advance_to(600U);
    AJFX_CHECK(roomier.emitter(0).alive() == 7U);
    AJFX_CHECK(roomier.emitter(0).refused == 0U);
}

void impossible_breaks_are_refused() {
    const auto refused = [](const Result<Effect>& built, const std::string& fragment) {
        if (built.ok() || built.error().message.find(fragment) == std::string::npos) {
            std::cout << "  expected '" << fragment << "', got: "
                      << (built.ok() ? std::string{"accepted"} : built.error().message) << '\n';
            return false;
        }
        return true;
    };
    const auto with = [](const Break& breaking, std::vector<RigidBodyKind> kinds = {RigidBodyKind{}}) {
        EffectBuilder builder{"x"};
        builder.emitter("e")
            .capacity(64U)
            .init(Lifetime{})
            .update(RigidPhysics{.bodies = std::move(kinds)})
            .update(breaking);
        return builder.build();
    };
    // Only a body in a physics world can come apart.
    EffectBuilder alone{"x"};
    alone.emitter("e").init(Lifetime{}).update(Break{.pieces = {BreakPiece{}}});
    AJFX_CHECK(refused(alone.build(), "no 'rigid_physics' module"));

    AJFX_CHECK(refused(with(Break{.pieces = {}}), "needs at least one piece"));
    AJFX_CHECK(refused(with(Break{.threshold = 0.0F, .pieces = {BreakPiece{}}}), "threshold"));
    AJFX_CHECK(refused(with(Break{.pieces = {BreakPiece{.size = 0.0F}}}), "pieces[0].size"));
    AJFX_CHECK(refused(with(Break{.body = 4U, .pieces = {BreakPiece{}}}), "breaks body kind 4"));
    AJFX_CHECK(refused(with(Break{.pieces = {BreakPiece{.body = 7U}}}), "piece 0 is body kind 7"));
    std::vector<BreakPiece> crowd(max_break_pieces + 1U);
    AJFX_CHECK(refused(with(Break{.pieces = crowd}), "more than the"));

    // One kind cannot break two ways.
    EffectBuilder twice{"x"};
    twice.emitter("e")
        .capacity(64U)
        .init(Lifetime{})
        .update(RigidPhysics{})
        .update(Break{.pieces = {BreakPiece{}}})
        .update(Break{.pieces = {BreakPiece{}}});
    AJFX_CHECK(refused(twice.build(), "which another 'break' module already breaks"));

    // And breaking has to end: a kind whose pieces break back into it would
    // come apart for ever.
    EffectBuilder loop{"x"};
    loop.emitter("e")
        .capacity(64U)
        .init(Lifetime{})
        .update(RigidPhysics{.bodies = {RigidBodyKind{}}})
        .update(Break{.body = 0U, .pieces = {BreakPiece{.body = 1U}}})
        .update(Break{.body = 1U, .pieces = {BreakPiece{.body = 0U}}});
    AJFX_CHECK(refused(loop.build(), "breaking has to end"));

    // A piece that breaks into a kind that stops is fine.
    EffectBuilder twice_over{"x"};
    twice_over.emitter("e")
        .capacity(64U)
        .init(Lifetime{})
        .update(RigidPhysics{.bodies = {RigidBodyKind{}, RigidBodyKind{}}})
        .update(Break{.body = 0U, .pieces = {BreakPiece{.body = 1U}}})
        .update(Break{.body = 1U, .pieces = {BreakPiece{.body = 2U}}});
    AJFX_CHECK(twice_over.build().ok());
}

// ------------------------------------------------------- bodies as colliders

// One crate standing still, with snow falling on it. The crate is a box that
// nothing moves (its layer has no gravity and nothing hits it), so where the
// snow may not be is known exactly. A pile of one stands its body half a
// lattice step up, and a box of size 1 reaches sqrt(3) across however it is
// turned, so the crate's middle is at 0.866 and its lid at 1.366.
Effect snow_on_crates(bool collide) {
    EffectBuilder builder{"snow"};
    builder.emitter("crates")
        .capacity(16U)
        .spawn(SpawnPile{.count = 1U,
                         .shape = PileShape::Box,
                         .center = Vec3{0.0F, 0.0F, 0.0F},
                         .width = 1.0F,
                         .depth = 1.0F,
                         .spacing = 1.0F,
                         .jitter = 0.0F,
                         .release = 0.0F,
                         .body = 0U})
        .init(Lifetime{.min = 30.0F, .max = 30.0F})
        .init(InitSize{.min = 1.0F, .max = 1.0F})
        .update(RigidPhysics{.shape = RigidShape::Box, .friction = 0.9F, .restitution = 0.0F});
    EmitterBuilder& flakes = builder.emitter("snow");
    flakes.capacity(1024U)
        .seed(11U)
        .spawn(SpawnRate{.rate = 90.0F})
        .init(Lifetime{.min = 8.0F, .max = 8.0F})
        .init(ShapeLocation{.shape = Shape::Disc, .center = Vec3{0.0F, 4.0F, 0.0F}, .radius_min = 0.0F,
                            .radius_max = 1.2F})
        .init(Velocity{.direction = Vec3{0.0F, -1.0F, 0.0F}, .speed_min = 0.8F, .speed_max = 1.2F})
        .init(InitSize{.min = 0.05F, .max = 0.05F})
        .update(Gravity{.acceleration = Vec3{0.0F, -2.0F, 0.0F}});
    if (collide) {
        // Friction 1 holds a flake where it lands, as the Snowfall recipe's
        // ground does: snow settles rather than skating off.
        flakes.update(BodyCollision{.emitter = "crates", .restitution = 0.0F, .friction = 1.0F});
    }
    flakes.update(PlaneCollision{.restitution = 0.0F, .friction = 1.0F});
    Result<Effect> built = builder.build();
    if (!built) {
        std::cout << "  " << built.error().message << "\n";
    }
    AJFX_CHECK(built.ok());
    return std::move(built).value();
}

void snow_lands_on_the_bodies_of_another_emitter() {
    // Four crates a unit across, stacked two by two on the floor, so their
    // lids are a unit up. Nothing moves them.
    Simulation without = make(snow_on_crates(false));
    Simulation with = make(snow_on_crates(true));
    without.advance_to(600U);
    with.advance_to(600U);
    const EmitterState& fell = without.emitter(1U);
    const EmitterState& landed = with.emitter(1U);
    AJFX_CHECK(fell.alive() == landed.alive() && landed.alive() > 100U);
    // Over the crate itself, well inside its lid, flakes lie on the lid
    // rather than on the floor beneath it.
    const auto over_the_crate = [](Vec3 p) { return std::fabs(p.x) < 0.35F && std::fabs(p.z) < 0.35F; };
    const auto count = [&](const EmitterState& s, float low, float high) {
        std::size_t n = 0U;
        for (std::size_t i = 0U; i < s.alive(); ++i) {
            if (over_the_crate(s.position[i]) && s.position[i].y > low && s.position[i].y < high) {
                ++n;
            }
        }
        return n;
    };
    const std::size_t on_the_lid = count(landed, 1.3F, 1.45F);
    const std::size_t under_it = count(landed, -1.0F, 0.05F);
    std::cout << "  " << on_the_lid << " flakes rest on the crate's lid and " << under_it
              << " reached the floor under it\n";
    AJFX_CHECK(on_the_lid > 10U);
    AJFX_CHECK(under_it == 0U);
    // Without the collider they fall straight through to that floor.
    const std::size_t through = count(fell, -1.0F, 0.05F);
    std::cout << "  without it, " << through << " lie on the floor under the crate\n";
    AJFX_CHECK(through > 10U);
    // Flakes are in the lid's own height band either way, since without the
    // collider they are falling through it, so the floor beneath the crate
    // is what tells the two apart.
}

void bodies_as_colliders_rerun_and_seek_exactly() {
    const Effect effect = snow_on_crates(true);
    const std::unique_ptr<RigidSolver> four = rigid::make_jolt_solver(rigid::JoltOptions{.threads = 4U});
    Simulation a = make(effect);
    Simulation b = make(effect);
    Simulation c = make(effect, four.get());
    for (const std::uint64_t tick : {1U, 90U, 300U, 600U}) {
        a.advance_to(tick);
        b.advance_to(tick);
        c.advance_to(tick);
        AJFX_CHECK(a.state() == b.state());
        AJFX_CHECK(a.state() == c.state());
    }
    CheckpointRing ring{60U, 8U};
    for (const std::uint64_t target : {200U, 95U, 400U, 61U, 600U}) {
        Simulation seeker = make(effect);
        ring.seek(seeker, target);
        Simulation straight = make(effect);
        straight.advance_to(target);
        AJFX_CHECK(seeker.tick() == target);
        AJFX_CHECK(seeker.state() == straight.state());
    }
}

void impossible_body_colliders_are_refused() {
    const auto refused = [](const Result<Effect>& built, const std::string& fragment) {
        if (built.ok() || built.error().message.find(fragment) == std::string::npos) {
            std::cout << "  expected '" << fragment << "', got: "
                      << (built.ok() ? std::string{"accepted"} : built.error().message) << '\n';
            return false;
        }
        return true;
    };
    // An emitter this effect has none of.
    EffectBuilder absent{"x"};
    absent.emitter("snow").init(Lifetime{}).update(BodyCollision{.emitter = "crates"});
    AJFX_CHECK(refused(absent.build(), "which this effect has none of"));

    // One that holds no bodies.
    EffectBuilder no_bodies{"x"};
    no_bodies.emitter("crates").init(Lifetime{});
    no_bodies.emitter("snow").init(Lifetime{}).update(BodyCollision{.emitter = "crates"});
    AJFX_CHECK(refused(no_bodies.build(), "holds no bodies to hit"));

    // Its own.
    EffectBuilder itself{"x"};
    itself.emitter("crates").init(Lifetime{}).update(RigidPhysics{}).update(BodyCollision{.emitter = "crates"});
    AJFX_CHECK(refused(itself.build(), "names its own emitter"));

    // One that comes after, whose bodies would be last step's.
    EffectBuilder later{"x"};
    later.emitter("snow").init(Lifetime{}).update(BodyCollision{.emitter = "crates"});
    later.emitter("crates").init(Lifetime{}).update(RigidPhysics{});
    AJFX_CHECK(refused(later.build(), "comes after this one"));

    // And one holding more bodies than every particle may be tested against.
    EffectBuilder crowded{"x"};
    crowded.emitter("crates").capacity(max_body_colliders + 1U).init(Lifetime{}).update(RigidPhysics{});
    crowded.emitter("snow").init(Lifetime{}).update(BodyCollision{.emitter = "crates"});
    AJFX_CHECK(refused(crowded.build(), "may be collided with"));
}

// ---------------------------------------------------------------- glue

// A wall of small bricks held to each other, with a heavy ball dropped on
// its middle. Bricks 0.2 across reach 0.35 corner to corner, so a spacing of
// 0.6 sets them 0.21 apart, just touching: a footprint 3 long and 0.4 deep
// holds fourteen in a row, one deep, and 42 of them stand three courses
// high. The ball lands on the middle of the top course.
Effect glued_wall(bool glued) {
    EffectBuilder builder{"wall"};
    RigidProp ball{};
    ball.shape = PropShape::Sphere;
    ball.motion = PropMotion::Loose;
    ball.position = Vec3{0.0F, 3.0F, 0.0F};
    ball.radius = 0.3F;
    ball.size = 1.0F;
    ball.density = 4000.0F;
    ball.friction = 0.4F;
    ball.restitution = 0.3F;

    EmitterBuilder& bricks = builder.emitter("bricks");
    bricks.capacity(64U)
        .spawn(SpawnPile{.count = 42U,
                         .shape = PileShape::Box,
                         .width = 3.0F,
                         .depth = 0.4F,
                         .spacing = 0.6F,
                         .jitter = 0.0F,
                         .release = 0.0F})
        .init(Lifetime{.min = 30.0F, .max = 30.0F})
        .init(InitSize{.min = 0.2F, .max = 0.2F})
        .update(Gravity{})
        .update(RigidPhysics{.shape = RigidShape::Box, .friction = 0.8F, .restitution = 0.0F, .props = {ball}})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 0.8F});
    if (glued) {
        bricks.update(Glue{.range = 0.02F, .strength = 10.0F});
    }
    Result<Effect> built = builder.build();
    if (!built) {
        std::cout << "  " << built.error().message << "\n";
    }
    AJFX_CHECK(built.ok());
    return std::move(built).value();
}

void a_glued_wall_breaks_where_it_is_hit() {
    Simulation simulation = make(glued_wall(true));
    const EmitterState& s = simulation.emitter(0U);
    AJFX_CHECK(s.alive() == 42U);
    // Every brick is joined to the ones it touches, and the wall stands.
    AJFX_CHECK(!s.joins.empty());
    const std::size_t joined = s.joins.size();
    std::vector<Vec3> stood = s.position;
    simulation.advance_to(18U);
    float settled = 0.0F;
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        const Vec3 d = s.position[i] - stood[i];
        settled = std::max(settled, std::sqrt(dot(d, d)));
    }
    std::cout << "  the standing wall settled by " << settled << '\n';
    AJFX_CHECK(settled < 0.05F);

    // The ball lands on the middle. What it hits goes; the ends stand.
    simulation.advance_to(240U);
    std::size_t middle_moved = 0U;
    std::size_t middle = 0U;
    std::size_t ends_still = 0U;
    std::size_t ends = 0U;
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        const Vec3 d = s.position[i] - stood[i];
        const float moved = std::sqrt(dot(d, d));
        if (std::fabs(stood[i].x) < 0.4F) {
            ++middle;
            middle_moved += moved > 0.1F ? 1U : 0U;
        } else if (std::fabs(stood[i].x) > 1.0F) {
            ++ends;
            ends_still += moved < 0.05F ? 1U : 0U;
        }
    }
    std::cout << "  " << joined << " joins held " << s.alive() << " bricks; " << middle_moved << " of " << middle
              << " in the middle went, " << ends_still << " of " << ends << " at the ends stand\n";
    std::cout << "  " << s.joins.size() << " joins left\n";
    // What the ball reached went, and what it did not reach stands: the
    // bricks under the blow are counted too, and those are pressed down
    // rather than thrown, so a quarter of the middle moving is the wall
    // opening where it was hit.
    AJFX_CHECK(middle > 0U && ends > 0U);
    AJFX_CHECK(middle_moved >= middle / 4U && middle_moved > 0U);
    AJFX_CHECK(ends_still == ends);
    // Joins gave way where it was hit, and not everywhere.
    AJFX_CHECK(s.joins.size() < joined && !s.joins.empty());

    // Unglued, the same blow takes the whole wall down.
    Simulation loose = make(glued_wall(false));
    const EmitterState& l = loose.emitter(0U);
    const std::vector<Vec3> loose_stood = l.position;
    loose.advance_to(240U);
    std::size_t loose_ends_still = 0U;
    std::size_t loose_ends = 0U;
    for (std::size_t i = 0U; i < l.alive(); ++i) {
        if (std::fabs(loose_stood[i].x) > 1.0F) {
            ++loose_ends;
            const Vec3 d = l.position[i] - loose_stood[i];
            loose_ends_still += std::sqrt(dot(d, d)) < 0.05F ? 1U : 0U;
        }
    }
    std::cout << "  unglued, " << loose_ends_still << " of " << loose_ends << " at the ends stand\n";
    AJFX_CHECK(l.joins.empty());
    AJFX_CHECK(loose_ends_still < ends_still);
}

void glued_walls_rerun_and_seek_exactly() {
    const Effect effect = glued_wall(true);
    const std::unique_ptr<RigidSolver> four = rigid::make_jolt_solver(rigid::JoltOptions{.threads = 4U});
    Simulation a = make(effect);
    Simulation b = make(effect);
    Simulation c = make(effect, four.get());
    for (const std::uint64_t tick : {1U, 60U, 150U, 300U}) {
        a.advance_to(tick);
        b.advance_to(tick);
        c.advance_to(tick);
        AJFX_CHECK(a.state() == b.state());
        AJFX_CHECK(a.state() == c.state());
    }
    // A seek over the blow puts the joins back as they were, so the wall
    // carries on breaking the same way.
    CheckpointRing ring{30U, 8U};
    for (const std::uint64_t target : {120U, 45U, 300U, 61U, 240U}) {
        Simulation seeker = make(effect);
        ring.seek(seeker, target);
        Simulation straight = make(effect);
        straight.advance_to(target);
        AJFX_CHECK(seeker.tick() == target);
        if (!(seeker.state() == straight.state())) {
            const EmitterState& sought = seeker.state().emitters[0];
            const EmitterState& stepped = straight.state().emitters[0];
            std::cout << "  step " << target << " differs:";
            if (sought.alive() != stepped.alive()) {
                std::cout << " alive " << sought.alive() << " vs " << stepped.alive();
            }
            if (sought.joins != stepped.joins) {
                std::cout << " joins " << sought.joins.size() << " vs " << stepped.joins.size();
                const std::size_t both = std::min(sought.joins.size(), stepped.joins.size());
                for (std::size_t i = 0U; i < both; ++i) {
                    if (!(sought.joins[i] == stepped.joins[i])) {
                        std::cout << " first at " << i;
                        break;
                    }
                }
            }
            if (sought.position != stepped.position) {
                float worst = 0.0F;
                for (std::size_t i = 0U; i < std::min(sought.alive(), stepped.alive()); ++i) {
                    const Vec3 d = sought.position[i] - stepped.position[i];
                    worst = std::max(worst, std::sqrt(dot(d, d)));
                }
                std::cout << " position by " << worst;
            }
            if (sought.solver_state != stepped.solver_state) {
                std::cout << " solver state " << sought.solver_state.size() << " vs " << stepped.solver_state.size()
                          << " bytes";
            }
            std::cout << '\n';
        }
        AJFX_CHECK(seeker.state() == straight.state());
    }
}

void impossible_glue_is_refused() {
    const auto refused = [](const Result<Effect>& built, const std::string& fragment) {
        if (built.ok() || built.error().message.find(fragment) == std::string::npos) {
            std::cout << "  expected '" << fragment << "', got: "
                      << (built.ok() ? std::string{"accepted"} : built.error().message) << '\n';
            return false;
        }
        return true;
    };
    EffectBuilder alone{"x"};
    alone.emitter("e").init(Lifetime{}).update(Glue{});
    AJFX_CHECK(refused(alone.build(), "no 'rigid_physics' module"));

    EffectBuilder weak{"x"};
    weak.emitter("e").init(Lifetime{}).update(RigidPhysics{}).update(Glue{.strength = 0.0F});
    AJFX_CHECK(refused(weak.build(), "strength"));

    EffectBuilder twice{"x"};
    twice.emitter("e").init(Lifetime{}).update(RigidPhysics{}).update(Glue{}).update(Glue{});
    AJFX_CHECK(refused(twice.build(), "more than one 'glue' module"));
}

// ------------------------------------------------- a body cut into cells

// One cube body standing on the floor, which comes apart into the cells the
// cube model is cut into when something hits it hard enough.
Effect cut_crate(std::uint32_t cells, MeshSet& meshes) {
    const MeshData model = cube_mesh();
    Result<FracturedPieces> cut = cells_as_pieces(model, "crate", FractureOptions{.count = cells, .seed = 7U});
    if (!cut) {
        std::cout << "  " << cut.error().message << '\n';
    }
    AJFX_CHECK(cut.ok());
    meshes = cut.value().meshes;
    meshes.emplace("crate", model);

    // A ball dropped on it, as a prop, so the only body that can break is
    // the crate: a second crate would be of the same kind and break too.
    RigidProp ball{};
    ball.shape = PropShape::Sphere;
    ball.motion = PropMotion::Loose;
    ball.position = Vec3{0.0F, 3.0F, 0.0F};
    ball.radius = 0.3F;
    ball.size = 1.0F;
    ball.density = 3000.0F;
    ball.restitution = 0.2F;

    EffectBuilder builder{"cut"};
    builder.emitter("crates")
        .capacity(128U)
        .spawn(SpawnPile{.count = 1U, .shape = PileShape::Box, .jitter = 0.0F, .release = 0.0F})
        .init(Lifetime{.min = 30.0F, .max = 30.0F})
        .init(InitSize{.min = 1.0F, .max = 1.0F})
        .update(Gravity{})
        .update(RigidPhysics{.shape = RigidShape::Box,
                             .friction = 0.7F,
                             .restitution = 0.0F,
                             .bodies = std::move(cut).value().bodies,
                             .props = {ball}})
        .update(Break{.body = 0U, .threshold = 3.0F, .scatter = 0.2F, .spin = 60.0F, .pieces = cut.value().pieces})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 0.8F})
        .renderer(SpriteRenderer{.geometry = Geometry::Mesh, .mesh = "crate"});
    Result<Effect> built = builder.build();
    if (!built) {
        std::cout << "  " << built.error().message << '\n';
    }
    AJFX_CHECK(built.ok());
    return std::move(built).value();
}

void a_cut_body_comes_apart_into_its_cells() {
    constexpr std::uint32_t cells = 8U;
    MeshSet meshes;
    const Effect effect = cut_crate(cells, meshes);
    Result<Simulation> made =
        Simulation::create(effect, 3U, SimulationOptions{.meshes = meshes, .rigid = g_solver.get()});
    if (!made) {
        std::cout << "  " << made.error().message << '\n';
    }
    AJFX_CHECK(made.ok());
    Simulation simulation = std::move(made).value();
    const EmitterState& s = simulation.emitter(0U);
    // The crate stands, with the ball on its way down onto it.
    simulation.advance_to(12U);
    AJFX_CHECK(s.alive() == 1U);
    const Vec3 stood = s.position[0];

    std::uint64_t broke_at = 0U;
    for (std::uint64_t tick = 13U; tick <= 300U && broke_at == 0U; ++tick) {
        simulation.advance_to(tick);
        if (s.alive() > 1U) {
            broke_at = tick;
        }
    }
    AJFX_CHECK(broke_at > 0U);
    // The cells are all of it and nothing but it: eight pieces where one
    // crate was, each starting inside the crate it was cut from. The crate
    // is a unit across at size 1, so a cell's middle is within half a unit.
    std::size_t within = 0U;
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        AJFX_CHECK(s.body_kind[i] >= 1U && s.body_kind[i] <= cells);
        const Vec3 d = s.position[i] - stood;
        if (std::sqrt(dot(d, d)) < 0.87F) {
            ++within;
        }
    }
    std::cout << "  the cut crate came apart at step " << broke_at << " into " << s.alive() << " cells, "
              << within << " of them starting inside where it stood\n";
    AJFX_CHECK(s.alive() == cells);
    AJFX_CHECK(within == cells);

    // Every cell is a kind of its own, drawn and collided as that cell.
    const RigidPhysics* physics = rigid_physics(effect.emitters[0]);
    AJFX_CHECK(physics != nullptr && physics->bodies.size() == cells);
    for (std::size_t i = 0U; i < cells; ++i) {
        AJFX_CHECK(physics->bodies[i].shape == RigidShape::Hull);
        AJFX_CHECK(physics->bodies[i].mesh == "crate.cell." + std::to_string(i));
        AJFX_CHECK(physics->bodies[i].draw == physics->bodies[i].mesh);
    }
    AJFX_CHECK(check_meshes(effect, meshes).ok());

    // And they settle rather than exploding: the cut puts them where they
    // were, so they are not born inside one another.
    simulation.advance_to(600U);
    float highest = 0.0F;
    std::size_t still = 0U;
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        highest = std::max(highest, s.position[i].y);
        still += length(s.velocity[i]) < 0.05F ? 1U : 0U;
    }
    std::cout << "  they rest no higher than " << highest << ", " << still << " of " << s.alive() << " still\n";
    AJFX_CHECK(highest < 1.6F);
    AJFX_CHECK(still >= s.alive() - 2U);
}

void cutting_more_than_a_body_may_hold_is_refused() {
    const MeshData model = cube_mesh();
    const Result<FracturedPieces> many =
        cells_as_pieces(model, "crate", FractureOptions{.count = max_break_pieces + 1U});
    AJFX_CHECK(!many.ok() && many.error().code == ErrorCode::BudgetExceeded);
    std::cout << "  " << many.error().message << '\n';
    // And the kinds a cut needs have to fit beside the ones already there.
    const Result<FracturedPieces> crowded = cells_as_pieces(
        model, "crate", FractureOptions{.count = 8U}, static_cast<std::uint32_t>(max_rigid_body_kinds) - 2U);
    AJFX_CHECK(!crowded.ok() && crowded.error().code == ErrorCode::BudgetExceeded);
    std::cout << "  " << crowded.error().message << '\n';
}

int main() {
    g_solver = rigid::make_jolt_solver();
    const ajfx::test::Case cases[] = {
        {"piles_rest_on_the_floor_and_each_other", piles_rest_on_the_floor_and_each_other},
        {"bodies_fall_when_what_held_them_up_goes", bodies_fall_when_what_held_them_up_goes},
        {"reruns_and_thread_counts_agree", reruns_and_thread_counts_agree},
        {"seeking_lands_on_the_stepped_state", seeking_lands_on_the_stepped_state},
        {"births_deaths_and_colour_follow_the_reference", births_deaths_and_colour_follow_the_reference},
        {"hulls_wrap_the_renderers_mesh", hulls_wrap_the_renderers_mesh},
        {"unsupported_combinations_are_refused", unsupported_combinations_are_refused},
        {"piles_stand_from_the_start_until_released", piles_stand_from_the_start_until_released},
        {"piles_seek_and_follow_their_parameters", piles_seek_and_follow_their_parameters},
        {"piles_need_rigid_physics_and_room", piles_need_rigid_physics_and_room},
        {"runner_matches_the_simulation", runner_matches_the_simulation},
        {"runner_keeps_serving_beside_a_failed_track", runner_keeps_serving_beside_a_failed_track},
        {"balls_rest_inside_a_hollow_prop", balls_rest_inside_a_hollow_prop},
        {"a_driven_prop_tips_what_it_holds", a_driven_prop_tips_what_it_holds},
        {"loose_props_fall_and_settle", loose_props_fall_and_settle},
        {"props_rerun_and_seek_exactly", props_rerun_and_seek_exactly},
        {"impossible_props_are_refused", impossible_props_are_refused},
        {"contacts_report_the_hit_and_not_the_rest", contacts_report_the_hit_and_not_the_rest},
        {"contacts_are_the_same_on_any_thread_count", contacts_are_the_same_on_any_thread_count},
        {"contacts_survive_a_seek", contacts_survive_a_seek},
        {"contacts_over_the_limit_are_counted", contacts_over_the_limit_are_counted},
        {"contacts_name_the_prop_that_was_hit", contacts_name_the_prop_that_was_hit},
        {"body_kinds_mix_shapes_and_materials", body_kinds_mix_shapes_and_materials},
        {"body_kinds_rerun_and_seek_exactly", body_kinds_rerun_and_seek_exactly},
        {"impossible_body_kinds_are_refused", impossible_body_kinds_are_refused},
        {"a_kinds_own_size_decides_the_body_it_is", a_kinds_own_size_decides_the_body_it_is},
        {"kind_scales_rerun_and_seek_exactly", kind_scales_rerun_and_seek_exactly},
        {"impossible_kind_scales_are_refused", impossible_kind_scales_are_refused},
        {"a_hit_crate_comes_apart_into_its_pieces", a_hit_crate_comes_apart_into_its_pieces},
        {"breaking_reruns_and_seeks_exactly", breaking_reruns_and_seeks_exactly},
        {"a_break_with_no_room_is_refused_whole", a_break_with_no_room_is_refused_whole},
        {"impossible_breaks_are_refused", impossible_breaks_are_refused},
        {"snow_lands_on_the_bodies_of_another_emitter", snow_lands_on_the_bodies_of_another_emitter},
        {"bodies_as_colliders_rerun_and_seek_exactly", bodies_as_colliders_rerun_and_seek_exactly},
        {"impossible_body_colliders_are_refused", impossible_body_colliders_are_refused},
        {"a_glued_wall_breaks_where_it_is_hit", a_glued_wall_breaks_where_it_is_hit},
        {"glued_walls_rerun_and_seek_exactly", glued_walls_rerun_and_seek_exactly},
        {"impossible_glue_is_refused", impossible_glue_is_refused},
        {"a_cut_body_comes_apart_into_its_cells", a_cut_body_comes_apart_into_its_cells},
        {"cutting_more_than_a_body_may_hold_is_refused", cutting_more_than_a_body_may_hold_is_refused},
    };
    const int result = ajfx::test::run(cases);
    g_solver.reset();
    return result;
}
