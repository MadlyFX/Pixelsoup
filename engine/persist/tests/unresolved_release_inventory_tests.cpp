#include <mediaserver/persist/durable_director.hpp>
#include <mediaserver/persist/unresolved_release_inventory.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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

#define INVENTORY_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TempFile final {
public:
    explicit TempFile(std::string_view suffix) {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("mediaserver-unresolved-inventory-"
               + std::to_string(timestamp) + "-"
               + std::to_string(sequence.fetch_add(1U))
               + std::string{suffix});
    }

    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
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

[[nodiscard]] persist::OpenOptions wal_options(
    bool repair = true) noexcept {
    persist::OpenOptions options;
    options.sync_policy = persist::SyncPolicy::Data;
    options.repair_torn_tail = repair;
    return options;
}

[[nodiscard]] persist::ReleaseLedgerOpenOptions ledger_options(
    bool repair = true) noexcept {
    persist::ReleaseLedgerOpenOptions options;
    options.sync_policy = persist::SyncPolicy::Data;
    options.repair_torn_tail = repair;
    return options;
}

[[nodiscard]] persist::CommandLog open_log(
    const std::filesystem::path& path,
    cmd::StreamCursor base = genesis(),
    bool repair = true) {
    auto opened = persist::CommandLog::open_or_create(
        path, base, wal_options(repair));
    INVENTORY_CHECK(opened);
    return std::move(opened).value();
}

[[nodiscard]] persist::ReleaseLedger open_ledger(
    const std::filesystem::path& path,
    const persist::CommandLogSnapshot& wal,
    bool repair = true) {
    auto opened = persist::ReleaseLedger::open_or_create(
        path, wal, ledger_options(repair));
    INVENTORY_CHECK(opened);
    return std::move(opened).value();
}

[[nodiscard]] persist::WalFrameIdentity identity_of(
    const persist::CommittedFrameBatch& batch) noexcept {
    return persist::WalFrameIdentity{
        as_log_cursor(batch.before_cursor()),
        batch.receipt().cursor,
        batch.receipt().offset,
        batch.receipt().bytes_written,
        batch.receipt().entry_crc32c,
    };
}

[[nodiscard]] persist::CommittedFrameBatch seal_empty(
    persist::DurableDirector& durable,
    std::uint64_t frame) {
    auto sealed = durable.seal_frame(cmd::FrameNumber{frame});
    INVENTORY_CHECK(sealed);
    return std::move(sealed).value();
}

void release(
    persist::DurableDirector& durable,
    const persist::CommittedFrameBatch& batch) {
    const auto ordinal = durable.next_release_ordinal();
    INVENTORY_CHECK(ordinal != 0U);
    auto released = durable.persist_release(batch, ordinal);
    INVENTORY_CHECK(released);
    INVENTORY_CHECK(released.value().frame() == identity_of(batch));
}

[[nodiscard]] cmd::FrameSeal empty_seal(
    std::uint64_t frame,
    std::uint64_t revision) {
    return cmd::FrameSeal{
        cmd::FrameNumber{frame},
        doc::Version{revision},
        doc::Version{revision},
        {},
    };
}

[[nodiscard]] persist::WalFrameIdentity append_empty_frame(
    persist::CommandLog& log,
    cmd::StreamCursor before,
    std::uint64_t frame) {
    auto appended = log.append(
        std::span<const cmd::Command>{},
        empty_seal(frame, before.revision.value()));
    INVENTORY_CHECK(appended);
    return persist::WalFrameIdentity{
        as_log_cursor(before),
        appended.value().cursor,
        appended.value().offset,
        appended.value().bytes_written,
        appended.value().entry_crc32c,
    };
}

[[nodiscard]] cmd::StreamCursor as_stream_cursor(
    persist::LogCursor cursor) noexcept {
    return cmd::StreamCursor{
        cmd::FrameNumber{cursor.through_frame},
        doc::Version{cursor.revision},
        cmd::CommandId{cursor.next_command},
    };
}

