#include "test_support.hpp"

#include "blast.hpp"

#include <array>
#include <optional>
#include <string_view>
#include <utility>

// Blowing things up: an Explosion is one blast every layer answers, Break
// shatters what is blasted hard enough, materials decide how a thing breaks,
// and "Explode this" makes the whole shot from one object. Each is built into
// an ajfx::Simulation, as the preview does, and measured.
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

// A compiled page with its cuts made, built into a simulation as the preview
// builds it. The solver is held beside it, since the simulation uses it.
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

float apart(ajfx::Vec3 a, ajfx::Vec3 b) {
    const ajfx::Vec3 d = a - b;
    return std::sqrt(ajfx::dot(d, d));
}

// How far the emitter's particles are from `centre` on average, across the
// floor.
float spread_from(const ajfx::EmitterState& state, ajfx::Vec3 centre) {
    if (state.alive() == 0U) {
        return 0.0F;
    }
    float total = 0.0F;
    for (const ajfx::Vec3& p : state.position) {
        total += std::sqrt((p.x - centre.x) * (p.x - centre.x) + (p.z - centre.z) * (p.z - centre.z));
    }
    return total / static_cast<float>(state.alive());
}

float mean_distance(const ajfx::EmitterState& state, ajfx::Vec3 centre) {
    if (state.alive() == 0U) {
        return 0.0F;
    }
    float total = 0.0F;
    for (const ajfx::Vec3& p : state.position) {
        total += apart(p, centre);
    }
    return total / static_cast<float>(state.alive());
}

std::uint64_t tick_of(const Block& explosion) {
    return static_cast<std::uint64_t>(std::ceil(static_cast<double>(number_of(explosion, "time")) * 60.0));
}

// The Explosion block is the page's: one blast, named from its uid, scaled by
// the page, and answered by every layer that plays and does not pass
// through, whichever layer holds it. Moving layers moves nothing.
void an_explosion_is_one_blast_every_layer_answers() {
    Document document;
    Layer solid = make_layer(document, LayerWhat::Solid, "Crates");
    add(document, solid, "object", {{"model", choice("cube")}, {"collides", choice("block")}, {"at", vec3(0.0F, 0.2F, 0.0F)}});
    document.layers.push_back(std::move(solid));
    Layer sparks = make_layer(document, LayerWhat::Sparks, "Sparks");
    add(document, sparks, "stream");
    const Uid boom = add(document, sparks, "explosion",
                         {{"at", vec3(0.5F, 1.0F, -0.25F)},
                          {"power", number(9.0F)},
                          {"radius", number(2.0F)},
                          {"time", number(0.75F)}})
                         .uid;
    document.layers.push_back(std::move(sparks));
    Layer ghosts = make_layer(document, LayerWhat::Smoke, "Ghosts");
    add(document, ghosts, "stream");
    add(document, ghosts, "passes_through");
    document.layers.push_back(std::move(ghosts));
    document.scale = 2.0F;

    const Compiled compiled = compile(document, Purpose::Export);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    CHECK(compiled.effect->blasts.size() == 1U);
    const ajfx::Blast blast = compiled.effect->blasts[0];
    CHECK(blast.name == blast_name(boom));
    CHECK(near(blast.center.x, 1.0F) && near(blast.center.y, 2.0F) && near(blast.center.z, -0.5F));
    CHECK(near(blast.power, 18.0F) && near(blast.radius, 4.0F) && near(blast.time, 0.75F));

    const auto* on_solid = find_module<ajfx::Shockwave>(emitter_of(compiled, document.layers[0]));
    const auto* on_sparks = find_module<ajfx::Shockwave>(emitter_of(compiled, document.layers[1]));
    CHECK(on_solid != nullptr && on_sparks != nullptr);
    CHECK(on_solid->blast == blast.name && on_sparks->blast == blast.name);
    // A solid piece takes the blast as it is; each spark its own share.
    CHECK(on_solid->scale_min.value == 1.0F && on_solid->scale_max.value == 1.0F);
    CHECK(on_sparks->scale_min.value < on_sparks->scale_max.value);
    // Passing through, it is left alone.
    CHECK(find_module<ajfx::Shockwave>(emitter_of(compiled, document.layers[2])) == nullptr);

    // The sparks moved to the top of the page answer it exactly as before.
    Document moved = document;
    CHECK(move_layer(moved, moved.layers[1].uid, 0U));
    const Compiled again = compile(moved, Purpose::Export);
    CHECK(again.error.empty() && again.effect.has_value());
    CHECK(again.effect->blasts == compiled.effect->blasts);
    const auto* moved_sparks = find_module<ajfx::Shockwave>(emitter_of(again, moved.layers[0]));
    CHECK(moved_sparks != nullptr && *moved_sparks == *on_sparks);

    // Switched off, nothing goes off and nothing answers.
    document.block(boom)->enabled = false;
    const Compiled quiet = compile(document, Purpose::Export);
    CHECK(quiet.error.empty() && quiet.effect.has_value() && quiet.effect->blasts.empty());
    for (const ajfx::Emitter& emitter : quiet.effect->emitters) {
        CHECK(find_module<ajfx::Shockwave>(emitter) == nullptr);
    }
}

