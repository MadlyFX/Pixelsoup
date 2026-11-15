#pragma once

#include <cstddef>
#include <utility>

#include <mediaserver/cmd/node_command_bus.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/show/frame_root_mailbox.hpp>
#include <mediaserver/show/show_clock.hpp>
#include <mediaserver/show/video_reference.hpp>

namespace mediaserver::show {

enum class FrameCommitOutcome : core::u8 {
    // No frame boundary was crossed, so no commit was attempted.
    Idle,
    // Precheck only: the frame may be committed now.
    Ready,
    Committed,
    // The Director has not approved this frame yet. Holding the previously
    // presented frame is the correct outcome, not a fault.
    NotApproved,
    CheckpointPending,
    NeedsResync,
    // The commit path itself is unusable: no session, or a stopped service.
    Unavailable,
};

struct FrameCommitReport final {
    FrameCommitOutcome outcome{FrameCommitOutcome::Idle};
    cmd::FrameNumber frame{};
    doc::Version version{};
    std::size_t command_count{};

    bool operator==(const FrameCommitReport&) const = default;
};

// The commit seam. The driver never performs network I/O itself: an
// implementation wraps either a local cmd::NodeCommandBus or a live node
// session whose commit is permit-gated and allocation-free.
//
// Implementations are called on the frame boundary and must not allocate.
class FrameCommitSink {
public:
    FrameCommitSink() = default;
    FrameCommitSink(const FrameCommitSink&) = delete;
    FrameCommitSink& operator=(const FrameCommitSink&) = delete;
    FrameCommitSink(FrameCommitSink&&) = delete;
    FrameCommitSink& operator=(FrameCommitSink&&) = delete;
    virtual ~FrameCommitSink() = default;

    // The last control frame this sink published. The driver always commits
    // the successor, so a checkpoint that rewinds the cursor is followed
    // without the driver holding its own copy of the chain.
    [[nodiscard]] virtual cmd::FrameNumber committed_through() const noexcept = 0;

    // Non-mutating precheck. Committing a frame that is not approved would
    // latch a resync fault, so the driver never guesses.
    [[nodiscard]] virtual FrameCommitOutcome poll(
        cmd::FrameNumber frame) const noexcept = 0;

    [[nodiscard]] virtual FrameCommitReport commit(
        cmd::FrameNumber frame) noexcept = 0;

    // A checkpoint staged by the transport is deliberately invisible until a
    // frame boundary adopts it. Returning true means the replacement is now
    // the visible root and the driver may re-poll; the default holds the
    // previous frame, which is correct for any sink whose checkpoints are
    // activated by their owner rather than by the boundary.
    [[nodiscard]] virtual bool activate_pending_checkpoint() noexcept {
        return false;
    }

    // The document published by the most recent successful commit.
    [[nodiscard]] virtual const doc::Root& visible_root() const noexcept = 0;
};

// Commit seam over a directly held command bus. This is the offline and
// single-process path; it takes no lock and performs no I/O, which makes it
// the reference implementation of the boundary contract.
class LocalNodeCommitSink final : public FrameCommitSink {
public:
    explicit LocalNodeCommitSink(cmd::NodeCommandBus& node) noexcept
        : node_(&node) {}

    [[nodiscard]] cmd::FrameNumber committed_through() const noexcept override {
        return node_->committed_through();
    }
    [[nodiscard]] FrameCommitOutcome poll(
        cmd::FrameNumber frame) const noexcept override;
    [[nodiscard]] FrameCommitReport commit(
        cmd::FrameNumber frame) noexcept override;
    [[nodiscard]] const doc::Root& visible_root() const noexcept override {
        return node_->document();
    }

private:
    cmd::NodeCommandBus* node_;
};

struct FrameBoundaryDriverConfig final {
    // A pacer: build it with pacer_clock_config(), never from a show range.
    ShowClockConfig clock{};
    // In VideoReference mode only tick_from_video_reference() with a counting
    // reference supplies frames for the picture. Without one the boundaries
    // come from the system clock and are reported `unreferenced`.
    enum class TimingAuthority : core::u8 {
        SystemClock,
        VideoReference,
    } timing_authority{TimingAuthority::SystemClock};
    // Slots handed to playback. Must be a power of two.
    std::size_t mailbox_capacity{8U};
    // Upper bound on commits performed in one tick. A node that fell behind
    // catches up with the Director across ticks rather than doing unbounded
    // work inside one.
    core::u64 max_commits_per_tick{4U};

