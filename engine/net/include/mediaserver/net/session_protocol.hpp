#pragma once
#include <mediaserver/core/frame_timing.hpp>
#include <mediaserver/core/render_conformance.hpp>
#include <mediaserver/core/asset_publication.hpp>

#include <cstddef>
#include <span>
#include <variant>
#include <vector>

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/show/transport_state.hpp>

namespace mediaserver::net {

// Version 11 adds the priority transport message an operator's stop, pause or
// seek travels in ahead of the frame stream. All participants must upgrade
// together.
inline constexpr core::u16 current_session_format_version = 11U;
inline constexpr core::u32 min_heartbeat_response_timeout_ms = 250U;
inline constexpr core::u32 max_heartbeat_response_timeout_ms = 120'000U;
inline constexpr core::u32 max_session_node_index = 4'095U;
inline constexpr std::size_t max_runtime_media_assets = 1'024U;

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

// One tracked camera pose on the per-frame path, stamped by the Director with
// the exact show frame it applies to.
//
// This is the ICVFX tracking channel. Three properties matter and each is a
// deliberate choice:
//
//  * **It is not a document command.** Tracking written into the document would
//    bump the document version on every frame, and document version is the
//    conservative temporal epoch: every frame would reset temporal history,
//    permanently defeating temporal antialiasing and everything after it.
//  * **It is frame-exact, not recent.** Riding Heartbeat would give a node a
//    pose that was true a moment ago, and two nodes would use it for different
//    frames. The show frame is carried in the message and is authoritative.
//  * **The Director samples it, once.** Nodes never read a tracker. A node
//    sampling its own feed would get a different pose from its neighbour and
//    open a seam mid-wall.
//
// Latency compensation is already applied when the Director stamps the poses,
// so the message's `control_frame` is the frame they are *for*. Angles are in
// turns and position is in metres of stage space, matching
// `scene::TrackedCamera`.
// One camera's pose, separated from the session envelope so a Director can hand
// one to a session without being able to forge a session id, an epoch, or an
// outbound sequence.
struct TrackedCameraSample final {
    // The authored camera node this pose drives. A stage runs several cameras,
    // each with its own feed, so a pose means nothing without saying whose.
    core::u64 camera_node{};
    float position_x{};
    float position_y{};
    float position_z{};
    float yaw_turns{};
    float pitch_turns{};
    float roll_turns{};
    float field_of_view_turns{};
    float aspect_ratio{};
    // Monotonic tracker ordinal. Diagnostics only; it never orders frames.
    core::u64 tracker_sequence{};
    core::u32 camera_id{};
    // Source-native zoom/focus/iris coordinates. Zero is a valid position, so
    // each component carries an explicit validity bit.
    float zoom_encoder{};
    float focus_encoder{};
    float iris_encoder{};
    bool zoom_encoder_valid{};
    bool focus_encoder_valid{};
    bool iris_encoder_valid{};

    float focal_length_mm{};
    float focus_distance_m{};
    float aperture_f_number{};

    bool operator==(const TrackedCameraSample&) const = default;
};

// One control frame's worth of tracked poses: what the Director stamped, and
// what a node applies together.
//
// Carried as one value through the whole session layer because the stack has
// to land together. A node that applied camera A's new pose and camera B's
// previous one would composite two moments into a single frame.
struct TrackedCameraFrame final {
    // The control frame these poses are for, after latency compensation.
    core::u64 control_frame{};
    // Numbers the Director's pose stream, assigned once when the pose is
    // published rather than when it is sent. A node compares it against the
    // last it saw: a gap is a pose that was overwritten in this session's
    // mailbox before the session drained it, which is the one loss the
    // outbound `sequence` cannot show, because that is assigned at send time
    // and is therefore gap-free however many poses were coalesced away.
    core::u64 pose_sequence{};
    // In ascending camera-node order, at most `max_tracked_camera_samples`.
    std::vector<TrackedCameraSample> samples;

