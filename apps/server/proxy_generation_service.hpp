#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/assets/derivative_cache.hpp>
#include <mediaserver/assets/proxy_frame_map.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

namespace mediaserver::server {

// Reports decoded video frames as a proxy is encoded. The total comes from
// the encoder's source probe, so callers can show determinate progress without
// estimating from file bytes or wall-clock time.
using ProxyEncodeProgressCallback =
    std::function<void(core::u64 completed_frames,
                       core::u64 total_frames)>;

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

struct ProxyBackendOutput final {
    // Fingerprint of the exact source byte stream consumed by the encoder.
    // This closes the gap where a path could change and then be restored
    // between the service's before/after verification passes.
    assets::ContentFingerprint encoded_source_fingerprint;
    ProxySourceMetadata source_metadata;
    ProxyEncodedMetadata encoded_metadata;
    assets::ProxyFrameMap frame_map;
};

// A run of source frames encoded as a proxy file of its own: frame j of the
// file is source frame `first + j`, numbered as the playback decoder numbers
// them, and the file opens on a keyframe.
struct ProxySourceRange final {
    core::u64 first{};
    core::u64 count{};

    bool operator==(const ProxySourceRange&) const = default;
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
    ProxyEncodeProgressCallback progress;
    // Set while the show or the editor plays: the backend encodes this job
    // within its smallest CPU budget whatever priority it was configured
    // with, so generation never competes with playback decode.
    bool yield_to_playback{};
    // Empty encodes the whole source.
    std::optional<ProxySourceRange> range;
    // Set instead when every segment is ready: the backend joins these
    // segment files, in order, into the whole-file proxy without decoding
    // the source again.
    std::vector<std::filesystem::path> join_segments;
    // Set with `range` to encode it in one pass as consecutive files of this
    // many source frames (the last may be shorter), each opening on a
    // keyframe and written to proxy_piece_path(output_file, piece). Each is
    // handed to `piece_done` as soon as it is complete, which owns the file
    // from then on; returning false stops the encode, which then reports
    // Cancelled. The result describes the last piece.
    core::u64 piece_frames{};
    std::function<bool(core::u32 piece, const ProxyBackendOutput&)>
        piece_done;
};

// Where a piece of a split encode is written.
[[nodiscard]] std::filesystem::path
proxy_piece_path(const std::filesystem::path& output_file, core::u32 piece);

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

    // On success, output_file must contain a non-empty payload matching the
    // requested derivative profile, and encoded_metadata must come from probing
    // that completed file. The backend owns no persistent cache state and must
    // observe stop during long work.
    [[nodiscard]] virtual ProxyBackendResult encode(
        const ProxyEncodeRequest& request, std::stop_token stop) = 0;
};

// Explicit no-encoder backend for tests and configurations where proxy
// generation is disabled. It fails with Unavailable and never writes bytes.
[[nodiscard]] std::unique_ptr<ProxyEncoderBackend>
unavailable_proxy_encoder_backend();

struct ProxyGenerationConfig final {
    core::u64 maximum_encoded_payload_bytes{
        4ULL * 1'024ULL * 1'024ULL * 1'024ULL};
    core::u64 maximum_proxy_frames{10'000'000U};
};

// Routes each asset to a dedicated cache beside its source. runtime_root is
// still machine-local and is used only for transient playback materialization;
// encoded proxy artifacts live in `<source folder>/.mediaserver-proxies`.
struct NextToSourceProxyCacheConfig final {
    std::filesystem::path runtime_root;
    core::u64 maximum_bytes{};
};

struct ProxyCacheClearResult final {
    std::error_code error;

    [[nodiscard]] explicit operator bool() const noexcept { return !error; }
};

// Removes only Pixelsoup-owned children from one dedicated proxy root. The
// root itself and unrelated files alongside those children are preserved.
[[nodiscard]] ProxyCacheClearResult clear_proxy_cache_root(
    const std::filesystem::path& root) noexcept;

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
// `range` is the run of source frames a segment encodes; empty checks a
// whole-file output.
[[nodiscard]] bool valid_proxy_backend_output(
    const ProxyBackendOutput& output,
    const assets::DerivativeProfile& profile,
    core::u64 maximum_proxy_frames,
    std::optional<ProxySourceRange> range = std::nullopt) noexcept;
// The source frames segment `segment` of this source covers.
[[nodiscard]] std::optional<ProxySourceRange>
proxy_segment_source_range(core::u32 segment,
                           const ProxySourceMetadata& source) noexcept;

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
    ProxyGenerationService(
        NextToSourceProxyCacheConfig cache,
        std::unique_ptr<ProxyEncoderBackend> backend,
        ProxyGenerationConfig config = {});

