#pragma once

#include <algorithm>
#include <chrono>

#include <mediaserver/core/types.hpp>
#include <mediaserver/show/show_clock.hpp>

namespace mediaserver::show {

enum class VideoReferenceState : core::u8 {
    Unknown,
    Unsupported,
    Waiting,
    Locked,
    Lost,
    Failed,
};

// One reading of a hardware frame counter. `Locked` here means only that the
// source answered with a count; whether the count is moving is decided by
// VideoReferenceCadence, which sees more than one reading.
struct VideoReferenceSample final {
    VideoReferenceState state{VideoReferenceState::Unknown};
    core::u32 frame_counter{};

    bool operator==(const VideoReferenceSample&) const = default;
};

// What one reading is worth to a pacer.
struct VideoReferenceStep final {
    // Frames the reference counted since the previous reading. Always zero
    // when `referenced` is false.
    core::u64 boundaries{};
    // The state to report. A source that answers Locked while its counter
    // stands still is reported Lost: it is not supplying frames.
    VideoReferenceState state{VideoReferenceState::Unknown};
    // The reference is supplying this pacer's frames. When false, nothing may
    // be paced from it: the caller keeps its control work going on the system
    // clock and does not put a new picture on its outputs.
    bool referenced{};
    // The counter moved further than the time since the last reading can
    // account for - it was reset, or it now belongs to another source. The
    // move is taken as a new starting point, never as that many frames.
    bool discontinuity{};

    bool operator==(const VideoReferenceStep&) const = default;
};

// How long a counter that answers Locked may stand still before it is treated
// as not supplying frames. Four frame periods, and never under a quarter of a
// second, so a caller that polls faster than the frame rate does not mistake
// the gap between two frames for a stall.
[[nodiscard]] constexpr std::chrono::nanoseconds video_reference_stall_limit(
    FrameRate rate) noexcept {
    constexpr core::u64 floor_nanoseconds = 250'000'000ULL;
    const core::u64 four_periods = rate.numerator == 0U
        ? 0U
        : 4U * 1'000'000'000ULL * static_cast<core::u64>(rate.denominator)
              / static_cast<core::u64>(rate.numerator);
    return std::chrono::nanoseconds{static_cast<std::chrono::nanoseconds::rep>(
        std::max(floor_nanoseconds, four_periods))};
}

// The most frames a counter can credibly have counted in `elapsed`: twice
// what the rate predicts, plus a second's worth. Generous on purpose - a late
// caller must still be credited every frame it missed - and still far under
// the billions of frames a reset counter reads as.
[[nodiscard]] constexpr core::u64 video_reference_plausible_frames(
    FrameRate rate,
    std::chrono::nanoseconds elapsed) noexcept {
    if (rate.numerator == 0U || rate.denominator == 0U) {
        return 0U;
    }
    const core::u64 nanoseconds = elapsed.count() <= 0
        ? 0U
        : std::min(static_cast<core::u64>(elapsed.count()),
                   max_elapsed_nanoseconds);
    const core::u64 numerator = rate.numerator;
    const core::u64 denominator = rate.denominator;
    // Bounded like ShowClock's own conversion: an hour of nanoseconds times
    // the largest numerator still fits.
    const core::u64 expected =
        nanoseconds * numerator / (denominator * 1'000'000'000ULL);
    const core::u64 one_second = (numerator + denominator - 1U) / denominator;
    return 2U * expected + one_second + 1U;
}

// Turns successive readings of a hardware frame counter into frame
// boundaries, and decides whether the counter is fit to pace anything.
//
// The Director and every node read the same counter through this one rule,
// so they agree on what a stall or a reset is worth. The 32-bit counter is
// differenced modulo 2^32, matching the hardware API.
class VideoReferenceCadence final {
public:
    using TimePoint = ShowClock::TimePoint;

    [[nodiscard]] constexpr VideoReferenceStep observe(
        VideoReferenceSample sample,
        TimePoint now,
        FrameRate rate) noexcept {
        if (sample.state != VideoReferenceState::Locked) {
            reset();
            return VideoReferenceStep{0U, sample.state, false, false};
        }
        if (!seen_) {
            // The first reading only establishes where counting starts.
            seen_ = true;
            stalled_ = false;
            last_frame_ = sample.frame_counter;
            moved_at_ = now;
            return VideoReferenceStep{
                0U, VideoReferenceState::Locked, true, false};
        }

        const core::u32 moved = sample.frame_counter - last_frame_;
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                now > moved_at_ ? now - moved_at_ : TimePoint::duration{});
        if (moved == 0U) {
            if (stalled_ || elapsed > video_reference_stall_limit(rate)) {
                stalled_ = true;
                return VideoReferenceStep{
                    0U, VideoReferenceState::Lost, false, false};
            }
            return VideoReferenceStep{
                0U, VideoReferenceState::Locked, true, false};
        }

        const bool resumed = stalled_;
        const bool jumped =
            moved > video_reference_plausible_frames(rate, elapsed);
        stalled_ = false;
        last_frame_ = sample.frame_counter;
        moved_at_ = now;
        if (resumed || jumped) {
            // Counting starts again from here. The frames a stalled or reset
            // counter appears to have skipped were never frames.
            return VideoReferenceStep{
                0U, VideoReferenceState::Locked, true, jumped};
        }
        return VideoReferenceStep{
            moved, VideoReferenceState::Locked, true, false};
    }

    // Forgets where counting started. Called when something other than the
    // reference repositions the pacer, so the next reading starts afresh.
    constexpr void reset() noexcept {
        seen_ = false;
        stalled_ = false;
    }

private:
    bool seen_{};
    bool stalled_{};
    core::u32 last_frame_{};
    TimePoint moved_at_{};
};

} // namespace mediaserver::show
