#pragma once

#include <chrono>
#include <limits>

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/strong_id.hpp>
#include <mediaserver/core/types.hpp>

namespace mediaserver::show {

// Position on the show timeline. This is deliberately not cmd::FrameNumber:
// the control plane's frame ordinal is a monotonic sealed-batch counter, while
// a show position is seekable, loopable, and may repeat.
using ShowFrame = core::StrongId<struct ShowFrameTag, core::u64>;

// Exact rational frame rate. 60000/1001 and 24/1 are both represented without
// rounding; the clock never converts a rate to a floating-point period.
struct FrameRate final {
    core::u32 numerator{};
    core::u32 denominator{};

    bool operator==(const FrameRate&) const = default;
};

inline constexpr core::u32 max_rate_numerator = 1'000'000U;
inline constexpr core::u32 max_rate_denominator = 1'000'000U;

// One evaluation may not span more than an hour of wall clock. Beyond that the
// caller has stalled so badly that silently synthesising the elapsed frames is
// worse than refusing and forcing an explicit re-anchor.
inline constexpr core::u64 max_elapsed_nanoseconds = 3'600'000'000'000ULL;

enum class TransportState : core::u8 {
    Stopped,
    Paused,
    Playing,
};

struct ShowClockConfig final {
    FrameRate rate{};
    ShowFrame first_frame{};
    ShowFrame last_frame{}; // inclusive
    bool loop{};

    bool operator==(const ShowClockConfig&) const = default;
};

// The configuration of a pacer: a clock used only as a metronome that says
// when the next frame boundary is due.
//
// A transport has a range and, with looping off, parks on its last frame. A
// pacer must never do that, because everything a frame loop does is gated on
// the boundaries it reports: a parked pacer reports none, for ever. So a pacer
// never takes its range from the timeline. This range is long enough that no
// run reaches its end (2^64 - 1 frames), which makes the loop flag irrelevant.
[[nodiscard]] constexpr ShowClockConfig pacer_clock_config(
    FrameRate rate) noexcept {
    return ShowClockConfig{
        rate,
        ShowFrame{0U},
        ShowFrame{std::numeric_limits<core::u64>::max() - 1U},
        false};
}

enum class ShowClockError : core::u8 {
    InvalidConfiguration,
    FrameOutOfRange,
    TimeRegression,
    TimeOverflow,
    AlreadyPlaying,
    NotPlaying,
};

// The result of every position-changing operation. `advanced` counts frame
// boundaries actually crossed on the timeline, so a caller that fell behind
// sees the true count and decides its own catch-up policy; the clock never
// hides elapsed time.
struct ShowTick final {
    TransportState state{TransportState::Stopped};
    ShowFrame frame{};
    core::u64 advanced{};
    bool wrapped{};
    bool reached_end{};

    bool operator==(const ShowTick&) const = default;
};

// A deterministic transport clock. Like net::HeartbeatSchedule it never reads a
// clock itself: every time point is supplied by the caller, so a test drives
// exact wall-clock scenarios and two processes can be stepped identically.
//
// Sub-frame phase is carried as an exact integer remainder rather than a
// truncated period, so a 60000/1001 show accumulates no drift over any run
// length. Every operation is noexcept and allocation-free, which is what makes
// it legal to call on the frame boundary.
class ShowClock final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    [[nodiscard]] static core::Result<ShowClock, ShowClockError> create(
        ShowClockConfig config,
        TimePoint created_at) noexcept;

    [[nodiscard]] static bool valid_configuration(
        const ShowClockConfig& config) noexcept;

    // Anchors playback at `at`. Sub-frame phase restarts, so resuming never
    // inherits a partial frame from a previous run.
    [[nodiscard]] core::Result<ShowTick, ShowClockError> play(
        TimePoint at) noexcept;

    // Advances to `at` first, so the boundaries crossed before the pause are
    // reported and can still be committed, then holds the position.
    [[nodiscard]] core::Result<ShowTick, ShowClockError> pause(
        TimePoint at) noexcept;

