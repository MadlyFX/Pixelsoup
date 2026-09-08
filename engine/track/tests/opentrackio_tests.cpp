#include <mediaserver/track/opentrackio.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace track = mediaserver::track;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define OTIO_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] std::span<const std::byte> bytes(std::string_view text) noexcept {
    return std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

[[nodiscard]] bool near(float value, float expected, float tolerance) noexcept {
    return std::fabs(value - expected) <= tolerance;
}

// A sample shaped like the ones the specification publishes: the fields this
// parser reads, surrounded by the ones it must validate and ignore.
constexpr std::string_view complete_sample = R"({
  "static": {
    "duration": {"num": 1, "denom": 25},
    "camera": {
      "activeSensorPhysicalDimensions": {"height": 24.0, "width": 36.0},
      "make": "CameraMaker",
      "anamorphicSqueeze": {"num": 1, "denom": 1}
    },
    "lens": {"make": "LensMaker", "nominalFocalLength": 14.0}
  },
  "tracker": {"notes": "Example", "recording": false, "slate": "A101_A_4",
              "status": "Optical Good"},
  "timing": {
    "mode": "internal",
    "sampleRate": {"num": 24000, "denom": 1001},
    "sequenceNumber": 182,
    "timecode": {"hours": 1, "minutes": 2, "seconds": 3, "frames": 4}
  },
  "lens": {
    "distortion": [{"model": "Brown-Conrady", "radial": [1.0, 2.0, 3.0]}],
    "encoders": {"focus": 0.1, "iris": 0.2, "zoom": 0.3},
    "entrancePupilOffset": 0.1234,
    "fStop": 4.0,
    "focalLength": 24.305,
    "focusDistance": 10.0,
    "pinholeFocalLength": 24.0
  },
  "protocol": {"name": "OpenTrackIO", "version": [1, 0, 0]},
  "sampleId": "urn:uuid:5ca5f233-11b5-4f43-8815-948d73e48a33",
  "sourceId": "urn:uuid:f81d4fae-7dec-11d0-a765-00a0c91e6bf6",
  "sourceNumber": 1,
  "globalStage": {"E": 100.0, "N": 200.0, "U": 300.0,
                  "lateral": 1.0, "longitudinal": 2.0, "vertical": 3.0},
  "transforms": [
    {"translation": {"x": 1.0, "y": 2.0, "z": 3.0},
     "rotation": {"pan": 180.0, "tilt": 90.0, "roll": 45.0},
     "id": "Dolly"},
    {"translation": {"x": 1.5, "y": -0.75, "z": 4.25},
     "rotation": {"pan": 90.0, "tilt": -30.0, "roll": 12.5},
     "id": "Camera"}
  ]
})";

void a_complete_sample_produces_a_known_pose() {
    const auto parsed = track::parse_opentrackio(bytes(complete_sample), 7U);
    OTIO_CHECK(parsed);
    const track::OpenTrackIOSample& sample = parsed.value();

    OTIO_CHECK(sample.pose.protocol == track::TrackerProtocol::OpenTrackIO);
    OTIO_CHECK(sample.named_camera_transform);
    OTIO_CHECK(sample.version_major == 1U);
    // The sample carried a sequence, so the fallback is not used.
    OTIO_CHECK(sample.pose.sequence == 182U);

    OTIO_CHECK(sample.pose.x == 1.5F);
    OTIO_CHECK(sample.pose.y == -0.75F);
    OTIO_CHECK(sample.pose.z == 4.25F);
    OTIO_CHECK(sample.pose.pan == 0.25F);
    OTIO_CHECK(near(sample.pose.tilt, -30.0F / 360.0F, 1e-7F));
    OTIO_CHECK(near(sample.pose.roll, 12.5F / 360.0F, 1e-7F));
    OTIO_CHECK(near(sample.pose.focal_length_mm, 24.305F, 1e-4F));
    OTIO_CHECK(sample.pose.focus_distance_m == 10.0F);
    OTIO_CHECK(sample.pose.aperture_f_number == 4.0F);
    OTIO_CHECK(near(sample.entrance_pupil_offset_m, 0.1234F, 1e-6F));
    OTIO_CHECK(track::valid(sample.pose));
}

void the_named_camera_transform_wins_over_the_chain_order() {
    // The camera is first here and a dolly last, so a parser that simply took
    // the last entry would pick the wrong one.
    constexpr std::string_view text = R"({
      "protocol": {"name": "OpenTrackIO", "version": [1,0,0]},
      "transforms": [
        {"id": "Camera",
         "translation": {"x": 9.0, "y": 0.0, "z": 0.0},
         "rotation": {"pan": 0.0, "tilt": 0.0, "roll": 0.0}},
        {"id": "Dolly",
         "translation": {"x": -9.0, "y": 0.0, "z": 0.0},
         "rotation": {"pan": 0.0, "tilt": 0.0, "roll": 0.0}}
      ]})";
    const auto parsed = track::parse_opentrackio(bytes(text), 1U);
    OTIO_CHECK(parsed);
    OTIO_CHECK(parsed.value().pose.x == 9.0F);
    OTIO_CHECK(parsed.value().named_camera_transform);
}

