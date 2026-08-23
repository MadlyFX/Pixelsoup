#include <mediaserver/render/scene_renderer.hpp>

#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/geometry.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// The GPU half of the scene proof. The scene math suite already establishes,
// with no GPU at all, the load-bearing geometric property: adjacent slices
// share a bit-identical frustum plane and every slice projects a point exactly
// where the whole canvas does. What that suite cannot show is that a pipeline
// exists which turns those matrices into pixels, and that two nodes rendering
// two slices reconstruct one image rather than two nearly-aligned ones.
//
// So this suite draws. It needs a GPU and skips loudly without one, exactly as
// the rhi suite does, and it reads its own output back off an offscreen target
// so it needs no display. Its centrepiece is the seam test the implementation
// plan (§10.3) calls the highest-value test in the suite: render the whole
// canvas on one target, render each half on its own, and prove the halves are
// the whole - which is the entire claim of "one scene, many outputs" reduced to
// something a machine can check.
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

#define RENDER_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] rhi::Device open_device() {
    rhi::DeviceConfig config{};
    config.debug = false;
    auto created = rhi::Device::create(config);
    check(created.has_value(), "device creation", __LINE__);
    return std::move(created).value();
}

[[nodiscard]] render::RendererLimits test_limits() noexcept {
    // A box is 24 vertices and 36 indices; a test wants none of the production
    // caps and the memory they reserve.
    render::RendererLimits limits{};
    limits.max_instances = 64U;
    limits.max_vertices = 1U << 14U;
    limits.max_indices = 1U << 15U;
    limits.max_lights = 16U;
    return limits;
}

// A camera looking down -Z at the origin, framed so a unit box fills the middle
// of the canvas and leaves a border of sky.
[[nodiscard]] scene::Camera front_camera() noexcept {
    scene::Camera camera{};
    camera.eye = scene::Vec3{0.0F, 0.0F, 3.0F};
    camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    camera.field_of_view = 0.125F; // 45 degrees
    camera.near_plane = 0.1F;
    camera.far_plane = 100.0F;
    return camera;
}

[[nodiscard]] scene::Environment test_environment() noexcept {
    scene::Environment environment{};
    environment.sky_color = scene::Vec3{0.05F, 0.07F, 0.10F};
    environment.ground_color = scene::Vec3{0.02F, 0.02F, 0.02F};
    environment.ambient = 1.0F;
    return environment;
}

// An empty scene: a canvas, a camera, an environment, and nothing to draw. The
// reference for what "background" is - every pixel of this is the clear.
[[nodiscard]] scene::Scene empty_scene(scene::Canvas canvas) {
    scene::Scene scn{};
    scn.canvas = canvas;
    scn.camera = front_camera();
    scn.environment = test_environment();
    return scn;
}

// One lit box on the origin, small enough to leave a sky border. This is the
// "does it draw at all" scene.
[[nodiscard]] scene::Scene box_scene(scene::Canvas canvas) {
    scene::Scene scn = empty_scene(canvas);

    scene::MeshInstance instance{};
    instance.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    instance.world = scene::identity();
    instance.normal = scene::normal_basis(instance.world);
    instance.material.base_color = scene::Vec3{0.80F, 0.30F, 0.20F};
    instance.material.roughness = 0.5F;
    instance.material.metallic = 0.0F;
    scn.instances.push_back(instance);

    scene::DirectionalLight light{};
    light.direction = scene::Vec3{-0.3F, -0.5F, -0.8F};
    light.color = scene::Vec3{1.0F, 1.0F, 1.0F};
    light.intensity = 3.0F;
    scn.directional_lights.push_back(light);

    return scn;
}

// A box scaled to overfill the frame, lit by an off-centre point light. The
// point of it is the seam test: there is no silhouette against the sky in the
// interior of the frame, so every pixel the two halves share is smoothly shaded
// surface, and the point light's falloff writes a real gradient across it so the
// agreement being checked is agreement about something that varies rather than a
// flat colour two wrong renders could both produce.
[[nodiscard]] scene::Scene wall_scene(scene::Canvas canvas) {
    scene::Scene scn = empty_scene(canvas);

    scene::MeshInstance instance{};
    instance.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    // Pushed back and scaled wide and tall: the front face sits at z = -2.5 and
    // spans well past the frustum edges at that depth, so it covers the frame to
    // the corners for the whole canvas and for either half of it.
    instance.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, 0.0F, -3.0F}),
        scene::scaling(scene::Vec3{12.0F, 9.0F, 1.0F}));
    instance.normal = scene::normal_basis(instance.world);
    instance.material.base_color = scene::Vec3{0.70F, 0.70F, 0.72F};
    instance.material.roughness = 0.6F;
    instance.material.metallic = 0.0F;
    scn.instances.push_back(instance);

    scene::PointLight light{};
    light.position = scene::Vec3{2.0F, 1.0F, -1.0F};
    light.color = scene::Vec3{1.0F, 0.95F, 0.85F};
    light.intensity = 8.0F;
    light.range = 20.0F;
    scn.point_lights.push_back(light);

    return scn;
}

