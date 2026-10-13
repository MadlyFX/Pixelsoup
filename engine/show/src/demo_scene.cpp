#include <mediaserver/show/demo_scene.hpp>

#include <mediaserver/scene/scene_schema.hpp>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace mediaserver::show {
namespace {

namespace schema = mediaserver::scene::schema;

[[nodiscard]] doc::Node scene_node(
    core::u64 id,
    std::string_view type,
    std::initializer_list<std::pair<std::string_view, doc::PropertyValue>>
        properties) {
    doc::Node node{};
    node.id = doc::NodeId{id};
    node.type = std::string{type};
    for (const auto& [name, value] : properties) {
        node.properties.emplace(std::string{name}, value);
    }
    return node;
}

[[nodiscard]] doc::PropertyValue real(double value) {
    return doc::PropertyValue{value};
}
[[nodiscard]] doc::PropertyValue whole(std::int64_t value) {
    return doc::PropertyValue{value};
}
[[nodiscard]] doc::PropertyValue text(std::string_view value) {
    return doc::PropertyValue{std::string{value}};
}

} // namespace

std::vector<doc::Node> demo_scene() {
    return {
        scene_node(
            demo_scene_node_base + 0U,
            schema::stage_type,
            {{schema::canvas_width, whole(2'560)},
             {schema::canvas_height, whole(720)},
             {schema::sky_r, real(0.04)},
             {schema::sky_g, real(0.05)},
             {schema::sky_b, real(0.08)},
             {schema::ambient, real(0.8)}}),
        scene_node(
            demo_scene_node_base + 1U,
            schema::camera_type,
            {{schema::position_x, real(0.0)},
             {schema::position_y, real(2.5)},
             {schema::position_z, real(7.0)},
             {schema::rotation_pitch, real(-0.0295)},
             {schema::fov_turns, real(0.1)},
             {schema::near_plane, real(0.05)},
             {schema::far_plane, real(400.0)},
             {schema::orbit_turns_per_second, real(0.02)}}),
        scene_node(
            demo_scene_node_base + 2U,
            schema::object_type,
            {{schema::primitive, text("plane")},
             {schema::segments, whole(4)},
             {schema::scale_x, real(40.0)},
             {schema::scale_z, real(40.0)},
             {schema::color_r, real(0.18)},
             {schema::color_g, real(0.19)},
             {schema::color_b, real(0.20)}}),
        scene_node(
            demo_scene_node_base + 3U,
            schema::object_type,
            {{schema::primitive, text("box")},
             {schema::position_x, real(-1.5)},
             {schema::position_y, real(0.5)},
             {schema::spin_turns_per_second, real(0.15)},
             {schema::color_r, real(0.90)},
             {schema::color_g, real(0.35)},
             {schema::color_b, real(0.15)}}),
        scene_node(
            demo_scene_node_base + 4U,
            schema::object_type,
            {{schema::primitive, text("sphere")},
             {schema::segments, whole(24)},
             {schema::position_x, real(1.5)},
             {schema::position_y, real(0.75)},
             {schema::bob_amplitude, real(0.4)},
             {schema::bob_turns_per_second, real(0.25)},
             {schema::roughness, real(0.25)}}),
        scene_node(
            demo_scene_node_base + 5U,
            schema::light_type,
            {{schema::light_kind, text("directional")},
             {schema::direction_x, real(-0.4)},
             {schema::direction_y, real(-1.0)},
             {schema::direction_z, real(-0.3)},
             {schema::intensity, real(3.0)}}),
        scene_node(
            demo_scene_node_base + 6U,
            schema::slice_type,
            {{schema::node_index, whole(0)},
             {schema::slice_x, whole(0)},
             {schema::slice_y, whole(0)},
             {schema::slice_width, whole(1'280)},
             {schema::slice_height, whole(720)}}),
        scene_node(
            demo_scene_node_base + 7U,
            schema::slice_type,
            {{schema::node_index, whole(1)},
             {schema::slice_x, whole(1'280)},
             {schema::slice_y, whole(0)},
             {schema::slice_width, whole(1'280)},
             {schema::slice_height, whole(720)}}),
    };
}

} // namespace mediaserver::show
