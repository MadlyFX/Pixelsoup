#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/render/model_package.hpp>
#include <mediaserver/render/slice_apron.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/output_vert_dxil.h>
#include <mediaserver/shaders/shadow_probe_frag_dxil.h>
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;
#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error(std::to_string(__LINE__) + ": " + #__VA_ARGS__); } while (false)
template<class T> std::span<const core::u8> bytes(const T& v) {
    return {reinterpret_cast<const core::u8*>(&v), sizeof(v)};
}
template<class T> std::span<const core::u8> bytes(const std::vector<T>& v) {
    return {reinterpret_cast<const core::u8*>(v.data()), v.size() * sizeof(T)};
}
rhi::CommandList acquire(rhi::Device& device) {
    auto cmd = device.acquire_commands(); CHECK(cmd); return std::move(cmd).value();
}
std::vector<core::u8> download(rhi::Device& device, rhi::CommandList cmd,
                              const rhi::Texture& target, core::u32 stride) {
    const auto size = static_cast<std::size_t>(target.width()) * target.height() * stride;
    CHECK(size <= std::numeric_limits<core::u32>::max());
    auto rb = device.create_readback(static_cast<core::u32>(size)); CHECK(rb);
    CHECK(cmd.download(target, rb.value()));
    auto submitted = device.submit(std::move(cmd)); CHECK(submitted); CHECK(submitted.value().wait());
    const auto mapped = rb.value().map(); CHECK(mapped.size() == size);
    std::vector<core::u8> result(mapped.begin(), mapped.end()); rb.value().unmap(); return result;
}
rhi::Texture texture(rhi::Device& device, core::u32 w, core::u32 h, rhi::TextureFormat format) {
    rhi::TextureDesc desc; desc.width=w; desc.height=h; desc.format=format;
    desc.sampled=desc.color_target=true;
    auto result=device.create_texture(desc); CHECK(result); return std::move(result).value();
}
void settings_contract() {
    constexpr std::array fields{&scene::RendererSettings::shadow_blend_band,
        &scene::RendererSettings::shadow_filter_radius, &scene::RendererSettings::contact_shadow_length,
        &scene::RendererSettings::contact_shadow_thickness};
    constexpr std::array values{0.19F, 3.5F, 0.6F, 0.09F};
    constexpr std::array maxima{0.25F, 4.0F, 10.0F, 1.0F};
    scene::RendererSettings authored;
    for (std::size_t i=0; i<fields.size(); ++i) {
        authored.*fields[i]=values[i];
        scene::Scene a,b; b.renderer_settings.*fields[i]=values[i];
        CHECK(scene::digest(a)!=scene::digest(b));
        CHECK(scene::renderer_settings_digest(a.renderer_settings)!=scene::renderer_settings_digest(b.renderer_settings));
        b.renderer_settings.*fields[i]=999.0F;
        CHECK(scene::clamped(b.renderer_settings).*fields[i]==maxima[i]);
        b.renderer_settings.*fields[i]=std::numeric_limits<float>::quiet_NaN();
        CHECK(std::isfinite(scene::clamped(b.renderer_settings).*fields[i]));
    }
    doc::Node node; node.id=doc::NodeId{1U}; node.type=std::string{scene::schema::settings_type};
    scene::write_renderer_settings(node,authored);
    scene::SceneModel model; model.reset(doc::Root{}.set(node));
    CHECK(model.renderer_settings()==authored);
    scene::Scene value; value.canvas={256U,128U}; value.renderer_settings=authored;
    value.renderer_settings.backend=scene::RendererBackend::Advanced;
    const auto requirements=render::apron_requirements(value);
    CHECK(requirements.contact_shadow==32U);
    CHECK(requirements.maximum()==32U);
    CHECK(!render::padded_slice(value.canvas,{64U,0U,64U,128U},requirements,31U));
    auto basic=render::padded_slice_for(value,{64U,0U,64U,128U},64U,128U,false);
    CHECK(basic && basic.value().apron==0U);
}

