#include <mediaserver/track/pose_channel.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace core = mediaserver::core;
namespace track = mediaserver::track;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define CHANNEL_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] track::StampedPose stamped(float x, core::u64 frame) noexcept {
    track::TrackedPose pose{};
    pose.protocol = track::TrackerProtocol::OpenTrackIO;
    pose.x = x;
    pose.sequence = frame;
    return track::StampedPose{pose, frame};
}

// Latency compensation only means something if a pose stamped forward is kept
// until its frame comes round. Applying it on receipt - which this channel used
// to do, because it held exactly one pose and answered with it whatever frame
// was asked for - cancels precisely the offset the Director just added, and
// makes the authored compensation a number that changes nothing.
void a_pose_stamped_forward_waits_for_its_frame() {
    track::PoseChannel channel;
    // Two frames of compensation: sampled at 100, stamped for 102.
    CHANNEL_CHECK(channel.accept(stamped(1.0F, 102U))
                  == track::PoseChannelOutcome::Accepted);
    CHANNEL_CHECK(channel.pending() == 1U);

    // Frames before it have no pose at all. Not the future pose applied early,
    // and not an invented one: there is genuinely no pose for frame 100 yet.
    CHANNEL_CHECK(channel.resolve(100U).state == track::PoseHold::None);
    CHANNEL_CHECK(channel.resolve(101U).state == track::PoseHold::None);

    // And on its own frame it lands, exactly.
    const auto landed = channel.resolve(102U);
    CHANNEL_CHECK(landed.state == track::PoseHold::Exact);
    CHANNEL_CHECK(landed.pose.x == 1.0F);
    CHANNEL_CHECK(landed.stamped_frame == 102U);
    CHANNEL_CHECK(channel.pending() == 0U);

    // Past its frame it is held, which is the ordinary between-poses answer.
    CHANNEL_CHECK(channel.resolve(103U).state == track::PoseHold::Held);

    // A stream of compensated poses stays two frames ahead without ever
    // running early: each is accepted while the one before it is still the
    // answer.
    CHANNEL_CHECK(channel.accept(stamped(2.0F, 104U))
                  == track::PoseChannelOutcome::Accepted);
    CHANNEL_CHECK(channel.accept(stamped(3.0F, 105U))
                  == track::PoseChannelOutcome::Accepted);
    CHANNEL_CHECK(channel.pending() == 2U);
    const auto still_old = channel.resolve(103U);
    CHANNEL_CHECK(still_old.state == track::PoseHold::Held);
    CHANNEL_CHECK(still_old.pose.x == 1.0F);
    CHANNEL_CHECK(channel.resolve(104U).pose.x == 2.0F);
    CHANNEL_CHECK(channel.resolve(105U).pose.x == 3.0F);

    // Two nodes asking for the same frame get the same pose whatever order
    // they arrived at it, which is the property the wall depends on.
    track::PoseChannel other;
    CHANNEL_CHECK(other.accept(stamped(1.0F, 102U))
                  == track::PoseChannelOutcome::Accepted);
    CHANNEL_CHECK(other.accept(stamped(2.0F, 104U))
                  == track::PoseChannelOutcome::Accepted);
    CHANNEL_CHECK(other.accept(stamped(3.0F, 105U))
                  == track::PoseChannelOutcome::Accepted);
    CHANNEL_CHECK(other.resolve(104U).pose.x == 2.0F);
}