[[nodiscard]] doc::Node node(std::int64_t value) {
    return doc::Node{
        doc::NodeId{1U},
        "inventory.test",
        {{"value", value}},
    };
}

void schedule_value(cmd::DirectorScheduler& director, std::int64_t value) {
    auto scheduled = director.schedule_edit(
        cmd::PutNode{node(value)},
        cmd::EraseNode{doc::NodeId{1U}},
        cmd::UndoContextId{1U},
        cmd::TransactionId{1U},
        cmd::FrameNumber{1U});
    INVENTORY_CHECK(scheduled);
}

void flip_byte(const std::filesystem::path& path, std::uint64_t offset) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    INVENTORY_CHECK(file);
    file.seekg(static_cast<std::streamoff>(offset));
    char byte{};
    file.read(&byte, 1);
    INVENTORY_CHECK(file);
    byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0x01U);
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(&byte, 1);
    file.flush();
    INVENTORY_CHECK(file);
}

void truncate_to(const std::filesystem::path& path, std::uint64_t size) {
    std::error_code error;
    std::filesystem::resize_file(path, size, error);
    INVENTORY_CHECK(!error);
}

struct ReleaseFixture final {
    TempFile wal_path{".wal"};
    TempFile ledger_path{".release"};
    persist::CommandLog log;
    persist::ReleaseLedger ledger;
    cmd::DirectorScheduler director;
    persist::DurableDirector durable;

    ReleaseFixture()
        : log(open_log(wal_path.path())),
          ledger(open_empty_ledger()),
          durable(director, log, ledger) {
        INVENTORY_CHECK(!durable.recovery_required());
    }

private:
    [[nodiscard]] persist::ReleaseLedger open_empty_ledger() {
        auto snapshot = log.capture_committed();
        INVENTORY_CHECK(snapshot);
        return open_ledger(ledger_path.path(), snapshot.value());
    }
};

void empty_sources_are_equal() {
    ReleaseFixture fixture;
    auto snapshot = fixture.log.capture_committed();
    INVENTORY_CHECK(snapshot);
    const auto expected_bytes = snapshot.value().valid_bytes();
    auto inspected = persist::UnresolvedReleaseInventory::inspect(
        snapshot.value(), fixture.ledger);
    INVENTORY_CHECK(inspected);
    const auto& inventory = inspected.value();
    INVENTORY_CHECK(inventory.base_cursor == as_log_cursor(genesis()));
    INVENTORY_CHECK(inventory.released_frontier == inventory.base_cursor);
    INVENTORY_CHECK(inventory.wal_tail == inventory.base_cursor);
    INVENTORY_CHECK(inventory.wal_frame_count == 0U);
    INVENTORY_CHECK(inventory.released_frame_count == 0U);
    INVENTORY_CHECK(inventory.unresolved_frame_count == 0U);
    INVENTORY_CHECK(inventory.next_release_ordinal == 1U);
    INVENTORY_CHECK(inventory.wal_valid_bytes == expected_bytes);
    INVENTORY_CHECK(inventory.release_ledger_valid_bytes == 64U);
    INVENTORY_CHECK(inventory.released_wal_bytes == expected_bytes);
    INVENTORY_CHECK(inventory.unresolved_wal_bytes == 0U);
    INVENTORY_CHECK(!inventory.next_unresolved);
}

