#include <mediaserver/persist/cohort_journal.hpp>

#include <algorithm>
#include <array>
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

namespace persist = mediaserver::persist;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define JOURNAL_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TempFile final {
public:
    TempFile() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("mediaserver-cohort-journal-" + std::to_string(timestamp)
               + "-" + std::to_string(sequence.fetch_add(1U)) + ".bin");
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

template <typename Id>
[[nodiscard]] Id id(std::uint64_t value) noexcept {
    return Id{value, value + 0x100U};
}

[[nodiscard]] persist::LogCursor genesis() noexcept {
    return persist::LogCursor{0U, 0U, 1U};
}

[[nodiscard]] persist::UnresolvedReleaseInventory empty_inventory() {
    persist::UnresolvedReleaseInventory inventory;
    inventory.base_cursor = genesis();
    inventory.released_frontier = genesis();
    inventory.wal_tail = genesis();
    inventory.next_release_ordinal = 1U;
    inventory.wal_valid_bytes = 64U;
    inventory.release_ledger_valid_bytes = 64U;
    inventory.released_wal_bytes = 64U;
    return inventory;
}

[[nodiscard]] persist::CohortJournalOpenOptions options(
    bool repair = true) noexcept {
    persist::CohortJournalOpenOptions result;
    result.sync_policy = persist::SyncPolicy::Data;
    result.repair_torn_tail = repair;
    return result;
}

[[nodiscard]] persist::CohortJournal open_journal(
    const std::filesystem::path& path,
    const persist::UnresolvedReleaseInventory& inventory = empty_inventory(),
    persist::CohortJournalOpenOptions open_options = options()) {
    auto opened = persist::CohortJournal::open_or_create(
        path,
        id<persist::CohortJournalUuid>(1U),
        id<persist::RecoveryShowId>(2U),
        inventory,
        open_options);
    JOURNAL_CHECK(opened);
    return std::move(opened).value();
}

[[nodiscard]] persist::RecoveryCohort cohort() {
    return persist::RecoveryCohort{
        persist::RecoveryCohortMember{
            id<persist::RecoveryNodeId>(10U),
            id<persist::RecoveryNodeIncarnation>(20U),
        },
        persist::RecoveryCohortMember{
            id<persist::RecoveryNodeId>(30U),
            id<persist::RecoveryNodeIncarnation>(40U),
        },
    };
}

[[nodiscard]] persist::BeginFrame begin_frame(
    persist::LogCursor before = genesis(),
    std::uint64_t token_value = 3U,
    persist::RecoveryCohort members = cohort()) {
    persist::BeginFrame record;
    record.plan_token = id<persist::RecoveryPlanToken>(token_value);
    record.expected_before = before;
    record.next_frame = before.through_frame + 1U;
    record.cut_director_epoch = id<persist::RecoveryDirectorEpoch>(4U);
    record.plan_generation = 1U;
    record.cohort = std::move(members);
    record.empty_confirmation = record.cohort.empty()
        ? persist::EmptyCohortConfirmation::ExplicitlyEmpty
        : persist::EmptyCohortConfirmation::None;
    record.plan_fingerprint = persist::recovery_plan_fingerprint(
        record.plan_generation,
        record.plan_token,
        record.cohort,
        record.empty_confirmation);
    return record;
}

[[nodiscard]] persist::WalFrameIdentity frame_identity(
    persist::LogCursor before,
    std::uint64_t offset,
    std::uint64_t bytes,
    std::uint32_t crc) noexcept {
    auto after = before;
    ++after.through_frame;
    return persist::WalFrameIdentity{before, after, offset, bytes, crc};
}

[[nodiscard]] persist::BindFrame bind_frame(
    const persist::BeginFrame& begin,
    std::uint64_t begin_ordinal,
    persist::WalFrameIdentity frame) noexcept {
    return persist::BindFrame{
        begin.plan_token,
        begin_ordinal,
        frame,
        begin.plan_generation,
        begin.plan_token,
        begin.plan_fingerprint,
    };
}

[[nodiscard]] std::vector<char> read_file(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    JOURNAL_CHECK(input);
    return std::vector<char>(
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{});
}

void write_file(
    const std::filesystem::path& path,
    std::span<const char> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    JOURNAL_CHECK(output);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    JOURNAL_CHECK(output);
}

[[nodiscard]] std::string bytes_to_hex(std::span<const char> bytes) {
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string result;
    result.reserve(bytes.size() * 2U);
    for (const auto byte : bytes) {
        const auto value = static_cast<unsigned char>(byte);
        result.push_back(digits[value >> 4U]);
        result.push_back(digits[value & 0x0fU]);
    }
    return result;
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
    JOURNAL_CHECK(offset + 4U <= bytes.size());
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        bytes[offset + static_cast<std::size_t>(shift / 8U)] =
            static_cast<char>((value >> shift) & 0xffU);
    }
}

void put_u64(
    std::vector<char>& bytes,
    std::size_t offset,
    std::uint64_t value) {
    JOURNAL_CHECK(offset + 8U <= bytes.size());
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        bytes[offset + static_cast<std::size_t>(shift / 8U)] =
            static_cast<char>((value >> shift) & 0xffU);
    }
}

[[nodiscard]] std::uint64_t get_u64(
    const std::vector<char>& bytes,
    std::size_t offset) {
    JOURNAL_CHECK(offset + 8U <= bytes.size());
    std::uint64_t value = 0U;
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(
                     bytes[offset + static_cast<std::size_t>(shift / 8U)]))
            << shift;
    }
    return value;
}

