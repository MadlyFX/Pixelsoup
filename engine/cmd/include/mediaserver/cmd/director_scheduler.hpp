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

// Bounds on the indexes the Director keeps purely for authoring. None of them
// is needed to seal the next frame or to describe the current document; they
// exist so an operator can query recent history and undo recent work. Left
// unbounded they grow once per sealed frame, which a show running for hours at
// 120 fps turns into a leak.
struct DirectorSchedulerLimits final {
    // Sealed frames whose command list and seal stay queryable. Frames behind
    // this window are retired as later ones commit. Zero retains everything,
    // which is what an offline tool replaying a whole log wants.
    std::size_t retained_sealed_frames{256U};

    // Transactions retained per undo context, in creation order. Pushing past
    // this depth drops the oldest, so undo reaches back a bounded distance
    // rather than to the start of the show. Zero retains everything.
    std::size_t max_undo_transactions{256U};

    bool operator==(const DirectorSchedulerLimits&) const = default;
};

// What the authoring indexes currently hold. Exposed so a long run can assert
// flat memory rather than describe it.
struct DirectorAuthoringFootprint final {
    std::size_t frames_with_commands{};
    std::size_t retained_seals{};
    std::size_t undo_contexts{};
    std::size_t undo_transactions{};

    bool operator==(const DirectorAuthoringFootprint&) const = default;
};

class DirectorScheduler final {
public:
    explicit DirectorScheduler(
        doc::Root initial = {},
        FrameNumber sealed_through = FrameNumber{0U},
        CommandId next_command = CommandId{1U},
        DirectorSchedulerLimits limits = {});

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
    [[nodiscard]] DirectorAuthoringFootprint authoring_footprint() const noexcept;
    [[nodiscard]] const DirectorSchedulerLimits& limits() const noexcept {
        return limits_;
    }

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

    // Records a freshly created history key and drops the oldest transactions
    // in that context once the retained depth is exceeded. A dropped
    // transaction leaves the undo stack too, so undo_depth() never reports a
    // transaction whose commands have been released.
    void retain_history(UndoContextId context, TransactionId transaction);

    // Drops committed command lists and seals behind the retention window.
    // Runs after the cursor advances, so the frame just sealed always stays.
    void retire_sealed_history() noexcept;

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
    // Creation order of every retained history key in a context, which is what
    // decides retirement. The undo stack cannot serve that purpose: an undone
    // transaction leaves the stack but its commands stay reachable.
    std::map<UndoContextId, std::vector<TransactionId>> history_order_;
    DirectorSchedulerLimits limits_{};
    core::u64 preparation_generation_{};
    std::optional<PreparedSealState> prepared_seal_;
};

} // namespace mediaserver::cmd
