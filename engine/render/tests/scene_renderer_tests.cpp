#include <mediaserver/render/environment_cache.hpp>
#include <mediaserver/render/scene_renderer.hpp>

#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/geometry.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// The GPU half of the scene proof. The scene math suite already establishes,
// with no GPU at all, the load-bearing geometric property: adjacent slices
// share a bit-identical frustum plane and every slice projects a point exactly
// where the whole canvas does. What that suite cannot show is that a pipeline
// exists which turns those matrices into pixels, and that two nodes rendering
// two slices reconstruct one image rather than two nearly-aligned ones.
//
// So this suite draws. It needs a GPU and skips loudly without one, exactly as
// the rhi suite does, and it reads its own output back off an offscreen target
// so it needs no display. Its centrepiece is the seam test the implementation
// plan (§10.3) calls the highest-value test in the suite: render the whole
// canvas on one target, render each half on its own, and prove the halves are
// the whole - which is the entire claim of "one scene, many outputs" reduced to
// something a machine can check.
namespace {

namespace core = mediaserver::core;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;
namespace render = mediaserver::render;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define RENDER_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] rhi::Device open_device() {
    rhi::DeviceConfig config{};
    config.debug = false;
    auto created = rhi::Device::create(config);
    check(created.has_value(), "device creation", __LINE__);
    return std::move(created).value();
}

[[nodiscard]] render::RendererLimits test_limits() noexcept {
    // A box is 24 vertices and 36 indices; a test wants none of the production
    // caps and the memory they reserve.
    render::RendererLimits limits{};
    limits.max_instances = 64U;
    limits.max_vertices = 1U << 14U;
    limits.max_indices = 1U << 15U;
    limits.max_lights = 16U;
    limits.max_inline_effect_ops = 128U;
    limits.max_inline_curve_luts = 8U;
    limits.max_inline_effect_parameter_vectors =
        128U * 8U + 8U * scene::curve_lut_entries;
    return limits;
}

// A camera looking down -Z at the origin, framed so a unit box fills the middle
// of the canvas and leaves a border of sky.
[[nodiscard]] scene::Camera front_camera() noexcept {
    scene::Camera camera{};
    camera.eye = scene::Vec3{0.0F, 0.0F, 3.0F};
    camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    camera.field_of_view = 0.125F; // 45 degrees
    camera.near_plane = 0.1F;
    camera.far_plane = 100.0F;
    return camera;
}

[[nodiscard]] scene::Environment test_environment() noexcept {
    scene::Environment environment{};
    environment.sky_color = scene::Vec3{0.05F, 0.07F, 0.10F};
    environment.ground_color = scene::Vec3{0.02F, 0.02F, 0.02F};
    environment.ambient = 1.0F;
    return environment;
}

// An empty scene: a canvas, a camera, an environment, and nothing to draw. The
// reference for what "background" is - every pixel of this is the clear.
[[nodiscard]] scene::Scene empty_scene(scene::Canvas canvas) {
    scene::Scene scn{};
    scn.canvas = canvas;
    scn.camera = front_camera();
    scn.environment = test_environment();
    return scn;
}

// One lit box on the origin, small enough to leave a sky border. This is the
// "does it draw at all" scene.
[[nodiscard]] scene::Scene box_scene(scene::Canvas canvas) {
    scene::Scene scn = empty_scene(canvas);

    scene::MeshInstance instance{};
    instance.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    instance.world = scene::identity();
    instance.normal = scene::normal_basis(instance.world);
    instance.material.base_color = scene::Vec3{0.80F, 0.30F, 0.20F};
    instance.material.roughness = 0.5F;
    instance.material.metallic = 0.0F;
    scn.instances.push_back(instance);

    scene::DirectionalLight light{};
    light.direction = scene::Vec3{-0.3F, -0.5F, -0.8F};
    light.color = scene::Vec3{1.0F, 1.0F, 1.0F};
    light.intensity = 3.0F;
    scn.directional_lights.push_back(light);

    return scn;
}

// A box scaled to overfill the frame, lit by an off-centre point light. The
// point of it is the seam test: there is no silhouette against the sky in the
// interior of the frame, so every pixel the two halves share is smoothly shaded
// surface, and the point light's falloff writes a real gradient across it so the
// agreement being checked is agreement about something that varies rather than a
// flat colour two wrong renders could both produce.
[[nodiscard]] scene::Scene wall_scene(scene::Canvas canvas) {
    scene::Scene scn = empty_scene(canvas);

    scene::MeshInstance instance{};
    instance.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    // Pushed back and scaled wide and tall: the front face sits at z = -2.5 and
    // spans well past the frustum edges at that depth, so it covers the frame to
    // the corners for the whole canvas and for either half of it.
    instance.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, 0.0F, -3.0F}),
        scene::scaling(scene::Vec3{12.0F, 9.0F, 1.0F}));
    instance.normal = scene::normal_basis(instance.world);
    instance.material.base_color = scene::Vec3{0.70F, 0.70F, 0.72F};
    instance.material.roughness = 0.6F;
    instance.material.metallic = 0.0F;
    scn.instances.push_back(instance);

    scene::PointLight light{};
    light.position = scene::Vec3{2.0F, 1.0F, -1.0F};
    light.color = scene::Vec3{1.0F, 0.95F, 0.85F};
    light.intensity = 8.0F;
    light.range = 20.0F;
    scn.point_lights.push_back(light);

    return scn;
}

[[nodiscard]] std::string write_material_model() {
    const auto directory =
        std::filesystem::temp_directory_path() / "mediaserver_render_scene_tests";
    std::filesystem::create_directories(directory);
    {
        std::ofstream material{directory / "multipart.mtl", std::ios::trunc};
        material
            << "newmtl Red\nKd 0.9 0.02 0.02\nPr 0.65\nPm 0.0\n"
               "Ke 0.45 0.0 0.0\n"
               "newmtl Green\nKd 0.02 0.9 0.02\nPr 0.35\nPm 0.0\n"
               "Ke 0.0 0.45 0.0\n";
        material.close();
        check(static_cast<bool>(material), "write model material", __LINE__);
    }
    const auto path = directory / "multipart.obj";
    {
        std::ofstream model{path, std::ios::trunc};
        model
            << "mtllib multipart.mtl\n"
               "v -1.15 -0.6 0\nv -0.45 -0.6 0\nv -0.45 0.6 0\nv -1.15 0.6 0\n"
               "v -0.35 -0.6 0\nv 0.35 -0.6 0\nv 0.35 0.6 0\nv -0.35 0.6 0\n"
               "v 0.45 -0.6 0\nv 1.15 -0.6 0\nv 1.15 0.6 0\nv 0.45 0.6 0\n"
               // The first quad is deliberately unassigned and must use the
               // authored fallback; the two assigned runs must not use it.
               "f 5 6 7\nf 5 7 8\n"
               "usemtl Red\nf 1 2 3\nf 1 3 4\n"
               "usemtl Green\nf 9 10 11\nf 9 11 12\n";
        model.close();
        check(static_cast<bool>(model), "write multipart model", __LINE__);
    }
    return path.string();
}

void append_fixture_f32(std::vector<core::u8>& bytes, float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    bytes.push_back(static_cast<core::u8>(bits & 0xffU));
    bytes.push_back(static_cast<core::u8>((bits >> 8U) & 0xffU));
    bytes.push_back(static_cast<core::u8>((bits >> 16U) & 0xffU));
    bytes.push_back(static_cast<core::u8>((bits >> 24U) & 0xffU));
}

void append_fixture_u16(
    std::vector<core::u8>& bytes,
    std::uint16_t value) {
    bytes.push_back(static_cast<core::u8>(value & 0xffU));
    bytes.push_back(static_cast<core::u8>((value >> 8U) & 0xffU));
}

void write_fixture_tga(
    const std::filesystem::path& path,
    const std::array<core::u8, 4U>& rgba) {
    std::array<core::u8, 22U> bytes{};
    bytes[2U] = 2U;
    bytes[12U] = 1U;
    bytes[14U] = 1U;
    bytes[16U] = 32U;
    bytes[17U] = 0x28U;
    bytes[18U] = rgba[2U];
    bytes[19U] = rgba[1U];
    bytes[20U] = rgba[0U];
    bytes[21U] = rgba[3U];
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    out.close();
    check(static_cast<bool>(out), "write material texture", __LINE__);
}

[[nodiscard]] std::string write_textured_model(
    const std::string& stem,
    const std::array<core::u8, 4U>& base_map,
    const std::array<core::u8, 4U>& metallic_roughness_map,
    const std::array<core::u8, 4U>& emissive_map,
    scene::Vec3 base_factor,
    float roughness_factor,
    float metallic_factor,
    scene::Vec3 emissive_factor,
    const std::array<core::u8, 4U>& normal_map =
        std::array<core::u8, 4U>{128U, 128U, 255U, 255U},
    const std::array<core::u8, 4U>& occlusion_map =
        std::array<core::u8, 4U>{255U, 255U, 255U, 255U},
    float normal_scale = 0.0F,
    float occlusion_strength = 0.0F) {
    const auto directory =
        std::filesystem::temp_directory_path() / "mediaserver_render_scene_tests";
    std::filesystem::create_directories(directory);
    const auto buffer_name = stem + ".bin";
    const auto base_name = stem + "_base.tga";
    const auto mr_name = stem + "_mr.tga";
    const auto emissive_name = stem + "_emissive.tga";
    const auto normal_name = stem + "_normal.tga";
    const auto occlusion_name = stem + "_occlusion.tga";

    std::vector<core::u8> bytes;
    for (float value : std::array<float, 12U>{
             -1.0F, -1.0F, 0.0F,
              1.0F, -1.0F, 0.0F,
              1.0F,  1.0F, 0.0F,
             -1.0F,  1.0F, 0.0F}) {
        append_fixture_f32(bytes, value);
    }
    for (std::size_t vertex = 0U; vertex < 4U; ++vertex) {
        append_fixture_f32(bytes, 0.0F);
        append_fixture_f32(bytes, 0.0F);
        append_fixture_f32(bytes, 1.0F);
    }
    for (float value : std::array<float, 8U>{
             0.0F, 1.0F, 1.0F, 1.0F,
             1.0F, 0.0F, 0.0F, 0.0F}) {
        append_fixture_f32(bytes, value);
    }
    for (std::uint16_t index :
         std::array<std::uint16_t, 6U>{0U, 1U, 2U, 0U, 2U, 3U}) {
        append_fixture_u16(bytes, index);
    }
    check(bytes.size() == 140U, "textured fixture geometry size", __LINE__);
    {
        std::ofstream out{
            directory / buffer_name, std::ios::binary | std::ios::trunc};
        out.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        out.close();
        check(static_cast<bool>(out), "write textured model buffer", __LINE__);
    }
    write_fixture_tga(directory / base_name, base_map);
    write_fixture_tga(directory / mr_name, metallic_roughness_map);
    write_fixture_tga(directory / emissive_name, emissive_map);
    write_fixture_tga(directory / normal_name, normal_map);
    write_fixture_tga(directory / occlusion_name, occlusion_map);

    const auto path = directory / (stem + ".gltf");
    std::ofstream out{path, std::ios::trunc};
    out << "{\n"
           "  \"asset\": {\"version\": \"2.0\"},\n"
           "  \"buffers\": [{\"uri\": \"" << buffer_name
        << "\", \"byteLength\": 140}],\n"
           "  \"bufferViews\": [\n"
           "    {\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 48},\n"
           "    {\"buffer\": 0, \"byteOffset\": 48, \"byteLength\": 48},\n"
           "    {\"buffer\": 0, \"byteOffset\": 96, \"byteLength\": 32},\n"
           "    {\"buffer\": 0, \"byteOffset\": 128, \"byteLength\": 12}\n"
           "  ],\n"
           "  \"accessors\": [\n"
           "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC3\"},\n"
           "    {\"bufferView\": 1, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC3\"},\n"
           "    {\"bufferView\": 2, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC2\"},\n"
           "    {\"bufferView\": 3, \"componentType\": 5123, \"count\": 6, \"type\": \"SCALAR\"}\n"
           "  ],\n"
           "  \"images\": [{\"uri\": \"" << base_name
        << "\"}, {\"uri\": \"" << mr_name
        << "\"}, {\"uri\": \"" << emissive_name
        << "\"}, {\"uri\": \"" << normal_name
        << "\"}, {\"uri\": \"" << occlusion_name << "\"}],\n"
           "  \"textures\": [{\"source\": 0}, {\"source\": 1}, {\"source\": 2},\n"
           "               {\"source\": 3}, {\"source\": 4}],\n"
           "  \"materials\": [{\n"
           "    \"pbrMetallicRoughness\": {\n"
           "      \"baseColorFactor\": [" << base_factor.x << ", "
        << base_factor.y << ", " << base_factor.z << ", 1.0],\n"
           "      \"baseColorTexture\": {\"index\": 0},\n"
           "      \"roughnessFactor\": " << roughness_factor << ",\n"
           "      \"metallicFactor\": " << metallic_factor << ",\n"
           "      \"metallicRoughnessTexture\": {\"index\": 1}\n"
           "    },\n"
           "    \"emissiveFactor\": [" << emissive_factor.x << ", "
        << emissive_factor.y << ", " << emissive_factor.z << "],\n"
           "    \"emissiveTexture\": {\"index\": 2},\n"
           "    \"normalTexture\": {\"index\": 3, \"scale\": "
        << normal_scale << "},\n"
           "    \"occlusionTexture\": {\"index\": 4, \"strength\": "
        << occlusion_strength << "}\n"
           "  }],\n"
           "  \"meshes\": [{\"primitives\": [{\n"
           "    \"attributes\": {\"POSITION\": 0, \"NORMAL\": 1, \"TEXCOORD_0\": 2},\n"
           "    \"indices\": 3, \"material\": 0\n"
           "  }]}],\n"
           "  \"nodes\": [{\"mesh\": 0}],\n"
           "  \"scenes\": [{\"nodes\": [0]}],\n"
           "  \"scene\": 0\n"
           "}\n";
    out.close();
    check(static_cast<bool>(out), "write textured gltf", __LINE__);
    return path.string();
}

[[nodiscard]] scene::Scene textured_model_scene(
    scene::Canvas canvas,
    const std::string& source,
    scene::Vec3 ambient,
    float ambient_strength,
    float directional_intensity) {
    auto scn = empty_scene(canvas);
    scn.environment.sky_color = ambient;
    scn.environment.ground_color = ambient;
    scn.environment.ambient = ambient_strength;
    scene::ModelInstance model{};
    model.source = source;
    model.world = scene::identity();
    model.normal = scene::normal_basis(model.world);
    scn.models.push_back(model);
    if (directional_intensity > 0.0F) {
        scene::DirectionalLight light{};
        light.direction = scene::Vec3{0.0F, 0.0F, -1.0F};
        light.color = scene::Vec3{1.0F, 1.0F, 1.0F};
        light.intensity = directional_intensity;
        scn.directional_lights.push_back(light);
    }
    return scn;
}

