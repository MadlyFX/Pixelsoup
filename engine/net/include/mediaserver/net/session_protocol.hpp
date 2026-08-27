#pragma once

#include <cstddef>
#include <span>
#include <variant>
#include <vector>

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>

namespace mediaserver::net {

inline constexpr core::u16 current_session_format_version = 2U;
inline constexpr core::u32 min_heartbeat_response_timeout_ms = 250U;
inline constexpr core::u32 max_heartbeat_response_timeout_ms = 120'000U;
inline constexpr core::u32 max_session_node_index = 4'095U;

struct Id128 final {
    core::u64 high{};
    core::u64 low{};

    [[nodiscard]] explicit operator bool() const noexcept { return high != 0U || low != 0U; }
    bool operator==(const Id128&) const = default;
};

struct SessionLimits final {
    core::u64 max_packet_bytes{};
    core::u64 max_frame_bytes{};
    core::u32 max_inflight_frames{};
    core::u64 max_inflight_bytes{};
    core::u64 max_checkpoint_bytes{};

    bool operator==(const SessionLimits&) const = default;
};

enum class ResumeMode : core::u8 {
    None = 0U,
    UpToDate = 1U,
    Replay = 2U,
    Checkpoint = 3U,
};

enum class RejectCode : core::u16 {
    None = 0U,
    UnsupportedVersion = 1U,
    ShowMismatch = 2U,
    InvalidCursor = 3U,
    ResumeUnavailable = 4U,
    CapacityExceeded = 5U,
    ProtocolError = 6U,
    ServerShutdown = 7U,
    NodeLeaseRejected = 8U,
};

enum class GoodbyeReason : core::u8 {
    None = 0U,
    NormalShutdown = 1U,
    Restart = 2U,
    Maintenance = 3U,
    SessionReplaced = 4U,
};

struct Packet final {
    std::vector<std::byte> bytes;

    bool operator==(const Packet&) const = default;
};

struct ClientHello final {
    core::u16 min_version{};
    core::u16 max_version{};
    Id128 show_id;
    Id128 node_id;
    Id128 incarnation;
    // Runtime output identity. This is deliberately separate from node_id,
    // whose ownership/lease semantics must remain stable across output-map
    // rebinding.
    core::u32 node_index{};
    // Zero means that this node has not yet observed a Director epoch.
    Id128 last_director_epoch;
    cmd::StreamCursor cursor;
    bool force_checkpoint{};
    SessionLimits limits;

    bool operator==(const ClientHello&) const = default;
};

struct ServerWelcome final {
    core::u16 selected_version{};
    Id128 show_id;
    Id128 session_id;
    Id128 director_epoch;
    ResumeMode mode{ResumeMode::UpToDate};
    cmd::StreamCursor accepted_from;
    cmd::StreamCursor target;
    SessionLimits limits;

    bool operator==(const ServerWelcome&) const = default;
};

struct FrameBatch final {
    Id128 session_id;
    core::u64 sequence{};
    cmd::StreamCursor before;
    cmd::StreamCursor after;
    std::vector<Packet> packets;

    bool operator==(const FrameBatch&) const = default;
};

struct CheckpointTransfer final {
    Id128 session_id;
    core::u64 sequence{};
    cmd::StreamCursor cursor;
    std::vector<std::byte> wire_packet;

    bool operator==(const CheckpointTransfer&) const = default;
};

struct SyncComplete final {
    Id128 session_id;
    core::u64 sequence{};
    cmd::StreamCursor target;

    bool operator==(const SyncComplete&) const = default;
};

struct ResumeAck final {
    Id128 session_id;
    core::u64 sequence{};
    cmd::StreamCursor target;

    bool operator==(const ResumeAck&) const = default;
};

struct Reject final {
    RejectCode code{RejectCode::ProtocolError};
    Id128 show_id;
    Id128 director_epoch;
    cmd::StreamCursor authoritative_tail;

    bool operator==(const Reject&) const = default;
};

// A node-to-Director acknowledgement that the referenced FrameBatch has been
// validated and staged without making its document state visible yet. sequence
// is the monotonic node-to-Director control sequence (ResumeAck is 1, so the
// first frame acknowledgement is 2); acknowledged_sequence identifies the
// Director-to-node FrameBatch.
struct FramePrepared final {
    Id128 session_id;
    core::u64 sequence{};
    core::u64 acknowledged_sequence{};
    cmd::StreamCursor after;

    bool operator==(const FramePrepared&) const = default;
};

// A node-to-Director acknowledgement that the referenced staged frame became
// visible at the node's show-clock boundary.
struct FrameCommitted final {
    Id128 session_id;
    core::u64 sequence{};
    core::u64 acknowledged_sequence{};
    cmd::StreamCursor after;

    bool operator==(const FrameCommitted&) const = default;
};

// A Director-to-node authorization to make the referenced prepared FrameBatch
// visible. sequence follows the Director outbound stream; acknowledged_sequence
// binds the exact earlier FrameBatch, and barrier_ordinal identifies the durable
// barrier release decision.
struct FrameCommitPermit final {
    Id128 session_id;
    Id128 director_epoch;
    core::u64 sequence{};
    core::u64 acknowledged_sequence{};
    core::u64 barrier_ordinal{};
    cmd::StreamCursor before;
    cmd::StreamCursor after;

    bool operator==(const FrameCommitPermit&) const = default;
};

// A Director-to-node liveness probe. sequence consumes the next item in the
// Director outbound stream, released_through is the Director's contiguous
// durably released cursor, and response_timeout_ms is measured locally after
// the complete message is received. Only an exact HeartbeatAck proves
// liveness; wall-clock timestamps are deliberately not exchanged.
struct Heartbeat final {
    Id128 session_id;
    Id128 director_epoch;
    core::u64 sequence{};
    core::u32 response_timeout_ms{};
    cmd::StreamCursor released_through;