void refresh_record_crcs(
    std::vector<char>& bytes,
    std::size_t record_offset) {
    constexpr std::size_t prefix_size = 64U;
    constexpr std::size_t trailer_size = 24U;
    put_u32(
        bytes,
        record_offset + 40U,
        crc32c(std::span<const char>{bytes}.subspan(record_offset, 40U)));
    const auto total = static_cast<std::size_t>(
        get_u64(bytes, record_offset + 16U));
    JOURNAL_CHECK(total >= prefix_size + trailer_size);
    const auto trailer = record_offset + total - trailer_size;
    put_u32(
        bytes,
        trailer + 16U,
        crc32c(std::span<const char>{bytes}.subspan(
            record_offset, total - 8U)));
    put_u32(
        bytes,
        trailer + 20U,
        crc32c(std::span<const char>{bytes}.subspan(trailer, 20U)));
}

void exact_version_one_golden() {
    TempFile temporary;
    auto inventory = empty_inventory();
    inventory.base_cursor = persist::LogCursor{
        0x0102030405060708ULL,
        0x1112131415161718ULL,
        0x2122232425262728ULL,
    };
    inventory.released_frontier = persist::LogCursor{
        inventory.base_cursor.through_frame + 2U,
        inventory.base_cursor.revision + 4U,
        inventory.base_cursor.next_command + 4U,
    };
    inventory.wal_tail = inventory.released_frontier;
    inventory.wal_frame_count = 2U;
    inventory.released_frame_count = 2U;
    inventory.next_release_ordinal = 3U;
    inventory.released_wal_bytes = 0x3132333435363738ULL;
    inventory.wal_valid_bytes = inventory.released_wal_bytes;
    inventory.release_ledger_valid_bytes = 320U;
    auto opened = persist::CohortJournal::open_or_create(
        temporary.path(),
        persist::CohortJournalUuid{
            0x5152535455565758ULL, 0x6162636465666768ULL},
        persist::RecoveryShowId{
            0x7172737475767778ULL, 0x8182838485868788ULL},
        inventory,
        options());
    JOURNAL_CHECK(opened);
    auto journal = std::move(opened).value();
    auto begin = begin_frame(
        inventory.released_frontier,
        0x111U,
        persist::RecoveryCohort{
            persist::RecoveryCohortMember{
                persist::RecoveryNodeId{0x201U, 0x202U},
                persist::RecoveryNodeIncarnation{0x203U, 0x204U},
            },
        });
    begin.cut_director_epoch =
        persist::RecoveryDirectorEpoch{0x301U, 0x302U};
    begin.plan_fingerprint = persist::recovery_plan_fingerprint(
        begin.plan_generation,
        begin.plan_token,
        begin.cohort,
        begin.empty_confirmation);
    auto appended = journal.append(1U, begin);
    JOURNAL_CHECK(appended);
    const auto actual = bytes_to_hex(read_file(temporary.path()));
    constexpr std::string_view expected{
        "4d53434f484f5254010080000000000058575655545352516867666564636261"
        "7877767574737271888786858483828108070605040302011817161514131211"
        "28272625242322210a070605040302011c171615141312112c27262524232221"
        "383736353433323102000000000000004001000000000000000000002668a4dc"
        "4d53434f485254310100400001000000d8000000000000000100000000000000"
        "0000000080000000464daebb0000000000000000000000000000000000000000"
        "110100000000000011020000000000000a070605040302011c17161514131211"
        "2c272625242322210b0706050403020101030000000000000203000000000000"
        "0100000000000000c06194b9a1bbb6ad01000000000000000000000000000000"
        "0102000000000000020200000000000003020000000000000402000000000000"
        "4d53434f4d4d4954d800000000000000a67aad446740c1f7"};
    if (actual != expected) {
        throw std::runtime_error{"golden bytes: " + actual};
    }
}

