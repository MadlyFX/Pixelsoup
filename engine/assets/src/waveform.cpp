#include <mediaserver/assets/waveform.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>

namespace mediaserver::assets {
namespace {

constexpr std::array<core::u8, 4U> waveform_magic{{'M', 'S', 'W', 'F'}};
constexpr std::size_t payload_header_bytes = 12U;
constexpr std::size_t bytes_per_bin = sizeof(float) * 2U;
constexpr std::size_t expected_payload_bytes =
    payload_header_bytes + waveform_peak_rms_bins * bytes_per_bin;

[[nodiscard]] WaveformDerivativeError derivative_error(
    WaveformDerivativeErrorCode code,
    std::optional<WaveformError> waveform = std::nullopt,
    std::optional<DerivativeCacheError> cache = std::nullopt,
    std::optional<DerivativeState> state = std::nullopt) noexcept {
    return WaveformDerivativeError{code, waveform, cache, state};
}

[[nodiscard]] const DerivativeProfile* waveform_profile() noexcept {
    return derivative_profile(DerivativeProfileId::Waveform2048PeakRms);
}

[[nodiscard]] std::optional<DerivativeKey> waveform_key(
    const MediaAsset& asset) noexcept {
    const DerivativeProfile* profile = waveform_profile();
    if (!asset.id || !asset.fingerprint.has_value() || profile == nullptr) {
        return std::nullopt;
    }
    return make_derivative_key(asset.id, *asset.fingerprint, *profile);
}

void append_u32(std::vector<core::u8>& output, core::u32 value) {
    output.push_back(static_cast<core::u8>(value & 0xffU));
    output.push_back(static_cast<core::u8>((value >> 8U) & 0xffU));
    output.push_back(static_cast<core::u8>((value >> 16U) & 0xffU));
    output.push_back(static_cast<core::u8>((value >> 24U) & 0xffU));
}

[[nodiscard]] core::u32 read_u32(std::span<const core::u8> input,
                                 std::size_t offset) noexcept {
    return static_cast<core::u32>(input[offset]) |
        (static_cast<core::u32>(input[offset + 1U]) << 8U) |
        (static_cast<core::u32>(input[offset + 2U]) << 16U) |
        (static_cast<core::u32>(input[offset + 3U]) << 24U);
}

void append_float(std::vector<core::u8>& output, float value) {
    append_u32(output, std::bit_cast<core::u32>(value));
}

[[nodiscard]] float read_float(std::span<const core::u8> input,
                               std::size_t offset) noexcept {
    return std::bit_cast<float>(read_u32(input, offset));
}

[[nodiscard]] float canonical_sample(float sample) noexcept {
    const float clamped = std::clamp(sample, -1.0F, 1.0F);
    return clamped == 0.0F ? 0.0F : clamped;
}

} // namespace

WaveformBuildResult build_waveform_peak_rms(
    std::span<const float> interleaved_samples, core::u32 channel_count) {
    if (channel_count == 0U || channel_count > maximum_waveform_channels) {
        return WaveformBuildResult::failure(
            WaveformError::InvalidChannelCount);
    }
    if (interleaved_samples.empty()) {
        return WaveformBuildResult::failure(WaveformError::EmptySource);
    }
    if (interleaved_samples.size() % channel_count != 0U) {
        return WaveformBuildResult::failure(WaveformError::IncompleteFrame);
    }
    if (std::any_of(interleaved_samples.begin(), interleaved_samples.end(),
                    [](float sample) noexcept {
                        return !std::isfinite(sample);
                    })) {
        return WaveformBuildResult::failure(
            WaveformError::NonFiniteSample);
    }

    const std::size_t frame_count =
        interleaved_samples.size() / channel_count;
    WaveformEnvelope result{};
    const std::size_t quotient = frame_count / result.size();
    const std::size_t remainder = frame_count % result.size();
    for (std::size_t bin = 0U; bin < result.size(); ++bin) {
        // Split the ratio before multiplying so even a maximum-size source
        // span cannot overflow merely because the fixed bin index is 2047.
        const std::size_t first = bin * quotient +
            (bin * remainder) / result.size();
        std::size_t last = (bin + 1U) * quotient +
            ((bin + 1U) * remainder) / result.size();
        last = std::max(last, first + 1U);
        last = std::min(last, frame_count);

        float peak = 0.0F;
        double sum_squares = 0.0;
        std::size_t sample_count = 0U;
        for (std::size_t frame = first; frame < last; ++frame) {
            const std::size_t frame_offset = frame * channel_count;
            for (core::u32 channel = 0U; channel < channel_count; ++channel) {
                const float sample = canonical_sample(
                    interleaved_samples[frame_offset + channel]);
                peak = std::max(peak, std::fabs(sample));
                const double promoted = static_cast<double>(sample);
                sum_squares += promoted * promoted;
                ++sample_count;
            }
        }
        const double mean_square =
            sum_squares / static_cast<double>(sample_count);
        const float rms = std::min(
            peak, static_cast<float>(std::sqrt(mean_square)));
        result[bin] = WaveformPeakRms{peak, rms};
    }
    return WaveformBuildResult::success(result);
}

std::vector<core::u8>
encode_waveform_peak_rms(const WaveformEnvelope& envelope) {
    std::vector<core::u8> output;
    output.reserve(expected_payload_bytes);
    output.insert(output.end(), waveform_magic.begin(), waveform_magic.end());
    append_u32(output, waveform_payload_version);
    append_u32(output, static_cast<core::u32>(envelope.size()));
    for (const WaveformPeakRms& bin : envelope) {
        append_float(output, bin.peak);
        append_float(output, bin.rms);
    }
    return output;
}

WaveformDecodeResult
decode_waveform_peak_rms(std::span<const core::u8> payload) noexcept {
    if (payload.size() != expected_payload_bytes ||
        !std::equal(waveform_magic.begin(), waveform_magic.end(),
                    payload.begin())) {
        return WaveformDecodeResult::failure(WaveformError::InvalidPayload);
    }
    if (read_u32(payload, 4U) != waveform_payload_version) {
        return WaveformDecodeResult::failure(
            WaveformError::UnsupportedVersion);
    }
    if (read_u32(payload, 8U) != waveform_peak_rms_bins) {
        return WaveformDecodeResult::failure(WaveformError::InvalidPayload);
    }

    WaveformEnvelope result{};
    std::size_t offset = payload_header_bytes;
    for (WaveformPeakRms& bin : result) {
        const float peak = read_float(payload, offset);
        const float rms = read_float(payload, offset + sizeof(float));
        offset += bytes_per_bin;
        if (!std::isfinite(peak) || !std::isfinite(rms) || peak < 0.0F ||
            peak > 1.0F || rms < 0.0F || rms > peak) {
            return WaveformDecodeResult::failure(
                WaveformError::InvalidPayload);
        }
        bin = WaveformPeakRms{peak == 0.0F ? 0.0F : peak,
                              rms == 0.0F ? 0.0F : rms};
    }
    return WaveformDecodeResult::success(result);
}

WaveformDerivativeResult publish_waveform_derivative(
    DerivativeCache& cache, const MediaAsset& asset,
    std::span<const float> interleaved_samples, core::u32 channel_count,
    bool pin, std::stop_token stop) {
    if (!asset.id) {
        return WaveformDerivativeResult::failure(derivative_error(
            WaveformDerivativeErrorCode::InvalidAsset));
    }
    if (!asset.fingerprint.has_value()) {
        return WaveformDerivativeResult::failure(derivative_error(
            WaveformDerivativeErrorCode::MissingFingerprint));
    }
    const DerivativeProfile* profile = waveform_profile();
    if (profile == nullptr || profile->kind != DerivativeKind::Waveform ||
        profile->codec != DerivativeCodec::PeakRmsF32 ||
        profile->sample_count != waveform_peak_rms_bins) {
        return WaveformDerivativeResult::failure(derivative_error(
            WaveformDerivativeErrorCode::ProfileUnavailable));
    }
    const DerivativeKey key =
        make_derivative_key(asset.id, *asset.fingerprint, *profile);
    auto built = build_waveform_peak_rms(interleaved_samples, channel_count);
    if (!built) {
        return WaveformDerivativeResult::failure(derivative_error(
            WaveformDerivativeErrorCode::BuildFailed, built.error()));
    }
    WaveformEnvelope envelope = std::move(built).value();
    const std::vector<core::u8> payload =
        encode_waveform_peak_rms(envelope);
    auto published = cache.publish(key, payload, pin, stop);
    if (!published) {
        return WaveformDerivativeResult::failure(derivative_error(
            WaveformDerivativeErrorCode::CacheFailure, std::nullopt,
            published.error()));
    }
    return WaveformDerivativeResult::success(WaveformDerivative{
        key, std::move(published).value(), std::move(envelope)});
}

WaveformDerivativeResult load_waveform_derivative(
    DerivativeCache& cache, const MediaAsset& asset, bool pin,
    std::stop_token stop) noexcept {
    if (!asset.id) {
        return WaveformDerivativeResult::failure(derivative_error(
            WaveformDerivativeErrorCode::InvalidAsset));
    }
    if (!asset.fingerprint.has_value()) {
        return WaveformDerivativeResult::failure(derivative_error(
            WaveformDerivativeErrorCode::MissingFingerprint));
    }
    const auto key = waveform_key(asset);
    if (!key.has_value()) {
        return WaveformDerivativeResult::failure(derivative_error(
            WaveformDerivativeErrorCode::ProfileUnavailable));
    }
    auto loaded = cache.load(
        *key, static_cast<core::u64>(expected_payload_bytes), pin, stop);
    if (!loaded) {
        return WaveformDerivativeResult::failure(derivative_error(
            WaveformDerivativeErrorCode::CacheFailure, std::nullopt,
            loaded.error()));
    }
    DerivativeCacheLoad cache_load = std::move(loaded).value();
    if (cache_load.entry.state != DerivativeState::Ready) {
        return WaveformDerivativeResult::failure(derivative_error(
            WaveformDerivativeErrorCode::NotReady, std::nullopt,
            std::nullopt, cache_load.entry.state));
    }
    auto decoded = decode_waveform_peak_rms(cache_load.payload);
    if (!decoded) {
        return WaveformDerivativeResult::failure(derivative_error(
            WaveformDerivativeErrorCode::InvalidCachedPayload,
            decoded.error()));
    }
    return WaveformDerivativeResult::success(WaveformDerivative{
        *key, std::move(cache_load.entry), std::move(decoded).value()});
}

} // namespace mediaserver::assets
