#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/cmd/loopback_transport.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/persist/command_log.hpp>
#include <mediaserver/persist/release_ledger.hpp>

namespace mediaserver::persist {

enum class DurableDirectorState : std::uint8_t {
    Ready,
    RecoveryRequired,
};

// How often the durable log is written, which is a separate question from how
// often a frame is sealed.
//
// Under restart-and-re-cue there is no requirement to resume an exact frame
// after a Director failure, so there is no reason to durably record that
// control frame 84,102 was empty. The seal, release and permit path stays
// intact in both modes: it is what makes two nodes agree, and that guarantee
// does not come from the disk.
enum class FrameDurability : std::uint8_t {
    // Every sealed frame is journalled and released before it may be
    // published. Correct, and bounded by the storage device's sync rate --
    // roughly 20-25 frames per second on a development machine.
    EveryFrame,
    // Frames are sealed, released and published in memory. The WAL and the
    // release ledger keep exactly what they held when this coordinator opened
    // them and are not appended to for the life of the run, so a restart
    // resumes the durable prefix rather than the live show.
    EditsOnly,
};

// Release decisions this coordinator can answer for after their barrier record
// has been retired. Fixed at construction so recording one allocates nothing,
// and bounded because an in-memory index that grows per frame is the leak this
// mode exists to remove. A catch-up older than the window takes a checkpoint.
inline constexpr std::size_t max_retained_memory_releases = 256U;

enum class DurableDirectorErrorCode : std::uint8_t {
    RecoveryRequired,
    NonDurablePolicy,
    CursorMismatch,
    SnapshotAllocationFailed,
    PrepareFailed,
    PrewriteRejected,
    PersistenceFailure,
    AbortFailed,
    PostAppendInvariant,
    CommitFailed,
    ResumeSourceUnavailable,
    ResumeRegistrationRejected,
    ReleasePending,
    ReleaseUnavailable,
    ReleasePrewriteRejected,
    ReleasePersistenceFailure,
    PostReleaseInvariant,
};

struct DurableDirectorError final {
    DurableDirectorErrorCode code;
    std::optional<cmd::SealError> seal_error;
    std::optional<CommandLogError> command_log_error;
    std::optional<ReleaseLedgerError> release_ledger_error;
};

class CommittedFrameBatch final {
public:
    CommittedFrameBatch(CommittedFrameBatch&& other) noexcept;
    CommittedFrameBatch& operator=(CommittedFrameBatch&& other) noexcept;
    CommittedFrameBatch(const CommittedFrameBatch&) = delete;
    CommittedFrameBatch& operator=(const CommittedFrameBatch&) = delete;
    ~CommittedFrameBatch() = default;

    [[nodiscard]] std::span<const cmd::Command> commands() const noexcept {
        return commands_;
    }
    [[nodiscard]] const cmd::FrameSeal& seal() const noexcept { return seal_; }
    [[nodiscard]] const AppendReceipt& receipt() const noexcept { return receipt_; }
    [[nodiscard]] const cmd::StreamCursor& before_cursor() const noexcept {
        return before_cursor_;
    }
    [[nodiscard]] cmd::StreamCursor after_cursor() const noexcept {
        return cmd::StreamCursor{
            cmd::FrameNumber{receipt_.cursor.through_frame},
            doc::Version{receipt_.cursor.revision},
            cmd::CommandId{receipt_.cursor.next_command},
        };
    }
private:
    friend class DurableDirector;

    CommittedFrameBatch(
        std::vector<cmd::Command> commands,
        cmd::FrameSeal seal,
        AppendReceipt receipt,
        cmd::StreamCursor before_cursor,
        std::uint64_t coordinator_id) noexcept;

