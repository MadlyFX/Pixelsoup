#pragma once

#include <mediaserver/render/image_decode.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

// The decode seam: a clip's (source, frame_index) turned into pixels.
//
// This is the boundary §6 of the plan draws between the renderer and codecs.
// Every implementation is *frame-exact* - the same (source, frame_index) yields
// the same pixels, and any frame is reachable directly, never "seek to the
// nearest keyframe" - and pure with respect to that pair, so two nodes decoding
// the same frame get the same bytes. Image sequences are the first
// implementation; HAP, ProRes/DNxHR via FFmpeg, and H.26x via NVDEC slot in
// behind this same call, so nothing above the renderer learns which codec a
// clip is. The MediaCache owns the GPU residency; a source only produces CPU
// pixels.
namespace mediaserver::render {

// How MediaCache may obtain a cold CPU frame from this source. The default is
// deliberately conservative: MediaSource::try_decode() calls decode()
// directly, so a source must opt in before the render thread may invoke it.
// SourceManaged implementations guarantee that try_decode(),
// try_decode_latest(), and prefetch() do not wait on file or codec I/O.
enum class MediaDecodeScheduling : core::u8 {
    BackgroundRequired,
    SourceManaged,
};

class MediaSource {
  public:
    MediaSource() = default;
    MediaSource(const MediaSource &) = delete;
    MediaSource &operator=(const MediaSource &) = delete;
    MediaSource(MediaSource &&) = delete;
    MediaSource &operator=(MediaSource &&) = delete;
    virtual ~MediaSource() = default;

    // A stable key identifying (source, frame_index), used to cache the decoded
    // frame. Pure: no file access, no GPU. Empty means the pair names nothing.
    [[nodiscard]] virtual std::string key(const std::string &source,
                                          core::u32 frame_index) const = 0;

    // Pure scheduling declaration for one source path. Dispatching sources
    // may return SourceManaged for video and BackgroundRequired for stills or
    // image sequences. It must not inspect the filesystem or decoder here.
    // If the answer can vary by path, decode() for a BackgroundRequired path
    // may run concurrently with render-thread key/try_decode/prefetch calls
    // for SourceManaged paths; those modes must not share mutable state.
    [[nodiscard]] virtual MediaDecodeScheduling scheduling(
        const std::string &source) const noexcept;

    // Decodes the frame to CPU pixels. Returns an invalid frame on any failure
    // (missing file, undecodable data), which the renderer draws as nothing.
    // Non-const because a video source keeps an open decoder to walk frames
    // efficiently rather than re-opening the file each time.
    [[nodiscard]] virtual DecodedFrame decode(const std::string &source,
                                              core::u32 frame_index) = 0;

    // Frame access for a SourceManaged implementation. The inherited adapter
    // decodes immediately and is therefore BackgroundRequired; MediaCache
    // never invokes it from the render path, and calls decode() only on its
    // bounded executor. Video sources override this with a non-blocking poll;
    // null then means "pending", while a non-null invalid frame means decode
    // completed without a usable image. The immutable shared frame lets every
    // in-process renderer consume one CPU decode without copying its pixels.
    [[nodiscard]] virtual std::shared_ptr<const DecodedFrame>
    try_decode(const std::string &source, core::u32 frame_index);

    // The same file can be on air through several independently timed clip
    // placements. `stream` identifies that playback continuity, allowing an
    // asynchronous source to retain one forward-walking decoder per clip
    // instead of making two playheads continually seek and displace each
    // other's current-frame request. Synchronous sources inherit the adapter
    // and remain pure functions of (source, frame_index).
    [[nodiscard]] virtual std::shared_ptr<const DecodedFrame>
    try_decode(const std::string &source, core::u32 frame_index,
               core::u64 stream);

    // Optional non-blocking fallback for a moving playback stream. Returns the
    // newest completed frame at or shortly before frame_index, with its actual
    // ordinal in DecodedFrame::source_frame. Exact callers never use this;
    // MediaCache uses it only to refresh a clip's held texture while the exact
    // request remains pending.
    [[nodiscard]] virtual std::shared_ptr<const DecodedFrame>
    try_decode_latest(const std::string &source, core::u32 frame_index,
                      core::u64 stream);

    // Poll only: no demand admission, seek, or playback-continuity update.
    // Used to upload speculative frames without promoting them over live work.
    [[nodiscard]] virtual std::shared_ptr<const DecodedFrame>
    try_prefetched(const std::string &source, core::u32 frame_index,
                   core::u64 stream);

    // A bounded look-ahead hint. Synchronous sources ignore it, and an
    // asynchronous implementation must never perform I/O on the caller.
    virtual void prefetch(const std::string &source, core::u32 frame_index);
    virtual void prefetch(const std::string &source, core::u32 frame_index,
                          core::u64 stream);

    // Explicitly invalidates machine-local decode residency for an operator-
    // requested source refresh. This is deliberately not a frame-path method:
    // a SourceManaged implementation may wait for its short scheduler critical
    // section and retire decoder state here. Failed video frames otherwise stay
    // failed indefinitely; neither wall time nor repeated resolves retries I/O.
    // False means this source has no refresh seam or could not invalidate it.
    [[nodiscard]] virtual bool refresh(const std::string &source) noexcept;
};

// A numbered still-image sequence decoded with stb_image. The source is a path
// template with a run of '#' marking the zero-padded frame number
// (plate_####.png); a template with no '#' is a single still, held for every
// frame. This is the spike-grade path §6 stands up first.
class ImageSequenceSource final : public MediaSource {
  public:
    [[nodiscard]] std::string key(const std::string &source,
                                  core::u32 frame_index) const override;

    [[nodiscard]] DecodedFrame decode(const std::string &source,
                                      core::u32 frame_index) override;

