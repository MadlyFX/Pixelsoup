#include <mediaserver/render/object_motion_blur.hpp>
#include <mediaserver/render/compositor_scene_executor.hpp>
#include <mediaserver/render/icvfx.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;
#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error(std::to_string(__LINE__) + ": " + #__VA_ARGS__); } while (false)

template<class T> std::span<const core::u8> bytes(const std::vector<T>& values) {
    return {reinterpret_cast<const core::u8*>(values.data()), values.size() * sizeof(T)};
}
rhi::CommandList acquire(rhi::Device& device) {
    auto result = device.acquire_commands(); CHECK(result); return std::move(result).value();
}
std::vector<core::u8> download(rhi::Device& device, rhi::CommandList cmd,
    const rhi::Texture& target, core::u32 stride) {
    auto rb = device.create_readback(target.width() * target.height() * stride); CHECK(rb);
    CHECK(cmd.download(target, rb.value()));
    auto submitted = device.submit(std::move(cmd)); CHECK(submitted); CHECK(submitted.value().wait());
    auto mapped = rb.value().map(); std::vector<core::u8> result(mapped.begin(), mapped.end());
    rb.value().unmap(); return result;
}
rhi::Texture texture(rhi::Device& device, core::u32 width, core::u32 height, rhi::TextureFormat format) {
    rhi::TextureDesc desc; desc.width = width; desc.height = height; desc.format = format;
    desc.sampled = desc.color_target = true;
    auto result = device.create_texture(desc); CHECK(result); return std::move(result).value();
}
render::RendererLimits limits() {
    render::RendererLimits result;
    result.max_instances = 16U; result.max_vertices = 1024U; result.max_indices = 4096U;
    result.directional_shadow_resolution = 16U; result.shadow_atlas.extent = 32U;
    result.max_history_slots = 4U; result.max_temporal_history_targets = 2U;
    return result;
}
scene::Scene fixture() {
    scene::Scene frame; frame.id = doc::NodeId{1U}; frame.camera_source = doc::NodeId{2U};
    frame.canvas = {128U, 64U}; frame.show_rate = {24U, 1U};
    frame.camera.eye = {0, 0, 1}; frame.camera.target = {0, 0, 0};
    frame.camera.field_of_view = scene::Turns{0.25F};
    frame.camera.near_plane = 0.1F; frame.camera.far_plane = 20.0F;
    frame.renderer_settings.backend = scene::RendererBackend::Advanced;
    frame.renderer_settings.required = true; frame.renderer_settings.motion_blur = true;
    frame.renderer_settings.motion_blur_radius = 16U;
    frame.renderer_settings.shadow_cascades = 0U; frame.renderer_settings.gi_strength = 0.0F;
    frame.environment.ambient = 0; frame.environment.sky_color = frame.environment.ground_color = {0, 0, 0};
    scene::MeshInstance object; object.source = doc::NodeId{3U};
    object.mesh = {scene::Primitive::Quad, 1U};
    object.world = scene::scaling({0.25F, 0.75F, 1.0F}); object.normal = scene::normal_basis(object.world);
    object.material.base_color = {0, 0, 0}; object.material.emissive = {2, 1, 0.25F};
    frame.instances.push_back(object); return frame;
}

