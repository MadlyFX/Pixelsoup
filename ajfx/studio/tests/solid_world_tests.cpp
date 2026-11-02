#include "test_support.hpp"

#include "blast.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

// The page's solid layers as one world, and what is born of it: puffs and
// sparks where its things hit, trails behind them as they move, and a glow
// that cools on what a blast broke. Each page is built into an
// ajfx::Simulation, as the preview does, and measured.
namespace {

using namespace studio_test;

std::size_t index_of(const Compiled& compiled, const Layer& layer) {
    const LayerReport* report = compiled.layer(layer.uid);
    CHECK(report != nullptr && report->included && compiled.effect.has_value());
    for (std::size_t i = 0U; i < compiled.effect->emitters.size(); ++i) {
        if (compiled.effect->emitters[i].name == report->emitter) {
            return i;
        }
    }
    throw std::runtime_error{"layer not in the effect"};
}

// A compiled page with its cuts made, built into a simulation. The solver is
// held beside it, since the simulation uses it.
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
    conform_block(*block, layer.kind);
    insert_block(layer, layer.blocks.size(), std::move(*block));
    return layer.blocks.back();
}

// Whether particle `i` of `state` is the layer's, by its report's scope.
bool belongs(const LayerReport& report, const ajfx::EmitterState& state, std::size_t i) {
    return report.scope.empty() ||
           ajfx::in_scope(report.scope, state.spawn_index[i], i < state.body_kind.size() ? state.body_kind[i] : 0U);
}

// One loose object standing, or dropped, as a layer of its own.
Layer one_object(Document& document, std::string name, std::string_view model, std::string_view collides, float size,
                 ajfx::Vec3 at) {
    Layer layer = make_layer(document, LayerWhat::Solid, std::move(name));
    add(document, layer, "object",
        {{"model", choice(std::string{model})},
         {"collides", choice(std::string{collides})},
         {"size", number(size)},
         {"at", vec3(at.x, at.y, at.z)},
         {"own_material", toggle(true)},
         {"friction", number(0.9F)},
         {"bounce", number(0.05F)},
         {"density", number(600.0F)}});
    return layer;
}

// A particle as it is compared to the bit, by its spawn index.
struct Snapshot final {
    std::uint32_t index{};
    ajfx::Vec3 position{};
    ajfx::Vec3 velocity{};
    ajfx::Quat orientation{};
    ajfx::Vec3 spin{};
    ajfx::Color color{};
    float size{};
    float age{};

    friend bool operator==(const Snapshot&, const Snapshot&) = default;
};

std::vector<Snapshot> snapshot(const LayerReport& report, const ajfx::EmitterState& state) {
    std::vector<Snapshot> out;
    for (std::size_t i = 0U; i < state.alive(); ++i) {
        if (belongs(report, state, i)) {
            out.push_back(Snapshot{state.spawn_index[i], state.position[i], state.velocity[i], state.orientation[i],
                                   state.angular_velocity[i], state.color[i], state.size[i], state.age[i]});
        }
    }
    std::sort(out.begin(), out.end(), [](const Snapshot& a, const Snapshot& b) { return a.index < b.index; });
    return out;
}

// The particles of `state` not seen before, recorded as seen.
std::vector<std::size_t> newborn(const ajfx::EmitterState& state, std::set<std::uint32_t>& seen) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0U; i < state.alive(); ++i) {
        if (seen.insert(state.spawn_index[i]).second) {
            out.push_back(i);
        }
    }
    return out;
}

float distance(ajfx::Vec3 a, ajfx::Vec3 b) {
    const ajfx::Vec3 d = a - b;
    return std::sqrt(ajfx::dot(d, d));
}