[[nodiscard]] float decode_srgb_code(core::u8 value) noexcept {
    const float encoded = static_cast<float>(value) / 255.0F;
    return encoded <= 0.04045F
        ? encoded / 12.92F
        : std::pow((encoded + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] scene::Scene multipart_model_scene(scene::Canvas canvas) {
    auto scn = empty_scene(canvas);
    scene::ModelInstance model{};
    model.world = scene::identity();
    model.normal = scene::normal_basis(model.world);
    model.source = write_material_model();
    // Deliberately vivid authored fallback: only the unassigned centre part is
    // blue; the source-assigned side parts must remain red/green.
    model.material.base_color = scene::Vec3{0.0F, 0.0F, 1.0F};
    model.material.emissive = scene::Vec3{0.0F, 0.0F, 1.0F};
    scn.models.push_back(model);
    return scn;
}

[[nodiscard]] scene::Scene compositing_scene(scene::Canvas canvas) {
    auto scn = empty_scene(canvas);
    // Black and unlit makes emissive material the complete shader result, so
    // the fixed-function equations can be checked against read-back values.
    scn.environment.sky_color = scene::Vec3{};
    scn.environment.ground_color = scene::Vec3{};
    scn.environment.ambient = 0.0F;
    return scn;
}

void add_emissive_quad(
    scene::Scene& scn,
    scene::Vec3 emissive,
    float z,
    mediaserver::doc::NodeId source,
    scene::LayerStyle style = scene::LayerStyle{}) {
    scene::MeshInstance instance{};
    instance.mesh = scene::MeshKey{scene::Primitive::Quad, 1U};
    instance.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, 0.0F, z}),
        scene::scaling(scene::Vec3{3.0F, 3.0F, 1.0F}));
    instance.normal = scene::normal_basis(instance.world);
    instance.material.base_color = scene::Vec3{};
    instance.material.emissive = emissive;
    instance.source = source;
    instance.style = style;
    scn.instances.push_back(instance);
}

struct RenderedImage final {
    core::u32 width{};
    core::u32 height{};
    std::vector<core::u8> rgba;
    render::RenderStats stats{};
    std::vector<render::GeometrySelection> selection;
};

// Renders one slice of a scene into a fresh target of `out_width` x `out_height`
// and reads the pixels back. The record, the draw, and the download all ride on
// one command list, so the readback sees exactly what the draw produced.
[[nodiscard]] RenderedImage render_slice(
    rhi::Device& device,
    const scene::Scene& scn,
    scene::CanvasRect slice,
    core::u32 out_width,
    core::u32 out_height,
    std::optional<render::SceneRenderView> explicit_view = std::nullopt,
    render::SceneRendererMode mode = render::SceneRendererMode::BasicDisplay) {
    auto created =
        render::SceneRenderer::create(device, out_width, out_height, test_limits(), mode);
    check(created.has_value(), "renderer creation", __LINE__);
    auto renderer = std::move(created).value();

    const auto bytes = out_width * out_height
        * (mode == render::SceneRendererMode::AdvancedLinear ? 8U : 4U);
    auto readback = device.create_readback(bytes);
    check(readback.has_value(), "readback creation", __LINE__);

    // Imported models are intentionally cold/background-only. Record one
    // non-presented admission frame, wait at this CPU-test seam for work
    // already accepted by the process worker, then acquire a fresh command
    // list so its publication epoch can include the completed immutable asset.
    if (!scn.models.empty()) {
        auto warm_acquired = device.acquire_commands();
        check(warm_acquired.has_value(), "warm acquire", __LINE__);
        auto warm_commands = std::move(warm_acquired).value();
        render::SceneRenderOptions warm_options{};
        warm_options.view = explicit_view;
        check(
            renderer.render(warm_commands, scn, slice, warm_options),
            "warm render", __LINE__);
        auto warm_submitted = device.submit(std::move(warm_commands));
        check(warm_submitted.has_value(), "warm submit", __LINE__);
        check(
            std::move(warm_submitted).value().wait(),
            "warm wait", __LINE__);
        check(
            render::detail::wait_for_shared_model_cache_for_test(),
            "model decode wait", __LINE__);
    }

    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "acquire commands", __LINE__);
    auto commands = std::move(acquired).value();

    render::SceneRenderOptions options{};
    options.view = explicit_view;
    check(renderer.render(commands, scn, slice, options), "render", __LINE__);
    RenderedImage image{};
    image.width = out_width;
    image.height = out_height;
    image.stats = renderer.stats();
    image.selection.assign(renderer.geometry_selection().begin(), renderer.geometry_selection().end());

    check(commands.download(renderer.target(), readback.value()), "download", __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "submit", __LINE__);
    check(std::move(submitted).value().wait(), "wait", __LINE__);

    const auto pixels = readback.value().map();
    check(pixels.size() >= bytes, "readback map", __LINE__);
    image.rgba.assign(pixels.begin(), pixels.begin() + bytes);
    readback.value().unmap();
    return image;
}

[[nodiscard]] bool pixels_equal(
    const RenderedImage& image,
    std::size_t a,
    std::size_t b) noexcept {
    for (std::size_t channel = 0U; channel < 4U; ++channel) {
        if (image.rgba[a + channel] != image.rgba[b + channel]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] int channel_difference(int lhs, int rhs) noexcept {
    return lhs > rhs ? lhs - rhs : rhs - lhs;
}

[[nodiscard]] std::array<int, 3U> centre_rgb(
    const RenderedImage& image) noexcept {
    const auto offset =
        (static_cast<std::size_t>(image.height / 2U) * image.width
         + image.width / 2U)
        * 4U;
    return std::array<int, 3U>{
        image.rgba[offset], image.rgba[offset + 1U], image.rgba[offset + 2U]};
}

[[nodiscard]] int expected_blend(
    scene::BlendMode mode,
    int source,
    int destination,
    float opacity) noexcept {
    const float src = static_cast<float>(source) / 255.0F;
    const float dst = static_cast<float>(destination) / 255.0F;
    float result{};
    switch (mode) {
    case scene::BlendMode::Normal:
        result = src * opacity + dst * (1.0F - opacity);
        break;
    case scene::BlendMode::Add:
        result = src * opacity + dst;
        break;
    case scene::BlendMode::Multiply:
        result = dst * ((1.0F - opacity) + src * opacity);
        break;
    case scene::BlendMode::Screen:
        result = src * opacity + dst * (1.0F - src * opacity);
        break;
    }
    result = std::clamp(result, 0.0F, 1.0F);
    return static_cast<int>(std::lround(result * 255.0F));
}

// The build machine may have a GPU whose backend this build carries no bytecode
// for - a Vulkan-only device against a DXIL build. That is the renderer's own
// ShaderFormatUnsupported, and it is a skip, not a failure, exactly as a machine
// with no GPU is.
[[nodiscard]] bool renderer_usable(rhi::Device& device) {
    auto probe = render::SceneRenderer::create(device, 16U, 16U, test_limits());
    if (probe) {
        return true;
    }
    return probe.error() != render::RenderError::ShaderFormatUnsupported;
}

// A Radiance image whose two halves differ enough that a reflection of one is
// obviously not a reflection of the other.
//
// Written to a real file because that is the path an authored environment
// actually takes - through the residency cache, off a disk, on a worker - and a
// test that handed the renderer bytes directly would skip the half most likely
// to be wrong.
// Half float to float, for reading an RGBA16F target back.
[[nodiscard]] float half_to_float(core::u16 bits) noexcept {
    const core::u32 sign = static_cast<core::u32>(bits & 0x8000U) << 16U;
    const core::u32 exponent = (bits >> 10U) & 0x1FU;
    const core::u32 mantissa = bits & 0x3FFU;
    core::u32 result{};
    if (exponent == 0U) {
        if (mantissa != 0U) {
            core::u32 shifted = mantissa;
            core::u32 shift = 0U;
            while ((shifted & 0x400U) == 0U) {
                shifted <<= 1U;
                ++shift;
            }
            result = sign | ((127U - 15U - shift + 1U) << 23U)
                | ((shifted & 0x3FFU) << 13U);
        } else {
            result = sign;
        }
    } else if (exponent == 0x1FU) {
        result = sign | 0x7F80'0000U | (mantissa << 13U);
    } else {
        result = sign | ((exponent + 127U - 15U) << 23U) | (mantissa << 13U);
    }
    float value{};
    std::memcpy(&value, &result, sizeof(value));
    return value;
}

[[nodiscard]] std::filesystem::path write_environment_fixture() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path()
        / "mediaserver_test_environment.hdr";
    // 4x2 flat RGBE. The +X half is bright, the -X half is nearly black.
    const std::string header =
        "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 4\n";
    const std::array<core::u8, 32U> pixels{{
        // Row one: two bright texels then two dark ones.
        128U, 128U, 128U, 132U, 128U, 128U, 128U, 132U,
        128U, 128U, 128U, 120U, 128U, 128U, 128U, 120U,
        // Row two: the same, so the map has no vertical gradient to confuse a
        // reflection with a hemisphere.
        128U, 128U, 128U, 132U, 128U, 128U, 128U, 132U,
        128U, 128U, 128U, 120U, 128U, 128U, 128U, 120U,
    }};
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    check(static_cast<bool>(file), "environment fixture open", __LINE__);
    file.write(header.data(), static_cast<std::streamsize>(header.size()));
    file.write(reinterpret_cast<const char*>(pixels.data()),
               static_cast<std::streamsize>(pixels.size()));
    file.close();
    return path;
}

// Renders an Advanced frame, giving the environment cache the frames it needs
// to admit, decode and adopt a map.
//
// Three recordings rather than one: the first admits the source, the wait is
// the CPU-test seam standing in for however many frames a real show would take,
// and the third is the frame whose pixels are read. That sequence is the point
// - the frame path never waits, so a map arrives some frames after it is
// authored and the renderer has to keep working in between.
[[nodiscard]] RenderedImage render_advanced_environment(
    rhi::Device& device, const scene::Scene& scn, core::u32 extent,
    bool transparent_background = false) {
    auto created = render::SceneRenderer::create(
        device, extent, extent, test_limits(),
        render::SceneRendererMode::AdvancedLinear);
    check(created.has_value(), "advanced renderer creation", __LINE__);
    auto renderer = std::move(created).value();

    // The Advanced target is RGBA16F, so eight bytes a pixel. Sizing this for
    // RGBA8 makes `download` refuse, and a test that checked the refusal's
    // return value would fail rather than silently read nothing.
    const auto bytes = extent * extent * 8U;
    auto readback = device.create_readback(bytes);
    check(readback.has_value(), "readback creation", __LINE__);

    render::EnvironmentCache cache;
    for (int pass = 0; pass < 2; ++pass) {
        auto acquired = device.acquire_commands();
        check(acquired.has_value(), "warm acquire", __LINE__);
        auto commands = std::move(acquired).value();
        render::SceneRenderOptions warm_options{};
        warm_options.transparent_background = transparent_background;
        check(renderer.render(commands, scn, scene::whole(scn.canvas),
                              warm_options),
              "warm advanced render", __LINE__);
        auto submitted = device.submit(std::move(commands));
        check(submitted.has_value(), "warm submit", __LINE__);
        check(std::move(submitted).value().wait(), "warm wait", __LINE__);
        check(cache.wait_for_test(), "environment cache wait", __LINE__);
    }

    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "acquire", __LINE__);
    auto commands = std::move(acquired).value();
    render::SceneRenderOptions options{};
    options.transparent_background = transparent_background;
    check(renderer.render(commands, scn, scene::whole(scn.canvas), options),
          "advanced render", __LINE__);
    check(commands.download(renderer.target(), readback.value()), "download",
          __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "submit", __LINE__);
    check(std::move(submitted).value().wait(), "wait", __LINE__);

    RenderedImage image{};
    image.width = extent;
    image.height = extent;
    image.stats = renderer.stats();
    image.selection.assign(renderer.geometry_selection().begin(), renderer.geometry_selection().end());
    const auto pixels = readback.value().map();
    check(pixels.size() >= bytes, "readback map", __LINE__);
    image.rgba.assign(pixels.begin(), pixels.begin() + bytes);
    readback.value().unmap();
    return image;
}

[[nodiscard]] scene::Scene metallic_scene() {
    scene::Scene value{};
    value.canvas = scene::Canvas{64U, 64U};
    value.camera = front_camera();
    value.environment.sky_color = scene::Vec3{0.05F, 0.07F, 0.10F};
    value.environment.ground_color = scene::Vec3{0.02F, 0.02F, 0.02F};
    value.environment.ambient = 1.0F;
    value.renderer_settings.backend = scene::RendererBackend::Advanced;

    scene::MeshInstance metal{};
    metal.source = mediaserver::doc::NodeId{7U};
    metal.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    metal.world = scene::identity();
    // A mirror. With no environment it has nothing to reflect and reads as the
    // flat grey this whole step exists to fix.
    metal.material.base_color = scene::Vec3{1.0F, 1.0F, 1.0F};
    metal.material.metallic = 1.0F;
    metal.material.roughness = 0.0F;
    value.instances.push_back(metal);
    return value;
}