    bool operator==(const Heartbeat&) const = default;
};

// Whether a render process joined the vendor swap group / present barrier its
// project asked for. Unknown is the state of a process that has not reported;
// Unsupported is a presentation backend that cannot join one at all. Neither
// is ever treated as evidence of synchronized presentation.
enum class RuntimeBarrierState : core::u8 {
    Unknown = 0U,
    Unsupported = 1U,
    NotJoined = 2U,
    Joining = 3U,
    Joined = 4U,
    // Joined earlier and dropped out: a loss-of-sync event, not a
    // configuration that never started.
    Left = 5U,
    Failed = 6U,
};

// Renderer-owned physical presentation state, reported over a validated
// heartbeat beside asset adoption.
//
// The machine-local lifecycle agent reports the sync device and per-display
// lock; only the render process knows whether its swapchains joined the
// barrier and how its frames actually landed. Both halves are needed before
// anything may claim that separate outputs presented the same image on the
// same raster boundary.
struct RuntimeOutputSync final {
    RuntimeBarrierState barrier{RuntimeBarrierState::Unknown};
    core::u32 group{};
    core::u32 barrier_id{};
    core::u32 joined_outputs{};
    core::u32 expected_outputs{};
    core::u64 presented_frames{};
    // Boundaries with no complete frame to present.
    core::u64 missed_frames{};
    // Frames presented after their raster deadline.
    core::u64 late_frames{};
    // Boundaries where the previous complete frame was deliberately held by
    // the project late-frame policy rather than partially advancing a wall.
    core::u64 held_frames{};
    // Frames the presentation backend showed again because no new swapchain
    // image was acquired.
    core::u64 repeated_frames{};
    core::u64 loss_of_sync_events{};

    bool operator==(const RuntimeOutputSync&) const = default;
};

// A node-to-Director response to one outstanding Heartbeat. sequence consumes
// the next item in the node control stream, while acknowledged_sequence binds
// the Heartbeat in the opposite-direction stream. visible_through is the
// node's last show-clock-committed cursor and is not release authority.
struct HeartbeatAck final {
    Id128 session_id;
    Id128 director_epoch;
    core::u64 sequence{};
    core::u64 acknowledged_sequence{};
    cmd::StreamCursor visible_through;
    core::u64 asset_manifest_digest{};
    bool asset_manifest_ready{};
    RuntimeOutputSync output_sync;

    bool operator==(const HeartbeatAck&) const = default;
};

// Renderer-owned runtime state reported over a validated heartbeat. `ready`
// means the exact manifest's verified local-path overlay has already been
// installed by the render loop; store presence by itself is insufficient.
struct RuntimeAssetAdoption final {
    core::u64 manifest_digest{};
    bool ready{};

    bool operator==(const RuntimeAssetAdoption&) const = default;
};

// A bidirectional graceful-close notice. sequence belongs to the sender's
// stream and final_cursor is only that sender's safe externally visible
// frontier. It must never be used to advance the peer's authoritative state.
struct Goodbye final {
    Id128 session_id;
    Id128 director_epoch;
    core::u64 sequence{};
    GoodbyeReason reason{GoodbyeReason::None};
    cmd::StreamCursor final_cursor;

    bool operator==(const Goodbye&) const = default;
};

using SessionMessage = std::variant<
    ClientHello,
    ServerWelcome,
    FrameBatch,
    CheckpointTransfer,
    SyncComplete,
    ResumeAck,
    Reject,
    FramePrepared,
    FrameCommitted,
    FrameCommitPermit,
    Heartbeat,
    HeartbeatAck,
    Goodbye>;

enum class SessionEncodeError {
    InvalidClientHello,
    InvalidServerWelcome,
    InvalidFrameBatch,
    InvalidCheckpointTransfer,
    InvalidSyncComplete,
    InvalidResumeAck,
    InvalidReject,
    SizeLimitExceeded,
    InvalidFramePrepared,
    InvalidFrameCommitted,
    InvalidFrameCommitPermit,
    InvalidHeartbeat,
    InvalidHeartbeatAck,
    InvalidGoodbye,
};

enum class SessionDecodeError {
    InvalidBuffer,
    UnsupportedVersion,
    MissingBody,
    MissingRequiredField,
    InvalidUnion,
    InvalidClientHello,
    InvalidServerWelcome,
    InvalidFrameBatch,
    InvalidCheckpointTransfer,
    InvalidSyncComplete,
    InvalidResumeAck,
    InvalidReject,
    SizeLimitExceeded,
    InvalidFramePrepared,
    InvalidFrameCommitted,
    InvalidFrameCommitPermit,
    InvalidHeartbeat,
    InvalidHeartbeatAck,
    InvalidGoodbye,
};

struct SessionDecodeLimits final {
    std::size_t max_message_bytes{72U * 1024U * 1024U};
    std::size_t max_packet_bytes{16U * 1024U * 1024U};
    std::size_t max_checkpoint_bytes{64U * 1024U * 1024U};
    std::size_t max_packets_per_batch{65'536U};
    std::size_t max_aggregate_packet_bytes{64U * 1024U * 1024U};
    std::size_t verifier_max_depth{16U};
    std::size_t verifier_max_tables{200'000U};
};

[[nodiscard]] core::Result<std::vector<std::byte>, SessionEncodeError> encode_session(
    const SessionMessage& message,
    const SessionDecodeLimits& limits = {});

[[nodiscard]] core::Result<SessionMessage, SessionDecodeError> decode_session(
    std::span<const std::byte> bytes,
    const SessionDecodeLimits& limits = {});

} // namespace mediaserver::net
