#include "timeline_playback_cache.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace app = mediaserver::app;
namespace core = mediaserver::core;
namespace scene = mediaserver::scene;

void check(bool condition, const char *expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define CACHE_CHECK(...)                                                       \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

constexpr scene::SceneTime show_rate{0U, 50U, 1U};

[[nodiscard]] scene::ClipPlacement clip(core::i64 start, core::i64 duration,
                                        core::i64 source_in,
                                        core::u32 count = 400U) {
    scene::ClipPlacement placement{};
    placement.source = "plate.mp4";
    placement.sequence_count = count;
    placement.timing.start = start;
    placement.timing.duration = duration;
    placement.timing.source_in = source_in;
    return placement;
}

void ready_source_frames_mark_the_show_frames_that_show_them() {
    // Show 100 shows source 10; 120 is the first frame past the clip.
    const scene::ClipPlacement plate = clip(100, 20, 10);
    const std::vector<core::u32> ready{10U, 11U, 12U, 20U, 29U, 30U};
    const std::array<app::BufferedClip, 1U> clips{{{&plate, ready, 0}}};
    const auto runs = app::buffered_show_frames(clips, show_rate, 90U, 130U);
    const std::vector<app::ShowFrameRun> expected{
        {100U, 102U}, {110U, 110U}, {119U, 119U}};
    CACHE_CHECK(runs == expected);
}

void a_frame_is_buffered_only_when_every_video_on_it_is() {
    const scene::ClipPlacement lower = clip(0, 40, 0);
    const scene::ClipPlacement upper = clip(20, 40, 100);
    // Lower holds sources 0-39; upper holds 100-104 (show 20-24) and 130
    // (show 50), so show 20-24 needs both and 25-39 lacks upper.
    std::vector<core::u32> lower_ready;
    for (core::u32 frame = 0U; frame < 40U; ++frame) {
        lower_ready.push_back(frame);
    }
    const std::vector<core::u32> upper_ready{100U, 101U, 102U, 103U, 104U,
                                             130U};
    const std::array<app::BufferedClip, 2U> clips{
        {{&lower, lower_ready, 0}, {&upper, upper_ready, 0}}};
    const auto runs = app::buffered_show_frames(clips, show_rate, 0U, 70U);
    const std::vector<app::ShowFrameRun> expected{
        {0U, 24U}, {50U, 50U}};
    CACHE_CHECK(runs == expected);

    // An upper clip with nothing decoded holds back every frame it is on.
    const std::array<app::BufferedClip, 2U> cold{
        {{&lower, lower_ready, 0}, {&upper, {}, 0}}};
    const std::vector<app::ShowFrameRun> lower_only{{0U, 19U}};
    CACHE_CHECK(app::buffered_show_frames(cold, show_rate, 0U, 70U) ==
                lower_only);
}

void frames_without_video_and_outside_the_window_are_not_listed() {
    const scene::ClipPlacement plate = clip(10, 5, 0);
    const std::vector<core::u32> ready{0U, 1U, 2U, 3U, 4U};
    const std::array<app::BufferedClip, 1U> clips{{{&plate, ready, 0}}};
    // The window opens two frames into the clip.
    const auto runs = app::buffered_show_frames(clips, show_rate, 12U, 40U);
    const std::vector<app::ShowFrameRun> expected{{12U, 14U}};
    CACHE_CHECK(runs == expected);
    CACHE_CHECK(app::buffered_show_frames({}, show_rate, 0U, 40U).empty());
    CACHE_CHECK(app::buffered_show_frames(clips, show_rate, 40U, 12U).empty());
}

void a_native_rate_clip_buffers_every_show_frame_a_source_frame_covers() {
    // 25 fps source in a 50 fps show: each source frame is on two show
    // frames, so three ready sources cover six.
    scene::ClipPlacement plate = clip(200, 100, 0);
    plate.frame_rate = scene::MediaFrameRate{25U, 1U};
    const std::vector<core::u32> ready{0U, 1U, 2U, 5U};
    const std::array<app::BufferedClip, 1U> clips{{{&plate, ready, 0}}};
    const auto runs = app::buffered_show_frames(clips, show_rate, 190U, 220U);
    const std::vector<app::ShowFrameRun> expected{
        {200U, 205U}, {210U, 211U}};
    CACHE_CHECK(runs == expected);
}

void a_proxy_numbering_offset_reads_the_decoded_files_frames() {
    const scene::ClipPlacement plate = clip(0, 10, 0);
    // The decoded file numbers from 1000; its 1003-1004 are show 3-4.
    const std::vector<core::u32> ready{3U, 1'003U, 1'004U};
    const std::array<app::BufferedClip, 1U> clips{{{&plate, ready, 1'000}}};
    const auto runs = app::buffered_show_frames(clips, show_rate, 0U, 9U);
    const std::vector<app::ShowFrameRun> expected{{3U, 4U}};
    CACHE_CHECK(runs == expected);
}

void an_unplaced_clip_is_on_air_everywhere_and_loops_its_source() {
    // An always-on clip counts from show zero and wraps its 8 frames.
    const scene::ClipPlacement plate = clip(0, 0, 0, 8U);
    const std::vector<core::u32> ready{0U, 6U, 7U};
    const std::array<app::BufferedClip, 1U> clips{{{&plate, ready, 0}}};
    const auto runs = app::buffered_show_frames(clips, show_rate, 4U, 17U);
    const std::vector<app::ShowFrameRun> expected{{6U, 8U}, {14U, 16U}};
    CACHE_CHECK(runs == expected);
}

struct Case final {
    const char *name;
    void (*function)();
};

const std::array<Case, 6U> cases{{
    {"ready source frames mark the show frames that show them",
     &ready_source_frames_mark_the_show_frames_that_show_them},
    {"a frame is buffered only when every video on it is",
     &a_frame_is_buffered_only_when_every_video_on_it_is},
    {"frames without video and outside the window are not listed",
     &frames_without_video_and_outside_the_window_are_not_listed},
    {"a native-rate clip buffers every show frame a source frame covers",
     &a_native_rate_clip_buffers_every_show_frame_a_source_frame_covers},
    {"a proxy numbering offset reads the decoded file's frames",
     &a_proxy_numbering_offset_reads_the_decoded_files_frames},
    {"an unplaced clip is on air everywhere and loops its source",
     &an_unplaced_clip_is_on_air_everywhere_and_loops_its_source},
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