    bool operator==(const TrackedCameraFrame&) const = default;
};

// How many camera poses one message may carry. The same bound the scene puts
// on tracked cameras, so a message can never describe a stack a node could not
// draw.
inline constexpr std::size_t max_tracked_camera_samples = 4U;

// Every tracked camera's pose for one control frame.
//
// One message rather than one per camera: the stack has to land together, or a
// node would composite camera A's new pose over camera B's previous one while
// its neighbour split them the other way.
struct TrackedCameraPose final {
    Id128 session_id;
    Id128 director_epoch;
    core::u64 sequence{};
    // Numbers the Director's pose stream, not the outbound message stream, so
    // a node can tell that a pose was coalesced away before it was ever sent.
    // See the note on TrackedCameraFrame.
    core::u64 pose_sequence{};
    // The control frame these poses are for, after latency compensation. Not
    // the show frame: a parked timeline stops advancing that, and parked is
    // exactly when an operator is lining up a shot.
    core::u64 control_frame{};
    // In the scene's composite order, and at most `max_tracked_camera_samples`.
    std::vector<TrackedCameraSample> samples;

    bool operator==(const TrackedCameraPose&) const = default;
};

// True when every number in a sample is finite, inside the bounds below, and
// describes a frustum that can be built. A sample this rejects never reaches a
// renderer: a node holds its previous pose instead, which is always better than
// swinging the wall.
[[nodiscard]] bool valid(const TrackedCameraSample& sample) noexcept;

// The widest stage a pose may describe, in metres, and the bounds on the angles
// and lens shape. A pose outside these is a unit mistake or a corrupt sample,
// and is refused at the codec rather than reaching a renderer: swinging the
// wall is worse than holding the previous pose.
inline constexpr float max_tracked_position_metres = 100'000.0F;
inline constexpr float max_tracked_turns = 1'000.0F;
inline constexpr float max_tracked_aspect_ratio = 1'000.0F;
inline constexpr float max_tracked_lens_encoder = 16'777'215.0F;

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

    core::u64 conformance_request{};

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

// Hardware source used by the node's frame-boundary driver. It travels with
// the barrier counters so Director health distinguishes a locked presentation
// cohort from one paced by a free-running local clock.
enum class RuntimeReferenceClockState : core::u8 {
    Unknown = 0U,
    Unsupported = 1U,
    Waiting = 2U,
    Locked = 3U,
    Lost = 4U,
    Failed = 5U,
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
    // Counters reported by the present-barrier driver itself, never inferred
    // from this process's local present calls.
    core::u64 barrier_presents{};
    core::u64 barrier_presents_in_sync{};
    core::u64 barrier_flips_in_sync{};
    RuntimeReferenceClockState reference_clock{
        RuntimeReferenceClockState::Unknown};
    core::u32 reference_frame_counter{};

    bool operator==(const RuntimeOutputSync&) const = default;
};

// Exact renderer policy and selection last observed by the node's Program
// presentation path. The profile digest is calculated from every authored
// RendererSettings field; a Director never treats an older profile as evidence
// for a newly edited one. These protocol enums deliberately do not depend on
// engine/render so a headless session can still carry an honest Unknown.
enum class RuntimeRendererBackend : core::u8 {
    Unknown = 0U,
    Basic = 1U,
    Advanced = 2U,
};

enum class RuntimeRendererReadinessState : core::u8 {
    Unknown = 0U,
    Ready = 1U,
    BasicFallback = 2U,
    MissingCapabilities = 3U,
    AdvancedImplementationUnavailable = 4U,
    InvalidPolicy = 5U,
};

struct RuntimeRendererReadiness final {
    core::u64 profile_digest{};
    RuntimeRendererReadinessState state{
        RuntimeRendererReadinessState::Unknown};
    RuntimeRendererBackend requested{RuntimeRendererBackend::Unknown};
    RuntimeRendererBackend selected{RuntimeRendererBackend::Unknown};
    core::u64 missing_capabilities{};
    bool satisfied{};
    bool fallback{};

