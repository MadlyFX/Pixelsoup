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
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>

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

[[nodiscard]] double stream_fps(const AVStream* stream) {
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
        return av_q2d(stream->avg_frame_rate);
    }
    if (stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0) {
        return av_q2d(stream->r_frame_rate);
    }
    return 25.0;
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
    while (true) {
        const int read = av_read_frame(decoder.format, decoder.packet);
        if (read < 0) {
            // End of file: drain the decoder for any buffered frames.
            avcodec_send_packet(decoder.codec, nullptr);
        } else if (decoder.packet->stream_index != decoder.stream) {
            av_packet_unref(decoder.packet);
            continue;
        } else {
            const int sent = avcodec_send_packet(decoder.codec, decoder.packet);
            av_packet_unref(decoder.packet);
            if (sent < 0) {
                continue;
            }
        }

        while (true) {
            const int got = avcodec_receive_frame(decoder.codec, decoder.frame);
            if (got == AVERROR(EAGAIN)) {
                break; // need another packet
            }
            if (got < 0) {
                return DecodedFrame{}; // EOF or error, nothing to return
            }
            const auto pts = decoder.frame->best_effort_timestamp
                    == AV_NOPTS_VALUE
                ? decoder.frame->pts
                : decoder.frame->best_effort_timestamp;
            const core::i64 index = index_of(decoder, pts);
            if (index < target) {
                av_frame_unref(decoder.frame);
                continue;
            }

            const int width = decoder.codec->width;
            const int height = decoder.codec->height;
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

        if (read < 0) {
            return DecodedFrame{}; // drained with nothing at or after target
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

        // Seek unless the decoder is already positioned to read this frame.
        if (decoder.next_index != target) {
            const AVStream* const stream =
                decoder.format->streams[decoder.stream];
            const double seconds =
                static_cast<double>(target) / std::max(decoder.fps, 0.001);
            const auto ts = decoder.start_pts
                + static_cast<core::i64>(seconds / av_q2d(stream->time_base));
            av_seek_frame(
                decoder.format, decoder.stream, ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(decoder.codec);
            decoder.next_index = -1;
        }
        return decode_until(decoder, target);
    }

private:
    std::map<std::string, std::unique_ptr<Decoder>> decoders_;
};

// Routes each source to the still-image decoder or the video decoder by
// extension, so the MediaCache above never learns which a clip is.
class DispatchMediaSource final : public MediaSource {
public:
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

private:
    ImageSequenceSource image_;
    FfmpegSource video_;
};

} // namespace

std::unique_ptr<MediaSource> make_media_source() {
    return std::make_unique<DispatchMediaSource>();
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

} // namespace mediaserver::render
