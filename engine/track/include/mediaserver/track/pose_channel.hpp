#pragma once

#include <mediaserver/track/tracked_pose.hpp>

#include <array>
#include <cstddef>

// The per-frame pose channel, seen from the receiving end.
//
// The Director samples one pose at a frame boundary, stamps it with the exact
// 64-bit show frame it belongs to, and replicates it. This is what the other
// end does with that stream, and it exists as its own tested value because the
// interesting behaviour is entirely in what it refuses to do:
//
//  * A pose arriving for a frame already held is a repeat. The held pose does
//    not change, so a Director that resends does not jitter the wall.
//  * A pose arriving for an older frame than the one held is stale - a
//    reordered or replayed packet - and is refused rather than winding the
//    camera backwards.
//  * A pose stamped for a frame that has not arrived yet is **kept**, not
//    applied. This is what makes latency compensation mean anything: the
//    Director stamps a pose forward so that it lands when the wall is actually
//    showing that frame, and a channel that applied it on receipt would cancel
//    exactly the offset it was given.
//  * A show frame with no pose **holds** the last one. It does not extrapolate,
//    and this is the whole point. Extrapolation is locally reasonable and
//    globally fatal: two nodes that each invented a pose would invent different
//    ones, and the seam between their slices would tear.
//
// Nothing here reads a clock. "How long have we been holding" is counted in
// show frames, which every node counts identically.
namespace mediaserver::track {

// One pose and the exact show frame the Director associated it with. Latency
// compensation is already applied upstream: the frame in this value is the
// frame the pose is *for*, not the frame it was sampled on.
struct StampedPose final {
    TrackedPose pose{};
    core::u64 show_frame{};

    bool operator==(const StampedPose&) const = default;
};

enum class PoseChannelOutcome : core::u8 {
    // A newer frame than the one held. The channel advanced.
    Accepted,
    // The frame already held. Nothing changed.
    Repeated,
    // Older than the frame held. Refused.
    Stale,
    // Not a usable pose. Refused.
    Rejected,
};

enum class PoseHold : core::u8 {
    // Nothing has ever been received on this channel.
    None,
    // A pose stamped for exactly the requested show frame.
    Exact,
    // The last accepted pose, held because the requested frame has none.
    Held,
};

struct PoseForFrame final {
    TrackedPose pose{};
    // The frame the returned pose was stamped for. Equal to the requested
    // frame only when `state` is Exact.
    core::u64 stamped_frame{};
    PoseHold state{PoseHold::None};

    bool operator==(const PoseForFrame&) const = default;
};

struct PoseChannelCounters final {
    core::u64 accepted{};
    core::u64 repeated{};
    core::u64 stale{};
    core::u64 rejected{};
    // Show frames answered exactly, and show frames answered by holding.
    core::u64 exact_frames{};
    core::u64 held_frames{};
    // Longest run of consecutive held frames since the channel opened. This is
    // the number an operator actually wants when a tracker starts dropping.
    core::u64 longest_hold{};

    bool operator==(const PoseChannelCounters&) const = default;
};

// Poses stamped for frames that have not come round yet.
//
// The Director publishes one pose per control frame, so the number in flight is
// the authored latency compensation. This covers the largest the scene layer
// permits, with headroom; past it the oldest pending pose becomes the held one
// rather than the channel growing, because a bounded channel that degrades to
// holding is better on a stage than an unbounded one that allocates.
inline constexpr std::size_t max_pending_poses = 32U;

class PoseChannel final {
public:
    PoseChannel() noexcept = default;

    [[nodiscard]] PoseChannelOutcome accept(const StampedPose& sample) noexcept;

    // The pose for `show_frame`. Pure with respect to what it returns: the
    // answer depends only on the pose the channel holds and the frame asked
    // for, so two nodes fed the same stream answer identically. The counters it
    // advances are local diagnostics.
    [[nodiscard]] PoseForFrame resolve(core::u64 show_frame) noexcept;

    // The same answer without touching a counter, for a diagnostic panel that
    // must not perturb what it is reporting.
    [[nodiscard]] PoseForFrame peek(core::u64 show_frame) const noexcept;

    // Drops the held pose. Used at a project transition or a Director epoch
    // change, where continuing to hold a pose from the previous show would be
    // worse than having none.
    void reset() noexcept;

    [[nodiscard]] const PoseChannelCounters& counters() const noexcept {
        return counters_;
    }
    // Whether this channel has any pose at all - applied or still waiting for
    // its frame. False only before the first accepted pose and after a reset.
    [[nodiscard]] bool holding() const noexcept { return any_accepted_; }
    // Poses accepted whose frame has not come round yet.
    [[nodiscard]] std::size_t pending() const noexcept {
        return pending_count_;
    }
    // Consecutive show frames answered by holding, right now.
    [[nodiscard]] core::u64 consecutive_holds() const noexcept {
        return consecutive_holds_;
    }

private:
    // The answer for `frame`, without touching anything. Both `peek` and
    // `resolve` are this plus their own bookkeeping, so the two can never
    // disagree about what the pose for a frame is.
    [[nodiscard]] PoseForFrame answer_for(core::u64 frame) const noexcept;

    // The newest pose whose frame has already come round. This is what a frame
    // with no pose of its own holds.
    StampedPose applied_{};
    bool has_applied_{};
    // Stamped for frames still ahead, ascending. Kept until their frame
    // arrives, which is the whole of latency compensation.
    std::array<StampedPose, max_pending_poses> pending_{};
    std::size_t pending_count_{};
    // The newest frame ever accepted, which is what repeat and stale are
    // measured against.
    core::u64 newest_stamp_{};
    bool any_accepted_{};
    core::u64 consecutive_holds_{};
    // Guards against counting one show frame's hold twice when a driver
    // resolves the same frame more than once.
    core::u64 last_resolved_frame_{};
    bool resolved_any_{};
    PoseChannelCounters counters_{};
};

} // namespace mediaserver::track
