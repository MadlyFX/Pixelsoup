#include <mediaserver/render/media_cache.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include "../src/nv12_conversion.hpp"

#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/geometry.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

// The GPU proof for the media pass. The scene-model suite establishes, with no
// GPU, that a clip node parses and advances with the show frame. What it cannot
// show is that a pipeline exists which samples the clip onto a quad in the 3D
// scene, oriented the right way. So this suite writes a known two-colour image,
// puts it on a frame-filling quad, renders, and reads back - proving the clip is
// sampled, composited over the scene, and that its left is the wall's left.
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

#define MEDIA_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] rhi::Device open_device(rhi::BackendKind backend = rhi::BackendKind::SdlGpu) {
    rhi::DeviceConfig config{};
    config.debug = false;
    config.backend = backend;
    auto created = rhi::Device::create(config);
    check(created.has_value(), "device creation", __LINE__);
    return std::move(created).value();
}

// A small uncompressed 32-bit TGA, top-left origin, left half red and right half
// green - the simplest image stb_image reads that also pins horizontal
// orientation. Returns the path it wrote.
[[nodiscard]] std::string write_split_tga() {
    constexpr core::u32 width = 8U;
    constexpr core::u32 height = 8U;
    std::array<core::u8, 18U> header{};
    header[2U] = 2U; // uncompressed true-colour
    header[12U] = static_cast<core::u8>(width & 0xffU);
    header[13U] = static_cast<core::u8>((width >> 8U) & 0xffU);
    header[14U] = static_cast<core::u8>(height & 0xffU);
    header[15U] = static_cast<core::u8>((height >> 8U) & 0xffU);
    header[16U] = 32U;   // bits per pixel
    header[17U] = 0x28U; // top-left origin, 8 alpha bits

    std::vector<core::u8> pixels;
    pixels.reserve(static_cast<std::size_t>(width) * height * 4U);
    for (core::u32 y = 0U; y < height; ++y) {
        for (core::u32 x = 0U; x < width; ++x) {
            // TGA stores BGRA. Left half red, right half green.
            const bool left = x < width / 2U;
            pixels.push_back(0U);                       // B
            pixels.push_back(left ? 0U : 255U);         // G
            pixels.push_back(left ? 255U : 0U);         // R
            pixels.push_back(255U);                     // A
        }
    }

    const auto path =
        (std::filesystem::temp_directory_path() / "mediaserver_split_clip.tga")
            .string();
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(reinterpret_cast<const char*>(header.data()), header.size());
    out.write(
        reinterpret_cast<const char*>(pixels.data()),
        static_cast<std::streamsize>(pixels.size()));
    out.close();
    check(static_cast<bool>(out), "wrote the test clip", __LINE__);
    return path;
}

[[nodiscard]] std::string write_solid_tga(
    std::string_view name, core::u8 red, core::u8 green, core::u8 blue) {
    constexpr core::u32 width = 8U;
    constexpr core::u32 height = 8U;
    std::array<core::u8, 18U> header{};
    header[2U] = 2U;
    header[12U] = static_cast<core::u8>(width);
    header[14U] = static_cast<core::u8>(height);
    header[16U] = 32U;
    header[17U] = 0x28U;
    std::vector<core::u8> pixels;
    pixels.reserve(static_cast<std::size_t>(width) * height * 4U);
    for (core::u32 index = 0U; index < width * height; ++index) {
        pixels.push_back(blue);
        pixels.push_back(green);
        pixels.push_back(red);
        pixels.push_back(255U);
    }
    const auto path = std::filesystem::temp_directory_path()
        / ("mediaserver_" + std::string{name} + ".tga");
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(reinterpret_cast<const char*>(header.data()), header.size());
    out.write(reinterpret_cast<const char*>(pixels.data()),
              static_cast<std::streamsize>(pixels.size()));
    out.close();
    check(static_cast<bool>(out), "wrote solid test clip", __LINE__);
    return path.string();
}

[[nodiscard]] render::RendererLimits test_limits() noexcept {
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

// A scene that is nothing but a clip on a frame-filling quad, facing a camera
// down -Z.
[[nodiscard]] scene::Scene clip_scene(scene::Canvas canvas, const std::string& path) {
    scene::Scene scn{};
    scn.canvas = canvas;
    scn.camera.eye = scene::Vec3{0.0F, 0.0F, 3.0F};
    scn.camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    scn.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    scn.camera.field_of_view = 0.125F;
    scn.camera.near_plane = 0.1F;
    scn.camera.far_plane = 100.0F;
    scn.environment.sky_color = scene::Vec3{0.05F, 0.07F, 0.10F};

    scene::MediaInstance clip{};
    clip.mesh = scene::MeshKey{scene::Primitive::Quad, 1U};
    // Overfills the frame at this distance, so every sampled pixel is on the
    // clip rather than the sky.
    clip.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, 0.0F, 0.0F}),
        scene::scaling(scene::Vec3{3.0F, 3.0F, 1.0F}));
    clip.normal = scene::normal_basis(clip.world);
    clip.source = path; // no '#': a still image, path used as-is
    clip.frame_index = 0U;
    clip.source_node = mediaserver::doc::NodeId{1U};
    scn.media.push_back(clip);
    return scn;
}

struct Image final {
    core::u32 width{};
    core::u32 height{};
    std::vector<core::u8> rgba;

    [[nodiscard]] std::size_t at(core::u32 x, core::u32 y) const noexcept {
        return (static_cast<std::size_t>(y) * width + x) * 4U;
    }
};

[[nodiscard]] Image render_clip(
    rhi::Device& device,
    const scene::Scene& scn,
    core::u32 expected_media = 1U) {
    auto created = render::SceneRenderer::create(
        device, scn.canvas.width, scn.canvas.height, test_limits());
    check(created.has_value(), "renderer creation", __LINE__);
    auto renderer = std::move(created).value();

    const auto bytes = scn.canvas.width * scn.canvas.height * 4U;
    auto readback = device.create_readback(bytes);
    check(readback.has_value(), "readback creation", __LINE__);

    // A real still is intentionally cold on the first pass: the render path
    // queues its disk decode and draws the last complete answer (none here).
    // Submit each attempted frame so command/resource state remains truthful,
    // then poll with ordinary renders until the background result is ready.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{5};
    for (;;) {
        auto acquired = device.acquire_commands();
        check(acquired.has_value(), "acquire commands", __LINE__);
        auto commands = std::move(acquired).value();

        check(
            renderer.render(commands, scn, scene::whole(scn.canvas)),
            "render",
            __LINE__);
        const bool ready = renderer.stats().media == expected_media;
        if (ready) {
            check(
                commands.download(renderer.target(), readback.value()),
                "download",
                __LINE__);
        }
        auto submitted = device.submit(std::move(commands));
        check(submitted.has_value(), "submit", __LINE__);
        check(std::move(submitted).value().wait(), "wait", __LINE__);
        if (ready) {
            break;
        }
        check(
            std::chrono::steady_clock::now() < deadline,
            "background still decode completed",
            __LINE__);
        std::this_thread::yield();
    }

    Image image{};
    image.width = scn.canvas.width;
    image.height = scn.canvas.height;
    const auto pixels = readback.value().map();
    check(pixels.size() >= bytes, "readback map", __LINE__);
    image.rgba.assign(pixels.begin(), pixels.begin() + bytes);
    readback.value().unmap();
    return image;
}

[[nodiscard]] Image render_clip(
    rhi::Device& device,
    const scene::Scene& scn,
    render::MediaCache& media_cache) {
    auto created = render::SceneRenderer::create(
        device, scn.canvas.width, scn.canvas.height, test_limits());
    check(created.has_value(), "renderer creation", __LINE__);
    auto renderer = std::move(created).value();
    auto readback = device.create_readback(
        scn.canvas.width * scn.canvas.height * 4U);
    check(readback.has_value(), "readback creation", __LINE__);
    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "acquire commands", __LINE__);
    auto commands = std::move(acquired).value();

    media_cache.begin_frame();
    check(
        renderer.render(
            commands, scn, scene::whole(scn.canvas),
            render::SceneRenderOptions{}, media_cache),
        "render with injected media cache", __LINE__);
    check(renderer.stats().media == 1U, "expected clip drawn", __LINE__);
    check(
        commands.download(renderer.target(), readback.value()),
        "download", __LINE__);
    media_cache.end_frame();
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "submit", __LINE__);
    check(std::move(submitted).value().wait(), "wait", __LINE__);

    Image image{};
    image.width = scn.canvas.width;
    image.height = scn.canvas.height;
    const auto pixels = readback.value().map();
    const auto bytes = scn.canvas.width * scn.canvas.height * 4U;
    check(pixels.size() >= bytes, "readback map", __LINE__);
    image.rgba.assign(pixels.begin(), pixels.begin() + bytes);
    readback.value().unmap();
    return image;
}

void depth_orders_media_and_lane_order_breaks_ties() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};
    auto scn = clip_scene(canvas, write_solid_tga("lane_red", 255U, 0U, 0U));
    scn.environment.sky_color = scene::Vec3{};
    auto& red = scn.media.front();
    red.style.opacity = 0.5F;
    red.compositing_order = 0U;
    // Red is physically nearer on a later lane's far side. Media clips never
    // write depth, so the sort is the only thing that occludes them: the nearer
    // clip has to be drawn last whatever the timeline says, or the clip added
    // most recently would paint over surfaces standing in front of it.
    red.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, 0.0F, 0.2F}),
        scene::scaling(scene::Vec3{3.0F, 3.0F, 1.0F}));
    red.normal = scene::normal_basis(red.world);

    scene::MediaInstance green = red;
    green.source = write_solid_tga("lane_green", 0U, 255U, 0U);
    green.source_node = mediaserver::doc::NodeId{2U};
    green.compositing_order = 1U;
    green.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, 0.0F, -0.2F}),
        scene::scaling(scene::Vec3{3.0F, 3.0F, 1.0F}));
    green.normal = scene::normal_basis(green.world);
    scn.media.push_back(green);

    const Image ordered = render_clip(device, scn, 2U);
    const auto centre = ordered.at(canvas.width / 2U, canvas.height / 2U);
    MEDIA_CHECK(ordered.rgba[centre + 0U] > ordered.rgba[centre + 1U]);

    // Where a clip sits in the evaluated vector is not what orders it.
    std::reverse(scn.media.begin(), scn.media.end());
    const Image reversed_vector = render_clip(device, scn, 2U);
    MEDIA_CHECK(reversed_vector.rgba == ordered.rgba);

    // Clips sharing one surface are at one depth to the bit, and there the
    // timeline decides. That is the case lane order exists for: stacking
    // several clips on the same LED wall.
    for (auto& coplanar_clip : scn.media) {
        coplanar_clip.world = scene::scaling(scene::Vec3{3.0F, 3.0F, 1.0F});
        coplanar_clip.normal = scene::normal_basis(coplanar_clip.world);
    }
    // The reverse above left green first in the vector.
    scn.media[0U].compositing_order = 1U;
    scn.media[1U].compositing_order = 0U;
    const Image green_lane_last = render_clip(device, scn, 2U);
    const auto coplanar = green_lane_last.at(
        canvas.width / 2U, canvas.height / 2U);
    MEDIA_CHECK(green_lane_last.rgba[coplanar + 1U]
                > green_lane_last.rgba[coplanar + 0U]);

    scn.media[0U].compositing_order = 0U;
    scn.media[1U].compositing_order = 1U;
    const Image swapped_lanes = render_clip(device, scn, 2U);
    const auto swapped = swapped_lanes.at(
        canvas.width / 2U, canvas.height / 2U);
    MEDIA_CHECK(swapped_lanes.rgba[swapped + 0U]
                > swapped_lanes.rgba[swapped + 1U]);
}

void a_crossing_clip_and_card_each_hide_the_other() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};
    const auto red_path = write_solid_tga("crossing_red", 255U, 0U, 0U);
    // A red clip and a green emitting card, both blended and both centred on
    // the origin, turned opposite ways about Y so they cross down the middle
    // of the frame: each is nearer the camera on one side.
    struct Crossing final {
        Image image;
        bool clip_nearer_on_left{};
    };
    const auto render_crossing = [&](float clip_yaw, float card_alpha) {
        auto scn = clip_scene(canvas, red_path);
        scn.environment.sky_color = scene::Vec3{};
        scn.environment.ground_color = scene::Vec3{};
        scn.environment.ambient = 0.0F;
        auto& clip = scn.media.front();
        clip.world = scene::multiply(
            scene::rotation(clip_yaw, 0.0F, 0.0F),
            scene::scaling(scene::Vec3{3.0F, 3.0F, 1.0F}));
        clip.normal = scene::normal_basis(clip.world);
        scene::MeshInstance card{};
        card.mesh = scene::MeshKey{scene::Primitive::Quad, 1U};
        card.source = mediaserver::doc::NodeId{2U};
        card.world = scene::multiply(
            scene::rotation(-clip_yaw, 0.0F, 0.0F),
            scene::scaling(scene::Vec3{3.0F, 3.0F, 1.0F}));
        card.normal = scene::normal_basis(card.world);
        card.material.base_color = scene::Vec3{};
        card.material.emissive = scene::Vec3{0.0F, 1.0F, 0.0F};
        card.material.alpha_mode = scene::MaterialAlphaMode::Blend;
        scene::Fill fill{};
        fill.color_a = scene::FillColor{1.0F, 1.0F, 1.0F, card_alpha};
        card.material.fill = fill;
        scn.instances.push_back(card);
        const scene::Vec3 left{-0.2F, 0.0F, 0.0F};
        const bool clip_nearer =
            scene::transform_point(clip.world, left).z
            > scene::transform_point(card.world, left).z;
        return Crossing{render_clip(device, scn), clip_nearer};
    };
    const auto red_wins = [](const Image& image, core::u32 x) {
        const auto pixel = image.at(x, 32U);
        return image.rgba[pixel + 0U] > 200U && image.rgba[pixel + 1U] < 40U;
    };
    const auto green_wins = [](const Image& image, core::u32 x) {
        const auto pixel = image.at(x, 32U);
        return image.rgba[pixel + 1U] > 200U && image.rgba[pixel + 0U] < 40U;
    };

    // Opaque, whichever way round they cross: the nearer one shows on each
    // side. The two are drawn in the same order both times, so one of these
    // needs the layer drawn first to hide the one drawn after it.
    for (const float yaw : {0.05F, -0.05F}) {
        const auto crossing = render_crossing(yaw, 1.0F);
        const bool clip_left = crossing.clip_nearer_on_left;
        MEDIA_CHECK(clip_left ? red_wins(crossing.image, 16U)
                              : green_wins(crossing.image, 16U));
        MEDIA_CHECK(clip_left ? green_wins(crossing.image, 48U)
                              : red_wins(crossing.image, 48U));
    }

    // A translucent card writes no depth of its own, but the opaque clip
    // still hides it wherever the clip is the nearer of the two.
    for (const float yaw : {0.05F, -0.05F}) {
        const auto crossing = render_crossing(yaw, 0.5F);
        MEDIA_CHECK(red_wins(crossing.image,
                             crossing.clip_nearer_on_left ? 16U : 48U));
    }

    // On one plane neither hides the other by depth: the composite order
    // decides the whole surface, the same on both sides.
    const auto coplanar = render_crossing(0.0F, 1.0F);
    MEDIA_CHECK(red_wins(coplanar.image, 16U) == red_wins(coplanar.image, 48U));
    MEDIA_CHECK(red_wins(coplanar.image, 16U)
                != green_wins(coplanar.image, 16U));
}

[[nodiscard]] bool renderer_usable(rhi::Device& device) {
    auto probe = render::SceneRenderer::create(device, 16U, 16U, test_limits());
    if (probe) {
        return true;
    }
    return probe.error() != render::RenderError::ShaderFormatUnsupported;
}

void a_clip_is_sampled_onto_the_quad_the_right_way_round() {
    auto device = open_device();
    const auto path = write_split_tga();
    constexpr scene::Canvas canvas{128U, 128U};
    const auto image = render_clip(device, clip_scene(canvas, path));

    // A pixel left of centre samples the clip's left half: red. One right of
    // centre samples the right half: green. This proves the clip is on the
    // quad, and that its left is the wall's left rather than mirrored.
    const auto left = image.at(canvas.width / 4U, canvas.height / 2U);
    MEDIA_CHECK(image.rgba[left + 0U] > 200U);
    MEDIA_CHECK(image.rgba[left + 1U] < 60U);
    MEDIA_CHECK(image.rgba[left + 2U] < 60U);

    const auto right = image.at((canvas.width * 3U) / 4U, canvas.height / 2U);
    MEDIA_CHECK(image.rgba[right + 1U] > 200U);
    MEDIA_CHECK(image.rgba[right + 0U] < 60U);
    MEDIA_CHECK(image.rgba[right + 2U] < 60U);
}

