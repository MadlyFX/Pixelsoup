#include <mediaserver/render/media_source.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
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

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define VIDEO_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

constexpr std::string_view fixture_base64 =
    "AAAAHGZ0eXBpc29tAAACAGlzb21pc28ybXA0MQAAAAhmcmVlAAAAp21kYXQAAAGzABAHAAABthBgUYVm2EAG4wAAAbMAEAcAAAG2"
    "EOBxrtsEkC1fAAABswAQBwAAAbYRYNGFFtgZ4XMAAAGzABAHAAABthHhEY6bYF2PvwAAAbMAEAcAAAG2EmERlG2DUDq/AAABswAQ"
    "BwAAAbYS4RGabYbgXj8AAAGzABAHAAABthNhEZBtg1DrAAABswAQBwAAAbYT4RGo2wVAaH8AAANRbW9vdgAAAGxtdmhkAAAAAAAA"
    "AAAAAAAAAAAD6AAAAU4AAQAAAQAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAgAAAnt0cmFrAAAAXHRraGQAAAADAAAAAAAAAAAAAAABAAAAAAAAAU4AAAAAAAAAAAAAAAAAAAAA"
    "AAEAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAABAAAAAQAAAAAAAkZWR0cwAAABxlbHN0AAAAAAAAAAEAAAFOAAAA"
    "AAABAAAAAAHzbWRpYQAAACBtZGhkAAAAAAAAAAAAAAAAAAAwAAAAEABVxAAAAAAALWhkbHIAAAAAAAAAAHZpZGUAAAAAAAAAAAAA"
    "AABWaWRlb0hhbmRsZXIAAAABnm1pbmYAAAAUdm1oZAAAAAEAAAAAAAAAAAAAACRkaW5mAAAAHGRyZWYAAAAAAAAAAQAAAAx1cmwg"
    "AAAAAQAAAV5zdGJsAAAA2nN0c2QAAAAAAAAAAQAAAMptcDR2AAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAABAAEABIAAAASAAAAAAA"
    "AAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAGP//AAAAYGVzZHMAAAAAA4CAgE8AAQAEgICAQSARAAAAAAAO6AAA"
    "AAAFgICALwAAAbABAAABtYkTAAABAAAAASAAxI2IAMUAhAIUYwAAAbJMYXZjNjIuMjguMTAxBoCAgAECAAAAFGJ0cnQAAAAAAAAO"
    "6AAAAAAAAAAYc3R0cwAAAAAAAAABAAAACAAAAgAAAAAcc3RzYwAAAAAAAAABAAAAAQAAAAgAAAABAAAANHN0c3oAAAAAAAAAAAAA"
    "AAgAAAAUAAAAFAAAABQAAAAUAAAAFAAAABQAAAATAAAAFAAAABRzdGNvAAAAAAAAAAEAAAAsAAAAYnVkdGEAAABabWV0YQAAAAAA"
    "AAAhaGRscgAAAAAAAAAAbWRpcmFwcGwAAAAAAAAAAAAAAAAtaWxzdAAAACWpdG9vAAAAHWRhdGEAAAABAAAAAExhdmY2Mi4xMi4x"
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
        accumulator = (accumulator << 6U)
            | static_cast<std::uint32_t>(position);
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

void write_fixture(const std::string& path) {
    const auto bytes = decode_base64(fixture_base64);
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    out.close();
    check(static_cast<bool>(out), "wrote MP4 fixture", __LINE__);
}

[[nodiscard]] std::shared_ptr<const render::DecodedFrame> await_frame(
    render::MediaSource& source,
    const std::string& path,
    core::u32 frame_index,
    std::chrono::milliseconds timeout = std::chrono::seconds{5}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        auto frame = source.try_decode(path, frame_index);
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
    VIDEO_CHECK(render::probe_frame_count(path) == 8U);
    const auto rate = render::probe_media_frame_rate(path);
    VIDEO_CHECK(rate.has_value());
    VIDEO_CHECK(rate->numerator == 24U);
    VIDEO_CHECK(rate->denominator == 1U);

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
    VIDEO_CHECK(
        render::media_decode_stats().completed_frames - before == 1U);
}

void a_failed_path_can_be_retried_after_the_file_is_replaced() {
    const auto path = fixture_path("mediaserver_replaced_video_fixture.mp4");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    auto source = render::make_media_source();
    const auto failed = await_frame(*source, path, 0U);
    VIDEO_CHECK(!failed->valid());

    write_fixture(path);
    // Failed records have a short cooldown so a missing file does not hammer
    // storage every UI frame, but they are not process-lifetime poison.
    std::this_thread::sleep_for(std::chrono::milliseconds{1100});
    VIDEO_CHECK(source->try_decode(path, 0U) == nullptr);
    const auto recovered = await_frame(*source, path, 0U);
    VIDEO_CHECK(recovered->valid());
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 3U> cases{{
    {"a real MP4 decodes exact frames in forward and reverse order",
     &a_real_mp4_decodes_exact_frames_in_forward_and_reverse_order},
    {"two sources share one in-process decode",
     &two_sources_share_one_in_process_decode},
    {"a failed path can be retried after the file is replaced",
     &a_failed_path_can_be_retried_after_the_file_is_replaced},
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
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
