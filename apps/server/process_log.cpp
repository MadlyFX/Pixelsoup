#include "process_log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <mutex>
#include <new>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mediaserver::app {
namespace {

[[nodiscard]] unsigned long current_process_id() noexcept {
#if defined(_WIN32)
    return static_cast<unsigned long>(::GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

#if defined(_WIN32)
[[nodiscard]] std::optional<std::filesystem::path>
environment_path(const wchar_t* name) {
    const DWORD required = ::GetEnvironmentVariableW(name, nullptr, 0U);
    if (required == 0U) {
        return std::nullopt;
    }
    std::wstring value(required, L'\0');
    const DWORD written =
        ::GetEnvironmentVariableW(name, value.data(), required);
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
environment_path(const char* name) {
    const char* const value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::filesystem::path{value};
}
#endif

// An append-only log file that other processes may read, rename and delete
// while it is open, but not write: a second writer is how a reused name is
// detected.
class LogFile final {
public:
    LogFile() = default;
    LogFile(const LogFile&) = delete;
    LogFile& operator=(const LogFile&) = delete;
    ~LogFile() { close(); }

    enum class Opened : core::u8 { Ok, Busy, Failed };

    [[nodiscard]] Opened open(const std::filesystem::path& path) noexcept {
        close();
#if defined(_WIN32)
        handle_ = ::CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            return ::GetLastError() == ERROR_SHARING_VIOLATION
                ? Opened::Busy
                : Opened::Failed;
        }
        LARGE_INTEGER size{};
        size_ = ::GetFileSizeEx(handle_, &size) != 0
            ? static_cast<std::uint64_t>(size.QuadPart)
            : 0U;
#else
        descriptor_ = ::open(path.c_str(),
                             O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (descriptor_ < 0) {
            return Opened::Failed;
        }
        if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            const bool busy = errno == EWOULDBLOCK;
            close();
            return busy ? Opened::Busy : Opened::Failed;
        }
        struct stat status{};
        size_ = ::fstat(descriptor_, &status) == 0
            ? static_cast<std::uint64_t>(status.st_size)
            : 0U;
#endif
        return Opened::Ok;
    }

    [[nodiscard]] bool write(std::string_view bytes) noexcept {
        std::size_t offset{};
        while (offset < bytes.size()) {
#if defined(_WIN32)
            if (handle_ == INVALID_HANDLE_VALUE) {
                return false;
            }
            const DWORD request = static_cast<DWORD>(
                std::min<std::size_t>(bytes.size() - offset, 1U << 20U));
            DWORD written{};
            if (::WriteFile(handle_, bytes.data() + offset, request, &written,
                            nullptr) == 0 ||
                written == 0U) {
                return false;
            }
#else
            if (descriptor_ < 0) {
                return false;
            }
            const ssize_t written = ::write(descriptor_, bytes.data() + offset,
                                            bytes.size() - offset);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                return false;
            }
#endif
            offset += static_cast<std::size_t>(written);
            size_ += static_cast<std::uint64_t>(written);
        }
        return true;
    }

    void close() noexcept {
#if defined(_WIN32)
        if (handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (descriptor_ >= 0) {
            ::close(descriptor_);
            descriptor_ = -1;
        }
#endif
        size_ = 0U;
    }

    [[nodiscard]] bool is_open() const noexcept {
#if defined(_WIN32)
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return descriptor_ >= 0;
#endif
    }

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }

private:
#if defined(_WIN32)
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    int descriptor_{-1};
#endif
    std::uint64_t size_{};
};

// Reads [offset, offset + length) of a file the writer may hold open,
// rename or delete meanwhile. Returns nullopt when the file does not exist.
[[nodiscard]] std::optional<std::string>
read_shared(const std::filesystem::path& path, std::size_t window,
            bool from_end, bool& whole_file) {
    whole_file = false;
#if defined(_WIN32)
    const HANDLE handle = ::CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(handle, &size) == 0) {
        ::CloseHandle(handle);
        return std::string{};
    }
    const auto total = static_cast<std::uint64_t>(size.QuadPart);
    const std::uint64_t length = std::min<std::uint64_t>(total, window);
    const std::uint64_t offset = from_end ? total - length : 0U;
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    std::string bytes(static_cast<std::size_t>(length), '\0');
    DWORD read{};
    if (::SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) == 0 ||
        (length > 0U &&
         ::ReadFile(handle, bytes.data(), static_cast<DWORD>(length), &read,
                    nullptr) == 0)) {
        read = 0U;
    }
    ::CloseHandle(handle);
    bytes.resize(read);
    whole_file = offset == 0U && length == total;
    return bytes;
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return std::nullopt;
    }
    struct stat status{};
    if (::fstat(descriptor, &status) != 0) {
        ::close(descriptor);
        return std::string{};
    }
    const auto total = static_cast<std::uint64_t>(status.st_size);
    const std::uint64_t length = std::min<std::uint64_t>(total, window);
    const std::uint64_t offset = from_end ? total - length : 0U;
    std::string bytes(static_cast<std::size_t>(length), '\0');
    std::size_t filled{};
    while (filled < bytes.size()) {
        const ssize_t read =
            ::pread(descriptor, bytes.data() + filled, bytes.size() - filled,
                    static_cast<off_t>(offset + filled));
        if (read < 0 && errno == EINTR) {
            continue;
        }
        if (read <= 0) {
            break;
        }
        filled += static_cast<std::size_t>(read);
    }
    ::close(descriptor);
    bytes.resize(filled);
    whole_file = offset == 0U && length == total;
    return bytes;