// Turning the authored rotation turns the environment, on a renderer that has
// already adopted it.
//
// One renderer across both frames, deliberately. Rotation is baked into the
// cube by the equirect pass rather than applied per frame, so a fresh renderer
// would pick up a new rotation through ordinary first-time adoption and prove
// nothing about the case an operator actually hits: dragging the dial on a
// running show. That path has to notice the authored value changed and
// re-record the chain, and it did not - the rotation was never passed to the
// prefilter at all, and the re-record test did not look at it.
void turning_the_authored_rotation_turns_a_resident_environment() {
    if (!render::SceneRenderer::shaders_available()
        || !rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    RENDER_CHECK(device.capabilities().advanced_raster());
    RENDER_CHECK(device.capabilities().compute);

    constexpr core::u32 extent = 64U;
    auto created = render::SceneRenderer::create(
        device, extent, extent, test_limits(),
        render::SceneRendererMode::AdvancedLinear);
    RENDER_CHECK(created.has_value());
    auto renderer = std::move(created).value();

    const auto bytes = extent * extent * 8U;
    auto readback = device.create_readback(bytes);
    RENDER_CHECK(readback.has_value());

    const std::filesystem::path fixture = write_environment_fixture();
    scene::Scene scn = metallic_scene();
    scn.environment.map_source = fixture.string();
    scn.environment.map_intensity = 1.0F;

    // Renders one frame at the authored rotation and reads it back. The
    // environment cache is given room to admit and decode before the first
    // capture, so the second frame differs by rotation alone.
    const auto capture = [&](scene::Turns rotation) {
        scn.environment.map_rotation = rotation;
        for (int pass = 0; pass < 2; ++pass) {
            auto acquired = device.acquire_commands();
            check(acquired.has_value(), "warm acquire", __LINE__);
            auto commands = std::move(acquired).value();
            check(renderer.render(commands, scn, scene::whole(scn.canvas)),
                  "warm render", __LINE__);
            auto submitted = device.submit(std::move(commands));
            check(submitted.has_value(), "warm submit", __LINE__);
            check(std::move(submitted).value().wait(), "warm wait", __LINE__);
            render::EnvironmentCache cache;
            check(cache.wait_for_test(), "environment cache wait", __LINE__);
        }
        auto acquired = device.acquire_commands();
        check(acquired.has_value(), "acquire", __LINE__);
        auto commands = std::move(acquired).value();
        check(renderer.render(commands, scn, scene::whole(scn.canvas)),
              "render", __LINE__);
        check(commands.download(renderer.target(), readback.value()),
              "download", __LINE__);
        auto submitted = device.submit(std::move(commands));
        check(submitted.has_value(), "submit", __LINE__);
        check(std::move(submitted).value().wait(), "wait", __LINE__);
        const auto pixels = readback.value().map();
        check(pixels.size() >= bytes, "readback map", __LINE__);
        std::vector<core::u8> image(pixels.begin(), pixels.begin() + bytes);
        readback.value().unmap();
        return image;
    };

    const std::vector<core::u8> unturned = capture(0.0F);
    // A quarter turn. The fixture's bright half spans half the sphere, so this
    // moves it decisively rather than by an amount a filter could swallow.
    const std::vector<core::u8> turned = capture(0.25F);
    // Back where it started: the dial has to work in both directions, and a
    // renderer that only ever re-recorded on an increasing value would pass a
    // one-way check.
    const std::vector<core::u8> returned = capture(0.0F);

    std::error_code removed;
    std::filesystem::remove(fixture, removed);

    std::size_t differing = 0U;
    for (std::size_t index = 0U; index + 8U <= unturned.size(); index += 8U) {
        if (std::memcmp(unturned.data() + index, turned.data() + index, 6U)
            != 0) {
            ++differing;
        }
    }
    RENDER_CHECK(differing > 0U);
    RENDER_CHECK(unturned == returned);
}

// An authored environment changes what a metal reflects, and no environment
// changes nothing at all.
//
// Both halves matter. The first is the entire point of the step: without it
// there is no specular ambient term and every chrome surface in a shot reads as
// flat grey. The second is the promise that this costs nothing when unused -
// asserted byte for byte, because "looks the same" is exactly the claim that
// hides a one-code-value shift across an entire wall.
void an_authored_environment_lights_metal_and_absence_changes_nothing() {
    // D3D12 explicitly. The default backend reports no advanced raster, so a
    // test that opened it would take the skip below and pass having rendered
    // nothing - which is exactly what this one did until the skip was checked.
    if (!render::SceneRenderer::shaders_available()
        || !rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    RENDER_CHECK(device.capabilities().advanced_raster());
    RENDER_CHECK(device.capabilities().compute);

    const scene::Scene without = metallic_scene();
    const RenderedImage plain = render_advanced_environment(device, without, 64U);

    // The same scene rendered again with nothing authored is identical. This
    // is the fallback promise, and it is checked before the interesting case so
    // a failure here is not mistaken for a lighting difference.
    const RenderedImage plain_again =
        render_advanced_environment(device, without, 64U);
    RENDER_CHECK(plain.rgba == plain_again.rgba);

    const std::filesystem::path fixture = write_environment_fixture();
    scene::Scene with = without;
    with.environment.map_source = fixture.string();
    with.environment.map_intensity = 1.0F;
    const RenderedImage lit = render_advanced_environment(device, with, 64U);

    std::error_code removed;
    std::filesystem::remove(fixture, removed);

    // Something changed. Eight bytes a pixel: the Advanced target is RGBA16F.
    std::size_t differing = 0U;
    for (std::size_t index = 0U; index + 8U <= plain.rgba.size();
         index += 8U) {
        if (std::memcmp(plain.rgba.data() + index, lit.rgba.data() + index,
                        6U) != 0) {
            ++differing;
        }
    }
    RENDER_CHECK(differing > 0U);

    // The background changed too, which is the sky pass drawing the
    // environment instead of the flat clear. On a volume that is not
    // decoration: what the wall shows *is* the light hitting the subject, so a
    // flat sky behind a scene lit by an HDRI is a contradiction the camera
    // sees.
    //
    // The corner is outside the object at this camera, so it is background by
    // construction rather than by luck.
    RENDER_CHECK(std::memcmp(plain.rgba.data(), lit.rgba.data(), 6U) != 0);

    // And the frame got brighter, which is what an environment does. A darker
    // result would mean a term was subtracting - or, as it did once, that the
    // sky was being tone mapped into a linear target and coming back nearly
    // black.
    //
    // Summed as radiance, not as bytes: these are half floats, and summing
    // their bytes compares exponent patterns rather than light.
    const auto luminance = [](const std::vector<core::u8>& image) {
        double total = 0.0;
        for (std::size_t index = 0U; index + 8U <= image.size(); index += 8U) {
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                core::u16 bits{};
                std::memcpy(&bits,
                            image.data() + index + (channel * 2U),
                            sizeof(bits));
                total += static_cast<double>(half_to_float(bits));
            }
        }
        return total;
    };
    RENDER_CHECK(luminance(lit.rgba) > luminance(plain.rgba));
}

// An isolated compositor layer keeps its transparent background, environment
// or not.
//
// This is the case the test above cannot see. A layer renderer used by the
// compositor clears to transparent black so uncovered pixels stay uncovered;
// filling one with a sky makes every layer opaque and the composite shows only
// whichever was drawn last. The editor renders its Program view exactly this
// way, so the first version of the sky pass turned that view black while a
// direct render of the same scene looked correct.
void an_isolated_layer_keeps_its_transparent_background() {
    if (!render::SceneRenderer::shaders_available()
        || !rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    if (!device.capabilities().advanced_raster()
        || !device.capabilities().compute) {
        return;
    }

    const std::filesystem::path fixture = write_environment_fixture();
    scene::Scene lit = metallic_scene();
    lit.environment.map_source = fixture.string();
    lit.environment.map_intensity = 1.0F;

    const RenderedImage layer =
        render_advanced_environment(device, lit, 64U, true);
    std::error_code removed;
    std::filesystem::remove(fixture, removed);

    // The corner is background at this camera. Alpha there must be zero: the
    // compositor composites this layer over whatever is behind it, and an
    // opaque background would hide all of it.
    core::u16 alpha_bits{};
    std::memcpy(&alpha_bits, layer.rgba.data() + 6U, sizeof(alpha_bits));
    RENDER_CHECK(half_to_float(alpha_bits) == 0.0F);

    // And nothing was painted there either - a premultiplied target with zero
    // alpha still has to carry zero colour, or the composite adds light that
    // belongs to no surface.
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        core::u16 bits{};
        std::memcpy(&bits, layer.rgba.data() + (channel * 2U), sizeof(bits));
        RENDER_CHECK(half_to_float(bits) == 0.0F);
    }
}

void an_empty_scene_is_the_clear_colour_everywhere() {
    auto device = open_device();
    constexpr scene::Canvas canvas{96U, 64U};
    const auto image =
        render_slice(device, empty_scene(canvas), scene::whole(canvas), 96U, 64U);

    RENDER_CHECK(image.stats.batches == 0U);
    RENDER_CHECK(image.stats.instances == 0U);
    RENDER_CHECK(image.stats.triangles == 0U);

    // Uniform: nothing was drawn, so every pixel is the sky clear. This also
    // pins down what "background" means for the next test.
    for (std::size_t pixel = 4U; pixel < image.rgba.size(); pixel += 4U) {
        RENDER_CHECK(pixels_equal(image, 0U, pixel));
    }
    // Opaque, and not black: an all-zero target would also be uniform.
    RENDER_CHECK(image.rgba[3U] == 255U);
    RENDER_CHECK(image.rgba[0U] + image.rgba[1U] + image.rgba[2U] > 0U);
}

void a_lit_object_covers_part_of_the_frame_and_leaves_sky() {
    auto device = open_device();
    constexpr scene::Canvas canvas{96U, 64U};

    // The clear colour, taken from the corner where the box cannot reach.
    const auto background =
        render_slice(device, empty_scene(canvas), scene::whole(canvas), 96U, 64U);
    const std::array<core::u8, 4U> sky{
        background.rgba[0U], background.rgba[1U],
        background.rgba[2U], background.rgba[3U]};

    const auto image =
        render_slice(device, box_scene(canvas), scene::whole(canvas), 96U, 64U);
    RENDER_CHECK(image.stats.batches == 1U);
    RENDER_CHECK(image.stats.instances == 1U);
    RENDER_CHECK(image.stats.triangles > 0U);
    RENDER_CHECK(image.stats.dropped_instances == 0U);
    RENDER_CHECK(image.stats.dropped_lights == 0U);

    std::size_t drawn = 0U;
    std::size_t background_pixels = 0U;
    const auto total = image.rgba.size() / 4U;
    for (std::size_t pixel = 0U; pixel < image.rgba.size(); pixel += 4U) {
        int distance = 0;
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            distance += channel_difference(
                image.rgba[pixel + channel], sky[channel]);
        }
        if (distance <= 3) {
            ++background_pixels;
        } else {
            ++drawn;
        }
    }

    // Both are present: an object that filled the frame, or one that vanished,
    // would each pass a weaker check. The box sits in the middle of the frame,
    // so it is a real fraction of it and so is the sky around it.
    RENDER_CHECK(drawn > total / 20U);
    RENDER_CHECK(background_pixels > total / 20U);
}

void the_halves_reconstruct_the_whole() {
    auto device = open_device();
    // Even width so the two halves are the same size and their shared seam is a
    // pixel boundary, not a pixel.
    constexpr scene::Canvas canvas{128U, 96U};
    constexpr core::u32 half = canvas.width / 2U;

    const auto scn = wall_scene(canvas);
    const auto whole =
        render_slice(device, scn, scene::whole(canvas), canvas.width, canvas.height);
    const auto left = render_slice(
        device, scn, scene::CanvasRect{0U, 0U, half, canvas.height}, half,
        canvas.height);
    const auto right = render_slice(
        device, scn, scene::CanvasRect{half, 0U, half, canvas.height}, half,
        canvas.height);

    // The scene is meant to vary across the frame; if it did not, agreement
    // would prove nothing. Measure the spread of the green channel across the
    // whole image and require it to be substantial.
    int low = 255;
    int high = 0;
    for (std::size_t pixel = 0U; pixel < whole.rgba.size(); pixel += 4U) {
        const int value = whole.rgba[pixel + 1U];
        low = value < low ? value : low;
        high = value > high ? value : high;
    }
    RENDER_CHECK(high - low > 40);

    // Stitch the halves and compare to the whole. GPU float rasterisation is
    // not bit-exact across two different-but-equal projections, so this is a
    // tolerance, not an equality - but a genuinely wrong slice (the whole canvas
    // squashed into half, say) would diverge across the entire gradient, not by
    // a few least-significant bits. The interior-surface scene keeps silhouette
    // pixels out of the comparison, so the tolerance can stay tight.
    long total_difference = 0;
    std::size_t worst = 0U;
    const auto sample = [](const RenderedImage& image, core::u32 x, core::u32 y) {
        return static_cast<std::size_t>(y) * image.width * 4U
            + static_cast<std::size_t>(x) * 4U;
    };
    for (core::u32 y = 0U; y < canvas.height; ++y) {
        for (core::u32 x = 0U; x < canvas.width; ++x) {
            const auto whole_index = sample(whole, x, y);
            const auto& part = x < half ? left : right;
            const auto part_index = sample(part, x < half ? x : x - half, y);
            int pixel_difference = 0;
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                pixel_difference += channel_difference(
                    whole.rgba[whole_index + channel],
                    part.rgba[part_index + channel]);
            }
            total_difference += pixel_difference;
            worst = pixel_difference > static_cast<int>(worst)
                ? static_cast<std::size_t>(pixel_difference)
                : worst;
        }
    }

    const auto samples =
        static_cast<double>(canvas.width) * static_cast<double>(canvas.height);
    const auto mean = static_cast<double>(total_difference) / samples;
    // Averaged over three channels this is well under one code value per
    // channel. A squashed or misplaced slice would be tens.
    RENDER_CHECK(mean < 3.0);
    // No single interior pixel is wildly off either, which a torn seam would be.
    RENDER_CHECK(worst < 48U);
}

void imported_material_parts_draw_as_separate_pbr_batches() {
    auto device = open_device();
    constexpr scene::Canvas canvas{128U, 80U};
    const auto image = render_slice(
        device,
        multipart_model_scene(canvas),
        scene::whole(canvas),
        canvas.width,
        canvas.height);

    RENDER_CHECK(image.stats.batches == 3U);
    RENDER_CHECK(image.stats.instances == 3U);
    RENDER_CHECK(image.stats.triangles == 6U);
    RENDER_CHECK(image.stats.meshes_resident == 1U);
    RENDER_CHECK(image.stats.dropped_instances == 0U);

    std::size_t red_pixels = 0U;
    std::size_t green_pixels = 0U;
    std::size_t blue_pixels = 0U;
    for (std::size_t pixel = 0U; pixel < image.rgba.size(); pixel += 4U) {
        const int red = image.rgba[pixel];
        const int green = image.rgba[pixel + 1U];
        const int blue = image.rgba[pixel + 2U];
        if (red > green + 40 && red > blue + 40) {
            ++red_pixels;
        }
        if (green > red + 40 && green > blue + 40) {
            ++green_pixels;
        }
        if (blue > red + 40 && blue > green + 40) {
            ++blue_pixels;
        }
    }
    // Each half-quad covers thousands of pixels. A small lower bound keeps the
    // assertion insensitive to backend edge rasterisation while proving both
    // imported materials reached the PBR instance records.
    RENDER_CHECK(red_pixels > 100U);
    RENDER_CHECK(green_pixels > 100U);
    RENDER_CHECK(blue_pixels > 100U);
}

