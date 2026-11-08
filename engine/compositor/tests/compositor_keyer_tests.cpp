#include <mediaserver/compositor/compositor_model.hpp>
#include <mediaserver/compositor/compositor_schema.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace compositor = mediaserver::compositor;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace scene = mediaserver::scene;
namespace schema = mediaserver::compositor::schema;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define KEY_GRAPH_CHECK(...)                                                  \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

using Property = std::pair<std::string, doc::PropertyValue>;

[[nodiscard]] Property whole(std::string_view name, std::int64_t value) {
    return {std::string{name}, value};
}

[[nodiscard]] Property real(std::string_view name, double value) {
    return {std::string{name}, value};
}

[[nodiscard]] Property boolean(std::string_view name, bool value) {
    return {std::string{name}, value};
}

[[nodiscard]] doc::Node node(
    core::u64 id, std::string_view type,
    std::initializer_list<Property> properties = {}) {
    doc::Node result{};
    result.id = doc::NodeId{id};
    result.type = std::string{type};
    for (const auto& [name, value] : properties) {
        result.properties.emplace(name, value);
    }
    return result;
}

// A Scene Layer takes effects on port 0 and filters on port 2.
constexpr std::int64_t effects_port = 0;
constexpr std::int64_t filters_port = 2;

[[nodiscard]] doc::Node edge(core::u64 id, core::u64 from, core::u64 to,
                             core::i64 order = 0,
                             std::int64_t to_port = effects_port) {
    return node(id, schema::edge_type,
                {whole(schema::from_node,
                       static_cast<std::int64_t>(from)),
                 whole(schema::from_port, 0),
                 whole(schema::to_node, static_cast<std::int64_t>(to)),
                 whole(schema::to_port, to_port),
                 whole(schema::order, order)});
}

[[nodiscard]] doc::Node chroma_node(core::u64 id = 1U,
                                    double mix = 0.75) {
    return node(
        id, schema::chroma_key_type,
        {real(schema::chroma_key_color_r, 0.1),
         real(schema::chroma_key_color_g, 0.9),
         real(schema::chroma_key_color_b, 0.2),
         real(schema::chroma_key_tolerance, 0.15),
         real(schema::chroma_key_softness, 0.05),
         whole(schema::chroma_key_morphology_px, 3),
         real(schema::chroma_key_spill_suppression, 0.4),
         real(schema::chroma_key_mix, mix)});
}

[[nodiscard]] doc::Node luma_node(core::u64 id = 2U,
                                  double mix = 0.5) {
    return node(
        id, schema::luma_key_type,
        {real(schema::luma_key_low_threshold, 0.2),
         real(schema::luma_key_high_threshold, 0.8),
         real(schema::luma_key_low_softness, 0.1),
         real(schema::luma_key_high_softness, 0.15),
         boolean(schema::luma_key_invert, true),
         real(schema::luma_key_mix, mix)});
}

// A fixture with exactly one Stage owns every graph and scene node that does
// not name a Stage itself; fixtures with several Stages say which they mean.
[[nodiscard]] std::vector<doc::Node> owned_by_the_only_stage(std::vector<doc::Node> nodes) {
    const doc::Node* only{};
    std::size_t stages{};
    for (const doc::Node& value : nodes) {
        if (value.type == "scene.stage") {
            only = &value;
            ++stages;
        }
    }
    if (stages != 1U) {
        return nodes;
    }
    const auto owner = static_cast<std::int64_t>(only->id.value());
    for (doc::Node& value : nodes) {
        const std::string_view type{value.type};
        constexpr std::array<std::string_view, 9U> scoped{
            "scene.camera", "scene.object", "scene.group", "scene.light",
            "scene.media", "scene.timeline_lane", "scene.timeline_marker",
            "scene.model", "scene.particle_effect"};
        if ((type.starts_with("compositor.") && type != "compositor.edge") ||
            std::find(scoped.begin(), scoped.end(), type) != scoped.end()) {
            value.properties.emplace("scene_id", doc::PropertyValue{owner});
        }
    }
    return nodes;
}

[[nodiscard]] doc::Root root_of(std::vector<doc::Node> nodes) {
    nodes = owned_by_the_only_stage(std::move(nodes));
    doc::Root result;
    for (doc::Node& value : nodes) {
        result = result.set(std::move(value));
    }
    return result;
}

[[nodiscard]] doc::Root key_graph(bool identities = false) {
    return root_of({
        node(100U, scene::schema::stage_type,
             {whole(scene::schema::canvas_width, 320),
              whole(scene::schema::canvas_height, 180)}),
        node(200U, scene::schema::object_type,
             {whole(scene::schema::scene_id, 100)}),
        chroma_node(1U, identities ? 0.0 : 0.75),
        luma_node(2U, identities ? 0.0 : 0.5),
        node(3U, schema::scene_layer_type,
             {whole(schema::scene_node, 200)}),
        node(4U, schema::output_type), edge(10U, 1U, 3U, 0, filters_port),
        edge(11U, 2U, 3U, 1, filters_port), edge(12U, 3U, 4U),
    });
}

