#include <mediaserver/assets/proxy_policy.hpp>

#include <array>
#include <exception>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace assets = mediaserver::assets;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} +
                                 std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define PROXY_POLICY_CHECK(...)                                               \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] assets::ContentFingerprint fingerprint(
    std::string_view revision) {
    const auto* const bytes =
        reinterpret_cast<const core::u8*>(revision.data());
    return assets::ContentFingerprint{
        static_cast<core::u64>(revision.size()),
        assets::sha256_digest(
            std::span<const core::u8>{bytes, revision.size()}),
    };
}

[[nodiscard]] assets::MediaAsset media(
    assets::ProxyPlaybackPolicy policy,
    std::optional<assets::DerivativeProfileId> profile =
        assets::DerivativeProfileId::Proxy720pCfrH264,
    bool fingerprinted = true) {
    return assets::MediaAsset{
        doc::NodeId{71U},
        "Camera A",
        "C:\\show\\camera-a.mov",
        assets::PixelDimensions{1'920U, 1'080U},
        120U,
        assets::MediaFrameRate{24U, 1U},
        fingerprinted
            ? std::optional<assets::ContentFingerprint>{fingerprint("rev-a")}
            : std::nullopt,
        profile,
        policy,
    };
}

[[nodiscard]] assets::ProxyFrameMap exact_frame_map(
    core::u64 frame_count = 3U) {
    auto built = assets::make_identity_proxy_frame_map(
        frame_count, assets::MediaFrameRate{24U, 1U});
    PROXY_POLICY_CHECK(built.has_value());
    return std::move(built).value();
}

