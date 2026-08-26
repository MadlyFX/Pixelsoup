#include "project_asset_import_worker.hpp"

#include <algorithm>
#include <new>
#include <system_error>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace mediaserver::app {
namespace {

[[nodiscard]] ProjectAssetImportResult allocation_failure() noexcept {
    return ProjectAssetImportResult::failure(ProjectAssetImportError{
        ProjectAssetImportErrorCode::AllocationFailed, {}});
}

[[nodiscard]] ProjectAssetImportResult unexpected_failure() noexcept {
    return ProjectAssetImportResult::failure(ProjectAssetImportError{
        ProjectAssetImportErrorCode::SourceReadFailed,
        std::make_error_code(std::errc::io_error)});
}

[[nodiscard]] bool cancelled(
    const ProjectAssetImportResult& result) noexcept {
    return !result && result.error().code ==
        ProjectAssetImportErrorCode::Cancelled;
}

void cancel_synchronous_io(std::thread& thread) noexcept {
#if defined(_WIN32)
    if (thread.joinable()) {
        static_cast<void>(CancelSynchronousIo(thread.native_handle()));
    }
#else
    static_cast<void>(thread);
#endif
}

} // namespace

ProjectAssetImportWorker::ProjectAssetImportWorker(
    std::filesystem::path shared_store, std::size_t capacity,
    ProjectAssetImportFunction importer)
    : shared_store_(std::move(shared_store)),
      capacity_(std::max<std::size_t>(capacity, 1U)),
      importer_(importer ? std::move(importer)
                         : ProjectAssetImportFunction{
                               prepare_project_asset_import}),
      completed_(capacity_),
      thread_([this] { work(); }) {}

ProjectAssetImportWorker::~ProjectAssetImportWorker() {
    bool cancel_active{};
    {
        const std::scoped_lock lock{mutex_};
        stopping_ = true;
        const std::size_t retired = pending_.size() + completed_count_;
        outstanding_ = retired <= outstanding_
            ? outstanding_ - retired
            : 0U;
        pending_.clear();
        for (auto& completed : completed_) {
            completed.reset();
        }
        completed_head_ = 0U;
        completed_tail_ = 0U;
        completed_count_ = 0U;
        if (active_) {
            active_discarded_ = true;
            static_cast<void>(active_stop_.request_stop());
            cancel_active = true;
        }
    }
    ready_.notify_one();
    if (cancel_active) {
        // Cooperative 64 KiB/1 MiB checkpoints are the primary path. This is
        // a best-effort wake for a synchronous local/UNC read currently
        // blocked in the OS. The worker is still joined; a pathological
        // filesystem driver that refuses cancellation can delay shutdown.
        cancel_synchronous_io(thread_);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

ProjectAssetImportSubmitResult ProjectAssetImportWorker::submit(
    ProjectAssetImportJob job) noexcept {
    try {
        {
            const std::scoped_lock lock{mutex_};
            if (stopping_) {
                return ProjectAssetImportSubmitResult::Stopping;
            }
            if (outstanding_ >= capacity_) {
                return ProjectAssetImportSubmitResult::QueueFull;
            }
            pending_.push_back(std::move(job));
            ++outstanding_;
        }
        ready_.notify_one();
        return ProjectAssetImportSubmitResult::Accepted;
    } catch (const std::bad_alloc&) {
        return ProjectAssetImportSubmitResult::AllocationFailed;
    }
}

std::optional<ProjectAssetImportCompletion>
ProjectAssetImportWorker::take_completed() noexcept {
    const std::scoped_lock lock{mutex_};
    if (completed_count_ == 0U) {
        return std::nullopt;
    }
    std::optional<ProjectAssetImportCompletion> result{
        std::move(*completed_[completed_head_])};
    completed_[completed_head_].reset();
    completed_head_ = (completed_head_ + 1U) % capacity_;
    --completed_count_;
    --outstanding_;
    return result;
}

void ProjectAssetImportWorker::discard_waiting() noexcept {
    bool cancel_active{};
    {
        const std::scoped_lock lock{mutex_};
        const std::size_t retired = pending_.size() + completed_count_;
        outstanding_ = retired <= outstanding_
            ? outstanding_ - retired
            : 0U;
        pending_.clear();
        for (auto& completed : completed_) {
            completed.reset();
        }
        completed_head_ = 0U;
        completed_tail_ = 0U;
        completed_count_ = 0U;
        if (active_) {
            active_discarded_ = true;
            static_cast<void>(active_stop_.request_stop());
            cancel_active = true;
        }
    }
    if (cancel_active) {
        cancel_synchronous_io(thread_);
    }
}

void ProjectAssetImportWorker::work() noexcept {
    for (;;) {
        std::optional<ProjectAssetImportJob> job;
        {
            std::unique_lock lock{mutex_};
            ready_.wait(lock, [this] {
                return stopping_ || !pending_.empty();
            });
            if (pending_.empty()) {
                if (stopping_) {
                    return;
                }
                continue;
            }
            job.emplace(std::move(pending_.front()));
            pending_.pop_front();
            active_stop_ = std::stop_source{};
            active_ = true;
            active_discarded_ = false;
        }
        const std::stop_token stop = active_stop_.get_token();

        ProjectAssetImportResult result = [&]() noexcept {
            try {
                return importer_(
                    std::move(job->asset), shared_store_, stop);
            } catch (const std::bad_alloc&) {
                return allocation_failure();
            } catch (...) {
                // Background work must not be able to terminate the Director.
                return unexpected_failure();
            }
        }();

        static_assert(std::is_nothrow_move_constructible_v<
                      ProjectAssetImportCompletion>);
        const std::scoped_lock lock{mutex_};
        const bool suppress = stopping_ || active_discarded_ ||
            stop.stop_requested() || cancelled(result);
        active_ = false;
        active_discarded_ = false;
        if (suppress) {
            if (outstanding_ != 0U) {
                --outstanding_;
            }
            continue;
        }
        // Total outstanding work is bounded by capacity_, so a completion
        // slot is always available once this job leaves the pending queue.
        completed_[completed_tail_].emplace(ProjectAssetImportCompletion{
            job->token, job->project_generation, job->gesture,
            std::move(result)});
        completed_tail_ = (completed_tail_ + 1U) % capacity_;
        ++completed_count_;
    }
}

} // namespace mediaserver::app
