#include <mediaserver/persist/command_log.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace mediaserver::persist {
namespace {

using BridgeStatus = core::Result<std::monostate, CommandLogError>;

struct DecodedFrame final {
    std::vector<cmd::Command> commands;
    cmd::FrameSeal seal;
};

[[nodiscard]] LogCursor to_log_cursor(const cmd::StreamCursor& cursor) noexcept {
    return LogCursor{
        cursor.through_frame.value(),
        cursor.revision.value(),
        cursor.next_command.value(),
    };
}

[[nodiscard]] cmd::StreamCursor to_stream_cursor(LogCursor cursor) noexcept {
    return cmd::StreamCursor{
        cmd::FrameNumber{cursor.through_frame},
        doc::Version{cursor.revision},
        cmd::CommandId{cursor.next_command},
    };
}

[[nodiscard]] bool same_cursor(
    const cmd::StreamCursor& left,
    const cmd::StreamCursor& right) noexcept {
    return left.through_frame == right.through_frame && left.revision == right.revision
        && left.next_command == right.next_command;
}

[[nodiscard]] CommandLogError log_failure(LogError error) {
    return CommandLogError{LogFailure{std::move(error)}};
}

[[nodiscard]] CommandLogError range_failure(
    LogErrorCode code,
    std::string detail) {
    return log_failure(LogError{code, 0U, {}, std::move(detail)});
}

[[nodiscard]] CommandLogError protocol_failure(
    CommandProtocolError error,
    std::size_t record_index = no_record_index) {
    return CommandLogError{ProtocolFailure{error, record_index}};
}

[[nodiscard]] CommandLogError storage_limit_failure(
    const SealedFrameLog& storage,
    std::string detail) {
    return log_failure(LogError{
        LogErrorCode::LimitExceeded,
        storage.recovery_report().valid_bytes,
        {},
        std::move(detail),
    });
}

[[nodiscard]] std::optional<ProtocolFailure> validate_batch(
    std::span<const cmd::Command> commands,
    const cmd::FrameSeal& seal) noexcept {
    if (!seal.frame) {
        return ProtocolFailure{CommandProtocolError::InvalidSeal, no_record_index};
    }
    if (commands.size() != seal.commands.size()) {
        return ProtocolFailure{
            CommandProtocolError::CommandCountMismatch, no_record_index};
    }

    auto expected_revision = seal.base.value();
    core::u64 previous_id = 0U;
    for (std::size_t index = 0U; index < commands.size(); ++index) {
        const auto& command = commands[index];
        const auto& descriptor = seal.commands[index];
        const auto id = command.id.value();
        if (id == 0U || descriptor.id != command.id ||
            (index != 0U &&
             (previous_id == std::numeric_limits<core::u64>::max() ||
              id != previous_id + 1U))) {
            return ProtocolFailure{CommandProtocolError::CommandIdentityMismatch, index};
        }
        if (descriptor.fingerprint != cmd::fingerprint(command)) {
            return ProtocolFailure{CommandProtocolError::CommandFingerprintMismatch, index};
        }
        if (command.apply_at != seal.frame) {
            return ProtocolFailure{CommandProtocolError::CommandFrameMismatch, index};
        }
        if (command.base.value() != expected_revision ||
            expected_revision == std::numeric_limits<core::u64>::max() ||
            command.result.value() != expected_revision + 1U) {
            return ProtocolFailure{CommandProtocolError::CommandRevisionMismatch, index};
        }
        expected_revision = command.result.value();
        previous_id = id;
    }

    if (expected_revision != seal.result.value()) {
        return ProtocolFailure{
            CommandProtocolError::SealRevisionMismatch, no_record_index};
    }
    if (!commands.empty() && commands.back().id.value() ==
                                 std::numeric_limits<core::u64>::max()) {
        return ProtocolFailure{
            CommandProtocolError::CommandIdExhausted, commands.size() - 1U};
    }
    return std::nullopt;
}

using EncodedRecordsResult =
    core::Result<std::vector<RecordBytes>, CommandLogError>;

[[nodiscard]] EncodedRecordsResult encode_records(
    const SealedFrameLog* storage,
    const wire::DecodeLimits& decode_limits,
    std::span<const cmd::Command> commands,
    const cmd::FrameSeal& seal) {
    if (storage != nullptr &&
        (storage->limits().max_records_per_frame == 0U ||
         commands.size() > static_cast<std::size_t>(
                               storage->limits().max_records_per_frame - 1U))) {
        return EncodedRecordsResult::failure(storage_limit_failure(
            *storage, "typed frame exceeds the WAL record-count limit"));
    }
    if (const auto error = validate_batch(commands, seal)) {
        return EncodedRecordsResult::failure(CommandLogError{*error});
    }

    std::vector<RecordBytes> records;
    records.reserve(commands.size() + 1U);
    auto encode_limits = decode_limits;
    if (storage != nullptr) {
        encode_limits.max_message_bytes = std::min(
            encode_limits.max_message_bytes,
            static_cast<std::size_t>(storage->limits().max_record_bytes));
    }
    std::uint64_t payload_bytes = 0U;
    const auto append_record = [storage, &records, &payload_bytes](
                                   RecordBytes encoded)
        -> std::optional<CommandLogError> {
        if (storage == nullptr) {
            records.push_back(std::move(encoded));
            return std::nullopt;
        }
        const auto& limits = storage->limits();
        if (encoded.size() >
            static_cast<std::size_t>(limits.max_record_bytes)) {
            return storage_limit_failure(
                *storage, "encoded wire record exceeds the WAL record limit");
        }
        const auto framed_size =
            4U + static_cast<std::uint64_t>(encoded.size());
        if (payload_bytes >
                static_cast<std::uint64_t>(limits.max_payload_bytes) ||
            framed_size >
                static_cast<std::uint64_t>(limits.max_payload_bytes) -
                    payload_bytes) {
            return storage_limit_failure(
                *storage, "typed frame exceeds the WAL payload limit");
        }
        payload_bytes += framed_size;
        records.push_back(std::move(encoded));
        return std::nullopt;
    };

    for (std::size_t index = 0U; index < commands.size(); ++index) {
        auto encoded = wire::encode(
            wire::Message{commands[index]}, encode_limits);
        if (!encoded) {
            return EncodedRecordsResult::failure(CommandLogError{
                EncodeFailure{encoded.error(), index}});
        }
        auto verified = wire::decode(encoded.value(), decode_limits);
        if (!verified) {
            return EncodedRecordsResult::failure(CommandLogError{
                DecodeFailure{verified.error(), index}});
        }
        if (auto error = append_record(std::move(encoded).value())) {
            return EncodedRecordsResult::failure(std::move(*error));
        }
    }

    auto encoded_seal = wire::encode(wire::Message{seal}, encode_limits);
    if (!encoded_seal) {
        return EncodedRecordsResult::failure(CommandLogError{
            EncodeFailure{encoded_seal.error(), commands.size()}});
    }
    auto verified_seal = wire::decode(encoded_seal.value(), decode_limits);
    if (!verified_seal) {
        return EncodedRecordsResult::failure(CommandLogError{
            DecodeFailure{verified_seal.error(), commands.size()}});
    }
    if (auto error = append_record(std::move(encoded_seal).value())) {
        return EncodedRecordsResult::failure(std::move(*error));
    }

    return EncodedRecordsResult::success(std::move(records));
}

using EncodedBatchResult =
    core::Result<SealedFrameBytes, CommandLogError>;

[[nodiscard]] EncodedBatchResult encode_batch(
    const SealedFrameLog& storage,
    const wire::DecodeLimits& decode_limits,
    std::span<const cmd::Command> commands,
    const cmd::FrameSeal& seal) {
    auto records = encode_records(&storage, decode_limits, commands, seal);
    if (!records) {
        return EncodedBatchResult::failure(std::move(records).error());
    }

    const auto first_command = commands.empty()
        ? storage.tail_cursor().next_command
        : commands.front().id.value();
    const auto next_command = commands.empty()
        ? first_command
        : commands.back().id.value() + 1U;
    return EncodedBatchResult::success(SealedFrameBytes{
        seal.frame.value(),
        seal.base.value(),
        seal.result.value(),
        first_command,
        next_command,
        std::move(records).value(),
    });
}

[[nodiscard]] core::Result<DecodedFrame, CommandLogError> decode_frame(
    const SealedFrameBytes& outer,
    const wire::DecodeLimits& limits) {
    if (outer.records.empty()) {
        return core::Result<DecodedFrame, CommandLogError>::failure(
            protocol_failure(CommandProtocolError::ExpectedSealRecord));
    }

    DecodedFrame decoded;
    decoded.commands.reserve(outer.records.size() - 1U);
    for (std::size_t index = 0U; index < outer.records.size(); ++index) {
        auto message = wire::decode(outer.records[index], limits);
        if (!message) {
            return core::Result<DecodedFrame, CommandLogError>::failure(CommandLogError{
                DecodeFailure{message.error(), index}});
        }

        const auto final_record = index + 1U == outer.records.size();
        if (!final_record) {
            const auto* command = std::get_if<cmd::Command>(&message.value());
            if (command == nullptr) {
                return core::Result<DecodedFrame, CommandLogError>::failure(
                    protocol_failure(CommandProtocolError::ExpectedCommandRecord, index));
            }
            decoded.commands.push_back(*command);
            continue;
        }

        const auto* seal = std::get_if<cmd::FrameSeal>(&message.value());
        if (seal == nullptr) {
            return core::Result<DecodedFrame, CommandLogError>::failure(
                protocol_failure(CommandProtocolError::ExpectedSealRecord, index));
        }
        decoded.seal = *seal;
    }

    if (const auto error = validate_batch(decoded.commands, decoded.seal)) {
        return core::Result<DecodedFrame, CommandLogError>::failure(
            CommandLogError{*error});
    }

    if (decoded.seal.frame.value() != outer.frame ||
        decoded.seal.base.value() != outer.base_revision ||
        decoded.seal.result.value() != outer.result_revision) {
        return core::Result<DecodedFrame, CommandLogError>::failure(
            protocol_failure(CommandProtocolError::OuterMetadataMismatch));
    }
    if (decoded.commands.empty()) {
        if (outer.first_command_id != outer.next_command_id) {
            return core::Result<DecodedFrame, CommandLogError>::failure(
                protocol_failure(CommandProtocolError::OuterMetadataMismatch));
        }
    } else {
        const auto first = decoded.commands.front().id.value();
        const auto last = decoded.commands.back().id.value();
        if (outer.first_command_id != first || last == std::numeric_limits<core::u64>::max()
            || outer.next_command_id != last + 1U) {
            return core::Result<DecodedFrame, CommandLogError>::failure(
                protocol_failure(CommandProtocolError::OuterMetadataMismatch));
        }
    }

    return core::Result<DecodedFrame, CommandLogError>::success(std::move(decoded));
}

[[nodiscard]] NodeFailure node_failure(
    NodeReplayOperation operation,
    cmd::FrameNumber frame,
    const cmd::NodeCommandBus& node,
    std::optional<cmd::CommandId> command = std::nullopt,
    std::optional<cmd::ReceiveStatus> receive_status = std::nullopt,
    std::optional<cmd::CommitStatus> commit_status = std::nullopt) {
    return NodeFailure{
        operation,
        frame,
        command,
        receive_status,
        commit_status,
        node.fault(),
    };
}

void observe_replay_history(
    ReplayHistoryReport& report,
    const cmd::NodeCommandBus& node) noexcept {
    const auto current = node.replay_history_diagnostics();
    report.peak.frame_states = std::max(
        report.peak.frame_states, current.frame_states);
    report.peak.seen_commands = std::max(
        report.peak.seen_commands, current.seen_commands);
    report.peak.prepared_frames = std::max(
        report.peak.prepared_frames, current.prepared_frames);
    report.peak.committed_prepared_frames = std::max(
        report.peak.committed_prepared_frames,
        current.committed_prepared_frames);
    report.peak.retained_roots = std::max(
        report.peak.retained_roots, current.retained_roots);
    report.final = current;
}

[[nodiscard]] BridgeStatus deliver_frame(
    const DecodedFrame& frame,
    const SealedFrameBytes& outer,
    cmd::NodeCommandBus& node) {
    for (const auto& command : frame.commands) {
        const auto status = node.receive(command);
        if (status != cmd::ReceiveStatus::Accepted) {
            return BridgeStatus::failure(CommandLogError{node_failure(
                NodeReplayOperation::ReceiveCommand,
                frame.seal.frame,
                node,
                command.id,
                status)});
        }
    }

    const auto seal_status = node.receive(frame.seal);
    if (seal_status != cmd::ReceiveStatus::Prepared) {
        return BridgeStatus::failure(CommandLogError{node_failure(
            NodeReplayOperation::ReceiveSeal,
            frame.seal.frame,
            node,
            std::nullopt,
            seal_status)});
    }

    const auto committed = node.commit_frame(frame.seal.frame);
    if (committed.status != cmd::CommitStatus::Committed) {
        return BridgeStatus::failure(CommandLogError{node_failure(
            NodeReplayOperation::CommitFrame,
            frame.seal.frame,
            node,
            std::nullopt,
            std::nullopt,
            committed.status)});
    }

    const auto expected_cursor = cmd::StreamCursor{
        frame.seal.frame,
        frame.seal.result,
        cmd::CommandId{outer.next_command_id},
    };
    if (committed.frame != frame.seal.frame || committed.version != frame.seal.result
        || committed.command_count != frame.commands.size()
        || !same_cursor(node.cursor(), expected_cursor)) {
        return BridgeStatus::failure(CommandLogError{node_failure(
            NodeReplayOperation::VerifyCommit,
            frame.seal.frame,
            node,
            std::nullopt,
            std::nullopt,
            committed.status)});
    }
    return BridgeStatus::success(std::monostate{});
}

[[nodiscard]] LogStatus stop_raw_replay() {
    return LogStatus::failure(LogError{
        LogErrorCode::VisitorFailed,
        0U,
        {},
        "typed command replay rejected a WAL frame",
    });
}

} // namespace