void typed_nodes_plan_exact_zero_bounds_resources_and_order() {
    const doc::Root root = key_graph();
    compositor::CompositorModel model;
    model.reset(root);
    const compositor::CompositorGraph graph =
        model.evaluate(doc::NodeId{100U});
    KEY_GRAPH_CHECK(graph.valid());

    const auto* const chroma = graph.node(doc::NodeId{1U});
    const auto* const luma = graph.node(doc::NodeId{2U});
    KEY_GRAPH_CHECK(chroma != nullptr && luma != nullptr);
    KEY_GRAPH_CHECK(std::get<compositor::ChromaKeyParameters>(
                        chroma->parameters) ==
                    (scene::ChromaKeyParameters{
                        {0.1F, 0.9F, 0.2F}, 0.15F, 0.05F, 3, 0.4F,
                        0.75F}));
    KEY_GRAPH_CHECK(std::get<compositor::LumaKeyParameters>(
                        luma->parameters) ==
                    (scene::LumaKeyParameters{
                        0.2F, 0.8F, 0.1F, 0.15F, true, 0.5F}));
    KEY_GRAPH_CHECK(graph.step(chroma->node)->requires_offscreen);
    KEY_GRAPH_CHECK(graph.step(luma->node)->requires_offscreen);
    KEY_GRAPH_CHECK(graph.metadata(chroma->node)->execution_domain ==
                    compositor::GraphExecutionDomain::IsolatedLayer);
    KEY_GRAPH_CHECK(graph.metadata(luma->node)->execution_domain ==
                    compositor::GraphExecutionDomain::IsolatedLayer);
    KEY_GRAPH_CHECK(graph.metadata(chroma->node)->bounds_expansion ==
                    scene::EffectBoundsExpansion{});
    KEY_GRAPH_CHECK(graph.metadata(luma->node)->bounds_expansion ==
                    scene::EffectBoundsExpansion{});

    KEY_GRAPH_CHECK(graph.resources.available);
    KEY_GRAPH_CHECK(graph.resources.active_scene_layers == 1U);
    KEY_GRAPH_CHECK(graph.resources.chroma_keyed_scene_layers == 1U);
    KEY_GRAPH_CHECK(graph.resources.luma_keyed_scene_layers == 1U);
    KEY_GRAPH_CHECK(graph.resources.offscreen_textures == 8U);
    KEY_GRAPH_CHECK(graph.resources.render_passes == 8U);
    KEY_GRAPH_CHECK(graph.resources.blur_taps_per_pixel == 0U);
    KEY_GRAPH_CHECK(graph.resources.max_layer_bounds_expansion ==
                    scene::EffectBoundsExpansion{});

    scene::SceneModel scene_model;
    scene_model.reset(root);
    const scene::Scene source = scene_model.evaluate(
        doc::NodeId{100U}, scene::SceneTime{0U, 60U, 1U});
    const scene::Scene adjusted = compositor::apply(graph, source);
    KEY_GRAPH_CHECK(adjusted.instances.size() == 1U);
    const auto& operations =
        adjusted.instances.front().style.effects.spatial_operations;
    KEY_GRAPH_CHECK(operations.size() == 2U);
    KEY_GRAPH_CHECK(operations[0U].kind == scene::EffectKind::ChromaKey);
    KEY_GRAPH_CHECK(operations[1U].kind == scene::EffectKind::LumaKey);
}

void mix_zero_is_canonical_and_plan_resource_inert() {
    doc::Root root = key_graph(true);
    doc::Node chroma = *root.get(doc::NodeId{1U});
    chroma.properties.insert_or_assign(
        std::string{schema::chroma_key_color_r}, std::string{"stale"});
    chroma.properties.insert_or_assign(
        std::string{schema::chroma_key_morphology_px},
        std::numeric_limits<std::int64_t>::max());
    doc::Node luma = *root.get(doc::NodeId{2U});
    luma.properties.insert_or_assign(
        std::string{schema::luma_key_low_threshold}, 0.9);
    luma.properties.insert_or_assign(
        std::string{schema::luma_key_high_threshold}, 0.1);
    root = root.set(chroma).set(luma);

    compositor::CompositorModel model;
    model.reset(root);
    const compositor::CompositorGraph graph =
        model.evaluate(doc::NodeId{100U});
    KEY_GRAPH_CHECK(graph.valid());
    KEY_GRAPH_CHECK(compositor::effect_node_is_identity(
        *graph.node(doc::NodeId{1U})));
    KEY_GRAPH_CHECK(compositor::effect_node_is_identity(
        *graph.node(doc::NodeId{2U})));
    KEY_GRAPH_CHECK(!graph.step(doc::NodeId{1U})->requires_offscreen);
    KEY_GRAPH_CHECK(!graph.step(doc::NodeId{2U})->requires_offscreen);
    KEY_GRAPH_CHECK(graph.resources.chroma_keyed_scene_layers == 0U);
    KEY_GRAPH_CHECK(graph.resources.luma_keyed_scene_layers == 0U);
    KEY_GRAPH_CHECK(graph.resources.offscreen_textures == 4U);
    KEY_GRAPH_CHECK(graph.resources.render_passes == 2U);

    scene::SceneModel scene_model;
    scene_model.reset(root);
    const scene::Scene adjusted = compositor::apply(
        graph, scene_model.evaluate(
                   doc::NodeId{100U}, scene::SceneTime{0U, 60U, 1U}));
    KEY_GRAPH_CHECK(adjusted.instances.front()
                        .style.effects.spatial_operations.empty());
}

