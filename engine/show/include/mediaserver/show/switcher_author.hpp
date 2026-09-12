#pragma once

#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/show/switcher_state.hpp>

namespace mediaserver::show {

// Completes a prepared linked publication only at the existing asset permit's
// exact control frame. It is one current-frame transaction, preserving the
// scheduler's monotonic admission and intervening Preview edits.
[[nodiscard]] core::Result<bool, cmd::ScheduleError>
activate_linked_publication(cmd::DirectorScheduler& scheduler, cmd::FrameNumber frame);

[[nodiscard]] core::Result<cmd::Command, cmd::ScheduleError>
schedule_switcher_anchor(cmd::DirectorScheduler& scheduler,
                         const SwitcherAnchor& anchor,
                         cmd::FrameNumber apply_at,
                         cmd::UndoContextId undo_context,
                         cmd::TransactionId transaction);

} // namespace mediaserver::show