void authored_contract() {
    auto frame = fixture();
    auto settings = frame.renderer_settings;
    settings.motion_blur_radius = 23U;
    doc::Node node; node.id = doc::NodeId{1U}; node.type = std::string{scene::schema::settings_type};
    scene::write_renderer_settings(node, settings);
    scene::SceneModel model; model.reset(doc::Root{}.set(node));
    CHECK(model.renderer_settings() == settings);
    const auto evaluated = model.evaluate({7U, 24000U, 1001U});
    CHECK(evaluated.show_rate == scene::MediaFrameRate{24000U, 1001U});
    CHECK(evaluated.show_frame == 7U);
    for (int field = 0; field < 2; ++field) {
        auto changed = frame;
        if (field == 0) changed.renderer_settings.motion_blur = false;
        else changed.renderer_settings.motion_blur_radius = 23U;
        CHECK(scene::digest(changed) != scene::digest(frame));
        CHECK(scene::renderer_settings_digest(changed.renderer_settings) != scene::renderer_settings_digest(frame.renderer_settings));
        CHECK(render::renderer_profile_digest(changed.renderer_settings) != render::renderer_profile_digest(frame.renderer_settings));
    }
    settings.motion_blur_radius = 999U; CHECK(scene::clamped(settings).motion_blur_radius == 64U);
    auto changed = frame; changed.show_rate = {48U, 1U}; CHECK(scene::digest(changed) != scene::digest(frame));
    CHECK(std::abs(render::object_shutter_fraction(frame) - 0.5F) < 1.0e-6F);
    render::SceneRenderView view; view.shutter_seconds = 1.0F / 96.0F;
    CHECK(std::abs(render::object_shutter_fraction(frame, &view) - 0.25F) < 1.0e-6F);
    frame.show_rate = {24000U, 1001U}; frame.camera.shutter_seconds = 1001.0F / 48000.0F;
    CHECK(std::abs(render::object_shutter_fraction(frame) - 0.5F) < 1.0e-6F);
    frame.camera.shutter_seconds = 1.0F; CHECK(render::object_shutter_fraction(frame) == 1.0F);
    frame.show_rate.denominator = 0U; CHECK(render::object_shutter_fraction(frame) == 0.0F);
    const auto apron = render::apron_requirements(frame);
    CHECK(apron.motion_blur == 16U && apron.maximum() == 16U);
    CHECK(!render::padded_slice(frame.canvas, {32U, 0U, 64U, 64U}, apron, 15U));
    frame.renderer_settings.taau = true;
    CHECK(render::apron_requirements(frame).maximum() == 18U);
    auto plate = render::padded_view_for(frame, 128U, 64U, true); CHECK(plate && plate.value().apron == 18U);
    scene::TrackedCameraView camera;
    camera.camera = doc::NodeId{11U}; camera.source = doc::NodeId{12U};
    camera.authored = frame.camera; camera.authored.shutter_seconds = 1.0F / 96.0F;
    camera.icvfx.inner_frustum = true; camera.pose.valid = true;
    camera.pose.position = {0, 0, 1}; camera.pose.field_of_view = scene::Turns{0.25F};
    camera.pose.aspect_ratio = 2;
    frame.tracked_cameras.push_back(camera);
    frame.renderer_settings.icvfx.inner_frustum = true;
    const mediaserver::output::PhysicalSurface wall{{}, {1, 0, 0}, {0, 1, 0}, 4, 2, frame.canvas};
    const auto plan = render::plan_inner_frustum(frame, camera, wall, scene::whole(frame.canvas),
        {scene::whole(frame.canvas), true, false});
    const auto tracked = render::inner_frustum_view(frame, camera, wall, plan);
    CHECK(tracked && tracked->shutter_seconds == camera.authored.shutter_seconds);
    CHECK(tracked->shutter_seconds != frame.camera.shutter_seconds);
}

