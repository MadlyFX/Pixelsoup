#pragma once

#include <mediaserver/cmd/director_scheduler.hpp>

#include <chrono>
#include <optional>

namespace mediaserver::app {

enum class EditCoalescingAction : core::u8 {
    Begin,
    Continue,
};

struct EditCoalescingIdentity final {
    cmd::UndoContextId context{};
    cmd::CoalescingKey key;

    bool operator==(const EditCoalescingIdentity&) const = default;
};

struct EditCoalescingPlan final {
    EditCoalescingAction action{EditCoalescingAction::Begin};
    EditCoalescingIdentity identity;
    cmd::TransactionId fresh_transaction{};
    // Set only after one payload in this physical editor repeat is accepted;
    // every additional fan-out payload then continues the same transaction.
    bool started{};
};

// Machine-local policy for semantic keyboard repeats. Time is supplied by the
// Director so tests drive exact boundaries and no timestamp enters Command,
// persistence, fingerprints, or replication.
class EditCoalescingPolicy final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    static constexpr std::chrono::milliseconds inactivity_window{350};

    [[nodiscard]] EditCoalescingPlan observe(
        EditCoalescingIdentity identity,
        cmd::TransactionId fresh_transaction,
        TimePoint now) noexcept;

    // Returns true exactly once when a live stream reaches its inactivity
    // boundary. The caller then closes the scheduler's local session.
    [[nodiscard]] bool expire(TimePoint now) noexcept;
    void close() noexcept;
    [[nodiscard]] bool active() const noexcept { return active_.has_value(); }

private:
    struct Active final {
        EditCoalescingIdentity identity;
        TimePoint last_activity;
    };
    std::optional<Active> active_;
};

// Schedules one payload belonging to a prepared editor repeat. A stale local
// Continue decision can occur when another accepted command already closed the
// scheduler session; in that case this repeat safely becomes a fresh Begin.
[[nodiscard]] core::Result<cmd::Command, cmd::ScheduleError>
schedule_editor_coalesced_edit(
    cmd::DirectorScheduler& scheduler,
    cmd::Payload forward,
    cmd::Payload inverse,
    EditCoalescingPlan& plan,
    cmd::FrameNumber apply_at);

} // namespace mediaserver::app
