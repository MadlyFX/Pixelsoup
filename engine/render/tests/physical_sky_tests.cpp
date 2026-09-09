#include <mediaserver/render/environment_prefilter.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/render/compositor_scene_executor.hpp>
#include <mediaserver/render/baked_lighting.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/scene/key_edit.hpp>
#include <mediaserver/persist/project_file.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace scene = mediaserver::scene;
namespace schema = scene::schema;
namespace render = mediaserver::render;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace rhi = mediaserver::rhi;
namespace persist = mediaserver::persist;
#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error(std::to_string(__LINE__)+": " #__VA_ARGS__); } while(false)
doc::Node node(core::u64 id, std::string_view type) { doc::Node n; n.id=doc::NodeId{id}; n.type=type; return n; }
doc::Root document() {
    auto stage=node(10,schema::stage_type);
    stage.properties={{"sky_physical_enabled",true},{"sky_time_of_day",16.5},{"sky_latitude",42.0},
        {"sky_north_rotation",0.125},{"sky_haze",2.5},{"sky_solar_illuminance",95000.0},
        {"sky_angular_radius",0.0015},{"sky_ground_albedo",0.35},
        {"canvas_width",std::int64_t{1280}},{"canvas_height",std::int64_t{720}}};
    auto settings=node(11,schema::settings_type);
    scene::RendererSettings renderer; renderer.backend=scene::RendererBackend::Advanced;
    scene::write_renderer_settings(settings,renderer);
    auto camera=node(12,schema::camera_type);
    camera.properties={{"eye_x",0.0},{"eye_y",1.5},{"eye_z",4.0},
        {"target_x",0.0},{"target_y",2.5},{"target_z",0.0},{"exposure_compensation_ev",-4.0}};
    return doc::Root{}.set(stage).set(settings).set(camera);
}
void model_contract() {
    const auto root=document(); scene::SceneModel model; model.reset(root);
    const auto s=model.evaluate({0,60,1}); const auto& sky=s.environment.physical_sky;
    CHECK(model.counters().rejected==0 && sky.enabled);
    CHECK(sky.time_of_day==16.5F && sky.latitude==42 && sky.north_rotation==0.125F);
    CHECK(sky.haze==2.5F && sky.solar_illuminance==95000 && sky.angular_radius==0.0015F && sky.ground_albedo==0.35F);
    CHECK(s.directional_lights.size()==1 && s.directional_lights[0].unit==scene::LightUnit::Lux);
    const auto path=std::filesystem::temp_directory_path()/"mediaserver-physical-sky-test.mstage";
    CHECK(persist::save_project_file(path,root)); auto saved=persist::load_project_file(path); CHECK(saved);
    scene::SceneModel replay; replay.reset(saved.value()); CHECK(scene::digest(replay.evaluate({0,60,1}))==scene::digest(s));
    std::filesystem::remove(path);
    float scene::PhysicalSky::* const fields[]={&scene::PhysicalSky::time_of_day,&scene::PhysicalSky::latitude,
        &scene::PhysicalSky::north_rotation,&scene::PhysicalSky::haze,&scene::PhysicalSky::solar_illuminance,
        &scene::PhysicalSky::angular_radius,&scene::PhysicalSky::ground_albedo};
    for (auto field:fields) { auto changed=s; changed.environment.physical_sky.*field+=0.0001F;
        if (field==&scene::PhysicalSky::solar_illuminance) changed.environment.physical_sky.*field+=10;
        CHECK(scene::digest(changed)!=scene::digest(s)); CHECK(render::bake_input_digest(changed)!=render::bake_input_digest(s)); }
    auto changed=s; changed.environment.physical_sky.enabled=false; CHECK(scene::digest(changed)!=scene::digest(s));
    auto key=node(20,schema::channel_type); key.properties={{"channel_target",std::int64_t{10}},
        {"channel_component",std::string{"sky_time_of_day"}},{"key0_frame",std::int64_t{0}},{"key0_value",8.0},
        {"key1_frame",std::int64_t{60}},{"key1_value",16.0}};
    const auto keyed=root.set(key); model.reset(keyed);
    CHECK(model.evaluate({30,60,1}).environment.physical_sky.time_of_day==12);
    CHECK(model.evaluate({0,60,1}).directional_lights[0].direction.x * model.evaluate({60,60,1}).directional_lights[0].direction.x < 0);
    const std::array<scene::KeyAddress,1> selection{{{doc::NodeId{10},scene::PropertyChannel::SkyTimeOfDay,0}}};
    auto inspected=scene::inspect_key_selection(keyed,selection); CHECK(inspected && inspected.value().records.size()==1);
    auto invalid=sky; invalid.haze=99; invalid.time_of_day=-8; invalid.latitude=100;
    invalid.angular_radius=std::numeric_limits<float>::quiet_NaN();
    const auto bounded=scene::clamped(invalid); CHECK(bounded.haze==8 && bounded.time_of_day==0 && bounded.latitude==89);
    CHECK(std::isfinite(bounded.angular_radius));
    std::cout<<"[pass] non-default persistence, digests, animation and key editing\n";
}
void physics_contract() {
    scene::PhysicalSky s; s.enabled=true; s.latitude=0;
    auto d=scene::physical_sun_direction(s); CHECK(d.y>0.999F);
    auto noon=scene::physical_sun_illuminance(s); CHECK(noon.x>noon.z && noon.z>50000 && noon.x<120000);
    auto zenith=scene::physical_sky_radiance(s,{0,1,0}); CHECK(zenith.z>zenith.x && zenith.x>0);
    s.time_of_day=17.75F; auto evening=scene::physical_sun_illuminance(s);
    CHECK(evening.x<noon.x && evening.x/evening.z>noon.x/noon.z*2);
    s.time_of_day=0; CHECK(scene::physical_sun_direction(s).y < -0.999F);
    CHECK(scene::physical_sun_illuminance(s)==scene::Vec3{});
    CHECK(scene::physical_sky_radiance(s,{0,1,0})==scene::Vec3{});
    std::cout<<"[pass] solar arc, spectral extinction, blue zenith and night\n";
}
void bake_contract() {
    scene::Scene s;
    s.environment.physical_sky.enabled=true;
    s.environment.map_source="unused-physical-bake-environment.hdr";
    auto& grid=s.environment.baked;
    grid.minimum={-1,-1,-1}; grid.maximum={1,1,1};
    grid.count_x=grid.count_y=grid.count_z=2; grid.samples=128; grid.bounces=1;
    const auto baked=render::bake_lighting(s); CHECK(baked.ready());
    const auto diffuse=render::sample_probe_irradiance(*baked.asset,{0,0,0},{0,1,0});
    CHECK(diffuse.z>diffuse.x && diffuse.x>0.1F);
    CHECK(render::decode_probe_asset(render::encode_probe_asset(*baked.asset)).ready());
    std::cout<<"[pass] physical-sky diffuse bake and immutable asset roundtrip\n";
}
rhi::CommandList acquire(rhi::Device& d) { auto c=d.acquire_commands(); CHECK(c); return std::move(c).value(); }
void submit(rhi::Device& d,rhi::CommandList c) { auto s=d.submit(std::move(c)); CHECK(s && s.value().wait()); }
std::vector<core::u8> bytes(rhi::Device& d,const rhi::Texture& t,unsigned stride,unsigned mip=0,unsigned layer=0) {
    auto rb=d.create_readback(std::max(t.width()>>mip,1U)*std::max(t.height()>>mip,1U)*stride); CHECK(rb);
    auto c=acquire(d); CHECK(c.download(t,rb.value(),mip,layer)); submit(d,std::move(c));
    auto m=rb.value().map(); std::vector<core::u8> v(m.begin(),m.end()); rb.value().unmap(); return v;
}
float half(core::u16 b) { unsigned e=(b>>10)&31; CHECK(e!=31); return (b&32768?-1.0F:1.0F)*(e?std::ldexp(1+static_cast<float>(b&1023)/1024,static_cast<int>(e)-15):std::ldexp(static_cast<float>(b&1023),-24)); }
float pixel(const std::vector<core::u8>& b,unsigned width,unsigned x,unsigned y,unsigned channel) { core::u16 v; std::memcpy(&v,b.data()+(y*width+x)*8+channel*2,2); return half(v); }
void gpu_contract(rhi::Device& device) {
    render::EnvironmentPrefilterLimits lim{32,4,32,32,64,64};
    auto a=render::EnvironmentPrefilter::create(device,lim), b=render::EnvironmentPrefilter::create(device,lim); CHECK(a&&b);
    scene::PhysicalSky sky; sky.enabled=true; sky.time_of_day=15.5F; sky.haze=2;
    const auto update=[&](auto& filter) { auto c=acquire(device); CHECK(filter.prepare(c)&&filter.record_physical(c,sky,0.00314159265F)); submit(device,std::move(c)); };
    update(a.value()); update(b.value());
    const auto table=bytes(device,*a.value().scattering_table(),8);
    CHECK(table==bytes(device,*b.value().scattering_table(),8));
    const auto sh=bytes(device,*a.value().maps().irradiance,16);
    CHECK(sh==bytes(device,*b.value().maps().irradiance,16));
    for(unsigned face=0;face<6;++face) for(unsigned mip=0;mip<4;++mip)
        CHECK(bytes(device,*a.value().maps().specular,8,mip,face)==bytes(device,*b.value().maps().specular,8,mip,face));
    update(a.value()); CHECK(table==bytes(device,*a.value().scattering_table(),8));
    // Match the independently evaluated CPU bake transport at actual table rays.
    constexpr float pi=3.14159265358979323846F;
    for(unsigned y : {8U,24U,40U,50U}) {
        const unsigned x=83; const float theta=(static_cast<float>(y)+0.5F)/128*pi;
        const float phi=((static_cast<float>(x)+0.5F)/256-0.5F)*2*pi;
        const auto cpu=scene::physical_sky_radiance(sky,{std::cos(phi)*std::sin(theta),std::cos(theta),std::sin(phi)*std::sin(theta)})*0.00314159265F;
        CHECK(std::abs(pixel(table,256,x,y,0)-cpu.x)<std::max(cpu.x*0.02F,0.003F));
        CHECK(std::abs(pixel(table,256,x,y,2)-cpu.z)<std::max(cpu.z*0.02F,0.003F));
    }
    const auto sharp=bytes(device,*a.value().maps().specular,8,1,2);
    sky.angular_radius=0.02F; update(a.value());
    CHECK(sh==bytes(device,*a.value().maps().irradiance,16));
    CHECK(sharp!=bytes(device,*a.value().maps().specular,8,1,2));
    sky.time_of_day=12; update(a.value()); CHECK(sh!=bytes(device,*a.value().maps().irradiance,16));
    { auto canceled=acquire(device); CHECK(a.value().record_physical(canceled,sky,0.00314159265F)); }
    CHECK(!a.value().maps().complete()); update(a.value()); CHECK(a.value().maps().complete());
    std::cout<<"[pass] repeated and peer table/SH/all specular mips; elevation changes SH; cancellation recovery\n";
    render::RendererLimits limits; limits.max_instances=8; limits.max_lights=32; limits.max_vertices=1024; limits.max_indices=4096;
    limits.directional_shadow_resolution=16; limits.shadow_atlas={32,8,16,32};
    auto full=render::SceneRenderer::create(device,128,64,limits,render::SceneRendererMode::AdvancedLinear);
    auto left=render::SceneRenderer::create(device,64,64,limits,render::SceneRendererMode::AdvancedLinear);
    auto right=render::SceneRenderer::create(device,64,64,limits,render::SceneRendererMode::AdvancedLinear); CHECK(full&&left&&right);
    scene::Scene s; s.canvas={128,64}; s.camera.eye={0,1,3}; s.camera.target={0,1,0};
    s.renderer_settings.backend=scene::RendererBackend::Advanced; s.renderer_settings.shadow_cascades=0;
    s.environment.physical_sky=sky; s.environment.map_source="this-unused-map-must-not-be-loaded.hdr";
    scene::MeshInstance box; box.source=doc::NodeId{2}; box.mesh={scene::Primitive::Box,1};
    box.world=scene::translation({0,0.7F,0}); box.normal=scene::normal_basis(box.world);
    box.material.metallic=0.8F; box.material.roughness=0.25F; s.instances.push_back(box);
    const auto draw=[&](auto& renderer,scene::CanvasRect rect) {
        auto cmd=acquire(device); CHECK(renderer.render(cmd,s,rect)); submit(device,std::move(cmd));
        return bytes(device,renderer.target(),8);
    };
    for (float time : {8.0F,12.0F,17.25F,0.0F}) {
        s.environment.physical_sky.time_of_day=time; s.directional_lights={scene::physical_sun(s.environment.physical_sky,doc::NodeId{10})};
        const auto image=draw(full.value(),scene::whole(s.canvas)); CHECK(full.value().stats().physical_sky_updates==1);
        CHECK(draw(full.value(),scene::whole(s.canvas))==image && full.value().stats().physical_sky_updates==0);
        const auto l=draw(left.value(),{0,0,64,64}), r=draw(right.value(),{64,0,64,64});
        for(unsigned y=0;y<64;++y) {
            CHECK(std::equal(l.begin()+y*512,l.begin()+(y+1)*512,image.begin()+y*1024));
            CHECK(std::equal(r.begin()+y*512,r.begin()+(y+1)*512,image.begin()+y*1024+512));
        }
    }
    s.environment.physical_sky.enabled=false; s.environment.map_source.clear(); s.directional_lights.clear();
    const auto off=draw(full.value(),scene::whole(s.canvas));
    s.environment.physical_sky.enabled=true; s.environment.physical_sky.time_of_day=12;
    CHECK(draw(full.value(),scene::whole(s.canvas))!=off);
    std::cout<<"[pass] full canvas equals two slices at four solar times, held reuse and mode switching\n";

    auto composite=render::CompositorSceneExecutor::create(device,128,64,limits,{},rhi::TextureFormat::Rgba16Float);
    auto peer=render::CompositorSceneExecutor::create(device,128,64,limits,{},rhi::TextureFormat::Rgba16Float);
    CHECK(composite && peer);
    s.instances.clear(); s.environment.map_source="unused-while-physical.hdr";
    const auto compose=[&](auto& executor) {
        auto cmd=acquire(device); auto result=executor.execute(cmd,s,scene::whole(s.canvas)); CHECK(result);
        CHECK(executor.renderer_passes().geometry.assets.ready);
        CHECK(executor.renderer_passes().geometry.assets.digest==scene::asset_set_digest(s));
        CHECK(executor.stats().environment_backgrounds==1);
        submit(device,std::move(cmd)); return bytes(device,*result.value(),8);
    };
    std::vector<core::u8> previous;
    for (bool geometry : {false,true}) {
        if(geometry) s.instances.push_back(box);
        for (float time : {8.0F,12.0F,17.25F,0.0F}) {
            s.environment.physical_sky.time_of_day=time;
            s.directional_lights={scene::physical_sun(s.environment.physical_sky,doc::NodeId{10})};
            const auto current=compose(composite.value());
            CHECK(current!=previous);
            CHECK(compose(composite.value())==current); // No one-frame adoption lag.
            CHECK(compose(peer.value())==current);
            if(time==12) CHECK(pixel(current,128,64,8,2)>1.0F); // HDR survives layer composition.
            previous=current;
        }
    }
    std::cout<<"[pass] compositor sky-only startup readiness and same-frame sky/lighting changes\n";
}
int main(int argc,char** argv) { try {
    if(argc==3 && std::string_view{argv[1]}=="--write-demo") { CHECK(persist::save_project_file(argv[2],document())); return 0; }
    model_contract(); physics_contract();
    if(argc==1) bake_contract();
    if(argc>1 && std::string_view{argv[1]}=="--gpu") {
        if(!rhi::Device::available(rhi::BackendKind::D3d12)) return 77;
        rhi::DeviceConfig config; config.backend=rhi::BackendKind::D3d12;
        auto d=rhi::Device::create(config); CHECK(d); gpu_contract(d.value());
    }
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }
