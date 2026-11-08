#include <mediaserver/persist/release_ledger.hpp>

#include <mediaserver/core/crc32c.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <limits>
#include <span>
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
constexpr std::size_t kPrefixSize = 104U;
constexpr std::size_t kTrailerSize = 24U;
constexpr std::size_t kEntrySize = kPrefixSize + kTrailerSize;
constexpr std::size_t kHeaderCrcOffset = 60U;
constexpr std::size_t kPrefixCrcOffset = 100U;

constexpr std::array<std::byte, 8U> kHeaderMagic{
    std::byte{0x4d}, std::byte{0x53}, std::byte{0x52}, std::byte{0x45},
    std::byte{0x4c}, std::byte{0x4c}, std::byte{0x4f}, std::byte{0x47}}; // MSRELLOG
constexpr std::array<std::byte, 8U> kEntryMagic{
    std::byte{0x4d}, std::byte{0x53}, std::byte{0x52}, std::byte{0x45},
    std::byte{0x4c}, std::byte{0x45}, std::byte{0x53}, std::byte{0x31}}; // MSRELES1
constexpr std::array<std::byte, 8U> kCommitMagic{
    std::byte{0x4d}, std::byte{0x53}, std::byte{0x43}, std::byte{0x4f},
    std::byte{0x4d}, std::byte{0x4d}, std::byte{0x49}, std::byte{0x54}}; // MSCOMMIT

using ReleaseStatus = core::Result<std::monostate, ReleaseLedgerError>;

using core::crc32c;

void put_u16(
    std::span<std::byte> bytes,
    std::size_t offset,
    std::uint16_t value) noexcept {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void put_u32(
    std::span<std::byte> bytes,
    std::size_t offset,
    std::uint32_t value) noexcept {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        bytes[offset + static_cast<std::size_t>(shift / 8U)] =
            static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

void put_u64(
    std::span<std::byte> bytes,
    std::size_t offset,
    std::uint64_t value) noexcept {
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        bytes[offset + static_cast<std::size_t>(shift / 8U)] =
            static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

[[nodiscard]] std::uint16_t get_u16(
    std::span<const std::byte> bytes,
    std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(
            std::to_integer<std::uint8_t>(bytes[offset]))
        | static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(
                std::to_integer<std::uint8_t>(bytes[offset + 1U]))
            << 8U));
}

[[nodiscard]] std::uint32_t get_u32(
    std::span<const std::byte> bytes,
    std::size_t offset) noexcept {
    std::uint32_t value = 0U;
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        const auto index = offset + static_cast<std::size_t>(shift / 8U);
        value |= static_cast<std::uint32_t>(
                     std::to_integer<std::uint8_t>(bytes[index]))
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
        value |= static_cast<std::uint64_t>(
                     std::to_integer<std::uint8_t>(bytes[index]))
            << shift;
    }
    return value;
}

template <std::size_t Size>
[[nodiscard]] bool has_magic(
    std::span<const std::byte> bytes,
    std::size_t offset,
    const std::array<std::byte, Size>& magic) noexcept {
    return bytes.size() >= offset + Size
        && std::equal(
            magic.begin(),
            magic.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

[[nodiscard]] bool is_zero(std::span<const std::byte> bytes) noexcept {
    return std::all_of(bytes.begin(), bytes.end(), [](std::byte value) {
        return value == std::byte{0U};
    });
}

[[nodiscard]] ReleaseLedgerError make_error(
    ReleaseLedgerErrorCode code,
    std::uint64_t offset,
    std::string detail,
    std::error_code system_error = {},
    std::optional<CommandLogError> command_log_error = std::nullopt) {
    return ReleaseLedgerError{
        code,
        offset,
        system_error,
        std::move(detail),
        std::move(command_log_error),
    };
}

[[nodiscard]] ReleaseStatus success_status() {
    return ReleaseStatus::success(std::monostate{});
}

[[nodiscard]] std::error_code current_io_error() noexcept {
    return std::error_code{errno, std::generic_category()};
}

[[nodiscard]] core::Result<std::uint64_t, ReleaseLedgerError>
checked_file_size(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return core::Result<std::uint64_t, ReleaseLedgerError>::failure(
            make_error(
                ReleaseLedgerErrorCode::ReadFailed,
                0U,
                "could not determine release-ledger size",
                error));
    }
    if (size > std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<std::uint64_t, ReleaseLedgerError>::failure(
            make_error(
                ReleaseLedgerErrorCode::LimitExceeded,
                0U,
                "release ledger is too large to address"));
    }
    return core::Result<std::uint64_t, ReleaseLedgerError>::success(
        static_cast<std::uint64_t>(size));
}

[[nodiscard]] ReleaseStatus sync_file(
    const std::filesystem::path& path,
    SyncPolicy policy) {
    if (policy == SyncPolicy::None) {
        return ReleaseStatus::failure(make_error(
            ReleaseLedgerErrorCode::NonDurablePolicy,
            0U,
            "release ledger requires Data or Full sync policy"));
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
        return ReleaseStatus::failure(make_error(
            ReleaseLedgerErrorCode::SyncFailed,
            0U,
            "could not open release ledger for durable sync",
            std::error_code{
                static_cast<int>(GetLastError()), std::system_category()}));
    }

    const auto flushed = FlushFileBuffers(handle) != 0;
    const auto flush_error = flushed
        ? std::error_code{}
        : std::error_code{
            static_cast<int>(GetLastError()), std::system_category()};
    const auto closed = CloseHandle(handle) != 0;
    if (!flushed || !closed) {
        const auto error = flushed
            ? std::error_code{
                static_cast<int>(GetLastError()), std::system_category()}
            : flush_error;
        return ReleaseStatus::failure(make_error(
            ReleaseLedgerErrorCode::SyncFailed,
            0U,
            "durable release-ledger sync failed",
            error));
    }
#else
    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), O_RDWR);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        return ReleaseStatus::failure(make_error(
            ReleaseLedgerErrorCode::SyncFailed,
            0U,
            "could not open release ledger for durable sync",
            current_io_error()));
    }

    int sync_result = 0;
    do {
        sync_result = policy == SyncPolicy::Data ? ::fdatasync(descriptor)
                                                  : ::fsync(descriptor);
    } while (sync_result != 0 && errno == EINTR);
    const auto sync_error = sync_result == 0
        ? std::error_code{}
        : current_io_error();
    const auto close_result = ::close(descriptor);
    if (sync_result != 0 || close_result != 0) {
        return ReleaseStatus::failure(make_error(
            ReleaseLedgerErrorCode::SyncFailed,
            0U,
            "durable release-ledger sync failed",
            sync_result != 0 ? sync_error : current_io_error()));
    }
