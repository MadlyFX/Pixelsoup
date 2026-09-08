#include <mediaserver/render/material_sampling.hpp>
#include <mediaserver/render/baked_lighting.hpp>
#include <mediaserver/render/model_package.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/render/temporal_history.hpp>
#include <mediaserver/render/texture_streaming.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/scene/math.hpp>
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/output_vert_dxil.h>
#include <mediaserver/shaders/texture_probe_frag_dxil.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;
namespace schema = mediaserver::scene::schema;
void check(bool value, const char* expression, int line) {
    if (!value) throw std::runtime_error{std::to_string(line) + ": " + expression};
}
#define CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

void settings() {
    scene::RendererSettings authored;
    authored.texture_anisotropy = 8U;
    authored.texture_lod_bias = -1.25F;
    doc::Node node; node.id = doc::NodeId{1U}; node.type = std::string{schema::settings_type};
    scene::write_renderer_settings(node, authored);
    doc::Root root; root = root.set(node);
    scene::SceneModel model; model.reset(root);
    CHECK(model.renderer_settings() == authored);
    for (int field = 0; field < 2; ++field) {
        scene::Scene before, after;
        if (field == 0) after.renderer_settings.texture_anisotropy = 8U;
        else after.renderer_settings.texture_lod_bias = -1.25F;
        CHECK(scene::digest(before) != scene::digest(after));
        CHECK(scene::renderer_settings_digest(before.renderer_settings) != scene::renderer_settings_digest(after.renderer_settings));
        CHECK(render::renderer_profile_digest(before.renderer_settings) != render::renderer_profile_digest(after.renderer_settings));
    }
    authored.texture_anisotropy = 999U; authored.texture_lod_bias = -999.0F;
    CHECK(scene::clamped(authored).texture_anisotropy == 16U);
    CHECK(scene::clamped(authored).texture_lod_bias == -4.0F);
    authored.texture_anisotropy = 3U; authored.texture_lod_bias = std::numeric_limits<float>::quiet_NaN();
    CHECK(scene::clamped(authored).texture_anisotropy == 2U);
    CHECK(scene::clamped(authored).texture_lod_bias == 0.0F);
}

std::filesystem::path fixture_directory() {
    const auto dir = std::filesystem::temp_directory_path() / "mediaserver-material-sampling";
    std::filesystem::create_directories(dir); return dir;
}

void gltf_views_and_package() {
    const auto dir = fixture_directory();
    {
        const std::array<float, 15U> vertices{-1,-1,0, 1,-1,0, 0,1,0, 0,0, 4,0, 2,4};
        std::ofstream file{dir / "mesh.bin", std::ios::binary};
        file.write(reinterpret_cast<const char*>(vertices.data()), sizeof(vertices)); CHECK(file);
    }
    {
        std::array<core::u8, 22U> tga{};
        tga[2]=2U; tga[12]=tga[14]=1U; tga[16]=32U; tga[17]=0x28U;
        tga[18]=10U; tga[19]=80U; tga[20]=200U; tga[21]=255U;
        std::ofstream file{dir / "map.tga", std::ios::binary};
        file.write(reinterpret_cast<const char*>(tga.data()), tga.size()); CHECK(file);
    }
    const std::string prefix = R"({"asset":{"version":"2.0"},"buffers":[{"uri":"mesh.bin","byteLength":60}],
"bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},
{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"}],"images":[{"uri":"map.tga"}],)";
    const std::string suffix = R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}},"emissiveTexture":{"index":1},"emissiveFactor":[1,1,1],
