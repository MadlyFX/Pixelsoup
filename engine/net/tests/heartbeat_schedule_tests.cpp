#include <mediaserver/net/heartbeat_schedule.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace net = mediaserver::net;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define HEARTBEAT_CHECK(expression) \
    check(static_cast<bool>(expression), #expression, __LINE__)

using namespace std::chrono_literals;
using TimePoint = net::HeartbeatSchedule::TimePoint;

[[nodiscard]] TimePoint at(std::chrono::milliseconds value) {
    return TimePoint{
        std::chrono::duration_cast<net::HeartbeatSchedule::Clock::duration>(
            value)};
}

[[nodiscard]] net::HeartbeatSchedule schedule(
    net::HeartbeatScheduleConfig config = {5s, 10s},
    TimePoint live_at = at(1s)) {
    auto created = net::HeartbeatSchedule::create(config, live_at);
    HEARTBEAT_CHECK(created.has_value());
    return std::move(created).value();
}

void invalid_configuration_and_initial_overflow_are_rejected() {
    for (const auto config : {
             net::HeartbeatScheduleConfig{0ms, 10s},
             net::HeartbeatScheduleConfig{-1ms, 10s},
             net::HeartbeatScheduleConfig{5s, 249ms},
             net::HeartbeatScheduleConfig{5s, 120'001ms},
             net::HeartbeatScheduleConfig{10s, 10s},
             net::HeartbeatScheduleConfig{11s, 10s},
         }) {
        const auto created = net::HeartbeatSchedule::create(config, at(1s));
        HEARTBEAT_CHECK(!created.has_value());
        HEARTBEAT_CHECK(
            created.error()
            == net::HeartbeatScheduleError::InvalidConfiguration);
    }

    const auto overflow = net::HeartbeatSchedule::create(
        {100ms, 250ms},
        TimePoint::max() - 50ms);
    HEARTBEAT_CHECK(!overflow.has_value());
    HEARTBEAT_CHECK(
        overflow.error() == net::HeartbeatScheduleError::TimeOverflow);
}

void due_boundary_and_single_outstanding_sequence_are_exact() {
    auto timer = schedule();
    HEARTBEAT_CHECK(timer.next_probe_at() == at(6s));
    HEARTBEAT_CHECK(timer.poll(at(5999ms)) == net::HeartbeatSchedulePoll::NotDue);
    HEARTBEAT_CHECK(timer.poll(at(6s)) == net::HeartbeatSchedulePoll::Due);

    auto early = timer.mark_sent(7U, at(5999ms));
    HEARTBEAT_CHECK(!early.has_value());
    HEARTBEAT_CHECK(early.error() == net::HeartbeatScheduleError::NotDue);
    auto zero = timer.mark_sent(0U, at(6s));
    HEARTBEAT_CHECK(!zero.has_value());
    HEARTBEAT_CHECK(
        zero.error() == net::HeartbeatScheduleError::InvalidSequence);

    auto sent = timer.mark_sent(7U, at(6s));
    HEARTBEAT_CHECK(sent.has_value());
    HEARTBEAT_CHECK(sent.value().sequence == 7U);
    HEARTBEAT_CHECK(sent.value().sent_at == at(6s));
    HEARTBEAT_CHECK(sent.value().deadline == at(16s));
    HEARTBEAT_CHECK(timer.poll(at(15999ms)) == net::HeartbeatSchedulePoll::Awaiting);
    HEARTBEAT_CHECK(timer.poll(at(16s)) == net::HeartbeatSchedulePoll::Expired);

    auto second = timer.mark_sent(8U, at(7s));
    HEARTBEAT_CHECK(!second.has_value());
    HEARTBEAT_CHECK(
        second.error() == net::HeartbeatScheduleError::AlreadyOutstanding);
    HEARTBEAT_CHECK(timer.outstanding()->sequence == 7U);
}

void only_an_exact_on_time_acknowledgement_advances_from_ack_time() {
    auto timer = schedule();
    HEARTBEAT_CHECK(timer.mark_sent(41U, at(7s)));

    auto wrong = timer.acknowledge(40U, at(8s));
    HEARTBEAT_CHECK(!wrong.has_value());
    HEARTBEAT_CHECK(
        wrong.error()
        == net::HeartbeatScheduleError::AcknowledgementMismatch);
    HEARTBEAT_CHECK(timer.outstanding()->sequence == 41U);

    auto regressed = timer.acknowledge(41U, at(6999ms));
    HEARTBEAT_CHECK(!regressed.has_value());
    HEARTBEAT_CHECK(
        regressed.error() == net::HeartbeatScheduleError::TimeRegression);

    auto accepted = timer.acknowledge(41U, at(9s));
    HEARTBEAT_CHECK(accepted.has_value());
    HEARTBEAT_CHECK(accepted.value().sequence == 41U);
    HEARTBEAT_CHECK(accepted.value().acknowledged_at == at(9s));
    HEARTBEAT_CHECK(accepted.value().next_probe_at == at(14s));
    HEARTBEAT_CHECK(!timer.outstanding().has_value());
    HEARTBEAT_CHECK(timer.poll(at(13999ms)) == net::HeartbeatSchedulePoll::NotDue);
    HEARTBEAT_CHECK(timer.poll(at(14s)) == net::HeartbeatSchedulePoll::Due);

    auto none = timer.acknowledge(41U, at(10s));
    HEARTBEAT_CHECK(!none.has_value());
    HEARTBEAT_CHECK(
        none.error() == net::HeartbeatScheduleError::NoOutstanding);
}

void deadline_is_exclusive_and_expiry_is_sticky() {
    auto timer = schedule();
    HEARTBEAT_CHECK(timer.mark_sent(9U, at(6s)));
    auto late = timer.acknowledge(9U, at(16s));
    HEARTBEAT_CHECK(!late.has_value());
    HEARTBEAT_CHECK(late.error() == net::HeartbeatScheduleError::Expired);
    HEARTBEAT_CHECK(timer.poll(at(16s)) == net::HeartbeatSchedulePoll::Expired);
    HEARTBEAT_CHECK(timer.outstanding()->sequence == 9U);

    auto later = timer.acknowledge(9U, at(17s));
    HEARTBEAT_CHECK(!later.has_value());
    HEARTBEAT_CHECK(later.error() == net::HeartbeatScheduleError::Expired);
}

void send_and_acknowledgement_time_overflow_do_not_mutate_state() {
    auto send_overflow = schedule(
        {100ms, 250ms},
        TimePoint::max() - 400ms);
    const auto due = send_overflow.next_probe_at();
    auto not_representable = send_overflow.mark_sent(
        1U, TimePoint::max() - 200ms);
    HEARTBEAT_CHECK(!not_representable.has_value());
    HEARTBEAT_CHECK(
        not_representable.error()
        == net::HeartbeatScheduleError::TimeOverflow);
    HEARTBEAT_CHECK(!send_overflow.outstanding().has_value());
    HEARTBEAT_CHECK(send_overflow.next_probe_at() == due);

    auto ack_overflow = schedule(
        {100ms, 250ms},
        TimePoint::max() - 400ms);
    HEARTBEAT_CHECK(ack_overflow.mark_sent(
        2U, TimePoint::max() - 300ms));
    auto overflow = ack_overflow.acknowledge(
        2U, TimePoint::max() - 75ms);
    HEARTBEAT_CHECK(!overflow.has_value());
    HEARTBEAT_CHECK(
        overflow.error() == net::HeartbeatScheduleError::TimeOverflow);
    HEARTBEAT_CHECK(ack_overflow.outstanding()->sequence == 2U);
    HEARTBEAT_CHECK(
        ack_overflow.poll(TimePoint::max() - 75ms)
        == net::HeartbeatSchedulePoll::Awaiting);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"invalid configuration and initial overflow",
         &invalid_configuration_and_initial_overflow_are_rejected},
        {"due boundary and one outstanding sequence",
         &due_boundary_and_single_outstanding_sequence_are_exact},
        {"exact ack schedules from ack time",
         &only_an_exact_on_time_acknowledgement_advances_from_ack_time},
        {"exclusive deadline and sticky expiry",
         &deadline_is_exclusive_and_expiry_is_sticky},
        {"time overflow is transactional",
         &send_and_acknowledgement_time_overflow_do_not_mutate_state},
    };

    std::size_t failures = 0U;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[pass] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << (tests.size() - failures) << '/' << tests.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