void an_unnamed_chain_falls_back_to_its_last_entry() {
    constexpr std::string_view text = R"({
      "protocol": {"name": "OpenTrackIO"},
      "transforms": [
        {"translation": {"x": 1.0, "y": 0.0, "z": 0.0},
         "rotation": {"pan": 0.0, "tilt": 0.0, "roll": 0.0}},
        {"translation": {"x": 2.0, "y": 0.0, "z": 0.0},
         "rotation": {"pan": 0.0, "tilt": 0.0, "roll": 0.0}}
      ]})";
    const auto parsed = track::parse_opentrackio(bytes(text), 5U);
    OTIO_CHECK(parsed);
    OTIO_CHECK(parsed.value().pose.x == 2.0F);
    OTIO_CHECK(!parsed.value().named_camera_transform);
    // No sequenceNumber in the sample, so the receiver's ordinal is used.
    OTIO_CHECK(parsed.value().pose.sequence == 5U);
}

void every_truncation_is_refused_rather_than_partially_applied() {
    // This is the case the parser exists to get right. The fields it wants are
    // near the end of the document, so a scanner that stopped once it had them
    // would accept nearly every prefix.
    for (std::size_t length = 0U; length < complete_sample.size(); ++length) {
        const auto parsed = track::parse_opentrackio(
            bytes(complete_sample.substr(0U, length)), 1U);
        OTIO_CHECK(!parsed);
    }
    // And the whole thing still parses, so the loop above proves something.
    OTIO_CHECK(track::parse_opentrackio(bytes(complete_sample), 1U));
}

void a_foreign_protocol_is_refused() {
    constexpr std::string_view text = R"({
      "protocol": {"name": "SomethingElse", "version": [1,0,0]},
      "transforms": [{"translation": {"x":0.0,"y":0.0,"z":0.0},
                      "rotation": {"pan":0.0,"tilt":0.0,"roll":0.0}}]})";
    const auto parsed = track::parse_opentrackio(bytes(text), 1U);
    OTIO_CHECK(!parsed);
    OTIO_CHECK(parsed.error()
               == track::TrackerParseError::UnrecognizedMessage);
}

void a_malformed_document_is_refused() {
    constexpr std::array<std::string_view, 8U> broken{{
        R"(not json at all)",
        R"({"protocol": {"name": "OpenTrackIO"},})",
        R"({"protocol": {"name": "OpenTrackIO"} "transforms": []})",
        R"({"protocol": {"name": "OpenTrackIO"}, "transforms": [{)",
        // A number the JSON grammar forbids.
        R"({"protocol":{"name":"OpenTrackIO"},"transforms":[{"translation":{"x":01.0,"y":0.0,"z":0.0},"rotation":{"pan":0.0,"tilt":0.0,"roll":0.0}}]})",
        R"({"protocol":{"name":"OpenTrackIO"},"transforms":[{"translation":{"x":.5,"y":0.0,"z":0.0},"rotation":{"pan":0.0,"tilt":0.0,"roll":0.0}}]})",
        // Two coalesced samples are not one sample.
        R"({"protocol":{"name":"OpenTrackIO"},"transforms":[{"translation":{"x":0.0,"y":0.0,"z":0.0},"rotation":{"pan":0.0,"tilt":0.0,"roll":0.0}}]}{})",
        // A transform missing half its pose.
        R"({"protocol":{"name":"OpenTrackIO"},"transforms":[{"translation":{"x":0.0,"y":0.0,"z":0.0}}]})",
    }};
    for (const std::string_view text : broken) {
        OTIO_CHECK(!track::parse_opentrackio(bytes(text), 1U));
    }
}

void an_oversized_or_absurd_sample_is_refused() {
    std::string padded{"{\"protocol\":{\"name\":\"OpenTrackIO\"},\"pad\":\""};
    padded.append(track::max_opentrackio_sample_bytes, 'x');
    padded.append("\"}");
    OTIO_CHECK(!track::parse_opentrackio(bytes(padded), 1U));

    // A position in the 1e30 metre range is a unit mistake or a corrupt
    // sample. Holding the previous pose beats swinging the wall.
    constexpr std::string_view absurd = R"({
      "protocol": {"name": "OpenTrackIO"},
      "transforms": [{"translation": {"x": 1e30, "y": 0.0, "z": 0.0},
                      "rotation": {"pan":0.0,"tilt":0.0,"roll":0.0}}]})";
    OTIO_CHECK(!track::parse_opentrackio(bytes(absurd), 1U));
}

