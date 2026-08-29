#include <mediaserver/render/layer_keyer.hpp>

#include <mediaserver/rhi/device.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} +
                                 std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define KEY_CHECK(...)                                                        \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] int difference(core::u8 left, core::u8 right) noexcept {
    const int result = static_cast<int>(left) - static_cast<int>(right);
    return result < 0 ? -result : result;
}

[[nodiscard]] core::u8 as_byte(float value) noexcept {
    return static_cast<core::u8>(std::lround(
        std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

struct Image final {
    core::u32 width{};
    core::u32 height{};
    std::vector<core::u8> rgba;

    [[nodiscard]] std::size_t at(core::u32 x, core::u32 y) const noexcept {
        return (static_cast<std::size_t>(y) * width + x) * 4U;
    }

    [[nodiscard]] core::u8 channel(core::u32 x, core::u32 y,
                                   std::size_t component) const noexcept {
        return rgba[at(x, y) + component];
    }
};

[[nodiscard]] std::vector<core::u8> solid_pixels(
    core::u32 width, core::u32 height,
    const std::array<core::u8, 4U>& color = {}) {
    std::vector<core::u8> pixels(
        static_cast<std::size_t>(width) * height * 4U);
    for (std::size_t index = 0U; index < pixels.size(); index += 4U) {
        std::copy(color.begin(), color.end(), pixels.begin() + index);
    }
    return pixels;
}

void set_pixel(std::vector<core::u8>& pixels, core::u32 width, core::u32 x,
               core::u32 y, const std::array<core::u8, 4U>& color) {
    const std::size_t index =
        (static_cast<std::size_t>(y) * width + x) * 4U;
    KEY_CHECK(index + color.size() <= pixels.size());
    std::copy(color.begin(), color.end(), pixels.begin() + index);
}

[[nodiscard]] std::vector<rhi::Color> reference_source(
    std::span<const core::u8> pixels) {
    KEY_CHECK((pixels.size() % 4U) == 0U);
    std::vector<rhi::Color> result(pixels.size() / 4U);
    for (std::size_t index = 0U; index < result.size(); ++index) {
        const std::size_t byte = index * 4U;
        result[index] = rhi::Color{
            static_cast<float>(pixels[byte]) / 255.0F,
            static_cast<float>(pixels[byte + 1U]) / 255.0F,
            static_cast<float>(pixels[byte + 2U]) / 255.0F,
            static_cast<float>(pixels[byte + 3U]) / 255.0F};
    }
    return result;
}

void matches_reference(const Image& image,
                       std::span<const rhi::Color> expected,
                       int tolerance = 4) {
    KEY_CHECK(expected.size() ==
              static_cast<std::size_t>(image.width) * image.height);
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        const std::size_t byte = index * 4U;
        KEY_CHECK(difference(image.rgba[byte], as_byte(expected[index].r)) <=
                  tolerance);
        KEY_CHECK(difference(image.rgba[byte + 1U],
                             as_byte(expected[index].g)) <= tolerance);
        KEY_CHECK(difference(image.rgba[byte + 2U],
                             as_byte(expected[index].b)) <= tolerance);
        KEY_CHECK(difference(image.rgba[byte + 3U],
                             as_byte(expected[index].a)) <= tolerance);
    }
}

[[nodiscard]] rhi::Device open_device() {
    rhi::DeviceConfig config{};
    config.debug = false;
    auto created = rhi::Device::create(config);
    check(created.has_value(), "device creation", __LINE__);
    return std::move(created).value();
}

[[nodiscard]] rhi::Texture upload_source(rhi::Device& device,
                                         core::u32 width, core::u32 height,
                                         std::span<const core::u8> rgba) {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.sampled = true;
    auto texture = device.create_texture(desc);
    check(texture.has_value(), "source texture", __LINE__);

    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "upload command list", __LINE__);
    auto commands = std::move(acquired).value();
    check(commands.upload(texture.value(), rgba), "source upload", __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "upload submit", __LINE__);
    check(std::move(submitted).value().wait(), "upload wait", __LINE__);
    return std::move(texture).value();
}

[[nodiscard]] rhi::Texture create_depth(rhi::Device& device,
                                        core::u32 width,
                                        core::u32 height) {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Depth32Float;
    desc.depth_target = true;
    auto texture = device.create_texture(desc);
    check(texture.has_value(), "depth texture", __LINE__);
    return std::move(texture).value();
}

struct GpuResult final {
    Image image;
    const rhi::Texture* color{};
    const rhi::Texture* depth{};
};

template <typename Apply>
[[nodiscard]] GpuResult run(rhi::Device& device, Apply&& apply) {
    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "keyer command list", __LINE__);
    auto commands = std::move(acquired).value();
    auto result = apply(commands);
    check(result.has_value(), "keyer apply", __LINE__);
    const render::KeyedLayer layer = result.value();
    check(layer.color != nullptr, "keyed color", __LINE__);
    check(layer.depth != nullptr, "retained depth", __LINE__);

    const core::u32 byte_count =
        layer.color->width() * layer.color->height() * 4U;
    auto readback = device.create_readback(byte_count);
    check(readback.has_value(), "keyer readback", __LINE__);
    check(commands.download(*layer.color, readback.value()),
          "keyer download", __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "keyer submit", __LINE__);
    check(std::move(submitted).value().wait(), "keyer wait", __LINE__);

    Image image{};
    image.width = layer.color->width();
    image.height = layer.color->height();
    const auto mapped = readback.value().map();
    check(mapped.size() >= byte_count, "keyer map", __LINE__);
    image.rgba.assign(mapped.begin(), mapped.begin() + byte_count);
    readback.value().unmap();
    return GpuResult{std::move(image), layer.color, layer.depth};
}

