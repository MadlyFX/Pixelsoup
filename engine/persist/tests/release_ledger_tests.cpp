#include <mediaserver/persist/durable_director.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

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

#define RELEASE_CHECK(expression) \
    check(static_cast<bool>(expression), #expression, __LINE__)

class TempFile final {
public:
    explicit TempFile(std::string_view suffix = ".bin") {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("mediaserver-release-" + std::to_string(timestamp) + "-"
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

[[nodiscard]] persist::OpenOptions wal_options() noexcept {
    persist::OpenOptions options;
    options.sync_policy = persist::SyncPolicy::Data;
    return options;
}

[[nodiscard]] persist::ReleaseLedgerOpenOptions ledger_options(
    bool repair = true) noexcept {
    persist::ReleaseLedgerOpenOptions options;
    options.sync_policy = persist::SyncPolicy::Data;
    options.repair_torn_tail = repair;
    return options;
}

[[nodiscard]] cmd::FrameSeal empty_seal(
    std::uint64_t frame,
    std::uint64_t revision = 0U) {
    return cmd::FrameSeal{
        cmd::FrameNumber{frame},
        doc::Version{revision},
        doc::Version{revision},
        {},
    };
}

[[nodiscard]] persist::WalFrameIdentity append_empty_frame(
    persist::CommandLog& log,
    const cmd::StreamCursor& before,
    std::uint64_t frame) {
    auto appended = log.append(
        std::span<const cmd::Command>{},
        empty_seal(frame, before.revision.value()));
    RELEASE_CHECK(appended);
    const auto& receipt = appended.value();
    return persist::WalFrameIdentity{
        as_log_cursor(before),
        receipt.cursor,
        receipt.offset,
        receipt.bytes_written,
        receipt.entry_crc32c,
    };
}

template <typename Ledger>
concept HasPublicReleaseAppend = requires(
    Ledger& ledger,
    const persist::WalFrameIdentity& identity) {
    ledger.append_release(identity);
};

[[nodiscard]] persist::WalFrameIdentity seal_and_release_empty_frame(
    persist::DurableDirector& durable,
    std::uint64_t frame) {
    auto sealed = durable.seal_frame(cmd::FrameNumber{frame});
    RELEASE_CHECK(sealed);
    const auto ordinal = durable.next_release_ordinal();
    RELEASE_CHECK(ordinal != 0U);
    auto released = durable.persist_release(sealed.value(), ordinal);
    RELEASE_CHECK(released);
    RELEASE_CHECK(released.value().durable());
    return released.value().frame();
}

[[nodiscard]] std::vector<char> read_file(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    RELEASE_CHECK(input);
    return std::vector<char>(
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{});
}

[[nodiscard]] std::string bytes_to_hex(std::span<const char> bytes) {
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string encoded;
    encoded.reserve(bytes.size() * 2U);
    for (const auto byte : bytes) {
        const auto value = static_cast<unsigned char>(byte);
        encoded.push_back(digits[value >> 4U]);
        encoded.push_back(digits[value & 0x0fU]);
    }
    return encoded;
}

void write_prefix(
    const std::filesystem::path& path,
    const std::vector<char>& bytes,
    std::size_t count) {
    RELEASE_CHECK(count <= bytes.size());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    RELEASE_CHECK(output);
    output.write(bytes.data(), static_cast<std::streamsize>(count));
    output.flush();
    RELEASE_CHECK(output);
}

[[nodiscard]] std::uint32_t crc32c(std::span<const char> bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const auto value : bytes) {
        crc ^= static_cast<std::uint8_t>(
            static_cast<unsigned char>(value));
        for (std::uint32_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U)
                ^ ((crc & 1U) != 0U ? 0x82f63b78U : 0U);
        }
    }
    return ~crc;
}

void put_u32(
    std::vector<char>& bytes,
    std::size_t offset,
    std::uint32_t value) {
    RELEASE_CHECK(offset + 4U <= bytes.size());
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        bytes[offset + static_cast<std::size_t>(shift / 8U)] =
            static_cast<char>((value >> shift) & 0xffU);
    }
}

void put_u64(
    std::vector<char>& bytes,
    std::size_t offset,
    std::uint64_t value) {
    RELEASE_CHECK(offset + 8U <= bytes.size());
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        bytes[offset + static_cast<std::size_t>(shift / 8U)] =
            static_cast<char>((value >> shift) & 0xffU);
    }
}

