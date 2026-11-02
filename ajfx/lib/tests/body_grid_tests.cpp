#include "check.hpp"

// The grid is internal to the runtime; its search is tested directly as
// well as through the simulations that use it.
#include "../src/body_grid.hpp"

#include <ajfx/builder.hpp>
#include <ajfx/random.hpp>
#include <ajfx/rigid.hpp>
#include <ajfx/simulation.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Particles meeting another emitter's bodies through a grid (BodyCollision)
// and stirred by them (Wake): the grid finds the same bodies, in the same
// order, as looking at every body in turn, so the two agree to the bit; it
// reruns and seeks exactly; a moving body drags what is near it along and a
// still one leaves it alone; and snow settles on a thousand crates at once.
namespace {

using namespace ajfx;
using Clock = std::chrono::steady_clock;

std::unique_ptr<RigidSolver> g_solver;

Simulation make(const Effect& effect, bool broadphase = true) {
    Result<Simulation> created =
        Simulation::create(effect, 9U, SimulationOptions{.rigid = g_solver.get(), .body_broadphase = broadphase});
    if (!created) {
        std::cout << created.error().message << '\n';
    }
    AJFX_CHECK(created.ok());
    return std::move(created).value();
}

// ---------------------------------------------------------------- the search

// Every body whose surface, grown by `extra`, comes within reach of a point
// is found by the grid, however the bodies and points are strewn: the grid
// may look at more, never at fewer.
void the_grid_never_misses_a_body_in_reach() {
    std::vector<BodyCollider> bodies;
    for (std::uint32_t i = 0U; i < 3000U; ++i) {
        const auto u = [i](std::uint32_t draw) { return particle_unit(7U, i, draw); };
        BodyCollider body;
        body.position = Vec3{(u(0U) - 0.5F) * 40.0F, u(1U) * 6.0F, (u(2U) - 0.5F) * 40.0F};
        body.half = 0.05F + u(3U) * 0.4F;
        body.box = u(4U) < 0.5F;
        bodies.push_back(body);
    }
    // One far away stretches the grid's cells to fit the budget.
    BodyCollider distant;
    distant.position = Vec3{5000.0F, 0.0F, 0.0F};
    bodies.push_back(distant);
    for (const bool stretched : {false, true}) {
        std::vector<BodyCollider> these = bodies;
        if (!stretched) {
            these.pop_back();
        }
        const detail::BodyGrid grid = detail::make_body_grid(these, true);
        AJFX_CHECK(grid.usable);
        AJFX_CHECK(static_cast<std::uint64_t>(grid.nx) * grid.ny * grid.nz <= detail::body_grid_cells);
        std::size_t looked = 0U;
        std::size_t touching = 0U;
        for (std::uint32_t n = 0U; n < 2000U; ++n) {
            const auto u = [n](std::uint32_t draw) { return particle_unit(8U, n, draw); };
            const Vec3 p{(u(0U) - 0.5F) * 44.0F, u(1U) * 7.0F - 0.5F, (u(2U) - 0.5F) * 44.0F};
            const float extra = u(3U) < 0.5F ? 0.0F : u(4U) * 0.3F;
            std::set<std::uint32_t> wanted;
            for (std::uint32_t b = 0U; b < these.size(); ++b) {
                const Vec3 d = p - these[b].position;
                const float r = detail::body_bound(these[b]) + extra;
                if (std::sqrt(dot(d, d)) < r) {
                    wanted.insert(b);
                }
            }
            std::set<std::uint32_t> found;
            detail::meet_bodies(
                grid, [p] { return p; }, extra,
                [&](const BodyCollider& body) { found.insert(static_cast<std::uint32_t>(&body - grid.bodies.data())); });
            for (const std::uint32_t b : wanted) {
                AJFX_CHECK(found.contains(b));
            }
            looked += found.size();
            touching += wanted.size();
        }
        std::cout << "  " << (stretched ? "stretched" : "compact") << " grid " << grid.nx << " x " << grid.ny << " x "
                  << grid.nz << ": looked at " << looked << " bodies for " << touching << " in reach\n";
    }
    // Off, every body is looked at, in body order.
    const detail::BodyGrid off = detail::make_body_grid(bodies, false);
    AJFX_CHECK(!off.usable);
    std::uint32_t next = 0U;
    detail::meet_bodies(
        off, [] { return Vec3{}; }, 0.0F, [&](const BodyCollider& body) {
            AJFX_CHECK(&body - off.bodies.data() == static_cast<std::ptrdiff_t>(next));
            ++next;
        });
    AJFX_CHECK(next == bodies.size());
}

// ---------------------------------------------------------------- crates

// Crates 0.5 across stand in rows (a pile's box lattice, spaced 1.5 times
// their reach: 1.3 apart, 40 by 30 of them, their lids at 0.9), held still.
// Snow falls on them from just above, and rigid chips land among them.
constexpr float crate_spacing = 1.5F;
constexpr float crate_size = 0.5F;
constexpr std::uint32_t crate_rows = 30U;
constexpr std::uint32_t crate_columns = 40U;

float crate_step() {
    return crate_size * std::sqrt(3.0F) * crate_spacing;
}

Effect snow_on_crates(std::uint32_t flakes, std::uint32_t chips) {
    const float d = crate_step();
    const float width = static_cast<float>(crate_columns) * d;
    const float depth = static_cast<float>(crate_rows) * d;
    EffectBuilder builder{"crates"};
    builder.emitter("crates")
        .capacity(crate_rows * crate_columns)
        .spawn(SpawnPile{.count = crate_rows * crate_columns,
                         .shape = PileShape::Box,
                         .width = width,
                         .depth = depth,
                         .spacing = crate_spacing,
                         .jitter = 0.0F,
                         .release = 100.0F})
        .init(Lifetime{.min = 1000.0F, .max = 1000.0F})
        .init(InitSize{.min = crate_size, .max = crate_size})
        .update(RigidPhysics{.shape = RigidShape::Box, .friction = 0.9F, .restitution = 0.0F});
    builder.emitter("snow")
        .capacity(flakes)
        .seed(3U)
        .spawn(SpawnBurst{.count = flakes})
        .init(Lifetime{.min = 100.0F, .max = 100.0F})
        .init(ShapeLocation{.shape = Shape::Box, .center = Vec3{0.0F, 1.3F, 0.0F}, .size = Vec3{width, 0.4F, depth}})
        .init(Velocity{.direction = Vec3{0.0F, -1.0F, 0.0F}, .speed_min = 2.0F, .speed_max = 2.5F, .spread = 10.0F})
        .init(InitSize{.min = 0.02F, .max = 0.02F})
        .update(Gravity{})
        .update(BodyCollision{.emitter = "crates", .restitution = 0.0F, .friction = 1.0F})
        .update(PlaneCollision{.restitution = 0.0F, .friction = 1.0F});
    builder.emitter("chips")
        .capacity(chips)
        .seed(5U)
        .spawn(SpawnBurst{.count = chips})
        .init(Lifetime{.min = 100.0F, .max = 100.0F})
        .init(ShapeLocation{.shape = Shape::Box, .center = Vec3{0.0F, 1.5F, 0.0F}, .size = Vec3{width, 0.4F, depth}})
        .init(Velocity{.speed_min = 1.0F, .speed_max = 3.0F, .spread = 60.0F})
        .init(InitSize{.min = 0.06F, .max = 0.2F})
        .init(InitTumble{})
        .update(Gravity{})
        .update(RigidBody{.contact_scale = 0.5F})
        .update(BodyCollision{.emitter = "crates", .restitution = 0.2F, .friction = 0.6F})
        .update(PlaneCollision{.restitution = 0.2F, .friction = 0.6F});
    return builder.build().value();
}

double run_ms(Simulation& simulation, std::uint64_t target) {
    const auto start = Clock::now();
    simulation.advance_to(target);
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void the_grid_meets_what_every_body_in_turn_meets() {
    const Effect effect = snow_on_crates(1500U, 200U);
    Simulation grid = make(effect, true);
    Simulation every = make(effect, false);
    AJFX_CHECK(grid.emitter(0U).alive() == crate_rows * crate_columns);
    double grid_ms = 0.0;
    double every_ms = 0.0;
    for (const std::uint64_t tick : {1U, 8U, 20U}) {
        grid_ms += run_ms(grid, tick);
        every_ms += run_ms(every, tick);
        for (std::size_t e = 1U; e < 3U; ++e) {
            AJFX_CHECK(grid.emitter(e) == every.emitter(e));
        }
    }
    std::cout << "  " << grid.emitter(0U).alive() << " crates, " << grid.emitter(1U).alive() << " flakes and "
              << grid.emitter(2U).alive() << " chips agree to the bit; 20 steps took " << grid_ms
              << " ms through the grid, " << every_ms << " ms meeting every crate in turn\n";
}

void snow_settles_on_a_thousand_crates() {
    const Effect effect = snow_on_crates(6000U, 300U);
    Simulation simulation = make(effect);
    simulation.advance_to(60U);
    const float d = crate_step();
    const float x0 = -0.5F * static_cast<float>(crate_columns - 1U) * d;
    const float z0 = -0.5F * static_cast<float>(crate_rows - 1U) * d;
    const float lid = 0.5F * d + 0.5F * crate_size;
    const EmitterState& snow = simulation.emitter(1U);
    std::set<std::pair<int, int>> covered;
    std::size_t resting = 0U;
    std::size_t inside = 0U;
    for (std::size_t i = 0U; i < snow.alive(); ++i) {
        const Vec3 p = snow.position[i];
        const int column = static_cast<int>(std::lround((p.x - x0) / d));
        const int row = static_cast<int>(std::lround((p.z - z0) / d));
        const float across = std::fabs(p.x - (x0 + static_cast<float>(column) * d));
        const float along = std::fabs(p.z - (z0 + static_cast<float>(row) * d));
        const bool over = column >= 0 && column < static_cast<int>(crate_columns) && row >= 0 &&
                          row < static_cast<int>(crate_rows) && across < 0.5F * crate_size - 0.01F &&
                          along < 0.5F * crate_size - 0.01F;
        if (over && std::fabs(p.y - lid) < 0.02F && dot(snow.velocity[i], snow.velocity[i]) < 0.01F) {
            ++resting;
            covered.insert({row, column});
        }
        // Within a crate's footprint, below its lid and above its floor:
        // fallen into it. (The pile stands each crate on a ball of its
        // reach, so beneath its floor is open air down to the ground.)
        if (over && p.y < lid - 0.02F && p.y > lid - crate_size + 0.02F) {
            ++inside;
        }
    }
    const EmitterState& chips = simulation.emitter(2U);
    std::size_t chips_inside = 0U;
    for (std::size_t i = 0U; i < chips.alive(); ++i) {
        const Vec3 p = chips.position[i];
        const int column = static_cast<int>(std::lround((p.x - x0) / d));
        const int row = static_cast<int>(std::lround((p.z - z0) / d));
        const float across = std::fabs(p.x - (x0 + static_cast<float>(column) * d));
        const float along = std::fabs(p.z - (z0 + static_cast<float>(row) * d));
        // Only where there is a crate: chips thrown sideways land beyond
        // the rows, where the lattice names a crate that is not there.
        const bool crate = column >= 0 && column < static_cast<int>(crate_columns) && row >= 0 &&
                           row < static_cast<int>(crate_rows);
        if (crate && across < 0.5F * crate_size - 0.02F && along < 0.5F * crate_size - 0.02F &&
            p.y < lid - 0.02F && p.y > lid - crate_size + 0.02F) {
            ++chips_inside;
        }
    }
    std::cout << "  " << resting << " flakes rest on the lids of " << covered.size() << " crates; " << inside
              << " flakes and " << chips_inside << " chips inside one\n";
    AJFX_CHECK(inside == 0U && chips_inside == 0U);
    AJFX_CHECK(covered.size() > 300U);
}

// ---------------------------------------------------------------- wakes

// Two balls 0.5 across running along x at 1 unit up, at z -2 and z 2, of
// kinds 0 and 1, through still smoke spread 6 long, 1 high and 6 wide. With
// `speed` 0 they stand where they started. The smoke's Wake follows `kinds`
// (empty: both).
Effect balls_through_smoke(float speed, std::vector<std::uint32_t> kinds, bool collide) {
    EffectBuilder builder{"wake"};
    builder.emitter("balls")
        .capacity(4U)
        .spawn(SpawnBurst{.count = 1U, .body = 0U})
        .spawn(SpawnBurst{.count = 1U, .body = 1U})
        .init(Lifetime{.min = 100.0F, .max = 100.0F})
        .init(ShapeLocation{.shape = Shape::Point, .center = Vec3{-1.0F, 1.0F, -2.0F}, .only = Scope{.sources = {0U}}})
        .init(ShapeLocation{.shape = Shape::Point, .center = Vec3{-1.0F, 1.0F, 2.0F}, .only = Scope{.sources = {1U}}})
        .init(Velocity{.direction = Vec3{1.0F, 0.0F, 0.0F}, .speed_min = speed, .speed_max = speed, .spread = 0.0F})
        .init(InitSize{.min = 0.5F, .max = 0.5F})
        .update(RigidPhysics{.shape = RigidShape::Sphere,
                             .angular_damping = 0.0F,
                             .allow_sleep = false,
                             .bodies = {RigidBodyKind{.shape = RigidShape::Sphere}}});
    EmitterBuilder& smoke = builder.emitter("smoke");
    smoke.capacity(6000U)
        .seed(4U)
        .spawn(SpawnBurst{.count = 6000U})
        .init(Lifetime{.min = 100.0F, .max = 100.0F})
        .init(ShapeLocation{.shape = Shape::Box, .center = Vec3{2.0F, 1.0F, 0.0F}, .size = Vec3{6.0F, 1.0F, 6.0F}})
        .init(InitSize{.min = 0.05F, .max = 0.05F})
        .update(Wake{.emitter = "balls", .strength = 8.0F, .radius = 4.0F, .bodies = Scope{.kinds = std::move(kinds)}});
    if (collide) {
        smoke.update(BodyCollision{.emitter = "balls", .restitution = 0.0F, .friction = 0.0F});
    }
    return builder.build().value();
}

// The smoke within `half_width` of the line a ball runs along at z, behind
// its middle, and how much of it is moving along. What the wake drags keeps
// up with the ball, so most of it gathers just behind it rather than
// staying where it was passed.
struct Trail final {
    std::size_t count{0U};
    std::size_t dragged{0U};
    std::size_t still{0U};
};

Trail trail(const EmitterState& smoke, float ball_x, float z, float half_width) {
    Trail out;
    for (std::size_t i = 0U; i < smoke.alive(); ++i) {
        const Vec3 p = smoke.position[i];
        if (std::fabs(p.z - z) > half_width || std::fabs(p.y - 1.0F) > half_width || p.x > ball_x) {
            continue;
        }
        ++out.count;
        out.dragged += smoke.velocity[i].x > 0.3F ? 1U : 0U;
        out.still += smoke.velocity[i] == Vec3{} ? 1U : 0U;
    }
    return out;
}

void a_moving_body_drags_smoke_along_behind_it() {
    Simulation moving = make(balls_through_smoke(4.0F, {}, false));
    moving.advance_to(60U);
    const EmitterState& balls = moving.emitter(0U);
    AJFX_CHECK(balls.alive() == 2U && balls.position[0].x > 2.0F);
    const EmitterState& smoke = moving.emitter(1U);
    const Trail behind = trail(smoke, balls.position[0].x, -2.0F, 0.2F);
    std::size_t out_of_reach_moved = 0U;
    std::size_t out_of_reach = 0U;
    for (std::size_t i = 0U; i < smoke.alive(); ++i) {
        // Beyond the balls' reach (four of their half sizes, 1) on every side.
        if (std::fabs(smoke.position[i].z) < 0.9F) {
            ++out_of_reach;
            out_of_reach_moved += smoke.velocity[i] == Vec3{} ? 0U : 1U;
        }
    }
    std::cout << "  ball at x " << balls.position[0].x << ": " << behind.dragged << " of " << behind.count
              << " puffs behind it dragged along; " << out_of_reach_moved << " of " << out_of_reach
              << " out of reach moved\n";
    AJFX_CHECK(behind.count > 20U && behind.dragged * 10U >= behind.count * 9U);
    AJFX_CHECK(out_of_reach > 500U && out_of_reach_moved == 0U);

    // Standing still, the same balls leave every puff where it was.
    Simulation still = make(balls_through_smoke(0.0F, {}, false));
    still.advance_to(60U);
    for (const Vec3& v : still.emitter(1U).velocity) {
        AJFX_CHECK(v == Vec3{});
    }
}

void a_moving_body_pushes_smoke_out_of_its_way() {
    Simulation simulation = make(balls_through_smoke(4.0F, {}, true));
    simulation.advance_to(60U);
    const EmitterState& balls = simulation.emitter(0U);
    const EmitterState& smoke = simulation.emitter(1U);
    std::size_t inside = 0U;
    for (const Vec3& p : smoke.position) {
        for (const Vec3& centre : balls.position) {
            const Vec3 d = p - centre;
            inside += std::sqrt(dot(d, d)) < 0.25F - 1.0e-4F ? 1U : 0U;
        }
    }
    // Some smoke now sits on the ball's front, pushed along.
    std::size_t riding = 0U;
    for (std::size_t i = 0U; i < smoke.alive(); ++i) {
        const Vec3 d = smoke.position[i] - balls.position[0];
        riding += std::sqrt(dot(d, d)) < 0.3F && smoke.velocity[i].x > 1.0F ? 1U : 0U;
    }
    std::cout << "  " << inside << " puffs inside a ball, " << riding << " pushed along on its front\n";
    AJFX_CHECK(inside == 0U && riding > 0U);
}

void a_wake_follows_only_the_bodies_it_names() {
    // Kind 1 is the ball at z 2.
    Simulation simulation = make(balls_through_smoke(4.0F, {1U}, false));
    simulation.advance_to(60U);
    const EmitterState& balls = simulation.emitter(0U);
    const EmitterState& smoke = simulation.emitter(1U);
    const Trail named = trail(smoke, balls.position[1].x, 2.0F, 0.2F);
    const Trail other = trail(smoke, balls.position[0].x, -2.0F, 0.2F);
    std::cout << "  " << named.dragged << " of " << named.count << " behind the named ball dragged, " << other.still
              << " of " << other.count << " behind the other still\n";
    AJFX_CHECK(named.count > 20U && named.dragged * 10U >= named.count * 9U);
    AJFX_CHECK(other.count > 20U && other.still == other.count);
}

void wakes_agree_with_every_body_in_turn_rerun_and_seek_exactly() {
    const Effect effect = balls_through_smoke(4.0F, {}, true);
    Simulation grid = make(effect, true);
    Simulation every = make(effect, false);
    Simulation again = make(effect, true);
    for (const std::uint64_t tick : {10U, 45U, 90U}) {
        grid.advance_to(tick);
        every.advance_to(tick);
        again.advance_to(tick);
        AJFX_CHECK(grid.emitter(1U) == every.emitter(1U));
        AJFX_CHECK(grid.state() == again.state());
    }
    CheckpointRing ring{20U, 8U};
    Simulation seeker = make(effect);
    for (const std::uint64_t target : {70U, 13U, 90U, 41U}) {
        ring.seek(seeker, target);
        Simulation straight = make(effect);
        straight.advance_to(target);
        AJFX_CHECK(seeker.state() == straight.state());
    }
}

// ---------------------------------------------------------------- refusals

bool refused(const Result<Effect>& built, const std::string& fragment) {
    if (built.ok()) {
        return false;
    }
    std::cout << "  refused: " << built.error().message << '\n';
    return built.error().message.find(fragment) != std::string::npos;
}

void impossible_wakes_are_refused() {
    const auto with = [](Wake wake, std::uint32_t capacity) {
        EffectBuilder builder{"x"};
        builder.emitter("balls").capacity(capacity).init(Lifetime{}).update(RigidPhysics{});
        builder.emitter("smoke").init(Lifetime{}).update(std::move(wake));
        return builder.build();
    };
    AJFX_CHECK(with(Wake{.emitter = "balls"}, 8U).ok());
    AJFX_CHECK(with(Wake{.emitter = "balls"}, max_body_colliders).ok());
    AJFX_CHECK(refused(with(Wake{.emitter = "balls"}, max_body_colliders + 1U), "may stir particles"));
    AJFX_CHECK(refused(with(Wake{.emitter = "smoke"}, 8U), "rigid_physics"));
    AJFX_CHECK(refused(with(Wake{.emitter = "nobody"}, 8U), "none of"));
    AJFX_CHECK(refused(with(Wake{.emitter = "balls", .radius = 0.0F}, 8U), "radius"));
    AJFX_CHECK(refused(with(Wake{.emitter = "balls", .min_speed = -1.0F}, 8U), "min_speed"));
    AJFX_CHECK(refused(with(Wake{.emitter = "balls", .bodies = Scope{.kinds = {3U}}}, 8U), "body kind 3"));
    // Bodies are moved by their engine, not stirred.
    EffectBuilder rigid{"x"};
    rigid.emitter("balls").capacity(8U).init(Lifetime{}).update(RigidPhysics{});
    rigid.emitter("more").capacity(8U).init(Lifetime{}).update(RigidPhysics{}).update(Wake{.emitter = "balls"});
    AJFX_CHECK(refused(rigid.build(), "cannot act on rigid physics particles"));
}

} // namespace

int main() {
    g_solver = rigid::make_jolt_solver();
    const ajfx::test::Case cases[] = {
        {"the_grid_never_misses_a_body_in_reach", the_grid_never_misses_a_body_in_reach},
        {"the_grid_meets_what_every_body_in_turn_meets", the_grid_meets_what_every_body_in_turn_meets},
        {"snow_settles_on_a_thousand_crates", snow_settles_on_a_thousand_crates},
        {"a_moving_body_drags_smoke_along_behind_it", a_moving_body_drags_smoke_along_behind_it},
        {"a_moving_body_pushes_smoke_out_of_its_way", a_moving_body_pushes_smoke_out_of_its_way},
        {"a_wake_follows_only_the_bodies_it_names", a_wake_follows_only_the_bodies_it_names},
        {"wakes_agree_with_every_body_in_turn_rerun_and_seek_exactly",
         wakes_agree_with_every_body_in_turn_rerun_and_seek_exactly},
        {"impossible_wakes_are_refused", impossible_wakes_are_refused},
    };
    const int result = ajfx::test::run(cases);
    g_solver.reset();
    return result;
}
