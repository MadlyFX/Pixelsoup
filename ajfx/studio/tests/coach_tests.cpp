#include "test_support.hpp"

// The coach: what it points at, offers to start with and suggests next.
namespace {

using namespace studio_test;

void the_coach_fixes_what_it_points_at() {
    Document document = recipe("smoke");
    Layer& plume = document.layers[0];
    block_of(plume, "appearance").set("soft", number(0.0F));
    remove_block(document, block_of(plume, "stream").uid);

    std::vector<Advice> notes = advise(document, compile(document, Purpose::Preview));
    const auto find = [&](std::string_view label) -> const Advice* {
        for (const Advice& note : notes) {
            if (note.fix_label == label) {
                return &note;
            }
        }
        return nullptr;
    };
    // With nothing born it says nothing at all: what to start with is the
    // page's question to ask (`starters`), not a warning.
    CHECK(notes.empty());
    CHECK(!has_births(plume) && !starters(plume).empty());
    insert_block(plume, 0U, *make_block(document, "stream"));
    CHECK(plume.blocks.front().kind == BlockKind::Stream && has_births(plume) && starters(plume).empty());

    notes = advise(document, compile(document, Purpose::Preview));
    CHECK(find("Soften edges") != nullptr);
    find("Soften edges")->fix(document);
    notes = advise(document, compile(document, Purpose::Preview));
    CHECK(find("Soften edges") == nullptr);

    // Sparks on a page with no ground fall through the floor. That is the
    // way on rather than a fault, so the layer offers a ground of its own as
    // its next step and the coach does not name it. With the page's ground
    // there is nothing to offer.
    Document sparks = recipe("sparks");
    CHECK(std::none_of(sparks.layers[0].blocks.begin(), sparks.layers[0].blocks.end(),
                       [](const Block& b) { return b.kind == BlockKind::Ground; }));
    const std::vector<Suggestion> held = next_steps(sparks, sparks.layers[0]);
    CHECK(std::none_of(held.begin(), held.end(), [](const Suggestion& step) { return step.title == "Ground"; }));
    sparks.world.ground = false;
    notes = advise(sparks, compile(sparks, Purpose::Preview));
    CHECK(find("Add ground") == nullptr);
    std::vector<Suggestion> steps = next_steps(sparks, sparks.layers[0]);
    const auto is_ground = [](const Suggestion& step) { return step.title == "Ground"; };
    const auto ground = std::find_if(steps.begin(), steps.end(), is_ground);
    CHECK(ground != steps.end());
    CHECK(add_suggestion(sparks, sparks.layers[0].uid, *ground) != 0U);
    steps = next_steps(sparks, sparks.layers[0]);
    CHECK(std::none_of(steps.begin(), steps.end(), is_ground));
}

// A layer with nothing born in it used to get a warning with one button, "Add
// a stream", whatever the layer was for. It now gets the ways it can start:
// what each is in the artist's words, and the blocks that make it play as
// described. Gravity and a ground are the page's, so each is the one block it
// is named for.
void an_empty_layer_offers_what_it_can_start_with() {
    Document document;
    Layer flames = make_layer(document, LayerKind::Particles, "Flames");
    Layer yard = make_layer(document, LayerKind::Rigid, "Yard");
    const auto kinds = [](const Suggestion& starter) {
        std::vector<std::string> out;
        for (const SuggestedBlock& block : starter.blocks) {
            out.push_back(std::string{block_kind_id(block.kind)});
        }
        return out;
    };

    // A particle layer: a flow or a burst, and nothing it does not need.
    const std::vector<Suggestion> flow = starters(flames);
    CHECK(flow.size() == 2U);
    CHECK(flow[0].title == "Stream" && kinds(flow[0]) == std::vector<std::string>({"stream"}));
    CHECK(flow[1].title == "Burst" && kinds(flow[1]) == std::vector<std::string>({"burst"}));

    // A rigid layer: things that fall, and things that stand for them to land on.
    const std::vector<Suggestion> solid = starters(yard);
    CHECK(solid.size() == 4U);
    CHECK(kinds(solid[0]) == std::vector<std::string>({"stream"}));
    CHECK(kinds(solid[1]) == std::vector<std::string>({"burst"}));
    CHECK(kinds(solid[2]) == std::vector<std::string>({"object"}));
    CHECK(kinds(solid[3]) == std::vector<std::string>({"object"}));
    for (const Suggestion& starter : solid) {
        // Every one is a block the layer's kind can hold, with values its
        // controls have, so what it makes is what it says.
        CHECK(!starter.title.empty() && !starter.blocks.empty());
        for (const SuggestedBlock& block : starter.blocks) {
            CHECK(block_fits(block.kind, LayerKind::Rigid));
            const BlockDef* def = find_block_def(block.kind);
            CHECK(def != nullptr);
            for (const auto& [control, value] : block.values) {
                CHECK(find_control(*def, control) != nullptr);
            }
        }
    }
    for (const Suggestion& starter : flow) {
        for (const SuggestedBlock& block : starter.blocks) {
            CHECK(block_fits(block.kind, LayerKind::Particles));
        }
    }
    // Made as they say: a thing that stands is fixed and sits on the ground, a
    // heap is a heap of rocks.
    const SuggestedBlock& standing = solid[2].blocks[0];
    CHECK(standing.values.size() == 2U);
    std::map<std::string, Value> stands(standing.values.begin(), standing.values.end());
    CHECK(stands.at("motion") == choice("fixed") && near(stands.at("at").y, 0.2F));
    std::map<std::string, Value> heap(solid[3].blocks[0].values.begin(), solid[3].blocks[0].values.end());
    CHECK(heap.at("arrange") == choice("heap") && heap.at("model") == choice("rock"));

    // Played, each of the rigid starters is a layer that compiles.
    for (const Suggestion& starter : solid) {
        Document page;
        page.layers.push_back(make_layer(page, LayerKind::Rigid, "Yard"));
        // The first block is the one it opens, and every one is added.
        const Uid first = add_suggestion(page, page.layers[0].uid, starter);
        CHECK(first != 0U && first == page.layers[0].blocks.front().uid);
        CHECK(page.layers[0].blocks.size() == starter.blocks.size());
        CHECK(has_births(page.layers[0]) && starters(page.layers[0]).empty());
        const Compiled compiled = compile(page, Purpose::Preview);
        CHECK(compiled.error.empty() && compiled.effect.has_value());
        // And played: the preview builds a simulation of it, which needs every
        // mesh a body or a prop is shaped by to be in the package. Compiling
        // alone did not say that "One thing standing there" had none.
        const std::unique_ptr<ajfx::RigidSolver> solver = ajfx::rigid::make_jolt_solver();
        const auto simulation = ajfx::Simulation::create(
            *compiled.effect, 1U, ajfx::SimulationOptions{.meshes = compiled.meshes, .rigid = solver.get()});
        CHECK(simulation);
    }

    // Born anywhere counts, a raw spawn module among them; a disabled block
    // or a layer's other blocks do not.
    Layer idle = make_layer(document, LayerKind::Particles, "Idle");
    insert_block(idle, 0U, *make_block(document, "gravity"));
    insert_block(idle, 1U, *make_block(document, "stream"));
    idle.blocks[1].enabled = false;
    CHECK(!has_births(idle) && starters(idle).size() == 2U);
    idle.blocks[1].enabled = true;
    CHECK(has_births(idle) && starters(idle).empty());
    Layer raw = make_layer(document, LayerKind::Particles, "Raw");
    insert_block(raw, 0U, make_raw_block(document, ajfx::SpawnModule{ajfx::SpawnRate{}}));
    CHECK(has_births(raw) && starters(raw).empty());
    std::cout << "  an empty rigid layer offers four ways to start and a particle layer two\n";
}

// Once something is born, what carries a layer on used to arrive as coach
// notes, one amber card each, worded as faults ("Particles vanish suddenly").
// They are the layer's next steps now: what is missing, in the order to add
// it, and the coach keeps what is actually wrong. A rigid layer also says what
// it never did: that pieces moved by the world hang without gravity, and that
// a layer of things that only stand there has nothing falling on them.
void a_layer_suggests_what_comes_next() {
    using Titles = std::vector<std::string>;
    // A page of one layer holding blocks of these kinds, at their defaults.
    const auto page = [](LayerKind kind, const std::vector<std::string>& kinds) {
        Document made;
        Layer layer = make_layer(made, kind, "Layer");
        for (const std::string& block : kinds) {
            insert_block(layer, layer.blocks.size(), *make_block(made, block));
        }
        made.layers.push_back(std::move(layer));
        return made;
    };
    const auto titles = [](const Document& document) {
        return titles_of(next_steps(document, document.layers[0]));
    };

    // A particle layer: a look and a fade are missing; with no gravity there is
    // no floor to fall through.
    Document flow = page(LayerKind::Particles, {"stream"});
    CHECK(titles(flow) == Titles({"Look", "Fade"}));
    // The page's ground holds what falls, so none is offered...
    Document grounded = page(LayerKind::Particles, {"stream", "gravity"});
    CHECK(titles(grounded) == Titles({"Look", "Fade"}));
    // ...until the page has none: then falling brings a ground, first, since
    // falling comes before looking.
    Document falling = grounded;
    falling.world.ground = false;
    CHECK(titles(falling) == Titles({"Ground", "Look", "Fade"}));
    Document pouring = page(LayerKind::Particles, {"stream", "launch"});
    pouring.world.ground = false;
    block_of(pouring.layers[0], "launch").set("direction", choice("down"));
    CHECK(titles(pouring) == Titles({"Ground", "Look", "Fade"}));
    // Passing through the ground too is the same as having none.
    Document ghost = page(LayerKind::Particles, {"stream", "gravity", "passes_through"});
    block_of(ghost.layers[0], "passes_through").set("ground", toggle(true));
    CHECK(titles(ghost) == Titles({"Ground", "Look", "Fade"}));
    // Sparks feel the page's gravity without a block saying so.
    Document sparking = page(LayerKind::Particles, {"stream"});
    sparking.layers[0].what = LayerWhat::Sparks;
    sparking.world.ground = false;
    CHECK(titles(sparking) == Titles({"Ground", "Look", "Fade"}));
    // Nowhere to land in the void, so no ground is wanted there.
    Document hovering = falling;
    hovering.backdrop = Backdrop::Void;
    CHECK(titles(hovering) == Titles({"Look", "Fade"}));
    // A model look asks to tumble; everything there is leaves nothing to say.
    Document solid = page(LayerKind::Particles, {"stream", "appearance", "fade"});
    block_of(solid.layers[0], "appearance").set("material", choice("model"));
    CHECK(titles(solid) == Titles({"Tumble"}));
    Document done = page(LayerKind::Particles, {"burst", "appearance", "fade"});
    block_of(done.layers[0], "appearance").set("material", choice("glow"));
    CHECK(titles(done).empty());
    // Off, or with nothing born, a layer offers none: the second is `starters`.
    Document off = falling;
    off.layers[0].enabled = false;
    CHECK(titles(off).empty());
    CHECK(titles(page(LayerKind::Particles, {})).empty());

    // Solid pieces born in a rigid layer fall by the page's gravity onto its
    // ground. On a page with neither they hang, and want gravity, then a
    // ground; one with a model of its own needs no look to be seen, and the
    // pieces of an Object are not the ones that fade.
    Document falls = page(LayerKind::Rigid, {"stream"});
    CHECK(titles(falls) == Titles({"Look", "Fade"}));
    Document hanging = falls;
    hanging.world.gravity = 0.0F;
    hanging.world.ground = false;
    CHECK(titles(hanging) == Titles({"Gravity", "Look", "Fade"}));
    Document weighty = page(LayerKind::Rigid, {"stream", "gravity"});
    weighty.world.ground = false;
    CHECK(titles(weighty) == Titles({"Ground", "Look", "Fade"}));
    Document rocks = page(LayerKind::Rigid, {"stream"});
    block_of(rocks.layers[0], "stream").set("model", choice("rock"));
    CHECK(titles(rocks) == Titles({"Fade"}));
    Document heap = page(LayerKind::Rigid, {"object"});
    CHECK(titles(heap).empty());
    heap.world.gravity = 0.0F;
    CHECK(titles(heap) == Titles({"Gravity"}));

    // A layer of things that only stand there, or are moved, has nothing
    // falling on them: a Stream, with whatever it needs to fall and land.
    const auto standing = [&](std::string_view motion, const std::vector<std::string>& more) {
        std::vector<std::string> kinds{"object"};
        kinds.insert(kinds.end(), more.begin(), more.end());
        Document made = page(LayerKind::Rigid, kinds);
        block_of(made.layers[0], "object").set("motion", choice(std::string{motion}));
        return made;
    };
    const auto drop_blocks = [&](const Document& document) {
        const std::vector<Suggestion> steps = next_steps(document, document.layers[0]);
        CHECK(steps.size() == 1U && steps[0].title == "Stream");
        Titles kinds;
        for (const SuggestedBlock& block : steps[0].blocks) {
            kinds.push_back(std::string{block_kind_id(block.kind)});
        }
        return kinds;
    };
    // The page gives it gravity and a ground; where it does not, the Stream
    // brings what it lacks.
    CHECK(drop_blocks(standing("fixed", {})) == Titles({"stream"}));
    CHECK(drop_blocks(standing("driven", {})) == Titles({"stream"}));
    Document bare = standing("fixed", {});
    bare.world.gravity = 0.0F;
    bare.world.ground = false;
    CHECK(drop_blocks(bare) == Titles({"stream", "gravity", "ground"}));
    Document bare_ground = standing("fixed", {"ground"});
    bare_ground.world.gravity = 0.0F;
    bare_ground.world.ground = false;
    CHECK(drop_blocks(bare_ground) == Titles({"stream", "gravity"}));
    // Taking it makes a layer that is no longer waiting for something to fall.
    Document waiting = standing("fixed", {});
    CHECK(add_suggestion(waiting, waiting.layers[0].uid, next_steps(waiting, waiting.layers[0])[0]) != 0U);
    CHECK(titles(waiting) == Titles({"Look"}));

    // The coach no longer says what the layer offers, and keeps what is wrong.
    const std::vector<Advice> notes = advise(falling, compile(falling, Purpose::Preview));
    CHECK(!said(notes, "fall through the floor") && !said(notes, "plain soft dots") &&
          !said(notes, "vanish suddenly"));
    Document smoky = page(LayerKind::Particles, {"stream", "appearance", "fade"});
    block_of(smoky.layers[0], "appearance").set("material", choice("smoke"));
    block_of(smoky.layers[0], "appearance").set("soft", number(0.0F));
    CHECK(said(advise(smoky, compile(smoky, Purpose::Preview)), "hard line"));

    // Whatever is offered is a block the layer can hold, with values its
    // controls have; choosing it removes it from the list and leaves a layer
    // that still plays.
    for (const Document* scenario :
         {&flow, &grounded, &falling, &pouring, &ghost, &sparking, &solid, &falls, &hanging, &weighty, &heap,
          &waiting, &bare}) {
        const Document& before = *scenario;
        for (const Suggestion& step : next_steps(before, before.layers[0])) {
            CHECK(!step.title.empty() && !step.blocks.empty());
            for (const SuggestedBlock& made : step.blocks) {
                CHECK(block_fits(made.kind, before.layers[0].kind));
                const BlockDef* def = find_block_def(made.kind);
                CHECK(def != nullptr);
                for (const auto& [control, value] : made.values) {
                    CHECK(find_control(*def, control) != nullptr);
                }
            }
            Document after = before;
            const std::size_t blocks = after.layers[0].blocks.size();
            const Uid first = add_suggestion(after, after.layers[0].uid, step);
            CHECK(first != 0U && after.layers[0].blocks.size() == blocks + step.blocks.size());
            CHECK(after.layers[0].blocks[blocks].uid == first);
            const Titles left = titles(after);
            CHECK(std::find(left.begin(), left.end(), step.title) == left.end());
            const Compiled compiled = compile(after, Purpose::Preview);
            CHECK(compiled.error.empty() && compiled.effect.has_value());
        }
    }
    std::cout << "  a Stream offers a look and a fade, a page without a ground offers one, a lone crate asks for a Stream\n";
}

} // namespace

void add_coach_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"the_coach_fixes_what_it_points_at", the_coach_fixes_what_it_points_at});
    cases.push_back({"an_empty_layer_offers_what_it_can_start_with", an_empty_layer_offers_what_it_can_start_with});
    cases.push_back({"a_layer_suggests_what_comes_next", a_layer_suggests_what_comes_next});
}