    ProxyGenerationService(const ProxyGenerationService&) = delete;
    ProxyGenerationService& operator=(const ProxyGenerationService&) = delete;
    ProxyGenerationService(ProxyGenerationService&&) = delete;
    ProxyGenerationService& operator=(ProxyGenerationService&&) = delete;

    // What one generate() call makes. The default is the whole-file proxy,
    // encoded from the source.
    struct Step final {
        // Encode within the Background budget whatever the backend's
        // priority; see ProxyEncodeRequest::yield_to_playback.
        bool yield_to_playback{};
        // Encode only this segment, under its own key.
        std::optional<core::u32> segment;
        // Make the whole-file proxy by joining the asset's published
        // segments; fails with CacheNotReady while any is missing.
        bool join_segments{};
        // With `segment`: encode this many consecutive segments from it in
        // one pass, publishing each as it is made, which saves reopening the
        // source and the encoder for every segment. `published` hears of each
        // one; returning false stops the run there, and generate() then
        // reports Cancelled. A run stopped or failed part way keeps the
        // segments it published.
        core::u32 segments{1U};
        std::function<bool(core::u32 segment,
                           const ProxyGenerationReceipt& receipt)>
            published;
    };
    [[nodiscard]] ProxyGenerationResult generate(
        const assets::MediaAsset& asset, std::stop_token stop = {},
        ProxyEncodeProgressCallback progress = {}, Step step = {}) noexcept;
    [[nodiscard]] ProxyArtifactLoadResult load(
        const assets::DerivativeKey& key, std::stop_token stop = {},
        bool verify_payload_digest = true) noexcept;
    [[nodiscard]] ProxyArtifactLoadResult load(
        const assets::MediaAsset& asset,
        const assets::DerivativeKey& key,
        std::stop_token stop = {},
        bool verify_payload_digest = true) noexcept;
    [[nodiscard]] core::Result<assets::DerivativeCacheEntry,
                               assets::DerivativeCacheError>
    inspect(const assets::DerivativeKey& key) noexcept;
    [[nodiscard]] core::Result<assets::DerivativeCacheEntry,
                               assets::DerivativeCacheError>
    inspect(const assets::MediaAsset& asset,
            const assets::DerivativeKey& key) noexcept;

    [[nodiscard]] const std::filesystem::path& cache_root() const noexcept;

    // Counts publish attempts made through this service, from any thread.
    // Callers that remember inspect() results read them again once it moves.
    [[nodiscard]] core::u64 publications() const noexcept;

private:
    struct RoutedCache final {
        std::filesystem::path root;
        std::unique_ptr<assets::DerivativeCache> cache;
    };

    [[nodiscard]] assets::DerivativeCache* cache_for(
        const assets::MediaAsset& asset,
        assets::DerivativeCacheError& error) noexcept;
    [[nodiscard]] ProxyArtifactLoadResult load_from_cache(
        assets::DerivativeCache& cache,
        const assets::DerivativeKey& key,
        std::stop_token stop, bool verify_payload_digest = true) noexcept;
    // Checks one encoded file against its source and range and publishes it
    // under `key`. Throws only what generate() catches.
    [[nodiscard]] ProxyGenerationResult publish_encoded(
        const assets::MediaAsset& asset, assets::DerivativeCache& cache,
        const assets::DerivativeKey& key,
        const std::filesystem::path& raw_path,
        const ProxyBackendOutput& encoded,
        std::optional<ProxySourceRange> range,
        const assets::ContentFingerprint& before, std::stop_token stop);

    std::unique_ptr<assets::DerivativeCache> fixed_cache_;
    std::optional<NextToSourceProxyCacheConfig> next_to_source_cache_;
    std::vector<RoutedCache> routed_caches_;
    std::mutex routed_caches_mutex_;
    std::atomic<core::u64> publications_{};
    std::filesystem::path runtime_root_;
    std::unique_ptr<ProxyEncoderBackend> backend_;
    ProxyGenerationConfig config_;
};

} // namespace mediaserver::server