void clip_opacity_and_brightness_are_composited() {
    auto device = open_device();
    const auto path = write_split_tga();
    constexpr scene::Canvas canvas{128U, 128U};
    auto scn = clip_scene(canvas, path);
    scn.media.front().style.opacity = 0.5F;
    scn.media.front().style.effects.brightness_enabled = true;
    scn.media.front().style.effects.brightness = 0.5F;
    // The project default is a scene-referred exposure, so a clip - which is
    // an sRGB-encoded picture - is linearised before the multiply and encoded
    // again afterwards. Half the light is about 73% of the encoded value, not
    // half of it, which is the whole point of the distinction: a clip and a
    // lit surface set to the same brightness now match.
    scn.effect_settings.brightness_curve = scene::BrightnessCurve::Exposure;
    const auto exposed = render_clip(device, scn);

    const auto left = exposed.at(canvas.width / 4U, canvas.height / 2U);
    MEDIA_CHECK(exposed.rgba[left + 0U] > 75U);
    MEDIA_CHECK(exposed.rgba[left + 0U] < 115U);
    MEDIA_CHECK(exposed.rgba[left + 1U] < 35U);
    MEDIA_CHECK(exposed.rgba[left + 2U] < 40U);

    const auto right = exposed.at((canvas.width * 3U) / 4U, canvas.height / 2U);
    MEDIA_CHECK(exposed.rgba[right + 1U] > 75U);
    MEDIA_CHECK(exposed.rgba[right + 1U] < 120U);
    MEDIA_CHECK(exposed.rgba[right + 0U] < 35U);

    // The display-referred gain is the vision-mixer behaviour: it scales the
    // encoded picture directly, so the same brightness lands visibly darker.
    scn.effect_settings.brightness_curve = scene::BrightnessCurve::Gain;
    const auto gained = render_clip(device, scn);
    const auto gained_left = gained.at(canvas.width / 4U, canvas.height / 2U);
    MEDIA_CHECK(gained.rgba[gained_left + 0U] > 50U);
    MEDIA_CHECK(gained.rgba[gained_left + 0U] < 95U);
    MEDIA_CHECK(gained.rgba[gained_left + 0U] < exposed.rgba[left + 0U]);
}

void clip_saturation_is_identity_exact_in_both_response_domains() {
    auto device = open_device();
    const auto path = write_split_tga();
    constexpr scene::Canvas canvas{128U, 128U};
    for (const scene::BrightnessCurve response : {
             scene::BrightnessCurve::Exposure,
             scene::BrightnessCurve::Gain}) {
        scene::Scene plain = clip_scene(canvas, path);
        plain.effect_settings.brightness_curve = response;
        const Image reference = render_clip(device, plain);

        scene::Scene identity = plain;
        identity.media.front().style.effects.saturation_enabled = true;
        identity.media.front().style.effects.saturation = 1.0F;
        const Image identity_image = render_clip(device, identity);
        MEDIA_CHECK(identity_image.rgba == reference.rgba);

        scene::Scene monochrome = plain;
        monochrome.media.front().style.effects.saturation_enabled = true;
        monochrome.media.front().style.effects.saturation = 0.0F;
        const Image grey = render_clip(device, monochrome);
        MEDIA_CHECK(grey.rgba != reference.rgba);
        for (const core::u32 x : {canvas.width / 4U,
                                  (canvas.width * 3U) / 4U}) {
            const std::size_t pixel = grey.at(x, canvas.height / 2U);
            const auto difference = [&](std::size_t left,
                                        std::size_t right) noexcept {
                const int a = static_cast<int>(grey.rgba[pixel + left]);
                const int b = static_cast<int>(grey.rgba[pixel + right]);
                return a > b ? a - b : b - a;
            };
            MEDIA_CHECK(difference(0U, 1U) <= 2);
            MEDIA_CHECK(difference(1U, 2U) <= 2);
            MEDIA_CHECK(grey.rgba[pixel + 3U] == 255U);
        }
    }
}

void clip_levels_is_identity_exact_and_authored_order_is_visible() {
    auto device = open_device();
    const auto path = write_split_tga();
    constexpr scene::Canvas canvas{128U, 128U};
    for (const scene::BrightnessCurve response : {
             scene::BrightnessCurve::Exposure,
             scene::BrightnessCurve::Gain}) {
        scene::Scene plain = clip_scene(canvas, path);
        plain.effect_settings.brightness_curve = response;
        const Image reference = render_clip(device, plain);

        scene::Scene identity = plain;
        identity.media.front().style.effects.inline_operations.push_back(
            scene::InlineEffectOperation{
                scene::EffectKind::Levels, 1.0F,
                scene::LevelsParameters{}});
        const Image identity_image = render_clip(device, identity);
        MEDIA_CHECK(identity_image.rgba == reference.rgba);

        scene::LevelsParameters inverted_levels{};
        inverted_levels.master.output_black = 1.0F;
        inverted_levels.master.output_white = 0.0F;
        scene::Scene inverted = plain;
        inverted.media.front().style.effects.inline_operations.push_back(
            scene::InlineEffectOperation{
                scene::EffectKind::Levels, 1.0F, inverted_levels});
        const Image inversion = render_clip(device, inverted);
        const auto inverted_pixel =
            inversion.at(canvas.width / 4U, canvas.height / 2U);
        MEDIA_CHECK(inversion.rgba[inverted_pixel] < 8U);
        MEDIA_CHECK(inversion.rgba[inverted_pixel + 1U] > 247U);
        MEDIA_CHECK(inversion.rgba[inverted_pixel + 2U] > 247U);

        scene::LevelsParameters levels{};
        levels.master.output_black = 0.2F;
        levels.master.output_white = 0.8F;
        const scene::InlineEffectOperation brightness{
            scene::EffectKind::Brightness, 0.5F, {}};
        const scene::InlineEffectOperation grade{
            scene::EffectKind::Levels, 1.0F, levels};

        scene::Scene brightness_then_levels = plain;
        brightness_then_levels.media.front()
            .style.effects.inline_operations = {brightness, grade};
        const Image first = render_clip(device, brightness_then_levels);

        scene::Scene levels_then_brightness = plain;
        levels_then_brightness.media.front()
            .style.effects.inline_operations = {grade, brightness};
        const Image second = render_clip(device, levels_then_brightness);
        MEDIA_CHECK(first.rgba != second.rgba);
        const auto pixel = first.at(canvas.width / 4U, canvas.height / 2U);
        MEDIA_CHECK(first.rgba[pixel] > second.rgba[pixel] + 8U);
        MEDIA_CHECK(first.rgba[pixel + 1U] > second.rgba[pixel + 1U] + 8U);
    }
}

void clip_lift_gamma_gain_is_identity_exact_and_ordered() {
    auto device = open_device();
    const auto path = write_split_tga();
    constexpr scene::Canvas canvas{128U, 128U};
    for (const scene::BrightnessCurve response : {
             scene::BrightnessCurve::Exposure,
             scene::BrightnessCurve::Gain}) {
        scene::Scene plain = clip_scene(canvas, path);
        plain.effect_settings.brightness_curve = response;
        const Image reference = render_clip(device, plain);

        scene::Scene identity = plain;
        identity.media.front().style.effects.inline_operations.push_back(
            scene::InlineEffectOperation{
                scene::EffectKind::LiftGammaGain, 1.0F, {},
                scene::LiftGammaGainParameters{}});
        const Image identity_image = render_clip(device, identity);
        MEDIA_CHECK(identity_image.rgba == reference.rgba);

        scene::LiftGammaGainParameters lift_gamma_gain{};
        lift_gamma_gain.lift = scene::Vec3{0.2F, 0.2F, 0.2F};
        lift_gamma_gain.gamma = scene::Vec3{2.0F, 2.0F, 2.0F};
        lift_gamma_gain.gain = scene::Vec3{0.75F, 0.75F, 0.75F};
        const scene::InlineEffectOperation grade{
            scene::EffectKind::LiftGammaGain, 1.0F, {}, lift_gamma_gain};

        scene::LevelsParameters levels{};
        levels.master.output_black = 0.1F;
        levels.master.output_white = 0.8F;
        const scene::InlineEffectOperation remap{
            scene::EffectKind::Levels, 1.0F, levels};

        scene::Scene grade_then_levels = plain;
        grade_then_levels.media.front().style.effects.inline_operations = {
            grade, remap};
        const Image first = render_clip(device, grade_then_levels);

        scene::Scene levels_then_grade = plain;
        levels_then_grade.media.front().style.effects.inline_operations = {
            remap, grade};
        const Image second = render_clip(device, levels_then_grade);
        MEDIA_CHECK(first.rgba != second.rgba);

        // Both effects are nonlinear and deliberately non-commutative. This
        // sample also exercises all three Lift/Gamma/Gain parameter vectors.
        for (const core::u32 x : {canvas.width / 4U,
                                  (canvas.width * 3U) / 4U}) {
            const auto pixel = first.at(x, canvas.height / 2U);
            MEDIA_CHECK(second.rgba[pixel + 0U] > first.rgba[pixel + 0U] + 5U);
            MEDIA_CHECK(second.rgba[pixel + 1U] > first.rgba[pixel + 1U] + 5U);
            MEDIA_CHECK(second.rgba[pixel + 2U] > first.rgba[pixel + 2U] + 5U);
            MEDIA_CHECK(first.rgba[pixel + 3U] == 255U);
            MEDIA_CHECK(second.rgba[pixel + 3U] == 255U);
        }
    }
}

void clip_contrast_pivot_is_identity_exact_and_ordered() {
    auto device = open_device();
    const auto path = write_split_tga();
    constexpr scene::Canvas canvas{128U, 128U};
    for (const scene::BrightnessCurve response : {
             scene::BrightnessCurve::Exposure,
             scene::BrightnessCurve::Gain}) {
        scene::Scene plain = clip_scene(canvas, path);
        plain.effect_settings.brightness_curve = response;
        const Image reference = render_clip(device, plain);

        scene::Scene identity = plain;
        identity.media.front().style.effects.inline_operations.push_back(
            scene::InlineEffectOperation{
                scene::EffectKind::ContrastPivot, 1.0F, {}, {},
                scene::ContrastPivotParameters{1.0F, 0.9F}});
        const Image identity_image = render_clip(device, identity);
        MEDIA_CHECK(identity_image.rgba == reference.rgba);

        const scene::InlineEffectOperation contrast{
            scene::EffectKind::ContrastPivot, 1.0F, {}, {},
            scene::ContrastPivotParameters{2.0F, 0.4F}};
        scene::LevelsParameters levels{};
        levels.master.output_black = 0.2F;
        levels.master.output_white = 0.8F;
        const scene::InlineEffectOperation remap{
            scene::EffectKind::Levels, 1.0F, levels};

        scene::Scene contrast_then_levels = plain;
        contrast_then_levels.media.front()
            .style.effects.inline_operations = {contrast, remap};
        const Image first = render_clip(device, contrast_then_levels);

        scene::Scene levels_then_contrast = plain;
        levels_then_contrast.media.front()
            .style.effects.inline_operations = {remap, contrast};
        const Image second = render_clip(device, levels_then_contrast);
        MEDIA_CHECK(first.rgba != second.rgba);

        const std::array<core::u32, 2U> xs{
            canvas.width / 4U, (canvas.width * 3U) / 4U};
        for (std::size_t sample = 0U; sample < xs.size(); ++sample) {
            const auto pixel = first.at(xs[sample], canvas.height / 2U);
            const std::size_t dominant = sample == 0U ? 0U : 1U;
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                if (channel == dominant) {
                    MEDIA_CHECK(second.rgba[pixel + channel] >
                                first.rgba[pixel + channel] + 8U);
                } else {
                    MEDIA_CHECK(first.rgba[pixel + channel] >
                                second.rgba[pixel + channel] + 8U);
                }
            }
            MEDIA_CHECK(first.rgba[pixel + 3U] == 255U);
            MEDIA_CHECK(second.rgba[pixel + 3U] == 255U);
        }
    }
}

void clip_color_matrix_is_identity_exact_and_ordered() {
    auto device = open_device();
    const auto path = write_split_tga();
    constexpr scene::Canvas canvas{128U, 128U};
    for (const scene::BrightnessCurve response : {
             scene::BrightnessCurve::Exposure,
             scene::BrightnessCurve::Gain}) {
        scene::Scene plain = clip_scene(canvas, path);
        plain.effect_settings.brightness_curve = response;
        const Image reference = render_clip(device, plain);

        scene::Scene identity = plain;
        identity.media.front().style.effects.inline_operations.push_back(
            scene::InlineEffectOperation{
                scene::EffectKind::ColorMatrix, 1.0F, {}, {}, {},
                scene::ColorMatrixParameters{}});
        const Image identity_image = render_clip(device, identity);
        MEDIA_CHECK(identity_image.rgba == reference.rgba);

        scene::ColorMatrixParameters swap_red_green{};
        swap_red_green.matrix = {
            0.0F, 1.0F, 0.0F,
            1.0F, 0.0F, 0.0F,
            0.0F, 0.0F, 1.0F};
        const scene::InlineEffectOperation mixer{
            scene::EffectKind::ColorMatrix, 1.0F, {}, {}, {},
            swap_red_green};

        scene::LevelsParameters levels{};
        levels.red.output_black = 0.1F;
        levels.red.output_white = 0.9F;
        levels.green.output_black = 0.2F;
        levels.green.output_white = 0.6F;
        const scene::InlineEffectOperation remap{
            scene::EffectKind::Levels, 1.0F, levels};

        scene::Scene matrix_then_levels = plain;
        matrix_then_levels.media.front()
            .style.effects.inline_operations = {mixer, remap};
        const Image first = render_clip(device, matrix_then_levels);

        scene::Scene levels_then_matrix = plain;
        levels_then_matrix.media.front()
            .style.effects.inline_operations = {remap, mixer};
        const Image second = render_clip(device, levels_then_matrix);
        MEDIA_CHECK(first.rgba != second.rgba);

        const auto left = first.at(canvas.width / 4U, canvas.height / 2U);
        MEDIA_CHECK(second.rgba[left + 0U] > first.rgba[left + 0U] + 8U);
        MEDIA_CHECK(second.rgba[left + 1U] > first.rgba[left + 1U] + 8U);
        const auto right =
            first.at((canvas.width * 3U) / 4U, canvas.height / 2U);
        MEDIA_CHECK(first.rgba[right + 0U] > second.rgba[right + 0U] + 8U);
        MEDIA_CHECK(first.rgba[right + 1U] > second.rgba[right + 1U] + 8U);
        MEDIA_CHECK(first.rgba[left + 3U] == 255U);
        MEDIA_CHECK(second.rgba[left + 3U] == 255U);
        MEDIA_CHECK(first.rgba[right + 3U] == 255U);
        MEDIA_CHECK(second.rgba[right + 3U] == 255U);
    }
}

