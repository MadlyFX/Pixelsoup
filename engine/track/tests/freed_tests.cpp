#include <mediaserver/track/freed.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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

#define FREED_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

void write_signed_24(std::vector<std::byte>& out, core::i32 value) {
    const auto raw = static_cast<core::u32>(value) & 0x00FF'FFFFU;
    out.push_back(static_cast<std::byte>((raw >> 16U) & 0xFFU));
    out.push_back(static_cast<std::byte>((raw >> 8U) & 0xFFU));
    out.push_back(static_cast<std::byte>(raw & 0xFFU));
}

// A D1 datagram built the way the protocol defines it, so the parser is tested
// against the specification rather than against itself.
[[nodiscard]] std::vector<std::byte> datagram(
    core::u8 camera,
    core::i32 pan,
    core::i32 tilt,
    core::i32 roll,
    core::i32 x,
    core::i32 y,
    core::i32 z,
    core::u32 zoom,
    core::u32 focus) {
    std::vector<std::byte> bytes;
    bytes.reserve(track::freed_d1_message_bytes);
    bytes.push_back(track::freed_d1_identifier);
    bytes.push_back(static_cast<std::byte>(camera));
    write_signed_24(bytes, pan);
    write_signed_24(bytes, tilt);
    write_signed_24(bytes, roll);
    write_signed_24(bytes, x);
    write_signed_24(bytes, y);
    write_signed_24(bytes, z);
    write_signed_24(bytes, static_cast<core::i32>(zoom));
    write_signed_24(bytes, static_cast<core::i32>(focus));
    bytes.push_back(std::byte{0});
    bytes.push_back(std::byte{0});
    bytes.push_back(track::freed_d1_checksum(
        std::span<const std::byte>{bytes.data(), bytes.size()}));
    return bytes;
}

[[nodiscard]] bool near(float value, float expected, float tolerance) noexcept {
    return std::fabs(value - expected) <= tolerance;
}