void fully_released_sources_are_equal() {
    ReleaseFixture fixture;
    auto first = seal_empty(fixture.durable, 1U);
    release(fixture.durable, first);
    auto second = seal_empty(fixture.durable, 2U);
    release(fixture.durable, second);

    auto snapshot = fixture.log.capture_committed();
    INVENTORY_CHECK(snapshot);
    auto inspected = persist::UnresolvedReleaseInventory::inspect(
        snapshot.value(), fixture.ledger);
    INVENTORY_CHECK(inspected);
    const auto& inventory = inspected.value();
    INVENTORY_CHECK(inventory.released_frontier == identity_of(second).after);
    INVENTORY_CHECK(inventory.wal_tail == identity_of(second).after);
    INVENTORY_CHECK(inventory.wal_frame_count == 2U);
    INVENTORY_CHECK(inventory.released_frame_count == 2U);
    INVENTORY_CHECK(inventory.unresolved_frame_count == 0U);
    INVENTORY_CHECK(inventory.next_release_ordinal == 3U);
    INVENTORY_CHECK(inventory.release_ledger_valid_bytes == 320U);
    INVENTORY_CHECK(
        inventory.released_wal_bytes == inventory.wal_valid_bytes);
    INVENTORY_CHECK(inventory.unresolved_wal_bytes == 0U);
    INVENTORY_CHECK(!inventory.next_unresolved);
}

void one_forward_extension_is_pending() {
    ReleaseFixture fixture;
    auto first = seal_empty(fixture.durable, 1U);
    const auto first_identity = identity_of(first);
    auto snapshot = fixture.log.capture_committed();
    INVENTORY_CHECK(snapshot);

    auto inspected = persist::UnresolvedReleaseInventory::inspect(
        snapshot.value(), fixture.ledger);
    INVENTORY_CHECK(inspected);
    const auto& inventory = inspected.value();
    INVENTORY_CHECK(inventory.released_frontier == as_log_cursor(genesis()));
    INVENTORY_CHECK(inventory.wal_tail == first_identity.after);
    INVENTORY_CHECK(inventory.wal_frame_count == 1U);
    INVENTORY_CHECK(inventory.released_frame_count == 0U);
    INVENTORY_CHECK(inventory.unresolved_frame_count == 1U);
    INVENTORY_CHECK(inventory.next_release_ordinal == 1U);
    INVENTORY_CHECK(inventory.release_ledger_valid_bytes == 64U);
    INVENTORY_CHECK(inventory.next_unresolved == first_identity);
    INVENTORY_CHECK(
        inventory.released_wal_bytes == first_identity.offset);
    INVENTORY_CHECK(
        inventory.unresolved_wal_bytes == first_identity.bytes);
}

void multiple_bound_pending_frames_report_only_the_front() {
    ReleaseFixture fixture;
    auto first = seal_empty(fixture.durable, 1U);
    release(fixture.durable, first);
    auto second = seal_empty(fixture.durable, 2U);
    const auto second_identity = identity_of(second);
    auto third = seal_empty(fixture.durable, 3U);
    const auto third_identity = identity_of(third);
    auto snapshot = fixture.log.capture_committed();
    INVENTORY_CHECK(snapshot);

    auto rebound = persist::ReleaseLedger::open_or_create(
        fixture.ledger_path.path(), snapshot.value(), ledger_options());
    INVENTORY_CHECK(rebound);
    INVENTORY_CHECK(
        rebound.value().recovery_report().unresolved_wal_frames == 2U);
    auto inspected = persist::UnresolvedReleaseInventory::inspect(
        snapshot.value(), rebound.value());
    INVENTORY_CHECK(inspected);
    const auto& inventory = inspected.value();
    INVENTORY_CHECK(inventory.wal_tail == third_identity.after);
    INVENTORY_CHECK(inventory.wal_frame_count == 3U);
    INVENTORY_CHECK(inventory.released_frame_count == 1U);
    INVENTORY_CHECK(inventory.unresolved_frame_count == 2U);
    INVENTORY_CHECK(inventory.next_release_ordinal == 2U);
    INVENTORY_CHECK(inventory.next_unresolved == second_identity);
    INVENTORY_CHECK(
        inventory.released_wal_bytes == second_identity.offset);
    INVENTORY_CHECK(
        inventory.unresolved_wal_bytes
            == snapshot.value().valid_bytes() - second_identity.offset);
}

