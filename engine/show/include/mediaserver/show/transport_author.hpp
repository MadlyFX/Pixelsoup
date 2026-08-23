#pragma once

#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/show/transport_state.hpp>

namespace mediaserver::show {

// Director-side authoring of the replicated transport node.
//
// The show position reaches nodes the same way every other document change
// does: as a scheduled command inside a sealed frame. There is no side channel
// and no node-local extrapolation, which is what keeps two independently paced
// nodes presenting the same show frame.
//
// This allocates and is performed while scheduling, never on a boundary. The
// inverse restores the exact previous transport node, or erases it when the
// document did not have one, so transport edits participate in undo like any
// other command.
[[nodiscard]] core::Result<cmd::Command, cmd::ScheduleError>
schedule_transport_update(
    cmd::DirectorScheduler& scheduler,
    const TransportSnapshot& snapshot,
    cmd::FrameNumber apply_at,
    cmd::UndoContextId undo_context,
    cmd::TransactionId transaction);

} // namespace mediaserver::show
