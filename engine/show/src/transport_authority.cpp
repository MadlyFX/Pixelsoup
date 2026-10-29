#include <mediaserver/show/transport_authority.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <tuple>

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

    // Playing from a playhead parked outside In/Out starts at In.
    if (state == TransportState::Playing
        && (position < anchor_.first_frame || anchor_.last_frame < position)) {
        position = anchor_.first_frame;
    }

    auto updated = anchor_;
    updated.anchor_control_frame = at;
    updated.anchor_show_frame = position;
    updated.state = state;
    updated.direction = direction;
    updated.scheduled = false;
    updated.blend_frames = 0U;
    updated.blend_from = ShowFrame{};
    updated.blend_moves = false;
    if (!valid_transport_anchor(updated)) {
        return AnchorResult::failure(TransportCommandError::FrameOutOfRange);
    }

    anchor_ = updated;
    held_cue_ = 0U;
    looping_cue_ = 0U;
    released_cue_ = 0U;
    restart_.reset();
    cues_checked_from_ = std::max(cues_checked_from_, at.value());
    return AnchorResult::success(updated);
}

ShowFrame TransportAuthority::section_start(const CuePoint& cue) const noexcept {
    // Cues partition the show into the sections an operator talks about.
    ShowFrame section = anchor_.first_frame;
    for (const CuePoint& earlier : cues_) {
        if (earlier.frame >= cue.frame) {
            break;
        }
        if (anchor_.first_frame <= earlier.frame) {
            section = earlier.frame;
        }
    }
    return section;
}

bool TransportAuthority::go_round(
    ShowFrame position,
    ShowFrame from,
    core::u64 fade,
    bool moves,
    core::u64 cue,
    cmd::FrameNumber at) noexcept {
    if (!re_anchor(position, TransportState::Playing, TransportDirection::Forward,
                   at)) {
        return false;
    }
    if (fade != 0U) {
        auto blended = anchor_;
        blended.blend_frames = fade;
        blended.blend_from = from;
        blended.blend_moves = moves;
        // A crossfade that cannot be shown is dropped for a cut, never for a
        // show that fails to go round.
        if (valid_transport_anchor(blended)) {
            anchor_ = blended;
        }
    }
    looping_cue_ = cue;
    anchor_.scheduled = true;
    return true;
}

bool TransportAuthority::cue_known(core::u64 id) const noexcept {
    return id != 0U
        && std::any_of(cues_.begin(), cues_.end(), [id](const CuePoint& cue) {
               return cue.id == id;
           });
}

std::optional<TransportAuthority::NextCue> TransportAuthority::next_cue(
    core::u64 from) const noexcept {
    if (!anchor_.cues_armed || anchor_.state != TransportState::Playing
        || anchor_.direction != TransportDirection::Forward || cues_.empty()
        || !valid_transport_anchor(anchor_)) {
        return std::nullopt;
    }
    const core::u64 first = anchor_.first_frame.value();
    const core::u64 last = anchor_.last_frame.value();
    const core::u64 span = last - first + 1U;
    const core::u64 position = anchor_.anchor_show_frame.value();
    const core::u64 origin = anchor_.anchor_control_frame.value();

    std::optional<NextCue> best;
    for (std::size_t index = 0U; index < cues_.size(); ++index) {
        const CuePoint& cue = cues_[index];
        const core::u64 frame = cue.frame.value();
        if (frame < first || frame > last) {
            continue;
        }
        // Steps from the anchor until the cue's frame is presented. The frame
        // the anchor starts on is never "reached": GO from a hold has to leave
        // it, and a show looped back to a section start must not stop there.
        core::u64 steps{};
        if (anchor_.loop) {
            steps = frame >= position ? frame - position
                                      : span - (position - frame);
            if (steps == 0U) {
                steps = span;
            }
            if (cue.id == released_cue_ && released_cue_ != 0U) {
                if (steps > std::numeric_limits<core::u64>::max() - span) {
                    continue;
                }
                steps += span;
            }
        } else {
            if (frame <= position
                || (cue.id == released_cue_ && released_cue_ != 0U)) {
                continue;
            }
            steps = frame - position;
        }
        if (steps > std::numeric_limits<core::u64>::max() - origin) {
            continue;
        }
        const core::u64 reached_at = origin + steps;
        if (reached_at < from) {
            continue;
        }
        // The sheet is sorted, so the first of equal arrivals is the
        // lowest-frame, lowest-id cue on every machine.
        if (!best.has_value() || reached_at < best->reached_at) {
            best = NextCue{index, reached_at};
        }
    }
    return best;
}

