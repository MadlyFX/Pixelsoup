#include "../cluster_lifecycle_controller.hpp"

#include <asio/ip/tcp.hpp>

#include <mediaserver/net/cluster_lifecycle.hpp>
#include <mediaserver/net/director_frame_barrier.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <array>
#include <chrono>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace app = mediaserver::app;
namespace command = mediaserver::cmd;
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

// The controller drives only cabled Render Servers, so test maps run their
// servers' control cables from one operator machine, as a real project does.
// Deterministic, so the same map always wires to the same digest.
[[nodiscard]] topology::HardwareMap wired(topology::HardwareMap map) {
    constexpr core::u64 operator_id = 0xFFFF'0000U;
    topology::HardwareDevice operator_machine{};
    operator_machine.node = doc::NodeId{operator_id};
    operator_machine.kind = topology::HardwareDeviceKind::OperatorMachine;
    operator_machine.name = "Operator";
    core::u64 link_id = 0xFFFF'1000U;
    for (const topology::HardwareDevice& device : map.devices) {
        if (device.kind != topology::HardwareDeviceKind::RenderServer) {
            continue;
        }
        map.links.push_back(topology::HardwareLink{
            doc::NodeId{link_id++},
            topology::HardwareEndpoint{operator_machine.node, 0U},
            topology::HardwareEndpoint{device.node, 0U}});
    }
    map.devices.push_back(std::move(operator_machine));
    return map;
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

[[nodiscard]] app::ProjectAssetManifest portable_manifest(
    std::size_t count = 1U, core::u64 digest = 71U) {
    app::ProjectAssetManifest manifest{};
    manifest.digest = digest;
    for (std::size_t index = 0U; index < count; ++index) {
#if defined(_WIN32)
        const std::string root{"D:\\show\\"};
#else
        const std::string root{"/show/"};
#endif
        manifest.assets.push_back(app::RequiredProjectAsset{
            doc::NodeId{100U + index},
            mediaserver::assets::AssetKind::Media,
            root + "asset-" + std::to_string(index) + ".mov"});
    }
    return manifest;
}

[[nodiscard]] app::ClusterMachineInfoRequestResult no_inventory(
    const asio::ip::tcp::endpoint&, core::u32 node_index) {
    net::ClusterMachineInfo info{};
    info.availability = net::ClusterMachineInfoAvailability::Unavailable;
    info.node_index = node_index;
    return app::ClusterMachineInfoRequestResult::success(std::move(info));
}

[[nodiscard]] app::ClusterAssetReadinessRequestResult all_assets_ready(
    const asio::ip::tcp::endpoint&,
    core::u32 node_index,
    std::span<const net::ClusterAssetRequirement> sources,
    std::stop_token) {
    net::ClusterAssetReadinessReply reply{};
    reply.availability =
        net::ClusterAssetReadinessAvailability::Available;
    reply.node_index = node_index;
    for (const net::ClusterAssetRequirement& source : sources) {
        reply.entries.push_back(net::ClusterAssetReadinessEntry{
            source.source, net::ClusterAssetReadinessState::Ready});
        ++reply.ready;
    }
    return app::ClusterAssetReadinessRequestResult::success(
        std::move(reply));
}

[[nodiscard]] command::StreamCursor genesis_cursor() noexcept {
    return command::StreamCursor{
        command::FrameNumber{0U},
        mediaserver::doc::Version{0U},
        command::CommandId{1U}};
}

[[nodiscard]] net::Id128 runtime_id(core::u64 low) noexcept {
    return net::Id128{0x636c757374657221ULL, low};
}

[[nodiscard]] net::DirectorLiveRegistration runtime_registration(
    core::u64 session,
    core::u64 node,
    core::u32 node_index) noexcept {
    return net::DirectorLiveRegistration{
        runtime_id(session),
        net::NodeSessionIdentity{
            runtime_id(1U), runtime_id(node), runtime_id(session + 100U)},
        node_index,
        genesis_cursor(),
        1U};
}

void publish_runtime_adoption(
    net::DirectorFrameBarrier& barrier,
    core::u64 session,
    core::u64 acknowledgement_sequence,
    core::u64 manifest_digest,
    bool ready = true) {
    CONTROLLER_CHECK(barrier.on_heartbeat_ack(
        runtime_id(session),
        net::HeartbeatAcknowledgementReport{
            acknowledgement_sequence,
            900U + acknowledgement_sequence,
            genesis_cursor(),
            net::RuntimeAssetAdoption{manifest_digest, ready}}));
}

// The controller carries the machine half of the physical timing picture and
// merges the render-runtime half from the barrier. Neither half alone may
// claim a lock, and a reply for another node identity must be discarded.
void sync_status_merges_the_machine_and_runtime_halves() {
    std::atomic_size_t sync_calls{};
    app::ClusterLifecycleController controller{
        controller_config(),
        [&](const asio::ip::tcp::endpoint&, core::u32 node_index,
            net::ClusterLifecycleCommand) {
            return app::ClusterLifecycleRequestResult::success(
                net::ClusterLifecycleReply{
                    net::ClusterLifecycleOutcome::NoChange,
                    net::ClusterProcessState::Running, node_index, 1U,
                    "process running"});
        },
        {},
        {},
        [&](const asio::ip::tcp::endpoint&, core::u32 node_index) {
            ++sync_calls;
            net::ClusterSyncStatus status{};
            status.availability =
                net::ClusterSyncStatusAvailability::Available;
            status.board = net::ClusterSyncDeviceAvailability::Present;
            status.reference = net::ClusterSyncReferenceState::InUse;
            status.node_index = node_index;
            status.sample_generation = 5U;
            status.detail = "reference in use";
            status.outputs.push_back(net::ClusterSyncOutputStatus{
                0x1234U, 3U, 1U, 3840U, 2160U, 60'000U, 1'001U,
                net::ClusterSyncLockState::Locked, true});
            return app::ClusterSyncStatusRequestResult::success(
                std::move(status));
        }};

    topology::HardwareMap map{};
    map.devices.push_back(render_server(1U, 0U, "127.0.0.1:47840"));
    controller.sync(wired(map));
    CONTROLLER_CHECK(wait_idle(controller));
    CONTROLLER_CHECK(sync_calls >= 1U);

    // Without a barrier only the machine half is known, which the readiness
    // gate must treat as unproven rather than as an unlocked-but-known state.
    const auto machine_only = controller.sync_snapshot();
    CONTROLLER_CHECK(machine_only.nodes.size() == 1U);
    const mediaserver::sync::NodeSyncStatus* observed =
        machine_only.node(0U);
    CONTROLLER_CHECK(observed != nullptr);
    CONTROLLER_CHECK(observed->machine_reported);
    CONTROLLER_CHECK(!observed->runtime_reported);
    CONTROLLER_CHECK(observed->board ==
                     mediaserver::sync::SyncDeviceAvailability::Present);
    CONTROLLER_CHECK(observed->reference ==
                     mediaserver::sync::SyncReferenceState::InUse);
    CONTROLLER_CHECK(observed->outputs.size() == 1U);
    CONTROLLER_CHECK(observed->outputs.front().lock ==
                     mediaserver::sync::SyncLockState::Locked);
    CONTROLLER_CHECK(observed->outputs.front().target ==
                     (topology::HardwareDisplayTargetKey{0x1234U, 3U, 1U}));
    CONTROLLER_CHECK(observed->barrier ==
                     mediaserver::sync::PresentBarrierState::Unknown);
    CONTROLLER_CHECK(!observed->simulated);

    net::DirectorFrameBarrier barrier{genesis_cursor()};
    CONTROLLER_CHECK(barrier.register_live(runtime_registration(10U, 100U, 0U)));
    const net::HeartbeatAcknowledgementReport acknowledgement{
        2U, 902U, genesis_cursor(), net::RuntimeAssetAdoption{7U, true},
        net::RuntimeOutputSync{net::RuntimeBarrierState::Joined, 3U, 4U, 1U,
                               1U, 500U, 2U, 1U, 6U, 3U, 1U}};
    CONTROLLER_CHECK(barrier.on_heartbeat_ack(runtime_id(10U),
                                              acknowledgement));

    const auto merged = controller.sync_snapshot(&barrier);
    observed = merged.node(0U);
    CONTROLLER_CHECK(observed != nullptr);
    CONTROLLER_CHECK(observed->machine_reported);
    CONTROLLER_CHECK(observed->runtime_reported);
    CONTROLLER_CHECK(observed->barrier ==
                     mediaserver::sync::PresentBarrierState::Joined);
    CONTROLLER_CHECK(observed->barrier_group == 3U);
    CONTROLLER_CHECK(observed->barrier_id == 4U);
    CONTROLLER_CHECK(observed->barrier_joined_outputs == 1U);
    CONTROLLER_CHECK(observed->barrier_expected_outputs == 1U);
    CONTROLLER_CHECK(observed->counters.presented == 500U);
    CONTROLLER_CHECK(observed->counters.missed == 2U);
    CONTROLLER_CHECK(observed->counters.late == 1U);
    CONTROLLER_CHECK(observed->counters.held == 6U);
    CONTROLLER_CHECK(observed->counters.repeated == 3U);
    CONTROLLER_CHECK(observed->counters.loss_of_sync_events == 1U);
}

void sync_status_for_another_node_identity_is_discarded() {
    app::ClusterLifecycleController controller{
        controller_config(),
        [&](const asio::ip::tcp::endpoint&, core::u32 node_index,
            net::ClusterLifecycleCommand) {
            return app::ClusterLifecycleRequestResult::success(
                net::ClusterLifecycleReply{
                    net::ClusterLifecycleOutcome::NoChange,
                    net::ClusterProcessState::Running, node_index, 1U,
                    "process running"});
        },
        {},
        {},
        [&](const asio::ip::tcp::endpoint&, core::u32) {
            net::ClusterSyncStatus status{};
            status.availability =
                net::ClusterSyncStatusAvailability::Available;
            status.board = net::ClusterSyncDeviceAvailability::Present;
            status.reference = net::ClusterSyncReferenceState::InUse;
            // Deliberately the wrong listener identity.
            status.node_index = 9U;
            status.outputs.push_back(net::ClusterSyncOutputStatus{
                0x1234U, 3U, 1U, 1920U, 1080U, 60U, 1U,
                net::ClusterSyncLockState::Locked, true});
            return app::ClusterSyncStatusRequestResult::success(
                std::move(status));
        }};

    topology::HardwareMap map{};
    map.devices.push_back(render_server(1U, 0U, "127.0.0.1:47840"));
    controller.sync(wired(map));
    CONTROLLER_CHECK(wait_idle(controller));

    const auto snapshot = controller.sync_snapshot();
    CONTROLLER_CHECK(snapshot.nodes.size() == 1U);
    const mediaserver::sync::NodeSyncStatus* const observed =
        snapshot.node(0U);
    CONTROLLER_CHECK(observed != nullptr);
    CONTROLLER_CHECK(!observed->machine_reported);
    CONTROLLER_CHECK(observed->outputs.empty());
    CONTROLLER_CHECK(observed->board ==
                     mediaserver::sync::SyncDeviceAvailability::Unknown);
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
    controller.sync(wired(map));
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

    controller.sync(wired(map));
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
    controller.sync(wired(map));
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
    controller.sync(wired(map));
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (controller.idle() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }

    map.devices.push_back(render_server(2U, 0U, "127.0.0.1:47841"));
    controller.sync(wired(map));
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
    controller.sync(wired(map));
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
    controller.sync(wired(map), 10U);
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (call_index.load() == 0U &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CONTROLLER_CHECK(call_index.load() == 1U);
    // The next project deliberately reuses every persisted target value.
    controller.sync(wired(map), 11U);
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
                true, true, 3840U, 2160U, 60U, 1U, 0, 0, 3840U, 2160U,
                "LED Processor"});
            return app::ClusterMachineInfoRequestResult::success(
                std::move(info));
        }};
    topology::HardwareMap map{};
    map.devices.push_back(render_server(1U, 0U, "127.0.0.1:47840"));

    controller.sync(wired(map), 4U);
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
    const topology::HardwareDisplayTargetKey expected_target{7U, 2U, 0U};
    CONTROLLER_CHECK(
        hardware.nodes[0U].video_outputs[0U].target == expected_target);
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
    controller.sync(wired(map), 5U);
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

    controller.sync(wired(map));
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

    controller.sync(wired(map));
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
    CONTROLLER_CHECK(hardware.nodes[0U].detail.starts_with(
        "password rejected"));
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

