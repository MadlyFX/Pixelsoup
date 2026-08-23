#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>

#include <mediaserver/doc/ids.hpp>

namespace mediaserver::doc {

using PropertyValue = std::variant<bool, std::int64_t, double, std::string>;
using PropertyMap = std::map<std::string, PropertyValue, std::less<>>;

struct Node final {
    NodeId id;
    std::string type;
    PropertyMap properties;

    bool operator==(const Node&) const = default;
};

using NodeRef = std::shared_ptr<const Node>;

} // namespace mediaserver::doc