    std::vector<cmd::Command> commands_;
    cmd::FrameSeal seal_;
    AppendReceipt receipt_;
    cmd::StreamCursor before_cursor_{};
    std::uint64_t coordinator_id_{};
    bool valid_{true};
};

// A resume owner uses this hook to install its live backlog at the exact WAL
// tail represented by a captured source. The hook runs while DurableDirector's
// transaction mutex is held, after the immutable WAL/checkpoint pair has been
// validated. Implementations must be noexcept, bounded, and must not perform
// network I/O or re-enter any DurableDirector method.
class ResumeSourceRegistration {
public:
    virtual ~ResumeSourceRegistration() = default;
    [[nodiscard]] virtual bool register_captured_tail(
        const cmd::StreamCursor& tail) noexcept = 0;
};

class DurableResumeSource final {
public:
    DurableResumeSource(DurableResumeSource&& other) noexcept;
    DurableResumeSource& operator=(DurableResumeSource&& other) noexcept;
    DurableResumeSource(const DurableResumeSource&) = delete;
    DurableResumeSource& operator=(const DurableResumeSource&) = delete;
    ~DurableResumeSource() = default;

    [[nodiscard]] const CommandLogSnapshot& log() const noexcept { return log_; }
    [[nodiscard]] const cmd::StreamCheckpoint& checkpoint() const noexcept {
        return checkpoint_;
    }

private:
    friend class DurableDirector;

    DurableResumeSource(
        CommandLogSnapshot log,
        cmd::StreamCheckpoint checkpoint,
        std::uint64_t coordinator_id) noexcept
        : log_(std::move(log)),
          checkpoint_(std::move(checkpoint)),
          coordinator_id_(coordinator_id) {}

    CommandLogSnapshot log_;
    cmd::StreamCheckpoint checkpoint_;
    std::uint64_t coordinator_id_{};
    bool valid_{true};
};

class DurableDirector final {
public:
    DurableDirector(
        cmd::DirectorScheduler& director,
        CommandLog& log,
        FrameDurability durability = FrameDurability::EveryFrame) noexcept;
    // Opt-in coherence gate. All dependencies are borrowed and must outlive
    // this coordinator. Construction fails closed when an existing WAL suffix
    // is not already covered by the ledger; normal in-process seals may create
    // a pending suffix that persist_release() subsequently advances.
    DurableDirector(
        cmd::DirectorScheduler& director,
        CommandLog& log,
        ReleaseLedger& release_ledger,
        FrameDurability durability = FrameDurability::EveryFrame) noexcept;
    DurableDirector(const DurableDirector&) = delete;
    DurableDirector& operator=(const DurableDirector&) = delete;
    DurableDirector(DurableDirector&&) = delete;
    DurableDirector& operator=(DurableDirector&&) = delete;

    [[nodiscard]] core::Result<CommittedFrameBatch, DurableDirectorError> seal_frame(
        cmd::FrameNumber frame);

    // Persists the next exact release decision. Foreign, stale, out-of-order,
    // and ordinal-mismatched capabilities are rejected before the ledger is
    // touched. Any failure after the ledger append protocol becomes ambiguous
    // latches RecoveryRequired, preventing all later publication.
    [[nodiscard]] core::Result<DurableReleaseReceipt, DurableDirectorError>
    persist_release(
        const CommittedFrameBatch& batch,
        std::uint64_t expected_ordinal);

    // The EditsOnly counterpart. It performs the same exact-identity,
    // exact-ordinal and contiguous-frontier checks persist_release() does and
    // then records the decision in the bounded in-memory index instead of the
    // ledger. It refuses outright under EveryFrame, so a caller cannot
    // downgrade a durable coordinator by choosing the wrong entry point.
    [[nodiscard]] bool record_release(
        const CommittedFrameBatch& batch,
        std::uint64_t expected_ordinal) noexcept;

    // Captures a value-owned immutable WAL prefix and its exact Director
    // checkpoint under the same transaction mutex used by seal_frame().
    [[nodiscard]] core::Result<DurableResumeSource, DurableDirectorError>
    capture_resume_source() const;

    // Atomically registers a live-backlog starting cursor with the captured
    // source. A rejected registration publishes neither the source nor a
    // partially usable subscription.
    [[nodiscard]] core::Result<DurableResumeSource, DurableDirectorError>
    capture_resume_source(ResumeSourceRegistration& registration) const;

    // Publication capabilities remain valid only while this exact coordinator
    // is Ready. Moved-from, foreign, and retained-after-failure batches fail.
    [[nodiscard]] bool authorizes(
        const CommittedFrameBatch& batch) const noexcept;

    // Captured resume sources are move-only capabilities bound to this exact
    // coordinator. This prevents a session from pairing another runtime's WAL
    // snapshot with the authority used for subsequent live publication.
    [[nodiscard]] bool authorizes(
        const DurableResumeSource& source) const noexcept;

