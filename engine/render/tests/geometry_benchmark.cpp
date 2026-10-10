// Times the device-resident geometry arena at the ~30M-triangle tier, the way
// a detailed Blender scene arrives: hundreds of distinct ~100k-triangle parts.
// Reports how long they take to stream in and the steady frame time once they
// have, on native D3D12 with the Advanced renderer and four shadow cascades.
//
//   mediaserver_render_geometry_benchmark [parts=300] [grid side=223] [shadow cascades=4]
#include <mediaserver/render/model_cache.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/scene.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace mediaserver;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

void append(std::vector<std::uint8_t>& out, const void* data, std::size_t bytes) {
    const auto* begin = static_cast<const std::uint8_t*>(data);
    out.insert(out.end(), begin, begin + bytes);
}

// A rippled grid of side x side quads: (side + 1)^2 vertices, 2 side^2 triangles.
void write_grid(const fs::path& path, std::uint32_t side, float phase) {
    const std::uint32_t row = side + 1U;
    std::vector<float> positions, normals;
    positions.reserve(static_cast<std::size_t>(row) * row * 3U);
    normals.reserve(positions.capacity());
    float low = 1e30F, high = -1e30F;
    for (std::uint32_t z = 0U; z < row; ++z) {
        for (std::uint32_t x = 0U; x < row; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(side);
            const float v = static_cast<float>(z) / static_cast<float>(side);
            const float y = 0.08F * std::sin(18.0F * u + phase) * std::cos(14.0F * v + phase);
            positions.insert(positions.end(), {u - 0.5F, y, v - 0.5F});
            normals.insert(normals.end(), {0.0F, 1.0F, 0.0F});
            low = std::min(low, y);
            high = std::max(high, y);
        }
    }
    std::vector<std::uint32_t> indices;
    indices.reserve(static_cast<std::size_t>(side) * side * 6U);
    for (std::uint32_t z = 0U; z < side; ++z) {
        for (std::uint32_t x = 0U; x < side; ++x) {
            const std::uint32_t a = z * row + x, b = a + 1U, c = a + row, d = c + 1U;
            indices.insert(indices.end(), {a, c, b, b, c, d});
        }
    }
    std::vector<std::uint8_t> binary;
    append(binary, positions.data(), positions.size() * sizeof(float));
    const std::size_t normal_offset = binary.size();
    append(binary, normals.data(), normals.size() * sizeof(float));
    const std::size_t index_offset = binary.size();
    append(binary, indices.data(), indices.size() * sizeof(std::uint32_t));
    const std::size_t vertex_count = positions.size() / 3U;
    std::ostringstream json;
    json << "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
         << "\"nodes\":[{\"mesh\":0}],"
         << "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},\"indices\":2,\"material\":0}]}],"
         << "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.6,0.6,0.62,1],\"metallicFactor\":0,\"roughnessFactor\":0.6}}],"
         << "\"buffers\":[{\"byteLength\":" << binary.size() << "}],"
         << "\"bufferViews\":["
         << "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" << normal_offset << "},"
         << "{\"buffer\":0,\"byteOffset\":" << normal_offset << ",\"byteLength\":" << index_offset - normal_offset << "},"
         << "{\"buffer\":0,\"byteOffset\":" << index_offset << ",\"byteLength\":" << binary.size() - index_offset << "}],"
         << "\"accessors\":["
         << "{\"bufferView\":0,\"componentType\":5126,\"count\":" << vertex_count
         << ",\"type\":\"VEC3\",\"min\":[-0.5," << low << ",-0.5],\"max\":[0.5," << high << ",0.5]},"
         << "{\"bufferView\":1,\"componentType\":5126,\"count\":" << vertex_count << ",\"type\":\"VEC3\"},"
         << "{\"bufferView\":2,\"componentType\":5125,\"count\":" << indices.size() << ",\"type\":\"SCALAR\"}]}";
    std::string text = json.str();
    while (text.size() % 4U != 0U) text.push_back(' ');
    while (binary.size() % 4U != 0U) binary.push_back(0U);
    const auto u32 = [](std::ofstream& out, std::uint32_t value) { out.write(reinterpret_cast<const char*>(&value), 4); };
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    u32(out, 0x46546C67U);
    u32(out, 2U);
    u32(out, static_cast<std::uint32_t>(12U + 8U + text.size() + 8U + binary.size()));
    u32(out, static_cast<std::uint32_t>(text.size()));
    u32(out, 0x4E4F534AU);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    u32(out, static_cast<std::uint32_t>(binary.size()));
    u32(out, 0x004E4942U);
    out.write(reinterpret_cast<const char*>(binary.data()), static_cast<std::streamsize>(binary.size()));
}

[[nodiscard]] double milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

} // namespace

