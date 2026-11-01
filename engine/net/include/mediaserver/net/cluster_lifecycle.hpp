#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>

#include <asio/ip/tcp.hpp>

#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
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
// The physical displays one NVIDIA Mosaic surface may report.
inline constexpr std::size_t max_cluster_display_mosaic_tiles = 16U;

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

// One physical display of an NVIDIA Mosaic surface, in the surface's pixels.
struct ClusterMosaicTile final {
    core::u32 x{};
    core::u32 y{};
    core::u32 width{};
    core::u32 height{};

    bool operator==(const ClusterMosaicTile&) const = default;
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
    // Local desktop placement and extent of the active source path. These
    // fields are consumed by the render child when it resolves project
    // bindings on the same machine; the authenticated inventory wire
    // intentionally omits them because remote operators bind by stable target
    // identity. The extent is the source mode's size, which is what the
    // desktop actually occupies - it is not the target signal size above when
    // the driver scales, and it is what a window toolkit reports as this
    // display's bounds.
    core::i32 desktop_x{};
    core::i32 desktop_y{};
    core::u32 desktop_width{};
    core::u32 desktop_height{};
    std::string name;
    // The displays an NVIDIA Mosaic surface spans, row by row; empty for an
    // ordinary display. Windows sees the surface as this one target.
    std::vector<ClusterMosaicTile> mosaic_tiles;

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

// Content readiness travels over the already-authenticated machine lifecycle
// listener so it remains available while the render child is stopped.
//
// The request carries the authored absolute paths themselves, because that is
// what the show is built on: every machine opens the same path. Fingerprinted
// requirements additionally bind that path to the exact bytes ingested by the
// Director. The agent never returns a directory listing, a resolved path, or
// file contents.
inline constexpr std::size_t max_cluster_asset_readiness_entries = 256U;
inline constexpr std::size_t max_cluster_asset_source_bytes = 1'024U;
inline constexpr std::size_t max_cluster_asset_readiness_message_bytes =
    320U * 1024U;
// The size and entry limits are protocol-safety caps. A presence check is one
// stat per entry, so the default deadline covers a full batch comfortably even
// on a cold network share.
inline constexpr std::chrono::milliseconds
    max_cluster_asset_readiness_timeout{24LL * 60LL * 60LL * 1'000LL};

enum class ClusterAssetReadinessState : core::u8 {
    Ready,
    Missing,
    ByteLengthMismatch,
    Sha256Mismatch,
    VerificationFailed,
};

struct ClusterAssetRequirement final {
    std::string source;
    bool fingerprinted{};
    core::u64 expected_byte_length{};
    std::array<core::u8, 32U> expected_sha256{};

    bool operator==(const ClusterAssetRequirement&) const = default;
};

enum class ClusterAssetReadinessAvailability : core::u8 {
    Available,
    Unavailable,
    Failed,
    ConfigurationMismatch,
};

struct ClusterAssetReadinessEntry final {
    std::string source;
    ClusterAssetReadinessState state{ClusterAssetReadinessState::Missing};

    bool operator==(const ClusterAssetReadinessEntry&) const = default;
};

struct ClusterAssetReadinessReply final {
    ClusterAssetReadinessAvailability availability{
        ClusterAssetReadinessAvailability::Unavailable};
    core::u32 node_index{};
    core::u32 ready{};
    core::u32 missing{};
    core::u32 mismatched{};
    core::u32 verification_failed{};
    std::vector<ClusterAssetReadinessEntry> entries;

    bool operator==(const ClusterAssetReadinessReply&) const = default;
};

// nullopt means the provider itself failed; all enum values are ordinary,
// reportable observations about one valid requirement.
class ClusterAssetReadinessProvider {
public:
    virtual ~ClusterAssetReadinessProvider() = default;
    // Calls are serialized by ClusterLifecycleService. A second concurrent
    // readiness request fails fast while a probe is active, preserving worker
    // capacity for Status/Start/Restart/Stop. Implementations may do
    // substantial local I/O. Service stop requests the token and waits for an
    // active probe to return; implementations must propagate it through any
    // long-running filesystem/hash operation.
    [[nodiscard]] virtual std::optional<ClusterAssetReadinessState>
    probe(const ClusterAssetRequirement& requirement,
          std::stop_token stop = {}) noexcept = 0;
};

// Physical output-timing state, served by the same machine-local agent as
// lifecycle control and inventory.
//
// This half of the sync picture is deliberately the machine half: whether a
// sync device exists, whether a house reference is present, whether the device
// is actually disciplined to it, and the lock state and exact timing of each
// display target. The agent can answer all of that while the render child is
// stopped, which is precisely when an operator needs it.
//
// The other half - whether each swapchain joined the expected present barrier,
// and how many frames were missed, ran late, were held or repeated - is known
// only inside the render process and travels on the already-authenticated
// node-session heartbeat (RuntimeOutputSync in session_protocol.hpp). Neither
// half alone proves that separate displays scanned the same image.
inline constexpr std::size_t max_cluster_sync_status_message_bytes =
    8U * 1024U;
inline constexpr std::size_t max_cluster_sync_outputs = 32U;
inline constexpr std::size_t max_cluster_sync_detail_bytes = 160U;

enum class ClusterSyncStatusAvailability : core::u8 {
    Available,
    // The agent has no sync provider configured. Never evidence of lock.
    Unavailable,
    Failed,
    ConfigurationMismatch,
};

enum class ClusterSyncDeviceAvailability : core::u8 {
    Unknown,
    // This platform/build carries no vendor sync integration.
    Unsupported,
    Absent,
    Present,
    Failed,
};

enum class ClusterSyncReferenceState : core::u8 {
    Unknown,
    Absent,
    // A reference is present but the device is not disciplined to it.
    Present,
    // Present and in use as the timing source.
    InUse,
    Unstable,
    Unsupported,
};

enum class ClusterSyncLockState : core::u8 {
    Unknown,
    Unlocked,
    Locked,
    Unsupported,
};

struct ClusterSyncOutputStatus final {
    // Joins to the same runtime display identity ClusterDisplayTargetInfo
    // reports, so an authored Hardware Map binding can be matched exactly.
    core::u64 adapter_id{};
    core::u32 target_id{};
    core::u32 connector_instance{};
    core::u32 width{};
    core::u32 height{};
    core::u32 refresh_numerator{};
    core::u32 refresh_denominator{};
    ClusterSyncLockState lock{ClusterSyncLockState::Unknown};
    // False means the provider did not report timing at all, which is not the
    // same as a zero-rate mode.
    bool timing_reported{};

    bool operator==(const ClusterSyncOutputStatus&) const = default;
};

struct ClusterSyncStatus final {
    ClusterSyncStatusAvailability availability{
        ClusterSyncStatusAvailability::Unavailable};
    ClusterSyncDeviceAvailability board{
        ClusterSyncDeviceAvailability::Unknown};
    ClusterSyncReferenceState reference{ClusterSyncReferenceState::Unknown};
    core::u32 node_index{};
    core::u64 sample_generation{};
    // Cumulative transitions out of a locked reference observed by the agent.
    core::u64 loss_of_sync_events{};
    // True when this sample came from a deliberately simulated provider. It
    // travels all the way to the operator surface and can never satisfy a
    // strict synchronization policy.
    bool simulated{};
    bool truncated{};
    std::string detail;
    std::vector<ClusterSyncOutputStatus> outputs;

    bool operator==(const ClusterSyncStatus&) const = default;
};

// Providers cache their platform queries and publish immutable samples, so
// snapshot() is a bounded pointer copy. Calls are serialized by
// ClusterLifecycleService and the provider must outlive it.
class ClusterSyncStatusProvider {
public:
    virtual ~ClusterSyncStatusProvider() = default;
    [[nodiscard]] virtual std::shared_ptr<const ClusterSyncStatus>
    snapshot() noexcept = 0;
};

// Providers cache their expensive platform enumeration and publish immutable
// samples. snapshot() is therefore a bounded pointer copy. Calls are
// serialized by ClusterLifecycleService, and the provider must outlive it.
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
    // The listener admits connections into a bounded queue serviced by a
    // fixed pool. At least two workers keep ordinary lifecycle commands
    // responsive while one worker performs a potentially expensive content
    // verification pass.
    std::size_t connection_worker_count{4U};
    std::size_t max_pending_connections{32U};
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
    core::u64 cancelled_requests{};
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
                            ClusterMachineInfoProvider* machine_info = nullptr,
                            ClusterAssetReadinessProvider* asset_readiness =
                                nullptr,
                            ClusterSyncStatusProvider* sync_status = nullptr);
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

// The key both ends of the lifecycle channel authenticate with, derived from
// an operator's password: PBKDF2-HMAC-SHA256 over a fixed, protocol-specific
// salt with lifecycle_password_iterations rounds, as 64 lowercase hex
// characters (a valid shared_secret for the configs here).
//
// The empty password is a working choice, not an error: it derives a key every
// Media Server knows, so a render machine's agent can be controlled with no
// setup at all - and by anyone on the network who runs Media Server. A
// password is what makes the channel private. Either way the protocol, its
// nonces and its message authentication are unchanged.
inline constexpr std::uint32_t lifecycle_password_iterations = 100'000U;
[[nodiscard]] std::string lifecycle_key_from_password(std::string_view password);
// True for the key the empty password derives: the unsecured configuration.
[[nodiscard]] bool lifecycle_key_is_unsecured(std::string_view key);

struct ClusterLifecycleClientConfig final {
    std::string shared_secret;
    std::chrono::milliseconds connect_timeout{750};
    // Includes the listener's bounded local process transition (up to three
    // seconds in the bundled agent), not just network handshake time.
    std::chrono::milliseconds handshake_timeout{5'000};
    std::chrono::milliseconds write_timeout{1'500};
    // Hashing a large manifest can legitimately take much longer than the
    // challenge/response handshake. This configurable, bounded receive budget
    // is used only after an authenticated asset-readiness request is sent.
    // A timeout is reported as TimedOut; callers keep readiness fail-closed
    // and may retry. Increase it for stores whose verified hashing throughput
    // cannot cover a batch in the default two minutes.
    std::chrono::milliseconds asset_readiness_timeout{120'000};
};

enum class ClusterLifecycleClientErrorCode : std::uint8_t {
    InvalidConfiguration,
    OpenFailed,
    ConnectFailed,
    TimedOut,
    Transport,
    Protocol,
    AuthenticationFailed,
    Cancelled,
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

// Performs the physical output-timing query over its own authenticated
// message kind. A valid response with ConfigurationMismatch carries only the
// listener node index and a generic diagnostic; no timing is disclosed. A
// transport or protocol failure keeps readiness fail-closed at the caller: an
// unanswered sync query is never treated as a lock.
[[nodiscard]] core::Result<ClusterSyncStatus,
                           ClusterLifecycleClientError>
request_cluster_sync_status(const asio::ip::tcp::endpoint& endpoint,
                            core::u32 node_index,
                            const ClusterLifecycleClientConfig& config);

// Empty requests are valid (the selected state requires no assets). Requests
// above max_cluster_asset_readiness_entries, duplicate sources, malformed
// fingerprint records, empty sources, or sources longer than
// max_cluster_asset_source_bytes fail locally before connecting. The deadline
// is per wire request, not per source.
[[nodiscard]] core::Result<ClusterAssetReadinessReply,
                           ClusterLifecycleClientError>
request_cluster_asset_readiness(
    const asio::ip::tcp::endpoint& endpoint,
    core::u32 node_index,
    std::span<const ClusterAssetRequirement> requirements,
    const ClusterLifecycleClientConfig& config,
    std::stop_token stop = {});

// Convenience for callers whose assets have not yet been fingerprinted. It
// sends explicit presence-only requirement records on the same wire protocol.
[[nodiscard]] core::Result<ClusterAssetReadinessReply,
                           ClusterLifecycleClientError>
request_cluster_asset_readiness(
    const asio::ip::tcp::endpoint& endpoint,
    core::u32 node_index,
    std::span<const std::string> sources,
    const ClusterLifecycleClientConfig& config,
    std::stop_token stop = {});

} // namespace mediaserver::net
