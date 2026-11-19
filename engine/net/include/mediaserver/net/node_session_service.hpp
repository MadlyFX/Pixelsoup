#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>
#include <variant>
#include <vector>

#include <asio/ip/tcp.hpp>

#include <mediaserver/cmd/node_command_bus.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/net/resume_session.hpp>

namespace mediaserver::net {

enum class NodeSessionConnectErrorCode : std::uint8_t {
    Cancelled,
    TimedOut,
    OpenFailed,
    ConnectFailed,
};

struct NodeSessionConnectError final {
    NodeSessionConnectErrorCode code{};
    std::error_code system_error{};
};

using NodeSessionConnectResult = core::Result<
    asio::ip::tcp::socket,
    NodeSessionConnectError>;

// Cancellable connection seam. Production uses the built-in asynchronous
// dialer; tests may inject a deterministic pending attempt. A borrowed dialer
// must outlive the service and its joined supervisor thread.
class NodeSessionDialer {
public:
    virtual ~NodeSessionDialer() = default;

    [[nodiscard]] virtual NodeSessionConnectResult connect(
        const asio::ip::tcp::endpoint& endpoint,
        std::chrono::milliseconds timeout,
        const std::atomic_bool& stop_requested) = 0;
    virtual void cancel() noexcept = 0;
};

struct NodeSessionServiceConfig final {
    asio::ip::tcp::endpoint director_endpoint{};
    NodeSessionConfig session_defaults;

