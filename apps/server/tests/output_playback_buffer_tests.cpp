#include "output_playback_buffer.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

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

struct Case final {
    const char *name{};
    void (*function)(){};
};

constexpr std::array<Case, 5U> cases{{
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
