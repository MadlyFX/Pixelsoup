#include <mediaserver/render/display_lut_cache.hpp>
#include <mediaserver/render/display_transform.hpp>
#include <mediaserver/render/output_transform.hpp>

#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/scene.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// The GPU half of the output-stage proof. The feed-map model suite establishes,
// with no GPU, that adjacent nodes' regions tile the canvas exactly. What that
// suite cannot show is that a pipeline exists which turns a node's rendered
// region into output pixels, and that two nodes' outputs, placed at their canvas
// positions, tile back into the source with no gap, overlap, or resampling
// seam. This product drives LED walls, so that pixel-exact tiling - not a
// cross-fade - is the seam correctness being checked.
//
// The pass is a straight copy today (the calibration LUT and dither land here
// next), so these tests double as its regression: a source in, the same pixels
// out, and two halves that reassemble the whole.
namespace {

namespace core = mediaserver::core;
namespace rhi = mediaserver::rhi;
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

// A reference image that varies in both axes, so a misplacement or a resampling
// seam has something to disagree about rather than a flat colour two wrong
// passes could both produce.
[[nodiscard]] std::vector<core::u8> gradient(core::u32 width, core::u32 height) {
    std::vector<core::u8> rgba(
        static_cast<std::size_t>(width) * height * 4U, 0U);
    for (core::u32 y = 0U; y < height; ++y) {
        for (core::u32 x = 0U; x < width; ++x) {
            const auto index = (static_cast<std::size_t>(y) * width + x) * 4U;
            rgba[index + 0U] =
                static_cast<core::u8>(40U + (x * 200U) / (width - 1U));
            rgba[index + 1U] =
                static_cast<core::u8>(30U + (y * 200U) / (height - 1U));
            rgba[index + 2U] = static_cast<core::u8>(
                20U + ((x + y) * 200U) / (width + height - 2U));
            rgba[index + 3U] = 255U;
        }
    }
    return rgba;
}

// The sub-rectangle of `source` a node covers, as its own tight image - what
// that node would have rendered or received.
[[nodiscard]] std::vector<core::u8> crop(
    const std::vector<core::u8>& source,
    core::u32 source_width,
    core::u32 x0,
    core::u32 width,
    core::u32 height) {
    std::vector<core::u8> out(
        static_cast<std::size_t>(width) * height * 4U, 0U);
    for (core::u32 y = 0U; y < height; ++y) {
        for (core::u32 x = 0U; x < width; ++x) {
            const auto src =
                (static_cast<std::size_t>(y) * source_width + (x0 + x)) * 4U;
            const auto dst = (static_cast<std::size_t>(y) * width + x) * 4U;
            for (std::size_t channel = 0U; channel < 4U; ++channel) {
                out[dst + channel] = source[src + channel];
            }
        }
    }
    return out;
}

[[nodiscard]] rhi::Texture upload_source(
    rhi::Device& device,
    core::u32 width,
    core::u32 height,
    const std::vector<core::u8>& rgba) {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.sampled = true;
    auto texture = device.create_texture(desc);
    check(texture.has_value(), "source texture", __LINE__);

    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "acquire upload", __LINE__);
    auto commands = std::move(acquired).value();
    check(
        commands.upload(
            texture.value(),
            std::span<const core::u8>{rgba.data(), rgba.size()}),
        "upload source",
        __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "submit upload", __LINE__);
    check(std::move(submitted).value().wait(), "wait upload", __LINE__);
    return std::move(texture).value();
}

struct Image final {
    core::u32 width{};
    core::u32 height{};
    std::vector<core::u8> rgba;

    [[nodiscard]] std::size_t at(core::u32 x, core::u32 y) const noexcept {
        return (static_cast<std::size_t>(y) * width + x) * 4U;
    }
};

