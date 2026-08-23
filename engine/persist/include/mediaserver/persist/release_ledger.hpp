#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include <mediaserver/core/result.hpp>
#include <mediaserver/persist/command_log.hpp>

namespace mediaserver::persist {

class DurableDirector;
class ReleaseLedgerRecoveryReleaseAuthority;
struct UnresolvedReleaseInventory;

enum class ReleaseLedgerErrorCode : std::uint8_t {
    NonDurablePolicy,
    MissingForNonEmptyWal,
    OpenFailed,
    ReadFailed,
    WriteFailed,
    SyncFailed,
    TruncateFailed,
    InvalidHeader,
    UnsupportedVersion,
    BaseCursorMismatch,
    TornTail,
    CorruptEntry,
    ChainMismatch,
    WalIdentityMismatch,
    WalSnapshotFailure,
    LimitExceeded,
    Unusable,
};

struct ReleaseLedgerError final {
    ReleaseLedgerErrorCode code{ReleaseLedgerErrorCode::OpenFailed};
    std::uint64_t offset{};
    std::error_code system_error{};
    std::string detail;
    std::optional<CommandLogError> command_log_error;
};

struct ReleaseLedgerOpenOptions final {
    SyncPolicy sync_policy{SyncPolicy::Full};
    bool repair_torn_tail{true};
};

struct ReleaseLedgerRecoveryReport final {
    LogCursor base_cursor{};
    LogCursor frontier{};
    LogCursor wal_tail{};
    std::uint64_t decision_count{};
    std::uint64_t wal_entry_count{};
    std::uint64_t unresolved_wal_frames{};
    std::uint64_t released_wal_bytes{};
    std::uint64_t valid_bytes{};
    std::uint64_t truncated_bytes{};
    bool repaired_torn_tail{};
};

// Move-only evidence that one exact WAL frame has a durably committed release
// decision. Higher layers must not publish a commit permit without this value.
class DurableReleaseReceipt final {
public:
    DurableReleaseReceipt(DurableReleaseReceipt&& other) noexcept;
    DurableReleaseReceipt& operator=(DurableReleaseReceipt&& other) noexcept;
    DurableReleaseReceipt(const DurableReleaseReceipt&) = delete;
    DurableReleaseReceipt& operator=(const DurableReleaseReceipt&) = delete;
    ~DurableReleaseReceipt() = default;

    [[nodiscard]] explicit operator bool() const noexcept { return valid_; }
    [[nodiscard]] bool durable() const noexcept { return valid_; }
    [[nodiscard]] std::uint64_t ordinal() const noexcept { return ordinal_; }
    [[nodiscard]] std::uint64_t offset() const noexcept { return offset_; }
    [[nodiscard]] std::uint64_t bytes_written() const noexcept {
        return bytes_written_;
    }
    [[nodiscard]] const WalFrameIdentity& frame() const noexcept {
        return frame_;
    }

private:
    friend class ReleaseLedger;

    DurableReleaseReceipt(
        std::uint64_t ordinal,
        std::uint64_t offset,
        std::uint64_t bytes_written,
        WalFrameIdentity frame) noexcept
        : ordinal_(ordinal),
          offset_(offset),
          bytes_written_(bytes_written),
          frame_(frame) {}

    std::uint64_t ordinal_{};
    std::uint64_t offset_{};
    std::uint64_t bytes_written_{};
    WalFrameIdentity frame_{};
    bool valid_{true};
};

// Append-only release decisions for one immutable WAL lineage. Creation is
// permitted only for an empty captured WAL. Opening an existing ledger scans
// its decisions and binds each one, in order, to the exact physical WAL entry
// exposed by CommandLogSnapshot.
class ReleaseLedger final {
public:
    [[nodiscard]] static core::Result<ReleaseLedger, ReleaseLedgerError>
    open_or_create(
        std::filesystem::path path,
        const CommandLogSnapshot& wal,
        ReleaseLedgerOpenOptions options = {});

    ReleaseLedger(ReleaseLedger&& other) noexcept;
    ReleaseLedger& operator=(ReleaseLedger&& other) noexcept;
    ReleaseLedger(const ReleaseLedger&) = delete;
    ReleaseLedger& operator=(const ReleaseLedger&) = delete;
    ~ReleaseLedger() = default;

    [[nodiscard]] LogCursor frontier() const noexcept;
    // Zero reports an unusable ledger or ordinal exhaustion. Otherwise this
    // is the only ordinal accepted by the next contiguous append.
    [[nodiscard]] std::uint64_t next_release_ordinal() const noexcept;
    // Exact historical lookup for a released cursor transition. The ordinal
    // is returned only when both cursors match the committed, WAL-bound record
    // at that ordinal. Lookup reads one fixed-size record and retains no
    // history-sized in-memory index.
    [[nodiscard]] std::optional<std::uint64_t> release_ordinal(
        LogCursor before,
        LogCursor after) const noexcept;
    [[nodiscard]] ReleaseLedgerRecoveryReport recovery_report() const noexcept;
    [[nodiscard]] SyncPolicy sync_policy() const noexcept {
        return options_.sync_policy;
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    friend class DurableDirector;
    friend class ReleaseLedgerRecoveryReleaseAuthority;
    friend struct UnresolvedReleaseInventory;

    struct InventoryPrefix final {
        ReleaseLedgerRecoveryReport recovery{};
        bool usable{};
    };

    // Only DurableDirector may mint release evidence. Its move-only
    // CommittedFrameBatch binds this physical identity to the exact WAL append
    // produced by the same coordinator.
    [[nodiscard]] core::Result<DurableReleaseReceipt, ReleaseLedgerError>
    append_release(const WalFrameIdentity& frame);

    // Recovery inventory captures this metadata once, then reads only the
    // immutable fixed records bounded by it. Later appends are deliberately
    // invisible to that inspection.
    [[nodiscard]] InventoryPrefix capture_inventory_prefix() const noexcept;
    [[nodiscard]] core::Result<WalFrameIdentity, ReleaseLedgerError>
    read_release_identity(
        const InventoryPrefix& prefix,
        std::uint64_t ordinal) const;

    ReleaseLedger(
        std::filesystem::path path,
        ReleaseLedgerOpenOptions options,
        ReleaseLedgerRecoveryReport recovery) noexcept
        : path_(std::move(path)),
          options_(options),
          recovery_(recovery) {}

    std::filesystem::path path_;
    ReleaseLedgerOpenOptions options_{};
    ReleaseLedgerRecoveryReport recovery_{};
    bool usable_{true};
    mutable std::mutex mutex_;
};

} // namespace mediaserver::persist
