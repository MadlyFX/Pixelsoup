#include <mediaserver/persist/durable_director.hpp>

#include <atomic>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace mediaserver::persist {
namespace {

[[nodiscard]] bool same_cursor(
    const cmd::StreamCursor& left,
    const cmd::StreamCursor& right) noexcept {
    return left.through_frame == right.through_frame && left.revision == right.revision
        && left.next_command == right.next_command;
}

[[nodiscard]] DurableDirectorError coordinator_error(
    DurableDirectorErrorCode code,
    std::optional<cmd::SealError> seal_error = std::nullopt,
    std::optional<CommandLogError> command_log_error = std::nullopt,
    std::optional<ReleaseLedgerError> release_ledger_error = std::nullopt) {
    return DurableDirectorError{
        code,
        seal_error,
        std::move(command_log_error),
        std::move(release_ledger_error),
    };
}

[[nodiscard]] bool is_known_prewrite_rejection(
    const CommandLogError& error) noexcept {
    return std::holds_alternative<EncodeFailure>(error)
        || std::holds_alternative<DecodeFailure>(error)
        || std::holds_alternative<ProtocolFailure>(error);
}

[[nodiscard]] LogCursor as_log_cursor(const cmd::StreamCursor& cursor) noexcept {
    return LogCursor{
        cursor.through_frame.value(),
        cursor.revision.value(),
        cursor.next_command.value(),
    };
}

[[nodiscard]] cmd::StreamCursor as_stream_cursor(LogCursor cursor) noexcept {
    return cmd::StreamCursor{
        cmd::FrameNumber{cursor.through_frame},
        doc::Version{cursor.revision},
        cmd::CommandId{cursor.next_command},
    };
}

[[nodiscard]] bool is_known_release_prewrite_rejection(
    const ReleaseLedgerError& error) noexcept {
    return error.code == ReleaseLedgerErrorCode::ChainMismatch
        || error.code == ReleaseLedgerErrorCode::LimitExceeded;
}

[[nodiscard]] StartupRecoveryError startup_error(
    StartupRecoveryErrorCode code,
    std::optional<CommandLogError> command_log_error = std::nullopt) {
    return StartupRecoveryError{code, std::move(command_log_error)};
}

[[nodiscard]] std::uint64_t next_coordinator_id() noexcept {
    static std::atomic<std::uint64_t> next{1U};
    return next.fetch_add(1U, std::memory_order_relaxed);
}

} // namespace

CommittedFrameBatch::CommittedFrameBatch(
    std::vector<cmd::Command> commands,
    cmd::FrameSeal seal,
    AppendReceipt receipt,
    cmd::StreamCursor before_cursor,
    std::uint64_t coordinator_id) noexcept
    : commands_(std::move(commands)),
      seal_(std::move(seal)),
      receipt_(receipt),
      before_cursor_(before_cursor),
      coordinator_id_(coordinator_id) {}

CommittedFrameBatch::CommittedFrameBatch(CommittedFrameBatch&& other) noexcept
    : commands_(std::move(other.commands_)),
      seal_(std::move(other.seal_)),
      receipt_(other.receipt_),
      before_cursor_(other.before_cursor_),
      coordinator_id_(other.coordinator_id_),
      valid_(std::exchange(other.valid_, false)) {}

CommittedFrameBatch& CommittedFrameBatch::operator=(
    CommittedFrameBatch&& other) noexcept {
    if (this != &other) {
        commands_ = std::move(other.commands_);
        seal_ = std::move(other.seal_);
        receipt_ = other.receipt_;
        before_cursor_ = other.before_cursor_;
        coordinator_id_ = other.coordinator_id_;
        valid_ = std::exchange(other.valid_, false);
    }
    return *this;
}

DurableResumeSource::DurableResumeSource(
    DurableResumeSource&& other) noexcept
    : log_(std::move(other.log_)),
      checkpoint_(std::move(other.checkpoint_)),
      coordinator_id_(other.coordinator_id_),
      valid_(std::exchange(other.valid_, false)) {}