// Runs the transform over `source` into an out_width x out_height target and
// reads the pixels back. Record, draw, and download ride one command list.
[[nodiscard]] Image run_transform(
    rhi::Device& device,
    const rhi::Texture& source,
    core::u32 out_width,
    core::u32 out_height,
    mediaserver::scene::CanvasRect viewport) {
    auto created = render::OutputTransform::create(device, out_width, out_height);
    check(created.has_value(), "transform creation", __LINE__);
    auto transform = std::move(created).value();

    const auto bytes = out_width * out_height * 4U;
    auto readback = device.create_readback(bytes);
    check(readback.has_value(), "readback creation", __LINE__);

    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "acquire commands", __LINE__);
    auto commands = std::move(acquired).value();

    check(transform.render(commands, source, viewport), "render", __LINE__);
    check(
        commands.download(transform.target(), readback.value()),
        "download",
        __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "submit", __LINE__);
    check(std::move(submitted).value().wait(), "wait", __LINE__);

    Image image{};
    image.width = out_width;
    image.height = out_height;
    const auto pixels = readback.value().map();
    check(pixels.size() >= bytes, "readback map", __LINE__);
    image.rgba.assign(pixels.begin(), pixels.begin() + bytes);
    readback.value().unmap();
    return image;
}

[[nodiscard]] int channel_difference(int lhs, int rhs) noexcept {
    return lhs > rhs ? lhs - rhs : rhs - lhs;
}

// A build machine may have a GPU whose backend this build carries no bytecode
// for - a Vulkan device against a DXIL build. That is a skip, not a failure.
[[nodiscard]] bool transform_usable(rhi::Device& device) {
    auto probe = render::OutputTransform::create(device, 16U, 16U);
    if (probe) {
        return true;
    }
    return probe.error() != render::RenderError::ShaderFormatUnsupported;
}

// Scene-linear output goes through the baked table, and the shader's lookup
// agrees with the one that baked it.
//
// The reference is `sample_display_lut` - the same manual trilinear the shader
// performs, in the same fixed order. Comparing against it rather than against
// stored pixels is the point: it is the shader's arithmetic being checked, and
// the two implementations of it are asserted not to have drifted.
//
// Every other test in this file drives the display-referred path, so without
// this one the LUT could be wired to nothing and the suite would still pass.
// binary32 to binary16, for writing a scene-linear source by hand.
[[nodiscard]] core::u16 half_bits(float value) noexcept {
    core::u32 bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    const core::u32 sign = (bits >> 16U) & 0x8000U;
    const auto exponent = static_cast<int>((bits >> 23U) & 0xFFU) - 127;
    const core::u32 mantissa = bits & 0x007F'FFFFU;
    if (exponent >= 16) {
        return static_cast<core::u16>(sign | 0x7C00U);
    }
    if (exponent < -14) {
        return static_cast<core::u16>(sign);
    }
    return static_cast<core::u16>(
        sign | (static_cast<core::u32>(exponent + 15) << 10U)
        | (mantissa >> 13U));
}