CommandLog::CommandLog(SealedFrameLog storage, wire::DecodeLimits decode_limits)
    : storage_(std::move(storage)), decode_limits_(decode_limits) {}

CommandLog::CommandLog(CommandLog&& other) noexcept
    : storage_(std::move(other.storage_)),
      decode_limits_(other.decode_limits_) {}

CommandLog& CommandLog::operator=(CommandLog&& other) noexcept {
    if (this != &other) {
        // Moving a published CommandLog concurrently with any operation is not
        // supported; the destination keeps its own synchronization primitive.
        storage_ = std::move(other.storage_);
        decode_limits_ = other.decode_limits_;
    }
    return *this;
}

core::Result<CommandLog, CommandLogError> CommandLog::open_or_create(
    std::filesystem::path path,
    cmd::StreamCursor base_cursor,
    OpenOptions options,
    wire::DecodeLimits decode_limits) {
    auto storage = SealedFrameLog::open_or_create(
        std::move(path), to_log_cursor(base_cursor), options);
    if (!storage) {
        return core::Result<CommandLog, CommandLogError>::failure(
            log_failure(storage.error()));
    }
    return core::Result<CommandLog, CommandLogError>::success(
        CommandLog{std::move(storage).value(), decode_limits});
}

core::Result<AppendReceipt, CommandLogError> CommandLog::append(
    std::span<const cmd::Command> commands,
    const cmd::FrameSeal& seal) {
    const std::lock_guard lock{mutex_};
    auto encoded = encode_batch(storage_, decode_limits_, commands, seal);
    if (!encoded) {
        return core::Result<AppendReceipt, CommandLogError>::failure(
            std::move(encoded).error());
    }

    auto appended = storage_.append(encoded.value());
    if (!appended) {
        return core::Result<AppendReceipt, CommandLogError>::failure(
            log_failure(appended.error()));
    }
    return core::Result<AppendReceipt, CommandLogError>::success(
        std::move(appended).value());
}

