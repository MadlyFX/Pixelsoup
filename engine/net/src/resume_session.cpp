#include <mediaserver/net/resume_session.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mediaserver::net {
namespace {

using SessionStatus = core::Result<std::monostate, ResumeSessionError>;

// Upper bound on how long a session that sent its own Goodbye waits for the
// peer's close before releasing the socket. A peer that is shutting down
// closes as soon as it reads the record, so this is a ceiling on misbehaviour
// rather than a cost the normal path pays.
constexpr auto goodbye_drain_timeout = std::chrono::milliseconds{1'000};

struct DecodedFrame final {
    std::vector<cmd::Command> commands;
    cmd::FrameSeal seal;
};

[[nodiscard]] bool same_cursor(
    const cmd::StreamCursor& left,
    const cmd::StreamCursor& right) noexcept {
    return left.through_frame == right.through_frame && left.revision == right.revision
        && left.next_command == right.next_command;
}

[[nodiscard]] bool valid_cursor(const cmd::StreamCursor& cursor) noexcept {
    return static_cast<bool>(cursor.next_command)
        && cursor.through_frame.value() != std::numeric_limits<core::u64>::max();
}

[[nodiscard]] bool fits_size_t(core::u64 value) noexcept {
    if constexpr (sizeof(std::size_t) >= sizeof(core::u64)) {
        return true;
    } else {
        return value <= static_cast<core::u64>(
                            std::numeric_limits<std::size_t>::max());
    }
}

[[nodiscard]] bool valid_limits(
    const SessionLimits& limits,
    const SessionDecodeLimits& codec_limits,
    const wire::DecodeLimits& wire_limits) noexcept {
    return limits.max_packet_bytes != 0U && limits.max_frame_bytes != 0U
        && limits.max_checkpoint_bytes != 0U && limits.max_inflight_frames != 0U
        && limits.max_inflight_bytes != 0U
        && limits.max_packet_bytes <= limits.max_frame_bytes
        && limits.max_frame_bytes <= limits.max_inflight_bytes
        && limits.max_checkpoint_bytes <= limits.max_inflight_bytes
        && fits_size_t(limits.max_packet_bytes)
        && fits_size_t(limits.max_frame_bytes)
        && fits_size_t(limits.max_checkpoint_bytes)
        && limits.max_packet_bytes <= codec_limits.max_packet_bytes
        && limits.max_frame_bytes <= codec_limits.max_aggregate_packet_bytes
        && limits.max_checkpoint_bytes <= codec_limits.max_checkpoint_bytes
        && limits.max_packet_bytes <= wire_limits.max_message_bytes
        && limits.max_checkpoint_bytes <= wire_limits.max_message_bytes;
}

[[nodiscard]] bool valid_io_policy(
    const SessionIoPolicy& policy,
    const SessionDecodeLimits& codec_limits) noexcept {
    const auto zero = std::chrono::milliseconds::zero();
    return policy.handshake_timeout >= zero && policy.read_timeout >= zero
        && policy.write_timeout >= zero
        && (!policy.admission_budget
            || policy.admission_budget->capacity_bytes()
                >= codec_limits.max_message_bytes);
}

[[nodiscard]] FramedStreamLimits framed_stream_limits(
    const SessionDecodeLimits& codec_limits,
    const SessionIoPolicy& policy) noexcept {
    return FramedStreamLimits{
        codec_limits.max_message_bytes,
        policy.handshake_timeout,
        policy.read_timeout,
        policy.write_timeout,
    };
}

[[nodiscard]] SessionLimits negotiate_limits(
    const SessionLimits& local,
    const SessionLimits& peer) noexcept {
    return SessionLimits{
        std::min(local.max_packet_bytes, peer.max_packet_bytes),
        std::min(local.max_frame_bytes, peer.max_frame_bytes),
        std::min(local.max_inflight_frames, peer.max_inflight_frames),
        std::min(local.max_inflight_bytes, peer.max_inflight_bytes),
        std::min(local.max_checkpoint_bytes, peer.max_checkpoint_bytes),
    };
}

[[nodiscard]] bool limits_at_most(
    const SessionLimits& candidate,
    const SessionLimits& maximum) noexcept {
    return candidate.max_packet_bytes <= maximum.max_packet_bytes
        && candidate.max_frame_bytes <= maximum.max_frame_bytes
        && candidate.max_checkpoint_bytes <= maximum.max_checkpoint_bytes
        && candidate.max_inflight_frames <= maximum.max_inflight_frames
        && candidate.max_inflight_bytes <= maximum.max_inflight_bytes;
}

[[nodiscard]] ResumeSessionError protocol_failure(
    SessionProtocolErrorCode code,
    std::optional<RejectCode> reject_code = std::nullopt,
    std::optional<cmd::ReceiveStatus> receive_status = std::nullopt,
    std::optional<cmd::CheckpointPrepareError> checkpoint_error = std::nullopt) {
    return ResumeSessionError{SessionProtocolFailure{
        code,
        reject_code,
        receive_status,
        checkpoint_error,
    }};
}

[[nodiscard]] SessionStatus send_message(
    TcpFramedStream& stream,
    const SessionMessage& message,
    const SessionDecodeLimits& limits) {
    try {
        auto encoded = encode_session(message, limits);
        if (!encoded) {
            return SessionStatus::failure(
                ResumeSessionError{SessionEncodeFailure{encoded.error()}});
        }
        auto sent = stream.send(encoded.value());
        if (!sent) {
            return SessionStatus::failure(ResumeSessionError{sent.error()});
        }
        return SessionStatus::success(std::monostate{});
    } catch (const std::bad_alloc&) {
        return SessionStatus::failure(
            protocol_failure(SessionProtocolErrorCode::AllocationFailed));
    } catch (const std::length_error&) {
        return SessionStatus::failure(
            protocol_failure(SessionProtocolErrorCode::AllocationFailed));
    }
}

[[nodiscard]] core::Result<SessionMessage, ResumeSessionError> receive_message(
    TcpFramedStream& stream,
    const SessionDecodeLimits& limits,
    std::size_t* encoded_bytes = nullptr) {
    using Result = core::Result<SessionMessage, ResumeSessionError>;
    auto received = stream.receive();
    if (!received) {
        return Result::failure(ResumeSessionError{received.error()});
    }
    if (encoded_bytes != nullptr) {
        *encoded_bytes = received.value().size();
    }
    try {
        auto decoded = decode_session(received.value(), limits);
        if (!decoded) {
            return Result::failure(
                ResumeSessionError{SessionDecodeFailure{decoded.error()}});
        }
        return Result::success(std::move(decoded).value());
    } catch (const std::bad_alloc&) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::AllocationFailed));
    } catch (const std::length_error&) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::AllocationFailed));
    }
}

[[nodiscard]] bool is_replay_capacity_error(
    const ResumeSessionError& error) noexcept {
    if (const auto* wire_failure = std::get_if<WireEncodeFailure>(&error)) {
        return wire_failure->error == wire::EncodeError::SizeLimitExceeded;
    }
    if (const auto* session_failure = std::get_if<SessionEncodeFailure>(&error)) {
        return session_failure->error == SessionEncodeError::SizeLimitExceeded;
    }
    const auto* protocol = std::get_if<SessionProtocolFailure>(&error);
    return protocol != nullptr
        && protocol->code == SessionProtocolErrorCode::BatchMismatch;
}

[[nodiscard]] bool validate_batch_identity(
    std::span<const cmd::Command> commands,
    const cmd::FrameSeal& seal,
    const cmd::StreamCursor& before,
    const cmd::StreamCursor& after) noexcept {
    if (!valid_cursor(before) || !valid_cursor(after)
        || before.through_frame.value() == std::numeric_limits<core::u64>::max()
        || after.through_frame.value() != before.through_frame.value() + 1U
        || seal.frame != after.through_frame || seal.base != before.revision
        || seal.result != after.revision || commands.size() != seal.commands.size()) {
        return false;
    }

    auto expected_id = before.next_command.value();
    auto expected_revision = before.revision.value();
    for (std::size_t index = 0U; index < commands.size(); ++index) {
        const auto& command = commands[index];
        const auto& descriptor = seal.commands[index];
        if (expected_id == 0U || expected_id == std::numeric_limits<core::u64>::max()
            || expected_revision == std::numeric_limits<core::u64>::max()
            || command.id.value() != expected_id || descriptor.id != command.id
            || descriptor.fingerprint != cmd::fingerprint(command)
            || command.apply_at != seal.frame
            || command.base.value() != expected_revision
            || command.result.value() != expected_revision + 1U) {
            return false;
        }
        ++expected_id;
        ++expected_revision;
    }
    return expected_id == after.next_command.value()
        && expected_revision == after.revision.value();
}

[[nodiscard]] core::Result<FrameBatch, ResumeSessionError> encode_frame_batch(
    Id128 session_id,
    core::u64 sequence,
    const cmd::StreamCursor& before,
    const cmd::StreamCursor& after,
    std::span<const cmd::Command> commands,
    const cmd::FrameSeal& seal,
    const SessionLimits& negotiated_limits,
    const SessionDecodeLimits& codec_limits,
    const wire::DecodeLimits& configured_wire_limits) {
    using Result = core::Result<FrameBatch, ResumeSessionError>;
    if (!session_id || sequence == 0U
        || !validate_batch_identity(commands, seal, before, after)) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::BatchMismatch));
    }
    if (commands.size() >= codec_limits.max_packets_per_batch) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::BatchMismatch));
    }

    try {
        FrameBatch frame{
            session_id,
            sequence,
            before,
            after,
            {},
        };
        frame.packets.reserve(commands.size() + 1U);
        auto wire_limits = configured_wire_limits;
        wire_limits.max_message_bytes = std::min(
            wire_limits.max_message_bytes,
            static_cast<std::size_t>(negotiated_limits.max_packet_bytes));
        std::size_t aggregate_bytes = 0U;

        const auto append_packet = [&frame,
                                    &aggregate_bytes,
                                    &negotiated_limits,
                                    &codec_limits](
                                       std::vector<std::byte> encoded)
            -> bool {
            const auto packet_bytes = encoded.size();
            const auto frame_limit = static_cast<std::size_t>(
                negotiated_limits.max_frame_bytes);
            if (packet_bytes == 0U
                || packet_bytes > static_cast<std::size_t>(
                                      negotiated_limits.max_packet_bytes)
                || packet_bytes > codec_limits.max_packet_bytes
                || aggregate_bytes > frame_limit
                || packet_bytes > frame_limit - aggregate_bytes
                || aggregate_bytes > codec_limits.max_aggregate_packet_bytes
                || packet_bytes
                    > codec_limits.max_aggregate_packet_bytes - aggregate_bytes) {
                return false;
            }
            aggregate_bytes += packet_bytes;
            frame.packets.push_back(Packet{std::move(encoded)});
            return true;
        };

        for (std::size_t index = 0U; index < commands.size(); ++index) {
            auto encoded = wire::encode(wire::Message{commands[index]}, wire_limits);
            if (!encoded) {
                return Result::failure(ResumeSessionError{
                    WireEncodeFailure{encoded.error(), index}});
            }
            if (!append_packet(std::move(encoded).value())) {
                return Result::failure(
                    protocol_failure(SessionProtocolErrorCode::BatchMismatch));
            }
        }
        auto encoded_seal = wire::encode(wire::Message{seal}, wire_limits);
        if (!encoded_seal) {
            return Result::failure(ResumeSessionError{
                WireEncodeFailure{encoded_seal.error(), commands.size()}});
        }
        if (!append_packet(std::move(encoded_seal).value())) {
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::BatchMismatch));
        }
        return Result::success(std::move(frame));
    } catch (const std::bad_alloc&) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::AllocationFailed));
    } catch (const std::length_error&) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::AllocationFailed));
    }
}

