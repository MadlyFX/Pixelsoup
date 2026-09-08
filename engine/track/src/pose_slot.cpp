#include <mediaserver/track/pose_slot.hpp>

namespace mediaserver::track {
namespace {

// A reader that loses the race this many times in a row has an unusually
// aggressive writer, not a broken slot. Giving up and reporting nothing keeps
// the bound on a frame boundary hard rather than statistical.
constexpr int max_sample_attempts = 8;

} // namespace

bool PoseSlot::publish(const TrackedPose& pose) noexcept {
    if (!valid(pose)) {
        refusals_.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    const core::u64 start = sequence_.load(std::memory_order_relaxed);
    // Odd marks the write in progress. The release fence pairs with the
    // reader's acquire load of the same counter.
    sequence_.store(start + 1U, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    pose_ = pose;
    ++publications_;
    std::atomic_thread_fence(std::memory_order_release);
    sequence_.store(start + 2U, std::memory_order_relaxed);
    return true;
}

std::optional<PoseSample> PoseSlot::sample() const noexcept {
    for (int attempt = 0; attempt < max_sample_attempts; ++attempt) {
        const core::u64 before = sequence_.load(std::memory_order_relaxed);
        if ((before & 1U) != 0U) {
            continue;
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        PoseSample result{pose_, publications_};
        std::atomic_thread_fence(std::memory_order_acquire);
        if (sequence_.load(std::memory_order_relaxed) != before) {
            continue;
        }
        if (result.publications == 0U) {
            return std::nullopt;
        }
        return result;
    }
    contended_.fetch_add(1U, std::memory_order_relaxed);
    return std::nullopt;
}

core::u64 PoseSlot::publications() const noexcept {
    // Read through the sequence counter rather than the field: a torn count is
    // as wrong as a torn pose, and this costs one relaxed load.
    const auto observed = sample();
    return observed.has_value() ? observed->publications : 0U;
}

core::u64 PoseSlot::refusals() const noexcept {
    return refusals_.load(std::memory_order_relaxed);
}

core::u64 PoseSlot::contended_samples() const noexcept {
    return contended_.load(std::memory_order_relaxed);
}

} // namespace mediaserver::track
