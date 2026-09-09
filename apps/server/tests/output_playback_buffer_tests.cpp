#include "output_playback_buffer.hpp"
#include "output_frame_submission.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
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

constexpr std::array<Case, 9U> cases{{
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
