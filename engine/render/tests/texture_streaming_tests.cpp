#include <mediaserver/render/model_cache.hpp>
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/output_vert_dxil.h>
#include <mediaserver/shaders/texture_probe_frag_dxil.h>
#endif
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/render/texture_streaming.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
namespace core = mediaserver::core;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;
void check(bool value, const char *what, int line) {
    if (!value)
        throw std::runtime_error(std::to_string(line) + ": " + what);
}
#define CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

std::shared_ptr<render::ModelAsset> asset(core::u32 size, core::u8 value = 96U) {
    auto result = std::make_shared<render::ModelAsset>();
    render::ModelTexture texture;
    texture.width = texture.height = size;
    texture.rgba.resize(static_cast<std::size_t>(size) * size * 4U, value);
    for (std::size_t i = 3U; i < texture.rgba.size(); i += 4U)
        texture.rgba[i] = 255U;
    result->textures.push_back(std::move(texture));
    CHECK(render::prepare_model_mips(*result));
    return result;
}

void filtering_preserves_material_roles_and_odd_edges() {
    render::ModelAsset value;
    value.textures.push_back({2U, 1U, {0U, 0U, 0U, 255U, 255U, 255U, 255U, 255U}, {}});
    render::ModelPart part;
    part.base_color_texture = 0U;
    part.occlusion_texture = 0U;
    value.parts.push_back(part);
    CHECK(render::prepare_model_mips(value));
    CHECK(value.textures.size() == 2U);
    CHECK(value.textures[*value.parts[0].base_color_texture].lower_mips[0].rgba[0] ==
          188U);
    CHECK(value.textures[*value.parts[0].occlusion_texture].lower_mips[0].rgba[0] ==
          128U);
    // The final column participates in a 3x1 footprint.
    render::ModelAsset odd;
    odd.textures.push_back(
        {3U, 1U, {0U, 0U, 0U, 255U, 0U, 0U, 0U, 255U, 255U, 255U, 255U, 255U}, {}});
    CHECK(render::prepare_model_mips(odd));
    CHECK(odd.textures[0].lower_mips[0].rgba[0] == 85U);
    // Hidden RGB cannot bleed a black fringe into an alpha-bearing colour map.
    value.textures.clear();
    value.parts.resize(1U);
    value.parts[0].occlusion_texture.reset();
    value.textures.push_back({2U, 1U, {0U, 0U, 0U, 0U, 255U, 0U, 0U, 255U}, {}});
    CHECK(render::prepare_model_mips(value));
    CHECK(value.textures[0].lower_mips[0].rgba[0] == 255U);
    CHECK(value.textures[0].lower_mips[0].rgba[3] == 128U);
}

