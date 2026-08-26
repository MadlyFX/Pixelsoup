#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <variant>

#include <mediaserver/cmd/node_command_bus.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/persist/sealed_frame_log.hpp>
#include <mediaserver/wire/protocol_codec.hpp>

namespace mediaserver::persist {

inline constexpr std::size_t no_record_index = std::numeric_limits<std::size_t>::max();

struct LogFailure final {
    LogError error;
};

struct EncodeFailure final {
    wire::EncodeError error;
    std::size_t record_index{no_record_index};
};

struct DecodeFailure final {
    wire::DecodeError error;
    std::size_t record_index{no_record_index};
};

enum class CommandProtocolError : std::uint8_t {
    InvalidSeal,
    CommandCountMismatch,
    CommandIdentityMismatch,
    CommandFingerprintMismatch,
    CommandFrameMismatch,
    CommandRevisionMismatch,
    CommandIdExhausted,
    SealRevisionMismatch,
    ExpectedCommandRecord,
    ExpectedSealRecord,
    OuterMetadataMismatch,
};

struct ProtocolFailure final {
    CommandProtocolError error;
    std::size_t record_index{no_record_index};
};

enum class NodeReplayOperation : std::uint8_t {
    ReceiveCommand,
    ReceiveSeal,
    CommitFrame,
    VerifyCommit,
};

struct NodeFailure final {
    NodeReplayOperation operation;
    cmd::FrameNumber frame;
    std::optional<cmd::CommandId> command;
    std::optional<cmd::ReceiveStatus> receive_status;
    std::optional<cmd::CommitStatus> commit_status;
    std::optional<cmd::BusFault> fault;
};

struct ReplayRetirementFailure final {
    cmd::FrameNumber frame;
    cmd::ReplayHistoryDiagnostics history;
};

using CommandLogError = std::variant<
    LogFailure,
    EncodeFailure,
    DecodeFailure,
    ProtocolFailure,
    NodeFailure,
    ReplayRetirementFailure>;

using CommandLogStatus = core::Result<std::monostate, CommandLogError>;

enum class ReplayRetention : std::uint8_t {
    // Preserve the live-node duplicate and committed-frame history. This is
    // the original/default replay behavior.
    Retain,
    // Offline/unpublished nodes only: retire each frame immediately after its
    // successful commit and cursor verification.
    RetireCommitted,
};

struct ReplayHistoryReport final {
    cmd::ReplayHistoryDiagnostics peak{};
    cmd::ReplayHistoryDiagnostics final{};
    std::size_t retirements{};
};

struct ReplayReport final {
    std::size_t frames{};
    std::size_t commands{};
    cmd::StreamCursor cursor{};
    ReplayHistoryReport history{};
};

enum class BatchVisitControl : std::uint8_t {
    Continue,
    Stop,
};

struct ValidatedCommandBatchView final {
    cmd::StreamCursor before;
    cmd::StreamCursor after;
    std::span<const cmd::Command> commands;
    const cmd::FrameSeal& seal;
    WalFrameIdentity identity;
};

using ValidatedCommandBatchVisitor =
    std::function<BatchVisitControl(const ValidatedCommandBatchView&)>;

struct BatchVisitReport final {
    std::size_t frames{};
    std::size_t commands{};
    cmd::StreamCursor cursor{};
    bool completed{};
};

class CommandLogSnapshot final {
public:
    CommandLogSnapshot(CommandLogSnapshot&&) noexcept = default;
    CommandLogSnapshot& operator=(CommandLogSnapshot&&) noexcept = default;
    CommandLogSnapshot(const CommandLogSnapshot&) = delete;
    CommandLogSnapshot& operator=(const CommandLogSnapshot&) = delete;
    ~CommandLogSnapshot() = default;

    // Visits only the committed prefix captured with this value. Later appends
    // may grow the backing WAL but can neither extend nor invalidate this range.
    [[nodiscard]] core::Result<BatchVisitReport, CommandLogError>
    visit_batches_after(
        cmd::StreamCursor from,
        cmd::StreamCursor target,
        const ValidatedCommandBatchVisitor& visitor) const;

    // Recovery-oriented single-pass visit of the complete captured prefix.
    // Every callback receives the exact physical WAL entry identity after the
    // raw and typed frame invariants have both validated.
    [[nodiscard]] core::Result<BatchVisitReport, CommandLogError>
    visit_committed_batches(
        const ValidatedCommandBatchVisitor& visitor) const;

