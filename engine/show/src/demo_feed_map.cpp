#include <mediaserver/show/demo_feed_map.hpp>

#include <mediaserver/output/feed_map_schema.hpp>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace mediaserver::show {
namespace {

namespace schema = mediaserver::output::schema;

[[nodiscard]] doc::Node map_node(
    core::u64 id,
    std::initializer_list<std::pair<std::string_view, doc::PropertyValue>>
        properties) {
    doc::Node node{};
    node.id = doc::NodeId{id};
    node.type = std::string{schema::map_type};
    for (const auto& [name, value] : properties) {
        node.properties.emplace(std::string{name}, value);
    }
    return node;
}

[[nodiscard]] doc::PropertyValue whole(std::int64_t value) {
    return doc::PropertyValue{value};
}

} // namespace

std::vector<doc::Node> demo_feed_map() {
    return {
        map_node(
            demo_feed_map_node_base + 0U,
            {{schema::node_index, whole(0)},
             {schema::source_x, whole(0)},
             {schema::source_y, whole(0)},
             {schema::source_width, whole(1'280)},
             {schema::source_height, whole(720)}}),
        map_node(
            demo_feed_map_node_base + 1U,
            {{schema::node_index, whole(1)},
             {schema::source_x, whole(1'280)},
             {schema::source_y, whole(0)},
             {schema::source_width, whole(1'280)},
             {schema::source_height, whole(720)}}),
    };
}

} // namespace mediaserver::show