void a_gpu_model_integration_refusal_is_sticky_for_its_generation() {
    auto device = open_device();
    constexpr scene::Canvas canvas{32U, 32U};
    constexpr std::array<core::u8, 4U> white{255U, 255U, 255U, 255U};
    const std::string source = write_textured_model(
        "sticky_gpu_integration_refusal", white, white, white,
        scene::Vec3{1.0F, 1.0F, 1.0F}, 1.0F, 0.0F, scene::Vec3{});
    const scene::Scene scn = textured_model_scene(
        canvas, source, scene::Vec3{}, 0.0F, 0.0F);
    auto limits = test_limits();
    limits.max_vertices = 1U;
    auto created = render::SceneRenderer::create(
        device, canvas.width, canvas.height, limits);
    RENDER_CHECK(created.has_value());
    auto renderer = std::move(created).value();

    const auto record = [&] {
        auto acquired = device.acquire_commands();
        RENDER_CHECK(acquired.has_value());
        auto commands = std::move(acquired).value();
        RENDER_CHECK(renderer.render(commands, scn, scene::whole(canvas)));
        const render::RenderStats stats = renderer.stats();
        auto submitted = device.submit(std::move(commands));
        RENDER_CHECK(submitted.has_value());
        RENDER_CHECK(std::move(submitted).value().wait());
        return stats;
    };

    static_cast<void>(record());
    RENDER_CHECK(render::detail::wait_for_shared_model_cache_for_test());
    const render::RenderStats first = record();
    RENDER_CHECK(first.model_integration_attempts == 1U);
    RENDER_CHECK(first.model_integration_refusals == 1U);
    const render::RenderStats repeated = record();
    RENDER_CHECK(repeated.model_integration_attempts == 0U);
    RENDER_CHECK(repeated.model_integration_refusals == 1U);
}

void canceled_recordings_reuse_and_eventually_commit_one_model_candidate() {
    auto device = open_device();
    constexpr scene::Canvas canvas{32U, 32U};
    constexpr std::array<core::u8, 4U> white{255U, 255U, 255U, 255U};
    const std::string source = write_textured_model(
        "canceled_model_candidate", white, white, white,
        scene::Vec3{1.0F, 1.0F, 1.0F}, 1.0F, 0.0F, scene::Vec3{});
    const scene::Scene scn = textured_model_scene(
        canvas, source, scene::Vec3{1.0F, 1.0F, 1.0F}, 1.0F, 0.0F);
    auto created = render::SceneRenderer::create(
        device, canvas.width, canvas.height, test_limits());
    RENDER_CHECK(created.has_value());
    auto renderer = std::move(created).value();

    // Admission also records the fallback texture, then deliberately cancels
    // it. The next recording must not believe that upload was committed.
    {
        auto acquired = device.acquire_commands();
        RENDER_CHECK(acquired.has_value());
        auto commands = std::move(acquired).value();
        RENDER_CHECK(renderer.render(commands, scn, scene::whole(canvas)));
    }
    RENDER_CHECK(render::detail::wait_for_shared_model_cache_for_test());

    core::u64 candidate_texture_bytes{};
    for (core::u32 canceled = 0U; canceled < 3U; ++canceled) {
        auto acquired = device.acquire_commands();
        RENDER_CHECK(acquired.has_value());
        auto commands = std::move(acquired).value();
        RENDER_CHECK(renderer.render(commands, scn, scene::whole(canvas)));
        const auto stats = renderer.stats();
        RENDER_CHECK(stats.model_integration_attempts == 1U);
        RENDER_CHECK(stats.model_integrations == 1U);
        RENDER_CHECK(stats.geometry_uploaded);
        RENDER_CHECK(stats.model_texture_bytes != 0U);
        if (canceled == 0U) {
            candidate_texture_bytes = stats.model_texture_bytes;
        } else {
            // Cancellation re-records into the same bounded candidate; it
            // neither creates another texture generation nor appends geometry.
            RENDER_CHECK(
                stats.model_texture_bytes == candidate_texture_bytes);
        }
    }

    auto readback = device.create_readback(canvas.width * canvas.height * 4U);
    RENDER_CHECK(readback.has_value());
    auto acquired = device.acquire_commands();
    RENDER_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();
    RENDER_CHECK(renderer.render(commands, scn, scene::whole(canvas)));
    RENDER_CHECK(
        renderer.stats().model_texture_bytes == candidate_texture_bytes);
    RENDER_CHECK(commands.download(renderer.target(), readback.value()));
    auto submitted = device.submit(std::move(commands));
    RENDER_CHECK(submitted.has_value());
    RENDER_CHECK(std::move(submitted).value().wait());
    const auto pixels = readback.value().map();
    const std::size_t centre =
        (static_cast<std::size_t>(canvas.height / 2U) * canvas.width
         + canvas.width / 2U) * 4U;
    RENDER_CHECK(pixels.size() > centre + 3U);
    RENDER_CHECK(pixels[centre] > 180U);
    RENDER_CHECK(pixels[centre + 1U] > 180U);
    RENDER_CHECK(pixels[centre + 2U] > 180U);
    readback.value().unmap();

    // Reconcile Submitted on a new recording. The decoded generation is now
    // resident and no texture/geometry publication repeats on a frame timer.
    auto verify = device.acquire_commands();
    RENDER_CHECK(verify.has_value());
    auto verify_commands = std::move(verify).value();
    RENDER_CHECK(renderer.render(
        verify_commands, scn, scene::whole(canvas)));
    RENDER_CHECK(renderer.stats().model_integration_attempts == 0U);
    RENDER_CHECK(!renderer.stats().geometry_uploaded);
    auto verify_submitted = device.submit(std::move(verify_commands));
    RENDER_CHECK(verify_submitted.has_value());
    RENDER_CHECK(std::move(verify_submitted).value().wait());
}

void imported_texture_roles_reach_pixels_with_their_correct_encoding() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};
    constexpr std::array<core::u8, 4U> white{255U, 255U, 255U, 255U};
    constexpr std::array<core::u8, 4U> half{128U, 128U, 128U, 255U};
    constexpr std::array<core::u8, 4U> rough_half{0U, 128U, 0U, 255U};
    constexpr std::array<core::u8, 4U> rough_one{0U, 255U, 0U, 255U};
    constexpr std::array<core::u8, 4U> metal_half{0U, 255U, 128U, 255U};
    constexpr std::array<core::u8, 4U> metal_one{0U, 255U, 255U, 255U};
    constexpr scene::Vec3 zero{};
    constexpr scene::Vec3 one{1.0F, 1.0F, 1.0F};
    const float half_linear = 128.0F / 255.0F;
    const float half_srgb_decoded = decode_srgb_code(128U);

    const auto render_red = [&](const std::string& source,
                                scene::Vec3 ambient,
                                float ambient_strength,
                                float light_intensity) {
        const auto scn = textured_model_scene(
            canvas, source, ambient, ambient_strength, light_intensity);
        return centre_rgb(render_slice(
            device, scn, scene::whole(canvas), canvas.width, canvas.height))[0U];
    };
    const auto check_matches_linear_not_encoded = [](
        int actual,
        int linear_reference,
        int encoded_reference) {
        RENDER_CHECK(channel_difference(actual, linear_reference) <= 4);
        RENDER_CHECK(channel_difference(actual, encoded_reference) > 15);
    };

    // Base colour is authored sRGB. A half-code texture must match a white map
    // multiplied by the decoded linear factor, not by a 0.5 linear factor.
    const int base_actual = render_red(
        write_textured_model(
            "pbr_base_actual", half, rough_one, white,
            one, 1.0F, 0.0F, zero),
        one, 1.0F, 0.0F);
    const int base_linear_reference = render_red(
        write_textured_model(
            "pbr_base_linear_reference", white, rough_one, white,
            scene::Vec3{half_srgb_decoded, half_srgb_decoded, half_srgb_decoded},
            1.0F, 0.0F, zero),
        one, 1.0F, 0.0F);
    const int base_encoded_reference = render_red(
        write_textured_model(
            "pbr_base_encoded_reference", white, rough_one, white,
            scene::Vec3{half_linear, half_linear, half_linear},
            1.0F, 0.0F, zero),
        one, 1.0F, 0.0F);
    check_matches_linear_not_encoded(
        base_actual, base_linear_reference, base_encoded_reference);

    // Emissive uses the same colour encoding, independently of base colour or
    // lighting, and therefore must make the same decoded-vs-encoded choice.
    const int emissive_actual = render_red(
        write_textured_model(
            "pbr_emissive_actual", white, rough_one, half,
            zero, 1.0F, 0.0F, one),
        zero, 0.0F, 0.0F);
    const int emissive_linear_reference = render_red(
        write_textured_model(
            "pbr_emissive_linear_reference", white, rough_one, white,
            zero, 1.0F, 0.0F,
            scene::Vec3{half_srgb_decoded, half_srgb_decoded, half_srgb_decoded}),
        zero, 0.0F, 0.0F);
    const int emissive_encoded_reference = render_red(
        write_textured_model(
            "pbr_emissive_encoded_reference", white, rough_one, white,
            zero, 1.0F, 0.0F,
            scene::Vec3{half_linear, half_linear, half_linear}),
        zero, 0.0F, 0.0F);
    check_matches_linear_not_encoded(
        emissive_actual,
        emissive_linear_reference,
        emissive_encoded_reference);

    // Packed metallic-roughness is data, not colour. Its G and B half-code
    // values each match a 0.5 factor behind a full-code map, while an sRGB
    // decoded factor produces a visibly different specular response.
    const int roughness_actual = render_red(
        write_textured_model(
            "pbr_roughness_actual", white, rough_half, white,
            one, 1.0F, 0.0F, zero),
        zero, 0.0F, 0.2F);
    const int roughness_linear_reference = render_red(
        write_textured_model(
            "pbr_roughness_linear_reference", white, rough_one, white,
            one, half_linear, 0.0F, zero),
        zero, 0.0F, 0.2F);
    const int roughness_srgb_reference = render_red(
        write_textured_model(
            "pbr_roughness_srgb_reference", white, rough_one, white,
            one, half_srgb_decoded, 0.0F, zero),
        zero, 0.0F, 0.2F);
    check_matches_linear_not_encoded(
        roughness_actual,
        roughness_linear_reference,
        roughness_srgb_reference);

    const int metallic_actual = render_red(
        write_textured_model(
            "pbr_metallic_actual", white, metal_half, white,
            one, 1.0F, 1.0F, zero),
        zero, 0.0F, 1.0F);
    const int metallic_linear_reference = render_red(
        write_textured_model(
            "pbr_metallic_linear_reference", white, metal_one, white,
            one, 1.0F, half_linear, zero),
        zero, 0.0F, 1.0F);
    const int metallic_srgb_reference = render_red(
        write_textured_model(
            "pbr_metallic_srgb_reference", white, metal_one, white,
            one, 1.0F, half_srgb_decoded, zero),
        zero, 0.0F, 1.0F);
    check_matches_linear_not_encoded(
        metallic_actual,
        metallic_linear_reference,
        metallic_srgb_reference);

    // A tangent-space +X normal receives a +X light that the geometric +Z
    // normal cannot. The fixture omits TANGENT, so this also exercises the
    // load-time default MikkTSpace basis through the real GPU shader.
    constexpr std::array<core::u8, 4U> tangent_x{
        255U, 128U, 128U, 255U};
    const auto render_tangent_red = [&](const std::string& source) {
        auto normal_scene = textured_model_scene(
            canvas, source, zero, 0.0F, 0.0F);
        scene::DirectionalLight tangent_light{};
        tangent_light.direction = scene::Vec3{-1.0F, 0.0F, 0.0F};
        tangent_light.color = one;
        tangent_light.intensity = 1.0F;
        normal_scene.directional_lights.push_back(tangent_light);
        return centre_rgb(render_slice(
            device, normal_scene, scene::whole(canvas), canvas.width,
            canvas.height))[0U];
    };
    const int normal_disabled = render_tangent_red(write_textured_model(
        "pbr_normal_disabled", white, rough_one, white,
        one, 1.0F, 0.0F, zero, tangent_x, white, 0.0F, 0.0F));
    const int normal_enabled = render_tangent_red(write_textured_model(
        "pbr_normal_enabled", white, rough_one, white,
        one, 1.0F, 0.0F, zero, tangent_x, white, 1.0F, 0.0F));
    RENDER_CHECK(normal_enabled > normal_disabled + 40);

    // glTF occlusion affects indirect ambient only. A black R channel at full
    // strength must darken the same otherwise-white ambient surface, while a
    // zero strength leaves it unchanged.
    constexpr std::array<core::u8, 4U> black_occlusion{0U, 0U, 0U, 255U};
    const int occlusion_disabled = render_red(
        write_textured_model(
            "pbr_occlusion_disabled", white, rough_one, white,
            one, 1.0F, 0.0F, zero,
            std::array<core::u8, 4U>{128U, 128U, 255U, 255U},
            black_occlusion, 0.0F, 0.0F),
        one, 1.0F, 0.0F);
    const int occlusion_enabled = render_red(
        write_textured_model(
            "pbr_occlusion_enabled", white, rough_one, white,
            one, 1.0F, 0.0F, zero,
            std::array<core::u8, 4U>{128U, 128U, 255U, 255U},
            black_occlusion, 0.0F, 1.0F),
        one, 1.0F, 0.0F);
    RENDER_CHECK(occlusion_disabled > occlusion_enabled + 40);
}

void every_layer_blend_mode_uses_its_real_equation() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};
    constexpr scene::Vec3 destination_emissive{0.06F, 0.16F, 0.035F};
    constexpr scene::Vec3 source_emissive{0.20F, 0.04F, 0.11F};
    constexpr float opacity = 0.4F;

    auto destination_scene = compositing_scene(canvas);
    add_emissive_quad(
        destination_scene,
        destination_emissive,
        0.0F,
        mediaserver::doc::NodeId{1U});
    const auto destination = centre_rgb(render_slice(
        device, destination_scene, scene::whole(canvas), canvas.width,
        canvas.height));

    auto source_scene = compositing_scene(canvas);
    add_emissive_quad(
        source_scene,
        source_emissive,
        0.2F,
        mediaserver::doc::NodeId{2U});
    const auto source = centre_rgb(render_slice(
        device, source_scene, scene::whole(canvas), canvas.width,
        canvas.height));

    for (const auto mode : {
             scene::BlendMode::Normal,
             scene::BlendMode::Add,
             scene::BlendMode::Multiply,
             scene::BlendMode::Screen}) {
        auto combined_scene = compositing_scene(canvas);
        add_emissive_quad(
            combined_scene,
            destination_emissive,
            0.0F,
            mediaserver::doc::NodeId{1U});
        scene::LayerStyle style{};
        style.opacity = opacity;
        style.blend = mode;
        add_emissive_quad(
            combined_scene,
            source_emissive,
            0.2F,
            mediaserver::doc::NodeId{2U},
            style);
        const auto image = render_slice(
            device,
            combined_scene,
            scene::whole(canvas),
            canvas.width,
            canvas.height);
        RENDER_CHECK(image.stats.batches == 2U);
        RENDER_CHECK(image.stats.instances == 2U);
        const auto actual = centre_rgb(image);
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            const int expected = expected_blend(
                mode, source[channel], destination[channel], opacity);
            RENDER_CHECK(channel_difference(actual[channel], expected) <= 4);
        }
    }
}

