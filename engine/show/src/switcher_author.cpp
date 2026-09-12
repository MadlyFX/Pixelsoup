#include <mediaserver/show/switcher_author.hpp>
#include <mediaserver/scene/linked_publication.hpp>

#include <utility>

namespace mediaserver::show {

core::Result<bool, cmd::ScheduleError> activate_linked_publication(
    cmd::DirectorScheduler& scheduler, cmd::FrameNumber frame) {
    namespace pub = scene::publication;
    const auto& root=scheduler.projected_document();
    auto switcher=read_switcher(root);
    if (!switcher) return core::Result<bool,cmd::ScheduleError>::success(false);
    const auto candidate=pub::render_scene(root,switcher->program_scene);
    if (candidate == switcher->program_scene ||
        pub::integer(root.get(candidate),scene::schema::asset_commit_control) != frame.value())
        return core::Result<bool,cmd::ScheduleError>::success(false);
    auto active=*root.get(candidate);
    active.properties["publication_committed"]=core::i64{1};
    const auto retained=doc::NodeId{pub::integer(&active,pub::previous)};
    active.properties[std::string{pub::rollback}]=static_cast<core::i64>(
        retained.value() && root.get(retained) ? retained.value() : switcher->program_scene.value());
    active.properties.erase(std::string{pub::previous});
    auto prior=*root.get(switcher->program_scene);
    prior.properties.erase(std::string{pub::candidate});
    switcher->program_scene=candidate;
    switcher->transition_from=candidate; switcher->transition_to=candidate;
    switcher->anchor_control_frame=frame; switcher->duration_control_frames=0;
    auto result=root.set(std::move(active)).set(std::move(prior)).set(make_switcher_node(*switcher));
    const auto scheduled=scheduler.schedule_document_replace(result,cmd::UndoContextId{0x505542},cmd::TransactionId{frame.value()},frame);
    return scheduled ? core::Result<bool,cmd::ScheduleError>::success(true)
                     : core::Result<bool,cmd::ScheduleError>::failure(scheduled.error());
}

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
