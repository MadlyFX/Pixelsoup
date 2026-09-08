#include <mediaserver/render/plate_depth_of_field.hpp>
#include <mediaserver/render/icvfx_stage.hpp>
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
namespace output = mediaserver::output;
#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error(std::to_string(__LINE__) + ": " + #__VA_ARGS__); } while (false)
template<class T> std::span<const core::u8> bytes(const std::vector<T>& v) {
    return {reinterpret_cast<const core::u8*>(v.data()), v.size() * sizeof(T)};
}
rhi::CommandList acquire(rhi::Device& d) { auto c = d.acquire_commands(); CHECK(c); return std::move(c).value(); }
rhi::Texture texture(rhi::Device& d, core::u32 w, core::u32 h, rhi::TextureFormat f) {
    rhi::TextureDesc desc{}; desc.width = w; desc.height = h; desc.format = f;
    desc.sampled = desc.color_target = true;
    auto t = d.create_texture(desc); CHECK(t); return std::move(t).value();
}
std::vector<core::u8> download(rhi::Device& d, rhi::CommandList cmd, const rhi::Texture& t, core::u32 stride) {
    auto rb = d.create_readback(t.width() * t.height() * stride); CHECK(rb);
    CHECK(cmd.download(t, rb.value())); auto work = d.submit(std::move(cmd)); CHECK(work); CHECK(work.value().wait());
    auto mapped = rb.value().map(); std::vector<core::u8> result(mapped.begin(), mapped.end()); rb.value().unmap(); return result;
}
render::PlateLens lens() {
    render::PlateLens p{}; p.focal_length_m = 0.1F; p.focus_distance_m = 1.1F; p.aperture_f_number = 2;
    p.forward = {0, 0, -1}; p.right = p.wall_right = {1, 0, 0}; p.up = p.wall_up = {0, 1, 0};
    p.wall_normal = {0, 0, 1}; p.wall_plane_distance = -2.2F;
    p.region_width_m = 0.2F; p.region_height_m = 0.1F; return p;
}
render::RendererLimits limits() {
    render::RendererLimits l{}; l.max_instances = 16; l.max_vertices = 1024; l.max_indices = 4096;
    l.directional_shadow_resolution = 16; l.shadow_atlas.extent = 32;
    l.max_history_slots = 4; l.max_temporal_history_targets = 2; return l;
}
void authored_and_optical_contract() {
    scene::RendererSettings settings{}; settings.backend = scene::RendererBackend::Advanced;
    settings.plate_depth_of_field = true; settings.plate_dof_radius = 23;
    doc::Node n{}; n.id = doc::NodeId{1}; n.type = std::string{scene::schema::settings_type};
    scene::write_renderer_settings(n, settings);
    scene::SceneModel model; model.reset(doc::Root{}.set(n)); CHECK(model.renderer_settings() == settings);
    scene::Scene s{}; s.canvas = {128, 64}; s.renderer_settings = settings;
    for (int field = 0; field < 2; ++field) {
        auto changed = s;
        if (field == 0) changed.renderer_settings.plate_depth_of_field = false;
        else changed.renderer_settings.plate_dof_radius = 7;
        CHECK(scene::digest(s) != scene::digest(changed));
        CHECK(scene::renderer_settings_digest(settings) != scene::renderer_settings_digest(changed.renderer_settings));
        CHECK(render::renderer_profile_digest(settings) != render::renderer_profile_digest(changed.renderer_settings));
    }
    settings.plate_dof_radius = 999; CHECK(scene::clamped(settings).plate_dof_radius == 64);
    CHECK(render::apron_requirements(s).maximum() == 0); // outer output unaffected
    auto p = render::padded_view_for(s, 128, 64, true, true); CHECK(p && p.value().apron == 24);
    CHECK(render::padded_view_for(s, 128, 64, true).value().apron == 0);
    s.renderer_settings.slice_apron_limit = 23; CHECK(!render::padded_view_for(s, 128, 64, true, true));
    s.renderer_settings.slice_apron_limit = 512; s.renderer_settings.taau = true;
    s.renderer_settings.motion_blur = true; s.renderer_settings.motion_blur_radius = 8;
    CHECK(render::padded_view_for(s, 128, 64, true, true).value().apron == 34);
    auto l = lens(); CHECK(render::valid(l));
    // Independent thin-lens example: f=100 mm, N=2, focus=1.1 m,
    // wall=2.2 m and object=4.4 m -> residual sensor radius 0.625 mm.
    CHECK(std::abs(render::residual_coc_radius_m(l, 4.4F, 2.2F) - 0.000625F) < 1e-9F);
    CHECK(render::residual_coc_radius_m(l, 2.2F, 2.2F) == 0);
    CHECK(render::residual_coc_radius_m(l, 1.1F, 2.2F) == 0);
    CHECK(std::abs(render::residual_coc_radius_m(l, std::numeric_limits<float>::infinity(), 2.2F) - 0.00125F) < 1e-9F);
    l.aperture_f_number = 4;
    CHECK(std::abs(render::residual_coc_radius_m(l, 4.4F, 2.2F) - 0.0003125F) < 1e-9F);
    l.focus_distance_m = l.focal_length_m; CHECK(!render::valid(l));
    scene::TrackedCameraView tracked{}; tracked.camera = doc::NodeId{2}; tracked.source = doc::NodeId{3};
    tracked.pose.valid = true; tracked.pose.focal_length_mm = 85; tracked.pose.focus_distance_m = 4.75F;
    tracked.pose.aperture_f_number = 1.8F; s.tracked_cameras.push_back(tracked);
    for (int field = 0; field < 3; ++field) {
        auto changed = s;
        if (field == 0) changed.tracked_cameras[0].pose.focal_length_mm = 50;
        if (field == 1) changed.tracked_cameras[0].pose.focus_distance_m = 2;
        if (field == 2) changed.tracked_cameras[0].pose.aperture_f_number = 4;
        CHECK(scene::digest(changed) != scene::digest(s));
    }
}