void clip_white_balance_is_identity_exact_and_ordered() {
    auto device = open_device();
    const auto path = write_split_tga();
    constexpr scene::Canvas canvas{128U, 128U};
    for (const scene::BrightnessCurve response : {
             scene::BrightnessCurve::Exposure,
             scene::BrightnessCurve::Gain}) {
        scene::Scene plain = clip_scene(canvas, path);
        plain.effect_settings.brightness_curve = response;
        const Image reference = render_clip(device, plain);

        scene::Scene identity = plain;
        identity.media.front().style.effects.inline_operations.push_back(
            scene::InlineEffectOperation{
                scene::EffectKind::WhiteBalance, 1.0F, {}, {}, {}, {},
                scene::WhiteBalanceParameters{}});
        const Image identity_image = render_clip(device, identity);
        MEDIA_CHECK(identity_image.rgba == reference.rgba);

        const auto white_balance = scene::normalized_white_balance(
            scene::WhiteBalanceParameters{2'000.0F, 0.0F});
        MEDIA_CHECK(white_balance.has_value());
        const scene::InlineEffectOperation balance{
            scene::EffectKind::WhiteBalance, 1.0F, {}, {}, {}, {},
            *white_balance};
        scene::LevelsParameters levels{};
        levels.master.output_black = 0.2F;
        levels.master.output_white = 0.8F;
        const scene::InlineEffectOperation remap{
            scene::EffectKind::Levels, 1.0F, levels};

        scene::Scene balance_then_levels = plain;
        balance_then_levels.media.front()
            .style.effects.inline_operations = {balance, remap};
        const Image first = render_clip(device, balance_then_levels);

        scene::Scene levels_then_balance = plain;
        levels_then_balance.media.front()
            .style.effects.inline_operations = {remap, balance};
        const Image second = render_clip(device, levels_then_balance);
        MEDIA_CHECK(first.rgba != second.rgba);

        const auto left = first.at(canvas.width / 4U, canvas.height / 2U);
        MEDIA_CHECK(second.rgba[left + 0U] > first.rgba[left + 0U] + 8U);
        MEDIA_CHECK(first.rgba[left + 1U] > second.rgba[left + 1U] + 8U);
        MEDIA_CHECK(first.rgba[left + 2U] > second.rgba[left + 2U] + 8U);
        const auto right =
            first.at((canvas.width * 3U) / 4U, canvas.height / 2U);
        MEDIA_CHECK(second.rgba[right + 0U] > first.rgba[right + 0U] + 8U);
        MEDIA_CHECK(first.rgba[right + 2U] > second.rgba[right + 2U] + 8U);
        MEDIA_CHECK(first.rgba[left + 3U] == 255U);
        MEDIA_CHECK(second.rgba[left + 3U] == 255U);
        MEDIA_CHECK(first.rgba[right + 3U] == 255U);
        MEDIA_CHECK(second.rgba[right + 3U] == 255U);
    }
}

void clip_curves_are_identity_exact_and_ordered() {
    auto device = open_device();
    const auto path = write_split_tga();
    constexpr scene::Canvas canvas{128U, 128U};
    scene::CurvesParameters authored{};
    authored.master.points = {
        {0.0F, 0.0F}, {0.5F, 0.2F}, {1.0F, 1.0F}};
    const auto curves = scene::normalized_curves(authored);
    MEDIA_CHECK(curves.has_value());
    for (const scene::BrightnessCurve response : {
             scene::BrightnessCurve::Exposure,
             scene::BrightnessCurve::Gain}) {
        scene::Scene plain = clip_scene(canvas, path);
        plain.effect_settings.brightness_curve = response;
        const Image reference = render_clip(device, plain);

        scene::Scene identity = plain;
        identity.media.front().style.effects.inline_operations.push_back(
            scene::InlineEffectOperation{
                scene::EffectKind::Curves, 1.0F, {}, {}, {}, {}, {},
                scene::CurvesParameters{}});
        const Image identity_image = render_clip(device, identity);
        MEDIA_CHECK(identity_image.rgba == reference.rgba);

        const scene::InlineEffectOperation curve_operation{
            scene::EffectKind::Curves, 1.0F, {}, {}, {}, {}, {}, *curves};
        const scene::InlineEffectOperation contrast{
            scene::EffectKind::ContrastPivot, 1.0F, {}, {},
            scene::ContrastPivotParameters{0.0F, 0.4F}};

        scene::Scene curve_then_contrast = plain;
        curve_then_contrast.media.front().style.effects.inline_operations = {
            curve_operation, contrast};
        const Image first = render_clip(device, curve_then_contrast);
        scene::Scene contrast_then_curve = plain;
        contrast_then_curve.media.front().style.effects.inline_operations = {
            contrast, curve_operation};
        const Image second = render_clip(device, contrast_then_curve);
        MEDIA_CHECK(first.rgba != second.rgba);
        const auto pixel = first.at(canvas.width / 4U, canvas.height / 2U);
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            MEDIA_CHECK(first.rgba[pixel + channel]
                        > second.rgba[pixel + channel] + 20U);
        }
        MEDIA_CHECK(first.rgba[pixel + 3U] == 255U);
        MEDIA_CHECK(second.rgba[pixel + 3U] == 255U);
    }
}

void clip_film_grain_is_identity_exact_temporal_and_ordered() {
    auto device = open_device();
    const auto path = write_split_tga();
    constexpr scene::Canvas canvas{128U, 128U};
    const auto grain_operation = [](float amount) {
        scene::InlineEffectOperation operation{};
        operation.kind = scene::EffectKind::FilmGrain;
        operation.film_grain = scene::FilmGrainParameters{
            amount, 2.0F, 0.8F, 0x1020'3040U,
            scene::FilmGrainMode::RGB};
        return operation;
    };
    const scene::InlineEffectOperation contrast{
        scene::EffectKind::ContrastPivot, 1.0F, {}, {},
        scene::ContrastPivotParameters{0.0F, 0.4F}};
    for (const scene::BrightnessCurve response : {
             scene::BrightnessCurve::Exposure,
             scene::BrightnessCurve::Gain}) {
        scene::Scene plain = clip_scene(canvas, path);
        plain.effect_settings.brightness_curve = response;
        plain.show_frame = 9U;
        const Image reference = render_clip(device, plain);

        scene::Scene identity = plain;
        identity.media.front().style.effects.inline_operations = {
            grain_operation(0.0F)};
        const Image identity_image = render_clip(device, identity);
        MEDIA_CHECK(identity_image.rgba == reference.rgba);

        scene::Scene grain = plain;
        grain.media.front().style.effects.inline_operations = {
            grain_operation(0.08F)};
        const Image first_grain = render_clip(device, grain);
        const Image repeated = render_clip(device, grain);
        MEDIA_CHECK(first_grain.rgba == repeated.rgba);
        MEDIA_CHECK(first_grain.rgba != reference.rgba);

        scene::Scene next_frame = grain;
        next_frame.show_frame = 10U;
        const Image next = render_clip(device, next_frame);
        MEDIA_CHECK(next.rgba != first_grain.rgba);

        scene::Scene grain_then_contrast = plain;
        grain_then_contrast.media.front()
            .style.effects.inline_operations = {
                grain_operation(0.08F), contrast};
        const Image first = render_clip(device, grain_then_contrast);
        scene::Scene contrast_then_grain = plain;
        contrast_then_grain.media.front()
            .style.effects.inline_operations = {
                contrast, grain_operation(0.08F)};
        const Image second = render_clip(device, contrast_then_grain);
        MEDIA_CHECK(first.rgba != second.rgba);

        for (std::size_t pixel = 3U; pixel < first_grain.rgba.size();
             pixel += 4U) {
            MEDIA_CHECK(first_grain.rgba[pixel] == reference.rgba[pixel]);
            MEDIA_CHECK(first.rgba[pixel] == reference.rgba[pixel]);
            MEDIA_CHECK(second.rgba[pixel] == reference.rgba[pixel]);
        }
    }
}

void a_missing_clip_leaves_the_scene_rather_than_failing() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};
    // A missing sequence frame still records a visible placeholder.
    auto scn = clip_scene(canvas, "does_not_exist_####.png");
    scn.media.front().frame_index = 7U;

    auto created = render::SceneRenderer::create(
        device, canvas.width, canvas.height, test_limits());
    MEDIA_CHECK(created.has_value());
    auto renderer = std::move(created).value();
    auto acquired = device.acquire_commands();
    MEDIA_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();
    MEDIA_CHECK(renderer.render(commands, scn, scene::whole(canvas)));
    MEDIA_CHECK(renderer.stats().media == 0U);
    MEDIA_CHECK(renderer.stats().offline_media == 1U);
    auto submitted = device.submit(std::move(commands));
    MEDIA_CHECK(submitted.has_value());
    MEDIA_CHECK(std::move(submitted).value().wait());

    const Image image = render_clip(device, scn, 0U);
    const auto center = image.at(32U, 32U);
    const auto grey = image.at(16U, 32U);
    const auto diagonal = image.at(16U, 16U);
    const auto other_diagonal = image.at(47U, 16U);
    MEDIA_CHECK(image.rgba[grey] > image.rgba[center] + 30U);
    MEDIA_CHECK(image.rgba[grey] > image.rgba[diagonal] + 30U);
    MEDIA_CHECK(image.rgba[grey] > image.rgba[other_diagonal] + 30U);
    MEDIA_CHECK(image.rgba[grey] == image.rgba[grey + 1U]);
    MEDIA_CHECK(image.rgba[grey] == image.rgba[grey + 2U]);
    MEDIA_CHECK(image.rgba[grey + 3U] == 255U);
}

struct ControlledSourceState final {
    core::u32 pending_frame{1U};
    bool frame_one_ready{};
    std::optional<core::u32> latest_frame;
    std::optional<core::u32> failed_frame;
    std::vector<core::u32> prefetched;
    std::vector<std::string> decoded_sources;
};

class ControlledAsyncSource final : public render::MediaSource {
public:
    explicit ControlledAsyncSource(
        std::shared_ptr<ControlledSourceState> state)
        : state_(std::move(state)) {}

    [[nodiscard]] std::string key(
        const std::string& source, core::u32 frame_index) const override {
        return source + "#" + std::to_string(frame_index);
    }

    [[nodiscard]] render::MediaDecodeScheduling scheduling(
        const std::string&) const noexcept override {
        return render::MediaDecodeScheduling::SourceManaged;
    }

    [[nodiscard]] render::DecodedFrame decode(
        const std::string&, core::u32 frame_index) override {
        render::DecodedFrame frame{};
        frame.width = 4U;
        frame.height = 4U;
        frame.pixels.resize(4U * 4U * 4U, 255U);
        frame.pixels[0U] = static_cast<core::u8>(frame_index);
        return frame;
    }

    [[nodiscard]] std::shared_ptr<const render::DecodedFrame> try_decode(
        const std::string& source, core::u32 frame_index) override {
        state_->decoded_sources.push_back(source);
        if (state_->failed_frame == frame_index) {
            return std::make_shared<const render::DecodedFrame>();
        }
        if (frame_index == state_->pending_frame && !state_->frame_one_ready) {
            return nullptr;
        }
        return std::make_shared<const render::DecodedFrame>(
            decode(source, frame_index));
    }

    [[nodiscard]] std::shared_ptr<const render::DecodedFrame> try_decode_latest(
        const std::string& source,
        core::u32 frame_index,
        core::u64) override {
        if (!state_->latest_frame.has_value() ||
            *state_->latest_frame > frame_index) {
            return nullptr;
        }
        auto frame = decode(source, *state_->latest_frame);
        frame.source_frame = *state_->latest_frame;
        return std::make_shared<const render::DecodedFrame>(std::move(frame));
    }

    [[nodiscard]] std::shared_ptr<const render::DecodedFrame> try_prefetched(
        const std::string& source, core::u32 frame_index, core::u64) override {
        if (frame_index == state_->pending_frame && !state_->frame_one_ready)
            return nullptr;
        return std::make_shared<const render::DecodedFrame>(decode(source, frame_index));
    }

    void prefetch(const std::string&, core::u32 frame_index) override {
        state_->prefetched.push_back(frame_index);
    }

private:
    std::shared_ptr<ControlledSourceState> state_;
};

class P010Source final : public render::MediaSource {
public:
    explicit P010Source(
        render::DecodedPixelLayout layout =
            render::DecodedPixelLayout::P010Rec709Limited,
        core::u16 y_code = 502U)
        : layout_(layout), y_code_(y_code) {}

    [[nodiscard]] std::string key(
        const std::string& source, core::u32 frame_index) const override {
        return source + "#" + std::to_string(frame_index);
    }

    [[nodiscard]] render::MediaDecodeScheduling scheduling(
        const std::string&) const noexcept override {
        return render::MediaDecodeScheduling::SourceManaged;
    }

    [[nodiscard]] render::DecodedFrame decode(
        const std::string&, core::u32 frame_index) override {
        render::DecodedFrame frame{};
        frame.width = 2U;
        frame.height = 2U;
        frame.layout = layout_;
        frame.chroma = render::ChromaExtent{1U, 1U};
        frame.source_frame = frame_index;
        // Four neutral-gray luma samples, followed by one neutral U/V pair.
        const core::u16 y = static_cast<core::u16>(y_code_ << 6U);
        constexpr core::u16 neutral = 512U << 6U;
        const std::array<core::u16, 6U> samples{{
            y, y, y, y, neutral, neutral,
        }};
        frame.pixels.resize(samples.size() * sizeof(core::u16));
        for (std::size_t index = 0U; index < samples.size(); ++index) {
            frame.pixels[index * 2U] =
                static_cast<core::u8>(samples[index] & 0xffU);
            frame.pixels[index * 2U + 1U] =
                static_cast<core::u8>(samples[index] >> 8U);
        }
        return frame;
    }

private:
    render::DecodedPixelLayout layout_;
    core::u16 y_code_{};
};

class Nv12ColorSource final : public render::MediaSource {
public:
    explicit Nv12ColorSource(std::array<core::u8, 3U> yuv) : yuv_(yuv) {}

    [[nodiscard]] std::string key(
        const std::string& source, core::u32 frame_index) const override {
        return source + "#" + std::to_string(frame_index);
    }

    [[nodiscard]] render::MediaDecodeScheduling scheduling(
        const std::string&) const noexcept override {
        return render::MediaDecodeScheduling::SourceManaged;
    }

    [[nodiscard]] render::DecodedFrame decode(
        const std::string&, core::u32 frame_index) override {
        const std::array<core::u8, 4U> luma{{
            yuv_[0], yuv_[0], yuv_[0], yuv_[0]}};
        const std::array<core::u8, 2U> chroma{{yuv_[1], yuv_[2]}};
        auto result = render::detail::pack_nv12_rec709(
            2U, 2U, true, luma, 2U, chroma, 2U);
        result.source_frame = frame_index;
        return result;
    }

private:
    std::array<core::u8, 3U> yuv_{};
};

// In-memory RGBA plates for the native Advanced-product proof. Keeping this
// source SourceManaged makes resolve deterministic and immediate without
// introducing a filesystem wait into the test render loop.
class AuxiliaryProductSource final : public render::MediaSource {
public:
    [[nodiscard]] std::string key(
        const std::string& source, core::u32 frame_index) const override {
        return source + "#" + std::to_string(frame_index);
    }

    [[nodiscard]] render::MediaDecodeScheduling scheduling(
        const std::string&) const noexcept override {
        return render::MediaDecodeScheduling::SourceManaged;
    }

    [[nodiscard]] render::DecodedFrame decode(
        const std::string& source, core::u32 frame_index) override {
        constexpr core::u32 width = 8U;
        constexpr core::u32 height = 8U;
        render::DecodedFrame frame{};
        frame.width = width;
        frame.height = height;
        frame.source_frame = frame_index;
        frame.pixels.resize(width * height * 4U);
        for (core::u32 y = 0U; y < height; ++y) {
            for (core::u32 x = 0U; x < width; ++x) {
                const auto offset =
                    (static_cast<std::size_t>(y) * width + x) * 4U;
                const bool opaque = source == "opaque";
                const bool cutoff = source == "cutoff";
                frame.pixels[offset + 0U] = opaque ? 0U : 255U;
                frame.pixels[offset + 1U] = opaque ? 255U : 0U;
                frame.pixels[offset + 2U] = 0U;
                // The right half is deliberately translucent rather than
                // opaque: every nonzero premultiplied coverage is a visible
                // surface, while the left half is exact transparent padding.
                frame.pixels[offset + 3U] = opaque
                    ? 255U
                    : (x < width / 2U ? (cutoff ? 127U : 0U) : 128U);
            }
        }
        return frame;
    }
};

struct AdvancedProductPixels final {
    core::u32 width{};
    std::vector<float> depth;
    std::vector<float> tested_depth;
    std::vector<float> coverage;
    std::vector<core::u32> object_id;
    std::vector<std::array<float, 4U>> velocity;

    [[nodiscard]] std::size_t at(core::u32 x, core::u32 y) const noexcept {
        return static_cast<std::size_t>(y) * width + x;
    }
};

[[nodiscard]] bool motion_is_zero(const std::array<float, 4U>& value) noexcept {
    return value[0] == 0.0F && value[1] == 0.0F;
}

