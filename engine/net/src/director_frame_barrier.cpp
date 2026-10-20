#include <mediaserver/net/director_frame_barrier.hpp>

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace mediaserver::net {
namespace {

enum class SessionReadiness : std::uint8_t {
    CatchingUp,
    Live,
};

enum class MemberState : std::uint8_t {
    AwaitingPrepared,
    Prepared,
    Committed,
    Lost,
};

enum class ReleaseState : std::uint8_t {
    CollectingPrepared,
    QuorumPrepared,
    ReleasePersisting,
    Released,
    DegradedBeforeRelease,
};

enum class PendingPermitState : std::uint8_t {
    Held,
    Eligible,
    Claimed,
};

struct PendingPermit final {
    FrameCommitPermitIntent intent{};
    PendingPermitState state{PendingPermitState::Held};
};

struct SessionSlot final {
    bool active{};
    std::uint64_t generation{};
    Id128 session_id{};
    NodeSessionIdentity peer{};
    core::u32 node_index{};
    RuntimeAssetAdoption asset_adoption{};
    RuntimeOutputSync output_sync{};
    RuntimeTracking tracking{};
    RuntimeRendererReadiness renderer{};
    cmd::StreamCursor prepared_cursor{};
    cmd::StreamCursor committed_cursor{};
    bool has_acknowledgement_sequence{};
    std::uint64_t last_acknowledgement_sequence{};
    SessionReadiness readiness{SessionReadiness::CatchingUp};
    std::size_t permit_head{};
    std::size_t permit_size{};
    std::vector<PendingPermit> permits;
    // The tracked camera pose this session has not been sent yet.
    //
    // One slot, overwritten rather than queued. A pose belongs to a frame, so
    // a session that fell behind wants the newest one, not a backlog of stale
    // ones it would send late; and a session that misses one holds its
    // previous pose, which every node agrees on. Queueing would trade that
    // agreement for a resend nobody can use.
    bool has_pose{};
    TrackedCameraFrame pose{};
};

struct FrameMember final {
    DirectorFrameBarrierSessionId id{};
    Id128 session_id{};
    NodeSessionIdentity peer{};
    MemberState state{MemberState::AwaitingPrepared};
    DirectorSessionLeaveReason leave_reason{
        DirectorSessionLeaveReason::SessionFailure};
    bool was_prepared{};
    core::u64 acknowledged_sequence{};
};

struct FrameSlot final {
    bool active{};
    bool reserved{};
    bool tracking_fault{};
    std::uint64_t generation{};
    std::uint64_t ordinal{};
    cmd::StreamCursor before{};
    cmd::StreamCursor after{};
    std::size_t member_count{};
    ReleaseState release_state{ReleaseState::CollectingPrepared};
    std::optional<PublishedDirectorFrame> publication;
    std::vector<FrameMember> members;
};

class FrameReservation final {
public:
    explicit FrameReservation(FrameSlot& frame) noexcept : frame_(&frame) {
        frame_->reserved = true;
    }

    FrameReservation(const FrameReservation&) = delete;
    FrameReservation& operator=(const FrameReservation&) = delete;

    ~FrameReservation() {
        if (frame_ != nullptr) {
            frame_->reserved = false;
            frame_->member_count = 0U;
            frame_->publication.reset();
        }
    }

    void release() noexcept { frame_ = nullptr; }

private:
    FrameSlot* frame_;
};

[[nodiscard]] bool valid_cursor(const cmd::StreamCursor& cursor) noexcept {
    return static_cast<bool>(cursor.next_command);
}

[[nodiscard]] bool same_logical_node(
    const NodeSessionIdentity& left,
    const NodeSessionIdentity& right) noexcept {
    return left.show_id == right.show_id && left.node_id == right.node_id;
}

[[nodiscard]] bool cursor_can_advance(
    const cmd::StreamCursor& before,
    const cmd::StreamCursor& after) noexcept {
    if (!valid_cursor(before) || !valid_cursor(after)
        || after.through_frame < before.through_frame
        || after.revision < before.revision
        || after.next_command < before.next_command) {
        return false;
    }
    return after.revision.value() - before.revision.value()
        == after.next_command.value() - before.next_command.value();
}

[[nodiscard]] bool valid_frame_transition(
    const cmd::StreamCursor& before,
    const cmd::StreamCursor& after) noexcept {
    return cursor_can_advance(before, after)
        && before.through_frame.value()
            != std::numeric_limits<core::u64>::max()
        && after.through_frame.value() == before.through_frame.value() + 1U;
}

[[nodiscard]] bool cursor_covers(
    const cmd::StreamCursor& watermark,
    const cmd::StreamCursor& target) noexcept {
    if (watermark.through_frame > target.through_frame) {
        return true;
    }
    return watermark.through_frame == target.through_frame
        && watermark == target;
}

[[nodiscard]] bool strictly_advances(
    const cmd::StreamCursor& before,
    const cmd::StreamCursor& after) noexcept {
    return after.through_frame > before.through_frame
        && cursor_can_advance(before, after);
}

void increment_saturated(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}

[[nodiscard]] bool phase_reached(
    DirectorFrameBarrierProgress progress,
    DirectorFrameBarrierPhase phase) noexcept {
    if (progress == DirectorFrameBarrierProgress::EmptyCohort
        || progress == DirectorFrameBarrierProgress::Complete) {
        return true;
    }
    return phase == DirectorFrameBarrierPhase::Prepared
        && progress == DirectorFrameBarrierProgress::AwaitingCommitted;
}

} // namespace

namespace detail {

struct DirectorFrameBarrierControl final {
    bool valid{};
    bool faulted{};
    cmd::StreamCursor tracked_tail{};
    cmd::StreamCursor released_through{};
    DirectorFrameReleaseAuthority* release_authority{};
    std::uint64_t next_frame_ordinal{1U};
    // Numbers the pose stream, and counts the poses a session was handed but
    // never drained before the next one replaced it.
    std::uint64_t tracked_pose_sequence{};
    std::uint64_t tracked_poses_coalesced{};
    std::size_t active_session_count{};
    std::size_t live_session_count{};
    std::size_t catching_up_session_count{};
    std::size_t open_frame_count{};
    DirectorFrameBarrierObserverDiagnostics diagnostics{};
    mutable std::mutex mutex;
    mutable std::condition_variable changed;
    std::vector<SessionSlot> sessions;
    std::vector<FrameSlot> frames;
};

} // namespace detail

namespace {

[[nodiscard]] DirectorFrameBarrierError barrier_error(
    DirectorFrameBarrierErrorCode code) noexcept {
    return DirectorFrameBarrierError{code, std::nullopt};
}

[[nodiscard]] DirectorFrameBarrierError hub_error(
    DirectorSessionHubError error) noexcept {
    return DirectorFrameBarrierError{
        DirectorFrameBarrierErrorCode::HubFailure,
        std::move(error),
    };
}

void reject_registration(
    detail::DirectorFrameBarrierControl& control,
    Id128 session_id,
    DirectorFrameBarrierObserverError error) noexcept {
    control.diagnostics.last_error = error;
    control.diagnostics.last_session_id = session_id;
    increment_saturated(control.diagnostics.rejected_registrations);
}

void reject_acknowledgement(
    detail::DirectorFrameBarrierControl& control,
    Id128 session_id,
    DirectorFrameBarrierObserverError error) noexcept {
    control.diagnostics.last_error = error;
    control.diagnostics.last_session_id = session_id;
    increment_saturated(control.diagnostics.rejected_acknowledgements);
}

[[nodiscard]] SessionSlot* find_session(
    detail::DirectorFrameBarrierControl& control,
    Id128 session_id) noexcept {
    const auto found = std::find_if(
        control.sessions.begin(),
        control.sessions.end(),
        [session_id](const SessionSlot& session) {
            return session.active && session.session_id == session_id;
        });
    return found == control.sessions.end() ? nullptr : &*found;
}

[[nodiscard]] FrameSlot* find_frame(
    detail::DirectorFrameBarrierControl& control,
    DirectorFrameBarrierId id) noexcept {
    if (id.slot >= control.frames.size()) {
        return nullptr;
    }
    auto& frame = control.frames[id.slot];
    return frame.active && frame.generation == id.generation
            && frame.ordinal == id.ordinal
        ? &frame
        : nullptr;
}

[[nodiscard]] const FrameSlot* find_frame(
    const detail::DirectorFrameBarrierControl& control,
    DirectorFrameBarrierId id) noexcept {
    if (id.slot >= control.frames.size()) {
        return nullptr;
    }
    const auto& frame = control.frames[id.slot];
    return frame.active && frame.generation == id.generation
            && frame.ordinal == id.ordinal
        ? &frame
        : nullptr;
}

[[nodiscard]] FrameSlot* find_frame_after(
    detail::DirectorFrameBarrierControl& control,
    const cmd::StreamCursor& after) noexcept {
    const auto found = std::find_if(
        control.frames.begin(),
        control.frames.end(),
        [&after](const FrameSlot& frame) {
            return frame.active && frame.after == after;
        });
    return found == control.frames.end() ? nullptr : &*found;
}

[[nodiscard]] std::size_t permit_index(
    const SessionSlot& session,
    std::size_t offset) noexcept {
    const auto capacity = session.permits.size();
    const auto unwrapped = session.permit_head + offset;
    return unwrapped < capacity ? unwrapped : unwrapped - capacity;
}

[[nodiscard]] bool append_permit(
    SessionSlot& session,
    const FrameCommitPermitIntent& intent,
    PendingPermitState state) noexcept {
    if (session.permits.empty() || session.permit_size == session.permits.size()) {
        return false;
    }
    const auto index = permit_index(session, session.permit_size);
    session.permits[index] = PendingPermit{intent, state};
    ++session.permit_size;
    return true;
}

[[nodiscard]] PendingPermit* pending_permit(
    SessionSlot& session,
    const FrameAcknowledgementReport& acknowledgement) noexcept {
    for (std::size_t offset = 0U; offset < session.permit_size; ++offset) {
        auto& permit = session.permits[permit_index(session, offset)];
        if (permit.intent.acknowledged_sequence
                == acknowledgement.acknowledged_sequence
            && permit.intent.before == acknowledgement.before
            && permit.intent.after == acknowledgement.after) {
            return &permit;
        }
    }
    return nullptr;
}

void pop_permit(SessionSlot& session) noexcept {
    if (session.permit_size == 0U) {
        return;
    }
    ++session.permit_head;
    if (session.permit_head == session.permits.size()) {
        session.permit_head = 0U;
    }
    --session.permit_size;
}

[[nodiscard]] bool exact_quorum_prepared(const FrameSlot& frame) noexcept {
    return std::all_of(
        frame.members.begin(),
        frame.members.begin()
            + static_cast<std::ptrdiff_t>(frame.member_count),
        [](const FrameMember& member) {
            return member.state == MemberState::Prepared
                || member.state == MemberState::Committed;
        });
}

void make_matching_permits_eligible(
    detail::DirectorFrameBarrierControl& control,
    const FrameSlot& frame) noexcept {
    for (auto& session : control.sessions) {
        if (!session.active) {
            continue;
        }
        for (std::size_t offset = 0U; offset < session.permit_size; ++offset) {
            auto& permit = session.permits[permit_index(session, offset)];
            if (permit.state == PendingPermitState::Held
                && permit.intent.barrier_ordinal == frame.ordinal
                && permit.intent.before == frame.before
                && permit.intent.after == frame.after) {
                permit.state = PendingPermitState::Eligible;
            }
        }
    }
}

[[nodiscard]] bool advance_ready_releases(
    detail::DirectorFrameBarrierControl& control,
    std::unique_lock<std::mutex>& lock) noexcept {
    if (control.release_authority == nullptr) {
        return true;
    }

    for (;;) {
        auto candidate = std::find_if(
            control.frames.begin(),
            control.frames.end(),
            [&control](const FrameSlot& frame) {
                return frame.active && frame.before == control.released_through;
            });
        if (candidate == control.frames.end()) {
            return true;
        }
        if (candidate->release_state == ReleaseState::CollectingPrepared
            && exact_quorum_prepared(*candidate)) {
            candidate->release_state = ReleaseState::QuorumPrepared;
        }
        if (candidate->release_state == ReleaseState::Released) {
            control.released_through = candidate->after;
            make_matching_permits_eligible(control, *candidate);
            continue;
        }
        if (candidate->release_state != ReleaseState::QuorumPrepared
            || !candidate->publication) {
            return true;
        }

        candidate->release_state = ReleaseState::ReleasePersisting;
        const auto slot = static_cast<std::size_t>(
            candidate - control.frames.begin());
        const auto generation = candidate->generation;
        const auto ordinal = candidate->ordinal;
        const auto publication = *candidate->publication;
        auto* const authority = control.release_authority;

        lock.unlock();
        const auto persisted = authority->persist_release(publication, ordinal);
        lock.lock();

        auto& current = control.frames[slot];
        if (!current.active || current.generation != generation
            || current.ordinal != ordinal
            || current.release_state != ReleaseState::ReleasePersisting) {
            if (current.active && current.generation == generation
                && current.ordinal == ordinal) {
                current.release_state = ReleaseState::DegradedBeforeRelease;
                current.tracking_fault = true;
            }
            control.faulted = true;
            control.diagnostics.last_error =
                DirectorFrameBarrierObserverError::ReleaseFailed;
            control.changed.notify_all();
            return false;
        }
        if (!persisted) {
            current.release_state = ReleaseState::DegradedBeforeRelease;
            current.tracking_fault = true;
            control.faulted = true;
            control.diagnostics.last_error =
                DirectorFrameBarrierObserverError::ReleaseFailed;
            return false;
        }

        current.release_state = ReleaseState::Released;
        control.released_through = current.after;
        make_matching_permits_eligible(control, current);
        control.changed.notify_all();
    }
}

[[nodiscard]] DirectorFrameBarrierSummary summarize(
    const FrameSlot& frame,
    std::size_t slot) noexcept {
    DirectorFrameBarrierSummary summary{};
    summary.id = DirectorFrameBarrierId{
        slot,
        frame.generation,
        frame.ordinal,
    };
    summary.before = frame.before;
    summary.after = frame.after;
    summary.members = frame.member_count;
    summary.tracking_fault = frame.tracking_fault;

    for (std::size_t index = 0U; index < frame.member_count; ++index) {
        switch (frame.members[index].state) {
        case MemberState::AwaitingPrepared:
            break;
        case MemberState::Prepared:
            ++summary.prepared;
            break;
        case MemberState::Committed:
            ++summary.prepared;
            ++summary.committed;
            break;
        case MemberState::Lost:
            ++summary.lost;
            break;
        }
    }

    if (summary.members == 0U) {
        summary.progress = DirectorFrameBarrierProgress::EmptyCohort;
    } else if (summary.committed + summary.lost == summary.members) {
        summary.progress = DirectorFrameBarrierProgress::Complete;
    } else if (summary.prepared + summary.lost == summary.members) {
        summary.progress = DirectorFrameBarrierProgress::AwaitingCommitted;
    } else {
        summary.progress = DirectorFrameBarrierProgress::AwaitingPrepared;
    }
    return summary;
}

[[nodiscard]] bool acknowledgement_cursor_is_known(
    const detail::DirectorFrameBarrierControl& control,
    const cmd::StreamCursor& cursor) noexcept {
    if (!valid_cursor(cursor) || !static_cast<bool>(cursor.through_frame)
        || cursor.through_frame > control.tracked_tail.through_frame
        || !cursor_can_advance(cursor, control.tracked_tail)) {
        return false;
    }
    if (cursor.through_frame == control.tracked_tail.through_frame) {
        return cursor == control.tracked_tail;
    }
    for (const auto& frame : control.frames) {
        if (frame.active && frame.after.through_frame == cursor.through_frame) {
            return frame.after == cursor;
        }
    }
    // A validated session may still be committing replay/catch-up frames that
    // predate this barrier or an explicitly retired diagnostic record.
    return true;
}

void promote_to_live(
    detail::DirectorFrameBarrierControl& control,
    SessionSlot& session) noexcept {
    session.readiness = SessionReadiness::Live;
    --control.catching_up_session_count;
    ++control.live_session_count;
    control.diagnostics.live_sessions = control.live_session_count;
    control.diagnostics.catching_up_sessions =
        control.catching_up_session_count;
}

} // namespace

DirectorFrameBarrier::DirectorFrameBarrier(
    cmd::StreamCursor initial_tail,
    DirectorFrameBarrierLimits limits)
    : limits_(limits),
      control_(std::make_unique<detail::DirectorFrameBarrierControl>()) {
    control_->valid = limits.max_sessions != 0U
        && limits.max_open_frames != 0U
        && limits.max_pending_per_session != 0U && valid_cursor(initial_tail)
        && initial_tail.through_frame.value()
            != std::numeric_limits<core::u64>::max();
    control_->tracked_tail = initial_tail;
    control_->released_through = initial_tail;
    control_->next_frame_ordinal = initial_tail.through_frame.value() + 1U;
    if (!control_->valid) {
        return;
    }

    control_->sessions.resize(limits.max_sessions);
    for (auto& session : control_->sessions) {
        session.permits.resize(limits.max_pending_per_session);
    }
    control_->frames.resize(limits.max_open_frames);
    for (auto& frame : control_->frames) {
        frame.members.resize(limits.max_sessions);
    }
}

DirectorFrameBarrier::DirectorFrameBarrier(
    cmd::StreamCursor initial_tail,
    DirectorFrameReleaseAuthority& release_authority,
    DirectorFrameBarrierLimits limits)
    : DirectorFrameBarrier(initial_tail, limits) {
    control_->release_authority = &release_authority;
    const auto next_ordinal = release_authority.next_release_ordinal();
    if (next_ordinal == 0U) {
        control_->valid = false;
    } else {
        control_->next_frame_ordinal = next_ordinal;
    }
}

DirectorFrameBarrier::~DirectorFrameBarrier() = default;

bool DirectorFrameBarrier::register_live(
    const DirectorLiveRegistration& registration) noexcept {
    auto& control = *control_;
    const std::lock_guard lock{control.mutex};

    if (!control.valid || control.faulted) {
        reject_registration(
            control,
            registration.session_id,
            control.faulted
                ? DirectorFrameBarrierObserverError::BarrierFaulted
                : DirectorFrameBarrierObserverError::InvalidRegistration);
        return false;
    }
    if (!static_cast<bool>(registration.session_id)
        || !static_cast<bool>(registration.peer.show_id)
        || !static_cast<bool>(registration.peer.node_id)
        || !static_cast<bool>(registration.peer.incarnation)
        || registration.node_index > max_session_node_index
        || registration.last_node_control_sequence == 0U
        || !valid_cursor(registration.committed_cursor)) {
        reject_registration(
            control,
            registration.session_id,
            DirectorFrameBarrierObserverError::InvalidRegistration);
        return false;
    }
    const auto& admission_tail = control.release_authority == nullptr
        ? control.tracked_tail
        : control.released_through;
    if (registration.committed_cursor.through_frame
        > admission_tail.through_frame) {
        reject_registration(
            control,
            registration.session_id,
            DirectorFrameBarrierObserverError::CursorAhead);
        return false;
    }
    if (registration.committed_cursor.through_frame
            == admission_tail.through_frame
        && registration.committed_cursor != admission_tail) {
        reject_registration(
            control,
            registration.session_id,
            DirectorFrameBarrierObserverError::CursorMismatch);
        return false;
    }
    if (!cursor_can_advance(
            registration.committed_cursor, admission_tail)) {
        reject_registration(
            control,
            registration.session_id,
            DirectorFrameBarrierObserverError::CursorMismatch);
        return false;
    }
    for (const auto& session : control.sessions) {
        if (!session.active) {
            continue;
        }
        if (session.session_id == registration.session_id) {
            reject_registration(
                control,
                registration.session_id,
                DirectorFrameBarrierObserverError::DuplicateSession);
            return false;
        }
        if (same_logical_node(session.peer, registration.peer)) {
            reject_registration(
                control,
                registration.session_id,
                DirectorFrameBarrierObserverError::DuplicateNode);
            return false;
        }
        if (session.node_index == registration.node_index) {
            reject_registration(
                control,
                registration.session_id,
                DirectorFrameBarrierObserverError::DuplicateNodeIndex);
            return false;
        }
    }
    if (control.active_session_count == limits_.max_sessions) {
        reject_registration(
            control,
            registration.session_id,
            DirectorFrameBarrierObserverError::SessionCapacity);
        return false;
    }

    auto available = std::find_if(
        control.sessions.begin(),
        control.sessions.end(),
        [](const SessionSlot& session) {
            return !session.active
                && session.generation
                    != std::numeric_limits<std::uint64_t>::max();
        });
    if (available == control.sessions.end()) {
        reject_registration(
            control,
            registration.session_id,
            DirectorFrameBarrierObserverError::SessionCapacity);
        return false;
    }

    ++available->generation;
    available->active = true;
    available->session_id = registration.session_id;
    available->peer = registration.peer;
    available->node_index = registration.node_index;
    available->asset_adoption = {};
    available->output_sync = {};
    available->tracking = {};
    available->renderer = {};
    available->prepared_cursor = registration.committed_cursor;
    available->committed_cursor = registration.committed_cursor;
    available->has_acknowledgement_sequence = true;
    available->last_acknowledgement_sequence =
        registration.last_node_control_sequence;
    available->permit_head = 0U;
    available->permit_size = 0U;
    available->readiness = registration.committed_cursor == control.tracked_tail
        ? SessionReadiness::Live
        : SessionReadiness::CatchingUp;
    ++control.active_session_count;
    if (available->readiness == SessionReadiness::Live) {
        ++control.live_session_count;
    } else {
        ++control.catching_up_session_count;
    }
    control.diagnostics.live_sessions = control.live_session_count;
    control.diagnostics.catching_up_sessions =
        control.catching_up_session_count;
    return true;
}

bool DirectorFrameBarrier::on_ack(
    Id128 session_id,
    const FrameAcknowledgementReport& acknowledgement) noexcept {
    auto& control = *control_;
    std::unique_lock lock{control.mutex};

    if (!control.valid || control.faulted) {
        reject_acknowledgement(
            control,
            session_id,
            DirectorFrameBarrierObserverError::BarrierFaulted);
        return false;
    }
    auto* session = find_session(control, session_id);
    if (session == nullptr) {
        reject_acknowledgement(
            control,
            session_id,
            DirectorFrameBarrierObserverError::UnknownSession);
        return false;
    }
    if (acknowledgement.sequence == 0U
        || acknowledgement.acknowledged_sequence == 0U
        || !valid_frame_transition(
            acknowledgement.before, acknowledgement.after)
        || !acknowledgement_cursor_is_known(control, acknowledgement.after)) {
        reject_acknowledgement(
            control,
            session_id,
            DirectorFrameBarrierObserverError::InvalidAcknowledgement);
        return false;
    }
    if (session->has_acknowledgement_sequence
        && (session->last_acknowledgement_sequence
                == std::numeric_limits<std::uint64_t>::max()
            || acknowledgement.sequence
                != session->last_acknowledgement_sequence + 1U)) {
        reject_acknowledgement(
            control,
            session_id,
            DirectorFrameBarrierObserverError::AcknowledgementSequenceMismatch);
        return false;
    }

    auto* frame = find_frame_after(control, acknowledgement.after);
    if (frame != nullptr && frame->before != acknowledgement.before) {
        reject_acknowledgement(
            control,
            session_id,
            DirectorFrameBarrierObserverError::AcknowledgementCursorMismatch);
        return false;
    }

    const auto session_slot = static_cast<std::size_t>(
        session - control.sessions.data());
    const DirectorFrameBarrierSessionId member_id{
        session_slot,
        session->generation,
    };
    FrameMember* exact_member = nullptr;
    if (frame != nullptr) {
        const auto found = std::find_if(
            frame->members.begin(),
            frame->members.begin()
                + static_cast<std::ptrdiff_t>(frame->member_count),
            [member_id](const FrameMember& member) {
                return member.id == member_id;
            });
        if (found != frame->members.begin()
                + static_cast<std::ptrdiff_t>(frame->member_count)) {
            exact_member = &*found;
        }
    }

    switch (acknowledgement.kind) {
    case FrameAcknowledgementKind::Prepared:
        if (session->prepared_cursor != acknowledgement.before
            || !strictly_advances(
                session->prepared_cursor, acknowledgement.after)) {
            reject_acknowledgement(
                control,
                session_id,
                DirectorFrameBarrierObserverError::AcknowledgementOutOfOrder);
            return false;
        }
        if (exact_member != nullptr
            && exact_member->state != MemberState::AwaitingPrepared) {
            reject_acknowledgement(
                control,
                session_id,
                DirectorFrameBarrierObserverError::AcknowledgementOutOfOrder);
            return false;
        }
        {
            core::u64 ordinal{};
            if (frame != nullptr) {
                ordinal = frame->ordinal;
            } else if (
                control.release_authority != nullptr
                && cursor_covers(
                    control.released_through, acknowledgement.after)) {
                const auto historical =
                    control.release_authority->release_ordinal(
                        acknowledgement.before, acknowledgement.after);
                if (historical) {
                    ordinal = *historical;
                }
            } else if (control.release_authority == nullptr) {
                ordinal = acknowledgement.after.through_frame.value();
            }
            if (ordinal == 0U) {
                reject_acknowledgement(
                    control,
                    session_id,
                    DirectorFrameBarrierObserverError::AcknowledgementCursorMismatch);
                return false;
            }
            const auto permit_state = control.release_authority != nullptr
                    && cursor_covers(
                        control.released_through, acknowledgement.after)
                ? PendingPermitState::Eligible
                : PendingPermitState::Held;
            if (!append_permit(
                    *session,
                    FrameCommitPermitIntent{
                        ordinal,
                        acknowledgement.acknowledged_sequence,
                        acknowledgement.before,
                        acknowledgement.after,
                    },
                    permit_state)) {
                reject_acknowledgement(
                    control,
                    session_id,
                    DirectorFrameBarrierObserverError::PermitCapacity);
                return false;
            }
        }
        if (exact_member != nullptr) {
            exact_member->state = MemberState::Prepared;
            exact_member->acknowledged_sequence =
                acknowledgement.acknowledged_sequence;
            if (frame->release_state == ReleaseState::CollectingPrepared
                && exact_quorum_prepared(*frame)) {
                // Record this cohort's quorum independently of whether an
                // earlier ordinal currently blocks durable release. A later
                // disconnect must not erase a quorum that was exact when the
                // final Prepared acknowledgement arrived.
                frame->release_state = ReleaseState::QuorumPrepared;
            }
        }
        session->prepared_cursor = acknowledgement.after;
        break;
    case FrameAcknowledgementKind::Committed:
        if (session->committed_cursor != acknowledgement.before
            || !strictly_advances(
                session->committed_cursor, acknowledgement.after)
            || !cursor_covers(
                session->prepared_cursor, acknowledgement.after)) {
            reject_acknowledgement(
                control,
                session_id,
                DirectorFrameBarrierObserverError::AcknowledgementOutOfOrder);
            return false;
        }
        if (exact_member != nullptr
            && (exact_member->state != MemberState::Prepared
                || exact_member->acknowledged_sequence
                    != acknowledgement.acknowledged_sequence)) {
            reject_acknowledgement(
                control,
                session_id,
                DirectorFrameBarrierObserverError::AcknowledgementOutOfOrder);
            return false;
        }
        {
            auto* permit = pending_permit(*session, acknowledgement);
            const auto* head = session->permit_size == 0U
                ? nullptr
                : &session->permits[session->permit_head];
            if (permit == nullptr || permit != head
                || (control.release_authority != nullptr
                    && permit->state != PendingPermitState::Claimed)) {
                reject_acknowledgement(
                    control,
                    session_id,
                    DirectorFrameBarrierObserverError::PermitMismatch);
                return false;
            }
            pop_permit(*session);
        }
        if (exact_member != nullptr) {
            exact_member->state = MemberState::Committed;
        }
        session->committed_cursor = acknowledgement.after;
        break;
    default:
        reject_acknowledgement(
            control,
            session_id,
            DirectorFrameBarrierObserverError::InvalidAcknowledgement);
        return false;
    }

    session->has_acknowledgement_sequence = true;
    session->last_acknowledgement_sequence = acknowledgement.sequence;

    if (acknowledgement.kind == FrameAcknowledgementKind::Prepared
        && !advance_ready_releases(control, lock)) {
        reject_acknowledgement(
            control,
            session_id,
            DirectorFrameBarrierObserverError::ReleaseFailed);
        control.changed.notify_all();
        return false;
    }

    if (session->readiness == SessionReadiness::CatchingUp
        && session->committed_cursor == control.tracked_tail) {
        promote_to_live(control, *session);
    }
    control.changed.notify_all();
    return true;
}

bool DirectorFrameBarrier::on_heartbeat_ack(
    Id128 session_id,
    const HeartbeatAcknowledgementReport& acknowledgement) noexcept {
    auto& control = *control_;
    const std::lock_guard lock{control.mutex};
    if (!control.valid || control.faulted) {
        reject_acknowledgement(
            control,
            session_id,
            DirectorFrameBarrierObserverError::BarrierFaulted);
        return false;
    }
    auto* session = find_session(control, session_id);
    if (session == nullptr) {
        reject_acknowledgement(
            control,
            session_id,
            DirectorFrameBarrierObserverError::UnknownSession);
        return false;
    }
    if (acknowledgement.sequence == 0U
        || acknowledgement.acknowledged_sequence == 0U
        || !valid_cursor(acknowledgement.visible_through)) {
        reject_acknowledgement(
            control,
            session_id,
            DirectorFrameBarrierObserverError::InvalidAcknowledgement);
        return false;
    }
    if (session->has_acknowledgement_sequence
        && (session->last_acknowledgement_sequence
                == std::numeric_limits<std::uint64_t>::max()
            || acknowledgement.sequence
                != session->last_acknowledgement_sequence + 1U)) {
        reject_acknowledgement(
            control,
            session_id,
            DirectorFrameBarrierObserverError::AcknowledgementSequenceMismatch);
        return false;
    }
    if (acknowledgement.visible_through != session->committed_cursor) {
        reject_acknowledgement(
            control,
            session_id,
            DirectorFrameBarrierObserverError::AcknowledgementCursorMismatch);
        return false;
    }
    try {
        session->asset_adoption = acknowledgement.asset_adoption;
    } catch (const std::bad_alloc&) {
        reject_acknowledgement(
            control, session_id,
            DirectorFrameBarrierObserverError::InvalidAcknowledgement);
        return false;
    }
    session->has_acknowledgement_sequence = true;
    session->last_acknowledgement_sequence = acknowledgement.sequence;
    session->output_sync = acknowledgement.output_sync;
    session->renderer = acknowledgement.renderer;
    session->tracking = acknowledgement.tracking;
    return true;
}

void DirectorFrameBarrier::unregister(
    Id128 session_id,
    DirectorSessionLeaveReason reason) noexcept {
    auto& control = *control_;
    const std::lock_guard lock{control.mutex};
    auto* session = find_session(control, session_id);
    if (session == nullptr) {
        increment_saturated(control.diagnostics.ignored_unregistrations);
        return;
    }

    const auto session_slot = static_cast<std::size_t>(session - control.sessions.data());
    const DirectorFrameBarrierSessionId member_id{
        session_slot,
        session->generation,
    };
    for (auto& frame : control.frames) {
        if (!frame.active) {
            continue;
        }
        for (std::size_t index = 0U; index < frame.member_count; ++index) {
            auto& member = frame.members[index];
            if (member.id != member_id || member.state == MemberState::Lost
                || member.state == MemberState::Committed) {
                continue;
            }
            member.was_prepared = member.state == MemberState::Prepared;
            member.state = MemberState::Lost;
            member.leave_reason = reason;
            if (frame.release_state == ReleaseState::CollectingPrepared) {
                frame.release_state = ReleaseState::DegradedBeforeRelease;
            }
        }
    }

    if (session->readiness == SessionReadiness::Live) {
        --control.live_session_count;
    } else {
        --control.catching_up_session_count;
    }
    --control.active_session_count;
    session->active = false;
    session->asset_adoption = {};
    session->output_sync = {};
    session->tracking = {};
    session->renderer = {};
    session->permit_head = 0U;
    session->permit_size = 0U;
    control.diagnostics.live_sessions = control.live_session_count;
    control.diagnostics.catching_up_sessions =
        control.catching_up_session_count;
    control.changed.notify_all();
}

bool DirectorFrameBarrier::coordinates_commit_permits() const noexcept {
    const auto& control = *control_;
    const std::lock_guard lock{control.mutex};
    return control.valid && !control.faulted
        && control.release_authority != nullptr;
}

DirectorSessionObserver::CommitPermitPoll
DirectorFrameBarrier::try_take_commit_permit(
    Id128 session_id,
    FrameCommitPermitIntent& output) noexcept {
    auto& control = *control_;
    const std::lock_guard lock{control.mutex};
    if (!control.valid || control.faulted
        || control.release_authority == nullptr) {
        return CommitPermitPoll::Rejected;
    }
    auto* session = find_session(control, session_id);
    if (session == nullptr) {
        return CommitPermitPoll::Rejected;
    }

    for (std::size_t offset = 0U; offset < session->permit_size; ++offset) {
        auto& permit = session->permits[permit_index(*session, offset)];
        if (permit.state == PendingPermitState::Claimed) {
            continue;
        }
        if (permit.state == PendingPermitState::Held) {
            return CommitPermitPoll::Empty;
        }
        permit.state = PendingPermitState::Claimed;
        output = permit.intent;
        return CommitPermitPoll::Ready;
    }
    return CommitPermitPoll::Empty;
}

DirectorSessionObserver::TrackedPosePoll
DirectorFrameBarrier::try_take_tracked_pose(
    Id128 session_id, TrackedCameraFrame& output) noexcept {
    auto& control = *control_;
    const std::lock_guard lock{control.mutex};
    if (!control.valid || control.faulted) {
        return TrackedPosePoll::Empty;
    }
    auto* session = find_session(control, session_id);
    if (session == nullptr || !session->has_pose) {
        return TrackedPosePoll::Empty;
    }
    // Taken, not peeked: a pose is sent once. If the send fails the session is
    // ending anyway, and the next pose is a frame away.
    session->has_pose = false;
    output = session->pose;
    return TrackedPosePoll::Ready;
}

core::u64 DirectorFrameBarrier::tracked_poses_coalesced() const noexcept {
    auto& control = *control_;
    const std::lock_guard lock{control.mutex};
    return control.tracked_poses_coalesced;
}

std::size_t DirectorFrameBarrier::publish_tracked_pose(
    const TrackedCameraFrame& frame) noexcept {
    auto& control = *control_;
    const std::lock_guard lock{control.mutex};
    if (!control.valid || control.faulted || frame.samples.empty()
        || frame.samples.size() > max_tracked_camera_samples) {
        return 0U;
    }
    for (const TrackedCameraSample& sample : frame.samples) {
        if (!valid(sample)) {
            return 0U;
        }
    }
    // One ordinal per published pose, shared by every session so the whole
    // cohort numbers the same stream. Assigned here rather than at send time,
    // which is the entire point: a pose overwritten before it was sent still
    // consumed an ordinal, so the gap survives to the node that missed it.
    ++control.tracked_pose_sequence;
    TrackedCameraFrame numbered = frame;
    numbered.pose_sequence = control.tracked_pose_sequence;

    std::size_t published = 0U;
    for (auto& session : control.sessions) {
        if (!session.active) {
            continue;
        }
        // Overwrites whatever was pending. See the note on the slot: the
        // newest frame of poses is the only one worth sending, and a node that
        // missed one holds what it has rather than being handed a stale stack.
        //
        // Counted, though. Coalescing is a legitimate answer to a session that
        // fell behind, but it is not free: that node's plate is a sample older
        // than its neighbour's until the next pose lands, and an operator
        // watching one wall lag the other deserves to be told why rather than
        // left to find it on camera.
        if (session.has_pose) {
            ++control.tracked_poses_coalesced;
        }
        session.has_pose = true;
        session.pose = numbered;
        ++published;
    }
    return published;
}

core::Result<DirectorTrackedSealOutcome, DirectorFrameBarrierError>
DirectorFrameBarrier::seal_and_track(
    DirectorSessionHub& hub,
    cmd::FrameNumber frame_number) {
    using Result = core::Result<
        DirectorTrackedSealOutcome,
        DirectorFrameBarrierError>;
    auto& control = *control_;
    std::unique_lock lock{control.mutex};

    if (!control.valid) {
        return Result::failure(
            barrier_error(DirectorFrameBarrierErrorCode::InvalidConfiguration));
    }
    if (control.faulted) {
        return Result::failure(
            barrier_error(DirectorFrameBarrierErrorCode::BarrierFaulted));
    }
    if (control.open_frame_count == limits_.max_open_frames) {
        return Result::failure(
            barrier_error(DirectorFrameBarrierErrorCode::FrameCapacity));
    }
    if (control.next_frame_ordinal
        == std::numeric_limits<std::uint64_t>::max()) {
        return Result::failure(barrier_error(
            DirectorFrameBarrierErrorCode::FrameGenerationExhausted));
    }

    auto available = std::find_if(
        control.frames.begin(),
        control.frames.end(),
        [](const FrameSlot& frame) {
            return !frame.active && !frame.reserved
                && frame.generation
                    != std::numeric_limits<std::uint64_t>::max();
        });
    if (available == control.frames.end()) {
        return Result::failure(barrier_error(
            DirectorFrameBarrierErrorCode::FrameGenerationExhausted));
    }

    FrameReservation reservation{*available};
    available->tracking_fault = false;
    available->member_count = 0U;
    available->release_state = ReleaseState::CollectingPrepared;
    available->publication.reset();
    // The cut is where a session that has received everything but the
    // in-flight tail stops catching up. Its earlier frames keep the
    // historical permits they were already queued for; this frame and every
    // later one wait for its Prepared like any other member's.
    for (auto& session : control.sessions) {
        if (session.active && session.readiness == SessionReadiness::CatchingUp
            && control.tracked_tail.through_frame.value()
                    - session.prepared_cursor.through_frame.value()
                <= limits_.max_join_prepared_lag) {
            promote_to_live(control, session);
        }
    }
    for (std::size_t index = 0U; index < control.sessions.size(); ++index) {
        const auto& session = control.sessions[index];
        if (!session.active || session.readiness != SessionReadiness::Live) {
            continue;
        }
        auto& member = available->members[available->member_count];
        member = FrameMember{
            DirectorFrameBarrierSessionId{index, session.generation},
            session.session_id,
            session.peer,
            MemberState::AwaitingPrepared,
            DirectorSessionLeaveReason::SessionFailure,
            false,
        };
        ++available->member_count;
    }

    auto sealed = hub.seal_and_publish(frame_number);
    if (!sealed) {
        return Result::failure(hub_error(std::move(sealed).error()));
    }

    // Every operation below is assignment into storage fixed in the
    // constructor. A successful durable outcome can no longer be hidden by a
    // barrier error.
    const auto slot = static_cast<std::size_t>(available - control.frames.begin());
    ++available->generation;
    available->ordinal = control.next_frame_ordinal;
    ++control.next_frame_ordinal;
    available->before = sealed.value().frame.before_cursor();
    available->after = sealed.value().frame.after_cursor();
    available->tracking_fault =
        available->before != control.tracked_tail
        || !valid_frame_transition(available->before, available->after)
        || available->after.through_frame != frame_number;
    available->active = true;
    available->reserved = false;
    available->publication = sealed.value().frame;
    if (available->member_count == 0U) {
        available->release_state = ReleaseState::QuorumPrepared;
    }
    reservation.release();
    ++control.open_frame_count;
    control.tracked_tail = available->after;

    DirectorFrameBarrierTrackingStatus tracking_status{
        DirectorFrameBarrierTrackingStatus::Tracked};
    if (available->tracking_fault) {
        control.faulted = true;
        tracking_status = DirectorFrameBarrierTrackingStatus::InvariantFault;
    } else if (!advance_ready_releases(control, lock)) {
        tracking_status = DirectorFrameBarrierTrackingStatus::InvariantFault;
    }
    const DirectorFrameBarrierId id{
        slot,
        available->generation,
        available->ordinal,
    };
    control.changed.notify_all();
    return Result::success(DirectorTrackedSealOutcome{
        std::move(sealed).value(),
        id,
        tracking_status,
    });
}

core::Result<DirectorFrameBarrierSummary, DirectorFrameBarrierError>
DirectorFrameBarrier::snapshot(DirectorFrameBarrierId frame_id) const noexcept {
    using Result = core::Result<
        DirectorFrameBarrierSummary,
        DirectorFrameBarrierError>;
    const auto& control = *control_;
    const std::lock_guard lock{control.mutex};
    const auto* frame = find_frame(control, frame_id);
    if (frame == nullptr) {
        return Result::failure(
            barrier_error(DirectorFrameBarrierErrorCode::StaleFrame));
    }
    return Result::success(summarize(*frame, frame_id.slot));
}

core::Result<DirectorFrameBarrierWaitReport, DirectorFrameBarrierError>
DirectorFrameBarrier::wait_for(
    DirectorFrameBarrierId frame_id,
    DirectorFrameBarrierPhase phase,
    std::chrono::milliseconds timeout) const noexcept {
    using Result = core::Result<
        DirectorFrameBarrierWaitReport,
        DirectorFrameBarrierError>;
    auto& control = *control_;
    std::unique_lock lock{control.mutex};
    const auto bounded_timeout = std::max(timeout, std::chrono::milliseconds{0});
    const auto deadline = std::chrono::steady_clock::now() + bounded_timeout;

    for (;;) {
        const auto* frame = find_frame(control, frame_id);
        if (frame == nullptr) {
            return Result::failure(
                barrier_error(DirectorFrameBarrierErrorCode::StaleFrame));
        }
        auto summary = summarize(*frame, frame_id.slot);
        if (summary.degraded()) {
            return Result::success(DirectorFrameBarrierWaitReport{
                DirectorFrameBarrierWaitStatus::Degraded,
                std::move(summary),
            });
        }
        if (phase_reached(summary.progress, phase)) {
            return Result::success(DirectorFrameBarrierWaitReport{
                DirectorFrameBarrierWaitStatus::Reached,
                std::move(summary),
            });
        }
        if (bounded_timeout.count() == 0
            || control.changed.wait_until(lock, deadline)
                == std::cv_status::timeout) {
            const auto* current = find_frame(control, frame_id);
            if (current == nullptr) {
                return Result::failure(
                    barrier_error(DirectorFrameBarrierErrorCode::StaleFrame));
            }
            summary = summarize(*current, frame_id.slot);
            return Result::success(DirectorFrameBarrierWaitReport{
                summary.degraded()
                    ? DirectorFrameBarrierWaitStatus::Degraded
                    : phase_reached(summary.progress, phase)
                    ? DirectorFrameBarrierWaitStatus::Reached
                    : DirectorFrameBarrierWaitStatus::TimedOut,
                std::move(summary),
            });
        }
    }
}

core::Result<DirectorFrameBarrierLostSessionsReport, DirectorFrameBarrierError>
DirectorFrameBarrier::copy_lost_sessions(
    DirectorFrameBarrierId frame_id,
    std::span<DirectorFrameBarrierLostSession> output) const noexcept {
    using Result = core::Result<
        DirectorFrameBarrierLostSessionsReport,
        DirectorFrameBarrierError>;
    const auto& control = *control_;
    const std::lock_guard lock{control.mutex};
    const auto* frame = find_frame(control, frame_id);
    if (frame == nullptr) {
        return Result::failure(
            barrier_error(DirectorFrameBarrierErrorCode::StaleFrame));
    }

    DirectorFrameBarrierLostSessionsReport report{};
    for (std::size_t index = 0U; index < frame->member_count; ++index) {
        const auto& member = frame->members[index];
        if (member.state != MemberState::Lost) {
            continue;
        }
        if (report.copied < output.size()) {
            output[report.copied] = DirectorFrameBarrierLostSession{
                member.id,
                member.session_id,
                member.peer,
                member.leave_reason,
                member.was_prepared,
            };
            ++report.copied;
        }
        ++report.total;
    }
    return Result::success(report);
}

core::Result<DirectorFrameBarrierSummary, DirectorFrameBarrierError>
DirectorFrameBarrier::retire(DirectorFrameBarrierId frame_id) noexcept {
    using Result = core::Result<
        DirectorFrameBarrierSummary,
        DirectorFrameBarrierError>;
    auto& control = *control_;
    const std::lock_guard lock{control.mutex};
    auto* frame = find_frame(control, frame_id);
    if (frame == nullptr) {
        return Result::failure(
            barrier_error(DirectorFrameBarrierErrorCode::StaleFrame));
    }
    auto summary = summarize(*frame, frame_id.slot);
    if (summary.progress != DirectorFrameBarrierProgress::Complete
        && summary.progress != DirectorFrameBarrierProgress::EmptyCohort) {
        return Result::failure(
            barrier_error(DirectorFrameBarrierErrorCode::FrameIncomplete));
    }
    if (control.release_authority != nullptr
        && frame->release_state != ReleaseState::Released) {
        return Result::failure(
            barrier_error(DirectorFrameBarrierErrorCode::FrameIncomplete));
    }
    frame->active = false;
    frame->member_count = 0U;
    frame->publication.reset();
    --control.open_frame_count;
    control.changed.notify_all();
    return Result::success(std::move(summary));
}

cmd::StreamCursor DirectorFrameBarrier::tracked_tail() const noexcept {
    const auto& control = *control_;
    const std::lock_guard lock{control.mutex};
    return control.tracked_tail;
}

cmd::StreamCursor DirectorFrameBarrier::released_through() const noexcept {
    const auto& control = *control_;
    const std::lock_guard lock{control.mutex};
    return control.released_through;
}

bool DirectorFrameBarrier::heartbeat_released_through(
    cmd::StreamCursor& output) const noexcept {
    const auto& control = *control_;
    const std::lock_guard lock{control.mutex};
    if (!control.valid || control.faulted
        || control.release_authority == nullptr) {
        return false;
    }
    output = control.released_through;
    return true;
}

std::size_t DirectorFrameBarrier::active_sessions() const noexcept {
    const auto& control = *control_;
    const std::lock_guard lock{control.mutex};
    return control.active_session_count;
}

DirectorNodeConnection DirectorFrameBarrier::node_connection(
    core::u32 node_index) const noexcept {
    const auto& control = *control_;
    const std::lock_guard lock{control.mutex};
    DirectorNodeConnection result{};
    result.target_frame = control.tracked_tail.through_frame.value();
    if (!control.valid || control.faulted) return result;
    const auto session = std::find_if(
        control.sessions.begin(), control.sessions.end(),
        [node_index](const SessionSlot& candidate) noexcept {
            return candidate.active && candidate.node_index == node_index;
        });
    if (session != control.sessions.end()) {
        result.state = session->readiness == SessionReadiness::Live
            ? DirectorNodeConnection::State::Live
            : DirectorNodeConnection::State::CatchingUp;
        result.committed_frame = session->committed_cursor.through_frame.value();
    }
    return result;
}

bool DirectorFrameBarrier::runtime_tracking(
    core::u32 node_index, RuntimeTracking& output) const noexcept {
    const auto& control = *control_;
    const std::lock_guard lock{control.mutex};
    if (!control.valid || control.faulted) {
        return false;
    }
    const auto session = std::find_if(
        control.sessions.begin(),
        control.sessions.end(),
        [node_index](const SessionSlot& candidate) noexcept {
            return candidate.active && candidate.node_index == node_index;
        });
    if (session == control.sessions.end()
        || session->readiness != SessionReadiness::Live) {
        return false;
    }
    output = session->tracking;
    return true;
}

bool DirectorFrameBarrier::runtime_output_sync(
    core::u32 node_index, RuntimeOutputSync& output) const noexcept {
    const auto& control = *control_;
    const std::lock_guard lock{control.mutex};
    if (!control.valid || control.faulted) {
        return false;
    }
    const auto session = std::find_if(
        control.sessions.begin(),
        control.sessions.end(),
        [node_index](const SessionSlot& candidate) noexcept {
            return candidate.active && candidate.node_index == node_index;
        });
    if (session == control.sessions.end()
        || session->readiness != SessionReadiness::Live) {
        return false;
    }
    output = session->output_sync;
    return true;
}

bool DirectorFrameBarrier::runtime_renderer_readiness(
    core::u32 node_index,
    RuntimeRendererReadiness& output) const noexcept {
    const auto& control = *control_;
    const std::lock_guard lock{control.mutex};
    if (!control.valid || control.faulted) {
        return false;
    }
    const auto session = std::find_if(
        control.sessions.begin(),
        control.sessions.end(),
        [node_index](const SessionSlot& candidate) noexcept {
            return candidate.active && candidate.node_index == node_index;
        });
    if (session == control.sessions.end() ||
        session->readiness != SessionReadiness::Live) {
        return false;
    }
    output = session->renderer;
    return true;
}

bool DirectorFrameBarrier::runtime_asset_adoption(
    core::u32 node_index, core::u64 manifest_digest,
    RuntimeAssetAdoption& output) const noexcept {
    const auto& control = *control_;
    const std::lock_guard lock{control.mutex};
    if (!control.valid || control.faulted) {
        return false;
    }
    const auto session = std::find_if(
        control.sessions.begin(), control.sessions.end(),
        [node_index](const SessionSlot& candidate) noexcept {
            return candidate.active && candidate.node_index == node_index;
        });
    if (session == control.sessions.end() ||
        session->readiness != SessionReadiness::Live ||
        session->asset_adoption.manifest_digest != manifest_digest) {
        return false;
    }
    try {
        output = session->asset_adoption;
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

bool DirectorFrameBarrier::runtime_assets_ready(
    core::u64 manifest_digest,
    std::span<const core::u32> required_node_indices) const noexcept {
    const auto& control = *control_;
    const std::lock_guard lock{control.mutex};
    if (!control.valid || control.faulted) {
        return false;
    }

    for (std::size_t index = 0U; index < required_node_indices.size();
         ++index) {
        const core::u32 required = required_node_indices[index];
        if (required > max_session_node_index) {
            return false;
        }
        for (std::size_t earlier = 0U; earlier < index; ++earlier) {
            if (required_node_indices[earlier] == required) {
                return false;
            }
        }

        const auto session = std::find_if(
            control.sessions.begin(),
            control.sessions.end(),
            [required](const SessionSlot& candidate) noexcept {
                return candidate.active &&
                    candidate.node_index == required;
            });
        if (session == control.sessions.end()
            || session->readiness != SessionReadiness::Live
            || !session->asset_adoption.ready
            || session->asset_adoption.manifest_digest != manifest_digest) {
            return false;
        }
    }
    return true;
}

std::size_t DirectorFrameBarrier::open_frames() const noexcept {
    const auto& control = *control_;
    const std::lock_guard lock{control.mutex};
    return control.open_frame_count;
}

DirectorFrameBarrierObserverDiagnostics
DirectorFrameBarrier::observer_diagnostics() const noexcept {
    const auto& control = *control_;
    const std::lock_guard lock{control.mutex};
    return control.diagnostics;
}

} // namespace mediaserver::net
