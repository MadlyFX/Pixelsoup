#include <mediaserver/persist/release_ledger_recovery_release_authority.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

namespace cmd = mediaserver::cmd;
namespace doc = mediaserver::doc;
namespace persist = mediaserver::persist;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define RECOVERY_RELEASE_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TempStore final {
public:
    TempStore() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        const auto stem = std::string{"mediaserver-recovery-release-"}
            + std::to_string(timestamp) + "-"
            + std::to_string(sequence.fetch_add(1U));
        wal_ = std::filesystem::temp_directory_path() / (stem + ".wal");
        ledger_ = std::filesystem::temp_directory_path() / (stem + ".release");
    }

    ~TempStore() {
        std::error_code ignored;
        std::filesystem::remove(wal_, ignored);
        std::filesystem::remove(ledger_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& wal() const noexcept {
        return wal_;
    }
    [[nodiscard]] const std::filesystem::path& ledger() const noexcept {
        return ledger_;
    }

private:
    std::filesystem::path wal_;
    std::filesystem::path ledger_;
};

[[nodiscard]] cmd::StreamCursor genesis() noexcept {
    return cmd::StreamCursor{
        cmd::FrameNumber{0U},
        doc::Version{0U},
        cmd::CommandId{1U},
    };
}

[[nodiscard]] persist::LogCursor as_log_cursor(
    const cmd::StreamCursor& cursor) noexcept {
    return persist::LogCursor{
        cursor.through_frame.value(),
        cursor.revision.value(),
        cursor.next_command.value(),
    };
}

[[nodiscard]] persist::CommandLog open_log(
    const std::filesystem::path& path) {
    persist::OpenOptions options;
    options.sync_policy = persist::SyncPolicy::Data;
    auto opened = persist::CommandLog::open_or_create(path, genesis(), options);
    RECOVERY_RELEASE_CHECK(opened);
    return std::move(opened).value();
}

[[nodiscard]] persist::ReleaseLedger open_ledger(
    const std::filesystem::path& path,
    const persist::CommandLog& log) {
    auto snapshot = log.capture_committed();
    RECOVERY_RELEASE_CHECK(snapshot);
    persist::ReleaseLedgerOpenOptions options;
    options.sync_policy = persist::SyncPolicy::Data;
    auto opened = persist::ReleaseLedger::open_or_create(
        path, snapshot.value(), options);
    RECOVERY_RELEASE_CHECK(opened);
    return std::move(opened).value();
}

[[nodiscard]] persist::WalFrameIdentity append_empty(
    persist::CommandLog& log,
    cmd::StreamCursor before,
    std::uint64_t frame) {
    const cmd::FrameSeal seal{
        cmd::FrameNumber{frame},
        before.revision,
        before.revision,
        {},
    };
    auto appended = log.append(std::span<const cmd::Command>{}, seal);
    RECOVERY_RELEASE_CHECK(appended);
    return persist::WalFrameIdentity{
        as_log_cursor(before),
        appended.value().cursor,
        appended.value().offset,
        appended.value().bytes_written,
        appended.value().entry_crc32c,
    };
}

[[nodiscard]] persist::UnresolvedReleaseInventory inspect(
    const persist::CommandLogSnapshot& snapshot,
    const persist::ReleaseLedger& ledger) {
    auto inspected = persist::UnresolvedReleaseInventory::inspect(
        snapshot, ledger);
    RECOVERY_RELEASE_CHECK(inspected);
    return std::move(inspected).value();
}

void exact_front_release_reauthenticates_and_exposes_exact_next() {
    TempStore store;
    auto log = open_log(store.wal());
    auto ledger = open_ledger(store.ledger(), log);
    const auto first = append_empty(log, genesis(), 1U);
    const auto second = append_empty(
        log,
        cmd::StreamCursor{
            cmd::FrameNumber{first.after.through_frame},
            doc::Version{first.after.revision},
            cmd::CommandId{first.after.next_command},
        },
        2U);
    auto snapshot = log.capture_committed();
    RECOVERY_RELEASE_CHECK(snapshot);
    const auto inventory = inspect(snapshot.value(), ledger);
    persist::ReleaseLedgerRecoveryReleaseAuthority authority{ledger};

    auto released = authority.persist_exact(
        snapshot.value(), inventory, first, inventory.next_release_ordinal);
    RECOVERY_RELEASE_CHECK(released);
    RECOVERY_RELEASE_CHECK(released.value().durable());
    RECOVERY_RELEASE_CHECK(released.value().ordinal() == 1U);
    RECOVERY_RELEASE_CHECK(released.value().frame() == first);
    RECOVERY_RELEASE_CHECK(!authority.faulted());

    const auto after = inspect(snapshot.value(), ledger);
    RECOVERY_RELEASE_CHECK(after.released_frontier == first.after);
    RECOVERY_RELEASE_CHECK(after.unresolved_frame_count == 1U);
    RECOVERY_RELEASE_CHECK(after.next_unresolved == second);
    RECOVERY_RELEASE_CHECK(after.next_release_ordinal == 2U);
}

void preappend_mismatches_do_not_fault_or_touch_the_ledger() {
    TempStore store;
    auto log = open_log(store.wal());
    auto ledger = open_ledger(store.ledger(), log);
    const auto frame = append_empty(log, genesis(), 1U);
    auto snapshot = log.capture_committed();
    RECOVERY_RELEASE_CHECK(snapshot);
    const auto inventory = inspect(snapshot.value(), ledger);
    const auto original_bytes = std::filesystem::file_size(store.ledger());
    persist::ReleaseLedgerRecoveryReleaseAuthority authority{ledger};

    auto wrong_inventory = inventory;
    auto wrong_frame = frame;
    wrong_frame.entry_crc32c ^= 0x1U;
    wrong_inventory.next_unresolved = wrong_frame;
    auto snapshot_mismatch = authority.persist_exact(
        snapshot.value(), wrong_inventory, wrong_frame, 1U);
    RECOVERY_RELEASE_CHECK(!snapshot_mismatch);
    RECOVERY_RELEASE_CHECK(snapshot_mismatch.error().code
        == persist::RecoveryReleaseFailureCode::SnapshotMismatch);
    RECOVERY_RELEASE_CHECK(!authority.faulted());
    RECOVERY_RELEASE_CHECK(
        std::filesystem::file_size(store.ledger()) == original_bytes);

    auto bad_ordinal = authority.persist_exact(
        snapshot.value(), inventory, frame, 2U);
    RECOVERY_RELEASE_CHECK(!bad_ordinal);
    RECOVERY_RELEASE_CHECK(bad_ordinal.error().code
        == persist::RecoveryReleaseFailureCode::InvalidRequest);
    RECOVERY_RELEASE_CHECK(!authority.faulted());

    auto released = authority.persist_exact(snapshot.value(), inventory, frame, 1U);
    RECOVERY_RELEASE_CHECK(released);
    auto stale = authority.persist_exact(snapshot.value(), inventory, frame, 1U);
    RECOVERY_RELEASE_CHECK(!stale);
    RECOVERY_RELEASE_CHECK(stale.error().code
        == persist::RecoveryReleaseFailureCode::InventoryChanged);
    RECOVERY_RELEASE_CHECK(!authority.faulted());
}

void append_phase_ambiguity_latches_the_adapter() {
    TempStore store;
    auto log = open_log(store.wal());
    auto ledger = open_ledger(store.ledger(), log);
    const auto frame = append_empty(log, genesis(), 1U);
    auto snapshot = log.capture_committed();
    RECOVERY_RELEASE_CHECK(snapshot);
    const auto inventory = inspect(snapshot.value(), ledger);

    // Inventory inspection is bounded by the captured valid ledger prefix and
    // deliberately ignores a later external suffix. The private append then
    // detects the changed physical size after the adapter enters append phase.
    {
        std::ofstream output(store.ledger(), std::ios::binary | std::ios::app);
        RECOVERY_RELEASE_CHECK(output);
        constexpr char byte = '\x5a';
        output.write(&byte, 1);
        output.flush();
        RECOVERY_RELEASE_CHECK(output);
    }

    persist::ReleaseLedgerRecoveryReleaseAuthority authority{ledger};
    auto rejected = authority.persist_exact(snapshot.value(), inventory, frame, 1U);
    RECOVERY_RELEASE_CHECK(!rejected);
    RECOVERY_RELEASE_CHECK(rejected.error().code
        == persist::RecoveryReleaseFailureCode::LedgerFailure);
    RECOVERY_RELEASE_CHECK(authority.faulted());
    auto fenced = authority.persist_exact(snapshot.value(), inventory, frame, 1U);
    RECOVERY_RELEASE_CHECK(!fenced);
    RECOVERY_RELEASE_CHECK(fenced.error().code
        == persist::RecoveryReleaseFailureCode::AuthorityFaulted);
}

static_assert(
    !std::is_copy_constructible_v<
        persist::ReleaseLedgerRecoveryReleaseAuthority>);
static_assert(
    !std::is_move_constructible_v<
        persist::ReleaseLedgerRecoveryReleaseAuthority>);

} // namespace

int main() {
    struct Test final {
        const char* name;
        void (*run)();
    };
    const std::array tests{
        Test{"exact front release and next identity",
             &exact_front_release_reauthenticates_and_exposes_exact_next},
        Test{"preappend mismatch remains retryable",
             &preappend_mismatches_do_not_fault_or_touch_the_ledger},
        Test{"append ambiguity latches adapter",
             &append_phase_ambiguity_latches_the_adapter},
    };
    std::size_t failures = 0U;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": "
                      << exception.what() << '\n';
        }
    }
    return failures == 0U ? 0 : 1;
}
