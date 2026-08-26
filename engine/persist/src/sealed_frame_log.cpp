#include <mediaserver/persist/sealed_frame_log.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace mediaserver::persist {
namespace {

constexpr std::uint16_t kFormatVersion = 1U;
constexpr std::size_t kHeaderSize = 64U;
constexpr std::size_t kPrefixSize = 80U;
constexpr std::size_t kTrailerSize = 24U;
constexpr std::size_t kPrefixCrcOffset = 72U;
constexpr std::size_t kHeaderCrcOffset = 60U;

constexpr std::array<std::byte, 8U> kHeaderMagic{
    std::byte{0x4d}, std::byte{0x53}, std::byte{0x43}, std::byte{0x4d},
    std::byte{0x44}, std::byte{0x4c}, std::byte{0x4f}, std::byte{0x47}}; // MSCMDLOG
constexpr std::array<std::byte, 8U> kEntryMagic{
    std::byte{0x4d}, std::byte{0x53}, std::byte{0x46}, std::byte{0x52},
    std::byte{0x41}, std::byte{0x4d}, std::byte{0x45}, std::byte{0x31}}; // MSFRAME1
constexpr std::array<std::byte, 8U> kCommitMagic{
    std::byte{0x4d}, std::byte{0x53}, std::byte{0x43}, std::byte{0x4f},
    std::byte{0x4d}, std::byte{0x4d}, std::byte{0x49}, std::byte{0x54}}; // MSCOMMIT

constexpr std::array<std::uint32_t, 256U> make_crc32c_table() noexcept {
    std::array<std::uint32_t, 256U> table{};
    for (std::size_t index = 0U; index < table.size(); ++index) {
        auto value = static_cast<std::uint32_t>(index);
        for (std::uint32_t bit = 0U; bit < 8U; ++bit) {
            value = (value >> 1U) ^ ((value & 1U) != 0U ? 0x82f63b78U : 0U);
        }
        table[index] = value;
    }
    return table;
}

constexpr auto kCrc32cTable = make_crc32c_table();

[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
        const auto lookup = static_cast<std::uint8_t>(
            (crc ^ std::to_integer<std::uint8_t>(byte)) & 0xffU);
        crc = kCrc32cTable[lookup] ^ (crc >> 8U);
    }
    return ~crc;
}

void put_u16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) noexcept {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void put_u32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) noexcept {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        bytes[offset + static_cast<std::size_t>(shift / 8U)] =
            static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

void put_u64(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) noexcept {
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        bytes[offset + static_cast<std::size_t>(shift / 8U)] =
            static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

[[nodiscard]] std::uint16_t get_u16(
    std::span<const std::byte> bytes,
    std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U]))
            << 8U));
}

[[nodiscard]] std::uint32_t get_u32(
    std::span<const std::byte> bytes,
    std::size_t offset) noexcept {
    std::uint32_t value = 0U;
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        const auto index = offset + static_cast<std::size_t>(shift / 8U);
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[index]))
                 << shift;
    }
    return value;
}

[[nodiscard]] std::uint64_t get_u64(
    std::span<const std::byte> bytes,
    std::size_t offset) noexcept {
    std::uint64_t value = 0U;
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        const auto index = offset + static_cast<std::size_t>(shift / 8U);
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[index]))
                 << shift;
    }
    return value;
}

template <std::size_t Size>
[[nodiscard]] bool has_magic(
    std::span<const std::byte> bytes,
    std::size_t offset,
    const std::array<std::byte, Size>& magic) noexcept {
    return bytes.size() >= offset + Size &&
           std::equal(magic.begin(), magic.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

[[nodiscard]] bool is_zero(std::span<const std::byte> bytes) noexcept {
    return std::all_of(bytes.begin(), bytes.end(), [](std::byte value) {
        return value == std::byte{0U};
    });
}

[[nodiscard]] LogError make_error(
    LogErrorCode code,
    std::uint64_t offset,
    std::string detail,
    std::error_code system_error = {}) {
    return LogError{code, offset, system_error, std::move(detail)};
}

[[nodiscard]] LogStatus success_status() {
    return LogStatus::success(std::monostate{});
}

[[nodiscard]] std::error_code current_io_error() noexcept {
    return std::error_code{errno, std::generic_category()};
}

[[nodiscard]] core::Result<std::uint64_t, LogError> checked_file_size(
    const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return core::Result<std::uint64_t, LogError>::failure(make_error(
            LogErrorCode::ReadFailed, 0U, "could not determine WAL size", error));
    }
    if (size > std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<std::uint64_t, LogError>::failure(make_error(
            LogErrorCode::LimitExceeded, 0U, "WAL is too large to address"));
    }
    return core::Result<std::uint64_t, LogError>::success(static_cast<std::uint64_t>(size));
}

[[nodiscard]] LogStatus sync_file(
    const std::filesystem::path& path,
    SyncPolicy policy) {
    if (policy == SyncPolicy::None) {
        return success_status();
    }

#ifdef _WIN32
    const auto handle = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = std::error_code{
            static_cast<int>(GetLastError()), std::system_category()};
        return LogStatus::failure(make_error(
            LogErrorCode::SyncFailed, 0U, "could not open WAL for durable sync", error));
    }

    const auto flushed = FlushFileBuffers(handle) != 0;
    const auto flush_error = flushed
                                 ? std::error_code{}
                                 : std::error_code{static_cast<int>(GetLastError()),
                                                   std::system_category()};
    const auto closed = CloseHandle(handle) != 0;
    if (!flushed || !closed) {
        const auto error = flushed
                               ? std::error_code{static_cast<int>(GetLastError()),
                                                 std::system_category()}
                               : flush_error;
        return LogStatus::failure(make_error(
            LogErrorCode::SyncFailed, 0U, "durable WAL sync failed", error));
    }
#else
    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), O_RDWR);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        return LogStatus::failure(make_error(
            LogErrorCode::SyncFailed,
            0U,
            "could not open WAL for durable sync",
            current_io_error()));
    }

    int sync_result = 0;
    do {
        sync_result = policy == SyncPolicy::Data ? ::fdatasync(descriptor)
                                                  : ::fsync(descriptor);
    } while (sync_result != 0 && errno == EINTR);
    const auto sync_error = sync_result == 0 ? std::error_code{} : current_io_error();
    const auto close_result = ::close(descriptor);
    if (sync_result != 0 || close_result != 0) {
        const auto error = sync_result != 0 ? sync_error : current_io_error();
        return LogStatus::failure(make_error(
            LogErrorCode::SyncFailed, 0U, "durable WAL sync failed", error));
    }
