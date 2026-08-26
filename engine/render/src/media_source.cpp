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

namespace mediaserver::render {

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
    if (pixels == nullptr || width <= 0 || height <= 0) {
        if (pixels != nullptr) {
            stbi_image_free(pixels);
        }
        return DecodedFrame{};
    }

    const auto bytes = static_cast<std::size_t>(width)
        * static_cast<std::size_t>(height) * 4U;
    DecodedFrame frame{};
    frame.width = static_cast<core::u32>(width);
    frame.height = static_cast<core::u32>(height);
    frame.format = rhi::TextureFormat::Rgba8Unorm;
    frame.pixels.assign(pixels, pixels + bytes);
    stbi_image_free(pixels);
    return frame;
}

} // namespace mediaserver::render
