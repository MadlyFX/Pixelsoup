#include "../cluster_lifecycle_controller.hpp"

#include <asio/ip/tcp.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace app = mediaserver::app;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace net = mediaserver::net;
namespace topology = mediaserver::topology;
using namespace std::chrono_literals;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line) +
            ": check failed: " + expression};
    }
}

#define CONTROLLER_CHECK(expression) \
    check(static_cast<bool>(expression), #expression, __LINE__)

struct ObservedRequest final {
    core::u32 node_index{};
    net::ClusterLifecycleCommand command{};
};

[[nodiscard]] topology::HardwareDevice render_server(
    core::u64 id, core::u32 node_index, std::string endpoint) {
    topology::HardwareDevice device{};
    device.node = doc::NodeId{id};
    device.kind = topology::HardwareDeviceKind::RenderServer;
    device.name = "Render " + std::to_string(node_index);
    device.lifecycle_endpoint = std::move(endpoint);
    device.node_index = node_index;
    device.width = 1920U;
    device.height = 1080U;
    return device;
}

[[nodiscard]] app::ClusterLifecycleControllerConfig controller_config() {
    app::ClusterLifecycleControllerConfig config{};
    config.shared_secret = "0123456789abcdef0123456789abcdef";
    config.poll_interval = 1h;
    config.worker_count = 2U;
    config.client.connect_timeout = 100ms;
    config.client.handshake_timeout = 100ms;
    config.client.write_timeout = 100ms;
    return config;
}

[[nodiscard]] bool wait_idle(app::ClusterLifecycleController& controller) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (controller.idle()) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return controller.idle();
}

void sync_never_waits_for_a_dead_or_slow_listener() {
    std::atomic_size_t calls{};
    app::ClusterLifecycleController controller{
        controller_config(),
        [&](const asio::ip::tcp::endpoint&, core::u32 node_index,
            net::ClusterLifecycleCommand) {
            ++calls;
            std::this_thread::sleep_for(150ms);
            return app::ClusterLifecycleRequestResult::success(
                net::ClusterLifecycleReply{
                    net::ClusterLifecycleOutcome::NoChange,
                    net::ClusterProcessState::Running, node_index, 8U,
                    "process running"});
        }};
    topology::HardwareMap map{};
    map.devices.push_back(render_server(1U, 0U, "127.0.0.1:47840"));

    const auto begin = std::chrono::steady_clock::now();
    controller.sync(map);
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    CONTROLLER_CHECK(elapsed < 50ms);
    const auto pending = controller.snapshot();
    CONTROLLER_CHECK(pending.size() == 1U);
    CONTROLLER_CHECK(
        pending[0U].status ==
        editor::OutputMonitorState::Node::Status::Probing);
    CONTROLLER_CHECK(wait_idle(controller));
    const auto complete = controller.snapshot();
    CONTROLLER_CHECK(
        complete[0U].status ==
        editor::OutputMonitorState::Node::Status::Running);
    CONTROLLER_CHECK(complete[0U].generation == 8U);
    CONTROLLER_CHECK(calls == 1U);
}

