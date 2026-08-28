#include <mediaserver/render/compositor_scene_executor.hpp>

#include <mediaserver/compositor/compositor_model.hpp>
#include <mediaserver/compositor/compositor_schema.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace compositor = mediaserver::compositor;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace render = mediaserver::render;
namespace scene = mediaserver::scene;
namespace schema = mediaserver::compositor::schema;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define PLAN_CHECK(...)                                                        \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

using Property = std::pair<std::string, doc::PropertyValue>;

[[nodiscard]] Property whole(std::string_view name, std::int64_t value) {
    return {std::string{name}, doc::PropertyValue{value}};
}

[[nodiscard]] Property real(std::string_view name, double value) {
    return {std::string{name}, doc::PropertyValue{value}};
}

[[nodiscard]] doc::Node node(core::u64 id, std::string_view type,
                             std::initializer_list<Property> properties = {}) {
    doc::Node result{};
    result.id = doc::NodeId{id};
    result.type = std::string{type};
    for (const auto& property : properties) {
        result.properties.emplace(property.first, property.second);
    }
    return result;
}

[[nodiscard]] doc::Node layer(core::u64 id, core::u64 scene_node) {
    return node(
        id, schema::scene_layer_type,
        {whole(schema::scene_node, static_cast<std::int64_t>(scene_node))});
}

[[nodiscard]] doc::Node edge(core::u64 id, core::u64 from, core::u8 from_port,
                             core::u64 to, core::u8 to_port,
                             core::i64 order = 0) {
    return node(id, schema::edge_type,
                {whole(schema::from_node, static_cast<std::int64_t>(from)),
                 whole(schema::from_port, from_port),
                 whole(schema::to_node, static_cast<std::int64_t>(to)),
                 whole(schema::to_port, to_port), whole(schema::order, order)});
}

[[nodiscard]] compositor::CompositorGraph
graph_of(const std::vector<doc::Node>& nodes) {
    doc::Root root;
    for (const doc::Node& value : nodes) {
        root = root.set(value);
    }
    compositor::CompositorModel model;
    model.reset(root);
    return model.evaluate();
}

[[nodiscard]] std::vector<doc::Node> flat_graph_nodes() {
    return {
        node(100U, "scene.object"),
        node(101U, "scene.object"),
        node(1U, schema::brightness_type, {real(schema::amount, 2.0)}),
        node(2U, schema::blur_type, {real(schema::radius, 6.0)}),
        node(3U, schema::radial_array_type,
             {whole(schema::count, 3), real(schema::radius, 2.0)}),
        layer(10U, 100U),
        layer(11U, 101U),
        node(12U, schema::composite_type),
        node(13U, schema::output_type),
        edge(20U, 1U, 0U, 10U, 0U),
        edge(21U, 2U, 0U, 10U, 0U),
        edge(22U, 3U, 0U, 10U, 1U),
        edge(23U, 10U, 0U, 12U, 0U, 10),
        edge(24U, 11U, 0U, 12U, 0U, -5),
        edge(25U, 12U, 0U, 13U, 0U),
    };
}

[[nodiscard]] scene::MeshInstance instance(core::u64 id,
                                           scene::BlendMode blend) {
    scene::MeshInstance value{};
    value.source = doc::NodeId{id};
    value.world = scene::identity();
    value.normal = scene::identity();
    value.style.blend = blend;
    return value;
}

