#include "test_support.hpp"

// The block catalog: kinds, which layer a block belongs in, what it acts on.
namespace {

using namespace studio_test;

// The flat model was called "chip", which says nothing about how it looks. It
// is half as tall as a cube and four fifths as deep: a slab.
void the_flat_block_is_called_a_slab() {
    const ajfx::MeshData* slab = library_mesh("slab");
    CHECK(slab != nullptr && library_mesh("chip") == nullptr);
    float low[3] = {1.0e9F, 1.0e9F, 1.0e9F};
    float high[3] = {-1.0e9F, -1.0e9F, -1.0e9F};
    for (const ajfx::MeshVertex& v : slab->vertices) {
        const float at[3] = {v.position.x, v.position.y, v.position.z};
        for (int axis = 0; axis < 3; ++axis) {
            low[axis] = std::min(low[axis], at[axis]);
            high[axis] = std::max(high[axis], at[axis]);
        }
    }
    CHECK(near(high[0] - low[0], 1.0F) && near(high[1] - low[1], 0.5F) && near(high[2] - low[2], 0.8F));

    // Offered by what it looks like, wherever a model is chosen.
    const auto offered = [](std::string_view block, std::string_view control) {
        const BlockDef* def = find_block_def(block);
        const ControlDef* found = def != nullptr ? find_control(*def, control) : nullptr;
        CHECK(found != nullptr);
        std::vector<std::string> labels;
        for (const ChoiceDef& option : found->choices) {
            labels.emplace_back(option.label);
        }
        return std::make_pair(std::string{found->fallback.text}, labels);
    };
    const auto look = offered("appearance", "mesh");
    CHECK(look.first == "slab");
    CHECK(std::find(look.second.begin(), look.second.end(), "Slab") != look.second.end());
    CHECK(std::find(look.second.begin(), look.second.end(), "Chip") == look.second.end());
    for (const std::string_view block : {"object", "stream", "burst", "part"}) {
        const auto labels = offered(block, "model").second;
        CHECK(std::find(labels.begin(), labels.end(), "Chip") == labels.end());
    }

    Document document;
    std::optional<Block> appearance = make_block(document, "appearance");
    CHECK(appearance.has_value());
    appearance->set("material", choice("model"));
    appearance->set("mesh", choice("rock"));
    // The summary names the material as the page does, not as it once was.
    CHECK(block_summary(*appearance) == "Model, Rock");
}

// A kind is named once, in block_kind.hpp, and everything keyed by it reads
// that name: the page stores the id, the catalog holds one definition per
// kind, and a kind the table does not know is not a block.
void every_block_kind_has_one_id_and_one_definition() {
    Document document;
    std::set<std::string_view> seen;
    for (std::size_t i = 0U; i < block_kind_count; ++i) {
        const BlockKind kind = static_cast<BlockKind>(i);
        const std::string_view id = block_kind_id(kind);
        CHECK(!id.empty() && seen.insert(id).second);
        CHECK(block_kind_from(id) == kind);
        const BlockDef* def = find_block_def(kind);
        CHECK(def != nullptr && def->kind == kind);
        CHECK(find_block_def(id) == def);
        // A saved page spells the kind as its id, and reads it back.
        CHECK(id == kind);
    }
    CHECK(block_catalog().size() == block_kind_count);
    CHECK(!block_kind_from("pile").has_value() && !block_kind_from("prop").has_value());
    CHECK(!block_kind_from("").has_value() && !block_kind_from("Stream").has_value());
    CHECK(find_block_def("stream ") == nullptr && !make_block(document, "strem").has_value());
    // The Module block is the only kind make_block refuses: it holds a raw
    // module that make_raw_block puts in.
    for (std::size_t i = 0U; i < block_kind_count; ++i) {
        const BlockKind kind = static_cast<BlockKind>(i);
        const std::optional<Block> made = make_block(document, kind);
        CHECK(made.has_value() == (kind != BlockKind::Module));
        CHECK(!made || made->kind == kind);
    }
}

// A layer's kind is what decides which blocks it can hold, so the catalog's
// answer, what the page offers, what a drop accepts and what the compiler
// will build must be one answer. Checked against the runtime's own refusal
// list: every block the catalog keeps out of a rigid layer is one
// ajfx::validate would refuse there, and every block it lets in compiles.
void a_layer_kind_decides_which_blocks_it_can_hold() {
    // Every block in the catalog belongs to at least one kind, except the
    // world, which is the layer's own settings.
    std::size_t offered_in_rigid = 0U;
    for (const BlockDef& def : block_catalog()) {
        if (def.kind == BlockKind::Physics) {
            CHECK(def.layers == in_no_layer && !def.detachable);
            continue;
        }
        CHECK(def.layers != in_no_layer);
        // A block kept out of a kind says why, for the artist who looks.
        CHECK(def.layers == in_any_layer || !def.only_in.empty());
        offered_in_rigid += fits(def.layers, LayerKind::Rigid) ? 1U : 0U;
    }
    CHECK(offered_in_rigid > 0U);

    // The forces a rigid layer refuses are kept out of it, and the ones it
    // takes are let in.
    for (const char* kept_out : {"turbulence", "swirl", "attract", "draw_in", "spin", "mesh_collider"}) {
        CHECK(block_fits(kept_out, LayerKind::Particles));
        CHECK(!block_fits(kept_out, LayerKind::Rigid));
    }
    for (const char* rigid_only : {"object"}) {
        CHECK(block_fits(rigid_only, LayerKind::Rigid));
        CHECK(!block_fits(rigid_only, LayerKind::Particles));
    }
    for (const char* both : {"stream", "burst", "gravity", "wind", "drag", "ground", "tumble", "color", "light"}) {
        CHECK(block_fits(both, LayerKind::Particles) && block_fits(both, LayerKind::Rigid));
    }

    // Every block a rigid layer offers really does compile there: the
    // catalog's list and the runtime's refusals agree, which is the whole
    // point of filtering rather than explaining afterwards.
    for (const BlockDef& def : block_catalog()) {
        if (def.kind == BlockKind::Module || def.kind == BlockKind::Physics || !fits(def.layers, LayerKind::Rigid)) {
            continue;
        }
        Document document;
        Layer layer = make_layer(document, LayerKind::Rigid, "Bodies");
        insert_block(layer, 0U, *make_block(document, "object"));
        insert_block(layer, 1U, *make_block(document, "gravity"));
        if (def.kind != BlockKind::Object && def.kind != BlockKind::Gravity) {
            insert_block(layer, 2U, *make_block(document, def.kind));
        }
        document.layers.push_back(std::move(layer));
        const Compiled compiled = compile(document, Purpose::Export);
        if (!compiled.error.empty()) {
            throw std::runtime_error{std::string{def.title} + " is offered in a rigid layer but will not compile: " +
                                     compiled.error};
        }
    }

    // A block the kind does not have cannot be moved into such a layer, and
    // adding one by hand is reported rather than quietly ignored.
    Document document;
    Layer rigid = make_layer(document, LayerKind::Rigid, "Rocks");
    insert_block(rigid, 0U, *make_block(document, "object"));
    document.layers.push_back(std::move(rigid));
    Layer sparks = make_layer(document, LayerKind::Particles, "Sparks");
    insert_block(sparks, 0U, *make_block(document, "stream"));
    const Uid swirl = insert_block(sparks, 1U, *make_block(document, "swirl"));
    document.layers.push_back(std::move(sparks));
    CHECK(!move_block(document, swirl, document.layers[0].uid, 0U));
    CHECK(document.find_block(swirl)->layer->uid == document.layers[1].uid);
    document.layers[0].blocks.push_back(*make_block(document, "swirl"));
    const Compiled smuggled = compile(document, Purpose::Export);
    CHECK(smuggled.error.find("Swirl does not belong") != std::string::npos);
}

// A rigid layer holds blocks for four different things at once: its own
// pieces, what they come apart into, the props standing beside them and the
// world they all share. The page reads them under headings that say which,
// and the grouping is the catalog's answer, so it is checked here rather
// than in the drawing.
void a_rigid_block_says_what_it_acts_on() {
    // What the compiler makes of each in a rigid layer: a force is the
    // world's one gravity or a body's damping and a collider an unmoving
    // body in it, where spawning, placing, tumbling and every look belong
    // to the pieces themselves.
    CHECK(rigid_scope("object") == RigidScope::Objects);
    CHECK(rigid_scope("stream") == RigidScope::Pieces);
    CHECK(rigid_scope("burst") == RigidScope::Pieces);
    CHECK(rigid_scope("shape") == RigidScope::Pieces);
    CHECK(rigid_scope("launch") == RigidScope::Pieces);
    CHECK(rigid_scope("tumble") == RigidScope::Pieces);
    CHECK(rigid_scope("appearance") == RigidScope::Pieces);
    CHECK(rigid_scope("color") == RigidScope::Pieces);
    CHECK(rigid_scope("size") == RigidScope::Pieces);
    CHECK(rigid_scope("fade") == RigidScope::Pieces);
    CHECK(rigid_scope("light") == RigidScope::Pieces);
    CHECK(rigid_scope("break") == RigidScope::Breaking);
    CHECK(rigid_scope("part") == RigidScope::Breaking);
    CHECK(rigid_scope("part") == RigidScope::Breaking);
    CHECK(rigid_scope("gravity") == RigidScope::World);
    CHECK(rigid_scope("rise") == RigidScope::World);
    CHECK(rigid_scope("wind") == RigidScope::World);
    CHECK(rigid_scope("drag") == RigidScope::World);
    CHECK(rigid_scope("ground") == RigidScope::World);
    CHECK(rigid_scope("sphere") == RigidScope::World);
    CHECK(rigid_scope("box") == RigidScope::World);
    // What every piece is made of is the world's, not one block's.
    CHECK(rigid_scope(BlockKind::Physics) == RigidScope::World);
    CHECK(rigid_scope(BlockKind::Module) == RigidScope::Hood);

    // Every scope is read once, so no heading is drawn twice and none is
    // left without words. The gui font atlas is ASCII only.
    std::set<RigidScope> seen;
    for (const RigidScope scope : rigid_scopes) {
        CHECK(seen.insert(scope).second);
        CHECK(!rigid_scope_name(scope).empty());
        CHECK(!rigid_scope_help(scope).empty());
        for (const char c : rigid_scope_name(scope)) {
            CHECK(static_cast<unsigned char>(c) < 128U);
        }
        for (const char c : rigid_scope_help(scope)) {
            CHECK(static_cast<unsigned char>(c) < 128U);
        }
    }
    CHECK(seen.size() == rigid_scopes.size());
    // And every block a rigid layer can hold has a heading to read under.
    for (const BlockDef& def : block_catalog()) {
        if (block_fits(def.kind, LayerKind::Rigid)) {
            CHECK(seen.contains(rigid_scope(def.kind)));
        }
    }

    // The worked shot falls where the shot says it does: nine crates, the
    // ball dropped on them, the cut that breaks them, and the world.
    Document document = recipe("breaking_crates");
    const Layer& crates = layer_named(document, "Crates and a dropped ball");
    std::map<RigidScope, int> counted;
    for (const Block& block : crates.blocks) {
        ++counted[rigid_scope(block.kind)];
    }
    CHECK(counted[RigidScope::Objects] == 2);  // the crates and the ball
    CHECK(counted[RigidScope::Breaking] == 1); // the cut, under the crates
    CHECK(counted[RigidScope::Pieces] == 3);   // how the layer's pieces look
    // Gravity and the ground are the page's, so the world holds no block.
    CHECK(counted[RigidScope::World] == 0);
    CHECK(counted[RigidScope::Hood] == 0);

    // The cut sits under the crates and not under the ball, so it is the
    // crates that come apart: reading down the page is what says so.
    const auto at = [&](const char* kind) {
        for (std::size_t i = 0U; i < crates.blocks.size(); ++i) {
            if (crates.blocks[i].kind == kind) {
                return i;
            }
        }
        return crates.blocks.size();
    };
    const std::optional<std::size_t> owner = object_of(crates, at("break"));
    CHECK(owner && *owner == at("object"));
}

// Changing a layer's kind takes its blocks with it, and refuses rather than
// throwing away anything the artist authored.
void changing_a_layer_kind_keeps_what_fits_and_refuses_the_rest() {
    Document document;
    Layer layer = make_layer(document, LayerKind::Particles, "Embers");
    insert_block(layer, 0U, *make_block(document, "stream"));
    insert_block(layer, 1U, *make_block(document, "gravity"));
    const Uid sized = insert_block(layer, 2U, *make_block(document, "size"));
    document.layers.push_back(std::move(layer));
    Layer& work = document.layers[0];
    work.blocks[2].set("over_life", choice("shrink"));

    // Nothing is in the way, so it changes: it gains a world, and the
    // settings a rigid layer cannot act on are cleared rather than kept
    // where no one can see them.
    CHECK(blocks_against_kind(work, LayerKind::Rigid).empty());
    CHECK(set_layer_kind(document, work, LayerKind::Rigid));
    CHECK(work.kind == LayerKind::Rigid && work.world && work.world->uid != 0U);
    CHECK(document.block(work.world->uid) == &*work.world);
    CHECK(document.block(sized)->find("over_life") == nullptr);
    CHECK(compile(document, Purpose::Export).error.empty());

    // A block the other kind does not have stops the change and is named,
    // so nothing an artist made is dropped behind their back.
    insert_block(work, work.blocks.size(), *make_block(document, "object"));
    const std::vector<std::string> blocking = blocks_against_kind(work, LayerKind::Particles);
    CHECK(blocking.size() == 1U && blocking[0] == "Object");
    CHECK(!set_layer_kind(document, work, LayerKind::Particles));
    CHECK(work.kind == LayerKind::Rigid);

    // Going back drops the world, since a particle layer has none.
    remove_block(document, work.blocks.back().uid);
    CHECK(set_layer_kind(document, work, LayerKind::Particles));
    CHECK(!work.world.has_value());
}

// Everything the palette and the pickers offer must be something that can
// actually be made. An id neither side recognised once produced a layer with
// no uid, which looked like a layer but shared every widget id with every
// other such layer, so its menu never opened.
void every_layer_the_palette_offers_can_be_made() {
    Document document;
    const auto made = [&](const std::string& id) {
        if (const std::optional<LayerWhat> empty = empty_layer_what(id)) {
            return std::optional<Layer>{make_layer(document, *empty, "Layer")};
        }
        return make_layer_preset(document, id);
    };
    std::vector<std::string> ids;
    for (const LayerWhat what : layer_whats) {
        ids.push_back(empty_layer_id(what));
    }
    for (const LayerPreset& preset : layer_presets()) {
        ids.emplace_back(preset.id);
    }
    std::vector<Uid> seen;
    for (const std::string& id : ids) {
        const std::optional<Layer> layer = made(id);
        if (!layer) {
            throw std::runtime_error{"the palette offers '" + id + "', which cannot be made"};
        }
        // A uid of its own: without one every widget on it collides with
        // every other layer's, and the layer menu never opens.
        CHECK(layer->uid != 0U);
        CHECK(std::find(seen.begin(), seen.end(), layer->uid) == seen.end());
        seen.push_back(layer->uid);
        if (layer->kind == LayerKind::Rigid) {
            CHECK(layer->world && layer->world->uid != 0U && layer->world->uid != layer->uid);
        } else {
            CHECK(!layer->world.has_value());
        }
        for (const Block& block : layer->blocks) {
            CHECK(block.uid != 0U);
            CHECK(block.kind == BlockKind::Module || block_fits(block.kind, layer->kind));
        }
    }
    // An empty layer's id round-trips, and nothing else is read as one.
    // An empty layer's id round-trips, it is made as what it says with the
    // kind that goes with it, and nothing else is read as one.
    for (const LayerWhat what : layer_whats) {
        CHECK(empty_layer_what(empty_layer_id(what)) == what);
        const Layer made_as = make_layer(document, what, "Layer");
        CHECK(made_as.what == what && made_as.kind == layer_what_kind(what));
        CHECK(made_as.world.has_value() == (what == LayerWhat::Solid));
    }
    CHECK(empty_layer_what("") == LayerWhat::Other);
    CHECK(!empty_layer_what("rock_pile").has_value());
    CHECK(!empty_layer_what("empty:volumes").has_value());
    // Every ready-made layer says what it is, and is of the kind that is.
    for (const LayerPreset& preset : layer_presets()) {
        const std::optional<Layer> layer = make_layer_preset(document, preset.id);
        CHECK(layer && layer->what == preset.what && layer->kind == preset.kind &&
              layer_what_kind(preset.what) == preset.kind);
    }
}

} // namespace

void add_catalog_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"the_flat_block_is_called_a_slab", the_flat_block_is_called_a_slab});
    cases.push_back({"every_block_kind_has_one_id_and_one_definition", every_block_kind_has_one_id_and_one_definition});
    cases.push_back({"a_layer_kind_decides_which_blocks_it_can_hold", a_layer_kind_decides_which_blocks_it_can_hold});
    cases.push_back({"a_rigid_block_says_what_it_acts_on", a_rigid_block_says_what_it_acts_on});
    cases.push_back({"changing_a_layer_kind_keeps_what_fits_and_refuses_the_rest", changing_a_layer_kind_keeps_what_fits_and_refuses_the_rest});
    cases.push_back({"every_layer_the_palette_offers_can_be_made", every_layer_the_palette_offers_can_be_made});
}