void cpu_references_are_deterministic_and_fail_closed() {
    constexpr core::u32 width = 3U;
    constexpr core::u32 height = 2U;
    const std::vector<rhi::Color> source{
        {0.0F, 1.0F, 0.0F, 1.0F},
        {1.0F, 0.0F, 0.0F, 1.0F},
        {0.25F, 0.5F, 0.25F, 0.5F},
        {0.0F, 0.0F, 0.0F, 0.0F},
        {0.2F, 0.2F, 0.2F, 1.0F},
        {0.8F, 0.8F, 0.8F, 1.0F},
    };
    render::ChromaKeyParameters chroma{};
    chroma.tolerance = 0.04F;
    chroma.softness = 0.25F;
    chroma.morphology_px = 1;
    chroma.spill_suppression = 0.75F;
    chroma.mix = 0.6F;
    const auto first =
        render::chroma_key_reference(source, width, height, chroma);
    const auto second =
        render::chroma_key_reference(source, width, height, chroma);
    KEY_CHECK(first.has_value());
    KEY_CHECK(second.has_value());
    KEY_CHECK(first.value() == second.value());

    render::LumaKeyParameters luma{};
    luma.low_threshold = 0.25F;
    luma.high_threshold = 0.75F;
    luma.low_softness = 0.1F;
    luma.high_softness = 0.15F;
    luma.invert = true;
    luma.mix = 0.4F;
    const auto luma_first =
        render::luma_key_reference(source, width, height, luma);
    const auto luma_second =
        render::luma_key_reference(source, width, height, luma);
    KEY_CHECK(luma_first.has_value());
    KEY_CHECK(luma_second.has_value());
    KEY_CHECK(luma_first.value() == luma_second.value());

    render::ChromaKeyParameters chroma_identity{};
    chroma_identity.mix = 0.0F;
    chroma_identity.tolerance =
        std::numeric_limits<float>::quiet_NaN();
    chroma_identity.morphology_px = 1000;
    const auto chroma_copy = render::chroma_key_reference(
        source, width, height, chroma_identity);
    KEY_CHECK(chroma_copy.has_value());
    KEY_CHECK(chroma_copy.value() == source);
    KEY_CHECK(render::chroma_key_is_identity(chroma_identity));
    KEY_CHECK(!render::valid_chroma_key(chroma_identity));

    render::LumaKeyParameters luma_identity{};
    luma_identity.low_threshold = 0.9F;
    luma_identity.high_threshold = 0.1F;
    luma_identity.mix = 0.0F;
    const auto luma_copy =
        render::luma_key_reference(source, width, height, luma_identity);
    KEY_CHECK(luma_copy.has_value());
    KEY_CHECK(luma_copy.value() == source);
    KEY_CHECK(render::luma_key_is_identity(luma_identity));
    KEY_CHECK(!render::valid_luma_key(luma_identity));

    luma_identity.mix = 1.0F;
    const auto invalid_luma =
        render::luma_key_reference(source, width, height, luma_identity);
    KEY_CHECK(!invalid_luma.has_value());
    KEY_CHECK(invalid_luma.error() ==
              render::RenderError::InvalidConfiguration);

    auto invalid_source = source;
    invalid_source[0] = rhi::Color{1.0F, 0.0F, 0.0F, 0.5F};
    const auto invalid_premultiplication = render::chroma_key_reference(
        invalid_source, width, height, chroma);
    KEY_CHECK(!invalid_premultiplication.has_value());

    chroma.morphology_px = render::max_key_morphology_radius_px;
    KEY_CHECK(render::chroma_key_bounds(chroma) ==
              render::KeyBoundsExpansion{});
    chroma.morphology_px = -render::max_key_morphology_radius_px;
    KEY_CHECK(render::chroma_key_bounds(chroma) ==
              render::KeyBoundsExpansion{});
    KEY_CHECK(render::luma_key_bounds(luma) ==
              render::KeyBoundsExpansion{});
}