[[nodiscard]] core::Result<DecodedFrame, ResumeSessionError> decode_frame_batch(
    const FrameBatch& frame,
    const SessionLimits& negotiated_limits,
    const SessionDecodeLimits& codec_limits,
    const wire::DecodeLimits& configured_wire_limits) {
    using Result = core::Result<DecodedFrame, ResumeSessionError>;
    if (frame.packets.empty()
        || frame.packets.size() > codec_limits.max_packets_per_batch) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::BatchMismatch));
    }

    try {
        DecodedFrame decoded;
        decoded.commands.reserve(frame.packets.size() - 1U);
        auto wire_limits = configured_wire_limits;
        wire_limits.max_message_bytes = std::min(
            wire_limits.max_message_bytes,
            static_cast<std::size_t>(negotiated_limits.max_packet_bytes));
        std::size_t aggregate_bytes = 0U;
        const auto frame_limit = static_cast<std::size_t>(
            negotiated_limits.max_frame_bytes);

        for (std::size_t index = 0U; index < frame.packets.size(); ++index) {
            const auto& packet = frame.packets[index].bytes;
            if (packet.empty()
                || packet.size() > static_cast<std::size_t>(
                                      negotiated_limits.max_packet_bytes)
                || aggregate_bytes > frame_limit
                || packet.size() > frame_limit - aggregate_bytes) {
                return Result::failure(
                    protocol_failure(SessionProtocolErrorCode::BatchMismatch));
            }
            aggregate_bytes += packet.size();
            auto message = wire::decode(packet, wire_limits);
            if (!message) {
                return Result::failure(ResumeSessionError{
                    WireDecodeFailure{message.error(), index}});
            }

            const auto final_packet = index + 1U == frame.packets.size();
            if (!final_packet) {
                const auto* command = std::get_if<cmd::Command>(&message.value());
                if (command == nullptr) {
                    return Result::failure(
                        protocol_failure(SessionProtocolErrorCode::BatchMismatch));
                }
                decoded.commands.push_back(*command);
                continue;
            }
            const auto* seal = std::get_if<cmd::FrameSeal>(&message.value());
            if (seal == nullptr) {
                return Result::failure(
                    protocol_failure(SessionProtocolErrorCode::BatchMismatch));
            }
            decoded.seal = *seal;
        }

        if (!validate_batch_identity(
                decoded.commands,
                decoded.seal,
                frame.before,
                frame.after)) {
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::BatchMismatch));
        }
        return Result::success(std::move(decoded));
    } catch (const std::bad_alloc&) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::AllocationFailed));
    } catch (const std::length_error&) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::AllocationFailed));
    }
}

[[nodiscard]] core::Result<ReceivedFrameReport, ResumeSessionError> deliver_frame(
    const FrameBatch& outer,
    const DecodedFrame& decoded,
    cmd::NodeCommandBus& node) {
    using Result = core::Result<ReceivedFrameReport, ResumeSessionError>;
    auto received = node.receive_complete_batch(decoded.commands, decoded.seal);
    if (!received) {
        return Result::failure(
            ResumeSessionError{NodeBatchFailure{std::move(received.error())}});
    }
    const auto status = received.value();
    if ((status != cmd::ReceiveStatus::Prepared
         && status != cmd::ReceiveStatus::Duplicate)
        || !node.is_prepared(decoded.seal.frame)) {
        return Result::failure(protocol_failure(
            SessionProtocolErrorCode::NodeRejected,
            std::nullopt,
            status));
    }
    return Result::success(ReceivedFrameReport{
        outer.before,
        outer.after,
        decoded.commands.size(),
    });
}

[[nodiscard]] bool is_cursor_range_error(
    const persist::CommandLogError& error) noexcept {
    const auto* failure = std::get_if<persist::LogFailure>(&error);
    return failure != nullptr
        && (failure->error.code == persist::LogErrorCode::CursorNotFound
            || failure->error.code == persist::LogErrorCode::ChainMismatch);
}

[[nodiscard]] bool cursor_is_ahead(
    const cmd::StreamCursor& cursor,
    const cmd::StreamCursor& tail) noexcept {
    return cursor.through_frame > tail.through_frame
        || (cursor.through_frame == tail.through_frame && !same_cursor(cursor, tail));
}

[[nodiscard]] bool sequence_matches_and_advances(
    core::u64 actual,
    core::u64& expected) noexcept {
    if (actual != expected || expected == std::numeric_limits<core::u64>::max()) {
        return false;
    }
    ++expected;
    return true;
}

[[nodiscard]] bool cursor_can_advance(
    const cmd::StreamCursor& before,
    const cmd::StreamCursor& after) noexcept {
    if (!valid_cursor(before) || !valid_cursor(after)
        || after.through_frame < before.through_frame
        || after.revision < before.revision
        || after.next_command < before.next_command) {
        return false;
    }
    return after.revision.value() - before.revision.value()
        == after.next_command.value() - before.next_command.value();
}

[[nodiscard]] cmd::StreamCursor receipt_cursor(
    const persist::AppendReceipt& receipt) noexcept {
    return cmd::StreamCursor{
        cmd::FrameNumber{receipt.cursor.through_frame},
        doc::Version{receipt.cursor.revision},
        cmd::CommandId{receipt.cursor.next_command},
    };
}

} // namespace

NodeSessionLeaseRegistry::NodeSessionLeaseRegistry(std::size_t max_sessions)
    : max_sessions_(max_sessions) {
    leases_.reserve(max_sessions_);
}

