#include <mediaserver/cmd/node_command_bus.hpp>

#include <algorithm>
#include <cassert>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <mediaserver/cmd/reducer.hpp>

namespace mediaserver::cmd {
namespace {

[[nodiscard]] FrameNumber next_frame(FrameNumber frame) noexcept {
    return FrameNumber{frame.value() + 1U};
}

enum class PreparationProbeStatus {
    Incomplete,
    Prepared,
    WouldFault,
};

struct PreparationProbe final {
    PreparationProbeStatus status{PreparationProbeStatus::Incomplete};
    doc::Root prepared;
    CommandId next_command{};
    std::optional<BusFault> fault;
};

[[nodiscard]] PreparationProbe incomplete_preparation() noexcept {
    return PreparationProbe{};
}

[[nodiscard]] PreparationProbe preparation_fault(
    FaultCode code,
    FrameNumber frame,
    std::optional<CommandId> command = std::nullopt) noexcept {
    return PreparationProbe{
        PreparationProbeStatus::WouldFault,
        {},
        {},
        BusFault{code, frame, command},
    };
}

[[nodiscard]] PreparationProbe probe_preparation(
    const FrameSeal& seal,
    const std::map<CommandId, Command>& commands,
    const doc::Root& preparation_head,
    CommandId preparation_next_command) {
    if (commands.size() < seal.commands.size()) {
        return incomplete_preparation();
    }
    if (commands.size() > seal.commands.size()) {
        return preparation_fault(FaultCode::UnexpectedCommand, seal.frame);
    }
    if (seal.base != preparation_head.version()) {
        return preparation_fault(FaultCode::SealBaseMismatch, seal.frame);
    }

    auto working = preparation_head;
    auto expected_command = preparation_next_command;
    for (const auto& descriptor : seal.commands) {
        if (descriptor.id != expected_command) {
            return preparation_fault(
                FaultCode::MalformedSeal, seal.frame, descriptor.id);
        }
        if (expected_command.value() == std::numeric_limits<core::u64>::max()) {
            return preparation_fault(
                FaultCode::MalformedSeal, seal.frame, descriptor.id);
        }
        expected_command = CommandId{expected_command.value() + 1U};
        const auto command = commands.find(descriptor.id);
        if (command == commands.end()) {
            return incomplete_preparation();
        }
        if (fingerprint(command->second) != descriptor.fingerprint) {
            return preparation_fault(
                FaultCode::FingerprintMismatch, seal.frame, descriptor.id);
        }
        if (command->second.apply_at != seal.frame) {
            return preparation_fault(
                FaultCode::CommandFrameMismatch, seal.frame, descriptor.id);
        }
        if (command->second.base != working.version()) {
            return preparation_fault(
                FaultCode::CommandBaseMismatch, seal.frame, descriptor.id);
        }

        auto applied = apply_payload(working, command->second.forward);
        if (!applied) {
            return preparation_fault(
                FaultCode::PayloadRejected, seal.frame, descriptor.id);
        }
        if (applied.value().version() != command->second.result) {
            return preparation_fault(
                FaultCode::CommandResultMismatch, seal.frame, descriptor.id);
        }
        working = std::move(applied).value();
    }

    if (working.version() != seal.result) {
        return preparation_fault(FaultCode::SealResultMismatch, seal.frame);
    }
    return PreparationProbe{
        PreparationProbeStatus::Prepared,
        std::move(working),
        expected_command,
        std::nullopt,
    };
}

[[nodiscard]] std::optional<BusFault> validate_complete_batch_identity(
    std::span<const Command> commands,
    const FrameSeal& seal) noexcept {
    if (commands.size() != seal.commands.size()) {
        return BusFault{FaultCode::MalformedSeal, seal.frame, std::nullopt};
    }
    if (commands.empty()) {
        if (seal.base != seal.result) {
            return BusFault{FaultCode::SealResultMismatch, seal.frame, std::nullopt};
        }
        return std::nullopt;
    }

    auto expected_id = commands.front().id.value();
    auto expected_revision = seal.base.value();
    if (expected_id == 0U) {
        return BusFault{
            FaultCode::MalformedSeal, seal.frame, commands.front().id};
    }
    for (std::size_t index = 0U; index < commands.size(); ++index) {
        const auto& command = commands[index];
        const auto& descriptor = seal.commands[index];
        if (expected_id == std::numeric_limits<core::u64>::max()
            || command.id.value() != expected_id || descriptor.id != command.id) {
            return BusFault{FaultCode::MalformedSeal, seal.frame, command.id};
        }
        if (descriptor.fingerprint != fingerprint(command)) {
            return BusFault{
                FaultCode::FingerprintMismatch, seal.frame, command.id};
        }
        if (command.apply_at != seal.frame) {
            return BusFault{
                FaultCode::CommandFrameMismatch, seal.frame, command.id};
        }
        if (command.base.value() != expected_revision) {
            return BusFault{
                FaultCode::CommandBaseMismatch, seal.frame, command.id};
        }
        if (expected_revision == std::numeric_limits<core::u64>::max()
            || command.result.value() != expected_revision + 1U) {
            return BusFault{
                FaultCode::CommandResultMismatch, seal.frame, command.id};
        }
        ++expected_id;
        ++expected_revision;
    }
    if (seal.result.value() != expected_revision) {
        return BusFault{FaultCode::SealResultMismatch, seal.frame, std::nullopt};
    }
    return std::nullopt;
}

[[nodiscard]] BatchReceiveResult batch_status(ReceiveStatus status) noexcept {
    return BatchReceiveResult::success(status);
}

[[nodiscard]] BatchReceiveResult batch_would_fault(BusFault fault) noexcept {
    return BatchReceiveResult::failure(BatchReceiveError{
        BatchReceiveErrorCode::WouldFault,
        fault,
    });
}

[[nodiscard]] BatchReceiveResult batch_allocation_failed() noexcept {
    return BatchReceiveResult::failure(BatchReceiveError{
        BatchReceiveErrorCode::AllocationFailed,
        std::nullopt,
    });
}

} // namespace

NodeCommandBus::NodeCommandBus(
    doc::Root initial,
    FrameNumber committed_through,
    CommandId next_command)
    : document_(std::move(initial)),
      preparation_head_(document_),
      committed_through_(committed_through),
      next_to_prepare_(next_frame(committed_through)),
      next_command_(next_command),
      preparation_next_command_(next_command),
      retained_roots_{document_} {}

void NodeCommandBus::latch_fault(
    FaultCode code,
    FrameNumber frame,
    std::optional<CommandId> command) noexcept {
    if (!fault_) {
        fault_ = BusFault{code, frame, command};
    }
}

const SealedCommand* NodeCommandBus::find_descriptor(
    const FrameSeal& seal,
    CommandId id) noexcept {
    const auto descriptor = std::lower_bound(
        seal.commands.begin(),
        seal.commands.end(),
        id,
        [](const SealedCommand& candidate, CommandId expected) {
            return candidate.id < expected;
        });
    return descriptor != seal.commands.end() && descriptor->id == id
        ? &*descriptor
        : nullptr;
}

ReceiveStatus NodeCommandBus::receive(Command command) {
    if (prepared_checkpoint_) {
        return ReceiveStatus::CheckpointPending;
    }
    if (fault_) {
        return ReceiveStatus::NeedsResync;
    }

    const auto seen = seen_commands_.find(command.id);
    if (seen != seen_commands_.end()) {
        if (seen->second == command) {
            return ReceiveStatus::Duplicate;
        }
        latch_fault(FaultCode::ConflictingCommand, command.apply_at, command.id);
        return ReceiveStatus::NeedsResync;
    }
    if (command.id < next_command_) {
        return ReceiveStatus::Stale;
    }
    if (command.apply_at <= committed_through_) {
        latch_fault(FaultCode::LateCommand, command.apply_at, command.id);
        return ReceiveStatus::NeedsResync;
    }

    auto& state = frames_[command.apply_at];
    if (state.seal) {
        const auto* descriptor = find_descriptor(*state.seal, command.id);
        if (descriptor == nullptr) {
            latch_fault(FaultCode::UnexpectedCommand, command.apply_at, command.id);
            return ReceiveStatus::NeedsResync;
        }
        if (descriptor->fingerprint != fingerprint(command)) {
            latch_fault(FaultCode::FingerprintMismatch, command.apply_at, command.id);
            return ReceiveStatus::NeedsResync;
        }
    }

    const auto prepared_before = prepared_order_.size();
    seen_commands_.emplace(command.id, command);
    state.commands.emplace(command.id, std::move(command));
    try_prepare();
    if (fault_) {
        return ReceiveStatus::NeedsResync;
    }
    return prepared_order_.size() > prepared_before
        ? ReceiveStatus::Prepared
        : ReceiveStatus::Accepted;
}

ReceiveStatus NodeCommandBus::receive(FrameSeal seal) {
    if (prepared_checkpoint_) {
        return ReceiveStatus::CheckpointPending;
    }
    if (fault_) {
        return ReceiveStatus::NeedsResync;
    }

    auto existing_state = frames_.find(seal.frame);
    if (existing_state != frames_.end() && existing_state->second.seal) {
        if (*existing_state->second.seal == seal) {
            return ReceiveStatus::Duplicate;
        }
        latch_fault(FaultCode::ConflictingSeal, seal.frame);
        return ReceiveStatus::NeedsResync;
    }
    if (seal.frame <= committed_through_) {
        return ReceiveStatus::Stale;
    }

    for (std::size_t index = 0U; index < seal.commands.size(); ++index) {
        if (index > 0U && seal.commands[index - 1U].id >= seal.commands[index].id) {
            latch_fault(FaultCode::MalformedSeal, seal.frame);
            return ReceiveStatus::NeedsResync;
        }
        if (seal.commands[index].id < next_command_) {
            latch_fault(FaultCode::MalformedSeal, seal.frame, seal.commands[index].id);
            return ReceiveStatus::NeedsResync;
        }
    }

    auto& state = frames_[seal.frame];
    for (const auto& [id, command] : state.commands) {
        const auto* descriptor = find_descriptor(seal, id);
        if (descriptor == nullptr) {
            latch_fault(FaultCode::UnexpectedCommand, seal.frame, id);
            return ReceiveStatus::NeedsResync;
        }
        if (descriptor->fingerprint != fingerprint(command)) {
            latch_fault(FaultCode::FingerprintMismatch, seal.frame, id);
            return ReceiveStatus::NeedsResync;
        }
    }

    const auto prepared_before = prepared_order_.size();
    state.seal = std::move(seal);
    try_prepare();
    if (fault_) {
        return ReceiveStatus::NeedsResync;
    }
    return prepared_order_.size() > prepared_before
        ? ReceiveStatus::Prepared
        : ReceiveStatus::Accepted;
}

BatchReceiveResult NodeCommandBus::receive_complete_batch(
    std::span<const Command> commands,
    const FrameSeal& seal) noexcept {
    if (prepared_checkpoint_) {
        return batch_status(ReceiveStatus::CheckpointPending);
    }
    if (fault_) {
        return batch_status(ReceiveStatus::NeedsResync);
    }
    if (const auto invalid = validate_complete_batch_identity(commands, seal)) {
        return batch_would_fault(*invalid);
    }

    struct PreparedUpdate final {
        FrameNumber frame;
        doc::Root prepared;
    };

    try {
        const auto existing_frame = frames_.find(seal.frame);
        FrameState staged_frame = existing_frame == frames_.end()
            ? FrameState{}
            : existing_frame->second;
        std::map<CommandId, Command> staged_seen;
        bool changed = false;

        for (const auto& command : commands) {
            const auto seen = seen_commands_.find(command.id);
            const auto buffered = staged_frame.commands.find(command.id);
            if (seen != seen_commands_.end()) {
                if (seen->second != command || buffered == staged_frame.commands.end()
                    || buffered->second != command) {
                    return batch_would_fault(BusFault{
                        FaultCode::ConflictingCommand,
                        command.apply_at,
                        command.id,
                    });
                }
                continue;
            }
            if (command.id < next_command_) {
                return batch_status(ReceiveStatus::Stale);
            }
            if (command.apply_at <= committed_through_) {
                return batch_would_fault(BusFault{
                    FaultCode::LateCommand,
                    command.apply_at,
                    command.id,
                });
            }
            if (buffered != staged_frame.commands.end()
                && buffered->second != command) {
                return batch_would_fault(BusFault{
                    FaultCode::ConflictingCommand,
                    command.apply_at,
                    command.id,
                });
            }
            if (staged_frame.seal) {
                const auto* descriptor = find_descriptor(*staged_frame.seal, command.id);
                if (descriptor == nullptr) {
                    return batch_would_fault(BusFault{
                        FaultCode::UnexpectedCommand,
                        command.apply_at,
                        command.id,
                    });
                }
                if (descriptor->fingerprint != fingerprint(command)) {
                    return batch_would_fault(BusFault{
                        FaultCode::FingerprintMismatch,
                        command.apply_at,
                        command.id,
                    });
                }
            }

            const auto [seen_position, seen_inserted] =
                staged_seen.emplace(command.id, command);
            static_cast<void>(seen_position);
            assert(seen_inserted);
            if (buffered == staged_frame.commands.end()) {
                const auto [frame_position, frame_inserted] =
                    staged_frame.commands.emplace(command.id, command);
                static_cast<void>(frame_position);
                assert(frame_inserted);
            }
            changed = true;
        }

        if (staged_frame.seal) {
            if (*staged_frame.seal != seal) {
                return batch_would_fault(BusFault{
                    FaultCode::ConflictingSeal,
                    seal.frame,
                    std::nullopt,
                });
            }
        } else {
            if (seal.frame <= committed_through_) {
                return batch_status(ReceiveStatus::Stale);
            }
            for (const auto& descriptor : seal.commands) {
                if (descriptor.id < next_command_) {
                    return batch_would_fault(BusFault{
                        FaultCode::MalformedSeal,
                        seal.frame,
                        descriptor.id,
                    });
                }
            }
            for (const auto& [id, command] : staged_frame.commands) {
                const auto* descriptor = find_descriptor(seal, id);
                if (descriptor == nullptr) {
                    return batch_would_fault(BusFault{
                        FaultCode::UnexpectedCommand,
                        seal.frame,
                        id,
                    });
                }
                if (descriptor->fingerprint != fingerprint(command)) {
                    return batch_would_fault(BusFault{
                        FaultCode::FingerprintMismatch,
                        seal.frame,
                        id,
                    });
                }
            }
            staged_frame.seal = seal;
            changed = true;
        }

        if (!changed) {
            return batch_status(ReceiveStatus::Duplicate);
        }
        if (seal.frame < next_to_prepare_) {
            return batch_would_fault(BusFault{
                FaultCode::LateSeal,
                seal.frame,
                std::nullopt,
            });
        }

        std::vector<PreparedUpdate> prepared_updates;
        auto staged_preparation_head = preparation_head_;
        auto staged_next_command = preparation_next_command_;
        auto staged_next_frame = next_to_prepare_;
        if (seal.frame == next_to_prepare_) {
            while (true) {
                const FrameState* candidate = nullptr;
                if (staged_next_frame == seal.frame) {
                    candidate = &staged_frame;
                } else {
                    const auto buffered = frames_.find(staged_next_frame);
                    if (buffered == frames_.end()) {
                        break;
                    }
                    candidate = &buffered->second;
                }
                if (!candidate->seal) {
                    break;
                }

                auto probe = probe_preparation(
                    *candidate->seal,
                    candidate->commands,
                    staged_preparation_head,
                    staged_next_command);
                if (probe.status == PreparationProbeStatus::Incomplete) {
                    break;
                }
                if (probe.status == PreparationProbeStatus::WouldFault) {
                    assert(probe.fault.has_value());
                    return batch_would_fault(*probe.fault);
                }
                staged_preparation_head = probe.prepared;
                staged_next_command = probe.next_command;
                prepared_updates.push_back(PreparedUpdate{
                    staged_next_frame,
                    std::move(probe.prepared),
                });
                if (staged_next_frame.value()
                    == std::numeric_limits<core::u64>::max()) {
                    break;
                }
                staged_next_frame = next_frame(staged_next_frame);
            }
        }

        std::map<FrameNumber, FrameState> staged_new_frame;
        if (existing_frame == frames_.end()) {
            const auto [position, inserted] = staged_new_frame.emplace(
                seal.frame, std::move(staged_frame));
            static_cast<void>(position);
            assert(inserted);
        }

        if (prepared_updates.size()
            > prepared_order_.max_size() - prepared_order_.size()) {
            return batch_allocation_failed();
        }
        prepared_order_.reserve(
            prepared_order_.size() + prepared_updates.size());

        if (existing_frame == frames_.end()) {
            auto node = staged_new_frame.extract(staged_new_frame.begin());
            const auto inserted = frames_.insert(std::move(node));
            assert(inserted.inserted);
        } else {
            static_assert(std::is_nothrow_move_assignable_v<FrameState>);
            existing_frame->second = std::move(staged_frame);
        }

        while (!staged_seen.empty()) {
            auto node = staged_seen.extract(staged_seen.begin());
            const auto inserted = seen_commands_.insert(std::move(node));
            assert(inserted.inserted);
        }

        for (auto& update : prepared_updates) {
            const auto frame = frames_.find(update.frame);
            assert(frame != frames_.end());
            assert(!frame->second.prepared.has_value());
            frame->second.prepared = std::move(update.prepared);
            prepared_order_.push_back(&frame->second);
        }
        if (!prepared_updates.empty()) {
            static_assert(std::is_nothrow_move_assignable_v<doc::Root>);
            preparation_head_ = std::move(staged_preparation_head);
            preparation_next_command_ = staged_next_command;
            next_to_prepare_ = staged_next_frame;
            return batch_status(ReceiveStatus::Prepared);
        }
        return batch_status(ReceiveStatus::Accepted);
    } catch (const std::bad_alloc&) {
        return batch_allocation_failed();
    } catch (const std::length_error&) {
        return batch_allocation_failed();
    }
}

void NodeCommandBus::try_prepare() {
    while (!fault_) {
        const auto frame = frames_.find(next_to_prepare_);
        if (frame == frames_.end() || !frame->second.seal) {
            return;
        }

        auto& state = frame->second;
        auto probe = probe_preparation(
            *state.seal,
            state.commands,
            preparation_head_,
            preparation_next_command_);
        if (probe.status == PreparationProbeStatus::Incomplete) {
            return;
        }
        if (probe.status == PreparationProbeStatus::WouldFault) {
            assert(probe.fault.has_value());
            latch_fault(
                probe.fault->code,
                probe.fault->frame,
                probe.fault->command);
            return;
        }
        state.prepared = std::move(probe.prepared);
        preparation_head_ = *state.prepared;
        preparation_next_command_ = probe.next_command;
        prepared_order_.push_back(&state);

        if (next_to_prepare_.value() == std::numeric_limits<core::u64>::max()) {
            return;
        }
        next_to_prepare_ = next_frame(next_to_prepare_);
    }
}

CommitResult NodeCommandBus::commit_frame(FrameNumber frame) noexcept {
    if (prepared_checkpoint_) {
        return CommitResult{
            CommitStatus::CheckpointPending, frame, document_.version(), 0U};
    }
    if (fault_) {
        return CommitResult{
            CommitStatus::NeedsResync, frame, document_.version(), 0U};
    }
    if (committed_through_.value() == std::numeric_limits<core::u64>::max()
        || frame.value() != committed_through_.value() + 1U) {
        latch_fault(FaultCode::NonConsecutiveCommit, frame);
        return CommitResult{
            CommitStatus::NeedsResync, frame, document_.version(), 0U};
    }
    if (commit_cursor_ >= prepared_order_.size()
        || prepared_order_[commit_cursor_]->seal->frame != frame) {
        latch_fault(FaultCode::FrameNotPrepared, frame);
        return CommitResult{
            CommitStatus::NeedsResync, frame, document_.version(), 0U};
    }

    auto& state = *prepared_order_[commit_cursor_++];
    document_ = *state.prepared;
    state.committed = true;
    committed_through_ = frame;
    if (!state.seal->commands.empty()) {
        next_command_ = CommandId{state.seal->commands.back().id.value() + 1U};
    }
    return CommitResult{
        CommitStatus::Committed,
        frame,
        document_.version(),
        state.seal->commands.size(),
    };
}

bool NodeCommandBus::retire_committed_replay_history() noexcept {
    if (prepared_checkpoint_) {
        return false;
    }
    if (commit_cursor_ != prepared_order_.size()) {
        return false;
    }
    for (const auto& entry : frames_) {
        if (!entry.second.committed) {
            return false;
        }
    }

    std::vector<FrameState*>{}.swap(prepared_order_);
    frames_.clear();
    seen_commands_.clear();
    std::vector<doc::Root>{}.swap(retained_roots_);
    commit_cursor_ = 0U;
    return true;
}

core::Result<CheckpointPreparationToken, CheckpointPrepareError>
NodeCommandBus::prepare_checkpoint(StreamCheckpoint checkpoint) {
    using Result =
        core::Result<CheckpointPreparationToken, CheckpointPrepareError>;
    if (prepared_checkpoint_) {
        return Result::failure(
            CheckpointPrepareError::PreparationOutstanding);
    }
    if (checkpoint_preparation_generation_ ==
        std::numeric_limits<core::u64>::max()) {
        return Result::failure(CheckpointPrepareError::GenerationExhausted);
    }
    if (checkpoint.document.version() != checkpoint.cursor.revision) {
        return Result::failure(CheckpointPrepareError::RevisionMismatch);
    }
    if (!checkpoint.cursor.next_command) {
        return Result::failure(CheckpointPrepareError::InvalidNextCommand);
    }
    if (checkpoint.cursor.through_frame.value() ==
        std::numeric_limits<core::u64>::max()) {
        return Result::failure(CheckpointPrepareError::FrameExhausted);
    }

    try {
        auto& stored = prepared_checkpoint_.emplace();
        static_assert(std::is_nothrow_move_assignable_v<StreamCheckpoint>);
        stored.checkpoint = std::move(checkpoint);
        stored.preparation_head = stored.checkpoint.document;
        stored.next_to_prepare = next_frame(stored.checkpoint.cursor.through_frame);
        stored.preparation_next_command = stored.checkpoint.cursor.next_command;
        stored.retained_roots.push_back(stored.checkpoint.document);

        const auto token = CheckpointPreparationToken{
            stored.checkpoint.cursor.through_frame,
            ++checkpoint_preparation_generation_,
        };
        stored.token = token;
        return Result::success(token);
    } catch (const std::bad_alloc&) {
        prepared_checkpoint_.reset();
        return Result::failure(CheckpointPrepareError::AllocationFailed);
    } catch (const std::length_error&) {
        prepared_checkpoint_.reset();
        return Result::failure(CheckpointPrepareError::AllocationFailed);
    }
}

bool NodeCommandBus::activate_prepared_checkpoint(
    CheckpointPreparationToken token) noexcept {
    if (!prepared_checkpoint_ || prepared_checkpoint_->token != token) {
        return false;
    }

    static_assert(std::is_nothrow_move_assignable_v<doc::Root>);
    auto& staged = *prepared_checkpoint_;
    document_ = std::move(staged.checkpoint.document);
    preparation_head_ = std::move(staged.preparation_head);
    committed_through_ = staged.checkpoint.cursor.through_frame;
    next_to_prepare_ = staged.next_to_prepare;
    next_command_ = staged.checkpoint.cursor.next_command;
    preparation_next_command_ = staged.preparation_next_command;
    frames_.swap(staged.frames);
    seen_commands_.swap(staged.seen_commands);
    retained_roots_.swap(staged.retained_roots);
    prepared_order_.swap(staged.prepared_order);
    commit_cursor_ = 0U;
    fault_.reset();
    prepared_checkpoint_.reset();
    return true;
}

bool NodeCommandBus::abort_prepared_checkpoint(
    CheckpointPreparationToken token) noexcept {
    if (!prepared_checkpoint_ || prepared_checkpoint_->token != token) {
        return false;
    }
    prepared_checkpoint_.reset();
    return true;
}

const StreamCheckpoint* NodeCommandBus::prepared_checkpoint() const noexcept {
    return prepared_checkpoint_ ? &prepared_checkpoint_->checkpoint : nullptr;
}

bool NodeCommandBus::install_checkpoint(StreamCheckpoint checkpoint) {
    auto prepared = prepare_checkpoint(std::move(checkpoint));
    if (!prepared) {
        return false;
    }
    const auto token = prepared.value();
    if (!activate_prepared_checkpoint(token)) {
        static_cast<void>(abort_prepared_checkpoint(token));
        return false;
    }
    return true;
}

bool NodeCommandBus::is_prepared(FrameNumber frame) const noexcept {
    const auto state = frames_.find(frame);
    return state != frames_.end() && state->second.prepared.has_value();
}

} // namespace mediaserver::cmd
