#include "test_support.hpp"

#include "blast.hpp"
#include "sandbox.hpp"

#include <bit>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

// The preview's sandbox tools, headless: a click picks a solid thing or the
// floor, and each tool writes one block at the playhead's time, which the
// compiled simulation then plays -- a dropped crate is there after that time
// and not before, an explosion throws things out from where it was clicked, a
// push moves what was clicked away from the camera. A new seed changes the
// simulation and the page before it plays it again to the bit; and a bake goes
// stale when a blast in another layer moves.
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

// The bodies of `block` where the run stands now.
std::vector<StageBody> bodies_of(const Compiled& compiled, const Run& run, Uid block) {
    const std::optional<std::size_t> world = solid_world_index(compiled);
    CHECK(world.has_value());
    std::vector<StageBody> out;
    for (const StageBody& body : stage_bodies(compiled, run.simulation->emitter(*world))) {
        if (body.block == block) {
            out.push_back(body);
        }
    }
    return out;
}

// Every position of every emitter, to the bit.
bool same_positions(const ajfx::Simulation& a, const ajfx::Simulation& b) {
    const ajfx::SimulationState& x = a.state();
    const ajfx::SimulationState& y = b.state();
    if (x.tick != y.tick || x.emitters.size() != y.emitters.size()) {
        return false;
    }
    for (std::size_t e = 0U; e < x.emitters.size(); ++e) {
        const std::vector<ajfx::Vec3>& p = x.emitters[e].position;
        const std::vector<ajfx::Vec3>& q = y.emitters[e].position;
        if (p.size() != q.size()) {
            return false;
        }
        for (std::size_t i = 0U; i < p.size(); ++i) {
            if (std::bit_cast<std::uint32_t>(p[i].x) != std::bit_cast<std::uint32_t>(q[i].x) ||
                std::bit_cast<std::uint32_t>(p[i].y) != std::bit_cast<std::uint32_t>(q[i].y) ||
                std::bit_cast<std::uint32_t>(p[i].z) != std::bit_cast<std::uint32_t>(q[i].z)) {
                return false;
            }
        }
    }
    return true;
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

// Drop a thing: a crate of the crate's own material, a metre above where the
// floor was clicked, appearing at the playhead. It is not there before that
// time, it is after, and it falls; the loop still ends a few seconds later
// rather than a solid layer's whole minute.
void a_dropped_crate_appears_at_the_playhead() {
    Document document = one_crate();
    const Uid layer = document.layers[0].uid;
    const ajfx::Vec3 floor_point{1.2F, 0.0F, 0.0F};
    const float seconds = 1.0F;
    const std::string material = sandbox_material(document, 0U, layer);
    CHECK(material == "wood");
    const std::optional<SandboxEdit> made = drop_thing(document, layer, floor_point, seconds, material);
    CHECK(made.has_value() && made->layer == layer);
    const Block* dropped = document.block(made->block);
    CHECK(dropped != nullptr && dropped->kind == BlockKind::Object);
    CHECK(near(number_of(*dropped, "appear"), seconds));
    CHECK(choice_of(*dropped, "material") == "wood" && toggle_of(*dropped, "own_material"));
    const Value at = get(*dropped, "at");
    const float top = 0.2F + 1.0F;
    CHECK(near(at.x, 1.2F) && near(at.y, top) && near(at.z, 0.0F));

    Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    const ajfx::Emitter& world = emitter_of(compiled, document.layers[0]);
    const auto* burst = find_module<ajfx::SpawnBurst>(world);
    CHECK(burst != nullptr && near(burst->time, seconds) && burst->count == 1U);
    const float loop = loop_seconds(document, compiled);
    std::cout << "  the page with a crate dropped at 1 s loops over " << loop << " s\n";
    CHECK(loop > seconds + 1.0F && loop < 10.0F);

    Run run = start(compiled);
    run.simulation->advance_to(55U);
    CHECK(bodies_of(compiled, run, made->block).empty());
    CHECK(bodies_of(compiled, run, document.layers[0].blocks[0].uid).size() == 1U);
    run.simulation->advance_to(64U);
    std::vector<StageBody> born = bodies_of(compiled, run, made->block);
    CHECK(born.size() == 1U);
    CHECK(near(born[0].center.x, 1.2F, 0.01F) && near(born[0].center.z, 0.0F, 0.01F));
    CHECK(born[0].center.y > top - 0.1F && born[0].center.y <= top + 1.0e-4F);
    // A second later it has fallen to the floor.
    run.simulation->advance_to(130U);
    born = bodies_of(compiled, run, made->block);
    CHECK(born.size() == 1U);
    std::cout << "  the dropped crate stands at " << born[0].center.y << " m a second after it appeared\n";
    CHECK(born[0].center.y < 0.4F && born[0].center.y > 0.0F);
    // It is on the transport, at its time.
    const std::vector<PageEvent> events = page_events(document);
    CHECK(events.size() == 1U && events[0].block == made->block && near(events[0].seconds, seconds));
    CHECK(events[0].label == "Object appears at 1 s");

    // On a page with no solid layer, a solid layer is made for it.
    Document empty;
    const std::optional<SandboxEdit> first = drop_thing(empty, 0U, floor_point, 0.5F);
    CHECK(first.has_value() && empty.layers.size() == 1U && empty.layers[0].kind == LayerKind::Rigid);
    CHECK(empty.layers[0].baked);
    CHECK(compile(empty, Purpose::Preview).error.empty());
}

// Explode here: an Explosion where the floor was clicked, at the playhead,
// that throws the pieces of a heap out from that point.
void exploding_here_throws_things_out_from_the_click() {
    const auto heap = [](bool exploding, Uid& explosion) {
        Document document;
        Layer layer = make_layer(document, LayerWhat::Solid, "Rubble");
        add(document, layer, "object",
            {{"model", choice("cube")},
             {"arrange", choice("heap")},
             {"count", number(12.0F)},
             {"size", number(0.25F)},
             {"at", vec3(0.0F, 0.0F, 0.0F)}});
        document.layers.push_back(std::move(layer));
        if (exploding) {
            const std::optional<SandboxEdit> made =
                explode_here(document, document.layers[0].uid, ajfx::Vec3{0.8F, 0.0F, 0.0F}, 0.5F);
            CHECK(made.has_value());
            explosion = made->block;
        }
        return document;
    };
    Uid explosion = 0U;
    Document document = heap(true, explosion);
    const Block* block = document.block(explosion);
    CHECK(block != nullptr && block->kind == BlockKind::Explosion);
    CHECK(near(number_of(*block, "time"), 0.5F));
    const Value at = get(*block, "at");
    CHECK(near(at.x, 0.8F) && near(at.y, 0.0F) && near(at.z, 0.0F));

    Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect->blasts.size() == 1U);
    const std::uint64_t tick = ajfx::blast_tick(compiled.effect->blasts[0], 60U);
    CHECK(tick == 30U);
    Run run = start(compiled);
    run.simulation->advance_to(tick + 2U);
    const ajfx::EmitterState& s = run.simulation->emitter(*solid_world_index(compiled));
    CHECK(s.alive() == 12U);
    // Each piece moves away from the click, across the floor.
    std::size_t outward = 0U;
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        const ajfx::Vec3 from{s.position[i].x - 0.8F, 0.0F, s.position[i].z};
        const ajfx::Vec3 going{s.velocity[i].x, 0.0F, s.velocity[i].z};
        if (ajfx::dot(from, going) > 0.0F) {
            ++outward;
        }
    }
    std::cout << "  " << outward << " of 12 pieces fly out from the click\n";
    CHECK(outward >= 10U);

    // Against the same heap with no explosion, the pieces end up further from
    // the click.
    Uid none = 0U;
    Document calm = heap(false, none);
    Compiled quiet = compile(calm, Purpose::Preview);
    Run still = start(quiet);
    run.simulation->advance_to(tick + 40U);
    still.simulation->advance_to(tick + 40U);
    const auto mean_from_click = [](const ajfx::EmitterState& state) {
        float total = 0.0F;
        for (const ajfx::Vec3& p : state.position) {
            total += std::sqrt((p.x - 0.8F) * (p.x - 0.8F) + p.z * p.z);
        }
        return total / static_cast<float>(std::max<std::size_t>(state.alive(), 1U));
    };
    const float thrown = mean_from_click(run.simulation->emitter(*solid_world_index(compiled)));
    const float stood = mean_from_click(still.simulation->emitter(*solid_world_index(quiet)));
    std::cout << "  the pieces stand " << stood << " m from the click, " << thrown << " m once blown out\n";
    CHECK(thrown > stood + 0.2F);
}

