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

namespace command = mediaserver::cmd;
namespace document = mediaserver::doc;
namespace net = mediaserver::net;
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
    return net::FrameCommitted{id(4U), 12U, 7U, cursor(11U, 22U, 32U)};
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
    return net::HeartbeatAck{
        id(4U),
        id(5U),
        13U,
        14U,
        cursor(11U, 22U, 32U),
        0x1122334455667788ULL,
        true,
    };
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

void current_version_messages_match_frozen_goldens() {
    const std::array<std::string_view, 13U> expected{
        "cc000000140000004d534e5000000a000c000600050008000a000000000102001c00000018009400"
        "08000a000c001c002c0040004400540007006c001800000000000001020002008070605040302010"
        "01000000000000008070605040302010020000000000000080706050403020100300000000000000"
        "0000000007000000000000000000000000000000000000000a000000000000001400000000000000"
        "1e000000000000000000000100000000000000040000000008000000000000000000000800000000"
        "0000000400000000",
        "c4000000140000004d534e5000000a000c000600050008000a000000000202001800000014009000"
        "06000800180028000500380050006800140000000002020080706050403020100100000000000000"
        "80706050403020100400000000000000807060504030201005000000000000000a00000000000000"
        "14000000000000001e000000000000000c0000000000000017000000000000002100000000000000"
        "00000001000000000000000400000000080000000000000000000008000000000000000400000000",
        "cc000000140000004d534e5000000a000e000600050008000a000000000302001400000000000e005800"
        "04004c0014002c0048000e000000807060504030201004000000000000000a0000000000000014000000"
        "000000001e000000000000000b0000000000000016000000000000002000000000000000000000001000"
        "000007000000000000000000000003000000300000001800000004000000e2ffffff0400000002000000"
        "05060000f2ffffff0400000001000000040006000800040006000000040000000300000001020300",
        "74000000180000004d534e500000000000000a000c000600050008000a00000000040200100000000c00"
        "3c0004003400140030000c000000807060504030201004000000000000000c0000000000000017000000"
        "000000002100000000000000000000000c00000008000000000000000400000010203040",
        "64000000180000004d534e500000000000000a000e000600050008000a00000000050200100000000000"
        "0a00340004002c0014000a000000807060504030201004000000000000000c0000000000000017000000"
        "0000000021000000000000000900000000000000",
        "64000000180000004d534e500000000000000a000e000600050008000a00000000060200100000000000"
        "0a00340004002c0014000a000000807060504030201004000000000000000c0000000000000017000000"
        "0000000021000000000000000a00000000000000",
        "6c000000140000004d534e5000000a000c000600050008000a00000000070200100000000c0040000600"
        "0800180028000c0000000000040080706050403020100100000000000000807060504030201005000000"
        "000000000c0000000000000017000000000000002100000000000000",
        "6c000000180000004d534e500000000000000a000c000600050008000a00000000080200100000000c00"
        "3c0004002c00340014000c000000807060504030201004000000000000000b0000000000000016000000"
        "0000000020000000000000000b000000000000000700000000000000",
        "6c000000180000004d534e500000000000000a000c000600050008000a00000000090200100000000c00"
        "3c0004002c00340014000c000000807060504030201004000000000000000b0000000000000016000000"
        "0000000020000000000000000c000000000000000700000000000000",
        "a4000000180000004d534e500000000000000a000e000600050008000a000000000a0200180000000000"
        "12006c000400140054005c00640024003c0012000000807060504030201004000000000000008070605040"
        "30201005000000000000000a0000000000000014000000000000001e000000000000000b000000000000"
        "00160000000000000020000000000000000d0000000000000007000000000000002900000000000000",
        "7c000000140000004d534e5000000a000e000600050008000a000000000b02001400000000000e004c00"
        "04001400440028002c000e00000080706050403020100400000000000000807060504030201005000000"
        "0000000000000000881300000b00000000000000160000000000000020000000000000000e0000000000"
        "0000",
        "8c000000140000004d534e5000000a000e000600050008000a000000000c020018000000000012005800"
        "080018004000480028005000070012000000000000018070605040302010040000000000000080706050"
        "4030201005000000000000000b00000000000000160000000000000020000000000000000d0000000000"
        "00000e000000000000008877665544332211",
        "7c000000180000004d534e500000000000000a000e000600050008000a000000000d0200140000000000"
        "0e004800080018004000070028000e000000000000028070605040302010040000000000000080706050"
        "4030201005000000000000000b00000000000000160000000000000020000000000000000f0000000000"
        "0000",
    };
    const auto messages = every_message();
    SESSION_CHECK(messages.size() == expected.size());
    SESSION_CHECK(net::SessionMessage{client_hello()}.index() == 0U);
    SESSION_CHECK(net::SessionMessage{frame_committed()}.index() == 8U);
    SESSION_CHECK(net::SessionMessage{frame_commit_permit()}.index() == 9U);
    SESSION_CHECK(net::SessionMessage{heartbeat()}.index() == 10U);
    SESSION_CHECK(net::SessionMessage{heartbeat_ack()}.index() == 11U);
    SESSION_CHECK(net::SessionMessage{goodbye()}.index() == 12U);
    SESSION_CHECK(static_cast<unsigned int>(fb::SessionBody::FrameCommitPermit) == 10U);
    SESSION_CHECK(static_cast<unsigned int>(fb::SessionBody::Heartbeat) == 11U);
    SESSION_CHECK(static_cast<unsigned int>(fb::SessionBody::HeartbeatAck) == 12U);
    SESSION_CHECK(static_cast<unsigned int>(fb::SessionBody::Goodbye) == 13U);
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
