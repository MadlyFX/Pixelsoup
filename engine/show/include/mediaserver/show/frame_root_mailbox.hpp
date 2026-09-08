#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/show/transport_state.hpp>

namespace mediaserver::show {

// One committed control frame handed from the commit boundary to playback.
// `transport` is read from `root` once, on the publishing side, so the
// playback thread never has to walk the document to learn what to present.
//
// `show_frame` is that anchor evaluated at this exact control frame. Deriving
// it here rather than at present time means the position and the frame it was
// derived from can never be a mismatched pair, and playback still receives one
// ready-to-render number.
struct PublishedFrame final {
    cmd::FrameNumber control_frame{};
    doc::Version version{};
    ShowFrame show_frame{};
    std::optional<TransportAnchor> transport{};
    doc::Root root{};
};

enum class PublishStatus : core::u8 {
    Published,
    // Playback is more than `capacity` frames behind. Nothing is written and
    // no earlier frame is discarded: falling behind is a reportable condition,
    // not something the handoff resolves by silently dropping approved state.
    Overflow,
};

enum class MailboxError : core::u8 {
    InvalidCapacity,
    AllocationFailed,
};

// A bounded parking area for roots a real-time thread has stopped using.
//
// Releasing the last reference to a document root frees its trie, and free()
// is no more admissible on a frame boundary than malloc(). The playback thread
// moves the outgoing root here instead; a housekeeping caller drains it away
// from the boundary. A full ring is reported rather than hidden, because the
// alternative is an unbounded backlog of live snapshots.
class RootRetirementRing final {
public:
    [[nodiscard]] static core::Result<RootRetirementRing, MailboxError> create(
        std::size_t capacity) noexcept;

    RootRetirementRing(const RootRetirementRing&) = delete;
    RootRetirementRing& operator=(const RootRetirementRing&) = delete;
    RootRetirementRing(RootRetirementRing&& other) noexcept;
    RootRetirementRing& operator=(RootRetirementRing&& other) noexcept;
    ~RootRetirementRing() = default;

    // Real-time side. Never allocates and never runs a trie destructor.
    // Returns false when the ring is full; the caller then owns the decision
    // to destroy inline and count the stall.
    [[nodiscard]] bool retire(doc::Root&& root) noexcept;

    // Housekeeping side. Releases every parked root and returns the count.
    [[nodiscard]] std::size_t drain() noexcept;

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t size() const noexcept {
        return static_cast<std::size_t>(
            tail_.load(std::memory_order_acquire)
            - head_.load(std::memory_order_acquire));
    }
    [[nodiscard]] core::u64 retired_count() const noexcept {
        return tail_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] core::u64 rejected_count() const noexcept {
        return rejected_.load(std::memory_order_relaxed);
    }

private:
    RootRetirementRing(
        std::unique_ptr<doc::Root[]> slots,
        std::size_t capacity) noexcept
        : slots_(std::move(slots)), capacity_(capacity), mask_(capacity - 1U) {}

    std::unique_ptr<doc::Root[]> slots_;
    std::size_t capacity_{};
    std::size_t mask_{};
    std::atomic<core::u64> head_{};
    std::atomic<core::u64> tail_{};
    std::atomic<core::u64> rejected_{};
};

[[nodiscard]] constexpr bool is_power_of_two(std::size_t value) noexcept {
    return value != 0U && (value & (value - 1U)) == 0U;
}

// A bounded single-producer/single-consumer handoff of immutable document
// roots.
//
// Every slot is allocated once at construction. Publishing copies a
// shared_ptr and stores an index, so the commit boundary performs no
// allocation, no lock, and no system call. Consumption moves the root out of
// its slot, which leaves that slot empty; a later publish into the same slot
// therefore destroys nothing and cannot deallocate on the boundary either.
//
// Exactly one thread may publish and exactly one may consume; both may be the
// same thread. Moving is defined only before either endpoint is handed to
// another thread, which is why the move operations copy the cursors relaxed.
class FrameRootMailbox final {
public:
    [[nodiscard]] static core::Result<FrameRootMailbox, MailboxError> create(
        std::size_t capacity) noexcept;

    FrameRootMailbox(const FrameRootMailbox&) = delete;
    FrameRootMailbox& operator=(const FrameRootMailbox&) = delete;
    FrameRootMailbox(FrameRootMailbox&& other) noexcept;
    FrameRootMailbox& operator=(FrameRootMailbox&& other) noexcept;
    ~FrameRootMailbox() = default;

    // Producer side. Allocation-free and wait-free. The frame is moved out
    // only when it is actually published: an overflow leaves the caller
    // holding it, so a full mailbox never turns into a trie release on the
    // frame boundary.
    [[nodiscard]] PublishStatus publish(PublishedFrame&& frame) noexcept;

    // Consumer side. Returns false when nothing new has been published.
    // On success `out` holds the oldest unconsumed frame and its slot is left
    // empty.
    [[nodiscard]] bool consume(PublishedFrame& out) noexcept;

    // Consumer side. Drains to the newest published frame, reporting how many
    // intermediate frames were skipped. Playback that is behind should present
    // the newest approved state rather than replay a backlog. Skipped roots go
    // to `retirement` when one is supplied, which keeps this call free of
    // deallocation; without one they are released inline.
    // A nonzero stop_at preserves an activation snapshot and leaves later
    // frames queued. The producer continues committing while it is presented.
    [[nodiscard]] bool consume_latest(
        PublishedFrame& out,
        std::size_t& skipped,
        RootRetirementRing* retirement = nullptr,
        cmd::FrameNumber stop_at = {}) noexcept;

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t size() const noexcept {
        return static_cast<std::size_t>(
            tail_.load(std::memory_order_acquire)
            - head_.load(std::memory_order_acquire));
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0U; }
    // Producer-side precheck. A caller that must not deallocate uses this to
    // avoid building a frame it would then have to release.
    [[nodiscard]] bool full() const noexcept { return size() >= capacity_; }
    [[nodiscard]] core::u64 published_count() const noexcept {
        return tail_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] core::u64 consumed_count() const noexcept {
        return head_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] core::u64 overflow_count() const noexcept {
        return overflows_.load(std::memory_order_relaxed);
    }

private:
    FrameRootMailbox(
        std::unique_ptr<PublishedFrame[]> slots,
        std::size_t capacity) noexcept
        : slots_(std::move(slots)), capacity_(capacity), mask_(capacity - 1U) {}

    std::unique_ptr<PublishedFrame[]> slots_;
    std::size_t capacity_{};
    std::size_t mask_{};
    std::atomic<core::u64> head_{};
    std::atomic<core::u64> tail_{};
    std::atomic<core::u64> overflows_{};
};

} // namespace mediaserver::show
