#pragma once

#include <mediaserver/render/image_decode.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <vector>

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

// Halves a frame decoded into system memory, for a clip held at half
// resolution: each output sample is the rounded mean of the 2x2 block it
// covers, plane by plane, so the chroma keeps its own subsampling relative to
// the smaller luma. An odd extent rounds up and the last row or column
// averages with itself. Integer arithmetic throughout, so every node produces
// the same bytes.
//
// Eight-bit RGBA and the two-plane NV12/P010 layouts are halved. Anything else
// - a half-float still - is returned unchanged rather than averaged as the
// wrong type; only video is held at half resolution.
[[nodiscard]] inline bool halve_decoded_frame(DecodedFrame& frame) noexcept {
    if (!frame.valid() || frame.gpu.has_value()) {
        return false;
    }
    const bool planar = planar_video(frame.layout);
    if (!planar && frame.layout != DecodedPixelLayout::Rgba8) {
        return false;
    }
    // One plane: `channels` interleaved samples of `sample_bytes` each.
    struct Plane final {
        core::u32 width;
        core::u32 height;
        core::u32 channels;
    };
    const core::u32 sample_bytes =
        planar ? video_sample_bytes(frame.layout) : 1U;
    const auto plane_bytes = [sample_bytes](const Plane& plane) {
        return static_cast<std::size_t>(plane.width) * plane.height *
            plane.channels * sample_bytes;
    };
    const auto half = [](core::u32 extent) { return (extent + 1U) / 2U; };
    std::array<Plane, 2U> planes{};
    std::size_t count = 1U;
    if (planar) {
        planes[0] = Plane{frame.width, frame.height, 1U};
        planes[1] = Plane{frame.chroma.width, frame.chroma.height, 2U};
        count = 2U;
    } else {
        planes[0] = Plane{frame.width, frame.height, 4U};
    }
    std::size_t input_bytes = 0U;
    std::size_t output_bytes = 0U;
    for (std::size_t index = 0U; index < count; ++index) {
        input_bytes += plane_bytes(planes[index]);
        output_bytes += plane_bytes(Plane{half(planes[index].width),
                                          half(planes[index].height),
                                          planes[index].channels});
    }
    if (input_bytes != frame.pixels.size()) {
        return false;
    }
    try {
        std::vector<core::u8> output(output_bytes);
        const auto read = [sample_bytes](const core::u8* at) -> core::u32 {
            return sample_bytes == 1U
                ? at[0]
                : static_cast<core::u32>(at[0]) |
                    (static_cast<core::u32>(at[1]) << 8U);
        };
        const auto write = [sample_bytes](core::u8* at, core::u32 value) {
            at[0] = static_cast<core::u8>(value & 0xffU);
            if (sample_bytes == 2U) {
                at[1] = static_cast<core::u8>(value >> 8U);
            }
        };
        const core::u8* source = frame.pixels.data();
        core::u8* destination = output.data();
        for (std::size_t index = 0U; index < count; ++index) {
            const Plane& plane = planes[index];
            const core::u32 out_width = half(plane.width);
            const core::u32 out_height = half(plane.height);
            const std::size_t texel = plane.channels * sample_bytes;
            const std::size_t row = plane.width * texel;
            for (core::u32 y = 0U; y < out_height; ++y) {
                const core::u32 top = y * 2U;
                const core::u32 bottom = std::min(top + 1U, plane.height - 1U);
                for (core::u32 x = 0U; x < out_width; ++x) {
                    const core::u32 left = x * 2U;
                    const core::u32 right =
                        std::min(left + 1U, plane.width - 1U);
                    for (core::u32 channel = 0U; channel < plane.channels;
                         ++channel) {
                        const std::size_t offset = channel * sample_bytes;
                        const core::u32 sum =
                            read(source + top * row + left * texel + offset) +
                            read(source + top * row + right * texel + offset) +
                            read(source + bottom * row + left * texel + offset) +
                            read(source + bottom * row + right * texel + offset);
                        write(destination +
                                  (static_cast<std::size_t>(y) * out_width + x) *
                                      texel +
                                  offset,
                              (sum + 2U) / 4U);
                    }
                }
            }
            source += plane_bytes(plane);
            destination +=
                plane_bytes(Plane{out_width, out_height, plane.channels});
        }
        frame.pixels = std::move(output);
        frame.width = half(frame.width);
        frame.height = half(frame.height);
        if (planar) {
            frame.chroma = ChromaExtent{half(frame.chroma.width),
                                        half(frame.chroma.height)};
        }
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace mediaserver::render::detail
