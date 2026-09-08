#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/render/media_source.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/scene.hpp>

#include <cstddef>
#include <array>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Resolves a clip's document reference to a GPU texture, and keeps recent ones.
//
// The decode itself lives behind a MediaSource (media_source.hpp). This type
// owns GPU residency plus a bounded CPU completion mailbox: a cold blocking
// miss is admitted to the process executor, then a later resolve uploads and
// caches the result by the source's key. Swapping the source changes what is
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

    MediaCache(const MediaCache &) = delete;
    MediaCache &operator=(const MediaCache &) = delete;
    MediaCache(MediaCache &&) noexcept;
    MediaCache &operator=(MediaCache &&) noexcept;
    ~MediaCache();

    // Pins every texture resolved until end_frame(), so a reusable slot can
    // never be overwritten while the same command list still references it.
    // SceneRenderer brackets its media pass with these calls.
    void begin_frame() noexcept;
    void end_frame() noexcept;

    // Resolves (source, frame) to a texture and uploads on `commands` on a
    // completed CPU-residency miss. Cold blocking sources are only queued to
    // bounded background work here; resolve returns the placement's last
    // ready texture, or null, without waiting for file or codec I/O.
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
    // nothing; it only queues bounded work to buy the decoder a head start.
    void prewarm(const std::string &source_template, core::u32 frame_index,
                 core::u64 clip = 0U);

    // Operator/control-path invalidation for a replaced or repaired source.
    // This may synchronize with source-owned worker publication and must not be
    // called from resolve(), the show clock, or a periodic task. Call it only at
    // a safe cache boundary where previously submitted work no longer refers to
    // this cache's textures. Ordinary project/cache reconstruction does not
    // invalidate the process-wide video registry.
    [[nodiscard]] bool refresh_source(
        const std::string &source_template) noexcept;
    // Replicated generation admission. Source invalidation executes on the
    // decode worker; this observation never waits for source teardown or IO.
    [[nodiscard]] bool adopt_generation(core::u64 generation,
        std::span<const scene::MediaInstance> media) noexcept;

    [[nodiscard]] const MediaCacheStats &stats() const noexcept {
        return stats_;
    }

  private:
    struct AsyncDecodeState;
    core::u64 authored_generation_{};

    struct ResidentFrame final {
        std::string key;
        std::string source;
        DecodedPixelLayout layout{DecodedPixelLayout::Rgba8};
        // RGBA for ordinary frames; R16 luma for P010.
        rhi::Texture texture;
        rhi::Texture chroma_plane;
        std::size_t bytes{};
        bool pinned{};
    };

    enum class CandidateUploadState : core::u8 {
        Retryable,
        RecordingComplete,
        RecordingPartial,
        Quarantined,
    };

    struct CandidateClaim final {
        core::u64 clip{};
        core::u32 resolved_frame{};
        std::string previous_key;
        bool had_previous{};
    };

    // A texture upload is provisional until its command recording is accepted
    // by Device::submit. Keeping both the decoded bytes and the GPU handles in
    // this bounded list makes cancellation a rollback rather than a poisoned
    // cache hit. A partial upload stays here as well, so no resource captured
    // by a command list can be destroyed on resolve's failure path.
    struct UploadCandidate final {
        std::string key;
        std::string source;
        core::u32 resolved_frame{};
        DecodedPixelLayout layout{DecodedPixelLayout::Rgba8};
        rhi::Texture texture;
        rhi::Texture chroma_plane;
        std::size_t bytes{};
        std::shared_ptr<const DecodedFrame> decoded;
        rhi::CommandRecordingToken recording;
        CandidateUploadState state{CandidateUploadState::Retryable};
        std::vector<CandidateClaim> claims;
        core::u64 stamp{};
        bool discard{};
    };

    // One command can render several mapped views. A held answer is the exact
    // texture identity observed by its first miss, not merely a note that the
    // key missed: another command may publish a replacement between those
    // views. Raw texture pointers remain stable while their owning list node is
    // protected below; candidate nodes survive until their recording resolves.
    struct DeferredSnapshot final {
        std::string key;
        core::u64 clip{};
        const rhi::Texture* primary{};
        const rhi::Texture* chroma{};
        DecodedPixelLayout layout{DecodedPixelLayout::Rgba8};
        bool occupied{};
    };

    struct DeferredRecording final {
        rhi::CommandRecordingToken recording;
        std::array<DeferredSnapshot, 64U> snapshots{};
    };

    using ResidentList = std::list<ResidentFrame>;
    using CandidateList = std::list<UploadCandidate>;
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
    static constexpr std::size_t max_upload_candidates = 16U;
    // Valid backends expose at most four recording contexts. Extra slots make
    // nested Program/Stage callers conservative without growing per key.
    static constexpr std::size_t max_deferred_recordings = 16U;

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
    [[nodiscard]] ResolvedMediaFrame candidate_frame(
        UploadCandidate &candidate, const ClipKey &clip,
        core::u32 resolved_frame) noexcept;
    void reconcile_recordings() noexcept;
    [[nodiscard]] std::optional<ResolvedMediaFrame> recording_snapshot(
        const rhi::CommandRecordingToken &recording,
        const std::string &key, core::u64 clip) const noexcept;
    [[nodiscard]] bool defer_recording(
        const rhi::CommandRecordingToken &recording,
        const std::string &key, core::u64 clip,
        ResolvedMediaFrame frame) noexcept;
    void remember_candidate_claim(
        UploadCandidate &candidate, const ClipKey &clip,
        core::u32 resolved_frame) noexcept;
    [[nodiscard]] bool resident_is_held(
        const std::string &key) const noexcept;
    void remember_read_ahead(const ClipKey &clip_key, const std::string &source,
                             core::u32 frame_index, MediaLoopWindow window,
                             core::i64 stride, core::u64 clip);
    void submit_read_ahead() noexcept;
    void erase(ResidentList::iterator entry) noexcept;
    void trim() noexcept;
    void refresh_resident_stats() noexcept;

    // Shared ownership lets an executing process-global job retain its source
    // without retaining this cache. Queued source/completion references are
    // weak, so cache teardown and move-assignment never join disk I/O.
    std::shared_ptr<AsyncDecodeState> async_decode_;
    std::shared_ptr<MediaSource> source_;
    MediaCacheConfig config_{};
    ResidentList resident_;
    CandidateList candidates_;
    // Keep the exact per-recording snapshots in one fixed-size heap block.
    // MediaCache is embedded twice by HybridSceneRenderer and is returned by
    // value during construction; placing the full 16 x 64 string table inline
    // makes those setup frames large enough to exhaust Windows' default stack.
    // The vector is sized once during cache construction and never grows on
    // the render path.
    std::vector<DeferredRecording> deferred_recordings_ =
        std::vector<DeferredRecording>(max_deferred_recordings);
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
    std::size_t candidate_bytes_{};
    core::u64 candidate_stamp_{};
    MediaCacheStats stats_{};
};

} // namespace mediaserver::render