void refresh_release_entry_crcs(
    std::vector<char>& bytes,
    std::size_t entry_offset) {
    constexpr std::size_t prefix_size = 104U;
    constexpr std::size_t prefix_crc_offset = 100U;
    constexpr std::size_t entry_crc_offset = 120U;
    constexpr std::size_t trailer_crc_offset = 124U;
    put_u32(
        bytes,
        entry_offset + prefix_crc_offset,
        crc32c(std::span<const char>{bytes}.subspan(
            entry_offset, prefix_crc_offset)));
    put_u32(
        bytes,
        entry_offset + entry_crc_offset,
        crc32c(std::span<const char>{bytes}.subspan(
            entry_offset, entry_crc_offset)));
    put_u32(
        bytes,
        entry_offset + trailer_crc_offset,
        crc32c(std::span<const char>{bytes}.subspan(
            entry_offset + prefix_size, 20U)));
}

[[nodiscard]] std::uint32_t read_u32(
    const std::vector<char>& bytes,
    std::size_t offset) {
    RELEASE_CHECK(offset + 4U <= bytes.size());
    std::uint32_t value = 0U;
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(
                     bytes[offset + static_cast<std::size_t>(shift / 8U)]))
            << shift;
    }
    return value;
}

void version_one_exact_byte_golden() {
    TempFile wal_path{".wal"};
    TempFile ledger_path{".release"};
    const auto base = cmd::StreamCursor{
        cmd::FrameNumber{0x0102030405060708ULL},
        doc::Version{0x1112131415161718ULL},
        cmd::CommandId{0x2122232425262728ULL},
    };
    auto opened = persist::CommandLog::open_or_create(
        wal_path.path(), base, wal_options());
    RELEASE_CHECK(opened);
    auto log = std::move(opened).value();
    auto empty = log.capture_committed();
    RELEASE_CHECK(empty);
    auto created = persist::ReleaseLedger::open_or_create(
        ledger_path.path(), empty.value(), ledger_options());
    RELEASE_CHECK(created);
    auto ledger = std::move(created).value();

    auto restored = doc::Root::restore(
        std::span<const doc::Node>{}, base.revision);
    RELEASE_CHECK(restored);
    cmd::DirectorScheduler director{
        std::move(*restored), base.through_frame, base.next_command};
    persist::DurableDirector durable{director, log, ledger};
    const auto identity = seal_and_release_empty_frame(
        durable, base.through_frame.value() + 1U);
    RELEASE_CHECK(identity.before == as_log_cursor(base));

    // Complete v1 file: 64-byte header plus one 104-byte decision prefix and
    // 24-byte commit trailer. The decision was minted through DurableDirector
    // from the exact WAL entry whose real byte length and CRC are frozen here.
    constexpr std::string_view expected_hex =
        "4d5352454c4c4f47010040000000000008070605040302011817161514131211"
        "28272625242322210000000000000000000000000000000000000000b764f646"
        "4d5352454c455331010068000000000080000000000000000100000000000000"
        "0807060504030201181716151413121128272625242322210907060504030201"
        "181716151413121128272625242322214000000000000000c400000000000000"
        "efbc6afaefc895f94d53434f4d4d4954800000000000000022bb025017f15970";
    const auto actual = read_file(ledger_path.path());
    RELEASE_CHECK(actual.size() == 192U);
    RELEASE_CHECK(bytes_to_hex(actual) == expected_hex);
}

