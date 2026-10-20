#pragma once

#include <mediaserver/core/types.hpp>

#include <chrono>

namespace mediaserver::show {

// Blocks a frame loop until its next boundary.
//
// std::this_thread::sleep_for on Windows waits in whole system-timer ticks
// (15.6 ms unless the process raises the resolution), so a loop that sleeps
// toward a 16.7 ms boundary wakes a frame late or polls its last millisecond.
// This waiter uses a high-resolution waitable timer where one exists, stops
// short of the deadline by a small spin window, and yields through that window,
// so a wake lands just after the boundary without burning a core between
// frames. Elsewhere it falls back to std::this_thread::sleep_until.
//
// The waiter owns an OS handle and is used by one thread.
class FrameWaiter final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // How far before the deadline the timer wait ends and yielding begins.
    static constexpr std::chrono::microseconds spin_window{500};

    FrameWaiter() noexcept;
    ~FrameWaiter();
    FrameWaiter(const FrameWaiter&) = delete;
    FrameWaiter& operator=(const FrameWaiter&) = delete;

    // Returns at `deadline` or after `ceiling`, whichever is sooner, so a
    // caller sleeping toward a distant boundary still observes an interrupt.
    // A deadline already past returns after one yield.
    void wait_until(TimePoint deadline, Clock::duration ceiling) noexcept;

    // Sleeps for about `duration` with the same timer and no spin: for a
    // fixed-rate poll that is not waiting on a boundary.
    void sleep_for(Clock::duration duration) noexcept;

    // True when waits use a high-resolution OS timer rather than the
    // portable fallback.
    [[nodiscard]] bool high_resolution() const noexcept {
        return high_resolution_;
    }

private:
    void* timer_ = nullptr;
    bool high_resolution_ = false;
};

// How late a frame loop observes the boundaries it waited for.
//
// arm() records the boundary a loop is about to wait for; observe() at the top
// of the next iteration measures how far past it the loop woke. A wake before
// the boundary (another timer, an early return) leaves it armed, and arming
// again replaces it, so each boundary is counted once, including one the loop
// overran without sleeping. take() returns the window and starts a new one.
class BoundaryLateness final {
public:
    using Clock = FrameWaiter::Clock;
    using TimePoint = FrameWaiter::TimePoint;

    // A wake this late is reported separately: an eighth of a 60 Hz frame.
    static constexpr std::chrono::microseconds late_threshold{2000};

    struct Window final {
        core::u64 boundaries = 0U;
        core::u64 late = 0U;
        std::chrono::microseconds max{0};
        std::chrono::microseconds mean{0};
    };

    void arm(TimePoint boundary) noexcept {
        armed_ = boundary;
        is_armed_ = true;
    }

    void observe(TimePoint now) noexcept;

    [[nodiscard]] Window take() noexcept;

private:
    TimePoint armed_{};
    bool is_armed_ = false;
    core::u64 boundaries_ = 0U;
    core::u64 late_ = 0U;
    Clock::duration max_{0};
    Clock::duration total_{0};
};

} // namespace mediaserver::show
