#include <mediaserver/show/transport_state.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <variant>

namespace mediaserver::show {
namespace {

[[nodiscard]] const std::int64_t* find_integer(
    const doc::PropertyMap& properties,
    std::string_view key) noexcept {
    const auto entry = properties.find(key);
    if (entry == properties.end()) {
        return nullptr;
    }
    return std::get_if<std::int64_t>(&entry->second);
}

[[nodiscard]] bool read_u64(
    const doc::PropertyMap& properties,
    std::string_view key,
    core::u64& parsed) noexcept {
    const auto* const value = find_integer(properties, key);
    if (value == nullptr || *value < 0) {
        return false;
    }
    parsed = static_cast<core::u64>(*value);
    return true;
}

[[nodiscard]] bool read_u32(
    const doc::PropertyMap& properties,
    std::string_view key,
    core::u32& parsed) noexcept {
    core::u64 wide{};
    if (!read_u64(properties, key, wide)
        || wide > std::numeric_limits<core::u32>::max()) {
        return false;
    }
    parsed = static_cast<core::u32>(wide);
    return true;
}

[[nodiscard]] bool read_transport_state(
    const doc::PropertyMap& properties,
    TransportState& parsed) noexcept {
    core::u64 wide{};
    if (!read_u64(properties, transport_property::transport_state, wide)) {
        return false;
    }
    switch (wide) {
    case 0U:
        parsed = TransportState::Stopped;
        return true;
    case 1U:
        parsed = TransportState::Paused;
        return true;
    case 2U:
        parsed = TransportState::Playing;
        return true;
    default:
        return false;
    }
}

} // namespace

doc::Node make_transport_node(const TransportSnapshot& snapshot) {
    doc::Node node{};
    node.id = transport_node_id;
    node.type = std::string{transport_node_type};
    node.properties.emplace(
        std::string{transport_property::show_frame},
        static_cast<std::int64_t>(snapshot.show_frame.value()));
    node.properties.emplace(
        std::string{transport_property::transport_state},
        static_cast<std::int64_t>(snapshot.state));
    node.properties.emplace(
        std::string{transport_property::rate_numerator},
        static_cast<std::int64_t>(snapshot.rate.numerator));
    node.properties.emplace(
        std::string{transport_property::rate_denominator},
        static_cast<std::int64_t>(snapshot.rate.denominator));
    return node;
}

std::optional<TransportSnapshot> read_transport(const doc::Root& root) noexcept {
    const auto* const node = root.get(transport_node_id);
    if (node == nullptr
        || std::string_view{node->type} != transport_node_type) {
        return std::nullopt;
    }

    TransportSnapshot snapshot{};
    core::u64 frame{};
    if (!read_u64(node->properties, transport_property::show_frame, frame)
        || !read_transport_state(node->properties, snapshot.state)
        || !read_u32(
            node->properties,
            transport_property::rate_numerator,
            snapshot.rate.numerator)
        || !read_u32(
            node->properties,
            transport_property::rate_denominator,
            snapshot.rate.denominator)) {
        return std::nullopt;
    }

    snapshot.show_frame = ShowFrame{frame};
    if (snapshot.rate.numerator == 0U || snapshot.rate.denominator == 0U) {
        return std::nullopt;
    }
    return snapshot;
}

} // namespace mediaserver::show
