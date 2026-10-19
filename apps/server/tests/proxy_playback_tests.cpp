#include "proxy_playback.hpp"
#include "project_asset_import.hpp"

#include <mediaserver/assets/asset_model.hpp>
#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace app = mediaserver::app;
namespace assets = mediaserver::assets;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace fs = std::filesystem;
namespace scene = mediaserver::scene;
namespace server = mediaserver::server;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} +
                                 std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define PLAYBACK_CHECK(...)                                                   \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        static std::atomic<core::u64> sequence{};
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = fs::temp_directory_path() /
            ("mediaserver_proxy_playback_" + std::to_string(stamp) + "_" +
             std::to_string(sequence.fetch_add(1U)));
        std::error_code error;
        if (!fs::create_directory(path_, error) || error) {
            throw std::runtime_error{"could not create temporary directory"};
        }
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        static_cast<void>(fs::remove_all(path_, ignored));
    }

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

[[nodiscard]] std::vector<core::u8> bytes(std::string_view value) {
    return {value.begin(), value.end()};
}

void write_file(const fs::path& path, std::span<const core::u8> content) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char*>(content.data()),
                 static_cast<std::streamsize>(content.size()));
    output.close();
    if (!output) {
        throw std::runtime_error{"could not write fixture"};
    }
}

[[nodiscard]] assets::ContentFingerprint fingerprint(const fs::path& path) {
    auto result = assets::source_fingerprint(path.string());
    PLAYBACK_CHECK(result.has_value());
    return result.value();
}

[[nodiscard]] assets::DerivativeCache open_cache(const fs::path& root) {
    auto result = assets::DerivativeCache::open(
        {root, 16U * 1'024U * 1'024U});
    PLAYBACK_CHECK(result.has_value());
    return std::move(result).value();
}

[[nodiscard]] assets::MediaAsset media_asset(
    const fs::path& source, assets::ProxyPlaybackPolicy policy,
    core::u64 id = 41U, core::u32 frame_count = 6U,
    assets::MediaFrameRate rate = {24'000U, 1'001U}) {
    return assets::MediaAsset{
        doc::NodeId{id},
        "Source",
        source.string(),
        assets::PixelDimensions{1'920U, 1'080U},
        frame_count,
        rate,
        fingerprint(source),
        policy == assets::ProxyPlaybackPolicy::Source
            ? std::optional<assets::DerivativeProfileId>{}
            : assets::DerivativeProfileId::Proxy720pCfrH264,
        policy,
    };
}

[[nodiscard]] assets::AssetRegistry registry(
    const assets::MediaAsset& asset) {
    return assets::AssetRegistry{{assets::Asset{asset}}};
}

[[nodiscard]] app::ProjectAssetAvailability availability(
    const assets::MediaAsset& asset,
    app::ProjectAssetPresence::State state =
        app::ProjectAssetPresence::State::Ready) {
    app::ProjectAssetAvailability result{};
    result.assets.push_back(app::ProjectAssetPresence{
        asset.id,
        asset.source,
        state == app::ProjectAssetPresence::State::Ready,
        state,
        asset.fingerprint,
        state == app::ProjectAssetPresence::State::Ready
            ? asset.fingerprint
            : std::optional<assets::ContentFingerprint>{},
    });
    if (state == app::ProjectAssetPresence::State::Missing) {
        result.missing = 1U;
    } else if (state ==
                   app::ProjectAssetPresence::State::ByteLengthMismatch ||
               state == app::ProjectAssetPresence::State::Sha256Mismatch) {
        result.mismatched = 1U;
    } else if (state ==
               app::ProjectAssetPresence::State::VerificationFailed) {
        result.verification_failed = 1U;
    }
    return result;
}

[[nodiscard]] scene::Scene evaluated_scene(
    const assets::MediaAsset& asset, core::u32 frame_index = 3U) {
    scene::Scene result{};
    result.canvas = scene::Canvas{1'920U, 1'080U};
    scene::MediaInstance media{};
    media.source = asset.source;
    media.frame_index = frame_index;
    media.asset = asset.id;
    media.source_node = doc::NodeId{701U};
    result.media.push_back(std::move(media));
    return result;
}

