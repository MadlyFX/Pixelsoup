#include "check.hpp"

#include <ajfx/builder.hpp>
#include <ajfx/presets.hpp>
#include <ajfx/rigid.hpp>
#include <ajfx/simulation.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

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

} // namespace

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
    };
    const int result = ajfx::test::run(cases);
    g_solver.reset();
    return result;
}
