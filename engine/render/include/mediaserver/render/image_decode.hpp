#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/rhi/device.hpp>

#include <span>
#include <vector>

// In-memory still-image decoding shared by media sources and machine-local
// derivative consumers. stb_image's implementation remains private to the
// renderer; callers exchange only bounded encoded bytes and ordinary RGBA.
namespace mediaserver::render {

// One decoded frame in CPU memory, ready to upload. An invalid frame (no
// pixels) is how the decoder reports malformed or unsupported input.
struct DecodedFrame final {
  core::u32 width{};
  core::u32 height{};
  rhi::TextureFormat format{rhi::TextureFormat::Rgba8Unorm};
  std::vector<core::u8> pixels;

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