bool TransportAuthority::set_cues(
    std::span<const CuePoint> cues, cmd::FrameNumber at) noexcept {
    try {
        std::vector<CuePoint> sorted{cues.begin(), cues.end()};
        std::sort(sorted.begin(), sorted.end(),
                  [](const CuePoint& left, const CuePoint& right) noexcept {
                      return std::tuple{left.frame.value(), left.id}
                          < std::tuple{right.frame.value(), right.id};
                  });
        cues_ = std::move(sorted);
    } catch (const std::bad_alloc&) {
        return false;
    }
    // Anything presented before `at` has already been shown; only arrivals
    // from here on are the show reaching a cue.
    cues_checked_from_ = std::max(cues_checked_from_, at.value());
    if (!cue_known(held_cue_)) {
        held_cue_ = 0U;
    }
    if (!cue_known(looping_cue_)) {
        looping_cue_ = 0U;
    }
    if (!cue_known(released_cue_)) {
        released_cue_ = 0U;
    }
    if (restart_.has_value() && !cue_known(restart_->cue)) {
        // The cue it was waiting on is gone; the show stays where it is.
        restart_.reset();
    }
    return true;
}

AnchorResult TransportAuthority::set_cues_armed(
    bool armed,
    cmd::FrameNumber at) noexcept {
    if (at < anchor_.anchor_control_frame) {
        return AnchorResult::failure(
            TransportCommandError::ControlFrameRegression);
    }
    auto updated = anchor_;
    updated.anchor_control_frame = at;
    updated.anchor_show_frame = frame_at(at);
    updated.cues_armed = armed;
    updated.scheduled = false;
    updated.blend_frames = 0U;
    updated.blend_from = ShowFrame{};
    updated.blend_moves = false;
    if (!valid_transport_anchor(updated)) {
        return AnchorResult::failure(TransportCommandError::FrameOutOfRange);
    }
    anchor_ = updated;
    cues_checked_from_ = std::max(cues_checked_from_, at.value());
    if (!armed) {
        held_cue_ = 0U;
        looping_cue_ = 0U;
        released_cue_ = 0U;
        restart_.reset();
    }
    return AnchorResult::success(updated);
}

AnchorResult TransportAuthority::go(cmd::FrameNumber at) noexcept {
    if (at < anchor_.anchor_control_frame) {
        return AnchorResult::failure(
            TransportCommandError::ControlFrameRegression);
    }
    if (anchor_.state == TransportState::Playing
        && anchor_.direction == TransportDirection::Forward) {
        // Overlapping into the next pass, the pass fading out is the show the
        // operator is watching end: GO goes back to it and plays on past the
        // cue from there rather than round once more.
        if (const auto blend = derive_transport_blend(anchor_, at);
            blend.has_value() && anchor_.blend_moves && looping_cue_ != 0U) {
            const core::u64 cue = looping_cue_;
            const auto resumed = re_anchor(
                blend->from, TransportState::Playing,
                TransportDirection::Forward, at);
            if (resumed) {
                released_cue_ = cue;
            }
            return resumed;
        }
        if (looping_cue_ == 0U) {
            return AnchorResult::failure(TransportCommandError::AlreadyPlaying);
        }
        // Released, not re-cut: the show keeps playing exactly where it is
        // and simply does not go round when it next reaches the loop point.
        released_cue_ = looping_cue_;
        looping_cue_ = 0U;
        return AnchorResult::success(anchor_);
    }
    return re_anchor(
        frame_at(at), TransportState::Playing, TransportDirection::Forward, at);
}