void all_record_variants_round_trip_and_operator_retries_are_idempotent() {
    TempFile temporary;
    auto journal = open_journal(temporary.path());
    const auto begin_one = begin_frame();
    auto receipt = journal.append(1U, begin_one);
    JOURNAL_CHECK(receipt && !receipt.value().replayed());
    const auto frame_one = frame_identity(genesis(), 64U, 100U, 0x12345678U);
    const auto bind_one = bind_frame(begin_one, 1U, frame_one);
    JOURNAL_CHECK(journal.append(2U, bind_one));

    persist::AttemptStarted attempt_one{
        frame_one,
        1U,
        id<persist::RecoveryAttemptToken>(50U),
        id<persist::RecoveryDirectorEpoch>(51U),
        id<persist::RecoverySessionNamespace>(52U),
        genesis(),
        frame_one.after,
        1U,
        begin_one.plan_token,
        2U,
    };
    JOURNAL_CHECK(journal.append(3U, attempt_one));

    persist::ReconfigureFrame reconfigure;
    reconfigure.frame = frame_one;
    reconfigure.expected_plan_generation = 1U;
    reconfigure.expected_plan_token = begin_one.plan_token;
    reconfigure.expected_plan_fingerprint = begin_one.plan_fingerprint;
    reconfigure.expected_plan_journal_ordinal = 2U;
    reconfigure.expected_release_frontier = genesis();
    reconfigure.expected_wal_tail = frame_one.after;
    reconfigure.new_plan_generation = 2U;
    reconfigure.new_plan_token = id<persist::RecoveryPlanToken>(60U);
    reconfigure.operator_request_id =
        id<persist::RecoveryOperatorRequestId>(61U);
    reconfigure.reason = persist::ReconfigureFrameReason::NodeUnavailable;
    reconfigure.cohort = cohort();
    reconfigure.new_plan_fingerprint = persist::recovery_plan_fingerprint(
        reconfigure.new_plan_generation,
        reconfigure.new_plan_token,
        reconfigure.cohort,
        reconfigure.empty_confirmation);
    auto reconfigured = journal.append(4U, reconfigure);
    JOURNAL_CHECK(reconfigured);

    persist::AttemptStarted attempt_two{
        frame_one,
        1U,
        id<persist::RecoveryAttemptToken>(70U),
        id<persist::RecoveryDirectorEpoch>(71U),
        id<persist::RecoverySessionNamespace>(72U),
        genesis(),
        frame_one.after,
        2U,
        reconfigure.new_plan_token,
        4U,
    };
    JOURNAL_CHECK(journal.append(5U, attempt_two));
    auto reused_attempt_token = attempt_two;
    reused_attempt_token.attempt_generation = 2U;
    reused_attempt_token.attempt_token = attempt_one.attempt_token;
    auto rejected_reuse = journal.append(6U, reused_attempt_token);
    JOURNAL_CHECK(!rejected_reuse);
    JOURNAL_CHECK(rejected_reuse.error().code
        == persist::CohortJournalErrorCode::InvalidRecord);

    const auto begin_two = begin_frame(frame_one.after, 80U);
    JOURNAL_CHECK(journal.append(6U, begin_two));
    const persist::AbortFrame abort_two{
        begin_two.plan_token,
        6U,
        persist::AbortFrameReason::OperatorCancelled,
    };
    JOURNAL_CHECK(journal.append(7U, abort_two));

    const auto frame_two = frame_identity(
        frame_one.after, 164U, 120U, 0x87654321U);
    auto inventory = empty_inventory();
    inventory.wal_tail = frame_two.after;
    inventory.wal_frame_count = 2U;
    inventory.unresolved_frame_count = 2U;
    inventory.wal_valid_bytes = 284U;
    inventory.unresolved_wal_bytes = 220U;
    inventory.next_unresolved = frame_one;
    journal = open_journal(temporary.path(), inventory);
    persist::AdoptFrame adopt;
    adopt.frame = frame_two;
    adopt.operator_request_id = id<persist::RecoveryOperatorRequestId>(81U);
    adopt.plan_token = id<persist::RecoveryPlanToken>(82U);
    adopt.cohort = cohort();
    adopt.plan_fingerprint = persist::recovery_plan_fingerprint(
        adopt.plan_generation,
        adopt.plan_token,
        adopt.cohort,
        adopt.empty_confirmation);
    auto adopted = journal.append(8U, adopt);
    JOURNAL_CHECK(adopted && !adopted.value().replayed());
    auto retried = journal.append(1U, adopt);
    JOURNAL_CHECK(retried && retried.value().replayed());
    JOURNAL_CHECK(retried.value().ordinal() == adopted.value().ordinal());
    JOURNAL_CHECK(retried.value().record_crc32c()
        == adopted.value().record_crc32c());

    auto conflicting = adopt;
    conflicting.plan_token = id<persist::RecoveryPlanToken>(99U);
    conflicting.plan_fingerprint = persist::recovery_plan_fingerprint(
        conflicting.plan_generation,
        conflicting.plan_token,
        conflicting.cohort,
        conflicting.empty_confirmation);
    auto conflict = journal.append(9U, conflicting);
    JOURNAL_CHECK(!conflict);
    JOURNAL_CHECK(conflict.error().code
        == persist::CohortJournalErrorCode::RequestConflict);
    JOURNAL_CHECK(journal.records().size() == 8U);

    auto reopened = open_journal(temporary.path(), inventory);
    JOURNAL_CHECK(reopened.records() == journal.records());
    JOURNAL_CHECK(reopened.next_ordinal() == 9U);
    JOURNAL_CHECK(reopened.recovery_report().record_count == 8U);
}

void empty_confirmation_and_cohort_canonicality_are_strict() {
    TempFile temporary;
    auto journal = open_journal(temporary.path());

    auto empty = begin_frame(genesis(), 10U, {});
    empty.empty_confirmation = persist::EmptyCohortConfirmation::None;
    empty.plan_fingerprint = persist::recovery_plan_fingerprint(
        empty.plan_generation,
        empty.plan_token,
        empty.cohort,
        empty.empty_confirmation);
    auto rejected_empty = journal.append(1U, empty);
    JOURNAL_CHECK(!rejected_empty);
    JOURNAL_CHECK(rejected_empty.error().code
        == persist::CohortJournalErrorCode::InvalidRecord);

    auto explicit_nonempty = begin_frame();
    explicit_nonempty.empty_confirmation =
        persist::EmptyCohortConfirmation::ExplicitlyEmpty;
    explicit_nonempty.plan_fingerprint = persist::recovery_plan_fingerprint(
        explicit_nonempty.plan_generation,
        explicit_nonempty.plan_token,
        explicit_nonempty.cohort,
        explicit_nonempty.empty_confirmation);
    JOURNAL_CHECK(!journal.append(1U, explicit_nonempty));

    auto unsorted = begin_frame();
    std::reverse(unsorted.cohort.begin(), unsorted.cohort.end());
    unsorted.plan_fingerprint = persist::recovery_plan_fingerprint(
        unsorted.plan_generation,
        unsorted.plan_token,
        unsorted.cohort,
        unsorted.empty_confirmation);
    JOURNAL_CHECK(!journal.append(1U, unsorted));

    auto duplicate_node = begin_frame();
    duplicate_node.cohort[1U].node_id = duplicate_node.cohort[0U].node_id;
    duplicate_node.plan_fingerprint = persist::recovery_plan_fingerprint(
        duplicate_node.plan_generation,
        duplicate_node.plan_token,
        duplicate_node.cohort,
        duplicate_node.empty_confirmation);
    JOURNAL_CHECK(!journal.append(1U, duplicate_node));

    auto zero_id = begin_frame();
    zero_id.cohort[0U].incarnation = {};
    zero_id.plan_fingerprint = persist::recovery_plan_fingerprint(
        zero_id.plan_generation,
        zero_id.plan_token,
        zero_id.cohort,
        zero_id.empty_confirmation);
    JOURNAL_CHECK(!journal.append(1U, zero_id));

    auto bad_fingerprint = begin_frame();
    bad_fingerprint.plan_fingerprint ^= 1U;
    JOURNAL_CHECK(!journal.append(1U, bad_fingerprint));

    auto valid_empty = begin_frame(genesis(), 11U, {});
    auto accepted = journal.append(1U, valid_empty);
    JOURNAL_CHECK(accepted);
}