void advanced_media_products_follow_sampled_coverage() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    config.debug = true;
    config.frames_in_flight = 2U;
    auto opened = rhi::Device::create(config);
    MEDIA_CHECK(opened);
    auto device = std::move(opened).value();

    constexpr scene::Canvas canvas{64U, 64U};
    auto created = render::SceneRenderer::create(
        device, canvas.width, canvas.height, test_limits(),
        render::SceneRendererMode::AdvancedLinear);
    MEDIA_CHECK(created);
    auto renderer = std::move(created).value();
    render::MediaCache cache{std::make_unique<AuxiliaryProductSource>()};

    const auto record = [&](const scene::Scene& value,
                            const render::SceneRenderOptions& options) {
        const std::size_t pixels =
            static_cast<std::size_t>(canvas.width) * canvas.height;
        auto depth = device.create_readback(pixels * sizeof(float));
        auto tested_depth = device.create_readback(pixels * sizeof(float));
        auto coverage = device.create_readback(pixels * sizeof(float));
        auto object_id = device.create_readback(pixels * sizeof(core::u32));
        auto velocity = device.create_readback(pixels * sizeof(std::array<float, 4U>));
        MEDIA_CHECK(depth && tested_depth && coverage && object_id && velocity);

        auto acquired = device.acquire_commands();
        MEDIA_CHECK(acquired);
        auto commands = std::move(acquired).value();
        cache.begin_frame();
        MEDIA_CHECK(renderer.render(
            commands, value, scene::whole(canvas), options, cache));
        MEDIA_CHECK(renderer.stats().media > 0U);
        MEDIA_CHECK(commands.download(
            renderer.product_depth_target(), depth.value()));
        MEDIA_CHECK(commands.download(
            renderer.depth_target(), tested_depth.value()));
        MEDIA_CHECK(commands.download(
            renderer.coverage_target(), coverage.value()));
        MEDIA_CHECK(commands.download(
            renderer.object_id_target(), object_id.value()));
        MEDIA_CHECK(commands.download(
            renderer.velocity_target(), velocity.value()));
        cache.end_frame();
        auto submitted = device.submit(std::move(commands));
        MEDIA_CHECK(submitted);
        MEDIA_CHECK(std::move(submitted).value().wait());

        AdvancedProductPixels result{};
        result.width = canvas.width;
        result.depth.resize(pixels);
        result.tested_depth.resize(pixels);
        result.coverage.resize(pixels);
        result.object_id.resize(pixels);
        result.velocity.resize(pixels);
        const auto copy = [pixels](auto& readback, auto& destination) {
            const auto bytes = readback.map();
            MEDIA_CHECK(bytes.size()
                        == pixels * sizeof(typename std::remove_reference_t<
                            decltype(destination)>::value_type));
            std::memcpy(
                destination.data(), bytes.data(), bytes.size());
            readback.unmap();
        };
        copy(depth.value(), result.depth);
        copy(tested_depth.value(), result.tested_depth);
        copy(coverage.value(), result.coverage);
        copy(object_id.value(), result.object_id);
        copy(velocity.value(), result.velocity);
        return result;
    };

    auto value = clip_scene(canvas, "coverage");
    value.id = scene::SceneId{901U};
    value.show_frame = 0U;
    value.environment.sky_color = scene::Vec3{};
    render::SceneRenderOptions options{};
    options.transparent_background = true;
    options.history = render::SceneHistoryAction::Reset;
    const AdvancedProductPixels initial = record(value, options);
    const auto left = initial.at(canvas.width / 4U, canvas.height / 2U);
    const auto right = initial.at(
        (canvas.width * 3U) / 4U, canvas.height / 2U);

    // Exact transparent padding contributes nothing to any Advanced product.
    MEDIA_CHECK(initial.coverage[left] == 0.0F);
    MEDIA_CHECK(initial.depth[left] == 0.0F);
    MEDIA_CHECK(initial.object_id[left] == 0U);
    MEDIA_CHECK(motion_is_zero(initial.velocity[left]));
    // The right side is only about half covered. Nonzero visible coverage is
    // still a surface for identity, depth, and motion products.
    MEDIA_CHECK(initial.coverage[right] > 0.45F);
    MEDIA_CHECK(initial.coverage[right] < 0.55F);
    MEDIA_CHECK(initial.depth[right] > 0.0F);
    MEDIA_CHECK(initial.object_id[right] != 0U);
    MEDIA_CHECK(motion_is_zero(initial.velocity[right]));
    // Media composites do not mutate the depth attachment used by the colour
    // pass; the public Advanced product is the coverage-aware reconstruction.
    MEDIA_CHECK(initial.tested_depth[right] == 0.0F);

    value.show_frame = 1U;
    value.media.front().world = scene::multiply(
        scene::translation(scene::Vec3{0.15F, 0.0F, 0.0F}),
        scene::scaling(scene::Vec3{3.0F, 3.0F, 1.0F}));
    value.media.front().normal =
        scene::normal_basis(value.media.front().world);
    options.history = render::SceneHistoryAction::Advance;
    const AdvancedProductPixels moved = record(value, options);
    MEDIA_CHECK(moved.coverage[left] == 0.0F);
    MEDIA_CHECK(moved.depth[left] == 0.0F);
    MEDIA_CHECK(moved.object_id[left] == 0U);
    MEDIA_CHECK(motion_is_zero(moved.velocity[left]));
    MEDIA_CHECK(moved.depth[right] > 0.0F);
    MEDIA_CHECK(moved.object_id[right] == initial.object_id[right]);
    MEDIA_CHECK(!motion_is_zero(moved.velocity[right]));

    // Transparent media reveals the opaque geometry behind it; the covered
    // texel selects the nearer media surface instead.
    auto layered = clip_scene(canvas, "coverage");
    layered.id = scene::SceneId{902U};
    layered.environment.sky_color = scene::Vec3{};
    scene::MeshInstance behind{};
    behind.mesh = scene::MeshKey{scene::Primitive::Quad, 1U};
    behind.source = mediaserver::doc::NodeId{222U};
    behind.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, 0.0F, -0.5F}),
        scene::scaling(scene::Vec3{3.0F, 3.0F, 1.0F}));
    behind.normal = scene::normal_basis(behind.world);
    behind.material.emissive = scene::Vec3{0.0F, 0.0F, 1.0F};
    behind.material.emissive_strength = 1.0F;
    layered.instances.push_back(behind);
    options.history = render::SceneHistoryAction::Reset;
    const AdvancedProductPixels geometry = record(layered, options);
    MEDIA_CHECK(geometry.coverage[left] > 0.99F);
    MEDIA_CHECK(geometry.coverage[right] > 0.99F);
    MEDIA_CHECK(geometry.object_id[left] != 0U);
    MEDIA_CHECK(geometry.object_id[right] != 0U);
    MEDIA_CHECK(geometry.object_id[left] != geometry.object_id[right]);
    MEDIA_CHECK(geometry.depth[right] > geometry.depth[left]);

    // A depth-only clip uses the same composed-coverage 0.5 cutoff in the
    // colour and auxiliary passes. Alpha 127/255 lets the opaque media behind
    // through; alpha 128/255 occludes it without claiming ID or velocity.
    auto occluded = clip_scene(canvas, "cutoff");
    occluded.id = scene::SceneId{903U};
    occluded.environment.sky_color = scene::Vec3{};
    occluded.media.front().source_node = mediaserver::doc::NodeId{310U};
    scene::MediaInstance background = occluded.media.front();
    background.source = "opaque";
    background.source_node = mediaserver::doc::NodeId{311U};
    background.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, 0.0F, -0.5F}),
        scene::scaling(scene::Vec3{3.0F, 3.0F, 1.0F}));
    background.normal = scene::normal_basis(background.world);
    background.compositing_order = 1U;
    occluded.media.push_back(background);
    const std::array depth_only{mediaserver::doc::NodeId{310U}};
    options.depth_only_sources = depth_only;
    const AdvancedProductPixels cutoff = record(occluded, options);
    MEDIA_CHECK(cutoff.coverage[left] > 0.99F);
    MEDIA_CHECK(cutoff.object_id[left] != 0U);
    MEDIA_CHECK(cutoff.depth[left] > 0.0F);
    MEDIA_CHECK(cutoff.coverage[right] == 0.0F);
    MEDIA_CHECK(cutoff.object_id[right] == 0U);
    MEDIA_CHECK(motion_is_zero(cutoff.velocity[right]));
    MEDIA_CHECK(cutoff.depth[right] > cutoff.depth[left]);

    // The cutoff is based on composed coverage, not raw file alpha. Halving
    // the 128/255 texel's layer opacity makes it non-occluding again.
    occluded.media.front().style.opacity = 0.5F;
    const AdvancedProductPixels faded_occluder = record(occluded, options);
    MEDIA_CHECK(faded_occluder.coverage[right] > 0.99F);
    MEDIA_CHECK(faded_occluder.object_id[right] != 0U);
    MEDIA_CHECK(faded_occluder.depth[right] < cutoff.depth[right]);
}

struct BlockingSourceState final {
    std::mutex mutex;
    std::condition_variable changed;
    bool decode_entered{};
    bool caller_returned{};
    bool release_decode{};
    bool forced_release{};
    bool worker_never_entered{};
    core::u32 decode_calls{};
    std::thread::id decode_thread;
    std::vector<core::u32> decoded_frames;
};

// Inherits BackgroundRequired deliberately. If MediaCache ever regresses to
// the inherited synchronous try_decode adapter, decode() enters on the test's
// calling thread and blocks until the deadlock guard releases it.
class BlockingCountingSource final : public render::MediaSource {
public:
    explicit BlockingCountingSource(
        std::shared_ptr<BlockingSourceState> state)
        : state_(std::move(state)) {}

    [[nodiscard]] std::string key(
        const std::string& source, core::u32 frame_index) const override {
        return source + "#" + std::to_string(frame_index);
    }

    [[nodiscard]] render::DecodedFrame decode(
        const std::string&, core::u32 frame_index) override {
        {
            std::unique_lock lock{state_->mutex};
            ++state_->decode_calls;
            state_->decode_thread = std::this_thread::get_id();
            state_->decoded_frames.push_back(frame_index);
            state_->decode_entered = true;
            state_->changed.notify_all();
            state_->changed.wait(
                lock, [&] { return state_->release_decode; });
        }

        render::DecodedFrame frame{};
        frame.width = 4U;
        frame.height = 4U;
        frame.source_frame = frame_index;
        frame.pixels.resize(4U * 4U * 4U, 255U);
        return frame;
    }

private:
    std::shared_ptr<BlockingSourceState> state_;
};

void a_cold_still_decodes_without_blocking_the_resolve_thread() {
    auto device = open_device();
    const auto state = std::make_shared<BlockingSourceState>();
    render::MediaCache cache{
        std::make_unique<BlockingCountingSource>(state)};
    auto acquired = device.acquire_commands();
    MEDIA_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();
    const std::thread::id caller = std::this_thread::get_id();

    // This guard makes a synchronous-regression failure bounded rather than a
    // hung test. Correct code returns from resolve before release_decode is
    // signalled, because only the cache worker can be inside decode().
    std::jthread deadlock_guard{[state] {
        std::unique_lock lock{state->mutex};
        if (!state->changed.wait_for(
                lock, std::chrono::seconds{2},
                [&] { return state->decode_entered; })) {
            state->worker_never_entered = true;
            state->release_decode = true;
            lock.unlock();
            state->changed.notify_all();
            return;
        }
        if (!state->changed.wait_for(
                lock, std::chrono::seconds{2},
                [&] { return state->caller_returned; })) {
            state->forced_release = true;
        }
        state->release_decode = true;
        lock.unlock();
        state->changed.notify_all();
    }};

    cache.begin_frame();
    constexpr render::MediaLoopWindow held_still{0U, 1U};
    const render::ResolvedMediaFrame cold = cache.resolve(
        device, commands, "blocking-still.png", 0U, held_still, 41U);
    {
        std::lock_guard lock{state->mutex};
        state->caller_returned = true;
    }
    state->changed.notify_all();
    deadlock_guard.join();

    MEDIA_CHECK(!cold);
    {
        std::lock_guard lock{state->mutex};
        MEDIA_CHECK(!state->worker_never_entered);
        MEDIA_CHECK(!state->forced_release);
        MEDIA_CHECK(state->decode_calls == 1U);
        MEDIA_CHECK(state->decode_thread != caller);
    }
    cache.end_frame();
    auto cold_submitted = device.submit(std::move(commands));
    MEDIA_CHECK(cold_submitted.has_value());
    MEDIA_CHECK(std::move(cold_submitted).value().wait());

    render::ResolvedMediaFrame ready{};
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!ready && std::chrono::steady_clock::now() < deadline) {
        auto retry_commands = device.acquire_commands();
        MEDIA_CHECK(retry_commands.has_value());
        auto retry = std::move(retry_commands).value();
        cache.begin_frame();
        ready = cache.resolve(
            device, retry, "blocking-still.png", 0U, held_still, 41U);
        if (ready) {
            MEDIA_CHECK(cache.stats().uploads == 1U);
        }
        cache.end_frame();
        auto retry_submitted = device.submit(std::move(retry));
        MEDIA_CHECK(retry_submitted.has_value());
        MEDIA_CHECK(std::move(retry_submitted).value().wait());
        if (!ready) {
            std::this_thread::yield();
        }
    }
    MEDIA_CHECK(ready);
    {
        std::lock_guard lock{state->mutex};
        MEDIA_CHECK(state->decode_calls == 1U);
    }
}

void preloaded_video_survives_playback_and_enters_without_an_upload() {
    auto device = open_device();
    const auto state = std::make_shared<ControlledSourceState>();
    render::MediaCache cache{std::make_unique<ControlledAsyncSource>(state)};
    constexpr render::MediaLoopWindow window{0U, 100U};
    // Keep another placement playing while frame one of the future cut is
    // unavailable, then ready. Speculation must never demand or hold it.
    for (core::u32 pass = 0U; pass < 25U; ++pass) {
        auto acquired = device.acquire_commands();
        MEDIA_CHECK(acquired.has_value());
        auto commands = std::move(acquired).value();
        cache.begin_frame();
        MEDIA_CHECK(cache.resolve(device, commands, "live.mp4", pass + 2U, window, 101U));
        state->frame_one_ready = pass != 0U;
        const auto demands = state->decoded_sources.size();
        const auto uploads = cache.stats().uploads;
        cache.prewarm(device, commands, "future.mp4", 1U, window, 202U);
        // Only one speculative upload per frame, even if many cuts are ready.
        cache.prewarm(device, commands, "other.mp4", 0U, window, 303U);
        MEDIA_CHECK(cache.stats().uploads <= uploads + 1U);
        MEDIA_CHECK(cache.stats().pending_requests == 0U);
        MEDIA_CHECK(state->decoded_sources.size() == demands);
        cache.end_frame();
        auto submitted = device.submit(std::move(commands));
        MEDIA_CHECK(submitted.has_value());
        MEDIA_CHECK(std::move(submitted).value().wait());
    }
    MEDIA_CHECK(std::find(state->prefetched.begin(), state->prefetched.end(), 4U)
                != state->prefetched.end());
    auto acquired = device.acquire_commands();
    MEDIA_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();
    cache.begin_frame();
    const auto demands = state->decoded_sources.size();
    MEDIA_CHECK(cache.resolve(device, commands, "future.mp4", 1U, window, 202U));
    MEDIA_CHECK(cache.stats().cache_hits == 1U);
    MEDIA_CHECK(cache.stats().uploads == 0U);
    MEDIA_CHECK(cache.stats().pending_requests == 0U);
    MEDIA_CHECK(state->decoded_sources.size() == demands);
    cache.end_frame();
    auto submitted = device.submit(std::move(commands));
    MEDIA_CHECK(submitted.has_value());
    MEDIA_CHECK(std::move(submitted).value().wait());
}

void an_off_air_texture_can_be_reused_by_an_upcoming_clip() {
    auto device = open_device();
    const auto state = std::make_shared<ControlledSourceState>();
    render::MediaCache cache{std::make_unique<ControlledAsyncSource>(state)};
    const void* allocation{};
    for (core::u32 pass = 0U; pass < 3U; ++pass) {
        auto acquired = device.acquire_commands();
        MEDIA_CHECK(acquired.has_value());
        auto commands = std::move(acquired).value();
        cache.begin_frame();
        if (pass == 0U) {
            const auto frame = cache.resolve(device, commands, "old.mp4", 0U, {}, 101U);
            MEDIA_CHECK(frame);
            allocation = frame.primary->backend_handle();
        } else if (pass == 2U) {
            // One full pass with no live placement retires its stale hold.
            // The speculative opening upload can reuse that same allocation.
            cache.prewarm(device, commands, "next.mp4", 0U, {}, 202U);
            const auto frame = cache.resolve(device, commands, "next.mp4", 0U, {}, 202U);
            MEDIA_CHECK(frame && frame.primary->backend_handle() == allocation);
            MEDIA_CHECK(cache.stats().resident_frames == 1U);
        }
        cache.end_frame();
        auto submitted = device.submit(std::move(commands));
        MEDIA_CHECK(submitted.has_value());
        MEDIA_CHECK(std::move(submitted).value().wait());
    }
}

void a_preloaded_loop_frame_never_replaces_the_live_held_image() {
    auto device = open_device();
    const auto state = std::make_shared<ControlledSourceState>();
    state->pending_frame = 3U;
    render::MediaCache cache{std::make_unique<ControlledAsyncSource>(state)};
    const void* held_allocation{};
    for (core::u32 pass = 0U; pass < 3U; ++pass) {
        auto acquired = device.acquire_commands();
        MEDIA_CHECK(acquired.has_value());
        auto commands = std::move(acquired).value();
        cache.begin_frame();
        if (pass == 0U) {
            const auto frame = cache.resolve(device, commands, "loop.mp4", 2U, {}, 71U);
            MEDIA_CHECK(frame);
            held_allocation = frame.primary->backend_handle();
        } else {
            cache.prewarm(device, commands, "loop.mp4", 0U,
                          render::MediaLoopWindow{0U, 100U}, 71U);
            MEDIA_CHECK(cache.stats().pending_requests == 0U);
            const auto held = cache.resolve(device, commands, "loop.mp4", 3U, {}, 71U);
            MEDIA_CHECK(held && held.primary->backend_handle() == held_allocation);
            MEDIA_CHECK(cache.stats().held_frames == 1U);
        }
        cache.end_frame();
        auto submitted = device.submit(std::move(commands));
        MEDIA_CHECK(submitted.has_value());
        MEDIA_CHECK(std::move(submitted).value().wait());
    }
    // Cancel another speculative upload. The exact frame may be retried,
    // while the placement's accepted stale answer remains frame two.
    {
        auto acquired = device.acquire_commands();
        MEDIA_CHECK(acquired.has_value());
        auto canceled = std::move(acquired).value();
        cache.begin_frame();
        cache.prewarm(device, canceled, "loop.mp4", 4U, {}, 71U);
        MEDIA_CHECK(cache.stats().uploads == 1U);
        const auto held = cache.resolve(device, canceled, "loop.mp4", 3U, {}, 71U);
        MEDIA_CHECK(held && held.primary->backend_handle() == held_allocation);
        cache.end_frame();
    }
    auto acquired = device.acquire_commands();
    MEDIA_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();
    cache.begin_frame();
    const auto held = cache.resolve(device, commands, "loop.mp4", 3U, {}, 71U);
    MEDIA_CHECK(held && held.primary->backend_handle() == held_allocation);
    MEDIA_CHECK(cache.resolve(device, commands, "loop.mp4", 4U, {}, 71U));
    MEDIA_CHECK(cache.stats().uploads == 1U);
    cache.end_frame();
    auto submitted = device.submit(std::move(commands));
    MEDIA_CHECK(submitted.has_value());
    MEDIA_CHECK(std::move(submitted).value().wait());
}