// Push: a click on the crate pushes it at its middle away from the camera,
// and it slides away; untouched, it stays put.
void a_push_moves_the_clicked_thing_away_from_the_camera() {
    Document document = one_crate();
    const Uid crate = document.layers[0].blocks[0].uid;
    Compiled before = compile(document, Purpose::Preview);
    Run first = start(before);
    // Picked where it stands at the playhead, when the push will happen.
    first.simulation->advance_to(30U);
    const std::vector<StageBody> standing = stage_bodies(before, first.simulation->emitter(*solid_world_index(before)));
    const ajfx::Vec3 eye{0.6F, 1.0F, 4.0F};
    const ajfx::Vec3 forward = unit(ajfx::Vec3{0.0F, 0.2F, 0.0F} - eye);
    const std::optional<StageHit> hit = pick_stage(standing, eye, forward, stage_floor(document));
    CHECK(hit.has_value() && hit->on_body && hit->block == crate);
    // Away from the camera, level.
    const ajfx::Vec3 away = unit(ajfx::Vec3{forward.x, 0.0F, forward.z});
    const std::optional<SandboxEdit> made = push_here(document, document.layers[0].uid, hit->center, away, 0.5F);
    CHECK(made.has_value());
    const Block* push = document.block(made->block);
    CHECK(push != nullptr && push->kind == BlockKind::Push && near(number_of(*push, "time"), 0.5F));
    const Value toward = get(*push, "toward");
    CHECK(near(toward.x, away.x, 1.0e-4F) && near(toward.z, away.z, 1.0e-4F));

    Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect->blasts.size() == 1U);
    // What stands at the point is thrown along the push at its power.
    const ajfx::BlastPush there = ajfx::blast_push(compiled.effect->blasts[0], hit->center);
    CHECK(near(there.speed, number_of(*push, "power"), 1.0e-3F));
    CHECK(ajfx::dot(there.direction, away) > 0.999F);
    Run run = start(compiled);
    run.simulation->advance_to(50U);
    const std::vector<StageBody> moved = bodies_of(compiled, run, crate);
    CHECK(moved.size() == 1U);
    const ajfx::Vec3 travel = moved[0].center - hit->center;
    std::cout << "  the pushed crate moved " << ajfx::dot(travel, away) << " m away from the camera\n";
    CHECK(ajfx::dot(travel, away) > 0.1F);
    // Untouched, it stands where it was.
    first.simulation->advance_to(50U);
    const std::vector<StageBody> still = bodies_of(before, first, crate);
    CHECK(still.size() == 1U && ajfx::dot(still[0].center - hit->center, away) < 0.02F);
}