#endif

    return success_status();
}

[[nodiscard]] LogStatus sync_parent_directory(const std::filesystem::path& path) {
#ifdef _WIN32
    static_cast<void>(path);
    return success_status();
#else
    auto parent = path.parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    auto flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    int descriptor = -1;
    do {
        descriptor = ::open(parent.c_str(), flags);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        return LogStatus::failure(make_error(
            LogErrorCode::SyncFailed,
            0U,
            "could not open the WAL parent directory for durable sync",
            current_io_error()));
    }

    int sync_result = 0;
    do {
        sync_result = ::fsync(descriptor);
    } while (sync_result != 0 && errno == EINTR);
    const auto sync_error = sync_result == 0 ? std::error_code{} : current_io_error();
    const auto close_result = ::close(descriptor);
    if (sync_result != 0 || close_result != 0) {
        const auto error = sync_result != 0 ? sync_error : current_io_error();
        return LogStatus::failure(make_error(
            LogErrorCode::SyncFailed,
            0U,
            "durable WAL parent-directory sync failed",
            error));
    }
    return success_status();
#endif
}

[[nodiscard]] LogStatus validate_limits(const LogLimits& limits) {
    if (limits.max_record_bytes == 0U || limits.max_payload_bytes < 4U ||
        limits.max_records_per_frame == 0U) {
        return LogStatus::failure(make_error(
            LogErrorCode::LimitExceeded, 0U, "WAL limits must all be positive"));
    }
    const auto framing_bytes = static_cast<std::uint64_t>(kPrefixSize + kTrailerSize);
    const auto addressable_bytes = static_cast<std::uint64_t>(
        std::numeric_limits<std::size_t>::max());
    if (framing_bytes > addressable_bytes
        || static_cast<std::uint64_t>(limits.max_payload_bytes)
            > addressable_bytes - framing_bytes) {
        return LogStatus::failure(make_error(
            LogErrorCode::LimitExceeded,
            0U,
            "WAL payload limit exceeds platform addressability"));
    }
    return success_status();
}

[[nodiscard]] std::array<std::byte, kHeaderSize> make_header(LogCursor cursor) {
    std::array<std::byte, kHeaderSize> bytes{};
    std::copy(kHeaderMagic.begin(), kHeaderMagic.end(), bytes.begin());
    auto span = std::span<std::byte>{bytes};
    put_u16(span, 8U, kFormatVersion);
    put_u16(span, 10U, static_cast<std::uint16_t>(kHeaderSize));
    put_u32(span, 12U, 0U);
    put_u64(span, 16U, cursor.through_frame);
    put_u64(span, 24U, cursor.revision);
    put_u64(span, 32U, cursor.next_command);
    put_u32(span, kHeaderCrcOffset, crc32c(std::span<const std::byte>{bytes}.first(kHeaderCrcOffset)));
    return bytes;
}

[[nodiscard]] core::Result<LogCursor, LogError> read_header(
    const std::filesystem::path& path,
    std::uint64_t file_size) {
    if (file_size < kHeaderSize) {
        return core::Result<LogCursor, LogError>::failure(make_error(
            LogErrorCode::InvalidHeader, 0U, "WAL header is truncated"));
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return core::Result<LogCursor, LogError>::failure(make_error(
            LogErrorCode::OpenFailed, 0U, "could not open WAL for reading", current_io_error()));
    }

    std::array<std::byte, kHeaderSize> bytes{};
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return core::Result<LogCursor, LogError>::failure(make_error(
            LogErrorCode::ReadFailed, 0U, "could not read complete WAL header", current_io_error()));
    }

    const auto view = std::span<const std::byte>{bytes};
    if (!has_magic(view, 0U, kHeaderMagic) ||
        get_u16(view, 10U) != static_cast<std::uint16_t>(kHeaderSize) ||
        get_u32(view, 12U) != 0U || !is_zero(view.subspan(40U, 20U)) ||
        get_u32(view, kHeaderCrcOffset) != crc32c(view.first(kHeaderCrcOffset))) {
        return core::Result<LogCursor, LogError>::failure(make_error(
            LogErrorCode::InvalidHeader, 0U, "WAL header failed structural or CRC validation"));
    }
    if (get_u16(view, 8U) != kFormatVersion) {
        return core::Result<LogCursor, LogError>::failure(make_error(
            LogErrorCode::UnsupportedVersion, 0U, "unsupported WAL format version"));
    }

    LogCursor cursor{get_u64(view, 16U), get_u64(view, 24U), get_u64(view, 32U)};
    if (cursor.next_command == 0U) {
        return core::Result<LogCursor, LogError>::failure(make_error(
            LogErrorCode::InvalidHeader, 0U, "base cursor has a zero next command ID"));
    }
    return core::Result<LogCursor, LogError>::success(cursor);
}

[[nodiscard]] LogStatus initialize_file(
    const std::filesystem::path& path,
    LogCursor base_cursor,
    SyncPolicy policy) {
    const auto header = make_header(base_cursor);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return LogStatus::failure(make_error(
            LogErrorCode::OpenFailed, 0U, "could not create WAL", current_io_error()));
    }
    stream.write(
        reinterpret_cast<const char*>(header.data()),
        static_cast<std::streamsize>(header.size()));
    stream.flush();
    if (!stream) {
        return LogStatus::failure(make_error(
            LogErrorCode::WriteFailed, 0U, "could not write WAL header", current_io_error()));
    }
    stream.close();
    if (!stream) {
        return LogStatus::failure(make_error(
            LogErrorCode::WriteFailed, 0U, "could not close WAL header", current_io_error()));
    }
    return sync_file(path, policy);
}

