#pragma once

#include <compare>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/core/result.hpp>

namespace mediaserver::cmd {

enum class ScheduleError {
    SealPrepared,
    FrameSealed,
    FrameRegression,
    CommandIdExhausted,
    VersionExhausted,
    ForwardRejected,
    InvalidInverse,
    TransactionNotTop,
};

enum class SealError {
    PrepareOutstanding,
    PreparationTokenExhausted,
    NonConsecutiveFrame,
    VersionChainMismatch,
    SealHistoryConflict,
};

enum class DirectorUndoError {
    SealPrepared,
    NoHistory,
    DuplicateTransaction,
    HistoryConflict,
    FrameRejected,
    CommandIdExhausted,
    VersionExhausted,
};

struct SealPreparationToken final {
    FrameNumber frame;
    core::u64 generation{};

    [[nodiscard]] explicit operator bool() const noexcept { return generation != 0U; }
    auto operator<=>(const SealPreparationToken&) const noexcept = default;
};

struct SealPreparation final {
    FrameSeal seal;
    SealPreparationToken token;

    bool operator==(const SealPreparation&) const = default;
};

class DirectorScheduler final {
public:
    explicit DirectorScheduler(
        doc::Root initial = {},
        FrameNumber sealed_through = FrameNumber{0U},
        CommandId next_command = CommandId{1U});

    [[nodiscard]] core::Result<Command, ScheduleError> schedule_edit(
        Payload forward,
        Payload inverse,
        UndoContextId undo_context,
        TransactionId transaction,
        FrameNumber apply_at);

    [[nodiscard]] core::Result<std::vector<Command>, DirectorUndoError> schedule_undo(
        UndoContextId undo_context,
        TransactionId undo_transaction,
        FrameNumber apply_at);

    // Stages the next consecutive frame without changing the committed cursor,
    // document, or visible seal history. No scheduling mutation is accepted
    // until the staged seal is committed or aborted.
    [[nodiscard]] core::Result<SealPreparation, SealError> prepare_seal(FrameNumber frame);

    // The allocation and validation work is completed by prepare_seal(). A
    // commit with the exact transient token only publishes the staged state
    // and cannot fail because of allocation or I/O. A stale token returns
    // false and leaves the current prepared seal latched.
    [[nodiscard]] bool commit_prepared_seal(SealPreparationToken token) noexcept;
    [[nodiscard]] bool abort_prepared_seal(SealPreparationToken token) noexcept;
    [[nodiscard]] const FrameSeal* prepared_seal() const noexcept;

    // In-memory convenience operation equivalent to prepare followed by
    // commit. It never commits a seal prepared by a different caller.
    [[nodiscard]] core::Result<FrameSeal, SealError> seal_frame(FrameNumber frame);

    [[nodiscard]] std::span<const Command> commands_for(FrameNumber frame) const noexcept;
    [[nodiscard]] const FrameSeal* seal_for(FrameNumber frame) const noexcept;
    [[nodiscard]] StreamCheckpoint checkpoint() const;
    [[nodiscard]] const doc::Root& sealed_document() const noexcept {
        return sealed_document_;
    }
    [[nodiscard]] const doc::Root& projected_document() const noexcept { return projected_; }
    [[nodiscard]] FrameNumber sealed_through() const noexcept { return sealed_through_; }
    [[nodiscard]] std::size_t undo_depth(UndoContextId context) const noexcept;

private:
    struct HistoryKey final {
        UndoContextId context;
        TransactionId transaction;

        auto operator<=>(const HistoryKey&) const noexcept = default;
    };

    struct PreparedSealState final {
        SealPreparationToken token;
        doc::Version result;
        doc::Root document;
        CommandId next_command;
    };

    [[nodiscard]] bool frame_is_schedulable(FrameNumber frame) const noexcept;

    doc::Root sealed_document_;
    doc::Root projected_;
    FrameNumber sealed_through_;
    doc::Version sealed_version_;
    std::optional<FrameNumber> last_scheduled_frame_;
    core::u64 next_command_id_{1U};
    CommandId sealed_next_command_{1U};
    std::map<FrameNumber, std::vector<Command>> commands_;
    std::map<FrameNumber, FrameSeal> seals_;
    std::map<HistoryKey, std::vector<Command>> history_;
    std::map<UndoContextId, std::vector<TransactionId>> undo_stacks_;
    core::u64 preparation_generation_{};
    std::optional<PreparedSealState> prepared_seal_;
};

} // namespace mediaserver::cmd
