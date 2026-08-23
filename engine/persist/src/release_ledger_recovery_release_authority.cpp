#include <mediaserver/persist/release_ledger_recovery_release_authority.hpp>

#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>

namespace mediaserver::persist {
namespace {

using Result = core::Result<DurableReleaseReceipt, RecoveryReleaseFailure>;

[[nodiscard]] Result failure(RecoveryReleaseFailureCode code) {
    return Result::failure(RecoveryReleaseFailure{code});
}

[[nodiscard]] cmd::StreamCursor as_stream_cursor(LogCursor cursor) noexcept {
    return cmd::StreamCursor{
        cmd::FrameNumber{cursor.through_frame},
        doc::Version{cursor.revision},
        cmd::CommandId{cursor.next_command},
    };
}

[[nodiscard]] bool same_inventory(
    const UnresolvedReleaseInventory& left,
    const UnresolvedReleaseInventory& right) noexcept {
    return left.base_cursor == right.base_cursor
        && left.released_frontier == right.released_frontier
        && left.wal_tail == right.wal_tail
        && left.wal_frame_count == right.wal_frame_count
        && left.released_frame_count == right.released_frame_count
        && left.unresolved_frame_count == right.unresolved_frame_count
        && left.next_release_ordinal == right.next_release_ordinal
        && left.wal_valid_bytes == right.wal_valid_bytes
        && left.release_ledger_valid_bytes
            == right.release_ledger_valid_bytes
        && left.released_wal_bytes == right.released_wal_bytes
        && left.unresolved_wal_bytes == right.unresolved_wal_bytes
        && left.next_unresolved == right.next_unresolved;
}

[[nodiscard]] bool request_is_structurally_valid(
    const CommandLogSnapshot& wal,
    const UnresolvedReleaseInventory& inventory,
    const WalFrameIdentity& frame,
    std::uint64_t expected_ordinal) noexcept {
    if (expected_ordinal == 0U || inventory.unresolved_frame_count == 0U
        || inventory.unresolved_wal_bytes == 0U
        || !inventory.next_unresolved
        || *inventory.next_unresolved != frame
        || inventory.next_release_ordinal != expected_ordinal
        || inventory.released_frontier != frame.before
        || frame.bytes == 0U || frame.offset != inventory.released_wal_bytes
        || frame.offset > inventory.wal_valid_bytes
        || frame.bytes > inventory.wal_valid_bytes - frame.offset
        || frame.before.through_frame
            == std::numeric_limits<std::uint64_t>::max()
        || frame.after.through_frame != frame.before.through_frame + 1U) {
        return false;
    }
    return wal.base_cursor() == as_stream_cursor(inventory.base_cursor)
        && wal.tail_cursor() == as_stream_cursor(inventory.wal_tail)
        && wal.valid_bytes() == inventory.wal_valid_bytes;
}

[[nodiscard]] core::Result<std::monostate, RecoveryReleaseFailure>
validate_exact_snapshot_frame(
    const CommandLogSnapshot& wal,
    const UnresolvedReleaseInventory& inventory,
    const WalFrameIdentity& expected) {
    using Status = core::Result<std::monostate, RecoveryReleaseFailure>;
    std::size_t visited = 0U;
    bool exact = true;
    auto visit = wal.visit_batches_after(
        as_stream_cursor(inventory.released_frontier),
        as_stream_cursor(expected.after),
        [&visited, &exact, &expected](const ValidatedCommandBatchView& batch) {
            ++visited;
            exact = exact && batch.identity == expected;
            return BatchVisitControl::Continue;
        });
    if (!visit || !visit.value().completed || visit.value().frames != 1U
        || visited != 1U || !exact
        || visit.value().cursor != as_stream_cursor(expected.after)) {
        return Status::failure(RecoveryReleaseFailure{
            RecoveryReleaseFailureCode::SnapshotMismatch});
    }
    return Status::success(std::monostate{});
}

[[nodiscard]] core::Result<
    std::optional<WalFrameIdentity>,
    RecoveryReleaseFailure>
capture_expected_next_identity(
    const CommandLogSnapshot& wal,
    const UnresolvedReleaseInventory& inventory,
    const WalFrameIdentity& current) {
    using NextResult = core::Result<
        std::optional<WalFrameIdentity>,
        RecoveryReleaseFailure>;
    if (inventory.unresolved_frame_count == 1U) {
        return NextResult::success(std::nullopt);
    }
    std::optional<WalFrameIdentity> next;
    auto visited = wal.visit_batches_after(
        as_stream_cursor(current.after),
        as_stream_cursor(inventory.wal_tail),
        [&next](const ValidatedCommandBatchView& batch) {
            next = batch.identity;
            return BatchVisitControl::Stop;
        });
    if (!visited || visited.value().completed || visited.value().frames != 1U
        || !next || next->before != current.after
        || next->offset != current.offset + current.bytes) {
        return NextResult::failure(RecoveryReleaseFailure{
            RecoveryReleaseFailureCode::SnapshotMismatch});
    }
    return NextResult::success(next);
}

[[nodiscard]] bool post_report_matches(
    const ReleaseLedgerRecoveryReport& before,
    const ReleaseLedgerRecoveryReport& after,
    const WalFrameIdentity& frame,
    const DurableReleaseReceipt& receipt,
    std::uint64_t expected_ordinal) noexcept {
    if (before.decision_count == std::numeric_limits<std::uint64_t>::max()
        || before.valid_bytes
            > std::numeric_limits<std::uint64_t>::max()
                - receipt.bytes_written()) {
        return false;
    }
    auto expected_wal_tail = before.wal_tail;
    auto expected_wal_entries = before.wal_entry_count;
    auto expected_unresolved = before.unresolved_wal_frames;
    if (expected_unresolved != 0U) {
        --expected_unresolved;
    } else if (before.wal_tail == frame.before) {
        if (expected_wal_entries == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        expected_wal_tail = frame.after;
        ++expected_wal_entries;
    }
    return before.frontier == frame.before
        && before.decision_count + 1U == expected_ordinal
        && before.released_wal_bytes == frame.offset
        && after.base_cursor == before.base_cursor
        && after.frontier == frame.after
        && after.wal_tail == expected_wal_tail
        && after.decision_count == expected_ordinal
        && after.wal_entry_count == expected_wal_entries
        && after.unresolved_wal_frames == expected_unresolved
        && after.released_wal_bytes == frame.offset + frame.bytes
        && after.valid_bytes == before.valid_bytes + receipt.bytes_written()
        && after.truncated_bytes == before.truncated_bytes
        && after.repaired_torn_tail == before.repaired_torn_tail;
}

[[nodiscard]] bool post_inventory_matches(
    const UnresolvedReleaseInventory& before,
    const UnresolvedReleaseInventory& after,
    const WalFrameIdentity& frame,
    const std::optional<WalFrameIdentity>& expected_next,
    const DurableReleaseReceipt& receipt,
    std::uint64_t expected_ordinal) noexcept {
    if (before.unresolved_frame_count == 0U
        || before.unresolved_wal_bytes < frame.bytes
        || before.released_frame_count
            == std::numeric_limits<std::uint64_t>::max()
        || before.release_ledger_valid_bytes
            > std::numeric_limits<std::uint64_t>::max()
                - receipt.bytes_written()) {
        return false;
    }
    const auto next_identity_matches = after.next_unresolved == expected_next
        && (after.unresolved_frame_count == 0U) == !expected_next;
    return next_identity_matches
        && receipt.durable() && receipt.ordinal() == expected_ordinal
        && receipt.offset() == before.release_ledger_valid_bytes
        && receipt.bytes_written() != 0U && receipt.frame() == frame
        && after.base_cursor == before.base_cursor
        && after.released_frontier == frame.after
        && after.wal_tail == before.wal_tail
        && after.wal_frame_count == before.wal_frame_count
        && after.released_frame_count == before.released_frame_count + 1U
        && after.unresolved_frame_count == before.unresolved_frame_count - 1U
        && after.next_release_ordinal
            == (expected_ordinal == std::numeric_limits<std::uint64_t>::max()
                    ? 0U
                    : expected_ordinal + 1U)
        && after.wal_valid_bytes == before.wal_valid_bytes
        && after.release_ledger_valid_bytes
            == before.release_ledger_valid_bytes + receipt.bytes_written()
        && after.released_wal_bytes == frame.offset + frame.bytes
        && after.unresolved_wal_bytes
            == before.unresolved_wal_bytes - frame.bytes;
}

} // namespace

core::Result<DurableReleaseReceipt, RecoveryReleaseFailure>
ReleaseLedgerRecoveryReleaseAuthority::persist_exact(
    const CommandLogSnapshot& wal,
    const UnresolvedReleaseInventory& inventory,
    WalFrameIdentity frame,
    std::uint64_t expected_ordinal) noexcept {
    bool append_entered = false;
    try {
        const std::lock_guard lock{mutex_};
        if (faulted_.load(std::memory_order_acquire)) {
            return failure(RecoveryReleaseFailureCode::AuthorityFaulted);
        }
        if (!request_is_structurally_valid(
                wal, inventory, frame, expected_ordinal)) {
            return failure(RecoveryReleaseFailureCode::InvalidRequest);
        }
        auto selected = validate_exact_snapshot_frame(wal, inventory, frame);
        if (!selected) {
            return failure(selected.error().code);
        }
        auto expected_next = capture_expected_next_identity(
            wal, inventory, frame);
        if (!expected_next) {
            return failure(expected_next.error().code);
        }

        auto refreshed = UnresolvedReleaseInventory::inspect(wal, *ledger_);
        if (!refreshed || !same_inventory(refreshed.value(), inventory)
            || ledger_->next_release_ordinal() != expected_ordinal) {
            return failure(RecoveryReleaseFailureCode::InventoryChanged);
        }
        const auto ledger_before = ledger_->recovery_report();

        append_entered = true;
        auto appended = ledger_->append_release(frame);
        if (!appended) {
            faulted_.store(true, std::memory_order_release);
            return failure(RecoveryReleaseFailureCode::LedgerFailure);
        }
        auto receipt = std::move(appended).value();
        const auto ledger_after = ledger_->recovery_report();
        auto post = UnresolvedReleaseInventory::inspect(wal, *ledger_);
        if (!post_report_matches(
                ledger_before,
                ledger_after,
                frame,
                receipt,
                expected_ordinal)
            || !post
            || !post_inventory_matches(
                inventory,
                post.value(),
                frame,
                expected_next.value(),
                receipt,
                expected_ordinal)) {
            faulted_.store(true, std::memory_order_release);
            return failure(RecoveryReleaseFailureCode::ReceiptMismatch);
        }
        return Result::success(std::move(receipt));
    } catch (const std::bad_alloc&) {
        if (append_entered) {
            faulted_.store(true, std::memory_order_release);
        }
        return failure(RecoveryReleaseFailureCode::AllocationFailure);
    } catch (const std::length_error&) {
        if (append_entered) {
            faulted_.store(true, std::memory_order_release);
        }
        return failure(RecoveryReleaseFailureCode::AllocationFailure);
    } catch (...) {
        if (append_entered) {
            faulted_.store(true, std::memory_order_release);
        }
        return failure(RecoveryReleaseFailureCode::UnexpectedFailure);
    }
}

} // namespace mediaserver::persist
