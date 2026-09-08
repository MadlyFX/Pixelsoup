#include "tracked_camera_bridge.hpp"

#include <mediaserver/track/freed.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace net = mediaserver::net;
namespace scene = mediaserver::scene;
namespace doc = mediaserver::doc;
namespace server = mediaserver::server;
namespace track = mediaserver::track;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define BRIDGE_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// The camera every sample in this file drives. A pose that names no camera is
// refused, because a node cannot guess which of several it belongs to.
const doc::NodeId camera_node{7U};

[[nodiscard]] track::TrackedPose tracker_pose(float x = 1.5F) noexcept {
    track::TrackedPose pose{};
    pose.protocol = track::TrackerProtocol::FreeD;
    pose.x = x;
    pose.y = 1.6F;
    pose.z = -2.25F;
    pose.pan = 0.125F;
    pose.tilt = -0.03125F;
    pose.roll = 0.0F;
    pose.focal_length_mm = 85.0F;
    pose.focus_distance_m = 4.75F;
    pose.aperture_f_number = 1.8F;
    pose.zoom_encoder = 1'024.0F;
    pose.focus_encoder = 2'048.0F;
    pose.iris_encoder = 4'096.0F;
    pose.zoom_encoder_valid = true;
    pose.focus_encoder_valid = true;
    pose.iris_encoder_valid = true;
    pose.sequence = 4'242U;
    pose.camera_id = 2U;
    return pose;
}

