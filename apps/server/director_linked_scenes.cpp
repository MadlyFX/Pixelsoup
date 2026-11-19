// Linked-scene publication and Blender link servicing at the frame boundary.

#include "director_show_loop.hpp"

#include "blender_links.hpp"

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/render/blender_discovery.hpp>
#include <mediaserver/render/director_link_service.hpp>
#include <mediaserver/render/linked_publication.hpp>
#include <mediaserver/render/linked_scene_ingest.hpp>
#include <mediaserver/scene/linked_publication.hpp>
#include <mediaserver/scene/linked_scene.hpp>
#include <mediaserver/scene/scene_project.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/show/switcher_state.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mediaserver::app {

PhaseExit DirectorShowLoop::apply_publication_requests(OperatorFrame &op) {
    const auto &frame = op.frame;
    // Publication snapshots Preview; the installed revision stays
    // editable on Program. Preview may continue receiving Blender
    // edits while its worker stages bytes.
    namespace pub = scene::publication;
    const auto publication_anchor = show::read_switcher(scheduler.projected_document());
    const auto program_id = publication_anchor ? publication_anchor->program_scene : doc::NodeId{};
    const bool publication_in_flight = publication_activation > frame.value();
    const auto clear_candidate = [&]() {
        const auto* source=scheduler.projected_document().get(program_id);
        if (!source) return true;
        auto node=*source; node.properties.erase(std::string{pub::candidate});
        auto root=scheduler.projected_document().set(node);
        const doc::NodeId discarded{pub::integer(source,pub::candidate)};
        if (discarded.value() && pub::integer(root.get(discarded),"publication_committed") == 0
            && discarded.value() != pub::integer(source,pub::rollback)) {
            const doc::NodeId retained{pub::integer(root.get(discarded),pub::previous)};
            if (auto deletion=scene::plan_delete_scene(root,discarded))
                for (auto id : deletion.value().erases) if (auto erased=root.erase(id)) root=std::move(*erased);
            if (retained.value()) if (auto deletion=scene::plan_delete_scene(root,retained))
                for (auto id : deletion.value().erases) if (auto erased=root.erase(id)) root=std::move(*erased);
        }
        return static_cast<bool>(scheduler.schedule_document_replace(root,
            cmd::UndoContextId{0x505542},cmd::TransactionId{frame.value()},frame));
    };
    if (pending_publication_storage) {
        const auto* source=scheduler.projected_document().get(program_id);
        if (source) {
            auto node=*source; node.properties[std::string{pub::storage}]=*pending_publication_storage;
            if (!scheduler.schedule_edit(cmd::PutNode{node},cmd::PutNode{*source},cmd::UndoContextId{0x505542},cmd::TransactionId{frame.value()},frame)) return 1;
        }
        pending_publication_storage.reset();
    }
    if (pending_publication_cancel && !publication_in_flight) {
        linked_publication.cancel(); publication_staging=false;
        if (!clear_candidate()) return 1;
        pending_asset_commit.reset();
        publication_message="Publication cancelled; Program retains its accepted revision.";
    }
    pending_publication_cancel=false;
    if (pending_publication_rollback && publication_anchor && !publication_in_flight) {
        linked_publication.cancel(); publication_staging=false;
        const auto* old=scheduler.projected_document().get(program_id);
        const doc::NodeId previous{pub::integer(old,pub::rollback)};
        if (previous.value() && scheduler.projected_document().get(previous)) {
            auto node=*old; node.properties[std::string{pub::candidate}]=static_cast<core::i64>(previous.value());
            auto rollback_stage=*scheduler.projected_document().get(previous);
            for (auto key : {scene::schema::asset_commit_digest,scene::schema::asset_commit_control,scene::schema::asset_commit_show}) rollback_stage.properties.erase(std::string{key});
            auto root=scheduler.projected_document().set(node).set(rollback_stage);
            if (!scheduler.schedule_document_replace(root,cmd::UndoContextId{0x505542},cmd::TransactionId{frame.value()},frame)) return 1;
            pending_asset_commit.reset();
            publication_message="Rollback verifying retained resources and waiting for the cohort.";
        }
    }
    pending_publication_rollback=false;
    if (pending_publication && !publication_in_flight) {
        if (!publication_anchor || show::derive_switcher_progress(*publication_anchor,frame).active) publication_message="Select an unlocked Preview scene.";
        else {
            if (!clear_candidate()) return 1;
            publication_program=program_id; publication_project=project_generation;
            // Without a chosen folder, Program files live beside the
            // project: on a share every node sees, that is enough.
            auto storage=pub::text(scheduler.projected_document().get(program_id),pub::storage);
            if (storage.empty()) storage=default_publication_storage(project_path);
            linked_publication.submit({scheduler.projected_document(),publication_anchor->rehearsal_scene,program_id,storage,false,{}});
            publication_staging=true; pending_asset_commit.reset();
            publication_message="Staging a frozen Preview revision in "+storage+"...";
        }
    }
    pending_publication=false;
    if (auto prepared=linked_publication.take()) {
        publication_staging=false;
        if (publication_project != project_generation || publication_program != program_id) publication_message="Publication discarded because the project or Program changed.";
        else if (!prepared->diagnostic.empty()) publication_message=prepared->diagnostic;
        else {
            auto planned=render::install_linked_publication(scheduler.projected_document(),*prepared,program_id,doc::NodeId{next_new_node});
            if (!planned) publication_message=planned.error().message;
            else {
                pending_asset_commit.reset();
                // Revisions nothing can return to any more leave
                // with the install, so repeated publishing never
                // fills the project's revision history.
                std::vector<doc::NodeId> in_use;
                if (publication_anchor) in_use = {publication_anchor->rehearsal_scene,
                    publication_anchor->transition_from, publication_anchor->transition_to};
                const auto installed=render::prune_publications(planned.value(),program_id,in_use);
                if (!scheduler.schedule_document_replace(installed,cmd::UndoContextId{0x505542},cmd::TransactionId{frame.value()},frame)) return 1;
                for (const auto& c : doc::diff({},installed).changes) next_new_node=std::max(next_new_node,c.id.value()+1);
                publication_message="Revision "+prepared->revision.substr(0,12)+" staged; waiting for node verification and GPU readiness."
                    +(prepared->notice.empty() ? std::string{} : " "+prepared->notice);
                std::cout << "linked-publication staged revision=" << prepared->revision << " document=" << prepared->document_revision << " bytes=" << prepared->bytes << '\n';
            }
        }
    }
    return std::nullopt;
}

