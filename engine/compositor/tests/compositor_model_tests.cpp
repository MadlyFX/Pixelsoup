#include <mediaserver/compositor/compositor_model.hpp>

#include <mediaserver/compositor/compositor_schema.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <limits>
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
namespace scene = mediaserver::scene;

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

[[nodiscard]] doc::Node stage(core::u64 id, core::u32 width,
                              core::u32 height) {
    return node(id, scene::schema::stage_type,
                {whole(scene::schema::canvas_width, width),
                 whole(scene::schema::canvas_height, height)});
}

[[nodiscard]] doc::Node effect_settings(core::u64 id, core::u32 taps,
                                        core::u32 passes) {
    return node(id, scene::schema::settings_type,
                {whole(scene::schema::blur_taps, taps),
                 whole(scene::schema::blur_passes, passes)});
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

[[nodiscard]] doc::Root root_of(const std::vector<doc::Node>& authored) {
    const std::vector<doc::Node> nodes = owned_by_the_only_stage(authored);
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
        edge(20U, 1U, 0U, 3U, 2U),
        edge(21U, 1U, 0U, 4U, 2U),
        edge(22U, 2U, 0U, 3U, 1U),
        edge(23U, 2U, 0U, 4U, 1U),
        edge(24U, 3U, 0U, 5U, 0U, 10),
        edge(25U, 4U, 0U, 5U, 0U, -2),
        edge(26U, 5U, 0U, 6U, 0U),
    };
}

[[nodiscard]] std::vector<doc::Node> saturation_graph(double amount = 1.0) {
    return {
        node(100U, "scene.object"),
        node(1U, schema::saturation_type, {real(schema::amount, amount)}),
        scene_layer(2U, 100U),
        node(3U, schema::output_type),
        edge(10U, 1U, 0U, 2U, 0U),
        edge(11U, 2U, 0U, 3U, 0U),
    };
}

[[nodiscard]] doc::Node levels_node(
    core::u64 id, const scene::LevelsParameters& levels) {
    return node(
        id,
        schema::levels_type,
        {real(schema::levels_master_input_black, levels.master.input_black),
         real(schema::levels_master_input_white, levels.master.input_white),
         real(schema::levels_master_gamma, levels.master.gamma),
         real(schema::levels_master_output_black, levels.master.output_black),
         real(schema::levels_master_output_white, levels.master.output_white),
         real(schema::levels_red_input_black, levels.red.input_black),
         real(schema::levels_red_input_white, levels.red.input_white),
         real(schema::levels_red_gamma, levels.red.gamma),
         real(schema::levels_red_output_black, levels.red.output_black),
         real(schema::levels_red_output_white, levels.red.output_white),
         real(schema::levels_green_input_black, levels.green.input_black),
         real(schema::levels_green_input_white, levels.green.input_white),
         real(schema::levels_green_gamma, levels.green.gamma),
         real(schema::levels_green_output_black, levels.green.output_black),
         real(schema::levels_green_output_white, levels.green.output_white),
         real(schema::levels_blue_input_black, levels.blue.input_black),
         real(schema::levels_blue_input_white, levels.blue.input_white),
         real(schema::levels_blue_gamma, levels.blue.gamma),
         real(schema::levels_blue_output_black, levels.blue.output_black),
         real(schema::levels_blue_output_white, levels.blue.output_white)});
}

[[nodiscard]] doc::Node lift_gamma_gain_node(
    core::u64 id, const scene::LiftGammaGainParameters& parameters) {
    return node(
        id,
        schema::lift_gamma_gain_type,
        {real(schema::lift_r, parameters.lift.x),
         real(schema::lift_g, parameters.lift.y),
         real(schema::lift_b, parameters.lift.z),
         real(schema::gamma_r, parameters.gamma.x),
         real(schema::gamma_g, parameters.gamma.y),
         real(schema::gamma_b, parameters.gamma.z),
         real(schema::gain_r, parameters.gain.x),
         real(schema::gain_g, parameters.gain.y),
         real(schema::gain_b, parameters.gain.z)});
}

[[nodiscard]] doc::Node contrast_pivot_node(
    core::u64 id, const scene::ContrastPivotParameters& parameters) {
    return node(
        id,
        schema::contrast_pivot_type,
        {real(schema::contrast, parameters.contrast),
         real(schema::pivot, parameters.pivot)});
}

[[nodiscard]] doc::Node color_matrix_node(
    core::u64 id, const scene::ColorMatrixParameters& parameters) {
    return node(
        id,
        schema::color_matrix_type,
        {real(schema::m00, parameters.matrix[0U]),
         real(schema::m01, parameters.matrix[1U]),
         real(schema::m02, parameters.matrix[2U]),
         real(schema::m10, parameters.matrix[3U]),
         real(schema::m11, parameters.matrix[4U]),
         real(schema::m12, parameters.matrix[5U]),
         real(schema::m20, parameters.matrix[6U]),
         real(schema::m21, parameters.matrix[7U]),
         real(schema::m22, parameters.matrix[8U]),
         real(schema::offset_r, parameters.offset.x),
         real(schema::offset_g, parameters.offset.y),
         real(schema::offset_b, parameters.offset.z)});
}

[[nodiscard]] doc::Node white_balance_node(
    core::u64 id, const scene::WhiteBalanceParameters& parameters) {
    return node(
        id,
        schema::white_balance_type,
        {real(schema::temperature_kelvin, parameters.temperature_kelvin),
         real(schema::tint, parameters.tint)});
}

void add_curve_properties(doc::Node& built, std::string_view prefix,
                          const scene::CurveChannelParameters& channel) {
    for (std::size_t index = 0U; index < channel.points.size(); ++index) {
        const std::string base =
            std::string{prefix} + std::to_string(index);
        built.properties.emplace(
            base + std::string{schema::curve_x_suffix},
            doc::PropertyValue{static_cast<double>(channel.points[index].x)});
        built.properties.emplace(
            base + std::string{schema::curve_y_suffix},
            doc::PropertyValue{static_cast<double>(channel.points[index].y)});
    }
}

[[nodiscard]] doc::Node curves_node(
    core::u64 id, const scene::CurvesParameters& parameters) {
    doc::Node built = node(id, schema::curves_type);
    add_curve_properties(built, schema::curve_master_prefix,
                         parameters.master);
    if (parameters.red.has_value()) {
        add_curve_properties(built, schema::curve_red_prefix,
                             *parameters.red);
    }
    if (parameters.green.has_value()) {
        add_curve_properties(built, schema::curve_green_prefix,
                             *parameters.green);
    }
    if (parameters.blue.has_value()) {
        add_curve_properties(built, schema::curve_blue_prefix,
                             *parameters.blue);
    }
    return built;
}

[[nodiscard]] doc::Node film_grain_node(
    core::u64 id, const scene::FilmGrainParameters& parameters) {
    return node(
        id, schema::film_grain_type,
        {real(schema::amount, parameters.amount),
         real(schema::pixel_scale, parameters.pixel_scale),
         real(schema::chroma_ratio, parameters.chroma_ratio),
         whole(schema::seed, static_cast<std::int64_t>(parameters.seed)),
         text(schema::mode,
              parameters.mode == scene::FilmGrainMode::RGB
                  ? schema::rgb_mode
                  : schema::monochrome_mode)});
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
    // EFFECTS, then FILTERS (numbered 2 so INSTANCES keeps 1), then
    // INSTANCES.
    COMPOSITOR_CHECK(layer_inputs.size() == 3U);
    COMPOSITOR_CHECK(layer_inputs[0U].index == 0U);
    COMPOSITOR_CHECK(layer_inputs[0U].name == "EFFECTS");
    COMPOSITOR_CHECK(layer_inputs[0U].type ==
                     compositor::PortValueType::Effect);
    COMPOSITOR_CHECK(layer_inputs[0U].multiple);
    COMPOSITOR_CHECK(layer_inputs[1U].index == 2U);
    COMPOSITOR_CHECK(layer_inputs[1U].name == "FILTERS");
    COMPOSITOR_CHECK(layer_inputs[1U].type ==
                     compositor::PortValueType::Filter);
    COMPOSITOR_CHECK(layer_inputs[1U].multiple);
    COMPOSITOR_CHECK(layer_inputs[2U].index == 1U);
    COMPOSITOR_CHECK(layer_inputs[2U].type ==
                     compositor::PortValueType::TransformSet);
    COMPOSITOR_CHECK(!layer_inputs[2U].multiple);

    const auto blur_outputs =
        compositor::output_ports(compositor::NodeKind::Blur);
    COMPOSITOR_CHECK(blur_outputs.size() == 1U);
    COMPOSITOR_CHECK(blur_outputs.front().type ==
                     compositor::PortValueType::Filter);
    COMPOSITOR_CHECK(blur_outputs.front().multiple);

    const auto saturation_outputs =
        compositor::output_ports(compositor::NodeKind::Saturation);
    COMPOSITOR_CHECK(saturation_outputs.size() == 1U);
    COMPOSITOR_CHECK(saturation_outputs.front().type ==
                     compositor::PortValueType::Effect);
    COMPOSITOR_CHECK(saturation_outputs.front().multiple);

    const auto levels_outputs =
        compositor::output_ports(compositor::NodeKind::Levels);
    COMPOSITOR_CHECK(levels_outputs.size() == 1U);
    COMPOSITOR_CHECK(levels_outputs.front().type ==
                     compositor::PortValueType::Effect);
    COMPOSITOR_CHECK(levels_outputs.front().multiple);

    const auto composite_inputs =
        compositor::input_ports(compositor::NodeKind::Composite);
    COMPOSITOR_CHECK(composite_inputs.front().type ==
                     compositor::PortValueType::Image);
    COMPOSITOR_CHECK(composite_inputs.front().multiple);
    COMPOSITOR_CHECK(composite_inputs.front().required);

    const auto car_inputs =
            compositor::input_ports(compositor::NodeKind::LedCarProcess);
    COMPOSITOR_CHECK(car_inputs.size() == 11U);
    for (core::u8 index = 0U; index < 9U; ++index) {
        COMPOSITOR_CHECK(car_inputs[index].index == index);
        COMPOSITOR_CHECK(car_inputs[index].type ==
                         compositor::PortValueType::Image);
        COMPOSITOR_CHECK(!car_inputs[index].required);
        COMPOSITOR_CHECK(!car_inputs[index].multiple);
    }
    COMPOSITOR_CHECK(car_inputs[8U].name == "CEILING REFLECTION");
    COMPOSITOR_CHECK(car_inputs[9U].type == compositor::PortValueType::Effect);
    COMPOSITOR_CHECK(car_inputs[9U].multiple);
    COMPOSITOR_CHECK(!car_inputs[9U].required);
    COMPOSITOR_CHECK(car_inputs[10U].index == 10U);
    COMPOSITOR_CHECK(car_inputs[10U].type == compositor::PortValueType::Filter);
    COMPOSITOR_CHECK(car_inputs[10U].multiple);
    const auto car_outputs =
            compositor::output_ports(compositor::NodeKind::LedCarProcess);
    COMPOSITOR_CHECK(car_outputs.size() == 1U);
    COMPOSITOR_CHECK(car_outputs.front().type ==
                     compositor::PortValueType::Image);
}

void persistent_node_catalog_includes_led_car_process() {
    COMPOSITOR_CHECK(schema::is_node_type(schema::led_car_process_type));
    COMPOSITOR_CHECK(schema::is_node_type(schema::radial_array_type));
    COMPOSITOR_CHECK(schema::is_node_type(schema::output_type));
    COMPOSITOR_CHECK(!schema::is_node_type(schema::edge_type));
    COMPOSITOR_CHECK(!schema::is_node_type(scene::schema::object_type));
}

void saturation_is_typed_bounded_and_digested() {
    compositor::CompositorModel model;
    model.reset(root_of(saturation_graph(2.25)));
    const compositor::CompositorGraph graph = model.evaluate();
    COMPOSITOR_CHECK(graph.valid());
    const compositor::GraphNode* const saturation =
        graph.node(doc::NodeId{1U});
    COMPOSITOR_CHECK(saturation != nullptr);
    COMPOSITOR_CHECK(saturation->kind == compositor::NodeKind::Saturation);
    COMPOSITOR_CHECK(std::get<compositor::SaturationParameters>(
                         saturation->parameters)
                         .amount == 2.25F);

    compositor::CompositorModel identity;
    identity.reset(root_of(saturation_graph(1.0)));
    COMPOSITOR_CHECK(compositor::digest(identity.evaluate()) !=
                     compositor::digest(graph));

    compositor::CompositorModel malformed;
    malformed.reset(root_of({
        node(1U, schema::saturation_type, {real(schema::amount, -0.01)}),
        node(2U, schema::saturation_type,
             {real(schema::amount,
                   static_cast<double>(compositor::max_saturation) + 0.01)}),
        node(3U, schema::saturation_type,
             {real(schema::amount,
                   std::numeric_limits<double>::infinity())}),
    }));
    COMPOSITOR_CHECK(malformed.counters().rejected == 3U);
    COMPOSITOR_CHECK(malformed.size() == 0U);
}

