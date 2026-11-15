#pragma once

// Reads a project for the Director on a thread of its own.
//
// Opening a project reads and decodes its file, which takes as long as the
// disk or the file server takes. The Director's loops hand the request here
// and carry on sealing, evaluating and drawing; the answer is adopted on the
// frame that finds it, and only staging the already-prepared document into
// the scheduler is left for that frame to do.
//
// The newest request is the one that matters. A request not yet started is
// replaced by a newer one, and a caller compares the ticket of what it takes
// with the last one it was given, so a project the operator has since
// replaced is never adopted.

#include "director_project_load.hpp"

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/persist/project_file.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

namespace mediaserver::app {

struct LoadedProject final {
    // The request this answers, as request() returned it.
    core::u64 ticket{};
    // The path asked for (UTF-8); empty for a project with no file.
    std::string path;
    // The document ready to stage - asset permits and publication locks of
    // the run that saved it removed - or why the file could not be read.
    persist::ProjectLoadResult project;
};

// Reads and decodes the project file at a UTF-8 path. The real one is
// persist::load_project_file, as slow as the disk or share it reads from.
using ProjectReader =
    std::function<persist::ProjectLoadResult(const std::string &)>;

class ProjectLoader final {
  public:
    // `read` reads a project file; persist::load_project_file when empty.
    explicit ProjectLoader(ProjectReader read = {});
    ProjectLoader(const ProjectLoader &) = delete;
    ProjectLoader &operator=(const ProjectLoader &) = delete;
    // Waits for a read under way, then stops.
    ~ProjectLoader();

    // Queues a load and returns its ticket, or 0 when it could not be queued.
    // `preloaded` is a document already decoded (the start-up project, a
    // recovery copy, a new project): the file at `path` is then not read.
    // Never touches the disk and never waits for the worker.
    [[nodiscard]] core::u64
    request(std::string path, std::optional<doc::Root> preloaded) noexcept;

    // The newest finished load not yet taken. Never blocks: while the worker
    // is publishing one it answers nothing, and the next frame asks again.
    [[nodiscard]] std::optional<LoadedProject> take() noexcept;

    // Requests replaced before they were answered.
    [[nodiscard]] core::u64 superseded() const noexcept {
        return superseded_.load(std::memory_order_relaxed);
    }

    // Blocks until nothing is queued or running. Start-up and tests only.
    void drain();

  private:
    struct Job final {
        core::u64 ticket{};
        std::string path;
        std::optional<doc::Root> preloaded;
    };

    void run(std::stop_token stop);
    [[nodiscard]] LoadedProject load(const Job &job) const;

    ProjectReader read_;

    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::condition_variable idle_;
    std::optional<Job> queued_;
    std::optional<LoadedProject> finished_;
    core::u64 next_ticket_{1U};
    bool busy_{};
    // The ticket of the newest request, for the worker to notice it has been
    // overtaken while it was reading.
    std::atomic<core::u64> newest_{};
    std::atomic<core::u64> superseded_{};
    std::jthread worker_;
};

} // namespace mediaserver::app
