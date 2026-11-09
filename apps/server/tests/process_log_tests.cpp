#include "process_log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace app = mediaserver::app;
namespace net = mediaserver::net;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define LOG_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("mediaserver-process-log-" + std::to_string(timestamp) + "-" +
                 std::to_string(sequence.fetch_add(1U)));
        std::filesystem::create_directories(path_);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::size_t count_lines(std::string_view text) {
    return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
}

[[nodiscard]] std::unique_ptr<app::ProcessLog>
open_log(const std::filesystem::path& directory, std::string name,
         std::uint64_t max_file_bytes = 8U * 1024U * 1024U,
         std::size_t queue_bytes = 1024U * 1024U) {
    app::ProcessLogConfig config{};
    config.directory = directory;
    config.name = std::move(name);
    config.header = "test run";
    config.max_file_bytes = max_file_bytes;
    config.queue_bytes = queue_bytes;
    auto opened = app::ProcessLog::open(std::move(config));
    if (!opened) {
        throw std::runtime_error{"open failed: " + opened.error()};
    }
    return std::move(opened).value();
}

[[nodiscard]] bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.substr(text.size() - suffix.size()) == suffix;
}

void names_and_paths_are_plain() {
    LOG_CHECK(app::valid_process_log_name("node-12"));
    LOG_CHECK(app::valid_process_log_name("director"));
    LOG_CHECK(!app::valid_process_log_name(""));
    LOG_CHECK(!app::valid_process_log_name("../node"));
    LOG_CHECK(!app::valid_process_log_name("node/0"));
    LOG_CHECK(!app::valid_process_log_name(".hidden"));
    LOG_CHECK(app::node_process_log_name(7U) == "node-7");
    LOG_CHECK(app::agent_process_log_name(7U) == "agent-7");
    const std::filesystem::path folder{"logs"};
    LOG_CHECK(app::process_log_file(folder, "node-3") ==
              folder / "node-3.log");
    LOG_CHECK(app::process_log_file(folder, "node-3", 2U) ==
              folder / "node-3.2.log");
    const auto fallback = app::default_log_directory();
    LOG_CHECK(fallback.empty() || fallback.is_absolute());
}

void lines_are_timestamped_after_a_run_header() {
    TempDirectory temp;
    auto log = open_log(temp.path(), "node-4");
    LOG_CHECK(log->file() == temp.path() / "node-4.log");
    log->write("first status line\nsecond ");
    log->write("half\r\nthird\n");
    log->flush();
    const std::string text = read_file(log->file());
    LOG_CHECK(count_lines(text) == 4U);
    LOG_CHECK(text.find(app::process_log_run_marker) != std::string::npos);
    LOG_CHECK(text.find("test run") != std::string::npos);
    LOG_CHECK(text.find(" first status line\n") != std::string::npos);
    // A line split across two writes is one line, without its CR.
    LOG_CHECK(text.find(" second half\n") != std::string::npos);
    LOG_CHECK(text.find('\r') == std::string::npos);
    // "YYYY-MM-DD HH:MM:SS.mmm " before every line.
    LOG_CHECK(text.size() > 24U && text[4] == '-' && text[10] == ' ' &&
              text[19] == '.' && text[23] == ' ');
    LOG_CHECK(log->counters().written_lines == 4U);
    LOG_CHECK(log->counters().dropped_bytes == 0U);
}

void a_new_run_keeps_the_previous_one_readable() {
    TempDirectory temp;
    {
        auto first = open_log(temp.path(), "node-1");
        first->write("loading clip.mov\nrender thread stalled\n");
    }
    auto second = open_log(temp.path(), "node-1");
    second->write("reconnecting\n");
    second->flush();
    LOG_CHECK(std::filesystem::exists(temp.path() / "node-1.1.log"));
    const auto tail = app::read_process_log_tail(temp.path(), "node-1", 50U);
    LOG_CHECK(tail.found);
    // The previous run's header and two lines, then this run's header and
    // one line.
    LOG_CHECK(tail.lines.size() == 5U);
    LOG_CHECK(tail.previous_run_lines == 3U);
    LOG_CHECK(ends_with(tail.lines[2], "render thread stalled"));
    LOG_CHECK(tail.lines[3].find(app::process_log_run_marker) !=
              std::string::npos);
    LOG_CHECK(ends_with(tail.lines[4], "reconnecting"));
    LOG_CHECK(!tail.truncated);

    // Fewer lines than the current run holds come from it alone.
    const auto short_tail =
        app::read_process_log_tail(temp.path(), "node-1", 1U);
    LOG_CHECK(short_tail.lines.size() == 1U);
    LOG_CHECK(short_tail.previous_run_lines == 0U);
    LOG_CHECK(short_tail.truncated);
    LOG_CHECK(ends_with(short_tail.lines[0], "reconnecting"));
}

void files_rotate_by_size_and_stay_bounded() {
    TempDirectory temp;
    constexpr std::uint64_t limit = 8U * 1024U;
    auto log = open_log(temp.path(), "node-2", limit);
    const std::string line(100U, 'x');
    for (int index = 0; index < 600; ++index) {
        log->write(line + std::to_string(index) + "\n");
        if (index % 50 == 0) {
            log->flush();
        }
    }
    log->flush();
    const auto counters = log->counters();
    LOG_CHECK(counters.rotations >= 3U);
    LOG_CHECK(counters.write_failures == 0U);
    for (std::size_t generation = 0U; generation <= 3U; ++generation) {
        const auto file =
            app::process_log_file(temp.path(), "node-2", generation);
        LOG_CHECK(std::filesystem::exists(file));
        LOG_CHECK(std::filesystem::file_size(file) <= limit);
    }
    // Three backups are kept, never a fourth.
    LOG_CHECK(!std::filesystem::exists(
        app::process_log_file(temp.path(), "node-2", 4U)));
    // A file rotated for size continues the same run: nothing in the tail is
    // attributed to a previous one.
    // About 64 lines fit a file; the tail reaches into the newest backup.
    const auto tail = app::read_process_log_tail(temp.path(), "node-2", 50U);
    LOG_CHECK(tail.lines.size() == 50U);
    LOG_CHECK(tail.previous_run_lines == 0U);
    LOG_CHECK(ends_with(tail.lines.back(), line + "599"));
}