void predicted_gpu_radius(rhi::Device& d) {
    constexpr core::u32 w = 128, h = 64;
    auto made = render::PlateDepthOfField::create(d, w, h, rhi::TextureFormat::Rgba32Float); CHECK(made);
    auto filter = std::move(made).value(); auto l = lens();
    auto color = texture(d, w, h, rhi::TextureFormat::Rgba32Float);
    auto depth = texture(d, w, h, rhi::TextureFormat::R32Float);
    output::PhysicalSurface wall{{0, 0, -2.2F}, {1, 0, 0}, {0, 1, 0}, 0.2F, 0.1F, {w, h}};
    auto built = output::perspective_view_for(wall, {0, 0, w, h}, {}, 0.1F, 100); CHECK(built);
    auto vp = built.value().matrices.view_projection;
    std::vector<std::array<float, 4>> colors(w * h, {0, 0, 0, 0});
    for (core::u32 y = 0; y < h; ++y) colors[y * w + 64] = {4, 2, 1, 0.5F};
    std::vector<float> depths(w * h);
    const auto set_depth = [&](float z) {
        auto clip = scene::transform(vp, {0, 0, -z, 1}); std::fill(depths.begin(), depths.end(), clip.z / clip.w);
    };
    set_depth(4.4F);
    const auto draw = [&](core::u32 radius) {
        auto cmd = acquire(d); CHECK(cmd.upload(color, bytes(colors))); CHECK(cmd.upload(depth, bytes(depths)));
        auto result = filter.apply(cmd, color, depth, l, vp, w, h, radius); CHECK(result);
        auto raw = download(d, std::move(cmd), *result.value(), 16);
        std::vector<std::array<float, 4>> pixels(w * h); std::memcpy(pixels.data(), raw.data(), raw.size()); return pixels;
    };
    CHECK(draw(0) == colors);
    const auto blurred = draw(32); CHECK(blurred != colors && draw(32) == blurred);
    // Sensor 0.625 mm * wall/f 22 * 640 pixels/metre = 8.8 px radius.
    core::u32 left = w, right = 0; float energy = 0;
    for (core::u32 x = 0; x < w; ++x) {
        const auto pixel = blurred[32 * w + x];
        if (pixel[0] > 1e-6F) { left = std::min(left, x); right = x; }
        energy += pixel[0]; CHECK(std::abs(pixel[0] - pixel[3] * 8) < 1e-5F);
    }
    CHECK(left == 55 && right == 73); CHECK(std::abs(energy - 4) < 1e-4F);
    const auto capped = draw(3); CHECK(capped[32 * w + 60][0] == 0 && capped[32 * w + 61][0] > 0);
    l.aperture_f_number = 4;
    const auto stopped = draw(32); CHECK(stopped[32 * w + 59][0] == 0 && stopped[32 * w + 60][0] > 0);
    l = lens(); set_depth(2.2F); CHECK(draw(32) == colors);
    l.focus_distance_m = 2.2F; CHECK(draw(32) == colors);
    set_depth(4.4F); CHECK(draw(32) != colors); // focusing on the wall does not focus virtual background
    l = lens(); set_depth(1.1F); CHECK(draw(32) == colors); // physical wall blur is already larger
    set_depth(4.4F);
    for (core::u32 y = 0; y < h; ++y) for (core::u32 x = 66; x < 70; ++x) {
        auto clip = scene::transform(vp, {0, 0, -1.1F, 1}); depths[y * w + x] = clip.z / clip.w;
        colors[y * w + x] = {0, 3, 0, 1};
    }
    const auto occluded = draw(32);
    for (core::u32 x = 66; x < 70; ++x) CHECK(occluded[32 * w + x] == colors[32 * w + x]);
    { auto cmd = acquire(d); CHECK(filter.apply(cmd, color, depth, l, vp, w, h, 32)); }
    CHECK(draw(32) == occluded);
    // A 30-degree wall projects a sensor disk into an ellipse: horizontal
    // radius is 8.8/cos(30)=10.16 pixels, vertical remains 8.8 pixels.
    l = lens(); l.wall_right = {0.8660254F, 0, -0.5F}; l.wall_normal = {0.5F, 0, 0.8660254F};
    wall.right = l.wall_right; l.wall_plane_distance = -2.2F * 0.8660254F;
    built = output::perspective_view_for(wall, {0,0,w,h}, {}, 0.1F, 100); CHECK(built);
    vp = built.value().matrices.view_projection;
    const auto inverse = scene::invert(vp);
    std::fill(colors.begin(), colors.end(), std::array<float,4>{}); colors[32*w+64] = {4,2,1,0.5F};
    for (core::u32 y = 0; y < h; ++y) for (core::u32 x = 0; x < w; ++x) {
        const auto ray = scene::transform(inverse, {(x + 0.5F)/w*2-1, 1-(y+0.5F)/h*2, 1, 1});
        const auto clip = scene::transform(vp, {ray.x/(-ray.z)*4.4F, ray.y/(-ray.z)*4.4F, -4.4F, 1});
        depths[y*w+x] = clip.z/clip.w;
    }
    const auto tilted = draw(32);
    CHECK(tilted[32*w+54][0] > 0 && tilted[32*w+53][0] == 0);
    CHECK(tilted[23*w+64][0] > 0 && tilted[22*w+64][0] == 0);
    l.focus_distance_m = 0; auto cmd = acquire(d); CHECK(!filter.apply(cmd, color, depth, l, vp, w, h, 32));
}

