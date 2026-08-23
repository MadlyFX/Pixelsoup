#include <mediaserver/show/frame_boundary_driver.hpp>

#include <limits>
#include <utility>

namespace mediaserver::show {
namespace {

// A tick that could not do at least one commit could never make progress, and
// an unbounded budget would let one late tick monopolise the boundary.
inline constexpr core::u64 max_commit_budget = 256U;

[[nodiscard]] FrameCommitOutcome map_commit_status(
    cmd::CommitStatus status) noexcept {
    switch (status) {
    case cmd::CommitStatus::Committed:
        return FrameCommitOutcome::Committed;
    case cmd::CommitStatus::CheckpointPending:
        return FrameCommitOutcome::CheckpointPending;
    case cmd::CommitStatus::NeedsResync:
        break;
    }
    return FrameCommitOutcome::NeedsResync;
}

} // namespace

FrameCommitOutcome LocalNodeCommitSink::poll(
    cmd::FrameNumber frame) const noexcept {
    // A staged checkpoint blocks traffic, and a latched fault means the
    // visible root is no longer known to match the Director. Either way the
    // frame is not committable, and asking the bus would latch a second fault.
    if (node_->prepared_checkpoint() != nullptr) {
        return FrameCommitOutcome::CheckpointPending;
    }
    if (node_->needs_resync()) {
        return FrameCommitOutcome::NeedsResync;
    }
    if (node_->committed_through().value()
        == std::numeric_limits<core::u64>::max()) {
        return FrameCommitOutcome::Unavailable;
    }
    if (frame.value() != node_->committed_through().value() + 1U) {
        return FrameCommitOutcome::NotApproved;
    }
    if (!node_->is_prepared(frame)) {
        return FrameCommitOutcome::NotApproved;
    }
    return FrameCommitOutcome::Ready;
}

FrameCommitReport LocalNodeCommitSink::commit(cmd::FrameNumber frame) noexcept {
    const auto polled = poll(frame);
    if (polled != FrameCommitOutcome::Ready) {
        return FrameCommitReport{
            polled, frame, node_->document().version(), 0U};
    }

    const auto committed = node_->commit_frame(frame);
    return FrameCommitReport{
        map_commit_status(committed.status),
        committed.frame,
        committed.version,
        committed.command_count,
    };
}

bool FrameBoundaryDriver::valid_configuration(
    const FrameBoundaryDriverConfig& config) noexcept {
    return ShowClock::valid_configuration(config.clock)
        && is_power_of_two(config.mailbox_capacity)
        && config.max_commits_per_tick != 0U
        && config.max_commits_per_tick <= max_commit_budget;
}

core::Result<FrameBoundaryDriver, FrameBoundaryDriverError>
FrameBoundaryDriver::create(
    FrameBoundaryDriverConfig config,
    FrameCommitSink& sink,
    TimePoint created_at) noexcept {
    using CreateResult =
        core::Result<FrameBoundaryDriver, FrameBoundaryDriverError>;

    if (!valid_configuration(config)) {
        return CreateResult::failure(
            FrameBoundaryDriverError::InvalidConfiguration);
    }

    auto clock = ShowClock::create(config.clock, created_at);
    if (!clock) {
        return CreateResult::failure(
            FrameBoundaryDriverError::InvalidConfiguration);
    }

    auto mailbox = FrameRootMailbox::create(config.mailbox_capacity);
    if (!mailbox) {
        return CreateResult::failure(
            mailbox.error() == MailboxError::InvalidCapacity
                ? FrameBoundaryDriverError::InvalidConfiguration
                : FrameBoundaryDriverError::AllocationFailed);
    }

    return CreateResult::success(FrameBoundaryDriver(
        config,
        sink,
        std::move(clock).value(),
        std::move(mailbox).value()));
}

core::Result<FrameBoundaryReport, ShowClockError> FrameBoundaryDriver::tick(
    TimePoint now) noexcept {
    using TickResult = core::Result<FrameBoundaryReport, ShowClockError>;

    auto advanced = clock_.advance_to(now);
    if (!advanced) {
        return TickResult::failure(advanced.error());
    }

    FrameBoundaryReport report{};
    report.tick = std::move(advanced).value();
    report.boundaries = report.tick.advanced;
    report.committed_through = sink_->committed_through();

    ++counters_.ticks;
    counters_.boundaries += report.boundaries;

    if (report.boundaries == 0U) {
        report.last_outcome = FrameCommitOutcome::Idle;
        return TickResult::success(report);
    }

    for (core::u64 attempt = 0U; attempt < config_.max_commits_per_tick;
         ++attempt) {
        const auto through = sink_->committed_through().value();
        if (through == std::numeric_limits<core::u64>::max()) {
            report.last_outcome = FrameCommitOutcome::Unavailable;
            break;
        }

        const cmd::FrameNumber next{through + 1U};
        auto polled = sink_->poll(next);
        if (polled == FrameCommitOutcome::CheckpointPending
            && !report.checkpoint_activated
            && sink_->activate_pending_checkpoint()) {
            // Adoption is a boundary event, and it may rewind the cursor, so
            // the successor is recomputed from scratch on the next pass.
            report.checkpoint_activated = true;
            ++counters_.checkpoint_activations;
            report.committed_through = sink_->committed_through();
            continue;
        }
        if (polled != FrameCommitOutcome::Ready) {
            report.last_outcome = polled;
            break;
        }

        const auto committed = sink_->commit(next);
        report.last_outcome = committed.outcome;
        if (committed.outcome != FrameCommitOutcome::Committed) {
            break;
        }

        ++report.committed;
        ++counters_.committed_frames;
        report.committed_through = committed.frame;

        if (mailbox_.full()) {
            // Playback falling behind is reported, but it never stalls the
            // control plane: committing and acknowledging continue so the
            // cohort is not held up by one slow presenter. The root is not
            // even copied, so a full handoff cannot cause a release here
            // either.
            if (!report.mailbox_overflow) {
                report.mailbox_overflow = true;
                ++counters_.mailbox_overflows;
            }
        } else {
            PublishedFrame published{};
            published.control_frame = committed.frame;
            published.version = committed.version;
            // Copying a root is a refcount increment on an immutable trie, and
            // the transport lookup walks that trie without allocating.
            published.root = sink_->visible_root();
            published.transport = read_transport(published.root);
            if (published.transport) {
                published.show_frame =
                    derive_show_frame(*published.transport, committed.frame);
            }

            if (mailbox_.publish(std::move(published))
                == PublishStatus::Published) {
                ++report.published;
                ++counters_.published_frames;
            }
        }

        if (attempt + 1U == config_.max_commits_per_tick) {
            report.budget_exhausted = true;
        }
    }

    if (report.committed == 0U) {
        report.held = true;
        ++counters_.held_boundaries;
    }
    if (report.last_outcome == FrameCommitOutcome::NeedsResync) {
        ++counters_.resync_reports;
    }
    return TickResult::success(report);
}

} // namespace mediaserver::show
