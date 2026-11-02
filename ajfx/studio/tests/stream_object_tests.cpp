#include "test_support.hpp"

// An Object standing in a rigid layer leaves the Stream beside it as it was.
namespace {

using namespace studio_test;

// A Stream can only meet an Object inside one rigid layer, since a layer is
// one physics world. Putting the Object in used to rewrite the Stream: the
// compiler switched the whole layer to the slab model (so the pieces' Auto
// shape went from a ball to a slab's hull) and gave every body a born size of a metre,
// ten times the 0.1 m a piece has when nothing sizes it.
void an_object_leaves_the_stream_as_it_was() {
    Document alone;
    Layer pour = make_layer(alone, LayerKind::Rigid, "Pour");
    insert_block(pour, 0U, *make_block(alone, "stream"));
    insert_block(pour, 1U, *make_block(alone, "gravity"));
    insert_block(pour, 2U, *make_block(alone, "ground"));
    alone.layers.push_back(std::move(pour));
    // No Appearance and no Size: the layer's own look and size, unasked.
    CHECK(std::none_of(alone.layers[0].blocks.begin(), alone.layers[0].blocks.end(),
                       [](const Block& b) { return b.kind == BlockKind::Appearance || b.kind == BlockKind::Size; }));

    Document with_crate = alone;
    std::optional<Block> crate = make_block(with_crate, "object");
    CHECK(crate.has_value());
    crate->set("model", choice("cube"));
    crate->set("arrange", choice("one"));
    crate->set("collides", choice("block"));
    crate->set("size", number(0.4F));
    crate->set("at", vec3(0.0F, 0.2F, 0.0F));
    insert_block(with_crate.layers[0], 3U, std::move(*crate));

    const Compiled before = compile(alone, Purpose::Preview);
    const Compiled after = compile(with_crate, Purpose::Preview);
    CHECK(before.error.empty() && before.effect.has_value());
    CHECK(after.error.empty() && after.effect.has_value());
    const ajfx::Emitter& was = before.effect->emitters[0];
    const ajfx::Emitter& now = after.effect->emitters[0];

    // A layer of solid things draws solid things from the start, with or
    // without an Object in it; so the Object is not what switches that on.
    CHECK(was.renderer.geometry == ajfx::Geometry::Mesh);
    CHECK(now.renderer.geometry == was.renderer.geometry);
    CHECK(now.renderer.mesh == was.renderer.mesh);
    CHECK(now.renderer.texture == was.renderer.texture);
    // The Stream is born as it was: the same module, rate and id, and body
    // kind 0, the layer's own. Nothing about the Object re-kinds its pieces.
    const auto* stream_was = find_module<ajfx::SpawnRate>(was);
    const auto* stream_now = find_module<ajfx::SpawnRate>(now);
    CHECK(stream_was != nullptr && stream_now != nullptr);
    CHECK(stream_was->body == 0U && *stream_now == *stream_was);
    const auto* world_was = find_module<ajfx::RigidPhysics>(was);
    const auto* world_now = find_module<ajfx::RigidPhysics>(now);
    CHECK(world_was != nullptr && world_now != nullptr);
    CHECK(world_now->shape == world_was->shape);
    CHECK(near(world_now->friction, world_was->friction) && near(world_now->density, world_was->density));

    // The pieces the Stream makes are the size they were, and the Object is
    // the 0.4 m it was asked to be. A body is born at the layer's size and
    // multiplied by its kind's scale, so the scale is measured against the
    // size the Stream's pieces really have.
    const auto stream_sizes = [](const Compiled& compiled) {
        const std::unique_ptr<ajfx::RigidSolver> solver = ajfx::rigid::make_jolt_solver();
        ajfx::Simulation simulation =
            ajfx::Simulation::create(*compiled.effect, 1U,
                                     ajfx::SimulationOptions{.meshes = compiled.meshes, .rigid = solver.get()})
                .value();
        simulation.advance_to(60U);
        const ajfx::EmitterState& state = simulation.emitter(0U);
        std::vector<float> sizes;
        for (std::size_t i = 0U; i < state.alive(); ++i) {
            if ((i < state.body_kind.size() ? state.body_kind[i] : 0U) == 0U) {
                sizes.push_back(state.size[i]);
            }
        }
        return sizes;
    };
    const std::vector<float> small = stream_sizes(before);
    const std::vector<float> after_sizes = stream_sizes(after);
    CHECK(!small.empty() && !after_sizes.empty());
    for (const float size : small) {
        CHECK(near(size, small.front()));
    }
    for (const float size : after_sizes) {
        CHECK(near(size, small.front()));
    }
    CHECK(world_now->bodies.size() == 1U);
    CHECK(near(world_now->bodies[0].scale * small.front(), 0.4F));
    std::cout << "  a Stream born at " << small.front() << " m kept its look, shape and size beside a 0.4 m crate\n";
}

} // namespace

void add_stream_object_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"an_object_leaves_the_stream_as_it_was", an_object_leaves_the_stream_as_it_was});
}
