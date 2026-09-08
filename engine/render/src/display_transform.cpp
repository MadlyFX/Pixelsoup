#include <mediaserver/render/display_transform.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <new>

namespace mediaserver::render {
namespace {

// binary32 to binary16, written out rather than taken from an intrinsic.
//
// Two nodes bake the same table independently and F16C would make the result
// depend on what each machine implements. Values past the half range saturate
// to infinity rather than wrapping a bright texel into a dark one.
[[nodiscard]] core::u16 to_half(float value) noexcept {
    core::u32 bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    const core::u32 sign = (bits >> 16U) & 0x8000U;
    const auto exponent = static_cast<core::i32>((bits >> 23U) & 0xFFU) - 127;
    const core::u32 mantissa = bits & 0x007F'FFFFU;
    if (exponent >= 16) {
        return static_cast<core::u16>(sign | 0x7C00U);
    }
    if (exponent < -14) {
        return static_cast<core::u16>(sign);
    }
    return static_cast<core::u16>(
        sign | (static_cast<core::u32>(exponent + 15) << 10U)
        | (mantissa >> 13U));
}

// sRGB (Rec.709 primaries) to and from ACEScg-ish AP1, and the tone curve
// between them. The matrices and the curve together are the well-known
// "ACES fitted" approximation; the header says plainly that it is a fit.
constexpr float ap1_from_srgb[9U]{
    0.59719F, 0.35458F, 0.04823F,
    0.07600F, 0.90834F, 0.01566F,
    0.02840F, 0.13383F, 0.83777F,
};
constexpr float srgb_from_ap1[9U]{
     1.60475F, -0.53108F, -0.07367F,
    -0.10208F,  1.10813F, -0.00605F,
    -0.00327F, -0.07276F,  1.07602F,
};

void transform_rgb(const float matrix[9U], const float in[3U],
                   float out[3U]) noexcept {
    // Fixed order, written out. A loop would be identical here, but the
    // explicit form makes it obvious that nothing reassociates.
    out[0U] = matrix[0U] * in[0U] + matrix[1U] * in[1U] + matrix[2U] * in[2U];
    out[1U] = matrix[3U] * in[0U] + matrix[4U] * in[1U] + matrix[5U] * in[2U];
    out[2U] = matrix[6U] * in[0U] + matrix[7U] * in[1U] + matrix[8U] * in[2U];
}

[[nodiscard]] float rrt_odt_fit(float value) noexcept {
    const float a = value * (value + 0.0245786F) - 0.000090537F;
    const float b = value * (0.983729F * value + 0.4329510F) + 0.238081F;
    return b > 0.0F ? a / b : 0.0F;
}

[[nodiscard]] float encode_srgb(float value) noexcept {
    const float clamped = std::clamp(value, 0.0F, 1.0F);
    return clamped <= 0.0031308F
        ? clamped * 12.92F
        : 1.055F * std::pow(clamped, 1.0F / 2.4F) - 0.055F;
}

void apply_transform(DisplayTransform transform, const float linear[3U],
                     float out[3U]) noexcept {
    if (transform == DisplayTransform::Srgb) {
        for (core::u32 channel = 0U; channel < 3U; ++channel) {
            out[channel] = encode_srgb(linear[channel]);
        }
        return;
    }
    float ap1[3U]{};
    transform_rgb(ap1_from_srgb, linear, ap1);
    for (float& lane : ap1) {
        lane = rrt_odt_fit(lane);
    }
    float back[3U]{};
    transform_rgb(srgb_from_ap1, ap1, back);
    for (core::u32 channel = 0U; channel < 3U; ++channel) {
        out[channel] = encode_srgb(back[channel]);
    }
}

} // namespace

float display_shaper(float linear) noexcept {
    const float value = linear > 0.0F ? linear : 0.0F;
    return value / (value + display_shaper_pivot);
}

float display_shaper_inverse(float shaped) noexcept {
    const float value = std::clamp(shaped, 0.0F, 1.0F);
    // One at the top of the range is an infinite linear value. The table's
    // last grid point stands for "everything brighter", so it is pinned to a
    // large finite value rather than left as an infinity that would poison
    // every interpolation touching it.
    if (value >= 1.0F) {
        return display_shaper_pivot * 65504.0F;
    }
    return display_shaper_pivot * value / (1.0F - value);
}

DisplayLut bake_display_lut(
    DisplayTransform transform, core::u32 size, const DisplayLut* show_lut) {
    DisplayLut lut{};
    if (size < 2U || size > 128U) {
        return lut;
    }
    try {
        lut.values.assign(
            static_cast<std::size_t>(size) * size * size * 3U, 0.0F);
    } catch (const std::bad_alloc&) {
        return DisplayLut{};
    }
    lut.size = size;

    const auto steps = static_cast<float>(size - 1U);
    for (core::u32 b = 0U; b < size; ++b) {
        for (core::u32 g = 0U; g < size; ++g) {
            for (core::u32 r = 0U; r < size; ++r) {
                const float linear[3U]{
                    display_shaper_inverse(static_cast<float>(r) / steps),
                    display_shaper_inverse(static_cast<float>(g) / steps),
                    display_shaper_inverse(static_cast<float>(b) / steps),
                };
                float display[3U]{};
                apply_transform(transform, linear, display);
                if (show_lut != nullptr && show_lut->valid()) {
                    // The look sits *after* the display transform, which is
                    // where a grade belongs: it is applied to display-referred
                    // values, the same ones a colourist saw on a reference
                    // monitor.
                    float graded[3U]{};
                    sample_display_lut(*show_lut, display, graded);
                    display[0U] = graded[0U];
                    display[1U] = graded[1U];
                    display[2U] = graded[2U];
                }
                const std::size_t index =
                    ((static_cast<std::size_t>(b) * size + g) * size + r) * 3U;
                lut.values[index] = display[0U];
                lut.values[index + 1U] = display[1U];
                lut.values[index + 2U] = display[2U];
            }
        }
    }
    return lut;
}

void sample_display_lut(
    const DisplayLut& lut, const float linear[3U], float out[3U]) noexcept {
    out[0U] = 0.0F;
    out[1U] = 0.0F;
    out[2U] = 0.0F;
    if (!lut.valid()) {
        return;
    }
    const auto steps = static_cast<float>(lut.size - 1U);
    float position[3U]{};
    core::u32 base[3U]{};
    float fraction[3U]{};
    for (core::u32 axis = 0U; axis < 3U; ++axis) {
        position[axis] = std::clamp(linear[axis], 0.0F, 1.0F) * steps;
        const float floored = std::floor(position[axis]);
        base[axis] = static_cast<core::u32>(floored);
        if (base[axis] + 1U >= lut.size) {
            base[axis] = lut.size - 2U;
        }
        fraction[axis] = position[axis] - static_cast<float>(base[axis]);
    }

    const auto at = [&](core::u32 r, core::u32 g, core::u32 b,
                        core::u32 channel) noexcept {
        const std::size_t index =
            ((static_cast<std::size_t>(b) * lut.size + g) * lut.size + r) * 3U;
        return lut.values[index + channel];
    };
    // Trilinear by hand, in a fixed order. The hardware would do this too, and
    // its precision is not guaranteed identical between implementations - and
    // the two halves of a wall are graded by two different machines.
    for (core::u32 channel = 0U; channel < 3U; ++channel) {
        const float c000 = at(base[0U], base[1U], base[2U], channel);
        const float c100 = at(base[0U] + 1U, base[1U], base[2U], channel);
        const float c010 = at(base[0U], base[1U] + 1U, base[2U], channel);
        const float c110 = at(base[0U] + 1U, base[1U] + 1U, base[2U], channel);
        const float c001 = at(base[0U], base[1U], base[2U] + 1U, channel);
        const float c101 = at(base[0U] + 1U, base[1U], base[2U] + 1U, channel);
        const float c011 = at(base[0U], base[1U] + 1U, base[2U] + 1U, channel);
        const float c111 =
            at(base[0U] + 1U, base[1U] + 1U, base[2U] + 1U, channel);
        const float x00 = c000 + (c100 - c000) * fraction[0U];
        const float x10 = c010 + (c110 - c010) * fraction[0U];
        const float x01 = c001 + (c101 - c001) * fraction[0U];
        const float x11 = c011 + (c111 - c011) * fraction[0U];
        const float y0 = x00 + (x10 - x00) * fraction[1U];
        const float y1 = x01 + (x11 - x01) * fraction[1U];
        out[channel] = y0 + (y1 - y0) * fraction[2U];
    }
}

DisplayLut parse_cube_lut(std::string_view text) {
    DisplayLut lut{};
    core::u32 declared = 0U;
    std::vector<float> values;
    std::size_t cursor = 0U;
    while (cursor <= text.size()) {
        const std::size_t line_end = std::min(text.find('\n', cursor),
                                              text.size());
        std::string_view line = text.substr(cursor, line_end - cursor);
        cursor = line_end + 1U;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.remove_suffix(1U);
        }
        while (!line.empty() && line.front() == ' ') {
            line.remove_prefix(1U);
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (line.starts_with("LUT_3D_SIZE")) {
            std::string_view digits = line.substr(11U);
            while (!digits.empty() && digits.front() == ' ') {
                digits.remove_prefix(1U);
            }
            core::u32 parsed{};
            const auto result = std::from_chars(
                digits.data(), digits.data() + digits.size(), parsed);
            if (result.ec != std::errc{} || parsed < 2U || parsed > 128U) {
                return DisplayLut{};
            }
            declared = parsed;
            continue;
        }
        // Any other keyword line is metadata this seam does not need. A 1D
        // table is refused rather than half-read: it is a different transform,
        // not a smaller version of this one.
        if (line.starts_with("LUT_1D_SIZE")) {
            return DisplayLut{};
        }
        if ((line.front() >= 'A' && line.front() <= 'Z')
            || line.front() == '"') {
            continue;
        }
        float triple[3U]{};
        std::size_t offset = 0U;
        bool ok = true;
        for (float& lane : triple) {
            while (offset < line.size() && line[offset] == ' ') {
                ++offset;
            }
            const auto result = std::from_chars(
                line.data() + offset, line.data() + line.size(), lane);
            if (result.ec != std::errc{}) {
                ok = false;
                break;
            }
            offset = static_cast<std::size_t>(result.ptr - line.data());
        }
        if (!ok) {
            return DisplayLut{};
        }
        try {
            values.push_back(triple[0U]);
            values.push_back(triple[1U]);
            values.push_back(triple[2U]);
        } catch (const std::bad_alloc&) {
            return DisplayLut{};
        }
    }
    if (declared == 0U) {
        return DisplayLut{};
    }
    const std::size_t expected =
        static_cast<std::size_t>(declared) * declared * declared * 3U;
    if (values.size() != expected) {
        return DisplayLut{};
    }
    lut.size = declared;
    lut.values = std::move(values);
    return lut;
}

namespace {

// The classic 8x8 Bayer matrix: every value from 0 to 63 exactly once, laid
// out so that any 2x2, 4x4 or 8x8 block averages to the same mean. Mirrored in
// output.frag.hlsl; the GPU output test holds the two together.
constexpr core::u8 bayer_8x8[64U]{
     0U, 32U,  8U, 40U,  2U, 34U, 10U, 42U,
    48U, 16U, 56U, 24U, 50U, 18U, 58U, 26U,
    12U, 44U,  4U, 36U, 14U, 46U,  6U, 38U,
    60U, 28U, 52U, 20U, 62U, 30U, 54U, 22U,
     3U, 35U, 11U, 43U,  1U, 33U,  9U, 41U,
    51U, 19U, 59U, 27U, 49U, 17U, 57U, 25U,
    15U, 47U,  7U, 39U, 13U, 45U,  5U, 37U,
    63U, 31U, 55U, 23U, 61U, 29U, 53U, 21U,
};

} // namespace

float output_dither_threshold(core::i64 canvas_x, core::i64 canvas_y) noexcept {
    // Two's-complement masking, so a coordinate left of the canvas origin
    // wraps the same way the shader's integer `&` does rather than mirroring.
    const core::u32 x = static_cast<core::u32>(canvas_x) & 7U;
    const core::u32 y = static_cast<core::u32>(canvas_y) & 7U;
    return (static_cast<float>(bayer_8x8[y * 8U + x]) + 0.5F) / 64.0F;
}

float quantize_with_dither(float encoded, float threshold,
                           core::u32 levels) noexcept {
    if (levels == 0U) {
        return encoded;
    }
    const float scale = static_cast<float>(levels);
    const float clamped = std::clamp(encoded, 0.0F, 1.0F);
    return std::floor(clamped * scale + threshold) / scale;
}

core::u32 display_lut_atlas_width(core::u32 size) noexcept {
    return size * size;
}

core::u32 display_lut_atlas_height(core::u32 size) noexcept {
    return size;
}

std::vector<core::u8> pack_display_lut_atlas(const DisplayLut& lut) {
    std::vector<core::u8> bytes;
    if (!lut.valid()) {
        return bytes;
    }
    const core::u32 width = display_lut_atlas_width(lut.size);
    const core::u32 height = display_lut_atlas_height(lut.size);
    try {
        bytes.assign(static_cast<std::size_t>(width) * height * 8U, 0U);
    } catch (const std::bad_alloc&) {
        return {};
    }
    for (core::u32 b = 0U; b < lut.size; ++b) {
        for (core::u32 g = 0U; g < lut.size; ++g) {
            for (core::u32 r = 0U; r < lut.size; ++r) {
                // Slice `b` occupies columns `b * size` to `b * size + size`.
                const core::u32 x = b * lut.size + r;
                const core::u32 y = g;
                const std::size_t source =
                    ((static_cast<std::size_t>(b) * lut.size + g) * lut.size
                     + r) * 3U;
                const std::size_t target =
                    (static_cast<std::size_t>(y) * width + x) * 8U;
                for (core::u32 channel = 0U; channel < 3U; ++channel) {
                    const core::u16 half =
                        to_half(lut.values[source + channel]);
                    std::memcpy(bytes.data() + target + channel * 2U, &half,
                                sizeof(half));
                }
                const core::u16 one = to_half(1.0F);
                std::memcpy(bytes.data() + target + 6U, &one, sizeof(one));
            }
        }
    }
    return bytes;
}

} // namespace mediaserver::render
