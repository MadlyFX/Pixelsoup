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
#include <vector>

// Resolves a clip's document reference to a GPU texture, and keeps recent ones.
//
// The decode itself lives behind a MediaSource (media_source.hpp); this type is
// only the GPU residency around it: on a miss it asks the source to decode,
// then uploads and caches by the source's key, evicting oldest-first. Swapping
// the source - image sequence today, a real codec later - changes what is
// decoded without touching anything above this seam.
namespace mediaserver::render {

struct MediaCacheStats final {
    std::size_t resident_frames{};
    std::size_t resident_bytes{};
    std::size_t byte_budget{};
    core::u64 cache_hits{};
    core::u64 uploads{};
    core::u64 planar_uploads{};
    core::u64 pending_requests{};
    core::u64 held_frames{};

    bool operator==(const MediaCacheStats &) const = default;
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

    bool operator==(const MediaLoopWindow &) const = default;
};

// One frame in GPU residency. Ordinary decoded images use `primary` as RGBA.
// P010 video uses it as the R16 luma plane and supplies an RG16 chroma plane;
// the scene media shader samples both directly. The pointers remain valid for
// the command-list frame bracketed by MediaCache::begin_frame/end_frame.
struct ResolvedMediaFrame final {
    const rhi::Texture *primary{};
    const rhi::Texture *chroma{};
    DecodedPixelLayout layout{DecodedPixelLayout::Rgba8};

    [[nodiscard]] bool planar() const noexcept {
        return layout == DecodedPixelLayout::P010Rec709Limited ||
               layout == DecodedPixelLayout::P010Rec709Full;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return primary != nullptr && (!planar() || chroma != nullptr);
    }
};

struct MediaCacheConfig final {
    core::u32 read_ahead_frames{3U};
    std::size_t max_read_ahead_bytes{384U * 1024U * 1024U};
    std::size_t max_read_ahead_requests{32U};
    std::size_t unknown_frame_bytes{32U * 1024U * 1024U};

    [[nodiscard]] static constexpr MediaCacheConfig buffered_output(
        core::u32 read_ahead = 12U,
        std::size_t decoded_cache_bytes = 2U * 1024U * 1024U * 1024U) noexcept {
        // Speculative work receives three quarters of decoded residency. The
        // remainder leaves space for current frames, a retained trail, and
        // cut/loop warm-up. Request count assumes at most sixteen independently
        // timed placements, which is also the renderer's retained stream cap.
        const core::u32 ahead = read_ahead == 0U ? 1U : read_ahead;
        const std::size_t bytes =
            decoded_cache_bytes == 0U ? 1U : decoded_cache_bytes;
        const std::size_t requests = static_cast<std::size_t>(ahead) * 16U;
        return MediaCacheConfig{ahead, bytes - bytes / 4U,
                                requests < 32U ? 32U : requests,
                                32U * 1024U * 1024U};
    }
};

class MediaCache final {
  public:
    // Defaults to an image-sequence source; a caller can inject another (a HAP
    // or FFmpeg source, or a stub in a test).
    MediaCache();
    explicit MediaCache(MediaCacheConfig config);
    explicit MediaCache(std::unique_ptr<MediaSource> source,
                        MediaCacheConfig config = MediaCacheConfig{});

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
    [[nodiscard]] ResolvedMediaFrame
    resolve(rhi::Device &device, rhi::CommandList &commands,
            const std::string &source_template, core::u32 frame_index,
            MediaLoopWindow window = {}, core::u64 clip = 0U);

    // Asks the source to decode a frame nothing is drawing yet - the first
    // frame of a clip that is about to cut in. Uploads nothing and resolves
    // nothing; it only buys the decoder the head start that reaching an
    // arbitrary frame of a long-GOP file needs.
    void prewarm(const std::string &source_template, core::u32 frame_index,
                 core::u64 clip = 0U);

    [[nodiscard]] const MediaCacheStats &stats() const noexcept {
        return stats_;
    }

  private:
    struct ResidentFrame final {
        std::string key;
        DecodedPixelLayout layout{DecodedPixelLayout::Rgba8};
        // RGBA for ordinary frames; R16 luma for P010.
        rhi::Texture texture;
        rhi::Texture chroma_plane;
        std::size_t bytes{};
        bool pinned{};
    };

    using ResidentList = std::list<ResidentFrame>;
    // A placement, not a file: (source template, clip identity).
    using ClipKey = std::pair<std::string, core::u64>;

    struct ReadAheadRequest final {
        std::string source;
        core::u32 frame_index{};
        MediaLoopWindow window;
        core::i64 stride{1};
        core::u64 clip{};
        std::size_t frame_bytes{};
    };

    // One command list can contain Program and Stage. Six clips in each view
    // need twelve reusable slots when the playheads differ, while identical
    // (source, frame) pairs collapse to one. Bytes include the sampled texture
    // and its same-sized persistent streaming-upload buffer.
    static constexpr std::size_t max_resident_frames = 16U;
    static constexpr std::size_t max_resident_bytes = 512U * 1024U * 1024U;

    // How many frames of decoded picture to keep ahead of the playhead. One
    // frame of look-ahead leaves no headroom at all: any hitch in the decoder
    // is immediately a held stale frame on the wall. These live in the source's
    // CPU residency, not here, so the count is bounded by that byte budget
    // rather than by this cache's texture slots.
    // Look-ahead is speculative. Its configured complete per-pass working set
    // prevents a many-plate composition from continuously evicting and
    // re-decoding the same future frames while the playhead is stopped.
    // Beyond this, the show is not playing the clip - it is scrubbing or
    // cutting - and reading further along the old stride would be guesswork.
    static constexpr core::i64 max_playback_stride = 4;

    [[nodiscard]] ResolvedMediaFrame held_frame(const ClipKey &clip);
    void remember_read_ahead(const ClipKey &clip_key, const std::string &source,
                             core::u32 frame_index, MediaLoopWindow window,
                             core::i64 stride, core::u64 clip);
    void submit_read_ahead() noexcept;
    void erase(ResidentList::iterator entry) noexcept;
    void trim() noexcept;
    void refresh_resident_stats() noexcept;

    std::unique_ptr<MediaSource> source_;
    MediaCacheConfig config_{};
    ResidentList resident_;
    std::map<std::string, ResidentList::iterator> by_key_;
    std::map<ClipKey, std::string> last_by_clip_;
    std::map<ClipKey, core::u32> last_frame_by_clip_;
    std::map<ClipKey, core::u32> last_request_by_clip_;
    std::map<ClipKey, ReadAheadRequest> read_ahead_requests_;
    std::map<std::string, std::size_t> decoded_frame_bytes_;
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
