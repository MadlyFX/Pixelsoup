#include "playback_media.hpp"
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
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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
        // A segment covers its range of the source; a join, or a whole-file
        // encode, all of it.
        const server::ProxySourceRange covered = request.range.value_or(
            server::ProxySourceRange{0U, frame_count_});
        auto map = assets::make_identity_proxy_frame_map(covered.count, rate_,
                                                         covered.first);
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
                covered.count, rate_, 1U, 1U, 0U,
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

// The cache state is remembered between frames rather than read from disk
// for every placement. A proxy this process publishes is still seen on the
// very next frame, and a change made outside it is picked up by the worker.
void remembered_cache_state_follows_publications_and_outside_changes() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "remembered.mov";
    write_file(source, bytes("remembered-source"));
    const auto asset = media_asset(
        source, assets::ProxyPlaybackPolicy::PreferProxy, 57U);
    const fs::path cache_root = temporary.path() / "cache";
    auto state = std::make_shared<BackendState>();
    server::ProxyGenerationService artifacts{
        open_cache(cache_root),
        std::make_unique<Backend>(state, *asset.frame_rate,
                                  *asset.frame_count)};
    app::ProxyPlaybackService playback{artifacts};
    const auto reason_now = [&] {
        const auto resolved = playback.resolve(
            evaluated_scene(asset), registry(asset), availability(asset));
        PLAYBACK_CHECK(resolved.has_value());
        return resolved.value().placements.front().resolution;
    };

    PLAYBACK_CHECK(reason_now().reason ==
                   assets::ProxyPlaybackReason::CacheMissing);
    const core::u64 before = artifacts.publications();
    PLAYBACK_CHECK(artifacts.generate(asset).has_value());
    PLAYBACK_CHECK(artifacts.publications() > before);
    // No wait: the publication alone makes this frame read the cache again.
    PLAYBACK_CHECK(reason_now().reason !=
                   assets::ProxyPlaybackReason::CacheMissing);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    assets::ProxyPlaybackResolution resolution = reason_now();
    while (resolution.selection != assets::ProxyPlaybackSelection::Proxy &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        resolution = reason_now();
    }
    PLAYBACK_CHECK(resolution.selection ==
                   assets::ProxyPlaybackSelection::Proxy);

    // Removed behind the service's back: nothing publishes, so only the
    // inspection worker can notice, within a few refreshes.
    const auto key = assets::requested_proxy_key(asset);
    PLAYBACK_CHECK(key.has_value());
    const fs::path entry = assets::derivative_cache_path(cache_root, *key);
    PLAYBACK_CHECK(fs::remove(entry));
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (resolution.reason != assets::ProxyPlaybackReason::CacheMissing &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        resolution = reason_now();
    }
    PLAYBACK_CHECK(resolution.reason ==
                   assets::ProxyPlaybackReason::CacheMissing);
    PLAYBACK_CHECK(resolution.selection ==
                   assets::ProxyPlaybackSelection::Source);
    PLAYBACK_CHECK(state->calls == 1U);
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
    placement.properties.emplace(std::string{scene::schema::scene_id},
                                 doc::PropertyValue{std::int64_t{700}});
    doc::Node stage{};
    stage.id = doc::NodeId{700U};
    stage.type = std::string{scene::schema::stage_type};
    const doc::Root projected = doc::Root{}
        .set(std::move(stage))
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