// A crate dropped in one solid layer onto a crate standing in another lands
// on it and stays there. When each solid layer was a world of its own, it
// fell through to the ground.
void loose_things_in_two_solid_layers_knock_into_each_other() {
    Document document;
    document.layers.push_back(one_object(document, "Floor crate", "cube", "block", 0.6F, {0.0F, 0.3F, 0.0F}));
    document.layers.push_back(one_object(document, "Falling crate", "cube", "block", 0.6F, {0.0F, 2.0F, 0.0F}));
    // Each layer its own colour, which it keeps in the one world.
    add(document, document.layers[0], "color", {{"color", color(0.8F, 0.1F, 0.1F)}});
    add(document, document.layers[1], "color", {{"color", color(0.1F, 0.1F, 0.8F)}});
    Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    // One emitter, both layers' bodies in it.
    CHECK(compiled.effect->emitters.size() == 1U && compiled.solids.layers.size() == 2U);
    const std::size_t world = index_of(compiled, document.layers[0]);
    CHECK(world == index_of(compiled, document.layers[1]));
    const LayerReport& below = *compiled.layer(document.layers[0].uid);
    const LayerReport& above = *compiled.layer(document.layers[1].uid);
    CHECK(!below.scope.empty() && !above.scope.empty() && !(below.scope == above.scope));

    Run run = start(compiled);
    run.simulation->advance_to(240U);
    const ajfx::EmitterState& state = run.simulation->emitter(world);
    CHECK(state.alive() == 2U);
    float low = -1.0F;
    float high = -1.0F;
    for (std::size_t i = 0U; i < state.alive(); ++i) {
        if (belongs(below, state, i)) {
            low = state.position[i].y;
            CHECK(state.color[i].r > state.color[i].b);
        } else if (belongs(above, state, i)) {
            high = state.position[i].y;
            CHECK(state.color[i].b > state.color[i].r);
        }
    }
    std::cout << "  the standing crate is at " << low << " m, the one dropped on it at " << high << " m\n";
    CHECK(near(low, 0.3F, 0.05F));
    // On top of it, not through it to the ground.
    CHECK(high > 0.8F && high < 1.0F);
}

// A solid layer added beside another, far enough away that nothing of it
// touches the first, leaves the first layer's bodies exactly as they were:
// its spawn sources, kinds and random values are where they were.
void a_second_solid_layer_leaves_the_first_alone() {
    Document document;
    Layer heap = make_layer(document, LayerWhat::Solid, "Heap");
    add(document, heap, "object",
        {{"model", choice("rock")},
         {"arrange", choice("heap")},
         {"count", number(30.0F)},
         {"size", number(0.15F)},
         {"jitter", number(0.25F)}});
    add(document, heap, "color", {{"color", color(0.3F, 0.28F, 0.25F)}, {"mix", toggle(true)},
                                  {"second", color(0.5F, 0.45F, 0.4F)}});
    document.layers.push_back(std::move(heap));
    const auto first_layer = [](const Document& page, Compiled& compiled) {
        Run run = start(compiled);
        run.simulation->advance_to(150U);
        return snapshot(*compiled.layer(page.layers[0].uid),
                        run.simulation->emitter(index_of(compiled, page.layers[0])));
    };
    Compiled alone = compile(document, Purpose::Preview);
    CHECK(alone.error.empty() && alone.effect.has_value());
    const std::vector<Snapshot> before = first_layer(document, alone);
    CHECK(before.size() == 30U);

    Layer distant = make_layer(document, LayerWhat::Solid, "Far crates");
    add(document, distant, "object",
        {{"model", choice("cube")},
         {"collides", choice("block")},
         {"arrange", choice("stack")},
         {"count", number(4.0F)},
         {"size", number(0.3F)},
         {"width", number(0.6F)},
         {"depth", number(0.6F)},
         {"at", vec3(6.0F, 0.0F, 0.0F)}});
    add(document, distant, "color", {{"color", color(0.2F, 0.6F, 0.2F)}});
    document.layers.push_back(std::move(distant));
    Compiled together = compile(document, Purpose::Preview);
    CHECK(together.error.empty() && together.effect.has_value() && together.solids.layers.size() == 2U);
    const std::vector<Snapshot> after = first_layer(document, together);
    std::cout << "  " << after.size() << " rocks beside a new solid layer, compared to the bit\n";
    CHECK(after == before);
    // And the new layer's things are there.
    Run run = start(together);
    run.simulation->advance_to(10U);
    CHECK(run.simulation->emitter(index_of(together, document.layers[1])).alive() == 34U);
}