void transparent_order_is_back_to_front_not_vector_order() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};
    scene::LayerStyle transparent{};
    transparent.opacity = 0.5F;

    auto ordered = compositing_scene(canvas);
    add_emissive_quad(
        ordered,
        scene::Vec3{0.25F, 0.01F, 0.01F},
        -0.2F,
        mediaserver::doc::NodeId{20U},
        transparent);
    add_emissive_quad(
        ordered,
        scene::Vec3{0.01F, 0.25F, 0.01F},
        0.2F,
        mediaserver::doc::NodeId{10U},
        transparent);

    auto reversed = ordered;
    std::reverse(reversed.instances.begin(), reversed.instances.end());
    const auto first = render_slice(
        device, ordered, scene::whole(canvas), canvas.width, canvas.height);
    const auto second = render_slice(
        device, reversed, scene::whole(canvas), canvas.width, canvas.height);
    RENDER_CHECK(first.stats.batches == 2U);
    RENDER_CHECK(first.rgba == second.rgba);

    // Near green is composed last, so it contributes more than far red at the
    // centre. This also rules out a stable-but-front-to-back implementation.
    const auto centre = centre_rgb(first);
    RENDER_CHECK(centre[1U] > centre[0U]);
}

void brightness_is_pointwise_while_blur_remains_pending() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};

    auto normal = compositing_scene(canvas);
    add_emissive_quad(
        normal,
        scene::Vec3{0.18F, 0.18F, 0.18F},
        0.0F,
        mediaserver::doc::NodeId{1U});
    const auto normal_image = render_slice(
        device, normal, scene::whole(canvas), canvas.width, canvas.height);

    scene::LayerStyle dim_style{};
    dim_style.effects.brightness_enabled = true;
    dim_style.effects.brightness = 0.25F;
    auto dim = compositing_scene(canvas);
    add_emissive_quad(
        dim,
        scene::Vec3{0.18F, 0.18F, 0.18F},
        0.0F,
        mediaserver::doc::NodeId{1U},
        dim_style);
    const auto dim_image = render_slice(
        device, dim, scene::whole(canvas), canvas.width, canvas.height);
    const auto bright_pixel = centre_rgb(normal_image);
    const auto dim_pixel = centre_rgb(dim_image);
    RENDER_CHECK(dim_pixel[0U] + 30 < bright_pixel[0U]);

    // Blur needs an offscreen neighbourhood and is not implemented in this
    // pointwise pass. Enabling it must not silently substitute a colour tweak.
    dim_style.effects.blur_enabled = true;
    dim_style.effects.blur_radius = 24.0F;
    auto blur_pending = compositing_scene(canvas);
    add_emissive_quad(
        blur_pending,
        scene::Vec3{0.18F, 0.18F, 0.18F},
        0.0F,
        mediaserver::doc::NodeId{1U},
        dim_style);
    const auto blur_image = render_slice(
        device,
        blur_pending,
        scene::whole(canvas),
        canvas.width,
        canvas.height);
    RENDER_CHECK(blur_image.rgba == dim_image.rgba);
}

void levels_executes_in_authored_order_on_lit_geometry() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};
    for (const scene::BrightnessCurve response : {
             scene::BrightnessCurve::Exposure,
             scene::BrightnessCurve::Gain}) {
        scene::Scene plain = compositing_scene(canvas);
        plain.effect_settings.brightness_curve = response;
        add_emissive_quad(
            plain,
            scene::Vec3{0.18F, 0.18F, 0.18F},
            0.0F,
            mediaserver::doc::NodeId{1U});
        const auto reference = render_slice(
            device, plain, scene::whole(canvas), canvas.width, canvas.height);

        scene::Scene identity = plain;
        identity.instances.front().style.effects.inline_operations.push_back(
            scene::InlineEffectOperation{
                scene::EffectKind::Levels, 1.0F,
                scene::LevelsParameters{}});
        const auto identity_image = render_slice(
            device,
            identity,
            scene::whole(canvas),
            canvas.width,
            canvas.height);
        RENDER_CHECK(identity_image.rgba == reference.rgba);
        RENDER_CHECK(identity_image.stats.inline_effect_ops == 0U);

        scene::LevelsParameters levels{};
        levels.master.output_black = 0.2F;
        levels.master.output_white = 0.8F;
        const scene::InlineEffectOperation brightness{
            scene::EffectKind::Brightness, 0.5F, {}};
        const scene::InlineEffectOperation grade{
            scene::EffectKind::Levels, 1.0F, levels};

        scene::Scene brightness_then_levels = plain;
        brightness_then_levels.instances.front()
            .style.effects.inline_operations = {brightness, grade};
        const auto first = render_slice(
            device,
            brightness_then_levels,
            scene::whole(canvas),
            canvas.width,
            canvas.height);

        scene::Scene levels_then_brightness = plain;
        levels_then_brightness.instances.front()
            .style.effects.inline_operations = {grade, brightness};
        const auto second = render_slice(
            device,
            levels_then_brightness,
            scene::whole(canvas),
            canvas.width,
            canvas.height);
        RENDER_CHECK(first.stats.inline_effect_ops == 2U);
        RENDER_CHECK(second.stats.inline_effect_ops == 2U);
        RENDER_CHECK(first.rgba != second.rgba);
        RENDER_CHECK(centre_rgb(first)[0U] > centre_rgb(second)[0U] + 8);
    }
}

void lift_gamma_gain_executes_in_authored_order_on_lit_geometry() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};
    for (const scene::BrightnessCurve response : {
             scene::BrightnessCurve::Exposure,
             scene::BrightnessCurve::Gain}) {
        scene::Scene plain = compositing_scene(canvas);
        plain.effect_settings.brightness_curve = response;
        add_emissive_quad(
            plain,
            scene::Vec3{0.18F, 0.18F, 0.18F},
            0.0F,
            mediaserver::doc::NodeId{1U});
        const auto reference = render_slice(
            device, plain, scene::whole(canvas), canvas.width, canvas.height);

        scene::Scene identity = plain;
        identity.instances.front().style.effects.inline_operations.push_back(
            scene::InlineEffectOperation{
                scene::EffectKind::LiftGammaGain, 1.0F, {},
                scene::LiftGammaGainParameters{}});
        const auto identity_image = render_slice(
            device,
            identity,
            scene::whole(canvas),
            canvas.width,
            canvas.height);
        RENDER_CHECK(identity_image.rgba == reference.rgba);
        RENDER_CHECK(identity_image.stats.inline_effect_ops == 0U);

        scene::LiftGammaGainParameters parameters{};
        parameters.lift = scene::Vec3{0.2F, 0.2F, 0.2F};
        const scene::InlineEffectOperation grade{
            scene::EffectKind::LiftGammaGain, 1.0F, {}, parameters};
        scene::LevelsParameters levels{};
        levels.master.output_black = 0.1F;
        levels.master.output_white = 0.8F;
        const scene::InlineEffectOperation remap{
            scene::EffectKind::Levels, 1.0F, levels};

        scene::Scene grade_then_levels = plain;
        grade_then_levels.instances.front().style.effects.inline_operations =
            {grade, remap};
        const auto first = render_slice(
            device,
            grade_then_levels,
            scene::whole(canvas),
            canvas.width,
            canvas.height);

        scene::Scene levels_then_grade = plain;
        levels_then_grade.instances.front().style.effects.inline_operations =
            {remap, grade};
        const auto second = render_slice(
            device,
            levels_then_grade,
            scene::whole(canvas),
            canvas.width,
            canvas.height);
        RENDER_CHECK(first.stats.inline_effect_ops == 2U);
        RENDER_CHECK(second.stats.inline_effect_ops == 2U);
        RENDER_CHECK(first.rgba != second.rgba);
        RENDER_CHECK(centre_rgb(second)[0U] > centre_rgb(first)[0U] + 5);
    }
}

void contrast_pivot_executes_without_an_intermediate_clamp() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};
    for (const scene::BrightnessCurve response : {
             scene::BrightnessCurve::Exposure,
             scene::BrightnessCurve::Gain}) {
        scene::Scene plain = compositing_scene(canvas);
        plain.effect_settings.brightness_curve = response;
        add_emissive_quad(
            plain,
            scene::Vec3{0.05F, 0.05F, 0.05F},
            0.0F,
            mediaserver::doc::NodeId{1U});
        const auto reference = render_slice(
            device, plain, scene::whole(canvas), canvas.width, canvas.height);

        scene::Scene identity = plain;
        identity.instances.front().style.effects.inline_operations.push_back(
            scene::InlineEffectOperation{
                scene::EffectKind::ContrastPivot, 1.0F, {}, {},
                scene::ContrastPivotParameters{1.0F, 0.9F}});
        const auto identity_image = render_slice(
            device,
            identity,
            scene::whole(canvas),
            canvas.width,
            canvas.height);
        RENDER_CHECK(identity_image.rgba == reference.rgba);
        RENDER_CHECK(identity_image.stats.inline_effect_ops == 0U);

        const scene::InlineEffectOperation contrast{
            scene::EffectKind::ContrastPivot, 1.0F, {}, {},
            scene::ContrastPivotParameters{4.0F, 1.0F}};
        scene::LiftGammaGainParameters lift{};
        lift.lift = scene::Vec3{1.0F, 1.0F, 1.0F};
        const scene::InlineEffectOperation lift_operation{
            scene::EffectKind::LiftGammaGain, 1.0F, {}, lift};

        scene::Scene contrast_then_lift = plain;
        contrast_then_lift.instances.front()
            .style.effects.inline_operations = {contrast, lift_operation};
        const auto first = render_slice(
            device,
            contrast_then_lift,
            scene::whole(canvas),
            canvas.width,
            canvas.height);

        scene::Scene lift_then_contrast = plain;
        lift_then_contrast.instances.front()
            .style.effects.inline_operations = {lift_operation, contrast};
        const auto second = render_slice(
            device,
            lift_then_contrast,
            scene::whole(canvas),
            canvas.width,
            canvas.height);
        RENDER_CHECK(first.stats.inline_effect_ops == 2U);
        RENDER_CHECK(second.stats.inline_effect_ops == 2U);
        RENDER_CHECK(first.rgba != second.rgba);
        const auto first_rgb = centre_rgb(first);
        const auto second_rgb = centre_rgb(second);
        for (std::size_t channel = 0U; channel < first_rgb.size(); ++channel) {
            // Contrast first drives the value below zero. Only the following
            // Lift/Gamma/Gain operation clamps, proving there was no hidden
            // saturation between the two authored operations.
            RENDER_CHECK(first_rgb[channel] < 8);
            RENDER_CHECK(static_cast<int>(second_rgb[channel]) >
                         static_cast<int>(first_rgb[channel]) + 40);
        }
    }
}

void color_matrix_executes_in_authored_order_on_lit_geometry() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};
    for (const scene::BrightnessCurve response : {
             scene::BrightnessCurve::Exposure,
             scene::BrightnessCurve::Gain}) {
        scene::Scene plain = compositing_scene(canvas);
        plain.effect_settings.brightness_curve = response;
        add_emissive_quad(
            plain,
            scene::Vec3{0.05F, 0.05F, 0.05F},
            0.0F,
            mediaserver::doc::NodeId{1U});
        const auto reference = render_slice(
            device, plain, scene::whole(canvas), canvas.width, canvas.height);

        scene::Scene identity = plain;
        identity.instances.front().style.effects.inline_operations.push_back(
            scene::InlineEffectOperation{
                scene::EffectKind::ColorMatrix, 1.0F, {}, {}, {},
                scene::ColorMatrixParameters{}});
        const auto identity_image = render_slice(
            device,
            identity,
            scene::whole(canvas),
            canvas.width,
            canvas.height);
        RENDER_CHECK(identity_image.rgba == reference.rgba);
        RENDER_CHECK(identity_image.stats.inline_effect_ops == 0U);

        scene::ColorMatrixParameters matrix{};
        matrix.matrix = {4.0F, 0.0F, 0.0F,
                         0.0F, 4.0F, 0.0F,
                         0.0F, 0.0F, 4.0F};
        matrix.offset = scene::Vec3{-2.0F, -2.0F, -2.0F};
        const scene::InlineEffectOperation matrix_operation{
            scene::EffectKind::ColorMatrix, 1.0F, {}, {}, {}, matrix};
        scene::LiftGammaGainParameters lift{};
        lift.lift = scene::Vec3{1.0F, 1.0F, 1.0F};
        const scene::InlineEffectOperation lift_operation{
            scene::EffectKind::LiftGammaGain, 1.0F, {}, lift};

        scene::Scene matrix_then_lift = plain;
        matrix_then_lift.instances.front()
            .style.effects.inline_operations = {
                matrix_operation, lift_operation};
        const auto first = render_slice(
            device,
            matrix_then_lift,
            scene::whole(canvas),
            canvas.width,
            canvas.height);

        scene::Scene lift_then_matrix = plain;
        lift_then_matrix.instances.front()
            .style.effects.inline_operations = {
                lift_operation, matrix_operation};
        const auto second = render_slice(
            device,
            lift_then_matrix,
            scene::whole(canvas),
            canvas.width,
            canvas.height);
        RENDER_CHECK(first.stats.inline_effect_ops == 2U);
        RENDER_CHECK(second.stats.inline_effect_ops == 2U);
        const auto first_rgb = centre_rgb(first);
        const auto second_rgb = centre_rgb(second);
        for (std::size_t channel = 0U; channel < first_rgb.size(); ++channel) {
            // The matrix creates a negative value. Lift/Gamma/Gain is the
            // next authored clamp, so a hidden matrix clamp would make this
            // first result bright instead of black.
            RENDER_CHECK(first_rgb[channel] < 8);
            RENDER_CHECK(second_rgb[channel] > first_rgb[channel] + 40);
        }

        scene::ColorMatrixParameters half{};
        half.matrix = {0.5F, 0.0F, 0.0F,
                       0.0F, 0.5F, 0.0F,
                       0.0F, 0.0F, 0.5F};
        const scene::InlineEffectOperation half_operation{
            scene::EffectKind::ColorMatrix, 1.0F, {}, {}, {}, half};
        const scene::InlineEffectOperation contrast{
            scene::EffectKind::ContrastPivot, 1.0F, {}, {},
            scene::ContrastPivotParameters{0.0F, 0.4F}};

        scene::Scene matrix_then_contrast = plain;
        matrix_then_contrast.instances.front()
            .style.effects.inline_operations = {half_operation, contrast};
        const auto third = render_slice(
            device,
            matrix_then_contrast,
            scene::whole(canvas),
            canvas.width,
            canvas.height);
        scene::Scene contrast_then_matrix = plain;
        contrast_then_matrix.instances.front()
            .style.effects.inline_operations = {contrast, half_operation};
        const auto fourth = render_slice(
            device,
            contrast_then_matrix,
            scene::whole(canvas),
            canvas.width,
            canvas.height);
        RENDER_CHECK(third.stats.inline_effect_ops == 2U);
        RENDER_CHECK(fourth.stats.inline_effect_ops == 2U);
        const auto third_rgb = centre_rgb(third);
        const auto fourth_rgb = centre_rgb(fourth);
        for (std::size_t channel = 0U; channel < third_rgb.size(); ++channel) {
            RENDER_CHECK(third_rgb[channel] > fourth_rgb[channel] + 15);
        }
    }
}