void stale_ordinals_and_full_state_cas_are_rejected() {
    TempFile temporary;
    auto journal = open_journal(temporary.path());
    const auto begin = begin_frame();
    auto stale = journal.append(2U, begin);
    JOURNAL_CHECK(!stale);
    JOURNAL_CHECK(stale.error().code
        == persist::CohortJournalErrorCode::StaleWrite);
    JOURNAL_CHECK(journal.append(1U, begin));
    const auto frame = frame_identity(genesis(), 64U, 100U, 1U);
    JOURNAL_CHECK(journal.append(2U, bind_frame(begin, 1U, frame)));

    persist::ReconfigureFrame reconfigure;
    reconfigure.frame = frame;
    reconfigure.expected_plan_generation = 1U;
    reconfigure.expected_plan_token = begin.plan_token;
    reconfigure.expected_plan_fingerprint = begin.plan_fingerprint;
    reconfigure.expected_plan_journal_ordinal = 2U;
    reconfigure.expected_release_frontier = genesis();
    reconfigure.expected_wal_tail = frame.after;
    reconfigure.new_plan_generation = 2U;
    reconfigure.new_plan_token = id<persist::RecoveryPlanToken>(90U);
    reconfigure.operator_request_id =
        id<persist::RecoveryOperatorRequestId>(91U);
    reconfigure.cohort = cohort();
    reconfigure.new_plan_fingerprint = persist::recovery_plan_fingerprint(
        reconfigure.new_plan_generation,
        reconfigure.new_plan_token,
        reconfigure.cohort,
        reconfigure.empty_confirmation);

    auto stale_plan = reconfigure;
    stale_plan.expected_plan_journal_ordinal = 1U;
    JOURNAL_CHECK(!journal.append(3U, stale_plan));
    auto stale_tail = reconfigure;
    stale_tail.expected_wal_tail = genesis();
    JOURNAL_CHECK(!journal.append(3U, stale_tail));
    JOURNAL_CHECK(journal.append(3U, reconfigure));

    auto repeated_old_cas = reconfigure;
    repeated_old_cas.operator_request_id =
        id<persist::RecoveryOperatorRequestId>(92U);
    auto rejected = journal.append(4U, repeated_old_cas);
    JOURNAL_CHECK(!rejected);
}

void later_frames_cannot_start_and_unbound_wal_blocks_attempts() {
    TempFile temporary;
    auto journal = open_journal(temporary.path());
    const auto begin_one = begin_frame();
    JOURNAL_CHECK(journal.append(1U, begin_one));
    const auto frame_one = frame_identity(genesis(), 64U, 100U, 1U);
    JOURNAL_CHECK(journal.append(2U, bind_frame(begin_one, 1U, frame_one)));

    const auto begin_two = begin_frame(frame_one.after, 20U);
    JOURNAL_CHECK(journal.append(3U, begin_two));
    const auto frame_two = frame_identity(
        frame_one.after, 164U, 100U, 2U);

    auto inventory = empty_inventory();
    inventory.wal_tail = frame_two.after;
    inventory.wal_frame_count = 2U;
    inventory.unresolved_frame_count = 2U;
    inventory.wal_valid_bytes = 264U;
    inventory.unresolved_wal_bytes = 200U;
    inventory.next_unresolved = frame_one;
    journal = open_journal(temporary.path(), inventory);

    persist::AttemptStarted blocked{
        frame_one,
        1U,
        id<persist::RecoveryAttemptToken>(30U),
        id<persist::RecoveryDirectorEpoch>(31U),
        id<persist::RecoverySessionNamespace>(32U),
        genesis(),
        frame_two.after,
        1U,
        begin_one.plan_token,
        2U,
    };
    auto before_bind = journal.append(4U, blocked);
    JOURNAL_CHECK(!before_bind);
    JOURNAL_CHECK(before_bind.error().code
        == persist::CohortJournalErrorCode::StaleWrite);

    JOURNAL_CHECK(journal.append(4U, bind_frame(begin_two, 3U, frame_two)));
    JOURNAL_CHECK(journal.append(5U, blocked));

    persist::ReconfigureFrame later_reconfigure;
    later_reconfigure.frame = frame_two;
    later_reconfigure.expected_plan_generation = 1U;
    later_reconfigure.expected_plan_token = begin_two.plan_token;
    later_reconfigure.expected_plan_fingerprint = begin_two.plan_fingerprint;
    later_reconfigure.expected_plan_journal_ordinal = 4U;
    later_reconfigure.expected_release_frontier = genesis();
    later_reconfigure.expected_wal_tail = frame_two.after;
    later_reconfigure.new_plan_generation = 2U;
    later_reconfigure.new_plan_token = id<persist::RecoveryPlanToken>(35U);
    later_reconfigure.operator_request_id =
        id<persist::RecoveryOperatorRequestId>(36U);
    later_reconfigure.cohort = cohort();
    later_reconfigure.new_plan_fingerprint =
        persist::recovery_plan_fingerprint(
            later_reconfigure.new_plan_generation,
            later_reconfigure.new_plan_token,
            later_reconfigure.cohort,
            later_reconfigure.empty_confirmation);
    auto rejected_reconfigure = journal.append(6U, later_reconfigure);
    JOURNAL_CHECK(!rejected_reconfigure);
    JOURNAL_CHECK(rejected_reconfigure.error().code
        == persist::CohortJournalErrorCode::InvalidRecord);

    persist::AttemptStarted later{
        frame_two,
        1U,
        id<persist::RecoveryAttemptToken>(40U),
        id<persist::RecoveryDirectorEpoch>(41U),
        id<persist::RecoverySessionNamespace>(42U),
        genesis(),
        frame_two.after,
        1U,
        begin_two.plan_token,
        4U,
    };
    auto rejected_later = journal.append(6U, later);
    JOURNAL_CHECK(!rejected_later);
    JOURNAL_CHECK(rejected_later.error().code
        == persist::CohortJournalErrorCode::InvalidRecord);

    auto reopened = open_journal(temporary.path(), inventory);
    JOURNAL_CHECK(reopened.next_ordinal() == 6U);
    JOURNAL_CHECK(reopened.records().size() == 5U);
}

