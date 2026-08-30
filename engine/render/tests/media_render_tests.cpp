#include <mediaserver/render/media_cache.hpp>
#include <mediaserver/render/scene_renderer.hpp>

#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/geometry.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
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

[[nodiscard]] rhi::Device open_device() {
    rhi::DeviceConfig config{};
    config.debug = false;
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

    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "acquire commands", __LINE__);
    auto commands = std::move(acquired).value();

    check(
        renderer.render(commands, scn, scene::whole(scn.canvas)),
        "render",
        __LINE__);
    check(renderer.stats().media == expected_media, "expected clips drawn", __LINE__);
    check(
        commands.download(renderer.target(), readback.value()),
        "download",
        __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "submit", __LINE__);
    check(std::move(submitted).value().wait(), "wait", __LINE__);

    Image image{};
    image.width = scn.canvas.width;
    image.height = scn.canvas.height;
    const auto pixels = readback.value().map();
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
    // A path that does not exist: the clip draws nothing, but the frame still
    // renders (the sky), and the renderer reports it drew no media.
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
    auto submitted = device.submit(std::move(commands));
    MEDIA_CHECK(submitted.has_value());
    MEDIA_CHECK(std::move(submitted).value().wait());
}

struct ControlledSourceState final {
    core::u32 pending_frame{1U};
    bool frame_one_ready{};
    std::vector<core::u32> prefetched;
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
        if (frame_index == state_->pending_frame && !state_->frame_one_ready) {
            return nullptr;
        }
        return std::make_shared<const render::DecodedFrame>(
            decode(source, frame_index));
    }

    void prefetch(const std::string&, core::u32 frame_index) override {
        state_->prefetched.push_back(frame_index);
    }

private:
    std::shared_ptr<ControlledSourceState> state_;
};

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

void pending_video_holds_then_reuses_the_last_texture() {
    auto device = open_device();
    const auto state = std::make_shared<ControlledSourceState>();
    render::MediaCache cache{
        std::make_unique<ControlledAsyncSource>(state)};

    auto first_commands = device.acquire_commands();
    MEDIA_CHECK(first_commands.has_value());
    auto first = std::move(first_commands).value();
    cache.begin_frame();
    const rhi::Texture* const frame_zero =
        cache.resolve(device, first, "controlled.mp4", 0U);
    MEDIA_CHECK(frame_zero != nullptr);
    const void* const allocation = frame_zero->backend_handle();
    const rhi::Texture* const pending =
        cache.resolve(device, first, "controlled.mp4", 1U);
    MEDIA_CHECK(pending != nullptr);
    MEDIA_CHECK(pending->backend_handle() == allocation);
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
    const rhi::Texture* const frame_one =
        cache.resolve(device, next, "controlled.mp4", 1U);
    MEDIA_CHECK(frame_one != nullptr);
    // Same-size video frames reuse the backing texture allocation.
    MEDIA_CHECK(frame_one->backend_handle() == allocation);
    MEDIA_CHECK(cache.stats().uploads == 1U);
    MEDIA_CHECK(cache.stats().resident_frames == 1U);

    static_cast<void>(cache.resolve(device, next, "controlled.mp4", 3U));
    state->prefetched.clear();
    static_cast<void>(cache.resolve(device, next, "controlled.mp4", 2U));
    // A backwards step reads backwards, and with no loop range stated it stops
    // at the start of the file rather than wrapping to a frame number that
    // names nothing.
    MEDIA_CHECK(
        state->prefetched == std::vector<core::u32>{1U, 0U});
    cache.end_frame();
    auto next_submitted = device.submit(std::move(next));
    MEDIA_CHECK(next_submitted.has_value());
    MEDIA_CHECK(std::move(next_submitted).value().wait());
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

    cache.begin_frame();
    // Settle the stride at one frame per show frame, then land on the clip's
    // last frame.
    static_cast<void>(cache.resolve(device, commands, "loop.mp4", 106U, window));
    state->prefetched.clear();
    static_cast<void>(cache.resolve(device, commands, "loop.mp4", 107U, window));
    // The frame after the clip's last is the clip's first. Decoding past it
    // reaches the end of the file, produces nothing, and leaves the loop point
    // to be seeked cold on the frame the show actually needs it.
    MEDIA_CHECK(
        state->prefetched
        == std::vector<core::u32>{100U, 101U, 102U});

    // And the wrap itself is one step forward, not a scrub back across the
    // whole clip: the read-ahead keeps pointing the way the show is going.
    state->prefetched.clear();
    static_cast<void>(cache.resolve(device, commands, "loop.mp4", 100U, window));
    MEDIA_CHECK(
        state->prefetched
        == std::vector<core::u32>{101U, 102U, 103U});
    cache.end_frame();

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
    MEDIA_CHECK(
        cache.resolve(device, commands, "shared.mp4", 0U, {}, left_clip)
        != nullptr);
    // The right clip is cut from the same file and is still decoding its own
    // first frame. It shows nothing rather than the left clip's picture: they
    // are at different places in the same file, so holding the other one's
    // frame puts a plainly wrong image on the wall.
    MEDIA_CHECK(
        cache.resolve(device, commands, "shared.mp4", 1U, {}, right_clip)
        == nullptr);
    MEDIA_CHECK(cache.stats().pending_requests == 1U);
    MEDIA_CHECK(cache.stats().held_frames == 0U);

    // Once the right clip has a frame of its own, that is what it holds while
    // a later one is still decoding.
    MEDIA_CHECK(
        cache.resolve(device, commands, "shared.mp4", 0U, {}, right_clip)
        != nullptr);
    const rhi::Texture* const held =
        cache.resolve(device, commands, "shared.mp4", 1U, {}, right_clip);
    MEDIA_CHECK(held != nullptr);
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
            MEDIA_CHECK(
                cache.resolve(
                    device, commands, "placed.mp4", frame, window, clip)
                != nullptr);
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
        MEDIA_CHECK(
            cache.resolve(device, commands, "placed.mp4", 30U, window, clip)
            == nullptr);
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
            MEDIA_CHECK(
                cache.resolve(
                    device, commands, "spanning.mp4", frame, window, clip)
                != nullptr);
        });
    }

    // The show loops. This clip is on air on both sides of the wrap, so it is
    // continuing - but not from where it was.
    state->frame_one_ready = false;
    state->prefetched.clear();
    pass([&](rhi::CommandList& commands) {
        MEDIA_CHECK(
            cache.resolve(device, commands, "spanning.mp4", 0U, window, clip)
            == nullptr);
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

constexpr std::array<Case, 18U> cases{{
    {"a clip is sampled onto the quad the right way round",
     &a_clip_is_sampled_onto_the_quad_the_right_way_round},
    {"clip opacity and brightness are composited",
     &clip_opacity_and_brightness_are_composited},
    {"depth orders media and lane order breaks ties",
     &depth_orders_media_and_lane_order_breaks_ties},
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
    {"two renderers share one GPU media upload per command frame",
     &two_renderers_share_one_gpu_media_upload_per_command_frame},
    {"pending video holds then reuses the last texture",
     &pending_video_holds_then_reuses_the_last_texture},
    {"read-ahead wraps at the loop point",
     &read_ahead_wraps_at_the_loop_point},
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
