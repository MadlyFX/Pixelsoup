#include "allocation_guard.hpp"

#include <mediaserver/show/frame_root_mailbox.hpp>

#include "show_test_support.hpp"

#include <array>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace doc = mediaserver::doc;
namespace show = mediaserver::show;
namespace cmd = mediaserver::cmd;

using mediaserver::show::test::AllocationGuard;

[[nodiscard]] doc::Root root_with(mediaserver::core::u64 marker) {
    doc::Node node{};
    node.id = doc::NodeId{marker};
    node.type = "test.marker";
    node.properties.emplace(
        std::string{"marker"}, static_cast<std::int64_t>(marker));
    return doc::Root{}.set(node);
}

[[nodiscard]] show::PublishedFrame frame_at(mediaserver::core::u64 index) {
    show::PublishedFrame published{};
    published.control_frame = cmd::FrameNumber{index};
    published.root = root_with(index);
    published.version = published.root.version();
    return published;
}

[[nodiscard]] show::FrameRootMailbox mailbox(std::size_t capacity = 4U) {
    auto created = show::FrameRootMailbox::create(capacity);
    SHOW_CHECK(created.has_value());
    return std::move(created).value();
}

[[nodiscard]] show::RootRetirementRing retirement(std::size_t capacity = 4U) {
    auto created = show::RootRetirementRing::create(capacity);
    SHOW_CHECK(created.has_value());
    return std::move(created).value();
}

void only_power_of_two_capacities_are_accepted() {
    for (const std::size_t capacity : {std::size_t{0U}, std::size_t{3U},
                                       std::size_t{6U}, std::size_t{1U << 21U}}) {
        const auto created = show::FrameRootMailbox::create(capacity);
        SHOW_CHECK(!created.has_value());
        SHOW_CHECK(created.error() == show::MailboxError::InvalidCapacity);

        const auto ring = show::RootRetirementRing::create(capacity);
        SHOW_CHECK(!ring.has_value());
        SHOW_CHECK(ring.error() == show::MailboxError::InvalidCapacity);
    }

    const auto accepted = show::FrameRootMailbox::create(1U);
    SHOW_CHECK(accepted.has_value());
    SHOW_CHECK(accepted.value().capacity() == 1U);
}

void frames_are_delivered_in_order() {
    auto box = mailbox();
    for (mediaserver::core::u64 index = 1U; index <= 4U; ++index) {
        SHOW_CHECK(
            box.publish(frame_at(index)) == show::PublishStatus::Published);
    }
    SHOW_CHECK(box.size() == 4U);

    for (mediaserver::core::u64 index = 1U; index <= 4U; ++index) {
        show::PublishedFrame received{};
        SHOW_CHECK(box.consume(received));
        SHOW_CHECK(received.control_frame == cmd::FrameNumber{index});
        SHOW_CHECK(received.root.contains(doc::NodeId{index}));
    }

    show::PublishedFrame empty{};
    SHOW_CHECK(!box.consume(empty));
    SHOW_CHECK(box.empty());
    SHOW_CHECK(box.published_count() == 4U);
    SHOW_CHECK(box.consumed_count() == 4U);
}

void publishing_never_allocates_or_frees() {
    auto box = mailbox();
    // Prime every slot so the steady-state path, not first touch, is measured.
    for (mediaserver::core::u64 index = 1U; index <= 4U; ++index) {
        SHOW_CHECK(
            box.publish(frame_at(index)) == show::PublishStatus::Published);
    }
    for (mediaserver::core::u64 index = 1U; index <= 4U; ++index) {
        show::PublishedFrame received{};
        SHOW_CHECK(box.consume(received));
    }

    auto prepared = frame_at(9U);
    show::PublishedFrame received{};
    {
        const AllocationGuard guard{};
        const auto status = box.publish(std::move(prepared));
        const auto consumed = box.consume(received);
        if (status != show::PublishStatus::Published || !consumed) {
            throw std::runtime_error{"handoff failed under the guard"};
        }
        SHOW_CHECK(guard.quiet());
    }
    SHOW_CHECK(received.control_frame == cmd::FrameNumber{9U});
}