void a_flat_graph_becomes_an_ordered_layer_plan() {
    const auto graph = graph_of(flat_graph_nodes());
    PLAN_CHECK(graph.valid());
    scene::Scene source{};
    source.instances.push_back(instance(100U, scene::BlendMode::Add));
    source.instances.push_back(instance(101U, scene::BlendMode::Normal));
    source.instances.push_back(instance(999U, scene::BlendMode::Screen));

    const auto built = render::plan_compositor_scene(graph, source);
    PLAN_CHECK(built.has_value());
    const auto& plan = built.value();
    PLAN_CHECK(!plan.replaces_scene);
    PLAN_CHECK(plan.layers.size() == 3U);
    PLAN_CHECK(plan.layers[0U].graph_node == doc::NodeId{});
    PLAN_CHECK(plan.layers[0U].scene_node == doc::NodeId{999U});
    PLAN_CHECK(plan.layers[0U].blend == scene::BlendMode::Screen);
    PLAN_CHECK(plan.layers[1U].graph_node == doc::NodeId{11U});
    PLAN_CHECK(plan.layers[1U].scene_node == doc::NodeId{101U});
    PLAN_CHECK(plan.layers[1U].blur_radius_px == 0.0F);
    PLAN_CHECK(plan.layers[2U].graph_node == doc::NodeId{10U});
    PLAN_CHECK(plan.layers[2U].scene_node == doc::NodeId{100U});
    PLAN_CHECK(plan.layers[2U].blend == scene::BlendMode::Add);
    PLAN_CHECK(std::abs(plan.layers[2U].blur_radius_px - 6.0F) < 0.001F);

    const auto red_count = static_cast<std::size_t>(std::count_if(
        plan.adjusted_scene.instances.begin(),
        plan.adjusted_scene.instances.end(),
        [](const auto& value) { return value.source == doc::NodeId{100U}; }));
    PLAN_CHECK(red_count == 3U);
    const auto red = std::find_if(
        plan.adjusted_scene.instances.begin(),
        plan.adjusted_scene.instances.end(),
        [](const auto& value) { return value.source == doc::NodeId{100U}; });
    PLAN_CHECK(red != plan.adjusted_scene.instances.end());
    PLAN_CHECK(red->style.effects.brightness_enabled);
    PLAN_CHECK(std::abs(red->style.effects.brightness - 2.0F) < 0.001F);
    PLAN_CHECK(red->style.effects.blur_enabled);
    PLAN_CHECK(std::any_of(
        plan.adjusted_scene.instances.begin(),
        plan.adjusted_scene.instances.end(),
        [](const auto& value) { return value.source == doc::NodeId{999U}; }));
}

void a_direct_scene_layer_preserves_unconnected_drawables() {
    const auto graph = graph_of({
        node(100U, "scene.object"),
        node(999U, "scene.object"),
        layer(10U, 100U),
        node(13U, schema::output_type),
        edge(20U, 10U, 0U, 13U, 0U),
    });
    PLAN_CHECK(graph.valid());
    scene::Scene source{};
    source.instances.push_back(instance(100U, scene::BlendMode::Normal));
    source.instances.push_back(instance(999U, scene::BlendMode::Normal));

    const auto built = render::plan_compositor_scene(graph, source);
    PLAN_CHECK(built.has_value());
    const auto& plan = built.value();
    PLAN_CHECK(!plan.replaces_scene);
    PLAN_CHECK(plan.layers.size() == 2U);
    PLAN_CHECK(plan.layers[0U].graph_node == doc::NodeId{});
    PLAN_CHECK(plan.layers[0U].scene_nodes ==
               std::vector<doc::NodeId>{doc::NodeId{999U}});
    PLAN_CHECK(plan.layers[1U].graph_node == doc::NodeId{10U});
    PLAN_CHECK(plan.layers[1U].scene_node == doc::NodeId{100U});
}

void output_can_explicitly_replace_the_whole_scene() {
    auto nodes = flat_graph_nodes();
    nodes[8U].properties.emplace(std::string{schema::replace_scene},
                                 doc::PropertyValue{true});
    const auto graph = graph_of(nodes);
    PLAN_CHECK(graph.valid());
    scene::Scene source{};
    source.instances.push_back(instance(100U, scene::BlendMode::Add));
    source.instances.push_back(instance(101U, scene::BlendMode::Normal));
    source.instances.push_back(instance(999U, scene::BlendMode::Screen));

    const auto built = render::plan_compositor_scene(graph, source);
    PLAN_CHECK(built.has_value());
    const auto& plan = built.value();
    PLAN_CHECK(plan.replaces_scene);
    PLAN_CHECK(plan.layers.size() == 2U);
    PLAN_CHECK(std::none_of(
        plan.layers.begin(), plan.layers.end(),
        [](const auto& pass) { return pass.scene_node == doc::NodeId{999U}; }));
}

void nested_image_composites_are_rejected_explicitly() {
    auto nodes = flat_graph_nodes();
    // Remove Composite -> Output, then insert a nested Composite in between.
    nodes.pop_back();
    nodes.push_back(node(14U, schema::composite_type));
    nodes.push_back(edge(25U, 12U, 0U, 14U, 0U));
    nodes.push_back(edge(26U, 14U, 0U, 13U, 0U));
    const auto graph = graph_of(nodes);
    PLAN_CHECK(graph.valid());
    scene::Scene source{};
    source.instances.push_back(instance(100U, scene::BlendMode::Normal));
    source.instances.push_back(instance(101U, scene::BlendMode::Normal));
    const auto built = render::plan_compositor_scene(graph, source);
    PLAN_CHECK(!built.has_value());
    PLAN_CHECK(built.error() ==
               render::CompositorScenePlanError::UnsupportedTopology);
}