void white_balance_executes_in_authored_order_on_lit_geometry() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};
    for (const scene::BrightnessCurve response : {
             scene::BrightnessCurve::Exposure,
             scene::BrightnessCurve::Gain}) {
        scene::Scene plain = compositing_scene(canvas);
        plain.effect_settings.brightness_curve = response;
        add_emissive_quad(
            plain,
            scene::Vec3{0.18F, 0.18F, 0.18F},
            0.0F,
            mediaserver::doc::NodeId{1U});
        const auto reference = render_slice(
            device, plain, scene::whole(canvas), canvas.width, canvas.height);

        scene::Scene identity = plain;
        identity.instances.front().style.effects.inline_operations.push_back(
            scene::InlineEffectOperation{
                scene::EffectKind::WhiteBalance, 1.0F, {}, {}, {}, {},
                scene::WhiteBalanceParameters{}});
        const auto identity_image = render_slice(
            device,
            identity,
            scene::whole(canvas),
            canvas.width,
            canvas.height);
        RENDER_CHECK(identity_image.rgba == reference.rgba);
        RENDER_CHECK(identity_image.stats.inline_effect_ops == 0U);

        const auto white_balance = scene::normalized_white_balance(
            scene::WhiteBalanceParameters{2'000.0F, 0.5F});
        RENDER_CHECK(white_balance.has_value());
        const scene::InlineEffectOperation balance{
            scene::EffectKind::WhiteBalance, 1.0F, {}, {}, {}, {},
            *white_balance};
        const scene::InlineEffectOperation contrast{
            scene::EffectKind::ContrastPivot, 1.0F, {}, {},
            scene::ContrastPivotParameters{0.0F, 0.4F}};

        scene::Scene balance_then_contrast = plain;
        balance_then_contrast.instances.front()
            .style.effects.inline_operations = {balance, contrast};
        const auto first = render_slice(
            device,
            balance_then_contrast,
            scene::whole(canvas),
            canvas.width,
            canvas.height);

        scene::Scene contrast_then_balance = plain;
        contrast_then_balance.instances.front()
            .style.effects.inline_operations = {contrast, balance};
        const auto second = render_slice(
            device,
            contrast_then_balance,
            scene::whole(canvas),
            canvas.width,
            canvas.height);
        RENDER_CHECK(first.stats.inline_effect_ops == 2U);
        RENDER_CHECK(second.stats.inline_effect_ops == 2U);
        RENDER_CHECK(first.rgba != second.rgba);
        const auto first_rgb = centre_rgb(first);
        const auto second_rgb = centre_rgb(second);
        RENDER_CHECK(second_rgb[0U] > first_rgb[0U] + 15);
        RENDER_CHECK(first_rgb[2U] > second_rgb[2U] + 20);
        RENDER_CHECK(second_rgb[0U] > second_rgb[1U] + 20);
    }
}

void curves_execute_in_authored_order_on_lit_geometry() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};
    scene::CurvesParameters authored{};
    authored.master.points = {
        {0.0F, 0.0F}, {0.5F, 0.2F}, {1.0F, 1.0F}};
    const auto curves = scene::normalized_curves(authored);
    RENDER_CHECK(curves.has_value());
    for (const scene::BrightnessCurve response : {
             scene::BrightnessCurve::Exposure,
             scene::BrightnessCurve::Gain}) {
        scene::Scene plain = compositing_scene(canvas);
        plain.effect_settings.brightness_curve = response;
        add_emissive_quad(
            plain, scene::Vec3{0.18F, 0.18F, 0.18F}, 0.0F,
            mediaserver::doc::NodeId{1U});
        const auto reference = render_slice(
            device, plain, scene::whole(canvas), canvas.width, canvas.height);

        scene::Scene identity = plain;
        identity.instances.front().style.effects.inline_operations.push_back(
            scene::InlineEffectOperation{
                scene::EffectKind::Curves, 1.0F, {}, {}, {}, {}, {},
                scene::CurvesParameters{}});
        const auto identity_image = render_slice(
            device, identity, scene::whole(canvas), canvas.width,
            canvas.height);
        RENDER_CHECK(identity_image.rgba == reference.rgba);
        RENDER_CHECK(identity_image.stats.inline_effect_ops == 0U);

        const scene::InlineEffectOperation curve_operation{
            scene::EffectKind::Curves, 1.0F, {}, {}, {}, {}, {}, *curves};
        const scene::InlineEffectOperation contrast{
            scene::EffectKind::ContrastPivot, 1.0F, {}, {},
            scene::ContrastPivotParameters{0.0F, 0.4F}};

        scene::Scene curve_then_contrast = plain;
        curve_then_contrast.instances.front()
            .style.effects.inline_operations = {curve_operation, contrast};
        const auto first = render_slice(
            device, curve_then_contrast, scene::whole(canvas), canvas.width,
            canvas.height);
        scene::Scene contrast_then_curve = plain;
        contrast_then_curve.instances.front()
            .style.effects.inline_operations = {contrast, curve_operation};
        const auto second = render_slice(
            device, contrast_then_curve, scene::whole(canvas), canvas.width,
            canvas.height);
        RENDER_CHECK(first.stats.inline_effect_ops == 2U);
        RENDER_CHECK(second.stats.inline_effect_ops == 2U);
        RENDER_CHECK(first.rgba != second.rgba);
        const auto first_rgb = centre_rgb(first);
        const auto second_rgb = centre_rgb(second);
        for (std::size_t channel = 0U; channel < first_rgb.size(); ++channel) {
            RENDER_CHECK(first_rgb[channel] > second_rgb[channel] + 20);
        }
    }
}