// The Crate blast recipe is Explode this on one wooden crate: the crate
// stands whole until the blast, then is its cut pieces, flying out from where
// it went off.
void the_crate_blast_blows_the_crate_apart() {
    Document document = recipe("crate_blast");
    CHECK(document.layers.size() == 7U);
    const std::array<LayerWhat, 7> whats{LayerWhat::Solid, LayerWhat::Fire,  LayerWhat::Sparks, LayerWhat::Smoke,
                                         LayerWhat::Dust,  LayerWhat::Dust, LayerWhat::Fire};
    for (std::size_t i = 0U; i < whats.size(); ++i) {
        CHECK(document.layers[i].what == whats[i]);
    }
    Layer& crate = layer_named(document, "Crate");
    const Block& breaking = block_of(crate, "break");
    CHECK(choice_of(breaking, "into") == "cut" && number_of(breaking, "blast") > 0.0F);
    const Block& explosion = block_of(crate, "explosion");

    Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    CHECK(compiled.effect->blasts.size() == 1U);
    const ajfx::Blast blast = compiled.effect->blasts[0];
    CHECK(blast.name == blast_name(explosion.uid));
    // Every layer answers it, and it is strong enough where the crate stands.
    for (const ajfx::Emitter& emitter : compiled.effect->emitters) {
        const auto* wave = find_module<ajfx::Shockwave>(emitter);
        CHECK(wave != nullptr && wave->blast == blast.name);
    }
    CHECK(near(blast.center.y, 0.25F) && blast.power >= number_of(breaking, "blast"));

    const std::size_t solid = index_of(compiled, crate);
    const std::uint64_t tick = ajfx::blast_tick(blast, compiled.effect->simulation_rate);
    CHECK(tick == tick_of(explosion));
    Run run = start(compiled);
    const auto* cut = find_module<ajfx::Break>(compiled.effect->emitters[solid]);
    CHECK(cut != nullptr && cut->blast_threshold > 0.0F);
    const std::size_t cells = cut->pieces.size();
    CHECK(cells == static_cast<std::size_t>(number_of(breaking, "cells")));

    run.simulation->advance_to(tick);
    CHECK(run.simulation->emitter(solid).alive() == 1U);
    run.simulation->advance_to(tick + 1U);
    const ajfx::EmitterState& after = run.simulation->emitter(solid);
    CHECK(after.alive() == cells && after.refused == 0U);
    std::size_t outward = 0U;
    for (std::size_t i = 0U; i < after.alive(); ++i) {
        // Kind 1 is the crate; nothing of it is left whole.
        CHECK(after.body_kind[i] != 1U);
        if (ajfx::dot(after.position[i] - blast.center, after.velocity[i]) > 0.0F) {
            ++outward;
        }
    }
    std::cout << "  the crate became " << after.alive() << " pieces, " << outward << " flying outward\n";
    // A piece pressed into the ground by the blast may have lost its throw
    // already; the rest fly out.
    CHECK(outward * 3U >= cells * 2U);
    const float early = mean_distance(after, blast.center);
    run.simulation->advance_to(tick + 20U);
    const float later = mean_distance(run.simulation->emitter(solid), blast.center);
    std::cout << "  pieces " << early << " m from the blast, a third of a second later " << later << " m\n";
    CHECK(later > early + 0.3F);

    // Once they come down, the pieces are the crate's size and lie on the
    // floor rather than in it: each cell of a 0.5 m crate spans more than
    // 0.15 m, and no corner of one sits below the ground.
    run.simulation->advance_to(tick + 150U);
    const ajfx::EmitterState& settled = run.simulation->emitter(solid);
    const auto* physics = find_module<ajfx::RigidPhysics>(compiled.effect->emitters[solid]);
    CHECK(physics != nullptr && settled.alive() == cells);
    for (std::size_t i = 0U; i < settled.alive(); ++i) {
        const ajfx::RigidBodyKind& kind = physics->bodies[settled.body_kind[i] - 1U];
        const auto mesh = compiled.meshes.find(kind.mesh);
        CHECK(mesh != compiled.meshes.end());
        const float scale = kind.scale * settled.size[i];
        float lowest = 1.0e9F;
        float lo = 1.0e9F;
        float hi = -1.0e9F;
        for (const ajfx::MeshVertex& vertex : mesh->second.vertices) {
            const ajfx::Vec3 at = ajfx::rotate(settled.orientation[i], vertex.position * scale) + settled.position[i];
            lowest = std::min(lowest, at.y);
            lo = std::min(lo, vertex.position.x * scale);
            hi = std::max(hi, vertex.position.x * scale);
        }
        CHECK(hi - lo > 0.15F && hi - lo < 0.75F);
        CHECK(lowest > -0.03F);
    }
}

