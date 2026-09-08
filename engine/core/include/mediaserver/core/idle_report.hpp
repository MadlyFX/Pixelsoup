#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>

// Why a frame-path feature is drawing nothing.
//
// Every reason a feature declines is individually legitimate - nothing
// authored, a switch off, this output not bound to the thing the feature needs,
// geometry that does not intersect - and on a wall they are all the same black
// rectangle. A node that cannot say which one it is has made a fault
// indistinguishable from a correct idle, and the operator finds out on camera.
//
// This is the counterpart to honest capability reporting: honest *idle*
// reporting. It exists because the inner frustum shipped unreachable for months
// and looked exactly like a show with no tracking, and one line of this turned
// hours of guessing into a single run.
namespace mediaserver::core {

// Enough for every declining condition in the frame path several times over.
// Past it the table stops recording, which is the right failure: repeating a
// reason every frame would be worse than the silence this replaces.
inline constexpr std::size_t max_idle_reasons = 64U;

// Says one reason once per process, and never says it again. Returns true only
// on the call that actually said it, which is what makes the dedup testable and
// lets a caller attach anything else it wants to do exactly once.
//
// `feature` and `reason` must be string literals or otherwise outlive the
// process: identity is by pointer, which is what keeps this cheap enough to sit
// on a path that runs every frame. Distinct literals have distinct addresses,
// so two call sites naming different reasons both get heard; two call sites
// sharing one pooled literal would be heard once, which is why a reason should
// name the feature's own condition rather than something generic.
//
// Lock-free and allocation-free by construction: a bounded array of atomic
// pointers, one compare-exchange to claim a slot. The frame path never blocks
// here, and after the first occurrence the whole call is a short scan of
// already-published pointers.
[[nodiscard]] inline bool report_idle_once(const char* feature,
                                          const char* reason) noexcept {
    if (feature == nullptr || reason == nullptr) {
        return false;
    }
    static std::array<std::atomic<const char*>, max_idle_reasons> said{};
    for (std::atomic<const char*>& slot : said) {
        const char* observed = slot.load(std::memory_order_acquire);
        if (observed == reason) {
            return false;
        }
        if (observed == nullptr) {
            const char* expected = nullptr;
            if (slot.compare_exchange_strong(expected, reason,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                static_cast<void>(
                    std::fprintf(stderr, "%s: %s\n", feature, reason));
                return true;
            }
            // Lost the race. If the winner wrote this very reason it has now
            // been said; otherwise keep scanning from the next slot.
            if (expected == reason) {
                return false;
            }
        }
    }
    return false;
}

} // namespace mediaserver::core
