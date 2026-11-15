#pragma once

// Protects unsaved project work against the editor stopping unexpectedly.
//
// While a project differs from its saved file, a recovery copy of it is kept
// in the machine's recovery folder. The copy is an ordinary project file, one
// per project path, written by this class's own thread: the Director hands it
// an immutable document snapshot and never waits for the disk. Saving the
// project, or deliberately discarding its changes, removes the copy; anything
// else that ends the editor leaves it for the next start to offer back.

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/doc/node.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace mediaserver::app {

// How long a changed project may go without a fresh recovery copy.
inline constexpr std::chrono::seconds project_autosave_interval{10};

// The recovery copy of the project saved at `project_path` (UTF-8, as the
// Director holds it). An untitled project - an empty path - has one of its
// own, so work that was never saved is protected too.
[[nodiscard]] std::filesystem::path
project_recovery_file(const std::filesystem::path &directory,
                      std::string_view project_path);

// The document Save writes for a live one: asset activation permits belong to
// the running cohort and are left out, and the transport is the one given.
[[nodiscard]] doc::Root saved_project_content(const doc::Root &projected,
                                              const doc::Node &transport);

struct ProjectAutosaveCounters final {
    core::u64 written{};
    core::u64 failed{};
    core::u64 removed{};
    // Snapshots replaced by a newer one before the worker reached them.
    core::u64 superseded{};

    bool operator==(const ProjectAutosaveCounters &) const = default;
};

class ProjectAutosave final {
  public:
    using Clock = std::chrono::steady_clock;

    explicit ProjectAutosave(
        std::filesystem::path directory,
        Clock::duration interval = project_autosave_interval);
    ProjectAutosave(const ProjectAutosave &) = delete;
    ProjectAutosave &operator=(const ProjectAutosave &) = delete;
    // Finishes what is queued, so a removal asked for at shutdown happens.
    ~ProjectAutosave();

    // Whether the project should be looked at again: the interval has passed
    // and the document or its path is not the one last observed. Cheap enough
    // to ask every frame; the dirty comparison is only made when this is true.
    [[nodiscard]] bool due(Clock::time_point now, doc::Version version,
                           std::string_view project_path) const noexcept;

    // Records what the project looks like now. A dirty project's snapshot is
    // queued for writing; a clean one has the copy this session wrote for it
    // removed, so undoing back to the saved file leaves nothing to recover.
    void observe(Clock::time_point now, bool dirty, const doc::Root &projected,
                 const doc::Node &transport, std::string_view project_path);

    // The project was saved, or its changes were deliberately given up:
    // removes its recovery copy and anything still queued for it.
    void discard(std::string_view project_path);

    // Makes the next due() answer true once the interval allows, whatever the
    // document version: a loaded project may reuse the previous one's.
    void invalidate() noexcept;

    // Blocks until the queue is empty. Shutdown and tests only.
    void drain();

    [[nodiscard]] ProjectAutosaveCounters counters() const noexcept;
    [[nodiscard]] const std::filesystem::path &directory() const noexcept {
        return directory_;
    }

  private:
    struct Job final {
        std::filesystem::path target;
        // Empty for a removal.
        std::optional<doc::Root> projected;
        std::optional<doc::Node> transport;
    };

    void enqueue(Job job);
    void run();
    void write(const Job &job) noexcept;
    void remove(const Job &job) noexcept;

    std::filesystem::path directory_;
    Clock::duration interval_;

    // Control-thread state.
    bool observed_{};
    doc::Version observed_version_{};
    std::string observed_path_;
    Clock::time_point next_due_{Clock::time_point::min()};
    // The project path this session last queued a recovery copy for.
    std::optional<std::string> written_path_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<Job> jobs_;
    bool busy_{};
    bool stopping_{};

    std::atomic<core::u64> written_{};
    std::atomic<core::u64> failed_{};
    std::atomic<core::u64> removed_{};
    std::atomic<core::u64> superseded_{};

    std::thread worker_;
};

// Unsaved work found in the recovery folder at start-up.
struct ProjectRecovery final {
    // The project it belongs to; empty for one that was never saved.
    std::string project_path;
    doc::Root project;
    // When the copy was written, in local time, for the operator.
    std::string saved_at;
};

// The newest readable recovery copy for the project being opened or for an
// untitled one. `saved` is the opened project's saved content: a copy that
// holds nothing more than the file already does is not offered. Reads project
// files, so it belongs to start-up and never to the frame loop.
[[nodiscard]] std::optional<ProjectRecovery>
find_project_recovery(const std::filesystem::path &directory,
                      std::string_view project_path, const doc::Root *saved);

} // namespace mediaserver::app