void required_assets_gate_launch_and_recover_after_verification() {
    auto config = controller_config();
    config.asset_readiness_poll_interval = 1ms;
    auto manifest = portable_manifest();
    manifest.assets[0U].kind = mediaserver::assets::AssetKind::Model;
    mediaserver::assets::ContentFingerprint fingerprint{};
    fingerprint.byte_length = 1'234U;
    fingerprint.sha256.bytes[0U] = 0xa5U;
    manifest.assets[0U].expected_fingerprint = fingerprint;
    std::atomic_int readiness_state{};
    std::atomic_bool fingerprint_seen{};
    std::atomic_size_t lifecycle_calls{};
    app::ClusterLifecycleController controller{
        config,
        [&](const asio::ip::tcp::endpoint&, core::u32 node_index,
            net::ClusterLifecycleCommand) {
            ++lifecycle_calls;
            return app::ClusterLifecycleRequestResult::success(
                net::ClusterLifecycleReply{
                    net::ClusterLifecycleOutcome::NoChange,
                    net::ClusterProcessState::Stopped,
                    node_index, 1U, "listener online"});
        },
        no_inventory,
        [&](const asio::ip::tcp::endpoint&, core::u32 node_index,
            std::span<const net::ClusterAssetRequirement> sources,
            std::stop_token) {
            net::ClusterAssetReadinessReply reply{};
            reply.availability =
                net::ClusterAssetReadinessAvailability::Available;
            reply.node_index = node_index;
            for (const net::ClusterAssetRequirement& source : sources) {
                fingerprint_seen.store(
                    source.fingerprinted &&
                        source.expected_byte_length ==
                            fingerprint.byte_length &&
                        source.expected_sha256 == fingerprint.sha256.bytes,
                    std::memory_order_release);
                const int observed = readiness_state.load();
                const auto state = observed == 0
                    ? net::ClusterAssetReadinessState::Missing
                    : (observed == 1
                           ? net::ClusterAssetReadinessState::Sha256Mismatch
                           : net::ClusterAssetReadinessState::Ready);
                reply.entries.push_back(
                    net::ClusterAssetReadinessEntry{source.source, state});
                if (state == net::ClusterAssetReadinessState::Ready) {
                    ++reply.ready;
                } else if (state ==
                           net::ClusterAssetReadinessState::Missing) {
                    ++reply.missing;
                } else {
                    ++reply.mismatched;
                }
            }
            return app::ClusterAssetReadinessRequestResult::success(
                std::move(reply));
        }};
    topology::HardwareMap map{};
    map.devices.push_back(render_server(1U, 0U, "127.0.0.1:47840"));

    controller.sync(wired(map), manifest, 1U);
    CONTROLLER_CHECK(wait_idle(controller));
    auto rows = controller.snapshot();
    CONTROLLER_CHECK(rows.size() == 1U);
    CONTROLLER_CHECK(
        rows[0U].content ==
        editor::OutputMonitorState::Node::ContentStatus::Missing);
    CONTROLLER_CHECK(rows[0U].assets_missing == 1U);
    CONTROLLER_CHECK(fingerprint_seen.load(std::memory_order_acquire));
    CONTROLLER_CHECK(!controller.content_ready(manifest));
    const std::size_t before_start = lifecycle_calls.load();
    controller.submit(editor::OutputLifecycleRequest{
        doc::NodeId{1U}, 0U, "127.0.0.1:47840",
        editor::OutputLifecycleAction::Start, false});
    CONTROLLER_CHECK(controller.idle());
    CONTROLLER_CHECK(lifecycle_calls.load() == before_start);

    readiness_state.store(1);
    std::this_thread::sleep_for(3ms);
    controller.submit(editor::OutputLifecycleRequest{
        doc::NodeId{1U}, 0U, "127.0.0.1:47840",
        editor::OutputLifecycleAction::Status, false});
    CONTROLLER_CHECK(wait_idle(controller));
    rows = controller.snapshot();
    CONTROLLER_CHECK(
        rows[0U].content ==
        editor::OutputMonitorState::Node::ContentStatus::Missing);
    CONTROLLER_CHECK(rows[0U].assets_mismatched == 1U);
    CONTROLLER_CHECK(!controller.content_ready(manifest));

    readiness_state.store(2);
    std::this_thread::sleep_for(3ms);
    controller.submit(editor::OutputLifecycleRequest{
        doc::NodeId{1U}, 0U, "127.0.0.1:47840",
        editor::OutputLifecycleAction::Status, false});
    CONTROLLER_CHECK(wait_idle(controller));
    rows = controller.snapshot();
    CONTROLLER_CHECK(
        rows[0U].content ==
        editor::OutputMonitorState::Node::ContentStatus::Ready);
    CONTROLLER_CHECK(rows[0U].assets_ready == 1U);
    CONTROLLER_CHECK(controller.content_ready(manifest));
}

