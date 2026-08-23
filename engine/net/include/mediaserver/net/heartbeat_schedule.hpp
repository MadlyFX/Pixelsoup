#pragma once

#include <chrono>
#include <optional>

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>

namespace mediaserver::net {

struct HeartbeatScheduleConfig final {
    std::chrono::milliseconds interval{};
    std::chrono::milliseconds response_timeout{};

    bool operator==(const HeartbeatScheduleConfig&) const = default;
};

enum class HeartbeatSchedulePoll : core::u8 {
    NotDue,
    Due,
    Awaiting,
    Expired,
};

enum class HeartbeatScheduleError : core::u8 {
    InvalidConfiguration,
    TimeOverflow,
    InvalidSequence,
    NotDue,
    AlreadyOutstanding,
    NoOutstanding,
    AcknowledgementMismatch,
    TimeRegression,
    Expired,
};

struct ScheduledHeartbeat final {
    core::u64 sequence{};
    std::chrono::steady_clock::time_point sent_at{};
    std::chrono::steady_clock::time_point deadline{};

    bool operator==(const ScheduledHeartbeat&) const = default;
};

struct AcknowledgedHeartbeat final {
    core::u64 sequence{};
    std::chrono::steady_clock::time_point acknowledged_at{};
    std::chrono::steady_clock::time_point next_probe_at{};

    bool operator==(const AcknowledgedHeartbeat&) const = default;
};

// A deterministic, connection-local heartbeat timer state machine. It never
// reads a clock: callers supply every time point explicitly. One successful
// mark_sent() creates exactly one outstanding sequence, and only an on-time
// acknowledgement of that exact sequence clears it. The following probe is
// scheduled from acknowledgement time rather than send time or deadline.
class HeartbeatSchedule final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    [[nodiscard]] static core::Result<
        HeartbeatSchedule,
        HeartbeatScheduleError>
    create(HeartbeatScheduleConfig config, TimePoint live_at) noexcept;

    [[nodiscard]] HeartbeatSchedulePoll poll(TimePoint now) const noexcept;

    [[nodiscard]] core::Result<ScheduledHeartbeat, HeartbeatScheduleError>
    mark_sent(core::u64 sequence, TimePoint sent_at) noexcept;

    [[nodiscard]] core::Result<AcknowledgedHeartbeat, HeartbeatScheduleError>
    acknowledge(core::u64 acknowledged_sequence, TimePoint acknowledged_at)
        noexcept;

    [[nodiscard]] const HeartbeatScheduleConfig& config() const noexcept {
        return config_;
    }
    [[nodiscard]] TimePoint next_probe_at() const noexcept {
        return next_probe_at_;
    }
    [[nodiscard]] const std::optional<ScheduledHeartbeat>& outstanding()
        const noexcept {
        return outstanding_;
    }

private:
    HeartbeatSchedule(
        HeartbeatScheduleConfig config,
        TimePoint next_probe_at) noexcept
        : config_(config), next_probe_at_(next_probe_at) {}

    HeartbeatScheduleConfig config_{};
    TimePoint next_probe_at_{};
    std::optional<ScheduledHeartbeat> outstanding_;
};

} // namespace mediaserver::net