void desired_levels_and_budget_are_deterministic() {
    CHECK(render::desired_texture_mip(4096U, 2048U, 512.0F, 0U) == 3U);
    CHECK(render::desired_texture_mip(4096U, 2048U, 513.0F, 0U) == 2U);
    CHECK(render::desired_texture_mip(
              4096U, 2048U, std::numeric_limits<float>::infinity(), 9U) == 0U);
    scene::RendererSettings authored;
    const auto digest = scene::renderer_settings_digest(authored);
    authored.texture_streaming = false;
    CHECK(scene::renderer_settings_digest(authored) != digest);
    authored = {};
    authored.texture_budget_mib = 37U;
    CHECK(scene::renderer_settings_digest(authored) != digest);
    authored = {};
    authored.texture_mip_bias = 3U;
    CHECK(scene::renderer_settings_digest(authored) != digest);
    scene::Scene baseline, changed;
    for (int i = 0; i < 3; ++i) {
        changed = baseline;
        if (i == 0)
            changed.renderer_settings.texture_streaming = false;
        if (i == 1)
            changed.renderer_settings.texture_budget_mib = 37U;
        if (i == 2)
            changed.renderer_settings.texture_mip_bias = 3U;
        CHECK(scene::digest(changed) != scene::digest(baseline));
    }
    authored.texture_budget_mib = 0U;
    authored.texture_mip_bias = 999U;
    CHECK(scene::clamped(authored).texture_budget_mib == 1U);
    // The authored ceiling and the renderer's envelope are one number, so a
    // budget the editor accepts is never silently cut by the renderer.
    authored.texture_budget_mib = 4096U;
    CHECK(scene::clamped(authored).texture_budget_mib == 1024U);
    CHECK(render::RendererLimits{}.max_model_texture_bytes == 1024ULL * 1024ULL * 1024ULL);
    CHECK(scene::clamped(authored).texture_mip_bias == 12U);
    auto decoded = asset(16U);
    std::array<render::TextureRequest, 3U> requests{
        {{"b", decoded, 1U, 0U, 16.0F, 100U},
         {"a", decoded, 1U, 0U, 1.0F, 99U},
         {"a", decoded, 1U, 0U, 16.0F, 99U}}};
    std::vector<render::TextureSelection> plan;
    plan.reserve(3U);
    CHECK(render::plan_texture_residency(requests, 1448U, true, 0U, plan));
    CHECK(plan.size() == 2U);
    CHECK(plan[0].request.source == "a" && plan[0].first_mip == 2U &&
          plan[0].bytes == 84U);
    CHECK(plan[1].request.source == "b" && plan[1].first_mip == 0U &&
          plan[1].bytes == 1364U);
    std::reverse(requests.begin(), requests.end());
    CHECK(render::plan_texture_residency(requests, 1448U, true, 0U, plan));
    CHECK(plan[0].first_mip == 2U && plan[1].first_mip == 0U);
    CHECK(!render::plan_texture_residency(requests, 167U, true, 0U, plan));
    CHECK(!render::plan_texture_residency(requests, 1448U, false, 0U, plan));
    CHECK(render::plan_texture_residency(requests, 168U, true, 0U, plan));
    CHECK(plan[0].first_mip == 2U && plan[1].first_mip == 2U);
    // Equal recency always evicts a before b, even above the float-exact frame range.
    for (auto &request : requests)
        request.last_desired = (1ULL << 53U) + 7U;
    CHECK(render::plan_texture_residency(requests, 1448U, true, 0U, plan));
    CHECK(plan[0].first_mip == 2U && plan[1].first_mip == 0U);
}

rhi::CommandList acquire(rhi::Device &device) {
    auto result = device.acquire_commands();
    CHECK(result);
    return std::move(result).value();
}
void submit(rhi::Device &device, rhi::CommandList commands) {
    auto result = device.submit(std::move(commands));
    CHECK(result);
    CHECK(result.value().wait());
}
void check_pixels(rhi::Device &device, rhi::CommandList &commands,
                  const rhi::Texture &texture, core::u8 expected) {
    auto readback = device.create_readback(texture.width() * texture.height() * 4U);
    CHECK(readback);
    CHECK(commands.download(texture, readback.value()));
    submit(device, std::move(commands));
    const auto bytes = readback.value().map();
    for (std::size_t i = 0U; i < bytes.size(); ++i)
        CHECK(bytes[i] == (i % 4U == 3U ? 255U : expected));
    readback.value().unmap();
}

void row_uploads_preserve_other_rows(rhi::Device &device) {
    rhi::TextureDesc desc;
    desc.width = 4U;
    desc.height = 4U;
    desc.sampled = true;
    desc.mip_levels = 3U;
    auto texture = device.create_texture(desc);
    CHECK(texture);
    auto commands = acquire(device);
    std::array<core::u8, 32U> first{};
    first.fill(41U);
    std::array<core::u8, 32U> second{};
    second.fill(177U);
    CHECK(!commands.upload_texture_rows(texture.value(), 0U, 0U, 3U, 2U, first));
    CHECK(!commands.upload_texture_rows(texture.value(), 3U, 0U, 0U, 1U, first));
    CHECK(!commands.upload_texture_rows(texture.value(), 0U, 1U, 0U, 2U, first));
    CHECK(commands.upload_texture_rows(texture.value(), 0U, 0U, 0U, 2U, first));
    CHECK(commands.upload_texture_rows(texture.value(), 0U, 0U, 2U, 2U, second));
    const std::array<core::u8, 16U> lower{};
    CHECK(commands.upload_texture_rows(texture.value(), 1U, 0U, 0U, 2U, lower));
    const std::array<core::u8, 4U> last{};
    CHECK(commands.upload_texture_rows(texture.value(), 2U, 0U, 0U, 1U, last));
    auto readback = device.create_readback(64U);
    CHECK(readback);
    CHECK(commands.download(texture.value(), readback.value()));
    submit(device, std::move(commands));
    const auto bytes = readback.value().map();
    CHECK(bytes.size() == 64U);
    for (std::size_t i = 0U; i < bytes.size(); ++i)
        CHECK(bytes[i] == (i < 32U ? 41U : 177U));
    readback.value().unmap();
}

