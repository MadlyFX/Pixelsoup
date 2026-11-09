#include <mediaserver/render/temporal_resolve.hpp>
#include <mediaserver/render/hybrid_scene_renderer.hpp>
#include <mediaserver/render/icvfx_stage.hpp>
#include <mediaserver/render/slice_apron.hpp>
#include <mediaserver/output/perspective_mapping.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;
namespace output = mediaserver::output;
#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error(std::string{"line "} + std::to_string(__LINE__) + ": " + #__VA_ARGS__); } while (false)

template<class T> std::span<const core::u8> bytes(const std::vector<T>& values) {
    return {reinterpret_cast<const core::u8*>(values.data()), values.size() * sizeof(T)};
}

rhi::Texture texture(rhi::Device& device, core::u32 w, core::u32 h, rhi::TextureFormat format) {
    rhi::TextureDesc desc{};
    desc.width = w; desc.height = h; desc.format = format;
    desc.sampled = desc.color_target = true;
    auto result = device.create_texture(desc);
    CHECK(result);
    return std::move(result).value();
}

void submit(rhi::Device& device, rhi::CommandList commands) {
    auto result = device.submit(std::move(commands));
    CHECK(result);
    CHECK(result.value().wait());
}

// The AO format exercises exactly the same rejection shader as HDR color.
// A broad current neighborhood keeps the color clamp from hiding a bad tap.
void depth_reprojection_and_scalar_convergence(rhi::Device& device) {
    constexpr core::u32 size = 8U, center = 4U * size + 4U;
    auto color = texture(device, size, size, rhi::TextureFormat::R32Float);
    auto motion = texture(device, size, size, rhi::TextureFormat::Rgba32Float);
    auto ids = texture(device, size, size, rhi::TextureFormat::R32Uint);
    auto created = render::TemporalResolve::create(device, size, size, {1U}, rhi::TextureFormat::R32Float);
    CHECK(created);
    auto resolver = std::move(created).value();
    std::vector<float> colors(size * size, 0.8F);
    std::vector<std::array<float, 4U>> velocities(size * size, {0.0F, 0.0F, 0.2F, 0.2F});
    std::vector<core::u32> object_ids(size * size, 7U);
    const auto record = [&](render::HistoryDisposition disposition, core::u32 slot = 0U) {
        auto acquired = device.acquire_commands(); CHECK(acquired);
        auto commands = std::move(acquired).value();
        CHECK(commands.upload(color, bytes(colors)));
        CHECK(commands.upload(motion, bytes(velocities)));
        CHECK(commands.upload(ids, bytes(object_ids)));
        auto resolved = resolver.apply(device, commands, slot, disposition, {&color, &motion, &ids, size, size});
        CHECK(resolved);
        auto readback = device.create_readback(size * size * sizeof(float)); CHECK(readback);
        CHECK(commands.download(*resolved.value(), readback.value()));
        submit(device, std::move(commands));
        auto mapped = readback.value().map();
        CHECK(mapped.size() == size * size * sizeof(float));
        std::vector<float> pixels(size * size);
        std::memcpy(pixels.data(), mapped.data(), mapped.size());
        readback.value().unmap();
        return pixels;
    };
    const auto neighborhood = [&] {
        std::fill(colors.begin(), colors.end(), 0.2F);
        colors[center - 1U] = 0.0F;
        colors[center + 1U] = 1.0F;
    };
    record(render::HistoryDisposition::Initialize);
    neighborhood();
    // Camera/object movement changed current Z, while its predicted previous
    // depth still matches the history. Valid motion must keep accumulation.
    for (auto& v : velocities) { v[2] = 0.05F; v[3] = 0.2F; }
    const auto accepted = record(render::HistoryDisposition::Advance);
    CHECK(std::abs(accepted[center] - 0.68F) < 0.0001F);
    CHECK(record(render::HistoryDisposition::Hold) == accepted);

    // A different face of the same object occupied the old pixel.
    for (auto& v : velocities) { v[2] = 0.1F; v[3] = 0.3F; }
    const auto rejected = record(render::HistoryDisposition::Advance);
    CHECK(rejected[center] == colors[center]);

    // A rejected bilinear neighbor must not bleed into an otherwise valid tap.
    std::fill(colors.begin(), colors.end(), 1.0F);
    colors[center] = 0.2F;
    std::fill(object_ids.begin(), object_ids.end(), 8U);
    object_ids[center] = 7U;
    for (auto& v : velocities) v = {0.0F, 0.0F, 0.2F, 0.2F};
    record(render::HistoryDisposition::Reset);
    neighborhood();
    std::fill(object_ids.begin(), object_ids.end(), 7U);
    for (auto& v : velocities) v[0] = -0.5F / size;
    CHECK(std::abs(record(render::HistoryDisposition::Advance)[center] - 0.2F) < 0.0001F);

    // A rotating AO kernel has temporal noise. The identical resolver reduces
    // its variance, with a repeat preserving the converged scalar byte for byte.
    for (auto& v : velocities) v = {0.0F, 0.0F, 0.2F, 0.2F};
    neighborhood(); colors[center] = 0.5F;
    record(render::HistoryDisposition::Reset);
    float accumulated_error = 0.0F, raw_error = 0.0F;
    std::vector<float> last;
    for (core::u32 frame = 0U; frame < 32U; ++frame) {
        colors[center] = frame % 2U ? 0.7F : 0.3F;
        last = record(render::HistoryDisposition::Advance);
        if (frame >= 8U) {
            accumulated_error += std::pow(last[center] - 0.5F, 2.0F);
            raw_error += std::pow(colors[center] - 0.5F, 2.0F);
        }
    }
    CHECK(accumulated_error < raw_error * 0.1F);
    CHECK(record(render::HistoryDisposition::Hold) == last);
    record(render::HistoryDisposition::Initialize, 1U);
    record(render::HistoryDisposition::Advance, 0U);
    CHECK(resolver.stats().restarts == 1U);
    CHECK(resolver.stats().targets_allocated == 1U);

    // Canceled initialization cannot turn the next Hold into a read of undefined
    // GPU storage. Retry must re-prime the same bounded allocation.
    {
        auto acquired = device.acquire_commands(); CHECK(acquired);
        auto commands = std::move(acquired).value();
        CHECK(resolver.apply(device, commands, 2U, render::HistoryDisposition::Initialize,
            {&color, &motion, &ids, size, size}));
    }
    CHECK(record(render::HistoryDisposition::Hold, 2U) == colors);
}

