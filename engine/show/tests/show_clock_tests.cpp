#include <mediaserver/show/show_clock.hpp>

#include "show_test_support.hpp"

#include <array>
#include <chrono>
#include <utility>

namespace {

namespace show = mediaserver::show;

using Clock = show::ShowClock::Clock;
using TimePoint = show::ShowClock::TimePoint;
using namespace std::chrono_literals;

[[nodiscard]] TimePoint at(std::chrono::nanoseconds value) {
    return TimePoint{std::chrono::duration_cast<Clock::duration>(value)};
}

// 60000/1001 is the rate that exposes truncation bugs: one frame is
// 16 683 333 + 1/3 nanoseconds, so any implementation that stores a rounded
// period drifts.
constexpr show::FrameRate ntsc_60{60000U, 1001U};
constexpr show::FrameRate pal_50{50U, 1U};

[[nodiscard]] show::ShowClock make(
    show::FrameRate rate = pal_50,
    show::ShowFrame first = show::ShowFrame{0U},
    show::ShowFrame last = show::ShowFrame{999U},
    bool loop = false) {
    auto created = show::ShowClock::create(
        show::ShowClockConfig{rate, first, last, loop}, at(0ns));
    SHOW_CHECK(created.has_value());
    return std::move(created).value();
}

void invalid_configurations_are_refused() {
    const std::array<show::ShowClockConfig, 5U> rejected{
        show::ShowClockConfig{
            show::FrameRate{0U, 1U}, show::ShowFrame{0U}, show::ShowFrame{9U},
            false},
        show::ShowClockConfig{
            show::FrameRate{50U, 0U}, show::ShowFrame{0U}, show::ShowFrame{9U},
            false},
        show::ShowClockConfig{
            show::FrameRate{show::max_rate_numerator + 1U, 1U},
            show::ShowFrame{0U}, show::ShowFrame{9U}, false},
        show::ShowClockConfig{
            show::FrameRate{50U, show::max_rate_denominator + 1U},
            show::ShowFrame{0U}, show::ShowFrame{9U}, false},
        // last before first
        show::ShowClockConfig{
            pal_50, show::ShowFrame{10U}, show::ShowFrame{9U}, false},
    };

    for (const auto& config : rejected) {
        SHOW_CHECK(!show::ShowClock::valid_configuration(config));
        const auto created = show::ShowClock::create(config, at(0ns));
        SHOW_CHECK(!created.has_value());
        SHOW_CHECK(
            created.error() == show::ShowClockError::InvalidConfiguration);
    }
}

void a_new_clock_is_stopped_on_the_first_frame() {
    auto clock = make(pal_50, show::ShowFrame{100U}, show::ShowFrame{200U});
    SHOW_CHECK(clock.state() == show::TransportState::Stopped);
    SHOW_CHECK(clock.frame() == show::ShowFrame{100U});
    SHOW_CHECK(clock.frame_span() == 101U);

    // A stopped clock records time without advancing.
    const auto idle = clock.advance_to(at(5s));
    SHOW_CHECK(idle.has_value());
    SHOW_CHECK(idle.value().advanced == 0U);
    SHOW_CHECK(clock.frame() == show::ShowFrame{100U});
}

void playback_advances_exactly_one_frame_per_period() {
    auto clock = make();
    SHOW_CHECK(clock.play(at(0ns)).has_value());
    SHOW_CHECK(clock.state() == show::TransportState::Playing);

    // 50fps: one frame is exactly 20ms.
    for (int index = 1; index <= 10; ++index) {
        const auto tick = clock.advance_to(at(std::chrono::nanoseconds{
            static_cast<long long>(index) * 20'000'000LL}));
        SHOW_CHECK(tick.has_value());
        SHOW_CHECK(tick.value().advanced == 1U);
        SHOW_CHECK(
            tick.value().frame == show::ShowFrame{static_cast<unsigned>(index)});
    }
}

void a_partial_period_crosses_no_boundary() {
    auto clock = make();
    SHOW_CHECK(clock.play(at(0ns)).has_value());

    const auto early = clock.advance_to(at(19'999'999ns));
    SHOW_CHECK(early.has_value());
    SHOW_CHECK(early.value().advanced == 0U);
    SHOW_CHECK(clock.frame() == show::ShowFrame{0U});

    // The remaining nanosecond completes the frame: phase is carried, not lost.
    const auto complete = clock.advance_to(at(20'000'000ns));
    SHOW_CHECK(complete.has_value());
    SHOW_CHECK(complete.value().advanced == 1U);
    SHOW_CHECK(clock.frame() == show::ShowFrame{1U});
}

void fractional_rates_accumulate_without_drift() {
    auto clock = make(ntsc_60, show::ShowFrame{0U}, show::ShowFrame{1'000'000U});
    SHOW_CHECK(clock.play(at(0ns)).has_value());

    // Poll on a 1ms cadence that never lands on a frame boundary. After
    // exactly 1001 seconds a 60000/1001 show must be on frame 60000 - a
    // truncated period would be short by hundreds of frames.
    constexpr long long poll_step = 1'000'000LL;
    constexpr long long total = 1001LL * 1'000'000'000LL;
    mediaserver::core::u64 advanced_total = 0U;
    for (long long now = poll_step; now <= total; now += poll_step) {
        const auto tick = clock.advance_to(at(std::chrono::nanoseconds{now}));
        SHOW_CHECK(tick.has_value());
        advanced_total += tick.value().advanced;
    }

    SHOW_CHECK(advanced_total == 60'000U);
    SHOW_CHECK(clock.frame() == show::ShowFrame{60'000U});
}

void a_late_caller_is_told_how_many_boundaries_it_missed() {
    auto clock = make();
    SHOW_CHECK(clock.play(at(0ns)).has_value());

    // Seven frame periods in one observation.
    const auto tick = clock.advance_to(at(140'000'000ns));
    SHOW_CHECK(tick.has_value());
    SHOW_CHECK(tick.value().advanced == 7U);
    SHOW_CHECK(clock.frame() == show::ShowFrame{7U});
    SHOW_CHECK(!tick.value().wrapped);
}

void pause_reports_the_boundaries_it_absorbed_then_holds() {
    auto clock = make();
    SHOW_CHECK(clock.play(at(0ns)).has_value());

    const auto paused = clock.pause(at(60'000'000ns));
    SHOW_CHECK(paused.has_value());
    SHOW_CHECK(paused.value().advanced == 3U);
    SHOW_CHECK(paused.value().state == show::TransportState::Paused);
    SHOW_CHECK(clock.frame() == show::ShowFrame{3U});

    // Time passing while paused moves nothing.
    const auto held = clock.advance_to(at(5s));
    SHOW_CHECK(held.has_value());
    SHOW_CHECK(held.value().advanced == 0U);
    SHOW_CHECK(clock.frame() == show::ShowFrame{3U});

    SHOW_CHECK(!clock.pause(at(6s)).has_value());
    SHOW_CHECK(clock.pause(at(6s)).error() == show::ShowClockError::NotPlaying);
}

void resuming_restarts_sub_frame_phase() {
    auto clock = make();
    SHOW_CHECK(clock.play(at(0ns)).has_value());
    // Nineteen of twenty milliseconds consumed, then paused.
    SHOW_CHECK(clock.pause(at(19'000'000ns)).has_value());
    SHOW_CHECK(clock.subframe_remainder() == 0U);

    SHOW_CHECK(clock.play(at(19'000'000ns)).has_value());
    const auto tick = clock.advance_to(at(38'000'000ns));
    SHOW_CHECK(tick.has_value());
    // A full period is required after the resume, not the leftover 1ms.
    SHOW_CHECK(tick.value().advanced == 0U);
    SHOW_CHECK(clock.frame() == show::ShowFrame{0U});
}

void playing_twice_is_refused() {
    auto clock = make();
    SHOW_CHECK(clock.play(at(0ns)).has_value());
    const auto again = clock.play(at(1s));
    SHOW_CHECK(!again.has_value());
    SHOW_CHECK(again.error() == show::ShowClockError::AlreadyPlaying);
}

void an_exact_seek_repositions_and_reanchors() {
    auto clock = make();
    SHOW_CHECK(clock.play(at(0ns)).has_value());
    SHOW_CHECK(clock.advance_to(at(100'000'000ns)).has_value());
    SHOW_CHECK(clock.frame() == show::ShowFrame{5U});

    const auto sought = clock.seek(show::ShowFrame{742U}, at(100'000'000ns));
    SHOW_CHECK(sought.has_value());
    SHOW_CHECK(sought.value().frame == show::ShowFrame{742U});
    SHOW_CHECK(sought.value().advanced == 0U);
    SHOW_CHECK(clock.state() == show::TransportState::Playing);

    // Playback continues from the sought frame with a fresh phase.
    const auto next = clock.advance_to(at(120'000'000ns));
    SHOW_CHECK(next.has_value());
    SHOW_CHECK(next.value().advanced == 1U);
    SHOW_CHECK(clock.frame() == show::ShowFrame{743U});
}

void a_seek_outside_the_show_range_is_refused() {
    auto clock = make(pal_50, show::ShowFrame{10U}, show::ShowFrame{20U});
    for (const auto target : {show::ShowFrame{9U}, show::ShowFrame{21U}}) {
        const auto sought = clock.seek(target, at(0ns));
        SHOW_CHECK(!sought.has_value());
        SHOW_CHECK(sought.error() == show::ShowClockError::FrameOutOfRange);
    }
    SHOW_CHECK(clock.frame() == show::ShowFrame{10U});
    SHOW_CHECK(clock.seek(show::ShowFrame{20U}, at(0ns)).has_value());
}

void a_non_looping_show_parks_on_its_last_frame() {
    auto clock = make(pal_50, show::ShowFrame{0U}, show::ShowFrame{4U});
    SHOW_CHECK(clock.play(at(0ns)).has_value());

    const auto tick = clock.advance_to(at(200'000'000ns)); // ten frames
    SHOW_CHECK(tick.has_value());
    SHOW_CHECK(tick.value().reached_end);
    // Only the boundaries that exist on the timeline are reported.
    SHOW_CHECK(tick.value().advanced == 4U);
    SHOW_CHECK(tick.value().state == show::TransportState::Paused);
    SHOW_CHECK(clock.frame() == show::ShowFrame{4U});

    const auto after = clock.advance_to(at(400'000'000ns));
    SHOW_CHECK(after.has_value());
    SHOW_CHECK(after.value().advanced == 0U);
    SHOW_CHECK(clock.frame() == show::ShowFrame{4U});
}

void a_looping_show_wraps_to_its_first_frame() {
    auto clock = make(pal_50, show::ShowFrame{10U}, show::ShowFrame{14U}, true);
    SHOW_CHECK(clock.play(at(0ns)).has_value());

    // Five frames from frame 10 lands one past the last frame, which is the
    // first frame again.
    const auto tick = clock.advance_to(at(100'000'000ns));
    SHOW_CHECK(tick.has_value());
    SHOW_CHECK(tick.value().wrapped);
    SHOW_CHECK(tick.value().advanced == 5U);
    SHOW_CHECK(clock.frame() == show::ShowFrame{10U});
    SHOW_CHECK(clock.state() == show::TransportState::Playing);

    // A jump of several whole spans still lands deterministically.
    const auto far = clock.advance_to(at(100'000'000ns + 260'000'000ns));
    SHOW_CHECK(far.has_value());
    SHOW_CHECK(far.value().advanced == 13U);
    SHOW_CHECK(far.value().wrapped);
    SHOW_CHECK(clock.frame() == show::ShowFrame{13U});
}

void stop_returns_to_the_first_frame() {
    auto clock = make(pal_50, show::ShowFrame{7U}, show::ShowFrame{99U});
    SHOW_CHECK(clock.play(at(0ns)).has_value());
    SHOW_CHECK(clock.advance_to(at(100'000'000ns)).has_value());
    SHOW_CHECK(clock.frame() == show::ShowFrame{12U});

    const auto stopped = clock.stop(at(100'000'000ns));
    SHOW_CHECK(stopped.has_value());
    SHOW_CHECK(stopped.value().state == show::TransportState::Stopped);
    SHOW_CHECK(stopped.value().frame == show::ShowFrame{7U});
    SHOW_CHECK(clock.frame() == show::ShowFrame{7U});
}

void time_regression_and_overflow_are_refused() {
    auto clock = make();
    SHOW_CHECK(clock.play(at(10s)).has_value());
    SHOW_CHECK(clock.advance_to(at(11s)).has_value());

    const auto backwards = clock.advance_to(at(10s));
    SHOW_CHECK(!backwards.has_value());
    SHOW_CHECK(backwards.error() == show::ShowClockError::TimeRegression);
    // The refusal changed nothing.
    SHOW_CHECK(clock.frame() == show::ShowFrame{50U});

    const auto stale = clock.advance_to(at(11s + 3601s));
    SHOW_CHECK(!stale.has_value());
    SHOW_CHECK(stale.error() == show::ShowClockError::TimeOverflow);
    SHOW_CHECK(clock.frame() == show::ShowFrame{50U});

    // Re-anchoring with a seek recovers.
    SHOW_CHECK(clock.seek(show::ShowFrame{50U}, at(11s + 3601s)).has_value());
    SHOW_CHECK(clock.advance_to(at(11s + 3601s + 20'000'000ns)).has_value());
    SHOW_CHECK(clock.frame() == show::ShowFrame{51U});
}

void the_next_boundary_is_reported_for_waiting() {
    auto clock = make();
    SHOW_CHECK(clock.play(at(0ns)).has_value());
    SHOW_CHECK(clock.next_boundary_at() == at(20'000'000ns));

    SHOW_CHECK(clock.advance_to(at(5'000'000ns)).has_value());
    SHOW_CHECK(clock.next_boundary_at() == at(20'000'000ns));

    // While not playing there is no future boundary to wait for.
    SHOW_CHECK(clock.pause(at(5'000'000ns)).has_value());
    SHOW_CHECK(clock.next_boundary_at() == at(5'000'000ns));
}

void changing_rate_reanchors_without_moving_the_show() {
    auto clock = make();
    SHOW_CHECK(clock.play(at(0ns)).has_value());
    SHOW_CHECK(clock.advance_to(at(20'000'000ns)).has_value());
    SHOW_CHECK(clock.frame() == show::ShowFrame{1U});

    constexpr show::FrameRate film_23976{24'000U, 1'001U};
    const auto changed =
        clock.reconfigure_rate(film_23976, at(25'000'000ns));
    SHOW_CHECK(changed.has_value());
    SHOW_CHECK(changed.value().frame == show::ShowFrame{1U});
    SHOW_CHECK(changed.value().state == show::TransportState::Playing);
    SHOW_CHECK(clock.config().rate == film_23976);
    SHOW_CHECK(clock.subframe_remainder() == 0U);
    SHOW_CHECK(clock.next_boundary_at() == at(66'708'334ns));

    const auto early = clock.advance_to(at(66'708'333ns));
    SHOW_CHECK(early.has_value());
    SHOW_CHECK(early.value().advanced == 0U);
    const auto boundary = clock.advance_to(at(66'708'334ns));
    SHOW_CHECK(boundary.has_value());
    SHOW_CHECK(boundary.value().advanced == 1U);
    SHOW_CHECK(boundary.value().frame == show::ShowFrame{2U});

    const auto before = clock.config();
    SHOW_CHECK(!clock.reconfigure_rate(show::FrameRate{0U, 1U},
                                       at(70'000'000ns)));
    SHOW_CHECK(clock.config() == before);
    SHOW_CHECK(!clock.reconfigure_rate(show::FrameRate{25U, 1U},
                                       at(60'000'000ns)));
    SHOW_CHECK(clock.config() == before);
}

void timecode_follows_the_labelled_rate() {
    SHOW_CHECK(show::frames_per_second(ntsc_60) == 60U);
    SHOW_CHECK(show::frames_per_second(show::FrameRate{24000U, 1001U}) == 24U);
    SHOW_CHECK(show::frames_per_second(pal_50) == 50U);

    const auto code = show::timecode(show::ShowFrame{3661U * 50U + 7U}, pal_50);
    SHOW_CHECK(code.hours == 1U);
    SHOW_CHECK(code.minutes == 1U);
    SHOW_CHECK(code.seconds == 1U);
    SHOW_CHECK(code.frames == 7U);

    const auto zero = show::timecode(show::ShowFrame{0U}, ntsc_60);
    SHOW_CHECK(zero == mediaserver::show::Timecode{});
}

constexpr std::array<mediaserver::show::test::Case, 18U> cases{{
    {"invalid configurations", &invalid_configurations_are_refused},
    {"new clock is stopped", &a_new_clock_is_stopped_on_the_first_frame},
    {"one frame per period", &playback_advances_exactly_one_frame_per_period},
    {"partial period holds", &a_partial_period_crosses_no_boundary},
    {"fractional rate has no drift", &fractional_rates_accumulate_without_drift},
    {"late caller sees the gap",
     &a_late_caller_is_told_how_many_boundaries_it_missed},
    {"pause absorbs then holds",
     &pause_reports_the_boundaries_it_absorbed_then_holds},
    {"resume restarts phase", &resuming_restarts_sub_frame_phase},
    {"double play refused", &playing_twice_is_refused},
    {"exact seek", &an_exact_seek_repositions_and_reanchors},
    {"seek out of range", &a_seek_outside_the_show_range_is_refused},
    {"non-looping end", &a_non_looping_show_parks_on_its_last_frame},
    {"looping wrap", &a_looping_show_wraps_to_its_first_frame},
    {"stop rewinds", &stop_returns_to_the_first_frame},
    {"regression and overflow", &time_regression_and_overflow_are_refused},
    {"next boundary", &the_next_boundary_is_reported_for_waiting},
    {"rate changes re-anchor", &changing_rate_reanchors_without_moving_the_show},
    {"timecode", &timecode_follows_the_labelled_rate},
}};

} // namespace

int main() {
    return mediaserver::show::test::run(cases);
}
