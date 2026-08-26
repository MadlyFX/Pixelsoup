#include "cluster_lifecycle_controller.hpp"

#include <mediaserver/net/director_frame_barrier.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <map>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace mediaserver::app {
namespace {

using Status = editor::OutputMonitorState::Node::Status;
using ContentStatus = editor::OutputMonitorState::Node::ContentStatus;

inline constexpr core::u64 deployment_fnv_offset = 14695981039346656037ULL;
inline constexpr core::u64 deployment_fnv_prime = 1099511628211ULL;

void deployment_hash_byte(core::u64& hash, core::u8 value) noexcept {
    hash ^= value;
    hash *= deployment_fnv_prime;
}

void deployment_hash_u64(core::u64& hash, core::u64 value) noexcept {
    for (core::u8 shift = 0U; shift < 64U; shift += 8U) {
        deployment_hash_byte(
            hash, static_cast<core::u8>((value >> shift) & 0xffU));
    }
}

void deployment_hash_text(core::u64& hash, std::string_view value) noexcept {
    deployment_hash_u64(hash, static_cast<core::u64>(value.size()));
    for (const char character : value) {
        deployment_hash_byte(
            hash, static_cast<core::u8>(
                      static_cast<unsigned char>(character)));
    }
}

// topology::digest fences every authored hardware value (including window and
// output geometry). The derived suffix additionally fences the exact listener
// endpoint and eligibility classification consumed by this controller, rather
// than assuming two differently-authored maps resolve identically.
[[nodiscard]] core::u64 controller_deployment_digest(
    const topology::HardwareMap& map,
    std::string_view shared_secret) {
    core::u64 hash = deployment_fnv_offset;
    deployment_hash_u64(hash, topology::digest(map));
    const bool secret_available = shared_secret.size() >= 32U &&
                                  shared_secret.size() <= 256U;
    deployment_hash_byte(hash, secret_available ? 1U : 0U);

    std::size_t render_server_count{};
    for (const topology::HardwareDevice& device : map.devices) {
        if (device.kind == topology::HardwareDeviceKind::RenderServer) {
            ++render_server_count;
        }
    }
    deployment_hash_u64(
        hash, static_cast<core::u64>(render_server_count));
    for (const topology::HardwareDevice& device : map.devices) {
        if (device.kind != topology::HardwareDeviceKind::RenderServer) {
            continue;
        }
        std::size_t duplicate_count{};
        for (const topology::HardwareDevice& candidate : map.devices) {
            if (candidate.kind ==
                    topology::HardwareDeviceKind::RenderServer &&
                candidate.node_index == device.node_index) {
                ++duplicate_count;
            }
        }
        const auto endpoint = topology::lifecycle_endpoint_for(map, device);
        const auto parsed = endpoint.has_value()
            ? net::parse_lifecycle_endpoint(*endpoint)
            : std::nullopt;
        const bool duplicate = duplicate_count != 1U;
        const bool eligible = secret_available && parsed.has_value() &&
                              !duplicate;
        deployment_hash_u64(hash, device.node.value());
        deployment_hash_u64(hash, device.node_index);
        deployment_hash_byte(hash, endpoint.has_value() ? 1U : 0U);
        if (endpoint.has_value()) {
            deployment_hash_text(hash, *endpoint);
        }
        deployment_hash_byte(hash, parsed.has_value() ? 1U : 0U);
        deployment_hash_byte(hash, duplicate ? 1U : 0U);
        deployment_hash_byte(hash, eligible ? 1U : 0U);
    }
    return hash;
}

struct ControllerManifest final {
    core::u64 digest{};
    core::u32 required{};
    core::u32 unverified{};
    bool portable{};
    std::shared_ptr<const std::vector<net::ClusterAssetIdentity>> identities;
};

[[nodiscard]] core::u32 saturated_add(core::u32 left,
                                      core::u32 right) noexcept {
    return right > std::numeric_limits<core::u32>::max() - left
        ? std::numeric_limits<core::u32>::max()
        : left + right;
}

[[nodiscard]] ControllerManifest controller_manifest(
    const ProjectAssetManifest& manifest) {
    ControllerManifest result{};
    result.digest = manifest.digest;
    result.unverified = manifest.unverified_assets;
    result.unverified = saturated_add(result.unverified,
        manifest.unregistered_references);
    result.unverified = saturated_add(result.unverified,
        manifest.kind_mismatches);
    result.unverified = saturated_add(result.unverified,
        manifest.dependency_manifests_required);
    result.unverified = saturated_add(result.unverified,
        manifest.non_shared_assets);
    result.portable = manifest.shared_store_ready();
    auto identities =
        std::make_shared<std::vector<net::ClusterAssetIdentity>>();
    identities->reserve(manifest.assets.size());
    for (const RequiredProjectAsset& asset : manifest.assets) {
        if (!asset.content.identity.has_value()) {
            continue;
        }
        const net::ClusterAssetIdentity identity{
            asset.content.identity->sha256,
            asset.content.identity->byte_size};
        if (std::find(identities->begin(), identities->end(), identity) ==
            identities->end()) {
            identities->push_back(identity);
        }
    }
    const std::size_t managed_count = result.portable
        ? identities->size()
        : manifest.assets.size();
    result.required = static_cast<core::u32>(std::min<std::size_t>(
        managed_count, std::numeric_limits<core::u32>::max()));
    result.required = saturated_add(result.required,
                                    manifest.unregistered_references);
    result.required = saturated_add(result.required,
                                    manifest.kind_mismatches);
    result.identities = std::move(identities);
    return result;
}

[[nodiscard]] bool same_manifest(const ControllerManifest& left,
                                 const ControllerManifest& right) noexcept {
    return left.digest == right.digest && left.required == right.required &&
           left.unverified == right.unverified &&
           left.portable == right.portable && left.identities != nullptr &&
           right.identities != nullptr &&
           *left.identities == *right.identities;
}

[[nodiscard]] bool content_status_ready(ContentStatus status) noexcept {
    return status == ContentStatus::NotRequired ||
           status == ContentStatus::Ready;
}

[[nodiscard]] net::ClusterLifecycleCommand command_for(
    editor::OutputLifecycleAction action) noexcept {
    switch (action) {
    case editor::OutputLifecycleAction::Status:
        return net::ClusterLifecycleCommand::Status;
    case editor::OutputLifecycleAction::Start:
        return net::ClusterLifecycleCommand::Start;
    case editor::OutputLifecycleAction::Restart:
        return net::ClusterLifecycleCommand::Restart;
    case editor::OutputLifecycleAction::Stop:
        return net::ClusterLifecycleCommand::Stop;
    }
    return net::ClusterLifecycleCommand::Status;
}

[[nodiscard]] Status status_for(net::ClusterProcessState state) noexcept {
    switch (state) {
    case net::ClusterProcessState::Stopped:
        return Status::Stopped;
    case net::ClusterProcessState::Starting:
        return Status::Starting;
    case net::ClusterProcessState::Running:
        return Status::Running;
    case net::ClusterProcessState::Stopping:
        return Status::Stopping;
    case net::ClusterProcessState::Faulted:
        return Status::Faulted;
    }
    return Status::Faulted;
}

[[nodiscard]] Status error_status(
    net::ClusterLifecycleClientErrorCode error) noexcept {
    switch (error) {
    case net::ClusterLifecycleClientErrorCode::InvalidConfiguration:
        return Status::InvalidConfiguration;
    case net::ClusterLifecycleClientErrorCode::AuthenticationFailed:
        return Status::AuthenticationFailed;
    case net::ClusterLifecycleClientErrorCode::Protocol:
        return Status::ProtocolError;
    case net::ClusterLifecycleClientErrorCode::OpenFailed:
    case net::ClusterLifecycleClientErrorCode::ConnectFailed:
    case net::ClusterLifecycleClientErrorCode::TimedOut:
    case net::ClusterLifecycleClientErrorCode::Transport:
        return Status::Unreachable;
    case net::ClusterLifecycleClientErrorCode::Cancelled:
    case net::ClusterLifecycleClientErrorCode::AllocationFailed:
        return Status::Busy;
    }
    return Status::ProtocolError;
}

[[nodiscard]] std::string error_detail(
    const net::ClusterLifecycleClientError& error) {
    switch (error.code) {
    case net::ClusterLifecycleClientErrorCode::InvalidConfiguration:
        return "invalid lifecycle client configuration";
    case net::ClusterLifecycleClientErrorCode::AuthenticationFailed:
        return "shared secret rejected by listener";
    case net::ClusterLifecycleClientErrorCode::Protocol:
        return "listener returned an invalid protocol response";
    case net::ClusterLifecycleClientErrorCode::OpenFailed:
        return "could not open lifecycle socket";
    case net::ClusterLifecycleClientErrorCode::ConnectFailed:
        return error.system_error
                   ? "listener connection failed: " +
                         error.system_error.message()
                   : "listener connection failed";
    case net::ClusterLifecycleClientErrorCode::TimedOut:
        return "listener request timed out";
    case net::ClusterLifecycleClientErrorCode::Transport:
        return error.system_error
                   ? "listener transport failed: " +
                         error.system_error.message()
                   : "listener transport failed";
    case net::ClusterLifecycleClientErrorCode::Cancelled:
        return "lifecycle request cancelled";
    case net::ClusterLifecycleClientErrorCode::AllocationFailed:
        return "lifecycle request allocation failed";
    }
    return "lifecycle request failed";
}

[[nodiscard]] bool usable_secret(std::string_view secret) noexcept {
    return secret.size() >= 32U && secret.size() <= 256U;
}

[[nodiscard]] topology::HardwareRuntimeReachability runtime_reachability(
    Status status, bool has_inventory) noexcept {
    using Reachability = topology::HardwareRuntimeReachability;
    switch (status) {
    case Status::Disabled:
        return Reachability::Unmonitored;
    case Status::Probing:
        return Reachability::Probing;
    case Status::Stopped:
    case Status::Starting:
    case Status::Running:
    case Status::Stopping:
    case Status::Faulted:
        return Reachability::Online;
    case Status::Unreachable:
        return Reachability::Offline;
    case Status::Busy:
        return has_inventory ? Reachability::Online : Reachability::Probing;
    case Status::InvalidConfiguration:
    case Status::DuplicateNodeIndex:
    case Status::AuthenticationFailed:
    case Status::ProtocolError:
    case Status::ConfigurationMismatch:
        return Reachability::ConfigurationError;
    }
    return Reachability::Unmonitored;
}

[[nodiscard]] topology::HardwareRuntimeProcessState runtime_process(
    Status status) noexcept {
    using Process = topology::HardwareRuntimeProcessState;
    switch (status) {
    case Status::Stopped:
        return Process::Stopped;
    case Status::Starting:
        return Process::Starting;
    case Status::Running:
        return Process::Running;
    case Status::Stopping:
        return Process::Stopping;
    case Status::Faulted:
        return Process::Faulted;
    case Status::Disabled:
    case Status::InvalidConfiguration:
    case Status::DuplicateNodeIndex:
    case Status::Probing:
    case Status::Unreachable:
    case Status::AuthenticationFailed:
    case Status::ProtocolError:
    case Status::ConfigurationMismatch:
    case Status::Busy:
        return Process::Unknown;
    }
    return Process::Unknown;
}

[[nodiscard]] bool inventory_detail_is_current_for(Status status) noexcept {
    switch (status) {
    case Status::Stopped:
    case Status::Starting:
    case Status::Running:
    case Status::Stopping:
        return true;
    case Status::Disabled:
    case Status::InvalidConfiguration:
    case Status::DuplicateNodeIndex:
    case Status::Probing:
    case Status::Unreachable:
    case Status::AuthenticationFailed:
    case Status::ProtocolError:
    case Status::ConfigurationMismatch:
    case Status::Busy:
    case Status::Faulted:
        return false;
    }
    return false;
}

[[nodiscard]] std::string_view connector_name(
    net::ClusterVideoOutputTechnology technology) noexcept {
    using Technology = net::ClusterVideoOutputTechnology;
    switch (technology) {
    case Technology::Hd15:
        return "VGA";
    case Technology::SVideo:
        return "S-VIDEO";
    case Technology::Composite:
        return "COMPOSITE";
    case Technology::Component:
        return "COMPONENT";
    case Technology::Dvi:
        return "DVI";
    case Technology::Hdmi:
        return "HDMI";
    case Technology::Lvds:
        return "LVDS";
    case Technology::DisplayPort:
        return "DISPLAYPORT";
    case Technology::Internal:
        return "INTERNAL";
    case Technology::Sdi:
        return "SDI";
    case Technology::Miracast:
        return "MIRACAST";
    case Technology::Indirect:
        return "INDIRECT";
    case Technology::Other:
        return "OTHER";
    case Technology::Unknown:
        return "";
    }
    return "";
}

} // namespace

