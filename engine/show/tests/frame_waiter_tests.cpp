#include <mediaserver/show/frame_waiter.hpp>

#include "show_test_support.hpp"

#include <array>
#include <chrono>

namespace {

namespace show = mediaserver::show;

using Clock = show::FrameWaiter::Clock;
using TimePoint = show::FrameWaiter::TimePoint;
using namespace std::chrono_literals;

[[nodiscard]] TimePoint at(std::chrono::microseconds value) {
    return TimePoint{std::chrono::duration_cast<Clock::duration>(value)};
}

void lateness_counts_a_boundary_once_when_reached() {
    show::BoundaryLateness lateness;
    lateness.arm(at(10'000us));
    lateness.observe(at(4'000us)); // an editor wake before the boundary
    lateness.observe(at(10'300us));
    lateness.observe(at(10'900us)); // already counted
    lateness.observe(at(30'000us)); // nothing armed
    const auto window = lateness.take();
    SHOW_CHECK(window.boundaries == 1U);
    SHOW_CHECK(window.late == 0U);
    SHOW_CHECK(window.max == 300us);
    SHOW_CHECK(window.mean == 300us);
}

void lateness_reports_wakes_beyond_the_threshold() {
    show::BoundaryLateness lateness;
    // 2.5 ms is late; exactly the 2 ms threshold is not.
    const std::array<std::chrono::microseconds, 3U> lates{2'500us, 2'000us,
                                                          1'000us};
    auto boundary = 0us;
    for (const auto late : lates) {
        boundary += 16'683us;
        lateness.arm(at(boundary));
        lateness.observe(at(boundary + late));
    }
    const auto window = lateness.take();
    SHOW_CHECK(window.boundaries == 3U);
    SHOW_CHECK(window.late == 1U);
    SHOW_CHECK(window.max == 2'500us);
    SHOW_CHECK(window.mean == 1'833us);
}

void arming_again_replaces_the_pending_boundary() {
    show::BoundaryLateness lateness;
    lateness.arm(at(5'000us));
    lateness.arm(at(9'000us));
    lateness.observe(at(6'000us)); // past the replaced boundary only
    SHOW_CHECK(lateness.take().boundaries == 0U);
    lateness.observe(at(9'700us));
    const auto window = lateness.take();
    SHOW_CHECK(window.boundaries == 1U);
    SHOW_CHECK(window.max == 700us);
}

void take_starts_a_new_window() {
    show::BoundaryLateness lateness;
    lateness.arm(at(1'000us));
    lateness.observe(at(4'000us));
    SHOW_CHECK(lateness.take().late == 1U);
    const auto empty = lateness.take();
    SHOW_CHECK(empty.boundaries == 0U);
    SHOW_CHECK(empty.late == 0U);
    SHOW_CHECK(empty.max == 0us);
    SHOW_CHECK(empty.mean == 0us);
}

void a_past_deadline_returns_without_sleeping() {
    show::FrameWaiter waiter;
    const auto before = Clock::now();
    waiter.wait_until(before - 1ms, 10ms);
    SHOW_CHECK(Clock::now() - before < 50ms);
}

// Timing bounds are loose so a loaded test machine does not fail them; the
// call count is the property: the old loop polled its final millisecond
// hundreds of times per frame.
void a_frame_loop_wakes_at_the_boundary_in_few_calls() {
    show::FrameWaiter waiter;
    const auto deadline = Clock::now() + 8ms;
    int calls = 0;
    while (Clock::now() < deadline) {
        waiter.wait_until(deadline, 10ms);
        ++calls;
    }
    SHOW_CHECK(Clock::now() - deadline < 50ms);
    if (waiter.high_resolution()) {
        SHOW_CHECK(calls <= 4);
    }
}

void the_ceiling_bounds_a_distant_wait() {
    show::FrameWaiter waiter;
    const auto before = Clock::now();
    const auto deadline = before + 2s;
    waiter.wait_until(deadline, 5ms);
    const auto after = Clock::now();
    SHOW_CHECK(after < deadline);
    SHOW_CHECK(after - before < 500ms);
}

constexpr std::array cases{
    show::test::Case{"lateness counts a boundary once when reached",
                     lateness_counts_a_boundary_once_when_reached},
    show::test::Case{"lateness reports wakes beyond the threshold",
                     lateness_reports_wakes_beyond_the_threshold},
    show::test::Case{"arming again replaces the pending boundary",
                     arming_again_replaces_the_pending_boundary},
    show::test::Case{"take starts a new window", take_starts_a_new_window},
    show::test::Case{"a past deadline returns without sleeping",
                     a_past_deadline_returns_without_sleeping},
    show::test::Case{"a frame loop wakes at the boundary in few calls",
                     a_frame_loop_wakes_at_the_boundary_in_few_calls},
    show::test::Case{"the ceiling bounds a distant wait",
                     the_ceiling_bounds_a_distant_wait},
};

} // namespace

int main() {
    return mediaserver::show::test::run(cases);
}