void policies_creation_and_limits_fail_closed() {
    {
        TempFile temporary;
        auto nondurable = options();
        nondurable.sync_policy = persist::SyncPolicy::None;
        auto opened = persist::CohortJournal::open_or_create(
            temporary.path(),
            id<persist::CohortJournalUuid>(1U),
            id<persist::RecoveryShowId>(2U),
            empty_inventory(),
            nondurable);
        JOURNAL_CHECK(!opened);
        JOURNAL_CHECK(opened.error().code
            == persist::CohortJournalErrorCode::NonDurablePolicy);
    }
    {
        TempFile temporary;
        auto uncovered = empty_inventory();
        uncovered.wal_tail = persist::LogCursor{1U, 0U, 1U};
        uncovered.wal_frame_count = 1U;
        uncovered.unresolved_frame_count = 1U;
        uncovered.wal_valid_bytes = 164U;
        uncovered.unresolved_wal_bytes = 100U;
        uncovered.next_unresolved =
            frame_identity(genesis(), 64U, 100U, 1U);
        auto opened = persist::CohortJournal::open_or_create(
            temporary.path(),
            id<persist::CohortJournalUuid>(1U),
            id<persist::RecoveryShowId>(2U),
            uncovered,
            options());
        JOURNAL_CHECK(!opened);
        JOURNAL_CHECK(opened.error().code
            == persist::CohortJournalErrorCode::UncoveredWalSuffix);
    }
    {
        TempFile temporary;
        auto journal = open_journal(temporary.path());
        persist::AdoptFrame future;
        future.frame = frame_identity(genesis(), 64U, 100U, 1U);
        future.operator_request_id =
            id<persist::RecoveryOperatorRequestId>(200U);
        future.plan_token = id<persist::RecoveryPlanToken>(201U);
        future.cohort = cohort();
        future.plan_fingerprint = persist::recovery_plan_fingerprint(
            future.plan_generation,
            future.plan_token,
            future.cohort,
            future.empty_confirmation);
        auto rejected = journal.append(1U, future);
        JOURNAL_CHECK(!rejected);
        JOURNAL_CHECK(rejected.error().code
            == persist::CohortJournalErrorCode::StaleWrite);
    }
    {
        TempFile temporary;
        auto limited = options();
        limited.limits.max_cohort_members = 1U;
        auto journal = open_journal(
            temporary.path(), empty_inventory(), limited);
        auto rejected = journal.append(1U, begin_frame());
        JOURNAL_CHECK(!rejected);
        JOURNAL_CHECK(rejected.error().code
            == persist::CohortJournalErrorCode::InvalidRecord);
    }
    {
        TempFile temporary;
        auto limited = options();
        limited.limits.max_records = 1U;
        auto journal = open_journal(
            temporary.path(), empty_inventory(), limited);
        const auto begin = begin_frame();
        JOURNAL_CHECK(journal.append(1U, begin));
        auto rejected = journal.append(
            2U,
            persist::AbortFrame{
                begin.plan_token,
                1U,
                persist::AbortFrameReason::OperatorCancelled,
            });
        JOURNAL_CHECK(!rejected);
        JOURNAL_CHECK(rejected.error().code
            == persist::CohortJournalErrorCode::LimitExceeded);
    }
    {
        TempFile temporary;
        auto malformed = empty_inventory();
        malformed.unresolved_wal_bytes = 1U;
        auto opened = persist::CohortJournal::open_or_create(
            temporary.path(),
            id<persist::CohortJournalUuid>(1U),
            id<persist::RecoveryShowId>(2U),
            malformed,
            options());
        JOURNAL_CHECK(!opened);
        JOURNAL_CHECK(opened.error().code
            == persist::CohortJournalErrorCode::InvalidArgument);
    }
    {
        TempFile temporary;
        auto limited = options();
        limited.limits.max_record_bytes = 160U;
        auto journal = open_journal(
            temporary.path(), empty_inventory(), limited);
        auto rejected = journal.append(1U, begin_frame());
        JOURNAL_CHECK(!rejected);
        JOURNAL_CHECK(rejected.error().code
            == persist::CohortJournalErrorCode::LimitExceeded);
    }
}