    // Advances to `at`, then parks on the first frame.
    [[nodiscard]] core::Result<ShowTick, ShowClockError> stop(
        TimePoint at) noexcept;

    // Exact frame seek, valid in any transport state. Sub-frame phase restarts
    // and playback re-anchors at `at`, so a seek during playback does not
    // inherit the phase of the frame it left.
    [[nodiscard]] core::Result<ShowTick, ShowClockError> seek(
        ShowFrame target,
        TimePoint at) noexcept;

    // Changes only the cadence, preserving the current frame and transport
    // state. A rate edit is itself a new timing anchor: elapsed time and any
    // partial old-rate frame since the last observation are discarded, so the
    // first boundary at the new rate is one complete new-rate period after
    // `at`. This is the safe operation for a replicated project setting that
    // takes effect at an exact control-frame boundary.
    [[nodiscard]] core::Result<ShowTick, ShowClockError> reconfigure_rate(
        FrameRate rate,
        TimePoint at) noexcept;

    // The frame-boundary evaluation. While paused or stopped this only records
    // the observed time and reports zero advancement.
    [[nodiscard]] core::Result<ShowTick, ShowClockError> advance_to(
        TimePoint now) noexcept;

    // Advances by a count observed from an external frame reference (for
    // example a Quadro Sync frame counter). `observed_at` is recorded only as
    // an administrative anchor for later transport commands; elapsed wall
    // time never contributes frames through this path.
    [[nodiscard]] core::Result<ShowTick, ShowClockError> advance_by(
        core::u64 boundaries,
        TimePoint observed_at) noexcept;

    // Time point of the next boundary while playing; `observed_at()` while not.
    [[nodiscard]] TimePoint next_boundary_at() const noexcept;

    [[nodiscard]] const ShowClockConfig& config() const noexcept {
        return config_;
    }
    [[nodiscard]] TransportState state() const noexcept { return state_; }
    [[nodiscard]] ShowFrame frame() const noexcept { return frame_; }
    [[nodiscard]] TimePoint observed_at() const noexcept { return observed_at_; }
    [[nodiscard]] core::u64 subframe_remainder() const noexcept {
        return remainder_;
    }
    [[nodiscard]] core::u64 frame_span() const noexcept {
        return config_.last_frame.value() - config_.first_frame.value() + 1U;
    }

private:
    ShowClock(ShowClockConfig config, TimePoint created_at) noexcept
        : config_(config),
          frame_(config.first_frame),
          observed_at_(created_at) {}

    // Numerator-domain duration of one frame. Every validated configuration
    // has a non-zero denominator, so this cannot be a division by zero.
    [[nodiscard]] core::u64 period() const noexcept {
        return static_cast<core::u64>(config_.rate.denominator)
            * 1'000'000'000ULL;
    }

    [[nodiscard]] core::Result<ShowTick, ShowClockError> observe(
        TimePoint now) noexcept;
    [[nodiscard]] ShowTick reposition(core::u64 advanced) noexcept;
    [[nodiscard]] ShowTick current_tick(
        core::u64 advanced,
        bool wrapped,
        bool reached_end) const noexcept;

    ShowClockConfig config_{};
    TransportState state_{TransportState::Stopped};
    ShowFrame frame_{};
    TimePoint observed_at_{};
    core::u64 remainder_{};
};

// Non-drop-frame timecode for the supplied rate. `frames_per_second` rounds the
// rational rate to the nearest whole frame count, which is the standard
// non-drop convention: a 60000/1001 show is labelled at 60 frames per second.
struct Timecode final {
    core::u32 hours{};
    core::u32 minutes{};
    core::u32 seconds{};
    core::u32 frames{};

    bool operator==(const Timecode&) const = default;
};

[[nodiscard]] core::u32 frames_per_second(FrameRate rate) noexcept;
[[nodiscard]] Timecode timecode(ShowFrame frame, FrameRate rate) noexcept;

} // namespace mediaserver::show