bool NodeSessionLeaseRegistry::claim(
    const NodeSessionIdentity& identity,
    Id128 session_id) noexcept {
    if (!identity.show_id || !identity.node_id || !identity.incarnation
        || !session_id) {
        return false;
    }

    const std::lock_guard lock{mutex_};
    if (leases_.size() >= max_sessions_) {
        return false;
    }
    const auto conflict = std::find_if(
        leases_.begin(),
        leases_.end(),
        [&identity, session_id](const Lease& lease) {
            const auto same_logical_node =
                lease.identity.show_id == identity.show_id
                && lease.identity.node_id == identity.node_id;
            return same_logical_node || lease.session_id == session_id;
        });
    if (conflict != leases_.end()) {
        return false;
    }
    try {
        leases_.push_back(Lease{identity, session_id});
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
    return true;
}

void NodeSessionLeaseRegistry::release(
    const NodeSessionIdentity& identity,
    Id128 session_id) noexcept {
    const std::lock_guard lock{mutex_};
    const auto lease = std::find_if(
        leases_.begin(),
        leases_.end(),
        [&identity, session_id](const Lease& candidate) {
            return candidate.identity == identity
                && candidate.session_id == session_id;
        });
    if (lease != leases_.end()) {
        leases_.erase(lease);
    }
}

std::size_t NodeSessionLeaseRegistry::active_sessions() const noexcept {
    const std::lock_guard lock{mutex_};
    return leases_.size();
}

bool NodeSessionLeaseRegistry::owns(
    const NodeSessionIdentity& identity,
    Id128 session_id) const noexcept {
    const std::lock_guard lock{mutex_};
    return std::any_of(
        leases_.begin(),
        leases_.end(),
        [&identity, session_id](const Lease& lease) {
            return lease.identity == identity && lease.session_id == session_id;
        });
}

DirectorResumeSession::DirectorResumeSession(
    asio::ip::tcp::socket socket,
    DirectorSessionConfig config,
    const persist::DurableDirector& authority,
    NodeSessionLeaseAuthority& lease_authority) noexcept
    : stream_(
          std::move(socket),
          framed_stream_limits(config.codec_limits, config.io),
          config.io.admission_budget),
      config_(std::move(config)),
      authority_(&authority),
      lease_authority_(&lease_authority) {}

DirectorResumeSession::~DirectorResumeSession() {
    close();
}

core::Result<ResumeReport, ResumeSessionError> DirectorResumeSession::resume() {
    return resume_impl(std::nullopt);
}

core::Result<ResumeReport, ResumeSessionError> DirectorResumeSession::resume(
    persist::DurableResumeSource source) {
    return resume_impl(std::move(source));
}

core::Result<ResumeReport, ResumeSessionError>
DirectorResumeSession::resume_impl(
    std::optional<persist::DurableResumeSource> source) {
    using Result = core::Result<ResumeReport, ResumeSessionError>;
    struct LeaseFailureGuard final {
        DirectorResumeSession* session;
        bool armed{};

        ~LeaseFailureGuard() {
            if (armed && session->state() != DirectorSessionState::Live) {
                session->close();
            }
        }
    } lease_guard{this, false};
    if (state_ != DirectorSessionState::AwaitHello) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::WrongState));
    }
    if (source && !authority_->authorizes(*source)) {
        close();
        return Result::failure(protocol_failure(
            SessionProtocolErrorCode::UnauthorizedResumeSource));
    }
    if (!config_.show_id || !config_.director_epoch || !config_.session_id
        || !valid_limits(config_.limits, config_.codec_limits, config_.wire_limits)
        || !valid_io_policy(config_.io, config_.codec_limits)) {
        state_ = DirectorSessionState::Closed;
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::InvalidConfiguration));
    }

    auto received = receive_message(stream_, config_.codec_limits);
    if (!received) {
        state_ = DirectorSessionState::Closed;
        return Result::failure(std::move(received.error()));
    }
    const auto* hello = std::get_if<ClientHello>(&received.value());
    if (hello == nullptr) {
        state_ = DirectorSessionState::Closed;
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::UnexpectedMessage));
    }

    if (!source) {
        auto captured = authority_->capture_resume_source();
        if (!captured) {
            auto error = std::move(captured.error());
            close();
            return Result::failure(
                ResumeSessionError{ResumeSourceFailure{std::move(error)}});
        }
        source.emplace(std::move(captured).value());
    }
    const auto& log = source->log();
    const auto& checkpoint = source->checkpoint();
    // The authoritative tail is the Director checkpoint, not the durable
    // prefix. They are the same cursor when every frame is journalled; when
    // durability is edit-shaped the log covers only what was written, and a
    // cursor outside it resolves to a checkpoint rather than a replay.
    const auto target = checkpoint.cursor;
    const auto reject = [this, &target](
                            RejectCode reject_code,
                            SessionProtocolErrorCode local_code) -> Result {
        auto sent = send_message(
            stream_,
            SessionMessage{Reject{
                reject_code,
                config_.show_id,
                config_.director_epoch,
                target,
            }},
            config_.codec_limits);
        state_ = DirectorSessionState::Closed;
        if (!sent) {
            stream_.close();
            return Result::failure(std::move(sent.error()));
        }
        stream_.close();
        return Result::failure(protocol_failure(local_code, reject_code));
    };

    if (hello->min_version > current_session_format_version
        || hello->max_version < current_session_format_version) {
        return reject(
            RejectCode::UnsupportedVersion,
            SessionProtocolErrorCode::UnsupportedVersion);
    }
    if (hello->show_id != config_.show_id) {
        return reject(
            RejectCode::ShowMismatch,
            SessionProtocolErrorCode::ShowMismatch);
    }
    negotiated_limits_ = negotiate_limits(config_.limits, hello->limits);
    if (!valid_limits(
            negotiated_limits_, config_.codec_limits, config_.wire_limits)) {
        return reject(
            RejectCode::CapacityExceeded,
            SessionProtocolErrorCode::InvalidConfiguration);
    }
    try {
        outstanding_frames_.reserve(static_cast<std::size_t>(
            negotiated_limits_.max_inflight_frames));
    } catch (const std::bad_alloc&) {
        close();
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::AllocationFailed));
    } catch (const std::length_error&) {
        close();
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::AllocationFailed));
    }
    if (cursor_is_ahead(hello->cursor, target)) {
        return reject(
            RejectCode::InvalidCursor,
            SessionProtocolErrorCode::CursorAhead);
    }

    const NodeSessionIdentity identity{
        hello->show_id,
        hello->node_id,
        hello->incarnation,
    };
    if (!lease_authority_->claim(identity, config_.session_id)) {
        return reject(
            RejectCode::NodeLeaseRejected,
            SessionProtocolErrorCode::NodeLeaseRejected);
    }
    {
        const std::lock_guard lock{lease_mutex_};
        peer_identity_ = identity;
        peer_node_index_ = hello->node_index;
    }
    lease_guard.armed = true;

    auto mode = ResumeMode::Checkpoint;
    if (!hello->force_checkpoint
        && hello->last_director_epoch == config_.director_epoch
        && same_cursor(hello->cursor, target)) {
        // Already at the authoritative tail, so there is nothing to replay.
        // Asking the log to confirm that only works when the log covers the
        // tail; under an edit-shaped cadence it does not, and re-sending a
        // checkpoint of state the node already holds is waste, not safety.
        mode = ResumeMode::UpToDate;
    } else if (
        !hello->force_checkpoint
        && hello->last_director_epoch == config_.director_epoch) {
        bool replay_window_fits = true;
        core::u64 replay_bytes = 0U;
        core::u64 replay_sequence = 1U;
        std::optional<ResumeSessionError> callback_error;
        auto preflight = log.visit_batches_after(
            hello->cursor,
            target,
            [this,
             &replay_window_fits,
             &replay_bytes,
             &replay_sequence,
             &callback_error](const persist::ValidatedCommandBatchView& batch) {
                const auto frame_count = replay_sequence - 1U;
                if (frame_count
                    >= static_cast<core::u64>(
                        negotiated_limits_.max_inflight_frames)) {
                    replay_window_fits = false;
                    return persist::BatchVisitControl::Stop;
                }

                auto frame = encode_frame_batch(
                    config_.session_id,
                    replay_sequence,
                    batch.before,
                    batch.after,
                    batch.commands,
                    batch.seal,
                    negotiated_limits_,
                    config_.codec_limits,
                    config_.wire_limits);
                if (!frame) {
                    callback_error = std::move(frame.error());
                    replay_window_fits = false;
                    return persist::BatchVisitControl::Stop;
                }

                try {
                    auto encoded = encode_session(
                        SessionMessage{std::move(frame).value()},
                        config_.codec_limits);
                    if (!encoded) {
                        callback_error = ResumeSessionError{
                            SessionEncodeFailure{encoded.error()}};
                        replay_window_fits = false;
                        return persist::BatchVisitControl::Stop;
                    }
                    const auto encoded_size = static_cast<core::u64>(
                        encoded.value().size());
                    if (replay_bytes > negotiated_limits_.max_inflight_bytes
                        || encoded_size
                            > negotiated_limits_.max_inflight_bytes
                                - replay_bytes) {
                        replay_window_fits = false;
                        return persist::BatchVisitControl::Stop;
                    }
                    replay_bytes += encoded_size;
                    ++replay_sequence;
                    return persist::BatchVisitControl::Continue;
                } catch (const std::bad_alloc&) {
                    callback_error = protocol_failure(
                        SessionProtocolErrorCode::AllocationFailed);
                } catch (const std::length_error&) {
                    callback_error = protocol_failure(
                        SessionProtocolErrorCode::AllocationFailed);
                }
                replay_window_fits = false;
                return persist::BatchVisitControl::Stop;
            });
        if (callback_error && !is_replay_capacity_error(*callback_error)) {
            state_ = DirectorSessionState::Closed;
            return Result::failure(std::move(*callback_error));
        }
        if (preflight && replay_window_fits && preflight.value().completed) {
            mode = same_cursor(hello->cursor, target)
                ? ResumeMode::UpToDate
                : ResumeMode::Replay;
        } else if (!preflight && !is_cursor_range_error(preflight.error())) {
            state_ = DirectorSessionState::Closed;
            return Result::failure(
                ResumeSessionError{std::move(preflight.error())});
        }
    }

    std::optional<std::vector<std::byte>> encoded_checkpoint;
    if (mode == ResumeMode::Checkpoint) {
        if (!same_cursor(checkpoint.cursor, target)
            || checkpoint.document.version() != checkpoint.cursor.revision
            || !valid_cursor(checkpoint.cursor)) {
            return reject(
                RejectCode::ResumeUnavailable,
                SessionProtocolErrorCode::CheckpointMismatch);
        }
        try {
            auto wire_limits = config_.wire_limits;
            wire_limits.max_message_bytes = std::min(
                wire_limits.max_message_bytes,
                static_cast<std::size_t>(
                    negotiated_limits_.max_checkpoint_bytes));
            auto encoded = wire::encode(wire::Message{checkpoint}, wire_limits);
            if (!encoded) {
                if (encoded.error() == wire::EncodeError::SizeLimitExceeded) {
                    return reject(
                        RejectCode::CapacityExceeded,
                        SessionProtocolErrorCode::CheckpointMismatch);
                }
                state_ = DirectorSessionState::Closed;
                return Result::failure(ResumeSessionError{
                    WireEncodeFailure{encoded.error(), 0U}});
            }
            encoded_checkpoint = std::move(encoded).value();
        } catch (const std::bad_alloc&) {
            state_ = DirectorSessionState::Closed;
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::AllocationFailed));
        } catch (const std::length_error&) {
            state_ = DirectorSessionState::Closed;
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::AllocationFailed));
        }
    }

    const auto accepted_from = mode == ResumeMode::Checkpoint
        ? checkpoint.cursor
        : hello->cursor;
    auto welcomed = send_message(
        stream_,
        SessionMessage{ServerWelcome{
            current_session_format_version,
            config_.show_id,
            config_.session_id,
            config_.director_epoch,
            mode,
            accepted_from,
            target,
            negotiated_limits_,
        }},
        config_.codec_limits);
    if (!welcomed) {
        state_ = DirectorSessionState::Closed;
        return Result::failure(std::move(welcomed.error()));
    }

    ResumeReport report{mode, hello->cursor, target, 0U, 0U, false};
    next_sequence_ = 1U;
    sent_cursor_ = accepted_from;
    if (mode == ResumeMode::Replay) {
        std::optional<ResumeSessionError> callback_error;
        auto replayed = log.visit_batches_after(
            hello->cursor,
            target,
            [this, &callback_error](
                const persist::ValidatedCommandBatchView& batch) {
                if (next_sequence_ == std::numeric_limits<core::u64>::max()) {
                    callback_error = protocol_failure(
                        SessionProtocolErrorCode::SequenceMismatch);
                    return persist::BatchVisitControl::Stop;
                }
                auto frame = encode_frame_batch(
                    config_.session_id,
                    next_sequence_,
                    batch.before,
                    batch.after,
                    batch.commands,
                    batch.seal,
                    negotiated_limits_,
                    config_.codec_limits,
                    config_.wire_limits);
                if (!frame) {
                    callback_error = std::move(frame.error());
                    return persist::BatchVisitControl::Stop;
                }
                try {
                    auto encoded = encode_session(
                        SessionMessage{std::move(frame).value()},
                        config_.codec_limits);
                    if (!encoded) {
                        callback_error = ResumeSessionError{
                            SessionEncodeFailure{encoded.error()}};
                        return persist::BatchVisitControl::Stop;
                    }
                    const auto encoded_bytes = static_cast<core::u64>(
                        encoded.value().size());
                    if (outstanding_frames_.size()
                            >= static_cast<std::size_t>(
                                negotiated_limits_.max_inflight_frames)
                        || outstanding_bytes_
                            > negotiated_limits_.max_inflight_bytes
                        || encoded_bytes
                            > negotiated_limits_.max_inflight_bytes
                                - outstanding_bytes_) {
                        callback_error = protocol_failure(
                            SessionProtocolErrorCode::InflightWindowFull);
                        return persist::BatchVisitControl::Stop;
                    }
                    outstanding_frames_.push_back(OutstandingFrame{
                        next_sequence_,
                        batch.before,
                        batch.after,
                        encoded_bytes,
                        false,
                        true,
                        0U,
                    });
                    outstanding_bytes_ += encoded_bytes;
                    auto sent = stream_.send(encoded.value());
                    if (!sent) {
                        outstanding_bytes_ -= encoded_bytes;
                        outstanding_frames_.pop_back();
                        callback_error = ResumeSessionError{sent.error()};
                        return persist::BatchVisitControl::Stop;
                    }
                } catch (const std::bad_alloc&) {
                    callback_error = protocol_failure(
                        SessionProtocolErrorCode::AllocationFailed);
                    return persist::BatchVisitControl::Stop;
                } catch (const std::length_error&) {
                    callback_error = protocol_failure(
                        SessionProtocolErrorCode::AllocationFailed);
                    return persist::BatchVisitControl::Stop;
                }
                sent_cursor_ = batch.after;
                ++next_sequence_;
                return persist::BatchVisitControl::Continue;
            });
        if (callback_error) {
            state_ = DirectorSessionState::Closed;
            return Result::failure(std::move(*callback_error));
        }
        if (!replayed) {
            state_ = DirectorSessionState::Closed;
            return Result::failure(
                ResumeSessionError{std::move(replayed.error())});
        }
        if (!replayed.value().completed || !same_cursor(sent_cursor_, target)) {
            state_ = DirectorSessionState::Closed;
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::ReplayInterrupted));
        }
        report.frames = replayed.value().frames;
        report.commands = replayed.value().commands;
    } else if (mode == ResumeMode::Checkpoint) {
        try {
            if (!encoded_checkpoint) {
                state_ = DirectorSessionState::Closed;
                return Result::failure(protocol_failure(
                    SessionProtocolErrorCode::CheckpointMismatch));
            }
            auto transferred = send_message(
                stream_,
                SessionMessage{CheckpointTransfer{
                    config_.session_id,
                    next_sequence_,
                    checkpoint.cursor,
                    std::move(*encoded_checkpoint),
                }},
                config_.codec_limits);
            if (!transferred) {
                state_ = DirectorSessionState::Closed;
                return Result::failure(std::move(transferred.error()));
            }
            ++next_sequence_;
            sent_cursor_ = checkpoint.cursor;
        } catch (const std::bad_alloc&) {
            state_ = DirectorSessionState::Closed;
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::AllocationFailed));
        } catch (const std::length_error&) {
            state_ = DirectorSessionState::Closed;
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::AllocationFailed));
        }
    } else {
        sent_cursor_ = target;
    }

    if (next_sequence_ == std::numeric_limits<core::u64>::max()) {
        state_ = DirectorSessionState::Closed;
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::SequenceMismatch));
    }
    auto completed = send_message(
        stream_,
        SessionMessage{SyncComplete{
            config_.session_id,
            next_sequence_,
            target,
        }},
        config_.codec_limits);
    if (!completed) {
        state_ = DirectorSessionState::Closed;
        return Result::failure(std::move(completed.error()));
    }
    ++next_sequence_;
    state_ = DirectorSessionState::AwaitResumeAck;

    auto acknowledged = receive_message(stream_, config_.codec_limits);
    if (!acknowledged) {
        state_ = DirectorSessionState::Closed;
        return Result::failure(std::move(acknowledged.error()));
    }
    const auto* ack = std::get_if<ResumeAck>(&acknowledged.value());
    if (ack == nullptr) {
        state_ = DirectorSessionState::Closed;
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::UnexpectedMessage));
    }
    if (ack->session_id != config_.session_id) {
        state_ = DirectorSessionState::Closed;
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::SessionMismatch));
    }
    if (ack->sequence != 1U) {
        state_ = DirectorSessionState::Closed;
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::SequenceMismatch));
    }
    if (!same_cursor(ack->target, target)) {
        state_ = DirectorSessionState::Closed;
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::CursorMismatch));
    }

    for (auto& frame : outstanding_frames_) {
        frame.prepared = true;
    }
    expected_ack_sequence_ = 2U;
    prepared_cursor_ = target;
    committed_cursor_ = mode == ResumeMode::Replay ? hello->cursor : target;
    sent_cursor_ = target;
    state_ = DirectorSessionState::Live;
    return Result::success(report);
}

