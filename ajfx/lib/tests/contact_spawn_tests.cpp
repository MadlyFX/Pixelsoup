#include "check.hpp"

#include <ajfx/builder.hpp>
#include <ajfx/cache.hpp>
#include <ajfx/package.hpp>
#include <ajfx/rigid.hpp>
#include <ajfx/rigid_runner.hpp>
#include <ajfx/simulation.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Particles born of what rigid bodies do -- dust and sparks where they hit,
// smoke off them as they move -- and what lets one emitter hold several
// kinds of thing: init modules and appearance curves scoped to a spawn
// module or a body kind, and a heat that cools. Expectations are worked out
// here from the contacts and bodies the simulation reports, not from the
// code that bears the particles.
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

Simulation make(const Effect& effect, const RigidSolver* solver = g_solver.get(),
                std::shared_ptr<const Cache> cache = nullptr) {
    SimulationOptions options;
    options.rigid = solver;
    options.cache = std::move(cache);
    return must(Simulation::create(effect, 3U, std::move(options)));
}

float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

// A block half a unit across dropped from `height` onto the floor, and the
// dust its landing raises: six particles for every unit a second it lands
// at, thrown up and out a little. `rate` is loose dust that has nothing to
// do with the block, from a spawn module listed first.
Effect dust_from_a_drop(float height, bool contacts = true, float rate = 0.0F) {
    EffectBuilder builder{"drop"};
    builder.emitter("block")
        .capacity(2U)
        .spawn(SpawnBurst{.time = 0.0F, .count = 1U})
        .init(Lifetime{.min = 30.0F, .max = 30.0F})
        .init(ShapeLocation{.shape = Shape::Point, .center = Vec3{0.0F, height, 0.0F}})
        .init(InitSize{.min = 0.5F, .max = 0.5F})
        .update(Gravity{})
        .update(RigidPhysics{.shape = RigidShape::Box, .friction = 0.8F, .restitution = 0.0F})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 0.8F});
    EmitterBuilder& dust = builder.emitter("dust");
    // The loose dust is always the first spawn module, so the landing's is
    // the second whether it is there or not.
    dust.capacity(8192U).seed(5U).spawn(SpawnRate{.rate = rate});
    dust.init(Lifetime{.min = 1.0F, .max = 1.5F})
        .init(ShapeLocation{.shape = Shape::Sphere, .center = Vec3{0.0F, 1.0F, 0.0F}, .radius_min = 0.5F,
                            .radius_max = 0.5F, .only = Scope{.sources = {0U}}})
        .init(Velocity{.speed_min = 0.5F, .speed_max = 1.0F, .spread = 180.0F, .only = Scope{.sources = {0U}}})
        .init(InitSize{.min = 0.02F, .max = 0.04F})
        .init(InitColor{.a = Color{0.6F, 0.5F, 0.4F, 1.0F}, .b = Color{0.7F, 0.6F, 0.5F, 1.0F}})
        .update(Gravity{.acceleration = Vec3{0.0F, -2.0F, 0.0F}})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 0.5F});
    // Added last, so it takes the last module id and every other module
    // keeps its id, and so its random draws, with it or without it.
    if (contacts) {
        dust.spawn(SpawnFromContacts{.emitter = "block",
                                     .min_speed = 0.5F,
                                     .per_speed = 6.0F,
                                     .max_per_step = 64U,
                                     .along_normal = 0.3F,
                                     .along_surface = 0.2F,
                                     .spread = 30.0F});
    }
    return must(builder.build());
}

