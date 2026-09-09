#include <mediaserver/render/ies_profile.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/render/scene_bridge.hpp>
#include <mediaserver/render/baked_lighting.hpp>
#include <mediaserver/render/volumetric_fog.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace render = mediaserver::render;
namespace scene = mediaserver::scene;
namespace rhi = mediaserver::rhi;
namespace doc = mediaserver::doc;
namespace core = mediaserver::core;
#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error(std::to_string(__LINE__) + ": " #__VA_ARGS__); } while (false)
constexpr float pi = 3.14159265358979323846F;
const std::string web = "IESNA:LM-63-2002\n[TEST] synthetic asymmetric fixture\nTILT=NONE\n"
    "1 1000 1 5 3 1 2 0 0 0 1 1 10\n0 30 60 90 180\n0 90 180\n"
    "100 100 100 0 0\n100 50 0 0 0\n100 0 0 0 0\n";
const std::string uniform = "IESNA:LM-63-2002\nTILT=NONE\n1 1000 1 2 1 1 2 0 0 0 1 1 10\n0 180\n0\n100 100\n";
std::filesystem::path directory() {
    auto result = std::filesystem::temp_directory_path() / "mediaserver-step26-ies";
    std::filesystem::create_directories(result); return result;
}
std::string write(const char* name, const std::string& text) {
    auto path = directory() / name;
    std::ofstream file(path, std::ios::binary); file << text; CHECK(file);
    const auto utf8 = path.u8string(); return {utf8.begin(), utf8.end()};
}
void parser_and_photometry() {
    const auto result = render::parse_ies_profile(web); CHECK(result.profile);
    const auto& p = *result.profile;
    CHECK(render::sample_ies_profile(p, 60, 0) == 1);
    CHECK(render::sample_ies_profile(p, 60, 90) == 0);
    CHECK(render::sample_ies_profile(p, 60, 180) == 0);
    CHECK(std::abs(render::sample_ies_profile(p, 30, 45) - 0.75F) < 1e-6F);
    CHECK(render::sample_ies_profile(p, 90, 0) == 0);
    CHECK(render::sample_ies_profile(p, 30, -45) == render::sample_ies_profile(p, 30, 45));
    CHECK(render::sample_ies_profile(p, 30, 360) == render::sample_ies_profile(p, 30, 0));
    CHECK(render::parse_ies_profile(web).profile->values == p.values);
    auto bad = web; bad.replace(bad.find("TILT=NONE"), 9, "TILT=INCLUDE");
    CHECK(render::parse_ies_profile(bad).error == render::IesError::Tilt);
    bad = web; bad.replace(bad.find("5 3 1 2"), 7, "5 3 2 2");
    CHECK(render::parse_ies_profile(bad).error == render::IesError::PhotometricType);
    bad = web; bad.replace(bad.find("0 30 60"), 7, "0 30 30");
    CHECK(render::parse_ies_profile(bad).error == render::IesError::Angles);
    bad = web; bad.replace(bad.find("100 100 100"), 3, "nan");
    CHECK(!render::parse_ies_profile(bad).profile);
    CHECK(!render::parse_ies_profile(web.substr(0, web.size() - 5)).profile);
    bad = uniform; bad.replace(bad.find("100 100\n"), 7, "0 0");
    CHECK(render::parse_ies_profile(bad).error == render::IesError::Dark);
    scene::PointLight light;
    light.direction = {0, 0, -1}; light.intensity = 1200; light.unit = scene::LightUnit::Lumens;
    const auto omni = render::parse_ies_profile(uniform); CHECK(omni.profile);
    const auto emission = render::ies_light_emission(light, 1, omni.profile.get());
    CHECK(std::abs(emission.intensity * 4 * pi - 1200) < 0.05F);
    // An axial uniform IES must retain ordinary spot lumen calibration,
    // including cones narrower than a resampling ring and hard boundaries.
    light.kind = scene::LocalLightKind::Spot;
    for (const float outer : {0.001F, 0.003F, 0.07F, 0.249F}) {
        light.outer_cone = outer;
        for (const float fraction : {0.0F, 0.5F, 1.0F}) {
            light.inner_cone = outer * fraction;
            const auto ies = render::ies_light_emission(light, 1, omni.profile.get());
            const auto ordinary = scene::light_emission(light, 1);
            CHECK(std::abs(ies.intensity / ordinary.intensity - 1) < 0.005F);
        }
    }
    // Independently integrate the rendered distribution on a uniform solid-
    // angle grid. The packed lumen value must conserve flux after clipping.
    for (auto kind : {scene::LocalLightKind::Point, scene::LocalLightKind::Spot}) {
        light.kind = kind; light.inner_cone = 0.04F; light.outer_cone = 0.18F;
        const auto e = render::ies_light_emission(light, 1, &p);
        double integral = 0;
        for (unsigned v = 0; v < 1024; ++v) {
            const float cosine = -1 + (static_cast<float>(v) + 0.5F) / 512.0F;
            const float theta = std::acos(cosine) * 180 / pi;
            const float cone = kind == scene::LocalLightKind::Spot ? scene::spot_cone_attenuation(light, cosine) : 1;
            for (unsigned h = 0; h < 360; ++h)
                integral += render::sample_ies_profile(p, theta, static_cast<float>(h) + 0.5F) * cone;
        }
        integral *= 4.0 * pi / (1024 * 360);
        CHECK(std::abs(integral * e.intensity - 1200) < 1.0);
    }
    light.kind = scene::LocalLightKind::Point; light.unit = scene::LightUnit::Candela;
    CHECK(render::ies_light_emission(light, 1, &p).intensity == 1200);
    const auto bright = scene::normalize(scene::Vec3{0.8660254F, 0, -0.5F}, {0, 0, -1});
    CHECK(render::ies_weight(p, light, bright) > 0.99F);
    light.ies_rotation = 0.5F;
    CHECK(render::ies_weight(p, light, bright) < 0.001F);
    scene::Scene s; light.source = doc::NodeId{9}; light.ies_source = "a.ies"; s.point_lights.push_back(light);
    auto bridge = render::SceneBridge::create(8); CHECK(bridge); CHECK(bridge.value().update(s));
    const auto before = bridge.value().records()[0].visual_identity;
    const auto identity = scene::digest(s), assets = scene::asset_set_digest(s), bake = render::bake_input_digest(s);
    s.point_lights[0].ies_source = "b.ies";
    CHECK(scene::digest(s) != identity && scene::asset_set_digest(s) != assets && render::bake_input_digest(s) != bake);
    CHECK(bridge.value().update(s)); CHECK(bridge.value().records()[0].visual_identity != before);
    const auto changed = scene::digest(s); s.point_lights[0].ies_rotation = 0.125F;
    CHECK(scene::digest(s) != changed);
}

