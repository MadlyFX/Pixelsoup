#include <mediaserver/render/media_cache.hpp>

#include <span>
#include <utility>

namespace mediaserver::render {

MediaCache::MediaCache() : source_(make_media_source()) {}

MediaCache::MediaCache(std::unique_ptr<MediaSource> source)
    : source_(std::move(source)) {}

const rhi::Texture* MediaCache::resolve(
    rhi::Device& device,
    rhi::CommandList& commands,
    const std::string& source_template,
    core::u32 frame_index) {
    const auto key = source_->key(source_template, frame_index);
    if (key.empty()) {
        return nullptr;
    }
    const auto found = textures_.find(key);
    if (found != textures_.end()) {
        return &found->second;
    }

    const auto frame = source_->decode(source_template, frame_index);
    if (!frame.valid()) {
        return nullptr;
    }

    rhi::TextureDesc desc{};
    desc.width = frame.width;
    desc.height = frame.height;
    desc.format = frame.format;
    desc.sampled = true;
    auto texture = device.create_texture(desc);
    if (!texture) {
        return nullptr;
    }

    const bool uploaded = commands.upload(
        texture.value(),
        std::span<const core::u8>{frame.pixels.data(), frame.pixels.size()});
    if (!uploaded) {
        return nullptr;
    }

    const auto inserted = textures_.emplace(key, std::move(texture).value());
    order_.push_back(key);
    // Evict oldest-first. The just-inserted key is newest, so it is never the
    // one evicted here even at the cap.
    while (textures_.size() > max_resident && !order_.empty()) {
        const auto oldest = order_.front();
        order_.pop_front();
        if (oldest != key) {
            textures_.erase(oldest);
        }
    }
    return &inserted.first->second;
}

} // namespace mediaserver::render