#endif
    return success_status();
}

[[nodiscard]] ReleaseStatus sync_parent_directory(
    const std::filesystem::path& path) {
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
        return ReleaseStatus::failure(make_error(
            ReleaseLedgerErrorCode::SyncFailed,
            0U,
            "could not open release-ledger parent directory for sync",
            current_io_error()));
    }

    int sync_result = 0;
    do {
        sync_result = ::fsync(descriptor);
    } while (sync_result != 0 && errno == EINTR);
    const auto sync_error = sync_result == 0
        ? std::error_code{}
        : current_io_error();
    const auto close_result = ::close(descriptor);
    if (sync_result != 0 || close_result != 0) {
        return ReleaseStatus::failure(make_error(
            ReleaseLedgerErrorCode::SyncFailed,
            0U,
            "durable release-ledger parent-directory sync failed",
            sync_result != 0 ? sync_error : current_io_error()));
    }
    return success_status();
#endif
}

[[nodiscard]] std::array<std::byte, kHeaderSize> make_header(
    LogCursor base) noexcept {
    std::array<std::byte, kHeaderSize> bytes{};
    std::copy(kHeaderMagic.begin(), kHeaderMagic.end(), bytes.begin());
    auto span = std::span<std::byte>{bytes};
    put_u16(span, 8U, kFormatVersion);
    put_u16(span, 10U, static_cast<std::uint16_t>(kHeaderSize));
    put_u32(span, 12U, 0U);
    put_u64(span, 16U, base.through_frame);
    put_u64(span, 24U, base.revision);
    put_u64(span, 32U, base.next_command);
    put_u32(
        span,
        kHeaderCrcOffset,
        crc32c(std::span<const std::byte>{bytes}.first(kHeaderCrcOffset)));
    return bytes;
}

[[nodiscard]] core::Result<LogCursor, ReleaseLedgerError> read_header(
    const std::filesystem::path& path,
    std::uint64_t file_size) {
    if (file_size < kHeaderSize) {
        return core::Result<LogCursor, ReleaseLedgerError>::failure(make_error(
            ReleaseLedgerErrorCode::InvalidHeader,
            0U,
            "release-ledger header is truncated"));
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return core::Result<LogCursor, ReleaseLedgerError>::failure(make_error(
            ReleaseLedgerErrorCode::OpenFailed,
            0U,
            "could not open release ledger for reading",
            current_io_error()));
    }
    std::array<std::byte, kHeaderSize> bytes{};
    stream.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return core::Result<LogCursor, ReleaseLedgerError>::failure(make_error(
            ReleaseLedgerErrorCode::ReadFailed,
            0U,
            "could not read complete release-ledger header",
            current_io_error()));
    }

    const auto view = std::span<const std::byte>{bytes};
    if (!has_magic(view, 0U, kHeaderMagic)
        || get_u16(view, 10U) != static_cast<std::uint16_t>(kHeaderSize)
        || get_u32(view, 12U) != 0U || !is_zero(view.subspan(40U, 20U))
        || get_u32(view, kHeaderCrcOffset)
            != crc32c(view.first(kHeaderCrcOffset))) {
        return core::Result<LogCursor, ReleaseLedgerError>::failure(make_error(
            ReleaseLedgerErrorCode::InvalidHeader,
            0U,
            "release-ledger header failed structural or CRC validation"));
    }
    if (get_u16(view, 8U) != kFormatVersion) {
        return core::Result<LogCursor, ReleaseLedgerError>::failure(make_error(
            ReleaseLedgerErrorCode::UnsupportedVersion,
            0U,
            "unsupported release-ledger format version"));
    }

    const auto base = LogCursor{
        get_u64(view, 16U),
        get_u64(view, 24U),
        get_u64(view, 32U),
    };
    if (base.next_command == 0U) {
        return core::Result<LogCursor, ReleaseLedgerError>::failure(make_error(
            ReleaseLedgerErrorCode::InvalidHeader,
            0U,
            "release-ledger base has a zero next-command ID"));
    }
    return core::Result<LogCursor, ReleaseLedgerError>::success(base);
}

