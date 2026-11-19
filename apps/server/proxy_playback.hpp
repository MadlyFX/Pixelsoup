#pragma once

#include "project_assets.hpp"
#include "proxy_generation_service.hpp"

#include <mediaserver/assets/proxy_policy.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/scene/scene.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace mediaserver::app {

// Runtime-only failures that sit outside the authored Source/Prefer/Require
// policy. The policy decision remains in ProxyPlaybackResolution; this value
// explains why playback could not obtain or use the exact observation needed
// to make that decision.
enum class ProxyPlacementIssue : core::u8 {
    None,
    MissingAssetIdentity,
    AssetNotFound,
    AssetKindMismatch,
    PlacementSourceMismatch,
    AvailabilityMissing,
    CacheInspectionFailed,
    ArtifactLoadFailed,
    ArtifactMetadataMismatch,
    FrameMappingFailed,
    MaterializationFailed,
};

struct ProxyPlacementObservation final {
    doc::NodeId placement{};
    doc::NodeId asset{};
    assets::ProxyPlaybackResolution resolution;
    ProxyPlacementIssue issue{ProxyPlacementIssue::None};
    core::u32 source_frame_index{};
    std::optional<core::u32> proxy_frame_index;
    bool offline{};

    bool operator==(const ProxyPlacementObservation&) const = default;
};

// One asset-keyed summary for operator surfaces. Placement observations are
// reduced with the documented severity order, while an asset that is not
// currently evaluated is observed by inspecting only its exact derivative
// key. No encoded payload is loaded or materialized by this projection.
enum class ProxyAssetRuntimeState : core::u8 {
    Source,
    ProxyReady,
    PreferFallback,
    RequiredUnready,
    Failed,
};

struct ProxyAssetRuntimeObservation final {
    doc::NodeId asset{};
    std::optional<assets::DerivativeKey> key;
    ProxyAssetRuntimeState state{ProxyAssetRuntimeState::Source};
    assets::ProxyPlaybackReason reason{
        assets::ProxyPlaybackReason::SourcePolicy};
    ProxyPlacementIssue issue{ProxyPlacementIssue::None};

    bool operator==(const ProxyAssetRuntimeObservation&) const = default;
};

struct ProxyPlaybackFrame final {
    // A runtime copy. Proxy selection changes only media.source/frame_index;
    // persistent asset and placement identities remain exactly as evaluated.
    scene::Scene scene;
    std::vector<ProxyPlacementObservation> placements;

    [[nodiscard]] bool ready() const noexcept;
};

enum class ProxyPlaybackErrorCode : core::u8 {
    AllocationFailed,
};

struct ProxyPlaybackError final {
    ProxyPlaybackErrorCode code{ProxyPlaybackErrorCode::AllocationFailed};

    bool operator==(const ProxyPlaybackError&) const = default;
};

using ProxyPlaybackResult =
    core::Result<ProxyPlaybackFrame, ProxyPlaybackError>;
using ProxyAssetRuntimeResult =
    core::Result<std::vector<ProxyAssetRuntimeObservation>,
                 ProxyPlaybackError>;

// What one editor view's clips decoded, for the view's media menu: how many
// distinct placements drew a proxy, and how many fell back to the source
// only while their proxy is missing, queued or generating.
struct ViewProxySummary final {
    core::u32 proxied{};
    core::u32 waiting{};
    // How much of the waiting placements' proxies is made, each asset
    // counted once, as `progress` reported it.
    core::u64 waiting_made_frames{};
    core::u64 waiting_total_frames{};

    bool operator==(const ViewProxySummary &) const = default;
};

// Frames made and in all of an asset's proxy, or empty when unknown.
using ProxyProgressLookup =
    std::function<std::optional<std::pair<core::u64, core::u64>>(
        doc::NodeId asset)>;

[[nodiscard]] ViewProxySummary summarize_view_proxies(
    std::span<const ProxyPlacementObservation> observations,
    const ProxyProgressLookup &progress = {}) noexcept;

