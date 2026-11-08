#include <mediaserver/net/frame_commit_sink.hpp>

namespace mediaserver::net {
namespace {

[[nodiscard]] show::FrameCommitOutcome classify(
    const NodeBoundaryState& state,
    cmd::FrameNumber frame) noexcept {
    if (state.checkpoint_staged) {
        return show::FrameCommitOutcome::CheckpointPending;
    }
    if (state.needs_resync) {
        return show::FrameCommitOutcome::NeedsResync;
    }
    if (!state.live) {
        return show::FrameCommitOutcome::Unavailable;
    }
    // A frame the Director has not both delivered and permitted is simply not
    // approved yet. Holding the previous frame is the correct presentation.
    if (!state.next_frame_permitted || state.next_frame != frame) {
        return show::FrameCommitOutcome::NotApproved;
    }
    return show::FrameCommitOutcome::Ready;
}

[[nodiscard]] show::FrameCommitOutcome classify(
    const NodeSessionBoundaryError& error) noexcept {
    switch (error.code) {
    case NodeSessionBoundaryErrorCode::ServiceStopped:
    case NodeSessionBoundaryErrorCode::NoActiveSession:
    case NodeSessionBoundaryErrorCode::WrongState:
        return show::FrameCommitOutcome::Unavailable;
    case NodeSessionBoundaryErrorCode::SessionRejected:
        // The session refused without faulting: the frame was not prepared,
        // not permitted, or arrived outside the live phase.
        return show::FrameCommitOutcome::NotApproved;
    case NodeSessionBoundaryErrorCode::SessionFaulted:
        break;
    }
    return show::FrameCommitOutcome::NeedsResync;
}

[[nodiscard]] show::FrameCommitOutcome map_commit_status(
    cmd::CommitStatus status) noexcept {
    switch (status) {
    case cmd::CommitStatus::Committed:
        return show::FrameCommitOutcome::Committed;
    case cmd::CommitStatus::CheckpointPending:
        return show::FrameCommitOutcome::CheckpointPending;
    case cmd::CommitStatus::NeedsResync:
        break;
    }
    return show::FrameCommitOutcome::NeedsResync;
}

} // namespace

cmd::FrameNumber NodeServiceFrameCommitSink::committed_through() const noexcept {
    return node_->committed_through();
}

show::FrameCommitOutcome NodeServiceFrameCommitSink::poll(
    cmd::FrameNumber frame) const noexcept {
    const auto state = service_->boundary_state();
    if (!state) {
        return show::FrameCommitOutcome::Unavailable;
    }
    return classify(*state, frame);
}

show::FrameCommitReport NodeServiceFrameCommitSink::commit(
    cmd::FrameNumber frame) noexcept {
    const auto polled = poll(frame);
    if (polled != show::FrameCommitOutcome::Ready) {
        return show::FrameCommitReport{
            polled, frame, node_->document().version(), 0U};
    }

    auto committed = service_->commit_prepared_frame(frame);
    if (!committed) {
        return show::FrameCommitReport{
            classify(committed.error()),
            frame,
            node_->document().version(),
            0U,
        };
    }

    const auto& result = committed.value();
    return show::FrameCommitReport{
        map_commit_status(result.status),
        result.frame,
        result.version,
        result.command_count,
    };
}

bool NodeServiceFrameCommitSink::activate_pending_checkpoint() noexcept {
    // Deliberately not gated on a live session: a staged checkpoint is exactly
    // the state a resuming session is held in, and adopting it at a boundary
    // is what lets the session become live. The service still enforces its own
    // phase, so a checkpoint staged in any other state is refused there.
    const auto state = service_->boundary_state();
    if (!state || !state->checkpoint_staged) {
        return false;
    }
    if (!service_->activate_checkpoint()) {
        return false;
    }
    ++checkpoint_activations_;
    return true;
}

} // namespace mediaserver::net