void a_document_reference_must_resolve_to_a_drawable() {
    const auto graph = graph_of(flat_graph_nodes());
    scene::Scene source{};
    source.instances.push_back(instance(101U, scene::BlendMode::Normal));
    const auto built = render::plan_compositor_scene(graph, source);
    PLAN_CHECK(!built.has_value());
    PLAN_CHECK(built.error() ==
               render::CompositorScenePlanError::MissingSceneLayer);
}

void invalid_graphs_never_form_partial_render_plans() {
    const auto graph = graph_of({node(100U, "scene.object"), layer(10U, 100U),
                                 node(13U, schema::output_type)});
    PLAN_CHECK(!graph.valid());
    scene::Scene source{};
    source.instances.push_back(instance(100U, scene::BlendMode::Normal));
    const auto built = render::plan_compositor_scene(graph, source);
    PLAN_CHECK(!built.has_value());
    PLAN_CHECK(built.error() == render::CompositorScenePlanError::InvalidGraph);
}

void scene_effects_isolate_only_layers_that_need_it() {
    scene::Scene source{};
    source.instances.push_back(instance(60U, scene::BlendMode::Normal));
    source.instances.push_back(instance(10U, scene::BlendMode::Normal));
    source.instances.push_back(instance(20U, scene::BlendMode::Normal));
    source.instances.push_back(instance(40U, scene::BlendMode::Normal));
    auto blurred = instance(30U, scene::BlendMode::Normal);
    blurred.style.effects.blur_enabled = true;
    blurred.style.effects.blur_radius = 7.0F;
    source.instances.push_back(blurred);
    source.instances.push_back(instance(50U, scene::BlendMode::Screen));

    const auto built = render::plan_scene_effects(source);
    PLAN_CHECK(built.has_value());
    const auto& plan = built.value();
    PLAN_CHECK(scene::digest(plan.adjusted_scene) == scene::digest(source));
    PLAN_CHECK(plan.layers.size() == 3U);
    PLAN_CHECK(plan.layers[0U].scene_nodes ==
               std::vector<doc::NodeId>({doc::NodeId{10U}, doc::NodeId{20U},
                                         doc::NodeId{40U}, doc::NodeId{60U}}));
    PLAN_CHECK(plan.layers[1U].scene_node == doc::NodeId{30U});
    PLAN_CHECK(plan.layers[1U].scene_nodes.empty());
    PLAN_CHECK(std::abs(plan.layers[1U].blur_radius_px - 7.0F) < 0.001F);
    PLAN_CHECK(plan.layers[2U].scene_node == doc::NodeId{50U});
    PLAN_CHECK(plan.layers[2U].blend == scene::BlendMode::Screen);
}

void camera_motion_never_splits_the_ordinary_scene() {
    scene::Scene source{};
    source.camera.eye = scene::Vec3{0.0F, 0.0F, 5.0F};
    source.camera.target = scene::Vec3{};

    auto near = instance(10U, scene::BlendMode::Normal);
    near.world = scene::translation(scene::Vec3{0.0F, 0.0F, 1.0F});
    source.instances.push_back(near);
    auto blurred = instance(20U, scene::BlendMode::Normal);
    blurred.world = scene::translation(scene::Vec3{});
    blurred.style.effects.blur_enabled = true;
    blurred.style.effects.blur_radius = 7.0F;
    source.instances.push_back(blurred);
    auto far = instance(30U, scene::BlendMode::Normal);
    far.world = scene::translation(scene::Vec3{0.0F, 0.0F, -1.0F});
    source.instances.push_back(far);

    const auto front = render::plan_scene_effects(source);
    PLAN_CHECK(front.has_value());

    // Orbit to the opposite side. The near/far order of both ordinary objects
    // reverses, but they must remain one depth-buffered base pass rather than
    // becoming camera-dependent flat layers around the blur pass.
    source.camera.eye = scene::Vec3{0.0F, 0.0F, -5.0F};
    const auto back = render::plan_scene_effects(source);
    PLAN_CHECK(back.has_value());
    for (const auto* const plan : {&front.value(), &back.value()}) {
        PLAN_CHECK(plan->layers.size() == 2U);
        PLAN_CHECK(plan->layers[0U].scene_nodes ==
                   std::vector<doc::NodeId>(
                       {doc::NodeId{10U}, doc::NodeId{30U}}));
        PLAN_CHECK(plan->layers[1U].scene_node == doc::NodeId{20U});
        PLAN_CHECK(std::abs(plan->layers[1U].blur_radius_px - 7.0F) <
                   0.001F);
    }
}