void every_final_record_truncation_is_repairable_and_only_final() {
    TempFile source_path;
    auto journal = open_journal(source_path.path());
    const auto begin = begin_frame();
    JOURNAL_CHECK(journal.append(1U, begin));
    const persist::AbortFrame abort{
        begin.plan_token,
        1U,
        persist::AbortFrameReason::Superseded,
    };
    JOURNAL_CHECK(journal.append(2U, abort));
    const auto source = read_file(source_path.path());
    const auto final_offset = static_cast<std::size_t>(
        journal.records().back().offset);
    const auto final_bytes = static_cast<std::size_t>(
        journal.records().back().bytes);
    JOURNAL_CHECK(final_offset + final_bytes == source.size());

    for (std::size_t retained = 1U; retained < final_bytes; ++retained) {
        TempFile damaged_path;
        write_file(
            damaged_path.path(),
            std::span<const char>{source}.first(final_offset + retained));
        auto rejected = persist::CohortJournal::open_or_create(
            damaged_path.path(),
            id<persist::CohortJournalUuid>(1U),
            id<persist::RecoveryShowId>(2U),
            empty_inventory(),
            options(false));
        JOURNAL_CHECK(!rejected);
        JOURNAL_CHECK(rejected.error().code
            == persist::CohortJournalErrorCode::TornTail);

        auto repaired = open_journal(damaged_path.path());
        JOURNAL_CHECK(repaired.records().size() == 1U);
        JOURNAL_CHECK(repaired.recovery_report().repaired_torn_tail);
        JOURNAL_CHECK(repaired.recovery_report().truncated_bytes == retained);
        JOURNAL_CHECK(read_file(damaged_path.path()).size() == final_offset);
    }

    TempFile interior_path;
    write_file(interior_path.path(), source);
    auto interior = source;
    const auto first_trailer = static_cast<std::size_t>(
        journal.records().front().offset + journal.records().front().bytes - 24U);
    interior[first_trailer + 20U] = static_cast<char>(
        static_cast<unsigned char>(interior[first_trailer + 20U]) ^ 1U);
    write_file(interior_path.path(), interior);
    auto rejected = persist::CohortJournal::open_or_create(
        interior_path.path(),
        id<persist::CohortJournalUuid>(1U),
        id<persist::RecoveryShowId>(2U),
        empty_inventory(),
        options());
    JOURNAL_CHECK(!rejected);
    JOURNAL_CHECK(rejected.error().code
        == persist::CohortJournalErrorCode::CorruptRecord);
}

void every_final_commit_marker_byte_is_torn_not_committed_corruption() {
    TempFile source_path;
    auto journal = open_journal(source_path.path());
    const auto begin = begin_frame();
    JOURNAL_CHECK(journal.append(1U, begin));
    JOURNAL_CHECK(journal.append(
        2U,
        persist::AbortFrame{
            begin.plan_token,
            1U,
            persist::AbortFrameReason::Superseded,
        }));
    const auto source = read_file(source_path.path());
    const auto final_offset = static_cast<std::size_t>(
        journal.records().back().offset);
    const auto final_bytes = static_cast<std::size_t>(
        journal.records().back().bytes);
    const auto trailer_offset = final_offset + final_bytes - 24U;

    for (std::size_t trailer_byte = 0U;
         trailer_byte < 24U;
         ++trailer_byte) {
        TempFile damaged_path;
        auto damaged = source;
        damaged[trailer_offset + trailer_byte] = static_cast<char>(
            static_cast<unsigned char>(
                damaged[trailer_offset + trailer_byte])
            ^ 1U);
        write_file(damaged_path.path(), damaged);

        auto rejected = persist::CohortJournal::open_or_create(
            damaged_path.path(),
            id<persist::CohortJournalUuid>(1U),
            id<persist::RecoveryShowId>(2U),
            empty_inventory(),
            options(false));
        JOURNAL_CHECK(!rejected);
        JOURNAL_CHECK(rejected.error().code
            == persist::CohortJournalErrorCode::TornTail);

        auto repaired = open_journal(damaged_path.path());
        JOURNAL_CHECK(repaired.records().size() == 1U);
        JOURNAL_CHECK(repaired.recovery_report().repaired_torn_tail);
        JOURNAL_CHECK(repaired.recovery_report().truncated_bytes
            == final_bytes);
        JOURNAL_CHECK(read_file(damaged_path.path()).size() == final_offset);
    }
}

