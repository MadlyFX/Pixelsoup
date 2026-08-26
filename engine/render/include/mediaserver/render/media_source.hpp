#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/rhi/device.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The decode seam: a clip's (source, frame_index) turned into pixels.
//
// This is the boundary §6 of the plan draws between the renderer and codecs.
// Every implementation is *frame-exact* - the same (source, frame_index) yields
// the same pixels, and any frame is reachable directly, never "seek to the
// nearest keyframe" - and pure with respect to that pair, so two nodes decoding
// the same frame get the same bytes. Image sequences are the first
// implementation; HAP, ProRes/DNxHR via FFmpeg, and H.26x via NVDEC slot in
// behind this same call, so nothing above the renderer learns which codec a clip
// is. The MediaCache owns the GPU residency; a source only produces CPU pixels.
namespace mediaserver::render {

// One decoded frame in CPU memory, ready to upload. An invalid frame (no pixels)
// is how a source reports a frame it could not produce; the caller draws nothing
// rather than a failed frame.
struct DecodedFrame final {
    core::u32 width{};
    core::u32 height{};
    rhi::TextureFormat format{rhi::TextureFormat::Rgba8Unorm};
    std::vector<core::u8> pixels;

    [[nodiscard]] bool valid() const noexcept {
        return width > 0U && height > 0U && !pixels.empty();
    }
};

class MediaSource {
public:
    MediaSource() = default;
    MediaSource(const MediaSource&) = delete;
    MediaSource& operator=(const MediaSource&) = delete;
    MediaSource(MediaSource&&) = delete;
    MediaSource& operator=(MediaSource&&) = delete;
    virtual ~MediaSource() = default;

    // A stable key identifying (source, frame_index), used to cache the decoded
    // frame. Pure: no file access, no GPU. Empty means the pair names nothing.
    [[nodiscard]] virtual std::string key(
        const std::string& source, core::u32 frame_index) const = 0;

    // Decodes the frame to CPU pixels. Returns an invalid frame on any failure
    // (missing file, undecodable data), which the renderer draws as nothing.
    // Non-const because a video source keeps an open decoder to walk frames
    // efficiently rather than re-opening the file each time.
    [[nodiscard]] virtual DecodedFrame decode(
        const std::string& source, core::u32 frame_index) = 0;

    // Non-blocking frame-path access. Synchronous sources inherit an adapter
    // that decodes immediately. Video sources override this to enqueue work;
    // null then means "pending", while a non-null invalid frame means decode
    // completed without a usable image. The immutable shared frame lets every
    // in-process renderer consume one CPU decode without copying its pixels.
    [[nodiscard]] virtual std::shared_ptr<const DecodedFrame> try_decode(
        const std::string& source, core::u32 frame_index);

    // A bounded look-ahead hint. Synchronous sources ignore it, and an
    // asynchronous implementation must never perform I/O on the caller.
    virtual void prefetch(const std::string& source, core::u32 frame_index);
};

// A numbered still-image sequence decoded with stb_image. The source is a path
// template with a run of '#' marking the zero-padded frame number
// (plate_####.png); a template with no '#' is a single still, held for every
// frame. This is the spike-grade path §6 stands up first.
class ImageSequenceSource final : public MediaSource {
public:
    [[nodiscard]] std::string key(
        const std::string& source, core::u32 frame_index) const override;

    [[nodiscard]] DecodedFrame decode(
        const std::string& source, core::u32 frame_index) override;

    // Substitutes the zero-padded frame number into a '#'-run template. Pure and
    // testable with no file or GPU. Deliberately not a printf format, so a
    // document-supplied string cannot be a format-string attack.
    [[nodiscard]] static std::string resolve_path(
        const std::string& source, core::u32 frame_index);
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
    std::size_t pending_requests{};
    std::size_t ready_frames{};
    std::size_t ready_bytes{};
    std::size_t byte_budget{};
    core::u64 completed_frames{};
    core::u64 failed_frames{};

    bool operator==(const MediaDecodeStats&) const = default;
};

[[nodiscard]] MediaDecodeStats media_decode_stats();

// The number of frames a media file holds, for a clip's loop length: a video's
// frame count, or 1 for a still image (and on any failure). Opens and probes
// the file, so it is called once when a clip is added, not per frame.
[[nodiscard]] core::u32 probe_frame_count(const std::string& source);

// A video's exact native frame rate, reduced to a positive rational. Still
// images and image sequences have no intrinsic playback rate, and failures are
// reported as absence so their existing one-source-frame-per-show-frame
// behaviour remains available to the scene layer.
struct MediaFrameRate final {
    core::u32 numerator{};
    core::u32 denominator{};

    bool operator==(const MediaFrameRate&) const = default;
};

[[nodiscard]] std::optional<MediaFrameRate>
probe_media_frame_rate(const std::string& source);

// The width/height aspect ratio of a media file, so its quad can be sized to
// match rather than assumed 16:9. Reads only the header (no full decode).
// Returns 16/9 on any failure.
[[nodiscard]] float probe_media_aspect(const std::string& source);

} // namespace mediaserver::render
