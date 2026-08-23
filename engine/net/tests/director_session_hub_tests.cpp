#include <mediaserver/net/director_session_hub.hpp>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace command = mediaserver::cmd;
namespace core = mediaserver::core;
namespace document = mediaserver::doc;
namespace net = mediaserver::net;
namespace persist = mediaserver::persist;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define HUB_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TempWal final {
public:
    TempWal() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("mediaserver-session-hub-" + std::to_string(timestamp) + "-"
               + std::to_string(sequence.fetch_add(1U)) + ".bin");
    }

    ~TempWal() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TempWal(const TempWal&) = delete;
    TempWal& operator=(const TempWal&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] command::StreamCursor genesis_cursor() noexcept {
    return command::StreamCursor{
        command::FrameNumber{0U},
        document::Version{0U},
        command::CommandId{1U},
    };
}

[[nodiscard]] bool same_cursor(
    const command::StreamCursor& left,
    const command::StreamCursor& right) noexcept {
    return left.through_frame == right.through_frame && left.revision == right.revision
        && left.next_command == right.next_command;
}

[[nodiscard]] persist::CommandLog open_log(
    const std::filesystem::path& path) {
    persist::OpenOptions options;
    options.sync_policy = persist::SyncPolicy::Data;
    auto opened = persist::CommandLog::open_or_create(
        path, genesis_cursor(), options);
    HUB_CHECK(opened);
    return std::move(opened).value();
}

[[nodiscard]] document::Node make_node(
    std::uint64_t node_id,
    std::int64_t value) {
    return document::Node{
        document::NodeId{node_id},
        "hub.node",
        {{"value", value}},
    };
}

void schedule_put(
    command::DirectorScheduler& director,
    std::uint64_t frame) {
    auto scheduled = director.schedule_edit(
        command::PutNode{make_node(frame, static_cast<std::int64_t>(frame))},
        command::EraseNode{document::NodeId{frame}},
        command::UndoContextId{1U},
        command::TransactionId{frame},
        command::FrameNumber{frame});
    HUB_CHECK(scheduled);
}

void snapshot_tail_hands_off_to_the_next_live_frame() {
    TempWal temporary;
    auto log = open_log(temporary.path());
    command::DirectorScheduler director;
    persist::DurableDirector durable{director, log};
    net::DirectorSessionHub hub{durable};

    schedule_put(director, 1U);
    auto first = hub.seal_and_publish(command::FrameNumber{1U});
    HUB_CHECK(first);
    HUB_CHECK(first.value().publication.queued == 0U);

    auto subscribed = hub.subscribe();
    HUB_CHECK(subscribed);
    const auto captured = subscribed.value().source.checkpoint().cursor;
    HUB_CHECK(same_cursor(captured, first.value().frame.after_cursor()));

    schedule_put(director, 2U);
    auto second = hub.seal_and_publish(command::FrameNumber{2U});
    HUB_CHECK(second);
    HUB_CHECK(second.value().publication.queued == 1U);
    HUB_CHECK(second.value().publication.already_covered == 0U);
    HUB_CHECK(second.value().publication.resync_required == 0U);

    auto polled = subscribed.value().live.try_pop();
    HUB_CHECK(polled.state == net::DirectorSubscriptionState::Active);
    HUB_CHECK(polled.frame.has_value());
    HUB_CHECK(same_cursor(polled.frame->before_cursor(), captured));
    HUB_CHECK(same_cursor(
        polled.frame->after_cursor(), second.value().frame.after_cursor()));
    HUB_CHECK(polled.frame->commands().size() == 1U);
    HUB_CHECK(polled.frame->seal().frame == command::FrameNumber{2U});
    HUB_CHECK(
        polled.frame->serialized_bytes()
        == polled.frame->batch().receipt().bytes_written);
    HUB_CHECK(subscribed.value().live.queued_frames() == 0U);
    HUB_CHECK(subscribed.value().live.queued_bytes() == 0U);
}