void a_full_mailbox_refuses_without_losing_anything() {
    auto box = mailbox(2U);
    SHOW_CHECK(box.publish(frame_at(1U)) == show::PublishStatus::Published);
    SHOW_CHECK(box.publish(frame_at(2U)) == show::PublishStatus::Published);

    // Approved state already in the mailbox is never displaced to make room.
    SHOW_CHECK(box.publish(frame_at(3U)) == show::PublishStatus::Overflow);
    SHOW_CHECK(box.publish(frame_at(4U)) == show::PublishStatus::Overflow);
    SHOW_CHECK(box.overflow_count() == 2U);
    SHOW_CHECK(box.size() == 2U);

    show::PublishedFrame received{};
    SHOW_CHECK(box.consume(received));
    SHOW_CHECK(received.control_frame == cmd::FrameNumber{1U});

    // Space freed by the consumer is immediately usable again.
    SHOW_CHECK(box.publish(frame_at(5U)) == show::PublishStatus::Published);
    SHOW_CHECK(box.consume(received));
    SHOW_CHECK(received.control_frame == cmd::FrameNumber{2U});
    SHOW_CHECK(box.consume(received));
    SHOW_CHECK(received.control_frame == cmd::FrameNumber{5U});
}

void an_overflow_leaves_the_frame_with_its_caller() {
    auto box = mailbox(1U);
    SHOW_CHECK(box.publish(frame_at(1U)) == show::PublishStatus::Published);

    auto rejected = frame_at(2U);
    {
        const AllocationGuard guard{};
        const auto status = box.publish(std::move(rejected));
        if (status != show::PublishStatus::Overflow) {
            throw std::runtime_error{"expected an overflow"};
        }
        SHOW_CHECK(guard.quiet());
        // A refused frame is still owned by the caller, so nothing was
        // released on the boundary. The caller decides when it goes.
        SHOW_CHECK(rejected.control_frame == cmd::FrameNumber{2U});
        SHOW_CHECK(rejected.root.contains(doc::NodeId{2U}));
    }

    // A retry after the consumer makes room succeeds with the same frame.
    show::PublishedFrame received{};
    SHOW_CHECK(box.consume(received));
    SHOW_CHECK(box.publish(std::move(rejected)) == show::PublishStatus::Published);
    SHOW_CHECK(box.consume(received));
    SHOW_CHECK(received.control_frame == cmd::FrameNumber{2U});
}

void catching_up_returns_the_newest_and_reports_the_gap() {
    auto box = mailbox();
    for (mediaserver::core::u64 index = 1U; index <= 4U; ++index) {
        SHOW_CHECK(
            box.publish(frame_at(index)) == show::PublishStatus::Published);
    }

    show::PublishedFrame received{};
    std::size_t skipped = 0U;
    SHOW_CHECK(box.consume_latest(received, skipped));
    SHOW_CHECK(skipped == 3U);
    SHOW_CHECK(received.control_frame == cmd::FrameNumber{4U});
    SHOW_CHECK(box.empty());

    SHOW_CHECK(!box.consume_latest(received, skipped));
    SHOW_CHECK(skipped == 0U);

    // A single pending frame is returned without reporting a gap.
    SHOW_CHECK(box.publish(frame_at(7U)) == show::PublishStatus::Published);
    SHOW_CHECK(box.consume_latest(received, skipped));
    SHOW_CHECK(skipped == 0U);
    SHOW_CHECK(received.control_frame == cmd::FrameNumber{7U});
}

void catching_up_through_a_retirement_ring_frees_nothing() {
    auto box = mailbox();
    auto ring = retirement();
    for (mediaserver::core::u64 index = 1U; index <= 4U; ++index) {
        SHOW_CHECK(
            box.publish(frame_at(index)) == show::PublishStatus::Published);
    }

    show::PublishedFrame received{};
    std::size_t skipped = 0U;
    {
        const AllocationGuard guard{};
        const auto ok = box.consume_latest(received, skipped, &ring);
        if (!ok) {
            throw std::runtime_error{"expected a frame"};
        }
        SHOW_CHECK(guard.quiet());
    }
    SHOW_CHECK(skipped == 3U);
    SHOW_CHECK(received.control_frame == cmd::FrameNumber{4U});
    SHOW_CHECK(ring.size() == 3U);
    SHOW_CHECK(ring.retired_count() == 3U);

    // The parked roots are released away from the boundary, not on it.
    SHOW_CHECK(ring.drain() == 3U);
    SHOW_CHECK(ring.size() == 0U);
}