CommandLogStatus CommandLog::preflight(
    std::span<const cmd::Command> commands,
    const cmd::FrameSeal& seal) const {
    const std::lock_guard lock{mutex_};
    auto encoded = encode_batch(storage_, decode_limits_, commands, seal);
    if (!encoded) {
        return CommandLogStatus::failure(std::move(encoded).error());
    }
    auto storage_status = storage_.preflight(encoded.value());
    if (!storage_status) {
        return CommandLogStatus::failure(
            log_failure(std::move(storage_status).error()));
    }
    return CommandLogStatus::success(std::monostate{});
}

CommandLogStatus CommandLog::preflight_wire(
    std::span<const cmd::Command> commands,
    const cmd::FrameSeal& seal) const {
    const std::lock_guard lock{mutex_};
    auto encoded = encode_records(nullptr, decode_limits_, commands, seal);
    if (!encoded) {
        return CommandLogStatus::failure(std::move(encoded).error());
    }
    return CommandLogStatus::success(std::monostate{});
}

core::Result<ReplayReport, CommandLogError> CommandLog::replay_into(
    cmd::NodeCommandBus& node) const {
    return replay_into(node, ReplayRetention::Retain);
}

core::Result<ReplayReport, CommandLogError> CommandLog::replay_into(
    cmd::NodeCommandBus& node,
    ReplayRetention retention) const {
    ReplayReport report{0U, 0U, node.cursor(), {}};
    observe_replay_history(report.history, node);
    std::optional<CommandLogError> bridge_error;
    auto replay = storage_.replay_after(
        to_log_cursor(node.cursor()),
        [this, &node, &report, &bridge_error, retention](
            const SealedFrameBytes& outer) {
            auto decoded = decode_frame(outer, decode_limits_);
            if (!decoded) {
                bridge_error = decoded.error();
                return stop_raw_replay();
            }
            auto delivered = deliver_frame(decoded.value(), outer, node);
            if (!delivered) {
                bridge_error = delivered.error();
                return stop_raw_replay();
            }
            observe_replay_history(report.history, node);
            if (retention == ReplayRetention::RetireCommitted) {
                if (!node.retire_committed_replay_history()) {
                    bridge_error = CommandLogError{ReplayRetirementFailure{
                        decoded.value().seal.frame,
                        node.replay_history_diagnostics(),
                    }};
                    return stop_raw_replay();
                }
                ++report.history.retirements;
                observe_replay_history(report.history, node);
            }
            ++report.frames;
            report.commands += decoded.value().commands.size();
            report.cursor = node.cursor();
            return LogStatus::success(std::monostate{});
        });

    if (bridge_error) {
        return core::Result<ReplayReport, CommandLogError>::failure(
            std::move(*bridge_error));
    }
    if (!replay) {
        return core::Result<ReplayReport, CommandLogError>::failure(
            log_failure(replay.error()));
    }
    return core::Result<ReplayReport, CommandLogError>::success(report);
}