DurableResumeSource& DurableResumeSource::operator=(
    DurableResumeSource&& other) noexcept {
    if (this != &other) {
        log_ = std::move(other.log_);
        checkpoint_ = std::move(other.checkpoint_);
        coordinator_id_ = other.coordinator_id_;
        valid_ = std::exchange(other.valid_, false);
    }
    return *this;
}

DurableDirector::DurableDirector(
    cmd::DirectorScheduler& director,
    CommandLog& log,
    FrameDurability durability) noexcept
    : director_(&director),
      log_(&log),
      durability_(durability),
      coordinator_id_(next_coordinator_id()),
      published_tail_(log.tail_cursor()),
      memory_release_frontier_(log.tail_cursor()) {}

DurableDirector::DurableDirector(
    cmd::DirectorScheduler& director,
    CommandLog& log,
    ReleaseLedger& release_ledger,
    FrameDurability durability) noexcept
    : director_(&director),
      log_(&log),
      release_ledger_(&release_ledger),
      durability_(durability),
      coordinator_id_(next_coordinator_id()),
      published_tail_(log.tail_cursor()),
      memory_release_frontier_(log.tail_cursor()) {
    const auto checkpoint = director_->checkpoint().cursor;
    const auto log_base = as_log_cursor(log_->base_cursor());
    const auto log_tail = as_log_cursor(log_->tail_cursor());
    const auto release = release_ledger_->recovery_report();
    // The startup coherence gate is the same under either cadence: a
    // coordinator may only begin from a WAL whose every entry is already
    // released. What EditsOnly changes is that it then stops writing, not what
    // it is allowed to start from.
    if (release_ledger_->sync_policy() == SyncPolicy::None
        || !same_cursor(checkpoint, log_->tail_cursor())
        || release.base_cursor != log_base || release.wal_tail != log_tail
        || release.frontier != log_tail) {
        require_recovery();
    }
}

