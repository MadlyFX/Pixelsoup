#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/assets/derivative_cache.hpp>
#include <mediaserver/assets/proxy_frame_map.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

namespace mediaserver::server {

// Complete source metadata needed to prove that a proxy preserves source-time
// playback semantics. Unknown colour metadata is represented by the existing
// None enum values and must remain unknown in the encoded output.
struct ProxySourceMetadata final {
    assets::PixelDimensions dimensions;
    core::u64 frame_count{};
    assets::MediaFrameRate frame_rate;
    core::u32 pixel_aspect_numerator{1U};
    core::u32 pixel_aspect_denominator{1U};
    core::u8 clockwise_quarter_turns{};
    assets::DerivativeColorPrimaries color_primaries{
        assets::DerivativeColorPrimaries::None};
    assets::DerivativeTransfer transfer{assets::DerivativeTransfer::None};
    assets::DerivativeMatrix matrix{assets::DerivativeMatrix::None};
    assets::DerivativeColorRange color_range{
        assets::DerivativeColorRange::None};
    bool has_audio{};

    bool operator==(const ProxySourceMetadata&) const = default;
};

// Metadata obtained by reopening/probing the completed backend output. A
// backend success is a contract that these fields describe the actual file,
// not merely the encoder settings that were requested.
struct ProxyEncodedMetadata final {
    assets::PixelDimensions dimensions;
    core::u64 frame_count{};
    assets::MediaFrameRate frame_rate;
    core::u32 pixel_aspect_numerator{1U};
    core::u32 pixel_aspect_denominator{1U};
    core::u8 clockwise_quarter_turns{};
    assets::DerivativeColorPrimaries color_primaries{
        assets::DerivativeColorPrimaries::None};
    assets::DerivativeTransfer transfer{assets::DerivativeTransfer::None};
    assets::DerivativeMatrix matrix{assets::DerivativeMatrix::None};
    assets::DerivativeColorRange color_range{
        assets::DerivativeColorRange::None};
    assets::DerivativeContainer container{assets::DerivativeContainer::Mp4};
    assets::DerivativeCodec video_codec{assets::DerivativeCodec::H264};
    assets::DerivativePixelFormat pixel_format{
        assets::DerivativePixelFormat::Yuv420p};
    assets::DerivativeAudioLayout audio_layout{
        assets::DerivativeAudioLayout::None};
    assets::DerivativeAudioCodec audio_codec{
        assets::DerivativeAudioCodec::None};
    core::u32 audio_sample_rate{};
    core::u32 audio_bitrate{};

    bool operator==(const ProxyEncodedMetadata&) const = default;
};

struct ProxyEncodeRequest final {
    // This is the persistent Library identity. The backend has no API for
    // creating or replacing an Asset node.
    doc::NodeId asset{};
    std::string source;
    assets::ContentFingerprint source_fingerprint;
    assets::DerivativeKey key;
    assets::DerivativeProfile profile;
    // Absolute, service-owned scratch path. The service deletes this file on
    // every outcome after packaging or failure.
    std::filesystem::path output_file;
};

struct ProxyBackendOutput final {
    // Fingerprint of the exact source byte stream consumed by the encoder.
    // This closes the gap where a path could change and then be restored
    // between the service's before/after verification passes.
    assets::ContentFingerprint encoded_source_fingerprint;
    ProxySourceMetadata source_metadata;
    ProxyEncodedMetadata encoded_metadata;
    assets::ProxyFrameMap frame_map;
};

enum class ProxyBackendErrorCode : core::u8 {
    Unavailable,
    SourceUnavailable,
    MetadataUnavailable,
    EncodeFailed,
    OutputValidationFailed,
    Cancelled,
    AllocationFailed,
};

struct ProxyBackendError final {
    ProxyBackendErrorCode code{ProxyBackendErrorCode::Unavailable};
    std::error_code system_error;

    bool operator==(const ProxyBackendError&) const = default;
};

using ProxyBackendResult =
    core::Result<ProxyBackendOutput, ProxyBackendError>;

class ProxyEncoderBackend {
public:
    ProxyEncoderBackend() = default;
    ProxyEncoderBackend(const ProxyEncoderBackend&) = delete;
    ProxyEncoderBackend& operator=(const ProxyEncoderBackend&) = delete;
    virtual ~ProxyEncoderBackend() = default;