    [[nodiscard]] cmd::StreamCursor base_cursor() const noexcept;
    [[nodiscard]] cmd::StreamCursor tail_cursor() const noexcept;
    [[nodiscard]] std::uint64_t valid_bytes() const noexcept {
        return storage_.valid_bytes();
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return storage_.path();
    }

private:
    friend class CommandLog;

    CommandLogSnapshot(
        SealedFrameLogSnapshot storage,
        wire::DecodeLimits decode_limits) noexcept
        : storage_(std::move(storage)), decode_limits_(decode_limits) {}

    SealedFrameLogSnapshot storage_;
    wire::DecodeLimits decode_limits_{};
};

class CommandLog final {
public:
    [[nodiscard]] static core::Result<CommandLog, CommandLogError> open_or_create(
        std::filesystem::path path,
        cmd::StreamCursor base_cursor,
        OpenOptions options = {},
        wire::DecodeLimits decode_limits = {});

    CommandLog(CommandLog&& other) noexcept;
    CommandLog& operator=(CommandLog&& other) noexcept;
    CommandLog(const CommandLog&) = delete;
    CommandLog& operator=(const CommandLog&) = delete;
    ~CommandLog() = default;

    // Validates the complete cross-record batch, encodes commands in canonical
    // ID order, encodes the seal last, then performs one raw WAL append.
    [[nodiscard]] core::Result<AppendReceipt, CommandLogError> append(
        std::span<const cmd::Command> commands,
        const cmd::FrameSeal& seal);

    // Runs the exact typed protocol checks, wire encode/decode verification,
    // current-tail validation, and WAL record/payload limits used by append(),
    // but neither writes nor changes the file, cursor, recovery report, or
    // entry index. I/O can still fail after a successful preflight.
    [[nodiscard]] CommandLogStatus preflight(
        std::span<const cmd::Command> commands,
        const cmd::FrameSeal& seal) const;

    // Runs the typed protocol and canonical wire encode/decode checks used by
    // append(), including the configured message and sealed-command limits,
    // without applying WAL record, payload, index, or live-tail constraints.
    // This is the appropriate gate for an in-memory (EditsOnly) frame that is
    // still serialized for replication but is deliberately not journalled.
    [[nodiscard]] CommandLogStatus preflight_wire(
        std::span<const cmd::Command> commands,
        const cmd::FrameSeal& seal) const;

    // Starts at node.cursor(). Every WAL frame is fully decoded and checked
    // against its outer metadata before any message from that frame is given
    // to the node. Successfully prepared frames are committed immediately.
    [[nodiscard]] core::Result<ReplayReport, CommandLogError> replay_into(
        cmd::NodeCommandBus& node) const;

    // Recovery-only bounded-history replay. RetireCommitted is deliberately
    // opt-in because it releases the duplicate-detection history required by
    // a live node. Retirement happens after commit, so refusal is a distinct
    // CommandLogError variant and the unpublished node must be discarded.
    [[nodiscard]] core::Result<ReplayReport, CommandLogError> replay_into(
        cmd::NodeCommandBus& node,
        ReplayRetention retention) const;

    // Visits committed WAL batches in (from, target]. Both cursors must be
    // exact committed boundaries and are validated, including their ordering,
    // before the first callback. Each batch is completely decoded and checked
    // before it is exposed. The command span and seal reference remain valid
    // only for the duration of that callback. Stop is successful and returns
    // completed=false after counting the batch that requested it.
    [[nodiscard]] core::Result<BatchVisitReport, CommandLogError>
    visit_batches_after(
        cmd::StreamCursor from,
        cmd::StreamCursor target,
        const ValidatedCommandBatchVisitor& visitor) const;

    // Captures a value-owned immutable view of the current committed prefix.
    // The append/capture mutex is released before the snapshot can be visited.
    [[nodiscard]] core::Result<CommandLogSnapshot, CommandLogError>
    capture_committed() const;

    [[nodiscard]] cmd::StreamCursor base_cursor() const noexcept;
    [[nodiscard]] cmd::StreamCursor tail_cursor() const;
    [[nodiscard]] SyncPolicy sync_policy() const noexcept {
        return storage_.sync_policy();
    }
    [[nodiscard]] RecoveryReport recovery_report() const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return storage_.path();
    }

private:
    CommandLog(SealedFrameLog storage, wire::DecodeLimits decode_limits);

    SealedFrameLog storage_;
    wire::DecodeLimits decode_limits_{};
    mutable std::mutex mutex_;
};

} // namespace mediaserver::persist
