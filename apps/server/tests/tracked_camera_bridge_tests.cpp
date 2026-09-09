#include "tracked_camera_bridge.hpp"
#include "camera_prediction.hpp"
#include <mediaserver/net/director_frame_barrier.hpp>

#include <mediaserver/track/freed.hpp>

#include <array>
#include <algorithm>
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

struct PredictorFixture final {
    server::CameraPosePredictor predictor;
    scene::TrackingSourceSettings source{};
    scene::IcvfxSettings policy{};
    core::u64 publication{};
    PredictorFixture() {
        source.enabled = true;
        source.port = 40'001;
        policy.pose_prediction = true;
        configure();
    }
    void configure() { predictor.configure(doc::NodeId{3}, source, policy); }
    std::optional<server::StampedTrackerSample> send(core::u64 frame, track::TrackedPose pose) {
        pose.sequence = ++publication;
        return predictor.sample(track::PoseSample{pose, publication}, camera_node,
            frame, frame + policy.latency_frames, 0.05F, 1.85F);
    }
};

void constant_motion_predicts_exactly_at_large_irregular_control_stamps() {
    PredictorFixture fixture;
    fixture.policy.latency_frames = 3;
    fixture.policy.jitter_filter_percent = 73;
    fixture.configure();
    constexpr core::u64 base = (core::u64{1} << 60) + 7;
    for (const core::u64 offset : {0U, 1U, 3U, 6U, 7U, 9U, 12U}) {
        auto pose = tracker_pose();
        pose.x = 100.0F + static_cast<float>(offset) * 0.5F;
        pose.y = -3.0F + static_cast<float>(offset) * 0.25F;
        pose.pan = 0.125F + static_cast<float>(offset) / 256.0F;
        const auto stamped = fixture.send(base + offset, pose);
        BRIDGE_CHECK(stamped && stamped->control_frame == base + offset + 3);
        if (offset != 0) {
            BRIDGE_CHECK(stamped->sample.position_x == 100.0F + static_cast<float>(offset + 3) * 0.5F);
            BRIDGE_CHECK(stamped->sample.position_y == -3.0F + static_cast<float>(offset + 3) * 0.25F);
            BRIDGE_CHECK(stamped->sample.yaw_turns == 0.125F + static_cast<float>(offset + 3) / 256.0F);
        }
        BRIDGE_CHECK(stamped->sample.zoom_encoder == 1'024.0F);
        BRIDGE_CHECK(stamped->sample.focus_encoder == 2'048.0F);
        BRIDGE_CHECK(stamped->sample.iris_encoder == 4'096.0F);
        BRIDGE_CHECK(stamped->sample.zoom_encoder_valid && stamped->sample.focus_encoder_valid
                     && stamped->sample.iris_encoder_valid);
    }
    BRIDGE_CHECK(fixture.predictor.telemetry().fit_samples == 4);
    BRIDGE_CHECK(fixture.predictor.telemetry().next_error.samples > 0);
    BRIDGE_CHECK(fixture.predictor.telemetry().next_error.position_metres == 0.0);
    BRIDGE_CHECK(fixture.predictor.telemetry().latency_error.samples > 0);
    BRIDGE_CHECK(fixture.predictor.telemetry().latency_error.position_metres == 0.0);
}

void missing_and_repeated_samples_hold_the_last_published_forecast() {
    PredictorFixture fixture;
    BRIDGE_CHECK(fixture.send(100, tracker_pose(1.0F)));
    const auto predicted = fixture.send(101, tracker_pose(2.0F));
    BRIDGE_CHECK(predicted && predicted->sample.position_x == 4.0F);
    auto raw = tracker_pose(2.0F);
    raw.sequence = 2;
    const auto before = fixture.predictor.telemetry();
    for (core::u64 frame = 102; frame < 120; ++frame) {
        BRIDGE_CHECK(!fixture.predictor.sample(track::PoseSample{raw, 2}, camera_node,
                                               frame, frame + 2, 0.05F, 1.85F));
        BRIDGE_CHECK(!fixture.predictor.sample(track::PoseSample{tracker_pose(90.0F), 3},
                                               camera_node, frame, frame + 2, 0.05F, 1.85F));
    }
    BRIDGE_CHECK(fixture.predictor.telemetry().predicted == before.predicted);
    BRIDGE_CHECK(fixture.predictor.telemetry().target_frame == 103);
    BRIDGE_CHECK(fixture.predictor.telemetry().output_position.x == 4.0F);
    BRIDGE_CHECK(fixture.predictor.telemetry().next_error.samples == 0);
    BRIDGE_CHECK(fixture.predictor.telemetry().held == 18);
    track::PoseChannel node;
    BRIDGE_CHECK(node.accept(server::stamped_pose(*predicted)) == track::PoseChannelOutcome::Accepted);
    BRIDGE_CHECK(node.resolve(102).state == track::PoseHold::None);
    BRIDGE_CHECK(node.resolve(103).pose.x == 4.0F);
    BRIDGE_CHECK(node.resolve(999).pose.x == 4.0F);
    BRIDGE_CHECK(node.resolve(999).state == track::PoseHold::Held);
    // A gap past the fixed fit horizon starts from the next real observation.
    const auto resumed = fixture.send(120, tracker_pose(8.0F));
    BRIDGE_CHECK(resumed && resumed->sample.position_x == 8.0F);
    BRIDGE_CHECK(fixture.predictor.telemetry().resets >= 1);
    const auto last_state = fixture.predictor.telemetry();
    BRIDGE_CHECK(!fixture.predictor.sample(std::nullopt, camera_node, 120, 122, 0.05F, 1.85F));
    BRIDGE_CHECK(fixture.predictor.telemetry() == last_state);
}

void angle_wrap_uses_the_short_arc_on_all_axes() {
    PredictorFixture fixture;
    fixture.policy.latency_frames = 1;
    fixture.configure();
    auto first = tracker_pose();
    first.pan = first.tilt = first.roll = 0.484375F;
    BRIDGE_CHECK(fixture.send(10, first));
    auto second = first;
    second.pan = second.tilt = second.roll = -0.484375F;
    const auto predicted = fixture.send(11, second);
    BRIDGE_CHECK(predicted);
    BRIDGE_CHECK(predicted->sample.yaw_turns == -0.453125F);
    BRIDGE_CHECK(predicted->sample.pitch_turns == -0.453125F);
    BRIDGE_CHECK(predicted->sample.roll_turns == -0.453125F);
    second.pan = second.tilt = second.roll = -0.453125F;
    BRIDGE_CHECK(fixture.send(12, second));
    BRIDGE_CHECK(fixture.predictor.telemetry().next_error.angle_turns == 0.0);
    BRIDGE_CHECK(fixture.predictor.telemetry().latency_error.angle_turns == 0.0);
}

void jitter_filter_reduces_noise_and_errors_report_acceleration() {
    PredictorFixture fixture;
    fixture.policy.latency_frames = 0;
    fixture.policy.pose_prediction = false;
    fixture.policy.jitter_filter_percent = 100;
    fixture.policy.prediction_window = 8;
    fixture.configure();
    double raw_energy = 0.0;
    double filtered_energy = 0.0;
    for (core::u64 frame = 1; frame <= 64; ++frame) {
        const float x = 5.0F + (frame % 2 == 0 ? 0.125F : -0.125F);
        const auto output = fixture.send(frame, tracker_pose(x));
        BRIDGE_CHECK(output);
        if (frame > 8) {
            raw_energy += static_cast<double>(x - 5.0F) * (x - 5.0F);
            const double difference = output->sample.position_x - 5.0;
            filtered_energy += difference * difference;
        }
    }
    BRIDGE_CHECK(filtered_energy < raw_energy * 0.3);
    BRIDGE_CHECK(fixture.predictor.telemetry().fit_samples == 8);
    BRIDGE_CHECK(fixture.predictor.telemetry().predicted == 0);
    PredictorFixture accelerating;
    accelerating.policy.latency_frames = 1;
    accelerating.configure();
    BRIDGE_CHECK(accelerating.send(10, tracker_pose(0.0F)));
    BRIDGE_CHECK(accelerating.send(11, tracker_pose(1.0F)));
    BRIDGE_CHECK(accelerating.predictor.telemetry().next_error.samples == 0);
    BRIDGE_CHECK(accelerating.send(12, tracker_pose(3.0F)));
    const auto& error = accelerating.predictor.telemetry();
    BRIDGE_CHECK(error.next_error.samples == 1 && error.next_error.control_frame == 12);
    BRIDGE_CHECK(error.next_error.position_metres == 1.0);
    BRIDGE_CHECK(error.latency_error.samples == 1 && error.latency_error.position_metres == 1.0);
    BRIDGE_CHECK(accelerating.send(14, tracker_pose(6.0F)));
    BRIDGE_CHECK(accelerating.predictor.telemetry().next_error.samples == 1);
    BRIDGE_CHECK(accelerating.predictor.telemetry().next_error.missed == 1);
}

void policy_changes_restart_fits_and_unusable_forecasts_are_refused() {
    PredictorFixture fixture;
    BRIDGE_CHECK(fixture.send(10, tracker_pose(0.0F)));
    BRIDGE_CHECK(fixture.send(11, tracker_pose(1.0F)));
    fixture.policy.prediction_window = 9;
    fixture.configure();
    const auto after_edit = fixture.send(12, tracker_pose(20.0F));
    BRIDGE_CHECK(after_edit && after_edit->sample.position_x == 20.0F);
    BRIDGE_CHECK(fixture.predictor.telemetry().resets == 1);
    fixture.policy.pose_prediction = false;
    fixture.configure();
    auto raw = tracker_pose(3.0F);
    raw.pan = 0.75F;
    const auto off = fixture.send(13, raw);
    BRIDGE_CHECK(off && off->sample.position_x == 3.0F && off->sample.yaw_turns == 0.75F);
    fixture.policy.pose_prediction = true;
    fixture.configure();
    BRIDGE_CHECK(fixture.send(14, tracker_pose(99'990.0F)));
    BRIDGE_CHECK(!fixture.send(15, tracker_pose(100'000.0F)));
    BRIDGE_CHECK(fixture.predictor.telemetry().state == scene::PosePredictionState::Refused);
    BRIDGE_CHECK(fixture.predictor.telemetry().refused == 1);
    BRIDGE_CHECK(fixture.predictor.refusal_reason() == "Predicted pose exceeds the replicated pose bounds");
    raw.x = std::numeric_limits<float>::quiet_NaN();
    BRIDGE_CHECK(!fixture.send(16, raw));
    raw = tracker_pose();
    BRIDGE_CHECK(!fixture.predictor.sample(track::PoseSample{raw, 100}, camera_node,
        std::numeric_limits<core::u64>::max(), std::numeric_limits<core::u64>::max(), 0.05F, 1.85F));
    BRIDGE_CHECK(fixture.predictor.telemetry().refused == 3);
}

void predictor_bank_retires_cameras_and_preserves_stack_reordering() {
    server::DirectorPosePredictors bank;
    std::array<scene::TrackedCameraView, 4> views{};
    for (std::size_t i = 0; i < views.size(); ++i) views[i].camera = doc::NodeId{i + 1};
    bank.retain(views);
    auto* first = bank.find(doc::NodeId{1});
    BRIDGE_CHECK(first);
    PredictorFixture fixture;
    first->configure(doc::NodeId{3}, fixture.source, fixture.policy);
    BRIDGE_CHECK(first->sample(track::PoseSample{tracker_pose(), 1}, doc::NodeId{1}, 10, 12, 0.05F, 1.85F));
    std::reverse(views.begin(), views.end());
    bank.retain(views);
    BRIDGE_CHECK(bank.find(doc::NodeId{1}) == first);
    BRIDGE_CHECK(first->telemetry().sampled_frame == 10);
    bank.set_cadence(24, 1);
    BRIDGE_CHECK(first->telemetry().fit_samples == 0);
    BRIDGE_CHECK(!first->sample(track::PoseSample{tracker_pose(), 1}, doc::NodeId{1}, 11, 13, 0.05F, 1.85F));
    BRIDGE_CHECK(first->telemetry().sampled_frame == 10);
    for (core::u64 i = 5; i < 100; ++i) {
        views[0].camera = doc::NodeId{i};
        bank.retain(views);
        BRIDGE_CHECK(bank.find(doc::NodeId{i}));
    }
    bank.reset();
    BRIDGE_CHECK(!bank.find(doc::NodeId{1}));
}

void duplicate_datagrams_restarts_and_latency_edits_are_bounded() {
    PredictorFixture fixture;
    BRIDGE_CHECK(fixture.send(100, tracker_pose(1.0F)));
    BRIDGE_CHECK(fixture.send(101, tracker_pose(2.0F)));
    auto raw = tracker_pose(2.0F);
    raw.sequence = 2;
    // A repeated tracker ordinal in a new datagram is not a zero-velocity
    // measurement. Count the hold and retain the actual two-sample fit.
    BRIDGE_CHECK(!fixture.predictor.sample(track::PoseSample{raw, 3}, camera_node, 102, 104, 0.05F, 1.85F));
    BRIDGE_CHECK(fixture.predictor.telemetry().fit_samples == 2);
    BRIDGE_CHECK(fixture.predictor.telemetry().held == 1);
    raw.sequence = 1;
    raw.x = 9.0F;
    const auto restarted = fixture.predictor.sample(track::PoseSample{raw, 4}, camera_node, 103, 105, 0.05F, 1.85F);
    BRIDGE_CHECK(restarted && restarted->sample.position_x == 9.0F);
    BRIDGE_CHECK(fixture.predictor.telemetry().resets == 1);
    fixture.policy.latency_frames = 0;
    fixture.configure();
    // The wire's monotonic stamp waits for control time to catch up when the
    // operator reduces latency. This is a hold, never a second pose at 105.
    raw.sequence = 2;
    BRIDGE_CHECK(!fixture.predictor.sample(track::PoseSample{raw, 5}, camera_node, 104, 105, 0.05F, 1.85F));
    BRIDGE_CHECK(!fixture.predictor.sample(track::PoseSample{raw, 5}, camera_node, 105, 105, 0.05F, 1.85F));
    BRIDGE_CHECK(fixture.predictor.sample(track::PoseSample{raw, 5}, camera_node, 106, 106, 0.05F, 1.85F));
    fixture.source.port = 40'002;
    fixture.configure();
    BRIDGE_CHECK(fixture.predictor.telemetry().fit_samples == 0);
    BRIDGE_CHECK(!fixture.predictor.sample(track::PoseSample{raw, 5}, camera_node, 107, 107, 0.05F, 1.85F));
}

void two_sessions_receive_identical_predicted_streams_and_hold_them() {
    namespace command = mediaserver::cmd;
    const command::StreamCursor genesis{command::FrameNumber{0}, doc::Version{0}, command::CommandId{1}};
    net::DirectorFrameBarrier barrier{genesis};
    for (core::u32 i = 1; i <= 2; ++i) {
        BRIDGE_CHECK(barrier.register_live(net::DirectorLiveRegistration{
            net::Id128{10, i}, net::NodeSessionIdentity{{1, 1}, {2, i}, {3, i}}, i, genesis, 1}));
    }
    PredictorFixture fixture;
    std::array<track::PoseChannel, 2> channels{};
    for (core::u64 frame = 100; frame < 110; ++frame) {
        const auto predicted = fixture.send(frame, tracker_pose(static_cast<float>(frame - 100)));
        BRIDGE_CHECK(predicted);
        net::TrackedCameraFrame published{};
        published.control_frame = predicted->control_frame;
        published.samples.push_back(predicted->sample);
        BRIDGE_CHECK(barrier.publish_tracked_pose(published) == 2);
        std::array<net::TrackedCameraFrame, 2> received{};
        for (std::size_t index = 0; index < channels.size(); ++index) {
            const net::Id128 session{10, index + 1};
            BRIDGE_CHECK(barrier.try_take_tracked_pose(session, received[index])
                         == net::DirectorSessionObserver::TrackedPosePoll::Ready);
            net::TrackedCameraPose wire{session, {11, 12}, frame, received[index].pose_sequence,
                                       received[index].control_frame, received[index].samples};
            const auto bytes = net::encode_session(net::SessionMessage{wire});
            BRIDGE_CHECK(bytes);
            const auto decoded = net::decode_session(bytes.value());
            BRIDGE_CHECK(decoded);
            const auto& pose = std::get<net::TrackedCameraPose>(decoded.value());
            BRIDGE_CHECK(pose.samples.front() == predicted->sample);
            BRIDGE_CHECK(channels[index].accept(server::stamped_pose(pose.samples.front(), pose.control_frame))
                         == track::PoseChannelOutcome::Accepted);
            static_cast<void>(channels[index].resolve(frame));
        }
        BRIDGE_CHECK(received[0] == received[1]);
        BRIDGE_CHECK(channels[0].peek(frame).pose == channels[1].peek(frame).pose);
    }
    BRIDGE_CHECK(channels[0].resolve(500).pose == channels[1].resolve(500).pose);
    BRIDGE_CHECK(channels[0].peek(500).pose.x == 11.0F);
    BRIDGE_CHECK(barrier.tracked_poses_coalesced() == 0);
}

constexpr std::array<Case, 12U> cases{{
    {"duplicate datagrams restarts and latency edits are bounded", &duplicate_datagrams_restarts_and_latency_edits_are_bounded},
    {"constant motion predicts exactly at large irregular control stamps", &constant_motion_predicts_exactly_at_large_irregular_control_stamps},
    {"missing and repeated samples hold the last published forecast", &missing_and_repeated_samples_hold_the_last_published_forecast},
    {"angle wrap uses the short arc on all axes", &angle_wrap_uses_the_short_arc_on_all_axes},
    {"jitter filter reduces noise and errors report acceleration", &jitter_filter_reduces_noise_and_errors_report_acceleration},
    {"policy changes restart fits and unusable forecasts are refused", &policy_changes_restart_fits_and_unusable_forecasts_are_refused},
    {"predictor bank retires cameras and preserves stack reordering", &predictor_bank_retires_cameras_and_preserves_stack_reordering},
    {"two sessions receive identical predicted streams and hold them", &two_sessions_receive_identical_predicted_streams_and_hold_them},
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
