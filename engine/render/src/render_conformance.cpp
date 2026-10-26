#include <mediaserver/render/render_conformance.hpp>
#include <mediaserver/render/hybrid_scene_renderer.hpp>
#include <mediaserver/render/output_transform.hpp>
#include <mediaserver/render/plate_depth_of_field.hpp>
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/bundle_identity.hpp>
#endif
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace mediaserver::render {
namespace {
using Pass = core::ConformancePass;
constexpr core::u32 width = 128U, height = 128U;
constexpr scene::CanvasRect whole{0U, 0U, width, height};
constexpr core::u64 hash_basis = 14695981039346656037ULL;
void hash_bytes(core::u64& hash, std::span<const core::u8> bytes) noexcept {
    for (const auto byte : bytes) { hash ^= byte; hash *= 1099511628211ULL; }
}
void hash_word(core::u64& hash, core::u32 word) noexcept {
    for (unsigned i = 0; i != 4; ++i) {
        const core::u8 byte = static_cast<core::u8>(word >> (i * 8U));
        hash_bytes(hash, {&byte, 1U});
    }
}
core::u32 pixel_size(rhi::TextureFormat format) noexcept {
    switch (format) {
    case rhi::TextureFormat::Rgba32Float: return 16U;
    case rhi::TextureFormat::Rgba16Float: return 8U;
    default: return 4U;
    }
}
scene::Scene fixture() {
    scene::Scene value{};
    value.id = doc::NodeId{1U}; value.document_version = doc::Version{1U};
    value.camera_source = doc::NodeId{2U}; value.canvas = {width, height};
    value.camera.eye = {0.0F, 0.0F, 4.0F}; value.camera.target = {0.0F, 0.0F, 0.0F};
    value.camera.near_plane = 0.1F; value.camera.far_plane = 30.0F;
    auto& settings = value.renderer_settings;
    settings.backend = scene::RendererBackend::Advanced; settings.required = true;
    settings.taau = true; settings.shadow_cascades = 4U;
    settings.occlusion_radius = 0.6F; settings.occlusion_strength = 0.7F;
    settings.contact_shadow_length = 0.35F; settings.contact_shadow_thickness = 0.05F;
    settings.shadow_blend_band = 0.12F; settings.shadow_filter_radius = 2.0F;
    settings.bloom_strength = 0.2F; settings.slice_apron_limit = 512U;
    settings.motion_blur = true; settings.motion_blur_radius = 12U;
    settings.texture_streaming = false; settings.texture_anisotropy = 8U;
    value.environment.ambient = 0.2F;
    value.environment.map_intensity = 0.65F;
    value.environment.fog_density = 0.045F;
    scene::MeshInstance box{};
    box.source = doc::NodeId{3U}; box.mesh = {scene::Primitive::Box, 1U};
    box.world = scene::rotation(0.07F, 0.09F, 0.0F); box.normal = scene::normal_basis(box.world);
    box.material.base_color = {0.8F, 0.25F, 0.08F}; box.material.roughness = 0.13F;
    box.material.metallic = 0.6F; box.material.emissive = {2.0F, 0.1F, 0.02F};
    value.instances.push_back(box);
    box.source = doc::NodeId{4U}; box.material.metallic = 0.0F; box.material.roughness = 0.7F;
    box.material.emissive = {}; box.material.base_color = {0.35F, 0.45F, 0.55F};
    box.world = scene::multiply(scene::translation({0.0F, 0.0F, -1.0F}), scene::scaling({7.0F, 7.0F, 0.1F}));
    box.normal = scene::normal_basis(box.world); value.instances.push_back(box);
    scene::DirectionalLight sun{}; sun.direction = {-0.4F, -0.6F, -0.8F};
    sun.intensity = 3.0F; sun.angular_radius = 0.015F;
    value.directional_lights.push_back(sun);
    scene::PointLight spot{}; spot.source = doc::NodeId{5U}; spot.kind = scene::LocalLightKind::Spot;
    spot.position = {1.0F, 0.6F, 2.0F}; spot.direction = {-0.3F, -0.2F, -1.0F};
    spot.intensity = 6.0F; spot.range = 12.0F; spot.casts_shadow = true;
    spot.ies_source = "compiled-conformance.ies"; spot.ies_rotation = 0.13F;
    spot.outer_cone = 0.14F; spot.inner_cone = 0.08F;
    value.point_lights.push_back(spot);
    scene::PointLight area;
    area.source = doc::NodeId{6}; area.kind = scene::LocalLightKind::Rectangle;
    area.position = {-1.2F, 2.0F, 1.0F}; area.direction = {0,-1,0};
    area.width = 1.8F; area.height = 0.7F; area.area_rotation = 0.07F;
    area.intensity = 120; area.unit = scene::LightUnit::Nits; area.range = 12;
    value.point_lights.push_back(area);
    area.source = doc::NodeId{7}; area.kind = scene::LocalLightKind::Sphere;
    area.position = {1.3F,0.7F,1.4F}; area.radius = 0.35F; area.color = {0.15F,0.3F,1};
    value.point_lights.push_back(area);
    return value;
}
RendererLimits fixture_limits() {
    RendererLimits limits{}; limits.max_instances = 16U; limits.max_vertices = 8192U;
    limits.max_indices = 32768U; limits.max_lights = 4U;
    limits.directional_shadow_resolution = 128U; limits.shadow_atlas = {128U, 32U, 64U, 128U};
    limits.max_history_slots = 2U; limits.max_temporal_history_targets = 1U;
    return limits;
}
struct PendingReadback final {
    Pass pass{};
    core::u32 width{}, height{}, mip{}, layer{}, format{}, frame{}, size{};
    rhi::Readback buffer;
};
} // namespace

// Privileged only to supply the compiled environment and read its products.
// The actual frame goes through HybridSceneRenderer's production ordering.
class ConformanceHarness final {
public:
    ConformanceHarness(rhi::Device& device, ConformanceRun& run, bool capture)
        : device_(device), run_(run), capture_(capture) { pending_.reserve(96U); }
    bool execute() {
        auto created = HybridSceneRenderer::create(device_, width, height, fixture_limits());
        if (!created) return false;
        auto renderer = std::move(created).value();
        // The Advanced renderer is built when a scene asks for it, and this
        // harness asks by reaching for it directly rather than through a
        // scene. Say so, or there is nothing here to compile the environment
        // into.
        renderer.ensure_advanced();
        if (!renderer.advanced_) return false;
        auto& raster = *renderer.advanced_;
        raster.conformance_ies_ = parse_ies_profile(
            "IESNA:LM-63-2002\nTILT=NONE\n1 1000 1 5 3 1 2 0 0 0 1 1 10\n"
            "0 30 60 90 180\n0 90 180\n100 90 40 0 0\n100 50 10 0 0\n100 20 0 0 0\n").profile;
        if (!raster.conformance_ies_) return false;
        auto output_result = OutputTransform::create(device_, width, height, true);
        if (!output_result) return false;
        auto output = std::move(output_result).value();
        auto dof_result = PlateDepthOfField::create(device_, width, height, rhi::TextureFormat::Rgba16Float);
        if (!dof_result) return false;
        auto dof = std::move(dof_result).value();
        output.set_display_lut(bake_display_lut(DisplayTransform::AcesApproximate, 33U));
        EnvironmentPrefilterLimits env_limits{};
        env_limits.face_size = 32U; env_limits.specular_mips = 4U;
        env_limits.irradiance_face_size = 16U; env_limits.brdf_extent = 32U;
        env_limits.specular_samples = 64U; env_limits.brdf_samples = 128U;
        auto prefiltered = EnvironmentPrefilter::create(device_, env_limits);
        if (!prefiltered) return false;
        raster.environment_ = std::move(prefiltered).value();
        DecodedFrame environment{};
        environment.width = 64U; environment.height = 32U;
        environment.format = rhi::TextureFormat::Rgba16Float;
        environment.pixels.resize(64U * 32U * 8U);
        // Half-float bit patterns avoid host libm, file decoders and local paths.
        for (core::u32 y = 0U; y < 32U; ++y) for (core::u32 x = 0U; x < 64U; ++x) {
            const std::array<core::u16, 4U> pixel{
                static_cast<core::u16>(0x3400U + x * 32U),
                static_cast<core::u16>(0x3000U + y * 64U),
                static_cast<core::u16>((x > 40U && x < 48U && y > 12U && y < 20U) ? 0x4400U : 0x3800U), 0x3c00U};
            std::memcpy(environment.pixels.data() + (y * 64U + x) * 8U, pixel.data(), 8U);
        }
        auto acquired = device_.acquire_commands(); if (!acquired) return false;
        auto commands = std::move(acquired).value();
        scene::PhysicalSky physical{};
        physical.enabled = true; physical.time_of_day = 17.25F;
        physical.latitude = 42; physical.north_rotation = 0.125F; physical.haze = 2.5F;
        run_.evidence.failed_pass = Pass::PhysicalSky;
        if (!raster.environment_->prepare(commands)
            || !raster.environment_->record_physical(commands, physical, 0.00314159265F)
            || !record(commands, Pass::PhysicalSky, *raster.environment_->scattering_table())
            || !record(commands, Pass::PhysicalIrradiance, *raster.environment_->maps().irradiance)
            || !record(commands, Pass::PhysicalSpecular, *raster.environment_->maps().specular)) return false;
        run_.evidence.failed_pass = Pass::Environment;
        if (!raster.environment_->prepare(commands) || !raster.adopt_environment(commands, environment, 0.125F)) return false;
        raster.environment_generation_ = 1U; raster.embedded_environment_ = true;
        const auto maps = raster.environment_->maps();
        if (!record(commands, Pass::Environment, raster.environment_->cube_)
            || !record(commands, Pass::Irradiance, *maps.irradiance)
            || !record(commands, Pass::Specular, *maps.specular)
            || !record(commands, Pass::Brdf, *maps.brdf) || !submit(std::move(commands), false)) return false;
        auto scene = fixture();
        ConformanceCapture tap{this, [](void* context, rhi::CommandList& cmd, Pass pass, const rhi::Texture& target) {
            return static_cast<ConformanceHarness*>(context)->record(cmd, pass, target);
        }};
        for (frame_ = 0U; frame_ < 16U; ++frame_) {
            scene.show_frame = frame_;
            scene.instances[0].world = scene::multiply(scene::translation({static_cast<float>(frame_) / 32.0F - 0.25F, 0.0F, 0.0F}), scene::rotation(0.07F, 0.09F, 0.0F));
            scene.instances[0].normal = scene::normal_basis(scene.instances[0].world);
            auto cmd_result = device_.acquire_commands(); if (!cmd_result) return false;
            auto cmd = std::move(cmd_result).value();
            auto request = renderer.convenience_request(scene, whole, {});
            request.conformance = &tap;
            run_.evidence.failed_pass = Pass::Lighting;
            auto products = renderer.render_frame(cmd, request);
            if (!products) return false;
            const auto& p = products.value();
            if (p.history.disposition != (frame_ == 0U ? HistoryDisposition::Initialize : HistoryDisposition::Advance)
                || raster.stats().directional_shadow_cascades != 4U
                || raster.stats().camera_instances < 2U) return false;
            if (p.passes.shadows != PassState::Active || p.passes.occlusion != PassState::Active
                || p.passes.temporal_antialiasing != PassState::Active || p.passes.fog != PassState::Active
                || p.passes.bloom != PassState::Active || p.passes.motion_blur != PassState::Active) return false;
            if (!record(cmd, Pass::Bloom, *p.color)) return false;
            // Qualify the production plate gather independently of the outer
            // scene passes. A near physical wall leaves measurable residual
            // blur on the virtual box, with a changing physical focus each frame.
            PlateLens lens{};
            lens.focal_length_m = 0.2F; lens.focus_distance_m = 1.0F + frame_ * 0.01F;
            lens.aperture_f_number = 0.5F; lens.eye = scene.camera.eye;
            lens.forward = {0, 0, -1}; lens.right = lens.wall_right = {1, 0, 0};
            lens.up = lens.wall_up = {0, 1, 0}; lens.wall_normal = {0, 0, 1};
            lens.wall_plane_distance = -3.0F; lens.region_width_m = lens.region_height_m = 2.48528147F;
            run_.evidence.failed_pass = Pass::PlateDof;
            if (!p.depth) return false;
            const auto blurred = dof.apply(cmd, *p.color, *p.depth, lens,
                scene::view_for_slice(scene.camera, scene.canvas, whole).view_projection, width, height, 12U);
            if (!blurred || !record(cmd, Pass::PlateDof, *blurred.value())) return false;
            scene::ColorSettings color{}; color.dither = false;
            output.color_settings_ = color;
            if (!output.render(cmd, *p.color, whole, true, whole)
                || !record(cmd, Pass::Display, output.target())) return false;
            color.dither = true; output.color_settings_ = color;
            if (!output.render(cmd, *p.color, whole, true, whole)
                || !record(cmd, Pass::Dither, output.target()) || !submit(std::move(cmd), true)) return false;
        }
        return true;
    }
private:
    bool record(rhi::CommandList& cmd, Pass pass, const rhi::Texture& target) noexcept {
        run_.evidence.failed_pass = pass;
        if (!capture_) return true;
        try {
            const auto& desc = target.desc();
            for (core::u32 layer = 0; layer < desc.array_layers; ++layer)
                for (core::u32 mip = 0; mip < desc.mip_levels; ++mip) {
                    const core::u32 w = std::max(1U, desc.width >> mip), h = std::max(1U, desc.height >> mip);
                    const core::u32 size = w * h * pixel_size(desc.format);
                    auto rb = device_.create_readback(size); if (!rb) return false;
                    pending_.push_back({pass, w, h, mip, layer, static_cast<core::u32>(desc.format), frame_, size, std::move(rb).value()});
                    if (!cmd.download(target, pending_.back().buffer, mip, layer)) return false;
                }
            return true;
        } catch (...) { return false; }
    }
    bool submit(rhi::CommandList commands, bool timed) {
        auto work = device_.submit(std::move(commands));
        if (!work || !work.value().wait()) return false;
        const auto elapsed = static_cast<core::u64>(work.value().gpu_execution().count());
        if (timed) run_.gpu_nanoseconds += elapsed;
        else run_.environment_gpu_nanoseconds += elapsed;
        for (auto& pending : pending_) {
            auto& hash = run_.evidence.passes[static_cast<std::size_t>(pending.pass)];
            if (!hash) hash = hash_basis;
            for (auto word : {pending.width, pending.height, pending.mip, pending.layer, pending.format, pending.frame, pending.size}) hash_word(hash, word);
            const auto mapped = pending.buffer.map();
            if (mapped.size() != pending.size) { pending.buffer.unmap(); return false; }
            hash_bytes(hash, mapped); pending.buffer.unmap();
        }
        pending_.clear(); return true;
    }
    rhi::Device& device_;
    ConformanceRun& run_;
    bool capture_{};
    core::u32 frame_{};
    std::vector<PendingReadback> pending_;
};

ConformanceRun run_render_conformance(rhi::Device& device, core::u64 request, bool capture_bytes) noexcept {
    ConformanceRun result{};
    auto& evidence = result.evidence;
    evidence.version = core::render_conformance_version; evidence.request = request;
    evidence.adapter = device.adapter_identity(); evidence.state = core::ConformanceState::Failed;
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    evidence.shader_bundle = shaders::bundle_sha256; evidence.strict_float = shaders::strict_float;
#endif
    try {
        if (device.capabilities().advanced_raster() && ConformanceHarness{device, result, capture_bytes}.execute()) {
            evidence.state = core::ConformanceState::Passed;
            evidence.failed_pass = Pass::Count;
            if (capture_bytes && !evidence.complete()) evidence.state = core::ConformanceState::Failed;
        }
    } catch (...) { evidence.state = core::ConformanceState::Failed; }
    return result;
}

struct RenderConformanceService::State final {
    std::mutex mutex;
    std::condition_variable wake;
    core::RenderConformance result{};
    core::RenderAdapterIdentity expected{};
    core::u64 request{};
    bool queued{}, stop{}, admitted{};
    std::atomic<core::u64> requests_coalesced{}, stale_results{};
    std::thread worker;
    State() : worker([this] { run(); }) {}
    ~State() {
        { const std::lock_guard lock{mutex}; stop = true; }
        wake.notify_one(); worker.join();
    }
    void run() noexcept {
        for (;;) {
            core::RenderAdapterIdentity adapter;
            core::u64 generation{};
            {
                std::unique_lock lock{mutex}; wake.wait(lock, [this] { return stop || queued; });
                if (stop) return;
                adapter = expected; generation = request; queued = false;
            }
            core::RenderConformance report{};
            report.state = core::ConformanceState::Failed; report.request = generation;
            report.adapter = adapter;
            try {
                rhi::DeviceConfig config{}; config.backend = rhi::BackendKind::D3d12;
                auto device = rhi::Device::create(config);
                if (device && device.value().adapter_identity() == adapter)
                    report = run_render_conformance(device.value(), generation).evidence;
            } catch (...) {}
            const std::lock_guard lock{mutex};
            if (generation == request && adapter == expected) result = report;
            else stale_results.fetch_add(1U, std::memory_order_relaxed);
        }
    }
};
RenderConformanceService::RenderConformanceService() : state_(std::make_unique<State>()) {}
RenderConformanceService::~RenderConformanceService() = default;
RenderConformanceService::Stats RenderConformanceService::stats() const noexcept {
    return {state_->requests_coalesced.load(std::memory_order_relaxed), state_->stale_results.load(std::memory_order_relaxed)};
}
void RenderConformanceService::poll(const core::RenderAdapterIdentity& adapter, core::u64 request,
                                    core::RenderConformance& result) noexcept {
    if (result.request != request || result.adapter != adapter) {
        result = {};
        result.adapter = adapter;
        result.request = request;
        result.state = core::ConformanceState::Running;
    }
    std::unique_lock lock{state_->mutex, std::try_to_lock}; if (!lock.owns_lock()) return;
    if (!state_->admitted || state_->expected != adapter || state_->request != request) {
        if (state_->queued) state_->requests_coalesced.fetch_add(1U, std::memory_order_relaxed);
        state_->expected = adapter; state_->request = request; state_->admitted = state_->queued = true;
        state_->result = {}; state_->result.adapter = adapter; state_->result.request = request;
        state_->result.state = core::ConformanceState::Running;
        state_->wake.notify_one();
    }
    result = state_->result;
}
} // namespace mediaserver::render