void saturation_identity_is_inert_and_nonidentity_folds_once() {
    scene::Scene source{};
    scene::MeshInstance mesh{};
    mesh.source = doc::NodeId{100U};
    mesh.world = scene::identity();
    mesh.normal = scene::identity();
    source.instances.push_back(mesh);

    compositor::CompositorModel identity;
    identity.reset(root_of(saturation_graph(1.0)));
    const scene::EffectStack before = source.instances.front().style.effects;
    const scene::Scene unchanged = compositor::apply(identity.evaluate(), source);
    COMPOSITOR_CHECK(unchanged.instances.size() == 1U);
    COMPOSITOR_CHECK(unchanged.instances.front().style.effects == before);

    // A disabled stored amount is dormant. Activating a graph Saturation must
    // compose from identity rather than from that unauthored rendering state.
    scene::Scene dormant = source;
    dormant.instances.front().style.effects.saturation = 3.0F;
    compositor::CompositorModel half;
    half.reset(root_of(saturation_graph(0.5)));
    const scene::Scene activated = compositor::apply(half.evaluate(), dormant);
    COMPOSITOR_CHECK(
        activated.instances.front().style.effects.saturation_enabled);
    COMPOSITOR_CHECK(
        activated.instances.front().style.effects.saturation == 0.5F);

    scene::Scene dormant_stack = source;
    dormant_stack.instances.front().style.effects.brightness = 0.25F;
    dormant_stack.instances.front().style.effects.saturation = 3.0F;
    dormant_stack.instances.front().style.effects.blur_radius = 40.0F;
    compositor::CompositorModel activated_stack;
    activated_stack.reset(root_of({
        node(100U, "scene.object"),
        node(1U, schema::brightness_type, {real(schema::amount, 2.0)}),
        node(2U, schema::saturation_type, {real(schema::amount, 0.5)}),
        node(3U, schema::blur_type, {real(schema::radius, 3.0)}),
        scene_layer(4U, 100U),
        node(5U, schema::output_type),
        edge(10U, 1U, 0U, 4U, 0U, 0),
        edge(11U, 2U, 0U, 4U, 0U, 1),
        edge(12U, 3U, 0U, 4U, 2U, 2),
        edge(13U, 4U, 0U, 5U, 0U),
    }));
    const scene::Scene activated_all =
        compositor::apply(activated_stack.evaluate(), dormant_stack);
    const scene::EffectStack& activated_effects =
        activated_all.instances.front().style.effects;
    COMPOSITOR_CHECK(activated_effects.brightness == 2.0F);
    COMPOSITOR_CHECK(activated_effects.saturation == 0.5F);
    COMPOSITOR_CHECK(activated_effects.blur_radius == 3.0F);

    scene::Scene gamma_source = source;
    gamma_source.effect_settings.brightness_gamma = 2.0F;
    const scene::Scene gamma_adjusted =
        compositor::apply(activated_stack.evaluate(), gamma_source);
    COMPOSITOR_CHECK(
        gamma_adjusted.instances.front().style.effects.brightness == 4.0F);

    scene::Scene first_class_brightness = source;
    auto& raw_brightness =
        first_class_brightness.instances.front().style.effects;
    raw_brightness.brightness_enabled = true;
    raw_brightness.brightness = compositor::max_brightness;
    raw_brightness.brightness_product_valid = true;
    raw_brightness.brightness_product = 256.0;
    compositor::CompositorModel hundredth;
    hundredth.reset(root_of({
        node(100U, "scene.object"),
        node(1U, schema::brightness_type, {real(schema::amount, 0.01)}),
        scene_layer(2U, 100U),
        node(3U, schema::output_type),
        edge(10U, 1U, 0U, 2U, 0U),
        edge(11U, 2U, 0U, 3U, 0U),
    }));
    const scene::Scene brightness_combined =
        compositor::apply(hundredth.evaluate(), first_class_brightness);
    const auto& combined_brightness =
        brightness_combined.instances.front().style.effects;
    COMPOSITOR_CHECK(
        std::abs(combined_brightness.brightness - 2.56F) < 0.0001F);
    COMPOSITOR_CHECK(combined_brightness.brightness_product_valid);
    COMPOSITOR_CHECK(
        std::abs(combined_brightness.brightness_product - 2.56) < 0.0001);

    // Enabled malformed scene data follows the same identity normalization as
    // SceneModel and the render registry before a graph factor is composed.
    scene::Scene malformed_base = source;
    malformed_base.instances.front().style.effects.brightness_enabled = true;
    malformed_base.instances.front().style.effects.brightness =
        std::numeric_limits<float>::quiet_NaN();
    malformed_base.instances.front().style.effects.saturation_enabled = true;
    malformed_base.instances.front().style.effects.saturation =
        std::numeric_limits<float>::quiet_NaN();
    malformed_base.instances.front().style.effects.blur_enabled = true;
    malformed_base.instances.front().style.effects.blur_radius =
        std::numeric_limits<float>::quiet_NaN();
    const scene::Scene normalized_base =
        compositor::apply(activated_stack.evaluate(), malformed_base);
    COMPOSITOR_CHECK(
        normalized_base.instances.front().style.effects.brightness == 2.0F);
    COMPOSITOR_CHECK(
        normalized_base.instances.front().style.effects.saturation == 0.5F);
    COMPOSITOR_CHECK(
        normalized_base.instances.front().style.effects.blur_radius == 3.0F);

    // Multiplication is accumulated before the one final clamp. Clamping after
    // every node would turn 4 * 4 * 0.25 into 1 instead of the correct 4.
    compositor::CompositorModel folded;
    folded.reset(root_of({
        node(100U, "scene.object"),
        node(1U, schema::saturation_type, {real(schema::amount, 4.0)}),
        node(2U, schema::saturation_type, {real(schema::amount, 4.0)}),
        node(3U, schema::saturation_type, {real(schema::amount, 0.25)}),
        scene_layer(4U, 100U),
        node(5U, schema::output_type),
        edge(10U, 1U, 0U, 4U, 0U, 0),
        edge(11U, 2U, 0U, 4U, 0U, 1),
        edge(12U, 3U, 0U, 4U, 0U, 2),
        edge(13U, 4U, 0U, 5U, 0U),
    }));
    const scene::Scene saturated = compositor::apply(folded.evaluate(), source);
    const scene::EffectStack& effects =
        saturated.instances.front().style.effects;
    COMPOSITOR_CHECK(effects.saturation_enabled);
    COMPOSITOR_CHECK(effects.saturation == compositor::max_saturation);

    // A first-class Scene boundary may already have clamped its immediate
    // shader value while retaining a larger raw product. The downstream graph
    // continues that raw product before the one final clamp.
    scene::Scene first_class = source;
    auto& first_class_effects =
        first_class.instances.front().style.effects;
    first_class_effects.saturation_enabled = true;
    first_class_effects.saturation = scene::max_saturation;
    first_class_effects.saturation_product_valid = true;
    first_class_effects.saturation_product = 16.0;
    compositor::CompositorModel quarter;
    quarter.reset(root_of(saturation_graph(0.25)));
    const scene::Scene combined =
        compositor::apply(quarter.evaluate(), first_class);
    const scene::EffectStack& combined_effects =
        combined.instances.front().style.effects;
    COMPOSITOR_CHECK(combined_effects.saturation == scene::max_saturation);
    COMPOSITOR_CHECK(combined_effects.saturation_product_valid);
    COMPOSITOR_CHECK(combined_effects.saturation_product == 4.0);
}

void saturation_checkpoint_and_streamed_deltas_match() {
    const auto nodes = saturation_graph(1.75);
    compositor::CompositorModel checkpointed;
    checkpointed.reset(root_of(nodes));

    compositor::CompositorModel streamed;
    doc::Root running;
    for (auto iterator = nodes.rbegin(); iterator != nodes.rend(); ++iterator) {
        const doc::Root next = running.set(*iterator);
        streamed.apply(doc::diff(running, next));
        running = next;
    }
    COMPOSITOR_CHECK(checkpointed.evaluate() == streamed.evaluate());
    COMPOSITOR_CHECK(compositor::digest(checkpointed.evaluate()) ==
                     compositor::digest(streamed.evaluate()));
    COMPOSITOR_CHECK(checkpointed.counters() == streamed.counters());
}

void levels_is_typed_ordered_digested_and_replicated() {
    scene::LevelsParameters levels{};
    levels.master.gamma = 2.0F;
    levels.red.input_black = 0.1F;
    levels.red.output_black = 0.2F;
    levels.green.input_white = 0.9F;
    levels.blue.output_white = 0.8F;
    const std::vector<doc::Node> nodes{
        node(100U, "scene.object"),
        node(1U, schema::brightness_type, {real(schema::amount, 2.0)}),
        levels_node(2U, levels),
        node(3U, schema::saturation_type, {real(schema::amount, 0.5)}),
        scene_layer(4U, 100U),
        node(5U, schema::output_type),
        edge(10U, 1U, 0U, 4U, 0U, 0),
        edge(11U, 2U, 0U, 4U, 0U, 1),
        edge(12U, 3U, 0U, 4U, 0U, 2),
        edge(13U, 4U, 0U, 5U, 0U),
    };

    compositor::CompositorModel checkpointed;
    checkpointed.reset(root_of(nodes));
    const compositor::CompositorGraph graph = checkpointed.evaluate();
    COMPOSITOR_CHECK(graph.valid());
    const compositor::GraphNode* const typed = graph.node(doc::NodeId{2U});
    COMPOSITOR_CHECK(typed != nullptr);
    COMPOSITOR_CHECK(typed->kind == compositor::NodeKind::Levels);
    COMPOSITOR_CHECK(
        std::get<compositor::LevelsParameters>(typed->parameters) == levels);
    COMPOSITOR_CHECK(!compositor::effect_node_is_identity(*typed));

    scene::Scene source{};
    scene::MeshInstance instance{};
    instance.source = doc::NodeId{100U};
    instance.world = scene::identity();
    instance.normal = scene::identity();
    source.instances.push_back(instance);
    const scene::Scene adjusted = compositor::apply(graph, source);
    const auto& program =
        adjusted.instances.front().style.effects.inline_operations;
    COMPOSITOR_CHECK(program.size() == 3U);
    COMPOSITOR_CHECK(program[0U].kind == scene::EffectKind::Brightness);
    COMPOSITOR_CHECK(program[1U].kind == scene::EffectKind::Levels);
    COMPOSITOR_CHECK(program[1U].levels == levels);
    COMPOSITOR_CHECK(program[2U].kind == scene::EffectKind::Saturation);

    compositor::CompositorModel streamed;
    doc::Root running;
    for (auto iterator = nodes.rbegin(); iterator != nodes.rend(); ++iterator) {
        const doc::Root next = running.set(*iterator);
        streamed.apply(doc::diff(running, next));
        running = next;
    }
    COMPOSITOR_CHECK(streamed.evaluate() == graph);
    COMPOSITOR_CHECK(compositor::digest(streamed.evaluate()) ==
                     compositor::digest(graph));

    scene::LevelsParameters changed = levels;
    changed.master.gamma = 3.0F;
    compositor::CompositorModel different;
    auto changed_nodes = nodes;
    changed_nodes[2U] = levels_node(2U, changed);
    different.reset(root_of(changed_nodes));
    COMPOSITOR_CHECK(compositor::digest(different.evaluate()) !=
                     compositor::digest(graph));

    scene::LevelsParameters equal = levels;
    equal.red.input_white = equal.red.input_black;
    scene::LevelsParameters reversed = levels;
    reversed.blue.output_black = 0.9F;
    reversed.blue.output_white = 0.1F;
    compositor::CompositorModel malformed;
    malformed.reset(root_of({levels_node(20U, equal)}));
    COMPOSITOR_CHECK(malformed.counters().rejected == 1U);
    COMPOSITOR_CHECK(malformed.size() == 0U);

    compositor::CompositorModel inversion;
    inversion.reset(root_of({levels_node(21U, reversed)}));
    COMPOSITOR_CHECK(inversion.counters().rejected == 0U);
    const auto inverted_graph = inversion.evaluate();
    COMPOSITOR_CHECK(inverted_graph.nodes.size() == 1U);
    COMPOSITOR_CHECK(
        std::get<compositor::LevelsParameters>(
            inverted_graph.nodes.front().parameters).blue == reversed.blue);
}

void lift_gamma_gain_is_typed_ordered_and_replicated() {
    scene::LiftGammaGainParameters grade{};
    grade.lift = scene::Vec3{0.1F, -0.1F, 0.2F};
    grade.gamma = scene::Vec3{1.2F, 0.8F, 2.0F};
    grade.gain = scene::Vec3{1.5F, 0.5F, 2.0F};
    scene::LevelsParameters levels{};
    levels.master.gamma = 1.5F;
    const std::vector<doc::Node> nodes{
        node(100U, "scene.object"),
        node(1U, schema::brightness_type, {real(schema::amount, 0.5)}),
        lift_gamma_gain_node(2U, grade),
        levels_node(3U, levels),
        node(4U, schema::saturation_type, {real(schema::amount, 0.5)}),
        scene_layer(5U, 100U),
        node(6U, schema::output_type),
        edge(10U, 1U, 0U, 5U, 0U, 0),
        edge(11U, 2U, 0U, 5U, 0U, 1),
        edge(12U, 3U, 0U, 5U, 0U, 2),
        edge(13U, 4U, 0U, 5U, 0U, 3),
        edge(14U, 5U, 0U, 6U, 0U),
    };

    compositor::CompositorModel checkpointed;
    checkpointed.reset(root_of(nodes));
    const auto graph = checkpointed.evaluate();
    COMPOSITOR_CHECK(graph.valid());
    const auto* const typed = graph.node(doc::NodeId{2U});
    COMPOSITOR_CHECK(typed != nullptr);
    COMPOSITOR_CHECK(typed->kind == compositor::NodeKind::LiftGammaGain);
    COMPOSITOR_CHECK(
        std::get<compositor::LiftGammaGainParameters>(typed->parameters)
        == grade);
    COMPOSITOR_CHECK(!compositor::effect_node_is_identity(*typed));

    scene::Scene source{};
    scene::MeshInstance instance{};
    instance.source = doc::NodeId{100U};
    source.instances.push_back(instance);
    const auto adjusted = compositor::apply(graph, source);
    const auto& program =
        adjusted.instances.front().style.effects.inline_operations;
    COMPOSITOR_CHECK(program.size() == 4U);
    COMPOSITOR_CHECK(program[0U].kind == scene::EffectKind::Brightness);
    COMPOSITOR_CHECK(program[1U].kind == scene::EffectKind::LiftGammaGain);
    COMPOSITOR_CHECK(program[1U].lift_gamma_gain == grade);
    COMPOSITOR_CHECK(program[2U].kind == scene::EffectKind::Levels);
    COMPOSITOR_CHECK(program[3U].kind == scene::EffectKind::Saturation);

    compositor::CompositorModel streamed;
    doc::Root running;
    for (auto iterator = nodes.rbegin(); iterator != nodes.rend(); ++iterator) {
        const doc::Root next = running.set(*iterator);
        streamed.apply(doc::diff(running, next));
        running = next;
    }
    COMPOSITOR_CHECK(streamed.evaluate() == graph);
    COMPOSITOR_CHECK(compositor::digest(streamed.evaluate()) ==
                     compositor::digest(graph));

    compositor::CompositorModel identity;
    identity.reset(root_of({lift_gamma_gain_node(
        20U, scene::LiftGammaGainParameters{})}));
    COMPOSITOR_CHECK(compositor::effect_node_is_identity(
        identity.evaluate().nodes.front()));

    scene::LiftGammaGainParameters bounded{};
    bounded.lift = scene::Vec3{-10.0F, 0.0F, 10.0F};
    bounded.gamma = scene::Vec3{0.0F, 1.0F, 20.0F};
    bounded.gain = scene::Vec3{-1.0F, 1.0F, 20.0F};
    compositor::CompositorModel normalized;
    normalized.reset(root_of({lift_gamma_gain_node(21U, bounded)}));
    const auto canonical = std::get<compositor::LiftGammaGainParameters>(
        normalized.evaluate().nodes.front().parameters);
    COMPOSITOR_CHECK(canonical.lift == scene::Vec3(-4.0F, 0.0F, 4.0F));
    COMPOSITOR_CHECK(canonical.gamma == scene::Vec3(0.01F, 1.0F, 10.0F));
    COMPOSITOR_CHECK(canonical.gain == scene::Vec3(0.0F, 1.0F, 16.0F));

    bounded.gain.y = std::numeric_limits<float>::quiet_NaN();
    compositor::CompositorModel malformed;
    malformed.reset(root_of({lift_gamma_gain_node(22U, bounded)}));
    COMPOSITOR_CHECK(malformed.counters().rejected == 1U);
    COMPOSITOR_CHECK(malformed.size() == 0U);
}