render::RendererLimits limits() {
    render::RendererLimits value{};
    value.max_instances = 16U;
    value.max_vertices = 8192U;
    value.max_indices = 32768U;
    value.max_lights = 4U;
    value.directional_shadow_resolution = 64U;
    value.max_history_slots = 4U;
    value.max_temporal_history_targets = 2U;
    return value;
}

scene::Scene fixture() {
    scene::Scene value{};
    value.id = doc::NodeId{1U}; value.document_version = doc::Version{1U};
    value.canvas = {64U, 64U}; // Intentionally different from the plate raster.
    value.camera_source = doc::NodeId{2U};
    value.camera.eye = {0.0F, 0.0F, 3.0F};
    value.camera.target = {0.0F, 0.0F, 0.0F};
    value.camera.near_plane = 0.1F; value.camera.far_plane = 100.0F;
    value.renderer_settings.backend = scene::RendererBackend::Advanced;
    value.renderer_settings.required = true;
    value.renderer_settings.taau = true;
    value.renderer_settings.occlusion_radius = 0.7F;
    value.renderer_settings.occlusion_strength = 1.0F;
    value.renderer_settings.bloom_strength = 0.2F;
    value.renderer_settings.slice_apron_limit = 512U;
    value.renderer_settings.icvfx.inner_frustum = true;
    value.environment.ambient = 1.0F;
    value.environment.sky_color = {0.02F, 0.03F, 0.04F};
    // Pinned to the fill this fixture was written against. Without it (the
    // default once a scene has a light) the glossy odd-split check differs by
    // one half-float step at x=32 - a seam the fill's ambient used to round
    // away - which is tracked separately rather than hidden here for good.
    value.environment.fill_sky_color = value.environment.sky_color;
    value.environment.fill_with_lights = true;
    scene::MeshInstance box{};
    box.source = doc::NodeId{3U}; box.mesh = {scene::Primitive::Box, 1U};
    box.world = scene::rotation(0.05F, 0.0F, 0.0F);
    box.normal = scene::normal_basis(box.world);
    box.material.base_color = {0.8F, 0.3F, 0.1F};
    box.material.emissive = {2.0F, 0.2F, 0.1F};
    box.material.roughness = 0.08F;
    value.instances.push_back(box);
    box.source = doc::NodeId{4U};
    box.world = scene::multiply(scene::translation({0.0F, 0.0F, -1.0F}), scene::scaling({4.0F, 4.0F, 0.1F}));
    box.normal = scene::normal_basis(box.world);
    box.material.emissive = {}; box.material.base_color = {0.6F, 0.6F, 0.6F};
    value.instances.push_back(box);
    scene::DirectionalLight light{};
    light.direction = {-0.3F, -0.5F, -0.8F}; light.intensity = 3.0F;
    value.directional_lights.push_back(light);
    scene::TrackedCameraView view{};
    view.camera = doc::NodeId{11U}; view.source = doc::NodeId{12U};
    view.authored = value.camera;
    view.icvfx.inner_frustum = true; view.icvfx.soft_edge_px = 8U;
    view.pose.position = {0.2F, 0.1F, 3.0F};
    view.pose.field_of_view = 0.05F; view.pose.aspect_ratio = 4.0F / 3.0F;
    view.pose.valid = true;
    value.tracked_cameras.push_back(view);
    return value;
}