void scene_linear_output_goes_through_the_baked_table() {
    auto device = open_device();
    constexpr core::u32 width = 16U;
    constexpr core::u32 height = 4U;

    // A premultiplied scene-linear source at alpha one, so the shader's
    // unpremultiply is the identity and what it samples is what is written
    // here. Values above one included, because the table exists to cover them.
    const std::array<float, 4U> levels{{0.0F, 0.18F, 1.0F, 6.0F}};
    std::vector<core::u8> source_bytes(
        static_cast<std::size_t>(width) * height * 8U, 0U);
    for (core::u32 y = 0U; y < height; ++y) {
        for (core::u32 x = 0U; x < width; ++x) {
            const float level = levels[x % levels.size()];
            const float rgb[3U]{level, level * 0.5F, level * 0.25F};
            const std::size_t texel =
                (static_cast<std::size_t>(y) * width + x) * 8U;
            for (core::u32 channel = 0U; channel < 3U; ++channel) {
                const core::u16 half = half_bits(rgb[channel]);
                std::memcpy(source_bytes.data() + texel + channel * 2U, &half,
                            sizeof(half));
            }
            const core::u16 one = half_bits(1.0F);
            std::memcpy(source_bytes.data() + texel + 6U, &one, sizeof(one));
        }
    }

    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba16Float;
    desc.sampled = true;
    auto created_source = device.create_texture(desc);
    check(created_source.has_value(), "linear source", __LINE__);
    auto source = std::move(created_source).value();
    {
        auto acquired = device.acquire_commands();
        check(acquired.has_value(), "acquire upload", __LINE__);
        auto commands = std::move(acquired).value();
        check(commands.upload(source, source_bytes), "upload", __LINE__);
        auto submitted = device.submit(std::move(commands));
        check(submitted.has_value(), "submit upload", __LINE__);
        check(std::move(submitted).value().wait(), "wait upload", __LINE__);
    }

    auto created = render::OutputTransform::create(device, width, height, true);
    check(created.has_value(), "linear transform", __LINE__);
    auto transform = std::move(created).value();
    auto readback = device.create_readback(width * height * 4U);
    check(readback.has_value(), "readback", __LINE__);
    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "acquire", __LINE__);
    auto commands = std::move(acquired).value();
    check(transform.composite(commands, source,
                              mediaserver::scene::CanvasRect{0U, 0U, width,
                                                             height},
                              true),
          "composite", __LINE__);
    check(commands.download(transform.target(), readback.value()), "download",
          __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "submit", __LINE__);
    check(std::move(submitted).value().wait(), "wait", __LINE__);

    Image image{};
    image.width = width;
    image.height = height;
    const auto pixels = readback.value().map();
    check(pixels.size() >= static_cast<std::size_t>(width) * height * 4U,
          "map", __LINE__);
    image.rgba.assign(pixels.begin(),
                      pixels.begin() + static_cast<std::size_t>(width) * height
                          * 4U);
    readback.value().unmap();

    const render::DisplayLut reference = render::bake_display_lut(
        render::DisplayTransform::AcesApproximate, 33U);
    check(reference.valid(), "reference table", __LINE__);

    bool saw_midtone = false;
    for (core::u32 x = 0U; x < width; ++x) {
        const float level = levels[x % levels.size()];
        const float linear[3U]{level, level * 0.5F, level * 0.25F};
        float shaped[3U]{};
        for (core::u32 channel = 0U; channel < 3U; ++channel) {
            shaped[channel] = render::display_shaper(linear[channel]);
        }
        float expected[3U]{};
        render::sample_display_lut(reference, shaped, expected);

        const auto index = image.at(x, 1U);
        for (core::u32 channel = 0U; channel < 3U; ++channel) {
            const int actual = static_cast<int>(image.rgba[index + channel]);
            const int wanted = static_cast<int>(
                expected[channel] * 255.0F + 0.5F);
            // Three code values: the source is binary16, the table is
            // binary16, and the target is eight bits. The tolerance is
            // rounding, not slack - a wrong lookup misses by far more.
            RENDER_CHECK(std::abs(actual - wanted) <= 3);
        }
        if (level > 0.1F && level < 1.5F) {
            // Something in the middle actually came out mid-grey rather than
            // black or clipped, so this is a transform rather than a constant.
            RENDER_CHECK(image.rgba[index] > 40U);
            RENDER_CHECK(image.rgba[index] < 240U);
            saw_midtone = true;
        }
    }
    RENDER_CHECK(saw_midtone);
}

void the_output_pass_copies_the_source_through() {
    auto device = open_device();
    constexpr core::u32 width = 96U;
    constexpr core::u32 height = 64U;
    const auto reference = gradient(width, height);
    const auto source = upload_source(device, width, height, reference);

    // Same size in and out: the output is the source, sampled at texel centres,
    // so it comes back unchanged (within one code value of sampler rounding).
    const auto image = run_transform(
        device, source, width, height,
        mediaserver::scene::CanvasRect{0U, 0U, width, height});
    for (core::u32 y = 0U; y < height; ++y) {
        for (core::u32 x = 0U; x < width; ++x) {
            const auto index = image.at(x, y);
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                RENDER_CHECK(
                    channel_difference(
                        image.rgba[index + channel], reference[index + channel])
                    <= 1);
            }
        }
    }
}

void adjacent_regions_tile_the_source() {
    auto device = open_device();
    // A 256-wide canvas covered by two 128-wide nodes that abut at column 128
    // with no overlap - the LED case.
    constexpr core::u32 canvas_width = 256U;
    constexpr core::u32 height = 64U;
    constexpr core::u32 node_width = canvas_width / 2U;

    const auto reference = gradient(canvas_width, height);

    const auto left_source = upload_source(
        device, node_width, height,
        crop(reference, canvas_width, 0U, node_width, height));
    const auto right_source = upload_source(
        device, node_width, height,
        crop(reference, canvas_width, node_width, node_width, height));

    const mediaserver::scene::CanvasRect full_node{
        0U, 0U, node_width, height};
    const auto left =
        run_transform(device, left_source, node_width, height, full_node);
    const auto right =
        run_transform(device, right_source, node_width, height, full_node);

    // Reassemble the canvas by placing each node's output at its own position.
    // With no blend and no scaling, the two tiles must reconstruct the source
    // pixel-exact - the boundary column of one abutting the next with no gap,
    // no overlap, and no resampling seam.
    std::size_t worst = 0U;
    for (core::u32 y = 0U; y < height; ++y) {
        for (core::u32 x = 0U; x < canvas_width; ++x) {
            const auto ref = (static_cast<std::size_t>(y) * canvas_width + x) * 4U;
            const auto& tile = x < node_width ? left : right;
            const auto local = tile.at(x < node_width ? x : x - node_width, y);
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                const auto difference = channel_difference(
                    tile.rgba[local + channel], reference[ref + channel]);
                worst = static_cast<std::size_t>(difference) > worst
                    ? static_cast<std::size_t>(difference)
                    : worst;
            }
        }
    }
    // No blend, so this is near-exact: at most one code value of sampler
    // rounding. A gap, an overlap, or a squashed tile would be tens.
    RENDER_CHECK(worst <= 1U);
}

