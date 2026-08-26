#pragma once

#include <mediaserver/editor/workspace_panels.hpp>
#include <mediaserver/net/cluster_lifecycle.hpp>
#include <mediaserver/topology/hardware_map.hpp>
#include <mediaserver/topology/hardware_runtime.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <vector>

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

struct ClusterLifecycleControllerConfig final {
    std::string shared_secret;
    std::chrono::milliseconds poll_interval{2'000};
    std::chrono::milliseconds machine_info_poll_interval{10'000};
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
        ClusterMachineInfoRequestFunction machine_info_request = {});
    ClusterLifecycleController(const ClusterLifecycleController&) = delete;
    ClusterLifecycleController& operator=(const ClusterLifecycleController&) =
        delete;
    ClusterLifecycleController(ClusterLifecycleController&&) = delete;
    ClusterLifecycleController& operator=(ClusterLifecycleController&&) =
        delete;
    ~ClusterLifecycleController();

    void sync(const topology::HardwareMap& map,
              core::u64 project_generation = 0U);
    [[nodiscard]] std::vector<editor::OutputMonitorState::Node> snapshot()
        const;
    [[nodiscard]] topology::HardwareRuntimeSnapshot hardware_snapshot() const;
    void submit(const editor::OutputLifecycleRequest& request);

    // Test/diagnostic observation only; production never waits for this on the
    // editor thread.
    [[nodiscard]] bool idle() const noexcept;

private:
    std::unique_ptr<detail::ClusterLifecycleControllerState> state_;
};

} // namespace mediaserver::app