void creation_round_trip_and_exact_wal_identity() {
    static_assert(!std::is_copy_constructible_v<
                  persist::DurableReleaseReceipt>);
    static_assert(std::is_nothrow_move_constructible_v<
                  persist::DurableReleaseReceipt>);

    TempFile wal_path{".wal"};
    TempFile ledger_path{".release"};
    auto opened = persist::CommandLog::open_or_create(
        wal_path.path(), genesis(), wal_options());
    RELEASE_CHECK(opened);
    auto log = std::move(opened).value();
    auto empty_snapshot = log.capture_committed();
    RELEASE_CHECK(empty_snapshot);
    auto created = persist::ReleaseLedger::open_or_create(
        ledger_path.path(), empty_snapshot.value(), ledger_options());
    RELEASE_CHECK(created);
    auto ledger = std::move(created).value();
    RELEASE_CHECK(ledger.frontier() == as_log_cursor(genesis()));
    RELEASE_CHECK(ledger.next_release_ordinal() == 1U);

    cmd::DirectorScheduler director;
    persist::DurableDirector durable{director, log, ledger};
    auto first_batch = durable.seal_frame(cmd::FrameNumber{1U});
    RELEASE_CHECK(first_batch);
    auto first_release = durable.persist_release(first_batch.value(), 1U);
    RELEASE_CHECK(first_release);
    const auto first = first_release.value().frame();
    const auto first_wal_bytes = read_file(wal_path.path());
    constexpr std::size_t trailer_size = 24U;
    const auto stored_crc_offset = static_cast<std::size_t>(
        first.offset + first.bytes - trailer_size + 16U);
    RELEASE_CHECK(
        read_u32(first_wal_bytes, stored_crc_offset) == first.entry_crc32c);

    RELEASE_CHECK(first_release.value().durable());
    RELEASE_CHECK(first_release.value().ordinal() == 1U);
    RELEASE_CHECK(first_release.value().offset() == 64U);
    RELEASE_CHECK(first_release.value().bytes_written() == 128U);
    RELEASE_CHECK(first_release.value().frame() == first);
    RELEASE_CHECK(ledger.next_release_ordinal() == 2U);
    RELEASE_CHECK(
        ledger.release_ordinal(first.before, first.after)
        == std::optional<std::uint64_t>{1U});
    auto wrong_first_before = first.before;
    ++wrong_first_before.revision;
    RELEASE_CHECK(
        !ledger.release_ordinal(wrong_first_before, first.after));
    auto moved_receipt = std::move(first_release).value();
    RELEASE_CHECK(moved_receipt);

    auto second_batch = durable.seal_frame(cmd::FrameNumber{2U});
    RELEASE_CHECK(second_batch);
    auto second_release = durable.persist_release(second_batch.value(), 2U);
    RELEASE_CHECK(second_release);
    const auto second = second_release.value().frame();
    RELEASE_CHECK(second_release.value().ordinal() == 2U);
    RELEASE_CHECK(ledger.frontier() == second.after);
    RELEASE_CHECK(ledger.next_release_ordinal() == 3U);
    RELEASE_CHECK(
        ledger.release_ordinal(second.before, second.after)
        == std::optional<std::uint64_t>{2U});

    auto captured = log.capture_committed();
    RELEASE_CHECK(captured);
    auto reopened = persist::ReleaseLedger::open_or_create(
        ledger_path.path(), captured.value(), ledger_options());
    RELEASE_CHECK(reopened);
    const auto report = reopened.value().recovery_report();
    RELEASE_CHECK(report.base_cursor == as_log_cursor(genesis()));
    RELEASE_CHECK(report.frontier == second.after);
    RELEASE_CHECK(report.wal_tail == second.after);
    RELEASE_CHECK(report.decision_count == 2U);
    RELEASE_CHECK(report.wal_entry_count == 2U);
    RELEASE_CHECK(report.unresolved_wal_frames == 0U);
    RELEASE_CHECK(report.released_wal_bytes == captured.value().valid_bytes());
    RELEASE_CHECK(report.valid_bytes == 64U + 2U * 128U);
    RELEASE_CHECK(reopened.value().next_release_ordinal() == 3U);
    RELEASE_CHECK(
        reopened.value().release_ordinal(first.before, first.after)
        == std::optional<std::uint64_t>{1U});
    RELEASE_CHECK(
        reopened.value().release_ordinal(second.before, second.after)
        == std::optional<std::uint64_t>{2U});

    const auto ledger_bytes = read_file(ledger_path.path());
    RELEASE_CHECK(ledger_bytes.size() == 320U);
    RELEASE_CHECK((std::string_view{ledger_bytes.data(), 8U} == "MSRELLOG"));
    RELEASE_CHECK((std::string_view{ledger_bytes.data() + 64U, 8U}
                   == "MSRELES1"));
    RELEASE_CHECK((std::string_view{ledger_bytes.data() + 168U, 8U}
                   == "MSCOMMIT"));
}