[[nodiscard]] LogStatus validate_frame(
    const SealedFrameBytes& frame,
    LogCursor expected,
    const LogLimits& limits,
    std::uint64_t offset) {
    if (expected.next_command == 0U ||
        expected.through_frame == std::numeric_limits<std::uint64_t>::max() ||
        frame.frame != expected.through_frame + 1U ||
        frame.base_revision != expected.revision ||
        frame.first_command_id != expected.next_command || frame.first_command_id == 0U ||
        frame.next_command_id < frame.first_command_id) {
        return LogStatus::failure(make_error(
            LogErrorCode::ChainMismatch, offset, "sealed frame does not continue the WAL cursor"));
    }

    const auto command_count = frame.next_command_id - frame.first_command_id;
    if (command_count > static_cast<std::uint64_t>(limits.max_records_per_frame - 1U)) {
        return LogStatus::failure(make_error(
            LogErrorCode::LimitExceeded, offset, "sealed frame exceeds WAL record-count limit"));
    }
    if (command_count > std::numeric_limits<std::uint64_t>::max() - frame.base_revision ||
        frame.result_revision != frame.base_revision + command_count ||
        frame.records.size() != static_cast<std::size_t>(command_count + 1U)) {
        return LogStatus::failure(make_error(
            LogErrorCode::ChainMismatch,
            offset,
            "record count and revision range do not describe one command record per command plus a seal"));
    }

    std::uint64_t payload_size = 0U;
    for (const auto& record : frame.records) {
        if (record.size() > limits.max_record_bytes) {
            return LogStatus::failure(make_error(
                LogErrorCode::LimitExceeded, offset, "encoded record exceeds WAL record limit"));
        }
        payload_size += 4U + static_cast<std::uint64_t>(record.size());
        if (payload_size > limits.max_payload_bytes) {
            return LogStatus::failure(make_error(
                LogErrorCode::LimitExceeded, offset, "sealed frame exceeds WAL payload limit"));
        }
    }
    return success_status();
}

[[nodiscard]] std::vector<std::byte> encode_entry(const SealedFrameBytes& frame) {
    std::size_t payload_size = 0U;
    for (const auto& record : frame.records) {
        payload_size += 4U + record.size();
    }
    const auto total_size = kPrefixSize + payload_size + kTrailerSize;
    std::vector<std::byte> bytes(total_size);
    std::copy(kEntryMagic.begin(), kEntryMagic.end(), bytes.begin());
    auto span = std::span<std::byte>{bytes};
    put_u16(span, 8U, kFormatVersion);
    put_u16(span, 10U, static_cast<std::uint16_t>(kPrefixSize));
    put_u32(span, 12U, 0U);
    put_u64(span, 16U, static_cast<std::uint64_t>(total_size));
    put_u64(span, 24U, frame.frame);
    put_u64(span, 32U, frame.base_revision);
    put_u64(span, 40U, frame.result_revision);
    put_u64(span, 48U, frame.first_command_id);
    put_u64(span, 56U, frame.next_command_id);
    put_u32(span, 64U, static_cast<std::uint32_t>(frame.records.size()));
    put_u32(span, 68U, static_cast<std::uint32_t>(payload_size));
    put_u32(span, kPrefixCrcOffset, crc32c(std::span<const std::byte>{bytes}.first(kPrefixCrcOffset)));

    auto cursor = kPrefixSize;
    for (const auto& record : frame.records) {
        put_u32(span, cursor, static_cast<std::uint32_t>(record.size()));
        cursor += 4U;
        std::copy(record.begin(), record.end(), bytes.begin() + static_cast<std::ptrdiff_t>(cursor));
        cursor += record.size();
    }

    std::copy(kCommitMagic.begin(), kCommitMagic.end(), bytes.begin() + static_cast<std::ptrdiff_t>(cursor));
    put_u64(span, cursor + 8U, static_cast<std::uint64_t>(total_size));
    put_u32(span, cursor + 16U, crc32c(std::span<const std::byte>{bytes}.first(cursor + 16U)));
    put_u32(
        span,
        cursor + 20U,
        crc32c(std::span<const std::byte>{bytes}.subspan(cursor, 20U)));
    return bytes;
}

struct DecodedEntry final {
    SealedFrameBytes frame;
    std::uint64_t total_bytes{};
    std::uint32_t entry_crc32c{};
};