[[nodiscard]] ReleaseStatus initialize_file(
    const std::filesystem::path& path,
    LogCursor base,
    SyncPolicy policy) {
    const auto header = make_header(base);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return ReleaseStatus::failure(make_error(
            ReleaseLedgerErrorCode::OpenFailed,
            0U,
            "could not create release ledger",
            current_io_error()));
    }
    stream.write(
        reinterpret_cast<const char*>(header.data()),
        static_cast<std::streamsize>(header.size()));
    stream.flush();
    if (!stream) {
        return ReleaseStatus::failure(make_error(
            ReleaseLedgerErrorCode::WriteFailed,
            0U,
            "could not write release-ledger header",
            current_io_error()));
    }
    stream.close();
    if (!stream) {
        return ReleaseStatus::failure(make_error(
            ReleaseLedgerErrorCode::WriteFailed,
            0U,
            "could not close release-ledger header",
            current_io_error()));
    }
    return sync_file(path, policy);
}

[[nodiscard]] bool valid_cursor_transition(
    const LogCursor& before,
    const LogCursor& after) noexcept {
    if (before.next_command == 0U || after.next_command == 0U
        || before.through_frame == std::numeric_limits<std::uint64_t>::max()
        || after.through_frame != before.through_frame + 1U
        || after.revision < before.revision
        || after.next_command < before.next_command) {
        return false;
    }
    return after.revision - before.revision
        == after.next_command - before.next_command;
}

[[nodiscard]] std::array<std::byte, kEntrySize> encode_entry(
    std::uint64_t ordinal,
    const WalFrameIdentity& identity) noexcept {
    std::array<std::byte, kEntrySize> bytes{};
    std::copy(kEntryMagic.begin(), kEntryMagic.end(), bytes.begin());
    auto span = std::span<std::byte>{bytes};
    put_u16(span, 8U, kFormatVersion);
    put_u16(span, 10U, static_cast<std::uint16_t>(kPrefixSize));
    put_u32(span, 12U, 0U);
    put_u64(span, 16U, static_cast<std::uint64_t>(kEntrySize));
    put_u64(span, 24U, ordinal);
    put_u64(span, 32U, identity.before.through_frame);
    put_u64(span, 40U, identity.before.revision);
    put_u64(span, 48U, identity.before.next_command);
    put_u64(span, 56U, identity.after.through_frame);
    put_u64(span, 64U, identity.after.revision);
    put_u64(span, 72U, identity.after.next_command);
    put_u64(span, 80U, identity.offset);
    put_u64(span, 88U, identity.bytes);
    put_u32(span, 96U, identity.entry_crc32c);
    put_u32(
        span,
        kPrefixCrcOffset,
        crc32c(std::span<const std::byte>{bytes}.first(kPrefixCrcOffset)));

    std::copy(
        kCommitMagic.begin(),
        kCommitMagic.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(kPrefixSize));
    put_u64(
        span,
        kPrefixSize + 8U,
        static_cast<std::uint64_t>(kEntrySize));
    put_u32(
        span,
        kPrefixSize + 16U,
        crc32c(std::span<const std::byte>{bytes}.first(kPrefixSize + 16U)));
    put_u32(
        span,
        kPrefixSize + 20U,
        crc32c(std::span<const std::byte>{bytes}.subspan(kPrefixSize, 20U)));
    return bytes;
}

struct DecodedRelease final {
    std::uint64_t ordinal{};
    WalFrameIdentity identity{};
};

[[nodiscard]] bool decode_committed_entry_bytes(
    std::span<const std::byte, kEntrySize> view,
    DecodedRelease& decoded) noexcept {
    const auto trailer = view.subspan(kPrefixSize, kTrailerSize);
    if (!has_magic(view, 0U, kEntryMagic)
        || get_u16(view, 8U) != kFormatVersion
        || get_u16(view, 10U) != static_cast<std::uint16_t>(kPrefixSize)
        || get_u32(view, 12U) != 0U
        || get_u64(view, 16U) != kEntrySize
        || get_u32(view, kPrefixCrcOffset)
            != crc32c(view.first(kPrefixCrcOffset))
        || !has_magic(trailer, 0U, kCommitMagic)
        || get_u64(trailer, 8U) != kEntrySize
        || get_u32(trailer, 16U)
            != crc32c(view.first(kPrefixSize + 16U))
        || get_u32(trailer, 20U) != crc32c(trailer.first(20U))) {
        return false;
    }

    decoded = DecodedRelease{
        get_u64(view, 24U),
        WalFrameIdentity{
            LogCursor{
                get_u64(view, 32U),
                get_u64(view, 40U),
                get_u64(view, 48U),
            },
            LogCursor{
                get_u64(view, 56U),
                get_u64(view, 64U),
                get_u64(view, 72U),
            },
            get_u64(view, 80U),
            get_u64(view, 88U),
            get_u32(view, 96U),
        },
    };
    return true;
}

// Historical permit catch-up must not retain an O(history) index. This helper
// reads and authenticates exactly one fixed-size record without throwing or
// allocating through the C++ stream/filesystem layers.
[[nodiscard]] std::optional<DecodedRelease> read_committed_entry_noexcept(
    const std::filesystem::path& path,
    std::uint64_t offset) noexcept {
    std::array<std::byte, kEntrySize> bytes{};
#ifdef _WIN32
    if (offset > static_cast<std::uint64_t>(
                     std::numeric_limits<LONGLONG>::max())) {
        return std::nullopt;
    }
    const auto handle = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    auto succeeded = SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) != 0;
    std::size_t read_bytes = 0U;
    while (succeeded && read_bytes < bytes.size()) {
        DWORD consumed = 0U;
        const auto remaining = static_cast<DWORD>(bytes.size() - read_bytes);
        succeeded = ReadFile(
                        handle,
                        bytes.data() + read_bytes,
                        remaining,
                        &consumed,
                        nullptr)
            != 0;
        if (!succeeded || consumed == 0U) {
            succeeded = false;
            break;
        }
        read_bytes += static_cast<std::size_t>(consumed);
    }
    if (CloseHandle(handle) == 0 || !succeeded
        || read_bytes != bytes.size()) {
        return std::nullopt;
    }
