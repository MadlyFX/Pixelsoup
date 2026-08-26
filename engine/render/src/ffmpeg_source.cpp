#include <mediaserver/render/media_source.hpp>

// FFmpeg is a C library; its headers are included through extern "C" and with
// warnings silenced, as they are not clean under this project's warnings-as-
// errors. Everything below stays fully checked.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
// stb_image, declarations only - the implementation is compiled in
// media_source.cpp. Used here to read an image's dimensions for the aspect probe.
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace mediaserver::render {
namespace {

[[nodiscard]] bool ends_with(std::string_view text, std::string_view suffix) {
    if (suffix.size() > text.size()) {
        return false;
    }
    const auto lower = [](char c) {
        return static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    };
    for (std::size_t i = 0U; i < suffix.size(); ++i) {
        if (lower(text[text.size() - suffix.size() + i]) != lower(suffix[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_video_path(std::string_view source) {
    return ends_with(source, ".mp4") || ends_with(source, ".mov")
        || ends_with(source, ".mkv") || ends_with(source, ".avi")
        || ends_with(source, ".webm") || ends_with(source, ".m4v")
        || ends_with(source, ".mpg") || ends_with(source, ".mpeg")
        || ends_with(source, ".wmv") || ends_with(source, ".mxf");
}

// One open video file: the demux/decode/scale contexts and where the decoder is
// positioned. Reused across frames so sequential playback does not reopen.
struct Decoder final {
    AVFormatContext* format{};
    AVCodecContext* codec{};
    SwsContext* scaler{};
    AVPacket* packet{};
    AVFrame* frame{};
    int stream{-1};
    double fps{25.0};
    core::i64 start_pts{};
    // The next frame index a plain decode will produce, or -1 when unknown (just
    // opened or seeked), so a matching request avoids a seek.
    core::i64 next_index{-1};

    ~Decoder() {
        if (scaler != nullptr) {
            sws_freeContext(scaler);
        }
        if (frame != nullptr) {
            av_frame_free(&frame);
        }
        if (packet != nullptr) {
            av_packet_free(&packet);
        }
        if (codec != nullptr) {
            avcodec_free_context(&codec);
        }
        if (format != nullptr) {
            avformat_close_input(&format);
        }
    }
};

[[nodiscard]] std::optional<MediaFrameRate>
stream_frame_rate(const AVStream* stream) noexcept {
    const auto accepted = [](AVRational rate)
        -> std::optional<MediaFrameRate> {
        if (rate.num <= 0 || rate.den <= 0) {
            return std::nullopt;
        }
        core::u32 numerator = static_cast<core::u32>(rate.num);
        core::u32 denominator = static_cast<core::u32>(rate.den);
        const core::u32 divisor = std::gcd(numerator, denominator);
        numerator /= divisor;
        denominator /= divisor;
        return MediaFrameRate{numerator, denominator};
    };
    if (const auto average = accepted(stream->avg_frame_rate)) {
        return average;
    }
    return accepted(stream->r_frame_rate);
}

[[nodiscard]] double stream_fps(const AVStream* stream) {
    const auto rate = stream_frame_rate(stream);
    return rate.has_value()
        ? static_cast<double>(rate->numerator)
            / static_cast<double>(rate->denominator)
        : 25.0;
}

[[nodiscard]] std::unique_ptr<Decoder> open_decoder(const std::string& source) {
    auto decoder = std::make_unique<Decoder>();
    if (avformat_open_input(&decoder->format, source.c_str(), nullptr, nullptr)
        != 0) {
        return nullptr;
    }
    if (avformat_find_stream_info(decoder->format, nullptr) < 0) {
        return nullptr;
    }
    const AVCodec* codec = nullptr;
    decoder->stream = av_find_best_stream(
        decoder->format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (decoder->stream < 0 || codec == nullptr) {
        return nullptr;
    }
    const AVStream* const stream = decoder->format->streams[decoder->stream];
    decoder->codec = avcodec_alloc_context3(codec);
    if (decoder->codec == nullptr) {
        return nullptr;
    }
    if (avcodec_parameters_to_context(decoder->codec, stream->codecpar) < 0) {
        return nullptr;
    }
    // FFmpeg's automatic setting can consume every logical core per decoder.
    // A show commonly has more than one active source, so bound codec-internal
    // software parallelism while retaining frame/slice threading where the
    // selected decoder supports it. The process-wide async scheduler below
    // runs at most two source decodes concurrently.
    const unsigned hardware_threads = std::max(1U, std::thread::hardware_concurrency());
    decoder->codec->thread_count = static_cast<int>(
        std::clamp(hardware_threads / 2U, 1U, 8U));
    decoder->codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (avcodec_open2(decoder->codec, codec, nullptr) < 0) {
        return nullptr;
    }
    decoder->packet = av_packet_alloc();
    decoder->frame = av_frame_alloc();
    if (decoder->packet == nullptr || decoder->frame == nullptr) {
        return nullptr;
    }
    decoder->fps = stream_fps(stream);
    decoder->start_pts =
        stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
    return decoder;
}

// The frame ordinal a decoded frame's timestamp corresponds to.
[[nodiscard]] core::i64 index_of(const Decoder& decoder, core::i64 pts) {
    const AVStream* const stream = decoder.format->streams[decoder.stream];
    const double seconds =
        static_cast<double>(pts - decoder.start_pts) * av_q2d(stream->time_base);
    const auto index = static_cast<core::i64>(std::llround(seconds * decoder.fps));
    return index < 0 ? 0 : index;
}

// Reads and decodes until a frame at or after `target` is produced, converting
// it to RGBA. Returns an invalid frame at end of stream with nothing decoded.
[[nodiscard]] DecodedFrame decode_until(Decoder& decoder, core::i64 target) {
    bool packet_pending = false;
    bool drain_pending = false;
    bool draining = false;
    while (true) {
        // Always drain every currently available decoded frame before sending
        // more input. In particular, avcodec_send_packet(EAGAIN) means the
        // *same* packet must be retained while receive_frame is drained; losing
        // it here causes intermittent frame skips with frame-threaded codecs.
        const int got = avcodec_receive_frame(decoder.codec, decoder.frame);
        if (got == 0) {
            const auto pts = decoder.frame->best_effort_timestamp
                    == AV_NOPTS_VALUE
                ? decoder.frame->pts
                : decoder.frame->best_effort_timestamp;
            const core::i64 index = pts == AV_NOPTS_VALUE
                ? (decoder.next_index >= 0 ? decoder.next_index : target)
                : index_of(decoder, pts);
            if (index < target) {
                decoder.next_index = index + 1;
                av_frame_unref(decoder.frame);
                continue;
            }
            if (index > target) {
                // Frame-exact keys may never contain a plausible later frame.
                // An imprecise/failed timestamp seek is reported as a miss so
                // the cache holds its last ready texture instead of mislabeling
                // pixels under the requested ordinal.
                decoder.next_index = index + 1;
                av_frame_unref(decoder.frame);
                return DecodedFrame{};
            }

            const int width = decoder.frame->width;
            const int height = decoder.frame->height;
            if (width <= 0 || height <= 0) {
                av_frame_unref(decoder.frame);
                return DecodedFrame{};
            }
            decoder.scaler = sws_getCachedContext(
                decoder.scaler,
                width,
                height,
                static_cast<AVPixelFormat>(decoder.frame->format),
                width,
                height,
                AV_PIX_FMT_RGBA,
                SWS_BILINEAR,
                nullptr,
                nullptr,
                nullptr);
            if (decoder.scaler == nullptr) {
                av_frame_unref(decoder.frame);
                return DecodedFrame{};
            }

            DecodedFrame out{};
            out.width = static_cast<core::u32>(width);
            out.height = static_cast<core::u32>(height);
            out.format = rhi::TextureFormat::Rgba8Unorm;
            out.pixels.resize(
                static_cast<std::size_t>(width) * static_cast<std::size_t>(height)
                * 4U);
            std::uint8_t* const dst[4]{out.pixels.data(), nullptr, nullptr, nullptr};
            const int stride[4]{width * 4, 0, 0, 0};
            sws_scale(
                decoder.scaler,
                decoder.frame->data,
                decoder.frame->linesize,
                0,
                height,
                dst,
                stride);
            decoder.next_index = index + 1;
            av_frame_unref(decoder.frame);
            return out;
        }
        if (got == AVERROR_EOF) {
            if (packet_pending) {
                av_packet_unref(decoder.packet);
            }
            return DecodedFrame{};
        }
        if (got != AVERROR(EAGAIN)) {
            if (packet_pending) {
                av_packet_unref(decoder.packet);
            }
            return DecodedFrame{};
        }

        if (packet_pending) {
            const int sent = avcodec_send_packet(decoder.codec, decoder.packet);
            if (sent == AVERROR(EAGAIN)) {
                // FFmpeg promises that send-EAGAIN and receive-EAGAIN cannot
                // occur simultaneously. Refuse the broken state rather than
                // spin forever; importantly, the packet was not silently lost.
                av_packet_unref(decoder.packet);
                return DecodedFrame{};
            }
            av_packet_unref(decoder.packet);
            packet_pending = false;
            if (sent < 0) {
                continue;
            }
            continue;
        }

        if (drain_pending) {
            const int sent = avcodec_send_packet(decoder.codec, nullptr);
            if (sent == AVERROR(EAGAIN)) {
                continue;
            }
            drain_pending = false;
            if (sent < 0) {
                return DecodedFrame{};
            }
            draining = true;
            continue;
        }
        if (draining) {
            // A drained decoder should report AVERROR_EOF. Treat a repeated
            // request for input as end-of-stream rather than busy-looping.
            return DecodedFrame{};
        }

        while (true) {
            const int read = av_read_frame(decoder.format, decoder.packet);
            if (read < 0) {
                drain_pending = true;
                break;
            }
            if (decoder.packet->stream_index != decoder.stream) {
                av_packet_unref(decoder.packet);
                continue;
            }
            packet_pending = true;
            break;
        }
    }
}

// A video decoder behind the MediaSource seam. Holds an open decoder per source
// and walks frames, seeking only when the request is not the next frame.
class FfmpegSource final : public MediaSource {
public:
    [[nodiscard]] std::string key(
        const std::string& source, core::u32 frame_index) const override {
        return source + "#f" + std::to_string(frame_index);
    }

    [[nodiscard]] DecodedFrame decode(
        const std::string& source, core::u32 frame_index) override {
        auto found = decoders_.find(source);
        if (found == decoders_.end()) {
            auto decoder = open_decoder(source);
            if (decoder == nullptr) {
                return DecodedFrame{};
            }
            found = decoders_.emplace(source, std::move(decoder)).first;
        }
        Decoder& decoder = *found->second;
        const auto target = static_cast<core::i64>(frame_index);

        // Small forward gaps are cheaper and more stable when decoded and
        // discarded. Seeking H.26x walks back to a keyframe and flushes all
        // codec state; doing that whenever the show skips a frame creates a
        // feedback loop once playback falls slightly behind. Large jumps and
        // reverse scrubs still seek so random access stays bounded.
        constexpr core::i64 max_forward_decode_gap = 12;
        const bool sequential_or_small_forward_gap =
            decoder.next_index >= 0 && target >= decoder.next_index
            && target - decoder.next_index <= max_forward_decode_gap;
        if (decoder.next_index != target && !sequential_or_small_forward_gap) {
            const AVStream* const stream =
                decoder.format->streams[decoder.stream];
            const double seconds =
                static_cast<double>(target) / std::max(decoder.fps, 0.001);
            const auto ts = decoder.start_pts
                + static_cast<core::i64>(seconds / av_q2d(stream->time_base));
            if (av_seek_frame(
                    decoder.format, decoder.stream, ts, AVSEEK_FLAG_BACKWARD)
                < 0) {
                return DecodedFrame{};
            }
            avcodec_flush_buffers(decoder.codec);
            decoder.next_index = -1;
        }
        return decode_until(decoder, target);
    }

private:
    std::map<std::string, std::unique_ptr<Decoder>> decoders_;
};

// Normalize once when a renderer first sees a source. This deliberately avoids
// weakly_canonical/canonical: those may stat a network-backed media path and
// block the render thread. Absolute lexical normalization touches no source
// file; on Windows, case folding also merges ordinary spelling differences.
[[nodiscard]] std::string canonical_source_key(const std::string& source) {
    std::error_code error;
    auto path = std::filesystem::absolute(std::filesystem::path{source}, error);
    std::string key = error ? source : path.lexically_normal().string();
#if defined(_WIN32)
    std::transform(key.begin(), key.end(), key.begin(), [](char value) {
        return static_cast<char>(
            value >= 'A' && value <= 'Z' ? value - 'A' + 'a' : value);
    });
#endif
    return key;
}

struct DecodeRequest final {
    core::u32 frame{};
    bool urgent{};
};

// Decoder state is serial per source, while work is scheduled onto a tiny
// shared pool. This retains FFmpeg's efficient forward-walking state without
// creating one operating-system thread per visible clip.
struct AsyncVideoSourceState final {
    AsyncVideoSourceState(std::string key, std::string path)
        : source_key(std::move(key)), decode_path(std::move(path)),
          decoder(std::make_unique<FfmpegSource>()) {}

    std::string source_key;
    std::string decode_path;
    std::unique_ptr<FfmpegSource> decoder;
    std::deque<DecodeRequest> requests;
    std::optional<core::u32> in_flight;
    bool scheduled{};
};

struct ReadyFrame final {
    std::shared_ptr<const DecodedFrame> frame;
    std::size_t bytes{};
    core::u64 stamp{};
    std::chrono::steady_clock::time_point retry_after{};
};

class SharedVideoRegistry final {
public:
    SharedVideoRegistry() {
        constexpr std::size_t worker_count = 2U;
        workers_.reserve(worker_count);
        for (std::size_t index = 0U; index < worker_count; ++index) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    SharedVideoRegistry(const SharedVideoRegistry&) = delete;
    SharedVideoRegistry& operator=(const SharedVideoRegistry&) = delete;

    ~SharedVideoRegistry() {
        {
            const std::lock_guard lock{mutex_};
            stopping_ = true;
        }
        ready_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    [[nodiscard]] std::shared_ptr<AsyncVideoSourceState> source(
        const std::string& decode_path) {
        const std::string key = canonical_source_key(decode_path);
        const std::lock_guard lock{mutex_};
        prune_inactive_locked();
        const auto found = sources_.find(key);
        if (found != sources_.end()) {
            if (auto shared = found->second.lock()) {
                return shared;
            }
        }
        auto state =
            std::make_shared<AsyncVideoSourceState>(key, decode_path);
        sources_[key] = state;
        return state;
    }

    [[nodiscard]] std::shared_ptr<const DecodedFrame> try_frame(
        const std::shared_ptr<AsyncVideoSourceState>& source,
        core::u32 frame_index) {
        const std::lock_guard lock{mutex_};
        auto found = frames_.find(
            std::make_pair(source->source_key, frame_index));
        if (found != frames_.end() && !found->second.frame->valid()
            && std::chrono::steady_clock::now()
                >= found->second.retry_after) {
            resident_bytes_ -= found->second.bytes;
            frames_.erase(found);
            found = frames_.end();
        }
        if (found == frames_.end()) {
            enqueue_locked(source, frame_index, true);
            return nullptr;
        }
        found->second.stamp = ++stamp_;
        return found->second.frame;
    }

    void prefetch(
        const std::shared_ptr<AsyncVideoSourceState>& source,
        core::u32 frame_index) {
        const std::lock_guard lock{mutex_};
        if (frames_.find(std::make_pair(source->source_key, frame_index))
            == frames_.end()) {
            enqueue_locked(source, frame_index, false);
        }
    }

    [[nodiscard]] MediaDecodeStats stats() {
        const std::lock_guard lock{mutex_};
        prune_inactive_locked();
        MediaDecodeStats result{};
        for (const auto& [key, weak] : sources_) {
            static_cast<void>(key);
            if (const auto source = weak.lock()) {
                ++result.active_sources;
                result.pending_requests += source->requests.size();
                if (source->in_flight.has_value()) {
                    ++result.pending_requests;
                }
            }
        }
        result.ready_frames = frames_.size();
        result.ready_bytes = resident_bytes_;
        result.byte_budget = max_ready_bytes;
        result.completed_frames = completed_frames_;
        result.failed_frames = failed_frames_;
        return result;
    }

private:
    using FrameKey = std::pair<std::string, core::u32>;

    static constexpr std::size_t max_requests_per_source = 3U;
    static constexpr std::size_t max_ready_frames = 16U;
    static constexpr std::size_t max_ready_bytes = 256U * 1024U * 1024U;

    void enqueue_locked(
        const std::shared_ptr<AsyncVideoSourceState>& source,
        core::u32 frame_index,
        bool urgent) {
        if (source->in_flight == frame_index) {
            return;
        }
        auto queued = std::find_if(
            source->requests.begin(), source->requests.end(),
            [frame_index](const DecodeRequest& request) {
                return request.frame == frame_index;
            });
        if (queued != source->requests.end()) {
            if (!urgent || queued->urgent) {
                return;
            }
            // Upgrade a look-ahead request to the current-frame request and
            // drop stale queued work. The in-flight decode remains untouched.
            source->requests.clear();
            source->requests.push_front(DecodeRequest{frame_index, true});
        } else if (urgent) {
            // Latest current-frame demand wins. Holding the last uploaded frame
            // is preferable to decoding a backlog the playhead has passed.
            source->requests.clear();
            source->requests.push_front(DecodeRequest{frame_index, true});
        } else if (source->requests.size() < max_requests_per_source) {
            source->requests.push_back(DecodeRequest{frame_index, false});
        } else {
            return;
        }

        if (!source->scheduled) {
            source->scheduled = true;
            work_.push_back(source);
            ready_.notify_one();
        }
    }

    void publish_locked(
        const std::shared_ptr<AsyncVideoSourceState>& source,
        core::u32 frame_index,
        std::shared_ptr<const DecodedFrame> frame) {
        const FrameKey key{source->source_key, frame_index};
        const std::size_t bytes = frame->valid() ? frame->pixels.size() : 0U;
        const auto old = frames_.find(key);
        if (old != frames_.end()) {
            resident_bytes_ -= old->second.bytes;
            frames_.erase(old);
        }
        frames_.emplace(
            key,
            ReadyFrame{
                std::move(frame), bytes, ++stamp_,
                bytes == 0U
                    ? std::chrono::steady_clock::now()
                        + std::chrono::seconds{1}
                    : std::chrono::steady_clock::time_point::max()});
        resident_bytes_ += bytes;

        // Keep the newly decoded frame even when one unusually large image is
        // itself above budget. Otherwise evict globally least-recently-used
        // frames until both the byte and small-frame guards are satisfied.
        while ((resident_bytes_ > max_ready_bytes
                || frames_.size() > max_ready_frames)
               && frames_.size() > 1U) {
            auto oldest = frames_.end();
            for (auto candidate = frames_.begin(); candidate != frames_.end();
                 ++candidate) {
                if (candidate->first == key) {
                    continue;
                }
                if (oldest == frames_.end()
                    || candidate->second.stamp < oldest->second.stamp) {
                    oldest = candidate;
                }
            }
            if (oldest == frames_.end()) {
                break;
            }
            resident_bytes_ -= oldest->second.bytes;
            frames_.erase(oldest);
        }
    }

    void prune_inactive_locked() {
        for (auto source = sources_.begin(); source != sources_.end();) {
            if (!source->second.expired()) {
                ++source;
                continue;
            }
            const std::string dead_key = source->first;
            source = sources_.erase(source);
            for (auto frame = frames_.begin(); frame != frames_.end();) {
                if (frame->first.first == dead_key) {
                    resident_bytes_ -= frame->second.bytes;
                    frame = frames_.erase(frame);
                } else {
                    ++frame;
                }
            }
        }
    }

    void worker_loop() {
        while (true) {
            std::shared_ptr<AsyncVideoSourceState> source;
            core::u32 frame_index = 0U;
            {
                std::unique_lock lock{mutex_};
                ready_.wait(lock, [this] { return stopping_ || !work_.empty(); });
                if (stopping_) {
                    return;
                }
                source = std::move(work_.front());
                work_.pop_front();
                if (source->requests.empty()) {
                    source->scheduled = false;
                    continue;
                }
                frame_index = source->requests.front().frame;
                source->requests.pop_front();
                source->in_flight = frame_index;
            }

            std::shared_ptr<const DecodedFrame> decoded;
            try {
                decoded = std::make_shared<const DecodedFrame>(
                    source->decoder->decode(source->decode_path, frame_index));
            } catch (...) {
                decoded = std::make_shared<const DecodedFrame>();
            }

            {
                const std::lock_guard lock{mutex_};
                publish_locked(source, frame_index, decoded);
                ++completed_frames_;
                if (!decoded->valid()) {
                    ++failed_frames_;
                    // A replaced/repaired file must be reopened on the retry;
                    // retaining the old demuxer would permanently observe its
                    // former EOF or container state.
                    try {
                        source->decoder = std::make_unique<FfmpegSource>();
                    } catch (...) {
                        // The existing decoder remains valid enough to close;
                        // the cooldown still prevents a frame-path retry loop.
                    }
                }
                source->in_flight.reset();
                if (!source->requests.empty()) {
                    // One frame per turn keeps two active clips fair while each
                    // source's decoder remains strictly serial.
                    work_.push_back(source);
                    ready_.notify_one();
                } else {
                    source->scheduled = false;
                }
            }
            source.reset();
            {
                const std::lock_guard lock{mutex_};
                prune_inactive_locked();
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    bool stopping_{};
    std::vector<std::thread> workers_;
    std::deque<std::shared_ptr<AsyncVideoSourceState>> work_;
    std::map<std::string, std::weak_ptr<AsyncVideoSourceState>> sources_;
    std::map<FrameKey, ReadyFrame> frames_;
    std::size_t resident_bytes_{};
    core::u64 stamp_{};
    core::u64 completed_frames_{};
    core::u64 failed_frames_{};
};

[[nodiscard]] SharedVideoRegistry& video_registry() {
    static SharedVideoRegistry registry;
    return registry;
}

// Routes each source to the still-image decoder or the video decoder by
// extension, so the MediaCache above never learns which a clip is.
class DispatchMediaSource final : public MediaSource {
public:
    DispatchMediaSource() {
        // Construct the bounded scheduler alongside renderer setup, not on the
        // first frame-path request.
        static_cast<void>(video_registry());
    }

    [[nodiscard]] std::string key(
        const std::string& source, core::u32 frame_index) const override {
        return is_video_path(source) ? video_.key(source, frame_index)
                                     : image_.key(source, frame_index);
    }

    [[nodiscard]] DecodedFrame decode(
        const std::string& source, core::u32 frame_index) override {
        return is_video_path(source) ? video_.decode(source, frame_index)
                                     : image_.decode(source, frame_index);
    }

    [[nodiscard]] std::shared_ptr<const DecodedFrame> try_decode(
        const std::string& source, core::u32 frame_index) override {
        if (!is_video_path(source)) {
            return MediaSource::try_decode(source, frame_index);
        }
        return video_registry().try_frame(worker_for(source), frame_index);
    }

    void prefetch(
        const std::string& source, core::u32 frame_index) override {
        if (is_video_path(source)) {
            video_registry().prefetch(worker_for(source), frame_index);
        }
    }

private:
    struct WorkerEntry final {
        std::shared_ptr<AsyncVideoSourceState> worker;
        core::u64 stamp{};
    };

    [[nodiscard]] std::shared_ptr<AsyncVideoSourceState> worker_for(
        const std::string& source) {
        const auto found = workers_.find(source);
        if (found != workers_.end()) {
            found->second.stamp = ++worker_stamp_;
            return found->second.worker;
        }
        auto worker = video_registry().source(source);
        workers_.emplace(source, WorkerEntry{worker, ++worker_stamp_});
        constexpr std::size_t max_retained_sources = 16U;
        if (workers_.size() > max_retained_sources) {
            auto oldest = workers_.end();
            for (auto candidate = workers_.begin(); candidate != workers_.end();
                 ++candidate) {
                if (candidate->first == source) {
                    continue;
                }
                if (oldest == workers_.end()
                    || candidate->second.stamp < oldest->second.stamp) {
                    oldest = candidate;
                }
            }
            if (oldest != workers_.end()) {
                workers_.erase(oldest);
            }
        }
        return worker;
    }

    ImageSequenceSource image_;
    FfmpegSource video_;
    std::map<std::string, WorkerEntry> workers_;
    core::u64 worker_stamp_{};
};

} // namespace

std::unique_ptr<MediaSource> make_media_source() {
    return std::make_unique<DispatchMediaSource>();
}

MediaDecodeStats media_decode_stats() {
    return video_registry().stats();
}

core::u32 probe_frame_count(const std::string& source) {
    if (!is_video_path(source)) {
        return 1U;
    }
    auto decoder = open_decoder(source);
    if (decoder == nullptr) {
        return 1U;
    }
    const AVStream* const stream = decoder->format->streams[decoder->stream];
    if (stream->nb_frames > 0) {
        return static_cast<core::u32>(stream->nb_frames);
    }
    // No frame count in the container: estimate from duration and rate.
    const double seconds = stream->duration != AV_NOPTS_VALUE
        ? static_cast<double>(stream->duration) * av_q2d(stream->time_base)
        : (decoder->format->duration != AV_NOPTS_VALUE
               ? static_cast<double>(decoder->format->duration) / AV_TIME_BASE
               : 0.0);
    const auto frames = static_cast<core::i64>(std::llround(seconds * decoder->fps));
    return frames > 0 ? static_cast<core::u32>(frames) : 1U;
}

std::optional<MediaFrameRate>
probe_media_frame_rate(const std::string& source) {
    if (!is_video_path(source)) {
        return std::nullopt;
    }
    auto decoder = open_decoder(source);
    if (decoder == nullptr) {
        return std::nullopt;
    }
    return stream_frame_rate(
        decoder->format->streams[decoder->stream]);
}

float probe_media_aspect(const std::string& source) {
    constexpr float default_aspect = 16.0F / 9.0F;
    if (is_video_path(source)) {
        auto decoder = open_decoder(source);
        if (decoder != nullptr && decoder->codec->height > 0) {
            return static_cast<float>(decoder->codec->width)
                / static_cast<float>(decoder->codec->height);
        }
        return default_aspect;
    }
    const auto path = ImageSequenceSource::resolve_path(source, 0U);
    int width = 0;
    int height = 0;
    int channels = 0;
    if (stbi_info(path.c_str(), &width, &height, &channels) != 0 && height > 0) {
        return static_cast<float>(width) / static_cast<float>(height);
    }
    return default_aspect;
}

} // namespace mediaserver::render