void base_cursor_mismatch_fails_closed() {
    ReleaseFixture fixture;
    TempFile foreign_path{".wal"};
    const auto foreign_base = cmd::StreamCursor{
        cmd::FrameNumber{7U},
        doc::Version{9U},
        cmd::CommandId{11U},
    };
    auto foreign = open_log(foreign_path.path(), foreign_base);
    auto snapshot = foreign.capture_committed();
    INVENTORY_CHECK(snapshot);
    auto inspected = persist::UnresolvedReleaseInventory::inspect(
        snapshot.value(), fixture.ledger);
    INVENTORY_CHECK(!inspected);
    INVENTORY_CHECK(
        inspected.error().code
        == persist::UnresolvedReleaseInventoryErrorCode::BaseCursorMismatch);
}

void released_identity_mismatch_fails_closed() {
    ReleaseFixture authority;
    schedule_value(authority.director, 10);
    auto authoritative = seal_empty(authority.durable, 1U);
    release(authority.durable, authoritative);

    TempFile foreign_path{".wal"};
    auto foreign_log = open_log(foreign_path.path());
    cmd::DirectorScheduler foreign_director;
    schedule_value(foreign_director, 11);
    persist::DurableDirector foreign_durable{foreign_director, foreign_log};
    auto foreign = seal_empty(foreign_durable, 1U);
    INVENTORY_CHECK(identity_of(foreign).after == identity_of(authoritative).after);
    INVENTORY_CHECK(identity_of(foreign) != identity_of(authoritative));
    auto snapshot = foreign_log.capture_committed();
    INVENTORY_CHECK(snapshot);

    auto inspected = persist::UnresolvedReleaseInventory::inspect(
        snapshot.value(), authority.ledger);
    INVENTORY_CHECK(!inspected);
    INVENTORY_CHECK(
        inspected.error().code
        == persist::UnresolvedReleaseInventoryErrorCode::ReleasedIdentityMismatch);
}

void a_stale_wal_snapshot_cannot_truncate_the_bound_prefix() {
    TempFile wal_path{".wal"};
    TempFile ledger_path{".release"};
    auto log = open_log(wal_path.path());
    auto stale = log.capture_committed();
    INVENTORY_CHECK(stale);
    auto initial_ledger = open_ledger(ledger_path.path(), stale.value());
    INVENTORY_CHECK(initial_ledger.recovery_report().wal_entry_count == 0U);

    const auto first = append_empty_frame(log, genesis(), 1U);
    const auto second = append_empty_frame(
        log, as_stream_cursor(first.after), 2U);
    auto current = log.capture_committed();
    INVENTORY_CHECK(current);
    auto rebound = persist::ReleaseLedger::open_or_create(
        ledger_path.path(), current.value(), ledger_options());
    INVENTORY_CHECK(rebound);
    INVENTORY_CHECK(rebound.value().recovery_report().wal_tail == second.after);

    auto inspected = persist::UnresolvedReleaseInventory::inspect(
        stale.value(), rebound.value());
    INVENTORY_CHECK(!inspected);
    INVENTORY_CHECK(
        inspected.error().code
        == persist::UnresolvedReleaseInventoryErrorCode::BoundWalPrefixMissing);
}

void post_open_ledger_corruption_is_reported_without_repair() {
    ReleaseFixture fixture;
    auto first = seal_empty(fixture.durable, 1U);
    release(fixture.durable, first);
    auto snapshot = fixture.log.capture_committed();
    INVENTORY_CHECK(snapshot);
    const auto size_before = std::filesystem::file_size(
        fixture.ledger_path.path());
    flip_byte(fixture.ledger_path.path(), 64U + 32U);

    auto inspected = persist::UnresolvedReleaseInventory::inspect(
        snapshot.value(), fixture.ledger);
    INVENTORY_CHECK(!inspected);
    INVENTORY_CHECK(
        inspected.error().code
        == persist::UnresolvedReleaseInventoryErrorCode::LedgerReadFailure);
    INVENTORY_CHECK(inspected.error().release_ledger_error.has_value());
    INVENTORY_CHECK(
        std::filesystem::file_size(fixture.ledger_path.path()) == size_before);
}

