#include "test_support.hpp"

// Rigid layers: Break, Part and cutting a model into pieces.
namespace {

using namespace studio_test;

void a_break_uses_the_parts_beside_it() {
    // A crate of its own, broken into four parts an artist placed: a lid,
    // two walls and a base, as the Crates recipe did before it cut instead.
    Document document;
    Layer crates = make_layer(document, LayerKind::Rigid, "Crates");
    insert_block(crates, 0U, *make_block(document, "object"));
    insert_block(crates, 1U, *make_block(document, "gravity"));
    insert_block(crates, 2U, *make_block(document, "appearance"));
    insert_block(crates, 3U, *make_block(document, "break"));
    for (std::size_t i = 0U; i < 4U; ++i) {
        insert_block(crates, crates.blocks.size(), *make_block(document, "part"));
    }
    document.layers.push_back(std::move(crates));
    Layer& layer = document.layers[0];
    block_of(layer, "object").set("count", number(4.0F));
    block_of(layer, "appearance").set("material", choice("model"));
    block_of(layer, "appearance").set("mesh", choice("cube"));
    const ajfx::Vec3 faces[4] = {{0.0F, 0.5F, 0.0F}, {0.5F, 0.0F, 0.0F}, {-0.5F, 0.0F, 0.0F}, {0.0F, -0.5F, 0.0F}};
    std::size_t at = 0U;
    for (Block& block : layer.blocks) {
        if (block.kind == BlockKind::Part) {
            block.set("at", vec3(faces[at].x, faces[at].y, faces[at].z));
            block.set("size", number(0.45F));
            ++at;
        }
    }

    const Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    // No cut was asked for, so the parts are what it breaks into, and they
    // are in the effect from the compiler rather than from the builder.
    CHECK(compiled.cuts.empty());
    const ajfx::Emitter& bodies = compiled.effect->emitters[0];
    const auto* physics = find_module<ajfx::RigidPhysics>(bodies);
    const auto* breaking = find_module<ajfx::Break>(bodies);
    CHECK(physics != nullptr && breaking != nullptr);
    // The Object is kind 1 and each Part a kind after it.
    CHECK(physics->bodies.size() == 5U && breaking->pieces.size() == 4U);
    // The Object the Break is under is kind 1, so that is what breaks.
    CHECK(breaking->body == 1U);
    for (std::size_t i = 0U; i < 4U; ++i) {
        // The Object is kind 1, so its cells are the kinds after it.
        CHECK(breaking->pieces[i].body == static_cast<std::uint32_t>(i + 2U));
        const ajfx::RigidBodyKind& made = physics->bodies[i + 1U];
        CHECK(made.shape == ajfx::RigidShape::Hull);
        CHECK(!made.mesh.empty() && made.draw == made.mesh);
        // A part is made of what the layer's pieces are made of.
        CHECK(near(made.density, number_of(*layer.world, "density")));
    }
    CHECK(near(breaking->pieces[0].offset.y, 0.5F));
    CHECK(near(breaking->pieces[1].offset.x, 0.5F));
    CHECK(ajfx::check_meshes(*compiled.effect, compiled.meshes).ok());
}

// The Crates recipe cuts its crates rather than placing parts, so the pieces
// are the crate: the cut is named when it compiles and made when the models
// are read.
void the_crates_recipe_cuts_its_crates() {
    Document document = recipe("breaking_crates");
    Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    CHECK(compiled.cuts.size() == 1U && compiled.cuts[0].cells == 6U);
    std::string issue;
    make_cuts(compiled, issue);
    CHECK(issue.empty() && compiled.effect.has_value());

    const ajfx::Emitter& crates = emitter_of(compiled, layer_named(document, "Crates and a dropped ball"));
    const auto* physics = find_module<ajfx::RigidPhysics>(crates);
    const auto* breaking = find_module<ajfx::Break>(crates);
    CHECK(physics != nullptr && breaking != nullptr);
    // The crates are kind 1, the ball kind 2, then a kind a cell.
    CHECK(physics->bodies.size() == 8U && breaking->pieces.size() == 6U);
    // Nine crates into six apiece is 54, which the layer has room for.
    CHECK(crates.capacity >= 54U);

    // And it breaks when the ball lands: more bodies than the nine that
    // stood, every one of them a crate or one of its cells.
    const std::unique_ptr<ajfx::RigidSolver> solver = ajfx::rigid::make_jolt_solver();
    std::size_t index = 0U;
    while (compiled.effect->emitters[index].name != crates.name) {
        ++index;
    }
    ajfx::Simulation simulation =
        ajfx::Simulation::create(*compiled.effect, 1U,
                                 ajfx::SimulationOptions{.meshes = compiled.meshes, .rigid = solver.get()})
            .value();
    simulation.advance_to(12U);
    // Nine crates and the ball dropped on them: the ball is one of the
    // layer's bodies now rather than a prop.
    CHECK(simulation.emitter(index).alive() == 10U);
    simulation.advance_to(240U);
    const ajfx::EmitterState& s = simulation.emitter(index);
    std::cout << "  nine cut crates and a ball became " << s.alive() << " bodies, " << s.refused << " refused\n";
    CHECK(s.alive() > 10U);
    CHECK(s.refused == 0U);
    // Kind 1 is a crate, 2 the ball, and 3 to 8 the cells a crate cuts into.
    for (const std::uint32_t kind : s.body_kind) {
        CHECK(kind <= 8U);
    }
}

// A Break block set to cut its model names the cut and places nothing: the
// model is not read until the package is built, so the cells are made there.
void a_break_can_cut_its_own_model_into_pieces() {
    Document document;
    Layer crates = make_layer(document, LayerKind::Rigid, "Crates");
    insert_block(crates, 0U, *make_block(document, "object"));
    insert_block(crates, 1U, *make_block(document, "gravity"));
    insert_block(crates, 2U, *make_block(document, "appearance"));
    insert_block(crates, 3U, *make_block(document, "break"));
    document.layers.push_back(std::move(crates));
    Layer& layer = document.layers[0];
    block_of(layer, "appearance").set("material", choice("model"));
    block_of(layer, "appearance").set("mesh", choice("cube"));
    block_of(layer, "break").set("into", choice("cut"));
    block_of(layer, "break").set("cells", number(6.0F));

    // The compiler names the cut and leaves the layer unbroken, since it has
    // not read the model.
    Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    CHECK(compiled.cuts.size() == 1U);
    CHECK(compiled.cuts[0].cells == 6U && compiled.cuts[0].model == "cube");
    CHECK(compiled.cuts[0].emitter == emitter_of(compiled, layer).name);
    CHECK(find_module<ajfx::Break>(compiled.effect->emitters[0]) == nullptr);

    // Making the cut puts the cells in: a body kind and a mesh each, and the
    // Break that scatters them.
    std::string issue;
    make_cuts(compiled, issue);
    CHECK(issue.empty());
    CHECK(compiled.effect.has_value());
    const ajfx::Emitter& bodies = compiled.effect->emitters[0];
    const auto* physics = find_module<ajfx::RigidPhysics>(bodies);
    const auto* breaking = find_module<ajfx::Break>(bodies);
    CHECK(physics != nullptr && breaking != nullptr);
    // The Object is kind 1, then a kind a cell.
    CHECK(physics->bodies.size() == 7U && breaking->pieces.size() == 6U);
    for (std::size_t i = 0U; i < 6U; ++i) {
        // The Object is kind 1, so its cells are the kinds after it.
        CHECK(breaking->pieces[i].body == static_cast<std::uint32_t>(i + 2U));
        const ajfx::RigidBodyKind& cell = physics->bodies[i + 1U];
        CHECK(cell.shape == ajfx::RigidShape::Hull);
        CHECK(cell.mesh == "cube.cell." + std::to_string(i));
        CHECK(cell.draw == cell.mesh);
        // A cell is placed at its own middle inside the model it was cut
        // from, which is a unit across, and is left at the size it was cut.
        CHECK(near(breaking->pieces[i].size, 1.0F));
        CHECK(std::fabs(breaking->pieces[i].offset.x) < 0.5F);
        CHECK(std::fabs(breaking->pieces[i].offset.y) < 0.5F);
        CHECK(std::fabs(breaking->pieces[i].offset.z) < 0.5F);
    }
    // Every cell's mesh is in the package, and what came out still validates.
    CHECK(ajfx::check_meshes(*compiled.effect, compiled.meshes).ok());
    CHECK(ajfx::validate(*compiled.effect).ok());

    // The same page cut again gives the same cells, as a bake must.
    Compiled again = compile(document, Purpose::Preview);
    std::string second;
    make_cuts(again, second);
    CHECK(second.empty() && again.effect.has_value());
    CHECK(*find_module<ajfx::RigidPhysics>(again.effect->emitters[0]) == *physics);
    CHECK(*find_module<ajfx::Break>(again.effect->emitters[0]) == *breaking);

    // And a different cut seed cuts it differently.
    block_of(document.layers[0], "break").set("cut_seed", number(5.0F));
    Compiled other = compile(document, Purpose::Preview);
    std::string third;
    make_cuts(other, third);
    CHECK(third.empty() && other.effect.has_value());
    CHECK(!(*find_module<ajfx::Break>(other.effect->emitters[0]) == *breaking));
    std::cout << "  six cells cut from the cube, the same twice over and different on another seed\n";
}

// Breaking is two halves, and half of it alone does nothing: the coach says
// which half is missing rather than the compiler guessing at the other.
void half_a_break_is_refused_with_the_missing_half() {
    const auto page = [&](bool with_break, bool with_part) {
        Document document;
        Layer layer = make_layer(document, LayerKind::Rigid, "Crate");
        // Breaking belongs to the Object above it, so the halves are the
        // Break and the Parts under one Object.
        insert_block(layer, 0U, *make_block(document, "object"));
        insert_block(layer, layer.blocks.size(), *make_block(document, "appearance"));
        if (with_break) {
            insert_block(layer, layer.blocks.size(), *make_block(document, "break"));
        }
        if (with_part) {
            insert_block(layer, layer.blocks.size(), *make_block(document, "part"));
        }
        document.layers.push_back(std::move(layer));
        return document;
    };
    // A Break with nothing to break into leaves the pieces whole.
    Document only_break = page(true, false);
    Compiled compiled = compile(only_break, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    CHECK(find_module<ajfx::Break>(compiled.effect->emitters[0]) == nullptr);

    // A Part with nothing that breaks is never made.
    Document only_part = page(false, true);
    compiled = compile(only_part, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    CHECK(find_module<ajfx::Break>(compiled.effect->emitters[0]) == nullptr);

    // Both halves, and the layer breaks.
    Document both = page(true, true);
    compiled = compile(both, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    const auto* breaking = find_module<ajfx::Break>(compiled.effect->emitters[0]);
    CHECK(breaking != nullptr && breaking->pieces.size() == 1U);
    // The Object is kind 1 and the Part kind 2.
    const auto* physics = find_module<ajfx::RigidPhysics>(compiled.effect->emitters[0]);
    CHECK(physics != nullptr && physics->bodies.size() == 2U);
    // A layer of solid things draws them, so the part is drawn as its own
    // model without anyone saying so.
    CHECK(!physics->bodies[1].draw.empty() && physics->bodies[1].draw == physics->bodies[1].mesh);

    // Asked for Glow instead, the layer draws sprites: the part still hits
    // things as its own shape, and is not drawn as its model.
    Document glowing = both;
    for (Block& block : glowing.layers[0].blocks) {
        if (block.kind == BlockKind::Appearance) {
            block.set("material", choice("glow"));
        }
    }
    const Compiled sprites = compile(glowing, Purpose::Preview);
    CHECK(sprites.error.empty() && sprites.effect.has_value());
    const auto* flat = find_module<ajfx::RigidPhysics>(sprites.effect->emitters[0]);
    CHECK(flat != nullptr && flat->bodies[1].draw.empty() && !flat->bodies[1].mesh.empty());
}

// What the Object block is for. The complaint it answers was that fracturing
// one mesh meant reaching for a Pile, and that a second mesh to fracture it
// had to be a Prop, which cannot break at all. Two Objects in one layer, each
// with its own model, its own size and its own Break, is the answer.
void two_objects_break_in_one_world() {
    Document document;
    Layer layer = make_layer(document, LayerKind::Rigid, "Yard");
    insert_block(layer, 0U, *make_block(document, "object"));
    insert_block(layer, 1U, *make_block(document, "break"));
    insert_block(layer, 2U, *make_block(document, "object"));
    insert_block(layer, 3U, *make_block(document, "break"));
    insert_block(layer, 4U, *make_block(document, "part"));
    insert_block(layer, 5U, *make_block(document, "appearance"));
    document.layers.push_back(std::move(layer));
    Layer& yard = document.layers[0];
    yard.blocks[5].set("material", choice("model"));

    // A crate: its own model, 0.4 across, cut into four.
    yard.blocks[0].set("model", choice("cube"));
    yard.blocks[0].set("size", number(0.4F));
    yard.blocks[1].set("into", choice("cut"));
    yard.blocks[1].set("cells", number(4.0F));
    // A boulder beside it: another model, two and a half times the size,
    // coming apart into the Part under it.
    yard.blocks[2].set("model", choice("rock"));
    yard.blocks[2].set("size", number(1.0F));
    yard.blocks[2].set("at", vec3(2.0F, 0.0F, 0.0F));

    Compiled compiled = compile(document, Purpose::Export);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    const ajfx::Emitter& emitter = compiled.effect->emitters[0];
    const auto* physics = find_module<ajfx::RigidPhysics>(emitter);
    CHECK(physics != nullptr);

    // Each Object is a body kind of its own, at a size of its own: that is
    // what one emitter with one size at birth could not do before.
    CHECK(physics->bodies.size() >= 3U);
    CHECK(physics->bodies[0].draw == "cube");
    CHECK(physics->bodies[1].draw == "rock");
    CHECK(physics->bodies[0].scale < physics->bodies[1].scale);
    CHECK(near(physics->bodies[1].scale / physics->bodies[0].scale, 2.5F));

    // Each Object placed its own bodies, naming its own kind.
    std::map<std::uint32_t, std::uint32_t> placed;
    for (const ajfx::SpawnModule& module : emitter.spawn) {
        if (const auto* pile = std::get_if<ajfx::SpawnPile>(&module)) {
            placed[pile->body] = pile->count;
        }
    }
    CHECK(placed.size() == 2U && placed.contains(1U) && placed.contains(2U));

    // The Break under each Object breaks that Object, which is what a block
    // belonging to the thing above it buys. The boulder breaks into its Part
    // at once; the crate names a cut, made once the model is read.
    const auto first_break = std::find_if(emitter.update.begin(), emitter.update.end(),
                                          [](const ajfx::UpdateModule& m) {
                                              return std::holds_alternative<ajfx::Break>(m);
                                          });
    CHECK(first_break != emitter.update.end());
    CHECK(std::get<ajfx::Break>(*first_break).body == 2U);
    CHECK(compiled.cuts.size() == 1U);
    CHECK(compiled.cuts[0].body == 1U && compiled.cuts[0].model == "cube");

    std::string issue;
    make_cuts(compiled, issue);
    CHECK(issue.empty() && compiled.effect.has_value());
    // Two Breaks now, each naming its own kind and neither the other.
    std::vector<std::uint32_t> breaks;
    for (const ajfx::UpdateModule& module : compiled.effect->emitters[0].update) {
        if (const auto* breaking = std::get_if<ajfx::Break>(&module)) {
            breaks.push_back(breaking->body);
        }
    }
    std::sort(breaks.begin(), breaks.end());
    CHECK(breaks.size() == 2U && breaks[0] == 1U && breaks[1] == 2U);
    std::cout << "  a crate cut into four and a boulder two and a half times its size, breaking in one world\n";

    // And the layer is sized for them coming apart, which nothing else would
    // have made room for: the crate into four and the boulder into one part.
    const LayerReport* report = compiled.layer(yard.uid);
    CHECK(report != nullptr && report->capacity >= 5U);
}

} // namespace

void add_rigid_break_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"a_break_uses_the_parts_beside_it", a_break_uses_the_parts_beside_it});
    cases.push_back({"the_crates_recipe_cuts_its_crates", the_crates_recipe_cuts_its_crates});
    cases.push_back({"a_break_can_cut_its_own_model_into_pieces", a_break_can_cut_its_own_model_into_pieces});
    cases.push_back({"half_a_break_is_refused_with_the_missing_half", half_a_break_is_refused_with_the_missing_half});
    cases.push_back({"two_objects_break_in_one_world", two_objects_break_in_one_world});
}