"extensions":{"KHR_materials_clearcoat":{"clearcoatTexture":{"index":1}}}}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"material":0}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    const auto source = (dir / "views.gltf").string();
    const auto write = [&](std::string samplers) {
        std::ofstream file{source}; file << prefix << samplers << suffix; CHECK(file);
    };
    write(R"("samplers":[{"wrapS":33648,"wrapT":33071,"minFilter":9984,"magFilter":9728},{"wrapS":10497,"wrapT":33648,"minFilter":9729,"magFilter":9729}],
"textures":[{"source":0,"sampler":0},{"source":0,"sampler":1}],)" );
    render::ModelAsset asset;
    CHECK(render::load_model(source, asset)); CHECK(asset.parts.size() == 1U);
    const auto& part = asset.parts[0];
    CHECK(part.base_color_texture == part.emissive_texture); // Same bytes, distinct views.
    CHECK(part.samplers[0].u == render::ModelAddressMode::Mirror);
    CHECK(part.samplers[0].v == render::ModelAddressMode::Clamp);
    CHECK(part.samplers[0].min == render::ModelFilter::Nearest);
    CHECK(part.samplers[0].mag == render::ModelFilter::Nearest);
    CHECK(part.samplers[0].mip == render::ModelMipFilter::Nearest);
    CHECK(part.samplers[4].u == render::ModelAddressMode::Repeat);
    CHECK(part.samplers[4].v == render::ModelAddressMode::Mirror);
    CHECK(part.samplers[4].mip == render::ModelMipFilter::None);
    CHECK(part.samplers[5] == part.samplers[4]);
    CHECK(render::prepare_model_mips(asset));
    CHECK(asset.textures[*asset.parts[0].emissive_texture].requires_base_level);
    const auto packaged = render::write_model_package(source, asset); CHECK(!packaged.empty());
    render::ModelAsset restored; CHECK(render::load_model(packaged, restored));
    CHECK(restored.parts == asset.parts); CHECK(restored.textures == asset.textures);
    auto changed = asset; changed.parts[0].samplers[5].u = render::ModelAddressMode::Clamp;
    CHECK(render::write_model_package(source, changed) != packaged);
    changed.parts[0].samplers[5].u = static_cast<render::ModelAddressMode>(255U);
    bool refused=false;
    try { static_cast<void>(render::write_model_package(source, changed)); }
    catch (const std::runtime_error&) { refused=true; }
    CHECK(refused);
    write(R"("textures":[{"source":0},{"source":0}],)" );
    CHECK(render::load_model(source, restored));
    CHECK(restored.parts[0].samplers[0] == render::ModelSampler{});
    write(R"("samplers":[{"wrapS":12345}],"textures":[{"source":0,"sampler":0},{"source":0}],)" );
    CHECK(!render::load_model(source, restored));
}

render::ModelAsset striped_asset(core::u32 size) {
    render::ModelAsset asset;
    render::ModelTexture texture; texture.width = texture.height = size;
    texture.rgba.resize(static_cast<std::size_t>(size) * size * 4U);
    for (core::u32 y=0U; y<size; ++y) for (core::u32 x=0U; x<size; ++x) {
        const auto offset = (static_cast<std::size_t>(y)*size+x)*4U;
        const core::u8 value = (x/16U)%2U ? 255U : 0U;
        texture.rgba[offset] = texture.rgba[offset+1] = texture.rgba[offset+2] = value;
        texture.rgba[offset+3]=255U;
    }
    asset.textures.push_back(std::move(texture)); CHECK(render::prepare_model_mips(asset)); return asset;
}

void base_level_residency() {
    auto asset = std::make_shared<render::ModelAsset>(striped_asset(64U));
    asset->parts.resize(1U); asset->parts[0].base_color_texture=0U;
    asset->parts[0].samplers[0].mip=render::ModelMipFilter::None;
    CHECK(render::prepare_model_mips(*asset));
    std::array<render::TextureRequest, 1U> requests{{{"base",asset,1U,0U,1.0F,1U}}};
    std::vector<render::TextureSelection> plan; plan.reserve(1U);
    CHECK(render::plan_texture_residency(requests, 65536U, true, 12U, plan));
    CHECK(plan[0].first_mip==0U); CHECK(plan[0].tail_mip==0U);
    CHECK(!render::plan_texture_residency(requests, 100U, true, 12U, plan));
}

