#pragma once

#include <cstdint>
#include <string>

#include <mediaserver/cmd/director_scheduler.hpp>

namespace mediaserver::test_support {

[[nodiscard]] inline doc::Node node(
    std::uint64_t id,
    std::int64_t value,
    std::string type = "test") {
    return doc::Node{
        doc::NodeId{id},
        std::move(type),
        {{"value", value}},
    };
}

[[nodiscard]] inline cmd::Payload erase(std::uint64_t id) {
    return cmd::EraseNode{doc::NodeId{id}};
}

[[nodiscard]] inline cmd::Payload put(std::uint64_t id, std::int64_t value) {
    return cmd::PutNode{node(id, value)};
}

} // namespace mediaserver::test_support
