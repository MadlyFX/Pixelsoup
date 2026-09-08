#pragma once

#include <mediaserver/core/types.hpp>

// Camera tracking, and nothing else.
//
// This module exists so that "where is the physical camera" is a value with no
// opinions about documents, scenes, renderers, or transports. It parses tracker
// protocol bytes somebody else received, holds one pose in a slot a frame
// boundary can sample without blocking, and answers "which pose belongs to show
// frame N" without ever inventing one.
//
// Two rules give the module its shape, and both are load bearing on a wall:
//
//  * **There is no I/O here.** No socket, no file, no clock, no thread. A
//    caller hands in a datagram it already received. That is the structural
//    reason a render node cannot accidentally read its own tracker: there is
//    nothing in this library that could. A node that sampled its own feed would
//    get a different pose from its neighbour and open a seam mid-wall, so the
//    only correct sampler is the Director, and it is the Director because it is
//    the only process that owns a socket pointed at a tracker.
//
//  * **Nothing here extrapolates.** A missing sample holds the last one. Two
//    nodes that each guessed would guess differently.
//
// Angles are in turns rather than degrees for the same reason the scene layer
// uses them: the conversion from a protocol's fixed-point degrees happens once,
// here, in binary64, instead of at every consumer.
namespace mediaserver::track {

// Turns of a circle. The same unit the scene layer authors rotations in; the
// duplicate alias avoids a dependency purely for a typedef.
using Turns = float;

// Which protocol produced a pose. Carried so a diagnostic can say what is
// actually feeding the stage, and so a mixed deployment cannot silently prefer
// whichever packet arrived last.
enum class TrackerProtocol : core::u8 {
    None = 0U,
    FreeD = 1U,
    OpenTrackIO = 2U,
};

// One tracker sample.
//
// Position is in metres in stage/world space and names the camera's entrance
// pupil, because that is the point a frustum is built from. Orientation is the
// pan/tilt/roll triple every camera tracker reports, converted to turns:
// pan is yaw about +Y, tilt is pitch about +X, roll is about -Z. That is the
// same fixed order `scene::rotation` composes in, so a consumer never has to
// decide an Euler convention.
//
// The lens fields are what the tracker itself reported. They are deliberately
// separate from the authored lens profile: a tracker that reports no zoom
// encoder leaves `focal_length_mm` at zero, and a consumer then uses the
// authored profile rather than a fabricated focal length.
struct TrackedPose final {
    float x{};
    float y{};
    float z{};
    Turns pan{};
    Turns tilt{};
    Turns roll{};
    // Zero means the tracker reported no usable zoom/focus/iris encoder.
    float focal_length_mm{};
    float focus_distance_m{};
    float aperture_f_number{};
    // Lens controls in the source's calibrated units. FreeD supplies its
    // exact 24-bit zoom/focus counts; OpenTrackIO supplies the measured lens
    // values it carries. A zero is a legitimate encoder position, so validity
    // is explicit and travels with the pose to the Director frame boundary.
    float zoom_encoder{};
    float focus_encoder{};
    float iris_encoder{};
    bool zoom_encoder_valid{};
    bool focus_encoder_valid{};
    bool iris_encoder_valid{};
    // Monotonic per-source ordinal. FreeD has no sequence field, so the
    // receiver's own datagram count is used; OpenTrackIO carries one.
    core::u64 sequence{};
    // Which physical camera on the stage. FreeD carries a camera id byte;
    // OpenTrackIO samples are per-camera by construction and report zero.
    core::u32 camera_id{};
    TrackerProtocol protocol{TrackerProtocol::None};

    bool operator==(const TrackedPose&) const = default;
};

// True when every field is finite and the pose could describe a camera that
// exists. A parser never returns a pose this rejects; the check is exposed so a
// consumer at the far end of a wire can refuse one it was handed.
[[nodiscard]] bool valid(const TrackedPose& pose) noexcept;

// Why a datagram was refused. Refusals are counted rather than logged on the
// receive path, and a refused datagram never partially updates a pose.
enum class TrackerParseError : core::u8 {
    // Wrong length for the message the identifier claims to be.
    Truncated,
    // Not a message this parser knows: wrong leading identifier, wrong
    // protocol name, or a body that is not the expected shape.
    UnrecognizedMessage,
    // The message's own integrity check failed.
    ChecksumMismatch,
    // Structurally intact but semantically unusable: a non-finite number, an
    // angle outside a turn, a nesting depth past the bound.
    InvalidValue,
};

} // namespace mediaserver::track
