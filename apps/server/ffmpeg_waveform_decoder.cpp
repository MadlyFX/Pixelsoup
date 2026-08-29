#include "ffmpeg_waveform_decoder.hpp"

#include <mediaserver/assets/content_fingerprint.hpp>

// FFmpeg is a C library and its public headers are not clean under this
// project's warnings-as-errors policy. Keep the suppression scoped to the
// headers; the decoder implementation below remains fully checked.
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
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
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
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace mediaserver::server {
namespace {

[[nodiscard]] WaveformBackendResult failure(
    WaveformBackendErrorCode code) noexcept {
  return WaveformBackendResult::failure(WaveformBackendError{code, {}});
}

struct FormatCloser final {
  void operator()(AVFormatContext *value) const noexcept {
    if (value != nullptr) {
      avformat_close_input(&value);
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

struct SwrCloser final {
  void operator()(SwrContext *value) const noexcept { swr_free(&value); }
};

class FfmpegWaveformDecoder final : public WaveformDecoderBackend {
public:
  WaveformBackendResult decode(const WaveformDecodeRequest &request,
                               std::stop_token stop) override {
    if (stop.stop_requested()) {
      return failure(WaveformBackendErrorCode::Cancelled);
    }
    if (request.source.empty() || request.maximum_interleaved_samples == 0U) {
      return failure(WaveformBackendErrorCode::SourceUnavailable);
    }
    try {
      AVFormatContext *opened = nullptr;
      if (avformat_open_input(&opened, request.source.c_str(), nullptr,
                              nullptr) < 0 ||
          opened == nullptr) {
        return failure(WaveformBackendErrorCode::SourceUnavailable);
      }
      std::unique_ptr<AVFormatContext, FormatCloser> format{opened};
      if (avformat_find_stream_info(format.get(), nullptr) < 0) {
        return failure(WaveformBackendErrorCode::DecodeFailed);
      }
      const int stream_index = av_find_best_stream(
          format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
      if (stream_index < 0) {
        return failure(WaveformBackendErrorCode::DecodeFailed);
      }
      AVStream *const stream = format->streams[stream_index];
      const AVCodec *const codec =
          avcodec_find_decoder(stream->codecpar->codec_id);
      if (codec == nullptr) {
        return failure(WaveformBackendErrorCode::DecodeFailed);
      }
      std::unique_ptr<AVCodecContext, CodecCloser> context{
          avcodec_alloc_context3(codec)};
      if (context == nullptr ||
          avcodec_parameters_to_context(context.get(), stream->codecpar) < 0 ||
          avcodec_open2(context.get(), codec, nullptr) < 0 ||
          context->sample_rate <= 0 || context->ch_layout.nb_channels <= 0 ||
          context->ch_layout.nb_channels >
              static_cast<int>(assets::maximum_waveform_channels)) {
        return failure(WaveformBackendErrorCode::DecodeFailed);
      }
      const core::u32 channels =
          static_cast<core::u32>(context->ch_layout.nb_channels);

      AVChannelLayout output_layout{};
      if (av_channel_layout_copy(&output_layout, &context->ch_layout) < 0) {
        return failure(WaveformBackendErrorCode::AllocationFailed);
      }
      SwrContext *raw_resampler = nullptr;
      const int configured = swr_alloc_set_opts2(
          &raw_resampler, &output_layout, AV_SAMPLE_FMT_FLT,
          context->sample_rate, &context->ch_layout, context->sample_fmt,
          context->sample_rate, 0, nullptr);
      av_channel_layout_uninit(&output_layout);
      std::unique_ptr<SwrContext, SwrCloser> resampler{raw_resampler};
      if (configured < 0 || resampler == nullptr ||
          swr_init(resampler.get()) < 0) {
        return failure(WaveformBackendErrorCode::DecodeFailed);
      }

      std::unique_ptr<AVPacket, PacketCloser> packet{av_packet_alloc()};
      std::unique_ptr<AVFrame, FrameCloser> frame{av_frame_alloc()};
      if (packet == nullptr || frame == nullptr) {
        return failure(WaveformBackendErrorCode::AllocationFailed);
      }
      std::vector<float> samples;
      const core::u64 modest_reserve = std::min<core::u64>(
          request.maximum_interleaved_samples, 1U << 20U);
      samples.reserve(static_cast<std::size_t>(modest_reserve));

      const auto append_samples = [&](const AVFrame *decoded)
          -> WaveformBackendErrorCode {
        if (stop.stop_requested()) {
          return WaveformBackendErrorCode::Cancelled;
        }
        const int input_samples = decoded == nullptr ? 0 : decoded->nb_samples;
        const int capacity =
            swr_get_out_samples(resampler.get(), input_samples);
        if (capacity < 0 ||
            static_cast<core::u64>(capacity) >
                request.maximum_interleaved_samples / channels) {
          return WaveformBackendErrorCode::DecodeFailed;
        }
        if (capacity == 0) {
          return WaveformBackendErrorCode::Unavailable;
        }
        std::vector<float> converted(
            static_cast<std::size_t>(capacity) * channels);
        std::array<core::u8 *, 1U> output{{
            reinterpret_cast<core::u8 *>(converted.data())}};
        const core::u8 **input =
            decoded == nullptr
                ? nullptr
                : const_cast<const core::u8 **>(decoded->extended_data);
        const int produced = swr_convert(
            resampler.get(), output.data(), capacity, input,
            input_samples);
        if (produced < 0) {
          return WaveformBackendErrorCode::DecodeFailed;
        }
        const core::u64 produced_samples =
            static_cast<core::u64>(produced) * channels;
        if (produced_samples > request.maximum_interleaved_samples -
                                   static_cast<core::u64>(samples.size())) {
          return WaveformBackendErrorCode::DecodeFailed;
        }
        converted.resize(static_cast<std::size_t>(produced_samples));
        samples.insert(samples.end(), converted.begin(), converted.end());
        return WaveformBackendErrorCode::Unavailable;
      };

      const auto drain = [&](bool flushing) -> WaveformBackendErrorCode {
        while (true) {
          const int received = avcodec_receive_frame(context.get(), frame.get());
          if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
            return WaveformBackendErrorCode::Unavailable;
          }
          if (received < 0) {
            return WaveformBackendErrorCode::DecodeFailed;
          }
          const WaveformBackendErrorCode appended = append_samples(frame.get());
          av_frame_unref(frame.get());
          if (appended != WaveformBackendErrorCode::Unavailable) {
            return appended;
          }
          if (!flushing && stop.stop_requested()) {
            return WaveformBackendErrorCode::Cancelled;
          }
        }
      };

      int read_result{};
      while ((read_result = av_read_frame(format.get(), packet.get())) >= 0) {
        if (stop.stop_requested()) {
          return failure(WaveformBackendErrorCode::Cancelled);
        }
        if (packet->stream_index == stream_index) {
          int sent{};
          while ((sent = avcodec_send_packet(context.get(), packet.get())) ==
                 AVERROR(EAGAIN)) {
            const WaveformBackendErrorCode drained = drain(false);
            if (drained != WaveformBackendErrorCode::Unavailable) {
              av_packet_unref(packet.get());
              return failure(drained);
            }
          }
          av_packet_unref(packet.get());
          if (sent < 0) {
            return failure(WaveformBackendErrorCode::DecodeFailed);
          }
          const WaveformBackendErrorCode drained = drain(false);
          if (drained != WaveformBackendErrorCode::Unavailable) {
            return failure(drained);
          }
        } else {
          av_packet_unref(packet.get());
        }
      }
      if (read_result != AVERROR_EOF) {
        return failure(WaveformBackendErrorCode::DecodeFailed);
      }
      int flush_result{};
      while ((flush_result = avcodec_send_packet(context.get(), nullptr)) ==
             AVERROR(EAGAIN)) {
        const WaveformBackendErrorCode intermediate = drain(false);
        if (intermediate != WaveformBackendErrorCode::Unavailable) {
          return failure(intermediate);
        }
      }
      if (flush_result < 0 && flush_result != AVERROR_EOF) {
        return failure(WaveformBackendErrorCode::DecodeFailed);
      }
      const WaveformBackendErrorCode drained = drain(true);
      if (drained != WaveformBackendErrorCode::Unavailable) {
        return failure(drained);
      }
      while (swr_get_delay(resampler.get(), context->sample_rate) > 0) {
        const std::size_t before = samples.size();
        const WaveformBackendErrorCode appended = append_samples(nullptr);
        if (appended != WaveformBackendErrorCode::Unavailable) {
          return failure(appended);
        }
        if (samples.size() == before) {
          break;
        }
      }
      if (samples.empty()) {
        return failure(WaveformBackendErrorCode::DecodeFailed);
      }
      if (stop.stop_requested()) {
        return failure(WaveformBackendErrorCode::Cancelled);
      }
      auto fingerprint = assets::content_fingerprint(request.source, stop);
      if (!fingerprint) {
        return failure(
            fingerprint.error().code ==
                    assets::ContentFingerprintErrorCode::Cancelled
                ? WaveformBackendErrorCode::Cancelled
                : WaveformBackendErrorCode::SourceUnavailable);
      }
      return WaveformBackendResult::success(WaveformDecodedPcm{
          std::move(fingerprint).value(), channels, std::move(samples)});
    } catch (const std::bad_alloc &) {
      return failure(WaveformBackendErrorCode::AllocationFailed);
    }
  }
};

} // namespace

WaveformDecoderFactory ffmpeg_waveform_decoder_factory() {
  return [] { return std::make_unique<FfmpegWaveformDecoder>(); };
}

} // namespace mediaserver::server
