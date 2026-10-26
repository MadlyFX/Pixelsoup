#include <mediaserver/render/baked_lighting.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
namespace render = mediaserver::render;
namespace scene = mediaserver::scene;
namespace rhi = mediaserver::rhi;
namespace doc = mediaserver::doc;
namespace core = mediaserver::core;
#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error(std::to_string(__LINE__) + ": " #__VA_ARGS__); } while (false)
float half(core::u16 bits) {
    const auto e = (bits >> 10) & 31;
    const float m = static_cast<float>(bits & 1023);
    return ((bits & 32768) ? -1.0F : 1.0F) * (e ? std::ldexp(1 + m / 1024, static_cast<int>(e) - 15) : std::ldexp(m, -24));
}
float pixel(const std::vector<core::u8>& bytes, unsigned width, unsigned x, unsigned y, unsigned channel = 0) {
    core::u16 value; std::memcpy(&value, bytes.data() + (y * width + x) * 8 + channel * 2, 2); return half(value);
}
rhi::CommandList acquire(rhi::Device& device) {
    auto result = device.acquire_commands(); CHECK(result); return std::move(result).value();
}
std::vector<core::u8> readback(rhi::Device& device, rhi::CommandList cmd, const rhi::Texture& target, unsigned stride) {
    auto rb = device.create_readback(target.width() * target.height() * stride); CHECK(rb);
    CHECK(cmd.download(target, rb.value()));
    auto submitted = device.submit(std::move(cmd)); CHECK(submitted && submitted.value().wait());
    const auto mapped = rb.value().map(); std::vector<core::u8> bytes(mapped.begin(), mapped.end()); rb.value().unmap(); return bytes;
}

