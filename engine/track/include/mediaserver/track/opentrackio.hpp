#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/track/tracked_pose.hpp>

#include <cstddef>
#include <span>

// OpenTrackIO, the JSON sample protocol from the SMPTE RIS on OSVP.
//
// Where FreeD is a fixed 29-byte frame, an OpenTrackIO sample is a JSON
// document carrying pose, lens, timing, and static camera metadata together.
// The parts this parser reads are:
//
//   protocol.name           must be "OpenTrackIO"
//   protocol.version        optional [major, minor, patch]
//   transforms[]            translation in metres, rotation in degrees
//   lens.focalLength        millimetres
//   lens.focusDistance      metres
//   lens.fStop              f-number
//   lens.entrancePupilOffset  metres along the optical axis
//   timing.sequenceNumber   monotonic sample ordinal
//
// Everything else in the sample is validated as well-formed JSON and then
// ignored. Validating rather than skipping matters: a truncated datagram whose
// tail happens to fall after the fields of interest would otherwise be accepted
// as a complete sample, and the next one would be too, and the wall would track
// a camera from half a packet.
//
// **Which transform is the camera.** `transforms` is a chain from the world to
// the camera. This parser uses the entry whose `id` is exactly "Camera" when
// one is present, and otherwise the last entry in the array - the two
// conventions real senders use. It does not compose a chain of parent
// transforms; a sample that needs composition is outside what this reads and
// should be refused upstream rather than silently mis-tracked.
//
// **Numbers.** JSON numbers are parsed by an explicit decimal reader rather
// than the C library, because the pose reaches a wall through a replicated
// channel and every machine that reads a sample must reach identical bits. The
// reader accepts the full JSON number grammar with a decimal exponent inside
// +/-22, which is the range where the scale factor is exactly representable in
// binary64. A tracker does not report a camera position of 1e30 metres, so the
// bound costs nothing and buys exactness.
namespace mediaserver::track {

// A complete sample is on the order of one to two kilobytes. The bound exists
// so a hostile or broken sender cannot make the parser walk an arbitrarily
// large buffer on the Director's tracker thread.
inline constexpr std::size_t max_opentrackio_sample_bytes = 16U * 1024U;
// JSON nesting past this is not an OpenTrackIO sample.
inline constexpr std::size_t max_opentrackio_depth = 16U;
// Transform-chain entries retained while looking for the camera.
inline constexpr std::size_t max_opentrackio_transforms = 8U;

struct OpenTrackIOSample final {
    TrackedPose pose{};
    // Distance from the tracked position to the entrance pupil along the
    // optical axis, in metres. Carried rather than pre-applied: applying it
    // needs the lens model, which is the next step's subject.
    float entrance_pupil_offset_m{};
    // Protocol major version the sample declared. Zero when it declared none.
    core::u32 version_major{};
    // True when the sample named a transform with id "Camera" rather than
    // falling back to the last entry in the chain.
    bool named_camera_transform{};
    // True when the sample carried a `sourceNumber`, so `pose.camera_id` names
    // a camera rather than merely being zero.
    //
    // FreeD puts a camera id in every datagram; OpenTrackIO makes it optional,
    // and the two cases have to stay apart. A sample that does not say which
    // camera it is cannot be matched against an authored id, and treating
    // "did not say" as "camera zero" is how a feed authored for any other
    // camera silently refused everything it received.
    bool identifies_camera{};

    bool operator==(const OpenTrackIOSample&) const = default;
};

// Decodes one OpenTrackIO JSON sample.
//
// The datagram must be a complete, well-formed JSON object declaring
// `protocol.name` of "OpenTrackIO" and carrying at least one transform with
// both a translation and a rotation. Anything else is refused whole; no field
// of a refused sample reaches the returned pose.
//
// `fallback_sequence` is used only when the sample declares no
// `timing.sequenceNumber`, so a sender that omits it still produces a pose a
// consumer can tell apart from a repeat.
[[nodiscard]] core::Result<OpenTrackIOSample, TrackerParseError>
parse_opentrackio(
    std::span<const std::byte> datagram, core::u64 fallback_sequence) noexcept;

} // namespace mediaserver::track
