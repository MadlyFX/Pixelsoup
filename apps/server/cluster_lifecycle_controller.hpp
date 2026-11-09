#pragma once

#include "project_assets.hpp"

#include <mediaserver/editor/live_data_panel.hpp>
#include <mediaserver/editor/workspace_panels.hpp>
#include <mediaserver/net/cluster_lifecycle.hpp>
#include <mediaserver/sync/sync_status.hpp>
#include <mediaserver/topology/hardware_map.hpp>
#include <mediaserver/topology/hardware_runtime.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace mediaserver::net {
class DirectorFrameBarrier;
} // namespace mediaserver::net

namespace mediaserver::app {

using ClusterLifecycleRequestResult =
    core::Result<net::ClusterLifecycleReply,
                 net::ClusterLifecycleClientError>;
using ClusterLifecycleRequestFunction = std::function<
    ClusterLifecycleRequestResult(const asio::ip::tcp::endpoint&, core::u32,
                                  net::ClusterLifecycleCommand)>;

using ClusterMachineInfoRequestResult =
    core::Result<net::ClusterMachineInfo,
                 net::ClusterLifecycleClientError>;
using ClusterMachineInfoRequestFunction = std::function<
    ClusterMachineInfoRequestResult(const asio::ip::tcp::endpoint&,
                                    core::u32)>;

using ClusterSyncStatusRequestResult =
    core::Result<net::ClusterSyncStatus, net::ClusterLifecycleClientError>;
using ClusterSyncStatusRequestFunction = std::function<
    ClusterSyncStatusRequestResult(const asio::ip::tcp::endpoint&, core::u32)>;

using ClusterAssetReadinessRequestResult =
    core::Result<net::ClusterAssetReadinessReply,
                 net::ClusterLifecycleClientError>;
using ClusterAssetReadinessRequestFunction = std::function<
    ClusterAssetReadinessRequestResult(
        const asio::ip::tcp::endpoint&, core::u32,
        std::span<const net::ClusterAssetRequirement>, std::stop_token)>;

using ClusterNodeLogRequestResult =
    core::Result<net::ClusterNodeLog, net::ClusterLifecycleClientError>;
using ClusterNodeLogRequestFunction = std::function<
    ClusterNodeLogRequestResult(const asio::ip::tcp::endpoint&, core::u32,
                                std::size_t)>;

struct ClusterLifecycleControllerConfig final {
    std::string shared_secret;
    std::chrono::milliseconds poll_interval{2'000};
    std::chrono::milliseconds machine_info_poll_interval{2'000};
    // Physical timing changes on a cable, not on an edit. It is polled on the
    // same authenticated connection as lifecycle status and kept brisk so a
    // lost reference reaches the operator surface promptly.
    std::chrono::milliseconds sync_status_poll_interval{2'000};
    std::chrono::milliseconds asset_readiness_poll_interval{30'000};
    // How many recent lines a FetchLog asks a node's agent for.
    std::size_t log_lines{400U};
    std::size_t worker_count{4U};
    net::ClusterLifecycleClientConfig client{};
};

namespace detail {
class ClusterLifecycleControllerState;
} // namespace detail

// Director-side asynchronous control plane. sync(), snapshot(), and submit()
// only touch bounded in-memory state; all socket work occurs on worker threads.
class ClusterLifecycleController final {
public:
    explicit ClusterLifecycleController(
        ClusterLifecycleControllerConfig config,
        ClusterLifecycleRequestFunction request = {},
        ClusterMachineInfoRequestFunction machine_info_request = {},
        ClusterAssetReadinessRequestFunction asset_readiness_request = {},
        ClusterSyncStatusRequestFunction sync_status_request = {},
        ClusterNodeLogRequestFunction node_log_request = {});
    ClusterLifecycleController(const ClusterLifecycleController&) = delete;
    ClusterLifecycleController& operator=(const ClusterLifecycleController&) =
        delete;
    ClusterLifecycleController(ClusterLifecycleController&&) = delete;
    ClusterLifecycleController& operator=(ClusterLifecycleController&&) =
        delete;
    ~ClusterLifecycleController();

    // Switches the key every request authenticates with - a render node
    // password changed in Preferences - and asks every node again at once.
    void set_shared_secret(std::string secret);

    void sync(const topology::HardwareMap& map,
              core::u64 project_generation = 0U);
    void sync(const topology::HardwareMap& map,
              const ProjectAssetManifest& manifest,
              core::u64 project_generation = 0U);
    [[nodiscard]] std::vector<editor::OutputMonitorState::Node> snapshot(
        const net::DirectorFrameBarrier* barrier = nullptr)
        const;
    [[nodiscard]] topology::HardwareRuntimeSnapshot hardware_snapshot() const;
    // Both halves of the physical timing picture, merged by render output
    // identity: the machine-local sync device from this controller, and the
    // present-barrier/frame accounting the render process acknowledged over
    // its session heartbeat. Passing no barrier yields the machine half only,
    // which the readiness gate then treats as unproven.
    [[nodiscard]] sync::SyncStatusSnapshot sync_snapshot(
        const net::DirectorFrameBarrier* barrier = nullptr) const;

    // What each configured render node last reported about the tracking it was
    // sent. Rows exist for every node, reported or not: a node that has never
    // answered is the state an operator most needs to see, and omitting its
    // row would hide it. Passing no barrier yields every row unreported.
    [[nodiscard]] std::vector<editor::NodeTrackingObservation>
    tracking_snapshot(const net::DirectorFrameBarrier* barrier = nullptr) const;
    // The Hardware Map name of the Render Server configured for a node index,
    // or empty when none is.
    [[nodiscard]] std::string node_name(core::u32 node_index) const;
    // FetchLog is answered on its own worker, beside lifecycle commands
    // rather than behind them: it works whatever state the node is in, and
    // a request while one is on its way for that node is ignored.
    void submit(const editor::OutputLifecycleRequest& request);
    // Fail-closed promotion guard for the exact manifest last passed to sync.
    // No configured Render Servers and a manifest with no required assets are
    // both vacuously ready.
    [[nodiscard]] bool content_ready(
        const ProjectAssetManifest& manifest) const noexcept;

    // Program promotion requires more than store presence: the exact
    // projected hardware configuration must be the one most recently synced,
    // and every configured render node must have adopted this manifest in its
    // live render runtime. The implementation deliberately acquires the
    // controller mutex before the barrier mutex; the barrier never calls back
    // into this controller, so this is the sole cross-component lock order.
    [[nodiscard]] bool program_ready(
        const ProjectAssetManifest& manifest,
        const topology::HardwareMap& projected_hardware_map,
        core::u64 project_generation,
        const net::DirectorFrameBarrier& barrier) const noexcept;

    // Test/diagnostic observation only; production never waits for this on the
    // editor thread.
    [[nodiscard]] bool idle() const noexcept;

private:
    std::unique_ptr<detail::ClusterLifecycleControllerState> state_;
};

} // namespace mediaserver::app
