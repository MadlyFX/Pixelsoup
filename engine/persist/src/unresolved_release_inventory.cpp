#include <mediaserver/persist/unresolved_release_inventory.hpp>

#include <limits>
#include <optional>
#include <utility>

namespace mediaserver::persist {
namespace {

using InventoryResult = core::Result<
    UnresolvedReleaseInventory,
    UnresolvedReleaseInventoryError>;

[[nodiscard]] LogCursor to_log_cursor(
    const cmd::StreamCursor& cursor) noexcept {
    return LogCursor{
        cursor.through_frame.value(),
        cursor.revision.value(),
        cursor.next_command.value(),
    };
}

[[nodiscard]] UnresolvedReleaseInventoryError make_error(
    UnresolvedReleaseInventoryErrorCode code,
    std::uint64_t wal_frame_index,
    std::string detail,
    std::optional<CommandLogError> command_log_error = std::nullopt,
    std::optional<ReleaseLedgerError> release_ledger_error = std::nullopt) {
    return UnresolvedReleaseInventoryError{
        code,
        wal_frame_index,
        std::move(detail),
        std::move(command_log_error),
        std::move(release_ledger_error),
    };
}

} // namespace

InventoryResult UnresolvedReleaseInventory::inspect(
    const CommandLogSnapshot& wal,
    const ReleaseLedger& ledger) {
    const auto prefix = ledger.capture_inventory_prefix();
    const auto& ledger_report = prefix.recovery;
    if (!prefix.usable) {
        return InventoryResult::failure(make_error(
            UnresolvedReleaseInventoryErrorCode::LedgerUnusable,
            0U,
            "release ledger is unusable and must be reopened"));
    }

    UnresolvedReleaseInventory inventory;
    inventory.base_cursor = to_log_cursor(wal.base_cursor());
    inventory.released_frontier = ledger_report.frontier;
    inventory.wal_tail = to_log_cursor(wal.tail_cursor());
    inventory.released_frame_count = ledger_report.decision_count;
    inventory.wal_valid_bytes = wal.valid_bytes();
    inventory.release_ledger_valid_bytes = ledger_report.valid_bytes;
    inventory.released_wal_bytes = ledger_report.released_wal_bytes;
    inventory.next_release_ordinal =
        ledger_report.decision_count
                == std::numeric_limits<std::uint64_t>::max()
            ? 0U
            : ledger_report.decision_count + 1U;

    if (ledger_report.base_cursor != inventory.base_cursor) {
        return InventoryResult::failure(make_error(
            UnresolvedReleaseInventoryErrorCode::BaseCursorMismatch,
            0U,
            "release-ledger base differs from the supplied WAL snapshot"));
    }
    if (ledger_report.decision_count > ledger_report.wal_entry_count
        || ledger_report.unresolved_wal_frames
            != ledger_report.wal_entry_count
                - ledger_report.decision_count) {
        return InventoryResult::failure(make_error(
            UnresolvedReleaseInventoryErrorCode::LedgerStateMismatch,
            0U,
            "captured release-ledger WAL counts are inconsistent"));
    }
    if ((ledger_report.wal_entry_count == 0U
         && ledger_report.wal_tail != inventory.base_cursor)
        || (ledger_report.wal_entry_count != 0U
            && ledger_report.wal_tail == inventory.base_cursor)) {
        return InventoryResult::failure(make_error(
            UnresolvedReleaseInventoryErrorCode::LedgerStateMismatch,
            0U,
            "captured release-ledger WAL tail is inconsistent with its count"));
    }
    if ((ledger_report.decision_count == 0U
         && ledger_report.frontier != inventory.base_cursor)
        || (ledger_report.decision_count != 0U
            && ledger_report.frontier == inventory.base_cursor)) {
        return InventoryResult::failure(make_error(
            UnresolvedReleaseInventoryErrorCode::LedgerStateMismatch,
            0U,
            "captured release frontier is inconsistent with its decision count"));
    }

    std::optional<WalFrameIdentity> last_released;
    std::optional<UnresolvedReleaseInventoryError> scan_error;
    bool found_bound_wal_tail = ledger_report.wal_entry_count == 0U;
    auto visited = wal.visit_committed_batches(
        [&ledger,
         &prefix,
         &ledger_report,
         &inventory,
         &last_released,
         &scan_error,
         &found_bound_wal_tail](const ValidatedCommandBatchView& batch) {
            const auto index = inventory.wal_frame_count;
            if (index == std::numeric_limits<std::uint64_t>::max()) {
                scan_error = make_error(
                    UnresolvedReleaseInventoryErrorCode::LimitExceeded,
                    index,
                    "WAL frame count is not representable");
                return BatchVisitControl::Stop;
            }
            ++inventory.wal_frame_count;

            if (inventory.wal_frame_count == ledger_report.wal_entry_count) {
                if (batch.identity.after != ledger_report.wal_tail) {
                    scan_error = make_error(
                        UnresolvedReleaseInventoryErrorCode::BoundWalPrefixMissing,
                        index,
                        "supplied WAL diverges before the ledger-bound tail");
                    return BatchVisitControl::Stop;
                }
                found_bound_wal_tail = true;
            } else if (batch.identity.after == ledger_report.wal_tail) {
                scan_error = make_error(
                    UnresolvedReleaseInventoryErrorCode::BoundWalPrefixMissing,
                    index,
                    "ledger-bound WAL tail occurs at a different frame count");
                return BatchVisitControl::Stop;
            }

            if (index < ledger_report.decision_count) {
                auto released = ledger.read_release_identity(prefix, index + 1U);
                if (!released) {
                    scan_error = make_error(
                        UnresolvedReleaseInventoryErrorCode::LedgerReadFailure,
                        index,
                        "could not authenticate a captured release record",
                        std::nullopt,
                        std::move(released).error());
                    return BatchVisitControl::Stop;
                }
                if (released.value() != batch.identity) {
                    scan_error = make_error(
                        UnresolvedReleaseInventoryErrorCode::ReleasedIdentityMismatch,
                        index,
                        "released frame does not match the exact WAL identity");
                    return BatchVisitControl::Stop;
                }
                last_released = batch.identity;
            } else if (!inventory.next_unresolved) {
                inventory.next_unresolved = batch.identity;
            }
            return BatchVisitControl::Continue;
        });

    if (scan_error) {
        return InventoryResult::failure(std::move(*scan_error));
    }
    if (!visited) {
        return InventoryResult::failure(make_error(
            UnresolvedReleaseInventoryErrorCode::WalSnapshotFailure,
            inventory.wal_frame_count,
            "supplied WAL snapshot failed typed identity validation",
            std::move(visited).error()));
    }
    if (!visited.value().completed) {
        return InventoryResult::failure(make_error(
            UnresolvedReleaseInventoryErrorCode::WalSnapshotFailure,
            inventory.wal_frame_count,
            "supplied WAL snapshot visit stopped unexpectedly"));
    }
    if (!found_bound_wal_tail
        || inventory.wal_frame_count < ledger_report.wal_entry_count) {
        return InventoryResult::failure(make_error(
            UnresolvedReleaseInventoryErrorCode::BoundWalPrefixMissing,
            inventory.wal_frame_count,
            "supplied WAL truncates the prefix previously bound to the ledger"));
    }
    if (inventory.wal_frame_count < ledger_report.decision_count) {
        return InventoryResult::failure(make_error(
            UnresolvedReleaseInventoryErrorCode::BoundWalPrefixMissing,
            inventory.wal_frame_count,
            "release decisions extend beyond the supplied WAL snapshot"));
    }

    const auto computed_frontier = last_released
        ? last_released->after
        : inventory.base_cursor;
    if (computed_frontier != ledger_report.frontier) {
        return InventoryResult::failure(make_error(
            UnresolvedReleaseInventoryErrorCode::LedgerStateMismatch,
            inventory.released_frame_count,
            "authenticated releases do not end at the captured frontier"));
    }

    std::uint64_t computed_released_wal_bytes = inventory.wal_valid_bytes;
    if (last_released) {
        if (last_released->bytes
            > std::numeric_limits<std::uint64_t>::max()
                - last_released->offset) {
            return InventoryResult::failure(make_error(
                UnresolvedReleaseInventoryErrorCode::LimitExceeded,
                inventory.released_frame_count - 1U,
                "released WAL byte frontier is not representable"));
        }
        computed_released_wal_bytes =
            last_released->offset + last_released->bytes;
    } else if (inventory.next_unresolved) {
        computed_released_wal_bytes = inventory.next_unresolved->offset;
    }
    if (computed_released_wal_bytes != ledger_report.released_wal_bytes) {
        return InventoryResult::failure(make_error(
            UnresolvedReleaseInventoryErrorCode::LedgerStateMismatch,
            inventory.released_frame_count,
            "captured release byte frontier differs from the WAL boundary"));
    }
    if (inventory.released_wal_bytes > inventory.wal_valid_bytes) {
        return InventoryResult::failure(make_error(
            UnresolvedReleaseInventoryErrorCode::LedgerStateMismatch,
            inventory.released_frame_count,
            "release byte frontier is ahead of the supplied WAL snapshot"));
    }

    inventory.unresolved_frame_count =
        inventory.wal_frame_count - inventory.released_frame_count;
    inventory.unresolved_wal_bytes =
        inventory.wal_valid_bytes - inventory.released_wal_bytes;
    if ((inventory.unresolved_frame_count == 0U)
        != !inventory.next_unresolved) {
        return InventoryResult::failure(make_error(
            UnresolvedReleaseInventoryErrorCode::LedgerStateMismatch,
            inventory.released_frame_count,
            "first unresolved identity disagrees with the unresolved count"));
    }
    return InventoryResult::success(std::move(inventory));
}

} // namespace mediaserver::persist
