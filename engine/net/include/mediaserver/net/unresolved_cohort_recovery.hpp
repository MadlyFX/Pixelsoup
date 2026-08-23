#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include <mediaserver/core/result.hpp>
#include <mediaserver/persist/cohort_journal.hpp>
#include <mediaserver/persist/recovery_release_authority.hpp>

namespace mediaserver::net {

struct RecoverySessionToken final {
    std::uint64_t high{};
    std::uint64_t low{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return high != 0U || low != 0U;
    }
    bool operator==(const RecoverySessionToken&) const = default;
};

struct RecoveryActionToken final {
    std::uint64_t value{};

    [[nodiscard]] explicit operator bool() const noexcept { return value != 0U; }
    bool operator==(const RecoveryActionToken&) const = default;
};

struct RecoveryWalFrame final {
    persist::WalFrameIdentity identity;
    std::vector<cmd::Command> commands;
    cmd::FrameSeal seal;
};

struct RecoveryActivePlan final {
    persist::WalFrameIdentity frame;
    std::uint64_t generation{};
    persist::RecoveryPlanToken token;
    std::uint64_t fingerprint{};
    std::uint64_t journal_ordinal{};
    persist::RecoveryCohort cohort;
    persist::EmptyCohortConfirmation empty_confirmation{
        persist::EmptyCohortConfirmation::None};
    std::uint64_t last_attempt_generation{};
};

struct RecoveryAttemptRequest final {
    persist::RecoveryAttemptToken attempt_token;
    persist::RecoveryDirectorEpoch director_epoch;
    persist::RecoverySessionNamespace session_namespace;
};

struct RecoveryCheckpointAction final {
    RecoveryActionToken action;
    RecoverySessionToken session;
    persist::RecoveryCohortMember member;
    RecoveryAttemptRequest attempt;
    std::shared_ptr<const cmd::StreamCheckpoint> checkpoint;
};

struct RecoveryPrepareAction final {
    RecoveryActionToken action;
    RecoverySessionToken session;
    persist::RecoveryCohortMember member;
    RecoveryAttemptRequest attempt;
    std::shared_ptr<const RecoveryWalFrame> frame;
};

using RecoveryAction = std::variant<
    RecoveryCheckpointAction,
    RecoveryPrepareAction>;

struct RecoveryCheckpointCompletion final {
    RecoveryActionToken action;
    RecoverySessionToken session;
    cmd::StreamCursor activated_cursor;
    bool activated{};
};

enum class RecoveryFrameAcknowledgementKind : std::uint8_t {
    Prepared,
    Committed,
};

struct RecoveryPrepareCompletion final {
    RecoveryActionToken action;
    RecoverySessionToken session;
    RecoveryFrameAcknowledgementKind kind{
        RecoveryFrameAcknowledgementKind::Prepared};
    persist::WalFrameIdentity frame;
    cmd::StreamCursor before;
    cmd::StreamCursor after;
};

enum class UnresolvedCohortRecoveryState : std::uint8_t {
    Ready,
    Admitting,
    Checkpointing,
    Preparing,
    PreparedQuorum,
    PersistingRelease,
    // Recovery deliberately has no transition to normal Live. Every durable
    // release ends this coordinator and requires a fresh process startup. If
    // more WAL suffix remains, the next recovery process handles its front.
    RestartRequired,
    Faulted,
};

enum class UnresolvedCohortRecoveryErrorCode : std::uint8_t {
    InvalidInventory,
    NoUnresolvedSuffix,
    NonGenesisBase,
    SnapshotMismatch,
    CheckpointBuildFailed,
    JournalMismatch,
    MissingPlan,
    DuplicatePlan,
    InvalidState,
    InvalidAttempt,
    JournalFailure,
    MemberNotInCohort,
    DuplicateMember,
    DuplicateSession,
    CohortIncomplete,
    ActionExhausted,
    ActionMismatch,
    CheckpointMismatch,
    AcknowledgementMismatch,
    ReleaseFailure,
    ReleaseReceiptMismatch,
    AllocationFailure,
};

struct UnresolvedCohortRecoveryError final {
    UnresolvedCohortRecoveryErrorCode code{
        UnresolvedCohortRecoveryErrorCode::InvalidInventory};
    std::optional<persist::CommandLogError> command_log_error;
    std::optional<persist::CohortJournalError> journal_error;
    std::optional<persist::RecoveryReleaseFailure> release_error;
};

struct UnresolvedCohortRecoveryReport final {
    UnresolvedCohortRecoveryState state{
        UnresolvedCohortRecoveryState::Faulted};
    persist::LogCursor release_frontier{};
    persist::LogCursor wal_tail{};
    persist::WalFrameIdentity frame{};
    std::uint64_t release_ordinal{};
    std::uint64_t plan_generation{};
    std::uint64_t attempt_generation{};
    std::size_t cohort_members{};
    std::size_t admitted{};
    std::size_t checkpointed{};
    std::size_t prepared{};
};

namespace detail {
struct UnresolvedCohortRecoveryControl;
} // namespace detail

// Caller-serialized recovery state machine for exactly the first unresolved
// frame of one immutable WAL snapshot. A multi-frame suffix is recovered by
// repeated clean recovery-process starts, one durable front release at a time.
// It performs no socket I/O. The caller
// takes an action, performs the blocking transport operation without any
// coordinator lock held, then submits the exact completion. There is no permit
// action or authoring entry point.
class UnresolvedCohortRecovery final {
public:
    [[nodiscard]] static core::Result<
        UnresolvedCohortRecovery,
        UnresolvedCohortRecoveryError>
    create(
        persist::CommandLogSnapshot wal,
        persist::UnresolvedReleaseInventory inventory,
        persist::CohortJournal& journal,
        persist::RecoveryReleaseAuthority& release_authority);

