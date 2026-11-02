#include "test_support.hpp"

#include "compile_worker.hpp"

// The page's world: one ground, gravity and wind for every layer, what each
// layer is deciding how they act on it, every solid thing met by default, and
// a page whose layers are in any order playing the same.
namespace {

using namespace studio_test;

// The emitter a layer compiled to, by its place in the effect.
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

// Builds the compiled effect into a simulation, as the preview does, and runs
// it to `tick`.
ajfx::SimulationState play(const Compiled& compiled, std::uint64_t tick) {
    const std::unique_ptr<ajfx::RigidSolver> solver = ajfx::rigid::make_jolt_solver();
    ajfx::Result<ajfx::Simulation> created = ajfx::Simulation::create(
        *compiled.effect, 1U, ajfx::SimulationOptions{.meshes = compiled.meshes, .rigid = solver.get()});
    if (!created) {
        throw std::runtime_error{"the simulation was not made: " + created.error().message};
    }
    ajfx::Simulation simulation = std::move(created).value();
    simulation.advance_to(tick);
    ajfx::SimulationState state;
    state.tick = simulation.tick();
    for (std::size_t i = 0U; i < compiled.effect->emitters.size(); ++i) {
        ajfx::EmitterState emitter = simulation.emitter(i);
        // The engine's own bytes are brought up to date only on asking; what
        // is compared is what the particles and bodies are.
        emitter.solver_state.clear();
        state.emitters.push_back(std::move(emitter));
    }
    return state;
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

// A page of everything the world wires together: things that fall and
// knock, a shelf that stands, sparks, dust and smoke, and a wind.
Document busy_page() {
    Document document;
    document.world.wind = 1.5F;
    document.world.wind_heading = 30.0F;
    Layer crates = make_layer(document, LayerWhat::Solid, "Crates");
    add(document, crates, "object",
        {{"model", choice("cube")}, {"collides", choice("block")}, {"arrange", choice("stack")},
         {"count", number(4.0F)}, {"size", number(0.3F)}, {"width", number(0.6F)}, {"depth", number(0.6F)},
         {"jitter", number(0.0F)}});
    add(document, crates, "burst", {{"count", number(10.0F)}, {"time", number(0.5F)}, {"model", choice("rock")}});
    add(document, crates, "shape", {{"shape", choice("point")}, {"offset", vec3(0.0F, 2.0F, 0.0F)}});
    document.layers.push_back(std::move(crates));

    Layer shelf = make_layer(document, LayerWhat::Solid, "Shelf");
    add(document, shelf, "object",
        {{"model", choice("cube")}, {"motion", choice("fixed")}, {"collides", choice("block")},
         {"size", number(1.0F)}, {"extent", vec3(1.0F, 0.6F, 1.0F)}, {"at", vec3(1.5F, 0.3F, 0.0F)}});
    document.layers.push_back(std::move(shelf));

    Layer sparks = make_layer(document, LayerWhat::Sparks, "Sparks");
    sparks.life_min = sparks.life_max = 3.0F;
    add(document, sparks, "burst", {{"count", number(60.0F)}});
    add(document, sparks, "shape",
        {{"shape", choice("ball")}, {"size", range(0.1F, 0.1F)}, {"offset", vec3(0.0F, 1.5F, 0.0F)}});
    add(document, sparks, "launch", {{"direction", choice("out")}, {"speed", range(2.0F, 4.0F)}});
    document.layers.push_back(std::move(sparks));

    Layer dust = make_layer(document, LayerWhat::Dust, "Dust");
    dust.life_min = dust.life_max = 4.0F;
    add(document, dust, "burst", {{"count", number(40.0F)}});
    add(document, dust, "shape",
        {{"shape", choice("disc")}, {"size", range(0.5F, 0.5F)}, {"offset", vec3(0.0F, 1.0F, 0.0F)}});
    document.layers.push_back(std::move(dust));

    Layer smoke = make_layer(document, LayerWhat::Smoke, "Smoke");
    add(document, smoke, "stream", {{"rate", number(30.0F)}});
    add(document, smoke, "rise", {});
    add(document, smoke, "launch", {{"speed", range(0.2F, 0.6F)}});
    document.layers.push_back(std::move(smoke));
    return document;
}

// Every solid is met by every particle layer, the crates and the shelf are
// one world, and the wind is everyone's -- so a page wired together is a page
// where the order of its layers could reach into the simulation. It must not:
// the modules a layer gets from the page, the world it meets, and the kinds,
// spawn stack and standing things of that world, all follow the layers' and
// blocks' uids, not where the layers sit.
void layer_order_does_not_change_the_simulation() {
    Document original = busy_page();
    const Compiled first = compile(original, Purpose::Preview);
    CHECK(first.error.empty() && first.effect.has_value());
    const ajfx::SimulationState played = play(first, 150U);

    for (const std::vector<std::size_t>& order :
         {std::vector<std::size_t>{4U, 3U, 2U, 1U, 0U}, std::vector<std::size_t>{2U, 0U, 4U, 1U, 3U},
          std::vector<std::size_t>{1U, 3U, 0U, 2U, 4U}}) {
        Document moved = original;
        moved.layers.clear();
        for (const std::size_t at : order) {
            moved.layers.push_back(original.layers[at]);
        }
        const Compiled compiled = compile(moved, Purpose::Preview);
        CHECK(compiled.error.empty() && compiled.effect.has_value());
        const ajfx::SimulationState again = play(compiled, 150U);
        for (const Layer& layer : original.layers) {
            // The same emitter, module for module, and the same particles and
            // bodies to the bit.
            CHECK(emitter_of(first, layer) == emitter_of(compiled, layer));
            CHECK(played.emitters[index_of(first, layer)] == again.emitters[index_of(compiled, layer)]);
        }
    }
    // Something happened to compare: the one world holds the crates and the
    // rocks, and the shelf from the other solid layer stands in it.
    const std::size_t world = index_of(first, layer_named(original, "Crates"));
    CHECK(world == index_of(first, layer_named(original, "Shelf")));
    const ajfx::EmitterState& crates = played.emitters[world];
    CHECK(crates.alive() == 14U && crates.props.size() == 1U);
    std::cout << "  five layers in four orders play the same particles and bodies to the bit\n";
}

// Sparks with no Gravity and no Ground of their own fall by the page's
// gravity and bounce off the page's ground.
void sparks_fall_and_bounce_off_the_page_ground() {
    Document document;
    Layer sparks = make_layer(document, LayerWhat::Sparks, "Sparks");
    sparks.life_min = sparks.life_max = 3.0F;
    add(document, sparks, "burst", {{"count", number(50.0F)}});
    add(document, sparks, "shape", {{"shape", choice("point")}, {"offset", vec3(0.0F, 1.0F, 0.0F)}});
    document.layers.push_back(std::move(sparks));
    const Layer& layer = document.layers[0];
    CHECK(std::none_of(layer.blocks.begin(), layer.blocks.end(), [](const Block& b) {
        return b.kind == BlockKind::Gravity || b.kind == BlockKind::Ground;
    }));

    const Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    const ajfx::Emitter& emitter = emitter_of(compiled, layer);
    const auto* gravity = find_module<ajfx::Gravity>(emitter);
    const auto* ground = find_module<ajfx::PlaneCollision>(emitter);
    CHECK(gravity != nullptr && near(gravity->acceleration.value.y, -9.81F));
    CHECK(ground != nullptr && near(ground->restitution.value, 0.35F) && !ground->kill);

    ajfx::Simulation simulation = ajfx::Simulation::create(*compiled.effect, 1U).value();
    float lowest = 1.0F;
    bool bounced = false;
    for (std::uint64_t step = 0U; step < 90U; ++step) {
        simulation.step();
        const ajfx::EmitterState& state = simulation.emitter(0U);
        CHECK(state.alive() == 50U);
        for (std::size_t i = 0U; i < state.alive(); ++i) {
            lowest = std::min(lowest, state.position[i].y);
            // Dropped from rest: rising at all is the bounce.
            bounced = bounced || state.velocity[i].y > 0.5F;
        }
    }
    std::cout << "  sparks fell from 1 m to " << lowest << " m and bounced\n";
    CHECK(lowest < 0.05F && lowest > -0.02F && bounced);

    // Without the page's ground they fall through.
    document.world.ground = false;
    const Compiled through = compile(document, Purpose::Preview);
    CHECK(find_module<ajfx::PlaneCollision>(emitter_of(through, document.layers[0])) == nullptr);
}

// A dust layer and a crate on another layer: nothing wires one to the other,
// and the dust settles on the crate.
void dust_comes_to_rest_on_a_solid() {
    Document document;
    Layer crate = make_layer(document, LayerWhat::Solid, "Crate");
    add(document, crate, "object",
        {{"model", choice("cube")}, {"collides", choice("block")}, {"size", number(0.6F)},
         {"at", vec3(0.0F, 0.3F, 0.0F)}});
    document.layers.push_back(std::move(crate));
    Layer dust = make_layer(document, LayerWhat::Dust, "Dust");
    dust.life_min = dust.life_max = 8.0F;
    add(document, dust, "burst", {{"count", number(40.0F)}});
    add(document, dust, "shape",
        {{"shape", choice("disc")}, {"size", range(0.15F, 0.15F)}, {"offset", vec3(0.0F, 1.0F, 0.0F)}});
    document.layers.push_back(std::move(dust));

    const auto resting_on_top = [](const Compiled& compiled, const Layer& layer) {
        const ajfx::SimulationState state = play(compiled, 300U);
        const ajfx::EmitterState& flakes = state.emitters[index_of(compiled, layer)];
        std::size_t on_top = 0U;
        for (std::size_t i = 0U; i < flakes.alive(); ++i) {
            const float speed = std::sqrt(ajfx::dot(flakes.velocity[i], flakes.velocity[i]));
            if (flakes.position[i].y > 0.5F && speed < 0.05F) {
                ++on_top;
            }
        }
        return on_top;
    };
    const Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    const auto* meets = find_module<ajfx::BodyCollision>(emitter_of(compiled, document.layers[1]));
    CHECK(meets != nullptr && meets->emitter == emitter_of(compiled, document.layers[0]).name);
    // Dust sticks.
    CHECK(near(meets->friction.value, 1.0F) && meets->restitution.value == 0.0F && !meets->kill);
    const std::size_t rested = resting_on_top(compiled, document.layers[1]);
    std::cout << "  " << rested << " of 40 dust specks rest on the crate\n";
    CHECK(rested >= 30U);

    // Passing through, they fall past it to the ground.
    add(document, document.layers[1], "passes_through");
    const Compiled ghost = compile(document, Purpose::Preview);
    CHECK(find_module<ajfx::BodyCollision>(emitter_of(ghost, document.layers[1])) == nullptr);
    CHECK(resting_on_top(ghost, document.layers[1]) == 0U);
}

// The page's solid layers are one world: rocks poured in one layer land on a
// shelf standing in another, which is in that world once and drawn once.
void standing_solids_stand_in_the_one_world() {
    Document document;
    Layer shelf = make_layer(document, LayerWhat::Solid, "Shelf");
    add(document, shelf, "object",
        {{"model", choice("cube")}, {"motion", choice("fixed")}, {"collides", choice("block")},
         {"size", number(1.0F)}, {"extent", vec3(1.4F, 0.6F, 1.4F)}, {"at", vec3(0.0F, 0.3F, 0.0F)}});
    document.layers.push_back(std::move(shelf));
    Layer rocks = make_layer(document, LayerWhat::Solid, "Rocks");
    add(document, rocks, "stream", {{"rate", number(20.0F)}, {"model", choice("rock")}});
    add(document, rocks, "shape", {{"shape", choice("point")}, {"offset", vec3(0.0F, 1.5F, 0.0F)}});
    document.layers.push_back(std::move(rocks));

    const Compiled compiled = compile(document, Purpose::Preview);
    CHECK(compiled.error.empty() && compiled.effect.has_value());
    CHECK(compiled.effect->emitters.size() == 1U);
    CHECK(index_of(compiled, document.layers[0]) == index_of(compiled, document.layers[1]));
    const auto* world = find_module<ajfx::RigidPhysics>(emitter_of(compiled, document.layers[0]));
    CHECK(world != nullptr);
    // The shelf is there once, drawn as its model.
    CHECK(world->props.size() == 1U && world->props[0].mesh == "cube");
    CHECK(world->props[0].shape == ajfx::PropShape::Box);
    // The rocks' layer is two kinds of the world's: its own (what a piece
    // with no model of its own is, drawn as that layer draws, here slabs)
    // and the Stream's rock, which its newborns are and which draws as a
    // rock rather than as whatever the world's first layer draws.
    const LayerReport& poured_report = *compiled.layer(document.layers[1].uid);
    CHECK(poured_report.scope.kinds.size() == 2U);
    for (const std::uint32_t kind : poured_report.scope.kinds) {
        CHECK(kind >= 1U && kind <= world->bodies.size());
    }
    const auto* stream = find_module<ajfx::SpawnRate>(emitter_of(compiled, document.layers[1]));
    CHECK(stream != nullptr && stream->body >= 1U && stream->body <= world->bodies.size());
    CHECK(std::find(poured_report.scope.kinds.begin(), poured_report.scope.kinds.end(), stream->body) !=
          poured_report.scope.kinds.end());
    CHECK(world->bodies[stream->body - 1U].draw == "rock");
    const std::uint32_t own = poured_report.scope.kinds.front();
    CHECK(own != stream->body && world->bodies[own - 1U].draw == "slab");

    const ajfx::SimulationState state = play(compiled, 150U);
    const ajfx::EmitterState& poured = state.emitters[index_of(compiled, document.layers[1])];
    std::size_t up = 0U;
    for (std::size_t i = 0U; i < poured.alive(); ++i) {
        up += poured.position[i].y > 0.45F && poured.position[i].y < 1.2F ? 1U : 0U;
    }
    std::cout << "  " << up << " of " << poured.alive() << " rocks rest on a shelf from another layer\n";
    CHECK(up >= 5U);
}

// A Gravity block is a share of the page's gravity.
void the_page_world_scales_gravity() {
    Document document = recipe("snow");
    document.world.gravity = 0.5F;
    document.world.wind = 2.0F;
    document.world.wind_heading = 90.0F;
    document.world.ground = false;
    CHECK(document.layers[0].what == LayerWhat::Snow);

    // Snow feels a twenty-fifth of it, and the whole of the wind, which blows
    // toward -Z at 90 degrees.
    Compiled compiled = compile(document, Purpose::Export);
    const ajfx::Emitter& flakes = emitter_of(compiled, document.layers[0]);
    float down = 0.0F;
    float across = 0.0F;
    for (const ajfx::UpdateModule& module : flakes.update) {
        if (const auto* g = std::get_if<ajfx::Gravity>(&module)) {
            down += g->acceleration.value.y;
            across += g->acceleration.value.z;
        }
    }
    CHECK(near(down, -9.81F * 0.5F * 0.04F) && near(across, -2.0F, 1.0e-4F));
    CHECK(find_module<ajfx::PlaneCollision>(flakes) == nullptr);

    // A Gravity block stands in for the page's, as a share of it.
    add(document, document.layers[0], "gravity", {{"strength", range(2.0F, 2.0F)}});
    compiled = compile(document, Purpose::Export);
    down = 0.0F;
    for (const ajfx::UpdateModule& module : emitter_of(compiled, document.layers[0]).update) {
        if (const auto* g = std::get_if<ajfx::Gravity>(&module); g != nullptr && g->acceleration.value.y < 0.0F) {
            down += g->acceleration.value.y * g->scale_max.value;
        }
    }
    CHECK(near(down, -9.81F * 0.5F * 2.0F));
}

// Every recipe plays in the page's world: compiled and built into a
// simulation, as the preview does, and run for a second.
void every_recipe_plays_in_the_page_world() {
    for (const Recipe& entry : recipes()) {
        const Document document = recipe(entry.id);
        const Compiled compiled = compile(document, Purpose::Preview);
        CHECK(compiled.error.empty() && compiled.effect.has_value());
        // No recipe carries a Gravity or a Ground of its own: the page's do.
        for (const Layer& layer : document.layers) {
            CHECK(layer.what != LayerWhat::Other);
            for (const Block& block : layer.blocks) {
                CHECK(block.kind != BlockKind::Gravity && block.kind != BlockKind::Ground);
            }
        }
        const ajfx::SimulationState state = play(compiled, 60U);
        CHECK(state.tick == 60U);
    }
    // The ready-made layers, on their own.
    for (const LayerPreset& preset : layer_presets()) {
        Document document;
        std::optional<Layer> layer = make_layer_preset(document, preset.id);
        CHECK(layer.has_value());
        document.layers.push_back(std::move(*layer));
        const Compiled compiled = compile(document, Purpose::Preview);
        CHECK(compiled.error.empty() && compiled.effect.has_value());
        static_cast<void>(play(compiled, 30U));
    }
}

// The compile worker hands out only what is newer than anything it handed
// out or was told of, so a slow compile never replaces a newer page.
void a_compile_off_the_interface_thread_never_goes_back() {
    CompileWorker worker;
    Document document = recipe("campfire");
    for (std::uint64_t revision = 1U; revision <= 5U; ++revision) {
        document.scale = static_cast<float>(revision);
        worker.submit(document, revision);
    }
    worker.wait_idle();
    std::optional<CompileWorker::Result> done = worker.take();
    // Newest wins: whatever was compiled on the way, the page handed out is
    // the last one.
    CHECK(done.has_value() && done->revision == 5U && near(done->document.scale, 5.0F));
    CHECK(done->compiled.effect == compile(document, Purpose::Preview).effect);
    CHECK(near(done->natural_loop, natural_loop_seconds(done->compiled)));
    CHECK(!worker.take().has_value());

    // A compile made elsewhere, of a newer page, fences off anything older.
    worker.fence(10U);
    worker.submit(document, 7U);
    worker.wait_idle();
    CHECK(!worker.take().has_value());
    worker.submit(document, 11U);
    worker.wait_idle();
    done = worker.take();
    CHECK(done.has_value() && done->revision == 11U);
    CHECK(worker.idle());
}

} // namespace

void add_world_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"layer_order_does_not_change_the_simulation", layer_order_does_not_change_the_simulation});
    cases.push_back({"sparks_fall_and_bounce_off_the_page_ground", sparks_fall_and_bounce_off_the_page_ground});
    cases.push_back({"dust_comes_to_rest_on_a_solid", dust_comes_to_rest_on_a_solid});
    cases.push_back({"standing_solids_stand_in_the_one_world", standing_solids_stand_in_the_one_world});
    cases.push_back({"the_page_world_scales_gravity", the_page_world_scales_gravity});
    cases.push_back({"every_recipe_plays_in_the_page_world", every_recipe_plays_in_the_page_world});
    cases.push_back({"a_compile_off_the_interface_thread_never_goes_back",
                     a_compile_off_the_interface_thread_never_goes_back});
}
