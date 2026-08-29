#include <mediaserver/assets/derivative.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <type_traits>

namespace mediaserver::assets {
namespace {

constexpr std::array<DerivativeProfile, 4U> profiles{{
    {
        DerivativeProfileId::Thumbnail320x180Srgb,
        "thumb-320x180-srgb",
        1U,
        1U,
        DerivativeKind::Thumbnail,
        DerivativeContainer::Png,
        DerivativeCodec::Png,
        DerivativeSampling::FirstValidAtOrAfterTenPercent,
        320U,
        180U,
        1U,
        DerivativeFrameRatePolicy::NotApplicable,
        {},
        DerivativePixelFormat::Rgba8,
        DerivativeColorPrimaries::Rec709,
        DerivativeTransfer::Srgb,
        DerivativeMatrix::Rgb,
        DerivativeColorRange::Full,
        DerivativeAlphaPolicy::Straight,
        DerivativeAudioLayout::None,
        DerivativeAudioCodec::None,
        0U,
        0U,
        DerivativeOrientationPolicy::ApplySourceRotation,
        DerivativePixelAspectPolicy::ApplyBeforeFit,
        DerivativeFitPolicy::TransparentLetterbox,
        0U,
        0U,
    },
    {
        DerivativeProfileId::Filmstrip8x160x90Srgb,
        "filmstrip-8x160x90-srgb",
        1U,
        1U,
        DerivativeKind::Filmstrip,
        DerivativeContainer::Png,
        DerivativeCodec::Png,
        DerivativeSampling::UniformTimeline,
        160U,
        90U,
        8U,
        DerivativeFrameRatePolicy::NotApplicable,
        {},
        DerivativePixelFormat::Rgba8,
        DerivativeColorPrimaries::Rec709,
        DerivativeTransfer::Srgb,
        DerivativeMatrix::Rgb,
        DerivativeColorRange::Full,
        DerivativeAlphaPolicy::Straight,
        DerivativeAudioLayout::None,
        DerivativeAudioCodec::None,
        0U,
        0U,
        DerivativeOrientationPolicy::ApplySourceRotation,
        DerivativePixelAspectPolicy::ApplyBeforeFit,
        DerivativeFitPolicy::TransparentLetterbox,
        0U,
        0U,
    },
    {
        DerivativeProfileId::Waveform2048PeakRms,
        "waveform-2048-peak-rms",
        1U,
        1U,
        DerivativeKind::Waveform,
        DerivativeContainer::WaveformF32,
        DerivativeCodec::PeakRmsF32,
        DerivativeSampling::WholeAsset,
        0U,
        0U,
        2'048U,
        DerivativeFrameRatePolicy::NotApplicable,
        {},
        DerivativePixelFormat::None,
        DerivativeColorPrimaries::None,
        DerivativeTransfer::None,
        DerivativeMatrix::None,
        DerivativeColorRange::None,
        DerivativeAlphaPolicy::None,
        DerivativeAudioLayout::MonoAnalysis,
        DerivativeAudioCodec::None,
        0U,
        0U,
        DerivativeOrientationPolicy::NotApplicable,
        DerivativePixelAspectPolicy::NotApplicable,
        DerivativeFitPolicy::NotApplicable,
        0U,
        0U,
    },
    {
        DerivativeProfileId::Proxy720pCfrH264,
        "proxy-720p-cfr-h264",
        1U,
        1U,
        DerivativeKind::Proxy,
        DerivativeContainer::Mp4,
        DerivativeCodec::H264,
        DerivativeSampling::ConstantFrameRate,
        1'280U,
        720U,
        0U,
        DerivativeFrameRatePolicy::PreserveSource,
        {},
        DerivativePixelFormat::Yuv420p,
        DerivativeColorPrimaries::Rec709,
        DerivativeTransfer::Rec709,
        DerivativeMatrix::Rec709,
        DerivativeColorRange::Limited,
        DerivativeAlphaPolicy::None,
        DerivativeAudioLayout::Stereo,
        DerivativeAudioCodec::Aac,
        48'000U,
        192'000U,
        DerivativeOrientationPolicy::PreserveSourceMetadata,
        DerivativePixelAspectPolicy::PreserveSourceMetadata,
        DerivativeFitPolicy::ScaleWithinBounds,
        23U,
        60U,
    },
}};

void hash_bytes(Sha256Hasher& hash, std::string_view bytes) noexcept {
    hash.update(std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(bytes.data()), bytes.size()});
}

void hash_u16(Sha256Hasher& hash, core::u16 value) noexcept {
    const std::array<core::u8, 2U> bytes{{
        static_cast<core::u8>((value >> 8U) & 0xffU),
        static_cast<core::u8>(value & 0xffU),
    }};
    hash.update(bytes);
}

void hash_u32(Sha256Hasher& hash, core::u32 value) noexcept {
    const std::array<core::u8, 4U> bytes{{
        static_cast<core::u8>((value >> 24U) & 0xffU),
        static_cast<core::u8>((value >> 16U) & 0xffU),
        static_cast<core::u8>((value >> 8U) & 0xffU),
        static_cast<core::u8>(value & 0xffU),
    }};
    hash.update(bytes);
}

void hash_u64(Sha256Hasher& hash, core::u64 value) noexcept {
    std::array<core::u8, 8U> bytes{};
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        bytes[index] = static_cast<core::u8>(
            (value >> static_cast<core::u32>((7U - index) * 8U)) & 0xffU);
    }
    hash.update(bytes);
}