#endif
}

// Splits on '\n', dropping '\r'. A window that starts mid-file drops its
// first, partial line.
[[nodiscard]] std::vector<std::string> split_lines(std::string_view bytes,
                                                   bool starts_at_line) {
    std::vector<std::string> lines;
    std::size_t start{};
    if (!starts_at_line) {
        const std::size_t first = bytes.find('\n');
        start = first == std::string_view::npos ? bytes.size() : first + 1U;
    }
    while (start < bytes.size()) {
        std::size_t end = bytes.find('\n', start);
        if (end == std::string_view::npos) {
            end = bytes.size();
        }
        std::string_view line = bytes.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1U);
        }
        lines.emplace_back(line);
        start = end + 1U;
    }
    return lines;
}

[[nodiscard]] std::string timestamp_text(
    std::chrono::system_clock::time_point time) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            time.time_since_epoch())
            .count() %
        1000;
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    std::array<char, 40U> text{};
    const std::size_t length =
        std::strftime(text.data(), text.size(), "%Y-%m-%d %H:%M:%S", &local);
    const int suffix =
        std::snprintf(text.data() + length, text.size() - length, ".%03d ",
                      static_cast<int>(milliseconds < 0 ? 0 : milliseconds));
    return std::string{text.data(),
                       length + static_cast<std::size_t>(std::max(suffix, 0))};
}

} // namespace

bool valid_process_log_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 64U || name.front() == '.') {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' ||
               character == '_' || character == '.';
    });
}

std::filesystem::path process_log_file(const std::filesystem::path& directory,
                                       std::string_view name,
                                       std::size_t generation) {
    std::string file{name};
    if (generation != 0U) {
        file += '.';
        file += std::to_string(generation);
    }
    file += ".log";
    return directory / file;
}

std::filesystem::path default_log_directory() {
    try {
#if defined(_WIN32)
        auto root = environment_path(L"LOCALAPPDATA");
        if (!root.has_value()) {
            root = environment_path(L"APPDATA");
        }
        if (!root.has_value()) {
            return {};
        }
        return (*root / "MediaServer" / "logs").lexically_normal();
#else
        auto root = environment_path("XDG_STATE_HOME");
        if (!root.has_value()) {
            const auto home = environment_path("HOME");
            if (!home.has_value()) {
                return {};
            }
            root = *home / ".local" / "state";
        }
        return (*root / "mediaserver" / "logs").lexically_normal();
#endif
    } catch (...) {
        return {};
    }
}