core::Result<CommittedFrameBatch, DurableDirectorError> DurableDirector::seal_frame(
    cmd::FrameNumber frame) {
    using Result = core::Result<CommittedFrameBatch, DurableDirectorError>;

    // Lock ordering is always Director transaction mutex, then CommandLog's
    // internal append/capture mutex. No persistence callback takes this lock.
    const std::lock_guard transaction_lock{transaction_mutex_};

    if (state() == DurableDirectorState::RecoveryRequired) {
        return Result::failure(
            coordinator_error(DurableDirectorErrorCode::RecoveryRequired));
    }
    const auto journalling = durability_ == FrameDurability::EveryFrame;
    if (journalling && log_->sync_policy() == SyncPolicy::None) {
        return Result::failure(
            coordinator_error(DurableDirectorErrorCode::NonDurablePolicy));
    }
    const auto checkpoint = director_->checkpoint().cursor;
    // Under EveryFrame the WAL tail is the authority, exactly as it has always
    // been. Under EditsOnly the WAL stops at the last journalled frame, so the
    // authority is the tail this coordinator has published.
    const auto authoritative_tail =
        journalling ? log_->tail_cursor() : published_tail_;
    if (!same_cursor(checkpoint, authoritative_tail)) {
        return Result::failure(
            coordinator_error(DurableDirectorErrorCode::CursorMismatch));
    }
    std::vector<cmd::Command> commands;
    try {
        const auto scheduled = director_->commands_for(frame);
        commands.assign(scheduled.begin(), scheduled.end());
    } catch (const std::bad_alloc&) {
        return Result::failure(
            coordinator_error(DurableDirectorErrorCode::SnapshotAllocationFailed));
    } catch (const std::length_error&) {
        return Result::failure(
            coordinator_error(DurableDirectorErrorCode::SnapshotAllocationFailed));
    }

    std::optional<core::Result<cmd::SealPreparation, cmd::SealError>> prepared;
    try {
        prepared.emplace(director_->prepare_seal(frame));
    } catch (const std::bad_alloc&) {
        // A preparation token is intentionally only available after a
        // successful return. If an exceptional Director implementation ever
        // exposes staged state without returning its token, it cannot be
        // aborted safely; recovery is required rather than guessing by frame.
        if (director_->prepared_seal() != nullptr) {
            require_recovery();
            return Result::failure(
                coordinator_error(DurableDirectorErrorCode::AbortFailed));
        }
        return Result::failure(
            coordinator_error(DurableDirectorErrorCode::PrepareFailed));
    } catch (const std::length_error&) {
        if (director_->prepared_seal() != nullptr) {
            require_recovery();
            return Result::failure(
                coordinator_error(DurableDirectorErrorCode::AbortFailed));
        }
        return Result::failure(
            coordinator_error(DurableDirectorErrorCode::PrepareFailed));
    }
    if (!*prepared) {
        if (director_->prepared_seal() != nullptr) {
            require_recovery();
            return Result::failure(coordinator_error(
                DurableDirectorErrorCode::AbortFailed,
                prepared->error()));
        }
        return Result::failure(coordinator_error(
            DurableDirectorErrorCode::PrepareFailed,
            prepared->error()));
    }

    auto preparation = std::move(*prepared).value();
    const auto token = preparation.token;
    auto seal = std::move(preparation.seal);
    auto expected_next_command = checkpoint.next_command.value();
    if (!commands.empty()) {
        expected_next_command = commands.back().id.value() + 1U;
    }
    const auto expected_cursor = cmd::StreamCursor{
        frame,
        seal.result,
        cmd::CommandId{expected_next_command},
    };
    const auto expected_log_cursor = as_log_cursor(expected_cursor);

    if (!journalling) {
        // No append, no sync, no release-ledger write. The receipt records
        // exactly that: it names the cursor this frame publishes at and says
        // plainly that nothing was written, so a caller can never mistake it
        // for a durable one.
        const auto* const staged = director_->prepared_seal();
        if (staged == nullptr || *staged != seal) {
            require_recovery();
            return Result::failure(
                coordinator_error(DurableDirectorErrorCode::PostAppendInvariant));
        }
        const AppendReceipt receipt{
            log_->recovery_report().valid_bytes,
            0U,
            expected_log_cursor,
            false,
            0U,
        };
        if (!director_->commit_prepared_seal(token)) {
            require_recovery();
            return Result::failure(
                coordinator_error(DurableDirectorErrorCode::CommitFailed));
        }
        published_tail_ = expected_cursor;
        if (!commands.empty()
            && unjournalled_edit_frames_
                != std::numeric_limits<std::uint64_t>::max()) {
            ++unjournalled_edit_frames_;
        }
        return Result::success(CommittedFrameBatch{
            std::move(commands),
            std::move(seal),
            receipt,
            checkpoint,
            coordinator_id_,
        });
    }

    const auto prior_valid_bytes = log_->recovery_report().valid_bytes;
    const auto prior_entry_count = log_->recovery_report().entry_count;

    std::optional<core::Result<AppendReceipt, CommandLogError>> appended;
    try {
        appended.emplace(log_->append(commands, seal));
    } catch (const std::bad_alloc&) {
        require_recovery();
        return Result::failure(
            coordinator_error(DurableDirectorErrorCode::PersistenceFailure));
    } catch (const std::length_error&) {
        require_recovery();
        return Result::failure(
            coordinator_error(DurableDirectorErrorCode::PersistenceFailure));
    }
    if (!*appended) {
        if (is_known_prewrite_rejection(appended->error())) {
            auto append_error = std::move(appended->error());
            if (!director_->abort_prepared_seal(token)) {
                require_recovery();
                return Result::failure(coordinator_error(
                    DurableDirectorErrorCode::AbortFailed,
                    std::nullopt,
                    std::move(append_error)));
            }
            return Result::failure(coordinator_error(
                DurableDirectorErrorCode::PrewriteRejected,
                std::nullopt,
                std::move(append_error)));
        }

        // Storage failures can be reported after any point in the append
        // protocol. Latch before moving the diagnostic so even an exceptional
        // error handoff cannot leave this coordinator publishable.
        require_recovery();
        return Result::failure(coordinator_error(
            DurableDirectorErrorCode::PersistenceFailure,
            std::nullopt,
            std::move(appended->error())));
    }

    const auto receipt = appended->value();
    const auto receipt_end_fits =
        receipt.bytes_written <=
        std::numeric_limits<std::uint64_t>::max() - receipt.offset;
    const auto entry_count_fits =
        prior_entry_count != std::numeric_limits<std::uint64_t>::max();
    const auto report = log_->recovery_report();
    const auto* still_prepared = director_->prepared_seal();
    if (!receipt.durable || receipt.offset != prior_valid_bytes
        || receipt.bytes_written == 0U || receipt.cursor != expected_log_cursor
        || !same_cursor(log_->tail_cursor(), expected_cursor) || !receipt_end_fits
        || report.valid_bytes != receipt.offset + receipt.bytes_written
        || !entry_count_fits || report.entry_count != prior_entry_count + 1U
        || report.tail_cursor != expected_log_cursor
        || still_prepared == nullptr || *still_prepared != seal) {
        require_recovery();
        return Result::failure(
            coordinator_error(DurableDirectorErrorCode::PostAppendInvariant));
    }

    if (!director_->commit_prepared_seal(token)) {
        require_recovery();
        return Result::failure(
            coordinator_error(DurableDirectorErrorCode::CommitFailed));
    }
    published_tail_ = expected_cursor;

    return Result::success(CommittedFrameBatch{
        std::move(commands),
        std::move(seal),
        receipt,
        checkpoint,
        coordinator_id_,
    });
}