void numbers_are_read_exactly() {
    constexpr std::string_view text = R"({
      "protocol": {"name": "OpenTrackIO"},
      "transforms": [{
        "translation": {"x": -0.00390625, "y": 1e2, "z": 12345.6789},
        "rotation": {"pan": 0.0, "tilt": -180.0, "roll": 359.9}}]})";
    const auto parsed = track::parse_opentrackio(bytes(text), 1U);
    OTIO_CHECK(parsed);
    const track::TrackedPose& pose = parsed.value().pose;
    // Exactly representable, so exactness is the assertion rather than
    // proximity.
    OTIO_CHECK(pose.x == -0.00390625F);
    OTIO_CHECK(pose.y == 100.0F);
    OTIO_CHECK(near(pose.z, 12'345.6789F, 1e-2F));
    OTIO_CHECK(pose.tilt == -0.5F);
    OTIO_CHECK(near(pose.roll, 359.9F / 360.0F, 1e-6F));
}

void a_transform_chain_past_the_bound_is_refused_not_truncated() {
    std::string text{R"({"protocol":{"name":"OpenTrackIO"},"transforms":[)"};
    for (std::size_t index = 0U;
         index <= track::max_opentrackio_transforms; ++index) {
        if (index != 0U) {
            text.push_back(',');
        }
        text.append(
            R"({"translation":{"x":0.0,"y":0.0,"z":0.0},)"
            R"("rotation":{"pan":0.0,"tilt":0.0,"roll":0.0}})");
    }
    text.append("]}");
    const auto parsed = track::parse_opentrackio(bytes(text), 1U);
    OTIO_CHECK(!parsed);
    OTIO_CHECK(parsed.error() == track::TrackerParseError::InvalidValue);
}

struct Case final {
    const char* name;
    void (*function)();
};

// Which camera a sample is for.
//
// FreeD puts a camera id in every datagram and the listener filters on it, so
// two cameras can share a port. OpenTrackIO carries the same thing as an
// optional `sourceNumber`, and this parser used to ignore it entirely - leaving
// `camera_id` at zero on every sample, so a feed authored for any camera but
// zero refused 100% of the datagrams it received while showing a bound link.
void a_source_number_names_the_camera_a_sample_is_for() {
    constexpr std::string_view numbered = R"({
      "protocol": {"name": "OpenTrackIO", "version": [1, 0, 0]},
      "sourceNumber": 3,
      "transforms": [{"id": "Camera",
                      "translation": {"x": 1.0, "y": 2.0, "z": 3.0},
                      "rotation": {"pan": 0.0, "tilt": 0.0, "roll": 0.0}}]
    })";
    const auto parsed = track::parse_opentrackio(bytes(numbered), 1U);
    OTIO_CHECK(parsed.has_value());
    OTIO_CHECK(parsed.value().pose.camera_id == 3U);
    OTIO_CHECK(parsed.value().identifies_camera);

    // Absent is not the same as zero, and the difference has to survive to the
    // listener: a sample that does not say which camera it is cannot be
    // matched against an authored id.
    constexpr std::string_view unnumbered = R"({
      "protocol": {"name": "OpenTrackIO", "version": [1, 0, 0]},
      "transforms": [{"id": "Camera",
                      "translation": {"x": 1.0, "y": 2.0, "z": 3.0},
                      "rotation": {"pan": 0.0, "tilt": 0.0, "roll": 0.0}}]
    })";
    const auto anonymous = track::parse_opentrackio(bytes(unnumbered), 1U);
    OTIO_CHECK(anonymous.has_value());
    OTIO_CHECK(anonymous.value().pose.camera_id == 0U);
    OTIO_CHECK(!anonymous.value().identifies_camera);

    // Out of range is refused whole rather than truncated into a valid id.
    constexpr std::string_view absurd = R"({
      "protocol": {"name": "OpenTrackIO", "version": [1, 0, 0]},
      "sourceNumber": 4096,
      "transforms": [{"id": "Camera",
                      "translation": {"x": 1.0, "y": 2.0, "z": 3.0},
                      "rotation": {"pan": 0.0, "tilt": 0.0, "roll": 0.0}}]
    })";
    OTIO_CHECK(!track::parse_opentrackio(bytes(absurd), 1U));
}

constexpr std::array<Case, 10U> cases{{
    {"a complete sample produces a known pose",
     &a_complete_sample_produces_a_known_pose},
    {"the named camera transform wins over the chain order",
     &the_named_camera_transform_wins_over_the_chain_order},
    {"an unnamed chain falls back to its last entry",
     &an_unnamed_chain_falls_back_to_its_last_entry},
    {"every truncation is refused rather than partially applied",
     &every_truncation_is_refused_rather_than_partially_applied},
    {"a foreign protocol is refused", &a_foreign_protocol_is_refused},
    {"a malformed document is refused", &a_malformed_document_is_refused},
    {"an oversized or absurd sample is refused",
     &an_oversized_or_absurd_sample_is_refused},
    {"numbers are read exactly", &numbers_are_read_exactly},
    {"a transform chain past the bound is refused, not truncated",
     &a_transform_chain_past_the_bound_is_refused_not_truncated},
    {"a source number names the camera a sample is for",
     &a_source_number_names_the_camera_a_sample_is_for},
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
