#include "test_support.hpp"

#include "blast.hpp"
#include "stage_pick.hpp"

#include <bit>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

// Picking in the preview, headless: a click picks a solid thing or the
// floor, from where the simulation has it. And a bake goes stale when a blast
// in another layer moves.
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

Block& add(Document& document, Layer& layer, std::string_view kind,
           std::initializer_list<std::pair<std::string_view, Value>> values = {}) {
    std::optional<Block> block = make_block(document, kind);
    CHECK(block.has_value());
    for (const auto& [control, value] : values) {
        block->set(control, value);
    }
    insert_block(layer, layer.blocks.size(), std::move(*block));
    return layer.blocks.back();
}

ajfx::Vec3 unit(ajfx::Vec3 v) {
    return v * (1.0F / std::sqrt(ajfx::dot(v, v)));
}

// One wooden crate standing on the floor at the middle of a solid layer.
Document one_crate() {
    Document document;
    Layer layer = make_layer(document, LayerWhat::Solid, "Crates");
    add(document, layer, "object",
        {{"model", choice("cube")},
         {"collides", choice("block")},
         {"size", number(0.4F)},
         {"extent", vec3(0.4F, 0.4F, 0.4F)},
         {"at", vec3(0.0F, 0.2F, 0.0F)},
         {"material", choice("wood")}});
    document.layers.push_back(std::move(layer));
    CHECK(apply_material(document, document.layers[0].blocks[0].uid));
    return document;
}

// A click on the floor picks the floor, a click on the crate picks the
// crate's block, from where the simulation has it.
void a_click_picks_the_thing_or_the_floor() {
    Document document = one_crate();
    const Uid crate = document.layers[0].blocks[0].uid;
    Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty());
    Run run = start(compiled);
    const std::optional<std::size_t> world = solid_world_index(compiled);
    CHECK(world.has_value());
    const std::vector<StageBody> standing = stage_bodies(compiled, run.simulation->emitter(*world));
    CHECK(standing.size() == 1U && standing[0].block == crate && standing[0].layer == document.layers[0].uid);
    CHECK(standing[0].radius > 0.2F && standing[0].radius < 0.5F);
    // A lone Object's pile stands it a body's reach above its place at step 0,
    // and it settles onto the floor; a click picks it where it is at the
    // playhead, as the preview's samples have it, so pick half a second in.
    run.simulation->advance_to(30U);
    const std::vector<StageBody> settled = stage_bodies(compiled, run.simulation->emitter(*world));
    CHECK(settled.size() == 1U && near(settled[0].center.y, 0.2F, 0.02F));

    const ajfx::Vec3 eye{0.0F, 1.2F, 4.0F};
    const std::optional<StageHit> on_crate =
        pick_stage(settled, eye, unit(ajfx::Vec3{0.0F, 0.2F, 0.0F} - eye), stage_floor(document));
    CHECK(on_crate.has_value() && on_crate->on_body && on_crate->block == crate);
    CHECK(on_crate->point.z > 0.0F && on_crate->point.z < 0.5F);

    const ajfx::Vec3 floor_point{1.5F, 0.0F, 0.5F};
    const std::optional<StageHit> on_floor = pick_stage(settled, eye, unit(floor_point - eye), stage_floor(document));
    CHECK(on_floor.has_value() && !on_floor->on_body && on_floor->block == 0U);
    CHECK(near(on_floor->point.x, 1.5F, 1.0e-3F) && near(on_floor->point.y, 0.0F, 1.0e-4F) &&
          near(on_floor->point.z, 0.5F, 1.0e-3F));

    // Looking up, there is neither.
    CHECK(!pick_stage(settled, eye, ajfx::Vec3{0.0F, 1.0F, 0.0F}, 0.0F).has_value());
    // The samples a preview keeps are read at the newest at or before a frame.
    const std::vector<StageBodies> samples{{0U, standing}, {6U, {}}, {12U, standing}};
    CHECK(stage_bodies_at(samples, 0U) == &samples[0]);
    CHECK(stage_bodies_at(samples, 11U) == &samples[1]);
    CHECK(stage_bodies_at(samples, 400U) == &samples[2]);
}

// The cache keeps a layer as it was; moving an Explosion in another layer
// changes what every layer does, so the solid layer's cache goes stale and
// says which block moved. A Push does too.
void a_bake_goes_stale_when_a_blast_in_another_layer_moves() {
    Document document = one_crate();
    const Uid crates = document.layers[0].uid;
    CHECK(document.layers[0].baked);
    Layer dust = make_layer(document, LayerWhat::Dust, "Dust");
    add(document, dust, "stream");
    const Uid explosion = add(document, dust, "explosion", {{"at", vec3(0.5F, 0.2F, 0.0F)}}).uid;
    document.layers.push_back(std::move(dust));
    CHECK(!document.layers[1].baked);

    Compiled ready = compile(document, Purpose::Export);
    std::string issue;
    make_cuts(ready, issue);
    CHECK(issue.empty() && ready.effect.has_value());
    const std::unique_ptr<ajfx::RigidSolver> solver = ajfx::rigid::make_jolt_solver();
    const ajfx::Result<BakeRecord> baked = bake_page(document, ready, 5U, solver.get());
    CHECK(baked.ok());
    BakeStatus status = bake_status(document, compile(document, Purpose::Export), &baked.value());
    CHECK(status.current && status.layer(crates)->state == BakeState::Current);

    Document moved = document;
    moved.block(explosion)->set("at", vec3(0.9F, 0.2F, 0.0F));
    status = bake_status(moved, compile(moved, Purpose::Export), &baked.value());
    const LayerBake* stale = status.layer(crates);
    CHECK(stale->state == BakeState::Stale && !status.current);
    std::cout << "  stale because " << stale->why << '\n';
    CHECK(stale->block == explosion && stale->why.find("Explosion") != std::string::npos);
}

} // namespace

void add_stage_pick_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"a_click_picks_the_thing_or_the_floor", a_click_picks_the_thing_or_the_floor});
    cases.push_back(
        {"a_bake_goes_stale_when_a_blast_in_another_layer_moves", a_bake_goes_stale_when_a_blast_in_another_layer_moves});
}
