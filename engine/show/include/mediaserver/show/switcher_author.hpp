#pragma once

#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/show/switcher_state.hpp>

namespace mediaserver::show {

[[nodiscard]] core::Result<cmd::Command, cmd::ScheduleError>
schedule_switcher_anchor(cmd::DirectorScheduler& scheduler,
                         const SwitcherAnchor& anchor,
                         cmd::FrameNumber apply_at,
                         cmd::UndoContextId undo_context,
                         cmd::TransactionId transaction);

} // namespace mediaserver::show