// The die deals new seeds as one edit: the simulation changes, and the page
// as it was plays again to the bit. The same press on the same page always
// deals the same seeds, and a run played twice is the same run.
void a_new_seed_changes_the_run_and_undo_restores_it() {
    Document original = recipe("rock_pile");
    const auto run_of = [](const Document& document) {
        Compiled compiled = compile(document, Purpose::Preview);
        CHECK(compiled.error.empty());
        Run run = start(compiled);
        run.simulation->advance_to(40U);
        return run;
    };
    Run a = run_of(original);
    Run again = run_of(original);
    CHECK(same_positions(*a.simulation, *again.simulation));

    Document rolled = original;
    CHECK(reroll_seed(rolled, 0U));
    for (std::size_t l = 0U; l < rolled.layers.size(); ++l) {
        CHECK(rolled.layers[l].seed != original.layers[l].seed && rolled.layers[l].seed != 0U);
    }
    Document rolled_again = original;
    CHECK(reroll_seed(rolled_again, 0U));
    CHECK(rolled_again == rolled);
    Run b = run_of(rolled);
    CHECK(!same_positions(*a.simulation, *b.simulation));

    // Undo is the page as it was.
    const Document undone = original;
    Run c = run_of(undone);
    CHECK(same_positions(*a.simulation, *c.simulation));

    // A solid layer's seed is its world's: one press re-rolls every solid
    // layer, and leaves the rest alone.
    Document mixed = recipe("breaking_crates");
    const Document kept = mixed;
    Uid solid = 0U;
    for (const Layer& layer : mixed.layers) {
        if (layer.kind == LayerKind::Rigid) {
            solid = layer.uid;
        }
    }
    CHECK(solid != 0U);
    CHECK(reroll_seed(mixed, solid));
    for (std::size_t l = 0U; l < mixed.layers.size(); ++l) {
        const bool rigid = mixed.layers[l].kind == LayerKind::Rigid;
        CHECK((mixed.layers[l].seed != kept.layers[l].seed) == rigid);
    }
    CHECK(!reroll_seed(mixed, 999999U));
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
    // Under automatic baking the coach says it quietly.
    status.automatic = true;
    const std::vector<Advice> notes = advise(moved, compile(moved, Purpose::Preview), &status);
    CHECK(std::none_of(notes.begin(), notes.end(),
                       [&](const Advice& a) { return a.layer == crates && a.level == AdviceLevel::Warning; }));

    // A Push added in the dust layer, at the playhead, does too.
    Document pushed = document;
    CHECK(push_here(pushed, pushed.layers[1].uid, ajfx::Vec3{0.0F, 0.2F, 0.0F}, ajfx::Vec3{0.0F, 0.0F, -1.0F}, 1.0F)
              .has_value());
    status = bake_status(pushed, compile(pushed, Purpose::Export), &baked.value());
    CHECK(status.layer(crates)->state == BakeState::Stale);
    CHECK(status.layer(crates)->why.find("Push") != std::string::npos);
}

} // namespace

void add_sandbox_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"a_click_picks_the_thing_or_the_floor", a_click_picks_the_thing_or_the_floor});
    cases.push_back({"a_dropped_crate_appears_at_the_playhead", a_dropped_crate_appears_at_the_playhead});
    cases.push_back({"exploding_here_throws_things_out_from_the_click", exploding_here_throws_things_out_from_the_click});
    cases.push_back(
        {"a_push_moves_the_clicked_thing_away_from_the_camera", a_push_moves_the_clicked_thing_away_from_the_camera});
    cases.push_back({"a_new_seed_changes_the_run_and_undo_restores_it", a_new_seed_changes_the_run_and_undo_restores_it});
    cases.push_back(
        {"a_bake_goes_stale_when_a_blast_in_another_layer_moves", a_bake_goes_stale_when_a_blast_in_another_layer_moves});
}