void creation_rejects_nonempty_wal_and_none_policy() {
    TempFile wal_path{".wal"};
    TempFile missing_ledger{".release"};
    auto opened = persist::CommandLog::open_or_create(
        wal_path.path(), genesis(), wal_options());
    RELEASE_CHECK(opened);
    auto log = std::move(opened).value();
    static_cast<void>(append_empty_frame(log, genesis(), 1U));
    auto nonempty = log.capture_committed();
    RELEASE_CHECK(nonempty);
    auto missing = persist::ReleaseLedger::open_or_create(
        missing_ledger.path(), nonempty.value(), ledger_options());
    RELEASE_CHECK(!missing);
    RELEASE_CHECK(
        missing.error().code
        == persist::ReleaseLedgerErrorCode::MissingForNonEmptyWal);
    RELEASE_CHECK(!std::filesystem::exists(missing_ledger.path()));

    TempFile empty_wal_path{".wal"};
    TempFile none_ledger{".release"};
    auto empty_open = persist::CommandLog::open_or_create(
        empty_wal_path.path(), genesis(), wal_options());
    RELEASE_CHECK(empty_open);
    auto empty_log = std::move(empty_open).value();
    auto empty = empty_log.capture_committed();
    RELEASE_CHECK(empty);
    auto options = ledger_options();
    options.sync_policy = persist::SyncPolicy::None;
    auto nondurable = persist::ReleaseLedger::open_or_create(
        none_ledger.path(), empty.value(), options);
    RELEASE_CHECK(!nondurable);
    RELEASE_CHECK(
        nondurable.error().code
        == persist::ReleaseLedgerErrorCode::NonDurablePolicy);
    RELEASE_CHECK(!std::filesystem::exists(none_ledger.path()));
}

void existing_empty_ledger_reports_unresolved_wal_suffix() {
    TempFile wal_path{".wal"};
    TempFile ledger_path{".release"};
    auto opened = persist::CommandLog::open_or_create(
        wal_path.path(), genesis(), wal_options());
    RELEASE_CHECK(opened);
    auto log = std::move(opened).value();
    auto empty = log.capture_committed();
    RELEASE_CHECK(empty);
    auto created = persist::ReleaseLedger::open_or_create(
        ledger_path.path(), empty.value(), ledger_options());
    RELEASE_CHECK(created);

    const auto first = append_empty_frame(log, genesis(), 1U);
    const auto first_after = cmd::StreamCursor{
        cmd::FrameNumber{first.after.through_frame},
        doc::Version{first.after.revision},
        cmd::CommandId{first.after.next_command},
    };
    const auto second = append_empty_frame(log, first_after, 2U);
    auto captured = log.capture_committed();
    RELEASE_CHECK(captured);
    auto reopened = persist::ReleaseLedger::open_or_create(
        ledger_path.path(), captured.value(), ledger_options());
    RELEASE_CHECK(reopened);
    const auto report = reopened.value().recovery_report();
    RELEASE_CHECK(report.frontier == as_log_cursor(genesis()));
    RELEASE_CHECK(report.wal_tail == second.after);
    RELEASE_CHECK(report.decision_count == 0U);
    RELEASE_CHECK(report.wal_entry_count == 2U);
    RELEASE_CHECK(report.unresolved_wal_frames == 2U);
}

