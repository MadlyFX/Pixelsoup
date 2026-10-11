// Times the device-resident geometry arena at the ~30M-triangle tier, the way
// a detailed Blender scene arrives: hundreds of distinct ~100k-triangle parts.
// Reports how long they take to stream in and the steady frame time once they
// have, on native D3D12 with the Advanced renderer and four shadow cascades.
//
//   mediaserver_render_geometry_benchmark [parts=300] [grid side=223] [shadow cascades=4] [mode=static]
//
// Modes: `static` times a held frame; `orbit` times a camera that moves every
// show frame; `moving` times a part that moves every show frame, so the shadow
// maps it is in are drawn again; `digest` prints hashes of the rendered
// products over static, orbiting, moving-part and relit sequences, for
// comparing two builds that must draw identical pictures.
//
// MS_BENCH_VIEWS=N records N renderers of the same scene into each frame's one
// command list, as the editor does with Program, its transition target and the
// stage view. MS_BENCH_TEXTURED=K gives the first K parts a 1024x1024
// base-colour map of their own.
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <dxgi1_4.h>
#pragma comment(lib, "dxgi.lib")
#endif

#include <mediaserver/render/model_cache.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
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

// An uncompressed 32-bit TGA checkerboard, coloured by `seed`.
void write_texture(const fs::path& path, std::uint32_t size, std::uint32_t seed) {
    std::array<std::uint8_t, 18U> header{};
    header[2U] = 2U;
    header[12U] = static_cast<std::uint8_t>(size & 0xFFU);
    header[13U] = static_cast<std::uint8_t>(size >> 8U);
    header[14U] = header[12U];
    header[15U] = header[13U];
    header[16U] = 32U;
    header[17U] = 0x28U;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size) * size * 4U);
    for (std::uint32_t y = 0U; y < size; ++y) {
        for (std::uint32_t x = 0U; x < size; ++x) {
            const bool dark = ((x / 32U) + (y / 32U)) % 2U != 0U;
            auto* const texel = pixels.data() + (static_cast<std::size_t>(y) * size + x) * 4U;
            texel[0U] = static_cast<std::uint8_t>(dark ? 40U : 120U + (seed * 37U) % 120U);
            texel[1U] = static_cast<std::uint8_t>(dark ? 40U : 120U + (seed * 61U) % 120U);
            texel[2U] = static_cast<std::uint8_t>(dark ? 40U : 120U + (seed * 89U) % 120U);
            texel[3U] = 255U;
        }
    }
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(reinterpret_cast<const char*>(header.data()), header.size());
    out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
}

