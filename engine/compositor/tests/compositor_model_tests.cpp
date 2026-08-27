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
#include <variant>
#include <vector>

namespace {

namespace compositor = mediaserver::compositor;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace schema = mediaserver::compositor::schema;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define COMPOSITOR_CHECK(...)                                                  \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

using Property = std::pair<std::string, doc::PropertyValue>;

[[nodiscard]] Property whole(std::string_view name, std::int64_t value) {
    return {std::string{name}, doc::PropertyValue{value}};
}

[[nodiscard]] Property real(std::string_view name, double value) {
    return {std::string{name}, doc::PropertyValue{value}};
}

[[nodiscard]] Property flag(std::string_view name, bool value) {
    return {std::string{name}, doc::PropertyValue{value}};
}

[[nodiscard]] Property text(std::string_view name, std::string_view value) {
    return {std::string{name}, doc::PropertyValue{std::string{value}}};
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

[[nodiscard]] doc::Node scene_layer(core::u64 id, core::u64 scene_node) {
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

[[nodiscard]] doc::Root root_of(const std::vector<doc::Node>& nodes) {
    doc::Root root;
    for (const doc::Node& one : nodes) {
        root = root.set(one);
    }
    return root;
}

[[nodiscard]] std::vector<doc::Node> shared_effect_graph() {
    return {
        node(100U, "scene.object"),
        node(101U, "scene.media"),
        node(1U, schema::blur_type,
             {text(schema::label, "Shared plate blur"),
              real(schema::radius, 12.5)}),
        node(2U, schema::radial_array_type,
             {whole(schema::count, 9), real(schema::center_x, 2.0),
              real(schema::center_y, 1.25), real(schema::center_z, -3.0),
              real(schema::radius, 8.0),
              real(schema::start_angle_turns, 0.125)}),
        scene_layer(3U, 100U),
        scene_layer(4U, 101U),
        node(5U, schema::composite_type),
        node(6U, schema::output_type),
        edge(20U, 1U, 0U, 3U, 0U),
        edge(21U, 1U, 0U, 4U, 0U),
        edge(22U, 2U, 0U, 3U, 1U),
        edge(23U, 2U, 0U, 4U, 1U),
        edge(24U, 3U, 0U, 5U, 0U, 10),
        edge(25U, 4U, 0U, 5U, 0U, -2),
        edge(26U, 5U, 0U, 6U, 0U),
    };
}

[[nodiscard]] bool has_diagnostic(const compositor::CompositorGraph& graph,
                                  compositor::DiagnosticCode code,
                                  doc::NodeId subject) {
    return std::any_of(
        graph.diagnostics.begin(), graph.diagnostics.end(),
        [code, subject](const compositor::GraphDiagnostic& diagnostic) {
            return diagnostic.code == code && diagnostic.subject == subject;
        });
}

void typed_ports_describe_the_editor_contract() {
    const auto layer_inputs =
        compositor::input_ports(compositor::NodeKind::SceneLayer);
    COMPOSITOR_CHECK(layer_inputs.size() == 2U);
    COMPOSITOR_CHECK(layer_inputs[0U].type ==
                     compositor::PortValueType::Effect);
    COMPOSITOR_CHECK(layer_inputs[0U].multiple);
    COMPOSITOR_CHECK(layer_inputs[1U].type ==
                     compositor::PortValueType::TransformSet);
    COMPOSITOR_CHECK(!layer_inputs[1U].multiple);

    const auto blur_outputs =
        compositor::output_ports(compositor::NodeKind::Blur);
    COMPOSITOR_CHECK(blur_outputs.size() == 1U);
    COMPOSITOR_CHECK(blur_outputs.front().type ==
                     compositor::PortValueType::Effect);
    COMPOSITOR_CHECK(blur_outputs.front().multiple);

    const auto composite_inputs =
        compositor::input_ports(compositor::NodeKind::Composite);
    COMPOSITOR_CHECK(composite_inputs.front().type ==
                     compositor::PortValueType::Image);
    COMPOSITOR_CHECK(composite_inputs.front().multiple);
    COMPOSITOR_CHECK(composite_inputs.front().required);
}

void shared_blur_and_nine_angle_array_produce_an_explicit_plan() {
    compositor::CompositorModel model;
    model.reset(root_of(shared_effect_graph()));
    const compositor::CompositorGraph graph = model.evaluate();

    COMPOSITOR_CHECK(graph.valid());
    COMPOSITOR_CHECK(graph.nodes.size() == 6U);
    COMPOSITOR_CHECK(graph.edges.size() == 7U);
    COMPOSITOR_CHECK(graph.plan.steps.size() == 6U);
    const std::array<core::u64, 6U> expected_order{{1U, 2U, 3U, 4U, 5U, 6U}};
    for (std::size_t index = 0U; index < expected_order.size(); ++index) {
        COMPOSITOR_CHECK(graph.plan.steps[index].node ==
                         doc::NodeId{expected_order[index]});
    }

    const compositor::PlanStep* const blur = graph.step(doc::NodeId{1U});
    COMPOSITOR_CHECK(blur != nullptr);
    COMPOSITOR_CHECK(blur->requires_offscreen);
    COMPOSITOR_CHECK(blur->consumers.size() == 2U);
    COMPOSITOR_CHECK(blur->consumers[0U].target == doc::NodeId{3U});
    COMPOSITOR_CHECK(blur->consumers[1U].target == doc::NodeId{4U});

    const compositor::PlanStep* const composite = graph.step(doc::NodeId{5U});
    COMPOSITOR_CHECK(composite != nullptr);
    COMPOSITOR_CHECK(composite->inputs.size() == 2U);
    COMPOSITOR_CHECK(composite->inputs[0U].edge == doc::NodeId{25U});
    COMPOSITOR_CHECK(composite->inputs[1U].edge == doc::NodeId{24U});

    const compositor::PlanStep* const radial = graph.step(doc::NodeId{2U});
    COMPOSITOR_CHECK(radial != nullptr);
    COMPOSITOR_CHECK(radial->consumers.size() == 2U);
    COMPOSITOR_CHECK(radial->radial_instances.size() == 9U);
    constexpr float centre_x = 2.0F;
    constexpr float centre_z = -3.0F;
    for (const compositor::RadialInstance& instance :
         radial->radial_instances) {
        const float x = instance.world.at(0U, 3U);
        const float y = instance.world.at(1U, 3U);
        const float z = instance.world.at(2U, 3U);
        const float from_centre_x = x - centre_x;
        const float from_centre_z = z - centre_z;
        const float distance = std::sqrt(from_centre_x * from_centre_x +
                                         from_centre_z * from_centre_z);
        COMPOSITOR_CHECK(std::abs(distance - 8.0F) < 0.001F);
        COMPOSITOR_CHECK(std::abs(y - 1.25F) < 0.0001F);

        const float forward_x = instance.world.at(0U, 2U);
        const float forward_z = instance.world.at(2U, 2U);
        const float inward_x = -from_centre_x / distance;
        const float inward_z = -from_centre_z / distance;
        COMPOSITOR_CHECK(forward_x * inward_x + forward_z * inward_z > 0.999F);
    }
}

void invalid_edges_are_diagnosed_and_never_partially_executed() {
    {
        compositor::CompositorModel model;
        model.reset(root_of(
            {node(1U, schema::output_type), edge(10U, 99U, 0U, 1U, 0U)}));
        const auto graph = model.evaluate();
        COMPOSITOR_CHECK(has_diagnostic(
            graph, compositor::DiagnosticCode::DanglingNode, doc::NodeId{10U}));
        COMPOSITOR_CHECK(graph.plan.steps.empty());
    }
    {
        compositor::CompositorModel model;
        model.reset(
            root_of({node(1U, schema::blur_type), node(2U, schema::output_type),
                     edge(10U, 1U, 0U, 2U, 0U)}));
        const auto graph = model.evaluate();
        COMPOSITOR_CHECK(
            has_diagnostic(graph, compositor::DiagnosticCode::IncompatiblePorts,
                           doc::NodeId{10U}));
        COMPOSITOR_CHECK(graph.plan.steps.empty());
    }
    {
        compositor::CompositorModel model;
        model.reset(root_of({node(100U, "scene.object"), scene_layer(1U, 100U),
                             node(2U, schema::output_type),
                             edge(10U, 1U, 7U, 2U, 0U)}));
        const auto graph = model.evaluate();
        COMPOSITOR_CHECK(has_diagnostic(
            graph, compositor::DiagnosticCode::UnknownPort, doc::NodeId{10U}));
        COMPOSITOR_CHECK(graph.plan.steps.empty());
    }
    {
        compositor::CompositorModel model;
        model.reset(
            root_of({node(100U, "scene.object"), node(101U, "scene.media"),
                     scene_layer(1U, 100U), scene_layer(2U, 101U),
                     node(3U, schema::output_type), edge(10U, 1U, 0U, 3U, 0U),
                     edge(11U, 2U, 0U, 3U, 0U)}));
        const auto graph = model.evaluate();
        COMPOSITOR_CHECK(
            has_diagnostic(graph, compositor::DiagnosticCode::DuplicateInput,
                           doc::NodeId{11U}));
        COMPOSITOR_CHECK(graph.plan.steps.empty());
    }
}

void cycles_name_only_cycle_members_not_downstream_output() {
    compositor::CompositorModel model;
    model.reset(root_of(
        {node(1U, schema::composite_type), node(2U, schema::composite_type),
         node(3U, schema::output_type), edge(10U, 1U, 0U, 2U, 0U),
         edge(11U, 2U, 0U, 1U, 0U), edge(12U, 1U, 0U, 3U, 0U)}));
    const auto graph = model.evaluate();
    COMPOSITOR_CHECK(has_diagnostic(graph, compositor::DiagnosticCode::Cycle,
                                    doc::NodeId{1U}));
    COMPOSITOR_CHECK(has_diagnostic(graph, compositor::DiagnosticCode::Cycle,
                                    doc::NodeId{2U}));
    COMPOSITOR_CHECK(!has_diagnostic(graph, compositor::DiagnosticCode::Cycle,
                                     doc::NodeId{3U}));
    COMPOSITOR_CHECK(graph.plan.steps.empty());
}

void scene_references_and_required_outputs_are_validated() {
    compositor::CompositorModel model;
    model.reset(root_of({scene_layer(1U, 999U)}));
    const auto graph = model.evaluate();
    COMPOSITOR_CHECK(has_diagnostic(
        graph, compositor::DiagnosticCode::DanglingSceneReference,
        doc::NodeId{1U}));
    COMPOSITOR_CHECK(has_diagnostic(
        graph, compositor::DiagnosticCode::MissingOutput, doc::NodeId{}));
    COMPOSITOR_CHECK(graph.plan.steps.empty());
}

void a_second_output_is_diagnosed_before_render_planning() {
    compositor::CompositorModel model;
    model.reset(
        root_of({node(1U, "scene.object"), scene_layer(2U, 1U),
                 node(3U, schema::output_type), node(4U, schema::output_type),
                 edge(10U, 2U, 0U, 3U, 0U), edge(11U, 2U, 0U, 4U, 0U)}));
    const auto graph = model.evaluate();
    COMPOSITOR_CHECK(has_diagnostic(
        graph, compositor::DiagnosticCode::DuplicateOutput, doc::NodeId{4U}));
    COMPOSITOR_CHECK(!graph.valid());
    COMPOSITOR_CHECK(graph.plan.steps.empty());
}

void checkpoint_and_streamed_deltas_are_identical() {
    const auto nodes = shared_effect_graph();
    compositor::CompositorModel checkpointed;
    checkpointed.reset(root_of(nodes));

    compositor::CompositorModel streamed;
    doc::Root running;
    for (auto iterator = nodes.rbegin(); iterator != nodes.rend(); ++iterator) {
        const doc::Root next = running.set(*iterator);
        streamed.apply(diff(running, next));
        running = next;
    }
    const auto checkpoint_graph = checkpointed.evaluate();
    const auto streamed_graph = streamed.evaluate();
    COMPOSITOR_CHECK(checkpoint_graph == streamed_graph);
    COMPOSITOR_CHECK(compositor::digest(checkpoint_graph) ==
                     compositor::digest(streamed_graph));
    COMPOSITOR_CHECK(checkpointed.counters() == streamed.counters());
}

void every_authored_control_changes_the_digest() {
    const auto base_nodes = shared_effect_graph();
    compositor::CompositorModel base;
    base.reset(root_of(base_nodes));
    const core::u64 reference = compositor::digest(base.evaluate());
    COMPOSITOR_CHECK(reference != 0U);

    auto changed = base_nodes;
    changed[2U].properties.insert_or_assign(std::string{schema::radius},
                                            doc::PropertyValue{24.0});
    compositor::CompositorModel blur_changed;
    blur_changed.reset(root_of(changed));
    COMPOSITOR_CHECK(compositor::digest(blur_changed.evaluate()) != reference);

    changed = base_nodes;
    changed[3U].properties.insert_or_assign(
        std::string{schema::start_angle_turns}, doc::PropertyValue{0.25});
    compositor::CompositorModel array_changed;
    array_changed.reset(root_of(changed));
    COMPOSITOR_CHECK(compositor::digest(array_changed.evaluate()) != reference);

    changed = base_nodes;
    changed[12U].properties.insert_or_assign(
        std::string{schema::order}, doc::PropertyValue{std::int64_t{30}});
    compositor::CompositorModel order_changed;
    order_changed.reset(root_of(changed));
    COMPOSITOR_CHECK(compositor::digest(order_changed.evaluate()) != reference);

    changed = base_nodes;
    changed[4U].properties.insert_or_assign(
        std::string{schema::scene_node}, doc::PropertyValue{std::int64_t{101}});
    compositor::CompositorModel reference_changed;
    reference_changed.reset(root_of(changed));
    COMPOSITOR_CHECK(compositor::digest(reference_changed.evaluate()) !=
                     reference);

    changed = base_nodes;
    changed[7U].properties.insert_or_assign(std::string{schema::replace_scene},
                                            doc::PropertyValue{true});
    compositor::CompositorModel output_changed;
    output_changed.reset(root_of(changed));
    const auto replacement = output_changed.evaluate();
    const auto* const output = replacement.node(doc::NodeId{6U});
    COMPOSITOR_CHECK(output != nullptr);
    COMPOSITOR_CHECK(std::get<compositor::OutputParameters>(output->parameters)
                         .replace_scene);
    COMPOSITOR_CHECK(compositor::digest(replacement) != reference);
}

void malformed_values_and_capacity_are_bounded() {
    compositor::CompositorModel malformed;
    malformed.reset(root_of(
        {node(1U, schema::radial_array_type, {whole(schema::count, 0)}),
         node(2U, schema::blur_type, {real(schema::radius, 257.0)}),
         node(3U, schema::brightness_type, {text(schema::editor_x, "left")})}));
    COMPOSITOR_CHECK(malformed.counters().rejected == 3U);
    COMPOSITOR_CHECK(malformed.size() == 0U);
    COMPOSITOR_CHECK(malformed.evaluate().valid());

    doc::Root node_root;
    const core::u32 node_count = compositor::max_graph_nodes + 3U;
    for (core::u32 index = 0U; index < node_count; ++index) {
        node_root = node_root.set(
            node(static_cast<core::u64>(index) + 1U, schema::brightness_type));
    }
    compositor::CompositorModel nodes_over;
    nodes_over.reset(node_root);
    const auto bounded_nodes = nodes_over.evaluate();
    COMPOSITOR_CHECK(nodes_over.counters().nodes_over_capacity() == 3U);
    COMPOSITOR_CHECK(bounded_nodes.nodes.size() == compositor::max_graph_nodes);
    COMPOSITOR_CHECK(has_diagnostic(
        bounded_nodes, compositor::DiagnosticCode::NodesOverCapacity,
        doc::NodeId{}));

    doc::Root edge_root;
    edge_root = edge_root.set(node(10001U, "scene.object"));
    edge_root = edge_root.set(scene_layer(1U, 10001U));
    edge_root = edge_root.set(node(2U, schema::blur_type));
    edge_root = edge_root.set(node(3U, schema::output_type));
    edge_root = edge_root.set(edge(10U, 1U, 0U, 3U, 0U));
    // One image edge already exists, so this many shared-effect edges takes
    // the authored total exactly one past the graph limit.
    const core::u32 edge_count = compositor::max_graph_edges;
    for (core::u32 index = 0U; index < edge_count; ++index) {
        edge_root = edge_root.set(
            edge(static_cast<core::u64>(index) + 20U, 2U, 0U, 1U, 0U));
    }
    compositor::CompositorModel edges_over;
    edges_over.reset(edge_root);
    const auto bounded_edges = edges_over.evaluate();
    COMPOSITOR_CHECK(edges_over.counters().edges_over_capacity() == 1U);
    COMPOSITOR_CHECK(bounded_edges.edges.size() == compositor::max_graph_edges);
    COMPOSITOR_CHECK(has_diagnostic(
        bounded_edges, compositor::DiagnosticCode::EdgesOverCapacity,
        doc::NodeId{}));
    COMPOSITOR_CHECK(bounded_edges.plan.steps.empty());
}

void a_valid_graph_modifies_only_its_referenced_scene_layers() {
    compositor::CompositorModel shared;
    shared.reset(root_of(shared_effect_graph()));

    mediaserver::scene::Scene source{};
    mediaserver::scene::MeshInstance mesh{};
    mesh.source = doc::NodeId{100U};
    mesh.world = mediaserver::scene::identity();
    mesh.normal = mediaserver::scene::identity();
    source.instances.push_back(mesh);
    mediaserver::scene::MediaInstance media{};
    media.source_node = doc::NodeId{101U};
    media.world = mediaserver::scene::identity();
    media.normal = mediaserver::scene::identity();
    source.media.push_back(media);
    mediaserver::scene::MeshInstance untouched = mesh;
    untouched.source = doc::NodeId{999U};
    source.instances.push_back(untouched);

    const auto expanded = compositor::apply(shared.evaluate(), source);
    COMPOSITOR_CHECK(expanded.instances.size() == 10U);
    COMPOSITOR_CHECK(expanded.media.size() == 9U);
    const auto referenced = std::find_if(
        expanded.instances.begin(), expanded.instances.end(),
        [](const auto& value) { return value.source == doc::NodeId{100U}; });
    COMPOSITOR_CHECK(referenced != expanded.instances.end());
    COMPOSITOR_CHECK(referenced->style.effects.blur_enabled);
    COMPOSITOR_CHECK(std::abs(referenced->style.effects.blur_radius - 12.5F) <
                     0.001F);
    const auto left_alone = std::find_if(
        expanded.instances.begin(), expanded.instances.end(),
        [](const auto& value) { return value.source == doc::NodeId{999U}; });
    COMPOSITOR_CHECK(left_alone != expanded.instances.end());
    COMPOSITOR_CHECK(!left_alone->style.effects.blur_enabled);

    compositor::CompositorModel bright;
    bright.reset(
        root_of({node(100U, "scene.object"),
                 node(1U, schema::brightness_type, {real(schema::amount, 2.0)}),
                 scene_layer(2U, 100U), node(3U, schema::output_type),
                 edge(10U, 1U, 0U, 2U, 0U), edge(11U, 2U, 0U, 3U, 0U)}));
    const auto lit = compositor::apply(bright.evaluate(), source);
    const auto brightened = std::find_if(
        lit.instances.begin(), lit.instances.end(),
        [](const auto& value) { return value.source == doc::NodeId{100U}; });
    COMPOSITOR_CHECK(brightened != lit.instances.end());
    COMPOSITOR_CHECK(brightened->style.effects.brightness_enabled);
    COMPOSITOR_CHECK(std::abs(brightened->style.effects.brightness - 2.0F) <
                     0.001F);
}

void disconnected_scene_layers_never_modify_program() {
    compositor::CompositorModel model;
    model.reset(root_of({
        node(100U, "scene.object"),
        node(999U, "scene.object"),
        node(1U, schema::brightness_type, {real(schema::amount, 4.0)}),
        scene_layer(2U, 100U),
        scene_layer(3U, 999U),
        node(4U, schema::output_type, {flag(schema::replace_scene, false)}),
        edge(10U, 1U, 0U, 3U, 0U),
        edge(11U, 2U, 0U, 4U, 0U),
    }));
    const auto graph = model.evaluate();
    COMPOSITOR_CHECK(graph.valid());

    mediaserver::scene::Scene source{};
    mediaserver::scene::MeshInstance active{};
    active.source = doc::NodeId{100U};
    active.world = mediaserver::scene::identity();
    active.normal = mediaserver::scene::identity();
    source.instances.push_back(active);
    mediaserver::scene::MeshInstance disconnected = active;
    disconnected.source = doc::NodeId{999U};
    source.instances.push_back(disconnected);

    const auto result = compositor::apply(graph, source);
    const auto untouched = std::find_if(
        result.instances.begin(), result.instances.end(),
        [](const auto& value) { return value.source == doc::NodeId{999U}; });
    COMPOSITOR_CHECK(untouched != result.instances.end());
    COMPOSITOR_CHECK(!untouched->style.effects.brightness_enabled);
    COMPOSITOR_CHECK(untouched->style.effects.brightness == 1.0F);
}

// Two scenes, each with a complete one-layer graph, plus one legacy graph node
// carrying no owner at all. Scene 10 is the lowest-id stage, so the legacy node
// belongs to it - which is where a project authored before graphs were scoped
// already composited it.
[[nodiscard]] std::vector<doc::Node> two_scene_graph() {
    const auto owned = [](core::u64 id, std::string_view type, core::u64 scene,
                          std::initializer_list<Property> extra = {}) {
        doc::Node result = node(id, type, extra);
        result.properties.insert_or_assign(
            std::string{schema::scene_id},
            doc::PropertyValue{static_cast<std::int64_t>(scene)});
        return result;
    };
    const auto owned_layer = [&owned](core::u64 id, core::u64 scene_node,
                                      core::u64 scene) {
        return owned(id, schema::scene_layer_type, scene,
                     {whole(schema::scene_node,
                            static_cast<std::int64_t>(scene_node))});
    };
    return {
        node(10U, "scene.stage"),
        node(20U, "scene.stage"),
        node(100U, "scene.object"),
        node(200U, "scene.object"),
        owned_layer(1U, 100U, 10U),
        owned(2U, schema::output_type, 10U),
        owned(3U, schema::blur_type, 10U, {real(schema::radius, 4.0)}),
        owned_layer(4U, 200U, 20U),
        owned(5U, schema::output_type, 20U),
        // No scene_id: legacy content, and therefore scene 10's.
        node(6U, schema::brightness_type, {real(schema::amount, 0.5)}),
        edge(30U, 3U, 0U, 1U, 0U),
        edge(31U, 1U, 0U, 2U, 0U),
        edge(32U, 4U, 0U, 5U, 0U),
    };
}

void a_graph_belongs_to_one_scene() {
    compositor::CompositorModel model;
    model.reset(root_of(two_scene_graph()));

    const auto program = model.evaluate(doc::NodeId{10U}, doc::NodeId{10U});
    COMPOSITOR_CHECK(program.valid());
    COMPOSITOR_CHECK(program.nodes.size() == 4U);
    COMPOSITOR_CHECK(program.node(doc::NodeId{1U}) != nullptr);
    COMPOSITOR_CHECK(program.node(doc::NodeId{4U}) == nullptr);
    // The unowned legacy node lands in the default scene, not in both.
    COMPOSITOR_CHECK(program.node(doc::NodeId{6U}) != nullptr);
    // Only edges whose endpoints are both in scope survive.
    COMPOSITOR_CHECK(program.edges.size() == 2U);

    const auto preview = model.evaluate(doc::NodeId{20U}, doc::NodeId{10U});
    COMPOSITOR_CHECK(preview.valid());
    COMPOSITOR_CHECK(preview.nodes.size() == 2U);
    COMPOSITOR_CHECK(preview.node(doc::NodeId{4U}) != nullptr);
    COMPOSITOR_CHECK(preview.node(doc::NodeId{6U}) == nullptr);
    COMPOSITOR_CHECK(preview.edges.size() == 1U);

    // Each scope carries exactly one Output, so neither is diagnosed for the
    // other scene's - the whole-document view would report a duplicate.
    COMPOSITOR_CHECK(!has_diagnostic(program,
                                     compositor::DiagnosticCode::DuplicateOutput,
                                     doc::NodeId{5U}));
    COMPOSITOR_CHECK(has_diagnostic(model.evaluate(),
                                    compositor::DiagnosticCode::DuplicateOutput,
                                    doc::NodeId{5U}));
}

void editing_one_scene_graph_leaves_the_other_untouched() {
    const auto nodes = two_scene_graph();
    doc::Root root = root_of(nodes);
    compositor::CompositorModel model;
    model.reset(root);
    const auto before = model.evaluate(doc::NodeId{10U}, doc::NodeId{10U});

    // Retune the blur that belongs to scene 20 only.
    doc::Node retuned = node(7U, schema::blur_type, {real(schema::radius, 9.0)});
    retuned.properties.insert_or_assign(std::string{schema::scene_id},
                                        doc::PropertyValue{std::int64_t{20}});
    doc::Root edited = root.set(retuned);
    edited = edited.set(
        node(33U, schema::edge_type,
             {whole(schema::from_node, 7), whole(schema::from_port, 0),
              whole(schema::to_node, 4), whole(schema::to_port, 0)}));
    model.apply(doc::diff(root, edited));

    const auto after = model.evaluate(doc::NodeId{10U}, doc::NodeId{10U});
    COMPOSITOR_CHECK(after.nodes == before.nodes);
    COMPOSITOR_CHECK(after.edges == before.edges);
    COMPOSITOR_CHECK(after.plan == before.plan);

    const auto preview = model.evaluate(doc::NodeId{20U}, doc::NodeId{10U});
    COMPOSITOR_CHECK(preview.node(doc::NodeId{7U}) != nullptr);
    COMPOSITOR_CHECK(preview.edges.size() == 2U);
}

void a_scene_copy_carries_its_graph() {
    const doc::Root root = root_of(two_scene_graph());
    // The scene layer copies drawable 100 to 500 the way a scene clone does.
    const std::array<std::pair<doc::NodeId, doc::NodeId>, 1U> elements{
        {{doc::NodeId{100U}, doc::NodeId{500U}}}};
    auto planned = compositor::plan_clone_graph(
        root, doc::NodeId{10U}, doc::NodeId{400U}, doc::NodeId{10U}, elements,
        doc::NodeId{1'000U});
    COMPOSITOR_CHECK(static_cast<bool>(planned));
    const compositor::GraphClonePlan plan = std::move(planned).value();
    // Scene 10 owns three graph nodes plus the legacy brightness, and the two
    // edges that join them.
    COMPOSITOR_CHECK(plan.puts.size() == 6U);

    // The scene clone that produced `elements` also created the copied
    // drawable; without it the copied layer would reference nothing.
    doc::Root copied = root.set(node(500U, "scene.object"));
    for (const doc::Node& put : plan.puts) {
        COMPOSITOR_CHECK(!root.contains(put.id));
        copied = copied.set(put);
    }
    compositor::CompositorModel model;
    model.reset(copied);

    const auto source = model.evaluate(doc::NodeId{10U}, doc::NodeId{10U});
    const auto copy = model.evaluate(doc::NodeId{400U}, doc::NodeId{10U});
    COMPOSITOR_CHECK(copy.valid());
    COMPOSITOR_CHECK(copy.nodes.size() == source.nodes.size());
    COMPOSITOR_CHECK(copy.edges.size() == source.edges.size());
    COMPOSITOR_CHECK(copy.plan.steps.size() == source.plan.steps.size());

    // The copied layer points at the copied drawable, not the original.
    const auto layer = std::find_if(
        copy.nodes.begin(), copy.nodes.end(),
        [](const compositor::GraphNode& value) {
            return value.kind == compositor::NodeKind::SceneLayer;
        });
    COMPOSITOR_CHECK(layer != copy.nodes.end());
    COMPOSITOR_CHECK(
        std::get<compositor::SceneLayerParameters>(layer->parameters).scene_node ==
        doc::NodeId{500U});

    // The scene the copy came from is bit-identical to what it was.
    compositor::CompositorModel original;
    original.reset(root);
    COMPOSITOR_CHECK(
        original.evaluate(doc::NodeId{10U}, doc::NodeId{10U}).plan ==
        source.plan);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 15U> cases{{
    {"a graph belongs to one scene", &a_graph_belongs_to_one_scene},
    {"editing one scene graph leaves the other untouched",
     &editing_one_scene_graph_leaves_the_other_untouched},
    {"a scene copy carries its graph", &a_scene_copy_carries_its_graph},
    {"typed ports describe the editor contract",
     &typed_ports_describe_the_editor_contract},
    {"shared blur and nine-angle array produce an explicit plan",
     &shared_blur_and_nine_angle_array_produce_an_explicit_plan},
    {"invalid edges are diagnosed and never partially executed",
     &invalid_edges_are_diagnosed_and_never_partially_executed},
    {"cycles name only cycle members, not downstream output",
     &cycles_name_only_cycle_members_not_downstream_output},
    {"scene references and required outputs are validated",
     &scene_references_and_required_outputs_are_validated},
    {"a second output is diagnosed before render planning",
     &a_second_output_is_diagnosed_before_render_planning},
    {"checkpoint and streamed deltas are identical",
     &checkpoint_and_streamed_deltas_are_identical},
    {"every authored control changes the digest",
     &every_authored_control_changes_the_digest},
    {"malformed values and capacity are bounded",
     &malformed_values_and_capacity_are_bounded},
    {"a valid graph modifies only its referenced scene layers",
     &a_valid_graph_modifies_only_its_referenced_scene_layers},
    {"disconnected scene layers never modify Program",
     &disconnected_scene_layers_never_modify_program},
    {"an empty document is an empty valid plan",
     [] {
         compositor::CompositorModel model;
         model.reset(doc::Root{});
         const auto graph = model.evaluate();
         COMPOSITOR_CHECK(graph.valid());
         COMPOSITOR_CHECK(graph.nodes.empty());
         COMPOSITOR_CHECK(graph.plan.steps.empty());
     }},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const Case& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
