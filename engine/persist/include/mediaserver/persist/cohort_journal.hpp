#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <mediaserver/core/result.hpp>
#include <mediaserver/persist/unresolved_release_inventory.hpp>

namespace mediaserver::persist {

template <typename Tag>
struct CohortJournalId128 final {
    std::uint64_t high{};
    std::uint64_t low{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return high != 0U || low != 0U;
    }
    bool operator==(const CohortJournalId128&) const = default;
};

using CohortJournalUuid =
    CohortJournalId128<struct CohortJournalUuidTag>;
using RecoveryShowId = CohortJournalId128<struct RecoveryShowIdTag>;
using RecoveryNodeId = CohortJournalId128<struct RecoveryNodeIdTag>;
using RecoveryNodeIncarnation =
    CohortJournalId128<struct RecoveryNodeIncarnationTag>;
using RecoveryPlanToken = CohortJournalId128<struct RecoveryPlanTokenTag>;
using RecoveryOperatorRequestId =
    CohortJournalId128<struct RecoveryOperatorRequestIdTag>;
using RecoveryAttemptToken =
    CohortJournalId128<struct RecoveryAttemptTokenTag>;
using RecoveryDirectorEpoch =
    CohortJournalId128<struct RecoveryDirectorEpochTag>;
using RecoverySessionNamespace =
    CohortJournalId128<struct RecoverySessionNamespaceTag>;

struct RecoveryCohortMember final {
    RecoveryNodeId node_id;
    RecoveryNodeIncarnation incarnation;

    bool operator==(const RecoveryCohortMember&) const = default;
};

using RecoveryCohort = std::vector<RecoveryCohortMember>;

enum class EmptyCohortConfirmation : std::uint8_t {
    None = 0U,
    ExplicitlyEmpty = 1U,
};

enum class AbortFrameReason : std::uint8_t {
    OperatorCancelled = 1U,
    Superseded = 2U,
    PlanInvalidated = 3U,
};

enum class AdoptionProvenance : std::uint8_t {
    LegacyUncovered = 1U,
};

enum class ReconfigureFrameReason : std::uint8_t {
    NodeUnavailable = 1U,
    IncarnationLost = 2U,
    OperatorOverride = 3U,
};

struct BeginFrame final {
    RecoveryPlanToken plan_token;
    LogCursor expected_before{};
    std::uint64_t next_frame{};
    RecoveryDirectorEpoch cut_director_epoch;
    std::uint64_t plan_generation{1U};
    std::uint64_t plan_fingerprint{};
    RecoveryCohort cohort;
    EmptyCohortConfirmation empty_confirmation{
        EmptyCohortConfirmation::None};

    bool operator==(const BeginFrame&) const = default;
};

struct BindFrame final {
    RecoveryPlanToken begin_token;
    std::uint64_t begin_ordinal{};
    WalFrameIdentity frame{};
    std::uint64_t plan_generation{};
    RecoveryPlanToken plan_token;
    std::uint64_t plan_fingerprint{};

    bool operator==(const BindFrame&) const = default;
};

struct AbortFrame final {
    RecoveryPlanToken begin_token;
    std::uint64_t begin_ordinal{};
    AbortFrameReason reason{AbortFrameReason::OperatorCancelled};

    bool operator==(const AbortFrame&) const = default;
};

struct AdoptFrame final {
    WalFrameIdentity frame{};
    RecoveryOperatorRequestId operator_request_id;
    AdoptionProvenance provenance{AdoptionProvenance::LegacyUncovered};
    std::uint64_t plan_generation{1U};
    RecoveryPlanToken plan_token;
    std::uint64_t plan_fingerprint{};
    RecoveryCohort cohort;
    EmptyCohortConfirmation empty_confirmation{
        EmptyCohortConfirmation::None};

    bool operator==(const AdoptFrame&) const = default;
};

struct ReconfigureFrame final {
    WalFrameIdentity frame{};
    std::uint64_t expected_plan_generation{};
    RecoveryPlanToken expected_plan_token;
    std::uint64_t expected_plan_fingerprint{};
    std::uint64_t expected_plan_journal_ordinal{};
    LogCursor expected_release_frontier{};
    LogCursor expected_wal_tail{};
    std::uint64_t new_plan_generation{};
    RecoveryPlanToken new_plan_token;
    std::uint64_t new_plan_fingerprint{};
    RecoveryOperatorRequestId operator_request_id;
    ReconfigureFrameReason reason{ReconfigureFrameReason::NodeUnavailable};
    RecoveryCohort cohort;
    EmptyCohortConfirmation empty_confirmation{
        EmptyCohortConfirmation::None};

