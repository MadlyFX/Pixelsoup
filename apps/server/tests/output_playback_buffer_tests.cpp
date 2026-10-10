#include "output_playback_buffer.hpp"
#include "output_frame_submission.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace app = mediaserver::app;
namespace cmd = mediaserver::cmd;
namespace core = mediaserver::core;
namespace show = mediaserver::show;

void check(bool condition, const char *expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define BUFFER_CHECK(...)                                                      \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] show::TransportAnchor playing(core::u64 control = 100U,
                                            core::u64 frame = 1'000U) noexcept {
    show::TransportAnchor anchor{};
    anchor.anchor_control_frame = cmd::FrameNumber{control};
    anchor.anchor_show_frame = show::ShowFrame{frame};
    anchor.state = show::TransportState::Playing;
    anchor.direction = show::TransportDirection::Forward;
    anchor.rate = show::FrameRate{24U, 1U};
    anchor.first_frame = show::ShowFrame{0U};
    anchor.last_frame = show::ShowFrame{10'000U};
    return anchor;
}

void playback_waits_for_twelve_frames_of_headroom() {
    app::OutputPlaybackBuffer buffer;
    const auto anchor = playing();
    for (core::u64 control = 100U; control <= 112U; ++control) {
        const auto selected =
            buffer.select(anchor, cmd::FrameNumber{control}, true);
        BUFFER_CHECK(selected.control_frame == cmd::FrameNumber{100U});
        BUFFER_CHECK(selected.show_frame == show::ShowFrame{1'000U});
        buffer.observe(true);
    }

    const auto advanced = buffer.select(anchor, cmd::FrameNumber{113U}, true);
    BUFFER_CHECK(advanced.control_frame == cmd::FrameNumber{101U});
    BUFFER_CHECK(advanced.show_frame == show::ShowFrame{1'001U});
    BUFFER_CHECK(buffer.latency_frames(cmd::FrameNumber{113U}) == 12U);
}

void an_incomplete_frame_grows_latency_instead_of_skipping() {
    app::OutputPlaybackBuffer buffer;
    const auto anchor = playing();
    auto selected = buffer.select(anchor, cmd::FrameNumber{113U}, true);
    BUFFER_CHECK(selected.control_frame == cmd::FrameNumber{101U});
    buffer.observe(false);

    selected = buffer.select(anchor, cmd::FrameNumber{116U}, true);
    BUFFER_CHECK(selected.control_frame == cmd::FrameNumber{101U});
    BUFFER_CHECK(buffer.latency_frames(cmd::FrameNumber{116U}) == 15U);

    buffer.observe(true);
    selected = buffer.select(anchor, cmd::FrameNumber{117U}, true);
    // Recovery advances one ordinary frame, not directly to the newly delayed
    // live position. No show-frame position is silently discarded.
    BUFFER_CHECK(selected.control_frame == cmd::FrameNumber{102U});
    BUFFER_CHECK(selected.show_frame == show::ShowFrame{1'002U});
}

void a_new_anchor_restarts_the_buffer_at_the_new_show_position() {
    app::OutputPlaybackBuffer buffer;
    const auto first = playing();
    static_cast<void>(buffer.select(first, cmd::FrameNumber{140U}, true));
    buffer.observe(true);

    auto seek = playing(200U, 5'000U);
    const auto selected = buffer.select(seek, cmd::FrameNumber{200U}, true);
    BUFFER_CHECK(selected.control_frame == cmd::FrameNumber{200U});
    BUFFER_CHECK(selected.show_frame == show::ShowFrame{5'000U});
}

void reverse_playback_uses_the_same_control_frame_cushion() {
    app::OutputPlaybackBuffer buffer;
    auto anchor = playing(50U, 500U);
    anchor.direction = show::TransportDirection::Reverse;
    const auto selected = buffer.select(anchor, cmd::FrameNumber{70U}, true);
    BUFFER_CHECK(selected.control_frame == cmd::FrameNumber{58U});
    BUFFER_CHECK(selected.show_frame == show::ShowFrame{492U});
}

void a_custom_headroom_changes_latency_without_skipping() {
    app::OutputPlaybackBuffer buffer{3U};
    const auto anchor = playing();
    auto selected = buffer.select(anchor, cmd::FrameNumber{103U}, true);
    BUFFER_CHECK(selected.control_frame == cmd::FrameNumber{100U});
    buffer.observe(true);
    selected = buffer.select(anchor, cmd::FrameNumber{104U}, true);
    BUFFER_CHECK(selected.control_frame == cmd::FrameNumber{101U});
    BUFFER_CHECK(buffer.latency_frames(cmd::FrameNumber{104U}) == 3U);
    BUFFER_CHECK(buffer.headroom_frames() == 3U);
}

void a_prepared_asset_activates_without_getting_stuck_in_decode_headroom() {
    app::OutputPlaybackBuffer buffer;
    const auto anchor = playing();
    auto selected = buffer.select(anchor, cmd::FrameNumber{140}, true);
    BUFFER_CHECK(selected.control_frame == cmd::FrameNumber{128});
    buffer.observe(false); // The new candidate is ready, but publication holds.
    selected = buffer.select(anchor, cmd::FrameNumber{141}, true, cmd::FrameNumber{141});
    BUFFER_CHECK(selected.control_frame == cmd::FrameNumber{141});
    BUFFER_CHECK(selected.show_frame == show::ShowFrame{1041});
    buffer.observe(false); // A busy GPU retries the same immutable snapshot.
    selected = buffer.select(anchor, cmd::FrameNumber{141}, false, cmd::FrameNumber{141});
    BUFFER_CHECK(selected.control_frame == cmd::FrameNumber{141});
    for (core::u64 control = 142; control <= 153; ++control) {
        buffer.observe(true);
        selected = buffer.select(anchor, cmd::FrameNumber{control}, true);
        BUFFER_CHECK(selected.control_frame == cmd::FrameNumber{141});
    }
    buffer.observe(true);
    selected = buffer.select(anchor, cmd::FrameNumber{154}, true);
    BUFFER_CHECK(selected.control_frame == cmd::FrameNumber{142});
    // A missed activation is never reconstructed from a later snapshot.
    selected = buffer.select(anchor, cmd::FrameNumber{160}, true, cmd::FrameNumber{155});
    BUFFER_CHECK(selected.control_frame != cmd::FrameNumber{155});
}

struct Case final {
    const char *name{};
    void (*function)(){};
};

void busy_outputs_retry_without_skipping_or_resubmitting_their_neighbours() {
    app::OutputPlaybackBuffer buffer;
    const auto anchor = playing();
    std::array<app::OutputFrameSubmission, 2> outputs;
    std::array<std::vector<core::u64>, 2> submitted;
    std::array<core::u64, 2> attempts{};
    core::u64 holds{};
    auto selected = buffer.select(anchor, cmd::FrameNumber{140}, true);
    BUFFER_CHECK(selected.show_frame == show::ShowFrame{1028});
    const auto tick = [&](bool busy) {
        bool complete = true;
        const app::OutputFrameKey key{selected.control_frame.value(),
                                     selected.show_frame.value(), 1};
        for (std::size_t index = 0; index < outputs.size(); ++index) {
            const auto status = outputs[index].present(key, [&] {
                ++attempts[index];
                if (index == 1 && busy) return show::PresenterStatus::Deferred;
                submitted[index].push_back(key.show_frame);
                return show::PresenterStatus::Presented;
            }, [&] {
                ++holds;
                return show::PresenterStatus::Skipped;
            });
            complete = complete && (status == show::PresenterStatus::Presented ||
                                    status == show::PresenterStatus::Skipped);
        }
        buffer.observe(complete);
    };

    tick(true);
    for (core::u64 control = 141; control <= 145; ++control) {
        selected = buffer.select(anchor, cmd::FrameNumber{control}, true);
        BUFFER_CHECK(selected.show_frame == show::ShowFrame{1028});
        tick(true);
        selected = buffer.select(anchor, cmd::FrameNumber{control}, false);
        tick(true); // Housekeeping between boundaries cannot acknowledge a miss.
    }
    BUFFER_CHECK(attempts[0] == 1);
    BUFFER_CHECK(submitted[1].empty());
    tick(false);
    const auto recovered_attempts = attempts;
    for (int poll = 0; poll < 5; ++poll) tick(false);
    BUFFER_CHECK(attempts == recovered_attempts);
    BUFFER_CHECK(holds > 0);
    // Even after an overrun, both outputs resume at the next consecutive frame.
    selected = buffer.select(anchor, cmd::FrameNumber{160}, true);
    BUFFER_CHECK(selected.show_frame == show::ShowFrame{1029});
    tick(false);
    BUFFER_CHECK(submitted[0] == submitted[1]);
    BUFFER_CHECK(submitted[0] == std::vector<core::u64>({1028, 1029}));
}

void held_outputs_invalidate_for_edits_assets_and_replacement_windows() {
    app::OutputFrameSubmission output;
    app::OutputFrameKey key{100, 20, 1, 2, 0, 0, 3, 4};
    core::u64 submissions{};
    core::u64 holds{};
    auto next_status = show::PresenterStatus::Presented;
    const auto present = [&] {
        return output.present(key, [&] {
            ++submissions;
            return next_status;
        }, [&] {
            ++holds;
            return show::PresenterStatus::Skipped;
        });
    };
    BUFFER_CHECK(present() == show::PresenterStatus::Presented);
    BUFFER_CHECK(present() == show::PresenterStatus::Skipped);
    BUFFER_CHECK(submissions == 1 && holds == 1);

    ++key.document_version; // A paused edit must replace the held picture.
    next_status = show::PresenterStatus::Deferred;
    BUFFER_CHECK(present() == show::PresenterStatus::Deferred);
    BUFFER_CHECK(present() == show::PresenterStatus::Deferred);
    next_status = show::PresenterStatus::Presented;
    BUFFER_CHECK(present() == show::PresenterStatus::Presented);
    ++key.scene_digest; // Local proxy or stamped camera pose changed.
    BUFFER_CHECK(present() == show::PresenterStatus::Presented);
    ++key.feed_map_digest;
    BUFFER_CHECK(present() == show::PresenterStatus::Presented);
    ++key.asset_manifest_digest;
    BUFFER_CHECK(present() == show::PresenterStatus::Presented);
    ++key.control_frame; // A new activation can have the same paused show frame.
    BUFFER_CHECK(present() == show::PresenterStatus::Presented);
    key.transition_from_digest = 7;
    key.transition_to_digest = 8;
    BUFFER_CHECK(present() == show::PresenterStatus::Presented);
    output.reset();
    BUFFER_CHECK(present() == show::PresenterStatus::Presented);
    BUFFER_CHECK(submissions == 10 && holds == 1);
}

void unsuccessful_submissions_are_never_reused() {
    app::OutputFrameSubmission output;
    const app::OutputFrameKey key{100, 20};
    for (const auto status : {show::PresenterStatus::Deferred,
                              show::PresenterStatus::Failed,
                              show::PresenterStatus::Closed}) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            BUFFER_CHECK(output.present(key, [=] { return status; }, [] {
                BUFFER_CHECK(false && "an unsuccessful candidate cannot hold");
                return show::PresenterStatus::Skipped;
            }) == status);
        }
    }
    // Offscreen submission does not prove that the connector has the image.
    // A temporarily unavailable swapchain must retry the unchanged candidate.
    BUFFER_CHECK(output.present(key, [] { return show::PresenterStatus::Skipped; },
                                [] { return show::PresenterStatus::Failed; }) ==
                 show::PresenterStatus::Skipped);
    BUFFER_CHECK(output.present(key, [] { return show::PresenterStatus::Presented; },
                                [] { return show::PresenterStatus::Failed; }) ==
                 show::PresenterStatus::Presented);
    BUFFER_CHECK(output.present(key, [] { return show::PresenterStatus::Failed; },
                                [] { return show::PresenterStatus::Skipped; }) ==
                 show::PresenterStatus::Skipped);
}

// Walks the buffer one control frame at a time, every frame completing, and
// returns the show frame presented at each step: what the wall shows.
[[nodiscard]] std::vector<core::u64> present(
    app::OutputPlaybackBuffer &buffer,
    const std::vector<std::pair<core::u64, show::TransportAnchor>> &anchors,
    core::u64 from, core::u64 through) {
    std::vector<core::u64> shown;
    for (core::u64 control = from; control <= through; ++control) {
        const show::TransportAnchor *current = &anchors.front().second;
        for (const auto &[arrives, anchor] : anchors) {
            if (arrives <= control) {
                current = &anchor;
            }
        }
        const auto selected =
            buffer.select(*current, cmd::FrameNumber{control}, true);
        shown.push_back(selected.show_frame.value());
        buffer.observe(true);
    }
    return shown;
}

[[nodiscard]] show::TransportAnchor hold(core::u64 control,
                                         core::u64 frame) noexcept {
    auto anchor = playing(control, frame);
    anchor.state = show::TransportState::Paused;
    anchor.scheduled = true;
    return anchor;
}

void a_scheduled_hold_plays_its_buffered_frames_through_to_the_cue() {
    // Playing from frame 0 at control 100; the show reaches its Hold cue on
    // frame 50, presented at control 150, and the hold lands at control 151.
    app::OutputPlaybackBuffer buffer;
    const std::vector<std::pair<core::u64, show::TransportAnchor>> anchors{
        {100U, playing(100U, 0U)}, {151U, hold(151U, 50U)}};
    const auto shown = present(buffer, anchors, 100U, 190U);
    // Every frame from 0 to 50 reaches the wall exactly once, in order, then
    // it holds 50. Nothing is skipped to catch up with the hold.
    std::vector<core::u64> expected;
    for (core::u64 frame = 0U; frame <= 50U; ++frame) {
        expected.push_back(frame);
    }
    std::vector<core::u64> moving;
    for (const core::u64 frame : shown) {
        if (moving.empty() || moving.back() != frame) {
            moving.push_back(frame);
        }
    }
    BUFFER_CHECK(moving == expected);
    BUFFER_CHECK(shown.back() == 50U);
    // Once the cursor reaches the held frame the output follows the stopped
    // show immediately again.
    BUFFER_CHECK(buffer.latency_frames(cmd::FrameNumber{190U}) == 0U);
}

void a_scheduled_loop_wraps_without_dropping_or_freezing_frames() {
    // A section 101..110 looping: the show presents 110 on control 210 and
    // the loop anchor puts it back on 101 at control 211, then again at 221.
    app::OutputPlaybackBuffer buffer;
    auto first_wrap = playing(211U, 101U);
    first_wrap.scheduled = true;
    auto second_wrap = playing(221U, 101U);
    second_wrap.scheduled = true;
    const std::vector<std::pair<core::u64, show::TransportAnchor>> anchors{
        {100U, playing(100U, 0U)},
        {211U, first_wrap},
        {221U, second_wrap}};
    const auto shown = present(buffer, anchors, 100U, 250U);
    // After the initial headroom the wall advances by exactly one frame per
    // control frame, wrapping 110 -> 101 with no repeat and no gap, even
    // though the section is shorter than the headroom.
    std::size_t start = 0U;
    while (shown[start] == 0U) {
        ++start;
    }
    for (std::size_t index = start; index + 1U < shown.size(); ++index) {
        const core::u64 now = shown[index];
        const core::u64 next = shown[index + 1U];
        BUFFER_CHECK(next == now + 1U || (now == 110U && next == 101U));
    }
    BUFFER_CHECK(buffer.latency_frames(cmd::FrameNumber{250U}) == 12U);
}

void a_stalled_output_does_not_play_through_into_a_stopped_show() {
    // Media that never arrives holds the cursor at frame 0 while the show
    // runs on. When the show then holds, the output must show the held frame
    // rather than stay on the stale one waiting to play its backlog through.
    app::OutputPlaybackBuffer buffer;
    auto selected = buffer.select(playing(100U, 0U), cmd::FrameNumber{100U}, true);
    for (core::u64 control = 101U; control <= 200U; ++control) {
        selected = buffer.select(playing(100U, 0U), cmd::FrameNumber{control},
                                 true);
        buffer.observe(false);
    }
    BUFFER_CHECK(selected.show_frame == show::ShowFrame{0U});
    selected = buffer.select(hold(201U, 100U), cmd::FrameNumber{201U}, true);
    BUFFER_CHECK(selected.control_frame == cmd::FrameNumber{201U});
    BUFFER_CHECK(selected.show_frame == show::ShowFrame{100U});

    // And one that stalls part-way through a play-through gives up once it
    // falls past its reach.
    app::OutputPlaybackBuffer stalling;
    static_cast<void>(present(stalling, {{100U, playing(100U, 0U)}}, 100U, 150U));
    selected = stalling.select(hold(151U, 50U), cmd::FrameNumber{151U}, true);
    BUFFER_CHECK(selected.show_frame.value() < 50U);
    for (core::u64 control = 152U; control <= 180U; ++control) {
        selected = stalling.select(hold(151U, 50U), cmd::FrameNumber{control},
                                   true);
        stalling.observe(false);
    }
    BUFFER_CHECK(selected.show_frame == show::ShowFrame{50U});
}

void a_crossfade_travels_with_the_frame_it_belongs_to() {
    // A Loop overlapping into its next pass on control 150: four frames that
    // mix 50.. into 10... The output plays its buffered frames through to it
    // and must mix exactly those four, whichever anchor is newest.
    app::OutputPlaybackBuffer buffer;
    auto overlap = playing(150U, 10U);
    overlap.scheduled = true;
    overlap.blend_frames = 4U;
    overlap.blend_from = show::ShowFrame{50U};
    overlap.blend_moves = true;
    std::size_t mixed{};
    for (core::u64 control = 100U; control <= 180U; ++control) {
        const auto &anchor = control >= 150U ? overlap : playing(100U, 0U);
        const auto selected =
            buffer.select(anchor, cmd::FrameNumber{control}, true);
        buffer.observe(true);
        const core::u64 at = selected.control_frame.value();
        if (at >= 150U && at < 154U) {
            BUFFER_CHECK(selected.blend.has_value());
            BUFFER_CHECK(selected.blend->from ==
                         show::ShowFrame{50U + (at - 150U)});
            ++mixed;
        } else {
            BUFFER_CHECK(!selected.blend.has_value());
        }
    }
    BUFFER_CHECK(mixed == 4U);
}

void an_operator_anchor_still_restarts_immediately() {
    // A pause the operator asked for is answered at once, as before.
    app::OutputPlaybackBuffer buffer;
    static_cast<void>(present(buffer, {{100U, playing(100U, 0U)}}, 100U, 140U));
    auto paused = playing(141U, 41U);
    paused.state = show::TransportState::Paused;
    const auto selected = buffer.select(paused, cmd::FrameNumber{141U}, true);
    BUFFER_CHECK(selected.control_frame == cmd::FrameNumber{141U});
    BUFFER_CHECK(selected.show_frame == show::ShowFrame{41U});
}

constexpr std::array<Case, 14U> cases{{
    {"busy outputs preserve consecutive frames and completed neighbours",
     &busy_outputs_retry_without_skipping_or_resubmitting_their_neighbours},
    {"held outputs update after edits, publication and window replacement",
     &held_outputs_invalidate_for_edits_assets_and_replacement_windows},
    {"unsuccessful submissions cannot acknowledge an output frame",
     &unsuccessful_submissions_are_never_reused},
    {"prepared asset activation bypasses headroom without rewinding",
     &a_prepared_asset_activates_without_getting_stuck_in_decode_headroom},
    {"playback waits for twelve frames of headroom",
     &playback_waits_for_twelve_frames_of_headroom},
    {"an incomplete frame grows latency instead of skipping",
     &an_incomplete_frame_grows_latency_instead_of_skipping},
    {"a new anchor restarts the buffer at the new show position",
     &a_new_anchor_restarts_the_buffer_at_the_new_show_position},
    {"reverse playback uses the same control-frame cushion",
     &reverse_playback_uses_the_same_control_frame_cushion},
    {"custom headroom changes latency without skipping",
     &a_custom_headroom_changes_latency_without_skipping},
    {"a scheduled Hold plays its buffered frames through to the cue",
     &a_scheduled_hold_plays_its_buffered_frames_through_to_the_cue},
    {"a scheduled Loop wraps without dropping or freezing frames",
     &a_scheduled_loop_wraps_without_dropping_or_freezing_frames},
    {"a stalled output does not play through into a stopped show",
     &a_stalled_output_does_not_play_through_into_a_stopped_show},
    {"a crossfade travels with the frame it belongs to",
     &a_crossfade_travels_with_the_frame_it_belongs_to},
    {"an operator anchor still restarts immediately",
     &an_operator_anchor_still_restarts_immediately},
}};

} // namespace

int main() {
    std::size_t failures{};
    for (const auto &entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception &error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
