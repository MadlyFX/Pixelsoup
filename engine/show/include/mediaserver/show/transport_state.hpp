#pragma once

#include <optional>
#include <string_view>

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/show/show_clock.hpp>

namespace mediaserver::show {

// The show position is replicated document state, not a local clock reading,
// and it is carried as an *anchor* rather than as a per-frame position.
//
// The control-frame stream is already an exactly replicated, totally ordered
// timebase, so a node derives what it presents by integer arithmetic on the
// control frame it committed:
//
//   presented = anchor_show_frame + (control_frame - anchor_control_frame)
//
// with the term suppressed while the transport is not playing, negated while
// it runs in reverse, and wrapped or clamped by the show range. Two nodes on
// the same control frame therefore compute the same show frame with no clock
// involved, and a playing show authors no document edits at all: the anchor
// changes only when an operator acts or the show reaches its end.
//
// The identifier is a reserved well-known value for the playback slice. A
// document-wide reservation registry replaces it once more than one system
// owns well-known nodes.
inline constexpr doc::NodeId transport_node_id{1U};
inline constexpr std::string_view transport_node_type = "show.transport";

namespace transport_property {

inline constexpr std::string_view anchor_control_frame = "anchor_control_frame";
inline constexpr std::string_view anchor_show_frame = "anchor_show_frame";
inline constexpr std::string_view transport_state = "transport_state";
inline constexpr std::string_view direction = "direction";
inline constexpr std::string_view rate_numerator = "rate_numerator";
inline constexpr std::string_view rate_denominator = "rate_denominator";
inline constexpr std::string_view first_frame = "first_frame";
inline constexpr std::string_view last_frame = "last_frame";
inline constexpr std::string_view loop = "loop";

} // namespace transport_property

enum class TransportDirection : core::u8 {
    Forward,
    Reverse,
};

// The replicated transport anchor.
//
// The show range and loop flag travel with the anchor deliberately. Carrying
// them is what lets derivation wrap a looping show and hold a finished one
// without a fresh anchor at every wrap, which keeps re-anchoring an
// operator-action-only event.
struct TransportAnchor final {
    cmd::FrameNumber anchor_control_frame{};
    ShowFrame anchor_show_frame{};
    TransportState state{TransportState::Stopped};
    TransportDirection direction{TransportDirection::Forward};
    FrameRate rate{};
    ShowFrame first_frame{};
    ShowFrame last_frame{}; // inclusive
    bool loop{};

    bool operator==(const TransportAnchor&) const = default;
};

// True when the anchor is self-consistent: a usable rate, a non-degenerate
// inclusive range, and an anchor position inside it.
[[nodiscard]] bool valid_transport_anchor(const TransportAnchor& anchor) noexcept;

// The derivation every node performs. Exact integer arithmetic on replicated
// state: no clock is read, nothing is allocated, and no branch depends on
// local timing, so two nodes given the same anchor and control frame return
// the same frame.
//
// A control frame at or before the anchor reads as the anchor position, which
// is also what the frame carrying a fresh anchor presents.
[[nodiscard]] ShowFrame derive_show_frame(
    const TransportAnchor& anchor,
    cmd::FrameNumber control_frame) noexcept;

// True once a forward, non-looping show has derived its last frame. The
// Director uses this to re-anchor into the paused end-of-show hold that
// ShowClock performs locally.
[[nodiscard]] bool reached_end(
    const TransportAnchor& anchor,
    cmd::FrameNumber control_frame) noexcept;

// Authoring side. Allocates, and is therefore a Director operation performed
// while scheduling a command, never on a frame boundary.
[[nodiscard]] doc::Node make_transport_node(const TransportAnchor& anchor);

// Playback side. Performs one trie lookup and heterogeneous map lookups, so it
// allocates nothing and is legal on a frame boundary. A missing, mistyped, or
// out-of-domain node reads as absent rather than as a default position.
[[nodiscard]] std::optional<TransportAnchor> read_transport(
    const doc::Root& root) noexcept;

} // namespace mediaserver::show
