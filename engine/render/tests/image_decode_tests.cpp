#include <mediaserver/render/image_decode.hpp>
#include "../src/nv12_conversion.hpp"

#include <algorithm>
#include <array>
#include <cstring>
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

// Three RGBE pixels as an uncompressed Radiance image: (1, 0.5, 0.25),
// (4, 2, 1), and one whose exponent puts it far past what a half can hold.
//
// Embedded rather than written by an encoder, for the same reason the PNG above
// is: this proves the public API consumes real Radiance bytes, not that two
// halves of this repository agree with each other.
constexpr std::array<core::u8, 57U> radiance_hdr{{
    0x23U, 0x3fU, 0x52U, 0x41U, 0x44U, 0x49U, 0x41U, 0x4eU, 0x43U, 0x45U, 0x0aU,
    0x46U, 0x4fU, 0x52U, 0x4dU, 0x41U, 0x54U, 0x3dU, 0x33U, 0x32U, 0x2dU, 0x62U,
    0x69U, 0x74U, 0x5fU, 0x72U, 0x6cU, 0x65U, 0x5fU, 0x72U, 0x67U, 0x62U, 0x65U,
    0x0aU, 0x0aU, 0x2dU, 0x59U, 0x20U, 0x31U, 0x20U, 0x2bU, 0x58U, 0x20U, 0x33U,
    0x0aU, 0x80U, 0x40U, 0x20U, 0x81U, 0x80U, 0x40U, 0x20U, 0x83U, 0xffU, 0xffU,
    0xffU, 0xffU,
}};

[[nodiscard]] core::u16 half_at(const render::DecodedFrame &frame,
                                std::size_t index) {
  core::u16 value{};
  std::memcpy(&value, frame.pixels.data() + (index * sizeof(core::u16)),
              sizeof(value));
  return value;
}

// Radiance decodes to exact half-float bits.
//
// Bits, not a tolerance. Two nodes convert the same map independently, and a
// conversion that rounded differently on one of them would light the scene
// differently there - so the rounding is asserted rather than the magnitude.
void radiance_bytes_decode_to_exact_half_floats() {
  const render::DecodedFrame frame =
      render::decode_radiance_bytes(radiance_hdr);

  IMAGE_CHECK(frame.valid());
  IMAGE_CHECK(frame.width == 3U);
  IMAGE_CHECK(frame.height == 1U);
  IMAGE_CHECK(frame.format == rhi::TextureFormat::Rgba16Float);
  IMAGE_CHECK(frame.layout == render::DecodedPixelLayout::Rgba16FloatLinear);
  IMAGE_CHECK(frame.pixels.size() == 3U * 4U * sizeof(core::u16));

  // 1.0, 0.5, 0.25, and an alpha the decoder sets rather than carries.
  IMAGE_CHECK(half_at(frame, 0U) == 0x3C00U);
  IMAGE_CHECK(half_at(frame, 1U) == 0x3800U);
  IMAGE_CHECK(half_at(frame, 2U) == 0x3400U);
  IMAGE_CHECK(half_at(frame, 3U) == 0x3C00U);

  // Above one, which is the entire reason this path exists. An RGBA8 decode
  // would have clipped both of these to white.
  IMAGE_CHECK(half_at(frame, 4U) == 0x4400U);
  IMAGE_CHECK(half_at(frame, 5U) == 0x4000U);
  IMAGE_CHECK(half_at(frame, 6U) == 0x3C00U);

  // Past the half-float range: saturates to infinity rather than wrapping to
  // something small, which would be a bright texel silently becoming a dark
  // one.
  IMAGE_CHECK(half_at(frame, 8U) == 0x7C00U);
  IMAGE_CHECK(half_at(frame, 9U) == 0x7C00U);
  IMAGE_CHECK(half_at(frame, 10U) == 0x7C00U);
  IMAGE_CHECK(half_at(frame, 11U) == 0x3C00U);
}