#else
    constexpr auto max_offset =
        static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    if (offset > max_offset
        || static_cast<std::uint64_t>(bytes.size()) > max_offset - offset) {
        return std::nullopt;
    }
    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), O_RDONLY);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        return std::nullopt;
    }
    std::size_t read_bytes = 0U;
    bool succeeded = true;
    while (read_bytes < bytes.size()) {
        ssize_t consumed = -1;
        do {
            consumed = ::pread(
                descriptor,
                bytes.data() + read_bytes,
                bytes.size() - read_bytes,
                static_cast<off_t>(offset + read_bytes));
        } while (consumed < 0 && errno == EINTR);
        if (consumed <= 0) {
            succeeded = false;
            break;
        }
        read_bytes += static_cast<std::size_t>(consumed);
    }
    if (::close(descriptor) != 0 || !succeeded
        || read_bytes != bytes.size()) {
        return std::nullopt;
    }
#endif

    DecodedRelease decoded;
    if (!decode_committed_entry_bytes(
            std::span<const std::byte, kEntrySize>{bytes}, decoded)) {
        return std::nullopt;
    }
    return decoded;
}

[[nodiscard]] core::Result<bool, ReleaseLedgerError>
has_authenticated_commit_suffix_at_or_after(
    std::ifstream& stream,
    std::uint64_t scan_offset,
    std::uint64_t file_size) {
    if (scan_offset > file_size || file_size - scan_offset < kTrailerSize) {
        return core::Result<bool, ReleaseLedgerError>::success(false);
    }
    if (file_size > static_cast<std::uint64_t>(
                        std::numeric_limits<std::streamoff>::max())) {
        return core::Result<bool, ReleaseLedgerError>::failure(make_error(
            ReleaseLedgerErrorCode::LimitExceeded,
            scan_offset,
            "release-ledger suffix exceeds stream addressing range"));
    }

    stream.clear();
    stream.seekg(static_cast<std::streamoff>(scan_offset));
    constexpr std::size_t chunk_bytes = 64U * 1024U;
    constexpr std::size_t overlap_bytes = kTrailerSize - 1U;
    std::array<std::byte, chunk_bytes + overlap_bytes> buffer{};
    std::size_t overlap = 0U;
    auto read_offset = scan_offset;
    while (read_offset < file_size) {
        const auto remaining = file_size - read_offset;
        const auto requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, chunk_bytes));
        stream.read(
            reinterpret_cast<char*>(buffer.data() + overlap),
            static_cast<std::streamsize>(requested));
        if (stream.gcount() != static_cast<std::streamsize>(requested)) {
            return core::Result<bool, ReleaseLedgerError>::failure(make_error(
                ReleaseLedgerErrorCode::ReadFailed,
                read_offset,
                "could not scan release-ledger suffix for commit evidence",
                current_io_error()));
        }

        const auto available = overlap + requested;
        const auto buffer_offset = read_offset - overlap;
        const auto view = std::span<const std::byte>{buffer}.first(available);
        for (std::size_t local = 0U;
             local + kTrailerSize <= available;
             ++local) {
            const auto trailer = view.subspan(local, kTrailerSize);
            if (!has_magic(trailer, 0U, kCommitMagic)
                || get_u32(trailer, 20U) != crc32c(trailer.first(20U))
                || get_u64(trailer, 8U) != kEntrySize) {
                continue;
            }
            const auto trailer_offset =
                buffer_offset + static_cast<std::uint64_t>(local);
            const auto candidate_end = trailer_offset + kTrailerSize;
            if (kEntrySize <= candidate_end
                && candidate_end - kEntrySize >= scan_offset) {
                return core::Result<bool, ReleaseLedgerError>::success(true);
            }
        }

        read_offset += requested;
        const auto next_overlap = std::min(overlap_bytes, available);
        for (std::size_t index = 0U; index < next_overlap; ++index) {
            buffer[index] = buffer[available - next_overlap + index];
        }
        overlap = next_overlap;
    }
    return core::Result<bool, ReleaseLedgerError>::success(false);
}