// One crate standing on the floor, and a ball dropped on its lid half a
// second in. Sparks fly only where the ball hits (a scope on the ball's
// kind), and settle on the crate, the ball or the floor.
Effect sparks_on_a_crate() {
    EffectBuilder builder{"sparks"};
    builder.emitter("pieces")
        .capacity(8U)
        .spawn(SpawnPile{.count = 1U, .shape = PileShape::Box, .jitter = 0.0F, .release = 0.0F})
        .spawn(SpawnBurst{.time = 0.5F, .count = 1U, .body = 1U})
        .init(Lifetime{.min = 30.0F, .max = 30.0F})
        .init(ShapeLocation{.shape = Shape::Point, .center = Vec3{0.15F, 3.0F, 0.1F}, .only = Scope{.sources = {1U}}})
        .init(InitSize{.min = 1.0F, .max = 1.0F, .only = Scope{.kinds = {0U}}})
        .init(InitSize{.min = 0.3F, .max = 0.3F, .only = Scope{.kinds = {1U}}})
        .update(Gravity{})
        .update(RigidPhysics{.shape = RigidShape::Box,
                             .friction = 0.9F,
                             .restitution = 0.0F,
                             .bodies = {RigidBodyKind{.shape = RigidShape::Sphere, .friction = 0.9F,
                                                      .restitution = 0.1F}}})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 0.9F});
    builder.emitter("sparks")
        .capacity(16384U)
        .seed(7U)
        .spawn(SpawnFromContacts{.emitter = "pieces",
                                 .min_speed = 1.0F,
                                 .per_speed = 6.0F,
                                 .max_per_step = 32U,
                                 .along_normal = 0.25F,
                                 .along_surface = 0.05F,
                                 .spread = 20.0F,
                                 .bodies = Scope{.kinds = {1U}}})
        .init(Lifetime{.min = 8.0F, .max = 8.0F})
        .init(InitSize{.min = 0.02F, .max = 0.02F})
        .init(InitColor{.a = Color{0.3F, 0.3F, 0.3F, 1.0F}, .b = Color{0.3F, 0.3F, 0.3F, 1.0F}})
        .update(Gravity{})
        .update(BodyCollision{.emitter = "pieces", .restitution = 0.0F, .friction = 1.0F})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 1.0F})
        .update(Heat{.start = 1.2F, .half_life = 0.4F, .glow = 3.0F});
    return must(builder.build());
}

// The crate and ball above, with smoke trailing whatever moves.
Effect everything() {
    Effect effect = sparks_on_a_crate();
    Emitter smoke;
    smoke.name = "smoke";
    smoke.seed = 13U;
    smoke.capacity = 4096U;
    smoke.spawn.push_back(SpawnFromBodies{.id = 1U,
                                          .emitter = "pieces",
                                          .min_speed = 0.3F,
                                          .per_speed = 20.0F,
                                          .max_per_step = 8U,
                                          .surface = 1.0F,
                                          .inherit = 0.2F});
    smoke.init.push_back(Lifetime{.id = 2U, .min = 2.0F, .max = 3.0F});
    smoke.init.push_back(InitSize{.id = 3U, .min = 0.05F, .max = 0.1F});
    smoke.update.push_back(Gravity{.id = 4U, .acceleration = Vec3{0.0F, 0.5F, 0.0F}});
    smoke.update.push_back(Drag{.id = 5U, .coefficient = 1.0F});
    smoke.update.push_back(ColorOverLife{.id = 6U});
    effect.emitters.push_back(std::move(smoke));
    AJFX_CHECK(validate(effect).ok());
    return effect;
}

// ------------------------------------------------------------ contacts

void dust_is_born_where_the_block_lands() {
    Simulation simulation = make(dust_from_a_drop(1.5F));
    bool seen = false;
    for (std::uint64_t tick = 1U; tick <= 120U && !seen; ++tick) {
        simulation.advance_to(tick);
        const EmitterState& dust = simulation.emitter(1U);
        if (dust.alive() == 0U) {
            continue;
        }
        seen = true;
        // Born this step, at age 0, at the point the block met the floor:
        // under its half-unit footprint, on the floor.
        std::size_t under = 0U;
        for (std::size_t i = 0U; i < dust.alive(); ++i) {
            AJFX_CHECK(dust.age[i] == 0.0F);
            const Vec3 p = dust.position[i];
            if (std::fabs(p.x) <= 0.3F && std::fabs(p.z) <= 0.3F && std::fabs(p.y) < 0.05F) {
                ++under;
            }
            // Thrown up and away from the floor, never into it.
            AJFX_CHECK(dust.velocity[i].y > 0.0F);
        }
        std::cout << "  step " << tick << ": " << dust.alive() << " specks of dust, " << under
                  << " of them where the block landed\n";
        AJFX_CHECK(under == dust.alive());
    }
    AJFX_CHECK(seen);
}

// Dust raised by a block dropped from `height` over two seconds, against
// what its landings ask for: six for every unit a second of each contact
// that began, at most 64 a step, each rounded up or down.
void count_dust(float height, std::uint64_t& born, double& expected, std::size_t& hits) {
    Simulation simulation = make(dust_from_a_drop(height));
    born = 0U;
    expected = 0.0;
    hits = 0U;
    for (std::uint64_t tick = 1U; tick <= 120U; ++tick) {
        const std::uint64_t before = simulation.emitter(1U).spawned;
        simulation.advance_to(tick);
        double wanted = 0.0;
        for (const RigidContact& contact : simulation.contacts(0U)) {
            if (contact.began && contact.speed >= 0.5F) {
                wanted += 6.0 * contact.speed;
                ++hits;
            }
        }
        expected += std::min(wanted, 64.0);
        born += simulation.emitter(1U).spawned - before;
    }
}

