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

#include <cstddef>
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

std::shared_ptr<const DecodedFrame> MediaSource::try_decode(
    const std::string& source, core::u32 frame_index) {
    return std::make_shared<const DecodedFrame>(decode(source, frame_index));
}

void MediaSource::prefetch(const std::string&, core::u32) {}

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