struct RenderedImage final {
    core::u32 width{};
    core::u32 height{};
    std::vector<core::u8> rgba;
    render::RenderStats stats{};
};

// Renders one slice of a scene into a fresh target of `out_width` x `out_height`
// and reads the pixels back. The record, the draw, and the download all ride on
// one command list, so the readback sees exactly what the draw produced.
[[nodiscard]] RenderedImage render_slice(
    rhi::Device& device,
    const scene::Scene& scn,
    scene::CanvasRect slice,
    core::u32 out_width,
    core::u32 out_height) {
    auto created =
        render::SceneRenderer::create(device, out_width, out_height, test_limits());
    check(created.has_value(), "renderer creation", __LINE__);
    auto renderer = std::move(created).value();

    const auto bytes = out_width * out_height * 4U;
    auto readback = device.create_readback(bytes);
    check(readback.has_value(), "readback creation", __LINE__);

    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "acquire commands", __LINE__);
    auto commands = std::move(acquired).value();

    check(renderer.render(commands, scn, slice), "render", __LINE__);
    RenderedImage image{};
    image.width = out_width;
    image.height = out_height;
    image.stats = renderer.stats();

    check(commands.download(renderer.target(), readback.value()), "download", __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "submit", __LINE__);
    check(std::move(submitted).value().wait(), "wait", __LINE__);

    const auto pixels = readback.value().map();
    check(pixels.size() >= bytes, "readback map", __LINE__);
    image.rgba.assign(pixels.begin(), pixels.begin() + bytes);
    readback.value().unmap();
    return image;
}

[[nodiscard]] bool pixels_equal(
    const RenderedImage& image,
    std::size_t a,
    std::size_t b) noexcept {
    for (std::size_t channel = 0U; channel < 4U; ++channel) {
        if (image.rgba[a + channel] != image.rgba[b + channel]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] int channel_difference(int lhs, int rhs) noexcept {
    return lhs > rhs ? lhs - rhs : rhs - lhs;
}

// The build machine may have a GPU whose backend this build carries no bytecode
// for - a Vulkan-only device against a DXIL build. That is the renderer's own
// ShaderFormatUnsupported, and it is a skip, not a failure, exactly as a machine
// with no GPU is.
[[nodiscard]] bool renderer_usable(rhi::Device& device) {
    auto probe = render::SceneRenderer::create(device, 16U, 16U, test_limits());
    if (probe) {
        return true;
    }
    return probe.error() != render::RenderError::ShaderFormatUnsupported;
}

void an_empty_scene_is_the_clear_colour_everywhere() {
    auto device = open_device();
    constexpr scene::Canvas canvas{96U, 64U};
    const auto image =
        render_slice(device, empty_scene(canvas), scene::whole(canvas), 96U, 64U);

    RENDER_CHECK(image.stats.batches == 0U);
    RENDER_CHECK(image.stats.instances == 0U);
    RENDER_CHECK(image.stats.triangles == 0U);

    // Uniform: nothing was drawn, so every pixel is the sky clear. This also
    // pins down what "background" means for the next test.
    for (std::size_t pixel = 4U; pixel < image.rgba.size(); pixel += 4U) {
        RENDER_CHECK(pixels_equal(image, 0U, pixel));
    }
    // Opaque, and not black: an all-zero target would also be uniform.
    RENDER_CHECK(image.rgba[3U] == 255U);
    RENDER_CHECK(image.rgba[0U] + image.rgba[1U] + image.rgba[2U] > 0U);
}

void a_lit_object_covers_part_of_the_frame_and_leaves_sky() {
    auto device = open_device();
    constexpr scene::Canvas canvas{96U, 64U};

    // The clear colour, taken from the corner where the box cannot reach.
    const auto background =
        render_slice(device, empty_scene(canvas), scene::whole(canvas), 96U, 64U);
    const std::array<core::u8, 4U> sky{
        background.rgba[0U], background.rgba[1U],
        background.rgba[2U], background.rgba[3U]};

    const auto image =
        render_slice(device, box_scene(canvas), scene::whole(canvas), 96U, 64U);
    RENDER_CHECK(image.stats.batches == 1U);
    RENDER_CHECK(image.stats.instances == 1U);
    RENDER_CHECK(image.stats.triangles > 0U);
    RENDER_CHECK(image.stats.dropped_instances == 0U);
    RENDER_CHECK(image.stats.dropped_lights == 0U);

    std::size_t drawn = 0U;
    std::size_t background_pixels = 0U;
    const auto total = image.rgba.size() / 4U;
    for (std::size_t pixel = 0U; pixel < image.rgba.size(); pixel += 4U) {
        int distance = 0;
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            distance += channel_difference(
                image.rgba[pixel + channel], sky[channel]);
        }
        if (distance <= 3) {
            ++background_pixels;
        } else {
            ++drawn;
        }
    }

    // Both are present: an object that filled the frame, or one that vanished,
    // would each pass a weaker check. The box sits in the middle of the frame,
    // so it is a real fraction of it and so is the sky around it.
    RENDER_CHECK(drawn > total / 20U);
    RENDER_CHECK(background_pixels > total / 20U);
}

void the_halves_reconstruct_the_whole() {
    auto device = open_device();
    // Even width so the two halves are the same size and their shared seam is a
    // pixel boundary, not a pixel.
    constexpr scene::Canvas canvas{128U, 96U};
    constexpr core::u32 half = canvas.width / 2U;

    const auto scn = wall_scene(canvas);
    const auto whole =
        render_slice(device, scn, scene::whole(canvas), canvas.width, canvas.height);
    const auto left = render_slice(
        device, scn, scene::CanvasRect{0U, 0U, half, canvas.height}, half,
        canvas.height);
    const auto right = render_slice(
        device, scn, scene::CanvasRect{half, 0U, half, canvas.height}, half,
        canvas.height);

    // The scene is meant to vary across the frame; if it did not, agreement
    // would prove nothing. Measure the spread of the green channel across the
    // whole image and require it to be substantial.
    int low = 255;
    int high = 0;
    for (std::size_t pixel = 0U; pixel < whole.rgba.size(); pixel += 4U) {
        const int value = whole.rgba[pixel + 1U];
        low = value < low ? value : low;
        high = value > high ? value : high;
    }
    RENDER_CHECK(high - low > 40);

    // Stitch the halves and compare to the whole. GPU float rasterisation is
    // not bit-exact across two different-but-equal projections, so this is a
    // tolerance, not an equality - but a genuinely wrong slice (the whole canvas
    // squashed into half, say) would diverge across the entire gradient, not by
    // a few least-significant bits. The interior-surface scene keeps silhouette
    // pixels out of the comparison, so the tolerance can stay tight.
    long total_difference = 0;
    std::size_t worst = 0U;
    const auto sample = [](const RenderedImage& image, core::u32 x, core::u32 y) {
        return static_cast<std::size_t>(y) * image.width * 4U
            + static_cast<std::size_t>(x) * 4U;
    };
    for (core::u32 y = 0U; y < canvas.height; ++y) {
        for (core::u32 x = 0U; x < canvas.width; ++x) {
            const auto whole_index = sample(whole, x, y);
            const auto& part = x < half ? left : right;
            const auto part_index = sample(part, x < half ? x : x - half, y);
            int pixel_difference = 0;
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                pixel_difference += channel_difference(
                    whole.rgba[whole_index + channel],
                    part.rgba[part_index + channel]);
            }
            total_difference += pixel_difference;
            worst = pixel_difference > static_cast<int>(worst)
                ? static_cast<std::size_t>(pixel_difference)
                : worst;
        }
    }

    const auto samples =
        static_cast<double>(canvas.width) * static_cast<double>(canvas.height);
    const auto mean = static_cast<double>(total_difference) / samples;
    // Averaged over three channels this is well under one code value per
    // channel. A squashed or misplaced slice would be tens.
    RENDER_CHECK(mean < 3.0);
    // No single interior pixel is wildly off either, which a torn seam would be.
    RENDER_CHECK(worst < 48U);
}

