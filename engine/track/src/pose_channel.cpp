#include <mediaserver/track/pose_channel.hpp>

#include <algorithm>

namespace mediaserver::track {

PoseChannelOutcome PoseChannel::accept(const StampedPose& sample) noexcept {
    if (!valid(sample.pose)) {
        ++counters_.rejected;
        return PoseChannelOutcome::Rejected;
    }
    if (any_accepted_) {
        if (sample.show_frame == newest_stamp_) {
            ++counters_.repeated;
            return PoseChannelOutcome::Repeated;
        }
        if (sample.show_frame < newest_stamp_) {
            ++counters_.stale;
            return PoseChannelOutcome::Stale;
        }
    }
    newest_stamp_ = sample.show_frame;
    any_accepted_ = true;

    // Stamps only ever increase here - anything else was refused above - so
    // the pending window stays ascending by appending, with no sort and no
    // search for where this one belongs.
    if (pending_count_ == pending_.size()) {
        // Full. The oldest pending pose is the one closest to being applied,
        // so it becomes the held pose rather than being dropped: a consumer
        // that stalled long enough to fill this should end up holding the
        // newest pose it could legitimately have used, not the oldest one it
        // happened to still have.
        applied_ = pending_.front();
        has_applied_ = true;
        std::rotate(pending_.begin(), pending_.begin() + 1, pending_.end());
        --pending_count_;
    }
    pending_[pending_count_] = sample;
    ++pending_count_;
    ++counters_.accepted;
    return PoseChannelOutcome::Accepted;
}

PoseForFrame PoseChannel::answer_for(core::u64 frame) const noexcept {
    const StampedPose* best = has_applied_ ? &applied_ : nullptr;
    for (std::size_t index = 0U; index < pending_count_; ++index) {
        if (pending_[index].show_frame > frame) {
            // Ascending, so nothing after this one applies either.
            break;
        }
        best = &pending_[index];
    }
    if (best == nullptr) {
        // Every pose this channel holds is stamped for a frame still ahead.
        // That is the ordinary state for the first few frames of a compensated
        // feed, and holding nothing is the honest answer: there is no pose for
        // this frame yet, and inventing one is what two nodes must never do.
        return PoseForFrame{};
    }
    return PoseForFrame{
        best->pose,
        best->show_frame,
        best->show_frame == frame ? PoseHold::Exact : PoseHold::Held,
    };
}

PoseForFrame PoseChannel::peek(core::u64 show_frame) const noexcept {
    return answer_for(show_frame);
}

PoseForFrame PoseChannel::resolve(core::u64 show_frame) noexcept {
    // Everything whose frame has now come round leaves the pending window and
    // becomes the held pose, newest last. Done before answering so the answer
    // and the state agree, and so the window cannot fill with poses that are
    // already in the past.
    std::size_t applied_now = 0U;
    while (applied_now < pending_count_
           && pending_[applied_now].show_frame <= show_frame) {
        applied_ = pending_[applied_now];
        has_applied_ = true;
        ++applied_now;
    }
    if (applied_now != 0U) {
        std::rotate(pending_.begin(),
                    pending_.begin() + static_cast<std::ptrdiff_t>(applied_now),
                    pending_.end());
        pending_count_ -= applied_now;
    }

    const PoseForFrame answer = answer_for(show_frame);
    // A driver may resolve the same show frame more than once - Program and a
    // second mapped output on one node, say - and that is one frame, not two.
    const bool new_frame = !resolved_any_ || show_frame != last_resolved_frame_;
    last_resolved_frame_ = show_frame;
    resolved_any_ = true;
    if (!new_frame) {
        return answer;
    }
    switch (answer.state) {
    case PoseHold::Exact:
        ++counters_.exact_frames;
        consecutive_holds_ = 0U;
        break;
    case PoseHold::Held:
        ++counters_.held_frames;
        ++consecutive_holds_;
        counters_.longest_hold =
            std::max(counters_.longest_hold, consecutive_holds_);
        break;
    case PoseHold::None:
        consecutive_holds_ = 0U;
        break;
    }
    return answer;
}

void PoseChannel::reset() noexcept {
    applied_ = StampedPose{};
    has_applied_ = false;
    pending_ = {};
    pending_count_ = 0U;
    newest_stamp_ = 0U;
    any_accepted_ = false;
    consecutive_holds_ = 0U;
    last_resolved_frame_ = 0U;
    resolved_any_ = false;
}

} // namespace mediaserver::track
