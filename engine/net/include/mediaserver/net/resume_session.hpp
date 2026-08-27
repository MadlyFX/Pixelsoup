#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <variant>
#include <vector>

#include <asio/ip/tcp.hpp>

#include <mediaserver/cmd/node_command_bus.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/net/session_protocol.hpp>
#include <mediaserver/net/tcp_framed_stream.hpp>
#include <mediaserver/persist/command_log.hpp>
#include <mediaserver/persist/durable_director.hpp>
#include <mediaserver/wire/protocol_codec.hpp>

namespace mediaserver::net {

inline constexpr SessionLimits default_session_limits{
    16U * 1024U * 1024U,
    64U * 1024U * 1024U,
    8U,
    128U * 1024U * 1024U,
    64U * 1024U * 1024U,
};

struct SessionEncodeFailure final {
    SessionEncodeError error;
};

struct SessionDecodeFailure final {
    SessionDecodeError error;
};

struct WireEncodeFailure final {
    wire::EncodeError error;
    std::size_t packet_index{};
};

struct WireDecodeFailure final {
    wire::DecodeError error;
    std::size_t packet_index{};
};

struct ResumeSourceFailure final {
    persist::DurableDirectorError error;
};

struct NodeBatchFailure final {
    cmd::BatchReceiveError error;
};

enum class SessionProtocolErrorCode : std::uint8_t {
    InvalidConfiguration,
    WrongState,
    UnexpectedMessage,
    UnsupportedVersion,
    ShowMismatch,
    EpochMismatch,
    SessionMismatch,
    SequenceMismatch,
    CursorMismatch,
    CursorAhead,
    CheckpointMismatch,
    BatchMismatch,
    NodeRejected,
    CheckpointPrepareRejected,
    CheckpointActivationRejected,
    NonDurableBatch,
    UnauthorizedResumeSource,
    UnauthorizedBatch,
    NodeLeaseRejected,
    AcknowledgementMismatch,
    FrameNotPrepared,
    FrameNotPermitted,
    FrameNotCommitted,
    InvalidCommitPermit,
    InflightWindowFull,
    InflightLimitExceeded,
    AllocationFailed,
    PeerRejected,
    ReplayInterrupted,
};

struct SessionProtocolFailure final {
    SessionProtocolErrorCode code;
    std::optional<RejectCode> reject_code;
    std::optional<cmd::ReceiveStatus> receive_status;
    std::optional<cmd::CheckpointPrepareError> checkpoint_error;
};

using ResumeSessionError = std::variant<
    FramedStreamError,
    SessionEncodeFailure,
    SessionDecodeFailure,
    WireEncodeFailure,
    WireDecodeFailure,
    persist::CommandLogError,
    ResumeSourceFailure,
    NodeBatchFailure,
    SessionProtocolFailure>;

enum class DirectorSessionState : std::uint8_t {
    AwaitHello,
    AwaitResumeAck,
    Live,
    Draining,
    Closed,
};

enum class NodeSessionState : std::uint8_t {
    Disconnected,
    AwaitWelcome,
    Resuming,
    CheckpointStaged,
    CheckpointActivated,
    Live,
    Draining,
    Closed,
};

struct SessionIoPolicy final {
    std::chrono::milliseconds handshake_timeout{10'000};
    std::chrono::milliseconds read_timeout{30'000};
    std::chrono::milliseconds write_timeout{10'000};
    std::shared_ptr<FramedStreamAdmissionBudget> admission_budget;
};

struct DirectorSessionConfig final {
    Id128 show_id;
    Id128 director_epoch;
    Id128 session_id;
    SessionLimits limits{default_session_limits};
    SessionDecodeLimits codec_limits{};
    wire::DecodeLimits wire_limits{};
    SessionIoPolicy io{};
};

struct NodeSessionConfig final {
    Id128 show_id;
    Id128 node_id;
    Id128 incarnation;
    core::u32 node_index{};
    Id128 last_director_epoch;
    bool force_checkpoint{};
    SessionLimits limits{default_session_limits};
    SessionDecodeLimits codec_limits{};
    wire::DecodeLimits wire_limits{};
    SessionIoPolicy io{};
};

struct ResumeReport final {
    ResumeMode mode{ResumeMode::None};
    cmd::StreamCursor requested{};
    cmd::StreamCursor target{};
    std::size_t frames{};
    std::size_t commands{};
    bool checkpoint_staged{};
};

struct ReceivedFrameReport final {
    cmd::StreamCursor before{};
    cmd::StreamCursor after{};
    std::size_t commands{};
};

// Everything a show-clock boundary needs to decide whether to publish, read
// under the same locks the session publishes with. A driver uses this instead
// of reading the shared NodeCommandBus, whose staging containers the network
// worker mutates concurrently.
struct NodeBoundaryState final {
    cmd::StreamCursor cursor{};
    bool live{};
    bool needs_resync{};
    bool checkpoint_staged{};
    // The earliest uncommitted frame has had its Prepared acknowledgement
    // written and carries a durable Director commit permit.
    bool next_frame_permitted{};
    cmd::FrameNumber next_frame{};

    bool operator==(const NodeBoundaryState&) const = default;
};

enum class FrameAcknowledgementKind : std::uint8_t {
    Prepared,
    Committed,
};

struct FrameAcknowledgementReport final {
    FrameAcknowledgementKind kind{FrameAcknowledgementKind::Prepared};
    core::u64 sequence{};
    core::u64 acknowledged_sequence{};
    cmd::StreamCursor before{};
    cmd::StreamCursor after{};
    std::size_t outstanding_frames{};
    core::u64 outstanding_bytes{};
};

struct FrameAcknowledgementFlushReport final {
    std::size_t flushed{};
    std::size_t remaining{};
    std::size_t heartbeat_acknowledgements{};
    std::size_t goodbyes{};
};

// A durable coordinator's exact authorization intent. Passing the retained
// frame identity into the session lets it reject barrier/session drift before
// any irreversible permit bytes are written.
struct FrameCommitPermitIntent final {
    core::u64 barrier_ordinal{};
    core::u64 acknowledged_sequence{};
    cmd::StreamCursor before{};
    cmd::StreamCursor after{};
};

struct FrameCommitPermitReport final {
    core::u64 sequence{};
    core::u64 acknowledged_sequence{};
    core::u64 barrier_ordinal{};
    cmd::StreamCursor before{};
    cmd::StreamCursor after{};
    std::size_t permitted_frames{};
};

struct HeartbeatSendReport final {
    core::u64 sequence{};
    core::u32 response_timeout_ms{};
    cmd::StreamCursor released_through;
    std::chrono::steady_clock::time_point sent_at;
};

struct HeartbeatReceivedReport final {
    core::u64 sequence{};
    core::u64 acknowledgement_sequence{};
    core::u32 response_timeout_ms{};
    cmd::StreamCursor released_through;
    cmd::StreamCursor visible_through;
    RuntimeAssetAdoption asset_adoption;
    RuntimeOutputSync output_sync;
};

struct HeartbeatAcknowledgementReport final {
    core::u64 sequence{};
    core::u64 acknowledged_sequence{};
    cmd::StreamCursor visible_through;
    RuntimeAssetAdoption asset_adoption;
    RuntimeOutputSync output_sync;
};

struct GoodbyeReport final {
    core::u64 sequence{};
    GoodbyeReason reason{GoodbyeReason::None};
    cmd::StreamCursor final_cursor;
};

using NodeLiveInboundReport = std::variant<
    ReceivedFrameReport,
    FrameCommitPermitReport,
    HeartbeatReceivedReport,
    GoodbyeReport>;

using DirectorLiveInboundReport = std::variant<
    FrameAcknowledgementReport,
    HeartbeatAcknowledgementReport,
    GoodbyeReport>;

struct NodeSessionIdentity final {
    Id128 show_id;
    Id128 node_id;
    Id128 incarnation;

    bool operator==(const NodeSessionIdentity&) const = default;
};

// Implemented by the Director's connection registry. claim() must atomically
// fence duplicate/stale owners for one logical node. The authority outlives all
// sessions that refer to it, and release() must tolerate a normal disconnect.
class NodeSessionLeaseAuthority {
public:
    virtual ~NodeSessionLeaseAuthority() = default;
    [[nodiscard]] virtual bool claim(
        const NodeSessionIdentity& identity,
        Id128 session_id) noexcept = 0;
    virtual void release(
        const NodeSessionIdentity& identity,
        Id128 session_id) noexcept = 0;
};

// Bounded in-process implementation for the Director listener. A logical node
// is keyed by (show_id, node_id): a newer incarnation cannot silently steal an
// active connection, and an old disconnect cannot release a newer lease.
class NodeSessionLeaseRegistry final : public NodeSessionLeaseAuthority {
public:
    explicit NodeSessionLeaseRegistry(std::size_t max_sessions = 1'024U);

    NodeSessionLeaseRegistry(const NodeSessionLeaseRegistry&) = delete;
    NodeSessionLeaseRegistry& operator=(const NodeSessionLeaseRegistry&) = delete;

    [[nodiscard]] bool claim(
        const NodeSessionIdentity& identity,
        Id128 session_id) noexcept override;
    void release(
        const NodeSessionIdentity& identity,
        Id128 session_id) noexcept override;

    [[nodiscard]] std::size_t active_sessions() const noexcept;
    [[nodiscard]] bool owns(
        const NodeSessionIdentity& identity,
        Id128 session_id) const noexcept;

private:
    struct Lease final {
        NodeSessionIdentity identity;
        Id128 session_id;
    };

    std::size_t max_sessions_{};
    mutable std::mutex mutex_;
    std::vector<Lease> leases_;
};

// One blocking Director-side connection. The owner gives each node its own
// worker so socket backpressure cannot stall other nodes. The authoritative
// DurableDirector must outlive the session; it supplies the only accepted
// checkpoint/log pair and authorizes every live batch.
class DirectorResumeSession final {
public:
    DirectorResumeSession(
        asio::ip::tcp::socket socket,
        DirectorSessionConfig config,
        const persist::DurableDirector& authority,
        NodeSessionLeaseAuthority& lease_authority) noexcept;

    DirectorResumeSession(const DirectorResumeSession&) = delete;
    DirectorResumeSession& operator=(const DirectorResumeSession&) = delete;
    DirectorResumeSession(DirectorResumeSession&&) = delete;
    DirectorResumeSession& operator=(DirectorResumeSession&&) = delete;
    ~DirectorResumeSession();

    // Convenience path for a quiescent authority. A production connection
    // with concurrent sealing must use the hub-bound overload below so frames
    // committed after capture enter its registered live backlog.
    [[nodiscard]] core::Result<ResumeReport, ResumeSessionError> resume();

    // Uses the snapshot captured atomically with a DirectorSessionHub live
    // subscription. The source must have been issued by this session's exact
    // DurableDirector authority.
    [[nodiscard]] core::Result<ResumeReport, ResumeSessionError> resume(
        persist::DurableResumeSource source);

    // Live publication accepts only the move-only product of DurableDirector.
    // The complete command/seal batch is encoded before the first byte is sent.
    [[nodiscard]] core::Result<std::monostate, ResumeSessionError> send_committed(
        const persist::CommittedFrameBatch& batch);

    // Authorizes the earliest unpermitted live frame after its validated
    // Prepared acknowledgement. The caller supplies the exact frame identity
    // and nonzero, monotonically increasing ordinal of a decision that was
    // made durable before this call. All fields are checked before encoding.
    // This method shares the session's sole Director-to-node writer with
    // send_committed(). The frame is marked permitted only after the wire send
    // succeeds.
    [[nodiscard]] core::Result<FrameCommitPermitReport, ResumeSessionError>
    send_commit_permit(const FrameCommitPermitIntent& intent);

    // Heartbeat and Goodbye share the exact Director outbound sequence and
    // writer lock with frames and permits. released_through is diagnostic and
    // is deliberately not compared with this session's sent cursor.
    [[nodiscard]] core::Result<HeartbeatSendReport, ResumeSessionError>
    send_heartbeat(
        const cmd::StreamCursor& released_through,
        std::chrono::milliseconds response_timeout);
    [[nodiscard]] core::Result<GoodbyeReport, ResumeSessionError>
    send_goodbye(
        GoodbyeReason reason,
        const cmd::StreamCursor& final_cursor);

    // Reads and validates exactly one node control record. HeartbeatAck and
    // Goodbye consume the same sequence as Prepared and Committed.
    [[nodiscard]] core::Result<DirectorLiveInboundReport, ResumeSessionError>
    receive_live_message();

    // Receives one node-to-Director live-frame acknowledgement. Prepared
    // confirms an invisible staged root; Committed retires the oldest
    // outstanding frame and reopens the publication window.
    [[nodiscard]] core::Result<FrameAcknowledgementReport, ResumeSessionError>
    receive_frame_acknowledgement();

    void close() noexcept;

    [[nodiscard]] DirectorSessionState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }
    [[nodiscard]] cmd::StreamCursor sent_cursor() const noexcept;
    [[nodiscard]] cmd::StreamCursor prepared_cursor() const noexcept;
    [[nodiscard]] cmd::StreamCursor committed_cursor() const noexcept;
    [[nodiscard]] std::optional<NodeSessionIdentity> peer_identity() const noexcept;
    [[nodiscard]] std::optional<core::u32> peer_node_index() const noexcept;
    [[nodiscard]] std::size_t outstanding_frames() const noexcept;
    [[nodiscard]] std::size_t permitted_frames() const noexcept;
    [[nodiscard]] core::u64 last_commit_permit_ordinal() const noexcept;
    [[nodiscard]] core::u64 last_node_control_sequence() const noexcept;

private:
    struct OutstandingFrame final {
        core::u64 sequence{};
        cmd::StreamCursor before{};
        cmd::StreamCursor after{};
        core::u64 encoded_bytes{};
        bool prepared{};
        bool permitted{};
        core::u64 barrier_ordinal{};
    };

    [[nodiscard]] core::Result<ResumeReport, ResumeSessionError> resume_impl(
        std::optional<persist::DurableResumeSource> source);
    void release_lease() noexcept;

    TcpFramedStream stream_;
    DirectorSessionConfig config_;
    const persist::DurableDirector* authority_;
    NodeSessionLeaseAuthority* lease_authority_;
    std::atomic<DirectorSessionState> state_{DirectorSessionState::AwaitHello};
    SessionLimits negotiated_limits_{};
    cmd::StreamCursor sent_cursor_{};
    cmd::StreamCursor prepared_cursor_{};
    cmd::StreamCursor committed_cursor_{};
    core::u64 next_sequence_{1U};
    core::u64 expected_ack_sequence_{2U};
    core::u64 last_commit_permit_ordinal_{};
    std::optional<core::u64> outstanding_heartbeat_sequence_;
    core::u64 outstanding_bytes_{};
    std::vector<OutstandingFrame> outstanding_frames_;
    std::optional<NodeSessionIdentity> peer_identity_;
    std::optional<core::u32> peer_node_index_;
    mutable std::mutex send_mutex_;
    mutable std::mutex acknowledgement_mutex_;
    mutable std::mutex lease_mutex_;
};

// One blocking node-side connection. Network delivery only prepares sealed
// roots; while a session is active, the show-clock owner must publish through
// commit_prepared_frame() rather than touching the retained NodeCommandBus
// directly. A received checkpoint is staged and requires explicit boundary
// activation.
class NodeResumeSession final {
public:
    NodeResumeSession(
        asio::ip::tcp::socket socket,
        cmd::NodeCommandBus& node,
        NodeSessionConfig config) noexcept;

    NodeResumeSession(const NodeResumeSession&) = delete;
    NodeResumeSession& operator=(const NodeResumeSession&) = delete;
    NodeResumeSession(NodeResumeSession&&) = delete;
    NodeResumeSession& operator=(NodeResumeSession&&) = delete;
    ~NodeResumeSession();

    [[nodiscard]] core::Result<ResumeReport, ResumeSessionError> resume();
    [[nodiscard]] core::Result<std::monostate, ResumeSessionError>
    activate_checkpoint();
    // Show-clock boundary half of checkpoint activation. This only publishes
    // the already-staged NodeCommandBus root and changes local session state;
    // it performs no allocation, encoding, or socket I/O. The connection's
    // sole writer must subsequently call complete_checkpoint_activation().
    [[nodiscard]] core::Result<std::monostate, ResumeSessionError>
    activate_checkpoint_boundary();
    [[nodiscard]] core::Result<std::monostate, ResumeSessionError>
    complete_checkpoint_activation();
    // Reads and dispatches exactly one current live Director message. A frame
    // is staged and its Prepared intent is enqueued, but this generic path does
    // not write; the connection's dedicated writer flushes the intent.
    [[nodiscard]] core::Result<NodeLiveInboundReport, ResumeSessionError>
    receive_live_message();
    // Stages one live frame, then the network worker flushes the control FIFO
    // through that frame's Prepared acknowledgement before returning success.
    // Any earlier queued Committed acknowledgements are written first.
    [[nodiscard]] core::Result<ReceivedFrameReport, ResumeSessionError>
    receive_live_frame();
    // Receives and validates one Director authorization for the earliest
    // unpermitted live frame. The connection's network worker remains the sole
    // socket reader and calls this when the next Director message is a permit.
    [[nodiscard]] core::Result<FrameCommitPermitReport, ResumeSessionError>
    receive_commit_permit();
    // Called by the show-clock owner at its frame boundary. This performs only
    // the allocation-free NodeCommandBus publish and a bounded enqueue into a
    // ring allocated during negotiation. It never encodes or writes a packet.
    [[nodiscard]] core::Result<cmd::CommitResult, ResumeSessionError>
    commit_prepared_frame(cmd::FrameNumber frame);

    // Called by the connection's network worker. Encodes and writes queued
    // control acknowledgements in exact sequence order. A frame remains in the
    // local receive window until its FrameCommitted write succeeds.
    [[nodiscard]] core::Result<FrameAcknowledgementFlushReport, ResumeSessionError>
    flush_frame_acknowledgements();

    // Enqueues a graceful close behind all earlier node control records. It
    // allocates no memory and performs no socket I/O.
    [[nodiscard]] core::Result<GoodbyeReport, ResumeSessionError>
    request_goodbye(GoodbyeReason reason) noexcept;

    // Cross-thread cancellation only. This wakes active framed-stream I/O but
    // deliberately does not inspect checkpoint/session-owned state. The sole
    // session worker must call close() after its resume/read operation unwinds.
    void interrupt_transport() noexcept;

    // Releases session-owned state and closes the transport. A session that
    // sent its own Goodbye closes gracefully here instead: it waits, bounded,
    // for the peer's close before releasing the socket, because a Goodbye the
    // peer never read is not a shutdown the peer can distinguish from a
    // dropped connection. Must be called from the thread that owns the receive
    // direction, which is the same sole session worker as before.
    void close() noexcept;

    [[nodiscard]] NodeSessionState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }
    [[nodiscard]] Id128 director_epoch() const noexcept { return director_epoch_; }
    [[nodiscard]] cmd::StreamCursor delivered_cursor() const noexcept {
        return delivered_cursor_;
    }
    [[nodiscard]] std::size_t inflight_frames() const noexcept;
    [[nodiscard]] core::u64 inflight_bytes() const noexcept;
    [[nodiscard]] std::size_t pending_frame_acknowledgements() const noexcept;
    [[nodiscard]] std::size_t pending_control_messages() const noexcept;
    [[nodiscard]] std::size_t permitted_frames() const noexcept;
    [[nodiscard]] core::u64 last_commit_permit_ordinal() const noexcept;

    // Cross-thread renderer publication. Heartbeat receive snapshots this
    // pair into the exact queued acknowledgement intent; a later state change
    // cannot rewrite an already acknowledged heartbeat.
    void publish_runtime_asset_adoption(
        RuntimeAssetAdoption adoption) noexcept;

    // Physical presentation state, published by the render loop on the same
    // cross-thread contract as asset adoption: heartbeat receive snapshots
    // whatever was last published into the exact queued acknowledgement.
    void publish_runtime_output_sync(RuntimeOutputSync sync) noexcept;

    // Non-mutating boundary query taken under the same locks that publish. It
    // performs no I/O and never latches a fault, so a show clock may call it
    // every frame to decide between publishing and holding.
    [[nodiscard]] NodeBoundaryState boundary_state() const noexcept;

private:
    struct InflightFrame final {
        cmd::FrameNumber frame{};
        core::u64 encoded_bytes{};
        core::u64 acknowledged_sequence{};
        cmd::StreamCursor before{};
        cmd::StreamCursor after{};
        bool prepared_sent{};
        bool permitted{};
        bool committed{};
        core::u64 barrier_ordinal{};
    };

    enum class NodeControlKind : std::uint8_t {
        Prepared,
        Committed,
        HeartbeatAcknowledgement,
        Goodbye,
    };

    struct NodeControlIntent final {
        NodeControlKind kind{NodeControlKind::Prepared};
        core::u64 sequence{};
        core::u64 acknowledged_sequence{};
        cmd::StreamCursor after{};
        RuntimeAssetAdoption asset_adoption;
        RuntimeOutputSync output_sync;
        GoodbyeReason goodbye_reason{GoodbyeReason::None};
    };

    [[nodiscard]] core::u64 enqueue_control_locked(
        NodeControlKind kind,
        core::u64 acknowledged_sequence,
        const cmd::StreamCursor& after,
        RuntimeAssetAdoption asset_adoption = {},
        RuntimeOutputSync output_sync = {},
        GoodbyeReason goodbye_reason = GoodbyeReason::None) noexcept;
    [[nodiscard]] core::Result<FrameAcknowledgementFlushReport, ResumeSessionError>
    flush_frame_acknowledgements_through(
        std::optional<core::u64> through_sequence);
    [[nodiscard]] core::Result<ReceivedFrameReport, ResumeSessionError>
    receive_decoded_live_frame(
        const FrameBatch& frame,
        std::size_t encoded_bytes);
    [[nodiscard]] core::Result<FrameCommitPermitReport, ResumeSessionError>
    receive_decoded_commit_permit(const FrameCommitPermit& permit);
    [[nodiscard]] core::Result<HeartbeatReceivedReport, ResumeSessionError>
    receive_decoded_heartbeat(const Heartbeat& heartbeat);
    [[nodiscard]] core::Result<GoodbyeReport, ResumeSessionError>
    receive_decoded_goodbye(const Goodbye& goodbye);

    TcpFramedStream stream_;
    cmd::NodeCommandBus* node_;
    NodeSessionConfig config_;
    std::atomic<NodeSessionState> state_{NodeSessionState::Disconnected};
    SessionLimits negotiated_limits_{};
    Id128 session_id_{};
    Id128 director_epoch_{};
    cmd::StreamCursor delivered_cursor_{};
    cmd::StreamCursor target_cursor_{};
    core::u64 expected_sequence_{1U};
    std::optional<cmd::CheckpointPreparationToken> checkpoint_token_;
    std::vector<InflightFrame> inflight_frames_;
    core::u64 inflight_bytes_{};
    std::vector<NodeControlIntent> acknowledgement_intents_;
    std::size_t acknowledgement_head_{};
    std::size_t acknowledgement_size_{};
    core::u64 next_ack_sequence_{2U};
    core::u64 last_flushed_ack_sequence_{1U};
    core::u64 last_commit_permit_ordinal_{};
    std::optional<cmd::StreamCursor> last_released_through_;
    RuntimeAssetAdoption runtime_asset_adoption_{};
    RuntimeOutputSync runtime_output_sync_{};
    // Latched by the writer when a local Goodbye leaves the socket, consumed
    // once by close(). Written and read from different threads.
    std::atomic_bool sent_goodbye_{};
    mutable std::mutex node_mutex_;
    mutable std::mutex inflight_mutex_;
    mutable std::mutex control_write_mutex_;
};

} // namespace mediaserver::net
