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

enum class ImagePrimaries : core::u8 { Rec709, AcesCg };

enum class DecodedPixelLayout : core::u8 {
  Rgba8,
  // Rec.709 P010: tightly packed 16-bit luma followed by a half-height plane
  // of interleaved 16-bit U/V samples. Range remains explicit because applying
  // limited-range offsets to a full-range plate visibly crushes the picture.
  P010Rec709Limited,
  P010Rec709Full,
  // Linear scene-referred half-float RGBA, retaining negative and HDR values.
  // Media uses straight alpha; environment decode forces opaque alpha.
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
  ImagePrimaries primaries{ImagePrimaries::Rec709};

  [[nodiscard]] bool valid() const noexcept {
    return width > 0U && height > 0U && !pixels.empty();
  }
};

// Decodes ordinary images as RGBA8 and EXR as scene-linear RGBA16F.
// EXR media is converted to Rec.709 and unpremultiplied for the clip shader. Empty, oversized,
// malformed, truncated, or unsupported input returns an invalid frame. The
// function performs no filesystem access and retains no pointer into `bytes`.
[[nodiscard]] DecodedFrame
decode_image_bytes(std::span<const core::u8> bytes) noexcept;

// Bounded, single-part scanline/tiled RGB(A), HALF or FLOAT. HALF channels
// retain their bits, including subnormals and alpha; FLOAT rounds to nearest
// even. ACEScg chromaticities or an explicit ACEScg tag are retained here.
// Unknown/conflicting colour tags, deep/multipart and subsampling are refused.
[[nodiscard]] DecodedFrame decode_exr_bytes(
    std::span<const core::u8> bytes) noexcept;
[[nodiscard]] bool exr_bytes(std::span<const core::u8> bytes) noexcept;
// Image/environment workers call this before upload into the Rec.709 renderer.
[[nodiscard]] bool convert_image_to_working_space(DecodedFrame& frame) noexcept;
void unpremultiply_exr_image(DecodedFrame& frame) noexcept;

// Decodes one complete high-dynamic-range image from memory as RGBA16F.
//
// Separate from `decode_image_bytes` rather than a format argument on it,
// because the two answer different questions. An environment map is radiance:
// values above one are the whole point, and a decoder that returned it as RGBA8
// would clip the highlights that make a reflection look like anything. The
// low-dynamic-range path stays exactly as it was for every caller that wants
// display pixels.
//
// Radiance .hdr and single-part RGB(A) OpenEXR HALF/FLOAT. Tagged ACEScg
// is converted to the renderer's linear Rec.709 on this worker path.
//
// Alpha is set to one: a radiance map has no transparency, and leaving whatever
// the source had there would multiply into the lighting.
[[nodiscard]] DecodedFrame
decode_radiance_bytes(std::span<const core::u8> bytes) noexcept;

// Whether this extension names a format `decode_radiance_bytes` can read.
// Case-insensitive, and it inspects the name only - it opens nothing.
[[nodiscard]] bool radiance_source(std::string_view source) noexcept;

} // namespace mediaserver::render