void concurrent_capture_and_seal_has_no_snapshot_to_live_gap() {
    TempWal temporary;
    auto log = open_log(temporary.path());
    command::DirectorScheduler director;
    persist::DurableDirector durable{director, log};
    net::DirectorSessionHub hub{durable};
    auto previous = genesis_cursor();

    for (std::uint64_t frame = 1U; frame <= 12U; ++frame) {
        schedule_put(director, frame);
        std::barrier start{3};
        std::optional<core::Result<
            net::DirectorResumeSubscription,
            net::DirectorSessionHubError>> subscribed;
        std::optional<core::Result<
            net::DirectorSealOutcome,
            net::DirectorSessionHubError>> sealed;
        std::exception_ptr subscribe_exception;
        std::exception_ptr seal_exception;

        std::thread subscriber{[&] {
            try {
                start.arrive_and_wait();
                subscribed.emplace(hub.subscribe());
            } catch (...) {
                subscribe_exception = std::current_exception();
            }
        }};
        std::thread sealer{[&] {
            try {
                start.arrive_and_wait();
                sealed.emplace(hub.seal_and_publish(command::FrameNumber{frame}));
            } catch (...) {
                seal_exception = std::current_exception();
            }
        }};
        start.arrive_and_wait();
        subscriber.join();
        sealer.join();
        if (subscribe_exception) {
            std::rethrow_exception(subscribe_exception);
        }
        if (seal_exception) {
            std::rethrow_exception(seal_exception);
        }
        HUB_CHECK(subscribed.has_value() && *subscribed);
        HUB_CHECK(sealed.has_value() && *sealed);

        const auto captured = subscribed->value().source.checkpoint().cursor;
        const auto after = sealed->value().frame.after_cursor();
        auto poll = subscribed->value().live.try_pop();
        if (same_cursor(captured, previous)) {
            HUB_CHECK(poll.frame.has_value());
            HUB_CHECK(same_cursor(poll.frame->before_cursor(), previous));
            HUB_CHECK(same_cursor(poll.frame->after_cursor(), after));
            HUB_CHECK(sealed->value().publication.queued == 1U);
        } else {
            HUB_CHECK(same_cursor(captured, after));
            HUB_CHECK(!poll.frame.has_value());
            HUB_CHECK(
                sealed->value().publication.queued
                    + sealed->value().publication.already_covered
                <= 1U);
        }
        HUB_CHECK(sealed->value().publication.resync_required == 0U);
        previous = after;
        subscribed->value().live.close();
    }
}

void frame_capacity_overflow_preserves_the_committed_outcome() {
    TempWal temporary;
    auto log = open_log(temporary.path());
    command::DirectorScheduler director;
    persist::DurableDirector durable{director, log};
    net::DirectorSessionHub hub{
        durable,
        net::DirectorSessionHubLimits{4U, 1U, 128U * 1024U * 1024U},
    };
    auto subscribed = hub.subscribe();
    HUB_CHECK(subscribed);

    schedule_put(director, 1U);
    auto first = hub.seal_and_publish(command::FrameNumber{1U});
    HUB_CHECK(first);
    HUB_CHECK(first.value().publication.queued == 1U);
    HUB_CHECK(subscribed.value().live.queued_frames() == 1U);

    schedule_put(director, 2U);
    auto second = hub.seal_and_publish(command::FrameNumber{2U});
    HUB_CHECK(second);
    HUB_CHECK(second.value().publication.resync_required == 1U);
    HUB_CHECK(
        subscribed.value().live.state()
        == net::DirectorSubscriptionState::ResyncRequired);
    HUB_CHECK(subscribed.value().live.queued_frames() == 0U);
    HUB_CHECK(hub.active_subscriptions() == 0U);
    HUB_CHECK(durable.authorizes(second.value().frame.batch()));

    auto source = durable.capture_resume_source();
    HUB_CHECK(source);
    HUB_CHECK(same_cursor(
        source.value().checkpoint().cursor,
        second.value().frame.after_cursor()));
}