void gpu_transactions(rhi::Device &device) {
    auto decoded = asset(16U);
    std::array<render::TextureRequest, 1U> requests{
        {{"set", decoded, 1U, 0U, 16.0F, 1U}}};
    render::TextureStreamer stream{4U};
    for (int i = 0; i < 3; ++i) {
        auto commands = acquire(device);
        CHECK(stream.prepare(device, commands, requests, 4096U, true, 0U));
        CHECK(stream.stats().allocated_bytes == 1364U);
        CHECK(stream.stats().upload_bytes == 1364U);
        CHECK(stream.find("set", 1U, 0U));
        if (device.capabilities().backend == rhi::BackendKind::D3d12)
            CHECK(!stream.find("set", 1U, 0U)->safe_to_release());
        // Deliberately cancel every allocation's recording.
    }
    auto commands = acquire(device);
    CHECK(stream.prepare(device, commands, requests, 4096U, true, 0U));
    check_pixels(device, commands, *stream.find("set", 1U, 0U), 96U);
    commands = acquire(device);
    CHECK(stream.prepare(device, commands, requests, 4096U, true, 0U));
    CHECK(stream.stats().upload_bytes == 0U);
    submit(device, std::move(commands));
    // Genuine tail exhaustion preserves the last complete texture and pixels.
    commands = acquire(device);
    CHECK(!stream.prepare(device, commands, requests, 100U, true, 0U));
    CHECK(stream.stats().state == render::TextureStreamState::CapacityRefused);
    CHECK(stream.find("set", 1U, 0U)->width() == 16U);
    check_pixels(device, commands, *stream.find("set", 1U, 0U), 96U);
    // Budget eviction retains all three tail levels, including sampleable 4x4.
    commands = acquire(device);
    CHECK(stream.prepare(device, commands, requests, 168U, true, 0U));
    CHECK(stream.first_mip("set", 0U) == 2U);
    CHECK(stream.find("set", 1U, 0U)->desc().mip_levels == 3U);
    check_pixels(device, commands, *stream.find("set", 1U, 0U), 96U);
    commands = acquire(device);
    CHECK(stream.prepare(device, commands, requests, 168U, true, 0U));
    CHECK(stream.stats().allocated_bytes ==
          84U); // Submitted old generation was reclaimed.
    // A new desired plan cannot leak into another call in the same recording.
    requests[0].generation = 2U;
    requests[0].asset = asset(16U, 177U);
    CHECK(stream.prepare(device, commands, requests, 4096U, true, 0U));
    CHECK(stream.find("set", 2U, 0U) == nullptr);
    submit(device, std::move(commands));
    commands = acquire(device);
    CHECK(stream.prepare(device, commands, requests, 4096U, true, 0U));
    check_pixels(device, commands, *stream.find("set", 2U, 0U), 177U);
    // Mip churn does not spend the model cache's 64 refresh-generation slots.
    for (core::u32 i = 0U; i < 70U; ++i) {
        commands = acquire(device);
        CHECK(
            stream.prepare(device, commands, requests, 4096U, true, i % 2U ? 12U : 0U));
        CHECK(stream.stats().allocated_bytes <= 1448U);
        submit(device, std::move(commands));
    }
    device.wait_idle();
}

