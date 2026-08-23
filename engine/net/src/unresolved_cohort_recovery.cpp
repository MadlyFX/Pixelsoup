#include <mediaserver/net/unresolved_cohort_recovery.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <mediaserver/cmd/node_command_bus.hpp>

namespace mediaserver::net {
namespace {

using Error = UnresolvedCohortRecoveryError;

template <typename Value>
using RecoveryResult = core::Result<Value, Error>;

[[nodiscard]] Error error(UnresolvedCohortRecoveryErrorCode code) {
    return Error{code, std::nullopt, std::nullopt, std::nullopt};
}

[[nodiscard]] Error command_log_error(persist::CommandLogError detail) {
    auto result = error(UnresolvedCohortRecoveryErrorCode::SnapshotMismatch);
    result.command_log_error.emplace(std::move(detail));
    return result;
}

[[nodiscard]] Error journal_error(persist::CohortJournalError detail) {
    auto result = error(UnresolvedCohortRecoveryErrorCode::JournalFailure);
    result.journal_error.emplace(std::move(detail));
    return result;
}

[[nodiscard]] Error release_error(persist::RecoveryReleaseFailure detail) {
    auto result = error(UnresolvedCohortRecoveryErrorCode::ReleaseFailure);
    result.release_error.emplace(detail);
    return result;
}

[[nodiscard]] cmd::StreamCursor as_stream_cursor(
    persist::LogCursor cursor) noexcept {
    return cmd::StreamCursor{
        cmd::FrameNumber{cursor.through_frame},
        doc::Version{cursor.revision},
        cmd::CommandId{cursor.next_command},
    };
}

[[nodiscard]] bool cursor_not_after(
    persist::LogCursor left,
    persist::LogCursor right) noexcept {
    return left.through_frame <= right.through_frame
        && left.revision <= right.revision
        && left.next_command <= right.next_command;
}

[[nodiscard]] bool valid_cursor(persist::LogCursor cursor) noexcept {
    return cursor.next_command != 0U;
}

[[nodiscard]] bool genesis_cursor(persist::LogCursor cursor) noexcept {
    return cursor.through_frame == 0U && cursor.revision == 0U
        && cursor.next_command == 1U;
}

[[nodiscard]] bool valid_inventory(
    const persist::CommandLogSnapshot& wal,
    const persist::UnresolvedReleaseInventory& inventory) noexcept {
    if (!valid_cursor(inventory.base_cursor)
        || !valid_cursor(inventory.released_frontier)
        || !valid_cursor(inventory.wal_tail)
        || !genesis_cursor(inventory.base_cursor)
        || inventory.released_frame_count > inventory.wal_frame_count
        || inventory.unresolved_frame_count > inventory.wal_frame_count
        || inventory.released_frame_count
            != inventory.wal_frame_count - inventory.unresolved_frame_count
        || inventory.released_wal_bytes > inventory.wal_valid_bytes
        || inventory.unresolved_wal_bytes
            != inventory.wal_valid_bytes - inventory.released_wal_bytes
        || !cursor_not_after(inventory.base_cursor, inventory.released_frontier)
        || !cursor_not_after(inventory.released_frontier, inventory.wal_tail)
        || inventory.wal_valid_bytes < 64U
        || inventory.released_wal_bytes < 64U
        || inventory.release_ledger_valid_bytes < 64U
        || inventory.wal_tail.through_frame != inventory.wal_frame_count
        || inventory.released_frontier.through_frame
            != inventory.released_frame_count) {
        return false;
    }
    const auto ordinal_matches = inventory.released_frame_count
            == std::numeric_limits<std::uint64_t>::max()
        ? inventory.next_release_ordinal == 0U
        : inventory.next_release_ordinal
            == inventory.released_frame_count + 1U;
    if (!ordinal_matches) {
        return false;
    }
    if (inventory.unresolved_frame_count == 0U) {
        if (inventory.unresolved_wal_bytes != 0U
            || inventory.next_unresolved
            || inventory.released_frontier != inventory.wal_tail
            || inventory.released_wal_bytes != inventory.wal_valid_bytes) {
            return false;
        }
    } else {
        if (inventory.unresolved_wal_bytes == 0U
            || !inventory.next_unresolved
            || inventory.released_frontier == inventory.wal_tail) {
            return false;
        }
        const auto& next = *inventory.next_unresolved;
        if (next.before != inventory.released_frontier
            || next.offset != inventory.released_wal_bytes
            || next.bytes == 0U
            || next.bytes > inventory.unresolved_wal_bytes
            || next.before.through_frame
                == std::numeric_limits<std::uint64_t>::max()
            || next.after.through_frame != next.before.through_frame + 1U
            || !valid_cursor(next.before) || !valid_cursor(next.after)
            || !cursor_not_after(next.before, next.after)
            || !cursor_not_after(next.after, inventory.wal_tail)) {
            return false;
        }
    }
    return wal.base_cursor() == as_stream_cursor(inventory.base_cursor)
        && wal.tail_cursor() == as_stream_cursor(inventory.wal_tail)
        && wal.valid_bytes() == inventory.wal_valid_bytes;
}

[[nodiscard]] RecoveryResult<cmd::StreamCheckpoint> build_checkpoint(
    const persist::CommandLogSnapshot& wal,
    const persist::UnresolvedReleaseInventory& inventory) {
    using Result = RecoveryResult<cmd::StreamCheckpoint>;
    cmd::NodeCommandBus node;
    std::optional<Error> callback_error;
    auto replayed = wal.visit_batches_after(
        as_stream_cursor(inventory.base_cursor),
        as_stream_cursor(inventory.released_frontier),
        [&node, &callback_error](
            const persist::ValidatedCommandBatchView& batch) {
            auto received = node.receive_complete_batch(batch.commands, batch.seal);
            if (!received || received.value() != cmd::ReceiveStatus::Prepared) {
                callback_error = error(
                    UnresolvedCohortRecoveryErrorCode::CheckpointBuildFailed);
                return persist::BatchVisitControl::Stop;
            }
            const auto committed = node.commit_frame(batch.seal.frame);
            if (committed.status != cmd::CommitStatus::Committed
                || node.cursor() != batch.after
                || !node.retire_committed_replay_history()) {
                callback_error = error(
                    UnresolvedCohortRecoveryErrorCode::CheckpointBuildFailed);
                return persist::BatchVisitControl::Stop;
            }
            return persist::BatchVisitControl::Continue;
        });
    if (callback_error) {
        return Result::failure(std::move(*callback_error));
    }
    if (!replayed) {
        return Result::failure(command_log_error(
            std::move(replayed.error())));
    }
    if (!replayed.value().completed
        || node.cursor() != as_stream_cursor(inventory.released_frontier)
        || node.document().version().value()
            != inventory.released_frontier.revision) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::CheckpointBuildFailed));
    }
    return Result::success(cmd::StreamCheckpoint{node.cursor(), node.document()});
}

