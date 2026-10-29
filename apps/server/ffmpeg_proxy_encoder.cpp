#include "ffmpeg_proxy_encoder.hpp"

#include <mediaserver/assets/content_fingerprint.hpp>

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
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
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
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mediaserver::server {
namespace {

[[nodiscard]] int codec_thread_count(ProxyTranscodePriority priority) noexcept {
    switch (priority) {
    case ProxyTranscodePriority::Background:
        return 2;
    case ProxyTranscodePriority::Balanced:
        return 4;
    case ProxyTranscodePriority::Fast:
        // Zero asks FFmpeg to choose from the machine's available CPUs.
        return 0;
    }
    return 2;
}

[[nodiscard]] const char *
nvenc_preset(ProxyTranscodePriority priority) noexcept {
    switch (priority) {
    case ProxyTranscodePriority::Background:
        return "p4";
    case ProxyTranscodePriority::Balanced:
        return "p3";
    case ProxyTranscodePriority::Fast:
        return "p1";
    }
    return "p4";
}

[[nodiscard]] const char *
software_preset(ProxyTranscodePriority priority) noexcept {
    switch (priority) {
    case ProxyTranscodePriority::Background:
        return "veryfast";
    case ProxyTranscodePriority::Balanced:
        return "faster";
    case ProxyTranscodePriority::Fast:
        return "ultrafast";
    }
    return "veryfast";
}

struct InputCloser final {
    void operator()(AVFormatContext *value) const noexcept {
        if (value != nullptr) {
            avformat_close_input(&value);
        }
    }
};

struct OutputCloser final {
    void operator()(AVFormatContext *value) const noexcept {
        if (value != nullptr) {
            if (value->pb != nullptr) {
                static_cast<void>(avio_closep(&value->pb));
            }
            avformat_free_context(value);
        }
    }
};

struct CodecCloser final {
    void operator()(AVCodecContext *value) const noexcept {
        avcodec_free_context(&value);
    }
};

struct PacketCloser final {
    void operator()(AVPacket *value) const noexcept { av_packet_free(&value); }
};

struct FrameCloser final {
    void operator()(AVFrame *value) const noexcept { av_frame_free(&value); }
};

struct ScaleCloser final {
    void operator()(SwsContext *value) const noexcept {
        sws_freeContext(value);
    }
};

struct ResampleCloser final {
    void operator()(SwrContext *value) const noexcept { swr_free(&value); }
};

struct AudioFifoCloser final {
    void operator()(AVAudioFifo *value) const noexcept {
        av_audio_fifo_free(value);
    }
};

[[nodiscard]] ProxyBackendResult failure(ProxyBackendErrorCode code) noexcept {
    return ProxyBackendResult::failure(ProxyBackendError{code, {}});
}

[[nodiscard]] std::optional<assets::MediaFrameRate>
media_rate(const AVStream &stream) noexcept {
    const auto accept =
        [](AVRational value) -> std::optional<assets::MediaFrameRate> {
        if (value.num <= 0 || value.den <= 0) {
            return std::nullopt;
        }
        core::u32 numerator = static_cast<core::u32>(value.num);
        core::u32 denominator = static_cast<core::u32>(value.den);
        const core::u32 divisor = std::gcd(numerator, denominator);
        return assets::MediaFrameRate{numerator / divisor,
                                      denominator / divisor};
    };
    if (const auto average = accept(stream.avg_frame_rate)) {
        return average;
    }
    return accept(stream.r_frame_rate);
}

[[nodiscard]] std::optional<assets::DerivativeColorPrimaries>
primaries(AVColorPrimaries value) noexcept {
    if (value == AVCOL_PRI_UNSPECIFIED) {
        return assets::DerivativeColorPrimaries::None;
    }
    if (value == AVCOL_PRI_BT709) {
        return assets::DerivativeColorPrimaries::Rec709;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<assets::DerivativeTransfer>
transfer(AVColorTransferCharacteristic value) noexcept {
    if (value == AVCOL_TRC_UNSPECIFIED) {
        return assets::DerivativeTransfer::None;
    }
    if (value == AVCOL_TRC_BT709) {
        return assets::DerivativeTransfer::Rec709;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<assets::DerivativeMatrix>
matrix(AVColorSpace value) noexcept {
    if (value == AVCOL_SPC_UNSPECIFIED) {
        return assets::DerivativeMatrix::None;
    }
    if (value == AVCOL_SPC_RGB) {
        return assets::DerivativeMatrix::Rgb;
    }
    if (value == AVCOL_SPC_BT709) {
        return assets::DerivativeMatrix::Rec709;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<assets::DerivativeColorRange>
color_range(AVColorRange value) noexcept {
    if (value == AVCOL_RANGE_UNSPECIFIED) {
        return assets::DerivativeColorRange::None;
    }
    if (value == AVCOL_RANGE_JPEG) {
        return assets::DerivativeColorRange::Full;
    }
    if (value == AVCOL_RANGE_MPEG) {
        return assets::DerivativeColorRange::Limited;
    }
    return std::nullopt;
}

[[nodiscard]] AVColorPrimaries
av_primaries(assets::DerivativeColorPrimaries value) noexcept {
    return value == assets::DerivativeColorPrimaries::Rec709
               ? AVCOL_PRI_BT709
               : AVCOL_PRI_UNSPECIFIED;
}

[[nodiscard]] AVColorTransferCharacteristic
av_transfer(assets::DerivativeTransfer value) noexcept {
    return value == assets::DerivativeTransfer::Rec709 ? AVCOL_TRC_BT709
                                                       : AVCOL_TRC_UNSPECIFIED;
}

[[nodiscard]] AVColorSpace av_matrix(assets::DerivativeMatrix value) noexcept {
    if (value == assets::DerivativeMatrix::Rec709) {
        return AVCOL_SPC_BT709;
    }
    return value == assets::DerivativeMatrix::Rgb ? AVCOL_SPC_RGB
                                                  : AVCOL_SPC_UNSPECIFIED;
}

[[nodiscard]] AVColorRange
av_color_range(assets::DerivativeColorRange value) noexcept {
    if (value == assets::DerivativeColorRange::Full) {
        return AVCOL_RANGE_JPEG;
    }
    return value == assets::DerivativeColorRange::Limited
               ? AVCOL_RANGE_MPEG
               : AVCOL_RANGE_UNSPECIFIED;
}

[[nodiscard]] std::optional<ProxySourceMetadata>
source_metadata(AVFormatContext &format, const AVStream &stream) noexcept {
    const AVCodecParameters &parameters = *stream.codecpar;
    const auto rate_value = media_rate(stream);
    const auto primaries_value = primaries(parameters.color_primaries);
    const auto transfer_value = transfer(parameters.color_trc);
    const auto matrix_value = matrix(parameters.color_space);
    const auto range_value = color_range(parameters.color_range);
    if (parameters.width <= 0 || parameters.height <= 0 ||
        stream.nb_frames <= 0 || !rate_value || !primaries_value ||
        !transfer_value || !matrix_value || !range_value) {
        return std::nullopt;
    }
    AVRational aspect = parameters.sample_aspect_ratio;
    if (aspect.num <= 0 || aspect.den <= 0) {
        aspect = AVRational{1, 1};
    }
    const int divisor = std::gcd(aspect.num, aspect.den);
    const int audio =
        av_find_best_stream(&format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    return ProxySourceMetadata{
        assets::PixelDimensions{static_cast<core::u32>(parameters.width),
                                static_cast<core::u32>(parameters.height)},
        static_cast<core::u64>(stream.nb_frames),
        *rate_value,
        static_cast<core::u32>(aspect.num / divisor),
        static_cast<core::u32>(aspect.den / divisor),
        0U,
        *primaries_value,
        *transfer_value,
        *matrix_value,
        *range_value,
        audio >= 0,
    };
}

[[nodiscard]] assets::PixelDimensions
fitted_dimensions(assets::PixelDimensions source,
                  const assets::DerivativeProfile &profile) noexcept {
    core::u64 width = source.width;
    core::u64 height = source.height;
    if (width > profile.width) {
        height = (height * profile.width) / width;
        width = profile.width;
    }
    if (height > profile.height) {
        width = (width * profile.height) / height;
        height = profile.height;
    }
    width = std::max<core::u64>(2U, width & ~core::u64{1U});
    height = std::max<core::u64>(2U, height & ~core::u64{1U});
    return assets::PixelDimensions{static_cast<core::u32>(width),
                                   static_cast<core::u32>(height)};
}

[[nodiscard]] const AVCodec *
open_encoder(AVCodecContext *&opened, const ProxySourceMetadata &source,
             assets::PixelDimensions dimensions,
             const assets::DerivativeProfile &profile,
             ProxyTranscodePriority priority) noexcept {
    constexpr std::array<std::string_view, 5U> h264_names{
        {"h264_nvenc", "h264_mf", "libx264", "libopenh264", "h264"}};
    constexpr std::array<std::string_view, 2U> hevc_main10_names{
        {"hevc_nvenc", "libx265"}};
    const bool hevc_main10 =
        profile.codec == assets::DerivativeCodec::Hevc &&
        profile.pixel_format == assets::DerivativePixelFormat::Yuv420p10;
    const auto try_encoder = [&](std::string_view name) -> const AVCodec * {
        const std::string owned{name};
        const AVCodec *const codec =
            avcodec_find_encoder_by_name(owned.c_str());
        if (codec == nullptr) {
            return nullptr;
        }
        std::unique_ptr<AVCodecContext, CodecCloser> context{
            avcodec_alloc_context3(codec)};
        if (!context) {
            return nullptr;
        }
        context->width = static_cast<int>(dimensions.width);
        context->height = static_cast<int>(dimensions.height);
        context->pix_fmt = hevc_main10 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_YUV420P;
        context->time_base =
            AVRational{static_cast<int>(source.frame_rate.denominator),
                       static_cast<int>(source.frame_rate.numerator)};
        context->framerate = av_inv_q(context->time_base);
        context->sample_aspect_ratio =
            AVRational{static_cast<int>(source.pixel_aspect_numerator),
                       static_cast<int>(source.pixel_aspect_denominator)};
        context->gop_size = static_cast<int>(profile.keyframe_interval);
        // Background mode preserves the former two-thread ceiling; the
        // operator may explicitly trade more machine capacity for a faster
        // transcode. Hardware encoders largely ignore this CPU-side count.
        context->thread_count = codec_thread_count(priority);
        context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        const bool bitrate_encoder =
            name == "h264_mf" || name == "hevc_mf" || name == "libopenh264";
        const bool nvenc = name == "h264_nvenc" || name == "hevc_nvenc";
        context->max_b_frames = nvenc || bitrate_encoder ? 0 : 2;
        if (bitrate_encoder) {
            context->bit_rate = hevc_main10 ? 40'000'000 : 8'000'000;
        }
        context->color_primaries = av_primaries(source.color_primaries);
        context->color_trc = av_transfer(source.transfer);
        context->colorspace = av_matrix(source.matrix);
        context->color_range = av_color_range(source.color_range);
        context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVDictionary *options{};
        const std::string quality = std::to_string(profile.quality);
        if (nvenc || !bitrate_encoder) {
            av_dict_set(&options, "forced-idr", "1", 0);
        }
        if (nvenc) {
            av_dict_set(&options, "preset", nvenc_preset(priority), 0);
            av_dict_set(&options, "rc", "vbr", 0);
            av_dict_set(&options, "cq", quality.c_str(), 0);
            if (hevc_main10) {
                av_dict_set(&options, "profile", "main10", 0);
            }
        } else if (!bitrate_encoder) {
            av_dict_set(&options, "preset", software_preset(priority), 0);
            av_dict_set(&options, "crf", quality.c_str(), 0);
        }
        const int result = avcodec_open2(context.get(), codec, &options);
        av_dict_free(&options);
        if (result >= 0) {
            opened = context.release();
            return codec;
        }
        return nullptr;
    };
    if (hevc_main10) {
        for (const std::string_view name : hevc_main10_names) {
            if (const AVCodec *const codec = try_encoder(name)) {
                return codec;
            }
        }
    } else if (profile.codec == assets::DerivativeCodec::H264 &&
               profile.pixel_format == assets::DerivativePixelFormat::Yuv420p) {
        for (const std::string_view name : h264_names) {
            if (const AVCodec *const codec = try_encoder(name)) {
                return codec;
            }
        }
    }
    return nullptr;
}

[[nodiscard]] std::optional<ProxyEncodedMetadata>
probe_output(const std::filesystem::path &path,
             const ProxySourceMetadata &source,
             const assets::DerivativeProfile &profile) noexcept {
    AVFormatContext *raw{};
    const std::string owned = path.string();
    if (avformat_open_input(&raw, owned.c_str(), nullptr, nullptr) < 0 ||
        raw == nullptr) {
        return std::nullopt;
    }
    std::unique_ptr<AVFormatContext, InputCloser> format{raw};
    if (avformat_find_stream_info(format.get(), nullptr) < 0) {
        return std::nullopt;
    }
    const int index = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1,
                                          -1, nullptr, 0);
    if (index < 0) {
        return std::nullopt;
    }
    const AVStream &stream = *format->streams[index];
    const AVCodecParameters &parameters = *stream.codecpar;
    const auto rate_value = media_rate(stream);
    const bool yuv420_8 = parameters.format == AV_PIX_FMT_YUV420P ||
                          parameters.format == AV_PIX_FMT_YUVJ420P ||
                          parameters.format == AV_PIX_FMT_NV12;
    const bool yuv420_10 = parameters.format == AV_PIX_FMT_YUV420P10LE ||
                           parameters.format == AV_PIX_FMT_P010LE;
    const bool expected_video =
        (profile.codec == assets::DerivativeCodec::H264 &&
         parameters.codec_id == AV_CODEC_ID_H264 && yuv420_8) ||
        (profile.codec == assets::DerivativeCodec::Hevc &&
         parameters.codec_id == AV_CODEC_ID_HEVC && yuv420_10);
    if (!expected_video || parameters.width <= 0 || parameters.height <= 0 ||
        stream.nb_frames <= 0 || !rate_value) {
        std::cerr << "proxy output video validation failed: codec="
                  << static_cast<int>(parameters.codec_id)
                  << " format=" << parameters.format
                  << " size=" << parameters.width << 'x' << parameters.height
                  << " frames=" << stream.nb_frames
                  << " rate=" << (rate_value.has_value() ? 1 : 0) << '\n';
        return std::nullopt;
    }
    const int audio_index = av_find_best_stream(
        format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    const bool encoded_audio = audio_index >= 0;
    const bool expected_audio =
        source.has_audio &&
        profile.audio_codec != assets::DerivativeAudioCodec::None;
    if (encoded_audio != expected_audio) {
        return std::nullopt;
    }
    if (encoded_audio) {
        const AVCodecParameters &audio =
            *format->streams[audio_index]->codecpar;
        if (audio.codec_id != AV_CODEC_ID_AAC || audio.sample_rate != 48'000 ||
            audio.ch_layout.nb_channels != 2) {
            std::cerr << "proxy output audio validation failed: codec="
                      << static_cast<int>(audio.codec_id)
                      << " rate=" << audio.sample_rate
                      << " channels=" << audio.ch_layout.nb_channels << '\n';
            return std::nullopt;
        }
    }
    return ProxyEncodedMetadata{
        assets::PixelDimensions{static_cast<core::u32>(parameters.width),
                                static_cast<core::u32>(parameters.height)},
        static_cast<core::u64>(stream.nb_frames),
        *rate_value,
        source.pixel_aspect_numerator,
        source.pixel_aspect_denominator,
        source.clockwise_quarter_turns,
        source.color_primaries,
        source.transfer,
        source.matrix,
        source.color_range,
        assets::DerivativeContainer::Mp4,
        profile.codec,
        profile.pixel_format,
        encoded_audio ? assets::DerivativeAudioLayout::Stereo
                      : assets::DerivativeAudioLayout::None,
        encoded_audio ? assets::DerivativeAudioCodec::Aac
                      : assets::DerivativeAudioCodec::None,
        encoded_audio ? 48'000U : 0U,
        encoded_audio ? 192'000U : 0U,
    };
}

class FfmpegProxyEncoder final : public ProxyEncoderBackend {
  public:
    explicit FfmpegProxyEncoder(ProxyTranscodePriority priority) noexcept
        : priority_(priority) {}

    ProxyBackendResult encode(const ProxyEncodeRequest &request,
                              std::stop_token stop) override {
        if (stop.stop_requested()) {
            return failure(ProxyBackendErrorCode::Cancelled);
        }
        const auto measured = assets::source_fingerprint(request.source, stop);
        if (!measured) {
            return failure(stop.stop_requested()
                               ? ProxyBackendErrorCode::Cancelled
                               : ProxyBackendErrorCode::SourceUnavailable);
        }

        AVFormatContext *raw_input{};
        if (avformat_open_input(&raw_input, request.source.c_str(), nullptr,
                                nullptr) < 0 ||
            raw_input == nullptr) {
            return failure(ProxyBackendErrorCode::SourceUnavailable);
        }
        std::unique_ptr<AVFormatContext, InputCloser> input{raw_input};
        if (avformat_find_stream_info(input.get(), nullptr) < 0) {
            return failure(ProxyBackendErrorCode::MetadataUnavailable);
        }
        const int video_index = av_find_best_stream(
            input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (video_index < 0) {
            return failure(ProxyBackendErrorCode::MetadataUnavailable);
        }
        AVStream &source_stream = *input->streams[video_index];
        const auto metadata = source_metadata(*input, source_stream);
        if (!metadata) {
            const AVCodecParameters &parameters = *source_stream.codecpar;
            std::cerr << "proxy source metadata unsupported: frames="
                      << source_stream.nb_frames << " primaries="
                      << static_cast<int>(parameters.color_primaries)
                      << " transfer=" << static_cast<int>(parameters.color_trc)
                      << " matrix=" << static_cast<int>(parameters.color_space)
                      << " range=" << static_cast<int>(parameters.color_range)
                      << '\n';
            return failure(ProxyBackendErrorCode::MetadataUnavailable);
        }
        if (metadata->clockwise_quarter_turns != 0U) {
            std::cerr << "proxy source rotation unsupported: "
                      << static_cast<unsigned>(
                             metadata->clockwise_quarter_turns)
                      << '\n';
            return failure(ProxyBackendErrorCode::MetadataUnavailable);
        }
        report_progress(request, 0U, metadata->frame_count);
        if (!request.join_segments.empty()) {
            return join(request, *metadata, measured.value(), stop);
        }
        const AVCodec *const decoder =
            avcodec_find_decoder(source_stream.codecpar->codec_id);
        std::unique_ptr<AVCodecContext, CodecCloser> decode{
            decoder == nullptr ? nullptr : avcodec_alloc_context3(decoder)};
        if (!decode || avcodec_parameters_to_context(
                           decode.get(), source_stream.codecpar) < 0) {
            return failure(ProxyBackendErrorCode::EncodeFailed);
        }
        // Jobs remain serialized. This setting controls only how much CPU the
        // one active source decode may use. The budget is chosen when the job
        // starts: a job begun during playback keeps the Background budget.
        const ProxyTranscodePriority priority =
            request.yield_to_playback ? ProxyTranscodePriority::Background
                                      : priority_;
        decode->thread_count = codec_thread_count(priority);
        decode->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        if (avcodec_open2(decode.get(), decoder, nullptr) < 0) {
            return failure(ProxyBackendErrorCode::EncodeFailed);
        }

        const assets::PixelDimensions dimensions =
            fitted_dimensions(metadata->dimensions, request.profile);
        AVCodecContext *raw_encode{};
        const AVCodec *const encoder = open_encoder(
            raw_encode, *metadata, dimensions, request.profile, priority);
        std::unique_ptr<AVCodecContext, CodecCloser> encode{raw_encode};
        if (encoder == nullptr || !encode) {
            return failure(ProxyBackendErrorCode::Unavailable);
        }

        // A profile without audio, such as the editor's 720p proxy, skips
        // the source's audio entirely: nothing decodes or encodes it.
        const bool with_audio =
            metadata->has_audio &&
            request.profile.audio_codec != assets::DerivativeAudioCodec::None;
        const ProxySourceRange range =
            request.range.value_or(ProxySourceRange{0U, metadata->frame_count});
        const bool pieces = request.piece_frames != 0U;
        if (range.count == 0U || range.first >= metadata->frame_count ||
            range.count > metadata->frame_count - range.first ||
            (request.range.has_value() && with_audio) ||
            (pieces && (!request.range.has_value() || !request.piece_done))) {
            // A range of a profile with audio would need the audio cut on
            // the same frames; no such profile is encoded in ranges.
            return failure(ProxyBackendErrorCode::Unavailable);
        }
        const int audio_index =
            with_audio
                ? av_find_best_stream(input.get(), AVMEDIA_TYPE_AUDIO, -1, -1,
                                      nullptr, 0)
                : -1;
        std::unique_ptr<AVCodecContext, CodecCloser> audio_decode;
        std::unique_ptr<AVCodecContext, CodecCloser> audio_encode;
        const AVCodec *audio_encoder{};
        if (with_audio) {
            AVStream &audio_source = *input->streams[audio_index];
            const AVCodec *const audio_decoder =
                avcodec_find_decoder(audio_source.codecpar->codec_id);
            audio_decode.reset(audio_decoder == nullptr
                                   ? nullptr
                                   : avcodec_alloc_context3(audio_decoder));
            if (!audio_decode ||
                avcodec_parameters_to_context(audio_decode.get(),
                                              audio_source.codecpar) < 0 ||
                avcodec_open2(audio_decode.get(), audio_decoder, nullptr) < 0) {
                return failure(ProxyBackendErrorCode::EncodeFailed);
            }
            audio_encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
            audio_encode.reset(audio_encoder == nullptr
                                   ? nullptr
                                   : avcodec_alloc_context3(audio_encoder));
            if (!audio_encode) {
                return failure(ProxyBackendErrorCode::Unavailable);
            }
            audio_encode->sample_rate =
                static_cast<int>(request.profile.audio_sample_rate);
            audio_encode->sample_fmt = AV_SAMPLE_FMT_FLTP;
            av_channel_layout_default(&audio_encode->ch_layout, 2);
            audio_encode->bit_rate = request.profile.audio_bitrate;
            audio_encode->time_base = AVRational{1, audio_encode->sample_rate};
            audio_encode->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if (avcodec_open2(audio_encode.get(), audio_encoder, nullptr) < 0) {
                return failure(ProxyBackendErrorCode::EncodeFailed);
            }
        }

        std::unique_ptr<AVFormatContext, OutputCloser> output;
        AVStream *encoded_stream{};
        AVStream *audio_stream{};
        const auto open_output =
            [&](const std::filesystem::path &path)
            -> std::optional<ProxyBackendErrorCode> {
            AVFormatContext *raw_output{};
            const std::string output_path = path.string();
            if (avformat_alloc_output_context2(&raw_output, nullptr, "mp4",
                                               output_path.c_str()) < 0 ||
                raw_output == nullptr) {
                return ProxyBackendErrorCode::EncodeFailed;
            }
            output.reset(raw_output);
            encoded_stream = avformat_new_stream(output.get(), encoder);
            if (encoded_stream == nullptr ||
                avcodec_parameters_from_context(encoded_stream->codecpar,
                                                encode.get()) < 0) {
                return ProxyBackendErrorCode::AllocationFailed;
            }
            encoded_stream->time_base = encode->time_base;
            encoded_stream->avg_frame_rate = encode->framerate;
            encoded_stream->sample_aspect_ratio = encode->sample_aspect_ratio;
            if (audio_encode) {
                audio_stream =
                    avformat_new_stream(output.get(), audio_encoder);
                if (audio_stream == nullptr ||
                    avcodec_parameters_from_context(audio_stream->codecpar,
                                                    audio_encode.get()) < 0) {
                    return ProxyBackendErrorCode::AllocationFailed;
                }
                audio_stream->time_base = audio_encode->time_base;
            }
            if (avio_open(&output->pb, output_path.c_str(), AVIO_FLAG_WRITE) <
                0) {
                return ProxyBackendErrorCode::EncodeFailed;
            }
            AVDictionary *mux_options{};
            av_dict_set(&mux_options, "movflags", "+faststart", 0);
            const int header =
                avformat_write_header(output.get(), &mux_options);
            av_dict_free(&mux_options);
            if (header < 0) {
                return ProxyBackendErrorCode::EncodeFailed;
            }
            return std::nullopt;
        };
        if (const auto failed = open_output(
                pieces ? proxy_piece_path(request.output_file, 0U)
                       : request.output_file)) {
            return failure(*failed);
        }

        std::unique_ptr<AVPacket, PacketCloser> input_packet{av_packet_alloc()};
        std::unique_ptr<AVPacket, PacketCloser> output_packet{
            av_packet_alloc()};
        std::unique_ptr<AVFrame, FrameCloser> decoded{av_frame_alloc()};
        std::unique_ptr<AVFrame, FrameCloser> converted{av_frame_alloc()};
        if (!input_packet || !output_packet || !decoded || !converted) {
            return failure(ProxyBackendErrorCode::AllocationFailed);
        }
        converted->format = encode->pix_fmt;
        converted->width = encode->width;
        converted->height = encode->height;
        converted->sample_aspect_ratio = encode->sample_aspect_ratio;
        converted->color_primaries = encode->color_primaries;
        converted->color_trc = encode->color_trc;
        converted->colorspace = encode->colorspace;
        converted->color_range = encode->color_range;
        if (av_frame_get_buffer(converted.get(), 32) < 0) {
            return failure(ProxyBackendErrorCode::AllocationFailed);
        }
        std::unique_ptr<SwsContext, ScaleCloser> scaler{
            sws_getContext(decode->width, decode->height, decode->pix_fmt,
                           encode->width, encode->height, encode->pix_fmt,
                           SWS_BILINEAR, nullptr, nullptr, nullptr)};
        if (!scaler) {
            return failure(ProxyBackendErrorCode::EncodeFailed);
        }

        std::unique_ptr<AVFrame, FrameCloser> decoded_audio;
        std::unique_ptr<SwrContext, ResampleCloser> resampler;
        std::unique_ptr<AVAudioFifo, AudioFifoCloser> audio_fifo;
        if (audio_encode) {
            decoded_audio.reset(av_frame_alloc());
            SwrContext *raw_resampler{};
            if (!decoded_audio ||
                swr_alloc_set_opts2(
                    &raw_resampler, &audio_encode->ch_layout,
                    audio_encode->sample_fmt, audio_encode->sample_rate,
                    &audio_decode->ch_layout, audio_decode->sample_fmt,
                    audio_decode->sample_rate, 0, nullptr) < 0) {
                swr_free(&raw_resampler);
                return failure(ProxyBackendErrorCode::AllocationFailed);
            }
            resampler.reset(raw_resampler);
            audio_fifo.reset(
                av_audio_fifo_alloc(audio_encode->sample_fmt,
                                    audio_encode->ch_layout.nb_channels, 1));
            if (!resampler || swr_init(resampler.get()) < 0 || !audio_fifo) {
                return failure(ProxyBackendErrorCode::EncodeFailed);
            }
        }

        core::u64 frame_count{};
        // The piece being written, how many frames it holds, and the output
        // of the one handed over last.
        core::u32 piece{};
        core::u64 piece_packets{};
        bool piece_refused = false;
        std::optional<ProxyBackendOutput> last_piece;
        const auto piece_size = [&](core::u32 index) -> core::u64 {
            const core::u64 first =
                static_cast<core::u64>(index) * request.piece_frames;
            return std::min(request.piece_frames, range.count - first);
        };
        // Closes the current piece, proves it holds exactly its frames, and
        // hands it to the caller.
        const auto finish_piece = [&]() -> bool {
            if (piece_packets != piece_size(piece) ||
                av_write_trailer(output.get()) < 0) {
                return false;
            }
            if (output->pb != nullptr) {
                static_cast<void>(avio_closep(&output->pb));
            }
            output.reset();
            const std::filesystem::path file =
                proxy_piece_path(request.output_file, piece);
            const auto probed =
                probe_output(file, *metadata, request.profile);
            auto map = assets::make_identity_proxy_frame_map(
                piece_packets, metadata->frame_rate,
                range.first +
                    static_cast<core::u64>(piece) * request.piece_frames);
            if (!probed || !map || probed->frame_count != piece_packets) {
                std::cerr << "proxy piece " << piece
                          << " contract validation failed: packets="
                          << piece_packets << " probed="
                          << (probed ? probed->frame_count : 0U) << '\n';
                return false;
            }
            last_piece = ProxyBackendOutput{measured.value(), *metadata,
                                            *probed, std::move(map).value()};
            const bool last =
                static_cast<core::u64>(piece + 1U) * request.piece_frames >=
                range.count;
            if (!request.piece_done(piece, *last_piece) && !last) {
                piece_refused = true;
                return false;
            }
            return true;
        };
        const auto write_packets = [&]() -> bool {
            while (true) {
                const int received =
                    avcodec_receive_packet(encode.get(), output_packet.get());
                if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
                    return true;
                }
                if (received < 0) {
                    return false;
                }
                if (pieces) {
                    // Encoder timestamps count frames of the range. A frame
                    // past this piece must open the next one as an IDR;
                    // anything else would make the next piece undecodable
                    // on its own.
                    const std::int64_t piece_first =
                        static_cast<std::int64_t>(piece) *
                        static_cast<std::int64_t>(request.piece_frames);
                    const std::int64_t piece_end =
                        piece_first +
                        static_cast<std::int64_t>(piece_size(piece));
                    if (output_packet->pts >= piece_end) {
                        if ((output_packet->flags & AV_PKT_FLAG_KEY) == 0 ||
                            output_packet->pts != piece_end ||
                            !finish_piece()) {
                            av_packet_unref(output_packet.get());
                            return false;
                        }
                        ++piece;
                        piece_packets = 0U;
                        if (open_output(proxy_piece_path(request.output_file,
                                                         piece))) {
                            av_packet_unref(output_packet.get());
                            return false;
                        }
                    } else if (output_packet->pts < piece_first) {
                        av_packet_unref(output_packet.get());
                        return false;
                    }
                    // Each piece's timestamps start at zero, as a segment
                    // encoded on its own does.
                    const std::int64_t shift =
                        static_cast<std::int64_t>(piece) *
                        static_cast<std::int64_t>(request.piece_frames);
                    output_packet->pts -= shift;
                    if (output_packet->dts != AV_NOPTS_VALUE) {
                        output_packet->dts -= shift;
                    }
                    ++piece_packets;
                }
                av_packet_rescale_ts(output_packet.get(), encode->time_base,
                                     encoded_stream->time_base);
                output_packet->stream_index = encoded_stream->index;
                const int written = av_interleaved_write_frame(
                    output.get(), output_packet.get());
                av_packet_unref(output_packet.get());
                if (written < 0) {
                    return false;
                }
            }
        };
        const auto convert_and_send = [&]() -> bool {
            if (stop.stop_requested() ||
                frame_count >= static_cast<core::u64>(
                                   std::numeric_limits<std::int64_t>::max()) ||
                av_frame_make_writable(converted.get()) < 0) {
                return false;
            }
            const int scaled = sws_scale(scaler.get(), decoded->data,
                                         decoded->linesize, 0, decoded->height,
                                         converted->data, converted->linesize);
            if (scaled != converted->height) {
                return false;
            }
            converted->pts = static_cast<std::int64_t>(frame_count);
            converted->pict_type =
                pieces && frame_count != 0U &&
                        frame_count % request.piece_frames == 0U
                    ? AV_PICTURE_TYPE_I
                    : AV_PICTURE_TYPE_NONE;
            ++frame_count;
            if (frame_count == range.count || frame_count % 8U == 0U) {
                report_progress(request, frame_count, range.count);
            }
            if (avcodec_send_frame(encode.get(), converted.get()) < 0) {
                return false;
            }
            return write_packets();
        };
        // A range numbers each decoded frame from its timestamp, exactly as
        // the playback decoder does, so proxy frame j is the picture the
        // source shows as frame first + j. A whole-file encode counts frames.
        const std::int64_t start_pts = source_stream.start_time == AV_NOPTS_VALUE
            ? 0 : source_stream.start_time;
        const AVRational frame_duration{
            static_cast<int>(metadata->frame_rate.denominator),
            static_cast<int>(metadata->frame_rate.numerator)};
        bool range_complete = false;
        const auto source_index = [&]() -> std::optional<core::u64> {
            const std::int64_t pts =
                decoded->best_effort_timestamp == AV_NOPTS_VALUE
                    ? decoded->pts : decoded->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) {
                return std::nullopt;
            }
            const std::int64_t index = av_rescale_q_rnd(
                pts - start_pts, source_stream.time_base, frame_duration,
                static_cast<AVRounding>(AV_ROUND_NEAR_INF |
                                        AV_ROUND_PASS_MINMAX));
            return static_cast<core::u64>(std::max<std::int64_t>(index, 0));
        };
        const auto drain_decoder = [&]() -> bool {
            while (true) {
                const int received =
                    avcodec_receive_frame(decode.get(), decoded.get());
                if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
                    return true;
                }
                if (received < 0) {
                    return false;
                }
                if (request.range.has_value()) {
                    const auto index = source_index();
                    if (!index.has_value()) {
                        return false;
                    }
                    if (*index < range.first || range_complete) {
                        av_frame_unref(decoded.get());
                        continue;
                    }
                    // A frame the seek skipped past is a gap; the range
                    // fails rather than mislabel what follows.
                    if (*index != range.first + frame_count) {
                        return false;
                    }
                }
                if (!convert_and_send()) {
                    return false;
                }
                // A whole-file encode reads to the end, so trailing audio
                // packets are still muxed.
                range_complete =
                    request.range.has_value() && frame_count == range.count;
                av_frame_unref(decoded.get());
            }
        };
        if (range.first != 0U) {
            const std::int64_t target =
                start_pts + av_rescale_q(static_cast<std::int64_t>(range.first),
                                         frame_duration,
                                         source_stream.time_base);
            if (av_seek_frame(input.get(), video_index, target,
                              AVSEEK_FLAG_BACKWARD) < 0) {
                return failure(ProxyBackendErrorCode::EncodeFailed);
            }
            avcodec_flush_buffers(decode.get());
        }

        std::int64_t encoded_audio_samples{};
        const auto write_audio_packets = [&]() -> bool {
            while (true) {
                const int received = avcodec_receive_packet(
                    audio_encode.get(), output_packet.get());
                if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
                    return true;
                }
                if (received < 0) {
                    return false;
                }
                av_packet_rescale_ts(output_packet.get(),
                                     audio_encode->time_base,
                                     audio_stream->time_base);
                output_packet->stream_index = audio_stream->index;
                const int written = av_interleaved_write_frame(
                    output.get(), output_packet.get());
                av_packet_unref(output_packet.get());
                if (written < 0) {
                    return false;
                }
            }
        };
        const auto encode_audio_fifo = [&](bool flushing) -> bool {
            if (!audio_encode) {
                return true;
            }
            const int frame_samples =
                audio_encode->frame_size > 0 ? audio_encode->frame_size : 1'024;
            while (av_audio_fifo_size(audio_fifo.get()) >= frame_samples ||
                   (flushing && av_audio_fifo_size(audio_fifo.get()) > 0)) {
                const int available = av_audio_fifo_size(audio_fifo.get());
                const int read_samples = std::min(available, frame_samples);
                std::unique_ptr<AVFrame, FrameCloser> frame{av_frame_alloc()};
                if (!frame) {
                    return false;
                }
                frame->format = audio_encode->sample_fmt;
                frame->sample_rate = audio_encode->sample_rate;
                frame->nb_samples = frame_samples;
                if (av_channel_layout_copy(&frame->ch_layout,
                                           &audio_encode->ch_layout) < 0 ||
                    av_frame_get_buffer(frame.get(), 0) < 0 ||
                    av_frame_make_writable(frame.get()) < 0 ||
                    av_samples_set_silence(
                        frame->data, 0, frame_samples,
                        frame->ch_layout.nb_channels,
                        static_cast<AVSampleFormat>(frame->format)) < 0 ||
                    av_audio_fifo_read(audio_fifo.get(),
                                       reinterpret_cast<void **>(frame->data),
                                       read_samples) != read_samples) {
                    return false;
                }
                frame->pts = encoded_audio_samples;
                encoded_audio_samples += frame_samples;
                if (avcodec_send_frame(audio_encode.get(), frame.get()) < 0 ||
                    !write_audio_packets()) {
                    return false;
                }
            }
            return true;
        };
        const auto resample_audio = [&](const AVFrame *source) -> bool {
            if (!audio_encode) {
                return true;
            }
            const int input_samples =
                source == nullptr ? 0 : source->nb_samples;
            const int capacity =
                swr_get_out_samples(resampler.get(), input_samples);
            if (capacity < 0) {
                return false;
            }
            if (capacity == 0) {
                return true;
            }
            std::unique_ptr<AVFrame, FrameCloser> converted_audio{
                av_frame_alloc()};
            if (!converted_audio) {
                return false;
            }
            converted_audio->format = audio_encode->sample_fmt;
            converted_audio->sample_rate = audio_encode->sample_rate;
            converted_audio->nb_samples = capacity;
            if (av_channel_layout_copy(&converted_audio->ch_layout,
                                       &audio_encode->ch_layout) < 0 ||
                av_frame_get_buffer(converted_audio.get(), 0) < 0) {
                return false;
            }
            const core::u8 **source_data =
                source == nullptr
                    ? nullptr
                    : const_cast<const core::u8 **>(source->extended_data);
            const int produced =
                swr_convert(resampler.get(), converted_audio->data, capacity,
                            source_data, input_samples);
            if (produced < 0 ||
                av_audio_fifo_realloc(audio_fifo.get(),
                                      av_audio_fifo_size(audio_fifo.get()) +
                                          produced) < 0 ||
                av_audio_fifo_write(
                    audio_fifo.get(),
                    reinterpret_cast<void **>(converted_audio->data),
                    produced) != produced) {
                return false;
            }
            return encode_audio_fifo(false);
        };
        const auto drain_audio_decoder = [&]() -> bool {
            while (true) {
                const int received = avcodec_receive_frame(audio_decode.get(),
                                                           decoded_audio.get());
                if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
                    return true;
                }
                if (received < 0 || !resample_audio(decoded_audio.get())) {
                    return false;
                }
                av_frame_unref(decoded_audio.get());
            }
        };

        while (!range_complete &&
               av_read_frame(input.get(), input_packet.get()) >= 0) {
            if (stop.stop_requested()) {
                return failure(ProxyBackendErrorCode::Cancelled);
            }
            if (input_packet->stream_index == video_index &&
                (avcodec_send_packet(decode.get(), input_packet.get()) < 0 ||
                 !drain_decoder())) {
                return failure(piece_refused || stop.stop_requested()
                                   ? ProxyBackendErrorCode::Cancelled
                                   : ProxyBackendErrorCode::EncodeFailed);
            }
            if (audio_decode && input_packet->stream_index == audio_index &&
                (avcodec_send_packet(audio_decode.get(), input_packet.get()) <
                     0 ||
                 !drain_audio_decoder())) {
                return failure(ProxyBackendErrorCode::EncodeFailed);
            }
            av_packet_unref(input_packet.get());
        }
        const bool audio_finished =
            !audio_decode ||
            (avcodec_send_packet(audio_decode.get(), nullptr) >= 0 &&
             drain_audio_decoder() && resample_audio(nullptr) &&
             encode_audio_fifo(true) &&
             avcodec_send_frame(audio_encode.get(), nullptr) >= 0 &&
             write_audio_packets());
        if (avcodec_send_packet(decode.get(), nullptr) < 0 ||
            !drain_decoder() || avcodec_send_frame(encode.get(), nullptr) < 0 ||
            !write_packets() || !audio_finished ||
            frame_count != range.count) {
            return failure(stop.stop_requested() || piece_refused
                               ? ProxyBackendErrorCode::Cancelled
                               : ProxyBackendErrorCode::OutputValidationFailed);
        }
        if (pieces) {
            if (!finish_piece() || !last_piece.has_value()) {
                return failure(ProxyBackendErrorCode::OutputValidationFailed);
            }
            return ProxyBackendResult::success(std::move(*last_piece));
        }
        if (av_write_trailer(output.get()) < 0) {
            return failure(ProxyBackendErrorCode::OutputValidationFailed);
        }
        if (output->pb != nullptr) {
            static_cast<void>(avio_closep(&output->pb));
        }
        output.reset();

        const auto encoded =
            probe_output(request.output_file, *metadata, request.profile);
        auto frame_map = assets::make_identity_proxy_frame_map(
            frame_count, metadata->frame_rate, range.first);
        if (!encoded || !frame_map || encoded->frame_count != frame_count) {
            std::cerr << "proxy output contract validation failed: decoded="
                      << frame_count
                      << " probed=" << (encoded ? encoded->frame_count : 0U)
                      << " map=" << (frame_map.has_value() ? 1 : 0) << '\n';
            return failure(ProxyBackendErrorCode::OutputValidationFailed);
        }
        return ProxyBackendResult::success(ProxyBackendOutput{
            measured.value(),
            *metadata,
            *encoded,
            std::move(frame_map).value(),
        });
    }

  private:
    // The whole-file proxy from its segments, by copying their packets into
    // one MP4 one after another. Nothing is decoded or encoded, so it takes
    // as long as reading and writing the bytes. Segments made by different
    // encoders, whose stream headers differ, cannot be joined; the caller
    // then encodes the whole file instead.
    ProxyBackendResult join(const ProxyEncodeRequest &request,
                            const ProxySourceMetadata &metadata,
                            const assets::ContentFingerprint &measured,
                            std::stop_token stop) const {
        if (request.range.has_value() ||
            request.profile.audio_codec != assets::DerivativeAudioCodec::None) {
            return failure(ProxyBackendErrorCode::Unavailable);
        }
        const std::string output_path = request.output_file.string();
        AVFormatContext *raw_output{};
        if (avformat_alloc_output_context2(&raw_output, nullptr, "mp4",
                                           output_path.c_str()) < 0 ||
            raw_output == nullptr) {
            return failure(ProxyBackendErrorCode::EncodeFailed);
        }
        std::unique_ptr<AVFormatContext, OutputCloser> output{raw_output};
        std::unique_ptr<AVPacket, PacketCloser> packet{av_packet_alloc()};
        if (!packet) {
            return failure(ProxyBackendErrorCode::AllocationFailed);
        }
        AVStream *joined{};
        std::vector<core::u8> extradata;
        std::int64_t offset{};
        core::u64 frames{};
        for (const std::filesystem::path &segment : request.join_segments) {
            if (stop.stop_requested()) {
                return failure(ProxyBackendErrorCode::Cancelled);
            }
            AVFormatContext *raw_input{};
            const std::string segment_path = segment.string();
            if (avformat_open_input(&raw_input, segment_path.c_str(), nullptr,
                                    nullptr) < 0 ||
                raw_input == nullptr) {
                return failure(ProxyBackendErrorCode::SourceUnavailable);
            }
            std::unique_ptr<AVFormatContext, InputCloser> input{raw_input};
            if (avformat_find_stream_info(input.get(), nullptr) < 0) {
                return failure(ProxyBackendErrorCode::MetadataUnavailable);
            }
            const int index = av_find_best_stream(
                input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (index < 0) {
                return failure(ProxyBackendErrorCode::MetadataUnavailable);
            }
            const AVStream &stream = *input->streams[index];
            const std::vector<core::u8> headers(
                stream.codecpar->extradata,
                stream.codecpar->extradata + stream.codecpar->extradata_size);
            if (joined == nullptr) {
                joined = avformat_new_stream(output.get(), nullptr);
                if (joined == nullptr ||
                    avcodec_parameters_copy(joined->codecpar,
                                            stream.codecpar) < 0) {
                    return failure(ProxyBackendErrorCode::AllocationFailed);
                }
                joined->codecpar->codec_tag = 0;
                joined->time_base = stream.time_base;
                joined->avg_frame_rate = stream.avg_frame_rate;
                joined->sample_aspect_ratio = stream.sample_aspect_ratio;
                extradata = headers;
                if (avio_open(&output->pb, output_path.c_str(),
                              AVIO_FLAG_WRITE) < 0) {
                    return failure(ProxyBackendErrorCode::EncodeFailed);
                }
                AVDictionary *mux_options{};
                av_dict_set(&mux_options, "movflags", "+faststart", 0);
                const int header =
                    avformat_write_header(output.get(), &mux_options);
                av_dict_free(&mux_options);
                if (header < 0) {
                    return failure(ProxyBackendErrorCode::EncodeFailed);
                }
            } else if (headers != extradata ||
                       stream.codecpar->width != joined->codecpar->width ||
                       stream.codecpar->height != joined->codecpar->height) {
                return failure(ProxyBackendErrorCode::OutputValidationFailed);
            }
            // Each segment's timestamps start at zero; shifted by the frames
            // before it, the joined file numbers its frames as the source.
            const AVRational frame_duration = av_inv_q(
                stream.avg_frame_rate.num > 0 ? stream.avg_frame_rate
                                              : stream.r_frame_rate);
            const std::int64_t shift =
                av_rescale_q(offset, frame_duration, joined->time_base);
            std::int64_t segment_frames{};
            while (av_read_frame(input.get(), packet.get()) >= 0) {
                if (packet->stream_index != index) {
                    av_packet_unref(packet.get());
                    continue;
                }
                av_packet_rescale_ts(packet.get(), stream.time_base,
                                     joined->time_base);
                if (packet->pts != AV_NOPTS_VALUE) {
                    packet->pts += shift;
                }
                if (packet->dts != AV_NOPTS_VALUE) {
                    packet->dts += shift;
                }
                packet->stream_index = joined->index;
                packet->pos = -1;
                ++segment_frames;
                if (av_interleaved_write_frame(output.get(), packet.get()) <
                    0) {
                    return failure(
                        ProxyBackendErrorCode::OutputValidationFailed);
                }
            }
            offset += segment_frames;
            frames += static_cast<core::u64>(segment_frames);
            report_progress(request, frames, metadata.frame_count);
        }
        if (joined == nullptr || frames != metadata.frame_count ||
            av_write_trailer(output.get()) < 0) {
            return failure(ProxyBackendErrorCode::OutputValidationFailed);
        }
        if (output->pb != nullptr) {
            static_cast<void>(avio_closep(&output->pb));
        }
        output.reset();
        const auto encoded =
            probe_output(request.output_file, metadata, request.profile);
        auto frame_map = assets::make_identity_proxy_frame_map(
            frames, metadata.frame_rate);
        if (!encoded || !frame_map || encoded->frame_count != frames) {
            return failure(ProxyBackendErrorCode::OutputValidationFailed);
        }
        return ProxyBackendResult::success(ProxyBackendOutput{
            measured,
            metadata,
            *encoded,
            std::move(frame_map).value(),
        });
    }

    static void report_progress(const ProxyEncodeRequest &request,
                                core::u64 completed, core::u64 total) noexcept {
        if (!request.progress) {
            return;
        }
        try {
            request.progress(std::min(completed, total), total);
        } catch (...) {
            // Progress is observational. A UI callback must never invalidate
            // an otherwise valid proxy encode.
        }
    }

    ProxyTranscodePriority priority_{ProxyTranscodePriority::Background};
};

} // namespace

std::unique_ptr<ProxyEncoderBackend>
ffmpeg_proxy_encoder_backend(ProxyTranscodePriority priority) {
    return std::make_unique<FfmpegProxyEncoder>(priority);
}

} // namespace mediaserver::server