void film_grain_is_deterministic_ordered_and_canvas_anchored() {
    auto device = open_device();
    constexpr scene::Canvas canvas{96U, 64U};
    constexpr core::u32 half = canvas.width / 2U;
    const auto grain_operation = [](scene::FilmGrainMode mode) {
        scene::InlineEffectOperation operation{};
        operation.kind = scene::EffectKind::FilmGrain;
        operation.film_grain = scene::FilmGrainParameters{
            0.08F, 1.0F, 1.0F, 0xC001'D00DU, mode};
        return operation;
    };
    const scene::InlineEffectOperation contrast{
        scene::EffectKind::ContrastPivot, 1.0F, {}, {},
        scene::ContrastPivotParameters{0.0F, 0.5F}};

    for (const scene::BrightnessCurve response : {
             scene::BrightnessCurve::Exposure,
             scene::BrightnessCurve::Gain}) {
        scene::Scene plain = compositing_scene(canvas);
        plain.effect_settings.brightness_curve = response;
        add_emissive_quad(
            plain, scene::Vec3{0.18F, 0.18F, 0.18F}, 0.0F,
            mediaserver::doc::NodeId{1U});
        // Overfill every slice so projection/raster edge coverage cannot mask
        // a canvas-coordinate disagreement in the noise itself.
        plain.instances.front().world =
            scene::scaling(scene::Vec3{8.0F, 8.0F, 1.0F});
        plain.instances.front().normal =
            scene::normal_basis(plain.instances.front().world);
        plain.show_frame = 7U;
        const auto reference = render_slice(
            device, plain, scene::whole(canvas), canvas.width, canvas.height);

        scene::Scene identity = plain;
        identity.instances.front().style.effects.inline_operations.push_back(
            grain_operation(scene::FilmGrainMode::Monochrome));
        identity.instances.front().style.effects.inline_operations.back()
            .film_grain.amount = 0.0F;
        const auto identity_image = render_slice(
            device, identity, scene::whole(canvas), canvas.width,
            canvas.height);
        RENDER_CHECK(identity_image.rgba == reference.rgba);
        RENDER_CHECK(identity_image.stats.inline_effect_ops == 0U);

        scene::Scene monochrome = plain;
        monochrome.instances.front().style.effects.inline_operations = {
            grain_operation(scene::FilmGrainMode::Monochrome)};
        const auto mono_image = render_slice(
            device, monochrome, scene::whole(canvas), canvas.width,
            canvas.height);
        RENDER_CHECK(mono_image.stats.inline_effect_ops == 1U);
        RENDER_CHECK(mono_image.rgba != reference.rgba);
        for (std::size_t pixel = 0U; pixel < mono_image.rgba.size();
             pixel += 4U) {
            RENDER_CHECK(mono_image.rgba[pixel] ==
                         mono_image.rgba[pixel + 1U]);
            RENDER_CHECK(mono_image.rgba[pixel + 1U] ==
                         mono_image.rgba[pixel + 2U]);
            RENDER_CHECK(mono_image.rgba[pixel + 3U] ==
                         reference.rgba[pixel + 3U]);
        }

        scene::Scene rgb = plain;
        rgb.instances.front().style.effects.inline_operations = {
            grain_operation(scene::FilmGrainMode::RGB)};
        const auto whole = render_slice(
            device, rgb, scene::whole(canvas), canvas.width, canvas.height);
        const auto repeated = render_slice(
            device, rgb, scene::whole(canvas), canvas.width, canvas.height);
        RENDER_CHECK(whole.rgba == repeated.rgba);
        RENDER_CHECK(whole.rgba != mono_image.rgba);
        std::size_t chromatic_pixels = 0U;
        for (std::size_t pixel = 0U; pixel < whole.rgba.size(); pixel += 4U) {
            if (whole.rgba[pixel] != whole.rgba[pixel + 1U] ||
                whole.rgba[pixel + 1U] != whole.rgba[pixel + 2U]) {
                ++chromatic_pixels;
            }
        }
        RENDER_CHECK(chromatic_pixels > 100U);

        scene::Scene high_frame = rgb;
        high_frame.show_frame = 0x1'0000'0007ULL;
        const auto high_frame_image = render_slice(
            device, high_frame, scene::whole(canvas), canvas.width,
            canvas.height);
        RENDER_CHECK(high_frame_image.rgba != whole.rgba);

        scene::Scene grain_then_contrast = rgb;
        grain_then_contrast.instances.front()
            .style.effects.inline_operations = {
                grain_operation(scene::FilmGrainMode::RGB), contrast};
        const auto first = render_slice(
            device, grain_then_contrast, scene::whole(canvas), canvas.width,
            canvas.height);
        scene::Scene contrast_then_grain = rgb;
        contrast_then_grain.instances.front()
            .style.effects.inline_operations = {
                contrast, grain_operation(scene::FilmGrainMode::RGB)};
        const auto second = render_slice(
            device, contrast_then_grain, scene::whole(canvas), canvas.width,
            canvas.height);
        RENDER_CHECK(first.stats.inline_effect_ops == 2U);
        RENDER_CHECK(second.stats.inline_effect_ops == 2U);
        RENDER_CHECK(first.rgba != second.rgba);

        // Independent wall nodes render local target pixels, but the hash is
        // anchored to the authored canvas. Stitching adjacent slices must
        // therefore reproduce the whole frame exactly, noise sample for noise
        // sample, rather than restarting the pattern at each slice origin.
        const auto left = render_slice(
            device, rgb, scene::CanvasRect{0U, 0U, half, canvas.height}, half,
            canvas.height);
        const auto right = render_slice(
            device, rgb,
            scene::CanvasRect{half, 0U, half, canvas.height}, half,
            canvas.height);
        const auto offset = [](const RenderedImage& image, core::u32 x,
                               core::u32 y) noexcept {
            return (static_cast<std::size_t>(y) * image.width + x) * 4U;
        };
        for (core::u32 y = 0U; y < canvas.height; ++y) {
            for (core::u32 x = 0U; x < canvas.width; ++x) {
                const std::size_t whole_pixel = offset(whole, x, y);
                const RenderedImage& part = x < half ? left : right;
                const std::size_t part_pixel =
                    offset(part, x < half ? x : x - half, y);
                for (std::size_t channel = 0U; channel < 4U; ++channel) {
                    RENDER_CHECK(whole.rgba[whole_pixel + channel] ==
                                 part.rgba[part_pixel + channel]);
                }
            }
        }
    }
}

void a_slice_outside_the_canvas_is_refused() {
    auto device = open_device();
    constexpr scene::Canvas canvas{96U, 64U};
    auto created =
        render::SceneRenderer::create(device, 64U, 64U, test_limits());
    RENDER_CHECK(created.has_value());
    auto renderer = std::move(created).value();

    auto acquired = device.acquire_commands();
    RENDER_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();

    const auto scn = box_scene(canvas);
    // Starts at the right edge and runs off it: a feed-map error, refused rather
    // than clamped so the error is visible instead of producing a plausible
    // image of the wrong region.
    RENDER_CHECK(!renderer.render(
        commands, scn, scene::CanvasRect{canvas.width, 0U, 64U, 64U}));
    // A valid slice on the same renderer still draws, so the refusal was the
    // slice and not a wedged command list.
    RENDER_CHECK(renderer.render(
        commands, scn, scene::CanvasRect{0U, 0U, 64U, 64U}));
}

void an_explicit_projection_view_drives_the_same_geometry_and_shading() {
    auto device = open_device();
    constexpr scene::Canvas canvas{96U, 64U};
    const scene::Scene scn = box_scene(canvas);
    const scene::CanvasRect whole = scene::whole(canvas);

    const RenderedImage ordinary =
        render_slice(device, scn, whole, canvas.width, canvas.height);
    const scene::ViewMatrices ordinary_matrices =
        scene::view_for_slice(scn.camera, canvas, whole);
    const render::SceneRenderView same_view{
        ordinary_matrices, scn.camera.eye,
        scene::normalize(scn.camera.target - scn.camera.eye)};
    const RenderedImage explicit_same = render_slice(
        device, scn, whole, canvas.width, canvas.height, same_view);
    RENDER_CHECK(explicit_same.rgba == ordinary.rgba);

    // A physical right wall looks along +X from the same design eye. The box
    // in this fixture sits in front along -Z, so it leaves that wall's view.
    // This proves the explicit view is used by the actual GPU draw rather than
    // accepted and then ignored in favour of Scene::camera.
    scene::Camera right_camera = scn.camera;
    right_camera.target = right_camera.eye + scene::Vec3{1.0F, 0.0F, 0.0F};
    const render::SceneRenderView right_view{
        scene::view_for_slice(right_camera, canvas, whole),
        right_camera.eye,
        scene::Vec3{1.0F, 0.0F, 0.0F}};
    const RenderedImage right = render_slice(
        device, scn, whole, canvas.width, canvas.height, right_view);
    RENDER_CHECK(right.rgba != ordinary.rgba);
    RENDER_CHECK(centre_rgb(right) != centre_rgb(ordinary));
}

void verify_mapped_view_storage_uploads(rhi::Device& device) {
    constexpr scene::Canvas canvas{48U, 48U};
    const scene::CanvasRect whole = scene::whole(canvas);
    auto created = render::SceneRenderer::create(
        device, canvas.width, canvas.height, test_limits());
    RENDER_CHECK(created.has_value());
    auto renderer = std::move(created).value();

    scene::Scene red = compositing_scene(canvas);
    add_emissive_quad(
        red, scene::Vec3{1.0F, 0.0F, 0.0F}, 0.0F,
        mediaserver::doc::NodeId{101U});
    scene::Scene green = compositing_scene(canvas);
    add_emissive_quad(
        green, scene::Vec3{0.0F, 1.0F, 0.0F}, 0.0F,
        mediaserver::doc::NodeId{202U});

    const auto first_matrices = scene::view_for_slice(red.camera, canvas, whole);
    render::SceneRenderView first_view{
        first_matrices,
        red.camera.eye,
        scene::normalize(red.camera.target - red.camera.eye),
        1U};
    scene::Camera shifted_camera = green.camera;
    shifted_camera.eye.x += 0.1F;
    shifted_camera.target.x += 0.1F;
    render::SceneRenderView second_view{
        scene::view_for_slice(shifted_camera, canvas, whole),
        shifted_camera.eye,
        scene::normalize(shifted_camera.target - shifted_camera.eye),
        2U};

    const auto byte_count = canvas.width * canvas.height * 4U;
    auto first_readback = device.create_readback(byte_count);
    auto second_readback = device.create_readback(byte_count);
    RENDER_CHECK(first_readback.has_value());
    RENDER_CHECK(second_readback.has_value());
    auto acquired = device.acquire_commands();
    RENDER_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();

    render::SceneRenderOptions first_options{};
    first_options.view = first_view;
    first_options.history = render::SceneHistoryAction::Initialize;
    first_options.history_slot = 0U;
    RENDER_CHECK(renderer.render(commands, red, whole, first_options));
    RENDER_CHECK(commands.download(renderer.target(), first_readback.value()));

    render::SceneRenderOptions second_options{};
    second_options.view = second_view;
    second_options.history = render::SceneHistoryAction::Initialize;
    second_options.history_slot = 1U;
    RENDER_CHECK(renderer.render(commands, green, whole, second_options));
    RENDER_CHECK(commands.download(renderer.target(), second_readback.value()));

    auto submitted = device.submit(std::move(commands));
    RENDER_CHECK(submitted.has_value());
    RENDER_CHECK(std::move(submitted).value().wait());

    const auto first_pixels = first_readback.value().map();
    const auto second_pixels = second_readback.value().map();
    RENDER_CHECK(first_pixels.size() >= byte_count);
    RENDER_CHECK(second_pixels.size() >= byte_count);
    const auto centre =
        (static_cast<std::size_t>(canvas.height / 2U) * canvas.width
         + canvas.width / 2U)
        * 4U;
    RENDER_CHECK(first_pixels[centre] > first_pixels[centre + 1U] + 80U);
    RENDER_CHECK(second_pixels[centre + 1U] > second_pixels[centre] + 80U);
    first_readback.value().unmap();
    second_readback.value().unmap();
}

void mapped_views_recorded_together_keep_their_own_storage_uploads() {
    auto device = open_device();
    verify_mapped_view_storage_uploads(device);

    // The default device exercises SDL's transfer/destination cycling. Run
    // the same one-list proof against native D3D12 as well, where each upload
    // must retain its own fixed-page GPU address.
    if (device.capabilities().backend != rhi::BackendKind::D3d12
        && rhi::Device::available(rhi::BackendKind::D3d12)) {
        rhi::DeviceConfig config{};
        config.backend = rhi::BackendKind::D3d12;
        auto native = rhi::Device::create(config);
        RENDER_CHECK(native.has_value());
        auto native_device = std::move(native).value();
        verify_mapped_view_storage_uploads(native_device);
    }
}

void culling_and_instancing_preserve_pixels_and_cohort_selection() {
    auto device=open_device();
    scene::Scene frame=empty_scene({128U,64U});
    for (core::u32 i=0U;i<5U;++i) {
        scene::MeshInstance instance;
        instance.source=mediaserver::doc::NodeId{10U+i};
        instance.world=scene::multiply(scene::translation({(static_cast<float>(i)-2.0F)*0.55F,0.0F,0.0F}),scene::scaling({0.4F,0.4F,0.4F}));
        instance.normal=scene::normal_basis(instance.world);
        frame.instances.push_back(instance);
    }
    const auto optimized=render_slice(device,frame,{0U,0U,128U,64U},128U,64U);
    frame.renderer_settings.frustum_culling=false;
    frame.renderer_settings.geometry_instancing=false;
    const auto reference=render_slice(device,frame,{0U,0U,128U,64U},128U,64U);
    RENDER_CHECK(optimized.rgba==reference.rgba);
    RENDER_CHECK(optimized.stats.batches==1U && reference.stats.batches==5U);
    frame.renderer_settings.frustum_culling=true;
    frame.renderer_settings.geometry_instancing=true;
    auto hidden=frame.instances.front();
    hidden.source=mediaserver::doc::NodeId{99U};
    hidden.world=scene::translation({0.0F,0.0F,10.0F});
    frame.instances.push_back(hidden);
    const auto left=render_slice(device,frame,{0U,0U,64U,64U},64U,64U);
    const auto right=render_slice(device,frame,{64U,0U,64U,64U},64U,64U);
    RENDER_CHECK(left.selection==right.selection);
    RENDER_CHECK(left.selection.size()==6U);
    RENDER_CHECK(!left.selection.back().visible && left.selection.back().source.value()==99U);
    RENDER_CHECK(left.stats.culled_geometry==1U && left.stats.camera_instances==5U);
    frame.document_version=mediaserver::doc::Version{12U};
    std::reverse(frame.instances.begin(),frame.instances.end());
    const auto reordered=render_slice(device,frame,{0U,0U,64U,64U},64U,64U);
    RENDER_CHECK(reordered.selection==left.selection && reordered.rgba==left.rgba);
}

void repeated_imported_parts_instance_and_lod_agrees_across_views() {
    auto device=open_device();
    scene::Scene frame=empty_scene({128U,64U});
    const auto base=write_material_model();
    // An authored alternate with identical geometry proves the disabled path
    // changes submission only; importer tests separately prove reduced ranges.
    const auto path=std::filesystem::path{base};
    const auto chain=(path.parent_path()/"instanced-lod.obj").string();
    std::filesystem::copy_file(path,chain,std::filesystem::copy_options::overwrite_existing);
    { std::ofstream sidecar{chain+".lod"}; sidecar << "MSLOD 1\n256 \"multipart.obj\"\n"; }
    for (core::u32 i=0U;i<3U;++i) {
        scene::ModelInstance model;
        model.source=chain;
        model.source_node=mediaserver::doc::NodeId{51U+i};
        model.world=scene::multiply(scene::translation({0.0F,(static_cast<float>(i)-1.0F)*0.8F,0.0F}),scene::scaling({0.5F,0.5F,0.5F}));
        model.normal=scene::normal_basis(model.world);
        frame.models.push_back(model);
    }
    const auto optimized=render_slice(device,frame,{0U,0U,128U,64U},128U,64U);
    RENDER_CHECK(optimized.stats.batches==3U && optimized.stats.instanced_batches==3U);
    RENDER_CHECK(optimized.stats.lod_reductions==3U);
    frame.renderer_settings.geometry_instancing=false;
    frame.renderer_settings.model_lod=false;
    const auto reference=render_slice(device,frame,{0U,0U,128U,64U},128U,64U);
    RENDER_CHECK(reference.stats.batches==9U);
    RENDER_CHECK(optimized.rgba==reference.rgba);
    frame.renderer_settings.model_lod=true;
    render::SceneRenderView view;
    auto other_camera=frame.camera;
    other_camera.eye={4.0F,1.0F,8.0F};
    view.matrices=scene::view_for_slice(other_camera,frame.canvas,{0U,0U,128U,64U});
    view.eye=other_camera.eye;
    view.forward=scene::normalize(other_camera.target-other_camera.eye);
    const auto mapped=render_slice(device,frame,{0U,0U,128U,64U},64U,32U,view);
    RENDER_CHECK(mapped.selection.size()==optimized.selection.size());
    for (std::size_t i=0U;i<mapped.selection.size();++i)
        RENDER_CHECK(mapped.selection[i].lod==optimized.selection[i].lod);
}

void culling_preserves_native_shadow_casters_and_fit() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) return;
    rhi::DeviceConfig config;
    config.backend=rhi::BackendKind::D3d12;
    auto opened=rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device=std::move(opened).value();
    auto frame=wall_scene({64U,64U});
    frame.renderer_settings.backend=scene::RendererBackend::Advanced;
    scene::DirectionalLight light;
    light.direction=scene::normalize({0.0F,-1.0F,-1.0F});
    frame.directional_lights.push_back(light);
    auto caster=frame.instances.front();
    caster.source=mediaserver::doc::NodeId{35U};
    caster.world=scene::translation({0.0F,1.0F,8.0F});
    caster.normal=scene::normal_basis(caster.world);
    frame.instances.push_back(caster);
    const auto optimized=render_advanced_environment(device,frame,64U);
    frame.renderer_settings.frustum_culling=false;
    frame.renderer_settings.geometry_instancing=false;
    const auto reference=render_advanced_environment(device,frame,64U);
    RENDER_CHECK(optimized.stats.culled_geometry==1U);
    RENDER_CHECK(optimized.stats.directional_shadow_casters==2U);
    RENDER_CHECK(reference.stats.directional_shadow_casters==2U);
    RENDER_CHECK(optimized.stats.directional_shadow_fit==reference.stats.directional_shadow_fit);
    RENDER_CHECK(optimized.rgba==reference.rgba);
}

