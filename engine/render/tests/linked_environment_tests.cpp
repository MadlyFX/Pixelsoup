#include <mediaserver/render/linked_scene_ingest.hpp>
#include <mediaserver/render/linked_publication.hpp>
#include <mediaserver/render/environment_budget.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/render/scene_bridge.hpp>
#include <mediaserver/scene/instance_batch.hpp>
#include <mediaserver/scene/manipulate.hpp>
#include <mediaserver/persist/project_file.hpp>
#include <mediaserver/persist/settings.hpp>
#include <mediaserver/show/switcher_author.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <thread>

using namespace mediaserver;
namespace fs=std::filesystem;
namespace ss=scene::schema;
namespace ls=scene::link_schema;
void check(bool ok,const std::string& reason) { if (!ok) throw std::runtime_error{reason}; }
double ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
}
int main(int argc,char** argv) {
    try {
        check(argc >= 3,"fixture directory and output directory");
        const fs::path fixture=fs::absolute(argv[1]), output=fs::absolute(argv[2]); fs::create_directories(output);
        auto start=std::chrono::steady_clock::now();
        auto first=render::ingest_linked_scene((fixture/"forest-r1.mslink").string()); check(first.diagnostic.empty(),first.diagnostic);
        const auto import_ms=ms(start);
        doc::Root root;
        root=root.set({doc::NodeId{100001},std::string{ss::stage_type},{{"scene_name",std::string{"Forest Preview"}},
            {"canvas_width",core::i64{640}},{"canvas_height",core::i64{360}},{"ambient",0.65}}});
        root=root.set({doc::NodeId{100002},std::string{ss::stage_type},{{"scene_name",std::string{"Program"}}}});
        scene::Camera aim{}; aim.eye=scene::Vec3{0.0F,18.0F,20.0F}; aim.target=scene::Vec3{0.0F,0.0F,-12.0F};
        const scene::Vec3 turns=scene::camera_rotation(aim);
        root=root.set({doc::NodeId{100003},std::string{ss::camera_type},{{"scene_id",core::i64{100001}},
            {"position_x",0.0},{"position_y",18.0},{"position_z",20.0},{"rotation_yaw",(static_cast<double>(turns.x)+0.0)},{"rotation_pitch",(static_cast<double>(turns.y)+0.0)},{"rotation_roll",(static_cast<double>(turns.z)+0.0)},{"fov_turns",0.17},{"far_plane",300.0}}});
        root=root.set({doc::NodeId{100004},std::string{ss::settings_type},{{"renderer_backend",std::string{"advanced"}},
            {"shadow_cascades",core::i64{4}}}});
        root=root.set({doc::NodeId{100005},std::string{ss::light_type},{{"scene_id",core::i64{100001}},
            {"light_kind",std::string{"directional"}},{"direction_x",-0.3},{"direction_y",-1.0},{"direction_z",-0.2}}});
        root=root.set(show::make_switcher_node({doc::NodeId{100002},doc::NodeId{100001},doc::NodeId{100002},doc::NodeId{100002},{},0}));
        auto planned=render::plan_linked_manifest(root,first.manifest,doc::NodeId{100001}); check(bool(planned),"initial plan"); root=planned.value();
        scene::SceneModel model; model.reset(root); auto frame=model.evaluate(doc::NodeId{100001},{});
        check(frame.models.size()==2064,"2048 trees and 16 terrain chunks");
        auto expensive=root;
        // Enough dense spheres to pass the ~30M-triangle scene budget.
        for (unsigned i=0;i<150;++i) expensive=expensive.set({doc::NodeId{110000+i},std::string{ss::object_type},
            {{"scene_id",core::i64{100001}},{"primitive",std::string{"sphere"}},{"segments",core::i64{256}}}});
        check(render::measure_environment(expensive,doc::NodeId{100001}).diagnostic.find("shadow/triangle")!=std::string::npos,
              "ordinary high-detail props share the environment triangle budget");
        check(doc::diff({},root).changes.size()<70,"compact document does not contain per-tree objects");
        const auto links=scene::read_linked_scene(root); check(bool(links),"valid links");
        doc::NodeId forest{},object{},attachment{};
        for (const auto& [id,p] : links.value().occurrences) if (!p.batch.empty()) { forest=id; object=p.object; attachment=p.instance; }
        check(forest.value()!=0,"one scatter group");
        std::set<doc::NodeId> histories;
        for (const auto& m : frame.models) if (m.source_node==forest) histories.insert(m.motion_source);
        check(histories.size()==2048,"independent render history identities");
        check(scene::world_for(frame,forest)==scene::identity(),"gizmo uses group origin");
        auto bridge=render::SceneBridge::create(8192); check(bool(bridge),"bridge");
        check(bool(bridge.value().update(frame)),"batch bridge");
        auto shot=scene::plan_linked_world(root,forest,scene::translation({1,0,0})); check(bool(shot),"group placement"); root=shot.value();
        start=std::chrono::steady_clock::now();
        auto repaint=render::ingest_linked_scene((fixture/"forest-r2.mslink").string()); check(repaint.diagnostic.empty(),repaint.diagnostic);
        const auto repaint_ms=ms(start);
        check(repaint.manifest.reused >= 17 && repaint.manifest.ingested <= 1,"repaint reuses all unaffected prototypes/chunks");
        planned=render::plan_linked_manifest(root,repaint.manifest,doc::NodeId{100001}); check(bool(planned),"repaint plan"); root=planned.value();
        check(scene::read_linked_scene(root).value().occurrences.at(forest).world==scene::translation({1,0,0}),"repaint preserves group placement");
        auto reseed=render::ingest_linked_scene((fixture/"forest-r3.mslink").string()); check(reseed.diagnostic.empty(),reseed.diagnostic);
        check(reseed.manifest.ingested==0,"density/seed change reuses geometry");
        planned=render::plan_linked_manifest(root,reseed.manifest,doc::NodeId{100001}); check(bool(planned),"reseed plan");
        check(scene::read_linked_scene(planned.value()).value().occurrences.at(forest).world==scene::translation({1,0,0}),"group override survives seed/count changes");
        // Removal retains an explicit orphan; no occurrence index gets a shot edit.
        auto removed=reseed.manifest;
        std::erase_if(removed.records,[&](const auto& r) { return r.node.type==ls::object_type && r.node.properties.contains(scene::instance_batch_property); });
        auto missing=render::plan_linked_manifest(root,removed,doc::NodeId{100001}); check(bool(missing),"remove batch");
        check(scene::read_linked_scene(missing.value()).value().occurrences.at(forest).removed,"removed scatter group orphan retained");
        auto restored=render::plan_linked_manifest(missing.value(),first.manifest,doc::NodeId{100001}); check(bool(restored),"restore batch");
        check(!scene::read_linked_scene(restored.value()).value().occurrences.at(forest).removed,"same stable group restores");
        // Qualify the declared whole-scene ceiling, including the terrain.
        std::vector<scene::Mat4> matrices;
        for (unsigned y=0;y<51;++y) for (unsigned x=0;x<80;++x)
            matrices.push_back(scene::translation({(static_cast<float>(x)-40)*0.65F,0,-static_cast<float>(y)*0.65F}));
        auto batch=*root.get(object); batch.properties[std::string{scene::instance_batch_property}]=scene::encode_instance_batch(matrices);
        auto full=scene::plan_linked_change(root,root.set(batch)); check(bool(full),"4096 full scene admitted");
        matrices.push_back(scene::identity()); batch.properties[std::string{scene::instance_batch_property}]=scene::encode_instance_batch(matrices);
        check(!scene::plan_linked_change(root,root.set(batch)),"4097th drawable refused atomically");
        batch.properties[std::string{scene::instance_batch_property}]=std::string{"not hex"};
        check(!scene::plan_linked_change(root,root.set(batch)),"malformed batch refused");
        root=full.value(); model.reset(root); frame=model.evaluate(doc::NodeId{100001},{});
        const auto budget=render::measure_environment(root,doc::NodeId{100001}); check(budget.diagnostic.empty(),budget.diagnostic);
        check(budget.instances==4096,"budget counts expanded occurrences");
        start=std::chrono::steady_clock::now();
        for (int i=0;i<20;++i) frame=model.evaluate(doc::NodeId{100001},{});
        const auto evaluate_ms=ms(start)/20;
        auto independent=scene::snapshot_link(root,attachment); check(bool(independent),"snapshot compact batches");
        auto prepared=render::prepare_linked_publication({root,doc::NodeId{100001},doc::NodeId{100002},(output/"shared").string(),false,{}});
        check(prepared.diagnostic.empty(),prepared.diagnostic);
        auto installed=render::install_linked_publication(root,prepared,doc::NodeId{100002},doc::NodeId{200000}); check(bool(installed),"publish forest/terrain");
        const auto target=scene::publication::render_scene(installed.value(),doc::NodeId{100002});
        for (int node=0;node<2;++node) {
            check(render::prepare_linked_publication({installed.value(),target,{}, {},true,{}}).diagnostic.empty(),"cohort verifies all immutable LODs");
            scene::SceneModel cohort; cohort.reset(installed.value());
            check(cohort.evaluate(target,{}).models.size()==4096,"frozen batch survives cohort document replay");
        }
        check(bool(persist::save_project_file(output/"preview.mstage",root)),"save UI fixture");
        check(bool(persist::save_recent_project_path(output/"local/Pixelsoup/settings/recent-project.bin",output/"preview.mstage")),"isolated UI startup");
        check(bool(persist::save_project_file(output/"published.mstage",installed.value())),"offline publication save");
        auto reopened=persist::load_project_file(output/"published.mstage"); check(bool(reopened),"offline reopen");
        scene::SceneModel offline; offline.reset(reopened.value()); check(offline.evaluate(target,{}).models.size()==4096,"offline compact content");
        render::RenderStats stats{}; double render_ms{};
        if (argc>3 && std::string_view{argv[3]}=="--gpu") {
            check(rhi::Device::available() && render::SceneRenderer::shaders_available(),"GPU and shaders required");
            rhi::DeviceConfig config; config.debug=false; config.backend=rhi::BackendKind::D3d12;
            auto device=rhi::Device::create(config); check(bool(device),"GPU device");
            render::RendererLimits limits; limits.planar_reflections=false;
            auto created=render::SceneRenderer::create(device.value(),320,360,limits,render::SceneRendererMode::AdvancedLinear);
            check(bool(created),"Advanced renderer"); auto renderer=std::move(created).value();
            const auto draw=[&](scene::CanvasRect slice,bool warming=false) {
                auto acquired=device.value().acquire_commands(); check(bool(acquired),"commands"); auto commands=std::move(acquired).value();
                const bool rendered=renderer.render(commands,frame,slice);
                if (!rendered) { const auto& s=renderer.stats();
                    if (warming && s.models_pending && s.texture_streaming.state==render::TextureStreamState::Loading) return false;
                    throw std::runtime_error{"forest render: instances="+std::to_string(s.instances)+" pending="+std::to_string(s.models_pending)
                        +" failed="+std::to_string(s.models_failed)+" dropped="+std::to_string(s.dropped_instances)
                        +" integrations="+std::to_string(s.model_integrations)+" refused="+std::to_string(s.model_integration_refusals)
                        +" textures="+std::to_string(static_cast<int>(s.texture_streaming.state))}; }
                auto submitted=device.value().submit(std::move(commands)); check(bool(submitted),"submit"); check(submitted.value().wait(),"GPU wait");
                return true;
            };
            const scene::CanvasRect left{0,0,320,360},right{320,0,320,360};
            bool ready=false;
            for (int attempt=0;attempt<100 && !ready;++attempt) {
                ready=draw(left,true);
                if (!ready) { check(render::detail::wait_for_shared_model_cache_for_test(),"worker decode"); std::this_thread::sleep_for(std::chrono::milliseconds{2}); }
            }
            check(ready,"bounded asset preparation completed");
            draw(left); draw(left);
            const auto a=std::vector<render::GeometrySelection>{renderer.geometry_selection().begin(),renderer.geometry_selection().end()};
            start=std::chrono::steady_clock::now();
            for (int i=0;i<5;++i) draw(left);
            render_ms=ms(start)/5; stats=renderer.stats();
            check(stats.dropped_instances==0 && stats.models_failed==0 && stats.models_pending==0,"full scene fits GPU");
            check(stats.instanced_batches>0 && stats.lod_reductions>0,"sharing and authored LOD used");
            check(stats.directional_shadow_cascades==4 && stats.directional_shadow_casters==4096,"complete shadow caster set");
            draw(right);
            check(std::equal(a.begin(),a.end(),renderer.geometry_selection().begin(),renderer.geometry_selection().end()),"identical full-canvas LOD/culling decisions on two output slices");
        }
        std::ofstream evidence{output/(argc>3?"gpu-evidence.json":"evidence.json")};
        evidence << "{\"instances\":" << budget.instances << ",\"document_nodes\":" << doc::diff({},root).changes.size()
            << ",\"vertices\":" << budget.vertices << ",\"indices\":" << budget.indices << ",\"worst_shadow_triangles\":" << budget.shadow_triangles
            << ",\"working_bytes\":" << budget.bytes << ",\"import_ms\":" << import_ms << ",\"repaint_ms\":" << repaint_ms
            << ",\"evaluate_ms\":" << evaluate_ms << ",\"render_submit_wait_ms\":" << render_ms << ",\"gpu_instances\":" << stats.instances
            << ",\"gpu_batches\":" << stats.instanced_batches << ",\"gpu_camera_triangles\":" << stats.camera_triangles
            << ",\"shadow_cascades\":" << stats.directional_shadow_cascades << ",\"shadow_casters\":" << stats.directional_shadow_casters
            << ",\"gpu_upload_bytes\":" << stats.upload_bytes << ",\"lod_reductions\":" << stats.lod_reductions << '}';
        std::cout << "Environment scale acceptance passed: 4096 instances, " << doc::diff({},root).changes.size() << " document nodes, evaluate " << evaluate_ms << " ms\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
