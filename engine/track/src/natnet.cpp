#include <mediaserver/track/natnet.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace mediaserver::track {
namespace {

using FrameResult = core::Result<NatNetFrame, TrackerParseError>;
using InfoResult = core::Result<NatNetServerInfo, TrackerParseError>;

constexpr std::size_t header_bytes = 4U;
// szName[256], Version[4], NatNetVersion[4].
constexpr std::size_t server_info_common_bytes = 264U;
// HighResClockFrequency u64, DataPort u16, IsMulticast u8, group u8[4].
constexpr std::size_t server_info_transport_bytes = 8U + 2U + 1U + 4U;

// A bounded little-endian reader. Every read reports whether the bytes were
// there, and a failed read leaves the position where it was, so the parser
// can refuse a short frame at the point it ran out rather than reading past
// the end of the datagram.
class Reader final {
public:
    explicit Reader(std::span<const std::byte> data) noexcept : data_(data) {}

    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return data_.size() - position_;
    }
    [[nodiscard]] bool exhausted() const noexcept { return remaining() == 0U; }

    [[nodiscard]] bool skip(std::size_t bytes) noexcept {
        if (bytes > remaining()) {
            return false;
        }
        position_ += bytes;
        return true;
    }

    [[nodiscard]] bool read_u16(core::u16& into) noexcept {
        if (remaining() < 2U) {
            return false;
        }
        into = static_cast<core::u16>(
            static_cast<core::u32>(data_[position_])
            | (static_cast<core::u32>(data_[position_ + 1U]) << 8U));
        position_ += 2U;
        return true;
    }

    [[nodiscard]] bool read_i16(core::i16& into) noexcept {
        core::u16 raw = 0U;
        if (!read_u16(raw)) {
            return false;
        }
        into = static_cast<core::i16>(raw);
        return true;
    }

    [[nodiscard]] bool read_u32(core::u32& into) noexcept {
        if (remaining() < 4U) {
            return false;
        }
        into = static_cast<core::u32>(data_[position_])
            | (static_cast<core::u32>(data_[position_ + 1U]) << 8U)
            | (static_cast<core::u32>(data_[position_ + 2U]) << 16U)
            | (static_cast<core::u32>(data_[position_ + 3U]) << 24U);
        position_ += 4U;
        return true;
    }

    [[nodiscard]] bool read_i32(core::i32& into) noexcept {
        core::u32 raw = 0U;
        if (!read_u32(raw)) {
            return false;
        }
        into = static_cast<core::i32>(raw);
        return true;
    }

    [[nodiscard]] bool read_u64(core::u64& into) noexcept {
        core::u32 low = 0U;
        core::u32 high = 0U;
        if (remaining() < 8U || !read_u32(low) || !read_u32(high)) {
            return false;
        }
        into = static_cast<core::u64>(low)
            | (static_cast<core::u64>(high) << 32U);
        return true;
    }

    [[nodiscard]] bool read_f32(float& into) noexcept {
        core::u32 raw = 0U;
        if (!read_u32(raw)) {
            return false;
        }
        into = std::bit_cast<float>(raw);
        return true;
    }

    [[nodiscard]] bool read_f64(double& into) noexcept {
        core::u64 raw = 0U;
        if (!read_u64(raw)) {
            return false;
        }
        into = std::bit_cast<double>(raw);
        return true;
    }

    // A count that must be non-negative. A negative count is not a short
    // frame but a frame of a different shape.
    [[nodiscard]] bool read_count(core::u32& into) noexcept {
        core::i32 signed_count = 0;
        if (!read_i32(signed_count)) {
            return false;
        }
        if (signed_count < 0) {
            malformed_ = true;
            return false;
        }
        into = static_cast<core::u32>(signed_count);
        return true;
    }

