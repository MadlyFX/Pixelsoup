#include <mediaserver/show/video_reference.hpp>

#include "show_test_support.hpp"

#include <array>
#include <chrono>

namespace {

namespace show = mediaserver::show;

using Clock = show::ShowClock::Clock;
using TimePoint = show::ShowClock::TimePoint;
using namespace std::chrono_literals;

constexpr show::FrameRate ntsc_60{60'000U, 1'001U};
constexpr auto locked = show::VideoReferenceState::Locked;

[[nodiscard]] TimePoint at(std::chrono::nanoseconds value) {
    return TimePoint{std::chrono::duration_cast<Clock::duration>(value)};
}

void a_source_that_is_not_locked_supplies_nothing() {
    show::VideoReferenceCadence cadence;
    const show::VideoReferenceState absent[] = {
        show::VideoReferenceState::Unknown,
        show::VideoReferenceState::Unsupported,
        show::VideoReferenceState::Waiting,
        show::VideoReferenceState::Lost,
        show::VideoReferenceState::Failed,
    };
    auto now = 0ms;
    for (const show::VideoReferenceState state : absent) {
        now += 17ms;
        // Whatever number rides along with a source that is not locked is
        // not a count of anything.
        const auto step = cadence.observe({state, 812U}, at(now), ntsc_60);
        SHOW_CHECK(!step.referenced);
        SHOW_CHECK(step.boundaries == 0U);
        SHOW_CHECK(step.state == state);
        SHOW_CHECK(!step.discontinuity);
    }

    // And it leaves no starting point behind: the first locked reading after
    // it begins the count, however far its number is from the last one seen.
    const auto epoch =
        cadence.observe({locked, 90'000U}, at(now + 17ms), ntsc_60);
    SHOW_CHECK(epoch.referenced);
    SHOW_CHECK(epoch.boundaries == 0U);
    SHOW_CHECK(!epoch.discontinuity);
    const auto counted =
        cadence.observe({locked, 90'003U}, at(now + 67ms), ntsc_60);
    SHOW_CHECK(counted.referenced);
    SHOW_CHECK(counted.boundaries == 3U);
}

void the_stall_limit_follows_the_frame_rate() {
    // Four periods at 59.94 fps is 66.7 ms, under the quarter-second floor.
    SHOW_CHECK(show::video_reference_stall_limit(ntsc_60) == 250ms);
    // At two frames a second a period is half a second, so the gap between
    // two frames must not read as a stall: four periods is two seconds.
    constexpr show::FrameRate slow{2U, 1U};
    SHOW_CHECK(show::video_reference_stall_limit(slow) == 2s);

    show::VideoReferenceCadence cadence;
    SHOW_CHECK(cadence.observe({locked, 10U}, at(0ms), slow).referenced);
    const auto waiting = cadence.observe({locked, 10U}, at(2s), slow);
    SHOW_CHECK(waiting.referenced);
    SHOW_CHECK(waiting.state == locked);
    const auto stalled = cadence.observe({locked, 10U}, at(2'001ms), slow);
    SHOW_CHECK(!stalled.referenced);
    SHOW_CHECK(stalled.state == show::VideoReferenceState::Lost);
    SHOW_CHECK(stalled.boundaries == 0U);
    // It stays stalled for as long as the count stands still.
    SHOW_CHECK(!cadence.observe({locked, 10U}, at(2'002ms), slow).referenced);
}

void a_plausible_count_is_bounded_by_elapsed_time() {
    // Nothing elapsed still allows a second's worth: 59.94 rounds up to 60.
    SHOW_CHECK(show::video_reference_plausible_frames(ntsc_60, 0ns) == 61U);
    // Ten seconds at 59.94 fps is 599 whole frames; twice that plus a second.
    SHOW_CHECK(show::video_reference_plausible_frames(ntsc_60, 10s) == 1'259U);
    // A time that ran backwards explains no frames beyond the allowance.
    SHOW_CHECK(show::video_reference_plausible_frames(ntsc_60, -5s) == 61U);

    show::VideoReferenceCadence cadence;
    SHOW_CHECK(cadence.observe({locked, 400U}, at(0ms), ntsc_60).referenced);
    // Exactly at the bound is believed; one past it is a discontinuity.
    const auto most = cadence.observe({locked, 400U + 1'259U}, at(10s), ntsc_60);
    SHOW_CHECK(most.boundaries == 1'259U);
    SHOW_CHECK(!most.discontinuity);
    const auto beyond = cadence.observe(
        {locked, 400U + 1'259U + 1'260U}, at(20s), ntsc_60);
    SHOW_CHECK(beyond.referenced);
    SHOW_CHECK(beyond.boundaries == 0U);
    SHOW_CHECK(beyond.discontinuity);
    // A count that steps backwards is the same thing seen modulo 2^32.
    const auto backwards = cadence.observe({locked, 7U}, at(20'017ms), ntsc_60);
    SHOW_CHECK(backwards.boundaries == 0U);
    SHOW_CHECK(backwards.discontinuity);
    const auto onward = cadence.observe({locked, 8U}, at(20'034ms), ntsc_60);
    SHOW_CHECK(onward.boundaries == 1U);
    SHOW_CHECK(!onward.discontinuity);
}

void reset_forgets_the_starting_point() {
    show::VideoReferenceCadence cadence;
    SHOW_CHECK(cadence.observe({locked, 50U}, at(0ms), ntsc_60).referenced);
    cadence.reset();
    // Without the reset this would be five frames.
    const auto again = cadence.observe({locked, 55U}, at(84ms), ntsc_60);
    SHOW_CHECK(again.referenced);
    SHOW_CHECK(again.boundaries == 0U);
    SHOW_CHECK(cadence.observe({locked, 57U}, at(118ms), ntsc_60).boundaries
               == 2U);
}

constexpr std::array<mediaserver::show::test::Case, 4U> cases{{
    {"not locked supplies nothing",
     &a_source_that_is_not_locked_supplies_nothing},
    {"stall limit follows the rate", &the_stall_limit_follows_the_frame_rate},
    {"plausible count is bounded", &a_plausible_count_is_bounded_by_elapsed_time},
    {"reset forgets the start", &reset_forgets_the_starting_point},
}};

} // namespace

int main() {
    return mediaserver::show::test::run(cases);
}