void committed_corruption_chain_and_crossing_lengths_are_fatal() {
    TempFile source_path;
    auto journal = open_journal(source_path.path());
    const auto begin = begin_frame();
    JOURNAL_CHECK(journal.append(1U, begin));
    JOURNAL_CHECK(journal.append(
        2U,
        persist::AbortFrame{
            begin.plan_token,
            1U,
            persist::AbortFrameReason::PlanInvalidated,
        }));
    const auto source = read_file(source_path.path());
    const auto first_offset = static_cast<std::size_t>(
        journal.records()[0U].offset);
    const auto second_offset = static_cast<std::size_t>(
        journal.records()[1U].offset);

    {
        TempFile damaged;
        auto bytes = source;
        bytes[first_offset + 64U] = static_cast<char>(
            static_cast<unsigned char>(bytes[first_offset + 64U]) ^ 1U);
        write_file(damaged.path(), bytes);
        auto opened = persist::CohortJournal::open_or_create(
            damaged.path(),
            id<persist::CohortJournalUuid>(1U),
            id<persist::RecoveryShowId>(2U),
            empty_inventory(),
            options());
        JOURNAL_CHECK(!opened);
        JOURNAL_CHECK(opened.error().code
            == persist::CohortJournalErrorCode::CorruptRecord);
    }
    {
        TempFile damaged;
        auto bytes = source;
        put_u32(bytes, second_offset + 32U, 0xabcdef01U);
        refresh_record_crcs(bytes, second_offset);
        write_file(damaged.path(), bytes);
        auto opened = persist::CohortJournal::open_or_create(
            damaged.path(),
            id<persist::CohortJournalUuid>(1U),
            id<persist::RecoveryShowId>(2U),
            empty_inventory(),
            options());
        JOURNAL_CHECK(!opened);
        JOURNAL_CHECK(opened.error().code
            == persist::CohortJournalErrorCode::ChainMismatch);
    }
    {
        TempFile damaged;
        auto bytes = source;
        // Authenticate a false first-record length that crosses the valid
        // second commit marker. Recovery must find that marker and refuse to
        // truncate the suffix as though it were an incomplete final append.
        put_u64(
            bytes,
            first_offset + 16U,
            static_cast<std::uint64_t>(bytes.size() - first_offset + 1U));
        put_u32(
            bytes,
            first_offset + 36U,
            static_cast<std::uint32_t>(
                bytes.size() - first_offset + 1U - 88U));
        put_u32(
            bytes,
            first_offset + 40U,
            crc32c(std::span<const char>{bytes}.subspan(first_offset, 40U)));
        write_file(damaged.path(), bytes);
        auto opened = persist::CohortJournal::open_or_create(
            damaged.path(),
            id<persist::CohortJournalUuid>(1U),
            id<persist::RecoveryShowId>(2U),
            empty_inventory(),
            options());
        JOURNAL_CHECK(!opened);
        JOURNAL_CHECK(opened.error().code
            == persist::CohortJournalErrorCode::CorruptRecord);
    }
    {
        TempFile damaged;
        auto bytes = source;
        const auto claimed_total = bytes.size() - first_offset;
        put_u64(
            bytes,
            first_offset + 16U,
            static_cast<std::uint64_t>(claimed_total));
        put_u32(
            bytes,
            first_offset + 36U,
            static_cast<std::uint32_t>(claimed_total - 88U));
        put_u32(
            bytes,
            first_offset + 40U,
            crc32c(std::span<const char>{bytes}.subspan(first_offset, 40U)));
        write_file(damaged.path(), bytes);
        auto opened = persist::CohortJournal::open_or_create(
            damaged.path(),
            id<persist::CohortJournalUuid>(1U),
            id<persist::RecoveryShowId>(2U),
            empty_inventory(),
            options());
        JOURNAL_CHECK(!opened);
        JOURNAL_CHECK(opened.error().code
            == persist::CohortJournalErrorCode::CorruptRecord);
    }
    {
        TempFile damaged;
        auto bytes = source;
        bytes[first_offset] = static_cast<char>(
            static_cast<unsigned char>(bytes[first_offset]) ^ 1U);
        write_file(damaged.path(), bytes);
        auto lowered = options();
        lowered.limits.max_record_bytes = 200U;
        auto opened = persist::CohortJournal::open_or_create(
            damaged.path(),
            id<persist::CohortJournalUuid>(1U),
            id<persist::RecoveryShowId>(2U),
            empty_inventory(),
            lowered);
        JOURNAL_CHECK(!opened);
        JOURNAL_CHECK(opened.error().code
            == persist::CohortJournalErrorCode::CorruptRecord);
    }
}

void header_binding_and_non_regression_are_enforced() {
    TempFile temporary;
    auto journal = open_journal(temporary.path());
    const auto bytes = read_file(temporary.path());
    JOURNAL_CHECK(bytes.size() == 128U);

    auto wrong_uuid = persist::CohortJournal::open_or_create(
        temporary.path(),
        id<persist::CohortJournalUuid>(99U),
        id<persist::RecoveryShowId>(2U),
        empty_inventory(),
        options());
    JOURNAL_CHECK(!wrong_uuid);
    JOURNAL_CHECK(wrong_uuid.error().code
        == persist::CohortJournalErrorCode::BindingMismatch);

    TempFile corrupt_header;
    auto damaged = bytes;
    damaged[16U] = static_cast<char>(
        static_cast<unsigned char>(damaged[16U]) ^ 1U);
    write_file(corrupt_header.path(), damaged);
    auto rejected = persist::CohortJournal::open_or_create(
        corrupt_header.path(),
        id<persist::CohortJournalUuid>(1U),
        id<persist::RecoveryShowId>(2U),
        empty_inventory(),
        options());
    JOURNAL_CHECK(!rejected);
    JOURNAL_CHECK(rejected.error().code
        == persist::CohortJournalErrorCode::InvalidHeader);
}