// A rippled grid of side x side quads: (side + 1)^2 vertices, 2 side^2
// triangles, with a base-colour map when `texture` names one.
void write_grid(const fs::path& path, std::uint32_t side, float phase, float amplitude,
                const std::string& texture = {}) {
    const std::uint32_t row = side + 1U;
    const bool textured = !texture.empty();
    std::vector<float> positions, normals, uvs;
    positions.reserve(static_cast<std::size_t>(row) * row * 3U);
    normals.reserve(positions.capacity());
    if (textured) uvs.reserve(static_cast<std::size_t>(row) * row * 2U);
    float low = 1e30F, high = -1e30F;
    for (std::uint32_t z = 0U; z < row; ++z) {
        for (std::uint32_t x = 0U; x < row; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(side);
            const float v = static_cast<float>(z) / static_cast<float>(side);
            const float y = amplitude * std::sin(18.0F * u + phase) * std::cos(14.0F * v + phase);
            positions.insert(positions.end(), {u - 0.5F, y, v - 0.5F});
            normals.insert(normals.end(), {0.0F, 1.0F, 0.0F});
            if (textured) uvs.insert(uvs.end(), {u, v});
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
    const std::size_t uv_offset = binary.size();
    append(binary, uvs.data(), uvs.size() * sizeof(float));
    const std::size_t vertex_count = positions.size() / 3U;
    std::ostringstream json;
    json << "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
         << "\"nodes\":[{\"mesh\":0}],"
         << "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1"
         << (textured ? ",\"TEXCOORD_0\":3" : "") << "},\"indices\":2,\"material\":0}]}],"
         << "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.6,0.6,0.62,1],\"metallicFactor\":0,\"roughnessFactor\":0.6"
         << (textured ? ",\"baseColorTexture\":{\"index\":0}" : "") << "}}],";
    if (textured) {
        json << "\"images\":[{\"uri\":\"" << texture << "\"}],\"textures\":[{\"source\":0}],";
    }
    json << "\"buffers\":[{\"byteLength\":" << binary.size() << "}],"
         << "\"bufferViews\":["
         << "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" << normal_offset << "},"
         << "{\"buffer\":0,\"byteOffset\":" << normal_offset << ",\"byteLength\":" << index_offset - normal_offset << "},"
         << "{\"buffer\":0,\"byteOffset\":" << index_offset << ",\"byteLength\":" << uv_offset - index_offset << "}";
    if (textured) {
        json << ",{\"buffer\":0,\"byteOffset\":" << uv_offset << ",\"byteLength\":" << binary.size() - uv_offset << "}";
    }
    json << "],\"accessors\":["
         << "{\"bufferView\":0,\"componentType\":5126,\"count\":" << vertex_count
         << ",\"type\":\"VEC3\",\"min\":[-0.5," << low << ",-0.5],\"max\":[0.5," << high << ",0.5]},"
         << "{\"bufferView\":1,\"componentType\":5126,\"count\":" << vertex_count << ",\"type\":\"VEC3\"},"
         << "{\"bufferView\":2,\"componentType\":5125,\"count\":" << indices.size() << ",\"type\":\"SCALAR\"}";
    if (textured) {
        json << ",{\"bufferView\":3,\"componentType\":5126,\"count\":" << vertex_count << ",\"type\":\"VEC2\"}";
    }
    json << "]}";
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

[[nodiscard]] std::uint64_t fnv1a(std::uint64_t hash, std::span<const std::uint8_t> bytes) {
    for (const std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

[[nodiscard]] std::optional<std::string> environment(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t length = 0U;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return std::nullopt;
    std::string text{value};
    std::free(value);
    return text;
#else
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    return std::string{value};
#endif
}

[[nodiscard]] std::optional<float> environment_float(const char* name) {
    const auto text = environment(name);
    if (!text) return std::nullopt;
    return std::stof(*text);
}

[[nodiscard]] std::uint32_t texel_bytes(rhi::TextureFormat format) {
    switch (format) {
    case rhi::TextureFormat::Rgba32Float: return 16U;
    case rhi::TextureFormat::Rgba16Float: return 8U;
    default: return 4U;
    }
}

// Device-local memory this process holds, as the video memory manager counts
// it. Negative where it cannot be asked.
[[nodiscard]] double process_vram_mib() {
#if defined(_WIN32)
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return -1.0;
    UINT64 total = 0U;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT index = 0U; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        IDXGIAdapter3* queried = nullptr;
        if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&queried)))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (SUCCEEDED(queried->QueryVideoMemoryInfo(0U, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                total += info.CurrentUsage;
            }
            queried->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return static_cast<double>(total) / 1048576.0;
#else
    return -1.0;
#endif
}

} // namespace

int main(int argc, char** argv) {
    const std::uint32_t parts = argc > 1 ? static_cast<std::uint32_t>(std::stoul(argv[1])) : 300U;
    const std::uint32_t side = argc > 2 ? static_cast<std::uint32_t>(std::stoul(argv[2])) : 223U;
    const std::uint32_t cascades = argc > 3 ? static_cast<std::uint32_t>(std::stoul(argv[3])) : 4U;
    const std::string mode = argc > 4 ? argv[4] : "static";
    // MS_BENCH_FLAT=1 flattens the grids; MS_BENCH_PILLARS=N stands N boxes on them.
    const bool flat = environment_float("MS_BENCH_FLAT").value_or(0.0F) != 0.0F;
    const auto pillars = static_cast<std::uint32_t>(environment_float("MS_BENCH_PILLARS").value_or(0.0F));
    const auto view_count = std::max(1U, static_cast<std::uint32_t>(environment_float("MS_BENCH_VIEWS").value_or(1.0F)));
    const auto textured = static_cast<std::uint32_t>(environment_float("MS_BENCH_TEXTURED").value_or(0.0F));
    if (mode != "static" && mode != "orbit" && mode != "moving" && mode != "digest") {
        std::cerr << "mode must be static, orbit, moving or digest\n";
        return 2;
    }
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
        const bool mapped = part < textured;
        const auto name = std::string{flat ? "flat-" : "part-"} + (mapped ? "tex-" : "")
            + std::to_string(side) + "-" + std::to_string(part);
        const auto path = directory / (name + ".glb");
        const auto texture = mapped ? "map-" + std::to_string(part) + ".tga" : std::string{};
        if (mapped && !fs::exists(directory / texture)) write_texture(directory / texture, 1024U, part);
        if (!fs::exists(path)) write_grid(path, side, static_cast<float>(part) * 0.37F, flat ? 0.0F : 0.08F, texture);
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
    // Look overrides for experiments; the digests use the defaults.
    if (const auto gi = environment_float("MS_BENCH_GI")) scn.renderer_settings.gi_strength = *gi;
    if (const auto radius = environment_float("MS_BENCH_FILTER")) scn.renderer_settings.shadow_filter_radius = *radius;
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
    // Upright boxes standing on the grid, casting long shadows across it: a
    // set-like shadow layout, where most of the frame is fully lit or fully
    // shadowed, rather than the ripples' self-shadowing everywhere.
    for (std::uint32_t pillar = 0U; pillar < pillars; ++pillar) {
        scene::MeshInstance box{};
        box.source = doc::NodeId{900000U + pillar};
        box.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
        const float x = std::fmod(static_cast<float>(pillar) * 7.31F, extent) - extent * 0.5F;
        const float z = std::fmod(static_cast<float>(pillar) * 3.17F, extent) - extent * 0.5F;
        box.world = scene::multiply(scene::translation({x, 1.0F, z}), scene::scaling({0.3F, 2.0F, 0.3F}));
        box.normal = scene::normal_basis(box.world);
        box.material.base_color = scene::Vec3{0.5F, 0.5F, 0.55F};
        scn.instances.push_back(box);
    }

    const double idle_vram = process_vram_mib();
    render::RendererLimits limits{};
    std::vector<render::SceneRenderer> renderers;
    renderers.reserve(view_count);
    for (std::uint32_t view = 0U; view < view_count; ++view) {
        auto created = render::SceneRenderer::create(
            device, canvas.width, canvas.height, limits, render::SceneRendererMode::AdvancedLinear);
        if (!created) {
            std::cerr << "renderer creation failed\n";
            return 1;
        }
        renderers.push_back(std::move(created).value());
    }
    auto& renderer = renderers.front();

    struct Timed final {
        render::RenderStats stats;
        core::FrameTiming timing{};
        double record_ms{}, frame_ms{};
        bool recorded{};
        bool settled{};
    };
    // Every view in one recording, in order, as the editor records its own.
    const auto frame = [&]() -> Timed {
        Timed timed{};
        const auto begin = Clock::now();
        auto acquired = device.acquire_commands();
        if (!acquired) return timed;
        auto commands = std::move(acquired).value();
        timed.recorded = true;
        timed.settled = true;
        for (auto& view : renderers) {
            timed.recorded = view.render(commands, scn, scene::whole(canvas)) && timed.recorded;
            timed.settled = timed.settled && view.stats().models_pending == 0U
                && view.stats().dropped_instances == 0U;
        }
        timed.stats = renderer.stats();
        timed.record_ms = milliseconds(Clock::now() - begin);
        // The presenter submits a recording whose assets are still loading.
        auto submitted = device.submit(std::move(commands));
        if (submitted) {
            auto work = std::move(submitted).value();
            if (work.wait()) timed.timing = work.frame_timing();
        }
        timed.frame_ms = milliseconds(Clock::now() - begin);
        return timed;
    };

    const auto loading = Clock::now();
    std::uint32_t frames{};
    Timed last{};
    for (; frames < 20000U; ++frames) {
        last = frame();
        if (last.recorded && last.settled) break;
    }
    const double load_ms = milliseconds(Clock::now() - loading);
    std::cout << "resident after " << frames << " frames, " << load_ms << " ms"
              << " (pending " << last.stats.models_pending << ", dropped " << last.stats.dropped_instances
              << ", refused " << last.stats.model_integration_refusals << ")\n";

    const scene::Vec3 home_eye = scn.camera.eye;
    // A slow orbit about the target, one step per show frame.
    const auto orbit = [&](std::uint32_t step) {
        const float angle = 0.004F * static_cast<float>(step);
        scn.camera.eye = scene::Vec3{home_eye.x * std::cos(angle) + home_eye.z * std::sin(angle), home_eye.y,
            home_eye.z * std::cos(angle) - home_eye.x * std::sin(angle)};
    };

    if (mode == "digest") {
        // MS_BENCH_DUMP names a folder that receives each digested frame's
        // colour target, for measuring how two builds' pictures differ.
        const auto dump_folder = environment("MS_BENCH_DUMP");
        std::uint32_t dumped = 0U;
        const auto failed = [&](const char* step) -> std::uint64_t {
            const auto& stats = renderer.stats();
            std::cerr << "digest frame failed at " << step << " (pending " << stats.models_pending
                      << ", dropped " << stats.dropped_instances << ", refused "
                      << stats.model_integration_refusals << ", texture state "
                      << static_cast<unsigned>(stats.texture_streaming.state) << ")\n";
            return 0U;
        };
        // Every view's products are hashed; the first view's hash is the
        // digest, and every other view must agree with it.
        bool views_agree = true;
        const auto digest = [&]() -> std::uint64_t {
            auto acquired = device.acquire_commands();
            if (!acquired) return failed("acquire");
            auto commands = std::move(acquired).value();
            std::vector<std::vector<rhi::Readback>> readbacks(renderers.size());
            for (std::size_t view = 0U; view < renderers.size(); ++view) {
                auto& drawn = renderers[view];
                if (!drawn.render(commands, scn, scene::whole(canvas))) return failed("render");
                std::vector<const rhi::Texture*> products{&drawn.target(), &drawn.velocity_target(),
                    &drawn.object_id_target(), &drawn.normal_target()};
                for (std::uint32_t cascade = 0U; cascade < drawn.stats().directional_shadow_cascades; ++cascade)
                    products.push_back(&drawn.cascade_target(cascade));
                for (const auto* texture : products) {
                    auto readback = device.create_readback(texture->width() * texture->height()
                        * texel_bytes(texture->desc().format));
                    if (!readback) return failed("readback");
                    if (!commands.download(*texture, readback.value())) return failed("download");
                    readbacks[view].push_back(std::move(readback).value());
                }
            }
            auto submitted = device.submit(std::move(commands));
            if (!submitted || !submitted.value().wait()) return failed("submit");
            std::vector<std::uint64_t> hashes;
            for (auto& view : readbacks) {
                std::uint64_t hash = 0xCBF29CE484222325ULL;
                for (auto& readback : view) {
                    const auto bytes = readback.map();
                    hash = fnv1a(hash, bytes);
                    if (dump_folder && &view == &readbacks.front() && &readback == &view.front()) {
                        std::ofstream out{fs::path{*dump_folder} / ("frame-" + std::to_string(dumped++) + ".rgba16f"),
                            std::ios::binary | std::ios::trunc};
                        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                    }
                    readback.unmap();
                }
                hashes.push_back(hash);
            }
            for (const auto hash : hashes) views_agree = views_agree && hash == hashes.front();
            return hashes.front();
        };
        const auto print = [](const char* name, std::uint64_t hash) {
            std::cout << "digest " << name << ": " << std::hex << hash << std::dec << '\n';
        };
        scn.show_frame = 1000U;
        print("static first", digest());
        print("static held", digest());
        print("static held again", digest());
        std::uint64_t combined = 0xCBF29CE484222325ULL;
        for (std::uint32_t step = 1U; step <= 6U; ++step) {
            ++scn.show_frame;
            orbit(step);
            const auto hash = digest();
            combined = fnv1a(combined, {reinterpret_cast<const std::uint8_t*>(&hash), sizeof(hash)});
        }
        print("orbit", combined);
        scn.camera.eye = home_eye;
        combined = 0xCBF29CE484222325ULL;
        for (std::uint32_t step = 1U; step <= 6U; ++step) {
            ++scn.show_frame;
            auto& moving = scn.models[parts / 2U];
            moving.world = scene::multiply(scene::translation({0.0F, 0.05F, 0.0F}), moving.world);
            moving.normal = scene::normal_basis(moving.world);
            const auto hash = digest();
            combined = fnv1a(combined, {reinterpret_cast<const std::uint8_t*>(&hash), sizeof(hash)});
        }
        print("moving part", combined);
        combined = 0xCBF29CE484222325ULL;
        for (std::uint32_t step = 1U; step <= 2U; ++step) {
            ++scn.show_frame;
            scn.directional_lights.front().direction.x += 0.05F;
            const auto hash = digest();
            combined = fnv1a(combined, {reinterpret_cast<const std::uint8_t*>(&hash), sizeof(hash)});
        }
        print("relit", combined);
        ++scn.show_frame;
        print("relit held", digest());
        if (renderers.size() > 1U) {
            std::cout << renderers.size() << " views " << (views_agree ? "agree" : "DIFFER") << '\n';
        }
        return views_agree ? 0 : 1;
    }

    std::vector<double> record, total, gpu;
    std::array<double, core::render_pass_count> pass_ms{};
    bool steady_uploads = false;
    for (std::uint32_t sample = 0U; sample < 120U; ++sample) {
        if (mode == "orbit") {
            ++scn.show_frame;
            orbit(sample + 1U);
        } else if (mode == "moving") {
            ++scn.show_frame;
            auto& moving = scn.models[parts / 2U];
            moving.world = scene::multiply(
                scene::translation({0.0F, sample % 2U == 0U ? 0.05F : -0.05F, 0.0F}), moving.world);
            moving.normal = scene::normal_basis(moving.world);
        }
        const auto timed = frame();
        steady_uploads = steady_uploads || timed.stats.geometry_uploaded;
        if (!timed.recorded) {
            std::cerr << "steady frame failed to record\n";
            return 1;
        }
        record.push_back(timed.record_ms);
        total.push_back(timed.frame_ms);
        if (timed.timing.gpu_valid) {
            gpu.push_back(static_cast<double>(timed.timing.gpu_ns) / 1e6);
            for (std::size_t pass = 0U; pass < pass_ms.size(); ++pass)
                pass_ms[pass] += static_cast<double>(timed.timing.pass_ns[pass]) / 1e6 / 120.0;
        }
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
    if (!gpu.empty()) {
        report("GPU (timestamps)", gpu);
        for (std::size_t pass = 0U; pass < pass_ms.size(); ++pass)
            if (pass_ms[pass] >= 0.05)
                std::cout << "  " << core::render_pass_names[pass] << ": " << pass_ms[pass] << " ms\n";
    }
    const auto& stats = renderer.stats();
    std::cout << "triangles " << stats.triangles << ", camera triangles " << stats.camera_triangles
              << ", visible parts " << stats.visible_geometry << ", culled " << stats.culled_geometry
              << ", cascades " << stats.directional_shadow_cascades << '\n';
    std::cout << "geometry arena " << static_cast<double>(stats.geometry_capacity_bytes) / 1048576.0
              << " MiB (" << static_cast<double>(stats.geometry_used_bytes) / 1048576.0 << " MiB in use)\n";
    std::cout << "geometry uploaded on steady frames: " << (steady_uploads ? "yes" : "no") << '\n';
    for (std::size_t view = 0U; view < renderers.size(); ++view) {
        const auto& seen = renderers[view].stats();
        std::cout << "view " << view << ": shadow maps reused " << seen.shadow_maps_reused
                  << " (drawn by another view " << seen.shadow_maps_shared << "), model textures "
                  << static_cast<double>(seen.texture_streaming.allocated_bytes) / 1048576.0
                  << " MiB (" << seen.texture_streaming.textures << " textures), "
                  << static_cast<double>(seen.texture_streaming.device_bytes) / 1048576.0
                  << " MiB on the device\n";
    }
    std::cout << "process device memory " << process_vram_mib() << " MiB (" << idle_vram
              << " MiB before any renderer)\n";
    return 0;
}
