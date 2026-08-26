#include <mediaserver/show/demo_edit_scene.hpp>

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

std::vector<doc::Node> demo_edit_scene(std::string_view clip_source) {
    return {
        scene_node(
            demo_edit_node_base + 0U,
            schema::stage_type,
            {{schema::scene_name, text("Opening")},
             {schema::canvas_width, whole(demo_edit_canvas_width)},
             {schema::canvas_height, whole(demo_edit_canvas_height)},
             {schema::sky_r, real(0.05)},
             {schema::sky_g, real(0.06)},
             {schema::sky_b, real(0.09)},
             {schema::ambient, real(0.9)}}),
        scene_node(
            demo_edit_node_base + 1U,
            schema::camera_type,
            {{schema::eye_x, real(0.0)},
             {schema::eye_y, real(0.5)},
             {schema::eye_z, real(6.0)},
             {schema::target_y, real(0.0)},
             {schema::fov_turns, real(0.12)},
             {schema::near_plane, real(0.05)},
             {schema::far_plane, real(200.0)}}),
        scene_node(
            demo_edit_node_base + 2U,
            schema::object_type,
            {{schema::primitive, text("plane")},
             {schema::segments, whole(4)},
             {schema::position_y, real(-2.5)},
             {schema::scale_x, real(40.0)},
             {schema::scale_z, real(40.0)},
             {schema::color_r, real(0.16)},
             {schema::color_g, real(0.17)},
             {schema::color_b, real(0.19)}}),
        scene_node(
            demo_edit_node_base + 3U,
            schema::light_type,
            {{schema::light_kind, text("directional")},
             {schema::direction_x, real(-0.3)},
             {schema::direction_y, real(-1.0)},
             {schema::direction_z, real(-0.4)},
             {schema::intensity, real(2.5)}}),
        scene_node(
            demo_edit_node_base + 4U,
            schema::media_type,
            {{schema::source, text(clip_source)},
             {schema::sequence_start, whole(0)},
             {schema::sequence_count,
              whole(static_cast<std::int64_t>(demo_clip_frames))},
             {schema::position_x, real(0.0)},
             {schema::position_y, real(0.0)},
             {schema::position_z, real(0.0)},
             // A 16:9 clip, a couple of metres across.
             {schema::scale_x, real(3.556)},
             {schema::scale_y, real(2.0)},
             {schema::scale_z, real(1.0)}}),
        // Two more clips of the same source at different positions and depths, so
        // multi-select and a rigid group move (every clip by one world delta,
        // whatever its depth) are visible. Same source, so the cache holds one
        // texture and all three draw it.
        scene_node(
            demo_edit_node_base + 5U,
            schema::media_type,
            {{schema::source, text(clip_source)},
             {schema::sequence_start, whole(0)},
             {schema::sequence_count,
              whole(static_cast<std::int64_t>(demo_clip_frames))},
             {schema::position_x, real(-4.0)},
             {schema::position_y, real(1.2)},
             {schema::position_z, real(-2.0)},
             {schema::scale_x, real(2.133)},
             {schema::scale_y, real(1.2)},
             {schema::scale_z, real(1.0)}}),
        scene_node(
            demo_edit_node_base + 6U,
            schema::media_type,
            {{schema::source, text(clip_source)},
             {schema::sequence_start, whole(0)},
             {schema::sequence_count,
              whole(static_cast<std::int64_t>(demo_clip_frames))},
             {schema::position_x, real(3.8)},
             {schema::position_y, real(-1.4)},
             {schema::position_z, real(1.5)},
             {schema::scale_x, real(2.133)},
             {schema::scale_y, real(1.2)},
             {schema::scale_z, real(1.0)}}),
    };
}

} // namespace mediaserver::show
