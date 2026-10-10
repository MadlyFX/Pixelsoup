#include <mediaserver/render/linked_publication.hpp>
#include <mediaserver/render/linked_scene_ingest.hpp>
#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/cmd/node_command_bus.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/show/switcher_author.hpp>
#include <mediaserver/persist/project_file.hpp>
#include <mediaserver/persist/settings.hpp>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <thread>
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

using namespace mediaserver;
namespace fs=std::filesystem;
namespace ss=scene::schema;
namespace pub=scene::publication;
using namespace std::chrono_literals;
void check(bool ok,const std::string& message) { if (!ok) throw std::runtime_error{message}; }
std::string uuid(core::u64 value) { return scene::link_instance_uuid(doc::NodeId{value}); }
std::string hash(const fs::path& path) {
    const auto result=assets::content_fingerprint(path.string()); check(bool(result),"fingerprint fixture");
    return assets::sha256_hex(result.value().sha256);
}
doc::Node stage(core::u64 id,std::string name) {
    return {doc::NodeId{id},std::string{ss::stage_type},{{"scene_name",std::move(name)},
        {"canvas_width",core::i64{640}},{"canvas_height",core::i64{360}},
        {"ambient",0.8},{"sky_r",0.08},{"sky_g",0.12},{"sky_b",0.18}}};
}
int main(int argc,char** argv) {
#if defined(_MSC_VER)
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    try {
        check(argc==3,"fixture and output directory arguments");
        const fs::path directory=fs::absolute(argv[2]); fs::create_directories(directory);
        const auto run=directory/("run-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())); fs::create_directories(run);
        fs::copy_file(fs::path{argv[1]}/"prop-r1.glb",run/"prop.glb");
        { std::ofstream file{run/"scene.mslink"};
          file << "MSLINK 1 " << std::quoted(uuid(1)) << " \"source-r1\"\nprototype " << std::quoted(uuid(2)) << ' '
               << std::quoted(hash(run/"prop.glb")) << " \"prop.glb\"\nobject " << std::quoted(uuid(3))
               << " \"Linked prop\" \"\" " << std::quoted(uuid(2)) << " \"\" \"origin\" 1 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\n"; }
        auto ingested=render::ingest_linked_scene((run/"scene.mslink").string()); check(ingested.diagnostic.empty(),ingested.diagnostic);
        doc::Root root; root=root.set(stage(10,"Program")).set(stage(20,"Blender Preview"));
        root=root.set({doc::NodeId{21},std::string{ss::camera_type},{{"scene_id",core::i64{20}},{"eye_z",5.0},{"eye_y",1.5},{"target_y",0.0}}});
        root=root.set(show::make_switcher_node({doc::NodeId{10},doc::NodeId{20},doc::NodeId{10},doc::NodeId{10},{},0}));
        auto planned=render::plan_linked_manifest(root,ingested.manifest,doc::NodeId{20}); check(bool(planned),"link Preview"); root=planned.value();
        const auto initial=root;
        const auto occurrence=scene::read_linked_scene(root).value().occurrences.begin()->first;
        // A separate texture row exercises transfer completeness independently
        // of the model package. Rendering qualification uses the real GLB below.
        { std::ofstream file{run/"texture.bin",std::ios::binary}; file << "delayed texture bytes"; }
        auto camera=*root.get(doc::NodeId{20}); camera.properties["emissive_texture"]=(run/"texture.bin").string(); root=root.set(camera);
        auto prepared=render::prepare_linked_publication({root,doc::NodeId{20},doc::NodeId{10},(run/"shared").string(),false,{}});
        check(prepared.diagnostic.empty(),prepared.diagnostic);
        auto installed=render::install_linked_publication(root,prepared,doc::NodeId{10},doc::NodeId{1000}); check(bool(installed),"install atomic candidate");
        root=installed.value(); const auto candidate=pub::render_scene(root,doc::NodeId{10});
        check(candidate!=doc::NodeId{10} && !pub::linked(root,candidate),"frozen candidate has independent drawables");
        check(show::read_switcher(root)->program_scene==doc::NodeId{10},"staging does not switch Program");
        scene::SceneModel model; model.reset(root); const auto before=model.evaluate(candidate,{});
        check(!before.models.empty() && !before.publication_revision.empty(),"frozen drawable and revision evaluate");
        auto edited=scene::plan_linked_world(root,occurrence,scene::translation({8,0,0})); check(bool(edited),"Preview remains editable");
        model.reset(edited.value()); const auto after=model.evaluate(candidate,{});
        check(before.models==after.models && scene::asset_set_digest(before)==scene::asset_set_digest(after),"Preview edits cannot change Program snapshot or resource generation");
        check(std::get<bool>(root.get(candidate)->properties.at(std::string{ss::locked})),"publication scene is read-only");
        doc::Node settings{doc::NodeId{5000},std::string{ss::settings_type},{}};
        scene::RendererSettings changed_settings; changed_settings.texture_budget_mib=64;
        scene::write_renderer_settings(settings,changed_settings);
        auto settings_edit=root.set(settings); model.reset(settings_edit);
        check(model.evaluate(candidate,{}).renderer_settings.texture_budget_mib==before.renderer_settings.texture_budget_mib,
            "global Preview settings cannot change frozen Program settings");
        auto republished=render::prepare_linked_publication({settings_edit,candidate,doc::NodeId{10},(run/"republished").string(),false,{}});
        check(republished.diagnostic.empty(),republished.diagnostic);
        auto recloned=render::install_linked_publication(settings_edit,republished,doc::NodeId{10},doc::NodeId{6000});
        check(bool(recloned),"republish retained snapshot"); model.reset(recloned.value());
        check(model.evaluate(pub::render_scene(recloned.value(),doc::NodeId{10}),{}).renderer_settings.texture_budget_mib==before.renderer_settings.texture_budget_mib,
            "republishing a snapshot preserves its original settings");
        auto refreshed=after; refreshed.asset_manifest_digest=321; refreshed.asset_refresh_generation=789;
        check(scene::asset_set_digest(after)==scene::asset_set_digest(refreshed),"unrelated Preview asset refresh does not invalidate immutable Program");
        auto a=render::prepare_linked_publication({root,candidate,{}, {},true,{}}); check(a.diagnostic.empty(),a.diagnostic);
        const auto texture=pub::text(root.get(candidate),"emissive_texture"); check(!texture.empty(),"texture in candidate");
        fs::rename(texture,texture+".delayed");
        auto b=render::prepare_linked_publication({root,candidate,{}, {},true,{}});
        check(!b.diagnostic.empty(),"second node refuses incomplete texture transfer");
        const auto digest=scene::asset_set_digest(before);
        core::AssetSetReadiness node_a{digest,true,0},node_b{digest,false,0};
        core::AssetCohort cohort; cohort.observe(&node_a); cohort.observe(&node_b); check(!cohort.can_commit(),"delayed cohort cannot authorize");
        core::AssetPublicationGate gate_a,gate_b;
        check(!gate_a.present(node_a,{},20,0,true),"ready node holds before cohort authorization");
        cohort={}; cohort.observe(&node_a); cohort.observe(nullptr); check(!cohort.can_commit(),"disconnected required node blocks publication");
        fs::rename(texture+".delayed",texture);
        b=render::prepare_linked_publication({root,candidate,{}, {},true,{}}); check(b.diagnostic.empty(),b.diagnostic);
        render::PublicationLimits tiny; tiny.vertices=0;
        auto refused=render::prepare_linked_publication({root,candidate,{}, {},true,tiny});
        check(refused.diagnostic.find("capacity refused")!=std::string::npos,"node capacity refusal is explicit before publication");
        node_b.ready=true; cohort={}; cohort.observe(&node_a); cohort.observe(&node_b); check(cohort.can_commit(),"complete cohort can authorize");
        const core::AssetCommit permit{digest,40,7};
        auto authorized=*root.get(candidate); authorized.properties[std::string{ss::asset_commit_digest}]=static_cast<core::i64>(digest);
        authorized.properties[std::string{ss::asset_commit_control}]=core::i64{40}; authorized.properties[std::string{ss::asset_commit_show}]=core::i64{7};
        root=root.set(authorized);
        cmd::DirectorScheduler director{root}; cmd::NodeCommandBus nodes[]{cmd::NodeCommandBus{root},cmd::NodeCommandBus{root}};
        for (core::u64 frame=1;frame<=40;++frame) {
            if (frame==35) {
                auto edit=*director.projected_document().get(occurrence); edit.properties["note"]=std::string{"during lead"};
                check(bool(director.schedule_edit(cmd::PutNode{edit},cmd::PutNode{*director.projected_document().get(occurrence)},cmd::UndoContextId{2},cmd::TransactionId{35},cmd::FrameNumber{frame})),"Preview edit admitted during permit lead");
            }
            const auto activated=show::activate_linked_publication(director,cmd::FrameNumber{frame}); check(bool(activated),"activation admission");
            check(activated.value()==(frame==40),"scene switch only at permit frame");
            const auto seal=director.seal_frame(cmd::FrameNumber{frame}); check(bool(seal),"seal cohort frame");
            for (auto& node : nodes) {
                check(bool(node.receive_complete_batch(director.commands_for(cmd::FrameNumber{frame}),seal.value())),"receive complete frame");
                check(node.commit_frame(cmd::FrameNumber{frame}).status==cmd::CommitStatus::Committed,"commit frame");
                check(show::read_switcher(node.document())->program_scene==(frame==40?candidate:doc::NodeId{10}),"two nodes switch same revision at same frame");
            }
            check(gate_a.present(node_a,permit,frame,7,true)==(frame==40),"first node exact-frame reveal");
            check(gate_b.present(node_b,permit,frame,7,true)==(frame==40),"second node exact-frame reveal");
        }
        check(nodes[0].document().get(occurrence)->properties.contains("note"),"activation preserves intervening Preview edit");
        const auto rollback=doc::NodeId{pub::integer(nodes[0].document().get(candidate),pub::rollback)};
        check(rollback.value() && rollback!=doc::NodeId{10} && !pub::text(nodes[0].document().get(rollback),pub::revision).empty(),
            "ordinary previous Program frozen into independent rollback revision");
        check(render::prepare_linked_publication({nodes[0].document(),rollback,{}, {},true,{}}).diagnostic.empty(),
            "retained rollback has its own verified manifest");
        check(gate_a.visible_commit()==gate_b.visible_commit(),"same publication evidence on both nodes");
        core::AssetPublicationGate late;
        check(!late.present(node_b,permit,41,7,true),"missed-frame hold policy unchanged");
        auto saved=scene::without_asset_commits(nodes[0].document());
        check(pub::integer(saved.get(candidate),ss::asset_commit_control)==0,"restart discards runtime permit");
        check(pub::text(saved.get(candidate),pub::revision)==prepared.revision,"restart retains immutable revision");
        check(bool(persist::save_project_file(run/"published.mstage",saved)),"save offline Program");
        auto reopened=persist::load_project_file(run/"published.mstage"); check(bool(reopened),"reopen offline Program");
        check(render::prepare_linked_publication({reopened.value(),candidate,{}, {},true,{}}).diagnostic.empty(),"offline revision verifies without Blender");
        { std::ofstream corrupt{prepared.manifest,std::ios::app}; corrupt << "# corrupt"; }
        check(!render::prepare_linked_publication({root,candidate,{}, {},true,{}}).diagnostic.empty(),"manifest tampering rejected");
        std::stop_source stop; stop.request_stop();
        check(!render::prepare_linked_publication({initial,doc::NodeId{20},doc::NodeId{10},(run/"cancelled").string(),false,{}},stop.get_token()).diagnostic.empty(),"cancelled worker cannot publish");
        render::LinkedPublication worker;
        worker.submit({initial,doc::NodeId{20},doc::NodeId{10},(run/"superseded").string(),false,{}}); worker.cancel();
        worker.submit({initial,doc::NodeId{20},doc::NodeId{10},(run/"latest").string(),false,{}});
        std::shared_ptr<const render::PublicationResult> latest;
        const auto deadline=std::chrono::steady_clock::now()+20s;
        while (!latest && std::chrono::steady_clock::now()<deadline) { latest=worker.take(); std::this_thread::sleep_for(2ms); }
        check(latest && latest->diagnostic.empty() && latest->manifest.find("latest")!=std::string::npos,"latest-only worker completion");
        {
            // A Program that cannot be frozen (its environment map is gone)
            // does not block the first publication; rollback returns to the
            // live scene. A clip's image sequence publishes in place.
            auto sequence=initial.set({doc::NodeId{30},std::string{ss::media_type},
                {{"scene_id",core::i64{20}},{"source",(run/"plate_####.png").string()},{"sequence_count",core::i64{24}}}});
            auto broken=*sequence.get(doc::NodeId{10});
            broken.properties[std::string{ss::environment_map}]=(run/"missing.hdr").string();
            sequence=sequence.set(broken);
            auto first_take=render::prepare_linked_publication({sequence,doc::NodeId{20},doc::NodeId{10},(run/"sequence").string(),false,{}});
            check(first_take.diagnostic.empty(),first_take.diagnostic);
            check(!first_take.retained && first_take.notice.find("live Program scene")!=std::string::npos,"unfreezable Program keeps a live rollback");
            check(first_take.notice.find("1 image sequence stays")!=std::string::npos,"sequence clip stays at its project path");
            auto installed_take=render::install_linked_publication(sequence,first_take,doc::NodeId{10},doc::NodeId{3000});
            check(bool(installed_take),"install without a frozen rollback");
            const auto frozen=pub::render_scene(installed_take.value(),doc::NodeId{10});
            check(frozen!=doc::NodeId{10} && pub::integer(installed_take.value().get(frozen),pub::previous)==0,"no retained revision");
            check(pub::integer(installed_take.value().get(frozen),pub::source_scene)==20,"frozen revision names the scene it came from");
            check(!installed_take.value().get(frozen)->properties.contains(ss::blender_file),"frozen revision has no Blender file");
            check(pub::text(installed_take.value().get(frozen),ss::scene_name).starts_with("Blender Preview (published "),
                "revision is named after the scene it came from");
            // Publishing again supersedes the unactivated candidate; it is
            // pruned with the install instead of filling the history.
            auto second_take=render::prepare_linked_publication({installed_take.value(),doc::NodeId{20},doc::NodeId{10},(run/"sequence").string(),false,{}});
            check(second_take.diagnostic.empty(),second_take.diagnostic);
            auto reinstalled=render::install_linked_publication(installed_take.value(),second_take,doc::NodeId{10},doc::NodeId{4000});
            check(bool(reinstalled),"install a second candidate");
            const auto pruned=render::prune_publications(reinstalled.value(),doc::NodeId{10});
            const auto latest_candidate=pub::render_scene(pruned,doc::NodeId{10});
            check(!pruned.get(frozen) && latest_candidate!=frozen && pruned.get(latest_candidate),"superseded revision pruned, new one kept");
            check(pruned.get(doc::NodeId{20}) && pruned.get(doc::NodeId{10}),"authored scenes are never pruned");
            const std::array<doc::NodeId,1> previewing{frozen};
            check(render::prune_publications(reinstalled.value(),doc::NodeId{10},previewing).get(frozen)!=nullptr,"a revision the switcher uses is kept");
        }
        auto fixture=initial; auto program=*fixture.get(doc::NodeId{10}); program.properties[std::string{pub::storage}]=(directory/"shared").string(); fixture=fixture.set(program);
        check(bool(persist::save_project_file(directory/"preview.mstage",fixture)),"write UI fixture");
        check(bool(persist::save_recent_project_path(directory/"local/MediaServer/settings/recent-project.bin",directory/"preview.mstage")),"isolated startup fixture");
        for (core::u64 index=0;index<2;++index) fixture=fixture.set({doc::NodeId{9000+index},"hardware.device",{{"kind",std::string{"render_server"}},{"name",std::string{"Publication node "}+std::to_string(index)},{"node_index",static_cast<core::i64>(index)}}});
        check(bool(persist::save_project_file(directory/"two-nodes.mstage",fixture)),"write two-node fixture");
        std::ofstream evidence{directory/"evidence.json"};
        evidence << "{\"revision\":" << std::quoted(prepared.revision) << ",\"document_revision\":" << prepared.document_revision
                 << ",\"node_0_control_frame\":40,\"node_1_control_frame\":40,\"show_frame\":7,\"delayed_texture\":true,\"disconnect\":true,\"capacity_refusal\":true,\"restart\":true,\"cancel_supersede\":true}";
        std::cout << "Publication staging, two-node frame/revision agreement, texture delay, disconnect, capacity, cancellation, superseding and offline recovery passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
