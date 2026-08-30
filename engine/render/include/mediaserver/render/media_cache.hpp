#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/render/media_source.hpp>
#include <mediaserver/rhi/device.hpp>

#include <cstddef>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

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

// The half-open range of frame numbers a clip cycles over. The cache reads
// ahead inside it, so the frames it warms across a loop point are the clip's
// own first frames rather than the ones past the end of the file - decoding
// past the end produces nothing and leaves the loop to be seeked cold, which is
// the stutter an operator sees on every repeat. A zero count means the caller
// does not know the range and read-ahead stays linear.
struct MediaLoopWindow final {
    core::u32 first{};
    core::u32 count{};

    bool operator==(const MediaLoopWindow&) const = default;
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
    //
    // `window` is the clip's loop range, used only to aim the read-ahead.
    // `clip` names the placement asking, so a pending decode on one placement
    // holds that placement's own last frame; two clips cut from the same file
    // must never show each other's picture while one of them is waiting. Both
    // default to "not stated", which is the behaviour of a caller that has
    // neither - one clip per file, read ahead in a straight line.
    [[nodiscard]] const rhi::Texture* resolve(
        rhi::Device& device,
        rhi::CommandList& commands,
        const std::string& source_template,
        core::u32 frame_index,
        MediaLoopWindow window = {},
        core::u64 clip = 0U);

    // Asks the source to decode a frame nothing is drawing yet - the first
    // frame of a clip that is about to cut in. Uploads nothing and resolves
    // nothing; it only buys the decoder the head start that reaching an
    // arbitrary frame of a long-GOP file needs.
    void prewarm(const std::string& source_template, core::u32 frame_index);

    [[nodiscard]] const MediaCacheStats& stats() const noexcept {
        return stats_;
    }

private:
    struct ResidentFrame final {
        std::string key;
        rhi::Texture texture;
        std::size_t bytes{};
        bool pinned{};
    };

    using ResidentList = std::list<ResidentFrame>;
    // A placement, not a file: (source template, clip identity).
    using ClipKey = std::pair<std::string, core::u64>;

    // Four ordinary slots cover current + short look-ahead while the byte cap
    // is authoritative for high-resolution media. Bytes include the sampled
    // texture and its same-sized persistent streaming-upload buffer. More than
    // four slots remain only when one frame references that many distinct clips.
    static constexpr std::size_t max_resident_frames = 4U;
    static constexpr std::size_t max_resident_bytes = 256U * 1024U * 1024U;

    // How many frames of decoded picture to keep ahead of the playhead. One
    // frame of look-ahead leaves no headroom at all: any hitch in the decoder
    // is immediately a held stale frame on the wall. These live in the source's
    // CPU residency, not here, so the count is bounded by that byte budget
    // rather than by this cache's texture slots.
    static constexpr core::u32 read_ahead_frames = 4U;
    // Beyond this, the show is not playing the clip - it is scrubbing or
    // cutting - and reading further along the old stride would be guesswork.
    static constexpr core::i64 max_playback_stride = 4;

    [[nodiscard]] const rhi::Texture* held_texture(const ClipKey& clip);
    void erase(ResidentList::iterator entry) noexcept;
    void trim() noexcept;
    void refresh_resident_stats() noexcept;

    std::unique_ptr<MediaSource> source_;
    ResidentList resident_;
    std::map<std::string, ResidentList::iterator> by_key_;
    std::map<ClipKey, std::string> last_by_clip_;
    std::map<ClipKey, core::u32> last_request_by_clip_;
    // Which placements were drawn in the previous pass, so a clip coming back
    // on air is told apart from one that never left. Both of the things kept
    // per clip above - the frame it holds while waiting, and the direction it
    // was travelling - describe a run of play, and neither survives the gap.
    std::set<ClipKey> on_air_;
    std::set<ClipKey> on_air_now_;
    std::size_t resident_bytes_{};
    MediaCacheStats stats_{};
};

} // namespace mediaserver::render