    UnresolvedCohortRecovery(UnresolvedCohortRecovery&&) noexcept;
    UnresolvedCohortRecovery& operator=(UnresolvedCohortRecovery&&) noexcept;
    UnresolvedCohortRecovery(const UnresolvedCohortRecovery&) = delete;
    UnresolvedCohortRecovery& operator=(const UnresolvedCohortRecovery&) = delete;
    ~UnresolvedCohortRecovery();

    // Reconfiguration is accepted only before an attempt. The supplied record
    // carries CohortJournal's complete old-plan/frontier/tail CAS. Exact
    // idempotent operator retries retain the journal's normal replay semantics.
    [[nodiscard]] core::Result<
        persist::CohortJournalReceipt,
        UnresolvedCohortRecoveryError>
    reconfigure(const persist::ReconfigureFrame& mutation);

    // The coordinator fills all inventory and active-plan fields. The durable
    // AttemptStarted append completes before admission or action generation.
    [[nodiscard]] core::Result<
        persist::CohortJournalReceipt,
        UnresolvedCohortRecoveryError>
    start_attempt(const RecoveryAttemptRequest& request);

    [[nodiscard]] core::Result<std::monostate, UnresolvedCohortRecoveryError>
    admit(
        const persist::RecoveryCohortMember& member,
        RecoverySessionToken session);

    // Empty means the exact cohort is not fully admitted or every currently
    // eligible member already has an action in flight. Independent member
    // actions may run concurrently. Once every checkpoint completes, only
    // Prepared-frame actions are emitted.
    [[nodiscard]] core::Result<
        std::optional<RecoveryAction>,
        UnresolvedCohortRecoveryError>
    take_next_action();

    [[nodiscard]] core::Result<std::monostate, UnresolvedCohortRecoveryError>
    complete_checkpoint(const RecoveryCheckpointCompletion& completion);
    [[nodiscard]] core::Result<std::monostate, UnresolvedCohortRecoveryError>
    complete_prepare(const RecoveryPrepareCompletion& completion);

    // A transport failure abandons only the in-memory attempt. A later attempt
    // must append the next durable AttemptStarted generation before new I/O.
    [[nodiscard]] core::Result<std::monostate, UnresolvedCohortRecoveryError>
    fail_attempt() noexcept;

    // Valid only at exact Prepared quorum. A durable success never emits a
    // permit and always transitions to RestartRequired.
    [[nodiscard]] core::Result<
        persist::DurableReleaseReceipt,
        UnresolvedCohortRecoveryError>
    persist_release();

    [[nodiscard]] UnresolvedCohortRecoveryState state() const noexcept;
    [[nodiscard]] const persist::UnresolvedReleaseInventory& inventory()
        const noexcept;
    [[nodiscard]] const RecoveryActivePlan& active_plan() const noexcept;
    [[nodiscard]] const cmd::StreamCheckpoint& checkpoint() const noexcept;
    [[nodiscard]] const RecoveryWalFrame& frame() const noexcept;
    [[nodiscard]] UnresolvedCohortRecoveryReport report() const noexcept;

private:
    explicit UnresolvedCohortRecovery(
        std::unique_ptr<detail::UnresolvedCohortRecoveryControl> control) noexcept;

    std::unique_ptr<detail::UnresolvedCohortRecoveryControl> control_;
};

} // namespace mediaserver::net
