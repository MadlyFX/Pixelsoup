#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>

#include <asio/ip/tcp.hpp>

#include <mediaserver/core/result.hpp>
#include <mediaserver/net/director_session_hub.hpp>
#include <mediaserver/net/resume_session.hpp>

namespace mediaserver::net {

struct DirectorLiveRegistration final {
    Id128 session_id;
    NodeSessionIdentity peer;
    core::u32 node_index{};
    cmd::StreamCursor committed_cursor;
    core::u64 last_node_control_sequence{1U};
};

enum class DirectorSessionLeaveReason : std::uint8_t {
    ServiceStopped,
    PeerGoodbye,
    HubClosed,
    ResyncRequired,
    SessionFailure,
    ObserverRejected,
};

// Bounded acknowledgement-aggregation hook. Implementations may briefly
// block behind a coordinator's publication cut, but must not throw. A
// successful register_live() is paired with exactly one unregister(). The
// service invokes these callbacks without holding listener, socket, or worker
// resource locks.
class DirectorSessionObserver {
public:
    virtual ~DirectorSessionObserver() = default;

    [[nodiscard]] virtual bool register_live(
        const DirectorLiveRegistration& registration) noexcept = 0;
    [[nodiscard]] virtual bool on_ack(
        Id128 session_id,
        const FrameAcknowledgementReport& acknowledgement) noexcept = 0;
    // The session has already validated identity, epoch, exact outstanding
    // heartbeat, node-control sequence, and visible cursor. Coordinators use
    // this hook only to advance that same per-session control sequence; it
    // must not advance frame or release progress.
    [[nodiscard]] virtual bool on_heartbeat_ack(
        Id128 session_id,
        const HeartbeatAcknowledgementReport& acknowledgement) noexcept {
        static_cast<void>(session_id);
        static_cast<void>(acknowledgement);
        return true;
    }
    virtual void unregister(
        Id128 session_id,
        DirectorSessionLeaveReason reason) noexcept = 0;

    // A running service requires an observer that coordinates durable commit
    // permits. The defaults keep unrelated observer implementations source
    // compatible while making a missing coordinator fail closed at start().
    [[nodiscard]] virtual bool coordinates_commit_permits() const noexcept {
        return false;
    }

    enum class CommitPermitPoll : std::uint8_t {
        Unsupported,
        Empty,
        Ready,
        Rejected,
    };

    [[nodiscard]] virtual CommitPermitPoll try_take_commit_permit(
        Id128 session_id,
        FrameCommitPermitIntent& output) noexcept {
        static_cast<void>(session_id);
        static_cast<void>(output);
        return CommitPermitPoll::Unsupported;
    }

    // Returns the global contiguous durable release frontier used only as a
    // heartbeat diagnostic. It may be ahead of an individual session's sent
    // cursor and never authorizes a node commit.
    [[nodiscard]] virtual bool heartbeat_released_through(
        cmd::StreamCursor& output) const noexcept {
        static_cast<void>(output);
        return false;
    }
};

struct DirectorSessionServiceConfig final {
    asio::ip::tcp::endpoint bind_endpoint{
        asio::ip::address_v4::any(),
        0U,
    };
    std::size_t max_workers{1'024U};

    // Every admitted worker receives {session_id_namespace, sequence}. The
    // sequence starts here and never resets across stop/start on one service.
    // Zero values are rejected, and UINT64_MAX is issued once before explicit
    // exhaustion prevents any later worker admission.
    core::u64 session_id_namespace{};
    core::u64 first_session_sequence{1U};

    // One aggregate budget is constructed by the service and installed into
    // every worker config. session_defaults.io.admission_budget must be empty,
    // and this capacity must admit at least one codec-sized message.
    std::size_t max_receive_bytes{256U * 1024U * 1024U};

    // session_id is a worker-specific value and therefore must be zero here.
    DirectorSessionConfig session_defaults;

    std::chrono::milliseconds heartbeat_interval{5'000};
    std::chrono::milliseconds heartbeat_response_timeout{10'000};
};

enum class DirectorSessionServiceErrorCode : std::uint8_t {
    InvalidConfiguration,
    AlreadyRunning,
    OpenFailed,
    BindFailed,
    ListenFailed,
    AllocationFailed,
    ThreadStartFailed,
};

struct DirectorSessionServiceError final {
    DirectorSessionServiceErrorCode code{};
    std::error_code system_error{};
};

struct DirectorSessionServiceCounters final {
    std::uint64_t accepted_connections{};
    std::uint64_t capacity_rejections{};
    std::uint64_t session_id_exhaustions{};
    std::uint64_t worker_start_failures{};
    std::uint64_t subscription_failures{};
    std::uint64_t resume_failures{};
    std::uint64_t live_registrations{};
    std::uint64_t observer_rejections{};
    std::uint64_t validated_acknowledgements{};
    std::uint64_t validated_heartbeat_acknowledgements{};
    std::uint64_t sent_heartbeats{};
    std::uint64_t heartbeat_timeouts{};
    std::uint64_t graceful_goodbyes{};
    std::uint64_t resync_disconnects{};
    std::uint64_t hub_closed_disconnects{};
    std::uint64_t session_failures{};
};

namespace detail {
class DirectorSessionServiceState;
} // namespace detail

// Bounded blocking-TCP Director listener. The accept thread owns a fixed set
// of worker slots. Each worker consumes exactly one atomic hub subscription,
// completes historical replay, registers acknowledgement membership, and then
// runs one publisher plus one acknowledgement reader. Socket backpressure is
// isolated to that worker and never reaches DirectorSessionHub::seal_and_publish().
// The borrowed hub, durable authority, lease authority, and optional observer
// must all outlive the service and every worker joined by its destructor.
// stop() must therefore precede hub shutdown or destruction.
class DirectorSessionService final {
public:
    DirectorSessionService(
        DirectorSessionServiceConfig config,
        DirectorSessionHub& hub,
        const persist::DurableDirector& authority,
        NodeSessionLeaseAuthority& leases,
        DirectorSessionObserver* observer = nullptr);
    DirectorSessionService(const DirectorSessionService&) = delete;
    DirectorSessionService& operator=(const DirectorSessionService&) = delete;
    DirectorSessionService(DirectorSessionService&&) = delete;
    DirectorSessionService& operator=(DirectorSessionService&&) = delete;
    ~DirectorSessionService();

    [[nodiscard]] core::Result<
        asio::ip::tcp::endpoint,
        DirectorSessionServiceError>
    start();
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::optional<asio::ip::tcp::endpoint> local_endpoint()
        const noexcept;
    [[nodiscard]] std::size_t active_workers() const noexcept;
    [[nodiscard]] DirectorSessionServiceCounters counters() const noexcept;
    [[nodiscard]] std::size_t receive_capacity_bytes() const noexcept;
    [[nodiscard]] std::size_t reserved_receive_bytes() const noexcept;

private:
    std::unique_ptr<detail::DirectorSessionServiceState> state_;
};

} // namespace mediaserver::net