[[nodiscard]] const app::ProxyAssetRuntimeObservation& runtime_for(
    std::span<const app::ProxyAssetRuntimeObservation> observations,
    doc::NodeId asset) {
    const auto found = std::find_if(
        observations.begin(), observations.end(),
        [asset](const app::ProxyAssetRuntimeObservation& observation) {
            return observation.asset == asset;
        });
    PLAYBACK_CHECK(found != observations.end());
    return *found;
}

struct BackendState final {
    core::u32 calls{};
    std::vector<core::u8> payload{bytes("opaque-h264-test-payload")};
};

class Backend final : public server::ProxyEncoderBackend {
public:
    Backend(std::shared_ptr<BackendState> state,
            assets::MediaFrameRate rate, core::u32 frame_count)
        : state_(std::move(state)), rate_(rate), frame_count_(frame_count) {}

    server::ProxyBackendResult encode(
        const server::ProxyEncodeRequest& request,
        std::stop_token) override {
        ++state_->calls;
        write_file(request.output_file, state_->payload);
        auto map = assets::make_identity_proxy_frame_map(frame_count_, rate_);
        PLAYBACK_CHECK(map.has_value());
        return server::ProxyBackendResult::success(server::ProxyBackendOutput{
            request.source_fingerprint,
            server::ProxySourceMetadata{
                assets::PixelDimensions{1'920U, 1'080U},
                frame_count_, rate_, 1U, 1U, 0U,
                assets::DerivativeColorPrimaries::Rec709,
                assets::DerivativeTransfer::Rec709,
                assets::DerivativeMatrix::Rec709,
                assets::DerivativeColorRange::Limited,
                false,
            },
            server::ProxyEncodedMetadata{
                assets::PixelDimensions{1'280U, 720U},
                frame_count_, rate_, 1U, 1U, 0U,
                assets::DerivativeColorPrimaries::Rec709,
                assets::DerivativeTransfer::Rec709,
                assets::DerivativeMatrix::Rec709,
                assets::DerivativeColorRange::Limited,
                assets::DerivativeContainer::Mp4,
                assets::DerivativeCodec::H264,
                assets::DerivativePixelFormat::Yuv420p,
                assets::DerivativeAudioLayout::None,
                assets::DerivativeAudioCodec::None,
                0U,
                0U,
            },
            std::move(map).value(),
        });
    }

private:
    std::shared_ptr<BackendState> state_;
    assets::MediaFrameRate rate_;
    core::u32 frame_count_{};
};

void source_policy_selects_only_the_same_ready_source() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "source.mov";
    write_file(source, bytes("source-bytes"));
    const auto asset = media_asset(
        source, assets::ProxyPlaybackPolicy::Source);
    auto state = std::make_shared<BackendState>();
    server::ProxyGenerationService artifacts{
        open_cache(temporary.path() / "cache"),
        std::make_unique<Backend>(state, *asset.frame_rate,
                                  *asset.frame_count)};
    app::ProxyPlaybackService playback{artifacts};

    const scene::Scene input = evaluated_scene(asset);
    auto resolved = playback.resolve(input, registry(asset), availability(asset));
    PLAYBACK_CHECK(resolved.has_value());
    PLAYBACK_CHECK(resolved.value().ready());
    PLAYBACK_CHECK(resolved.value().scene.canvas == input.canvas);
    PLAYBACK_CHECK(resolved.value().scene.media == input.media);
    PLAYBACK_CHECK(resolved.value().placements.front().resolution.selection ==
                   assets::ProxyPlaybackSelection::Source);
    PLAYBACK_CHECK(resolved.value().placements.front().resolution.reason ==
                   assets::ProxyPlaybackReason::SourcePolicy);
    PLAYBACK_CHECK(state->calls == 0U);
}

