#include <mediaserver/track/natnet.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
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

#define NATNET_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] bool near(float value, float expected, float tolerance) noexcept {
    return std::fabs(value - expected) <= tolerance;
}

constexpr track::NatNetVersion v2_9{2U, 9U, 0U, 0U};
constexpr track::NatNetVersion v3_0{3U, 0U, 0U, 0U};
constexpr track::NatNetVersion v3_1{3U, 1U, 0U, 0U};
constexpr track::NatNetVersion v4_0{4U, 0U, 0U, 0U};
constexpr track::NatNetVersion v4_1{4U, 1U, 0U, 0U};

// Little-endian byte writer, so a frame is assembled the way the SDK's packet
// client reads it rather than by reusing the parser's own reader.
struct Bytes final {
    std::vector<std::byte> data;

    void u8(core::u8 value) { data.push_back(static_cast<std::byte>(value)); }
    void u16(core::u16 value) {
        u8(static_cast<core::u8>(value & 0xFFU));
        u8(static_cast<core::u8>(value >> 8U));
    }
    void i16(core::i16 value) { u16(static_cast<core::u16>(value)); }
    void u32(core::u32 value) {
        u16(static_cast<core::u16>(value & 0xFFFFU));
        u16(static_cast<core::u16>(value >> 16U));
    }
    void i32(core::i32 value) { u32(static_cast<core::u32>(value)); }
    void u64(core::u64 value) {
        u32(static_cast<core::u32>(value & 0xFFFF'FFFFU));
        u32(static_cast<core::u32>(value >> 32U));
    }
    void f32(float value) { u32(std::bit_cast<core::u32>(value)); }
    void f64(double value) { u64(std::bit_cast<core::u64>(value)); }
    void cstring(std::string_view text) {
        for (const char c : text) {
            u8(static_cast<core::u8>(c));
        }
        u8(0U);
    }
    void append(const Bytes& other) {
        data.insert(data.end(), other.data.begin(), other.data.end());
    }
};

struct Body final {
    core::i32 id{};
    std::array<float, 3U> position{};
    // x, y, z, w
    std::array<float, 4U> quaternion{0.0F, 0.0F, 0.0F, 1.0F};
    bool tracked{true};
};

[[nodiscard]] bool sized(track::NatNetVersion version) noexcept {
    return track::at_least(version, 4U, 1U);
}

void write_body(Bytes& out, track::NatNetVersion version, const Body& body) {
    out.i32(body.id);
    for (const float value : body.position) {
        out.f32(value);
    }
    for (const float value : body.quaternion) {
        out.f32(value);
    }
    if (version.major < 3U) {
        // Two inline markers with ids and sizes, as 2.x wrote them.
        out.i32(2);
        for (int marker = 0; marker < 2; ++marker) {
            out.f32(0.1F);
            out.f32(0.2F);
            out.f32(0.3F);
        }
        out.i32(11);
        out.i32(12);
        out.f32(0.01F);
        out.f32(0.01F);
    }
    out.f32(0.0005F); // mean error
    if (track::at_least(version, 2U, 6U)) {
        out.i16(body.tracked ? 1 : 0);
    }
}

// Emits a section: count, then (4.1+) the byte size of what follows, then the
// records themselves.
void write_section(Bytes& out, track::NatNetVersion version, core::u32 count,
                   const Bytes& records) {
    out.u32(count);
    if (sized(version)) {
        out.u32(static_cast<core::u32>(records.data.size()));
    }
    out.append(records);
}

// A complete frame of data as the given version writes it, populated with
// every kind of section so the parser has to walk past all of them.
[[nodiscard]] std::vector<std::byte> frame(
    track::NatNetVersion version, core::u32 frame_number,
    const std::vector<Body>& bodies) {
    Bytes payload;
    payload.u32(frame_number);

    // One marker set with three markers.
    {
        Bytes records;
        records.cstring("CameraRig");
        records.i32(3);
        for (int marker = 0; marker < 9; ++marker) {
            records.f32(static_cast<float>(marker) * 0.25F);
        }
        write_section(payload, version, 1U, records);
    }
    // Two unlabeled markers.
    {
        Bytes records;
        for (int value = 0; value < 6; ++value) {
            records.f32(1.0F);
        }
        write_section(payload, version, 2U, records);
    }
    // Rigid bodies.
    {
        Bytes records;
        for (const Body& body : bodies) {
            write_body(records, version, body);
        }
        write_section(payload, version,
                      static_cast<core::u32>(bodies.size()), records);
    }
    // One skeleton with two bones (2.1+).
    if (track::at_least(version, 2U, 1U)) {
        Bytes records;
        records.i32(100);
        records.i32(2);
        write_body(records, version, Body{101, {0.0F, 1.0F, 0.0F}});
        write_body(records, version, Body{102, {0.0F, 1.5F, 0.0F}});
        write_section(payload, version, 1U, records);
    }
    // One trained asset (4.1+), skipped by size.
    if (track::at_least(version, 4U, 1U)) {
        Bytes records;
        records.i32(500);
        records.i32(1);
        write_body(records, version, Body{501, {2.0F, 0.0F, 0.0F}});
        records.i32(0);
        write_section(payload, version, 1U, records);
    }
    // Two labeled markers (2.3+).
    if (track::at_least(version, 2U, 3U)) {
        Bytes records;
        for (int marker = 0; marker < 2; ++marker) {
            records.i32(1000 + marker);
            records.f32(0.0F);
            records.f32(0.0F);
            records.f32(0.0F);
            records.f32(0.012F);
            if (track::at_least(version, 2U, 6U)) {
                records.i16(0);
            }
            if (track::at_least(version, 3U, 0U)) {
                records.f32(0.0001F);
            }
        }
        write_section(payload, version, 2U, records);
    }
    // One force plate with two channels (2.9+).
    if (track::at_least(version, 2U, 9U)) {
        Bytes records;
        records.i32(7);
        records.i32(2);
        for (int channel = 0; channel < 2; ++channel) {
            records.i32(3);
            records.f32(1.0F);
            records.f32(2.0F);
            records.f32(3.0F);
        }
        write_section(payload, version, 1U, records);
    }
    // One device with one channel (3.0+).
    if (track::at_least(version, 3U, 0U)) {
        Bytes records;
        records.i32(8);
        records.i32(1);
        records.i32(1);
        records.f32(5.0F);
        write_section(payload, version, 1U, records);
    }

    if (version.major < 3U) {
        payload.f32(0.002F); // software latency
    }
    payload.u32(0x01020304U); // timecode
    payload.u32(0x00000005U); // timecode subframe
    if (track::at_least(version, 2U, 7U)) {
        payload.f64(1234.5);
    } else {
        payload.f32(1234.5F);
    }
    if (track::at_least(version, 3U, 0U)) {
        payload.u64(1'000U);
        payload.u64(2'000U);
        payload.u64(3'000U);
    }
    if (track::at_least(version, 4U, 1U)) {
        payload.u32(1234U);
        payload.u32(500U);
    }
    payload.i16(0); // frame params
    payload.i32(0); // end of data

    Bytes datagram;
    datagram.u16(track::natnet_frame_message);
    datagram.u16(static_cast<core::u16>(payload.data.size()));
    datagram.append(payload);
    return datagram.data;
}

[[nodiscard]] std::vector<std::byte> server_info(
    std::string_view name, track::NatNetVersion app,
    track::NatNetVersion natnet, bool with_transport) {
    Bytes payload;
    std::array<char, 256U> name_field{};
    std::memcpy(name_field.data(), name.data(),
                std::min(name.size(), name_field.size() - 1U));
    for (const char c : name_field) {
        payload.u8(static_cast<core::u8>(c));
    }
    for (const core::u8 value : {app.major, app.minor, app.build, app.revision}) {
        payload.u8(value);
    }
    for (const core::u8 value :
         {natnet.major, natnet.minor, natnet.build, natnet.revision}) {
        payload.u8(value);
    }
    if (with_transport) {
        payload.u64(10'000'000U); // high-resolution clock frequency
        payload.u16(1511U);
        payload.u8(1U); // multicast
        payload.u8(239U);
        payload.u8(255U);
        payload.u8(42U);
        payload.u8(99U);
        payload.u8(0U); // struct padding the sender's sizeof carries
    }
    Bytes datagram;
    datagram.u16(track::natnet_server_info_message);
    datagram.u16(static_cast<core::u16>(payload.data.size()));
    datagram.append(payload);
    return datagram.data;
}

// A unit quaternion for a rotation of `turns` about a unit axis.
[[nodiscard]] std::array<float, 4U> about(
    std::array<double, 3U> axis, double turns) {
    const double half = turns * 3.14159265358979323846;
    const double s = std::sin(half);
    return {static_cast<float>(axis[0] * s), static_cast<float>(axis[1] * s),
            static_cast<float>(axis[2] * s), static_cast<float>(std::cos(half))};
}

// Hamilton product a * b, both (x, y, z, w).
[[nodiscard]] std::array<double, 4U> multiply(
    std::array<double, 4U> a, std::array<double, 4U> b) {
    return {
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    };
}

// The quaternion of Y(pan) * X(tilt) * Z(-roll), the composition
// `scene::rotation` performs, built independently of the parser.
[[nodiscard]] std::array<float, 4U> quaternion_of(
    double pan, double tilt, double roll) {
    const auto axis = [](std::array<double, 3U> unit, double turns) {
        const double half = turns * 3.14159265358979323846;
        return std::array<double, 4U>{unit[0] * std::sin(half),
                                      unit[1] * std::sin(half),
                                      unit[2] * std::sin(half), std::cos(half)};
    };
    const auto q = multiply(
        axis({0.0, 1.0, 0.0}, pan),
        multiply(axis({1.0, 0.0, 0.0}, tilt), axis({0.0, 0.0, 1.0}, -roll)));
    return {static_cast<float>(q[0]), static_cast<float>(q[1]),
            static_cast<float>(q[2]), static_cast<float>(q[3])};
}

void the_connect_request_and_message_ids_are_the_protocols_own() {
    const auto request = track::natnet_connect_request();
    NATNET_CHECK(request.size() == 4U);
    for (const std::byte value : request) {
        NATNET_CHECK(value == std::byte{0});
    }
    NATNET_CHECK(track::natnet_message_id(request) == 0U);

    const auto info = server_info("Motive", v3_1, v4_1, true);
    NATNET_CHECK(track::natnet_message_id(info)
                 == track::natnet_server_info_message);
    const auto data = frame(v4_1, 1U, {});
    NATNET_CHECK(track::natnet_message_id(data) == track::natnet_frame_message);

    const std::array<std::byte, 1U> stub{std::byte{7}};
    NATNET_CHECK(!track::natnet_message_id(stub).has_value());
}

void server_info_reports_the_version_and_the_transport() {
    const auto modern = track::parse_natnet_server_info(
        server_info("Motive", track::NatNetVersion{3U, 1U, 2U, 0U}, v4_1, true));
    NATNET_CHECK(modern.has_value());
    NATNET_CHECK(std::string_view{modern.value().application_name.data()}
                 == "Motive");
    NATNET_CHECK(modern.value().application_version
                 == (track::NatNetVersion{3U, 1U, 2U, 0U}));
    NATNET_CHECK(modern.value().natnet_version == v4_1);
    NATNET_CHECK(modern.value().describes_transport);
    NATNET_CHECK(modern.value().data_port == 1511U);
    NATNET_CHECK(modern.value().multicast);
    NATNET_CHECK(modern.value().multicast_group
                 == track::natnet_default_multicast_group);

    // An older server says who it is and nothing about its transport.
    const auto old = track::parse_natnet_server_info(
        server_info("Motive", track::NatNetVersion{1U, 10U, 0U, 0U}, v2_9,
                    false));
    NATNET_CHECK(old.has_value());
    NATNET_CHECK(old.value().natnet_version == v2_9);
    NATNET_CHECK(!old.value().describes_transport);
    NATNET_CHECK(old.value().data_port == 0U);

    // A frame is not a server reply, a short reply is short, and a reply with
    // no version cannot be decoded against.
    const auto wrong = track::parse_natnet_server_info(frame(v3_1, 1U, {}));
    NATNET_CHECK(!wrong && wrong.error() == track::TrackerParseError::UnrecognizedMessage);
    auto truncated = server_info("Motive", v3_1, v4_1, false);
    truncated.resize(truncated.size() - 10U);
    const auto cut = track::parse_natnet_server_info(truncated);
    NATNET_CHECK(!cut && cut.error() == track::TrackerParseError::Truncated);
    const auto versionless = track::parse_natnet_server_info(server_info(
        "Motive", v3_1, track::NatNetVersion{}, false));
    NATNET_CHECK(!versionless
                 && versionless.error() == track::TrackerParseError::InvalidValue);
}

void a_rigid_body_is_picked_by_its_streaming_id() {
    const Body other{2, {9.0F, 9.0F, 9.0F}};
    // Camera at (1, 2, -3), turned a quarter turn about +Y.
    const Body camera{5, {1.0F, 2.0F, -3.0F}, about({0.0, 1.0, 0.0}, 0.25)};
    const auto datagram = frame(v3_1, 4'242U, {other, camera});

    const auto parsed = track::parse_natnet_frame(datagram, v3_1, 5U,
                                                  track::NatNetUpAxis::Y);
    NATNET_CHECK(parsed.has_value());
    const track::NatNetFrame& decoded = parsed.value();
    NATNET_CHECK(decoded.frame_number == 4'242U);
    NATNET_CHECK(decoded.rigid_bodies == 2U);
    NATNET_CHECK(decoded.found);
    NATNET_CHECK(decoded.tracked);
    NATNET_CHECK(decoded.timestamp == 1234.5);

    const track::TrackedPose& pose = decoded.pose;
    NATNET_CHECK(pose.protocol == track::TrackerProtocol::NatNet);
    NATNET_CHECK(pose.camera_id == 5U);
    NATNET_CHECK(pose.sequence == 4'242U);
    NATNET_CHECK(pose.x == 1.0F && pose.y == 2.0F && pose.z == -3.0F);
    NATNET_CHECK(near(pose.pan, 0.25F, 1.0e-6F));
    NATNET_CHECK(near(pose.tilt, 0.0F, 1.0e-6F));
    NATNET_CHECK(near(pose.roll, 0.0F, 1.0e-6F));
    // Motive tracks the body, not the lens.
    NATNET_CHECK(pose.focal_length_mm == 0.0F);
    NATNET_CHECK(!pose.zoom_encoder_valid && !pose.focus_encoder_valid
                 && !pose.iris_encoder_valid);
    NATNET_CHECK(track::valid(pose));

    // Id zero takes the first body in the frame, whatever its id.
    const auto first = track::parse_natnet_frame(datagram, v3_1, 0U,
                                                 track::NatNetUpAxis::Y);
    NATNET_CHECK(first && first.value().found);
    NATNET_CHECK(first.value().pose.camera_id == 2U);
    NATNET_CHECK(first.value().pose.x == 9.0F);

    // A body that is not in the frame is reported absent, not invented.
    const auto absent = track::parse_natnet_frame(datagram, v3_1, 9U,
                                                  track::NatNetUpAxis::Y);
    NATNET_CHECK(absent && !absent.value().found);
    NATNET_CHECK(absent.value().rigid_bodies == 2U);
    NATNET_CHECK(absent.value().pose == track::TrackedPose{});

    // An empty frame is a valid frame with nobody in it.
    const auto empty = track::parse_natnet_frame(frame(v3_1, 1U, {}), v3_1, 0U,
                                                 track::NatNetUpAxis::Y);
    NATNET_CHECK(empty && !empty.value().found
                 && empty.value().rigid_bodies == 0U);
}

void an_occluded_body_is_reported_untracked_and_carries_no_pose() {
    Body camera{5, {1.0F, 2.0F, -3.0F}};
    camera.tracked = false;
    const auto parsed = track::parse_natnet_frame(
        frame(v3_1, 7U, {camera}), v3_1, 5U, track::NatNetUpAxis::Y);
    NATNET_CHECK(parsed.has_value());
    NATNET_CHECK(parsed.value().found);
    NATNET_CHECK(!parsed.value().tracked);
    // The stale numbers are deliberately not carried.
    NATNET_CHECK(parsed.value().pose == track::TrackedPose{});
}

void every_supported_layout_decodes_against_its_own_version() {
    const Body camera{3, {0.5F, 1.5F, 2.5F}, about({0.0, 1.0, 0.0}, -0.125)};
    for (const track::NatNetVersion version : {v2_9, v3_0, v3_1, v4_0, v4_1}) {
        const auto datagram = frame(version, 99U, {Body{1}, camera, Body{4}});
        const auto parsed = track::parse_natnet_frame(
            datagram, version, 3U, track::NatNetUpAxis::Y);
        NATNET_CHECK(parsed.has_value());
        NATNET_CHECK(parsed.value().found && parsed.value().tracked);
        NATNET_CHECK(parsed.value().rigid_bodies == 3U);
        NATNET_CHECK(parsed.value().pose.x == 0.5F);
        NATNET_CHECK(parsed.value().pose.z == 2.5F);
        NATNET_CHECK(near(parsed.value().pose.pan, -0.125F, 1.0e-6F));
        NATNET_CHECK(parsed.value().timestamp == 1234.5);
    }
}

void a_frame_read_with_the_wrong_version_is_refused_whole() {
    const Body camera{3, {0.5F, 1.5F, 2.5F}};
    // The layouts differ in where sections begin and end, so a frame walked
    // with another version's rules does not land on its end-of-data tag.
    //
    // 3.0 and 4.0 share a layout - 4.0 changed the connect handshake, not the
    // frame - so a 3.0 frame read as 4.0 is correctly accepted, and only the
    // three distinct layouts are set against one another here.
    const std::array<track::NatNetVersion, 3U> versions{v2_9, v3_0, v4_1};
    const auto same_layout_as_3_0 = track::parse_natnet_frame(
        frame(v3_0, 5U, {camera}), v4_0, 3U, track::NatNetUpAxis::Y);
    NATNET_CHECK(same_layout_as_3_0 && same_layout_as_3_0.value().found);
    for (const track::NatNetVersion wrote : versions) {
        const auto datagram = frame(wrote, 5U, {camera});
        for (const track::NatNetVersion read : versions) {
            if (read == wrote) {
                continue;
            }
            const auto parsed = track::parse_natnet_frame(
                datagram, read, 3U, track::NatNetUpAxis::Y);
            NATNET_CHECK(!parsed.has_value());
            NATNET_CHECK(parsed.error() == track::TrackerParseError::UnrecognizedMessage
                         || parsed.error() == track::TrackerParseError::Truncated);
        }
    }
    // And an unknown version decodes nothing at all.
    const auto unknown = track::parse_natnet_frame(
        frame(v4_1, 5U, {camera}), track::NatNetVersion{}, 3U,
        track::NatNetUpAxis::Y);
    NATNET_CHECK(!unknown
                 && unknown.error() == track::TrackerParseError::UnrecognizedMessage);
}

void a_short_or_foreign_datagram_is_refused() {
    const Body camera{3, {0.5F, 1.5F, 2.5F}};
    const auto good = frame(v4_1, 5U, {camera});

    // Cut anywhere: inside the rigid bodies, inside the timing, one byte.
    const std::array<std::size_t, 4U> cuts{good.size() - 1U, good.size() - 20U,
                                           40U, 3U};
    for (const std::size_t keep : cuts) {
        const auto parsed = track::parse_natnet_frame(
            std::span<const std::byte>{good.data(), keep}, v4_1, 3U,
            track::NatNetUpAxis::Y);
        NATNET_CHECK(!parsed.has_value());
        NATNET_CHECK(parsed.error() == track::TrackerParseError::Truncated
                     || parsed.error() == track::TrackerParseError::UnrecognizedMessage);
    }
    // Trailing bytes past the declared payload are not this frame's.
    auto padded = good;
    padded.push_back(std::byte{0});
    const auto extra = track::parse_natnet_frame(padded, v4_1, 3U,
                                                 track::NatNetUpAxis::Y);
    NATNET_CHECK(!extra && extra.error() == track::TrackerParseError::UnrecognizedMessage);
    // A server reply is not a frame.
    const auto reply = track::parse_natnet_frame(
        server_info("Motive", v3_1, v4_1, true), v4_1, 3U,
        track::NatNetUpAxis::Y);
    NATNET_CHECK(!reply && reply.error() == track::TrackerParseError::UnrecognizedMessage);
}

void a_body_that_is_not_a_rotation_is_refused() {
    // The zero quaternion an unsolved body carried before 2.6 flagged it.
    Body zero{3, {0.5F, 1.5F, 2.5F}, {0.0F, 0.0F, 0.0F, 0.0F}};
    const auto unsolved = track::parse_natnet_frame(
        frame(v3_1, 5U, {zero}), v3_1, 3U, track::NatNetUpAxis::Y);
    NATNET_CHECK(!unsolved && unsolved.error() == track::TrackerParseError::InvalidValue);

    Body nan{3, {0.5F, std::nanf(""), 2.5F}};
    const auto non_finite = track::parse_natnet_frame(
        frame(v3_1, 5U, {nan}), v3_1, 3U, track::NatNetUpAxis::Y);
    NATNET_CHECK(!non_finite
                 && non_finite.error() == track::TrackerParseError::InvalidValue);
}

void the_quaternion_decomposition_matches_the_scenes_composition() {
    // Triples across the range, each composed the way `scene::rotation` does
    // and then decomposed; the parser must hand back what went in.
    const std::array<std::array<double, 3U>, 8U> triples{{
        {0.0, 0.0, 0.0},
        {0.125, 0.0, 0.0},
        {0.0, 0.1, 0.0},
        {0.0, 0.0, 0.05},
        {-0.3, 0.2, -0.1},
        {0.45, -0.2, 0.2},
        {0.1, 0.24, 0.0},
        {-0.49, 0.05, 0.3},
    }};
    for (const auto& [pan, tilt, roll] : triples) {
        const auto q = quaternion_of(pan, tilt, roll);
        const auto angles = track::pan_tilt_roll_from_quaternion(
            q[0], q[1], q[2], q[3], track::NatNetUpAxis::Y);
        NATNET_CHECK(near(angles.pan, static_cast<float>(pan), 2.0e-6F));
        NATNET_CHECK(near(angles.tilt, static_cast<float>(tilt), 2.0e-6F));
        NATNET_CHECK(near(angles.roll, static_cast<float>(roll), 2.0e-6F));
    }

    // Straight up: pan and roll coincide, and the whole heading goes to pan.
    const auto up = quaternion_of(0.2, 0.25, 0.0);
    const auto locked = track::pan_tilt_roll_from_quaternion(
        up[0], up[1], up[2], up[3], track::NatNetUpAxis::Y);
    NATNET_CHECK(near(locked.tilt, 0.25F, 2.0e-6F));
    NATNET_CHECK(near(locked.pan, 0.2F, 2.0e-6F));
    NATNET_CHECK(locked.roll == 0.0F);

    // A degenerate quaternion is the identity rather than a NaN.
    NATNET_CHECK(track::pan_tilt_roll_from_quaternion(0.0F, 0.0F, 0.0F, 0.0F,
                                                      track::NatNetUpAxis::Y)
                 == track::PanTiltRoll{});
}

void a_z_up_tracker_pans_about_its_own_up() {
    // A quarter turn about the tracker's +Z is a quarter turn of pan on a
    // Z-up volume, and would read as roll on a Y-up one.
    const auto q = about({0.0, 0.0, 1.0}, 0.25);
    const auto z_up = track::pan_tilt_roll_from_quaternion(
        q[0], q[1], q[2], q[3], track::NatNetUpAxis::Z);
    NATNET_CHECK(near(z_up.pan, 0.25F, 1.0e-6F));
    NATNET_CHECK(near(z_up.tilt, 0.0F, 1.0e-6F));
    NATNET_CHECK(near(z_up.roll, 0.0F, 1.0e-6F));
    const auto y_up = track::pan_tilt_roll_from_quaternion(
        q[0], q[1], q[2], q[3], track::NatNetUpAxis::Y);
    NATNET_CHECK(near(y_up.pan, 0.0F, 1.0e-6F));
    NATNET_CHECK(near(std::fabs(y_up.roll), 0.25F, 1.0e-6F));

    // Tilt on a Z-up volume is about its +X, the same axis as the stage's.
    const auto tilt = about({1.0, 0.0, 0.0}, 0.1);
    const auto tilted = track::pan_tilt_roll_from_quaternion(
        tilt[0], tilt[1], tilt[2], tilt[3], track::NatNetUpAxis::Z);
    NATNET_CHECK(near(tilted.tilt, 0.1F, 1.0e-6F));
    NATNET_CHECK(near(tilted.pan, 0.0F, 1.0e-6F));

    // And the frame parser applies the same mapping.
    const Body camera{3, {0.5F, 1.5F, 2.5F}, q};
    const auto parsed = track::parse_natnet_frame(
        frame(v4_1, 5U, {camera}), v4_1, 3U, track::NatNetUpAxis::Z);
    NATNET_CHECK(parsed && near(parsed.value().pose.pan, 0.25F, 1.0e-6F));
    // Positions are left in the tracker's frame for the scene to map.
    NATNET_CHECK(parsed.value().pose.y == 1.5F && parsed.value().pose.z == 2.5F);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 10U> cases{{
    {"the connect request and message ids are the protocol's own",
     &the_connect_request_and_message_ids_are_the_protocols_own},
    {"server info reports the version and the transport",
     &server_info_reports_the_version_and_the_transport},
    {"a rigid body is picked by its streaming id",
     &a_rigid_body_is_picked_by_its_streaming_id},
    {"an occluded body is reported untracked and carries no pose",
     &an_occluded_body_is_reported_untracked_and_carries_no_pose},
    {"every supported layout decodes against its own version",
     &every_supported_layout_decodes_against_its_own_version},
    {"a frame read with the wrong version is refused whole",
     &a_frame_read_with_the_wrong_version_is_refused_whole},
    {"a short or foreign datagram is refused",
     &a_short_or_foreign_datagram_is_refused},
    {"a body that is not a rotation is refused",
     &a_body_that_is_not_a_rotation_is_refused},
    {"the quaternion decomposition matches the scene's composition",
     &the_quaternion_decomposition_matches_the_scenes_composition},
    {"a z-up tracker pans about its own up",
     &a_z_up_tracker_pans_about_its_own_up},
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