void project_control_settings_derive_each_listener_endpoint() {
    struct ObservedEndpoint final {
        core::u32 node_index{};
        std::string address;
        core::u16 port{};
    };
    std::mutex observed_mutex;
    std::vector<ObservedEndpoint> observed;
    app::ClusterLifecycleController controller{
        controller_config(),
        [&](const asio::ip::tcp::endpoint& endpoint, core::u32 node_index,
            net::ClusterLifecycleCommand) {
            {
                const std::lock_guard lock{observed_mutex};
                observed.push_back(ObservedEndpoint{
                    node_index, endpoint.address().to_string(),
                    endpoint.port()});
            }
            return app::ClusterLifecycleRequestResult::success(
                net::ClusterLifecycleReply{
                    net::ClusterLifecycleOutcome::NoChange,
                    net::ClusterProcessState::Running, node_index, 1U,
                    "running"});
        }};

    topology::HardwareMap map{};
    map.control = topology::HardwareControlSettings{
        doc::NodeId{90U}, 50'000U, true};
    auto local = render_server(1U, 3U, "192.0.2.200:1234");
    local.address.clear();
    map.devices.push_back(std::move(local));
    auto remote = render_server(2U, 4U, "192.0.2.201:1234");
    remote.address = "192.0.2.25";
    map.devices.push_back(std::move(remote));

    controller.sync(map);
    CONTROLLER_CHECK(wait_idle(controller));
    const std::lock_guard lock{observed_mutex};
    CONTROLLER_CHECK(observed.size() == 2U);
    const auto endpoint_for = [&](core::u32 node_index)
        -> const ObservedEndpoint* {
        for (const ObservedEndpoint& endpoint : observed) {
            if (endpoint.node_index == node_index) {
                return &endpoint;
            }
        }
        return nullptr;
    };
    const ObservedEndpoint* const local_endpoint = endpoint_for(3U);
    const ObservedEndpoint* const remote_endpoint = endpoint_for(4U);
    CONTROLLER_CHECK(local_endpoint != nullptr);
    CONTROLLER_CHECK(local_endpoint->address == "127.0.0.1");
    CONTROLLER_CHECK(local_endpoint->port == 50'003U);
    CONTROLLER_CHECK(remote_endpoint != nullptr);
    CONTROLLER_CHECK(remote_endpoint->address == "192.0.2.25");
    CONTROLLER_CHECK(remote_endpoint->port == 50'004U);
}

void duplicates_are_disabled_and_cluster_actions_skip_them() {
    std::mutex observed_mutex;
    std::vector<ObservedRequest> observed;
    app::ClusterLifecycleController controller{
        controller_config(),
        [&](const asio::ip::tcp::endpoint&, core::u32 node_index,
            net::ClusterLifecycleCommand command) {
            {
                const std::lock_guard lock{observed_mutex};
                observed.push_back({node_index, command});
            }
            return app::ClusterLifecycleRequestResult::success(
                net::ClusterLifecycleReply{
                    net::ClusterLifecycleOutcome::NoChange,
                    net::ClusterProcessState::Running, node_index, 1U,
                    "running"});
        }};
    topology::HardwareMap map{};
    map.devices.push_back(render_server(1U, 0U, "127.0.0.1:47840"));
    map.devices.push_back(render_server(2U, 0U, "127.0.0.1:47841"));
    map.devices.push_back(render_server(3U, 2U, "127.0.0.1:47842"));
    controller.sync(map);
    CONTROLLER_CHECK(wait_idle(controller));
    auto rows = controller.snapshot();
    CONTROLLER_CHECK(rows.size() == 3U);
    CONTROLLER_CHECK(rows[0U].status ==
                     editor::OutputMonitorState::Node::Status::DuplicateNodeIndex);
    CONTROLLER_CHECK(rows[1U].status ==
                     editor::OutputMonitorState::Node::Status::DuplicateNodeIndex);
    {
        const std::lock_guard lock{observed_mutex};
        CONTROLLER_CHECK(observed.size() == 1U);
        CONTROLLER_CHECK(observed[0U].node_index == 2U);
        observed.clear();
    }

    controller.submit(editor::OutputLifecycleRequest{
        {}, 0U, {}, editor::OutputLifecycleAction::Restart, true});
    CONTROLLER_CHECK(wait_idle(controller));
    {
        const std::lock_guard lock{observed_mutex};
        CONTROLLER_CHECK(observed.size() == 1U);
        CONTROLLER_CHECK(observed[0U].node_index == 2U);
        CONTROLLER_CHECK(observed[0U].command ==
                         net::ClusterLifecycleCommand::Restart);
    }

    // A forged/stale per-row click cannot address either duplicate.
    controller.submit(editor::OutputLifecycleRequest{
        doc::NodeId{1U}, 0U, "127.0.0.1:47840",
        editor::OutputLifecycleAction::Start, false});
    CONTROLLER_CHECK(controller.idle());
}

void a_duplicate_transition_rejects_an_in_flight_stale_reply() {
    std::atomic_bool release{};
    app::ClusterLifecycleController controller{
        controller_config(),
        [&](const asio::ip::tcp::endpoint&, core::u32 node_index,
            net::ClusterLifecycleCommand) {
            while (!release.load()) {
                std::this_thread::sleep_for(1ms);
            }
            return app::ClusterLifecycleRequestResult::success(
                net::ClusterLifecycleReply{
                    net::ClusterLifecycleOutcome::NoChange,
                    net::ClusterProcessState::Running, node_index, 9U,
                    "stale running"});
        }};
    topology::HardwareMap map{};
    map.devices.push_back(render_server(1U, 0U, "127.0.0.1:47840"));
    controller.sync(map);
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (controller.idle() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }

    map.devices.push_back(render_server(2U, 0U, "127.0.0.1:47841"));
    controller.sync(map);
    release.store(true);
    CONTROLLER_CHECK(wait_idle(controller));
    const auto rows = controller.snapshot();
    CONTROLLER_CHECK(rows.size() == 2U);
    CONTROLLER_CHECK(rows[0U].status ==
                     editor::OutputMonitorState::Node::Status::DuplicateNodeIndex);
    CONTROLLER_CHECK(rows[0U].generation == 0U);
}

void overlapping_mutations_are_not_sent_out_of_order() {
    std::atomic_bool release{};
    std::mutex observed_mutex;
    std::vector<ObservedRequest> observed;
    app::ClusterLifecycleController controller{
        controller_config(),
        [&](const asio::ip::tcp::endpoint&, core::u32 node_index,
            net::ClusterLifecycleCommand command) {
            {
                const std::lock_guard lock{observed_mutex};
                observed.push_back({node_index, command});
            }
            if (command == net::ClusterLifecycleCommand::Restart) {
                while (!release.load()) {
                    std::this_thread::sleep_for(1ms);
                }
            }
            return app::ClusterLifecycleRequestResult::success(
                net::ClusterLifecycleReply{
                    net::ClusterLifecycleOutcome::Applied,
                    net::ClusterProcessState::Running, node_index, 2U,
                    "complete"});
        }};
    topology::HardwareMap map{};
    map.devices.push_back(render_server(1U, 0U, "127.0.0.1:47840"));
    controller.sync(map);
    CONTROLLER_CHECK(wait_idle(controller));
    {
        const std::lock_guard lock{observed_mutex};
        observed.clear();
    }
    const editor::OutputLifecycleRequest restart{
        doc::NodeId{1U}, 0U, "127.0.0.1:47840",
        editor::OutputLifecycleAction::Restart, false};
    controller.submit(restart);
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    for (;;) {
        {
            const std::lock_guard lock{observed_mutex};
            if (!observed.empty()) {
                break;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    controller.submit(editor::OutputLifecycleRequest{
        doc::NodeId{1U}, 0U, "127.0.0.1:47840",
        editor::OutputLifecycleAction::Stop, false});
    release.store(true);
    CONTROLLER_CHECK(wait_idle(controller));
    const std::lock_guard lock{observed_mutex};
    CONTROLLER_CHECK(observed.size() == 1U);
    CONTROLLER_CHECK(observed[0U].command ==
                     net::ClusterLifecycleCommand::Restart);
}

void project_generation_fences_identical_target_replies() {
    std::atomic_size_t call_index{};
    std::atomic_bool release_first{};
    app::ClusterLifecycleController controller{
        controller_config(),
        [&](const asio::ip::tcp::endpoint&, core::u32 node_index,
            net::ClusterLifecycleCommand) {
            const std::size_t call = call_index.fetch_add(1U);
            if (call == 0U) {
                while (!release_first.load()) {
                    std::this_thread::sleep_for(1ms);
                }
            }
            return app::ClusterLifecycleRequestResult::success(
                net::ClusterLifecycleReply{
                    net::ClusterLifecycleOutcome::NoChange,
                    call == 0U ? net::ClusterProcessState::Faulted
                               : net::ClusterProcessState::Running,
                    node_index, call == 0U ? 99U : 2U,
                    call == 0U ? "old project" : "new project"});
        }};
    topology::HardwareMap map{};
    map.devices.push_back(render_server(1U, 0U, "127.0.0.1:47840"));
    controller.sync(map, 10U);
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (call_index.load() == 0U &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CONTROLLER_CHECK(call_index.load() == 1U);
    // The next project deliberately reuses every persisted target value.
    controller.sync(map, 11U);
    release_first.store(true);
    CONTROLLER_CHECK(wait_idle(controller));
    const auto rows = controller.snapshot();
    CONTROLLER_CHECK(rows.size() == 1U);
    CONTROLLER_CHECK(rows[0U].status ==
                     editor::OutputMonitorState::Node::Status::Running);
    CONTROLLER_CHECK(rows[0U].generation == 2U);
    CONTROLLER_CHECK(rows[0U].detail == "new project");
}

void machine_inventory_is_transient_retained_and_project_fenced() {
    auto config = controller_config();
    config.machine_info_poll_interval = 1ms;
    std::atomic_bool fail_inventory{};
    std::atomic_size_t inventory_calls{};
    app::ClusterLifecycleController controller{
        config,
        [](const asio::ip::tcp::endpoint&, core::u32 node_index,
           net::ClusterLifecycleCommand) {
            return app::ClusterLifecycleRequestResult::success(
                net::ClusterLifecycleReply{
                    net::ClusterLifecycleOutcome::NoChange,
                    net::ClusterProcessState::Stopped, node_index, 3U,
                    "listener online; process stopped"});
        },
        [&](const asio::ip::tcp::endpoint&, core::u32 node_index) {
            ++inventory_calls;
            if (fail_inventory.load()) {
                return app::ClusterMachineInfoRequestResult::failure(
                    {net::ClusterLifecycleClientErrorCode::TimedOut, {}});
            }
            net::ClusterMachineInfo info{};
            info.availability =
                net::ClusterMachineInfoAvailability::Available;
            info.display_state =
                net::ClusterDisplayEnumerationState::Available;
            info.node_index = node_index;
            info.logical_cpu_count = 24U;
            info.physical_memory_bytes = 64ULL * 1024ULL * 1024ULL * 1024ULL;
            info.host_name = "render-a";
            info.operating_system = "Windows";
            info.adapters.push_back(net::ClusterGpuAdapterInfo{
                7U, 0x10deU, 0x2684U, 0U, 1U,
                24ULL * 1024ULL * 1024ULL * 1024ULL, 0U, false, false,
                "GPU A"});
            info.display_targets.push_back(net::ClusterDisplayTargetInfo{
                7U, 2U, 0U, 0U, 0U,
                net::ClusterVideoOutputTechnology::DisplayPort,
                true, true, 3840U, 2160U, 60U, 1U, "LED Processor"});
            return app::ClusterMachineInfoRequestResult::success(
                std::move(info));
        }};
    topology::HardwareMap map{};
    map.devices.push_back(render_server(1U, 0U, "127.0.0.1:47840"));

    controller.sync(map, 4U);
    CONTROLLER_CHECK(wait_idle(controller));
    auto hardware = controller.hardware_snapshot();
    CONTROLLER_CHECK(hardware.nodes.size() == 1U);
    CONTROLLER_CHECK(
        hardware.nodes[0U].reachability ==
        topology::HardwareRuntimeReachability::Online);
    CONTROLLER_CHECK(
        hardware.nodes[0U].process ==
        topology::HardwareRuntimeProcessState::Stopped);
    CONTROLLER_CHECK(hardware.nodes[0U].hostname == "render-a");
    CONTROLLER_CHECK(
        hardware.nodes[0U].inventory ==
        topology::HardwareRuntimeInventoryState::Available);
    CONTROLLER_CHECK(hardware.nodes[0U].logical_processor_count == 24U);
    CONTROLLER_CHECK(hardware.nodes[0U].gpu_adapters.size() == 1U);
    CONTROLLER_CHECK(hardware.nodes[0U].gpu_adapters[0U].name == "GPU A");
    CONTROLLER_CHECK(hardware.nodes[0U].video_outputs.size() == 1U);
    CONTROLLER_CHECK(hardware.nodes[0U].video_outputs[0U].connector ==
                     "DISPLAYPORT");
    CONTROLLER_CHECK(!hardware.nodes[0U].stale);

    std::this_thread::sleep_for(3ms);
    fail_inventory.store(true);
    controller.submit(editor::OutputLifecycleRequest{
        doc::NodeId{1U}, 0U, "127.0.0.1:47840",
        editor::OutputLifecycleAction::Status, false});
    CONTROLLER_CHECK(wait_idle(controller));
    hardware = controller.hardware_snapshot();
    CONTROLLER_CHECK(hardware.nodes[0U].hostname == "render-a");
    CONTROLLER_CHECK(hardware.nodes[0U].video_outputs.size() == 1U);
    CONTROLLER_CHECK(hardware.nodes[0U].stale);

    // An identical document identity in a newly loaded project must not
    // inherit the previous project's last-known machine report.
    controller.sync(map, 5U);
    CONTROLLER_CHECK(wait_idle(controller));
    hardware = controller.hardware_snapshot();
    CONTROLLER_CHECK(hardware.nodes.size() == 1U);
    CONTROLLER_CHECK(hardware.nodes[0U].hostname.empty());
    CONTROLLER_CHECK(hardware.nodes[0U].video_outputs.empty());
    CONTROLLER_CHECK(
        hardware.nodes[0U].inventory ==
        topology::HardwareRuntimeInventoryState::Failed);
    CONTROLLER_CHECK(!hardware.nodes[0U].stale);
    CONTROLLER_CHECK(inventory_calls.load() >= 3U);
}

void inventory_for_another_node_is_rejected_and_latched() {
    auto config = controller_config();
    config.machine_info_poll_interval = 1h;
    app::ClusterLifecycleController controller{
        config,
        [](const asio::ip::tcp::endpoint&, core::u32 node_index,
           net::ClusterLifecycleCommand) {
            return app::ClusterLifecycleRequestResult::success(
                net::ClusterLifecycleReply{
                    net::ClusterLifecycleOutcome::NoChange,
                    net::ClusterProcessState::Running, node_index, 4U,
                    "lifecycle status succeeded"});
        },
        [](const asio::ip::tcp::endpoint&, core::u32 node_index) {
            net::ClusterMachineInfo wrong{};
            wrong.availability =
                net::ClusterMachineInfoAvailability::Available;
            wrong.display_state =
                net::ClusterDisplayEnumerationState::Available;
            wrong.node_index = node_index + 1U;
            wrong.host_name = "wrong-machine";
            wrong.adapters.push_back(net::ClusterGpuAdapterInfo{
                1U, 0U, 0U, 0U, 0U, 0U, 0U, false, false,
                "must not be disclosed"});
            return app::ClusterMachineInfoRequestResult::success(
                std::move(wrong));
        }};
    topology::HardwareMap map{};
    map.devices.push_back(render_server(1U, 7U, "127.0.0.1:47847"));

    controller.sync(map);
    CONTROLLER_CHECK(wait_idle(controller));
    auto rows = controller.snapshot();
    CONTROLLER_CHECK(rows.size() == 1U);
    CONTROLLER_CHECK(
        rows[0U].status ==
        editor::OutputMonitorState::Node::Status::ConfigurationMismatch);
    CONTROLLER_CHECK(rows[0U].detail.find("different node index") !=
                     std::string::npos);
    auto hardware = controller.hardware_snapshot();
    CONTROLLER_CHECK(hardware.nodes.size() == 1U);
    CONTROLLER_CHECK(hardware.nodes[0U].hostname.empty());
    CONTROLLER_CHECK(hardware.nodes[0U].gpu_adapters.empty());
    CONTROLLER_CHECK(
        hardware.nodes[0U].inventory ==
        topology::HardwareRuntimeInventoryState::Unknown);

    // A later ordinary Status success cannot erase the identity fault. Only a
    // matching authenticated inventory response may clear the latch.
    controller.submit(editor::OutputLifecycleRequest{
        doc::NodeId{1U}, 7U, "127.0.0.1:47847",
        editor::OutputLifecycleAction::Status, false});
    CONTROLLER_CHECK(wait_idle(controller));
    rows = controller.snapshot();
    CONTROLLER_CHECK(
        rows[0U].status ==
        editor::OutputMonitorState::Node::Status::ConfigurationMismatch);
}

void fresh_lifecycle_failures_override_cached_inventory_detail() {
    auto config = controller_config();
    config.machine_info_poll_interval = 1h;
    std::atomic_int failure_mode{};
    app::ClusterLifecycleController controller{
        config,
        [&](const asio::ip::tcp::endpoint&, core::u32 node_index,
            net::ClusterLifecycleCommand) {
            const int mode = failure_mode.load();
            if (mode == 1) {
                return app::ClusterLifecycleRequestResult::failure(
                    {net::ClusterLifecycleClientErrorCode::
                         AuthenticationFailed,
                     {}});
            }
            if (mode == 2) {
                return app::ClusterLifecycleRequestResult::failure(
                    {net::ClusterLifecycleClientErrorCode::ConnectFailed,
                     {}});
            }
            return app::ClusterLifecycleRequestResult::success(
                net::ClusterLifecycleReply{
                    net::ClusterLifecycleOutcome::NoChange,
                    net::ClusterProcessState::Stopped, node_index, 2U,
                    "listener online"});
        },
        [](const asio::ip::tcp::endpoint&, core::u32 node_index) {
            net::ClusterMachineInfo info{};
            info.availability =
                net::ClusterMachineInfoAvailability::Available;
            info.display_state =
                net::ClusterDisplayEnumerationState::Available;
            info.node_index = node_index;
            info.host_name = "render-a";
            info.message = "cached inventory is healthy";
            return app::ClusterMachineInfoRequestResult::success(
                std::move(info));
        }};
    topology::HardwareMap map{};
    map.devices.push_back(render_server(1U, 0U, "127.0.0.1:47840"));

    controller.sync(map);
    CONTROLLER_CHECK(wait_idle(controller));
    auto hardware = controller.hardware_snapshot();
    CONTROLLER_CHECK(hardware.nodes[0U].detail ==
                     "cached inventory is healthy");

    failure_mode.store(1);
    controller.submit(editor::OutputLifecycleRequest{
        doc::NodeId{1U}, 0U, "127.0.0.1:47840",
        editor::OutputLifecycleAction::Status, false});
    CONTROLLER_CHECK(wait_idle(controller));
    hardware = controller.hardware_snapshot();
    CONTROLLER_CHECK(hardware.nodes[0U].detail ==
                     "shared secret rejected by listener");
    CONTROLLER_CHECK(hardware.nodes[0U].stale);
    CONTROLLER_CHECK(hardware.nodes[0U].hostname == "render-a");

    failure_mode.store(2);
    controller.submit(editor::OutputLifecycleRequest{
        doc::NodeId{1U}, 0U, "127.0.0.1:47840",
        editor::OutputLifecycleAction::Status, false});
    CONTROLLER_CHECK(wait_idle(controller));
    hardware = controller.hardware_snapshot();
    CONTROLLER_CHECK(hardware.nodes[0U].detail ==
                     "listener connection failed");
    CONTROLLER_CHECK(hardware.nodes[0U].stale);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"sync is nonblocking", &sync_never_waits_for_a_dead_or_slow_listener},
        {"project control settings derive listener endpoints",
         &project_control_settings_derive_each_listener_endpoint},
        {"duplicates disable and batch skips",
         &duplicates_are_disabled_and_cluster_actions_skip_them},
        {"duplicate transition rejects stale reply",
         &a_duplicate_transition_rejects_an_in_flight_stale_reply},
        {"overlapping mutations are suppressed",
         &overlapping_mutations_are_not_sent_out_of_order},
        {"project generation fences identical targets",
         &project_generation_fences_identical_target_replies},
        {"machine inventory is transient and project fenced",
         &machine_inventory_is_transient_retained_and_project_fenced},
        {"inventory for another node is rejected and latched",
         &inventory_for_another_node_is_rejected_and_latched},
        {"fresh lifecycle failures override cached inventory detail",
         &fresh_lifecycle_failures_override_cached_inventory_detail},
    };
    std::size_t failures{};
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[pass] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