void shutter_reconstruction(rhi::Device& device) {
    constexpr core::u32 width = 96U, height = 16U;
    auto color = texture(device, width, height, rhi::TextureFormat::Rgba32Float);
    auto motion = texture(device, width, height, rhi::TextureFormat::Rgba32Float);
    auto made = render::ObjectMotionBlur::create(device, width, height, rhi::TextureFormat::Rgba32Float); CHECK(made);
    auto blur = std::move(made).value();
    std::vector<std::array<float, 4>> colors(width * height, {0, 0, 0, 0});
    std::vector<std::array<float, 4>> velocities(width * height, {0, 0, 0, 0});
    for (core::u32 y = 0; y < height; ++y) for (core::u32 x = 40; x < 48; ++x) {
        colors[y * width + x] = {4, 2, 1, 1};
        velocities[y * width + x] = {16.0F / width, 0, 0.3F, 0.3F};
    }
    const auto draw = [&](float shutter, core::u32 radius) {
        auto cmd = acquire(device); CHECK(cmd.upload(color, bytes(colors))); CHECK(cmd.upload(motion, bytes(velocities)));
        auto result = blur.apply(cmd, color, motion, shutter, radius, width, height); CHECK(result);
        const auto data = download(device, std::move(cmd), *result.value(), 16U);
        std::vector<std::array<float, 4>> pixels(width * height);
        std::memcpy(pixels.data(), data.data(), data.size()); return pixels;
    };
    CHECK(draw(0, 16U) == colors); CHECK(draw(0.5F, 0U) == colors);
    for (const float shutter : {0.25F, 0.5F, 1.0F}) {
        const auto pixels = draw(shutter, 16U); CHECK(pixels == draw(shutter, 16U));
        core::u32 left = width, right = 0; float energy = 0;
        for (core::u32 x = 0; x < width; ++x) {
            const auto& p = pixels[8U * width + x];
            if (p[0] > 0) { left = std::min(left, x); right = x; }
            energy += p[0];
            CHECK(std::abs(p[0] - 4.0F * p[3]) < 1.0e-5F);
        }
        // Eight pixels of geometry plus the exposed part of its 16 px motion.
        CHECK(right - left + 1U == 8U + static_cast<core::u32>(16.0F * shutter));
        CHECK(std::abs(energy - 32.0F) < 0.001F);
    }
    auto clamped = draw(1, 3U);
    CHECK(clamped[8U * width + 36U][0] == 0 && clamped[8U * width + 37U][0] > 0);
    // A near static strip stays intact when the moving emitter passes behind it.
    for (core::u32 y = 0; y < height; ++y) for (core::u32 x = 49; x < 53; ++x) {
        colors[y * width + x] = {0, 0.75F, 0, 1}; velocities[y * width + x] = {0, 0, 0.8F, 0.8F};
    }
    const auto occluded = draw(1, 16U);
    for (core::u32 x = 49; x < 53; ++x) CHECK(occluded[8U * width + x] == colors[8U * width + x]);
    // An abandoned stateless recording consumes no motion or filter history.
    { auto cmd = acquire(device); CHECK(blur.apply(cmd, color, motion, 0.5F, 16U, width, height)); }
    CHECK(draw(0.5F, 16U) == draw(0.5F, 16U));
}

