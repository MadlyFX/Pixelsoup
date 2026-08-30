#include <mediaserver/editor/viewport_panel.hpp>
#include <mediaserver/rhi/device.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

namespace editor = mediaserver::editor;
namespace core = mediaserver::core;
namespace gui = mediaserver::gui;
namespace rhi = mediaserver::rhi;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define SAMPLE_CHECK(...)                                                     \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] bool near(float left, float right) noexcept {
  return std::abs(left - right) < 0.0001F;
}

void a_viewport_sample_reads_the_letterboxed_texture_pixel() {
  auto created_device = rhi::Device::create(rhi::DeviceConfig{});
  SAMPLE_CHECK(created_device);
  rhi::Device device = std::move(created_device).value();

  rhi::TextureDesc desc{};
  desc.width = 4U;
  desc.height = 2U;
  desc.format = rhi::TextureFormat::Rgba8Unorm;
  desc.color_target = true;
  desc.sampled = true;
  auto created_texture = device.create_texture(desc);
  SAMPLE_CHECK(created_texture);
  rhi::Texture texture = std::move(created_texture).value();

  std::array<core::u8, 4U * 2U * 4U> pixels{};
  for (std::size_t index = 0U; index < pixels.size(); index += 4U) {
    pixels[index + 3U] = 255U;
  }
  // Pixel (3, 0) has a deliberately non-primary colour so channel order is
  // proved as well as coordinate mapping.
  pixels[12U] = 17U;
  pixels[13U] = 34U;
  pixels[14U] = 51U;

  auto acquired = device.acquire_commands();
  SAMPLE_CHECK(acquired);
  auto commands = std::move(acquired).value();
  SAMPLE_CHECK(commands.upload(texture, pixels));
  auto submitted = device.submit(std::move(commands));
  SAMPLE_CHECK(submitted);
  SAMPLE_CHECK(std::move(submitted).value().wait());

  constexpr gui::Rect image{100.0F, 50.0F, 400.0F, 200.0F};
  const auto sampled = editor::sample_viewport_color(
      device, texture, image, 450.0F, 75.0F);
  SAMPLE_CHECK(sampled.has_value());
  SAMPLE_CHECK(near(sampled->r, 17.0F / 255.0F));
  SAMPLE_CHECK(near(sampled->g, 34.0F / 255.0F));
  SAMPLE_CHECK(near(sampled->b, 51.0F / 255.0F));
  SAMPLE_CHECK(!editor::sample_viewport_color(
                    device, texture, image, 99.0F, 75.0F)
                    .has_value());
}

} // namespace

int main() {
  try {
    a_viewport_sample_reads_the_letterboxed_texture_pixel();
    std::cout << "[pass] viewport eyedropper samples a rendered pixel\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "[fail] viewport eyedropper samples a rendered pixel: "
              << error.what() << '\n';
    return 1;
  }
}