void a_network_still_preloads_without_blocking_or_demanding_a_frame() {
    auto device = open_device();
    const auto state = std::make_shared<BlockingSourceState>();
    render::MediaCache cache{std::make_unique<BlockingCountingSource>(state)};
    constexpr auto path = R"(\\host\share\still.png)";
    constexpr render::MediaLoopWindow still{0U, 1U};
    auto acquired = device.acquire_commands();
    MEDIA_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();
    const auto caller = std::this_thread::get_id();
    std::jthread guard{[state] {
        std::unique_lock lock{state->mutex};
        if (!state->changed.wait_for(lock, std::chrono::seconds{2},
                [&] { return state->decode_entered; })) state->worker_never_entered = true;
        if (!state->changed.wait_for(lock, std::chrono::seconds{2},
                [&] { return state->caller_returned; })) state->forced_release = true;
        state->release_decode = true;
        lock.unlock();
        state->changed.notify_all();
    }};
    cache.begin_frame();
    cache.prewarm(device, commands, path, 0U, still, 42U);
    {
        std::lock_guard lock{state->mutex};
        state->caller_returned = true;
    }
    state->changed.notify_all();
    guard.join();
    MEDIA_CHECK(cache.stats().pending_requests == 0U);
    {
        std::lock_guard lock{state->mutex};
        MEDIA_CHECK(!state->worker_never_entered && !state->forced_release);
        MEDIA_CHECK(state->decode_thread != caller);
    }
    cache.end_frame();
    auto submitted = device.submit(std::move(commands));
    MEDIA_CHECK(submitted.has_value());
    MEDIA_CHECK(std::move(submitted).value().wait());
    bool uploaded = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!uploaded && std::chrono::steady_clock::now() < deadline) {
        auto next = device.acquire_commands();
        MEDIA_CHECK(next.has_value());
        auto retry = std::move(next).value();
        cache.begin_frame();
        cache.prewarm(device, retry, path, 0U, still, 42U);
        uploaded = cache.stats().uploads == 1U;
        MEDIA_CHECK(cache.stats().pending_requests == 0U);
        cache.end_frame();
        auto result = device.submit(std::move(retry));
        MEDIA_CHECK(result.has_value());
        MEDIA_CHECK(std::move(result).value().wait());
        if (!uploaded) std::this_thread::yield();
    }
    MEDIA_CHECK(uploaded);
    acquired = device.acquire_commands();
    MEDIA_CHECK(acquired.has_value());
    commands = std::move(acquired).value();
    cache.begin_frame();
    MEDIA_CHECK(cache.resolve(device, commands, path, 0U, still, 42U));
    MEDIA_CHECK(cache.stats().uploads == 0U && cache.stats().cache_hits == 1U);
    MEDIA_CHECK(cache.stats().pending_requests == 0U);
    cache.end_frame();
    submitted = device.submit(std::move(commands));
    MEDIA_CHECK(submitted.has_value());
    MEDIA_CHECK(std::move(submitted).value().wait());
    std::lock_guard lock{state->mutex};
    MEDIA_CHECK(state->decode_calls == 1U);
}

void visible_stills_promote_and_displace_speculative_tail_work() {
    auto device = open_device();
    const auto state = std::make_shared<BlockingSourceState>();
    render::MediaCacheConfig config{};
    config.max_read_ahead_requests = 3U;
    render::MediaCache cache{
        std::make_unique<BlockingCountingSource>(state), config};

    const auto enter_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    bool entered = false;
    while (!entered && std::chrono::steady_clock::now() < enter_deadline) {
        cache.prewarm("priority.png", 0U);
        std::unique_lock lock{state->mutex};
        entered = state->changed.wait_for(
            lock, std::chrono::milliseconds{1},
            [&] { return state->decode_entered; });
    }
    if (!entered) {
        std::lock_guard lock{state->mutex};
        state->release_decode = true;
        state->changed.notify_all();
    }
    MEDIA_CHECK(entered);

    // With frame zero in flight, frames one and two fill this cache's bounded
    // speculative allowance. Resolving one promotes it. Resolving new frame
    // three then evicts the newest remaining speculative tail (frame two).
    for (core::u32 retry = 0U; retry < 8U; ++retry) {
        cache.prewarm("priority.png", 1U);
        cache.prewarm("priority.png", 2U);
    }
    auto acquired = device.acquire_commands();
    MEDIA_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();
    cache.begin_frame();
    MEDIA_CHECK(!cache.resolve(
        device, commands, "priority.png", 1U,
        render::MediaLoopWindow{1U, 1U}, 51U));
    MEDIA_CHECK(!cache.resolve(
        device, commands, "priority.png", 3U,
        render::MediaLoopWindow{3U, 1U}, 53U));

    // A different cache's speculative job is a process-queue barrier. Once it
    // starts, every earlier accepted job has started, making the tail-eviction
    // assertion independent of wall-clock sleeps.
    const auto barrier_state = std::make_shared<BlockingSourceState>();
    {
        std::lock_guard lock{barrier_state->mutex};
        barrier_state->release_decode = true;
    }
    render::MediaCache barrier{
        std::make_unique<BlockingCountingSource>(barrier_state)};
    for (core::u32 retry = 0U; retry < 8U; ++retry) {
        barrier.prewarm("priority-barrier.png", 9U);
    }
    {
        std::lock_guard lock{state->mutex};
        state->release_decode = true;
        state->changed.notify_all();
    }
    {
        std::unique_lock lock{barrier_state->mutex};
        MEDIA_CHECK(barrier_state->changed.wait_for(
            lock, std::chrono::seconds{2},
            [&] { return !barrier_state->decoded_frames.empty(); }));
    }
    {
        std::lock_guard lock{state->mutex};
        MEDIA_CHECK(
            state->decoded_frames
            == std::vector<core::u32>{0U, 1U, 3U});
    }
    cache.end_frame();

    auto submitted = device.submit(std::move(commands));
    MEDIA_CHECK(submitted.has_value());
    MEDIA_CHECK(std::move(submitted).value().wait());
}

void offscreen_media_does_not_claim_a_decoder() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};
    auto scn = clip_scene(canvas, "visible.mp4");
    auto offscreen = scn.media.front();
    offscreen.source = "offscreen.mp4";
    offscreen.source_node = mediaserver::doc::NodeId{2U};
    offscreen.world = scene::multiply(
        scene::translation(scene::Vec3{100.0F, 0.0F, 0.0F}),
        offscreen.world);
    offscreen.normal = scene::normal_basis(offscreen.world);
    scn.media.push_back(offscreen);

    const auto state = std::make_shared<ControlledSourceState>();
    state->pending_frame = 99U;
    render::MediaCache cache{
        std::make_unique<ControlledAsyncSource>(state)};
    auto created = render::SceneRenderer::create(
        device, canvas.width, canvas.height, test_limits());
    MEDIA_CHECK(created.has_value());
    auto renderer = std::move(created).value();
    auto acquired = device.acquire_commands();
    MEDIA_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();

    cache.begin_frame();
    MEDIA_CHECK(renderer.render(
        commands, scn, scene::whole(canvas), render::SceneRenderOptions{},
        cache));
    cache.end_frame();
    MEDIA_CHECK(renderer.stats().media == 1U);
    MEDIA_CHECK(renderer.stats().culled_media == 1U);
    MEDIA_CHECK(
        state->decoded_sources == std::vector<std::string>{"visible.mp4"});

    auto submitted = device.submit(std::move(commands));
    MEDIA_CHECK(submitted.has_value());
    MEDIA_CHECK(std::move(submitted).value().wait());
}

void two_renderers_share_one_gpu_media_upload_per_command_frame() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};
    auto scn = clip_scene(canvas, "shared.mp4");
    const auto state = std::make_shared<ControlledSourceState>();
    state->pending_frame = 99U;
    render::MediaCache cache{
        std::make_unique<ControlledAsyncSource>(state)};
    auto first_created = render::SceneRenderer::create(
        device, canvas.width, canvas.height, test_limits());
    auto second_created = render::SceneRenderer::create(
        device, canvas.width, canvas.height, test_limits());
    MEDIA_CHECK(first_created.has_value());
    MEDIA_CHECK(second_created.has_value());
    auto first = std::move(first_created).value();
    auto second = std::move(second_created).value();
    auto acquired = device.acquire_commands();
    MEDIA_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();

    cache.begin_frame();
    MEDIA_CHECK(first.render(
        commands, scn, scene::whole(canvas), render::SceneRenderOptions{},
        cache));
    MEDIA_CHECK(cache.stats().uploads == 1U);
    MEDIA_CHECK(cache.stats().cache_hits == 0U);
    MEDIA_CHECK(second.render(
        commands, scn, scene::whole(canvas), render::SceneRenderOptions{},
        cache));
    MEDIA_CHECK(cache.stats().uploads == 1U);
    MEDIA_CHECK(cache.stats().cache_hits == 1U);
    MEDIA_CHECK(first.stats().media == 1U);
    MEDIA_CHECK(second.stats().media == 1U);
    cache.end_frame();

    auto submitted = device.submit(std::move(commands));
    MEDIA_CHECK(submitted.has_value());
    MEDIA_CHECK(std::move(submitted).value().wait());
}

void canceled_media_uploads_reuse_the_candidate_until_submit() {
    auto device = open_device();
    const auto state = std::make_shared<ControlledSourceState>();
    state->pending_frame = 99U;
    render::MediaCache cache{
        std::make_unique<ControlledAsyncSource>(state)};

    const void* allocation{};
    for (core::u32 attempt = 0U; attempt < 12U; ++attempt) {
        auto acquired = device.acquire_commands();
        MEDIA_CHECK(acquired.has_value());
        auto commands = std::move(acquired).value();
        cache.begin_frame();
        const render::ResolvedMediaFrame first = cache.resolve(
            device, commands, "cancel.mp4", 0U, {}, 61U);
        MEDIA_CHECK(first);
        const render::ResolvedMediaFrame same = cache.resolve(
            device, commands, "cancel.mp4", 0U, {}, 61U);
        MEDIA_CHECK(same.primary == first.primary);
        MEDIA_CHECK(cache.stats().uploads == 1U);
        MEDIA_CHECK(cache.stats().cache_hits == 1U);
        if (allocation == nullptr) {
            allocation = first.primary->backend_handle();
        }
        MEDIA_CHECK(first.primary->backend_handle() == allocation);
        cache.end_frame();
        // Destruction deliberately cancels the recording. The next iteration
        // must re-record into the same bounded candidate, not publish a cache
        // hit whose upload never executed or allocate another texture.
    }

    auto accepted_commands = device.acquire_commands();
    MEDIA_CHECK(accepted_commands.has_value());
    auto accepted = std::move(accepted_commands).value();
    cache.begin_frame();
    const render::ResolvedMediaFrame retried = cache.resolve(
        device, accepted, "cancel.mp4", 0U, {}, 61U);
    MEDIA_CHECK(retried);
    MEDIA_CHECK(retried.primary->backend_handle() == allocation);
    MEDIA_CHECK(cache.stats().uploads == 1U);
    cache.end_frame();
    auto submitted = device.submit(std::move(accepted));
    MEDIA_CHECK(submitted.has_value());
    MEDIA_CHECK(std::move(submitted).value().wait());

    auto published_commands = device.acquire_commands();
    MEDIA_CHECK(published_commands.has_value());
    auto published = std::move(published_commands).value();
    cache.begin_frame();
    const render::ResolvedMediaFrame resident = cache.resolve(
        device, published, "cancel.mp4", 0U, {}, 61U);
    MEDIA_CHECK(resident);
    MEDIA_CHECK(resident.primary->backend_handle() == allocation);
    MEDIA_CHECK(cache.stats().uploads == 0U);
    MEDIA_CHECK(cache.stats().cache_hits == 1U);
    cache.end_frame();
    auto final_submit = device.submit(std::move(published));
    MEDIA_CHECK(final_submit.has_value());
    MEDIA_CHECK(std::move(final_submit).value().wait());
}

void a_cpu_completion_cannot_appear_between_views_of_one_command() {
    auto device = open_device();
    const auto state = std::make_shared<ControlledSourceState>();
    state->pending_frame = 1U;
    render::MediaCache cache{
        std::make_unique<ControlledAsyncSource>(state)};

    auto acquired = device.acquire_commands();
    MEDIA_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();
    cache.begin_frame();
    MEDIA_CHECK(!cache.resolve(
        device, commands, "between-views.mp4", 1U, {}, 63U));
    state->frame_one_ready = true;
    // A later Stage/Program view in the same recording still observes the
    // first view's null answer even though the non-blocking source is ready.
    MEDIA_CHECK(!cache.resolve(
        device, commands, "between-views.mp4", 1U, {}, 63U));
    MEDIA_CHECK(cache.stats().uploads == 0U);
    cache.end_frame();
    auto submitted = device.submit(std::move(commands));
    MEDIA_CHECK(submitted.has_value());
    MEDIA_CHECK(std::move(submitted).value().wait());

    auto next_commands = device.acquire_commands();
    MEDIA_CHECK(next_commands.has_value());
    auto next = std::move(next_commands).value();
    cache.begin_frame();
    MEDIA_CHECK(cache.resolve(
        device, next, "between-views.mp4", 1U, {}, 63U));
    MEDIA_CHECK(cache.stats().uploads == 1U);
    cache.end_frame();
    auto next_submit = device.submit(std::move(next));
    MEDIA_CHECK(next_submit.has_value());
    MEDIA_CHECK(std::move(next_submit).value().wait());
}

void a_submitted_command_cannot_change_another_commands_held_frame() {
    auto device = open_device();
    const auto state = std::make_shared<ControlledSourceState>();
    render::MediaCache cache{
        std::make_unique<ControlledAsyncSource>(state)};
    constexpr core::u64 clip = 64U;

    // Publish frame zero so it is the placement's authoritative held answer.
    auto bootstrap_created = device.acquire_commands();
    MEDIA_CHECK(bootstrap_created.has_value());
    auto bootstrap = std::move(bootstrap_created).value();
    cache.begin_frame();
    const render::ResolvedMediaFrame frame_zero = cache.resolve(
        device, bootstrap, "interleaved.mp4", 0U, {}, clip);
    MEDIA_CHECK(frame_zero);
    const void* const frame_zero_allocation =
        frame_zero.primary->backend_handle();
    cache.end_frame();
    auto bootstrap_submit = device.submit(std::move(bootstrap));
    MEDIA_CHECK(bootstrap_submit.has_value());
    MEDIA_CHECK(std::move(bootstrap_submit).value().wait());

    // Command B misses frame one and freezes frame zero while it remains open.
    auto held_created = device.acquire_commands();
    MEDIA_CHECK(held_created.has_value());
    auto held_commands = std::move(held_created).value();
    cache.begin_frame();
    const render::ResolvedMediaFrame held_first = cache.resolve(
        device, held_commands, "interleaved.mp4", 1U, {}, clip);
    MEDIA_CHECK(held_first);
    MEDIA_CHECK(
        held_first.primary->backend_handle() == frame_zero_allocation);
    cache.end_frame();

    // A different command publishes frame one before B records its next mapped
    // view. B must retain its exact earlier texture, not recompute last_by_clip.
    state->frame_one_ready = true;
    auto publisher_created = device.acquire_commands();
    MEDIA_CHECK(publisher_created.has_value());
    auto publisher = std::move(publisher_created).value();
    cache.begin_frame();
    const render::ResolvedMediaFrame frame_one = cache.resolve(
        device, publisher, "interleaved.mp4", 1U, {}, clip);
    MEDIA_CHECK(frame_one);
    const void* const frame_one_allocation =
        frame_one.primary->backend_handle();
    MEDIA_CHECK(frame_one_allocation != frame_zero_allocation);
    cache.end_frame();
    auto publisher_submit = device.submit(std::move(publisher));
    MEDIA_CHECK(publisher_submit.has_value());
    MEDIA_CHECK(std::move(publisher_submit).value().wait());

    cache.begin_frame();
    const render::ResolvedMediaFrame held_second = cache.resolve(
        device, held_commands, "interleaved.mp4", 1U, {}, clip);
    MEDIA_CHECK(held_second);
    MEDIA_CHECK(
        held_second.primary->backend_handle() == frame_zero_allocation);
    MEDIA_CHECK(
        held_second.primary->backend_handle() != frame_one_allocation);
    cache.end_frame();
    auto held_submit = device.submit(std::move(held_commands));
    MEDIA_CHECK(held_submit.has_value());
    MEDIA_CHECK(std::move(held_submit).value().wait());
}

void canceled_p010_uploads_retain_both_plane_allocations() {
    auto device = open_device();
    render::MediaCache cache{std::make_unique<P010Source>()};

    const void* luma_allocation{};
    const void* chroma_allocation{};
    {
        auto acquired = device.acquire_commands();
        MEDIA_CHECK(acquired.has_value());
        auto commands = std::move(acquired).value();
        cache.begin_frame();
        const render::ResolvedMediaFrame first = cache.resolve(
            device, commands, "cancel-p010.mp4", 0U, {}, 65U);
        MEDIA_CHECK(first && first.planar());
        luma_allocation = first.primary->backend_handle();
        chroma_allocation = first.chroma->backend_handle();
        cache.end_frame();
    }

    auto acquired = device.acquire_commands();
    MEDIA_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();
    cache.begin_frame();
    const render::ResolvedMediaFrame retried = cache.resolve(
        device, commands, "cancel-p010.mp4", 0U, {}, 65U);
    MEDIA_CHECK(retried && retried.planar());
    MEDIA_CHECK(retried.primary->backend_handle() == luma_allocation);
    MEDIA_CHECK(retried.chroma->backend_handle() == chroma_allocation);
    MEDIA_CHECK(cache.stats().uploads == 1U);
    MEDIA_CHECK(cache.stats().planar_uploads == 1U);
    cache.end_frame();
    auto submitted = device.submit(std::move(commands));
    MEDIA_CHECK(submitted.has_value());
    MEDIA_CHECK(std::move(submitted).value().wait());
}