    bool operator==(const ReconfigureFrame&) const = default;
};

struct AttemptStarted final {
    WalFrameIdentity frame{};
    std::uint64_t attempt_generation{};
    RecoveryAttemptToken attempt_token;
    RecoveryDirectorEpoch director_epoch;
    RecoverySessionNamespace session_namespace;
    LogCursor release_frontier{};
    LogCursor wal_tail{};
    std::uint64_t active_plan_generation{};
    RecoveryPlanToken active_plan_token;
    std::uint64_t active_plan_journal_ordinal{};

    bool operator==(const AttemptStarted&) const = default;
};

using CohortJournalMutation = std::variant<
    BeginFrame,
    BindFrame,
    AbortFrame,
    AdoptFrame,
    ReconfigureFrame,
    AttemptStarted>;

enum class CohortJournalRecordKind : std::uint8_t {
    BeginFrame = 1U,
    BindFrame = 2U,
    AbortFrame = 3U,
    AdoptFrame = 4U,
    ReconfigureFrame = 5U,
    AttemptStarted = 6U,
};

struct CohortJournalRecord final {
    std::uint64_t ordinal{};
    std::uint64_t offset{};
    std::uint64_t bytes{};
    std::uint32_t record_crc32c{};
    CohortJournalMutation mutation;

    bool operator==(const CohortJournalRecord&) const = default;
};

struct CohortJournalBinding final {
    CohortJournalUuid journal_uuid;
    RecoveryShowId show_id;
    LogCursor wal_base{};
    LogCursor anchor_release_cursor{};
    std::uint64_t anchor_wal_byte_offset{};
    std::uint64_t release_decision_count{};
    std::uint64_t release_ledger_valid_bytes{};

