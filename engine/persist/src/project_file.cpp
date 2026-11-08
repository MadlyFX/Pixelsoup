#include <mediaserver/persist/project_file.hpp>

#include <mediaserver/core/crc32c.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace mediaserver::persist {
namespace {

constexpr std::size_t kHeaderSize = 32U;
constexpr std::size_t kHeaderCrcOffset = 28U;
constexpr std::size_t kMaximumTempAttempts = 128U;
constexpr std::array<std::byte, 8U> kProjectMagic{
    std::byte{0x4d}, std::byte{0x53}, std::byte{0x50}, std::byte{0x52},
    std::byte{0x4f}, std::byte{0x4a}, std::byte{0x30}, std::byte{0x31}}; // MSPROJ01

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
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
        static_cast<std::uint16_t>(
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

[[nodiscard]] ProjectFileError make_error(
    ProjectFileErrorCode code,
    std::error_code system_error = {},
    bool committed = false) noexcept {
    ProjectFileError error;
    error.code = code;
    error.system_error = system_error;
    error.committed = committed;
    return error;
}

[[nodiscard]] ProjectSaveResult save_failure(ProjectFileError error) {
    return ProjectSaveResult::failure(std::move(error));
}

[[nodiscard]] ProjectLoadResult load_failure(ProjectFileError error) {
    return ProjectLoadResult::failure(std::move(error));
}

[[nodiscard]] std::error_code current_io_error() noexcept {
#ifdef _WIN32
    return std::error_code{
        static_cast<int>(GetLastError()), std::system_category()};
#else
    return std::error_code{errno, std::generic_category()};
#endif
}

[[nodiscard]] std::error_code stream_io_error() noexcept {
    if (errno != 0) {
        return std::error_code{errno, std::generic_category()};
    }
    return std::make_error_code(std::errc::io_error);
}

[[nodiscard]] bool valid_target_path(const std::filesystem::path& path) {
    if (path.empty() || path.filename().empty()) {
        return false;
    }
    const auto filename = path.filename();
    return filename != "." && filename != "..";
}

#ifdef _WIN32
using NativeFile = HANDLE;
const NativeFile kInvalidFile = INVALID_HANDLE_VALUE;
#else
using NativeFile = int;
constexpr NativeFile kInvalidFile = -1;
#endif

void close_file_noexcept(NativeFile file) noexcept {
    if (file == kInvalidFile) {
        return;
    }
#ifdef _WIN32
    static_cast<void>(CloseHandle(file));
#else
    static_cast<void>(::close(file));
#endif
}

class TemporaryFile final {
public:
    TemporaryFile(std::filesystem::path path, NativeFile file)
        : path_(std::move(path)), file_(file) {}

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    TemporaryFile(TemporaryFile&& other) noexcept
        : path_(std::move(other.path_)),
          file_(std::exchange(other.file_, kInvalidFile)),
          owns_path_(std::exchange(other.owns_path_, false)) {}

    TemporaryFile& operator=(TemporaryFile&&) = delete;

    ~TemporaryFile() {
        close_file_noexcept(file_);
        if (owns_path_) {
            std::error_code ignored;
            static_cast<void>(std::filesystem::remove(path_, ignored));
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    [[nodiscard]] NativeFile file() const noexcept { return file_; }

    void mark_closed() noexcept { file_ = kInvalidFile; }

    void mark_replaced() noexcept { owns_path_ = false; }

private:
    std::filesystem::path path_;
    NativeFile file_{kInvalidFile};
    bool owns_path_{true};
};

using TemporaryFileResult = core::Result<TemporaryFile, ProjectFileError>;

[[nodiscard]] std::uint64_t process_id() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

[[nodiscard]] TemporaryFileResult create_temporary_file(
    const std::filesystem::path& target) {
    static std::atomic<std::uint64_t> nonce{0U};
    std::error_code last_error;
    for (std::size_t attempt = 0U; attempt < kMaximumTempAttempts; ++attempt) {
        auto path = target;
        const auto suffix = ".tmp." + std::to_string(process_id()) + "." +
                            std::to_string(nonce.fetch_add(1U));
        path += std::filesystem::path{suffix};
#ifdef _WIN32
        const auto file = CreateFileW(
            path.c_str(),
            GENERIC_WRITE,
            0U,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file != kInvalidFile) {
            return TemporaryFileResult::success(
                TemporaryFile{std::move(path), file});
        }
        last_error = current_io_error();
        if (last_error.value() != ERROR_FILE_EXISTS &&
            last_error.value() != ERROR_ALREADY_EXISTS) {
            break;
        }
#else
        int file = -1;
        do {
            file = ::open(
                path.c_str(),
                O_WRONLY | O_CREAT | O_EXCL,
                static_cast<mode_t>(0666));
        } while (file < 0 && errno == EINTR);
        if (file != kInvalidFile) {
            return TemporaryFileResult::success(
                TemporaryFile{std::move(path), file});
        }
        last_error = current_io_error();
        if (errno != EEXIST) {
            break;
        }
#endif
    }
    return TemporaryFileResult::failure(
        make_error(ProjectFileErrorCode::OpenFailed, last_error));
}

[[nodiscard]] core::Result<std::monostate, ProjectFileError>
write_sync_and_close(
    TemporaryFile& temporary,
    std::span<const std::byte> bytes) {
#ifdef _WIN32
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const auto amount = static_cast<DWORD>(std::min<std::size_t>(
            remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0U;
        if (WriteFile(
                temporary.file(),
                bytes.data() + offset,
                amount,
                &written,
                nullptr) == 0 ||
            written != amount) {
            return core::Result<std::monostate, ProjectFileError>::failure(
                make_error(ProjectFileErrorCode::WriteFailed, current_io_error()));
        }
        offset += static_cast<std::size_t>(written);
    }
    if (FlushFileBuffers(temporary.file()) == 0) {
        return core::Result<std::monostate, ProjectFileError>::failure(
            make_error(ProjectFileErrorCode::SyncFailed, current_io_error()));
    }
    if (CloseHandle(temporary.file()) == 0) {
        return core::Result<std::monostate, ProjectFileError>::failure(
            make_error(ProjectFileErrorCode::WriteFailed, current_io_error()));
    }
    temporary.mark_closed();
#else
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        ssize_t written = -1;
        do {
            written = ::write(
                temporary.file(), bytes.data() + offset, bytes.size() - offset);
        } while (written < 0 && errno == EINTR);
        if (written <= 0) {
            return core::Result<std::monostate, ProjectFileError>::failure(
                make_error(ProjectFileErrorCode::WriteFailed, current_io_error()));
        }
        offset += static_cast<std::size_t>(written);
    }
    int sync_result = -1;
    do {
        sync_result = ::fsync(temporary.file());
    } while (sync_result != 0 && errno == EINTR);
    if (sync_result != 0) {
        return core::Result<std::monostate, ProjectFileError>::failure(
            make_error(ProjectFileErrorCode::SyncFailed, current_io_error()));
    }
    const auto close_result = ::close(temporary.file());
    temporary.mark_closed();
    if (close_result != 0) {
        return core::Result<std::monostate, ProjectFileError>::failure(
            make_error(ProjectFileErrorCode::WriteFailed, current_io_error()));
    }
#endif
    return core::Result<std::monostate, ProjectFileError>::success(
        std::monostate{});
}

[[nodiscard]] std::error_code replace_file(
    const std::filesystem::path& source,
    const std::filesystem::path& target) noexcept {
#ifdef _WIN32
    if (MoveFileExW(
            source.c_str(),
            target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        return current_io_error();
    }
#else
    int result = -1;
    do {
        result = ::rename(source.c_str(), target.c_str());
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        return current_io_error();
    }
#endif
    return {};
}

[[nodiscard]] std::error_code sync_parent_directory(
    const std::filesystem::path& parent) noexcept {
#ifdef _WIN32
    static_cast<void>(parent);
    return {};
#else
    auto flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    int descriptor = -1;
    do {
        descriptor = ::open(parent.c_str(), flags);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        return current_io_error();
    }
    int sync_result = -1;
    do {
        sync_result = ::fsync(descriptor);
    } while (sync_result != 0 && errno == EINTR);
    const auto sync_error = sync_result == 0 ? std::error_code{} : current_io_error();
    const auto close_result = ::close(descriptor);
    if (sync_result != 0) {
        return sync_error;
    }
    if (close_result != 0) {
        return current_io_error();
    }
    return {};
#endif
}

[[nodiscard]] std::vector<std::byte> make_project_bytes(
    std::span<const std::byte> payload) {
    std::vector<std::byte> bytes(kHeaderSize + payload.size());
    std::copy(kProjectMagic.begin(), kProjectMagic.end(), bytes.begin());
    auto view = std::span<std::byte>{bytes};
    put_u16(view, 8U, current_project_format_version);
    put_u16(view, 10U, static_cast<std::uint16_t>(kHeaderSize));
    put_u32(view, 12U, 0U);
    put_u64(view, 16U, static_cast<std::uint64_t>(payload.size()));
    put_u32(view, 24U, crc32c(payload));
    put_u32(view, kHeaderCrcOffset, crc32c(view.first(kHeaderCrcOffset)));
    std::copy(payload.begin(), payload.end(), bytes.begin() + kHeaderSize);
    return bytes;
}

[[nodiscard]] ProjectSaveResult save_project_file_impl(
    const std::filesystem::path& path,
    const doc::Root& project,
    const wire::DecodeLimits& limits) {
    if (!valid_target_path(path)) {
        return save_failure(make_error(ProjectFileErrorCode::InvalidPath));
    }

    const auto checkpoint = cmd::StreamCheckpoint{
        cmd::StreamCursor{
            cmd::FrameNumber{0U}, project.version(), cmd::CommandId{1U}},
        project};
    auto encoded = wire::encode(wire::Message{checkpoint}, limits);
    if (!encoded) {
        const auto code = encoded.error() == wire::EncodeError::SizeLimitExceeded
                              ? ProjectFileErrorCode::SizeLimitExceeded
                              : ProjectFileErrorCode::EncodeFailed;
        auto error = make_error(code);
        error.encode_error = encoded.error();
        return save_failure(std::move(error));
    }
    if (encoded.value().size() >
        std::numeric_limits<std::size_t>::max() - kHeaderSize) {
        return save_failure(make_error(ProjectFileErrorCode::SizeLimitExceeded));
    }
    auto bytes = make_project_bytes(encoded.value());

    auto parent = path.parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    auto temporary_result = create_temporary_file(path);
    if (!temporary_result) {
        return save_failure(std::move(temporary_result.error()));
    }
    auto temporary = std::move(temporary_result).value();
    auto written = write_sync_and_close(temporary, bytes);
    if (!written) {
        return save_failure(std::move(written.error()));
    }

    const auto replace_error = replace_file(temporary.path(), path);
    if (replace_error) {
        return save_failure(make_error(
            ProjectFileErrorCode::ReplaceFailed, replace_error));
    }
    temporary.mark_replaced();

    const auto directory_error = sync_parent_directory(parent);
    if (directory_error) {
        return save_failure(make_error(
            ProjectFileErrorCode::SyncFailed, directory_error, true));
    }

    return ProjectSaveResult::success(ProjectSaveReceipt{
        project.version(), static_cast<std::uint64_t>(bytes.size()), true});
}

[[nodiscard]] ProjectLoadResult load_project_file_impl(
    const std::filesystem::path& path,
    const wire::DecodeLimits& limits) {
    if (!valid_target_path(path)) {
        return load_failure(make_error(ProjectFileErrorCode::InvalidPath));
    }

    errno = 0;
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return load_failure(make_error(
            ProjectFileErrorCode::OpenFailed, stream_io_error()));
    }
    const auto end = stream.tellg();
    if (end < 0) {
        return load_failure(make_error(
            ProjectFileErrorCode::ReadFailed, stream_io_error()));
    }
    const auto file_size = static_cast<std::uint64_t>(end);
    if (file_size < kHeaderSize) {
        return load_failure(make_error(ProjectFileErrorCode::CorruptFile));
    }
    const auto payload_file_bytes = file_size - kHeaderSize;
    if (payload_file_bytes > static_cast<std::uint64_t>(limits.max_message_bytes) ||
        file_size > static_cast<std::uint64_t>(
                        std::numeric_limits<std::size_t>::max())) {
        return load_failure(make_error(ProjectFileErrorCode::SizeLimitExceeded));
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(file_size));
    stream.seekg(0, std::ios::beg);
    if (!stream) {
        return load_failure(make_error(
            ProjectFileErrorCode::ReadFailed, stream_io_error()));
    }
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const auto requested = static_cast<std::streamsize>(
            std::min<std::size_t>(
                bytes.size() - offset,
                static_cast<std::size_t>(
                    std::numeric_limits<std::streamsize>::max())));
        stream.read(
            reinterpret_cast<char*>(bytes.data() + offset), requested);
        if (stream.gcount() != requested) {
            return load_failure(make_error(
                ProjectFileErrorCode::ReadFailed, stream_io_error()));
        }
        offset += static_cast<std::size_t>(requested);
    }
    char trailing_byte{};
    stream.read(&trailing_byte, 1);
    if (stream.gcount() != 0) {
        return load_failure(make_error(ProjectFileErrorCode::CorruptFile));
    }
    if (stream.bad()) {
        return load_failure(make_error(
            ProjectFileErrorCode::ReadFailed, stream_io_error()));
    }

    const auto view = std::span<const std::byte>{bytes};
    if (!std::equal(
            kProjectMagic.begin(), kProjectMagic.end(), view.begin())) {
        return load_failure(make_error(ProjectFileErrorCode::InvalidFormat));
    }
    if (get_u16(view, 8U) != current_project_format_version) {
        return load_failure(make_error(ProjectFileErrorCode::UnsupportedVersion));
    }
    if (get_u32(view, kHeaderCrcOffset) !=
        crc32c(view.first(kHeaderCrcOffset))) {
        return load_failure(make_error(ProjectFileErrorCode::CorruptFile));
    }
    if (get_u16(view, 10U) != static_cast<std::uint16_t>(kHeaderSize) ||
        get_u32(view, 12U) != 0U) {
        return load_failure(make_error(ProjectFileErrorCode::InvalidFormat));
    }
    const auto payload_size = get_u64(view, 16U);
    if (payload_size != payload_file_bytes) {
        return load_failure(make_error(ProjectFileErrorCode::CorruptFile));
    }
    const auto payload = view.subspan(kHeaderSize);
    if (get_u32(view, 24U) != crc32c(payload)) {
        return load_failure(make_error(ProjectFileErrorCode::CorruptFile));
    }

    auto decoded = wire::decode(payload, limits);
    if (!decoded) {
        auto code = ProjectFileErrorCode::CorruptFile;
        if (decoded.error() == wire::DecodeError::UnsupportedVersion) {
            code = ProjectFileErrorCode::UnsupportedVersion;
        } else if (decoded.error() == wire::DecodeError::SizeLimitExceeded) {
            code = ProjectFileErrorCode::SizeLimitExceeded;
        }
        auto error = make_error(code);
        error.decode_error = decoded.error();
        return load_failure(std::move(error));
    }

    auto message = std::move(decoded).value();
    if (!std::holds_alternative<cmd::StreamCheckpoint>(message)) {
        return load_failure(make_error(ProjectFileErrorCode::UnexpectedPayload));
    }
    auto checkpoint = std::get<cmd::StreamCheckpoint>(std::move(message));
    if (checkpoint.cursor.through_frame != cmd::FrameNumber{0U} ||
        checkpoint.cursor.next_command != cmd::CommandId{1U} ||
        checkpoint.cursor.revision != checkpoint.document.version()) {
        return load_failure(make_error(ProjectFileErrorCode::InvalidProjectCursor));
    }
    return ProjectLoadResult::success(std::move(checkpoint.document));
}

} // namespace

ProjectSaveResult save_project_file(
    const std::filesystem::path& path,
    const doc::Root& project,
    const wire::DecodeLimits& limits) {
    try {
        return save_project_file_impl(path, project, limits);
    } catch (const std::bad_alloc&) {
        return save_failure(make_error(ProjectFileErrorCode::AllocationFailed));
    } catch (const std::length_error&) {
        return save_failure(make_error(ProjectFileErrorCode::SizeLimitExceeded));
    } catch (const std::filesystem::filesystem_error& error) {
        return save_failure(make_error(
            ProjectFileErrorCode::OpenFailed, error.code()));
    }
}

ProjectLoadResult load_project_file(
    const std::filesystem::path& path,
    const wire::DecodeLimits& limits) {
    try {
        return load_project_file_impl(path, limits);
    } catch (const std::bad_alloc&) {
        return load_failure(make_error(ProjectFileErrorCode::AllocationFailed));
    } catch (const std::length_error&) {
        return load_failure(make_error(ProjectFileErrorCode::SizeLimitExceeded));
    } catch (const std::filesystem::filesystem_error& error) {
        return load_failure(make_error(
            ProjectFileErrorCode::ReadFailed, error.code()));
    }
}

} // namespace mediaserver::persist