void pending_video_holds_without_overwriting_its_stale_texture() {
    auto device = open_device();
    const auto state = std::make_shared<ControlledSourceState>();
    render::MediaCache cache{
        std::make_unique<ControlledAsyncSource>(state)};

    auto first_commands = device.acquire_commands();
    MEDIA_CHECK(first_commands.has_value());
    auto first = std::move(first_commands).value();
    cache.begin_frame();
    const render::ResolvedMediaFrame frame_zero =
        cache.resolve(device, first, "controlled.mp4", 0U);
    MEDIA_CHECK(frame_zero);
    const void* const allocation = frame_zero.primary->backend_handle();
    const render::ResolvedMediaFrame pending =
        cache.resolve(device, first, "controlled.mp4", 1U);
    MEDIA_CHECK(pending);
    MEDIA_CHECK(pending.primary->backend_handle() == allocation);
    MEDIA_CHECK(cache.stats().pending_requests == 1U);
    MEDIA_CHECK(cache.stats().held_frames == 1U);
    cache.end_frame();
    MEDIA_CHECK(cache.stats().resident_frames == 1U);
    // Residency includes both the texture and its reusable upload staging.
    MEDIA_CHECK(cache.stats().resident_bytes == 4U * 4U * 4U * 2U);
    auto first_submitted = device.submit(std::move(first));
    MEDIA_CHECK(first_submitted.has_value());
    MEDIA_CHECK(std::move(first_submitted).value().wait());

    state->frame_one_ready = true;
    auto next_commands = device.acquire_commands();
    MEDIA_CHECK(next_commands.has_value());
    auto next = std::move(next_commands).value();
    cache.begin_frame();
    const render::ResolvedMediaFrame frame_one =
        cache.resolve(device, next, "controlled.mp4", 1U);
    MEDIA_CHECK(frame_one);
    // Frame zero remains the clip's authoritative stale answer until this
    // command is accepted, so provisional frame one receives another bounded
    // allocation rather than overwriting it before submit.
    MEDIA_CHECK(frame_one.primary->backend_handle() != allocation);
    MEDIA_CHECK(cache.stats().uploads == 1U);
    MEDIA_CHECK(cache.stats().resident_frames == 2U);

    static_cast<void>(cache.resolve(device, next, "controlled.mp4", 3U));
    state->prefetched.clear();
    static_cast<void>(cache.resolve(device, next, "controlled.mp4", 2U));
    cache.end_frame();
    // A backwards step reads backwards, and with no loop range stated it stops
    // at the start of the file rather than wrapping to a frame number that
    // names nothing.
    MEDIA_CHECK(
        state->prefetched == std::vector<core::u32>{1U, 0U});
    auto next_submitted = device.submit(std::move(next));
    MEDIA_CHECK(next_submitted.has_value());
    MEDIA_CHECK(std::move(next_submitted).value().wait());
}

void failed_media_replaces_a_held_image_and_freezes_between_views() {
    auto device = open_device();
    const auto state = std::make_shared<ControlledSourceState>();
    render::MediaCache cache{std::make_unique<ControlledAsyncSource>(state)};
    auto acquired = device.acquire_commands();
    MEDIA_CHECK(acquired);
    auto commands = std::move(acquired).value();
    cache.begin_frame();
    MEDIA_CHECK(cache.resolve(device, commands, "deleted.mp4", 0U));
    cache.end_frame();
    auto submitted = device.submit(std::move(commands));
    MEDIA_CHECK(submitted && std::move(submitted).value().wait());

    state->failed_frame = 1U;
    state->frame_one_ready = true;
    acquired = device.acquire_commands();
    MEDIA_CHECK(acquired);
    commands = std::move(acquired).value();
    cache.begin_frame();
    MEDIA_CHECK(!cache.resolve(device, commands, "deleted.mp4", 1U));
    MEDIA_CHECK(cache.stats().pending_requests == 0U);
    state->failed_frame.reset();
    MEDIA_CHECK(!cache.resolve(device, commands, "deleted.mp4", 1U));
    cache.end_frame();
    submitted = device.submit(std::move(commands));
    MEDIA_CHECK(submitted && std::move(submitted).value().wait());

    acquired = device.acquire_commands();
    MEDIA_CHECK(acquired);
    commands = std::move(acquired).value();
    cache.begin_frame();
    MEDIA_CHECK(cache.resolve(device, commands, "deleted.mp4", 1U));
    cache.end_frame();
    submitted = device.submit(std::move(commands));
    MEDIA_CHECK(submitted && std::move(submitted).value().wait());
}

void a_late_video_frame_establishes_a_truthfully_keyed_hold() {
    auto device = open_device();
    const auto state = std::make_shared<ControlledSourceState>();
    state->pending_frame = 3U;
    state->latest_frame = 2U;
    render::MediaCache cache{
        std::make_unique<ControlledAsyncSource>(state)};

    auto acquired = device.acquire_commands();
    MEDIA_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();
    cache.begin_frame();
    const render::ResolvedMediaFrame late =
        cache.resolve(device, commands, "late.mp4", 3U, {}, 71U);
    MEDIA_CHECK(late);
    MEDIA_CHECK(cache.stats().uploads == 1U);
    MEDIA_CHECK(cache.stats().pending_requests == 1U);
    MEDIA_CHECK(cache.stats().held_frames == 1U);

    // A second viewport reuses frame 2's truthful cache key. It must not upload
    // the same late pixels again or poison the still-pending frame 3 key.
    const render::ResolvedMediaFrame shared =
        cache.resolve(device, commands, "late.mp4", 3U, {}, 71U);
    MEDIA_CHECK(shared.primary == late.primary);
    MEDIA_CHECK(cache.stats().uploads == 1U);
    MEDIA_CHECK(cache.stats().cache_hits == 1U);
    cache.end_frame();

    auto submitted = device.submit(std::move(commands));
    MEDIA_CHECK(submitted.has_value());
    MEDIA_CHECK(std::move(submitted).value().wait());
}

void an_older_late_frame_never_replaces_a_newer_presented_frame() {
    auto device = open_device();
    const auto state = std::make_shared<ControlledSourceState>();
    state->pending_frame = 4U;
    state->frame_one_ready = true;
    render::MediaCache cache{
        std::make_unique<ControlledAsyncSource>(state)};

    auto first_acquired = device.acquire_commands();
    MEDIA_CHECK(first_acquired.has_value());
    auto first_commands = std::move(first_acquired).value();
    cache.begin_frame();
    const render::ResolvedMediaFrame frame_three =
        cache.resolve(device, first_commands, "monotonic.mp4", 3U, {}, 81U);
    MEDIA_CHECK(frame_three);
    const void* const allocation = frame_three.primary->backend_handle();
    cache.end_frame();
    auto first_submitted = device.submit(std::move(first_commands));
    MEDIA_CHECK(first_submitted.has_value());
    MEDIA_CHECK(std::move(first_submitted).value().wait());

    state->frame_one_ready = false;
    state->latest_frame = 2U;
    auto next_acquired = device.acquire_commands();
    MEDIA_CHECK(next_acquired.has_value());
    auto next_commands = std::move(next_acquired).value();
    cache.begin_frame();
    const render::ResolvedMediaFrame held =
        cache.resolve(device, next_commands, "monotonic.mp4", 4U, {}, 81U);
    MEDIA_CHECK(held);
    MEDIA_CHECK(held.primary->backend_handle() == allocation);
    MEDIA_CHECK(cache.stats().uploads == 0U);
    MEDIA_CHECK(cache.stats().pending_requests == 1U);
    MEDIA_CHECK(cache.stats().held_frames == 1U);
    cache.end_frame();
    auto next_submitted = device.submit(std::move(next_commands));
    MEDIA_CHECK(next_submitted.has_value());
    MEDIA_CHECK(std::move(next_submitted).value().wait());
}

void p010_frames_are_sampled_directly_by_the_scene_shader() {
    auto device = open_device();
    render::MediaCache cache{std::make_unique<P010Source>()};
    constexpr scene::Canvas canvas{8U, 8U};
    const Image image = render_clip(
        device, clip_scene(canvas, "p010.mp4"), cache);
    MEDIA_CHECK(cache.stats().uploads == 1U);
    MEDIA_CHECK(cache.stats().planar_uploads == 1U);
    MEDIA_CHECK(cache.stats().resident_bytes == 24U);
    const std::size_t center = image.at(4U, 4U);
    MEDIA_CHECK(image.rgba[center] > 100U && image.rgba[center] < 160U);
    MEDIA_CHECK(std::abs(
        static_cast<int>(image.rgba[center]) - image.rgba[center + 1U]) <= 2);
    MEDIA_CHECK(std::abs(
        static_cast<int>(image.rgba[center + 1U]) - image.rgba[center + 2U]) <= 2);
    MEDIA_CHECK(image.rgba[center + 3U] == 255U);
}

void full_range_p010_preserves_its_code_range() {
    auto device = open_device();
    render::MediaCache cache{std::make_unique<P010Source>(
        render::DecodedPixelLayout::P010Rec709Full,
        static_cast<core::u16>(64U))};
    constexpr scene::Canvas canvas{8U, 8U};
    const Image image = render_clip(
        device, clip_scene(canvas, "p010-full.mp4"), cache);
    const std::size_t center = image.at(4U, 4U);
    // Code 64 is about 16/255 in full range. Limited-range conversion would
    // incorrectly subtract it as black.
    MEDIA_CHECK(image.rgba[center] >= 14U && image.rgba[center] <= 18U);
    MEDIA_CHECK(std::abs(
        static_cast<int>(image.rgba[center]) - image.rgba[center + 1U]) <= 2);
    MEDIA_CHECK(std::abs(
        static_cast<int>(image.rgba[center + 1U]) - image.rgba[center + 2U]) <= 2);
    MEDIA_CHECK(image.rgba[center + 3U] == 255U);
}

void native_nv12_colors_match_rec709_reference_pixels() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) return;
    auto device = open_device(rhi::BackendKind::D3d12);
    constexpr scene::Canvas canvas{8U, 8U};
    struct Color {
        std::array<core::u8, 3U> yuv;
        std::array<core::u8, 3U> rgb;
    };
    // Quantized Rec.709 legal-range black, white, gray and primary bars.
    // These independently specified display values catch swapped chroma,
    // accidental Rec.601 conversion, wrong range and non-opaque alpha.
    constexpr std::array<Color, 6U> colors{{
        {{{16U, 128U, 128U}}, {{0U, 0U, 0U}}},
        {{{235U, 128U, 128U}}, {{255U, 255U, 255U}}},
        {{{126U, 128U, 128U}}, {{128U, 128U, 128U}}},
        {{{63U, 102U, 240U}}, {{255U, 0U, 0U}}},
        {{{173U, 42U, 26U}}, {{0U, 255U, 0U}}},
        {{{32U, 240U, 118U}}, {{0U, 0U, 255U}}},
    }};
    for (const auto& color : colors) {
        render::MediaCache cache{std::make_unique<Nv12ColorSource>(color.yuv)};
        const Image image = render_clip(
            device, clip_scene(canvas, "nv12-colors.mp4"), cache);
        MEDIA_CHECK(cache.stats().planar_uploads == 1U);
        const std::size_t center = image.at(4U, 4U);
        for (std::size_t component = 0U; component < 3U; ++component) {
            MEDIA_CHECK(std::abs(static_cast<int>(image.rgba[center + component])
                - color.rgb[component]) <= 2);
        }
        MEDIA_CHECK(image.rgba[center + 3U] == 255U);
    }
}

// A plate far larger than the view, so clip_scene shows it about 26 source
// texels to an output pixel. Its finest detail is one texel, which a single
// bilinear fetch per pixel cannot represent: it lands on whatever texel the
// pixel centre happens to hit.
enum class FinePattern : core::u8 { Checker, Split };
constexpr core::u32 fine_plate_size = 1024U;

class FineRgbaSource final : public render::MediaSource {
public:
    explicit FineRgbaSource(FinePattern pattern) : pattern_(pattern) {}

    [[nodiscard]] std::string key(
        const std::string& source, core::u32 frame_index) const override {
        return source + "#" + std::to_string(frame_index);
    }

    [[nodiscard]] render::MediaDecodeScheduling scheduling(
        const std::string&) const noexcept override {
        return render::MediaDecodeScheduling::SourceManaged;
    }

    [[nodiscard]] render::DecodedFrame decode(
        const std::string&, core::u32 frame_index) override {
        render::DecodedFrame frame{};
        frame.width = fine_plate_size;
        frame.height = fine_plate_size;
        frame.source_frame = frame_index;
        frame.pixels.resize(
            static_cast<std::size_t>(fine_plate_size) * fine_plate_size * 4U);
        for (core::u32 y = 0U; y < fine_plate_size; ++y) {
            for (core::u32 x = 0U; x < fine_plate_size; ++x) {
                const auto offset =
                    (static_cast<std::size_t>(y) * fine_plate_size + x) * 4U;
                if (pattern_ == FinePattern::Checker) {
                    const core::u8 value = ((x + y) % 2U) == 0U ? 255U : 0U;
                    frame.pixels[offset + 0U] = value;
                    frame.pixels[offset + 1U] = value;
                    frame.pixels[offset + 2U] = value;
                } else {
                    const bool left = x < fine_plate_size / 2U;
                    frame.pixels[offset + 0U] = left ? 255U : 0U;
                    frame.pixels[offset + 1U] = left ? 0U : 255U;
                    frame.pixels[offset + 2U] = 0U;
                }
                frame.pixels[offset + 3U] = 255U;
            }
        }
        return frame;
    }

private:
    FinePattern pattern_;
};

// Legal-range black and white luma alternating per texel, and chroma
// alternating per chroma texel between two opposite colours. Each plane
// averages to neutral grey; a pixel that reads one texel of either shows
// black, white or a colour cast.
class FineNv12Source final : public render::MediaSource {
public:
    [[nodiscard]] std::string key(
        const std::string& source, core::u32 frame_index) const override {
        return source + "#" + std::to_string(frame_index);
    }

    [[nodiscard]] render::MediaDecodeScheduling scheduling(
        const std::string&) const noexcept override {
        return render::MediaDecodeScheduling::SourceManaged;
    }

    [[nodiscard]] render::DecodedFrame decode(
        const std::string&, core::u32 frame_index) override {
        constexpr core::u32 size = fine_plate_size;
        std::vector<core::u8> luma(static_cast<std::size_t>(size) * size);
        std::vector<core::u8> chroma(static_cast<std::size_t>(size) * size / 2U);
        for (core::u32 y = 0U; y < size; ++y) {
            for (core::u32 x = 0U; x < size; ++x) {
                luma[static_cast<std::size_t>(y) * size + x] =
                    ((x + y) % 2U) == 0U ? 235U : 16U;
            }
        }
        for (core::u32 y = 0U; y < size / 2U; ++y) {
            for (core::u32 x = 0U; x < size / 2U; ++x) {
                const bool even = ((x + y) % 2U) == 0U;
                const auto offset = static_cast<std::size_t>(y) * size + x * 2U;
                chroma[offset + 0U] = even ? 64U : 192U;
                chroma[offset + 1U] = even ? 192U : 64U;
            }
        }
        auto result = render::detail::pack_nv12_rec709(
            size, size, true, luma, size, chroma, size);
        result.source_frame = frame_index;
        return result;
    }
};

// Every pixel is within `tolerance` of mid grey in each colour channel.
[[nodiscard]] bool uniformly_mid_grey(const Image& image, int tolerance) {
    for (core::u32 y = 0U; y < image.height; ++y) {
        for (core::u32 x = 0U; x < image.width; ++x) {
            const auto pixel = image.at(x, y);
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                if (std::abs(static_cast<int>(image.rgba[pixel + channel]) - 128)
                    > tolerance) {
                    return false;
                }
            }
        }
    }
    return true;
}

void a_minified_clip_averages_its_footprint_instead_of_aliasing() {
    auto device = open_device();
    render::MediaCache cache{
        std::make_unique<FineRgbaSource>(FinePattern::Checker)};
    constexpr scene::Canvas canvas{32U, 32U};
    const Image image = render_clip(
        device, clip_scene(canvas, "fine-checker.png"), cache);
    // Black and white texels in equal measure under every pixel average to
    // the code midway between them. One fetch per pixel lands on one texel
    // or a two-texel blend, and the picture is a scatter of black, white and
    // in-between values.
    MEDIA_CHECK(uniformly_mid_grey(image, 4));
}

