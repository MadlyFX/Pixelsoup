#pragma once

#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/net/director_session_hub.hpp>
#include <mediaserver/net/director_session_service.hpp>

namespace mediaserver::net {

namespace detail {
struct DirectorFrameBarrierControl;
} // namespace detail

struct DirectorFrameBarrierLimits final {
    std::size_t max_sessions{1'024U};
    std::size_t max_open_frames{256U};
    std::size_t max_pending_per_session{256U};
    // A catching-up session joins the next publication cut once it has
    // Prepared to within this many frames of the tracked tail. What remains
    // is then the ordinary in-flight pipeline any live member has, not
    // history, so joining costs the cohort no more than its own link latency.
    // Requiring the exact tail instead never converges while the Director
    // keeps sealing, because a node's acknowledgement always trails the seal.
    std::uint64_t max_join_prepared_lag{2U};
    // Lockstep. Released frames are what nodes present, so a frame some
    // member has not committed yet is a frame its neighbours may already be
    // showing. At most this many such frames may be outstanding; beyond it,
    // release waits for the slowest member. When none are outstanding, every
    // frame the cohort has prepared is released at once, so a cohort that
    // fell behind catches up together instead of one frame per boundary.
    std::size_t lockstep_window{2U};
    // How far the tracked tail may run ahead of the release frontier before
    // the members holding it back are taken out of lockstep. Until then the
    // whole cohort waits for its slowest member while the Director keeps
    // sealing; past it, one stuck node would hold every output indefinitely.
    // A demoted member stays connected, catches up on its own, and rejoins at
    // a cut. Set it below max_open_frames so sealing never waits on records;
    // a larger value is treated as max_open_frames.
    std::size_t max_cohort_lag_frames{192U};
};

// Persistence boundary used by the permit coordinator until DurableDirector
// exposes its release receipt directly. true means the exact retained
// publication and ordinal are durable; false must be fail-closed. Calls happen
// without the barrier mutex held. The authority must outlive the barrier.
class DirectorFrameReleaseAuthority {
public:
    virtual ~DirectorFrameReleaseAuthority() = default;

    // One greater than the last durable release decision. Zero means the
    // authority cannot safely continue this epoch.
    [[nodiscard]] virtual core::u64 next_release_ordinal() const noexcept = 0;

    // Returns the exact durable ordinal for an already released historical
    // frame. This lets a catching-up session receive the original decision
    // after the barrier's diagnostic slot has been retired.
    [[nodiscard]] virtual std::optional<core::u64> release_ordinal(
        const cmd::StreamCursor& before,
        const cmd::StreamCursor& after) const noexcept = 0;

    [[nodiscard]] virtual bool persist_release(
        const PublishedDirectorFrame& frame,
        core::u64 barrier_ordinal) noexcept = 0;
};

struct DirectorFrameBarrierSessionId final {
    std::size_t slot{};
    std::uint64_t generation{};

    bool operator==(const DirectorFrameBarrierSessionId&) const = default;
};

struct DirectorFrameBarrierId final {
    std::size_t slot{};
    std::uint64_t generation{};
    std::uint64_t ordinal{};

