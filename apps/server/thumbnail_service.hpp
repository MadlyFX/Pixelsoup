#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/assets/derivative_cache.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace mediaserver::server {

enum class ThumbnailRequestPriority : core::u8 {
    Visible,
    Prefetch,
};

enum class ThumbnailFailureCode : core::u8 {
    None,
    QueueCapacity,
    SourceUnavailable,
    FingerprintMismatch,
    NoValidFrame,
    InvalidFrame,
    EncodeFailed,
    CacheFailure,
};

struct ThumbnailSnapshot final {
    assets::DerivativeKey key;
    assets::DerivativeState state{assets::DerivativeState::Missing};
    ThumbnailRequestPriority priority{ThumbnailRequestPriority::Visible};
    ThumbnailFailureCode failure{ThumbnailFailureCode::None};
    std::optional<assets::DerivativeCacheErrorCode> cache_error;
    std::optional<core::u64> source_ordinal;
    core::u64 attempted_frames{};
    core::u64 revision{};
    std::filesystem::path cache_path;
    // Thumbnail jobs contain zero or one entry; filmstrip jobs contain the
    // profile's exact ordered sample ordinals, including deliberate repeats
    // for short sources.
    std::vector<core::u64> source_ordinals;
};

struct ThumbnailServiceConfig final {
    // Values are clamped to the hard safety limits documented below.
    core::u32 worker_count{2U};
    core::u32 maximum_pending_requests{32U};
    core::u32 maximum_prefetch_requests{16U};
    core::u32 maximum_snapshots{512U};
};

inline constexpr core::u32 maximum_thumbnail_workers = 8U;
inline constexpr core::u32 maximum_thumbnail_pending_requests = 256U;
inline constexpr core::u32 maximum_thumbnail_snapshots = 4'096U;
inline constexpr core::u64 maximum_thumbnail_payload_bytes =
    16U * 1'024U * 1'024U;

struct ThumbnailServiceStats final {
    core::u64 queued{};
    core::u64 generating{};
    core::u64 ready{};
    core::u64 stale{};
    core::u64 failed{};
    core::u64 cancelled{};
    core::u32 workers{};
    core::u32 pending_limit{};
    core::u32 prefetch_limit{};

    bool operator==(const ThumbnailServiceStats&) const = default;
};

// Decoder-neutral RGBA. The metadata fields make the profile's
// apply-rotation/apply-pixel-aspect semantics explicit even though the current
// renderer decoder reports ordinary square-pixel, unrotated frames.
struct ThumbnailRgbaFrame final {
    core::u32 width{};
    core::u32 height{};
    std::vector<core::u8> pixels;
    core::u8 clockwise_quarter_turns{};
    core::u32 pixel_aspect_numerator{1U};
    core::u32 pixel_aspect_denominator{1U};

    [[nodiscard]] bool valid() const noexcept;
};

class ThumbnailDecoder {
public:
    ThumbnailDecoder() = default;
    ThumbnailDecoder(const ThumbnailDecoder&) = delete;
    ThumbnailDecoder& operator=(const ThumbnailDecoder&) = delete;
    virtual ~ThumbnailDecoder() = default;

    [[nodiscard]] virtual core::u32
    frame_count(const std::string& source) = 0;
    [[nodiscard]] virtual ThumbnailRgbaFrame decode(
        const std::string& source, core::u32 source_ordinal,
        std::stop_token stop) = 0;
};

using ThumbnailDecoderFactory =
    std::function<std::unique_ptr<ThumbnailDecoder>()>;

// One synchronous, frame-exact renderer MediaSource per service worker.
[[nodiscard]] ThumbnailDecoderFactory default_thumbnail_decoder_factory();

// A model's thumbnail waiting to be drawn. Only the editor has a GPU, so the
// service hands these out and takes the pictures back.
struct ModelThumbnailRender final {
    assets::DerivativeKey key;
    std::string source;
};