[[nodiscard]] std::string_view
proxy_placement_issue_name(ProxyPlacementIssue issue) noexcept;
[[nodiscard]] std::string_view proxy_playback_reason_name(
    assets::ProxyPlaybackReason reason) noexcept;

// Returns one observation for every registered MediaAsset in registry order.
// Existing placement observations override the off-air cache view, and the
// worst placement for an asset wins:
// Failed > RequiredUnready > PreferFallback > ProxyReady > Source.
[[nodiscard]] ProxyAssetRuntimeResult collect_proxy_asset_runtime(
    const assets::AssetRegistry& registry,
    const ProjectAssetAvailability& availability,
    server::ProxyGenerationService* artifacts,
    std::span<const ProxyPlacementObservation> placements) noexcept;

// Read-only playback adapter over the validated derivative service. It never
// calls generate(): an unavailable/missing proxy is resolved by the authored
// policy and therefore remains fail-closed for RequireProxy.
//
// The renderer accepts paths, while the cache stores a keyed artifact envelope.
// Ready MP4 bytes are materialized in a background worker and published
// atomically into a service-owned session directory using only the full
// DerivativeKey. reset() removes exactly those runtime files (for example on
// project replacement).
class ProxyPlaybackService final {
public:
    explicit ProxyPlaybackService(server::ProxyGenerationService& artifacts);
    explicit ProxyPlaybackService(server::ProxyGenerationService* artifacts);
    ~ProxyPlaybackService();

    ProxyPlaybackService(const ProxyPlaybackService&) = delete;
    ProxyPlaybackService& operator=(const ProxyPlaybackService&) = delete;
    ProxyPlaybackService(ProxyPlaybackService&&) = delete;
    ProxyPlaybackService& operator=(ProxyPlaybackService&&) = delete;

    [[nodiscard]] ProxyPlaybackResult resolve(
        const scene::Scene& evaluated,
        const assets::AssetRegistry& registry,
        const ProjectAssetAvailability& availability,
        std::stop_token stop = {}) noexcept;

    void reset() noexcept;

private:
    struct MaterializedProxy final {
        assets::DerivativeKey key;
        std::filesystem::path path;
        server::ProxySourceMetadata source_metadata;
        assets::ProxyFrameMapObservation frame_map;
    };

    struct PendingMaterialization;
    struct CacheInspections;
    using CacheInspection = core::Result<assets::DerivativeCacheEntry,
                                         assets::DerivativeCacheError>;
    struct FailedMaterialization final {
        assets::DerivativeKey key;
        ProxyPlacementIssue issue{ProxyPlacementIssue::ArtifactLoadFailed};
    };

    [[nodiscard]] static std::optional<MaterializedProxy> materialize(
        server::ProxyArtifactLoad artifact,
        const std::filesystem::path& session_root,
        std::stop_token stop,
        std::error_code& error);
    [[nodiscard]] bool ensure_session_root(std::error_code& error);
    void collect_completed_materialization() noexcept;
    [[nodiscard]] bool start_materialization(
        const assets::MediaAsset& asset,
        const assets::DerivativeKey& key) noexcept;
    // The cache state resolve() decides from. It reads the disk only for a
    // key it has not seen yet or after this service's generator published;
    // otherwise it returns what the inspection worker last read.
    [[nodiscard]] CacheInspection observe_cache(
        const assets::MediaAsset& asset,
        const assets::DerivativeKey& key) noexcept;

    server::ProxyGenerationService* artifacts_{};
    std::filesystem::path session_root_;
    std::vector<MaterializedProxy> materialized_;
    std::unique_ptr<PendingMaterialization> pending_materialization_;
    std::vector<FailedMaterialization> failed_materializations_;
    // Last, so its worker stops before anything above goes away.
    std::unique_ptr<CacheInspections> inspections_;
};

} // namespace mediaserver::app
