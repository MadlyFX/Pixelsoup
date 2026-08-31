#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/rhi/device.hpp>

#include <optional>
#include <span>
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

} // namespace mediaserver::render
