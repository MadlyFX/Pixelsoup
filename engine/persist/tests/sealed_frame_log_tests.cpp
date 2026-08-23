#include <mediaserver/persist/sealed_frame_log.hpp>

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
#include <system_error>
#include <utility>
#include <vector>

namespace {

using mediaserver::persist::LogCursor;
using mediaserver::persist::LogErrorCode;
using mediaserver::persist::LogStatus;
using mediaserver::persist::OpenOptions;
using mediaserver::persist::RecordBytes;
using mediaserver::persist::SealedFrameBytes;
using mediaserver::persist::SealedFrameLog;
using mediaserver::persist::SyncPolicy;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line) + ": check failed: " + expression};
    }
}

#define WAL_CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

class TempWal final {
public:
    TempWal() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("mediaserver-wal-" + std::to_string(timestamp) + "-" +
                 std::to_string(sequence.fetch_add(1U)) + ".bin");
    }

    ~TempWal() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TempWal(const TempWal&) = delete;
    TempWal& operator=(const TempWal&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

[[nodiscard]] RecordBytes record(std::initializer_list<std::uint8_t> values) {
    RecordBytes bytes;
    bytes.reserve(values.size());
    for (const auto value : values) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

[[nodiscard]] SealedFrameBytes first_frame() {
    return SealedFrameBytes{
        41U,
        8U,
        10U,
        100U,
        102U,
        {record({0x10U, 0x11U}), record({0x20U}), record({0xf0U, 0x01U})}};
}

[[nodiscard]] SealedFrameBytes empty_second_frame() {
    return SealedFrameBytes{42U, 10U, 10U, 102U, 102U, {record({0xf0U, 0x02U})}};
}

[[nodiscard]] SealedFrameBytes empty_third_frame() {
    return SealedFrameBytes{43U, 10U, 10U, 102U, 102U, {record({0xf0U, 0x03U})}};
}

[[nodiscard]] OpenOptions test_options(bool repair_torn_tail = true) {
    OpenOptions options;
    options.sync_policy = SyncPolicy::None;
    options.repair_torn_tail = repair_torn_tail;
    return options;
}

[[nodiscard]] std::string bytes_to_hex(const std::vector<char>& bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(bytes.size() * 2U);
    for (const auto value : bytes) {
        const auto byte = static_cast<std::uint8_t>(static_cast<unsigned char>(value));
        hex.push_back(digits[static_cast<std::size_t>(byte >> 4U)]);
        hex.push_back(digits[static_cast<std::size_t>(byte & 0x0fU)]);
    }
    return hex;
}

[[nodiscard]] std::vector<char> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    WAL_CHECK(input);
    return std::vector<char>(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

void write_prefix(
    const std::filesystem::path& path,
    const std::vector<char>& bytes,
    std::size_t size) {
    WAL_CHECK(size <= bytes.size());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    WAL_CHECK(output);
    output.write(bytes.data(), static_cast<std::streamsize>(size));
    output.flush();
    WAL_CHECK(output);
}

void flip_file_byte(const std::filesystem::path& path, std::uint64_t offset) {
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    WAL_CHECK(stream);
    stream.seekg(static_cast<std::streamoff>(offset));
    char value = 0;
    stream.read(&value, 1);
    WAL_CHECK(stream);
    value = static_cast<char>(static_cast<unsigned char>(value) ^ 0x01U);
    stream.seekp(static_cast<std::streamoff>(offset));
    stream.write(&value, 1);
    stream.flush();
    WAL_CHECK(stream);
}

[[nodiscard]] std::uint32_t fixture_crc32c(std::span<const char> bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const auto value : bytes) {
        crc ^= static_cast<std::uint8_t>(static_cast<unsigned char>(value));
        for (std::uint32_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0x82f63b78U : 0U);
        }
    }
    return ~crc;
}

void refresh_trailer_crc(std::vector<char>& bytes, std::size_t trailer_offset) {
    constexpr std::size_t covered_bytes = 20U;
    const auto crc = fixture_crc32c(
        std::span<const char>{bytes}.subspan(trailer_offset, covered_bytes));
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        bytes[trailer_offset + covered_bytes + static_cast<std::size_t>(shift / 8U)] =
            static_cast<char>((crc >> shift) & 0xffU);
    }
}

void expect_final_tail_then_repair(
    const std::filesystem::path& path,
    LogCursor base,
    std::uint64_t original_size) {
    auto rejected = SealedFrameLog::open_or_create(path, base, test_options(false));
    WAL_CHECK(!rejected);
    WAL_CHECK(rejected.error().code == LogErrorCode::TornTail);
    WAL_CHECK(std::filesystem::file_size(path) == original_size);

    auto repaired = SealedFrameLog::open_or_create(path, base, test_options());
    WAL_CHECK(repaired);
    WAL_CHECK(repaired.value().recovery_report().repaired_torn_tail);
    WAL_CHECK(repaired.value().recovery_report().entry_count == 0U);
    WAL_CHECK(repaired.value().recovery_report().valid_bytes == 64U);
    WAL_CHECK(repaired.value().recovery_report().truncated_bytes == original_size - 64U);
    WAL_CHECK(std::filesystem::file_size(path) == 64U);
}

void exact_v1_bytes_match_the_frozen_golden() {
    TempWal temporary;
    const LogCursor base{40U, 8U, 100U};
    auto open = SealedFrameLog::open_or_create(temporary.path(), base, test_options());
    WAL_CHECK(open);
    auto log = std::move(open).value();
    WAL_CHECK(log.append(first_frame()));

    std::ifstream input(temporary.path(), std::ios::binary);
    const std::vector<char> bytes(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());

    // Complete v1 file: 64-byte header, 80-byte entry prefix, three records,
    // and 24-byte commit trailer. This freezes endian order and both CRC scopes.
    constexpr std::string_view expected_hex =
        "4d53434d444c4f47010040000000000028000000000000000800000000000000"
        "64000000000000000000000000000000000000000000000000000000d3b3719e"
        "4d534652414d4531010050000000000079000000000000002900000000000000"
        "08000000000000000a0000000000000064000000000000006600000000000000"
        "03000000110000009c234cb900000000020000001011010000002002000000f0"
        "014d53434f4d4d49547900000000000000d7351f714ceb1167";
    WAL_CHECK(bytes.size() == 185U);
    WAL_CHECK(bytes_to_hex(bytes) == expected_hex);
}

void every_partial_or_invalid_final_trailer_is_repaired() {
    TempWal source;
    const LogCursor base{40U, 8U, 100U};
    {
        auto open = SealedFrameLog::open_or_create(source.path(), base, test_options());
        WAL_CHECK(open);
        auto log = std::move(open).value();
        WAL_CHECK(log.append(first_frame()));
    }

    const auto complete = read_file(source.path());
    constexpr std::size_t trailer_size = 24U;
    WAL_CHECK(complete.size() > 64U + trailer_size);
    const auto trailer_offset = complete.size() - trailer_size;

    for (std::size_t written = 0U; written < trailer_size; ++written) {
        TempWal partial;
        const auto partial_size = trailer_offset + written;
        write_prefix(partial.path(), complete, partial_size);
        expect_final_tail_then_repair(
            partial.path(), base, static_cast<std::uint64_t>(partial_size));
    }

    for (std::size_t byte = 0U; byte < trailer_size; ++byte) {
        TempWal invalid;
        auto mutated = complete;
        const auto index = trailer_offset + byte;
        mutated[index] = static_cast<char>(
            static_cast<unsigned char>(mutated[index]) ^ 0x01U);
        write_prefix(invalid.path(), mutated, mutated.size());
        expect_final_tail_then_repair(
            invalid.path(), base, static_cast<std::uint64_t>(mutated.size()));
    }

    // Structure is independently checked even when the checksum faithfully
    // covers the incorrectly written magic or total-length field.
    for (const auto structure_byte : {0U, 8U}) {
        TempWal invalid_structure;
        auto mutated = complete;
        const auto index = trailer_offset + structure_byte;
        mutated[index] = static_cast<char>(
            static_cast<unsigned char>(mutated[index]) ^ 0x01U);
        refresh_trailer_crc(mutated, trailer_offset);
        write_prefix(invalid_structure.path(), mutated, mutated.size());
        expect_final_tail_then_repair(
            invalid_structure.path(),
            base,
            static_cast<std::uint64_t>(mutated.size()));
    }
}

