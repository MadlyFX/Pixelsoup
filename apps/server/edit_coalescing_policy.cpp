#include "edit_coalescing_policy.hpp"

#include <utility>

namespace mediaserver::app {

EditCoalescingPlan EditCoalescingPolicy::observe(
    EditCoalescingIdentity identity,
    cmd::TransactionId fresh_transaction,
    TimePoint now) noexcept {
    const bool within_window = active_.has_value() &&
        active_->identity == identity && now >= active_->last_activity &&
        now - active_->last_activity < inactivity_window;
    active_ = Active{identity, now};
    return EditCoalescingPlan{
        within_window ? EditCoalescingAction::Continue
                      : EditCoalescingAction::Begin,
        identity, fresh_transaction, false};
}

bool EditCoalescingPolicy::expire(TimePoint now) noexcept {
    if (!active_.has_value() || now < active_->last_activity ||
        now - active_->last_activity < inactivity_window) {
        return false;
    }
    active_.reset();
    return true;
}

void EditCoalescingPolicy::close() noexcept {
    active_.reset();
}

core::Result<cmd::Command, cmd::ScheduleError>
schedule_editor_coalesced_edit(
    cmd::DirectorScheduler& scheduler,
    cmd::Payload forward,
    cmd::Payload inverse,
    EditCoalescingPlan& plan,
    cmd::FrameNumber apply_at) {
    if (plan.started ||
        plan.action == EditCoalescingAction::Continue) {
        auto continued = scheduler.continue_coalesced_edit(
            forward, inverse, plan.identity.context, plan.identity.key,
            apply_at);
        if (continued || plan.started ||
            continued.error() !=
                cmd::ScheduleError::CoalescingSessionMismatch) {
            if (continued) {
                plan.started = true;
            }
            return continued;
        }
        // An accepted non-editor command may have closed the scheduler after
        // policy observed this key. Starting with the event's fresh identity
        // preserves the boundary instead of dropping the operator's nudge.
    }

    auto begun = scheduler.begin_coalesced_edit(
        std::move(forward), std::move(inverse), plan.identity.context,
        plan.identity.key, plan.fresh_transaction, apply_at);
    if (begun) {
        plan.started = true;
        plan.action = EditCoalescingAction::Begin;
    }
    return begun;
}

} // namespace mediaserver::app