    bool operator==(const CohortJournalBinding&) const = default;
};

struct CohortJournalLimits final {
    std::uint32_t max_records{65'536U};
    std::uint32_t max_cohort_members{4'096U};
    std::uint32_t max_record_bytes{1024U * 1024U};
    std::uint64_t max_file_bytes{256U * 1024U * 1024U};
};

struct CohortJournalOpenOptions final {
    SyncPolicy sync_policy{SyncPolicy::Full};
    bool repair_torn_tail{true};
    CohortJournalLimits limits{};
};

enum class CohortJournalErrorCode : std::uint8_t {
    NonDurablePolicy,
    InvalidArgument,
    UncoveredWalSuffix,
    OpenFailed,
    ReadFailed,
    WriteFailed,
    SyncFailed,
    TruncateFailed,
    InvalidHeader,
    UnsupportedVersion,
    BindingMismatch,
    TornTail,
    CorruptRecord,
    ChainMismatch,
    InvalidRecord,
    StaleWrite,
    RequestConflict,
    LimitExceeded,
    Unusable,
};

struct CohortJournalError final {
    CohortJournalErrorCode code{CohortJournalErrorCode::OpenFailed};
    std::uint64_t offset{};
    std::error_code system_error{};
    std::string detail;
};

struct CohortJournalRecoveryReport final {
    std::uint64_t record_count{};
    std::uint64_t valid_bytes{};
    std::uint64_t truncated_bytes{};
    std::uint64_t last_ordinal{};
    std::uint32_t last_record_crc32c{};
    bool repaired_torn_tail{};
};

class CohortJournalReceipt final {
public:
    CohortJournalReceipt(CohortJournalReceipt&& other) noexcept;
    CohortJournalReceipt& operator=(CohortJournalReceipt&& other) noexcept;
    CohortJournalReceipt(const CohortJournalReceipt&) = delete;
    CohortJournalReceipt& operator=(const CohortJournalReceipt&) = delete;
    ~CohortJournalReceipt() = default;

    [[nodiscard]] explicit operator bool() const noexcept { return valid_; }
    [[nodiscard]] bool durable() const noexcept { return valid_; }
    [[nodiscard]] bool replayed() const noexcept { return replayed_; }
    [[nodiscard]] std::uint64_t ordinal() const noexcept { return ordinal_; }
    [[nodiscard]] std::uint64_t offset() const noexcept { return offset_; }
    [[nodiscard]] std::uint64_t bytes_written() const noexcept {
        return bytes_written_;
    }
    [[nodiscard]] std::uint32_t record_crc32c() const noexcept {
        return record_crc32c_;
    }

private:
    friend class CohortJournal;

    CohortJournalReceipt(
        std::uint64_t ordinal,
        std::uint64_t offset,
        std::uint64_t bytes_written,
        std::uint32_t record_crc32c,
        bool replayed) noexcept
        : ordinal_(ordinal),
          offset_(offset),
          bytes_written_(bytes_written),
          record_crc32c_(record_crc32c),
          replayed_(replayed) {}

    std::uint64_t ordinal_{};
    std::uint64_t offset_{};
    std::uint64_t bytes_written_{};
    std::uint32_t record_crc32c_{};
    bool replayed_{};
    bool valid_{true};
};

[[nodiscard]] bool is_canonical_recovery_cohort(
    const RecoveryCohort& cohort,
    std::uint32_t max_members,
    EmptyCohortConfirmation empty_confirmation) noexcept;

[[nodiscard]] std::uint64_t recovery_plan_fingerprint(
    std::uint64_t generation,
    RecoveryPlanToken token,
    const RecoveryCohort& cohort,
    EmptyCohortConfirmation empty_confirmation) noexcept;

// A caller-serialized, append-only recovery-intent journal. Its durable records
// fence recovery plans and attempts but never authorize release, adoption, or
// publication. Creating a new journal is allowed only from a validated
// inventory with no uncovered WAL suffix. BindFrame must carry the exact
// durable WAL append receipt from its preceding BeginFrame transaction.
// AdoptFrame must already fall within the immutable WAL prefix captured by the
// supplied inventory. These identities are still intent, not trusted WAL
// evidence: before any network use, a recovery consumer must exact-match every
// selected WalFrameIdentity, including entry_crc32c, against an immutable
// CommandLogSnapshot.
//
// An open freezes the inventory's authoritative release and WAL cursors.
// External WAL/release advances are deliberately invisible. Callers reopen
// after such an advance before ReconfigureFrame or AttemptStarted; BindFrame's
// sole exception is the ordered BeginFrame -> WAL append -> BindFrame path.
class CohortJournal final {
public:
    [[nodiscard]] static core::Result<CohortJournal, CohortJournalError>
    open_or_create(
        std::filesystem::path path,
        CohortJournalUuid journal_uuid,
        RecoveryShowId show_id,
        const UnresolvedReleaseInventory& inventory,
        CohortJournalOpenOptions options = {});

    CohortJournal(CohortJournal&& other) noexcept;
    CohortJournal& operator=(CohortJournal&& other) noexcept;
    CohortJournal(const CohortJournal&) = delete;
    CohortJournal& operator=(const CohortJournal&) = delete;
    ~CohortJournal();

    // Adopt/Reconfigure retransmits with the same operator request ID and
    // byte-equivalent payload return the original receipt before ordinal CAS.
    [[nodiscard]] core::Result<CohortJournalReceipt, CohortJournalError>
    append(
        std::uint64_t expected_next_ordinal,
        const CohortJournalMutation& mutation);

    [[nodiscard]] std::uint64_t next_ordinal() const noexcept;
    [[nodiscard]] const CohortJournalBinding& binding() const noexcept {
        return binding_;
    }
    [[nodiscard]] const CohortJournalRecoveryReport& recovery_report() const noexcept {
        return recovery_;
    }
    [[nodiscard]] const std::vector<CohortJournalRecord>& records() const noexcept {
        return records_;
    }
    [[nodiscard]] SyncPolicy sync_policy() const noexcept {
        return options_.sync_policy;
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    struct State;

    CohortJournal(
        std::filesystem::path path,
        CohortJournalOpenOptions options,
        CohortJournalBinding binding,
        CohortJournalRecoveryReport recovery,
        std::vector<CohortJournalRecord> records,
        std::unique_ptr<State> state) noexcept;

    std::filesystem::path path_;
    CohortJournalOpenOptions options_{};
    CohortJournalBinding binding_{};
    CohortJournalRecoveryReport recovery_{};
    std::vector<CohortJournalRecord> records_;
    std::unique_ptr<State> state_;
    bool usable_{true};
};

} // namespace mediaserver::persist
