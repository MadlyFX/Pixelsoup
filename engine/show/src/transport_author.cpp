#include <mediaserver/show/transport_author.hpp>

#include <utility>

namespace mediaserver::show {

core::Result<cmd::Command, cmd::ScheduleError> schedule_transport_anchor(
    cmd::DirectorScheduler& scheduler,
    const TransportAnchor& anchor,
    cmd::FrameNumber apply_at,
    cmd::UndoContextId undo_context,
    cmd::TransactionId transaction) {
    cmd::Payload forward{cmd::PutNode{make_transport_node(anchor)}};

    const auto* const existing =
        scheduler.projected_document().get(transport_node_id);
    cmd::Payload inverse = existing != nullptr
        ? cmd::Payload{cmd::PutNode{*existing}}
        : cmd::Payload{cmd::EraseNode{transport_node_id}};

    return scheduler.schedule_edit(
        std::move(forward),
        std::move(inverse),
        undo_context,
        transaction,
        apply_at);
}

} // namespace mediaserver::show