void dust_rises_with_the_impact_speed() {
    std::uint64_t low = 0U;
    std::uint64_t high = 0U;
    double low_expected = 0.0;
    double high_expected = 0.0;
    std::size_t low_hits = 0U;
    std::size_t high_hits = 0U;
    count_dust(0.8F, low, low_expected, low_hits);
    count_dust(3.0F, high, high_expected, high_hits);
    std::cout << "  dropped from 0.8: " << low << " specks (" << low_expected << " asked by " << low_hits
              << " hit(s)); from 3.0: " << high << " (" << high_expected << " by " << high_hits << ")\n";
    AJFX_CHECK(low > 0U);
    AJFX_CHECK(high > low + low / 2U);
    // Each hit rounds by less than one either way.
    AJFX_CHECK(std::fabs(static_cast<double>(low) - low_expected) <= static_cast<double>(low_hits) + 1.0);
    AJFX_CHECK(std::fabs(static_cast<double>(high) - high_expected) <= static_cast<double>(high_hits) + 1.0);
}

void sparks_come_to_rest_on_the_pieces() {
    Simulation simulation = make(sparks_on_a_crate());
    simulation.advance_to(300U);
    const EmitterState& sparks = simulation.emitter(1U);
    const EmitterState& pieces = simulation.emitter(0U);
    AJFX_CHECK(pieces.alive() == 2U);
    std::size_t on_pieces = 0U;
    std::size_t moving = 0U;
    for (std::size_t i = 0U; i < sparks.alive(); ++i) {
        const Vec3 p = sparks.position[i];
        if (length(sparks.velocity[i]) > 0.05F) {
            ++moving;
        }
        // Up on the crate's lid (a unit up) or on the ball resting there.
        if (p.y > 0.9F && std::fabs(p.x) < 0.6F && std::fabs(p.z) < 0.6F) {
            ++on_pieces;
        }
    }
    std::cout << "  " << sparks.alive() << " sparks, " << on_pieces << " resting on the crate or the ball, "
              << moving << " still moving\n";
    AJFX_CHECK(sparks.alive() > 10U);
    AJFX_CHECK(on_pieces > 5U);
    AJFX_CHECK(moving == 0U);
    // Only the ball's hits bear sparks: the crate settling onto the floor
    // in the first steps raised none.
    Simulation early = make(sparks_on_a_crate());
    early.advance_to(29U);
    AJFX_CHECK(early.emitter(1U).spawned == 0U);
}

// ------------------------------------------------------------ bodies

// A box slid along the floor at three units a second until friction stops
// it, beside a crate held still in the air; smoke trails whatever moves.
Effect smoke_off_a_sliding_box() {
    EffectBuilder builder{"slide"};
    builder.emitter("rocks")
        .capacity(8U)
        .spawn(SpawnBurst{.time = 0.0F, .count = 1U})
        .spawn(SpawnPile{.count = 1U, .shape = PileShape::Box, .center = Vec3{-3.0F, 0.0F, 0.0F}, .jitter = 0.0F,
                         .release = 100.0F})
        .init(Lifetime{.min = 30.0F, .max = 30.0F})
        .init(ShapeLocation{.shape = Shape::Point, .center = Vec3{0.0F, 0.25F, 0.0F}, .only = Scope{.sources = {0U}}})
        .init(Velocity{.direction = Vec3{1.0F, 0.0F, 0.0F}, .speed_min = 3.0F, .speed_max = 3.0F,
                       .only = Scope{.sources = {0U}}})
        .init(InitSize{.min = 0.5F, .max = 0.5F})
        .update(Gravity{})
        .update(RigidPhysics{.shape = RigidShape::Box, .friction = 0.8F, .restitution = 0.0F})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 0.8F});
    builder.emitter("smoke")
        .capacity(8192U)
        .seed(3U)
        .spawn(SpawnFromBodies{.emitter = "rocks", .min_speed = 0.3F, .per_speed = 60.0F, .max_per_step = 8U})
        .init(Lifetime{.min = 10.0F, .max = 10.0F})
        .update(Gravity{.acceleration = Vec3{0.0F, 0.2F, 0.0F}});
    return must(builder.build());
}