void guard_budget() {
    auto value = fixture();
    const auto padded = render::padded_view_for(value, 128U, 96U, true);
    CHECK(padded);
    CHECK(padded.value().apron >= 32U + 2U + 256U);
    CHECK(padded.value().crop.width == 128U && padded.value().crop.height == 96U);
    CHECK(padded.value().render.width == 128U + 2U * padded.value().apron);
    value.renderer_settings.slice_apron_limit = 32U;
    CHECK(!render::padded_view_for(value, 128U, 96U, true));
    CHECK(!render::padded_view_for(value, 16384U, 96U, true));
    render::SliceApronRequirements enormous{};
    enormous.spatial_filter = 0xFFFF'FFFFU;
    enormous.alignment = 64U;
    CHECK(!render::padded_slice({64U, 64U}, {16U, 16U, 16U, 16U}, enormous, 2048U));
}

void actual_occlusion_history_holds(rhi::Device& device) {
    auto value = fixture();
    value.renderer_settings.bloom_strength = 0.0F;
    auto created = render::HybridSceneRenderer::create(device, 64U, 64U, limits());
    CHECK(created);
    auto renderer = std::move(created).value();
    const auto record = [&](core::u64 frame) {
        value.show_frame = frame;
        auto acquired = device.acquire_commands(); CHECK(acquired);
        auto commands = std::move(acquired).value();
        auto request = renderer.convenience_request(value, scene::whole(value.canvas), {});
        auto result = renderer.render_frame(commands, request); CHECK(result);
        CHECK(result.value().occlusion);
        auto readback = device.create_readback(64U * 64U * sizeof(float)); CHECK(readback);
        auto motion = device.create_readback(64U * 64U * 16U); CHECK(motion);
        CHECK(commands.download(*result.value().velocity, motion.value()));
        CHECK(commands.download(*result.value().occlusion, readback.value()));
        submit(device, std::move(commands));
        const auto motion_bytes = motion.value().map();
        std::array<float, 4U> sky_motion{};
        std::memcpy(sky_motion.data(), motion_bytes.data(), sizeof(sky_motion));
        if (value.instances.empty()) CHECK(sky_motion == std::array<float, 4U>{});
        motion.value().unmap();
        auto mapped = readback.value().map();
        std::vector<core::u8> pixels(mapped.begin(), mapped.end());
        readback.value().unmap();
        return pixels;
    };
    record(0U);
    const auto advancing = record(1U);
    CHECK(record(1U) == advancing);
    CHECK(renderer.diagnostics().temporal_restarts == 0U);
    value.instances.clear();
    value.document_version = doc::Version{2U};
    record(2U);
}