// The blast throws the smoke and dust out from where it went off: with it,
// they spread across the floor much further than they do from their own
// launch alone.
void the_blast_throws_smoke_and_dust_outward() {
    const auto spread = [](bool blasting, std::string_view which) {
        Document document = recipe("crate_blast");
        Block& explosion = block_of(layer_named(document, "Crate"), "explosion");
        const std::uint64_t tick = tick_of(explosion);
        const Value at = get(explosion, "at");
        explosion.enabled = blasting;
        Compiled compiled = compile(document, Purpose::Preview);
        CHECK(compiled.error.empty() && compiled.effect.has_value());
        const std::size_t index = index_of(compiled, layer_named(document, which));
        Run run = start(compiled);
        run.simulation->advance_to(tick + 15U);
        const ajfx::EmitterState& state = run.simulation->emitter(index);
        CHECK(state.alive() > 0U);
        return spread_from(state, ajfx::Vec3{at.x, at.y, at.z});
    };
    for (const std::string_view which : {std::string_view{"Smoke"}, std::string_view{"Dust"}}) {
        const float blown = spread(true, which);
        const float still = spread(false, which);
        std::cout << "  " << which << " spreads " << blown << " m with the blast, " << still << " m without\n";
        CHECK(blown > still + 0.3F);
    }
}

// A row of two crates and a blast at the first: the near one is thrown hard
// enough to shatter, the far one only hard enough to be pushed away whole.
void a_blast_breaks_what_is_near_and_pushes_what_is_far() {
    const auto row = [](bool blasting) {
        Document document;
        Layer layer = make_layer(document, LayerWhat::Solid, "Crates");
        std::vector<Uid> objects;
        for (const float x : {0.0F, 2.2F}) {
            objects.push_back(add(document, layer, "object",
                                  {{"model", choice("cube")},
                                   {"collides", choice("block")},
                                   {"size", number(0.4F)},
                                   {"extent", vec3(0.4F, 0.4F, 0.4F)},
                                   {"at", vec3(x, 0.2F, 0.0F)},
                                   {"material", choice("wood")}})
                                  .uid);
            add(document, layer, "break", {{"into", choice("cut")}});
        }
        Block& explosion = add(document, layer, "explosion",
                               {{"at", vec3(0.0F, 0.2F, 0.0F)}, {"power", number(10.0F)}, {"radius", number(3.0F)}});
        explosion.enabled = blasting;
        document.layers.push_back(std::move(layer));
        for (const Uid object : objects) {
            CHECK(apply_material(document, object));
        }
        return document;
    };
    Document document = row(true);
    const Layer& layer = document.layers[0];
    // Wood: a blast of 6 m/s shatters it, into 6.
    CHECK(near(number_of(block_of(document.layers[0], "break"), "blast"), 6.0F));
    Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    const std::uint64_t tick = ajfx::blast_tick(compiled.effect->blasts.at(0), 60U);
    Run run = start(compiled);
    const std::size_t index = index_of(compiled, layer);
    run.simulation->advance_to(tick + 2U);
    const ajfx::EmitterState& s = run.simulation->emitter(index);
    // Kind 1 is the near crate, 2 the far one; the cells come after.
    std::size_t near_whole = 0U;
    std::size_t far_whole = 0U;
    float far_x = 0.0F;
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        if (s.body_kind[i] == 1U) {
            ++near_whole;
        } else if (s.body_kind[i] == 2U) {
            ++far_whole;
            far_x = s.position[i].x;
            CHECK(s.velocity[i].x > 0.0F);
        }
    }
    std::cout << "  near crate whole: " << near_whole << ", far crate whole: " << far_whole << ", "
              << s.alive() << " bodies\n";
    CHECK(near_whole == 0U && far_whole == 1U);
    CHECK(s.alive() == 7U);

    // Without the blast, the far crate stands where it was.
    Document calm = row(false);
    Compiled quiet = compile(calm, Purpose::Preview);
    CHECK(quiet.error.empty() && quiet.effect.has_value() && quiet.effect->blasts.empty());
    Run still = start(quiet);
    still.simulation->advance_to(tick + 2U);
    const ajfx::EmitterState& standing = still.simulation->emitter(index_of(quiet, calm.layers[0]));
    CHECK(standing.alive() == 2U);
    float stood_x = 0.0F;
    for (std::size_t i = 0U; i < standing.alive(); ++i) {
        if (standing.body_kind[i] == 2U) {
            stood_x = standing.position[i].x;
        }
    }
    CHECK(far_x > stood_x + 0.01F);
}