// A low-dynamic-range image is refused rather than promoted.
//
// stb will happily hand back floats for a PNG, and the result would look like
// it worked: an environment map with no value above one, lighting the scene
// with nothing. That is far harder to diagnose than a refusal.
void a_low_dynamic_range_image_is_not_a_radiance_map() {
  IMAGE_CHECK(!render::decode_radiance_bytes(rgba_png).valid());
  IMAGE_CHECK(!render::decode_radiance_bytes({}).valid());

  std::array<core::u8, 64U> noise{};
  for (std::size_t index = 0U; index < noise.size(); ++index) {
    noise[index] = static_cast<core::u8>(index * 7U);
  }
  IMAGE_CHECK(!render::decode_radiance_bytes(noise).valid());

  // Truncated mid-pixel: an invalid frame, never a partial one.
  const std::span<const core::u8> truncated{radiance_hdr.data(),
                                            radiance_hdr.size() - 6U};
  const render::DecodedFrame partial =
      render::decode_radiance_bytes(truncated);
  IMAGE_CHECK(!partial.valid() ||
              partial.pixels.size() == static_cast<std::size_t>(partial.width) *
                                           partial.height * 4U *
                                           sizeof(core::u16));

  // And the ordinary decoder still reads the ordinary image, unchanged.
  IMAGE_CHECK(render::decode_image_bytes(rgba_png).valid());
}

// The extension test names what can be read, and says nothing about what is on
// disk - it opens nothing.
void radiance_sources_are_recognised_by_extension() {
  IMAGE_CHECK(render::radiance_source("studio.hdr"));
  IMAGE_CHECK(render::radiance_source("STUDIO.HDR"));
  IMAGE_CHECK(render::radiance_source("a/b/c.Hdr"));
  IMAGE_CHECK(render::radiance_source("studio.exr"));
  IMAGE_CHECK(render::radiance_source("STUDIO.EXR"));
  IMAGE_CHECK(!render::radiance_source("studio.png"));
  IMAGE_CHECK(!render::radiance_source("hdr"));
  IMAGE_CHECK(!render::radiance_source(""));
}

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

void nv12_packing_preserves_codes_and_ignores_row_padding() {
  // Distinct legal/extreme luma and non-neutral interleaved chroma samples;
  // padding must never enter either tightly packed output plane.
  constexpr std::array<core::u8, 12U> luma{{
      0U, 16U, 128U, 235U, 99U, 98U,
      255U, 32U, 64U, 192U, 97U, 96U}};
  constexpr std::array<core::u8, 6U> chroma{{
      16U, 240U, 128U, 102U, 95U, 94U}};
  const auto frame = render::detail::pack_nv12_rec709_limited(
      4U, 2U, luma, 6U, chroma, 6U);
  IMAGE_CHECK(frame.valid());
  IMAGE_CHECK(frame.layout == render::DecodedPixelLayout::P010Rec709Limited);
  IMAGE_CHECK(frame.width == 4U && frame.height == 2U);
  constexpr std::array<core::u16, 12U> expected{{
      0x0000U, 0x1000U, 0x8000U, 0xeb00U,
      0xff00U, 0x2000U, 0x4000U, 0xc000U,
      0x1000U, 0xf000U, 0x8000U, 0x6600U}};
  IMAGE_CHECK(frame.pixels.size() == expected.size() * sizeof(core::u16));
  for (std::size_t index = 0U; index < expected.size(); ++index) {
    IMAGE_CHECK(half_at(frame, index) == expected[index]);
  }
  IMAGE_CHECK(!render::detail::pack_nv12_rec709_limited(
      4U, 2U, luma, 3U, chroma, 6U).valid());
  IMAGE_CHECK(!render::detail::pack_nv12_rec709_limited(
      4U, 2U, std::span{luma}.first(11U), 6U, chroma, 6U).valid());
  IMAGE_CHECK(!render::detail::pack_nv12_rec709_limited(
      4U, 2U, luma, 6U, std::span{chroma}.first(5U), 6U).valid());
  IMAGE_CHECK(!render::detail::pack_nv12_rec709_limited(
      3U, 2U, luma, 6U, chroma, 6U).valid());
  IMAGE_CHECK(!render::detail::pack_nv12_rec709_limited(
      4U, 0U, luma, 6U, chroma, 6U).valid());
}

constexpr std::array<Case, 6U> cases{{
    {"NV12 packing preserves codes and ignores row padding",
     &nv12_packing_preserves_codes_and_ignores_row_padding},
    {"radiance bytes decode to exact half floats",
     &radiance_bytes_decode_to_exact_half_floats},
    {"a low dynamic range image is not a radiance map",
     &a_low_dynamic_range_image_is_not_a_radiance_map},
    {"radiance sources are recognised by extension",
     &radiance_sources_are_recognised_by_extension},
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
