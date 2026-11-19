#include <mediaserver/net/session_protocol.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <flatbuffers/flatbuffers.h>

#include "session_protocol_generated.h"

namespace {
namespace core = mediaserver::core;

namespace command = mediaserver::cmd;
namespace document = mediaserver::doc;
namespace net = mediaserver::net;
namespace show = mediaserver::show;
namespace fb = mediaserver::net::protocol_fb::v1;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line) + ": check failed: " + expression};
    }
}

#define SESSION_CHECK(expression) \
    check(static_cast<bool>(expression), #expression, __LINE__)

[[nodiscard]] net::Id128 id(std::uint64_t low) {
    return net::Id128{0x1020304050607080ULL, low};
}

[[nodiscard]] command::StreamCursor cursor(
    std::uint64_t frame,
    std::uint64_t revision,
    std::uint64_t next_command) {
    return command::StreamCursor{
        command::FrameNumber{frame},
        document::Version{revision},
        command::CommandId{next_command},
    };
}

[[nodiscard]] net::SessionLimits session_limits() {
    return net::SessionLimits{
        16U * 1024U * 1024U,
        64U * 1024U * 1024U,
        8U,
        128U * 1024U * 1024U,
        64U * 1024U * 1024U,
    };
}

[[nodiscard]] std::vector<std::byte> bytes(
    std::initializer_list<std::uint8_t> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const auto value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

[[nodiscard]] net::ClientHello client_hello() {
    return net::ClientHello{
        net::current_session_format_version,
        net::current_session_format_version,
        id(1U),
        id(2U),
        id(3U),
        7U,
        {},
        cursor(10U, 20U, 30U),
        true,
        session_limits(),
    };
}

[[nodiscard]] net::ServerWelcome server_welcome() {
    return net::ServerWelcome{
        net::current_session_format_version,
        id(1U),
        id(4U),
        id(5U),
        net::ResumeMode::Replay,
        cursor(10U, 20U, 30U),
        cursor(12U, 23U, 33U),
        session_limits(),
    };
}

[[nodiscard]] net::FrameBatch frame_batch() {
    return net::FrameBatch{
        id(4U),
        7U,
        cursor(10U, 20U, 30U),
        cursor(11U, 22U, 32U),
        {
            net::Packet{bytes({0x01U, 0x02U, 0x03U})},
            net::Packet{bytes({0x04U})},
            net::Packet{bytes({0x05U, 0x06U})},
        },
    };
}

[[nodiscard]] net::CheckpointTransfer checkpoint_transfer() {
    return net::CheckpointTransfer{
        id(4U),
        8U,
        cursor(12U, 23U, 33U),
        bytes({0x10U, 0x20U, 0x30U, 0x40U}),
    };
}

[[nodiscard]] net::SyncComplete sync_complete() {
    return net::SyncComplete{id(4U), 9U, cursor(12U, 23U, 33U)};
}

[[nodiscard]] net::ResumeAck resume_ack() {
    return net::ResumeAck{id(4U), 10U, cursor(12U, 23U, 33U)};
}

[[nodiscard]] net::Reject reject_message() {
    return net::Reject{
        net::RejectCode::ResumeUnavailable,
        id(1U),
        id(5U),
        cursor(12U, 23U, 33U),
    };
}

[[nodiscard]] net::FramePrepared frame_prepared() {
    return net::FramePrepared{id(4U), 11U, 7U, cursor(11U, 22U, 32U)};
}

[[nodiscard]] net::FrameCommitted frame_committed() {
    // A node a dozen control frames behind what it holds, on show frame 1234.
    return net::FrameCommitted{id(4U), 12U, 7U, cursor(11U, 22U, 32U),
                               net::RuntimePlayhead{true, 1'234U, 5'678U, 13U}};
}

[[nodiscard]] net::FrameCommitPermit frame_commit_permit() {
    return net::FrameCommitPermit{
        id(4U),
        id(5U),
        13U,
        7U,
        41U,
        cursor(10U, 20U, 30U),
        cursor(11U, 22U, 32U),
    };
}

[[nodiscard]] net::TrackedCameraSample tracked_camera_sample() {
    return net::TrackedCameraSample{
        4'096U,
        1.5F,
        -0.75F,
        4.25F,
        0.25F,
        -0.125F,
        0.0625F,
        0.05F,
        16.0F / 9.0F,
        987U,
        3U,
        12'345.0F,
        54'321.0F,
        1'234.0F,
        true,
        true,
        true,
        85.0F, 4.75F, 1.8F,
    };
}

[[nodiscard]] net::TrackedCameraPose tracked_camera_pose() {
    net::TrackedCameraPose pose{};
    pose.session_id = id(4U);
    pose.director_epoch = id(5U);
    pose.sequence = 16U;
    // Deliberately unequal to `sequence`: they number different streams, and a
    // golden that used the same value for both would not catch them being
    // crossed in the codec.
    pose.pose_sequence = 9U;
    pose.control_frame = 4'096U;
    pose.samples.push_back(tracked_camera_sample());
    return pose;
}

[[nodiscard]] net::Heartbeat heartbeat() {
    return net::Heartbeat{
        id(4U),
        id(5U),
        14U,
        5'000U,
        cursor(11U, 22U, 32U),
    };
}

[[nodiscard]] net::HeartbeatAck heartbeat_ack() {
    net::HeartbeatAck value{
        id(4U),
        id(5U),
        13U,
        14U,
        cursor(11U, 22U, 32U),
        0x1122334455667788ULL,
        true,
    };
    // A specific non-default reason, not Ready: the round-trip has to carry
    // which physical fault stopped the outputs, because a value that decoded
    // as Unknown would read as "this node said nothing" and the Director
    // would go back to blaming the assets.
    value.output_deployment =
        net::RuntimeOutputDeploymentState::MirroredDisplayTarget;
    return value;
}

[[nodiscard]] net::Goodbye goodbye() {
    return net::Goodbye{
        id(4U),
        id(5U),
        15U,
        net::GoodbyeReason::Restart,
        cursor(11U, 22U, 32U),
    };
}

// Every field distinct and away from its default, so a swapped or dropped
// field cannot round-trip by coincidence.
[[nodiscard]] net::PriorityTransport priority_transport() {
    net::PriorityTransport value{};
    value.session_id = id(4U);
    value.director_epoch = id(5U);
    value.sequence = 17U;
    value.anchor.anchor_control_frame = command::FrameNumber{4'099U};
    value.anchor.anchor_show_frame = show::ShowFrame{213U};
    value.anchor.state = show::TransportState::Paused;
    value.anchor.direction = show::TransportDirection::Reverse;
    value.anchor.rate = show::FrameRate{60'000U, 1'001U};
    value.anchor.first_frame = show::ShowFrame{7U};
    value.anchor.last_frame = show::ShowFrame{9'000U};
    value.anchor.loop = true;
    value.anchor.cues_armed = false;
    return value;
}

// Every field distinct and away from its default, two strokes so a dropped
// stroke end cannot round-trip by coincidence.
[[nodiscard]] net::PointerOverlayMessage pointer_overlay() {
    net::PointerOverlayMessage value{};
    value.session_id = id(4U);
    value.director_epoch = id(5U);
    value.sequence = 19U;
    value.overlay.control_frame = 4'321U;
    value.overlay.dot = show::PointerPoint{960.5F, -12.25F};
    value.overlay.points = {
        {10.0F, 20.0F}, {11.5F, 21.5F}, {13.0F, 24.0F}, {700.0F, 300.0F}};
    value.overlay.stroke_ends = {3U, 4U};
    return value;
}

[[nodiscard]] std::vector<net::SessionMessage> legacy_messages() {
    return {
        net::SessionMessage{client_hello()},
        net::SessionMessage{server_welcome()},
        net::SessionMessage{frame_batch()},
        net::SessionMessage{checkpoint_transfer()},
        net::SessionMessage{sync_complete()},
        net::SessionMessage{resume_ack()},
        net::SessionMessage{reject_message()},
    };
}

[[nodiscard]] std::vector<net::SessionMessage> every_message() {
    auto messages = legacy_messages();
    messages.push_back(net::SessionMessage{frame_prepared()});
    messages.push_back(net::SessionMessage{frame_committed()});
    messages.push_back(net::SessionMessage{frame_commit_permit()});
    messages.push_back(net::SessionMessage{heartbeat()});
    messages.push_back(net::SessionMessage{heartbeat_ack()});
    messages.push_back(net::SessionMessage{goodbye()});
    messages.push_back(net::SessionMessage{tracked_camera_pose()});
    messages.push_back(net::SessionMessage{priority_transport()});
    messages.push_back(net::SessionMessage{pointer_overlay()});
    return messages;
}

[[nodiscard]] std::vector<std::byte> encode(const net::SessionMessage& message) {
    auto result = net::encode_session(message);
    SESSION_CHECK(result.has_value());
    return std::move(result).value();
}

[[nodiscard]] std::uint32_t read_u32_le(std::span<const std::byte> value) {
    SESSION_CHECK(value.size() >= 4U);
    return static_cast<std::uint32_t>(std::to_integer<unsigned int>(value[0]))
        | (static_cast<std::uint32_t>(std::to_integer<unsigned int>(value[1])) << 8U)
        | (static_cast<std::uint32_t>(std::to_integer<unsigned int>(value[2])) << 16U)
        | (static_cast<std::uint32_t>(std::to_integer<unsigned int>(value[3])) << 24U);
}

void write_u32_le(std::span<std::byte> value, std::uint32_t number) {
    SESSION_CHECK(value.size() >= 4U);
    for (unsigned int index = 0U; index < 4U; ++index) {
        value[index] = static_cast<std::byte>((number >> (index * 8U)) & 0xffU);
    }
}

[[nodiscard]] std::string to_hex(std::span<const std::byte> value) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2U);
    for (const auto byte : value) {
        const auto number = std::to_integer<unsigned int>(byte);
        result.push_back(digits[(number >> 4U) & 0x0fU]);
        result.push_back(digits[number & 0x0fU]);
    }
    return result;
}

[[nodiscard]] std::vector<std::byte> finish_raw(
    flatbuffers::FlatBufferBuilder& builder,
    flatbuffers::Offset<fb::SessionEnvelope> envelope) {
    fb::FinishSizePrefixedSessionEnvelopeBuffer(builder, envelope);
    std::vector<std::byte> result(builder.GetSize());
    std::memcpy(result.data(), builder.GetBufferPointer(), builder.GetSize());
    return result;
}

[[nodiscard]] std::vector<std::byte> raw_sync_complete(std::uint16_t version) {
    flatbuffers::FlatBufferBuilder builder;
    const fb::Id128 session{0x1020304050607080ULL, 4U};
    const fb::StreamCursor target{12U, 23U, 33U};
    const auto body = fb::CreateSyncComplete(builder, &session, 9U, &target);
    return finish_raw(
        builder,
        fb::CreateSessionEnvelope(
            builder, version, fb::SessionBody::SyncComplete, body.Union()));
}

[[nodiscard]] std::vector<std::byte> raw_frame_with_zero_sequence() {
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<fb::Packet>> packets;
    for (const auto value : {0x01U, 0x02U, 0x03U}) {
        const std::array packet{static_cast<std::uint8_t>(value)};
        packets.push_back(
            fb::CreatePacket(builder, builder.CreateVector(packet.data(), packet.size())));
    }
    const fb::Id128 session{0x1020304050607080ULL, 4U};
    const fb::StreamCursor before{10U, 20U, 30U};
    const fb::StreamCursor after{11U, 22U, 32U};
    const auto body = fb::CreateFrameBatch(
        builder, &session, 0U, &before, &after, builder.CreateVector(packets));
    return finish_raw(
        builder,
        fb::CreateSessionEnvelope(
            builder,
            net::current_session_format_version,
            fb::SessionBody::FrameBatch,
            body.Union()));
}

[[nodiscard]] std::vector<std::byte> raw_prepared_with_zero_acknowledged_sequence() {
    flatbuffers::FlatBufferBuilder builder;
    const fb::Id128 session{0x1020304050607080ULL, 4U};
    const fb::StreamCursor after{11U, 22U, 32U};
    const auto body = fb::CreateFramePrepared(builder, &session, 11U, 0U, &after);
    return finish_raw(
        builder,
        fb::CreateSessionEnvelope(
            builder,
            net::current_session_format_version,
            fb::SessionBody::FramePrepared,
            body.Union()));
}

[[nodiscard]] std::vector<std::byte> raw_permit_with_zero_barrier_ordinal() {
    flatbuffers::FlatBufferBuilder builder;
    const fb::Id128 session{0x1020304050607080ULL, 4U};
    const fb::Id128 epoch{0x1020304050607080ULL, 5U};
    const fb::StreamCursor before{10U, 20U, 30U};
    const fb::StreamCursor after{11U, 22U, 32U};
    const auto body = fb::CreateFrameCommitPermit(
        builder, &session, &epoch, 13U, 7U, 0U, &before, &after);
    return finish_raw(
        builder,
        fb::CreateSessionEnvelope(
            builder,
            net::current_session_format_version,
            fb::SessionBody::FrameCommitPermit,
            body.Union()));
}

[[nodiscard]] std::vector<std::byte> raw_heartbeat_with_short_timeout() {
    flatbuffers::FlatBufferBuilder builder;
    const fb::Id128 session{0x1020304050607080ULL, 4U};
    const fb::Id128 epoch{0x1020304050607080ULL, 5U};
    const fb::StreamCursor released{11U, 22U, 32U};
    const auto body = fb::CreateHeartbeat(
        builder,
        &session,
        &epoch,
        14U,
        net::min_heartbeat_response_timeout_ms - 1U,
        &released);
    return finish_raw(
        builder,
        fb::CreateSessionEnvelope(
            builder,
            net::current_session_format_version,
            fb::SessionBody::Heartbeat,
            body.Union()));
}

[[nodiscard]] std::vector<std::byte> raw_heartbeat_ack_with_zero_acknowledgement() {
    flatbuffers::FlatBufferBuilder builder;
    const fb::Id128 session{0x1020304050607080ULL, 4U};
    const fb::Id128 epoch{0x1020304050607080ULL, 5U};
    const fb::StreamCursor visible{11U, 22U, 32U};
    const auto body =
        fb::CreateHeartbeatAck(builder, &session, &epoch, 13U, 0U, &visible);
    return finish_raw(
        builder,
        fb::CreateSessionEnvelope(
            builder,
            net::current_session_format_version,
            fb::SessionBody::HeartbeatAck,
            body.Union()));
}

[[nodiscard]] std::vector<std::byte> raw_goodbye_with_no_reason() {
    flatbuffers::FlatBufferBuilder builder;
    const fb::Id128 session{0x1020304050607080ULL, 4U};
    const fb::Id128 epoch{0x1020304050607080ULL, 5U};
    const fb::StreamCursor final_cursor{11U, 22U, 32U};
    const auto body = fb::CreateGoodbye(
        builder, &session, &epoch, 15U, fb::GoodbyeReason::NONE, &final_cursor);
    return finish_raw(
        builder,
        fb::CreateSessionEnvelope(
            builder,
            net::current_session_format_version,
            fb::SessionBody::Goodbye,
            body.Union()));
}

[[nodiscard]] std::vector<std::byte> raw_priority_transport_with_state(
    std::uint8_t state) {
    flatbuffers::FlatBufferBuilder builder;
    const fb::Id128 session{0x1020304050607080ULL, 4U};
    const fb::Id128 epoch{0x1020304050607080ULL, 5U};
    const auto body = fb::CreatePriorityTransport(
        builder, &session, &epoch, 17U, 4'099U, 213U, state, 0U, 60'000U,
        1'001U, 7U, 9'000U, true, false);
    return finish_raw(
        builder,
        fb::CreateSessionEnvelope(
            builder,
            net::current_session_format_version,
            fb::SessionBody::PriorityTransport,
            body.Union()));
}

// Only an operator's own anchor may jump the frame stream. A scheduled anchor
// is the show re-anchoring itself and must play through in order, so it is
// refused on both sides of the wire, as is anything a node could not present.
void priority_transport_admits_only_operator_anchors() {
    auto scheduled = priority_transport();
    scheduled.anchor.scheduled = true;
    auto refused = net::encode_session(net::SessionMessage{scheduled});
    SESSION_CHECK(!refused.has_value());
    SESSION_CHECK(
        refused.error() == net::SessionEncodeError::InvalidPriorityTransport);

    auto blended = priority_transport();
    blended.anchor.blend_frames = 12U;
    blended.anchor.blend_from = show::ShowFrame{8U};
    refused = net::encode_session(net::SessionMessage{blended});
    SESSION_CHECK(!refused.has_value());
    SESSION_CHECK(
        refused.error() == net::SessionEncodeError::InvalidPriorityTransport);

    auto outside = priority_transport();
    // Only a playing show must sit inside its range.
    outside.anchor.state = show::TransportState::Playing;
    outside.anchor.anchor_show_frame = show::ShowFrame{9'001U};
    refused = net::encode_session(net::SessionMessage{outside});
    SESSION_CHECK(!refused.has_value());

    auto unsequenced = priority_transport();
    unsequenced.sequence = 0U;
    refused = net::encode_session(net::SessionMessage{unsequenced});
    SESSION_CHECK(!refused.has_value());

    // A well-formed message decodes to exactly the anchor that was sent.
    auto decoded = net::decode_session(raw_priority_transport_with_state(
        static_cast<std::uint8_t>(show::TransportState::Stopped)));
    SESSION_CHECK(decoded.has_value());
    const auto* priority =
        std::get_if<net::PriorityTransport>(&decoded.value());
    SESSION_CHECK(priority != nullptr);
    SESSION_CHECK(priority->anchor.state == show::TransportState::Stopped);
    SESSION_CHECK(
        priority->anchor.direction == show::TransportDirection::Forward);
    SESSION_CHECK(priority->anchor.anchor_control_frame.value() == 4'099U);

    // An enumerator past the last state is a corrupt message, not a state.
    decoded = net::decode_session(raw_priority_transport_with_state(3U));
    SESSION_CHECK(!decoded.has_value());
    SESSION_CHECK(
        decoded.error() == net::SessionDecodeError::InvalidPriorityTransport);
}

[[nodiscard]] std::vector<std::byte> raw_pointer_overlay(
    std::span<const fb::PointerPoint> points,
    std::span<const std::uint32_t> ends, bool has_dot) {
    flatbuffers::FlatBufferBuilder builder;
    const fb::Id128 session{0x1020304050607080ULL, 4U};
    const fb::Id128 epoch{0x1020304050607080ULL, 5U};
    const auto point_vector = builder.CreateVectorOfStructs(
        points.data(), points.size());
    const auto end_vector = builder.CreateVector(ends.data(), ends.size());
    const fb::PointerPoint dot{3.0F, 4.0F};
    const auto body = fb::CreatePointerOverlay(
        builder, &session, &epoch, 21U, 77U, has_dot,
        has_dot ? &dot : nullptr, point_vector, end_vector);
    return finish_raw(
        builder,
        fb::CreateSessionEnvelope(
            builder,
            net::current_session_format_version,
            fb::SessionBody::PointerOverlay,
            body.Union()));
}

// A pointer picture a node could not draw - strokes that do not cover their
// points, a number that is not a coordinate, a count over budget - is refused
// on both sides of the wire.
void pointer_overlay_refuses_what_a_node_could_not_draw() {
    auto uncovered = pointer_overlay();
    uncovered.overlay.stroke_ends = {3U};
    auto refused = net::encode_session(net::SessionMessage{uncovered});
    SESSION_CHECK(!refused.has_value());
    SESSION_CHECK(
        refused.error() == net::SessionEncodeError::InvalidPointerOverlay);

    auto unsequenced = pointer_overlay();
    unsequenced.sequence = 0U;
    refused = net::encode_session(net::SessionMessage{unsequenced});
    SESSION_CHECK(!refused.has_value());

    auto cleared = pointer_overlay();
    cleared.overlay.dot.reset();
    cleared.overlay.points.clear();
    cleared.overlay.stroke_ends.clear();
    auto decoded = net::decode_session(encode(net::SessionMessage{cleared}));
    SESSION_CHECK(decoded.has_value());
    const auto* pointer =
        std::get_if<net::PointerOverlayMessage>(&decoded.value());
    SESSION_CHECK(pointer != nullptr);
    SESSION_CHECK(pointer->overlay.empty());
    SESSION_CHECK(pointer->overlay.control_frame == 4'321U);

    const std::array<fb::PointerPoint, 2U> two{
        fb::PointerPoint{1.0F, 2.0F}, fb::PointerPoint{3.0F, 4.0F}};
    const std::array<std::uint32_t, 1U> both{2U};
    decoded = net::decode_session(raw_pointer_overlay(two, both, true));
    SESSION_CHECK(decoded.has_value());
    pointer = std::get_if<net::PointerOverlayMessage>(&decoded.value());
    SESSION_CHECK(pointer != nullptr && pointer->overlay.dot.has_value());
    SESSION_CHECK(pointer->overlay.dot->y == 4.0F);

    const std::array<std::uint32_t, 1U> short_end{1U};
    decoded = net::decode_session(raw_pointer_overlay(two, short_end, false));
    SESSION_CHECK(!decoded.has_value());
    SESSION_CHECK(
        decoded.error() == net::SessionDecodeError::InvalidPointerOverlay);

    const std::array<fb::PointerPoint, 1U> not_a_number{fb::PointerPoint{
        std::numeric_limits<float>::quiet_NaN(), 0.0F}};
    const std::array<std::uint32_t, 1U> one{1U};
    decoded = net::decode_session(raw_pointer_overlay(not_a_number, one, false));
    SESSION_CHECK(!decoded.has_value());

    std::vector<fb::PointerPoint> many(show::max_pointer_points + 1U,
                                       fb::PointerPoint{1.0F, 1.0F});
    const std::array<std::uint32_t, 1U> all{
        static_cast<std::uint32_t>(many.size())};
    decoded = net::decode_session(raw_pointer_overlay(many, all, false));
    SESSION_CHECK(!decoded.has_value());
    SESSION_CHECK(
        decoded.error() == net::SessionDecodeError::InvalidPointerOverlay);
}

void every_message_round_trips() {
    for (const auto& original : every_message()) {
        const auto encoded = encode(original);
        auto decoded = net::decode_session(encoded);
        SESSION_CHECK(decoded.has_value());
        SESSION_CHECK(decoded.value() == original);
    }
}

void exact_prefix_and_all_truncations_are_rejected() {
    for (const auto& message : every_message()) {
        const auto original = encode(message);
        SESSION_CHECK(read_u32_le(original) == original.size() - 4U);

        auto trailing = original;
        trailing.push_back(std::byte{0U});
        auto decoded = net::decode_session(trailing);
        SESSION_CHECK(!decoded.has_value());
        SESSION_CHECK(decoded.error() == net::SessionDecodeError::InvalidBuffer);

        auto short_prefix = original;
        write_u32_le(short_prefix, read_u32_le(short_prefix) - 1U);
        decoded = net::decode_session(short_prefix);
        SESSION_CHECK(!decoded.has_value());
        SESSION_CHECK(decoded.error() == net::SessionDecodeError::InvalidBuffer);

        auto long_prefix = original;
        write_u32_le(long_prefix, read_u32_le(long_prefix) + 1U);
        decoded = net::decode_session(long_prefix);
        SESSION_CHECK(!decoded.has_value());
        SESSION_CHECK(decoded.error() == net::SessionDecodeError::InvalidBuffer);

        auto bad_identifier = original;
        SESSION_CHECK(bad_identifier.size() > 12U);
        bad_identifier[8] ^= std::byte{0xffU};
        decoded = net::decode_session(bad_identifier);
        SESSION_CHECK(!decoded.has_value());
        SESSION_CHECK(decoded.error() == net::SessionDecodeError::InvalidBuffer);

        for (std::size_t size = 0U; size < original.size(); ++size) {
            const auto truncated = net::decode_session(
                std::span{original.data(), size});
            SESSION_CHECK(!truncated.has_value());
        }
    }

    const auto legacy = raw_sync_complete(
        static_cast<std::uint16_t>(net::current_session_format_version - 1U));
    auto decoded = net::decode_session(legacy);
    SESSION_CHECK(!decoded.has_value());
    SESSION_CHECK(decoded.error() == net::SessionDecodeError::UnsupportedVersion);

    const auto future = raw_sync_complete(
        static_cast<std::uint16_t>(net::current_session_format_version + 1U));
    decoded = net::decode_session(future);
    SESSION_CHECK(!decoded.has_value());
    SESSION_CHECK(decoded.error() == net::SessionDecodeError::UnsupportedVersion);
}

void packet_count_aggregate_and_message_caps_are_enforced() {
    const auto frame = frame_batch();
    const auto frame_bytes = encode(net::SessionMessage{frame});

    auto limits = net::SessionDecodeLimits{};
    limits.max_packet_bytes = 2U;
    auto encoded = net::encode_session(net::SessionMessage{frame}, limits);
    SESSION_CHECK(!encoded.has_value());
    SESSION_CHECK(encoded.error() == net::SessionEncodeError::SizeLimitExceeded);
    auto decoded = net::decode_session(frame_bytes, limits);
    SESSION_CHECK(!decoded.has_value());
    SESSION_CHECK(decoded.error() == net::SessionDecodeError::SizeLimitExceeded);

    limits = net::SessionDecodeLimits{};
    limits.max_packets_per_batch = 2U;
    encoded = net::encode_session(net::SessionMessage{frame}, limits);
    SESSION_CHECK(!encoded.has_value());
    SESSION_CHECK(encoded.error() == net::SessionEncodeError::SizeLimitExceeded);
    decoded = net::decode_session(frame_bytes, limits);
    SESSION_CHECK(!decoded.has_value());
    SESSION_CHECK(decoded.error() == net::SessionDecodeError::SizeLimitExceeded);

    limits = net::SessionDecodeLimits{};
    limits.max_aggregate_packet_bytes = 5U;
    encoded = net::encode_session(net::SessionMessage{frame}, limits);
    SESSION_CHECK(!encoded.has_value());
    SESSION_CHECK(encoded.error() == net::SessionEncodeError::SizeLimitExceeded);
    decoded = net::decode_session(frame_bytes, limits);
    SESSION_CHECK(!decoded.has_value());
    SESSION_CHECK(decoded.error() == net::SessionDecodeError::SizeLimitExceeded);

    limits = net::SessionDecodeLimits{};
    limits.max_message_bytes = frame_bytes.size() - 1U;
    encoded = net::encode_session(net::SessionMessage{frame}, limits);
    SESSION_CHECK(!encoded.has_value());
    SESSION_CHECK(encoded.error() == net::SessionEncodeError::SizeLimitExceeded);
    decoded = net::decode_session(frame_bytes, limits);
    SESSION_CHECK(!decoded.has_value());
    SESSION_CHECK(decoded.error() == net::SessionDecodeError::SizeLimitExceeded);

    const auto checkpoint = checkpoint_transfer();
    const auto checkpoint_bytes = encode(net::SessionMessage{checkpoint});
    limits = net::SessionDecodeLimits{};
    limits.max_packet_bytes = checkpoint.wire_packet.size() - 1U;
    encoded = net::encode_session(net::SessionMessage{checkpoint}, limits);
    SESSION_CHECK(encoded.has_value());
    decoded = net::decode_session(checkpoint_bytes, limits);
    SESSION_CHECK(decoded.has_value());

    limits = net::SessionDecodeLimits{};
    limits.max_checkpoint_bytes = checkpoint.wire_packet.size() - 1U;
    encoded = net::encode_session(net::SessionMessage{checkpoint}, limits);
    SESSION_CHECK(!encoded.has_value());
    SESSION_CHECK(encoded.error() == net::SessionEncodeError::SizeLimitExceeded);
    decoded = net::decode_session(checkpoint_bytes, limits);
    SESSION_CHECK(!decoded.has_value());
    SESSION_CHECK(decoded.error() == net::SessionDecodeError::SizeLimitExceeded);
}

void invalid_public_semantics_are_rejected() {
    auto hello = client_hello();
    hello.min_version = 0U;
    auto result = net::encode_session(net::SessionMessage{hello});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidClientHello);

    hello = client_hello();
    hello.show_id = {};
    result = net::encode_session(net::SessionMessage{hello});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidClientHello);

    hello = client_hello();
    hello.node_index = net::max_session_node_index + 1U;
    result = net::encode_session(net::SessionMessage{hello});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidClientHello);

    hello = client_hello();
    hello.cursor.next_command = command::CommandId{0U};
    result = net::encode_session(net::SessionMessage{hello});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidClientHello);

    hello = client_hello();
    hello.limits.max_frame_bytes = hello.limits.max_packet_bytes - 1U;
    result = net::encode_session(net::SessionMessage{hello});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidClientHello);

    hello = client_hello();
    hello.limits.max_checkpoint_bytes = hello.limits.max_inflight_bytes + 1U;
    result = net::encode_session(net::SessionMessage{hello});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidClientHello);

    auto welcome = server_welcome();
    welcome.selected_version = static_cast<std::uint16_t>(
        net::current_session_format_version + 1U);
    result = net::encode_session(net::SessionMessage{welcome});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidServerWelcome);

    welcome = server_welcome();
    welcome.mode = net::ResumeMode::UpToDate;
    result = net::encode_session(net::SessionMessage{welcome});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidServerWelcome);

    auto frame = frame_batch();
    frame.sequence = 0U;
    result = net::encode_session(net::SessionMessage{frame});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidFrameBatch);

    frame = frame_batch();
    frame.after.through_frame = frame.before.through_frame;
    result = net::encode_session(net::SessionMessage{frame});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidFrameBatch);

    frame = frame_batch();
    frame.after.revision = document::Version{21U};
    result = net::encode_session(net::SessionMessage{frame});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidFrameBatch);

    frame = frame_batch();
    frame.packets.pop_back();
    result = net::encode_session(net::SessionMessage{frame});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidFrameBatch);

    auto checkpoint = checkpoint_transfer();
    checkpoint.wire_packet.clear();
    result = net::encode_session(net::SessionMessage{checkpoint});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidCheckpointTransfer);

    auto complete = sync_complete();
    complete.sequence = 0U;
    result = net::encode_session(net::SessionMessage{complete});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidSyncComplete);

    auto ack = resume_ack();
    ack.session_id = {};
    result = net::encode_session(net::SessionMessage{ack});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidResumeAck);

    auto rejected = reject_message();
    rejected.code = net::RejectCode::None;
    result = net::encode_session(net::SessionMessage{rejected});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidReject);

    auto decoded = net::decode_session(raw_frame_with_zero_sequence());
    SESSION_CHECK(!decoded.has_value());
    SESSION_CHECK(decoded.error() == net::SessionDecodeError::InvalidFrameBatch);

    auto prepared = frame_prepared();
    prepared.acknowledged_sequence = 0U;
    result = net::encode_session(net::SessionMessage{prepared});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidFramePrepared);

    auto committed = frame_committed();
    committed.after.through_frame = command::FrameNumber{0U};
    result = net::encode_session(net::SessionMessage{committed});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidFrameCommitted);

    // A position beside "presented nothing" names a frame no output showed.
    committed = frame_committed();
    committed.playhead.presented = false;
    result = net::encode_session(net::SessionMessage{committed});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidFrameCommitted);
    committed.playhead = net::RuntimePlayhead{};
    SESSION_CHECK(net::encode_session(net::SessionMessage{committed}).has_value());

    decoded = net::decode_session(raw_prepared_with_zero_acknowledged_sequence());
    SESSION_CHECK(!decoded.has_value());
    SESSION_CHECK(decoded.error() == net::SessionDecodeError::InvalidFramePrepared);

    auto permit = frame_commit_permit();
    permit.session_id = {};
    result = net::encode_session(net::SessionMessage{permit});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidFrameCommitPermit);

    permit = frame_commit_permit();
    permit.director_epoch = {};
    result = net::encode_session(net::SessionMessage{permit});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidFrameCommitPermit);

    permit = frame_commit_permit();
    permit.sequence = 0U;
    result = net::encode_session(net::SessionMessage{permit});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidFrameCommitPermit);

    permit = frame_commit_permit();
    permit.acknowledged_sequence = 0U;
    result = net::encode_session(net::SessionMessage{permit});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidFrameCommitPermit);

    permit = frame_commit_permit();
    permit.sequence = permit.acknowledged_sequence;
    result = net::encode_session(net::SessionMessage{permit});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidFrameCommitPermit);

    permit = frame_commit_permit();
    permit.barrier_ordinal = 0U;
    result = net::encode_session(net::SessionMessage{permit});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidFrameCommitPermit);

    permit = frame_commit_permit();
    permit.after.through_frame = permit.before.through_frame;
    result = net::encode_session(net::SessionMessage{permit});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidFrameCommitPermit);

    permit = frame_commit_permit();
    permit.after.revision = document::Version{21U};
    result = net::encode_session(net::SessionMessage{permit});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidFrameCommitPermit);

    decoded = net::decode_session(raw_permit_with_zero_barrier_ordinal());
    SESSION_CHECK(!decoded.has_value());
    SESSION_CHECK(
        decoded.error() == net::SessionDecodeError::InvalidFrameCommitPermit);

    // A tracked pose decides where the plate is on every node in the cohort,
    // so an unusable one is refused at the codec rather than reaching a
    // renderer. Every node then holds its previous pose, which is always a
    // better outcome than every node swinging the wall together.
    auto pose = tracked_camera_pose();
    pose.session_id = {};
    result = net::encode_session(net::SessionMessage{pose});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error()
                  == net::SessionEncodeError::InvalidTrackedCameraPose);

    pose = tracked_camera_pose();
    pose.director_epoch = {};
    result = net::encode_session(net::SessionMessage{pose});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error()
                  == net::SessionEncodeError::InvalidTrackedCameraPose);

    pose = tracked_camera_pose();
    pose.sequence = 0U;
    result = net::encode_session(net::SessionMessage{pose});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error()
                  == net::SessionEncodeError::InvalidTrackedCameraPose);

    const std::array<net::TrackedCameraSample, 13U> unusable_samples{{
        [] { auto s = tracked_camera_sample(); s.focus_distance_m = -1.0F; return s; }(),
        [] { auto s = tracked_camera_sample(); s.aperture_f_number = std::numeric_limits<float>::quiet_NaN(); return s; }(),
        [] { auto s = tracked_camera_sample(); s.focal_length_mm = 2001.0F; return s; }(),
        [] { auto s = tracked_camera_sample();
             s.position_x = std::numeric_limits<float>::quiet_NaN();
             return s; }(),
        [] { auto s = tracked_camera_sample();
             s.position_y = std::numeric_limits<float>::infinity();
             return s; }(),
        [] { auto s = tracked_camera_sample();
             s.position_z = net::max_tracked_position_metres * 2.0F;
             return s; }(),
        [] { auto s = tracked_camera_sample();
             s.yaw_turns = net::max_tracked_turns * 2.0F;
             return s; }(),
        // A field of view of zero or half a turn cannot build a frustum.
        [] { auto s = tracked_camera_sample();
             s.field_of_view_turns = 0.0F;
             return s; }(),
        [] { auto s = tracked_camera_sample();
             s.field_of_view_turns = 0.5F;
             return s; }(),
        [] { auto s = tracked_camera_sample();
             s.aspect_ratio = 0.0F;
             return s; }(),
        [] { auto s = tracked_camera_sample();
             s.aspect_ratio = net::max_tracked_aspect_ratio * 2.0F;
             return s; }(),
        [] { auto s = tracked_camera_sample();
             s.zoom_encoder = std::numeric_limits<float>::quiet_NaN();
             return s; }(),
        [] { auto s = tracked_camera_sample();
             s.focus_encoder = net::max_tracked_lens_encoder * 2.0F;
             return s; }(),
    }};
    for (const net::TrackedCameraSample& sample : unusable_samples) {
        SESSION_CHECK(!net::valid(sample));
        pose = tracked_camera_pose();
        pose.samples.assign(1U, sample);
        result = net::encode_session(net::SessionMessage{pose});
        SESSION_CHECK(!result.has_value());
        SESSION_CHECK(result.error()
                      == net::SessionEncodeError::InvalidTrackedCameraPose);
    }
    SESSION_CHECK(net::valid(tracked_camera_sample()));

    auto heartbeat_message = heartbeat();
    heartbeat_message.session_id = {};
    result = net::encode_session(net::SessionMessage{heartbeat_message});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidHeartbeat);

    heartbeat_message = heartbeat();
    heartbeat_message.director_epoch = {};
    result = net::encode_session(net::SessionMessage{heartbeat_message});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidHeartbeat);

    heartbeat_message = heartbeat();
    heartbeat_message.sequence = 0U;
    result = net::encode_session(net::SessionMessage{heartbeat_message});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidHeartbeat);

    heartbeat_message = heartbeat();
    heartbeat_message.response_timeout_ms =
        net::min_heartbeat_response_timeout_ms - 1U;
    result = net::encode_session(net::SessionMessage{heartbeat_message});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidHeartbeat);

    heartbeat_message = heartbeat();
    heartbeat_message.response_timeout_ms =
        net::max_heartbeat_response_timeout_ms + 1U;
    result = net::encode_session(net::SessionMessage{heartbeat_message});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidHeartbeat);

    heartbeat_message = heartbeat();
    heartbeat_message.released_through.next_command = command::CommandId{0U};
    result = net::encode_session(net::SessionMessage{heartbeat_message});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidHeartbeat);

    heartbeat_message = heartbeat();
    heartbeat_message.response_timeout_ms = net::min_heartbeat_response_timeout_ms;
    result = net::encode_session(net::SessionMessage{heartbeat_message});
    SESSION_CHECK(result.has_value());
    heartbeat_message.response_timeout_ms = net::max_heartbeat_response_timeout_ms;
    result = net::encode_session(net::SessionMessage{heartbeat_message});
    SESSION_CHECK(result.has_value());

    decoded = net::decode_session(raw_heartbeat_with_short_timeout());
    SESSION_CHECK(!decoded.has_value());
    SESSION_CHECK(decoded.error() == net::SessionDecodeError::InvalidHeartbeat);

    auto heartbeat_ack_message = heartbeat_ack();
    heartbeat_ack_message.session_id = {};
    result = net::encode_session(net::SessionMessage{heartbeat_ack_message});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidHeartbeatAck);

    heartbeat_ack_message = heartbeat_ack();
    heartbeat_ack_message.director_epoch = {};
    result = net::encode_session(net::SessionMessage{heartbeat_ack_message});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidHeartbeatAck);

    heartbeat_ack_message = heartbeat_ack();
    heartbeat_ack_message.sequence = 0U;
    result = net::encode_session(net::SessionMessage{heartbeat_ack_message});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidHeartbeatAck);

    heartbeat_ack_message = heartbeat_ack();
    heartbeat_ack_message.acknowledged_sequence = 0U;
    result = net::encode_session(net::SessionMessage{heartbeat_ack_message});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidHeartbeatAck);

    heartbeat_ack_message = heartbeat_ack();
    heartbeat_ack_message.visible_through.next_command = command::CommandId{0U};
    result = net::encode_session(net::SessionMessage{heartbeat_ack_message});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidHeartbeatAck);

    // The two values belong to opposite-direction sequence domains and are
    // intentionally not compared by the codec.
    heartbeat_ack_message = heartbeat_ack();
    heartbeat_ack_message.sequence = 1U;
    heartbeat_ack_message.acknowledged_sequence =
        std::numeric_limits<std::uint64_t>::max();
    result = net::encode_session(net::SessionMessage{heartbeat_ack_message});
    SESSION_CHECK(result.has_value());

    decoded = net::decode_session(raw_heartbeat_ack_with_zero_acknowledgement());
    SESSION_CHECK(!decoded.has_value());
    SESSION_CHECK(decoded.error() == net::SessionDecodeError::InvalidHeartbeatAck);

    auto goodbye_message = goodbye();
    goodbye_message.session_id = {};
    result = net::encode_session(net::SessionMessage{goodbye_message});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidGoodbye);

    goodbye_message = goodbye();
    goodbye_message.director_epoch = {};
    result = net::encode_session(net::SessionMessage{goodbye_message});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidGoodbye);

    goodbye_message = goodbye();
    goodbye_message.sequence = 0U;
    result = net::encode_session(net::SessionMessage{goodbye_message});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidGoodbye);

    goodbye_message = goodbye();
    goodbye_message.reason = net::GoodbyeReason::None;
    result = net::encode_session(net::SessionMessage{goodbye_message});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidGoodbye);

    goodbye_message = goodbye();
    goodbye_message.reason = static_cast<net::GoodbyeReason>(255U);
    result = net::encode_session(net::SessionMessage{goodbye_message});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidGoodbye);

    goodbye_message = goodbye();
    goodbye_message.final_cursor.next_command = command::CommandId{0U};
    result = net::encode_session(net::SessionMessage{goodbye_message});
    SESSION_CHECK(!result.has_value());
    SESSION_CHECK(result.error() == net::SessionEncodeError::InvalidGoodbye);

    decoded = net::decode_session(raw_goodbye_with_no_reason());
    SESSION_CHECK(!decoded.has_value());
    SESSION_CHECK(decoded.error() == net::SessionDecodeError::InvalidGoodbye);
}

