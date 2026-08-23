#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <mediaserver/core/result.hpp>
#include <mediaserver/persist/release_ledger.hpp>

namespace mediaserver::persist {

enum class UnresolvedReleaseInventoryErrorCode : std::uint8_t {
    LedgerUnusable,
    BaseCursorMismatch,
    BoundWalPrefixMissing,
    ReleasedIdentityMismatch,
    LedgerStateMismatch,
    WalSnapshotFailure,
    LedgerReadFailure,
    LimitExceeded,
};

struct UnresolvedReleaseInventoryError final {
    UnresolvedReleaseInventoryErrorCode code{
        UnresolvedReleaseInventoryErrorCode::LedgerStateMismatch};
    // Zero-based index in the supplied WAL snapshot. It is zero for failures
    // that occur before any frame can be selected.
    std::uint64_t wal_frame_index{};
    std::string detail;
    std::optional<CommandLogError> command_log_error;
    std::optional<ReleaseLedgerError> release_ledger_error;
};

// Read-only recovery inventory for one immutable WAL snapshot and one captured
// release-ledger prefix. Inspection never appends to or repairs either file.
// It retains only the first unresolved frame while streaming through history.
struct UnresolvedReleaseInventory final {
    LogCursor base_cursor{};
    LogCursor released_frontier{};
    LogCursor wal_tail{};
    std::uint64_t wal_frame_count{};
    std::uint64_t released_frame_count{};
    std::uint64_t unresolved_frame_count{};
    // Zero only when the release ordinal space is exhausted.
    std::uint64_t next_release_ordinal{};
    std::uint64_t wal_valid_bytes{};
    // Immutable byte boundary of the release-ledger prefix captured by
    // inspect(). Recovery journals use this together with the decision count
    // to bind their creation header to exact durable release evidence.
    std::uint64_t release_ledger_valid_bytes{};
    // Exact byte offset at which the next WAL frame begins. This is the WAL
    // header size when no release decisions exist.
    std::uint64_t released_wal_bytes{};
    std::uint64_t unresolved_wal_bytes{};
    std::optional<WalFrameIdentity> next_unresolved;

    // The ledger state is captured once. A later append cannot extend this
    // result: only records within the captured decision_count/valid_bytes are
    // read and authenticated. The supplied WAL may extend, but must contain
    // the ledger's previously bound WAL prefix without truncation/divergence.
    [[nodiscard]] static core::Result<
        UnresolvedReleaseInventory,
        UnresolvedReleaseInventoryError>
    inspect(const CommandLogSnapshot& wal, const ReleaseLedger& ledger);
};

} // namespace mediaserver::persist