std::string node_process_log_name(core::u32 node_index) {
    return "node-" + std::to_string(node_index);
}

std::string agent_process_log_name(core::u32 node_index) {
    return "agent-" + std::to_string(node_index);
}

namespace detail {

class ProcessLogState final {
public:
    static constexpr std::size_t source_count = 3U;
    struct Chunk final {
        std::chrono::system_clock::time_point time;
        std::string bytes;
        core::u8 source{};
    };
    struct Partial final {
        std::string text;
        std::chrono::system_clock::time_point time{};
    };

    ProcessLogConfig config;
    std::filesystem::path path;
    std::string name;
    LogFile file;

    mutable std::mutex mutex;
    std::condition_variable changed;
    std::condition_variable drained;
    std::deque<Chunk> queue;
    std::size_t queued_bytes{};
    bool writing{};
    bool stopping{};
    core::u64 dropped_bytes{};
    core::u64 written_lines{};
    core::u64 rotations{};
    core::u64 write_failures{};
    std::thread writer;

    // Writer-thread state.
    std::array<Partial, source_count> partials{};
    core::u64 reported_dropped{};

    void rotate_files() noexcept {
        std::error_code error;
        const std::size_t backups = std::max<std::size_t>(config.backups, 1U);
        std::filesystem::remove(process_log_file(config.directory, name,
                                                 backups),
                                error);
        for (std::size_t generation = backups; generation > 0U;
             --generation) {
            const auto from =
                process_log_file(config.directory, name, generation - 1U);
            const auto to =
                process_log_file(config.directory, name, generation);
            std::filesystem::rename(from, to, error);
        }
    }

    // Writer thread only.
    void append(std::string& out, std::chrono::system_clock::time_point time,
                std::string_view line) {
        out += timestamp_text(time);
        out.append(line);
        out.push_back('\n');
        ++written_lines;
    }

    void write_out(const std::string& out) {
        if (out.empty()) {
            return;
        }
        if (file.is_open() && file.size() > 0U &&
            file.size() + out.size() > config.max_file_bytes) {
            file.close();
            rotate_files();
            ++rotations;
            if (file.open(path) != LogFile::Opened::Ok) {
                ++write_failures;
            }
        }
        if (!file.is_open() && file.open(path) != LogFile::Opened::Ok) {
            ++write_failures;
            return;
        }
        if (!file.write(out)) {
            ++write_failures;
        }
    }

    void run() noexcept {
        std::deque<Chunk> batch;
        for (;;) {
            core::u64 dropped{};
            bool stop{};
            {
                std::unique_lock lock{mutex};
                changed.wait(lock, [&] { return stopping || !queue.empty(); });
                batch.swap(queue);
                queued_bytes = 0U;
                writing = true;
                dropped = dropped_bytes;
                stop = stopping;
            }
            try {
                std::string out;
                for (const Chunk& chunk : batch) {
                    Partial& partial = partials[std::min<std::size_t>(
                        chunk.source, source_count - 1U)];
                    std::string_view rest{chunk.bytes};
                    while (!rest.empty()) {
                        const std::size_t end = rest.find('\n');
                        if (partial.text.empty()) {
                            partial.time = chunk.time;
                        }
                        if (end == std::string_view::npos) {
                            partial.text.append(rest);
                            break;
                        }
                        partial.text.append(rest.substr(0U, end));
                        rest.remove_prefix(end + 1U);
                        if (!partial.text.empty() &&
                            partial.text.back() == '\r') {
                            partial.text.pop_back();
                        }
                        append(out, partial.time, partial.text);
                        partial.text.clear();
                    }
                    // A line that never ends is written in pieces rather
                    // than held without bound.
                    if (partial.text.size() >= 4096U) {
                        append(out, partial.time, partial.text);
                        partial.text.clear();
                    }
                }
                if (stop) {
                    for (Partial& partial : partials) {
                        if (!partial.text.empty()) {
                            append(out, partial.time, partial.text);
                            partial.text.clear();
                        }
                    }
                }
                if (dropped != reported_dropped) {
                    append(out, std::chrono::system_clock::now(),
                           std::string{process_log_dropped_marker} +
                               std::to_string(dropped - reported_dropped) +
                               " bytes: the disk writer fell behind");
                    reported_dropped = dropped;
                }
                write_out(out);
            } catch (...) {
                ++write_failures;
            }
            batch.clear();
            {
                const std::lock_guard lock{mutex};
                writing = false;
                if (queue.empty()) {
                    drained.notify_all();
                }
                if (stop && queue.empty()) {
                    return;
                }
            }
        }
    }
};

} // namespace detail

