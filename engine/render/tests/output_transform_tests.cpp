#include <mediaserver/render/output_transform.hpp>

#include <mediaserver/rhi/device.hpp>

#include <array>
#include <cstddef>
#include <exception>
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
    core::u32 out_height) {
    auto created = render::OutputTransform::create(device, out_width, out_height);
    check(created.has_value(), "transform creation", __LINE__);
    auto transform = std::move(created).value();

    const auto bytes = out_width * out_height * 4U;
    auto readback = device.create_readback(bytes);
    check(readback.has_value(), "readback creation", __LINE__);

    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "acquire commands", __LINE__);
    auto commands = std::move(acquired).value();

    check(transform.render(commands, source), "render", __LINE__);
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

void the_output_pass_copies_the_source_through() {
    auto device = open_device();
    constexpr core::u32 width = 96U;
    constexpr core::u32 height = 64U;
    const auto reference = gradient(width, height);
    const auto source = upload_source(device, width, height, reference);

    // Same size in and out: the output is the source, sampled at texel centres,
    // so it comes back unchanged (within one code value of sampler rounding).
    const auto image = run_transform(device, source, width, height);
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

    const auto left = run_transform(device, left_source, node_width, height);
    const auto right = run_transform(device, right_source, node_width, height);

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

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 2U> cases{{
    {"the output pass copies the source through",
     &the_output_pass_copies_the_source_through},
    {"adjacent regions tile the source",
     &adjacent_regions_tile_the_source},
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