// A consumer that stalls long enough to fill the pending window must degrade to
// holding rather than growing, and must end up holding the newest pose it could
// legitimately have used.
void a_full_pending_window_holds_rather_than_growing() {
    track::PoseChannel channel;
    for (core::u64 frame = 0U; frame < track::max_pending_poses + 4U; ++frame) {
        CHANNEL_CHECK(channel.accept(stamped(static_cast<float>(frame),
                                             1'000U + frame))
                      == track::PoseChannelOutcome::Accepted);
    }
    CHANNEL_CHECK(channel.pending() <= track::max_pending_poses);
    // The evicted poses became the held one rather than vanishing, so a frame
    // before the window still answers with the newest pose that applied to it.
    const auto held = channel.resolve(1'000U);
    CHANNEL_CHECK(held.state != track::PoseHold::None);
    // And the newest pose still lands on its own frame.
    const core::u64 last = 1'000U + track::max_pending_poses + 3U;
    CHANNEL_CHECK(channel.resolve(last).state == track::PoseHold::Exact);
}

void a_node_given_the_pose_for_a_frame_renders_that_frame() {
    track::PoseChannel channel;
    CHANNEL_CHECK(channel.accept(stamped(1.0F, 100U))
                  == track::PoseChannelOutcome::Accepted);

    const auto answer = channel.resolve(100U);
    CHANNEL_CHECK(answer.state == track::PoseHold::Exact);
    CHANNEL_CHECK(answer.pose.x == 1.0F);
    CHANNEL_CHECK(answer.stamped_frame == 100U);
    CHANNEL_CHECK(channel.counters().exact_frames == 1U);
    CHANNEL_CHECK(channel.counters().held_frames == 0U);
}

void a_repeated_frame_holds_the_same_pose() {
    track::PoseChannel channel;
    CHANNEL_CHECK(channel.accept(stamped(1.0F, 100U))
                  == track::PoseChannelOutcome::Accepted);
    // A Director resend for the same show frame, with a different pose in it.
    // The frame already has an answer, so it keeps it.
    CHANNEL_CHECK(channel.accept(stamped(9.0F, 100U))
                  == track::PoseChannelOutcome::Repeated);

    const auto answer = channel.resolve(100U);
    CHANNEL_CHECK(answer.state == track::PoseHold::Exact);
    CHANNEL_CHECK(answer.pose.x == 1.0F);
    CHANNEL_CHECK(channel.counters().repeated == 1U);
}

void a_missing_pose_holds_rather_than_extrapolating() {
    track::PoseChannel channel;
    CHANNEL_CHECK(channel.accept(stamped(1.0F, 100U))
                  == track::PoseChannelOutcome::Accepted);
    CHANNEL_CHECK(channel.accept(stamped(2.0F, 101U))
                  == track::PoseChannelOutcome::Accepted);

    // Frames 102 and 103 arrive with nothing. A channel that extrapolated
    // would answer 3.0 and 4.0 here; two nodes would extrapolate differently
    // and tear the seam between their slices.
    for (core::u64 frame = 102U; frame <= 105U; ++frame) {
        const auto answer = channel.resolve(frame);
        CHANNEL_CHECK(answer.state == track::PoseHold::Held);
        CHANNEL_CHECK(answer.pose.x == 2.0F);
        CHANNEL_CHECK(answer.stamped_frame == 101U);
    }
    CHANNEL_CHECK(channel.counters().held_frames == 4U);
    CHANNEL_CHECK(channel.consecutive_holds() == 4U);
    CHANNEL_CHECK(channel.counters().longest_hold == 4U);

    // The tracker comes back and the hold run ends.
    CHANNEL_CHECK(channel.accept(stamped(7.0F, 106U))
                  == track::PoseChannelOutcome::Accepted);
    const auto recovered = channel.resolve(106U);
    CHANNEL_CHECK(recovered.state == track::PoseHold::Exact);
    CHANNEL_CHECK(recovered.pose.x == 7.0F);
    CHANNEL_CHECK(channel.consecutive_holds() == 0U);
    CHANNEL_CHECK(channel.counters().longest_hold == 4U);
}

void a_stale_or_invalid_sample_is_refused() {
    track::PoseChannel channel;
    CHANNEL_CHECK(channel.accept(stamped(1.0F, 200U))
                  == track::PoseChannelOutcome::Accepted);
    CHANNEL_CHECK(channel.accept(stamped(5.0F, 199U))
                  == track::PoseChannelOutcome::Stale);

    track::StampedPose broken = stamped(5.0F, 201U);
    broken.pose.x = std::nanf("");
    CHANNEL_CHECK(channel.accept(broken)
                  == track::PoseChannelOutcome::Rejected);

    const auto answer = channel.resolve(200U);
    CHANNEL_CHECK(answer.state == track::PoseHold::Exact);
    CHANNEL_CHECK(answer.pose.x == 1.0F);
    CHANNEL_CHECK(channel.counters().stale == 1U);
    CHANNEL_CHECK(channel.counters().rejected == 1U);
    CHANNEL_CHECK(channel.counters().accepted == 1U);
}

void a_channel_that_never_received_anything_answers_nothing() {
    track::PoseChannel channel;
    const auto answer = channel.resolve(1U);
    CHANNEL_CHECK(answer.state == track::PoseHold::None);
    CHANNEL_CHECK(!channel.holding());
    CHANNEL_CHECK(channel.counters().exact_frames == 0U);
    CHANNEL_CHECK(channel.counters().held_frames == 0U);
}

// Two mapped outputs on one node resolve the same show frame twice. That is
// one show frame, not two, or the hold diagnostics would read double on any
// node with more than one output.
void resolving_one_frame_twice_counts_it_once() {
    track::PoseChannel channel;
    CHANNEL_CHECK(channel.accept(stamped(1.0F, 10U))
                  == track::PoseChannelOutcome::Accepted);
    (void)channel.resolve(11U);
    (void)channel.resolve(11U);
    (void)channel.resolve(11U);
    CHANNEL_CHECK(channel.counters().held_frames == 1U);
    CHANNEL_CHECK(channel.consecutive_holds() == 1U);
}

// Two nodes fed the identical stream must answer identically for every frame,
// including the frames neither of them received a pose for.
void two_nodes_fed_the_same_stream_agree_on_every_frame() {
    track::PoseChannel left;
    track::PoseChannel right;
    const std::array<track::StampedPose, 5U> stream{{
        stamped(0.5F, 10U),
        stamped(0.75F, 11U),
        stamped(1.25F, 14U),
        stamped(1.5F, 15U),
        stamped(2.0F, 19U),
    }};

    std::size_t next = 0U;
    for (core::u64 frame = 10U; frame <= 20U; ++frame) {
        while (next < stream.size() && stream[next].show_frame == frame) {
            CHANNEL_CHECK(left.accept(stream[next])
                          == right.accept(stream[next]));
            ++next;
        }
        const auto a = left.resolve(frame);
        const auto b = right.resolve(frame);
        CHANNEL_CHECK(a == b);
        // And in particular the gaps are holds, not guesses.
        if (frame == 12U || frame == 13U) {
            CHANNEL_CHECK(a.state == track::PoseHold::Held);
            CHANNEL_CHECK(a.pose.x == 0.75F);
        }
    }
    CHANNEL_CHECK(left.counters() == right.counters());
}

void a_reset_drops_the_held_pose() {
    track::PoseChannel channel;
    CHANNEL_CHECK(channel.accept(stamped(1.0F, 10U))
                  == track::PoseChannelOutcome::Accepted);
    channel.reset();
    CHANNEL_CHECK(!channel.holding());
    CHANNEL_CHECK(channel.resolve(10U).state == track::PoseHold::None);
    // A pose for an older frame is accepted again, because there is nothing
    // left to be stale against.
    CHANNEL_CHECK(channel.accept(stamped(3.0F, 5U))
                  == track::PoseChannelOutcome::Accepted);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 10U> cases{{
    {"a node given the pose for a frame renders that frame",
     &a_node_given_the_pose_for_a_frame_renders_that_frame},
    {"a repeated frame holds the same pose",
     &a_repeated_frame_holds_the_same_pose},
    {"a missing pose holds rather than extrapolating",
     &a_missing_pose_holds_rather_than_extrapolating},
    {"a stale or invalid sample is refused",
     &a_stale_or_invalid_sample_is_refused},
    {"a channel that never received anything answers nothing",
     &a_channel_that_never_received_anything_answers_nothing},
    {"resolving one frame twice counts it once",
     &resolving_one_frame_twice_counts_it_once},
    {"two nodes fed the same stream agree on every frame",
     &two_nodes_fed_the_same_stream_agree_on_every_frame},
    {"a pose stamped forward waits for its frame",
     &a_pose_stamped_forward_waits_for_its_frame},
    {"a full pending window holds rather than growing",
     &a_full_pending_window_holds_rather_than_growing},
    {"a reset drops the held pose", &a_reset_drops_the_held_pose},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const auto& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