void bodies_leave_trails_only_while_moving() {
    Simulation simulation = make(smoke_off_a_sliding_box());
    std::uint64_t while_moving = 0U;
    std::uint64_t while_still = 0U;
    std::size_t still_steps = 0U;
    float reached = 0.0F;
    for (std::uint64_t tick = 1U; tick <= 180U; ++tick) {
        const std::uint64_t before = simulation.emitter(1U).spawned;
        simulation.advance_to(tick);
        const EmitterState& rocks = simulation.emitter(0U);
        float fastest = 0.0F;
        for (std::size_t i = 0U; i < rocks.alive(); ++i) {
            fastest = std::max(fastest, length(rocks.velocity[i]));
            reached = std::max(reached, rocks.position[i].x);
        }
        const std::uint64_t born = simulation.emitter(1U).spawned - before;
        if (fastest < 0.3F) {
            while_still += born;
            ++still_steps;
        } else {
            while_moving += born;
        }
    }
    std::cout << "  " << while_moving << " puffs while the box slid to x = " << reached << ", " << while_still
              << " in " << still_steps << " still steps\n";
    AJFX_CHECK(while_moving > 10U);
    AJFX_CHECK(while_still == 0U);
    AJFX_CHECK(still_steps > 60U);
    // Along the box's own path, and none off the crate held at x = -3.
    const EmitterState& smoke = simulation.emitter(1U);
    for (std::size_t i = 0U; i < smoke.alive(); ++i) {
        AJFX_CHECK(smoke.position[i].x > -0.6F && smoke.position[i].x < reached + 0.6F);
    }
}

// ------------------------------------------------------------ heat and scopes

// Heat's glow at an age, worked out here: start * 2^(-age / half_life),
// through black, red, orange, yellow and white, times the glow.
Color expected_glow(float start, float half_life, float glow, float age) {
    const double heat = static_cast<double>(start) * std::exp2(-static_cast<double>(age) / half_life);
    const double t = std::clamp(heat, 0.0, 1.0);
    const auto ramp = [&](double from) { return static_cast<float>(std::clamp(3.0 * t - from, 0.0, 1.0)) * glow; };
    return Color{ramp(0.0), ramp(1.0), ramp(2.0), 0.0F};
}

void heat_cools_monotonically() {
    EffectBuilder builder{"embers"};
    builder.emitter("embers")
        .capacity(16U)
        .spawn(SpawnBurst{.time = 0.0F, .count = 4U})
        .init(Lifetime{.min = 5.0F, .max = 5.0F})
        .init(InitColor{.a = Color{0.1F, 0.1F, 0.1F, 1.0F}, .b = Color{0.1F, 0.1F, 0.1F, 1.0F}})
        .update(Heat{.start = 1.5F, .half_life = 0.3F, .glow = 4.0F});
    Simulation simulation = make(must(builder.build()));
    float previous = 1.0e9F;
    std::size_t cooled = 0U;
    for (std::uint64_t tick = 1U; tick <= 180U; ++tick) {
        simulation.advance_to(tick);
        const EmitterState& embers = simulation.emitter(0U);
        AJFX_CHECK(embers.alive() == 4U);
        const Color c = embers.color[0];
        const Color want = expected_glow(1.5F, 0.3F, 4.0F, embers.age[0]);
        AJFX_CHECK(std::fabs(c.r - (0.1F + want.r)) < 1.0e-4F);
        AJFX_CHECK(std::fabs(c.g - (0.1F + want.g)) < 1.0e-4F);
        AJFX_CHECK(std::fabs(c.b - (0.1F + want.b)) < 1.0e-4F);
        AJFX_CHECK(c.a == 1.0F);
        const float total = c.r + c.g + c.b;
        AJFX_CHECK(total <= previous);
        if (total < previous) {
            ++cooled;
        }
        previous = total;
    }
    // White hot while the heat is over 1 (0.18 s), then cooling every step
    // until the glow is all but gone.
    const Color last = simulation.emitter(0U).color[0];
    std::cout << "  cooled in " << cooled << " of 180 steps, to " << last.r << " " << last.g << " " << last.b << '\n';
    AJFX_CHECK(cooled > 100U);
    AJFX_CHECK(last.r < 0.15F && last.g == 0.1F && last.b == 0.1F);
}

