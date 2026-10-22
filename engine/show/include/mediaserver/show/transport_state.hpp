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
// Optional. Absent reads as armed, which is what a project authored before
// cues existed would have meant had it carried any.
inline constexpr std::string_view cues_armed = "cues_armed";
// Optional. Absent reads as an operator anchor.
inline constexpr std::string_view scheduled = "scheduled";
// Optional, together. Absent reads as a cut.
inline constexpr std::string_view blend_frames = "blend_frames";
inline constexpr std::string_view blend_from = "blend_from";
inline constexpr std::string_view blend_moves = "blend_moves";

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
    // Whether the Director stops and loops on authored cues. Derivation never
    // reads it: a cue acts by re-anchoring, exactly like the end-of-show hold,
    // so a node only ever follows anchors. It rides here because it is
    // show-wide transport behaviour that has to survive a save, like `loop`.
    bool cues_armed{true};
    // Set when the show re-anchored itself - it reached a cue or its end -
    // rather than an operator interrupting it. Every frame an output had
    // already buffered from the previous anchor is still the show, so outputs
    // play those through before this anchor takes effect instead of flushing
    // them: a Hold shows every frame up to the cue, and a Loop wraps without
    // dropping or freezing frames. Operator anchors keep the immediate
    // restart, because a scrub or a seek must answer the hand that made it.
    bool scheduled{};
    // A crossfade the anchor opens with, which only a Loop cue authors. For
    // `blend_frames` control frames from the anchor, every node mixes the
    // picture the show is leaving - show frame `blend_from` at the anchor,
    // advancing one frame per control frame when `blend_moves` - into the
    // one this anchor derives. Zero is a cut. Like the position, it is
    // derived from the control frame, so every node mixes the same two
    // frames by the same amount with no clock involved.
    core::u64 blend_frames{};
    ShowFrame blend_from{};
    bool blend_moves{};

    bool operator==(const TransportAnchor&) const = default;
};

// The crossfade a node presents on one control frame: the show frame being
// left, and how far the mix has come. `mix()` is zero for all of the frame
// being left and approaches one as the anchor's own frame takes over.
struct TransportBlend final {
    ShowFrame from{};
    core::u64 elapsed{};
    core::u64 frames{};

    [[nodiscard]] double mix() const noexcept {
        return frames == 0U
            ? 1.0
            : static_cast<double>(elapsed) / static_cast<double>(frames);
    }

    bool operator==(const TransportBlend&) const = default;
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

// The anchor a node presents: its committed document's, unless an operator
// anchor that arrived ahead of the frame stream was authored on a later
// control frame. A node behind on frames then answers a stop, pause or seek at
// once - a stopped or paused anchor derives its position from nothing but
// itself - and a play holds on its first frame until the stream reaches it.
// Once the committed document carries that anchor, or a newer one, the
// document wins again.
[[nodiscard]] std::optional<TransportAnchor> presented_anchor(
    const std::optional<TransportAnchor>& committed,
    const std::optional<TransportAnchor>& priority) noexcept;

// The crossfade in progress on `control_frame`, or nothing outside one.
// Exact integer arithmetic on replicated state, like derive_show_frame.
[[nodiscard]] std::optional<TransportBlend> derive_transport_blend(
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
