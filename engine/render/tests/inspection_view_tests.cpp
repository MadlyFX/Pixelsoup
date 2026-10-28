#include <mediaserver/render/inspection_view.hpp>

#include <mediaserver/rhi/device.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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

#define INSPECT_CHECK(...)                                                    \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] rhi::Device open_device() {
    rhi::DeviceConfig config{};
    config.debug = false;
    auto created = rhi::Device::create(config);
    check(created.has_value(), "device creation", __LINE__);
    return std::move(created).value();
}

// Two columns: the left half `left`, the right half `right`.
[[nodiscard]] rhi::Texture two_tone(rhi::Device& device,
                                    std::array<core::u8, 4U> left,
                                    std::array<core::u8, 4U> right) {
    constexpr core::u32 width = 8U;
    constexpr core::u32 height = 4U;
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.sampled = true;
    auto texture = device.create_texture(desc);
    check(texture.has_value(), "texture creation", __LINE__);
    std::vector<core::u8> pixels(static_cast<std::size_t>(width) * height * 4U);
    for (core::u32 y = 0U; y < height; ++y) {
        for (core::u32 x = 0U; x < width; ++x) {
            const auto& colour = x < width / 2U ? left : right;
            std::copy(colour.begin(), colour.end(),
                      pixels.begin() + (static_cast<std::size_t>(y) * width + x) * 4U);
        }
    }
    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "upload commands", __LINE__);
    auto commands = std::move(acquired).value();
    check(commands.upload(texture.value(), pixels), "texture upload", __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "upload submit", __LINE__);
    check(std::move(submitted).value().wait(), "upload wait", __LINE__);
    return std::move(texture).value();
}

// The inspected image's pixel at column `x` of row zero.
[[nodiscard]] std::array<core::u8, 4U>
inspect(rhi::Device& device, render::InspectionView& view,
        const rhi::Texture& image, render::InspectionChannel channel,
        float stops, core::u32 x) {
    const core::u32 bytes = view.target().width() * view.target().height() * 4U;
    auto readback = device.create_readback(bytes);
    check(readback.has_value(), "readback", __LINE__);
    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "draw commands", __LINE__);
    auto commands = std::move(acquired).value();
    check(view.render(commands, image, channel, stops), "inspection render",
          __LINE__);
    check(commands.download(view.target(), readback.value()), "download",
          __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "draw submit", __LINE__);
    check(std::move(submitted).value().wait(), "draw wait", __LINE__);
    const auto pixels = readback.value().map();
    const std::size_t offset = static_cast<std::size_t>(x) * 4U;
    check(pixels.size() >= offset + 4U, "mapped pixel", __LINE__);
    const std::array<core::u8, 4U> result{pixels[offset], pixels[offset + 1U],
                                          pixels[offset + 2U],
                                          pixels[offset + 3U]};
    readback.value().unmap();
    return result;
}

[[nodiscard]] bool near(core::u8 actual, int wanted) noexcept {
    const int difference = static_cast<int>(actual) - wanted;
    return difference >= -1 && difference <= 1;
}

[[nodiscard]] int encode(double linear) noexcept {
    const double value =
        linear <= 0.0031308 ? linear * 12.92
                            : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    return static_cast<int>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}

[[nodiscard]] double decode(int code) noexcept {
    const double value = static_cast<double>(code) / 255.0;
    return value <= 0.04045 ? value / 12.92
                            : std::pow((value + 0.055) / 1.055, 2.4);
}

void rgb_passes_each_pixel_through_unchanged() {
    auto device = open_device();
    auto created = render::InspectionView::create(device, 8U, 4U);
    INSPECT_CHECK(created.has_value());
    auto view = std::move(created).value();
    const auto image = two_tone(device, {30U, 120U, 210U, 255U},
                                {200U, 10U, 90U, 255U});
    // Same size and a nearest read: the sharp edge between the halves stays
    // exactly where it was.
    const auto left = inspect(device, view, image,
                              render::InspectionChannel::Rgb, 0.0F, 3U);
    const auto right = inspect(device, view, image,
                               render::InspectionChannel::Rgb, 0.0F, 4U);
    INSPECT_CHECK(left[0U] == 30U && left[1U] == 120U && left[2U] == 210U);
    INSPECT_CHECK(right[0U] == 200U && right[1U] == 10U && right[2U] == 90U);
    INSPECT_CHECK(left[3U] == 255U);
}