render::IesObservation observe(render::IesCache& cache, std::string_view source, core::u64 fence = 0, core::u64 generation = 0) {
    for (unsigned i = 0; i < 10000; ++i) {
        auto result = cache.observe(source, fence, generation);
        if (result.state != render::IesCacheState::Busy) return result;
        std::this_thread::yield();
    }
    throw std::runtime_error("cache stayed busy");
}

void cache_contract() {
    render::IesCache cache, peer;
    const auto source = write("cache.ies", web);
    const auto fence = cache.publication_epoch();
    CHECK(observe(cache, source, fence).state == render::IesCacheState::Pending);
    cache.wait_for_test();
    CHECK(!observe(cache, source, fence).profile);
    const auto first = observe(cache, source); CHECK(first.profile);
    CHECK(observe(peer, source).profile == first.profile);
    const auto missing = (directory() / "missing.ies").string();
    std::filesystem::remove(missing);
    CHECK(observe(cache, missing).state == render::IesCacheState::Pending);
    cache.wait_for_test(); CHECK(observe(cache, missing).state == render::IesCacheState::Failed);
    write("missing.ies", uniform);
    CHECK(observe(cache, missing).state == render::IesCacheState::Failed);
    CHECK(cache.refresh(missing)); cache.wait_for_test(); CHECK(observe(cache, missing).profile);
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    cache.set_next_load_gate_for_test(std::make_shared<const std::function<void()>>([&] { entered.set_value(); released.wait(); }));
    const auto prior_late = cache.stats().late_results;
    CHECK(cache.refresh(source)); entered.get_future().wait();
    const auto fresh = observe(cache, source, 0, 43); CHECK(fresh.state == render::IesCacheState::Pending && !fresh.profile);
    static_cast<void>(observe(cache, source, 0, 44));
    release.set_value(); cache.wait_for_test();
    const auto current = observe(cache, source, 0, 44);
    CHECK(current.profile && current.generation > first.generation);
    CHECK(cache.stats().late_results == prior_late + 1 && cache.stats().overwrites > 0);
    CHECK(observe(cache, std::string(1025, 'x')).state == render::IesCacheState::Capacity);
}

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
    scene::Scene s; s.canvas = {128, 64}; s.camera.eye = {0, 0, 2}; s.camera.target = {0, 0, 0};
    s.camera.field_of_view = 0.25F; s.camera.near_plane = 0.1F; s.camera.far_plane = 30;
    s.environment.sky_color = s.environment.ground_color = {}; s.environment.ambient = 0;
    s.renderer_settings.backend = scene::RendererBackend::Advanced;
    s.renderer_settings.gi_strength = 0; s.renderer_settings.shadow_cascades = 0;
    return s;
}
void gpu_contract(rhi::Device& device) {
    auto full = render::SceneRenderer::create(device, 128, 64, limits(), render::SceneRendererMode::AdvancedLinear); CHECK(full);
    auto left = render::SceneRenderer::create(device, 64, 64, limits(), render::SceneRendererMode::AdvancedLinear); CHECK(left);
    auto right = render::SceneRenderer::create(device, 64, 64, limits(), render::SceneRendererMode::AdvancedLinear); CHECK(right);
    auto s = scene_fixture();
    const auto draw = [&](render::SceneRenderer& renderer, scene::CanvasRect slice) {
        auto cmd = acquire(device); CHECK(renderer.render(cmd, s, slice));
        return readback(device, std::move(cmd), renderer.target(), 8);
    };
    const auto whole = scene::whole(s.canvas);
    const auto compare_slices = [&](const std::vector<core::u8>& image) {
        const auto a = draw(left.value(), {0, 0, 64, 64});
        const auto b = draw(right.value(), {64, 0, 64, 64});
        for (std::size_t y = 0; y < 64; ++y) {
            CHECK(std::equal(a.begin() + y * 512, a.begin() + (y + 1) * 512, image.begin() + y * 1024));
            CHECK(std::equal(b.begin() + y * 512, b.begin() + (y + 1) * 512, image.begin() + y * 1024 + 512));
        }
    };
    const auto dark_sky = draw(full.value(), whole);
    scene::DirectionalLight sun; sun.direction = {0, 0, 1}; sun.angular_radius = 0.025F;
    sun.unit = scene::LightUnit::Lux; sun.intensity = 100; s.directional_lights.push_back(sun);
    const auto small_sun = draw(full.value(), whole); CHECK(small_sun != dark_sky);
    CHECK(pixel(small_sun, 128, 64, 32) > 1 && pixel(small_sun, 128, 4, 4) == 0);
    CHECK(small_sun == draw(full.value(), whole)); compare_slices(small_sun);
    s.directional_lights[0].angular_radius = 0.05F;
    const auto large_sun = draw(full.value(), whole);
    CHECK(pixel(large_sun, 128, 64, 32) < pixel(small_sun, 128, 64, 32) * 0.3F);
    const auto lit_pixels = [](const auto& image) { unsigned count = 0; for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 128; ++x) if (pixel(image, 128, x, y) > 0.01F) ++count; return count; };
    CHECK(lit_pixels(large_sun) > lit_pixels(small_sun) * 3); compare_slices(large_sun);
    s.directional_lights[0].angular_radius = 0; CHECK(draw(full.value(), whole) == dark_sky);
    s.directional_lights.clear();
    scene::MeshInstance wall; wall.mesh = {scene::Primitive::Quad, 1}; wall.source = doc::NodeId{2};
    wall.world = scene::scaling({8, 4, 1}); wall.normal = scene::normal_basis(wall.world); wall.material.base_color = {0.5F, 0.5F, 0.5F};
    s.instances.push_back(wall);
    scene::PointLight lamp; lamp.source = doc::NodeId{3}; lamp.position = {0, 0, 1}; lamp.direction = {0, 0, -1};
    lamp.unit = scene::LightUnit::Candela; lamp.intensity = 1000; lamp.range = 20;
    s.point_lights.push_back(lamp);
    const auto unprofiled = draw(full.value(), whole);
    const auto source = write("gpu.ies", web); s.point_lights[0].ies_source = source;
    render::IesCache cache; static_cast<void>(observe(cache, source)); cache.wait_for_test();
    const auto profiled = draw(full.value(), whole); CHECK(full.value().stats().ies_ready == 1 && full.value().stats().assets.ready);
    CHECK(profiled != unprofiled);
    CHECK(pixel(profiled, 128, 88, 32) > pixel(profiled, 128, 40, 32) * 8);
    compare_slices(profiled); CHECK(profiled == draw(full.value(), whole));
    const auto settled_bytes = full.value().stats().upload_bytes;
    CHECK(settled_bytes < 1024 * 1024); // No atlas upload on a settled recording.
    // Canceled uploads must be retried, including replacement in the same renderer.
    const auto second = write("gpu-second.ies", uniform); s.point_lights[0].ies_source = second;
    static_cast<void>(observe(cache, second)); cache.wait_for_test();
    { auto cancelled = acquire(device); CHECK(full.value().render(cancelled, s, whole)); }
    const auto uniform_image = draw(full.value(), whole); CHECK(uniform_image == unprofiled);
    CHECK(full.value().stats().upload_bytes > 1024 * 1024);
    s.point_lights[0].ies_source = source;
    s.point_lights[0].ies_rotation = 0.5F;
    const auto rotated = draw(full.value(), whole);
    CHECK(pixel(rotated, 128, 40, 32) > pixel(rotated, 128, 88, 32) * 8);
    s.point_lights[0].ies_source = (directory() / "not-a-profile.ies").string();
    static_cast<void>(draw(full.value(), whole)); cache.wait_for_test();
    const auto missing = draw(full.value(), whole);
    CHECK(!full.value().stats().assets.ready && full.value().stats().ies_failed == 1);
    CHECK(pixel(missing, 128, 64, 32) == 0);
    // Fog reads the same packed profile and must change when it changes.
    s.environment.fog_density = 0.5F; s.environment.fog_color = {1, 1, 1};
    auto fog = render::VolumetricFog::create(device, 128, 64, {16, 8, 16}); CHECK(fog);
    const auto fogged = [&](const std::string& path) {
        s.point_lights[0].ies_source = path;
        auto cmd = acquire(device); CHECK(full.value().render(cmd, s, whole));
        CHECK(fog.value().record(cmd, full.value().target(), full.value().product_depth_target(), full.value().shadow_atlas_target(),
            full.value().light_storage(), full.value().light_shadow_storage(), full.value().light_storage_count(),
            s, scene::view_for_slice(s.camera, s.canvas, whole).view_projection, s.canvas, whole, {}, nullptr, &full.value().ies_atlas()));
        return readback(device, std::move(cmd), fog.value().target(), 8);
    };
    const auto air = fogged(source); CHECK(air != fogged(second)); CHECK(air == fogged(source));
    s.point_lights.clear();
    for (unsigned i = 0; i < render::max_ies_profiles + 1; ++i) {
        lamp.source = doc::NodeId{100U + i}; lamp.ies_source = source + std::to_string(i);
        s.point_lights.push_back(lamp);
    }
    static_cast<void>(draw(full.value(), whole));
    CHECK(full.value().stats().ies_refused == 1 && !full.value().stats().assets.ready);
}

int main() {
    try {
        parser_and_photometry(); std::cout << "[pass] IES parser, angular samples, flux and identities\n";
        cache_contract(); std::cout << "[pass] IES shared worker, sticky failure, refresh, publication fence and late result\n";
        if (!rhi::Device::available(rhi::BackendKind::D3d12)) { std::cout << "[skip] D3D12 GPU checks unavailable\n"; return 77; }
        rhi::DeviceConfig config; config.backend = rhi::BackendKind::D3d12;
        auto device = rhi::Device::create(config); CHECK(device);
        gpu_contract(device.value());
        std::cout << "[pass] sun size, IES geometry and fog, held bytes, canceled upload and exact slices\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
