#include <mediaserver/render/media_source.hpp>

#include "../src/ffmpeg_source_test_support.hpp"

#include <array>
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
    VIDEO_CHECK(red->pixels[0U] > 170U);
    VIDEO_CHECK(red->pixels[1U] < 80U);

    // Frame 2 is a small forward gap. The decoder must discard frame 1 rather
    // than seek/flush, yet still return the exact blue frame.
    const auto blue = await_frame(*source, path, 2U);
    VIDEO_CHECK(blue->valid());
    VIDEO_CHECK(blue->pixels[2U] > 160U);
    VIDEO_CHECK(blue->pixels[0U] < 90U);

    const auto yellow = await_frame(*source, path, 3U);
    VIDEO_CHECK(yellow->valid());
    VIDEO_CHECK(yellow->pixels[0U] > 160U);
    VIDEO_CHECK(yellow->pixels[1U] > 150U);

    // Reverse access still uses a bounded seek and reaches frame 1 exactly.
    const auto green = await_frame(*source, path, 1U);
    VIDEO_CHECK(green->valid());
    VIDEO_CHECK(green->pixels[1U] > 150U);
    VIDEO_CHECK(green->pixels[0U] < 90U);

    // Another far-forward then reverse-to-zero pair catches a seek that lands
    // later than requested being mislabeled under the requested cache key.
    const auto orange = await_frame(*source, path, 7U);
    VIDEO_CHECK(orange->valid());
    VIDEO_CHECK(orange->pixels[0U] > 160U);
    VIDEO_CHECK(orange->pixels[1U] > 70U);
    const auto red_again = await_frame(*source, path, 0U);
    VIDEO_CHECK(red_again->valid());
    VIDEO_CHECK(red_again->pixels[0U] > 170U);
    VIDEO_CHECK(red_again->pixels[1U] < 80U);
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

struct Case final {
    const char *name;
    void (*function)();
};

constexpr std::array<Case, 8U> cases{{
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