[[nodiscard]] core::Result<bool, LogError> has_authenticated_commit_suffix_at_or_after(
    std::ifstream& stream,
    std::uint64_t scan_offset,
    std::uint64_t file_size) {
    if (scan_offset > file_size || file_size - scan_offset < kTrailerSize) {
        return core::Result<bool, LogError>::success(false);
    }
    if (file_size > static_cast<std::uint64_t>(
                        std::numeric_limits<std::streamoff>::max())) {
        return core::Result<bool, LogError>::failure(make_error(
            LogErrorCode::LimitExceeded,
            scan_offset,
            "WAL suffix exceeds stream addressing range"));
    }

    stream.clear();
    stream.seekg(static_cast<std::streamoff>(scan_offset));

    constexpr std::size_t chunk_bytes = 64U * 1024U;
    constexpr std::size_t overlap_bytes = kTrailerSize - 1U;
    constexpr auto minimum_entry_bytes =
        static_cast<std::uint64_t>(kPrefixSize + kTrailerSize);
    std::array<std::byte, chunk_bytes + overlap_bytes> buffer{};
    std::size_t overlap = 0U;
    auto read_offset = scan_offset;

    while (read_offset < file_size) {
        const auto remaining = file_size - read_offset;
        const auto requested = static_cast<std::size_t>(std::min<std::uint64_t>(
            remaining, static_cast<std::uint64_t>(chunk_bytes)));
        stream.read(
            reinterpret_cast<char*>(buffer.data() + overlap),
            static_cast<std::streamsize>(requested));
        if (stream.gcount() != static_cast<std::streamsize>(requested)) {
            return core::Result<bool, LogError>::failure(make_error(
                LogErrorCode::ReadFailed,
                read_offset,
                "could not scan the WAL suffix for commit evidence",
                current_io_error()));
        }

        const auto available = overlap + requested;
        const auto buffer_offset = read_offset - static_cast<std::uint64_t>(overlap);
        const auto view = std::span<const std::byte>{buffer}.first(available);
        for (std::size_t local = 0U; local + kTrailerSize <= available; ++local) {
            const auto trailer = view.subspan(local, kTrailerSize);
            if (!has_magic(trailer, 0U, kCommitMagic) ||
                get_u32(trailer, 20U) != crc32c(trailer.first(20U))) {
                continue;
            }

            const auto total_bytes = get_u64(trailer, 8U);
            const auto trailer_offset =
                buffer_offset + static_cast<std::uint64_t>(local);
            const auto candidate_end = trailer_offset + kTrailerSize;
            if (total_bytes < minimum_entry_bytes || total_bytes > candidate_end) {
                continue;
            }
            const auto candidate_start = candidate_end - total_bytes;
            if (candidate_start >= scan_offset) {
                return core::Result<bool, LogError>::success(true);
            }
        }

        read_offset += static_cast<std::uint64_t>(requested);
        const auto next_overlap = std::min(overlap_bytes, available);
        for (std::size_t index = 0U; index < next_overlap; ++index) {
            buffer[index] = buffer[available - next_overlap + index];
        }
        overlap = next_overlap;
    }
    return core::Result<bool, LogError>::success(false);
}

[[nodiscard]] core::Result<DecodedEntry, LogError> decode_entry(
    const std::filesystem::path& path,
    std::uint64_t offset,
    std::uint64_t file_size,
    const LogLimits& limits) {
    if (offset > file_size || file_size - offset < kPrefixSize) {
        return core::Result<DecodedEntry, LogError>::failure(make_error(
            LogErrorCode::TornTail, offset, "WAL ends inside an entry prefix"));
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return core::Result<DecodedEntry, LogError>::failure(make_error(
            LogErrorCode::OpenFailed, offset, "could not open WAL entry", current_io_error()));
    }
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return core::Result<DecodedEntry, LogError>::failure(make_error(
            LogErrorCode::LimitExceeded, offset, "WAL offset exceeds stream addressing range"));
    }
    stream.seekg(static_cast<std::streamoff>(offset));

    std::array<std::byte, kPrefixSize> prefix{};
    stream.read(
        reinterpret_cast<char*>(prefix.data()),
        static_cast<std::streamsize>(prefix.size()));
    if (stream.gcount() != static_cast<std::streamsize>(prefix.size())) {
        return core::Result<DecodedEntry, LogError>::failure(make_error(
            LogErrorCode::ReadFailed, offset, "could not read WAL entry prefix", current_io_error()));
    }

    const auto prefix_view = std::span<const std::byte>{prefix};
    if (!has_magic(prefix_view, 0U, kEntryMagic) ||
        get_u16(prefix_view, 10U) != static_cast<std::uint16_t>(kPrefixSize) ||
        get_u32(prefix_view, 12U) != 0U || get_u32(prefix_view, 76U) != 0U ||
        get_u32(prefix_view, kPrefixCrcOffset) != crc32c(prefix_view.first(kPrefixCrcOffset))) {
        auto commit_evidence = has_authenticated_commit_suffix_at_or_after(
            stream, offset, file_size);
        if (!commit_evidence) {
            return core::Result<DecodedEntry, LogError>::failure(
                std::move(commit_evidence).error());
        }
        return core::Result<DecodedEntry, LogError>::failure(make_error(
            commit_evidence.value() ? LogErrorCode::CorruptEntry
                                    : LogErrorCode::TornTail,
            offset,
            commit_evidence.value()
                ? "WAL entry prefix is invalid before authenticated commit evidence"
                : "final WAL append has an invalid prefix and no authenticated commit marker"));
    }
    if (get_u16(prefix_view, 8U) != kFormatVersion) {
        return core::Result<DecodedEntry, LogError>::failure(make_error(
            LogErrorCode::UnsupportedVersion, offset, "unsupported WAL entry version"));
    }

    const auto total_bytes = get_u64(prefix_view, 16U);
    const auto record_count = get_u32(prefix_view, 64U);
    const auto payload_bytes = get_u32(prefix_view, 68U);
    const auto expected_total = static_cast<std::uint64_t>(kPrefixSize) + payload_bytes + kTrailerSize;
    if (payload_bytes > limits.max_payload_bytes || record_count > limits.max_records_per_frame) {
        return core::Result<DecodedEntry, LogError>::failure(make_error(
            LogErrorCode::LimitExceeded, offset, "WAL entry exceeds configured limits"));
    }
    if (total_bytes != expected_total) {
        return core::Result<DecodedEntry, LogError>::failure(make_error(
            LogErrorCode::CorruptEntry, offset, "WAL entry length fields disagree"));
    }
    if (total_bytes > file_size - offset) {
        return core::Result<DecodedEntry, LogError>::failure(make_error(
            LogErrorCode::TornTail, offset, "WAL ends before the entry commit trailer"));
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(total_bytes));
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset));
    stream.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return core::Result<DecodedEntry, LogError>::failure(make_error(
            LogErrorCode::ReadFailed, offset, "could not read complete WAL entry", current_io_error()));
    }

    const auto view = std::span<const std::byte>{bytes};
    const auto trailer_offset = kPrefixSize + static_cast<std::size_t>(payload_bytes);
    const auto trailer = view.subspan(trailer_offset, kTrailerSize);
    const auto valid_commit_marker = has_magic(trailer, 0U, kCommitMagic)
        && get_u64(trailer, 8U) == total_bytes
        && get_u32(trailer, 20U) == crc32c(trailer.first(20U));
    if (!valid_commit_marker) {
        const auto final_entry = total_bytes == file_size - offset;
        return core::Result<DecodedEntry, LogError>::failure(make_error(
            final_entry ? LogErrorCode::TornTail : LogErrorCode::CorruptEntry,
            offset,
            final_entry ? "final WAL entry has no valid commit marker"
                        : "interior WAL entry has an invalid commit marker"));
    }
    if (get_u32(trailer, 16U) != crc32c(view.first(trailer_offset + 16U))) {
        return core::Result<DecodedEntry, LogError>::failure(make_error(
            LogErrorCode::CorruptEntry,
            offset,
            "committed WAL entry failed body CRC validation"));
    }

    SealedFrameBytes frame{
        get_u64(view, 24U),
        get_u64(view, 32U),
        get_u64(view, 40U),
        get_u64(view, 48U),
        get_u64(view, 56U),
        {}};
    frame.records.reserve(record_count);

    auto payload_cursor = kPrefixSize;
    for (std::uint32_t index = 0U; index < record_count; ++index) {
        if (payload_cursor > trailer_offset || trailer_offset - payload_cursor < 4U) {
            return core::Result<DecodedEntry, LogError>::failure(make_error(
                LogErrorCode::CorruptEntry, offset, "record length crosses the payload boundary"));
        }
        const auto record_size = get_u32(view, payload_cursor);
        payload_cursor += 4U;
        if (record_size > limits.max_record_bytes) {
            return core::Result<DecodedEntry, LogError>::failure(make_error(
                LogErrorCode::LimitExceeded, offset, "encoded record exceeds WAL record limit"));
        }
        if (payload_cursor > trailer_offset ||
            static_cast<std::size_t>(record_size) > trailer_offset - payload_cursor) {
            return core::Result<DecodedEntry, LogError>::failure(make_error(
                LogErrorCode::CorruptEntry, offset, "record bytes cross the payload boundary"));
        }
        const auto record_end = payload_cursor + static_cast<std::size_t>(record_size);
        frame.records.emplace_back(
            bytes.begin() + static_cast<std::ptrdiff_t>(payload_cursor),
            bytes.begin() + static_cast<std::ptrdiff_t>(record_end));
        payload_cursor = record_end;
    }
    if (payload_cursor != trailer_offset) {
        return core::Result<DecodedEntry, LogError>::failure(make_error(
            LogErrorCode::CorruptEntry, offset, "payload contains unframed trailing bytes"));
    }

    return core::Result<DecodedEntry, LogError>::success(DecodedEntry{
        std::move(frame),
        total_bytes,
        get_u32(trailer, 16U),
    });
}