    bool operator==(const DirectorFrameBarrierId&) const = default;
};

// The progress view remains a diagnostic acknowledgement aggregate. When the
// barrier is constructed with a release authority, the same exact cohort also
// gates durable, cursor-bound commit permits; the legacy constructor retains
// observational-only behaviour for tooling that does not run a TCP service.
enum class DirectorFrameBarrierProgress : std::uint8_t {
    EmptyCohort,
    AwaitingPrepared,
    AwaitingCommitted,
    Complete,
};

enum class DirectorFrameBarrierPhase : std::uint8_t {
    Prepared,
    Committed,
};

struct DirectorNodeConnection final {
    enum class State : std::uint8_t { Disconnected, CatchingUp, Live };
    State state{State::Disconnected};
    core::u64 committed_frame{};
    core::u64 target_frame{};
};

enum class DirectorFrameBarrierWaitStatus : std::uint8_t {
    Reached,
    Degraded,
    TimedOut,
};

enum class DirectorFrameBarrierTrackingStatus : std::uint8_t {
    Tracked,
    InvariantFault,
};

enum class DirectorFrameBarrierErrorCode : std::uint8_t {
    InvalidConfiguration,
    BarrierFaulted,
    FrameCapacity,
    FrameGenerationExhausted,
    HubFailure,
    StaleFrame,
    FrameIncomplete,
};

// Observer callbacks return only bool/void so a session worker can remain
// decoupled from aggregation policy. These diagnostics retain the precise
// reason for the latest rejected observer callback.
enum class DirectorFrameBarrierObserverError : std::uint8_t {
    None,
    InvalidRegistration,
    SessionCapacity,
    DuplicateNode,
    DuplicateNodeIndex,
    DuplicateSession,
    CursorAhead,
    CursorMismatch,
    UnknownSession,
    InvalidAcknowledgement,
    AcknowledgementSequenceMismatch,
    AcknowledgementCursorMismatch,
    AcknowledgementOutOfOrder,
    PermitCapacity,
    PermitMismatch,
    ReleaseFailed,
    BarrierFaulted,
};

struct DirectorFrameBarrierError final {
    DirectorFrameBarrierErrorCode code{};
    std::optional<DirectorSessionHubError> hub_error;
};

struct DirectorFrameBarrierObserverDiagnostics final {
    DirectorFrameBarrierObserverError last_error{
        DirectorFrameBarrierObserverError::None};
    Id128 last_session_id{};
    std::uint64_t rejected_registrations{};
    std::uint64_t rejected_acknowledgements{};
    std::uint64_t ignored_unregistrations{};
    std::size_t live_sessions{};
    std::size_t catching_up_sessions{};
    // Members taken out of lockstep for exceeding max_cohort_lag_frames, and
    // the node index of the most recent one.
    std::uint64_t lockstep_demotions{};
    core::u32 last_demoted_node{};
};

struct DirectorFrameBarrierSummary final {
    DirectorFrameBarrierId id;
    cmd::StreamCursor before;
    cmd::StreamCursor after;
    DirectorFrameBarrierProgress progress{
        DirectorFrameBarrierProgress::EmptyCohort};
    std::size_t members{};
    std::size_t prepared{};
    std::size_t committed{};
    std::size_t lost{};
    bool tracking_fault{};

    [[nodiscard]] bool degraded() const noexcept {
        return lost != 0U || tracking_fault;
    }
};

struct DirectorFrameBarrierLostSession final {
    DirectorFrameBarrierSessionId member;
    Id128 session_id;
    NodeSessionIdentity peer;
    DirectorSessionLeaveReason reason{DirectorSessionLeaveReason::SessionFailure};
    bool was_prepared{};
};

struct DirectorFrameBarrierLostSessionsReport final {
    std::size_t total{};
    std::size_t copied{};
};

struct DirectorNodePlayhead final {
    core::u32 node_index{};
    RuntimePlayhead playhead{};