void a_full_retirement_ring_reports_rather_than_grows() {
    auto ring = retirement(2U);
    SHOW_CHECK(ring.retire(root_with(1U)));
    SHOW_CHECK(ring.retire(root_with(2U)));
    SHOW_CHECK(!ring.retire(root_with(3U)));
    SHOW_CHECK(ring.rejected_count() == 1U);
    SHOW_CHECK(ring.size() == 2U);

    SHOW_CHECK(ring.drain() == 2U);
    SHOW_CHECK(ring.retire(root_with(4U)));
    SHOW_CHECK(ring.drain() == 1U);
    SHOW_CHECK(ring.drain() == 0U);
}

void retiring_a_root_never_allocates() {
    auto ring = retirement();
    // Prime and drain so the slots are in their steady, emptied state.
    SHOW_CHECK(ring.retire(root_with(1U)));
    SHOW_CHECK(ring.drain() == 1U);

    auto parked = root_with(2U);
    {
        const AllocationGuard guard{};
        const auto accepted = ring.retire(std::move(parked));
        if (!accepted) {
            throw std::runtime_error{"expected the retirement to be accepted"};
        }
        SHOW_CHECK(guard.quiet());
    }
    SHOW_CHECK(ring.drain() == 1U);
}

void a_producer_and_consumer_thread_see_every_frame_in_order() {
    constexpr mediaserver::core::u64 total = 4'096U;
    auto box = mailbox(16U);

    std::vector<mediaserver::core::u64> received;
    received.reserve(total);

    std::thread consumer{[&box, &received]() {
        show::PublishedFrame frame{};
        while (received.size() < total) {
            if (box.consume(frame)) {
                received.push_back(frame.control_frame.value());
            } else {
                std::this_thread::yield();
            }
        }
    }};

    for (mediaserver::core::u64 index = 1U; index <= total; ++index) {
        while (box.publish(frame_at(index)) != show::PublishStatus::Published) {
            std::this_thread::yield();
        }
    }
    consumer.join();

    SHOW_CHECK(received.size() == total);
    for (mediaserver::core::u64 index = 0U; index < total; ++index) {
        SHOW_CHECK(received[index] == index + 1U);
    }
}

void catch_up_preserves_an_asset_activation_snapshot() {
    auto box = mailbox(8);
    auto retired = retirement(8);
    for (unsigned i = 1; i <= 6; ++i)
        SHOW_CHECK(box.publish(frame_at(i)) == show::PublishStatus::Published);
    show::PublishedFrame frame;
    std::size_t skipped{};
    SHOW_CHECK(box.consume_latest(frame, skipped, &retired, cmd::FrameNumber{3}));
    SHOW_CHECK(frame.control_frame == cmd::FrameNumber{3});
    SHOW_CHECK(skipped == 2 && box.size() == 3);
    // The control plane can keep publishing during a busy GPU's retry.
    SHOW_CHECK(box.publish(frame_at(7)) == show::PublishStatus::Published);
    SHOW_CHECK(retired.retire(std::move(frame.root)));
    SHOW_CHECK(box.consume_latest(frame, skipped, &retired));
    SHOW_CHECK(frame.control_frame == cmd::FrameNumber{7} && skipped == 3);
}

constexpr std::array<mediaserver::show::test::Case, 10U> cases{{
    {"asset activation survives catch-up", &catch_up_preserves_an_asset_activation_snapshot},
    {"power-of-two capacity", &only_power_of_two_capacities_are_accepted},
    {"ordered delivery", &frames_are_delivered_in_order},
    {"publish is allocation-free", &publishing_never_allocates_or_frees},
    {"full mailbox keeps state", &a_full_mailbox_refuses_without_losing_anything},
    {"overflow keeps ownership", &an_overflow_leaves_the_frame_with_its_caller},
    {"catch up reports the gap",
     &catching_up_returns_the_newest_and_reports_the_gap},
    {"catch up through retirement",
     &catching_up_through_a_retirement_ring_frees_nothing},
    {"full retirement ring", &a_full_retirement_ring_reports_rather_than_grows},
    {"retire is allocation-free", &retiring_a_root_never_allocates},
}};

constexpr std::array<mediaserver::show::test::Case, 1U> threaded{{
    {"single producer and consumer",
     &a_producer_and_consumer_thread_see_every_frame_in_order},
}};

} // namespace

int main() {
    const auto core = mediaserver::show::test::run(cases);
    const auto concurrent = mediaserver::show::test::run(threaded);
    return core != 0 || concurrent != 0 ? 1 : 0;
}
