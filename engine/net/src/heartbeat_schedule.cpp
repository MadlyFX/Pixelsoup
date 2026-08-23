#include <mediaserver/net/heartbeat_schedule.hpp>

#include <mediaserver/net/session_protocol.hpp>

namespace mediaserver::net {
namespace {

using Clock = HeartbeatSchedule::Clock;
using TimePoint = HeartbeatSchedule::TimePoint;

[[nodiscard]] bool add_time(
    TimePoint base,
    std::chrono::milliseconds delta,
    TimePoint& output) noexcept {
    const auto converted =
        std::chrono::duration_cast<Clock::duration>(delta);
    if (base > TimePoint::max() - converted) {
        return false;
    }
    output = base + converted;
    return true;
}

[[nodiscard]] bool valid_config(
    const HeartbeatScheduleConfig& config) noexcept {
    const auto minimum = std::chrono::milliseconds{
        min_heartbeat_response_timeout_ms};
    const auto maximum = std::chrono::milliseconds{
        max_heartbeat_response_timeout_ms};
    return config.interval > std::chrono::milliseconds::zero()
        && config.response_timeout >= minimum
        && config.response_timeout <= maximum
        && config.interval < config.response_timeout;
}

} // namespace

core::Result<HeartbeatSchedule, HeartbeatScheduleError>
HeartbeatSchedule::create(
    HeartbeatScheduleConfig config,
    TimePoint live_at) noexcept {
    using Result = core::Result<
        HeartbeatSchedule,
        HeartbeatScheduleError>;
    if (!valid_config(config)) {
        return Result::failure(
            HeartbeatScheduleError::InvalidConfiguration);
    }
    TimePoint first_probe{};
    if (!add_time(live_at, config.interval, first_probe)) {
        return Result::failure(HeartbeatScheduleError::TimeOverflow);
    }
    return Result::success(HeartbeatSchedule{config, first_probe});
}

HeartbeatSchedulePoll HeartbeatSchedule::poll(TimePoint now) const noexcept {
    if (!outstanding_) {
        return now < next_probe_at_
            ? HeartbeatSchedulePoll::NotDue
            : HeartbeatSchedulePoll::Due;
    }
    return now < outstanding_->deadline
        ? HeartbeatSchedulePoll::Awaiting
        : HeartbeatSchedulePoll::Expired;
}

core::Result<ScheduledHeartbeat, HeartbeatScheduleError>
HeartbeatSchedule::mark_sent(
    core::u64 sequence,
    TimePoint sent_at) noexcept {
    using Result = core::Result<
        ScheduledHeartbeat,
        HeartbeatScheduleError>;
    if (sequence == 0U) {
        return Result::failure(HeartbeatScheduleError::InvalidSequence);
    }
    if (outstanding_) {
        return Result::failure(
            HeartbeatScheduleError::AlreadyOutstanding);
    }
    if (sent_at < next_probe_at_) {
        return Result::failure(HeartbeatScheduleError::NotDue);
    }
    TimePoint deadline{};
    if (!add_time(sent_at, config_.response_timeout, deadline)) {
        return Result::failure(HeartbeatScheduleError::TimeOverflow);
    }
    outstanding_ = ScheduledHeartbeat{sequence, sent_at, deadline};
    return Result::success(*outstanding_);
}

core::Result<AcknowledgedHeartbeat, HeartbeatScheduleError>
HeartbeatSchedule::acknowledge(
    core::u64 acknowledged_sequence,
    TimePoint acknowledged_at) noexcept {
    using Result = core::Result<
        AcknowledgedHeartbeat,
        HeartbeatScheduleError>;
    if (!outstanding_) {
        return Result::failure(HeartbeatScheduleError::NoOutstanding);
    }
    if (acknowledged_sequence != outstanding_->sequence) {
        return Result::failure(
            HeartbeatScheduleError::AcknowledgementMismatch);
    }
    if (acknowledged_at < outstanding_->sent_at) {
        return Result::failure(HeartbeatScheduleError::TimeRegression);
    }
    if (acknowledged_at >= outstanding_->deadline) {
        return Result::failure(HeartbeatScheduleError::Expired);
    }
    TimePoint following_probe{};
    if (!add_time(acknowledged_at, config_.interval, following_probe)) {
        return Result::failure(HeartbeatScheduleError::TimeOverflow);
    }

    const AcknowledgedHeartbeat acknowledged{
        outstanding_->sequence,
        acknowledged_at,
        following_probe,
    };
    outstanding_.reset();
    next_probe_at_ = following_probe;
    return Result::success(acknowledged);
}

} // namespace mediaserver::net
