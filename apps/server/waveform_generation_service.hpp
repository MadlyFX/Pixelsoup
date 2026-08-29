#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/assets/derivative.hpp>
#include <mediaserver/assets/derivative_cache.hpp>
#include <mediaserver/assets/waveform.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

namespace mediaserver::server {

enum class WaveformRequestPriority : core::u8 {
  Visible,
  Prefetch,
};

struct WaveformDecodeRequest final {
  // The persistent Library identity and its exact authored revision. A
  // backend may decode this source, but it cannot substitute another asset
  // id or derivative profile.
  doc::NodeId asset{};
  std::string source;
  assets::ContentFingerprint source_fingerprint;
  assets::DerivativeKey key;
  assets::DerivativeProfile profile;
  core::u64 maximum_interleaved_samples{};
};

struct WaveformDecodedPcm final {
  // Fingerprint of the exact source bytes consumed by the decoder. The
  // service also verifies the authored path before and after decode.
  assets::ContentFingerprint decoded_source_fingerprint;
  core::u32 channel_count{};
  std::vector<float> interleaved_samples;
};

enum class WaveformBackendErrorCode : core::u8 {
  Unavailable,
  SourceUnavailable,
  DecodeFailed,
  Cancelled,
  AllocationFailed,
};

struct WaveformBackendError final {
  WaveformBackendErrorCode code{WaveformBackendErrorCode::Unavailable};
  std::error_code system_error;

  bool operator==(const WaveformBackendError &) const = default;
};

using WaveformBackendResult =
    core::Result<WaveformDecodedPcm, WaveformBackendError>;

class WaveformDecoderBackend {
public:
  WaveformDecoderBackend() = default;
  WaveformDecoderBackend(const WaveformDecoderBackend &) = delete;
  WaveformDecoderBackend &operator=(const WaveformDecoderBackend &) = delete;
  virtual ~WaveformDecoderBackend() = default;

  // Long decodes must observe stop. On success the result is finite
  // interleaved binary32 PCM from the exact requested source revision.
  [[nodiscard]] virtual WaveformBackendResult
  decode(const WaveformDecodeRequest &request, std::stop_token stop) = 0;
};

using WaveformDecoderFactory =
    std::function<std::unique_ptr<WaveformDecoderBackend>()>;

// MediaSource currently exposes video pixels only. The production default is
// therefore explicitly unavailable and never fabricates PCM; deployments may
// inject a real audio decoder without changing cache or asset identity.
[[nodiscard]] WaveformDecoderFactory unavailable_waveform_decoder_factory();

enum class WaveformFailureCode : core::u8 {
  None,
  QueueCapacity,
  SourceUnavailable,
  FingerprintMismatch,
  BackendUnavailable,
  BackendFailure,
  InvalidPcm,
  CacheFailure,
  InvalidCachedPayload,
};

struct WaveformSnapshot final {
  assets::DerivativeKey key;
  assets::DerivativeState state{assets::DerivativeState::Missing};
  WaveformRequestPriority priority{WaveformRequestPriority::Visible};
  WaveformFailureCode failure{WaveformFailureCode::None};
  std::optional<WaveformBackendError> backend_error;
  std::optional<assets::DerivativeCacheErrorCode> cache_error;
  std::optional<assets::WaveformError> waveform_error;
  core::u64 source_frames{};
  core::u64 interleaved_samples{};
  core::u64 revision{};
  std::filesystem::path cache_path;
};

struct WaveformGenerationConfig final {
  core::u32 worker_count{2U};
  core::u32 maximum_pending_requests{32U};
  core::u32 maximum_prefetch_requests{16U};
  core::u32 maximum_snapshots{512U};
  core::u64 maximum_interleaved_samples{32ULL * 1'024ULL * 1'024ULL};
};

inline constexpr core::u32 maximum_waveform_workers = 8U;
inline constexpr core::u32 maximum_waveform_pending_requests = 256U;
inline constexpr core::u32 maximum_waveform_snapshots = 4'096U;
inline constexpr core::u64 maximum_waveform_interleaved_samples =
    256ULL * 1'024ULL * 1'024ULL;

struct WaveformGenerationStats final {
  core::u64 queued{};
  core::u64 generating{};
  core::u64 ready{};
  core::u64 stale{};
  core::u64 failed{};
  core::u64 cancelled{};
  core::u32 workers{};
  core::u32 pending_limit{};
  core::u32 prefetch_limit{};

  bool operator==(const WaveformGenerationStats &) const = default;
};

enum class WaveformServiceErrorCode : core::u8 {
  InvalidAsset,
  MissingFingerprint,
  ProfileUnavailable,
  AllocationFailed,
};

struct WaveformServiceError final {
  WaveformServiceErrorCode code{WaveformServiceErrorCode::InvalidAsset};

  bool operator==(const WaveformServiceError &) const = default;
};

using WaveformRequestResult =
    core::Result<WaveformSnapshot, WaveformServiceError>;

class WaveformGenerationService final {
public:
  explicit WaveformGenerationService(assets::DerivativeCache cache,
                                     WaveformGenerationConfig config = {});
  WaveformGenerationService(assets::DerivativeCache cache,
                            WaveformGenerationConfig config,
                            WaveformDecoderFactory decoder_factory);
  ~WaveformGenerationService();

  WaveformGenerationService(const WaveformGenerationService &) = delete;
  WaveformGenerationService &
  operator=(const WaveformGenerationService &) = delete;
  WaveformGenerationService(WaveformGenerationService &&) = delete;
  WaveformGenerationService &operator=(WaveformGenerationService &&) = delete;

  // Existing valid cache content is returned Ready immediately. Every other
  // request is deduplicated by the exact Waveform2048 DerivativeKey; a
  // visible request upgrades queued prefetch work.
  [[nodiscard]] WaveformRequestResult
  request(const assets::MediaAsset &asset,
          WaveformRequestPriority priority = WaveformRequestPriority::Visible);

  [[nodiscard]] std::optional<WaveformSnapshot>
  snapshot(const assets::DerivativeKey &key);
  [[nodiscard]] std::optional<WaveformSnapshot>
  cancel(const assets::DerivativeKey &key);

  // Cancels queued/generating jobs outside the caller's visible+prefetch
  // window. Terminal snapshots remain available until bounded retention
  // evicts them.
  void retain(std::span<const assets::DerivativeKey> needed);

  // Loads through the typed waveform wrapper, validating both the cache
  // envelope and the fixed versioned peak/RMS payload.
  [[nodiscard]] assets::WaveformDerivativeResult
  load(const assets::MediaAsset &asset, std::stop_token stop = {}) noexcept;

  [[nodiscard]] WaveformGenerationStats stats() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> implementation_;
};

} // namespace mediaserver::server
