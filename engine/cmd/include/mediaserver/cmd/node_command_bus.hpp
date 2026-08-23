#pragma once

#include <compare>
#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/core/result.hpp>

namespace mediaserver::cmd {

enum class ReceiveStatus {
    Accepted,
    Duplicate,
    Stale,
    Prepared,
    CheckpointPending,
    NeedsResync,
};

enum class FaultCode {
    LateCommand,
    LateSeal,
    ConflictingCommand,
    ConflictingSeal,
    MalformedSeal,
    UnexpectedCommand,
    FingerprintMismatch,
    SealBaseMismatch,
    CommandFrameMismatch,
    CommandBaseMismatch,
    CommandResultMismatch,
    PayloadRejected,
    SealResultMismatch,
    NonConsecutiveCommit,
    FrameNotPrepared,
};

struct BusFault final {
    FaultCode code;
    FrameNumber frame;
    std::optional<CommandId> command;
};

enum class BatchReceiveErrorCode {
    // The equivalent per-message delivery would latch fault. The transactional
    // API returns that prospective fault without changing the node.
    WouldFault,
    AllocationFailed,
};

struct BatchReceiveError final {
    BatchReceiveErrorCode code;
    std::optional<BusFault> fault;
};

using BatchReceiveResult = core::Result<ReceiveStatus, BatchReceiveError>;

enum class CommitStatus {
    Committed,
    CheckpointPending,
    NeedsResync,
};

struct CommitResult final {
    CommitStatus status;
    FrameNumber frame;
    doc::Version version;
    std::size_t command_count;
};

struct ReplayHistoryDiagnostics final {
    std::size_t frame_states{};
    std::size_t seen_commands{};
    std::size_t prepared_frames{};
    std::size_t committed_prepared_frames{};
    std::size_t retained_roots{};

    bool operator==(const ReplayHistoryDiagnostics&) const = default;
};

enum class CheckpointPrepareError {
    PreparationOutstanding,
    GenerationExhausted,
    RevisionMismatch,
    InvalidNextCommand,
    FrameExhausted,
    AllocationFailed,
};

struct CheckpointPreparationToken final {
    FrameNumber through_frame;
    core::u64 generation{};

    [[nodiscard]] explicit operator bool() const noexcept { return generation != 0U; }
    auto operator<=>(const CheckpointPreparationToken&) const noexcept = default;
};

class NodeCommandBus final {
public:
    explicit NodeCommandBus(
        doc::Root initial = {},
        FrameNumber committed_through = FrameNumber{0U},
        CommandId next_command = CommandId{1U});

    [[nodiscard]] ReceiveStatus receive(Command command);
    [[nodiscard]] ReceiveStatus receive(FrameSeal seal);

    // Atomically records one already-decoded complete frame. All validation,
    // reduction, and allocation is staged before the existing traffic state is
    // changed. On error, document/cursor/fault, buffered traffic, prepared
    // roots/pointers, and checkpoint state are unchanged. Normal no-mutation
    // outcomes (Duplicate, Stale, CheckpointPending, and NeedsResync) retain
    // their existing ReceiveStatus values.
    [[nodiscard]] BatchReceiveResult receive_complete_batch(
        std::span<const Command> commands,
        const FrameSeal& seal) noexcept;

    // Frame-boundary operation. All document reduction and allocation happens
    // during receive(); this method only publishes an already-prepared Root.
    [[nodiscard]] CommitResult commit_frame(FrameNumber frame) noexcept;

    // Offline/replay-only maintenance boundary. Live nodes intentionally keep
    // duplicate-detection history by default. Retirement succeeds only when
    // every buffered frame has committed, then releases committed frame,
    // command, prepared-pointer, and retained-root history without changing
    // the current document, cursor, preparation head, next IDs, or fault.
    [[nodiscard]] bool retire_committed_replay_history() noexcept;

    [[nodiscard]] ReplayHistoryDiagnostics replay_history_diagnostics() const noexcept {
        return ReplayHistoryDiagnostics{
            frames_.size(),
            seen_commands_.size(),
            prepared_order_.size(),
            commit_cursor_,
            retained_roots_.size(),
        };
    }

    // Builds every replacement container before exposing a token. The current
    // document, cursor, fault, and buffered traffic remain unchanged until an
    // exact-token activation. Traffic is blocked while a checkpoint is staged.
    [[nodiscard]] core::Result<CheckpointPreparationToken, CheckpointPrepareError>
    prepare_checkpoint(StreamCheckpoint checkpoint);
    [[nodiscard]] bool activate_prepared_checkpoint(
        CheckpointPreparationToken token) noexcept;
    [[nodiscard]] bool abort_prepared_checkpoint(
        CheckpointPreparationToken token) noexcept;
    [[nodiscard]] const StreamCheckpoint* prepared_checkpoint() const noexcept;

    // Compatibility wrapper. It never activates or aborts a checkpoint staged
    // by another caller.
    [[nodiscard]] bool install_checkpoint(StreamCheckpoint checkpoint);

    [[nodiscard]] const doc::Root& document() const noexcept { return document_; }
    [[nodiscard]] FrameNumber committed_through() const noexcept { return committed_through_; }
    [[nodiscard]] StreamCursor cursor() const noexcept {
        return StreamCursor{committed_through_, document_.version(), next_command_};
    }
    [[nodiscard]] bool is_prepared(FrameNumber frame) const noexcept;
    [[nodiscard]] bool needs_resync() const noexcept { return fault_.has_value(); }
    [[nodiscard]] const std::optional<BusFault>& fault() const noexcept { return fault_; }

private:
    struct FrameState final {
        std::optional<FrameSeal> seal;
        std::map<CommandId, Command> commands;
        std::optional<doc::Root> prepared;
        bool committed{};
    };

    struct PreparedCheckpointState final {
        CheckpointPreparationToken token{};
        StreamCheckpoint checkpoint{};
        doc::Root preparation_head{};
        FrameNumber next_to_prepare{};
        CommandId preparation_next_command{};
        std::map<FrameNumber, FrameState> frames;
        std::map<CommandId, Command> seen_commands;
        std::vector<doc::Root> retained_roots;
        std::vector<FrameState*> prepared_order;
    };

    void try_prepare();
    void latch_fault(
        FaultCode code,
        FrameNumber frame,
        std::optional<CommandId> command = std::nullopt) noexcept;
    [[nodiscard]] static const SealedCommand* find_descriptor(
        const FrameSeal& seal,
        CommandId id) noexcept;

    doc::Root document_;
    doc::Root preparation_head_;
    FrameNumber committed_through_;
    FrameNumber next_to_prepare_;
    CommandId next_command_{1U};
    CommandId preparation_next_command_{1U};
    std::map<FrameNumber, FrameState> frames_;
    std::map<CommandId, Command> seen_commands_;
    std::vector<doc::Root> retained_roots_;
    std::vector<FrameState*> prepared_order_;
    std::size_t commit_cursor_{};
    std::optional<BusFault> fault_;
    core::u64 checkpoint_preparation_generation_{};
    std::optional<PreparedCheckpointState> prepared_checkpoint_;
};

} // namespace mediaserver::cmd