[[nodiscard]] RecoveryResult<RecoveryWalFrame> select_exact_frame(
    const persist::CommandLogSnapshot& wal,
    const persist::UnresolvedReleaseInventory& inventory) {
    using Result = RecoveryResult<RecoveryWalFrame>;
    if (!inventory.next_unresolved) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::NoUnresolvedSuffix));
    }
    const auto expected = *inventory.next_unresolved;
    std::optional<RecoveryWalFrame> selected;
    std::optional<Error> callback_error;
    auto visited = wal.visit_batches_after(
        as_stream_cursor(inventory.released_frontier),
        as_stream_cursor(expected.after),
        [&selected, &callback_error, &expected](
            const persist::ValidatedCommandBatchView& batch) {
            if (selected || batch.identity != expected) {
                callback_error = error(
                    UnresolvedCohortRecoveryErrorCode::SnapshotMismatch);
                return persist::BatchVisitControl::Stop;
            }
            try {
                selected.emplace(RecoveryWalFrame{
                    batch.identity,
                    std::vector<cmd::Command>{
                        batch.commands.begin(), batch.commands.end()},
                    batch.seal,
                });
            } catch (const std::bad_alloc&) {
                callback_error = error(
                    UnresolvedCohortRecoveryErrorCode::AllocationFailure);
                return persist::BatchVisitControl::Stop;
            } catch (const std::length_error&) {
                callback_error = error(
                    UnresolvedCohortRecoveryErrorCode::AllocationFailure);
                return persist::BatchVisitControl::Stop;
            }
            return persist::BatchVisitControl::Continue;
        });
    if (callback_error) {
        return Result::failure(std::move(*callback_error));
    }
    if (!visited) {
        return Result::failure(command_log_error(
            std::move(visited.error())));
    }
    if (!visited.value().completed || visited.value().frames != 1U
        || !selected || selected->identity != expected
        || as_stream_cursor(selected->identity.before)
            != as_stream_cursor(expected.before)) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::SnapshotMismatch));
    }
    return Result::success(std::move(*selected));
}

struct PendingPlan final {
    std::uint64_t ordinal{};
    persist::BeginFrame begin;
    bool closed{};
};

