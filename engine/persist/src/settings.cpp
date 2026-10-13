#include <mediaserver/persist/settings.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
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

constexpr std::size_t kHeaderSize = 24U;
constexpr std::size_t kHeaderCrcOffset = 20U;
constexpr std::size_t kMaximumTempAttempts = 128U;
constexpr std::array<std::byte, 8U> kRecentMagic{
    std::byte{'M'}, std::byte{'S'}, std::byte{'L'}, std::byte{'A'},
    std::byte{'S'}, std::byte{'T'}, std::byte{'0'}, std::byte{'1'}};
constexpr std::array<std::byte, 8U> kPreferencesMagic{
    std::byte{'M'}, std::byte{'S'}, std::byte{'P'}, std::byte{'R'},
    std::byte{'E'}, std::byte{'F'}, std::byte{'0'}, std::byte{'1'}};
constexpr std::array<std::byte, 8U> kWorkspaceLayoutMagic{
    std::byte{'M'}, std::byte{'S'}, std::byte{'W'}, std::byte{'O'},
    std::byte{'R'}, std::byte{'K'}, std::byte{'0'}, std::byte{'1'}};

// One preference value on disk: a one-byte key, a two-byte length, then UTF-8
// bytes. An unknown key is skipped rather than rejected, so a settings file
// written by a newer build still yields the fields this one understands.
enum class PreferenceKey : std::uint8_t {
    DirectorListen = 1U,
    // 2 held a lifecycle secret file path; retired, and skipped if read.
    Wal = 3U,
    ShowId = 4U,
    DirectorEpoch = 5U,
    ProxyCacheDirectory = 6U,
    ProxyCacheNextToSource = 7U,
    ProxyCacheGibibytes = 8U,
    OutputBufferFrames = 9U,
    OutputReadAheadFrames = 10U,
    PlaybackCacheMebibytes = 11U,
    PlaybackTrailFrames = 12U,
    ProxyTranscodePriority = 13U,
    LifecyclePasswordKey = 14U,
    RenderMachineDirector = 15U,
    RenderMachineNodeIndex = 16U,
    RenderMachineControlPort = 17U,
    RenderMachineShowId = 18U,
};

// Every key at its largest. Older builds bounded the file by five values,
// which a record this build writes could exceed only with values far beyond
// anything an operator types.
constexpr std::uint64_t kMaximumPreferencesPayload =
    18ULL * (3ULL + max_preference_value_bytes);

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

void put_u16(std::span<std::byte> bytes, std::size_t offset,
             std::uint16_t value) noexcept {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void put_u32(std::span<std::byte> bytes, std::size_t offset,
             std::uint32_t value) noexcept {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        bytes[offset + static_cast<std::size_t>(shift / 8U)] =
            static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

[[nodiscard]] std::uint16_t get_u16(std::span<const std::byte> bytes,
                                    std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(
            std::to_integer<std::uint8_t>(bytes[offset])) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(
                std::to_integer<std::uint8_t>(bytes[offset + 1U]))
            << 8U));
}

[[nodiscard]] std::uint32_t get_u32(std::span<const std::byte> bytes,
                                    std::size_t offset) noexcept {
    std::uint32_t value{};
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(
                     bytes[offset + static_cast<std::size_t>(shift / 8U)]))
                 << shift;
    }
    return value;
}