void minified_video_planes_average_their_own_footprints() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) return;
    auto device = open_device(rhi::BackendKind::D3d12);
    render::MediaCache cache{std::make_unique<FineNv12Source>()};
    constexpr scene::Canvas canvas{32U, 32U};
    const Image image = render_clip(
        device, clip_scene(canvas, "fine-nv12.mp4"), cache);
    MEDIA_CHECK(cache.stats().planar_uploads == 1U);
    MEDIA_CHECK(uniformly_mid_grey(image, 4));
}

void a_minified_clip_keeps_its_edges() {
    auto device = open_device();
    render::MediaCache cache{
        std::make_unique<FineRgbaSource>(FinePattern::Split)};
    constexpr scene::Canvas canvas{32U, 32U};
    const Image image = render_clip(
        device, clip_scene(canvas, "fine-split.png"), cache);
    // The filter covers one pixel's footprint and no more, so the red/green
    // boundary stays within the pixel it falls in and at most one neighbour.
    for (core::u32 y = 0U; y < canvas.height; ++y) {
        core::u32 blended = 0U;
        for (core::u32 x = 0U; x < canvas.width; ++x) {
            const auto pixel = image.at(x, y);
            if (image.rgba[pixel + 0U] > 20U && image.rgba[pixel + 1U] > 20U) {
                ++blended;
            }
        }
        MEDIA_CHECK(blended <= 2U);
        const auto left = image.at(canvas.width / 4U, y);
        const auto right = image.at((canvas.width * 3U) / 4U, y);
        MEDIA_CHECK(image.rgba[left + 0U] > 250U && image.rgba[left + 1U] < 5U);
        MEDIA_CHECK(image.rgba[right + 1U] > 250U && image.rgba[right + 0U] < 5U);
    }
}

void read_ahead_wraps_at_the_loop_point() {
    auto device = open_device();
    const auto state = std::make_shared<ControlledSourceState>();
    render::MediaCache cache{
        std::make_unique<ControlledAsyncSource>(state)};
    auto acquired = device.acquire_commands();
    MEDIA_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();
    constexpr render::MediaLoopWindow window{100U, 8U};

    const auto pass = [&](core::u32 frame) {
        cache.begin_frame();
        static_cast<void>(
            cache.resolve(device, commands, "loop.mp4", frame, window));
        cache.end_frame();
    };

    // Settle the stride at one frame per show frame, then land on the clip's
    // last frame.
    pass(106U);
    state->prefetched.clear();
    pass(107U);
    // The frame after the clip's last is the clip's first. Decoding past it
    // reaches the end of the file, produces nothing, and leaves the loop point
    // to be seeked cold on the frame the show actually needs it.
    MEDIA_CHECK(
        state->prefetched
        == std::vector<core::u32>{100U, 101U, 102U});

    // And the wrap itself is one step forward, not a scrub back across the
    // whole clip: the read-ahead keeps pointing the way the show is going.
    state->prefetched.clear();
    pass(100U);
    MEDIA_CHECK(
        state->prefetched
        == std::vector<core::u32>{101U, 102U, 103U});
    auto submitted = device.submit(std::move(commands));
    MEDIA_CHECK(submitted.has_value());
    MEDIA_CHECK(std::move(submitted).value().wait());
}

void read_ahead_is_bounded_across_many_pending_plates() {
    auto device = open_device();
    const auto state = std::make_shared<ControlledSourceState>();
    render::MediaCache cache{
        std::make_unique<ControlledAsyncSource>(state)};
    auto acquired = device.acquire_commands();
    MEDIA_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();

    cache.begin_frame();
    for (core::u64 clip = 1U; clip <= 16U; ++clip) {
        static_cast<void>(cache.resolve(
            device, commands, "plate-" + std::to_string(clip) + ".mov",
            1U, {}, clip));
    }
    cache.end_frame();

    // Unknown video frames are budgeted conservatively at 32 MiB. The 384 MiB
    // speculative budget therefore admits twelve requests, not all three
    // future frames for every plate. Round-based issue gives twelve distinct
    // plates one frame of headroom before any receives a second.
    MEDIA_CHECK(state->prefetched.size() == 12U);
    MEDIA_CHECK(std::all_of(
        state->prefetched.begin(), state->prefetched.end(),
        [](core::u32 frame) { return frame == 2U; }));

    auto submitted = device.submit(std::move(commands));
    MEDIA_CHECK(submitted.has_value());
    MEDIA_CHECK(std::move(submitted).value().wait());
}

void buffered_output_keeps_twelve_future_frames_eligible() {
    auto device = open_device();
    const auto state = std::make_shared<ControlledSourceState>();
    render::MediaCache cache{
        std::make_unique<ControlledAsyncSource>(state),
        render::MediaCacheConfig::buffered_output()};
    auto acquired = device.acquire_commands();
    MEDIA_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();

    cache.begin_frame();
    MEDIA_CHECK(cache.resolve(
        device, commands, "buffered.mp4", 100U, {}, 91U));
    cache.end_frame();

    std::vector<core::u32> expected;
    for (core::u32 frame = 101U; frame <= 112U; ++frame) {
        expected.push_back(frame);
    }
    MEDIA_CHECK(state->prefetched == expected);

    auto submitted = device.submit(std::move(commands));
    MEDIA_CHECK(submitted.has_value());
    MEDIA_CHECK(std::move(submitted).value().wait());
}

void two_clips_on_one_file_never_show_each_other_s_frame() {
    auto device = open_device();
    const auto state = std::make_shared<ControlledSourceState>();
    render::MediaCache cache{
        std::make_unique<ControlledAsyncSource>(state)};
    auto acquired = device.acquire_commands();
    MEDIA_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();
    constexpr core::u64 left_clip = 7U;
    constexpr core::u64 right_clip = 9U;

    cache.begin_frame();
    // The left clip has a picture on screen.
    MEDIA_CHECK(cache.resolve(
        device, commands, "shared.mp4", 0U, {}, left_clip));
    // The right clip is cut from the same file and is still decoding its own
    // first frame. It shows nothing rather than the left clip's picture: they
    // are at different places in the same file, so holding the other one's
    // frame puts a plainly wrong image on the wall.
    MEDIA_CHECK(!cache.resolve(
        device, commands, "shared.mp4", 1U, {}, right_clip));
    MEDIA_CHECK(cache.stats().pending_requests == 1U);
    MEDIA_CHECK(cache.stats().held_frames == 0U);

    // Once the right clip has a frame of its own, that is what it holds while
    // a later one is still decoding.
    MEDIA_CHECK(cache.resolve(
        device, commands, "shared.mp4", 0U, {}, right_clip));
    const render::ResolvedMediaFrame held =
        cache.resolve(device, commands, "shared.mp4", 1U, {}, right_clip);
    MEDIA_CHECK(held);
    MEDIA_CHECK(cache.stats().held_frames == 1U);
    cache.end_frame();

    auto submitted = device.submit(std::move(commands));
    MEDIA_CHECK(submitted.has_value());
    MEDIA_CHECK(std::move(submitted).value().wait());
}

void a_clip_returning_to_air_starts_over_rather_than_resuming() {
    auto device = open_device();
    const auto state = std::make_shared<ControlledSourceState>();
    render::MediaCache cache{
        std::make_unique<ControlledAsyncSource>(state)};
    constexpr render::MediaLoopWindow window{0U, 200U};
    constexpr core::u64 clip = 3U;
    state->pending_frame = 30U;
    state->frame_one_ready = true;

    const auto pass = [&](auto&& body) {
        auto acquired = device.acquire_commands();
        MEDIA_CHECK(acquired.has_value());
        auto commands = std::move(acquired).value();
        cache.begin_frame();
        body(commands);
        cache.end_frame();
        auto submitted = device.submit(std::move(commands));
        MEDIA_CHECK(submitted.has_value());
        MEDIA_CHECK(std::move(submitted).value().wait());
    };

    // A clip placed away from the start of the timeline plays a stretch of its
    // middle and leaves the air on frame 34.
    for (core::u32 frame = 30U; frame <= 34U; ++frame) {
        pass([&](rhi::CommandList& commands) {
            MEDIA_CHECK(cache.resolve(
                device, commands, "placed.mp4", frame, window, clip));
        });
    }
    // The show runs on past the clip: a pass it is not in at all.
    pass([](rhi::CommandList&) {});

    // The show loops and the clip cuts back in at its own first frame, which
    // has not finished decoding.
    state->frame_one_ready = false;
    state->prefetched.clear();
    pass([&](rhi::CommandList& commands) {
        // Frame 34 is where this clip left off, five frames further into the
        // file than the one being asked for. Holding it puts the end of the
        // clip on the wall at the moment it starts.
        MEDIA_CHECK(!cache.resolve(
            device, commands, "placed.mp4", 30U, window, clip));
    });
    // And the read-ahead runs into the clip, not back out of it: the frame it
    // left on is not evidence of which way it is now playing.
    MEDIA_CHECK(
        state->prefetched == std::vector<core::u32>{31U, 32U, 33U});
}

void a_clip_the_show_loops_under_never_holds_a_frame_from_ahead() {
    auto device = open_device();
    const auto state = std::make_shared<ControlledSourceState>();
    render::MediaCache cache{
        std::make_unique<ControlledAsyncSource>(state)};
    // A 250-frame source under a 600-frame show. The show wrapping does not
    // land on this clip's own loop point, so the frame it is asked for jumps
    // backwards by an arbitrary amount while it never leaves the air.
    constexpr render::MediaLoopWindow window{0U, 250U};
    constexpr core::u64 clip = 5U;
    state->pending_frame = 0U;
    state->frame_one_ready = true;

    const auto pass = [&](auto&& body) {
        auto acquired = device.acquire_commands();
        MEDIA_CHECK(acquired.has_value());
        auto commands = std::move(acquired).value();
        cache.begin_frame();
        body(commands);
        cache.end_frame();
        auto submitted = device.submit(std::move(commands));
        MEDIA_CHECK(submitted.has_value());
        MEDIA_CHECK(std::move(submitted).value().wait());
    };

    for (core::u32 frame = 96U; frame <= 99U; ++frame) {
        pass([&](rhi::CommandList& commands) {
            MEDIA_CHECK(cache.resolve(
                device, commands, "spanning.mp4", frame, window, clip));
        });
    }

    // The show loops. This clip is on air on both sides of the wrap, so it is
    // continuing - but not from where it was.
    state->frame_one_ready = false;
    state->prefetched.clear();
    pass([&](rhi::CommandList& commands) {
        MEDIA_CHECK(!cache.resolve(
            device, commands, "spanning.mp4", 0U, window, clip));
    });
    // Play resumes forward from where the show landed. The direction of the
    // jump is the show moving, not the clip playing, so reading back along it
    // walks off the end of the file instead of into the frames coming next.
    MEDIA_CHECK(
        state->prefetched == std::vector<core::u32>{1U, 2U, 3U});
}

struct Case final {
    const char* name;
    void (*function)();
};

void authored_media_generation_refreshes_on_worker_and_replaces_cached_pixels() {
    struct State {
        std::atomic_uint revision{};
        std::atomic_bool refreshed_on_caller{};
        std::thread::id caller{std::this_thread::get_id()};
    };
    class Source final : public render::MediaSource {
    public:
        explicit Source(std::shared_ptr<State> value) : state(std::move(value)) {}
        std::string key(const std::string& path, core::u32) const override { return path; }
        bool refresh(const std::string&) noexcept override {
            if (std::this_thread::get_id() == state->caller) state->refreshed_on_caller = true;
            ++state->revision;
            return true;
        }
        render::DecodedFrame decode(const std::string&, core::u32) override {
            render::DecodedFrame frame;
            frame.width = frame.height = 4;
            frame.pixels.resize(64, 0);
            for (std::size_t i = 0; i < 64; i += 4) {
                frame.pixels[i + (state->revision == 1 ? 0 : 2)] = 255;
                frame.pixels[i + 3] = 255;
            }
            return frame;
        }
        std::shared_ptr<State> state;
    };
    auto device = open_device();
    const auto state = std::make_shared<State>();
    render::MediaCache cache{std::make_unique<Source>(state)};
    scene::MediaInstance clip;
    clip.source = "replaced-still.png";
    const std::array media{clip, clip}; // Refresh once per source, not placement.
    const auto adopt = [&](core::u64 generation) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!cache.adopt_generation(generation, media)) {
            MEDIA_CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
    };
    const auto read = [&]() {
        auto readback = device.create_readback(64);
        MEDIA_CHECK(readback);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        for (;;) {
            auto acquired = device.acquire_commands();
            MEDIA_CHECK(acquired);
            auto commands = std::move(acquired).value();
            cache.begin_frame();
            const auto ready = cache.resolve(device, commands, clip.source, 0, {0, 1}, 42);
            if (ready) MEDIA_CHECK(commands.download(*ready.primary, readback.value()));
            cache.end_frame();
            auto submitted = device.submit(std::move(commands));
            MEDIA_CHECK(submitted && std::move(submitted).value().wait());
            if (ready) break;
            MEDIA_CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        const auto mapped = readback.value().map();
        MEDIA_CHECK(mapped.size() >= 64);
        const std::array<core::u8, 3> rgb{mapped[0], mapped[1], mapped[2]};
        readback.value().unmap();
        return rgb;
    };
    adopt(15);
    MEDIA_CHECK(state->revision == 1 && !state->refreshed_on_caller);
    MEDIA_CHECK((read() == std::array<core::u8, 3>{255, 0, 0}));
    MEDIA_CHECK(cache.stats().uploads == 1);
    adopt(15);
    MEDIA_CHECK((read() == std::array<core::u8, 3>{255, 0, 0}));
    MEDIA_CHECK(state->revision == 1 && cache.stats().uploads == 0);
    adopt(16);
    MEDIA_CHECK(state->revision == 2 && !state->refreshed_on_caller);
    MEDIA_CHECK((read() == std::array<core::u8, 3>{0, 0, 255}));
    MEDIA_CHECK(cache.stats().uploads == 1);
}

// A clip deleted from a scene has to give its memory back, and it did not.
// The eviction that runs when the authored set changes hangs off the asset
// generation key, and that key is zero for the entire life of any project
// without a published package - so for those projects it never ran at all. A
// node went up 800 MiB when one 4K plate was added and stayed there when the
// plate was deleted, until the show was restarted. Residency is the half this
// cache owns; the decoder behind it holds a frames pool at the clip's full
// resolution, so it has to be told as well.
void a_deleted_clip_releases_its_residency_and_tells_its_decoder() {
    struct State final {
        std::mutex guard;
        std::vector<std::string> released;
    };
    class Source final : public render::MediaSource {
    public:
        explicit Source(std::shared_ptr<State> value)
            : state(std::move(value)) {}
        std::string key(const std::string& path,
                        core::u32 frame) const override {
            return path + "#" + std::to_string(frame);
        }
        bool refresh(const std::string& path) noexcept override {
            try {
                const std::lock_guard lock{state->guard};
                state->released.push_back(path);
            } catch (...) {
                return false;
            }
            return true;
        }
        render::DecodedFrame decode(const std::string&, core::u32) override {
            render::DecodedFrame frame;
            frame.width = frame.height = 4;
            frame.pixels.assign(64U, 255U);
            return frame;
        }
        std::shared_ptr<State> state;
    };
    auto device = open_device();
    const auto state = std::make_shared<State>();
    render::MediaCache cache{std::make_unique<Source>(state)};

    scene::MediaInstance kept;
    kept.source = "kept.mp4";
    scene::MediaInstance removed;
    removed.source = "removed.mp4";
    const std::array both{kept, removed};
    const std::array one{kept};

    // Generation zero throughout. That is the whole point: this is the
    // project shape whose media was never reconciled against the scene.
    // Both inside one frame, as two plates on screen together are: a clip
    // resolved in a frame of its own is not on air in the next one.
    const auto resolve = [&](std::span<const scene::MediaInstance> clips) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{5};
        for (;;) {
            auto acquired = device.acquire_commands();
            MEDIA_CHECK(acquired);
            auto commands = std::move(acquired).value();
            cache.begin_frame();
            bool all = true;
            core::u64 clip_id = 1U;
            for (const auto& entry : clips) {
                all = static_cast<bool>(cache.resolve(device, commands,
                                                      entry.source, 0U,
                                                      {0, 1}, clip_id))
                      && all;
                ++clip_id;
            }
            cache.end_frame();
            auto submitted = device.submit(std::move(commands));
            MEDIA_CHECK(submitted && std::move(submitted).value().wait());
            if (all) return;
            MEDIA_CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
    };
    static_cast<void>(cache.adopt_generation(0U, both));
    resolve(both);
    const auto before = cache.stats();
    MEDIA_CHECK(before.resident_frames == 2U && before.resident_bytes > 0U);

    // The scene drops one clip and keeps the other.
    static_cast<void>(cache.adopt_generation(0U, one));
    const auto after = cache.stats();
    MEDIA_CHECK(after.resident_frames == 1U);
    MEDIA_CHECK(after.resident_bytes < before.resident_bytes);

    // And the decoder for the departed clip was told to let go, while the
    // one still on screen was left alone.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{5};
    for (;;) {
        {
            const std::lock_guard lock{state->guard};
            if (std::find(state->released.begin(), state->released.end(),
                          removed.source) != state->released.end()) {
                MEDIA_CHECK(std::find(state->released.begin(),
                                      state->released.end(), kept.source)
                            == state->released.end());
                break;
            }
        }
        MEDIA_CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }

    // The clip still authored keeps its frame: this released one source, not
    // everything the cache happened to be holding.
    resolve(one);
    MEDIA_CHECK(cache.stats().uploads == 0U);
}