    // On success, output_file must contain a non-empty MP4/H.264 payload and
    // encoded_metadata must come from probing that completed file. The backend
    // owns no persistent cache state and must observe stop during long work.
    [[nodiscard]] virtual ProxyBackendResult encode(
        const ProxyEncodeRequest& request, std::stop_token stop) = 0;
};

// The repository currently has a decoder but no truthful encoder seam. This
// production default therefore fails with Unavailable and never writes bytes.
[[nodiscard]] std::unique_ptr<ProxyEncoderBackend>
unavailable_proxy_encoder_backend();

struct ProxyGenerationConfig final {
    core::u64 maximum_encoded_payload_bytes{
        4ULL * 1'024ULL * 1'024ULL * 1'024ULL};
    core::u64 maximum_proxy_frames{10'000'000U};
};

enum class ProxyGenerationErrorCode : core::u8 {
    InvalidAsset,
    MissingFingerprint,
    ProxyNotConfigured,
    InvalidProfile,
    SourceUnavailable,
    FingerprintMismatch,
    BackendUnavailable,
    BackendFailure,
    Cancelled,
    InvalidMetadata,
    OutputMissing,
    OutputTooLarge,
    ArtifactWriteFailed,
    ArtifactInvalid,
    CacheNotReady,
    CacheFailure,
    AllocationFailed,
};

struct ProxyGenerationError final {
    ProxyGenerationErrorCode code{ProxyGenerationErrorCode::InvalidAsset};
    doc::NodeId asset{};
    std::optional<assets::DerivativeKey> key;
    std::optional<ProxyBackendError> backend_error;
    std::optional<assets::DerivativeCacheError> cache_error;
    std::error_code system_error;

    bool operator==(const ProxyGenerationError&) const = default;
};

struct ProxyGenerationReceipt final {
    // asset is always the input MediaAsset id and key.asset must equal it.
    doc::NodeId asset{};
    assets::DerivativeKey key;
    ProxySourceMetadata source_metadata;
    ProxyEncodedMetadata encoded_metadata;
    assets::ProxyFrameMap frame_map;
    assets::ContentFingerprint encoded_payload_fingerprint;
    assets::DerivativeCacheEntry cache_entry;
    bool cache_hit{};

};

struct ProxyArtifactLoad final {
    ProxyGenerationReceipt receipt;
    // Raw encoded MP4 bytes, excluding the service's keyed sidecar envelope.
    std::vector<core::u8> encoded_payload;
};

using ProxyGenerationResult =
    core::Result<ProxyGenerationReceipt, ProxyGenerationError>;
using ProxyArtifactLoadResult =
    core::Result<ProxyArtifactLoad, ProxyGenerationError>;

[[nodiscard]] bool
valid_proxy_source_metadata(const ProxySourceMetadata& metadata) noexcept;
[[nodiscard]] bool valid_proxy_backend_output(
    const ProxyBackendOutput& output,
    const assets::DerivativeProfile& profile,
    core::u64 maximum_proxy_frames) noexcept;

// Synchronous machine-local generator/publication boundary. Callers choose the
// worker/thread policy; cancellation is observed during source verification,
// backend work, packaging, and the cache's atomic publication.
class ProxyGenerationService final {
public:
    explicit ProxyGenerationService(
        assets::DerivativeCache cache,
        ProxyGenerationConfig config = {});
    ProxyGenerationService(
        assets::DerivativeCache cache,
        std::unique_ptr<ProxyEncoderBackend> backend,
        ProxyGenerationConfig config = {});

    ProxyGenerationService(const ProxyGenerationService&) = delete;
    ProxyGenerationService& operator=(const ProxyGenerationService&) = delete;
    ProxyGenerationService(ProxyGenerationService&&) = delete;
    ProxyGenerationService& operator=(ProxyGenerationService&&) = delete;

    [[nodiscard]] ProxyGenerationResult generate(
        const assets::MediaAsset& asset, std::stop_token stop = {}) noexcept;
    [[nodiscard]] ProxyArtifactLoadResult load(
        const assets::DerivativeKey& key, std::stop_token stop = {}) noexcept;
    [[nodiscard]] core::Result<assets::DerivativeCacheEntry,
                               assets::DerivativeCacheError>
    inspect(const assets::DerivativeKey& key) noexcept;

    [[nodiscard]] const std::filesystem::path& cache_root() const noexcept;

private:
    assets::DerivativeCache cache_;
    std::unique_ptr<ProxyEncoderBackend> backend_;
    ProxyGenerationConfig config_;
};

} // namespace mediaserver::server
