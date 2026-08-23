#pragma once

#include <cstdint>

#include <mediaserver/core/result.hpp>
#include <mediaserver/persist/unresolved_release_inventory.hpp>

namespace mediaserver::persist {

enum class RecoveryReleaseFailureCode : std::uint8_t {
    InvalidRequest,
    SnapshotMismatch,
    InventoryChanged,
    LedgerFailure,
    ReceiptMismatch,
    AuthorityFaulted,
    AllocationFailure,
    UnexpectedFailure,
};

// Deliberately allocation-free. Recovery callers need a reliable fail-closed
// classification at a noexcept persistence boundary; detailed storage errors
// remain available from the underlying ledger's reopen diagnostics.
struct RecoveryReleaseFailure final {
    RecoveryReleaseFailureCode code{
        RecoveryReleaseFailureCode::InvalidRequest};
};

// Recovery-only release boundary. Implementations must authenticate the exact
// immutable WAL frame and current release inventory immediately before making
// one contiguous ledger append. This interface exposes neither authoring nor
// commit-permit authority.
class RecoveryReleaseAuthority {
public:
    virtual ~RecoveryReleaseAuthority() = default;

    [[nodiscard]] virtual core::Result<
        DurableReleaseReceipt,
        RecoveryReleaseFailure>
    persist_exact(
        const CommandLogSnapshot& wal,
        const UnresolvedReleaseInventory& inventory,
        WalFrameIdentity frame,
        std::uint64_t expected_ordinal) noexcept = 0;

    [[nodiscard]] virtual bool faulted() const noexcept = 0;
};

} // namespace mediaserver::persist