core::Result<std::monostate, ResumeSessionError>
DirectorResumeSession::send_committed(
    const persist::CommittedFrameBatch& batch) {
    const std::lock_guard send_lock{send_mutex_};
    if (state() != DirectorSessionState::Live) {
        return SessionStatus::failure(
            protocol_failure(SessionProtocolErrorCode::WrongState));
    }
    if (!batch.receipt().durable
        && authority_->frame_durability()
            == persist::FrameDurability::EveryFrame) {
        return SessionStatus::failure(
            protocol_failure(SessionProtocolErrorCode::NonDurableBatch));
    }
    if (!authority_->authorizes(batch)) {
        return SessionStatus::failure(
            protocol_failure(SessionProtocolErrorCode::UnauthorizedBatch));
    }
    if (next_sequence_ == std::numeric_limits<core::u64>::max()) {
        return SessionStatus::failure(
            protocol_failure(SessionProtocolErrorCode::SequenceMismatch));
    }

    const auto after = receipt_cursor(batch.receipt());
    if (!same_cursor(batch.before_cursor(), sent_cursor_)) {
        return SessionStatus::failure(
            protocol_failure(SessionProtocolErrorCode::CursorMismatch));
    }
    auto frame = encode_frame_batch(
        config_.session_id,
        next_sequence_,
        sent_cursor_,
        after,
        batch.commands(),
        batch.seal(),
        negotiated_limits_,
        config_.codec_limits,
        config_.wire_limits);
    if (!frame) {
        return SessionStatus::failure(std::move(frame.error()));
    }

    try {
        auto encoded = encode_session(
            SessionMessage{std::move(frame).value()}, config_.codec_limits);
        if (!encoded) {
            return SessionStatus::failure(
                ResumeSessionError{SessionEncodeFailure{encoded.error()}});
        }
        const auto encoded_bytes = static_cast<core::u64>(
            encoded.value().size());
        {
            const std::lock_guard acknowledgement_lock{acknowledgement_mutex_};
            if (outstanding_frames_.size()
                    >= static_cast<std::size_t>(
                        negotiated_limits_.max_inflight_frames)
                || outstanding_bytes_ > negotiated_limits_.max_inflight_bytes
                || encoded_bytes
                    > negotiated_limits_.max_inflight_bytes
                        - outstanding_bytes_) {
                return SessionStatus::failure(protocol_failure(
                    SessionProtocolErrorCode::InflightWindowFull));
            }
            outstanding_frames_.push_back(OutstandingFrame{
                next_sequence_,
                sent_cursor_,
                after,
                encoded_bytes,
                false,
                false,
                0U,
            });
            outstanding_bytes_ += encoded_bytes;
        }

        auto sent = stream_.send(encoded.value());
        if (!sent) {
            {
                const std::lock_guard acknowledgement_lock{
                    acknowledgement_mutex_};
                const auto pending = std::find_if(
                    outstanding_frames_.begin(),
                    outstanding_frames_.end(),
                    [this](const OutstandingFrame& candidate) {
                        return candidate.sequence == next_sequence_;
                    });
                if (pending != outstanding_frames_.end()) {
                    outstanding_bytes_ -= pending->encoded_bytes;
                    outstanding_frames_.erase(pending);
                }
            }
            close();
            return SessionStatus::failure(ResumeSessionError{sent.error()});
        }
    } catch (const std::bad_alloc&) {
        return SessionStatus::failure(
            protocol_failure(SessionProtocolErrorCode::AllocationFailed));
    } catch (const std::length_error&) {
        return SessionStatus::failure(
            protocol_failure(SessionProtocolErrorCode::AllocationFailed));
    }
    sent_cursor_ = after;
    ++next_sequence_;
    return SessionStatus::success(std::monostate{});
}

core::Result<FrameCommitPermitReport, ResumeSessionError>
DirectorResumeSession::send_commit_permit(
    const FrameCommitPermitIntent& intent) {
    using Result = core::Result<
        FrameCommitPermitReport,
        ResumeSessionError>;
    const std::lock_guard send_lock{send_mutex_};
    if (state() != DirectorSessionState::Live) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::WrongState));
    }
    if (intent.barrier_ordinal == 0U) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::InvalidCommitPermit));
    }
    if (next_sequence_ == std::numeric_limits<core::u64>::max()) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::SequenceMismatch));
    }

    const std::lock_guard acknowledgement_lock{acknowledgement_mutex_};
    if (intent.barrier_ordinal <= last_commit_permit_ordinal_) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::InvalidCommitPermit));
    }
    const auto pending = std::find_if(
        outstanding_frames_.begin(),
        outstanding_frames_.end(),
        [](const OutstandingFrame& candidate) {
            return !candidate.permitted;
        });
    if (pending == outstanding_frames_.end()) {
        return Result::failure(protocol_failure(
            SessionProtocolErrorCode::AcknowledgementMismatch));
    }
    if (!pending->prepared) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::FrameNotPrepared));
    }
    if (pending->sequence != intent.acknowledged_sequence) {
        return Result::failure(protocol_failure(
            SessionProtocolErrorCode::AcknowledgementMismatch));
    }
    if (!same_cursor(pending->before, intent.before)
        || !same_cursor(pending->after, intent.after)) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::CursorMismatch));
    }

    const FrameCommitPermit permit{
        config_.session_id,
        config_.director_epoch,
        next_sequence_,
        pending->sequence,
        intent.barrier_ordinal,
        pending->before,
        pending->after,
    };
    auto sent = send_message(
        stream_, SessionMessage{permit}, config_.codec_limits);
    if (!sent) {
        close();
        return Result::failure(std::move(sent.error()));
    }

    pending->permitted = true;
    pending->barrier_ordinal = intent.barrier_ordinal;
    last_commit_permit_ordinal_ = intent.barrier_ordinal;
    ++next_sequence_;
    const auto permitted_count = static_cast<std::size_t>(std::count_if(
        outstanding_frames_.begin(),
        outstanding_frames_.end(),
        [](const OutstandingFrame& candidate) {
            return candidate.permitted;
        }));
    return Result::success(FrameCommitPermitReport{
        permit.sequence,
        permit.acknowledged_sequence,
        permit.barrier_ordinal,
        permit.before,
        permit.after,
        permitted_count,
    });
}

core::Result<HeartbeatSendReport, ResumeSessionError>
DirectorResumeSession::send_heartbeat(
    const cmd::StreamCursor& released_through,
    std::chrono::milliseconds response_timeout) {
    using Result = core::Result<HeartbeatSendReport, ResumeSessionError>;
    const std::lock_guard send_lock{send_mutex_};
    const auto timeout_count = response_timeout.count();
    if (state() != DirectorSessionState::Live) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::WrongState));
    }
    if (!valid_cursor(released_through)
        || timeout_count
            < static_cast<std::chrono::milliseconds::rep>(
                min_heartbeat_response_timeout_ms)
        || timeout_count
            > static_cast<std::chrono::milliseconds::rep>(
                max_heartbeat_response_timeout_ms)
        || next_sequence_ == std::numeric_limits<core::u64>::max()) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::InvalidConfiguration));
    }

    const auto sequence = next_sequence_;
    const Heartbeat heartbeat{
        config_.session_id,
        config_.director_epoch,
        sequence,
        static_cast<core::u32>(timeout_count),
        released_through,
    };
    std::chrono::steady_clock::time_point sent_at{};
    {
        // Fence a very fast peer acknowledgement until the complete probe is
        // on the wire and its exact sequence has become outstanding.
        const std::lock_guard acknowledgement_lock{acknowledgement_mutex_};
        if (outstanding_heartbeat_sequence_) {
            return Result::failure(protocol_failure(
                SessionProtocolErrorCode::AcknowledgementMismatch));
        }
        outstanding_heartbeat_sequence_ = sequence;
        auto sent = send_message(
            stream_, SessionMessage{heartbeat}, config_.codec_limits);
        if (!sent) {
            outstanding_heartbeat_sequence_.reset();
            close();
            return Result::failure(std::move(sent.error()));
        }
        sent_at = std::chrono::steady_clock::now();
    }
    ++next_sequence_;
    return Result::success(HeartbeatSendReport{
        sequence,
        heartbeat.response_timeout_ms,
        released_through,
        sent_at,
    });
}

core::Result<GoodbyeReport, ResumeSessionError>
DirectorResumeSession::send_goodbye(
    GoodbyeReason reason,
    const cmd::StreamCursor& final_cursor) {
    using Result = core::Result<GoodbyeReport, ResumeSessionError>;
    const std::lock_guard send_lock{send_mutex_};
    if (state() != DirectorSessionState::Live) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::WrongState));
    }
    if (reason == GoodbyeReason::None || !valid_cursor(final_cursor)
        || next_sequence_ == std::numeric_limits<core::u64>::max()) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::InvalidConfiguration));
    }

    const Goodbye goodbye{
        config_.session_id,
        config_.director_epoch,
        next_sequence_,
        reason,
        final_cursor,
    };
    auto sent = send_message(
        stream_, SessionMessage{goodbye}, config_.codec_limits);
    if (!sent) {
        close();
        return Result::failure(std::move(sent.error()));
    }
    ++next_sequence_;
    state_ = DirectorSessionState::Draining;
    return Result::success(GoodbyeReport{
        goodbye.sequence,
        goodbye.reason,
        goodbye.final_cursor,
    });
}

core::Result<FrameAcknowledgementReport, ResumeSessionError>
DirectorResumeSession::receive_frame_acknowledgement() {
    using Result = core::Result<
        FrameAcknowledgementReport,
        ResumeSessionError>;
    auto received = receive_live_message();
    if (!received) {
        return Result::failure(std::move(received.error()));
    }
    const auto* acknowledgement =
        std::get_if<FrameAcknowledgementReport>(&received.value());
    if (acknowledgement == nullptr) {
        close();
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::UnexpectedMessage));
    }
    return Result::success(*acknowledgement);
}

core::Result<DirectorLiveInboundReport, ResumeSessionError>
DirectorResumeSession::receive_live_message() {
    using Result = core::Result<
        DirectorLiveInboundReport,
        ResumeSessionError>;
    if (state() != DirectorSessionState::Live) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::WrongState));
    }

    auto received = receive_message(stream_, config_.codec_limits);
    if (!received) {
        close();
        return Result::failure(std::move(received.error()));
    }

    if (const auto* heartbeat_ack =
            std::get_if<HeartbeatAck>(&received.value())) {
        std::optional<SessionProtocolErrorCode> failure;
        HeartbeatAcknowledgementReport report{};
        {
            const std::lock_guard acknowledgement_lock{
                acknowledgement_mutex_};
            if (heartbeat_ack->session_id != config_.session_id) {
                failure = SessionProtocolErrorCode::SessionMismatch;
            } else if (heartbeat_ack->director_epoch
                       != config_.director_epoch) {
                failure = SessionProtocolErrorCode::EpochMismatch;
            } else if (expected_ack_sequence_
                           == std::numeric_limits<core::u64>::max()
                       || heartbeat_ack->sequence
                           != expected_ack_sequence_) {
                failure = SessionProtocolErrorCode::SequenceMismatch;
            } else if (!outstanding_heartbeat_sequence_
                       || heartbeat_ack->acknowledged_sequence
                           != *outstanding_heartbeat_sequence_) {
                failure = SessionProtocolErrorCode::AcknowledgementMismatch;
            } else if (!same_cursor(
                           heartbeat_ack->visible_through,
                           committed_cursor_)) {
                failure = SessionProtocolErrorCode::CursorMismatch;
            } else {
                report = HeartbeatAcknowledgementReport{
                    heartbeat_ack->sequence,
                    heartbeat_ack->acknowledged_sequence,
                    heartbeat_ack->visible_through,
                    RuntimeAssetAdoption{
                        heartbeat_ack->asset_manifest_digest,
                        heartbeat_ack->asset_manifest_ready,
                    },
                    heartbeat_ack->output_sync,
                };
                outstanding_heartbeat_sequence_.reset();
                ++expected_ack_sequence_;
            }
        }
        if (failure) {
            close();
            return Result::failure(protocol_failure(*failure));
        }
        return Result::success(DirectorLiveInboundReport{report});
    }

    if (const auto* goodbye = std::get_if<Goodbye>(&received.value())) {
        std::optional<SessionProtocolErrorCode> failure;
        {
            const std::lock_guard acknowledgement_lock{
                acknowledgement_mutex_};
            if (goodbye->session_id != config_.session_id) {
                failure = SessionProtocolErrorCode::SessionMismatch;
            } else if (goodbye->director_epoch != config_.director_epoch) {
                failure = SessionProtocolErrorCode::EpochMismatch;
            } else if (expected_ack_sequence_
                           == std::numeric_limits<core::u64>::max()
                       || goodbye->sequence != expected_ack_sequence_) {
                failure = SessionProtocolErrorCode::SequenceMismatch;
            } else if (!same_cursor(
                           goodbye->final_cursor,
                           committed_cursor_)) {
                failure = SessionProtocolErrorCode::CursorMismatch;
            } else {
                ++expected_ack_sequence_;
                state_ = DirectorSessionState::Draining;
            }
        }
        if (failure) {
            close();
            return Result::failure(protocol_failure(*failure));
        }
        return Result::success(DirectorLiveInboundReport{GoodbyeReport{
            goodbye->sequence,
            goodbye->reason,
            goodbye->final_cursor,
        }});
    }

    FrameAcknowledgementKind kind{};
    Id128 session_id{};
    core::u64 sequence = 0U;
    core::u64 acknowledged_sequence = 0U;
    cmd::StreamCursor after{};
    if (const auto* prepared =
            std::get_if<FramePrepared>(&received.value())) {
        kind = FrameAcknowledgementKind::Prepared;
        session_id = prepared->session_id;
        sequence = prepared->sequence;
        acknowledged_sequence = prepared->acknowledged_sequence;
        after = prepared->after;
    } else if (const auto* committed =
                   std::get_if<FrameCommitted>(&received.value())) {
        kind = FrameAcknowledgementKind::Committed;
        session_id = committed->session_id;
        sequence = committed->sequence;
        acknowledged_sequence = committed->acknowledged_sequence;
        after = committed->after;
    } else {
        close();
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::UnexpectedMessage));
    }

    std::optional<SessionProtocolErrorCode> failure;
    FrameAcknowledgementReport report{};
    cmd::StreamCursor acknowledged_before{};
    {
        const std::lock_guard acknowledgement_lock{acknowledgement_mutex_};
        if (session_id != config_.session_id) {
            failure = SessionProtocolErrorCode::SessionMismatch;
        } else if (expected_ack_sequence_
                       == std::numeric_limits<core::u64>::max()
                   || sequence != expected_ack_sequence_) {
            failure = SessionProtocolErrorCode::SequenceMismatch;
        } else {
            const auto pending = std::find_if(
                outstanding_frames_.begin(),
                outstanding_frames_.end(),
                [acknowledged_sequence](const OutstandingFrame& candidate) {
                    return candidate.sequence == acknowledged_sequence;
                });
            if (pending == outstanding_frames_.end()) {
                failure = SessionProtocolErrorCode::AcknowledgementMismatch;
            } else if (!same_cursor(pending->after, after)) {
                failure = SessionProtocolErrorCode::CursorMismatch;
            } else {
                acknowledged_before = pending->before;
            }
            if (!failure && kind == FrameAcknowledgementKind::Prepared) {
                const auto next_unprepared = std::find_if(
                    outstanding_frames_.begin(),
                    outstanding_frames_.end(),
                    [](const OutstandingFrame& candidate) {
                        return !candidate.prepared;
                    });
                if (pending != next_unprepared || pending->prepared) {
                    failure =
                        SessionProtocolErrorCode::AcknowledgementMismatch;
                } else {
                    pending->prepared = true;
                    prepared_cursor_ = after;
                }
            } else if (!failure && pending != outstanding_frames_.begin()) {
                failure = SessionProtocolErrorCode::AcknowledgementMismatch;
            } else if (!failure && !pending->prepared) {
                failure = SessionProtocolErrorCode::FrameNotPrepared;
            } else if (!failure && !pending->permitted) {
                failure = SessionProtocolErrorCode::FrameNotPermitted;
            } else if (!failure) {
                outstanding_bytes_ -= pending->encoded_bytes;
                committed_cursor_ = after;
                outstanding_frames_.erase(pending);
            }
        }

        if (!failure) {
            ++expected_ack_sequence_;
            report = FrameAcknowledgementReport{
                kind,
                sequence,
                acknowledged_sequence,
                acknowledged_before,
                after,
                outstanding_frames_.size(),
                outstanding_bytes_,
            };
        }
    }

    if (failure) {
        close();
        return Result::failure(protocol_failure(*failure));
    }
    return Result::success(DirectorLiveInboundReport{report});
}

