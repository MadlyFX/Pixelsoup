#include <mediaserver/output/feed_map_model.hpp>

#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/output/feed_map_schema.hpp>
#include <mediaserver/scene/scene_schema.hpp>

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
#include <utility>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace output = mediaserver::output;
namespace scene = mediaserver::scene;
namespace schema = mediaserver::output::schema;
namespace scene_schema = mediaserver::scene::schema;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define OUTPUT_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

using Property = std::pair<std::string, doc::PropertyValue>;

[[nodiscard]] doc::Node node(
    core::u64 id,
    std::string_view type,
    std::initializer_list<Property> properties) {
    doc::Node built{};
    built.id = doc::NodeId{id};
    built.type = std::string{type};
    for (const auto& property : properties) {
        built.properties.emplace(property.first, property.second);
    }
    return built;
}

[[nodiscard]] Property text(std::string_view name, std::string_view value) {
    return Property{std::string{name}, doc::PropertyValue{std::string{value}}};
}

[[nodiscard]] Property whole(std::string_view name, std::int64_t value) {
    return Property{std::string{name}, doc::PropertyValue{value}};
}

[[nodiscard]] Property real(std::string_view name, double value) {
    return Property{std::string{name}, doc::PropertyValue{value}};
}

[[nodiscard]] doc::Node front_surface(core::u64 id = 50U) {
    return node(
        id, schema::surface_type,
        {text(schema::surface_name, "Main LED"),
         real(schema::center_x, 0.0), real(schema::center_y, 0.0),
         real(schema::center_z, -4.0), real(schema::right_x, 1.0),
         real(schema::right_y, 0.0), real(schema::right_z, 0.0),
         real(schema::up_x, 0.0), real(schema::up_y, 1.0),
         real(schema::up_z, 0.0), real(schema::physical_width, 8.0),
         real(schema::physical_height, 4.5),
         whole(schema::raster_width, 3'840),
         whole(schema::raster_height, 2'160)});
}

[[nodiscard]] doc::Node surface_channel(core::u64 id, core::u64 target,
                                        std::string_view component,
                                        double first, double last) {
    const std::string key0_frame =
        std::string{scene_schema::channel_key_prefix} + "0" +
        std::string{scene_schema::channel_key_frame_suffix};
    const std::string key0_value =
        std::string{scene_schema::channel_key_prefix} + "0" +
        std::string{scene_schema::channel_key_value_suffix};
    const std::string key1_frame =
        std::string{scene_schema::channel_key_prefix} + "1" +
        std::string{scene_schema::channel_key_frame_suffix};
    const std::string key1_value =
        std::string{scene_schema::channel_key_prefix} + "1" +
        std::string{scene_schema::channel_key_value_suffix};
    return node(id, scene_schema::channel_type,
                {whole(scene_schema::channel_target,
                       static_cast<std::int64_t>(target)),
                 text(scene_schema::channel_component, component),
                 whole(key0_frame, 0), real(key0_value, first),
                 whole(key1_frame, 10), real(key1_value, last)});
}

// A two-node LED wall across a 2560x720 canvas. Each node outputs a 1280-wide
// half; the two abut at column 1280 with no overlap.
[[nodiscard]] std::vector<doc::Node> demo_maps() {
    return {
        node(
            1U,
            schema::map_type,
            {whole(schema::node_index, 0),
             whole(schema::source_x, 0),
             whole(schema::source_y, 0),
             whole(schema::source_width, 1'280),
             whole(schema::source_height, 720)}),
        node(
            2U,
            schema::map_type,
            {whole(schema::node_index, 1),
             whole(schema::source_x, 1'280),
             whole(schema::source_y, 0),
             whole(schema::source_width, 1'280),
             whole(schema::source_height, 720)}),
    };
}

[[nodiscard]] doc::Root root_of(const std::vector<doc::Node>& nodes) {
    doc::Root root;
    for (const auto& one : nodes) {
        root = root.set(one);
    }
    return root;
}

void a_document_becomes_the_feed_map_it_describes() {
    output::FeedMapModel model;
    model.reset(root_of(demo_maps()));

    OUTPUT_CHECK(model.counters().rejected == 0U);
    OUTPUT_CHECK(model.counters().maps == 2U);
    OUTPUT_CHECK(model.counters().over_capacity() == 0U);

    const auto built = model.evaluate();
    OUTPUT_CHECK(built.maps.size() == 2U);

    const auto* const left = built.map_for(0U);
    const auto* const right = built.map_for(1U);
    OUTPUT_CHECK(left != nullptr && right != nullptr);
    OUTPUT_CHECK(left->source == (mediaserver::scene::CanvasRect{0U, 0U, 1'280U, 720U}));
    OUTPUT_CHECK(
        right->source == (mediaserver::scene::CanvasRect{1'280U, 0U, 1'280U, 720U}));
    // A node the document does not map outputs nothing rather than guessing.
    OUTPUT_CHECK(built.map_for(2U) == nullptr);
}

void physical_surface_channels_evaluate_at_the_show_frame() {
    std::vector<doc::Node> nodes{
        front_surface(),
        surface_channel(10U, 50U, scene_schema::position_x, 0.0, 10.0),
        surface_channel(11U, 50U, scene_schema::rotation_yaw, 0.0, 0.25),
    };
    output::FeedMapModel model;
    model.reset(root_of(nodes));

    const output::FeedMap start = model.evaluate();
    const output::ProjectionSurface* const start_surface =
        start.surface_for(doc::NodeId{50U});
    OUTPUT_CHECK(start_surface != nullptr);
    OUTPUT_CHECK(start_surface->geometry.center.x == 0.0F);
    OUTPUT_CHECK(start_surface->geometry.right ==
                 (scene::Vec3{1.0F, 0.0F, 0.0F}));

    const output::FeedMap middle =
        model.evaluate(scene::SceneTime{5U, 24U, 1U});
    const output::ProjectionSurface* const middle_surface =
        middle.surface_for(doc::NodeId{50U});
    OUTPUT_CHECK(middle_surface != nullptr);
    OUTPUT_CHECK(middle_surface->geometry.center.x == 5.0F);
    OUTPUT_CHECK(std::abs(middle_surface->geometry.right.x - 0.7071067F) <
                 1.0e-5F);
    OUTPUT_CHECK(std::abs(middle_surface->geometry.right.z + 0.7071067F) <
                 1.0e-5F);
    OUTPUT_CHECK(middle_surface->geometry.up ==
                 (scene::Vec3{0.0F, 1.0F, 0.0F}));
}

void physical_surfaces_are_locked_by_default_and_can_be_unlocked() {
    doc::Node surface = front_surface();
    output::FeedMapModel default_model;
    default_model.reset(doc::Root{}.set(surface));
    const output::FeedMap locked_map = default_model.evaluate();
    const output::ProjectionSurface* const locked =
        locked_map.surface_for(doc::NodeId{50U});
    OUTPUT_CHECK(locked != nullptr);
    OUTPUT_CHECK(locked->locked);

    surface.properties.insert_or_assign(
        std::string{schema::locked}, doc::PropertyValue{false});
    output::FeedMapModel unlocked_model;
    unlocked_model.reset(doc::Root{}.set(surface));
    const output::FeedMap unlocked_map = unlocked_model.evaluate();
    const output::ProjectionSurface* const unlocked =
        unlocked_map.surface_for(doc::NodeId{50U});
    OUTPUT_CHECK(unlocked != nullptr);
    OUTPUT_CHECK(!unlocked->locked);
}

void a_checkpoint_and_a_stream_of_deltas_agree_exactly() {
    // The replication claim: a node handed the whole document and a node that
    // folded in deltas must hold the same feed map, or their regions of the wall
    // meet at a seam.
    const auto nodes = demo_maps();

    output::FeedMapModel checkpointed;
    checkpointed.reset(root_of(nodes));

    output::FeedMapModel streamed;
    doc::Root running;
    for (const auto& one : nodes) {
        const auto next = running.set(one);
        streamed.apply(diff(running, next));
        running = next;
    }

    OUTPUT_CHECK(
        output::digest(checkpointed.evaluate())
        == output::digest(streamed.evaluate()));
}

void adjacent_maps_tile_the_canvas() {
    // The property the layer exists to guarantee on an LED wall: neighbouring
    // nodes' regions abut edge to edge with no gap and no overlap, so together
    // they tile the canvas exactly. There is no blend; the seam is pixel-exact.
    output::FeedMapModel model;
    model.reset(root_of(demo_maps()));
    const auto built = model.evaluate();
    const auto* const left = built.map_for(0U);
    const auto* const right = built.map_for(1U);
    OUTPUT_CHECK(left != nullptr && right != nullptr);

    // Same rows, and the right region begins exactly where the left one ends.
    OUTPUT_CHECK(left->source.y == right->source.y);
    OUTPUT_CHECK(left->source.height == right->source.height);
    OUTPUT_CHECK(left->source.x + left->source.width == right->source.x);
    // And together they cover the whole 2560-wide canvas.
    OUTPUT_CHECK(left->source.x == 0U);
    OUTPUT_CHECK(right->source.x + right->source.width == 2'560U);
}

void a_property_of_the_wrong_type_rejects_its_node() {
    // source_width as a string rather than a number. Defaulting it would leave
    // a node outputting a zero-width strip with no indication why.
    output::FeedMapModel model;
    model.reset(doc::Root{}.set(node(
        1U,
        schema::map_type,
        {whole(schema::source_height, 720),
         text(schema::source_width, "wide")})));
    OUTPUT_CHECK(model.counters().rejected == 1U);
    OUTPUT_CHECK(model.evaluate().maps.empty());

    // A zero-size source is refused: a node cannot output an empty rectangle.
    output::FeedMapModel empty;
    empty.reset(doc::Root{}.set(node(
        1U,
        schema::map_type,
        {whole(schema::source_width, 0), whole(schema::source_height, 720)})));
    OUTPUT_CHECK(empty.counters().rejected == 1U);

    // An absent property, by contrast, takes its default, so a map authored
    // before a property existed still loads.
    output::FeedMapModel sparse;
    sparse.reset(doc::Root{}.set(node(
        1U,
        schema::map_type,
        {whole(schema::source_width, 1'280), whole(schema::source_height, 720)})));
    OUTPUT_CHECK(sparse.counters().rejected == 0U);
    OUTPUT_CHECK(sparse.evaluate().maps.size() == 1U);
}

void retyping_or_erasing_a_node_removes_its_binding() {
    output::FeedMapModel model;
    doc::Root root = doc::Root{}.set(node(
        1U,
        schema::map_type,
        {whole(schema::source_width, 1'280), whole(schema::source_height, 720)}));
    model.reset(root);
    OUTPUT_CHECK(model.evaluate().maps.size() == 1U);

    const auto retyped = root.set(node(1U, "scene.object", {}));
    model.apply(diff(root, retyped));
    OUTPUT_CHECK(model.evaluate().maps.empty());
    OUTPUT_CHECK(model.counters().maps == 0U);
    OUTPUT_CHECK(model.size() == 0U);

    output::FeedMapModel erased_model;
    erased_model.reset(root);
    const auto erased = root.erase(doc::NodeId{1U});
    OUTPUT_CHECK(erased.has_value());
    erased_model.apply(diff(root, *erased));
    OUTPUT_CHECK(erased_model.evaluate().maps.empty());
    OUTPUT_CHECK(erased_model.counters().maps == 0U);
}

void the_lowest_node_id_wins_a_duplicated_index() {
    output::FeedMapModel model;
    doc::Root root;
    root = root.set(node(
        9U,
        schema::map_type,
        {whole(schema::node_index, 0),
         whole(schema::source_width, 800),
         whole(schema::source_height, 600)}));
    root = root.set(node(
        4U,
        schema::map_type,
        {whole(schema::node_index, 0),
         whole(schema::source_width, 1'024),
         whole(schema::source_height, 768)}));
    model.reset(root);

    const auto built = model.evaluate();
    // Two nodes claim index 0; the document decides which, not the order the
    // nodes were written in.
    OUTPUT_CHECK(built.maps.size() == 1U);
    const auto* const bound = built.map_for(0U);
    OUTPUT_CHECK(bound != nullptr);
    OUTPUT_CHECK(bound->source.width == 1'024U);
}

void the_digest_separates_maps_a_single_bit_apart() {
    output::FeedMapModel base;
    base.reset(root_of(demo_maps()));
    const auto reference = output::digest(base.evaluate());

    // The smallest change to a region a map can hold: one column wider. A digest
    // that missed it would miss the divergence that matters most - the one that
    // opens a one-pixel seam nobody notices until it is on a wall.
    auto nudged = demo_maps();
    nudged[0U].properties.insert_or_assign(
        std::string{schema::source_width}, doc::PropertyValue{std::int64_t{1'281}});

    output::FeedMapModel moved;
    moved.reset(root_of(nudged));
    OUTPUT_CHECK(output::digest(moved.evaluate()) != reference);

    // A feed map that is a prefix of another does not collide with it.
    auto shorter = demo_maps();
    shorter.pop_back();
    output::FeedMapModel truncated;
    truncated.reset(root_of(shorter));
    OUTPUT_CHECK(output::digest(truncated.evaluate()) != reference);
}

void a_document_past_capacity_truncates_the_same_way_everywhere() {
    output::FeedMapModel model;
    doc::Root root;
    const auto count = output::max_output_maps + 16U;
    for (core::u32 index = 0U; index < count; ++index) {
        root = root.set(node(
            static_cast<core::u64>(index) + 1U,
            schema::map_type,
            {whole(schema::node_index, static_cast<std::int64_t>(index)),
             whole(schema::source_width, 1'280),
             whole(schema::source_height, 720)}));
    }
    model.reset(root);
    OUTPUT_CHECK(model.counters().maps == count);
    OUTPUT_CHECK(model.counters().over_capacity() == 16U);

    const auto built = model.evaluate();
    OUTPUT_CHECK(built.maps.size() == output::max_output_maps);

    // Truncation is by node id, so a streamed document and a checkpointed one
    // drop the same maps.
    output::FeedMapModel streamed;
    doc::Root running;
    for (core::u32 index = 0U; index < count; ++index) {
        const auto next = root.get(doc::NodeId{static_cast<core::u64>(index) + 1U});
        OUTPUT_CHECK(next != nullptr);
        const auto grown = running.set(*next);
        streamed.apply(diff(running, grown));
        running = grown;
    }
    OUTPUT_CHECK(
        output::digest(streamed.evaluate()) == output::digest(built));
}

void a_physical_surface_is_shared_by_partitioned_output_feeds() {
    doc::Root root = doc::Root{}.set(front_surface());
    root = root.set(node(
        60U, schema::map_type,
        {whole(schema::node_index, 0), whole(schema::source_x, 0),
         whole(schema::source_y, 0), whole(schema::source_width, 1'920),
         whole(schema::source_height, 2'160),
         text(schema::projection, schema::projection_planar_surface),
         whole(schema::surface_id, 50)}));
    root = root.set(node(
        61U, schema::map_type,
        {whole(schema::node_index, 1), whole(schema::source_x, 1'920),
         whole(schema::source_y, 0), whole(schema::source_width, 1'920),
         whole(schema::source_height, 2'160),
         text(schema::projection, schema::projection_planar_surface),
         whole(schema::surface_id, 50)}));

    output::FeedMapModel model;
    model.reset(root);
    OUTPUT_CHECK(model.counters().rejected == 0U);
    OUTPUT_CHECK(model.counters().surfaces == 1U);
    OUTPUT_CHECK(model.surface_size() == 1U);

    const output::FeedMap built = model.evaluate();
    OUTPUT_CHECK(built.surfaces.size() == 1U);
    OUTPUT_CHECK(built.surface_for(doc::NodeId{50U}) != nullptr);
    const auto* const left = built.map_for(0U);
    const auto* const right = built.map_for(1U);
    OUTPUT_CHECK(left != nullptr && right != nullptr);
    OUTPUT_CHECK(left->projection == output::FeedProjection::PlanarSurface);
    OUTPUT_CHECK(left->surface == doc::NodeId{50U});
    OUTPUT_CHECK(left->perspective_ready());
    OUTPUT_CHECK(right->perspective_ready());
    OUTPUT_CHECK(left->resolved_surface == right->resolved_surface);

    const auto left_view = output::perspective_view_for(
        left->resolved_surface->geometry, left->source,
        mediaserver::scene::Vec3{0.0F, 0.0F, 0.0F}, 0.1F, 100.0F);
    const auto right_view = output::perspective_view_for(
        right->resolved_surface->geometry, right->source,
        mediaserver::scene::Vec3{0.0F, 0.0F, 0.0F}, 0.1F, 100.0F);
    OUTPUT_CHECK(left_view.has_value() && right_view.has_value());
    OUTPUT_CHECK(left_view.value().frustum.right ==
                 right_view.value().frustum.left);
}

void dangling_or_outside_surface_assignments_remain_diagnosable() {
    doc::Root root = doc::Root{}.set(front_surface());
    root = root.set(node(
        70U, schema::map_type,
        {whole(schema::node_index, 0), whole(schema::source_x, 3'800),
         whole(schema::source_y, 0), whole(schema::source_width, 100),
         whole(schema::source_height, 100),
         text(schema::projection, schema::projection_planar_surface),
         whole(schema::surface_id, 50)}));
    root = root.set(node(
        71U, schema::map_type,
        {whole(schema::node_index, 1), whole(schema::source_width, 100),
         whole(schema::source_height, 100),
         text(schema::projection, schema::projection_planar_surface),
         whole(schema::surface_id, 999)}));

    output::FeedMapModel model;
    model.reset(root);
    const output::FeedMap built = model.evaluate();
    OUTPUT_CHECK(built.maps.size() == 2U);
    OUTPUT_CHECK(!built.map_for(0U)->perspective_ready());
    OUTPUT_CHECK(!built.map_for(1U)->perspective_ready());
    OUTPUT_CHECK(built.map_for(0U)->surface == doc::NodeId{50U});
    OUTPUT_CHECK(built.map_for(1U)->surface == doc::NodeId{999U});

    // A bad policy string is a schema error, unlike a dangling reference,
    // which remains an editable assignment.
    output::FeedMapModel invalid;
    invalid.reset(doc::Root{}.set(node(
        72U, schema::map_type,
        {whole(schema::source_width, 100),
         whole(schema::source_height, 100),
         text(schema::projection, "mystery")})));
    OUTPUT_CHECK(invalid.counters().rejected == 1U);
    OUTPUT_CHECK(invalid.evaluate().maps.empty());
}

void projection_surface_capacity_never_resolves_a_hidden_surface() {
    doc::Root root;
    for (core::u32 index = 0U;
         index < output::max_projection_surfaces + 1U; ++index) {
        root = root.set(front_surface(static_cast<core::u64>(index) + 1U));
    }
    const doc::NodeId hidden{output::max_projection_surfaces + 1U};
    root = root.set(node(
        10'000U, schema::map_type,
        {whole(schema::node_index, 0), whole(schema::source_width, 100),
         whole(schema::source_height, 100),
         text(schema::projection, schema::projection_planar_surface),
         whole(schema::surface_id,
               static_cast<std::int64_t>(hidden.value()))}));

    output::FeedMapModel model;
    model.reset(root);
    OUTPUT_CHECK(model.counters().surfaces_over_capacity() == 1U);
    const output::FeedMap built = model.evaluate();
    OUTPUT_CHECK(built.surfaces.size() == output::max_projection_surfaces);
    OUTPUT_CHECK(built.surface_for(hidden) == nullptr);
    OUTPUT_CHECK(built.map_for(0U) != nullptr);
    OUTPUT_CHECK(!built.map_for(0U)->perspective_ready());
}

void a_horizontal_arc_surface_round_trips_as_a_mesh_projection() {
    doc::Node surface = front_surface(80U);
    surface.properties.insert_or_assign(
        std::string{schema::surface_shape},
        doc::PropertyValue{std::string{schema::shape_horizontal_arc}});
    surface.properties.insert_or_assign(
        std::string{schema::horizontal_arc_turns},
        doc::PropertyValue{0.25});
    surface.properties.insert_or_assign(
        std::string{schema::mesh_columns},
        doc::PropertyValue{std::int64_t{48}});
    doc::Root root = doc::Root{}.set(surface);
    root = root.set(node(
        81U, schema::map_type,
        {whole(schema::node_index, 0), whole(schema::source_x, 0),
         whole(schema::source_y, 0), whole(schema::source_width, 1'920),
         whole(schema::source_height, 2'160),
         text(schema::projection, schema::projection_planar_surface),
         whole(schema::surface_id, 80)}));

    output::FeedMapModel model;
    model.reset(root);
    const output::FeedMap built = model.evaluate();
    const output::ProjectionSurface* const calibrated =
        built.surface_for(doc::NodeId{80U});
    const output::OutputMap* const map = built.map_for(0U);
    OUTPUT_CHECK(calibrated != nullptr && map != nullptr);
    OUTPUT_CHECK(
        calibrated->shape == output::ProjectionSurfaceShape::HorizontalArc);
    OUTPUT_CHECK(calibrated->horizontal_arc_turns == 0.25F);
    OUTPUT_CHECK(calibrated->mesh_columns == 48U);
    OUTPUT_CHECK(map->perspective_ready());
    OUTPUT_CHECK(
        map->resolved_surface->shape ==
        output::ProjectionSurfaceShape::HorizontalArc);
    const auto mesh = output::horizontal_arc_mesh(
        map->resolved_surface->geometry,
        map->resolved_surface->horizontal_arc_turns,
        map->resolved_surface->mesh_columns, map->source);
    OUTPUT_CHECK(mesh.has_value());
    OUTPUT_CHECK(mesh.value().vertices.size() > 4U);

    // A zero bend cannot silently become a flat-but-mesh policy. The exact
    // planar policy already exists and has stronger seam guarantees.
    surface.properties.insert_or_assign(
        std::string{schema::horizontal_arc_turns},
        doc::PropertyValue{0.0});
    output::FeedMapModel invalid;
    invalid.reset(doc::Root{}.set(surface));
    OUTPUT_CHECK(invalid.counters().rejected == 1U);
    OUTPUT_CHECK(invalid.evaluate().surfaces.empty());
}

void an_imported_mesh_surface_round_trips_and_resolves() {
    doc::Node surface = front_surface(90U);
    surface.properties.insert_or_assign(
        std::string{schema::surface_shape},
        doc::PropertyValue{std::string{schema::shape_imported_mesh}});
    surface.properties.insert_or_assign(
        std::string{schema::mesh_source},
        doc::PropertyValue{std::string{"calibration/main-led.obj"}});
    surface.properties.insert_or_assign(
        std::string{schema::mesh_scale}, doc::PropertyValue{0.001});
    surface.properties.insert_or_assign(
        std::string{schema::mesh_asset_id},
        doc::PropertyValue{std::int64_t{123}});
    doc::Root root = doc::Root{}.set(surface);
    root = root.set(node(
        91U, schema::map_type,
        {whole(schema::node_index, 0), whole(schema::source_x, 0),
         whole(schema::source_y, 0), whole(schema::source_width, 1'920),
         whole(schema::source_height, 2'160),
         text(schema::projection, schema::projection_planar_surface),
         whole(schema::surface_id, 90)}));

    output::FeedMapModel model;
    model.reset(root);
    OUTPUT_CHECK(model.counters().rejected == 0U);
    const output::FeedMap built = model.evaluate();
    const output::ProjectionSurface* const calibrated =
        built.surface_for(doc::NodeId{90U});
    const output::OutputMap* const map = built.map_for(0U);
    OUTPUT_CHECK(calibrated != nullptr && map != nullptr);
    OUTPUT_CHECK(
        calibrated->shape == output::ProjectionSurfaceShape::ImportedMesh);
    OUTPUT_CHECK(calibrated->mesh_source == "calibration/main-led.obj");
    OUTPUT_CHECK(calibrated->mesh_scale == 0.001F);
    OUTPUT_CHECK(calibrated->mesh_asset == doc::NodeId{123U});
    OUTPUT_CHECK(map->perspective_ready());
    OUTPUT_CHECK(
        map->resolved_surface->shape ==
        output::ProjectionSurfaceShape::ImportedMesh);
    OUTPUT_CHECK(
        map->resolved_surface->mesh_source == "calibration/main-led.obj");
    OUTPUT_CHECK(map->resolved_surface->mesh_scale == 0.001F);
    OUTPUT_CHECK(map->resolved_surface->mesh_asset == doc::NodeId{123U});
    OUTPUT_CHECK(map->source ==
                 scene::CanvasRect{0U, 0U, 3'840U, 2'160U});

    // Both imported-mesh identity fields participate in replicated equality.
    surface.properties.insert_or_assign(
        std::string{schema::mesh_source},
        doc::PropertyValue{std::string{"calibration/alternate.obj"}});
    output::FeedMapModel changed_source;
    changed_source.reset(root.set(surface));
    OUTPUT_CHECK(output::digest(built) !=
                 output::digest(changed_source.evaluate()));
    surface.properties.insert_or_assign(
        std::string{schema::mesh_scale}, doc::PropertyValue{0.002});
    output::FeedMapModel changed_scale;
    changed_scale.reset(root.set(surface));
    OUTPUT_CHECK(output::digest(built) !=
                 output::digest(changed_scale.evaluate()));
}

void an_imported_mesh_requires_a_source() {
    doc::Node surface = front_surface(92U);
    surface.properties.insert_or_assign(
        std::string{schema::surface_shape},
        doc::PropertyValue{std::string{schema::shape_imported_mesh}});
    surface.properties.insert_or_assign(
        std::string{schema::mesh_scale}, doc::PropertyValue{1.0});

    output::FeedMapModel model;
    model.reset(doc::Root{}.set(surface));
    OUTPUT_CHECK(model.counters().rejected == 1U);
    OUTPUT_CHECK(model.evaluate().surfaces.empty());
}

void invalid_imported_mesh_scales_and_nonfinite_dormant_scales_are_rejected() {
    const auto rejected_imported_scale = [](double scale) {
        doc::Node surface = front_surface(93U);
        surface.properties.insert_or_assign(
            std::string{schema::surface_shape},
            doc::PropertyValue{std::string{schema::shape_imported_mesh}});
        surface.properties.insert_or_assign(
            std::string{schema::mesh_source},
            doc::PropertyValue{std::string{"calibration/wall.obj"}});
        surface.properties.insert_or_assign(
            std::string{schema::mesh_scale}, doc::PropertyValue{scale});
        output::FeedMapModel model;
        model.reset(doc::Root{}.set(surface));
        return model.counters().rejected == 1U &&
            model.evaluate().surfaces.empty();
    };
    OUTPUT_CHECK(rejected_imported_scale(0.0));
    OUTPUT_CHECK(rejected_imported_scale(-1.0));
    OUTPUT_CHECK(rejected_imported_scale(
        static_cast<double>(output::max_imported_mesh_scale) + 1.0));
    OUTPUT_CHECK(rejected_imported_scale(
        std::numeric_limits<double>::infinity()));

    // Dormant values are parsed too. This guarantees every node accepted by
    // authoring remains readable if its shape is changed later.
    doc::Node planar = front_surface(94U);
    planar.properties.insert_or_assign(
        std::string{schema::mesh_scale},
        doc::PropertyValue{std::numeric_limits<double>::infinity()});
    output::FeedMapModel invalid_planar;
    invalid_planar.reset(doc::Root{}.set(planar));
    OUTPUT_CHECK(invalid_planar.counters().rejected == 1U);

    doc::Node arc = front_surface(95U);
    arc.properties.insert_or_assign(
        std::string{schema::surface_shape},
        doc::PropertyValue{std::string{schema::shape_horizontal_arc}});
    arc.properties.insert_or_assign(
        std::string{schema::mesh_scale},
        doc::PropertyValue{std::numeric_limits<double>::infinity()});
    output::FeedMapModel invalid_arc;
    invalid_arc.reset(doc::Root{}.set(arc));
    OUTPUT_CHECK(invalid_arc.counters().rejected == 1U);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 17U> cases{{
    {"a document becomes the feed map it describes",
     &a_document_becomes_the_feed_map_it_describes},
    {"physical surface channels evaluate at the show frame",
     &physical_surface_channels_evaluate_at_the_show_frame},
    {"physical surfaces are locked by default and can be unlocked",
     &physical_surfaces_are_locked_by_default_and_can_be_unlocked},
    {"a checkpoint and a stream of deltas agree exactly",
     &a_checkpoint_and_a_stream_of_deltas_agree_exactly},
    {"adjacent maps tile the canvas",
     &adjacent_maps_tile_the_canvas},
    {"a property of the wrong type rejects its node",
     &a_property_of_the_wrong_type_rejects_its_node},
    {"retyping or erasing a node removes its binding",
     &retyping_or_erasing_a_node_removes_its_binding},
    {"the lowest node id wins a duplicated index",
     &the_lowest_node_id_wins_a_duplicated_index},
    {"the digest separates maps a single bit apart",
     &the_digest_separates_maps_a_single_bit_apart},
    {"a document past capacity truncates the same way everywhere",
     &a_document_past_capacity_truncates_the_same_way_everywhere},
    {"a physical surface is shared by partitioned output feeds",
     &a_physical_surface_is_shared_by_partitioned_output_feeds},
    {"dangling or outside surface assignments remain diagnosable",
     &dangling_or_outside_surface_assignments_remain_diagnosable},
    {"projection surface capacity never resolves a hidden surface",
     &projection_surface_capacity_never_resolves_a_hidden_surface},
    {"a horizontal arc surface round trips as a mesh projection",
     &a_horizontal_arc_surface_round_trips_as_a_mesh_projection},
    {"an imported mesh surface round trips and resolves",
     &an_imported_mesh_surface_round_trips_and_resolves},
    {"an imported mesh requires a source",
     &an_imported_mesh_requires_a_source},
    {"invalid imported mesh scales and nonfinite dormant scales are rejected",
     &invalid_imported_mesh_scales_and_nonfinite_dormant_scales_are_rejected},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const auto& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