void a_full_queue_drops_counts_and_says_so() {
    TempDirectory temp;
    auto log = open_log(temp.path(), "node-5", 8U * 1024U * 1024U, 4096U);
    // One write larger than the whole queue can never be accepted, whatever
    // the writer thread is doing; the bytes are counted, not waited on.
    const std::string burst(100'000U, 'b');
    const auto started = std::chrono::steady_clock::now();
    log->write(burst);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    LOG_CHECK(elapsed < std::chrono::milliseconds{50});
    log->write("after the burst\n");
    log->flush();
    LOG_CHECK(log->counters().dropped_bytes == 100'000U);
    const auto tail = app::read_process_log_tail(temp.path(), "node-5", 20U);
    LOG_CHECK(tail.dropped_bytes == 100'000U);
    const bool noted = std::any_of(
        tail.lines.begin(), tail.lines.end(), [](const std::string& line) {
            return line.find(app::process_log_dropped_marker) !=
                   std::string::npos;
        });
    LOG_CHECK(noted);
    LOG_CHECK(ends_with(tail.lines.back(), "after the burst") ||
              ends_with(tail.lines[tail.lines.size() - 2U],
                        "after the burst"));
}

void a_name_in_use_by_another_writer_gets_its_own_file() {
    TempDirectory temp;
    auto first = open_log(temp.path(), "director");
    first->write("first director\n");
    auto second = open_log(temp.path(), "director");
    second->write("second director\n");
    first->flush();
    second->flush();
    LOG_CHECK(second->file() != first->file());
    LOG_CHECK(second->name().starts_with("director-"));
    // Neither run disturbed the other's file.
    LOG_CHECK(read_file(first->file()).find("first director") !=
              std::string::npos);
    LOG_CHECK(read_file(first->file()).find("second director") ==
              std::string::npos);
    LOG_CHECK(read_file(second->file()).find("second director") !=
              std::string::npos);
    LOG_CHECK(!std::filesystem::exists(temp.path() / "director.1.log"));
}

void console_output_reaches_the_log() {
    TempDirectory temp;
    auto log = open_log(temp.path(), "node-6");
    {
        auto capture = app::StandardStreamCapture::start(*log);
        LOG_CHECK(capture != nullptr);
        std::printf("printf status frame=%d\n", 120);
        std::cout << "iostream session event\n" << std::flush;
        std::cerr << "stderr lockstep warning\n";
        std::fputs("unterminated tail", stdout);
    }
    log->flush();
    const std::string text = read_file(log->file());
    LOG_CHECK(text.find(" printf status frame=120\n") != std::string::npos);
    LOG_CHECK(text.find(" iostream session event\n") != std::string::npos);
    LOG_CHECK(text.find(" stderr lockstep warning\n") != std::string::npos);
    // After the capture ends, output goes to the console alone.
    std::cout << "after capture\n" << std::flush;
    log->flush();
    LOG_CHECK(read_file(log->file()).find("after capture") ==
              std::string::npos);
    log.reset();
    // A partial last line is written when the log closes.
    LOG_CHECK(read_file(temp.path() / "node-6.log")
                  .find(" unterminated tail\n") != std::string::npos);
}

void the_agent_provider_serves_the_render_log() {
    TempDirectory temp;
    app::ProcessLogTailProvider provider{temp.path(), "node-8"};
    const net::ClusterNodeLog none = provider.tail(100U);
    LOG_CHECK(none.availability ==
              net::ClusterNodeLogAvailability::Unavailable);
    LOG_CHECK(none.lines.empty());

    {
        auto previous = open_log(temp.path(), "node-8");
        previous->write("NODE 0 FELL BEHIND\n");
    }
    auto current = open_log(temp.path(), "node-8");
    current->write("adopted asset\tclip.mov\n");
    current->flush();
    const net::ClusterNodeLog log = provider.tail(100U);
    LOG_CHECK(log.availability == net::ClusterNodeLogAvailability::Available);
    LOG_CHECK(log.lines.size() == 4U);
    LOG_CHECK(log.previous_run_lines == 2U);
    LOG_CHECK(ends_with(log.lines[1], "NODE 0 FELL BEHIND"));
    // The raw tab is still here; the lifecycle service sanitizes it before
    // it crosses the wire.
    LOG_CHECK(ends_with(log.lines[3], "adopted asset\tclip.mov"));
    LOG_CHECK(log.detail.find("node-8.log") != std::string::npos);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"names and paths are plain", &names_and_paths_are_plain},
        {"lines are timestamped after a run header",
         &lines_are_timestamped_after_a_run_header},
        {"a new run keeps the previous one readable",
         &a_new_run_keeps_the_previous_one_readable},
        {"files rotate by size and stay bounded",
         &files_rotate_by_size_and_stay_bounded},
        {"a full queue drops, counts and says so",
         &a_full_queue_drops_counts_and_says_so},
        {"a name in use gets its own file",
         &a_name_in_use_by_another_writer_gets_its_own_file},
        {"console output reaches the log", &console_output_reaches_the_log},
        {"the agent provider serves the render log",
         &the_agent_provider_serves_the_render_log},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
