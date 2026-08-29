#include "proxy_playback.hpp"

#include <mediaserver/assets/content_fingerprint.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <limits>
#include <utility>

namespace mediaserver::app {
namespace {

namespace fs = std::filesystem;

[[nodiscard]] assets::MediaSourceReadiness source_readiness(
    doc::NodeId asset, std::string_view source,
    const ProjectAssetAvailability& availability) noexcept {
    const auto found = std::find_if(
        availability.assets.begin(), availability.assets.end(),
        [&](const ProjectAssetPresence& presence) {
            return presence.source == source &&
                (asset == doc::NodeId{} || presence.asset == asset);
        });
    if (found == availability.assets.end()) {
        return assets::MediaSourceReadiness::Missing;
    }
    switch (found->state) {
    case ProjectAssetPresence::State::Ready:
        return assets::MediaSourceReadiness::Ready;
    case ProjectAssetPresence::State::Missing:
        return assets::MediaSourceReadiness::Missing;
    case ProjectAssetPresence::State::ByteLengthMismatch:
    case ProjectAssetPresence::State::Sha256Mismatch:
        return assets::MediaSourceReadiness::FingerprintMismatch;
    case ProjectAssetPresence::State::VerificationFailed:
        return assets::MediaSourceReadiness::VerificationFailed;
    }
    return assets::MediaSourceReadiness::VerificationFailed;
}

[[nodiscard]] assets::ProxyPlaybackResolution unmanaged_resolution(
    const scene::MediaInstance& media,
    const ProjectAssetAvailability& availability) noexcept {
    const auto readiness =
        source_readiness({}, media.source, availability);
    return assets::ProxyPlaybackResolution{
        {},
        readiness == assets::MediaSourceReadiness::Ready
            ? assets::ProxyPlaybackSelection::Source
            : assets::ProxyPlaybackSelection::Unready,
        readiness == assets::MediaSourceReadiness::Ready
            ? assets::ProxyPlaybackReason::SourcePolicy
            : assets::ProxyPlaybackReason::SourceUnavailable,
        readiness,
        std::nullopt,
    };
}

[[nodiscard]] assets::ProxyPlaybackResolution invalid_asset_resolution(
    doc::NodeId asset) noexcept {
    return assets::ProxyPlaybackResolution{
        asset,
        assets::ProxyPlaybackSelection::Unready,
        assets::ProxyPlaybackReason::InvalidConfiguration,
        assets::MediaSourceReadiness::VerificationFailed,
        std::nullopt,
    };
}

[[nodiscard]] bool authored_probe_matches(
    const assets::MediaAsset& asset,
    const server::ProxySourceMetadata& source) noexcept {
    return (!asset.dimensions.has_value() ||
            *asset.dimensions == source.dimensions) &&
        (!asset.frame_count.has_value() ||
         static_cast<core::u64>(*asset.frame_count) == source.frame_count) &&
        (!asset.frame_rate.has_value() ||
         *asset.frame_rate == source.frame_rate);
}

[[nodiscard]] assets::ProxyPlaybackResolution resolve_as_failed_cache(
    const assets::MediaAsset& asset,
    assets::MediaSourceReadiness source,
    const assets::DerivativeKey& key) noexcept {
    const assets::ProxyCacheObservation failed{
        key, assets::DerivativeState::Failed};
    return assets::resolve_proxy_playback(asset, source, &failed, nullptr);
}

[[nodiscard]] std::optional<assets::MediaSourceReadiness>
observed_source_readiness(
    const assets::MediaAsset& asset,
    const ProjectAssetAvailability& availability) noexcept {
    const auto found = std::find_if(
        availability.assets.begin(), availability.assets.end(),
        [&](const ProjectAssetPresence& presence) {
            return presence.asset == asset.id &&
                presence.source == asset.source;
        });
    if (found == availability.assets.end()) {
        // The content manifest deliberately excludes unused Library assets.
        // Off-air derivative inspection must not invent source readiness for
        // an asset that was not part of this manifest.
        return std::nullopt;
    }
    return source_readiness(asset.id, asset.source, availability);
}

[[nodiscard]] assets::ProxyPlaybackReason cache_reason(
    assets::DerivativeState state) noexcept {
    switch (state) {
    case assets::DerivativeState::Missing:
        return assets::ProxyPlaybackReason::CacheMissing;
    case assets::DerivativeState::Queued:
        return assets::ProxyPlaybackReason::CacheQueued;
    case assets::DerivativeState::Generating:
        return assets::ProxyPlaybackReason::CacheGenerating;
    case assets::DerivativeState::Ready:
        return assets::ProxyPlaybackReason::None;
    case assets::DerivativeState::Stale:
        return assets::ProxyPlaybackReason::CacheStale;
    case assets::DerivativeState::Failed:
        return assets::ProxyPlaybackReason::CacheFailed;
    case assets::DerivativeState::Cancelled:
        return assets::ProxyPlaybackReason::CacheCancelled;
    }
    return assets::ProxyPlaybackReason::CacheFailed;
}

[[nodiscard]] bool failed_runtime_reason(
    assets::ProxyPlaybackReason reason) noexcept {
    switch (reason) {
    case assets::ProxyPlaybackReason::InvalidConfiguration:
    case assets::ProxyPlaybackReason::ProxyNotConfigured:
    case assets::ProxyPlaybackReason::SourceFingerprintUnavailable:
    case assets::ProxyPlaybackReason::CacheObservationMissing:
    case assets::ProxyPlaybackReason::CacheKeyMismatch:
    case assets::ProxyPlaybackReason::CacheFailed:
    case assets::ProxyPlaybackReason::FrameMapMissing:
    case assets::ProxyPlaybackReason::FrameMapKeyMismatch:
    case assets::ProxyPlaybackReason::FrameMapInvalid:
        return true;
    case assets::ProxyPlaybackReason::None:
    case assets::ProxyPlaybackReason::SourcePolicy:
    case assets::ProxyPlaybackReason::SourceUnavailable:
    case assets::ProxyPlaybackReason::CacheMissing:
    case assets::ProxyPlaybackReason::CacheQueued:
    case assets::ProxyPlaybackReason::CacheGenerating:
    case assets::ProxyPlaybackReason::CacheStale:
    case assets::ProxyPlaybackReason::CacheCancelled:
        return false;
    }
    return true;
}

[[nodiscard]] int runtime_priority(
    ProxyAssetRuntimeState state) noexcept {
    switch (state) {
    case ProxyAssetRuntimeState::Source:
        return 0;
    case ProxyAssetRuntimeState::ProxyReady:
        return 1;
    case ProxyAssetRuntimeState::PreferFallback:
        return 2;
    case ProxyAssetRuntimeState::RequiredUnready:
        return 3;
    case ProxyAssetRuntimeState::Failed:
        return 4;
    }
    return 4;
}

[[nodiscard]] assets::ProxyPlaybackReason missing_key_reason(
    const assets::MediaAsset& asset) noexcept {
    if (!asset.requested_proxy_profile.has_value()) {
        return assets::ProxyPlaybackReason::ProxyNotConfigured;
    }
    if (!asset.fingerprint.has_value()) {
        return assets::ProxyPlaybackReason::SourceFingerprintUnavailable;
    }
    return assets::ProxyPlaybackReason::InvalidConfiguration;
}

[[nodiscard]] ProxyAssetRuntimeObservation off_air_observation(
    const assets::MediaAsset& asset,
    const ProjectAssetAvailability& availability,
    server::ProxyGenerationService* artifacts) {
    ProxyAssetRuntimeObservation result{};
    result.asset = asset.id;
    result.key = assets::requested_proxy_key(asset);
    if (!assets::valid_media_proxy_preferences(asset)) {
        result.state = ProxyAssetRuntimeState::Failed;
        result.reason = assets::ProxyPlaybackReason::InvalidConfiguration;
        return result;
    }

    const std::optional<assets::MediaSourceReadiness> source =
        observed_source_readiness(asset, availability);
    if (asset.proxy_playback_policy ==
        assets::ProxyPlaybackPolicy::Source) {
        result.state = source.has_value() &&
                *source != assets::MediaSourceReadiness::Ready
            ? ProxyAssetRuntimeState::Failed
            : ProxyAssetRuntimeState::Source;
        result.reason = result.state == ProxyAssetRuntimeState::Source
            ? assets::ProxyPlaybackReason::SourcePolicy
            : assets::ProxyPlaybackReason::SourceUnavailable;
        return result;
    }
    if (!result.key.has_value()) {
        result.state = ProxyAssetRuntimeState::Failed;
        result.reason = missing_key_reason(asset);
        return result;
    }
    if (artifacts == nullptr) {
        result.state = ProxyAssetRuntimeState::Failed;
        result.reason =
            assets::ProxyPlaybackReason::CacheObservationMissing;
        result.issue = ProxyPlacementIssue::CacheInspectionFailed;
        return result;
    }

    // inspect() validates the outer cache envelope for this full key but does
    // not load encoded bytes, parse media, or create a playback materialization.
    const auto inspected = artifacts->inspect(*result.key);
    if (!inspected) {
        result.state = ProxyAssetRuntimeState::Failed;
        result.reason = assets::ProxyPlaybackReason::CacheFailed;
        result.issue = ProxyPlacementIssue::CacheInspectionFailed;
        return result;
    }
    result.reason = cache_reason(inspected.value().state);
    if (inspected.value().state == assets::DerivativeState::Ready) {
        result.state = ProxyAssetRuntimeState::ProxyReady;
    } else if (failed_runtime_reason(result.reason)) {
        result.state = ProxyAssetRuntimeState::Failed;
    } else if (asset.proxy_playback_policy ==
               assets::ProxyPlaybackPolicy::RequireProxy) {
        result.state = ProxyAssetRuntimeState::RequiredUnready;
    } else if (source.has_value() &&
               *source != assets::MediaSourceReadiness::Ready) {
        result.state = ProxyAssetRuntimeState::Failed;
    } else {
        result.state = ProxyAssetRuntimeState::PreferFallback;
    }
    return result;
}

[[nodiscard]] ProxyAssetRuntimeObservation placement_observation(
    const ProxyPlacementObservation& placement,
    const assets::MediaAsset& asset) noexcept {
    ProxyAssetRuntimeObservation result{};
    result.asset = asset.id;
    result.key = placement.resolution.proxy_key;
    result.reason = placement.resolution.reason;
    result.issue = placement.issue;

    if (result.key != assets::requested_proxy_key(asset)) {
        result.state = ProxyAssetRuntimeState::Failed;
        result.reason = assets::ProxyPlaybackReason::CacheKeyMismatch;
        return result;
    }

    if (placement.issue != ProxyPlacementIssue::None &&
        !(placement.issue == ProxyPlacementIssue::AvailabilityMissing &&
          placement.resolution.selection ==
              assets::ProxyPlaybackSelection::Proxy)) {
        result.state = ProxyAssetRuntimeState::Failed;
        return result;
    }
    if (failed_runtime_reason(placement.resolution.reason)) {
        result.state = ProxyAssetRuntimeState::Failed;
        return result;
    }
    switch (placement.resolution.selection) {
    case assets::ProxyPlaybackSelection::Proxy:
        result.state = ProxyAssetRuntimeState::ProxyReady;
        break;
    case assets::ProxyPlaybackSelection::Source:
        result.state = placement.resolution.reason ==
                assets::ProxyPlaybackReason::SourcePolicy
            ? ProxyAssetRuntimeState::Source
            : ProxyAssetRuntimeState::PreferFallback;
        break;
    case assets::ProxyPlaybackSelection::Unready:
        result.state = asset.proxy_playback_policy ==
                assets::ProxyPlaybackPolicy::RequireProxy
            ? ProxyAssetRuntimeState::RequiredUnready
            : ProxyAssetRuntimeState::Failed;
        break;
    }
    return result;
}

} // namespace

bool ProxyPlaybackFrame::ready() const noexcept {
    return std::none_of(
        placements.begin(), placements.end(),
        [](const ProxyPlacementObservation& placement) {
            return placement.resolution.selection ==
                assets::ProxyPlaybackSelection::Unready;
        });
}

std::string_view proxy_placement_issue_name(
    ProxyPlacementIssue issue) noexcept {
    switch (issue) {
    case ProxyPlacementIssue::None:
        return "none";
    case ProxyPlacementIssue::MissingAssetIdentity:
        return "missing asset identity";
    case ProxyPlacementIssue::AssetNotFound:
        return "asset not found";
    case ProxyPlacementIssue::AssetKindMismatch:
        return "asset kind mismatch";
    case ProxyPlacementIssue::PlacementSourceMismatch:
        return "placement source differs from asset source";
    case ProxyPlacementIssue::AvailabilityMissing:
        return "source readiness unavailable";
    case ProxyPlacementIssue::CacheInspectionFailed:
        return "proxy cache inspection failed";
    case ProxyPlacementIssue::ArtifactLoadFailed:
        return "proxy artifact load failed";
    case ProxyPlacementIssue::ArtifactMetadataMismatch:
        return "proxy source metadata differs from asset metadata";
    case ProxyPlacementIssue::FrameMappingFailed:
        return "proxy frame mapping failed";
    case ProxyPlacementIssue::MaterializationFailed:
        return "proxy materialization failed";
    }
    return "unknown";
}

std::string_view proxy_playback_reason_name(
    assets::ProxyPlaybackReason reason) noexcept {
    switch (reason) {
    case assets::ProxyPlaybackReason::None:
        return "proxy ready";
    case assets::ProxyPlaybackReason::SourcePolicy:
        return "source policy";
    case assets::ProxyPlaybackReason::SourceUnavailable:
        return "source unavailable";
    case assets::ProxyPlaybackReason::InvalidConfiguration:
        return "invalid proxy configuration";
    case assets::ProxyPlaybackReason::ProxyNotConfigured:
        return "proxy not configured";
    case assets::ProxyPlaybackReason::SourceFingerprintUnavailable:
        return "source fingerprint unavailable";
    case assets::ProxyPlaybackReason::CacheObservationMissing:
        return "proxy cache unavailable";
    case assets::ProxyPlaybackReason::CacheKeyMismatch:
        return "proxy cache key mismatch";
    case assets::ProxyPlaybackReason::CacheMissing:
        return "proxy missing";
    case assets::ProxyPlaybackReason::CacheQueued:
        return "proxy queued";
    case assets::ProxyPlaybackReason::CacheGenerating:
        return "proxy generating";
    case assets::ProxyPlaybackReason::CacheStale:
        return "proxy stale";
    case assets::ProxyPlaybackReason::CacheFailed:
        return "proxy failed";
    case assets::ProxyPlaybackReason::CacheCancelled:
        return "proxy cancelled";
    case assets::ProxyPlaybackReason::FrameMapMissing:
        return "proxy frame map missing";
    case assets::ProxyPlaybackReason::FrameMapKeyMismatch:
        return "proxy frame map key mismatch";
    case assets::ProxyPlaybackReason::FrameMapInvalid:
        return "proxy frame map invalid";
    }
    return "unknown";
}

ProxyAssetRuntimeResult collect_proxy_asset_runtime(
    const assets::AssetRegistry& registry,
    const ProjectAssetAvailability& availability,
    server::ProxyGenerationService* artifacts,
    std::span<const ProxyPlacementObservation> placements) noexcept {
    try {
        std::vector<ProxyAssetRuntimeObservation> result;
        std::vector<bool> selected_active_placement;
        result.reserve(registry.assets.size());
        selected_active_placement.reserve(registry.assets.size());
        for (const assets::Asset& registered : registry.assets) {
            const auto* const asset =
                std::get_if<assets::MediaAsset>(&registered);
            if (asset == nullptr) {
                continue;
            }
            result.push_back(
                off_air_observation(*asset, availability, artifacts));
            selected_active_placement.push_back(false);
        }

        for (const ProxyPlacementObservation& placement : placements) {
            const auto found = std::find_if(
                result.begin(), result.end(),
                [&](const ProxyAssetRuntimeObservation& observation) {
                    return observation.asset == placement.asset;
                });
            if (found == result.end()) {
                continue;
            }
            const assets::Asset* const registered =
                registry.asset(placement.asset);
            const auto* const asset = registered != nullptr
                ? std::get_if<assets::MediaAsset>(registered)
                : nullptr;
            if (asset == nullptr) {
                continue;
            }
            const std::size_t index = static_cast<std::size_t>(
                std::distance(result.begin(), found));
            ProxyAssetRuntimeObservation candidate =
                placement_observation(placement, *asset);
            const bool replacing_off_air =
                !selected_active_placement[index];
            if (runtime_priority(candidate.state) >
                    runtime_priority(found->state) ||
                (replacing_off_air &&
                 runtime_priority(candidate.state) ==
                     runtime_priority(found->state))) {
                *found = std::move(candidate);
                selected_active_placement[index] = true;
            }
        }
        return ProxyAssetRuntimeResult::success(std::move(result));
    } catch (const std::bad_alloc&) {
        return ProxyAssetRuntimeResult::failure(
            ProxyPlaybackError{ProxyPlaybackErrorCode::AllocationFailed});
    } catch (...) {
        return ProxyAssetRuntimeResult::failure(
            ProxyPlaybackError{ProxyPlaybackErrorCode::AllocationFailed});
    }
}

ProxyPlaybackService::ProxyPlaybackService(
    server::ProxyGenerationService& artifacts)
    : ProxyPlaybackService(&artifacts) {}

ProxyPlaybackService::ProxyPlaybackService(
    server::ProxyGenerationService* artifacts)
    : artifacts_(artifacts) {}

ProxyPlaybackService::~ProxyPlaybackService() { reset(); }

bool ProxyPlaybackService::ensure_session_root(std::error_code& error) {
    if (!session_root_.empty()) {
        return true;
    }
    if (artifacts_ == nullptr) {
        error = std::make_error_code(std::errc::state_not_recoverable);
        return false;
    }
    const fs::path parent = artifacts_->cache_root() / ".proxy-playback";
    static_cast<void>(fs::create_directories(parent, error));
    if (error) {
        return false;
    }
    static std::atomic<core::u64> sequence{};
    const auto stamp = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    for (core::u32 attempt = 0U; attempt < 16U; ++attempt) {
        const fs::path candidate = parent /
            ("session-" + std::to_string(stamp) + "-" +
             std::to_string(sequence.fetch_add(1U)));
        error.clear();
        if (fs::create_directory(candidate, error)) {
            session_root_ = candidate;
            return true;
        }
        if (error && error != std::errc::file_exists) {
            return false;
        }
    }
    if (!error) {
        error = std::make_error_code(std::errc::file_exists);
    }
    return false;
}

std::optional<fs::path> ProxyPlaybackService::materialize(
    const server::ProxyArtifactLoad& artifact,
    server::ProxySourceMetadata source_metadata,
    assets::ProxyFrameMapObservation frame_map, std::stop_token stop,
    std::error_code& error) {
    const auto existing = std::find_if(
        materialized_.begin(), materialized_.end(),
        [&](const MaterializedProxy& value) {
            return value.key == artifact.receipt.key;
        });
    if (existing != materialized_.end()) {
        return existing->path;
    }
    if (stop.stop_requested() || artifact.encoded_payload.empty() ||
        artifact.encoded_payload.size() !=
            artifact.receipt.encoded_payload_fingerprint.byte_length ||
        assets::sha256_digest(artifact.encoded_payload) !=
            artifact.receipt.encoded_payload_fingerprint.sha256 ||
        !ensure_session_root(error)) {
        return std::nullopt;
    }

    const std::string key =
        assets::derivative_key_string(artifact.receipt.key);
    const fs::path published = session_root_ / (key + ".mp4");
    const fs::path temporary = session_root_ / (key + ".mp4.tmp");
    std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
    output.write(
        reinterpret_cast<const char*>(artifact.encoded_payload.data()),
        static_cast<std::streamsize>(artifact.encoded_payload.size()));
    output.close();
    if (!output || stop.stop_requested()) {
        error = stop.stop_requested()
            ? std::make_error_code(std::errc::operation_canceled)
            : std::make_error_code(std::errc::io_error);
        std::error_code ignored;
        static_cast<void>(fs::remove(temporary, ignored));
        return std::nullopt;
    }
    fs::rename(temporary, published, error);
    if (error) {
        std::error_code ignored;
        static_cast<void>(fs::remove(temporary, ignored));
        return std::nullopt;
    }
    materialized_.push_back(MaterializedProxy{
        artifact.receipt.key, published, std::move(source_metadata),
        std::move(frame_map)});
    return published;
}

ProxyPlaybackResult ProxyPlaybackService::resolve(
    const scene::Scene& evaluated, const assets::AssetRegistry& registry,
    const ProjectAssetAvailability& availability,
    std::stop_token stop) noexcept {
    try {
        ProxyPlaybackFrame output{};
        output.scene = evaluated;
        output.placements.reserve(evaluated.media.size());

        std::vector<scene::MediaInstance> ready_media;
        ready_media.reserve(evaluated.media.size());
        for (const scene::MediaInstance& original : evaluated.media) {
            scene::MediaInstance selected = original;
            ProxyPlacementObservation observation{};
            observation.placement = original.source_node;
            observation.asset = original.asset;
            observation.source_frame_index = original.frame_index;

            if (original.asset == doc::NodeId{}) {
                observation.issue = ProxyPlacementIssue::MissingAssetIdentity;
                observation.resolution =
                    unmanaged_resolution(original, availability);
                if (observation.resolution.selection ==
                    assets::ProxyPlaybackSelection::Source) {
                    ready_media.push_back(std::move(selected));
                }
                output.placements.push_back(std::move(observation));
                continue;
            }

            const assets::Asset* const registered =
                registry.asset(original.asset);
            if (registered == nullptr) {
                observation.issue = ProxyPlacementIssue::AssetNotFound;
                observation.resolution =
                    invalid_asset_resolution(original.asset);
                output.placements.push_back(std::move(observation));
                continue;
            }
            const auto* const asset =
                std::get_if<assets::MediaAsset>(registered);
            if (asset == nullptr) {
                observation.issue = ProxyPlacementIssue::AssetKindMismatch;
                observation.resolution =
                    invalid_asset_resolution(original.asset);
                output.placements.push_back(std::move(observation));
                continue;
            }
            if (asset->source != original.source) {
                observation.issue =
                    ProxyPlacementIssue::PlacementSourceMismatch;
                observation.resolution =
                    invalid_asset_resolution(original.asset);
                output.placements.push_back(std::move(observation));
                continue;
            }

            const assets::MediaSourceReadiness readiness = source_readiness(
                asset->id, asset->source, availability);
            if (readiness == assets::MediaSourceReadiness::Missing &&
                std::none_of(availability.assets.begin(),
                             availability.assets.end(),
                             [&](const ProjectAssetPresence& presence) {
                                 return presence.asset == asset->id &&
                                     presence.source == asset->source;
                             })) {
                observation.issue = ProxyPlacementIssue::AvailabilityMissing;
            }

            const std::optional<assets::DerivativeKey> key =
                assets::requested_proxy_key(*asset);
            std::optional<assets::ProxyCacheObservation> cache;
            std::optional<assets::ProxyFrameMapObservation> loaded_frame_map;
            const assets::ProxyFrameMapObservation* frame_map{};
            const server::ProxySourceMetadata* proxy_source_metadata{};
            std::optional<fs::path> materialized_path;
            std::optional<server::ProxyArtifactLoad> artifact;
            if (asset->proxy_playback_policy !=
                    assets::ProxyPlaybackPolicy::Source &&
                key.has_value()) {
                const auto inspected = artifacts_ == nullptr
                    ? core::Result<assets::DerivativeCacheEntry,
                                   assets::DerivativeCacheError>::failure(
                          assets::DerivativeCacheError{
                              assets::DerivativeCacheErrorCode::OpenFailed,
                              {}})
                    : artifacts_->inspect(*key);
                if (!inspected) {
                    observation.issue =
                        ProxyPlacementIssue::CacheInspectionFailed;
                    cache = assets::ProxyCacheObservation{
                        *key, assets::DerivativeState::Failed};
                } else {
                    cache = assets::ProxyCacheObservation{
                        *key, inspected.value().state};
                    if (inspected.value().state ==
                        assets::DerivativeState::Ready) {
                        const auto local = std::find_if(
                            materialized_.begin(), materialized_.end(),
                            [&](const MaterializedProxy& value) {
                                return value.key == *key;
                            });
                        if (local != materialized_.end()) {
                            if (!authored_probe_matches(
                                    *asset, local->source_metadata)) {
                                observation.issue = ProxyPlacementIssue::
                                    ArtifactMetadataMismatch;
                                cache->state =
                                    assets::DerivativeState::Failed;
                            } else {
                                proxy_source_metadata =
                                    &local->source_metadata;
                                frame_map = &local->frame_map;
                                materialized_path = local->path;
                            }
                        } else {
                            auto loaded = artifacts_->load(*key, stop);
                            if (!loaded) {
                                observation.issue =
                                    ProxyPlacementIssue::ArtifactLoadFailed;
                                cache->state =
                                    assets::DerivativeState::Failed;
                            } else if (!authored_probe_matches(
                                           *asset,
                                           loaded.value()
                                               .receipt.source_metadata)) {
                                observation.issue = ProxyPlacementIssue::
                                    ArtifactMetadataMismatch;
                                cache->state =
                                    assets::DerivativeState::Failed;
                            } else {
                                artifact = std::move(loaded).value();
                                proxy_source_metadata =
                                    &artifact->receipt.source_metadata;
                                loaded_frame_map.emplace(
                                    assets::ProxyFrameMapObservation{
                                        *key,
                                        std::move(
                                            artifact->receipt.frame_map)});
                                frame_map = &*loaded_frame_map;
                            }
                        }
                    }
                }
            }

            observation.resolution = assets::resolve_proxy_playback(
                *asset, readiness,
                cache.has_value() ? &*cache : nullptr,
                frame_map);
            if (observation.resolution.selection ==
                assets::ProxyPlaybackSelection::Proxy) {
                const auto source_time = assets::presentation_time_for_frame(
                    original.frame_index,
                    proxy_source_metadata->frame_rate);
                const auto mapped = source_time
                    ? assets::proxy_frame_at_or_before(
                          frame_map->frame_map, source_time.value())
                    : assets::ProxyFrameLookupResult::failure(
                          source_time.error());
                if (!mapped || !mapped.value().has_value() ||
                    *mapped.value() >
                        std::numeric_limits<core::u32>::max()) {
                    observation.issue =
                        ProxyPlacementIssue::FrameMappingFailed;
                    observation.resolution = resolve_as_failed_cache(
                        *asset, readiness, *key);
                } else {
                    std::error_code materialization_error;
                    const auto path = materialized_path.has_value()
                        ? materialized_path
                        : materialize(
                              *artifact, *proxy_source_metadata,
                              std::move(*loaded_frame_map), stop,
                              materialization_error);
                    if (!path.has_value()) {
                        observation.issue =
                            ProxyPlacementIssue::MaterializationFailed;
                        observation.resolution = resolve_as_failed_cache(
                            *asset, readiness, *key);
                    } else {
                        observation.proxy_frame_index =
                            static_cast<core::u32>(*mapped.value());
                        selected.source = path->string();
                        selected.frame_index =
                            *observation.proxy_frame_index;
                    }
                }
            }

            if (observation.resolution.selection !=
                assets::ProxyPlaybackSelection::Unready) {
                ready_media.push_back(std::move(selected));
            }
            output.placements.push_back(std::move(observation));
        }
        output.scene.media = std::move(ready_media);
        return ProxyPlaybackResult::success(std::move(output));
    } catch (const std::bad_alloc&) {
        return ProxyPlaybackResult::failure(
            ProxyPlaybackError{ProxyPlaybackErrorCode::AllocationFailed});
    } catch (...) {
        return ProxyPlaybackResult::failure(
            ProxyPlaybackError{ProxyPlaybackErrorCode::AllocationFailed});
    }
}

void ProxyPlaybackService::reset() noexcept {
    for (const MaterializedProxy& value : materialized_) {
        std::error_code ignored;
        static_cast<void>(fs::remove(value.path, ignored));
    }
    materialized_.clear();
    if (!session_root_.empty()) {
        const fs::path parent = session_root_.parent_path();
        std::error_code ignored;
        static_cast<void>(fs::remove(session_root_, ignored));
        ignored.clear();
        static_cast<void>(fs::remove(parent, ignored));
        session_root_.clear();
    }
}

} // namespace mediaserver::app