void decode_and_reencode_is_deterministic() {
    for (const auto& message : every_message()) {
        const auto first = encode(message);
        auto decoded = net::decode_session(first);
        SESSION_CHECK(decoded.has_value());
        const auto second = encode(decoded.value());
        SESSION_CHECK(second == first);
    }
}

// The new fields default to zero, so an ordinary heartbeat encodes exactly as
// it did before. A node that has something to say about physical presentation
// round-trips it, and a self-contradictory report is refused rather than
// arriving as a partially believable barrier state.
void runtime_output_sync_round_trips_and_fails_closed() {
    auto message = heartbeat_ack();
    SESSION_CHECK(message.output_sync == net::RuntimeOutputSync{});

    message.output_sync = net::RuntimeOutputSync{
        net::RuntimeBarrierState::Joined, 3U, 5U, 2U, 2U,
        900U, 1U, 4U, 7U, 2U, 1U};
    message.output_sync.barrier_presents = 888U;
    message.output_sync.barrier_presents_in_sync = 887U;
    message.output_sync.barrier_flips_in_sync = 886U;
    message.output_sync.reference_clock =
        net::RuntimeReferenceClockState::Locked;
    message.output_sync.reference_frame_counter = 42U;
    auto encoded = net::encode_session(net::SessionMessage{message});
    SESSION_CHECK(encoded.has_value());
    auto decoded = net::decode_session(encoded.value());
    SESSION_CHECK(decoded.has_value());
    const auto* round_tripped =
        std::get_if<net::HeartbeatAck>(&decoded.value());
    SESSION_CHECK(round_tripped != nullptr);
    SESSION_CHECK(*round_tripped == message);

    // More joined outputs than the node has is a renderer bug, not a barrier.
    auto impossible = heartbeat_ack();
    impossible.output_sync.barrier = net::RuntimeBarrierState::Joined;
    impossible.output_sync.joined_outputs = 3U;
    impossible.output_sync.expected_outputs = 2U;
    auto refused = net::encode_session(net::SessionMessage{impossible});
    SESSION_CHECK(!refused.has_value());
    SESSION_CHECK(refused.error() ==
                  net::SessionEncodeError::InvalidHeartbeatAck);

    // Unsupported is a legal, meaningful report: a presentation backend that
    // cannot join a barrier at all must be able to say so.
    auto unsupported = heartbeat_ack();
    unsupported.output_sync.barrier = net::RuntimeBarrierState::Unsupported;
    unsupported.output_sync.expected_outputs = 2U;
    auto accepted = net::encode_session(net::SessionMessage{unsupported});
    SESSION_CHECK(accepted.has_value());
    auto reparsed = net::decode_session(accepted.value());
    SESSION_CHECK(reparsed.has_value());
    SESSION_CHECK(std::get_if<net::HeartbeatAck>(&reparsed.value())
                      ->output_sync.barrier ==
                  net::RuntimeBarrierState::Unsupported);
}