CueStatus TransportAuthority::cue_status(cmd::FrameNumber at) const noexcept {
    CueStatus status{};
    status.held = anchor_.state == TransportState::Paused ? held_cue_ : 0U;
    status.looping =
        anchor_.state == TransportState::Playing || restart_.has_value()
        ? looping_cue_
        : 0U;
    if (restart_.has_value()) {
        status.frames_to_restart =
            restart_->at > at.value() ? restart_->at - at.value() : 0U;
    }
    if (!anchor_.cues_armed) {
        return status;
    }
    if (anchor_.state == TransportState::Playing) {
        if (const auto next = next_cue(at.value()); next.has_value()) {
            status.next = cues_[next->index].id;
            status.frames_to_next = next->reached_at - at.value();
        }
        return status;
    }
    // Stopped or paused: the cue GO would run to, which is the first one
    // after the frame on screen. A cue on that frame is where the show
    // already is.
    const core::u64 position = frame_at(at).value();
    for (const CuePoint& cue : cues_) {
        if (cue.frame.value() > position
            && cue.frame.value() <= anchor_.last_frame.value()) {
            status.next = cue.id;
            break;
        }
    }
    return status;
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
    // Inside In/Out a seek keeps the state; outside it parks the show paused,
    // since only a paused or stopped show may rest there.
    const bool inside = anchor_.first_frame <= target && target <= anchor_.last_frame;
    const TransportState state =
        inside || anchor_.state != TransportState::Playing
        ? anchor_.state
        : TransportState::Paused;
    return re_anchor(target, state, anchor_.direction, at);
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
    updated.scheduled = false;
    updated.blend_frames = 0U;
    updated.blend_from = ShowFrame{};
    updated.blend_moves = false;
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

    // Keep where the show is. A playing show is clamped into the new bounds;
    // a paused or stopped one stays put, inside or not.
    ShowFrame position = frame_at(at);
    if (anchor_.state == TransportState::Playing) {
        if (position < first) {
            position = first;
        }
        if (last < position) {
            position = last;
        }
    }

    auto updated = anchor_;
    updated.anchor_control_frame = at;
    updated.anchor_show_frame = position;
    updated.first_frame = first;
    updated.last_frame = last;
    updated.loop = loop;
    updated.scheduled = false;
    updated.blend_frames = 0U;
    updated.blend_from = ShowFrame{};
    updated.blend_moves = false;
    if (!valid_transport_anchor(updated)) {
        return AnchorResult::failure(TransportCommandError::FrameOutOfRange);
    }

    anchor_ = updated;
    return AnchorResult::success(updated);
}

std::optional<TransportAnchor> TransportAuthority::settle(
    cmd::FrameNumber at) noexcept {
    // A Loop that held its frame for its delay goes round now.
    if (restart_.has_value() && at.value() >= restart_->at) {
        const PendingRestart restart = *restart_;
        restart_.reset();
        if (go_round(restart.section, restart.from, restart.fade, false,
                     restart.cue, at)) {
            return anchor_;
        }
    }
    // A cue acts on the frame after the one that presented it, so the window
    // to examine is every presentation from the last check through `at - 1`.
    // Called every frame that is exactly one frame; a caller that skipped some
    // still has the earliest missed cue honoured rather than lost.
    if (at.value() > cues_checked_from_) {
        const core::u64 from = cues_checked_from_;
        cues_checked_from_ = at.value();
        if (const auto next = next_cue(from); next.has_value()) {
            const CuePoint cue = cues_[next->index];
            const bool presented = next->reached_at < at.value();
            if (cue.action == CueAction::Hold) {
                if (presented
                    && re_anchor(cue.frame, TransportState::Paused,
                                 anchor_.direction, at)) {
                    held_cue_ = cue.id;
                    anchor_.scheduled = true;
                    return anchor_;
                }
            } else {
                const ShowFrame section = section_start(cue);
                const core::u64 length =
                    cue.frame.value() - section.value() + 1U;
                if (cue.wait_frames != 0U) {
                    // Hold the cue frame, then go round. The crossfade starts
                    // from the held frame, so it may run the whole section.
                    if (presented
                        && re_anchor(cue.frame, TransportState::Paused,
                                     anchor_.direction, at)) {
                        looping_cue_ = cue.id;
                        anchor_.scheduled = true;
                        restart_ = PendingRestart{
                            at.value() + cue.wait_frames, section, cue.frame,
                            std::min(cue.fade_frames, length), cue.id};
                        return anchor_;
                    }
                } else {
                    // Overlapping passes: the next one starts `fade` frames
                    // before the cue so it is wholly on screen on the frame
                    // after it. Each pass keeps half the section to itself.
                    const core::u64 fade = std::min(cue.fade_frames, length / 2U);
                    const core::u64 switched = next->reached_at + 1U;
                    const core::u64 starts =
                        switched > fade ? switched - fade : 0U;
                    if (starts <= at.value()) {
                        // Late - the show arrived inside the overlap - the
                        // crossfade is whatever is left of it.
                        const core::u64 left =
                            switched > at.value() ? switched - at.value() : 0U;
                        if (go_round(ShowFrame{section.value() + (fade - left)},
                                     frame_at(at), left, true, cue.id, at)) {
                            return anchor_;
                        }
                    }
                }
            }
        }
    }
    if (!reached_end(anchor_, at)) {
        return std::nullopt;
    }
    if (!re_anchor(
            frame_at(at), TransportState::Paused, anchor_.direction, at)) {
        return std::nullopt;
    }
    anchor_.scheduled = true;
    return anchor_;
}

} // namespace mediaserver::show
