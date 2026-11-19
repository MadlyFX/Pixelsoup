#include <mediaserver/render/image_decode.hpp>
#include <mediaserver/render/display_lut_cache.hpp>
#include <mediaserver/render/output_transform.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/output/feed_map_model.hpp>
#include <mediaserver/output/feed_map_schema.hpp>
#include <mediaserver/topology/hardware_map_model.hpp>
#include <mediaserver/topology/hardware_map_schema.hpp>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfTiledOutputFile.h>
#include <OpenEXR/ImfTileDescription.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfIO.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfStringAttribute.h>
#include <OpenColorIO/OpenColorIO.h>
#include <Imath/half.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;
namespace output = mediaserver::output;
namespace topology = mediaserver::topology;
namespace exr = OPENEXR_IMF_NAMESPACE;
namespace ocio = OCIO_NAMESPACE;
using Half = IMATH_NAMESPACE::half;
#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error(std::to_string(__LINE__) + ": " #__VA_ARGS__); } while(false)

class MemoryOutput final : public exr::OStream {
public:
    MemoryOutput() : exr::OStream("test.exr") {}
    std::vector<core::u8> bytes;
    std::size_t cursor{};
    void write(const char* source, int count) override {
        CHECK(count >= 0);
        const auto size = static_cast<std::size_t>(count);
        if (cursor + size > bytes.size()) bytes.resize(cursor + size);
        std::memcpy(bytes.data() + cursor, source, size); cursor += size;
    }
    std::uint64_t tellp() override { return cursor; }
    void seekp(std::uint64_t offset) override { cursor = static_cast<std::size_t>(offset); }
};

template<class Pixel>
std::vector<core::u8> encode_exr(const std::vector<Pixel>& pixels, exr::PixelType type,
                              const char* tag, exr::Compression compression = exr::ZIP_COMPRESSION,
                              bool tiled = false) {
    MemoryOutput output_stream;
    exr::Header header{static_cast<int>(pixels.size() / 4U), 1};
    header.compression() = compression;
    // Cropped/overscan files need not start at (0,0).
    header.dataWindow().min = {11,-4};
    header.dataWindow().max = {10 + static_cast<int>(pixels.size() / 4U),-4};
    if (tag != nullptr) header.insert("ocio:colorspace", exr::StringAttribute{tag});
    exr::FrameBuffer buffer;
    constexpr std::array names{"R", "G", "B", "A"};
    for (std::size_t i = 0; i < 4U; ++i) {
        header.channels().insert(names[i], exr::Channel{type});
        buffer.insert(names[i], exr::Slice::Make(type, pixels.data() + i, header.dataWindow(),
            sizeof(Pixel) * 4U, pixels.size() * sizeof(Pixel)));
    }
    if (tiled) {
        header.setTileDescription(exr::TileDescription{16,16,exr::ONE_LEVEL});
        exr::TiledOutputFile file{output_stream, header, 1}; file.setFrameBuffer(buffer);
        file.writeTiles(0, file.numXTiles()-1, 0, file.numYTiles()-1);
    } else { exr::OutputFile file{output_stream, header, 1}; file.setFrameBuffer(buffer); file.writePixels(1); }
    return output_stream.bytes;
}