void baked_emission_uses_the_authored_sampler() {
    // Constant UVs on a large emitter isolate addressing from ray position.
    // At 1.375, repeat sees red, mirror sees green, and clamp sees blue.
    render::ModelAsset asset;
    asset.mesh.vertices={{{-100,-100,0},{0,0,1},1.375F,0.5F},{{100,-100,0},{0,0,1},1.375F,0.5F},
        {{100,100,0},{0,0,1},1.375F,0.5F},{{-100,100,0},{0,0,1},1.375F,0.5F}};
    asset.mesh.indices={0U,1U,2U,0U,2U,3U};
    asset.vertex_metadata.resize(4U);
    for (auto& vertex : asset.vertex_metadata) vertex.has_uv0=true;
    asset.bounds={{-100,-100,0},{100,100,0}};
    asset.parts.resize(1U); auto& part=asset.parts[0];
    part.first_index=0U; part.index_count=6U; part.has_material=true;
    part.material.base_color={0,0,0}; part.material.emissive={1,1,1};
    part.material.emissive_strength=1.0F; part.emissive_texture=0U;
    part.samplers[4].mag=render::ModelFilter::Nearest;
    render::ModelTexture texture; texture.width=4U; texture.height=1U;
    texture.rgba={0,0,0,255, 255,0,0,255, 0,255,0,255, 0,0,255,255};
    asset.textures.push_back(std::move(texture)); CHECK(render::prepare_model_mips(asset));
    scene::Scene frame; frame.environment.ambient=0.0F;
    frame.environment.baked.minimum={-0.1F,-0.1F,0.9F};
    frame.environment.baked.maximum={0.1F,0.1F,1.1F};
    frame.environment.baked.count_x=frame.environment.baked.count_y=frame.environment.baked.count_z=2U;
    frame.environment.baked.samples=128U; frame.environment.baked.bounces=0U;
    scene::ModelInstance instance; instance.world=scene::identity(); instance.normal=scene::normal_basis(instance.world);
    frame.models.push_back(instance);
    for (const auto mode : {render::ModelAddressMode::Repeat,render::ModelAddressMode::Mirror,render::ModelAddressMode::Clamp}) {
        part.samplers[4].u=mode;
        frame.models[0].source=render::write_model_package((fixture_directory()/"emitter.gltf").string(),asset);
        const auto bake=render::bake_lighting(frame); CHECK(bake.ready());
        const auto irradiance=render::sample_probe_irradiance(*bake.asset,{0,0,1},{0,0,-1});
        const std::array<float,3U> channels{irradiance.x,irradiance.y,irradiance.z};
        const std::size_t expected=mode==render::ModelAddressMode::Repeat?0U:mode==render::ModelAddressMode::Mirror?1U:2U;
        CHECK(channels[expected]>1.0F);
        CHECK(channels[(expected+1U)%3U]<0.001F); CHECK(channels[(expected+2U)%3U]<0.001F);
        const auto held=render::bake_lighting(frame); CHECK(held.ready());
        CHECK(render::encode_probe_asset(*bake.asset)==render::encode_probe_asset(*held.asset));
    }
}

rhi::CommandList acquire(rhi::Device& device) {
    auto result=device.acquire_commands(); CHECK(result); return std::move(result).value();
}
void submit(rhi::Device& device, rhi::CommandList commands) {
    auto result=device.submit(std::move(commands)); CHECK(result); CHECK(result.value().wait());
}
std::vector<core::u8> download(rhi::Device& device, rhi::CommandList commands, const rhi::Texture& target, core::u32 stride=4U) {
    auto readback=device.create_readback(target.width()*target.height()*stride); CHECK(readback);
    CHECK(commands.download(target,readback.value())); submit(device,std::move(commands));
    const auto bytes=readback.value().map(); std::vector<core::u8> result(bytes.begin(),bytes.end());
    readback.value().unmap(); return result;
}