struct ScanResult final {
    RecoveryReport report;
    std::vector<std::uint64_t> offsets;
};

[[nodiscard]] core::Result<ScanResult, LogError> scan_log(
    const std::filesystem::path& path,
    LogCursor expected_base,
    const OpenOptions& options) {
    auto size_result = checked_file_size(path);
    if (!size_result) {
        return core::Result<ScanResult, LogError>::failure(std::move(size_result).error());
    }
    auto file_size = size_result.value();

    auto header_result = read_header(path, file_size);
    if (!header_result) {
        return core::Result<ScanResult, LogError>::failure(std::move(header_result).error());
    }
    if (header_result.value() != expected_base) {
        return core::Result<ScanResult, LogError>::failure(make_error(
            LogErrorCode::BaseCursorMismatch,
            0U,
            "existing WAL base cursor differs from the requested base cursor"));
    }

    ScanResult scan{
        RecoveryReport{expected_base, expected_base, 0U, kHeaderSize, 0U, false}, {}};
    auto offset = static_cast<std::uint64_t>(kHeaderSize);
    auto tail = expected_base;

    while (offset < file_size) {
        if (file_size - offset < kPrefixSize) {
            if (!options.repair_torn_tail) {
                return core::Result<ScanResult, LogError>::failure(make_error(
                    LogErrorCode::TornTail, offset, "WAL has an incomplete entry prefix"));
            }
            break;
        }

        auto entry_result = decode_entry(path, offset, file_size, options.limits);
        if (!entry_result) {
            if (entry_result.error().code == LogErrorCode::TornTail && options.repair_torn_tail) {
                break;
            }
            return core::Result<ScanResult, LogError>::failure(std::move(entry_result).error());
        }

        const auto& entry = entry_result.value();
        auto validation = validate_frame(entry.frame, tail, options.limits, offset);
        if (!validation) {
            return core::Result<ScanResult, LogError>::failure(std::move(validation).error());
        }

        scan.offsets.push_back(offset);
        tail = LogCursor{
            entry.frame.frame, entry.frame.result_revision, entry.frame.next_command_id};
        offset += entry.total_bytes;
        ++scan.report.entry_count;
    }

    if (offset != file_size) {
        std::error_code truncate_error;
        std::filesystem::resize_file(path, offset, truncate_error);
        if (truncate_error) {
            return core::Result<ScanResult, LogError>::failure(make_error(
                LogErrorCode::TruncateFailed,
                offset,
                "could not remove torn WAL tail",
                truncate_error));
        }
        // Recovery changes the durable file boundary. Persist it even when
        // subsequent appends use a relaxed final-sync policy.
        auto sync_result = sync_file(path, SyncPolicy::Data);
        if (!sync_result) {
            return core::Result<ScanResult, LogError>::failure(std::move(sync_result).error());
        }
        scan.report.repaired_torn_tail = true;
        scan.report.truncated_bytes = file_size - offset;
        file_size = offset;
    }

    scan.report.tail_cursor = tail;
    scan.report.valid_bytes = file_size;
    return core::Result<ScanResult, LogError>::success(std::move(scan));
}

} // namespace