int main(int argc, char** argv) {
    const std::uint32_t parts = argc > 1 ? static_cast<std::uint32_t>(std::stoul(argv[1])) : 300U;
    const std::uint32_t side = argc > 2 ? static_cast<std::uint32_t>(std::stoul(argv[2])) : 223U;
    const std::uint32_t cascades = argc > 3 ? static_cast<std::uint32_t>(std::stoul(argv[3])) : 4U;
    if (!rhi::Device::available() || !render::SceneRenderer::shaders_available()) {
        std::cout << "[skip] no GPU or scene shaders\n";
        return 77;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    if (!opened) {
        std::cout << "[skip] no native D3D12 device\n";
        return 77;
    }
    auto device = std::move(opened).value();

    const fs::path directory = fs::temp_directory_path() / "ms-geometry-benchmark";
    fs::create_directories(directory);
    const auto written = Clock::now();
    std::vector<std::string> sources;
    for (std::uint32_t part = 0U; part < parts; ++part) {
        const auto path = directory / ("part-" + std::to_string(side) + "-" + std::to_string(part) + ".glb");
        if (!fs::exists(path)) write_grid(path, side, static_cast<float>(part) * 0.37F);
        sources.push_back(path.string());
    }
    const std::uint64_t triangles = static_cast<std::uint64_t>(parts) * side * side * 2U;
    std::cout << parts << " parts, " << triangles << " triangles, files ready in "
              << milliseconds(Clock::now() - written) << " ms\n";

    constexpr scene::Canvas canvas{1920U, 1080U};
    scene::Scene scn{};
    scn.canvas = canvas;
    scn.show_rate = {50U, 1U};
    const auto columns = static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(parts))));
    const float extent = static_cast<float>(columns);
    scn.camera.eye = scene::Vec3{0.0F, extent * 0.55F, extent * 0.9F};
    scn.camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    scn.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    scn.camera.field_of_view = 0.15F;
    scn.camera.near_plane = 0.1F;
    scn.camera.far_plane = extent * 4.0F;
    scn.environment.sky_color = scene::Vec3{0.3F, 0.35F, 0.45F};
    scn.environment.ground_color = scene::Vec3{0.1F, 0.1F, 0.1F};
    scn.environment.ambient = 0.6F;
    scn.renderer_settings.backend = scene::RendererBackend::Advanced;
    scn.renderer_settings.shadow_cascades = cascades;
    scene::DirectionalLight sun{};
    sun.direction = scene::Vec3{-0.4F, -1.0F, -0.3F};
    sun.color = scene::Vec3{1.0F, 0.97F, 0.9F};
    sun.intensity = 3.0F;
    scn.directional_lights.push_back(sun);
    for (std::uint32_t part = 0U; part < parts; ++part) {
        scene::ModelInstance model{};
        model.source = sources[part];
        const float x = static_cast<float>(part % columns) - extent * 0.5F;
        const float z = static_cast<float>(part / columns) - extent * 0.5F;
        model.world = scene::translation({x, 0.0F, z});
        model.normal = scene::normal_basis(model.world);
        model.source_node = doc::NodeId{1000U + part};
        scn.models.push_back(model);
    }

    render::RendererLimits limits{};
    auto created = render::SceneRenderer::create(
        device, canvas.width, canvas.height, limits, render::SceneRendererMode::AdvancedLinear);
    if (!created) {
        std::cerr << "renderer creation failed\n";
        return 1;
    }
    auto renderer = std::move(created).value();

    struct Timed final { render::RenderStats stats; double record_ms{}, frame_ms{}; bool recorded{}; };
    const auto frame = [&]() -> Timed {
        Timed timed{};
        const auto begin = Clock::now();
        auto acquired = device.acquire_commands();
        if (!acquired) return timed;
        auto commands = std::move(acquired).value();
        timed.recorded = renderer.render(commands, scn, scene::whole(canvas));
        timed.stats = renderer.stats();
        timed.record_ms = milliseconds(Clock::now() - begin);
        // The presenter submits a recording whose assets are still loading.
        auto submitted = device.submit(std::move(commands));
        if (submitted) static_cast<void>(std::move(submitted).value().wait());
        timed.frame_ms = milliseconds(Clock::now() - begin);
        return timed;
    };

    const auto loading = Clock::now();
    std::uint32_t frames{};
    Timed last{};
    for (; frames < 20000U; ++frames) {
        last = frame();
        if (last.recorded && last.stats.models_pending == 0U && last.stats.dropped_instances == 0U) break;
    }
    const double load_ms = milliseconds(Clock::now() - loading);
    std::cout << "resident after " << frames << " frames, " << load_ms << " ms"
              << " (pending " << last.stats.models_pending << ", dropped " << last.stats.dropped_instances
              << ", refused " << last.stats.model_integration_refusals << ")\n";

    std::vector<double> record, total;
    bool steady_uploads = false;
    for (std::uint32_t sample = 0U; sample < 120U; ++sample) {
        const auto timed = frame();
        steady_uploads = steady_uploads || timed.stats.geometry_uploaded;
        if (!timed.recorded) {
            std::cerr << "steady frame failed to record\n";
            return 1;
        }
        record.push_back(timed.record_ms);
        total.push_back(timed.frame_ms);
    }
    const auto report = [](const char* name, std::vector<double> values) {
        std::sort(values.begin(), values.end());
        double sum{};
        for (const double v : values) sum += v;
        std::cout << name << ": mean " << sum / static_cast<double>(values.size())
                  << " ms, p50 " << values[values.size() / 2U]
                  << " ms, p95 " << values[values.size() * 95U / 100U] << " ms\n";
    };
    report("record (CPU)", record);
    report("record + GPU (submit and wait)", total);
    std::cout << "geometry uploaded on steady frames: " << (steady_uploads ? "yes" : "no") << '\n';
    return 0;
}