bool DurableDirector::record_release(
    const CommittedFrameBatch& batch,
    std::uint64_t expected_ordinal) noexcept {
    const std::lock_guard transaction_lock{transaction_mutex_};

    if (state() != DurableDirectorState::RecoveryRequired
        && durability_ == FrameDurability::EditsOnly && expected_ordinal != 0U
        && batch.valid_ && batch.coordinator_id_ == coordinator_id_
        && !batch.receipt_.durable && batch.receipt_.bytes_written == 0U
        && expected_ordinal == next_memory_release_ordinal_
        && next_memory_release_ordinal_
            != std::numeric_limits<std::uint64_t>::max()
        && same_cursor(batch.before_cursor_, memory_release_frontier_)
        && batch.seal_.frame.value() == batch.receipt_.cursor.through_frame) {
        // Overwriting the oldest slot is deliberate: a release older than the
        // window is answered with a checkpoint, not with a stale ordinal.
        memory_releases_[memory_release_next_] = MemoryRelease{
            batch.before_cursor_,
            batch.after_cursor(),
            expected_ordinal,
        };
        memory_release_next_ =
            (memory_release_next_ + 1U) % memory_releases_.size();
        if (memory_release_count_ < memory_releases_.size()) {
            ++memory_release_count_;
        }
        memory_release_frontier_ = batch.after_cursor();
        ++next_memory_release_ordinal_;
        return true;
    }
    return false;
}

std::uint64_t DurableDirector::unjournalled_edit_frames() const noexcept {
    const std::lock_guard transaction_lock{transaction_mutex_};
    return unjournalled_edit_frames_;
}

cmd::StreamCursor DurableDirector::published_tail() const noexcept {
    const std::lock_guard transaction_lock{transaction_mutex_};
    return published_tail_;
}