void contrast_pivot_is_typed_ordered_digested_and_replicated() {
    scene::LevelsParameters levels{};
    levels.master.gamma = 1.5F;
    const scene::ContrastPivotParameters contrast{1.6F, 0.35F};
    scene::LiftGammaGainParameters lift_gamma_gain{};
    lift_gamma_gain.lift.x = 0.1F;
    const std::vector<doc::Node> nodes{
        node(100U, "scene.object"),
        node(1U, schema::brightness_type, {real(schema::amount, 0.5)}),
        levels_node(2U, levels),
        contrast_pivot_node(3U, contrast),
        lift_gamma_gain_node(4U, lift_gamma_gain),
        scene_layer(5U, 100U),
        node(6U, schema::output_type),
        edge(10U, 1U, 0U, 5U, 0U, 0),
        edge(11U, 2U, 0U, 5U, 0U, 1),
        edge(12U, 3U, 0U, 5U, 0U, 2),
        edge(13U, 4U, 0U, 5U, 0U, 3),
        edge(14U, 5U, 0U, 6U, 0U),
    };

    compositor::CompositorModel checkpointed;
    checkpointed.reset(root_of(nodes));
    const auto graph = checkpointed.evaluate();
    COMPOSITOR_CHECK(graph.valid());
    const auto* const typed = graph.node(doc::NodeId{3U});
    COMPOSITOR_CHECK(typed != nullptr);
    COMPOSITOR_CHECK(typed->kind == compositor::NodeKind::ContrastPivot);
    COMPOSITOR_CHECK(
        std::get<compositor::ContrastPivotParameters>(typed->parameters)
        == contrast);
    COMPOSITOR_CHECK(!compositor::effect_node_is_identity(*typed));

    scene::Scene source{};
    scene::MeshInstance instance{};
    instance.source = doc::NodeId{100U};
    source.instances.push_back(instance);
    const auto adjusted = compositor::apply(graph, source);
    const auto& program =
        adjusted.instances.front().style.effects.inline_operations;
    COMPOSITOR_CHECK(program.size() == 4U);
    COMPOSITOR_CHECK(program[0U].kind == scene::EffectKind::Brightness);
    COMPOSITOR_CHECK(program[1U].kind == scene::EffectKind::Levels);
    COMPOSITOR_CHECK(program[2U].kind == scene::EffectKind::ContrastPivot);
    COMPOSITOR_CHECK(program[2U].contrast_pivot == contrast);
    COMPOSITOR_CHECK(program[3U].kind == scene::EffectKind::LiftGammaGain);

    compositor::CompositorModel streamed;
    doc::Root running;
    for (auto iterator = nodes.rbegin(); iterator != nodes.rend(); ++iterator) {
        const doc::Root next = running.set(*iterator);
        streamed.apply(doc::diff(running, next));
        running = next;
    }
    COMPOSITOR_CHECK(streamed.evaluate() == graph);
    COMPOSITOR_CHECK(compositor::digest(streamed.evaluate()) ==
                     compositor::digest(graph));

    compositor::CompositorModel identity_a;
    identity_a.reset(root_of({contrast_pivot_node(
        20U, scene::ContrastPivotParameters{1.0F, 0.2F})}));
    compositor::CompositorModel identity_b;
    identity_b.reset(root_of({contrast_pivot_node(
        20U, scene::ContrastPivotParameters{1.0F, 0.8F})}));
    const auto identity_graph_a = identity_a.evaluate();
    const auto identity_graph_b = identity_b.evaluate();
    COMPOSITOR_CHECK(compositor::effect_node_is_identity(
        identity_graph_a.nodes.front()));
    COMPOSITOR_CHECK(compositor::effect_node_is_identity(
        identity_graph_b.nodes.front()));
    COMPOSITOR_CHECK(compositor::digest(identity_graph_a) !=
                     compositor::digest(identity_graph_b));

    compositor::CompositorModel bounded;
    bounded.reset(root_of({contrast_pivot_node(
        21U, scene::ContrastPivotParameters{8.0F, -1.0F})}));
    const auto canonical = std::get<compositor::ContrastPivotParameters>(
        bounded.evaluate().nodes.front().parameters);
    COMPOSITOR_CHECK(canonical ==
                     scene::ContrastPivotParameters(4.0F, 0.0F));

    compositor::CompositorModel malformed;
    malformed.reset(root_of({contrast_pivot_node(
        22U,
        scene::ContrastPivotParameters{
            std::numeric_limits<float>::quiet_NaN(), 0.5F})}));
    COMPOSITOR_CHECK(malformed.counters().rejected == 1U);
    COMPOSITOR_CHECK(malformed.size() == 0U);
}

void color_matrix_is_typed_ordered_digested_and_replicated() {
    scene::LevelsParameters levels{};
    levels.master.gamma = 1.5F;
    scene::LiftGammaGainParameters lift_gamma_gain{};
    lift_gamma_gain.lift.x = 0.1F;
    const scene::ContrastPivotParameters contrast{1.5F, 0.4F};
    scene::ColorMatrixParameters mixer{};
    mixer.matrix = {0.0F, 1.0F, 0.0F,
                    1.0F, 0.0F, 0.0F,
                    0.0F, 0.0F, 1.0F};
    mixer.offset = scene::Vec3{0.1F, -0.1F, 0.05F};
    const std::vector<doc::Node> nodes{
        node(100U, "scene.object"),
        levels_node(1U, levels),
        lift_gamma_gain_node(2U, lift_gamma_gain),
        contrast_pivot_node(3U, contrast),
        color_matrix_node(4U, mixer),
        scene_layer(5U, 100U),
        node(6U, schema::output_type),
        edge(10U, 1U, 0U, 5U, 0U, 0),
        edge(11U, 2U, 0U, 5U, 0U, 1),
        edge(12U, 3U, 0U, 5U, 0U, 2),
        edge(13U, 4U, 0U, 5U, 0U, 3),
        edge(14U, 5U, 0U, 6U, 0U),
    };

    compositor::CompositorModel checkpointed;
    checkpointed.reset(root_of(nodes));
    const auto graph = checkpointed.evaluate();
    COMPOSITOR_CHECK(graph.valid());
    const auto* const typed = graph.node(doc::NodeId{4U});
    COMPOSITOR_CHECK(typed != nullptr);
    COMPOSITOR_CHECK(typed->kind == compositor::NodeKind::ColorMatrix);
    COMPOSITOR_CHECK(
        std::get<compositor::ColorMatrixParameters>(typed->parameters)
        == mixer);
    COMPOSITOR_CHECK(!compositor::effect_node_is_identity(*typed));

    scene::Scene source{};
    scene::MeshInstance instance{};
    instance.source = doc::NodeId{100U};
    source.instances.push_back(instance);
    const auto adjusted = compositor::apply(graph, source);
    const auto& program =
        adjusted.instances.front().style.effects.inline_operations;
    COMPOSITOR_CHECK(program.size() == 4U);
    COMPOSITOR_CHECK(program[0U].kind == scene::EffectKind::Levels);
    COMPOSITOR_CHECK(program[1U].kind == scene::EffectKind::LiftGammaGain);
    COMPOSITOR_CHECK(program[2U].kind == scene::EffectKind::ContrastPivot);
    COMPOSITOR_CHECK(program[3U].kind == scene::EffectKind::ColorMatrix);
    COMPOSITOR_CHECK(program[3U].color_matrix == mixer);

    compositor::CompositorModel streamed;
    doc::Root running;
    for (auto iterator = nodes.rbegin(); iterator != nodes.rend(); ++iterator) {
        const doc::Root next = running.set(*iterator);
        streamed.apply(doc::diff(running, next));
        running = next;
    }
    COMPOSITOR_CHECK(streamed.evaluate() == graph);
    COMPOSITOR_CHECK(compositor::digest(streamed.evaluate()) ==
                     compositor::digest(graph));

    compositor::CompositorModel identity;
    identity.reset(root_of({color_matrix_node(
        20U, scene::ColorMatrixParameters{})}));
    COMPOSITOR_CHECK(compositor::effect_node_is_identity(
        identity.evaluate().nodes.front()));

    scene::ColorMatrixParameters bounded{};
    bounded.matrix[0U] = -20.0F;
    bounded.matrix[1U] = 20.0F;
    bounded.offset = scene::Vec3{-20.0F, 0.25F, 20.0F};
    compositor::CompositorModel normalized;
    normalized.reset(root_of({color_matrix_node(21U, bounded)}));
    const auto canonical = std::get<compositor::ColorMatrixParameters>(
        normalized.evaluate().nodes.front().parameters);
    COMPOSITOR_CHECK(canonical.matrix[0U] == -16.0F);
    COMPOSITOR_CHECK(canonical.matrix[1U] == 16.0F);
    COMPOSITOR_CHECK(canonical.offset ==
                     scene::Vec3(-16.0F, 0.25F, 16.0F));

    scene::ColorMatrixParameters changed = mixer;
    changed.offset.z = 0.15F;
    compositor::CompositorModel different;
    auto changed_nodes = nodes;
    changed_nodes[4U] = color_matrix_node(4U, changed);
    different.reset(root_of(changed_nodes));
    COMPOSITOR_CHECK(compositor::digest(different.evaluate()) !=
                     compositor::digest(graph));

    bounded.matrix[4U] = std::numeric_limits<float>::quiet_NaN();
    compositor::CompositorModel malformed;
    malformed.reset(root_of({color_matrix_node(22U, bounded)}));
    COMPOSITOR_CHECK(malformed.counters().rejected == 1U);
    COMPOSITOR_CHECK(malformed.size() == 0U);
}

