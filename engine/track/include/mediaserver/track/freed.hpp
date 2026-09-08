#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/track/tracked_pose.hpp>

#include <cstddef>
#include <span>

// FreeD D1, the camera-position message.
//
// FreeD is the oldest and most widely implemented of the tracker protocols: a
// fixed-size, self-delimiting datagram with a one-byte checksum and no version
// negotiation. That makes it the right protocol to start with - there is
// nothing to guess about, and a parser either agrees with the sender bit for
// bit or refuses.
//
// The message is 29 bytes:
//
//   0        identifier, 0xD1
//   1        camera id
//   2..4     pan,   24-bit signed, big endian, 1/32768 degree
//   5..7     tilt,  same
//   8..10    roll,  same
//   11..13   x,     24-bit signed, big endian, 1/64 mm
//   14..16   y,     same
//   17..19   z,     same
//   20..22   zoom,  24-bit unsigned, encoder counts
//   23..25   focus, 24-bit unsigned, encoder counts
//   26..27   reserved
//   28       checksum
//
// The checksum is defined so that the sum of all 29 bytes, modulo 256, is 0x40.
//
// Zoom and focus are raw encoder counts. There is no protocol-defined mapping
// from a count to a focal length in millimetres - that is a lens calibration
// table, and inventing one here would be worse than reporting nothing. So this
// parser reports the counts and leaves `focal_length_mm` and `focus_distance_m`
// at their explicit zero "no encoder value" form. See `02-lens-model.md`.
namespace mediaserver::track {

inline constexpr std::size_t freed_d1_message_bytes = 29U;
inline constexpr std::byte freed_d1_identifier{0xD1};

// One decoded D1 message: the pose, plus the raw encoder counts the pose
// deliberately does not pretend to interpret.
struct FreeDSample final {
    TrackedPose pose{};
    core::u32 zoom_counts{};
    core::u32 focus_counts{};

    bool operator==(const FreeDSample&) const = default;
};

// Decodes exactly one D1 datagram.
//
// `sequence` is the receiver's own datagram ordinal: FreeD carries no sequence
// number, and a consumer still needs to distinguish a repeated pose from a
// stalled sender.
//
// A datagram of the wrong length, with the wrong identifier, or with a failed
// checksum is refused whole. Nothing is partially applied, because a pose that
// took its position from a good packet and its rotation from a corrupt one is a
// camera that does not exist.
[[nodiscard]] core::Result<FreeDSample, TrackerParseError>
parse_freed_d1(std::span<const std::byte> datagram, core::u64 sequence) noexcept;

// The checksum byte a well-formed message with these first 28 bytes carries.
// Exposed so a test can build a valid datagram without restating the rule, and
// so a diagnostic can say what it expected.
[[nodiscard]] std::byte freed_d1_checksum(
    std::span<const std::byte> first_28_bytes) noexcept;

} // namespace mediaserver::track
