#include <mediaserver/show/show_clock.hpp>

#include <cstdint>
#include <limits>

namespace mediaserver::show {
namespace {

using Result = core::Result<ShowTick, ShowClockError>;

[[nodiscard]] constexpr core::u64 inclusive_span(
    ShowFrame first,
    ShowFrame last) noexcept {
    return last.value() - first.value() + 1U;
}

} // namespace

bool ShowClock::valid_configuration(const ShowClockConfig& config) noexcept {
    if (config.rate.numerator == 0U || config.rate.denominator == 0U) {
        return false;
    }
    if (config.rate.numerator > max_rate_numerator
        || config.rate.denominator > max_rate_denominator) {
        return false;
    }
    if (config.last_frame < config.first_frame) {
        return false;
    }
    // The span is reported as a count, so the inclusive range may not cover the
    // entire unsigned domain.
    return config.last_frame.value() - config.first_frame.value()
        != std::numeric_limits<core::u64>::max();
}

core::Result<ShowClock, ShowClockError> ShowClock::create(
    ShowClockConfig config,
    TimePoint created_at) noexcept {
    if (!valid_configuration(config)) {
        return core::Result<ShowClock, ShowClockError>::failure(
            ShowClockError::InvalidConfiguration);
    }
    return core::Result<ShowClock, ShowClockError>::success(
        ShowClock(config, created_at));
}

ShowTick ShowClock::current_tick(
    core::u64 advanced,
    bool wrapped,
    bool reached_end) const noexcept {
    return ShowTick{state_, frame_, advanced, wrapped, reached_end};
}

ShowTick ShowClock::reposition(core::u64 advanced) noexcept {
    if (advanced == 0U) {
        return current_tick(0U, false, false);
    }

    const auto remaining = config_.last_frame.value() - frame_.value();
    if (advanced <= remaining) {
        frame_ = ShowFrame{frame_.value() + advanced};
        return current_tick(advanced, false, false);
    }

    if (!config_.loop) {
        // Park on the final frame and hold. Only the boundaries that existed on
        // the timeline are reported as advanced.
        frame_ = config_.last_frame;
        state_ = TransportState::Paused;
        remainder_ = 0U;
        return current_tick(remaining, false, true);
    }

    // Frames consumed past the last frame, counted from the first frame.
    const auto overshoot = advanced - remaining - 1U;
    const auto span = inclusive_span(config_.first_frame, config_.last_frame);
    frame_ = ShowFrame{config_.first_frame.value() + (overshoot % span)};
    return current_tick(advanced, true, false);
}

Result ShowClock::observe(TimePoint now) noexcept {
    if (now < observed_at_) {
        return Result::failure(ShowClockError::TimeRegression);
    }

    if (state_ != TransportState::Playing) {
        observed_at_ = now;
        return Result::success(current_tick(0U, false, false));
    }

    // Bound the gap in the clock's own period before converting, so a wildly
    // stale observation cannot overflow the nanosecond conversion itself.
    constexpr auto elapsed_limit = std::chrono::duration_cast<Clock::duration>(
        std::chrono::nanoseconds{static_cast<std::int64_t>(
            max_elapsed_nanoseconds)});
    const auto delta = now - observed_at_;
    if (delta > elapsed_limit) {
        return Result::failure(ShowClockError::TimeOverflow);
    }

    const auto elapsed_nanoseconds = static_cast<core::u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(delta).count());

    // Exact integer accumulation: the sub-frame remainder is carried forward
    // rather than truncated, so no drift accrues at any rate or run length.
    // The configuration bounds guarantee this product cannot overflow.
    const auto scaled = elapsed_nanoseconds
        * static_cast<core::u64>(config_.rate.numerator);
    const auto accumulated = scaled + remainder_;
    const auto frame_period = period();

    const auto advanced = accumulated / frame_period;
    remainder_ = accumulated % frame_period;
    observed_at_ = now;

    return Result::success(reposition(advanced));
}