// Crates and hot fragments in one rigid physics emitter, each kind set up by
// its own colour and size, a colour over life every body takes, and a heat
// only the fragments have.
Effect crates_and_embers() {
    const Color brown{0.6F, 0.4F, 0.2F, 1.0F};
    const Color orange{1.0F, 0.5F, 0.1F, 1.0F};
    EffectBuilder builder{"rubble"};
    builder.emitter("rubble")
        .capacity(32U)
        .spawn(SpawnBurst{.time = 0.0F, .count = 4U})
        .spawn(SpawnBurst{.time = 0.0F, .count = 4U, .body = 1U})
        .init(Lifetime{.min = 10.0F, .max = 10.0F})
        .init(ShapeLocation{.shape = Shape::Box, .center = Vec3{0.0F, 2.0F, 0.0F}, .size = Vec3{4.0F, 2.0F, 4.0F}})
        .init(InitColor{.a = brown, .b = brown, .only = Scope{.kinds = {0U}}})
        .init(InitColor{.a = orange, .b = orange, .only = Scope{.kinds = {1U}}})
        .init(InitSize{.min = 0.4F, .max = 0.4F, .only = Scope{.kinds = {0U}}})
        .init(InitSize{.min = 0.1F, .max = 0.1F, .only = Scope{.kinds = {1U}}})
        .update(Gravity{})
        .update(RigidPhysics{.shape = RigidShape::Box, .bodies = {RigidBodyKind{.shape = RigidShape::Box}}})
        .update(PlaneCollision{})
        .update(ColorOverLife{.keys = {{0.0F, Color{0.5F, 0.5F, 0.5F, 1.0F}}, {1.0F, Color{0.5F, 0.5F, 0.5F, 1.0F}}}})
        .update(Heat{.start = 1.0F, .half_life = 0.5F, .glow = 3.0F, .only = Scope{.kinds = {1U}}});
    return must(builder.build());
}

void scoped_init_gives_two_kinds_their_own_look() {
    Simulation simulation = make(crates_and_embers());
    for (const std::uint64_t tick : {30U, 540U}) {
        simulation.advance_to(tick);
        const EmitterState& s = simulation.emitter(0U);
        AJFX_CHECK(s.alive() == 8U);
        for (std::size_t i = 0U; i < s.alive(); ++i) {
            const bool ember = s.body_kind[i] == 1U;
            // Each kind's own size and colour; the colour over life halves
            // both kinds' red, green and blue, since it is everyone's.
            AJFX_CHECK(s.size[i] == (ember ? 0.1F : 0.4F));
            const Color base = ember ? Color{1.0F, 0.5F, 0.1F, 1.0F} : Color{0.6F, 0.4F, 0.2F, 1.0F};
            const Color glow = ember ? expected_glow(1.0F, 0.5F, 3.0F, s.age[i]) : Color{0.0F, 0.0F, 0.0F, 0.0F};
            AJFX_CHECK(std::fabs(s.color[i].r - (base.r * 0.5F + glow.r)) < 1.0e-4F);
            AJFX_CHECK(std::fabs(s.color[i].g - (base.g * 0.5F + glow.g)) < 1.0e-4F);
            AJFX_CHECK(std::fabs(s.color[i].b - (base.b * 0.5F + glow.b)) < 1.0e-4F);
            AJFX_CHECK(s.color[i].a == 1.0F);
            if (!ember) {
                AJFX_CHECK(s.color[i] == (Color{0.3F, 0.2F, 0.1F, 1.0F}));
            }
        }
    }
    // A particle emitter's spawn modules, scoped by source: two colours, and
    // a size over life only the second takes.
    EffectBuilder builder{"two"};
    builder.emitter("two")
        .capacity(64U)
        .spawn(SpawnBurst{.time = 0.0F, .count = 8U})
        .spawn(SpawnBurst{.time = 0.0F, .count = 8U})
        .init(Lifetime{.min = 2.0F, .max = 2.0F})
        .init(InitSize{.min = 0.2F, .max = 0.2F})
        .init(InitColor{.a = Color{1.0F, 0.0F, 0.0F, 1.0F}, .b = Color{1.0F, 0.0F, 0.0F, 1.0F},
                        .only = Scope{.sources = {0U}}})
        .init(InitColor{.a = Color{0.0F, 0.0F, 1.0F, 1.0F}, .b = Color{0.0F, 0.0F, 1.0F, 1.0F},
                        .only = Scope{.sources = {1U}}})
        .update(SizeOverLife{.keys = {{0.0F, 0.5F}, {1.0F, 0.5F}}, .only = Scope{.sources = {1U}}});
    Simulation two = make(must(builder.build()));
    two.advance_to(10U);
    const EmitterState& s = two.emitter(0U);
    AJFX_CHECK(s.alive() == 16U);
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        const bool second = spawn_source(s.spawn_index[i]) == 1U;
        AJFX_CHECK(s.color[i] == (second ? Color{0.0F, 0.0F, 1.0F, 1.0F} : Color{1.0F, 0.0F, 0.0F, 1.0F}));
        AJFX_CHECK(s.size[i] == (second ? 0.1F : 0.2F));
    }
}

