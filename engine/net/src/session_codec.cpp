#include <mediaserver/net/session_protocol.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

#include <flatbuffers/flatbuffers.h>

#include "session_protocol_generated.h"

namespace mediaserver::net {
namespace {

namespace fb = mediaserver::net::protocol_fb::v1;

struct PacketBudget final {
    std::size_t count{};
    std::size_t aggregate_bytes{};
};

[[nodiscard]] bool valid_cursor(const cmd::StreamCursor& cursor) noexcept {
    return static_cast<bool>(cursor.next_command);
}

[[nodiscard]] bool valid_limits(const SessionLimits& limits) noexcept {
    return limits.max_packet_bytes != 0U && limits.max_frame_bytes != 0U
        && limits.max_inflight_frames != 0U && limits.max_inflight_bytes != 0U
        && limits.max_checkpoint_bytes != 0U
        && limits.max_packet_bytes <= limits.max_frame_bytes
        && limits.max_frame_bytes <= limits.max_inflight_bytes
        && limits.max_checkpoint_bytes <= limits.max_inflight_bytes;
}

[[nodiscard]] bool valid_progress(
    const cmd::StreamCursor& before,
    const cmd::StreamCursor& after) noexcept {
    if (!valid_cursor(before) || !valid_cursor(after)
        || after.through_frame < before.through_frame || after.revision < before.revision
        || after.next_command < before.next_command) {
        return false;
    }
    return after.revision.value() - before.revision.value()
        == after.next_command.value() - before.next_command.value();
}

[[nodiscard]] bool valid_consecutive_frame_transition(
    const cmd::StreamCursor& before,
    const cmd::StreamCursor& after) noexcept {
    return valid_progress(before, after)
        && before.through_frame.value() != std::numeric_limits<core::u64>::max()
        && after.through_frame.value() == before.through_frame.value() + 1U;
}

[[nodiscard]] bool valid_mode(ResumeMode mode) noexcept {
    switch (mode) {
    case ResumeMode::UpToDate:
    case ResumeMode::Replay:
    case ResumeMode::Checkpoint:
        return true;
    case ResumeMode::None:
    default:
        return false;
    }
}

[[nodiscard]] bool valid_reject_code(RejectCode code) noexcept {
    switch (code) {
    case RejectCode::UnsupportedVersion:
    case RejectCode::ShowMismatch:
    case RejectCode::InvalidCursor:
    case RejectCode::ResumeUnavailable:
    case RejectCode::CapacityExceeded:
    case RejectCode::ProtocolError:
    case RejectCode::ServerShutdown:
    case RejectCode::NodeLeaseRejected:
        return true;
    case RejectCode::None:
    default:
        return false;
    }
}

[[nodiscard]] bool valid_client_hello(const ClientHello& value) noexcept {
    return value.min_version != 0U && value.max_version >= value.min_version
        && static_cast<bool>(value.show_id) && static_cast<bool>(value.node_id)
        && static_cast<bool>(value.incarnation)
        && value.node_index <= max_session_node_index
        && valid_cursor(value.cursor)
        && valid_limits(value.limits);
}

[[nodiscard]] bool valid_server_welcome(const ServerWelcome& value) noexcept {
    if (value.selected_version != current_session_format_version
        || !static_cast<bool>(value.show_id) || !static_cast<bool>(value.session_id)
        || !static_cast<bool>(value.director_epoch) || !valid_mode(value.mode)
        || !valid_progress(value.accepted_from, value.target)
        || !valid_limits(value.limits)) {
        return false;
    }
    if (value.mode == ResumeMode::UpToDate) {
        return value.accepted_from == value.target;
    }
    if (value.mode == ResumeMode::Replay) {
        return value.target.through_frame > value.accepted_from.through_frame;
    }
    return true;
}

[[nodiscard]] bool add_packet_size(
    std::size_t bytes,
    const SessionDecodeLimits& limits,
    PacketBudget& budget) noexcept {
    if (bytes == 0U || bytes > limits.max_packet_bytes
        || budget.count >= limits.max_packets_per_batch
        || budget.aggregate_bytes > limits.max_aggregate_packet_bytes
        || bytes > limits.max_aggregate_packet_bytes - budget.aggregate_bytes) {
        return false;
    }
    ++budget.count;
    budget.aggregate_bytes += bytes;
    return true;
}

[[nodiscard]] bool valid_frame_header(const FrameBatch& value) noexcept {
    if (!static_cast<bool>(value.session_id) || value.sequence == 0U
        || !valid_consecutive_frame_transition(value.before, value.after)) {
        return false;
    }

    const auto command_count =
        value.after.next_command.value() - value.before.next_command.value();
    if (command_count == std::numeric_limits<core::u64>::max()
        || command_count + 1U != static_cast<core::u64>(value.packets.size())) {
        return false;
    }
    return std::all_of(
        value.packets.begin(), value.packets.end(), [](const Packet& packet) {
            return !packet.bytes.empty();
        });
}

[[nodiscard]] bool valid_frame_batch(
    const FrameBatch& value,
    const SessionDecodeLimits& limits,
    PacketBudget* observed = nullptr) noexcept {
    if (!valid_frame_header(value)) {
        return false;
    }
    PacketBudget budget;
    for (const auto& packet : value.packets) {
        if (!add_packet_size(packet.bytes.size(), limits, budget)) {
            return false;
        }
    }
    if (observed != nullptr) {
        *observed = budget;
    }
    return true;
}

[[nodiscard]] bool valid_checkpoint_header(const CheckpointTransfer& value) noexcept {
    return static_cast<bool>(value.session_id) && value.sequence != 0U
        && valid_cursor(value.cursor) && !value.wire_packet.empty();
}

[[nodiscard]] bool valid_checkpoint_transfer(
    const CheckpointTransfer& value,
    const SessionDecodeLimits& limits,
    PacketBudget* observed = nullptr) noexcept {
    if (!valid_checkpoint_header(value)) {
        return false;
    }
    if (value.wire_packet.size() > limits.max_checkpoint_bytes) {
        return false;
    }
    if (observed != nullptr) {
        *observed = PacketBudget{1U, value.wire_packet.size()};
    }
    return true;
}

[[nodiscard]] bool valid_sync_complete(const SyncComplete& value) noexcept {
    return static_cast<bool>(value.session_id) && value.sequence != 0U
        && valid_cursor(value.target);
}

[[nodiscard]] bool valid_resume_ack(const ResumeAck& value) noexcept {
    return static_cast<bool>(value.session_id) && value.sequence != 0U
        && valid_cursor(value.target);
}

template <typename Acknowledgement>
[[nodiscard]] bool valid_frame_acknowledgement(
    const Acknowledgement& value) noexcept {
    return static_cast<bool>(value.session_id) && value.sequence != 0U
        && value.acknowledged_sequence != 0U && valid_cursor(value.after)
        && value.after.through_frame.value() != 0U;
}

[[nodiscard]] bool valid_frame_commit_permit(
    const FrameCommitPermit& value) noexcept {
    return static_cast<bool>(value.session_id)
        && static_cast<bool>(value.director_epoch) && value.sequence != 0U
        && value.acknowledged_sequence != 0U
        && value.sequence > value.acknowledged_sequence
        && value.barrier_ordinal != 0U
        && valid_consecutive_frame_transition(value.before, value.after);
}

[[nodiscard]] bool valid_heartbeat(const Heartbeat& value) noexcept {
    return static_cast<bool>(value.session_id)
        && static_cast<bool>(value.director_epoch) && value.sequence != 0U
        && value.response_timeout_ms >= min_heartbeat_response_timeout_ms
        && value.response_timeout_ms <= max_heartbeat_response_timeout_ms
        && valid_cursor(value.released_through);
}

[[nodiscard]] bool valid_heartbeat_ack(const HeartbeatAck& value) noexcept {
    return static_cast<bool>(value.session_id)
        && static_cast<bool>(value.director_epoch) && value.sequence != 0U
        && value.acknowledged_sequence != 0U
        && valid_cursor(value.visible_through);
}

[[nodiscard]] bool valid_goodbye_reason(GoodbyeReason reason) noexcept {
    switch (reason) {
    case GoodbyeReason::NormalShutdown:
    case GoodbyeReason::Restart:
    case GoodbyeReason::Maintenance:
    case GoodbyeReason::SessionReplaced:
        return true;
    case GoodbyeReason::None:
    default:
        return false;
    }
}

[[nodiscard]] bool valid_goodbye(const Goodbye& value) noexcept {
    return static_cast<bool>(value.session_id)
        && static_cast<bool>(value.director_epoch) && value.sequence != 0U
        && valid_goodbye_reason(value.reason) && valid_cursor(value.final_cursor);
}

[[nodiscard]] bool valid_reject(const Reject& value) noexcept {
    return valid_reject_code(value.code) && static_cast<bool>(value.show_id)
        && static_cast<bool>(value.director_epoch)
        && valid_cursor(value.authoritative_tail);
}

[[nodiscard]] bool estimated_size_fits(
    std::size_t packet_count,
    std::size_t packet_bytes,
    const SessionDecodeLimits& limits) noexcept {
    constexpr std::size_t envelope_allowance = 512U;
    constexpr std::size_t packet_allowance = 96U;
    if (limits.max_message_bytes < envelope_allowance
        || packet_count
            > (limits.max_message_bytes - envelope_allowance) / packet_allowance) {
        return false;
    }
    const auto overhead = envelope_allowance + packet_count * packet_allowance;
    return packet_bytes <= limits.max_message_bytes - overhead;
}

[[nodiscard]] fb::Id128 encode_id(Id128 value) noexcept {
    return fb::Id128{value.high, value.low};
}

[[nodiscard]] Id128 decode_id(const fb::Id128& value) noexcept {
    return Id128{value.high(), value.low()};
}

[[nodiscard]] fb::StreamCursor encode_cursor(const cmd::StreamCursor& value) noexcept {
    return fb::StreamCursor{
        value.through_frame.value(),
        value.revision.value(),
        value.next_command.value(),
    };
}

[[nodiscard]] cmd::StreamCursor decode_cursor(const fb::StreamCursor& value) noexcept {
    return cmd::StreamCursor{
        cmd::FrameNumber{value.through_frame()},
        doc::Version{value.revision()},
        cmd::CommandId{value.next_command_id()},
    };
}

[[nodiscard]] fb::SessionLimits encode_limits(const SessionLimits& value) noexcept {
    return fb::SessionLimits{
        value.max_packet_bytes,
        value.max_frame_bytes,
        value.max_inflight_frames,
        value.max_inflight_bytes,
        value.max_checkpoint_bytes,
    };
}

[[nodiscard]] SessionLimits decode_limits(const fb::SessionLimits& value) noexcept {
    return SessionLimits{
        value.max_packet_bytes(),
        value.max_frame_bytes(),
        value.max_inflight_frames(),
        value.max_inflight_bytes(),
        value.max_checkpoint_bytes(),
    };
}

[[nodiscard]] flatbuffers::uoffset_t verifier_limit(std::size_t value) noexcept {
    const auto maximum = static_cast<std::size_t>(
        std::numeric_limits<flatbuffers::uoffset_t>::max());
    return static_cast<flatbuffers::uoffset_t>(std::min(value, maximum));
}

[[nodiscard]] std::vector<std::byte> copy_bytes(
    const flatbuffers::Vector<std::uint8_t>& source) {
    std::vector<std::byte> result(source.size());
    if (!result.empty()) {
        std::memcpy(result.data(), source.data(), result.size());
    }
    return result;
}

} // namespace

core::Result<std::vector<std::byte>, SessionEncodeError> encode_session(
    const SessionMessage& message,
    const SessionDecodeLimits& limits) {
    auto effective_limits = limits;
    effective_limits.max_message_bytes = std::min(
        effective_limits.max_message_bytes,
        static_cast<std::size_t>(FLATBUFFERS_MAX_BUFFER_SIZE) - 1U);

    PacketBudget packet_budget;
    SessionEncodeError invalid_error = SessionEncodeError::SizeLimitExceeded;
    bool valid = false;
    bool packet_limit_failed = false;
    if (const auto* hello = std::get_if<ClientHello>(&message)) {
        valid = valid_client_hello(*hello);
        invalid_error = SessionEncodeError::InvalidClientHello;
    } else if (const auto* welcome = std::get_if<ServerWelcome>(&message)) {
        valid = valid_server_welcome(*welcome);
        invalid_error = SessionEncodeError::InvalidServerWelcome;
    } else if (const auto* frame_batch = std::get_if<FrameBatch>(&message)) {
        valid = valid_frame_header(*frame_batch);
        if (valid && !valid_frame_batch(*frame_batch, effective_limits, &packet_budget)) {
            valid = false;
            packet_limit_failed = true;
        }
        invalid_error = SessionEncodeError::InvalidFrameBatch;
    } else if (const auto* checkpoint = std::get_if<CheckpointTransfer>(&message)) {
        valid = valid_checkpoint_header(*checkpoint);
        if (valid
            && !valid_checkpoint_transfer(*checkpoint, effective_limits, &packet_budget)) {
            valid = false;
            packet_limit_failed = true;
        }
        invalid_error = SessionEncodeError::InvalidCheckpointTransfer;
    } else if (const auto* sync_complete = std::get_if<SyncComplete>(&message)) {
        valid = valid_sync_complete(*sync_complete);
        invalid_error = SessionEncodeError::InvalidSyncComplete;
    } else if (const auto* resume_ack = std::get_if<ResumeAck>(&message)) {
        valid = valid_resume_ack(*resume_ack);
        invalid_error = SessionEncodeError::InvalidResumeAck;
    } else if (const auto* rejection = std::get_if<Reject>(&message)) {
        valid = valid_reject(*rejection);
        invalid_error = SessionEncodeError::InvalidReject;
    } else if (const auto* prepared = std::get_if<FramePrepared>(&message)) {
        valid = valid_frame_acknowledgement(*prepared);
        invalid_error = SessionEncodeError::InvalidFramePrepared;
    } else if (const auto* committed = std::get_if<FrameCommitted>(&message)) {
        valid = valid_frame_acknowledgement(*committed);
        invalid_error = SessionEncodeError::InvalidFrameCommitted;
    } else if (const auto* permit = std::get_if<FrameCommitPermit>(&message)) {
        valid = valid_frame_commit_permit(*permit);
        invalid_error = SessionEncodeError::InvalidFrameCommitPermit;
    } else if (const auto* heartbeat = std::get_if<Heartbeat>(&message)) {
        valid = valid_heartbeat(*heartbeat);
        invalid_error = SessionEncodeError::InvalidHeartbeat;
    } else if (const auto* heartbeat_ack = std::get_if<HeartbeatAck>(&message)) {
        valid = valid_heartbeat_ack(*heartbeat_ack);
        invalid_error = SessionEncodeError::InvalidHeartbeatAck;
    } else if (const auto* goodbye = std::get_if<Goodbye>(&message)) {
        valid = valid_goodbye(*goodbye);
        invalid_error = SessionEncodeError::InvalidGoodbye;
    }
    if (!valid) {
        return core::Result<std::vector<std::byte>, SessionEncodeError>::failure(
            packet_limit_failed ? SessionEncodeError::SizeLimitExceeded : invalid_error);
    }
    if (!estimated_size_fits(
            packet_budget.count, packet_budget.aggregate_bytes, effective_limits)) {
        return core::Result<std::vector<std::byte>, SessionEncodeError>::failure(
            SessionEncodeError::SizeLimitExceeded);
    }

    flatbuffers::FlatBufferBuilder builder(
        std::min<std::size_t>(1024U, effective_limits.max_message_bytes));
    fb::SessionBody body_type = fb::SessionBody::NONE;
    flatbuffers::Offset<void> body;

    if (const auto* hello = std::get_if<ClientHello>(&message)) {
        const auto show = encode_id(hello->show_id);
        const auto node = encode_id(hello->node_id);
        const auto incarnation = encode_id(hello->incarnation);
        const auto epoch = encode_id(hello->last_director_epoch);
        const auto cursor = encode_cursor(hello->cursor);
        const auto negotiated_limits = encode_limits(hello->limits);
        body_type = fb::SessionBody::ClientHello;
        body = fb::CreateClientHello(
                   builder,
                   hello->min_version,
                   hello->max_version,
                   &show,
                   &node,
                   &incarnation,
                   hello->node_index,
                   &epoch,
                   &cursor,
                   hello->force_checkpoint,
                   &negotiated_limits)
                   .Union();
    } else if (const auto* welcome = std::get_if<ServerWelcome>(&message)) {
        const auto show = encode_id(welcome->show_id);
        const auto session = encode_id(welcome->session_id);
        const auto epoch = encode_id(welcome->director_epoch);
        const auto accepted = encode_cursor(welcome->accepted_from);
        const auto target = encode_cursor(welcome->target);
        const auto negotiated_limits = encode_limits(welcome->limits);
        body_type = fb::SessionBody::ServerWelcome;
        body = fb::CreateServerWelcome(
                   builder,
                   welcome->selected_version,
                   &show,
                   &session,
                   &epoch,
                   static_cast<fb::ResumeMode>(welcome->mode),
                   &accepted,
                   &target,
                   &negotiated_limits)
                   .Union();
    } else if (const auto* frame_batch = std::get_if<FrameBatch>(&message)) {
        std::vector<flatbuffers::Offset<fb::Packet>> packets;
        packets.reserve(frame_batch->packets.size());
        for (const auto& packet : frame_batch->packets) {
            const auto* data = reinterpret_cast<const std::uint8_t*>(packet.bytes.data());
            packets.push_back(fb::CreatePacket(
                builder, builder.CreateVector(data, packet.bytes.size())));
        }
        const auto session = encode_id(frame_batch->session_id);
        const auto before = encode_cursor(frame_batch->before);
        const auto after = encode_cursor(frame_batch->after);
        body_type = fb::SessionBody::FrameBatch;
        body = fb::CreateFrameBatch(
                   builder,
                   &session,
                   frame_batch->sequence,
                   &before,
                   &after,
                   builder.CreateVector(packets))
                   .Union();
    } else if (const auto* checkpoint = std::get_if<CheckpointTransfer>(&message)) {
        const auto session = encode_id(checkpoint->session_id);
        const auto cursor = encode_cursor(checkpoint->cursor);
        const auto* data = reinterpret_cast<const std::uint8_t*>(
            checkpoint->wire_packet.data());
        body_type = fb::SessionBody::CheckpointTransfer;
        body = fb::CreateCheckpointTransfer(
                   builder,
                   &session,
                   checkpoint->sequence,
                   &cursor,
                   builder.CreateVector(data, checkpoint->wire_packet.size()))
                   .Union();
    } else if (const auto* sync_complete = std::get_if<SyncComplete>(&message)) {
        const auto session = encode_id(sync_complete->session_id);
        const auto target = encode_cursor(sync_complete->target);
        body_type = fb::SessionBody::SyncComplete;
        body = fb::CreateSyncComplete(
                   builder, &session, sync_complete->sequence, &target)
                   .Union();
    } else if (const auto* resume_ack = std::get_if<ResumeAck>(&message)) {
        const auto session = encode_id(resume_ack->session_id);
        const auto target = encode_cursor(resume_ack->target);
        body_type = fb::SessionBody::ResumeAck;
        body = fb::CreateResumeAck(
                   builder, &session, resume_ack->sequence, &target)
                   .Union();
    } else if (const auto* rejection = std::get_if<Reject>(&message)) {
        const auto show = encode_id(rejection->show_id);
        const auto epoch = encode_id(rejection->director_epoch);
        const auto tail = encode_cursor(rejection->authoritative_tail);
        body_type = fb::SessionBody::Reject;
        body = fb::CreateReject(
                   builder,
                   static_cast<fb::RejectCode>(rejection->code),
                   &show,
                   &epoch,
                   &tail)
                   .Union();
    } else if (const auto* prepared = std::get_if<FramePrepared>(&message)) {
        const auto session = encode_id(prepared->session_id);
        const auto after = encode_cursor(prepared->after);
        body_type = fb::SessionBody::FramePrepared;
        body = fb::CreateFramePrepared(
                   builder,
                   &session,
                   prepared->sequence,
                   prepared->acknowledged_sequence,
                   &after)
                   .Union();
    } else if (const auto* committed = std::get_if<FrameCommitted>(&message)) {
        const auto session = encode_id(committed->session_id);
        const auto after = encode_cursor(committed->after);
        body_type = fb::SessionBody::FrameCommitted;
        body = fb::CreateFrameCommitted(
                   builder,
                   &session,
                   committed->sequence,
                   committed->acknowledged_sequence,
                   &after)
                   .Union();
    } else if (const auto* permit = std::get_if<FrameCommitPermit>(&message)) {
        const auto session = encode_id(permit->session_id);
        const auto epoch = encode_id(permit->director_epoch);
        const auto before = encode_cursor(permit->before);
        const auto after = encode_cursor(permit->after);
        body_type = fb::SessionBody::FrameCommitPermit;
        body = fb::CreateFrameCommitPermit(
                   builder,
                   &session,
                   &epoch,
                   permit->sequence,
                   permit->acknowledged_sequence,
                   permit->barrier_ordinal,
                   &before,
                   &after)
                   .Union();
    } else if (const auto* heartbeat = std::get_if<Heartbeat>(&message)) {
        const auto session = encode_id(heartbeat->session_id);
        const auto epoch = encode_id(heartbeat->director_epoch);
        const auto released = encode_cursor(heartbeat->released_through);
        body_type = fb::SessionBody::Heartbeat;
        body = fb::CreateHeartbeat(
                   builder,
                   &session,
                   &epoch,
                   heartbeat->sequence,
                   heartbeat->response_timeout_ms,
                   &released)
                   .Union();
    } else if (const auto* heartbeat_ack = std::get_if<HeartbeatAck>(&message)) {
        const auto session = encode_id(heartbeat_ack->session_id);
        const auto epoch = encode_id(heartbeat_ack->director_epoch);
        const auto visible = encode_cursor(heartbeat_ack->visible_through);
        body_type = fb::SessionBody::HeartbeatAck;
        body = fb::CreateHeartbeatAck(
                   builder,
                   &session,
                   &epoch,
                   heartbeat_ack->sequence,
                   heartbeat_ack->acknowledged_sequence,
                   &visible,
                   heartbeat_ack->asset_manifest_digest,
                   heartbeat_ack->asset_manifest_ready)
                   .Union();
    } else if (const auto* goodbye = std::get_if<Goodbye>(&message)) {
        const auto session = encode_id(goodbye->session_id);
        const auto epoch = encode_id(goodbye->director_epoch);
        const auto final_cursor = encode_cursor(goodbye->final_cursor);
        body_type = fb::SessionBody::Goodbye;
        body = fb::CreateGoodbye(
                   builder,
                   &session,
                   &epoch,
                   goodbye->sequence,
                   static_cast<fb::GoodbyeReason>(goodbye->reason),
                   &final_cursor)
                   .Union();
    }

    const auto envelope = fb::CreateSessionEnvelope(
        builder, current_session_format_version, body_type, body);
    fb::FinishSizePrefixedSessionEnvelopeBuffer(builder, envelope);
    if (builder.GetSize() > effective_limits.max_message_bytes) {
        return core::Result<std::vector<std::byte>, SessionEncodeError>::failure(
            SessionEncodeError::SizeLimitExceeded);
    }

    std::vector<std::byte> bytes(builder.GetSize());
    std::memcpy(bytes.data(), builder.GetBufferPointer(), builder.GetSize());
    return core::Result<std::vector<std::byte>, SessionEncodeError>::success(
        std::move(bytes));
}

core::Result<SessionMessage, SessionDecodeError> decode_session(
    std::span<const std::byte> bytes,
    const SessionDecodeLimits& limits) {
    constexpr std::size_t size_prefix_bytes = sizeof(flatbuffers::uoffset_t);
    const auto flatbuffer_limit = static_cast<std::size_t>(FLATBUFFERS_MAX_BUFFER_SIZE);
    if (bytes.size() < size_prefix_bytes || bytes.size() > limits.max_message_bytes
        || bytes.size() >= flatbuffer_limit) {
        return core::Result<SessionMessage, SessionDecodeError>::failure(
            bytes.size() > limits.max_message_bytes || bytes.size() >= flatbuffer_limit
                ? SessionDecodeError::SizeLimitExceeded
                : SessionDecodeError::InvalidBuffer);
    }

    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const auto payload_bytes = static_cast<std::size_t>(flatbuffers::GetPrefixedSize(data));
    if (payload_bytes > bytes.size() - size_prefix_bytes
        || payload_bytes + size_prefix_bytes != bytes.size()) {
        return core::Result<SessionMessage, SessionDecodeError>::failure(
            SessionDecodeError::InvalidBuffer);
    }

    flatbuffers::Verifier::Options options;
    options.max_depth = verifier_limit(limits.verifier_max_depth);
    options.max_tables = verifier_limit(limits.verifier_max_tables);
    const auto configured_verifier_size =
        limits.max_message_bytes == std::numeric_limits<std::size_t>::max()
        ? limits.max_message_bytes
        : limits.max_message_bytes + 1U;
    options.max_size = std::min(configured_verifier_size, flatbuffer_limit);
    flatbuffers::Verifier verifier(data, bytes.size(), options);
    if (!fb::VerifySizePrefixedSessionEnvelopeBuffer(verifier)) {
        return core::Result<SessionMessage, SessionDecodeError>::failure(
            SessionDecodeError::InvalidBuffer);
    }

    const auto* envelope = fb::GetSizePrefixedSessionEnvelope(data);
    if (envelope == nullptr) {
        return core::Result<SessionMessage, SessionDecodeError>::failure(
            SessionDecodeError::InvalidBuffer);
    }
    if (envelope->format_version() != current_session_format_version) {
        return core::Result<SessionMessage, SessionDecodeError>::failure(
            SessionDecodeError::UnsupportedVersion);
    }
    if (envelope->body() == nullptr || envelope->body_type() == fb::SessionBody::NONE) {
        return core::Result<SessionMessage, SessionDecodeError>::failure(
            SessionDecodeError::MissingBody);
    }

    switch (envelope->body_type()) {
    case fb::SessionBody::ClientHello: {
        const auto* source = envelope->body_as_ClientHello();
        if (source == nullptr || source->show_id() == nullptr || source->node_id() == nullptr
            || source->incarnation() == nullptr || source->last_director_epoch() == nullptr
            || source->cursor() == nullptr || source->limits() == nullptr) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::MissingRequiredField);
        }
        ClientHello value{
            source->min_version(),
            source->max_version(),
            decode_id(*source->show_id()),
            decode_id(*source->node_id()),
            decode_id(*source->incarnation()),
            source->node_index(),
            decode_id(*source->last_director_epoch()),
            decode_cursor(*source->cursor()),
            source->force_checkpoint(),
            decode_limits(*source->limits()),
        };
        if (!valid_client_hello(value)) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::InvalidClientHello);
        }
        return core::Result<SessionMessage, SessionDecodeError>::success(
            SessionMessage{std::move(value)});
    }
    case fb::SessionBody::ServerWelcome: {
        const auto* source = envelope->body_as_ServerWelcome();
        if (source == nullptr || source->show_id() == nullptr || source->session_id() == nullptr
            || source->director_epoch() == nullptr || source->accepted_from() == nullptr
            || source->target() == nullptr || source->limits() == nullptr) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::MissingRequiredField);
        }
        ServerWelcome value{
            source->selected_version(),
            decode_id(*source->show_id()),
            decode_id(*source->session_id()),
            decode_id(*source->director_epoch()),
            static_cast<ResumeMode>(source->mode()),
            decode_cursor(*source->accepted_from()),
            decode_cursor(*source->target()),
            decode_limits(*source->limits()),
        };
        if (!valid_server_welcome(value)) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::InvalidServerWelcome);
        }
        return core::Result<SessionMessage, SessionDecodeError>::success(
            SessionMessage{std::move(value)});
    }
    case fb::SessionBody::FrameBatch: {
        const auto* source = envelope->body_as_FrameBatch();
        if (source == nullptr || source->session_id() == nullptr || source->before() == nullptr
            || source->after() == nullptr || source->packets() == nullptr) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::MissingRequiredField);
        }
        const auto count = static_cast<std::size_t>(source->packets()->size());
        if (count > limits.max_packets_per_batch) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::SizeLimitExceeded);
        }

        FrameBatch value{
            decode_id(*source->session_id()),
            source->sequence(),
            decode_cursor(*source->before()),
            decode_cursor(*source->after()),
            {},
        };
        const auto command_count = value.after.next_command.value()
            >= value.before.next_command.value()
            ? value.after.next_command.value() - value.before.next_command.value()
            : std::numeric_limits<core::u64>::max();
        if (!static_cast<bool>(value.session_id) || value.sequence == 0U
            || !valid_progress(value.before, value.after)
            || value.before.through_frame.value() == std::numeric_limits<core::u64>::max()
            || value.after.through_frame.value() != value.before.through_frame.value() + 1U
            || command_count == std::numeric_limits<core::u64>::max()
            || command_count + 1U != static_cast<core::u64>(count)) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::InvalidFrameBatch);
        }

        PacketBudget budget;
        for (const auto* packet : *source->packets()) {
            if (packet == nullptr || packet->bytes() == nullptr) {
                return core::Result<SessionMessage, SessionDecodeError>::failure(
                    SessionDecodeError::MissingRequiredField);
            }
            if (packet->bytes()->empty()) {
                return core::Result<SessionMessage, SessionDecodeError>::failure(
                    SessionDecodeError::InvalidFrameBatch);
            }
            if (!add_packet_size(packet->bytes()->size(), limits, budget)) {
                return core::Result<SessionMessage, SessionDecodeError>::failure(
                    SessionDecodeError::SizeLimitExceeded);
            }
        }

        value.packets.reserve(count);
        for (const auto* packet : *source->packets()) {
            value.packets.push_back(Packet{copy_bytes(*packet->bytes())});
        }
        return core::Result<SessionMessage, SessionDecodeError>::success(
            SessionMessage{std::move(value)});
    }
    case fb::SessionBody::CheckpointTransfer: {
        const auto* source = envelope->body_as_CheckpointTransfer();
        if (source == nullptr || source->session_id() == nullptr || source->cursor() == nullptr
            || source->wire_packet() == nullptr) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::MissingRequiredField);
        }
        CheckpointTransfer value{
            decode_id(*source->session_id()),
            source->sequence(),
            decode_cursor(*source->cursor()),
            {},
        };
        if (!static_cast<bool>(value.session_id) || value.sequence == 0U
            || !valid_cursor(value.cursor) || source->wire_packet()->empty()) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::InvalidCheckpointTransfer);
        }
        if (source->wire_packet()->size() > limits.max_checkpoint_bytes) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::SizeLimitExceeded);
        }
        value.wire_packet = copy_bytes(*source->wire_packet());
        return core::Result<SessionMessage, SessionDecodeError>::success(
            SessionMessage{std::move(value)});
    }
    case fb::SessionBody::SyncComplete: {
        const auto* source = envelope->body_as_SyncComplete();
        if (source == nullptr || source->session_id() == nullptr || source->target() == nullptr) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::MissingRequiredField);
        }
        SyncComplete value{
            decode_id(*source->session_id()),
            source->sequence(),
            decode_cursor(*source->target()),
        };
        if (!valid_sync_complete(value)) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::InvalidSyncComplete);
        }
        return core::Result<SessionMessage, SessionDecodeError>::success(
            SessionMessage{value});
    }
    case fb::SessionBody::ResumeAck: {
        const auto* source = envelope->body_as_ResumeAck();
        if (source == nullptr || source->session_id() == nullptr || source->target() == nullptr) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::MissingRequiredField);
        }
        ResumeAck value{
            decode_id(*source->session_id()),
            source->sequence(),
            decode_cursor(*source->target()),
        };
        if (!valid_resume_ack(value)) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::InvalidResumeAck);
        }
        return core::Result<SessionMessage, SessionDecodeError>::success(
            SessionMessage{value});
    }
    case fb::SessionBody::Reject: {
        const auto* source = envelope->body_as_Reject();
        if (source == nullptr || source->show_id() == nullptr
            || source->director_epoch() == nullptr || source->authoritative_tail() == nullptr) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::MissingRequiredField);
        }
        Reject value{
            static_cast<RejectCode>(source->code()),
            decode_id(*source->show_id()),
            decode_id(*source->director_epoch()),
            decode_cursor(*source->authoritative_tail()),
        };
        if (!valid_reject(value)) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::InvalidReject);
        }
        return core::Result<SessionMessage, SessionDecodeError>::success(
            SessionMessage{value});
    }
    case fb::SessionBody::FramePrepared: {
        const auto* source = envelope->body_as_FramePrepared();
        if (source == nullptr || source->session_id() == nullptr
            || source->after() == nullptr) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::MissingRequiredField);
        }
        FramePrepared value{
            decode_id(*source->session_id()),
            source->sequence(),
            source->acknowledged_sequence(),
            decode_cursor(*source->after()),
        };
        if (!valid_frame_acknowledgement(value)) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::InvalidFramePrepared);
        }
        return core::Result<SessionMessage, SessionDecodeError>::success(
            SessionMessage{value});
    }
    case fb::SessionBody::FrameCommitted: {
        const auto* source = envelope->body_as_FrameCommitted();
        if (source == nullptr || source->session_id() == nullptr
            || source->after() == nullptr) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::MissingRequiredField);
        }
        FrameCommitted value{
            decode_id(*source->session_id()),
            source->sequence(),
            source->acknowledged_sequence(),
            decode_cursor(*source->after()),
        };
        if (!valid_frame_acknowledgement(value)) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::InvalidFrameCommitted);
        }
        return core::Result<SessionMessage, SessionDecodeError>::success(
            SessionMessage{value});
    }
    case fb::SessionBody::FrameCommitPermit: {
        const auto* source = envelope->body_as_FrameCommitPermit();
        if (source == nullptr || source->session_id() == nullptr
            || source->director_epoch() == nullptr || source->before() == nullptr
            || source->after() == nullptr) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::MissingRequiredField);
        }
        FrameCommitPermit value{
            decode_id(*source->session_id()),
            decode_id(*source->director_epoch()),
            source->sequence(),
            source->acknowledged_sequence(),
            source->barrier_ordinal(),
            decode_cursor(*source->before()),
            decode_cursor(*source->after()),
        };
        if (!valid_frame_commit_permit(value)) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::InvalidFrameCommitPermit);
        }
        return core::Result<SessionMessage, SessionDecodeError>::success(
            SessionMessage{value});
    }
    case fb::SessionBody::Heartbeat: {
        const auto* source = envelope->body_as_Heartbeat();
        if (source == nullptr || source->session_id() == nullptr
            || source->director_epoch() == nullptr
            || source->released_through() == nullptr) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::MissingRequiredField);
        }
        Heartbeat value{
            decode_id(*source->session_id()),
            decode_id(*source->director_epoch()),
            source->sequence(),
            source->response_timeout_ms(),
            decode_cursor(*source->released_through()),
        };
        if (!valid_heartbeat(value)) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::InvalidHeartbeat);
        }
        return core::Result<SessionMessage, SessionDecodeError>::success(
            SessionMessage{value});
    }
    case fb::SessionBody::HeartbeatAck: {
        const auto* source = envelope->body_as_HeartbeatAck();
        if (source == nullptr || source->session_id() == nullptr
            || source->director_epoch() == nullptr
            || source->visible_through() == nullptr) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::MissingRequiredField);
        }
        HeartbeatAck value{
            decode_id(*source->session_id()),
            decode_id(*source->director_epoch()),
            source->sequence(),
            source->acknowledged_sequence(),
            decode_cursor(*source->visible_through()),
            source->asset_manifest_digest(),
            source->asset_manifest_ready(),
        };
        if (!valid_heartbeat_ack(value)) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::InvalidHeartbeatAck);
        }
        return core::Result<SessionMessage, SessionDecodeError>::success(
            SessionMessage{value});
    }
    case fb::SessionBody::Goodbye: {
        const auto* source = envelope->body_as_Goodbye();
        if (source == nullptr || source->session_id() == nullptr
            || source->director_epoch() == nullptr
            || source->final_cursor() == nullptr) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::MissingRequiredField);
        }
        Goodbye value{
            decode_id(*source->session_id()),
            decode_id(*source->director_epoch()),
            source->sequence(),
            static_cast<GoodbyeReason>(source->reason()),
            decode_cursor(*source->final_cursor()),
        };
        if (!valid_goodbye(value)) {
            return core::Result<SessionMessage, SessionDecodeError>::failure(
                SessionDecodeError::InvalidGoodbye);
        }
        return core::Result<SessionMessage, SessionDecodeError>::success(
            SessionMessage{value});
    }
    case fb::SessionBody::NONE:
    default:
        return core::Result<SessionMessage, SessionDecodeError>::failure(
            SessionDecodeError::InvalidUnion);
    }
}

} // namespace mediaserver::net