void active_malformed_parameters_fail_closed() {
    doc::Node bad_chroma = chroma_node();
    bad_chroma.properties.insert_or_assign(
        std::string{schema::chroma_key_morphology_px},
        static_cast<std::int64_t>(scene::max_key_morphology_radius_px) + 1);
    doc::Node bad_luma = luma_node();
    bad_luma.properties.insert_or_assign(
        std::string{schema::luma_key_low_threshold}, 0.9);
    bad_luma.properties.insert_or_assign(
        std::string{schema::luma_key_high_threshold}, 0.1);
    compositor::CompositorModel model;
    model.reset(root_of({bad_chroma, bad_luma}));
    KEY_GRAPH_CHECK(model.size() == 0U);
    KEY_GRAPH_CHECK(model.counters().rejected == 2U);
}

// An effect added to a keyed layer goes into EFFECTS and runs before
// the keys however late it was wired. It cannot go into FILTERS.
void an_effect_beside_keys_runs_before_them() {
    doc::Root root = key_graph();
    root = root.set(
        node(5U, schema::brightness_type,
             {real(schema::amount, 1.5), whole(scene::schema::scene_id, 100)}));
    doc::Root effect = root.set(edge(13U, 5U, 3U, 2, effects_port));
    compositor::CompositorModel model;
    model.reset(effect);
    const compositor::CompositorGraph graph =
        model.evaluate(doc::NodeId{100U});
    KEY_GRAPH_CHECK(graph.valid());
    const compositor::PlanStep* const layer = graph.step(doc::NodeId{3U});
    KEY_GRAPH_CHECK(layer != nullptr && layer->inputs.size() == 3U);
    KEY_GRAPH_CHECK(layer->inputs[0].source == doc::NodeId{5U});
    KEY_GRAPH_CHECK(layer->inputs[1].source == doc::NodeId{1U});
    KEY_GRAPH_CHECK(layer->inputs[2].source == doc::NodeId{2U});

    compositor::CompositorModel wrong;
    wrong.reset(root.set(edge(13U, 5U, 3U, 2, filters_port)));
    const compositor::CompositorGraph refused =
        wrong.evaluate(doc::NodeId{100U});
    KEY_GRAPH_CHECK(!refused.valid());
    KEY_GRAPH_CHECK(std::any_of(
        refused.diagnostics.begin(), refused.diagnostics.end(),
        [](const compositor::GraphDiagnostic& diagnostic) noexcept {
            return diagnostic.code ==
                compositor::DiagnosticCode::IncompatiblePorts;
        }));
}

void every_key_parameter_participates_in_graph_digest() {
    compositor::CompositorModel model;
    model.reset(key_graph());
    const compositor::CompositorGraph baseline =
        model.evaluate(doc::NodeId{100U});
    compositor::CompositorGraph changed = baseline;
    std::get<compositor::ChromaKeyParameters>(
        changed.nodes[0U].parameters).spill_suppression = 0.25F;
    KEY_GRAPH_CHECK(compositor::digest(changed) !=
                    compositor::digest(baseline));
    changed = baseline;
    const auto luma = std::find_if(
        changed.nodes.begin(), changed.nodes.end(),
        [](const compositor::GraphNode& value) noexcept {
            return value.kind == compositor::NodeKind::LumaKey;
        });
    KEY_GRAPH_CHECK(luma != changed.nodes.end());
    std::get<compositor::LumaKeyParameters>(luma->parameters).invert = false;
    KEY_GRAPH_CHECK(compositor::digest(changed) !=
                    compositor::digest(baseline));
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 5U> cases{{
    {"typed nodes plan exact zero bounds resources and order",
     &typed_nodes_plan_exact_zero_bounds_resources_and_order},
    {"mix zero is canonical and plan resource inert",
     &mix_zero_is_canonical_and_plan_resource_inert},
    {"active malformed parameters fail closed",
     &active_malformed_parameters_fail_closed},
    {"an effect beside keys runs before them",
     &an_effect_beside_keys_runs_before_them},
    {"every key parameter participates in graph digest",
     &every_key_parameter_participates_in_graph_digest},
}};

} // namespace

int main() {
    std::size_t failures{};
    for (const Case& test : cases) {
        try {
            test.function();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << cases.size() - failures << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
