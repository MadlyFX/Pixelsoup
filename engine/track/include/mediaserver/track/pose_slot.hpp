#pragma once

#include <mediaserver/track/tracked_pose.hpp>

#include <atomic>
#include <optional>

// The handoff from a tracker thread to the Director's frame boundary.
//
// One writer - the thread that owns the tracker socket - and any number of
// readers. The frame boundary must never wait on that thread: a tracker that
// stalls, a datagram that arrives mid-sample, a receive loop descheduled by the
// operating system, none of them may hold up the Director's frame. So this is a
// sequence-counted slot rather than a mutex or a queue:
//
//  * `publish()` never blocks and never allocates.
//  * `sample()` never blocks. Concurrent with a publish it either reads the
//    complete previous pose or the complete new one, never a mix.
//  * There is no backlog. A pose that was superseded before anybody sampled it
//    is simply gone, which is the correct behaviour: the frame boundary wants
//    the newest pose, not every pose.
//
// The slot deliberately holds no notion of a show frame. Associating a sample
// with an exact 64-bit show frame is the Director's act, not the tracker's, and
// keeping the two apart is what makes it obvious that a node has no business
// owning one of these.
namespace mediaserver::track {

// One published pose and the count of how many have been published before it.
// The count is what tells a caller whether the tracker is still sending: two
// samples of the same `publications` mean nothing new arrived.
struct PoseSample final {
    TrackedPose pose{};
    core::u64 publications{};

    bool operator==(const PoseSample&) const = default;
};

class PoseSlot final {
public:
    PoseSlot() noexcept = default;
    PoseSlot(const PoseSlot&) = delete;
    PoseSlot& operator=(const PoseSlot&) = delete;
    PoseSlot(PoseSlot&&) = delete;
    PoseSlot& operator=(PoseSlot&&) = delete;
    ~PoseSlot() = default;

    // Writer side. A pose `valid()` rejects is refused and counted rather than
    // published, so a reader can never observe one.
    [[nodiscard]] bool publish(const TrackedPose& pose) noexcept;

    // Reader side. Absent until the first successful publish. Never waits: if a
    // publish is in flight this retries the read a bounded number of times and
    // then reports the last complete pose it managed to observe, or nothing.
    [[nodiscard]] std::optional<PoseSample> sample() const noexcept;

    // Diagnostics. Already-computed counters; reading them performs no
    // synchronization beyond a relaxed load.
    [[nodiscard]] core::u64 publications() const noexcept;
    [[nodiscard]] core::u64 refusals() const noexcept;

private:
    // Even means settled, odd means a publish is in progress.
    mutable std::atomic<core::u64> sequence_{0U};
    TrackedPose pose_{};
    core::u64 publications_{};
    std::atomic<core::u64> refusals_{0U};
};

} // namespace mediaserver::track