SealedFrameLog::SealedFrameLog(
    std::filesystem::path path,
    LogCursor base_cursor,
    OpenOptions options,
    RecoveryReport recovery,
    std::vector<std::uint64_t> entry_offsets)
    : path_(std::move(path)),
      base_cursor_(base_cursor),
      tail_cursor_(recovery.tail_cursor),
      options_(options),
      recovery_(recovery),
      entry_offsets_(std::move(entry_offsets)) {}

core::Result<SealedFrameLog, LogError> SealedFrameLog::open_or_create(
    std::filesystem::path path,
    LogCursor base_cursor,
    OpenOptions options) {
    auto limits_result = validate_limits(options.limits);
    if (!limits_result) {
        return core::Result<SealedFrameLog, LogError>::failure(std::move(limits_result).error());
    }
    if (base_cursor.next_command == 0U) {
        return core::Result<SealedFrameLog, LogError>::failure(make_error(
            LogErrorCode::InvalidHeader, 0U, "base cursor must have a nonzero next command ID"));
    }

    std::error_code exists_error;
    const auto exists = std::filesystem::exists(path, exists_error);
    if (exists_error) {
        return core::Result<SealedFrameLog, LogError>::failure(make_error(
            LogErrorCode::OpenFailed, 0U, "could not inspect WAL path", exists_error));
    }

    bool initialize = !exists;
    if (exists) {
        auto size_result = checked_file_size(path);
        if (!size_result) {
            return core::Result<SealedFrameLog, LogError>::failure(std::move(size_result).error());
        }
        initialize = size_result.value() == 0U;
    }
    if (initialize) {
        auto initialization = initialize_file(path, base_cursor, options.sync_policy);
        if (!initialization) {
            return core::Result<SealedFrameLog, LogError>::failure(
                std::move(initialization).error());
        }
    }

    auto scan = scan_log(path, base_cursor, options);
    if (!scan) {
        return core::Result<SealedFrameLog, LogError>::failure(std::move(scan).error());
    }
    // A durable receipt is useful only if the WAL remains reachable by name.
    // Sync on every durable open so a retry after a prior directory-sync
    // failure cannot accidentally skip namespace durability.
    if (options.sync_policy != SyncPolicy::None) {
        auto directory_sync = sync_parent_directory(path);
        if (!directory_sync) {
            return core::Result<SealedFrameLog, LogError>::failure(
                std::move(directory_sync).error());
        }
    }
    auto scan_value = std::move(scan).value();
    return core::Result<SealedFrameLog, LogError>::success(SealedFrameLog{
        std::move(path),
        base_cursor,
        options,
        scan_value.report,
        std::move(scan_value.offsets)});
}

core::Result<AppendReceipt, LogError> SealedFrameLog::append(
    const SealedFrameBytes& frame) {
    auto validation = preflight(frame);
    if (!validation) {
        return core::Result<AppendReceipt, LogError>::failure(std::move(validation).error());
    }

    auto size_result = checked_file_size(path_);
    if (!size_result) {
        usable_ = false;
        return core::Result<AppendReceipt, LogError>::failure(std::move(size_result).error());
    }
    if (size_result.value() != recovery_.valid_bytes) {
        usable_ = false;
        return core::Result<AppendReceipt, LogError>::failure(make_error(
            LogErrorCode::Unusable,
            recovery_.valid_bytes,
            "WAL size changed outside this writer"));
    }

    try {
        // Make the bookkeeping after the durable commit marker non-throwing.
        entry_offsets_.reserve(entry_offsets_.size() + 1U);
    } catch (const std::bad_alloc&) {
        return core::Result<AppendReceipt, LogError>::failure(make_error(
            LogErrorCode::LimitExceeded,
            recovery_.valid_bytes,
            "could not reserve the WAL entry index"));
    } catch (const std::length_error&) {
        return core::Result<AppendReceipt, LogError>::failure(make_error(
            LogErrorCode::LimitExceeded,
            recovery_.valid_bytes,
            "WAL entry index reached its maximum size"));
    }

    auto bytes = encode_entry(frame);
    const auto offset = recovery_.valid_bytes;
    const auto body_size = bytes.size() - kTrailerSize;
    {
        std::ofstream stream(path_, std::ios::binary | std::ios::app);
        if (!stream) {
            usable_ = false;
            return core::Result<AppendReceipt, LogError>::failure(make_error(
                LogErrorCode::OpenFailed,
                offset,
                "could not open WAL body for append",
                current_io_error()));
        }
        stream.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(body_size));
        stream.flush();
        if (!stream) {
            usable_ = false;
            return core::Result<AppendReceipt, LogError>::failure(make_error(
                LogErrorCode::WriteFailed,
                offset,
                "could not append the sealed-frame body",
                current_io_error()));
        }
        stream.close();
        if (!stream) {
            usable_ = false;
            return core::Result<AppendReceipt, LogError>::failure(make_error(
                LogErrorCode::WriteFailed,
                offset,
                "could not close the appended sealed-frame body",
                current_io_error()));
        }
    }

    // The body must reach stable storage before the commit marker can become
    // visible. This ordering is what makes an incomplete final entry safely
    // truncatable instead of indistinguishable from committed corruption.
    auto body_sync = sync_file(path_, SyncPolicy::Data);
    if (!body_sync) {
        usable_ = false;
        auto error = std::move(body_sync).error();
        error.offset = offset;
        return core::Result<AppendReceipt, LogError>::failure(std::move(error));
    }

    {
        std::ofstream stream(path_, std::ios::binary | std::ios::app);
        if (!stream) {
            usable_ = false;
            return core::Result<AppendReceipt, LogError>::failure(make_error(
                LogErrorCode::OpenFailed,
                offset + static_cast<std::uint64_t>(body_size),
                "could not open WAL commit trailer for append",
                current_io_error()));
        }
        stream.write(
            reinterpret_cast<const char*>(bytes.data() + body_size),
            static_cast<std::streamsize>(kTrailerSize));
        stream.flush();
        if (!stream) {
            usable_ = false;
            return core::Result<AppendReceipt, LogError>::failure(make_error(
                LogErrorCode::WriteFailed,
                offset + static_cast<std::uint64_t>(body_size),
                "could not append the sealed-frame commit trailer",
                current_io_error()));
        }
        stream.close();
        if (!stream) {
            usable_ = false;
            return core::Result<AppendReceipt, LogError>::failure(make_error(
                LogErrorCode::WriteFailed,
                offset + static_cast<std::uint64_t>(body_size),
                "could not close the sealed-frame commit trailer",
                current_io_error()));
        }
    }

    auto sync_result = sync_file(path_, options_.sync_policy);
    if (!sync_result) {
        usable_ = false;
        auto error = std::move(sync_result).error();
        error.offset = offset;
        return core::Result<AppendReceipt, LogError>::failure(std::move(error));
    }

    const auto cursor = LogCursor{frame.frame, frame.result_revision, frame.next_command_id};
    entry_offsets_.push_back(offset);
    tail_cursor_ = cursor;
    recovery_.tail_cursor = cursor;
    ++recovery_.entry_count;
    recovery_.valid_bytes += static_cast<std::uint64_t>(bytes.size());

    return core::Result<AppendReceipt, LogError>::success(
        AppendReceipt{
            offset,
            static_cast<std::uint64_t>(bytes.size()),
            cursor,
            options_.sync_policy != SyncPolicy::None,
            get_u32(
                std::span<const std::byte>{bytes},
                bytes.size() - kTrailerSize + 16U),
        });
}