    core::AssetSetReadiness assets{};

    core::u64 temporal_restarts{};
    core::u64 temporal_restart_frame{};

    core::RenderConformance conformance{};

    core::FrameTiming timing{};

    bool operator==(const RuntimeRendererReadiness&) const = default;
};

// Machine-local media truth belongs to the renderer that will put the plate
// on air. These values are reported by output nodes and never enter the
// authored project document.
enum class RuntimeMediaDecodeState : core::u8 {
    Unknown = 0U,
    HardwareAccelerated = 1U,
    SoftwareOnly = 2U,
    Unavailable = 3U,
};

enum class RuntimeProxyState : core::u8 {
    Source = 0U,
    ProxyReady = 1U,
    PreferFallback = 2U,
    RequiredUnready = 3U,
    Failed = 4U,
};

enum class RuntimeProxyReason : core::u8 {
    None = 0U,
    SourcePolicy = 1U,
    SourceUnavailable = 2U,
    InvalidConfiguration = 3U,
    ProxyNotConfigured = 4U,
    SourceFingerprintUnavailable = 5U,
    CacheObservationMissing = 6U,
    CacheKeyMismatch = 7U,
    CacheMissing = 8U,
    CacheQueued = 9U,
    CacheGenerating = 10U,
    CacheStale = 11U,
    CacheFailed = 12U,
    CacheCancelled = 13U,
    FrameMapMissing = 14U,
    FrameMapKeyMismatch = 15U,
    FrameMapInvalid = 16U,
};

enum class RuntimeProxyIssue : core::u8 {
    None = 0U,
    MissingAssetIdentity = 1U,
    AssetNotFound = 2U,
    AssetKindMismatch = 3U,
    PlacementSourceMismatch = 4U,
    AvailabilityMissing = 5U,
    CacheInspectionFailed = 6U,
    ArtifactLoadFailed = 7U,
    ArtifactMetadataMismatch = 8U,
    FrameMappingFailed = 9U,
    MaterializationFailed = 10U,
};

struct RuntimeMediaAssetStatus final {
    core::u64 asset_id{};
    RuntimeMediaDecodeState decode{RuntimeMediaDecodeState::Unknown};
    RuntimeProxyState proxy{RuntimeProxyState::Source};
    RuntimeProxyReason proxy_reason{RuntimeProxyReason::SourcePolicy};
    RuntimeProxyIssue proxy_issue{RuntimeProxyIssue::None};
    bool has_transcode_progress{};
    bool transcode_queued{};
    core::u64 transcode_completed_frames{};
    core::u64 transcode_total_frames{};

    bool operator==(const RuntimeMediaAssetStatus&) const = default;
};
// What one node is doing with the tracking it was sent, reported back over the
// heartbeat.
//
// A node used to say nothing about this. The Director could see how many poses
// it had published and nothing about what became of them, so a node holding a
// stale pose, a node that missed one, and a node quietly skipping its inner
// render were indistinguishable from a healthy one - and all three put a
// different picture on that node's share of the wall.
struct RuntimeTracking final {
    // The Director's pose-stream ordinal this node last received, and how many
    // it never saw. A gap is a pose coalesced away before this session drained
    // it: not a fault, but this node's plate is running behind its neighbours'.
    core::u64 pose_sequence{};
    core::u64 poses_missed{};
    // Frames this node answered with a pose stamped for exactly that frame,
    // and frames it answered by holding an older one. Holding is correct
    // behaviour - never extrapolation - but a long run of it is a tracker
    // that has stopped.
    core::u64 poses_applied{};
    core::u64 held_frames{};
    // Plates this node actually rendered, and plates it skipped because its
    // slice missed the rectangle. The skip is the whole performance argument
    // for ICVFX and on a wide wall it is most outputs, so a healthy node
    // reports a large number here.
    core::u64 plates_drawn{};
    core::u64 plates_skipped{};
    // Plans refused: a target past its bound, or a compositor that would not
    // accept one. Counted, never absorbed.
    core::u64 plate_refusals{};
    // Poses this node received and overwrote before its render loop drained
    // them. Distinct from poses_missed on purpose: a miss is a pose that never
    // reached this node, this is one that did and was dropped here. They point
    // at different machines.
    core::u64 poses_coalesced{};

