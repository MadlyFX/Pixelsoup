#include <mediaserver/track/freed.hpp>

namespace mediaserver::track {
namespace {

using Result = core::Result<FreeDSample, TrackerParseError>;

[[nodiscard]] core::u32 unsigned_24(
    std::span<const std::byte> datagram, std::size_t offset) noexcept {
    return (static_cast<core::u32>(datagram[offset]) << 16U)
        | (static_cast<core::u32>(datagram[offset + 1U]) << 8U)
        | static_cast<core::u32>(datagram[offset + 2U]);
}

// Sign-extends a 24-bit two's-complement field. Written as an explicit branch
// on the sign bit rather than a shift pair, because a right shift of a signed
// value was implementation defined until C++20 and this reads as what it is.
[[nodiscard]] core::i32 signed_24(
    std::span<const std::byte> datagram, std::size_t offset) noexcept {
    const core::u32 raw = unsigned_24(datagram, offset);
    constexpr core::u32 sign_bit = 0x0080'0000U;
    constexpr core::i64 modulus = 0x0100'0000;
    if ((raw & sign_bit) != 0U) {
        return static_cast<core::i32>(static_cast<core::i64>(raw) - modulus);
    }
    return static_cast<core::i32>(raw);
}

// 1/32768 degree per count, expressed in turns. The division happens in
// binary64 and lands on a power of two times a whole number, so every platform
// produces the same binary32 result.
[[nodiscard]] Turns turns_from_freed_angle(core::i32 counts) noexcept {
    constexpr double counts_per_degree = 32'768.0;
    constexpr double degrees_per_turn = 360.0;
    return static_cast<Turns>(
        static_cast<double>(counts) / (counts_per_degree * degrees_per_turn));
}

// 1/64 mm per count, reported in metres so the pose is in the same units the
// stage is authored in.
[[nodiscard]] float metres_from_freed_position(core::i32 counts) noexcept {
    constexpr double counts_per_millimetre = 64.0;
    constexpr double millimetres_per_metre = 1'000.0;
    return static_cast<float>(
        static_cast<double>(counts)
        / (counts_per_millimetre * millimetres_per_metre));
}

} // namespace

std::byte freed_d1_checksum(
    std::span<const std::byte> first_28_bytes) noexcept {
    // The protocol defines the checksum as the value which brings the sum of
    // every byte in the message to 0x40 modulo 256.
    core::u32 sum = 0x40U;
    for (const std::byte value : first_28_bytes) {
        sum -= static_cast<core::u32>(value);
    }
    return static_cast<std::byte>(sum & 0xFFU);
}

Result parse_freed_d1(
    std::span<const std::byte> datagram, core::u64 sequence) noexcept {
    if (datagram.size() != freed_d1_message_bytes) {
        // A short read and a coalesced pair are both length faults, and both
        // must be refused rather than parsed from whatever prefix arrived.
        return Result::failure(TrackerParseError::Truncated);
    }
    if (datagram[0] != freed_d1_identifier) {
        return Result::failure(TrackerParseError::UnrecognizedMessage);
    }
    if (datagram[freed_d1_message_bytes - 1U]
        != freed_d1_checksum(datagram.first(freed_d1_message_bytes - 1U))) {
        return Result::failure(TrackerParseError::ChecksumMismatch);
    }

    FreeDSample sample{};
    sample.pose.protocol = TrackerProtocol::FreeD;
    sample.pose.camera_id = static_cast<core::u32>(datagram[1]);
    sample.pose.pan = turns_from_freed_angle(signed_24(datagram, 2U));
    sample.pose.tilt = turns_from_freed_angle(signed_24(datagram, 5U));
    sample.pose.roll = turns_from_freed_angle(signed_24(datagram, 8U));
    sample.pose.x = metres_from_freed_position(signed_24(datagram, 11U));
    sample.pose.y = metres_from_freed_position(signed_24(datagram, 14U));
    sample.pose.z = metres_from_freed_position(signed_24(datagram, 17U));
    sample.pose.sequence = sequence;
    sample.zoom_counts = unsigned_24(datagram, 20U);
    sample.focus_counts = unsigned_24(datagram, 23U);
    // Preserve the protocol's exact 24-bit controls. The conversion to float
    // is exact over this range, so a profile table and a replicated sample
    // compare the same encoder position on every node.
    sample.pose.zoom_encoder = static_cast<float>(sample.zoom_counts);
    sample.pose.focus_encoder = static_cast<float>(sample.focus_counts);
    sample.pose.zoom_encoder_valid = true;
    sample.pose.focus_encoder_valid = true;

    // Every field above is bounded by its own 24-bit range, so this cannot
    // fail today. It is still checked, because the pose contract is "a parser
    // never returns a pose `valid()` rejects" and a future field must not be
    // able to break that quietly.
    if (!valid(sample.pose)) {
        return Result::failure(TrackerParseError::InvalidValue);
    }
    return Result::success(sample);
}

} // namespace mediaserver::track