void post_capture_wal_corruption_is_reported_without_repair() {
    ReleaseFixture fixture;
    auto first = seal_empty(fixture.durable, 1U);
    release(fixture.durable, first);
    auto snapshot = fixture.log.capture_committed();
    INVENTORY_CHECK(snapshot);
    const auto size_before = std::filesystem::file_size(fixture.wal_path.path());
    flip_byte(fixture.wal_path.path(), first.receipt().offset + 16U);

    auto inspected = persist::UnresolvedReleaseInventory::inspect(
        snapshot.value(), fixture.ledger);
    INVENTORY_CHECK(!inspected);
    INVENTORY_CHECK(
        inspected.error().code
        == persist::UnresolvedReleaseInventoryErrorCode::WalSnapshotFailure);
    INVENTORY_CHECK(inspected.error().command_log_error.has_value());
    INVENTORY_CHECK(
        std::filesystem::file_size(fixture.wal_path.path()) == size_before);
}

void repaired_torn_ledger_exposes_the_wal_frame_as_pending() {
    ReleaseFixture fixture;
    auto first = seal_empty(fixture.durable, 1U);
    const auto first_identity = identity_of(first);
    release(fixture.durable, first);
    auto snapshot = fixture.log.capture_committed();
    INVENTORY_CHECK(snapshot);

    truncate_to(fixture.ledger_path.path(), 64U + 47U);
    auto repaired = persist::ReleaseLedger::open_or_create(
        fixture.ledger_path.path(), snapshot.value(), ledger_options());
    INVENTORY_CHECK(repaired);
    INVENTORY_CHECK(repaired.value().recovery_report().repaired_torn_tail);
    INVENTORY_CHECK(repaired.value().recovery_report().decision_count == 0U);
    const auto repaired_size = std::filesystem::file_size(
        fixture.ledger_path.path());

    auto inspected = persist::UnresolvedReleaseInventory::inspect(
        snapshot.value(), repaired.value());
    INVENTORY_CHECK(inspected);
    INVENTORY_CHECK(inspected.value().unresolved_frame_count == 1U);
    INVENTORY_CHECK(inspected.value().next_unresolved == first_identity);
    INVENTORY_CHECK(
        std::filesystem::file_size(fixture.ledger_path.path()) == repaired_size);
}

void repaired_torn_wal_and_ledger_converge_on_the_same_frontier() {
    ReleaseFixture fixture;
    auto first = seal_empty(fixture.durable, 1U);
    release(fixture.durable, first);
    auto second = seal_empty(fixture.durable, 2U);
    release(fixture.durable, second);

    truncate_to(
        fixture.wal_path.path(),
        second.receipt().offset + second.receipt().bytes_written - 1U);
    truncate_to(fixture.ledger_path.path(), 64U + 128U + 53U);

    auto repaired_log = persist::CommandLog::open_or_create(
        fixture.wal_path.path(), genesis(), wal_options());
    INVENTORY_CHECK(repaired_log);
    INVENTORY_CHECK(repaired_log.value().recovery_report().repaired_torn_tail);
    auto snapshot = repaired_log.value().capture_committed();
    INVENTORY_CHECK(snapshot);
    INVENTORY_CHECK(snapshot.value().tail_cursor() == first.after_cursor());

    auto repaired_ledger = persist::ReleaseLedger::open_or_create(
        fixture.ledger_path.path(), snapshot.value(), ledger_options());
    INVENTORY_CHECK(repaired_ledger);
    INVENTORY_CHECK(
        repaired_ledger.value().recovery_report().repaired_torn_tail);
    INVENTORY_CHECK(
        repaired_ledger.value().recovery_report().decision_count == 1U);

    const auto wal_size = std::filesystem::file_size(fixture.wal_path.path());
    const auto ledger_size = std::filesystem::file_size(
        fixture.ledger_path.path());
    auto inspected = persist::UnresolvedReleaseInventory::inspect(
        snapshot.value(), repaired_ledger.value());
    INVENTORY_CHECK(inspected);
    INVENTORY_CHECK(inspected.value().wal_frame_count == 1U);
    INVENTORY_CHECK(inspected.value().released_frame_count == 1U);
    INVENTORY_CHECK(inspected.value().unresolved_frame_count == 0U);
    INVENTORY_CHECK(inspected.value().released_frontier == identity_of(first).after);
    INVENTORY_CHECK(!inspected.value().next_unresolved);
    INVENTORY_CHECK(
        std::filesystem::file_size(fixture.wal_path.path()) == wal_size);
    INVENTORY_CHECK(
        std::filesystem::file_size(fixture.ledger_path.path()) == ledger_size);
}

