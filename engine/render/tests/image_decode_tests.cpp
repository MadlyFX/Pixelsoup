#include <mediaserver/render/image_decode.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

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

#define IMAGE_CHECK(...)                                                       \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// Two RGBA pixels encoded as a standards-compliant PNG: opaque red followed
// by half-alpha green. Keeping the bytes embedded makes this test independent
// of a writer implementation and proves the public API consumes PNG bytes.
constexpr std::array<core::u8, 72U> rgba_png{{
    0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU, 0x00U, 0x00U, 0x00U,
    0x0dU, 0x49U, 0x48U, 0x44U, 0x52U, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U,
    0x00U, 0x01U, 0x08U, 0x06U, 0x00U, 0x00U, 0x00U, 0xf4U, 0x22U, 0x7fU, 0x8aU,
    0x00U, 0x00U, 0x00U, 0x0fU, 0x49U, 0x44U, 0x41U, 0x54U, 0x78U, 0xdaU, 0x63U,
    0xf8U, 0xcfU, 0xc0U, 0xf0U, 0x1fU, 0x08U, 0x1bU, 0x00U, 0x10U, 0x79U, 0x03U,
    0x7eU, 0xc4U, 0x18U, 0x72U, 0x90U, 0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x45U,
    0x4eU, 0x44U, 0xaeU, 0x42U, 0x60U, 0x82U,
}};

void png_bytes_decode_to_exact_rgba() {
  const render::DecodedFrame frame = render::decode_image_bytes(rgba_png);

  IMAGE_CHECK(frame.valid());
  IMAGE_CHECK(frame.width == 2U);
  IMAGE_CHECK(frame.height == 1U);
  IMAGE_CHECK(frame.format == rhi::TextureFormat::Rgba8Unorm);
  constexpr std::array<core::u8, 8U> expected{{
      255U,
      0U,
      0U,
      255U,
      0U,
      255U,
      0U,
      128U,
  }};
  IMAGE_CHECK(std::ranges::equal(frame.pixels, expected));
}

void malformed_and_truncated_bytes_are_invalid() {
  IMAGE_CHECK(!render::decode_image_bytes({}).valid());

  constexpr std::array<core::u8, 8U> noise{{
      0x89U,
      0x50U,
      0x4eU,
      0x47U,
      0x00U,
      0x01U,
      0x02U,
      0x03U,
  }};
  IMAGE_CHECK(!render::decode_image_bytes(noise).valid());

  const std::span<const core::u8> truncated{rgba_png.data(), 41U};
  const render::DecodedFrame frame = render::decode_image_bytes(truncated);
  IMAGE_CHECK(!frame.valid());
  IMAGE_CHECK(frame.width == 0U);
  IMAGE_CHECK(frame.height == 0U);
  IMAGE_CHECK(frame.pixels.empty());
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 2U> cases{{
    {"PNG bytes decode to exact RGBA", &png_bytes_decode_to_exact_rgba},
    {"malformed and truncated bytes are invalid",
     &malformed_and_truncated_bytes_are_invalid},
}};

} // namespace

int main() {
  std::size_t failures = 0U;
  for (const Case &entry : cases) {
    try {
      entry.function();
      std::cout << "[pass] " << entry.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
    }
  }
  std::cout << (cases.size() - failures) << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