template <typename Enum>
void hash_enum(Sha256Hasher& hash, Enum value) noexcept {
    static_assert(std::is_enum_v<Enum>);
    hash_u32(hash, static_cast<core::u32>(value));
}

void hash_profile(Sha256Hasher& hash,
                  const DerivativeProfile& profile) noexcept {
    hash_bytes(hash, "mediaserver.derivative-profile.v1\0");
    hash_enum(hash, profile.id);
    hash_u32(hash, static_cast<core::u32>(profile.name.size()));
    hash_bytes(hash, profile.name);
    hash_u32(hash, profile.profile_version);
    hash_u32(hash, profile.generator_version);
    hash_enum(hash, profile.kind);
    hash_enum(hash, profile.container);
    hash_enum(hash, profile.codec);
    hash_enum(hash, profile.sampling);
    hash_u32(hash, profile.width);
    hash_u32(hash, profile.height);
    hash_u32(hash, profile.sample_count);
    hash_enum(hash, profile.frame_rate_policy);
    hash_u32(hash, profile.fixed_frame_rate.numerator);
    hash_u32(hash, profile.fixed_frame_rate.denominator);
    hash_enum(hash, profile.pixel_format);
    hash_enum(hash, profile.color_primaries);
    hash_enum(hash, profile.transfer);
    hash_enum(hash, profile.matrix);
    hash_enum(hash, profile.color_range);
    hash_enum(hash, profile.alpha_policy);
    hash_enum(hash, profile.audio_layout);
    hash_enum(hash, profile.audio_codec);
    hash_u32(hash, profile.audio_sample_rate);
    hash_u32(hash, profile.audio_bitrate);
    hash_enum(hash, profile.orientation_policy);
    hash_enum(hash, profile.pixel_aspect_policy);
    hash_enum(hash, profile.fit_policy);
    hash_u32(hash, profile.quality);
    hash_u32(hash, profile.keyframe_interval);
}

Sha256Digest key_digest(doc::NodeId asset,
                        const ContentFingerprint& fingerprint,
                        const DerivativeProfile& profile,
                        const Sha256Digest& signature) noexcept {
    Sha256Hasher hash;
    hash_bytes(hash, "mediaserver.derivative-key.v1\0");
    hash_u64(hash, asset.value());
    hash_u64(hash, fingerprint.byte_length);
    hash.update(fingerprint.sha256.bytes);
    hash_u16(hash, static_cast<core::u16>(profile.id));
    hash_u32(hash, profile.profile_version);
    hash_u32(hash, profile.generator_version);
    hash.update(signature.bytes);
    return hash.finish();
}

} // namespace

std::span<const DerivativeProfile> derivative_profiles() noexcept {
    return profiles;
}

const DerivativeProfile*
derivative_profile(DerivativeProfileId id) noexcept {
    for (const DerivativeProfile& profile : profiles) {
        if (profile.id == id) {
            return &profile;
        }
    }
    return nullptr;
}

const DerivativeProfile* derivative_profile(std::string_view name) noexcept {
    for (const DerivativeProfile& profile : profiles) {
        if (profile.name == name) {
            return &profile;
        }
    }
    return nullptr;
}

