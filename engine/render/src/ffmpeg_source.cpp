#include <mediaserver/render/media_source.hpp>

#include "ffmpeg_source_test_support.hpp"

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
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
// stb_image, declarations only - the implementation is compiled in
// media_source.cpp. Used here to read an image's dimensions for the aspect
// probe.
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
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
    return ends_with(source, ".mp4") || ends_with(source, ".mov") ||
           ends_with(source, ".mkv") || ends_with(source, ".avi") ||
           ends_with(source, ".webm") || ends_with(source, ".m4v") ||
           ends_with(source, ".mpg") || ends_with(source, ".mpeg") ||
           ends_with(source, ".wmv") || ends_with(source, ".mxf");
}

// One open video file: the demux/decode/scale contexts and where the decoder is
// positioned. Reused across frames so sequential playback does not reopen.
struct Decoder final {
    AVFormatContext *format{};
    AVCodecContext *codec{};
    SwsContext *scaler{};
    AVPacket *packet{};
    AVFrame *frame{};
    AVFrame *transferred_frame{};
    AVPixelFormat hardware_format{AV_PIX_FMT_NONE};
    bool hardware_active{};
    int stream{-1};
    double fps{25.0};
    core::i64 start_pts{};
    // The next frame index a plain decode will produce, or -1 when unknown
    // (just opened or seeked), so a matching request avoids a seek.
    core::i64 next_index{-1};