void plates_and_seams(rhi::Device& d) {
    constexpr scene::Canvas canvas{128, 64};
    const output::PhysicalSurface wall{{}, {1, 0, 0}, {0, 1, 0}, 1, 0.5F, canvas};
    const std::array<scene::CanvasRect, 4> slices{{{0,0,128,64}, {0,0,63,64}, {63,0,65,64}, {0,0,128,64}}};
    std::vector<render::IcvfxStage> stages; std::vector<rhi::Texture> outer;
    for (auto slice : slices) {
        auto stage = render::IcvfxStage::create(d, slice.width, slice.height, limits(), {}, rhi::TextureFormat::Rgba16Float);
        CHECK(stage); stages.push_back(std::move(stage).value());
        outer.push_back(texture(d, slice.width, slice.height, rhi::TextureFormat::Rgba16Float));
    }
    scene::Scene s{}; s.id = doc::NodeId{1}; s.camera_source = doc::NodeId{2}; s.canvas = canvas;
    s.camera.eye = {0,0,2.2F}; s.camera.target = {};
    s.renderer_settings.backend = scene::RendererBackend::Advanced; s.renderer_settings.required = true;
    s.renderer_settings.plate_depth_of_field = true; s.renderer_settings.plate_dof_radius = 12;
    s.renderer_settings.icvfx.inner_frustum = true; s.renderer_settings.shadow_cascades = 0;
    s.environment.ambient = 0; s.environment.sky_color = s.environment.ground_color = {};
    scene::TrackedCameraView camera{}; camera.camera = doc::NodeId{2}; camera.source = doc::NodeId{9};
    camera.authored = s.camera; camera.icvfx.inner_frustum = true; camera.icvfx.soft_edge_px = 0;
    camera.pose.valid = true; camera.pose.position = s.camera.eye; camera.pose.focal_length_mm = 100;
    camera.pose.focus_distance_m = 1.1F; camera.pose.aperture_f_number = 2;
    s.tracked_cameras.push_back(camera);
    scene::MeshInstance bar{}; bar.source = doc::NodeId{3}; bar.mesh = {scene::Primitive::Quad, 1};
    bar.material.base_color = {}; bar.material.emissive = {4,2,1};
    s.instances.push_back(bar);
    scene::InnerFrustumRegion region{{32, 12, 64, 40}, true, false};
    const auto draw = [&](std::size_t node) {
        auto cmd = acquire(d); CHECK(cmd.clear(outer[node], {0.125F,0.25F,0.5F,1}));
        auto result = stages[node].record(cmd, s, wall, slices[node], {&region,1}, outer[node]); CHECK(result);
        return download(d, std::move(cmd), *result, 8);
    };
    core::u32 changed = 0;
    for (core::u32 frame = 0; frame < 8; ++frame) {
        s.show_frame = frame;
        s.tracked_cameras[0].pose.position.x = static_cast<float>(frame) * 0.002F;
        s.tracked_cameras[0].pose.yaw = static_cast<float>(frame) * 0.0005F;
        s.tracked_cameras[0].pose.focus_distance_m = 1.1F + 0.05F * frame;
        s.instances[0].world = scene::multiply(scene::translation({0.01F * frame, 0, -2.2F}), scene::scaling({0.025F, 1, 1}));
        s.instances[0].normal = scene::normal_basis(s.instances[0].world);
        const auto full = draw(0), a = draw(1), b = draw(2);
        CHECK(full == draw(0));
        for (core::u32 y = 0; y < canvas.height; ++y) {
            CHECK(std::equal(a.begin()+y*63*8, a.begin()+(y+1)*63*8, full.begin()+y*128*8));
            CHECK(std::equal(b.begin()+y*65*8, b.begin()+(y+1)*65*8, full.begin()+(y*128+63)*8));
        }
        CHECK(stages[0].inner()->renderer_passes().plate_dof == render::PassState::Active);
        s.renderer_settings.plate_depth_of_field = false;
        const auto sharp = draw(3); if (sharp != full) ++changed;
        // Pixels outside the plate retain the identical outer image.
        CHECK(std::equal(sharp.begin(), sharp.begin() + 128*8, full.begin()));
        s.renderer_settings.plate_depth_of_field = true;
    }
    CHECK(changed == 8);
    // Ordinary Program views never execute this plate-only pass.
    auto executor_result = render::CompositorSceneExecutor::create(d, 128, 64, limits(), {}, rhi::TextureFormat::Rgba16Float);
    CHECK(executor_result); auto executor = std::move(executor_result).value();
    const auto ordinary = [&](bool enabled) {
        s.renderer_settings.plate_depth_of_field = enabled;
        auto cmd = acquire(d); auto result = executor.execute(cmd, s, scene::whole(canvas)); CHECK(result);
        return download(d, std::move(cmd), *result.value(), 8);
    };
    const auto outer_sharp = ordinary(false); CHECK(ordinary(true) == outer_sharp);
    CHECK(executor.renderer_passes().plate_dof == render::PassState::Idle && executor.stats().plate_dof_passes == 0);
    // Physical controls beat authored fallback; raw encoder counts are unrelated.
    auto plan = render::plan_inner_frustum(s, s.tracked_cameras[0], wall, slices[0], region);
    auto view = render::inner_frustum_view(s, s.tracked_cameras[0], wall, plan); CHECK(view && view->plate_lens);
    CHECK(view->plate_lens->focal_length_m == 0.1F && view->plate_lens->aperture_f_number == 2);
    auto& tracked = s.tracked_cameras[0]; tracked.pose.focus_distance_m = 0;
    tracked.authored.focus_distance_m = 1.75F; tracked.pose.focal_length_mm = 0;
    tracked.authored.lens_profile = scene::LensProfile{}; tracked.authored.lens_profile->focal_length_mm = 85;
    view = render::inner_frustum_view(s, tracked, wall, plan);
    CHECK(view->plate_lens->focus_distance_m == 1.75F && std::abs(view->plate_lens->focal_length_m - 0.085F) < 1e-8F);
    tracked.authored.focus_distance_m = 0;
    auto cmd = acquire(d); CHECK(!stages[0].record(cmd, s, wall, slices[0], {&region,1}, outer[0]));
    CHECK(stages[0].inner()->renderer_passes().plate_dof == render::PassState::Refused);
    CHECK(stages[0].inner()->stats().plate_dof_refusals == 1);
    tracked.authored.focus_distance_m = 1.75F; s.renderer_settings.slice_apron_limit = 0;
    cmd = acquire(d); CHECK(!stages[0].record(cmd, s, wall, slices[0], {&region,1}, outer[0]));
    CHECK(stages[0].inner()->renderer_passes().plate_dof == render::PassState::Refused);
    s.renderer_settings.slice_apron_limit = 512;
    s.instances[0].style.effects.blur_enabled = true; s.instances[0].style.effects.blur_radius = 3;
    cmd = acquire(d); CHECK(!stages[0].record(cmd, s, wall, slices[0], {&region,1}, outer[0]));
    CHECK(stages[0].inner()->renderer_passes().plate_dof == render::PassState::Refused);
    s.instances[0].style.effects.blur_enabled = false;
    s.renderer_settings.backend = scene::RendererBackend::Basic; s.renderer_settings.required = false;
    cmd = acquire(d); CHECK(!stages[0].record(cmd, s, wall, slices[0], {&region,1}, outer[0]));
    CHECK(stages[0].inner()->renderer_passes().plate_dof == render::PassState::BackendUnsupported);
}

int main() {
    try {
        authored_and_optical_contract(); std::cout << "[pass] physical optics, authored persistence, digests and plate guard\n";
        bool tested = false;
        for (auto backend : {rhi::BackendKind::SdlGpu, rhi::BackendKind::D3d12}) {
            if (!rhi::Device::available(backend)) continue;
            rhi::DeviceConfig config{}; config.backend = backend; config.debug = true;
            auto opened = rhi::Device::create(config); CHECK(opened); auto d = std::move(opened).value();
            predicted_gpu_radius(d); tested = true;
            if (d.capabilities().advanced_raster()) plates_and_seams(d);
            std::cout << "[pass] GPU radius, wall identity, aperture, HDR, Hold, refusals and plate seams: " << static_cast<int>(backend) << '\n';
        }
        return tested ? 0 : 77;
    } catch (const std::exception& e) { std::cerr << "[fail] " << e.what() << '\n'; return 1; }
}