    // Substitutes the zero-padded frame number into a '#'-run template. Pure
    // and testable with no file or GPU. Deliberately not a printf format, so a
    // document-supplied string cannot be a format-string attack.
    [[nodiscard]] static std::string resolve_path(const std::string &source,
                                                  core::u32 frame_index);
};

// The media source the MediaCache uses by default: routes each source to the
// image-sequence decoder or the video decoder by file extension, so a clip can
// be a plate sequence or an mp4 without the renderer knowing which.
[[nodiscard]] std::unique_ptr<MediaSource> make_media_source();

// Process-wide CPU decode residency. Video workers and their immutable ready
// frames are shared by source path, so Program, Stage, and compositor layers do
// not decode the same MP4 independently. The byte cap is authoritative; frame
// count is a secondary guard for small media and failed-frame records.
struct MediaDecodeStats final {
    std::size_t active_sources{};
    // Sources whose active FFmpeg decoder negotiated a hardware pixel format.
    // Zero is the expected fallback on unsupported codecs or machines.
    std::size_t hardware_sources{};
    // Sources for which a hardware device/configuration was available, even
    // when the stream profile ultimately required software fallback.
    std::size_t hardware_configured_sources{};
    // Independent clip playheads retaining decoder continuity. Several may
    // refer to one active source while sharing its immutable ready frames.
    std::size_t active_streams{};
    std::size_t pending_requests{};
    std::size_t ready_frames{};
    std::size_t ready_bytes{};
    std::size_t byte_budget{};
    core::u32 trail_frames{};
    core::u64 completed_frames{};
    core::u64 failed_frames{};

    bool operator==(const MediaDecodeStats &) const = default;
};

struct MediaDecodeConfig final {
    std::size_t maximum_ready_bytes{2U * 1024U * 1024U * 1024U};
    std::size_t maximum_ready_frames{192U};
    std::size_t maximum_requests_per_source{16U};
    core::u32 trail_frames{};

    [[nodiscard]] static constexpr MediaDecodeConfig
    buffered_output(core::u32 read_ahead = 12U,
                    std::size_t ready_bytes = 2U * 1024U * 1024U * 1024U,
                    core::u32 trail = 0U) noexcept {
        const std::size_t working_frames =
            (static_cast<std::size_t>(read_ahead) +
             static_cast<std::size_t>(trail) + 1U) *
            16U;
        return MediaDecodeConfig{ready_bytes == 0U ? 1U : ready_bytes,
                                 working_frames < 48U ? 48U : working_frames,
                                 static_cast<std::size_t>(read_ahead) + 4U < 6U
                                     ? 6U
                                     : static_cast<std::size_t>(read_ahead) +
                                           4U,
                                 trail};
    }
};

[[nodiscard]] MediaDecodeStats media_decode_stats();

// Configures the process-wide video scheduler for a render node that
// intentionally presents behind the control stream. Output presenters in one
// process share this machine-local configuration. The limits are upper bounds,
// not eager allocations.
void configure_buffered_output_media_decode(
    MediaDecodeConfig config = MediaDecodeConfig{}) noexcept;

// The number of frames a media file holds, for a clip's loop length: a video's
// frame count, or 1 for a still image (and on any failure). Opens and probes
// the file, so it is called once when a clip is added, not per frame.
[[nodiscard]] core::u32 probe_frame_count(const std::string &source);

// A video's exact native frame rate, reduced to a positive rational. Still
// images and image sequences have no intrinsic playback rate, and failures are
// reported as absence so their existing one-source-frame-per-show-frame
// behaviour remains available to the scene layer.
struct MediaFrameRate final {
    core::u32 numerator{};
    core::u32 denominator{};

    bool operator==(const MediaFrameRate &) const = default;
};

// Container metadata used when a source is added or imported. A single probe
// supplies every field so callers do not repeatedly open a large video merely
// to ask separate questions about its duration, cadence, and dimensions.
struct MediaProbe final {
    core::u32 frame_count{1U};
    std::optional<MediaFrameRate> frame_rate;
    core::u32 width{};
    core::u32 height{};
    std::string codec;
    std::string profile;
    std::string pixel_format;
    int bits_per_raw_sample{};

    [[nodiscard]] float aspect() const noexcept {
        return height > 0U
                   ? static_cast<float>(width) / static_cast<float>(height)
                   : 16.0F / 9.0F;
    }

    bool operator==(const MediaProbe &) const = default;
};

// Result of negotiating and decoding the first video frame on this machine.
// HardwareAccelerated means the dedicated video decoder accepted the stream;
// SoftwareOnly means FFmpeg could decode it only through the CPU fallback.
enum class MediaDecodeSupport : core::u8 {
    NotApplicable,
    HardwareAccelerated,
    SoftwareOnly,
    Unavailable,
};

// Performs real decoder negotiation rather than guessing from codec/profile
// names. It reads only far enough to receive the first frame and does not do
// the expensive YUV-to-RGBA conversion used by playback.
[[nodiscard]] MediaDecodeSupport
probe_media_decode_support(const std::string &source);

// Opens a video at most once. This can still involve filesystem I/O and FFmpeg
// stream discovery, so interactive callers should run it away from their UI
// thread. Still images only read enough bytes to discover their dimensions.
[[nodiscard]] MediaProbe probe_media(const std::string &source);

[[nodiscard]] std::optional<MediaFrameRate>
probe_media_frame_rate(const std::string &source);

// The width/height aspect ratio of a media file, so its quad can be sized to
// match rather than assumed 16:9. Reads only the header (no full decode).
// Returns 16/9 on any failure.
[[nodiscard]] float probe_media_aspect(const std::string &source);

} // namespace mediaserver::render
