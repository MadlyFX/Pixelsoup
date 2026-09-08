#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/rhi/device.hpp>

#include <optional>
#include <span>
#include <string_view>
#include <vector>

// In-memory still-image decoding shared by media sources and machine-local
// derivative consumers. stb_image's implementation remains private to the
// renderer; callers exchange only bounded encoded bytes and ordinary RGBA.
namespace mediaserver::render {

enum class DecodedPixelLayout : core::u8 {
  Rgba8,
  // Rec.709 P010: tightly packed 16-bit luma followed by a half-height plane
  // of interleaved 16-bit U/V samples. Range remains explicit because applying
  // limited-range offsets to a full-range plate visibly crushes the picture.
  P010Rec709Limited,
  P010Rec709Full,
  // Linear scene-referred half-float RGBA. Radiance, not display pixels: values
  // above one are the point of it, and the alpha channel is always one.
  Rgba16FloatLinear,
};

// One decoded frame in CPU memory, ready to upload. An invalid frame (no
// pixels) is how the decoder reports malformed or unsupported input.
struct DecodedFrame final {
  core::u32 width{};
  core::u32 height{};
  rhi::TextureFormat format{rhi::TextureFormat::Rgba8Unorm};
  DecodedPixelLayout layout{DecodedPixelLayout::Rgba8};
  std::vector<core::u8> pixels;
  // Video workers may satisfy a moving playhead with the most recently
  // completed frame while the exact request is still in flight. Stills leave
  // this empty; video decoders report the actual source ordinal so the GPU
  // cache never labels late pixels as the requested future frame.
  std::optional<core::u32> source_frame;

  [[nodiscard]] bool valid() const noexcept {
    return width > 0U && height > 0U && !pixels.empty();
  }
};

// Decodes one complete encoded image from memory as RGBA8. Empty, oversized,
// malformed, truncated, or unsupported input returns an invalid frame. The
// function performs no filesystem access and retains no pointer into `bytes`.
[[nodiscard]] DecodedFrame
decode_image_bytes(std::span<const core::u8> bytes) noexcept;

// Decodes one complete high-dynamic-range image from memory as RGBA16F.
//
// Separate from `decode_image_bytes` rather than a format argument on it,
// because the two answer different questions. An environment map is radiance:
// values above one are the whole point, and a decoder that returned it as RGBA8
// would clip the highlights that make a reflection look like anything. The
// low-dynamic-range path stays exactly as it was for every caller that wants
// display pixels.
//
// Radiance `.hdr` today. That covers most of what a stage ships and costs
// nothing - it is already in the vendored decoder. OpenEXR is what a facility
// with a colour pipeline actually delivers and is a real dependency; it is not
// here yet, and an `.exr` returns an invalid frame rather than something
// plausible.
//
// Alpha is set to one: a radiance map has no transparency, and leaving whatever
// the source had there would multiply into the lighting.
[[nodiscard]] DecodedFrame
decode_radiance_bytes(std::span<const core::u8> bytes) noexcept;

// Whether this extension names a format `decode_radiance_bytes` can read.
// Case-insensitive, and it inspects the name only - it opens nothing.
[[nodiscard]] bool radiance_source(std::string_view source) noexcept;

} // namespace mediaserver::render
