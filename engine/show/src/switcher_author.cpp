#include <mediaserver/show/switcher_author.hpp>

#include <utility>

namespace mediaserver::show {

core::Result<cmd::Command, cmd::ScheduleError>
schedule_switcher_anchor(cmd::DirectorScheduler& scheduler,
                         const SwitcherAnchor& anchor,
                         cmd::FrameNumber apply_at,
                         cmd::UndoContextId undo_context,
                         cmd::TransactionId transaction) {
    cmd::Payload forward{cmd::PutNode{make_switcher_node(anchor)}};
    const doc::Node* const existing =
        scheduler.projected_document().get(switcher_node_id);
    cmd::Payload inverse = existing != nullptr
        ? cmd::Payload{cmd::PutNode{*existing}}
        : cmd::Payload{cmd::EraseNode{switcher_node_id}};
    return scheduler.schedule_edit(std::move(forward), std::move(inverse),
                                   undo_context, transaction, apply_at);
}

} // namespace mediaserver::show