void gpu_identities_are_pass_pixel_and_stat_inert() {
    auto device = open_device();
    constexpr core::u32 width = 5U;
    constexpr core::u32 height = 3U;
    auto pixels = solid_pixels(width, height, {51U, 102U, 153U, 255U});
    set_pixel(pixels, width, 2U, 1U, {128U, 0U, 0U, 128U});
    const auto source = upload_source(device, width, height, pixels);
    const auto depth = create_depth(device, width, height);
    auto created = render::LayerKeyer::create(device, width, height);
    KEY_CHECK(created.has_value());
    auto keyer = std::move(created).value();

    render::ChromaKeyParameters chroma{};
    chroma.mix = 0.0F;
    chroma.tolerance = std::numeric_limits<float>::quiet_NaN();
    chroma.morphology_px = 999;
    const GpuResult chroma_identity = run(
        device, [&keyer, &source, &depth, &chroma](rhi::CommandList& commands) {
            return keyer.apply_chroma(commands, source, depth, chroma);
        });
    KEY_CHECK(chroma_identity.color == &source);
    KEY_CHECK(chroma_identity.depth == &depth);
    KEY_CHECK(chroma_identity.image.rgba == pixels);
    KEY_CHECK(keyer.stats() == render::KeyerStats{});

    render::LumaKeyParameters luma{};
    luma.low_threshold = 0.8F;
    luma.high_threshold = 0.2F;
    luma.mix = 0.0F;
    const GpuResult luma_identity = run(
        device, [&keyer, &source, &depth, &luma](rhi::CommandList& commands) {
            return keyer.apply_luma(commands, source, depth, luma);
        });
    KEY_CHECK(luma_identity.color == &source);
    KEY_CHECK(luma_identity.depth == &depth);
    KEY_CHECK(luma_identity.image.rgba == pixels);
    KEY_CHECK(keyer.stats() == render::KeyerStats{});

    luma.mix = 1.0F;
    auto acquired = device.acquire_commands();
    KEY_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();
    const auto invalid = keyer.apply_luma(commands, source, depth, luma);
    KEY_CHECK(!invalid.has_value());
    KEY_CHECK(invalid.error() == render::RenderError::InvalidConfiguration);
    KEY_CHECK(keyer.stats() == render::KeyerStats{});

    render::ChromaKeyParameters active{};
    const auto wrong_depth =
        keyer.apply_chroma(commands, source, source, active);
    KEY_CHECK(!wrong_depth.has_value());
    KEY_CHECK(wrong_depth.error() ==
              render::RenderError::InvalidConfiguration);
    KEY_CHECK(keyer.stats() == render::KeyerStats{});
}