void a_slice_outside_the_canvas_is_refused() {
    auto device = open_device();
    constexpr scene::Canvas canvas{96U, 64U};
    auto created =
        render::SceneRenderer::create(device, 64U, 64U, test_limits());
    RENDER_CHECK(created.has_value());
    auto renderer = std::move(created).value();

    auto acquired = device.acquire_commands();
    RENDER_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();

    const auto scn = box_scene(canvas);
    // Starts at the right edge and runs off it: a feed-map error, refused rather
    // than clamped so the error is visible instead of producing a plausible
    // image of the wrong region.
    RENDER_CHECK(!renderer.render(
        commands, scn, scene::CanvasRect{canvas.width, 0U, 64U, 64U}));
    // A valid slice on the same renderer still draws, so the refusal was the
    // slice and not a wedged command list.
    RENDER_CHECK(renderer.render(
        commands, scn, scene::CanvasRect{0U, 0U, 64U, 64U}));
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 4U> cases{{
    {"an empty scene is the clear colour everywhere",
     &an_empty_scene_is_the_clear_colour_everywhere},
    {"a lit object covers part of the frame and leaves sky",
     &a_lit_object_covers_part_of_the_frame_and_leaves_sky},
    {"the halves reconstruct the whole", &the_halves_reconstruct_the_whole},
    {"a slice outside the canvas is refused",
     &a_slice_outside_the_canvas_is_refused},
}};

} // namespace

int main() {
    if (!rhi::Device::available()) {
        std::cout << "[skip] no GPU backend on this machine; render suite skipped\n";
        return 0;
    }
    if (!render::SceneRenderer::shaders_available()) {
        std::cout
            << "[skip] this build carries no scene shader bytecode; render suite skipped\n";
        return 0;
    }
    {
        auto device = open_device();
        if (!renderer_usable(device)) {
            std::cout << "[skip] no shader format this build supports on this device; "
                         "render suite skipped\n";
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
