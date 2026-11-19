#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/track/tracked_pose.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>

// OptiTrack NatNet, the streaming protocol Motive speaks.
//
// Where FreeD is one camera's pose in 29 bytes, a NatNet frame of data is
// everything the optical system saw at once: every marker set, every unlabeled
// marker, every rigid body, skeleton, labeled marker, force plate and device,
// followed by timing. The camera is one rigid body in that frame - a cluster of
// markers on the camera body that Motive solves to a position and a
// quaternion - and it is picked out by its streaming id.
//
// Two things make NatNet different from the other two parsers here, and both
// shape the interface:
//
//  * **The layout depends on the version.** Fields were added in 2.0, 2.1,
//    2.3, 2.6, 2.7, 2.9, 3.0 and 4.1, and nothing in a frame says which
//    version wrote it. The version comes from the server's reply to a connect
//    request on its command port, so decoding a frame takes the version as an
//    argument and a receiver that has not learned it yet must hold rather than
//    guess. A guessed version does not fail loudly: it reads a marker count
//    where a timestamp was and tracks a camera that does not exist.
//
//  * **Orientation is a quaternion.** Every other tracker reports pan, tilt
//    and roll; Motive reports the rigid body's rotation as a unit quaternion
//    in its own world frame, which is Y-up by default and Z-up when the
//    operator sets it so. The conversion to the pan/tilt/roll triple the rest
//    of the product consumes happens here, once, in the fixed order
//    `scene::rotation` composes in, so no consumer ever sees a quaternion.
//
// Everything is little endian. Nothing here does I/O: a caller hands in the
// datagram it received, and `natnet_connect_request()` is the four bytes it
// should send to learn the version.
namespace mediaserver::track {

// Message ids in the two-byte header every NatNet packet starts with.
inline constexpr core::u16 natnet_connect_message = 0U;
inline constexpr core::u16 natnet_server_info_message = 1U;
inline constexpr core::u16 natnet_request_model_definitions_message = 4U;
inline constexpr core::u16 natnet_model_definitions_message = 5U;
inline constexpr core::u16 natnet_frame_message = 7U;

// Motive's defaults. The data port is where frames arrive; the command port is
// where a connect request goes, and the server answers to the address and
// port it came from.
inline constexpr core::u16 natnet_default_data_port = 1511U;
inline constexpr core::u16 natnet_default_command_port = 1510U;
inline constexpr std::array<core::u8, 4U> natnet_default_multicast_group{
    239U, 255U, 42U, 99U};

// The largest UDP payload. A frame with many markers approaches it, so this is
// the receive buffer a NatNet socket needs; an OpenTrackIO-sized one would
// truncate every frame from a busy volume.
inline constexpr std::size_t max_natnet_datagram_bytes = 65'507U;

struct NatNetVersion final {
    core::u8 major{};
    core::u8 minor{};
    core::u8 build{};
    core::u8 revision{};

    bool operator==(const NatNetVersion&) const = default;
};

[[nodiscard]] constexpr bool at_least(
    NatNetVersion version, core::u8 major, core::u8 minor) noexcept {
    return version.major > major
        || (version.major == major && version.minor >= minor);
}

// What the server says about itself in reply to a connect request.
struct NatNetServerInfo final {
    // Null-terminated application name, "Motive" in practice.
    std::array<char, 256U> application_name{};
    NatNetVersion application_version{};
    NatNetVersion natnet_version{};
    // NatNet 3.0 added the transport description below; an older server
    // leaves it absent and these fields zero.
    bool describes_transport{};
    core::u16 data_port{};
    bool multicast{};
    std::array<core::u8, 4U> multicast_group{};

    bool operator==(const NatNetServerInfo&) const = default;
};

// The message id of a datagram, or nothing when it is too short to carry a
// header. Exposed so a receiver can route a server reply and a frame of data
// arriving on the same socket without parsing either.
[[nodiscard]] std::optional<core::u16> natnet_message_id(
    std::span<const std::byte> datagram) noexcept;

// The four bytes that ask a server for its version: a connect message with an
// empty payload. Send it to the server's command port.
[[nodiscard]] std::array<std::byte, 4U> natnet_connect_request() noexcept;

[[nodiscard]] core::Result<NatNetServerInfo, TrackerParseError>
parse_natnet_server_info(std::span<const std::byte> datagram) noexcept;

// The four bytes that ask a server to describe its assets: what each rigid
// body is called, which is the only way an operator can pick "Camera A"
// rather than look up the streaming id Motive assigned it. Send it to the
// command port once the version is known; the reply arrives where it came
// from.
[[nodiscard]] std::array<std::byte, 4U>
natnet_model_definitions_request() noexcept;

// One rigid body the server described: its streaming id and asset name.
inline constexpr std::size_t max_natnet_rigid_body_name_bytes = 64U;
struct NatNetRigidBodyDescription final {
    core::i32 id{};
    // Null-terminated, truncated to fit.
    std::array<char, max_natnet_rigid_body_name_bytes> name{};

