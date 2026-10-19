#include <mediaserver/show/frame_waiter.hpp>

#include <algorithm>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace mediaserver::show {

#if defined(_WIN32)

FrameWaiter::FrameWaiter() noexcept {
    // Windows 10 1803 and later. Without it a waitable timer still has
    // system-tick resolution, so the fallback below is no better than one.
    HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    if (timer != nullptr) {
        timer_ = timer;
        high_resolution_ = true;
    }
}

FrameWaiter::~FrameWaiter() {
    if (timer_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(timer_));
    }
}

void FrameWaiter::wait_until(TimePoint deadline,
                             Clock::duration ceiling) noexcept {
    const auto now = Clock::now();
    if (deadline <= now) {
        std::this_thread::yield();
        return;
    }
    const auto remaining = deadline - now;
    if (remaining > spin_window) {
        const auto sleep = std::min<Clock::duration>(remaining - spin_window,
                                                     ceiling);
        if (high_resolution_) {
            // Relative due time, in 100 ns units, negative by convention.
            LARGE_INTEGER due{};
            due.QuadPart = -std::max<LONGLONG>(
                1, static_cast<LONGLONG>(
                       std::chrono::duration_cast<std::chrono::nanoseconds>(
                           sleep)
                           .count()
                       / 100));
            if (SetWaitableTimerEx(static_cast<HANDLE>(timer_), &due, 0,
                                   nullptr, nullptr, nullptr, 0)
                && WaitForSingleObject(static_cast<HANDLE>(timer_), INFINITE)
                       == WAIT_OBJECT_0) {
                return;
            }
        }
        std::this_thread::sleep_for(sleep);
        return;
    }
    // Inside the spin window: yield rather than sleep, since any sleep here
    // rounds up to a whole system tick. The window is far below any ceiling.
    while (Clock::now() < deadline) {
        std::this_thread::yield();
    }
}

#else

FrameWaiter::FrameWaiter() noexcept = default;
FrameWaiter::~FrameWaiter() = default;

void FrameWaiter::wait_until(TimePoint deadline,
                             Clock::duration ceiling) noexcept {
    const auto now = Clock::now();
    if (deadline <= now) {
        std::this_thread::yield();
        return;
    }
    std::this_thread::sleep_until(std::min(deadline, now + ceiling));
}

#endif

void BoundaryLateness::observe(TimePoint now) noexcept {
    if (!is_armed_ || now < armed_) {
        return;
    }
    is_armed_ = false;
    const auto late = now - armed_;
    ++boundaries_;
    if (late > late_threshold) {
        ++late_;
    }
    max_ = std::max(max_, late);
    total_ += late;
}

BoundaryLateness::Window BoundaryLateness::take() noexcept {
    Window window;
    window.boundaries = boundaries_;
    window.late = late_;
    window.max = std::chrono::duration_cast<std::chrono::microseconds>(max_);
    if (boundaries_ != 0U) {
        window.mean = std::chrono::duration_cast<std::chrono::microseconds>(
            total_ / static_cast<Clock::rep>(boundaries_));
    }
    boundaries_ = 0U;
    late_ = 0U;
    max_ = Clock::duration{0};
    total_ = Clock::duration{0};
    return window;
}

} // namespace mediaserver::show