    bool operator==(const RuntimeTracking&) const = default;
};


// A node-to-Director response to one outstanding Heartbeat. sequence consumes
// the next item in the node control stream, while acknowledged_sequence binds
// the Heartbeat in the opposite-direction stream. visible_through is the
// node's last show-clock-committed cursor and is not release authority.
// Whether this process opened the physical outputs its bindings name, and if
// not, which specific thing stopped it. A node that cannot deploy its outputs
// has no presenters, and a node with no presenters reports a default renderer
// readiness - which on the Director is indistinguishable from a node that has
// not finished loading. Without this the operator is told the assets are not
// ready while the real fault is a display. Unknown says nothing and is never
// evidence that the outputs opened.
enum class RuntimeOutputDeploymentState : core::u8 {
    Unknown = 0U,
    Ready = 1U,
    InventoryUnavailable = 2U,
    InvalidOutputMode = 3U,
    MissingDisplayTarget = 4U,
    InactiveDisplayTarget = 5U,
    DuplicateDisplayTarget = 6U,
    MirroredDisplayTarget = 7U,
    DuplicateRenderServer = 8U,
    PresenterUnavailable = 9U,
};

struct HeartbeatAck final {
    Id128 session_id;
    Id128 director_epoch;
    core::u64 sequence{};
    core::u64 acknowledged_sequence{};
    cmd::StreamCursor visible_through;
    core::u64 asset_manifest_digest{};
    bool asset_manifest_ready{};
    RuntimeOutputSync output_sync;
    std::vector<RuntimeMediaAssetStatus> media_assets;
    RuntimeRendererReadiness renderer;
    RuntimeTracking tracking;
    RuntimeOutputDeploymentState output_deployment{
        RuntimeOutputDeploymentState::Unknown};

    bool operator==(const HeartbeatAck&) const = default;
};

// Renderer-owned runtime state reported over a validated heartbeat. `ready`
// means the exact manifest's verified local-path overlay has already been
// installed by the render loop; store presence by itself is insufficient.
struct RuntimeAssetAdoption final {
    core::u64 manifest_digest{};
    bool ready{};
    std::vector<RuntimeMediaAssetStatus> media_assets;
    // Carried here because this is the report the node already republishes
    // whenever its runtime adoption changes, which includes every output
    // deployment attempt. It is deliberately not folded into `ready`: a
    // display fault and a missing asset are different things to fix, and an
    // operator told the wrong one looks in the wrong place.
    RuntimeOutputDeploymentState deployment{
        RuntimeOutputDeploymentState::Unknown};

    bool operator==(const RuntimeAssetAdoption&) const = default;
};

// An operator transport anchor sent ahead of the frame stream, so a cluster
// that is behind on frames still stops, pauses or seeks when the operator
// asks. The anchor is also authored into the sealed frame at its own control
// frame, which remains the authority: a node uses this only until its
// committed document carries an anchor at least as new. Never scheduled and
// never a crossfade - those are the show re-anchoring itself, and must play
// through in order.
struct PriorityTransport final {
    Id128 session_id;
    Id128 director_epoch;
    core::u64 sequence{};
    show::TransportAnchor anchor;

    bool operator==(const PriorityTransport&) const = default;
};

[[nodiscard]] bool valid_priority_anchor(
    const show::TransportAnchor& anchor) noexcept;

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
    Goodbye,
    TrackedCameraPose,
    PriorityTransport>;

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
    InvalidTrackedCameraPose,
    InvalidPriorityTransport,
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
    InvalidTrackedCameraPose,
    InvalidPriorityTransport,
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