void chroma_gpu_matches_rec709_alpha_and_spill_reference() {
    auto device = open_device();
    constexpr core::u32 width = 6U;
    constexpr core::u32 height = 1U;
    std::vector<core::u8> pixels{
        0U,   255U, 0U,   255U,
        64U,  192U, 64U,  255U,
        255U, 0U,   0U,   255U,
        128U, 0U,   0U,   128U,
        0U,   128U, 0U,   128U,
        0U,   0U,   0U,   0U,
    };
    const auto colors = reference_source(pixels);
    const auto source = upload_source(device, width, height, pixels);
    const auto depth = create_depth(device, width, height);
    auto created = render::LayerKeyer::create(device, width, height);
    KEY_CHECK(created.has_value());
    auto keyer = std::move(created).value();

    render::ChromaKeyParameters hard{};
    hard.tolerance = 0.0F;
    hard.softness = 0.0F;
    hard.mix = 1.0F;
    const auto hard_reference =
        render::chroma_key_reference(colors, width, height, hard);
    KEY_CHECK(hard_reference.has_value());
    const GpuResult hard_result = run(
        device, [&keyer, &source, &depth, &hard](rhi::CommandList& commands) {
            return keyer.apply_chroma(commands, source, depth, hard);
        });
    matches_reference(hard_result.image, hard_reference.value());
    KEY_CHECK(hard_result.color != &source);
    KEY_CHECK(hard_result.depth == &depth);
    KEY_CHECK(hard_result.image.channel(0U, 0U, 3U) == 0U);
    KEY_CHECK(hard_result.image.channel(0U, 0U, 0U) == 0U);
    KEY_CHECK(hard_result.image.channel(2U, 0U, 3U) == 255U);
    KEY_CHECK(difference(hard_result.image.channel(3U, 0U, 3U), 128U) <= 1);
    KEY_CHECK(hard_result.image.channel(3U, 0U, 0U) <=
              hard_result.image.channel(3U, 0U, 3U));
    KEY_CHECK(hard_result.image.channel(4U, 0U, 3U) == 0U);
    KEY_CHECK(keyer.stats() == render::KeyerStats{2U, 12U});

    keyer.reset_stats();
    render::ChromaKeyParameters soft{};
    soft.tolerance = 0.04F;
    soft.softness = 0.5F;
    soft.spill_suppression = 1.0F;
    soft.mix = 0.5F;
    const auto soft_reference =
        render::chroma_key_reference(colors, width, height, soft);
    KEY_CHECK(soft_reference.has_value());
    const GpuResult soft_result = run(
        device, [&keyer, &source, &depth, &soft](rhi::CommandList& commands) {
            return keyer.apply_chroma(commands, source, depth, soft);
        });
    matches_reference(soft_result.image, soft_reference.value(), 5);
    KEY_CHECK(soft_result.depth == &depth);
    KEY_CHECK(difference(soft_result.image.channel(0U, 0U, 3U), 128U) <= 2);
    KEY_CHECK(difference(soft_result.image.channel(4U, 0U, 3U), 64U) <= 2);
    KEY_CHECK(soft_result.image.channel(4U, 0U, 1U) <=
              soft_result.image.channel(4U, 0U, 3U));

    render::ChromaKeyParameters without_spill = soft;
    without_spill.spill_suppression = 0.0F;
    const auto no_spill =
        render::chroma_key_reference(colors, width, height, without_spill);
    KEY_CHECK(no_spill.has_value());
    KEY_CHECK(soft_result.image.channel(1U, 0U, 1U) <
              as_byte(no_spill.value()[1U].g));
    KEY_CHECK(keyer.stats() == render::KeyerStats{2U, 12U});
}

