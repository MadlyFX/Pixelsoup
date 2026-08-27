#include <mediaserver/render/media_cache.hpp>
#include <mediaserver/render/scene_renderer.hpp>

#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/geometry.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

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

[[nodiscard]] render::RendererLimits test_limits() noexcept {
    render::RendererLimits limits{};
    limits.max_instances = 64U;
    limits.max_vertices = 1U << 14U;
    limits.max_indices = 1U << 15U;
    limits.max_lights = 16U;
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
    const scene::Scene& scn) {
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
    check(renderer.stats().media == 1U, "one clip drawn", __LINE__);
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
        if (frame_index == 1U && !state_->frame_one_ready) {
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
    static_cast<void>(cache.resolve(device, next, "controlled.mp4", 2U));
    MEDIA_CHECK(!state->prefetched.empty());
    MEDIA_CHECK(state->prefetched.back() == 1U);
    cache.end_frame();
    auto next_submitted = device.submit(std::move(next));
    MEDIA_CHECK(next_submitted.has_value());
    MEDIA_CHECK(std::move(next_submitted).value().wait());
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 4U> cases{{
    {"a clip is sampled onto the quad the right way round",
     &a_clip_is_sampled_onto_the_quad_the_right_way_round},
    {"clip opacity and brightness are composited",
     &clip_opacity_and_brightness_are_composited},
    {"a missing clip leaves the scene rather than failing",
     &a_missing_clip_leaves_the_scene_rather_than_failing},
    {"pending video holds then reuses the last texture",
     &pending_video_holds_then_reuses_the_last_texture},
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