core::Result<std::unique_ptr<ProcessLog>, std::string>
ProcessLog::open(ProcessLogConfig config) {
    using Result = core::Result<std::unique_ptr<ProcessLog>, std::string>;
    if (config.directory.empty()) {
        return Result::failure("no log folder");
    }
    if (!valid_process_log_name(config.name)) {
        return Result::failure("invalid log name");
    }
    if (config.max_file_bytes < 4096U || config.queue_bytes < 4096U) {
        return Result::failure("log limits too small");
    }
    try {
        auto state = std::make_unique<detail::ProcessLogState>();
        state->config = std::move(config);
        std::error_code error;
        std::filesystem::create_directories(state->config.directory, error);
        if (error) {
            return Result::failure("could not create " +
                                   state->config.directory.string() + ": " +
                                   error.message());
        }
        state->name = state->config.name;
        state->path = process_log_file(state->config.directory, state->name);
        LogFile::Opened opened = state->file.open(state->path);
        if (opened == LogFile::Opened::Busy) {
            // Another live process owns the name - two Directors on one
            // machine. Neither run's file is disturbed.
            state->name += "-" + std::to_string(current_process_id());
            state->path =
                process_log_file(state->config.directory, state->name);
            opened = state->file.open(state->path);
        }
        if (opened != LogFile::Opened::Ok) {
            return Result::failure("could not open " + state->path.string());
        }
        if (state->file.size() > 0U) {
            state->file.close();
            state->rotate_files();
            if (state->file.open(state->path) != LogFile::Opened::Ok) {
                return Result::failure("could not open " +
                                       state->path.string());
            }
        }
        std::string header;
        state->append(header, std::chrono::system_clock::now(),
                      std::string{process_log_run_marker} + "; pid " +
                          std::to_string(current_process_id()) +
                          (state->config.header.empty() ? "" : "; ") +
                          state->config.header);
        if (!state->file.write(header)) {
            return Result::failure("could not write " + state->path.string());
        }
        detail::ProcessLogState* raw = state.get();
        state->writer = std::thread{[raw] { raw->run(); }};
        return Result::success(
            std::unique_ptr<ProcessLog>{new ProcessLog{std::move(state)}});
    } catch (const std::bad_alloc&) {
        return Result::failure("allocation failed");
    } catch (const std::system_error& error) {
        return Result::failure(error.what());
    }
}

ProcessLog::ProcessLog(std::unique_ptr<detail::ProcessLogState> state)
    : state_(std::move(state)) {}

ProcessLog::~ProcessLog() {
    {
        const std::lock_guard lock{state_->mutex};
        state_->stopping = true;
    }
    state_->changed.notify_all();
    if (state_->writer.joinable()) {
        state_->writer.join();
    }
}

void ProcessLog::write(std::string_view text, core::u8 source) noexcept {
    if (text.empty()) {
        return;
    }
    const auto now = std::chrono::system_clock::now();
    detail::ProcessLogState& state = *state_;
    {
        const std::lock_guard lock{state.mutex};
        if (state.stopping ||
            state.queued_bytes + text.size() > state.config.queue_bytes) {
            state.dropped_bytes += text.size();
            return;
        }
        try {
            state.queue.push_back({now, std::string{text}, source});
        } catch (...) {
            state.dropped_bytes += text.size();
            return;
        }
        state.queued_bytes += text.size();
    }
    state.changed.notify_one();
}