void readiness_batches_more_than_one_wire_request() {
    auto config = controller_config();
    std::atomic_size_t readiness_calls{};
    app::ClusterLifecycleController controller{
        config,
        [](const asio::ip::tcp::endpoint&, core::u32 node_index,
           net::ClusterLifecycleCommand) {
            return app::ClusterLifecycleRequestResult::success(
                net::ClusterLifecycleReply{
                    net::ClusterLifecycleOutcome::NoChange,
                    net::ClusterProcessState::Stopped, node_index, 1U,
                    "listener online"});
        },
        no_inventory,
        [&](const asio::ip::tcp::endpoint&, core::u32 node_index,
            std::span<const net::ClusterAssetRequirement> sources,
            std::stop_token) {
            ++readiness_calls;
            CONTROLLER_CHECK(sources.size() <=
                             net::max_cluster_asset_readiness_entries);
            net::ClusterAssetReadinessReply reply{};
            reply.availability =
                net::ClusterAssetReadinessAvailability::Available;
            reply.node_index = node_index;
            for (const net::ClusterAssetRequirement& source : sources) {
                reply.entries.push_back(net::ClusterAssetReadinessEntry{
                    source.source, net::ClusterAssetReadinessState::Ready});
                ++reply.ready;
            }
            return app::ClusterAssetReadinessRequestResult::success(
                std::move(reply));
        }};
    topology::HardwareMap map{};
    map.devices.push_back(render_server(1U, 0U, "127.0.0.1:47840"));
    const auto manifest = portable_manifest(300U, 99U);
    controller.sync(wired(map), manifest, 1U);
    CONTROLLER_CHECK(wait_idle(controller));
    const auto rows = controller.snapshot();
    CONTROLLER_CHECK(rows[0U].assets_required == 300U);
    CONTROLLER_CHECK(rows[0U].assets_ready == 300U);
    CONTROLLER_CHECK(controller.content_ready(manifest));
    CONTROLLER_CHECK(readiness_calls.load() == 2U);
}

