#include "allocation_guard.hpp"

#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/show/frame_boundary_driver.hpp>
#include <mediaserver/show/test_pattern.hpp>
#include <mediaserver/show/transport_author.hpp>
#include <mediaserver/show/transport_state.hpp>

#include "show_test_support.hpp"

#include <array>
#include <chrono>
#include <span>
#include <utility>
#include <vector>

namespace {

namespace cmd = mediaserver::cmd;
namespace show = mediaserver::show;

using mediaserver::show::test::AllocationGuard;
using Clock = show::ShowClock::Clock;
using TimePoint = show::ShowClock::TimePoint;
using namespace std::chrono_literals;

constexpr show::FrameRate pal_50{50U, 1U};
constexpr auto frame_period = 20'000'000ns;

[[nodiscard]] TimePoint at(std::chrono::nanoseconds value) {
    return TimePoint{std::chrono::duration_cast<Clock::duration>(value)};
}

[[nodiscard]] show::ShowClockConfig clock_config(bool loop = false) {
    return show::ShowClockConfig{
        pal_50, show::ShowFrame{0U}, show::ShowFrame{9'999U}, loop};
}

[[nodiscard]] show::FrameBoundaryDriverConfig driver_config(
    mediaserver::core::u64 budget = 4U) {
    show::FrameBoundaryDriverConfig config{};
    config.clock = clock_config();
    config.mailbox_capacity = 8U;
    config.max_commits_per_tick = budget;
    return config;
}

// One Director authoring a transport position, fanned out to every node as a
// complete sealed batch. This is the same path the network carries; only the
// transport is short-circuited.
class Show final {
public:
    explicit Show(std::size_t node_count) : nodes_(node_count) {}

    [[nodiscard]] cmd::NodeCommandBus& node(std::size_t index) noexcept {
        return nodes_[index];
    }
    [[nodiscard]] std::size_t node_count() const noexcept {
        return nodes_.size();
    }

    // Re-anchors so that the control frame about to be sealed derives exactly
    // `position`, then delivers it. Production re-anchors only on operator
    // action; these cases drive a fresh anchor per frame because the driver,
    // not the churn, is what is under test.
    void publish(show::ShowFrame position, show::TransportState state) {
        const cmd::FrameNumber frame{sealed_ + 1U};
        const auto command = author(position, state, frame);
        deliver(frame, &command, nullptr);
    }

    // Delivers to a single node, so one node can be starved deliberately.
    void publish_to(
        std::size_t index,
        show::ShowFrame position,
        show::TransportState state) {
        const cmd::FrameNumber frame{sealed_ + 1U};
        const auto command = author(position, state, frame);
        deliver(frame, &command, &index);
    }

    // Seals a frame carrying nothing at all, which is what every frame of a
    // playing show looks like once the transport is an anchor.
    void publish_empty() {
        const cmd::FrameNumber frame{sealed_ + 1U};
        deliver(frame, nullptr, nullptr);
    }

    [[nodiscard]] cmd::DirectorScheduler& director() noexcept {
        return director_;
    }

private:
    [[nodiscard]] cmd::Command author(
        show::ShowFrame position,
        show::TransportState state,
        cmd::FrameNumber frame) {
        show::TransportAnchor anchor{};
        anchor.anchor_control_frame = frame;
        anchor.anchor_show_frame = position;
        anchor.state = state;
        anchor.direction = show::TransportDirection::Forward;
        anchor.rate = pal_50;
        anchor.first_frame = show::ShowFrame{0U};
        anchor.last_frame = show::ShowFrame{9'999U};

        const auto command = show::schedule_transport_anchor(
            director_,
            anchor,
            frame,
            cmd::UndoContextId{1U},
            cmd::TransactionId{frame.value()});
        SHOW_CHECK(command.has_value());
        return command.value();
    }

    void deliver(
        cmd::FrameNumber frame,
        const cmd::Command* command,
        const std::size_t* only) {
        auto seal = director_.seal_frame(frame);
        SHOW_CHECK(seal.has_value());
        sealed_ = frame.value();

        const std::array<cmd::Command, 1U> one{
            command != nullptr ? *command : cmd::Command{}};
        const std::span<const cmd::Command> batch =
            command != nullptr ? std::span<const cmd::Command>{one}
                               : std::span<const cmd::Command>{};
        for (std::size_t index = 0U; index < nodes_.size(); ++index) {
            if (only != nullptr && *only != index) {
                continue;
            }
            const auto received =
                nodes_[index].receive_complete_batch(batch, seal.value());
            SHOW_CHECK(received.has_value());
        }
    }

    cmd::DirectorScheduler director_{};
    std::vector<cmd::NodeCommandBus> nodes_;
    mediaserver::core::u64 sealed_{};
};

[[nodiscard]] show::FrameBoundaryDriver make_driver(
    show::FrameCommitSink& sink,
    show::FrameBoundaryDriverConfig config = driver_config()) {
    auto created = show::FrameBoundaryDriver::create(config, sink, at(0ns));
    SHOW_CHECK(created.has_value());
    return std::move(created).value();
}

[[nodiscard]] show::PublishedFrame drain(show::FrameBoundaryDriver& driver) {
    show::PublishedFrame latest{};
    show::PublishedFrame frame{};
    while (driver.mailbox().consume(frame)) {
        latest = std::move(frame);
    }
    return latest;
}

void invalid_driver_configurations_are_refused() {
    cmd::NodeCommandBus node{};
    show::LocalNodeCommitSink sink{node};

    auto bad_capacity = driver_config();
    bad_capacity.mailbox_capacity = 5U;
    auto bad_budget = driver_config();
    bad_budget.max_commits_per_tick = 0U;
    auto bad_clock = driver_config();
    bad_clock.clock.rate = show::FrameRate{0U, 1U};

    for (const auto& config : {bad_capacity, bad_budget, bad_clock}) {
        SHOW_CHECK(!show::FrameBoundaryDriver::valid_configuration(config));
        const auto created =
            show::FrameBoundaryDriver::create(config, sink, at(0ns));
        SHOW_CHECK(!created.has_value());
        SHOW_CHECK(
            created.error()
            == show::FrameBoundaryDriverError::InvalidConfiguration);
    }
}

void two_nodes_reach_the_same_show_frame() {
    Show cluster{2U};
    show::LocalNodeCommitSink sink_a{cluster.node(0U)};
    show::LocalNodeCommitSink sink_b{cluster.node(1U)};
    auto driver_a = make_driver(sink_a);
    auto driver_b = make_driver(sink_b);

    SHOW_CHECK(driver_a.play(at(0ns)).has_value());
    SHOW_CHECK(driver_b.play(at(0ns)).has_value());

    show::PublishedFrame last_a{};
    show::PublishedFrame last_b{};
    for (mediaserver::core::u64 index = 1U; index <= 25U; ++index) {
        cluster.publish(show::ShowFrame{index}, show::TransportState::Playing);

        // The two nodes are paced independently: node B observes its boundary
        // a third of a frame after node A. Convergence must not depend on
        // their local clocks agreeing.
        const auto now = index * frame_period;
        const auto tick_a = driver_a.tick(at(now));
        const auto tick_b = driver_b.tick(at(now + frame_period / 3));
        SHOW_CHECK(tick_a.has_value());
        SHOW_CHECK(tick_b.has_value());
        SHOW_CHECK(tick_a.value().committed == 1U);
        SHOW_CHECK(tick_b.value().committed == 1U);

        last_a = drain(driver_a);
        last_b = drain(driver_b);

        SHOW_CHECK(last_a.control_frame == cmd::FrameNumber{index});
        SHOW_CHECK(last_a.control_frame == last_b.control_frame);
        SHOW_CHECK(last_a.transport.has_value());
        SHOW_CHECK(last_a.transport == last_b.transport);
        SHOW_CHECK(last_a.show_frame == show::ShowFrame{index});
    }

    // The presented surfaces agree byte for byte once the node tint matches,
    // which is the property an operator checks by looking at two windows.
    std::vector<mediaserver::core::u8> surface_a(64U * 64U * 4U, 0U);
    std::vector<mediaserver::core::u8> surface_b(64U * 64U * 4U, 0U);
    const show::FrameBufferView view_a{
        surface_a, 64U, 64U, 64U * 4U, show::PixelFormat::Rgba8888};
    const show::FrameBufferView view_b{
        surface_b, 64U, 64U, 64U * 4U, show::PixelFormat::Rgba8888};

    show::TestPatternInput input_a{};
    input_a.show_frame = last_a.show_frame;
    input_a.control_frame = last_a.control_frame;
    input_a.rate = pal_50;
    input_a.state = show::TransportState::Playing;
    auto input_b = input_a;
    input_b.show_frame = last_b.show_frame;
    input_b.control_frame = last_b.control_frame;

    SHOW_CHECK(show::render_test_pattern(view_a, input_a));
    SHOW_CHECK(show::render_test_pattern(view_b, input_b));
    SHOW_CHECK(
        show::frame_buffer_digest(view_a) == show::frame_buffer_digest(view_b));
}

void a_tick_at_the_boundary_neither_allocates_nor_frees() {
    Show cluster{1U};
    show::LocalNodeCommitSink sink{cluster.node(0U)};
    auto driver = make_driver(sink);
    SHOW_CHECK(driver.play(at(0ns)).has_value());

    // Warm every mailbox slot and the command bus so first-touch growth is not
    // attributed to the measured boundary.
    for (mediaserver::core::u64 index = 1U; index <= 8U; ++index) {
        cluster.publish(show::ShowFrame{index}, show::TransportState::Playing);
        SHOW_CHECK(driver.tick(at(index * frame_period)).has_value());
        static_cast<void>(drain(driver));
    }

    cluster.publish(show::ShowFrame{9U}, show::TransportState::Playing);
    show::PublishedFrame presented{};
    {
        const AllocationGuard guard{};
        const auto ticked = driver.tick(at(9U * frame_period));
        const auto consumed = driver.mailbox().consume(presented);
        if (!ticked || ticked.value().committed != 1U || !consumed) {
            throw std::runtime_error{"the boundary did not commit one frame"};
        }
        SHOW_CHECK(guard.quiet());
    }
    SHOW_CHECK(presented.control_frame == cmd::FrameNumber{9U});
    SHOW_CHECK(presented.show_frame == show::ShowFrame{9U});
}

void an_unapproved_frame_is_held_rather_than_invented() {
    Show cluster{1U};
    show::LocalNodeCommitSink sink{cluster.node(0U)};
    auto driver = make_driver(sink);
    SHOW_CHECK(driver.play(at(0ns)).has_value());

    cluster.publish(show::ShowFrame{1U}, show::TransportState::Playing);
    SHOW_CHECK(driver.tick(at(frame_period)).has_value());
    const auto presented = drain(driver);
    SHOW_CHECK(presented.control_frame == cmd::FrameNumber{1U});

    // The Director publishes nothing for the next three boundaries.
    for (mediaserver::core::u64 index = 2U; index <= 4U; ++index) {
        const auto tick = driver.tick(at(index * frame_period));
        SHOW_CHECK(tick.has_value());
        SHOW_CHECK(tick.value().boundaries == 1U);
        SHOW_CHECK(tick.value().committed == 0U);
        SHOW_CHECK(tick.value().held);
        SHOW_CHECK(
            tick.value().last_outcome == show::FrameCommitOutcome::NotApproved);
    }

    // Holding is not a fault: nothing was latched and the node still presents
    // the last approved frame.
    SHOW_CHECK(!cluster.node(0U).needs_resync());
    SHOW_CHECK(cluster.node(0U).committed_through() == cmd::FrameNumber{1U});
    SHOW_CHECK(driver.counters().held_boundaries == 3U);
    SHOW_CHECK(driver.mailbox().empty());

    // Once approval arrives the node resumes from exactly where it held.
    cluster.publish(show::ShowFrame{2U}, show::TransportState::Playing);
    const auto resumed = driver.tick(at(5U * frame_period));
    SHOW_CHECK(resumed.has_value());
    SHOW_CHECK(resumed.value().committed == 1U);
    SHOW_CHECK(drain(driver).control_frame == cmd::FrameNumber{2U});
}

void a_late_node_catches_up_within_its_budget() {
    Show cluster{1U};
    show::LocalNodeCommitSink sink{cluster.node(0U)};
    auto driver = make_driver(sink, driver_config(3U));
    SHOW_CHECK(driver.play(at(0ns)).has_value());

    for (mediaserver::core::u64 index = 1U; index <= 5U; ++index) {
        cluster.publish(show::ShowFrame{index}, show::TransportState::Playing);
    }

    // One late observation covering five boundaries. The budget caps the work
    // done inside a single tick.
    const auto first = driver.tick(at(5U * frame_period));
    SHOW_CHECK(first.has_value());
    SHOW_CHECK(first.value().boundaries == 5U);
    SHOW_CHECK(first.value().committed == 3U);
    SHOW_CHECK(first.value().budget_exhausted);

    // The remaining approved frames are taken on the next boundary, so the
    // backlog drains rather than being discarded.
    const auto second = driver.tick(at(6U * frame_period));
    SHOW_CHECK(second.has_value());
    SHOW_CHECK(second.value().committed == 2U);
    SHOW_CHECK(!second.value().budget_exhausted);
    SHOW_CHECK(cluster.node(0U).committed_through() == cmd::FrameNumber{5U});

    // Playback that is behind presents the newest approved frame and is told
    // how many it skipped.
    show::PublishedFrame latest{};
    std::size_t skipped = 0U;
    SHOW_CHECK(driver.mailbox().consume_latest(latest, skipped));
    SHOW_CHECK(skipped == 4U);
    SHOW_CHECK(latest.control_frame == cmd::FrameNumber{5U});
    SHOW_CHECK(latest.show_frame == show::ShowFrame{5U});
}

void a_paused_transport_crosses_no_boundary() {
    Show cluster{1U};
    show::LocalNodeCommitSink sink{cluster.node(0U)};
    auto driver = make_driver(sink);
    SHOW_CHECK(driver.play(at(0ns)).has_value());

    cluster.publish(show::ShowFrame{1U}, show::TransportState::Playing);
    SHOW_CHECK(driver.tick(at(frame_period)).has_value());
    SHOW_CHECK(drain(driver).control_frame == cmd::FrameNumber{1U});

    SHOW_CHECK(driver.pause(at(frame_period)).has_value());
    cluster.publish(show::ShowFrame{2U}, show::TransportState::Paused);

    for (mediaserver::core::u64 index = 2U; index <= 10U; ++index) {
        const auto tick = driver.tick(at(index * frame_period));
        SHOW_CHECK(tick.has_value());
        SHOW_CHECK(tick.value().boundaries == 0U);
        SHOW_CHECK(tick.value().committed == 0U);
        SHOW_CHECK(tick.value().last_outcome == show::FrameCommitOutcome::Idle);
    }
    SHOW_CHECK(cluster.node(0U).committed_through() == cmd::FrameNumber{1U});
    SHOW_CHECK(driver.mailbox().empty());

    // Resuming picks the approved frame straight up.
    SHOW_CHECK(driver.play(at(10U * frame_period)).has_value());
    SHOW_CHECK(driver.tick(at(11U * frame_period)).has_value());
    SHOW_CHECK(drain(driver).control_frame == cmd::FrameNumber{2U});
}

void an_exact_seek_reaches_every_node() {
    Show cluster{2U};
    show::LocalNodeCommitSink sink_a{cluster.node(0U)};
    show::LocalNodeCommitSink sink_b{cluster.node(1U)};
    auto driver_a = make_driver(sink_a);
    auto driver_b = make_driver(sink_b);
    SHOW_CHECK(driver_a.play(at(0ns)).has_value());
    SHOW_CHECK(driver_b.play(at(0ns)).has_value());

    // A scrub to an arbitrary frame is an ordinary command, so it lands on the
    // same control frame on both nodes.
    cluster.publish(show::ShowFrame{4'321U}, show::TransportState::Paused);
    SHOW_CHECK(driver_a.tick(at(frame_period)).has_value());
    SHOW_CHECK(driver_b.tick(at(frame_period)).has_value());

    const auto a = drain(driver_a);
    const auto b = drain(driver_b);
    SHOW_CHECK(a.show_frame == show::ShowFrame{4'321U});
    SHOW_CHECK(b.show_frame == show::ShowFrame{4'321U});
    SHOW_CHECK(a.transport.value().state == show::TransportState::Paused);
    SHOW_CHECK(a.version == b.version);
}

void a_latched_fault_is_reported_and_stops_committing() {
    Show cluster{1U};
    auto& node = cluster.node(0U);
    show::LocalNodeCommitSink sink{node};
    auto driver = make_driver(sink);
    SHOW_CHECK(driver.play(at(0ns)).has_value());

    cluster.publish(show::ShowFrame{1U}, show::TransportState::Playing);
    SHOW_CHECK(driver.tick(at(frame_period)).has_value());
    SHOW_CHECK(drain(driver).control_frame == cmd::FrameNumber{1U});

    // Committing out of order latches the resync fault the driver must respect.
    const auto forced = node.commit_frame(cmd::FrameNumber{7U});
    SHOW_CHECK(forced.status == cmd::CommitStatus::NeedsResync);
    SHOW_CHECK(node.needs_resync());

    cluster.publish(show::ShowFrame{2U}, show::TransportState::Playing);
    const auto tick = driver.tick(at(2U * frame_period));
    SHOW_CHECK(tick.has_value());
    SHOW_CHECK(tick.value().committed == 0U);
    SHOW_CHECK(tick.value().held);
    SHOW_CHECK(
        tick.value().last_outcome == show::FrameCommitOutcome::NeedsResync);
    SHOW_CHECK(driver.counters().resync_reports == 1U);

    // Nothing unapproved was presented while faulted.
    SHOW_CHECK(driver.mailbox().empty());
    SHOW_CHECK(node.committed_through() == cmd::FrameNumber{1U});
}

void a_staged_checkpoint_blocks_the_boundary_until_activation() {
    Show cluster{1U};
    auto& node = cluster.node(0U);
    show::LocalNodeCommitSink sink{node};
    auto driver = make_driver(sink);
    SHOW_CHECK(driver.play(at(0ns)).has_value());

    cluster.publish(show::ShowFrame{1U}, show::TransportState::Playing);
    SHOW_CHECK(driver.tick(at(frame_period)).has_value());
    SHOW_CHECK(drain(driver).control_frame == cmd::FrameNumber{1U});

    cluster.publish(show::ShowFrame{2U}, show::TransportState::Playing);
    auto staged = node.prepare_checkpoint(cluster.director().checkpoint());
    SHOW_CHECK(staged.has_value());
    SHOW_CHECK(node.prepared_checkpoint() != nullptr);

    const auto blocked = driver.tick(at(2U * frame_period));
    SHOW_CHECK(blocked.has_value());
    SHOW_CHECK(blocked.value().committed == 0U);
    SHOW_CHECK(
        blocked.value().last_outcome
        == show::FrameCommitOutcome::CheckpointPending);
    SHOW_CHECK(driver.mailbox().empty());

    // Activation is the only thing that makes the replacement visible, and the
    // driver simply follows the cursor it finds afterwards.
    SHOW_CHECK(node.activate_prepared_checkpoint(staged.value()));
    SHOW_CHECK(node.prepared_checkpoint() == nullptr);
    SHOW_CHECK(node.committed_through() == cmd::FrameNumber{2U});

    const auto after = driver.tick(at(3U * frame_period));
    SHOW_CHECK(after.has_value());
    SHOW_CHECK(after.value().committed_through == cmd::FrameNumber{2U});
    SHOW_CHECK(
        after.value().last_outcome == show::FrameCommitOutcome::NotApproved);

    cluster.publish(show::ShowFrame{3U}, show::TransportState::Playing);
    const auto resumed = driver.tick(at(4U * frame_period));
    SHOW_CHECK(resumed.has_value());
    SHOW_CHECK(resumed.value().committed == 1U);
    SHOW_CHECK(drain(driver).control_frame == cmd::FrameNumber{3U});
}

void a_stalled_presenter_never_stalls_the_control_plane() {
    Show cluster{1U};
    show::LocalNodeCommitSink sink{cluster.node(0U)};
    // Two mailbox slots and a budget wide enough to overrun them.
    auto config = driver_config(8U);
    config.mailbox_capacity = 2U;
    auto driver = make_driver(sink, config);
    SHOW_CHECK(driver.play(at(0ns)).has_value());

    for (mediaserver::core::u64 index = 1U; index <= 6U; ++index) {
        cluster.publish(show::ShowFrame{index}, show::TransportState::Playing);
    }

    // Playback consumes nothing, so the handoff fills after two frames.
    const auto tick = driver.tick(at(6U * frame_period));
    SHOW_CHECK(tick.has_value());
    SHOW_CHECK(tick.value().mailbox_overflow);
    SHOW_CHECK(tick.value().published == 2U);
    // Committing and acknowledging continued regardless.
    SHOW_CHECK(tick.value().committed == 6U);
    SHOW_CHECK(cluster.node(0U).committed_through() == cmd::FrameNumber{6U});
    SHOW_CHECK(driver.counters().mailbox_overflows == 1U);

    // Nothing durable was lost: the next frame playback receives carries the
    // whole document, so the skipped roots cost frames, not state.
    show::PublishedFrame latest{};
    std::size_t skipped = 0U;
    SHOW_CHECK(driver.mailbox().consume_latest(latest, skipped));
    SHOW_CHECK(latest.control_frame == cmd::FrameNumber{2U});

    cluster.publish(show::ShowFrame{7U}, show::TransportState::Playing);
    SHOW_CHECK(driver.tick(at(7U * frame_period)).has_value());
    SHOW_CHECK(driver.mailbox().consume_latest(latest, skipped));
    SHOW_CHECK(latest.control_frame == cmd::FrameNumber{7U});
    SHOW_CHECK(latest.show_frame == show::ShowFrame{7U});
}

void one_node_falling_behind_does_not_move_the_other() {
    Show cluster{2U};
    show::LocalNodeCommitSink sink_a{cluster.node(0U)};
    show::LocalNodeCommitSink sink_b{cluster.node(1U)};
    auto driver_a = make_driver(sink_a);
    auto driver_b = make_driver(sink_b);
    SHOW_CHECK(driver_a.play(at(0ns)).has_value());
    SHOW_CHECK(driver_b.play(at(0ns)).has_value());

    cluster.publish(show::ShowFrame{1U}, show::TransportState::Playing);
    SHOW_CHECK(driver_a.tick(at(frame_period)).has_value());
    SHOW_CHECK(driver_b.tick(at(frame_period)).has_value());
    SHOW_CHECK(drain(driver_a).control_frame == cmd::FrameNumber{1U});
    SHOW_CHECK(drain(driver_b).control_frame == cmd::FrameNumber{1U});

    // Node B is starved of frame 2 while node A receives it.
    cluster.publish_to(0U, show::ShowFrame{2U}, show::TransportState::Playing);

    const auto tick_a = driver_a.tick(at(2U * frame_period));
    const auto tick_b = driver_b.tick(at(2U * frame_period));
    SHOW_CHECK(tick_a.has_value());
    SHOW_CHECK(tick_b.has_value());
    SHOW_CHECK(tick_a.value().committed == 1U);
    SHOW_CHECK(tick_b.value().committed == 0U);
    SHOW_CHECK(tick_b.value().held);

    // Node B holds an approved frame rather than presenting an unapproved one.
    SHOW_CHECK(!cluster.node(1U).needs_resync());
    SHOW_CHECK(cluster.node(1U).committed_through() == cmd::FrameNumber{1U});
    SHOW_CHECK(drain(driver_a).control_frame == cmd::FrameNumber{2U});
    SHOW_CHECK(driver_b.mailbox().empty());
}

void an_empty_frame_still_advances_the_derived_show_position() {
    Show cluster{2U};
    show::LocalNodeCommitSink sink_a{cluster.node(0U)};
    show::LocalNodeCommitSink sink_b{cluster.node(1U)};
    auto driver_a = make_driver(sink_a);
    auto driver_b = make_driver(sink_b);
    SHOW_CHECK(driver_a.play(at(0ns)).has_value());
    SHOW_CHECK(driver_b.play(at(0ns)).has_value());

    // One anchor, authored once. Every frame after it carries no command at
    // all, which is what a playing show costs the document under V1.
    cluster.publish(show::ShowFrame{100U}, show::TransportState::Playing);
    for (mediaserver::core::u64 index = 0U; index < 24U; ++index) {
        cluster.publish_empty();
    }

    SHOW_CHECK(driver_a.tick(at(25U * frame_period)).has_value());
    SHOW_CHECK(driver_b.tick(at(25U * frame_period + frame_period / 3)).has_value());

    const auto last_a = drain(driver_a);
    const auto last_b = drain(driver_b);
    SHOW_CHECK(last_a.control_frame == last_b.control_frame);
    SHOW_CHECK(last_a.show_frame == last_b.show_frame);
    // The default per-tick budget commits four frames, and the position
    // derived for whichever frame that lands on is exactly the anchor plus the
    // frames since it was cut.
    SHOW_CHECK(
        last_a.show_frame.value()
        == 100U + (last_a.control_frame.value() - 1U));
    // Only the first frame changed the document; the rest share its revision.
    SHOW_CHECK(last_a.version == last_b.version);
    SHOW_CHECK(last_a.control_frame.value() > 1U);
}

constexpr std::array<mediaserver::show::test::Case, 12U> cases{{
    {"invalid configuration", &invalid_driver_configurations_are_refused},
    {"two nodes converge", &two_nodes_reach_the_same_show_frame},
    {"boundary is allocation-free",
     &a_tick_at_the_boundary_neither_allocates_nor_frees},
    {"unapproved frames are held", &an_unapproved_frame_is_held_rather_than_invented},
    {"bounded catch-up", &a_late_node_catches_up_within_its_budget},
    {"pause crosses no boundary", &a_paused_transport_crosses_no_boundary},
    {"exact seek reaches nodes", &an_exact_seek_reaches_every_node},
    {"latched fault reported", &a_latched_fault_is_reported_and_stops_committing},
    {"checkpoint blocks boundary",
     &a_staged_checkpoint_blocks_the_boundary_until_activation},
    {"stalled presenter", &a_stalled_presenter_never_stalls_the_control_plane},
    {"one node behind", &one_node_falling_behind_does_not_move_the_other},
    {"empty frames still advance the show",
     &an_empty_frame_still_advances_the_derived_show_position},
}};

} // namespace

int main() {
    return mediaserver::show::test::run(cases);
}