struct ProbeConstants {
    std::array<scene::Mat4,4U> matrices{scene::identity(),scene::identity(),scene::identity(),scene::identity()};
    std::array<float,4U> texel{2.0F/128.0F,2.0F/128.0F,0,0};
    std::array<float,4U> parameters{1.0F/128.0F,1.0F/128.0F,0.1F,0};
    std::array<float,4U> quality{0.1F,2,0,0};
    scene::Mat4 projection=scene::identity(), inverse=scene::identity();
    std::array<float,4U> canvas{128,1,0,0},contact{0.4F,0.06F,32,0},eye{0,0,2,1};
    std::array<core::u32,4U> extent{128,1,128,1},counts{1,0,1,0};
    scene::Mat4 spot=scene::identity();
    std::array<float,4U> tile{0,0,64,0.001F},probe{0,0.4F,0,0};
};
static_assert(sizeof(ProbeConstants)==608U);

void analytic_gpu_filters(rhi::Device& device) {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    constexpr core::u32 size=128U;
    auto near_map=texture(device,size,size,rhi::TextureFormat::R32Float);
    auto far_map=texture(device,size,size,rhi::TextureFormat::R32Float);
    auto depth=texture(device,size,1U,rhi::TextureFormat::R32Float);
    auto atlas=texture(device,size,size,rhi::TextureFormat::R32Float);
    auto target=texture(device,size,1U,rhi::TextureFormat::Rgba32Float);
    rhi::ShaderDesc vertex; vertex.stage=rhi::ShaderStage::Vertex; vertex.format=rhi::ShaderFormat::Dxil;
    vertex.bytecode={mediaserver_output_vert_dxil,sizeof(mediaserver_output_vert_dxil)};
    rhi::ShaderDesc fragment; fragment.stage=rhi::ShaderStage::Fragment; fragment.format=rhi::ShaderFormat::Dxil;
    fragment.bytecode={mediaserver_shadow_probe_frag_dxil,sizeof(mediaserver_shadow_probe_frag_dxil)};
    fragment.sampler_count=4U; fragment.uniform_buffer_count=1U;
    auto pipeline=device.create_pipeline(vertex,fragment,rhi::TextureFormat::Rgba32Float,rhi::ColorBlend::Disabled); CHECK(pipeline);
    auto sampler=device.create_sampler(rhi::Filter::Nearest); CHECK(sampler);
    const std::array<rhi::SampledTexture,4U> textures{{{&near_map,&sampler.value()},
        {&far_map,&sampler.value()},{&depth,&sampler.value()},{&atlas,&sampler.value()}}};
    std::vector<float> near_depth(size*size,0.0F),far_depth(size*size,0.0F),scene_depth(size,0.4F),atlas_depth(size*size,1.0F);
    ProbeConstants constants;
    const auto draw=[&] {
        auto cmd=acquire(device);
        CHECK(cmd.upload(near_map,bytes(near_depth))); CHECK(cmd.upload(far_map,bytes(far_depth)));
        CHECK(cmd.upload(depth,bytes(scene_depth))); CHECK(cmd.upload(atlas,bytes(atlas_depth)));
        CHECK(cmd.draw_output_transform(target,rhi::Color{},pipeline.value(),textures,bytes(constants)));
        const auto raw=download(device,std::move(cmd),target,16U);
        std::vector<float> result(size);
        for (core::u32 x=0;x<size;++x) std::memcpy(&result[x],raw.data()+x*16U,4U);
        return result;
    };
    // No caster, every kernel including the largest sun. A sloped receiver
    // rendered into the map also stays entirely lit with plane correction.
    for (float slope : {0.0F,0.25F,-0.25F}) for (float radius : {0.0F,2.0F,4.0F}) {
        constants.probe[2]=slope; constants.quality[1]=radius; constants.quality[2]=1.0F;
        for (core::u32 y=0;y<size;++y) for (core::u32 x=0;x<size;++x)
            near_depth[y*size+x]=0.4F+slope*((static_cast<float>(x)+0.5F)/64.0F-1.0F);
        const auto pixels=draw();
        CHECK(std::all_of(pixels.begin(),pixels.end(),[](float v){return v==1.0F;}));
    }
    constants.probe[2]=0; constants.quality[1]=0; constants.quality[2]=0;
    for (core::u32 y=0;y<size;++y) for (core::u32 x=0;x<size;++x) near_depth[y*size+x]=x<64U?0.7F:0.0F;
    const auto hard=draw();
    constants.quality[1]=2.0F; const auto wide=draw();
    constants.quality[2]=0.6F; const auto sun=draw();
    const auto partial=[](const auto& pixels){return std::count_if(pixels.begin()+24,pixels.end()-24,[](float v){return v>0.0F&&v<1.0F;});};
    CHECK(partial(wide)>partial(hard)); CHECK(partial(sun)>partial(wide)); CHECK(sun==draw());
    // Blend a fully occluded near map into a fully lit far map. The transition
    // is continuous, stays in its authored edge band and has no discontinuity
    // when a dolly translates that band over a stationary world receiver.
    std::fill(near_depth.begin(),near_depth.end(),0.7F);
    constants.counts[2]=2U; constants.quality={0.2F,2.0F,0.0F,0.0F};
    const auto blended=draw();
    CHECK(blended[0]==1.0F && blended[32]==0.0F && blended[64]==0.0F);
    for (std::size_t x=1;x<32;++x) { CHECK(blended[x]<=blended[x-1]); CHECK(blended[x-1]-blended[x]<0.08F); }
    float previous=1.0F;
    for (int step=0;step<=64;++step) {
        constants.matrices[0].set(0U,3U,1.0F-static_cast<float>(step)*0.008F);
        const auto moved=draw(); CHECK(std::abs(moved[64]-previous)<0.04F); previous=moved[64];
        CHECK(moved[32]==0.0F); // Well away from the moving blend band.
    }
    // An empty tile bordered by fully occluded tiles must never borrow them,
    // even at the last half-texel with the widest kernel.
    constants.probe[0]=1; constants.quality[1]=4;
    for (core::u32 y=0;y<64U;++y) for (core::u32 x=0;x<64U;++x) atlas_depth[y*size+x]=0.0F;
    const auto isolated=draw(); CHECK(std::all_of(isolated.begin(),isolated.end(),[](float v){return v==1.0F;}));
    constants.probe[0]=2; constants.quality[2]=0;
    const auto flat=draw(); CHECK(std::all_of(flat.begin(),flat.end(),[](float v){return v==1.0F;}));
    for (core::u32 x=68U;x<76U;++x) scene_depth[x]=0.53F;
    const auto contact=draw(); CHECK(contact[62]<0.5F); CHECK(contact[90]==1.0F); CHECK(contact==draw());
    std::cout << "[pass] analytic slopes, kernel/sun widths, cascade dolly, atlas isolation and contact rays\n";
#else
    (void)device;
#endif
}