void a_channel_and_luma_show_as_grey() {
    auto device = open_device();
    auto created = render::InspectionView::create(device, 8U, 4U);
    INSPECT_CHECK(created.has_value());
    auto view = std::move(created).value();
    const auto image = two_tone(device, {40U, 150U, 230U, 255U},
                                {40U, 150U, 230U, 255U});
    const auto red = inspect(device, view, image,
                             render::InspectionChannel::Red, 0.0F, 0U);
    const auto green = inspect(device, view, image,
                               render::InspectionChannel::Green, 0.0F, 0U);
    const auto blue = inspect(device, view, image,
                              render::InspectionChannel::Blue, 0.0F, 0U);
    INSPECT_CHECK(red[0U] == 40U && red[1U] == 40U && red[2U] == 40U);
    INSPECT_CHECK(green[0U] == 150U && green[1U] == 150U && green[2U] == 150U);
    INSPECT_CHECK(blue[0U] == 230U && blue[1U] == 230U && blue[2U] == 230U);
    const auto luma = inspect(device, view, image,
                              render::InspectionChannel::Luma, 0.0F, 0U);
    // Rec. 709 weights over the code values.
    const int wanted = static_cast<int>(
        std::lround(0.2126 * 40.0 + 0.7152 * 150.0 + 0.0722 * 230.0));
    INSPECT_CHECK(near(luma[0U], wanted));
    INSPECT_CHECK(luma[0U] == luma[1U] && luma[1U] == luma[2U]);
}

void exposure_scales_light_not_code_values() {
    auto device = open_device();
    auto created = render::InspectionView::create(device, 8U, 4U);
    INSPECT_CHECK(created.has_value());
    auto view = std::move(created).value();
    const auto image = two_tone(device, {60U, 100U, 180U, 255U},
                                {60U, 100U, 180U, 255U});
    const auto up = inspect(device, view, image,
                            render::InspectionChannel::Rgb, 1.0F, 0U);
    INSPECT_CHECK(near(up[0U], encode(decode(60) * 2.0)));
    INSPECT_CHECK(near(up[1U], encode(decode(100) * 2.0)));
    INSPECT_CHECK(near(up[2U], encode(decode(180) * 2.0)));
    const auto down = inspect(device, view, image,
                              render::InspectionChannel::Rgb, -2.0F, 0U);
    INSPECT_CHECK(near(down[0U], encode(decode(60) * 0.25)));
    INSPECT_CHECK(near(down[2U], encode(decode(180) * 0.25)));
    // Clamped, and a non-finite offset is no offset.
    INSPECT_CHECK(near(inspect(device, view, image,
                               render::InspectionChannel::Rgb, 99.0F, 0U)[0U],
                       255));
    INSPECT_CHECK(inspect(device, view, image, render::InspectionChannel::Rgb,
                          std::nanf(""), 0U)[0U] == 60U);
    INSPECT_CHECK(view.resize(device, 16U, 12U));
    INSPECT_CHECK(view.target().width() == 16U &&
                  view.target().height() == 12U);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 3U> cases{{
    {"RGB passes each pixel through unchanged",
     &rgb_passes_each_pixel_through_unchanged},
    {"a channel and luma show as grey", &a_channel_and_luma_show_as_grey},
    {"exposure scales light, not code values",
     &exposure_scales_light_not_code_values},
}};

} // namespace

int main() {
    if (!rhi::Device::available() ||
        !render::InspectionView::shaders_available()) {
        std::cout << "[skip] inspection view unavailable on this machine\n";
        return 0;
    }
    {
        auto device = open_device();
        const auto probe = render::InspectionView::create(device, 4U, 4U);
        if (!probe &&
            probe.error() == render::RenderError::ShaderFormatUnsupported) {
            std::cout << "[skip] inspection shader format unsupported\n";
            return 0;
        }
    }
    int failures = 0;
    for (const Case& test : cases) {
        try {
            test.function();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
