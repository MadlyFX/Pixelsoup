#include <mediaserver/render/media_source.hpp>

#include <mediaserver/rhi/device.hpp>

#include "../src/ffmpeg_source_test_support.hpp"
#include "../src/nv12_conversion.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <d3d12.h>
#include <d3d12video.h>
#pragma comment(lib, "dxguid.lib")
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// A real MP4 correctness fixture, kept tiny and synthetic so the test is
// deterministic and redistributable. It contains eight 16x16 MPEG-4 frames at
// 24 fps: red, green, blue, yellow, magenta, cyan, purple, orange. The test
// exercises the same FFmpeg demux/decode path as imported operator media.
namespace {

namespace core = mediaserver::core;
namespace rhi = mediaserver::rhi;
namespace render = mediaserver::render;

void check(bool condition, const char *expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define VIDEO_CHECK(...)                                                       \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

constexpr std::string_view fixture_base64 =
    "AAAAHGZ0eXBpc29tAAACAGlzb21pc28ybXA0MQAAAAhmcmVlAAAAp21kYXQAAAGzABAHAAABth"
    "BgUYVm2EAG4wAAAbMAEAcAAAG2"
    "EOBxrtsEkC1fAAABswAQBwAAAbYRYNGFFtgZ4XMAAAGzABAHAAABthHhEY6bYF2PvwAAAbMAEA"
    "cAAAG2EmERlG2DUDq/AAABswAQ"
    "BwAAAbYS4RGabYbgXj8AAAGzABAHAAABthNhEZBtg1DrAAABswAQBwAAAbYT4RGo2wVAaH8AAA"
    "NRbW9vdgAAAGxtdmhkAAAAAAAA"
    "AAAAAAAAAAAD6AAAAU4AAQAAAQAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAABAAAAAAAAAA"
    "AAAAAAAABAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAgAAAnt0cmFrAAAAXHRraGQAAAADAAAAAAAAAAAAAAABAAAAAA"
    "AAAU4AAAAAAAAAAAAAAAAAAAAA"
    "AAEAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAABAAAAAQAAAAAAAkZWR0cwAAAB"
    "xlbHN0AAAAAAAAAAEAAAFOAAAA"
    "AAABAAAAAAHzbWRpYQAAACBtZGhkAAAAAAAAAAAAAAAAAAAwAAAAEABVxAAAAAAALWhkbHIAAA"
    "AAAAAAAHZpZGUAAAAAAAAAAAAA"
    "AABWaWRlb0hhbmRsZXIAAAABnm1pbmYAAAAUdm1oZAAAAAEAAAAAAAAAAAAAACRkaW5mAAAAHG"
    "RyZWYAAAAAAAAAAQAAAAx1cmwg"
    "AAAAAQAAAV5zdGJsAAAA2nN0c2QAAAAAAAAAAQAAAMptcDR2AAAAAAAAAAEAAAAAAAAAAAAAAA"
    "AAAAAAABAAEABIAAAASAAAAAAA"
    "AAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAGP//"
    "AAAAYGVzZHMAAAAAA4CAgE8AAQAEgICAQSARAAAAAAAO6AAA"
    "AAAFgICALwAAAbABAAABtYkTAAABAAAAASAAxI2IAMUAhAIUYwAAAbJMYXZjNjIuMjguMTAxBo"
    "CAgAECAAAAFGJ0cnQAAAAAAAAO"
    "6AAAAAAAAAAYc3R0cwAAAAAAAAABAAAACAAAAgAAAAAcc3RzYwAAAAAAAAABAAAAAQAAAAgAAA"
    "ABAAAANHN0c3oAAAAAAAAAAAAA"
    "AAgAAAAUAAAAFAAAABQAAAAUAAAAFAAAABQAAAATAAAAFAAAABRzdGNvAAAAAAAAAAEAAAAsAA"
    "AAYnVkdGEAAABabWV0YQAAAAAA"
    "AAAhaGRscgAAAAAAAAAAbWRpcmFwcGwAAAAAAAAAAAAAAAAtaWxzdAAAACWpdG9vAAAAHWRhdG"
    "EAAAABAAAAAExhdmY2Mi4xMi4x"
    "MDE=";

[[nodiscard]] std::vector<core::u8> decode_base64(std::string_view encoded) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<core::u8> bytes;
    bytes.reserve(encoded.size() * 3U / 4U);
    std::uint32_t accumulator = 0U;
    unsigned bits = 0U;
    for (const char value : encoded) {
        if (value == '=') {
            break;
        }
        const auto position = alphabet.find(value);
        if (position == std::string_view::npos) {
            continue;
        }
        accumulator =
            (accumulator << 6U) | static_cast<std::uint32_t>(position);
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            bytes.push_back(static_cast<core::u8>(accumulator >> bits));
            accumulator &= bits == 0U ? 0U : (1U << bits) - 1U;
        }
    }
    return bytes;
}