core::Result<DurableReleaseReceipt, DurableDirectorError>
DurableDirector::persist_release(
    const CommittedFrameBatch& batch,
    std::uint64_t expected_ordinal) {
    using Result = core::Result<
        DurableReleaseReceipt,
        DurableDirectorError>;
    const std::lock_guard transaction_lock{transaction_mutex_};

    if (state() == DurableDirectorState::RecoveryRequired) {
        return Result::failure(
            coordinator_error(DurableDirectorErrorCode::RecoveryRequired));
    }
    if (release_ledger_ == nullptr
        || durability_ != FrameDurability::EveryFrame) {
        return Result::failure(
            coordinator_error(DurableDirectorErrorCode::ReleaseUnavailable));
    }
    if (expected_ordinal == 0U || !batch.valid_
        || batch.coordinator_id_ != coordinator_id_
        || !batch.receipt_.durable || batch.receipt_.bytes_written == 0U) {
        return Result::failure(coordinator_error(
            DurableDirectorErrorCode::ReleasePrewriteRejected));
    }

    const auto identity = WalFrameIdentity{
        as_log_cursor(batch.before_cursor_),
        batch.receipt_.cursor,
        batch.receipt_.offset,
        batch.receipt_.bytes_written,
        batch.receipt_.entry_crc32c,
    };
    const auto ledger_before = release_ledger_->recovery_report();
    if (ledger_before.decision_count
            == std::numeric_limits<std::uint64_t>::max()
        || expected_ordinal != ledger_before.decision_count + 1U
        || ledger_before.frontier != identity.before
        || ledger_before.released_wal_bytes != identity.offset) {
        return Result::failure(coordinator_error(
            DurableDirectorErrorCode::ReleasePrewriteRejected));
    }
    const auto command_log = log_->recovery_report();
    const auto identity_end_fits = identity.bytes
        <= std::numeric_limits<std::uint64_t>::max() - identity.offset;
    if (!identity_end_fits
        || identity.offset + identity.bytes > command_log.valid_bytes
        || identity.after != batch.receipt_.cursor
        || batch.seal_.frame.value() != identity.after.through_frame
        || as_log_cursor(batch.before_cursor_) != identity.before) {
        return Result::failure(coordinator_error(
            DurableDirectorErrorCode::ReleasePrewriteRejected));
    }

    std::optional<core::Result<DurableReleaseReceipt, ReleaseLedgerError>>
        appended;
    try {
        appended.emplace(release_ledger_->append_release(identity));
    } catch (const std::bad_alloc&) {
        require_recovery();
        return Result::failure(coordinator_error(
            DurableDirectorErrorCode::ReleasePersistenceFailure));
    } catch (const std::length_error&) {
        require_recovery();
        return Result::failure(coordinator_error(
            DurableDirectorErrorCode::ReleasePersistenceFailure));
    }
    if (!*appended) {
        auto error = std::move(appended->error());
        if (is_known_release_prewrite_rejection(error)) {
            return Result::failure(coordinator_error(
                DurableDirectorErrorCode::ReleasePrewriteRejected,
                std::nullopt,
                std::nullopt,
                std::move(error)));
        }
        require_recovery();
        return Result::failure(coordinator_error(
            DurableDirectorErrorCode::ReleasePersistenceFailure,
            std::nullopt,
            std::nullopt,
            std::move(error)));
    }

    auto receipt = std::move(appended->value());
    const auto ledger_after = release_ledger_->recovery_report();
    const auto receipt_end_fits = receipt.bytes_written()
        <= std::numeric_limits<std::uint64_t>::max() - receipt.offset();
    const auto wal_end_fits = identity.bytes
        <= std::numeric_limits<std::uint64_t>::max() - identity.offset;
    if (!receipt.durable() || receipt.ordinal() != expected_ordinal
        || receipt.frame() != identity
        || receipt.offset() != ledger_before.valid_bytes
        || receipt.bytes_written() == 0U || !receipt_end_fits
        || ledger_after.valid_bytes
            != receipt.offset() + receipt.bytes_written()
        || ledger_after.decision_count != expected_ordinal
        || ledger_after.frontier != identity.after || !wal_end_fits
        || ledger_after.released_wal_bytes
            != identity.offset + identity.bytes) {
        require_recovery();
        return Result::failure(coordinator_error(
            DurableDirectorErrorCode::PostReleaseInvariant));
    }
    return Result::success(std::move(receipt));
}