cmd::StreamCursor DirectorResumeSession::sent_cursor() const noexcept {
    const std::lock_guard lock{send_mutex_};
    return sent_cursor_;
}

cmd::StreamCursor DirectorResumeSession::prepared_cursor() const noexcept {
    const std::lock_guard lock{acknowledgement_mutex_};
    return prepared_cursor_;
}

cmd::StreamCursor DirectorResumeSession::committed_cursor() const noexcept {
    const std::lock_guard lock{acknowledgement_mutex_};
    return committed_cursor_;
}

std::optional<NodeSessionIdentity>
DirectorResumeSession::peer_identity() const noexcept {
    const std::lock_guard lock{lease_mutex_};
    return peer_identity_;
}

std::optional<core::u32>
DirectorResumeSession::peer_node_index() const noexcept {
    const std::lock_guard lock{lease_mutex_};
    return peer_node_index_;
}

std::size_t DirectorResumeSession::outstanding_frames() const noexcept {
    const std::lock_guard lock{acknowledgement_mutex_};
    return outstanding_frames_.size();
}

std::size_t DirectorResumeSession::permitted_frames() const noexcept {
    const std::lock_guard lock{acknowledgement_mutex_};
    return static_cast<std::size_t>(std::count_if(
        outstanding_frames_.begin(),
        outstanding_frames_.end(),
        [](const OutstandingFrame& candidate) {
            return candidate.permitted;
        }));
}

core::u64 DirectorResumeSession::last_commit_permit_ordinal() const noexcept {
    const std::lock_guard lock{acknowledgement_mutex_};
    return last_commit_permit_ordinal_;
}

core::u64 DirectorResumeSession::last_node_control_sequence() const noexcept {
    const std::lock_guard lock{acknowledgement_mutex_};
    return expected_ack_sequence_ == 0U
        ? 0U
        : expected_ack_sequence_ - 1U;
}

void DirectorResumeSession::close() noexcept {
    state_ = DirectorSessionState::Closed;
    stream_.close();
    release_lease();
}

void DirectorResumeSession::release_lease() noexcept {
    const std::lock_guard lock{lease_mutex_};
    if (!peer_identity_) {
        return;
    }
    lease_authority_->release(*peer_identity_, config_.session_id);
    peer_identity_.reset();
    peer_node_index_.reset();
}

NodeResumeSession::NodeResumeSession(
    asio::ip::tcp::socket socket,
    cmd::NodeCommandBus& node,
    NodeSessionConfig config) noexcept
    : stream_(
          std::move(socket),
          framed_stream_limits(config.codec_limits, config.io),
          config.io.admission_budget),
      node_(&node),
      config_(std::move(config)),
      delivered_cursor_(node.cursor()),
      target_cursor_(node.cursor()) {}

NodeResumeSession::~NodeResumeSession() {
    close();
}

core::Result<ResumeReport, ResumeSessionError> NodeResumeSession::resume() {
    using Result = core::Result<ResumeReport, ResumeSessionError>;
    if (state_ != NodeSessionState::Disconnected) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::WrongState));
    }
    if (!config_.show_id || !config_.node_id || !config_.incarnation
        || config_.node_index > max_session_node_index
        || !valid_cursor(node_->cursor())
        || !valid_limits(config_.limits, config_.codec_limits, config_.wire_limits)
        || !valid_io_policy(config_.io, config_.codec_limits)) {
        state_ = NodeSessionState::Closed;
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::InvalidConfiguration));
    }

    const auto requested = node_->cursor();
    auto hello_sent = send_message(
        stream_,
        SessionMessage{ClientHello{
            current_session_format_version,
            current_session_format_version,
            config_.show_id,
            config_.node_id,
            config_.incarnation,
            config_.node_index,
            config_.last_director_epoch,
            requested,
            config_.force_checkpoint || node_->needs_resync(),
            config_.limits,
        }},
        config_.codec_limits);
    if (!hello_sent) {
        state_ = NodeSessionState::Closed;
        return Result::failure(std::move(hello_sent.error()));
    }
    state_ = NodeSessionState::AwaitWelcome;

    auto received = receive_message(stream_, config_.codec_limits);
    if (!received) {
        state_ = NodeSessionState::Closed;
        return Result::failure(std::move(received.error()));
    }
    if (const auto* rejected = std::get_if<Reject>(&received.value())) {
        state_ = NodeSessionState::Closed;
        return Result::failure(protocol_failure(
            SessionProtocolErrorCode::PeerRejected,
            rejected->code));
    }
    const auto* welcome = std::get_if<ServerWelcome>(&received.value());
    if (welcome == nullptr) {
        state_ = NodeSessionState::Closed;
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::UnexpectedMessage));
    }
    if (welcome->selected_version != current_session_format_version) {
        state_ = NodeSessionState::Closed;
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::UnsupportedVersion));
    }
    if (welcome->show_id != config_.show_id) {
        state_ = NodeSessionState::Closed;
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::ShowMismatch));
    }
    if (!limits_at_most(welcome->limits, config_.limits)
        || !valid_limits(
            welcome->limits, config_.codec_limits, config_.wire_limits)) {
        state_ = NodeSessionState::Closed;
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::InvalidConfiguration));
    }
    const auto max_inflight_frames = static_cast<std::size_t>(
        welcome->limits.max_inflight_frames);
    if (max_inflight_frames
        > (std::numeric_limits<std::size_t>::max() - 2U) / 2U) {
        close();
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::InvalidConfiguration));
    }
    try {
        inflight_frames_.reserve(max_inflight_frames);
        acknowledgement_intents_.resize(max_inflight_frames * 2U + 2U);
    } catch (const std::bad_alloc&) {
        close();
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::AllocationFailed));
    } catch (const std::length_error&) {
        close();
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::AllocationFailed));
    }
    if ((welcome->mode == ResumeMode::Replay
         || welcome->mode == ResumeMode::UpToDate)
        && (welcome->accepted_from != requested
            || welcome->director_epoch != config_.last_director_epoch)) {
        state_ = NodeSessionState::Closed;
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::EpochMismatch));
    }
    if (welcome->mode == ResumeMode::Checkpoint
        && !same_cursor(welcome->accepted_from, welcome->target)) {
        state_ = NodeSessionState::Closed;
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::CheckpointMismatch));
    }

    session_id_ = welcome->session_id;
    director_epoch_ = welcome->director_epoch;
    negotiated_limits_ = welcome->limits;
    delivered_cursor_ = requested;
    target_cursor_ = welcome->target;
    expected_sequence_ = 1U;
    state_ = NodeSessionState::Resuming;
    ResumeReport report{
        welcome->mode,
        requested,
        welcome->target,
        0U,
        0U,
        false,
    };

    if (welcome->mode == ResumeMode::Checkpoint) {
        auto transfer_message = receive_message(stream_, config_.codec_limits);
        if (!transfer_message) {
            state_ = NodeSessionState::Closed;
            return Result::failure(std::move(transfer_message.error()));
        }
        auto* transfer = std::get_if<CheckpointTransfer>(&transfer_message.value());
        if (transfer == nullptr) {
            state_ = NodeSessionState::Closed;
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::UnexpectedMessage));
        }
        if (transfer->session_id != session_id_) {
            state_ = NodeSessionState::Closed;
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::SessionMismatch));
        }
        if (!sequence_matches_and_advances(
                transfer->sequence, expected_sequence_)) {
            state_ = NodeSessionState::Closed;
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::SequenceMismatch));
        }
        if (!same_cursor(transfer->cursor, target_cursor_)
            || transfer->wire_packet.size()
                > static_cast<std::size_t>(
                    negotiated_limits_.max_checkpoint_bytes)) {
            state_ = NodeSessionState::Closed;
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::CheckpointMismatch));
        }

        auto wire_limits = config_.wire_limits;
        wire_limits.max_message_bytes = std::min(
            wire_limits.max_message_bytes,
            static_cast<std::size_t>(negotiated_limits_.max_checkpoint_bytes));
        auto decoded = wire::decode(transfer->wire_packet, wire_limits);
        if (!decoded) {
            state_ = NodeSessionState::Closed;
            return Result::failure(ResumeSessionError{
                WireDecodeFailure{decoded.error(), 0U}});
        }
        const auto* checkpoint = std::get_if<cmd::StreamCheckpoint>(&decoded.value());
        if (checkpoint == nullptr || !same_cursor(checkpoint->cursor, target_cursor_)
            || checkpoint->document.version() != checkpoint->cursor.revision) {
            state_ = NodeSessionState::Closed;
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::CheckpointMismatch));
        }

        auto sync_message = receive_message(stream_, config_.codec_limits);
        if (!sync_message) {
            state_ = NodeSessionState::Closed;
            return Result::failure(std::move(sync_message.error()));
        }
        const auto* sync = std::get_if<SyncComplete>(&sync_message.value());
        if (sync == nullptr) {
            state_ = NodeSessionState::Closed;
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::UnexpectedMessage));
        }
        if (sync->session_id != session_id_) {
            state_ = NodeSessionState::Closed;
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::SessionMismatch));
        }
        if (!sequence_matches_and_advances(sync->sequence, expected_sequence_)) {
            state_ = NodeSessionState::Closed;
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::SequenceMismatch));
        }
        if (!same_cursor(sync->target, target_cursor_)) {
            state_ = NodeSessionState::Closed;
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::CursorMismatch));
        }

        auto prepared = [&]() {
            const std::lock_guard node_lock{node_mutex_};
            return node_->prepare_checkpoint(*checkpoint);
        }();
        if (!prepared) {
            state_ = NodeSessionState::Closed;
            return Result::failure(protocol_failure(
                SessionProtocolErrorCode::CheckpointPrepareRejected,
                std::nullopt,
                std::nullopt,
                prepared.error()));
        }
        checkpoint_token_ = prepared.value();
        state_ = NodeSessionState::CheckpointStaged;
        report.checkpoint_staged = true;
        return Result::success(report);
    }

    while (true) {
        std::size_t encoded_bytes = 0U;
        auto next = receive_message(
            stream_, config_.codec_limits, &encoded_bytes);
        if (!next) {
            state_ = NodeSessionState::Closed;
            return Result::failure(std::move(next.error()));
        }
        if (auto* frame = std::get_if<FrameBatch>(&next.value())) {
            if (frame->session_id != session_id_) {
                state_ = NodeSessionState::Closed;
                return Result::failure(
                    protocol_failure(SessionProtocolErrorCode::SessionMismatch));
            }
            if (!sequence_matches_and_advances(
                    frame->sequence, expected_sequence_)) {
                state_ = NodeSessionState::Closed;
                return Result::failure(
                    protocol_failure(SessionProtocolErrorCode::SequenceMismatch));
            }
            if (!same_cursor(frame->before, delivered_cursor_)
                || frame->after.through_frame > target_cursor_.through_frame
                || (frame->after.through_frame == target_cursor_.through_frame
                    && !same_cursor(frame->after, target_cursor_))) {
                state_ = NodeSessionState::Closed;
                return Result::failure(
                    protocol_failure(SessionProtocolErrorCode::CursorMismatch));
            }
            const auto frame_bytes = static_cast<core::u64>(encoded_bytes);
            if (inflight_frames_.size()
                    >= static_cast<std::size_t>(
                        negotiated_limits_.max_inflight_frames)
                || inflight_bytes_ > negotiated_limits_.max_inflight_bytes
                || frame_bytes
                    > negotiated_limits_.max_inflight_bytes - inflight_bytes_) {
                state_ = NodeSessionState::Closed;
                return Result::failure(protocol_failure(
                    SessionProtocolErrorCode::InflightLimitExceeded));
            }
            auto decoded = decode_frame_batch(
                *frame,
                negotiated_limits_,
                config_.codec_limits,
                config_.wire_limits);
            if (!decoded) {
                state_ = NodeSessionState::Closed;
                return Result::failure(std::move(decoded.error()));
            }
            try {
                inflight_frames_.push_back(
                    InflightFrame{
                        frame->after.through_frame,
                        frame_bytes,
                        frame->sequence,
                        frame->before,
                        frame->after,
                        false,
                        true,
                        false,
                        0U,
                    });
            } catch (const std::bad_alloc&) {
                state_ = NodeSessionState::Closed;
                return Result::failure(protocol_failure(
                    SessionProtocolErrorCode::AllocationFailed));
            } catch (const std::length_error&) {
                state_ = NodeSessionState::Closed;
                return Result::failure(protocol_failure(
                    SessionProtocolErrorCode::AllocationFailed));
            }
            inflight_bytes_ += frame_bytes;
            core::Result<ReceivedFrameReport, ResumeSessionError> delivered =
                [&]() {
                    const std::lock_guard node_lock{node_mutex_};
                    return deliver_frame(*frame, decoded.value(), *node_);
                }();
            if (!delivered) {
                inflight_bytes_ -= inflight_frames_.back().encoded_bytes;
                inflight_frames_.pop_back();
                state_ = NodeSessionState::Closed;
                return Result::failure(std::move(delivered.error()));
            }
            delivered_cursor_ = frame->after;
            ++report.frames;
            report.commands += delivered.value().commands;
            continue;
        }

        const auto* sync = std::get_if<SyncComplete>(&next.value());
        if (sync == nullptr) {
            state_ = NodeSessionState::Closed;
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::UnexpectedMessage));
        }
        if (sync->session_id != session_id_) {
            state_ = NodeSessionState::Closed;
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::SessionMismatch));
        }
        if (!sequence_matches_and_advances(sync->sequence, expected_sequence_)) {
            state_ = NodeSessionState::Closed;
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::SequenceMismatch));
        }
        if (!same_cursor(sync->target, target_cursor_)
            || !same_cursor(delivered_cursor_, target_cursor_)) {
            state_ = NodeSessionState::Closed;
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::CursorMismatch));
        }
        break;
    }

    auto acknowledged = send_message(
        stream_,
        SessionMessage{ResumeAck{session_id_, 1U, target_cursor_}},
        config_.codec_limits);
    if (!acknowledged) {
        state_ = NodeSessionState::Closed;
        return Result::failure(std::move(acknowledged.error()));
    }
    for (auto& frame : inflight_frames_) {
        frame.prepared_sent = true;
    }
    next_ack_sequence_ = 2U;
    last_flushed_ack_sequence_ = 1U;
    state_ = NodeSessionState::Live;
    return Result::success(report);
}