// What a thing is made of decides how it breaks: at the same blast, glass
// shatters into many pieces where concrete holds, and at a stronger one
// glass still breaks into more.
void glass_breaks_into_more_pieces_than_concrete() {
    const auto pieces = [](std::string_view material, float power) {
        Document document;
        Layer layer = make_layer(document, LayerWhat::Solid, "Thing");
        const Uid object = add(document, layer, "object",
                               {{"model", choice("cube")},
                                {"collides", choice("block")},
                                {"size", number(0.4F)},
                                {"extent", vec3(0.4F, 0.4F, 0.4F)},
                                {"at", vec3(0.0F, 0.2F, 0.0F)},
                                {"material", choice(std::string{material})}})
                               .uid;
        add(document, layer, "break", {{"into", choice("cut")}});
        add(document, layer, "explosion",
            {{"at", vec3(0.0F, 0.2F, 0.0F)}, {"power", number(power)}, {"radius", number(3.0F)}});
        document.layers.push_back(std::move(layer));
        CHECK(apply_material(document, object));
        Compiled compiled = compile(document, Purpose::Preview);
        CHECK(compiled.error.empty() && compiled.effect.has_value());
        const std::uint64_t tick = ajfx::blast_tick(compiled.effect->blasts.at(0), 60U);
        Run run = start(compiled);
        run.simulation->advance_to(tick + 2U);
        return run.simulation->emitter(index_of(compiled, document.layers[0])).alive();
    };
    // The materials are what the Object and its Break say once one is picked.
    const Material* glass = find_material("glass");
    const Material* concrete = find_material("concrete");
    CHECK(glass != nullptr && concrete != nullptr && find_material("layer") == nullptr);
    CHECK(glass->blasted_at < concrete->blasted_at && glass->pieces > concrete->pieces);

    // 8 m/s: glass shatters, concrete holds.
    const std::size_t glass_weak = pieces("glass", 8.0F);
    const std::size_t concrete_weak = pieces("concrete", 8.0F);
    std::cout << "  at 8 m/s: glass " << glass_weak << " pieces, concrete " << concrete_weak << "\n";
    CHECK(glass_weak == static_cast<std::size_t>(glass->pieces) && concrete_weak == 1U);
    // 16 m/s: both break, glass into more.
    const std::size_t glass_strong = pieces("glass", 16.0F);
    const std::size_t concrete_strong = pieces("concrete", 16.0F);
    std::cout << "  at 16 m/s: glass " << glass_strong << " pieces, concrete " << concrete_strong << "\n";
    CHECK(concrete_strong == static_cast<std::size_t>(concrete->pieces));
    CHECK(glass_strong > concrete_strong);
}