void white_balance_is_typed_ordered_digested_and_replicated() {
    scene::LevelsParameters levels{};
    levels.master.gamma = 1.5F;
    scene::ColorMatrixParameters mixer{};
    mixer.matrix[1U] = 0.2F;
    const scene::WhiteBalanceParameters white_balance{4'000.0F, 0.25F};
    const auto canonical = scene::normalized_white_balance(white_balance);
    COMPOSITOR_CHECK(canonical.has_value());
    const scene::ContrastPivotParameters contrast{1.5F, 0.4F};
    const std::vector<doc::Node> nodes{
        node(100U, "scene.object"),
        levels_node(1U, levels),
        color_matrix_node(2U, mixer),
        white_balance_node(3U, white_balance),
        contrast_pivot_node(4U, contrast),
        scene_layer(5U, 100U),
        node(6U, schema::output_type),
        edge(10U, 1U, 0U, 5U, 0U, 0),
        edge(11U, 2U, 0U, 5U, 0U, 1),
        edge(12U, 3U, 0U, 5U, 0U, 2),
        edge(13U, 4U, 0U, 5U, 0U, 3),
        edge(14U, 5U, 0U, 6U, 0U),
    };

    compositor::CompositorModel checkpointed;
    checkpointed.reset(root_of(nodes));
    const auto graph = checkpointed.evaluate();
    COMPOSITOR_CHECK(graph.valid());
    const auto* const typed = graph.node(doc::NodeId{3U});
    COMPOSITOR_CHECK(typed != nullptr);
    COMPOSITOR_CHECK(typed->kind == compositor::NodeKind::WhiteBalance);
    COMPOSITOR_CHECK(
        std::get<compositor::WhiteBalanceParameters>(typed->parameters)
        == *canonical);
    COMPOSITOR_CHECK(!compositor::effect_node_is_identity(*typed));

    scene::Scene source{};
    scene::MeshInstance instance{};
    instance.source = doc::NodeId{100U};
    source.instances.push_back(instance);
    const auto adjusted = compositor::apply(graph, source);
    const auto& program =
        adjusted.instances.front().style.effects.inline_operations;
    COMPOSITOR_CHECK(program.size() == 4U);
    COMPOSITOR_CHECK(program[0U].kind == scene::EffectKind::Levels);
    COMPOSITOR_CHECK(program[1U].kind == scene::EffectKind::ColorMatrix);
    COMPOSITOR_CHECK(program[2U].kind == scene::EffectKind::WhiteBalance);
    COMPOSITOR_CHECK(program[2U].white_balance == *canonical);
    COMPOSITOR_CHECK(program[3U].kind == scene::EffectKind::ContrastPivot);

    compositor::CompositorModel streamed;
    doc::Root running;
    for (auto iterator = nodes.rbegin(); iterator != nodes.rend(); ++iterator) {
        const doc::Root next = running.set(*iterator);
        streamed.apply(doc::diff(running, next));
        running = next;
    }
    COMPOSITOR_CHECK(streamed.evaluate() == graph);
    COMPOSITOR_CHECK(compositor::digest(streamed.evaluate()) ==
                     compositor::digest(graph));

    compositor::CompositorModel identity;
    identity.reset(root_of({white_balance_node(
        20U, scene::WhiteBalanceParameters{})}));
    COMPOSITOR_CHECK(compositor::effect_node_is_identity(
        identity.evaluate().nodes.front()));

    compositor::CompositorModel bounded;
    bounded.reset(root_of({white_balance_node(
        21U, scene::WhiteBalanceParameters{20'000.0F, -2.0F})}));
    const auto normalized = std::get<compositor::WhiteBalanceParameters>(
        bounded.evaluate().nodes.front().parameters);
    COMPOSITOR_CHECK(normalized.temperature_kelvin == 12'000.0F);
    COMPOSITOR_CHECK(normalized.tint == -1.0F);

    scene::WhiteBalanceParameters changed = white_balance;
    changed.tint = -0.25F;
    compositor::CompositorModel different;
    auto changed_nodes = nodes;
    changed_nodes[3U] = white_balance_node(3U, changed);
    different.reset(root_of(changed_nodes));
    COMPOSITOR_CHECK(compositor::digest(different.evaluate()) !=
                     compositor::digest(graph));

    scene::WhiteBalanceParameters malformed_parameters{};
    malformed_parameters.tint =
        std::numeric_limits<float>::quiet_NaN();
    compositor::CompositorModel malformed;
    malformed.reset(root_of({white_balance_node(
        22U, malformed_parameters)}));
    COMPOSITOR_CHECK(malformed.counters().rejected == 1U);
    COMPOSITOR_CHECK(malformed.size() == 0U);
}

void curves_are_typed_ordered_digested_and_replicated() {
    scene::CurvesParameters curves{};
    curves.master.points = {
        {0.0F, 0.0F}, {0.4F, 0.2F}, {1.0F, 1.0F}};
    curves.green = scene::CurveChannelParameters{{
        {0.0F, 0.0F}, {0.6F, 0.8F}, {1.0F, 1.0F}}};
    const auto canonical = scene::normalized_curves(curves);
    COMPOSITOR_CHECK(canonical.has_value());

    scene::LevelsParameters levels{};
    levels.master.gamma = 1.5F;
    const scene::ContrastPivotParameters contrast{1.5F, 0.4F};
    const std::vector<doc::Node> nodes{
        node(100U, "scene.object"), levels_node(1U, levels),
        curves_node(2U, curves), contrast_pivot_node(3U, contrast),
        scene_layer(4U, 100U), node(5U, schema::output_type),
        edge(10U, 1U, 0U, 4U, 0U, 0),
        edge(11U, 2U, 0U, 4U, 0U, 1),
        edge(12U, 3U, 0U, 4U, 0U, 2),
        edge(13U, 4U, 0U, 5U, 0U)};

    compositor::CompositorModel checkpointed;
    checkpointed.reset(root_of(nodes));
    const auto graph = checkpointed.evaluate();
    COMPOSITOR_CHECK(graph.valid());
    const auto* const typed = graph.node(doc::NodeId{2U});
    COMPOSITOR_CHECK(typed != nullptr);
    COMPOSITOR_CHECK(typed->kind == compositor::NodeKind::Curves);
    COMPOSITOR_CHECK(
        std::get<compositor::CurvesParameters>(typed->parameters)
        == *canonical);
    COMPOSITOR_CHECK(!compositor::effect_node_is_identity(*typed));

    scene::Scene source{};
    scene::MeshInstance instance{};
    instance.source = doc::NodeId{100U};
    source.instances.push_back(instance);
    const auto adjusted = compositor::apply(graph, source);
    const auto& program =
        adjusted.instances.front().style.effects.inline_operations;
    COMPOSITOR_CHECK(program.size() == 3U);
    COMPOSITOR_CHECK(program[0U].kind == scene::EffectKind::Levels);
    COMPOSITOR_CHECK(program[1U].kind == scene::EffectKind::Curves);
    COMPOSITOR_CHECK(program[1U].curves == *canonical);
    COMPOSITOR_CHECK(program[2U].kind == scene::EffectKind::ContrastPivot);

    compositor::CompositorModel streamed;
    doc::Root running;
    for (auto iterator = nodes.rbegin(); iterator != nodes.rend(); ++iterator) {
        const doc::Root next = running.set(*iterator);
        streamed.apply(doc::diff(running, next));
        running = next;
    }
    COMPOSITOR_CHECK(streamed.evaluate() == graph);
    COMPOSITOR_CHECK(compositor::digest(streamed.evaluate()) ==
                     compositor::digest(graph));

    compositor::CompositorModel identity;
    identity.reset(root_of({curves_node(20U, scene::CurvesParameters{})}));
    COMPOSITOR_CHECK(compositor::effect_node_is_identity(
        identity.evaluate().nodes.front()));

    scene::CurvesParameters changed = curves;
    changed.master.points[1U].y = 0.35F;
    auto changed_nodes = nodes;
    changed_nodes[2U] = curves_node(2U, changed);
    compositor::CompositorModel different;
    different.reset(root_of(changed_nodes));
    COMPOSITOR_CHECK(compositor::digest(different.evaluate()) !=
                     compositor::digest(graph));

    scene::CurvesParameters malformed = curves;
    malformed.master.points[1U].x = 0.0F;
    compositor::CompositorModel rejected;
    rejected.reset(root_of({curves_node(21U, malformed)}));
    COMPOSITOR_CHECK(rejected.counters().rejected == 1U);
    COMPOSITOR_CHECK(rejected.size() == 0U);

    doc::Node gap = curves_node(22U, curves);
    gap.properties.erase("curve_master_1_x");
    gap.properties.erase("curve_master_1_y");
    compositor::CompositorModel rejected_gap;
    rejected_gap.reset(root_of({gap}));
    COMPOSITOR_CHECK(rejected_gap.counters().rejected == 1U);
}

void film_grain_is_typed_ordered_digested_and_replicated() {
    const scene::FilmGrainParameters grain{
        0.4F, 3.5F, 0.75F, 0xF234'5678U,
        scene::FilmGrainMode::RGB};
    scene::LevelsParameters levels{};
    levels.master.gamma = 1.5F;
    const scene::ContrastPivotParameters contrast{1.5F, 0.4F};
    const std::vector<doc::Node> nodes{
        node(100U, "scene.object"), levels_node(1U, levels),
        film_grain_node(2U, grain), contrast_pivot_node(3U, contrast),
        scene_layer(4U, 100U), node(5U, schema::output_type),
        edge(10U, 1U, 0U, 4U, 0U, 0),
        edge(11U, 2U, 0U, 4U, 0U, 1),
        edge(12U, 3U, 0U, 4U, 0U, 2),
        edge(13U, 4U, 0U, 5U, 0U)};

    compositor::CompositorModel checkpointed;
    checkpointed.reset(root_of(nodes));
    const auto graph = checkpointed.evaluate();
    COMPOSITOR_CHECK(graph.valid());
    const auto* const typed = graph.node(doc::NodeId{2U});
    COMPOSITOR_CHECK(typed != nullptr);
    COMPOSITOR_CHECK(typed->kind == compositor::NodeKind::FilmGrain);
    COMPOSITOR_CHECK(
        std::get<compositor::FilmGrainParameters>(typed->parameters) == grain);
    COMPOSITOR_CHECK(!compositor::effect_node_is_identity(*typed));
    COMPOSITOR_CHECK(
        compositor::output_ports(compositor::NodeKind::FilmGrain).size()
        == 1U);

    scene::Scene source{};
    scene::MeshInstance instance{};
    instance.source = doc::NodeId{100U};
    source.instances.push_back(instance);
    const auto adjusted = compositor::apply(graph, source);
    const auto& program =
        adjusted.instances.front().style.effects.inline_operations;
    COMPOSITOR_CHECK(program.size() == 3U);
    COMPOSITOR_CHECK(program[0U].kind == scene::EffectKind::Levels);
    COMPOSITOR_CHECK(program[1U].kind == scene::EffectKind::FilmGrain);
    COMPOSITOR_CHECK(program[1U].film_grain == grain);
    COMPOSITOR_CHECK(program[2U].kind == scene::EffectKind::ContrastPivot);

    compositor::CompositorModel streamed;
    doc::Root running;
    for (auto iterator = nodes.rbegin(); iterator != nodes.rend(); ++iterator) {
        const doc::Root next = running.set(*iterator);
        streamed.apply(doc::diff(running, next));
        running = next;
    }
    COMPOSITOR_CHECK(streamed.evaluate() == graph);
    COMPOSITOR_CHECK(compositor::digest(streamed.evaluate()) ==
                     compositor::digest(graph));

    compositor::CompositorModel identity;
    identity.reset(root_of({film_grain_node(
        20U, scene::FilmGrainParameters{})}));
    COMPOSITOR_CHECK(compositor::effect_node_is_identity(
        identity.evaluate().nodes.front()));

    compositor::CompositorModel bounded;
    bounded.reset(root_of({film_grain_node(
        21U, scene::FilmGrainParameters{-1.0F, 100.0F, 2.0F, 5U,
                                        scene::FilmGrainMode::Monochrome})}));
    const auto normalized = std::get<compositor::FilmGrainParameters>(
        bounded.evaluate().nodes.front().parameters);
    COMPOSITOR_CHECK(normalized.amount == 0.0F);
    COMPOSITOR_CHECK(normalized.pixel_scale == 64.0F);
    COMPOSITOR_CHECK(normalized.chroma_ratio == 1.0F);

    scene::FilmGrainParameters changed = grain;
    ++changed.seed;
    auto changed_nodes = nodes;
    changed_nodes[2U] = film_grain_node(2U, changed);
    compositor::CompositorModel different;
    different.reset(root_of(changed_nodes));
    COMPOSITOR_CHECK(compositor::digest(different.evaluate()) !=
                     compositor::digest(graph));

    doc::Node invalid_mode = film_grain_node(22U, grain);
    invalid_mode.properties[std::string{schema::mode}] =
        doc::PropertyValue{std::string{"random"}};
    compositor::CompositorModel rejected;
    rejected.reset(root_of({invalid_mode}));
    COMPOSITOR_CHECK(rejected.counters().rejected == 1U);
    COMPOSITOR_CHECK(rejected.size() == 0U);
}

// Effects and filters have inputs of their own on a Scene Layer: EFFECTS
// (0) runs before FILTERS (2) however the wires were made, and each
// kind is refused on the other's input.
void effects_and_filters_take_their_own_inputs() {
    const auto evaluate = [](std::vector<doc::Node> nodes) {
        compositor::CompositorModel model;
        model.reset(root_of(nodes));
        return model.evaluate();
    };
    const std::vector<doc::Node> shared{
        node(100U, "scene.object"),
        node(1U, schema::blur_type, {real(schema::radius, 8.0)}),
        node(2U, schema::brightness_type, {real(schema::amount, 2.0)}),
        node(3U, schema::saturation_type, {real(schema::amount, 0.5)}),
        scene_layer(4U, 100U),
        node(5U, schema::output_type),
        edge(13U, 4U, 0U, 5U, 0U),
    };
    std::vector<doc::Node> split = shared;
    // The blur was wired first and sorts first by order; it still runs last.
    split.push_back(edge(10U, 1U, 0U, 4U, 2U, 0));
    split.push_back(edge(11U, 2U, 0U, 4U, 0U, 1));
    split.push_back(edge(12U, 3U, 0U, 4U, 0U, 2));
    const compositor::CompositorGraph graph = evaluate(split);
    COMPOSITOR_CHECK(graph.valid());
    COMPOSITOR_CHECK(graph.diagnostics.empty());
    const compositor::PlanStep* const layer = graph.step(doc::NodeId{4U});
    COMPOSITOR_CHECK(layer != nullptr && layer->inputs.size() == 3U);
    COMPOSITOR_CHECK(layer->inputs[0].source == doc::NodeId{2U} &&
                     layer->inputs[0].type == compositor::PortValueType::Effect);
    COMPOSITOR_CHECK(layer->inputs[1].source == doc::NodeId{3U});
    COMPOSITOR_CHECK(layer->inputs[2].source == doc::NodeId{1U} &&
                     layer->inputs[2].type == compositor::PortValueType::Filter);

    const auto refused = [&](doc::Node wire) {
        std::vector<doc::Node> nodes = shared;
        nodes.push_back(std::move(wire));
        const compositor::CompositorGraph wrong = evaluate(nodes);
        return !wrong.valid() &&
            std::any_of(wrong.diagnostics.begin(), wrong.diagnostics.end(),
                        [](const compositor::GraphDiagnostic& diagnostic) {
                            return diagnostic.code ==
                                compositor::DiagnosticCode::IncompatiblePorts;
                        });
    };
    COMPOSITOR_CHECK(refused(edge(10U, 1U, 0U, 4U, 0U)));
    COMPOSITOR_CHECK(refused(edge(11U, 2U, 0U, 4U, 2U)));

    // A zero-radius Blur is an identity and does not force isolation.
    compositor::CompositorModel identities;
    identities.reset(root_of({
        node(100U, "scene.object"),
        node(1U, schema::blur_type, {real(schema::radius, 0.0)}),
        node(2U, schema::saturation_type, {real(schema::amount, 0.5)}),
        scene_layer(6U, 100U),
        node(7U, schema::output_type),
        edge(10U, 1U, 0U, 6U, 2U, 0),
        edge(11U, 2U, 0U, 6U, 0U, 1),
        edge(15U, 6U, 0U, 7U, 0U),
    }));
    const compositor::CompositorGraph identity_graph = identities.evaluate();
    COMPOSITOR_CHECK(identity_graph.valid());
    const compositor::PlanStep* const identity_blur =
        identity_graph.step(doc::NodeId{1U});
    COMPOSITOR_CHECK(identity_blur != nullptr);
    COMPOSITOR_CHECK(!identity_blur->requires_offscreen);
}

// An LED Car Process takes setup effects (9) and setup filters (10) for
// every plate: the effect joins each plate's pointwise operations and the
// filter its spatial ones, after the plate's own.
void led_car_setup_takes_effects_and_filters_apart() {
    std::vector<doc::Node> nodes{
        node(100U, "scene.object"),
        scene_layer(1U, 100U),
        node(2U, schema::led_car_process_type),
        node(3U, schema::output_type),
        node(4U, schema::brightness_type, {real(schema::amount, 2.0)}),
        node(5U, schema::blur_type, {real(schema::radius, 6.0)}),
        node(6U, schema::saturation_type, {real(schema::amount, 0.5)}),
        edge(20U, 1U, 0U, 2U, 0U),
        edge(21U, 2U, 0U, 3U, 0U),
        edge(22U, 4U, 0U, 2U, 9U),
        edge(23U, 5U, 0U, 2U, 10U),
        edge(24U, 6U, 0U, 1U, 0U),
    };
    compositor::CompositorModel model;
    model.reset(root_of(nodes));
    const compositor::CompositorGraph graph = model.evaluate();
    COMPOSITOR_CHECK(graph.valid());
    scene::Scene source{};
    scene::MeshInstance mesh{};
    mesh.source = doc::NodeId{100U};
    mesh.world = scene::identity();
    mesh.normal = scene::identity();
    source.instances.push_back(mesh);
    const scene::Scene applied = compositor::apply(graph, source);
    const scene::EffectStack& effects =
        applied.instances.front().style.effects;
    COMPOSITOR_CHECK(effects.inline_operations.size() == 2U);
    COMPOSITOR_CHECK(effects.inline_operations[0U].kind ==
                     scene::EffectKind::Saturation);
    COMPOSITOR_CHECK(effects.inline_operations[1U].kind ==
                     scene::EffectKind::Brightness);
    COMPOSITOR_CHECK(effects.spatial_operations.size() == 1U);
    COMPOSITOR_CHECK(effects.spatial_operations[0U].kind ==
                     scene::EffectKind::Blur);

    // The other way round, each is refused.
    nodes[9] = edge(22U, 4U, 0U, 2U, 10U);
    compositor::CompositorModel crossed;
    crossed.reset(root_of(nodes));
    COMPOSITOR_CHECK(!crossed.evaluate().valid());
}

void duplicate_active_scene_layer_sources_are_diagnosed() {
    compositor::CompositorModel duplicate;
    duplicate.reset(root_of({
        node(100U, "scene.object"),
        node(1U, schema::saturation_type, {real(schema::amount, 0.5)}),
        node(2U, schema::blur_type, {real(schema::radius, 4.0)}),
        scene_layer(3U, 100U),
        scene_layer(4U, 100U),
        node(5U, schema::composite_type),
        node(6U, schema::output_type),
        edge(10U, 1U, 0U, 3U, 0U),
        edge(11U, 2U, 0U, 4U, 2U),
        edge(12U, 3U, 0U, 5U, 0U, 0),
        edge(13U, 4U, 0U, 5U, 0U, 1),
        edge(14U, 5U, 0U, 6U, 0U),
    }));
    const compositor::CompositorGraph graph = duplicate.evaluate();
    COMPOSITOR_CHECK(!graph.valid());
    COMPOSITOR_CHECK(graph.plan.steps.empty());
    COMPOSITOR_CHECK(std::any_of(
        graph.diagnostics.begin(), graph.diagnostics.end(),
        [](const compositor::GraphDiagnostic& diagnostic) noexcept {
            return diagnostic.code ==
                compositor::DiagnosticCode::DuplicateSceneLayerReference;
        }));

    scene::Scene source{};
    scene::MeshInstance mesh{};
    mesh.source = doc::NodeId{100U};
    source.instances.push_back(mesh);
    COMPOSITOR_CHECK(scene::digest(compositor::apply(graph, source)) ==
                     scene::digest(source));

    // A disconnected alternate is harmless and remains available to edit.
    const auto without_second_branch = root_of({
        node(100U, "scene.object"),
        scene_layer(3U, 100U),
        scene_layer(4U, 100U),
        node(6U, schema::output_type),
        edge(12U, 3U, 0U, 6U, 0U),
    });
    duplicate.reset(without_second_branch);
    COMPOSITOR_CHECK(duplicate.evaluate().valid());

    // One graph-layer image may fan out to multiple composite inputs. That is
    // not two independently adjusted branches and therefore is not the source
    // aliasing this diagnostic protects against.
    duplicate.reset(root_of({
        node(100U, "scene.object"),
        scene_layer(3U, 100U),
        node(5U, schema::composite_type),
        node(6U, schema::output_type),
        edge(12U, 3U, 0U, 5U, 0U, 0),
        edge(13U, 3U, 0U, 5U, 0U, 1),
        edge(14U, 5U, 0U, 6U, 0U),
    }));
    const compositor::CompositorGraph fanout = duplicate.evaluate();
    COMPOSITOR_CHECK(fanout.valid());
    COMPOSITOR_CHECK(std::none_of(
        fanout.diagnostics.begin(), fanout.diagnostics.end(),
        [](const compositor::GraphDiagnostic& diagnostic) noexcept {
            return diagnostic.code ==
                compositor::DiagnosticCode::DuplicateSceneLayerReference;
        }));
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

    changed = base_nodes;
    changed[2U].properties.insert_or_assign(
        std::string{schema::scene_id}, doc::PropertyValue{std::int64_t{777}});
    compositor::CompositorModel owner_changed;
    owner_changed.reset(root_of(changed));
    COMPOSITOR_CHECK(compositor::digest(owner_changed.evaluate()) != reference);
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
            edge(static_cast<core::u64>(index) + 20U, 2U, 0U, 1U, 2U));
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

// Two scenes, each with a complete one-layer graph, plus one graph node
// carrying no owner at all, which belongs to neither scene.
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
        // No scene_id: no scene's graph.
        node(6U, schema::brightness_type, {real(schema::amount, 0.5)}),
        edge(30U, 3U, 0U, 1U, 2U),
        edge(31U, 1U, 0U, 2U, 0U),
        edge(32U, 4U, 0U, 5U, 0U),
    };
}

void a_graph_belongs_to_one_scene() {
    compositor::CompositorModel model;
    model.reset(root_of(two_scene_graph()));

    const auto program = model.evaluate(doc::NodeId{10U});
    COMPOSITOR_CHECK(program.valid());
    COMPOSITOR_CHECK(program.nodes.size() == 3U);
    COMPOSITOR_CHECK(program.node(doc::NodeId{1U}) != nullptr);
    COMPOSITOR_CHECK(program.node(doc::NodeId{4U}) == nullptr);
    // The unowned node is in neither scene, not adopted by the lowest stage.
    COMPOSITOR_CHECK(program.node(doc::NodeId{6U}) == nullptr);
    // Only edges whose endpoints are both in scope survive.
    COMPOSITOR_CHECK(program.edges.size() == 2U);

    const auto preview = model.evaluate(doc::NodeId{20U});
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
    const auto before = model.evaluate(doc::NodeId{10U});

    // Retune the blur that belongs to scene 20 only.
    doc::Node retuned = node(7U, schema::blur_type, {real(schema::radius, 9.0)});
    retuned.properties.insert_or_assign(std::string{schema::scene_id},
                                        doc::PropertyValue{std::int64_t{20}});
    doc::Root edited = root.set(retuned);
    edited = edited.set(
        node(33U, schema::edge_type,
             {whole(schema::from_node, 7), whole(schema::from_port, 0),
              whole(schema::to_node, 4), whole(schema::to_port, 2)}));
    model.apply(doc::diff(root, edited));

    const auto after = model.evaluate(doc::NodeId{10U});
    COMPOSITOR_CHECK(after.nodes == before.nodes);
    COMPOSITOR_CHECK(after.edges == before.edges);
    COMPOSITOR_CHECK(after.plan == before.plan);

    const auto preview = model.evaluate(doc::NodeId{20U});
    COMPOSITOR_CHECK(preview.node(doc::NodeId{7U}) != nullptr);
    COMPOSITOR_CHECK(preview.edges.size() == 2U);
}

void a_scene_copy_carries_its_graph() {
    const doc::Root root = root_of(two_scene_graph());
    // The scene layer copies drawable 100 to 500 the way a scene clone does.
    const std::array<std::pair<doc::NodeId, doc::NodeId>, 1U> elements{
        {{doc::NodeId{100U}, doc::NodeId{500U}}}};
    auto planned = compositor::plan_clone_graph(
        root, doc::NodeId{10U}, doc::NodeId{400U}, elements,
        doc::NodeId{1'000U});
    COMPOSITOR_CHECK(static_cast<bool>(planned));
    const compositor::GraphClonePlan plan = std::move(planned).value();
    // Scene 10 owns three graph nodes and the two edges that join them; the
    // unowned brightness stays behind.
    COMPOSITOR_CHECK(plan.puts.size() == 5U);

    // The scene clone that produced `elements` also created the copied
    // drawable; without it the copied layer would reference nothing.
    doc::Root copied = root.set(node(500U, "scene.object"));
    for (const doc::Node& put : plan.puts) {
        COMPOSITOR_CHECK(!root.contains(put.id));
        copied = copied.set(put);
    }
    compositor::CompositorModel model;
    model.reset(copied);

    const auto source = model.evaluate(doc::NodeId{10U});
    const auto copy = model.evaluate(doc::NodeId{400U});
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
        original.evaluate(doc::NodeId{10U}).plan ==
        source.plan);
}

void warnings_keep_the_plan_and_expose_canonical_node_paths() {
    compositor::CompositorModel model;
    model.reset(root_of({
        node(100U, "scene.object"),
        node(1U, schema::brightness_type, {real(schema::amount, 2.0)}),
        scene_layer(2U, 100U),
        node(3U, schema::blur_type, {real(schema::radius, 8.0)}),
        node(4U, schema::output_type),
        node(5U, schema::saturation_type, {real(schema::amount, 1.0)}),
        edge(10U, 1U, 0U, 2U, 0U),
        edge(11U, 5U, 0U, 2U, 0U, 1),
        edge(12U, 2U, 0U, 4U, 0U),
    }));

    const compositor::CompositorGraph graph = model.evaluate();
    COMPOSITOR_CHECK(graph.valid());
    COMPOSITOR_CHECK(!graph.has_errors());
    COMPOSITOR_CHECK(graph.has_warnings());
    COMPOSITOR_CHECK(!graph.plan.steps.empty());
    COMPOSITOR_CHECK(graph.diagnostics.size() == 1U);
    COMPOSITOR_CHECK(graph.diagnostics.front().code ==
                     compositor::DiagnosticCode::DeadBranch);
    COMPOSITOR_CHECK(graph.diagnostics.front().subject == doc::NodeId{3U});
    COMPOSITOR_CHECK(graph.diagnostics.front().severity ==
                     compositor::DiagnosticSeverity::Warning);

    const compositor::GraphNodeMetadata* const brightness =
        graph.metadata(doc::NodeId{1U});
    COMPOSITOR_CHECK(brightness != nullptr);
    COMPOSITOR_CHECK(brightness->execution_domain ==
                     compositor::GraphExecutionDomain::Inline);
    COMPOSITOR_CHECK(brightness->identity ==
                     compositor::EffectIdentityState::Active);
    COMPOSITOR_CHECK(brightness->path_to_output ==
                     std::vector<doc::NodeId>{doc::NodeId{1U},
                                              doc::NodeId{2U},
                                              doc::NodeId{4U}});
    COMPOSITOR_CHECK(brightness->edges_to_output ==
                     std::vector<doc::NodeId>{doc::NodeId{10U},
                                              doc::NodeId{12U}});

    const compositor::GraphNodeMetadata* const identity =
        graph.metadata(doc::NodeId{5U});
    COMPOSITOR_CHECK(identity != nullptr);
    COMPOSITOR_CHECK(identity->identity ==
                     compositor::EffectIdentityState::Identity);
    COMPOSITOR_CHECK(identity->path_to_output ==
                     std::vector<doc::NodeId>{doc::NodeId{5U},
                                              doc::NodeId{2U},
                                              doc::NodeId{4U}});

    const compositor::GraphNodeMetadata* const dead =
        graph.metadata(doc::NodeId{3U});
    COMPOSITOR_CHECK(dead != nullptr);
    COMPOSITOR_CHECK(dead->execution_domain ==
                     compositor::GraphExecutionDomain::IsolatedLayer);
    COMPOSITOR_CHECK(dead->path_to_output.empty());

    compositor::CompositorModel mixed;
    mixed.reset(root_of({node(1U, schema::blur_type,
                              {real(schema::radius, 8.0)}),
                         node(2U, schema::output_type)}));
    const compositor::CompositorGraph mixed_graph = mixed.evaluate();
    // An Output with nothing wired is not an error: it is disabled, and the
    // graph composites nothing - no plan - so Program shows the scene.
    COMPOSITOR_CHECK(mixed_graph.valid());
    COMPOSITOR_CHECK(mixed_graph.diagnostics.size() == 1U);
    COMPOSITOR_CHECK(mixed_graph.diagnostics[0U].severity ==
                     compositor::DiagnosticSeverity::Warning);
    COMPOSITOR_CHECK(mixed_graph.diagnostics[0U].code ==
                     compositor::DiagnosticCode::DeadBranch);
    COMPOSITOR_CHECK(mixed_graph.plan.steps.empty());
    COMPOSITOR_CHECK(mixed_graph.metadata(doc::NodeId{2U}) != nullptr);
    COMPOSITOR_CHECK(mixed_graph.metadata(doc::NodeId{2U})->disabled);
    COMPOSITOR_CHECK(!mixed_graph.metadata(doc::NodeId{1U})->disabled);

    // A node waiting for a required input is disabled, never an error, and
    // what it feeds waits with it. Parked, a Composite with nothing on LAYERS
    // and a Merge with no BASE leave the live layer planning. Wired between
    // a layer and Output, the Merge with no BASE disables the Output too, so
    // nothing is composited; given a BASE, it all comes back.
    enum class MergeWiring { Parked, NoBase, Complete };
    const auto parked_root = [](MergeWiring wiring) {
        std::vector<doc::Node> nodes{
            node(100U, "scene.object"),
            node(101U, "scene.object"),
            scene_layer(2U, 100U),
            scene_layer(3U, 101U),
            node(4U, schema::output_type),
            node(6U, schema::composite_type),
            node(7U, schema::merge_type),
            node(8U, schema::composite_type),
        };
        if (wiring == MergeWiring::Parked) {
            nodes.push_back(edge(12U, 2U, 0U, 4U, 0U));
        } else {
            nodes.push_back(edge(12U, 2U, 0U, 8U, 0U));
            nodes.push_back(edge(13U, 8U, 0U, 7U, 1U));
            nodes.push_back(edge(14U, 7U, 0U, 4U, 0U));
        }
        if (wiring == MergeWiring::Complete) {
            nodes.push_back(edge(15U, 3U, 0U, 7U, 0U));
        }
        return root_of(nodes);
    };
    const auto disabled = [](const compositor::CompositorGraph& graph,
                             core::u64 id) {
        const compositor::GraphNodeMetadata* const metadata =
            graph.metadata(doc::NodeId{id});
        return metadata != nullptr && metadata->disabled;
    };

    compositor::CompositorModel parked;
    parked.reset(parked_root(MergeWiring::Parked));
    const compositor::CompositorGraph parked_graph = parked.evaluate();
    COMPOSITOR_CHECK(parked_graph.valid());
    COMPOSITOR_CHECK(!parked_graph.plan.steps.empty());
    COMPOSITOR_CHECK(!disabled(parked_graph, 4U));
    for (const core::u64 id : {6U, 7U, 8U}) {
        COMPOSITOR_CHECK(disabled(parked_graph, id));
        COMPOSITOR_CHECK(has_diagnostic(
            parked_graph, compositor::DiagnosticCode::DeadBranch, doc::NodeId{id}));
    }

    compositor::CompositorModel waiting;
    waiting.reset(parked_root(MergeWiring::NoBase));
    const compositor::CompositorGraph waiting_graph = waiting.evaluate();
    COMPOSITOR_CHECK(waiting_graph.valid());
    COMPOSITOR_CHECK(!waiting_graph.has_errors());
    COMPOSITOR_CHECK(waiting_graph.plan.steps.empty());
    COMPOSITOR_CHECK(disabled(waiting_graph, 7U));
    COMPOSITOR_CHECK(disabled(waiting_graph, 4U));
    // The Composite has its layer and is enabled, but what it feeds is not,
    // so it reaches no Output.
    COMPOSITOR_CHECK(!disabled(waiting_graph, 8U));
    COMPOSITOR_CHECK(has_diagnostic(
        waiting_graph, compositor::DiagnosticCode::DeadBranch, doc::NodeId{8U}));

    compositor::CompositorModel complete;
    complete.reset(parked_root(MergeWiring::Complete));
    const compositor::CompositorGraph complete_graph = complete.evaluate();
    COMPOSITOR_CHECK(complete_graph.valid());
    COMPOSITOR_CHECK(!complete_graph.plan.steps.empty());
    for (const core::u64 id : {4U, 7U, 8U}) {
        COMPOSITOR_CHECK(!disabled(complete_graph, id));
    }
    COMPOSITOR_CHECK(disabled(complete_graph, 6U));

    // Two equally short live routes choose the lower next node id, independent
    // of edge insertion order.
    compositor::CompositorModel branched;
    branched.reset(root_of({
        node(100U, "scene.object"), node(101U, "scene.object"),
        node(1U, schema::brightness_type, {real(schema::amount, 2.0)}),
        scene_layer(2U, 100U), scene_layer(6U, 101U),
        node(7U, schema::composite_type), node(8U, schema::output_type),
        edge(20U, 1U, 0U, 6U, 0U), edge(19U, 1U, 0U, 2U, 0U),
        edge(22U, 6U, 0U, 7U, 0U), edge(21U, 2U, 0U, 7U, 0U),
        edge(23U, 7U, 0U, 8U, 0U),
    }));
    const compositor::CompositorGraph branched_graph = branched.evaluate();
    COMPOSITOR_CHECK(branched_graph.valid());
    const compositor::GraphNodeMetadata* const branch =
        branched_graph.metadata(doc::NodeId{1U});
    COMPOSITOR_CHECK(branch != nullptr);
    COMPOSITOR_CHECK(branch->path_to_output ==
                     std::vector<doc::NodeId>{doc::NodeId{1U},
                                              doc::NodeId{2U},
                                              doc::NodeId{7U},
                                              doc::NodeId{8U}});
    COMPOSITOR_CHECK(branch->edges_to_output ==
                     std::vector<doc::NodeId>{doc::NodeId{19U},
                                              doc::NodeId{21U},
                                              doc::NodeId{23U}});
}

void authored_resources_are_estimated_and_budgeted_without_machine_state() {
    const std::vector<doc::Node> ordinary{
        stage(1000U, 3840U, 2160U), effect_settings(1001U, 33U, 4U),
        node(100U, "scene.object"),
        node(1U, schema::blur_type, {real(schema::radius, 16.0)}),
        scene_layer(2U, 100U), node(3U, schema::output_type),
        edge(10U, 1U, 0U, 2U, 2U), edge(11U, 2U, 0U, 3U, 0U)};
    const doc::Root ordinary_root = root_of(ordinary);
    compositor::CompositorModel model;
    model.reset(ordinary_root);
    const compositor::CompositorGraph graph =
        model.evaluate(doc::NodeId{1000U});
    COMPOSITOR_CHECK(graph.valid());
    COMPOSITOR_CHECK(graph.resources.available);
    COMPOSITOR_CHECK(graph.resources.canvas == scene::Canvas{3840U, 2160U});
    COMPOSITOR_CHECK(graph.resources.effect_settings.blur_taps == 33U);
    COMPOSITOR_CHECK(graph.resources.effect_settings.blur_passes == 4U);
    COMPOSITOR_CHECK(graph.resources.active_scene_layers == 1U);
    COMPOSITOR_CHECK(graph.resources.blurred_scene_layers == 1U);
    COMPOSITOR_CHECK(graph.resources.offscreen_textures == 6U);
    COMPOSITOR_CHECK(graph.resources.bytes_per_texture ==
                     3840ULL * 2160ULL * 4ULL);
    COMPOSITOR_CHECK(graph.resources.offscreen_texture_bytes ==
                     3840ULL * 2160ULL * 4ULL * 6ULL);
    COMPOSITOR_CHECK(graph.resources.render_passes == 10U);
    COMPOSITOR_CHECK(graph.resources.blur_taps_per_pixel == 264U);

    compositor::CompositorModel streamed;
    streamed.apply(doc::diff(doc::Root{}, ordinary_root));
    const compositor::CompositorGraph streamed_graph =
        streamed.evaluate(doc::NodeId{1000U});
    COMPOSITOR_CHECK(streamed_graph == graph);
    COMPOSITOR_CHECK(compositor::digest(streamed_graph) ==
                     compositor::digest(graph));

    // Six 8192x8192 targets, about 1.6 GB, fit; six 16384x16384 do not.
    std::vector<doc::Node> texture_large = ordinary;
    texture_large[0U] = stage(1000U, 8'192U, 8'192U);
    model.reset(root_of(texture_large));
    const compositor::CompositorGraph large_graph =
        model.evaluate(doc::NodeId{1000U});
    COMPOSITOR_CHECK(large_graph.valid());
    COMPOSITOR_CHECK(large_graph.resources.offscreen_texture_bytes ==
                     8192ULL * 8192ULL * 4ULL * 6ULL);
    COMPOSITOR_CHECK(!has_diagnostic(
        large_graph, compositor::DiagnosticCode::TextureBudgetExceeded,
        doc::NodeId{3U}));

    // The budget is the project's own: a 1 GB setting refuses the graph the
    // 4 GB default accepted, and the graph digest moves with the setting.
    std::vector<doc::Node> texture_tight = texture_large;
    texture_tight[1U] = node(1001U, scene::schema::settings_type,
                             {whole(scene::schema::blur_taps, 33U),
                              whole(scene::schema::blur_passes, 4U),
                              real(scene::schema::graph_texture_budget_gb, 1.0)});
    model.reset(root_of(texture_tight));
    const compositor::CompositorGraph tight_graph =
        model.evaluate(doc::NodeId{1000U});
    COMPOSITOR_CHECK(!tight_graph.valid());
    COMPOSITOR_CHECK(tight_graph.resources.effect_settings
                         .graph_texture_budget_gb == 1.0F);
    COMPOSITOR_CHECK(has_diagnostic(
        tight_graph, compositor::DiagnosticCode::TextureBudgetExceeded,
        doc::NodeId{3U}));
    COMPOSITOR_CHECK(compositor::graph_texture_budget_bytes(
                         tight_graph.resources.effect_settings) ==
                     1'000'000'000ULL);
    COMPOSITOR_CHECK(compositor::digest(tight_graph) !=
                     compositor::digest(large_graph));

    std::vector<doc::Node> texture_over = ordinary;
    texture_over[0U] = stage(1000U, 16'384U, 16'384U);
    model.reset(root_of(texture_over));
    const compositor::CompositorGraph texture_graph =
        model.evaluate(doc::NodeId{1000U});
    COMPOSITOR_CHECK(!texture_graph.valid());
    COMPOSITOR_CHECK(texture_graph.plan.steps.empty());
    COMPOSITOR_CHECK(has_diagnostic(
        texture_graph, compositor::DiagnosticCode::TextureBudgetExceeded,
        doc::NodeId{3U}));

    std::vector<doc::Node> expensive{
        stage(2000U, 1920U, 1080U), effect_settings(2001U, 33U, 4U),
        node(1U, schema::blur_type, {real(schema::radius, 16.0)}),
        node(50U, schema::composite_type), node(51U, schema::output_type)};
    for (core::u32 index = 0U; index < 13U; ++index) {
        const core::u64 source = 100U + index;
        const core::u64 layer = 10U + index;
        expensive.push_back(node(source, "scene.object"));
        expensive.push_back(scene_layer(layer, source));
        expensive.push_back(edge(1000U + index, 1U, 0U, layer, 2U));
        expensive.push_back(edge(1100U + index, layer, 0U, 50U, 0U,
                                 static_cast<core::i64>(index)));
    }
    expensive.push_back(edge(1200U, 50U, 0U, 51U, 0U));
    model.reset(root_of(expensive));
    const compositor::CompositorGraph expensive_graph =
        model.evaluate(doc::NodeId{2000U});
    COMPOSITOR_CHECK(!expensive_graph.valid());
    COMPOSITOR_CHECK(expensive_graph.resources.active_scene_layers == 13U);
    COMPOSITOR_CHECK(expensive_graph.resources.blurred_scene_layers == 13U);
    COMPOSITOR_CHECK(expensive_graph.resources.render_passes == 130U);
    COMPOSITOR_CHECK(expensive_graph.resources.blur_taps_per_pixel == 3432U);
    COMPOSITOR_CHECK(has_diagnostic(
        expensive_graph, compositor::DiagnosticCode::PassBudgetExceeded,
        doc::NodeId{51U}));
    COMPOSITOR_CHECK(has_diagnostic(
        expensive_graph, compositor::DiagnosticCode::TapBudgetExceeded,
        doc::NodeId{51U}));
    COMPOSITOR_CHECK(!has_diagnostic(
        expensive_graph, compositor::DiagnosticCode::TextureBudgetExceeded,
        doc::NodeId{51U}));
    COMPOSITOR_CHECK(expensive_graph.plan.steps.empty());
}

// A wire's reroute points are layout: they read back in order, and a
// malformed list costs the points, never the connection.
void wire_points_are_layout_that_never_breaks_a_wire() {
    const auto routed = [](std::vector<Property> points) {
        doc::Node wire = edge(10U, 1U, 0U, 2U, 0U);
        for (Property& point : points) {
            wire.properties.insert_or_assign(point.first, point.second);
        }
        compositor::CompositorModel model;
        model.reset(root_of({node(100U, "scene.object"), scene_layer(1U, 100U),
                             node(2U, schema::output_type), wire}));
        return model.evaluate();
    };
    const auto points_of = [](const compositor::CompositorGraph& graph) {
        const compositor::GraphEdge* const wire = graph.edge(doc::NodeId{10U});
        COMPOSITOR_CHECK(wire != nullptr);
        return wire->points;
    };

    const auto plain = routed({});
    const auto two = routed({real("point_0_x", 120.5), real("point_0_y", -40.0),
                             real("point_1_x", 300.0), whole("point_1_y", 80)});
    COMPOSITOR_CHECK(points_of(two) ==
                     (std::vector<compositor::WirePoint>{{120.5F, -40.0F},
                                                         {300.0F, 80.0F}}));
    // Routing changes nothing the graph computes.
    COMPOSITOR_CHECK(two.diagnostics.empty());
    COMPOSITOR_CHECK(two.plan == plain.plan);
    COMPOSITOR_CHECK(compositor::digest(two) == compositor::digest(plain));

    const std::array<std::vector<Property>, 4U> malformed{{
        // A gap before a later point.
        {real("point_0_x", 1.0), real("point_0_y", 2.0), real("point_2_x", 3.0),
         real("point_2_y", 4.0)},
        // Half a point.
        {real("point_0_x", 1.0)},
        // Out of range, and the wrong type.
        {real("point_0_x", 2.0e6), real("point_0_y", 0.0)},
        {text("point_0_x", "left"), real("point_0_y", 0.0)},
    }};
    for (const std::vector<Property>& points : malformed) {
        const auto graph = routed(points);
        COMPOSITOR_CHECK(points_of(graph).empty());
        COMPOSITOR_CHECK(graph.diagnostics.empty());
        COMPOSITOR_CHECK(graph.plan == plain.plan);
    }
    // One point past the cap is too many.
    std::vector<Property> crowded;
    for (std::size_t index = 0U; index <= compositor::max_wire_points; ++index) {
        crowded.push_back(real("point_" + std::to_string(index) + "_x", 1.0));
        crowded.push_back(real("point_" + std::to_string(index) + "_y", 1.0));
    }
    COMPOSITOR_CHECK(points_of(routed(crowded)).empty());
}

// Bypass passes an effect or a Radial Array through as its identity: what
// is applied matches the graph without it, and no pass is planned for it.
// The flag is digested, and a kind with nothing to pass through as never
// reads as bypassed.
void bypass_renders_as_identity_and_is_digested() {
    const auto evaluate = [](const std::vector<doc::Node>& nodes) {
        compositor::CompositorModel model;
        model.reset(root_of(nodes));
        return model.evaluate();
    };
    const std::vector<doc::Node> active = shared_effect_graph();
    std::vector<doc::Node> passed = active;
    for (doc::Node& value : passed) {
        // Blur, Radial Array, a Scene Layer and Output.
        if (value.id == doc::NodeId{1U} || value.id == doc::NodeId{2U} ||
            value.id == doc::NodeId{3U} || value.id == doc::NodeId{6U}) {
            value.properties.insert_or_assign(std::string{schema::bypassed},
                                              doc::PropertyValue{true});
        }
    }
    std::vector<doc::Node> without;
    for (const doc::Node& value : active) {
        const core::u64 id = value.id.value();
        if (id != 1U && id != 2U && (id < 20U || id > 23U)) {
            without.push_back(value);
        }
    }
    const compositor::CompositorGraph on = evaluate(active);
    const compositor::CompositorGraph off = evaluate(passed);
    const compositor::CompositorGraph bare = evaluate(without);
    COMPOSITOR_CHECK(on.valid() && off.valid() && bare.valid());
    COMPOSITOR_CHECK(off.node(doc::NodeId{1U})->bypassed);
    COMPOSITOR_CHECK(off.node(doc::NodeId{2U})->bypassed);
    COMPOSITOR_CHECK(!off.node(doc::NodeId{3U})->bypassed);
    COMPOSITOR_CHECK(!off.node(doc::NodeId{6U})->bypassed);
    COMPOSITOR_CHECK(on.metadata(doc::NodeId{1U})->identity ==
                     compositor::EffectIdentityState::Active);
    COMPOSITOR_CHECK(off.metadata(doc::NodeId{1U})->identity ==
                     compositor::EffectIdentityState::Identity);
    COMPOSITOR_CHECK(on.step(doc::NodeId{1U})->requires_offscreen);
    COMPOSITOR_CHECK(!off.step(doc::NodeId{1U})->requires_offscreen);
    COMPOSITOR_CHECK(on.step(doc::NodeId{2U})->radial_instances.size() == 9U);
    COMPOSITOR_CHECK(off.step(doc::NodeId{2U})->radial_instances.empty());
    COMPOSITOR_CHECK(off.resources.blurred_scene_layers ==
                     bare.resources.blurred_scene_layers);
    COMPOSITOR_CHECK(compositor::digest(off) != compositor::digest(on));

    scene::Scene source{};
    scene::MeshInstance mesh{};
    mesh.source = doc::NodeId{100U};
    mesh.world = scene::identity();
    mesh.normal = scene::identity();
    source.instances.push_back(mesh);
    scene::MediaInstance media{};
    media.source_node = doc::NodeId{101U};
    media.world = scene::identity();
    media.normal = scene::identity();
    source.media.push_back(media);
    const scene::Scene plain = compositor::apply(bare, source);
    const scene::Scene bypassed = compositor::apply(off, source);
    COMPOSITOR_CHECK(bypassed.instances == plain.instances);
    COMPOSITOR_CHECK(bypassed.media == plain.media);
    COMPOSITOR_CHECK(compositor::apply(on, source).instances != plain.instances);
}

// A collapsed node is drawn smaller and nothing else: the flag reads back,
// and the plan and digest are the open graph's.
void collapse_is_layout_only() {
    const auto graph_of = [](bool folded) {
        doc::Node layer = scene_layer(1U, 100U);
        if (folded) {
            layer.properties.insert_or_assign(std::string{schema::collapsed},
                                              doc::PropertyValue{true});
        }
        compositor::CompositorModel model;
        model.reset(root_of({node(100U, "scene.object"), layer,
                             node(2U, schema::output_type),
                             edge(10U, 1U, 0U, 2U, 0U)}));
        return model.evaluate();
    };
    const auto open = graph_of(false);
    const auto folded = graph_of(true);
    COMPOSITOR_CHECK(!open.node(doc::NodeId{1U})->collapsed);
    COMPOSITOR_CHECK(folded.node(doc::NodeId{1U})->collapsed);
    COMPOSITOR_CHECK(!folded.node(doc::NodeId{2U})->collapsed);
    COMPOSITOR_CHECK(folded.plan == open.plan);
    COMPOSITOR_CHECK(folded.diagnostics == open.diagnostics);
    COMPOSITOR_CHECK(compositor::digest(folded) == compositor::digest(open));
}

// Comment boxes belong to their scene's graph like nodes, read back as
// authored, and change nothing the graph computes. A malformed box is
// dropped quietly.
void comment_boxes_are_layout_in_their_scene() {
    const auto comment = [](core::u64 id, core::i64 scene,
                            std::vector<Property> extra) {
        doc::Node box = node(id, schema::comment_type,
                             {whole(schema::scene_id, scene),
                              text(schema::label, "Plates"),
                              real(schema::editor_x, 10.0),
                              real(schema::editor_y, -20.0),
                              real(schema::comment_width, 400.0),
                              real(schema::comment_height, 240.0),
                              whole(schema::comment_color, 3)});
        for (Property& property : extra) {
            box.properties.insert_or_assign(property.first, property.second);
        }
        return box;
    };
    std::vector<doc::Node> plain{stage(5U, 1920U, 1080U),
                                 node(1U, schema::output_type,
                                      {whole(schema::scene_id, 5)})};
    std::vector<doc::Node> boxed = plain;
    boxed.push_back(comment(40U, 5, {}));
    boxed.push_back(comment(41U, 6, {}));
    boxed.push_back(comment(42U, 5, {real(schema::comment_width, 10.0)}));
    boxed.push_back(comment(43U, 5, {whole(schema::comment_color, 9)}));

    compositor::CompositorModel without;
    without.reset(root_of(plain));
    compositor::CompositorModel with;
    with.reset(root_of(boxed));
    const auto graph = with.evaluate(doc::NodeId{5U});
    COMPOSITOR_CHECK(graph.comments ==
                     (std::vector<compositor::GraphComment>{
                         {doc::NodeId{40U}, doc::NodeId{5U}, "Plates", 10.0F,
                          -20.0F, 400.0F, 240.0F, 3U}}));
    const auto bare = without.evaluate(doc::NodeId{5U});
    COMPOSITOR_CHECK(graph.plan == bare.plan);
    COMPOSITOR_CHECK(graph.diagnostics == bare.diagnostics);
    COMPOSITOR_CHECK(graph.nodes == bare.nodes);
    COMPOSITOR_CHECK(with.counters().nodes == without.counters().nodes);
}

// The frame loop's update() must leave the model a rebuild would: every node
// of a graph with a comment box is edited, broken, erased and restored in
// turn, and each time the updated model's graph is the rebuilt one's.
void an_updated_model_is_the_model_a_rebuild_would_be() {
    std::vector<doc::Node> authored = saturation_graph(2.25);
    authored.push_back(stage(5U, 1920U, 1080U));
    authored.push_back(node(40U, schema::comment_type,
                            {whole(schema::scene_id, 5),
                             text(schema::label, "Plates"),
                             real(schema::comment_width, 400.0),
                             real(schema::comment_height, 240.0)}));
    const doc::Root full = root_of(authored);

    compositor::CompositorModel updated;
    // The same again the way a frame loop does it, reading the canvas and
    // effect settings from the scene model the loop already keeps.
    scene::SceneModel scenes;
    compositor::CompositorModel sharing;
    std::size_t steps = 0U;
    const auto step = [&](const doc::Root& document) {
        updated.update(document);
        scenes.update(document);
        sharing.update(document, scenes);
        compositor::CompositorModel rebuilt;
        rebuilt.reset(document);
        COMPOSITOR_CHECK(updated.evaluate() == rebuilt.evaluate());
        COMPOSITOR_CHECK(sharing.evaluate() == rebuilt.evaluate());
        COMPOSITOR_CHECK(sharing.evaluate(doc::NodeId{5U}) ==
                         rebuilt.evaluate(doc::NodeId{5U}));
        // The estimates are only there to compare when a stage is: the
        // scoped graph's canvas is the authored one, by either route.
        if (document.contains(doc::NodeId{5U})) {
            COMPOSITOR_CHECK(sharing.evaluate(doc::NodeId{5U})
                                 .resources.available);
        }
        COMPOSITOR_CHECK(updated.evaluate(doc::NodeId{5U}) ==
                         rebuilt.evaluate(doc::NodeId{5U}));
        COMPOSITOR_CHECK(updated.size() == rebuilt.size());
        COMPOSITOR_CHECK(updated.counters().nodes == rebuilt.counters().nodes);
        COMPOSITOR_CHECK(updated.counters().edges == rebuilt.counters().edges);
        ++steps;
    };

    doc::Root running;
    for (const doc::Node& one : authored) {
        running = running.set(one);
        step(running);
    }
    const doc::Delta all = doc::diff(doc::Root{}, full);
    for (const doc::NodeChange& change : all.changes) {
        doc::Node changed = *change.after;
        changed.properties.insert_or_assign(std::string{schema::amount},
                                            doc::PropertyValue{0.375});
        step(full.set(changed));

        doc::Node malformed = *change.after;
        malformed.properties.insert_or_assign(
            std::string{schema::amount},
            doc::PropertyValue{std::string{"not a number"}});
        step(full.set(malformed));

        const auto erased = full.erase(change.id);
        COMPOSITOR_CHECK(erased.has_value());
        step(*erased);

        step(full);
    }
    COMPOSITOR_CHECK(steps == authored.size() + all.changes.size() * 4U);
    step(doc::Root{});
    step(full);

    // A scene model that is not at the document is not believed: the canvas
    // comes out as the document's own, not as the stale model's.
    scene::SceneModel stale;
    stale.update(doc::Root{}.set(stage(5U, 640U, 360U)));
    compositor::CompositorModel wary;
    wary.update(full);
    wary.update(full.set(stage(6U, 320U, 200U)), stale);
    const auto canvas = wary.evaluate(doc::NodeId{5U}).resources.canvas;
    COMPOSITOR_CHECK(canvas.width == 1920U && canvas.height == 1080U);
}

// A rebuild discards comment boxes with everything else: one deleted from the
// document, or left over from the project that was open before, is gone.
void a_rebuild_forgets_comment_boxes_the_document_no_longer_holds() {
    const std::vector<doc::Node> plain{stage(5U, 1920U, 1080U),
                                       node(1U, schema::output_type,
                                            {whole(schema::scene_id, 5)})};
    std::vector<doc::Node> boxed = plain;
    boxed.push_back(node(40U, schema::comment_type,
                         {whole(schema::scene_id, 5),
                          text(schema::label, "Plates"),
                          real(schema::comment_width, 400.0),
                          real(schema::comment_height, 240.0)}));

    compositor::CompositorModel model;
    model.reset(root_of(boxed));
    COMPOSITOR_CHECK(model.evaluate(doc::NodeId{5U}).comments.size() == 1U);
    model.reset(root_of(plain));
    COMPOSITOR_CHECK(model.evaluate(doc::NodeId{5U}).comments.empty());
}

// Pasting a copied selection: new ids in order, wires rewritten to the
// copies, everything moved together, and what a graph can hold only one of
// left behind.
void a_paste_copies_nodes_wires_and_boxes() {
    doc::Node routed = edge(30U, 2U, 0U, 3U, 0U);
    routed.properties.insert_or_assign("point_0_x", doc::PropertyValue{150.0});
    routed.properties.insert_or_assign("point_0_y", doc::PropertyValue{75.0});
    const doc::Root root = root_of({
        stage(5U, 1920U, 1080U),
        node(100U, "scene.object"),
        node(1U, schema::scene_layer_type,
             {whole(schema::scene_id, 5), whole(schema::scene_node, 100),
              real(schema::editor_x, 0.0), real(schema::editor_y, 0.0)}),
        node(2U, schema::brightness_type,
             {whole(schema::scene_id, 5), real(schema::editor_x, 100.0),
              real(schema::editor_y, 50.0), real(schema::amount, 1.5)}),
        node(3U, schema::composite_type,
             {whole(schema::scene_id, 5), real(schema::editor_x, 300.0),
              real(schema::editor_y, 80.0)}),
        node(4U, schema::output_type,
             {whole(schema::scene_id, 5), real(schema::editor_x, 600.0),
              real(schema::editor_y, 80.0)}),
        edge(20U, 1U, 0U, 3U, 0U),
        routed,
        edge(31U, 3U, 0U, 4U, 0U),
        node(40U, schema::comment_type,
             {whole(schema::scene_id, 5), real(schema::editor_x, 80.0),
              real(schema::editor_y, 20.0), real(schema::comment_width, 300.0),
              real(schema::comment_height, 200.0)}),
    });
    const std::array<doc::NodeId, 5U> chosen{
        doc::NodeId{1U}, doc::NodeId{2U}, doc::NodeId{3U}, doc::NodeId{4U},
        doc::NodeId{40U}};
    const std::vector<doc::Node> snapshot =
        compositor::snapshot_graph_selection(root, chosen);
    // The five, and the three wires between them; nothing outside.
    COMPOSITOR_CHECK(snapshot.size() == 8U);

    const auto planned = compositor::plan_paste_graph(
        root, snapshot, doc::NodeId{5U}, 1000.0F, 500.0F,
        doc::NodeId{200U});
    COMPOSITOR_CHECK(planned.has_value());
    const compositor::GraphPastePlan& plan = planned.value();
    // The Scene Layer's source already has one, and the graph has its
    // Output: both stay behind, with the wires to them.
    COMPOSITOR_CHECK(plan.skipped == 2U);
    COMPOSITOR_CHECK(plan.remap ==
                     (std::vector<std::pair<doc::NodeId, doc::NodeId>>{
                         {doc::NodeId{2U}, doc::NodeId{200U}},
                         {doc::NodeId{3U}, doc::NodeId{201U}},
                         {doc::NodeId{30U}, doc::NodeId{202U}},
                         {doc::NodeId{40U}, doc::NodeId{203U}}}));
    const auto put = [&](core::u64 id) -> const doc::Node& {
        return *std::find_if(plan.puts.begin(), plan.puts.end(),
                             [id](const doc::Node& one) {
                                 return one.id == doc::NodeId{id};
                             });
    };
    const auto number = [](const doc::Node& one, std::string_view name) {
        return std::get<double>(one.properties.at(std::string{name}));
    };
    COMPOSITOR_CHECK(number(put(200U), schema::editor_x) == 1100.0);
    COMPOSITOR_CHECK(number(put(200U), schema::editor_y) == 550.0);
    COMPOSITOR_CHECK(number(put(200U), schema::amount) == 1.5);
    COMPOSITOR_CHECK(number(put(203U), schema::editor_x) == 1080.0);
    COMPOSITOR_CHECK(std::get<std::int64_t>(put(202U).properties.at(
                         std::string{schema::from_node})) == 200);
    COMPOSITOR_CHECK(std::get<std::int64_t>(put(202U).properties.at(
                         std::string{schema::to_node})) == 201);
    COMPOSITOR_CHECK(number(put(202U), "point_0_x") == 1150.0);
    COMPOSITOR_CHECK(number(put(202U), "point_0_y") == 575.0);

    // Pasted into another scene, the Scene Layer has no rival there and
    // comes too, owned by that scene.
    const doc::Root other = root.set(stage(6U, 1920U, 1080U));
    const auto elsewhere = compositor::plan_paste_graph(
        other, snapshot, doc::NodeId{6U}, 0.0F, 0.0F,
        doc::NodeId{200U});
    COMPOSITOR_CHECK(elsewhere.has_value());
    COMPOSITOR_CHECK(elsewhere.value().skipped == 1U);
    const auto layer = std::find_if(
        elsewhere.value().puts.begin(), elsewhere.value().puts.end(),
        [](const doc::Node& one) { return one.type == schema::scene_layer_type; });
    COMPOSITOR_CHECK(layer != elsewhere.value().puts.end());
    COMPOSITOR_CHECK(std::get<std::int64_t>(layer->properties.at(
                         std::string{schema::scene_id})) == 6);
}

// Output <- Merge 4 (BASE <- layer 2, optionally LAYERS <- layer 3), with
// `merge` authored on the Merge node.
[[nodiscard]] std::vector<doc::Node>
merge_nodes(const std::vector<Property>& merge, bool wire_layers) {
    doc::Node merged = node(4U, schema::merge_type);
    for (const Property& property : merge) {
        merged.properties.insert_or_assign(property.first, property.second);
    }
    std::vector<doc::Node> nodes{
        stage(1000U, 1920U, 1080U), node(100U, "scene.object"),
        node(101U, "scene.object"), scene_layer(2U, 100U),
        scene_layer(3U, 101U), std::move(merged),
        node(5U, schema::output_type), edge(10U, 2U, 0U, 4U, 0U),
        edge(12U, 4U, 0U, 5U, 0U)};
    if (wire_layers) {
        nodes.push_back(edge(11U, 3U, 0U, 4U, 1U));
    }
    return nodes;
}

[[nodiscard]] compositor::CompositorGraph
evaluate_nodes(const std::vector<doc::Node>& nodes,
               compositor::CompositorModel* model = nullptr) {
    compositor::CompositorModel local;
    compositor::CompositorModel& used = model != nullptr ? *model : local;
    used.reset(root_of(nodes));
    return used.evaluate(doc::NodeId{1000U});
}

void merge_is_typed_bounded_and_digested() {
    const auto inputs = compositor::input_ports(compositor::NodeKind::Merge);
    COMPOSITOR_CHECK(inputs.size() == 2U);
    COMPOSITOR_CHECK(inputs[0U].name == "BASE" && inputs[0U].required &&
                     !inputs[0U].multiple);
    COMPOSITOR_CHECK(inputs[1U].name == "LAYERS" && !inputs[1U].required &&
                     inputs[1U].multiple);
    COMPOSITOR_CHECK(
        compositor::output_ports(compositor::NodeKind::Merge).size() == 1U);
    COMPOSITOR_CHECK(compositor::node_kind_bypassable(
        compositor::NodeKind::Merge));
    COMPOSITOR_CHECK(compositor::schema::is_node_type(schema::merge_type));

    // Nothing authored reads as Normal at full opacity with a unit constant,
    // and an unwired LAYERS is not a problem.
    const auto defaults = evaluate_nodes(merge_nodes({}, false));
    COMPOSITOR_CHECK(defaults.valid());
    const auto* const merge = defaults.node(doc::NodeId{4U});
    COMPOSITOR_CHECK(merge != nullptr);
    COMPOSITOR_CHECK(std::get<compositor::MergeParameters>(merge->parameters) ==
                     compositor::MergeParameters{});

    const std::vector<Property> authored{text(schema::blend_mode, "difference"),
                                         real(schema::opacity, 0.25),
                                         real(schema::value, 2.5)};
    const auto graph = evaluate_nodes(merge_nodes(authored, true));
    COMPOSITOR_CHECK(graph.valid());
    const auto& parameters = std::get<compositor::MergeParameters>(
        graph.node(doc::NodeId{4U})->parameters);
    COMPOSITOR_CHECK(parameters.blend ==
                     compositor::MergeBlendMode::Difference);
    COMPOSITOR_CHECK(parameters.opacity == 0.25F);
    COMPOSITOR_CHECK(parameters.value == 2.5F);

    // Each control is its own digest input.
    const core::u64 baseline = compositor::digest(graph);
    for (const Property& changed :
         {text(schema::blend_mode, "darken"), real(schema::opacity, 0.5),
          real(schema::value, 3.0)}) {
        std::vector<Property> edited = authored;
        for (Property& property : edited) {
            if (property.first == changed.first) {
                property.second = changed.second;
            }
        }
        COMPOSITOR_CHECK(compositor::digest(evaluate_nodes(
                             merge_nodes(edited, true))) != baseline);
    }

    // Out-of-range values refuse the node rather than clamp it.
    for (const Property& invalid :
         {text(schema::blend_mode, "burn"), real(schema::opacity, 1.5),
          real(schema::opacity, -0.25), real(schema::value, -0.1),
          real(schema::value, 17.0)}) {
        compositor::CompositorModel model;
        const auto refused =
            evaluate_nodes(merge_nodes({invalid}, true), &model);
        COMPOSITOR_CHECK(refused.node(doc::NodeId{4U}) == nullptr);
        COMPOSITOR_CHECK(model.counters().rejected == 1U);
    }

    // BASE is required; LAYERS is not.
    auto unbased = merge_nodes({}, true);
    unbased.erase(std::find_if(unbased.begin(), unbased.end(),
                               [](const doc::Node& one) {
                                   return one.id == doc::NodeId{10U};
                               }));
    const auto missing = evaluate_nodes(unbased);
    COMPOSITOR_CHECK(!missing.has_errors());
    COMPOSITOR_CHECK(missing.metadata(doc::NodeId{4U}) != nullptr);
    COMPOSITOR_CHECK(missing.metadata(doc::NodeId{4U})->disabled);

    // The constant changes nothing under Normal, at zero opacity, or when it
    // is the blend's own identity.
    using Blend = compositor::MergeBlendMode;
    COMPOSITOR_CHECK(compositor::merge_constant_is_identity({Blend::Normal,
                                                            1.0F, 5.0F}));
    COMPOSITOR_CHECK(compositor::merge_constant_is_identity({Blend::Multiply,
                                                            0.0F, 5.0F}));
    COMPOSITOR_CHECK(compositor::merge_constant_is_identity({Blend::Multiply,
                                                            1.0F, 1.0F}));
    COMPOSITOR_CHECK(compositor::merge_constant_is_identity({Blend::Add,
                                                            1.0F, 0.0F}));
    COMPOSITOR_CHECK(!compositor::merge_constant_is_identity({Blend::Multiply,
                                                             1.0F, 0.5F}));
    COMPOSITOR_CHECK(!compositor::merge_constant_is_identity({Blend::Add,
                                                             0.5F, 0.1F}));
}

// Output <- Background 4.
[[nodiscard]] std::vector<doc::Node>
background_nodes(const std::vector<Property>& background) {
    doc::Node generated = node(4U, schema::background_type);
    for (const Property& property : background) {
        generated.properties.insert_or_assign(property.first, property.second);
    }
    return {stage(1000U, 1920U, 1080U), std::move(generated),
            node(5U, schema::output_type), edge(12U, 4U, 0U, 5U, 0U)};
}

void background_is_typed_bounded_and_digested() {
    COMPOSITOR_CHECK(
        compositor::input_ports(compositor::NodeKind::Background).empty());
    const auto outputs =
        compositor::output_ports(compositor::NodeKind::Background);
    COMPOSITOR_CHECK(outputs.size() == 1U &&
                     outputs[0U].type == compositor::PortValueType::Image);
    COMPOSITOR_CHECK(!compositor::node_kind_bypassable(
        compositor::NodeKind::Background));
    COMPOSITOR_CHECK(compositor::schema::is_node_type(schema::background_type));

    const auto defaults = evaluate_nodes(background_nodes({}));
    COMPOSITOR_CHECK(defaults.valid());
    COMPOSITOR_CHECK(defaults.plan.steps.size() == 2U);
    COMPOSITOR_CHECK(std::get<compositor::BackgroundParameters>(
                         defaults.node(doc::NodeId{4U})->parameters) ==
                     compositor::BackgroundParameters{});
    COMPOSITOR_CHECK(defaults.resources.background_nodes == 1U);

    const std::vector<Property> authored{
        text(schema::background_mode, "radial"),
        real(schema::color_a_r, 0.25), real(schema::color_a_g, 0.5),
        real(schema::color_a_b, 0.75), real(schema::color_a_a, 1.0),
        real(schema::color_b_r, 0.125), real(schema::color_b_g, 0.0),
        real(schema::color_b_b, 1.0), real(schema::color_b_a, 0.5),
        real(schema::angle_turns, 0.375), real(schema::center_x, 0.25),
        real(schema::center_y, -0.5), real(schema::gradient_size, 2.0)};
    const auto graph = evaluate_nodes(background_nodes(authored));
    COMPOSITOR_CHECK(graph.valid());
    const auto& parameters = std::get<compositor::BackgroundParameters>(
        graph.node(doc::NodeId{4U})->parameters);
    COMPOSITOR_CHECK(parameters.mode == compositor::BackgroundMode::Radial);
    COMPOSITOR_CHECK(parameters.color_a ==
                     (compositor::BackgroundColor{0.25F, 0.5F, 0.75F, 1.0F}));
    COMPOSITOR_CHECK(parameters.color_b ==
                     (compositor::BackgroundColor{0.125F, 0.0F, 1.0F, 0.5F}));
    COMPOSITOR_CHECK(parameters.angle == 0.375F);
    COMPOSITOR_CHECK(parameters.center_x == 0.25F);
    COMPOSITOR_CHECK(parameters.center_y == -0.5F);
    COMPOSITOR_CHECK(parameters.size == 2.0F);

    const core::u64 baseline = compositor::digest(graph);
    for (std::size_t index = 0U; index < authored.size(); ++index) {
        std::vector<Property> edited = authored;
        edited[index].second =
            edited[index].first == schema::background_mode
            ? doc::PropertyValue{std::string{"linear"}}
            : doc::PropertyValue{
                  std::get<double>(edited[index].second) == 0.0 ? 0.0625
                                                                : 0.0};
        if (edited[index].first == schema::gradient_size) {
            edited[index].second = doc::PropertyValue{3.0};
        }
        COMPOSITOR_CHECK(compositor::digest(evaluate_nodes(
                             background_nodes(edited))) != baseline);
    }

    // A Background can be a Matte's matte; a Composite cannot.
    const auto matte_of = [](core::u64 matte_source_id,
                             std::string_view matte_source_type) {
        std::vector<doc::Node> nodes{
            stage(1000U, 1920U, 1080U), node(100U, "scene.object"),
            scene_layer(2U, 100U), node(4U, schema::matte_type),
            node(5U, schema::output_type),
            node(matte_source_id, matte_source_type),
            edge(10U, 2U, 0U, 4U, 0U),
            edge(11U, matte_source_id, 0U, 4U, 1U),
            edge(12U, 4U, 0U, 5U, 0U)};
        if (matte_source_type == schema::composite_type) {
            nodes.push_back(node(101U, "scene.object"));
            nodes.push_back(scene_layer(3U, 101U));
            nodes.push_back(edge(13U, 3U, 0U, matte_source_id, 0U));
        }
        return nodes;
    };
    const auto generated_matte =
        evaluate_nodes(matte_of(6U, schema::background_type));
    COMPOSITOR_CHECK(generated_matte.valid());
    COMPOSITOR_CHECK(generated_matte.resources.matte_nodes == 1U);
    const auto stacked_matte =
        evaluate_nodes(matte_of(6U, schema::composite_type));
    COMPOSITOR_CHECK(has_diagnostic(
        stacked_matte, compositor::DiagnosticCode::UnsupportedMatteTopology,
        doc::NodeId{4U}));

    for (const Property& invalid :
         {text(schema::background_mode, "conic"), real(schema::color_a_r, 1.25),
          real(schema::color_b_a, -0.5), real(schema::gradient_size, 0.0),
          real(schema::gradient_size, 65.0), real(schema::center_x, 100.0)}) {
        compositor::CompositorModel model;
        const auto refused = evaluate_nodes(background_nodes({invalid}), &model);
        COMPOSITOR_CHECK(refused.node(doc::NodeId{4U}) == nullptr);
        COMPOSITOR_CHECK(model.counters().rejected == 1U);
    }
}

void merges_nest_into_intermediate_images_and_are_budgeted() {
    // Output <- Merge 4 (BASE <- Background 6, LAYERS <- layer 3).
    std::vector<doc::Node> nodes{
        stage(1000U, 1920U, 1080U), node(101U, "scene.object"),
        scene_layer(3U, 101U), node(4U, schema::merge_type),
        node(5U, schema::output_type), node(6U, schema::background_type),
        edge(10U, 6U, 0U, 4U, 0U), edge(11U, 3U, 0U, 4U, 1U),
        edge(12U, 4U, 0U, 5U, 0U)};
    const auto merged = evaluate_nodes(nodes);
    COMPOSITOR_CHECK(merged.valid());
    COMPOSITOR_CHECK(merged.resources.merge_nodes == 1U);
    COMPOSITOR_CHECK(merged.resources.background_nodes == 1U);
    COMPOSITOR_CHECK(merged.resources.merge_nesting == 1U);
    // One layer and its depth, the frame's ping-pong pair, and the Merge's.
    COMPOSITOR_CHECK(merged.resources.offscreen_textures == 6U);
    COMPOSITOR_CHECK(merged.resources.offscreen_texture_bytes ==
                     1920ULL * 1080ULL * 4ULL * 6ULL);
    // The layer's render and landing, the layer landing on BASE, the
    // Merge landing, and the Background.
    COMPOSITOR_CHECK(merged.resources.render_passes == 5U);

    // A Composite inside a Composite is an image of its own; the outer one,
    // feeding Output, lands its stack straight on the frame.
    const auto nested = evaluate_nodes({
        stage(1000U, 1920U, 1080U), node(101U, "scene.object"),
        scene_layer(3U, 101U), node(4U, schema::composite_type),
        node(5U, schema::output_type), node(6U, schema::composite_type),
        edge(10U, 3U, 0U, 6U, 0U), edge(11U, 6U, 0U, 4U, 0U),
        edge(12U, 4U, 0U, 5U, 0U)});
    COMPOSITOR_CHECK(nested.valid());
    COMPOSITOR_CHECK(nested.resources.merge_nesting == 1U);

    // A Merge with only BASE and a Normal blend is BASE: nothing to open.
    const auto identity = evaluate_nodes(merge_nodes({}, false));
    COMPOSITOR_CHECK(identity.valid());
    COMPOSITOR_CHECK(identity.resources.merge_nesting == 0U);
    COMPOSITOR_CHECK(identity.resources.render_passes == 2U);

    // Each Merge holding the next as its BASE opens one more image.
    const auto chain = [](core::u32 merges) {
        std::vector<doc::Node> result{
            stage(1000U, 640U, 360U), node(100U, "scene.object"),
            scene_layer(2U, 100U), node(5U, schema::output_type)};
        core::u64 below = 2U;
        for (core::u32 index = 0U; index < merges; ++index) {
            const core::u64 merge = 20U + index;
            result.push_back(node(merge, schema::merge_type,
                                  {text(schema::blend_mode, "multiply"),
                                   real(schema::value, 0.5)}));
            result.push_back(edge(40U + index, below, 0U, merge, 0U));
            below = merge;
        }
        result.push_back(edge(60U, below, 0U, 5U, 0U));
        return result;
    };
    const auto deepest = evaluate_nodes(chain(compositor::max_merge_nesting));
    COMPOSITOR_CHECK(deepest.valid());
    COMPOSITOR_CHECK(deepest.resources.merge_nesting ==
                     compositor::max_merge_nesting);
    const auto too_deep =
        evaluate_nodes(chain(compositor::max_merge_nesting + 1U));
    COMPOSITOR_CHECK(!too_deep.valid());
    COMPOSITOR_CHECK(has_diagnostic(
        too_deep, compositor::DiagnosticCode::MergeNestingTooDeep,
        doc::NodeId{5U}));
    COMPOSITOR_CHECK(too_deep.plan.steps.empty());
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 41U> cases{{
    {"an updated model is the model a rebuild would be",
     &an_updated_model_is_the_model_a_rebuild_would_be},
    {"a rebuild forgets comment boxes the document no longer holds",
     &a_rebuild_forgets_comment_boxes_the_document_no_longer_holds},
    {"Merge is typed, bounded and digested",
     &merge_is_typed_bounded_and_digested},
    {"Background is typed, bounded and digested",
     &background_is_typed_bounded_and_digested},
    {"Merges nest into intermediate images and are budgeted",
     &merges_nest_into_intermediate_images_and_are_budgeted},
    {"LED Car setup takes effects and filters apart",
     &led_car_setup_takes_effects_and_filters_apart},
    {"bypass renders as identity and is digested",
     &bypass_renders_as_identity_and_is_digested},
    {"collapse is layout only", &collapse_is_layout_only},
    {"comment boxes are layout in their scene",
     &comment_boxes_are_layout_in_their_scene},
    {"a paste copies nodes, wires and boxes",
     &a_paste_copies_nodes_wires_and_boxes},
    {"wire points are layout that never breaks a wire",
     &wire_points_are_layout_that_never_breaks_a_wire},
    {"a graph belongs to one scene", &a_graph_belongs_to_one_scene},
    {"editing one scene graph leaves the other untouched",
     &editing_one_scene_graph_leaves_the_other_untouched},
    {"a scene copy carries its graph", &a_scene_copy_carries_its_graph},
    {"warnings keep the plan and expose canonical node paths",
     &warnings_keep_the_plan_and_expose_canonical_node_paths},
    {"authored resources are estimated and budgeted without machine state",
     &authored_resources_are_estimated_and_budgeted_without_machine_state},
    {"typed ports describe the editor contract",
     &typed_ports_describe_the_editor_contract},
        {"persistent node catalog includes LED Car Process",
         &persistent_node_catalog_includes_led_car_process},
    {"saturation is typed bounded and digested",
     &saturation_is_typed_bounded_and_digested},
    {"saturation identity is inert and nonidentity folds once",
     &saturation_identity_is_inert_and_nonidentity_folds_once},
    {"saturation checkpoint and streamed deltas match",
     &saturation_checkpoint_and_streamed_deltas_match},
    {"Levels is typed ordered digested and replicated",
     &levels_is_typed_ordered_digested_and_replicated},
    {"Lift Gamma Gain is typed ordered and replicated",
     &lift_gamma_gain_is_typed_ordered_and_replicated},
    {"Contrast Pivot is typed ordered digested and replicated",
     &contrast_pivot_is_typed_ordered_digested_and_replicated},
    {"Color Matrix is typed ordered digested and replicated",
     &color_matrix_is_typed_ordered_digested_and_replicated},
    {"White Balance is typed ordered digested and replicated",
     &white_balance_is_typed_ordered_digested_and_replicated},
    {"Curves are typed ordered digested and replicated",
     &curves_are_typed_ordered_digested_and_replicated},
    {"Film Grain is typed ordered digested and replicated",
     &film_grain_is_typed_ordered_digested_and_replicated},
    {"effects and filters take their own inputs",
     &effects_and_filters_take_their_own_inputs},
    {"duplicate active Scene Layer sources are diagnosed",
     &duplicate_active_scene_layer_sources_are_diagnosed},
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