    // Advances past a null-terminated string. A string that reaches the end
    // of the datagram without its terminator is a short frame.
    [[nodiscard]] bool skip_cstring() noexcept {
        while (position_ < data_.size()) {
            if (data_[position_] == std::byte{0}) {
                ++position_;
                return true;
            }
            ++position_;
        }
        return false;
    }

    // Whether a failed read was a shape fault rather than a length fault.
    [[nodiscard]] bool malformed() const noexcept { return malformed_; }

private:
    std::span<const std::byte> data_;
    std::size_t position_{};
    bool malformed_{};
};

[[nodiscard]] TrackerParseError refusal(const Reader& reader) noexcept {
    return reader.malformed() ? TrackerParseError::UnrecognizedMessage
                              : TrackerParseError::Truncated;
}

// NatNet 4.1 prefixed every section after the frame number with its size in
// bytes, so a client can skip a section it does not read.
[[nodiscard]] constexpr bool sized_sections(NatNetVersion version) noexcept {
    return at_least(version, 4U, 1U);
}

// One rigid body record, in the shape `version` writes it. Returns false when
// the datagram ran out or the record is malformed; the reader says which.
struct RigidBody final {
    core::i32 id{};
    float x{};
    float y{};
    float z{};
    float qx{};
    float qy{};
    float qz{};
    float qw{};
    bool tracked{true};
};

[[nodiscard]] bool read_rigid_body(
    Reader& reader, NatNetVersion version, RigidBody& body) noexcept {
    if (!reader.read_i32(body.id) || !reader.read_f32(body.x)
        || !reader.read_f32(body.y) || !reader.read_f32(body.z)
        || !reader.read_f32(body.qx) || !reader.read_f32(body.qy)
        || !reader.read_f32(body.qz) || !reader.read_f32(body.qw)) {
        return false;
    }
    if (version.major < 3U) {
        // Versions before 3.0 carried the body's own markers inline.
        core::u32 markers = 0U;
        if (!reader.read_count(markers)) {
            return false;
        }
        std::size_t marker_bytes = static_cast<std::size_t>(markers) * 12U;
        if (version.major >= 2U) {
            // Marker ids, then marker sizes, from 2.0.
            marker_bytes += static_cast<std::size_t>(markers) * 8U;
        }
        if (!reader.skip(marker_bytes)) {
            return false;
        }
    }
    if (version.major >= 2U) {
        float mean_error = 0.0F;
        if (!reader.read_f32(mean_error)) {
            return false;
        }
    }
    if (at_least(version, 2U, 6U)) {
        core::i16 params = 0;
        if (!reader.read_i16(params)) {
            return false;
        }
        body.tracked = (static_cast<core::u16>(params) & 0x01U) != 0U;
    }
    return true;
}