core::Result<DurableResumeSource, DurableDirectorError>
DurableDirector::capture_resume_source() const {
    return capture_resume_source_impl(nullptr);
}

core::Result<DurableResumeSource, DurableDirectorError>
DurableDirector::capture_resume_source(
    ResumeSourceRegistration& registration) const {
    return capture_resume_source_impl(&registration);
}

core::Result<DurableResumeSource, DurableDirectorError>
DurableDirector::capture_resume_source_impl(
    ResumeSourceRegistration* registration) const {
    using Result = core::Result<DurableResumeSource, DurableDirectorError>;
    const std::lock_guard transaction_lock{transaction_mutex_};

    if (state() == DurableDirectorState::RecoveryRequired) {
        return Result::failure(
            coordinator_error(DurableDirectorErrorCode::RecoveryRequired));
    }
    if (director_->prepared_seal() != nullptr) {
        return Result::failure(coordinator_error(
            DurableDirectorErrorCode::ResumeSourceUnavailable));
    }

    auto checkpoint = director_->checkpoint();
    auto snapshot = log_->capture_committed();
    if (!snapshot) {
        return Result::failure(coordinator_error(
            DurableDirectorErrorCode::PersistenceFailure,
            std::nullopt,
            std::move(snapshot).error()));
    }
    // The checkpoint is the authoritative tail a resuming session is placed
    // at. Under EveryFrame that must be the WAL tail exactly, which also
    // catches a Director whose committed state ran ahead of the log outside
    // this coordinator. Under EditsOnly the snapshot is only the durable
    // prefix, so the tail is checked against what this coordinator published
    // and a session cursor outside the snapshot resolves to a checkpoint
    // rather than a replay.
    const auto authoritative_tail = durability_ == FrameDurability::EveryFrame
        ? snapshot.value().tail_cursor()
        : published_tail_;
    if (!same_cursor(checkpoint.cursor, authoritative_tail)) {
        return Result::failure(
            coordinator_error(DurableDirectorErrorCode::CursorMismatch));
    }
    if (release_ledger_ != nullptr
        && durability_ == FrameDurability::EveryFrame) {
        const auto release = release_ledger_->recovery_report();
        const auto snapshot_base = as_log_cursor(snapshot.value().base_cursor());
        const auto snapshot_tail = as_log_cursor(snapshot.value().tail_cursor());
        if (release.base_cursor != snapshot_base) {
            return Result::failure(coordinator_error(
                DurableDirectorErrorCode::ReleaseUnavailable));
        }
        if (release.frontier != snapshot_tail) {
            const auto pending = release.frontier.through_frame
                < snapshot_tail.through_frame;
            return Result::failure(coordinator_error(
                pending ? DurableDirectorErrorCode::ReleasePending
                        : DurableDirectorErrorCode::ReleaseUnavailable));
        }
        if (!same_cursor(
                as_stream_cursor(release.frontier), checkpoint.cursor)) {
            return Result::failure(coordinator_error(
                DurableDirectorErrorCode::ReleaseUnavailable));
        }
    }
    if (registration != nullptr
        && !registration->register_captured_tail(checkpoint.cursor)) {
        return Result::failure(coordinator_error(
            DurableDirectorErrorCode::ResumeRegistrationRejected));
    }
    return Result::success(DurableResumeSource{
        std::move(snapshot).value(), std::move(checkpoint), coordinator_id_});
}

bool DurableDirector::authorizes(
    const CommittedFrameBatch& batch) const noexcept {
    // Durability is the coordinator's policy, not the batch's claim. Under
    // EveryFrame an undurable batch is still refused; under EditsOnly the
    // capability is the coordinator identity, which a session cannot forge.
    return state() == DurableDirectorState::Ready && batch.valid_
        && batch.coordinator_id_ == coordinator_id_
        && (batch.receipt().durable || durability_ == FrameDurability::EditsOnly);
}