Result ShowClock::play(TimePoint at) noexcept {
    if (state_ == TransportState::Playing) {
        return Result::failure(ShowClockError::AlreadyPlaying);
    }
    if (at < observed_at_) {
        return Result::failure(ShowClockError::TimeRegression);
    }

    observed_at_ = at;
    remainder_ = 0U;
    state_ = TransportState::Playing;
    return Result::success(current_tick(0U, false, false));
}

Result ShowClock::reconfigure_rate(FrameRate rate, TimePoint at) noexcept {
    ShowClockConfig updated = config_;
    updated.rate = rate;
    if (!valid_configuration(updated)) {
        return Result::failure(ShowClockError::InvalidConfiguration);
    }
    if (at < observed_at_) {
        return Result::failure(ShowClockError::TimeRegression);
    }

    config_ = updated;
    observed_at_ = at;
    remainder_ = 0U;
    return Result::success(current_tick(0U, false, false));
}

Result ShowClock::pause(TimePoint at) noexcept {
    if (state_ != TransportState::Playing) {
        return Result::failure(ShowClockError::NotPlaying);
    }

    auto observed = observe(at);
    if (!observed) {
        return observed;
    }

    // Reaching a non-looping end already parked the transport.
    state_ = TransportState::Paused;
    remainder_ = 0U;

    auto tick = std::move(observed).value();
    tick.state = state_;
    return Result::success(tick);
}

Result ShowClock::stop(TimePoint at) noexcept {
    auto observed = observe(at);
    if (!observed) {
        return observed;
    }

    state_ = TransportState::Stopped;
    frame_ = config_.first_frame;
    remainder_ = 0U;

    auto tick = std::move(observed).value();
    tick.state = state_;
    tick.frame = frame_;
    return Result::success(tick);
}

Result ShowClock::seek(ShowFrame target, TimePoint at) noexcept {
    if (target < config_.first_frame || config_.last_frame < target) {
        return Result::failure(ShowClockError::FrameOutOfRange);
    }
    if (at < observed_at_) {
        return Result::failure(ShowClockError::TimeRegression);
    }

    frame_ = target;
    observed_at_ = at;
    remainder_ = 0U;
    return Result::success(current_tick(0U, false, false));
}

Result ShowClock::advance_to(TimePoint now) noexcept {
    return observe(now);
}

ShowClock::TimePoint ShowClock::next_boundary_at() const noexcept {
    if (state_ != TransportState::Playing) {
        return observed_at_;
    }

    const auto frame_period = period();
    const auto numerator = static_cast<core::u64>(config_.rate.numerator);
    const auto outstanding = frame_period - remainder_;
    const auto nanoseconds = (outstanding + numerator - 1U) / numerator;
    // Truncating into a coarser clock period can only report the boundary
    // early, which is the safe direction for a wait hint.
    return observed_at_
        + std::chrono::duration_cast<Clock::duration>(
               std::chrono::nanoseconds{static_cast<std::int64_t>(nanoseconds)});
}

core::u32 frames_per_second(FrameRate rate) noexcept {
    if (rate.numerator == 0U || rate.denominator == 0U) {
        return 0U;
    }
    const auto numerator = static_cast<core::u64>(rate.numerator);
    const auto denominator = static_cast<core::u64>(rate.denominator);
    // Round to nearest: 60000/1001 labels as 60, 24000/1001 as 24.
    const auto rounded = (numerator + (denominator / 2U)) / denominator;
    return static_cast<core::u32>(rounded == 0U ? 1U : rounded);
}

Timecode timecode(ShowFrame frame, FrameRate rate) noexcept {
    const auto per_second = static_cast<core::u64>(frames_per_second(rate));
    if (per_second == 0U) {
        return Timecode{};
    }

    const auto total = frame.value();
    const auto seconds_total = total / per_second;
    Timecode result{};
    result.frames = static_cast<core::u32>(total % per_second);
    result.seconds = static_cast<core::u32>(seconds_total % 60U);
    result.minutes = static_cast<core::u32>((seconds_total / 60U) % 60U);
    result.hours = static_cast<core::u32>((seconds_total / 3600U) % 24U);
    return result;
}

} // namespace mediaserver::show