void large_mips_progress_in_bounded_recordings(rhi::Device &device) {
    auto decoded = asset(2048U, 41U);
    std::array<render::TextureRequest, 1U> requests{
        {{"large", decoded, 1U, 0U, 2048.0F, 1U}}};
    render::TextureStreamer stream{2U};
    auto commands = acquire(device);
    CHECK(stream.prepare(device, commands, requests, 64ULL * 1024U * 1024U, true, 0U));
    CHECK(stream.stats().state == render::TextureStreamState::Loading);
    CHECK(stream.stats().upload_bytes == 16U * 1024U * 1024U);
    CHECK(stream.find("large", 1U, 0U) == nullptr);
    submit(device, std::move(commands));
    const auto bytes = stream.stats().allocated_bytes;
    {
        auto canceled = acquire(device);
        CHECK(stream.prepare(device, canceled, requests, 64ULL * 1024U * 1024U, true,
                             0U));
        CHECK(stream.stats().state == render::TextureStreamState::Ready);
    }
    commands = acquire(device);
    CHECK(stream.prepare(device, commands, requests, 64ULL * 1024U * 1024U, true, 0U));
    CHECK(stream.stats().allocated_bytes == bytes);
    CHECK(stream.stats().upload_bytes == 5592404U);
    check_pixels(device, commands, *stream.find("large", 1U, 0U), 41U);
    device.wait_idle();
}

std::string write_scene_fixture() {
    const auto root = std::filesystem::temp_directory_path() / "mediaserver-step13";
    std::filesystem::create_directories(root);
    {
        std::ofstream out(root / "mesh.bin", std::ios::binary | std::ios::trunc);
        const std::array<float, 15U> data{-1.0F, -1.0F, 0.0F, 1.0F, -1.0F,
                                          0.0F,  0.0F,  1.0F, 0.0F, 0.0F,
                                          0.0F,  1.0F,  0.0F, 0.5F, 1.0F};
        out.write(reinterpret_cast<const char *>(data.data()), sizeof(data));
        CHECK(out);
    }
    {
        std::ofstream out(root / "image.tga", std::ios::binary | std::ios::trunc);
        std::array<core::u8, 18U> header{};
        header[2U] = 2U;
        header[13U] = 2U;
        header[15U] = 2U;
        header[16U] = 32U;
        header[17U] = 0x28U;
        out.write(reinterpret_cast<const char *>(header.data()), header.size());
        for (core::u32 i = 0U; i < 512U * 512U; ++i) {
            const std::array<core::u8, 4U> pixel{0U, 0U, 255U, 255U};
            out.write(reinterpret_cast<const char *>(pixel.data()), pixel.size());
        }
        CHECK(out);
    }
    {
        std::ofstream out(root / "set.gltf", std::ios::trunc);
        out << R"({"asset":{"version":"2.0"},"buffers":[{"uri":"mesh.bin","byteLength":60}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},
{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"}],
"images":[{"uri":"image.tga"}],"textures":[{"source":0}],
"materials":[{"pbrMetallicRoughness":{"baseColorFactor":[0,0,0,1]},"emissiveFactor":[1,1,1],"emissiveTexture":{"index":0}}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"material":0}]}],
"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
        CHECK(out);
    }
    return (root / "set.gltf").string();
}