core::Result<BatchVisitReport, CommandLogError>
CommandLogSnapshot::visit_batches_after(
    cmd::StreamCursor from,
    cmd::StreamCursor target,
    const ValidatedCommandBatchVisitor& visitor) const {
    if (!visitor) {
        return core::Result<BatchVisitReport, CommandLogError>::failure(
            range_failure(LogErrorCode::VisitorFailed, "batch visitor is empty"));
    }

    std::optional<std::size_t> from_boundary;
    std::optional<std::size_t> target_boundary;
    const auto base = base_cursor();
    if (same_cursor(from, base)) {
        from_boundary = 0U;
    }
    if (same_cursor(target, base)) {
        target_boundary = 0U;
    }

    std::size_t boundary = 0U;
    auto preflight = storage_.replay_after(
        storage_.base_cursor(),
        [&from, &target, &from_boundary, &target_boundary, &boundary](
            const SealedFrameBytes& outer) {
            ++boundary;
            const auto after = cmd::StreamCursor{
                cmd::FrameNumber{outer.frame},
                doc::Version{outer.result_revision},
                cmd::CommandId{outer.next_command_id},
            };
            if (!from_boundary && same_cursor(from, after)) {
                from_boundary = boundary;
            }
            if (!target_boundary && same_cursor(target, after)) {
                target_boundary = boundary;
            }
            return LogStatus::success(std::monostate{});
        });
    if (!preflight) {
        return core::Result<BatchVisitReport, CommandLogError>::failure(
            log_failure(std::move(preflight).error()));
    }
    if (!from_boundary) {
        return core::Result<BatchVisitReport, CommandLogError>::failure(
            range_failure(
                LogErrorCode::CursorNotFound,
                "batch start cursor is not the base or a committed WAL boundary"));
    }
    if (!target_boundary) {
        return core::Result<BatchVisitReport, CommandLogError>::failure(
            range_failure(
                LogErrorCode::CursorNotFound,
                "batch target cursor is not the base or a committed WAL boundary"));
    }
    if (*from_boundary > *target_boundary) {
        return core::Result<BatchVisitReport, CommandLogError>::failure(
            range_failure(
                LogErrorCode::ChainMismatch,
                "batch start cursor follows its target cursor"));
    }
    if (*from_boundary == *target_boundary) {
        return core::Result<BatchVisitReport, CommandLogError>::success(
            BatchVisitReport{0U, 0U, from, true});
    }

    BatchVisitReport report{0U, 0U, from, false};
    std::optional<CommandLogError> bridge_error;
    bool intentional_stop = false;
    auto replay = storage_.replay_after_with_identity(
        to_log_cursor(from),
        [this,
         &target,
         &visitor,
         &report,
         &bridge_error,
         &intentional_stop](
            const SealedFrameBytes& outer,
            const WalFrameIdentity& identity) {
            auto decoded = decode_frame(outer, decode_limits_);
            if (!decoded) {
                bridge_error = std::move(decoded).error();
                return stop_raw_replay();
            }

            const auto after = cmd::StreamCursor{
                decoded.value().seal.frame,
                decoded.value().seal.result,
                cmd::CommandId{outer.next_command_id},
            };
            const auto control = visitor(ValidatedCommandBatchView{
                report.cursor,
                after,
                std::span<const cmd::Command>{decoded.value().commands},
                decoded.value().seal,
                identity,
            });

            ++report.frames;
            report.commands += decoded.value().commands.size();
            report.cursor = after;
            if (control == BatchVisitControl::Stop) {
                intentional_stop = true;
                return stop_raw_replay();
            }
            if (same_cursor(after, target)) {
                report.completed = true;
                intentional_stop = true;
                return stop_raw_replay();
            }
            return LogStatus::success(std::monostate{});
        });

    if (bridge_error) {
        return core::Result<BatchVisitReport, CommandLogError>::failure(
            std::move(*bridge_error));
    }
    if (!replay && !intentional_stop) {
        return core::Result<BatchVisitReport, CommandLogError>::failure(
            log_failure(std::move(replay).error()));
    }
    if (!intentional_stop) {
        if (!same_cursor(report.cursor, target)) {
            return core::Result<BatchVisitReport, CommandLogError>::failure(
                range_failure(
                    LogErrorCode::ChainMismatch,
                    "validated batch target was not reached during replay"));
        }
        report.completed = true;
    }
    return core::Result<BatchVisitReport, CommandLogError>::success(report);
}