    // Queues commands in canonical order followed by the seal. A coordinator
    // that has entered RecoveryRequired never publishes, even if the caller
    // retains an older committed batch.
    [[nodiscard]] bool broadcast_committed(
        const CommittedFrameBatch& batch,
        cmd::LoopbackTransport& transport) const;

    [[nodiscard]] DurableDirectorState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool recovery_required() const noexcept {
        return state() == DurableDirectorState::RecoveryRequired;
    }
    [[nodiscard]] bool release_gated() const noexcept {
        return release_ledger_ != nullptr;
    }
    [[nodiscard]] FrameDurability frame_durability() const noexcept {
        return durability_;
    }
    // Frames that carried commands and were sealed without being journalled.
    // Under EditsOnly the live seal path never writes, so an operator edit
    // authored during a run is replicated and released but not recorded in the
    // durable artifact. This counts them rather than letting the omission be
    // invisible; reconnecting the edit journal is the remaining half of the
    // durability-cadence decision.
    [[nodiscard]] std::uint64_t unjournalled_edit_frames() const noexcept;
    // The authoritative published tail, which is the WAL tail under EveryFrame
    // and runs ahead of it under EditsOnly.
    [[nodiscard]] cmd::StreamCursor published_tail() const noexcept;
    // Zero means that this coordinator cannot safely advance its release
    // lineage. Otherwise this is the next opaque, per-ledger monotonic
    // decision ordinal; callers must not derive it from a show-frame number.
    [[nodiscard]] std::uint64_t next_release_ordinal() const noexcept;
    // Exact historical lookup used to replay a durable permit to a catching-up
    // session after the in-memory barrier record has been retired.
    [[nodiscard]] std::optional<std::uint64_t> release_ordinal(
        const cmd::StreamCursor& before,
        const cmd::StreamCursor& after) const noexcept;

private:
    struct MemoryRelease final {
        cmd::StreamCursor before{};
        cmd::StreamCursor after{};
        std::uint64_t ordinal{};
    };

    void require_recovery() noexcept {
        state_.store(DurableDirectorState::RecoveryRequired, std::memory_order_release);
    }

    [[nodiscard]] core::Result<DurableResumeSource, DurableDirectorError>
    capture_resume_source_impl(ResumeSourceRegistration* registration) const;

    cmd::DirectorScheduler* director_;
    CommandLog* log_;
    ReleaseLedger* release_ledger_{};
    FrameDurability durability_{FrameDurability::EveryFrame};
    std::uint64_t coordinator_id_{};
    std::atomic<DurableDirectorState> state_{DurableDirectorState::Ready};
    mutable std::mutex transaction_mutex_;

    // Guarded by transaction_mutex_.
    cmd::StreamCursor published_tail_{};
    cmd::StreamCursor memory_release_frontier_{};
    std::uint64_t next_memory_release_ordinal_{1U};
    std::uint64_t unjournalled_edit_frames_{};
    std::array<MemoryRelease, max_retained_memory_releases> memory_releases_{};
    std::size_t memory_release_count_{};
    std::size_t memory_release_next_{};
};

enum class StartupRecoveryErrorCode : std::uint8_t {
    NonGenesisLog,
    ReplayFailed,
    CursorMismatch,
    ConstructionFailed,
};

struct StartupRecoveryError final {
    StartupRecoveryErrorCode code;
    std::optional<CommandLogError> command_log_error;
};

struct StartupResetReport final {
    // The v1 WAL reconstructs document/cursor state, not Director-local undo,
    // transaction, command, or historical-seal indexes.
    bool undo_and_transaction_history_reset{true};
    bool command_and_seal_history_reset{true};
};

struct RecoveredRuntime final {
    cmd::DirectorScheduler director;
    cmd::NodeCommandBus local_node;
    ReplayReport replay;
    StartupResetReport reset;
};

// Genesis-only v1 startup path. Replay occurs in a temporary unpublished node;
// no runtime is returned unless every committed frame validates and applies.
[[nodiscard]] core::Result<RecoveredRuntime, StartupRecoveryError>
recover_genesis_runtime(const CommandLog& log);

} // namespace mediaserver::persist