void external_file_growth_and_truncation_latch_the_writer() {
    {
        TempFile temporary;
        auto journal = open_journal(temporary.path());
        std::ofstream external(
            temporary.path(), std::ios::binary | std::ios::app);
        JOURNAL_CHECK(external);
        external.put('\0');
        external.flush();
        JOURNAL_CHECK(external);
        external.close();
        auto rejected = journal.append(1U, begin_frame());
        JOURNAL_CHECK(!rejected);
        JOURNAL_CHECK(rejected.error().code
            == persist::CohortJournalErrorCode::ChainMismatch);
        JOURNAL_CHECK(journal.next_ordinal() == 0U);
    }
    {
        TempFile temporary;
        auto journal = open_journal(temporary.path());
        std::error_code error;
        std::filesystem::resize_file(temporary.path(), 64U, error);
        JOURNAL_CHECK(!error);
        auto rejected = journal.append(1U, begin_frame());
        JOURNAL_CHECK(!rejected);
        JOURNAL_CHECK(rejected.error().code
            == persist::CohortJournalErrorCode::ChainMismatch);
        JOURNAL_CHECK(journal.next_ordinal() == 0U);
    }
    {
        TempFile temporary;
        auto journal = open_journal(temporary.path());
        const auto frame = frame_identity(genesis(), 64U, 100U, 1U);
        auto inventory = empty_inventory();
        inventory.wal_tail = frame.after;
        inventory.wal_frame_count = 1U;
        inventory.unresolved_frame_count = 1U;
        inventory.wal_valid_bytes = 164U;
        inventory.unresolved_wal_bytes = 100U;
        inventory.next_unresolved = frame;
        journal = open_journal(temporary.path(), inventory);
        persist::AdoptFrame adopt;
        adopt.frame = frame;
        adopt.operator_request_id =
            id<persist::RecoveryOperatorRequestId>(300U);
        adopt.plan_token = id<persist::RecoveryPlanToken>(301U);
        adopt.cohort = cohort();
        adopt.plan_fingerprint = persist::recovery_plan_fingerprint(
            adopt.plan_generation,
            adopt.plan_token,
            adopt.cohort,
            adopt.empty_confirmation);
        JOURNAL_CHECK(journal.append(1U, adopt));

        std::ofstream external(
            temporary.path(), std::ios::binary | std::ios::app);
        JOURNAL_CHECK(external);
        external.put('\0');
        external.flush();
        JOURNAL_CHECK(external);
        external.close();
        auto false_replay = journal.append(1U, adopt);
        JOURNAL_CHECK(!false_replay);
        JOURNAL_CHECK(false_replay.error().code
            == persist::CohortJournalErrorCode::ChainMismatch);
        JOURNAL_CHECK(journal.next_ordinal() == 0U);
    }
}

void many_contiguous_begin_bind_records_replay_linearly() {
    TempFile temporary;
    auto journal = open_journal(temporary.path());
    auto before = genesis();
    auto wal_offset = 64U;
    std::optional<persist::WalFrameIdentity> first;
    constexpr std::uint64_t frame_count = 128U;
    for (std::uint64_t index = 0U; index < frame_count; ++index) {
        const auto begin = begin_frame(before, 1'000U + index, {});
        const auto begin_ordinal = index * 2U + 1U;
        JOURNAL_CHECK(journal.append(begin_ordinal, begin));
        const auto frame = frame_identity(
            before, wal_offset, 100U, static_cast<std::uint32_t>(index + 1U));
        if (!first) {
            first = frame;
        }
        JOURNAL_CHECK(journal.append(
            begin_ordinal + 1U,
            bind_frame(begin, begin_ordinal, frame)));
        before = frame.after;
        wal_offset += 100U;
    }

    auto inventory = empty_inventory();
    inventory.wal_tail = before;
    inventory.wal_frame_count = frame_count;
    inventory.unresolved_frame_count = frame_count;
    inventory.wal_valid_bytes = wal_offset;
    inventory.unresolved_wal_bytes = wal_offset - 64U;
    inventory.next_unresolved = first;
    auto reopened = open_journal(temporary.path(), inventory);
    JOURNAL_CHECK(reopened.records().size() == frame_count * 2U);
    JOURNAL_CHECK(reopened.next_ordinal() == frame_count * 2U + 1U);
}

static_assert(!std::is_copy_constructible_v<persist::CohortJournal>);
static_assert(!std::is_copy_assignable_v<persist::CohortJournal>);
static_assert(std::is_move_constructible_v<persist::CohortJournal>);
static_assert(!std::is_copy_constructible_v<persist::CohortJournalReceipt>);

} // namespace

int main() {
    struct Test final {
        const char* name;
        void (*run)();
    };
    const std::array tests{
        Test{"exact version-one golden", &exact_version_one_golden},
        Test{"all variants and idempotent retries",
             &all_record_variants_round_trip_and_operator_retries_are_idempotent},
        Test{"canonical cohorts and empty confirmation",
             &empty_confirmation_and_cohort_canonicality_are_strict},
        Test{"stale ordinal and full-state CAS",
             &stale_ordinals_and_full_state_cas_are_rejected},
        Test{"single next unresolved attempt and reopen closure",
             &later_frames_cannot_start_and_unbound_wal_blocks_attempts},
        Test{"policies, creation, and limits",
             &policies_creation_and_limits_fail_closed},
        Test{"exhaustive final truncation",
             &every_final_record_truncation_is_repairable_and_only_final},
        Test{"every final commit-marker byte",
             &every_final_commit_marker_byte_is_torn_not_committed_corruption},
        Test{"committed corruption and chains",
             &committed_corruption_chain_and_crossing_lengths_are_fatal},
        Test{"header binding and non-regression",
             &header_binding_and_non_regression_are_enforced},
        Test{"external file mutation latches writer",
             &external_file_growth_and_truncation_latch_the_writer},
        Test{"many contiguous plans replay",
             &many_contiguous_begin_bind_records_replay_linearly},
    };
    std::size_t failures = 0U;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what()
                      << '\n';
        }
    }
    return failures == 0U ? 0 : 1;
}