void prefer_proxy_falls_back_visibly_but_never_to_another_source() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "prefer.mov";
    write_file(source, bytes("prefer-source"));
    const auto asset = media_asset(
        source, assets::ProxyPlaybackPolicy::PreferProxy);
    auto state = std::make_shared<BackendState>();
    server::ProxyGenerationService artifacts{
        open_cache(temporary.path() / "cache"),
        std::make_unique<Backend>(state, *asset.frame_rate,
                                  *asset.frame_count)};
    app::ProxyPlaybackService playback{artifacts};

    auto resolved = playback.resolve(
        evaluated_scene(asset), registry(asset), availability(asset));
    PLAYBACK_CHECK(resolved.has_value());
    PLAYBACK_CHECK(resolved.value().scene.media.size() == 1U);
    const auto& fallback = resolved.value().placements.front();
    PLAYBACK_CHECK(fallback.resolution.selection ==
                   assets::ProxyPlaybackSelection::Source);
    PLAYBACK_CHECK(fallback.resolution.reason ==
                   assets::ProxyPlaybackReason::CacheMissing);

    scene::Scene mismatched = evaluated_scene(asset);
    mismatched.media.front().source =
        (temporary.path() / "different.mov").string();
    resolved = playback.resolve(mismatched, registry(asset), availability(asset));
    PLAYBACK_CHECK(resolved.has_value());
    PLAYBACK_CHECK(resolved.value().scene.media.empty());
    PLAYBACK_CHECK(resolved.value().placements.front().issue ==
                   app::ProxyPlacementIssue::PlacementSourceMismatch);
    PLAYBACK_CHECK(resolved.value().placements.front().resolution.selection ==
                   assets::ProxyPlaybackSelection::Unready);
    PLAYBACK_CHECK(state->calls == 0U);
}

void require_proxy_is_unready_when_the_exact_cache_entry_is_missing() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "require.mov";
    write_file(source, bytes("require-source"));
    const auto asset = media_asset(
        source, assets::ProxyPlaybackPolicy::RequireProxy);
    auto state = std::make_shared<BackendState>();
    server::ProxyGenerationService artifacts{
        open_cache(temporary.path() / "cache"),
        std::make_unique<Backend>(state, *asset.frame_rate,
                                  *asset.frame_count)};
    app::ProxyPlaybackService playback{artifacts};

    const auto resolved = playback.resolve(
        evaluated_scene(asset), registry(asset), availability(asset));
    PLAYBACK_CHECK(resolved.has_value());
    PLAYBACK_CHECK(!resolved.value().ready());
    PLAYBACK_CHECK(resolved.value().scene.media.empty());
    PLAYBACK_CHECK(resolved.value().placements.front().resolution.selection ==
                   assets::ProxyPlaybackSelection::Unready);
    PLAYBACK_CHECK(resolved.value().placements.front().resolution.reason ==
                   assets::ProxyPlaybackReason::CacheMissing);
    // Playback has no generation path; even Require remains fail-closed.
    PLAYBACK_CHECK(state->calls == 0U);
}

void offline_media_preserves_placements_and_does_not_hold_output() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "offline.mov";
    write_file(path, bytes("offline-source"));
    const auto asset = media_asset(path,
                                  assets::ProxyPlaybackPolicy::Source);
    fs::remove(path);
    app::ProxyPlaybackService playback{nullptr};
    const auto original = evaluated_scene(asset);
    const auto missing = availability(asset, app::ProjectAssetPresence::State::Missing);
    PLAYBACK_CHECK(!missing.ready());
    PLAYBACK_CHECK(missing.render_ready());
    auto resolved = playback.resolve(original, registry(asset), missing);
    PLAYBACK_CHECK(resolved);
    PLAYBACK_CHECK(resolved.value().ready());
    PLAYBACK_CHECK(resolved.value().scene.media.size() == 1U);
    auto expected = original.media.front();
    expected.offline = true;
    PLAYBACK_CHECK(resolved.value().scene.media.front() == expected);
    PLAYBACK_CHECK(resolved.value().placements.front().offline);

    // Restoring the file switches back to the authored source without a new
    // placement or a changed persistent identity.
    resolved = playback.resolve(original, registry(asset), availability(asset));
    PLAYBACK_CHECK(resolved && resolved.value().ready());
    PLAYBACK_CHECK(resolved.value().scene.media.front() == original.media.front());

    auto loose = original;
    loose.media.front().asset = {};
    resolved = playback.resolve(loose, {}, missing);
    PLAYBACK_CHECK(resolved && resolved.value().ready());
    PLAYBACK_CHECK(resolved.value().scene.media.size() == 1U);
    PLAYBACK_CHECK(resolved.value().scene.media.front().offline);

    auto model_missing = missing;
    model_missing.assets.front().kind = assets::AssetKind::Model;
    PLAYBACK_CHECK(!model_missing.render_ready());
    PLAYBACK_CHECK(!availability(asset, app::ProjectAssetPresence::State::Sha256Mismatch).render_ready());
}

