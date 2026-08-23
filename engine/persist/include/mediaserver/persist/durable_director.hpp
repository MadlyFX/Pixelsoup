#pragma once

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
    DurableDirector(cmd::DirectorScheduler& director, CommandLog& log) noexcept;
    // Opt-in coherence gate. All dependencies are borrowed and must outlive
    // this coordinator. Construction fails closed when an existing WAL suffix
    // is not already covered by the ledger; normal in-process seals may create
    // a pending suffix that persist_release() subsequently advances.
    DurableDirector(
        cmd::DirectorScheduler& director,
        CommandLog& log,
        ReleaseLedger& release_ledger) noexcept;
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
    void require_recovery() noexcept {
        state_.store(DurableDirectorState::RecoveryRequired, std::memory_order_release);
    }

    [[nodiscard]] core::Result<DurableResumeSource, DurableDirectorError>
    capture_resume_source_impl(ResumeSourceRegistration* registration) const;

    cmd::DirectorScheduler* director_;
    CommandLog* log_;
    ReleaseLedger* release_ledger_{};
    std::uint64_t coordinator_id_{};
    std::atomic<DurableDirectorState> state_{DurableDirectorState::Ready};
    mutable std::mutex transaction_mutex_;
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
