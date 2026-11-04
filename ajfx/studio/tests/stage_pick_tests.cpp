#include "test_support.hpp"

#include "stage_pick.hpp"

#include <bit>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

// Picking in the preview, headless: a click picks a solid thing or the
// floor, from where the simulation has it.
namespace {

using namespace studio_test;

struct Run final {
    std::unique_ptr<ajfx::RigidSolver> solver;
    std::optional<ajfx::Simulation> simulation;
};

Run start(Compiled& compiled) {
    std::string issue;
    make_cuts(compiled, issue);
    if (!issue.empty() || !compiled.effect) {
        throw std::runtime_error{"the cuts were not made: " + issue};
    }
    Run run;
    run.solver = ajfx::rigid::make_jolt_solver();
    ajfx::Result<ajfx::Simulation> created = ajfx::Simulation::create(
        *compiled.effect, 1U, ajfx::SimulationOptions{.meshes = compiled.meshes, .rigid = run.solver.get()});
    if (!created) {
        throw std::runtime_error{"the simulation was not made: " + created.error().message};
    }
    run.simulation.emplace(std::move(created).value());
    return run;
}

ajfx::Vec3 unit(ajfx::Vec3 v) {
    return v * (1.0F / std::sqrt(ajfx::dot(v, v)));
}

// One crate standing on the floor in the middle of the stage.
Document one_crate() {
    Document document = new_scene();
    const Uid crate = add_object(document);
    document.scene->item(crate)->name = "Crate";
    auto& box = std::get<Object>(document.scene->item(crate)->data);
    box.collider = ObjectCollider::Box;
    box.size = 0.4F;
    box.position = {0.0F, 0.2F, 0.0F};
    box.density = 600.0F;
    return document;
}

// A click on the floor picks the floor, a click on the crate picks the
// crate, from where the simulation has it.
void a_click_picks_the_thing_or_the_floor() {
    Document document = one_crate();
    const Uid crate = item_named(document, "Crate");
    const Uid ground = item_named(document, "Ground");
    Compiled compiled = compile(document);
    CHECK(compiled.error.empty());
    Run run = start(compiled);
    const std::optional<std::size_t> world = solid_world_index(compiled);
    CHECK(world.has_value());
    // The crate's body, then the floor it stands on, which is a prop.
    const std::vector<StageBody> standing = stage_bodies(compiled, run.simulation->emitter(*world));
    CHECK(standing.size() == 2U && standing[0].item == crate && !standing[0].piece && standing[0].spawn_index);
    CHECK(standing[1].item == ground && !standing[1].spawn_index);
    CHECK(standing[0].radius > 0.2F && standing[0].radius < 0.5F);
    // A lone Object's pile stands it a body's reach above its place at step 0,
    // and it settles onto the floor; a click picks it where it is at the
    // playhead, as the preview's samples have it, so pick half a second in.
    run.simulation->advance_to(30U);
    const std::vector<StageBody> settled = stage_bodies(compiled, run.simulation->emitter(*world));
    CHECK(settled.size() == 2U && near(settled[0].center.y, 0.2F, 0.02F));
    const std::vector<StageBody> crate_only{settled[0]};

    const ajfx::Vec3 eye{0.0F, 1.2F, 4.0F};
    const std::optional<StageHit> on_crate =
        pick_stage(crate_only, eye, unit(ajfx::Vec3{0.0F, 0.2F, 0.0F} - eye), stage_floor(document));
    CHECK(on_crate.has_value() && on_crate->on_body && on_crate->item == crate);
    CHECK(on_crate->point.z > 0.0F && on_crate->point.z < 0.5F);

    const ajfx::Vec3 floor_point{1.5F, 0.0F, 0.5F};
    const std::optional<StageHit> on_floor =
        pick_stage(crate_only, eye, unit(floor_point - eye), stage_floor(document));
    CHECK(on_floor.has_value() && !on_floor->on_body && on_floor->item == 0U);
    CHECK(near(on_floor->point.x, 1.5F, 1.0e-3F) && near(on_floor->point.y, 0.0F, 1.0e-4F) &&
          near(on_floor->point.z, 0.5F, 1.0e-3F));

    // Looking up, there is neither.
    CHECK(!pick_stage(crate_only, eye, ajfx::Vec3{0.0F, 1.0F, 0.0F}, 0.0F).has_value());
    // The samples a preview keeps are read at the newest at or before a frame.
    const std::vector<StageBodies> samples{{0U, standing}, {6U, {}}, {12U, standing}};
    CHECK(stage_bodies_at(samples, 0U) == &samples[0]);
    CHECK(stage_bodies_at(samples, 11U) == &samples[1]);
    CHECK(stage_bodies_at(samples, 400U) == &samples[2]);
}

} // namespace

void add_stage_pick_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"a_click_picks_the_thing_or_the_floor", a_click_picks_the_thing_or_the_floor});
}
