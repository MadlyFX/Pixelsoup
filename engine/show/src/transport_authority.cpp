#include <mediaserver/show/transport_authority.hpp>

namespace mediaserver::show {
namespace {

using CreateResult = core::Result<TransportAuthority, TransportCommandError>;
using AnchorResult = core::Result<TransportAnchor, TransportCommandError>;

} // namespace

CreateResult TransportAuthority::create(
    const ShowClockConfig& config,
    cmd::FrameNumber at) noexcept {
    TransportAnchor anchor{};
    anchor.anchor_control_frame = at;
    anchor.anchor_show_frame = config.first_frame;
    anchor.state = TransportState::Stopped;
    anchor.direction = TransportDirection::Forward;
    anchor.rate = config.rate;
    anchor.first_frame = config.first_frame;
    anchor.last_frame = config.last_frame;
    anchor.loop = config.loop;

    if (!valid_transport_anchor(anchor)) {
        return CreateResult::failure(
            TransportCommandError::InvalidConfiguration);
    }
    return CreateResult::success(TransportAuthority{anchor});
}

AnchorResult TransportAuthority::re_anchor(
    ShowFrame position,
    TransportState state,
    TransportDirection direction,
    cmd::FrameNumber at) noexcept {
    if (at < anchor_.anchor_control_frame) {
        return AnchorResult::failure(
            TransportCommandError::ControlFrameRegression);
    }

    auto updated = anchor_;
    updated.anchor_control_frame = at;
    updated.anchor_show_frame = position;
    updated.state = state;
    updated.direction = direction;
    if (!valid_transport_anchor(updated)) {
        return AnchorResult::failure(TransportCommandError::FrameOutOfRange);
    }

    anchor_ = updated;
    return AnchorResult::success(updated);
}

AnchorResult TransportAuthority::play(cmd::FrameNumber at) noexcept {
    if (anchor_.state == TransportState::Playing) {
        return AnchorResult::failure(TransportCommandError::AlreadyPlaying);
    }
    return re_anchor(
        frame_at(at), TransportState::Playing, anchor_.direction, at);
}

AnchorResult TransportAuthority::pause(cmd::FrameNumber at) noexcept {
    if (anchor_.state != TransportState::Playing) {
        return AnchorResult::failure(TransportCommandError::NotPlaying);
    }
    return re_anchor(
        frame_at(at), TransportState::Paused, anchor_.direction, at);
}

AnchorResult TransportAuthority::stop(cmd::FrameNumber at) noexcept {
    return re_anchor(
        anchor_.first_frame,
        TransportState::Stopped,
        anchor_.direction,
        at);
}

AnchorResult TransportAuthority::seek(
    ShowFrame target,
    cmd::FrameNumber at) noexcept {
    if (target < anchor_.first_frame || anchor_.last_frame < target) {
        return AnchorResult::failure(TransportCommandError::FrameOutOfRange);
    }
    return re_anchor(target, anchor_.state, anchor_.direction, at);
}

AnchorResult TransportAuthority::set_direction(
    TransportDirection direction,
    cmd::FrameNumber at) noexcept {
    return re_anchor(frame_at(at), anchor_.state, direction, at);
}

AnchorResult TransportAuthority::set_rate(
    FrameRate rate,
    cmd::FrameNumber at) noexcept {
    if (at < anchor_.anchor_control_frame) {
        return AnchorResult::failure(
            TransportCommandError::ControlFrameRegression);
    }

    auto updated = anchor_;
    updated.anchor_control_frame = at;
    updated.anchor_show_frame = frame_at(at);
    updated.rate = rate;
    if (!valid_transport_anchor(updated)) {
        return AnchorResult::failure(
            TransportCommandError::InvalidConfiguration);
    }

    anchor_ = updated;
    return AnchorResult::success(updated);
}

AnchorResult TransportAuthority::set_range(
    ShowFrame first,
    ShowFrame last,
    bool loop,
    cmd::FrameNumber at) noexcept {
    if (at < anchor_.anchor_control_frame) {
        return AnchorResult::failure(
            TransportCommandError::ControlFrameRegression);
    }
    if (last < first) {
        return AnchorResult::failure(
            TransportCommandError::InvalidConfiguration);
    }

    // Keep where the show is, clamped into the new bounds.
    ShowFrame position = frame_at(at);
    if (position < first) {
        position = first;
    }
    if (last < position) {
        position = last;
    }

    auto updated = anchor_;
    updated.anchor_control_frame = at;
    updated.anchor_show_frame = position;
    updated.first_frame = first;
    updated.last_frame = last;
    updated.loop = loop;
    if (!valid_transport_anchor(updated)) {
        return AnchorResult::failure(TransportCommandError::FrameOutOfRange);
    }

    anchor_ = updated;
    return AnchorResult::success(updated);
}

std::optional<TransportAnchor> TransportAuthority::settle(
    cmd::FrameNumber at) noexcept {
    if (!reached_end(anchor_, at)) {
        return std::nullopt;
    }
    auto held = re_anchor(
        frame_at(at), TransportState::Paused, anchor_.direction, at);
    if (!held) {
        return std::nullopt;
    }
    return std::move(held).value();
}

} // namespace mediaserver::show
