#include "test_support.hpp"

// Rigid layers: Objects, props, how pieces are drawn and what they meet.
namespace {

using namespace studio_test;

// A Prop block becomes an object standing in the layer's physics world, and
// the Bucket of stones recipe plays the whole shot: the balls start inside the
// bucket, it tips over, and they end on the floor.
void props_stand_in_the_world_and_spill_what_they_hold() {
    Document document = recipe("bucket_spill");
    const Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    const ajfx::Emitter& balls = emitter_of(compiled, layer_named(document, "Bucket of stones"));
    const auto* physics = find_module<ajfx::RigidPhysics>(balls);
    CHECK(physics != nullptr && physics->props.size() == 1U);
    const ajfx::RigidProp& bucket = physics->props[0];
    // A bucket is hollow, so it is built of convex pieces rather than wrapped,
    // and it is moved rather than knocked about so the tip-over is authored.
    CHECK(bucket.shape == ajfx::PropShape::HollowCylinder);
    CHECK(bucket.motion == ajfx::PropMotion::Driven);
    CHECK(near(bucket.radius, 0.5F) && near(bucket.height, 0.8F) && near(bucket.wall, 0.05F));
    CHECK(near(bucket.position.y, 0.4F));
    CHECK(near(bucket.to_turn.z, 115.0F));
    // An untouched "moves to" leaves the prop where it stands.
    CHECK(bucket.to_position == bucket.position);

    const std::unique_ptr<ajfx::RigidSolver> solver = ajfx::rigid::make_jolt_solver();
    ajfx::Simulation simulation =
        ajfx::Simulation::create(*compiled.effect, 1U,
                                 ajfx::SimulationOptions{.meshes = compiled.meshes, .rigid = solver.get()})
            .value();
    std::size_t index = 0U;
    while (compiled.effect->emitters[index].name != balls.name) {
        ++index;
    }
    const auto highest = [&] {
        float top = 0.0F;
        for (const ajfx::Vec3 p : simulation.emitter(index).position) {
            top = std::max(top, p.y);
        }
        return top;
    };
    // The prop stands in the world before the first step, and the balls are
    // inside it: above its floor at 0.05, within its 0.45 inside radius.
    CHECK(simulation.emitter(index).props.size() == 1U);
    CHECK(near(simulation.emitter(index).props[0].position.y, 0.4F));
    CHECK(simulation.emitter(index).alive() == 24U);
    for (const ajfx::Vec3 p : simulation.emitter(index).position) {
        CHECK(p.y > 0.05F && std::sqrt(p.x * p.x + p.z * p.z) < 0.45F);
    }
    const float held = highest();
    simulation.advance_to(300U);
    const float spilled = highest();
    std::cout << "  the bucket holds its balls at " << held << " m and spills them to " << spilled << " m\n";
    // Everything ends on the floor, a ball's radius up, not still cradled.
    CHECK(spilled < 0.2F && held > 0.05F);
    CHECK(simulation.emitter(index).alive() == 24U);
}

void snow_lands_on_the_pieces_of_the_layer_above() {
    Document document = recipe("breaking_crates");
    const Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    const Layer& crates = layer_named(document, "Crates and a dropped ball");
    const Layer& snow = layer_named(document, "Snow");
    const ajfx::Emitter& flakes = emitter_of(compiled, snow);
    const auto* lands = find_module<ajfx::BodyCollision>(flakes);
    CHECK(lands != nullptr);
    // It lands on the crates: the rigid layer above it on the page, named by
    // the emitter that layer compiled to.
    CHECK(lands->emitter == emitter_of(compiled, crates).name);
    // Sticking is friction 1 and no bounce, as the ground's Stick is.
    CHECK(near(lands->friction.value, 1.0F) && near(lands->restitution.value, 0.0F));
    CHECK(!lands->kill);
    // And the crates come first in the effect, which is what the runtime
    // needs: the bodies met have already moved this step.
    std::size_t crates_at = 0U;
    std::size_t snow_at = 0U;
    for (std::size_t i = 0U; i < compiled.effect->emitters.size(); ++i) {
        if (compiled.effect->emitters[i].name == lands->emitter) {
            crates_at = i;
        }
        if (compiled.effect->emitters[i].name == flakes.name) {
            snow_at = i;
        }
    }
    CHECK(crates_at < snow_at);

    // The flakes settle on the crates rather than passing through them: with
    // nothing to land on they would all end on the floor.
    const std::unique_ptr<ajfx::RigidSolver> solver = ajfx::rigid::make_jolt_solver();
    ajfx::Simulation simulation =
        ajfx::Simulation::create(*compiled.effect, 1U,
                                 ajfx::SimulationOptions{.meshes = compiled.meshes, .rigid = solver.get()})
            .value();
    // The flakes drift down slowly, so this is ten seconds in: long after
    // the ball has landed and the crates have come apart.
    simulation.advance_to(600U);
    const ajfx::EmitterState& settled = simulation.emitter(snow_at);
    std::size_t aloft = 0U;
    for (std::size_t i = 0U; i < settled.alive(); ++i) {
        if (settled.position[i].y > 0.2F && std::sqrt(ajfx::dot(settled.velocity[i], settled.velocity[i])) < 0.02F) {
            ++aloft;
        }
    }
    std::cout << "  " << aloft << " flakes rest above the floor, on what the crates left\n";
    CHECK(aloft > 5U);
}

// A particle layer meets every solid layer of the page, wherever it sits,
// without anything wiring it there; several particle layers meet the same
// one; and a Passes through block is the way out.
void solid_objects_are_met_wherever_they_are() {
    Document document;
    Layer snow = make_layer(document, LayerWhat::Snow, "Snow");
    insert_block(snow, 0U, *make_block(document, "stream"));
    document.layers.push_back(std::move(snow));
    Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    // Nothing solid, so there is nothing to meet and the layer plays.
    CHECK(find_module<ajfx::BodyCollision>(compiled.effect->emitters[0]) == nullptr);

    // Dust and sparks both meet the crates, and the crates are below them
    // both: every rigid emitter steps before any other, so where the layer
    // sits on the page settles nothing.
    Document shot;
    Layer dust = make_layer(shot, LayerWhat::Dust, "Dust");
    insert_block(dust, 0U, *make_block(shot, "stream"));
    shot.layers.push_back(std::move(dust));
    Layer sparks = make_layer(shot, LayerWhat::Sparks, "Sparks");
    insert_block(sparks, 0U, *make_block(shot, "stream"));
    shot.layers.push_back(std::move(sparks));
    Layer crates = make_layer(shot, LayerWhat::Solid, "Crates");
    insert_block(crates, 0U, *make_block(shot, "object"));
    shot.layers.push_back(std::move(crates));

    compiled = compile(shot, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    // The emitters are still in page order, which is the order they draw in.
    CHECK(compiled.effect->emitters.size() == 3U);
    const std::string solid = compiled.effect->emitters[2].name;
    CHECK(emitter_of(compiled, layer_named(shot, "Crates")).name == solid);
    for (const std::size_t at : {0U, 1U}) {
        const auto* meets = find_module<ajfx::BodyCollision>(compiled.effect->emitters[at]);
        CHECK(meets != nullptr && meets->emitter == solid);
    }
    // Each as what it is: dust sticks, sparks bounce.
    const auto* dust_meets = find_module<ajfx::BodyCollision>(compiled.effect->emitters[0]);
    const auto* spark_meets = find_module<ajfx::BodyCollision>(compiled.effect->emitters[1]);
    CHECK(near(dust_meets->friction.value, 1.0F) && dust_meets->restitution.value == 0.0F);
    CHECK(near(spark_meets->restitution.value, 0.35F) && near(spark_meets->friction.value, 0.25F));
    std::cout << "  dust and sparks both meet the crates listed after them, unasked\n";

    // A second solid layer joins the first in the page's one world, which is
    // met once: both layers' things are its bodies.
    Layer wall = make_layer(shot, LayerWhat::Solid, "Wall");
    insert_block(wall, 0U, *make_block(shot, "object"));
    shot.layers.insert(shot.layers.begin(), std::move(wall));
    compiled = compile(shot, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    const ajfx::Emitter& dusty = emitter_of(compiled, layer_named(shot, "Dust"));
    std::vector<std::string> met;
    for (const ajfx::UpdateModule& module : dusty.update) {
        if (const auto* meets = std::get_if<ajfx::BodyCollision>(&module)) {
            met.push_back(meets->emitter);
        }
    }
    CHECK(met.size() == 1U);
    CHECK(met[0] == emitter_of(compiled, layer_named(shot, "Crates")).name);
    CHECK(met[0] == emitter_of(compiled, layer_named(shot, "Wall")).name);
    CHECK(compiled.effect->emitters.size() == 3U && compiled.solids.layers.size() == 2U);

    // Passes through: no solid things, and the page's ground unless asked.
    Layer& sparks_layer = layer_named(shot, "Sparks");
    const Uid through = insert_block(sparks_layer, 1U, *make_block(shot, "passes_through"));
    compiled = compile(shot, Purpose::Preview);
    const ajfx::Emitter& ghost = emitter_of(compiled, sparks_layer);
    CHECK(find_module<ajfx::BodyCollision>(ghost) == nullptr);
    CHECK(find_module<ajfx::PlaneCollision>(ghost) != nullptr);
    shot.block(through)->set("ground", toggle(true));
    compiled = compile(shot, Purpose::Preview);
    CHECK(find_module<ajfx::PlaneCollision>(emitter_of(compiled, sparks_layer)) == nullptr);
    std::cout << "  and both solid layers once a second one is there; Passes through opts out\n";
}

// A prop needs no Physics block of its own, and what it cannot be is said on
// the block at fault rather than silently approximated.
void a_prop_takes_default_physics_and_names_what_it_cannot_be() {
    Document document;
    Layer layer = make_layer(document, LayerKind::Rigid, "Crate");
    insert_block(layer, 0U, *make_block(document, "object"));
    document.layers.push_back(std::move(layer));
    const Uid prop = document.layers[0].blocks[0].uid;
    // An object the world moves is one of the layer's bodies. One that
    // stands still is a prop, which is what this test is about.
    document.layers[0].blocks[0].set("motion", choice("fixed"));

    // A rigid layer always has a world, whatever is on its list: that is
    // what its kind means, so a prop stands in one without being asked.
    const Compiled bare = compile(document, Purpose::Preview);
    CHECK(bare.error.empty() && bare.effect.has_value());
    const auto* physics = find_module<ajfx::RigidPhysics>(bare.effect->emitters[0]);
    CHECK(physics != nullptr && physics->props.size() == 1U);
    // Drawn as the layer's own pieces it has no model to follow, so it
    // stands in as a plain block and the layer still plays.
    document.layers[0].blocks[0].set("model", choice("layer"));
    const Compiled plain = compile(document, Purpose::Preview);
    CHECK(plain.error.empty() && plain.effect.has_value());
    CHECK(find_module<ajfx::RigidPhysics>(plain.effect->emitters[0])->props[0].shape == ajfx::PropShape::Box);

    document.layers[0].blocks[0].set("model", choice("file"));
    document.layers[0].blocks[0].set("model_file", choice("bucket.glb"));

    // Knocked about, it is one of the layer's own bodies and collides as
    // its model's hull, which is what Object made possible: it needs no
    // prop and no fix.
    document.layers[0].blocks[0].set("motion", choice("loose"));
    const Compiled knocked = compile(document, Purpose::Preview);
    CHECK(knocked.error.empty() && knocked.effect.has_value());
    const auto* as_body = find_module<ajfx::RigidPhysics>(knocked.effect->emitters[0]);
    CHECK(as_body != nullptr && as_body->props.empty() && as_body->bodies.size() == 1U);
    CHECK(as_body->bodies[0].shape == ajfx::RigidShape::Hull);
    // Standing still it is fine, which is what a model-shaped prop is for.
    document.layers[0].blocks[0].set("motion", choice("fixed"));
    const Compiled standing = compile(document, Purpose::Preview);
    CHECK(standing.error.empty());
    CHECK(find_module<ajfx::RigidPhysics>(standing.effect->emitters[0])->props[0].shape ==
          ajfx::PropShape::Mesh);

    // Driven, it cannot collide as its own shape: a collision mesh has no
    // inside for the engine to move. As a hollow crate the pieces can sit
    // inside it.
    document.layers[0].blocks[0].set("motion", choice("driven"));
    document.layers[0].blocks[0].set("collides", choice("crate"));
    const Compiled fixed = compile(document, Purpose::Preview);
    CHECK(fixed.error.empty());
    const auto* mended = find_module<ajfx::RigidPhysics>(fixed.effect->emitters[0]);
    CHECK(mended != nullptr && mended->props[0].shape == ajfx::PropShape::HollowBox);
    CHECK(mended->props[0].motion == ajfx::PropMotion::Driven);
    // The model it names travels in the package, for the host to draw by.
    CHECK(!mended->props[0].mesh.empty());
    CHECK(std::any_of(fixed.requests.begin(), fixed.requests.end(), [&](const AssetRequest& r) {
        return r.block == prop && r.kind == AssetRequest::Kind::Mesh && r.path == "bucket.glb";
    }));
}

// A layer of solid things draws them. Until Object, the model an artist
// picked was drawn only where the layer also held an Appearance set to the
// material that happened to mean "a 3D model", and the model was quietly
// dropped otherwise.
void an_object_draws_its_model_without_being_asked() {
    Document document;
    Layer yard = make_layer(document, LayerKind::Rigid, "Yard");
    insert_block(yard, 0U, *make_block(document, "object"));
    document.layers.push_back(std::move(yard));
    document.layers[0].blocks[0].set("model", choice("rock"));

    // No Appearance block at all.
    CHECK(std::none_of(document.layers[0].blocks.begin(), document.layers[0].blocks.end(),
                       [](const Block& b) { return b.kind == BlockKind::Appearance; }));
    const Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    const ajfx::Emitter& emitter = compiled.effect->emitters[0];
    CHECK(emitter.renderer.geometry == ajfx::Geometry::Mesh);
    const auto* physics = find_module<ajfx::RigidPhysics>(emitter);
    CHECK(physics != nullptr && physics->bodies.size() == 1U);
    CHECK(physics->bodies[0].draw == "rock");
    std::cout << "  an object drawn as its model with no Appearance block on the layer\n";

    // And an Appearance added to a rigid layer starts on Model, so it is an
    // override rather than the thing that switched the models on.
    std::optional<Block> look = make_block(document, "appearance");
    CHECK(look.has_value());
    conform_block(*look, LayerKind::Rigid);
    CHECK(choice_of(*look, "material") == "model");
    // Smoke, Solid and Trail are not offered there at all.
    const BlockDef* def = find_block_def("appearance");
    CHECK(def != nullptr);
    const ControlDef* material = find_control(*def, "material");
    CHECK(material != nullptr);
    std::vector<std::string> offered;
    for (const ChoiceDef& option : choices_in(*material, LayerKind::Rigid)) {
        offered.emplace_back(option.id);
    }
    CHECK(offered == std::vector<std::string>({"glow", "model"}));
    CHECK(choices_in(*material, LayerKind::Particles).size() == 5U);
}

// A layer draws every piece one way. A Stream with no Appearance is slabs,
// not the soft dots a particle layer would be; and an object in a glowing
// layer is a glowing dot until the layer draws models.
void a_rigid_layer_says_how_it_draws_its_pieces() {
    Document pour;
    Layer pieces = make_layer(pour, LayerKind::Rigid, "Pour");
    insert_block(pieces, 0U, *make_block(pour, "stream"));
    pour.layers.push_back(std::move(pieces));
    Compiled compiled = compile(pour, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    // Slabs are drawn until a look is chosen.
    CHECK(compiled.effect->emitters[0].renderer.mesh == "slab");

    Document document;
    Layer layer = make_layer(document, LayerKind::Rigid, "Sparks");
    std::optional<Block> look = make_block(document, "appearance");
    CHECK(look.has_value());
    look->set("material", choice("glow"));
    insert_block(layer, 0U, *make_block(document, "stream"));
    insert_block(layer, 1U, *make_block(document, "object"));
    insert_block(layer, 2U, *make_block(document, "object"));
    insert_block(layer, 3U, std::move(*look));
    document.layers.push_back(std::move(layer));
    Layer& sparks = document.layers[0];
    CHECK(choice_of(block_of(sparks, "appearance"), "material") == "glow");

    compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    CHECK(compiled.effect->emitters[0].renderer.geometry == ajfx::Geometry::Sprites);
    // Drawn as models instead.
    block_of(sparks, "appearance").set("material", choice("model"));
    CHECK(choice_of(block_of(sparks, "appearance"), "material") == "model");
    compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    CHECK(compiled.effect->emitters[0].renderer.geometry == ajfx::Geometry::Mesh);
}

// A Stream or Burst in a rigid layer can be a model of its own, so a stream of
// rocks can fall on a crate in the one world they have to share. Every newborn
// used to be body kind 0, the layer's own, and only an Object could be anything
// else.
void a_stream_can_be_a_model_of_its_own() {
    Document document;
    Layer yard = make_layer(document, LayerKind::Rigid, "Yard");
    std::optional<Block> rain = make_block(document, "stream");
    std::optional<Block> crate = make_block(document, "object");
    CHECK(rain.has_value() && crate.has_value());
    rain->set("rate", number(40.0F));
    rain->set("model", choice("rock"));
    crate->set("model", choice("cube"));
    crate->set("arrange", choice("one"));
    crate->set("collides", choice("block"));
    crate->set("size", number(0.5F));
    crate->set("at", vec3(0.0F, 0.25F, 0.0F));
    CHECK(block_summary(*rain) == "40 a second, Rock");
    const Uid stream_uid = rain->uid;
    insert_block(yard, 0U, std::move(*rain));
    insert_block(yard, 1U, std::move(*crate));
    insert_block(yard, 2U, *make_block(document, "gravity"));
    insert_block(yard, 3U, *make_block(document, "ground"));
    document.layers.push_back(std::move(yard));

    Compiled compiled = compile(document, Purpose::Export);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    const ajfx::Emitter& emitter = compiled.effect->emitters[0];
    const auto* physics = find_module<ajfx::RigidPhysics>(emitter);
    CHECK(physics != nullptr && physics->bodies.size() == 2U);
    // The Stream is first on the page, so its kind is 1 and the crate's is 2.
    // It hits things as the rock it is drawn as, at the size every piece is
    // born at, made of what the layer's Bodies row says.
    CHECK(physics->bodies[0].shape == ajfx::RigidShape::Hull && physics->bodies[0].mesh == "rock");
    CHECK(physics->bodies[0].draw == "rock" && near(physics->bodies[0].scale, 1.0F));
    CHECK(near(physics->bodies[0].friction, physics->friction) && near(physics->bodies[0].density, physics->density));
    CHECK(physics->bodies[1].draw == "cube");
    const auto* flow = find_module<ajfx::SpawnRate>(emitter);
    const auto* pile = find_module<ajfx::SpawnPile>(emitter);
    CHECK(flow != nullptr && flow->body == 1U && near(flow->rate.value, 40.0F));
    CHECK(pile != nullptr && pile->body == 2U);

    // Played, the Stream's newborns are rocks and the one crate is a crate;
    // none is of the layer's own kind.
    {
        const std::unique_ptr<ajfx::RigidSolver> solver = ajfx::rigid::make_jolt_solver();
        ajfx::Simulation simulation =
            ajfx::Simulation::create(*compiled.effect, 1U,
                                     ajfx::SimulationOptions{.meshes = compiled.meshes, .rigid = solver.get()})
                .value();
        simulation.advance_to(60U);
        const ajfx::EmitterState& state = simulation.emitter(0U);
        std::map<std::uint32_t, std::size_t> kinds;
        for (std::size_t i = 0U; i < state.alive(); ++i) {
            ++kinds[i < state.body_kind.size() ? state.body_kind[i] : 0U];
        }
        CHECK(kinds[1U] >= 30U && kinds[2U] == 1U && !kinds.contains(0U));
    }

    // Left as "the layer's own" it is what it always was: kind 0, no body of
    // its own, and the crate is the only kind there is.
    Document plain = document;
    block_of(plain.layers[0], "stream").clear("model");
    compiled = compile(plain, Purpose::Export);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    physics = find_module<ajfx::RigidPhysics>(compiled.effect->emitters[0]);
    CHECK(physics != nullptr && physics->bodies.size() == 1U && physics->bodies[0].draw == "cube");
    CHECK(find_module<ajfx::SpawnRate>(compiled.effect->emitters[0])->body == 0U);
    CHECK(find_module<ajfx::SpawnPile>(compiled.effect->emitters[0])->body == 1U);

    // A model of the artist's own is requested like an Object's, and drawn
    // and hit as that.
    Block& source = block_of(document.layers[0], "stream");
    source.set("model", choice("file"));
    source.set("model_file", choice("C:/art/rubble.glb"));
    CHECK(block_summary(source) == "40 a second, rubble.glb");
    compiled = compile(document, Purpose::Export);
    CHECK(compiled.error.empty() && compiled.requests.size() == 1U);
    CHECK(compiled.requests[0].kind == AssetRequest::Kind::Mesh && compiled.requests[0].block == stream_uid &&
          compiled.requests[0].path == "C:/art/rubble.glb");
    physics = find_module<ajfx::RigidPhysics>(compiled.effect->emitters[0]);
    CHECK(physics->bodies[0].mesh == compiled.requests[0].entry && physics->bodies[0].draw == compiled.requests[0].entry);
    // No file picked yet: a cube until there is one.
    source.set("model_file", choice(""));
    compiled = compile(document, Purpose::Export);
    CHECK(compiled.error.empty() && compiled.requests.empty());
    physics = find_module<ajfx::RigidPhysics>(compiled.effect->emitters[0]);
    CHECK(physics->bodies[0].mesh == "cube");

    // A Burst does the same.
    Document bursting;
    Layer pop = make_layer(bursting, LayerKind::Rigid, "Pop");
    std::optional<Block> shower = make_block(bursting, "burst");
    CHECK(shower.has_value());
    shower->set("count", number(24.0F));
    shower->set("model", choice("shard"));
    CHECK(block_summary(*shower) == "24 at once, Shard");
    insert_block(pop, 0U, std::move(*shower));
    bursting.layers.push_back(std::move(pop));
    compiled = compile(bursting, Purpose::Export);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    physics = find_module<ajfx::RigidPhysics>(compiled.effect->emitters[0]);
    CHECK(physics != nullptr && physics->bodies.size() == 1U && physics->bodies[0].mesh == "shard");
    CHECK(find_module<ajfx::SpawnBurst>(compiled.effect->emitters[0])->body == 1U);

    // A particle layer has no bodies to be a model: the control is not there,
    // and a model stored by a layer that changed kind is cleared.
    const BlockDef* stream_def = find_block_def("stream");
    const ControlDef* model = stream_def != nullptr ? find_control(*stream_def, "model") : nullptr;
    CHECK(model != nullptr && fits(model->layers, LayerKind::Rigid) && !fits(model->layers, LayerKind::Particles));
    std::optional<Block> sparks = make_block(document, "stream");
    CHECK(sparks.has_value());
    sparks->set("model", choice("rock"));
    conform_block(*sparks, LayerKind::Particles);
    CHECK(sparks->find("model") == nullptr && choice_of(*sparks, "model") == "layer");
}

// A model only shows where the layer draws models. A Stream of rocks in a
// glowing layer still hits things as rocks, but is drawn as the glow, and the
// layer drawn as models draws them as rocks.
void a_models_stream_in_a_glowing_layer_is_a_dot() {
    Document document;
    Layer layer = make_layer(document, LayerKind::Rigid, "Sparks");
    std::optional<Block> flow = make_block(document, "stream");
    std::optional<Block> look = make_block(document, "appearance");
    CHECK(flow.has_value() && look.has_value());
    flow->set("model", choice("rock"));
    look->set("material", choice("glow"));
    insert_block(layer, 0U, std::move(*flow));
    insert_block(layer, 1U, std::move(*look));
    document.layers.push_back(std::move(layer));

    Compiled compiled = compile(document, Purpose::Export);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    CHECK(compiled.effect->emitters[0].renderer.geometry == ajfx::Geometry::Sprites);
    const auto* physics = find_module<ajfx::RigidPhysics>(compiled.effect->emitters[0]);
    CHECK(physics != nullptr && physics->bodies.size() == 1U);
    CHECK(physics->bodies[0].mesh == "rock" && physics->bodies[0].draw.empty());

    for (Block& block : document.layers[0].blocks) {
        if (block.kind == BlockKind::Appearance) {
            block.set("material", choice("model"));
        }
    }
    compiled = compile(document, Purpose::Export);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    CHECK(compiled.effect->emitters[0].renderer.geometry == ajfx::Geometry::Mesh);
    CHECK(find_module<ajfx::RigidPhysics>(compiled.effect->emitters[0])->bodies[0].draw == "rock");
}

// A thing that stands there, or is moved, is a prop in the physics world, and
// a prop that collides as its own shape is shaped by its model. The model has
// to be in the package for the world to be built, as a body kind's is; the
// package held the layer's own mesh and its body kinds' but not a prop's, so a
// crate set to stay put failed to play with "the package does not hold" its
// own cube.
void a_standing_object_brings_its_model_to_the_package() {
    for (const char* model : {"cube", "rock", "slab", "shard"}) {
        Document document;
        Layer yard = make_layer(document, LayerKind::Rigid, "Yard");
        std::optional<Block> crate = make_block(document, "object");
        CHECK(crate.has_value());
        crate->set("model", choice(model));
        crate->set("motion", choice("fixed"));
        crate->set("collides", choice("own"));
        insert_block(yard, 0U, std::move(*crate));
        insert_block(yard, 1U, *make_block(document, "ground"));
        document.layers.push_back(std::move(yard));

        const Compiled compiled = compile(document, Purpose::Export);
        CHECK(compiled.error.empty() && compiled.effect.has_value());
        const auto* physics = find_module<ajfx::RigidPhysics>(compiled.effect->emitters[0]);
        CHECK(physics != nullptr && physics->props.size() == 1U);
        CHECK(physics->props[0].shape == ajfx::PropShape::Mesh && physics->props[0].mesh == model);
        CHECK(compiled.meshes.contains(model));
        const std::unique_ptr<ajfx::RigidSolver> solver = ajfx::rigid::make_jolt_solver();
        const auto simulation = ajfx::Simulation::create(
            *compiled.effect, 1U, ajfx::SimulationOptions{.meshes = compiled.meshes, .rigid = solver.get()});
        CHECK(simulation);
    }
    std::cout << "  a crate set to stay put plays, with its model in the package\n";
}

void piles_stand_until_the_effect_plays() {
    Document document = recipe("rock_pile");
    const Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    const ajfx::Emitter& rocks = emitter_of(compiled, layer_named(document, "Rock pile"));
    const auto* pile = find_module<ajfx::SpawnPile>(rocks);
    CHECK(pile != nullptr && pile->count == 220U && pile->release == 0.0F);
    CHECK(rocks.capacity == 256U);
    // Physics turns them; Tumble only sets how each rock lies.
    CHECK(find_module<ajfx::RigidPhysics>(rocks) != nullptr);
    CHECK(find_module<ajfx::RigidBody>(rocks) == nullptr);
    CHECK(find_module<ajfx::InitTumble>(rocks) != nullptr);
    CHECK(near(natural_loop_seconds(compiled), 5.2F));

    // Standing at the start, collapsing as it plays.
    const std::unique_ptr<ajfx::RigidSolver> solver = ajfx::rigid::make_jolt_solver();
    ajfx::Simulation simulation =
        ajfx::Simulation::create(*compiled.effect, 1U,
                                 ajfx::SimulationOptions{.meshes = compiled.meshes, .rigid = solver.get()})
            .value();
    std::size_t index = 0U;
    while (compiled.effect->emitters[index].name != rocks.name) {
        ++index;
    }
    const auto top = [&] {
        float highest = 0.0F;
        for (const ajfx::Vec3 p : simulation.emitter(index).position) {
            highest = std::max(highest, p.y);
        }
        return highest;
    };
    CHECK(simulation.emitter(index).alive() == 220U);
    const float standing = top();
    simulation.advance_to(180U);
    std::cout << "  the rock pile stands " << standing << " m and comes down to " << top() << " m\n";
    CHECK(standing > 0.8F && top() < standing - 0.2F);

    // The world is the layer's own, and its values reach the module. A
    // rigid layer cannot be without one, so there is no "no physics" case.
    Layer& layer = layer_named(document, "Rock pile");
    CHECK(layer.kind == LayerKind::Rigid && layer.world.has_value());
    CHECK(near(find_module<ajfx::RigidPhysics>(rocks)->friction, 0.8F));
    layer.world->set("friction", number(0.25F));
    const Compiled changed = compile(document, Purpose::Export);
    CHECK(changed.error.empty());
    CHECK(near(find_module<ajfx::RigidPhysics>(emitter_of(changed, layer))->friction, 0.25F));
    CHECK(find_module<ajfx::RigidBody>(emitter_of(changed, layer)) == nullptr);
}

// A rigid layer's forces are the world's. Their strength has to reach the
// engine through the force itself, because the engine reads neither a
// force's per-particle scale nor its curve over life.
void a_rigid_layer_force_carries_its_strength() {
    Document document;
    Layer layer = make_layer(document, LayerKind::Rigid, "Rocks");
    insert_block(layer, 0U, *make_block(document, "object"));
    const Uid gravity = insert_block(layer, 1U, *make_block(document, "gravity"));
    const Uid wind = insert_block(layer, 2U, *make_block(document, "wind"));
    const Uid drag = insert_block(layer, 3U, *make_block(document, "drag"));
    document.layers.push_back(std::move(layer));
    document.block(gravity)->set("strength", range(0.5F, 0.5F));
    document.block(wind)->set("strength", range(4.0F, 4.0F));
    document.block(drag)->set("amount", range(3.0F, 3.0F));

    const Compiled compiled = compile(document, Purpose::Export);
    CHECK(compiled.error.empty());
    const ajfx::Emitter& bodies = compiled.effect->emitters[0];
    // Half a g down and 4 m/s^2 sideways, in the accelerations themselves.
    float down = 0.0F;
    float sideways = 0.0F;
    for (const ajfx::UpdateModule& module : bodies.update) {
        if (const auto* g = std::get_if<ajfx::Gravity>(&module)) {
            down += g->acceleration.value.y;
            sideways += g->acceleration.value.x;
            // Nothing is promised per particle that the engine cannot give.
            CHECK(near(g->scale_min.value, 1.0F) && near(g->scale_max.value, 1.0F));
        } else if (const auto* d = std::get_if<ajfx::Drag>(&module)) {
            CHECK(near(d->coefficient.value, 3.0F));
        }
    }
    CHECK(near(down, -9.81F * 0.5F) && near(sideways, 4.0F));

    // The same blocks in a particle layer keep the per-particle spread they
    // have always had: this is the rigid layer's rule, not a new one.
    Document other;
    Layer particles = make_layer(other, LayerKind::Particles, "Sparks");
    insert_block(particles, 0U, *make_block(other, "stream"));
    const Uid pull = insert_block(particles, 1U, *make_block(other, "gravity"));
    other.layers.push_back(std::move(particles));
    other.block(pull)->set("strength", range(0.5F, 2.0F));
    const Compiled loose = compile(other, Purpose::Export);
    CHECK(loose.error.empty());
    const auto* spread = find_module<ajfx::Gravity>(loose.effect->emitters[0]);
    CHECK(spread != nullptr && near(spread->scale_min.value, 0.5F) && near(spread->scale_max.value, 2.0F));
}

} // namespace

void add_rigid_objects_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"props_stand_in_the_world_and_spill_what_they_hold", props_stand_in_the_world_and_spill_what_they_hold});
    cases.push_back({"snow_lands_on_the_pieces_of_the_layer_above", snow_lands_on_the_pieces_of_the_layer_above});
    cases.push_back({"solid_objects_are_met_wherever_they_are", solid_objects_are_met_wherever_they_are});
    cases.push_back({"a_prop_takes_default_physics_and_names_what_it_cannot_be", a_prop_takes_default_physics_and_names_what_it_cannot_be});
    cases.push_back({"an_object_draws_its_model_without_being_asked", an_object_draws_its_model_without_being_asked});
    cases.push_back({"a_rigid_layer_says_how_it_draws_its_pieces", a_rigid_layer_says_how_it_draws_its_pieces});
    cases.push_back({"a_stream_can_be_a_model_of_its_own", a_stream_can_be_a_model_of_its_own});
    cases.push_back({"a_models_stream_in_a_glowing_layer_is_a_dot", a_models_stream_in_a_glowing_layer_is_a_dot});
    cases.push_back({"a_standing_object_brings_its_model_to_the_package", a_standing_object_brings_its_model_to_the_package});
    cases.push_back({"piles_stand_until_the_effect_plays", piles_stand_until_the_effect_plays});
    cases.push_back({"a_rigid_layer_force_carries_its_strength", a_rigid_layer_force_carries_its_strength});
}