[[nodiscard]] std::string fixture_path(std::string_view name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

void write_fixture(const std::string &path) {
    const auto bytes = decode_base64(fixture_base64);
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    check(static_cast<bool>(out), "wrote MP4 fixture", __LINE__);
}

// What a decoded frame shows at one pixel, whatever layout it arrived in.
//
// The Rec.709 constants here are written from the specification rather than
// taken from the renderer, so a test that uses this is still checking the
// decoder against an independent expectation. It handles either sample depth
// and any of the chroma extents the decoder produces, because a 4:2:2 source
// no longer becomes 4:2:0 on its way to the GPU.
struct DisplayRgb final {
    int r{};
    int g{};
    int b{};
};

[[nodiscard]] DisplayRgb display_rgb(const render::DecodedFrame &frame,
                                     core::u32 x, core::u32 y) {
    // A frame the decoder left on the renderer's device has no CPU pixels to
    // read. Callers check for that first; this keeps a mistake a failed
    // expectation rather than a crash.
    if (frame.pixels.empty() || x >= frame.width || y >= frame.height) {
        return DisplayRgb{-1, -1, -1};
    }
    if (frame.layout == render::DecodedPixelLayout::Rgba8) {
        const std::size_t at =
            (static_cast<std::size_t>(y) * frame.width + x) * 4U;
        return DisplayRgb{frame.pixels[at], frame.pixels[at + 1U],
                          frame.pixels[at + 2U]};
    }
    const bool eight =
        frame.layout == render::DecodedPixelLayout::Nv12Rec709Limited ||
        frame.layout == render::DecodedPixelLayout::Nv12Rec709Full;
    const std::size_t sample = eight ? 1U : 2U;
    const auto code = [&](std::size_t at) {
        return eight ? static_cast<int>(frame.pixels[at])
                     : static_cast<int>(
                           (static_cast<unsigned>(frame.pixels[at]) |
                            (static_cast<unsigned>(frame.pixels[at + 1U])
                             << 8U)) >> 6U);
    };
    const std::size_t luma_row = static_cast<std::size_t>(frame.width) * sample;
    const std::size_t luma_bytes = luma_row * frame.height;
    const std::size_t chroma_row =
        static_cast<std::size_t>(frame.chroma.width) * 2U * sample;
    const core::u32 cx = x * frame.chroma.width / frame.width;
    const core::u32 cy = y * frame.chroma.height / frame.height;
    const int luma = code(static_cast<std::size_t>(y) * luma_row + x * sample);
    const std::size_t chroma_at =
        luma_bytes + static_cast<std::size_t>(cy) * chroma_row
        + static_cast<std::size_t>(cx) * 2U * sample;
    const int blue_difference = code(chroma_at);
    const int red_difference = code(chroma_at + sample);
    // Legal-range Rec.709: luma 16..235 (64..940 at ten bits), chroma
    // centred on 128 (512) with a 224 (896) excursion.
    const double peak = eight ? 255.0 : 1023.0;
    const double black = eight ? 16.0 : 64.0;
    const double luma_span = eight ? 219.0 : 876.0;
    const double centre = eight ? 128.0 : 512.0;
    const double chroma_span = eight ? 224.0 : 896.0;
    const double luminance = (luma - black) / luma_span;
    const double cb = (blue_difference - centre) / chroma_span;
    const double cr = (red_difference - centre) / chroma_span;
    const auto encode = [peak](double value) {
        const double clamped = value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
        return static_cast<int>(clamped * 255.0 + 0.5);
    };
    static_cast<void>(peak);
    return DisplayRgb{
        encode(luminance + 1.5748 * cr),
        encode(luminance - 0.187324 * cb - 0.468124 * cr),
        encode(luminance + 1.8556 * cb)};
}

[[nodiscard]] std::shared_ptr<const render::DecodedFrame>
await_frame(render::MediaSource &source, const std::string &path,
            core::u32 frame_index, core::u64 stream = 0U,
            std::chrono::milliseconds timeout = std::chrono::seconds{5}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        auto frame = source.try_decode(path, frame_index, stream);
        if (frame != nullptr) {
            return frame;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error{"timed out waiting for decoded MP4 frame"};
}

void a_real_mp4_decodes_exact_frames_in_forward_and_reverse_order() {
    const auto path = fixture_path("mediaserver_async_video_fixture.mp4");
    write_fixture(path);
    const render::MediaProbe metadata = render::probe_media(path);
    VIDEO_CHECK(metadata.frame_count == 8U);
    VIDEO_CHECK(metadata.frame_rate.has_value());
    VIDEO_CHECK(metadata.frame_rate->numerator == 24U);
    VIDEO_CHECK(metadata.frame_rate->denominator == 1U);
    VIDEO_CHECK(metadata.width == 16U && metadata.height == 16U);
    VIDEO_CHECK(std::abs(metadata.aspect() - 1.0F) < 0.001F);
    VIDEO_CHECK(render::probe_frame_count(path) == 8U);
    const auto rate = render::probe_media_frame_rate(path);
    VIDEO_CHECK(rate.has_value());
    VIDEO_CHECK(rate->numerator == 24U);
    VIDEO_CHECK(rate->denominator == 1U);
    const render::MediaDecodeSupport decode_support =
        render::probe_media_decode_support(path);
    VIDEO_CHECK(decode_support == render::MediaDecodeSupport::SoftwareOnly ||
                decode_support ==
                    render::MediaDecodeSupport::HardwareAccelerated);

    auto source = render::make_media_source();
    const auto requested_at = std::chrono::steady_clock::now();
    const auto pending = source->try_decode(path, 0U);
    const auto request_time = std::chrono::steady_clock::now() - requested_at;
    VIDEO_CHECK(pending == nullptr);
    // Queueing must be independent of 4K decode cost. This loose bound catches
    // accidental synchronous regression without being scheduler-sensitive.
    VIDEO_CHECK(request_time < std::chrono::milliseconds{100});

    const auto red = await_frame(*source, path, 0U);
    VIDEO_CHECK(red->valid());
    VIDEO_CHECK(red->width == 16U && red->height == 16U);
    VIDEO_CHECK(display_rgb(*red, 8U, 8U).r > 170);
    VIDEO_CHECK(display_rgb(*red, 8U, 8U).g < 80);

    // Frame 2 is a small forward gap. The decoder must discard frame 1 rather
    // than seek/flush, yet still return the exact blue frame.
    const auto blue = await_frame(*source, path, 2U);
    VIDEO_CHECK(blue->valid());
    VIDEO_CHECK(display_rgb(*blue, 8U, 8U).b > 160);
    VIDEO_CHECK(display_rgb(*blue, 8U, 8U).r < 90);

    const auto yellow = await_frame(*source, path, 3U);
    VIDEO_CHECK(yellow->valid());
    VIDEO_CHECK(display_rgb(*yellow, 8U, 8U).r > 160);
    VIDEO_CHECK(display_rgb(*yellow, 8U, 8U).g > 150);

    // Reverse access still uses a bounded seek and reaches frame 1 exactly.
    const auto green = await_frame(*source, path, 1U);
    VIDEO_CHECK(green->valid());
    VIDEO_CHECK(display_rgb(*green, 8U, 8U).g > 150);
    VIDEO_CHECK(display_rgb(*green, 8U, 8U).r < 90);

    // Another far-forward then reverse-to-zero pair catches a seek that lands
    // later than requested being mislabeled under the requested cache key.
    const auto orange = await_frame(*source, path, 7U);
    VIDEO_CHECK(orange->valid());
    VIDEO_CHECK(display_rgb(*orange, 8U, 8U).r > 160);
    VIDEO_CHECK(display_rgb(*orange, 8U, 8U).g > 70);
    const auto red_again = await_frame(*source, path, 0U);
    VIDEO_CHECK(red_again->valid());
    VIDEO_CHECK(display_rgb(*red_again, 8U, 8U).r > 170);
    VIDEO_CHECK(display_rgb(*red_again, 8U, 8U).g < 80);
}

void decode_support_distinguishes_non_video_and_unavailable_video() {
    VIDEO_CHECK(render::probe_media_decode_support("still.png") ==
                render::MediaDecodeSupport::NotApplicable);
    VIDEO_CHECK(render::probe_media_decode_support(
                    fixture_path("mediaserver_missing_decode_probe.mp4")) ==
                render::MediaDecodeSupport::Unavailable);
}

void two_sources_share_one_in_process_decode() {
    const auto path = fixture_path("mediaserver_shared_video_fixture.mp4");
    write_fixture(path);
    auto program = render::make_media_source();
    auto stage = render::make_media_source();
    const auto before = render::media_decode_stats().completed_frames;

    const auto program_pending = program->try_decode(path, 4U);
    const auto stage_pending = stage->try_decode(path, 4U);
    VIDEO_CHECK(program_pending == nullptr);
    const auto program_frame = await_frame(*program, path, 4U);
    const auto stage_frame = stage_pending != nullptr
                                 ? stage_pending
                                 : await_frame(*stage, path, 4U);
    VIDEO_CHECK(program_frame->valid());
    VIDEO_CHECK(stage_frame->valid());
    VIDEO_CHECK(program_frame.get() == stage_frame.get());
    VIDEO_CHECK(render::media_decode_stats().completed_frames - before == 1U);
}

void ready_frames_report_what_shared_residency_holds() {
    const auto path = fixture_path("mediaserver_ready_frames_fixture.mp4");
    write_fixture(path);
    auto source = render::make_media_source();
    VIDEO_CHECK(await_frame(*source, path, 4U)->valid());
    VIDEO_CHECK(await_frame(*source, path, 7U)->valid());

    const std::array<std::string, 2U> sources{path, "still.png"};
    std::vector<render::MediaReadyFrames> ready;
    // The query never waits on the scheduler, so a test retries it the way
    // the Timeline simply asks again next frame.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!render::media_decode_ready_frames(sources, ready)) {
        VIDEO_CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    VIDEO_CHECK(ready.size() == 2U);
    VIDEO_CHECK(ready[0].video);
    VIDEO_CHECK(std::is_sorted(ready[0].frames.begin(), ready[0].frames.end()));
    VIDEO_CHECK(std::binary_search(ready[0].frames.begin(),
                                   ready[0].frames.end(), 4U));
    VIDEO_CHECK(std::binary_search(ready[0].frames.begin(),
                                   ready[0].frames.end(), 7U));
    // Stills are decoded by each cache, never by the shared video scheduler.
    VIDEO_CHECK(!ready[1].video);
    VIDEO_CHECK(ready[1].frames.empty());
}

void two_playheads_on_one_file_keep_independent_decode_continuity() {
    const auto path =
        fixture_path("mediaserver_independent_video_streams_fixture.mp4");
    write_fixture(path);
    auto source = render::make_media_source();
    const render::MediaDecodeStats before = render::media_decode_stats();
    constexpr core::u64 opening_clip = 101U;
    constexpr core::u64 closing_clip = 202U;

    // These are two simultaneous cuts from opposite ends of one file. They
    // need separate forward-walking decoder state even though an exact frame
    // completed by either remains process-wide shared residency.
    // Admission is try-lock/drop, so retry this test-side observation until
    // both stream identities have been accepted. Production naturally retries
    // from subsequent render boundaries without waiting.
    render::MediaDecodeStats queued{};
    const auto admission_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    do {
        static_cast<void>(source->try_decode(path, 0U, opening_clip));
        static_cast<void>(source->try_decode(path, 6U, closing_clip));
        queued = render::media_decode_stats();
        if (queued.active_streams < before.active_streams + 2U) {
            std::this_thread::yield();
        }
    } while (queued.active_streams < before.active_streams + 2U &&
             std::chrono::steady_clock::now() < admission_deadline);
    VIDEO_CHECK(queued.active_sources == before.active_sources + 1U);
    VIDEO_CHECK(queued.active_streams == before.active_streams + 2U);

    const auto red = await_frame(*source, path, 0U, opening_clip);
    const auto purple = await_frame(*source, path, 6U, closing_clip);
    VIDEO_CHECK(red->valid());
    VIDEO_CHECK(purple->valid());
    VIDEO_CHECK(red->pixels[0U] > 170U && red->pixels[1U] < 80U);
    VIDEO_CHECK(purple->pixels[0U] > 80U && purple->pixels[2U] > 80U);

    // Each playhead then advances locally instead of one decoder seeking from
    // one end of the file to the other on every renderer pass.
    const auto green = await_frame(*source, path, 1U, opening_clip);
    const auto orange = await_frame(*source, path, 7U, closing_clip);
    VIDEO_CHECK(green->valid());
    VIDEO_CHECK(orange->valid());
    VIDEO_CHECK(green->pixels[1U] > 150U);
    VIDEO_CHECK(orange->pixels[0U] > 160U && orange->pixels[1U] > 70U);
}

void speculative_video_frames_can_be_polled_without_current_frame_demand() {
    const auto path = fixture_path("mediaserver_speculative_video_fixture.mp4");
    write_fixture(path);
    auto source = render::make_media_source();
    constexpr core::u64 clip = 8001U;
    std::shared_ptr<const render::DecodedFrame> ready;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    do {
        source->prefetch(path, 2U, clip);
        ready = source->try_prefetched(path, 2U, clip);
        if (!ready) std::this_thread::yield();
    } while (!ready && std::chrono::steady_clock::now() < deadline);
    VIDEO_CHECK(ready && ready->valid());
    VIDEO_CHECK(ready->source_frame == 2U);
    const auto before = render::media_decode_stats().completed_frames;
    const auto demanded = await_frame(*source, path, 2U, clip);
    VIDEO_CHECK(demanded.get() == ready.get());
    VIDEO_CHECK(render::media_decode_stats().completed_frames == before);
}

void a_failed_path_retries_only_after_explicit_refresh() {
    const auto path = fixture_path("mediaserver_replaced_video_fixture.mp4");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    auto source = render::make_media_source();
    const auto failed = await_frame(*source, path, 0U);
    VIDEO_CHECK(!failed->valid());
    const core::u64 completed_after_failure =
        render::media_decode_stats().completed_frames;

    write_fixture(path);
    // Cross the former 250 ms local cooldown. A wall-clock retry regression
    // would now return pending and increment completed_frames; persistent
    // failure remains the same immutable completed answer.
    std::this_thread::sleep_for(std::chrono::milliseconds{400});
    const auto still_failed = await_frame(*source, path, 0U);
    VIDEO_CHECK(still_failed.get() == failed.get());
    VIDEO_CHECK(!still_failed->valid());
    // Repeated current-frame and speculative requests observe the exact same
    // completed failure. Neither wall time nor request count reopens storage.
    bool observed_failure = false;
    for (core::u32 attempt = 0U; attempt < 64U; ++attempt) {
        const auto repeated = source->try_decode(path, 0U);
        if (repeated != nullptr) {
            observed_failure = true;
            VIDEO_CHECK(repeated.get() == failed.get());
            VIDEO_CHECK(!repeated->valid());
        }
        source->prefetch(path, 0U);
    }
    VIDEO_CHECK(observed_failure);
    VIDEO_CHECK(render::media_decode_stats().completed_frames ==
                completed_after_failure);

    // Refresh is an explicit operator/control-path action. It invalidates the
    // process CPU answer and serial decoder; the next render request only
    // enqueues fresh work and still does not decode synchronously.
    VIDEO_CHECK(source->refresh(path));
    VIDEO_CHECK(source->try_decode(path, 0U) == nullptr);
    const auto recovered = await_frame(*source, path, 0U);
    VIDEO_CHECK(recovered->valid());
}

void render_queries_drop_while_the_video_registry_is_busy() {
    const auto path = fixture_path("mediaserver_busy_video_registry.mp4");
    write_fixture(path);
    auto source = render::make_media_source();
    constexpr core::u64 warm_stream = 7001U;
    const auto warm = await_frame(*source, path, 0U, warm_stream);
    VIDEO_CHECK(warm->valid());

    struct Gate final {
        std::mutex mutex;
        std::condition_variable changed;
        bool registry_held{};
        bool release_registry{};
        bool caller_returned{};
        bool forced_release{};
        bool exact_was_null{};
        bool latest_was_null{};
        bool cold_lookup_was_null{};
    } gate;

    std::jthread holder{[&gate] {
        mediaserver::render::detail::hold_video_registry_lock_for_test([&gate] {
            std::unique_lock lock{gate.mutex};
            gate.registry_held = true;
            gate.changed.notify_all();
            if (!gate.changed.wait_for(
                    lock, std::chrono::seconds{2},
                    [&gate] { return gate.release_registry; })) {
                gate.forced_release = true;
            }
        });
    }};
    {
        std::unique_lock lock{gate.mutex};
        VIDEO_CHECK(gate.changed.wait_for(
            lock, std::chrono::seconds{2},
            [&gate] { return gate.registry_held; }));
    }

    std::jthread caller{[&gate, &source, &path] {
        // The warm stream covers ready-frame polling, latest fallback, and
        // prefetch. A new stream identity covers cold worker lookup/admission.
        gate.exact_was_null =
            source->try_decode(path, 0U, warm_stream) == nullptr;
        gate.latest_was_null =
            source->try_decode_latest(path, 1U, warm_stream) == nullptr;
        source->prefetch(path, 1U, warm_stream);
        gate.cold_lookup_was_null =
            source->try_decode(path, 0U, 7002U) == nullptr;
        {
            std::lock_guard lock{gate.mutex};
            gate.caller_returned = true;
        }
        gate.changed.notify_all();
    }};

    bool returned_before_release = false;
    {
        std::unique_lock lock{gate.mutex};
        returned_before_release = gate.changed.wait_for(
            lock, std::chrono::seconds{1},
            [&gate] { return gate.caller_returned; });
        gate.release_registry = true;
        gate.changed.notify_all();
    }
    caller.join();
    holder.join();

    VIDEO_CHECK(returned_before_release);
    VIDEO_CHECK(!gate.forced_release);
    VIDEO_CHECK(gate.exact_was_null);
    VIDEO_CHECK(gate.latest_was_null);
    VIDEO_CHECK(gate.cold_lookup_was_null);

    // Contended cold admission was dropped, not cached as a poisoned stream.
    // Once the registry is available the next boundary admits normally and
    // observes the already resident immutable frame.
    const auto cold = await_frame(*source, path, 0U, 7002U);
    VIDEO_CHECK(cold->valid());
    VIDEO_CHECK(cold.get() == warm.get());
}

void buffered_output_raises_the_decode_cache_without_allocating_it() {
    const render::MediaDecodeStats before = render::media_decode_stats();
    render::configure_buffered_output_media_decode();
    const render::MediaDecodeStats defaults = render::media_decode_stats();
    VIDEO_CHECK(defaults.byte_budget >= before.byte_budget);
    VIDEO_CHECK(defaults.byte_budget >=
                std::size_t{2U} * 1024U * 1024U * 1024U);
    // Configuration changes a ceiling only. Existing decoded residency is not
    // copied or eagerly expanded.
    VIDEO_CHECK(defaults.ready_bytes == before.ready_bytes);
    VIDEO_CHECK(defaults.ready_frames == before.ready_frames);

    constexpr std::size_t selected_bytes = 768U * 1024U * 1024U;
    render::configure_buffered_output_media_decode(
        render::MediaDecodeConfig::buffered_output(6U, selected_bytes, 2U));
    const render::MediaDecodeStats selected = render::media_decode_stats();
    VIDEO_CHECK(selected.byte_budget == selected_bytes);
    VIDEO_CHECK(selected.trail_frames == 2U);
    VIDEO_CHECK(selected.ready_bytes == defaults.ready_bytes);
    VIDEO_CHECK(selected.ready_frames == defaults.ready_frames);
}

void speculative_publication_preserves_current_frames_with_a_full_cache() {
    struct RestoreDecodeLimits {
        ~RestoreDecodeLimits() {
            render::configure_buffered_output_media_decode(
                render::MediaDecodeConfig{512U * 1024U * 1024U, 48U, 6U, 0U});
        }
    } restore_limits;
    render::configure_buffered_output_media_decode(
        render::MediaDecodeConfig{2048U, 2U, 6U, 0U});
    const auto directory = std::filesystem::current_path() / "out" / "codec-fixtures";
    std::filesystem::create_directories(directory);
    const auto path_a = (directory / "cache-demand-a.mp4").string();
    const auto path_b = (directory / "cache-demand-b.mp4").string();
    write_fixture(path_a);
    write_fixture(path_b);
    auto source = render::make_media_source();
    const auto current_a = await_frame(*source, path_a, 0U, 901U);
    const auto current_b = await_frame(*source, path_b, 0U, 902U);
    VIDEO_CHECK(current_a->valid() && current_b->valid());
    const auto before = render::media_decode_stats().completed_frames;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (render::media_decode_stats().completed_frames == before &&
           std::chrono::steady_clock::now() < deadline) {
        source->prefetch(path_a, 1U, 901U);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const auto after = render::media_decode_stats();
    VIDEO_CHECK(after.completed_frames > before);
    VIDEO_CHECK(after.ready_bytes <= 2048U && after.ready_frames <= 2U);
    // Identity proves neither current frame had to be decoded again after
    // the speculative result arrived into an already full two-frame cache.
    VIDEO_CHECK(await_frame(*source, path_a, 0U, 901U).get() == current_a.get());
    VIDEO_CHECK(await_frame(*source, path_b, 0U, 902U).get() == current_b.get());
    VIDEO_CHECK(render::media_decode_stats().completed_frames == after.completed_frames);
}

struct Case final {
    const char *name;
    void (*function)();
};

// Generate tiny intraframe fixtures with the linked codec library, then read
// them through the shipping asynchronous source. No ffmpeg CLI or user media
// is needed, and the expected grey levels come from independently authored
// YUV samples rather than a second invocation of the playback decoder.
struct CodecFixtureWriter final {
    AVFormatContext* format{};
    AVCodecContext* codec{};
    AVFrame* frame{};
    AVPacket* packet{};
    ~CodecFixtureWriter() {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&codec);
        if (format != nullptr) {
            if (format->pb != nullptr) avio_closep(&format->pb);
            avformat_free_context(format);
        }
    }
};

void professional_intraframe_codec_decodes(AVCodecID id, int profile, bool ten_bit,
                                          const char* name) {
    const auto directory = std::filesystem::current_path() / "out" / "codec-fixtures";
    std::filesystem::create_directories(directory);
    const auto path = (directory / (std::string{name} + ".mov")).string();
    {
        CodecFixtureWriter writer;
        const AVCodec* encoder = avcodec_find_encoder(id);
        VIDEO_CHECK(encoder != nullptr);
        writer.codec = avcodec_alloc_context3(encoder);
        VIDEO_CHECK(writer.codec != nullptr);
        // Professional masters are not postage stamps, and the size
        // matters to what this exercises: an untagged source is only read as
        // Rec.709 from 720 lines up, and the ProRes encoder here does not
        // preserve the colour tag the way DNxHD does.
        writer.codec->width = 1'280;
        writer.codec->height = 720;
        writer.codec->time_base = AVRational{1, 24};
        writer.codec->framerate = AVRational{24, 1};
        writer.codec->pix_fmt = ten_bit ? AV_PIX_FMT_YUV422P10LE : AV_PIX_FMT_YUV422P;
        writer.codec->profile = profile;
        if (id == AV_CODEC_ID_DNXHD) {
            // DNxHR selects its profile through the encoder's private AVOption;
            // AVCodecContext::profile alone leaves the DNxHD default active.
            VIDEO_CHECK(av_opt_set_int(writer.codec->priv_data, "profile", profile, 0) >= 0);
        }
        writer.codec->thread_count = 1;
        writer.codec->colorspace = AVCOL_SPC_BT709;
        writer.codec->color_range = AVCOL_RANGE_MPEG;
        VIDEO_CHECK(avcodec_open2(writer.codec, encoder, nullptr) >= 0);
        VIDEO_CHECK(avformat_alloc_output_context2(&writer.format, nullptr, "mov", path.c_str()) >= 0);
        VIDEO_CHECK(writer.format != nullptr);
        AVStream* stream = avformat_new_stream(writer.format, nullptr);
        VIDEO_CHECK(stream != nullptr);
        stream->time_base = writer.codec->time_base;
        stream->avg_frame_rate = writer.codec->framerate;
        VIDEO_CHECK(avcodec_parameters_from_context(stream->codecpar, writer.codec) >= 0);
        VIDEO_CHECK(avio_open(&writer.format->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0);
        VIDEO_CHECK(avformat_write_header(writer.format, nullptr) >= 0);
        writer.frame = av_frame_alloc();
        writer.packet = av_packet_alloc();
        VIDEO_CHECK(writer.frame != nullptr && writer.packet != nullptr);
        writer.frame->format = writer.codec->pix_fmt;
        writer.frame->width = writer.codec->width;
        writer.frame->height = writer.codec->height;
        VIDEO_CHECK(av_frame_get_buffer(writer.frame, 0) >= 0);
        const auto drain = [&] {
            int status{};
            while ((status = avcodec_receive_packet(writer.codec, writer.packet)) == 0) {
                if (writer.packet->duration == 0) writer.packet->duration = 1;
                av_packet_rescale_ts(writer.packet, writer.codec->time_base, stream->time_base);
                writer.packet->stream_index = stream->index;
                VIDEO_CHECK(av_interleaved_write_frame(writer.format, writer.packet) >= 0);
                av_packet_unref(writer.packet);
            }
            VIDEO_CHECK(status == AVERROR(EAGAIN) || status == AVERROR_EOF);
        };
        for (int index = 0; index < 3; ++index) {
            VIDEO_CHECK(av_frame_make_writable(writer.frame) >= 0);
            for (int plane = 0; plane < 3; ++plane) {
                const int width = plane == 0 ? writer.frame->width : writer.frame->width / 2;
                const int level = plane == 0 ? 32 + index * 96 : 128;
                for (int row = 0; row < writer.frame->height; ++row) {
                    auto* pixels = writer.frame->data[plane] + row * writer.frame->linesize[plane];
                    for (int column = 0; column < width; ++column) {
                        if (ten_bit) {
                            pixels[column * 2] = static_cast<core::u8>((level * 4) & 255);
                            pixels[column * 2 + 1] = static_cast<core::u8>((level * 4) >> 8);
                        } else pixels[column] = static_cast<core::u8>(level);
                    }
                }
            }
            writer.frame->pts = index;
            writer.frame->duration = 1;
            VIDEO_CHECK(avcodec_send_frame(writer.codec, writer.frame) >= 0);
            drain();
        }
        VIDEO_CHECK(avcodec_send_frame(writer.codec, nullptr) >= 0);
        drain();
        VIDEO_CHECK(av_write_trailer(writer.format) >= 0);
    }
    const auto probe = render::probe_media(path);
    VIDEO_CHECK(probe.width == 1'280U && probe.height == 720U);
    VIDEO_CHECK(probe.frame_count == 3U);
    VIDEO_CHECK(probe.frame_rate == render::MediaFrameRate{24U, 1U});
    VIDEO_CHECK(probe.codec == (id == AV_CODEC_ID_PRORES ? "prores" : "dnxhd"));
    VIDEO_CHECK(render::probe_media_decode_support(path) == render::MediaDecodeSupport::SoftwareOnly);
    auto source = render::make_media_source();
    // Reverse order checks the decoder's frame-exact seek as well as ingestion.
    for (const core::u32 index : {2U, 0U, 1U}) {
        const auto frame = await_frame(*source, path, index);
        VIDEO_CHECK(frame->valid() && frame->width == 1'280U
                    && frame->height == 720U);
        VIDEO_CHECK(frame->source_frame == index);
        // These are 4:2:2 masters and reach the renderer as two planes at
        // their own depth: ten bits stay ten bits, and the chroma keeps its
        // full height instead of being resampled to 4:2:0 on the way.
        VIDEO_CHECK(frame->layout
                    == (ten_bit ? render::DecodedPixelLayout::P010Rec709Limited
                                : render::DecodedPixelLayout::Nv12Rec709Limited));
        VIDEO_CHECK(frame->chroma.width == 640U);
        VIDEO_CHECK(frame->chroma.height == 720U);
        VIDEO_CHECK(frame->pixels.size()
                    == (ten_bit ? 1'280U * 720U * 4U : 1'280U * 720U * 2U));
        const int expected = static_cast<int>((16U + index * 96U) * 255U / 219U);
        const int actual = display_rgb(*frame, 128U, 64U).r;
        VIDEO_CHECK(std::abs(expected - actual) <= 4);
    }
    // Held at half resolution, the same frame arrives at half size in both
    // planes - 4:2:2 chroma stays full height relative to the smaller luma -
    // and a flat picture keeps its level.
    const auto half = await_frame(
        *source, render::half_resolution_source(path), 1U);
    VIDEO_CHECK(half->valid() && half->width == 640U && half->height == 360U);
    VIDEO_CHECK(half->source_frame == 1U);
    VIDEO_CHECK(half->chroma.width == 320U && half->chroma.height == 360U);
    VIDEO_CHECK(half->pixels.size()
                == (ten_bit ? 640U * 360U * 4U : 640U * 360U * 2U));
    const int expected_half = static_cast<int>((16U + 96U) * 255U / 219U);
    VIDEO_CHECK(std::abs(expected_half - display_rgb(*half, 64U, 32U).r) <= 4);
}

// The system-memory halving, against hand-worked means. Each output sample is
// the 2x2 block's mean rounded half up; an odd extent averages its last row
// or column with itself; chroma is halved at its own extent.
void halving_averages_each_two_by_two_block() {
    render::DecodedFrame nv12{};
    nv12.width = 4U;
    nv12.height = 3U;
    nv12.layout = render::DecodedPixelLayout::Nv12Rec709Limited;
    nv12.chroma = render::ChromaExtent{2U, 2U};
    nv12.pixels = {
        10U, 20U, 30U, 41U,
        50U, 60U, 70U, 80U,
        90U, 91U, 200U, 203U,
        // Chroma: two rows of two U/V pairs.
        100U, 200U, 103U, 201U,
        110U, 10U, 111U, 12U};
    VIDEO_CHECK(render::detail::halve_decoded_frame(nv12));
    VIDEO_CHECK(nv12.width == 2U && nv12.height == 2U);
    VIDEO_CHECK(nv12.chroma == (render::ChromaExtent{1U, 1U}));
    // 10+20+50+60=140 -> 35; 30+41+70+80=221 -> 55 (55.25);
    // 90+91+90+91=362 -> 91 (90.5 rounds up); 200+203+200+203=806 -> 202.
    // U: 100+103+110+111=424 -> 106; V: 200+201+10+12=423 -> 106 (105.75).
    const std::vector<core::u8> expected_nv12{35U, 55U, 91U, 202U, 106U, 106U};
    VIDEO_CHECK(nv12.pixels == expected_nv12);

    render::DecodedFrame p010{};
    p010.width = 2U;
    p010.height = 2U;
    p010.layout = render::DecodedPixelLayout::P010Rec709Full;
    p010.chroma = render::ChromaExtent{1U, 1U};
    const std::array<core::u16, 6U> samples{
        {0xffc0U, 0x0040U, 0x8000U, 0x8001U, 0x1234U, 0xfedcU}};
    for (const core::u16 sample : samples) {
        p010.pixels.push_back(static_cast<core::u8>(sample & 0xffU));
        p010.pixels.push_back(static_cast<core::u8>(sample >> 8U));
    }
    VIDEO_CHECK(render::detail::halve_decoded_frame(p010));
    VIDEO_CHECK(p010.width == 1U && p010.height == 1U);
    // (0xffc0 + 0x0040 + 0x8000 + 0x8001 + 2) / 4 = 0x8000 (0x20003 / 4).
    // A 1x1 chroma plane averages its one pair with itself.
    const std::vector<core::u8> expected_p010{
        0x00U, 0x80U, 0x34U, 0x12U, 0xdcU, 0xfeU};
    VIDEO_CHECK(p010.pixels == expected_p010);

    render::DecodedFrame rgba{};
    rgba.width = 2U;
    rgba.height = 1U;
    rgba.pixels = {0U, 10U, 255U, 255U, 1U, 20U, 0U, 255U};
    VIDEO_CHECK(render::detail::halve_decoded_frame(rgba));
    VIDEO_CHECK(rgba.width == 1U && rgba.height == 1U);
    const std::vector<core::u8> expected_rgba{1U, 15U, 128U, 255U};
    VIDEO_CHECK(rgba.pixels == expected_rgba);

    render::DecodedFrame linear{};
    linear.width = 2U;
    linear.height = 2U;
    linear.layout = render::DecodedPixelLayout::Rgba16FloatLinear;
    linear.pixels.assign(2U * 2U * 8U, 0U);
    VIDEO_CHECK(!render::detail::halve_decoded_frame(linear));
    VIDEO_CHECK(linear.width == 2U);
}

// A high-definition source that carries no colour description at all, which
// is the normal state of a camera original. The decoder has to read it as
// Rec.709 limited: libswscale's own default is Rec.601, and the two matrices
// put red and green far enough apart to tell which one ran.
void untagged_high_definition_is_read_as_rec709() {
    const auto directory =
        std::filesystem::current_path() / "out" / "codec-fixtures";
    std::filesystem::create_directories(directory);
    const auto path = (directory / "untagged-hd.mov").string();
    // Deliberately off-neutral chroma. Independently computed Rec.709 limited
    // display values for Y'CbCr 100, 60, 200 (8-bit codes, written here at ten
    // bits): R 227, G 74, B 0 - against Rec.601's R 213, G 66, B 0.
    constexpr int luma_code = 400;
    constexpr int blue_code = 240;
    constexpr int red_code = 800;
    {
        CodecFixtureWriter writer;
        const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_PRORES);
        VIDEO_CHECK(encoder != nullptr);
        writer.codec = avcodec_alloc_context3(encoder);
        VIDEO_CHECK(writer.codec != nullptr);
        writer.codec->width = 1'280;
        writer.codec->height = 720;
        writer.codec->time_base = AVRational{1, 24};
        writer.codec->framerate = AVRational{24, 1};
        writer.codec->pix_fmt = AV_PIX_FMT_YUV422P10LE;
        writer.codec->profile = 2;
        writer.codec->thread_count = 1;
        // The point of the fixture: nothing is said about colour.
        writer.codec->colorspace = AVCOL_SPC_UNSPECIFIED;
        writer.codec->color_range = AVCOL_RANGE_UNSPECIFIED;
        VIDEO_CHECK(avcodec_open2(writer.codec, encoder, nullptr) >= 0);
        VIDEO_CHECK(avformat_alloc_output_context2(
            &writer.format, nullptr, "mov", path.c_str()) >= 0);
        VIDEO_CHECK(writer.format != nullptr);
        AVStream* stream = avformat_new_stream(writer.format, nullptr);
        VIDEO_CHECK(stream != nullptr);
        stream->time_base = writer.codec->time_base;
        stream->avg_frame_rate = writer.codec->framerate;
        VIDEO_CHECK(
            avcodec_parameters_from_context(stream->codecpar, writer.codec) >= 0);
        VIDEO_CHECK(avio_open(&writer.format->pb, path.c_str(),
                              AVIO_FLAG_WRITE) >= 0);
        VIDEO_CHECK(avformat_write_header(writer.format, nullptr) >= 0);
        writer.frame = av_frame_alloc();
        writer.packet = av_packet_alloc();
        VIDEO_CHECK(writer.frame != nullptr && writer.packet != nullptr);
        writer.frame->format = writer.codec->pix_fmt;
        writer.frame->width = writer.codec->width;
        writer.frame->height = writer.codec->height;
        VIDEO_CHECK(av_frame_get_buffer(writer.frame, 0) >= 0);
        VIDEO_CHECK(av_frame_make_writable(writer.frame) >= 0);
        for (int plane = 0; plane < 3; ++plane) {
            const int width = plane == 0 ? writer.frame->width
                                         : writer.frame->width / 2;
            const int level = plane == 0 ? luma_code
                            : plane == 1 ? blue_code
                                         : red_code;
            for (int row = 0; row < writer.frame->height; ++row) {
                auto* pixels =
                    writer.frame->data[plane] + row * writer.frame->linesize[plane];
                for (int column = 0; column < width; ++column) {
                    pixels[column * 2] = static_cast<core::u8>(level & 255);
                    pixels[column * 2 + 1] = static_cast<core::u8>(level >> 8);
                }
            }
        }
        writer.frame->pts = 0;
        writer.frame->duration = 1;
        VIDEO_CHECK(avcodec_send_frame(writer.codec, writer.frame) >= 0);
        VIDEO_CHECK(avcodec_send_frame(writer.codec, nullptr) >= 0);
        int status{};
        while ((status = avcodec_receive_packet(writer.codec, writer.packet))
               == 0) {
            if (writer.packet->duration == 0) writer.packet->duration = 1;
            av_packet_rescale_ts(writer.packet, writer.codec->time_base,
                                 stream->time_base);
            writer.packet->stream_index = stream->index;
            VIDEO_CHECK(
                av_interleaved_write_frame(writer.format, writer.packet) >= 0);
            av_packet_unref(writer.packet);
        }
        VIDEO_CHECK(status == AVERROR_EOF);
        VIDEO_CHECK(av_write_trailer(writer.format) >= 0);
    }

    auto source = render::make_media_source();
    const auto frame = await_frame(*source, path, 0U);
    VIDEO_CHECK(frame->valid());
    VIDEO_CHECK(frame->width == 1'280U && frame->height == 720U);
    // ProRes has no hardware path here, so this is the software conversion -
    // the one libswscale would otherwise do with the wrong matrix, into the
    // eight-bit interleaved buffer it used to be the only destination for.
    VIDEO_CHECK(frame->layout
                == render::DecodedPixelLayout::P010Rec709Limited);
    VIDEO_CHECK(frame->chroma.width == 640U && frame->chroma.height == 720U);
    const DisplayRgb centre = display_rgb(*frame, 640U, 360U);
    VIDEO_CHECK(std::abs(centre.r - 227) <= 4);
    VIDEO_CHECK(std::abs(centre.g - 74) <= 4);
    VIDEO_CHECK(centre.b <= 4);
}

void prores_422_decodes_exact_frames() {
    professional_intraframe_codec_decodes(AV_CODEC_ID_PRORES, 2, true, "prores-422");
}
void dnxhr_hqx_decodes_exact_frames() {
    professional_intraframe_codec_decodes(AV_CODEC_ID_DNXHD, AV_PROFILE_DNXHR_HQX, true, "dnxhr-hqx");
}
void dnxhr_lb_decodes_exact_frames() {
    professional_intraframe_codec_decodes(AV_CODEC_ID_DNXHD, AV_PROFILE_DNXHR_LB, false, "dnxhr-lb");
}

// A frame a hardware decoder produced on the renderer's own device is handed
// over as that surface, not copied back through system memory. The assertion
// is that nothing arrived in `pixels` and that the device can actually read
// the surface it was given as two planes - a handle that cannot be sampled
// would be worse than a copy, not better.
//
// Runs last, deliberately: offering a decode device is process-wide and
// sticky, and every test above reads decoded pixels out of system memory.
// A device video could be decoded onto, or nothing on a machine without one.
[[nodiscard]] std::optional<rhi::Device> create_decode_device() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return std::nullopt;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto created = rhi::Device::create(config);
    if (!created || !created.value().capabilities().external_memory) {
        return std::nullopt;
    }
    return std::move(created).value();
}

// Eight distinct 1280x720 H.264 frames under `name`, or nothing where this
// build cannot write them. Each test names its own file: decoded frames are
// shared process-wide by path, and one test must not be handed another's.
[[nodiscard]] std::optional<std::string>
write_device_fixture(std::string_view file_name) {
    // The same encoder preference the proxy service uses. A build with none of
    // them cannot write the fixture, and there is nothing to test.
    constexpr std::array<const char*, 5U> encoders{{
        "h264_nvenc", "h264_mf", "libx264", "libopenh264", "h264"}};
    const AVCodec* encoder = nullptr;
    for (const char* name : encoders) {
        encoder = avcodec_find_encoder_by_name(name);
        if (encoder != nullptr) {
            break;
        }
    }
    if (encoder == nullptr) {
        return std::nullopt;
    }

    const auto directory =
        std::filesystem::current_path() / "out" / "codec-fixtures";
    std::filesystem::create_directories(directory);
    const auto path = (directory / file_name).string();
    {
        CodecFixtureWriter writer;
        writer.codec = avcodec_alloc_context3(encoder);
        VIDEO_CHECK(writer.codec != nullptr);
        writer.codec->width = 1'280;
        writer.codec->height = 720;
        writer.codec->time_base = AVRational{1, 24};
        writer.codec->framerate = AVRational{24, 1};
        writer.codec->pix_fmt = AV_PIX_FMT_YUV420P;
        writer.codec->gop_size = 4;
        writer.codec->max_b_frames = 0;
        writer.codec->colorspace = AVCOL_SPC_BT709;
        writer.codec->color_range = AVCOL_RANGE_MPEG;
        if (avcodec_open2(writer.codec, encoder, nullptr) < 0) {
            return std::nullopt;
        }
        VIDEO_CHECK(avformat_alloc_output_context2(
            &writer.format, nullptr, "mp4", path.c_str()) >= 0);
        AVStream* stream = avformat_new_stream(writer.format, nullptr);
        VIDEO_CHECK(stream != nullptr);
        stream->time_base = writer.codec->time_base;
        stream->avg_frame_rate = writer.codec->framerate;
        VIDEO_CHECK(avcodec_parameters_from_context(
            stream->codecpar, writer.codec) >= 0);
        VIDEO_CHECK(
            avio_open(&writer.format->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0);
        VIDEO_CHECK(avformat_write_header(writer.format, nullptr) >= 0);
        writer.frame = av_frame_alloc();
        writer.packet = av_packet_alloc();
        VIDEO_CHECK(writer.frame != nullptr && writer.packet != nullptr);
        writer.frame->format = writer.codec->pix_fmt;
        writer.frame->width = writer.codec->width;
        writer.frame->height = writer.codec->height;
        VIDEO_CHECK(av_frame_get_buffer(writer.frame, 0) >= 0);
        const auto drain = [&] {
            int status{};
            while ((status = avcodec_receive_packet(
                        writer.codec, writer.packet)) == 0) {
                if (writer.packet->duration == 0) writer.packet->duration = 1;
                av_packet_rescale_ts(writer.packet, writer.codec->time_base,
                                     stream->time_base);
                writer.packet->stream_index = stream->index;
                VIDEO_CHECK(av_interleaved_write_frame(
                    writer.format, writer.packet) >= 0);
                av_packet_unref(writer.packet);
            }
            VIDEO_CHECK(status == AVERROR(EAGAIN) || status == AVERROR_EOF);
        };
        for (int index = 0; index < 8; ++index) {
            VIDEO_CHECK(av_frame_make_writable(writer.frame) >= 0);
            for (int plane = 0; plane < 3; ++plane) {
                const int rows = plane == 0 ? writer.frame->height
                                            : writer.frame->height / 2;
                const int columns = plane == 0 ? writer.frame->width
                                               : writer.frame->width / 2;
                for (int row = 0; row < rows; ++row) {
                    auto *const pixels = writer.frame->data[plane]
                        + row * writer.frame->linesize[plane];
                    for (int column = 0; column < columns; ++column) {
                        pixels[column] = static_cast<core::u8>(
                            plane == 0
                                ? 40 + index * 20 + (column * 3 + row * 5) % 120
                                : 96 + (column + row * plane) % 64);
                    }
                }
            }
            writer.frame->pts = index;
            writer.frame->duration = 1;
            VIDEO_CHECK(avcodec_send_frame(writer.codec, writer.frame) >= 0);
            drain();
        }
        VIDEO_CHECK(avcodec_send_frame(writer.codec, nullptr) >= 0);
        drain();
        VIDEO_CHECK(av_write_trailer(writer.format) >= 0);
    }
    return path;
}

void a_decoded_frame_stays_on_the_renderers_device() {
    auto made = create_decode_device();
    if (!made.has_value()) {
        return;
    }
    rhi::Device &device = *made;
    const auto written = write_device_fixture("device-resident-hd.mp4");
    if (!written.has_value()) {
        return;
    }
    const std::string &path = *written;

    const auto offer = render::offer_video_decode_device(device);
    auto source = render::make_media_source();
    const auto frame = await_frame(*source, path, 0U);
    VIDEO_CHECK(frame->valid());
    if (render::probe_media_decode_support(path)
        != render::MediaDecodeSupport::HardwareAccelerated) {
        // No hardware decoder for this stream on this machine: the software
        // path is correct and nothing about the device route applies.
        return;
    }
    VIDEO_CHECK(frame->width == 1'280U && frame->height == 720U);
    VIDEO_CHECK(frame->pixels.empty());
    VIDEO_CHECK(frame->gpu.has_value());
    VIDEO_CHECK(frame->gpu->device == device.native_handle());
    VIDEO_CHECK(frame->gpu->resource.value != nullptr);
    VIDEO_CHECK(frame->gpu->retained != nullptr);
    VIDEO_CHECK(frame->gpu->bytes == 1'280U * 720U * 3U / 2U);
    VIDEO_CHECK(frame->layout
                == render::DecodedPixelLayout::Nv12Rec709Limited);
    auto planes = device.import_video_planes(
        frame->gpu->resource, frame->width, frame->height,
        rhi::TextureFormat::R8Unorm, rhi::TextureFormat::Rg8Unorm);
    VIDEO_CHECK(planes.has_value());
    VIDEO_CHECK(planes.value().luma.width() == 1'280U
                && planes.value().luma.height() == 720U);
    VIDEO_CHECK(planes.value().chroma.width() == 640U
                && planes.value().chroma.height() == 360U);
    // An eight-bit surface read as ten-bit planes is a different picture, not
    // a rougher one, so the depths have to agree.
    VIDEO_CHECK(!device.import_video_planes(
        frame->gpu->resource, frame->width, frame->height,
        rhi::TextureFormat::R16Unorm, rhi::TextureFormat::Rg16Unorm));

    // Held at half resolution, the frame is halved on the same device into a
    // surface of its own - not the decoder's - and adopted the same way.
    const auto half = await_frame(
        *source, render::half_resolution_source(path), 0U);
    VIDEO_CHECK(half->valid() && half->gpu.has_value());
    VIDEO_CHECK(half->width == 640U && half->height == 360U);
    VIDEO_CHECK(half->source_frame == 0U);
    VIDEO_CHECK(half->gpu->device == device.native_handle());
    VIDEO_CHECK(half->gpu->resource.value != frame->gpu->resource.value);
    VIDEO_CHECK(half->gpu->bytes == 640U * 360U * 3U / 2U);
    VIDEO_CHECK(half->layout == render::DecodedPixelLayout::Nv12Rec709Limited);
    VIDEO_CHECK(half->chroma == (render::ChromaExtent{320U, 180U}));
    auto half_planes = device.import_video_planes(
        half->gpu->resource, half->width, half->height,
        rhi::TextureFormat::R8Unorm, rhi::TextureFormat::Rg8Unorm);
    VIDEO_CHECK(half_planes.has_value());
    VIDEO_CHECK(half_planes.value().chroma.width() == 320U
                && half_planes.value().chroma.height() == 180U);

    // Its luma is exactly the 2x2 mean of the full frame's, rounded half up:
    // the same bytes the system-memory path produces, so a node is not
    // shown a different picture for decoding in a different place.
    const auto read_luma = [&](const rhi::Texture &luma, core::u32 width,
                               core::u32 height) {
        auto readback = device.create_readback(width * height);
        VIDEO_CHECK(readback.has_value());
        auto acquired = device.acquire_commands();
        VIDEO_CHECK(acquired.has_value());
        auto commands = std::move(acquired).value();
        VIDEO_CHECK(commands.download(luma, readback.value()));
        auto submitted = device.submit(std::move(commands));
        VIDEO_CHECK(submitted.has_value());
        VIDEO_CHECK(std::move(submitted).value().wait());
        const auto bytes = readback.value().map();
        VIDEO_CHECK(bytes.size() >= static_cast<std::size_t>(width) * height);
        std::vector<core::u8> result(bytes.begin(),
                                     bytes.begin() + width * height);
        readback.value().unmap();
        return result;
    };
    const auto full_luma = read_luma(planes.value().luma, 1'280U, 720U);
    const auto half_luma = read_luma(half_planes.value().luma, 640U, 360U);
    std::size_t mismatched = 0U;
    for (core::u32 y = 0U; y < 360U; ++y) {
        for (core::u32 x = 0U; x < 640U; ++x) {
            const auto at = [&](core::u32 column, core::u32 row) {
                return static_cast<core::u32>(
                    full_luma[static_cast<std::size_t>(row) * 1'280U + column]);
            };
            const core::u32 mean = (at(x * 2U, y * 2U) + at(x * 2U + 1U, y * 2U)
                + at(x * 2U, y * 2U + 1U) + at(x * 2U + 1U, y * 2U + 1U) + 2U)
                / 4U;
            if (half_luma[static_cast<std::size_t>(y) * 640U + x] != mean) {
                ++mismatched;
            }
        }
    }
    VIDEO_CHECK(mismatched == 0U);
}

// Ten-bit video keeps its depth all the way to the renderer: two sixteen-bit
// planes, not the eight-bit pair the more common NV12 case uses and not the
// eight-bit interleaved buffer every software conversion used to produce.
//
// Which route carries it depends on the machine. Where a hardware decoder
// puts a P010 surface on the renderer's own device the frame is adopted
// there; otherwise it arrives in system memory. Both are checked, so the
// depth is asserted either way instead of the test quietly passing on a
// machine that takes the other route.
void ten_bit_video_keeps_its_depth() {
    constexpr std::array<const char*, 4U> encoders{{
        "hevc_nvenc", "hevc_mf", "libx265", "hevc"}};
    const AVCodec* encoder = nullptr;
    for (const char* name : encoders) {
        encoder = avcodec_find_encoder_by_name(name);
        if (encoder != nullptr) {
            break;
        }
    }
    if (encoder == nullptr) {
        return;
    }
    const auto directory =
        std::filesystem::current_path() / "out" / "codec-fixtures";
    std::filesystem::create_directories(directory);
    const auto path = (directory / "device-resident-hd10.mp4").string();
    {
        CodecFixtureWriter writer;
        writer.codec = avcodec_alloc_context3(encoder);
        VIDEO_CHECK(writer.codec != nullptr);
        writer.codec->width = 1'280;
        writer.codec->height = 720;
        writer.codec->time_base = AVRational{1, 24};
        writer.codec->framerate = AVRational{24, 1};
        writer.codec->pix_fmt = AV_PIX_FMT_P010LE;
        writer.codec->gop_size = 4;
        writer.codec->max_b_frames = 0;
        writer.codec->colorspace = AVCOL_SPC_BT709;
        writer.codec->color_range = AVCOL_RANGE_MPEG;
        if (avcodec_open2(writer.codec, encoder, nullptr) < 0) {
            return;
        }
        VIDEO_CHECK(avformat_alloc_output_context2(
            &writer.format, nullptr, "mp4", path.c_str()) >= 0);
        AVStream* stream = avformat_new_stream(writer.format, nullptr);
        VIDEO_CHECK(stream != nullptr);
        stream->time_base = writer.codec->time_base;
        stream->avg_frame_rate = writer.codec->framerate;
        VIDEO_CHECK(avcodec_parameters_from_context(
            stream->codecpar, writer.codec) >= 0);
        VIDEO_CHECK(
            avio_open(&writer.format->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0);
        VIDEO_CHECK(avformat_write_header(writer.format, nullptr) >= 0);
        writer.frame = av_frame_alloc();
        writer.packet = av_packet_alloc();
        VIDEO_CHECK(writer.frame != nullptr && writer.packet != nullptr);
        writer.frame->format = writer.codec->pix_fmt;
        writer.frame->width = writer.codec->width;
        writer.frame->height = writer.codec->height;
        VIDEO_CHECK(av_frame_get_buffer(writer.frame, 0) >= 0);
        const auto drain = [&] {
            int status{};
            while ((status = avcodec_receive_packet(
                        writer.codec, writer.packet)) == 0) {
                if (writer.packet->duration == 0) writer.packet->duration = 1;
                av_packet_rescale_ts(writer.packet, writer.codec->time_base,
                                     stream->time_base);
                writer.packet->stream_index = stream->index;
                VIDEO_CHECK(av_interleaved_write_frame(
                    writer.format, writer.packet) >= 0);
                av_packet_unref(writer.packet);
            }
            VIDEO_CHECK(status == AVERROR(EAGAIN) || status == AVERROR_EOF);
        };
        for (int index = 0; index < 8; ++index) {
            VIDEO_CHECK(av_frame_make_writable(writer.frame) >= 0);
            // P010 keeps ten bits in the high bits of each sixteen-bit word.
            for (int plane = 0; plane < 2; ++plane) {
                const int rows = plane == 0 ? writer.frame->height
                                            : writer.frame->height / 2;
                const auto level = static_cast<core::u16>(
                    (plane == 0 ? 160 + index * 80 : 512) << 6U);
                for (int row = 0; row < rows; ++row) {
                    auto* pixels = writer.frame->data[plane]
                        + row * writer.frame->linesize[plane];
                    for (int word = 0; word < writer.frame->width; ++word) {
                        pixels[word * 2] = static_cast<core::u8>(level & 0xffU);
                        pixels[word * 2 + 1] =
                            static_cast<core::u8>(level >> 8U);
                    }
                }
            }
            writer.frame->pts = index;
            writer.frame->duration = 1;
            VIDEO_CHECK(avcodec_send_frame(writer.codec, writer.frame) >= 0);
            drain();
        }
        VIDEO_CHECK(avcodec_send_frame(writer.codec, nullptr) >= 0);
        drain();
        VIDEO_CHECK(av_write_trailer(writer.format) >= 0);
    }

    auto source = render::make_media_source();
    const auto frame = await_frame(*source, path, 0U);
    VIDEO_CHECK(frame->valid());
    VIDEO_CHECK(frame->width == 1'280U && frame->height == 720U);
    // Ten bits, whichever route carried it, and 4:2:0 as the source was.
    VIDEO_CHECK(frame->layout
                == render::DecodedPixelLayout::P010Rec709Limited);
    VIDEO_CHECK(frame->chroma.width == 640U && frame->chroma.height == 360U);

    if (!frame->gpu.has_value()) {
        // System memory: three bytes a pixel, which is two per luma sample
        // plus a quarter-sized interleaved chroma plane. Four would be the
        // eight-bit interleaved buffer this used to produce.
        VIDEO_CHECK(frame->pixels.size() == 1'280U * 720U * 3U);
        // The authored picture is a flat mid grey with neutral chroma.
        const DisplayRgb shown = display_rgb(*frame, 640U, 360U);
        VIDEO_CHECK(std::abs(shown.r - shown.g) <= 3
                    && std::abs(shown.g - shown.b) <= 3);
        VIDEO_CHECK(shown.r > 20 && shown.r < 250);
        return;
    }
    // Adopted on the renderer's device: nothing came back through system
    // memory, and the surface reads as two sixteen-bit planes.
    VIDEO_CHECK(frame->pixels.empty());
    VIDEO_CHECK(frame->gpu->bytes == 1'280U * 720U * 3U);
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto created = rhi::Device::create(config);
    VIDEO_CHECK(created.has_value());
    auto device = std::move(created).value();
    auto planes = device.import_video_planes(
        frame->gpu->resource, frame->width, frame->height,
        rhi::TextureFormat::R16Unorm, rhi::TextureFormat::Rg16Unorm);
    VIDEO_CHECK(planes.has_value());
    VIDEO_CHECK(planes.value().luma.desc().format
                == rhi::TextureFormat::R16Unorm);
    VIDEO_CHECK(planes.value().chroma.desc().format
                == rhi::TextureFormat::Rg16Unorm);
}

#if defined(_WIN32)
// Whether the device's own video decoder says it decodes HEVC Main10 at this
// size into P010. Asked of D3D12 directly rather than of FFmpeg or
// probe_media_decode_support, so the test below cannot excuse itself through
// the code it is checking.
[[nodiscard]] bool hevc_main10_decodes_on(const rhi::Device &device,
                                          UINT width, UINT height) {
    auto *const native =
        static_cast<ID3D12Device *>(device.native_handle().value);
    ID3D12VideoDevice *video = nullptr;
    if (native == nullptr ||
        FAILED(native->QueryInterface(IID_PPV_ARGS(&video)))) {
        return false;
    }
    D3D12_FEATURE_DATA_VIDEO_DECODE_SUPPORT support{};
    support.Configuration.DecodeProfile = D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN10;
    support.Configuration.BitstreamEncryption =
        D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE;
    support.Configuration.InterlaceType =
        D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE;
    support.Width = width;
    support.Height = height;
    support.DecodeFormat = DXGI_FORMAT_P010;
    support.FrameRate = DXGI_RATIONAL{24U, 1U};
    const bool supported =
        SUCCEEDED(video->CheckFeatureSupport(
            D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &support, sizeof(support))) &&
        (support.SupportFlags & D3D12_VIDEO_DECODE_SUPPORT_FLAG_SUPPORTED)
            != 0;
    video->Release();
    return supported;
}
#endif

// A UHD ten-bit HEVC clip plays on the renderer's device, frame after frame.
//
// 2160 is not a whole number of HEVC coding blocks, and a decoder surface of
// exactly the picture's size was written past its end: the driver removed
// the device on the first frame or within twenty, and every frame after that
// failed to get a surface at all. So this decodes a run of frames rather
// than one, each from a distinct picture, and reads the last rows of the
// picture back - a surface cut from the wrong place, or not cut at all,
// shows the band at the bottom moved or missing.
//
// Wherever D3D12 itself says the device decodes HEVC Main10 at this size,
// every frame must arrive on the device; a machine that cannot is the only
// one that skips.
void a_uhd_ten_bit_hevc_clip_plays_on_the_renderers_device() {
#if defined(_WIN32)
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto created = rhi::Device::create(config);
    if (!created || !created.value().capabilities().external_memory) {
        return;
    }
    auto device = std::move(created).value();
    constexpr core::u32 width = 3'840U;
    constexpr core::u32 height = 2'160U;
    if (!hevc_main10_decodes_on(device, width, height)) {
        return;
    }
    constexpr std::array<const char*, 3U> encoders{{
        "hevc_nvenc", "hevc_mf", "libx265"}};
    const AVCodec* encoder = nullptr;
    for (const char* name : encoders) {
        encoder = avcodec_find_encoder_by_name(name);
        if (encoder != nullptr) {
            break;
        }
    }
    VIDEO_CHECK(encoder != nullptr);

    // Each frame's picture: one luma level above a 48-row band of another,
    // both moving with the frame so no frame can stand in for its neighbour.
    // Ten-bit codes; the band starts on a 64-row boundary so it is not
    // smeared by the encoder's blocks.
    constexpr int frames = 24;
    constexpr core::u32 band_top = 2'112U;
    const auto upper_code = [](int index) { return 160 + index * 24; };
    const auto band_code = [](int index) { return 940 - index * 4; };

    const auto directory =
        std::filesystem::current_path() / "out" / "codec-fixtures";
    std::filesystem::create_directories(directory);
    const auto path = (directory / "device-resident-uhd10.mp4").string();
    {
        CodecFixtureWriter writer;
        writer.codec = avcodec_alloc_context3(encoder);
        VIDEO_CHECK(writer.codec != nullptr);
        writer.codec->width = static_cast<int>(width);
        writer.codec->height = static_cast<int>(height);
        writer.codec->time_base = AVRational{1, 24};
        writer.codec->framerate = AVRational{24, 1};
        writer.codec->pix_fmt = AV_PIX_FMT_P010LE;
        writer.codec->profile = AV_PROFILE_HEVC_MAIN_10;
        writer.codec->gop_size = 12;
        writer.codec->max_b_frames = 0;
        writer.codec->colorspace = AVCOL_SPC_BT709;
        writer.codec->color_range = AVCOL_RANGE_MPEG;
        VIDEO_CHECK(avcodec_open2(writer.codec, encoder, nullptr) >= 0);
        VIDEO_CHECK(avformat_alloc_output_context2(
            &writer.format, nullptr, "mp4", path.c_str()) >= 0);
        AVStream* stream = avformat_new_stream(writer.format, nullptr);
        VIDEO_CHECK(stream != nullptr);
        stream->time_base = writer.codec->time_base;
        stream->avg_frame_rate = writer.codec->framerate;
        VIDEO_CHECK(avcodec_parameters_from_context(
            stream->codecpar, writer.codec) >= 0);
        VIDEO_CHECK(
            avio_open(&writer.format->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0);
        VIDEO_CHECK(avformat_write_header(writer.format, nullptr) >= 0);
        writer.frame = av_frame_alloc();
        writer.packet = av_packet_alloc();
        VIDEO_CHECK(writer.frame != nullptr && writer.packet != nullptr);
        writer.frame->format = writer.codec->pix_fmt;
        writer.frame->width = writer.codec->width;
        writer.frame->height = writer.codec->height;
        VIDEO_CHECK(av_frame_get_buffer(writer.frame, 0) >= 0);
        const auto drain = [&] {
            int status{};
            while ((status = avcodec_receive_packet(
                        writer.codec, writer.packet)) == 0) {
                if (writer.packet->duration == 0) writer.packet->duration = 1;
                av_packet_rescale_ts(writer.packet, writer.codec->time_base,
                                     stream->time_base);
                writer.packet->stream_index = stream->index;
                VIDEO_CHECK(av_interleaved_write_frame(
                    writer.format, writer.packet) >= 0);
                av_packet_unref(writer.packet);
            }
            VIDEO_CHECK(status == AVERROR(EAGAIN) || status == AVERROR_EOF);
        };
        const auto fill_row = [&](int plane, core::u32 row, int code) {
            // P010 keeps ten bits in the high bits of each sixteen-bit word.
            auto* const words = reinterpret_cast<std::uint16_t*>(
                writer.frame->data[plane]
                + static_cast<std::ptrdiff_t>(row)
                      * writer.frame->linesize[plane]);
            std::fill_n(words, width,
                        static_cast<std::uint16_t>(code << 6U));
        };
        for (int index = 0; index < frames; ++index) {
            VIDEO_CHECK(av_frame_make_writable(writer.frame) >= 0);
            for (core::u32 row = 0U; row < height; ++row) {
                fill_row(0, row,
                         row < band_top ? upper_code(index) : band_code(index));
            }
            for (core::u32 row = 0U; row < height / 2U; ++row) {
                fill_row(1, row, 512);
            }
            writer.frame->pts = index;
            writer.frame->duration = 1;
            VIDEO_CHECK(avcodec_send_frame(writer.codec, writer.frame) >= 0);
            drain();
        }
        VIDEO_CHECK(avcodec_send_frame(writer.codec, nullptr) >= 0);
        drain();
        VIDEO_CHECK(av_write_trailer(writer.format) >= 0);
    }

    const auto offer = render::offer_video_decode_device(device);
    auto source = render::make_media_source();
    // The luma code a frame shows at one row, read back from its plane.
    const auto luma_codes = [&](const rhi::Texture &luma) {
        const core::u32 bytes = width * height * 2U;
        auto readback = device.create_readback(bytes);
        VIDEO_CHECK(readback.has_value());
        auto acquired = device.acquire_commands();
        VIDEO_CHECK(acquired.has_value());
        auto commands = std::move(acquired).value();
        VIDEO_CHECK(commands.download(luma, readback.value()));
        auto submitted = device.submit(std::move(commands));
        VIDEO_CHECK(submitted.has_value());
        VIDEO_CHECK(std::move(submitted).value().wait());
        const auto mapped = readback.value().map();
        VIDEO_CHECK(mapped.size() >= bytes);
        std::vector<int> at_rows;
        for (const core::u32 row : {0U, 1'000U, band_top - 1U, band_top,
                                    height - 1U}) {
            const std::size_t at =
                (static_cast<std::size_t>(row) * width + width / 2U) * 2U;
            at_rows.push_back(static_cast<int>(
                (static_cast<unsigned>(mapped[at])
                 | (static_cast<unsigned>(mapped[at + 1U]) << 8U)) >> 6U));
        }
        readback.value().unmap();
        return at_rows;
    };
    const auto close_to = [](int value, int expected) {
        return std::abs(value - expected) <= 6;
    };
    for (int index = 0; index < frames; ++index) {
        const auto frame =
            await_frame(*source, path, static_cast<core::u32>(index), 0U,
                        std::chrono::seconds{20});
        VIDEO_CHECK(frame->valid());
        VIDEO_CHECK(frame->source_frame == static_cast<core::u32>(index));
        VIDEO_CHECK(frame->width == width && frame->height == height);
        VIDEO_CHECK(frame->layout
                    == render::DecodedPixelLayout::P010Rec709Limited);
        VIDEO_CHECK(frame->chroma == (render::ChromaExtent{1'920U, 1'080U}));
        VIDEO_CHECK(frame->pixels.empty());
        VIDEO_CHECK(frame->gpu.has_value());
        VIDEO_CHECK(frame->gpu->device == device.native_handle());
        VIDEO_CHECK(frame->gpu->bytes == width * height * 3U);
        auto planes = device.import_video_planes(
            frame->gpu->resource, frame->width, frame->height,
            rhi::TextureFormat::R16Unorm, rhi::TextureFormat::Rg16Unorm);
        VIDEO_CHECK(planes.has_value());
        if (index % 8 != 7) {
            continue;
        }
        const auto codes = luma_codes(planes.value().luma);
        VIDEO_CHECK(close_to(codes[0], upper_code(index)));
        VIDEO_CHECK(close_to(codes[1], upper_code(index)));
        VIDEO_CHECK(close_to(codes[2], upper_code(index)));
        VIDEO_CHECK(close_to(codes[3], band_code(index)));
        VIDEO_CHECK(close_to(codes[4], band_code(index)));
    }
#endif
}

// Direct3D 12 hands every device made on one adapter the same object, so the
// presenters of a multi-output node - one RHI device each - are one decode
// device, and each renderer's offer is counted rather than taken for a second
// device that would suspend the path.
void renderers_on_one_adapter_share_the_decode_device() {
    auto first = create_decode_device();
    auto second = create_decode_device();
    if (!first.has_value() || !second.has_value()) {
        return;
    }
    if (first->native_handle() != second->native_handle()) {
        // Two adapters: genuinely two devices, which is the suspended case.
        return;
    }
    const auto path = write_device_fixture("device-shared-hd.mp4");
    if (!path.has_value()) {
        return;
    }
    auto first_offer = render::offer_video_decode_device(*first);
    auto second_offer = render::offer_video_decode_device(*second);
    VIDEO_CHECK(static_cast<bool>(first_offer));
    VIDEO_CHECK(static_cast<bool>(second_offer));
    auto source = render::make_media_source();
    const auto frame = await_frame(*source, *path, 0U);
    VIDEO_CHECK(frame->valid());
    if (render::probe_media_decode_support(*path)
        != render::MediaDecodeSupport::HardwareAccelerated) {
        return;
    }
    VIDEO_CHECK(frame->gpu.has_value());
    VIDEO_CHECK(frame->gpu->device == first->native_handle());

    // One renderer going leaves the other's offer standing: the clip stays on
    // the device and the frame already there is still the one held.
    second_offer = render::VideoDecodeDeviceOffer{};
    VIDEO_CHECK(!static_cast<bool>(second_offer));
    const auto next = await_frame(*source, *path, 1U);
    VIDEO_CHECK(next->valid() && next->gpu.has_value());
    VIDEO_CHECK(next->gpu->device == first->native_handle());
    VIDEO_CHECK(await_frame(*source, *path, 0U).get() == frame.get());
}

// With no scene renderer left there is nothing that can sample a decoder's
// surface. Frames come back through system memory - including one that was
// already on the device, which would otherwise be held where nothing can draw
// it - and a renderer offering the device again brings the clip back onto it.
void withdrawing_every_offer_returns_frames_to_system_memory() {
    auto device = create_decode_device();
    if (!device.has_value()) {
        return;
    }
    const auto path = write_device_fixture("device-withdrawn-hd.mp4");
    if (!path.has_value()) {
        return;
    }
    auto source = render::make_media_source();
    {
        const auto offer = render::offer_video_decode_device(*device);
        const auto resident = await_frame(*source, *path, 0U);
        VIDEO_CHECK(resident->valid());
        if (render::probe_media_decode_support(*path)
            != render::MediaDecodeSupport::HardwareAccelerated) {
            return;
        }
        VIDEO_CHECK(resident->gpu.has_value());
        VIDEO_CHECK(resident->pixels.empty());
    }

    const auto reread = await_frame(*source, *path, 0U);
    VIDEO_CHECK(reread->valid());
    VIDEO_CHECK(!reread->gpu.has_value());
    VIDEO_CHECK(!reread->pixels.empty());
    VIDEO_CHECK(reread->width == 1'280U && reread->height == 720U);
    VIDEO_CHECK(reread->source_frame == 0U);
    const auto later = await_frame(*source, *path, 1U);
    VIDEO_CHECK(later->valid() && !later->gpu.has_value());
    VIDEO_CHECK(later->source_frame == 1U);

    const auto again = render::offer_video_decode_device(*device);
    const auto back = await_frame(*source, *path, 6U);
    VIDEO_CHECK(back->valid() && back->gpu.has_value());
    VIDEO_CHECK(back->gpu->device == device->native_handle());
    VIDEO_CHECK(back->source_frame == 6U);
}

// A renderer can be built and destroyed before any clip opens - an output
// deployment that failed does exactly that. The decoder must not reach for
// that renderer's device afterwards, and the device a later renderer offers
// is the one decoded onto.
void a_device_that_left_before_any_clip_opened_is_not_decoded_onto() {
    const auto path = write_device_fixture("device-gone-hd.mp4");
    if (!path.has_value()) {
        return;
    }
    {
        auto gone = create_decode_device();
        if (!gone.has_value()) {
            return;
        }
        const auto offer = render::offer_video_decode_device(*gone);
        VIDEO_CHECK(static_cast<bool>(offer));
    }
    auto source = render::make_media_source();
    const auto frame = await_frame(*source, *path, 0U);
    VIDEO_CHECK(frame->valid());
    VIDEO_CHECK(!frame->gpu.has_value());
    VIDEO_CHECK(!frame->pixels.empty());
    VIDEO_CHECK(frame->width == 1'280U && frame->height == 720U);

    auto replacement = create_decode_device();
    if (!replacement.has_value()) {
        return;
    }
    const auto offer = render::offer_video_decode_device(*replacement);
    const auto next = await_frame(*source, *path, 5U);
    VIDEO_CHECK(next->valid());
    if (render::probe_media_decode_support(*path)
        != render::MediaDecodeSupport::HardwareAccelerated) {
        return;
    }
    VIDEO_CHECK(next->gpu.has_value());
    VIDEO_CHECK(next->gpu->device == replacement->native_handle());
    VIDEO_CHECK(next->source_frame == 5U);
}

constexpr std::array<Case, 21U> cases{{
    {"halving averages each two by two block",
     &halving_averages_each_two_by_two_block},
    {"ready frames report what shared residency holds",
     &ready_frames_report_what_shared_residency_holds},
    {"speculative publication preserves current frames with a full cache",
     &speculative_publication_preserves_current_frames_with_a_full_cache},
    {"ProRes 422 decodes exact frames", &prores_422_decodes_exact_frames},
    {"untagged high definition is read as Rec.709",
     &untagged_high_definition_is_read_as_rec709},
    {"DNxHR HQX decodes exact frames", &dnxhr_hqx_decodes_exact_frames},
    {"DNxHR LB decodes exact frames", &dnxhr_lb_decodes_exact_frames},
    {"speculative video frames can be polled without current frame demand",
     &speculative_video_frames_can_be_polled_without_current_frame_demand},
    {"a real MP4 decodes exact frames in forward and reverse order",
     &a_real_mp4_decodes_exact_frames_in_forward_and_reverse_order},
    {"two sources share one in-process decode",
     &two_sources_share_one_in_process_decode},
    {"two playheads on one file keep independent decode continuity",
     &two_playheads_on_one_file_keep_independent_decode_continuity},
    {"a failed path retries only after explicit refresh",
     &a_failed_path_retries_only_after_explicit_refresh},
    {"render queries drop while the video registry is busy",
     &render_queries_drop_while_the_video_registry_is_busy},
    {"decode support distinguishes non-video and unavailable video",
     &decode_support_distinguishes_non_video_and_unavailable_video},
    {"buffered output raises the decode cache without allocating it",
     &buffered_output_raises_the_decode_cache_without_allocating_it},
    // Last: which device video is decoded onto is process-wide.
    {"a decoded frame stays on the renderer's device",
     &a_decoded_frame_stays_on_the_renderers_device},
    {"renderers on one adapter share the decode device",
     &renderers_on_one_adapter_share_the_decode_device},
    {"withdrawing every offer returns frames to system memory",
     &withdrawing_every_offer_returns_frames_to_system_memory},
    {"a device that left before any clip opened is not decoded onto",
     &a_device_that_left_before_any_clip_opened_is_not_decoded_onto},
    {"ten-bit video keeps its depth",
     &ten_bit_video_keeps_its_depth},
    {"a UHD ten-bit HEVC clip plays on the renderer's device",
     &a_uhd_ten_bit_hevc_clip_plays_on_the_renderers_device},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
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