void camera_motion_is_not_blurred(rhi::Device& device) {
    auto made = render::HybridSceneRenderer::create(device, 128U, 64U, limits()); CHECK(made);
    auto renderer = std::move(made).value();
    auto raw_made = render::HybridSceneRenderer::create(device, 128U, 64U, limits()); CHECK(raw_made);
    auto raw = std::move(raw_made).value();
    auto frame = fixture();
    doc::Node authored{doc::NodeId{10U}, "scene.settings", {}};
    scene::write_renderer_settings(authored, frame.renderer_settings);
    doc::Node transport{doc::NodeId{20U}, "show.transport", {{"transport_state", std::int64_t{0}}}};
    auto root = doc::Root{}.set(authored).set(transport);
    scene::SceneModel model;
    const auto adopt = [&] {
        model.reset(root); // Playback rebuilds the model at a new revision.
        const auto evaluated = model.evaluate({frame.show_frame, 24U, 1U});
        frame.document_version = evaluated.document_version;
        frame.temporal_document_digest = evaluated.temporal_document_digest;
    };
    adopt();
    const auto draw = [&](render::HybridSceneRenderer& r, bool enabled) {
        frame.renderer_settings.motion_blur = enabled;
        auto cmd = acquire(device); auto request = r.convenience_request(frame, scene::whole(frame.canvas), {});
        auto result = r.render_frame(cmd, request); CHECK(result);
        return download(device, std::move(cmd), *result.value().color, 8U);
    };
    for (core::u64 n = 0; n < 6; ++n) {
        frame.show_frame = n;
        frame.camera.eye.x = frame.camera.target.x = static_cast<float>(n) * 0.0625F;
        const auto blurred = draw(renderer, true); CHECK(blurred == draw(raw, false));
        CHECK(blurred == draw(renderer, true));
        CHECK(renderer.last_products().history.disposition == render::HistoryDisposition::Hold);
    }
    const auto object = download(device, acquire(device), *renderer.last_products().object_velocity, 16U);
    const auto camera = download(device, acquire(device), *renderer.last_products().velocity, 16U);
    bool found_camera = false;
    for (std::size_t i = 0; i < object.size(); i += 16U) {
        std::array<float, 4> o{}, c{};
        std::memcpy(o.data(), object.data() + i, 16U); std::memcpy(c.data(), camera.data() + i, 16U);
        CHECK(o[0] == 0 && o[1] == 0);
        found_camera = found_camera || std::abs(c[0]) > 0.001F;
    }
    CHECK(found_camera);
    // A moving object under that same moving camera retains its object motion.
    frame.show_frame++;
    frame.instances[0].world = scene::multiply(scene::translation({0.5F, 0, 0}), scene::scaling({0.25F, 0.75F, 1}));
    frame.instances[0].normal = scene::normal_basis(frame.instances[0].world);
    const auto streak = draw(renderer, true); CHECK(streak != draw(raw, false));
    CHECK(streak == draw(renderer, true));
    // Native Pause is a document edit. Its held show frame must retain the
    // previous transform pair even when the entire SceneModel is rebuilt.
    const auto identity = frame.temporal_document_digest;
    const auto revision = frame.document_version;
    transport.properties["transport_state"] = std::int64_t{1};
    const auto paused = root.set(transport);
    model.apply(doc::diff(root, paused));
    CHECK(model.evaluate({frame.show_frame, 24U, 1U}).temporal_document_digest == identity);
    root = paused; adopt();
    CHECK(frame.document_version != revision && frame.temporal_document_digest == identity);
    CHECK(streak == draw(renderer, true));
    CHECK(renderer.last_products().history.disposition == render::HistoryDisposition::Hold);
    // Play can arrive with the next ordinal; it must advance rather than cut.
    transport.properties["transport_state"] = std::int64_t{0};
    root = root.set(transport); ++frame.show_frame; adopt();
    frame.instances[0].world = scene::multiply(scene::translation({0.75F, 0, 0}), scene::scaling({0.25F, 0.75F, 1}));
    frame.instances[0].normal = scene::normal_basis(frame.instances[0].world);
    CHECK(draw(renderer, true) != draw(raw, false));
    CHECK(renderer.last_products().history.disposition == render::HistoryDisposition::Advance);
    // A real authored edit, including a future animation/calibration edit,
    // still cuts history even when the currently evaluated geometry agrees.
    authored.properties["future_key"] = 0.375;
    root = root.set(authored); adopt();
    CHECK(frame.temporal_document_digest != identity);
    CHECK(draw(renderer, true) == draw(raw, false));
    CHECK(renderer.last_products().history.disposition == render::HistoryDisposition::Reset);
    // Seeking breaks the motion pair and cannot smear a teleported object.
    frame.show_frame = 70U;
    CHECK(draw(renderer, true) == draw(raw, false));
    CHECK(renderer.last_products().history.disposition == render::HistoryDisposition::Reset);
}