PhaseExit DirectorShowLoop::apply_blender_links(OperatorFrame &op) {
    const auto &frame = op.frame;
    const auto &projected_switcher = op.projected_switcher;
    auto &scene_edits_locked = op.scene_edits_locked;
    const auto authoring_scene = [&]() -> doc::NodeId {
        return scene_being_authored(op);
    };
    // Each Blender link edits its own scene wherever it is -
    // Preview, Program, or neither - so Program follows Blender
    // live. A scene in a running dissolve waits: its link's next
    // revision is taken once the transition ends.
    const bool transition_locked = scene_edits_locked;
    const auto link_context_for = [&](doc::NodeId target) {
        const auto& document = scheduler.projected_document();
        const doc::NodeId program = projected_switcher ? projected_switcher->program_scene : doc::NodeId{};
        return render::DirectorLinkContext{document, target, program, project_generation};
    };
    const auto link_transitioning = [&](doc::NodeId target) {
        return transition_locked && projected_switcher
            && (target == projected_switcher->transition_from || target == projected_switcher->transition_to);
    };
    namespace pub = scene::publication;
    if (!blender_association_root.same_snapshot(scheduler.projected_document())) {
        refresh_blender_associations(blender_associations, blender_association_root,
            scheduler.projected_document());
        blender_association_root = scheduler.projected_document();
    }
    // What Blender sends is stored beside the project, where the
    // project's output nodes read it.
    if (blender_store_project != project_path) {
        blender_store_project = project_path;
        blender_links.set_cache_directory(path_utf8(link_resource_directory(project_path)));
    }
    blender_links.poll(blender_associations, link_context_for);
    if (pending_linked_paste) {
        const auto request = *pending_linked_paste; pending_linked_paste.reset();
        if (!rehearsal_mode || scene_edits_locked || request.scene != authoring_scene())
            project_message = "PASTE - select an unlocked Preview scene";
        else if (linked_clipboard) {
            auto planned = scene::paste_link_snapshot(scheduler.projected_document(),*linked_clipboard,request.scene);
            if (!planned) project_message = "PASTE - " + planned.error().message;
            else if (scheduler.schedule_document_replace(planned.value(),cmd::UndoContextId{2U},
                    cmd::TransactionId{request.transaction},frame)) {
                for (const auto& c : doc::diff({},planned.value()).changes) next_new_node = std::max(next_new_node,c.id.value()+1);
                project_message = "PASTED - independent accepted collection; Undo restores Preview";
            } else project_message = "PASTE - Director transaction failed";
        }
    }
    const auto blender_scene_name = [&](doc::NodeId scene) {
        const auto found = std::find_if(blender_associations.begin(), blender_associations.end(),
            [&](const BlenderAssociation& item) { return item.scene == scene; });
        return found == blender_associations.end() || found->name.empty()
            ? std::string{"Untitled Scene"} : found->name;
    };
    // At most one candidate per link per control frame; every link
    // is planned against its own scene, never the Stage's. A scene
    // in a running dissolve keeps its revision until the dissolve
    // ends, like the operator's own edits.
    for (std::size_t taken_count = 0; taken_count < BlenderLinks::max_links; ++taken_count) {
        auto taken = blender_links.take(link_transitioning);
        if (!taken) break;
        const auto& candidate = taken->candidate;
        auto planned = render::DirectorLinkService::plan(*candidate, link_context_for(taken->scene));
        std::string rejection;
        if (!planned) rejection = planned.error().message;
        else {
            // Reserve the high-bit transaction namespace for remote
            // edits while retaining the Scene panel's normal Undo.
            // Remote authoring history is independent of local Scene
            // gestures. Native Undo can restore a local shot after a
            // geometry publish without undoing the artist's mesh.
            // Every link numbers its own revisions from 1, so the
            // transaction is numbered here, across all links.
            auto scheduled = scheduler.schedule_document_replace(planned.value(), cmd::UndoContextId{render::director_link_undo_context},
                cmd::TransactionId{(core::u64{1} << 63U) | ++blender_link_transactions}, frame);
            if (!scheduled) rejection = "director_transaction_failed";
            else for (const auto& change : doc::diff({}, planned.value()).changes)
                next_new_node = std::max(next_new_node, change.id.value()+1);
        }
        blender_links.resolve(taken->scene, candidate->ticket, rejection, link_context_for(taken->scene));
        project_message = rejection.empty()
            ? "BLENDER - " + blender_scene_name(taken->scene) + " updated: " + candidate->manifest.revision
            : "BLENDER - " + blender_scene_name(taken->scene) + " update refused: " + rejection;
    }
    for (const auto& [scene, state] : blender_links.take_status_changes())
        if (state && !state->message.empty())
            project_message = "BLENDER - " + blender_scene_name(scene) + ": " + state->message;
    if (auto message = blender_links.take_message(); !message.empty()) project_message = std::move(message);
    for (auto& added : blender_links.take_created())
        if (blender_scene_requests.size() < 16) blender_scene_requests.push_back(std::move(added));
    if (!blender_scene_requests.empty() && !pending_switcher && !blender_scene_creating) {
        blender_scene_creating = std::move(blender_scene_requests.front());
        blender_scene_requests.pop_front();
        pending_switcher = editor::SwitcherRequest{editor::SwitcherAction::CreateScene};
        project_message = "BLENDER - adding " + blender_scene_creating->name + " from Blender";
    }
    for (const auto& moved : blender_links.take_moved()) {
        // Saved As in Blender while linked: Open in Blender follows.
        if (const auto* stage = scheduler.projected_document().get(moved.scene)) {
            auto updated = *stage;
            updated.properties[std::string{scene::schema::blender_file}] = moved.file;
            if (!scheduler.schedule_edit(cmd::PutNode{updated}, cmd::PutNode{*stage},
                    cmd::UndoContextId{render::director_link_undo_context}, cmd::TransactionId{frame.value()}, frame))
                return 1;
        }
    }
    // Operator Blender requests run here, at a control frame, where
    // recording a scene's Blender file and source identity is an
    // ordinary edit. A scene opened for the first time joins the
    // project's .blend - one file with a Blender scene per
    // Pixelsoup scene; one linked from the Connect dialog adopts the
    // file that Blender already has open.
    const auto blender_association = [&](doc::NodeId target, const std::string& instance_file,
                                         bool choose_file) -> std::optional<BlenderAssociation> {
        const auto& document = scheduler.projected_document();
        const auto* stage = document.get(target);
        if (!stage || stage->type != scene::schema::stage_type) return std::nullopt;
        BlenderAssociation result{target, pub::text(stage, scene::schema::scene_name),
            pub::text(stage, scene::schema::blender_file), pub::text(stage, scene::schema::blender_link_source)};
        if (result.name.empty()) result.name = "Untitled Scene";
        auto updated = *stage;
        if (!render::valid_link_source(result.source)) {
            result.source = new_link_source();
            updated.properties.insert_or_assign(std::string{scene::schema::blender_link_source}, result.source);
        }
        if (result.file.empty()) {
            if (!instance_file.empty()) result.file = instance_file;
            else if (choose_file) {
                if (project_path.empty() && unsaved_blender_name.empty()) {
                    // An unsaved project's .blend gets a name of its own.
                    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                    std::tm local{};
#if defined(_WIN32)
                    localtime_s(&local, &now);
#else
                    localtime_r(&now, &local);
#endif
                    char stamp[32]{};
                    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M-%S", &local);
                    unsaved_blender_name = std::string{"Unsaved Project "} + stamp;
                }
                result.file = shared_blender_file(blender_associations, target,
                    path_utf8(project_blender_file(project_path, unsaved_blender_name)));
            }
            if (!result.file.empty())
                updated.properties.insert_or_assign(std::string{scene::schema::blender_file}, result.file);
        }
        if (updated.properties != stage->properties && !scheduler.schedule_edit(cmd::PutNode{updated}, cmd::PutNode{*stage},
                cmd::UndoContextId{render::director_link_undo_context}, cmd::TransactionId{frame.value()}, frame))
            return std::nullopt;
        return result;
    };
    const bool blender_switching = projected_switcher.has_value()
        && show::derive_switcher_progress(*projected_switcher, frame).active;
    if (deferred_blender_open && !blender_switching) {
        const auto requested = *deferred_blender_open;
        deferred_blender_open.reset();
        const auto& document = scheduler.projected_document();
        const doc::NodeId program = projected_switcher ? projected_switcher->program_scene : doc::NodeId{};
        doc::NodeId editable = requested;
        if (!pub::text(document.get(requested), pub::revision).empty()) {
            // A published revision is frozen; its Blender is the one
            // of the scene it was published from.
            editable = doc::NodeId{pub::integer(document.get(requested), pub::source_scene)};
            if (!editable.value() || !document.get(editable)
                || !pub::text(document.get(editable), pub::revision).empty()) {
                project_message = "BLENDER - this published revision has no editable scene left; open another scene";
                editable = doc::NodeId{};
            }
        }
        if (editable.value() && document.get(editable)) {
            // The scene opens as itself, on air or not: Blender
            // edits it live. A scene off air is brought into
            // Preview so the operator sees what Blender changes;
            // Program is left exactly where it is.
            if (projected_switcher && editable != program) {
                if (projected_switcher->rehearsal_scene != editable) {
                    if (!pending_switcher)
                        pending_switcher = editor::SwitcherRequest{editor::SwitcherAction::SelectRehearsal, editable, 0U};
                } else if (!rehearsal_mode) enter_preview(frame);
            }
            if (auto association = blender_association(editable, {}, true))
                blender_links.open(*association, link_context_for(editable));
        }
    }
    if (!deferred_blender_resync.empty()) {
        // An instance linked to no scene resyncs by connecting to Preview.
        if (!blender_links.resync(deferred_blender_resync, link_context_for))
            deferred_blender_connect = deferred_blender_resync;
        deferred_blender_resync.clear();
    }
    if (!deferred_blender_connect.empty()) {
        const auto rehearsal = projected_switcher ? projected_switcher->rehearsal_scene : doc::NodeId{};
        if (blender_switching) {
            blender_connection_error = "Waiting for the Preview transition to finish...";
        } else if (!rehearsal.value() || rehearsal == projected_switcher->program_scene) {
            // First connection creates a Preview through the ordinary
            // undoable scene authoring path, then connects next frame.
            if (!pending_switcher) pending_switcher = editor::SwitcherRequest{editor::SwitcherAction::CreateScene};
            blender_connection_error = "Preparing a Blender Preview scene...";
        } else {
            blender_connection_error.clear();
            if (!rehearsal_mode) enter_preview(frame);
            const auto instance = std::exchange(deferred_blender_connect, std::string{});
            std::string open_file;
            if (const auto state = blender_links.discovery())
                for (const auto& item : state->instances) if (item.id == instance) open_file = item.file;
            if (auto association = blender_association(rehearsal, open_file, false)) {
                // The scene that Blender shows becomes this scene.
                blender_links.bind(instance, *association, link_context_for(rehearsal), true);
                project_message = "BLENDER - connecting to " + association->name + "...";
            }
        }
    }
    if (auto completed = linked_ingest.take()) {
        const auto& current = scheduler.projected_document();
        bool stale = linked_ingest_project != project_generation || scene_edits_locked;
        for (const auto& change : doc::diff(linked_ingest_base, current).changes) {
            const auto relevant = [](const doc::Node* node) {
                return node && node->type.starts_with("link.");
            };
            if (relevant(change.before ? &*change.before : nullptr) || relevant(change.after ? &*change.after : nullptr)
                || (change.before && scene::is_linked_occurrence(*change.before) && !change.after)) stale = true;
        }
        // A source shared with the scene on air updates it too:
        // linked content is live wherever it is used.
        if (stale) project_message = "LINK IMPORT DISCARDED - project or linked source changed; retry import";
        else if (!completed->diagnostic.empty()) project_message = "LINK IMPORT REJECTED - " + completed->diagnostic;
        else {
            auto planned = render::plan_linked_manifest(current, completed->manifest, linked_ingest_target);
            if (!planned) project_message = "LINK IMPORT REJECTED - " + planned.error().message;
            else {
                auto scheduled = scheduler.schedule_document_replace(planned.value(), cmd::UndoContextId{2U},
                    cmd::TransactionId{linked_ingest_gesture}, frame);
                if (!scheduled) project_message = "LINK IMPORT REJECTED - Director transaction failed";
                else {
                    for (const auto& change : doc::diff({}, planned.value()).changes)
                        next_new_node = std::max(next_new_node, change.id.value()+1);
                    project_message = "LINK IMPORT COMPLETE - " + std::to_string(completed->manifest.ingested)
                        + " prototypes ingested, " + std::to_string(completed->manifest.reused) + " reused";
                }
            }
        }
    }
    return std::nullopt;
}

} // namespace mediaserver::app
