#pragma once

#include <optional>

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/show/transport_state.hpp>

namespace mediaserver::show {

enum class TransportCommandError : core::u8 {
    InvalidConfiguration,
    FrameOutOfRange,
    // The control frame an operator action would be anchored to is behind one
    // already published. Re-anchoring into the past would make nodes derive a
    // position the Director never showed.
    ControlFrameRegression,
    AlreadyPlaying,
    NotPlaying,
};

// Director-side ownership of the replicated transport anchor.
//
// This is the transport half of what used to be a second ShowClock on the
// Director. The remaining clock is a pacer: it decides *when* a control frame
// is due. Where the show is, is decided here, as exact integer state keyed to
// the control-frame ordinal, so the Director and every node compute the
// presented frame with the same function over the same replicated values.
//
// Every mutator takes the control frame its result will be sealed into and
// returns the anchor to author. Nothing here allocates, reads a clock, or
// performs I/O.
class TransportAuthority final {
public:
    [[nodiscard]] static core::Result<TransportAuthority, TransportCommandError>
    create(const ShowClockConfig& config, cmd::FrameNumber at) noexcept;

    // Resumes from wherever the show currently is.
    [[nodiscard]] core::Result<TransportAnchor, TransportCommandError> play(
        cmd::FrameNumber at) noexcept;

    // Freezes the position the show has reached at `at`, so pausing holds the
    // frame the operator was looking at rather than the next one.
    [[nodiscard]] core::Result<TransportAnchor, TransportCommandError> pause(
        cmd::FrameNumber at) noexcept;

    [[nodiscard]] core::Result<TransportAnchor, TransportCommandError> stop(
        cmd::FrameNumber at) noexcept;

    // Exact frame seek, valid in any transport state. A seek while playing
    // re-anchors and keeps playing, so playback continues from exactly the
    // requested frame.
    [[nodiscard]] core::Result<TransportAnchor, TransportCommandError> seek(
        ShowFrame target,
        cmd::FrameNumber at) noexcept;

    // Reverses without moving the show: the anchor is re-cut at the position
    // currently derived for `at`.
    [[nodiscard]] core::Result<TransportAnchor, TransportCommandError>
    set_direction(TransportDirection direction, cmd::FrameNumber at) noexcept;

    // Changes the show range and loop flag without otherwise moving the show:
    // the current position is clamped into the new range and the state is kept.
    // Re-anchors like any other change, so nodes pick up the new bounds. Rejects
    // a degenerate range (last before first).
    [[nodiscard]] core::Result<TransportAnchor, TransportCommandError> set_range(
        ShowFrame first,
        ShowFrame last,
        bool loop,
        cmd::FrameNumber at) noexcept;

    // End-of-show hold. A non-looping show that has just derived its terminal
    // frame is re-anchored as paused, which is the same behaviour a local
    // ShowClock performs when it runs out of timeline. Returns the anchor to
    // author, or nothing when the transport is unchanged.
    [[nodiscard]] std::optional<TransportAnchor> settle(
        cmd::FrameNumber at) noexcept;

    [[nodiscard]] ShowFrame frame_at(
        cmd::FrameNumber control_frame) const noexcept {
        return derive_show_frame(anchor_, control_frame);
    }
    [[nodiscard]] const TransportAnchor& anchor() const noexcept {
        return anchor_;
    }
    [[nodiscard]] TransportState state() const noexcept { return anchor_.state; }

private:
    explicit TransportAuthority(const TransportAnchor& anchor) noexcept
        : anchor_(anchor) {}

    [[nodiscard]] core::Result<TransportAnchor, TransportCommandError> re_anchor(
        ShowFrame position,
        TransportState state,
        TransportDirection direction,
        cmd::FrameNumber at) noexcept;

    TransportAnchor anchor_{};
};

} // namespace mediaserver::show
