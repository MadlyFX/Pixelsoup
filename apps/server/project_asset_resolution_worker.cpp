#include "project_asset_resolution_worker.hpp"

#include <mediaserver/assets/asset_registry.hpp>

#include <algorithm>
#include <new>
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

[[nodiscard]] bool retryable(LocalAssetState state) noexcept {
    return state == LocalAssetState::Missing ||
        state == LocalAssetState::Corrupt ||
        state == LocalAssetState::Error;
}

[[nodiscard]] LocalAssetState observation_for(
    const ResolvedProjectAssets& resolved, doc::NodeId asset) noexcept {
    const auto found = std::lower_bound(
        resolved.observations.begin(), resolved.observations.end(), asset,
        [](const LocalAssetObservation& candidate,
           doc::NodeId wanted) noexcept {
            return candidate.asset < wanted;
        });
    return found != resolved.observations.end() && found->asset == asset
        ? found->state
        : LocalAssetState::Error;
}

} // namespace

ProjectAssetResolutionWorker::ProjectAssetResolutionWorker(
    ProjectAssetCheckFunction checker)
    : checker_(checker ? std::move(checker)
                       : ProjectAssetCheckFunction{check_project_asset}),
      thread_([this] { work(); }) {}

ProjectAssetResolutionWorker::~ProjectAssetResolutionWorker() {
    {
        const std::scoped_lock lock{mutex_};
        stopping_ = true;
        pending_.reset();
        completed_.reset();
        static_cast<void>(active_stop_.request_stop());
    }
    ready_.notify_one();
#if defined(_WIN32)
    // Cooperative checkpoints normally unwind immediately. This is a
    // best-effort wake for a synchronous local/UNC filesystem read currently
    // blocked inside the worker; joining remains mandatory so no code can run
    // through CRT/module teardown.
    if (thread_.joinable()) {
        static_cast<void>(CancelSynchronousIo(thread_.native_handle()));
    }
#endif
    if (thread_.joinable()) {
        thread_.join();
    }
}

ProjectAssetResolutionSubmitResult ProjectAssetResolutionWorker::submit(
    ProjectAssetResolutionRequest request) noexcept {
    try {
        if (request.key.request_id == 0U || request.plan == nullptr ||
            request.baseline == nullptr ||
            request.key.manifest_digest != request.plan->manifest.digest ||
            request.plan->manifest.assets.size() >
                assets::max_project_assets) {
            return ProjectAssetResolutionSubmitResult::InvalidRequest;
        }
        {
            const std::scoped_lock lock{mutex_};
            if (stopping_) {
                return ProjectAssetResolutionSubmitResult::Stopping;
            }
            if (request.key.request_id <= latest_request_id_) {
                return ProjectAssetResolutionSubmitResult::InvalidRequest;
            }
            latest_request_id_ = request.key.request_id;
            static_cast<void>(active_stop_.request_stop());
            completed_.reset();
            pending_.emplace(std::move(request));
        }
        ready_.notify_one();
        return ProjectAssetResolutionSubmitResult::Accepted;
    } catch (const std::bad_alloc&) {
        return ProjectAssetResolutionSubmitResult::AllocationFailed;
    } catch (...) {
        return ProjectAssetResolutionSubmitResult::AllocationFailed;
    }
}

void ProjectAssetResolutionWorker::cancel() noexcept {
    {
        const std::scoped_lock lock{mutex_};
        pending_.reset();
        completed_.reset();
        static_cast<void>(active_stop_.request_stop());
    }
    ready_.notify_one();
}

std::optional<ProjectAssetResolutionCompletion>
ProjectAssetResolutionWorker::take_completed() noexcept {
    const std::scoped_lock lock{mutex_};
    if (!completed_.has_value()) {
        return std::nullopt;
    }
    std::optional<ProjectAssetResolutionCompletion> result{
        std::move(*completed_)};
    completed_.reset();
    return result;
}

bool ProjectAssetResolutionWorker::idle() const noexcept {
    const std::scoped_lock lock{mutex_};
    return !active_ && !pending_.has_value();
}

void ProjectAssetResolutionWorker::work() noexcept {
    for (;;) {
        std::optional<ProjectAssetResolutionRequest> request;
        std::stop_token stop;
        {
            std::unique_lock lock{mutex_};
            ready_.wait(lock, [this] {
                return stopping_ || pending_.has_value();
            });
            if (stopping_) {
                return;
            }
            request.emplace(std::move(*pending_));
            pending_.reset();
            active_stop_ = std::stop_source{};
            stop = active_stop_.get_token();
            active_ = true;
        }

        ProjectAssetResolutionCompletion completion{};
        completion.key = request->key;
        try {
            ResolvedProjectAssets resolved = *request->baseline;
            for (const RequiredProjectAsset& asset :
                 request->plan->manifest.assets) {
                if (stop.stop_requested()) {
                    break;
                }
                const bool check =
                    request->mode == ProjectAssetResolutionMode::Full
                    ? asset.content.identity.has_value()
                    : retryable(observation_for(resolved, asset.asset));
                if (!check) {
                    continue;
                }
                ProjectAssetLocalCheck local{};
                try {
                    local = checker_(asset, request->key.roots, stop);
                } catch (...) {
                    local = ProjectAssetLocalCheck{
                        asset.asset, LocalAssetState::Error, {}};
                }
                if (stop.stop_requested()) {
                    break;
                }
                apply_project_asset_check(
                    resolved, *request->plan, std::move(local));
            }
            if (!stop.stop_requested()) {
                completion.locally_usable =
                    resolved.all_required_locally_usable(
                        request->plan->manifest);
                completion.ready = resolved.all_required_verified(
                    request->plan->manifest);
                completion.resolved =
                    std::make_shared<ResolvedProjectAssets>(
                        std::move(resolved));
                completion.succeeded = true;
            }
        } catch (...) {
            completion.succeeded = false;
            completion.ready = false;
        }

        const std::scoped_lock lock{mutex_};
        active_ = false;
        if (!stopping_ && !stop.stop_requested() &&
            completion.key.request_id == latest_request_id_) {
            completed_.emplace(std::move(completion));
        }
    }
}

} // namespace mediaserver::app