void luma_gpu_matches_soft_band_and_invert_reference() {
    auto device = open_device();
    constexpr core::u32 width = 5U;
    constexpr core::u32 height = 1U;
    std::vector<core::u8> pixels;
    pixels.reserve(width * 4U);
    constexpr std::array<core::u8, 5U> gray_values{
        0U, 76U, 128U, 179U, 255U};
    for (const core::u8 value : gray_values) {
        pixels.insert(pixels.end(), {value, value, value, 255U});
    }
    const auto colors = reference_source(pixels);
    const auto source = upload_source(device, width, height, pixels);
    const auto depth = create_depth(device, width, height);
    auto created = render::LayerKeyer::create(device, width, height);
    KEY_CHECK(created.has_value());
    auto keyer = std::move(created).value();

    render::LumaKeyParameters parameters{};
    parameters.low_threshold = 0.4F;
    parameters.high_threshold = 0.6F;
    parameters.low_softness = 0.2F;
    parameters.high_softness = 0.2F;
    parameters.mix = 1.0F;
    const auto expected =
        render::luma_key_reference(colors, width, height, parameters);
    KEY_CHECK(expected.has_value());
    const GpuResult keyed = run(
        device,
        [&keyer, &source, &depth, &parameters](rhi::CommandList& commands) {
            return keyer.apply_luma(commands, source, depth, parameters);
        });
    matches_reference(keyed.image, expected.value(), 4);
    KEY_CHECK(keyed.depth == &depth);
    KEY_CHECK(keyed.image.channel(0U, 0U, 3U) == 0U);
    KEY_CHECK(keyed.image.channel(2U, 0U, 3U) >= 250U);
    KEY_CHECK(keyed.image.channel(4U, 0U, 3U) == 0U);
    KEY_CHECK(keyed.image.channel(1U, 0U, 0U) <=
              keyed.image.channel(1U, 0U, 3U));
    KEY_CHECK(keyer.stats() == render::KeyerStats{2U, 10U});

    keyer.reset_stats();
    parameters.invert = true;
    const auto inverted_expected =
        render::luma_key_reference(colors, width, height, parameters);
    KEY_CHECK(inverted_expected.has_value());
    const GpuResult inverted = run(
        device,
        [&keyer, &source, &depth, &parameters](rhi::CommandList& commands) {
            return keyer.apply_luma(commands, source, depth, parameters);
        });
    matches_reference(inverted.image, inverted_expected.value(), 4);
    KEY_CHECK(inverted.depth == &depth);
    KEY_CHECK(inverted.image.channel(0U, 0U, 3U) == 255U);
    KEY_CHECK(inverted.image.channel(2U, 0U, 3U) <= 5U);
    KEY_CHECK(inverted.image.channel(4U, 0U, 3U) == 255U);
    KEY_CHECK(keyer.stats() == render::KeyerStats{2U, 10U});
}

void morphology_is_square_and_cannot_expand_source_coverage() {
    auto device = open_device();
    constexpr core::u32 width = 9U;
    constexpr core::u32 height = 9U;
    auto pixels = solid_pixels(width, height);
    for (core::u32 y = 1U; y <= 7U; ++y) {
        for (core::u32 x = 1U; x <= 7U; ++x) {
            set_pixel(pixels, width, x, y, {0U, 255U, 0U, 255U});
        }
    }
    for (core::u32 y = 3U; y <= 5U; ++y) {
        for (core::u32 x = 3U; x <= 5U; ++x) {
            set_pixel(pixels, width, x, y, {255U, 0U, 0U, 255U});
        }
    }
    const auto colors = reference_source(pixels);
    const auto source = upload_source(device, width, height, pixels);
    const auto depth = create_depth(device, width, height);
    auto created = render::LayerKeyer::create(device, width, height);
    KEY_CHECK(created.has_value());
    auto keyer = std::move(created).value();

    render::ChromaKeyParameters parameters{};
    parameters.tolerance = 0.0F;
    parameters.softness = 0.0F;
    parameters.morphology_px = 3;
    parameters.mix = 1.0F;
    const auto dilated_expected =
        render::chroma_key_reference(colors, width, height, parameters);
    KEY_CHECK(dilated_expected.has_value());
    const GpuResult dilated = run(
        device,
        [&keyer, &source, &depth, &parameters](rhi::CommandList& commands) {
            return keyer.apply_chroma(commands, source, depth, parameters);
        });
    matches_reference(dilated.image, dilated_expected.value());
    KEY_CHECK(dilated.depth == &depth);
    KEY_CHECK(dilated.image.channel(1U, 1U, 3U) == 255U);
    KEY_CHECK(dilated.image.channel(0U, 0U, 3U) == 0U);
    KEY_CHECK(dilated.image.channel(0U, 0U, 0U) == 0U);
    KEY_CHECK(dilated.image.channel(8U, 8U, 3U) == 0U);
    KEY_CHECK(keyer.stats() == render::KeyerStats{4U, 324U});
    KEY_CHECK(render::chroma_key_bounds(parameters) ==
              render::KeyBoundsExpansion{});

    keyer.reset_stats();
    parameters.morphology_px = -1;
    const auto eroded_expected =
        render::chroma_key_reference(colors, width, height, parameters);
    KEY_CHECK(eroded_expected.has_value());
    const GpuResult eroded = run(
        device,
        [&keyer, &source, &depth, &parameters](rhi::CommandList& commands) {
            return keyer.apply_chroma(commands, source, depth, parameters);
        });
    matches_reference(eroded.image, eroded_expected.value());
    KEY_CHECK(eroded.depth == &depth);
    KEY_CHECK(eroded.image.channel(4U, 4U, 3U) == 255U);
    KEY_CHECK(eroded.image.channel(3U, 3U, 3U) == 0U);
    KEY_CHECK(eroded.image.channel(5U, 5U, 3U) == 0U);
    KEY_CHECK(keyer.stats() == render::KeyerStats{4U, 324U});
}