core::Result<BatchVisitReport, CommandLogError>
CommandLogSnapshot::visit_committed_batches(
    const ValidatedCommandBatchVisitor& visitor) const {
    using Result = core::Result<BatchVisitReport, CommandLogError>;
    if (!visitor) {
        return Result::failure(
            range_failure(LogErrorCode::VisitorFailed, "batch visitor is empty"));
    }

    BatchVisitReport report{0U, 0U, base_cursor(), false};
    std::optional<CommandLogError> bridge_error;
    bool intentional_stop = false;
    auto visited = storage_.visit_committed(
        [this, &visitor, &report, &bridge_error, &intentional_stop](
            const SealedFrameBytes& outer,
            const WalFrameIdentity& identity) {
            auto decoded = decode_frame(outer, decode_limits_);
            if (!decoded) {
                bridge_error = std::move(decoded).error();
                return stop_raw_replay();
            }

            const auto after = cmd::StreamCursor{
                decoded.value().seal.frame,
                decoded.value().seal.result,
                cmd::CommandId{outer.next_command_id},
            };
            if (to_log_cursor(report.cursor) != identity.before
                || to_log_cursor(after) != identity.after) {
                bridge_error = protocol_failure(
                    CommandProtocolError::OuterMetadataMismatch);
                return stop_raw_replay();
            }

            const auto control = visitor(ValidatedCommandBatchView{
                report.cursor,
                after,
                std::span<const cmd::Command>{decoded.value().commands},
                decoded.value().seal,
                identity,
            });
            ++report.frames;
            report.commands += decoded.value().commands.size();
            report.cursor = after;
            if (control == BatchVisitControl::Stop) {
                intentional_stop = true;
                return stop_raw_replay();
            }
            return LogStatus::success(std::monostate{});
        });

    if (bridge_error) {
        return Result::failure(std::move(*bridge_error));
    }
    if (!visited && !intentional_stop) {
        return Result::failure(log_failure(std::move(visited).error()));
    }
    report.completed = !intentional_stop;
    if (report.completed && !same_cursor(report.cursor, tail_cursor())) {
        return Result::failure(range_failure(
            LogErrorCode::ChainMismatch,
            "single-pass batch visit did not reach the captured WAL tail"));
    }
    return Result::success(report);
}