core::Result<std::monostate, ResumeSessionError>
NodeResumeSession::activate_checkpoint() {
    auto activated = activate_checkpoint_boundary();
    if (!activated) {
        return activated;
    }
    return complete_checkpoint_activation();
}

core::Result<std::monostate, ResumeSessionError>
NodeResumeSession::activate_checkpoint_boundary() {
    if (state_ != NodeSessionState::CheckpointStaged || !checkpoint_token_) {
        return SessionStatus::failure(
            protocol_failure(SessionProtocolErrorCode::WrongState));
    }
    const auto token = *checkpoint_token_;
    const auto activated = [&]() {
        const std::lock_guard node_lock{node_mutex_};
        return node_->activate_prepared_checkpoint(token);
    }();
    if (!activated) {
        state_ = NodeSessionState::Closed;
        return SessionStatus::failure(
            protocol_failure(
                SessionProtocolErrorCode::CheckpointActivationRejected));
    }
    checkpoint_token_.reset();
    delivered_cursor_ = target_cursor_;
    state_ = NodeSessionState::CheckpointActivated;
    return SessionStatus::success(std::monostate{});
}

core::Result<std::monostate, ResumeSessionError>
NodeResumeSession::complete_checkpoint_activation() {
    if (state_ != NodeSessionState::CheckpointActivated
        || checkpoint_token_) {
        return SessionStatus::failure(
            protocol_failure(SessionProtocolErrorCode::WrongState));
    }
    auto acknowledged = send_message(
        stream_,
        SessionMessage{ResumeAck{session_id_, 1U, target_cursor_}},
        config_.codec_limits);
    if (!acknowledged) {
        state_ = NodeSessionState::Closed;
        return acknowledged;
    }
    next_ack_sequence_ = 2U;
    last_flushed_ack_sequence_ = 1U;
    state_ = NodeSessionState::Live;
    return SessionStatus::success(std::monostate{});
}

core::Result<ReceivedFrameReport, ResumeSessionError>
NodeResumeSession::receive_live_frame() {
    using Result = core::Result<ReceivedFrameReport, ResumeSessionError>;
    if (state_ != NodeSessionState::Live) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::WrongState));
    }

    {
        const std::lock_guard inflight_lock{inflight_mutex_};
        if (inflight_frames_.size()
                >= static_cast<std::size_t>(
                    negotiated_limits_.max_inflight_frames)
            || inflight_bytes_ >= negotiated_limits_.max_inflight_bytes) {
            return Result::failure(protocol_failure(
                SessionProtocolErrorCode::InflightWindowFull));
        }
    }

    auto received = receive_live_message();
    if (!received) {
        return Result::failure(std::move(received.error()));
    }
    const auto* frame = std::get_if<ReceivedFrameReport>(&received.value());
    if (frame == nullptr) {
        close();
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::UnexpectedMessage));
    }
    auto prepared = flush_frame_acknowledgements();
    if (!prepared) {
        return Result::failure(std::move(prepared.error()));
    }
    return Result::success(*frame);
}

core::Result<NodeLiveInboundReport, ResumeSessionError>
NodeResumeSession::receive_live_message() {
    using Result = core::Result<NodeLiveInboundReport, ResumeSessionError>;
    if (state() != NodeSessionState::Live) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::WrongState));
    }

    std::size_t encoded_bytes = 0U;
    auto received = receive_message(
        stream_, config_.codec_limits, &encoded_bytes);
    if (!received) {
        close();
        return Result::failure(std::move(received.error()));
    }
    if (const auto* frame = std::get_if<FrameBatch>(&received.value())) {
        auto processed = receive_decoded_live_frame(*frame, encoded_bytes);
        if (!processed) {
            return Result::failure(std::move(processed.error()));
        }
        return Result::success(NodeLiveInboundReport{processed.value()});
    }
    if (const auto* permit =
            std::get_if<FrameCommitPermit>(&received.value())) {
        auto processed = receive_decoded_commit_permit(*permit);
        if (!processed) {
            return Result::failure(std::move(processed.error()));
        }
        return Result::success(NodeLiveInboundReport{processed.value()});
    }
    if (const auto* heartbeat = std::get_if<Heartbeat>(&received.value())) {
        auto processed = receive_decoded_heartbeat(*heartbeat);
        if (!processed) {
            return Result::failure(std::move(processed.error()));
        }
        return Result::success(NodeLiveInboundReport{processed.value()});
    }
    if (const auto* goodbye = std::get_if<Goodbye>(&received.value())) {
        auto processed = receive_decoded_goodbye(*goodbye);
        if (!processed) {
            return Result::failure(std::move(processed.error()));
        }
        return Result::success(NodeLiveInboundReport{processed.value()});
    }

    close();
    return Result::failure(
        protocol_failure(SessionProtocolErrorCode::UnexpectedMessage));
}

core::Result<HeartbeatReceivedReport, ResumeSessionError>
NodeResumeSession::receive_decoded_heartbeat(const Heartbeat& heartbeat) {
    using Result = core::Result<
        HeartbeatReceivedReport,
        ResumeSessionError>;
    std::optional<SessionProtocolErrorCode> failure;
    HeartbeatReceivedReport report{};
    {
        // This is the same lock/order used by show-clock commit. The cursor
        // snapshot and HeartbeatAck enqueue therefore occupy one precise slot
        // in the node control FIFO relative to Committed acknowledgements.
        const std::lock_guard inflight_lock{inflight_mutex_};
        if (state() != NodeSessionState::Live) {
            failure = SessionProtocolErrorCode::WrongState;
        } else if (heartbeat.session_id != session_id_) {
            failure = SessionProtocolErrorCode::SessionMismatch;
        } else if (heartbeat.director_epoch != director_epoch_) {
            failure = SessionProtocolErrorCode::EpochMismatch;
        } else if (expected_sequence_
                       == std::numeric_limits<core::u64>::max()
                   || heartbeat.sequence != expected_sequence_) {
            failure = SessionProtocolErrorCode::SequenceMismatch;
        } else if (last_released_through_
                   && !cursor_can_advance(
                       *last_released_through_,
                       heartbeat.released_through)) {
            failure = SessionProtocolErrorCode::CursorMismatch;
        } else if (acknowledgement_size_
                       >= acknowledgement_intents_.size()
                   || next_ack_sequence_
                       == std::numeric_limits<core::u64>::max()) {
            failure = SessionProtocolErrorCode::AcknowledgementMismatch;
        } else {
            cmd::StreamCursor visible{};
            {
                const std::lock_guard node_lock{node_mutex_};
                visible = node_->cursor();
            }
            const RuntimeAssetAdoption asset_adoption =
                runtime_asset_adoption_;
            const RuntimeOutputSync output_sync = runtime_output_sync_;
            const auto acknowledgement_sequence = enqueue_control_locked(
                NodeControlKind::HeartbeatAcknowledgement,
                heartbeat.sequence,
                visible,
                asset_adoption,
                output_sync);
            ++expected_sequence_;
            last_released_through_ = heartbeat.released_through;
            report = HeartbeatReceivedReport{
                heartbeat.sequence,
                acknowledgement_sequence,
                heartbeat.response_timeout_ms,
                heartbeat.released_through,
                visible,
                asset_adoption,
                output_sync,
            };
        }
    }
    if (failure) {
        if (*failure != SessionProtocolErrorCode::WrongState) {
            close();
        }
        return Result::failure(protocol_failure(*failure));
    }
    return Result::success(report);
}