void a_viewport_scales_inside_the_fixed_window_and_leaves_black_outside() {
    auto device = open_device();
    constexpr core::u32 source_width = 48U;
    constexpr core::u32 source_height = 24U;
    constexpr core::u32 window_width = 96U;
    constexpr core::u32 window_height = 64U;
    const auto reference = gradient(source_width, source_height);
    const auto source =
        upload_source(device, source_width, source_height, reference);
    const mediaserver::scene::CanvasRect viewport{12U, 20U, 72U, 32U};
    const Image image = run_transform(device, source, window_width,
                                      window_height, viewport);

    const auto black = [&image](core::u32 x, core::u32 y) {
        const std::size_t pixel = image.at(x, y);
        return image.rgba[pixel] == 0U && image.rgba[pixel + 1U] == 0U &&
               image.rgba[pixel + 2U] == 0U &&
               image.rgba[pixel + 3U] == 255U;
    };
    RENDER_CHECK(black(0U, 0U));
    RENDER_CHECK(black(viewport.x - 1U, viewport.y));
    RENDER_CHECK(black(viewport.x + viewport.width, viewport.y));
    RENDER_CHECK(black(viewport.x, viewport.y + viewport.height));

    const std::size_t centre = image.at(
        viewport.x + viewport.width / 2U,
        viewport.y + viewport.height / 2U);
    RENDER_CHECK(image.rgba[centre + 0U] > 0U);
    RENDER_CHECK(image.rgba[centre + 1U] > 0U);
    RENDER_CHECK(image.rgba[centre + 2U] > 0U);
    RENDER_CHECK(image.rgba[centre + 3U] == 255U);
}

void multiple_viewports_composite_into_one_mosaic_window() {
    auto device = open_device();
    constexpr core::u32 source_width = 32U;
    constexpr core::u32 source_height = 24U;
    constexpr core::u32 window_width = 160U;
    constexpr core::u32 window_height = 48U;
    const auto reference = gradient(source_width, source_height);
    const auto source =
        upload_source(device, source_width, source_height, reference);
    auto created =
        render::OutputTransform::create(device, window_width, window_height);
    RENDER_CHECK(created.has_value());
    auto transform = std::move(created).value();
    auto readback = device.create_readback(window_width * window_height * 4U);
    RENDER_CHECK(readback.has_value());
    auto acquired = device.acquire_commands();
    RENDER_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();

    const mediaserver::scene::CanvasRect first{4U, 12U, 56U, 24U};
    const mediaserver::scene::CanvasRect second{100U, 12U, 56U, 24U};
    RENDER_CHECK(transform.clear(commands));
    RENDER_CHECK(transform.composite(commands, source, first));
    RENDER_CHECK(transform.composite(commands, source, second));
    RENDER_CHECK(commands.download(transform.target(), readback.value()));
    auto submitted = device.submit(std::move(commands));
    RENDER_CHECK(submitted.has_value());
    RENDER_CHECK(std::move(submitted).value().wait());

    const auto pixels = readback.value().map();
    const auto at = [](core::u32 x, core::u32 y) {
        return (static_cast<std::size_t>(y) * window_width + x) * 4U;
    };
    const auto lit = [&pixels, &at](core::u32 x, core::u32 y) {
        const std::size_t pixel = at(x, y);
        return pixels[pixel] > 0U && pixels[pixel + 1U] > 0U &&
               pixels[pixel + 2U] > 0U && pixels[pixel + 3U] == 255U;
    };
    const auto black = [&pixels, &at](core::u32 x, core::u32 y) {
        const std::size_t pixel = at(x, y);
        return pixels[pixel] == 0U && pixels[pixel + 1U] == 0U &&
               pixels[pixel + 2U] == 0U && pixels[pixel + 3U] == 255U;
    };
    RENDER_CHECK(lit(first.x + first.width / 2U,
                     first.y + first.height / 2U));
    RENDER_CHECK(lit(second.x + second.width / 2U,
                     second.y + second.height / 2U));
    RENDER_CHECK(black(80U, 24U));
    readback.value().unmap();
}