// Picking a material writes its values into the Object and its Break, which
// stay ordinary values; the layer's own takes the Object back to the layer's.
void a_material_sets_values_that_stay_editable() {
    Document document;
    Layer layer = make_layer(document, LayerWhat::Solid, "Thing");
    const Uid object = add(document, layer, "object", {{"material", choice("metal")}}).uid;
    const Uid breaking = add(document, layer, "break").uid;
    document.layers.push_back(std::move(layer));
    CHECK(apply_material(document, object));
    const Material& metal = *find_material("metal");
    const Block& thing = *document.block(object);
    CHECK(toggle_of(thing, "own_material"));
    CHECK(near(number_of(thing, "density"), metal.density) && near(number_of(thing, "bounce"), metal.bounce));
    CHECK(near(number_of(*document.block(breaking), "force"), metal.breaks_at));
    CHECK(near(number_of(*document.block(breaking), "blast"), metal.blasted_at));
    CHECK(near(number_of(*document.block(breaking), "cells"), metal.pieces));
    // Changed by hand afterwards, a value is the artist's.
    document.block(breaking)->set("cells", number(12.0F));
    CHECK(near(number_of(*document.block(breaking), "cells"), 12.0F));
    // The layer's own material again.
    document.block(object)->set("material", choice("layer"));
    CHECK(apply_material(document, object));
    CHECK(!toggle_of(*document.block(object), "own_material"));
    CHECK(!apply_material(document, breaking));
}

