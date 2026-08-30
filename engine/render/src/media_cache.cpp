#include <mediaserver/render/media_cache.hpp>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace mediaserver::render {
namespace {

// Where `frame_index` sits inside the clip's loop, or nothing when the caller
// stated no loop range - or stated one the frame is not actually inside, which
// a caller mixing hand-built requests with real ones can do.
[[nodiscard]] std::optional<core::u32> offset_within(
    const MediaLoopWindow& window, core::u32 frame_index) noexcept {
    if (window.count == 0U || frame_index < window.first) {
        return std::nullopt;
    }
    const core::u32 offset = frame_index - window.first;
    return offset < window.count ? std::optional{offset} : std::nullopt;
}

// How far the show moved through the clip since the last request, measured the
// short way around the loop. A clip that wrapped from its last frame to its
// first advanced by one, not backwards across its whole length: without this
// every loop point reads as a reverse scrub and aims the read-ahead at exactly
// the frames the show has just finished with.
[[nodiscard]] core::i64 loop_delta(
    const MediaLoopWindow& window,
    core::u32 previous,
    core::u32 current) noexcept {
    const auto from = offset_within(window, previous);
    const auto to = offset_within(window, current);
    if (!from.has_value() || !to.has_value()) {
        return static_cast<core::i64>(current) - static_cast<core::i64>(previous);
    }
    const auto count = static_cast<core::i64>(window.count);
    const core::i64 forward =
        (static_cast<core::i64>(*to) - static_cast<core::i64>(*from) + count)
        % count;
    return forward * 2 <= count ? forward : forward - count;
}

// The frame `delta` steps along from `frame_index`, wrapped inside the loop.
// With no loop range it is plain arithmetic that refuses to run off either end
// of the frame numbering rather than wrapping around it.
[[nodiscard]] std::optional<core::u32> advance(
    const MediaLoopWindow& window,
    core::u32 frame_index,
    core::i64 delta) noexcept {
    if (const auto offset = offset_within(window, frame_index)) {
        const auto count = static_cast<core::i64>(window.count);
        core::i64 next = (static_cast<core::i64>(*offset) + delta) % count;
        if (next < 0) {
            next += count;
        }
        return window.first + static_cast<core::u32>(next);
    }
    const core::i64 next = static_cast<core::i64>(frame_index) + delta;
    constexpr auto highest =
        static_cast<core::i64>(std::numeric_limits<core::u32>::max());
    if (next < 0 || next > highest) {
        return std::nullopt;
    }
    return static_cast<core::u32>(next);
}

} // namespace

MediaCache::MediaCache() : source_(make_media_source()) {}

MediaCache::MediaCache(std::unique_ptr<MediaSource> source)
    : source_(std::move(source)) {}

void MediaCache::begin_frame() noexcept {
    for (auto& frame : resident_) {
        frame.pinned = false;
    }
    on_air_.swap(on_air_now_);
    on_air_now_.clear();
    stats_ = MediaCacheStats{};
    stats_.byte_budget = max_resident_bytes;
    refresh_resident_stats();
}

void MediaCache::end_frame() noexcept {
    trim();
    refresh_resident_stats();
}

const rhi::Texture* MediaCache::held_texture(const ClipKey& clip) {
    const auto latest = last_by_clip_.find(clip);
    if (latest == last_by_clip_.end()) {
        return nullptr;
    }
    const auto found = by_key_.find(latest->second);
    if (found == by_key_.end()) {
        last_by_clip_.erase(latest);
        return nullptr;
    }
    found->second->pinned = true;
    resident_.splice(resident_.end(), resident_, found->second);
    ++stats_.held_frames;
    return &found->second->texture;
}