// A ball dropped onto the ground, with dust where it hits: born at the hit,
// and more the harder it lands.
void puffs_are_born_where_things_hit_and_more_for_harder_hits() {
    const auto drop = [](float height) {
        Document document;
        document.layers.push_back(one_object(document, "Ball", "rock", "ball", 0.3F, {0.0F, height, 0.0F}));
        Layer dust = make_layer(document, LayerWhat::Dust, "Dust");
        dust.life_min = dust.life_max = 4.0F;
        add(document, dust, "hit_puffs",
            {{"hardness", number(0.5F)}, {"per_hit", number(0.0F)}, {"per_speed", number(4.0F)}});
        document.layers.push_back(std::move(dust));
        Compiled compiled = compile(document, Purpose::Preview);
        CHECK(compiled.error.empty() && compiled.effect.has_value());
        const auto* hits = find_module<ajfx::SpawnFromContacts>(emitter_of(compiled, document.layers[1]));
        CHECK(hits != nullptr && hits->emitter == emitter_of(compiled, document.layers[0]).name);
        const std::size_t ball = index_of(compiled, document.layers[0]);
        const std::size_t puffs = index_of(compiled, document.layers[1]);
        Run run = start(compiled);
        std::set<std::uint32_t> seen;
        float farthest = 0.0F;
        for (std::uint64_t step = 0U; step < 150U; ++step) {
            run.simulation->step();
            const ajfx::EmitterState& bodies = run.simulation->emitter(ball);
            const ajfx::EmitterState& born = run.simulation->emitter(puffs);
            for (const std::size_t i : newborn(born, seen)) {
                CHECK(bodies.alive() == 1U);
                farthest = std::max(farthest, distance(born.position[i], bodies.position[0]));
                // At the ground, where it lands.
                CHECK(born.position[i].y < 0.4F);
            }
        }
        return std::pair<std::size_t, float>{seen.size(), farthest};
    };
    const auto [soft, soft_far] = drop(0.6F);
    const auto [hard, hard_far] = drop(2.4F);
    std::cout << "  a drop from 0.6 m made " << soft << " puffs, from 2.4 m " << hard << "; the farthest "
              << std::max(soft_far, hard_far) << " m from the ball\n";
    CHECK(soft > 0U);
    CHECK(hard * 10U > soft * 14U);
    // Born where it hit: within its radius (0.15 m) and a step's travel.
    CHECK(soft_far < 0.4F && hard_far < 0.4F);
}

