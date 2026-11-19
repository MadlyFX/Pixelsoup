#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/net/cluster_lifecycle.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Persistent diagnostics for every pixelsoup process: what it prints to
// stdout and stderr is also written, timestamped, to a rotating file under a
// per-user log folder. A render node is often a fullscreen release build with
// no console, and the machine-local lifecycle agent reads its log back from
// that folder for Output Monitor (see ProcessLogTailProvider).
//
// The frame and control paths never touch the disk. They write to stdout as
// they always have; a reader thread drains the redirected stream into a
// bounded in-memory queue, and one writer thread owns the file. When the queue
// is full the bytes are dropped and counted, and the writer records how many
// were lost, so a stalled disk degrades the log rather than the show.
namespace mediaserver::app {

struct ProcessLogConfig final {
    std::filesystem::path directory;
    // The file stem: "director", "node-3", "agent-3". It must be a plain
    // name - letters, digits, '-', '_' and '.' only.
    std::string name;
    // The first line of the new file, after its timestamp. It marks where a
    // run begins, which is how a reader tells the previous run apart.
    std::string header;
    // A file is rotated once it would grow past this many bytes.
    std::uint64_t max_file_bytes{8U * 1024U * 1024U};
    // How many rotated files are kept: <name>.1.log is the newest.
    std::size_t backups{3U};
    // Bytes the writer may have queued and not yet written.
    std::size_t queue_bytes{1024U * 1024U};
};

struct ProcessLogCounters final {
    core::u64 written_lines{};
    core::u64 dropped_bytes{};
    core::u64 rotations{};
    core::u64 write_failures{};
};

// Every line of a run begins with this after its timestamp; see
// ProcessLogConfig::header.
inline constexpr std::string_view process_log_run_marker{"=== run started"};
// The writer's own note that bytes were dropped; see ProcessLog::write.
inline constexpr std::string_view process_log_dropped_marker{"[log] dropped "};

[[nodiscard]] bool valid_process_log_name(std::string_view name) noexcept;

// <directory>/<name>.log for generation 0, <directory>/<name>.<n>.log after.
[[nodiscard]] std::filesystem::path
process_log_file(const std::filesystem::path& directory,
                 std::string_view name, std::size_t generation = 0U);

// %LOCALAPPDATA%\Pixelsoup\logs on Windows; $XDG_STATE_HOME/mediaserver/logs
// or ~/.local/state/mediaserver/logs elsewhere. Empty when neither exists.
[[nodiscard]] std::filesystem::path default_log_directory();

namespace detail {
class ProcessLogState;
class StandardStreamCaptureState;
} // namespace detail

class ProcessLog final {
public:
    // Creates the folder, moves the previous run's file to <name>.1.log, opens
    // a fresh file with the header line and starts the writer thread. A file
    // another live process is writing is left alone: this run then logs to
    // <name>-<pid>.log instead. Returns a reason on failure.
    [[nodiscard]] static core::Result<std::unique_ptr<ProcessLog>,
                                      std::string>
    open(ProcessLogConfig config);

    ProcessLog(const ProcessLog&) = delete;
    ProcessLog& operator=(const ProcessLog&) = delete;
    // Writes everything queued, then stops the writer.
    ~ProcessLog();

    // Never waits for the disk. Copies the bytes into the queue, or counts
    // them as dropped when the queue is full. Lines are split on '\n' and
    // stamped with the time they were queued. Each source (0 for direct
    // writes, 1 stdout, 2 stderr) assembles its own lines, so a partial
    // line on one never swallows another's.
    void write(std::string_view text, core::u8 source = 0U) noexcept;
    // Blocks until everything queued so far is on disk. For shutdown and
    // tests; never call it from a frame or control path.
    void flush() noexcept;

    [[nodiscard]] const std::filesystem::path& file() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] ProcessLogCounters counters() const noexcept;

private:
    explicit ProcessLog(std::unique_ptr<detail::ProcessLogState> state);
    std::unique_ptr<detail::ProcessLogState> state_;
};

// Redirects this process's stdout and stderr through pipes so that every byte
// written to them - printf, iostreams, any library - reaches the log as well
// as the console it was going to. Console forwarding has its own bounded
// queue, so a console that stops reading (a selection in a Windows console
// window) no longer stalls the writer either. Win32 standard handles keep
// pointing at the original console, so child processes still share it.
class StandardStreamCapture final {
public:
    // nullptr when the streams cannot be redirected; the process then simply
    // runs without a disk log of its console output.
    [[nodiscard]] static std::unique_ptr<StandardStreamCapture>
    start(ProcessLog& log);

    StandardStreamCapture(const StandardStreamCapture&) = delete;
    StandardStreamCapture& operator=(const StandardStreamCapture&) = delete;
    // Flushes the C streams, restores the original descriptors and drains
    // both pipes into the log and the console.
    ~StandardStreamCapture();

private:
    explicit StandardStreamCapture(
        std::unique_ptr<detail::StandardStreamCaptureState> state);
    std::unique_ptr<detail::StandardStreamCaptureState> state_;
};

struct ProcessLogTail final {
    // False when no log file exists for the name.
    bool found{};
    // Oldest first. The first previous_run_lines come from the run before the
    // current file's.
    std::vector<std::string> lines;
    std::size_t previous_run_lines{};
    // The sum of the writer's dropped-byte notes among the returned lines.
    core::u64 dropped_bytes{};
    // More lines exist than were read.
    bool truncated{};
};

// Reads at most `window_bytes` from the end of the current file and, when it
// has fewer than max_lines, from the end of <name>.1.log. Files are opened
// for shared reading, so the writer can keep appending and rotating.
[[nodiscard]] ProcessLogTail
read_process_log_tail(const std::filesystem::path& directory,
                      std::string_view name, std::size_t max_lines,
                      std::size_t window_bytes = 128U * 1024U);

// What the lifecycle agent serves to Output Monitor: the render process's
// log, read from the folder it was told to write into.
class ProcessLogTailProvider final : public net::ClusterNodeLogProvider {
public:
    ProcessLogTailProvider(std::filesystem::path directory, std::string name);
    [[nodiscard]] net::ClusterNodeLog tail(std::size_t max_lines) noexcept
        override;

private:
    std::filesystem::path directory_;
    std::string name_;
};

// The log name of a role: "director", "node-<index>", "agent-<index>".
[[nodiscard]] std::string node_process_log_name(core::u32 node_index);
[[nodiscard]] std::string agent_process_log_name(core::u32 node_index);

} // namespace mediaserver::app
