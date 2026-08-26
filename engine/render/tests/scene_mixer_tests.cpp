#include <mediaserver/render/scene_mixer.hpp>

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

#define MIX_CHECK(...)                                                        \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] rhi::Device open_device() {
    rhi::DeviceConfig config{};
    config.debug = false;
    auto created = rhi::Device::create(config);
    check(created.has_value(), "device creation", __LINE__);
    return std::move(created).value();
}

[[nodiscard]] rhi::Texture solid_texture(rhi::Device& device,
                                         std::array<core::u8, 4U> color,
                                         core::u32 width = 8U,
                                         core::u32 height = 8U) {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.sampled = true;
    auto texture = device.create_texture(desc);
    check(texture.has_value(), "texture creation", __LINE__);
    std::vector<core::u8> pixels(
        static_cast<std::size_t>(width) * height * 4U);
    for (std::size_t index = 0U; index < pixels.size(); index += 4U) {
        std::copy(color.begin(), color.end(), pixels.begin() + index);
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

[[nodiscard]] std::array<core::u8, 4U>
mix_pixel(rhi::Device& device, render::SceneMixer& mixer,
          const rhi::Texture& from, const rhi::Texture& to, float amount) {
    const core::u32 bytes = mixer.target().width() * mixer.target().height() * 4U;
    auto readback = device.create_readback(bytes);
    check(readback.has_value(), "readback", __LINE__);
    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "draw commands", __LINE__);
    auto commands = std::move(acquired).value();
    check(mixer.render(commands, from, to, amount), "mix render", __LINE__);
    check(commands.download(mixer.target(), readback.value()), "download",
          __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "draw submit", __LINE__);
    check(std::move(submitted).value().wait(), "draw wait", __LINE__);
    const auto pixels = readback.value().map();
    check(pixels.size() >= 4U, "mapped pixel", __LINE__);
    const std::array<core::u8, 4U> result{
        pixels[0U], pixels[1U], pixels[2U], pixels[3U]};
    readback.value().unmap();
    return result;
}

[[nodiscard]] bool near(core::u8 actual, int wanted) noexcept {
    const int difference = static_cast<int>(actual) - wanted;
    return difference >= -1 && difference <= 1;
}

void mix_endpoints_and_midpoint_are_exact() {
    auto device = open_device();
    auto created = render::SceneMixer::create(device, 8U, 8U);
    MIX_CHECK(created.has_value());
    auto mixer = std::move(created).value();
    const auto from = solid_texture(device, {20U, 40U, 60U, 255U});
    const auto to = solid_texture(device, {220U, 140U, 100U, 255U});

    const auto first = mix_pixel(device, mixer, from, to, 0.0F);
    MIX_CHECK(near(first[0U], 20));
    MIX_CHECK(near(first[1U], 40));
    MIX_CHECK(near(first[2U], 60));

    const auto middle = mix_pixel(device, mixer, from, to, 0.5F);
    MIX_CHECK(near(middle[0U], 120));
    MIX_CHECK(near(middle[1U], 90));
    MIX_CHECK(near(middle[2U], 80));

    const auto last = mix_pixel(device, mixer, from, to, 1.0F);
    MIX_CHECK(near(last[0U], 220));
    MIX_CHECK(near(last[1U], 140));
    MIX_CHECK(near(last[2U], 100));
}

void mix_clamps_and_resizes() {
    auto device = open_device();
    auto created = render::SceneMixer::create(device, 8U, 8U);
    MIX_CHECK(created.has_value());
    auto mixer = std::move(created).value();
    const auto from = solid_texture(device, {10U, 20U, 30U, 255U});
    const auto to = solid_texture(device, {90U, 100U, 110U, 255U});
    MIX_CHECK(near(mix_pixel(device, mixer, from, to, -4.0F)[0U], 10));
    MIX_CHECK(near(mix_pixel(device, mixer, from, to, 4.0F)[0U], 90));
    MIX_CHECK(mixer.resize(device, 16U, 12U));
    MIX_CHECK(mixer.target().width() == 16U);
    MIX_CHECK(mixer.target().height() == 12U);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 2U> cases{{
    {"mix endpoints and midpoint are exact",
     &mix_endpoints_and_midpoint_are_exact},
    {"mix clamps and resizes", &mix_clamps_and_resizes},
}};

} // namespace

int main() {
    if (!rhi::Device::available() || !render::SceneMixer::shaders_available()) {
        std::cout << "[skip] scene mixer unavailable on this machine\n";
        return 0;
    }
    {
        auto device = open_device();
        const auto probe = render::SceneMixer::create(device, 4U, 4U);
        if (!probe &&
            probe.error() == render::RenderError::ShaderFormatUnsupported) {
            std::cout << "[skip] mixer shader format unsupported\n";
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
