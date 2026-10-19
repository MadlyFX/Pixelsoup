#pragma once

#include <mediaserver/render/image_decode.hpp>

#include <cstddef>
#include <span>

namespace mediaserver::render::detail {

// Worker-only packing for opaque, limited-range Rec.709 NV12. The 8-bit
// legal-range codes map exactly to 10-bit legal-range codes (16 -> 64,
// 235 -> 940, neutral chroma 128 -> 512). Reuse the existing P010 GPU path
// without a CPU YUV-to-RGB conversion. Full-range NV12 must not use this map.
[[nodiscard]] inline DecodedFrame pack_nv12_rec709_limited(
    core::u32 width, core::u32 height,
    std::span<const core::u8> luma, std::size_t luma_stride,
    std::span<const core::u8> chroma, std::size_t chroma_stride) noexcept {
    if (width == 0U || height == 0U || width > 16384U || height > 16384U ||
        width % 2U != 0U || height % 2U != 0U ||
        luma_stride < width || chroma_stride < width ||
        luma_stride > luma.size() / height ||
        chroma_stride > chroma.size() / (height / 2U)) {
        return {};
    }
    try {
        DecodedFrame out{};
        out.width = width;
        out.height = height;
        out.layout = DecodedPixelLayout::P010Rec709Limited;
        const std::size_t row_bytes = static_cast<std::size_t>(width) * 2U;
        const std::size_t luma_bytes = row_bytes * height;
        out.pixels.resize(luma_bytes + row_bytes * (height / 2U));
        const auto pack = [width, row_bytes](
            std::span<const core::u8> input, std::size_t stride,
            core::u32 rows, core::u8* output) {
            for (core::u32 row = 0U; row < rows; ++row) {
                const auto* source = input.data() + row * stride;
                auto* destination = output + row * row_bytes;
                for (core::u32 column = 0U; column < width; ++column) {
                    // P010 stores each 10-bit code in the high bits, so an
                    // 8-bit code promoted by four is simply shifted by eight.
                    destination[column * 2U] = 0U;
                    destination[column * 2U + 1U] = source[column];
                }
            }
        };
        pack(luma, luma_stride, height, out.pixels.data());
        pack(chroma, chroma_stride, height / 2U,
             out.pixels.data() + luma_bytes);
        return out;
    } catch (...) {
        return {};
    }
}

} // namespace mediaserver::render::detail