void policy_and_profile_vocabulary_is_finite() {
    PROXY_POLICY_CHECK(assets::valid_proxy_playback_policy(
        assets::ProxyPlaybackPolicy::Source));
    PROXY_POLICY_CHECK(assets::valid_proxy_playback_policy(
        assets::ProxyPlaybackPolicy::PreferProxy));
    PROXY_POLICY_CHECK(assets::valid_proxy_playback_policy(
        assets::ProxyPlaybackPolicy::RequireProxy));
    PROXY_POLICY_CHECK(!assets::valid_proxy_playback_policy(
        static_cast<assets::ProxyPlaybackPolicy>(255U)));

    PROXY_POLICY_CHECK(assets::proxy_playback_policy_name(
                           assets::ProxyPlaybackPolicy::Source) == "source");
    PROXY_POLICY_CHECK(assets::proxy_playback_policy_from_name(
                           "prefer_proxy") ==
                       assets::ProxyPlaybackPolicy::PreferProxy);
    PROXY_POLICY_CHECK(assets::proxy_playback_policy_from_name(
                           "require_proxy") ==
                       assets::ProxyPlaybackPolicy::RequireProxy);
    PROXY_POLICY_CHECK(!assets::proxy_playback_policy_from_name(
                            "automatic")
                            .has_value());

    PROXY_POLICY_CHECK(assets::valid_requested_proxy_profile(
        assets::DerivativeProfileId::Proxy720pCfrH264));
    PROXY_POLICY_CHECK(!assets::valid_requested_proxy_profile(
        assets::DerivativeProfileId::Thumbnail320x180Srgb));
    PROXY_POLICY_CHECK(!assets::valid_requested_proxy_profile(
        static_cast<assets::DerivativeProfileId>(65'535U)));
    PROXY_POLICY_CHECK(assets::valid_media_proxy_preferences(
        media(assets::ProxyPlaybackPolicy::Source, std::nullopt)));
    PROXY_POLICY_CHECK(assets::valid_media_proxy_preferences(
        media(assets::ProxyPlaybackPolicy::PreferProxy, std::nullopt)));
}

void requested_key_uses_the_persistent_asset_and_exact_revision() {
    const assets::MediaAsset first =
        media(assets::ProxyPlaybackPolicy::PreferProxy);
    const auto first_key = assets::requested_proxy_key(first);
    PROXY_POLICY_CHECK(first_key.has_value());
    PROXY_POLICY_CHECK(first_key->asset == first.id);
    PROXY_POLICY_CHECK(first_key->profile ==
                       assets::DerivativeProfileId::Proxy720pCfrH264);
    PROXY_POLICY_CHECK(assets::valid_derivative_key(*first_key));

    assets::MediaAsset revised = first;
    revised.fingerprint = fingerprint("rev-b");
    const auto revised_key = assets::requested_proxy_key(revised);
    PROXY_POLICY_CHECK(revised_key.has_value());
    PROXY_POLICY_CHECK(revised_key->digest != first_key->digest);

    assets::MediaAsset unconfigured = first;
    unconfigured.requested_proxy_profile.reset();
    PROXY_POLICY_CHECK(!assets::requested_proxy_key(unconfigured).has_value());
    assets::MediaAsset unfingerprinted = first;
    unfingerprinted.fingerprint.reset();
    PROXY_POLICY_CHECK(
        !assets::requested_proxy_key(unfingerprinted).has_value());
}

void source_policy_never_substitutes_a_ready_proxy() {
    const assets::MediaAsset source_asset =
        media(assets::ProxyPlaybackPolicy::Source);
    const auto key = assets::requested_proxy_key(source_asset);
    PROXY_POLICY_CHECK(key.has_value());
    const assets::ProxyCacheObservation cache{*key,
                                               assets::DerivativeState::Ready};
    const assets::ProxyFrameMapObservation map{*key, exact_frame_map()};

    const auto ready = assets::resolve_proxy_playback(
        source_asset, assets::MediaSourceReadiness::Ready, &cache, &map);
    PROXY_POLICY_CHECK(ready.selection ==
                       assets::ProxyPlaybackSelection::Source);
    PROXY_POLICY_CHECK(ready.reason ==
                       assets::ProxyPlaybackReason::SourcePolicy);
    PROXY_POLICY_CHECK(ready.asset == source_asset.id);
    PROXY_POLICY_CHECK(ready.proxy_key == key);

    const auto missing = assets::resolve_proxy_playback(
        source_asset, assets::MediaSourceReadiness::Missing, &cache, &map);
    PROXY_POLICY_CHECK(missing.selection ==
                       assets::ProxyPlaybackSelection::Unready);
    PROXY_POLICY_CHECK(missing.reason ==
                       assets::ProxyPlaybackReason::SourceUnavailable);
}

void prefer_proxy_uses_only_an_exact_ready_payload_and_map() {
    const assets::MediaAsset asset =
        media(assets::ProxyPlaybackPolicy::PreferProxy);
    const auto key = assets::requested_proxy_key(asset);
    PROXY_POLICY_CHECK(key.has_value());
    const assets::ProxyCacheObservation cache{*key,
                                               assets::DerivativeState::Ready};
    const assets::ProxyFrameMapObservation map{*key, exact_frame_map()};

    const auto result = assets::resolve_proxy_playback(
        asset, assets::MediaSourceReadiness::Missing, &cache, &map);
    PROXY_POLICY_CHECK(result.selection ==
                       assets::ProxyPlaybackSelection::Proxy);
    PROXY_POLICY_CHECK(result.reason == assets::ProxyPlaybackReason::None);
    PROXY_POLICY_CHECK(result.asset == asset.id);
    PROXY_POLICY_CHECK(result.proxy_key == key);
    PROXY_POLICY_CHECK(result.proxy_key->asset == asset.id);
}

void prefer_proxy_reports_each_visible_source_fallback() {
    const assets::MediaAsset asset =
        media(assets::ProxyPlaybackPolicy::PreferProxy);
    const auto key = assets::requested_proxy_key(asset);
    PROXY_POLICY_CHECK(key.has_value());

    const auto absent = assets::resolve_proxy_playback(
        asset, assets::MediaSourceReadiness::Ready);
    PROXY_POLICY_CHECK(absent.selection ==
                       assets::ProxyPlaybackSelection::Source);
    PROXY_POLICY_CHECK(absent.reason ==
                       assets::ProxyPlaybackReason::CacheObservationMissing);
    PROXY_POLICY_CHECK(absent.asset == asset.id);

    const assets::MediaAsset unconfigured =
        media(assets::ProxyPlaybackPolicy::PreferProxy, std::nullopt);
    const auto no_profile = assets::resolve_proxy_playback(
        unconfigured, assets::MediaSourceReadiness::Ready);
    PROXY_POLICY_CHECK(no_profile.selection ==
                       assets::ProxyPlaybackSelection::Source);
    PROXY_POLICY_CHECK(no_profile.reason ==
                       assets::ProxyPlaybackReason::ProxyNotConfigured);

    struct StateCase final {
        assets::DerivativeState state;
        assets::ProxyPlaybackReason reason;
    };
    constexpr std::array<StateCase, 6U> states{{
        {assets::DerivativeState::Missing,
         assets::ProxyPlaybackReason::CacheMissing},
        {assets::DerivativeState::Queued,
         assets::ProxyPlaybackReason::CacheQueued},
        {assets::DerivativeState::Generating,
         assets::ProxyPlaybackReason::CacheGenerating},
        {assets::DerivativeState::Stale,
         assets::ProxyPlaybackReason::CacheStale},
        {assets::DerivativeState::Failed,
         assets::ProxyPlaybackReason::CacheFailed},
        {assets::DerivativeState::Cancelled,
         assets::ProxyPlaybackReason::CacheCancelled},
    }};
    for (const StateCase state : states) {
        const assets::ProxyCacheObservation observation{*key, state.state};
        const auto result = assets::resolve_proxy_playback(
            asset, assets::MediaSourceReadiness::Ready, &observation);
        PROXY_POLICY_CHECK(result.selection ==
                           assets::ProxyPlaybackSelection::Source);
        PROXY_POLICY_CHECK(result.reason == state.reason);
        PROXY_POLICY_CHECK(result.asset == asset.id);
        PROXY_POLICY_CHECK(result.proxy_key == key);
    }

    const assets::MediaAsset unfingerprinted =
        media(assets::ProxyPlaybackPolicy::PreferProxy,
              assets::DerivativeProfileId::Proxy720pCfrH264, false);
    const auto no_revision = assets::resolve_proxy_playback(
        unfingerprinted, assets::MediaSourceReadiness::Ready);
    PROXY_POLICY_CHECK(no_revision.selection ==
                       assets::ProxyPlaybackSelection::Source);
    PROXY_POLICY_CHECK(
        no_revision.reason ==
        assets::ProxyPlaybackReason::SourceFingerprintUnavailable);
}

void mismatched_keys_and_maps_cannot_cross_asset_revisions() {
    const assets::MediaAsset asset =
        media(assets::ProxyPlaybackPolicy::PreferProxy);
    const auto key = assets::requested_proxy_key(asset);
    PROXY_POLICY_CHECK(key.has_value());

    assets::MediaAsset other = asset;
    other.id = doc::NodeId{72U};
    const auto other_key = assets::requested_proxy_key(other);
    PROXY_POLICY_CHECK(other_key.has_value());

    const assets::ProxyCacheObservation wrong_cache{
        *other_key, assets::DerivativeState::Ready};
    const assets::ProxyFrameMapObservation exact_map{*key, exact_frame_map()};
    const auto cache_result = assets::resolve_proxy_playback(
        asset, assets::MediaSourceReadiness::Ready, &wrong_cache, &exact_map);
    PROXY_POLICY_CHECK(cache_result.selection ==
                       assets::ProxyPlaybackSelection::Source);
    PROXY_POLICY_CHECK(cache_result.reason ==
                       assets::ProxyPlaybackReason::CacheKeyMismatch);

    const assets::ProxyCacheObservation exact_cache{
        *key, assets::DerivativeState::Ready};
    const assets::ProxyFrameMapObservation wrong_map{*other_key,
                                                      exact_frame_map()};
    const auto map_result = assets::resolve_proxy_playback(
        asset, assets::MediaSourceReadiness::Ready, &exact_cache, &wrong_map);
    PROXY_POLICY_CHECK(map_result.selection ==
                       assets::ProxyPlaybackSelection::Source);
    PROXY_POLICY_CHECK(map_result.reason ==
                       assets::ProxyPlaybackReason::FrameMapKeyMismatch);

    const assets::ProxyFrameMapObservation empty_map{
        *key, assets::ProxyFrameMap{assets::MediaFrameRate{24U, 1U}, {}}};
    const auto empty_result = assets::resolve_proxy_playback(
        asset, assets::MediaSourceReadiness::Ready, &exact_cache, &empty_map);
    PROXY_POLICY_CHECK(empty_result.selection ==
                       assets::ProxyPlaybackSelection::Source);
    PROXY_POLICY_CHECK(empty_result.reason ==
                       assets::ProxyPlaybackReason::FrameMapInvalid);
}

void require_proxy_is_unready_instead_of_falling_back() {
    const assets::MediaAsset asset =
        media(assets::ProxyPlaybackPolicy::RequireProxy);
    const auto key = assets::requested_proxy_key(asset);
    PROXY_POLICY_CHECK(key.has_value());

    const auto missing = assets::resolve_proxy_playback(
        asset, assets::MediaSourceReadiness::Ready);
    PROXY_POLICY_CHECK(missing.selection ==
                       assets::ProxyPlaybackSelection::Unready);
    PROXY_POLICY_CHECK(missing.reason ==
                       assets::ProxyPlaybackReason::CacheObservationMissing);
    PROXY_POLICY_CHECK(missing.asset == asset.id);

    const assets::ProxyCacheObservation cache{*key,
                                               assets::DerivativeState::Ready};
    const assets::ProxyFrameMapObservation map{*key, exact_frame_map()};
    const auto ready = assets::resolve_proxy_playback(
        asset, assets::MediaSourceReadiness::FingerprintMismatch, &cache,
        &map);
    PROXY_POLICY_CHECK(ready.selection ==
                       assets::ProxyPlaybackSelection::Proxy);
    PROXY_POLICY_CHECK(ready.reason == assets::ProxyPlaybackReason::None);
}

void malformed_in_memory_preferences_fail_closed() {
    assets::MediaAsset invalid =
        media(assets::ProxyPlaybackPolicy::PreferProxy,
              static_cast<assets::DerivativeProfileId>(65'535U));
    const auto result = assets::resolve_proxy_playback(
        invalid, assets::MediaSourceReadiness::Ready);
    PROXY_POLICY_CHECK(result.selection ==
                       assets::ProxyPlaybackSelection::Unready);
    PROXY_POLICY_CHECK(result.reason ==
                       assets::ProxyPlaybackReason::InvalidConfiguration);

    invalid = media(assets::ProxyPlaybackPolicy::Source);
    invalid.proxy_playback_policy =
        static_cast<assets::ProxyPlaybackPolicy>(255U);
    const auto unknown = assets::resolve_proxy_playback(
        invalid, assets::MediaSourceReadiness::Ready);
    PROXY_POLICY_CHECK(unknown.selection ==
                       assets::ProxyPlaybackSelection::Unready);
    PROXY_POLICY_CHECK(unknown.reason ==
                       assets::ProxyPlaybackReason::InvalidConfiguration);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 8U> cases{{
    {"policy and profile vocabulary is finite",
     &policy_and_profile_vocabulary_is_finite},
    {"requested key uses the persistent asset and exact revision",
     &requested_key_uses_the_persistent_asset_and_exact_revision},
    {"source policy never substitutes a ready proxy",
     &source_policy_never_substitutes_a_ready_proxy},
    {"prefer proxy uses only an exact ready payload and map",
     &prefer_proxy_uses_only_an_exact_ready_payload_and_map},
    {"prefer proxy reports each visible source fallback",
     &prefer_proxy_reports_each_visible_source_fallback},
    {"mismatched keys and maps cannot cross asset revisions",
     &mismatched_keys_and_maps_cannot_cross_asset_revisions},
    {"require proxy is unready instead of falling back",
     &require_proxy_is_unready_instead_of_falling_back},
    {"malformed in memory preferences fail closed",
     &malformed_in_memory_preferences_fail_closed},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const Case& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& caught) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << caught.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