    // One attempt exists at a time, but the complete framed-message
    // allocation is still admitted through this fixed aggregate budget.
    std::size_t max_receive_bytes{72U * 1024U * 1024U};
    std::chrono::milliseconds connect_timeout{10'000};
    std::chrono::milliseconds reconnect_initial_delay{50};
    std::chrono::milliseconds reconnect_max_delay{5'000};
    // A killed child can redial before the Director releases its old lease.
    // Retry that overlap, but still diagnose a persistent duplicate node.
    std::chrono::milliseconds lease_rejection_timeout{15'000};
};

enum class NodeSessionServiceState : std::uint8_t {
    Stopped,
    Connecting,
    Resuming,
    CheckpointStaged,
    Live,
    Draining,
    Backoff,
    Faulted,
};

enum class NodeSessionServiceFailure : std::uint8_t {
    None,
    Connect,
    Transport,
    PeerCapacity,
    PeerShutdown,
    Protocol,
    LeaseRejected,
    UnsupportedLiveControl,
    Internal,
};

enum class NodeSessionServiceErrorCode : std::uint8_t {
    InvalidConfiguration,
    AlreadyRunning,
    AllocationFailed,
    ThreadStartFailed,
};

struct NodeSessionServiceError final {
    NodeSessionServiceErrorCode code{};
};

enum class NodeSessionBoundaryErrorCode : std::uint8_t {
    ServiceStopped,
    NoActiveSession,
    WrongState,
    SessionRejected,
    SessionFaulted,
};

struct NodeSessionBoundaryError final {
    NodeSessionBoundaryErrorCode code{};
    std::optional<SessionProtocolErrorCode> protocol_error;
};

struct NodeSessionServiceCounters final {
    std::uint64_t connection_attempts{};
    std::uint64_t connect_failures{};
    std::uint64_t successful_resumes{};
    std::uint64_t reconnect_waits{};
    std::uint64_t received_frames{};
    std::uint64_t received_commit_permits{};
    std::uint64_t received_heartbeats{};
    std::uint64_t received_tracked_poses{};
    std::uint64_t received_priority_transports{};
    std::uint64_t received_pointer_overlays{};
    std::uint64_t received_goodbyes{};
    std::uint64_t flushed_acknowledgements{};
    std::uint64_t graceful_goodbyes{};
    std::uint64_t committed_frames{};
    std::uint64_t checkpoint_activations{};
    std::uint64_t terminal_failures{};
    std::uint64_t unsupported_live_controls{};
};

struct NodeSessionServiceSnapshot final {
    NodeSessionServiceState state{NodeSessionServiceState::Stopped};
    NodeSessionServiceFailure last_failure{NodeSessionServiceFailure::None};
    Id128 director_epoch{};
    std::size_t inflight_frames{};
    std::size_t permitted_frames{};
    std::size_t pending_acknowledgements{};
};

namespace detail {
class NodeSessionServiceStateData;
} // namespace detail

// One supervised outbound node connection. The supervisor performs handshake
// and is the sole live reader. A per-attempt writer is the sole post-handshake
// sender. The borrowed NodeCommandBus and optional dialer must outlive stop().
class NodeSessionService final {
public:
    explicit NodeSessionService(
        NodeSessionServiceConfig config,
        cmd::NodeCommandBus& node,
        NodeSessionDialer* dialer = nullptr);
    NodeSessionService(const NodeSessionService&) = delete;
    NodeSessionService& operator=(const NodeSessionService&) = delete;
    NodeSessionService(NodeSessionService&&) = delete;
    NodeSessionService& operator=(NodeSessionService&&) = delete;
    ~NodeSessionService();

    [[nodiscard]] core::Result<std::monostate, NodeSessionServiceError> start();
    void stop() noexcept;

    // Configuration-only validation for startup dry runs. This performs no
    // allocation, DNS lookup, socket open, or connection attempt.
    [[nodiscard]] static bool valid_configuration(
        const NodeSessionServiceConfig& config) noexcept;

    // Boundary-only operations: neither method encodes nor writes. Successful
    // completion wakes the dedicated writer to send the queued control record.
    [[nodiscard]] core::Result<std::monostate, NodeSessionBoundaryError>
    activate_checkpoint() noexcept;
    [[nodiscard]] core::Result<cmd::CommitResult, NodeSessionBoundaryError>
    commit_prepared_frame(cmd::FrameNumber frame) noexcept;

    // Non-mutating boundary query. Absent means there is no session that could
    // publish: stopped, disconnected, or not yet live. This never latches a
    // fault, so a show clock may poll it at every frame boundary.
    [[nodiscard]] std::optional<NodeBoundaryState> boundary_state()
        const noexcept;

    // The last tracked camera pose this node received, or nothing when no
    // session is live or none has arrived. Non-blocking and non-latching on
    // the same contract as boundary_state(), so a render loop may read it at
    // every frame boundary.
    //
    // Deciding whether that pose belongs to the frame being drawn is not this
    // seam's job: hand it to a `track::PoseChannel`, which holds rather than
    // extrapolates and is tested on exactly that.
    //
    // Taken, not read: the pose channels downstream are what retain a pose,
    // so emptying the slot here is what lets an overwrite be told apart from
    // an ordinary re-read and counted.
    [[nodiscard]] std::optional<TrackedCameraFrame>
    take_tracked_camera_pose() noexcept;

    // The newest operator transport anchor the live session received ahead
    // of the frame stream, or nothing. Session-scoped: a new connection
    // starts without one, so an anchor from an earlier Director never
    // overrides the document of a later one. Non-blocking, like
    // boundary_state(), so a render loop may read it on every boundary.
    [[nodiscard]] std::optional<show::TransportAnchor>
    priority_transport() const noexcept;

    // The laser pointer pictures the live session received since the last
    // call, oldest first. Taken, so a render loop hands each to its
    // `show::PointerOverlayChannel` once. Empty with no live session.
    [[nodiscard]] std::vector<show::PointerOverlay>
    take_pointer_overlays() noexcept;

    // Poses this node received and overwrote before draining. See the note on
    // NodeResumeSession: this is the node's own backlog, not a lost pose.
    [[nodiscard]] core::u64 tracked_poses_coalesced() const noexcept;

    // Publishes renderer adoption without performing network I/O. The latest
    // pair survives reconnects; each newly registered Director session still
    // remains fail-closed until it validates a heartbeat carrying the pair.
    void publish_runtime_asset_adoption(
        RuntimeAssetAdoption adoption) noexcept;

    // Publishes the render loop physical presentation state on the same
    // contract. It survives reconnects so a restarted session reports the
    // current barrier state on its first validated heartbeat rather than a
    // stale Unknown.
    void publish_runtime_output_sync(RuntimeOutputSync sync) noexcept;
    // What this node is doing with the tracking it was sent. Reported on the
    // next heartbeat, so the Director can tell a node holding a stale pose
    // from one that never received it - which from the wall look identical.
    void publish_runtime_tracking(RuntimeTracking tracking) noexcept;
    // The frame this node's output last presented complete. Carried on every
    // frame commit, so the Director's Timeline follows it while it plays.
    void publish_runtime_playhead(RuntimePlayhead playhead) noexcept;

    // Publishes the already-observed Program renderer profile and selected
    // backend. This is an in-memory snapshot; network I/O remains on the
    // session writer and happens only in response to the normal heartbeat.
    void publish_runtime_renderer_readiness(
        RuntimeRendererReadiness readiness) noexcept;

    [[nodiscard]] core::u64 conformance_request() const noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] NodeSessionServiceSnapshot snapshot() const noexcept;
    [[nodiscard]] NodeSessionServiceCounters counters() const noexcept;
    [[nodiscard]] std::size_t receive_capacity_bytes() const noexcept;
    [[nodiscard]] std::size_t reserved_receive_bytes() const noexcept;

private:
    std::unique_ptr<detail::NodeSessionServiceStateData> state_;
};

} // namespace mediaserver::net
