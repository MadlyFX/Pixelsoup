#pragma once

#include <mediaserver/net/cluster_lifecycle.hpp>

#include <memory>
#include <string_view>

namespace mediaserver::app {

// Optional Windows/NVIDIA implementation of the machine-level sync probe.
// It is intentionally separate from the RHI present-barrier backend: this
// probe runs in the lifecycle worker even when no render process is running.
class NvapiSyncStatusProvider {
public:
    NvapiSyncStatusProvider() = default;
    NvapiSyncStatusProvider(const NvapiSyncStatusProvider&) = delete;
    NvapiSyncStatusProvider& operator=(const NvapiSyncStatusProvider&) =
        delete;
    NvapiSyncStatusProvider(NvapiSyncStatusProvider&&) = delete;
    NvapiSyncStatusProvider& operator=(NvapiSyncStatusProvider&&) = delete;
    virtual ~NvapiSyncStatusProvider() = default;

    [[nodiscard]] virtual bool ready() const noexcept = 0;
    [[nodiscard]] virtual std::string_view detail() const noexcept = 0;
    [[nodiscard]] virtual net::ClusterSyncStatus collect(
        core::u64 sample_generation) noexcept = 0;
};

[[nodiscard]] std::shared_ptr<NvapiSyncStatusProvider>
create_nvapi_sync_status_provider() noexcept;

} // namespace mediaserver::app