[[nodiscard]] core::Result<DecodedRelease, ReleaseLedgerError> decode_entry(
    const std::filesystem::path& path,
    std::uint64_t offset,
    std::uint64_t file_size) {
    if (offset > file_size || file_size - offset < kPrefixSize) {
        return core::Result<DecodedRelease, ReleaseLedgerError>::failure(
            make_error(
                ReleaseLedgerErrorCode::TornTail,
                offset,
                "release ledger ends inside an entry prefix"));
    }
    if (offset > static_cast<std::uint64_t>(
                     std::numeric_limits<std::streamoff>::max())) {
        return core::Result<DecodedRelease, ReleaseLedgerError>::failure(
            make_error(
                ReleaseLedgerErrorCode::LimitExceeded,
                offset,
                "release-ledger offset exceeds stream addressing range"));
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return core::Result<DecodedRelease, ReleaseLedgerError>::failure(
            make_error(
                ReleaseLedgerErrorCode::OpenFailed,
                offset,
                "could not open release-ledger entry",
                current_io_error()));
    }
    stream.seekg(static_cast<std::streamoff>(offset));
    std::array<std::byte, kPrefixSize> prefix{};
    stream.read(
        reinterpret_cast<char*>(prefix.data()),
        static_cast<std::streamsize>(prefix.size()));
    if (stream.gcount() != static_cast<std::streamsize>(prefix.size())) {
        return core::Result<DecodedRelease, ReleaseLedgerError>::failure(
            make_error(
                ReleaseLedgerErrorCode::ReadFailed,
                offset,
                "could not read complete release-ledger prefix",
                current_io_error()));
    }

    const auto prefix_view = std::span<const std::byte>{prefix};
    if (!has_magic(prefix_view, 0U, kEntryMagic)
        || get_u16(prefix_view, 10U) != static_cast<std::uint16_t>(kPrefixSize)
        || get_u32(prefix_view, 12U) != 0U
        || get_u64(prefix_view, 16U) != kEntrySize
        || get_u32(prefix_view, kPrefixCrcOffset)
            != crc32c(prefix_view.first(kPrefixCrcOffset))) {
        auto commit_evidence = has_authenticated_commit_suffix_at_or_after(
            stream, offset, file_size);
        if (!commit_evidence) {
            return core::Result<DecodedRelease, ReleaseLedgerError>::failure(
                std::move(commit_evidence).error());
        }
        return core::Result<DecodedRelease, ReleaseLedgerError>::failure(
            make_error(
                commit_evidence.value()
                    ? ReleaseLedgerErrorCode::CorruptEntry
                    : ReleaseLedgerErrorCode::TornTail,
                offset,
                commit_evidence.value()
                    ? "release-ledger prefix is invalid before commit evidence"
                    : "final release append has no authenticated commit marker"));
    }
    if (get_u16(prefix_view, 8U) != kFormatVersion) {
        return core::Result<DecodedRelease, ReleaseLedgerError>::failure(
            make_error(
                ReleaseLedgerErrorCode::UnsupportedVersion,
                offset,
                "unsupported release-ledger entry version"));
    }
    if (file_size - offset < kEntrySize) {
        return core::Result<DecodedRelease, ReleaseLedgerError>::failure(
            make_error(
                ReleaseLedgerErrorCode::TornTail,
                offset,
                "release ledger ends before the entry commit trailer"));
    }

    std::array<std::byte, kEntrySize> bytes{};
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset));
    stream.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return core::Result<DecodedRelease, ReleaseLedgerError>::failure(
            make_error(
                ReleaseLedgerErrorCode::ReadFailed,
                offset,
                "could not read complete release-ledger entry",
                current_io_error()));
    }

    const auto view = std::span<const std::byte>{bytes};
    const auto trailer = view.subspan(kPrefixSize, kTrailerSize);
    const auto valid_commit = has_magic(trailer, 0U, kCommitMagic)
        && get_u64(trailer, 8U) == kEntrySize
        && get_u32(trailer, 20U) == crc32c(trailer.first(20U));
    if (!valid_commit) {
        const auto final_entry = file_size - offset == kEntrySize;
        return core::Result<DecodedRelease, ReleaseLedgerError>::failure(
            make_error(
                final_entry ? ReleaseLedgerErrorCode::TornTail
                            : ReleaseLedgerErrorCode::CorruptEntry,
                offset,
                final_entry
                    ? "final release entry has no valid commit marker"
                    : "interior release entry has an invalid commit marker"));
    }
    if (get_u32(trailer, 16U)
        != crc32c(view.first(kPrefixSize + 16U))) {
        return core::Result<DecodedRelease, ReleaseLedgerError>::failure(
            make_error(
                ReleaseLedgerErrorCode::CorruptEntry,
                offset,
                "committed release entry failed body CRC validation"));
    }

    return core::Result<DecodedRelease, ReleaseLedgerError>::success(
        DecodedRelease{
            get_u64(view, 24U),
            WalFrameIdentity{
                LogCursor{
                    get_u64(view, 32U),
                    get_u64(view, 40U),
                    get_u64(view, 48U),
                },
                LogCursor{
                    get_u64(view, 56U),
                    get_u64(view, 64U),
                    get_u64(view, 72U),
                },
                get_u64(view, 80U),
                get_u64(view, 88U),
                get_u32(view, 96U),
            },
        });
}

struct LedgerScan final {
    ReleaseLedgerRecoveryReport report{};
};