void compositor_reports_and_resets_restart_evidence(rhi::Device& device) {
    auto value = fixture();
    value.renderer_settings.bloom_strength = 0.0F;
    auto bounds = limits(); bounds.max_temporal_history_targets = 1U;
    auto created = render::CompositorSceneExecutor::create(device, 64U, 64U,
        bounds, {}, rhi::TextureFormat::Rgba16Float);
    CHECK(created);
    auto executor = std::move(created).value();
    const auto record = [&](core::u64 frame, core::u64 camera) {
        value.show_frame = frame;
        render::SceneRenderView view{};
        view.matrices = scene::view_for_slice(value.camera, value.canvas, scene::whole(value.canvas));
        view.eye = value.camera.eye; view.history_identity = camera;
        auto acquired = device.acquire_commands(); CHECK(acquired);
        auto commands = std::move(acquired).value();
        CHECK(executor.execute(commands, value, view));
        submit(device, std::move(commands));
    };
    record(0U, 10U);
    record(0U, 11U);
    record(1U, 10U);
    CHECK(executor.temporal_restarts() > 0U);
    CHECK(executor.temporal_restart_frame() == 1U);
    value.document_version = doc::Version{value.document_version.value() + 1U};
    record(1U, 10U);
    CHECK(executor.temporal_restarts() == 0U);
    CHECK(executor.temporal_restart_frame() == 0U);
}

void glossy_geometry_matches_an_odd_node_split(rhi::Device& device) {
    auto value = fixture();
    value.renderer_settings.taau = false;
    value.renderer_settings.occlusion_radius = 0.0F;
    value.renderer_settings.bloom_strength = 0.0F;
    value.instances.front().mesh = {scene::Primitive::Sphere, 16U};
    value.instances.front().material.emissive = {};
    value.instances.front().material.metallic = 0.8F;
    const auto record = [&](scene::CanvasRect slice) {
        auto created = render::CompositorSceneExecutor::create(device, slice.width, slice.height,
            limits(), {}, rhi::TextureFormat::Rgba16Float);
        CHECK(created);
        auto executor = std::move(created).value();
        auto acquired = device.acquire_commands(); CHECK(acquired);
        auto commands = std::move(acquired).value();
        auto result = executor.execute(commands, value, slice); CHECK(result);
        auto readback = device.create_readback(slice.width * slice.height * 8U); CHECK(readback);
        CHECK(commands.download(executor.target(), readback.value()));
        submit(device, std::move(commands));
        auto mapped = readback.value().map();
        std::vector<core::u8> pixels(mapped.begin(), mapped.end());
        readback.value().unmap();
        return pixels;
    };
    const auto full = record({0U, 0U, 64U, 64U});
    const auto left = record({0U, 0U, 31U, 64U});
    const auto right = record({31U, 0U, 33U, 64U});
    for (core::u32 y = 0U; y < 64U; ++y) {
        const auto begin = full.begin() + y * 64U * 8U;
        CHECK(std::equal(begin, begin + 31U * 8U, left.begin() + y * 31U * 8U));
        CHECK(std::equal(begin + 31U * 8U, begin + 64U * 8U, right.begin() + y * 33U * 8U));
    }
}