[[nodiscard]] RecoveryResult<RecoveryActivePlan> select_active_plan(
    const persist::CohortJournal& journal,
    const persist::UnresolvedReleaseInventory& inventory,
    const persist::WalFrameIdentity& expected) {
    using Result = RecoveryResult<RecoveryActivePlan>;
    std::vector<PendingPlan> pending;
    std::vector<RecoveryActivePlan> active;
    auto covered_tail = journal.binding().anchor_release_cursor;
    auto covered_bytes = journal.binding().anchor_wal_byte_offset;
    try {
        for (const auto& entry : journal.records()) {
            if (const auto* begin = std::get_if<persist::BeginFrame>(
                    &entry.mutation)) {
                pending.push_back(PendingPlan{entry.ordinal, *begin, false});
                continue;
            }
            if (const auto* bound = std::get_if<persist::BindFrame>(
                    &entry.mutation)) {
                const auto found = std::find_if(
                    pending.begin(),
                    pending.end(),
                    [bound](const PendingPlan& candidate) {
                        return candidate.ordinal == bound->begin_ordinal
                            && candidate.begin.plan_token == bound->begin_token;
                    });
                if (found == pending.end() || found->closed
                    || bound->frame.before != covered_tail
                    || bound->frame.offset != covered_bytes
                    || bound->frame.bytes == 0U
                    || bound->frame.bytes
                        > std::numeric_limits<std::uint64_t>::max()
                            - covered_bytes) {
                    return Result::failure(error(
                        UnresolvedCohortRecoveryErrorCode::JournalMismatch));
                }
                found->closed = true;
                covered_tail = bound->frame.after;
                covered_bytes += bound->frame.bytes;
                active.push_back(RecoveryActivePlan{
                    bound->frame,
                    bound->plan_generation,
                    bound->plan_token,
                    bound->plan_fingerprint,
                    entry.ordinal,
                    found->begin.cohort,
                    found->begin.empty_confirmation,
                    0U,
                });
                continue;
            }
            if (const auto* aborted = std::get_if<persist::AbortFrame>(
                    &entry.mutation)) {
                const auto found = std::find_if(
                    pending.begin(),
                    pending.end(),
                    [aborted](const PendingPlan& candidate) {
                        return candidate.ordinal == aborted->begin_ordinal
                            && candidate.begin.plan_token
                                == aborted->begin_token;
                    });
                if (found == pending.end() || found->closed) {
                    return Result::failure(error(
                        UnresolvedCohortRecoveryErrorCode::JournalMismatch));
                }
                found->closed = true;
                continue;
            }
            if (const auto* adopted = std::get_if<persist::AdoptFrame>(
                    &entry.mutation)) {
                if (adopted->frame.before != covered_tail
                    || adopted->frame.offset != covered_bytes
                    || adopted->frame.bytes == 0U
                    || adopted->frame.bytes
                        > std::numeric_limits<std::uint64_t>::max()
                            - covered_bytes) {
                    return Result::failure(error(
                        UnresolvedCohortRecoveryErrorCode::JournalMismatch));
                }
                covered_tail = adopted->frame.after;
                covered_bytes += adopted->frame.bytes;
                active.push_back(RecoveryActivePlan{
                    adopted->frame,
                    adopted->plan_generation,
                    adopted->plan_token,
                    adopted->plan_fingerprint,
                    entry.ordinal,
                    adopted->cohort,
                    adopted->empty_confirmation,
                    0U,
                });
                continue;
            }
            if (const auto* reconfigured =
                    std::get_if<persist::ReconfigureFrame>(&entry.mutation)) {
                const auto found = std::find_if(
                    active.begin(),
                    active.end(),
                    [reconfigured](const RecoveryActivePlan& candidate) {
                        return candidate.frame == reconfigured->frame;
                    });
                if (found == active.end()) {
                    return Result::failure(error(
                        UnresolvedCohortRecoveryErrorCode::JournalMismatch));
                }
                found->generation = reconfigured->new_plan_generation;
                found->token = reconfigured->new_plan_token;
                found->fingerprint = reconfigured->new_plan_fingerprint;
                found->journal_ordinal = entry.ordinal;
                found->cohort = reconfigured->cohort;
                found->empty_confirmation = reconfigured->empty_confirmation;
                found->last_attempt_generation = 0U;
                continue;
            }
            const auto* attempt = std::get_if<persist::AttemptStarted>(
                &entry.mutation);
            if (attempt == nullptr) {
                return Result::failure(error(
                    UnresolvedCohortRecoveryErrorCode::JournalMismatch));
            }
            const auto found = std::find_if(
                active.begin(),
                active.end(),
                [attempt](const RecoveryActivePlan& candidate) {
                    return candidate.frame == attempt->frame;
                });
            if (found == active.end()) {
                return Result::failure(error(
                    UnresolvedCohortRecoveryErrorCode::JournalMismatch));
            }
            found->last_attempt_generation = attempt->attempt_generation;
        }
    } catch (const std::bad_alloc&) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::AllocationFailure));
    } catch (const std::length_error&) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::AllocationFailure));
    }

    if (covered_tail != inventory.wal_tail
        || covered_bytes != inventory.wal_valid_bytes) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::JournalMismatch));
    }

    const auto first = std::find_if(
        active.begin(),
        active.end(),
        [&expected](const RecoveryActivePlan& candidate) {
            return candidate.frame == expected;
        });
    if (first == active.end()) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::MissingPlan));
    }
    if (std::find_if(
            std::next(first),
            active.end(),
            [&expected](const RecoveryActivePlan& candidate) {
                return candidate.frame == expected;
            }) != active.end()) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::DuplicatePlan));
    }
    if (!persist::is_canonical_recovery_cohort(
            first->cohort,
            std::numeric_limits<std::uint32_t>::max(),
            first->empty_confirmation)
        || first->fingerprint
            != persist::recovery_plan_fingerprint(
                first->generation,
                first->token,
                first->cohort,
                first->empty_confirmation)) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::JournalMismatch));
    }
    return Result::success(std::move(*first));
}