[[nodiscard]] core::Result<LedgerScan, ReleaseLedgerError> scan_ledger(
    const std::filesystem::path& path,
    LogCursor expected_base,
    const ReleaseLedgerOpenOptions& options) {
    auto size_result = checked_file_size(path);
    if (!size_result) {
        return core::Result<LedgerScan, ReleaseLedgerError>::failure(
            std::move(size_result).error());
    }
    auto file_size = size_result.value();
    auto header = read_header(path, file_size);
    if (!header) {
        return core::Result<LedgerScan, ReleaseLedgerError>::failure(
            std::move(header).error());
    }
    if (header.value() != expected_base) {
        return core::Result<LedgerScan, ReleaseLedgerError>::failure(
            make_error(
                ReleaseLedgerErrorCode::BaseCursorMismatch,
                0U,
                "release-ledger base differs from captured WAL base"));
    }

    LedgerScan scan;
    scan.report.base_cursor = expected_base;
    scan.report.frontier = expected_base;
    scan.report.released_wal_bytes = kHeaderSize;
    scan.report.valid_bytes = kHeaderSize;
    auto offset = static_cast<std::uint64_t>(kHeaderSize);
    while (offset < file_size) {
        auto decoded = decode_entry(path, offset, file_size);
        if (!decoded) {
            if (decoded.error().code == ReleaseLedgerErrorCode::TornTail
                && options.repair_torn_tail) {
                break;
            }
            return core::Result<LedgerScan, ReleaseLedgerError>::failure(
                std::move(decoded).error());
        }

        const auto& release = decoded.value();
        if (scan.report.decision_count
                == std::numeric_limits<std::uint64_t>::max()
            || release.ordinal != scan.report.decision_count + 1U
            || release.identity.before != scan.report.frontier
            || !valid_cursor_transition(
                release.identity.before, release.identity.after)
            || release.identity.offset != scan.report.released_wal_bytes
            || release.identity.bytes == 0U
            || release.identity.bytes
                > std::numeric_limits<std::uint64_t>::max()
                    - release.identity.offset) {
            return core::Result<LedgerScan, ReleaseLedgerError>::failure(
                make_error(
                    ReleaseLedgerErrorCode::ChainMismatch,
                    offset,
                    "release entry does not continue its ordinal, cursor, or WAL-byte chain"));
        }

        ++scan.report.decision_count;
        scan.report.frontier = release.identity.after;
        scan.report.released_wal_bytes =
            release.identity.offset + release.identity.bytes;
        offset += kEntrySize;
    }

    if (offset != file_size) {
        if (!options.repair_torn_tail) {
            return core::Result<LedgerScan, ReleaseLedgerError>::failure(
                make_error(
                    ReleaseLedgerErrorCode::TornTail,
                    offset,
                    "release ledger has a torn final append"));
        }
        std::error_code truncate_error;
        std::filesystem::resize_file(path, offset, truncate_error);
        if (truncate_error) {
            return core::Result<LedgerScan, ReleaseLedgerError>::failure(
                make_error(
                    ReleaseLedgerErrorCode::TruncateFailed,
                    offset,
                    "could not remove torn release-ledger tail",
                    truncate_error));
        }
        auto synced = sync_file(path, SyncPolicy::Data);
        if (!synced) {
            return core::Result<LedgerScan, ReleaseLedgerError>::failure(
                std::move(synced).error());
        }
        scan.report.repaired_torn_tail = true;
        scan.report.truncated_bytes = file_size - offset;
        file_size = offset;
    }
    scan.report.valid_bytes = file_size;
    return core::Result<LedgerScan, ReleaseLedgerError>::success(
        std::move(scan));
}

[[nodiscard]] LogCursor to_log_cursor(
    const cmd::StreamCursor& cursor) noexcept {
    return LogCursor{
        cursor.through_frame.value(),
        cursor.revision.value(),
        cursor.next_command.value(),
    };
}

[[nodiscard]] core::Result<ReleaseLedgerRecoveryReport, ReleaseLedgerError>
bind_to_wal(
    LedgerScan& scan,
    const std::filesystem::path& ledger_path,
    const CommandLogSnapshot& wal) {
    std::uint64_t release_index = 0U;
    std::optional<ReleaseLedgerError> binding_error;
    auto visited = wal.visit_committed_batches(
        [&scan, &ledger_path, &release_index, &binding_error](
            const ValidatedCommandBatchView& batch) {
            ++scan.report.wal_entry_count;
            if (release_index >= scan.report.decision_count) {
                return BatchVisitControl::Continue;
            }
            if (release_index
                > (std::numeric_limits<std::uint64_t>::max() - kHeaderSize)
                    / kEntrySize) {
                binding_error = make_error(
                    ReleaseLedgerErrorCode::LimitExceeded,
                    scan.report.valid_bytes,
                    "release-ledger record offset is not representable");
                return BatchVisitControl::Stop;
            }
            const auto ledger_offset =
                static_cast<std::uint64_t>(kHeaderSize)
                + release_index * static_cast<std::uint64_t>(kEntrySize);
            auto release = decode_entry(
                ledger_path, ledger_offset, scan.report.valid_bytes);
            if (!release) {
                binding_error = std::move(release).error();
                return BatchVisitControl::Stop;
            }
            if (release.value().identity != batch.identity) {
                binding_error = make_error(
                    ReleaseLedgerErrorCode::WalIdentityMismatch,
                    ledger_offset,
                    "release entry does not match the exact captured WAL frame");
                return BatchVisitControl::Stop;
            }
            ++release_index;
            return BatchVisitControl::Continue;
        });
    if (binding_error) {
        return core::Result<
            ReleaseLedgerRecoveryReport,
            ReleaseLedgerError>::failure(std::move(*binding_error));
    }
    if (!visited) {
        return core::Result<
            ReleaseLedgerRecoveryReport,
            ReleaseLedgerError>::failure(make_error(
            ReleaseLedgerErrorCode::WalSnapshotFailure,
            0U,
            "captured WAL failed typed identity validation",
            {},
            std::move(visited.error())));
    }
    if (!visited.value().completed
        || release_index != scan.report.decision_count) {
        return core::Result<
            ReleaseLedgerRecoveryReport,
            ReleaseLedgerError>::failure(make_error(
            ReleaseLedgerErrorCode::WalIdentityMismatch,
            scan.report.valid_bytes,
            "release ledger extends beyond the captured WAL prefix"));
    }

    scan.report.wal_tail = to_log_cursor(wal.tail_cursor());
    if (scan.report.decision_count > scan.report.wal_entry_count) {
        return core::Result<
            ReleaseLedgerRecoveryReport,
            ReleaseLedgerError>::failure(make_error(
            ReleaseLedgerErrorCode::WalIdentityMismatch,
            scan.report.valid_bytes,
            "release frontier is ahead of the captured WAL tail"));
    }
    scan.report.unresolved_wal_frames =
        scan.report.wal_entry_count - scan.report.decision_count;
    return core::Result<
        ReleaseLedgerRecoveryReport,
        ReleaseLedgerError>::success(scan.report);
}

} // namespace

