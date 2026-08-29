#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/assets/derivative_cache.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>

#include <array>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace mediaserver::assets {

// Exact payload contract for waveform-2048-peak-rms. Each bin aggregates all
// interleaved channels over its source-frame interval. Short sources repeat
// their nearest source frame so the derivative always has the profile's fixed
// sample count; no document or asset identity is created for this data.
inline constexpr std::size_t waveform_peak_rms_bins = 2'048U;
inline constexpr core::u32 maximum_waveform_channels = 64U;
inline constexpr core::u32 waveform_payload_version = 1U;

struct WaveformPeakRms final {
    float peak{};
    float rms{};

    bool operator==(const WaveformPeakRms&) const = default;
};

using WaveformEnvelope =
    std::array<WaveformPeakRms, waveform_peak_rms_bins>;

enum class WaveformError : core::u8 {
    InvalidChannelCount,
    EmptySource,
    IncompleteFrame,
    NonFiniteSample,
    InvalidPayload,
    UnsupportedVersion,
};

using WaveformBuildResult = core::Result<WaveformEnvelope, WaveformError>;
using WaveformDecodeResult = core::Result<WaveformEnvelope, WaveformError>;

// Samples are binary32 PCM in interleaved frame order. Values outside [-1, 1]
// are canonically clamped; non-finite input fails visibly. Peak is the maximum
// absolute channel sample. RMS is sqrt(sum(sample^2)/(frames*channels)), using
// a binary64 accumulator before the one binary32 projection per bin.
[[nodiscard]] WaveformBuildResult build_waveform_peak_rms(
    std::span<const float> interleaved_samples, core::u32 channel_count);

// A cache envelope already carries the DerivativeKey. This inner payload is
// independently versioned and self-describing so a truncated or stale cache
// entry cannot be interpreted as host-endian float memory.
[[nodiscard]] std::vector<core::u8>
encode_waveform_peak_rms(const WaveformEnvelope& envelope);
[[nodiscard]] WaveformDecodeResult
decode_waveform_peak_rms(std::span<const core::u8> payload) noexcept;

enum class WaveformDerivativeErrorCode : core::u8 {
    InvalidAsset,
    MissingFingerprint,
    ProfileUnavailable,
    BuildFailed,
    CacheFailure,
    NotReady,
    InvalidCachedPayload,
};

struct WaveformDerivativeError final {
    WaveformDerivativeErrorCode code{
        WaveformDerivativeErrorCode::InvalidAsset};
    std::optional<WaveformError> waveform_error;
    std::optional<DerivativeCacheError> cache_error;
    std::optional<DerivativeState> cache_state;

    bool operator==(const WaveformDerivativeError&) const = default;
};

struct WaveformDerivative final {
    DerivativeKey key;
    DerivativeCacheEntry entry;
    WaveformEnvelope envelope;
};

using WaveformDerivativeResult =
    core::Result<WaveformDerivative, WaveformDerivativeError>;

// Synchronous publication boundary used by an audio decoder/worker. Identity
// comes strictly from the existing persistent MediaAsset id+fingerprint and
// the frozen waveform profile. Cache publication is atomic and disposable.
[[nodiscard]] WaveformDerivativeResult publish_waveform_derivative(
    DerivativeCache& cache, const MediaAsset& asset,
    std::span<const float> interleaved_samples, core::u32 channel_count,
    bool pin = false, std::stop_token stop = {});

// Loads the exact key derived from the same persistent asset and validates the
// versioned inner payload before exposing bins. A different source revision is
// therefore a different cache lookup, never a rewritten asset identity.
[[nodiscard]] WaveformDerivativeResult load_waveform_derivative(
    DerivativeCache& cache, const MediaAsset& asset, bool pin = false,
    std::stop_token stop = {}) noexcept;

} // namespace mediaserver::assets