enum class MemberPhase : std::uint8_t {
    Missing,
    Admitted,
    CheckpointInFlight,
    Checkpointed,
    PrepareInFlight,
    Prepared,
};

struct MemberSlot final {
    persist::RecoveryCohortMember member;
    RecoverySessionToken session;
    RecoveryActionToken action;
    MemberPhase phase{MemberPhase::Missing};
};

[[nodiscard]] bool mutation_matches_current_plan(
    const persist::ReconfigureFrame& mutation,
    const RecoveryActivePlan& plan,
    const persist::UnresolvedReleaseInventory& inventory) noexcept {
    return mutation.frame == plan.frame
        && mutation.expected_plan_generation == plan.generation
        && mutation.expected_plan_token == plan.token
        && mutation.expected_plan_fingerprint == plan.fingerprint
        && mutation.expected_plan_journal_ordinal == plan.journal_ordinal
        && mutation.expected_release_frontier == inventory.released_frontier
        && mutation.expected_wal_tail == inventory.wal_tail;
}

} // namespace

namespace detail {

struct UnresolvedCohortRecoveryControl final {
    persist::CommandLogSnapshot wal;
    persist::UnresolvedReleaseInventory inventory;
    persist::CohortJournal* journal{};
    persist::RecoveryReleaseAuthority* release_authority{};
    RecoveryActivePlan plan;
    std::shared_ptr<const cmd::StreamCheckpoint> checkpoint;
    std::shared_ptr<const RecoveryWalFrame> frame;
    std::vector<MemberSlot> members;
    std::optional<RecoveryAttemptRequest> attempt;
    std::uint64_t action_generation{};
    UnresolvedCohortRecoveryState state{
        UnresolvedCohortRecoveryState::Ready};

    UnresolvedCohortRecoveryControl(
        persist::CommandLogSnapshot captured_wal,
        persist::UnresolvedReleaseInventory captured_inventory,
        persist::CohortJournal& captured_journal,
        persist::RecoveryReleaseAuthority& authority,
        RecoveryActivePlan active,
        std::shared_ptr<const cmd::StreamCheckpoint> exact_checkpoint,
        std::shared_ptr<const RecoveryWalFrame> exact_frame) noexcept
        : wal(std::move(captured_wal)),
          inventory(std::move(captured_inventory)),
          journal(&captured_journal),
          release_authority(&authority),
          plan(std::move(active)),
          checkpoint(std::move(exact_checkpoint)),
          frame(std::move(exact_frame)) {}
};

} // namespace detail

namespace {

void reset_attempt(detail::UnresolvedCohortRecoveryControl& control) noexcept {
    control.members.clear();
    control.attempt.reset();
    control.state = UnresolvedCohortRecoveryState::Ready;
}

[[nodiscard]] bool all_admitted(
    const detail::UnresolvedCohortRecoveryControl& control) noexcept {
    return std::all_of(
        control.members.begin(),
        control.members.end(),
        [](const MemberSlot& member) {
            return member.phase != MemberPhase::Missing;
        });
}

[[nodiscard]] bool all_checkpointed_or_later(
    const detail::UnresolvedCohortRecoveryControl& control) noexcept {
    return std::all_of(
        control.members.begin(),
        control.members.end(),
        [](const MemberSlot& member) {
            return member.phase == MemberPhase::Checkpointed
                || member.phase == MemberPhase::PrepareInFlight
                || member.phase == MemberPhase::Prepared;
        });
}

[[nodiscard]] bool all_prepared(
    const detail::UnresolvedCohortRecoveryControl& control) noexcept {
    return std::all_of(
        control.members.begin(),
        control.members.end(),
        [](const MemberSlot& member) {
            return member.phase == MemberPhase::Prepared;
        });
}

[[nodiscard]] RecoveryResult<RecoveryActionToken> next_action_token(
    detail::UnresolvedCohortRecoveryControl& control) {
    using Result = RecoveryResult<RecoveryActionToken>;
    if (control.action_generation
        == std::numeric_limits<std::uint64_t>::max()) {
        control.state = UnresolvedCohortRecoveryState::Faulted;
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::ActionExhausted));
    }
    ++control.action_generation;
    return Result::success(RecoveryActionToken{control.action_generation});
}

} // namespace