Sha256Digest derivative_profile_signature(
    const DerivativeProfile& profile) noexcept {
    Sha256Hasher hash;
    hash_profile(hash, profile);
    return hash.finish();
}

std::optional<core::u64> thumbnail_probe_ordinal(
    core::u64 frame_count, core::u64 probe_index) noexcept {
    if (frame_count == 0U || probe_index >= frame_count) {
        return std::nullopt;
    }
    core::u64 preferred = frame_count / 10U;
    if (frame_count % 10U != 0U) {
        ++preferred;
    }
    preferred = std::min(preferred, frame_count - 1U);
    const core::u64 forward_count = frame_count - preferred;
    if (probe_index < forward_count) {
        return preferred + probe_index;
    }
    return preferred - 1U - (probe_index - forward_count);
}

std::optional<std::array<core::u64, 8U>>
filmstrip_source_ordinals(core::u64 frame_count) noexcept {
    if (frame_count == 0U) {
        return std::nullopt;
    }
    std::array<core::u64, 8U> ordinals{};
    const core::u64 last = frame_count - 1U;
    const core::u64 quotient = last / 7U;
    const core::u64 remainder = last % 7U;
    for (core::u64 sample = 0U; sample < ordinals.size(); ++sample) {
        ordinals[static_cast<std::size_t>(sample)] =
            sample * quotient + (sample * remainder) / 7U;
    }
    return ordinals;
}

DerivativeKey make_derivative_key(
    doc::NodeId asset, const ContentFingerprint& fingerprint,
    const DerivativeProfile& profile) noexcept {
    const Sha256Digest signature = derivative_profile_signature(profile);
    return DerivativeKey{
        asset,
        fingerprint,
        profile.id,
        profile.profile_version,
        profile.generator_version,
        signature,
        key_digest(asset, fingerprint, profile, signature),
    };
}

bool valid_derivative_key(const DerivativeKey& key) noexcept {
    if (!key.asset) {
        return false;
    }
    const DerivativeProfile* profile = derivative_profile(key.profile);
    if (profile == nullptr ||
        key.profile_version != profile->profile_version ||
        key.generator_version != profile->generator_version) {
        return false;
    }
    const Sha256Digest signature = derivative_profile_signature(*profile);
    return key.profile_signature == signature &&
        key.digest == key_digest(key.asset, key.fingerprint, *profile,
                                 signature);
}

std::string derivative_key_string(const DerivativeKey& key) {
    return sha256_hex(key.digest);
}

std::filesystem::path
derivative_cache_relative_path(const DerivativeKey& key) {
    const DerivativeProfile* profile = derivative_profile(key.profile);
    const std::string name = profile == nullptr
        ? std::string{"unknown-profile"}
        : std::string{profile->name};
    const std::string digest = derivative_key_string(key);
    return std::filesystem::path{"v1"} / name / digest.substr(0U, 2U) /
        (digest + ".msderiv");
}

std::filesystem::path derivative_cache_path(
    const std::filesystem::path& absolute_root, const DerivativeKey& key) {
    return absolute_root / derivative_cache_relative_path(key);
}

bool valid_derivative_state_transition(
    DerivativeState from, DerivativeState to) noexcept {
    if (from == to) {
        return true;
    }
    switch (from) {
    case DerivativeState::Missing:
        return to == DerivativeState::Queued;
    case DerivativeState::Queued:
        return to == DerivativeState::Generating ||
            to == DerivativeState::Cancelled ||
            to == DerivativeState::Failed ||
            to == DerivativeState::Missing;
    case DerivativeState::Generating:
        return to == DerivativeState::Ready ||
            to == DerivativeState::Failed ||
            to == DerivativeState::Cancelled ||
            to == DerivativeState::Stale ||
            to == DerivativeState::Missing;
    case DerivativeState::Ready:
        return to == DerivativeState::Stale ||
            to == DerivativeState::Failed ||
            to == DerivativeState::Queued ||
            to == DerivativeState::Missing;
    case DerivativeState::Stale:
    case DerivativeState::Failed:
    case DerivativeState::Cancelled:
        return to == DerivativeState::Queued ||
            to == DerivativeState::Missing;
    }
    return false;
}

} // namespace mediaserver::assets
