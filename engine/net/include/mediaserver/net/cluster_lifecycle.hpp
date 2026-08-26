#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>

#include <asio/ip/tcp.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A deliberately small, separate control plane for render-process lifecycle.
// The replicated node-session channel starts after a render process exists and
// therefore cannot launch an offline machine. This listener is intended to run
// as a machine-local service and owns one fixed child configuration. A remote
// request carries only {node_index, status/start/restart/stop}; paths and
// arguments never cross the wire.
namespace mediaserver::net {

enum class ClusterLifecycleCommand : std::uint8_t {
    Status,
    Start,
    Restart,
    Stop,
};

enum class ClusterProcessState : std::uint8_t {
    Stopped,
    Starting,
    Running,
    Stopping,
    Faulted,
};

enum class ClusterLifecycleOutcome : std::uint8_t {
    Applied,
    NoChange,
    Failed,
    ConfigMismatch,
    Busy,
};

struct ClusterLifecycleReply final {
    ClusterLifecycleOutcome outcome{ClusterLifecycleOutcome::Failed};
    ClusterProcessState state{ClusterProcessState::Faulted};
    core::u32 node_index{};
    core::u64 generation{};
    // Bounded to 160 bytes by the codec. This is an operator diagnostic, never
    // executable text returned to a shell.
    std::string message;
};

// A separately authenticated, bounded inventory snapshot served by the same
// machine-local agent as lifecycle control. It remains useful while the render
// child is stopped, which is why it does not travel over node-session.
inline constexpr std::size_t max_cluster_machine_info_message_bytes =
    16U * 1024U;
inline constexpr std::size_t max_cluster_machine_gpu_adapters = 8U;
inline constexpr std::size_t max_cluster_machine_display_targets = 32U;
inline constexpr std::size_t max_cluster_machine_host_name_bytes = 64U;
inline constexpr std::size_t max_cluster_machine_operating_system_bytes = 96U;
inline constexpr std::size_t max_cluster_machine_message_bytes = 160U;
inline constexpr std::size_t max_cluster_machine_adapter_name_bytes = 128U;
inline constexpr std::size_t max_cluster_machine_display_name_bytes = 96U;

enum class ClusterMachineInfoAvailability : std::uint8_t {
    Available,
    Partial,
    Unavailable,
    Failed,
    ConfigurationMismatch,
};

enum class ClusterDisplayEnumerationState : std::uint8_t {
    Available,
    Unsupported,
    AccessDenied,
    Failed,
};

enum class ClusterVideoOutputTechnology : std::uint8_t {
    Unknown,
    Hd15,
    SVideo,
    Composite,
    Component,
    Dvi,
    Hdmi,
    Lvds,
    DisplayPort,
    Internal,
    Sdi,
    Miracast,
    Indirect,
    Other,
};

struct ClusterGpuAdapterInfo final {
    // An opaque runtime adapter identity. On Windows this is the adapter LUID;
    // it is suitable for joining one snapshot, not as authored project state.
    core::u64 adapter_id{};
    core::u32 vendor_id{};
    core::u32 device_id{};
    core::u32 subsystem_id{};
    core::u32 revision{};
    core::u64 dedicated_video_memory_bytes{};
    core::u64 shared_system_memory_bytes{};
    bool software{};
    bool remote{};
    std::string name;

    bool operator==(const ClusterGpuAdapterInfo&) const = default;
};

struct ClusterDisplayTargetInfo final {
    // adapter_id + target_id identifies a display target only within this
    // runtime inventory. MST, indirect, and virtual targets mean the count is
    // not necessarily the number of physical sockets on a chassis.
    core::u64 adapter_id{};
    core::u32 target_id{};
    core::u32 connector_instance{};
    core::u16 edid_manufacturer_id{};
    core::u16 edid_product_code_id{};
    ClusterVideoOutputTechnology technology{
        ClusterVideoOutputTechnology::Unknown};
    bool active{};
    bool available{};
    core::u32 width{};
    core::u32 height{};
    core::u32 refresh_numerator{};
    core::u32 refresh_denominator{};
    std::string name;

    bool operator==(const ClusterDisplayTargetInfo&) const = default;
};

struct ClusterMachineInfo final {
    ClusterMachineInfoAvailability availability{
        ClusterMachineInfoAvailability::Unavailable};
    ClusterDisplayEnumerationState display_state{
        ClusterDisplayEnumerationState::Unsupported};
    core::u32 node_index{};
    core::u32 logical_cpu_count{};
    core::u64 sample_generation{};
    core::u64 uptime_ms{};
    core::u64 physical_memory_bytes{};
    core::u64 available_memory_bytes{};
    bool truncated{};
    std::string host_name;
    std::string operating_system;
    std::string message;
    std::vector<ClusterGpuAdapterInfo> adapters;
    std::vector<ClusterDisplayTargetInfo> display_targets;