void ProcessLog::flush() noexcept {
    std::unique_lock lock{state_->mutex};
    state_->drained.wait(lock, [&] {
        return state_->queue.empty() && !state_->writing;
    });
}

const std::filesystem::path& ProcessLog::file() const noexcept {
    return state_->path;
}

std::string_view ProcessLog::name() const noexcept { return state_->name; }

ProcessLogCounters ProcessLog::counters() const noexcept {
    const std::lock_guard lock{state_->mutex};
    return ProcessLogCounters{state_->written_lines, state_->dropped_bytes,
                              state_->rotations, state_->write_failures};
}

// ---------------------------------------------------------------- capture

namespace detail {

class StandardStreamCaptureState final {
public:
    struct Stream final {
        int target{-1};   // 1 or 2
        int original{-1}; // the console it wrote to, or -1
        int read{-1};
        std::thread reader;
    };
    struct ConsoleChunk final {
        int fd{};
        std::string bytes;
    };

    ProcessLog* log{};
    std::array<Stream, 2U> streams{};

    std::mutex mutex;
    std::condition_variable changed;
    std::deque<ConsoleChunk> console;
    std::size_t console_bytes{};
    bool console_stopping{};
    std::thread forwarder;
    static constexpr std::size_t console_capacity = 1024U * 1024U;

    static int platform_write(int fd, const char* data, std::size_t size) {
#if defined(_WIN32)
        return ::_write(fd, data, static_cast<unsigned>(size));
#else
        const ssize_t written = ::write(fd, data, size);
        return static_cast<int>(written);
#endif
    }

    void read_loop(Stream& stream) noexcept {
        std::array<char, 16U * 1024U> buffer{};
        for (;;) {
#if defined(_WIN32)
            const int received = ::_read(stream.read, buffer.data(),
                                         static_cast<unsigned>(buffer.size()));
#else
            const ssize_t raw = ::read(stream.read, buffer.data(),
                                       buffer.size());
            if (raw < 0 && errno == EINTR) {
                continue;
            }
            const int received = static_cast<int>(raw);
#endif
            if (received <= 0) {
                return;
            }
            const std::string_view bytes{buffer.data(),
                                         static_cast<std::size_t>(received)};
            log->write(bytes, static_cast<core::u8>(stream.target));
            if (stream.original < 0) {
                continue;
            }
            {
                const std::lock_guard lock{mutex};
                if (console_bytes + bytes.size() > console_capacity) {
                    continue;
                }
                try {
                    console.push_back({stream.original, std::string{bytes}});
                } catch (...) {
                    continue;
                }
                console_bytes += bytes.size();
            }
            changed.notify_one();
        }
    }

    void forward_loop() noexcept {
        std::array<bool, 2U> dead{};
        for (;;) {
            ConsoleChunk chunk{};
            {
                std::unique_lock lock{mutex};
                changed.wait(lock, [&] {
                    return console_stopping || !console.empty();
                });
                if (console.empty()) {
                    return;
                }
                chunk = std::move(console.front());
                console.pop_front();
                console_bytes -= chunk.bytes.size();
            }
            const std::size_t slot = chunk.fd == streams[0].original ? 0U : 1U;
            std::size_t offset{};
            while (!dead[slot] && offset < chunk.bytes.size()) {
                const int written = platform_write(
                    chunk.fd, chunk.bytes.data() + offset,
                    chunk.bytes.size() - offset);
                if (written <= 0) {
                    // The console went away (FreeConsole, a closed pipe).
                    dead[slot] = true;
                    break;
                }
                offset += static_cast<std::size_t>(written);
            }
        }
    }
};

} // namespace detail