// Before the whole proxy exists, a published segment plays the frames it
// holds, as frames of its own file; frames whose segment is not published
// play the source. Near the end of a segment the next one is decoded ahead,
// and once the segments are joined the whole file takes over.
void published_segments_play_until_the_whole_proxy_is_ready() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "segmented.mov";
    write_file(source, bytes("segmented-source"));
    constexpr assets::MediaFrameRate rate{24'000U, 1'001U};
    // Segments of 240, 240 and 120 frames.
    const auto asset = media_asset(
        source, assets::ProxyPlaybackPolicy::PreferProxy, 93U, 600U, rate);
    auto state = std::make_shared<BackendState>();
    server::ProxyGenerationService artifacts{
        open_cache(temporary.path() / "cache"),
        std::make_unique<Backend>(state, rate, 600U)};
    using Step = server::ProxyGenerationService::Step;
    PLAYBACK_CHECK(artifacts.generate(asset, {}, {}, Step{false, 1U}));
    app::ProxyPlaybackService playback{artifacts};

    // Resolves until nothing more changes: segments are copied out one at a
    // time, off the calling thread.
    const auto settle = [&](core::u32 frame) {
        auto resolved = playback.resolve(evaluated_scene(asset, frame),
                                         registry(asset), availability(asset));
        PLAYBACK_CHECK(resolved.has_value());
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{2};
        for (int quiet = 0; quiet < 20 &&
             std::chrono::steady_clock::now() < deadline;) {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
            auto next = playback.resolve(evaluated_scene(asset, frame),
                                         registry(asset),
                                         availability(asset));
            PLAYBACK_CHECK(next.has_value());
            quiet = next.value().scene.media == resolved.value().scene.media &&
                    next.value().scene.media_prewarm ==
                        resolved.value().scene.media_prewarm &&
                    next.value().placements == resolved.value().placements
                ? quiet + 1 : 0;
            resolved = std::move(next);
        }
        return std::move(resolved).value();
    };

    // Frame 300 is frame 60 of segment 1, read from the segment's own file.
    auto in_segment = settle(300U);
    PLAYBACK_CHECK(in_segment.placements.front().resolution.selection ==
                   assets::ProxyPlaybackSelection::Proxy);
    PLAYBACK_CHECK(in_segment.placements.front().proxy_frame_index == 60U);
    const auto& segment_media = in_segment.scene.media.front();
    PLAYBACK_CHECK(segment_media.source != asset.source);
    PLAYBACK_CHECK(fs::is_regular_file(segment_media.source));
    PLAYBACK_CHECK(segment_media.frame_index == 60U);
    PLAYBACK_CHECK(segment_media.loop_count == 0U);
    PLAYBACK_CHECK(in_segment.scene.media_prewarm.empty());

    // Frame 100 lies in segment 0, which is not published: the source plays.
    auto outside = settle(100U);
    PLAYBACK_CHECK(outside.placements.front().resolution.selection ==
                   assets::ProxyPlaybackSelection::Source);
    PLAYBACK_CHECK(outside.scene.media.front().source == asset.source);
    PLAYBACK_CHECK(outside.scene.media.front().frame_index == 100U);

    // Ten frames before segment 2, once it is published, it is decoded
    // ahead from its first frame.
    PLAYBACK_CHECK(artifacts.generate(asset, {}, {}, Step{false, 2U}));
    static_cast<void>(settle(480U));
    auto near_end = settle(470U);
    PLAYBACK_CHECK(near_end.scene.media.front().frame_index == 230U);
    PLAYBACK_CHECK(near_end.scene.media_prewarm.size() == 1U);
    const auto& upcoming = near_end.scene.media_prewarm.front();
    PLAYBACK_CHECK(upcoming.frame_index == 0U);
    PLAYBACK_CHECK(upcoming.frames_until_start == 10U);
    PLAYBACK_CHECK(upcoming.source != segment_media.source);
    PLAYBACK_CHECK(upcoming.asset == asset.id);

    // Joined, the whole proxy plays frame 300 as its own frame 300.
    PLAYBACK_CHECK(artifacts.generate(asset, {}, {}, Step{false, 0U}));
    PLAYBACK_CHECK(artifacts.generate(asset, {}, {}, Step{false, {}, true}));
    auto whole = settle(300U);
    PLAYBACK_CHECK(whole.placements.front().resolution.selection ==
                   assets::ProxyPlaybackSelection::Proxy);
    PLAYBACK_CHECK(whole.scene.media.front().frame_index == 300U);
    PLAYBACK_CHECK(whole.scene.media.front().source != segment_media.source);
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