    bool operator==(const DirectorNodePlayhead&) const = default;
};

struct DirectorFrameBarrierWaitReport final {
    DirectorFrameBarrierWaitStatus status{
        DirectorFrameBarrierWaitStatus::TimedOut};
    DirectorFrameBarrierSummary summary;
};

struct DirectorTrackedSealOutcome final {
    DirectorSealOutcome seal;
    DirectorFrameBarrierId barrier;
    DirectorFrameBarrierTrackingStatus tracking_status{
        DirectorFrameBarrierTrackingStatus::Tracked};
};

// register_live(), on_ack(), unregister(), and seal_and_track() share one
// membership linearization mutex. seal_and_track() reserves its fixed record
// and exact cohort before calling the hub, keeps the cut held through the
// durable seal, and finalizes without allocation after success. Consequently:
//
//  * a join is either wholly before or wholly after a publication cohort;
//  * a leave after the cut marks the retained member lost and never removes
//    it. A lost member no longer holds back release: a frame releases once
//    every surviving member has prepared, and the snapshot still reports it
//    degraded so the loss stays visible;
//  * a full barrier table rejects before the WAL or hub can advance.
//
// Registrations behind tracked_tail() are bounded CatchingUp sessions. They do
// not enter cohorts until they are caught up: either a validated Committed
// acknowledgement advances their cursor to the exact current tail, or, at a
// publication cut, their Prepared cursor is within max_join_prepared_lag of
// it. Both are decided while holding the same cut mutex.
class DirectorFrameBarrier final : public DirectorSessionObserver {
public:
    explicit DirectorFrameBarrier(
        cmd::StreamCursor initial_tail,
        DirectorFrameBarrierLimits limits = {});
    DirectorFrameBarrier(
        cmd::StreamCursor initial_tail,
        DirectorFrameReleaseAuthority& release_authority,
        DirectorFrameBarrierLimits limits = {});
    DirectorFrameBarrier(const DirectorFrameBarrier&) = delete;
    DirectorFrameBarrier& operator=(const DirectorFrameBarrier&) = delete;
    DirectorFrameBarrier(DirectorFrameBarrier&&) = delete;
    DirectorFrameBarrier& operator=(DirectorFrameBarrier&&) = delete;
    ~DirectorFrameBarrier() override;
    void request_render_conformance(core::u64 request) noexcept { conformance_request_.store(request, std::memory_order_release); }
    [[nodiscard]] core::u64 conformance_request() const noexcept override { return conformance_request_.load(std::memory_order_acquire); }

    [[nodiscard]] bool register_live(
        const DirectorLiveRegistration& registration) noexcept override;
    // acknowledgement must be the successful result returned by this exact
    // session's DirectorResumeSession. The aggregate independently fences the
    // observer session generation, control ordering, cursor lineage, and
    // per-phase watermarks; it intentionally does not duplicate the session's
    // acknowledged_sequence/outstanding-window validation.
    [[nodiscard]] bool on_ack(
        Id128 session_id,
        const FrameAcknowledgementReport& acknowledgement) noexcept override;
    [[nodiscard]] bool on_heartbeat_ack(
        Id128 session_id,
        const HeartbeatAcknowledgementReport& acknowledgement) noexcept override;
    void unregister(
        Id128 session_id,
        DirectorSessionLeaveReason reason) noexcept override;

    [[nodiscard]] bool coordinates_commit_permits() const noexcept override;
    [[nodiscard]] CommitPermitPoll try_take_commit_permit(
        Id128 session_id,
        FrameCommitPermitIntent& output) noexcept override;
    [[nodiscard]] bool heartbeat_released_through(
        cmd::StreamCursor& output) const noexcept override;
    [[nodiscard]] TrackedPosePoll try_take_tracked_pose(
        Id128 session_id, TrackedCameraFrame& output) noexcept override;
    [[nodiscard]] PriorityTransportPoll try_take_priority_transport(
        Id128 session_id, show::TransportAnchor& output) noexcept override;

    // Hands an operator transport anchor to every session, live or catching
    // up, ahead of the frame stream, and returns how many were given it. A
    // newer anchor replaces one not yet sent. Refuses anything but an
    // operator anchor: a scheduled one plays through in order.
    [[nodiscard]] std::size_t publish_priority_transport(
        const show::TransportAnchor& anchor) noexcept;

    // Hands one tracked camera pose to every live session, and returns how
    // many were given it.
    //
    // The Director samples its tracker once at a frame boundary and calls this
    // once with the result. Fanning out here rather than per session is the
    // whole point: every node in the cohort is given the identical sample, so
    // there is nothing for two of them to disagree about. A sample that cannot
    // build a frustum is refused and nobody is given anything, which leaves
    // every node holding the pose it already had.
    [[nodiscard]] std::size_t publish_tracked_pose(
        const TrackedCameraFrame& sample) noexcept;

    // Poses handed to a session that were replaced before that session drained
    // them. Nonzero means at least one node's plate has been running a sample
    // behind its neighbours', which is a real difference on the wall and was
    // previously invisible from either end.
    [[nodiscard]] core::u64 tracked_poses_coalesced() const noexcept;

    [[nodiscard]] core::Result<
        DirectorTrackedSealOutcome,
        DirectorFrameBarrierError>
    seal_and_track(DirectorSessionHub& hub, cmd::FrameNumber frame);