[[nodiscard]] bool valid_utf8(std::span<const std::byte> bytes) noexcept {
    std::size_t index = 0U;
    while (index < bytes.size()) {
        const auto lead = std::to_integer<std::uint8_t>(bytes[index]);
        std::size_t continuation_count{};
        std::uint32_t code_point{};
        if (lead <= 0x7fU) {
            if (lead == 0U) {
                return false;
            }
            ++index;
            continue;
        }
        if (lead >= 0xc2U && lead <= 0xdfU) {
            continuation_count = 1U;
            code_point = lead & 0x1fU;
        } else if (lead >= 0xe0U && lead <= 0xefU) {
            continuation_count = 2U;
            code_point = lead & 0x0fU;
        } else if (lead >= 0xf0U && lead <= 0xf4U) {
            continuation_count = 3U;
            code_point = lead & 0x07U;
        } else {
            return false;
        }
        if (continuation_count > bytes.size() - index - 1U) {
            return false;
        }
        for (std::size_t offset = 1U; offset <= continuation_count; ++offset) {
            const auto continuation =
                std::to_integer<std::uint8_t>(bytes[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3fU);
        }
        const bool overlong =
            (continuation_count == 1U && code_point < 0x80U) ||
            (continuation_count == 2U && code_point < 0x800U) ||
            (continuation_count == 3U && code_point < 0x10000U);
        if (overlong || code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

[[nodiscard]] SettingsError make_error(SettingsErrorCode code,
                                       std::error_code system_error = {},
                                       bool committed = false) noexcept {
    return SettingsError{code, system_error, committed};
}

[[nodiscard]] std::error_code current_io_error() noexcept {
#ifdef _WIN32
    return std::error_code{static_cast<int>(GetLastError()),
                           std::system_category()};
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

[[nodiscard]] bool valid_target_path(const std::filesystem::path &path) {
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
    TemporaryFile(const TemporaryFile &) = delete;
    TemporaryFile &operator=(const TemporaryFile &) = delete;
    TemporaryFile(TemporaryFile &&other) noexcept
        : path_(std::move(other.path_)),
          file_(std::exchange(other.file_, kInvalidFile)),
          owns_path_(std::exchange(other.owns_path_, false)) {}
    TemporaryFile &operator=(TemporaryFile &&) = delete;
    ~TemporaryFile() {
        close_file_noexcept(file_);
        if (owns_path_) {
            std::error_code ignored;
            static_cast<void>(std::filesystem::remove(path_, ignored));
        }
    }
    [[nodiscard]] const std::filesystem::path &path() const noexcept {
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

using TemporaryFileResult = core::Result<TemporaryFile, SettingsError>;

[[nodiscard]] std::uint64_t process_id() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

[[nodiscard]] TemporaryFileResult
create_temporary_file(const std::filesystem::path &target) {
    static std::atomic<std::uint64_t> nonce{0U};
    std::error_code last_error;
    for (std::size_t attempt = 0U; attempt < kMaximumTempAttempts; ++attempt) {
        auto path = target;
        path +=
            std::filesystem::path{".tmp." + std::to_string(process_id()) + "." +
                                  std::to_string(nonce.fetch_add(1U))};
#ifdef _WIN32
        const auto file =
            CreateFileW(path.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
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
            file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL,
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
        make_error(SettingsErrorCode::OpenFailed, last_error));
}

[[nodiscard]] core::Result<std::monostate, SettingsError>
write_sync_and_close(TemporaryFile &temporary,
                     std::span<const std::byte> bytes) {
    using Result = core::Result<std::monostate, SettingsError>;
#ifdef _WIN32
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const auto amount = static_cast<DWORD>(std::min<std::size_t>(
            remaining,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written{};
        if (WriteFile(temporary.file(), bytes.data() + offset, amount, &written,
                      nullptr) == 0 ||
            written != amount) {
            return Result::failure(
                make_error(SettingsErrorCode::WriteFailed, current_io_error()));
        }
        offset += static_cast<std::size_t>(written);
    }
    if (FlushFileBuffers(temporary.file()) == 0) {
        return Result::failure(
            make_error(SettingsErrorCode::SyncFailed, current_io_error()));
    }
    if (CloseHandle(temporary.file()) == 0) {
        return Result::failure(
            make_error(SettingsErrorCode::WriteFailed, current_io_error()));
    }
    temporary.mark_closed();
#else
    std::size_t offset{};
    while (offset < bytes.size()) {
        ssize_t written = -1;
        do {
            written = ::write(temporary.file(), bytes.data() + offset,
                              bytes.size() - offset);
        } while (written < 0 && errno == EINTR);
        if (written <= 0) {
            return Result::failure(
                make_error(SettingsErrorCode::WriteFailed, current_io_error()));
        }
        offset += static_cast<std::size_t>(written);
    }
    int sync_result = -1;
    do {
        sync_result = ::fsync(temporary.file());
    } while (sync_result != 0 && errno == EINTR);
    if (sync_result != 0) {
        return Result::failure(
            make_error(SettingsErrorCode::SyncFailed, current_io_error()));
    }
    const auto close_result = ::close(temporary.file());
    temporary.mark_closed();
    if (close_result != 0) {
        return Result::failure(
            make_error(SettingsErrorCode::WriteFailed, current_io_error()));
    }
#endif
    return Result::success(std::monostate{});
}

[[nodiscard]] std::error_code
replace_file(const std::filesystem::path &source,
             const std::filesystem::path &target) noexcept {
#ifdef _WIN32
    if (MoveFileExW(source.c_str(), target.c_str(),
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

[[nodiscard]] std::error_code
sync_parent_directory(const std::filesystem::path &parent) noexcept {
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
    const auto error =
        sync_result == 0 ? std::error_code{} : current_io_error();
    const auto close_result = ::close(descriptor);
    if (sync_result != 0) {
        return error;
    }
    if (close_result != 0) {
        return current_io_error();
    }
    return {};
#endif
}

[[nodiscard]] SettingsPathResult settings_path_failure(SettingsError error) {
    return SettingsPathResult::failure(std::move(error));
}

[[nodiscard]] RecentProjectPathLoadResult load_failure(SettingsError error) {
    return RecentProjectPathLoadResult::failure(std::move(error));
}

[[nodiscard]] RecentProjectPathSaveResult save_failure(SettingsError error) {
    return RecentProjectPathSaveResult::failure(std::move(error));
}

[[nodiscard]] std::vector<std::byte>
make_setting_bytes(std::span<const std::byte, 8U> magic, std::uint16_t version,
                   std::span<const std::byte> payload) {
    std::vector<std::byte> bytes(kHeaderSize + payload.size());
    std::copy(magic.begin(), magic.end(), bytes.begin());
    auto view = std::span<std::byte>{bytes};
    put_u16(view, 8U, version);
    put_u16(view, 10U, static_cast<std::uint16_t>(kHeaderSize));
    put_u32(view, 12U, static_cast<std::uint32_t>(payload.size()));
    put_u32(view, 16U, crc32c(payload));
    put_u32(view, kHeaderCrcOffset, crc32c(view.first(kHeaderCrcOffset)));
    std::copy(payload.begin(), payload.end(), bytes.begin() + kHeaderSize);
    return bytes;
}

[[nodiscard]] RecentProjectPathSaveResult
save_recent_project_path_impl(const std::filesystem::path &settings_file,
                              const std::filesystem::path &project_file) {
    if (!valid_target_path(settings_file)) {
        return save_failure(make_error(SettingsErrorCode::InvalidSettingsPath));
    }
    if (!valid_target_path(project_file)) {
        return save_failure(make_error(SettingsErrorCode::InvalidProjectPath));
    }

    std::error_code absolute_error;
    auto absolute = std::filesystem::absolute(project_file, absolute_error);
    if (absolute_error || !absolute.is_absolute()) {
        return save_failure(
            make_error(SettingsErrorCode::InvalidProjectPath, absolute_error));
    }
    absolute = absolute.lexically_normal();
    const auto encoded = absolute.u8string();
    if (encoded.empty() || encoded.size() > max_recent_project_path_bytes) {
        return save_failure(make_error(SettingsErrorCode::SizeLimitExceeded));
    }
    const auto payload = std::span<const std::byte>{
        reinterpret_cast<const std::byte *>(encoded.data()), encoded.size()};
    if (!valid_utf8(payload)) {
        return save_failure(make_error(SettingsErrorCode::InvalidProjectPath));
    }
    auto bytes =
        make_setting_bytes(std::span<const std::byte, 8U>{kRecentMagic},
                           current_recent_project_format_version, payload);

    auto parent = settings_file.parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    std::error_code directory_error;
    static_cast<void>(
        std::filesystem::create_directories(parent, directory_error));
    if (directory_error) {
        return save_failure(make_error(SettingsErrorCode::DirectoryCreateFailed,
                                       directory_error));
    }

    auto temporary_result = create_temporary_file(settings_file);
    if (!temporary_result) {
        return save_failure(std::move(temporary_result.error()));
    }
    auto temporary = std::move(temporary_result).value();
    auto written = write_sync_and_close(temporary, bytes);
    if (!written) {
        return save_failure(std::move(written.error()));
    }
    const auto replace_error = replace_file(temporary.path(), settings_file);
    if (replace_error) {
        return save_failure(
            make_error(SettingsErrorCode::ReplaceFailed, replace_error));
    }
    temporary.mark_replaced();

    const auto parent_sync_error = sync_parent_directory(parent);
    if (parent_sync_error) {
        return save_failure(
            make_error(SettingsErrorCode::SyncFailed, parent_sync_error, true));
    }
    return RecentProjectPathSaveResult::success(std::move(absolute));
}

[[nodiscard]] RecentProjectPathLoadResult
load_recent_project_path_impl(const std::filesystem::path &settings_file) {
    if (!valid_target_path(settings_file)) {
        return load_failure(make_error(SettingsErrorCode::InvalidSettingsPath));
    }

    errno = 0;
    std::ifstream input(settings_file, std::ios::binary | std::ios::ate);
    if (!input) {
        std::error_code exists_error;
        const bool exists =
            std::filesystem::exists(settings_file, exists_error);
        if (!exists && !exists_error) {
            return RecentProjectPathLoadResult::success(std::nullopt);
        }
        return load_failure(
            make_error(SettingsErrorCode::OpenFailed,
                       exists_error ? exists_error : stream_io_error()));
    }
    const auto end = input.tellg();
    if (end < 0) {
        return load_failure(
            make_error(SettingsErrorCode::ReadFailed, stream_io_error()));
    }
    const auto size = static_cast<std::uint64_t>(end);
    if (size < kHeaderSize ||
        size > kHeaderSize + max_recent_project_path_bytes) {
        return load_failure(
            make_error(size > kHeaderSize + max_recent_project_path_bytes
                           ? SettingsErrorCode::SizeLimitExceeded
                           : SettingsErrorCode::CorruptFile));
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    if (!input) {
        return load_failure(
            make_error(SettingsErrorCode::ReadFailed, stream_io_error()));
    }
    input.read(reinterpret_cast<char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input ||
        input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return load_failure(
            make_error(SettingsErrorCode::ReadFailed, stream_io_error()));
    }

    const auto view = std::span<const std::byte>{bytes};
    if (!std::equal(kRecentMagic.begin(), kRecentMagic.end(), view.begin())) {
        return load_failure(make_error(SettingsErrorCode::InvalidFormat));
    }
    if (get_u16(view, 8U) != current_recent_project_format_version) {
        return load_failure(make_error(SettingsErrorCode::UnsupportedVersion));
    }
    if (get_u16(view, 10U) != kHeaderSize ||
        get_u32(view, kHeaderCrcOffset) !=
            crc32c(view.first(kHeaderCrcOffset))) {
        return load_failure(make_error(SettingsErrorCode::CorruptFile));
    }
    const auto payload_size = get_u32(view, 12U);
    if (payload_size == 0U || payload_size > max_recent_project_path_bytes ||
        static_cast<std::uint64_t>(payload_size) + kHeaderSize != size) {
        return load_failure(
            make_error(payload_size > max_recent_project_path_bytes
                           ? SettingsErrorCode::SizeLimitExceeded
                           : SettingsErrorCode::CorruptFile));
    }
    const auto payload = view.subspan(kHeaderSize, payload_size);
    if (get_u32(view, 16U) != crc32c(payload) || !valid_utf8(payload)) {
        return load_failure(make_error(SettingsErrorCode::CorruptFile));
    }

    std::u8string encoded;
    encoded.resize(payload.size());
    std::transform(
        payload.begin(), payload.end(), encoded.begin(), [](std::byte byte) {
            return static_cast<char8_t>(std::to_integer<std::uint8_t>(byte));
        });
    auto project = std::filesystem::path{encoded}.lexically_normal();
    if (!valid_target_path(project) || !project.is_absolute()) {
        return load_failure(make_error(SettingsErrorCode::CorruptFile));
    }
    return RecentProjectPathLoadResult::success(std::move(project));
}

[[nodiscard]] PreferencesLoadResult
preferences_load_failure(SettingsError error) {
    return PreferencesLoadResult::failure(std::move(error));
}

[[nodiscard]] PreferencesSaveResult
preferences_save_failure(SettingsError error) {
    return PreferencesSaveResult::failure(std::move(error));
}

[[nodiscard]] std::string utf8_from_bytes(std::span<const std::byte> bytes) {
    std::string value;
    value.resize(bytes.size());
    std::transform(
        bytes.begin(), bytes.end(), value.begin(), [](std::byte byte) {
            return static_cast<char>(std::to_integer<std::uint8_t>(byte));
        });
    return value;
}

void put_preference(std::vector<std::byte> &payload, PreferenceKey key,
                    std::string_view value) {
    if (value.empty()) {
        return;
    }
    payload.push_back(static_cast<std::byte>(key));
    payload.push_back(static_cast<std::byte>(value.size() & 0xffU));
    payload.push_back(static_cast<std::byte>((value.size() >> 8U) & 0xffU));
    for (const char character : value) {
        payload.push_back(
            static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
}

void put_preference(std::vector<std::byte> &payload, PreferenceKey key,
                    std::uint32_t value) {
    std::array<char, 16U> encoded{};
    const auto result =
        std::to_chars(encoded.data(), encoded.data() + encoded.size(), value);
    if (result.ec == std::errc{}) {
        put_preference(
            payload, key,
            std::string_view{encoded.data(), static_cast<std::size_t>(
                                                 result.ptr - encoded.data())});
    }
}

[[nodiscard]] bool decode_u32(std::span<const std::byte> bytes,
                              std::uint32_t &out) noexcept {
    const auto *const first = reinterpret_cast<const char *>(bytes.data());
    const auto *const last = first + bytes.size();
    std::uint32_t value{};
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last) {
        return false;
    }
    out = value;
    return true;
}

[[nodiscard]] bool decode_preferences(std::span<const std::byte> payload,
                                      Preferences &out) {
    Preferences decoded{};
    bool seen_listen = false;
    bool seen_secret = false;
    bool seen_wal = false;
    bool seen_show = false;
    bool seen_epoch = false;
    bool seen_proxy_directory = false;
    bool seen_proxy_next_to_source = false;
    bool seen_proxy_cache_gibibytes = false;
    bool seen_output_buffer_frames = false;
    bool seen_output_read_ahead_frames = false;
    bool seen_playback_cache_mebibytes = false;
    bool seen_playback_trail_frames = false;
    bool seen_proxy_transcode_priority = false;
    bool seen_render_director = false;
    bool seen_render_node_index = false;
    bool seen_render_control_port = false;
    bool seen_render_show_id = false;
    std::size_t cursor = 0U;
    while (cursor < payload.size()) {
        if (payload.size() - cursor < 3U) {
            return false;
        }
        const auto key = std::to_integer<std::uint8_t>(payload[cursor]);
        const std::size_t length =
            static_cast<std::size_t>(
                std::to_integer<std::uint8_t>(payload[cursor + 1U])) |
            (static_cast<std::size_t>(
                 std::to_integer<std::uint8_t>(payload[cursor + 2U]))
             << 8U);
        cursor += 3U;
        if (length == 0U || length > max_preference_value_bytes ||
            payload.size() - cursor < length) {
            return false;
        }
        const auto value = payload.subspan(cursor, length);
        cursor += length;
        if (!valid_utf8(value)) {
            return false;
        }
        // A repeated key is corruption, not a later-wins update: the writer
        // emits each field at most once.
        switch (static_cast<PreferenceKey>(key)) {
        case PreferenceKey::DirectorListen:
            if (seen_listen) {
                return false;
            }
            seen_listen = true;
            decoded.director_listen = utf8_from_bytes(value);
            break;
        case PreferenceKey::LifecyclePasswordKey:
            if (seen_secret) {
                return false;
            }
            seen_secret = true;
            decoded.lifecycle_password_key = utf8_from_bytes(value);
            break;
        case PreferenceKey::Wal:
            if (seen_wal) {
                return false;
            }
            seen_wal = true;
            decoded.wal_path = utf8_from_bytes(value);
            break;
        case PreferenceKey::ShowId:
            if (seen_show) {
                return false;
            }
            seen_show = true;
            decoded.show_id = utf8_from_bytes(value);
            break;
        case PreferenceKey::DirectorEpoch:
            if (seen_epoch) {
                return false;
            }
            seen_epoch = true;
            decoded.director_epoch = utf8_from_bytes(value);
            break;
        case PreferenceKey::ProxyCacheDirectory:
            if (seen_proxy_directory) {
                return false;
            }
            seen_proxy_directory = true;
            decoded.proxy_cache_directory = utf8_from_bytes(value);
            break;
        case PreferenceKey::ProxyCacheNextToSource:
            if (seen_proxy_next_to_source || value.size() != 1U ||
                value.front() != std::byte{'1'}) {
                return false;
            }
            seen_proxy_next_to_source = true;
            decoded.proxy_cache_next_to_source = true;
            break;
        case PreferenceKey::ProxyCacheGibibytes:
            if (seen_proxy_cache_gibibytes ||
                !decode_u32(value, decoded.proxy_cache_gibibytes)) {
                return false;
            }
            seen_proxy_cache_gibibytes = true;
            break;
        case PreferenceKey::OutputBufferFrames:
            if (seen_output_buffer_frames ||
                !decode_u32(value, decoded.output_buffer_frames)) {
                return false;
            }
            seen_output_buffer_frames = true;
            break;
        case PreferenceKey::OutputReadAheadFrames:
            if (seen_output_read_ahead_frames ||
                !decode_u32(value, decoded.output_read_ahead_frames)) {
                return false;
            }
            seen_output_read_ahead_frames = true;
            break;
        case PreferenceKey::PlaybackCacheMebibytes:
            if (seen_playback_cache_mebibytes ||
                !decode_u32(value, decoded.playback_cache_mebibytes)) {
                return false;
            }
            seen_playback_cache_mebibytes = true;
            break;
        case PreferenceKey::PlaybackTrailFrames:
            if (seen_playback_trail_frames ||
                !decode_u32(value, decoded.playback_trail_frames)) {
                return false;
            }
            seen_playback_trail_frames = true;
            break;
        case PreferenceKey::ProxyTranscodePriority: {
            std::uint32_t priority{};
            if (seen_proxy_transcode_priority || !decode_u32(value, priority) ||
                priority >
                    static_cast<std::uint32_t>(ProxyTranscodePriority::Fast)) {
                return false;
            }
            seen_proxy_transcode_priority = true;
            decoded.proxy_transcode_priority =
                static_cast<ProxyTranscodePriority>(priority);
            break;
        }
        case PreferenceKey::RenderMachineDirector:
            if (seen_render_director) {
                return false;
            }
            seen_render_director = true;
            decoded.render_machine_director = utf8_from_bytes(value);
            break;
        case PreferenceKey::RenderMachineNodeIndex:
            if (seen_render_node_index ||
                !decode_u32(value, decoded.render_machine_node_index)) {
                return false;
            }
            seen_render_node_index = true;
            break;
        case PreferenceKey::RenderMachineControlPort:
            if (seen_render_control_port ||
                !decode_u32(value, decoded.render_machine_control_port)) {
                return false;
            }
            seen_render_control_port = true;
            break;
        case PreferenceKey::RenderMachineShowId:
            if (seen_render_show_id) {
                return false;
            }
            seen_render_show_id = true;
            decoded.render_machine_show_id = utf8_from_bytes(value);
            break;
        default:
            // Written by a newer build. Skipping keeps this one usable.
            break;
        }
    }
    if (!decoded.valid_tuning() || !decoded.valid_render_machine()) {
        return false;
    }
    out = std::move(decoded);
    return true;
}

[[nodiscard]] PreferencesSaveResult
save_preferences_impl(const std::filesystem::path &preferences_file,
                      const Preferences &preferences) {
    if (!valid_target_path(preferences_file)) {
        return preferences_save_failure(
            make_error(SettingsErrorCode::InvalidSettingsPath));
    }
    if (!preferences.valid_tuning() || !preferences.valid_render_machine()) {
        return preferences_save_failure(
            make_error(SettingsErrorCode::InvalidFormat));
    }
    for (const std::string_view value :
         {std::string_view{preferences.director_listen},
          std::string_view{preferences.show_id},
          std::string_view{preferences.director_epoch},
          std::string_view{preferences.lifecycle_password_key},
          std::string_view{preferences.wal_path},
          std::string_view{preferences.proxy_cache_directory},
          std::string_view{preferences.render_machine_director},
          std::string_view{preferences.render_machine_show_id}}) {
        if (value.size() > max_preference_value_bytes) {
            return preferences_save_failure(
                make_error(SettingsErrorCode::SizeLimitExceeded));
        }
    }

    // Each value is text; the payload framing around them is not, so UTF-8 is
    // checked per value rather than over the encoded bytes.
    for (const std::string_view value :
         {std::string_view{preferences.director_listen},
          std::string_view{preferences.show_id},
          std::string_view{preferences.director_epoch},
          std::string_view{preferences.lifecycle_password_key},
          std::string_view{preferences.wal_path},
          std::string_view{preferences.proxy_cache_directory},
          std::string_view{preferences.render_machine_director},
          std::string_view{preferences.render_machine_show_id}}) {
        if (value.empty()) {
            continue;
        }
        const auto bytes = std::span<const std::byte>{
            reinterpret_cast<const std::byte *>(value.data()), value.size()};
        if (!valid_utf8(bytes)) {
            return preferences_save_failure(
                make_error(SettingsErrorCode::InvalidFormat));
        }
    }

    std::vector<std::byte> payload;
    put_preference(payload, PreferenceKey::DirectorListen,
                   preferences.director_listen);
    put_preference(payload, PreferenceKey::ShowId, preferences.show_id);
    put_preference(payload, PreferenceKey::DirectorEpoch,
                   preferences.director_epoch);
    put_preference(payload, PreferenceKey::LifecyclePasswordKey,
                   preferences.lifecycle_password_key);
    put_preference(payload, PreferenceKey::Wal, preferences.wal_path);
    put_preference(payload, PreferenceKey::ProxyCacheDirectory,
                   preferences.proxy_cache_directory);
    if (preferences.proxy_cache_next_to_source) {
        put_preference(payload, PreferenceKey::ProxyCacheNextToSource, "1");
    }
    if (preferences.proxy_cache_gibibytes != default_proxy_cache_gibibytes) {
        put_preference(payload, PreferenceKey::ProxyCacheGibibytes,
                       preferences.proxy_cache_gibibytes);
    }
    if (preferences.output_buffer_frames != default_output_buffer_frames) {
        put_preference(payload, PreferenceKey::OutputBufferFrames,
                       preferences.output_buffer_frames);
    }
    if (preferences.output_read_ahead_frames !=
        default_output_read_ahead_frames) {
        put_preference(payload, PreferenceKey::OutputReadAheadFrames,
                       preferences.output_read_ahead_frames);
    }
    if (preferences.playback_cache_mebibytes !=
        default_playback_cache_mebibytes) {
        put_preference(payload, PreferenceKey::PlaybackCacheMebibytes,
                       preferences.playback_cache_mebibytes);
    }
    if (preferences.playback_trail_frames != default_playback_trail_frames) {
        put_preference(payload, PreferenceKey::PlaybackTrailFrames,
                       preferences.playback_trail_frames);
    }
    if (preferences.proxy_transcode_priority !=
        ProxyTranscodePriority::Background) {
        put_preference(
            payload, PreferenceKey::ProxyTranscodePriority,
            static_cast<std::uint32_t>(preferences.proxy_transcode_priority));
    }
    put_preference(payload, PreferenceKey::RenderMachineDirector,
                   preferences.render_machine_director);
    if (preferences.render_machine_node_index != 0U) {
        put_preference(payload, PreferenceKey::RenderMachineNodeIndex,
                       preferences.render_machine_node_index);
    }
    if (preferences.render_machine_control_port != 0U) {
        put_preference(payload, PreferenceKey::RenderMachineControlPort,
                       preferences.render_machine_control_port);
    }
    put_preference(payload, PreferenceKey::RenderMachineShowId,
                   preferences.render_machine_show_id);
    auto bytes =
        make_setting_bytes(std::span<const std::byte, 8U>{kPreferencesMagic},
                           current_preferences_format_version, payload);

    auto parent = preferences_file.parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    std::error_code directory_error;
    static_cast<void>(
        std::filesystem::create_directories(parent, directory_error));
    if (directory_error) {
        return preferences_save_failure(make_error(
            SettingsErrorCode::DirectoryCreateFailed, directory_error));
    }

    auto temporary_result = create_temporary_file(preferences_file);
    if (!temporary_result) {
        return preferences_save_failure(std::move(temporary_result.error()));
    }
    auto temporary = std::move(temporary_result).value();
    auto written = write_sync_and_close(temporary, bytes);
    if (!written) {
        return preferences_save_failure(std::move(written.error()));
    }
    const auto replace_error = replace_file(temporary.path(), preferences_file);
    if (replace_error) {
        return preferences_save_failure(
            make_error(SettingsErrorCode::ReplaceFailed, replace_error));
    }
    temporary.mark_replaced();

    const auto parent_sync_error = sync_parent_directory(parent);
    if (parent_sync_error) {
        return preferences_save_failure(
            make_error(SettingsErrorCode::SyncFailed, parent_sync_error, true));
    }
    return PreferencesSaveResult::success(preferences_file);
}

[[nodiscard]] PreferencesLoadResult
load_preferences_impl(const std::filesystem::path &preferences_file) {
    if (!valid_target_path(preferences_file)) {
        return preferences_load_failure(
            make_error(SettingsErrorCode::InvalidSettingsPath));
    }

    errno = 0;
    std::ifstream input(preferences_file, std::ios::binary | std::ios::ate);
    if (!input) {
        std::error_code exists_error;
        const bool exists =
            std::filesystem::exists(preferences_file, exists_error);
        if (!exists && !exists_error) {
            // Never configured. That is the ordinary first-run state.
            return PreferencesLoadResult::success(Preferences{});
        }
        return preferences_load_failure(
            make_error(SettingsErrorCode::OpenFailed,
                       exists_error ? exists_error : stream_io_error()));
    }
    const auto end = input.tellg();
    if (end < 0) {
        return preferences_load_failure(
            make_error(SettingsErrorCode::ReadFailed, stream_io_error()));
    }
    constexpr std::uint64_t maximum_payload = kMaximumPreferencesPayload;
    const auto size = static_cast<std::uint64_t>(end);
    if (size < kHeaderSize || size > kHeaderSize + maximum_payload) {
        return preferences_load_failure(
            make_error(size > kHeaderSize + maximum_payload
                           ? SettingsErrorCode::SizeLimitExceeded
                           : SettingsErrorCode::CorruptFile));
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    if (!input) {
        return preferences_load_failure(
            make_error(SettingsErrorCode::ReadFailed, stream_io_error()));
    }
    input.read(reinterpret_cast<char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input ||
        input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return preferences_load_failure(
            make_error(SettingsErrorCode::ReadFailed, stream_io_error()));
    }

    const auto view = std::span<const std::byte>{bytes};
    if (!std::equal(kPreferencesMagic.begin(), kPreferencesMagic.end(),
                    view.begin())) {
        return preferences_load_failure(
            make_error(SettingsErrorCode::InvalidFormat));
    }
    if (get_u16(view, 8U) != current_preferences_format_version) {
        return preferences_load_failure(
            make_error(SettingsErrorCode::UnsupportedVersion));
    }
    if (get_u16(view, 10U) != kHeaderSize ||
        get_u32(view, kHeaderCrcOffset) !=
            crc32c(view.first(kHeaderCrcOffset))) {
        return preferences_load_failure(
            make_error(SettingsErrorCode::CorruptFile));
    }
    const auto payload_size = get_u32(view, 12U);
    if (payload_size > maximum_payload ||
        static_cast<std::uint64_t>(payload_size) + kHeaderSize != size) {
        return preferences_load_failure(
            make_error(payload_size > maximum_payload
                           ? SettingsErrorCode::SizeLimitExceeded
                           : SettingsErrorCode::CorruptFile));
    }
    const auto payload = view.subspan(kHeaderSize, payload_size);
    if (get_u32(view, 16U) != crc32c(payload)) {
        return preferences_load_failure(
            make_error(SettingsErrorCode::CorruptFile));
    }
    Preferences decoded{};
    if (!decode_preferences(payload, decoded)) {
        return preferences_load_failure(
            make_error(SettingsErrorCode::CorruptFile));
    }
    return PreferencesLoadResult::success(std::move(decoded));
}

[[nodiscard]] WorkspaceLayoutSaveResult
save_workspace_layout_impl(const std::filesystem::path &layout_file,
                           std::span<const std::byte> layout) {
    if (!valid_target_path(layout_file)) {
        return WorkspaceLayoutSaveResult::failure(
            make_error(SettingsErrorCode::InvalidSettingsPath));
    }
    if (layout.size() > max_workspace_layout_setting_bytes) {
        return WorkspaceLayoutSaveResult::failure(
            make_error(SettingsErrorCode::SizeLimitExceeded));
    }
    auto bytes = make_setting_bytes(
        std::span<const std::byte, 8U>{kWorkspaceLayoutMagic},
        current_workspace_layout_format_version, layout);

    auto parent = layout_file.parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    std::error_code directory_error;
    static_cast<void>(
        std::filesystem::create_directories(parent, directory_error));
    if (directory_error) {
        return WorkspaceLayoutSaveResult::failure(make_error(
            SettingsErrorCode::DirectoryCreateFailed, directory_error));
    }

    auto temporary_result = create_temporary_file(layout_file);
    if (!temporary_result) {
        return WorkspaceLayoutSaveResult::failure(
            std::move(temporary_result.error()));
    }
    auto temporary = std::move(temporary_result).value();
    auto written = write_sync_and_close(temporary, bytes);
    if (!written) {
        return WorkspaceLayoutSaveResult::failure(std::move(written.error()));
    }
    const auto replace_error = replace_file(temporary.path(), layout_file);
    if (replace_error) {
        return WorkspaceLayoutSaveResult::failure(
            make_error(SettingsErrorCode::ReplaceFailed, replace_error));
    }
    temporary.mark_replaced();

    const auto parent_sync_error = sync_parent_directory(parent);
    if (parent_sync_error) {
        return WorkspaceLayoutSaveResult::failure(
            make_error(SettingsErrorCode::SyncFailed, parent_sync_error, true));
    }
    return WorkspaceLayoutSaveResult::success(layout_file);
}

[[nodiscard]] WorkspaceLayoutLoadResult
load_workspace_layout_impl(const std::filesystem::path &layout_file) {
    if (!valid_target_path(layout_file)) {
        return WorkspaceLayoutLoadResult::failure(
            make_error(SettingsErrorCode::InvalidSettingsPath));
    }

    errno = 0;
    std::ifstream input(layout_file, std::ios::binary | std::ios::ate);
    if (!input) {
        std::error_code exists_error;
        const bool exists = std::filesystem::exists(layout_file, exists_error);
        if (!exists && !exists_error) {
            return WorkspaceLayoutLoadResult::success({});
        }
        return WorkspaceLayoutLoadResult::failure(
            make_error(SettingsErrorCode::OpenFailed,
                       exists_error ? exists_error : stream_io_error()));
    }
    const auto end = input.tellg();
    if (end < 0) {
        return WorkspaceLayoutLoadResult::failure(
            make_error(SettingsErrorCode::ReadFailed, stream_io_error()));
    }
    const auto size = static_cast<std::uint64_t>(end);
    if (size < kHeaderSize ||
        size > kHeaderSize + max_workspace_layout_setting_bytes) {
        return WorkspaceLayoutLoadResult::failure(
            make_error(size > kHeaderSize + max_workspace_layout_setting_bytes
                           ? SettingsErrorCode::SizeLimitExceeded
                           : SettingsErrorCode::CorruptFile));
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    if (!input) {
        return WorkspaceLayoutLoadResult::failure(
            make_error(SettingsErrorCode::ReadFailed, stream_io_error()));
    }
    input.read(reinterpret_cast<char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input ||
        input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return WorkspaceLayoutLoadResult::failure(
            make_error(SettingsErrorCode::ReadFailed, stream_io_error()));
    }

    const auto view = std::span<const std::byte>{bytes};
    if (!std::equal(kWorkspaceLayoutMagic.begin(), kWorkspaceLayoutMagic.end(),
                    view.begin())) {
        return WorkspaceLayoutLoadResult::failure(
            make_error(SettingsErrorCode::InvalidFormat));
    }
    if (get_u16(view, 8U) != current_workspace_layout_format_version) {
        return WorkspaceLayoutLoadResult::failure(
            make_error(SettingsErrorCode::UnsupportedVersion));
    }
    if (get_u16(view, 10U) != kHeaderSize ||
        get_u32(view, kHeaderCrcOffset) !=
            crc32c(view.first(kHeaderCrcOffset))) {
        return WorkspaceLayoutLoadResult::failure(
            make_error(SettingsErrorCode::CorruptFile));
    }
    const auto payload_size = get_u32(view, 12U);
    if (payload_size > max_workspace_layout_setting_bytes ||
        static_cast<std::uint64_t>(payload_size) + kHeaderSize != size) {
        return WorkspaceLayoutLoadResult::failure(
            make_error(payload_size > max_workspace_layout_setting_bytes
                           ? SettingsErrorCode::SizeLimitExceeded
                           : SettingsErrorCode::CorruptFile));
    }
    const auto payload = view.subspan(kHeaderSize, payload_size);
    if (get_u32(view, 16U) != crc32c(payload)) {
        return WorkspaceLayoutLoadResult::failure(
            make_error(SettingsErrorCode::CorruptFile));
    }
    return WorkspaceLayoutLoadResult::success(
        WorkspaceLayoutBytes{payload.begin(), payload.end()});
}

#ifdef _WIN32
[[nodiscard]] std::optional<std::filesystem::path>
environment_path(const wchar_t *name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0U);
    if (required == 0U) {
        return std::nullopt;
    }
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
    if (written == 0U || written >= required) {
        return std::nullopt;
    }
    value.resize(written);
    if (value.empty()) {
        return std::nullopt;
    }
    return std::filesystem::path{std::move(value)};
}
#else
[[nodiscard]] std::optional<std::filesystem::path>
environment_path(const char *name) {
    const char *const value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::filesystem::path{value};
}
#endif

[[nodiscard]] SettingsPathResult settings_file_named(const char *file_name) {
    try {
#ifdef _WIN32
        auto root = environment_path(L"LOCALAPPDATA");
        if (!root.has_value()) {
            root = environment_path(L"APPDATA");
        }
#else
        auto root = environment_path("XDG_CONFIG_HOME");
        if (!root.has_value()) {
            const auto home = environment_path("HOME");
            if (home.has_value()) {
                root = *home / ".config";
            }
        }
#endif
        if (!root.has_value() || root->empty()) {
            return settings_path_failure(
                make_error(SettingsErrorCode::SettingsUnavailable));
        }
        auto file =
            (*root / "MediaServer" / "settings" / file_name).lexically_normal();
        if (!file.is_absolute()) {
            return settings_path_failure(
                make_error(SettingsErrorCode::SettingsUnavailable));
        }
        return SettingsPathResult::success(std::move(file));
    } catch (const std::bad_alloc &) {
        return settings_path_failure(
            make_error(SettingsErrorCode::AllocationFailed));
    } catch (const std::length_error &) {
        return settings_path_failure(
            make_error(SettingsErrorCode::SizeLimitExceeded));
    } catch (const std::filesystem::filesystem_error &error) {
        return settings_path_failure(
            make_error(SettingsErrorCode::SettingsUnavailable, error.code()));
    }
}

} // namespace

SettingsPathResult default_recent_project_settings_file() {
    return settings_file_named("recent-project.bin");
}

SettingsPathResult default_preferences_file() {
    return settings_file_named("preferences.bin");
}

SettingsPathResult default_workspace_layout_file() {
    return settings_file_named("workspace-layout.bin");
}

PreferencesLoadResult
load_preferences(const std::filesystem::path &preferences_file) {
    try {
        return load_preferences_impl(preferences_file);
    } catch (const std::bad_alloc &) {
        return preferences_load_failure(
            make_error(SettingsErrorCode::AllocationFailed));
    } catch (const std::length_error &) {
        return preferences_load_failure(
            make_error(SettingsErrorCode::SizeLimitExceeded));
    } catch (const std::filesystem::filesystem_error &error) {
        return preferences_load_failure(
            make_error(SettingsErrorCode::ReadFailed, error.code()));
    }
}

PreferencesSaveResult
save_preferences(const std::filesystem::path &preferences_file,
                 const Preferences &preferences) {
    try {
        return save_preferences_impl(preferences_file, preferences);
    } catch (const std::bad_alloc &) {
        return preferences_save_failure(
            make_error(SettingsErrorCode::AllocationFailed));
    } catch (const std::length_error &) {
        return preferences_save_failure(
            make_error(SettingsErrorCode::SizeLimitExceeded));
    } catch (const std::filesystem::filesystem_error &error) {
        return preferences_save_failure(
            make_error(SettingsErrorCode::WriteFailed, error.code()));
    }
}

WorkspaceLayoutLoadResult
load_workspace_layout(const std::filesystem::path &layout_file) {
    try {
        return load_workspace_layout_impl(layout_file);
    } catch (const std::bad_alloc &) {
        return WorkspaceLayoutLoadResult::failure(
            make_error(SettingsErrorCode::AllocationFailed));
    } catch (const std::length_error &) {
        return WorkspaceLayoutLoadResult::failure(
            make_error(SettingsErrorCode::SizeLimitExceeded));
    } catch (const std::filesystem::filesystem_error &error) {
        return WorkspaceLayoutLoadResult::failure(
            make_error(SettingsErrorCode::ReadFailed, error.code()));
    }
}

WorkspaceLayoutSaveResult
save_workspace_layout(const std::filesystem::path &layout_file,
                      std::span<const std::byte> layout) {
    try {
        return save_workspace_layout_impl(layout_file, layout);
    } catch (const std::bad_alloc &) {
        return WorkspaceLayoutSaveResult::failure(
            make_error(SettingsErrorCode::AllocationFailed));
    } catch (const std::length_error &) {
        return WorkspaceLayoutSaveResult::failure(
            make_error(SettingsErrorCode::SizeLimitExceeded));
    } catch (const std::filesystem::filesystem_error &error) {
        return WorkspaceLayoutSaveResult::failure(
            make_error(SettingsErrorCode::WriteFailed, error.code()));
    }
}

RecentProjectPathLoadResult
load_recent_project_path(const std::filesystem::path &settings_file) {
    try {
        return load_recent_project_path_impl(settings_file);
    } catch (const std::bad_alloc &) {
        return load_failure(make_error(SettingsErrorCode::AllocationFailed));
    } catch (const std::length_error &) {
        return load_failure(make_error(SettingsErrorCode::SizeLimitExceeded));
    } catch (const std::filesystem::filesystem_error &error) {
        return load_failure(
            make_error(SettingsErrorCode::ReadFailed, error.code()));
    }
}

RecentProjectPathSaveResult
save_recent_project_path(const std::filesystem::path &settings_file,
                         const std::filesystem::path &project_file) {
    try {
        return save_recent_project_path_impl(settings_file, project_file);
    } catch (const std::bad_alloc &) {
        return save_failure(make_error(SettingsErrorCode::AllocationFailed));
    } catch (const std::length_error &) {
        return save_failure(make_error(SettingsErrorCode::SizeLimitExceeded));
    } catch (const std::filesystem::filesystem_error &error) {
        return save_failure(
            make_error(SettingsErrorCode::WriteFailed, error.code()));
    }
}

RecentProjectOpenResult
open_recent_project(const std::filesystem::path &settings_file,
                    const wire::DecodeLimits &limits) {
    auto configured = load_recent_project_path(settings_file);
    if (!configured) {
        return RecentProjectOpenResult::failure(
            RecentProjectOpenError{configured.error(), std::nullopt});
    }
    if (!configured.value().has_value()) {
        return RecentProjectOpenResult::success(std::nullopt);
    }
    auto project = load_project_file(*configured.value(), limits);
    if (!project) {
        return RecentProjectOpenResult::failure(
            RecentProjectOpenError{std::nullopt, project.error()});
    }
    return RecentProjectOpenResult::success(RecentProject{
        std::move(*configured.value()), std::move(project).value()});
}

} // namespace mediaserver::persist