void blocked_readiness_does_not_delay_stop() {
    auto config = controller_config();
    config.worker_count = 1U;
    config.asset_readiness_poll_interval = 1h;
    std::atomic_bool readiness_entered{};
    std::atomic_bool release_readiness{};
    std::atomic_bool stop_observed{};
    app::ClusterLifecycleController controller{
        config,
        [&](const asio::ip::tcp::endpoint&, core::u32 node_index,
            net::ClusterLifecycleCommand command) {
            if (command == net::ClusterLifecycleCommand::Stop) {
                stop_observed.store(true, std::memory_order_release);
            }
            return app::ClusterLifecycleRequestResult::success(
                net::ClusterLifecycleReply{
                    command == net::ClusterLifecycleCommand::Stop
                        ? net::ClusterLifecycleOutcome::Applied
                        : net::ClusterLifecycleOutcome::NoChange,
                    net::ClusterProcessState::Stopped, node_index, 2U,
                    command == net::ClusterLifecycleCommand::Stop
                        ? "stopped"
                        : "listener online"});
        },
        no_inventory,
        [&](const asio::ip::tcp::endpoint&, core::u32 node_index,
            std::span<const net::ClusterAssetRequirement> sources,
            std::stop_token) {
            readiness_entered.store(true, std::memory_order_release);
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (!release_readiness.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(1ms);
            }
            net::ClusterAssetReadinessReply reply{};
            reply.availability =
                net::ClusterAssetReadinessAvailability::Available;
            reply.node_index = node_index;
            for (const net::ClusterAssetRequirement& source : sources) {
                reply.entries.push_back(net::ClusterAssetReadinessEntry{
                    source.source, net::ClusterAssetReadinessState::Ready});
                ++reply.ready;
            }
            return app::ClusterAssetReadinessRequestResult::success(
                std::move(reply));
        }};
    topology::HardwareMap map{};
    map.devices.push_back(render_server(1U, 0U, "127.0.0.1:47840"));
    const auto manifest = portable_manifest();

    controller.sync(wired(map), manifest, 1U);
    const auto readiness_deadline =
        std::chrono::steady_clock::now() + 1s;
    while (!readiness_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < readiness_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    bool lifecycle_status_applied{};
    const auto lifecycle_deadline =
        std::chrono::steady_clock::now() + 1s;
    while (!lifecycle_status_applied &&
           std::chrono::steady_clock::now() < lifecycle_deadline) {
        const auto rows = controller.snapshot();
        lifecycle_status_applied =
            rows.size() == 1U &&
            rows[0U].status ==
                editor::OutputMonitorState::Node::Status::Stopped;
        if (!lifecycle_status_applied) {
            std::this_thread::sleep_for(1ms);
        }
    }

    controller.submit(editor::OutputLifecycleRequest{
        doc::NodeId{1U}, 0U, "127.0.0.1:47840",
        editor::OutputLifecycleAction::Stop, false});
    const auto stop_deadline = std::chrono::steady_clock::now() + 250ms;
    while (!stop_observed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < stop_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const bool observed_readiness =
        readiness_entered.load(std::memory_order_acquire);
    const bool observed_status = lifecycle_status_applied;
    const bool observed_stop = stop_observed.load(std::memory_order_acquire);
    const bool was_not_idle = !controller.idle();
    release_readiness.store(true, std::memory_order_release);

    CONTROLLER_CHECK(observed_readiness);
    CONTROLLER_CHECK(observed_status);
    CONTROLLER_CHECK(observed_stop);
    CONTROLLER_CHECK(was_not_idle);
    CONTROLLER_CHECK(wait_idle(controller));
}

void destruction_cancels_an_active_readiness_request() {
    auto config = controller_config();
    config.worker_count = 1U;
    config.asset_readiness_poll_interval = 1h;
    std::atomic_bool entered{};
    std::atomic_bool cancelled{};

    const auto started = std::chrono::steady_clock::now();
    {
        app::ClusterLifecycleController controller{
            config,
            [](const asio::ip::tcp::endpoint&, core::u32 node_index,
               net::ClusterLifecycleCommand) {
                return app::ClusterLifecycleRequestResult::success(
                    net::ClusterLifecycleReply{
                        net::ClusterLifecycleOutcome::NoChange,
                        net::ClusterProcessState::Stopped, node_index, 1U,
                        "listener online"});
            },
            no_inventory,
            [&](const asio::ip::tcp::endpoint&, core::u32,
                std::span<const net::ClusterAssetRequirement>,
                std::stop_token stop) {
                entered.store(true, std::memory_order_release);
                while (!stop.stop_requested()) {
                    std::this_thread::sleep_for(1ms);
                }
                cancelled.store(true, std::memory_order_release);
                return app::ClusterAssetReadinessRequestResult::failure(
                    {net::ClusterLifecycleClientErrorCode::Cancelled, {}});
            }};
        topology::HardwareMap map{};
        map.devices.push_back(
            render_server(1U, 0U, "127.0.0.1:47840"));
        controller.sync(wired(map), portable_manifest(), 1U);

        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (!entered.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        CONTROLLER_CHECK(entered.load(std::memory_order_acquire));
    }

    CONTROLLER_CHECK(cancelled.load(std::memory_order_acquire));
    CONTROLLER_CHECK(std::chrono::steady_clock::now() - started < 1s);
}

void manifest_change_discards_an_in_flight_readiness_reply() {
    auto config = controller_config();
    config.asset_readiness_poll_interval = 1h;
    std::atomic_bool first_entered{};
    std::atomic_bool second_completed{};
    std::atomic_bool release_first{};
    app::ClusterLifecycleController controller{
        config,
        [](const asio::ip::tcp::endpoint&, core::u32 node_index,
           net::ClusterLifecycleCommand) {
            return app::ClusterLifecycleRequestResult::success(
                net::ClusterLifecycleReply{
                    net::ClusterLifecycleOutcome::NoChange,
                    net::ClusterProcessState::Stopped, node_index, 1U,
                    "listener online"});
        },
        no_inventory,
        [&](const asio::ip::tcp::endpoint&, core::u32 node_index,
            std::span<const net::ClusterAssetRequirement> sources,
            std::stop_token) {
            CONTROLLER_CHECK(sources.size() == 1U);
            const bool first =
                sources[0U].source.find("first") != std::string::npos;
            if (first) {
                first_entered.store(true, std::memory_order_release);
                const auto deadline = std::chrono::steady_clock::now() + 2s;
                while (!release_first.load(std::memory_order_acquire) &&
                       std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(1ms);
                }
            }
            net::ClusterAssetReadinessReply reply{};
            reply.availability =
                net::ClusterAssetReadinessAvailability::Available;
            reply.node_index = node_index;
            const auto state = first
                ? net::ClusterAssetReadinessState::Ready
                : net::ClusterAssetReadinessState::Missing;
            reply.entries.push_back(
                net::ClusterAssetReadinessEntry{sources[0U].source, state});
            if (first) {
                ++reply.ready;
            } else {
                ++reply.missing;
                second_completed.store(true, std::memory_order_release);
            }
            return app::ClusterAssetReadinessRequestResult::success(
                std::move(reply));
        }};
    topology::HardwareMap map{};
    map.devices.push_back(render_server(1U, 0U, "127.0.0.1:47840"));
    auto manifest_a = portable_manifest(1U, 501U);
    manifest_a.assets[0U].source += "-first.mov";
    auto manifest_b = portable_manifest(1U, 502U);
    manifest_b.assets[0U].source += "-second.mov";

    controller.sync(wired(map), manifest_a, 9U);
    const auto first_deadline = std::chrono::steady_clock::now() + 1s;
    while (!first_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < first_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const bool observed_first =
        first_entered.load(std::memory_order_acquire);
    if (observed_first) {
        // Only the manifest changes: target identity and project generation
        // deliberately remain identical.
        controller.sync(wired(map), manifest_b, 9U);
    }
    const auto second_deadline = std::chrono::steady_clock::now() + 1s;
    while (!second_completed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < second_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const bool observed_second =
        second_completed.load(std::memory_order_acquire);
    release_first.store(true, std::memory_order_release);

    CONTROLLER_CHECK(observed_first);
    CONTROLLER_CHECK(observed_second);
    CONTROLLER_CHECK(wait_idle(controller));
    const auto rows = controller.snapshot();
    CONTROLLER_CHECK(rows.size() == 1U);
    CONTROLLER_CHECK(
        rows[0U].content ==
        editor::OutputMonitorState::Node::ContentStatus::Missing);
    CONTROLLER_CHECK(rows[0U].assets_missing == 1U);
    CONTROLLER_CHECK(rows[0U].assets_ready == 0U);
    CONTROLLER_CHECK(!controller.content_ready(manifest_a));
    CONTROLLER_CHECK(!controller.content_ready(manifest_b));
}

void program_gate_requires_exact_store_runtime_and_projected_configuration() {
    std::mutex observed_mutex;
    std::vector<ObservedRequest> observed;
    app::ClusterLifecycleController controller{
        controller_config(),
        [&](const asio::ip::tcp::endpoint&, core::u32 node_index,
            net::ClusterLifecycleCommand command) {
            {
                const std::lock_guard lock{observed_mutex};
                observed.push_back(ObservedRequest{node_index, command});
            }
            return app::ClusterLifecycleRequestResult::success(
                net::ClusterLifecycleReply{
                    command == net::ClusterLifecycleCommand::Start
                        ? net::ClusterLifecycleOutcome::Applied
                        : net::ClusterLifecycleOutcome::NoChange,
                    command == net::ClusterLifecycleCommand::Start
                        ? net::ClusterProcessState::Running
                        : net::ClusterProcessState::Stopped,
                    node_index, 3U, "listener online"});
        },
        no_inventory,
        all_assets_ready};
    topology::HardwareMap map{};
    map.devices.push_back(render_server(1U, 0U, "127.0.0.1:47840"));
    map.devices.push_back(render_server(2U, 2U, "127.0.0.1:47842"));
    const auto manifest = portable_manifest(1U, 701U);
    constexpr core::u64 project_generation = 41U;

    controller.sync(wired(map), manifest, project_generation);
    CONTROLLER_CHECK(wait_idle(controller));
    CONTROLLER_CHECK(controller.content_ready(manifest));
    {
        const std::lock_guard lock{observed_mutex};
        observed.clear();
    }

    net::DirectorFrameBarrier barrier{genesis_cursor()};
    CONTROLLER_CHECK(!controller.program_ready(
        manifest, wired(map), project_generation, barrier));

    // Lifecycle launch remains store-only gated. Runtime adoption gates the
    // Program switch, not the listener Start/Restart controls.
    controller.submit(editor::OutputLifecycleRequest{
        doc::NodeId{1U}, 0U, "127.0.0.1:47840",
        editor::OutputLifecycleAction::Start, false});
    CONTROLLER_CHECK(wait_idle(controller));
    {
        const std::lock_guard lock{observed_mutex};
        CONTROLLER_CHECK(observed.size() == 1U);
        CONTROLLER_CHECK(
            observed[0U].command == net::ClusterLifecycleCommand::Start);
        observed.clear();
    }

    CONTROLLER_CHECK(barrier.register_live(
        runtime_registration(10U, 100U, 0U)));
    CONTROLLER_CHECK(barrier.register_live(
        runtime_registration(11U, 101U, 2U)));
    CONTROLLER_CHECK(!controller.program_ready(
        manifest, wired(map), project_generation, barrier));

    publish_runtime_adoption(barrier, 10U, 2U, manifest.digest);
    publish_runtime_adoption(barrier, 11U, 2U, manifest.digest + 1U);
    CONTROLLER_CHECK(!controller.program_ready(
        manifest, wired(map), project_generation, barrier));
    publish_runtime_adoption(barrier, 11U, 3U, manifest.digest);
    CONTROLLER_CHECK(controller.program_ready(
        manifest, wired(map), project_generation, barrier));

    auto other_manifest = manifest;
    ++other_manifest.digest;
    CONTROLLER_CHECK(!controller.program_ready(
        other_manifest, wired(map), project_generation, barrier));
    CONTROLLER_CHECK(!controller.program_ready(
        manifest, wired(map), project_generation + 1U, barrier));

    auto pending_hardware = map;
    pending_hardware.devices[0U].window_mode =
        topology::HardwareWindowMode::Fullscreen;
    CONTROLLER_CHECK(!controller.program_ready(
        manifest, wired(pending_hardware), project_generation, barrier));
    CONTROLLER_CHECK(controller.program_ready(
        manifest, wired(map), project_generation, barrier));

    barrier.unregister(
        runtime_id(11U), net::DirectorSessionLeaveReason::SessionFailure);
    CONTROLLER_CHECK(!controller.program_ready(
        manifest, wired(map), project_generation, barrier));
}

void program_gate_requires_bound_display_identity_and_fullscreen_rate() {
    auto config = controller_config();
    config.machine_info_poll_interval = 1h;
    app::ClusterLifecycleController controller{
        config,
        [](const asio::ip::tcp::endpoint&, core::u32 node_index,
           net::ClusterLifecycleCommand) {
            return app::ClusterLifecycleRequestResult::success(
                net::ClusterLifecycleReply{
                    net::ClusterLifecycleOutcome::NoChange,
                    net::ClusterProcessState::Running, node_index, 1U,
                    "running"});
        },
        [](const asio::ip::tcp::endpoint&, core::u32 node_index) {
            net::ClusterMachineInfo info{};
            info.availability =
                net::ClusterMachineInfoAvailability::Available;
            info.display_state =
                net::ClusterDisplayEnumerationState::Available;
            info.node_index = node_index;
            info.display_targets.push_back(net::ClusterDisplayTargetInfo{
                7U, 2U, 0U, 0U, 0U,
                net::ClusterVideoOutputTechnology::DisplayPort,
                true, true, 3840U, 2160U, 60U, 1U, 0, 0, 3840U, 2160U, "LED"});
            return app::ClusterMachineInfoRequestResult::success(
                std::move(info));
        },
        all_assets_ready};
    topology::HardwareMap map{};
    map.devices.push_back(render_server(1U, 0U, "127.0.0.1:47840"));
    map.output_bindings.push_back(topology::HardwareOutputBinding{
        doc::NodeId{10U}, doc::NodeId{1U}, 1U, 0U,
        topology::HardwareDisplayTargetKey{7U, 2U, 0U},
        3840U, 2160U, 60U, 1U,
        topology::HardwareWindowMode::Fullscreen});
    const app::ProjectAssetManifest manifest{};
    constexpr core::u64 generation = 72U;
    net::DirectorFrameBarrier barrier{genesis_cursor()};
    CONTROLLER_CHECK(barrier.register_live(
        runtime_registration(40U, 400U, 0U)));
    publish_runtime_adoption(barrier, 40U, 2U, manifest.digest);

    controller.sync(wired(map), manifest, generation);
    CONTROLLER_CHECK(wait_idle(controller));
    CONTROLLER_CHECK(controller.program_ready(
        manifest, wired(map), generation, barrier));

    auto missing = map;
    missing.output_bindings[0U].target.target_id = 3U;
    controller.sync(wired(missing), manifest, generation);
    CONTROLLER_CHECK(wait_idle(controller));
    CONTROLLER_CHECK(!controller.program_ready(
        manifest, wired(missing), generation, barrier));

    auto incompatible = map;
    incompatible.output_bindings[0U].refresh_numerator = 59U;
    controller.sync(wired(incompatible), manifest, generation);
    CONTROLLER_CHECK(wait_idle(controller));
    CONTROLLER_CHECK(!controller.program_ready(
        manifest, wired(incompatible), generation, barrier));

    // The same named rate written as another fraction (a pixel-clock 60.0006)
    // is the display's mode, not a mismatch.
    auto same_rate = map;
    same_rate.output_bindings[0U].refresh_numerator = 2'000'000U;
    same_rate.output_bindings[0U].refresh_denominator = 33'333U;
    controller.sync(wired(same_rate), manifest, generation);
    CONTROLLER_CHECK(wait_idle(controller));
    CONTROLLER_CHECK(controller.program_ready(
        manifest, wired(same_rate), generation, barrier));

    auto borderless = incompatible;
    borderless.output_bindings[0U].window_mode =
        topology::HardwareWindowMode::Borderless;
    controller.sync(wired(borderless), manifest, generation);
    CONTROLLER_CHECK(wait_idle(controller));
    CONTROLLER_CHECK(controller.program_ready(
        manifest, wired(borderless), generation, barrier));
}

void hardware_revision_fences_an_in_flight_store_reply() {
    auto config = controller_config();
    config.asset_readiness_poll_interval = 1h;
    std::atomic_size_t readiness_calls{};
    std::atomic_bool first_entered{};
    std::atomic_bool release_first{};
    std::atomic_bool second_completed{};
    app::ClusterLifecycleController controller{
        config,
        [](const asio::ip::tcp::endpoint&, core::u32 node_index,
           net::ClusterLifecycleCommand) {
            return app::ClusterLifecycleRequestResult::success(
                net::ClusterLifecycleReply{
                    net::ClusterLifecycleOutcome::NoChange,
                    net::ClusterProcessState::Stopped, node_index, 1U,
                    "listener online"});
        },
        no_inventory,
        [&](const asio::ip::tcp::endpoint&, core::u32 node_index,
            std::span<const net::ClusterAssetRequirement> sources,
            std::stop_token) {
            const std::size_t call = readiness_calls.fetch_add(1U);
            if (call == 0U) {
                first_entered.store(true, std::memory_order_release);
                const auto deadline = std::chrono::steady_clock::now() + 2s;
                while (!release_first.load(std::memory_order_acquire) &&
                       std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(1ms);
                }
            }
            net::ClusterAssetReadinessReply reply{};
            reply.availability =
                net::ClusterAssetReadinessAvailability::Available;
            reply.node_index = node_index;
            const auto state = call == 0U
                ? net::ClusterAssetReadinessState::Ready
                : net::ClusterAssetReadinessState::Missing;
            for (const net::ClusterAssetRequirement& source : sources) {
                reply.entries.push_back(
                    net::ClusterAssetReadinessEntry{source.source, state});
                if (state == net::ClusterAssetReadinessState::Ready) {
                    ++reply.ready;
                } else {
                    ++reply.missing;
                }
            }
            if (call != 0U) {
                second_completed.store(true, std::memory_order_release);
            }
            return app::ClusterAssetReadinessRequestResult::success(
                std::move(reply));
        }};
    topology::HardwareMap map{};
    map.devices.push_back(render_server(1U, 0U, "127.0.0.1:47840"));
    const auto manifest = portable_manifest(1U, 801U);
    constexpr core::u64 project_generation = 52U;

    controller.sync(wired(map), manifest, project_generation);
    const auto first_deadline = std::chrono::steady_clock::now() + 1s;
    while (!first_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < first_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const bool observed_first =
        first_entered.load(std::memory_order_acquire);

    auto revised_map = map;
    revised_map.devices[0U].window_mode =
        topology::HardwareWindowMode::Fullscreen;
    if (observed_first) {
        // This field is deliberately outside ControllerEntry's legacy
        // endpoint/size comparison. The deployment digest must still advance
        // every entry revision and reject the outstanding old-config reply.
        controller.sync(wired(revised_map), manifest, project_generation);
    }
    const auto second_deadline = std::chrono::steady_clock::now() + 1s;
    while (!second_completed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < second_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const bool observed_second =
        second_completed.load(std::memory_order_acquire);
    release_first.store(true, std::memory_order_release);

    CONTROLLER_CHECK(observed_first);
    CONTROLLER_CHECK(observed_second);
    CONTROLLER_CHECK(wait_idle(controller));
    CONTROLLER_CHECK(readiness_calls.load() == 2U);
    const auto rows = controller.snapshot();
    CONTROLLER_CHECK(rows.size() == 1U);
    CONTROLLER_CHECK(
        rows[0U].content ==
        editor::OutputMonitorState::Node::ContentStatus::Missing);
    CONTROLLER_CHECK(rows[0U].assets_ready == 0U);
    CONTROLLER_CHECK(rows[0U].assets_missing == 1U);

    net::DirectorFrameBarrier barrier{genesis_cursor()};
    CONTROLLER_CHECK(barrier.register_live(
        runtime_registration(20U, 200U, 0U)));
    publish_runtime_adoption(barrier, 20U, 2U, manifest.digest);
    CONTROLLER_CHECK(controller.program_ready(
        manifest, wired(revised_map), project_generation, barrier));
    CONTROLLER_CHECK(!controller.program_ready(
        manifest, wired(map), project_generation, barrier));
}

void program_gate_is_vacuous_only_for_an_empty_valid_cluster() {
    app::ClusterLifecycleController controller{
        controller_config(),
        [](const asio::ip::tcp::endpoint&, core::u32 node_index,
           net::ClusterLifecycleCommand) {
            return app::ClusterLifecycleRequestResult::success(
                net::ClusterLifecycleReply{
                    net::ClusterLifecycleOutcome::NoChange,
                    net::ClusterProcessState::Stopped, node_index, 1U,
                    "listener online"});
        },
        no_inventory,
        all_assets_ready};
    const app::ProjectAssetManifest manifest{};
    constexpr core::u64 project_generation = 61U;
    topology::HardwareMap empty{};
    net::DirectorFrameBarrier barrier{genesis_cursor()};

    CONTROLLER_CHECK(!controller.program_ready(
        manifest, wired(empty), project_generation, barrier));
    controller.sync(wired(empty), manifest, project_generation);
    CONTROLLER_CHECK(controller.program_ready(
        manifest, wired(empty), project_generation, barrier));

    topology::HardwareMap duplicates{};
    duplicates.devices.push_back(
        render_server(1U, 0U, "127.0.0.1:47840"));
    duplicates.devices.push_back(
        render_server(2U, 0U, "127.0.0.1:47841"));
    controller.sync(wired(duplicates), manifest, project_generation);
    CONTROLLER_CHECK(wait_idle(controller));
    CONTROLLER_CHECK(barrier.register_live(
        runtime_registration(30U, 300U, 0U)));
    publish_runtime_adoption(barrier, 30U, 2U, manifest.digest);
    CONTROLLER_CHECK(!controller.program_ready(
        manifest, wired(duplicates), project_generation, barrier));
}

void offline_media_can_start_while_store_health_remains_missing() {
    std::atomic<std::size_t> starts{};
    app::ClusterLifecycleController controller{
        controller_config(),
        [&](const asio::ip::tcp::endpoint&, core::u32 node_index,
            net::ClusterLifecycleCommand command) {
            if (command == net::ClusterLifecycleCommand::Start) ++starts;
            return app::ClusterLifecycleRequestResult::success(
                net::ClusterLifecycleReply{net::ClusterLifecycleOutcome::NoChange,
                    net::ClusterProcessState::Stopped, node_index, 1U, "online"});
        }, no_inventory,
        [](const asio::ip::tcp::endpoint&, core::u32 node_index,
           std::span<const net::ClusterAssetRequirement> sources, std::stop_token) {
            net::ClusterAssetReadinessReply reply{};
            reply.availability = net::ClusterAssetReadinessAvailability::Available;
            reply.node_index = node_index;
            for (const auto& source : sources) {
                reply.entries.push_back({source.source, net::ClusterAssetReadinessState::Missing});
                ++reply.missing;
            }
            return app::ClusterAssetReadinessRequestResult::success(std::move(reply));
        }};
    topology::HardwareMap map{};
    map.devices.push_back(render_server(1U, 0U, "127.0.0.1:47840"));
    auto manifest = portable_manifest();
    controller.sync(wired(map), manifest, 1U);
    CONTROLLER_CHECK(wait_idle(controller));
    auto rows = controller.snapshot();
    CONTROLLER_CHECK(rows.size() == 1U && rows[0].missing_media_only);
    CONTROLLER_CHECK(rows[0].content_ready());
    CONTROLLER_CHECK(!controller.content_ready(manifest));
    controller.submit(editor::OutputLifecycleRequest{doc::NodeId{1U}, 0U,
        "127.0.0.1:47840", editor::OutputLifecycleAction::Start, false});
    CONTROLLER_CHECK(wait_idle(controller));
    CONTROLLER_CHECK(starts == 1U);

    // Reusing the same path for calibration must invalidate the fallback
    // classification, even if a caller accidentally reuses a digest.
    manifest.assets[0].kind = mediaserver::assets::AssetKind::ProjectionMesh;
    controller.sync(wired(map), manifest, 2U);
    CONTROLLER_CHECK(wait_idle(controller));
    rows = controller.snapshot();
    CONTROLLER_CHECK(!rows[0].missing_media_only);
    CONTROLLER_CHECK(!rows[0].content_ready());
    controller.submit(editor::OutputLifecycleRequest{doc::NodeId{1U}, 0U,
        "127.0.0.1:47840", editor::OutputLifecycleAction::Start, false});
    CONTROLLER_CHECK(wait_idle(controller));
    CONTROLLER_CHECK(starts == 1U);
}

// A Render Server card no cable touches is a parked alternative. It is asked
// only for its status and displays, so its real outputs can be cabled. It is
// not listed or started, it does not make the cabled card's node index a
// duplicate, and it never holds the Program gate for the rest of the cluster.
void a_parked_render_server_is_ignored_by_the_cluster() {
    std::mutex observed_mutex;
    std::vector<ObservedRequest> observed;
    std::vector<core::u32> inventoried;
    std::vector<core::u32> asset_checked;
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
                    net::ClusterProcessState::Stopped, node_index, 1U,
                    "stopped"});
        },
        [&](const asio::ip::tcp::endpoint&, core::u32 node_index) {
            {
                const std::lock_guard lock{observed_mutex};
                inventoried.push_back(node_index);
            }
            net::ClusterMachineInfo info{};
            info.availability =
                net::ClusterMachineInfoAvailability::Available;
            info.display_state =
                net::ClusterDisplayEnumerationState::Available;
            info.node_index = node_index;
            for (core::u32 target = 1U; target <= 2U; ++target) {
                info.display_targets.push_back(net::ClusterDisplayTargetInfo{
                    7U, target, 0U, 0U, 0U,
                    net::ClusterVideoOutputTechnology::Hdmi,
                    true, true, 1920U, 1080U, 60U, 1U, 0, 0, 1920U, 1080U,
                    "HDMI"});
            }
            return app::ClusterMachineInfoRequestResult::success(
                std::move(info));
        },
        [&](const asio::ip::tcp::endpoint& endpoint, core::u32 node_index,
            std::span<const net::ClusterAssetRequirement> sources,
            std::stop_token stop) {
            {
                const std::lock_guard lock{observed_mutex};
                asset_checked.push_back(node_index);
            }
            return all_assets_ready(endpoint, node_index, sources, stop);
        }};
    const app::ProjectAssetManifest manifest =
        portable_manifest();
    constexpr core::u64 project_generation = 62U;
    topology::HardwareMap map{};
    map.devices.push_back(render_server(1U, 0U, "127.0.0.1:47840"));
    map = wired(map);
    // Added after wiring: same node index, an unreachable endpoint, no cable.
    map.devices.insert(map.devices.begin() + 1,
                       render_server(2U, 0U, "not an endpoint"));
    map.devices.insert(map.devices.begin() + 2,
                       render_server(3U, 9U, "127.0.0.1:47849"));
    CONTROLLER_CHECK(topology::render_server_active(map, map.devices[0U]));
    CONTROLLER_CHECK(!topology::render_server_active(map, map.devices[1U]));
    CONTROLLER_CHECK(topology::cluster_render_servers(map).size() == 1U);
    CONTROLLER_CHECK(topology::next_free_node_index(map) == 1U);

    controller.sync(map, manifest, project_generation);
    CONTROLLER_CHECK(wait_idle(controller));
    controller.submit(editor::OutputLifecycleRequest{
        {}, 0U, {}, editor::OutputLifecycleAction::Start, true});
    controller.submit(editor::OutputLifecycleRequest{
        doc::NodeId{3U}, 9U, "127.0.0.1:47849",
        editor::OutputLifecycleAction::Start, false});
    CONTROLLER_CHECK(wait_idle(controller));
    const auto rows = controller.snapshot();
    CONTROLLER_CHECK(rows.size() == 1U);
    CONTROLLER_CHECK(rows[0U].device == doc::NodeId{1U});
    CONTROLLER_CHECK(rows[0U].status !=
                     editor::OutputMonitorState::Node::Status::DuplicateNodeIndex);
    CONTROLLER_CHECK(controller.node_name(9U).empty());
    CONTROLLER_CHECK(controller.sync_snapshot().nodes.size() == 1U);
    CONTROLLER_CHECK(controller.tracking_snapshot().size() == 1U);
    {
        const std::lock_guard lock{observed_mutex};
        bool parked_status_polled = false;
        for (const ObservedRequest& request : observed) {
            if (request.node_index == 9U) {
                // Asked, never started.
                CONTROLLER_CHECK(request.command ==
                                 net::ClusterLifecycleCommand::Status);
                parked_status_polled = true;
            }
        }
        CONTROLLER_CHECK(parked_status_polled);
        CONTROLLER_CHECK(std::find(inventoried.begin(), inventoried.end(),
                                   9U) != inventoried.end());
        CONTROLLER_CHECK(!asset_checked.empty());
        for (const core::u32 node_index : asset_checked) {
            CONTROLLER_CHECK(node_index == 0U);
        }
    }

    // The map sees the parked machine's two displays.
    const topology::HardwareRuntimeSnapshot hardware =
        controller.hardware_snapshot();
    const topology::HardwareRuntimeNode* const parked =
        hardware.node(doc::NodeId{3U});
    CONTROLLER_CHECK(parked != nullptr);
    CONTROLLER_CHECK(parked->video_outputs.size() == 2U);
    CONTROLLER_CHECK(parked->inventory ==
                     topology::HardwareRuntimeInventoryState::Available);

    net::DirectorFrameBarrier barrier{genesis_cursor()};
    CONTROLLER_CHECK(barrier.register_live(
        runtime_registration(31U, 310U, 0U)));
    publish_runtime_adoption(barrier, 31U, 2U, manifest.digest);
    CONTROLLER_CHECK(controller.content_ready(manifest));
    CONTROLLER_CHECK(controller.program_ready(
        manifest, map, project_generation, barrier));
}

// A render machine's agent and this editor agree on a password, or neither
// has one. With the wrong one the node says so in the operator's terms; a
// password saved in Preferences reaches the running controller, which asks
// every node again at once rather than waiting for a restart.
void a_new_password_reaches_a_running_controller() {
    struct StoppedHost final : net::ClusterProcessHost {
        net::ClusterLifecycleReply apply(
            net::ClusterLifecycleCommand) noexcept override {
            return net::ClusterLifecycleReply{
                net::ClusterLifecycleOutcome::NoChange,
                net::ClusterProcessState::Stopped, 0U, 0U, "stopped"};
        }
    } host;
    net::ClusterLifecycleService agent{
        net::ClusterLifecycleServiceConfig{
            {asio::ip::address_v4::loopback(), 0U}, 0U,
            net::lifecycle_key_from_password("hunter2"), 500ms, 500ms},
        host};
    auto started = agent.start();
    CONTROLLER_CHECK(started);

    app::ClusterLifecycleControllerConfig config = controller_config();
    config.shared_secret = net::lifecycle_key_from_password("");
    config.poll_interval = 50ms;
    config.client.connect_timeout = 500ms;
    config.client.handshake_timeout = 500ms;
    app::ClusterLifecycleController controller{std::move(config)};
    topology::HardwareMap map{};
    map.devices.push_back(render_server(
        1U, 0U, "127.0.0.1:" + std::to_string(started.value().port())));
    map = wired(map);

    const auto settle = [&](editor::OutputMonitorState::Node::Status wanted) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            controller.sync(map);
            const auto rows = controller.snapshot();
            if (rows.size() == 1U && rows[0U].status == wanted) {
                return rows[0U];
            }
            std::this_thread::sleep_for(20ms);
        }
        throw std::runtime_error{"node did not settle"};
    };
    const auto refused =
        settle(editor::OutputMonitorState::Node::Status::AuthenticationFailed);
    CONTROLLER_CHECK(refused.detail.find("password") != std::string::npos);

    controller.set_shared_secret(net::lifecycle_key_from_password("hunter2"));
    const auto accepted =
        settle(editor::OutputMonitorState::Node::Status::Stopped);
    CONTROLLER_CHECK(accepted.device == doc::NodeId{1U});
    agent.stop();
}

void conflicting_revisions_for_one_path_fail_without_a_probe() {
    auto manifest = portable_manifest(2U, 9'001U);
    manifest.assets[1U].source = manifest.assets[0U].source;
    mediaserver::assets::ContentFingerprint first{};
    first.byte_length = 100U;
    first.sha256.bytes[0U] = 1U;
    mediaserver::assets::ContentFingerprint second = first;
    second.sha256.bytes[0U] = 2U;
    manifest.assets[0U].expected_fingerprint = first;
    manifest.assets[1U].expected_fingerprint = second;

    std::atomic_size_t readiness_calls{};
    app::ClusterLifecycleController controller{
        controller_config(),
        [](const asio::ip::tcp::endpoint&, core::u32 node_index,
           net::ClusterLifecycleCommand) {
            return app::ClusterLifecycleRequestResult::success(
                net::ClusterLifecycleReply{
                    net::ClusterLifecycleOutcome::NoChange,
                    net::ClusterProcessState::Stopped, node_index, 1U,
                    "listener online"});
        },
        no_inventory,
        [&](const asio::ip::tcp::endpoint& endpoint, core::u32 node_index,
            std::span<const net::ClusterAssetRequirement> requirements,
            std::stop_token stop) {
            ++readiness_calls;
            return all_assets_ready(endpoint, node_index, requirements, stop);
        }};
    topology::HardwareMap map{};
    map.devices.push_back(render_server(1U, 0U, "127.0.0.1:47840"));
    controller.sync(wired(map), manifest, 1U);
    CONTROLLER_CHECK(wait_idle(controller));
    const auto rows = controller.snapshot();
    CONTROLLER_CHECK(rows.size() == 1U);
    CONTROLLER_CHECK(
        rows[0U].content ==
        editor::OutputMonitorState::Node::ContentStatus::Error);
    CONTROLLER_CHECK(rows[0U].content_detail ==
                     "conflicting asset revisions share one source path");
    CONTROLLER_CHECK(readiness_calls.load() == 0U);
    CONTROLLER_CHECK(!controller.content_ready(manifest));
}

} // namespace

void node_logs_are_fetched_on_request_beside_lifecycle_work() {
    std::atomic_size_t log_calls{};
    std::atomic<core::u32> asked_index{};
    std::atomic_size_t asked_lines{};
    std::atomic_bool fail_next{};
    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool gate_open = true;
    app::ClusterLifecycleControllerConfig config = controller_config();
    config.log_lines = 37U;
    app::ClusterLifecycleController controller{
        config,
        [&](const asio::ip::tcp::endpoint&, core::u32 node_index,
            net::ClusterLifecycleCommand) {
            // The process is faulted; the log must still be reachable.
            return app::ClusterLifecycleRequestResult::success(
                net::ClusterLifecycleReply{
                    net::ClusterLifecycleOutcome::NoChange,
                    net::ClusterProcessState::Faulted, node_index, 1U,
                    "process exited"});
        },
        &no_inventory,
        {},
        {},
        [&](const asio::ip::tcp::endpoint&, core::u32 node_index,
            std::size_t lines) {
            {
                std::unique_lock lock{gate_mutex};
                gate_changed.wait(lock, [&] { return gate_open; });
            }
            log_calls.fetch_add(1U);
            asked_index.store(node_index);
            asked_lines.store(lines);
            if (fail_next.exchange(false)) {
                return app::ClusterNodeLogRequestResult::failure(
                    {net::ClusterLifecycleClientErrorCode::TimedOut, {}});
            }
            net::ClusterNodeLog log{};
            log.availability = net::ClusterNodeLogAvailability::Available;
            log.node_index = node_index;
            log.previous_run_lines = 1U;
            log.dropped_bytes = 512U;
            log.lines = {"previous run: NODE 2 FELL BEHIND",
                         "current run: reconnecting"};
            log.detail = "node-2.log";
            return app::ClusterNodeLogRequestResult::success(std::move(log));
        }};

    topology::HardwareMap map{};
    map.devices.push_back(render_server(1U, 2U, "127.0.0.1:47842"));
    controller.sync(wired(map));
    CONTROLLER_CHECK(wait_idle(controller));
    // Nothing reads a log until it is asked for.
    CONTROLLER_CHECK(log_calls.load() == 0U);
    auto rows = controller.snapshot();
    CONTROLLER_CHECK(rows.size() == 1U);
    CONTROLLER_CHECK(rows[0].log == nullptr);
    CONTROLLER_CHECK(!rows[0].log_fetching);

    const editor::OutputLifecycleRequest fetch{
        doc::NodeId{1U}, 2U, "127.0.0.1:47842",
        editor::OutputLifecycleAction::FetchLog, false};
    {
        const std::lock_guard lock{gate_mutex};
        gate_open = false;
    }
    controller.submit(fetch);
    rows = controller.snapshot();
    CONTROLLER_CHECK(rows[0].log_fetching);
    CONTROLLER_CHECK(rows[0].log != nullptr);
    CONTROLLER_CHECK(rows[0].log->status ==
                     editor::OutputNodeLog::Status::Fetching);
    // A second request while one is on its way is the same request.
    controller.submit(fetch);
    controller.submit(fetch);
    // Asking for the log changes nothing about the process.
    CONTROLLER_CHECK(rows[0].status ==
                     editor::OutputMonitorState::Node::Status::Faulted);
    {
        const std::lock_guard lock{gate_mutex};
        gate_open = true;
    }
    gate_changed.notify_all();
    CONTROLLER_CHECK(wait_idle(controller));
    CONTROLLER_CHECK(log_calls.load() == 1U);
    CONTROLLER_CHECK(asked_index.load() == 2U);
    CONTROLLER_CHECK(asked_lines.load() == 37U);

    rows = controller.snapshot();
    CONTROLLER_CHECK(!rows[0].log_fetching);
    CONTROLLER_CHECK(rows[0].log != nullptr);
    const editor::OutputNodeLog& log = *rows[0].log;
    CONTROLLER_CHECK(log.status == editor::OutputNodeLog::Status::Available);
    CONTROLLER_CHECK(log.lines.size() == 2U);
    CONTROLLER_CHECK(log.lines[1] == "current run: reconnecting");
    CONTROLLER_CHECK(log.previous_run_lines == 1U);
    CONTROLLER_CHECK(log.dropped_bytes == 512U);
    CONTROLLER_CHECK(log.fetched_at.size() == 8U);
    const std::string summary = editor::output_node_log_summary(rows[0]);
    CONTROLLER_CHECK(summary.find("2 lines") != std::string::npos);
    CONTROLLER_CHECK(summary.find("1 from the previous run") !=
                     std::string::npos);
    CONTROLLER_CHECK(summary.find("512 bytes dropped") != std::string::npos);

    // A failed read replaces the old lines with the reason, rather than
    // leaving stale lines that read as current.
    fail_next.store(true);
    controller.submit(fetch);
    CONTROLLER_CHECK(wait_idle(controller));
    rows = controller.snapshot();
    CONTROLLER_CHECK(rows[0].log->status ==
                     editor::OutputNodeLog::Status::Failed);
    CONTROLLER_CHECK(rows[0].log->lines.empty());
    CONTROLLER_CHECK(editor::output_node_log_summary(rows[0]).starts_with(
        "Could not read the log"));

    // A request naming another endpoint is not this row's.
    controller.submit(editor::OutputLifecycleRequest{
        doc::NodeId{1U}, 2U, "127.0.0.1:9",
        editor::OutputLifecycleAction::FetchLog, false});
    CONTROLLER_CHECK(wait_idle(controller));
    CONTROLLER_CHECK(log_calls.load() == 2U);
}

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"sync is nonblocking", &sync_never_waits_for_a_dead_or_slow_listener},
        {"node logs are fetched on request beside lifecycle work",
         &node_logs_are_fetched_on_request_beside_lifecycle_work},
        {"sync status merges the machine and runtime halves",
         &sync_status_merges_the_machine_and_runtime_halves},
        {"sync status for another node identity is discarded",
         &sync_status_for_another_node_identity_is_discarded},
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
        {"required assets gate launch and recover",
         &required_assets_gate_launch_and_recover_after_verification},
        {"readiness batches beyond one wire request",
         &readiness_batches_more_than_one_wire_request},
        {"blocked readiness does not delay stop",
         &blocked_readiness_does_not_delay_stop},
        {"destruction cancels active readiness",
         &destruction_cancels_an_active_readiness_request},
        {"manifest change fences in-flight readiness",
         &manifest_change_discards_an_in_flight_readiness_reply},
        {"Program gate requires exact store runtime and configuration",
         &program_gate_requires_exact_store_runtime_and_projected_configuration},
        {"Program gate requires bound display identity and mode",
         &program_gate_requires_bound_display_identity_and_fullscreen_rate},
        {"hardware revision fences in-flight store readiness",
         &hardware_revision_fences_an_in_flight_store_reply},
        {"Program gate empty and invalid cluster semantics",
         &program_gate_is_vacuous_only_for_an_empty_valid_cluster},
        {"offline media starts with a missing-source warning",
         &offline_media_can_start_while_store_health_remains_missing},
        {"conflicting revisions for one path fail without a probe",
         &conflicting_revisions_for_one_path_fail_without_a_probe},
        {"a parked Render Server is ignored by the cluster",
         &a_parked_render_server_is_ignored_by_the_cluster},
        {"a new password reaches a running controller",
         &a_new_password_reaches_a_running_controller},
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