void exr_contract() {
    constexpr std::array<core::u16,16> bits{0x0001,0x03ff,0x3c00,0x3800,
        0xbc00,0x4000,0x7bff,0x3c00,0x0000,0x8000,0x3555,0x0000,0x4600,0x4800,0x4c00,0x3400};
    std::vector<Half> pixels(bits.size());
    for (std::size_t i=0; i<bits.size(); ++i) pixels[i].setBits(bits[i]);
    for (auto compression : {exr::NO_COMPRESSION, exr::ZIP_COMPRESSION, exr::PIZ_COMPRESSION}) {
        const auto bytes = encode_exr(pixels, exr::HALF, "ACEScg", compression);
        const auto decoded = render::decode_exr_bytes(bytes);
        CHECK(decoded.valid() && decoded.width == 4U && decoded.height == 1U);
        CHECK(decoded.primaries == render::ImagePrimaries::AcesCg);
        CHECK(decoded.pixels.size() == sizeof(bits));
        CHECK(std::memcmp(decoded.pixels.data(), bits.data(), sizeof(bits)) == 0);
        CHECK(!render::decode_exr_bytes(std::span<const core::u8>{bytes}.first(bytes.size()-9U)).valid());
    }
    const auto bytes = encode_exr(std::vector<float>{1.0F/3.0F, 6.5F, -2.0F, 0.5F}, exr::FLOAT, "lin_rec709");
    const auto converted = render::decode_exr_bytes(bytes);
    constexpr std::array<core::u16,4> expected{0x3555,0x4680,0xc000,0x3800};
    CHECK(converted.valid() && converted.pixels.size() == 8U);
    CHECK(std::memcmp(converted.pixels.data(), expected.data(), 8U) == 0);
    CHECK(!render::decode_exr_bytes(encode_exr(pixels, exr::HALF, "unrecognized-gamut")).valid());
    const auto tiled = render::decode_exr_bytes(encode_exr(pixels, exr::HALF, "ACEScg", exr::ZIP_COMPRESSION, true));
    CHECK(tiled.valid() && std::memcmp(tiled.pixels.data(), bits.data(), sizeof(bits)) == 0);
    const auto premult = render::decode_image_bytes(encode_exr(std::vector<float>{1,0.25F,0,0.5F}, exr::FLOAT, nullptr));
    std::array<Half,4> straight; std::memcpy(straight.data(), premult.pixels.data(), 8U);
    CHECK(static_cast<float>(straight[0]) == 2.0F && static_cast<float>(straight[1]) == 0.5F);
    CHECK(static_cast<float>(straight[3]) == 0.5F);
    const auto ap1 = encode_exr(std::vector<float>{1,0,0,0.5F}, exr::FLOAT, "ACEScg");
    const auto working = render::decode_image_bytes(ap1);
    CHECK(working.valid() && working.primaries == render::ImagePrimaries::Rec709);
    std::array<Half,4> working_pixel; std::memcpy(working_pixel.data(), working.pixels.data(), 8U);
    CHECK(static_cast<float>(working_pixel[0]) > 1.7F);
    CHECK(static_cast<float>(working_pixel[1]) < -0.1F);
    CHECK(working_pixel[3].bits() == 0x3800U);
    const auto radiance = render::decode_radiance_bytes(ap1);
    core::u16 alpha{}; std::memcpy(&alpha, radiance.pixels.data()+6U, 2U); CHECK(alpha == 0x3c00U);
}

void aces_reference() {
    const auto config = ocio::Config::CreateFromBuiltinConfig("cg-config-v1.0.0_aces-v1.3_ocio-v2.1");
    const auto cpu = config->getProcessor("Linear Rec.709 (sRGB)", "sRGB - Display", "ACES 1.0 - SDR Video",
        ocio::TRANSFORM_DIR_FORWARD)->getOptimizedCPUProcessor(ocio::OPTIMIZATION_NONE);
    const auto lut = render::bake_display_lut(scene::DisplayTransform::Aces13,33U);
    CHECK(lut.valid());
    CHECK(lut == render::bake_display_lut(scene::DisplayTransform::Aces13,33U));
    const std::array<std::array<float,3>,10> tests{{{0,0,0},{0.18F,0.18F,0.18F},{1,1,1},{4,4,4},
        {16,16,16},{0.18F,0,0},{0,0.18F,0},{0,0,0.18F},{0.18F,0.18F,0},{0.18F,0.09F,0.04F}}};
    float max_error = 0.0F;
    for (const auto& input : tests) {
        auto reference = input; cpu->applyRGB(reference.data());
        float shaped[3]{}; for(std::size_t i=0;i<3;++i) shaped[i] = render::display_shaper(input[i]);
        float actual[3]{}; render::sample_display_lut(lut,shaped,actual);
        for (std::size_t i=0;i<3;++i) {
            max_error = std::max(max_error, std::abs(reference[i]-actual[i]));
            CHECK(std::abs(reference[i]-actual[i]) < 0.018F);
        }
    }
    std::cout << "ACES standard-sample maximum interpolation error: " << max_error << '\n';
    // Grid vertices isolate reference accuracy from the 33-point interpolation error.
    for (unsigned b : {0U,4U,16U,24U,31U}) for(unsigned g : {0U,8U,16U,24U}) for(unsigned r : {0U,8U,16U,24U}) {
        float reference[3]{static_cast<float>(r)/(32.0F-static_cast<float>(r)),
            static_cast<float>(g)/(32.0F-static_cast<float>(g)),static_cast<float>(b)/(32.0F-static_cast<float>(b))};
        cpu->applyRGB(reference);
        const auto index = ((b*33U+g)*33U+r)*3U;
        for(unsigned c=0;c<3;++c) CHECK(std::abs(lut.values[index+c]-reference[c]) < 1.0e-6F);
    }
}

