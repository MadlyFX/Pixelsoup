#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/assets/derivative.hpp>
#include <mediaserver/assets/proxy_frame_map.hpp>

#include <optional>
#include <string_view>

namespace mediaserver::assets {

// Machine-local readiness of the authored source for this exact MediaAsset.
// The resolver never accepts an alternate source or a second asset identity.
enum class MediaSourceReadiness : core::u8 {
    Ready,
    Missing,
    FingerprintMismatch,
    VerificationFailed,
};

enum class ProxyPlaybackSelection : core::u8 {
    Source,
    Proxy,
    Unready,
};

// When Proxy is selected, reason is None. When PreferProxy falls back to
// Source, this says exactly why the requested derivative could not be used.
// source_readiness on ProxyPlaybackResolution separately explains why a Source
// fallback was or was not possible.
enum class ProxyPlaybackReason : core::u8 {
    None,
    SourcePolicy,
    SourceUnavailable,
    InvalidConfiguration,
    ProxyNotConfigured,
    SourceFingerprintUnavailable,
    CacheObservationMissing,
    CacheKeyMismatch,
    CacheMissing,
    CacheQueued,
    CacheGenerating,
    CacheStale,
    CacheFailed,
    CacheCancelled,
    FrameMapMissing,
    FrameMapKeyMismatch,
    FrameMapInvalid,
};

// Runtime observations retain the full key. A Ready observation for another
// asset, source revision, profile, or generator is never accepted.
struct ProxyCacheObservation final {
    DerivativeKey key;
    DerivativeState state{DerivativeState::Missing};

    bool operator==(const ProxyCacheObservation&) const = default;
};

// The sidecar is keyed independently of the media payload so a stale or
// mismatched frame map cannot be paired with an otherwise Ready proxy.
struct ProxyFrameMapObservation final {
    DerivativeKey key;
    ProxyFrameMap frame_map;
    // Set only by whoever already ran valid_proxy_frame_map on this exact
    // map, off the frame path. The resolver then skips its O(frames) check,
    // which would otherwise run for every placement on every frame.
    bool validated{false};

    bool operator==(const ProxyFrameMapObservation&) const = default;
};

struct ProxyPlaybackResolution final {
    // Always the persistent Library MediaAsset id supplied to the resolver.
    // A derivative never creates or replaces this identity.
    doc::NodeId asset{};
    ProxyPlaybackSelection selection{ProxyPlaybackSelection::Unready};
    ProxyPlaybackReason reason{ProxyPlaybackReason::InvalidConfiguration};
    MediaSourceReadiness source_readiness{MediaSourceReadiness::Missing};
    // Present when the requested proxy can be identified, including while it
    // is missing/pending/stale. This is a cache key, never an Asset id.
    std::optional<DerivativeKey> proxy_key;

    bool operator==(const ProxyPlaybackResolution&) const = default;
};

[[nodiscard]] bool
valid_proxy_playback_policy(ProxyPlaybackPolicy policy) noexcept;
[[nodiscard]] bool
valid_requested_proxy_profile(DerivativeProfileId profile) noexcept;
[[nodiscard]] bool
valid_media_proxy_preferences(const MediaAsset& asset) noexcept;

[[nodiscard]] std::string_view
proxy_playback_policy_name(ProxyPlaybackPolicy policy) noexcept;
[[nodiscard]] std::optional<ProxyPlaybackPolicy>
proxy_playback_policy_from_name(std::string_view name) noexcept;

// Returns no key when the asset has no requested profile, lacks a fingerprint,
// carries an invalid/non-proxy profile, or has no persistent NodeId.
[[nodiscard]] std::optional<DerivativeKey>
requested_proxy_key(const MediaAsset& asset) noexcept;

// Pure policy decision. Observations are optional because the cache and
// sidecar may not exist yet; neither is mutated or opened by this function.
[[nodiscard]] ProxyPlaybackResolution resolve_proxy_playback(
    const MediaAsset& asset, MediaSourceReadiness source_readiness,
    const ProxyCacheObservation* cache_observation = nullptr,
    const ProxyFrameMapObservation* frame_map_observation = nullptr) noexcept;

} // namespace mediaserver::assets