render::RendererLimits limits() {
    render::RendererLimits l; l.max_instances = 16; l.max_lights = 8; l.max_vertices = 16384; l.max_indices = 65536;
    l.directional_shadow_resolution = 16; l.shadow_atlas = {32, 8, 16, 32}; return l;
}
scene::MeshInstance object(core::u64 id, scene::Vec3 position, scene::Vec3 size, scene::Vec3 emission = {}) {
    scene::MeshInstance o; o.source = doc::NodeId{id}; o.mesh = {scene::Primitive::Box, 1};
    o.world = scene::multiply(scene::translation(position), scene::scaling(size)); o.normal = scene::normal_basis(o.world);
    o.material.base_color = {1,1,1}; o.material.emissive = emission; o.material.emissive_strength = 1;
    return o;
}
scene::Scene fixture() {
    scene::Scene s; s.canvas = {128,96}; s.camera.eye = {0,2,4}; s.camera.target = {0,0.5F,0};
    s.camera.field_of_view = 0.2F; s.camera.near_plane = 0.1F; s.camera.far_plane = 40;
    s.renderer_settings.backend = scene::RendererBackend::Advanced;
    s.renderer_settings.shadow_cascades = 0; s.renderer_settings.gi_strength = 0;
    s.environment.sky_color = s.environment.ground_color = {}; s.environment.ambient = 0;
    s.environment.baked.count_x = s.environment.baked.count_y = s.environment.baked.count_z = 2;
    s.environment.baked.samples = 64; s.environment.baked.bounces = 1;
    return s;
}
void model_contract() {
    auto b = fixture().environment.baked;
    for (std::size_t i=0; i<b.reflections.size(); ++i) {
        auto& p=b.reflections[i]; p.source=doc::NodeId{0x102030405ULL+i}; p.enabled=true; p.sphere=i%2==0;
        const auto n=static_cast<float>(i); p.position={n+0.25F,n+1.5F,n-3.25F}; p.extent={n+2.5F,n+3.5F,n+4.5F}; p.blend=0.125F+n*0.125F; p.intensity=2.25F+n;
    }
    scene::PlanarReflection p; p.enabled=true; p.receiver=doc::NodeId{0x203040507ULL}; p.point={1.25F,-2.5F,4.75F}; p.normal={0,0,1}; p.strength=0.625F;
    doc::Node stage; stage.id=doc::NodeId{1}; stage.type=std::string{scene::schema::stage_type}; stage.properties=scene::baked_lighting_properties(b);
    for (const auto& [key,value]:scene::planar_reflection_properties(p)) stage.properties.emplace(key,value);
    scene::SceneModel model; model.reset(doc::Root{}.set(stage)); const auto s=model.evaluate({0,60,1});
    CHECK(s.environment.baked==b); CHECK(s.environment.planar==p);
    const auto digest=scene::digest(s); auto changed=s;
    changed.environment.baked.reflections[2].blend=0.75F; CHECK(scene::digest(changed)!=digest);
    changed=s; changed.environment.planar.point.x+=1; CHECK(scene::digest(changed)!=digest);
    changed=s; changed.environment.baked.source="new.msgi"; CHECK(scene::asset_set_digest(changed)!=scene::asset_set_digest(s));
    auto invalid=b.reflections[0]; invalid.extent.x=-1; invalid.blend=0; invalid.position.y=std::numeric_limits<float>::infinity();
    const auto bounded=scene::clamped(invalid); CHECK(bounded.extent.x==0.01F && bounded.blend==0.001F && bounded.position.y==1);
    // Independent signed-distance property for an arbitrary, translated plane.
    p.point={1,2,3}; p.normal=scene::normalize({1,2,-3}); const auto mirror=scene::reflection_matrix(p);
    const scene::Vec3 q{-4,7,2}; const auto reflected=scene::transform_point(mirror,q);
    CHECK(std::abs(scene::dot(p.normal,reflected-p.point)+scene::dot(p.normal,q-p.point))<1e-5F);
    CHECK(scene::length(scene::transform_point(mirror,reflected)-q)<1e-5F);
    CHECK(scene::length(scene::transform_point(mirror,p.point)-p.point)<1e-5F);
}
render::BakeResult bake_contract() {
    auto s=fixture(); s.instances.push_back(object(7,{-2,1,0},{0.1F,6,8},{4,0,0}));
    auto& p=s.environment.baked.reflections[0]; p.enabled=true; p.source=doc::NodeId{7}; p.position={0,1,0}; p.extent={6,6,6};
    const auto first=render::bake_lighting(s); CHECK(first.ready());
    const auto encoded=render::encode_probe_asset(*first.asset); CHECK(encoded==render::encode_probe_asset(*render::bake_lighting(s).asset));
    s.camera.eye={100,50,80}; s.show_frame=123456789; CHECK(encoded==render::encode_probe_asset(*render::bake_lighting(s).asset));
    // -X points at the emissive red wall. +X points at a black surround.
    const auto& capture=first.asset->reflections[0];
    CHECK(capture[64*render::reflection_capture_width].x>3.9F);
    CHECK(capture[64*render::reflection_capture_width+128].x==0);
    auto corrupt=encoded; corrupt[corrupt.size()/2]^=1; CHECK(!render::decode_probe_asset(corrupt).ready());
    for (std::size_t cut : {std::size_t{0},std::size_t{59},encoded.size()-1}) CHECK(!render::decode_probe_asset(std::span{encoded}.first(cut)).ready());
    s.environment.baked.reflections[1]=p; CHECK(!render::bake_lighting(s).ready());
    auto path=std::filesystem::temp_directory_path()/"mediaserver-step27"; std::filesystem::create_directories(path);
    return render::write_probe_asset(*first.asset,(path/"reflection").string());
}
void gpu_contract(rhi::Device& device, const render::BakeResult& baked) {
    auto full=render::SceneRenderer::create(device,128,96,limits(),render::SceneRendererMode::AdvancedLinear); if (!full) std::cerr << "renderer creation error " << static_cast<int>(full.error()) << std::endl; CHECK(full);
    auto left=render::SceneRenderer::create(device,64,96,limits(),render::SceneRendererMode::AdvancedLinear); CHECK(left);
    auto right=render::SceneRenderer::create(device,64,96,limits(),render::SceneRendererMode::AdvancedLinear); CHECK(right);
    // The mirror is a second complete renderer and is built when a scene
    // authors a reflection, not with its owner. This fixture authors one
    // below, so these say so; the production path says it through
    // HybridSceneRenderer::prepare when it adopts the scene.
    full.value().ensure_planar(); left.value().ensure_planar(); right.value().ensure_planar();
    CHECK(full.value().planar_resident() && left.value().planar_resident() && right.value().planar_resident());
    auto s=fixture(); const auto whole=scene::whole(s.canvas);
    const auto draw=[&](render::SceneRenderer& renderer,scene::CanvasRect slice) {
        auto commands=acquire(device); CHECK(renderer.render(commands,s,slice)); return readback(device,std::move(commands),renderer.target(),8);
    };
    const auto exact=[&](const auto& image) {
        const auto a=draw(left.value(),{0,0,64,96}), b=draw(right.value(),{64,0,64,96});
        for (std::size_t y=0;y<96;++y) {
            CHECK(std::equal(a.begin()+y*512,a.begin()+(y+1)*512,image.begin()+y*1024));
            CHECK(std::equal(b.begin()+y*512,b.begin()+(y+1)*512,image.begin()+y*1024+512));
        }
    };
    s.instances.push_back(object(2,{0,-0.05F,0},{12,0.1F,12}));
    s.instances.back().material.metallic=1; s.instances.back().material.roughness=0.05F;
    s.instances.push_back(object(3,{0,1,0},{1,1,1},{3,0,0}));
    const auto plain=draw(full.value(),whole);
    s.environment.planar.enabled=true; s.environment.planar.receiver=doc::NodeId{2};
    const auto reflected=draw(full.value(),whole); CHECK(reflected!=plain); CHECK(full.value().stats().planar_passes==1); CHECK(full.value().stats().assets.ready);
    const auto projection=scene::view_for_slice(s.camera,s.canvas,whole).view_projection;
    const auto q=scene::transform(projection,{0,-1,0,1});
    const auto x=static_cast<unsigned>((q.x/q.w*0.5F+0.5F)*128), y=static_cast<unsigned>((0.5F-q.y/q.w*0.5F)*96);
    CHECK(x<128 && y<96); CHECK(pixel(reflected,128,x,y)>1); CHECK(pixel(plain,128,x,y)==0);
    CHECK(reflected==draw(full.value(),whole)); exact(reflected);
    // A compositor-isolated floor still reflects the box from the complete
    // scene, and subsequent layers can reuse that one capture.
    auto isolated=s; isolated.instances.resize(1);
    render::SceneRenderOptions isolated_options{}; isolated_options.residency_scene=&s;
    auto isolated_commands=acquire(device); CHECK(full.value().render(isolated_commands,isolated,whole,isolated_options));
    const auto isolated_image=readback(device,std::move(isolated_commands),full.value().target(),8);
    CHECK(pixel(isolated_image,128,x,y)>1);
    isolated_options.shared_planar=full.value().planar_target(); CHECK(isolated_options.shared_planar);
    auto borrowed_commands=acquire(device); CHECK(full.value().render(borrowed_commands,isolated,whole,isolated_options));
    CHECK(full.value().stats().planar_passes==0);
    CHECK(readback(device,std::move(borrowed_commands),full.value().target(),8)==isolated_image);
    auto borrower_limits=limits(); borrower_limits.planar_reflections=false;
    auto borrower=render::SceneRenderer::create(device,128,96,borrower_limits,render::SceneRendererMode::AdvancedLinear); CHECK(borrower);
    auto shared_commands=acquire(device); CHECK(borrower.value().render(shared_commands,isolated,whole,isolated_options));
    CHECK(borrower.value().stats().planar_passes==0 && borrower.value().stats().planar_refusals==0);
    CHECK(borrower.value().stats().assets.ready);
    CHECK(readback(device,std::move(shared_commands),borrower.value().target(),8)==isolated_image);
    // An omitted shared capture must retain a specific readiness failure.
    auto missing_options=isolated_options; missing_options.shared_planar=nullptr;
    auto missing_commands=acquire(device); CHECK(borrower.value().render(missing_commands,isolated,whole,missing_options));
    CHECK(borrower.value().stats().planar_refusals==1 && !borrower.value().stats().assets.ready);
    static_cast<void>(readback(device,std::move(missing_commands),borrower.value().target(),8));

    s.instances.push_back(object(4,{0,-0.8F,1},{2,0.5F,1},{0,8,0}));
    CHECK(draw(full.value(),whole)==reflected); // Geometry behind the plane is clipped.
    s.instances.pop_back();
    s.environment.planar.receiver=doc::NodeId{999}; static_cast<void>(draw(full.value(),whole)); CHECK(full.value().stats().planar_refusals==1 && !full.value().stats().assets.ready);
    s=fixture(); s.camera.eye={0,1,4}; s.camera.target={0,1,0};
    auto sphere=object(20,{0,1,0},{2,2,2}); sphere.mesh={scene::Primitive::Sphere,32}; sphere.material.metallic=1; sphere.material.roughness=0.08F; sphere.bake_static=false;
    s.instances.push_back(sphere); const auto dark=draw(full.value(),whole);
    s.environment.baked.reflections=baked.asset->grid.reflections; s.environment.baked.source=baked.source; s.environment.baked.content=baked.asset->content;
    render::ProbeCache cache; static_cast<void>(cache.observe(baked.source,baked.asset->content)); cache.wait_for_test();
    // Cancel the first upload, then require the complete prefilter + BRDF to retry.
    { auto canceled=acquire(device); CHECK(full.value().render(canceled,s,whole)); }
    const auto chrome=draw(full.value(),whole); CHECK(full.value().stats().reflection_probes==1 && full.value().stats().assets.ready); CHECK(chrome!=dark);
    unsigned red=0; for(unsigned py=0;py<96;++py) for(unsigned px=0;px<128;++px) if(pixel(chrome,128,px,py)>0.3F && pixel(chrome,128,px,py,1)<0.01F) ++red;
    CHECK(red>100); exact(chrome); CHECK(chrome==draw(full.value(),whole));
    s.environment.baked.reflections[0].sphere=true; const auto spherical=draw(full.value(),whole); CHECK(spherical!=dark); exact(spherical);
    s.environment.baked.reflections[0].position.x+=1; static_cast<void>(draw(full.value(),whole)); CHECK(!full.value().stats().assets.ready && full.value().stats().reflection_refusals==1);
}
int main() {
    try {
        model_contract(); std::cout<<"[pass] reflected geometry, authored state and digests\n";
        const auto baked=bake_contract(); CHECK(baked.ready()); std::cout<<"[pass] deterministic capture bytes, local red wall, corrupt and duplicate refusal\n";
        if(!rhi::Device::available(rhi::BackendKind::D3d12)) return 77;
        rhi::DeviceConfig config; config.backend=rhi::BackendKind::D3d12; config.debug=true;
        auto device=rhi::Device::create(config); CHECK(device); gpu_contract(device.value(),baked);
        std::cout<<"[pass] planar box, clipping, chrome probe, box/sphere proxy, canceled upload, held image and exact slices\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