LogStatus SealedFrameLog::preflight(const SealedFrameBytes& frame) const {
    if (!usable_) {
        return LogStatus::failure(make_error(
            LogErrorCode::Unusable,
            recovery_.valid_bytes,
            "WAL must be reopened after an earlier append or sync failure"));
    }

    auto validation = validate_frame(
        frame, tail_cursor_, options_.limits, recovery_.valid_bytes);
    if (!validation) {
        return validation;
    }
    if (entry_offsets_.size() == entry_offsets_.max_size()) {
        return LogStatus::failure(make_error(
            LogErrorCode::LimitExceeded,
            recovery_.valid_bytes,
            "WAL entry index cannot represent another frame"));
    }
    return success_status();
}

core::Result<SealedFrameLogSnapshot, LogError>
SealedFrameLog::capture_committed() const {
    if (!usable_) {
        return core::Result<SealedFrameLogSnapshot, LogError>::failure(make_error(
            LogErrorCode::Unusable,
            recovery_.valid_bytes,
            "WAL must be reopened before capturing a committed prefix"));
    }
    try {
        return core::Result<SealedFrameLogSnapshot, LogError>::success(
            SealedFrameLogSnapshot{
                path_,
                base_cursor_,
                tail_cursor_,
                recovery_.valid_bytes,
                options_.limits,
            });
    } catch (const std::bad_alloc&) {
        return core::Result<SealedFrameLogSnapshot, LogError>::failure(make_error(
            LogErrorCode::LimitExceeded,
            recovery_.valid_bytes,
            "could not allocate immutable WAL snapshot metadata"));
    } catch (const std::length_error&) {
        return core::Result<SealedFrameLogSnapshot, LogError>::failure(make_error(
            LogErrorCode::LimitExceeded,
            recovery_.valid_bytes,
            "immutable WAL snapshot metadata exceeds platform limits"));
    }
}

LogStatus SealedFrameLog::flush() const {
    if (!usable_) {
        return LogStatus::failure(make_error(
            LogErrorCode::Unusable,
            recovery_.valid_bytes,
            "WAL must be reopened after an earlier append or sync failure"));
    }
    auto result = sync_file(path_, options_.sync_policy);
    if (!result) {
        auto error = std::move(result).error();
        error.offset = recovery_.valid_bytes;
        return LogStatus::failure(std::move(error));
    }
    return success_status();
}

LogStatus SealedFrameLogSnapshot::replay_after(
    LogCursor cursor,
    const ReplayVisitor& visitor) const {
    if (!visitor) {
        return LogStatus::failure(make_error(
            LogErrorCode::VisitorFailed, 0U, "replay visitor is empty"));
    }

    return replay_after_with_identity(
        cursor,
        [&visitor](
            const SealedFrameBytes& frame,
            const WalFrameIdentity&) { return visitor(frame); });
}

LogStatus SealedFrameLogSnapshot::visit_committed(
    const IdentityReplayVisitor& visitor) const {
    if (!visitor) {
        return LogStatus::failure(make_error(
            LogErrorCode::VisitorFailed, 0U, "identity visitor is empty"));
    }

    auto size_result = checked_file_size(path_);
    if (!size_result) {
        return LogStatus::failure(std::move(size_result).error());
    }
    if (size_result.value() < valid_bytes_) {
        return LogStatus::failure(make_error(
            LogErrorCode::Unusable,
            valid_bytes_,
            "WAL is shorter than the captured committed prefix"));
    }

    auto header = read_header(path_, valid_bytes_);
    if (!header) {
        return LogStatus::failure(std::move(header).error());
    }
    if (header.value() != base_cursor_) {
        return LogStatus::failure(make_error(
            LogErrorCode::BaseCursorMismatch,
            0U,
            "captured WAL base cursor no longer matches the backing file"));
    }

    auto offset = static_cast<std::uint64_t>(kHeaderSize);
    auto expected = base_cursor_;
    while (offset < valid_bytes_) {
        auto entry_result = decode_entry(path_, offset, valid_bytes_, limits_);
        if (!entry_result) {
            return LogStatus::failure(std::move(entry_result).error());
        }
        const auto& entry = entry_result.value();
        auto validation = validate_frame(entry.frame, expected, limits_, offset);
        if (!validation) {
            return validation;
        }

        const auto after = LogCursor{
            entry.frame.frame,
            entry.frame.result_revision,
            entry.frame.next_command_id,
        };
        const auto identity = WalFrameIdentity{
            expected,
            after,
            offset,
            entry.total_bytes,
            entry.entry_crc32c,
        };
        auto visit_result = visitor(entry.frame, identity);
        if (!visit_result) {
            return visit_result;
        }
        expected = after;
        offset += entry.total_bytes;
    }
    if (offset != valid_bytes_ || expected != tail_cursor_) {
        return LogStatus::failure(make_error(
            LogErrorCode::ChainMismatch,
            offset,
            "captured WAL visit did not reach its recorded tail"));
    }
    return success_status();
}