void studio_mosaic_raster_places_four_viewports_exactly() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) return;
    rhi::DeviceConfig config{};
    config.debug = false;
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    RENDER_CHECK(opened);
    auto device = std::move(opened).value();
    constexpr core::u32 width = 15'360U;
    constexpr core::u32 height = 1'546U;
    const std::vector<core::u8> reference{
        64U, 128U, 192U, 255U, 64U, 128U, 192U, 255U,
        64U, 128U, 192U, 255U, 64U, 128U, 192U, 255U};
    const auto source = upload_source(device, 2U, 2U, reference);
    auto created = render::OutputTransform::create(device, width, height);
    RENDER_CHECK(created);
    auto transform = std::move(created).value();
    auto readback = device.create_readback(width * height * 4U);
    RENDER_CHECK(readback);
    auto acquired = device.acquire_commands();
    RENDER_CHECK(acquired);
    auto commands = std::move(acquired).value();
    RENDER_CHECK(transform.clear(commands));
    for (core::u32 index = 0U; index < 4U; ++index) {
        RENDER_CHECK(transform.composite(commands, source,
            mediaserver::scene::CanvasRect{index * 3'840U, 0U, 3'456U, 1'536U}));
    }
    RENDER_CHECK(commands.download(transform.target(), readback.value()));
    auto submitted = device.submit(std::move(commands));
    RENDER_CHECK(submitted);
    RENDER_CHECK(std::move(submitted).value().wait());
    const auto pixels = readback.value().map();
    RENDER_CHECK(pixels.size() >= static_cast<std::size_t>(width) * height * 4U);
    for (core::u32 y = 0U; y < height; ++y) {
        for (core::u32 x = 0U; x < width; ++x) {
            const bool inside = y < 1'536U && x % 3'840U < 3'456U;
            const auto offset = (static_cast<std::size_t>(y) * width + x) * 4U;
            RENDER_CHECK(pixels[offset] == (inside ? 64U : 0U));
            RENDER_CHECK(pixels[offset + 1U] == (inside ? 128U : 0U));
            RENDER_CHECK(pixels[offset + 2U] == (inside ? 192U : 0U));
            RENDER_CHECK(pixels[offset + 3U] == 255U);
        }
    }
    readback.value().unmap();
}

// binary16 back to binary32, so a reference computation starts from exactly
// the value the GPU sampled rather than the value it was asked to store.
[[nodiscard]] float half_value(core::u16 bits) noexcept {
    const core::u32 sign = static_cast<core::u32>(bits & 0x8000U) << 16U;
    const core::u32 exponent = (bits >> 10U) & 0x1FU;
    const core::u32 mantissa = bits & 0x3FFU;
    core::u32 result{};
    if (exponent == 0U) {
        result = sign;
    } else if (exponent == 31U) {
        result = sign | 0x7F80'0000U | (mantissa << 13U);
    } else {
        result = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
    }
    float value{};
    std::memcpy(&value, &result, sizeof(value));
    return value;
}

// The scene-linear value a wall pixel carries in the gradient below: a
// function of the *absolute* canvas coordinate, so a node's crop of it is the
// same set of numbers the whole-canvas node holds. Values above one included,
// because the table exists to cover them.
[[nodiscard]] std::array<float, 3U> gradient_value(core::u32 canvas_x,
                                                   core::u32 canvas_y,
                                                   core::u32 canvas_width) {
    const float along =
        static_cast<float>(canvas_x) / static_cast<float>(canvas_width - 1U);
    return {0.02F + 1.6F * along, (0.02F + 1.6F * along) * 0.6F,
            0.3F + 0.02F * static_cast<float>(canvas_y)};
}

// Uploads a premultiplied RGBA16F source covering canvas columns `x0` onwards.
[[nodiscard]] rhi::Texture upload_linear(rhi::Device& device, core::u32 width,
                                         core::u32 height, core::u32 x0,
                                         core::u32 canvas_width,
                                         const float* flat = nullptr) {
    std::vector<core::u8> bytes(
        static_cast<std::size_t>(width) * height * 8U, 0U);
    for (core::u32 y = 0U; y < height; ++y) {
        for (core::u32 x = 0U; x < width; ++x) {
            const std::array<float, 3U> value = flat != nullptr
                ? std::array<float, 3U>{flat[0U], flat[1U], flat[2U]}
                : gradient_value(x0 + x, y, canvas_width);
            const std::size_t texel =
                (static_cast<std::size_t>(y) * width + x) * 8U;
            for (core::u32 channel = 0U; channel < 3U; ++channel) {
                const core::u16 half = half_bits(value[channel]);
                std::memcpy(bytes.data() + texel + channel * 2U, &half,
                            sizeof(half));
            }
            const core::u16 one = half_bits(1.0F);
            std::memcpy(bytes.data() + texel + 6U, &one, sizeof(one));
        }
    }
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba16Float;
    desc.sampled = true;
    auto created = device.create_texture(desc);
    check(created.has_value(), "linear source", __LINE__);
    auto source = std::move(created).value();
    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "acquire upload", __LINE__);
    auto commands = std::move(acquired).value();
    check(commands.upload(source, bytes), "upload", __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "submit upload", __LINE__);
    check(std::move(submitted).value().wait(), "wait upload", __LINE__);
    return source;
}

// One node's output: a fresh transform at the node's extent, the project's
// colour adopted once before the draw, the whole window showing `region` of
// the canvas. Exactly what a presenter does per frame.
[[nodiscard]] Image run_linear(rhi::Device& device, const rhi::Texture& source,
                               core::u32 width, core::u32 height,
                               mediaserver::scene::CanvasRect region,
                               const mediaserver::scene::ColorSettings& color,
                               render::DisplayLutState* state = nullptr) {
    auto created = render::OutputTransform::create(device, width, height, true);
    check(created.has_value(), "linear transform", __LINE__);
    auto transform = std::move(created).value();
    transform.adopt_color(color);
    if (state != nullptr) {
        *state = transform.color_state();
    }
    auto readback = device.create_readback(width * height * 4U);
    check(readback.has_value(), "readback", __LINE__);
    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "acquire", __LINE__);
    auto commands = std::move(acquired).value();
    check(transform.composite(commands, source,
                              mediaserver::scene::CanvasRect{0U, 0U, width,
                                                             height},
                              true, region),
          "composite", __LINE__);
    check(commands.download(transform.target(), readback.value()), "download",
          __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "submit", __LINE__);
    check(std::move(submitted).value().wait(), "wait", __LINE__);
    Image image{};
    image.width = width;
    image.height = height;
    const auto pixels = readback.value().map();
    const std::size_t bytes = static_cast<std::size_t>(width) * height * 4U;
    check(pixels.size() >= bytes, "map", __LINE__);
    image.rgba.assign(pixels.begin(), pixels.begin() + bytes);
    readback.value().unmap();
    return image;
}