scene::Scene receiver_scene() {
    scene::Scene frame; frame.id=doc::NodeId{1U}; frame.document_version=doc::Version{1U};
    frame.canvas={128U,64U}; frame.camera_source=doc::NodeId{2U};
    frame.camera.eye={0,0,3}; frame.camera.target={0,0,0};
    frame.camera.field_of_view=0.125F; frame.camera.near_plane=0.1F; frame.camera.far_plane=100;
    frame.environment.ambient=0; frame.environment.sky_color=frame.environment.ground_color={0,0,0};
    frame.renderer_settings.backend=scene::RendererBackend::Advanced;
    frame.renderer_settings.gi_strength=0;
    frame.renderer_settings.texture_streaming=false;
    frame.renderer_settings.texture_anisotropy=1U;
    scene::MeshInstance receiver; receiver.source=doc::NodeId{3U}; receiver.mesh={scene::Primitive::Quad,1U};
    receiver.world=scene::scaling({8,4,1}); receiver.normal=scene::normal_basis(receiver.world);
    receiver.material.base_color={0.8F,0.8F,0.8F}; receiver.material.double_sided=true;
    frame.instances.push_back(receiver);
    scene::DirectionalLight sun; sun.direction={-0.9F,0,-1}; sun.intensity=3; sun.angular_radius=0.01F;
    frame.directional_lights.push_back(sun);
    scene::PointLight spot; spot.source=doc::NodeId{7U}; spot.kind=scene::LocalLightKind::Spot;
    spot.position={0.7F,0,2}; spot.direction={-0.4F,0,-1}; spot.intensity=3; spot.range=10;
    spot.outer_cone=0.125F; spot.inner_cone=0.08F; spot.casts_shadow=true;
    frame.point_lights.push_back(spot);
    return frame;
}
render::RendererLimits small_limits() {
    render::RendererLimits limits; limits.max_instances=16U; limits.max_vertices=1024U;
    limits.max_indices=4096U; limits.max_lights=4U; limits.directional_shadow_resolution=128U;
    limits.shadow_atlas={128U,32U,64U,128U}; return limits;
}
void native_cutouts_and_slices(rhi::Device& device) {
    const auto directory=std::filesystem::temp_directory_path()/"mediaserver-shadow-stability";
    std::filesystem::create_directories(directory);
    render::ModelAsset asset;
    asset.mesh.vertices={{{-0.5F,-0.4F,0.5F},{0,0,1},0,0},{{0.5F,-0.4F,0.5F},{0,0,1},1,0},
        {{0.5F,0.4F,0.5F},{0,0,1},1,1},{{-0.5F,0.4F,0.5F},{0,0,1},0,1}};
    asset.mesh.indices={0U,1U,2U,0U,2U,3U}; asset.vertex_metadata.resize(4U);
    for (auto& v:asset.vertex_metadata) v.has_uv0=true;
    asset.bounds={{-0.5F,-0.4F,0.5F},{0.5F,0.4F,0.5F}};
    asset.parts.resize(1U); auto& part=asset.parts.front(); part.index_count=6U; part.has_material=true;
    part.material.double_sided=true; part.material.alpha_mode=scene::MaterialAlphaMode::Mask;
    part.material.alpha_cutoff=0.5F; part.base_color_texture=0U;
    part.samplers[0].min=part.samplers[0].mag=render::ModelFilter::Nearest;
    part.samplers[0].mip=render::ModelMipFilter::None;
    render::ModelTexture map; map.width=map.height=16U; map.rgba.resize(16U*16U*4U,255U);
    for (core::u32 y=0;y<16U;++y) for(core::u32 x=0;x<8U;++x) map.rgba[(y*16U+x)*4U+3U]=0U;
    asset.textures.push_back(map); CHECK(render::prepare_model_mips(asset));
    const auto masked_source=render::write_model_package((directory/"cutout.gltf").string(),asset); CHECK(!masked_source.empty());
    part.material.alpha_mode=scene::MaterialAlphaMode::Opaque;
    const auto solid_source=render::write_model_package((directory/"solid.gltf").string(),asset); CHECK(!solid_source.empty());
    render::ModelCache cache;
    static_cast<void>(cache.observe(masked_source,cache.publication_epoch()));
    static_cast<void>(cache.observe(solid_source,cache.publication_epoch()));
    CHECK(render::detail::wait_for_shared_model_cache_for_test());
    auto frame=receiver_scene();
    scene::ModelInstance model; model.source_node=doc::NodeId{5U}; model.source=solid_source;
    model.world=scene::identity(); model.normal=scene::normal_basis(model.world); frame.models.push_back(model);
    auto created=render::SceneRenderer::create(device,128U,64U,small_limits(),render::SceneRendererMode::AdvancedLinear);
    CHECK(created); auto renderer=std::move(created).value();
    const auto draw=[&](int product) {
        auto cmd=acquire(device); CHECK(renderer.render(cmd,frame,scene::whole(frame.canvas)));
        CHECK(renderer.stats().instances==2U && renderer.stats().models_pending==0U);
        auto& target=product==1?renderer.object_id_target():product==2?renderer.shadow_atlas_target():renderer.target();
        return download(device,std::move(cmd),target,product==0?8U:4U);
    };
    const auto solid=draw(0), solid_ids=draw(1), solid_atlas=draw(2);
    frame.models[0].source=masked_source;
    const auto masked=draw(0), masked_ids=draw(1), masked_atlas=draw(2);
    CHECK(masked!=solid); CHECK(masked_atlas!=solid_atlas); CHECK(masked_ids!=solid_ids);
    std::size_t floor_changes=0,holes=0;
    for (std::size_t i=0;i<128U*64U;++i) {
        const bool same_id=std::equal(solid_ids.begin()+i*4U,solid_ids.begin()+(i+1U)*4U,masked_ids.begin()+i*4U);
        if (!same_id) ++holes;
        if (same_id && !std::equal(solid.begin()+i*8U,solid.begin()+(i+1U)*8U,masked.begin()+i*8U)) ++floor_changes;
    }
    CHECK(holes>100U); CHECK(floor_changes>30U); CHECK(masked==draw(0));
    frame.directional_lights[0].angular_radius=0.04F;
    CHECK(draw(0)!=masked);
    frame.directional_lights[0].angular_radius=0.01F;
    frame.renderer_settings.contact_shadow_length=0.6F; frame.renderer_settings.contact_shadow_thickness=0.1F;
    CHECK(draw(0)!=masked);
    CHECK(renderer.stats().contact_shadow_lights==2U);
    frame.renderer_settings.shadow_blend_band=0.18F; frame.renderer_settings.shadow_filter_radius=3.5F;
    const auto full=draw(0);
    for (const scene::CanvasRect slice : {scene::CanvasRect{0U,0U,63U,64U},scene::CanvasRect{63U,0U,65U,64U}}) {
        const auto padded=render::padded_slice_for(frame,slice,slice.width,slice.height,true); CHECK(padded);
        const auto region=padded.value();
        auto node=render::SceneRenderer::create(device,region.render.width,region.render.height,small_limits(),render::SceneRendererMode::AdvancedLinear); CHECK(node);
        auto cmd=acquire(device); CHECK(node.value().render(cmd,frame,region.render));
        const auto image=download(device,std::move(cmd),node.value().target(),8U);
        for (core::u32 y=0;y<slice.height;++y) for(core::u32 x=0;x<slice.width;++x) {
            const auto local=(static_cast<std::size_t>(y+region.crop.y)*region.render.width+x+region.crop.x)*8U;
            const auto global=(static_cast<std::size_t>(y+slice.y)*128U+x+slice.x)*8U;
            CHECK(std::equal(image.begin()+local,image.begin()+local+8U,full.begin()+global));
        }
        CHECK(node.value().stats().directional_shadow_fit==renderer.stats().directional_shadow_fit);
    }
    // Moving only the camera must preserve the shadow texel scale/depth basis.
    const auto fit=renderer.stats().directional_shadow_fit;
    frame.camera.eye.x+=0.125F; frame.camera.target.x+=0.125F; draw(0);
    const auto moved=renderer.stats().directional_shadow_fit;
    for (core::u32 cascade=0;cascade<2U;++cascade) for(core::u32 row=0;row<3U;++row)
        for(core::u32 col=0;col<3U;++col) CHECK(fit[cascade].at(row,col)==moved[cascade].at(row,col));
    CHECK(fit[1]==moved[1]);
    std::cout << "[pass] native alpha casters/depth, odd two-node seam with every shadow option, anchored cascade scales\n";
}

int main() {
    try {
        settings_contract();
        if (!render::SceneRenderer::shaders_available() || !rhi::Device::available(rhi::BackendKind::D3d12)) {
            std::cout << "[skip] native D3D12 shadow images unavailable; CPU settings/apron contracts passed\n";
            return 0;
        }
        rhi::DeviceConfig config; config.backend=rhi::BackendKind::D3d12;
        auto opened=rhi::Device::create(config); CHECK(opened);
        analytic_gpu_filters(opened.value());
        native_cutouts_and_slices(opened.value());
    } catch (const std::exception& error) { std::cerr << "[fail] " << error.what() << '\n'; return 1; }
}