std::unique_ptr<StandardStreamCapture>
StandardStreamCapture::start(ProcessLog& log) {
    try {
        auto state = std::make_unique<detail::StandardStreamCaptureState>();
        state->log = &log;
        std::fflush(stdout);
        std::fflush(stderr);
        const std::array<int, 2U> targets{1, 2};
        std::size_t redirected{};
        for (std::size_t index = 0U; index < targets.size(); ++index) {
            auto& stream = state->streams[index];
            stream.target = targets[index];
            std::array<int, 2U> pipe_fds{-1, -1};
#if defined(_WIN32)
            if (::_pipe(pipe_fds.data(), 1U << 20U,
                        _O_BINARY | _O_NOINHERIT) != 0) {
                break;
            }
            stream.original = ::_dup(stream.target);
            if (::_dup2(pipe_fds[1], stream.target) != 0) {
                ::_close(pipe_fds[0]);
                ::_close(pipe_fds[1]);
                if (stream.original >= 0) {
                    ::_close(stream.original);
                    stream.original = -1;
                }
                break;
            }
            ::_close(pipe_fds[1]);
            // _dup2 also repointed the Win32 standard handle. Point it back at
            // the console so render processes this one launches still share
            // it, as they did before the log existed.
            if (stream.original >= 0) {
                ::SetStdHandle(stream.target == 1 ? STD_OUTPUT_HANDLE
                                                  : STD_ERROR_HANDLE,
                               reinterpret_cast<HANDLE>(
                                   ::_get_osfhandle(stream.original)));
            }
#else
            if (::pipe(pipe_fds.data()) != 0) {
                break;
            }
            ::fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
            stream.original = ::dup(stream.target);
            if (stream.original >= 0) {
                ::fcntl(stream.original, F_SETFD, FD_CLOEXEC);
            }
            if (::dup2(pipe_fds[1], stream.target) < 0) {
                ::close(pipe_fds[0]);
                ::close(pipe_fds[1]);
                if (stream.original >= 0) {
                    ::close(stream.original);
                    stream.original = -1;
                }
                break;
            }
            ::close(pipe_fds[1]);
#endif
            stream.read = pipe_fds[0];
            ++redirected;
        }
        if (redirected == 0U) {
            return nullptr;
        }
        // Every printf now reaches the pipe at once; the reader thread is
        // what stands between a full buffer and the disk.
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        detail::StandardStreamCaptureState* raw = state.get();
        raw->forwarder = std::thread{[raw] { raw->forward_loop(); }};
        for (auto& stream : raw->streams) {
            if (stream.read >= 0) {
                stream.reader =
                    std::thread{[raw, &stream] { raw->read_loop(stream); }};
            }
        }
        return std::unique_ptr<StandardStreamCapture>{
            new StandardStreamCapture{std::move(state)}};
    } catch (...) {
        return nullptr;
    }
}

StandardStreamCapture::StandardStreamCapture(
    std::unique_ptr<detail::StandardStreamCaptureState> state)
    : state_(std::move(state)) {}

StandardStreamCapture::~StandardStreamCapture() {
    std::fflush(stdout);
    std::fflush(stderr);
    for (auto& stream : state_->streams) {
        if (stream.read < 0) {
            continue;
        }
        // Closing the pipe's last write end ends its reader.
#if defined(_WIN32)
        if (stream.original >= 0) {
            ::_dup2(stream.original, stream.target);
            ::SetStdHandle(stream.target == 1 ? STD_OUTPUT_HANDLE
                                              : STD_ERROR_HANDLE,
                           reinterpret_cast<HANDLE>(
                               ::_get_osfhandle(stream.target)));
        } else {
            ::_close(stream.target);
        }
#else
        if (stream.original >= 0) {
            ::dup2(stream.original, stream.target);
        } else {
            ::close(stream.target);
        }
#endif
        if (stream.reader.joinable()) {
            stream.reader.join();
        }
#if defined(_WIN32)
        ::_close(stream.read);
#else
        ::close(stream.read);
#endif
    }
    {
        const std::lock_guard lock{state_->mutex};
        state_->console_stopping = true;
    }
    state_->changed.notify_all();
    if (state_->forwarder.joinable()) {
        state_->forwarder.join();
    }
    for (auto& stream : state_->streams) {
        if (stream.original >= 0) {
#if defined(_WIN32)
            ::_close(stream.original);
#else
            ::close(stream.original);
#endif
        }
    }
}

// ---------------------------------------------------------------- tail

