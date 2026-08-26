#pragma once

#include "project_asset_import.hpp"

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/core/types.hpp>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace mediaserver::app {

struct ProjectAssetImportJob final {
    core::u64 token{};
    core::u64 project_generation{};
    core::u64 gesture{};
    assets::Asset asset;
};

struct ProjectAssetImportCompletion final {
    core::u64 token{};
    core::u64 project_generation{};
    core::u64 gesture{};
    ProjectAssetImportResult result;
};

enum class ProjectAssetImportSubmitResult {
    Accepted,
    QueueFull,
    AllocationFailed,
    Stopping,
};

using ProjectAssetImportFunction = std::function<ProjectAssetImportResult(
    assets::Asset, const std::filesystem::path&, std::stop_token)>;

// One worker owns every hashing/copy operation. The capacity covers queued,
// active, and completed-but-not-yet-consumed work, so a stalled Director can
// never grow either side of the handoff without bound. Destruction requests
// cooperative cancellation and joins the thread; an OS/filesystem driver that
// does not release a blocked synchronous operation can still delay that join.
class ProjectAssetImportWorker final {
public:
    explicit ProjectAssetImportWorker(
        std::filesystem::path shared_store,
        std::size_t capacity,
        ProjectAssetImportFunction importer = {});
    ~ProjectAssetImportWorker();

    ProjectAssetImportWorker(const ProjectAssetImportWorker&) = delete;
    ProjectAssetImportWorker& operator=(
        const ProjectAssetImportWorker&) = delete;

    [[nodiscard]] ProjectAssetImportSubmitResult submit(
        ProjectAssetImportJob job) noexcept;
    [[nodiscard]] std::optional<ProjectAssetImportCompletion>
    take_completed() noexcept;
    // Drops queued and completed work immediately and requests cancellation
    // of the active hash/copy. The active slot remains outstanding until the
    // worker observes cancellation, and stale work never publishes a result.
    void discard_waiting() noexcept;

private:
    void work() noexcept;

    std::filesystem::path shared_store_;
    std::size_t capacity_{};
    std::size_t outstanding_{};
    bool stopping_{};
    bool active_{};
    bool active_discarded_{};
    std::stop_source active_stop_;
    ProjectAssetImportFunction importer_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<ProjectAssetImportJob> pending_;
    std::vector<std::optional<ProjectAssetImportCompletion>> completed_;
    std::size_t completed_head_{};
    std::size_t completed_tail_{};
    std::size_t completed_count_{};
    std::thread thread_;
};

} // namespace mediaserver::app