enum class ThumbnailServiceErrorCode : core::u8 {
    InvalidAsset,
    MissingFingerprint,
    AllocationFailed,
};

struct ThumbnailServiceError final {
    ThumbnailServiceErrorCode code{ThumbnailServiceErrorCode::InvalidAsset};

    bool operator==(const ThumbnailServiceError&) const = default;
};

using ThumbnailRequestResult =
    core::Result<ThumbnailSnapshot, ThumbnailServiceError>;

class ThumbnailService final {
public:
    ThumbnailService(assets::DerivativeCache cache,
                     ThumbnailServiceConfig config = {});
    ThumbnailService(assets::DerivativeCache cache,
                     ThumbnailServiceConfig config,
                     ThumbnailDecoderFactory decoder_factory);
    ~ThumbnailService();

    ThumbnailService(const ThumbnailService&) = delete;
    ThumbnailService& operator=(const ThumbnailService&) = delete;
    ThumbnailService(ThumbnailService&&) = delete;
    ThumbnailService& operator=(ThumbnailService&&) = delete;

    // Existing Ready work is returned immediately. Missing/Stale work is
    // scheduled once per DerivativeKey; repeated visible requests upgrade an
    // existing prefetch rather than duplicating decode or asset identity.
    [[nodiscard]] ThumbnailRequestResult request(
        const assets::MediaAsset& asset,
        ThumbnailRequestPriority priority =
            ThumbnailRequestPriority::Visible);

    // Uses the same bounded workers/cache/retention policy as thumbnails but
    // decodes the frozen eight uniform source ordinals into one 8x1 PNG atlas
    // of 160x90 transparent-letterboxed tiles. The returned key remains a
    // derivative of the original persistent Asset NodeId.
    [[nodiscard]] ThumbnailRequestResult request_filmstrip(
        const assets::MediaAsset& asset,
        ThumbnailRequestPriority priority =
            ThumbnailRequestPriority::Visible);

    // A model's three-quarter view (ModelTurntable320x180Srgb). Ready work
    // is returned at once; otherwise it is Queued until the editor takes it
    // from take_model_renders, then Generating until complete_model_render
    // brings its picture back to be encoded and cached on a worker.
    [[nodiscard]] ThumbnailRequestResult request_model(
        const assets::ModelAsset& asset,
        ThumbnailRequestPriority priority =
            ThumbnailRequestPriority::Visible);
    // Up to `limit` waiting model renders, visible ones first, each marked
    // Generating as it is handed out.
    [[nodiscard]] std::vector<ModelThumbnailRender>
    take_model_renders(core::u32 limit);
    // The editor's render of `key`: rows top to bottom of profile-sized
    // RGBA8, or empty when it could not draw the model. A picture for work
    // no longer generating - cancelled by retain(), say - is dropped.
    void complete_model_render(const assets::DerivativeKey& key,
                               std::vector<core::u8> rgba);

    [[nodiscard]] std::optional<ThumbnailSnapshot>
    snapshot(const assets::DerivativeKey& key);
    [[nodiscard]] std::optional<ThumbnailSnapshot>
    cancel(const assets::DerivativeKey& key);

    // Reads and revalidates the PNG from its cache envelope. The envelope path
    // itself is intentionally not presented as if it were a raw PNG file.
    [[nodiscard]] core::Result<assets::DerivativeCacheLoad,
                               assets::DerivativeCacheError>
    load(const assets::DerivativeKey& key) noexcept;

    // Cancels every queued or generating key outside the visible rows plus
    // caller-chosen prefetch window. Completed snapshots remain queryable.
    void retain(std::span<const assets::DerivativeKey> needed);

    [[nodiscard]] ThumbnailServiceStats stats() const noexcept;

private:
    [[nodiscard]] ThumbnailRequestResult request_for_profile(
        const assets::MediaAsset& asset, assets::DerivativeProfileId profile,
        ThumbnailRequestPriority priority);

    struct Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace mediaserver::server