ProcessLogTail read_process_log_tail(const std::filesystem::path& directory,
                                     std::string_view name,
                                     std::size_t max_lines,
                                     std::size_t window_bytes) {
    ProcessLogTail tail{};
    if (max_lines == 0U || !valid_process_log_name(name)) {
        return tail;
    }
    bool whole{};
    const auto current = read_shared(process_log_file(directory, name),
                                     window_bytes, true, whole);
    if (!current.has_value()) {
        return tail;
    }
    tail.found = true;
    std::vector<std::string> lines = split_lines(*current, whole);
    // The current file begins a run when its first line is the run header;
    // otherwise it was rotated for size and the older file is the same run.
    bool begins_run{};
    if (whole) {
        begins_run = !lines.empty() &&
            lines.front().find(process_log_run_marker) != std::string::npos;
    } else {
        bool head_whole{};
        const auto head = read_shared(process_log_file(directory, name), 512U,
                                      false, head_whole);
        begins_run = head.has_value() &&
            head->substr(0U, head->find('\n')).find(process_log_run_marker) !=
                std::string::npos;
        tail.truncated = true;
    }
    if (lines.size() > max_lines) {
        lines.erase(lines.begin(),
                    lines.begin() +
                        static_cast<std::ptrdiff_t>(lines.size() - max_lines));
        tail.truncated = true;
    } else if (lines.size() < max_lines) {
        bool older_whole{};
        const auto older = read_shared(process_log_file(directory, name, 1U),
                                       window_bytes, true, older_whole);
        if (older.has_value()) {
            std::vector<std::string> earlier =
                split_lines(*older, older_whole);
            const std::size_t wanted = max_lines - lines.size();
            if (earlier.size() > wanted) {
                earlier.erase(earlier.begin(),
                              earlier.begin() +
                                  static_cast<std::ptrdiff_t>(
                                      earlier.size() - wanted));
                tail.truncated = true;
            } else if (!older_whole) {
                tail.truncated = true;
            }
            if (begins_run) {
                tail.previous_run_lines = earlier.size();
            }
            earlier.insert(earlier.end(),
                           std::make_move_iterator(lines.begin()),
                           std::make_move_iterator(lines.end()));
            lines = std::move(earlier);
        }
    }
    for (const std::string& line : lines) {
        const std::size_t marker = line.find(process_log_dropped_marker);
        if (marker == std::string::npos) {
            continue;
        }
        const char* begin =
            line.c_str() + marker + process_log_dropped_marker.size();
        char* end{};
        const unsigned long long value = std::strtoull(begin, &end, 10);
        if (end != begin) {
            tail.dropped_bytes += value;
        }
    }
    tail.lines = std::move(lines);
    return tail;
}

ProcessLogTailProvider::ProcessLogTailProvider(std::filesystem::path directory,
                                               std::string name)
    : directory_(std::move(directory)), name_(std::move(name)) {}

net::ClusterNodeLog ProcessLogTailProvider::tail(
    std::size_t max_lines) noexcept {
    net::ClusterNodeLog log{};
    try {
        ProcessLogTail tail =
            read_process_log_tail(directory_, name_, max_lines);
        if (!tail.found) {
            log.availability = net::ClusterNodeLogAvailability::Unavailable;
            log.detail = "no log yet: the render process has not run on "
                         "this machine";
            return log;
        }
        log.availability = net::ClusterNodeLogAvailability::Available;
        log.previous_run_lines =
            static_cast<core::u32>(tail.previous_run_lines);
        log.dropped_bytes = tail.dropped_bytes;
        log.truncated = tail.truncated;
        log.lines = std::move(tail.lines);
        const std::u8string file =
            process_log_file(directory_, name_).u8string();
        log.detail.assign(reinterpret_cast<const char*>(file.data()),
                          file.size());
    } catch (...) {
        log = net::ClusterNodeLog{};
        log.availability = net::ClusterNodeLogAvailability::Failed;
        log.detail = "the log could not be read";
    }
    return log;
}

} // namespace mediaserver::app
