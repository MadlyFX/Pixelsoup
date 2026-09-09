#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/render/scene_bridge.hpp>
#include <mediaserver/render/baked_lighting.hpp>
#include <mediaserver/render/volumetric_fog.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace render = mediaserver::render;
namespace scene = mediaserver::scene;
namespace rhi = mediaserver::rhi;
namespace doc = mediaserver::doc;
namespace core = mediaserver::core;
#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error(std::to_string(__LINE__) + ": " #__VA_ARGS__); } while (false)
constexpr float pi = 3.14159265358979323846F;
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
    render::RendererLimits l; l.max_instances = 8; l.max_lights = 32; l.max_vertices = 1024; l.max_indices = 4096;
    l.directional_shadow_resolution = 16; l.shadow_atlas = {32, 8, 16, 32}; return l;
}
scene::Scene scene_fixture() {
    scene::Scene s; s.canvas = {128, 64}; s.camera.eye = {0, 0, 4}; s.camera.target = {0, 0, 0};
    s.camera.field_of_view = 0.125F; s.camera.near_plane = 0.1F; s.camera.far_plane = 30;
    s.environment.sky_color = s.environment.ground_color = {}; s.environment.ambient = 0;
    s.renderer_settings.backend = scene::RendererBackend::Advanced;
    s.renderer_settings.gi_strength = 0; s.renderer_settings.shadow_cascades = 0;
    return s;
}

void photometry_and_identity() {
    scene::PointLight l; l.kind = scene::LocalLightKind::Rectangle;
    l.width = 3; l.height = 2; l.intensity = 600; l.unit = scene::LightUnit::Lumens;
    CHECK(scene::supported_light_unit(l));
    const float one = scene::light_emission(l, 1).intensity;
    CHECK(std::abs(one * (6*pi) - 600) < 0.001F);
    l.two_sided = true;
    CHECK(scene::light_emission(l, 1).intensity == one * 0.5F);
    l.unit = scene::LightUnit::Nits;
    CHECK(scene::supported_light_unit(l) && scene::light_emission(l, 0.5F).intensity == 300);
    l.kind = scene::LocalLightKind::Sphere; l.radius = 0.7F; l.unit = scene::LightUnit::Lumens;
    CHECK(std::abs(scene::light_emission(l, 1).intensity * (4*pi*pi*0.49F) - 600) < 0.001F);
    l.unit = scene::LightUnit::Candela; CHECK(!scene::supported_light_unit(l));
    l.direction = {0,0,-1}; l.area_rotation = 0.25F;
    CHECK(scene::length(scene::area_light_tangent(l) - scene::Vec3{0,-1,0}) < 1e-5F);
    auto s = scene_fixture(); l.source = doc::NodeId{10}; s.point_lights.push_back(l);
    const auto scene_id = scene::digest(s), bake_id = render::bake_input_digest(s);
    auto bridge = render::SceneBridge::create(8); CHECK(bridge && bridge.value().update(s));
    const auto visual = bridge.value().records()[0].visual_identity;
    for (unsigned field=0; field<5; ++field) {
        auto changed=s;
        if (field==0) changed.point_lights[0].radius += 0.2F;
        if (field==1) changed.point_lights[0].area_rotation += 0.1F;
        if (field==2) changed.point_lights[0].width += 0.4F;
        if (field==3) changed.point_lights[0].two_sided = false;
        if (field==4) changed.point_lights[0].kind = scene::LocalLightKind::Rectangle;
        CHECK(scene::digest(changed) != scene_id && render::bake_input_digest(changed) != bake_id);
        CHECK(bridge.value().update(changed)); CHECK(bridge.value().records()[0].visual_identity != visual);
    }
    const auto tables=render::ltc_table_data(); CHECK(tables.size()==8192);
    for (unsigned i=0;i<4096;++i) {
        const auto m=tables[i]; CHECK(std::isfinite(m.x) && std::isfinite(m.y) && std::isfinite(m.z) && std::isfinite(m.w));
        CHECK(m.x*m.w-m.y*m.z > 0); CHECK(tables[i+4096].x >= 0 && tables[i+4096].x <= 1.01F);
    }
}

