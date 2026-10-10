#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/show/transport_state.hpp>

namespace mediaserver::app {

// Output nodes deliberately trail the replicated control stream. The delay is
// decode headroom: while control frame N is arriving, the renderer presents
// N-headroom and asks the media scheduler for the frames between them. If a
// complete output frame is not ready, the cursor stops instead of allowing
// independently completing plates to pull different parts of the picture
// forward. The lost time becomes additional latency, never a skipped position.
//
// An operator anchor - a seek, a pause, a play - restarts the buffer at the new
// position so the wall answers the hand that asked. A scheduled anchor, which
// the show authored itself on reaching a cue or its end, does not: the frames
// already buffered from the previous anchor are still the show, so the cursor
// plays on through them with that anchor and reaches the new one on its own
// control frame. That is what lets a Hold show every frame up to its cue and a
// Loop wrap without dropping or freezing frames on any node.
class OutputPlaybackBuffer final {
  public:
    static constexpr core::u64 default_headroom_frames = 12U;

    explicit OutputPlaybackBuffer(
        core::u64 headroom_frames = default_headroom_frames) noexcept
        : headroom_frames_(headroom_frames) {}

    [[nodiscard]] core::u64 headroom_frames() const noexcept {
        return headroom_frames_;
    }

    struct Selection final {
        cmd::FrameNumber control_frame{};
        show::ShowFrame show_frame{};
        // A Loop cue's crossfade in force on that control frame, from the
        // same anchor as the show frame.
        std::optional<show::TransportBlend> blend;

        bool operator==(const Selection &) const = default;
    };

    [[nodiscard]] Selection select(const show::TransportAnchor &anchor,
                                   cmd::FrameNumber latest_control_frame,
                                   bool at_boundary,
                                   cmd::FrameNumber asset_activation = {}) noexcept {
        // A renderer that prepared the complete replacement before activation
        // must draw its exact permitted snapshot. Decode headroom must not
        // strand that snapshot behind a held, unpublished candidate.
        if (asset_activation.value() != 0 &&
            latest_control_frame == asset_activation) {
            history_size_ = 0U;
            remember(anchor);
            cursor_ = asset_activation;
            previous_complete_ = false;
            return Selection{cursor_, show::derive_show_frame(anchor, cursor_),
                             show::derive_transport_blend(anchor, cursor_)};
        }
        const core::u64 latest = latest_control_frame.value();
        const bool changed = !anchor_.has_value() || *anchor_ != anchor;
        if (changed && anchor.scheduled && can_play_through(anchor, latest)) {
            remember(anchor);
        } else if (changed) {
            history_size_ = 0U;
            remember(anchor);
            if (anchor.state != show::TransportState::Playing) {
                cursor_ = latest_control_frame;
            } else {
                cursor_ = cmd::FrameNumber{std::max(
                    delayed(latest), anchor.anchor_control_frame.value())};
            }
            previous_complete_ = false;
            return selection_at(cursor_);
        }

        // Still presenting frames buffered from an earlier anchor: walk them
        // at normal speed whatever the newest anchor's state, and only follow
        // a stopped show once the cursor has reached the frame it stopped on.
        const bool playing_through =
            history_size_ > 1U &&
            cursor_ < history_[history_size_ - 1U].anchor_control_frame &&
            within_reach(latest);
        if (anchor.state != show::TransportState::Playing && !playing_through) {
            history_size_ = 0U;
            remember(anchor);
            cursor_ = latest_control_frame;
            previous_complete_ = false;
            return selection_at(cursor_);
        }

        if (cursor_.value() > latest) {
            history_size_ = 0U;
            remember(anchor);
            cursor_ = cmd::FrameNumber{
                std::max(delayed(latest), anchor.anchor_control_frame.value())};
            previous_complete_ = false;
        } else if (at_boundary && previous_complete_ &&
                   cursor_.value() < desired(anchor, latest)) {
            // Catch up at normal playback speed. An underrun therefore grows
            // latency rather than causing a jump when decoding recovers.
            cursor_ = cmd::FrameNumber{cursor_.value() + 1U};
            previous_complete_ = false;
        }
        forget_passed();
        return selection_at(cursor_);
    }