void MediaCache::erase(ResidentList::iterator entry) noexcept {
    // Several placements can be holding this one texture - two clips cut from
    // the same file and sitting on the same frame share it - so every claim on
    // it is dropped, not just the first one found.
    for (auto held = last_by_clip_.begin(); held != last_by_clip_.end();) {
        held = held->second == entry->key ? last_by_clip_.erase(held)
                                          : std::next(held);
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

void MediaCache::prewarm(
    const std::string& source_template, core::u32 frame_index, core::u64 clip) {
    source_->prefetch(source_template, frame_index, clip);
}

const rhi::Texture* MediaCache::resolve(
    rhi::Device& device,
    rhi::CommandList& commands,
    const std::string& source_template,
    core::u32 frame_index,
    MediaLoopWindow window,
    core::u64 clip) {
    const auto key = source_->key(source_template, frame_index);
    if (key.empty()) {
        return nullptr;
    }
    const ClipKey clip_key{source_template, clip};
    // A clip that was not on air last pass is starting, not continuing - the
    // show has cut to it, or looped back onto it. What it was doing when it
    // last left the air says nothing about now: the frame it ended on is
    // further into the file than the one it is about to open with, and the
    // direction it was travelling then is not the direction it is travelling
    // now. Holding either is how the end of a clip appears on the wall at the
    // moment it begins.
    //
    // Decided once per pass, on the placement's first resolve: a radially
    // expanded clip resolves several times against one identity, and the second
    // of those is continuing what the first just established, not re-entering.
    const bool first_resolve_this_pass = on_air_now_.insert(clip_key).second;
    if (first_resolve_this_pass && on_air_.find(clip_key) == on_air_.end()) {
        last_by_clip_.erase(clip_key);
        last_request_by_clip_.erase(clip_key);
    }
    // How far the show moved this frame is how far it will move next frame, so
    // the stride is taken from what was actually asked for rather than assumed
    // to be one: a 24fps clip on a 60fps show advances by less than a frame at
    // a time, a 60fps clip on a 30fps show advances by two.
    const auto previous_request = last_request_by_clip_.find(clip_key);
    const core::i64 measured = previous_request == last_request_by_clip_.end()
        ? 1
        : loop_delta(window, previous_request->second, frame_index);
    last_request_by_clip_[clip_key] = frame_index;

    // A step larger than playback is the show moving, not the clip playing: a
    // cut, a scrub, or the show's own range looping under a clip that never
    // left the air. Whichever way it went, play resumes *forward* from where it
    // landed, so the read-ahead does too. Following the jump backwards instead
    // walks out of the clip towards the end of the file, which is the one place
    // the frames coming next certainly are not.
    const bool jumped = std::abs(measured) > max_playback_stride;
    const core::i64 stride = jumped || measured == 0 ? 1 : measured;
    // A jump backwards also retires the held frame. It sits further into the
    // clip than the one now being asked for, so holding it puts a picture the
    // clip has not reached yet on the wall - and where several clips are cut
    // from one file, that picture is recognisably a later clip's. Nothing is
    // the honest answer until the decode lands. A forward jump is left holding:
    // the frame behind is ordinary judder, not a glimpse of the future.
    if (jumped && measured < 0) {
        last_by_clip_.erase(clip_key);
    }
    const auto read_ahead = [&]() {
        for (core::u32 ahead = 1U; ahead <= read_ahead_frames; ++ahead) {
            const auto target = advance(
                window, frame_index, stride * static_cast<core::i64>(ahead));
            if (!target.has_value()) {
                return;
            }
            source_->prefetch(source_template, *target, clip);
        }
    };
    const auto found = by_key_.find(key);
    if (found != by_key_.end()) {
        found->second->pinned = true;
        resident_.splice(resident_.end(), resident_, found->second);
        last_by_clip_[clip_key] = key;
        ++stats_.cache_hits;
        read_ahead();
        return &found->second->texture;
    }

    const auto frame =
        source_->try_decode(source_template, frame_index, clip);
    read_ahead();
    if (frame == nullptr) {
        ++stats_.pending_requests;
        return held_texture(clip_key);
    }
    if (!frame->valid()) {
        return held_texture(clip_key);
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
            return held_texture(clip_key);
        }
        texture = std::move(created).value();
    }

    const bool uploaded = commands.upload(
        texture,
        std::span<const core::u8>{frame->pixels.data(), frame->pixels.size()});
    if (!uploaded) {
        return held_texture(clip_key);
    }

    resident_.push_back(
        ResidentFrame{key, std::move(texture), slot_bytes, true});
    const auto inserted = std::prev(resident_.end());
    by_key_[key] = inserted;
    last_by_clip_[clip_key] = key;
    resident_bytes_ += inserted->bytes;
    ++stats_.uploads;
    refresh_resident_stats();
    return &inserted->texture;
}

} // namespace mediaserver::render