void known_bytes_produce_a_known_pose() {
    // 90 degrees of pan, -30 of tilt, 12.5 of roll; 1.5 m, -0.75 m, 4.25 m.
    const auto bytes = datagram(
        7U,
        90 * 32'768,
        -30 * 32'768,
        (125 * 32'768) / 10,
        static_cast<core::i32>(1.5 * 1'000.0 * 64.0),
        static_cast<core::i32>(-0.75 * 1'000.0 * 64.0),
        static_cast<core::i32>(4.25 * 1'000.0 * 64.0),
        0x0012'3456U,
        0x0000'ABCDU);
    FREED_CHECK(bytes.size() == track::freed_d1_message_bytes);

    const auto parsed = track::parse_freed_d1(bytes, 42U);
    FREED_CHECK(parsed);
    const track::FreeDSample& sample = parsed.value();

    FREED_CHECK(sample.pose.protocol == track::TrackerProtocol::FreeD);
    FREED_CHECK(sample.pose.camera_id == 7U);
    FREED_CHECK(sample.pose.sequence == 42U);
    // 90 degrees is exactly a quarter turn, and the conversion is exact.
    FREED_CHECK(sample.pose.pan == 0.25F);
    FREED_CHECK(near(sample.pose.tilt, -30.0F / 360.0F, 1e-7F));
    FREED_CHECK(near(sample.pose.roll, 12.5F / 360.0F, 1e-7F));
    FREED_CHECK(sample.pose.x == 1.5F);
    FREED_CHECK(sample.pose.y == -0.75F);
    FREED_CHECK(sample.pose.z == 4.25F);
    FREED_CHECK(sample.zoom_counts == 0x0012'3456U);
    FREED_CHECK(sample.focus_counts == 0x0000'ABCDU);
    // Encoder counts are not focal lengths, and this parser does not pretend
    // they are. See the header.
    FREED_CHECK(sample.pose.focal_length_mm == 0.0F);
    FREED_CHECK(sample.pose.focus_distance_m == 0.0F);
    FREED_CHECK(track::valid(sample.pose));
}

void the_full_signed_range_round_trips() {
    // The extremes of the 24-bit fields, which is where a sign-extension
    // mistake shows up and nowhere else.
    const auto bytes = datagram(
        0U, -8'388'608, 8'388'607, -1, -8'388'608, 8'388'607, -1, 0U, 0U);
    const auto parsed = track::parse_freed_d1(bytes, 1U);
    FREED_CHECK(parsed);
    const track::TrackedPose& pose = parsed.value().pose;

    FREED_CHECK(near(pose.pan, -8'388'608.0F / (32'768.0F * 360.0F), 1e-6F));
    FREED_CHECK(near(pose.tilt, 8'388'607.0F / (32'768.0F * 360.0F), 1e-6F));
    FREED_CHECK(pose.roll < 0.0F);
    FREED_CHECK(pose.x < 0.0F);
    FREED_CHECK(pose.y > 0.0F);
    FREED_CHECK(pose.z < 0.0F);
    FREED_CHECK(near(pose.x, -8'388'608.0F / 64'000.0F, 1e-4F));
}

void a_truncated_datagram_is_refused_whole() {
    const auto bytes = datagram(1U, 0, 0, 0, 0, 0, 0, 0U, 0U);
    for (std::size_t length = 0U; length < bytes.size(); ++length) {
        const auto parsed = track::parse_freed_d1(
            std::span<const std::byte>{bytes.data(), length}, 1U);
        FREED_CHECK(!parsed);
        FREED_CHECK(parsed.error() == track::TrackerParseError::Truncated);
    }
    // A datagram longer than the message is equally not one message.
    auto padded = bytes;
    padded.push_back(std::byte{0});
    FREED_CHECK(!track::parse_freed_d1(padded, 1U));
}

void a_corrupt_checksum_is_refused_rather_than_partially_applied() {
    auto bytes = datagram(
        3U, 12'345, -6'789, 0, 64'000, 0, 0, 0U, 0U);
    // Flip one bit in the middle of the position fields. Every byte of the
    // message is covered by the checksum, so this must be caught.
    bytes[12] = static_cast<std::byte>(
        static_cast<core::u32>(bytes[12]) ^ 0x01U);
    const auto parsed = track::parse_freed_d1(bytes, 1U);
    FREED_CHECK(!parsed);
    FREED_CHECK(parsed.error() == track::TrackerParseError::ChecksumMismatch);
}

void a_foreign_message_type_is_refused() {
    auto bytes = datagram(1U, 0, 0, 0, 0, 0, 0, 0U, 0U);
    // D0 is the FreeD poll message, not a position message.
    bytes[0] = std::byte{0xD0};
    bytes[track::freed_d1_message_bytes - 1U] = track::freed_d1_checksum(
        std::span<const std::byte>{
            bytes.data(), track::freed_d1_message_bytes - 1U});
    const auto parsed = track::parse_freed_d1(bytes, 1U);
    FREED_CHECK(!parsed);
    FREED_CHECK(parsed.error()
                == track::TrackerParseError::UnrecognizedMessage);
}

void the_checksum_rule_is_the_protocols_own() {
    const auto bytes = datagram(9U, 111, 222, 333, 444, 555, 666, 7U, 8U);
    core::u32 sum = 0U;
    for (const std::byte value : bytes) {
        sum += static_cast<core::u32>(value);
    }
    FREED_CHECK((sum & 0xFFU) == 0x40U);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 6U> cases{{
    {"known bytes produce a known pose", &known_bytes_produce_a_known_pose},
    {"the full signed range round trips",
     &the_full_signed_range_round_trips},
    {"a truncated datagram is refused whole",
     &a_truncated_datagram_is_refused_whole},
    {"a corrupt checksum is refused rather than partially applied",
     &a_corrupt_checksum_is_refused_rather_than_partially_applied},
    {"a foreign message type is refused", &a_foreign_message_type_is_refused},
    {"the checksum rule is the protocol's own",
     &the_checksum_rule_is_the_protocols_own},
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
