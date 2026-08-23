#pragma once

#include <atomic>
#include <mutex>

#include <mediaserver/persist/recovery_release_authority.hpp>
#include <mediaserver/persist/release_ledger.hpp>

namespace mediaserver::persist {

// The sole production adapter for recovery release. One instance serializes
// the complete validate/reinspect/append/postcondition sequence against its
// borrowed ledger. A failure after entering append, or an impossible receipt
// or ledger postcondition, permanently faults the adapter until reconstruction.
class ReleaseLedgerRecoveryReleaseAuthority final
    : public RecoveryReleaseAuthority {
public:
    explicit ReleaseLedgerRecoveryReleaseAuthority(
        ReleaseLedger& ledger) noexcept
        : ledger_(&ledger) {}

    ReleaseLedgerRecoveryReleaseAuthority(
        const ReleaseLedgerRecoveryReleaseAuthority&) = delete;
    ReleaseLedgerRecoveryReleaseAuthority& operator=(
        const ReleaseLedgerRecoveryReleaseAuthority&) = delete;
    ReleaseLedgerRecoveryReleaseAuthority(
        ReleaseLedgerRecoveryReleaseAuthority&&) = delete;
    ReleaseLedgerRecoveryReleaseAuthority& operator=(
        ReleaseLedgerRecoveryReleaseAuthority&&) = delete;
    ~ReleaseLedgerRecoveryReleaseAuthority() override = default;

    [[nodiscard]] core::Result<
        DurableReleaseReceipt,
        RecoveryReleaseFailure>
    persist_exact(
        const CommandLogSnapshot& wal,
        const UnresolvedReleaseInventory& inventory,
        WalFrameIdentity frame,
        std::uint64_t expected_ordinal) noexcept override;

    [[nodiscard]] bool faulted() const noexcept override {
        return faulted_.load(std::memory_order_acquire);
    }

private:
    ReleaseLedger* ledger_;
    std::mutex mutex_;
    std::atomic_bool faulted_{false};
};

} // namespace mediaserver::persist