// The one piece of arithmetic in the channel, and the one that must never be
// machine-local: the Director shifts the pose forward by the authored number of
// frames so the wall shows it by the time it has crossed the wire, rendered,
// and presented.
void latency_compensation_is_applied_once_by_the_director() {
    const auto sample = server::stamped_tracker_sample(
        tracker_pose(), camera_node, 1'000U, 3U, 0.05F, 16.0F / 9.0F);
    BRIDGE_CHECK(sample.has_value());
    BRIDGE_CHECK(sample->control_frame == 1'003U);
    BRIDGE_CHECK(sample->sample.position_x == 1.5F);
    BRIDGE_CHECK(sample->sample.yaw_turns == 0.125F);
    BRIDGE_CHECK(sample->sample.pitch_turns == -0.03125F);
    BRIDGE_CHECK(sample->sample.field_of_view_turns == 0.05F);
    BRIDGE_CHECK(sample->sample.tracker_sequence == 4'242U);
    BRIDGE_CHECK(sample->sample.camera_id == 2U);
    BRIDGE_CHECK(sample->sample.focal_length_mm == 85.0F);
    BRIDGE_CHECK(sample->sample.focus_distance_m == 4.75F);
    BRIDGE_CHECK(sample->sample.aperture_f_number == 1.8F);
    BRIDGE_CHECK(sample->sample.zoom_encoder == 1'024.0F);
    BRIDGE_CHECK(sample->sample.focus_encoder == 2'048.0F);
    BRIDGE_CHECK(sample->sample.iris_encoder == 4'096.0F);
    BRIDGE_CHECK(sample->sample.zoom_encoder_valid && sample->sample.focus_encoder_valid
                 && sample->sample.iris_encoder_valid);
    const scene::LensEncoders encoders = server::tracked_camera_encoders(sample->sample);
    BRIDGE_CHECK(encoders.zoom == 1'024.0F && encoders.zoom_valid);
    BRIDGE_CHECK(encoders.focus == 2'048.0F && encoders.focus_valid);
    BRIDGE_CHECK(encoders.iris == 4'096.0F && encoders.iris_valid);
    BRIDGE_CHECK(net::valid(sample->sample));

    // Zero compensation is a legitimate authored choice, not a default to
    // guard against.
    const auto immediate = server::stamped_tracker_sample(
        tracker_pose(), camera_node, 1'000U, 0U, 0.05F, 16.0F / 9.0F);
    BRIDGE_CHECK(immediate.has_value() && immediate->control_frame == 1'000U);
}

void an_unusable_pose_or_lens_sends_nothing() {
    track::TrackedPose broken = tracker_pose();
    broken.y = std::numeric_limits<float>::quiet_NaN();
    BRIDGE_CHECK(!server::stamped_tracker_sample(
        broken, camera_node, 10U, 2U, 0.05F, 16.0F / 9.0F));

    track::TrackedPose unsourced = tracker_pose();
    unsourced.protocol = track::TrackerProtocol::None;
    BRIDGE_CHECK(!server::stamped_tracker_sample(
        unsourced, camera_node, 10U, 2U, 0.05F, 16.0F / 9.0F));

    // A lens angle the profile could not resolve, and one at half a turn.
    BRIDGE_CHECK(!server::stamped_tracker_sample(
        tracker_pose(), camera_node, 10U, 2U, 0.0F, 16.0F / 9.0F));
    BRIDGE_CHECK(!server::stamped_tracker_sample(
        tracker_pose(), camera_node, 10U, 2U, 0.5F, 16.0F / 9.0F));
    BRIDGE_CHECK(!server::stamped_tracker_sample(
        tracker_pose(), camera_node, 10U, 2U, 0.05F, 0.0F));

    // Compensation past the end of the frame counter sends nothing rather
    // than wrapping to frame zero.
    BRIDGE_CHECK(!server::stamped_tracker_sample(
        tracker_pose(), camera_node, std::numeric_limits<core::u64>::max(), 1U, 0.05F,
        16.0F / 9.0F));
}

// The whole path, from a FreeD datagram to the value the renderer evaluates.
void a_datagram_becomes_an_evaluated_tracked_camera() {
    // Build a D1 message the way the protocol defines it: 90 degrees of pan,
    // 1.5 m along X.
    std::vector<std::byte> datagram;
    const auto write24 = [&datagram](core::i32 value) {
        const auto raw = static_cast<core::u32>(value) & 0x00FF'FFFFU;
        datagram.push_back(static_cast<std::byte>((raw >> 16U) & 0xFFU));
        datagram.push_back(static_cast<std::byte>((raw >> 8U) & 0xFFU));
        datagram.push_back(static_cast<std::byte>(raw & 0xFFU));
    };
    datagram.push_back(track::freed_d1_identifier);
    datagram.push_back(std::byte{5});
    write24(90 * 32'768);
    write24(0);
    write24(0);
    write24(static_cast<core::i32>(1.5 * 64'000.0));
    write24(static_cast<core::i32>(1.6 * 64'000.0));
    write24(static_cast<core::i32>(-2.0 * 64'000.0));
    write24(0);
    write24(0);
    datagram.push_back(std::byte{0});
    datagram.push_back(std::byte{0});
    datagram.push_back(track::freed_d1_checksum(
        std::span<const std::byte>{datagram.data(), datagram.size()}));

    const auto parsed = track::parse_freed_d1(datagram, 11U);
    BRIDGE_CHECK(parsed);

    // Director: stamp it for the frame it applies to and put it on the wire.
    const auto sample = server::stamped_tracker_sample(
        parsed.value().pose, camera_node, 500U, 2U, 0.05F, 1.85F);
    BRIDGE_CHECK(sample.has_value());
    BRIDGE_CHECK(sample->control_frame == 502U);

    // Node: hold it in the channel and evaluate the scene value.
    track::PoseChannel channel;
    BRIDGE_CHECK(channel.accept(server::stamped_pose(*sample))
                 == track::PoseChannelOutcome::Accepted);
    const auto lens = server::tracked_camera_lens(sample->sample);
    BRIDGE_CHECK(lens.field_of_view_turns == 0.05F);
    BRIDGE_CHECK(lens.aspect_ratio == 1.85F);

    const auto exact = server::evaluated_tracked_camera(
        channel.resolve(502U), lens);
    const scene::LensEncoders encoders =
        server::evaluated_lens_encoders(channel.resolve(502U));
    BRIDGE_CHECK(exact.valid);
    BRIDGE_CHECK(!exact.held);
    BRIDGE_CHECK(exact.control_frame == 502U);
    BRIDGE_CHECK(exact.yaw == 0.25F);
    BRIDGE_CHECK(exact.position.x == 1.5F);
    BRIDGE_CHECK(exact.field_of_view == 0.05F);
    BRIDGE_CHECK(exact.aspect_ratio == 1.85F);
    // FreeD counts are valid even at zero. The channel carries that explicit
    // fact, so a later calibrated profile can distinguish a real zero from a
    // missing sample without any node guessing.
    BRIDGE_CHECK(encoders.zoom_valid && encoders.focus_valid);
    BRIDGE_CHECK(encoders.zoom == 0.0F && encoders.focus == 0.0F);

    // And it really is a camera the renderer can build a frustum from.
    scene::Camera authored{};
    authored.eye = scene::Vec3{0.0F, 0.0F, 5.0F};
    const auto effective = scene::tracked_view_camera(authored, exact);
    BRIDGE_CHECK(scene::valid(effective));
    BRIDGE_CHECK(effective.eye == exact.position);
    BRIDGE_CHECK(effective.field_of_view == 0.05F);
}

// The frames between poses. A held answer is still a valid tracked camera -
// every node holds the same pose and must draw the same picture - and it
// digests as the frame it was stamped for, which is the thing two nodes agree
// on rather than the frame each is drawing.
void a_held_answer_is_valid_and_names_the_frame_it_was_stamped_for() {
    const auto sample = server::stamped_tracker_sample(
        tracker_pose(), camera_node, 100U, 0U, 0.05F, 16.0F / 9.0F);
    BRIDGE_CHECK(sample.has_value());
    const auto lens = server::tracked_camera_lens(sample->sample);

    track::PoseChannel left;
    track::PoseChannel right;
    BRIDGE_CHECK(left.accept(server::stamped_pose(*sample))
                 == track::PoseChannelOutcome::Accepted);
    BRIDGE_CHECK(right.accept(server::stamped_pose(*sample))
                 == track::PoseChannelOutcome::Accepted);

    for (core::u64 frame = 100U; frame <= 104U; ++frame) {
        const auto a =
            server::evaluated_tracked_camera(left.resolve(frame), lens);
        const auto b =
            server::evaluated_tracked_camera(right.resolve(frame), lens);
        // Two nodes, same answer, byte for byte - which is what the scene
        // digest is going to compare.
        BRIDGE_CHECK(a == b);
        BRIDGE_CHECK(a.valid);
        BRIDGE_CHECK(a.focal_length_mm == 85.0F && a.focus_distance_m == 4.75F && a.aperture_f_number == 1.8F);
        BRIDGE_CHECK(a.control_frame == 100U);
        BRIDGE_CHECK(a.held == (frame != 100U));

        scene::TrackedCameraView first{};
        first.camera = camera_node;
        first.source = doc::NodeId{3U};
        first.pose = a;
        scene::TrackedCameraView second = first;
        second.pose = b;
        scene::Scene value{};
        value.tracked_cameras.push_back(first);
        scene::Scene other{};
        other.tracked_cameras.push_back(second);
        BRIDGE_CHECK(scene::digest(value) == scene::digest(other));
    }

    // Nothing ever received is an invalid value, and the wall shows the
    // authored view everywhere rather than a camera at the origin.
    track::PoseChannel empty;
    const auto nothing =
        server::evaluated_tracked_camera(empty.resolve(1U), lens);
    BRIDGE_CHECK(!nothing.valid);
    scene::Camera authored{};
    BRIDGE_CHECK(scene::tracked_view_camera(authored, nothing) == authored);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 4U> cases{{
    {"latency compensation is applied once by the Director",
     &latency_compensation_is_applied_once_by_the_director},
    {"an unusable pose or lens sends nothing",
     &an_unusable_pose_or_lens_sends_nothing},
    {"a datagram becomes an evaluated tracked camera",
     &a_datagram_becomes_an_evaluated_tracked_camera},
    {"a held answer is valid and names the frame it was stamped for",
     &a_held_answer_is_valid_and_names_the_frame_it_was_stamped_for},
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