void invalid_prefix_on_an_uncommitted_final_body_is_repaired() {
    TempWal source;
    const LogCursor base{40U, 8U, 100U};
    {
        auto open = SealedFrameLog::open_or_create(source.path(), base, test_options());
        WAL_CHECK(open);
        auto log = std::move(open).value();
        WAL_CHECK(log.append(first_frame()));
    }

    const auto complete = read_file(source.path());
    constexpr std::size_t trailer_size = 24U;
    constexpr std::uint64_t entry_offset = 64U;
    constexpr std::uint64_t frame_field_offset = entry_offset + 24U;
    WAL_CHECK(complete.size() > trailer_size);

    TempWal uncommitted;
    const auto body_size = complete.size() - trailer_size;
    write_prefix(uncommitted.path(), complete, body_size);
    flip_file_byte(uncommitted.path(), frame_field_offset);
    expect_final_tail_then_repair(
        uncommitted.path(), base, static_cast<std::uint64_t>(body_size));
}

void invalid_prefix_on_a_committed_final_entry_is_fatal() {
    TempWal temporary;
    const LogCursor base{40U, 8U, 100U};
    {
        auto open = SealedFrameLog::open_or_create(temporary.path(), base, test_options());
        WAL_CHECK(open);
        auto log = std::move(open).value();
        WAL_CHECK(log.append(first_frame()));
    }

    const auto original_size = std::filesystem::file_size(temporary.path());
    constexpr std::uint64_t frame_field_offset = 64U + 24U;
    flip_file_byte(temporary.path(), frame_field_offset);
    auto reopen = SealedFrameLog::open_or_create(temporary.path(), base, test_options());
    WAL_CHECK(!reopen);
    WAL_CHECK(reopen.error().code == LogErrorCode::CorruptEntry);
    WAL_CHECK(std::filesystem::file_size(temporary.path()) == original_size);
}

void invalid_interior_prefix_is_fatal() {
    TempWal temporary;
    const LogCursor base{40U, 8U, 100U};
    {
        auto open = SealedFrameLog::open_or_create(temporary.path(), base, test_options());
        WAL_CHECK(open);
        auto log = std::move(open).value();
        WAL_CHECK(log.append(first_frame()));
        WAL_CHECK(log.append(empty_second_frame()));
    }

    const auto original_size = std::filesystem::file_size(temporary.path());
    constexpr std::uint64_t first_frame_field_offset = 64U + 24U;
    flip_file_byte(temporary.path(), first_frame_field_offset);
    auto reopen = SealedFrameLog::open_or_create(temporary.path(), base, test_options());
    WAL_CHECK(!reopen);
    WAL_CHECK(reopen.error().code == LogErrorCode::CorruptEntry);
    WAL_CHECK(std::filesystem::file_size(temporary.path()) == original_size);
}