// The node's tracking report. Defaults to zero, so an ack from a node with no
// tracking encodes exactly as it did before this field existed; a node that has
// something to say round-trips every counter.
// A node that cannot draw its frames says where and why. Every field travels
// on its own, and a report that contradicts itself is refused rather than
// shown to the operator as a believable cause.
void runtime_present_failure_round_trips_and_fails_closed() {
    auto message = heartbeat_ack();
    SESSION_CHECK(message.output_sync.present_failure == core::PresentFailure{});

    message.output_sync.present_failure = core::PresentFailure{
        5'120U, core::PresentStep::DrawMap, core::GpuRefusal::BufferInUse,
        100'020U, 2'391U, true};
    auto encoded = net::encode_session(net::SessionMessage{message});
    SESSION_CHECK(encoded.has_value());
    auto decoded = net::decode_session(encoded.value());
    SESSION_CHECK(decoded.has_value());
    const auto* round_tripped =
        std::get_if<net::HeartbeatAck>(&decoded.value());
    SESSION_CHECK(round_tripped != nullptr);
    SESSION_CHECK(*round_tripped == message);
    const auto& failure = round_tripped->output_sync.present_failure;
    SESSION_CHECK(failure.failed_frames == 5'120U);
    SESSION_CHECK(failure.step == core::PresentStep::DrawMap);
    SESSION_CHECK(failure.refusal == core::GpuRefusal::BufferInUse);
    SESSION_CHECK(failure.map_node == 100'020U);
    SESSION_CHECK(failure.show_frame == 2'391U);
    SESSION_CHECK(failure.ongoing);

    // Recovered: the cause stays for the operator, the flag clears alone.
    auto recovered = message;
    recovered.output_sync.present_failure.ongoing = false;
    recovered.output_sync.present_failure.refusal =
        core::GpuRefusal::UploadSpaceFull;
    auto again = net::decode_session(
        net::encode_session(net::SessionMessage{recovered}).value());
    SESSION_CHECK(again.has_value());
    SESSION_CHECK(!std::get_if<net::HeartbeatAck>(&again.value())
                       ->output_sync.present_failure.ongoing);
    SESSION_CHECK(std::get_if<net::HeartbeatAck>(&again.value())
                      ->output_sync.present_failure.refusal ==
                  core::GpuRefusal::UploadSpaceFull);

    const auto refused = [](core::PresentFailure failure) {
        auto bad = heartbeat_ack();
        bad.output_sync.present_failure = failure;
        auto result = net::encode_session(net::SessionMessage{bad});
        return !result.has_value() &&
               result.error() == net::SessionEncodeError::InvalidHeartbeatAck;
    };
    // Failed frames with no step to name.
    SESSION_CHECK(refused({3U, core::PresentStep::None,
                           core::GpuRefusal::None, 0U, 0U, false}));
    // A step with no failed frames.
    SESSION_CHECK(refused({0U, core::PresentStep::Draw,
                           core::GpuRefusal::None, 0U, 0U, false}));
    // A map named for a step that is not about a map.
    SESSION_CHECK(refused({1U, core::PresentStep::Submit,
                           core::GpuRefusal::None, 7U, 0U, true}));
    // Values the protocol does not define.
    SESSION_CHECK(refused({1U, static_cast<core::PresentStep>(99U),
                           core::GpuRefusal::None, 0U, 0U, true}));
    SESSION_CHECK(refused({1U, core::PresentStep::Draw,
                           static_cast<core::GpuRefusal>(99U), 0U, 0U, true}));
}

void runtime_tracking_round_trips() {
    auto message = heartbeat_ack();
    SESSION_CHECK(message.tracking == net::RuntimeTracking{});

    message.tracking = net::RuntimeTracking{
        4'096U, 3U, 1'200U, 44U, 900U, 300U, 2U, 9U};
    auto encoded = net::encode_session(net::SessionMessage{message});
    SESSION_CHECK(encoded.has_value());
    auto decoded = net::decode_session(encoded.value());
    SESSION_CHECK(decoded.has_value());
    const auto* round_tripped =
        std::get_if<net::HeartbeatAck>(&decoded.value());
    SESSION_CHECK(round_tripped != nullptr);
    SESSION_CHECK(*round_tripped == message);
    SESSION_CHECK(round_tripped->tracking.pose_sequence == 4'096U);
    SESSION_CHECK(round_tripped->tracking.poses_missed == 3U);
    SESSION_CHECK(round_tripped->tracking.plates_skipped == 300U);
    // Carried apart from poses_missed, because they are different faults on
    // different machines and a wire that merged them would hide one.
    SESSION_CHECK(round_tripped->tracking.poses_coalesced == 9U);

    // Each counter is carried in its own field rather than packed, so a report
    // that moves one of them cannot silently move another.
    auto only_holds = heartbeat_ack();
    only_holds.tracking.held_frames = 17U;
    auto held = net::encode_session(net::SessionMessage{only_holds});
    SESSION_CHECK(held.has_value());
    auto reparsed = net::decode_session(held.value());
    SESSION_CHECK(reparsed.has_value());
    const auto& tracking =
        std::get_if<net::HeartbeatAck>(&reparsed.value())->tracking;
    SESSION_CHECK(tracking.held_frames == 17U);
    SESSION_CHECK(tracking.poses_applied == 0U);
    SESSION_CHECK(tracking.plates_drawn == 0U);
    SESSION_CHECK(tracking.poses_coalesced == 0U);
}

void runtime_renderer_readiness_round_trips_and_fails_closed() {
    auto message = heartbeat_ack();
    SESSION_CHECK(message.renderer == net::RuntimeRendererReadiness{});
    message.renderer = net::RuntimeRendererReadiness{
        0x52454E44504F4C31ULL,
        net::RuntimeRendererReadinessState::Ready,
        net::RuntimeRendererBackend::Advanced,
        net::RuntimeRendererBackend::Advanced,
        0U,
        true,
        false,
    };
    message.renderer.conformance.state = core::ConformanceState::Passed;
    message.renderer.conformance.version = core::render_conformance_version;
    message.renderer.conformance.request = 901U;
    const std::string adapter_name{"Conformance test GPU"};
    std::copy(adapter_name.begin(), adapter_name.end(), message.renderer.conformance.adapter.name.begin());
    message.renderer.conformance.adapter.driver_version = 0x123456789abcdef0ULL;
    message.renderer.conformance.adapter.vendor = 0x10deU;
    message.renderer.conformance.adapter.device = 0x2204U;
    message.renderer.conformance.strict_float = true;
    for (std::size_t i = 0; i < message.renderer.conformance.passes.size(); ++i)
        message.renderer.conformance.passes[i] = 0xfeed000000000000ULL + i;
    for (std::size_t i = 0; i < message.renderer.conformance.shader_bundle.size(); ++i)
        message.renderer.conformance.shader_bundle[i] = static_cast<core::u8>(i + 17U);
    message.renderer.timing = {.sequence = 13U, .profile_digest = 19U, .cpu_record_ns = 211U,
        .gpu_ns = 307U, .upload_bytes = 409U, .present_ns = 503U, .presents = 601U,
        .presents_in_sync = 599U, .flips_in_sync = 593U, .query_overflows = 2U};
    message.renderer.timing.gpu_valid = true;
    for (std::size_t i = 0U; i < core::render_pass_count; ++i) message.renderer.timing.pass_ns[i] = 701U + i;
    message.renderer.temporal_restarts = 7U;
    message.renderer.temporal_restart_frame = 4294967333ULL;

    auto encoded = net::encode_session(net::SessionMessage{message});
    SESSION_CHECK(encoded.has_value());
    auto decoded = net::decode_session(encoded.value());
    SESSION_CHECK(decoded.has_value());
    const auto* const round_tripped =
        std::get_if<net::HeartbeatAck>(&decoded.value());
    SESSION_CHECK(round_tripped != nullptr);
    SESSION_CHECK(*round_tripped == message);

    auto requested = heartbeat(); requested.conformance_request = 0x1122334455667788ULL;
    auto request_bytes = net::encode_session(net::SessionMessage{requested});
    SESSION_CHECK(request_bytes);
    auto request_back = net::decode_session(request_bytes.value());
    SESSION_CHECK(request_back && std::get<net::Heartbeat>(request_back.value()) == requested);
    auto failed_check = message;
    failed_check.renderer.conformance.state = core::ConformanceState::Failed;
    failed_check.renderer.conformance.failed_pass = core::ConformancePass::Fog;
    auto failed_bytes = net::encode_session(net::SessionMessage{failed_check});
    SESSION_CHECK(failed_bytes);
    auto failed_back = net::decode_session(failed_bytes.value());
    SESSION_CHECK(failed_back && std::get<net::HeartbeatAck>(failed_back.value()) == failed_check);
    for (unsigned invalid = 0; invalid < 5; ++invalid) {
        auto bad = message;
        auto& proof = bad.renderer.conformance;
        if (invalid == 0) proof.state = static_cast<core::ConformanceState>(255U);
        if (invalid == 1) proof.passes[5] = 0U;
        if (invalid == 2) proof.shader_bundle.fill(0U);
        if (invalid == 3) proof.adapter.name.fill('x');
        if (invalid == 4) proof.failed_pass = static_cast<core::ConformancePass>(255U);
        SESSION_CHECK(!net::encode_session(net::SessionMessage{bad}));
    }
    auto stale_unknown = heartbeat_ack();
    stale_unknown.renderer.profile_digest = 17U;
    auto refused =
        net::encode_session(net::SessionMessage{stale_unknown});
    SESSION_CHECK(!refused.has_value());
    SESSION_CHECK(refused.error() ==
                  net::SessionEncodeError::InvalidHeartbeatAck);

    auto contradictory_fallback = message;
    contradictory_fallback.renderer.state =
        net::RuntimeRendererReadinessState::BasicFallback;
    contradictory_fallback.renderer.selected =
        net::RuntimeRendererBackend::Advanced;
    contradictory_fallback.renderer.fallback = true;
    refused = net::encode_session(
        net::SessionMessage{contradictory_fallback});
    SESSION_CHECK(!refused.has_value());
    SESSION_CHECK(refused.error() ==
                  net::SessionEncodeError::InvalidHeartbeatAck);

    // A node short of GPU memory carries both figures across the wire, and
    // one whose need fits its headroom contradicts itself.
    auto memory_short = message;
    memory_short.renderer.state =
        net::RuntimeRendererReadinessState::VideoMemoryShort;
    memory_short.renderer.satisfied = false;
    memory_short.renderer.video_memory_needed = 5'100'000'017ULL;
    memory_short.renderer.video_memory_free = 2'400'000'029ULL;
    auto memory_bytes = net::encode_session(net::SessionMessage{memory_short});
    SESSION_CHECK(memory_bytes.has_value());
    auto memory_back = net::decode_session(memory_bytes.value());
    SESSION_CHECK(memory_back.has_value());
    SESSION_CHECK(std::get<net::HeartbeatAck>(memory_back.value()) ==
                  memory_short);
    auto memory_fits = memory_short;
    memory_fits.renderer.video_memory_free = 5'100'000'017ULL;
    refused = net::encode_session(net::SessionMessage{memory_fits});
    SESSION_CHECK(!refused.has_value());
    SESSION_CHECK(refused.error() ==
                  net::SessionEncodeError::InvalidHeartbeatAck);
}

void runtime_media_status_round_trips_and_is_bounded() {
    auto message = heartbeat_ack();
    message.media_assets = {
        net::RuntimeMediaAssetStatus{
            41U,
            net::RuntimeMediaDecodeState::SoftwareOnly,
            net::RuntimeProxyState::PreferFallback,
            net::RuntimeProxyReason::CacheGenerating,
            net::RuntimeProxyIssue::None,
            true,
            false,
            123U,
            1'000U,
        },
        net::RuntimeMediaAssetStatus{
            42U,
            net::RuntimeMediaDecodeState::HardwareAccelerated,
            net::RuntimeProxyState::ProxyReady,
            net::RuntimeProxyReason::None,
            net::RuntimeProxyIssue::None,
        },
    };
    auto encoded = net::encode_session(net::SessionMessage{message});
    SESSION_CHECK(encoded.has_value());
    auto decoded = net::decode_session(encoded.value());
    SESSION_CHECK(decoded.has_value());
    const auto* const round_tripped =
        std::get_if<net::HeartbeatAck>(&decoded.value());
    SESSION_CHECK(round_tripped != nullptr);
    SESSION_CHECK(*round_tripped == message);

    auto duplicate = message;
    duplicate.media_assets[1U].asset_id =
        duplicate.media_assets[0U].asset_id;
    auto refused = net::encode_session(net::SessionMessage{duplicate});
    SESSION_CHECK(!refused.has_value());
    SESSION_CHECK(refused.error() ==
                  net::SessionEncodeError::InvalidHeartbeatAck);

    auto impossible_progress = message;
    impossible_progress.media_assets[0U].transcode_completed_frames = 1'001U;
    refused = net::encode_session(
        net::SessionMessage{impossible_progress});
    SESSION_CHECK(!refused.has_value());
    SESSION_CHECK(refused.error() ==
                  net::SessionEncodeError::InvalidHeartbeatAck);

    auto oversized = heartbeat_ack();
    oversized.media_assets.resize(net::max_runtime_media_assets + 1U);
    for (std::size_t index = 0U; index < oversized.media_assets.size(); ++index) {
        oversized.media_assets[index].asset_id =
            static_cast<mediaserver::core::u64>(index + 1U);
    }
    refused = net::encode_session(net::SessionMessage{oversized});
    SESSION_CHECK(!refused.has_value());
    SESSION_CHECK(refused.error() ==
                  net::SessionEncodeError::InvalidHeartbeatAck);
}

void current_version_messages_match_frozen_goldens() {
    // Session v12 adds the laser pointer message.
    const std::array<std::string_view, 16U> expected{
        "cc000000140000004d534e5000000a000c000600050008000a00000000010c001c0000001800940008000a000c001c00"
        "2c0040004400540007006c0018000000000000010c000c00807060504030201001000000000000008070605040302010"
        "020000000000000080706050403020100300000000000000000000000700000000000000000000000000000000000000"
        "0a0000000000000014000000000000001e00000000000000000000010000000000000004000000000800000000000000"
        "00000008000000000000000400000000",
        "c4000000140000004d534e5000000a000c000600050008000a00000000020c0018000000140090000600080018002800"
        "05003800500068001400000000020c008070605040302010010000000000000080706050403020100400000000000000"
        "807060504030201005000000000000000a0000000000000014000000000000001e000000000000000c00000000000000"
        "170000000000000021000000000000000000000100000000000000040000000008000000000000000000000800000000"
        "0000000400000000",
        "cc000000140000004d534e5000000a000e000600050008000a00000000030c001400000000000e00580004004c001400"
        "2c0048000e000000807060504030201004000000000000000a0000000000000014000000000000001e00000000000000"
        "0b0000000000000016000000000000002000000000000000000000001000000007000000000000000000000003000000"
        "300000001800000004000000e2ffffff040000000200000005060000f2ffffff04000000010000000400060008000400"
        "06000000040000000300000001020300",
        "74000000180000004d534e500000000000000a000c000600050008000a00000000040c00100000000c003c0004003400"
        "140030000c000000807060504030201004000000000000000c0000000000000017000000000000002100000000000000"
        "000000000c00000008000000000000000400000010203040",
        "64000000180000004d534e500000000000000a000e000600050008000a00000000050c001000000000000a0034000400"
        "2c0014000a000000807060504030201004000000000000000c0000000000000017000000000000002100000000000000"
        "0900000000000000",
        "64000000180000004d534e500000000000000a000e000600050008000a00000000060c001000000000000a0034000400"
        "2c0014000a000000807060504030201004000000000000000c0000000000000017000000000000002100000000000000"
        "0a00000000000000",
        "6c000000140000004d534e5000000a000c000600050008000a00000000070c00100000000c0040000600080018002800"
        "0c0000000000040080706050403020100100000000000000807060504030201005000000000000000c00000000000000"
        "17000000000000002100000000000000",
        "6c000000180000004d534e500000000000000a000c000600050008000a00000000080c00100000000c003c0004002c00"
        "340014000c000000807060504030201004000000000000000b0000000000000016000000000000002000000000000000"
        "0b000000000000000700000000000000",
        "8c000000140000004d534e5000000a000c000600050008000a00000000090c0018000000140058000800300038001800"
        "07004000480050001400000000000001807060504030201004000000000000000b000000000000001600000000000000"
        "20000000000000000c000000000000000700000000000000d2040000000000002e160000000000000d00000000000000",
        "a4000000180000004d534e500000000000000a000e000600050008000a000000000a0c0018000000000012006c000400"
        "140054005c00640024003c00120000008070605040302010040000000000000080706050403020100500000000000000"
        "0a0000000000000014000000000000001e000000000000000b0000000000000016000000000000002000000000000000"
        "0d0000000000000007000000000000002900000000000000",
        "7c000000140000004d534e5000000a000e000600050008000a000000000b0c001400000000000e004c00040014004400"
        "28002c000e00000080706050403020100400000000000000807060504030201005000000000000000000000088130000"
        "0b00000000000000160000000000000020000000000000000e00000000000000",
        "dc000000140000004d534e5000000a000e000600050008000a000000000c0c0068000000000062005800080018004000"
        "480028005000060000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "000000000000000000000000000000000000000000000000000000000000000000000000000007006200000000000107"
        "80706050403020100400000000000000807060504030201005000000000000000b000000000000001600000000000000"
        "20000000000000000d000000000000000e000000000000008877665544332211",
        "7c000000180000004d534e500000000000000a000e000600050008000a000000000d0c001400000000000e0048000800"
        "18004000070028000e000000000000028070605040302010040000000000000080706050403020100500000000000000"
        "0b00000000000000160000000000000020000000000000000f00000000000000",
        "fc000000140000004d534e5000000a000c000600050008000a000000000e0c001400000010004400040014002c003400"
        "3c002800100000008070605040302010040000000000000080706050403020100500000000000000000000001c000000"
        "10000000000000000900000000000000001000000000000001000000300000002c005400440008000c00100014001800"
        "1c00200024004c0028002c003000340005000600070038003c0040002c000000000101010000c03f000040bf00008840"
        "0000803e000000be0000803dcdcc4c3d398ee33f0300000000e440460031544700409a440000aa42000098406666e63f"
        "0010000000000000db03000000000000",
        "94000000140000004d534e5000000a000c000600050008000a000000000f0c00200000001c0058000800180030003800"
        "40000500060028002c004800500007001c00000000010101807060504030201004000000000000008070605040302010"
        "050000000000000060ea0000e903000011000000000000000310000000000000d5000000000000000700000000000000"
        "2823000000000000",
        "ac000000140000004d534e5000000a000c000600050008000a00000000100c0018000000140048000800180038004000"
        "070028003000340014000000000000018070605040302010040000000000000080706050403020100500000000000000"
        "00207044000044c124000000140000001300000000000000e11000000000000002000000030000000400000004000000"
        "000020410000a041000038410000ac41000050410000c04100002f4400009643",
    };
    const auto messages = every_message();
    SESSION_CHECK(messages.size() == expected.size());
    SESSION_CHECK(net::SessionMessage{client_hello()}.index() == 0U);
    SESSION_CHECK(net::SessionMessage{frame_committed()}.index() == 8U);
    SESSION_CHECK(net::SessionMessage{frame_commit_permit()}.index() == 9U);
    SESSION_CHECK(net::SessionMessage{heartbeat()}.index() == 10U);
    SESSION_CHECK(net::SessionMessage{heartbeat_ack()}.index() == 11U);
    SESSION_CHECK(net::SessionMessage{goodbye()}.index() == 12U);
    SESSION_CHECK(net::SessionMessage{tracked_camera_pose()}.index() == 13U);
    SESSION_CHECK(net::SessionMessage{priority_transport()}.index() == 14U);
    SESSION_CHECK(net::SessionMessage{pointer_overlay()}.index() == 15U);
    SESSION_CHECK(static_cast<unsigned int>(fb::SessionBody::FrameCommitPermit) == 10U);
    SESSION_CHECK(static_cast<unsigned int>(fb::SessionBody::Heartbeat) == 11U);
    SESSION_CHECK(static_cast<unsigned int>(fb::SessionBody::HeartbeatAck) == 12U);
    SESSION_CHECK(static_cast<unsigned int>(fb::SessionBody::Goodbye) == 13U);
    SESSION_CHECK(
        static_cast<unsigned int>(fb::SessionBody::TrackedCameraPose) == 14U);
    SESSION_CHECK(
        static_cast<unsigned int>(fb::SessionBody::PriorityTransport) == 15U);
    SESSION_CHECK(
        static_cast<unsigned int>(fb::SessionBody::PointerOverlay) == 16U);
    bool all_match = true;
    for (std::size_t index = 0U; index < messages.size(); ++index) {
        const auto actual = to_hex(encode(messages[index]));
        if (actual != expected[index]) {
            all_match = false;
            std::size_t difference = 0U;
            while (difference < actual.size() && difference < expected[index].size()
                   && actual[difference] == expected[index][difference]) {
                ++difference;
            }
            std::cerr << "golden mismatch " << index << "\nexpected: " << expected[index]
                      << "\nactual:   " << actual << "\nfirst difference: " << difference
                      << ", lengths " << expected[index].size() << '/' << actual.size() << '\n';
        }
    }
    SESSION_CHECK(all_match);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"every message round trip", &every_message_round_trips},
        {"exact prefix and truncations", &exact_prefix_and_all_truncations_are_rejected},
        {"packet count and byte caps", &packet_count_aggregate_and_message_caps_are_enforced},
        {"invalid semantics", &invalid_public_semantics_are_rejected},
        {"deterministic reencode", &decode_and_reencode_is_deterministic},
        {"frozen current-version goldens",
         &current_version_messages_match_frozen_goldens},
        {"runtime output sync round trip",
         &runtime_output_sync_round_trips_and_fails_closed},
        {"runtime renderer readiness round trip",
         &runtime_renderer_readiness_round_trips_and_fails_closed},
        {"runtime media status round trip",
         &runtime_media_status_round_trips_and_is_bounded},
        {"runtime tracking round trip", &runtime_tracking_round_trips},
        {"runtime present failure round trip",
         &runtime_present_failure_round_trips_and_fails_closed},
        {"priority transport admits only operator anchors",
         &priority_transport_admits_only_operator_anchors},
        {"pointer overlay refuses what a node could not draw",
         &pointer_overlay_refuses_what_a_node_could_not_draw},
    };

    std::size_t failures = 0U;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[pass] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << (tests.size() - failures) << '/' << tests.size() << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