struct ReleasedFixture final {
    TempFile wal_path{".wal"};
    TempFile ledger_path{".release"};
    persist::CommandLog log;
    persist::ReleaseLedger ledger;
    cmd::DirectorScheduler director;
    persist::DurableDirector durable;
    persist::WalFrameIdentity first;

    ReleasedFixture()
        : log(open_log()),
          ledger(open_ledger()),
          durable(director, log, ledger),
          first(seal_and_release_empty_frame(durable, 1U)) {}

    [[nodiscard]] persist::CommandLog open_log() {
        auto opened = persist::CommandLog::open_or_create(
            wal_path.path(), genesis(), wal_options());
        RELEASE_CHECK(opened);
        return std::move(opened).value();
    }

    [[nodiscard]] persist::ReleaseLedger open_ledger() {
        auto empty = log.capture_committed();
        RELEASE_CHECK(empty);
        auto created = persist::ReleaseLedger::open_or_create(
            ledger_path.path(), empty.value(), ledger_options());
        RELEASE_CHECK(created);
        return std::move(created).value();
    }
};

void every_partial_release_entry_is_torn_and_repairable() {
    ReleasedFixture source;
    auto snapshot = source.log.capture_committed();
    RELEASE_CHECK(snapshot);
    const auto complete = read_file(source.ledger_path.path());
    RELEASE_CHECK(complete.size() == 192U);

    for (std::size_t cut = 65U; cut < complete.size(); ++cut) {
        TempFile partial{".release"};
        write_prefix(partial.path(), complete, cut);
        auto rejected = persist::ReleaseLedger::open_or_create(
            partial.path(), snapshot.value(), ledger_options(false));
        RELEASE_CHECK(!rejected);
        RELEASE_CHECK(
            rejected.error().code == persist::ReleaseLedgerErrorCode::TornTail);
        RELEASE_CHECK(std::filesystem::file_size(partial.path()) == cut);

        auto repaired = persist::ReleaseLedger::open_or_create(
            partial.path(), snapshot.value(), ledger_options());
        RELEASE_CHECK(repaired);
        const auto report = repaired.value().recovery_report();
        RELEASE_CHECK(report.repaired_torn_tail);
        RELEASE_CHECK(report.truncated_bytes == cut - 64U);
        RELEASE_CHECK(report.valid_bytes == 64U);
        RELEASE_CHECK(report.frontier == as_log_cursor(genesis()));
        RELEASE_CHECK(report.unresolved_wal_frames == 1U);
    }
}

void every_invalid_final_trailer_is_torn_and_repairable() {
    ReleasedFixture source;
    auto snapshot = source.log.capture_committed();
    RELEASE_CHECK(snapshot);
    const auto complete = read_file(source.ledger_path.path());
    constexpr std::size_t trailer_offset = 64U + 104U;
    constexpr std::size_t trailer_size = 24U;

    for (std::size_t byte = 0U; byte < trailer_size; ++byte) {
        TempFile damaged{".release"};
        auto bytes = complete;
        bytes[trailer_offset + byte] = static_cast<char>(
            static_cast<unsigned char>(bytes[trailer_offset + byte]) ^ 0x01U);
        write_prefix(damaged.path(), bytes, bytes.size());
        auto repaired = persist::ReleaseLedger::open_or_create(
            damaged.path(), snapshot.value(), ledger_options());
        RELEASE_CHECK(repaired);
        RELEASE_CHECK(repaired.value().recovery_report().repaired_torn_tail);
        RELEASE_CHECK(repaired.value().recovery_report().valid_bytes == 64U);
    }
}