    bool operator==(const FrameBoundaryDriverConfig&) const = default;
};

enum class FrameBoundaryDriverError : core::u8 {
    InvalidConfiguration,
    AllocationFailed,
};

struct FrameBoundaryReport final {
    ShowTick tick{};
    VideoReferenceState reference{VideoReferenceState::Unknown};
    // The driver is paced by a video reference that is not supplying frames:
    // not locked, or locked with a counter that stopped. Set on every tick
    // while that lasts, boundary or not. The boundaries below then came from
    // the system clock and everything they drive - commits, acknowledgements,
    // checkpoint adoption - happened as usual. The one thing the caller must
    // not do is put a new picture on its outputs: it repeats the previous one.
    bool unreferenced{};
    // Frame boundaries the local clock crossed during this tick. More than one
    // means the caller was late.
    core::u64 boundaries{};
    core::u64 committed{};
    core::u64 published{};
    // A boundary passed with nothing approved to commit, so the previously
    // presented frame stands.
    bool held{};
    // At least as many frames were approved as this tick was allowed to
    // commit, so a backlog may remain.
    bool budget_exhausted{};
    bool mailbox_overflow{};
    // A staged checkpoint was adopted at this boundary.
    bool checkpoint_activated{};
    FrameCommitOutcome last_outcome{FrameCommitOutcome::Idle};
    cmd::FrameNumber committed_through{};

    bool operator==(const FrameBoundaryReport&) const = default;
};

struct FrameBoundaryCounters final {
    core::u64 ticks{};
    core::u64 boundaries{};
    core::u64 committed_frames{};
    core::u64 published_frames{};
    core::u64 held_boundaries{};
    core::u64 mailbox_overflows{};
    core::u64 resync_reports{};
    core::u64 checkpoint_activations{};
    core::u64 reference_samples{};
    core::u64 reference_holds{};
    core::u64 reference_losses{};
    // Boundaries the system clock supplied while the reference was not
    // supplying frames.
    core::u64 unreferenced_boundaries{};
    // Times the reference counter moved further than elapsed time allows and
    // was taken as a new starting point.
    core::u64 reference_discontinuities{};

    bool operator==(const FrameBoundaryCounters&) const = default;
};

// The frame-boundary driver.
//
// It owns the local pacing clock and the playback handoff, and turns one
// wall-clock observation into three things: the boundaries crossed, the
// control frames the Director has approved since the last boundary, and the
// immutable roots playback should present.
//
// tick() allocates nothing, takes no lock of its own, and performs no I/O.
// Presented state is therefore always Director-approved: an unapproved frame
// is held, never synthesised.
class FrameBoundaryDriver final {
public:
    using TimePoint = ShowClock::TimePoint;

    [[nodiscard]] static core::Result<
        FrameBoundaryDriver,
        FrameBoundaryDriverError>
    create(
        FrameBoundaryDriverConfig config,
        FrameCommitSink& sink,
        TimePoint created_at) noexcept;

    [[nodiscard]] static bool valid_configuration(
        const FrameBoundaryDriverConfig& config) noexcept;

    FrameBoundaryDriver(const FrameBoundaryDriver&) = delete;
    FrameBoundaryDriver& operator=(const FrameBoundaryDriver&) = delete;
    FrameBoundaryDriver(FrameBoundaryDriver&&) noexcept = default;
    FrameBoundaryDriver& operator=(FrameBoundaryDriver&&) noexcept = default;
    ~FrameBoundaryDriver() = default;

