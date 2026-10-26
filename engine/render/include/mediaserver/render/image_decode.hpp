#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/rhi/device.hpp>

#include <cstddef>
#include <memory>
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
  // Rec.709 NV12, the same plane arrangement as P010 at eight bits: tightly
  // packed luma followed by a plane of interleaved U/V. Eight-bit sources
  // keep their own depth rather than being widened on the CPU, which halves
  // both the conversion work and the bytes uploaded per frame.
  Nv12Rec709Limited,
  Nv12Rec709Full,
};

// How much of the picture a chroma plane covers. The two-plane layouts above
// name a sample depth, not a subsampling: 4:2:0, 4:2:2 and 4:4:4 all travel
// as luma plus one interleaved chroma plane, and only its extent differs.
// Keeping that extent on the frame is what lets a 4:2:2 source reach the GPU
// without its chroma being thrown away to fit a 4:2:0 shape.
struct ChromaExtent final {
  core::u32 width{};
  core::u32 height{};

  bool operator==(const ChromaExtent&) const = default;
};

// Planar video, as opposed to an interleaved colour image. These layouts are
// uploaded as two textures and resolved to RGB in the media shader; the shader
// reads the same ordinal, so the order of this enum is part of that contract.
[[nodiscard]] constexpr bool planar_video(DecodedPixelLayout layout) noexcept {
  return layout == DecodedPixelLayout::P010Rec709Limited
      || layout == DecodedPixelLayout::P010Rec709Full
      || layout == DecodedPixelLayout::Nv12Rec709Limited
      || layout == DecodedPixelLayout::Nv12Rec709Full;
}

// Bytes per luma sample. Chroma interleaves two of them per texel.
[[nodiscard]] constexpr core::u32 video_sample_bytes(
    DecodedPixelLayout layout) noexcept {
  return layout == DecodedPixelLayout::Nv12Rec709Limited
          || layout == DecodedPixelLayout::Nv12Rec709Full
      ? 1U
      : 2U;
}

// A frame a hardware decoder left in GPU memory, on the renderer's own device.
// Nothing is read back and nothing is uploaded: the cache asks that device for
// plane views of `resource` and samples the decoder's surface where it is.
//
// `retained` holds the decoder's reference to that surface. The decoder cannot
// reuse the surface while it lives, so the cache keeps the whole DecodedFrame
// until its plane views are safe to release.
struct DecodedGpuFrame final {
  rhi::NativeHandle device{};
  rhi::NativeHandle resource{};
  std::shared_ptr<void> retained;
  // What this frame costs the decoded-residency budget. The budget bounds
  // video memory on this path rather than system memory, so it is the size
  // the planes occupy rather than anything held in `pixels`.
  std::size_t bytes{};
};

// One decoded frame, in CPU memory or on the GPU. An invalid frame (no pixels
// and no GPU surface) is how the decoder reports malformed or unsupported
// input.
struct DecodedFrame final {
  core::u32 width{};
  core::u32 height{};
  rhi::TextureFormat format{rhi::TextureFormat::Rgba8Unorm};
  DecodedPixelLayout layout{DecodedPixelLayout::Rgba8};
  std::vector<core::u8> pixels;
  // Set on every planar frame; meaningless and zero on the others.
  ChromaExtent chroma{};
  std::optional<DecodedGpuFrame> gpu;
  // Video workers may satisfy a moving playhead with the most recently
  // completed frame while the exact request is still in flight. Stills leave
  // this empty; video decoders report the actual source ordinal so the GPU
  // cache never labels late pixels as the requested future frame.
  std::optional<core::u32> source_frame;
  ImagePrimaries primaries{ImagePrimaries::Rec709};

  [[nodiscard]] bool valid() const noexcept {
    return width > 0U && height > 0U && (!pixels.empty() || gpu.has_value());
  }

  // What holding this frame costs the decoded-residency budget, wherever the
  // picture actually lives.
  [[nodiscard]] std::size_t residency_bytes() const noexcept {
    return gpu.has_value() ? gpu->bytes : pixels.size();
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