// The Rock slide recipe: rocks in one solid layer tumble down a slope in
// another and kick up dust wherever they hit, each puff born beside a rock.
void the_rock_slide_kicks_up_dust_where_rocks_hit() {
    Document document = recipe("rock_slide");
    Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    CHECK(compiled.solids.layers.size() == 2U);
    const std::size_t world = index_of(compiled, layer_named(document, "Rocks"));
    CHECK(world == index_of(compiled, layer_named(document, "Slope")));
    const std::size_t dust = index_of(compiled, layer_named(document, "Slide dust"));
    // Dust is stirred by the moving rocks without being asked; a Stirred
    // block says how instead.
    const auto* wake = find_module<ajfx::Wake>(compiled.effect->emitters[dust]);
    CHECK(wake != nullptr && wake->emitter == compiled.effect->emitters[world].name);
    {
        Document stirred = document;
        add(stirred, layer_named(stirred, "Slide dust"), "stirred", {{"strength", range(9.0F, 9.0F)}});
        const Compiled own = compile(stirred, Purpose::Preview);
        CHECK(own.error.empty());
        std::size_t wakes = 0U;
        for (const ajfx::UpdateModule& module : emitter_of(own, layer_named(stirred, "Slide dust")).update) {
            if (const auto* w = std::get_if<ajfx::Wake>(&module)) {
                ++wakes;
                CHECK(near(w->strength.value * w->scale_max.value, 9.0F));
            }
        }
        CHECK(wakes == 1U);
    }
    Run run = start(compiled);
    std::set<std::uint32_t> seen;
    std::size_t born = 0U;
    std::size_t near_a_rock = 0U;
    float lowest_rock = 10.0F;
    for (std::uint64_t step = 0U; step < 300U; ++step) {
        run.simulation->step();
        const ajfx::EmitterState& rocks = run.simulation->emitter(world);
        const ajfx::EmitterState& puffs = run.simulation->emitter(dust);
        for (const std::size_t i : newborn(puffs, seen)) {
            ++born;
            float nearest = 1.0e9F;
            for (std::size_t r = 0U; r < rocks.alive(); ++r) {
                nearest = std::min(nearest, distance(puffs.position[i], rocks.position[r]));
            }
            near_a_rock += nearest < 0.35F ? 1U : 0U;
        }
        if (step == 299U) {
            for (std::size_t r = 0U; r < rocks.alive(); ++r) {
                lowest_rock = std::min(lowest_rock, rocks.position[r].y);
            }
        }
    }
    std::cout << "  " << born << " puffs as the rocks slid, " << near_a_rock << " born beside a rock; the lowest rock at "
              << lowest_rock << " m\n";
    // The rocks came down the slope to the floor, and dust came of it.
    CHECK(lowest_rock < 0.3F);
    CHECK(born >= 20U);
    CHECK(near_a_rock * 10U >= born * 9U);
}

// A box dropped onto the ground with a trail: shed while it falls, and none
// once it has come to rest.
void a_trail_is_shed_only_while_things_move() {
    Document document;
    document.layers.push_back(one_object(document, "Box", "cube", "block", 0.3F, {0.0F, 2.0F, 0.0F}));
    Layer smoke = make_layer(document, LayerWhat::Smoke, "Smoke");
    smoke.life_min = smoke.life_max = 0.25F;
    add(document, smoke, "trail", {{"per_speed", number(40.0F)}, {"min_speed", number(0.5F)}});
    document.layers.push_back(std::move(smoke));
    Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    CHECK(find_module<ajfx::SpawnFromBodies>(emitter_of(compiled, document.layers[1])) != nullptr);
    const std::size_t box = index_of(compiled, document.layers[0]);
    const std::size_t trail = index_of(compiled, document.layers[1]);
    Run run = start(compiled);
    std::set<std::uint32_t> seen;
    std::size_t falling = 0U;
    std::size_t resting = 0U;
    float speed_at_rest = 0.0F;
    for (std::uint64_t step = 0U; step < 300U; ++step) {
        run.simulation->step();
        const ajfx::EmitterState& body = run.simulation->emitter(box);
        const std::vector<std::size_t> born = newborn(run.simulation->emitter(trail), seen);
        if (step >= 5U && step < 30U) {
            falling += born.size();
            // Behind the box, along the stretch it fell this step.
            for (const std::size_t i : born) {
                CHECK(distance(run.simulation->emitter(trail).position[i], body.position[0]) < 0.6F);
            }
        } else if (step >= 240U) {
            resting += born.size();
            speed_at_rest = std::max(speed_at_rest, std::sqrt(ajfx::dot(body.velocity[0], body.velocity[0])));
        }
    }
    std::cout << "  " << falling << " shed while falling, " << resting << " once at rest\n";
    CHECK(falling >= 20U);
    CHECK(speed_at_rest < 0.05F && resting == 0U);
}