// Explode this sizes everything to the thing blown up, makes even something
// standing still into a piece the world moves, and is offered only where
// there is an object to blow up.
void exploding_sizes_to_the_object() {
    const auto exploded = [](float size) {
        Document document;
        Layer layer = make_layer(document, LayerWhat::Solid, "Thing");
        add(document, layer, "object",
            {{"size", number(size)}, {"at", vec3(0.0F, size * 0.5F, 0.0F)}, {"motion", choice("fixed")}});
        document.layers.push_back(std::move(layer));
        const std::optional<Exploded> made = explode_this(document, document.layers[0].uid);
        CHECK(made.has_value() && made->layers.size() == 6U && made->heat != 0U && made->explosion != 0U && made->breaking != 0U);
        return std::pair<Document, Exploded>{std::move(document), *made};
    };
    auto [little, little_made] = exploded(0.5F);
    auto [large, large_made] = exploded(2.0F);
    CHECK(choice_of(*little.block(little_made.object), "motion") == "loose");
    CHECK(near(number_of(*large.block(large_made.explosion), "radius"),
               4.0F * number_of(*little.block(little_made.explosion), "radius")));
    CHECK(near(get(*large.block(large_made.explosion), "at").y, 1.0F));
    // The fireball is born four times as wide.
    const auto fire_size = [](const Document& document, Uid layer) {
        for (const Block& block : document.layer(layer)->blocks) {
            if (block.kind == BlockKind::Shape) {
                return get(block, "size").y;
            }
        }
        return 0.0F;
    };
    CHECK(near(fire_size(large, large_made.layers[0]), 4.0F * fire_size(little, little_made.layers[0])));
    // Every layer it made plays.
    Compiled compiled = compile(large, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    for (const Uid uid : large_made.layers) {
        CHECK(compiled.layer(uid) != nullptr && compiled.layer(uid)->included);
    }
    Run run = start(compiled);
    run.simulation->advance_to(90U);

    // Nothing to blow up in a particle layer, or a solid layer with no object.
    Document empty;
    empty.layers.push_back(make_layer(empty, LayerWhat::Sparks, "Sparks"));
    empty.layers.push_back(make_layer(empty, LayerWhat::Solid, "Nothing"));
    CHECK(explodable_object(empty, empty.layers[0].uid) == 0U && explodable_object(empty, empty.layers[1].uid) == 0U);
    CHECK(!explode_this(empty, empty.layers[1].uid).has_value() && empty.layers.size() == 2U);
}

// A Break set to shatter when blasted asks for an explosion that can, in a
// few words, with the fix.
void the_coach_asks_for_a_blast_that_can_break_it() {
    Document document;
    Layer layer = make_layer(document, LayerWhat::Solid, "Vase");
    add(document, layer, "object",
        {{"model", choice("cube")}, {"collides", choice("block")}, {"at", vec3(0.0F, 0.2F, 0.0F)}});
    const Uid breaking = add(document, layer, "break", {{"into", choice("cut")}, {"blast", number(2.5F)}}).uid;
    document.layers.push_back(std::move(layer));
    const auto note = [&](std::string_view text) -> const Advice* {
        static std::vector<Advice> notes;
        notes = advise(document, compile(document, Purpose::Preview));
        for (const Advice& advice : notes) {
            if (advice.block == breaking && advice.message.find(text) != std::string::npos) {
                return &advice;
            }
        }
        return nullptr;
    };
    const Advice* nothing = note("Nothing on the page explodes.");
    CHECK(nothing != nullptr && nothing->fix);
    nothing->fix(document);
    const Block& explosion = block_of(document.layers[0], "explosion");
    CHECK(near(get(explosion, "at").y, 0.2F));
    CHECK(note("explodes") == nullptr && note("hard enough") == nullptr && note("reaches") == nullptr);

    // Too far away to reach it.
    document.block(explosion.uid)->set("at", vec3(5.0F, 0.2F, 0.0F));
    document.block(explosion.uid)->set("radius", number(1.0F));
    CHECK(note("No explosion reaches it.") != nullptr);
    // Reaching it, too weak: 3 m/s halfway out is 1.5, under 2.5.
    document.block(explosion.uid)->set("at", vec3(1.0F, 0.2F, 0.0F));
    document.block(explosion.uid)->set("radius", number(2.0F));
    document.block(explosion.uid)->set("power", number(3.0F));
    const Advice* weak = note("hard enough");
    CHECK(weak != nullptr && weak->fix);
    weak->fix(document);
    CHECK(number_of(*document.block(breaking), "blast") <= 1.5F);
    CHECK(note("hard enough") == nullptr);
}

// Where it goes off has a handle, and dragging it says where.
void the_explosion_has_a_handle_where_it_goes_off() {
    Document document = recipe("crate_blast");
    Layer& crate = layer_named(document, "Crate");
    const Uid boom = block_of(crate, "explosion").uid;
    const GuideSet set = build_guides(document, GuideOptions{.focus = boom});
    const Handle* at = nullptr;
    const Handle* reach = nullptr;
    for (const Handle& handle : set.handles) {
        if (handle.block == boom && handle.control == "at") {
            at = &handle;
        } else if (handle.block == boom && handle.control == "radius") {
            reach = &handle;
        }
    }
    CHECK(at != nullptr && reach != nullptr && at->motion == HandleMotion::Floor);
    CHECK(near(at->at.y, 0.25F));
    const std::optional<Value> moved = handle_value(document, *at, ajfx::Vec3{1.0F, 0.25F, -1.0F});
    CHECK(moved.has_value() && near(moved->x, 1.0F) && near(moved->z, -1.0F));
    const std::optional<Value> wider = handle_value(document, *reach, at->at + ajfx::Vec3{3.0F, 0.0F, 0.0F});
    CHECK(wider.has_value() && near(wider->x, 3.0F));
    // Worked on or not, where it goes off is drawn on the stage.
    const GuideSet quiet = build_guides(document, GuideOptions{.layer = crate.uid});
    CHECK(std::any_of(quiet.guides.begin(), quiet.guides.end(), [&](const Guide& g) { return g.block == boom; }));
    CHECK(quiet.handles.empty());
}

} // namespace

void add_blast_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"an_explosion_is_one_blast_every_layer_answers", an_explosion_is_one_blast_every_layer_answers});
    cases.push_back({"the_crate_blast_blows_the_crate_apart", the_crate_blast_blows_the_crate_apart});
    cases.push_back({"the_blast_throws_smoke_and_dust_outward", the_blast_throws_smoke_and_dust_outward});
    cases.push_back(
        {"a_blast_breaks_what_is_near_and_pushes_what_is_far", a_blast_breaks_what_is_near_and_pushes_what_is_far});
    cases.push_back({"glass_breaks_into_more_pieces_than_concrete", glass_breaks_into_more_pieces_than_concrete});
    cases.push_back({"a_material_sets_values_that_stay_editable", a_material_sets_values_that_stay_editable});
    cases.push_back({"exploding_sizes_to_the_object", exploding_sizes_to_the_object});
    cases.push_back({"the_coach_asks_for_a_blast_that_can_break_it", the_coach_asks_for_a_blast_that_can_break_it});
    cases.push_back({"the_explosion_has_a_handle_where_it_goes_off", the_explosion_has_a_handle_where_it_goes_off});
}