// A placement switched to another resolution of its file keeps showing the
// picture it held until the new size has a frame of its own, and the old
// frame is given back once nothing shows it. A placement newly on air under
// the new size inherits nothing: that is a cut, and holds nothing.
void a_resolution_switch_holds_the_last_picture_until_the_new_size_is_ready() {
    struct State final {
        std::atomic_bool half_ready{};
    };
    class Source final : public render::MediaSource {
    public:
        explicit Source(std::shared_ptr<State> value)
            : state(std::move(value)) {}
        std::string key(const std::string& path,
                        core::u32 frame) const override {
            return path + "#" + std::to_string(frame);
        }
        render::MediaDecodeScheduling scheduling(
            const std::string&) const noexcept override {
            return render::MediaDecodeScheduling::SourceManaged;
        }
        render::DecodedFrame decode(const std::string& path,
                                    core::u32) override {
            const bool half = render::media_source_variant(path).half_resolution;
            render::DecodedFrame frame;
            frame.width = frame.height = half ? 4U : 8U;
            frame.pixels.assign(
                static_cast<std::size_t>(frame.width) * frame.height * 4U,
                half ? 200U : 10U);
            return frame;
        }
        std::shared_ptr<const render::DecodedFrame> try_decode(
            const std::string& path, core::u32 frame) override {
            if (render::media_source_variant(path).half_resolution
                && !state->half_ready) {
                return nullptr;
            }
            return std::make_shared<const render::DecodedFrame>(
                decode(path, frame));
        }
        std::shared_ptr<const render::DecodedFrame> try_prefetched(
            const std::string&, core::u32, core::u64) override {
            return nullptr;
        }
        std::shared_ptr<State> state;
    };
    auto device = open_device();
    const auto state = std::make_shared<State>();
    render::MediaCache cache{std::make_unique<Source>(state)};

    constexpr core::u64 placement = 7U;
    constexpr core::u64 cut_in = 8U;
    scene::MediaInstance full;
    full.source = "plate.mp4";
    scene::MediaInstance half = full;
    half.resolution = scene::MediaResolution::Half;
    const std::string half_source = render::media_decode_source(half);
    const std::array full_scene{full};
    const std::array half_scene{half};

    // One command frame: each named placement resolved once, in order.
    const auto frame = [&](const std::string& source, core::u32 index,
                           std::span<const core::u64> placements) {
        auto acquired = device.acquire_commands();
        MEDIA_CHECK(acquired);
        auto commands = std::move(acquired).value();
        cache.begin_frame();
        std::vector<render::ResolvedMediaFrame> resolved;
        for (const core::u64 clip : placements) {
            resolved.push_back(cache.resolve(device, commands, source, index,
                                             {0U, 100U}, clip));
        }
        const auto stats = cache.stats();
        cache.end_frame();
        auto submitted = device.submit(std::move(commands));
        MEDIA_CHECK(submitted && std::move(submitted).value().wait());
        return std::pair{resolved, stats};
    };
    const std::array only{placement};
    const std::array both{placement, cut_in};

    static_cast<void>(cache.adopt_generation(0U, full_scene));
    // The two sizes are told apart by extent: a texture object moves when
    // its upload is published, so its address is no identity.
    const auto extent = [](const render::ResolvedMediaFrame& resolved) {
        return resolved ? resolved.primary->width() : 0U;
    };
    bool shown = false;
    for (core::u32 index = 0U; !shown && index < 8U; ++index) {
        shown = extent(frame(full.source, index, only).first.front()) == 8U;
    }
    MEDIA_CHECK(shown);

    // The switch. The full source is no longer authored, the half one is
    // still decoding: the placement holds its full-size picture for as many
    // frames as that takes, while the clip cutting in under the new size
    // shows nothing rather than another placement's picture.
    static_cast<void>(cache.adopt_generation(0U, half_scene));
    for (core::u32 index = 8U; index < 11U; ++index) {
        const auto [resolved, stats] = frame(half_source, index, both);
        MEDIA_CHECK(extent(resolved[0]) == 8U);
        MEDIA_CHECK(!resolved[1]);
        MEDIA_CHECK(stats.held_frames == 1U);
        MEDIA_CHECK(stats.resident_frames == 1U);
    }

    // The new size lands and replaces the held picture.
    state->half_ready = true;
    bool replaced = false;
    for (core::u32 attempt = 0U; !replaced && attempt < 8U; ++attempt) {
        replaced = extent(frame(half_source, 11U, only).first.front()) == 4U;
    }
    MEDIA_CHECK(replaced);

    // And the full-size frame nothing shows any more is given back.
    const auto [resolved, stats] = frame(half_source, 11U, only);
    MEDIA_CHECK(extent(resolved.front()) == 4U);
    MEDIA_CHECK(stats.resident_frames == 1U);
}

class LargeReadySource final : public render::MediaSource {
public:
    LargeReadySource(core::u32 width, core::u32 height) {
        auto frame = std::make_shared<render::DecodedFrame>();
        frame->width = width;
        frame->height = height;
        frame->pixels.resize(static_cast<std::size_t>(width) * height * 4U);
        for (core::u32 y = 0U; y < height; ++y) {
            for (core::u32 x = 0U; x < width; ++x) {
                const auto offset = (static_cast<std::size_t>(y) * width + x) * 4U;
                frame->pixels[offset] = static_cast<core::u8>(y % 251U);
                frame->pixels[offset + 1U] = static_cast<core::u8>(x % 241U);
                frame->pixels[offset + 2U] = 73U;
                frame->pixels[offset + 3U] = 255U;
            }
        }
        frame_ = std::move(frame);
    }
    std::string key(const std::string& source, core::u32 index) const override {
        return source + "#" + std::to_string(index);
    }
    render::MediaDecodeScheduling scheduling(const std::string&) const noexcept override {
        return render::MediaDecodeScheduling::SourceManaged;
    }
    render::DecodedFrame decode(const std::string&, core::u32) override { return {}; }
    std::shared_ptr<const render::DecodedFrame> try_decode(const std::string&, core::u32) override {
        return frame_;
    }
private:
    std::shared_ptr<const render::DecodedFrame> frame_;
};

void a_6000_pixel_still_uploads_across_recordings_without_partial_publication() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) return;
    auto device = open_device(rhi::BackendKind::D3d12);
    constexpr core::u32 width = 6000U;
    constexpr core::u32 height = 4000U;
    render::MediaCache cache{std::make_unique<LargeReadySource>(width, height)};
    auto readback = device.create_readback(width * height * 4U);
    MEDIA_CHECK(readback);
    bool complete{};
    std::size_t initial_bytes{};
    // First transfer is deliberately canceled; then accepted row ranges must
    // eventually cover the image, including its final row, exactly once.
    for (core::u32 attempt = 0U; attempt < 8U && !complete; ++attempt) {
        auto acquired = device.acquire_commands();
        MEDIA_CHECK(acquired);
        auto commands = std::move(acquired).value();
        cache.begin_frame();
        const auto image = cache.resolve(device, commands, "phone.jpg", 0U, {}, 601U);
        MEDIA_CHECK(cache.stats().resident_frames == 1U);
        if (attempt == 0U) initial_bytes = cache.stats().resident_bytes;
        MEDIA_CHECK(cache.stats().resident_bytes == initial_bytes);
        if (attempt < 2U && device.capabilities().backend == rhi::BackendKind::D3d12) MEDIA_CHECK(!image);
        if (image) MEDIA_CHECK(commands.download(*image.primary, readback.value()));
        cache.end_frame();
        if (attempt == 0U) continue;
        auto submitted = device.submit(std::move(commands));
        MEDIA_CHECK(submitted);
        MEDIA_CHECK(std::move(submitted).value().wait());
        complete = static_cast<bool>(image);
    }
    MEDIA_CHECK(complete);
    const auto bytes = readback.value().map();
    MEDIA_CHECK(bytes.size() == static_cast<std::size_t>(width) * height * 4U);
    for (const core::u32 y : {0U, 1U, 2000U, 3999U}) {
        for (const core::u32 x : {0U, 3000U, 5999U}) {
            const auto offset = (static_cast<std::size_t>(y) * width + x) * 4U;
            MEDIA_CHECK(bytes[offset] == y % 251U);
            MEDIA_CHECK(bytes[offset + 1U] == x % 241U);
            MEDIA_CHECK(bytes[offset + 2U] == 73U);
            MEDIA_CHECK(bytes[offset + 3U] == 255U);
        }
    }
    readback.value().unmap();
}

void three_uhd_playheads_recover_from_shared_upload_budget_exhaustion() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) return;
    auto device = open_device(rhi::BackendKind::D3d12);
    render::MediaCache cache{std::make_unique<LargeReadySource>(3840U, 2160U)};
    std::array<core::u32, 3U> ready_counts{};
    for (core::u32 frame = 0U; frame < 8U; ++frame) {
        auto acquired = device.acquire_commands();
        MEDIA_CHECK(acquired);
        auto commands = std::move(acquired).value();
        cache.begin_frame();
        for (core::u32 clip = 0U; clip < 3U; ++clip) {
            const auto ready = cache.resolve(device, commands,
                "plate-" + std::to_string(clip) + ".mov", frame, {}, clip + 1U);
            if (ready) ++ready_counts[clip];
        }
        MEDIA_CHECK(cache.stats().resident_bytes <= cache.stats().byte_budget);
        cache.end_frame();
        auto submitted = device.submit(std::move(commands));
        MEDIA_CHECK(submitted);
        MEDIA_CHECK(std::move(submitted).value().wait());
    }
    for (const auto count : ready_counts) MEDIA_CHECK(count >= 2U);
}

void backwards_scrubbing_reclaims_abandoned_large_uploads() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) return;
    auto device = open_device(rhi::BackendKind::D3d12);
    render::MediaCache cache{std::make_unique<LargeReadySource>(6000U, 4000U)};
    bool ready{};
    for (const core::u32 frame : {100U, 90U, 80U, 70U, 60U, 60U}) {
        auto acquired = device.acquire_commands();
        MEDIA_CHECK(acquired);
        auto commands = std::move(acquired).value();
        cache.begin_frame();
        ready = static_cast<bool>(cache.resolve(device, commands, "large.mov", frame, {}, 1U));
        MEDIA_CHECK(cache.stats().resident_bytes <= cache.stats().byte_budget);
        cache.end_frame();
        auto submitted = device.submit(std::move(commands));
        MEDIA_CHECK(submitted);
        MEDIA_CHECK(std::move(submitted).value().wait());
    }
    MEDIA_CHECK(ready);
}

constexpr std::array<Case, 48U> cases{{
    {"a resolution switch holds the last picture until the new size is ready",
     &a_resolution_switch_holds_the_last_picture_until_the_new_size_is_ready},
    {"a deleted clip releases its residency and tells its decoder",
     &a_deleted_clip_releases_its_residency_and_tells_its_decoder},
    {"native NV12 colors match Rec709 reference pixels",
     &native_nv12_colors_match_rec709_reference_pixels},
    {"a_minified_clip_averages_its_footprint_instead_of_aliasing",
     &a_minified_clip_averages_its_footprint_instead_of_aliasing},
    {"minified_video_planes_average_their_own_footprints",
     &minified_video_planes_average_their_own_footprints},
    {"a_minified_clip_keeps_its_edges", &a_minified_clip_keeps_its_edges},
    {"a 6000 pixel still uploads across recordings without partial publication",
     &a_6000_pixel_still_uploads_across_recordings_without_partial_publication},
    {"three UHD playheads recover from shared upload budget exhaustion",
     &three_uhd_playheads_recover_from_shared_upload_budget_exhaustion},
    {"backwards scrubbing reclaims abandoned large uploads",
     &backwards_scrubbing_reclaims_abandoned_large_uploads},
    {"an off-air texture can be reused by an upcoming clip",
     &an_off_air_texture_can_be_reused_by_an_upcoming_clip},
    {"a preloaded loop frame never replaces the live held image",
     &a_preloaded_loop_frame_never_replaces_the_live_held_image},
    {"preloaded video survives playback and enters without an upload",
     &preloaded_video_survives_playback_and_enters_without_an_upload},
    {"a network still preloads without blocking or demanding a frame",
     &a_network_still_preloads_without_blocking_or_demanding_a_frame},
    {"authored media refresh runs on worker and replaces pixels", &authored_media_generation_refreshes_on_worker_and_replaces_cached_pixels},
    {"a clip is sampled onto the quad the right way round",
     &a_clip_is_sampled_onto_the_quad_the_right_way_round},
    {"clip opacity and brightness are composited",
     &clip_opacity_and_brightness_are_composited},
    {"depth orders media and lane order breaks ties",
     &depth_orders_media_and_lane_order_breaks_ties},
    {"a crossing clip and card each hide the other",
     &a_crossing_clip_and_card_each_hide_the_other},
    {"clip saturation is identity-exact in both response domains",
     &clip_saturation_is_identity_exact_in_both_response_domains},
    {"clip Levels is identity-exact and authored order is visible",
     &clip_levels_is_identity_exact_and_authored_order_is_visible},
    {"clip Lift/Gamma/Gain is identity-exact and authored order is visible",
     &clip_lift_gamma_gain_is_identity_exact_and_ordered},
    {"clip Contrast/Pivot is identity-exact and authored order is visible",
     &clip_contrast_pivot_is_identity_exact_and_ordered},
    {"clip Color Matrix is identity-exact and authored order is visible",
     &clip_color_matrix_is_identity_exact_and_ordered},
    {"clip White Balance is identity-exact and authored order is visible",
     &clip_white_balance_is_identity_exact_and_ordered},
    {"clip Curves are identity-exact and authored order is visible",
     &clip_curves_are_identity_exact_and_ordered},
    {"clip Film Grain is identity-exact temporal and ordered",
     &clip_film_grain_is_identity_exact_temporal_and_ordered},
    {"a missing clip leaves the scene rather than failing",
     &a_missing_clip_leaves_the_scene_rather_than_failing},
    {"a cold still decodes without blocking the resolve thread",
     &a_cold_still_decodes_without_blocking_the_resolve_thread},
    {"visible stills promote and displace speculative tail work",
     &visible_stills_promote_and_displace_speculative_tail_work},
    {"offscreen media does not claim a decoder",
     &offscreen_media_does_not_claim_a_decoder},
    {"two renderers share one GPU media upload per command frame",
     &two_renderers_share_one_gpu_media_upload_per_command_frame},
    {"canceled media uploads reuse the candidate until submit",
     &canceled_media_uploads_reuse_the_candidate_until_submit},
    {"a CPU completion cannot appear between views of one command",
     &a_cpu_completion_cannot_appear_between_views_of_one_command},
    {"a submitted command cannot change another command's held frame",
     &a_submitted_command_cannot_change_another_commands_held_frame},
    {"canceled P010 uploads retain both plane allocations",
     &canceled_p010_uploads_retain_both_plane_allocations},
    {"pending video holds without overwriting its stale texture",
     &pending_video_holds_without_overwriting_its_stale_texture},
    {"failed media replaces a held image and freezes between views",
     &failed_media_replaces_a_held_image_and_freezes_between_views},
    {"a late video frame establishes a truthfully keyed hold",
     &a_late_video_frame_establishes_a_truthfully_keyed_hold},
    {"an older late frame never replaces a newer presented frame",
     &an_older_late_frame_never_replaces_a_newer_presented_frame},
    {"P010 frames are sampled directly by the scene shader",
     &p010_frames_are_sampled_directly_by_the_scene_shader},
    {"Advanced media products follow sampled coverage",
     &advanced_media_products_follow_sampled_coverage},
    {"full-range P010 preserves its code range",
     &full_range_p010_preserves_its_code_range},
    {"read-ahead wraps at the loop point",
     &read_ahead_wraps_at_the_loop_point},
    {"read-ahead is bounded across many pending plates",
     &read_ahead_is_bounded_across_many_pending_plates},
    {"buffered output keeps twelve future frames eligible",
     &buffered_output_keeps_twelve_future_frames_eligible},
    {"two clips on one file never show each other's frame",
     &two_clips_on_one_file_never_show_each_other_s_frame},
    {"a clip returning to air starts over rather than resuming",
     &a_clip_returning_to_air_starts_over_rather_than_resuming},
    {"a clip the show loops under never holds a frame from ahead",
     &a_clip_the_show_loops_under_never_holds_a_frame_from_ahead},
}};

} // namespace

int main() {
    if (!rhi::Device::available()) {
        std::cout << "[skip] no GPU backend on this machine; media suite skipped\n";
        return 0;
    }
    if (!render::SceneRenderer::shaders_available()) {
        std::cout << "[skip] this build carries no scene shader bytecode; "
                     "media suite skipped\n";
        return 0;
    }
    {
        auto device = open_device();
        if (!renderer_usable(device)) {
            std::cout << "[skip] no shader format this build supports on this "
                         "device; media suite skipped\n";
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
