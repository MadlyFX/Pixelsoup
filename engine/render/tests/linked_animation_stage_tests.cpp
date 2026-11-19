#include <mediaserver/render/linked_scene_ingest.hpp>
#include <mediaserver/render/linked_publication.hpp>
#include <mediaserver/render/linked_stage_context.hpp>
#include <mediaserver/render/model_animation.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/render/scene_bridge.hpp>
#include <mediaserver/render/environment_budget.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>
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
void check(bool value,const std::string& why) { if (!value) throw std::runtime_error{why}; }
int main(int argc,char** argv) {
    try {
        check(argc>=3,"fixture and output required");
        const fs::path input=fs::absolute(argv[1]), output=fs::absolute(argv[2]);fs::create_directories(output);
        doc::Root root;
        const doc::NodeId preview{input.extension()==".mstage" ? 1ULL : 10000ULL},program{10001};
        if (input.extension()==".mstage") {
            auto loaded=persist::load_project_file(input);check(bool(loaded),"load real Blender project");root=loaded.value();
        } else {
            root=root.set({preview,std::string{ss::stage_type},{}});
            root=root.set(show::make_switcher_node({program,preview,program,program,{},0}));
            // Reserve the demo's authored nodes before allocating link records.
            root=root.set({program,std::string{ss::stage_type},{}});
            root=root.set({doc::NodeId{10002},std::string{ss::camera_type},{}});
            root=root.set({doc::NodeId{10003},std::string{ss::settings_type},{}});
            const auto imported=render::ingest_linked_scene((input/"animation.mslink").string());check(imported.diagnostic.empty(),imported.diagnostic);
            auto planned=render::plan_linked_manifest(root,imported.manifest,preview);check(bool(planned),planned ? "" : planned.error().message);root=planned.value();
        }
        auto stage=*root.get(preview);stage.properties["scene_name"]=std::string{"Animation Preview"};
        stage.properties["canvas_width"]=core::i64{640};stage.properties["canvas_height"]=core::i64{360};stage.properties["ambient"]=0.6;
        root=root.set(stage).set({program,std::string{ss::stage_type},{{"scene_name",std::string{"Program"}}}});
        scene::Camera aim{}; aim.eye=scene::Vec3{3.0F,2.0F,8.0F}; aim.target=scene::Vec3{3.0F,0.5F,-1.0F};
        const scene::Vec3 turns=scene::camera_rotation(aim);
        root=root.set({doc::NodeId{10002},std::string{ss::camera_type},{{"scene_id",static_cast<core::i64>(preview.value())},
            {"position_x",3.0},{"position_y",2.0},{"position_z",8.0},{"rotation_yaw",(static_cast<double>(turns.x)+0.0)},
            {"rotation_pitch",(static_cast<double>(turns.y)+0.0)},{"rotation_roll",(static_cast<double>(turns.z)+0.0)},{"fov_turns",0.17}}});
        root=root.set({doc::NodeId{10003},std::string{ss::settings_type},{{"renderer_backend",std::string{"advanced"}}}});
        auto links=scene::read_linked_scene(root);check(bool(links),links ? "" : links.error().message);
        check(links.value().occurrences.size()==1,"one assembly occurrence");
        const auto occurrence=links.value().occurrences.begin()->second;
        check(occurrence.animated && occurrence.animation.start_frame==10,"source animation binding and start");
        auto moved=scene::plan_linked_world(root,occurrence.node,scene::translation({3,0,0}));check(bool(moved),"shot offset");root=moved.value();
        auto held=scene::plan_linked_edit(root,occurrence.node,scene::LinkedEdit::Hold);check(bool(held),"hold assembly placement");root=held.value();
        render::ModelAsset asset;
        check(render::load_model_package(occurrence.source,asset),"load node/skin package");
        check(!asset.animation_nodes.empty() && asset.pose_bindings.size()>=2 && asset.animations.size()==1,"preserved node hierarchy and skin bindings");
        auto scratch=std::make_unique<render::ModelPoseScratch>();
        std::vector<scene::Mat4> first(asset.pose_bindings.size()),middle(first.size()),last(first.size());
        check(render::evaluate_model_pose(asset,occurrence.animation,10,{24,1},*scratch,first),"start pose");
        check(render::evaluate_model_pose(asset,occurrence.animation,22,{24,1},*scratch,middle),"middle pose");
        check(render::evaluate_model_pose(asset,occurrence.animation,34,{24,1},*scratch,last),"loop pose");
        check(first != middle && first == last,"animated parent moves with exact loop endpoint");
        if (input.extension()!=".mstage") check(std::abs(middle[0].m[12]-1)<1e-5F,"known parent translation at half second");
        auto settings=occurrence.animation;settings.loop=false;
        check(render::evaluate_model_pose(asset,settings,10000,{24,1},*scratch,last),"clamped final pose");
        check(last != first && last != middle,"non-looping clip holds final pose");
        std::ifstream reference_poses{input.parent_path()/"pose-reference.txt"};
        core::u64 source_frame{};std::size_t numeric_samples{};
        while (reference_poses >> source_frame) {
            float expected[6]{};for (auto& v : expected) reference_poses >> v;
            check(bool(reference_poses),"Blender numeric pose row");
            check(render::evaluate_model_pose(asset,settings,settings.start_frame+source_frame-1,{24,1},*scratch,last),"Blender pose comparison");
            scene::Vec3 minimum{1e10F,1e10F,1e10F},maximum{-1e10F,-1e10F,-1e10F};
            for (std::size_t i=0;i<asset.mesh.vertices.size();++i) {
                const auto& v=asset.vertex_metadata[i];const auto bind=asset.mesh.vertices[i].position;
                const float weights[]{v.weights.x,v.weights.y,v.weights.z,v.weights.w};scene::Vec3 p{};
                for (std::size_t j=0;j<4;++j) p=p+scene::transform_point(last[v.joints[j]],bind)*weights[j];
                minimum={std::min(minimum.x,p.x),std::min(minimum.y,p.y),std::min(minimum.z,p.z)};
                maximum={std::max(maximum.x,p.x),std::max(maximum.y,p.y),std::max(maximum.z,p.z)};
            }
            const float actual[]{minimum.x,minimum.y,minimum.z,maximum.x,maximum.y,maximum.z};
            for (std::size_t i=0;i<6;++i) check(std::abs(actual[i]-expected[i])<1e-4F,"Renderer pose matches Blender's evaluated node/constraint/skin bounds");
            ++numeric_samples;
        }
        check(render::model_animation_time(10,{24000,1001},settings,1)==0,"rational show start");
        auto snapshot=scene::snapshot_link(root,occurrence.instance);check(bool(snapshot),"independent animation snapshot");
        scene::SceneModel independent;independent.reset(snapshot.value().set(*root.get(preview)));
        const auto copied=independent.evaluate(preview,{});
        check(copied.models.size()==1 && copied.models[0].animation.start_frame==10,"independent copy retains source timing");
        const auto budget=render::measure_environment(root,preview);check(budget.diagnostic.empty(),budget.diagnostic);
        check(budget.pose_bindings==asset.pose_bindings.size(),"animation palette admission");
        check(bool(persist::save_project_file(output/"preview.mstage",root)),"save offline preview");
        check(bool(persist::save_recent_project_path(output/"local/Pixelsoup/settings/recent-project.bin",output/"preview.mstage")),"UI startup");
        auto reopened=persist::load_project_file(output/"preview.mstage");check(bool(reopened),"restart without Blender");
        scene::SceneModel a,b;a.reset(root);b.reset(reopened.value());
        for (core::u64 f : {0ULL,10ULL,22ULL,33ULL,34ULL,4001ULL,22ULL,0ULL}) {
            const auto x=a.evaluate(preview,{f,24000,1001}), y=b.evaluate(preview,{f,24000,1001});
            check(scene::digest(x)==scene::digest(y) && x.models.size()==1,"two node scenes agree after seek/restart");
            check(x.models[0].world.m[12]==3 && !x.models[0].bake_static,"held shot stays outside animation and dynamic GI");
            check(render::evaluate_model_pose(asset,x.models[0].animation,f,x.show_rate,*scratch,first),"node A pose");
            render::ModelAsset reloaded;check(render::load_model_package(y.models[0].source,reloaded),"node B independent resource");
            check(render::evaluate_model_pose(reloaded,y.models[0].animation,f,y.show_rate,*scratch,last) && first==last,"node pose equality at rational show frame");
        }
        const auto published=render::prepare_linked_publication({root,preview,program,(output/"shared").string(),false,{}});
        check(published.diagnostic.empty(),published.diagnostic);
        for (int node=0;node<2;++node) {
            const auto verified=render::prepare_linked_publication({published.document,published.scene,{}, {},true,{}});
            check(verified.diagnostic.empty(),verified.diagnostic);
        }
        auto frame=a.evaluate(preview,{22,24,1});
        auto reference=render::linked_stage_sample(frame,100);
        check(reference.show_frame==22 && reference.rate.numerator==24 && reference.camera_world.m[12]==3,"reference camera/time");
        const auto text=render::linked_stage_text(reference,scene::translation({2,0,0}));
        check(text.starts_with("MSSTAGE 1\n"),"bounded stage wire");
        scene::TrackedCameraView tracked;tracked.camera=doc::NodeId{99};tracked.source=doc::NodeId{98};
        tracked.icvfx.inner_frustum=true;tracked.pose.valid=true;tracked.pose.held=true;
        tracked.pose.position={1,2,3};tracked.authored.lens_profile=scene::LensProfile{};
        tracked.authored.lens_profile->sensor_width_mm=36;tracked.authored.lens_profile->sensor_height_mm=24;
        tracked.authored.lens_profile->focal_length_mm=50;
        frame.tracked_cameras.push_back(tracked);
        const auto calibrated=render::linked_stage_sample(frame,101);
        check(calibrated.tracked && calibrated.held && calibrated.calibrated && calibrated.camera==99
            && calibrated.focal_length_mm==50 && calibrated.sensor_height_mm==24 && calibrated.camera_world.m[12]==1,
            "physical camera lens and held sample metadata");
        frame.tracked_cameras.clear();
        if (argc>3) {
            rhi::DeviceConfig config;config.debug=false;config.backend=rhi::BackendKind::D3d12;
            auto device=rhi::Device::create(config);check(bool(device),"D3D12 device");
            render::RendererLimits limits;limits.planar_reflections=false;
            auto created=render::SceneRenderer::create(device.value(),640,360,limits,render::SceneRendererMode::AdvancedLinear);
            check(bool(created),"Advanced renderer");auto renderer=std::move(created).value();
            for (core::u64 f : {10ULL,11ULL,22ULL,23ULL,10ULL}) {
                frame=a.evaluate(preview,{f,24,1});bool ready=false;
                for (int attempt=0;attempt<150 && !ready;++attempt) {
                    auto acquired=device.value().acquire_commands();check(bool(acquired),"commands");auto commands=std::move(acquired).value();
                    if (renderer.render(commands,frame,{0,0,640,360})) {
                        auto submitted=device.value().submit(std::move(commands));check(bool(submitted) && submitted.value().wait(),"GPU submission");
                        const auto& stats=renderer.stats();ready=stats.models_pending==0 && stats.models_failed==0 && stats.dropped_instances==0;
                    }
                    if (!ready) {static_cast<void>(render::detail::wait_for_shared_model_cache_for_test());std::this_thread::sleep_for(std::chrono::milliseconds{10});}
                }
                check(ready,"Advanced skin/node animation after sequential frames and seek");
            }
        }
        std::ofstream evidence{output/"animation-evidence.json"};evidence<<"{\"bindings\":"<<asset.pose_bindings.size()<<",\"blender_numeric_samples\":"<<numeric_samples<<",\"node_readers\":2,\"rational_seek_restart\":true,\"hold_placement\":true,\"cohort_verifiers\":2,\"gpu\":"<<(argc>3?"true":"false")<<"}";
        std::cout<<"Animation/stage acceptance passed\n";return 0;
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