void gpu_sampling(rhi::Device& device) {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    rhi::ShaderDesc vertex; vertex.stage=rhi::ShaderStage::Vertex; vertex.format=rhi::ShaderFormat::Dxil;
    vertex.bytecode=mediaserver_output_vert_dxil;
    rhi::ShaderDesc fragment; fragment.stage=rhi::ShaderStage::Fragment; fragment.format=rhi::ShaderFormat::Dxil;
    fragment.bytecode=mediaserver_texture_probe_frag_dxil; fragment.sampler_count=1U; fragment.uniform_buffer_count=1U;
    auto pipeline=device.create_pipeline(vertex,fragment,rhi::TextureFormat::Rgba8Unorm,rhi::ColorBlend::Disabled); CHECK(pipeline);
    rhi::TextureDesc desc; desc.width=desc.height=16U; desc.color_target=true; desc.sampled=true;
    auto output=device.create_texture(desc); CHECK(output);
    desc.color_target=false; desc.width=desc.height=4U;
    auto source=device.create_texture(desc); CHECK(source);
    std::array<core::u8,64U> grid{};
    for (core::u32 y=0U;y<4U;++y) for (core::u32 x=0U;x<4U;++x) {
        const auto offset=(y*4U+x)*4U; grid[offset]=static_cast<core::u8>(x*80U);
        grid[offset+1U]=static_cast<core::u8>(y*80U); grid[offset+3U]=255U;
    }
    auto commands=acquire(device); CHECK(commands.upload(source.value(),grid)); submit(device,std::move(commands));
    const auto draw=[&](const rhi::Texture& texture,const rhi::Sampler& sampler,std::array<float,4U> parameters) {
        auto list=acquire(device);
        CHECK(list.draw_output_transform(output.value(),{},pipeline.value(),texture,sampler,
            {reinterpret_cast<const core::u8*>(parameters.data()),sizeof(parameters)}));
        return download(device,std::move(list),output.value());
    };
    std::array<rhi::Sampler,3U> addressing;
    for (std::size_t mode=0U;mode<3U;++mode) {
        rhi::SamplerDesc sampler; sampler.min_filter=sampler.mag_filter=sampler.mip_filter=rhi::Filter::Nearest;
        sampler.address_u=static_cast<rhi::AddressMode>(mode); sampler.address_v=rhi::AddressMode::Repeat;
        auto result=device.create_sampler(sampler); CHECK(result); addressing[mode]=std::move(result).value();
        const auto pixels=draw(source.value(),addressing[mode],{0,4,4,0});
        for (core::u32 y=0U;y<16U;++y) for (core::u32 x=0U;x<16U;++x) {
            const auto offset=(y*16U+x)*4U;
            const auto expected=mode==0U ? (x<4U ? 0U : x>=8U ? 240U : (x-4U)*80U)
                : mode==1U ? (x%4U)*80U : ((x/4U)%2U==0U ? 3U-x%4U : x%4U)*80U;
            CHECK(pixels[offset]==expected); CHECK(pixels[offset+1U]==(y%4U)*80U);
        }
        CHECK(draw(source.value(),addressing[mode],{0,4,4,0})==pixels);
    }
    // Thousands of alternating batches reuse three complete tables in a recording.
    if (device.capabilities().backend==rhi::BackendKind::D3d12) {
        const std::array<float,4U> parameters{};
        for (bool cancel : {true,false}) {
            auto list=acquire(device);
            for (std::size_t i=0U;i<2500U;++i)
                CHECK(list.draw_output_transform(output.value(),{},pipeline.value(),source.value(),addressing[i%3U],
                    {reinterpret_cast<const core::u8*>(parameters.data()),sizeof(parameters)}));
            if (!cancel) submit(device,std::move(list));
        }
        std::vector<rhi::Sampler> shared; shared.reserve(2500U);
        for (unsigned i=0U;i<2500U;++i) {
            auto sampler=device.create_sampler(rhi::SamplerDesc{}); CHECK(sampler); shared.push_back(std::move(sampler).value());
        }
    }
    auto stripes=striped_asset(256U);
    desc.width=desc.height=256U; desc.mip_levels=9U;
    auto mip_texture=device.create_texture(desc); CHECK(mip_texture);
    commands=acquire(device); CHECK(commands.upload_texture_rows(mip_texture.value(),0U,0U,0U,256U,stripes.textures[0].rgba));
    for (core::u32 i=0U;i<8U;++i) {
        const auto& mip=stripes.textures[0].lower_mips[i];
        CHECK(commands.upload_texture_rows(mip_texture.value(),i+1U,0U,0U,mip.height,mip.rgba));
    }
    submit(device,std::move(commands));
    rhi::SamplerDesc iso; iso.address_u=iso.address_v=rhi::AddressMode::Repeat;
    auto isotropic=device.create_sampler(iso); CHECK(isotropic);
    iso.max_anisotropy=16U; auto anisotropic=device.create_sampler(iso); CHECK(anisotropic);
    const auto soft=draw(mip_texture.value(),isotropic.value(),{0,1,8,1});
    const auto sharp=draw(mip_texture.value(),anisotropic.value(),{0,1,8,1});
    const auto contrast=[](const auto& pixels) {
        int lo=255,hi=0; for (std::size_t i=0U;i<pixels.size();i+=4U) {
            lo=std::min(lo,static_cast<int>(pixels[i])); hi=std::max(hi,static_cast<int>(pixels[i]));
        } return hi-lo;
    };
    CHECK(contrast(soft)<10); CHECK(contrast(sharp)>100);
    CHECK(sharp==draw(mip_texture.value(),anisotropic.value(),{0,1,8,1}));
    // Distinct constant mips expose descriptor LOD bias without relying on a sharpness metric.
    commands=acquire(device);
    for (core::u32 level=0U;level<9U;++level) {
        const auto extent=256U>>level;
        std::vector<core::u8> pixels(static_cast<std::size_t>(extent)*extent*4U,static_cast<core::u8>(20U+level*24U));
        CHECK(commands.upload_texture_rows(mip_texture.value(),level,0U,0U,extent,pixels));
    }
    submit(device,std::move(commands));
    rhi::SamplerDesc bias; bias.lod_bias=2.0F;
    auto biased=device.create_sampler(bias); CHECK(biased);
    const auto ordinary=draw(mip_texture.value(),isotropic.value(),{0,0,0,1});
    const auto shifted=draw(mip_texture.value(),biased.value(),{0,0,0,1});
    CHECK(std::abs(static_cast<int>(shifted[0])-ordinary[0]-48)<=1);
    CHECK(shifted==draw(mip_texture.value(),biased.value(),{0,0,0,1}));
    bias.max_anisotropy=17U; CHECK(!device.create_sampler(bias));
    bias={}; bias.lod_bias=std::numeric_limits<float>::quiet_NaN(); CHECK(!device.create_sampler(bias));
    bias={}; bias.min_lod=2; bias.max_lod=1; CHECK(!device.create_sampler(bias));
    bias={}; bias.comparison=rhi::SamplerCompare::GreaterEqual; CHECK(device.create_sampler(bias));
#else
    static_cast<void>(device);
#endif
}