bool DurableDirector::authorizes(
    const DurableResumeSource& source) const noexcept {
    return state() == DurableDirectorState::Ready && source.valid_
        && source.coordinator_id_ == coordinator_id_;
}

std::uint64_t DurableDirector::next_release_ordinal() const noexcept {
    const std::lock_guard transaction_lock{transaction_mutex_};
    if (state() != DurableDirectorState::Ready) {
        return 0U;
    }
    if (durability_ == FrameDurability::EditsOnly) {
        return next_memory_release_ordinal_;
    }
    if (release_ledger_ == nullptr) {
        return 0U;
    }
    return release_ledger_->next_release_ordinal();
}

std::optional<std::uint64_t> DurableDirector::release_ordinal(
    const cmd::StreamCursor& before,
    const cmd::StreamCursor& after) const noexcept {
    const std::lock_guard transaction_lock{transaction_mutex_};
    if (state() != DurableDirectorState::Ready) {
        return std::nullopt;
    }
    if (durability_ == FrameDurability::EditsOnly) {
        for (std::size_t index = 0U; index < memory_release_count_; ++index) {
            const auto& entry = memory_releases_[index];
            if (same_cursor(entry.before, before)
                && same_cursor(entry.after, after)) {
                return entry.ordinal;
            }
        }
        return std::nullopt;
    }
    if (release_ledger_ == nullptr) {
        return std::nullopt;
    }
    return release_ledger_->release_ordinal(
        as_log_cursor(before), as_log_cursor(after));
}

bool DurableDirector::broadcast_committed(
    const CommittedFrameBatch& batch,
    cmd::LoopbackTransport& transport) const {
    // A gated coordinator intentionally has no implicit local-output permit.
    // Its release receipt is consumed by the network barrier; until a future
    // receipt-bound local-output API exists, rejecting here prevents the
    // historical helper from bypassing cluster release.
    if (release_ledger_ != nullptr || !authorizes(batch)) {
        return false;
    }
    try {
        for (const auto& command : batch.commands()) {
            transport.broadcast(command);
        }
        transport.broadcast(batch.seal());
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
    return true;
}

core::Result<RecoveredRuntime, StartupRecoveryError> recover_genesis_runtime(
    const CommandLog& log) {
    using Result = core::Result<RecoveredRuntime, StartupRecoveryError>;
    const auto base = log.base_cursor();
    if (base.through_frame != cmd::FrameNumber{0U}
        || base.revision != doc::Version{0U}
        || base.next_command != cmd::CommandId{1U}) {
        return Result::failure(
            startup_error(StartupRecoveryErrorCode::NonGenesisLog));
    }

    try {
        cmd::NodeCommandBus replay_node;
        auto replay = log.replay_into(
            replay_node,
            ReplayRetention::RetireCommitted);
        if (!replay) {
            return Result::failure(startup_error(
                StartupRecoveryErrorCode::ReplayFailed,
                std::move(replay.error())));
        }

        const auto cursor = replay_node.cursor();
        if (!same_cursor(cursor, log.tail_cursor())
            || !same_cursor(cursor, replay.value().cursor)
            || replay_node.document().version() != cursor.revision) {
            return Result::failure(
                startup_error(StartupRecoveryErrorCode::CursorMismatch));
        }

        const auto document = replay_node.document();
        cmd::DirectorScheduler director{
            document,
            cursor.through_frame,
            cursor.next_command,
        };
        cmd::NodeCommandBus local_node{
            document,
            cursor.through_frame,
            cursor.next_command,
        };
        return Result::success(RecoveredRuntime{
            std::move(director),
            std::move(local_node),
            replay.value(),
            StartupResetReport{},
        });
    } catch (const std::bad_alloc&) {
        return Result::failure(
            startup_error(StartupRecoveryErrorCode::ConstructionFailed));
    } catch (const std::length_error&) {
        return Result::failure(
            startup_error(StartupRecoveryErrorCode::ConstructionFailed));
    }
}

} // namespace mediaserver::persist