void a_release_beyond_a_repaired_wal_cannot_form_an_inventory_pair() {
    ReleaseFixture fixture;
    auto first = seal_empty(fixture.durable, 1U);
    release(fixture.durable, first);
    auto second = seal_empty(fixture.durable, 2U);
    release(fixture.durable, second);

    truncate_to(
        fixture.wal_path.path(),
        second.receipt().offset + second.receipt().bytes_written - 1U);
    auto repaired_log = persist::CommandLog::open_or_create(
        fixture.wal_path.path(), genesis(), wal_options());
    INVENTORY_CHECK(repaired_log);
    INVENTORY_CHECK(repaired_log.value().recovery_report().repaired_torn_tail);
    auto snapshot = repaired_log.value().capture_committed();
    INVENTORY_CHECK(snapshot);
    INVENTORY_CHECK(snapshot.value().tail_cursor() == first.after_cursor());

    auto rejected_ledger = persist::ReleaseLedger::open_or_create(
        fixture.ledger_path.path(), snapshot.value(), ledger_options());
    INVENTORY_CHECK(!rejected_ledger);
    INVENTORY_CHECK(
        rejected_ledger.error().code
        == persist::ReleaseLedgerErrorCode::WalIdentityMismatch);
}

} // namespace

int main() {
    const struct TestCase final {
        const char* name;
        void (*run)();
    } tests[]{
        {"empty sources are equal", empty_sources_are_equal},
        {"fully released sources are equal", fully_released_sources_are_equal},
        {"one forward extension is pending", one_forward_extension_is_pending},
        {"multiple bound pending frames report only the front",
         multiple_bound_pending_frames_report_only_the_front},
        {"base cursor mismatch fails closed", base_cursor_mismatch_fails_closed},
        {"released identity mismatch fails closed",
         released_identity_mismatch_fails_closed},
        {"stale WAL snapshot cannot truncate bound prefix",
         a_stale_wal_snapshot_cannot_truncate_the_bound_prefix},
        {"post-open ledger corruption is reported without repair",
         post_open_ledger_corruption_is_reported_without_repair},
        {"post-capture WAL corruption is reported without repair",
         post_capture_wal_corruption_is_reported_without_repair},
        {"repaired torn ledger exposes WAL as pending",
         repaired_torn_ledger_exposes_the_wal_frame_as_pending},
        {"repaired torn WAL and ledger converge",
         repaired_torn_wal_and_ledger_converge_on_the_same_frontier},
        {"release beyond repaired WAL cannot form an inventory pair",
         a_release_beyond_a_repaired_wal_cannot_form_an_inventory_pair},
    };

    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[fail] " << test.name << ": " << error.what()
                      << '\n';
            return 1;
        }
    }
    return 0;
}