void tiled_materials_reach_opaque_and_composite_slices(rhi::Device& device) {
    render::ModelAsset asset;
    asset.mesh.vertices={{{-2,-1,0},{0,0,1},0,0},{{2,-1,0},{0,0,1},4,0},
        {{2,1,0},{0,0,1},4,1},{{-2,1,0},{0,0,1},0,1}};
    asset.mesh.indices={0U,1U,2U,0U,2U,3U};
    asset.vertex_metadata.resize(4U);
    for (auto& vertex : asset.vertex_metadata) vertex.has_uv0=true;
    asset.bounds={{-2,-1,0},{2,1,0}};
    asset.parts.resize(1U); auto& part=asset.parts[0];
    part.first_index=0U; part.index_count=6U; part.has_material=true;
    part.material.base_color={0,0,0}; part.material.specular=0.0F;
    part.material.emissive={1,1,1}; part.material.emissive_strength=1.0F;
    part.emissive_texture=0U;
    // The base-colour sampler deliberately differs from the emissive sampler.
    part.samplers[0].u=render::ModelAddressMode::Clamp;
    render::ModelTexture texture; texture.width=texture.height=64U; texture.rgba.resize(64U*64U*4U);
    for (core::u32 y=0U;y<64U;++y) for (core::u32 x=0U;x<64U;++x) {
        const auto at=(y*64U+x)*4U;
        texture.rgba[at]=x<32U?255U:0U; texture.rgba[at+1U]=x<32U?0U:255U; texture.rgba[at+3U]=255U;
    }
    asset.textures.push_back(std::move(texture)); CHECK(render::prepare_model_mips(asset));
    const auto source=render::write_model_package((fixture_directory()/"tiled.gltf").string(),asset); CHECK(!source.empty());
    render::ModelCache cache; static_cast<void>(cache.observe(source,cache.publication_epoch()));
    CHECK(render::detail::wait_for_shared_model_cache_for_test());
    scene::Scene frame; frame.canvas={128U,64U};
    frame.camera.eye={0,0,1}; frame.camera.target={0,0,0}; frame.camera.field_of_view=scene::Turns{0.25F};
    frame.environment.ambient=0.0F; frame.environment.sky_color={0,0,0}; frame.environment.ground_color={0,0,0};
    frame.renderer_settings.texture_streaming=false; frame.renderer_settings.texture_anisotropy=8U;
    scene::ModelInstance instance; instance.source=source; instance.source_node=doc::NodeId{41U};
    instance.world=scene::identity(); instance.normal=scene::normal_basis(instance.world);
    frame.models.push_back(instance);
    render::RendererLimits limits;
    limits.max_instances=8U; limits.max_vertices=128U; limits.max_indices=128U;
    limits.max_model_textures=8U; limits.max_model_texture_bytes=1024U*1024U;
    limits.directional_shadow_resolution=16U; limits.shadow_atlas.extent=64U;
    for (const auto mode : {render::SceneRendererMode::BasicDisplay,render::SceneRendererMode::AdvancedLinear}) {
        if (mode==render::SceneRendererMode::AdvancedLinear && !device.capabilities().advanced_raster()) continue;
        frame.renderer_settings.backend=mode==render::SceneRendererMode::AdvancedLinear
            ?scene::RendererBackend::Advanced:scene::RendererBackend::Basic;
        auto full=render::SceneRenderer::create(device,128U,64U,limits,mode); CHECK(full);
        auto left=render::SceneRenderer::create(device,64U,64U,limits,mode); CHECK(left);
        auto right=render::SceneRenderer::create(device,64U,64U,limits,mode); CHECK(right);
        const core::u32 stride=mode==render::SceneRendererMode::AdvancedLinear?8U:4U;
        const auto draw=[&](render::SceneRenderer& renderer,scene::CanvasRect slice) {
            auto commands=acquire(device); CHECK(renderer.render(commands,frame,slice));
            return download(device,std::move(commands),renderer.target(),stride);
        };
        for (float opacity : {1.0F,0.5F}) {
            frame.models[0].style.opacity=opacity;
            frame.renderer_settings.texture_lod_bias=0.0F;
            const auto image=draw(full.value(),{0U,0U,128U,64U});
            const auto a=draw(left.value(),{0U,0U,64U,64U});
            const auto b=draw(right.value(),{64U,0U,64U,64U});
            CHECK(image==draw(full.value(),{0U,0U,128U,64U}));
            for (core::u32 y=0U;y<64U;++y) {
                const auto start=static_cast<std::size_t>(y)*128U*stride;
                CHECK(std::equal(a.begin()+static_cast<std::size_t>(y)*64U*stride,a.begin()+static_cast<std::size_t>(y+1U)*64U*stride,image.begin()+start));
                CHECK(std::equal(b.begin()+static_cast<std::size_t>(y)*64U*stride,b.begin()+static_cast<std::size_t>(y+1U)*64U*stride,image.begin()+start+64U*stride));
            }
            const auto row=32U*128U*stride;
            for (core::u32 x=4U;x<28U;++x) for (core::u32 tile=1U;tile<4U;++tile)
                CHECK(std::equal(image.begin()+row+x*stride,image.begin()+row+(x+1U)*stride,image.begin()+row+(x+tile*32U)*stride));
            CHECK(!std::equal(image.begin()+row+8U*stride,image.begin()+row+9U*stride,image.begin()+row+24U*stride));
            frame.renderer_settings.texture_lod_bias=4.0F;
            const auto softer=draw(full.value(),{0U,0U,128U,64U}); CHECK(softer!=image);
            CHECK(softer==draw(full.value(),{0U,0U,128U,64U}));
            const auto biased_left=draw(left.value(),{0U,0U,64U,64U});
            for (core::u32 y=0U;y<64U;++y)
                CHECK(std::equal(biased_left.begin()+static_cast<std::size_t>(y)*64U*stride,biased_left.begin()+static_cast<std::size_t>(y+1U)*64U*stride,softer.begin()+static_cast<std::size_t>(y)*128U*stride));
        }
        device.wait_idle();
    }
}

int main() {
    try {
        settings(); gltf_views_and_package(); base_level_residency(); baked_emission_uses_the_authored_sampler();
        std::cout << "[pass] settings, per-view glTF samplers, packages and no-mip residency\n";
        for (const auto backend : {rhi::BackendKind::SdlGpu,rhi::BackendKind::D3d12}) {
            if (!rhi::Device::available(backend)) { std::cout << "[skip] unavailable GPU backend\n"; continue; }
            rhi::DeviceConfig config; config.backend=backend;
            auto device=rhi::Device::create(config); CHECK(device);
            gpu_sampling(device.value());
            tiled_materials_reach_opaque_and_composite_slices(device.value());
            std::cout << "[pass] GPU address modes, filtering, bias, hold and descriptor reuse: " << static_cast<int>(backend) << '\n';
        }
    } catch (const std::exception& e) { std::cerr << "[fail] " << e.what() << '\n'; return 1; }
}