void ready_proxy_uses_the_exact_fractional_rate_map_without_identity_drift() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "fractional.mov";
    write_file(source, bytes("fractional-source"));
    constexpr assets::MediaFrameRate rate{24'000U, 1'001U};
    const auto asset = media_asset(
        source, assets::ProxyPlaybackPolicy::RequireProxy, 91U, 6U, rate);
    const auto unchanged_asset = asset;
    auto state = std::make_shared<BackendState>();
    server::ProxyGenerationService artifacts{
        open_cache(temporary.path() / "cache"),
        std::make_unique<Backend>(state, rate, 6U)};
    PLAYBACK_CHECK(artifacts.generate(asset).has_value());
    PLAYBACK_CHECK(state->calls == 1U);
    app::ProxyPlaybackService playback{artifacts};

    doc::Node placement{};
    placement.id = doc::NodeId{701U};
    placement.type = std::string{scene::schema::media_type};
    placement.properties.emplace(
        std::string{scene::schema::source},
        doc::PropertyValue{asset.source});
    placement.properties.emplace(
        std::string{scene::schema::asset_id},
        doc::PropertyValue{static_cast<std::int64_t>(asset.id.value())});
    placement.properties.emplace(
        std::string{scene::schema::sequence_count},
        doc::PropertyValue{std::int64_t{6}});
    placement.properties.emplace(
        std::string{scene::schema::source_rate_numerator},
        doc::PropertyValue{std::int64_t{24'000}});
    placement.properties.emplace(
        std::string{scene::schema::source_rate_denominator},
        doc::PropertyValue{std::int64_t{1'001}});
    const doc::Root projected = doc::Root{}
        .set(app::make_project_asset_node(asset.id, assets::Asset{asset}))
        .set(std::move(placement));
    assets::AssetModel asset_model;
    asset_model.reset(projected);
    const assets::AssetRegistry projected_registry = asset_model.evaluate();
    scene::SceneModel scene_model;
    scene_model.reset(projected);
    const scene::Scene input =
        scene_model.evaluate(scene::SceneTime{5U, 24'000U, 1'001U});
    PLAYBACK_CHECK(input.media.size() == 1U);
    PLAYBACK_CHECK(input.media.front().asset == asset.id);
    auto resolved = playback.resolve(
        input, projected_registry, availability(asset));
    PLAYBACK_CHECK(resolved.has_value());
    PLAYBACK_CHECK(!resolved.value().ready());
    PLAYBACK_CHECK(resolved.value().placements.front().resolution.reason ==
                   assets::ProxyPlaybackReason::CacheGenerating);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!resolved.value().ready() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        resolved = playback.resolve(
            input, projected_registry, availability(asset));
        PLAYBACK_CHECK(resolved.has_value());
    }
    PLAYBACK_CHECK(resolved.value().ready());
    PLAYBACK_CHECK(resolved.value().scene.media.size() == 1U);
    const auto& selected = resolved.value().scene.media.front();
    PLAYBACK_CHECK(selected.source != asset.source);
    PLAYBACK_CHECK(fs::is_regular_file(selected.source));
    PLAYBACK_CHECK(selected.frame_index == 5U);
    PLAYBACK_CHECK(selected.asset == asset.id);
    PLAYBACK_CHECK(selected.source_node == input.media.front().source_node);
    PLAYBACK_CHECK(resolved.value().placements.front().source_frame_index ==
                   5U);
    PLAYBACK_CHECK(resolved.value().placements.front().proxy_frame_index ==
                   5U);
    PLAYBACK_CHECK(resolved.value().placements.front().resolution.selection ==
                   assets::ProxyPlaybackSelection::Proxy);
    PLAYBACK_CHECK(asset == unchanged_asset);
    PLAYBACK_CHECK(state->calls == 1U);
}

void upcoming_proxies_materialize_before_air_without_holding_program() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "upcoming.mov";
    write_file(source, bytes("upcoming-source"));
    constexpr assets::MediaFrameRate rate{24'000U, 1'001U};
    const auto asset = media_asset(source, assets::ProxyPlaybackPolicy::RequireProxy, 92U, 6U, rate);
    auto state = std::make_shared<BackendState>();
    server::ProxyGenerationService artifacts{open_cache(temporary.path() / "cache"),
        std::make_unique<Backend>(state, rate, 6U)};
    app::ProxyPlaybackService playback{artifacts};
    auto input = evaluated_scene(asset);
    auto clip = input.media.front();
    clip.frame_index = 2U;
    clip.loop_first = 0U;
    clip.loop_count = 6U;
    input.media.clear();
    input.media_prewarm.push_back(scene::MediaPrewarm{
        clip.source, clip.frame_index, clip.source_node, clip.asset, 0U, 6U, 60U});
    // A required proxy missing for a future clip does not gate today's output.
    auto resolved = playback.resolve(input, registry(asset), availability(asset));
    PLAYBACK_CHECK(resolved && resolved.value().ready());
    PLAYBACK_CHECK(resolved.value().placements.empty());
    PLAYBACK_CHECK(resolved.value().scene.media_prewarm.empty());
    PLAYBACK_CHECK(artifacts.generate(asset).has_value());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    do {
        resolved = playback.resolve(input, registry(asset), availability(asset));
        PLAYBACK_CHECK(resolved && resolved.value().ready());
        PLAYBACK_CHECK(resolved.value().placements.empty());
        if (resolved.value().scene.media_prewarm.empty())
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
    } while (resolved.value().scene.media_prewarm.empty()
             && std::chrono::steady_clock::now() < deadline);
    PLAYBACK_CHECK(resolved.value().scene.media_prewarm.size() == 1U);
    const auto warm = resolved.value().scene.media_prewarm.front();
    PLAYBACK_CHECK(warm.source != asset.source && fs::is_regular_file(warm.source));
    PLAYBACK_CHECK(warm.frame_index == 2U && warm.asset == asset.id);
    PLAYBACK_CHECK(warm.loop_first == 0U && warm.loop_count == 6U);
    PLAYBACK_CHECK(warm.source_node == clip.source_node && warm.frames_until_start == 60U);
    // At the cut the proxy path is already local and available immediately.
    input.media_prewarm.clear();
    input.media.push_back(clip);
    const auto live = playback.resolve(input, registry(asset), availability(asset));
    PLAYBACK_CHECK(live && live.value().ready());
    PLAYBACK_CHECK(live.value().scene.media.front().source == warm.source);
    PLAYBACK_CHECK(live.value().scene.media.front().frame_index == warm.frame_index);
    PLAYBACK_CHECK(state->calls == 1U);
}

void off_air_assets_report_exact_cache_progress_without_generation() {
    TemporaryDirectory temporary;
    const fs::path source_path = temporary.path() / "source.mov";
    const fs::path prefer_path = temporary.path() / "prefer.mov";
    const fs::path require_path = temporary.path() / "require.mov";
    write_file(source_path, bytes("source"));
    write_file(prefer_path, bytes("prefer"));
    write_file(require_path, bytes("require"));
    const auto source = media_asset(
        source_path, assets::ProxyPlaybackPolicy::Source, 101U);
    const auto prefer = media_asset(
        prefer_path, assets::ProxyPlaybackPolicy::PreferProxy, 102U);
    const auto require = media_asset(
        require_path, assets::ProxyPlaybackPolicy::RequireProxy, 103U);
    assets::AssetRegistry registered{{
        assets::Asset{source}, assets::Asset{prefer}, assets::Asset{require}}};
    auto state = std::make_shared<BackendState>();
    server::ProxyGenerationService artifacts{
        open_cache(temporary.path() / "cache"),
        std::make_unique<Backend>(state, *prefer.frame_rate,
                                  *prefer.frame_count)};

    const auto observed = app::collect_proxy_asset_runtime(
        registered, availability(source), &artifacts, {});
    PLAYBACK_CHECK(observed.has_value());
    PLAYBACK_CHECK(observed.value().size() == 3U);
    PLAYBACK_CHECK(runtime_for(observed.value(), source.id).state ==
                   app::ProxyAssetRuntimeState::Source);
    const auto& prefer_status = runtime_for(observed.value(), prefer.id);
    PLAYBACK_CHECK(prefer_status.state ==
                   app::ProxyAssetRuntimeState::PreferFallback);
    PLAYBACK_CHECK(prefer_status.reason ==
                   assets::ProxyPlaybackReason::CacheMissing);
    PLAYBACK_CHECK(prefer_status.key == assets::requested_proxy_key(prefer));
    const auto& require_status = runtime_for(observed.value(), require.id);
    PLAYBACK_CHECK(require_status.state ==
                   app::ProxyAssetRuntimeState::RequiredUnready);
    PLAYBACK_CHECK(require_status.reason ==
                   assets::ProxyPlaybackReason::CacheMissing);
    PLAYBACK_CHECK(require_status.key == assets::requested_proxy_key(require));
    PLAYBACK_CHECK(state->calls == 0U);
    PLAYBACK_CHECK(!fs::exists(
        artifacts.cache_root() / ".proxy-playback"));
}

void off_air_ready_inspection_never_materializes_or_decodes_the_proxy() {
    TemporaryDirectory temporary;
    const fs::path source_path = temporary.path() / "ready.mov";
    write_file(source_path, bytes("ready-source"));
    const auto asset = media_asset(
        source_path, assets::ProxyPlaybackPolicy::RequireProxy, 111U);
    auto state = std::make_shared<BackendState>();
    server::ProxyGenerationService artifacts{
        open_cache(temporary.path() / "cache"),
        std::make_unique<Backend>(state, *asset.frame_rate,
                                  *asset.frame_count)};
    PLAYBACK_CHECK(artifacts.generate(asset).has_value());
    PLAYBACK_CHECK(state->calls == 1U);

    const auto observed = app::collect_proxy_asset_runtime(
        registry(asset), {}, &artifacts, {});
    PLAYBACK_CHECK(observed.has_value());
    const auto& ready = runtime_for(observed.value(), asset.id);
    PLAYBACK_CHECK(ready.state == app::ProxyAssetRuntimeState::ProxyReady);
    PLAYBACK_CHECK(ready.reason == assets::ProxyPlaybackReason::None);
    PLAYBACK_CHECK(ready.key == assets::requested_proxy_key(asset));
    PLAYBACK_CHECK(state->calls == 1U);
    PLAYBACK_CHECK(!fs::exists(
        artifacts.cache_root() / ".proxy-playback"));
}

void placement_runtime_aggregation_uses_the_documented_worst_state() {
    TemporaryDirectory temporary;
    const fs::path prefer_path = temporary.path() / "prefer.mov";
    const fs::path require_path = temporary.path() / "require.mov";
    write_file(prefer_path, bytes("prefer"));
    write_file(require_path, bytes("require"));
    const auto prefer = media_asset(
        prefer_path, assets::ProxyPlaybackPolicy::PreferProxy, 121U);
    const auto require = media_asset(
        require_path, assets::ProxyPlaybackPolicy::RequireProxy, 122U);
    assets::AssetRegistry registered{{
        assets::Asset{prefer}, assets::Asset{require}}};
    auto backend_state = std::make_shared<BackendState>();
    server::ProxyGenerationService artifacts{
        open_cache(temporary.path() / "cache"),
        std::make_unique<Backend>(backend_state, *prefer.frame_rate,
                                  *prefer.frame_count)};
    const auto prefer_key = assets::requested_proxy_key(prefer);
    const auto require_key = assets::requested_proxy_key(require);
    PLAYBACK_CHECK(prefer_key.has_value());
    PLAYBACK_CHECK(require_key.has_value());
    const std::array<app::ProxyPlacementObservation, 5U> placements{{
        {doc::NodeId{1U}, prefer.id,
         {prefer.id, assets::ProxyPlaybackSelection::Proxy,
          assets::ProxyPlaybackReason::None,
          assets::MediaSourceReadiness::Ready, prefer_key},
         app::ProxyPlacementIssue::None, 0U, 0U},
        {doc::NodeId{2U}, prefer.id,
         {prefer.id, assets::ProxyPlaybackSelection::Source,
          assets::ProxyPlaybackReason::CacheMissing,
          assets::MediaSourceReadiness::Ready, prefer_key},
         app::ProxyPlacementIssue::None, 0U, std::nullopt},
        {doc::NodeId{3U}, prefer.id,
         {prefer.id, assets::ProxyPlaybackSelection::Unready,
          assets::ProxyPlaybackReason::FrameMapInvalid,
          assets::MediaSourceReadiness::Ready, prefer_key},
         app::ProxyPlacementIssue::FrameMappingFailed, 0U, std::nullopt},
        {doc::NodeId{4U}, require.id,
         {require.id, assets::ProxyPlaybackSelection::Proxy,
          assets::ProxyPlaybackReason::None,
          assets::MediaSourceReadiness::Missing, require_key},
         app::ProxyPlacementIssue::None, 0U, 0U},
        {doc::NodeId{5U}, require.id,
         {require.id, assets::ProxyPlaybackSelection::Unready,
          assets::ProxyPlaybackReason::CacheGenerating,
          assets::MediaSourceReadiness::Missing, require_key},
         app::ProxyPlacementIssue::None, 0U, std::nullopt},
    }};

    const auto observed = app::collect_proxy_asset_runtime(
        registered, {}, &artifacts, placements);
    PLAYBACK_CHECK(observed.has_value());
    const auto& prefer_status = runtime_for(observed.value(), prefer.id);
    PLAYBACK_CHECK(prefer_status.state ==
                   app::ProxyAssetRuntimeState::Failed);
    PLAYBACK_CHECK(prefer_status.issue ==
                   app::ProxyPlacementIssue::FrameMappingFailed);
    const auto& require_status = runtime_for(observed.value(), require.id);
    PLAYBACK_CHECK(require_status.state ==
                   app::ProxyAssetRuntimeState::RequiredUnready);
    PLAYBACK_CHECK(require_status.reason ==
                   assets::ProxyPlaybackReason::CacheGenerating);
    PLAYBACK_CHECK(prefer_status.asset == prefer.id);
    PLAYBACK_CHECK(require_status.asset == require.id);
    PLAYBACK_CHECK(backend_state->calls == 0U);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 9U> cases{{
    {"upcoming proxies materialize before air without holding Program",
     &upcoming_proxies_materialize_before_air_without_holding_program},
    {"Source selects only the same ready source",
     &source_policy_selects_only_the_same_ready_source},
    {"Prefer falls back visibly but never to another source",
     &prefer_proxy_falls_back_visibly_but_never_to_another_source},
    {"Require is unready when its exact cache entry is missing",
     &require_proxy_is_unready_when_the_exact_cache_entry_is_missing},
    {"offline media preserves placements and does not hold output",
     &offline_media_preserves_placements_and_does_not_hold_output},
    {"ready proxy uses exact fractional-rate map without identity drift",
     &ready_proxy_uses_the_exact_fractional_rate_map_without_identity_drift},
    {"off-air assets report exact cache progress without generation",
     &off_air_assets_report_exact_cache_progress_without_generation},
    {"off-air ready inspection never materializes or decodes the proxy",
     &off_air_ready_inspection_never_materializes_or_decodes_the_proxy},
    {"placement runtime aggregation uses the documented worst state",
     &placement_runtime_aggregation_uses_the_documented_worst_state},
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