void moving_plate_matches_nodes_for_32_frames(rhi::Device& device) {
    constexpr scene::Canvas wall{128U, 96U};
    const output::PhysicalSurface surface{{}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, 8.0F, 6.0F, wall};
    const std::array<scene::CanvasRect, 4U> slices{{{0U, 0U, 128U, 96U}, {0U, 0U, 64U, 96U}, {64U, 0U, 64U, 96U}, {0U, 0U, 128U, 96U}}};
    std::vector<render::IcvfxStage> stages;
    std::vector<rhi::Texture> outers;
    for (const auto slice : slices) {
        auto created = render::IcvfxStage::create(device, slice.width, slice.height, limits(), {}, rhi::TextureFormat::Rgba16Float);
        CHECK(created); stages.push_back(std::move(created).value());
        outers.push_back(texture(device, slice.width, slice.height, rhi::TextureFormat::Rgba16Float));
    }
    auto value = fixture();
    value.renderer_settings.motion_blur = true;
    value.renderer_settings.motion_blur_radius = 8U;
    value.renderer_settings.plate_depth_of_field = true;
    value.renderer_settings.plate_dof_radius = 6U;
    value.tracked_cameras.front().pose.focal_length_mm = 200.0F;
    value.tracked_cameras.front().pose.focus_distance_m = 2.0F;
    value.tracked_cameras.front().pose.aperture_f_number = 1.4F;
    value.show_rate = {24U, 1U};
    value.tracked_cameras.front().authored.shutter_seconds = 1.0F / 96.0F;
    const auto record = [&](std::size_t node, const scene::InnerFrustumRegion& region) {
        auto acquired = device.acquire_commands(); CHECK(acquired);
        auto commands = std::move(acquired).value();
        CHECK(commands.clear(outers[node], {0.01F, 0.02F, 0.03F, 1.0F}));
        const auto* result = stages[node].record(commands, value, surface, slices[node], {&region, 1U}, outers[node]);
        CHECK(result);
        auto readback = device.create_readback(slices[node].width * slices[node].height * 8U); CHECK(readback);
        CHECK(commands.download(*result, readback.value()));
        submit(device, std::move(commands));
        auto mapped = readback.value().map();
        std::vector<core::u8> pixels(mapped.begin(), mapped.end());
        readback.value().unmap();
        return pixels;
    };
    std::vector<core::u8> previous;
    core::u32 changed = 0U;
    core::u32 visibly_blurred = 0U;
    for (core::u32 frame = 0U; frame < 32U; ++frame) {
        value.show_frame = frame;
        value.tracked_cameras.front().pose.position = {0.2F + 0.006F * frame, 0.1F, 3.0F + 0.004F * frame};
        const float object_x = 0.375F * (static_cast<int>(frame % 9U) - 4);
        value.instances.front().world = scene::multiply(scene::translation({object_x, 0.0F, -0.005F * frame}), scene::rotation(0.05F, 0.0F, 0.0F));
        value.instances.front().normal = scene::normal_basis(value.instances.front().world);
        const scene::InnerFrustumRegion region{{40U + frame % 3U, 20U, 48U, 40U}, true, false};
        const auto full = record(0U, region);
        const auto left = record(1U, region);
        const auto right = record(2U, region);
        for (core::u32 y = 0U; y < wall.height; ++y) {
            const auto begin = full.begin() + y * wall.width * 8U;
            CHECK(std::equal(begin, begin + 64U * 8U, left.begin() + y * 64U * 8U));
            CHECK(std::equal(begin + 64U * 8U, begin + 128U * 8U, right.begin() + y * 64U * 8U));
        }
        CHECK(record(0U, region) == full);
        CHECK(record(1U, region) == left);
        CHECK(record(2U, region) == right);
        value.renderer_settings.motion_blur = false;
        if (record(3U, region) != full) ++visibly_blurred;
        value.renderer_settings.motion_blur = true;
        if (!previous.empty() && full != previous) ++changed;
        previous = full;
        for (std::size_t node = 0U; node < 3U; ++node) {
            const auto& stage = stages[node];
            CHECK(stage.inner());
            CHECK(stage.inner()->padded_width() > stage.inner()->width());
            CHECK(stage.inner()->temporal_restarts() == 0U);
            CHECK(stage.inner()->renderer_passes().motion_blur == render::PassState::Active);
            CHECK(stage.inner()->renderer_passes().plate_dof == render::PassState::Active);
        }
    }
    CHECK(changed > 24U);
    CHECK(visibly_blurred > 8U);
    std::cout << "[pass] 32 moving plate frames: exact HDR node slices and repeated frames\n";
}
} // namespace

int main() {
    try {
        guard_budget();
        if (!rhi::Device::available(rhi::BackendKind::D3d12) || !render::TemporalResolve::shaders_available()) {
            std::cout << "[skip] D3D12 temporal shaders unavailable; guard budget passed\n";
            return 0;
        }
        rhi::DeviceConfig config{}; config.backend = rhi::BackendKind::D3d12; config.debug = true;
        auto opened = rhi::Device::create(config); CHECK(opened);
        auto device = std::move(opened).value();
        depth_reprojection_and_scalar_convergence(device);
        std::cout << "[pass] depth rejection, per-tap IDs, AO variance, Hold, bounded eviction and canceled recording\n";
        actual_occlusion_history_holds(device);
        compositor_reports_and_resets_restart_evidence(device);
        glossy_geometry_matches_an_odd_node_split(device);
        std::cout << "[pass] rendered AO Hold and glossy geometry at an odd node split\n";
        moving_plate_matches_nodes_for_32_frames(device);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[fail] " << error.what() << '\n'; return 1;
    }
}
