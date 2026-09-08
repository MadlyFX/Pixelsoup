#pragma once

#include <algorithm>
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
            anchor_ = anchor;
            cursor_ = asset_activation;
            previous_complete_ = false;
            return Selection{cursor_, show::derive_show_frame(anchor, cursor_)};
        }
        if (anchor.state != show::TransportState::Playing) {
            anchor_ = anchor;
            cursor_ = latest_control_frame;
            previous_complete_ = false;
            return Selection{cursor_, show::derive_show_frame(anchor, cursor_)};
        }

        const core::u64 latest = latest_control_frame.value();
        const core::u64 delayed =
            latest > headroom_frames_ ? latest - headroom_frames_ : 0U;
        const core::u64 desired =
            std::max(delayed, anchor.anchor_control_frame.value());

        if (!anchor_.has_value() || *anchor_ != anchor ||
            cursor_.value() > latest) {
            anchor_ = anchor;
            cursor_ = cmd::FrameNumber{desired};
            previous_complete_ = false;
        } else if (at_boundary && previous_complete_ &&
                   cursor_.value() < desired) {
            // Catch up at normal playback speed. An underrun therefore grows
            // latency rather than causing a jump when decoding recovers.
            cursor_ = cmd::FrameNumber{cursor_.value() + 1U};
            previous_complete_ = false;
        }

        return Selection{cursor_, show::derive_show_frame(anchor, cursor_)};
    }

    void observe(bool complete) noexcept { previous_complete_ = complete; }

    [[nodiscard]] core::u64
    latency_frames(cmd::FrameNumber latest_control_frame) const noexcept {
        return latest_control_frame.value() > cursor_.value()
                   ? latest_control_frame.value() - cursor_.value()
                   : 0U;
    }

  private:
    std::optional<show::TransportAnchor> anchor_;
    cmd::FrameNumber cursor_{};
    bool previous_complete_{};
    core::u64 headroom_frames_{default_headroom_frames};
};

} // namespace mediaserver::app
