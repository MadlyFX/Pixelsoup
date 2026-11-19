#include <mediaserver/render/linked_scene_ingest.hpp>
#include <mediaserver/render/linked_publication.hpp>
#include <mediaserver/render/baked_lighting.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/render/scene_bridge.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/linked_light.hpp>
#include <mediaserver/persist/project_file.hpp>
#include <mediaserver/persist/settings.hpp>
#include <mediaserver/show/switcher_author.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace mediaserver;
namespace fs=std::filesystem;
namespace ss=scene::schema;
namespace ls=scene::link_schema;
void check(bool ok,const std::string& why) { if (!ok) throw std::runtime_error{why}; }
bool near(float a,float b) { return std::abs(a-b)<1e-5F; }
int main(int argc,char** argv) {
    try {
        check(argc>=3,"fixture and output directories required");
        const auto fixture=fs::absolute(argv[1]), output=fs::absolute(argv[2]); fs::create_directories(output);
        const auto a=render::ingest_linked_scene((fixture/"look-r1.mslink").string()); check(a.diagnostic.empty(),a.diagnostic);
        doc::Root root;
        const doc::NodeId preview{10001},program{10002};
        root=root.set({preview,std::string{ss::stage_type},{{"scene_name",std::string{"Look Preview"}},
            {"canvas_width",core::i64{640}},{"canvas_height",core::i64{360}},{"ambient",0.1}}});
        root=root.set({program,std::string{ss::stage_type},{{"scene_name",std::string{"Program"}}}});
        scene::Camera aim{}; aim.eye=scene::Vec3{0.0F,1.2F,5.0F}; aim.target=scene::Vec3{0.0F,1.0F,-1.0F};
        const scene::Vec3 turns=scene::camera_rotation(aim);
        root=root.set({doc::NodeId{10003},std::string{ss::camera_type},{{"scene_id",core::i64{10001}},
            {"position_x",0.0},{"position_y",1.2},{"position_z",5.0},{"rotation_yaw",(static_cast<double>(turns.x)+0.0)},{"rotation_pitch",(static_cast<double>(turns.y)+0.0)},{"rotation_roll",(static_cast<double>(turns.z)+0.0)},{"fov_turns",0.17}}});
        root=root.set({doc::NodeId{10004},std::string{ss::settings_type},{{"renderer_backend",std::string{"advanced"}}}});
        root=root.set(show::make_switcher_node({program,preview,program,program,{},0}));
        const auto planned=render::plan_linked_manifest(root,a.manifest,preview); check(bool(planned),planned ? "" : planned.error().message); root=planned.value();
        scene::SceneModel model; model.reset(root); auto frame=model.evaluate(preview,{});
        check(frame.models.size()==2 && frame.point_lights.size()==1 && frame.directional_lights.size()==1,"two surfaces and native light types");
        check(frame.point_lights[0].unit==scene::LightUnit::Lumens && near(frame.point_lights[0].intensity,1800)
            && near(frame.point_lights[0].width,2) && near(frame.point_lights[0].height,1),"rectangle lumens and metre dimensions");
        check(frame.directional_lights[0].unit==scene::LightUnit::Lux && near(frame.directional_lights[0].intensity,400),"sun lux");
        auto state=scene::read_linked_scene(root); check(bool(state),"links parsed");
        doc::NodeId copper{},paint{},key{},attachment{};
        for (const auto& [id,p] : state.value().occurrences) {
            if (p.name=="Material panels / Copper") copper=id;
            if (p.name=="Material panels / Paint") paint=id;
            if (p.name=="Key rectangle") key=id;
            attachment=p.instance;
        }
        check(copper.value() && paint.value() && key.value(),"stable selectable surfaces and light");
        render::ModelAsset asset;
        check(render::load_model_package(state.value().occurrences.at(copper).source,asset),"immutable textured package");
        check(asset.textures.size()==1 && asset.parts.size()==1,"one image and one material surface");
        check(asset.mesh.vertices.size()==3 && asset.bounds.maximum.x<0,"surface package compacts unused corners and bounds");
        const auto& material=asset.parts[0].material;
        check(near(material.roughness,0.35F) && near(material.metallic,0.7F)
            && near(material.base_alpha,0.8F) && material.alpha_mode==scene::MaterialAlphaMode::Mask
            && near(material.alpha_cutoff,0.4F) && material.double_sided && near(material.clearcoat,0.6F)
            && near(material.transmission,0.25F) && near(material.ior,1.45F),"numeric importer/PBR extension parity");
        const auto initial_digest=render::bake_input_digest(frame);
        auto reported=frame; reported.asset_manifest_digest=1234567;
        check(render::bake_input_digest(reported)==initial_digest,"transport readiness metadata cannot stale lighting");
        auto shot=*root.get(copper);shot.properties["roughness"]=0.9;
        auto edited=scene::plan_linked_change(root,root.set(shot)); check(bool(edited),"per-property material override");root=edited.value();
        auto light=*root.get(key);light.properties["intensity"]=900.0;
        edited=scene::plan_linked_change(root,root.set(light));check(bool(edited),"light shot override");root=edited.value();
        const auto texture=render::ingest_linked_scene((fixture/"look-r2.mslink").string());check(texture.diagnostic.empty(),texture.diagnostic);
        check(texture.manifest.ingested<=1 && texture.manifest.reused>=1,"one texture ingests independently");
        auto next=render::plan_linked_manifest(root,texture.manifest,preview);check(bool(next),"texture update");root=next.value();
        const auto reordered=render::ingest_linked_scene((fixture/"look-r3.mslink").string());check(reordered.diagnostic.empty(),reordered.diagnostic);
        check(reordered.manifest.ingested==0 && reordered.manifest.reused==2,"slot reorder reuses both packages");
        next=render::plan_linked_manifest(root,reordered.manifest,preview);check(bool(next),"reorder update");root=next.value();
        state=scene::read_linked_scene(root);check(bool(state),"reordered scene");
        check(near(state.value().occurrences.at(copper).material.roughness,0.9F)
            && near(state.value().occurrences.at(paint).material.roughness,0.35F),"override stays on copper after reorder");
        model.reset(root);frame=model.evaluate(preview,{});
        check(near(frame.point_lights[0].intensity,900),"source update retains light intensity override");
        check(render::bake_input_digest(frame)!=initial_digest,"look/light inputs invalidate lighting");
        auto transformed=scene::plan_linked_world(root,key,scene::translation({3,4,5}));check(bool(transformed),"light placement");
        model.reset(transformed.value());check(near(model.evaluate(preview,{}).point_lights[0].position.x,3),"light uses affine shot placement");
        check(render::bake_input_digest(model.evaluate(preview,{}))!=render::bake_input_digest(frame),"light placement invalidates probes");
        auto reset=scene::plan_linked_edit(root,key,scene::LinkedEdit::ResetMaterial);check(bool(reset),"reset light properties");
        model.reset(reset.value());check(near(model.evaluate(preview,{}).point_lights[0].intensity,1800),"light reset follows source");
        auto snapshot=scene::snapshot_link(root,attachment);check(bool(snapshot),"snapshot preserves lights");
        auto pasted=scene::paste_link_snapshot(root,snapshot.value(),preview);check(bool(pasted),"paste independent collection with native lights");
        model.reset(pasted.value());const auto copied=model.evaluate(preview,{});
        check(copied.models.size()==4 && copied.point_lights.size()==2 && copied.directional_lights.size()==2,
            "independent pasted copy retains both surfaces and both light types");
        auto offline_root=root;
        auto unlink=scene::plan_linked_edit(offline_root,attachment,scene::LinkedEdit::Unlink);check(bool(unlink),"unlink light and material scene");
        model.reset(unlink.value());check(model.evaluate(preview,{}).point_lights.size()==1,"unlinked light stays native");
        // Bake a tiny actual field plus a reflection capture. A completed job is
        // only current for this exact immutable dependency revision.
        auto excluded=*root.get(copper); excluded.properties["bake_static"]=false; root=root.set(excluded);
        auto stage=*root.get(preview);
        scene::BakedLighting settings;settings.enabled=true;settings.count_x=settings.count_y=settings.count_z=2;
        settings.samples=8;settings.bounces=1;settings.minimum={-2,0,-2};settings.maximum={2,3,2};
        settings.reflections[0].enabled=true;settings.reflections[0].source=doc::NodeId{50000};
        for (const auto& [k,v] : scene::baked_lighting_properties(settings)) stage.properties[k]=v;
        root=root.set(stage);model.reset(root);frame=model.evaluate(preview,{});
        check(render::baked_lighting_stale(frame),"unbaked authored lighting is stale");
        const auto baked=render::bake_lighting(frame);check(baked.ready(),baked.error);
        const auto written=render::write_probe_asset(*baked.asset,(output/"look-bake").string());check(written.ready(),written.error);
        check(written.asset->input_digest==render::bake_input_digest(frame),"file stores dependency revision");
        settings.source=written.source;settings.content=written.asset->content;settings.input_revision=std::to_string(written.asset->input_digest);
        for (const auto& [k,v] : scene::baked_lighting_properties(settings)) stage.properties[k]=v;
        root=root.set(stage);model.reset(root);frame=model.evaluate(preview,{});check(!render::baked_lighting_stale(frame),"completed field current");
        auto moved=scene::plan_linked_world(root,paint,scene::translation({1,0,0}));check(bool(moved),"static contributor move");
        model.reset(moved.value());auto stale=model.evaluate(preview,{});check(render::baked_lighting_stale(stale),"static transform marks GI and reflections stale");
        check(written.asset->input_digest!=render::bake_input_digest(stale),"old completed bake cannot become current");
        auto rebake=render::BakeJob::start(stale,(output/"new-bake-directory/superseded-bake").string());
        check(bool(rebake),"start requested rebake on worker");
        auto moved_again=scene::plan_linked_world(moved.value(),paint,scene::translation({2,0,0}));
        check(bool(moved_again),"edit contributor after submitting rebake");
        model.reset(moved_again.value());const auto newest=model.evaluate(preview,{});
        for (int attempt=0;attempt<3000 && !rebake->result();++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        check(rebake->result() && rebake->result()->ready(),"requested worker rebake completes");
        check(rebake->result()->asset->input_digest==render::bake_input_digest(stale)
            && rebake->result()->asset->input_digest!=render::bake_input_digest(newest),
            "rebake retains submitted snapshot and fails current-input completion fence after another edit");
        auto refused=render::prepare_linked_publication({moved.value(),preview,program,(output/"stale").string(),false,{}});
        check(refused.diagnostic.find("Stale")!=std::string::npos,"publication holds stale lighting");
        auto prepared=render::prepare_linked_publication({root,preview,program,(output/"shared").string(),false,{}});
        check(prepared.diagnostic.empty(),prepared.diagnostic);
        auto installed=render::install_linked_publication(root,prepared,program,doc::NodeId{60000});check(bool(installed),"publish geometry and lighting together");
        auto target=scene::publication::render_scene(installed.value(),program);
        for (int node=0;node<2;++node) check(render::prepare_linked_publication({installed.value(),target,{}, {},true,{}}).diagnostic.empty(),"cohort verifies complete look");
        check(bool(persist::save_project_file(output/"preview.mstage",root)),"save preview");
        check(bool(persist::save_recent_project_path(output/"local/Pixelsoup/settings/recent-project.bin",output/"preview.mstage")),"UI startup");
        check(bool(persist::save_project_file(output/"stale.mstage",moved.value())),"save stale fixture");
        check(bool(persist::save_project_file(output/"published.mstage",installed.value())),"offline publication");
        if (argc>3) {
            rhi::DeviceConfig config;config.debug=false;config.backend=rhi::BackendKind::D3d12;
            auto device=rhi::Device::create(config);check(bool(device),"D3D12 device");
            render::RendererLimits limits;limits.planar_reflections=false;
            auto created=render::SceneRenderer::create(device.value(),640,360,limits,render::SceneRendererMode::AdvancedLinear);
            check(bool(created),"Advanced renderer");auto renderer=std::move(created).value();
            bool ready=false;
            for (int attempt=0;attempt<150 && !ready;++attempt) {
                auto acquired=device.value().acquire_commands();check(bool(acquired),"commands");auto commands=std::move(acquired).value();
                if (renderer.render(commands,frame,{0,0,640,360})) {
                    auto submitted=device.value().submit(std::move(commands));check(bool(submitted) && submitted.value().wait(),"GPU submission");
                    const auto& stats=renderer.stats();ready=stats.models_pending==0 && stats.models_failed==0 && stats.dropped_instances==0 && stats.baked_ready;
                }
                if (!ready) { static_cast<void>(render::detail::wait_for_shared_model_cache_for_test());std::this_thread::sleep_for(std::chrono::milliseconds{10}); }
            }
            check(ready,"Advanced textured look, lights, GI/reflections become ready");
        }
        std::ofstream evidence{output/"look-evidence.json"};evidence<<"{\"surfaces\":2,\"native_lights\":2,\"texture_reused\":"<<texture.manifest.reused
            <<",\"reorder_ingested\":"<<reordered.manifest.ingested<<",\"stale_publication_refused\":true,\"cohort_verifiers\":2,\"gpu\":"<<(argc>3?"true":"false")<<"}";
        std::cout<<"Materials/light acceptance passed\n";return 0;
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
