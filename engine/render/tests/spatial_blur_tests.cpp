#include <mediaserver/render/spatial_blur.hpp>

#include <mediaserver/rhi/device.hpp>

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

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define SPATIAL_CHECK(...)                                                     \
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

[[nodiscard]] rhi::Texture upload_source(rhi::Device &device, core::u32 width,
                                         core::u32 height,
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

template <typename Apply>
[[nodiscard]] Image run(rhi::Device &device, const rhi::Texture &source,
                        Apply &&apply,
                        const rhi::Texture **returned = nullptr) {
  auto acquired = device.acquire_commands();
  check(acquired.has_value(), "filter command list", __LINE__);
  auto commands = std::move(acquired).value();
  auto result = apply(commands, source);
  check(result.has_value(), "filter apply", __LINE__);
  const rhi::Texture *const output = result.value();
  check(output != nullptr, "filter output", __LINE__);
  if (returned != nullptr) {
    *returned = output;
  }

  const core::u32 byte_count = output->width() * output->height() * 4U;
  auto readback = device.create_readback(byte_count);
  check(readback.has_value(), "filter readback", __LINE__);
  check(commands.download(*output, readback.value()), "filter download",
        __LINE__);
  auto submitted = device.submit(std::move(commands));
  check(submitted.has_value(), "filter submit", __LINE__);
  check(std::move(submitted).value().wait(), "filter wait", __LINE__);

  Image image{};
  image.width = output->width();
  image.height = output->height();
  const auto pixels = readback.value().map();
  check(pixels.size() >= byte_count, "filter map", __LINE__);
  image.rgba.assign(pixels.begin(), pixels.begin() + byte_count);
  readback.value().unmap();
  return image;
}

[[nodiscard]] std::vector<core::u8> impulse(core::u32 width, core::u32 height) {
  std::vector<core::u8> pixels(static_cast<std::size_t>(width) * height * 4U,
                               0U);
  const std::size_t centre =
      (static_cast<std::size_t>(height / 2U) * width + width / 2U) * 4U;
  pixels[centre + 0U] = 255U;
  pixels[centre + 1U] = 255U;
  pixels[centre + 2U] = 255U;
  pixels[centre + 3U] = 255U;
  return pixels;
}

[[nodiscard]] std::vector<core::u8> gradient(core::u32 width,
                                             core::u32 height) {
  std::vector<core::u8> pixels(static_cast<std::size_t>(width) * height * 4U);
  for (core::u32 y = 0U; y < height; ++y) {
    for (core::u32 x = 0U; x < width; ++x) {
      const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4U;
      pixels[index + 0U] = static_cast<core::u8>(x * 7U + y * 3U);
      pixels[index + 1U] = static_cast<core::u8>(x * 2U + y * 9U);
      pixels[index + 2U] = static_cast<core::u8>(x * 5U + y * 4U);
      pixels[index + 3U] = 255U;
    }
  }
  return pixels;
}

[[nodiscard]] int difference(core::u8 left, core::u8 right) noexcept {
  const int result = static_cast<int>(left) - static_cast<int>(right);
  return result < 0 ? -result : result;
}

void box_is_a_flat_separable_kernel() {
  auto device = open_device();
  constexpr core::u32 width = 21U;
  constexpr core::u32 height = 19U;
  constexpr core::u32 cx = width / 2U;
  constexpr core::u32 cy = height / 2U;
  const auto pixels = impulse(width, height);
  const auto source = upload_source(device, width, height, pixels);
  auto created = render::SpatialBlur::create(device, width, height);
  SPATIAL_CHECK(created.has_value());
  auto filter = std::move(created).value();

  const Image image = run(
      device, source,
      [&filter](rhi::CommandList &commands, const rhi::Texture &input) {
        return filter.apply_box(commands, input, render::BoxBlurKernel{1U, 1U});
      });
  const core::u8 expected = image.rgba[image.at(cx, cy)];
  SPATIAL_CHECK(expected >= 27U && expected <= 29U);
  for (core::u32 y = cy - 1U; y <= cy + 1U; ++y) {
    for (core::u32 x = cx - 1U; x <= cx + 1U; ++x) {
      SPATIAL_CHECK(difference(image.rgba[image.at(x, y)], expected) <= 1);
    }
  }
  SPATIAL_CHECK(image.rgba[image.at(cx - 2U, cy)] == 0U);
  SPATIAL_CHECK(image.rgba[image.at(cx, cy + 2U)] == 0U);
}

void box_pass_count_repeats_the_pair() {
  auto device = open_device();
  constexpr core::u32 width = 21U;
  constexpr core::u32 height = 21U;
  constexpr core::u32 cx = width / 2U;
  constexpr core::u32 cy = height / 2U;
  const auto pixels = impulse(width, height);
  const auto source = upload_source(device, width, height, pixels);
  auto created = render::SpatialBlur::create(device, width, height);
  SPATIAL_CHECK(created.has_value());
  auto filter = std::move(created).value();

  const Image image = run(
      device, source,
      [&filter](rhi::CommandList &commands, const rhi::Texture &input) {
        return filter.apply_box(commands, input, render::BoxBlurKernel{1U, 2U});
      });
  SPATIAL_CHECK(image.rgba[image.at(cx + 2U, cy)] > 0U);
  SPATIAL_CHECK(image.rgba[image.at(cx, cy - 2U)] > 0U);
  SPATIAL_CHECK(image.rgba[image.at(cx + 3U, cy)] == 0U);
}

void directional_samples_only_the_authored_line() {
  auto device = open_device();
  constexpr core::u32 width = 25U;
  constexpr core::u32 height = 25U;
  constexpr core::u32 cx = width / 2U;
  constexpr core::u32 cy = height / 2U;
  const auto pixels = impulse(width, height);
  const auto source = upload_source(device, width, height, pixels);
  auto created = render::SpatialBlur::create(device, width, height);
  SPATIAL_CHECK(created.has_value());
  auto filter = std::move(created).value();

  const Image horizontal =
      run(device, source,
          [&filter](rhi::CommandList &commands, const rhi::Texture &input) {
            return filter.apply_directional(
                commands, input, render::DirectionalBlurKernel{4.0F, 0.0F, 5U});
          });
  for (core::u32 x = cx - 2U; x <= cx + 2U; ++x) {
    SPATIAL_CHECK(horizontal.rgba[horizontal.at(x, cy)] >= 50U);
    SPATIAL_CHECK(horizontal.rgba[horizontal.at(x, cy)] <= 52U);
  }
  SPATIAL_CHECK(horizontal.rgba[horizontal.at(cx, cy - 1U)] == 0U);
  SPATIAL_CHECK(horizontal.rgba[horizontal.at(cx, cy + 1U)] == 0U);

  const Image vertical = run(
      device, source,
      [&filter](rhi::CommandList &commands, const rhi::Texture &input) {
        return filter.apply_directional(
            commands, input, render::DirectionalBlurKernel{4.0F, 0.25F, 5U});
      });
  for (core::u32 y = cy - 2U; y <= cy + 2U; ++y) {
    SPATIAL_CHECK(vertical.rgba[vertical.at(cx, y)] >= 50U);
    SPATIAL_CHECK(vertical.rgba[vertical.at(cx, y)] <= 52U);
  }
  SPATIAL_CHECK(vertical.rgba[vertical.at(cx - 1U, cy)] == 0U);
  SPATIAL_CHECK(vertical.rgba[vertical.at(cx + 1U, cy)] == 0U);
}

void alpha_is_filtered_and_expands_with_the_isolated_color() {
  auto device = open_device();
  constexpr core::u32 width = 21U;
  constexpr core::u32 height = 21U;
  constexpr core::u32 cx = width / 2U;
  constexpr core::u32 cy = height / 2U;
  std::vector<core::u8> pixels(static_cast<std::size_t>(width) * height * 4U,
                               0U);
  pixels[(static_cast<std::size_t>(cy) * width + cx) * 4U + 3U] = 255U;
  const auto source = upload_source(device, width, height, pixels);
  auto created = render::SpatialBlur::create(device, width, height);
  SPATIAL_CHECK(created.has_value());
  auto filter = std::move(created).value();

  const Image boxed = run(
      device, source,
      [&filter](rhi::CommandList &commands, const rhi::Texture &input) {
        return filter.apply_box(commands, input, render::BoxBlurKernel{1U, 1U});
      });
  SPATIAL_CHECK(boxed.rgba[boxed.at(cx + 1U, cy) + 3U] >= 27U);
  SPATIAL_CHECK(boxed.rgba[boxed.at(cx + 1U, cy) + 3U] <= 29U);
  SPATIAL_CHECK(boxed.rgba[boxed.at(cx + 1U, cy)] == 0U);
  SPATIAL_CHECK(boxed.rgba[boxed.at(cx, cy + 1U) + 3U] >= 27U);

  const Image directed =
      run(device, source,
          [&filter](rhi::CommandList &commands, const rhi::Texture &input) {
            return filter.apply_directional(
                commands, input, render::DirectionalBlurKernel{4.0F, 0.0F, 5U});
          });
  SPATIAL_CHECK(directed.rgba[directed.at(cx + 2U, cy) + 3U] >= 50U);
  SPATIAL_CHECK(directed.rgba[directed.at(cx + 2U, cy) + 3U] <= 52U);
  SPATIAL_CHECK(directed.rgba[directed.at(cx + 2U, cy)] == 0U);
  SPATIAL_CHECK(directed.rgba[directed.at(cx, cy + 1U) + 3U] == 0U);
}

void identities_return_the_exact_source_and_active_limits_fail_closed() {
  auto device = open_device();
  constexpr core::u32 width = 13U;
  constexpr core::u32 height = 11U;
  const auto reference = gradient(width, height);
  const auto source = upload_source(device, width, height, reference);
  auto created = render::SpatialBlur::create(device, width, height);
  SPATIAL_CHECK(created.has_value());
  auto filter = std::move(created).value();

  const rhi::Texture *box_returned{};
  const Image box_identity = run(
      device, source,
      [&filter](rhi::CommandList &commands, const rhi::Texture &input) {
        return filter.apply_box(commands, input, render::BoxBlurKernel{0U, 0U});
      },
      &box_returned);
  SPATIAL_CHECK(box_returned == &source);
  SPATIAL_CHECK(box_identity.rgba == reference);

  const rhi::Texture *directional_returned{};
  const Image directional_identity = run(
      device, source,
      [&filter](rhi::CommandList &commands, const rhi::Texture &input) {
        return filter.apply_directional(
            commands, input, render::DirectionalBlurKernel{0.0F, 0.75F, 3U});
      },
      &directional_returned);
  SPATIAL_CHECK(directional_returned == &source);
  SPATIAL_CHECK(directional_identity.rgba == reference);

  auto acquired = device.acquire_commands();
  SPATIAL_CHECK(acquired.has_value());
  auto commands = std::move(acquired).value();
  const auto invalid_box =
      filter.apply_box(commands, source, render::BoxBlurKernel{1U, 0U});
  SPATIAL_CHECK(!invalid_box.has_value());
  SPATIAL_CHECK(invalid_box.error() ==
                render::RenderError::InvalidConfiguration);
  const auto invalid_directional = filter.apply_directional(
      commands, source, render::DirectionalBlurKernel{2.0F, 0.0F, 4U});
  SPATIAL_CHECK(!invalid_directional.has_value());
  SPATIAL_CHECK(invalid_directional.error() ==
                render::RenderError::InvalidConfiguration);
  const auto negative_directional = filter.apply_directional(
      commands, source, render::DirectionalBlurKernel{-1.0F, 0.0F, 3U});
  SPATIAL_CHECK(!negative_directional.has_value());
  SPATIAL_CHECK(negative_directional.error() ==
                render::RenderError::InvalidConfiguration);
}

void resize_is_transactional() {
  auto device = open_device();
  auto created = render::SpatialBlur::create(device, 7U, 5U);
  SPATIAL_CHECK(created.has_value());
  auto filter = std::move(created).value();
  SPATIAL_CHECK(!filter.resize(device, 0U, 9U));
  SPATIAL_CHECK(filter.width() == 7U && filter.height() == 5U);
  SPATIAL_CHECK(filter.resize(device, 17U, 9U));
  SPATIAL_CHECK(filter.width() == 17U && filter.height() == 9U);
  SPATIAL_CHECK(filter.resize(device, 17U, 9U));
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 6U> cases{{
    {"box is a flat separable kernel", &box_is_a_flat_separable_kernel},
    {"box pass count repeats the pair", &box_pass_count_repeats_the_pair},
    {"directional samples only the authored line",
     &directional_samples_only_the_authored_line},
    {"alpha is filtered and expands with the isolated color",
     &alpha_is_filtered_and_expands_with_the_isolated_color},
    {"identities return the exact source and active limits fail closed",
     &identities_return_the_exact_source_and_active_limits_fail_closed},
    {"resize is transactional", &resize_is_transactional},
}};

} // namespace

int main() {
  if (!rhi::Device::available() || !render::SpatialBlur::shaders_available()) {
    std::cout << "[skip] spatial blur unavailable on this machine\n";
    return 0;
  }
  {
    auto device = open_device();
    const auto probe = render::SpatialBlur::create(device, 4U, 4U);
    if (!probe &&
        probe.error() == render::RenderError::ShaderFormatUnsupported) {
      std::cout << "[skip] spatial blur shader format unsupported\n";
      return 0;
    }
  }

  std::size_t failures = 0U;
  for (const Case &test : cases) {
    try {
      test.function();
      std::cout << "[pass] " << test.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << test.name << ": " << error.what() << '\n';
    }
  }
  std::cout << cases.size() - failures << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
