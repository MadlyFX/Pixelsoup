#include "project_loader.hpp"

#include <mediaserver/scene/scene_model.hpp>

#include <utility>

namespace mediaserver::app {

ProjectLoader::ProjectLoader(ProjectReader read)
    : read_{read ? std::move(read)
                 : ProjectReader{[](const std::string &path) {
                       return persist::load_project_file(
                           native_path_from_utf8(path));
                   }}},
      worker_([this](std::stop_token stop) { run(stop); }) {}

ProjectLoader::~ProjectLoader() {
    worker_.request_stop();
    wake_.notify_all();
}

core::u64 ProjectLoader::request(std::string path,
                                 std::optional<doc::Root> preloaded) noexcept {
    try {
        Job job{0U, std::move(path), std::move(preloaded)};
        core::u64 ticket{};
        {
            const std::lock_guard lock{mutex_};
            ticket = next_ticket_++;
            job.ticket = ticket;
            // Whatever was asked before - waiting, being read, or read and
            // not yet taken - is no longer the project to open.
            if (queued_.has_value() || busy_ || finished_.has_value()) {
                superseded_.fetch_add(1U, std::memory_order_relaxed);
            }
            finished_.reset();
            queued_ = std::move(job);
            newest_.store(ticket, std::memory_order_relaxed);
        }
        wake_.notify_one();
        return ticket;
    } catch (...) {
        return 0U;
    }
}

std::optional<LoadedProject> ProjectLoader::take() noexcept {
    const std::unique_lock lock{mutex_, std::try_to_lock};
    if (!lock.owns_lock() || !finished_.has_value()) {
        return std::nullopt;
    }
    std::optional<LoadedProject> taken = std::move(finished_);
    finished_.reset();
    return taken;
}

void ProjectLoader::drain() {
    std::unique_lock lock{mutex_};
    idle_.wait(lock, [this] { return !queued_.has_value() && !busy_; });
}

LoadedProject ProjectLoader::load(const Job &job) const {
    LoadedProject loaded{job.ticket, job.path,
                         job.preloaded.has_value()
                             ? persist::ProjectLoadResult::success(*job.preloaded)
                             : read_(job.path)};
    if (!loaded.project) {
        return loaded;
    }
    // Permits and locks belong to the cohort that was running when the file
    // was saved; this run issues its own.
    loaded.project = persist::ProjectLoadResult::success(
        scene::without_publication_locks(
            scene::without_asset_commits(loaded.project.value())));
    return loaded;
}

void ProjectLoader::run(std::stop_token stop) {
    for (;;) {
        Job job;
        {
            std::unique_lock lock{mutex_};
            busy_ = false;
            if (!queued_.has_value()) {
                idle_.notify_all();
            }
            wake_.wait(lock, stop, [this] { return queued_.has_value(); });
            if (stop.stop_requested()) {
                return;
            }
            job = std::move(*queued_);
            queued_.reset();
            busy_ = true;
        }
        std::optional<LoadedProject> loaded;
        try {
            loaded = load(job);
        } catch (...) {
            // Out of memory while decoding or preparing. The request is
            // still answered, so the caller stops waiting for it.
            try {
                persist::ProjectFileError error{};
                error.code = persist::ProjectFileErrorCode::AllocationFailed;
                loaded = LoadedProject{
                    job.ticket, job.path,
                    persist::ProjectLoadResult::failure(std::move(error))};
            } catch (...) {
                loaded.reset();
            }
        }
        const std::lock_guard lock{mutex_};
        // Overtaken while it was being read: the newer request answers.
        if (loaded.has_value() &&
            newest_.load(std::memory_order_relaxed) == job.ticket) {
            finished_ = std::move(loaded);
        }
    }
}

} // namespace mediaserver::app
