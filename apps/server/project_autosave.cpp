#include "project_autosave.hpp"

#include "director_project_load.hpp"

#include <mediaserver/core/fnv1a.hpp>
#include <mediaserver/persist/project_file.hpp>
#include <mediaserver/scene/scene_model.hpp>

#include <array>
#include <ctime>
#include <exception>
#include <iostream>
#include <system_error>
#include <utility>

namespace mediaserver::app {

namespace {

// Queued work is bounded by how it is asked for - one snapshot per interval,
// one removal per save or close - and same-target snapshots replace each
// other. The cap only keeps a disk that has stopped answering from growing
// the queue without limit.
constexpr std::size_t max_queued_jobs = 16U;

[[nodiscard]] std::string local_time_text(
    std::filesystem::file_time_type written) {
    // file_clock has no portable conversion to the system clock, so the copy's
    // age is measured on its own clock and taken off the present.
    const auto age = std::filesystem::file_time_type::clock::now() - written;
    const auto when = std::chrono::system_clock::now() -
                      std::chrono::duration_cast<
                          std::chrono::system_clock::duration>(age);
    const std::time_t seconds = std::chrono::system_clock::to_time_t(when);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    std::array<char, 32U> text{};
    const std::size_t length =
        std::strftime(text.data(), text.size(), "%Y-%m-%d %H:%M", &local);
    return std::string{text.data(), length};
}

} // namespace

std::filesystem::path
project_recovery_file(const std::filesystem::path &directory,
                      std::string_view project_path) {
    if (project_path.empty()) {
        return directory / "untitled.mstage";
    }
    // Named by the path's hash: the copy for a project is then found from
    // the project alone, with no second file that could name the wrong one.
    // The path is normalized first, because the one a dialog returned and the
    // one the recent-project setting stored may spell the same file apart.
    std::string normal = native_path_to_utf8(
        native_path_from_utf8(project_path).lexically_normal());
    for (char &byte : normal) {
        if (byte == '\\') {
            byte = '/';
        }
#if defined(_WIN32)
        if (byte >= 'A' && byte <= 'Z') {
            byte = static_cast<char>(byte - 'A' + 'a');
        }
#endif
    }
    core::u64 state = core::fnv1a_offset;
    core::fnv1a_bytes(state, normal.data(), normal.size());
    constexpr std::string_view digits = "0123456789abcdef";
    std::string name{"project-"};
    for (int shift = 60; shift >= 0; shift -= 4) {
        name.push_back(digits[static_cast<std::size_t>((state >> shift) & 0xFU)]);
    }
    name.append(".mstage");
    return directory / name;
}

doc::Root saved_project_content(const doc::Root &projected,
                                const doc::Node &transport) {
    return scene::without_asset_commits(projected).set(transport);
}

ProjectAutosave::ProjectAutosave(std::filesystem::path directory,
                                 Clock::duration interval)
    : directory_(std::move(directory)), interval_(interval),
      worker_([this] { run(); }) {}

ProjectAutosave::~ProjectAutosave() {
    {
        const std::lock_guard lock{mutex_};
        stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool ProjectAutosave::due(Clock::time_point now, doc::Version version,
                          std::string_view project_path) const noexcept {
    if (now < next_due_) {
        return false;
    }
    return !observed_ || version != observed_version_ ||
           project_path != observed_path_;
}

void ProjectAutosave::observe(Clock::time_point now, bool dirty,
                              const doc::Root &projected,
                              const doc::Node &transport,
                              std::string_view project_path) {
    observed_ = true;
    observed_version_ = projected.version();
    observed_path_.assign(project_path);
    next_due_ = now + interval_;
    if (dirty) {
        written_path_ = std::string{project_path};
        enqueue(Job{project_recovery_file(directory_, project_path), projected,
                    transport});
    } else if (written_path_.has_value() && *written_path_ == project_path) {
        written_path_.reset();
        enqueue(Job{project_recovery_file(directory_, project_path),
                    std::nullopt, std::nullopt});
    }
}

void ProjectAutosave::discard(std::string_view project_path) {
    if (written_path_.has_value() && *written_path_ == project_path) {
        written_path_.reset();
    }
    enqueue(Job{project_recovery_file(directory_, project_path), std::nullopt,
                std::nullopt});
}

void ProjectAutosave::invalidate() noexcept { observed_ = false; }

void ProjectAutosave::drain() {
    std::unique_lock lock{mutex_};
    idle_.wait(lock, [this] { return jobs_.empty() && !busy_; });
}

ProjectAutosaveCounters ProjectAutosave::counters() const noexcept {
    return ProjectAutosaveCounters{
        written_.load(std::memory_order_relaxed),
        failed_.load(std::memory_order_relaxed),
        removed_.load(std::memory_order_relaxed),
        superseded_.load(std::memory_order_relaxed)};
}

void ProjectAutosave::enqueue(Job job) {
    {
        // The worker holds this only to take a job, never across the disk.
        const std::lock_guard lock{mutex_};
        if (!job.projected.has_value()) {
            // A removal makes every queued snapshot for that file pointless,
            // and one left behind it would put the copy straight back.
            std::erase_if(jobs_, [&](const Job &queued) {
                if (queued.target != job.target) {
                    return false;
                }
                if (queued.projected.has_value()) {
                    superseded_.fetch_add(1U, std::memory_order_relaxed);
                }
                return true;
            });
        } else if (!jobs_.empty() && jobs_.back().projected.has_value() &&
                   jobs_.back().target == job.target) {
            jobs_.pop_back();
            superseded_.fetch_add(1U, std::memory_order_relaxed);
        }
        if (jobs_.size() >= max_queued_jobs) {
            if (jobs_.front().projected.has_value()) {
                superseded_.fetch_add(1U, std::memory_order_relaxed);
            }
            jobs_.pop_front();
        }
        jobs_.push_back(std::move(job));
    }
    wake_.notify_one();
}

void ProjectAutosave::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock{mutex_};
            busy_ = false;
            if (jobs_.empty()) {
                idle_.notify_all();
            }
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (jobs_.empty()) {
                return;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
            busy_ = true;
        }
        if (job.projected.has_value()) {
            write(job);
        } else {
            remove(job);
        }
        // The snapshot's document is released here, on this thread.
    }
}

void ProjectAutosave::write(const Job &job) noexcept {
    try {
        std::error_code directory_error;
        std::filesystem::create_directories(directory_, directory_error);
        const auto saved = persist::save_project_file(
            job.target, saved_project_content(*job.projected, *job.transport));
        // An installed copy whose final durability step failed is still the
        // newest copy there is.
        if (saved || saved.error().committed) {
            written_.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        std::cerr << "could not write the project recovery copy (code "
                  << static_cast<unsigned>(saved.error().code) << ')';
        if (saved.error().system_error) {
            std::cerr << ": " << saved.error().system_error.message();
        }
        std::cerr << '\n';
    } catch (const std::exception &error) {
        std::cerr << "could not write the project recovery copy: "
                  << error.what() << '\n';
    } catch (...) {
        std::cerr << "could not write the project recovery copy\n";
    }
    failed_.fetch_add(1U, std::memory_order_relaxed);
}

void ProjectAutosave::remove(const Job &job) noexcept {
    std::error_code error;
    if (std::filesystem::remove(job.target, error) && !error) {
        removed_.fetch_add(1U, std::memory_order_relaxed);
    } else if (error) {
        std::cerr << "could not remove the project recovery copy: "
                  << error.message() << '\n';
    }
}

std::optional<ProjectRecovery>
find_project_recovery(const std::filesystem::path &directory,
                      std::string_view project_path, const doc::Root *saved) {
    struct Candidate final {
        std::string project_path;
        const doc::Root *saved{};
    };
    std::array<Candidate, 2U> candidates{
        Candidate{std::string{project_path}, saved}, Candidate{{}, nullptr}};
    const std::size_t count = project_path.empty() ? 1U : 2U;
    if (project_path.empty()) {
        // Only the untitled copy applies, and it has no saved file to equal.
        candidates[0] = Candidate{{}, nullptr};
    }

    std::optional<ProjectRecovery> newest;
    std::filesystem::file_time_type newest_time{};
    for (std::size_t index = 0U; index < count; ++index) {
        const Candidate &candidate = candidates[index];
        const std::filesystem::path file =
            project_recovery_file(directory, candidate.project_path);
        std::error_code error;
        const auto written = std::filesystem::last_write_time(file, error);
        if (error) {
            continue;
        }
        auto loaded = persist::load_project_file(file);
        if (!loaded) {
            std::cerr << "ignored an unreadable project recovery copy (code "
                      << static_cast<unsigned>(loaded.error().code) << ")\n";
            continue;
        }
        if (candidate.saved != nullptr &&
            !project_content_changed(*candidate.saved, loaded.value())) {
            continue;
        }
        if (!newest.has_value() || written > newest_time) {
            newest_time = written;
            newest = ProjectRecovery{candidate.project_path,
                                     std::move(loaded).value(),
                                     local_time_text(written)};
        }
    }
    return newest;
}

} // namespace mediaserver::app