    [[nodiscard]] core::Result<
        DirectorFrameBarrierSummary,
        DirectorFrameBarrierError>
    snapshot(DirectorFrameBarrierId frame) const noexcept;

    [[nodiscard]] core::Result<
        DirectorFrameBarrierWaitReport,
        DirectorFrameBarrierError>
    wait_for(
        DirectorFrameBarrierId frame,
        DirectorFrameBarrierPhase phase,
        std::chrono::milliseconds timeout) const noexcept;

    [[nodiscard]] core::Result<
        DirectorFrameBarrierLostSessionsReport,
        DirectorFrameBarrierError>
    copy_lost_sessions(
        DirectorFrameBarrierId frame,
        std::span<DirectorFrameBarrierLostSession> output) const noexcept;

    [[nodiscard]] core::Result<
        DirectorFrameBarrierSummary,
        DirectorFrameBarrierError>
    retire(DirectorFrameBarrierId frame) noexcept;

    [[nodiscard]] cmd::StreamCursor tracked_tail() const noexcept;
    [[nodiscard]] cmd::StreamCursor released_through() const noexcept;
    [[nodiscard]] std::size_t active_sessions() const noexcept;
    [[nodiscard]] DirectorNodeConnection node_connection(
        core::u32 node_index) const noexcept;
    // One atomic membership/adoption observation for the exact configured
    // render-output set. Required indices must be unique. Unregistered,
    // catching-up, disconnected, unresolved, or stale-digest nodes fail
    // closed; unrelated live sessions are ignored.
    [[nodiscard]] bool runtime_assets_ready(
        core::u64 manifest_digest,
        std::span<const core::u32> required_node_indices) const noexcept;
    // The physical presentation state last acknowledged by one live render
    // output. False means there is no live session for that output at all,
    // which the readiness gate must treat as unproven rather than as an
    // Unknown-but-present report.
    // What one node last reported about the tracking it was sent. False when
    // that node has no live session, which is itself the answer.
    [[nodiscard]] bool runtime_tracking(
        core::u32 node_index, RuntimeTracking& output) const noexcept;
    // Where each live node's output last presented, from its newest frame
    // commit, in node order. A node that has presented nothing yet is left
    // out. Every live session counts, configured as a Render Server or not.
    [[nodiscard]] std::vector<DirectorNodePlayhead> runtime_playheads() const;

    [[nodiscard]] bool runtime_output_sync(
        core::u32 node_index, RuntimeOutputSync& output) const noexcept;
    // Exact renderer profile/selection carried by the most recent validated
    // heartbeat from one live output node. False is deliberately distinct from
    // an explicit Unknown report and is always fail-closed by Program.
    [[nodiscard]] bool runtime_renderer_readiness(
        core::u32 node_index,
        RuntimeRendererReadiness& output) const noexcept;
    // Complete renderer-owned media report for one live output node and one
    // exact manifest. False means absent, stale, or not yet live.
    [[nodiscard]] bool runtime_asset_adoption(
        core::u32 node_index, core::u64 manifest_digest,
        RuntimeAssetAdoption& output) const noexcept;
    // What this node did with its physical output bindings, independent of any
    // manifest. Deliberately not digest-gated like the media report above: a
    // node whose displays failed is exactly the one whose manifest will not
    // match, and gating the reason on the digest would hide it precisely when
    // it is needed. Unknown when the node is absent or has not reported.
    [[nodiscard]] RuntimeOutputDeploymentState runtime_output_deployment(
        core::u32 node_index) const noexcept;
    [[nodiscard]] std::size_t open_frames() const noexcept;
    [[nodiscard]] DirectorFrameBarrierObserverDiagnostics
    observer_diagnostics() const noexcept;
    [[nodiscard]] const DirectorFrameBarrierLimits& limits() const noexcept {
        return limits_;
    }

private:
    std::atomic<core::u64> conformance_request_{};
    DirectorFrameBarrierLimits limits_;
    std::unique_ptr<detail::DirectorFrameBarrierControl> control_;
};

} // namespace mediaserver::net