// Two nodes dither one wall identically.
//
// The whole point of keying the pattern to the absolute canvas pixel. A node
// showing the right half of the wall adds its region's origin to its own pixel
// coordinates and lands on the same threshold, and therefore the same code
// value, as the node showing all of it - byte for byte, which is the standard
// every other cross-node claim in this renderer is held to.
void two_nodes_dither_one_wall_identically() {
    auto device = open_device();
    constexpr core::u32 canvas_width = 96U;
    constexpr core::u32 height = 16U;
    constexpr core::u32 half = canvas_width / 2U;
    const auto whole_source =
        upload_linear(device, canvas_width, height, 0U, canvas_width);
    const auto left_source =
        upload_linear(device, half, height, 0U, canvas_width);
    const auto right_source =
        upload_linear(device, half, height, half, canvas_width);

    // Dither is on by default; nothing else authored.
    const mediaserver::scene::ColorSettings dithered{};
    const Image whole = run_linear(
        device, whole_source, canvas_width, height,
        mediaserver::scene::CanvasRect{0U, 0U, canvas_width, height},
        dithered);
    const Image left = run_linear(
        device, left_source, half, height,
        mediaserver::scene::CanvasRect{0U, 0U, half, height}, dithered);
    const Image right = run_linear(
        device, right_source, half, height,
        mediaserver::scene::CanvasRect{half, 0U, half, height}, dithered);
    for (core::u32 y = 0U; y < height; ++y) {
        for (core::u32 x = 0U; x < canvas_width; ++x) {
            const Image& tile = x < half ? left : right;
            const std::size_t mine = whole.at(x, y);
            const std::size_t theirs = tile.at(x < half ? x : x - half, y);
            for (core::u32 channel = 0U; channel < 4U; ++channel) {
                RENDER_CHECK(whole.rgba[mine + channel]
                             == tile.rgba[theirs + channel]);
            }
        }
    }

    // The dither is live: switching it off changes some pixels, and never by
    // more than the one code value a half-step threshold can move.
    mediaserver::scene::ColorSettings plain{};
    plain.dither = false;
    const Image flat = run_linear(
        device, whole_source, canvas_width, height,
        mediaserver::scene::CanvasRect{0U, 0U, canvas_width, height}, plain);
    std::size_t differing = 0U;
    int worst = 0;
    for (std::size_t index = 0U; index < whole.rgba.size(); ++index) {
        const int difference = channel_difference(whole.rgba[index],
                                                  flat.rgba[index]);
        if (difference != 0) {
            ++differing;
        }
        worst = std::max(worst, difference);
    }
    RENDER_CHECK(differing > 0U);
    RENDER_CHECK(worst <= 1);

    // And it is the reference arithmetic: the baked table sampled the way the
    // shader samples it, then the threshold for the wall pixel, then the
    // quantisation. The tolerance is the binary16 table's own error crossing
    // a rounding boundary, not slack - a wrong origin or a mirrored matrix
    // misses by whole code values across the image.
    const render::DisplayLut reference = render::bake_display_lut(
        render::DisplayTransform::AcesApproximate, 33U);
    for (core::u32 y = 0U; y < height; ++y) {
        for (core::u32 x = 0U; x < canvas_width; ++x) {
            const std::array<float, 3U> value =
                gradient_value(x, y, canvas_width);
            float shaped[3U]{};
            for (core::u32 channel = 0U; channel < 3U; ++channel) {
                shaped[channel] = render::display_shaper(
                    half_value(half_bits(value[channel])));
            }
            float encoded[3U]{};
            render::sample_display_lut(reference, shaped, encoded);
            const float threshold = render::output_dither_threshold(x, y);
            const std::size_t index = whole.at(x, y);
            for (core::u32 channel = 0U; channel < 3U; ++channel) {
                const float quantised = render::quantize_with_dither(
                    encoded[channel], threshold, 255U);
                const int wanted =
                    static_cast<int>(std::lround(quantised * 255.0F));
                RENDER_CHECK(std::abs(static_cast<int>(whole.rgba[index + channel])
                                      - wanted) <= 2);
            }
        }
    }
}