// ------------------------------------------------------------ determinism

bool same_particles(const EmitterState& a, const EmitterState& b) {
    return a.position == b.position && a.velocity == b.velocity && a.age == b.age && a.lifetime == b.lifetime &&
           a.color == b.color && a.size == b.size && a.spawn_index == b.spawn_index && a.spawned == b.spawned &&
           a.refused == b.refused;
}

void a_contact_spawn_leaves_the_rest_bit_identical() {
    // The same block and the same 40 specks a second of loose dust, with
    // and without dust raised where the block lands.
    Simulation without = make(dust_from_a_drop(2.0F, false, 40.0F));
    Simulation with = make(dust_from_a_drop(2.0F, true, 40.0F));
    std::size_t raised = 0U;
    for (const std::uint64_t tick : {20U, 60U, 120U}) {
        without.advance_to(tick);
        with.advance_to(tick);
        AJFX_CHECK(same_particles(without.emitter(0U), with.emitter(0U)));
        // The loose dust is the first spawn module's in both, particle for
        // particle and bit for bit.
        const EmitterState& a = without.emitter(1U);
        const EmitterState& b = with.emitter(1U);
        std::vector<std::size_t> loose;
        for (std::size_t i = 0U; i < b.alive(); ++i) {
            if (spawn_source(b.spawn_index[i]) == 0U) {
                loose.push_back(i);
            }
        }
        raised = b.alive() - loose.size();
        AJFX_CHECK(loose.size() == a.alive());
        for (std::size_t k = 0U; k < loose.size() && k < a.alive(); ++k) {
            const std::size_t i = loose[k];
            AJFX_CHECK(b.position[i] == a.position[k] && b.velocity[i] == a.velocity[k]);
            AJFX_CHECK(b.age[i] == a.age[k] && b.lifetime[i] == a.lifetime[k]);
            AJFX_CHECK(b.color[i] == a.color[k] && b.size[i] == a.size[k]);
            AJFX_CHECK(b.spawn_index[i] == a.spawn_index[k]);
        }
    }
    std::cout << "  " << raised << " specks raised by the landing beside the loose dust, which is unchanged\n";
}

void contact_spawns_rerun_and_seek_exactly() {
    const Effect effect = everything();
    const std::unique_ptr<RigidSolver> four = rigid::make_jolt_solver(rigid::JoltOptions{.threads = 4U});
    Simulation a = make(effect);
    Simulation b = make(effect);
    Simulation c = make(effect, four.get());
    for (const std::uint64_t tick : {1U, 40U, 90U, 240U}) {
        a.advance_to(tick);
        b.advance_to(tick);
        c.advance_to(tick);
        AJFX_CHECK(a.state() == b.state());
        AJFX_CHECK(a.state() == c.state());
    }
    std::cout << "  " << a.emitter(1U).spawned << " sparks and " << a.emitter(2U).spawned
              << " puffs of smoke, the same on 1 and 4 threads\n";
    AJFX_CHECK(a.emitter(1U).spawned > 0U && a.emitter(2U).spawned > 0U);
    CheckpointRing ring{30U, 8U};
    Simulation seeker = make(effect);
    for (const std::uint64_t target : {100U, 37U, 240U, 61U, 31U, 180U}) {
        ring.seek(seeker, target);
        Simulation straight = make(effect);
        straight.advance_to(target);
        AJFX_CHECK(seeker.tick() == target);
        AJFX_CHECK(seeker.state() == straight.state());
        // What the step into the target reported, a seek reports too, once
        // it has stepped into it.
        if (target % 30U != 0U) {
            const std::span<const RigidContact> x = seeker.contacts(0U);
            const std::span<const RigidContact> y = straight.contacts(0U);
            AJFX_CHECK(std::equal(x.begin(), x.end(), y.begin(), y.end()));
        }
    }
}