    ~Decoder() {
        if (scaler != nullptr) {
            sws_freeContext(scaler);
        }
        if (frame != nullptr) {
            av_frame_free(&frame);
        }
        if (transferred_frame != nullptr) {
            av_frame_free(&transferred_frame);
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

// One D3D11 video device is shared by every decoder. Creating one device per
// plate costs memory, duplicates driver scheduling state, and prevents the
// driver from seeing the nine streams as one decode workload.
class SharedHardwareDevice final {
  public:
    SharedHardwareDevice() {
#ifdef _WIN32
        static_cast<void>(av_hwdevice_ctx_create(
            &context_, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0));
#endif
    }

    SharedHardwareDevice(const SharedHardwareDevice &) = delete;
    SharedHardwareDevice &operator=(const SharedHardwareDevice &) = delete;

    ~SharedHardwareDevice() { av_buffer_unref(&context_); }

    [[nodiscard]] AVBufferRef *reference() const noexcept {
        return context_ == nullptr ? nullptr : av_buffer_ref(context_);
    }

  private:
    AVBufferRef *context_{};
};

[[nodiscard]] SharedHardwareDevice &shared_hardware_device() {
    static SharedHardwareDevice device;
    return device;
}

[[nodiscard]] AVPixelFormat
select_decoder_format(AVCodecContext *context,
                      const AVPixelFormat *formats) noexcept {
    auto *const decoder = static_cast<Decoder *>(context->opaque);
    const AVPixelFormat *candidate = formats;
    while (*candidate != AV_PIX_FMT_NONE) {
        if (decoder != nullptr && *candidate == decoder->hardware_format) {
            decoder->hardware_active = true;
            return *candidate;
        }
        ++candidate;
    }
    // FFmpeg supplies software formats after hardware formats. Returning the
    // first one keeps unsupported profiles playable through the old path.
    decoder->hardware_active = false;
    return formats[0];
}

[[nodiscard]] bool configure_hardware_decoder(Decoder &decoder,
                                              const AVCodec &codec) {
#ifdef _WIN32
    for (int index = 0;; ++index) {
        const AVCodecHWConfig *const config =
            avcodec_get_hw_config(&codec, index);
        if (config == nullptr) {
            return false;
        }
        if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) == 0 ||
            config->device_type != AV_HWDEVICE_TYPE_D3D11VA) {
            continue;
        }
        AVBufferRef *const device = shared_hardware_device().reference();
        if (device == nullptr) {
            return false;
        }
        decoder.hardware_format = config->pix_fmt;
        decoder.codec->opaque = &decoder;
        decoder.codec->get_format = &select_decoder_format;
        decoder.codec->hw_device_ctx = device;
        return true;
    }
#else
    static_cast<void>(decoder);
    static_cast<void>(codec);
    return false;
#endif
}

[[nodiscard]] std::optional<MediaFrameRate>
stream_frame_rate(const AVStream *stream) noexcept {
    const auto accepted = [](AVRational rate) -> std::optional<MediaFrameRate> {
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

[[nodiscard]] double stream_fps(const AVStream *stream) {
    const auto rate = stream_frame_rate(stream);
    return rate.has_value() ? static_cast<double>(rate->numerator) /
                                  static_cast<double>(rate->denominator)
                            : 25.0;
}

[[nodiscard]] std::unique_ptr<Decoder> open_decoder(const std::string &source) {
    auto decoder = std::make_unique<Decoder>();
    if (avformat_open_input(&decoder->format, source.c_str(), nullptr,
                            nullptr) != 0) {
        return nullptr;
    }
    if (avformat_find_stream_info(decoder->format, nullptr) < 0) {
        return nullptr;
    }
    const AVCodec *codec = nullptr;
    decoder->stream = av_find_best_stream(decoder->format, AVMEDIA_TYPE_VIDEO,
                                          -1, -1, &codec, 0);
    if (decoder->stream < 0 || codec == nullptr) {
        return nullptr;
    }
    const AVStream *const stream = decoder->format->streams[decoder->stream];
    const auto allocate_context = [&]() {
        decoder->codec = avcodec_alloc_context3(codec);
        return decoder->codec != nullptr &&
               avcodec_parameters_to_context(decoder->codec,
                                             stream->codecpar) >= 0;
    };
    if (!allocate_context()) {
        return nullptr;
    }
    // FFmpeg's automatic setting can consume every logical core per decoder.
    // A show commonly has more than one active source, so bound codec-internal
    // software parallelism while retaining frame/slice threading where the
    // selected decoder supports it. The process-wide async scheduler below
    // limits unknown/software streams to two concurrent jobs while allowing
    // confirmed hardware streams to use its additional bounded lanes.
    const bool hardware_configured =
        configure_hardware_decoder(*decoder, *codec);
    const auto configure_software_threads = [&]() {
        const unsigned hardware_threads =
            std::max(1U, std::thread::hardware_concurrency());
        decoder->codec->thread_count =
            static_cast<int>(std::clamp(hardware_threads / 4U, 1U, 2U));
        decoder->codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    };
    // Hardware backends ignore software frame threads. Configure them anyway
    // so a profile declined by get_format (for example H.264 4:2:2 10-bit on
    // NVDEC generations that only accept H.264 4:2:0 8-bit) retains the fully
    // threaded software fallback negotiated during the same avcodec_open2.
    configure_software_threads();
    if (avcodec_open2(decoder->codec, codec, nullptr) < 0) {
        if (!hardware_configured) {
            return nullptr;
        }
        // A decoder advertised D3D11VA but could not initialize it on this
        // driver. Recreate the context cleanly and retain software playback.
        avcodec_free_context(&decoder->codec);
        decoder->hardware_format = AV_PIX_FMT_NONE;
        decoder->hardware_active = false;
        if (!allocate_context()) {
            return nullptr;
        }
        configure_software_threads();
        if (avcodec_open2(decoder->codec, codec, nullptr) < 0) {
            return nullptr;
        }
    }
    decoder->packet = av_packet_alloc();
    decoder->frame = av_frame_alloc();
    decoder->transferred_frame = av_frame_alloc();
    if (decoder->packet == nullptr || decoder->frame == nullptr ||
        decoder->transferred_frame == nullptr) {
        return nullptr;
    }
    decoder->fps = stream_fps(stream);
    decoder->start_pts =
        stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
    return decoder;
}

// The frame ordinal a decoded frame's timestamp corresponds to.
[[nodiscard]] core::i64 index_of(const Decoder &decoder, core::i64 pts) {
    const AVStream *const stream = decoder.format->streams[decoder.stream];
    const double seconds = static_cast<double>(pts - decoder.start_pts) *
                           av_q2d(stream->time_base);
    const auto index =
        static_cast<core::i64>(std::llround(seconds * decoder.fps));
    return index < 0 ? 0 : index;
}

// Reads and decodes until a frame at or after `target` is produced, converting
// it to RGBA. Returns an invalid frame at end of stream with nothing decoded.
[[nodiscard]] DecodedFrame decode_until(Decoder &decoder, core::i64 target) {
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
            const auto pts =
                decoder.frame->best_effort_timestamp == AV_NOPTS_VALUE
                    ? decoder.frame->pts
                    : decoder.frame->best_effort_timestamp;
            const core::i64 index =
                pts == AV_NOPTS_VALUE
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

            AVFrame *source_frame = decoder.frame;
            if (decoder.hardware_active &&
                decoder.frame->format == decoder.hardware_format) {
                av_frame_unref(decoder.transferred_frame);
                if (av_hwframe_transfer_data(decoder.transferred_frame,
                                             decoder.frame, 0) < 0) {
                    av_frame_unref(decoder.frame);
                    return DecodedFrame{};
                }
                source_frame = decoder.transferred_frame;
            }
            const int width = source_frame->width;
            const int height = source_frame->height;
            if (width <= 0 || height <= 0) {
                av_frame_unref(decoder.transferred_frame);
                av_frame_unref(decoder.frame);
                return DecodedFrame{};
            }
#if defined(_WIN32) && defined(MEDIASERVER_HAS_DXIL_SHADERS)
            const AVColorSpace color_space =
                source_frame->colorspace != AVCOL_SPC_UNSPECIFIED
                    ? source_frame->colorspace
                    : decoder.codec->colorspace;
            const AVColorRange color_range =
                source_frame->color_range != AVCOL_RANGE_UNSPECIFIED
                    ? source_frame->color_range
                    : decoder.codec->color_range;
            const bool rec709 = color_space == AVCOL_SPC_BT709;
            const bool limited = color_range == AVCOL_RANGE_MPEG;
            const bool full = color_range == AVCOL_RANGE_JPEG;
            if (decoder.hardware_active &&
                source_frame->format == AV_PIX_FMT_P010LE && rec709 &&
                (limited || full) && width % 2 == 0 && height % 2 == 0 &&
                source_frame->linesize[0] >= width * 2 &&
                source_frame->linesize[1] >= width * 2) {
                const std::size_t row_bytes =
                    static_cast<std::size_t>(width) * 2U;
                const std::size_t luma_bytes =
                    row_bytes * static_cast<std::size_t>(height);
                const std::size_t chroma_bytes =
                    row_bytes * static_cast<std::size_t>(height / 2);
                DecodedFrame out{};
                out.width = static_cast<core::u32>(width);
                out.height = static_cast<core::u32>(height);
                out.format = rhi::TextureFormat::Rgba8Unorm;
                out.layout = limited ? DecodedPixelLayout::P010Rec709Limited
                                     : DecodedPixelLayout::P010Rec709Full;
                out.source_frame = static_cast<core::u32>(index);
                out.pixels.resize(luma_bytes + chroma_bytes);
                for (int row = 0; row < height; ++row) {
                    std::copy_n(source_frame->data[0] +
                                    static_cast<std::ptrdiff_t>(row) *
                                        source_frame->linesize[0],
                                row_bytes,
                                out.pixels.data() +
                                    static_cast<std::size_t>(row) * row_bytes);
                }
                for (int row = 0; row < height / 2; ++row) {
                    std::copy_n(source_frame->data[1] +
                                    static_cast<std::ptrdiff_t>(row) *
                                        source_frame->linesize[1],
                                row_bytes,
                                out.pixels.data() + luma_bytes +
                                    static_cast<std::size_t>(row) * row_bytes);
                }
                decoder.next_index = index + 1;
                av_frame_unref(decoder.transferred_frame);
                av_frame_unref(decoder.frame);
                return out;
            }
#endif
            if (decoder.scaler == nullptr) {
                decoder.scaler = sws_alloc_context();
                if (decoder.scaler == nullptr) {
                    av_frame_unref(decoder.transferred_frame);
                    av_frame_unref(decoder.frame);
                    return DecodedFrame{};
                }
                // libswscale otherwise converts an entire UHD frame on one CPU
                // core even when NVDEC supplied the YUV planes. Two slice
                // workers keep each plate's conversion bounded and concurrent.
                const AVPixelFormat source_format =
                    static_cast<AVPixelFormat>(source_frame->format);
                const bool configured =
                    av_opt_set_int(decoder.scaler, "srcw", width, 0) >= 0 &&
                    av_opt_set_int(decoder.scaler, "srch", height, 0) >= 0 &&
                    av_opt_set_int(decoder.scaler, "src_format", source_format,
                                   0) >= 0 &&
                    av_opt_set_int(decoder.scaler, "dstw", width, 0) >= 0 &&
                    av_opt_set_int(decoder.scaler, "dsth", height, 0) >= 0 &&
                    av_opt_set_int(decoder.scaler, "dst_format",
                                   AV_PIX_FMT_RGBA, 0) >= 0 &&
                    av_opt_set_int(decoder.scaler, "sws_flags", SWS_BILINEAR,
                                   0) >= 0 &&
                    av_opt_set_int(decoder.scaler, "threads", 2, 0) >= 0;
                if (!configured ||
                    sws_init_context(decoder.scaler, nullptr, nullptr) < 0) {
                    sws_freeContext(decoder.scaler);
                    decoder.scaler = nullptr;
                    av_frame_unref(decoder.transferred_frame);
                    av_frame_unref(decoder.frame);
                    return DecodedFrame{};
                }
            }

            DecodedFrame out{};
            out.width = static_cast<core::u32>(width);
            out.height = static_cast<core::u32>(height);
            out.format = rhi::TextureFormat::Rgba8Unorm;
            out.source_frame = static_cast<core::u32>(index);
            out.pixels.resize(static_cast<std::size_t>(width) *
                              static_cast<std::size_t>(height) * 4U);
            std::uint8_t *const dst[4]{out.pixels.data(), nullptr, nullptr,
                                       nullptr};
            const int stride[4]{width * 4, 0, 0, 0};
            sws_scale(decoder.scaler, source_frame->data,
                      source_frame->linesize, 0, height, dst, stride);
            decoder.next_index = index + 1;
            av_frame_unref(decoder.transferred_frame);
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

[[nodiscard]] MediaDecodeSupport probe_decoder_support(Decoder &decoder) {
    bool draining = false;
    while (true) {
        const int got = avcodec_receive_frame(decoder.codec, decoder.frame);
        if (got == 0) {
            const bool hardware =
                decoder.hardware_active &&
                decoder.frame->format == decoder.hardware_format;
            av_frame_unref(decoder.frame);
            return hardware ? MediaDecodeSupport::HardwareAccelerated
                            : MediaDecodeSupport::SoftwareOnly;
        }
        if (got == AVERROR_EOF) {
            return MediaDecodeSupport::Unavailable;
        }
        if (got != AVERROR(EAGAIN)) {
            return MediaDecodeSupport::Unavailable;
        }
        if (draining) {
            return MediaDecodeSupport::Unavailable;
        }

        while (true) {
            const int read = av_read_frame(decoder.format, decoder.packet);
            if (read < 0) {
                if (avcodec_send_packet(decoder.codec, nullptr) < 0) {
                    return MediaDecodeSupport::Unavailable;
                }
                draining = true;
                break;
            }
            if (decoder.packet->stream_index != decoder.stream) {
                av_packet_unref(decoder.packet);
                continue;
            }
            const int sent = avcodec_send_packet(decoder.codec, decoder.packet);
            av_packet_unref(decoder.packet);
            if (sent < 0) {
                continue;
            }
            break;
        }
    }
}

// A video decoder behind the MediaSource seam. Holds an open decoder per source
// and walks frames, seeking only when the request is not the next frame.
class FfmpegSource final : public MediaSource {
  public:
    [[nodiscard]] bool hardware_configured() const noexcept {
        return std::any_of(
            decoders_.begin(), decoders_.end(), [](const auto &entry) {
                return entry.second->hardware_format != AV_PIX_FMT_NONE;
            });
    }

    [[nodiscard]] bool hardware_accelerated() const noexcept {
        return std::any_of(
            decoders_.begin(), decoders_.end(),
            [](const auto &entry) { return entry.second->hardware_active; });
    }

    [[nodiscard]] std::string key(const std::string &source,
                                  core::u32 frame_index) const override {
        return source + "#f" + std::to_string(frame_index);
    }

    [[nodiscard]] DecodedFrame decode(const std::string &source,
                                      core::u32 frame_index) override {
        auto found = decoders_.find(source);
        if (found == decoders_.end()) {
            auto decoder = open_decoder(source);
            if (decoder == nullptr) {
                return DecodedFrame{};
            }
            found = decoders_.emplace(source, std::move(decoder)).first;
        }
        Decoder &decoder = *found->second;
        const auto target = static_cast<core::i64>(frame_index);

        // Small forward gaps are cheaper and more stable when decoded and
        // discarded. Seeking H.26x walks back to a keyframe and flushes all
        // codec state; doing that whenever the show skips a frame creates a
        // feedback loop once playback falls slightly behind. Large jumps and
        // reverse scrubs still seek so random access stays bounded.
        constexpr core::i64 max_forward_decode_gap = 12;
        const bool sequential_or_small_forward_gap =
            decoder.next_index >= 0 && target >= decoder.next_index &&
            target - decoder.next_index <= max_forward_decode_gap;
        if (decoder.next_index != target && !sequential_or_small_forward_gap) {
            const AVStream *const stream =
                decoder.format->streams[decoder.stream];
            const double seconds =
                static_cast<double>(target) / std::max(decoder.fps, 0.001);
            const auto ts =
                decoder.start_pts +
                static_cast<core::i64>(seconds / av_q2d(stream->time_base));
            if (av_seek_frame(decoder.format, decoder.stream, ts,
                              AVSEEK_FLAG_BACKWARD) < 0) {
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
[[nodiscard]] std::string canonical_source_key(const std::string &source) {
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
    core::u64 generation{};
};

// Decoder state is serial per playback stream, while work is scheduled onto a
// tiny shared pool. Several independently timed clips can use one file without
// seeking one shared decoder back and forth; their completed immutable frames
// are still shared process-wide by (source, frame).
struct AsyncVideoSourceState final {
    AsyncVideoSourceState(std::string key, std::string path)
        : source_key(std::move(key)), decode_path(std::move(path)),
          decoder(std::make_unique<FfmpegSource>()) {}

    std::string source_key;
    std::string decode_path;
    std::unique_ptr<FfmpegSource> decoder;
    std::deque<DecodeRequest> requests;
    std::optional<core::u32> in_flight;
    std::optional<core::u32> latest_valid;
    std::optional<core::u32> latest_requested;
    core::i64 playback_direction{1};
    core::u64 generation{1U};
    core::u64 in_flight_generation{};
    bool scheduled{};
    bool reset_decoder_after_in_flight{};
    std::atomic_bool hardware_accelerated{};
    std::atomic_bool hardware_configured{};
};

struct ReadyFrame final {
    std::shared_ptr<const DecodedFrame> frame;
    std::size_t bytes{};
    core::u64 stamp{};
};

using DecodeStreamKey = std::pair<std::string, core::u64>;

class SharedVideoRegistry final {
  public:
    SharedVideoRegistry() {
        // Hardware-decoded plates still need independent CPU transfer/colour
        // conversion turns. Keep enough lanes for a six-plate composition;
        // unknown and software-only streams are separately gated to two below.
        constexpr std::size_t worker_count = 6U;
        workers_.reserve(worker_count);
        for (std::size_t index = 0U; index < worker_count; ++index) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    SharedVideoRegistry(const SharedVideoRegistry &) = delete;
    SharedVideoRegistry &operator=(const SharedVideoRegistry &) = delete;

    ~SharedVideoRegistry() {
        {
            const std::lock_guard lock{mutex_};
            stopping_ = true;
        }
        ready_.notify_all();
        for (auto &worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    [[nodiscard]] std::shared_ptr<AsyncVideoSourceState>
    try_source(const std::string &decode_path, core::u64 stream) noexcept {
        try {
            const std::string key = canonical_source_key(decode_path);
            const DecodeStreamKey stream_key{key, stream};
            std::unique_lock lock{mutex_, std::try_to_lock};
            if (!lock.owns_lock()) {
                return nullptr;
            }
            // Source pruning can retire decoder/file state, so workers and
            // explicit refresh perform it. A frame-path lookup only observes or
            // admits while it owns the mutex immediately.
            const auto found = sources_.find(stream_key);
            if (found != sources_.end()) {
                if (auto shared = found->second.lock()) {
                    return shared;
                }
            }
            auto state =
                std::make_shared<AsyncVideoSourceState>(key, decode_path);
            sources_.insert_or_assign(stream_key, state);
            return state;
        } catch (...) {
            // Source admission is best effort. Allocation pressure or another
            // local setup failure is a dropped request, never a render failure.
            return nullptr;
        }
    }

    [[nodiscard]] std::shared_ptr<const DecodedFrame>
    try_frame(const std::shared_ptr<AsyncVideoSourceState> &source,
              core::u32 frame_index) noexcept {
        if (source == nullptr) {
            return nullptr;
        }
        try {
            std::unique_lock lock{mutex_, std::try_to_lock};
            if (!lock.owns_lock()) {
                return nullptr;
            }
            if (source->latest_requested.has_value()) {
                const core::i64 delta =
                    static_cast<core::i64>(frame_index) -
                    static_cast<core::i64>(*source->latest_requested);
                // A large jump is a seek or a forward loop wrapping to its
                // start; neither should reinterpret old frames as a reverse-
                // play trail.
                if (delta != 0 && std::abs(delta) <= 4) {
                    source->playback_direction = delta < 0 ? -1 : 1;
                } else if (std::abs(delta) > 4) {
                    source->playback_direction = 1;
                }
            }
            source->latest_requested = frame_index;
            const auto found =
                frames_.find(std::make_pair(source->source_key, frame_index));
            if (found == frames_.end()) {
                enqueue_locked(source, frame_index, true);
                return nullptr;
            }
            // Invalid is a persistent completed answer. It is retried only by
            // explicit refresh(), never because local wall time elapsed.
            found->second.stamp = ++stamp_;
            return found->second.frame;
        } catch (...) {
            return nullptr;
        }
    }

    [[nodiscard]] std::shared_ptr<const DecodedFrame>
    try_prefetched(const std::shared_ptr<AsyncVideoSourceState>& source,
                   core::u32 frame_index) noexcept {
        if (source == nullptr) return nullptr;
        try {
            std::unique_lock lock{mutex_, std::try_to_lock};
            if (!lock.owns_lock()) return nullptr;
            const auto found = frames_.find({source->source_key, frame_index});
            if (found == frames_.end()) return nullptr;
            found->second.stamp = ++stamp_;
            return found->second.frame;
        } catch (...) { return nullptr; }
    }

    [[nodiscard]] std::shared_ptr<const DecodedFrame>
    try_latest(const std::shared_ptr<AsyncVideoSourceState> &source,
               core::u32 frame_index) noexcept {
        if (source == nullptr) {
            return nullptr;
        }
        try {
            std::unique_lock lock{mutex_, std::try_to_lock};
            if (!lock.owns_lock()) {
                return nullptr;
            }
            // A moving playhead can advance while UHD transfer/conversion is in
            // flight. Return a recent frame from this same clip stream, never
            // from another cut that happens to use the same file.
            constexpr core::u32 maximum_late_frames = 4U;
            if (!source->latest_valid.has_value() ||
                *source->latest_valid > frame_index ||
                frame_index - *source->latest_valid > maximum_late_frames) {
                return nullptr;
            }
            const auto late = frames_.find(
                std::make_pair(source->source_key, *source->latest_valid));
            if (late == frames_.end() || !late->second.frame->valid()) {
                return nullptr;
            }
            late->second.stamp = ++stamp_;
            return late->second.frame;
        } catch (...) {
            return nullptr;
        }
    }

    void prefetch(const std::shared_ptr<AsyncVideoSourceState> &source,
                  core::u32 frame_index) noexcept {
        if (source == nullptr) {
            return;
        }
        try {
            std::unique_lock lock{mutex_, std::try_to_lock};
            if (!lock.owns_lock()) {
                return;
            }
            const auto found =
                frames_.find(std::make_pair(source->source_key, frame_index));
            if (found != frames_.end()) {
                // Both a valid frame and a persistent failed frame are complete.
                // Touch valid residency, but never turn a speculative hint into
                // an implicit retry of a failure.
                if (found->second.frame->valid()) {
                    found->second.stamp = ++stamp_;
                }
                return;
            }
            enqueue_locked(source, frame_index, false);
        } catch (...) {
            // Speculative admission is allowed to disappear.
        }
    }

    [[nodiscard]] bool refresh(const std::string &decode_path) noexcept {
        try {
            const std::string key = canonical_source_key(decode_path);
            // Explicit operator/control-path work may synchronize. Render-side
            // access uses try_lock and drops while invalidation retires frames
            // or decoder state under this mutex.
            const std::lock_guard lock{mutex_};
            for (auto frame = frames_.begin(); frame != frames_.end();) {
                if (frame->first.first == key) {
                    resident_bytes_ -= frame->second.bytes;
                    frame = frames_.erase(frame);
                } else {
                    ++frame;
                }
            }
            for (auto &[stream_key, weak] : sources_) {
                if (stream_key.first != key) {
                    continue;
                }
                const auto source = weak.lock();
                if (source == nullptr) {
                    continue;
                }
                ++source->generation;
                if (source->generation == 0U) {
                    ++source->generation;
                }
                source->requests.clear();
                source->latest_valid.reset();
                source->latest_requested.reset();
                source->playback_direction = 1;
                source->hardware_accelerated.store(
                    false, std::memory_order_relaxed);
                source->hardware_configured.store(
                    false, std::memory_order_relaxed);
                if (source->in_flight.has_value()) {
                    source->reset_decoder_after_in_flight = true;
                } else {
                    source->decoder.reset();
                    source->reset_decoder_after_in_flight = false;
                }
            }
            prune_inactive_locked();
            return true;
        } catch (...) {
            return false;
        }
    }

    template <typename ReleaseRenderReference>
    [[nodiscard]] bool try_defer_retirement(
        const std::shared_ptr<AsyncVideoSourceState> &source,
        ReleaseRenderReference &&release_render_reference) noexcept {
        static_assert(std::is_nothrow_invocable_v<ReleaseRenderReference &&>);
        if (source == nullptr) {
            std::forward<ReleaseRenderReference>(release_render_reference)();
            return true;
        }
        std::unique_lock lock{mutex_, std::try_to_lock};
        if (!lock.owns_lock() || retired_count_ >= retired_.size()) {
            return false;
        }
        const std::size_t slot =
            (retired_first_ + retired_count_) % retired_.size();
        retired_[slot] = source;
        // Keep the registry mutex until the caller has dropped its owning
        // render-side reference. A worker cannot consume the retirement slot
        // first and accidentally leave decoder teardown to erase().
        std::forward<ReleaseRenderReference>(release_render_reference)();
        ++retired_count_;
        ready_.notify_one();
        return true;
    }

    void hold_lock_for_test(const std::function<void()> &callback) {
        const std::lock_guard lock{mutex_};
        callback();
    }

    [[nodiscard]] MediaDecodeStats stats() {
        const std::lock_guard lock{mutex_};
        prune_inactive_locked();
        MediaDecodeStats result{};
        std::set<std::string> active_source_keys;
        std::set<std::string> hardware_source_keys;
        std::set<std::string> hardware_configured_source_keys;
        for (const auto &[stream_key, weak] : sources_) {
            static_cast<void>(stream_key);
            if (const auto source = weak.lock()) {
                active_source_keys.insert(source->source_key);
                if (source->hardware_accelerated.load(
                        std::memory_order_relaxed)) {
                    hardware_source_keys.insert(source->source_key);
                }
                if (source->hardware_configured.load(
                        std::memory_order_relaxed)) {
                    hardware_configured_source_keys.insert(source->source_key);
                }
                ++result.active_streams;
                result.pending_requests += source->requests.size();
                if (source->in_flight.has_value()) {
                    ++result.pending_requests;
                }
            }
        }
        result.active_sources = active_source_keys.size();
        result.hardware_sources = hardware_source_keys.size();
        result.hardware_configured_sources =
            hardware_configured_source_keys.size();
        result.ready_frames = frames_.size();
        result.ready_bytes = resident_bytes_;
        result.byte_budget = max_ready_bytes_;
        result.trail_frames = trail_frames_;
        result.completed_frames = completed_frames_;
        result.failed_frames = failed_frames_;
        return result;
    }

    void configure_buffered_output(MediaDecodeConfig config) noexcept {
        const std::lock_guard lock{mutex_};
        // Configuration only changes ceilings; decoded memory remains lazy.
        max_requests_per_source_ =
            std::max(config.maximum_requests_per_source, std::size_t{1U});
        max_ready_frames_ =
            std::max(config.maximum_ready_frames, std::size_t{1U});
        max_ready_bytes_ =
            std::max(config.maximum_ready_bytes, std::size_t{1U});
        trail_frames_ = config.trail_frames;
    }

  private:
    using FrameKey = std::pair<std::string, core::u32>;

    // Deep enough to hold the renderer's whole read-ahead window for one
    // source, so look-ahead work is not thrown away the moment a current-frame
    // request arrives and re-queued from scratch on the next show frame.
    static constexpr std::size_t default_max_requests_per_source = 6U;
    // The byte cap below is the authoritative one; this is a guard for media
    // small enough that thousands of frames would fit under it. It has to clear
    // what a show legitimately holds at once - every visible clip's current
    // frame and read-ahead, plus the frames warmed for a cut or the show's wrap
    // that nothing has drawn yet - or the warm-up evicts itself before the
    // frame it was decoded for arrives.
    static constexpr std::size_t default_max_ready_frames = 48U;
    // Six concurrent clips with a three-frame headroom is the first ordinary
    // operator-scale case: two 4K placements plus four 1080p placements need
    // just under 384 MiB of RGBA residency. Leave room for cut/loop prewarm
    // without allowing decoded video to grow with the show.
    static constexpr std::size_t default_max_ready_bytes = 512U * 1024U * 1024U;

    void enqueue_locked(const std::shared_ptr<AsyncVideoSourceState> &source,
                         core::u32 frame_index, bool urgent) {
        if (source->in_flight == frame_index &&
            source->in_flight_generation == source->generation) {
            return;
        }
        auto queued =
            std::find_if(source->requests.begin(), source->requests.end(),
                         [frame_index, generation = source->generation](
                             const DecodeRequest &request) {
                             return request.frame == frame_index &&
                                    request.generation == generation;
                         });
        if (queued != source->requests.end()) {
            if (!urgent || queued->urgent) {
                return;
            }
            // Upgrade a look-ahead request to the current-frame request and
            // drop stale queued work. The in-flight decode remains untouched.
            source->requests.clear();
            source->requests.push_front(
                DecodeRequest{frame_index, true, source->generation});
        } else if (urgent) {
            // Latest current-frame demand wins. Holding the last uploaded frame
            // is preferable to decoding a backlog the playhead has passed.
            source->requests.clear();
            source->requests.push_front(
                DecodeRequest{frame_index, true, source->generation});
        } else if (source->requests.size() < max_requests_per_source_) {
            source->requests.push_back(
                DecodeRequest{frame_index, false, source->generation});
        } else {
            return;
        }

        if (!source->scheduled) {
            work_.push_back(source);
            source->scheduled = true;
            ready_.notify_one();
        }
    }

    void publish_locked(const std::shared_ptr<AsyncVideoSourceState> &source,
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
            ReadyFrame{std::move(frame), bytes, ++stamp_});
        resident_bytes_ += bytes;
        if (bytes != 0U) {
            source->latest_valid = frame_index;
        }

        // Keep the newly decoded frame even when one unusually large image is
        // itself above budget. Otherwise evict globally least-recently-used
        // frames until both the byte and small-frame guards are satisfied.
        while ((resident_bytes_ > max_ready_bytes_ ||
                frames_.size() > max_ready_frames_) &&
               frames_.size() > 1U) {
            auto oldest = frames_.end();
            for (auto candidate = frames_.begin(); candidate != frames_.end();
                 ++candidate) {
                if (candidate->first == key ||
                    retained_trail_locked(candidate->first)) {
                    continue;
                }
                if (oldest == frames_.end() ||
                    candidate->second.stamp < oldest->second.stamp) {
                    oldest = candidate;
                }
            }
            // Trail retention is soft. If it alone would violate the selected
            // memory budget, evict its oldest frame rather than growing without
            // bound; the current publication remains protected in every case.
            if (oldest == frames_.end()) {
                for (auto candidate = frames_.begin();
                     candidate != frames_.end(); ++candidate) {
                    if (candidate->first == key) {
                        continue;
                    }
                    if (oldest == frames_.end() ||
                        candidate->second.stamp < oldest->second.stamp) {
                        oldest = candidate;
                    }
                }
                if (oldest == frames_.end()) {
                    break;
                }
            }
            resident_bytes_ -= oldest->second.bytes;
            frames_.erase(oldest);
        }
    }

    [[nodiscard]] bool
    retained_trail_locked(const FrameKey &frame) const noexcept {
        if (trail_frames_ == 0U) {
            return false;
        }
        for (const auto &[stream_key, weak] : sources_) {
            static_cast<void>(stream_key);
            const auto source = weak.lock();
            if (!source || source->source_key != frame.first ||
                !source->latest_requested.has_value()) {
                continue;
            }
            const core::i64 distance =
                source->playback_direction > 0
                    ? static_cast<core::i64>(*source->latest_requested) -
                          static_cast<core::i64>(frame.second)
                    : static_cast<core::i64>(frame.second) -
                          static_cast<core::i64>(*source->latest_requested);
            if (distance > 0 &&
                distance <= static_cast<core::i64>(trail_frames_)) {
                return true;
            }
        }
        return false;
    }

    void prune_inactive_locked() {
        std::set<std::string> maybe_inactive_sources;
        for (auto source = sources_.begin(); source != sources_.end();) {
            if (!source->second.expired()) {
                ++source;
                continue;
            }
            maybe_inactive_sources.insert(source->first.first);
            source = sources_.erase(source);
        }
        for (const auto &dead_key : maybe_inactive_sources) {
            const bool still_active =
                std::any_of(sources_.begin(), sources_.end(),
                            [&dead_key](const auto &candidate) {
                                return candidate.first.first == dead_key &&
                                       !candidate.second.expired();
                            });
            if (still_active) {
                continue;
            }
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
            std::shared_ptr<AsyncVideoSourceState> retired;
            DecodeRequest request{};
            bool counted_as_software = false;
            {
                std::unique_lock lock{mutex_};
                const auto eligible =
                    [this](const std::shared_ptr<AsyncVideoSourceState>
                               &candidate) {
                        return candidate->hardware_accelerated.load(
                                   std::memory_order_relaxed) ||
                               software_jobs_in_flight_ < max_software_jobs;
                    };
                ready_.wait(lock, [this, &eligible] {
                    return stopping_ || retired_count_ != 0U ||
                           std::any_of(work_.begin(), work_.end(), eligible);
                });
                if (stopping_) {
                    return;
                }
                if (retired_count_ != 0U) {
                    retired = std::move(retired_[retired_first_]);
                    retired_[retired_first_].reset();
                    retired_first_ = (retired_first_ + 1U) % retired_.size();
                    --retired_count_;
                } else {
                    const auto selected =
                        std::find_if(work_.begin(), work_.end(), eligible);
                    source = std::move(*selected);
                    work_.erase(selected);
                    if (source->requests.empty()) {
                        source->scheduled = false;
                        continue;
                    }
                    counted_as_software = !source->hardware_accelerated.load(
                        std::memory_order_relaxed);
                    if (counted_as_software) {
                        ++software_jobs_in_flight_;
                    }
                    request = source->requests.front();
                    source->requests.pop_front();
                    source->in_flight = request.frame;
                    source->in_flight_generation = request.generation;
                }
            }

            if (retired != nullptr) {
                // Decoder/file teardown happens only on this worker. A source
                // still referenced by queued/in-flight work simply survives
                // until that worker-owned reference is retired later.
                retired.reset();
                const std::lock_guard lock{mutex_};
                prune_inactive_locked();
                continue;
            }

            std::shared_ptr<const DecodedFrame> decoded;
            try {
                if (source->decoder == nullptr) {
                    source->decoder = std::make_unique<FfmpegSource>();
                }
                decoded = std::make_shared<const DecodedFrame>(
                    source->decoder->decode(source->decode_path, request.frame));
                source->hardware_accelerated.store(
                    source->decoder->hardware_accelerated(),
                    std::memory_order_relaxed);
                source->hardware_configured.store(
                    source->decoder->hardware_configured(),
                    std::memory_order_relaxed);
            } catch (...) {
                decoded = failed_frame_;
            }

            {
                const std::lock_guard lock{mutex_};
                if (counted_as_software) {
                    --software_jobs_in_flight_;
                }
                if (request.generation == source->generation) {
                    try {
                        publish_locked(source, request.frame, decoded);
                        ++completed_frames_;
                        if (!decoded->valid()) {
                            ++failed_frames_;
                        }
                    } catch (...) {
                        // A failed bounded publication leaves no completed
                        // answer. The next render may enqueue one best-effort
                        // retry; the worker itself remains alive.
                    }
                }
                if (source->reset_decoder_after_in_flight) {
                    // Explicit refresh raced an old-generation decode. Discard
                    // that result above, then retire the decoder on its serial
                    // worker before any refreshed request is allowed to run.
                    source->decoder.reset();
                    source->reset_decoder_after_in_flight = false;
                    source->hardware_accelerated.store(
                        false, std::memory_order_relaxed);
                    source->hardware_configured.store(
                        false, std::memory_order_relaxed);
                }
                source->in_flight.reset();
                source->in_flight_generation = 0U;
                if (!source->requests.empty()) {
                    // One frame per turn keeps two active clips fair while each
                    // source's decoder remains strictly serial.
                    try {
                        work_.push_back(source);
                        ready_.notify_one();
                    } catch (...) {
                        source->requests.clear();
                        source->scheduled = false;
                    }
                } else {
                    source->scheduled = false;
                }
                ready_.notify_all();
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
    static constexpr std::size_t maximum_deferred_retirements = 64U;
    std::array<std::shared_ptr<AsyncVideoSourceState>,
               maximum_deferred_retirements>
        retired_{};
    std::size_t retired_first_{};
    std::size_t retired_count_{};
    std::map<DecodeStreamKey, std::weak_ptr<AsyncVideoSourceState>> sources_;
    std::map<FrameKey, ReadyFrame> frames_;
    std::size_t max_requests_per_source_{default_max_requests_per_source};
    std::size_t max_ready_frames_{default_max_ready_frames};
    std::size_t max_ready_bytes_{default_max_ready_bytes};
    core::u32 trail_frames_{};
    static constexpr std::size_t max_software_jobs = 2U;
    std::size_t software_jobs_in_flight_{};
    std::size_t resident_bytes_{};
    core::u64 stamp_{};
    core::u64 completed_frames_{};
    core::u64 failed_frames_{};
    std::shared_ptr<const DecodedFrame> failed_frame_{
        std::make_shared<const DecodedFrame>()};
};

[[nodiscard]] SharedVideoRegistry &video_registry() {
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

    [[nodiscard]] std::string key(const std::string &source,
                                  core::u32 frame_index) const override {
        return is_video_path(source) ? video_.key(source, frame_index)
                                     : image_.key(source, frame_index);
    }

    [[nodiscard]] MediaDecodeScheduling scheduling(
        const std::string &source) const noexcept override {
        return is_video_path(source)
                   ? MediaDecodeScheduling::SourceManaged
                   : MediaDecodeScheduling::BackgroundRequired;
    }

    [[nodiscard]] DecodedFrame decode(const std::string &source,
                                      core::u32 frame_index) override {
        // image_ is stateless, so its cache-worker decode may safely overlap
        // render-thread video_ registry polling on this dispatcher.
        return is_video_path(source) ? video_.decode(source, frame_index)
                                     : image_.decode(source, frame_index);
    }

    [[nodiscard]] std::shared_ptr<const DecodedFrame>
    try_decode(const std::string &source, core::u32 frame_index) override {
        return try_decode(source, frame_index, 0U);
    }

    [[nodiscard]] std::shared_ptr<const DecodedFrame>
    try_decode(const std::string &source, core::u32 frame_index,
               core::u64 stream) override {
        if (!is_video_path(source)) {
            return MediaSource::try_decode(source, frame_index);
        }
        return video_registry().try_frame(worker_for(source, stream),
                                          frame_index);
    }

    [[nodiscard]] std::shared_ptr<const DecodedFrame>
    try_decode_latest(const std::string &source, core::u32 frame_index,
                      core::u64 stream) override {
        if (!is_video_path(source)) {
            return nullptr;
        }
        return video_registry().try_latest(worker_for(source, stream),
                                           frame_index);
    }

    [[nodiscard]] std::shared_ptr<const DecodedFrame>
    try_prefetched(const std::string& source, core::u32 frame_index,
                   core::u64 stream) override {
        if (!is_video_path(source)) return nullptr;
        return video_registry().try_prefetched(worker_for(source, stream), frame_index);
    }

    void prefetch(const std::string &source, core::u32 frame_index) override {
        prefetch(source, frame_index, 0U);
    }

    void prefetch(const std::string &source, core::u32 frame_index,
                  core::u64 stream) override {
        if (is_video_path(source)) {
            video_registry().prefetch(worker_for(source, stream), frame_index);
        }
    }

    [[nodiscard]] bool refresh(const std::string &source) noexcept override {
        return is_video_path(source) && video_registry().refresh(source);
    }

  private:
    struct WorkerEntry final {
        std::shared_ptr<AsyncVideoSourceState> worker;
        core::u64 stamp{};
    };

    [[nodiscard]] std::shared_ptr<AsyncVideoSourceState>
    worker_for(const std::string &source, core::u64 stream) noexcept {
        constexpr std::size_t max_retained_streams = 16U;
        try {
            const DecodeStreamKey worker_key{source, stream};
            const auto found = workers_.find(worker_key);
            if (found != workers_.end()) {
                found->second.stamp = ++worker_stamp_;
                return found->second.worker;
            }

            // Retire an idle decoder on a registry worker before erasing the
            // render-side strong reference. If the bounded retirement queue or
            // registry lock is unavailable, drop this cold admission instead
            // of closing an FFmpeg input on the frame path.
            if (workers_.size() >= max_retained_streams) {
                auto oldest = workers_.end();
                for (auto candidate = workers_.begin();
                     candidate != workers_.end(); ++candidate) {
                    if (oldest == workers_.end() ||
                        candidate->second.stamp < oldest->second.stamp) {
                        oldest = candidate;
                    }
                }
                if (oldest == workers_.end() ||
                    !video_registry().try_defer_retirement(
                        oldest->second.worker,
                        [this, oldest]() noexcept { workers_.erase(oldest); })) {
                    return nullptr;
                }
            }

            auto worker = video_registry().try_source(source, stream);
            if (worker == nullptr) {
                return nullptr;
            }
            try {
                workers_.emplace(
                    worker_key, WorkerEntry{worker, ++worker_stamp_});
            } catch (...) {
                static_cast<void>(video_registry().try_defer_retirement(
                    worker, [&worker]() noexcept { worker.reset(); }));
                return nullptr;
            }
            return worker;
        } catch (...) {
            return nullptr;
        }
    }

    ImageSequenceSource image_;
    FfmpegSource video_;
    std::map<DecodeStreamKey, WorkerEntry> workers_;
    core::u64 worker_stamp_{};
};

} // namespace

namespace detail {

void hold_video_registry_lock_for_test(
    const std::function<void()>& callback) {
    video_registry().hold_lock_for_test(callback);
}

} // namespace detail

std::unique_ptr<MediaSource> make_media_source() {
    return std::make_unique<DispatchMediaSource>();
}

MediaDecodeStats media_decode_stats() { return video_registry().stats(); }

void configure_buffered_output_media_decode(MediaDecodeConfig config) noexcept {
    try {
        video_registry().configure_buffered_output(config);
    } catch (...) {
        // Resource construction still has its ordinary failure path. Tuning is
        // an optimization and must never terminate a node during setup.
    }
}

MediaDecodeSupport probe_media_decode_support(const std::string &source) {
    if (!is_video_path(source)) {
        return MediaDecodeSupport::NotApplicable;
    }
    auto decoder = open_decoder(source);
    return decoder == nullptr ? MediaDecodeSupport::Unavailable
                              : probe_decoder_support(*decoder);
}

core::u32 probe_frame_count(const std::string &source) {
    return probe_media(source).frame_count;
}

MediaProbe probe_media(const std::string &source) {
    MediaProbe result{};
    if (!is_video_path(source)) {
        const auto path = ImageSequenceSource::resolve_path(source, 0U);
        if (radiance_source(path)) {
            ImageSequenceSource images;
            const auto image = images.decode(path, 0U);
            result.width = image.width;
            result.height = image.height;
            result.bits_per_raw_sample = 16;
            result.pixel_format = "rgba16f";
            return result;
        }
        int width = 0;
        int height = 0;
        int channels = 0;
        if (stbi_info(path.c_str(), &width, &height, &channels) != 0 &&
            width > 0 && height > 0) {
            result.width = static_cast<core::u32>(width);
            result.height = static_cast<core::u32>(height);
        }
        return result;
    }
    auto decoder = open_decoder(source);
    if (decoder == nullptr) {
        return result;
    }
    const AVStream *const stream = decoder->format->streams[decoder->stream];
    result.codec = avcodec_get_name(stream->codecpar->codec_id);
    if (const char *const profile = avcodec_profile_name(
            stream->codecpar->codec_id, stream->codecpar->profile)) {
        result.profile = profile;
    }
    if (stream->codecpar->format >= 0) {
        if (const char *const format = av_get_pix_fmt_name(
                static_cast<AVPixelFormat>(stream->codecpar->format))) {
            result.pixel_format = format;
        }
    }
    result.bits_per_raw_sample = stream->codecpar->bits_per_raw_sample;
    if (decoder->codec->width > 0 && decoder->codec->height > 0) {
        result.width = static_cast<core::u32>(decoder->codec->width);
        result.height = static_cast<core::u32>(decoder->codec->height);
    }
    result.frame_rate = stream_frame_rate(stream);
    if (stream->nb_frames > 0) {
        result.frame_count = static_cast<core::u32>(std::min<core::i64>(
            stream->nb_frames,
            static_cast<core::i64>(std::numeric_limits<core::u32>::max())));
    } else {
        // No frame count in the container: estimate from duration and rate.
        const double seconds =
            stream->duration != AV_NOPTS_VALUE
                ? static_cast<double>(stream->duration) *
                      av_q2d(stream->time_base)
                : (decoder->format->duration != AV_NOPTS_VALUE
                       ? static_cast<double>(decoder->format->duration) /
                             AV_TIME_BASE
                       : 0.0);
        const auto frames =
            static_cast<core::i64>(std::llround(seconds * decoder->fps));
        if (frames > 0) {
            result.frame_count = static_cast<core::u32>(std::min<core::i64>(
                frames,
                static_cast<core::i64>(std::numeric_limits<core::u32>::max())));
        }
    }
    return result;
}

std::optional<MediaFrameRate>
probe_media_frame_rate(const std::string &source) {
    return probe_media(source).frame_rate;
}

float probe_media_aspect(const std::string &source) {
    return probe_media(source).aspect();
}

} // namespace mediaserver::render