core::Result<UnresolvedCohortRecovery, UnresolvedCohortRecoveryError>
UnresolvedCohortRecovery::create(
    persist::CommandLogSnapshot wal,
    persist::UnresolvedReleaseInventory inventory,
    persist::CohortJournal& journal,
    persist::RecoveryReleaseAuthority& release_authority) {
    using Result = core::Result<UnresolvedCohortRecovery, Error>;
    try {
        if (!valid_inventory(wal, inventory)) {
            return Result::failure(error(
                genesis_cursor(inventory.base_cursor)
                    ? UnresolvedCohortRecoveryErrorCode::InvalidInventory
                    : UnresolvedCohortRecoveryErrorCode::NonGenesisBase));
        }
        if (inventory.unresolved_frame_count == 0U
            || inventory.unresolved_wal_bytes == 0U
            || !inventory.next_unresolved
            || inventory.wal_tail == inventory.released_frontier) {
            return Result::failure(error(
                UnresolvedCohortRecoveryErrorCode::NoUnresolvedSuffix));
        }
        if (release_authority.faulted()) {
            return Result::failure(error(
                UnresolvedCohortRecoveryErrorCode::ReleaseFailure));
        }
        const auto& binding = journal.binding();
        if (binding.wal_base != inventory.base_cursor
            || !cursor_not_after(
                binding.anchor_release_cursor, inventory.released_frontier)
            || binding.anchor_wal_byte_offset > inventory.released_wal_bytes
            || binding.release_decision_count > inventory.released_frame_count
            || binding.release_ledger_valid_bytes
                > inventory.release_ledger_valid_bytes) {
            return Result::failure(error(
                UnresolvedCohortRecoveryErrorCode::JournalMismatch));
        }

        auto checkpoint_result = build_checkpoint(wal, inventory);
        if (!checkpoint_result) {
            return Result::failure(std::move(checkpoint_result.error()));
        }
        auto frame_result = select_exact_frame(wal, inventory);
        if (!frame_result) {
            return Result::failure(std::move(frame_result.error()));
        }
        auto plan_result = select_active_plan(
            journal, inventory, frame_result.value().identity);
        if (!plan_result) {
            return Result::failure(std::move(plan_result.error()));
        }

        auto checkpoint = std::make_shared<const cmd::StreamCheckpoint>(
            std::move(checkpoint_result).value());
        auto frame = std::make_shared<const RecoveryWalFrame>(
            std::move(frame_result).value());
        auto control = std::make_unique<
            detail::UnresolvedCohortRecoveryControl>(
            std::move(wal),
            std::move(inventory),
            journal,
            release_authority,
            std::move(plan_result).value(),
            std::move(checkpoint),
            std::move(frame));
        return Result::success(UnresolvedCohortRecovery{std::move(control)});
    } catch (const std::bad_alloc&) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::AllocationFailure));
    } catch (const std::length_error&) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::AllocationFailure));
    }
}

UnresolvedCohortRecovery::UnresolvedCohortRecovery(
    std::unique_ptr<detail::UnresolvedCohortRecoveryControl> control) noexcept
    : control_(std::move(control)) {}

UnresolvedCohortRecovery::UnresolvedCohortRecovery(
    UnresolvedCohortRecovery&&) noexcept = default;

UnresolvedCohortRecovery& UnresolvedCohortRecovery::operator=(
    UnresolvedCohortRecovery&&) noexcept = default;

UnresolvedCohortRecovery::~UnresolvedCohortRecovery() = default;

