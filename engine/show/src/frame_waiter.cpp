#include <mediaserver/show/frame_waiter.hpp>

#include <algorithm>
#include <thread>
#include <utility>

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
    // Auto-reset, so one notification ends one wait.
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

FrameWaiter::~FrameWaiter() {
    if (timer_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(timer_));
    }
    if (wake_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(wake_));
    }
}

namespace {

// A relative due time for SetWaitableTimerEx: 100 ns units, negative by
// convention, and never zero.
[[nodiscard]] LARGE_INTEGER
relative_due(FrameWaiter::Clock::duration duration) noexcept {
    LARGE_INTEGER due{};
    due.QuadPart = -std::max<LONGLONG>(
        1, static_cast<LONGLONG>(
               std::chrono::duration_cast<std::chrono::nanoseconds>(duration)
                   .count()
               / 100));
    return due;
}

} // namespace

void FrameWaiter::notify() noexcept {
    if (wake_ != nullptr) {
        SetEvent(static_cast<HANDLE>(wake_));
        return;
    }
    {
        const std::lock_guard lock{wake_mutex_};
        wake_pending_ = true;
    }
    wake_changed_.notify_one();
}

bool FrameWaiter::take_notification() noexcept {
    if (wake_ != nullptr) {
        return WaitForSingleObject(static_cast<HANDLE>(wake_), 0)
               == WAIT_OBJECT_0;
    }
    const std::lock_guard lock{wake_mutex_};
    return std::exchange(wake_pending_, false);
}

bool FrameWaiter::wait_until_notified(TimePoint deadline,
                                      Clock::duration ceiling) noexcept {
    if (take_notification()) {
        return true;
    }
    const auto now = Clock::now();
    if (deadline <= now) {
        std::this_thread::yield();
        return take_notification();
    }
    const auto remaining = deadline - now;
    if (remaining <= spin_window) {
        // As wait_until: too close to the deadline for any sleep.
        while (Clock::now() < deadline) {
            if (take_notification()) {
                return true;
            }
            std::this_thread::yield();
        }
        return take_notification();
    }
    const auto sleep =
        std::min<Clock::duration>(remaining - spin_window, ceiling);
    if (wake_ == nullptr) {
        std::unique_lock lock{wake_mutex_};
        wake_changed_.wait_for(lock, sleep, [this] { return wake_pending_; });
        return std::exchange(wake_pending_, false);
    }
    if (high_resolution_) {
        const LARGE_INTEGER due = relative_due(sleep);
        if (SetWaitableTimerEx(static_cast<HANDLE>(timer_), &due, 0, nullptr,
                               nullptr, nullptr, 0)) {
            const HANDLE handles[2]{static_cast<HANDLE>(wake_),
                                    static_cast<HANDLE>(timer_)};
            const DWORD ended =
                WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (ended == WAIT_OBJECT_0) {
                CancelWaitableTimer(static_cast<HANDLE>(timer_));
                return true;
            }
            if (ended == WAIT_OBJECT_0 + 1) {
                return false;
            }
        }
    }
    // No high-resolution timer: the event's own timeout, in system ticks.
    const auto milliseconds = std::max<LONGLONG>(
        1, std::chrono::duration_cast<std::chrono::milliseconds>(sleep)
               .count());
    return WaitForSingleObject(static_cast<HANDLE>(wake_),
                               static_cast<DWORD>(milliseconds))
           == WAIT_OBJECT_0;
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
        sleep_for(std::min<Clock::duration>(remaining - spin_window, ceiling));
        return;
    }
    // Inside the spin window: yield rather than sleep, since any sleep here
    // rounds up to a whole system tick. The window is far below any ceiling.
    while (Clock::now() < deadline) {
        std::this_thread::yield();
    }
}

void FrameWaiter::sleep_for(Clock::duration duration) noexcept {
    if (high_resolution_) {
        const LARGE_INTEGER due = relative_due(duration);
        if (SetWaitableTimerEx(static_cast<HANDLE>(timer_), &due, 0, nullptr,
                               nullptr, nullptr, 0)
            && WaitForSingleObject(static_cast<HANDLE>(timer_), INFINITE)
                   == WAIT_OBJECT_0) {
            return;
        }
    }
    std::this_thread::sleep_for(duration);
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

void FrameWaiter::sleep_for(Clock::duration duration) noexcept {
    std::this_thread::sleep_for(duration);
}

void FrameWaiter::notify() noexcept {
    {
        const std::lock_guard lock{wake_mutex_};
        wake_pending_ = true;
    }
    wake_changed_.notify_one();
}

bool FrameWaiter::take_notification() noexcept {
    const std::lock_guard lock{wake_mutex_};
    return std::exchange(wake_pending_, false);
}

bool FrameWaiter::wait_until_notified(TimePoint deadline,
                                      Clock::duration ceiling) noexcept {
    std::unique_lock lock{wake_mutex_};
    const auto now = Clock::now();
    if (deadline > now) {
        wake_changed_.wait_until(lock, std::min(deadline, now + ceiling),
                                 [this] { return wake_pending_; });
    }
    return std::exchange(wake_pending_, false);
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