    bool operator==(const ClusterMachineInfo&) const = default;
};

// Providers cache their expensive platform enumeration and publish immutable
// samples. snapshot() is therefore a bounded pointer copy on the listener's
// serial accept thread. The provider must outlive ClusterLifecycleService.
class ClusterMachineInfoProvider {
public:
    virtual ~ClusterMachineInfoProvider() = default;
    [[nodiscard]] virtual std::shared_ptr<const ClusterMachineInfo>
    snapshot() noexcept = 0;
};

class ClusterProcessHost {
public:
    virtual ~ClusterProcessHost() = default;

    // Implementations own a fixed local launch configuration. Calls are
    // serialized by ClusterLifecycleService.
    [[nodiscard]] virtual ClusterLifecycleReply apply(
        ClusterLifecycleCommand command) noexcept = 0;
};

struct ClusterLifecycleServiceConfig final {
    asio::ip::tcp::endpoint bind_endpoint{
        asio::ip::address_v4::loopback(), 47840U};
    core::u32 node_index{};
    // Supplied from a machine-local secret file (32-256 bytes). It is never
    // persisted in a project and never sent over the network.
    std::string shared_secret;
    std::chrono::milliseconds handshake_timeout{1'500};
    std::chrono::milliseconds write_timeout{1'500};
};

enum class ClusterLifecycleServiceErrorCode : std::uint8_t {
    InvalidConfiguration,
    AlreadyRunning,
    OpenFailed,
    BindFailed,
    ListenFailed,
    AllocationFailed,
    ThreadStartFailed,
};

struct ClusterLifecycleServiceError final {
    ClusterLifecycleServiceErrorCode code{};
    std::error_code system_error{};
};

struct ClusterLifecycleServiceCounters final {
    core::u64 accepted_connections{};
    core::u64 authenticated_requests{};
    core::u64 authentication_failures{};
    core::u64 malformed_requests{};
    core::u64 config_mismatches{};
    core::u64 completed_requests{};
};

namespace detail {
class ClusterLifecycleServiceState;

// Narrow known-answer hook for the protocol test. It validates the private
// HMAC-SHA256 primitive without exposing it as a general crypto API.
[[nodiscard]] bool cluster_lifecycle_crypto_known_answer() noexcept;
} // namespace detail

class ClusterLifecycleService final {
public:
    ClusterLifecycleService(ClusterLifecycleServiceConfig config,
                            ClusterProcessHost& host,
                            ClusterMachineInfoProvider* machine_info = nullptr);
    ClusterLifecycleService(const ClusterLifecycleService&) = delete;
    ClusterLifecycleService& operator=(const ClusterLifecycleService&) = delete;
    ClusterLifecycleService(ClusterLifecycleService&&) = delete;
    ClusterLifecycleService& operator=(ClusterLifecycleService&&) = delete;
    ~ClusterLifecycleService();

    [[nodiscard]] core::Result<asio::ip::tcp::endpoint,
                               ClusterLifecycleServiceError>
    start();
    void stop() noexcept;

    [[nodiscard]] static bool valid_configuration(
        const ClusterLifecycleServiceConfig& config) noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::optional<asio::ip::tcp::endpoint> local_endpoint()
        const noexcept;
    [[nodiscard]] ClusterLifecycleServiceCounters counters() const noexcept;

private:
    std::unique_ptr<detail::ClusterLifecycleServiceState> state_;
};

struct ClusterLifecycleClientConfig final {
    std::string shared_secret;
    std::chrono::milliseconds connect_timeout{750};
    // Includes the listener's bounded local process transition (up to three
    // seconds in the bundled agent), not just network handshake time.
    std::chrono::milliseconds handshake_timeout{5'000};
    std::chrono::milliseconds write_timeout{1'500};
};

enum class ClusterLifecycleClientErrorCode : std::uint8_t {
    InvalidConfiguration,
    OpenFailed,
    ConnectFailed,
    TimedOut,
    Transport,
    Protocol,
    AuthenticationFailed,
    AllocationFailed,
};

struct ClusterLifecycleClientError final {
    ClusterLifecycleClientErrorCode code{};
    std::error_code system_error{};
};

[[nodiscard]] bool valid_lifecycle_target(
    const asio::ip::tcp::endpoint& endpoint) noexcept;

// Numeric endpoints only. IPv6 uses [address]:port. DNS belongs in a separate
// discovery/configuration layer rather than on the render frame.
[[nodiscard]] std::optional<asio::ip::tcp::endpoint>
parse_lifecycle_endpoint(std::string_view text) noexcept;

[[nodiscard]] core::Result<ClusterLifecycleReply,
                           ClusterLifecycleClientError>
request_cluster_lifecycle(const asio::ip::tcp::endpoint& endpoint,
                          core::u32 node_index,
                          ClusterLifecycleCommand command,
                           const ClusterLifecycleClientConfig& config);

// Performs the inventory operation over its own authenticated message kind.
// A valid response with ConfigurationMismatch carries only the listener's node
// index and a generic diagnostic; no machine fields are disclosed.
[[nodiscard]] core::Result<ClusterMachineInfo,
                           ClusterLifecycleClientError>
request_cluster_machine_info(const asio::ip::tcp::endpoint& endpoint,
                             core::u32 node_index,
                             const ClusterLifecycleClientConfig& config);

} // namespace mediaserver::net