LogStatus SealedFrameLogSnapshot::replay_after_with_identity(
    LogCursor cursor,
    const IdentityReplayVisitor& visitor) const {
    if (!visitor) {
        return LogStatus::failure(make_error(
            LogErrorCode::VisitorFailed, 0U, "identity replay visitor is empty"));
    }

    auto size_result = checked_file_size(path_);
    if (!size_result) {
        return LogStatus::failure(std::move(size_result).error());
    }
    if (size_result.value() < valid_bytes_) {
        return LogStatus::failure(make_error(
            LogErrorCode::Unusable,
            valid_bytes_,
            "WAL is shorter than the captured committed prefix"));
    }

    auto header = read_header(path_, valid_bytes_);
    if (!header) {
        return LogStatus::failure(std::move(header).error());
    }
    if (header.value() != base_cursor_) {
        return LogStatus::failure(make_error(
            LogErrorCode::BaseCursorMismatch,
            0U,
            "captured WAL base cursor no longer matches the backing file"));
    }

    auto offset = static_cast<std::uint64_t>(kHeaderSize);
    auto replay_offset = offset;
    auto expected = base_cursor_;
    bool found = cursor == base_cursor_;
    while (offset < valid_bytes_) {
        auto entry_result = decode_entry(path_, offset, valid_bytes_, limits_);
        if (!entry_result) {
            return LogStatus::failure(std::move(entry_result).error());
        }
        const auto& entry = entry_result.value();
        auto validation = validate_frame(entry.frame, expected, limits_, offset);
        if (!validation) {
            return validation;
        }
        expected = LogCursor{
            entry.frame.frame,
            entry.frame.result_revision,
            entry.frame.next_command_id,
        };
        offset += entry.total_bytes;
        if (!found && cursor == expected) {
            found = true;
            replay_offset = offset;
        }
    }
    if (offset != valid_bytes_ || expected != tail_cursor_) {
        return LogStatus::failure(make_error(
            LogErrorCode::ChainMismatch,
            offset,
            "captured WAL prefix does not terminate at its recorded tail"));
    }
    if (!found) {
        return LogStatus::failure(make_error(
            LogErrorCode::CursorNotFound,
            0U,
            "replay cursor is not the base or a captured committed frame boundary"));
    }

    offset = replay_offset;
    expected = cursor;
    while (offset < valid_bytes_) {
        auto entry_result = decode_entry(path_, offset, valid_bytes_, limits_);
        if (!entry_result) {
            return LogStatus::failure(std::move(entry_result).error());
        }
        const auto& entry = entry_result.value();
        auto validation = validate_frame(entry.frame, expected, limits_, offset);
        if (!validation) {
            return validation;
        }
        const auto after = LogCursor{
            entry.frame.frame,
            entry.frame.result_revision,
            entry.frame.next_command_id,
        };
        const auto identity = WalFrameIdentity{
            expected,
            after,
            offset,
            entry.total_bytes,
            entry.entry_crc32c,
        };
        auto visit_result = visitor(entry.frame, identity);
        if (!visit_result) {
            return visit_result;
        }
        expected = after;
        offset += entry.total_bytes;
    }
    if (offset != valid_bytes_ || expected != tail_cursor_) {
        return LogStatus::failure(make_error(
            LogErrorCode::ChainMismatch,
            offset,
            "captured WAL replay did not reach its recorded tail"));
    }
    return success_status();
}

LogStatus SealedFrameLog::replay_after(
    LogCursor cursor,
    const ReplayVisitor& visitor) const {
    if (!usable_) {
        return LogStatus::failure(make_error(
            LogErrorCode::Unusable,
            recovery_.valid_bytes,
            "WAL must be reopened before replay"));
    }
    if (!visitor) {
        return LogStatus::failure(make_error(
            LogErrorCode::VisitorFailed, 0U, "replay visitor is empty"));
    }

    auto size_result = checked_file_size(path_);
    if (!size_result) {
        return LogStatus::failure(std::move(size_result).error());
    }
    if (size_result.value() != recovery_.valid_bytes) {
        return LogStatus::failure(make_error(
            LogErrorCode::Unusable,
            recovery_.valid_bytes,
            "WAL size changed outside this reader"));
    }

    std::size_t replay_index = 0U;
    bool found = cursor == base_cursor_;
    auto expected = base_cursor_;
    for (std::size_t index = 0U; index < entry_offsets_.size(); ++index) {
        const auto offset = entry_offsets_[index];
        auto entry_result = decode_entry(path_, offset, recovery_.valid_bytes, options_.limits);
        if (!entry_result) {
            return LogStatus::failure(std::move(entry_result).error());
        }
        const auto& entry = entry_result.value();
        auto validation = validate_frame(entry.frame, expected, options_.limits, offset);
        if (!validation) {
            return validation;
        }
        expected = LogCursor{
            entry.frame.frame, entry.frame.result_revision, entry.frame.next_command_id};
        if (!found && cursor == expected) {
            found = true;
            replay_index = index + 1U;
        }
    }
    if (!found) {
        return LogStatus::failure(make_error(
            LogErrorCode::CursorNotFound,
            0U,
            "replay cursor is not the base or a committed frame boundary"));
    }

    for (auto index = replay_index; index < entry_offsets_.size(); ++index) {
        auto entry_result = decode_entry(
            path_, entry_offsets_[index], recovery_.valid_bytes, options_.limits);
        if (!entry_result) {
            return LogStatus::failure(std::move(entry_result).error());
        }
        auto visit_result = visitor(entry_result.value().frame);
        if (!visit_result) {
            return visit_result;
        }
    }
    return success_status();
}

} // namespace mediaserver::persist