core::Result<persist::CohortJournalReceipt, UnresolvedCohortRecoveryError>
UnresolvedCohortRecovery::reconfigure(
    const persist::ReconfigureFrame& mutation) {
    using Result = core::Result<persist::CohortJournalReceipt, Error>;
    if (control_->state != UnresolvedCohortRecoveryState::Ready) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::InvalidState));
    }

    const auto exact_retry = std::any_of(
        control_->journal->records().begin(),
        control_->journal->records().end(),
        [&mutation](const persist::CohortJournalRecord& record) {
            const auto* existing =
                std::get_if<persist::ReconfigureFrame>(&record.mutation);
            return existing != nullptr && *existing == mutation;
        });
    if (!exact_retry
        && !mutation_matches_current_plan(
            mutation, control_->plan, control_->inventory)) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::JournalMismatch));
    }

    bool append_completed = false;
    try {
        const persist::CohortJournalMutation encoded{mutation};
        auto appended = control_->journal->append(
            control_->journal->next_ordinal(), encoded);
        if (!appended) {
            control_->state = UnresolvedCohortRecoveryState::Faulted;
            return Result::failure(journal_error(std::move(appended.error())));
        }
        if (!appended.value().durable()) {
            control_->state = UnresolvedCohortRecoveryState::Faulted;
            return Result::failure(error(
                UnresolvedCohortRecoveryErrorCode::JournalFailure));
        }
        append_completed = true;
        auto refreshed = select_active_plan(
            *control_->journal, control_->inventory, control_->frame->identity);
        if (!refreshed) {
            control_->state = UnresolvedCohortRecoveryState::Faulted;
            return Result::failure(std::move(refreshed.error()));
        }
        control_->plan = std::move(refreshed).value();
        return Result::success(std::move(appended).value());
    } catch (const std::bad_alloc&) {
        if (append_completed) {
            control_->state = UnresolvedCohortRecoveryState::Faulted;
        }
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::AllocationFailure));
    } catch (const std::length_error&) {
        if (append_completed) {
            control_->state = UnresolvedCohortRecoveryState::Faulted;
        }
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::AllocationFailure));
    }
}

core::Result<persist::CohortJournalReceipt, UnresolvedCohortRecoveryError>
UnresolvedCohortRecovery::start_attempt(
    const RecoveryAttemptRequest& request) {
    using Result = core::Result<persist::CohortJournalReceipt, Error>;
    if (control_->state != UnresolvedCohortRecoveryState::Ready) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::InvalidState));
    }
    if (!static_cast<bool>(request.attempt_token)
        || !static_cast<bool>(request.director_epoch)
        || !static_cast<bool>(request.session_namespace)
        || control_->plan.last_attempt_generation
            == std::numeric_limits<std::uint64_t>::max()) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::InvalidAttempt));
    }
    auto exact_frame = select_exact_frame(control_->wal, control_->inventory);
    auto exact_plan = select_active_plan(
        *control_->journal, control_->inventory, control_->frame->identity);
    if (!exact_frame || exact_frame.value().identity != control_->frame->identity
        || !exact_plan || exact_plan.value().frame != control_->frame->identity
        || exact_plan.value().generation != control_->plan.generation
        || exact_plan.value().token != control_->plan.token
        || exact_plan.value().fingerprint != control_->plan.fingerprint
        || exact_plan.value().journal_ordinal != control_->plan.journal_ordinal) {
        control_->state = UnresolvedCohortRecoveryState::Faulted;
        if (!exact_frame) {
            return Result::failure(std::move(exact_frame.error()));
        }
        if (!exact_plan) {
            return Result::failure(std::move(exact_plan.error()));
        }
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::SnapshotMismatch));
    }
    std::vector<MemberSlot> prepared_members;
    try {
        prepared_members.reserve(control_->plan.cohort.size());
        for (const auto& member : control_->plan.cohort) {
            prepared_members.push_back(MemberSlot{
                member,
                RecoverySessionToken{},
                RecoveryActionToken{},
                MemberPhase::Missing,
            });
        }
    } catch (const std::bad_alloc&) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::AllocationFailure));
    } catch (const std::length_error&) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::AllocationFailure));
    }
    const auto attempt_generation = control_->plan.last_attempt_generation + 1U;
    const persist::AttemptStarted mutation{
        control_->frame->identity,
        attempt_generation,
        request.attempt_token,
        request.director_epoch,
        request.session_namespace,
        control_->inventory.released_frontier,
        control_->inventory.wal_tail,
        control_->plan.generation,
        control_->plan.token,
        control_->plan.journal_ordinal,
    };
    const auto expected_journal_ordinal = control_->journal->next_ordinal();
    auto appended = control_->journal->append(
        expected_journal_ordinal,
        persist::CohortJournalMutation{mutation});
    if (!appended) {
        control_->state = UnresolvedCohortRecoveryState::Faulted;
        return Result::failure(journal_error(std::move(appended.error())));
    }
    if (!appended.value().durable() || appended.value().replayed()
        || appended.value().ordinal() != expected_journal_ordinal) {
        control_->state = UnresolvedCohortRecoveryState::Faulted;
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::JournalFailure));
    }
    static_assert(std::is_nothrow_move_assignable_v<std::vector<MemberSlot>>);
    control_->members = std::move(prepared_members);
    control_->attempt = request;
    control_->plan.last_attempt_generation = attempt_generation;
    control_->state = control_->members.empty()
        ? UnresolvedCohortRecoveryState::PreparedQuorum
        : UnresolvedCohortRecoveryState::Admitting;
    return Result::success(std::move(appended).value());
}