void compositor_slices_and_holds(rhi::Device& device) {
    const auto make = [&](core::u32 w) {
        auto made = render::CompositorSceneExecutor::create(device, w, 64U, limits(), {}, rhi::TextureFormat::Rgba16Float);
        CHECK(made); return std::move(made).value();
    };
    auto full = make(128U), left = make(63U), right = make(65U);
    auto frame = fixture();
    const auto draw = [&](render::CompositorSceneExecutor& renderer, scene::CanvasRect slice) {
        auto cmd = acquire(device); auto result = renderer.execute(cmd, frame, slice); CHECK(result);
        CHECK(renderer.renderer_passes().motion_blur == render::PassState::Active);
        return download(device, std::move(cmd), *result.value(), 8U);
    };
    for (core::u64 n = 0; n < 12; ++n) {
        frame.show_frame = n;
        const float x = static_cast<float>(static_cast<int>(n % 5U) - 2) * 0.25F;
        frame.instances[0].world = scene::multiply(scene::translation({x, 0, 0}), scene::scaling({0.25F, 0.75F, 1}));
        frame.instances[0].normal = scene::normal_basis(frame.instances[0].world);
        const auto pixels = draw(full, {0, 0, 128, 64});
        const auto a = draw(left, {0, 0, 63, 64}), b = draw(right, {63, 0, 65, 64});
        for (std::size_t y = 0; y < 64U; ++y) {
            CHECK(std::equal(a.begin() + y * 63U * 8U, a.begin() + (y + 1U) * 63U * 8U, pixels.begin() + y * 128U * 8U));
            CHECK(std::equal(b.begin() + y * 65U * 8U, b.begin() + (y + 1U) * 65U * 8U, pixels.begin() + (y * 128U + 63U) * 8U));
        }
        CHECK(pixels == draw(full, {0, 0, 128, 64}));
    }
    // A blurred layer's coverage is no longer its geometry's, so it merges as
    // a still occluder and composites sharp in time - but the frame renders,
    // and the rest of the scene keeps its motion blur. Refusing the whole
    // frame for one layer's look took the wall down.
    frame.instances[0].style.effects.blur_enabled = true;
    frame.instances[0].style.effects.blur_radius = 3;
    auto cmd = acquire(device); auto result = full.execute(cmd, frame, scene::CanvasRect{0, 0, 128, 64});
    CHECK(result);
    CHECK(full.renderer_passes().motion_blur == render::PassState::Active);
    CHECK(full.renderer_passes().motion_blur_reason
          == std::string_view{"blurred, masked or matted layers are not motion blurred"});
    static_cast<void>(download(device, std::move(cmd), *result.value(), 8U));
    // A blend mode leaves coverage as drawn: that layer keeps real motion.
    frame.instances[0].style.effects.blur_enabled = false;
    frame.instances[0].style.blend = scene::BlendMode::Screen;
    cmd = acquire(device); result = full.execute(cmd, frame, scene::CanvasRect{0, 0, 128, 64});
    CHECK(result);
    CHECK(full.renderer_passes().motion_blur == render::PassState::Active);
    CHECK(full.renderer_passes().motion_blur_reason.empty());
    static_cast<void>(download(device, std::move(cmd), *result.value(), 8U));
}

int main() {
    try {
        authored_contract(); std::cout << "[pass] authored radius, cadence, shutter, digests and apron\n";
        for (const auto backend : {rhi::BackendKind::SdlGpu, rhi::BackendKind::D3d12}) {
            if (!rhi::Device::available(backend)) { std::cout << "[skip] unavailable GPU backend\n"; continue; }
            rhi::DeviceConfig config; config.backend = backend; config.debug = true;
            auto opened = rhi::Device::create(config); CHECK(opened); auto device = std::move(opened).value();
            shutter_reconstruction(device);
            std::cout << "[pass] shutter span, HDR energy, alpha, radius, occlusion and cancellation: " << static_cast<int>(backend) << '\n';
            if (device.capabilities().advanced_raster()) {
                camera_motion_is_not_blurred(device);
                compositor_slices_and_holds(device);
                std::cout << "[pass] camera exclusion, object motion, resets, holds and exact odd node slices\n";
            }
        }
    } catch (const std::exception& e) { std::cerr << "[fail] " << e.what() << '\n'; return 1; }
}
