#include "ffmpeg_waveform_decoder.hpp"

#include <mediaserver/assets/asset_path.hpp>
#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/assets/derivative.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace {

namespace assets = mediaserver::assets;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace fs = std::filesystem;
namespace server = mediaserver::server;

#define DECODER_CHECK(condition)                                               \
  do {                                                                         \
    if (!(condition)) {                                                        \
      throw std::runtime_error{"check failed: " #condition};                 \
    }                                                                          \
  } while (false)

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = fs::temp_directory_path() /
            ("mediaserver_waveform_ffmpeg_" + std::to_string(stamp));
    std::error_code error;
    if (!fs::create_directory(path_, error)) {
      throw std::runtime_error{"could not create temporary directory"};
    }
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    static_cast<void>(fs::remove_all(path_, ignored));
  }
  [[nodiscard]] const fs::path &path() const noexcept { return path_; }

private:
  fs::path path_;
};

void append_u16(std::vector<core::u8> &bytes, core::u16 value) {
  bytes.push_back(static_cast<core::u8>(value & 0xffU));
  bytes.push_back(static_cast<core::u8>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<core::u8> &bytes, core::u32 value) {
  for (core::u32 shift = 0U; shift < 32U; shift += 8U) {
    bytes.push_back(static_cast<core::u8>((value >> shift) & 0xffU));
  }
}

void append_tag(std::vector<core::u8> &bytes, std::string_view tag) {
  DECODER_CHECK(tag.size() == 4U);
  bytes.insert(bytes.end(), tag.begin(), tag.end());
}

[[nodiscard]] std::vector<std::int16_t> write_wav(const fs::path &path) {
  std::vector<std::int16_t> samples(256U);
  for (std::size_t index = 0U; index < samples.size(); ++index) {
    const int phase = static_cast<int>(index % 32U) - 16;
    samples[index] = static_cast<std::int16_t>(phase * 1'500);
  }
  const core::u32 data_bytes =
      static_cast<core::u32>(samples.size() * sizeof(std::int16_t));
  std::vector<core::u8> bytes;
  bytes.reserve(44U + data_bytes);
  append_tag(bytes, "RIFF");
  append_u32(bytes, 36U + data_bytes);
  append_tag(bytes, "WAVE");
  append_tag(bytes, "fmt ");
  append_u32(bytes, 16U);
  append_u16(bytes, 1U);
  append_u16(bytes, 1U);
  append_u32(bytes, 8'000U);
  append_u32(bytes, 16'000U);
  append_u16(bytes, 2U);
  append_u16(bytes, 16U);
  append_tag(bytes, "data");
  append_u32(bytes, data_bytes);
  for (const std::int16_t sample : samples) {
    append_u16(bytes, static_cast<core::u16>(sample));
  }
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  output.close();
  DECODER_CHECK(static_cast<bool>(output));
  return samples;
}

[[nodiscard]] server::WaveformDecodeRequest request_for(
    const fs::path &path) {
  const std::string source = assets::asset_source_from_path(path);
  auto fingerprint = assets::source_fingerprint(source);
  DECODER_CHECK(fingerprint);
  const assets::DerivativeProfile *const profile = assets::derivative_profile(
      assets::DerivativeProfileId::Waveform2048PeakRms);
  DECODER_CHECK(profile != nullptr);
  return server::WaveformDecodeRequest{
      doc::NodeId{7U},
      source,
      fingerprint.value(),
      assets::make_derivative_key(doc::NodeId{7U}, fingerprint.value(),
                                  *profile),
      *profile,
      4'096U,
  };
}

void decodes_exact_pcm_and_attests_the_source_revision() {
  TemporaryDirectory temporary;
  const fs::path source = temporary.path() / "tone.wav";
  const std::vector<std::int16_t> expected = write_wav(source);
  const server::WaveformDecodeRequest request = request_for(source);
  auto backend = server::ffmpeg_waveform_decoder_factory()();
  DECODER_CHECK(backend != nullptr);
  auto decoded = backend->decode(request, {});
  DECODER_CHECK(decoded);
  DECODER_CHECK(decoded.value().channel_count == 1U);
  DECODER_CHECK(decoded.value().interleaved_samples.size() == expected.size());
  DECODER_CHECK(decoded.value().decoded_source_fingerprint ==
                request.source_fingerprint);
  for (std::size_t index = 0U; index < expected.size(); ++index) {
    const float wanted = static_cast<float>(expected[index]) / 32'768.0F;
    DECODER_CHECK(std::abs(decoded.value().interleaved_samples[index] -
                           wanted) < 1.0e-5F);
  }
}

void cancellation_and_sources_without_audio_fail_visibly() {
  TemporaryDirectory temporary;
  const fs::path source = temporary.path() / "tone.wav";
  static_cast<void>(write_wav(source));
  const server::WaveformDecodeRequest request = request_for(source);
  auto backend = server::ffmpeg_waveform_decoder_factory()();
  std::stop_source stopped;
  stopped.request_stop();
  const auto cancelled = backend->decode(request, stopped.get_token());
  DECODER_CHECK(!cancelled);
  DECODER_CHECK(cancelled.error().code ==
                server::WaveformBackendErrorCode::Cancelled);

  const fs::path text = temporary.path() / "not-audio.bin";
  std::ofstream output{text, std::ios::binary | std::ios::trunc};
  output << "not an audio container";
  output.close();
  const auto invalid = backend->decode(request_for(text), {});
  DECODER_CHECK(!invalid);
  DECODER_CHECK(invalid.error().code ==
                    server::WaveformBackendErrorCode::SourceUnavailable ||
                invalid.error().code ==
                    server::WaveformBackendErrorCode::DecodeFailed);
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 2U> cases{{
    {"decodes exact PCM and attests the source revision",
     &decodes_exact_pcm_and_attests_the_source_revision},
    {"cancellation and sources without audio fail visibly",
     &cancellation_and_sources_without_audio_fail_visibly},
}};

} // namespace

int main() {
  std::size_t failures{};
  for (const Case &entry : cases) {
    try {
      entry.function();
      std::cout << "[pass] " << entry.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
    }
  }
  std::cout << (cases.size() - failures) << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