// Independently integrate a Lambert receiver over 256x256 emitting cells.
// This knows neither LTC tables nor the production polygon integration.
double rectangle_reference(unsigned x, unsigned y, const scene::PointLight& l, double roughness = 0) {
    const double scale=8.0*std::tan(3.141592653589793*0.125)/64;
    const double px=(static_cast<double>(x)+0.5-64)*scale;
    const double py=(32-static_cast<double>(y)-0.5)*scale;
    const double view_distance=std::sqrt(px*px+py*py+16);
    const double vx=-px/view_distance, vy=-py/view_distance, vz=4/view_distance;
    double sum=0;
    for (unsigned j=0;j<256;++j) for (unsigned i=0;i<256;++i) {
        const double dx=l.position.x+(static_cast<double>(i)+0.5)/256*l.width-l.width/2-px;
        const double dy=l.position.y+(static_cast<double>(j)+0.5)/256*l.height-l.height/2-py;
        const double z=l.position.z, d2=dx*dx+dy*dy+z*z;
        if (roughness==0) sum+=z*z/(d2*d2*3.141592653589793);
        else {
            const double distance=std::sqrt(d2), lx=dx/distance, ly=dy/distance, lz=z/distance;
            const double hx=vx+lx,hy=vy+ly,hz=vz+lz;
            const double nh=hz/std::sqrt(hx*hx+hy*hy+hz*hz), alpha2=std::pow(roughness,4);
            const double distribution=alpha2/(3.141592653589793*std::pow(nh*nh*(alpha2-1)+1,2));
            const double visibility=0.5/(lz*std::sqrt(vz*vz*(1-alpha2)+alpha2)+vz*std::sqrt(lz*lz*(1-alpha2)+alpha2));
            sum+=distribution*visibility*lz*lz/d2;
        }
    }
    const double fade=1-std::sqrt((l.position.x-px)*(l.position.x-px)+(l.position.y-py)*(l.position.y-py)+l.position.z*l.position.z)/l.range;
    return sum*l.width*l.height/(256*256)*l.intensity*fade*fade;
}
void gpu_contract(rhi::Device& device) {
    auto full=render::SceneRenderer::create(device,128,64,limits(),render::SceneRendererMode::AdvancedLinear); CHECK(full);
    auto left=render::SceneRenderer::create(device,64,64,limits(),render::SceneRendererMode::AdvancedLinear); CHECK(left);
    auto right=render::SceneRenderer::create(device,64,64,limits(),render::SceneRendererMode::AdvancedLinear); CHECK(right);
    auto s=scene_fixture();
    scene::MeshInstance wall; wall.source=doc::NodeId{2}; wall.mesh={scene::Primitive::Quad,1};
    wall.world=scene::scaling({12,6,1}); wall.normal=scene::normal_basis(wall.world);
    wall.material.base_color={1,1,1}; wall.material.specular=0; wall.material.roughness=0.35F; s.instances.push_back(wall);
    scene::PointLight lamp; lamp.source=doc::NodeId{3}; lamp.kind=scene::LocalLightKind::Rectangle;
    lamp.position={0,0,2}; lamp.direction={0,0,-1}; lamp.width=3; lamp.height=0.8F; lamp.intensity=5; lamp.range=100;
    s.point_lights.push_back(lamp);
    const auto draw=[&](render::SceneRenderer& r,scene::CanvasRect slice) {auto cmd=acquire(device); CHECK(r.render(cmd,s,slice));return readback(device,std::move(cmd),r.target(),8);};
    const auto whole=scene::whole(s.canvas);
    const auto slices=[&](const auto& image) {
        const auto a=draw(left.value(),{0,0,64,64}),b=draw(right.value(),{64,0,64,64});
        for (std::size_t y=0;y<64;++y) {
            CHECK(std::equal(a.begin()+y*512,a.begin()+(y+1)*512,image.begin()+y*1024));
            CHECK(std::equal(b.begin()+y*512,b.begin()+(y+1)*512,image.begin()+y*1024+512));
        }
    };
    const auto diffuse=draw(full.value(),whole); CHECK(full.value().stats().ltc_ready && full.value().stats().area_lights==1);
    for (const unsigned x : {32U,64U,83U}) {
        const double ref=rectangle_reference(x,32,lamp), observed=pixel(diffuse,128,x,32);
        CHECK(std::abs(observed-ref)/ref < 0.012);
    }
    slices(diffuse); CHECK(diffuse==draw(full.value(),whole));
    s.instances[0].material.metallic=1; s.instances[0].material.specular=1;
    const auto ggx=draw(full.value(),whole);
    for (const unsigned x : {64U,78U,88U}) {
        const double reference=rectangle_reference(x,32,lamp,0.35);
        CHECK(std::abs(pixel(ggx,128,x,32)/reference-1) < 0.15);
    }
    s.instances[0].material.metallic=1; s.instances[0].material.specular=1; s.instances[0].material.roughness=0.12F;
    const auto horizontal=draw(full.value(),whole);
    CHECK(pixel(horizontal,128,78,32)>pixel(horizontal,128,64,46)*4);
    s.point_lights[0].area_rotation=0.25F;
    const auto vertical=draw(full.value(),whole); CHECK(pixel(vertical,128,64,46)>pixel(vertical,128,78,32)*4);
    slices(vertical);
    s.point_lights[0].area_rotation=0;
    s.instances[0].material.roughness=0.6F;
    const auto rough=draw(full.value(),whole);
    CHECK(pixel(rough,128,64,46)>pixel(horizontal,128,64,46)*2);
    CHECK(pixel(rough,128,64,32)<pixel(horizontal,128,64,32));
    slices(rough); CHECK(rough==draw(full.value(),whole));
    s.point_lights[0].direction={0,0,1};
    const auto back=draw(full.value(),whole); CHECK(pixel(back,128,64,32)==0);
    s.point_lights[0].two_sided=true;
    const auto reversed=draw(full.value(),whole);
    CHECK(std::abs(pixel(reversed,128,64,32)/pixel(rough,128,64,32)-1) < 0.002F);
    slices(reversed); CHECK(reversed==draw(full.value(),whole));
    s.point_lights[0].two_sided=false; s.point_lights[0].kind=scene::LocalLightKind::Sphere; s.point_lights[0].radius=0.4F;
    const auto small=draw(full.value(),whole); CHECK(pixel(small,128,64,32)>0);
    s.point_lights[0].radius=0.8F; const auto large=draw(full.value(),whole);
    CHECK(pixel(large,128,64,32)>pixel(small,128,64,32)*2); slices(large); CHECK(large==draw(full.value(),whole));
    s.point_lights[0].direction={1,0,0}; CHECK(large==draw(full.value(),whole));
    s.point_lights[0].casts_shadow=true; CHECK(draw(full.value(),whole)==large); CHECK(full.value().stats().area_shadow_requests==1 && !full.value().stats().assets.ready);
    // A canceled first recording must retry the immutable table upload.
    auto fresh=render::SceneRenderer::create(device,128,64,limits(),render::SceneRendererMode::AdvancedLinear); CHECK(fresh);
    {auto canceled=acquire(device); CHECK(fresh.value().render(canceled,s,whole));}
    CHECK(draw(fresh.value(),whole)==large);
    render::LtcAsset asset; CHECK(asset.create(device));
    auto prepare=[&](core::u64 generation) {auto cmd=acquire(device); CHECK(asset.prepare(cmd,generation)); auto done=device.submit(std::move(cmd)); CHECK(done && done.value().wait());};
    prepare(17); CHECK(asset.upload_bytes()==131072 && asset.generation()==17);
    prepare(17); CHECK(asset.upload_bytes()==0);
    {auto canceled=acquire(device); CHECK(asset.prepare(canceled,29));}
    prepare(29); CHECK(asset.upload_bytes()==131072 && asset.generation()==29);
    prepare(29); CHECK(asset.upload_bytes()==0);
    // Fog consumes finite emitters, including the rectangle's emission side.
    s.instances.clear(); s.point_lights[0].kind=scene::LocalLightKind::Rectangle;
    s.point_lights[0].casts_shadow=false; s.point_lights[0].direction={0,0,1};
    s.point_lights[0].two_sided=false; s.environment.fog_density=0.15F;
    s.environment.fog_color={1,1,1};
    auto fog=render::VolumetricFog::create(device,128,64,{16,8,16}); CHECK(fog);
    const auto fogged=[&]() {
        auto cmd=acquire(device); CHECK(full.value().render(cmd,s,whole));
        CHECK(fog.value().record(cmd,full.value().target(),full.value().product_depth_target(),full.value().shadow_atlas_target(),
            full.value().light_storage(),full.value().light_shadow_storage(),full.value().light_storage_count(),s,
            scene::view_for_slice(s.camera,s.canvas,whole).view_projection,s.canvas,whole,{},nullptr,&full.value().ies_atlas()));
        return readback(device,std::move(cmd),fog.value().target(),8);
    };
    const auto one_side=fogged(); s.point_lights[0].two_sided=true;
    const auto two_sides=fogged(); CHECK(one_side!=two_sides && two_sides==fogged());
    s.point_lights[0].kind=scene::LocalLightKind::Sphere; CHECK(fogged()!=two_sides);
    std::ofstream image("step32-area-preview.ppm",std::ios::binary); image<<"P6\n128 64\n255\n";
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<128;++x)for(unsigned c=0;c<3;++c){const float value=pixel(rough,128,x,y,c); image.put(static_cast<char>(std::clamp(std::pow(value/(1+value),1/2.2F)*255,0.0F,255.0F)));}
}
int main() {
    try {
        photometry_and_identity(); std::cout<<"[pass] area units, dimensions, orientation, identities and fit tables\n";
        if (!rhi::Device::available(rhi::BackendKind::D3d12)) return 77;
        rhi::DeviceConfig config; config.backend=rhi::BackendKind::D3d12; auto device=rhi::Device::create(config); CHECK(device);
        gpu_contract(device.value()); std::cout<<"[pass] LTC diffuse reference, rough rectangle reflection, roll, sides, sphere, seams and generation retries\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