core::Result<GoodbyeReport, ResumeSessionError>
NodeResumeSession::receive_decoded_goodbye(const Goodbye& goodbye) {
    using Result = core::Result<GoodbyeReport, ResumeSessionError>;
    std::optional<SessionProtocolErrorCode> failure;
    {
        // Fence the dedicated writer before accepting a peer close. No new
        // node control record may begin after the fully received Goodbye.
        const std::lock_guard control_lock{control_write_mutex_};
        const std::lock_guard inflight_lock{inflight_mutex_};
        if (state() != NodeSessionState::Live) {
            failure = SessionProtocolErrorCode::WrongState;
        } else if (goodbye.session_id != session_id_) {
            failure = SessionProtocolErrorCode::SessionMismatch;
        } else if (goodbye.director_epoch != director_epoch_) {
            failure = SessionProtocolErrorCode::EpochMismatch;
        } else if (expected_sequence_
                       == std::numeric_limits<core::u64>::max()
                   || goodbye.sequence != expected_sequence_) {
            failure = SessionProtocolErrorCode::SequenceMismatch;
        } else if (last_released_through_
                   && !cursor_can_advance(
                       *last_released_through_,
                       goodbye.final_cursor)) {
            failure = SessionProtocolErrorCode::CursorMismatch;
        } else {
            ++expected_sequence_;
            acknowledgement_head_ = 0U;
            acknowledgement_size_ = 0U;
            state_ = NodeSessionState::Draining;
        }
    }
    if (failure) {
        if (*failure != SessionProtocolErrorCode::WrongState) {
            close();
        }
        return Result::failure(protocol_failure(*failure));
    }
    return Result::success(GoodbyeReport{
        goodbye.sequence,
        goodbye.reason,
        goodbye.final_cursor,
    });
}

core::Result<ReceivedFrameReport, ResumeSessionError>
NodeResumeSession::receive_decoded_live_frame(
    const FrameBatch& frame,
    std::size_t encoded_bytes) {
    using Result = core::Result<ReceivedFrameReport, ResumeSessionError>;
    if (frame.session_id != session_id_) {
        close();
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::SessionMismatch));
    }
    const auto frame_bytes = static_cast<core::u64>(encoded_bytes);
    {
        const std::lock_guard inflight_lock{inflight_mutex_};
        if (state() != NodeSessionState::Live) {
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::WrongState));
        }
        if (expected_sequence_ == std::numeric_limits<core::u64>::max()
            || frame.sequence != expected_sequence_) {
            close();
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::SequenceMismatch));
        }
        if (!same_cursor(frame.before, delivered_cursor_)) {
            close();
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::CursorMismatch));
        }
        if (inflight_frames_.size()
                >= static_cast<std::size_t>(
                    negotiated_limits_.max_inflight_frames)
            || inflight_bytes_ > negotiated_limits_.max_inflight_bytes
            || frame_bytes
                > negotiated_limits_.max_inflight_bytes - inflight_bytes_) {
            close();
            return Result::failure(protocol_failure(
                SessionProtocolErrorCode::InflightLimitExceeded));
        }
    }
    auto decoded = decode_frame_batch(
        frame,
        negotiated_limits_,
        config_.codec_limits,
        config_.wire_limits);
    if (!decoded) {
        close();
        return Result::failure(std::move(decoded.error()));
    }
    {
        const std::lock_guard inflight_lock{inflight_mutex_};
        if (state() != NodeSessionState::Live) {
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::WrongState));
        }
        if (frame.sequence != expected_sequence_
            || !same_cursor(frame.before, delivered_cursor_)) {
            close();
            return Result::failure(
                protocol_failure(SessionProtocolErrorCode::SequenceMismatch));
        }
        try {
            inflight_frames_.push_back(
                InflightFrame{
                    frame.after.through_frame,
                    frame_bytes,
                    frame.sequence,
                    frame.before,
                    frame.after,
                    false,
                    false,
                    false,
                    0U,
                });
        } catch (const std::bad_alloc&) {
            close();
            return Result::failure(protocol_failure(
                SessionProtocolErrorCode::AllocationFailed));
        } catch (const std::length_error&) {
            close();
            return Result::failure(protocol_failure(
                SessionProtocolErrorCode::AllocationFailed));
        }
        inflight_bytes_ += frame_bytes;
    }
    core::Result<ReceivedFrameReport, ResumeSessionError> delivered = [&]() {
        const std::lock_guard node_lock{node_mutex_};
        return deliver_frame(frame, decoded.value(), *node_);
    }();
    if (!delivered) {
        const std::lock_guard inflight_lock{inflight_mutex_};
        const auto pending = std::find_if(
            inflight_frames_.begin(),
            inflight_frames_.end(),
            [&frame](const InflightFrame& candidate) {
                return candidate.acknowledged_sequence == frame.sequence;
            });
        if (pending != inflight_frames_.end()) {
            inflight_bytes_ -= pending->encoded_bytes;
            inflight_frames_.erase(pending);
        }
        close();
        return Result::failure(std::move(delivered.error()));
    }
    std::optional<SessionProtocolErrorCode> enqueue_error;
    {
        const std::lock_guard inflight_lock{inflight_mutex_};
        const auto pending = std::find_if(
            inflight_frames_.begin(),
            inflight_frames_.end(),
            [&frame](const InflightFrame& candidate) {
                return candidate.acknowledged_sequence == frame.sequence;
            });
        if (state() != NodeSessionState::Live) {
            if (pending != inflight_frames_.end()) {
                inflight_bytes_ -= pending->encoded_bytes;
                inflight_frames_.erase(pending);
            }
            enqueue_error = SessionProtocolErrorCode::WrongState;
        } else if (frame.sequence != expected_sequence_
                   || !same_cursor(frame.before, delivered_cursor_)) {
            enqueue_error = SessionProtocolErrorCode::SequenceMismatch;
        } else if (pending == inflight_frames_.end()) {
            enqueue_error = SessionProtocolErrorCode::AcknowledgementMismatch;
        } else if (
            acknowledgement_size_ >= acknowledgement_intents_.size()) {
            enqueue_error = SessionProtocolErrorCode::AcknowledgementMismatch;
        } else if (
            next_ack_sequence_ == std::numeric_limits<core::u64>::max()) {
            enqueue_error = SessionProtocolErrorCode::SequenceMismatch;
        } else {
            static_cast<void>(enqueue_control_locked(
                NodeControlKind::Prepared,
                frame.sequence,
                frame.after));
            ++expected_sequence_;
            delivered_cursor_ = frame.after;
        }
    }
    if (enqueue_error.has_value()) {
        if (*enqueue_error != SessionProtocolErrorCode::WrongState) {
            close();
        }
        return Result::failure(protocol_failure(*enqueue_error));
    }
    return delivered;
}

core::Result<FrameCommitPermitReport, ResumeSessionError>
NodeResumeSession::receive_commit_permit() {
    using Result = core::Result<
        FrameCommitPermitReport,
        ResumeSessionError>;
    if (state() != NodeSessionState::Live) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::WrongState));
    }

    auto received = receive_live_message();
    if (!received) {
        return Result::failure(std::move(received.error()));
    }
    const auto* permit =
        std::get_if<FrameCommitPermitReport>(&received.value());
    if (permit == nullptr) {
        close();
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::UnexpectedMessage));
    }
    return Result::success(*permit);
}

core::Result<FrameCommitPermitReport, ResumeSessionError>
NodeResumeSession::receive_decoded_commit_permit(
    const FrameCommitPermit& permit) {
    using Result = core::Result<
        FrameCommitPermitReport,
        ResumeSessionError>;
    std::optional<SessionProtocolErrorCode> failure;
    FrameCommitPermitReport report{};
    {
        const std::lock_guard inflight_lock{inflight_mutex_};
        if (state() != NodeSessionState::Live) {
            failure = SessionProtocolErrorCode::WrongState;
        } else if (permit.session_id != session_id_) {
            failure = SessionProtocolErrorCode::SessionMismatch;
        } else if (permit.director_epoch != director_epoch_) {
            failure = SessionProtocolErrorCode::EpochMismatch;
        } else if (expected_sequence_
                       == std::numeric_limits<core::u64>::max()
                   || permit.sequence != expected_sequence_) {
            failure = SessionProtocolErrorCode::SequenceMismatch;
        } else {
            const auto pending = std::find_if(
                inflight_frames_.begin(),
                inflight_frames_.end(),
                [](const InflightFrame& candidate) {
                    return !candidate.permitted;
                });
            if (pending == inflight_frames_.end()) {
                failure = SessionProtocolErrorCode::AcknowledgementMismatch;
            } else if (permit.acknowledged_sequence
                       != pending->acknowledged_sequence) {
                failure = SessionProtocolErrorCode::AcknowledgementMismatch;
            } else if (!same_cursor(permit.before, pending->before)
                       || !same_cursor(permit.after, pending->after)) {
                failure = SessionProtocolErrorCode::CursorMismatch;
            } else if (!pending->prepared_sent) {
                failure = SessionProtocolErrorCode::FrameNotPrepared;
            } else if (pending->committed || permit.barrier_ordinal == 0U
                       || permit.barrier_ordinal
                           <= last_commit_permit_ordinal_) {
                failure = SessionProtocolErrorCode::InvalidCommitPermit;
            } else {
                pending->permitted = true;
                pending->barrier_ordinal = permit.barrier_ordinal;
                last_commit_permit_ordinal_ = permit.barrier_ordinal;
                ++expected_sequence_;
                const auto permitted_count =
                    static_cast<std::size_t>(std::count_if(
                        inflight_frames_.begin(),
                        inflight_frames_.end(),
                        [](const InflightFrame& candidate) {
                            return candidate.permitted;
                        }));
                report = FrameCommitPermitReport{
                    permit.sequence,
                    permit.acknowledged_sequence,
                    permit.barrier_ordinal,
                    permit.before,
                    permit.after,
                    permitted_count,
                };
            }
        }
    }

    if (failure) {
        if (*failure != SessionProtocolErrorCode::WrongState) {
            close();
        }
        return Result::failure(protocol_failure(*failure));
    }
    return Result::success(report);
}

core::u64 NodeResumeSession::enqueue_control_locked(
    NodeControlKind kind,
    core::u64 acknowledged_sequence,
    const cmd::StreamCursor& after,
    RuntimeAssetAdoption asset_adoption,
    RuntimeOutputSync output_sync,
    GoodbyeReason goodbye_reason) noexcept {
    const auto capacity = acknowledgement_intents_.size();
    auto tail = acknowledgement_head_;
    const auto until_wrap = capacity - acknowledgement_head_;
    if (acknowledgement_size_ >= until_wrap) {
        tail = acknowledgement_size_ - until_wrap;
    } else {
        tail += acknowledgement_size_;
    }
    const auto sequence = next_ack_sequence_;
    acknowledgement_intents_[tail] = NodeControlIntent{
        kind,
        sequence,
        acknowledged_sequence,
        after,
        asset_adoption,
        output_sync,
        goodbye_reason,
    };
    ++acknowledgement_size_;
    ++next_ack_sequence_;
    return sequence;
}

core::Result<FrameAcknowledgementFlushReport, ResumeSessionError>
NodeResumeSession::flush_frame_acknowledgements() {
    return flush_frame_acknowledgements_through(std::nullopt);
}

core::Result<GoodbyeReport, ResumeSessionError>
NodeResumeSession::request_goodbye(GoodbyeReason reason) noexcept {
    using Result = core::Result<GoodbyeReport, ResumeSessionError>;
    if (reason == GoodbyeReason::None
        || state() != NodeSessionState::Live) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::WrongState));
    }

    const std::lock_guard inflight_lock{inflight_mutex_};
    if (state() != NodeSessionState::Live) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::WrongState));
    }
    if (acknowledgement_size_ >= acknowledgement_intents_.size()
        || next_ack_sequence_ == std::numeric_limits<core::u64>::max()) {
        state_ = NodeSessionState::Closed;
        return Result::failure(protocol_failure(
            SessionProtocolErrorCode::AcknowledgementMismatch));
    }
    cmd::StreamCursor final_cursor{};
    {
        const std::lock_guard node_lock{node_mutex_};
        final_cursor = node_->cursor();
    }
    const auto sequence = enqueue_control_locked(
        NodeControlKind::Goodbye,
        0U,
        final_cursor,
        {},
        {},
        reason);
    state_ = NodeSessionState::Draining;
    return Result::success(GoodbyeReport{
        sequence,
        reason,
        final_cursor,
    });
}