// Crate blast: the pieces the blast breaks the crate into start glowing hot
// and cool steadily, while the crate before it never glowed; embers trail
// them only once they fly, and dust rises where they land.
void blasted_pieces_glow_and_cool_and_trail_embers() {
    Document document = recipe("crate_blast");
    Layer& crate = layer_named(document, "Crate");
    const Block& heat = block_of(crate, "heat");
    CHECK(choice_of(heat, "what") == "pieces");
    Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value() && compiled.effect->blasts.size() == 1U);
    const std::size_t solid = index_of(compiled, crate);
    const auto* glow = find_module<ajfx::Heat>(compiled.effect->emitters[solid]);
    CHECK(glow != nullptr && glow->only.sources == std::vector<std::uint32_t>{ajfx::piece_spawn_source});
    const std::size_t embers = index_of(compiled, layer_named(document, "Embers"));
    const std::size_t dust = index_of(compiled, layer_named(document, "Impact dust"));
    const std::uint64_t tick = ajfx::blast_tick(compiled.effect->blasts[0], compiled.effect->simulation_rate);
    Run run = start(compiled);

    const auto brightness = [](const ajfx::Color& c) { return c.r + c.g + c.b; };
    run.simulation->advance_to(tick);
    CHECK(run.simulation->emitter(solid).alive() == 1U);
    const float whole = brightness(run.simulation->emitter(solid).color[0]);
    CHECK(run.simulation->emitter(embers).alive() == 0U);

    // Mean brightness of the pieces, which are the only things left.
    const auto pieces = [&]() {
        const ajfx::EmitterState& state = run.simulation->emitter(solid);
        float sum = 0.0F;
        std::size_t count = 0U;
        for (std::size_t i = 0U; i < state.alive(); ++i) {
            if (ajfx::spawn_source(state.spawn_index[i]) == ajfx::piece_spawn_source) {
                sum += brightness(state.color[i]);
                ++count;
            }
        }
        CHECK(count > 1U);
        return sum / static_cast<float>(count);
    };
    run.simulation->advance_to(tick + 2U);
    const float hot = pieces();
    std::vector<float> cooling{hot};
    for (std::uint64_t later = 30U; later <= 240U; later += 30U) {
        run.simulation->advance_to(tick + 2U + later);
        cooling.push_back(pieces());
    }
    std::cout << "  the crate's brightness " << whole << "; its pieces " << hot << " after the blast, "
              << cooling.back() << " four seconds on\n";
    CHECK(hot > whole + 2.0F);
    for (std::size_t i = 1U; i < cooling.size(); ++i) {
        CHECK(cooling[i] <= cooling[i - 1U] + 1.0e-4F);
    }
    CHECK(cooling.back() < whole + 1.0F && cooling.back() < hot - 2.0F);

    // Embers were shed behind the flying pieces, and dust rose where they
    // landed.
    std::size_t ever_embers = 0U;
    std::size_t ever_dust = 0U;
    Compiled again = compile(document, Purpose::Preview);
    Run second = start(again);
    for (std::uint64_t step = 0U; step < tick + 150U; ++step) {
        second.simulation->step();
        ever_embers = std::max<std::size_t>(ever_embers, second.simulation->emitter(embers).alive());
        ever_dust = std::max<std::size_t>(ever_dust, second.simulation->emitter(dust).alive());
        if (step + 1U < tick) {
            CHECK(second.simulation->emitter(embers).alive() == 0U);
        }
    }
    std::cout << "  up to " << ever_embers << " embers and " << ever_dust << " puffs of impact dust alive\n";
    CHECK(ever_embers >= 10U && ever_dust >= 5U);
}

} // namespace

void add_solid_world_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"loose_things_in_two_solid_layers_knock_into_each_other",
                     loose_things_in_two_solid_layers_knock_into_each_other});
    cases.push_back({"a_second_solid_layer_leaves_the_first_alone", a_second_solid_layer_leaves_the_first_alone});
    cases.push_back({"puffs_are_born_where_things_hit_and_more_for_harder_hits",
                     puffs_are_born_where_things_hit_and_more_for_harder_hits});
    cases.push_back({"the_rock_slide_kicks_up_dust_where_rocks_hit", the_rock_slide_kicks_up_dust_where_rocks_hit});
    cases.push_back({"a_trail_is_shed_only_while_things_move", a_trail_is_shed_only_while_things_move});
    cases.push_back({"blasted_pieces_glow_and_cool_and_trail_embers", blasted_pieces_glow_and_cool_and_trail_embers});
}
