#include <mediaserver/render/gaussian_blur.hpp>

#include <mediaserver/rhi/device.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
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
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define BLUR_CHECK(...)                                                       \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] rhi::Device open_device() {
    rhi::DeviceConfig config{};
    config.debug = false;
    auto created = rhi::Device::create(config);
    check(created.has_value(), "device creation", __LINE__);
    return std::move(created).value();
}

struct Image final {
    core::u32 width{};
    core::u32 height{};
    std::vector<core::u8> rgba;

    [[nodiscard]] std::size_t at(core::u32 x, core::u32 y) const noexcept {
        return (static_cast<std::size_t>(y) * width + x) * 4U;
    }
};

[[nodiscard]] rhi::Texture upload_source(rhi::Device& device,
                                         core::u32 width, core::u32 height,
                                         const std::vector<core::u8>& rgba) {
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

[[nodiscard]] Image run_blur(rhi::Device& device,
                             render::GaussianBlur& blur,
                             const rhi::Texture& source, float radius,
                             const rhi::Texture** returned = nullptr) {
    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "blur command list", __LINE__);
    auto commands = std::move(acquired).value();
    auto applied = blur.apply(commands, source, radius);
    check(applied.has_value(), "blur apply", __LINE__);
    const rhi::Texture* const output = applied.value();
    check(output != nullptr, "blur output", __LINE__);
    if (returned != nullptr) {
        *returned = output;
    }

    const core::u32 byte_count = output->width() * output->height() * 4U;
    auto readback = device.create_readback(byte_count);
    check(readback.has_value(), "blur readback", __LINE__);
    check(commands.download(*output, readback.value()), "blur download",
          __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "blur submit", __LINE__);
    check(std::move(submitted).value().wait(), "blur wait", __LINE__);

    Image image{};
    image.width = output->width();
    image.height = output->height();
    const auto pixels = readback.value().map();
    check(pixels.size() >= byte_count, "blur map", __LINE__);
    image.rgba.assign(pixels.begin(), pixels.begin() + byte_count);
    readback.value().unmap();
    return image;
}

[[nodiscard]] std::vector<core::u8> gradient(core::u32 width,
                                             core::u32 height) {
    std::vector<core::u8> pixels(
        static_cast<std::size_t>(width) * height * 4U);
    for (core::u32 y = 0U; y < height; ++y) {
        for (core::u32 x = 0U; x < width; ++x) {
            const std::size_t index =
                (static_cast<std::size_t>(y) * width + x) * 4U;
            pixels[index + 0U] = static_cast<core::u8>(x * 17U + y * 3U);
            pixels[index + 1U] = static_cast<core::u8>(x * 5U + y * 19U);
            pixels[index + 2U] = static_cast<core::u8>(x * 11U + y * 7U);
            pixels[index + 3U] = 255U;
        }
    }
    return pixels;
}

[[nodiscard]] int difference(core::u8 left, core::u8 right) noexcept {
    const int signed_left = static_cast<int>(left);
    const int signed_right = static_cast<int>(right);
    return signed_left > signed_right ? signed_left - signed_right
                                     : signed_right - signed_left;
}

void an_impulse_spreads_in_both_axes() {
    auto device = open_device();
    constexpr core::u32 width = 33U;
    constexpr core::u32 height = 25U;
    constexpr core::u32 centre_x = width / 2U;
    constexpr core::u32 centre_y = height / 2U;
    std::vector<core::u8> pixels(
        static_cast<std::size_t>(width) * height * 4U, 0U);
    const std::size_t impulse =
        (static_cast<std::size_t>(centre_y) * width + centre_x) * 4U;
    pixels[impulse + 0U] = 255U;
    pixels[impulse + 1U] = 255U;
    pixels[impulse + 2U] = 255U;
    pixels[impulse + 3U] = 255U;
    const auto source = upload_source(device, width, height, pixels);
    auto created = render::GaussianBlur::create(device, width, height);
    BLUR_CHECK(created.has_value());
    auto blur = std::move(created).value();

    const Image image = run_blur(device, blur, source, 4.0F);
    const core::u8 centre = image.rgba[image.at(centre_x, centre_y)];
    const core::u8 left = image.rgba[image.at(centre_x - 1U, centre_y)];
    const core::u8 right = image.rgba[image.at(centre_x + 1U, centre_y)];
    const core::u8 up = image.rgba[image.at(centre_x, centre_y - 1U)];
    const core::u8 down = image.rgba[image.at(centre_x, centre_y + 1U)];
    BLUR_CHECK(centre > 0U && centre < 255U);
    BLUR_CHECK(left > 0U && right > 0U);
    BLUR_CHECK(up > 0U && down > 0U);
    BLUR_CHECK(difference(left, right) <= 1);
    BLUR_CHECK(difference(up, down) <= 1);
    BLUR_CHECK(image.rgba[image.at(0U, 0U)] == 0U);

    core::u32 energy = 0U;
    for (std::size_t index = 0U; index < image.rgba.size(); index += 4U) {
        energy += image.rgba[index];
    }
    BLUR_CHECK(energy >= 220U && energy <= 290U);
}