void committed_suffix_survives_a_later_torn_body() {
    TempWal source;
    const LogCursor base{40U, 8U, 100U};
    std::uint64_t first_entry_bytes = 0U;
    std::uint64_t third_entry_bytes = 0U;
    {
        auto open = SealedFrameLog::open_or_create(source.path(), base, test_options());
        WAL_CHECK(open);
        auto log = std::move(open).value();
        auto first = log.append(first_frame());
        WAL_CHECK(first);
        first_entry_bytes = first.value().bytes_written;
        WAL_CHECK(log.append(empty_second_frame()));
        auto third = log.append(empty_third_frame());
        WAL_CHECK(third);
        third_entry_bytes = third.value().bytes_written;
    }

    constexpr std::uint64_t header_size = 64U;
    constexpr std::uint64_t trailer_size = 24U;
    const auto complete = read_file(source.path());
    WAL_CHECK(third_entry_bytes > trailer_size);
    WAL_CHECK(third_entry_bytes <= complete.size());
    const auto third_start =
        static_cast<std::uint64_t>(complete.size()) - third_entry_bytes;
    const auto third_body_bytes = third_entry_bytes - trailer_size;

    for (const auto invalidate_first_trailer : {false, true}) {
        for (const auto third_bytes : {third_body_bytes / 2U, third_body_bytes}) {
            TempWal damaged;
            auto bytes = complete;
            constexpr std::size_t first_frame_field = 64U + 24U;
            bytes[first_frame_field] = static_cast<char>(
                static_cast<unsigned char>(bytes[first_frame_field]) ^ 0x01U);
            if (invalidate_first_trailer) {
                WAL_CHECK(first_entry_bytes >= 4U);
                const auto first_trailer_crc =
                    header_size + first_entry_bytes - 4U;
                bytes[static_cast<std::size_t>(first_trailer_crc)] = static_cast<char>(
                    static_cast<unsigned char>(
                        bytes[static_cast<std::size_t>(first_trailer_crc)]) ^
                    0x01U);
            }

            const auto damaged_size = third_start + third_bytes;
            write_prefix(
                damaged.path(), bytes, static_cast<std::size_t>(damaged_size));
            auto reopen =
                SealedFrameLog::open_or_create(damaged.path(), base, test_options());
            WAL_CHECK(!reopen);
            WAL_CHECK(reopen.error().code == LogErrorCode::CorruptEntry);
            WAL_CHECK(std::filesystem::file_size(damaged.path()) == damaged_size);
        }
    }
}

void round_trip_and_checkpoint_replay() {
    TempWal temporary;
    const LogCursor base{40U, 8U, 100U};
    auto open = SealedFrameLog::open_or_create(temporary.path(), base, test_options());
    WAL_CHECK(open);
    auto log = std::move(open).value();
    WAL_CHECK(log.base_cursor() == base);
    WAL_CHECK(log.recovery_report().valid_bytes == 64U);

    const auto first = first_frame();
    const auto second = empty_second_frame();
    auto first_append = log.append(first);
    WAL_CHECK(first_append);
    WAL_CHECK(first_append.value().offset == 64U);
    WAL_CHECK(first_append.value().cursor == LogCursor({41U, 10U, 102U}));
    WAL_CHECK(!first_append.value().durable);
    WAL_CHECK(log.append(second));
    WAL_CHECK(log.tail_cursor() == LogCursor({42U, 10U, 102U}));

    std::vector<SealedFrameBytes> replayed;
    auto replay = log.replay_after(base, [&replayed](const SealedFrameBytes& frame) {
        replayed.push_back(frame);
        return LogStatus::success(std::monostate{});
    });
    WAL_CHECK(replay);
    WAL_CHECK(replayed.size() == 2U);
    WAL_CHECK(replayed[0].records == first.records);
    WAL_CHECK(replayed[1].records == second.records);

    replayed.clear();
    replay = log.replay_after(LogCursor{41U, 10U, 102U}, [&replayed](const auto& frame) {
        replayed.push_back(frame);
        return LogStatus::success(std::monostate{});
    });
    WAL_CHECK(replay);
    WAL_CHECK(replayed.size() == 1U);
    WAL_CHECK(replayed.front().frame == 42U);

    auto missing = log.replay_after(LogCursor{41U, 9U, 102U}, [](const auto&) {
        return LogStatus::success(std::monostate{});
    });
    WAL_CHECK(!missing);
    WAL_CHECK(missing.error().code == LogErrorCode::CursorNotFound);

    auto reopen = SealedFrameLog::open_or_create(temporary.path(), base, test_options());
    WAL_CHECK(reopen);
    WAL_CHECK(reopen.value().recovery_report().entry_count == 2U);
    WAL_CHECK(reopen.value().tail_cursor() == LogCursor({42U, 10U, 102U}));
}