void renderers_share_detail_across_slices_and_view_overrides(rhi::Device &device) {
    const auto path = write_scene_fixture();
    render::ModelCache cache;
    static_cast<void>(cache.observe(path, cache.publication_epoch()));
    CHECK(render::detail::wait_for_shared_model_cache_for_test());
    scene::Scene frame;
    frame.canvas = {64U, 32U};
    frame.camera.eye = {0.0F, 0.0F, 4.0F};
    frame.camera.target = {0.0F, 0.0F, 0.0F};
    frame.renderer_settings.backend = scene::RendererBackend::Advanced;
    scene::ModelInstance model;
    model.source = path;
    model.world = scene::identity();
    model.normal = scene::normal_basis(model.world);
    frame.models.push_back(model);
    render::RendererLimits limits;
    limits.max_instances = 16U;
    limits.max_model_textures = 8U;
    limits.max_vertices = 256U;
    limits.max_indices = 1024U;
    limits.directional_shadow_resolution = 32U;
    limits.max_model_texture_bytes = 32768U; // The 1 MiB source cannot fit whole.
    auto a = render::SceneRenderer::create(device, 32U, 32U, limits,
                                           render::SceneRendererMode::AdvancedLinear);
    auto b = render::SceneRenderer::create(device, 32U, 32U, limits,
                                           render::SceneRendererMode::AdvancedLinear);
    CHECK(a && b);
    auto commands = acquire(device);
    CHECK(a.value().render(commands, frame, {0U, 0U, 32U, 32U}));
    CHECK(a.value().stats().instances == 1U);
    CHECK(a.value().stats().texture_streaming.reduced == 1U);
    auto readback = device.create_readback(32U * 32U * 8U);
    CHECK(readback);
    CHECK(commands.download(a.value().target(), readback.value()));
    submit(device, std::move(commands));
    const auto image = readback.value().map();
    core::u32 red_pixels{};
    for (std::size_t i = 0U; i + 7U < image.size(); i += 8U) {
        const auto red = static_cast<core::u32>(image[i]) |
                         (static_cast<core::u32>(image[i + 1U]) << 8U);
        const auto green = static_cast<core::u32>(image[i + 2U]) |
                           (static_cast<core::u32>(image[i + 3U]) << 8U);
        if (red >= 0x3800U && red <= 0x7bffU && green < 0x1000U)
            ++red_pixels;
    }
    CHECK(red_pixels > 8U); // The streamed emissive map reached real raster pixels.
    readback.value().unmap();
    commands = acquire(device);
    render::SceneRenderOptions options;
    render::SceneRenderView view;
    auto close = frame.camera;
    close.eye.z = 1.0F;
    view.matrices = scene::view_for_slice(close, frame.canvas, {32U, 0U, 32U, 32U});
    view.eye = close.eye;
    view.forward = {0.0F, 0.0F, -1.0F};
    options.view = view;
    CHECK(b.value().render(commands, frame, {32U, 0U, 32U, 32U}, options));
    CHECK(a.value().resident_texture_mip(path, 0U) ==
          b.value().resident_texture_mip(path, 0U));
    CHECK(a.value().stats().texture_streaming.resident_bytes ==
          b.value().stats().texture_streaming.resident_bytes);
    CHECK(b.value().stats().texture_streaming.allocated_bytes <= 32768U);
    submit(device, std::move(commands));
    device.wait_idle();
    // Complete textures cannot make a cold geometry-capacity refusal look
    // like a valid frame with the new model missing.
    limits.max_vertices = 2U;
    auto refused = render::SceneRenderer::create(
        device, 32U, 32U, limits, render::SceneRendererMode::AdvancedLinear);
    CHECK(refused);
    {
        auto canceled = acquire(device);
        CHECK(!refused.value().render(canceled, frame, {0U, 0U, 32U, 32U}));
        CHECK(refused.value().stats().model_integration_refusals == 1U);
    }
}