void zero_radius_returns_the_exact_source() {
    auto device = open_device();
    constexpr core::u32 width = 9U;
    constexpr core::u32 height = 7U;
    const auto reference = gradient(width, height);
    const auto source = upload_source(device, width, height, reference);
    auto created = render::GaussianBlur::create(device, width, height);
    BLUR_CHECK(created.has_value());
    auto blur = std::move(created).value();

    const rhi::Texture* returned{};
    const Image image = run_blur(device, blur, source, 0.0F, &returned);
    BLUR_CHECK(returned == &source);
    BLUR_CHECK(image.rgba == reference);
}

void resize_replaces_both_pass_targets() {
    auto device = open_device();
    auto created = render::GaussianBlur::create(device, 7U, 5U);
    BLUR_CHECK(created.has_value());
    auto blur = std::move(created).value();
    BLUR_CHECK(!blur.resize(device, 0U, 8U));
    BLUR_CHECK(blur.width() == 7U && blur.height() == 5U);
    BLUR_CHECK(blur.resize(device, 19U, 11U));
    BLUR_CHECK(blur.width() == 19U && blur.height() == 11U);
    BLUR_CHECK(blur.resize(device, 19U, 11U));

    std::vector<core::u8> pixels(19U * 11U * 4U, 0U);
    for (std::size_t index = 0U; index < pixels.size(); index += 4U) {
        pixels[index + 0U] = 70U;
        pixels[index + 1U] = 110U;
        pixels[index + 2U] = 150U;
        pixels[index + 3U] = 200U;
    }
    const auto source = upload_source(device, 19U, 11U, pixels);
    const rhi::Texture* returned{};
    const Image image = run_blur(device, blur, source, 3.0F, &returned);
    BLUR_CHECK(returned != &source);
    BLUR_CHECK(returned->width() == 19U && returned->height() == 11U);
    for (const std::pair<core::u32, core::u32> point :
         std::array<std::pair<core::u32, core::u32>, 3U>{
             {{0U, 0U}, {9U, 5U}, {18U, 10U}}}) {
        const std::size_t index = image.at(point.first, point.second);
        BLUR_CHECK(difference(image.rgba[index + 0U], 70U) <= 1);
        BLUR_CHECK(difference(image.rgba[index + 1U], 110U) <= 1);
        BLUR_CHECK(difference(image.rgba[index + 2U], 150U) <= 1);
        BLUR_CHECK(difference(image.rgba[index + 3U], 200U) <= 1);
    }

    const auto old_size = upload_source(device, 7U, 5U, gradient(7U, 5U));
    auto acquired = device.acquire_commands();
    BLUR_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();
    const auto mismatch = blur.apply(commands, old_size, 2.0F);
    BLUR_CHECK(!mismatch.has_value());
    BLUR_CHECK(mismatch.error() == render::RenderError::InvalidConfiguration);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 3U> cases{{
    {"an impulse spreads in both axes", &an_impulse_spreads_in_both_axes},
    {"zero radius returns the exact source",
     &zero_radius_returns_the_exact_source},
    {"resize replaces both pass targets", &resize_replaces_both_pass_targets},
}};

} // namespace

int main() {
    if (!rhi::Device::available() || !render::GaussianBlur::shaders_available()) {
        std::cout << "[skip] Gaussian blur unavailable on this machine\n";
        return 0;
    }
    {
        auto device = open_device();
        const auto probe = render::GaussianBlur::create(device, 4U, 4U);
        if (!probe &&
            probe.error() == render::RenderError::ShaderFormatUnsupported) {
            std::cout << "[skip] blur shader format unsupported\n";
            return 0;
        }
    }

    std::size_t failures = 0U;
    for (const Case& test : cases) {
        try {
            test.function();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << cases.size() - failures << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