    [[nodiscard]] core::Result<FrameBoundaryReport, ShowClockError> tick(
        TimePoint now) noexcept;

    // Reference-driven entry point; VideoReferenceCadence decides what each
    // reading is worth. A missing, lost or stalled source never supplies a
    // frame for the picture - the report says `unreferenced` and the caller
    // repeats its previous one. The control side does not stop with it: the
    // boundaries come from the system clock instead, because commits,
    // acknowledgements and checkpoint adoption are how a node is told what to
    // do about the fault. A driver that went quiet here could never receive
    // the edit that turns the reference requirement off.
    [[nodiscard]] core::Result<FrameBoundaryReport, ShowClockError>
    tick_from_video_reference(
        TimePoint now,
        VideoReferenceSample reference) noexcept;

    // Changes the pacing source at a replicated-policy boundary. It records
    // `at` without letting the old source contribute a partial frame.
    [[nodiscard]] core::Result<ShowTick, ShowClockError>
    set_timing_authority(
        FrameBoundaryDriverConfig::TimingAuthority authority,
        TimePoint at) noexcept;

    // Local pacing controls. The clock here is a pacer, not the show: a node
    // starts it once and never pauses or stops it, because a tick that
    // crosses no boundary commits nothing, adopts no checkpoint and
    // acknowledges nothing. The show's play, pause and seek are replicated
    // transport state read from the committed document, not these.
    [[nodiscard]] core::Result<ShowTick, ShowClockError> play(
        TimePoint at) noexcept {
        reference_.reset();
        return clock_.play(at);
    }
    [[nodiscard]] core::Result<ShowTick, ShowClockError> pause(
        TimePoint at) noexcept {
        reference_.reset();
        return clock_.pause(at);
    }
    [[nodiscard]] core::Result<ShowTick, ShowClockError> stop(
        TimePoint at) noexcept {
        reference_.reset();
        return clock_.stop(at);
    }
    [[nodiscard]] core::Result<ShowTick, ShowClockError> seek(
        ShowFrame target,
        TimePoint at) noexcept {
        reference_.reset();
        return clock_.seek(target, at);
    }
    [[nodiscard]] core::Result<ShowTick, ShowClockError> reconfigure_rate(
        FrameRate rate,
        TimePoint at) noexcept {
        auto changed = clock_.reconfigure_rate(rate, at);
        if (changed) {
            config_.clock.rate = rate;
            reference_.reset();
        }
        return changed;
    }

    [[nodiscard]] FrameRootMailbox& mailbox() noexcept { return mailbox_; }
    [[nodiscard]] const FrameRootMailbox& mailbox() const noexcept {
        return mailbox_;
    }
    [[nodiscard]] const ShowClock& clock() const noexcept { return clock_; }
    [[nodiscard]] const FrameBoundaryDriverConfig& config() const noexcept {
        return config_;
    }
    [[nodiscard]] const FrameBoundaryCounters& counters() const noexcept {
        return counters_;
    }

private:
    FrameBoundaryDriver(
        FrameBoundaryDriverConfig config,
        FrameCommitSink& sink,
        ShowClock clock,
        FrameRootMailbox mailbox) noexcept
        : config_(config),
          sink_(&sink),
          clock_(clock),
          mailbox_(std::move(mailbox)) {}

    [[nodiscard]] core::Result<FrameBoundaryReport, ShowClockError>
    process_tick(
        ShowTick tick,
        VideoReferenceState reference,
        bool unreferenced) noexcept;

    FrameBoundaryDriverConfig config_{};
    FrameCommitSink* sink_{};
    ShowClock clock_;
    FrameRootMailbox mailbox_;
    FrameBoundaryCounters counters_{};
    VideoReferenceCadence reference_{};
};

} // namespace mediaserver::show