DurableReleaseReceipt::DurableReleaseReceipt(
    DurableReleaseReceipt&& other) noexcept
    : ordinal_(other.ordinal_),
      offset_(other.offset_),
      bytes_written_(other.bytes_written_),
      frame_(other.frame_),
      valid_(std::exchange(other.valid_, false)) {}

DurableReleaseReceipt& DurableReleaseReceipt::operator=(
    DurableReleaseReceipt&& other) noexcept {
    if (this != &other) {
        ordinal_ = other.ordinal_;
        offset_ = other.offset_;
        bytes_written_ = other.bytes_written_;
        frame_ = other.frame_;
        valid_ = std::exchange(other.valid_, false);
    }
    return *this;
}

ReleaseLedger::ReleaseLedger(ReleaseLedger&& other) noexcept
    : path_(std::move(other.path_)),
      options_(other.options_),
      recovery_(other.recovery_),
      usable_(std::exchange(other.usable_, false)) {}

ReleaseLedger& ReleaseLedger::operator=(ReleaseLedger&& other) noexcept {
    if (this != &other) {
        // Moving a published ledger concurrently with an operation is not
        // supported; the destination retains its own mutex.
        path_ = std::move(other.path_);
        options_ = other.options_;
        recovery_ = other.recovery_;
        usable_ = std::exchange(other.usable_, false);
    }
    return *this;
}

core::Result<ReleaseLedger, ReleaseLedgerError>
ReleaseLedger::open_or_create(
    std::filesystem::path path,
    const CommandLogSnapshot& wal,
    ReleaseLedgerOpenOptions options) {
    using Result = core::Result<ReleaseLedger, ReleaseLedgerError>;
    if (options.sync_policy == SyncPolicy::None) {
        return Result::failure(make_error(
            ReleaseLedgerErrorCode::NonDurablePolicy,
            0U,
            "release ledger requires Data or Full sync policy"));
    }

    const auto base = to_log_cursor(wal.base_cursor());
    const auto tail = to_log_cursor(wal.tail_cursor());
    std::error_code existence_error;
    const auto existed = std::filesystem::exists(path, existence_error);
    if (existence_error) {
        return Result::failure(make_error(
            ReleaseLedgerErrorCode::OpenFailed,
            0U,
            "could not inspect release-ledger path",
            existence_error));
    }

    std::uint64_t existing_size = 0U;
    if (existed) {
        auto size = checked_file_size(path);
        if (!size) {
            return Result::failure(std::move(size).error());
        }
        existing_size = size.value();
    }
    if (!existed || existing_size == 0U) {
        if (tail != base) {
            return Result::failure(make_error(
                ReleaseLedgerErrorCode::MissingForNonEmptyWal,
                0U,
                "a release ledger cannot be created for a nonempty WAL"));
        }
        auto created = initialize_file(path, base, options.sync_policy);
        if (!created) {
            return Result::failure(std::move(created).error());
        }
    }

    auto scan = scan_ledger(path, base, options);
    if (!scan) {
        return Result::failure(std::move(scan).error());
    }
    auto bound = bind_to_wal(scan.value(), path, wal);
    if (!bound) {
        return Result::failure(std::move(bound).error());
    }
    auto synced = sync_file(path, options.sync_policy);
    if (!synced) {
        return Result::failure(std::move(synced).error());
    }
    // The directory entry is part of the durable release authority. Sync it
    // on every durable open, including Data-policy creation and retries after
    // a previous directory-sync failure.
    auto directory_sync = sync_parent_directory(path);
    if (!directory_sync) {
        return Result::failure(std::move(directory_sync).error());
    }

    return Result::success(
        ReleaseLedger{std::move(path), options, bound.value()});
}