cmd::StreamCursor CommandLogSnapshot::base_cursor() const noexcept {
    return to_stream_cursor(storage_.base_cursor());
}

cmd::StreamCursor CommandLogSnapshot::tail_cursor() const noexcept {
    return to_stream_cursor(storage_.tail_cursor());
}

core::Result<BatchVisitReport, CommandLogError> CommandLog::visit_batches_after(
    cmd::StreamCursor from,
    cmd::StreamCursor target,
    const ValidatedCommandBatchVisitor& visitor) const {
    auto snapshot = capture_committed();
    if (!snapshot) {
        return core::Result<BatchVisitReport, CommandLogError>::failure(
            std::move(snapshot).error());
    }
    return snapshot.value().visit_batches_after(from, target, visitor);
}

core::Result<CommandLogSnapshot, CommandLogError>
CommandLog::capture_committed() const {
    const std::lock_guard lock{mutex_};
    auto snapshot = storage_.capture_committed();
    if (!snapshot) {
        return core::Result<CommandLogSnapshot, CommandLogError>::failure(
            log_failure(std::move(snapshot).error()));
    }
    return core::Result<CommandLogSnapshot, CommandLogError>::success(
        CommandLogSnapshot{std::move(snapshot).value(), decode_limits_});
}

cmd::StreamCursor CommandLog::base_cursor() const noexcept {
    return to_stream_cursor(storage_.base_cursor());
}

cmd::StreamCursor CommandLog::tail_cursor() const {
    const std::lock_guard lock{mutex_};
    return to_stream_cursor(storage_.tail_cursor());
}

RecoveryReport CommandLog::recovery_report() const {
    const std::lock_guard lock{mutex_};
    return storage_.recovery_report();
}

} // namespace mediaserver::persist