    void observe(bool complete) noexcept { previous_complete_ = complete; }

    [[nodiscard]] core::u64
    latency_frames(cmd::FrameNumber latest_control_frame) const noexcept {
        return latest_control_frame.value() > cursor_.value()
                   ? latest_control_frame.value() - cursor_.value()
                   : 0U;
    }

  private:
    // Enough for a Loop section far shorter than the headroom to wrap several
    // times while its frames are still buffered.
    static constexpr std::size_t max_history = 32U;

    [[nodiscard]] core::u64 delayed(core::u64 latest) const noexcept {
        return latest > headroom_frames_ ? latest - headroom_frames_ : 0U;
    }

    // Where the cursor is heading. Frames before the newest anchor's own
    // control frame are only worth walking towards while an earlier anchor
    // still describes them.
    [[nodiscard]] core::u64 desired(const show::TransportAnchor& anchor,
                                    core::u64 latest) const noexcept {
        const core::u64 target = delayed(latest);
        return history_size_ > 1U
                   ? std::max(target, cursor_.value())
                   : std::max(target, anchor.anchor_control_frame.value());
    }

    // Playing through only makes sense for an output keeping up: one about
    // its ordinary headroom behind. An output stalled far behind - its media
    // never arrived - would otherwise sit on an old frame long after the show
    // stopped, so it restarts on the new anchor like any other.
    [[nodiscard]] bool within_reach(core::u64 latest) const noexcept {
        return cursor_.value() <= latest &&
               latest - cursor_.value() <= 2U * headroom_frames_ + 1U;
    }

    // A scheduled anchor plays through only when there is something buffered
    // to play: the previous anchor was moving and the cursor has not yet
    // reached the new one.
    [[nodiscard]] bool can_play_through(const show::TransportAnchor& anchor,
                                        core::u64 latest) const noexcept {
        return history_size_ > 0U && history_size_ < max_history &&
               history_[history_size_ - 1U].state ==
                   show::TransportState::Playing &&
               within_reach(latest) &&
               cursor_ < anchor.anchor_control_frame &&
               history_[history_size_ - 1U].anchor_control_frame <=
                   anchor.anchor_control_frame;
    }

    void remember(const show::TransportAnchor& anchor) noexcept {
        if (history_size_ == max_history) {
            history_size_ = 0U;
        }
        history_[history_size_++] = anchor;
        anchor_ = anchor;
    }

    // Anchors the cursor has fully passed describe nothing it will draw.
    void forget_passed() noexcept {
        std::size_t passed = 0U;
        while (passed + 1U < history_size_ &&
               history_[passed + 1U].anchor_control_frame <= cursor_) {
            ++passed;
        }
        if (passed == 0U) {
            return;
        }
        for (std::size_t index = passed; index < history_size_; ++index) {
            history_[index - passed] = history_[index];
        }
        history_size_ -= passed;
    }

    // The show frame at a buffered control frame comes from the anchor that
    // was in force for it, not from whichever arrived last.
    [[nodiscard]] const show::TransportAnchor &anchor_at(
        cmd::FrameNumber control) const noexcept {
        std::size_t index = 0U;
        while (index + 1U < history_size_ &&
               history_[index + 1U].anchor_control_frame <= control) {
            ++index;
        }
        return history_[index];
    }

    [[nodiscard]] Selection selection_at(cmd::FrameNumber control) const noexcept {
        const show::TransportAnchor &anchor = anchor_at(control);
        return Selection{control, show::derive_show_frame(anchor, control),
                         show::derive_transport_blend(anchor, control)};
    }

    std::optional<show::TransportAnchor> anchor_;
    std::array<show::TransportAnchor, max_history> history_{};
    std::size_t history_size_{};
    cmd::FrameNumber cursor_{};
    bool previous_complete_{};
    core::u64 headroom_frames_{default_headroom_frames};
};

} // namespace mediaserver::app