// Skips one section. With sized sections the prefix says how far; without, the
// caller walks the records itself through `walk`.
template <typename Walk>
[[nodiscard]] bool section(
    Reader& reader, NatNetVersion version, Walk&& walk) noexcept {
    core::u32 count = 0U;
    if (!reader.read_count(count)) {
        return false;
    }
    if (sized_sections(version)) {
        core::u32 bytes = 0U;
        if (!reader.read_count(bytes)) {
            return false;
        }
        return reader.skip(bytes);
    }
    for (core::u32 index = 0U; index < count; ++index) {
        if (!walk(reader)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool walk_marker_set(Reader& reader) noexcept {
    core::u32 markers = 0U;
    return reader.skip_cstring() && reader.read_count(markers)
        && reader.skip(static_cast<std::size_t>(markers) * 12U);
}

[[nodiscard]] bool walk_channel_device(Reader& reader) noexcept {
    core::i32 id = 0;
    core::u32 channels = 0U;
    if (!reader.read_i32(id) || !reader.read_count(channels)) {
        return false;
    }
    for (core::u32 channel = 0U; channel < channels; ++channel) {
        core::u32 frames = 0U;
        if (!reader.read_count(frames)
            || !reader.skip(static_cast<std::size_t>(frames) * 4U)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Turns turns_from_radians(double radians) noexcept {
    constexpr double two_pi = 6.283185307179586476925286766559;
    return static_cast<Turns>(radians / two_pi);
}

} // namespace

std::optional<core::u16> natnet_message_id(
    std::span<const std::byte> datagram) noexcept {
    Reader reader{datagram};
    core::u16 id = 0U;
    if (!reader.read_u16(id)) {
        return std::nullopt;
    }
    return id;
}

std::array<std::byte, 4U> natnet_connect_request() noexcept {
    // Message id 0, payload length 0. The same four zero bytes the SDK's
    // sample client sends as its ping.
    return {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
}

InfoResult parse_natnet_server_info(
    std::span<const std::byte> datagram) noexcept {
    Reader reader{datagram};
    core::u16 id = 0U;
    core::u16 payload_bytes = 0U;
    if (!reader.read_u16(id) || !reader.read_u16(payload_bytes)) {
        return InfoResult::failure(TrackerParseError::Truncated);
    }
    if (id != natnet_server_info_message) {
        return InfoResult::failure(TrackerParseError::UnrecognizedMessage);
    }
    if (payload_bytes < server_info_common_bytes) {
        return InfoResult::failure(TrackerParseError::UnrecognizedMessage);
    }
    if (reader.remaining() < payload_bytes) {
        return InfoResult::failure(TrackerParseError::Truncated);
    }

    NatNetServerInfo info{};
    std::memcpy(info.application_name.data(),
                datagram.data() + header_bytes,
                info.application_name.size());
    // Terminated whatever the sender did, so a reader can treat it as a
    // C string.
    info.application_name.back() = '\0';
    static_cast<void>(reader.skip(info.application_name.size()));
    const auto read_version = [&](NatNetVersion& version) noexcept {
        const std::size_t at = reader.position();
        if (!reader.skip(4U)) {
            return false;
        }
        version = NatNetVersion{
            static_cast<core::u8>(datagram[at]),
            static_cast<core::u8>(datagram[at + 1U]),
            static_cast<core::u8>(datagram[at + 2U]),
            static_cast<core::u8>(datagram[at + 3U])};
        return true;
    };
    if (!read_version(info.application_version)
        || !read_version(info.natnet_version)) {
        return InfoResult::failure(TrackerParseError::Truncated);
    }
    if (info.natnet_version.major == 0U) {
        // A server that reports no version cannot be decoded against.
        return InfoResult::failure(TrackerParseError::InvalidValue);
    }

    if (payload_bytes >= server_info_common_bytes + server_info_transport_bytes) {
        core::u64 clock_frequency = 0U;
        core::u16 data_port = 0U;
        if (!reader.read_u64(clock_frequency) || !reader.read_u16(data_port)) {
            return InfoResult::failure(TrackerParseError::Truncated);
        }
        const std::size_t at = reader.position();
        if (reader.remaining() < 5U) {
            return InfoResult::failure(TrackerParseError::Truncated);
        }
        info.describes_transport = true;
        info.data_port = data_port;
        info.multicast = datagram[at] != std::byte{0};
        for (std::size_t index = 0U; index < 4U; ++index) {
            info.multicast_group[index] =
                static_cast<core::u8>(datagram[at + 1U + index]);
        }
    }
    return InfoResult::success(info);
}

PanTiltRoll pan_tilt_roll_from_quaternion(
    float x, float y, float z, float w, NatNetUpAxis up) noexcept {
    // Normalised in binary64, so a quaternion Motive rounded to binary32 still
    // describes a rotation rather than a rotation and a scale.
    double qx = x;
    double qy = y;
    double qz = z;
    double qw = w;
    const double norm = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    if (!(norm > 0.0) || !std::isfinite(norm)) {
        return PanTiltRoll{};
    }
    qx /= norm;
    qy /= norm;
    qz /= norm;
    qw /= norm;

    // A Z-up tracker's rotation, expressed in the stage's Y-up frame. The
    // scene maps a Z-up position (x, y, z) to (x, z, -y); conjugating a
    // rotation by that same proper rotation turns its axis the same way, so
    // the vector part of the quaternion is permuted identically.
    if (up == NatNetUpAxis::Z) {
        const double mapped_y = qz;
        const double mapped_z = -qy;
        qy = mapped_y;
        qz = mapped_z;
    }

    // The rotation matrix for column vectors, as `scene::rotation` builds it:
    // the composition Y(pan) * X(tilt) * Z(-roll) has
    //   m[0][2] = sin pan * cos tilt      m[2][2] = cos pan * cos tilt
    //   m[1][2] = -sin tilt
    //   m[1][0] = cos tilt * sin(-roll)   m[1][1] = cos tilt * cos(-roll)
    // which is what is read back below.
    const double m02 = 2.0 * (qx * qz + qy * qw);
    const double m12 = 2.0 * (qy * qz - qx * qw);
    const double m22 = 1.0 - 2.0 * (qx * qx + qy * qy);
    const double m10 = 2.0 * (qx * qy + qz * qw);
    const double m11 = 1.0 - 2.0 * (qx * qx + qz * qz);
    const double m00 = 1.0 - 2.0 * (qy * qy + qz * qz);
    const double m20 = 2.0 * (qx * qz - qy * qw);

    const double sin_tilt = std::clamp(-m12, -1.0, 1.0);
    const double tilt = std::asin(sin_tilt);
    PanTiltRoll result{};
    result.tilt = turns_from_radians(tilt);
    // Within a hair of straight up or down the pan and roll axes coincide and
    // the split between them is arbitrary. Roll is taken as zero there, so the
    // whole heading goes to pan - the number an operator is looking at.
    constexpr double gimbal = 1.0 - 1.0e-9;
    if (std::fabs(sin_tilt) >= gimbal) {
        result.pan = turns_from_radians(std::atan2(-m20, m00));
        result.roll = 0.0F;
        return result;
    }
    result.pan = turns_from_radians(std::atan2(m02, m22));
    result.roll = turns_from_radians(-std::atan2(m10, m11));
    return result;
}

FrameResult parse_natnet_frame(
    std::span<const std::byte> datagram,
    NatNetVersion version,
    core::u32 rigid_body_id,
    NatNetUpAxis up) noexcept {
    if (version.major == 0U) {
        // Nothing can be decoded against an unknown version; see the header.
        return FrameResult::failure(TrackerParseError::UnrecognizedMessage);
    }
    Reader header{datagram};
    core::u16 id = 0U;
    core::u16 payload_bytes = 0U;
    if (!header.read_u16(id) || !header.read_u16(payload_bytes)) {
        return FrameResult::failure(TrackerParseError::Truncated);
    }
    if (id != natnet_frame_message) {
        return FrameResult::failure(TrackerParseError::UnrecognizedMessage);
    }
    if (datagram.size() < header_bytes + payload_bytes) {
        return FrameResult::failure(TrackerParseError::Truncated);
    }
    if (datagram.size() != header_bytes + payload_bytes) {
        // Bytes after the declared payload are not this frame's.
        return FrameResult::failure(TrackerParseError::UnrecognizedMessage);
    }
    Reader reader{datagram.subspan(header_bytes, payload_bytes)};

    NatNetFrame frame{};
    core::i32 frame_number = 0;
    if (!reader.read_i32(frame_number)) {
        return FrameResult::failure(refusal(reader));
    }
    if (frame_number < 0) {
        return FrameResult::failure(TrackerParseError::InvalidValue);
    }
    frame.frame_number = static_cast<core::u32>(frame_number);

    // Marker sets, then unlabeled markers.
    if (!section(reader, version, &walk_marker_set)) {
        return FrameResult::failure(refusal(reader));
    }
    {
        core::u32 other_markers = 0U;
        if (!reader.read_count(other_markers)) {
            return FrameResult::failure(refusal(reader));
        }
        if (sized_sections(version)) {
            core::u32 bytes = 0U;
            if (!reader.read_count(bytes) || !reader.skip(bytes)) {
                return FrameResult::failure(refusal(reader));
            }
        } else if (!reader.skip(static_cast<std::size_t>(other_markers) * 12U)) {
            return FrameResult::failure(refusal(reader));
        }
    }

    // Rigid bodies: the section this parser is for. Walked record by record
    // whatever the version, and checked against the declared size where there
    // is one, because a size that disagrees with the records is the clearest
    // sign the version is wrong.
    RigidBody chosen{};
    {
        core::u32 count = 0U;
        if (!reader.read_count(count)) {
            return FrameResult::failure(refusal(reader));
        }
        core::u32 declared_bytes = 0U;
        if (sized_sections(version) && !reader.read_count(declared_bytes)) {
            return FrameResult::failure(refusal(reader));
        }
        const std::size_t started = reader.position();
        frame.rigid_bodies = count;
        for (core::u32 index = 0U; index < count; ++index) {
            RigidBody body{};
            if (!read_rigid_body(reader, version, body)) {
                return FrameResult::failure(refusal(reader));
            }
            const bool wanted = rigid_body_id == 0U
                ? index == 0U
                : body.id >= 0
                    && static_cast<core::u32>(body.id) == rigid_body_id;
            if (wanted && !frame.found) {
                frame.found = true;
                chosen = body;
            }
        }
        if (sized_sections(version)
            && reader.position() - started != declared_bytes) {
            return FrameResult::failure(TrackerParseError::UnrecognizedMessage);
        }
    }

    // Skeletons, from 2.1: an id and a list of rigid bodies each.
    if (at_least(version, 2U, 1U)) {
        const auto walk_skeleton = [version](Reader& inner) noexcept {
            core::i32 skeleton_id = 0;
            core::u32 bones = 0U;
            if (!inner.read_i32(skeleton_id) || !inner.read_count(bones)) {
                return false;
            }
            for (core::u32 bone = 0U; bone < bones; ++bone) {
                RigidBody ignored{};
                if (!read_rigid_body(inner, version, ignored)) {
                    return false;
                }
            }
            return true;
        };
        if (!section(reader, version, walk_skeleton)) {
            return FrameResult::failure(refusal(reader));
        }
    }
    // Trained marker set assets, from 4.1. Always size-prefixed, so they are
    // skipped by size rather than walked.
    if (at_least(version, 4U, 1U)) {
        if (!section(reader, version, [](Reader&) noexcept { return false; })) {
            return FrameResult::failure(refusal(reader));
        }
    }
    // Labeled markers, from 2.3.
    if (at_least(version, 2U, 3U)) {
        const auto walk_labeled = [version](Reader& inner) noexcept {
            core::i32 marker_id = 0;
            if (!inner.read_i32(marker_id) || !inner.skip(16U)) {
                return false;
            }
            if (at_least(version, 2U, 6U) && !inner.skip(2U)) {
                return false;
            }
            return !at_least(version, 3U, 0U) || inner.skip(4U);
        };
        if (!section(reader, version, walk_labeled)) {
            return FrameResult::failure(refusal(reader));
        }
    }
    // Force plates from 2.9, then devices from 3.0, both channel lists.
    if (at_least(version, 2U, 9U)
        && !section(reader, version, &walk_channel_device)) {
        return FrameResult::failure(refusal(reader));
    }
    if (at_least(version, 3U, 0U)
        && !section(reader, version, &walk_channel_device)) {
        return FrameResult::failure(refusal(reader));
    }

    // Timing.
    if (version.major < 3U) {
        float software_latency = 0.0F;
        if (!reader.read_f32(software_latency)) {
            return FrameResult::failure(refusal(reader));
        }
    }
    core::u32 timecode = 0U;
    core::u32 timecode_sub = 0U;
    if (!reader.read_u32(timecode) || !reader.read_u32(timecode_sub)) {
        return FrameResult::failure(refusal(reader));
    }
    if (at_least(version, 2U, 7U)) {
        if (!reader.read_f64(frame.timestamp)) {
            return FrameResult::failure(refusal(reader));
        }
    } else {
        float timestamp = 0.0F;
        if (!reader.read_f32(timestamp)) {
            return FrameResult::failure(refusal(reader));
        }
        frame.timestamp = timestamp;
    }
    if (at_least(version, 3U, 0U)) {
        // Camera mid-exposure, camera data received, and transmit timestamps.
        core::u64 ignored = 0U;
        if (!reader.read_u64(ignored) || !reader.read_u64(ignored)
            || !reader.read_u64(ignored)) {
            return FrameResult::failure(refusal(reader));
        }
    }
    if (at_least(version, 4U, 1U)) {
        // Precision timestamp: whole seconds and fractional seconds.
        core::u32 ignored = 0U;
        if (!reader.read_u32(ignored) || !reader.read_u32(ignored)) {
            return FrameResult::failure(refusal(reader));
        }
    }
    core::i16 frame_params = 0;
    core::i32 end_of_data = 0;
    if (!reader.read_i16(frame_params) || !reader.read_i32(end_of_data)) {
        return FrameResult::failure(refusal(reader));
    }
    if (!reader.exhausted()) {
        // The walk did not land on the end of the payload: the frame was
        // written by a version other than the one claimed.
        return FrameResult::failure(TrackerParseError::UnrecognizedMessage);
    }
    if (!std::isfinite(frame.timestamp)) {
        return FrameResult::failure(TrackerParseError::InvalidValue);
    }

    if (!frame.found) {
        return FrameResult::success(frame);
    }
    frame.tracked = chosen.tracked;
    if (!frame.tracked) {
        // Listed but not solved this frame. Its numbers are stale and are
        // deliberately not carried; the receiver holds its last good pose.
        return FrameResult::success(frame);
    }
    if (!std::isfinite(chosen.x) || !std::isfinite(chosen.y)
        || !std::isfinite(chosen.z) || !std::isfinite(chosen.qx)
        || !std::isfinite(chosen.qy) || !std::isfinite(chosen.qz)
        || !std::isfinite(chosen.qw)) {
        return FrameResult::failure(TrackerParseError::InvalidValue);
    }
    const double norm_squared =
        static_cast<double>(chosen.qx) * chosen.qx
        + static_cast<double>(chosen.qy) * chosen.qy
        + static_cast<double>(chosen.qz) * chosen.qz
        + static_cast<double>(chosen.qw) * chosen.qw;
    // Motive writes unit quaternions rounded to binary32. Anything far from
    // unit is not a rotation, and a zero quaternion is what an unsolved body
    // carries in versions too old to flag it.
    if (std::fabs(norm_squared - 1.0) > 1.0e-3) {
        return FrameResult::failure(TrackerParseError::InvalidValue);
    }

    TrackedPose& pose = frame.pose;
    pose.protocol = TrackerProtocol::NatNet;
    pose.camera_id = static_cast<core::u32>(chosen.id);
    pose.sequence = frame.frame_number;
    pose.x = chosen.x;
    pose.y = chosen.y;
    pose.z = chosen.z;
    const PanTiltRoll angles = pan_tilt_roll_from_quaternion(
        chosen.qx, chosen.qy, chosen.qz, chosen.qw, up);
    pose.pan = angles.pan;
    pose.tilt = angles.tilt;
    pose.roll = angles.roll;
    // Motive tracks the body, not the lens: no zoom, focus or iris. Every
    // encoder is left at its explicit "none" form, so the authored fallback
    // focal length applies, as it does for FreeD without a calibration table.
    if (!valid(pose)) {
        return FrameResult::failure(TrackerParseError::InvalidValue);
    }
    return FrameResult::success(frame);
}

} // namespace mediaserver::track