void runner_frames_carry_the_steps_contacts() {
    const Effect effect = everything();
    RigidRunner runner{*g_solver, RigidRunnerOptions{.lead = 10U, .checkpoint_interval = 10U, .checkpoint_count = 8U}};
    std::unique_ptr<RigidTrack> track = runner.track(effect, 3U, {});
    // The steps whose contacts the effect reads: the crate settling, then
    // the ball landing on it.
    std::vector<std::uint64_t> hits;
    {
        Simulation scan = make(effect);
        for (std::uint64_t tick = 1U; tick <= 120U; ++tick) {
            scan.advance_to(tick);
            if (!scan.contacts(0U).empty()) {
                hits.push_back(tick);
            }
        }
    }
    AJFX_CHECK(hits.size() >= 2U);
    const std::uint64_t landed = hits.back();
    std::cout << "  contacts at " << hits.size() << " steps, the last at " << landed << '\n';
    std::size_t carried = 0U;
    // Forwards past the last hit, back onto it and onto the first, back to a
    // checkpoint's tick, to the start, and on again.
    const std::vector<std::uint64_t> ticks{landed + 12U, landed, hits.front(), landed - landed % 10U,
                                           landed + 3U, std::uint64_t{0U}, landed};
    for (const std::uint64_t tick : ticks) {
        std::shared_ptr<const RigidFrame> frame = track->frame(tick);
        while (!frame) {
            AJFX_CHECK(!track->error().has_value());
            runner.wait_idle();
            frame = track->frame(tick);
        }
        Simulation straight = make(effect);
        straight.advance_to(tick);
        AJFX_CHECK(frame->tick == tick);
        AJFX_CHECK(same_particles(frame->emitters[0], straight.emitter(0U)));
        const std::span<const RigidContact> want = straight.contacts(0U);
        AJFX_CHECK(frame->contacts.size() == effect.emitters.size());
        AJFX_CHECK(std::equal(frame->contacts[0].begin(), frame->contacts[0].end(), want.begin(), want.end()));
        AJFX_CHECK(frame->contacts[1].empty() && frame->contacts[2].empty());
        carried += want.size();
    }
    std::cout << "  " << carried << " contacts carried by the runner's frames, as the simulation reported them\n";
    AJFX_CHECK(carried > 0U);
}

// ------------------------------------------------------------ packages and caches

void new_modules_round_trip_through_a_package() {
    const Effect effect = everything();
    const std::string json = effect_to_json(effect);
    AJFX_CHECK(json.find("\"spawn_from_contacts\"") != std::string::npos);
    AJFX_CHECK(json.find("\"spawn_from_bodies\"") != std::string::npos);
    AJFX_CHECK(json.find("\"heat\"") != std::string::npos);
    AJFX_CHECK(json.find("\"only\"") != std::string::npos);
    const Effect read = must(read_package(must(write_package(effect))));
    AJFX_CHECK(read == effect);
    // An empty scope is left unsaid.
    EffectBuilder plain{"plain"};
    plain.emitter("e").spawn(SpawnBurst{}).init(Lifetime{});
    AJFX_CHECK(effect_to_json(must(plain.build())).find("\"only\"") == std::string::npos);
}

void contact_spawns_cache_with_their_bodies() {
    const Effect effect = everything();
    // By default the bodies are cached, and so is what is born of them.
    const std::vector<CacheMode> modes = default_cache_modes(effect);
    AJFX_CHECK(modes[0] == CacheMode::Rigid && modes[1] == CacheMode::Particles && modes[2] == CacheMode::Particles);
    BakeSettings settings;
    settings.frames = 200U;
    settings.keyframe_interval = 30U;
    const Baked baked = must(bake(effect, 3U, SimulationOptions{.rigid = g_solver.get()}, settings));
    auto cache = std::make_shared<const Cache>(must(Cache::open(baked.bytes, effect, 3U)));
    Simulation played = make(effect, nullptr, cache);
    Simulation straight = make(effect);
    for (const std::uint64_t tick : {40U, 120U, 199U}) {
        played.advance_to(tick);
        straight.advance_to(tick);
        for (const std::size_t emitter : {1U, 2U}) {
            const EmitterState& p = played.emitter(emitter);
            const EmitterState& s = straight.emitter(emitter);
            AJFX_CHECK(p.alive() == s.alive());
            AJFX_CHECK(p.spawn_index == s.spawn_index && p.age == s.age);
            // The glow and colour are worked out again from the age, exactly.
            AJFX_CHECK(p.color == s.color && p.size == s.size);
        }
    }
    // A live emitter cannot bear particles from cached bodies.
    settings.modes = {CacheMode::Rigid, CacheMode::Live, CacheMode::Live};
    const Result<Baked> refused = bake(effect, 3U, SimulationOptions{.rigid = g_solver.get()}, settings);
    AJFX_CHECK(!refused.ok());
    std::cout << "  refused: " << refused.error().message << '\n';
}