void identity_effects_produce_the_no_effect_pass_plan() {
    scene::Scene plain{};
    plain.instances.push_back(instance(10U, scene::BlendMode::Normal));
    scene::Scene identity = plain;
    identity.instances.front().style.effects.brightness_enabled = true;
    identity.instances.front().style.effects.brightness = 1.0F;
    identity.instances.front().style.effects.blur_enabled = true;
    identity.instances.front().style.effects.blur_radius = 0.0F;

    const auto plain_plan = render::plan_scene_effects(plain);
    const auto identity_plan = render::plan_scene_effects(identity);
    PLAN_CHECK(plain_plan.has_value());
    PLAN_CHECK(identity_plan.has_value());
    PLAN_CHECK(identity_plan.value().layers == plain_plan.value().layers);
    PLAN_CHECK(identity_plan.value().layers.size() == 1U);
}

void an_isolated_layer_is_primed_with_every_other_opaque_drawable() {
    // 30 is blurred, so it renders alone; the rest share one pass. Each pass
    // must still be told about the opaque geometry the other one owns, or the
    // blurred object floats in front of the whole frame.
    scene::Scene source{};
    source.instances.push_back(instance(10U, scene::BlendMode::Normal));
    source.instances.push_back(instance(20U, scene::BlendMode::Normal));
    auto blurred = instance(30U, scene::BlendMode::Normal);
    blurred.style.effects.blur_enabled = true;
    blurred.style.effects.blur_radius = 7.0F;
    source.instances.push_back(blurred);

    const auto built = render::plan_scene_effects(source);
    PLAN_CHECK(built.has_value());
    const auto& plan = built.value();
    PLAN_CHECK(plan.layers.size() == 2U);
    PLAN_CHECK(plan.layers[0U].scene_nodes ==
               std::vector<doc::NodeId>({doc::NodeId{10U}, doc::NodeId{20U}}));
    // The blurred layer has no crisp silhouette, so it is not an occluder of
    // the ordinary pass even though the ordinary pass occludes it.
    PLAN_CHECK(plan.layers[0U].depth_only_nodes.empty());
    PLAN_CHECK(plan.layers[1U].scene_node == doc::NodeId{30U});
    PLAN_CHECK(plan.layers[1U].depth_only_nodes ==
               std::vector<doc::NodeId>({doc::NodeId{10U}, doc::NodeId{20U}}));
}

void only_crisply_opaque_drawables_prime_another_layers_depth() {
    scene::Scene source{};
    // Opaque and unblurred: a genuine occluder.
    source.instances.push_back(instance(10U, scene::BlendMode::Normal));
    // Translucent: you can see through it, so it removes nothing.
    auto translucent = instance(20U, scene::BlendMode::Normal);
    translucent.style.opacity = 0.4F;
    source.instances.push_back(translucent);
    // Additive: destination-dependent, and never hides its backdrop.
    source.instances.push_back(instance(40U, scene::BlendMode::Add));
    // The layer under test.
    auto blurred = instance(30U, scene::BlendMode::Normal);
    blurred.style.effects.blur_enabled = true;
    blurred.style.effects.blur_radius = 5.0F;
    source.instances.push_back(blurred);

    const auto built = render::plan_scene_effects(source);
    PLAN_CHECK(built.has_value());
    const auto& plan = built.value();
    const auto blurred_pass =
        std::find_if(plan.layers.begin(), plan.layers.end(),
                     [](const render::CompositorLayerPass& pass) {
                         return pass.scene_node == doc::NodeId{30U};
                     });
    PLAN_CHECK(blurred_pass != plan.layers.end());
    PLAN_CHECK(blurred_pass->depth_only_nodes ==
               std::vector<doc::NodeId>({doc::NodeId{10U}}));
}

void one_pass_needs_no_depth_priming() {
    scene::Scene source{};
    source.instances.push_back(instance(10U, scene::BlendMode::Normal));
    source.instances.push_back(instance(20U, scene::BlendMode::Normal));

    const auto built = render::plan_scene_effects(source);
    PLAN_CHECK(built.has_value());
    PLAN_CHECK(built.value().layers.size() == 1U);
    PLAN_CHECK(built.value().layers[0U].depth_only_nodes.empty());
}