void committed_body_and_interior_corruption_are_fatal() {
    ReleasedFixture final_source;
    auto final_snapshot = final_source.log.capture_committed();
    RELEASE_CHECK(final_snapshot);
    auto final_bytes = read_file(final_source.ledger_path.path());
    final_bytes[64U + 32U] = static_cast<char>(
        static_cast<unsigned char>(final_bytes[64U + 32U]) ^ 0x01U);
    TempFile corrupt_final{".release"};
    write_prefix(corrupt_final.path(), final_bytes, final_bytes.size());
    auto final_open = persist::ReleaseLedger::open_or_create(
        corrupt_final.path(), final_snapshot.value(), ledger_options());
    RELEASE_CHECK(!final_open);
    RELEASE_CHECK(
        final_open.error().code
        == persist::ReleaseLedgerErrorCode::CorruptEntry);
    RELEASE_CHECK(
        std::filesystem::file_size(corrupt_final.path()) == final_bytes.size());

    ReleasedFixture interior_source;
    static_cast<void>(seal_and_release_empty_frame(
        interior_source.durable, 2U));
    auto interior_snapshot = interior_source.log.capture_committed();
    RELEASE_CHECK(interior_snapshot);
    auto interior_bytes = read_file(interior_source.ledger_path.path());
    interior_bytes[64U + 104U] = static_cast<char>(
        static_cast<unsigned char>(interior_bytes[64U + 104U]) ^ 0x01U);
    TempFile corrupt_interior{".release"};
    write_prefix(
        corrupt_interior.path(), interior_bytes, interior_bytes.size());
    auto interior_open = persist::ReleaseLedger::open_or_create(
        corrupt_interior.path(),
        interior_snapshot.value(),
        ledger_options());
    RELEASE_CHECK(!interior_open);
    RELEASE_CHECK(
        interior_open.error().code
        == persist::ReleaseLedgerErrorCode::CorruptEntry);
    RELEASE_CHECK(std::filesystem::file_size(corrupt_interior.path())
                  == interior_bytes.size());
}

void structural_chain_and_exact_wal_identity_mismatches_are_rejected() {
    ReleasedFixture source;
    auto snapshot = source.log.capture_committed();
    RELEASE_CHECK(snapshot);
    const auto complete = read_file(source.ledger_path.path());

    TempFile bad_ordinal{".release"};
    auto ordinal_bytes = complete;
    put_u64(ordinal_bytes, 64U + 24U, 2U);
    refresh_release_entry_crcs(ordinal_bytes, 64U);
    write_prefix(bad_ordinal.path(), ordinal_bytes, ordinal_bytes.size());
    auto ordinal_open = persist::ReleaseLedger::open_or_create(
        bad_ordinal.path(), snapshot.value(), ledger_options());
    RELEASE_CHECK(!ordinal_open);
    RELEASE_CHECK(
        ordinal_open.error().code
        == persist::ReleaseLedgerErrorCode::ChainMismatch);

    TempFile wrong_identity{".release"};
    auto identity_bytes = complete;
    put_u32(
        identity_bytes,
        64U + 96U,
        source.first.entry_crc32c ^ 0x01010101U);
    refresh_release_entry_crcs(identity_bytes, 64U);
    write_prefix(
        wrong_identity.path(), identity_bytes, identity_bytes.size());
    auto identity_open = persist::ReleaseLedger::open_or_create(
        wrong_identity.path(), snapshot.value(), ledger_options());
    RELEASE_CHECK(!identity_open);
    RELEASE_CHECK(
        identity_open.error().code
        == persist::ReleaseLedgerErrorCode::WalIdentityMismatch);

    TempFile wrong_cursor{".release"};
    auto cursor_bytes = complete;
    put_u64(cursor_bytes, 64U + 64U, 1U);
    put_u64(cursor_bytes, 64U + 72U, 2U);
    refresh_release_entry_crcs(cursor_bytes, 64U);
    write_prefix(wrong_cursor.path(), cursor_bytes, cursor_bytes.size());
    auto cursor_open = persist::ReleaseLedger::open_or_create(
        wrong_cursor.path(), snapshot.value(), ledger_options());
    RELEASE_CHECK(!cursor_open);
    RELEASE_CHECK(
        cursor_open.error().code
        == persist::ReleaseLedgerErrorCode::WalIdentityMismatch);
}