core::Result<std::monostate, UnresolvedCohortRecoveryError>
UnresolvedCohortRecovery::admit(
    const persist::RecoveryCohortMember& member,
    RecoverySessionToken session) {
    using Result = core::Result<std::monostate, Error>;
    if (control_->state != UnresolvedCohortRecoveryState::Admitting
        || !session || !control_->attempt) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::InvalidState));
    }
    const auto found = std::find_if(
        control_->members.begin(),
        control_->members.end(),
        [&member](const MemberSlot& candidate) {
            return candidate.member == member;
        });
    if (found == control_->members.end()) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::MemberNotInCohort));
    }
    if (found->phase != MemberPhase::Missing) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::DuplicateMember));
    }
    if (std::any_of(
            control_->members.begin(),
            control_->members.end(),
            [session](const MemberSlot& candidate) {
                return candidate.session == session;
            })) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::DuplicateSession));
    }
    found->session = session;
    found->phase = MemberPhase::Admitted;
    if (all_admitted(*control_)) {
        control_->state = UnresolvedCohortRecoveryState::Checkpointing;
    }
    return Result::success(std::monostate{});
}

core::Result<std::optional<RecoveryAction>, UnresolvedCohortRecoveryError>
UnresolvedCohortRecovery::take_next_action() {
    using Result = core::Result<std::optional<RecoveryAction>, Error>;
    if (!control_->attempt) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::InvalidState));
    }
    if (control_->state == UnresolvedCohortRecoveryState::Admitting) {
        return Result::success(std::nullopt);
    }
    if (control_->state == UnresolvedCohortRecoveryState::Checkpointing) {
        const auto candidate = std::find_if(
            control_->members.begin(),
            control_->members.end(),
            [](const MemberSlot& member) {
                return member.phase == MemberPhase::Admitted;
            });
        if (candidate == control_->members.end()) {
            return Result::success(std::nullopt);
        }
        auto action = next_action_token(*control_);
        if (!action) {
            return Result::failure(std::move(action.error()));
        }
        candidate->action = action.value();
        candidate->phase = MemberPhase::CheckpointInFlight;
        return Result::success(RecoveryAction{RecoveryCheckpointAction{
            candidate->action,
            candidate->session,
            candidate->member,
            *control_->attempt,
            control_->checkpoint,
        }});
    }
    if (control_->state == UnresolvedCohortRecoveryState::Preparing) {
        const auto candidate = std::find_if(
            control_->members.begin(),
            control_->members.end(),
            [](const MemberSlot& member) {
                return member.phase == MemberPhase::Checkpointed;
            });
        if (candidate == control_->members.end()) {
            return Result::success(std::nullopt);
        }
        auto action = next_action_token(*control_);
        if (!action) {
            return Result::failure(std::move(action.error()));
        }
        candidate->action = action.value();
        candidate->phase = MemberPhase::PrepareInFlight;
        return Result::success(RecoveryAction{RecoveryPrepareAction{
            candidate->action,
            candidate->session,
            candidate->member,
            *control_->attempt,
            control_->frame,
        }});
    }
    if (control_->state == UnresolvedCohortRecoveryState::PreparedQuorum) {
        return Result::success(std::nullopt);
    }
    return Result::failure(error(
        UnresolvedCohortRecoveryErrorCode::InvalidState));
}

core::Result<std::monostate, UnresolvedCohortRecoveryError>
UnresolvedCohortRecovery::complete_checkpoint(
    const RecoveryCheckpointCompletion& completion) {
    using Result = core::Result<std::monostate, Error>;
    if (control_->state != UnresolvedCohortRecoveryState::Checkpointing) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::InvalidState));
    }
    const auto member = std::find_if(
        control_->members.begin(),
        control_->members.end(),
        [&completion](const MemberSlot& candidate) {
            return candidate.phase == MemberPhase::CheckpointInFlight
                && candidate.action == completion.action
                && candidate.session == completion.session;
        });
    if (member == control_->members.end()) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::ActionMismatch));
    }
    if (!completion.activated
        || completion.activated_cursor != control_->checkpoint->cursor) {
        reset_attempt(*control_);
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::CheckpointMismatch));
    }
    member->phase = MemberPhase::Checkpointed;
    member->action = {};
    if (all_checkpointed_or_later(*control_)) {
        control_->state = UnresolvedCohortRecoveryState::Preparing;
    }
    return Result::success(std::monostate{});
}