void resize_is_transactional() {
    auto device = open_device();
    auto created = render::LayerKeyer::create(device, 7U, 5U);
    KEY_CHECK(created.has_value());
    auto keyer = std::move(created).value();
    KEY_CHECK(!keyer.resize(device, 0U, 9U));
    KEY_CHECK(keyer.width() == 7U && keyer.height() == 5U);
    KEY_CHECK(keyer.resize(device, 17U, 9U));
    KEY_CHECK(keyer.width() == 17U && keyer.height() == 9U);
    KEY_CHECK(keyer.resize(device, 17U, 9U));
    KEY_CHECK(keyer.stats() == render::KeyerStats{});
}

struct Case final {
    const char* name;
    void (*function)();
    bool requires_gpu{};
};

constexpr std::array<Case, 6U> cases{{
    {"CPU references are deterministic and fail closed",
     &cpu_references_are_deterministic_and_fail_closed, false},
    {"GPU identities are pass, pixel, and stat inert",
     &gpu_identities_are_pass_pixel_and_stat_inert, true},
    {"Chroma GPU matches Rec.709 alpha and spill reference",
     &chroma_gpu_matches_rec709_alpha_and_spill_reference, true},
    {"Luma GPU matches soft band and invert reference",
     &luma_gpu_matches_soft_band_and_invert_reference, true},
    {"Morphology is square and cannot expand source coverage",
     &morphology_is_square_and_cannot_expand_source_coverage, true},
    {"Resize is transactional", &resize_is_transactional, true},
}};

[[nodiscard]] bool gpu_keyer_available() {
    if (!rhi::Device::available() || !render::LayerKeyer::shaders_available()) {
        return false;
    }
    auto device = open_device();
    const auto probe = render::LayerKeyer::create(device, 4U, 4U);
    if (!probe &&
        probe.error() == render::RenderError::ShaderFormatUnsupported) {
        return false;
    }
    KEY_CHECK(probe.has_value());
    return true;
}

} // namespace

int main() {
    bool gpu_available = false;
    try {
        gpu_available = gpu_keyer_available();
    } catch (const std::exception& error) {
        std::cout << "[skip] layer keyer GPU unavailable: " << error.what()
                  << '\n';
    }

    std::size_t failures = 0U;
    std::size_t skipped = 0U;
    for (const Case& test : cases) {
        if (test.requires_gpu && !gpu_available) {
            ++skipped;
            std::cout << "[skip] " << test.name << '\n';
            continue;
        }
        try {
            test.function();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << cases.size() - failures - skipped << '/' << cases.size()
              << " tests passed";
    if (skipped != 0U) {
        std::cout << " (" << skipped << " GPU tests skipped)";
    }
    std::cout << '\n';
    return failures == 0U ? 0 : 1;
}
