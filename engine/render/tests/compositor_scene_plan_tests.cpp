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

[[nodiscard]] scene::MaskProgram rectangle_mask_program(
    core::u64 mask_id, core::u64 target,
    scene::MaskCoordinateSpace space =
        scene::MaskCoordinateSpace::SourceLayer) {
    scene::MaskInstance operation{};
    operation.source = doc::NodeId{mask_id};
    operation.scene = doc::NodeId{1U};
    operation.target = doc::NodeId{target};
    operation.kind = scene::MaskKind::Rectangle;
    operation.combine = scene::MaskCombine::Add;
    operation.bounds = scene::MaskBounds{0.2F, 0.2F, 0.8F, 0.8F};
    return scene::MaskProgram{
        doc::NodeId{target}, space, {std::move(operation)}};
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

void a_track_is_one_stable_compositor_layer() {
    const auto graph = graph_of({
        node(40U, scene::schema::timeline_lane_type),
        node(1U, schema::brightness_type, {real(schema::amount, 2.0)}),
        layer(10U, 40U), node(13U, schema::output_type),
        edge(20U, 1U, 0U, 10U, 0U), edge(21U, 10U, 0U, 13U, 0U),
    });
    PLAN_CHECK(graph.valid());

    scene::Scene source{};
    scene::SceneScreen screen{};
    screen.node = doc::NodeId{40U};
    screen.name = "Program Track";
    screen.world = scene::identity();
    source.screens.push_back(screen);
    scene::MediaInstance clip{};
    clip.source_node = doc::NodeId{100U};
    clip.timeline_lane = screen.node;
    clip.world = screen.world;
    clip.normal = scene::identity();
    source.media.push_back(clip);

    const auto active = render::plan_compositor_scene(graph, source);
    PLAN_CHECK(active.has_value());
    PLAN_CHECK(active.value().layers.size() == 1U);
    PLAN_CHECK(active.value().layers.front().graph_node == doc::NodeId{10U});
    PLAN_CHECK(active.value().layers.front().scene_node == screen.node);
    PLAN_CHECK(active.value().layers.front().scene_nodes ==
               std::vector<doc::NodeId>{doc::NodeId{100U}});
    PLAN_CHECK(active.value().adjusted_scene.media.front()
                   .style.effects.brightness_enabled);
    PLAN_CHECK(std::abs(active.value().adjusted_scene.media.front()
                            .style.effects.brightness -
                        2.0F) < 0.001F);

    // A gap in the Track produces transparency rather than invalidating the
    // graph, so its node stays usable across cuts and empty timeline spans.
    source.media.clear();
    const auto empty = render::plan_compositor_scene(graph, source);
    PLAN_CHECK(empty.has_value());
    PLAN_CHECK(empty.value().layers.size() == 1U);
    PLAN_CHECK(empty.value().layers.front().scene_node == screen.node);
    PLAN_CHECK(empty.value().layers.front().scene_nodes.empty());
}

void led_car_process_arranges_nine_additive_plate_layers() {
    std::vector<doc::Node> nodes;
    for (core::u64 index = 0U; index < 9U; ++index) {
        nodes.push_back(node(100U + index, "scene.object"));
        nodes.push_back(layer(10U + index, 100U + index));
    }
    nodes.push_back(
        node(1U, schema::brightness_type, {real(schema::amount, 2.0)}));
    nodes.push_back(node(
        30U, schema::led_car_process_type,
        {real(schema::position_x, 10.0), real(schema::position_y, 2.0),
         real(schema::position_z, -3.0),
         real(schema::rotation_yaw_turns, 0.125),
         real(schema::plate_scale, 2.0),
         real(schema::array_scale, 4.0),
         real(schema::reflection_height, 7.0),
         real(schema::reflection_scale, 3.0),
         real(schema::horizontal_blend, 0.1)}));
    nodes.push_back(node(31U, schema::output_type));
    for (core::u64 index = 0U; index < 9U; ++index) {
        nodes.push_back(edge(40U + index, 10U + index, 0U, 30U,
                             static_cast<core::u8>(index)));
    }
    nodes.push_back(edge(50U, 1U, 0U, 30U, 9U));
    nodes.push_back(edge(51U, 30U, 0U, 31U, 0U));

    const auto graph = graph_of(nodes);
    PLAN_CHECK(graph.valid());
    const auto* const setup = graph.node(doc::NodeId{30U});
    PLAN_CHECK(setup != nullptr);
    PLAN_CHECK(std::get<compositor::LedCarProcessParameters>(
                   setup->parameters)
                   .horizontal_blend == 0.1F);

    scene::Scene source{};
    for (core::u64 index = 0U; index < 9U; ++index) {
        scene::MeshInstance plate =
            instance(100U + index, scene::BlendMode::Normal);
        plate.world = scene::translation(scene::Vec3{1.0F, 0.0F, 0.0F});
        plate.normal = scene::normal_basis(plate.world);
        source.instances.push_back(std::move(plate));
    }
    source.instances.front().style.effects.brightness_enabled = true;
    source.instances.front().style.effects.brightness = 0.5F;
    source.mask_programs.push_back(rectangle_mask_program(700U, 100U));

    const auto built = render::plan_compositor_scene(graph, source);
    PLAN_CHECK(built.has_value());
    const auto& plan = built.value();
    PLAN_CHECK(plan.layers.size() == 9U);
    for (std::size_t index = 0U; index < plan.layers.size(); ++index) {
        PLAN_CHECK(plan.layers[index].graph_node ==
                   doc::NodeId{10U + static_cast<core::u64>(index)});
    }
    PLAN_CHECK(plan.layers.front().mask_program.has_value());
    PLAN_CHECK(plan.layers.front().mask_program->target == doc::NodeId{100U});

    const auto front = std::find_if(
        plan.adjusted_scene.instances.begin(),
        plan.adjusted_scene.instances.end(), [](const auto& plate) noexcept {
            return plate.source == doc::NodeId{100U};
        });
    const auto ceiling = std::find_if(
        plan.adjusted_scene.instances.begin(),
        plan.adjusted_scene.instances.end(), [](const auto& plate) noexcept {
            return plate.source == doc::NodeId{108U};
        });
    PLAN_CHECK(front != plan.adjusted_scene.instances.end());
    PLAN_CHECK(ceiling != plan.adjusted_scene.instances.end());
    PLAN_CHECK(std::abs(front->style.horizontal_blend - 0.1F) < 0.0001F);
    PLAN_CHECK(ceiling->style.horizontal_blend == 0.0F);
    PLAN_CHECK(front->style.effects.inline_operations.size() == 2U);
    PLAN_CHECK(front->style.effects.inline_operations[0U].kind ==
               scene::EffectKind::Brightness);
    PLAN_CHECK(front->style.effects.inline_operations[1U].kind ==
               scene::EffectKind::Brightness);
    PLAN_CHECK(std::abs(front->style.effects.brightness - 1.0F) < 0.0001F);

    const scene::Mat4 setup_world = scene::multiply(
        scene::translation(scene::Vec3{10.0F, 2.0F, -3.0F}),
        scene::rotation(0.125F, 0.0F, 0.0F));
    const scene::Mat4 front_slot = scene::multiply(
        scene::translation(scene::Vec3{0.0F, 0.0F, 4.0F}),
        scene::multiply(scene::rotation(0.5F, 0.0F, 0.0F),
                        scene::scaling(scene::Vec3{2.0F, 2.0F, 2.0F})));
    const scene::Mat4 expected_front = scene::multiply(
        scene::multiply(setup_world, front_slot),
        scene::translation(scene::Vec3{1.0F, 0.0F, 0.0F}));
    PLAN_CHECK(front->world == expected_front);

    const scene::Vec3 ceiling_origin =
        scene::transform_point(ceiling->world, scene::Vec3{});
    PLAN_CHECK(std::abs(ceiling_origin.y - 9.0F) < 0.0001F);
    const scene::Vec3 ceiling_x =
        scene::transform_point(ceiling->world, scene::Vec3{1.0F, 0.0F, 0.0F}) -
        ceiling_origin;
    PLAN_CHECK(std::abs(scene::length(ceiling_x) - 3.0F) < 0.0001F);
}

void led_car_process_accepts_unpopulated_plate_sockets() {
    std::vector<doc::Node> nodes{
        node(100U, "scene.object"),
        layer(10U, 100U),
        node(108U, "scene.object"),
        layer(18U, 108U),
        node(30U, schema::led_car_process_type),
        node(31U, schema::output_type),
        edge(40U, 10U, 0U, 30U, 0U),
        edge(48U, 18U, 0U, 30U, 8U),
        edge(51U, 30U, 0U, 31U, 0U),
    };
    const auto graph = graph_of(nodes);
    PLAN_CHECK(graph.valid());

    scene::Scene source{};
    source.instances.push_back(instance(100U, scene::BlendMode::Normal));
    source.instances.push_back(instance(108U, scene::BlendMode::Normal));
    const auto built = render::plan_compositor_scene(graph, source);
    PLAN_CHECK(built.has_value());
    PLAN_CHECK(built.value().layers.size() == 2U);
    PLAN_CHECK(built.value().layers[0U].scene_node == doc::NodeId{100U});
    PLAN_CHECK(built.value().layers[1U].scene_node == doc::NodeId{108U});
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

void equal_depth_object_model_and_clip_layers_follow_sibling_order() {
    scene::Scene source{};
    source.camera.eye = scene::Vec3{0.0F, 0.0F, 5.0F};
    source.camera.target = scene::Vec3{};
    auto object = instance(100U, scene::BlendMode::Screen);
    object.group_order = 5;
    source.instances.push_back(object);
    scene::ModelInstance model{};
    model.world = scene::identity();
    model.normal = scene::identity();
    model.source = "fixture.glb";
    model.source_node = doc::NodeId{10U};
    model.style.blend = scene::BlendMode::Screen;
    model.group_order = 1;
    source.models.push_back(model);
    scene::MediaInstance clip{};
    clip.world = scene::identity();
    clip.normal = scene::identity();
    clip.source_node = doc::NodeId{50U};
    clip.style.blend = scene::BlendMode::Screen;
    clip.group_order = 3;
    source.media.push_back(clip);

    const auto ordered = render::plan_scene_effects(source);
    PLAN_CHECK(ordered.has_value());
    PLAN_CHECK(ordered.value().layers.size() == 3U);
    PLAN_CHECK(ordered.value().layers[0U].scene_node == doc::NodeId{10U});
    PLAN_CHECK(ordered.value().layers[1U].scene_node == doc::NodeId{50U});
    PLAN_CHECK(ordered.value().layers[2U].scene_node == doc::NodeId{100U});

    source.instances.front().group_order = 0;
    source.models.front().group_order = 9;
    const auto reversed = render::plan_scene_effects(source);
    PLAN_CHECK(reversed.has_value());
    PLAN_CHECK(reversed.value().layers[0U].scene_node == doc::NodeId{100U});
    PLAN_CHECK(reversed.value().layers[1U].scene_node == doc::NodeId{50U});
    PLAN_CHECK(reversed.value().layers[2U].scene_node == doc::NodeId{10U});
}

void keyers_isolate_in_authored_order_and_invalid_active_payloads_fail() {
    scene::Scene source{};
    source.instances.push_back(instance(10U, scene::BlendMode::Normal));
    auto keyed = instance(20U, scene::BlendMode::Normal);
    scene::SpatialEffectOperation chroma{};
    chroma.kind = scene::EffectKind::ChromaKey;
    chroma.chroma_key = scene::ChromaKeyParameters{
        {0.0F, 1.0F, 0.0F}, 0.15F, 0.05F, 2, 0.4F, 0.75F};
    scene::SpatialEffectOperation luma{};
    luma.kind = scene::EffectKind::LumaKey;
    luma.luma_key = scene::LumaKeyParameters{
        0.2F, 0.8F, 0.1F, 0.1F, false, 0.5F};
    keyed.style.effects.spatial_operations = {chroma, luma};
    source.instances.push_back(keyed);

    const auto built = render::plan_scene_effects(source);
    PLAN_CHECK(built.has_value());
    PLAN_CHECK(built.value().layers.size() == 2U);
    const auto keyed_pass = std::find_if(
        built.value().layers.begin(), built.value().layers.end(),
        [](const render::CompositorLayerPass& pass) noexcept {
            return pass.scene_node == doc::NodeId{20U};
        });
    PLAN_CHECK(keyed_pass != built.value().layers.end());
    PLAN_CHECK(keyed_pass->spatial_effects ==
               std::vector<scene::SpatialEffectOperation>({chroma, luma}));
    PLAN_CHECK(keyed_pass->depth_only_nodes ==
               std::vector<doc::NodeId>({doc::NodeId{10U}}));

    source.instances.back().style.effects
        .spatial_operations.front().chroma_key.tolerance =
        std::numeric_limits<float>::quiet_NaN();
    const auto invalid = render::plan_scene_effects(source);
    PLAN_CHECK(!invalid.has_value());
    PLAN_CHECK(invalid.error() ==
               render::CompositorScenePlanError::InvalidConfiguration);

    source.instances.back().style.effects.spatial_operations.clear();
    chroma.chroma_key.mix = 0.0F;
    chroma.chroma_key.tolerance =
        std::numeric_limits<float>::quiet_NaN();
    source.instances.back().style.effects.spatial_operations.push_back(chroma);
    const auto identity = render::plan_scene_effects(source);
    PLAN_CHECK(identity.has_value());
    PLAN_CHECK(identity.value().layers.size() == 1U);
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
    identity.instances.front().style.effects.saturation_enabled = true;
    identity.instances.front().style.effects.saturation = 1.0F;
    identity.instances.front().style.effects.blur_enabled = true;
    identity.instances.front().style.effects.blur_radius = 0.0F;
    identity.instances.front().style.effects.inline_operations.push_back(
        scene::InlineEffectOperation{
            scene::EffectKind::Levels, 1.0F, scene::LevelsParameters{}});
    identity.instances.front().style.effects.inline_operations.push_back(
        scene::InlineEffectOperation{
            scene::EffectKind::LiftGammaGain, 1.0F, {},
            scene::LiftGammaGainParameters{}});
    identity.instances.front().style.effects.inline_operations.push_back(
        scene::InlineEffectOperation{
            scene::EffectKind::ContrastPivot, 1.0F, {}, {},
            scene::ContrastPivotParameters{1.0F, 0.9F}});
    identity.instances.front().style.effects.inline_operations.push_back(
        scene::InlineEffectOperation{
            scene::EffectKind::ColorMatrix, 1.0F, {}, {}, {},
            scene::ColorMatrixParameters{}});
    identity.instances.front().style.effects.inline_operations.push_back(
        scene::InlineEffectOperation{
            scene::EffectKind::WhiteBalance, 1.0F, {}, {}, {}, {},
            scene::WhiteBalanceParameters{}});
    identity.instances.front().style.effects.inline_operations.push_back(
        scene::InlineEffectOperation{
            scene::EffectKind::Curves, 1.0F, {}, {}, {}, {}, {},
            scene::CurvesParameters{}});
    {
        scene::InlineEffectOperation grain{};
        grain.kind = scene::EffectKind::FilmGrain;
        grain.film_grain = scene::FilmGrainParameters{};
        identity.instances.front().style.effects.inline_operations.push_back(
            grain);
    }

    const auto plain_plan = render::plan_scene_effects(plain);
    const auto identity_plan = render::plan_scene_effects(identity);
    PLAN_CHECK(plain_plan.has_value());
    PLAN_CHECK(identity_plan.has_value());
    PLAN_CHECK(identity_plan.value().layers == plain_plan.value().layers);
    PLAN_CHECK(identity_plan.value().layers.size() == 1U);
}

void levels_remains_inline_and_invalid_parameters_fail_planning() {
    scene::Scene source{};
    auto effected = instance(10U, scene::BlendMode::Normal);
    scene::LevelsParameters levels{};
    levels.master.gamma = 2.0F;
    effected.style.effects.inline_operations.push_back(
        scene::InlineEffectOperation{
            scene::EffectKind::Levels, 1.0F, levels});
    source.instances.push_back(effected);

    const auto inline_plan = render::plan_scene_effects(source);
    PLAN_CHECK(inline_plan.has_value());
    PLAN_CHECK(inline_plan.value().layers.size() == 1U);
    PLAN_CHECK(inline_plan.value().layers.front().blur_radius_px == 0.0F);
    PLAN_CHECK(inline_plan.value().adjusted_scene.instances.front()
                   .style.effects.inline_operations.front().levels == levels);

    source.instances.front().style.effects.inline_operations.front()
        .levels.master.input_black = 1.0F;
    source.instances.front().style.effects.inline_operations.front()
        .levels.master.input_white = 1.0F;
    const auto invalid = render::plan_scene_effects(source);
    PLAN_CHECK(!invalid.has_value());
    PLAN_CHECK(invalid.error() ==
               render::CompositorScenePlanError::InvalidConfiguration);
}

void lift_gamma_gain_remains_inline_and_invalid_payloads_fail_planning() {
    scene::Scene source{};
    auto effected = instance(10U, scene::BlendMode::Normal);
    scene::LiftGammaGainParameters parameters{};
    parameters.lift.x = 0.1F;
    parameters.gamma.y = 2.0F;
    parameters.gain.z = 1.5F;
    effected.style.effects.inline_operations.push_back(
        scene::InlineEffectOperation{
            scene::EffectKind::LiftGammaGain, 1.0F, {}, parameters});
    source.instances.push_back(effected);

    const auto inline_plan = render::plan_scene_effects(source);
    PLAN_CHECK(inline_plan.has_value());
    PLAN_CHECK(inline_plan.value().layers.size() == 1U);
    PLAN_CHECK(inline_plan.value().layers.front().blur_radius_px == 0.0F);
    PLAN_CHECK(inline_plan.value().adjusted_scene.instances.front()
                   .style.effects.inline_operations.front().lift_gamma_gain ==
               parameters);

    source.instances.front().style.effects.inline_operations.front()
        .lift_gamma_gain.gamma.z = std::numeric_limits<float>::infinity();
    const auto invalid = render::plan_scene_effects(source);
    PLAN_CHECK(!invalid.has_value());
    PLAN_CHECK(invalid.error() ==
               render::CompositorScenePlanError::InvalidConfiguration);
}

void contrast_pivot_remains_inline_and_invalid_payloads_fail_planning() {
    scene::Scene source{};
    auto effected = instance(10U, scene::BlendMode::Normal);
    const scene::ContrastPivotParameters parameters{1.5F, 0.35F};
    effected.style.effects.inline_operations.push_back(
        scene::InlineEffectOperation{
            scene::EffectKind::ContrastPivot, 1.0F, {}, {}, parameters});
    source.instances.push_back(effected);

    const auto inline_plan = render::plan_scene_effects(source);
    PLAN_CHECK(inline_plan.has_value());
    PLAN_CHECK(inline_plan.value().layers.size() == 1U);
    PLAN_CHECK(inline_plan.value().layers.front().blur_radius_px == 0.0F);
    PLAN_CHECK(inline_plan.value().adjusted_scene.instances.front()
                   .style.effects.inline_operations.front().contrast_pivot ==
               parameters);

    source.instances.front().style.effects.inline_operations.front()
        .contrast_pivot.contrast = std::numeric_limits<float>::infinity();
    const auto invalid = render::plan_scene_effects(source);
    PLAN_CHECK(!invalid.has_value());
    PLAN_CHECK(invalid.error() ==
               render::CompositorScenePlanError::InvalidConfiguration);
}

void color_matrix_remains_inline_and_invalid_payloads_fail_planning() {
    scene::Scene source{};
    auto effected = instance(10U, scene::BlendMode::Normal);
    scene::ColorMatrixParameters parameters{};
    parameters.matrix[1U] = 0.25F;
    parameters.offset.z = 0.1F;
    effected.style.effects.inline_operations.push_back(
        scene::InlineEffectOperation{
            scene::EffectKind::ColorMatrix, 1.0F, {}, {}, {}, parameters});
    source.instances.push_back(effected);

    const auto inline_plan = render::plan_scene_effects(source);
    PLAN_CHECK(inline_plan.has_value());
    PLAN_CHECK(inline_plan.value().layers.size() == 1U);
    PLAN_CHECK(inline_plan.value().layers.front().blur_radius_px == 0.0F);
    PLAN_CHECK(inline_plan.value().adjusted_scene.instances.front()
                   .style.effects.inline_operations.front().color_matrix ==
               parameters);

    source.instances.front().style.effects.inline_operations.front()
        .color_matrix.offset.x = std::numeric_limits<float>::infinity();
    const auto invalid = render::plan_scene_effects(source);
    PLAN_CHECK(!invalid.has_value());
    PLAN_CHECK(invalid.error() ==
               render::CompositorScenePlanError::InvalidConfiguration);
}

void white_balance_remains_inline_and_invalid_payloads_fail_planning() {
    scene::Scene source{};
    auto effected = instance(10U, scene::BlendMode::Normal);
    const auto parameters = scene::normalized_white_balance(
        scene::WhiteBalanceParameters{4'000.0F, 0.25F});
    PLAN_CHECK(parameters.has_value());
    effected.style.effects.inline_operations.push_back(
        scene::InlineEffectOperation{
            scene::EffectKind::WhiteBalance, 1.0F, {}, {}, {}, {},
            *parameters});
    source.instances.push_back(effected);

    const auto inline_plan = render::plan_scene_effects(source);
    PLAN_CHECK(inline_plan.has_value());
    PLAN_CHECK(inline_plan.value().layers.size() == 1U);
    PLAN_CHECK(inline_plan.value().layers.front().blur_radius_px == 0.0F);
    PLAN_CHECK(inline_plan.value().adjusted_scene.instances.front()
                   .style.effects.inline_operations.front().white_balance ==
               *parameters);

    source.instances.front().style.effects.inline_operations.front()
        .white_balance.temperature_kelvin =
            std::numeric_limits<float>::infinity();
    const auto invalid = render::plan_scene_effects(source);
    PLAN_CHECK(!invalid.has_value());
    PLAN_CHECK(invalid.error() ==
               render::CompositorScenePlanError::InvalidConfiguration);
}

void curves_remain_inline_and_invalid_payloads_fail_planning() {
    scene::Scene source{};
    auto effected = instance(10U, scene::BlendMode::Normal);
    scene::CurvesParameters authored{};
    authored.master.points = {
        {0.0F, 0.0F}, {0.5F, 0.25F}, {1.0F, 1.0F}};
    const auto parameters = scene::normalized_curves(authored);
    PLAN_CHECK(parameters.has_value());
    effected.style.effects.inline_operations.push_back(
        scene::InlineEffectOperation{
            scene::EffectKind::Curves, 1.0F, {}, {}, {}, {}, {},
            *parameters});
    source.instances.push_back(effected);

    const auto inline_plan = render::plan_scene_effects(source);
    PLAN_CHECK(inline_plan.has_value());
    PLAN_CHECK(inline_plan.value().layers.size() == 1U);
    PLAN_CHECK(inline_plan.value().layers.front().blur_radius_px == 0.0F);
    PLAN_CHECK(inline_plan.value().adjusted_scene.instances.front()
                   .style.effects.inline_operations.front().curves ==
               *parameters);

    source.instances.front().style.effects.inline_operations.front()
        .curves.master.points[1U].x = 0.0F;
    const auto invalid = render::plan_scene_effects(source);
    PLAN_CHECK(!invalid.has_value());
    PLAN_CHECK(invalid.error() ==
               render::CompositorScenePlanError::InvalidConfiguration);
}

void film_grain_remains_inline_and_invalid_payloads_fail_planning() {
    scene::Scene source{};
    auto effected = instance(10U, scene::BlendMode::Normal);
    const scene::FilmGrainParameters parameters{
        0.3F, 4.0F, 0.75F, 0xABCD'1234U,
        scene::FilmGrainMode::RGB};
    scene::InlineEffectOperation operation{};
    operation.kind = scene::EffectKind::FilmGrain;
    operation.film_grain = parameters;
    effected.style.effects.inline_operations.push_back(operation);
    source.instances.push_back(effected);

    const auto inline_plan = render::plan_scene_effects(source);
    PLAN_CHECK(inline_plan.has_value());
    PLAN_CHECK(inline_plan.value().layers.size() == 1U);
    PLAN_CHECK(inline_plan.value().layers.front().blur_radius_px == 0.0F);
    PLAN_CHECK(inline_plan.value().adjusted_scene.instances.front()
                   .style.effects.inline_operations.front().film_grain ==
               parameters);

    source.instances.front().style.effects.inline_operations.front()
        .film_grain.chroma_ratio =
            std::numeric_limits<float>::quiet_NaN();
    const auto invalid = render::plan_scene_effects(source);
    PLAN_CHECK(!invalid.has_value());
    PLAN_CHECK(invalid.error() ==
               render::CompositorScenePlanError::InvalidConfiguration);
}

void reverse_domain_effect_order_is_rejected_explicitly() {
    scene::Scene source{};
    auto effected = instance(10U, scene::BlendMode::Normal);
    effected.style.effects.saturation_enabled = true;
    effected.style.effects.saturation = 0.5F;
    effected.style.effects.blur_enabled = true;
    effected.style.effects.blur_radius = 4.0F;
    source.instances.push_back(effected);
    source.effects.push_back(scene::EffectInstance{
        doc::NodeId{200U}, doc::NodeId{10U}, scene::EffectKind::Blur, 0, true,
        4.0F});
    source.effects.push_back(scene::EffectInstance{
        doc::NodeId{100U}, doc::NodeId{10U},
        scene::EffectKind::Saturation, 1, true, 0.5F});

    const auto rejected = render::plan_scene_effects(source);
    PLAN_CHECK(!rejected.has_value());
    PLAN_CHECK(rejected.error() ==
               render::CompositorScenePlanError::InvalidConfiguration);

    // Authored order, not document identity, decides. Reversing the node IDs
    // while placing the inline operation first remains executable.
    source.effects[0U].order = 1;
    source.effects[1U].order = 0;
    const auto accepted = render::plan_scene_effects(source);
    PLAN_CHECK(accepted.has_value());
    PLAN_CHECK(accepted.value().layers.size() == 1U);

    // An identity inline node after Blur is inert and does not make an
    // otherwise supported stack fail.
    source.effects[1U].order = 2;
    source.effects[1U].value = 1.0F;
    const auto identity = render::plan_scene_effects(source);
    PLAN_CHECK(identity.has_value());
}

void scene_and_graph_effect_order_is_validated_as_one_stack() {
    const auto active_inline_graph = graph_of({
        node(10U, "scene.object"),
        node(1U, schema::saturation_type, {real(schema::amount, 0.5)}),
        layer(2U, 10U),
        node(3U, schema::output_type),
        edge(4U, 1U, 0U, 2U, 0U),
        edge(5U, 2U, 0U, 3U, 0U),
    });
    PLAN_CHECK(active_inline_graph.valid());

    scene::Scene source{};
    auto blurred = instance(10U, scene::BlendMode::Normal);
    blurred.style.effects.blur_enabled = true;
    blurred.style.effects.blur_radius = 4.0F;
    source.instances.push_back(blurred);
    source.effects.push_back(scene::EffectInstance{
        doc::NodeId{100U}, doc::NodeId{10U}, scene::EffectKind::Blur, 0, true,
        4.0F});
    const auto rejected =
        render::plan_compositor_scene(active_inline_graph, source);
    PLAN_CHECK(!rejected.has_value());
    PLAN_CHECK(rejected.error() ==
               render::CompositorScenePlanError::InvalidConfiguration);

    const auto identity_inline_graph = graph_of({
        node(10U, "scene.object"),
        node(1U, schema::saturation_type, {real(schema::amount, 1.0)}),
        layer(2U, 10U),
        node(3U, schema::output_type),
        edge(4U, 1U, 0U, 2U, 0U),
        edge(5U, 2U, 0U, 3U, 0U),
    });
    const auto identity =
        render::plan_compositor_scene(identity_inline_graph, source);
    PLAN_CHECK(identity.has_value());
    PLAN_CHECK(identity.value().adjusted_scene.instances.front().style.effects ==
               source.instances.front().style.effects);

    const auto blur_graph = graph_of({
        node(10U, "scene.object"),
        node(1U, schema::blur_type, {real(schema::radius, 4.0)}),
        layer(2U, 10U),
        node(3U, schema::output_type),
        edge(4U, 1U, 0U, 2U, 0U),
        edge(5U, 2U, 0U, 3U, 0U),
    });
    source.effects.clear();
    source.instances.front().style.effects.blur_enabled = false;
    source.instances.front().style.effects.blur_radius = 0.0F;
    source.instances.front().style.effects.saturation_enabled = true;
    source.instances.front().style.effects.saturation = 0.5F;
    source.effects.push_back(scene::EffectInstance{
        doc::NodeId{100U}, doc::NodeId{10U}, scene::EffectKind::Saturation, 0,
        true, 0.5F});
    PLAN_CHECK(render::plan_compositor_scene(blur_graph, source).has_value());
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

void drawable_masks_isolate_once_and_identity_is_plan_inert() {
    scene::Scene source{};
    source.instances.push_back(instance(10U, scene::BlendMode::Normal));
    source.instances.push_back(instance(20U, scene::BlendMode::Normal));
    source.instances.push_back(instance(30U, scene::BlendMode::Normal));
    source.mask_programs.push_back(rectangle_mask_program(100U, 20U));

    const auto built = render::plan_scene_effects(source);
    PLAN_CHECK(built.has_value());
    PLAN_CHECK(built.value().layers.size() == 2U);
    PLAN_CHECK(built.value().layers[0U].scene_nodes ==
               std::vector<doc::NodeId>(
                   {doc::NodeId{10U}, doc::NodeId{30U}}));
    PLAN_CHECK(built.value().layers[0U].depth_only_nodes.empty());
    const auto masked = std::find_if(
        built.value().layers.begin(), built.value().layers.end(),
        [](const render::CompositorLayerPass& pass) noexcept {
            return pass.scene_node == doc::NodeId{20U};
        });
    PLAN_CHECK(masked != built.value().layers.end());
    PLAN_CHECK(masked->mask_program == source.mask_programs.front());
    PLAN_CHECK(masked->depth_only_nodes ==
               std::vector<doc::NodeId>(
                   {doc::NodeId{10U}, doc::NodeId{30U}}));

    const auto graph = graph_of({
        node(20U, "scene.object"), layer(200U, 20U),
        node(201U, schema::output_type), edge(202U, 200U, 0U, 201U, 0U)});
    PLAN_CHECK(graph.valid());
    const auto graph_built = render::plan_compositor_scene(graph, source);
    PLAN_CHECK(graph_built.has_value());
    const auto graph_masked = std::find_if(
        graph_built.value().layers.begin(),
        graph_built.value().layers.end(),
        [](const render::CompositorLayerPass& pass) noexcept {
            return pass.scene_node == doc::NodeId{20U};
        });
    PLAN_CHECK(graph_masked != graph_built.value().layers.end());
    PLAN_CHECK(graph_masked->mask_program == source.mask_programs.front());

    source.mask_programs.clear();
    scene::MaskProgram identity{};
    identity.target = doc::NodeId{999U};
    identity.coordinate_space =
        static_cast<scene::MaskCoordinateSpace>(255U);
    source.mask_programs.push_back(identity);
    const auto identity_plan = render::plan_scene_effects(source);
    PLAN_CHECK(identity_plan.has_value());
    PLAN_CHECK(identity_plan.value().layers.size() == 1U);
    PLAN_CHECK(!identity_plan.value().layers.front().mask_program.has_value());
}

void group_and_malformed_mask_programs_fail_closed() {
    scene::Scene source{};
    auto child = instance(10U, scene::BlendMode::Normal);
    child.parent_group = doc::NodeId{500U};
    source.instances.push_back(child);
    scene::SceneGroup group{};
    group.node = doc::NodeId{500U};
    group.scene = doc::NodeId{1U};
    source.groups.push_back(group);
    // A group is never itself a drawable, so this also pins the order of the
    // two tests inside collect_mask_programs: the unsupported coordinate space
    // is judged before coverage, or a group mask would be quietly skipped and
    // the group would render unmasked.
    source.mask_programs.push_back(rectangle_mask_program(
        100U, 500U, scene::MaskCoordinateSpace::AuthoredCanvas));
    const auto group_plan = render::plan_scene_effects(source);
    PLAN_CHECK(!group_plan.has_value());
    PLAN_CHECK(group_plan.error() ==
               render::CompositorScenePlanError::InvalidConfiguration);

    source.mask_programs = {rectangle_mask_program(101U, 10U)};
    source.mask_programs.front().operations.front().bounds.left = 0.9F;
    source.mask_programs.front().operations.front().bounds.right = 0.1F;
    const auto malformed = render::plan_scene_effects(source);
    PLAN_CHECK(!malformed.has_value());
    PLAN_CHECK(malformed.error() ==
               render::CompositorScenePlanError::InvalidConfiguration);

    source.mask_programs = {rectangle_mask_program(102U, 10U),
                            rectangle_mask_program(103U, 10U)};
    const auto duplicate = render::plan_scene_effects(source);
    PLAN_CHECK(!duplicate.has_value());
    PLAN_CHECK(duplicate.error() ==
               render::CompositorScenePlanError::InvalidConfiguration);

}

void a_mask_with_nothing_to_cut_is_skipped_rather_than_fatal() {
    // The reported defect exactly: a clip carrying a mask leaves the picture
    // when the playhead moves past it, and the whole frame stopped rendering -
    // in the editor's Program Output and on the wall, which share this plan.
    // A mask whose layer is not in the frame has nothing to cut, so it is
    // skipped and the rest of the picture renders unchanged.
    scene::Scene source{};
    source.instances.push_back(instance(10U, scene::BlendMode::Normal));
    scene::MediaInstance clip{};
    clip.source_node = doc::NodeId{20U};
    clip.world = scene::identity();
    source.media.push_back(clip);
    source.mask_programs.push_back(rectangle_mask_program(100U, 20U));

    // On air, the mask still reaches its layer.
    const auto on_air = render::plan_scene_effects(source);
    PLAN_CHECK(on_air.has_value());
    const auto masked = std::find_if(
        on_air.value().layers.begin(), on_air.value().layers.end(),
        [](const render::CompositorLayerPass& pass) noexcept {
            return pass.scene_node == doc::NodeId{20U};
        });
    PLAN_CHECK(masked != on_air.value().layers.end());
    PLAN_CHECK(masked->mask_program == source.mask_programs.front());

    // Off air the placement is simply absent from the evaluated scene. The
    // frame still plans, and nothing claims to be masked.
    source.media.clear();
    const auto off_air = render::plan_scene_effects(source);
    PLAN_CHECK(off_air.has_value());
    PLAN_CHECK(off_air.value().layers.size() == 1U);
    PLAN_CHECK(!off_air.value().layers.front().mask_program.has_value());
    PLAN_CHECK(off_air.value().layers.front().scene_nodes ==
               std::vector<doc::NodeId>({doc::NodeId{10U}}));

    // The same holds through a compositor graph, which is the path Program
    // Output takes whenever a scene has one.
    const auto graph = graph_of({
        node(10U, "scene.object"), layer(200U, 10U),
        node(201U, schema::output_type), edge(202U, 200U, 0U, 201U, 0U)});
    PLAN_CHECK(graph.valid());
    const auto graphed = render::plan_compositor_scene(graph, source);
    PLAN_CHECK(graphed.has_value());

    // A mask on a target that never existed behaves the same way. Scene
    // evaluation drops and diagnoses those before the renderer sees them, so
    // reaching here at all means a hand-built scene, and blanking the output
    // is still the wrong answer.
    source.mask_programs = {rectangle_mask_program(104U, 999U)};
    const auto absent = render::plan_scene_effects(source);
    PLAN_CHECK(absent.has_value());
    PLAN_CHECK(!absent.value().layers.front().mask_program.has_value());
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

constexpr std::array<Case, 31U> cases{{
    {"a flat graph becomes an ordered layer plan",
     &a_flat_graph_becomes_an_ordered_layer_plan},
    {"a direct scene layer preserves unconnected drawables",
     &a_direct_scene_layer_preserves_unconnected_drawables},
    {"a Track is one stable compositor layer",
     &a_track_is_one_stable_compositor_layer},
    {"LED car process arranges nine additive plate layers",
     &led_car_process_arranges_nine_additive_plate_layers},
    {"LED car process accepts unpopulated plate sockets",
     &led_car_process_accepts_unpopulated_plate_sockets},
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
    {"equal depth object model and clip layers follow sibling order",
     &equal_depth_object_model_and_clip_layers_follow_sibling_order},
    {"keyers isolate in authored order and invalid active payloads fail",
     &keyers_isolate_in_authored_order_and_invalid_active_payloads_fail},
    {"camera motion never splits the ordinary scene",
     &camera_motion_never_splits_the_ordinary_scene},
    {"identity effects produce the no-effect pass plan",
     &identity_effects_produce_the_no_effect_pass_plan},
    {"Levels remains inline and invalid parameters fail planning",
     &levels_remains_inline_and_invalid_parameters_fail_planning},
    {"Lift Gamma Gain remains inline and invalid payloads fail planning",
     &lift_gamma_gain_remains_inline_and_invalid_payloads_fail_planning},
    {"Contrast Pivot remains inline and invalid payloads fail planning",
     &contrast_pivot_remains_inline_and_invalid_payloads_fail_planning},
    {"Color Matrix remains inline and invalid payloads fail planning",
     &color_matrix_remains_inline_and_invalid_payloads_fail_planning},
    {"White Balance remains inline and invalid payloads fail planning",
     &white_balance_remains_inline_and_invalid_payloads_fail_planning},
    {"Curves remain inline and invalid payloads fail planning",
     &curves_remain_inline_and_invalid_payloads_fail_planning},
    {"Film Grain remains inline and invalid payloads fail planning",
     &film_grain_remains_inline_and_invalid_payloads_fail_planning},
    {"reverse-domain effect order is rejected explicitly",
     &reverse_domain_effect_order_is_rejected_explicitly},
    {"scene and graph effect order is validated as one stack",
     &scene_and_graph_effect_order_is_validated_as_one_stack},
    {"an isolated layer is primed with every other opaque drawable",
     &an_isolated_layer_is_primed_with_every_other_opaque_drawable},
    {"only crisply opaque drawables prime another layer's depth",
     &only_crisply_opaque_drawables_prime_another_layers_depth},
    {"one pass needs no depth priming", &one_pass_needs_no_depth_priming},
    {"drawable masks isolate once and identity is plan inert",
     &drawable_masks_isolate_once_and_identity_is_plan_inert},
    {"group and malformed mask programs fail closed",
     &group_and_malformed_mask_programs_fail_closed},
    {"a mask with nothing to cut is skipped rather than fatal",
     &a_mask_with_nothing_to_cut_is_skipped_rather_than_fatal},
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
