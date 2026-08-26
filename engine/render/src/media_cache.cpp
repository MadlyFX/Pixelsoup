#include <mediaserver/render/media_cache.hpp>

#include <algorithm>
#include <limits>
#include <span>
#include <utility>

namespace mediaserver::render {

MediaCache::MediaCache() : source_(make_media_source()) {}

MediaCache::MediaCache(std::unique_ptr<MediaSource> source)
    : source_(std::move(source)) {}

void MediaCache::begin_frame() noexcept {
    for (auto& frame : resident_) {
        frame.pinned = false;
    }
    stats_ = MediaCacheStats{};
    stats_.byte_budget = max_resident_bytes;
    refresh_resident_stats();
}

void MediaCache::end_frame() noexcept {
    trim();
    refresh_resident_stats();
}

const rhi::Texture* MediaCache::held_texture(
    const std::string& source_template) {
    const auto latest = last_by_source_.find(source_template);
    if (latest == last_by_source_.end()) {
        return nullptr;
    }
    const auto found = by_key_.find(latest->second);
    if (found == by_key_.end()) {
        last_by_source_.erase(latest);
        return nullptr;
    }
    found->second->pinned = true;
    resident_.splice(resident_.end(), resident_, found->second);
    ++stats_.held_frames;
    return &found->second->texture;
}

void MediaCache::erase(ResidentList::iterator entry) noexcept {
    const auto latest = last_by_source_.find(entry->source);
    if (latest != last_by_source_.end() && latest->second == entry->key) {
        last_by_source_.erase(latest);
    }
    by_key_.erase(entry->key);
    resident_bytes_ -= entry->bytes;
    resident_.erase(entry);
}

void MediaCache::trim() noexcept {
    while (resident_.size() > max_resident_frames
           || resident_bytes_ > max_resident_bytes) {
        const auto candidate = std::find_if(
            resident_.begin(), resident_.end(),
            [](const ResidentFrame& frame) { return !frame.pinned; });
        if (candidate == resident_.end()) {
            // Current-frame references are authoritative. A scene using more
            // than four independent clips may exceed the soft frame count, and
            // one texture larger than the byte budget must remain drawable.
            break;
        }
        erase(candidate);
    }
}

void MediaCache::refresh_resident_stats() noexcept {
    stats_.resident_frames = resident_.size();
    stats_.resident_bytes = resident_bytes_;
    stats_.byte_budget = max_resident_bytes;
}

const rhi::Texture* MediaCache::resolve(
    rhi::Device& device,
    rhi::CommandList& commands,
    const std::string& source_template,
    core::u32 frame_index) {
    const auto key = source_->key(source_template, frame_index);
    if (key.empty()) {
        return nullptr;
    }
    const auto previous_request = last_request_by_source_.find(source_template);
    const bool reverse = previous_request != last_request_by_source_.end()
        && frame_index < previous_request->second;
    last_request_by_source_[source_template] = frame_index;
    const auto prefetch_adjacent = [&]() {
        if (reverse && frame_index > 0U) {
            source_->prefetch(source_template, frame_index - 1U);
        } else if (!reverse
                   && frame_index != std::numeric_limits<core::u32>::max()) {
            source_->prefetch(source_template, frame_index + 1U);
        }
    };
    const auto found = by_key_.find(key);
    if (found != by_key_.end()) {
        found->second->pinned = true;
        resident_.splice(resident_.end(), resident_, found->second);
        last_by_source_[source_template] = key;
        ++stats_.cache_hits;
        prefetch_adjacent();
        return &found->second->texture;
    }

    const auto frame = source_->try_decode(source_template, frame_index);
    prefetch_adjacent();
    if (frame == nullptr) {
        ++stats_.pending_requests;
        return held_texture(source_template);
    }
    if (!frame->valid()) {
        return held_texture(source_template);
    }

    rhi::TextureDesc desc{};
    desc.width = frame->width;
    desc.height = frame->height;
    desc.format = frame->format;
    desc.sampled = true;
    desc.streaming_upload = true;
    const std::size_t pixel_bytes = frame->pixels.size();
    const std::size_t slot_bytes = pixel_bytes
            > std::numeric_limits<std::size_t>::max() / 2U
        ? std::numeric_limits<std::size_t>::max()
        : pixel_bytes * 2U;

    // Prefer an unpinned texture with an identical immutable description. Its
    // backing allocation and opt-in streaming transfer buffer are both reused,
    // avoiding per-frame GPU resource allocation on settled video playback.
    rhi::Texture texture;
    const auto reusable = std::find_if(
        resident_.begin(), resident_.end(),
        [&desc](const ResidentFrame& candidate) {
            return !candidate.pinned && candidate.texture.desc() == desc;
        });
    if (reusable != resident_.end()) {
        texture = std::move(reusable->texture);
        erase(reusable);
    } else {
        // Bound peak residency before allocating a differently sized frame.
        const std::size_t incoming_bytes = slot_bytes;
        while (!resident_.empty()
               && (resident_.size() >= max_resident_frames
                   || (incoming_bytes <= max_resident_bytes
                       && resident_bytes_ > max_resident_bytes - incoming_bytes))) {
            const auto candidate = std::find_if(
                resident_.begin(), resident_.end(),
                [](const ResidentFrame& held) { return !held.pinned; });
            if (candidate == resident_.end()) {
                break;
            }
            erase(candidate);
        }
        auto created = device.create_texture(desc);
        if (!created) {
            return held_texture(source_template);
        }
        texture = std::move(created).value();
    }

    const bool uploaded = commands.upload(
        texture,
        std::span<const core::u8>{frame->pixels.data(), frame->pixels.size()});
    if (!uploaded) {
        return held_texture(source_template);
    }

    resident_.push_back(ResidentFrame{
        key, source_template, std::move(texture), slot_bytes, true});
    const auto inserted = std::prev(resident_.end());
    by_key_[key] = inserted;
    last_by_source_[source_template] = key;
    resident_bytes_ += inserted->bytes;
    ++stats_.uploads;
    refresh_resident_stats();
    return &inserted->texture;
}

} // namespace mediaserver::render