void a_graph_layer_is_occluded_by_the_pass_through_stack() {
    // The reported defect exactly: dropping a Brightness node onto an object
    // isolated it into its own pass, and a flat composite then drew it in
    // front of everything. 999 is untouched scene content behind or in front
    // of it, and both passes must now see the other one's depth.
    const auto graph = graph_of({
        node(100U, "scene.object"),
        node(999U, "scene.object"),
        layer(10U, 100U),
        node(1U, schema::brightness_type, {real(schema::amount, 2.0)}),
        node(13U, schema::output_type),
        edge(20U, 10U, 0U, 13U, 0U),
        edge(21U, 1U, 0U, 10U, 0U),
    });
    PLAN_CHECK(graph.valid());
    scene::Scene source{};
    source.instances.push_back(instance(100U, scene::BlendMode::Normal));
    source.instances.push_back(instance(999U, scene::BlendMode::Normal));

    const auto built = render::plan_compositor_scene(graph, source);
    PLAN_CHECK(built.has_value());
    const auto& plan = built.value();
    PLAN_CHECK(plan.layers.size() == 2U);
    PLAN_CHECK(plan.layers[0U].depth_only_nodes ==
               std::vector<doc::NodeId>({doc::NodeId{100U}}));
    PLAN_CHECK(plan.layers[1U].scene_node == doc::NodeId{100U});
    PLAN_CHECK(plan.layers[1U].depth_only_nodes ==
               std::vector<doc::NodeId>({doc::NodeId{999U}}));
}

void a_replaced_scene_never_primes_depth_with_what_it_dropped() {
    auto nodes = flat_graph_nodes();
    nodes[8U].properties.emplace(std::string{schema::replace_scene},
                                 doc::PropertyValue{true});
    const auto graph = graph_of(nodes);
    PLAN_CHECK(graph.valid());
    scene::Scene source{};
    source.instances.push_back(instance(100U, scene::BlendMode::Normal));
    source.instances.push_back(instance(101U, scene::BlendMode::Normal));
    source.instances.push_back(instance(999U, scene::BlendMode::Normal));

    const auto built = render::plan_compositor_scene(graph, source);
    PLAN_CHECK(built.has_value());
    const auto& plan = built.value();
    PLAN_CHECK(plan.replaces_scene);
    // 999 is deliberately not rendered by this graph, so it cannot occlude
    // anything: geometry outside the frame hides nothing in it.
    for (const auto& pass : plan.layers) {
        PLAN_CHECK(std::find(pass.depth_only_nodes.begin(),
                             pass.depth_only_nodes.end(),
                             doc::NodeId{999U}) ==
                   pass.depth_only_nodes.end());
    }
    // 100 carries the graph's blur and an additive blend, so it is not an
    // occluder either; 101 is the only crisp opaque layer in this plan.
    PLAN_CHECK(plan.layers[0U].scene_node == doc::NodeId{101U});
    PLAN_CHECK(plan.layers[0U].depth_only_nodes.empty());
    PLAN_CHECK(plan.layers[1U].scene_node == doc::NodeId{100U});
    PLAN_CHECK(plan.layers[1U].depth_only_nodes ==
               std::vector<doc::NodeId>({doc::NodeId{101U}}));
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 14U> cases{{
    {"a flat graph becomes an ordered layer plan",
     &a_flat_graph_becomes_an_ordered_layer_plan},
    {"a direct scene layer preserves unconnected drawables",
     &a_direct_scene_layer_preserves_unconnected_drawables},
    {"Output can explicitly replace the whole scene",
     &output_can_explicitly_replace_the_whole_scene},
    {"nested image composites are rejected explicitly",
     &nested_image_composites_are_rejected_explicitly},
    {"a document reference must resolve to a drawable",
     &a_document_reference_must_resolve_to_a_drawable},
    {"invalid graphs never form partial render plans",
     &invalid_graphs_never_form_partial_render_plans},
    {"scene effects isolate only layers that need it",
     &scene_effects_isolate_only_layers_that_need_it},
    {"camera motion never splits the ordinary scene",
     &camera_motion_never_splits_the_ordinary_scene},
    {"identity effects produce the no-effect pass plan",
     &identity_effects_produce_the_no_effect_pass_plan},
    {"an isolated layer is primed with every other opaque drawable",
     &an_isolated_layer_is_primed_with_every_other_opaque_drawable},
    {"only crisply opaque drawables prime another layer's depth",
     &only_crisply_opaque_drawables_prime_another_layers_depth},
    {"one pass needs no depth priming", &one_pass_needs_no_depth_priming},
    {"a graph layer is occluded by the pass-through stack",
     &a_graph_layer_is_occluded_by_the_pass_through_stack},
    {"a replaced scene never primes depth with what it dropped",
     &a_replaced_scene_never_primes_depth_with_what_it_dropped},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const Case& test : cases) {
        try {
            test.function();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << cases.size() - failures << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