// observe() never waits: while the worker holds the cache, for a moment after
// every bake, it answers Busy and admits nothing. A check that means "this
// request, now" asks again until it gets an answer.
render::DisplayLutObservation observed(render::DisplayLutCache& cache, const render::DisplayLutRequest& request) {
    for (;;) {
        auto result = cache.observe(request);
        if (result.state != render::DisplayLutState::Busy) return result;
        std::this_thread::yield();
    }
}

void wall_residency() {
    render::DisplayLutCache cache;
    render::DisplayLutRequest red{scene::DisplayTransform::Srgb,{},33U,30001U};
    red.wall.enabled=true; red.wall.primaries[0]=0.25F;
    auto green=red; green.wall.primaries[0]=1; green.wall.primaries[4]=0.25F;
    static_cast<void>(observed(cache, red)); static_cast<void>(observed(cache, green)); CHECK(cache.wait_for_test());
    const auto a=observed(cache, red), b=observed(cache, green);
    CHECK(a.state==render::DisplayLutState::Ready && b.state==render::DisplayLutState::Ready);
    const float white[3]{0.5F,0.5F,0.5F}; float r[3]{},g[3]{};
    render::sample_display_lut(*a.table,white,r); render::sample_display_lut(*b.table,white,g);
    CHECK(std::abs(r[0]-0.5370987F)<0.00001F && r[1]>0.999F);
    CHECK(std::abs(g[1]-0.5370987F)<0.00001F && g[0]>0.999F);
    for(unsigned i=0;i<30;++i) { CHECK(observed(cache, red).generation==a.generation); CHECK(observed(cache, green).generation==b.generation); }
    CHECK(cache.refresh(red) && cache.wait_for_test()); CHECK(observed(cache, red).generation>a.generation);
    CHECK(observed(cache, green).generation==b.generation);
    auto broken=red; broken.wall.camera_inverse_lut_source="missing-inverse.cube";
    static_cast<void>(observed(cache, broken)); CHECK(cache.wait_for_test()); CHECK(observed(cache, broken).state==render::DisplayLutState::Failed);

    const std::string inverse = "LUT_3D_SIZE 2\n0 0 0\n0.5 0 0\n0 1 0\n0.5 1 0\n0 0 1\n0.5 0 1\n0 1 1\n0.5 1 1\n";
    CHECK(render::parse_cube_lut("DOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n" + inverse).valid());
    CHECK(!render::parse_cube_lut("DOMAIN_MIN -1 0 0\n" + inverse).valid());
    CHECK(!render::parse_cube_lut(inverse + "nan 0 0\n").valid());
    CHECK(!render::parse_cube_lut(inverse + "LUT_3D_SIZE 2\n").valid());
    // Unique per run: the Debug and Release suites may run this at the same time.
    const auto path = std::filesystem::temp_directory_path() / ("mediaserver-camera-inverse-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".cube");
    { std::ofstream file{path}; file << inverse; }
    auto calibrated = red; calibrated.wall.primaries[0] = 1.0F;
    calibrated.wall.camera_inverse_lut_source = path.string();
    static_cast<void>(observed(cache, calibrated)); CHECK(cache.wait_for_test());
    const auto camera = observed(cache, calibrated); CHECK(camera.state == render::DisplayLutState::Ready);
    float result[3]{}; render::sample_display_lut(*camera.table, white, result);
    CHECK(std::abs(result[0]-0.5F) < 0.00001F && result[1] > 0.999F);
    std::filesystem::remove(path);
}

void document_contract() {
    doc::Node stage; stage.id=doc::NodeId{1}; stage.type=scene::schema::stage_type;
    scene::RendererSettings settings; settings.color.display_transform=scene::DisplayTransform::Aces13;
    settings.color.show_lut_source="show.cube"; settings.color.dither=false;
    doc::Node settings_node; settings_node.id=doc::NodeId{10}; settings_node.type=scene::schema::settings_type;
    scene::write_renderer_settings(settings_node,settings);
    doc::Node wall; wall.id=doc::NodeId{2}; wall.type=output::schema::surface_type;
    wall.properties[std::string{output::schema::wall_color_enabled}]=true;
    wall.properties[std::string{output::schema::camera_inverse_lut}]=std::string{"camera.cube"};
    for(std::size_t i=0;i<9;++i) wall.properties[std::string{output::schema::wall_primaries[i]}]=0.1*static_cast<double>(i+1);
    auto root=doc::Root{}.set(stage).set(settings_node).set(wall);
    doc::Node server; server.id=doc::NodeId{4}; server.type=topology::schema::device_type;
    server.properties["kind"]=std::string{topology::schema::render_server_kind}; root=root.set(server);
    scene::SceneModel scene_model; scene_model.reset(root); const auto frame=scene_model.evaluate({0,60,1});
    CHECK(frame.renderer_settings.color==settings.color);
    CHECK(frame.wall_colors.size()==1U && frame.wall_colors[0].second.camera_inverse_lut_source=="camera.cube");
    output::FeedMapModel output_model; output_model.reset(root); const auto mapped=output_model.evaluate();
    CHECK(mapped.surfaces.size()==1U && mapped.surfaces[0].color==frame.wall_colors[0].second);
    auto changed=frame; changed.wall_colors[0].second.primaries[5]+=0.1F;
    CHECK(scene::asset_set_digest(changed)!=scene::asset_set_digest(frame)); CHECK(scene::digest(changed)!=scene::digest(frame));
    auto changed_map=mapped; changed_map.surfaces[0].color.primaries[5]+=0.1F;
    CHECK(output::digest(mapped)!=output::digest(changed_map));
    doc::Node binding; binding.id=doc::NodeId{3}; binding.type=topology::schema::output_binding_type;
    binding.properties["render_server"]=std::int64_t{4}; binding.properties["logical_port"]=std::int64_t{1};
    binding.properties["display_adapter_id"]=topology::format_display_adapter_id(123U);
    binding.properties["output_bit_depth"]=std::int64_t{10};
    topology::HardwareMapModel hardware; hardware.reset(root.set(binding));
    const auto ten=hardware.evaluate(); CHECK(ten.output_bindings.size()==1U && ten.output_bindings[0].output_bit_depth==10U);
    binding.properties["output_bit_depth"]=std::int64_t{16}; hardware.reset(root.set(binding));
    CHECK(topology::digest(ten)!=topology::digest(hardware.evaluate()));
    binding.properties["output_bit_depth"]=std::int64_t{12}; hardware.reset(root.set(binding));
    CHECK(hardware.evaluate().output_bindings.empty());
}

rhi::CommandList acquire(rhi::Device& device) { auto result=device.acquire_commands(); CHECK(result); return std::move(result).value(); }
std::vector<core::u8> readback(rhi::Device& device,rhi::CommandList commands,const rhi::Texture& target,unsigned stride) {
    auto rb=device.create_readback(target.width()*target.height()*stride); CHECK(rb);
    CHECK(commands.download(target,rb.value())); auto submitted=device.submit(std::move(commands)); CHECK(submitted && submitted.value().wait());
    auto mapped=rb.value().map(); std::vector<core::u8> bytes(mapped.begin(),mapped.end()); rb.value().unmap(); return bytes;
}

void gpu_precision(rhi::Device& device) {
    constexpr unsigned width=1024;
    std::vector<Half> linear(width*4U);
    for(unsigned x=0;x<width;++x) {
        const float encoded=static_cast<float>(x)/1023.0F;
        const float value=encoded<=0.04045F?encoded/12.92F:std::pow((encoded+0.055F)/1.055F,2.4F);
        linear[x*4]=linear[x*4+1]=linear[x*4+2]=Half{value}; linear[x*4+3]=Half{1.0F};
    }
    auto source=device.create_texture({width,1,rhi::TextureFormat::Rgba16Float,false,true}); CHECK(source);
    auto upload=acquire(device); CHECK(upload.upload(source.value(),{reinterpret_cast<const core::u8*>(linear.data()),linear.size()*2U}));
    auto sent=device.submit(std::move(upload)); CHECK(sent && sent.value().wait());
    const auto lut=render::bake_display_lut(scene::DisplayTransform::Srgb,33U);
    const auto run=[&](rhi::TextureFormat format,unsigned start,unsigned count) {
        auto output_transform=render::OutputTransform::create(device,count,1,true,format); CHECK(output_transform);
        CHECK(output_transform.value().output_bit_depth()==(format==rhi::TextureFormat::Rgb10A2Unorm?10U:16U));
        output_transform.value().set_display_lut(lut);
        auto cut=device.create_texture({count,1,rhi::TextureFormat::Rgba16Float,false,true}); CHECK(cut);
        auto cmd=acquire(device); CHECK(cmd.upload(cut.value(),{reinterpret_cast<const core::u8*>(linear.data()+start*4U),count*8U}));
        CHECK(output_transform.value().render(cmd,cut.value(),{0,0,count,1},true,{start,0,count,1}));
        return readback(device,std::move(cmd),output_transform.value().target(),format==rhi::TextureFormat::Rgb10A2Unorm?4U:8U);
    };
    const auto full=run(rhi::TextureFormat::Rgb10A2Unorm,0,width);
    const auto left=run(rhi::TextureFormat::Rgb10A2Unorm,0,512),right=run(rhi::TextureFormat::Rgb10A2Unorm,512,512);
    CHECK(std::equal(left.begin(),left.end(),full.begin())); CHECK(std::equal(right.begin(),right.end(),full.begin()+2048));
    std::set<unsigned> codes;
    for(unsigned x=0;x<width;++x) { unsigned pixel{}; std::memcpy(&pixel,full.data()+x*4U,4U); codes.insert(pixel&1023U); }
    std::cout << "Unique ten-bit codes: " << codes.size() << " first " << *codes.begin() << " last " << *codes.rbegin() << '\n';
    CHECK(codes.size()>512U); // An 8-bit intermediate can produce at most 256 codes.
    const auto floating=run(rhi::TextureFormat::Rgba16Float,0,width);
    Half grey; std::memcpy(&grey,floating.data()+512U*8U,2U);
    CHECK(std::abs(static_cast<float>(grey)-0.214F)<0.003F); // scRGB carries linear, not encoded 0.5.
}

void gpu_walls(rhi::Device& device) {
    auto output_transform=render::OutputTransform::create(device,64,8,true,rhi::TextureFormat::Rgb10A2Unorm); CHECK(output_transform);
    auto& transform=output_transform.value(); scene::ColorSettings color; color.display_transform=scene::DisplayTransform::Srgb;
    scene::WallColorSettings red,green; red.enabled=green.enabled=true; red.primaries[0]=0.25F; green.primaries[4]=0.25F;
    render::DisplayLutCache cache;
    for (unsigned attempt=0; attempt<100U; ++attempt) {
        transform.adopt_color(color,30002U);
        CHECK(transform.adopt_wall_color(doc::NodeId{7},red)); CHECK(transform.adopt_wall_color(doc::NodeId{8},green));
        if (transform.color_state()==render::DisplayLutState::Ready) break;
        CHECK(cache.wait_for_test()); std::this_thread::yield();
    }
    CHECK(transform.color_state()==render::DisplayLutState::Ready);
    auto source=device.create_texture({32,8,rhi::TextureFormat::Rgba16Float,true,true}); CHECK(source);
    {
        // A discarded recording must not leave either wall marked uploaded.
        auto canceled = acquire(device); CHECK(canceled.clear(source.value(),{1,1,1,1})); CHECK(transform.clear(canceled));
        CHECK(transform.composite(canceled,source.value(),{0,0,32,8},true,{0,0,32,8},doc::NodeId{7}));
        CHECK(transform.composite(canceled,source.value(),{32,0,32,8},true,{0,0,32,8},doc::NodeId{8}));
    }
    auto cmd=acquire(device); CHECK(cmd.clear(source.value(),{1,1,1,1})); CHECK(transform.clear(cmd));
    CHECK(transform.composite(cmd,source.value(),{0,0,32,8},true,{0,0,32,8},doc::NodeId{7}));
    CHECK(transform.composite(cmd,source.value(),{32,0,32,8},true,{0,0,32,8},doc::NodeId{8}));
    const auto bytes=readback(device,std::move(cmd),transform.target(),4U);
    unsigned a{},b{}; std::memcpy(&a,bytes.data()+16U*4U,4U); std::memcpy(&b,bytes.data()+48U*4U,4U);
    CHECK((a&1023U)>540U && (a&1023U)<560U && ((a>>10U)&1023U)>=1022U);
    CHECK((b&1023U)>=1022U && ((b>>10U)&1023U)>540U && ((b>>10U)&1023U)<560U);
}

void gpu_exr_media(rhi::Device& device) {
    const auto path = std::filesystem::temp_directory_path() / "mediaserver-facility-color-media.exr";
    const auto encoded = encode_exr(std::vector<float>{2.0F,0.5F,0.0F,0.5F}, exr::FLOAT, "lin_rec709");
    { std::ofstream file{path, std::ios::binary}; file.write(reinterpret_cast<const char*>(encoded.data()), encoded.size()); }
    scene::Scene frame; frame.canvas = {32,32}; frame.camera.eye = {0,0,3}; frame.camera.target = {};
    frame.environment.sky_color = {}; frame.renderer_settings.shadow_cascades = 0;
    scene::MediaInstance clip; clip.source = path.string(); clip.source_node = doc::NodeId{900};
    clip.mesh = {scene::Primitive::Quad,1U}; clip.world = scene::scaling({3,3,1}); clip.normal = scene::normal_basis(clip.world);
    frame.media.push_back(clip);
    auto renderer = render::SceneRenderer::create(device,32,32,{},render::SceneRendererMode::AdvancedLinear); CHECK(renderer);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    for (;;) {
        auto cmd = acquire(device); CHECK(renderer.value().render(cmd,frame,{0,0,32,32}));
        if (renderer.value().stats().media == 1U) {
            const auto bytes = readback(device,std::move(cmd),renderer.value().target(),8U);
            std::array<Half,4> centre; std::memcpy(centre.data(),bytes.data()+(16U*32U+16U)*8U,8U);
            CHECK(std::abs(static_cast<float>(centre[0])-2.0F) < 0.01F);
            CHECK(std::abs(static_cast<float>(centre[1])-0.5F) < 0.01F);
            break;
        }
        auto sent = device.submit(std::move(cmd)); CHECK(sent && sent.value().wait());
        CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::yield();
    }
    std::filesystem::remove(path);
}

void swapchains(rhi::Device& device) {
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    for(auto format : {rhi::TextureFormat::Rgb10A2Unorm,rhi::TextureFormat::Rgba16Float}) {
        auto* window=SDL_CreateWindow("Facility colour verification",64,64,SDL_WINDOW_HIDDEN); CHECK(window);
        { auto chain=device.claim_window(window,rhi::PresentMode::Immediate,format); CHECK(chain); CHECK(chain.value().format()==format); }
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
}

int main(int argc,char** argv) {
    try {
        if (argc>1 && std::string_view{argv[1]}=="--gpu") {
            rhi::DeviceConfig config; config.backend=rhi::BackendKind::D3d12;
            auto device=rhi::Device::create(config); if(!device) return 77;
            gpu_precision(device.value()); gpu_walls(device.value()); gpu_exr_media(device.value()); swapchains(device.value());
        } else { exr_contract(); aces_reference(); wall_residency(); document_contract(); }
        std::cout << "PASS facility colour\n"; return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