namespace detail {

struct ControllerEntry final {
    doc::NodeId device{};
    std::string name;
    std::string endpoint_text;
    asio::ip::tcp::endpoint endpoint;
    core::u32 node_index{};
    core::u32 width{};
    core::u32 height{};
    Status status{Status::Disabled};
    core::u64 generation{};
    std::string detail;
    std::optional<net::ClusterMachineInfo> machine_info;
    topology::HardwareRuntimeInventoryState machine_info_state{
        topology::HardwareRuntimeInventoryState::Unknown};
    bool machine_info_stale{};
    std::string machine_info_detail;
    // An authenticated inventory response is still unusable if it belongs to
    // another listener identity. This remains latched across ordinary
    // lifecycle Status replies until a matching inventory reply clears it.
    bool machine_info_configuration_mismatch{};
    ContentStatus content{ContentStatus::NotRequired};
    core::u32 assets_required{};
    core::u32 assets_ready{};
    core::u32 assets_missing{};
    core::u32 assets_corrupt{};
    core::u32 assets_unverified{};
    std::string content_detail;
    bool eligible{};
    core::u8 configuration_class{};
    core::u64 revision{1U};
    core::u64 last_sequence{};
    std::size_t pending{};
    std::optional<net::ClusterLifecycleCommand> in_flight;
    std::optional<net::ClusterLifecycleCommand> deferred_mutation;
    bool asset_readiness_pending{};
    std::chrono::steady_clock::time_point next_poll{};
    std::chrono::steady_clock::time_point next_machine_info_poll{};
    std::chrono::steady_clock::time_point next_asset_readiness_poll{};
};

[[nodiscard]] bool output_bindings_observed_and_compatible(
    const topology::HardwareMap& map,
    const ControllerEntry& entry) noexcept {
    bool has_binding{};
    for (const topology::HardwareOutputBinding& binding :
         map.output_bindings) {
        if (binding.render_server == entry.device) {
            has_binding = true;
            break;
        }
    }
    if (!has_binding) {
        return true;
    }
    using Availability = net::ClusterMachineInfoAvailability;
    if (!entry.machine_info.has_value() || entry.machine_info_stale ||
        (entry.machine_info->availability != Availability::Available &&
         entry.machine_info->availability != Availability::Partial) ||
        entry.machine_info->display_state !=
            net::ClusterDisplayEnumerationState::Available) {
        return false;
    }
    for (const topology::HardwareOutputBinding& binding :
         map.output_bindings) {
        if (binding.render_server != entry.device) {
            continue;
        }
        const net::ClusterDisplayTargetInfo* observed{};
        for (const net::ClusterDisplayTargetInfo& candidate :
             entry.machine_info->display_targets) {
            if (candidate.adapter_id != binding.target.adapter_id ||
                candidate.target_id != binding.target.target_id ||
                candidate.connector_instance !=
                    binding.target.connector_instance) {
                continue;
            }
            if (observed != nullptr) {
                return false;
            }
            observed = &candidate;
        }
        if (observed == nullptr || !observed->available ||
            !observed->active) {
            return false;
        }
        if (binding.window_mode ==
                topology::HardwareWindowMode::Fullscreen &&
            (observed->width != binding.width ||
             observed->height != binding.height ||
             observed->refresh_numerator != binding.refresh_numerator ||
             std::max(observed->refresh_denominator, core::u32{1U}) !=
                 binding.refresh_denominator)) {
            return false;
        }
    }
    return true;
}

struct ControllerWork final {
    core::u64 device{};
    core::u64 revision{};
    core::u64 sequence{};
    asio::ip::tcp::endpoint endpoint;
    core::u32 node_index{};
    net::ClusterLifecycleCommand command{net::ClusterLifecycleCommand::Status};
    bool request_machine_info{};
};

struct ControllerAssetReadinessWork final {
    core::u64 device{};
    core::u64 revision{};
    asio::ip::tcp::endpoint endpoint;
    core::u32 node_index{};
    std::shared_ptr<const std::vector<net::ClusterAssetIdentity>>
        asset_identities;
};

class ClusterLifecycleControllerState final {
public:
    ClusterLifecycleControllerState(
        ClusterLifecycleControllerConfig configured,
        ClusterLifecycleRequestFunction requester,
        ClusterMachineInfoRequestFunction machine_info_requester,
        ClusterAssetReadinessRequestFunction asset_readiness_requester)
        : config(std::move(configured)), request(std::move(requester)),
          machine_info_request(std::move(machine_info_requester)),
          asset_readiness_request(std::move(asset_readiness_requester)) {
        config.worker_count = std::clamp(
            config.worker_count, std::size_t{1U}, std::size_t{8U});
        if (config.poll_interval <= std::chrono::milliseconds::zero()) {
            config.poll_interval = std::chrono::milliseconds{2'000};
        }
        if (config.machine_info_poll_interval <=
            std::chrono::milliseconds::zero()) {
            config.machine_info_poll_interval =
                std::chrono::milliseconds{10'000};
        }
        if (config.asset_readiness_poll_interval <=
            std::chrono::milliseconds::zero()) {
            config.asset_readiness_poll_interval =
                std::chrono::milliseconds{30'000};
        }
        config.client.shared_secret = config.shared_secret;
        if (!request) {
            const net::ClusterLifecycleClientConfig client = config.client;
            request = [client](const asio::ip::tcp::endpoint& endpoint,
                               core::u32 node_index,
                               net::ClusterLifecycleCommand command) {
                return net::request_cluster_lifecycle(endpoint, node_index,
                                                      command, client);
            };
        }
        if (!machine_info_request) {
            const net::ClusterLifecycleClientConfig client = config.client;
            machine_info_request =
                [client](const asio::ip::tcp::endpoint& endpoint,
                         core::u32 node_index) {
                    return net::request_cluster_machine_info(
                        endpoint, node_index, client);
                };
        }
        if (!asset_readiness_request) {
            const net::ClusterLifecycleClientConfig client = config.client;
            asset_readiness_request =
                [client](const asio::ip::tcp::endpoint& endpoint,
                         core::u32 node_index,
                         std::span<const net::ClusterAssetIdentity> identities,
                         std::stop_token stop) {
                    return net::request_cluster_asset_readiness(
                        endpoint, node_index, identities, client, stop);
                };
        }
        try {
            workers.reserve(config.worker_count);
            for (std::size_t index = 0U; index < config.worker_count; ++index) {
                workers.emplace_back([this] { worker(); });
            }
            constexpr std::size_t asset_readiness_worker_count = 2U;
            asset_readiness_workers.reserve(asset_readiness_worker_count);
            for (std::size_t index = 0U;
                 index < asset_readiness_worker_count; ++index) {
                asset_readiness_workers.emplace_back(
                    [this] { asset_readiness_worker(); });
            }
        } catch (...) {
            {
                const std::lock_guard lock{mutex};
                stopping = true;
            }
            static_cast<void>(asset_readiness_stop.request_stop());
            changed.notify_all();
            asset_readiness_changed.notify_all();
            for (std::thread& worker_thread : workers) {
                if (worker_thread.joinable()) {
                    worker_thread.join();
                }
            }
            for (std::thread& readiness_worker : asset_readiness_workers) {
                if (readiness_worker.joinable()) {
                    readiness_worker.join();
                }
            }
            throw;
        }
    }

    ~ClusterLifecycleControllerState() {
        {
            const std::lock_guard lock{mutex};
            stopping = true;
            queue.clear();
            asset_readiness_queue.clear();
        }
        static_cast<void>(asset_readiness_stop.request_stop());
        changed.notify_all();
        asset_readiness_changed.notify_all();
        for (std::thread& worker_thread : workers) {
            if (worker_thread.joinable()) {
                worker_thread.join();
            }
        }
        for (std::thread& readiness_worker : asset_readiness_workers) {
            if (readiness_worker.joinable()) {
                readiness_worker.join();
            }
        }
    }

    void remove_queued(core::u64 device, ControllerEntry& entry) {
        for (auto iterator = queue.begin(); iterator != queue.end();) {
            if (iterator->device == device &&
                iterator->revision == entry.revision) {
                if (entry.pending > 0U) {
                    --entry.pending;
                }
                iterator = queue.erase(iterator);
            } else {
                ++iterator;
            }
        }
        bool removed_asset_readiness{};
        for (auto iterator = asset_readiness_queue.begin();
             iterator != asset_readiness_queue.end();) {
            if (iterator->device == device &&
                iterator->revision == entry.revision) {
                removed_asset_readiness = true;
                iterator = asset_readiness_queue.erase(iterator);
            } else {
                ++iterator;
            }
        }
        if (removed_asset_readiness) {
            entry.asset_readiness_pending = false;
        }
    }

    void enqueue_asset_readiness(
        ControllerEntry& entry,
        std::chrono::steady_clock::time_point now) {
        if (entry.asset_readiness_pending || !manifest.portable ||
            manifest.identities == nullptr || manifest.identities->empty() ||
            now < entry.next_asset_readiness_poll) {
            return;
        }
        constexpr std::size_t maximum_queue = 512U;
        if (asset_readiness_queue.size() >= maximum_queue) {
            entry.content = ContentStatus::Error;
            entry.content_detail = "asset readiness work queue is full";
            return;
        }
        entry.next_asset_readiness_poll =
            now + config.asset_readiness_poll_interval;
        if (entry.content != ContentStatus::Ready) {
            entry.content = ContentStatus::Checking;
            entry.content_detail = "checking verified asset store";
        }
        entry.asset_readiness_pending = true;
        asset_readiness_queue.push_back(ControllerAssetReadinessWork{
            entry.device.value(), entry.revision, entry.endpoint,
            entry.node_index, manifest.identities});
        asset_readiness_changed.notify_one();
    }

    void enqueue(ControllerEntry& entry,
                 net::ClusterLifecycleCommand command) {
        const auto now = std::chrono::steady_clock::now();
        if (command == net::ClusterLifecycleCommand::Status) {
            enqueue_asset_readiness(entry, now);
        }
        constexpr std::size_t maximum_queue = 512U;
        if (queue.size() >= maximum_queue) {
            entry.status = Status::Busy;
            entry.detail = "lifecycle work queue is full";
            return;
        }
        const core::u64 sequence = next_sequence++;
        const bool include_machine_info =
            command == net::ClusterLifecycleCommand::Status &&
            now >= entry.next_machine_info_poll;
        if (include_machine_info) {
            entry.next_machine_info_poll =
                now + config.machine_info_poll_interval;
        }
        entry.last_sequence = sequence;
        ++entry.pending;
        queue.push_back(ControllerWork{
            entry.device.value(), entry.revision, sequence, entry.endpoint,
            entry.node_index, command, include_machine_info});
        changed.notify_one();
    }

    void worker() noexcept {
        for (;;) {
            ControllerWork work{};
            {
                std::unique_lock lock{mutex};
                changed.wait(lock, [&] { return stopping || !queue.empty(); });
                if (stopping) {
                    return;
                }
                work = queue.front();
                queue.pop_front();
                ++active;
                const auto found = entries.find(work.device);
                if (found != entries.end() &&
                    found->second.revision == work.revision) {
                    found->second.in_flight = work.command;
                }
            }

            ClusterLifecycleRequestResult outcome = [&] {
                try {
                    return request(work.endpoint, work.node_index,
                                   work.command);
                } catch (...) {
                    return ClusterLifecycleRequestResult::failure(
                        {net::ClusterLifecycleClientErrorCode::AllocationFailed,
                         {}});
                }
            }();

            std::optional<ClusterMachineInfoRequestResult>
                machine_info_outcome;
            if (work.request_machine_info && outcome) {
                try {
                    machine_info_outcome.emplace(machine_info_request(
                        work.endpoint, work.node_index));
                } catch (...) {
                    machine_info_outcome.emplace(
                        ClusterMachineInfoRequestResult::failure(
                            {net::ClusterLifecycleClientErrorCode::
                                 AllocationFailed,
                             {}}));
                }
            }

            {
                const std::lock_guard lock{mutex};
                --active;
                const auto found = entries.find(work.device);
                if (found != entries.end() &&
                    found->second.revision == work.revision) {
                    ControllerEntry& entry = found->second;
                    if (entry.pending > 0U) {
                        --entry.pending;
                    }
                    entry.in_flight.reset();
                    if (work.request_machine_info) {
                        try {
                            if (machine_info_outcome.has_value() &&
                                *machine_info_outcome) {
                                net::ClusterMachineInfo info =
                                    std::move(*machine_info_outcome).value();
                                using Availability =
                                    net::ClusterMachineInfoAvailability;
                                if (info.node_index != work.node_index) {
                                    entry.machine_info.reset();
                                    entry.machine_info_state = topology::
                                        HardwareRuntimeInventoryState::Unknown;
                                    entry.machine_info_stale = false;
                                    entry.machine_info_configuration_mismatch =
                                        true;
                                    entry.machine_info_detail =
                                        "listener returned inventory for a "
                                        "different node index";
                                } else {
                                  switch (info.availability) {
                                case Availability::Available:
                                case Availability::Partial:
                                    entry.machine_info_configuration_mismatch =
                                        false;
                                    switch (info.display_state) {
                                    case net::ClusterDisplayEnumerationState::
                                        Available:
                                        entry.machine_info_state =
                                            info.availability ==
                                                        Availability::Available &&
                                                    !info.truncated
                                                ? topology::
                                                      HardwareRuntimeInventoryState::
                                                          Available
                                                : topology::
                                                      HardwareRuntimeInventoryState::
                                                          Partial;
                                        break;
                                    case net::ClusterDisplayEnumerationState::
                                        Unsupported:
                                        entry.machine_info_state = topology::
                                            HardwareRuntimeInventoryState::
                                                Unsupported;
                                        break;
                                    case net::ClusterDisplayEnumerationState::
                                        AccessDenied:
                                    case net::ClusterDisplayEnumerationState::
                                        Failed:
                                        entry.machine_info_state = topology::
                                            HardwareRuntimeInventoryState::
                                                Failed;
                                        break;
                                    }
                                    entry.machine_info = std::move(info);
                                    entry.machine_info_stale = false;
                                    entry.machine_info_detail =
                                        entry.machine_info->message;
                                    break;
                                case Availability::Unavailable:
                                case Availability::Failed:
                                    entry.machine_info_configuration_mismatch =
                                        false;
                                    entry.machine_info_stale =
                                        entry.machine_info.has_value();
                                    if (!entry.machine_info.has_value()) {
                                        entry.machine_info_state =
                                            info.availability ==
                                                    Availability::Unavailable
                                                ? topology::
                                                      HardwareRuntimeInventoryState::
                                                          Unsupported
                                                : topology::
                                                      HardwareRuntimeInventoryState::
                                                          Failed;
                                    }
                                    entry.machine_info_detail = info.message;
                                    break;
                                case Availability::ConfigurationMismatch:
                                    entry.machine_info.reset();
                                    entry.machine_info_state = topology::
                                        HardwareRuntimeInventoryState::Unknown;
                                    entry.machine_info_stale = false;
                                    entry.machine_info_configuration_mismatch =
                                        true;
                                    entry.machine_info_detail =
                                        "listener owns a different node index";
                                    break;
                                }
                                }
                            } else if (machine_info_outcome.has_value()) {
                                entry.machine_info_stale =
                                    entry.machine_info.has_value();
                                if (!entry.machine_info.has_value()) {
                                    entry.machine_info_state = topology::
                                        HardwareRuntimeInventoryState::Failed;
                                }
                                entry.machine_info_detail = error_detail(
                                    machine_info_outcome->error());
                            } else {
                                entry.machine_info_stale =
                                    entry.machine_info.has_value();
                            }
                        } catch (...) {
                            entry.machine_info_stale =
                                entry.machine_info.has_value();
                            entry.machine_info_detail.clear();
                        }
                    }
                    if (entry.deferred_mutation.has_value() && entry.eligible) {
                        const net::ClusterLifecycleCommand deferred =
                            *entry.deferred_mutation;
                        entry.deferred_mutation.reset();
                        enqueue(entry, deferred);
                    } else if (work.sequence == entry.last_sequence) {
                        if (outcome) {
                            const net::ClusterLifecycleReply& response =
                                outcome.value();
                            try {
                                entry.generation = response.generation;
                                entry.detail = response.message;
                                switch (response.outcome) {
                                case net::ClusterLifecycleOutcome::ConfigMismatch:
                                    entry.status = Status::ConfigurationMismatch;
                                    break;
                                case net::ClusterLifecycleOutcome::Busy:
                                    entry.status = Status::Busy;
                                    break;
                                case net::ClusterLifecycleOutcome::Failed:
                                    entry.status = Status::Faulted;
                                    break;
                                case net::ClusterLifecycleOutcome::Applied:
                                case net::ClusterLifecycleOutcome::NoChange:
                                    entry.status = status_for(response.state);
                                    break;
                                }
                            } catch (...) {
                                entry.status = Status::Busy;
                                entry.detail.clear();
                            }
                        } else {
                            entry.status = error_status(outcome.error().code);
                            entry.machine_info_stale =
                                entry.machine_info.has_value();
                            try {
                                entry.detail = error_detail(outcome.error());
                            } catch (...) {
                                entry.status = Status::Busy;
                                entry.detail.clear();
                            }
                        }
                        entry.next_poll = std::chrono::steady_clock::now() +
                                          config.poll_interval;
                    }
                    if (entry.machine_info_configuration_mismatch) {
                        entry.status = Status::ConfigurationMismatch;
                        try {
                            entry.detail =
                                "listener returned inventory for a different "
                                "node index";
                        } catch (...) {
                            entry.detail.clear();
                        }
                    }
                }
                idle_changed.notify_all();
            }
        }
    }

    [[nodiscard]] ClusterAssetReadinessRequestResult
    request_asset_readiness_batches(
        const ControllerAssetReadinessWork& work,
        std::stop_token stop) noexcept {
        try {
            if (stop.stop_requested()) {
                return ClusterAssetReadinessRequestResult::failure(
                    {net::ClusterLifecycleClientErrorCode::Cancelled, {}});
            }
            if (work.asset_identities == nullptr) {
                return ClusterAssetReadinessRequestResult::failure(
                    {net::ClusterLifecycleClientErrorCode::Protocol, {}});
            }
            net::ClusterAssetReadinessReply aggregate{};
            aggregate.availability =
                net::ClusterAssetReadinessAvailability::Available;
            aggregate.node_index = work.node_index;
            const auto& identities = *work.asset_identities;
            for (std::size_t offset = 0U; offset < identities.size();) {
                if (stop.stop_requested()) {
                    return ClusterAssetReadinessRequestResult::failure(
                        {net::ClusterLifecycleClientErrorCode::Cancelled,
                         {}});
                }
                const std::size_t amount = std::min(
                    net::max_cluster_asset_readiness_entries,
                    identities.size() - offset);
                const std::span<const net::ClusterAssetIdentity> batch{
                    identities.data() + offset, amount};
                ClusterAssetReadinessRequestResult requested = [&] {
                    try {
                        return asset_readiness_request(
                            work.endpoint, work.node_index, batch, stop);
                    } catch (...) {
                        return ClusterAssetReadinessRequestResult::failure(
                            {net::ClusterLifecycleClientErrorCode::
                                 AllocationFailed,
                             {}});
                    }
                }();
                if (stop.stop_requested()) {
                    return ClusterAssetReadinessRequestResult::failure(
                        {net::ClusterLifecycleClientErrorCode::Cancelled,
                         {}});
                }
                if (!requested) {
                    return requested;
                }
                net::ClusterAssetReadinessReply reply =
                    std::move(requested).value();
                if (reply.node_index != work.node_index) {
                    return ClusterAssetReadinessRequestResult::failure(
                        {net::ClusterLifecycleClientErrorCode::Protocol, {}});
                }
                if (reply.availability !=
                    net::ClusterAssetReadinessAvailability::Available) {
                    reply.entries.clear();
                    reply.ready = 0U;
                    reply.missing = 0U;
                    reply.corrupt = 0U;
                    return ClusterAssetReadinessRequestResult::success(
                        std::move(reply));
                }
                core::u32 ready{};
                core::u32 missing{};
                core::u32 corrupt{};
                if (reply.entries.size() != amount) {
                    return ClusterAssetReadinessRequestResult::failure(
                        {net::ClusterLifecycleClientErrorCode::Protocol, {}});
                }
                for (std::size_t index = 0U; index < amount; ++index) {
                    const auto& entry = reply.entries[index];
                    if (entry.identity != batch[index]) {
                        return ClusterAssetReadinessRequestResult::failure(
                            {net::ClusterLifecycleClientErrorCode::Protocol,
                             {}});
                    }
                    switch (entry.state) {
                    case net::ClusterAssetReadinessState::Ready:
                        ++ready;
                        break;
                    case net::ClusterAssetReadinessState::Missing:
                        ++missing;
                        break;
                    case net::ClusterAssetReadinessState::Corrupt:
                        ++corrupt;
                        break;
                    }
                }
                if (reply.ready != ready || reply.missing != missing ||
                    reply.corrupt != corrupt) {
                    return ClusterAssetReadinessRequestResult::failure(
                        {net::ClusterLifecycleClientErrorCode::Protocol, {}});
                }
                aggregate.ready = saturated_add(aggregate.ready, ready);
                aggregate.missing =
                    saturated_add(aggregate.missing, missing);
                aggregate.corrupt =
                    saturated_add(aggregate.corrupt, corrupt);
                aggregate.entries.insert(aggregate.entries.end(),
                                         reply.entries.begin(),
                                         reply.entries.end());
                offset += amount;
            }
            if (stop.stop_requested()) {
                return ClusterAssetReadinessRequestResult::failure(
                    {net::ClusterLifecycleClientErrorCode::Cancelled, {}});
            }
            return ClusterAssetReadinessRequestResult::success(
                std::move(aggregate));
        } catch (...) {
            return ClusterAssetReadinessRequestResult::failure(
                {net::ClusterLifecycleClientErrorCode::AllocationFailed, {}});
        }
    }

    void apply_asset_readiness(
        ControllerEntry& entry,
        const ClusterAssetReadinessRequestResult& outcome) noexcept {
        entry.assets_ready = 0U;
        entry.assets_missing = 0U;
        entry.assets_corrupt = 0U;
        entry.assets_unverified = 0U;
        try {
            if (!outcome) {
                entry.content = ContentStatus::Error;
                entry.content_detail = error_detail(outcome.error());
                return;
            }
            const auto& readiness = outcome.value();
            entry.assets_ready = readiness.ready;
            entry.assets_missing = readiness.missing;
            entry.assets_corrupt = readiness.corrupt;
            using Availability = net::ClusterAssetReadinessAvailability;
            switch (readiness.availability) {
            case Availability::Available:
                if (entry.assets_corrupt > 0U) {
                    entry.content = ContentStatus::Corrupt;
                    entry.content_detail =
                        "replace damaged shared-store content";
                } else if (entry.assets_missing > 0U) {
                    entry.content = ContentStatus::Missing;
                    entry.content_detail =
                        "copy required content to this node's store";
                } else if (entry.assets_ready == entry.assets_required) {
                    entry.content = ContentStatus::Ready;
                    entry.content_detail = "verified by lifecycle agent";
                } else {
                    entry.content = ContentStatus::Error;
                    entry.content_detail = "asset readiness count mismatch";
                }
                break;
            case Availability::Unavailable:
                entry.content = ContentStatus::Error;
                entry.content_detail =
                    "listener does not expose asset readiness";
                break;
            case Availability::Failed:
                entry.content = ContentStatus::Error;
                entry.content_detail =
                    "listener could not inspect its asset store";
                break;
            case Availability::ConfigurationMismatch:
                entry.content = ContentStatus::Error;
                entry.content_detail =
                    "listener asset identity does not match this node";
                break;
            }
        } catch (...) {
            entry.content = ContentStatus::Error;
            entry.content_detail.clear();
        }
    }

    void asset_readiness_worker() noexcept {
        for (;;) {
            ControllerAssetReadinessWork work{};
            {
                std::unique_lock lock{mutex};
                asset_readiness_changed.wait(lock, [&] {
                    return stopping || !asset_readiness_queue.empty();
                });
                if (stopping) {
                    return;
                }
                work = std::move(asset_readiness_queue.front());
                asset_readiness_queue.pop_front();
                ++active_asset_readiness;
            }

            const ClusterAssetReadinessRequestResult outcome =
                request_asset_readiness_batches(
                    work, asset_readiness_stop.get_token());

            {
                const std::lock_guard lock{mutex};
                --active_asset_readiness;
                const auto found = entries.find(work.device);
                if (!stopping && found != entries.end() &&
                    found->second.revision == work.revision) {
                    ControllerEntry& entry = found->second;
                    entry.asset_readiness_pending = false;
                    apply_asset_readiness(entry, outcome);
                }
                idle_changed.notify_all();
            }
        }
    }

    ClusterLifecycleControllerConfig config;
    ClusterLifecycleRequestFunction request;
    ClusterMachineInfoRequestFunction machine_info_request;
    ClusterAssetReadinessRequestFunction asset_readiness_request;
    ControllerManifest manifest = controller_manifest(ProjectAssetManifest{});
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::condition_variable asset_readiness_changed;
    std::condition_variable idle_changed;
    std::map<core::u64, ControllerEntry> entries;
    std::deque<ControllerWork> queue;
    std::deque<ControllerAssetReadinessWork> asset_readiness_queue;
    std::vector<std::thread> workers;
    std::vector<std::thread> asset_readiness_workers;
    std::stop_source asset_readiness_stop;
    core::u64 next_sequence{1U};
    core::u64 next_revision{1U};
    core::u64 project_generation{};
    bool project_generation_initialized{};
    core::u64 deployment_digest{};
    bool deployment_digest_initialized{};
    std::size_t active{};
    std::size_t active_asset_readiness{};
    bool stopping{};
};

} // namespace detail

ClusterLifecycleController::ClusterLifecycleController(
    ClusterLifecycleControllerConfig config,
    ClusterLifecycleRequestFunction request,
    ClusterMachineInfoRequestFunction machine_info_request,
    ClusterAssetReadinessRequestFunction asset_readiness_request)
    : state_(std::make_unique<detail::ClusterLifecycleControllerState>(
          std::move(config), std::move(request),
          std::move(machine_info_request),
          std::move(asset_readiness_request))) {}

ClusterLifecycleController::~ClusterLifecycleController() = default;

void ClusterLifecycleController::sync(const topology::HardwareMap& map,
                                      core::u64 project_generation) {
    sync(map, ProjectAssetManifest{}, project_generation);
}

void ClusterLifecycleController::sync(
    const topology::HardwareMap& map,
    const ProjectAssetManifest& project_manifest,
    core::u64 project_generation) {
    const ControllerManifest requested_manifest =
        controller_manifest(project_manifest);
    const core::u64 requested_deployment_digest =
        controller_deployment_digest(map, state_->config.shared_secret);
    const auto now = std::chrono::steady_clock::now();
    std::unordered_map<core::u32, std::size_t> node_index_counts;
    for (const topology::HardwareDevice& device : map.devices) {
        if (device.kind == topology::HardwareDeviceKind::RenderServer) {
            ++node_index_counts[device.node_index];
        }
    }

    const std::lock_guard lock{state_->mutex};
    const bool generation_changed =
        !state_->project_generation_initialized ||
        state_->project_generation != project_generation;
    if (generation_changed) {
        state_->project_generation_initialized = true;
        state_->project_generation = project_generation;
        state_->queue.clear();
        state_->asset_readiness_queue.clear();
        state_->entries.clear();
    }
    const bool manifest_changed =
        !same_manifest(state_->manifest, requested_manifest);
    const bool deployment_changed =
        !state_->deployment_digest_initialized ||
        state_->deployment_digest != requested_deployment_digest;
    if (manifest_changed || deployment_changed) {
        for (auto& [id, entry] : state_->entries) {
            state_->remove_queued(id, entry);
            entry.revision = state_->next_revision++;
            entry.pending = 0U;
            entry.in_flight.reset();
            entry.deferred_mutation.reset();
            entry.asset_readiness_pending = false;
        }
    }
    state_->manifest = requested_manifest;
    state_->deployment_digest = requested_deployment_digest;
    state_->deployment_digest_initialized = true;
    const auto reset_content = [&](detail::ControllerEntry& entry,
                                   bool eligible) {
        entry.assets_required = state_->manifest.required;
        entry.assets_ready = 0U;
        entry.assets_missing = 0U;
        entry.assets_corrupt = 0U;
        entry.assets_unverified = 0U;
        entry.next_asset_readiness_poll = now;
        if (state_->manifest.required == 0U) {
            entry.content = ContentStatus::NotRequired;
            entry.content_detail.clear();
        } else if (!state_->manifest.portable) {
            entry.content = ContentStatus::Unverified;
            entry.assets_unverified = state_->manifest.unverified;
            entry.content_detail =
                "re-import unmanaged or compound assets before cluster launch";
        } else if (!eligible) {
            entry.content = ContentStatus::Error;
            entry.content_detail =
                "configure lifecycle control before checking node assets";
        } else {
            entry.content = ContentStatus::Checking;
            entry.content_detail = "checking verified asset store";
        }
    };
    std::set<core::u64> seen;
    for (const topology::HardwareDevice& device : map.devices) {
        if (device.kind != topology::HardwareDeviceKind::RenderServer) {
            continue;
        }
        const core::u64 id = device.node.value();
        seen.insert(id);
        auto [position, inserted] = state_->entries.try_emplace(id);
        detail::ControllerEntry& entry = position->second;
        if (inserted) {
            entry.revision = state_->next_revision++;
        }
        const auto resolved_endpoint =
            topology::lifecycle_endpoint_for(map, device);
        const std::string endpoint_text =
            resolved_endpoint.value_or(std::string{});
        const auto parsed = net::parse_lifecycle_endpoint(endpoint_text);
        const bool changed = inserted || entry.name != device.name ||
                             entry.endpoint_text != endpoint_text ||
                             entry.node_index != device.node_index ||
                             entry.width != device.width ||
                             entry.height != device.height;
        if (changed) {
            if (!inserted) {
                state_->remove_queued(id, entry);
                entry.revision = state_->next_revision++;
                entry.pending = 0U;
                entry.in_flight.reset();
                entry.deferred_mutation.reset();
                entry.asset_readiness_pending = false;
            }
            entry.device = device.node;
            entry.name = device.name;
            entry.endpoint_text = endpoint_text;
            entry.node_index = device.node_index;
            entry.width = device.width;
            entry.height = device.height;
            entry.generation = 0U;
            entry.machine_info.reset();
            entry.machine_info_state =
                topology::HardwareRuntimeInventoryState::Unknown;
            entry.machine_info_stale = false;
            entry.machine_info_detail.clear();
            entry.machine_info_configuration_mismatch = false;
            entry.next_poll = now;
            entry.next_machine_info_poll = now;
        }

        const bool secret_available = usable_secret(state_->config.shared_secret);
        const bool duplicate = node_index_counts[device.node_index] > 1U;
        const bool eligible = secret_available && parsed.has_value() &&
                              !duplicate;
        const core::u8 configuration_class =
            !secret_available ? 0U : (!parsed ? 1U : (duplicate ? 2U : 3U));
        const bool configuration_changed =
            !changed && entry.configuration_class != configuration_class;
        if (configuration_changed) {
            state_->remove_queued(id, entry);
            entry.revision = state_->next_revision++;
            entry.pending = 0U;
            entry.in_flight.reset();
            entry.deferred_mutation.reset();
            entry.asset_readiness_pending = false;
            entry.machine_info.reset();
            entry.machine_info_state =
                topology::HardwareRuntimeInventoryState::Unknown;
            entry.machine_info_stale = false;
            entry.machine_info_detail.clear();
            entry.machine_info_configuration_mismatch = false;
            entry.next_poll = now;
            entry.next_machine_info_poll = now;
        }
        entry.configuration_class = configuration_class;
        if (inserted || changed || manifest_changed || deployment_changed ||
            configuration_changed) {
            reset_content(entry, eligible);
        }
        if (!eligible) {
            state_->remove_queued(id, entry);
            entry.eligible = false;
            if (!secret_available) {
                entry.status = Status::Disabled;
                entry.detail = state_->config.shared_secret.empty()
                                   ? "launch controls require "
                                     "--lifecycle-secret-file"
                                   : "lifecycle secret must be at least 32 bytes";
            } else if (!parsed) {
                entry.status = Status::InvalidConfiguration;
                entry.detail =
                    "set a valid numeric Host IP and project Control port "
                    "(1-61440) in Hardware Mapping";
            } else {
                entry.status = Status::DuplicateNodeIndex;
                entry.detail = "CONFIG ERROR: node index " +
                               std::to_string(device.node_index) +
                               " is assigned to multiple Render Servers";
            }
            continue;
        }

        entry.endpoint = *parsed;
        if (!entry.eligible || changed) {
            entry.status = Status::Probing;
            entry.detail = "checking lifecycle listener";
        }
        entry.eligible = true;
        state_->enqueue_asset_readiness(entry, now);
        if (entry.pending == 0U && now >= entry.next_poll) {
            state_->enqueue(entry, net::ClusterLifecycleCommand::Status);
        }
    }
    for (auto iterator = state_->entries.begin();
         iterator != state_->entries.end();) {
        if (!seen.contains(iterator->first)) {
            state_->remove_queued(iterator->first, iterator->second);
            iterator = state_->entries.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

std::vector<editor::OutputMonitorState::Node>
ClusterLifecycleController::snapshot() const {
    const std::lock_guard lock{state_->mutex};
    std::vector<editor::OutputMonitorState::Node> rows;
    rows.reserve(state_->entries.size());
    for (const auto& [id, entry] : state_->entries) {
        static_cast<void>(id);
        editor::OutputMonitorState::Node row{
            entry.device, entry.name, entry.endpoint_text, entry.node_index,
            entry.width, entry.height, entry.status, entry.generation,
            entry.detail};
        row.content = entry.content;
        row.assets_required = entry.assets_required;
        row.assets_ready = entry.assets_ready;
        row.assets_missing = entry.assets_missing;
        row.assets_corrupt = entry.assets_corrupt;
        row.assets_unverified = entry.assets_unverified;
        row.content_detail = entry.content_detail;
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(), [](const auto& left,
                                           const auto& right) {
        return left.node_index < right.node_index ||
               (left.node_index == right.node_index &&
                left.device.value() < right.device.value());
    });
    return rows;
}

topology::HardwareRuntimeSnapshot
ClusterLifecycleController::hardware_snapshot() const {
    const std::lock_guard lock{state_->mutex};
    topology::HardwareRuntimeSnapshot snapshot{};
    snapshot.nodes.reserve(state_->entries.size());
    for (const auto& [id, entry] : state_->entries) {
        static_cast<void>(id);
        topology::HardwareRuntimeNode runtime{};
        runtime.device = entry.device;
        runtime.reachability = runtime_reachability(
            entry.status, entry.machine_info.has_value());
        runtime.process = runtime_process(entry.status);
        runtime.inventory = entry.machine_info_state;
        runtime.stale = entry.machine_info_stale ||
            (entry.machine_info.has_value() &&
             runtime.reachability ==
                 topology::HardwareRuntimeReachability::Offline);
        runtime.detail =
            inventory_detail_is_current_for(entry.status) &&
                    !entry.machine_info_detail.empty()
                ? entry.machine_info_detail
                : entry.detail;
        if (entry.machine_info.has_value()) {
            const net::ClusterMachineInfo& info = *entry.machine_info;
            runtime.hostname = info.host_name;
            runtime.operating_system = info.operating_system;
            runtime.logical_processor_count = info.logical_cpu_count;
            runtime.total_memory_bytes = info.physical_memory_bytes;
            const std::size_t adapter_count = std::min(
                info.adapters.size(), net::max_cluster_machine_gpu_adapters);
            runtime.gpu_adapters.reserve(adapter_count);
            for (std::size_t index = 0U; index < adapter_count; ++index) {
                const net::ClusterGpuAdapterInfo& adapter =
                    info.adapters[index];
                runtime.gpu_adapters.push_back(topology::HardwareGpuAdapter{
                    adapter.name, adapter.dedicated_video_memory_bytes,
                    adapter.software, adapter.remote});
            }
            const std::size_t target_count = std::min(
                info.display_targets.size(),
                net::max_cluster_machine_display_targets);
            runtime.video_outputs.reserve(target_count);
            for (std::size_t index = 0U; index < target_count; ++index) {
                const net::ClusterDisplayTargetInfo& target =
                    info.display_targets[index];
                std::string name = target.name;
                if (name.empty()) {
                    name = "DISPLAY " + std::to_string(target.target_id);
                }
                runtime.video_outputs.push_back(topology::HardwareVideoOutput{
                    topology::HardwareDisplayTargetKey{
                        target.adapter_id, target.target_id,
                        target.connector_instance},
                    std::move(name),
                    std::string{connector_name(target.technology)},
                    target.width,
                    target.height,
                    target.refresh_numerator,
                    std::max(target.refresh_denominator, core::u32{1U}),
                    target.active, target.available});
            }
        }
        snapshot.nodes.push_back(std::move(runtime));
    }
    return snapshot;
}

void ClusterLifecycleController::submit(
    const editor::OutputLifecycleRequest& request) {
    const std::lock_guard lock{state_->mutex};
    const auto submit_entry = [&](detail::ControllerEntry& entry) {
        if (!entry.eligible) {
            return;
        }
        if ((request.action == editor::OutputLifecycleAction::Start ||
             request.action == editor::OutputLifecycleAction::Restart) &&
            !content_status_ready(entry.content)) {
            return;
        }
        const net::ClusterLifecycleCommand command = command_for(request.action);
        state_->remove_queued(entry.device.value(), entry);
        switch (request.action) {
        case editor::OutputLifecycleAction::Start:
        case editor::OutputLifecycleAction::Restart:
            entry.status = Status::Starting;
            entry.detail = request.action == editor::OutputLifecycleAction::Start
                               ? "start requested"
                               : "restart requested";
            break;
        case editor::OutputLifecycleAction::Stop:
            entry.status = Status::Stopping;
            entry.detail = "stop requested";
            break;
        case editor::OutputLifecycleAction::Status:
            entry.status = Status::Probing;
            entry.detail = "checking lifecycle listener";
            break;
        }
        if (entry.pending > 0U) {
            if (entry.in_flight == net::ClusterLifecycleCommand::Status &&
                !entry.deferred_mutation.has_value() &&
                command != net::ClusterLifecycleCommand::Status) {
                entry.deferred_mutation = command;
            } else {
                entry.status = Status::Busy;
                entry.detail = "another lifecycle request is in progress";
            }
            return;
        }
        state_->enqueue(entry, command);
    };
    if (request.all) {
        for (auto& [id, entry] : state_->entries) {
            static_cast<void>(id);
            submit_entry(entry);
        }
        return;
    }
    const auto found = state_->entries.find(request.device.value());
    if (found == state_->entries.end() ||
        found->second.node_index != request.node_index ||
        found->second.endpoint_text != request.endpoint) {
        return;
    }
    submit_entry(found->second);
}

bool ClusterLifecycleController::content_ready(
    const ProjectAssetManifest& project_manifest) const noexcept {
    try {
        const ControllerManifest requested =
            controller_manifest(project_manifest);
        const std::lock_guard lock{state_->mutex};
        if (!same_manifest(state_->manifest, requested)) {
            return false;
        }
        if (state_->entries.empty() || requested.required == 0U) {
            return true;
        }
        return std::all_of(
            state_->entries.begin(), state_->entries.end(),
            [](const auto& item) noexcept {
                return content_status_ready(item.second.content);
            });
    } catch (...) {
        return false;
    }
}

bool ClusterLifecycleController::program_ready(
    const ProjectAssetManifest& project_manifest,
    const topology::HardwareMap& projected_hardware_map,
    core::u64 project_generation,
    const net::DirectorFrameBarrier& barrier) const noexcept {
    try {
        const ControllerManifest requested_manifest =
            controller_manifest(project_manifest);
        const core::u64 requested_deployment_digest =
            controller_deployment_digest(
                projected_hardware_map, state_->config.shared_secret);
        std::array<core::u32, topology::max_hardware_devices>
            required_node_indices{};

        // This lock intentionally remains held through the barrier query. It
        // fences project generation, deployment configuration, entry revision,
        // and store-readiness observations into one controller snapshot. The
        // global order is controller -> barrier; DirectorFrameBarrier never
        // calls back into this controller.
        const std::lock_guard lock{state_->mutex};
        if (!state_->project_generation_initialized ||
            state_->project_generation != project_generation ||
            !state_->deployment_digest_initialized ||
            state_->deployment_digest != requested_deployment_digest ||
            !same_manifest(state_->manifest, requested_manifest) ||
            state_->entries.size() > required_node_indices.size()) {
            return false;
        }

        std::size_t required_count{};
        for (const auto& [id, entry] : state_->entries) {
            static_cast<void>(id);
            // Store readiness and lifecycle configuration are independent of
            // runtime adoption. An invalid endpoint, missing secret, duplicate
            // node index, or stale store reply must fail before promotion.
            if (!entry.eligible || !content_status_ready(entry.content)) {
                return false;
            }
            if (!detail::output_bindings_observed_and_compatible(
                    projected_hardware_map, entry)) {
                return false;
            }
            required_node_indices[required_count] = entry.node_index;
            ++required_count;
        }

        return barrier.runtime_assets_ready(
            requested_manifest.digest,
            std::span<const core::u32>{
                required_node_indices.data(), required_count});
    } catch (...) {
        return false;
    }
}

bool ClusterLifecycleController::idle() const noexcept {
    const std::lock_guard lock{state_->mutex};
    return state_->queue.empty() && state_->asset_readiness_queue.empty() &&
           state_->active == 0U && state_->active_asset_readiness == 0U;
}

} // namespace mediaserver::app