void compressed_mips_upload_and_sample(rhi::Device& device) {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    if (!device.supports(rhi::ShaderFormat::Dxil)) return;
    for (const bool normal : {false,true}) {
        auto decoded=asset(16U,128U);
        render::ModelPart part;
        if(normal) part.normal_texture=0U; else part.base_color_texture=0U;
        decoded->parts.push_back(part);
        CHECK(render::compress_model_textures(*decoded));
        CHECK(decoded->textures[0].rgba.size()==256U);
        render::TextureStreamer stream{4U};
        std::array<render::TextureRequest,1U> requests{{{"bc",decoded,1U,0U,16.0F,1U}}};
        // Unequal and non-power-of-two dimensions must never promote a 6x6
        // or 4x2 mip into a BC resource's base level.
        for (const auto dimensions : {std::pair{12U,12U}, std::pair{16U,8U}}) {
            auto rectangular=asset(dimensions.first,128U);
            rectangular->textures[0].height=dimensions.second;
            rectangular->textures[0].rgba.resize(static_cast<std::size_t>(dimensions.first)*dimensions.second*4U,128U);
            rectangular->parts.push_back(part);
            CHECK(render::compress_model_textures(*rectangular));
            render::TextureStreamer reduced{4U};
            std::array<render::TextureRequest,1U> tiny{{{"rect",rectangular,1U,0U,1.0F,1U}}};
            auto command=acquire(device); CHECK(reduced.prepare(device,command,tiny,4096U,true,10U));
            const auto* resident=reduced.find("rect",1U,0U); CHECK(resident);
            CHECK(resident->width()%4U==0U && resident->height()%4U==0U);
            CHECK(resident->width()==(dimensions.first==12U?12U:8U));
            submit(device,std::move(command));
        }
        rhi::ShaderDesc vertex; vertex.stage=rhi::ShaderStage::Vertex; vertex.format=rhi::ShaderFormat::Dxil;
        vertex.bytecode={mediaserver_output_vert_dxil,sizeof(mediaserver_output_vert_dxil)};
        rhi::ShaderDesc fragment; fragment.stage=rhi::ShaderStage::Fragment; fragment.format=rhi::ShaderFormat::Dxil;
        fragment.bytecode={mediaserver_texture_probe_frag_dxil,sizeof(mediaserver_texture_probe_frag_dxil)};
        fragment.sampler_count=1U; fragment.uniform_buffer_count=1U;
        auto pipeline=device.create_pipeline(vertex,fragment,rhi::TextureFormat::Rgba8Unorm,rhi::ColorBlend::Disabled); CHECK(pipeline);
        auto sampler=device.create_sampler(rhi::Filter::Nearest); CHECK(sampler);
        rhi::TextureDesc output; output.width=output.height=4U; output.color_target=true; output.sampled=true;
        auto target=device.create_texture(output); CHECK(target);
        // Cancel a complete compressed upload. Retrying must re-record all
        // block rows, including 2x2 and 1x1 mip tails, before making it visible.
        { auto canceled=acquire(device); CHECK(stream.prepare(device,canceled,requests,4096U,true,0U)); }
        for(const float level : {0.0F,4.0F}) {
            auto commands=acquire(device); CHECK(stream.prepare(device,commands,requests,4096U,true,0U));
            const auto* texture=stream.find("bc",1U,0U); CHECK(texture);
            CHECK(texture->desc().format==(normal?rhi::TextureFormat::Bc5Unorm:rhi::TextureFormat::Bc7Unorm));
            std::array<float,4U> parameters{level,0,0,0};
            CHECK(commands.draw_output_transform(target.value(),{},pipeline.value(),*texture,sampler.value(),
                {reinterpret_cast<const core::u8*>(parameters.data()),sizeof(parameters)}));
            auto read=device.create_readback(64U); CHECK(read); CHECK(commands.download(target.value(),read.value()));
            submit(device,std::move(commands));
            const auto pixels=read.value().map(); CHECK(pixels.size()==64U);
            // BC5's third channel is zero; its XY reconstructs Z in the material
            // shader. Role-normalised lower mips differ from the base fixture.
            for(std::size_t i=0U;i<pixels.size();i+=4U) {
                const int expected=normal && level>0.0F ? 201 : 128;
                CHECK(std::abs(static_cast<int>(pixels[i])-expected)<=3);
                CHECK(std::abs(static_cast<int>(pixels[i+1U])-expected)<=3);
                CHECK(pixels[i+3U]>=254U);
            }
            read.value().unmap();
        }
    }
#else
    static_cast<void>(device);
#endif
}

} // namespace

int main() {
    try {
        filtering_preserves_material_roles_and_odd_edges();
        desired_levels_and_budget_are_deterministic();
        std::cout << "[pass] worker mip filtering, full-detail request and "
                     "deterministic tail budgets\n";
        for (const auto backend : {rhi::BackendKind::SdlGpu, rhi::BackendKind::D3d12}) {
            if (!rhi::Device::available(backend)) {
                std::cout << "[skip] unavailable GPU backend\n";
                continue;
            }
            rhi::DeviceConfig config;
            config.backend = backend;
            auto opened = rhi::Device::create(config);
            CHECK(opened);
            auto device = std::move(opened).value();
            compressed_mips_upload_and_sample(device);
            row_uploads_preserve_other_rows(device);
            gpu_transactions(device);
            large_mips_progress_in_bounded_recordings(device);
            if (backend == rhi::BackendKind::D3d12)
                renderers_share_detail_across_slices_and_view_overrides(device);
            std::cout << "[pass] GPU transactions, tail pixels, bounded uploads, "
                         "reclamation, backend "
                      << static_cast<int>(backend) << '\n';
        }
    } catch (const std::exception &error) {
        std::cerr << "[fail] " << error.what() << '\n';
        return 1;
    }
}