void serialized_byte_overflow_is_a_resync_not_a_seal_failure() {
    TempWal temporary;
    auto log = open_log(temporary.path());
    command::DirectorScheduler director;
    persist::DurableDirector durable{director, log};
    net::DirectorSessionHub hub{
        durable,
        net::DirectorSessionHubLimits{1U, 4U, 1U},
    };
    auto subscribed = hub.subscribe();
    HUB_CHECK(subscribed);

    schedule_put(director, 1U);
    auto sealed = hub.seal_and_publish(command::FrameNumber{1U});
    HUB_CHECK(sealed);
    HUB_CHECK(sealed.value().frame.serialized_bytes() > 1U);
    HUB_CHECK(sealed.value().publication.resync_required == 1U);
    HUB_CHECK(durable.authorizes(sealed.value().frame.batch()));
    HUB_CHECK(
        subscribed.value().live.state()
        == net::DirectorSubscriptionState::ResyncRequired);
}

void slot_generation_fences_a_stale_close() {
    TempWal temporary;
    auto log = open_log(temporary.path());
    command::DirectorScheduler director;
    persist::DurableDirector durable{director, log};
    net::DirectorSessionHub hub{
        durable,
        net::DirectorSessionHubLimits{1U, 2U, 1U * 1024U * 1024U},
    };

    auto first = hub.subscribe();
    HUB_CHECK(first);
    const auto first_id = first.value().live.id();
    HUB_CHECK(first_id.has_value());
    auto full = hub.subscribe();
    HUB_CHECK(!full);
    HUB_CHECK(
        full.error().code
        == net::DirectorSessionHubErrorCode::SubscriptionCapacity);

    first.value().live.close();
    HUB_CHECK(hub.active_subscriptions() == 0U);
    auto replacement = hub.subscribe();
    HUB_CHECK(replacement);
    const auto replacement_id = replacement.value().live.id();
    HUB_CHECK(replacement_id.has_value());
    HUB_CHECK(replacement_id->slot == first_id->slot);
    HUB_CHECK(replacement_id->generation > first_id->generation);

    first.value().live.close();
    HUB_CHECK(
        replacement.value().live.state()
        == net::DirectorSubscriptionState::Active);
    HUB_CHECK(hub.active_subscriptions() == 1U);
}

void hub_close_wakes_a_waiting_subscription() {
    TempWal temporary;
    auto log = open_log(temporary.path());
    command::DirectorScheduler director;
    persist::DurableDirector durable{director, log};
    net::DirectorSessionHub hub{durable};
    auto subscribed = hub.subscribe();
    HUB_CHECK(subscribed);

    std::optional<net::DirectorSubscriptionPoll> poll;
    std::thread waiter{[&] {
        poll.emplace(subscribed.value().live.wait_for(std::chrono::seconds{5}));
    }};
    hub.close();
    waiter.join();
    HUB_CHECK(poll.has_value());
    HUB_CHECK(poll->state == net::DirectorSubscriptionState::Closed);
    HUB_CHECK(!poll->frame.has_value());
    HUB_CHECK(hub.active_subscriptions() == 0U);

    auto after_close = hub.subscribe();
    HUB_CHECK(!after_close);
    HUB_CHECK(
        after_close.error().code == net::DirectorSessionHubErrorCode::Closed);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"snapshot tail live handoff",
         &snapshot_tail_hands_off_to_the_next_live_frame},
        {"concurrent capture/seal no gap",
         &concurrent_capture_and_seal_has_no_snapshot_to_live_gap},
        {"frame overflow preserves commit",
         &frame_capacity_overflow_preserves_the_committed_outcome},
        {"serialized-byte overflow preserves commit",
         &serialized_byte_overflow_is_a_resync_not_a_seal_failure},
        {"slot generation stale close fencing",
         &slot_generation_fences_a_stale_close},
        {"hub close wakes waiter", &hub_close_wakes_a_waiting_subscription},
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
