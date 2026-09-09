#include <mediaserver/cmd/director_scheduler.hpp>

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

#include <mediaserver/cmd/reducer.hpp>

namespace mediaserver::cmd {
namespace {

static_assert(std::is_nothrow_move_assignable_v<doc::Root>);

[[nodiscard]] bool semantic_equal(const doc::Root& left, const doc::Root& right) {
    return doc::diff(left, right).changes.empty();
}

} // namespace

DirectorScheduler::DirectorScheduler(
    doc::Root initial,
    FrameNumber sealed_through,
    CommandId next_command,
    DirectorSchedulerLimits limits)
    : sealed_document_(std::move(initial)),
      projected_(sealed_document_),
      sealed_through_(sealed_through),
      sealed_version_(projected_.version()),
      next_command_id_(next_command.value()),
      sealed_next_command_(next_command),
      limits_(limits) {}

void DirectorScheduler::retain_history(
    UndoContextId context,
    TransactionId transaction) {
    auto& order = history_order_[context];
    order.push_back(transaction);
    if (limits_.max_undo_transactions == 0U) {
        return;
    }

    while (order.size() > limits_.max_undo_transactions) {
        const auto retired = order.front();
        order.erase(order.begin());
        history_.erase(HistoryKey{context, retired});

        // Undoability and redoability follow the commands. A transaction whose
        // payloads have been released must not remain reachable from either
        // stack.
        const auto remove_from = [&](HistoryStackMap& stacks) {
            const auto stack = stacks.find(context);
            if (stack == stacks.end()) {
                return;
            }
            const auto position =
                std::find(stack->second.begin(), stack->second.end(), retired);
            if (position != stack->second.end()) {
                stack->second.erase(position);
            }
        };
        remove_from(undo_stacks_);
        remove_from(redo_stacks_);
    }
}

void DirectorScheduler::retire_sealed_history() noexcept {
    if (limits_.retained_sealed_frames == 0U) {
        return;
    }
    const auto retained =
        static_cast<core::u64>(limits_.retained_sealed_frames);
    if (sealed_through_.value() < retained) {
        return;
    }

    // Everything strictly before the window is unreachable: sealing only ever
    // moves forward, and the seal about to be prepared is always the successor
    // of the committed cursor.
    const FrameNumber oldest_kept{sealed_through_.value() - retained + 1U};
    commands_.erase(commands_.begin(), commands_.lower_bound(oldest_kept));
    seals_.erase(seals_.begin(), seals_.lower_bound(oldest_kept));
}

bool DirectorScheduler::frame_is_schedulable(FrameNumber frame) const noexcept {
    return frame > sealed_through_
        && (!last_scheduled_frame_ || frame >= *last_scheduled_frame_);
}

core::Result<Command, ScheduleError> DirectorScheduler::schedule_edit(
    Payload forward,
    Payload inverse,
    UndoContextId undo_context,
    TransactionId transaction,
    FrameNumber apply_at) {
    return schedule_edit_impl(
        std::move(forward), std::move(inverse), undo_context, transaction,
        apply_at, false);
}

core::Result<Command, ScheduleError> DirectorScheduler::begin_coalesced_edit(
    Payload forward,
    Payload inverse,
    UndoContextId undo_context,
    CoalescingKey key,
    TransactionId transaction,
    FrameNumber apply_at) {
    // Begin is itself an explicit semantic boundary, even when the proposed
    // first payload is later rejected. History and redo remain untouched.
    end_coalescing();
    if (history_.contains(HistoryKey{undo_context, transaction})) {
        return core::Result<Command, ScheduleError>::failure(
            ScheduleError::DuplicateTransaction);
    }
    auto scheduled = schedule_edit_impl(
        std::move(forward), std::move(inverse), undo_context, transaction,
        apply_at, false);
    if (scheduled) {
        coalescing_ = CoalescingSession{undo_context, key, transaction};
    }
    return scheduled;
}

core::Result<Command, ScheduleError> DirectorScheduler::continue_coalesced_edit(
    Payload forward,
    Payload inverse,
    UndoContextId undo_context,
    CoalescingKey key,
    FrameNumber apply_at) {
    if (!coalescing_.has_value() || coalescing_->context != undo_context ||
        coalescing_->key != key) {
        end_coalescing();
        return core::Result<Command, ScheduleError>::failure(
            ScheduleError::CoalescingSessionMismatch);
    }
    return schedule_edit_impl(
        std::move(forward), std::move(inverse), undo_context,
        coalescing_->transaction, apply_at, true);
}

void DirectorScheduler::end_coalescing() noexcept {
    coalescing_.reset();
}

core::Result<Command, ScheduleError> DirectorScheduler::schedule_edit_impl(
    Payload forward,
    Payload inverse,
    UndoContextId undo_context,
    TransactionId transaction,
    FrameNumber apply_at,
    bool preserve_coalescing) {
    if (prepared_seal_) {
        return core::Result<Command, ScheduleError>::failure(ScheduleError::SealPrepared);
    }
    if (apply_at <= sealed_through_) {
        return core::Result<Command, ScheduleError>::failure(ScheduleError::FrameSealed);
    }
    if (last_scheduled_frame_ && apply_at < *last_scheduled_frame_) {
        return core::Result<Command, ScheduleError>::failure(ScheduleError::FrameRegression);
    }
    if (next_command_id_ == std::numeric_limits<core::u64>::max()) {
        return core::Result<Command, ScheduleError>::failure(ScheduleError::CommandIdExhausted);
    }
    if (projected_.version().value() == std::numeric_limits<core::u64>::max()) {
        return core::Result<Command, ScheduleError>::failure(ScheduleError::VersionExhausted);
    }

    const HistoryKey key{undo_context, transaction};
    const auto existing_history = history_.find(key);
    const auto stack = undo_stacks_.find(undo_context);
    if (existing_history != history_.end()
        && (stack == undo_stacks_.end() || stack->second.empty()
            || stack->second.back() != transaction)) {
        return core::Result<Command, ScheduleError>::failure(ScheduleError::TransactionNotTop);
    }

    auto applied = apply_payload(projected_, forward);
    if (!applied) {
        return core::Result<Command, ScheduleError>::failure(ScheduleError::ForwardRejected);
    }
    auto restored = apply_payload(applied.value(), inverse);
    if (!restored || !semantic_equal(projected_, restored.value())) {
        return core::Result<Command, ScheduleError>::failure(ScheduleError::InvalidInverse);
    }

    Command command{
        CommandId{next_command_id_++},
        projected_.version(),
        applied.value().version(),
        apply_at,
        transaction,
        undo_context,
        std::move(forward),
        std::move(inverse),
    };
    projected_ = std::move(applied).value();
    commands_[apply_at].push_back(command);
    history_[key].push_back(command);
    if (existing_history == history_.end()) {
        // A successful edit on a new branch makes every undone transaction in
        // this context unreachable, exactly like a conventional editor. Drop
        // their generated payloads as well: they replaced the corresponding
        // user transactions in the bounded history rather than creating new
        // logical edits of their own.
        const auto redo = redo_stacks_.find(undo_context);
        if (redo != redo_stacks_.end()) {
            auto& order = history_order_[undo_context];
            for (const TransactionId discarded : redo->second) {
                history_.erase(HistoryKey{undo_context, discarded});
                const auto position =
                    std::find(order.begin(), order.end(), discarded);
                if (position != order.end()) {
                    order.erase(position);
                }
            }
        }
        redo_stacks_.erase(undo_context);
        undo_stacks_[undo_context].push_back(transaction);
        retain_history(undo_context, transaction);
    }
    last_scheduled_frame_ = apply_at;
    if (!preserve_coalescing) {
        end_coalescing();
    }
    return core::Result<Command, ScheduleError>::success(std::move(command));
}

core::Result<std::vector<Command>, ScheduleError>
DirectorScheduler::schedule_document_replace(
    const doc::Root& replacement,
    UndoContextId undo_context,
    TransactionId transaction,
    FrameNumber apply_at) {
    // Stage against a complete scheduler copy. Besides making a multi-node load
    // all-or-nothing, this deliberately exercises the exact same reducer,
    // inverse validation, transaction-top, and exhaustion checks as an ordinary
    // editor command.
    DirectorScheduler staged = *this;
    staged.end_coalescing();
    const doc::Delta delta = doc::diff(projected_, replacement);
    std::vector<Command> generated;
    generated.reserve(delta.changes.size());

    for (const doc::NodeChange& change : delta.changes) {
        Payload forward;
        Payload inverse;
        switch (change.kind) {
        case doc::ChangeKind::Added:
            if (!change.after) {
                return core::Result<std::vector<Command>, ScheduleError>::failure(
                    ScheduleError::ForwardRejected);
            }
            forward = PutNode{*change.after};
            inverse = EraseNode{change.id};
            break;
        case doc::ChangeKind::Removed:
            if (!change.before) {
                return core::Result<std::vector<Command>, ScheduleError>::failure(
                    ScheduleError::ForwardRejected);
            }
            forward = EraseNode{change.id};
            inverse = PutNode{*change.before};
            break;
        case doc::ChangeKind::Modified:
            if (!change.before || !change.after) {
                return core::Result<std::vector<Command>, ScheduleError>::failure(
                    ScheduleError::ForwardRejected);
            }
            forward = PutNode{*change.after};
            inverse = PutNode{*change.before};
            break;
        }

        auto scheduled = staged.schedule_edit(
            std::move(forward), std::move(inverse), undo_context, transaction,
            apply_at);
        if (!scheduled) {
            return core::Result<std::vector<Command>, ScheduleError>::failure(
                scheduled.error());
        }
        generated.push_back(std::move(scheduled).value());
    }

    *this = std::move(staged);
    return core::Result<std::vector<Command>, ScheduleError>::success(
        std::move(generated));
}

core::Result<std::vector<Command>, DirectorUndoError> DirectorScheduler::schedule_undo(
    UndoContextId undo_context,
    TransactionId undo_transaction,
    FrameNumber apply_at,
    std::span<const std::string_view> runtime_properties) {
    end_coalescing();
    return schedule_history_inverse(
        undo_stacks_, redo_stacks_, undo_context, undo_transaction, apply_at, runtime_properties);
}

core::Result<std::vector<Command>, DirectorUndoError> DirectorScheduler::schedule_redo(
    UndoContextId undo_context,
    TransactionId redo_transaction,
    FrameNumber apply_at,
    std::span<const std::string_view> runtime_properties) {
    end_coalescing();
    return schedule_history_inverse(
        redo_stacks_, undo_stacks_, undo_context, redo_transaction, apply_at, runtime_properties);
}

core::Result<std::vector<Command>, DirectorUndoError>
DirectorScheduler::schedule_history_inverse(
    HistoryStackMap& source_stacks,
    HistoryStackMap& destination_stacks,
    UndoContextId undo_context,
    TransactionId generated_transaction,
    FrameNumber apply_at,
    std::span<const std::string_view> runtime_properties) {
    if (prepared_seal_) {
        return core::Result<std::vector<Command>, DirectorUndoError>::failure(
            DirectorUndoError::SealPrepared);
    }
    const auto stack = source_stacks.find(undo_context);
    if (stack == source_stacks.end() || stack->second.empty()) {
        return core::Result<std::vector<Command>, DirectorUndoError>::failure(
            DirectorUndoError::NoHistory);
    }
    const HistoryKey generated_key{undo_context, generated_transaction};
    if (history_.contains(generated_key)) {
        return core::Result<std::vector<Command>, DirectorUndoError>::failure(
            DirectorUndoError::DuplicateTransaction);
    }
    if (!frame_is_schedulable(apply_at)) {
        return core::Result<std::vector<Command>, DirectorUndoError>::failure(
            DirectorUndoError::FrameRejected);
    }

    const HistoryKey original_key{undo_context, stack->second.back()};
    const auto original_history = history_.find(original_key);
    if (original_history == history_.end()) {
        return core::Result<std::vector<Command>, DirectorUndoError>::failure(
            DirectorUndoError::NoHistory);
    }
    const auto order = history_order_.find(undo_context);
    if (order == history_order_.end()) {
        return core::Result<std::vector<Command>, DirectorUndoError>::failure(
            DirectorUndoError::NoHistory);
    }
    const auto order_position =
        std::find(order->second.begin(), order->second.end(),
                  original_key.transaction);
    if (order_position == order->second.end()) {
        return core::Result<std::vector<Command>, DirectorUndoError>::failure(
            DirectorUndoError::NoHistory);
    }
    if (original_history->second.size()
        > static_cast<std::size_t>(
            std::numeric_limits<core::u64>::max() - next_command_id_)) {
        return core::Result<std::vector<Command>, DirectorUndoError>::failure(
            DirectorUndoError::CommandIdExhausted);
    }
    if (original_history->second.size()
        > static_cast<std::size_t>(
            std::numeric_limits<core::u64>::max() - projected_.version().value())) {
        return core::Result<std::vector<Command>, DirectorUndoError>::failure(
            DirectorUndoError::VersionExhausted);
    }

    auto working = projected_;
    auto next_id = next_command_id_;
    std::vector<Command> generated;
    generated.reserve(original_history->second.size());
    for (auto original = original_history->second.rbegin();
         original != original_history->second.rend();
         ++original) {
        auto expected = original->forward;
        auto inverse = original->inverse;
        for (auto* payload : {&expected, &inverse}) {
            auto* put = std::get_if<PutNode>(payload);
            if (!put) continue;
            const auto* current = working.get(put->node.id);
            if (!current || current->type != put->node.type) continue;
            for (const auto property : runtime_properties) {
                const auto found = current->properties.find(property);
                if (found != current->properties.end())
                    put->node.properties.insert_or_assign(found->first, found->second);
                else if (const auto stale = put->node.properties.find(property);
                         stale != put->node.properties.end())
                    put->node.properties.erase(stale);
            }
        }
        if (!payload_postcondition_matches(working, expected)) {
            return core::Result<std::vector<Command>, DirectorUndoError>::failure(
                DirectorUndoError::HistoryConflict);
        }
        auto applied = apply_payload(working, inverse);
        if (!applied) {
            return core::Result<std::vector<Command>, DirectorUndoError>::failure(
                DirectorUndoError::HistoryConflict);
        }
        generated.push_back(Command{
            CommandId{next_id++},
            working.version(),
            applied.value().version(),
            apply_at,
            generated_transaction,
            undo_context,
            std::move(inverse),
            std::move(expected),
        });
        working = std::move(applied).value();
    }

    projected_ = std::move(working);
    next_command_id_ = next_id;
    auto& frame_commands = commands_[apply_at];
    frame_commands.insert(frame_commands.end(), generated.begin(), generated.end());
    history_.emplace(generated_key, generated);
    history_.erase(original_key);
    *order_position = generated_transaction;
    stack->second.pop_back();
    destination_stacks[undo_context].push_back(generated_transaction);
    // Moving through history replaces the retained command payload for one
    // logical user transaction. It must not consume another retention slot or
    // evict an older edit merely because the operator navigated backward.
    last_scheduled_frame_ = apply_at;
    return core::Result<std::vector<Command>, DirectorUndoError>::success(
        std::move(generated));
}

core::Result<SealPreparation, SealError> DirectorScheduler::prepare_seal(FrameNumber frame) {
    if (prepared_seal_) {
        return core::Result<SealPreparation, SealError>::failure(
            SealError::PrepareOutstanding);
    }
    if (preparation_generation_ == std::numeric_limits<core::u64>::max()) {
        return core::Result<SealPreparation, SealError>::failure(
            SealError::PreparationTokenExhausted);
    }
    if (sealed_through_.value() == std::numeric_limits<core::u64>::max()
        || frame.value() != sealed_through_.value() + 1U) {
        return core::Result<SealPreparation, SealError>::failure(
            SealError::NonConsecutiveFrame);
    }

    FrameSeal seal{frame, sealed_version_, sealed_version_, {}};
    const auto scheduled = commands_.find(frame);
    auto sealed_working = sealed_document_;
    if (scheduled != commands_.end()) {
        seal.commands.reserve(scheduled->second.size());
        auto expected_base = sealed_version_;
        for (const auto& command : scheduled->second) {
            if (command.apply_at != frame || command.base != expected_base) {
                return core::Result<SealPreparation, SealError>::failure(
                    SealError::VersionChainMismatch);
            }
            seal.commands.push_back(SealedCommand{command.id, fingerprint(command)});
            auto applied = apply_payload(sealed_working, command.forward);
            if (!applied || applied.value().version() != command.result) {
                return core::Result<SealPreparation, SealError>::failure(
                    SealError::VersionChainMismatch);
            }
            sealed_working = std::move(applied).value();
            expected_base = command.result;
        }
        seal.result = expected_base;
    }

    auto staged_next_command = sealed_next_command_;
    if (!seal.commands.empty()) {
        staged_next_command = CommandId{seal.commands.back().id.value() + 1U};
    }

    auto caller_seal = seal;
    const auto [position, inserted] = seals_.emplace(frame, std::move(seal));
    if (!inserted) {
        return core::Result<SealPreparation, SealError>::failure(
            SealError::SealHistoryConflict);
    }
    const auto token = SealPreparationToken{frame, ++preparation_generation_};
    prepared_seal_.emplace(PreparedSealState{
        token,
        position->second.result,
        std::move(sealed_working),
        staged_next_command,
    });
    return core::Result<SealPreparation, SealError>::success(
        SealPreparation{std::move(caller_seal), token});
}

bool DirectorScheduler::commit_prepared_seal(SealPreparationToken token) noexcept {
    if (!prepared_seal_ || prepared_seal_->token != token) {
        return false;
    }

    sealed_version_ = prepared_seal_->result;
    sealed_document_ = std::move(prepared_seal_->document);
    sealed_next_command_ = prepared_seal_->next_command;
    sealed_through_ = token.frame;
    prepared_seal_.reset();
    retire_sealed_history();
    return true;
}

bool DirectorScheduler::abort_prepared_seal(SealPreparationToken token) noexcept {
    if (!prepared_seal_ || prepared_seal_->token != token) {
        return false;
    }

    const auto staged = seals_.find(token.frame);
    if (staged != seals_.end()) {
        seals_.erase(staged);
    }
    prepared_seal_.reset();
    return true;
}

const FrameSeal* DirectorScheduler::prepared_seal() const noexcept {
    if (!prepared_seal_) {
        return nullptr;
    }
    const auto staged = seals_.find(prepared_seal_->token.frame);
    return staged == seals_.end() ? nullptr : &staged->second;
}

core::Result<FrameSeal, SealError> DirectorScheduler::seal_frame(FrameNumber frame) {
    auto prepared = prepare_seal(frame);
    if (!prepared) {
        return core::Result<FrameSeal, SealError>::failure(prepared.error());
    }
    const auto token = prepared.value().token;
    if (!commit_prepared_seal(token)) {
        static_cast<void>(abort_prepared_seal(token));
        return core::Result<FrameSeal, SealError>::failure(SealError::SealHistoryConflict);
    }
    return core::Result<FrameSeal, SealError>::success(
        std::move(prepared).value().seal);
}

std::span<const Command> DirectorScheduler::commands_for(FrameNumber frame) const noexcept {
    const auto commands = commands_.find(frame);
    if (commands == commands_.end()) {
        return {};
    }
    return commands->second;
}

const FrameSeal* DirectorScheduler::seal_for(FrameNumber frame) const noexcept {
    if (frame > sealed_through_) {
        return nullptr;
    }
    const auto seal = seals_.find(frame);
    return seal == seals_.end() ? nullptr : &seal->second;
}

StreamCheckpoint DirectorScheduler::checkpoint() const {
    return StreamCheckpoint{
        StreamCursor{sealed_through_, sealed_version_, sealed_next_command_},
        sealed_document_,
    };
}

std::size_t DirectorScheduler::undo_depth(UndoContextId context) const noexcept {
    const auto stack = undo_stacks_.find(context);
    return stack == undo_stacks_.end() ? 0U : stack->second.size();
}

std::size_t DirectorScheduler::redo_depth(UndoContextId context) const noexcept {
    const auto stack = redo_stacks_.find(context);
    return stack == redo_stacks_.end() ? 0U : stack->second.size();
}

void DirectorScheduler::clear_undo_history() noexcept {
    end_coalescing();
    history_.clear();
    undo_stacks_.clear();
    redo_stacks_.clear();
    history_order_.clear();
}

DirectorAuthoringFootprint DirectorScheduler::authoring_footprint()
    const noexcept {
    return DirectorAuthoringFootprint{
        commands_.size(),
        seals_.size(),
        history_order_.size(),
        history_.size(),
    };
}

} // namespace mediaserver::cmd