// ------------------------------------------------------------ refusals

void impossible_contact_spawns_and_scopes_are_refused() {
    const auto refused = [](Effect effect, std::string_view says) {
        const Result<bool> valid = validate(effect);
        std::cout << "  " << (valid.ok() ? std::string{"accepted"} : valid.error().message) << '\n';
        AJFX_CHECK(!valid.ok());
        AJFX_CHECK(valid.error().message.find(says) != std::string::npos);
    };
    const Effect good = everything();
    AJFX_CHECK(validate(good).ok());
    {
        Effect e = good;
        std::get<SpawnFromContacts>(e.emitters[1].spawn[0]).emitter = "nowhere";
        refused(e, "which this effect has none of");
    }
    {
        Effect e = good;
        std::get<SpawnFromBodies>(e.emitters[2].spawn[0]).emitter = "sparks";
        refused(e, "no 'rigid_physics' module");
    }
    {
        Effect e = good;
        std::get<SpawnFromContacts>(e.emitters[1].spawn[0]).min_speed = 0.0F;
        refused(e, "min_speed");
    }
    {
        Effect e = good;
        std::get<SpawnFromContacts>(e.emitters[1].spawn[0]).bodies.kinds = {7U};
        refused(e, "names body kind 7");
    }
    {
        Effect e = good;
        e.emitters[0].spawn.push_back(SpawnFromContacts{.id = 90U, .emitter = "pieces"});
        refused(e, "bears particles from another emitter's bodies");
    }
    {
        Effect e = good;
        std::get<SpawnFromBodies>(e.emitters[2].spawn[0]).max_per_step = 100U;
        refused(e, "particles alive but its capacity is");
    }
    {
        Effect e = good;
        std::get<Lifetime>(e.emitters[0].init[0]).only = Scope{.sources = {0U}};
        refused(e, "no 'lifetime' module is in scope");
    }
    {
        Effect e = good;
        std::get<InitSize>(e.emitters[1].init[1]).only = Scope{.kinds = {0U}};
        refused(e, "has no rigid physics");
    }
    {
        Effect e = good;
        std::get<InitSize>(e.emitters[0].init[2]).only = Scope{.sources = {5U}};
        refused(e, "names spawn source 5");
    }
    {
        Effect e = good;
        std::get<Heat>(e.emitters[1].update[3]).half_life = 0.0F;
        refused(e, "half_life");
    }
}

} // namespace

int main() {
    g_solver = rigid::make_jolt_solver();
    const ajfx::test::Case cases[] = {
        {"dust_is_born_where_the_block_lands", dust_is_born_where_the_block_lands},
        {"dust_rises_with_the_impact_speed", dust_rises_with_the_impact_speed},
        {"sparks_come_to_rest_on_the_pieces", sparks_come_to_rest_on_the_pieces},
        {"bodies_leave_trails_only_while_moving", bodies_leave_trails_only_while_moving},
        {"heat_cools_monotonically", heat_cools_monotonically},
        {"scoped_init_gives_two_kinds_their_own_look", scoped_init_gives_two_kinds_their_own_look},
        {"a_contact_spawn_leaves_the_rest_bit_identical", a_contact_spawn_leaves_the_rest_bit_identical},
        {"contact_spawns_rerun_and_seek_exactly", contact_spawns_rerun_and_seek_exactly},
        {"runner_frames_carry_the_steps_contacts", runner_frames_carry_the_steps_contacts},
        {"new_modules_round_trip_through_a_package", new_modules_round_trip_through_a_package},
        {"contact_spawns_cache_with_their_bodies", contact_spawns_cache_with_their_bodies},
        {"impossible_contact_spawns_and_scopes_are_refused", impossible_contact_spawns_and_scopes_are_refused},
    };
    const int result = ajfx::test::run(cases);
    g_solver.reset();
    return result;
}