void every_incomplete_entry_suffix_is_repaired() {
    TempWal source;
    const LogCursor base{0U, 0U, 1U};
    {
        auto open = SealedFrameLog::open_or_create(source.path(), base, test_options());
        WAL_CHECK(open);
        auto log = std::move(open).value();
        const SealedFrameBytes frame{
            1U, 0U, 1U, 1U, 2U, {record({1U, 2U, 3U, 4U}), record({5U, 6U})}};
        WAL_CHECK(log.append(frame));
    }

    std::ifstream input(source.path(), std::ios::binary);
    std::vector<char> bytes(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    WAL_CHECK(bytes.size() > 65U);

    for (std::size_t cut = 65U; cut < bytes.size(); ++cut) {
        TempWal truncated;
        {
            std::ofstream output(truncated.path(), std::ios::binary | std::ios::trunc);
            output.write(bytes.data(), static_cast<std::streamsize>(cut));
        }
        auto repaired = SealedFrameLog::open_or_create(truncated.path(), base, test_options());
        WAL_CHECK(repaired);
        WAL_CHECK(repaired.value().recovery_report().repaired_torn_tail);
        WAL_CHECK(repaired.value().recovery_report().valid_bytes == 64U);
        WAL_CHECK(std::filesystem::file_size(truncated.path()) == 64U);
    }
}

void torn_tail_can_be_reported_without_repair() {
    TempWal temporary;
    const LogCursor base{0U, 0U, 1U};
    {
        auto open = SealedFrameLog::open_or_create(temporary.path(), base, test_options());
        WAL_CHECK(open);
    }
    {
        std::ofstream output(temporary.path(), std::ios::binary | std::ios::app);
        const std::vector<char> partial{'M', 'S', 'F'};
        output.write(partial.data(), static_cast<std::streamsize>(partial.size()));
    }

    auto open = SealedFrameLog::open_or_create(temporary.path(), base, test_options(false));
    WAL_CHECK(!open);
    WAL_CHECK(open.error().code == LogErrorCode::TornTail);
    WAL_CHECK(std::filesystem::file_size(temporary.path()) == 67U);
}

void valid_final_trailer_with_corrupted_body_is_fatal() {
    TempWal temporary;
    const LogCursor base{40U, 8U, 100U};
    {
        auto open = SealedFrameLog::open_or_create(temporary.path(), base, test_options());
        WAL_CHECK(open);
        auto log = std::move(open).value();
        auto appended = log.append(first_frame());
        WAL_CHECK(appended);
        WAL_CHECK(!appended.value().durable);
    }

    const auto original_size = std::filesystem::file_size(temporary.path());
    constexpr std::streamoff first_record_byte = 64 + 80 + 4;
    flip_file_byte(temporary.path(), static_cast<std::uint64_t>(first_record_byte));

    auto reopen = SealedFrameLog::open_or_create(temporary.path(), base, test_options());
    WAL_CHECK(!reopen);
    WAL_CHECK(reopen.error().code == LogErrorCode::CorruptEntry);
    WAL_CHECK(std::filesystem::file_size(temporary.path()) == original_size);
}

void invalid_interior_trailer_is_fatal() {
    TempWal temporary;
    const LogCursor base{40U, 8U, 100U};
    std::uint64_t first_entry_bytes = 0U;
    {
        auto open = SealedFrameLog::open_or_create(temporary.path(), base, test_options());
        WAL_CHECK(open);
        auto log = std::move(open).value();
        auto first = log.append(first_frame());
        WAL_CHECK(first);
        first_entry_bytes = first.value().bytes_written;
        WAL_CHECK(log.append(empty_second_frame()));
    }

    const auto original_size = std::filesystem::file_size(temporary.path());
    constexpr std::uint64_t header_size = 64U;
    constexpr std::uint64_t trailer_size = 24U;
    WAL_CHECK(first_entry_bytes >= trailer_size);
    const auto first_trailer_crc = header_size + first_entry_bytes - 4U;
    flip_file_byte(temporary.path(), first_trailer_crc);

    auto reopen = SealedFrameLog::open_or_create(temporary.path(), base, test_options());
    WAL_CHECK(!reopen);
    WAL_CHECK(reopen.error().code == LogErrorCode::CorruptEntry);
    WAL_CHECK(std::filesystem::file_size(temporary.path()) == original_size);
}

void append_rejects_invalid_chains_and_limits() {
    TempWal temporary;
    const LogCursor base{0U, 0U, 1U};
    auto open = SealedFrameLog::open_or_create(temporary.path(), base, test_options());
    WAL_CHECK(open);
    auto log = std::move(open).value();

    auto wrong_frame = first_frame();
    auto result = log.append(wrong_frame);
    WAL_CHECK(!result);
    WAL_CHECK(result.error().code == LogErrorCode::ChainMismatch);

    const SealedFrameBytes missing_seal{1U, 0U, 1U, 1U, 2U, {record({1U})}};
    result = log.append(missing_seal);
    WAL_CHECK(!result);
    WAL_CHECK(result.error().code == LogErrorCode::ChainMismatch);

    auto options = test_options();
    options.limits.max_record_bytes = 1U;
    TempWal limited_file;
    auto limited_open = SealedFrameLog::open_or_create(limited_file.path(), base, options);
    WAL_CHECK(limited_open);
    auto limited = std::move(limited_open).value();
    const SealedFrameBytes oversized{1U, 0U, 0U, 1U, 1U, {record({1U, 2U})}};
    auto oversized_result = limited.append(oversized);
    WAL_CHECK(!oversized_result);
    WAL_CHECK(oversized_result.error().code == LogErrorCode::LimitExceeded);
}

void existing_log_rejects_a_different_base_cursor() {
    TempWal temporary;
    {
        auto open = SealedFrameLog::open_or_create(
            temporary.path(), LogCursor{7U, 9U, 11U}, test_options());
        WAL_CHECK(open);
    }
    auto reopen = SealedFrameLog::open_or_create(
        temporary.path(), LogCursor{7U, 10U, 11U}, test_options());
    WAL_CHECK(!reopen);
    WAL_CHECK(reopen.error().code == LogErrorCode::BaseCursorMismatch);
}

void durable_sync_policies_append_and_reopen() {
    for (const auto policy : {SyncPolicy::Data, SyncPolicy::Full}) {
        TempWal temporary;
        const LogCursor base{40U, 8U, 100U};
        OpenOptions options;
        options.sync_policy = policy;

        auto open = SealedFrameLog::open_or_create(temporary.path(), base, options);
        WAL_CHECK(open);
        auto log = std::move(open).value();
        auto appended = log.append(first_frame());
        WAL_CHECK(appended);
        WAL_CHECK(appended.value().durable);
        WAL_CHECK(log.flush());

        auto reopen = SealedFrameLog::open_or_create(temporary.path(), base, options);
        WAL_CHECK(reopen);
        const LogCursor expected{41U, 10U, 102U};
        WAL_CHECK(reopen.value().tail_cursor() == expected);
    }
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"exact v1 bytes match frozen golden", &exact_v1_bytes_match_the_frozen_golden},
        {"partial or invalid final trailers repair", &every_partial_or_invalid_final_trailer_is_repaired},
        {"invalid uncommitted final prefix repairs", &invalid_prefix_on_an_uncommitted_final_body_is_repaired},
        {"invalid committed final prefix is fatal", &invalid_prefix_on_a_committed_final_entry_is_fatal},
        {"invalid interior prefix is fatal", &invalid_interior_prefix_is_fatal},
        {"committed suffix survives later torn body", &committed_suffix_survives_a_later_torn_body},
        {"round trip and checkpoint replay", &round_trip_and_checkpoint_replay},
        {"every incomplete entry suffix is repaired", &every_incomplete_entry_suffix_is_repaired},
        {"torn tail can be reported without repair", &torn_tail_can_be_reported_without_repair},
        {"valid trailer with corrupted body is fatal", &valid_final_trailer_with_corrupted_body_is_fatal},
        {"invalid interior trailer is fatal", &invalid_interior_trailer_is_fatal},
        {"append rejects invalid chains and limits", &append_rejects_invalid_chains_and_limits},
        {"existing log rejects a different base cursor", &existing_log_rejects_a_different_base_cursor},
        {"durable sync policies append and reopen", &durable_sync_policies_append_and_reopen},
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
    std::cout << (tests.size() - failures) << '/' << tests.size() << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
