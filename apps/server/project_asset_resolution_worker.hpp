#pragma once

#include "project_assets.hpp"

#include <mediaserver/core/types.hpp>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace mediaserver::app {

struct ProjectAssetResolutionKey final {
    core::u64 request_id{};
    core::u64 manifest_digest{};
    core::u64 project_generation{};
    assets::ContentResolutionRoots roots;

    bool operator==(const ProjectAssetResolutionKey&) const = default;
};

enum class ProjectAssetResolutionMode : core::u8 {
    Full,
    RetryUnresolved,
};

struct ProjectAssetResolutionRequest final {
    ProjectAssetResolutionKey key;
    std::shared_ptr<const ProjectAssetResolutionPlan> plan;
    std::shared_ptr<const ResolvedProjectAssets> baseline;
    ProjectAssetResolutionMode mode{ProjectAssetResolutionMode::Full};
};

struct ProjectAssetResolutionCompletion final {
    ProjectAssetResolutionKey key;
    std::shared_ptr<const ResolvedProjectAssets> resolved;
    bool succeeded{};
    bool locally_usable{};
    // Strict cluster adoption: portable shared-store manifest and every
    // required observation Verified.
    bool ready{};
};

enum class ProjectAssetResolutionSubmitResult : core::u8 {
    Accepted,
    InvalidRequest,
    AllocationFailed,
    Stopping,
};

using ProjectAssetCheckFunction = std::function<ProjectAssetLocalCheck(
    const RequiredProjectAsset&, const assets::ContentResolutionRoots&,
    std::stop_token)>;

// One persistent thread owns every initial, full, and retry verification.
// There is one active request, one latest-wins pending slot, and one completion
// slot. Superseding work requests cooperative cancellation; the filesystem
// layer checks the token at bounded stream/copy checkpoints.
class ProjectAssetResolutionWorker final {
public:
    explicit ProjectAssetResolutionWorker(
        ProjectAssetCheckFunction checker = {});
    ~ProjectAssetResolutionWorker();

    ProjectAssetResolutionWorker(const ProjectAssetResolutionWorker&) = delete;
    ProjectAssetResolutionWorker& operator=(
        const ProjectAssetResolutionWorker&) = delete;

    [[nodiscard]] ProjectAssetResolutionSubmitResult submit(
        ProjectAssetResolutionRequest request) noexcept;
    void cancel() noexcept;
    [[nodiscard]] std::optional<ProjectAssetResolutionCompletion>
    take_completed() noexcept;
    [[nodiscard]] bool idle() const noexcept;

private:
    void work() noexcept;

    ProjectAssetCheckFunction checker_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<ProjectAssetResolutionRequest> pending_;
    std::optional<ProjectAssetResolutionCompletion> completed_;
    std::stop_source active_stop_;
    core::u64 latest_request_id_{};
    bool active_{};
    bool stopping_{};
    std::thread thread_;
};

} // namespace mediaserver::app