void release_append_is_private_and_bridge_rejection_is_retryable() {
    static_assert(!HasPublicReleaseAppend<persist::ReleaseLedger>);

    TempFile wal_path{".wal"};
    TempFile ledger_path{".release"};
    auto opened = persist::CommandLog::open_or_create(
        wal_path.path(), genesis(), wal_options());
    RELEASE_CHECK(opened);
    auto log = std::move(opened).value();
    auto empty = log.capture_committed();
    RELEASE_CHECK(empty);
    auto created = persist::ReleaseLedger::open_or_create(
        ledger_path.path(), empty.value(), ledger_options());
    RELEASE_CHECK(created);
    auto ledger = std::move(created).value();
    cmd::DirectorScheduler director;
    persist::DurableDirector durable{director, log, ledger};
    auto first = durable.seal_frame(cmd::FrameNumber{1U});
    RELEASE_CHECK(first);
    const auto before_size = std::filesystem::file_size(ledger_path.path());
    auto rejected = durable.persist_release(first.value(), 2U);
    RELEASE_CHECK(!rejected);
    RELEASE_CHECK(
        rejected.error().code
        == persist::DurableDirectorErrorCode::ReleasePrewriteRejected);
    RELEASE_CHECK(std::filesystem::file_size(ledger_path.path()) == before_size);

    auto accepted = durable.persist_release(first.value(), 1U);
    RELEASE_CHECK(accepted);
    RELEASE_CHECK(ledger.frontier() == accepted.value().frame().after);
}

void historical_lookup_authenticates_the_exact_disk_record() {
    ReleasedFixture source;
    auto snapshot = source.log.capture_committed();
    RELEASE_CHECK(snapshot);
    auto opened = persist::ReleaseLedger::open_or_create(
        source.ledger_path.path(), snapshot.value(), ledger_options());
    RELEASE_CHECK(opened);
    auto ledger = std::move(opened).value();
    RELEASE_CHECK(
        ledger.release_ordinal(source.first.before, source.first.after)
        == std::optional<std::uint64_t>{1U});

    auto damaged = read_file(source.ledger_path.path());
    damaged[64U + 32U] = static_cast<char>(
        static_cast<unsigned char>(damaged[64U + 32U]) ^ 0x01U);
    write_prefix(
        source.ledger_path.path(), damaged, damaged.size());
    RELEASE_CHECK(
        !ledger.release_ordinal(source.first.before, source.first.after));
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"version one exact-byte golden", &version_one_exact_byte_golden},
        {"creation round trip and exact WAL identity",
         &creation_round_trip_and_exact_wal_identity},
        {"creation rejects nonempty WAL and None policy",
         &creation_rejects_nonempty_wal_and_none_policy},
        {"existing empty ledger reports unresolved suffix",
         &existing_empty_ledger_reports_unresolved_wal_suffix},
        {"every partial release entry repairs",
         &every_partial_release_entry_is_torn_and_repairable},
        {"every invalid final trailer repairs",
         &every_invalid_final_trailer_is_torn_and_repairable},
        {"committed body and interior corruption are fatal",
         &committed_body_and_interior_corruption_are_fatal},
        {"structural and WAL identity mismatches reject",
         &structural_chain_and_exact_wal_identity_mismatches_are_rejected},
        {"release append private; bridge rejection retryable",
         &release_append_is_private_and_bridge_rejection_is_retryable},
        {"historical lookup authenticates its disk record",
         &historical_lookup_authenticates_the_exact_disk_record},
    };

    std::size_t failures = 0U;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[pass] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << (tests.size() - failures) << '/' << tests.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