void view_summaries_count_each_placement_once_and_only_waiting_fallbacks() {
    using Reason = assets::ProxyPlaybackReason;
    using Selection = assets::ProxyPlaybackSelection;
    const auto observed = [](core::u64 placement, Selection selection,
                             Reason reason, bool offline = false) {
        app::ProxyPlacementObservation observation{};
        observation.placement = doc::NodeId{placement};
        observation.asset = doc::NodeId{placement + 100U};
        observation.resolution.selection = selection;
        observation.resolution.reason = reason;
        observation.offline = offline;
        return observation;
    };
    const std::array<app::ProxyPlacementObservation, 9U> drawn{{
        observed(1U, Selection::Proxy, Reason::None),
        // A transition draws placement 1 again; it is still one clip.
        observed(1U, Selection::Proxy, Reason::None),
        observed(2U, Selection::Proxy, Reason::None),
        observed(3U, Selection::Source, Reason::CacheGenerating),
        observed(4U, Selection::Source, Reason::CacheQueued),
        observed(5U, Selection::Source, Reason::CacheMissing),
        // Source by policy, a failed proxy and an offline clip are not
        // waiting for anything.
        observed(6U, Selection::Source, Reason::SourcePolicy),
        observed(7U, Selection::Source, Reason::CacheFailed),
        observed(8U, Selection::Source, Reason::CacheGenerating, true),
    }};
    const app::ViewProxySummary summary = app::summarize_view_proxies(drawn);
    PLAYBACK_CHECK(summary.proxied == 2U);
    PLAYBACK_CHECK(summary.waiting == 3U);
    PLAYBACK_CHECK(summary.waiting_total_frames == 0U);
    PLAYBACK_CHECK(app::summarize_view_proxies({}) == app::ViewProxySummary{});

    // With progress: each waiting asset's proxy counts once, however many
    // of its clips wait, and assets not waiting are not asked about.
    std::vector<doc::NodeId> asked;
    const app::ProxyProgressLookup progress =
        [&](doc::NodeId asset)
        -> std::optional<std::pair<core::u64, core::u64>> {
        asked.push_back(asset);
        return std::pair<core::u64, core::u64>{asset.value() - 100U,
                                               asset.value() * 10U};
    };
    std::array<app::ProxyPlacementObservation, 4U> shared{{
        observed(3U, Selection::Source, Reason::CacheGenerating),
        observed(4U, Selection::Source, Reason::CacheQueued),
        observed(9U, Selection::Source, Reason::CacheGenerating),
        observed(6U, Selection::Source, Reason::SourcePolicy),
    }};
    // Placement 9 shows the same asset as placement 3.
    shared[2U].asset = doc::NodeId{103U};
    const app::ViewProxySummary made =
        app::summarize_view_proxies(shared, progress);
    PLAYBACK_CHECK(made.waiting == 3U);
    PLAYBACK_CHECK((asked == std::vector<doc::NodeId>{doc::NodeId{103U},
                                                      doc::NodeId{104U}}));
    PLAYBACK_CHECK(made.waiting_made_frames == 3U + 4U);
    PLAYBACK_CHECK(made.waiting_total_frames == 1'030U + 1'040U);
}

void editor_proxies_cover_every_video_larger_than_the_proxy() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "source.mov";
    write_file(source, bytes("source-bytes"));
    const auto make = [&](core::u64 id, core::u32 width, core::u32 height,
                          core::u32 frames) {
        auto asset = media_asset(source, assets::ProxyPlaybackPolicy::Source,
                                 id, frames);
        asset.dimensions = assets::PixelDimensions{width, height};
        return asset;
    };
    assets::AssetRegistry authored{};
    authored.assets = {
        assets::Asset{make(1U, 1'920U, 1'080U, 240U)},
        assets::Asset{make(2U, 3'840U, 2'160U, 240U)},
        assets::Asset{make(3U, 1'280U, 720U, 240U)},
        assets::Asset{make(4U, 1'920U, 1'080U, 1U)},
    };
    const assets::AssetRegistry working = app::editor_playback_registry(authored);
    const auto proxied = [&](std::size_t index) {
        const auto& media = std::get<assets::MediaAsset>(working.assets[index]);
        return media.requested_proxy_profile ==
                   assets::DerivativeProfileId::Proxy720pCfrH264 &&
               media.proxy_playback_policy ==
                   assets::ProxyPlaybackPolicy::PreferProxy;
    };
    PLAYBACK_CHECK(proxied(0U));
    PLAYBACK_CHECK(proxied(1U));
    PLAYBACK_CHECK(!proxied(2U));
    PLAYBACK_CHECK(!proxied(3U));
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 13U> cases{{
    {"editor proxies cover every video larger than the proxy",
     &editor_proxies_cover_every_video_larger_than_the_proxy},
    {"published segments play until the whole proxy is ready",
     &published_segments_play_until_the_whole_proxy_is_ready},
    {"upcoming proxies materialize before air without holding Program",
     &upcoming_proxies_materialize_before_air_without_holding_program},
    {"Source selects only the same ready source",
     &source_policy_selects_only_the_same_ready_source},
    {"Prefer falls back visibly but never to another source",
     &prefer_proxy_falls_back_visibly_but_never_to_another_source},
    {"Require is unready when its exact cache entry is missing",
     &require_proxy_is_unready_when_the_exact_cache_entry_is_missing},
    {"remembered cache state follows publications and outside changes",
     &remembered_cache_state_follows_publications_and_outside_changes},
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
    {"view summaries count each placement once and only waiting fallbacks",
     &view_summaries_count_each_placement_once_and_only_waiting_fallbacks},
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
