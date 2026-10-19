#include <mediaserver/render/media_source.hpp>

#include "../src/ffmpeg_source_test_support.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

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
        writer.codec->width = 256;
        writer.codec->height = 128;
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
    VIDEO_CHECK(probe.width == 256U && probe.height == 128U);
    VIDEO_CHECK(probe.frame_count == 3U);
    VIDEO_CHECK(probe.frame_rate == render::MediaFrameRate{24U, 1U});
    VIDEO_CHECK(probe.codec == (id == AV_CODEC_ID_PRORES ? "prores" : "dnxhd"));
    VIDEO_CHECK(render::probe_media_decode_support(path) == render::MediaDecodeSupport::SoftwareOnly);
    auto source = render::make_media_source();
    // Reverse order checks the decoder's frame-exact seek as well as ingestion.
    for (const core::u32 index : {2U, 0U, 1U}) {
        const auto frame = await_frame(*source, path, index);
        VIDEO_CHECK(frame->valid() && frame->width == 256U && frame->height == 128U);
        VIDEO_CHECK(frame->source_frame == index);
        const int expected = static_cast<int>((16U + index * 96U) * 255U / 219U);
        const int actual = frame->pixels[(64U * 256U + 128U) * 4U];
        VIDEO_CHECK(std::abs(expected - actual) <= 4);
    }
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

constexpr std::array<Case, 12U> cases{{
    {"speculative publication preserves current frames with a full cache",
     &speculative_publication_preserves_current_frames_with_a_full_cache},
    {"ProRes 422 decodes exact frames", &prores_422_decodes_exact_frames},
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
