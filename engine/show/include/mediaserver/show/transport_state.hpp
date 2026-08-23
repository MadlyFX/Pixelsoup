#pragma once

#include <optional>
#include <string_view>

#include <mediaserver/doc/document.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/show/show_clock.hpp>

namespace mediaserver::show {

// The show position is replicated document state, not a local clock reading.
// Every node derives the frame it presents from this node in the committed
// root, which is what makes two independently paced nodes present the same
// show frame rather than merely the same control revision.
//
// The identifier is a reserved well-known value for the playback slice. A
// document-wide reservation registry replaces it once more than one system
// owns well-known nodes.
inline constexpr doc::NodeId transport_node_id{1U};
inline constexpr std::string_view transport_node_type = "show.transport";

namespace transport_property {

inline constexpr std::string_view show_frame = "show_frame";
inline constexpr std::string_view transport_state = "transport_state";
inline constexpr std::string_view rate_numerator = "rate_numerator";
inline constexpr std::string_view rate_denominator = "rate_denominator";

} // namespace transport_property

struct TransportSnapshot final {
    ShowFrame show_frame{};
    TransportState state{TransportState::Stopped};
    FrameRate rate{};

    bool operator==(const TransportSnapshot&) const = default;
};

// Authoring side. Allocates, and is therefore a Director operation performed
// while scheduling a command, never on a frame boundary.
[[nodiscard]] doc::Node make_transport_node(const TransportSnapshot& snapshot);

// Playback side. Performs one trie lookup and heterogeneous map lookups, so it
// allocates nothing and is legal on a frame boundary. A missing, mistyped, or
// out-of-domain node reads as absent rather than as a default position.
[[nodiscard]] std::optional<TransportSnapshot> read_transport(
    const doc::Root& root) noexcept;

} // namespace mediaserver::show
