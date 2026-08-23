#pragma once

#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/show/transport_state.hpp>

namespace mediaserver::show {

// Director-side authoring of the replicated transport anchor.
//
// The anchor reaches nodes the same way every other document change does: as a
// scheduled command inside a sealed frame. There is no side channel and no
// node-local extrapolation, which is what keeps two independently paced nodes
// presenting the same show frame.
//
// Because the anchor is keyed to the control-frame ordinal rather than to a
// position, this runs on operator action and end-of-show only. A playing show
// authors nothing at all, so the frames that carry it are empty.
//
// This allocates and is performed while scheduling, never on a boundary. The
// inverse restores the exact previous transport node, or erases it when the
// document did not have one, so transport edits participate in undo like any
// other command.
[[nodiscard]] core::Result<cmd::Command, cmd::ScheduleError>
schedule_transport_anchor(
    cmd::DirectorScheduler& scheduler,
    const TransportAnchor& anchor,
    cmd::FrameNumber apply_at,
    cmd::UndoContextId undo_context,
    cmd::TransactionId transaction);

} // namespace mediaserver::show