core::Result<DurableReleaseReceipt, ReleaseLedgerError>
ReleaseLedger::append_release(const WalFrameIdentity& frame) {
    using Result = core::Result<DurableReleaseReceipt, ReleaseLedgerError>;
    const std::lock_guard lock{mutex_};
    if (!usable_) {
        return Result::failure(make_error(
            ReleaseLedgerErrorCode::Unusable,
            recovery_.valid_bytes,
            "release ledger must be reopened after an append or sync failure"));
    }
    if (recovery_.decision_count == std::numeric_limits<std::uint64_t>::max()) {
        return Result::failure(make_error(
            ReleaseLedgerErrorCode::LimitExceeded,
            recovery_.valid_bytes,
            "release decision ordinal is exhausted"));
    }
    if (frame.before != recovery_.frontier
        || !valid_cursor_transition(frame.before, frame.after)
        || frame.offset != recovery_.released_wal_bytes || frame.bytes == 0U
        || frame.bytes
            > std::numeric_limits<std::uint64_t>::max() - frame.offset) {
        return Result::failure(make_error(
            ReleaseLedgerErrorCode::ChainMismatch,
            recovery_.valid_bytes,
            "release does not continue the frontier or exact WAL-byte chain"));
    }
    auto size = checked_file_size(path_);
    if (!size) {
        usable_ = false;
        return Result::failure(std::move(size).error());
    }
    if (size.value() != recovery_.valid_bytes) {
        usable_ = false;
        return Result::failure(make_error(
            ReleaseLedgerErrorCode::Unusable,
            recovery_.valid_bytes,
            "release-ledger size changed outside this writer"));
    }

    const auto ordinal = recovery_.decision_count + 1U;
    const auto bytes = encode_entry(ordinal, frame);
    const auto offset = recovery_.valid_bytes;
    {
        std::ofstream stream(path_, std::ios::binary | std::ios::app);
        if (!stream) {
            usable_ = false;
            return Result::failure(make_error(
                ReleaseLedgerErrorCode::OpenFailed,
                offset,
                "could not open release-ledger body for append",
                current_io_error()));
        }
        stream.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(kPrefixSize));
        stream.flush();
        if (!stream) {
            usable_ = false;
            return Result::failure(make_error(
                ReleaseLedgerErrorCode::WriteFailed,
                offset,
                "could not append release-ledger body",
                current_io_error()));
        }
        stream.close();
        if (!stream) {
            usable_ = false;
            return Result::failure(make_error(
                ReleaseLedgerErrorCode::WriteFailed,
                offset,
                "could not close release-ledger body append",
                current_io_error()));
        }
    }

    auto body_sync = sync_file(path_, SyncPolicy::Data);
    if (!body_sync) {
        usable_ = false;
        auto error = std::move(body_sync).error();
        error.offset = offset;
        return Result::failure(std::move(error));
    }

    {
        std::ofstream stream(path_, std::ios::binary | std::ios::app);
        if (!stream) {
            usable_ = false;
            return Result::failure(make_error(
                ReleaseLedgerErrorCode::OpenFailed,
                offset + kPrefixSize,
                "could not open release-ledger trailer for append",
                current_io_error()));
        }
        stream.write(
            reinterpret_cast<const char*>(bytes.data() + kPrefixSize),
            static_cast<std::streamsize>(kTrailerSize));
        stream.flush();
        if (!stream) {
            usable_ = false;
            return Result::failure(make_error(
                ReleaseLedgerErrorCode::WriteFailed,
                offset + kPrefixSize,
                "could not append release-ledger commit trailer",
                current_io_error()));
        }
        stream.close();
        if (!stream) {
            usable_ = false;
            return Result::failure(make_error(
                ReleaseLedgerErrorCode::WriteFailed,
                offset + kPrefixSize,
                "could not close release-ledger trailer append",
                current_io_error()));
        }
    }

    auto final_sync = sync_file(path_, options_.sync_policy);
    if (!final_sync) {
        usable_ = false;
        auto error = std::move(final_sync).error();
        error.offset = offset;
        return Result::failure(std::move(error));
    }

    ++recovery_.decision_count;
    recovery_.frontier = frame.after;
    recovery_.released_wal_bytes = frame.offset + frame.bytes;
    recovery_.valid_bytes += kEntrySize;
    if (recovery_.unresolved_wal_frames != 0U) {
        --recovery_.unresolved_wal_frames;
    } else if (recovery_.wal_tail == frame.before) {
        recovery_.wal_tail = frame.after;
        ++recovery_.wal_entry_count;
    }
    return Result::success(DurableReleaseReceipt{
        ordinal,
        offset,
        static_cast<std::uint64_t>(kEntrySize),
        frame,
    });
}

LogCursor ReleaseLedger::frontier() const noexcept {
    const std::lock_guard lock{mutex_};
    return recovery_.frontier;
}

std::uint64_t ReleaseLedger::next_release_ordinal() const noexcept {
    const std::lock_guard lock{mutex_};
    if (!usable_ || recovery_.decision_count
        == std::numeric_limits<std::uint64_t>::max()) {
        return 0U;
    }
    return recovery_.decision_count + 1U;
}

std::optional<std::uint64_t> ReleaseLedger::release_ordinal(
    LogCursor before,
    LogCursor after) const noexcept {
    const std::lock_guard lock{mutex_};
    if (!usable_
        || after.through_frame <= recovery_.base_cursor.through_frame) {
        return std::nullopt;
    }
    const auto ordinal =
        after.through_frame - recovery_.base_cursor.through_frame;
    if (ordinal == 0U || ordinal > recovery_.decision_count) {
        return std::nullopt;
    }
    const auto index = ordinal - 1U;
    if (index
        > (std::numeric_limits<std::uint64_t>::max() - kHeaderSize)
            / kEntrySize) {
        return std::nullopt;
    }
    const auto offset = static_cast<std::uint64_t>(kHeaderSize)
        + index * static_cast<std::uint64_t>(kEntrySize);
    if (offset > recovery_.valid_bytes
        || recovery_.valid_bytes - offset < kEntrySize) {
        return std::nullopt;
    }
    const auto release = read_committed_entry_noexcept(path_, offset);
    if (!release || release->ordinal != ordinal
        || release->identity.before != before
        || release->identity.after != after) {
        return std::nullopt;
    }
    return ordinal;
}

ReleaseLedgerRecoveryReport ReleaseLedger::recovery_report() const noexcept {
    const std::lock_guard lock{mutex_};
    return recovery_;
}

} // namespace mediaserver::persist
