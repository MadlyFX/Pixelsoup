#include <mediaserver/assets/proxy_policy.hpp>

#include <mediaserver/assets/asset_schema.hpp>

#include <utility>

namespace mediaserver::assets {
namespace {

[[nodiscard]] ProxyPlaybackResolution unavailable_proxy(
    const MediaAsset& asset, MediaSourceReadiness source_readiness,
    ProxyPlaybackReason reason,
    std::optional<DerivativeKey> proxy_key = std::nullopt) noexcept {
    const bool may_fallback =
        asset.proxy_playback_policy == ProxyPlaybackPolicy::PreferProxy &&
        source_readiness == MediaSourceReadiness::Ready;
    return ProxyPlaybackResolution{
        asset.id,
        may_fallback ? ProxyPlaybackSelection::Source
                     : ProxyPlaybackSelection::Unready,
        reason,
        source_readiness,
        std::move(proxy_key),
    };
}

[[nodiscard]] ProxyPlaybackReason cache_reason(
    DerivativeState state) noexcept {
    switch (state) {
    case DerivativeState::Missing:
        return ProxyPlaybackReason::CacheMissing;
    case DerivativeState::Queued:
        return ProxyPlaybackReason::CacheQueued;
    case DerivativeState::Generating:
        return ProxyPlaybackReason::CacheGenerating;
    case DerivativeState::Ready:
        return ProxyPlaybackReason::None;
    case DerivativeState::Stale:
        return ProxyPlaybackReason::CacheStale;
    case DerivativeState::Failed:
        return ProxyPlaybackReason::CacheFailed;
    case DerivativeState::Cancelled:
        return ProxyPlaybackReason::CacheCancelled;
    }
    return ProxyPlaybackReason::CacheFailed;
}

} // namespace

bool valid_proxy_playback_policy(ProxyPlaybackPolicy policy) noexcept {
    switch (policy) {
    case ProxyPlaybackPolicy::Source:
    case ProxyPlaybackPolicy::PreferProxy:
    case ProxyPlaybackPolicy::RequireProxy:
        return true;
    }
    return false;
}

bool valid_requested_proxy_profile(DerivativeProfileId profile) noexcept {
    const DerivativeProfile* const definition = derivative_profile(profile);
    return definition != nullptr && definition->kind == DerivativeKind::Proxy;
}

bool valid_media_proxy_preferences(const MediaAsset& asset) noexcept {
    if (!valid_proxy_playback_policy(asset.proxy_playback_policy)) {
        return false;
    }
    if (asset.requested_proxy_profile.has_value() &&
        !valid_requested_proxy_profile(*asset.requested_proxy_profile)) {
        return false;
    }
    return true;
}

std::string_view proxy_playback_policy_name(
    ProxyPlaybackPolicy policy) noexcept {
    switch (policy) {
    case ProxyPlaybackPolicy::Source:
        return schema::proxy_policy_source;
    case ProxyPlaybackPolicy::PreferProxy:
        return schema::proxy_policy_prefer;
    case ProxyPlaybackPolicy::RequireProxy:
        return schema::proxy_policy_require;
    }
    return {};
}

std::optional<ProxyPlaybackPolicy> proxy_playback_policy_from_name(
    std::string_view name) noexcept {
    if (name == schema::proxy_policy_source) {
        return ProxyPlaybackPolicy::Source;
    }
    if (name == schema::proxy_policy_prefer) {
        return ProxyPlaybackPolicy::PreferProxy;
    }
    if (name == schema::proxy_policy_require) {
        return ProxyPlaybackPolicy::RequireProxy;
    }
    return std::nullopt;
}

std::optional<DerivativeKey>
requested_proxy_key(const MediaAsset& asset) noexcept {
    if (!asset.id || !asset.fingerprint.has_value() ||
        !asset.requested_proxy_profile.has_value() ||
        !valid_requested_proxy_profile(*asset.requested_proxy_profile)) {
        return std::nullopt;
    }
    const DerivativeProfile* const profile =
        derivative_profile(*asset.requested_proxy_profile);
    if (profile == nullptr) {
        return std::nullopt;
    }
    return make_derivative_key(asset.id, *asset.fingerprint, *profile);
}

ProxyPlaybackResolution resolve_proxy_playback(
    const MediaAsset& asset, MediaSourceReadiness source_readiness,
    const ProxyCacheObservation* cache_observation,
    const ProxyFrameMapObservation* frame_map_observation) noexcept {
    if (!asset.id || !valid_media_proxy_preferences(asset)) {
        return ProxyPlaybackResolution{
            asset.id,
            ProxyPlaybackSelection::Unready,
            ProxyPlaybackReason::InvalidConfiguration,
            source_readiness,
            std::nullopt,
        };
    }

    const std::optional<DerivativeKey> key = requested_proxy_key(asset);
    if (asset.proxy_playback_policy == ProxyPlaybackPolicy::Source) {
        return ProxyPlaybackResolution{
            asset.id,
            source_readiness == MediaSourceReadiness::Ready
                ? ProxyPlaybackSelection::Source
                : ProxyPlaybackSelection::Unready,
            source_readiness == MediaSourceReadiness::Ready
                ? ProxyPlaybackReason::SourcePolicy
                : ProxyPlaybackReason::SourceUnavailable,
            source_readiness,
            key,
        };
    }

    if (!asset.requested_proxy_profile.has_value()) {
        return unavailable_proxy(asset, source_readiness,
                                 ProxyPlaybackReason::ProxyNotConfigured);
    }
    if (!asset.fingerprint.has_value()) {
        return unavailable_proxy(
            asset, source_readiness,
            ProxyPlaybackReason::SourceFingerprintUnavailable);
    }
    if (!key.has_value()) {
        return unavailable_proxy(asset, source_readiness,
                                 ProxyPlaybackReason::InvalidConfiguration);
    }
    if (cache_observation == nullptr) {
        return unavailable_proxy(
            asset, source_readiness,
            ProxyPlaybackReason::CacheObservationMissing, key);
    }
    // The whole proxy, or one segment of it standing in until the whole one
    // is ready; the frame map then says which source frames it holds.
    const auto covers = [&key](const DerivativeKey& observed) {
        return observed == *key ||
            (observed.segment.has_value() &&
             observed == derivative_segment_key(*key, *observed.segment));
    };
    if (!valid_derivative_key(cache_observation->key) ||
        !covers(cache_observation->key)) {
        return unavailable_proxy(asset, source_readiness,
                                 ProxyPlaybackReason::CacheKeyMismatch, key);
    }
    const ProxyPlaybackReason observed_reason =
        cache_reason(cache_observation->state);
    if (observed_reason != ProxyPlaybackReason::None) {
        return unavailable_proxy(asset, source_readiness, observed_reason,
                                 key);
    }
    if (frame_map_observation == nullptr) {
        return unavailable_proxy(asset, source_readiness,
                                 ProxyPlaybackReason::FrameMapMissing, key);
    }
    if (!valid_derivative_key(frame_map_observation->key) ||
        frame_map_observation->key != cache_observation->key) {
        return unavailable_proxy(
            asset, source_readiness,
            ProxyPlaybackReason::FrameMapKeyMismatch, key);
    }
    if (frame_map_observation->frame_map.source_time_by_proxy_frame.empty() ||
        (!frame_map_observation->validated &&
         !valid_proxy_frame_map(frame_map_observation->frame_map))) {
        return unavailable_proxy(asset, source_readiness,
                                 ProxyPlaybackReason::FrameMapInvalid, key);
    }

    return ProxyPlaybackResolution{
        asset.id,
        ProxyPlaybackSelection::Proxy,
        ProxyPlaybackReason::None,
        source_readiness,
        key,
    };
}

} // namespace mediaserver::assets
