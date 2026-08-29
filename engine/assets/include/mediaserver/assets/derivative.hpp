#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/assets/derivative_profile_id.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace mediaserver::assets {

enum class DerivativeKind : core::u8 {
    Thumbnail,
    Filmstrip,
    Waveform,
    Proxy,
};

enum class DerivativeContainer : core::u8 {
    Png,
    WaveformF32,
    Mp4,
};

enum class DerivativeCodec : core::u8 {
    Png,
    PeakRmsF32,
    H264,
};

enum class DerivativeSampling : core::u8 {
    FirstValidAtOrAfterTenPercent,
    UniformTimeline,
    WholeAsset,
    ConstantFrameRate,
};

enum class DerivativePixelFormat : core::u8 {
    None,
    Rgba8,
    Yuv420p,
};

enum class DerivativeColorPrimaries : core::u8 {
    None,
    Rec709,
};

enum class DerivativeTransfer : core::u8 {
    None,
    Srgb,
    Rec709,
};

enum class DerivativeMatrix : core::u8 {
    None,
    Rgb,
    Rec709,
};

enum class DerivativeColorRange : core::u8 {
    None,
    Full,
    Limited,
};

enum class DerivativeAlphaPolicy : core::u8 {
    None,
    Straight,
};

enum class DerivativeAudioLayout : core::u8 {
    None,
    MonoAnalysis,
    Stereo,
};

enum class DerivativeAudioCodec : core::u8 {
    None,
    Aac,
};

enum class DerivativeFrameRatePolicy : core::u8 {
    NotApplicable,
    PreserveSource,
    Fixed,
};

enum class DerivativeOrientationPolicy : core::u8 {
    NotApplicable,
    ApplySourceRotation,
    PreserveSourceMetadata,
};

enum class DerivativePixelAspectPolicy : core::u8 {
    NotApplicable,
    ApplyBeforeFit,
    PreserveSourceMetadata,
};

enum class DerivativeFitPolicy : core::u8 {
    NotApplicable,
    TransparentLetterbox,
    ScaleWithinBounds,
};

struct DerivativeProfile final {
    DerivativeProfileId id{};
    std::string_view name;
    core::u32 profile_version{};
    core::u32 generator_version{};
    DerivativeKind kind{};
    DerivativeContainer container{};
    DerivativeCodec codec{};
    DerivativeSampling sampling{};
    core::u32 width{};
    core::u32 height{};
    core::u32 sample_count{};
    DerivativeFrameRatePolicy frame_rate_policy{};
    MediaFrameRate fixed_frame_rate{};
    DerivativePixelFormat pixel_format{};
    DerivativeColorPrimaries color_primaries{};
    DerivativeTransfer transfer{};
    DerivativeMatrix matrix{};
    DerivativeColorRange color_range{};
    DerivativeAlphaPolicy alpha_policy{};
    DerivativeAudioLayout audio_layout{};
    DerivativeAudioCodec audio_codec{};
    core::u32 audio_sample_rate{};
    core::u32 audio_bitrate{};
    DerivativeOrientationPolicy orientation_policy{};
    DerivativePixelAspectPolicy pixel_aspect_policy{};
    DerivativeFitPolicy fit_policy{};
    // Codec-specific, frozen semantic fields. H.264 uses CRF and GOP frames;
    // lossless profiles leave both zero.
    core::u32 quality{};
    core::u32 keyframe_interval{};

    bool operator==(const DerivativeProfile&) const = default;
};

[[nodiscard]] std::span<const DerivativeProfile> derivative_profiles() noexcept;
[[nodiscard]] const DerivativeProfile*
derivative_profile(DerivativeProfileId id) noexcept;
[[nodiscard]] const DerivativeProfile*
derivative_profile(std::string_view name) noexcept;
[[nodiscard]] Sha256Digest
derivative_profile_signature(const DerivativeProfile& profile) noexcept;

// Probe zero is the first ordinal at/after 10% of CFR/image-sequence duration.
// Later probes advance to the end, then fall back from the preferred ordinal
// toward zero. A still therefore yields only ordinal zero. This lets a decoder
// skip corrupt frames without choosing its own non-deterministic fallback.
[[nodiscard]] std::optional<core::u64> thumbnail_probe_ordinal(
    core::u64 frame_count, core::u64 probe_index) noexcept;
// Eight exact floor-divided samples including both endpoints. Short sources
// deliberately repeat ordinals rather than changing the profile's tile count.
[[nodiscard]] std::optional<std::array<core::u64, 8U>>
filmstrip_source_ordinals(core::u64 frame_count) noexcept;

// This value deliberately retains every member of the identity tuple rather
// than reducing identity to an opaque string at API boundaries.
struct DerivativeKey final {
    doc::NodeId asset{};
    ContentFingerprint fingerprint;
    DerivativeProfileId profile{};
    core::u32 profile_version{};
    core::u32 generator_version{};
    Sha256Digest profile_signature;
    Sha256Digest digest;

    bool operator==(const DerivativeKey&) const = default;
};

[[nodiscard]] DerivativeKey make_derivative_key(
    doc::NodeId asset, const ContentFingerprint& fingerprint,
    const DerivativeProfile& profile) noexcept;
[[nodiscard]] bool valid_derivative_key(const DerivativeKey& key) noexcept;
[[nodiscard]] std::string derivative_key_string(const DerivativeKey& key);

// Cache paths contain only frozen profile vocabulary and the key digest. They
// never inherit a source filename or machine-specific authored path.
[[nodiscard]] std::filesystem::path
derivative_cache_relative_path(const DerivativeKey& key);
[[nodiscard]] std::filesystem::path derivative_cache_path(
    const std::filesystem::path& absolute_root, const DerivativeKey& key);

enum class DerivativeState : core::u8 {
    Missing,
    Queued,
    Generating,
    Ready,
    Stale,
    Failed,
    Cancelled,
};

[[nodiscard]] bool valid_derivative_state_transition(
    DerivativeState from, DerivativeState to) noexcept;

} // namespace mediaserver::assets
