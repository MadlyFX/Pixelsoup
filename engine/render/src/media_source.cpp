#include <mediaserver/render/media_source.hpp>

// This translation unit is the one place the stb_image implementation is
// compiled, so no other file may define STB_IMAGE_IMPLEMENTATION. The vendored
// header is not clean under this project's warnings-as-errors, so its warnings
// are silenced across the include; the code below stays fully checked.
#define STB_IMAGE_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

namespace mediaserver::render {
namespace {

struct StbiImageDeleter final {
    void operator()(stbi_uc* pixels) const noexcept {
        stbi_image_free(pixels);
    }
};

using StbiImage = std::unique_ptr<stbi_uc, StbiImageDeleter>;

[[nodiscard]] DecodedFrame
decoded_frame(stbi_uc* pixels, int width, int height) noexcept {
    StbiImage owned{pixels};
    if (!owned || width <= 0 || height <= 0) {
        return DecodedFrame{};
    }
    const std::size_t held_width = static_cast<std::size_t>(width);
    const std::size_t held_height = static_cast<std::size_t>(height);
    if (held_width >
        std::numeric_limits<std::size_t>::max() / held_height / 4U) {
        return DecodedFrame{};
    }
    const std::size_t byte_count = held_width * held_height * 4U;
    DecodedFrame frame{};
    frame.width = static_cast<core::u32>(width);
    frame.height = static_cast<core::u32>(height);
    frame.format = rhi::TextureFormat::Rgba8Unorm;
    if (byte_count > frame.pixels.max_size()) {
        return DecodedFrame{};
    }
    try {
        frame.pixels.assign(owned.get(), owned.get() + byte_count);
    } catch (const std::bad_alloc&) {
        return DecodedFrame{};
    }
    return frame;
}

} // namespace

MediaDecodeScheduling MediaSource::scheduling(
    const std::string&) const noexcept {
    return MediaDecodeScheduling::BackgroundRequired;
}

DecodedFrame decode_image_bytes(
    std::span<const core::u8> bytes) noexcept {
    if (bytes.empty() ||
        bytes.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return DecodedFrame{};
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* const pixels = stbi_load_from_memory(
        bytes.data(), static_cast<int>(bytes.size()), &width, &height,
        &channels, 4);
    return decoded_frame(pixels, width, height);
}

namespace {

// IEEE binary32 to binary16, round-to-nearest-even.
//
// Written out rather than taken from a library or an intrinsic because it has
// to produce the same bits on every machine in a cohort. F16C would be faster
// and would also make the result depend on what each machine implements, and
// two nodes whose environment maps differ in the last bit light the scene
// differently.
//
// Values past the half-float range saturate to infinity rather than wrapping,
// and a NaN stays a NaN. A map with an infinite texel is a broken map, but it
// is broken identically on every node.
[[nodiscard]] core::u16 half_from_float(float value) noexcept {
    core::u32 bits{};
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));

    const core::u32 sign = (bits >> 16U) & 0x8000U;
    const core::u32 exponent = (bits >> 23U) & 0xFFU;
    core::u32 mantissa = bits & 0x007F'FFFFU;

    if (exponent == 0xFFU) {
        // Infinity, or a NaN kept as a NaN rather than collapsing to infinity.
        return static_cast<core::u16>(
            sign | 0x7C00U | (mantissa != 0U ? 0x0200U : 0U));
    }

    // Rebias from 127 to 15.
    const auto biased = static_cast<std::int32_t>(exponent) - 127 + 15;
    if (biased >= 0x1F) {
        return static_cast<core::u16>(sign | 0x7C00U);
    }
    if (biased <= 0) {
        if (biased < -10) {
            // Below the smallest subnormal half; flushes to signed zero.
            return static_cast<core::u16>(sign);
        }
        // Subnormal: restore the implicit leading one and shift it down.
        mantissa |= 0x0080'0000U;
        const auto shift = static_cast<core::u32>(14 - biased);
        const core::u32 shifted = mantissa >> shift;
        const core::u32 remainder = mantissa & ((1U << shift) - 1U);
        const core::u32 halfway = 1U << (shift - 1U);
        core::u32 rounded = shifted;
        if (remainder > halfway || (remainder == halfway && (shifted & 1U))) {
            ++rounded;
        }
        return static_cast<core::u16>(sign | rounded);
    }

    core::u32 result =
        (static_cast<core::u32>(biased) << 10U) | (mantissa >> 13U);
    const core::u32 remainder = mantissa & 0x1FFFU;
    // Round half to even, so the conversion carries no bias and is
    // reproducible everywhere.
    if (remainder > 0x1000U || (remainder == 0x1000U && (result & 1U))) {
        ++result;
    }
    return static_cast<core::u16>(sign | result);
}

[[nodiscard]] bool ends_with_ignoring_case(
    std::string_view value, std::string_view suffix) noexcept {
    if (value.size() < suffix.size()) {
        return false;
    }
    const std::string_view tail = value.substr(value.size() - suffix.size());
    for (std::size_t index = 0U; index < suffix.size(); ++index) {
        const auto left = static_cast<unsigned char>(tail[index]);
        const auto right = static_cast<unsigned char>(suffix[index]);
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

} // namespace

bool radiance_source(std::string_view source) noexcept {
    return ends_with_ignoring_case(source, ".hdr");
}

DecodedFrame decode_radiance_bytes(
    std::span<const core::u8> bytes) noexcept {
    if (bytes.empty() ||
        bytes.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return DecodedFrame{};
    }
    // Radiance only. stbi_loadf will happily promote an 8-bit PNG to float,
    // which would look like it worked and give an environment with no values
    // above one - a map that lights nothing. Refuse it here rather than
    // shipping a flat result.
    if (stbi_is_hdr_from_memory(
            bytes.data(), static_cast<int>(bytes.size())) == 0) {
        return DecodedFrame{};
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    float* const pixels = stbi_loadf_from_memory(
        bytes.data(), static_cast<int>(bytes.size()), &width, &height,
        &channels, 4);
    if (pixels == nullptr || width <= 0 || height <= 0) {
        stbi_image_free(pixels);
        return DecodedFrame{};
    }
    const std::unique_ptr<float, void (*)(void*)> owned{
        pixels, [](void* value) { stbi_image_free(value); }};

    const auto texels = static_cast<std::size_t>(width)
        * static_cast<std::size_t>(height);
    DecodedFrame frame{};
    frame.width = static_cast<core::u32>(width);
    frame.height = static_cast<core::u32>(height);
    frame.format = rhi::TextureFormat::Rgba16Float;
    frame.layout = DecodedPixelLayout::Rgba16FloatLinear;
    const std::size_t byte_count = texels * 4U * sizeof(core::u16);
    if (byte_count > frame.pixels.max_size()) {
        return DecodedFrame{};
    }
    try {
        frame.pixels.resize(byte_count);
    } catch (const std::bad_alloc&) {
        return DecodedFrame{};
    }
    for (std::size_t index = 0U; index < texels; ++index) {
        const float* const source = owned.get() + (index * 4U);
        std::array<core::u16, 4U> half{
            half_from_float(source[0U]),
            half_from_float(source[1U]),
            half_from_float(source[2U]),
            // Opaque. A radiance map has no transparency, and whatever the
            // source left here would otherwise multiply into the lighting.
            half_from_float(1.0F),
        };
        std::memcpy(frame.pixels.data() + (index * 4U * sizeof(core::u16)),
                    half.data(), half.size() * sizeof(core::u16));
    }
    return frame;
}

std::shared_ptr<const DecodedFrame> MediaSource::try_decode(
    const std::string& source, core::u32 frame_index) {
    return std::make_shared<const DecodedFrame>(decode(source, frame_index));
}

std::shared_ptr<const DecodedFrame> MediaSource::try_decode(
    const std::string& source, core::u32 frame_index, core::u64 stream) {
    static_cast<void>(stream);
    return try_decode(source, frame_index);
}

std::shared_ptr<const DecodedFrame> MediaSource::try_decode_latest(
    const std::string& source, core::u32 frame_index, core::u64 stream) {
    static_cast<void>(source);
    static_cast<void>(frame_index);
    static_cast<void>(stream);
    return nullptr;
}

void MediaSource::prefetch(const std::string&, core::u32) {}

void MediaSource::prefetch(
    const std::string& source, core::u32 frame_index, core::u64 stream) {
    static_cast<void>(stream);
    prefetch(source, frame_index);
}

bool MediaSource::refresh(const std::string&) noexcept { return false; }

std::string ImageSequenceSource::resolve_path(
    const std::string& source, core::u32 frame_index) {
    const auto begin = source.find('#');
    if (begin == std::string::npos) {
        // No frame field: a still image, whose path is the template itself.
        return source;
    }
    auto end = begin;
    while (end < source.size() && source[end] == '#') {
        ++end;
    }
    const auto width = end - begin;
    auto number = std::to_string(frame_index);
    if (number.size() < width) {
        number.insert(0U, width - number.size(), '0');
    }
    return source.substr(0U, begin) + number + source.substr(end);
}

std::string ImageSequenceSource::key(
    const std::string& source, core::u32 frame_index) const {
    return resolve_path(source, frame_index);
}

DecodedFrame ImageSequenceSource::decode(
    const std::string& source, core::u32 frame_index) {
    const auto path = resolve_path(source, frame_index);
    if (path.empty()) {
        return DecodedFrame{};
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* const pixels =
        stbi_load(path.c_str(), &width, &height, &channels, 4);
    return decoded_frame(pixels, width, height);
}

} // namespace mediaserver::render
