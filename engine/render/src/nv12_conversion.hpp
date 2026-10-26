#pragma once

#include <mediaserver/render/image_decode.hpp>

#include <algorithm>
#include <cstddef>
#include <span>

namespace mediaserver::render::detail {

// Worker-only compaction for opaque Rec.709 NV12. The decoder's planes carry
// hardware row padding; the upload wants them tight, so each row is copied at
// its own width and nothing else is touched. The eight-bit codes travel to the
// GPU as they came out of the decoder, and the media shader applies the range
// and matrix - a CPU YUV-to-RGB pass per frame is what this exists to avoid.
[[nodiscard]] inline DecodedFrame pack_nv12_rec709(
    core::u32 width, core::u32 height, bool limited,
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
        out.layout = limited ? DecodedPixelLayout::Nv12Rec709Limited
                             : DecodedPixelLayout::Nv12Rec709Full;
        out.chroma = ChromaExtent{width / 2U, height / 2U};
        const auto row_bytes = static_cast<std::size_t>(width);
        const std::size_t luma_bytes = row_bytes * height;
        out.pixels.resize(luma_bytes + row_bytes * (height / 2U));
        const auto copy = [row_bytes](std::span<const core::u8> input,
                                      std::size_t stride, core::u32 rows,
                                      core::u8* output) {
            for (core::u32 row = 0U; row < rows; ++row) {
                std::copy_n(input.data() + row * stride, row_bytes,
                            output + row * row_bytes);
            }
        };
        copy(luma, luma_stride, height, out.pixels.data());
        copy(chroma, chroma_stride, height / 2U,
             out.pixels.data() + luma_bytes);
        return out;
    } catch (...) {
        return {};
    }
}

// One plane as a software decoder hands it over: its bytes and its row pitch,
// which is padded to the decoder's own alignment rather than to the width.
struct SourcePlane final {
    std::span<const core::u8> bytes;
    std::size_t stride{};
};

// Worker-only packing of a software decoder's three planes into the two the
// media shader reads: luma copied, U and V interleaved, and every sample
// shifted so its most significant bit lands at the top of its word.
//
// This is here rather than left to libswscale because libswscale has no tuned
// converter for the ten-bit semi-planar destinations: measured on one 4K
// ProRes 422 HQ frame it spends 19.5 ms where this spends 7.0. It is only
// correct when no colour conversion is wanted - the caller checks that the
// source is already Rec.709 and picks `limited` to match its range - because
// all this does is move samples.
//
// `shift` normalises any depth to the same scale. Left-shifting a code so it
// fills the top of a sixteen-bit word makes its value the same fraction of
// full scale whatever the depth was, which is exactly what the shader reads,
// so ten- and twelve-bit sources need no separate handling.
[[nodiscard]] inline DecodedFrame pack_planar_rec709(
    core::u32 width, core::u32 height, ChromaExtent chroma,
    core::u32 sample_bytes, core::u32 shift, bool limited,
    const SourcePlane& luma, const SourcePlane& blue,
    const SourcePlane& red) noexcept {
    const std::size_t luma_row =
        static_cast<std::size_t>(width) * sample_bytes;
    const std::size_t chroma_row =
        static_cast<std::size_t>(chroma.width) * sample_bytes;
    if (width == 0U || height == 0U || width > 16384U || height > 16384U ||
        chroma.width == 0U || chroma.height == 0U ||
        chroma.width > width || chroma.height > height ||
        (sample_bytes != 1U && sample_bytes != 2U) ||
        (sample_bytes == 1U && shift != 0U) || shift > 8U ||
        luma.stride < luma_row || blue.stride < chroma_row ||
        red.stride < chroma_row ||
        luma.bytes.size() < luma.stride * (height - 1U) + luma_row ||
        blue.bytes.size() < blue.stride * (chroma.height - 1U) + chroma_row ||
        red.bytes.size() < red.stride * (chroma.height - 1U) + chroma_row) {
        return {};
    }
    try {
        DecodedFrame out{};
        out.width = width;
        out.height = height;
        out.chroma = chroma;
        out.layout = sample_bytes == 1U
            ? (limited ? DecodedPixelLayout::Nv12Rec709Limited
                       : DecodedPixelLayout::Nv12Rec709Full)
            : (limited ? DecodedPixelLayout::P010Rec709Limited
                       : DecodedPixelLayout::P010Rec709Full);
        const std::size_t luma_bytes = luma_row * height;
        out.pixels.resize(luma_bytes + chroma_row * 2U * chroma.height);
        for (core::u32 row = 0U; row < height; ++row) {
            const core::u8* const source = luma.bytes.data() + row * luma.stride;
            core::u8* const destination = out.pixels.data() + row * luma_row;
            if (shift == 0U) {
                std::copy_n(source, luma_row, destination);
                continue;
            }
            for (core::u32 column = 0U; column < width; ++column) {
                const auto code = static_cast<core::u16>(
                    (static_cast<core::u16>(source[column * 2U + 1U]) << 8U)
                    | source[column * 2U]);
                const auto word = static_cast<core::u16>(code << shift);
                destination[column * 2U] = static_cast<core::u8>(word & 0xffU);
                destination[column * 2U + 1U] =
                    static_cast<core::u8>(word >> 8U);
            }
        }
        core::u8* const chroma_base = out.pixels.data() + luma_bytes;
        const std::size_t paired_row = chroma_row * 2U;
        for (core::u32 row = 0U; row < chroma.height; ++row) {
            const core::u8* const blue_row = blue.bytes.data() + row * blue.stride;
            const core::u8* const red_row = red.bytes.data() + row * red.stride;
            core::u8* const destination = chroma_base + row * paired_row;
            if (sample_bytes == 1U) {
                for (core::u32 column = 0U; column < chroma.width; ++column) {
                    destination[column * 2U] = blue_row[column];
                    destination[column * 2U + 1U] = red_row[column];
                }
                continue;
            }
            for (core::u32 column = 0U; column < chroma.width; ++column) {
                const auto blue_code = static_cast<core::u16>(
                    (static_cast<core::u16>(blue_row[column * 2U + 1U]) << 8U)
                    | blue_row[column * 2U]);
                const auto red_code = static_cast<core::u16>(
                    (static_cast<core::u16>(red_row[column * 2U + 1U]) << 8U)
                    | red_row[column * 2U]);
                const auto blue_word = static_cast<core::u16>(blue_code << shift);
                const auto red_word = static_cast<core::u16>(red_code << shift);
                destination[column * 4U] =
                    static_cast<core::u8>(blue_word & 0xffU);
                destination[column * 4U + 1U] =
                    static_cast<core::u8>(blue_word >> 8U);
                destination[column * 4U + 2U] =
                    static_cast<core::u8>(red_word & 0xffU);
                destination[column * 4U + 3U] =
                    static_cast<core::u8>(red_word >> 8U);
            }
        }
        return out;
    } catch (...) {
        return {};
    }
}

} // namespace mediaserver::render::detail
