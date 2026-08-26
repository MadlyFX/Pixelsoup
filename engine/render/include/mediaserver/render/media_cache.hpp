#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/render/media_source.hpp>
#include <mediaserver/rhi/device.hpp>

#include <cstddef>
#include <list>
#include <map>
#include <memory>
#include <string>

// Resolves a clip's document reference to a GPU texture, and keeps recent ones.
//
// The decode itself lives behind a MediaSource (media_source.hpp); this type is
// only the GPU residency around it: on a miss it asks the source to decode, then
// uploads and caches by the source's key, evicting oldest-first. Swapping the
// source - image sequence today, a real codec later - changes what is decoded
// without touching anything above this seam.
namespace mediaserver::render {

struct MediaCacheStats final {
    std::size_t resident_frames{};
    std::size_t resident_bytes{};
    std::size_t byte_budget{};
    core::u64 cache_hits{};
    core::u64 uploads{};
    core::u64 pending_requests{};
    core::u64 held_frames{};

    bool operator==(const MediaCacheStats&) const = default;
};

class MediaCache final {
public:
    // Defaults to an image-sequence source; a caller can inject another (a HAP
    // or FFmpeg source, or a stub in a test).
    MediaCache();
    explicit MediaCache(std::unique_ptr<MediaSource> source);

    // Pins every texture resolved until end_frame(), so a reusable slot can
    // never be overwritten while the same command list still references it.
    // SceneRenderer brackets its media pass with these calls.
    void begin_frame() noexcept;
    void end_frame() noexcept;

    // Resolves (source, frame) to a texture, decoding via the MediaSource and
    // uploading on `commands` on a miss. Returns null when the source cannot
    // produce the frame, which the caller renders as nothing rather than a
    // failed frame.
    [[nodiscard]] const rhi::Texture* resolve(
        rhi::Device& device,
        rhi::CommandList& commands,
        const std::string& source_template,
        core::u32 frame_index);

    [[nodiscard]] const MediaCacheStats& stats() const noexcept {
        return stats_;
    }

private:
    struct ResidentFrame final {
        std::string key;
        std::string source;
        rhi::Texture texture;
        std::size_t bytes{};
        bool pinned{};
    };

    using ResidentList = std::list<ResidentFrame>;

    // Four ordinary slots cover current + short look-ahead while the byte cap
    // is authoritative for high-resolution media. Bytes include the sampled
    // texture and its same-sized persistent streaming-upload buffer. More than
    // four slots remain only when one frame references that many distinct clips.
    static constexpr std::size_t max_resident_frames = 4U;
    static constexpr std::size_t max_resident_bytes = 256U * 1024U * 1024U;

    [[nodiscard]] const rhi::Texture* held_texture(
        const std::string& source_template);
    void erase(ResidentList::iterator entry) noexcept;
    void trim() noexcept;
    void refresh_resident_stats() noexcept;

    std::unique_ptr<MediaSource> source_;
    ResidentList resident_;
    std::map<std::string, ResidentList::iterator> by_key_;
    std::map<std::string, std::string> last_by_source_;
    std::map<std::string, core::u32> last_request_by_source_;
    std::size_t resident_bytes_{};
    MediaCacheStats stats_{};
};

} // namespace mediaserver::render