core::Result<FrameAcknowledgementFlushReport, ResumeSessionError>
NodeResumeSession::flush_frame_acknowledgements_through(
    std::optional<core::u64> through_sequence) {
    using Result = core::Result<
        FrameAcknowledgementFlushReport,
        ResumeSessionError>;
    if (state() != NodeSessionState::Live
        && state() != NodeSessionState::Draining) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::WrongState));
    }

    FrameAcknowledgementFlushReport report{};
    const std::lock_guard control_lock{control_write_mutex_};
    while (true) {
        NodeControlIntent pending{};
        bool missing_required_acknowledgement = false;
        {
            const std::lock_guard inflight_lock{inflight_mutex_};
            if (through_sequence.has_value()
                && last_flushed_ack_sequence_ >= *through_sequence) {
                report.remaining = acknowledgement_size_;
                return Result::success(report);
            }
            if (acknowledgement_size_ == 0U) {
                report.remaining = 0U;
                if (through_sequence.has_value()) {
                    missing_required_acknowledgement = true;
                } else {
                    return Result::success(report);
                }
            } else {
                pending = acknowledgement_intents_[acknowledgement_head_];
            }
        }
        if (missing_required_acknowledgement) {
            close();
            return Result::failure(protocol_failure(
                SessionProtocolErrorCode::AcknowledgementMismatch));
        }

        const auto send_pending = [this, &pending]() -> SessionStatus {
            switch (pending.kind) {
            case NodeControlKind::Prepared:
                return send_message(
                    stream_,
                    SessionMessage{FramePrepared{
                        session_id_,
                        pending.sequence,
                        pending.acknowledged_sequence,
                        pending.after,
                    }},
                    config_.codec_limits);
            case NodeControlKind::Committed:
                return send_message(
                    stream_,
                    SessionMessage{FrameCommitted{
                        session_id_,
                        pending.sequence,
                        pending.acknowledged_sequence,
                        pending.after,
                    }},
                    config_.codec_limits);
            case NodeControlKind::HeartbeatAcknowledgement:
                return send_message(
                    stream_,
                    SessionMessage{HeartbeatAck{
                        session_id_,
                        director_epoch_,
                        pending.sequence,
                        pending.acknowledged_sequence,
                        pending.after,
                        pending.asset_adoption.manifest_digest,
                        pending.asset_adoption.ready,
                        pending.output_sync,
                    }},
                    config_.codec_limits);
            case NodeControlKind::Goodbye:
                return send_message(
                    stream_,
                    SessionMessage{Goodbye{
                        session_id_,
                        director_epoch_,
                        pending.sequence,
                        pending.goodbye_reason,
                        pending.after,
                    }},
                    config_.codec_limits);
            }
            return SessionStatus::failure(protocol_failure(
                SessionProtocolErrorCode::UnexpectedMessage));
        };
        auto sent = send_pending();
        if (!sent) {
            close();
            return Result::failure(std::move(sent.error()));
        }

        bool acknowledgement_mismatch = false;
        {
            const std::lock_guard inflight_lock{inflight_mutex_};
            if (acknowledgement_size_ == 0U) {
                acknowledgement_mismatch = true;
            } else {
                const auto& queued =
                    acknowledgement_intents_[acknowledgement_head_];
                acknowledgement_mismatch = queued.kind != pending.kind
                    || queued.sequence != pending.sequence
                    || queued.acknowledged_sequence
                        != pending.acknowledged_sequence
                    || !same_cursor(queued.after, pending.after)
                    || pending.sequence != last_flushed_ack_sequence_ + 1U;
            }

            if (!acknowledgement_mismatch
                && pending.kind == NodeControlKind::Prepared) {
                const auto frame = std::find_if(
                    inflight_frames_.begin(),
                    inflight_frames_.end(),
                    [&pending](const InflightFrame& candidate) {
                        return candidate.acknowledged_sequence
                                == pending.acknowledged_sequence
                            && same_cursor(candidate.after, pending.after);
                    });
                if (frame == inflight_frames_.end() || frame->prepared_sent
                    || frame->committed) {
                    acknowledgement_mismatch = true;
                } else {
                    frame->prepared_sent = true;
                }
            }

            if (!acknowledgement_mismatch
                && pending.kind == NodeControlKind::Committed) {
                if (inflight_frames_.empty()) {
                    acknowledgement_mismatch = true;
                } else {
                    const auto& frame = inflight_frames_.front();
                    if (frame.acknowledged_sequence
                            != pending.acknowledged_sequence
                        || !same_cursor(frame.after, pending.after)
                        || !frame.prepared_sent || !frame.permitted
                        || !frame.committed) {
                        acknowledgement_mismatch = true;
                    } else {
                        inflight_bytes_ -= frame.encoded_bytes;
                        inflight_frames_.erase(inflight_frames_.begin());
                    }
                }
            }

            if (!acknowledgement_mismatch) {
                ++acknowledgement_head_;
                if (acknowledgement_head_
                    == acknowledgement_intents_.size()) {
                    acknowledgement_head_ = 0U;
                }
                --acknowledgement_size_;
                last_flushed_ack_sequence_ = pending.sequence;
                ++report.flushed;
                if (pending.kind
                    == NodeControlKind::HeartbeatAcknowledgement) {
                    ++report.heartbeat_acknowledgements;
                } else if (pending.kind == NodeControlKind::Goodbye) {
                    ++report.goodbyes;
                }
                report.remaining = acknowledgement_size_;
            }
        }

        if (acknowledgement_mismatch) {
            close();
            return Result::failure(protocol_failure(
                SessionProtocolErrorCode::AcknowledgementMismatch));
        }
        if (pending.kind == NodeControlKind::Goodbye) {
            state_ = NodeSessionState::Closed;
            // Half-close rather than close. Closing here would shut the
            // receive direction down with the Director's own traffic still
            // queued on it, which turns the close into a reset - and a reset
            // discards whatever the peer has not read yet, including the
            // Goodbye written a microsecond earlier. Whether the Director's
            // reader had already drained that byte range is pure scheduling,
            // so the graceful shutdown either worked or silently degraded to a
            // dropped connection depending on machine load. The FIN is ordered
            // behind the record instead, and close() completes the handshake
            // once this session's worker has unwound.
            sent_goodbye_.store(true, std::memory_order_release);
            stream_.shutdown_send();
            return Result::success(report);
        }
        if (through_sequence.has_value()
            && pending.sequence == *through_sequence) {
            return Result::success(report);
        }
    }
}

core::Result<cmd::CommitResult, ResumeSessionError>
NodeResumeSession::commit_prepared_frame(cmd::FrameNumber frame) {
    using Result = core::Result<cmd::CommitResult, ResumeSessionError>;
    if (state() != NodeSessionState::Live) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::WrongState));
    }

    std::unique_lock inflight_lock{inflight_mutex_};
    if (state() != NodeSessionState::Live) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::WrongState));
    }
    const auto pending = std::find_if(
        inflight_frames_.begin(),
        inflight_frames_.end(),
        [](const InflightFrame& candidate) { return !candidate.committed; });
    if (pending == inflight_frames_.end() || pending->frame != frame) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::FrameNotPrepared));
    }
    if (!pending->prepared_sent) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::FrameNotPrepared));
    }
    if (!pending->permitted) {
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::FrameNotPermitted));
    }
    if (acknowledgement_size_ >= acknowledgement_intents_.size()) {
        state_ = NodeSessionState::Closed;
        return Result::failure(protocol_failure(
            SessionProtocolErrorCode::AcknowledgementMismatch));
    }
    if (next_ack_sequence_ == std::numeric_limits<core::u64>::max()) {
        state_ = NodeSessionState::Closed;
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::SequenceMismatch));
    }

    cmd::CommitResult committed{};
    cmd::StreamCursor committed_cursor{};
    {
        const std::lock_guard node_lock{node_mutex_};
        committed = node_->commit_frame(frame);
        committed_cursor = node_->cursor();
    }
    if (committed.status != cmd::CommitStatus::Committed
        || !same_cursor(committed_cursor, pending->after)) {
        state_ = NodeSessionState::Closed;
        return Result::failure(
            protocol_failure(SessionProtocolErrorCode::FrameNotCommitted));
    }

    pending->committed = true;
    static_cast<void>(enqueue_control_locked(
        NodeControlKind::Committed,
        pending->acknowledged_sequence,
        pending->after));
    return Result::success(committed);
}

NodeBoundaryState NodeResumeSession::boundary_state() const noexcept {
    NodeBoundaryState state{};
    state.live = this->state() == NodeSessionState::Live;
    {
        // The same lock the publish path holds. The node bus is read here and
        // never from a caller thread, because the network worker mutates its
        // staging containers concurrently.
        const std::lock_guard node_lock{node_mutex_};
        state.cursor = node_->cursor();
        state.needs_resync = node_->needs_resync();
        state.checkpoint_staged = node_->prepared_checkpoint() != nullptr;
    }
    // Taken separately: the publish path holds inflight before node, so never
    // holding both here keeps this query outside that ordering entirely.
    {
        const std::lock_guard inflight_lock{inflight_mutex_};
        const auto pending = std::find_if(
            inflight_frames_.begin(),
            inflight_frames_.end(),
            [](const InflightFrame& candidate) { return !candidate.committed; });
        if (pending != inflight_frames_.end()) {
            state.next_frame = pending->frame;
            state.next_frame_permitted =
                pending->prepared_sent && pending->permitted;
        }
    }
    return state;
}

std::size_t NodeResumeSession::inflight_frames() const noexcept {
    const std::lock_guard lock{inflight_mutex_};
    return inflight_frames_.size();
}

core::u64 NodeResumeSession::inflight_bytes() const noexcept {
    const std::lock_guard lock{inflight_mutex_};
    return inflight_bytes_;
}

std::size_t NodeResumeSession::pending_frame_acknowledgements() const noexcept {
    const std::lock_guard lock{inflight_mutex_};
    std::size_t count = 0U;
    for (std::size_t offset = 0U; offset < acknowledgement_size_; ++offset) {
        const auto index =
            (acknowledgement_head_ + offset) % acknowledgement_intents_.size();
        const auto kind = acknowledgement_intents_[index].kind;
        if (kind == NodeControlKind::Prepared
            || kind == NodeControlKind::Committed) {
            ++count;
        }
    }
    return count;
}

std::size_t NodeResumeSession::pending_control_messages() const noexcept {
    const std::lock_guard lock{inflight_mutex_};
    return acknowledgement_size_;
}

std::size_t NodeResumeSession::permitted_frames() const noexcept {
    const std::lock_guard lock{inflight_mutex_};
    return static_cast<std::size_t>(std::count_if(
        inflight_frames_.begin(),
        inflight_frames_.end(),
        [](const InflightFrame& candidate) {
            return candidate.permitted;
        }));
}

core::u64 NodeResumeSession::last_commit_permit_ordinal() const noexcept {
    const std::lock_guard lock{inflight_mutex_};
    return last_commit_permit_ordinal_;
}

void NodeResumeSession::publish_runtime_asset_adoption(
    RuntimeAssetAdoption adoption) noexcept {
    const std::lock_guard lock{inflight_mutex_};
    runtime_asset_adoption_ = adoption;
}

void NodeResumeSession::publish_runtime_output_sync(
    RuntimeOutputSync sync) noexcept {
    // A node can only ever report at most as many joined outputs as it has.
    // Clamping here rather than at the codec keeps a renderer bug from
    // closing an otherwise healthy session.
    if (sync.joined_outputs > sync.expected_outputs) {
        sync.joined_outputs = sync.expected_outputs;
    }
    const std::lock_guard lock{inflight_mutex_};
    runtime_output_sync_ = sync;
}

void NodeResumeSession::interrupt_transport() noexcept {
    stream_.close();
}

void NodeResumeSession::close() noexcept {
    if (checkpoint_token_) {
        const std::lock_guard node_lock{node_mutex_};
        static_cast<void>(node_->abort_prepared_checkpoint(*checkpoint_token_));
        checkpoint_token_.reset();
    }
    state_ = NodeSessionState::Closed;
    if (sent_goodbye_.exchange(false, std::memory_order_acq_rel)) {
        // The peer's own close is the acknowledgement that it read the
        // Goodbye, so wait for it before releasing the socket. Bounded, and
        // only a peer that reads the record and then never closes pays it in
        // full.
        stream_.drain_and_close(goodbye_drain_timeout);
        return;
    }
    stream_.close();
}

} // namespace mediaserver::net