core::Result<std::monostate, UnresolvedCohortRecoveryError>
UnresolvedCohortRecovery::complete_prepare(
    const RecoveryPrepareCompletion& completion) {
    using Result = core::Result<std::monostate, Error>;
    if (control_->state != UnresolvedCohortRecoveryState::Preparing) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::InvalidState));
    }
    const auto member = std::find_if(
        control_->members.begin(),
        control_->members.end(),
        [&completion](const MemberSlot& candidate) {
            return candidate.phase == MemberPhase::PrepareInFlight
                && candidate.action == completion.action
                && candidate.session == completion.session;
        });
    if (member == control_->members.end()) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::ActionMismatch));
    }
    if (completion.kind != RecoveryFrameAcknowledgementKind::Prepared
        || completion.frame != control_->frame->identity
        || completion.before != as_stream_cursor(control_->frame->identity.before)
        || completion.after != as_stream_cursor(control_->frame->identity.after)) {
        reset_attempt(*control_);
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::AcknowledgementMismatch));
    }
    member->phase = MemberPhase::Prepared;
    member->action = {};
    if (all_prepared(*control_)) {
        control_->state = UnresolvedCohortRecoveryState::PreparedQuorum;
    }
    return Result::success(std::monostate{});
}

core::Result<std::monostate, UnresolvedCohortRecoveryError>
UnresolvedCohortRecovery::fail_attempt() noexcept {
    using Result = core::Result<std::monostate, Error>;
    if (control_->state == UnresolvedCohortRecoveryState::Ready
        || control_->state == UnresolvedCohortRecoveryState::PersistingRelease
        || control_->state == UnresolvedCohortRecoveryState::RestartRequired
        || control_->state == UnresolvedCohortRecoveryState::Faulted) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::InvalidState));
    }
    reset_attempt(*control_);
    return Result::success(std::monostate{});
}

core::Result<persist::DurableReleaseReceipt, UnresolvedCohortRecoveryError>
UnresolvedCohortRecovery::persist_release() {
    using Result = core::Result<persist::DurableReleaseReceipt, Error>;
    if (control_->state != UnresolvedCohortRecoveryState::PreparedQuorum
        || !control_->attempt || !all_prepared(*control_)) {
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::CohortIncomplete));
    }
    control_->state = UnresolvedCohortRecoveryState::PersistingRelease;
    auto persisted = control_->release_authority->persist_exact(
        control_->wal,
        control_->inventory,
        control_->frame->identity,
        control_->inventory.next_release_ordinal);
    if (!persisted) {
        control_->state = UnresolvedCohortRecoveryState::Faulted;
        return Result::failure(release_error(persisted.error()));
    }
    auto receipt = std::move(persisted).value();
    if (!receipt.durable()
        || receipt.ordinal() != control_->inventory.next_release_ordinal
        || receipt.frame() != control_->frame->identity) {
        control_->state = UnresolvedCohortRecoveryState::Faulted;
        return Result::failure(error(
            UnresolvedCohortRecoveryErrorCode::ReleaseReceiptMismatch));
    }
    control_->state = UnresolvedCohortRecoveryState::RestartRequired;
    return Result::success(std::move(receipt));
}

UnresolvedCohortRecoveryState UnresolvedCohortRecovery::state() const noexcept {
    return control_->state;
}

const persist::UnresolvedReleaseInventory&
UnresolvedCohortRecovery::inventory() const noexcept {
    return control_->inventory;
}

const RecoveryActivePlan& UnresolvedCohortRecovery::active_plan() const noexcept {
    return control_->plan;
}

const cmd::StreamCheckpoint& UnresolvedCohortRecovery::checkpoint() const noexcept {
    return *control_->checkpoint;
}

const RecoveryWalFrame& UnresolvedCohortRecovery::frame() const noexcept {
    return *control_->frame;
}

UnresolvedCohortRecoveryReport UnresolvedCohortRecovery::report() const noexcept {
    UnresolvedCohortRecoveryReport result{
        control_->state,
        control_->inventory.released_frontier,
        control_->inventory.wal_tail,
        control_->frame->identity,
        control_->inventory.next_release_ordinal,
        control_->plan.generation,
        control_->plan.last_attempt_generation,
        control_->members.size(),
        0U,
        0U,
        0U,
    };
    for (const auto& member : control_->members) {
        if (member.phase != MemberPhase::Missing) {
            ++result.admitted;
        }
        if (member.phase == MemberPhase::Checkpointed
            || member.phase == MemberPhase::PrepareInFlight
            || member.phase == MemberPhase::Prepared) {
            ++result.checkpointed;
        }
        if (member.phase == MemberPhase::Prepared) {
            ++result.prepared;
        }
    }
    return result;
}

} // namespace mediaserver::net
