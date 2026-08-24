#include <mediaserver/render/media_cache.hpp>

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

#include <span>
#include <utility>

namespace mediaserver::render {

std::string MediaCache::resolve_path(
    const std::string& source_template,
    core::u32 frame_index) {
    const auto begin = source_template.find('#');
    if (begin == std::string::npos) {
        // No frame field: a still image, whose path is the template itself.
        return source_template;
    }
    auto end = begin;
    while (end < source_template.size() && source_template[end] == '#') {
        ++end;
    }
    const auto width = end - begin;
    auto number = std::to_string(frame_index);
    if (number.size() < width) {
        number.insert(0U, width - number.size(), '0');
    }
    return source_template.substr(0U, begin) + number
        + source_template.substr(end);
}

const rhi::Texture* MediaCache::resolve(
    rhi::Device& device,
    rhi::CommandList& commands,
    const std::string& source_template,
    core::u32 frame_index) {
    const auto path = resolve_path(source_template, frame_index);
    if (path.empty()) {
        return nullptr;
    }
    const auto found = textures_.find(path);
    if (found != textures_.end()) {
        return &found->second;
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
        return nullptr;
    }

    rhi::TextureDesc desc{};
    desc.width = static_cast<core::u32>(width);
    desc.height = static_cast<core::u32>(height);
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.sampled = true;
    auto texture = device.create_texture(desc);
    if (!texture) {
        stbi_image_free(pixels);
        return nullptr;
    }

    const auto bytes = static_cast<std::size_t>(width)
        * static_cast<std::size_t>(height) * 4U;
    const bool uploaded = commands.upload(
        texture.value(),
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(pixels), bytes});
    stbi_image_free(pixels);
    if (!uploaded) {
        return nullptr;
    }

    const auto inserted = textures_.emplace(path, std::move(texture).value());
    order_.push_back(path);
    // Evict oldest-first. The just-inserted path is newest, so it is never the
    // one evicted here even at the cap.
    while (textures_.size() > max_resident && !order_.empty()) {
        const auto oldest = order_.front();
        order_.pop_front();
        if (oldest != path) {
            textures_.erase(oldest);
        }
    }
    return &inserted.first->second;
}

} // namespace mediaserver::render