    bool operator==(const NatNetRigidBodyDescription&) const = default;
};

// Far past any camera volume; a description listing more is truncated to
// this many rather than allocated without bound on the receive thread.
inline constexpr std::size_t max_natnet_rigid_body_descriptions = 64U;

struct NatNetModelDefinitions final {
    std::array<NatNetRigidBodyDescription,
               max_natnet_rigid_body_descriptions> rigid_bodies{};
    std::size_t rigid_body_count{};
    // Whether every dataset was walked. Before 4.1 a dataset carries no size,
    // so a kind this parser does not read ends the walk early; the bodies
    // found before it are still reported.
    bool complete{};

    [[nodiscard]] std::span<const NatNetRigidBodyDescription>
    bodies() const noexcept {
        return {rigid_bodies.data(), rigid_body_count};
    }
};

// Decodes the reply to `natnet_model_definitions_request()` for the
// top-level rigid bodies it describes - the assets an operator can put on a
// camera. Skeleton bones, marker sets, cameras, force plates and devices are
// skipped.
[[nodiscard]] core::Result<NatNetModelDefinitions, TrackerParseError>
parse_natnet_model_definitions(std::span<const std::byte> datagram,
                               NatNetVersion version) noexcept;

// Which axis Motive calls up. Positions are left in the tracker's frame for
// the scene's calibration to map, as every protocol's are; the quaternion
// cannot be, because pan, tilt and roll only mean something about an agreed
// up, so the decomposition has to know.
enum class NatNetUpAxis : core::u8 {
    Y,
    Z,
};

// One decoded frame of data, seen from one camera's point of view.
struct NatNetFrame final {
    // Motive's own frame counter. Carried as the pose's sequence.
    core::u32 frame_number{};
    // Rigid bodies in the frame, skeletons' bones not counted.
    core::u32 rigid_bodies{};
    // Whether the requested rigid body was in the frame at all.
    bool found{};
    // Whether Motive solved it this frame. An occluded body is still listed,
    // at its last position, with this flag clear - and that position must be
    // held upstream, never published as new. Versions before 2.6 carry no
    // flag and report true.
    bool tracked{};
    // Meaningful only when `found && tracked`.
    TrackedPose pose{};
    // Motive's timestamp in seconds on its own clock. Diagnostics only.
    double timestamp{};

    bool operator==(const NatNetFrame&) const = default;
};

// Decodes one frame of data for the rigid body with streaming id
// `rigid_body_id`, or the first rigid body in the frame when the id is zero -
// the one-camera-on-a-volume case, where nobody wants to look an id up.
//
// The whole frame is walked and validated, not just the part of interest: a
// frame that ends early or does not land on its end-of-data tag was written by
// a different version than `version` claims, and tracking from it would be
// tracking the wrong bytes. Such a frame is refused whole as unrecognized.
[[nodiscard]] core::Result<NatNetFrame, TrackerParseError> parse_natnet_frame(
    std::span<const std::byte> datagram,
    NatNetVersion version,
    core::u32 rigid_body_id,
    NatNetUpAxis up) noexcept;

struct PanTiltRoll final {
    Turns pan{};
    Turns tilt{};
    Turns roll{};

    bool operator==(const PanTiltRoll&) const = default;
};

// The pan/tilt/roll triple whose `scene::rotation` composition equals the
// rotation `(x, y, z, w)` describes, after the tracker's up axis has been
// mapped to the stage's. Pan is yaw about +Y, tilt is pitch about +X, roll is
// about -Z: at zero the camera looks down -Z. Exposed so the decomposition
// can be tested against rotations whose answer is known.
[[nodiscard]] PanTiltRoll pan_tilt_roll_from_quaternion(
    float x, float y, float z, float w, NatNetUpAxis up) noexcept;

} // namespace mediaserver::track
