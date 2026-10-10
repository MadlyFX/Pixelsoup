#pragma once

#include <optional>
#include <span>
#include <vector>

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/show/transport_state.hpp>

namespace mediaserver::show {

// What a cue does to a show that plays onto it. Cues only act on a show
// playing forward with cues armed: scrubbing, stepping, and reverse play walk
// straight past them, because those are an operator looking for something
// rather than a show being run.
enum class CueAction : core::u8 {
    // Stops on the cue's frame and waits for GO.
    Hold,
    // Plays the section ending on the cue - from the cue before it, or from
    // the start of the range when there is none - round and round until GO
    // lets the show run on past it. The vamp an operator holds a stage on
    // while talent gets ready.
    //
    // A Loop may wait and may crossfade. With a wait, the show holds the cue
    // frame that long before going round, and a crossfade then dissolves the
    // held frame into the restarted section. With no wait, a crossfade
    // overlaps the passes instead: the next pass starts that many frames
    // before the cue and the end of this one fades out over it, so the loop
    // point has no seam.
    Loop,
};

// One cue as the transport sees it: a frame, what happens there, and an
// opaque identity the Director reports back (the cue's document node).
struct CuePoint final {
    ShowFrame frame{};
    CueAction action{CueAction::Hold};
    core::u64 id{};
    // Loop only, in control frames: how long the show holds the cue frame
    // before going round, and how long the crossfade into the next pass is.
    core::u64 wait_frames{};
    core::u64 fade_frames{};

    bool operator==(const CuePoint&) const = default;
};

// Where the show stands against its cues, for the operator surface. Ids are
// CuePoint::id, zero meaning none.
struct CueStatus final {
    // Stopped on a Hold cue, waiting for GO.
    core::u64 held{};
    // Inside a Loop cue's section, which repeats until GO.
    core::u64 looping{};
    // The cue the show reaches next: the one a playing show is heading for,
    // or the one GO would run to from where a stopped show sits.
    core::u64 next{};
    // Control frames until a playing show presents `next`. Absent while the
    // show is not playing towards it.
    std::optional<core::u64> frames_to_next;
    // Control frames until a Loop cue waiting out its delay goes round.
    std::optional<core::u64> frames_to_restart;

    bool operator==(const CueStatus&) const = default;
};

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

    // Re-anchors at the position reached under the old cadence, then changes
    // the exact rational rate carried by the replicated transport node. The
    // Director changes its control-frame pacer at this same boundary, so one
    // control frame continues to mean one show frame on every node.
    [[nodiscard]] core::Result<TransportAnchor, TransportCommandError> set_rate(
        FrameRate rate,
        cmd::FrameNumber at) noexcept;

    // Changes the show range and loop flag without otherwise moving the show:
    // the current position is clamped into the new range and the state is kept.
    // Re-anchors like any other change, so nodes pick up the new bounds. Rejects
    // a degenerate range (last before first).
    [[nodiscard]] core::Result<TransportAnchor, TransportCommandError> set_range(
        ShowFrame first,
        ShowFrame last,
        bool loop,
        cmd::FrameNumber at) noexcept;

    // Called once per control frame. Acts on the cue a playing show has just
    // presented - re-anchoring paused on a Hold, or back to the section start
    // for a Loop - and otherwise performs the end-of-show hold: a non-looping
    // show that has just derived its terminal frame is re-anchored as paused,
    // which is what a local ShowClock does when it runs out of timeline.
    // Returns the anchor to author, or nothing when the transport is
    // unchanged.
    //
    // A cue acts on the control frame after the one that presented it. That
    // frame is the one the new anchor is sealed into, so every node presents
    // the cue frame, then the held (or looped-to) frame, with no frame of
    // overshoot in between - provided this is called for every frame.
    [[nodiscard]] std::optional<TransportAnchor> settle(
        cmd::FrameNumber at) noexcept;

    // Replaces the cue sheet. Cue frames the show has already played past at
    // `at` stay past: a cue added behind a running show never pulls it back.
    // Allocates, so the Director calls it when the document changes rather
    // than on a boundary. Returns false, leaving the sheet unchanged, when the
    // copy cannot be allocated.
    [[nodiscard]] bool set_cues(
        std::span<const CuePoint> cues, cmd::FrameNumber at) noexcept;
    [[nodiscard]] std::span<const CuePoint> cues() const noexcept {
        return cues_;
    }

    // Arms or disarms every cue without moving the show. Disarmed, the
    // transport plays straight through them, which is what editing wants.
    [[nodiscard]] core::Result<TransportAnchor, TransportCommandError>
    set_cues_armed(bool armed, cmd::FrameNumber at) noexcept;

    // The operator's one live action. Held on a cue, or stopped anywhere, it
    // plays forward from where the show sits; inside a looping section it lets
    // the show run on past the loop point once instead of going round again.
    // A Loop waiting out its delay plays on from the cue frame, and one
    // overlapping into its next pass goes back to the pass that was fading
    // out and plays on past the cue from there.
    // A show already playing through its cues refuses it as AlreadyPlaying,
    // because GO never skips content.
    [[nodiscard]] core::Result<TransportAnchor, TransportCommandError> go(
        cmd::FrameNumber at) noexcept;

    [[nodiscard]] CueStatus cue_status(cmd::FrameNumber at) const noexcept;

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

    // Every operator re-anchor goes through here, and every one of them ends
    // whatever cue the show was holding or looping on: the operator has taken
    // the show somewhere else.
    [[nodiscard]] core::Result<TransportAnchor, TransportCommandError> re_anchor(
        ShowFrame position,
        TransportState state,
        TransportDirection direction,
        cmd::FrameNumber at) noexcept;

    struct NextCue final {
        std::size_t index{};
        // The control frame on which the cue's frame is presented.
        core::u64 reached_at{};
    };
    // The first armed cue a playing show will present at or after control
    // frame `from`, measured from the current anchor. Allocation-free.
    [[nodiscard]] std::optional<NextCue> next_cue(
        core::u64 from) const noexcept;
    [[nodiscard]] bool cue_known(core::u64 id) const noexcept;
    // The frame a Loop cue's section starts on: the cue before it, or the
    // start of the range when there is none.
    [[nodiscard]] ShowFrame section_start(const CuePoint& cue) const noexcept;
    // Goes round a Loop: plays from `position`, opening with a crossfade of
    // `fade` frames from `from` when that is not zero.
    [[nodiscard]] bool go_round(
        ShowFrame position,
        ShowFrame from,
        core::u64 fade,
        bool moves,
        core::u64 cue,
        cmd::FrameNumber at) noexcept;

    // A Loop cue holding its frame until the show goes round.
    struct PendingRestart final {
        core::u64 at{};
        ShowFrame section{};
        ShowFrame from{};
        core::u64 fade{};
        core::u64 cue{};
    };

    TransportAnchor anchor_{};
    std::vector<CuePoint> cues_;
    // The first control frame whose presented cue has not been acted on yet.
    core::u64 cues_checked_from_{};
    core::u64 held_cue_{};
    core::u64 looping_cue_{};
    // A loop GO has released. Its next arrival is let through once.
    core::u64 released_cue_{};
    std::optional<PendingRestart> restart_;
};

} // namespace mediaserver::show