// An authored look reaches the output, and a missing one says so.
//
// The table is baked off the frame path, so the first frame after a look is
// authored reports Pending and shows the resident table; the frame after the
// bake shows the look. A path that does not exist reports Failed and shows the
// transform alone - byte for byte what the default shows - so the wall is a
// predictable picture while the operator reads the reason.
void an_authored_look_reaches_the_output_and_a_missing_one_says_so() {
    auto device = open_device();
    namespace fs = std::filesystem;
    const fs::path directory =
        fs::temp_directory_path() / "mediaserver-output-look-tests";
    fs::create_directories(directory);
    const fs::path look_path = directory / "half-red.cube";
    const std::string look_text =
        "LUT_3D_SIZE 2\n"
        "0.0 0.0 0.0\n"
        "0.5 0.0 0.0\n"
        "0.0 1.0 0.0\n"
        "0.5 1.0 0.0\n"
        "0.0 0.0 1.0\n"
        "0.5 0.0 1.0\n"
        "0.0 1.0 1.0\n"
        "0.5 1.0 1.0\n";
    {
        std::ofstream out{look_path, std::ios::binary};
        out << look_text;
    }

    constexpr core::u32 width = 16U;
    constexpr core::u32 height = 4U;
    // A flat scene-linear grey of one: display white through either transform,
    // so the look's effect on red is unmistakable.
    const float grey[3U]{1.0F, 1.0F, 1.0F};
    const auto source = upload_linear(device, width, height, 0U, width, grey);
    const mediaserver::scene::CanvasRect region{0U, 0U, width, height};

    mediaserver::scene::ColorSettings plain{};
    plain.dither = false;
    const Image reference =
        run_linear(device, source, width, height, region, plain);

    mediaserver::scene::ColorSettings graded{};
    graded.show_lut_source = look_path.string();
    graded.dither = false;
    render::DisplayLutState first{};
    const Image before =
        run_linear(device, source, width, height, region, graded, &first);
    RENDER_CHECK(first == render::DisplayLutState::Pending);
    // Not yet graded: the resident table stands until the bake lands.
    RENDER_CHECK(before.rgba == reference.rgba);

    render::DisplayLutCache cache;
    RENDER_CHECK(cache.wait_for_test());
    render::DisplayLutState after{};
    const Image with_look =
        run_linear(device, source, width, height, region, graded, &after);
    RENDER_CHECK(after == render::DisplayLutState::Ready);
    const std::size_t centre = with_look.at(width / 2U, height / 2U);
    // Red halved, green and blue untouched.
    RENDER_CHECK(with_look.rgba[centre] * 2U
                 <= reference.rgba[centre] + 4U);
    RENDER_CHECK(with_look.rgba[centre] * 2U + 4U
                 >= reference.rgba[centre]);
    RENDER_CHECK(channel_difference(with_look.rgba[centre + 1U],
                                    reference.rgba[centre + 1U]) <= 1);
    RENDER_CHECK(channel_difference(with_look.rgba[centre + 2U],
                                    reference.rgba[centre + 2U]) <= 1);
    // And exactly the composed bake, sampled the way the shader samples it.
    const render::DisplayLut look = render::parse_cube_lut(look_text);
    const render::DisplayLut composed = render::bake_display_lut(
        render::DisplayTransform::AcesApproximate, 33U, &look);
    const float shaped = render::display_shaper(1.0F);
    const float white[3U]{shaped, shaped, shaped};
    float expected[3U]{};
    render::sample_display_lut(composed, white, expected);
    for (core::u32 channel = 0U; channel < 3U; ++channel) {
        const int wanted =
            static_cast<int>(std::lround(expected[channel] * 255.0F));
        RENDER_CHECK(std::abs(static_cast<int>(with_look.rgba[centre + channel])
                              - wanted) <= 3);
    }

    // A path that does not exist.
    mediaserver::scene::ColorSettings missing{};
    missing.show_lut_source = (directory / "missing.cube").string();
    missing.dither = false;
    static_cast<void>(
        run_linear(device, source, width, height, region, missing));
    RENDER_CHECK(cache.wait_for_test());
    render::DisplayLutState state{};
    const Image fallback =
        run_linear(device, source, width, height, region, missing, &state);
    RENDER_CHECK(state == render::DisplayLutState::Failed);
    RENDER_CHECK(fallback.rgba == reference.rgba);

    std::error_code ignored;
    fs::remove_all(directory, ignored);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 8U> cases{{
    {"studio Mosaic raster has exact viewports and black gaps",
     &studio_mosaic_raster_places_four_viewports_exactly},
    {"the output pass copies the source through",
     &the_output_pass_copies_the_source_through},
    {"scene linear output goes through the baked table",
     &scene_linear_output_goes_through_the_baked_table},
    {"adjacent regions tile the source",
     &adjacent_regions_tile_the_source},
    {"a viewport scales inside the fixed window and leaves black outside",
     &a_viewport_scales_inside_the_fixed_window_and_leaves_black_outside},
    {"multiple viewports composite into one Mosaic window",
     &multiple_viewports_composite_into_one_mosaic_window},
    {"two nodes dither one wall identically",
     &two_nodes_dither_one_wall_identically},
    {"an authored look reaches the output and a missing one says so",
     &an_authored_look_reaches_the_output_and_a_missing_one_says_so},
}};

} // namespace

int main() {
    if (!rhi::Device::available()) {
        std::cout
            << "[skip] no GPU backend on this machine; output suite skipped\n";
        return 0;
    }
    if (!render::OutputTransform::shaders_available()) {
        std::cout << "[skip] this build carries no output shader bytecode; "
                     "output suite skipped\n";
        return 0;
    }
    {
        auto device = open_device();
        if (!transform_usable(device)) {
            std::cout << "[skip] no shader format this build supports on this "
                         "device; output suite skipped\n";
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