void geometry_capacity_refuses_and_large_shared_sets_render() {
    auto device=open_device();
    auto limits=test_limits();
    limits.max_instances=5'000U;
    auto created=render::SceneRenderer::create(device,32U,32U,limits);
    RENDER_CHECK(created);
    auto renderer=std::move(created).value();
    scene::Scene frame=empty_scene({32U,32U});
    // A realistic placement count, with one shared mesh. Zero legacy effects
    // lets this isolate geometry capacity from the separate effect budget.
    scene::MeshInstance instance;
    instance.world=scene::identity();
    instance.normal=scene::identity();
    frame.instances.resize(5'000U,instance);
    for (core::u32 i=0U;i<5'000U;++i) frame.instances[i].source=mediaserver::doc::NodeId{i+1U};
    auto acquired=device.acquire_commands();
    RENDER_CHECK(acquired);
    auto commands=std::move(acquired).value();
    RENDER_CHECK(renderer.render(commands,frame,{0U,0U,32U,32U}));
    RENDER_CHECK(renderer.stats().instances==5'000U);
    RENDER_CHECK(renderer.stats().batches==1U);
    auto submitted=device.submit(std::move(commands));
    RENDER_CHECK(submitted && std::move(submitted).value().wait());
    frame.instances.push_back(instance);
    auto over=device.acquire_commands();
    RENDER_CHECK(over);
    auto refused=std::move(over).value();
    RENDER_CHECK(!renderer.render(refused,frame,{0U,0U,32U,32U}));
    RENDER_CHECK(renderer.stats().dropped_instances==5'001U);
}


void extended_material_lobes_and_transmission_reach_pixels() {
    rhi::DeviceConfig config; config.backend = rhi::BackendKind::D3d12; config.debug = true;
    auto opened = rhi::Device::create(config); RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    scene::Scene scene = box_scene({64U,64U});
    scene.instances[0].mesh = {scene::Primitive::Sphere, 32U};
    scene.renderer_settings.backend = scene::RendererBackend::Advanced;
    scene.environment.ambient = 0.0F;
    scene.directional_lights[0].direction = {0.0F, 0.0F, -1.0F};
    auto& material = scene.instances[0].material;
    material.base_color = {0.0F,0.0F,0.0F}; material.roughness = 0.8F;
    const auto pixel = [](const RenderedImage& image, core::u32 x, core::u32 y, core::u32 c) {
        core::u16 value{}; std::memcpy(&value, image.rgba.data() + (y*image.width+x)*8U+c*2U, 2U);
        return half_to_float(value);
    };
    const auto base = render_advanced_environment(device, scene, 64U);
    material.clearcoat = 1.0F; material.clearcoat_roughness = 0.15F;
    const auto coated = render_advanced_environment(device, scene, 64U);
    RENDER_CHECK(pixel(coated,32U,32U,0U) > pixel(base,32U,32U,0U)*3.0F);
    material.clearcoat = 0.0F; material.sheen_color = {1.0F,0.0F,0.0F}; material.sheen_roughness = 0.8F;
    scene.directional_lights[0].direction = {-0.9F,0.0F,-0.1F};
    const auto velvet = render_advanced_environment(device, scene, 64U);
    RENDER_CHECK(pixel(velvet,32U,32U,0U) > pixel(velvet,32U,32U,1U) + 0.005F);
    material.sheen_color = {}; material.base_color = {0.8F,0.8F,0.8F}; material.metallic = 1.0F;
    material.roughness = 0.25F; material.anisotropy_strength = 0.9F;
    scene.directional_lights[0].direction = {-0.5F,0.0F,-1.0F};
    const auto horizontal = render_advanced_environment(device, scene, 64U);
    material.anisotropy_rotation = 1.57079632679F;
    const auto vertical = render_advanced_environment(device, scene, 64U);
    RENDER_CHECK(std::abs(pixel(horizontal,32U,32U,0U)-pixel(vertical,32U,32U,0U)) > 0.05F);
    // Glass transmits an opaque red object. With IOR=1, white base, zero
    // roughness and no lights, the centre is exactly the background radiance.
    scene.directional_lights.clear(); material = {};
    material.base_color = {1.0F,1.0F,1.0F}; material.roughness = 0.0F;
    material.transmission = 1.0F; material.ior = 1.0F;
    scene.instances[0].source = mediaserver::doc::NodeId{1U};
    auto behind = scene.instances[0]; behind.source = mediaserver::doc::NodeId{2U};
    behind.world = scene::translation({0.0F,0.0F,-1.5F}); behind.normal = scene::normal_basis(behind.world);
    behind.material = {}; behind.material.emissive = {0.8F,0.05F,0.02F};
    scene.instances.push_back(behind);
    const auto glass = render_advanced_environment(device, scene, 64U);
    scene.instances.erase(scene.instances.begin());
    const auto background = render_advanced_environment(device, scene, 64U);
    RENDER_CHECK(std::abs(pixel(glass,32U,32U,0U)-pixel(background,32U,32U,0U)) < 0.002F);
    RENDER_CHECK(pixel(glass,32U,32U,0U) > 0.5F);
    device.wait_idle();
}

void extended_lobes_preserve_slice_pixels() {
    for (const bool advanced : {false, true}) {
        rhi::DeviceConfig config;
        if (advanced) config.backend = rhi::BackendKind::D3d12;
        auto opened = rhi::Device::create(config); RENDER_CHECK(opened);
        auto device = std::move(opened).value();
        const auto mode = advanced ? render::SceneRendererMode::AdvancedLinear
                                   : render::SceneRendererMode::BasicDisplay;
        const core::u32 pixel_bytes = advanced ? 8U : 4U;
        for (int variant = 0; variant < 5; ++variant) {
            auto scene = wall_scene({128U,64U});
            if (advanced) scene.renderer_settings.backend = scene::RendererBackend::Advanced;
            auto& m = scene.instances[0].material;
            if (variant == 0) { m.clearcoat = 0.9F; m.clearcoat_roughness = 0.3F; }
            if (variant == 1) { m.sheen_color = {0.7F,0.1F,0.2F}; m.sheen_roughness = 0.8F; }
            if (variant == 2) { m.anisotropy_strength = 0.85F; m.anisotropy_rotation = 0.7F; }
            if (variant == 3) { m.ior = 1.8F; m.specular = 0.4F; m.specular_color = {0.6F,0.8F,0.9F}; }
            if (variant == 4) {
                auto glass = scene.instances[0]; glass.source = mediaserver::doc::NodeId{3U};
                glass.world.m[14U] += 1.0F; glass.normal = scene::normal_basis(glass.world);
                glass.material = {}; glass.material.base_color = {1.0F,1.0F,1.0F};
                glass.material.transmission = 1.0F; glass.material.ior = 1.5F;
                glass.material.roughness = 1.0F;
                scene.instances.push_back(glass);
            }
            const core::u32 apron = variant == 4 ? 8U : 0U;
            const auto full = render_slice(device,scene,{0U,0U,128U,64U},128U,64U,std::nullopt,mode);
            const auto left = render_slice(device,scene,{0U,0U,64U+apron,64U},64U+apron,64U,std::nullopt,mode);
            const auto right = render_slice(device,scene,{64U-apron,0U,64U+apron,64U},64U+apron,64U,std::nullopt,mode);
            for (core::u32 y=0U;y<64U;++y) for(core::u32 x=0U;x<128U;++x) {
                const auto& part=x<64U?left:right;
                const auto local_x = x < 64U ? x : x - 64U + apron;
                for(core::u32 c=0U;c<pixel_bytes;++c) {
                    const auto whole = full.rgba[(y*128U+x)*pixel_bytes+c];
                    const auto sliced = part.rgba[(y*part.width+local_x)*pixel_bytes+c];
                    if (whole != sliced) throw std::runtime_error{
                        std::string{advanced ? "Advanced" : "Basic"} + " material "
                        + std::to_string(variant) + " slice mismatch at " + std::to_string(x)
                        + "," + std::to_string(y) + " byte " + std::to_string(c)
                        + ": " + std::to_string(whole) + " != " + std::to_string(sliced)};
                }
            }
        }
    }
}

void wide_canvases_render_small_node_slices() {
    for (const bool advanced : {false, true}) {
        rhi::DeviceConfig config;
        if (advanced) config.backend = rhi::BackendKind::D3d12;
        auto opened = rhi::Device::create(config); RENDER_CHECK(opened);
        auto device = std::move(opened).value();
        const auto mode = advanced ? render::SceneRendererMode::AdvancedLinear
                                   : render::SceneRendererMode::BasicDisplay;
        auto scene = wall_scene({65535U, 64U});
        scene.instances[0].material.clearcoat = 0.7F;
        if (advanced) scene.renderer_settings.backend = scene::RendererBackend::Advanced;
        const auto whole = render_slice(device, scene, {32736U, 0U, 64U, 64U}, 64U, 64U, {}, mode);
        const auto left = render_slice(device, scene, {32736U, 0U, 32U, 64U}, 32U, 64U, {}, mode);
        const auto right = render_slice(device, scene, {32768U, 0U, 32U, 64U}, 32U, 64U, {}, mode);
        const core::u32 stride = advanced ? 8U : 4U;
        RENDER_CHECK(whole.stats.instances == 1U);
        for (core::u32 y = 0U; y < 64U; ++y) for (core::u32 x = 0U; x < 64U; ++x) {
            const auto& half = x < 32U ? left : right;
            for (core::u32 c = 0U; c < stride; ++c)
                RENDER_CHECK(whole.rgba[(y * 64U + x) * stride + c]
                    == half.rgba[(y * 32U + x % 32U) * stride + c]);
        }
    }
}

struct Case final {
    const char* name;
    void (*function)();
};

void baked_probe_pixels_are_slice_exact_and_cancel_safe() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) return;
    rhi::DeviceConfig config{}; config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config); RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    scene::Scene lighting{};
    lighting.environment.sky_color = {0.6F, 0.1F, 0.03F};
    lighting.environment.ground_color = lighting.environment.sky_color;
    auto& grid = lighting.environment.baked;
    grid.minimum = {-2,-2,-2}; grid.maximum = {2,2,2};
    grid.count_x = 2; grid.count_y = 2; grid.count_z = 2; grid.samples = 1024;
    const auto baked = render::bake_lighting(lighting); RENDER_CHECK(baked.ready());
    const auto destination = std::filesystem::temp_directory_path() / "mediaserver-gpu-baked";
    const auto bytes = destination.u8string();
    const auto written = render::write_probe_asset(*baked.asset, std::string{bytes.begin(),bytes.end()}); RENDER_CHECK(written.ready());
    render::ProbeCache cache; static_cast<void>(cache.observe(written.source, written.asset->content)); cache.wait_for_test();
    auto scn = empty_scene({64,32});
    scn.environment.sky_color = {}; scn.environment.ground_color = {};
    scn.environment.baked = grid; scn.environment.baked.enabled = true;
    scn.environment.baked.source = written.source; scn.environment.baked.content = written.asset->content;
    scene::MeshInstance object{}; object.mesh = {scene::Primitive::Sphere,32};
    object.world = scene::identity(); object.normal = scene::identity(); object.material.base_color = {1,1,1};
    scn.instances.push_back(object); scn.renderer_settings.gi_strength = 0;
    const auto mode = render::SceneRendererMode::AdvancedLinear;
    const auto whole = render_slice(device, scn, scene::whole(scn.canvas), 64,32, {},mode);
    RENDER_CHECK(whole.stats.baked_ready && whole.stats.baked_probes == 8);
    RENDER_CHECK(!whole.stats.directional_gi_rendered);
    const auto left = render_slice(device, scn, {0,0,32,32},32,32,{},mode);
    const auto right = render_slice(device, scn, {32,0,32,32},32,32,{},mode);
    for(core::u32 y=0;y<32;++y) for(core::u32 x=0;x<64;++x) for(core::u32 channel=0;channel<8;++channel) {
        const auto& slice = x<32?left:right;
        RENDER_CHECK(whole.rgba[(y*64+x)*8+channel] == slice.rgba[(y*32+x%32)*8+channel]);
    }
    auto zero = scn; zero.environment.baked.strength = 0;
    const auto dark = render_slice(device,zero,scene::whole(zero.canvas),64,32,{},mode);
    RENDER_CHECK(dark.rgba != whole.rgba);
    core::u16 red{}, green{};
    const auto center=(16*64+32)*8;
    std::memcpy(&red,whole.rgba.data()+center,2);std::memcpy(&green,whole.rgba.data()+center+2,2);
    RENDER_CHECK(half_to_float(red) > 4 * half_to_float(green));
    auto created=render::SceneRenderer::create(device,64,32,test_limits(),mode);RENDER_CHECK(created);
    auto renderer=std::move(created).value();
    {auto acquired=device.acquire_commands();RENDER_CHECK(acquired);auto commands=std::move(acquired).value();RENDER_CHECK(renderer.render(commands,scn,scene::whole(scn.canvas)));}
    auto acquired=device.acquire_commands();RENDER_CHECK(acquired);auto commands=std::move(acquired).value();
    RENDER_CHECK(renderer.render(commands,scn,scene::whole(scn.canvas)));RENDER_CHECK(renderer.stats().baked_ready);
    auto readback=device.create_readback(64*32*8);RENDER_CHECK(readback);RENDER_CHECK(commands.download(renderer.target(),readback.value()));
    auto submitted=device.submit(std::move(commands));RENDER_CHECK(submitted);RENDER_CHECK(std::move(submitted).value().wait());
    const auto read=readback.value().map();RENDER_CHECK(std::equal(whole.rgba.begin(),whole.rgba.end(),read.begin()));readback.value().unmap();
}

void baked_environment_matches_live_lighting() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) return;
    rhi::DeviceConfig config{}; config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config); RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    auto scn = metallic_scene();
    scn.instances[0].material.metallic = 0;
    scn.instances[0].normal = scene::identity();
    scn.instances[0].mesh = {scene::Primitive::Sphere,32};
    scn.instances[0].material.roughness = 1;
    scn.instances[0].bake_static = false;
    scn.environment.map_source = write_environment_fixture().string();
    scn.environment.map_intensity = 0.13F;
    scn.environment.map_rotation = 0.25F;
    auto& grid = scn.environment.baked;
    grid.minimum = {-2,-2,-2}; grid.maximum = {2,2,2};
    grid.count_x = grid.count_y = grid.count_z = 2; grid.samples = 4096;
    const auto baked = render::bake_lighting(scn); RENDER_CHECK(baked.ready());
    const auto written = render::write_probe_asset(*baked.asset,
        (std::filesystem::temp_directory_path() / "mediaserver-baked-parity").string());
    RENDER_CHECK(written.ready());
    render::ProbeCache cache;
    static_cast<void>(cache.observe(written.source, written.asset->content)); cache.wait_for_test();
    const auto live = render_advanced_environment(device, scn, 64);
    grid.enabled = true; grid.source = written.source; grid.content = written.asset->content;
    const auto after = render_advanced_environment(device, scn, 64);
    RENDER_CHECK(after.stats.baked_ready && after.stats.baked_probes == 8);
    float maximum = 0;
    for (std::size_t i = 0; i < live.rgba.size(); i += 8) {
        core::u16 a{}, b{};
        std::memcpy(&a,live.rgba.data()+i,2); std::memcpy(&b,after.rgba.data()+i,2);
        maximum = std::max(maximum,std::abs(half_to_float(a)-half_to_float(b)));
    }
    std::cout << "live/baked environment maximum linear error=" << maximum << '\n';
    RENDER_CHECK(maximum < 0.025F);
}

constexpr std::array<Case, 32U> cases{{
    {"baked environment matches live lighting", &baked_environment_matches_live_lighting},
    {"baked probe pixels, slices and cancellation", &baked_probe_pixels_are_slice_exact_and_cancel_safe},
    {"wide canvases render small node slices", &wide_canvases_render_small_node_slices},
    {"extended materials reach native pixels", &extended_material_lobes_and_transmission_reach_pixels},
    {"extended lobes preserve slice pixels", &extended_lobes_preserve_slice_pixels},
    {"culling preserves native shadow casters and fit", &culling_preserves_native_shadow_casters_and_fit},
    {"geometry culling and instancing preserve pixels and selection", &culling_and_instancing_preserve_pixels_and_cohort_selection},
    {"imported parts instance and LOD agrees across views", &repeated_imported_parts_instance_and_lod_agrees_across_views},
    {"geometry capacity refuses and large shared sets render", &geometry_capacity_refuses_and_large_shared_sets_render},
    {"an isolated layer keeps its transparent background",
     &an_isolated_layer_keeps_its_transparent_background},
    {"an authored environment lights metal and absence changes nothing",
     &an_authored_environment_lights_metal_and_absence_changes_nothing},
    {"turning the authored rotation turns a resident environment",
     &turning_the_authored_rotation_turns_a_resident_environment},
    {"an empty scene is the clear colour everywhere",
     &an_empty_scene_is_the_clear_colour_everywhere},
    {"a lit object covers part of the frame and leaves sky",
     &a_lit_object_covers_part_of_the_frame_and_leaves_sky},
    {"the halves reconstruct the whole", &the_halves_reconstruct_the_whole},
    {"imported material parts draw as separate pbr batches",
     &imported_material_parts_draw_as_separate_pbr_batches},
    {"a GPU model integration refusal is sticky for its generation",
     &a_gpu_model_integration_refusal_is_sticky_for_its_generation},
    {"canceled recordings reuse and eventually commit one model candidate",
     &canceled_recordings_reuse_and_eventually_commit_one_model_candidate},
    {"imported texture roles reach pixels with their correct encoding",
     &imported_texture_roles_reach_pixels_with_their_correct_encoding},
    {"every layer blend mode uses its real equation",
     &every_layer_blend_mode_uses_its_real_equation},
    {"transparent order is back to front not vector order",
     &transparent_order_is_back_to_front_not_vector_order},
    {"brightness is pointwise while blur remains pending",
     &brightness_is_pointwise_while_blur_remains_pending},
    {"Levels executes in authored order on lit geometry",
     &levels_executes_in_authored_order_on_lit_geometry},
    {"Lift Gamma Gain executes in authored order on lit geometry",
     &lift_gamma_gain_executes_in_authored_order_on_lit_geometry},
    {"Contrast Pivot executes without an intermediate clamp",
     &contrast_pivot_executes_without_an_intermediate_clamp},
    {"Color Matrix executes in authored order on lit geometry",
     &color_matrix_executes_in_authored_order_on_lit_geometry},
    {"White Balance executes in authored order on lit geometry",
     &white_balance_executes_in_authored_order_on_lit_geometry},
    {"Curves execute in authored order on lit geometry",
     &curves_execute_in_authored_order_on_lit_geometry},
    {"Film Grain is deterministic ordered and canvas anchored",
     &film_grain_is_deterministic_ordered_and_canvas_anchored},
    {"a slice outside the canvas is refused",
     &a_slice_outside_the_canvas_is_refused},
    {"an explicit projection view drives geometry and shading",
     &an_explicit_projection_view_drives_the_same_geometry_and_shading},
    {"mapped views recorded together keep their storage uploads",
     &mapped_views_recorded_together_keep_their_own_storage_uploads},
}};

} // namespace

int main() {
    if (!rhi::Device::available()) {
        std::cout << "[skip] no GPU backend on this machine; render suite skipped\n";
        return 0;
    }
    if (!render::SceneRenderer::shaders_available()) {
        std::cout
            << "[skip] this build carries no scene shader bytecode; render suite skipped\n";
        return 0;
    }
    {
        auto device = open_device();
        if (!renderer_usable(device)) {
            std::cout << "[skip] no shader format this build supports on this device; "
                         "render suite skipped\n";
            return 0;
        }
    }

    std::size_t failures = 0U;
    for (const auto& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
