#include "playback.hpp"
#include "clip_clipboard.hpp"
#include "cluster_observability.hpp"
#include "edit_coalescing_policy.hpp"
#include "ffmpeg_proxy_encoder.hpp"
#include "key_clipboard.hpp"
#include "output_deployment.hpp"
#include "output_media_runtime.hpp"
#include "output_playback_buffer.hpp"
#include "output_window_config.hpp"
#include "project_asset_import.hpp"
#include "project_assets.hpp"
#include "proxy_playback.hpp"
#include "system_hardware_probe.hpp"
#include "timeline_lane_authoring.hpp"

#include <mediaserver/assets/asset_model.hpp>
#include <mediaserver/assets/asset_path.hpp>
#include <mediaserver/assets/asset_schema.hpp>
#include <mediaserver/assets/proxy_policy.hpp>
#include <mediaserver/cmd/command.hpp>
#include <mediaserver/compositor/compositor_model.hpp>
#include <mediaserver/compositor/compositor_schema.hpp>
#include <mediaserver/net/frame_commit_sink.hpp>
#include <mediaserver/output/feed_map_model.hpp>
#include <mediaserver/output/feed_map_schema.hpp>
#include <mediaserver/scene/key_edit.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_project.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/scene/selection_set.hpp>
#include <mediaserver/show/demo_edit_scene.hpp>
#include <mediaserver/show/demo_feed_map.hpp>
#include <mediaserver/show/demo_scene.hpp>
#include <mediaserver/show/frame_boundary_driver.hpp>
#include <mediaserver/show/presenter.hpp>
#include <mediaserver/show/switcher_author.hpp>
#include <mediaserver/show/switcher_authority.hpp>
#include <mediaserver/show/switcher_state.hpp>
#include <mediaserver/show/test_pattern.hpp>
#include <mediaserver/show/transport_author.hpp>
#include <mediaserver/show/transport_authority.hpp>
#include <mediaserver/sync/sync_group.hpp>
#include <mediaserver/sync/sync_policy_model.hpp>
#include <mediaserver/sync/sync_readiness.hpp>
#include <mediaserver/sync/sync_schema.hpp>
#include <mediaserver/topology/hardware_map_model.hpp>
#include <mediaserver/topology/hardware_map_schema.hpp>

#if defined(MEDIASERVER_HAS_GUI)
#include "cluster_lifecycle_controller.hpp"
#include "mask_edit_integration.hpp"
#include "thumbnail_service.hpp"
#include "timeline_edit_integration.hpp"

#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/persist/project_file.hpp>
#include <mediaserver/persist/settings.hpp>
#include <mediaserver/render/media_source.hpp>
#include <mediaserver/ui/sdl_presenter.hpp>

#include <array>
#include <cstdio>
#include <fstream>
#endif

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace mediaserver::app {
namespace {

using Clock = show::ShowClock::Clock;

// Shutdown latency ceiling. A boundary further away than this is slept to in
// steps so an interrupt is still observed promptly.
constexpr std::chrono::milliseconds max_sleep{10};

// Bound on the frames one Director iteration will seal after a late wake-up.
constexpr core::u64 max_director_catch_up = 8U;

// Barrier records are diagnostic slots that must be retired once their cohort
// completes; this is the backlog the Director tolerates before refusing to
// seal further.
constexpr std::size_t max_tracked_frames = 64U;

// How often a process re-stats required paths while any of them is missing.
// A file can appear after start-up - a share mounts, or the media is copied in
// late - and the report has to follow. Nothing is re-checked once everything
// is present: only a manifest change asks again.
constexpr std::chrono::milliseconds library_proxy_retry_interval{500};

[[nodiscard]] core::u64
proxy_cache_bytes(const PlaybackOptions &options) noexcept {
    return static_cast<core::u64>(
               options.active_preferences.proxy_cache_gibibytes) *
           1'024ULL * 1'024ULL * 1'024ULL;
}

[[nodiscard]] server::ProxyTranscodePriority
proxy_transcode_priority(const PlaybackOptions &options) noexcept {
    switch (options.active_preferences.proxy_transcode_priority) {
    case persist::ProxyTranscodePriority::Background:
        return server::ProxyTranscodePriority::Background;
    case persist::ProxyTranscodePriority::Balanced:
        return server::ProxyTranscodePriority::Balanced;
    case persist::ProxyTranscodePriority::Fast:
        return server::ProxyTranscodePriority::Fast;
    }
    return server::ProxyTranscodePriority::Background;
}

[[nodiscard]] std::filesystem::path
derivative_cache_root(const PlaybackOptions &options, std::error_code &error) {
    if (!options.preferences_file.empty()) {
        return options.preferences_file.parent_path() / "derivatives";
    }
    return std::filesystem::temp_directory_path(error) /
           "mediaserver-derivatives";
}

[[nodiscard]] std::filesystem::path
fixed_proxy_cache_root(const PlaybackOptions &options, std::error_code &error) {
    if (!options.active_preferences.proxy_cache_next_to_source &&
        !options.active_preferences.proxy_cache_directory.empty()) {
        try {
            std::filesystem::path configured =
                assets::asset_path_from_source(
                    options.active_preferences.proxy_cache_directory)
                    .lexically_normal();
            if (configured.empty() || !configured.is_absolute()) {
                error = std::make_error_code(std::errc::invalid_argument);
                return {};
            }
            return configured;
        } catch (const std::filesystem::filesystem_error &caught) {
            error = caught.code();
            return {};
        } catch (...) {
            error = std::make_error_code(std::errc::invalid_argument);
            return {};
        }
    }
    return derivative_cache_root(options, error) / "proxies";
}

[[nodiscard]] bool valid_proxy_cache_preference(std::string_view directory,
                                                bool next_to_source) noexcept {
    if (next_to_source || directory.empty()) {
        return true;
    }
    try {
        const std::filesystem::path path =
            assets::asset_path_from_source(directory).lexically_normal();
        return !path.empty() && path.is_absolute() && path != path.root_path();
    } catch (...) {
        return false;
    }
}

[[nodiscard]] std::unique_ptr<server::ProxyGenerationService>
open_proxy_playback_artifacts(const PlaybackOptions &options,
                              std::string_view owner) {
    std::error_code cache_path_error;
    const std::filesystem::path cache_root =
        fixed_proxy_cache_root(options, cache_path_error);
    if (cache_path_error) {
        std::cerr << owner << " could not locate the proxy cache root: "
                  << cache_path_error.message() << '\n';
        return nullptr;
    }
    if (options.active_preferences.proxy_cache_next_to_source) {
        try {
            return std::make_unique<server::ProxyGenerationService>(
                server::NextToSourceProxyCacheConfig{
                    cache_root, proxy_cache_bytes(options)},
                server::ffmpeg_proxy_encoder_backend(
                    proxy_transcode_priority(options)));
        } catch (const std::bad_alloc &) {
            std::cerr << owner << " could not initialize next-to-source "
                      << "proxy storage: allocation failed\n";
            return nullptr;
        }
    }
    auto cache = assets::DerivativeCache::open(
        assets::DerivativeCacheConfig{cache_root, proxy_cache_bytes(options)});
    if (!cache) {
        std::cerr << owner << " could not open the proxy cache (code "
                  << static_cast<unsigned>(cache.error().code) << ")\n";
        return nullptr;
    }
    try {
        return std::make_unique<server::ProxyGenerationService>(
            std::move(cache).value(), server::ffmpeg_proxy_encoder_backend(
                                          proxy_transcode_priority(options)));
    } catch (const std::bad_alloc &) {
        std::cerr
            << owner
            << " could not initialize proxy playback: allocation failed\n";
        return nullptr;
    }
}

struct ProxyCacheClearResult final {
    std::size_t roots_cleared{};
    std::error_code error;

    [[nodiscard]] explicit operator bool() const noexcept { return !error; }
};

[[nodiscard]] ProxyCacheClearResult
clear_proxy_cache(const PlaybackOptions &options,
                  const assets::AssetRegistry &registry) noexcept {
    try {
        std::error_code path_error;
        const std::filesystem::path runtime_root =
            fixed_proxy_cache_root(options, path_error).lexically_normal();
        if (path_error || runtime_root.empty() || !runtime_root.is_absolute() ||
            runtime_root == runtime_root.root_path()) {
            return ProxyCacheClearResult{
                0U, path_error
                        ? path_error
                        : std::make_error_code(std::errc::invalid_argument)};
        }

        std::vector<std::filesystem::path> roots;
        roots.push_back(runtime_root);
        if (options.active_preferences.proxy_cache_next_to_source) {
            for (const assets::Asset &entry : registry.assets) {
                const auto *const media =
                    std::get_if<assets::MediaAsset>(&entry);
                if (media == nullptr ||
                    !assets::valid_asset_source(media->source)) {
                    continue;
                }
                const std::filesystem::path root =
                    (assets::asset_path_from_source(media->source)
                         .parent_path() /
                     ".mediaserver-proxies")
                        .lexically_normal();
                if (root.empty() || !root.is_absolute() ||
                    root == root.root_path() ||
                    std::find(roots.begin(), roots.end(), root) !=
                        roots.end()) {
                    continue;
                }
                roots.push_back(root);
            }
        }

        ProxyCacheClearResult result{};
        for (const std::filesystem::path &root : roots) {
            // The service validates every recursive target and preserves the
            // selected root plus any unrelated files it contains.
            const server::ProxyCacheClearResult cleared =
                server::clear_proxy_cache_root(root);
            if (!cleared) {
                return ProxyCacheClearResult{result.roots_cleared,
                                             cleared.error};
            }
            ++result.roots_cleared;
        }
        return result;
    } catch (const std::bad_alloc &) {
        return ProxyCacheClearResult{
            0U, std::make_error_code(std::errc::not_enough_memory)};
    } catch (const std::filesystem::filesystem_error &caught) {
        return ProxyCacheClearResult{0U, caught.code()};
    } catch (...) {
        return ProxyCacheClearResult{0U,
                                     std::make_error_code(std::errc::io_error)};
    }
}

// Proxy encoding is intentionally serialized: one camera-original decode can
// already use the machine's available CPU, while the live renderer must keep
// priority. Playback and generation use separate cache handles so inspecting
// a ready artifact never waits behind a minutes-long transcode.
class ProxyGenerationWorker final {
  public:
    struct ProgressSnapshot final {
        assets::DerivativeKey key;
        core::u64 completed_frames{};
        core::u64 total_frames{};
        bool queued{};
    };

    explicit ProxyGenerationWorker(
        std::unique_ptr<server::ProxyGenerationService> artifacts)
        : artifacts_(std::move(artifacts)),
          worker_([this](std::stop_token stop) { run(stop); }) {}

    ProxyGenerationWorker(const ProxyGenerationWorker &) = delete;
    ProxyGenerationWorker &operator=(const ProxyGenerationWorker &) = delete;

    ~ProxyGenerationWorker() {
        worker_.request_stop();
        condition_.notify_all();
    }

    void request(const assets::MediaAsset &asset) {
        if (artifacts_ == nullptr || asset.proxy_playback_policy ==
                                         assets::ProxyPlaybackPolicy::Source) {
            return;
        }
        const auto key = assets::requested_proxy_key(asset);
        if (!key.has_value()) {
            return;
        }
        std::scoped_lock lock{mutex_};
        const auto known =
            std::find(scheduled_.begin(), scheduled_.end(), *key);
        if (known != scheduled_.end()) {
            return;
        }
        const std::size_t scheduled_size = scheduled_.size();
        const std::size_t pending_size = pending_.size();
        const std::size_t progress_size = progress_.size();
        try {
            scheduled_.push_back(*key);
            pending_.push_back(asset);
            progress_.push_back(ProgressRecord{
                *key,
                0U,
                asset.frame_count.has_value()
                    ? static_cast<core::u64>(*asset.frame_count)
                    : 0U,
                true,
                true,
            });
        } catch (const std::bad_alloc &) {
            scheduled_.resize(scheduled_size);
            pending_.resize(pending_size);
            progress_.resize(progress_size);
            return;
        }
        condition_.notify_one();
    }

    [[nodiscard]] std::optional<ProgressSnapshot>
    progress_for(const assets::DerivativeKey &key) const {
        std::scoped_lock lock{mutex_};
        const auto found =
            std::find_if(progress_.begin(), progress_.end(),
                         [&](const ProgressRecord &record) {
                             return record.key == key && record.visible;
                         });
        if (found == progress_.end()) {
            return std::nullopt;
        }
        return ProgressSnapshot{found->key, found->completed_frames,
                                found->total_frames, found->queued};
    }

  private:
    struct ProgressRecord final {
        assets::DerivativeKey key;
        core::u64 completed_frames{};
        core::u64 total_frames{};
        bool queued{};
        bool visible{};
    };

    void begin(const assets::DerivativeKey &key) {
        std::scoped_lock lock{mutex_};
        const auto found = std::find_if(
            progress_.begin(), progress_.end(),
            [&](const ProgressRecord &record) { return record.key == key; });
        if (found != progress_.end()) {
            found->queued = false;
        }
    }

    void update(const assets::DerivativeKey &key, core::u64 completed,
                core::u64 total) {
        std::scoped_lock lock{mutex_};
        const auto found = std::find_if(
            progress_.begin(), progress_.end(),
            [&](const ProgressRecord &record) { return record.key == key; });
        if (found != progress_.end()) {
            found->queued = false;
            found->total_frames = total;
            found->completed_frames = std::min(completed, total);
        }
    }

    void finish(const assets::DerivativeKey &key) {
        std::scoped_lock lock{mutex_};
        const auto found = std::find_if(
            progress_.begin(), progress_.end(),
            [&](const ProgressRecord &record) { return record.key == key; });
        if (found != progress_.end()) {
            found->visible = false;
        }
    }

    void run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::optional<assets::MediaAsset> asset;
            {
                std::unique_lock lock{mutex_};
                condition_.wait(lock, stop,
                                [this] { return !pending_.empty(); });
                if (stop.stop_requested()) {
                    return;
                }
                asset = std::move(pending_.front());
                pending_.pop_front();
            }
            const auto key = assets::requested_proxy_key(*asset);
            if (!key.has_value()) {
                continue;
            }
            begin(*key);
            const auto generated = artifacts_->generate(
                *asset, stop,
                [this, key = *key](core::u64 completed, core::u64 total) {
                    update(key, completed, total);
                });
            finish(*key);
            if (!generated && !stop.stop_requested()) {
                std::cerr << "proxy generation failed for asset "
                          << asset->id.value() << " (code "
                          << static_cast<unsigned>(generated.error().code)
                          << ")\n";
            }
        }
    }

    std::unique_ptr<server::ProxyGenerationService> artifacts_;
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::deque<assets::MediaAsset> pending_;
    std::vector<assets::DerivativeKey> scheduled_;
    std::vector<ProgressRecord> progress_;
    std::jthread worker_;
};

[[nodiscard]] assets::AssetRegistry
editor_playback_registry(const assets::AssetRegistry &authored) {
    assets::AssetRegistry result = authored;
    for (assets::Asset &entry : result.assets) {
        auto *const media = std::get_if<assets::MediaAsset>(&entry);
        if (media == nullptr || !media->dimensions.has_value() ||
            !media->fingerprint.has_value()) {
            continue;
        }
        // Editor Program/Stage previews use an automatic working derivative
        // for UHD media. The authored asset and output-node policy stay
        // untouched, so render nodes still consume source quality.
        const bool source_policy =
            media->proxy_playback_policy == assets::ProxyPlaybackPolicy::Source;
        const bool full_resolution_output_proxy =
            media->requested_proxy_profile ==
            assets::DerivativeProfileId::ProxySourceCfrHevcMain10;
        if ((source_policy || full_resolution_output_proxy) &&
            (media->dimensions->width > 1'920U ||
             media->dimensions->height > 1'080U)) {
            media->requested_proxy_profile =
                assets::DerivativeProfileId::Proxy720pCfrH264;
            media->proxy_playback_policy =
                assets::ProxyPlaybackPolicy::PreferProxy;
        }
    }
    return result;
}

[[nodiscard]] const char *
proxy_selection_name(assets::ProxyPlaybackSelection selection) noexcept {
    switch (selection) {
    case assets::ProxyPlaybackSelection::Source:
        return "source";
    case assets::ProxyPlaybackSelection::Proxy:
        return "proxy";
    case assets::ProxyPlaybackSelection::Unready:
        return "unready";
    }
    return "unready";
}

[[nodiscard]] bool
same_proxy_status(const ProxyPlacementObservation &left,
                  const ProxyPlacementObservation &right) noexcept {
    return left.placement == right.placement && left.asset == right.asset &&
           left.resolution.selection == right.resolution.selection &&
           left.resolution.reason == right.resolution.reason &&
           left.resolution.source_readiness ==
               right.resolution.source_readiness &&
           left.issue == right.issue;
}

void report_proxy_status_changes(
    std::string_view owner,
    const std::vector<ProxyPlacementObservation> &current,
    const std::vector<ProxyPlacementObservation> &previous) {
    for (const ProxyPlacementObservation &observation : current) {
        const auto prior = std::find_if(
            previous.begin(), previous.end(),
            [&](const ProxyPlacementObservation &candidate) {
                return candidate.placement == observation.placement;
            });
        if (prior != previous.end() && same_proxy_status(*prior, observation)) {
            continue;
        }
        // Ordinary Source policy and unlinked demo media need no recurring
        // diagnostic. Fallbacks and unready states are always explicit.
        if (observation.resolution.selection ==
                assets::ProxyPlaybackSelection::Source &&
            observation.resolution.reason ==
                assets::ProxyPlaybackReason::SourcePolicy) {
            continue;
        }
        std::cerr << owner << " proxy playback placement="
                  << observation.placement.value()
                  << " asset=" << observation.asset.value() << " selection="
                  << proxy_selection_name(observation.resolution.selection)
                  << " reason="
                  << proxy_playback_reason_name(observation.resolution.reason);
        if (observation.issue != ProxyPlacementIssue::None) {
            std::cerr << " issue="
                      << proxy_placement_issue_name(observation.issue);
        }
        std::cerr << '\n';
    }
}

[[nodiscard]] bool
resolve_scene_proxies(ProxyPlaybackService &playback, scene::Scene &frame,
                      const assets::AssetRegistry &registry,
                      const ProjectAssetAvailability &availability,
                      std::vector<ProxyPlacementObservation> &observations) {
    auto resolved = playback.resolve(frame, registry, availability);
    if (!resolved) {
        // Allocation failure cannot be interpreted as permission to use the
        // source for a Require placement. Clearing media is the only
        // policy-independent fail-closed result.
        frame.media.clear();
        return false;
    }
    ProxyPlaybackFrame output = std::move(resolved).value();
    const bool ready = output.ready();
    frame = std::move(output.scene);
    auto placement_observations = std::move(output.placements);
    observations.insert(observations.end(),
                        std::make_move_iterator(placement_observations.begin()),
                        std::make_move_iterator(placement_observations.end()));
    return ready;
}

#if defined(MEDIASERVER_HAS_GUI)
class MediaDecodeSupportWorker final {
  public:
    struct Snapshot final {
        doc::NodeId asset{};
        net::RuntimeMediaDecodeState state{
            net::RuntimeMediaDecodeState::Unknown};
    };

    MediaDecodeSupportWorker()
        : worker_([this](std::stop_token stop) { run(stop); }) {}

    MediaDecodeSupportWorker(const MediaDecodeSupportWorker &) = delete;
    MediaDecodeSupportWorker &
    operator=(const MediaDecodeSupportWorker &) = delete;

    ~MediaDecodeSupportWorker() {
        worker_.request_stop();
        condition_.notify_all();
    }

    // A manifest digest is the cache key: replacing bytes at the same path
    // must cause a fresh negotiation on this output machine.
    void replace(core::u64 manifest_digest,
                 const assets::AssetRegistry &registry) noexcept {
        std::scoped_lock lock{mutex_};
        if (manifest_digest_ == manifest_digest && initialized_) {
            return;
        }
        initialized_ = true;
        manifest_digest_ = manifest_digest;
        pending_.clear();
        snapshots_.clear();
        try {
            for (const assets::Asset &entry : registry.assets) {
                const auto *const media =
                    std::get_if<assets::MediaAsset>(&entry);
                if (media == nullptr || !media->id) {
                    continue;
                }
                snapshots_.push_back(Snapshot{media->id});
                pending_.push_back(
                    Work{media->id, media->source, manifest_digest});
            }
        } catch (const std::bad_alloc &) {
            // Partial is honest: omitted assets remain Unknown at projection.
        }
        condition_.notify_one();
    }

    [[nodiscard]] std::optional<net::RuntimeMediaDecodeState>
    status_for(doc::NodeId asset, core::u64 manifest_digest) const noexcept {
        std::scoped_lock lock{mutex_};
        if (!initialized_ || manifest_digest != manifest_digest_) {
            return std::nullopt;
        }
        const auto found = std::find_if(
            snapshots_.begin(), snapshots_.end(),
            [asset](const Snapshot &value) { return value.asset == asset; });
        return found == snapshots_.end()
                   ? std::nullopt
                   : std::optional<net::RuntimeMediaDecodeState>{found->state};
    }

  private:
    struct Work final {
        doc::NodeId asset{};
        std::string source;
        core::u64 manifest_digest{};
    };

    static net::RuntimeMediaDecodeState
    project(render::MediaDecodeSupport support) noexcept {
        switch (support) {
        case render::MediaDecodeSupport::NotApplicable:
            return net::RuntimeMediaDecodeState::Unknown;
        case render::MediaDecodeSupport::HardwareAccelerated:
            return net::RuntimeMediaDecodeState::HardwareAccelerated;
        case render::MediaDecodeSupport::SoftwareOnly:
            return net::RuntimeMediaDecodeState::SoftwareOnly;
        case render::MediaDecodeSupport::Unavailable:
            return net::RuntimeMediaDecodeState::Unavailable;
        }
        return net::RuntimeMediaDecodeState::Unavailable;
    }

    void run(std::stop_token stop) noexcept {
        while (!stop.stop_requested()) {
            std::optional<Work> work;
            {
                std::unique_lock lock{mutex_};
                condition_.wait(lock, stop,
                                [this] { return !pending_.empty(); });
                if (stop.stop_requested()) {
                    return;
                }
                work = std::move(pending_.front());
                pending_.pop_front();
            }
            render::MediaDecodeSupport support =
                render::MediaDecodeSupport::Unavailable;
            try {
                support = render::probe_media_decode_support(work->source);
            } catch (...) {
            }
            std::scoped_lock lock{mutex_};
            if (work->manifest_digest != manifest_digest_) {
                continue;
            }
            const auto found =
                std::find_if(snapshots_.begin(), snapshots_.end(),
                             [&](const Snapshot &value) {
                                 return value.asset == work->asset;
                             });
            if (found != snapshots_.end()) {
                found->state = project(support);
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::deque<Work> pending_;
    std::vector<Snapshot> snapshots_;
    core::u64 manifest_digest_{};
    bool initialized_{};
    std::jthread worker_;
};

[[nodiscard]] net::RuntimeProxyState
runtime_proxy_state(ProxyAssetRuntimeState state) noexcept {
    switch (state) {
    case ProxyAssetRuntimeState::Source:
        return net::RuntimeProxyState::Source;
    case ProxyAssetRuntimeState::ProxyReady:
        return net::RuntimeProxyState::ProxyReady;
    case ProxyAssetRuntimeState::PreferFallback:
        return net::RuntimeProxyState::PreferFallback;
    case ProxyAssetRuntimeState::RequiredUnready:
        return net::RuntimeProxyState::RequiredUnready;
    case ProxyAssetRuntimeState::Failed:
        return net::RuntimeProxyState::Failed;
    }
    return net::RuntimeProxyState::Failed;
}

[[nodiscard]] net::RuntimeProxyReason
runtime_proxy_reason(assets::ProxyPlaybackReason reason) noexcept {
    static_assert(
        static_cast<core::u8>(assets::ProxyPlaybackReason::FrameMapInvalid) ==
        static_cast<core::u8>(net::RuntimeProxyReason::FrameMapInvalid));
    return static_cast<net::RuntimeProxyReason>(static_cast<core::u8>(reason));
}

[[nodiscard]] net::RuntimeProxyIssue
runtime_proxy_issue(ProxyPlacementIssue issue) noexcept {
    static_assert(
        static_cast<core::u8>(ProxyPlacementIssue::MaterializationFailed) ==
        static_cast<core::u8>(net::RuntimeProxyIssue::MaterializationFailed));
    return static_cast<net::RuntimeProxyIssue>(static_cast<core::u8>(issue));
}

[[nodiscard]] bool build_node_media_status(
    const assets::AssetRegistry &registry,
    const ProjectAssetAvailability &availability,
    server::ProxyGenerationService *artifacts,
    const ProxyGenerationWorker *generator,
    const MediaDecodeSupportWorker &decode_support, core::u64 manifest_digest,
    std::array<net::RuntimeMediaAssetStatus, net::max_runtime_media_assets>
        &output,
    std::size_t &output_count) noexcept {
    const auto observed = collect_proxy_asset_runtime(
        registry, availability, artifacts,
        std::span<const ProxyPlacementObservation>{});
    if (!observed) {
        output_count = 0U;
        return false;
    }
    output_count = 0U;
    for (const ProxyAssetRuntimeObservation &observation : observed.value()) {
        if (output_count >= output.size()) {
            break;
        }
        net::RuntimeMediaAssetStatus status{};
        status.asset_id = observation.asset.value();
        status.decode =
            decode_support.status_for(observation.asset, manifest_digest)
                .value_or(net::RuntimeMediaDecodeState::Unknown);
        status.proxy = runtime_proxy_state(observation.state);
        status.proxy_reason = runtime_proxy_reason(observation.reason);
        status.proxy_issue = runtime_proxy_issue(observation.issue);
        if (generator != nullptr && observation.key.has_value()) {
            const auto progress = generator->progress_for(*observation.key);
            if (progress.has_value()) {
                status.has_transcode_progress = true;
                status.transcode_queued = progress->queued;
                status.transcode_completed_frames = progress->completed_frames;
                status.transcode_total_frames = progress->total_frames;
            }
        }
        output[output_count++] = status;
    }
    return true;
}

[[nodiscard]] editor::LibraryProxyRuntimeState
editor_proxy_runtime_state(net::RuntimeProxyState state) noexcept {
    switch (state) {
    case net::RuntimeProxyState::Source:
        return editor::LibraryProxyRuntimeState::Source;
    case net::RuntimeProxyState::ProxyReady:
        return editor::LibraryProxyRuntimeState::ProxyReady;
    case net::RuntimeProxyState::PreferFallback:
        return editor::LibraryProxyRuntimeState::PreferFallback;
    case net::RuntimeProxyState::RequiredUnready:
        return editor::LibraryProxyRuntimeState::RequiredUnready;
    case net::RuntimeProxyState::Failed:
        return editor::LibraryProxyRuntimeState::Failed;
    }
    return editor::LibraryProxyRuntimeState::Failed;
}

[[nodiscard]] editor::LibraryProxyRuntimeIssue
editor_proxy_runtime_issue(net::RuntimeProxyIssue issue) noexcept {
    switch (issue) {
    case net::RuntimeProxyIssue::None:
        return editor::LibraryProxyRuntimeIssue::None;
    case net::RuntimeProxyIssue::MissingAssetIdentity:
        return editor::LibraryProxyRuntimeIssue::MissingAssetIdentity;
    case net::RuntimeProxyIssue::AssetNotFound:
        return editor::LibraryProxyRuntimeIssue::AssetNotFound;
    case net::RuntimeProxyIssue::AssetKindMismatch:
        return editor::LibraryProxyRuntimeIssue::AssetKindMismatch;
    case net::RuntimeProxyIssue::PlacementSourceMismatch:
        return editor::LibraryProxyRuntimeIssue::PlacementSourceMismatch;
    case net::RuntimeProxyIssue::AvailabilityMissing:
        return editor::LibraryProxyRuntimeIssue::AvailabilityMissing;
    case net::RuntimeProxyIssue::CacheInspectionFailed:
        return editor::LibraryProxyRuntimeIssue::CacheInspectionFailed;
    case net::RuntimeProxyIssue::ArtifactLoadFailed:
        return editor::LibraryProxyRuntimeIssue::ArtifactLoadFailed;
    case net::RuntimeProxyIssue::ArtifactMetadataMismatch:
        return editor::LibraryProxyRuntimeIssue::ArtifactMetadataMismatch;
    case net::RuntimeProxyIssue::FrameMappingFailed:
        return editor::LibraryProxyRuntimeIssue::FrameMappingFailed;
    case net::RuntimeProxyIssue::MaterializationFailed:
        return editor::LibraryProxyRuntimeIssue::MaterializationFailed;
    }
    return editor::LibraryProxyRuntimeIssue::CacheInspectionFailed;
}

[[nodiscard]] const net::RuntimeMediaAssetStatus *
runtime_media_status(const net::RuntimeAssetAdoption &adoption,
                     doc::NodeId asset) noexcept {
    for (const net::RuntimeMediaAssetStatus &status : adoption.media_assets) {
        if (status.asset_id == asset.value()) {
            return &status;
        }
    }
    return nullptr;
}

void project_output_node_media_runtime(
    const assets::AssetRegistry &registry, core::u64 manifest_digest,
    const topology::HardwareMap &hardware,
    const net::DirectorFrameBarrier &barrier,
    std::vector<editor::LibraryProxyRuntimeObservation> &destination) {
    std::vector<core::u32> node_indices;
    node_indices.reserve(hardware.devices.size());
    for (const topology::HardwareDevice &device : hardware.devices) {
        if (device.kind != topology::HardwareDeviceKind::RenderServer ||
            std::find(node_indices.begin(), node_indices.end(),
                      device.node_index) != node_indices.end()) {
            continue;
        }
        node_indices.push_back(device.node_index);
    }

    struct OutputNodeMediaReport final {
        bool available{};
        net::RuntimeAssetAdoption adoption;
    };
    std::vector<OutputNodeMediaReport> node_reports;
    node_reports.reserve(node_indices.size());
    for (const core::u32 node_index : node_indices) {
        OutputNodeMediaReport &report = node_reports.emplace_back();
        report.available = barrier.runtime_asset_adoption(
            node_index, manifest_digest, report.adoption);
    }

    destination.clear();
    destination.reserve(registry.assets.size());
    for (const assets::Asset &entry : registry.assets) {
        const auto *const media = std::get_if<assets::MediaAsset>(&entry);
        if (media == nullptr) {
            continue;
        }
        editor::LibraryProxyRuntimeObservation projected{};
        projected.asset = media->id;
        projected.key = assets::requested_proxy_key(*media);
        const net::RuntimeProxyState missing_proxy =
            media->proxy_playback_policy == assets::ProxyPlaybackPolicy::Source
                ? net::RuntimeProxyState::Source
                : (media->proxy_playback_policy ==
                           assets::ProxyPlaybackPolicy::RequireProxy
                       ? net::RuntimeProxyState::RequiredUnready
                       : net::RuntimeProxyState::PreferFallback);
        const net::RuntimeProxyReason missing_reason =
            media->proxy_playback_policy == assets::ProxyPlaybackPolicy::Source
                ? net::RuntimeProxyReason::SourcePolicy
                : net::RuntimeProxyReason::CacheObservationMissing;
        OutputMediaRuntimeAccumulator accumulator{
            node_reports.size(), missing_proxy, missing_reason};
        for (const OutputNodeMediaReport &report : node_reports) {
            if (!report.available) {
                accumulator.observe(nullptr);
                continue;
            }
            const net::RuntimeMediaAssetStatus *const status =
                runtime_media_status(report.adoption, media->id);
            accumulator.observe(status);
        }
        const OutputMediaRuntimeSummary summary = accumulator.finish();
        switch (summary.decode) {
        case net::RuntimeMediaDecodeState::Unknown:
            break;
        case net::RuntimeMediaDecodeState::HardwareAccelerated:
            projected.media_decode =
                editor::LibraryMediaDecodeState::HardwareAccelerated;
            break;
        case net::RuntimeMediaDecodeState::SoftwareOnly:
            projected.media_decode = editor::LibraryMediaDecodeState::CpuOnly;
            break;
        case net::RuntimeMediaDecodeState::Unavailable:
            projected.media_decode =
                editor::LibraryMediaDecodeState::Unavailable;
            break;
        }
        projected.state = editor_proxy_runtime_state(summary.proxy);
        projected.reason = static_cast<assets::ProxyPlaybackReason>(
            static_cast<core::u8>(summary.proxy_reason));
        projected.issue = editor_proxy_runtime_issue(summary.proxy_issue);
        if (summary.transcode_progress.has_value()) {
            projected.transcode_progress =
                editor::LibraryProxyTranscodeProgress{
                    summary.transcode_progress->completed_frames,
                    summary.transcode_progress->total_frames,
                    summary.transcode_progress->queued};
        }
        destination.push_back(std::move(projected));
    }
}

[[nodiscard]] std::filesystem::path
native_path_from_utf8(std::string_view text) {
    std::u8string encoded;
    encoded.reserve(text.size());
    for (const char byte : text) {
        encoded.push_back(
            static_cast<char8_t>(static_cast<unsigned char>(byte)));
    }
    return std::filesystem::path{encoded};
}

[[nodiscard]] std::string
native_path_to_utf8(const std::filesystem::path &path) {
    const std::u8string encoded = path.u8string();
    std::string text;
    text.reserve(encoded.size());
    for (const char8_t byte : encoded) {
        text.push_back(std::bit_cast<char>(byte));
    }
    return text;
}

[[nodiscard]] bool project_content_changed(const doc::Root &saved,
                                           const doc::Root &current) {
    const doc::Delta delta = doc::diff(saved, current);
    return std::any_of(delta.changes.begin(), delta.changes.end(),
                       [](const doc::NodeChange &change) {
                           // Play/pause/seek re-anchor live transport state;
                           // they should not put an asterisk beside an
                           // otherwise unchanged project. Timeline range edits
                           // set the explicit dirty flag at the editor
                           // boundary.
                           return change.id != show::transport_node_id;
                       });
}

// Recovery may borrow a saved path only when all durable project content
// agrees. Transport playhead/state is live session state, but its direction,
// rate, range, and loop flag are authored project configuration and must match.
[[nodiscard]] bool
recovered_project_matches_saved_baseline(const doc::Root &saved,
                                         const doc::Root &recovered) {
    if (project_content_changed(saved, recovered)) {
        return false;
    }
    const doc::Node *const saved_node = saved.get(show::transport_node_id);
    const doc::Node *const recovered_node =
        recovered.get(show::transport_node_id);
    if (saved_node == nullptr || recovered_node == nullptr) {
        return saved_node == nullptr && recovered_node == nullptr;
    }
    const auto saved_transport = show::read_transport(saved);
    const auto recovered_transport = show::read_transport(recovered);
    if (!saved_transport.has_value() || !recovered_transport.has_value()) {
        return false;
    }
    return saved_transport->direction == recovered_transport->direction &&
           saved_transport->rate == recovered_transport->rate &&
           saved_transport->first_frame == recovered_transport->first_frame &&
           saved_transport->last_frame == recovered_transport->last_frame &&
           saved_transport->loop == recovered_transport->loop;
}

[[nodiscard]] std::string project_file_failure(std::string_view operation,
                                               unsigned code) {
    std::string message{"ERROR "};
    message.append(operation);
    message.append(" PROJECT (");
    message.append(std::to_string(code));
    message.push_back(')');
    return message;
}

void append_key_target_report(std::string &message, std::string_view label,
                              std::span<const doc::NodeId> targets) {
    if (targets.empty()) {
        return;
    }
    message.append("; ");
    message.append(label);
    message.push_back(' ');
    constexpr std::size_t visible_limit = 4U;
    const std::size_t visible = std::min(targets.size(), visible_limit);
    for (std::size_t index = 0U; index < visible; ++index) {
        if (index != 0U) {
            message.push_back(',');
        }
        message.append(std::to_string(targets[index].value()));
    }
    if (targets.size() > visible) {
        message.append(" +");
        message.append(std::to_string(targets.size() - visible));
    }
}

void append_key_incompatible_report(std::string &message,
                                    std::span<const KeyAddress> incompatible) {
    if (incompatible.empty()) {
        return;
    }
    message.append("; INCOMPATIBLE ");
    constexpr std::size_t visible_limit = 4U;
    const std::size_t visible = std::min(incompatible.size(), visible_limit);
    for (std::size_t index = 0U; index < visible; ++index) {
        if (index != 0U) {
            message.push_back(',');
        }
        message.append(std::to_string(incompatible[index].target.value()));
        message.push_back('@');
        message.append(std::to_string(incompatible[index].frame));
    }
    if (incompatible.size() > visible) {
        message.append(" +");
        message.append(std::to_string(incompatible.size() - visible));
    }
}

[[nodiscard]] std::string
key_operation_message(std::string_view operation, std::size_t accepted,
                      std::string_view accepted_label,
                      std::span<const doc::NodeId> locked,
                      std::span<const KeyAddress> incompatible) {
    std::string message{"KEY "};
    message.append(operation);
    message.append(" - ");
    message.append(std::to_string(accepted));
    message.push_back(' ');
    message.append(accepted_label);
    append_key_target_report(message, "LOCKED", locked);
    append_key_incompatible_report(message, incompatible);
    return message;
}

[[nodiscard]] std::vector<KeyAddress>
key_addresses(std::span<const editor::TimelineKeyAddress> addresses) {
    std::vector<KeyAddress> converted;
    converted.reserve(addresses.size());
    for (const editor::TimelineKeyAddress &address : addresses) {
        converted.push_back(
            KeyAddress{address.target, address.property, address.frame});
    }
    return converted;
}

[[nodiscard]] std::vector<scene::KeyAddress>
scene_key_addresses(std::span<const editor::TimelineKeyAddress> addresses) {
    std::vector<scene::KeyAddress> converted;
    converted.reserve(addresses.size());
    for (const editor::TimelineKeyAddress &address : addresses) {
        converted.push_back(
            scene::KeyAddress{address.target, address.property, address.frame});
    }
    return converted;
}

[[nodiscard]] std::string
key_batch_message(editor::TimelineKeyBatchAction action,
                  const scene::KeyEditReport &report) {
    std::string_view operation = "MOVE";
    switch (action) {
    case editor::TimelineKeyBatchAction::Move:
        operation = "MOVE";
        break;
    case editor::TimelineKeyBatchAction::Duplicate:
        operation = "DUPLICATE";
        break;
    case editor::TimelineKeyBatchAction::Delete:
        operation = "DELETE";
        break;
    case editor::TimelineKeyBatchAction::Scale:
        operation = "SCALE";
        break;
    }
    std::string message{"KEY "};
    message.append(operation);
    message.append(" - ");
    message.append(std::to_string(report.applied));
    message.append(" APPLIED");
    if (!report.collisions.empty()) {
        message.append(" / ");
        message.append(std::to_string(report.collisions.size()));
        message.append(" COLLISIONS");
    }
    if (!report.skips.empty()) {
        message.append(" / ");
        message.append(std::to_string(report.skips.size()));
        message.append(" SKIPPED");
    }
    return message;
}

[[nodiscard]] std::string
group_edit_error_message(editor::GroupEditAction action,
                         scene::GroupEditError error) {
    const std::string operation =
        action == editor::GroupEditAction::Group ? "GROUP" : "UNGROUP";
    std::string_view reason = "INVALID REQUEST";
    switch (error) {
    case scene::GroupEditError::InvalidScene:
        reason = "SCENE IS NOT ACTIVE";
        break;
    case scene::GroupEditError::EmptySelection:
        reason = "NO ELIGIBLE SELECTION";
        break;
    case scene::GroupEditError::DuplicateSelection:
        reason = "DUPLICATE SELECTION";
        break;
    case scene::GroupEditError::UnsupportedNode:
        reason = "UNSUPPORTED NODE";
        break;
    case scene::GroupEditError::CrossSceneSelection:
        reason = "CROSS-SCENE SELECTION";
        break;
    case scene::GroupEditError::AlreadyGrouped:
        reason = "SELECT TOP-LEVEL NODES";
        break;
    case scene::GroupEditError::Locked:
        reason = "SELECTION IS LOCKED";
        break;
    case scene::GroupEditError::InvalidName:
        reason = "INVALID NAME";
        break;
    case scene::GroupEditError::InvalidFirstId:
    case scene::GroupEditError::IdExhausted:
        reason = "NODE IDS EXHAUSTED";
        break;
    case scene::GroupEditError::GroupNotFound:
    case scene::GroupEditError::NotAGroup:
        reason = "GROUP NO LONGER EXISTS";
        break;
    case scene::GroupEditError::InvalidHierarchy:
        reason = "HIERARCHY NEEDS REPAIR";
        break;
    case scene::GroupEditError::AnimatedTransform:
        reason = "ANIMATED TRANSFORM CANNOT BE PRESERVED";
        break;
    case scene::GroupEditError::TransformNotRepresentable:
        reason = "WORLD TRANSFORM CANNOT BE PRESERVED";
        break;
    case scene::GroupEditError::VisibilityNotRepresentable:
        reason = "HIDDEN STATE CANNOT BE PRESERVED";
        break;
    case scene::GroupEditError::OrderOverflow:
        reason = "SIBLING ORDER EXHAUSTED";
        break;
    case scene::GroupEditError::AllocationFailed:
        reason = "OUT OF MEMORY";
        break;
    }
    return operation + " REJECTED - " + std::string{reason};
}

[[nodiscard]] std::string
mask_edit_error_message(scene::MaskAuthoringErrorCode code) {
    std::string_view reason = "INVALID REQUEST";
    switch (code) {
    case scene::MaskAuthoringErrorCode::InvalidScene:
    case scene::MaskAuthoringErrorCode::MissingTargetSceneOwner:
    case scene::MaskAuthoringErrorCode::CrossSceneTarget:
    case scene::MaskAuthoringErrorCode::CrossSceneMask:
    case scene::MaskAuthoringErrorCode::CrossScenePoint:
        reason = "SCENE OR TARGET CHANGED";
        break;
    case scene::MaskAuthoringErrorCode::TargetMissing:
    case scene::MaskAuthoringErrorCode::MaskNotFound:
    case scene::MaskAuthoringErrorCode::PointNotFound:
        reason = "TARGET NO LONGER EXISTS";
        break;
    case scene::MaskAuthoringErrorCode::UnsupportedTarget:
    case scene::MaskAuthoringErrorCode::NotAMask:
    case scene::MaskAuthoringErrorCode::NotAMaskPoint:
    case scene::MaskAuthoringErrorCode::PointMaskMismatch:
    case scene::MaskAuthoringErrorCode::NonBezierMask:
        reason = "TARGET IS INCOMPATIBLE";
        break;
    case scene::MaskAuthoringErrorCode::MaskCapacity:
    case scene::MaskAuthoringErrorCode::PointCapacity:
    case scene::MaskAuthoringErrorCode::InvalidFirstId:
    case scene::MaskAuthoringErrorCode::IdExhausted:
        reason = "MASK OR NODE LIMIT REACHED";
        break;
    case scene::MaskAuthoringErrorCode::InvalidName:
    case scene::MaskAuthoringErrorCode::NameTooLong:
        reason = "NAME IS INVALID";
        break;
    case scene::MaskAuthoringErrorCode::InvalidValue:
    case scene::MaskAuthoringErrorCode::InvalidShape:
    case scene::MaskAuthoringErrorCode::InvalidMask:
    case scene::MaskAuthoringErrorCode::InvalidPoint:
    case scene::MaskAuthoringErrorCode::InvalidMaskIndex:
    case scene::MaskAuthoringErrorCode::InvalidPointIndex:
    case scene::MaskAuthoringErrorCode::MinimumBezierPoints:
    case scene::MaskAuthoringErrorCode::InvalidFirstActiveCombine:
        reason = "MASK GEOMETRY OR ORDER IS INVALID";
        break;
    case scene::MaskAuthoringErrorCode::AllocationFailed:
        reason = "OUT OF MEMORY";
        break;
    }
    return "MASK EDIT REJECTED - " + std::string{reason};
}

[[nodiscard]] std::string
selection_set_error_message(editor::SelectionSetEditAction action,
                            const scene::SelectionSetError &error) {
    std::string_view operation = "SELECTION SET EDIT";
    switch (action) {
    case editor::SelectionSetEditAction::Create:
        operation = "SELECTION SET CREATE";
        break;
    case editor::SelectionSetEditAction::ReplaceMembers:
        operation = "SELECTION SET REPLACE";
        break;
    case editor::SelectionSetEditAction::Recall:
        operation = "SELECTION SET RECALL";
        break;
    case editor::SelectionSetEditAction::Rename:
        operation = "SELECTION SET RENAME";
        break;
    case editor::SelectionSetEditAction::Delete:
        operation = "SELECTION SET DELETE";
        break;
    }
    std::string_view reason = "INVALID REQUEST";
    switch (error.code) {
    case scene::SelectionSetErrorCode::InvalidScene:
        reason = "SCENE IS NOT ACTIVE";
        break;
    case scene::SelectionSetErrorCode::InvalidName:
        reason = "NAME IS EMPTY OR INVALID UTF-8";
        break;
    case scene::SelectionSetErrorCode::NameTooLong:
        reason = "NAME IS TOO LONG";
        break;
    case scene::SelectionSetErrorCode::EmptySelection:
        reason = "NO ELIGIBLE SELECTION";
        break;
    case scene::SelectionSetErrorCode::MemberCapacity:
        reason = "TOO MANY MEMBERS";
        break;
    case scene::SelectionSetErrorCode::InvalidFirstId:
    case scene::SelectionSetErrorCode::IdExhausted:
        reason = "NODE IDS EXHAUSTED";
        break;
    case scene::SelectionSetErrorCode::SetNotFound:
    case scene::SelectionSetErrorCode::NotASelectionSet:
        reason = "SET NO LONGER EXISTS";
        break;
    case scene::SelectionSetErrorCode::InvalidSet:
    case scene::SelectionSetErrorCode::InvalidReference:
        reason = "SET NEEDS REPAIR";
        break;
    case scene::SelectionSetErrorCode::CrossSceneSet:
        reason = "SET BELONGS TO ANOTHER SCENE";
        break;
    case scene::SelectionSetErrorCode::TargetMissing:
        reason = "A SELECTED TARGET NO LONGER EXISTS";
        break;
    case scene::SelectionSetErrorCode::UnsupportedTarget:
        reason = "SELECTION CONTAINS AN UNSUPPORTED TARGET";
        break;
    case scene::SelectionSetErrorCode::CrossSceneTarget:
        reason = "SELECTION CROSSES SCENES";
        break;
    case scene::SelectionSetErrorCode::TargetLocked:
        reason = "SELECTION CONTAINS A LOCKED TARGET";
        break;
    case scene::SelectionSetErrorCode::InvalidTarget:
        reason = "SELECTION TARGET NEEDS REPAIR";
        break;
    case scene::SelectionSetErrorCode::OrderOverflow:
        reason = "MEMBER ORDER EXHAUSTED";
        break;
    case scene::SelectionSetErrorCode::AllocationFailed:
        reason = "OUT OF MEMORY";
        break;
    }
    return std::string{operation} + " REJECTED - " + std::string{reason};
}

[[nodiscard]] std::string_view
project_file_error_label(persist::ProjectFileErrorCode code) noexcept {
    switch (code) {
    case persist::ProjectFileErrorCode::InvalidPath:
        return "INVALID PROJECT PATH";
    case persist::ProjectFileErrorCode::EncodeFailed:
        return "PROJECT DATA CANNOT BE SAVED";
    case persist::ProjectFileErrorCode::OpenFailed:
        return "FILE COULD NOT BE OPENED";
    case persist::ProjectFileErrorCode::ReadFailed:
        return "FILE COULD NOT BE READ";
    case persist::ProjectFileErrorCode::WriteFailed:
        return "FILE COULD NOT BE WRITTEN";
    case persist::ProjectFileErrorCode::SyncFailed:
        return "FILE DURABILITY COULD NOT BE CONFIRMED";
    case persist::ProjectFileErrorCode::ReplaceFailed:
        return "EXISTING FILE COULD NOT BE REPLACED";
    case persist::ProjectFileErrorCode::InvalidFormat:
        return "NOT A MEDIA SERVER PROJECT";
    case persist::ProjectFileErrorCode::UnsupportedVersion:
        return "PROJECT VERSION IS NOT SUPPORTED";
    case persist::ProjectFileErrorCode::CorruptFile:
        return "PROJECT FILE IS DAMAGED OR INCOMPLETE";
    case persist::ProjectFileErrorCode::UnexpectedPayload:
    case persist::ProjectFileErrorCode::InvalidProjectCursor:
        return "NOT A STANDALONE PROJECT";
    case persist::ProjectFileErrorCode::SizeLimitExceeded:
        return "PROJECT EXCEEDS SAFETY LIMITS";
    case persist::ProjectFileErrorCode::AllocationFailed:
        return "NOT ENOUGH MEMORY";
    }
    return "UNKNOWN PROJECT FILE ERROR";
}

[[nodiscard]] std::string
project_file_failure(std::string_view operation,
                     const persist::ProjectFileError &error) {
    std::string message{"ERROR "};
    message.append(operation);
    message.append(" PROJECT: ");
    message.append(project_file_error_label(error.code));
    return message;
}

struct MediaRateEnrichment final {
    doc::Root project;
    std::size_t updated_nodes{};
};

// Projects authored before native video cadence was persisted remain readable
// as 1:1 sequences in SceneModel. At the Director's project boundary, enrich
// only nodes with *neither* rate field. A partial/malformed authored pair is
// deliberately left intact for the schema model to reject, rather than being
// silently repaired. probe_media_frame_rate itself rejects non-video paths, and
// this cache opens each distinct recognized video at most once per load.
[[nodiscard]] MediaRateEnrichment
enrich_media_frame_rates(const doc::Root &project) {
    struct CachedProbe final {
        std::string source;
        std::optional<render::MediaFrameRate> rate;
    };

    MediaRateEnrichment result{project, 0U};
    std::vector<CachedProbe> probes;
    const auto rate_for =
        [&](std::string_view source) -> std::optional<render::MediaFrameRate> {
        const auto found = std::find_if(
            probes.begin(), probes.end(),
            [&](const CachedProbe &entry) { return entry.source == source; });
        if (found != probes.end()) {
            return found->rate;
        }
        const std::string owned{source};
        const auto rate = render::probe_media_frame_rate(owned);
        probes.push_back(CachedProbe{owned, rate});
        return rate;
    };

    const doc::Delta contents = doc::diff(doc::Root{}, project);
    for (const doc::NodeChange &change : contents.changes) {
        if (!change.after) {
            continue;
        }
        const doc::Node &node = *change.after;
        const bool placement =
            std::string_view{node.type} == scene::schema::media_type;
        const bool asset =
            std::string_view{node.type} == assets::schema::media_type;
        if (!placement && !asset) {
            continue;
        }

        const std::string_view numerator_name =
            placement ? scene::schema::source_rate_numerator
                      : assets::schema::media_rate_numerator;
        const std::string_view denominator_name =
            placement ? scene::schema::source_rate_denominator
                      : assets::schema::media_rate_denominator;
        // Any authored member makes this pair intentional. In particular, do
        // not hide a partial pair by filling in its missing half.
        if (node.properties.find(numerator_name) != node.properties.end() ||
            node.properties.find(denominator_name) != node.properties.end()) {
            continue;
        }
        const auto source = node.properties.find(
            placement ? scene::schema::source : assets::schema::source);
        if (source == node.properties.end()) {
            continue;
        }
        const auto *const path = std::get_if<std::string>(&source->second);
        if (path == nullptr || path->empty()) {
            continue;
        }
        const auto rate = rate_for(*path);
        if (!rate.has_value()) {
            continue;
        }

        doc::Node enriched = node;
        enriched.properties.emplace(
            std::string{numerator_name},
            doc::PropertyValue{static_cast<std::int64_t>(rate->numerator)});
        enriched.properties.emplace(
            std::string{denominator_name},
            doc::PropertyValue{static_cast<std::int64_t>(rate->denominator)});
        result.project = result.project.set(std::move(enriched));
        ++result.updated_nodes;
    }
    return result;
}

[[nodiscard]] bool
restore_project_transport(show::TransportAuthority &transport,
                          const show::TransportAnchor &loaded,
                          cmd::FrameNumber at) noexcept {
    if (!show::valid_transport_anchor(loaded)) {
        return false;
    }
    if (transport.state() == show::TransportState::Playing &&
        !transport.pause(at)) {
        return false;
    }
    if (!transport.set_rate(loaded.rate, at) ||
        !transport.set_range(loaded.first_frame, loaded.last_frame, loaded.loop,
                             at) ||
        !transport.set_direction(loaded.direction, at) ||
        !transport.seek(loaded.anchor_show_frame, at)) {
        return false;
    }
    switch (loaded.state) {
    case show::TransportState::Stopped:
        return static_cast<bool>(transport.stop(at));
    case show::TransportState::Paused:
        if (transport.state() == show::TransportState::Paused) {
            return true;
        }
        if (transport.state() == show::TransportState::Stopped &&
            !transport.play(at)) {
            return false;
        }
        return static_cast<bool>(transport.pause(at));
    case show::TransportState::Playing:
        return transport.state() == show::TransportState::Playing ||
               static_cast<bool>(transport.play(at));
    }
    return false;
}

// A switcher dissolve is transient control-frame state, not a timeline asset.
// Loading it verbatim can leave Rehearsal locked until a control-frame ordinal
// from another run is reached. Preserve the stable Program/Rehearsal choices,
// validate that both still name scenes, and settle the transition at this
// Director's load boundary.
[[nodiscard]] bool normalize_project_switcher(doc::Root &project,
                                              cmd::FrameNumber at) {
    const doc::Node *const raw = project.get(show::switcher_node_id);
    const auto loaded = show::read_switcher(project);
    if (!loaded.has_value()) {
        return raw == nullptr ||
               std::string_view{raw->type} != show::switcher_node_type;
    }

    scene::SceneModel scenes;
    scenes.reset(project);
    const auto scene_infos = scenes.scenes();
    const auto contains = [&](scene::SceneId id) {
        return std::any_of(
            scene_infos.begin(), scene_infos.end(),
            [id](const scene::SceneInfo &info) { return info.id == id; });
    };
    if (!contains(loaded->program_scene) ||
        !contains(loaded->rehearsal_scene)) {
        return false;
    }

    auto settled = show::SwitcherAuthority::create(loaded->program_scene,
                                                   loaded->rehearsal_scene, at);
    if (!settled) {
        return false;
    }
    project = project.set(
        show::make_switcher_node(std::move(settled).value().anchor()));
    return true;
}

enum class ProjectLoadStageErrorCode {
    ScheduleFailed,
    PrepareFailed,
    WirePreflightFailed,
    WalPreflightFailed,
    AbortFailed,
    AllocationFailed,
};

struct ProjectLoadStageError final {
    ProjectLoadStageErrorCode code{};
    std::optional<cmd::ScheduleError> schedule_error;
    std::optional<cmd::SealError> seal_error;
    std::optional<persist::CommandLogError> command_log_error;
};

using ProjectLoadStageResult =
    core::Result<cmd::DirectorScheduler, ProjectLoadStageError>;

[[nodiscard]] bool
command_log_limit_error(const persist::CommandLogError &error) noexcept {
    if (const auto *const failure = std::get_if<persist::LogFailure>(&error)) {
        return failure->error.code == persist::LogErrorCode::LimitExceeded;
    }
    if (const auto *const failure =
            std::get_if<persist::EncodeFailure>(&error)) {
        return failure->error == wire::EncodeError::SizeLimitExceeded;
    }
    if (const auto *const failure =
            std::get_if<persist::DecodeFailure>(&error)) {
        return failure->error == wire::DecodeError::SizeLimitExceeded;
    }
    return false;
}

// Build the replacement and its exact next-frame seal away from the live
// scheduler. EditsOnly verifies the replicated wire batch without imposing a
// stale WAL tail; EveryFrame instead runs the complete CommandLog append gate.
// The prepared seal is aborted before returning so the normal barrier remains
// the sole live seal owner.
[[nodiscard]] ProjectLoadStageResult
stage_project_load(const cmd::DirectorScheduler &current,
                   const doc::Root &replacement, cmd::FrameNumber frame,
                   persist::CommandLog &command_log,
                   persist::FrameDurability frame_durability) {
    try {
        cmd::DirectorScheduler staged = current;
        auto replaced = staged.schedule_document_replace(
            replacement, cmd::UndoContextId{7U},
            cmd::TransactionId{frame.value()}, frame);
        if (!replaced) {
            return ProjectLoadStageResult::failure(ProjectLoadStageError{
                ProjectLoadStageErrorCode::ScheduleFailed,
                replaced.error(),
                std::nullopt,
                std::nullopt,
            });
        }

        staged.clear_undo_history();
        auto prepared = staged.prepare_seal(frame);
        if (!prepared) {
            return ProjectLoadStageResult::failure(ProjectLoadStageError{
                ProjectLoadStageErrorCode::PrepareFailed,
                std::nullopt,
                prepared.error(),
                std::nullopt,
            });
        }

        const auto token = prepared.value().token;
        auto preflight =
            frame_durability == persist::FrameDurability::EveryFrame
                ? command_log.preflight(staged.commands_for(frame),
                                        prepared.value().seal)
                : command_log.preflight_wire(staged.commands_for(frame),
                                             prepared.value().seal);
        if (!preflight) {
            return ProjectLoadStageResult::failure(ProjectLoadStageError{
                frame_durability == persist::FrameDurability::EveryFrame
                    ? ProjectLoadStageErrorCode::WalPreflightFailed
                    : ProjectLoadStageErrorCode::WirePreflightFailed,
                std::nullopt,
                std::nullopt,
                preflight.error(),
            });
        }
        if (!staged.abort_prepared_seal(token)) {
            return ProjectLoadStageResult::failure(ProjectLoadStageError{
                ProjectLoadStageErrorCode::AbortFailed,
                std::nullopt,
                std::nullopt,
                std::nullopt,
            });
        }
        return ProjectLoadStageResult::success(std::move(staged));
    } catch (const std::bad_alloc &) {
        return ProjectLoadStageResult::failure(ProjectLoadStageError{
            ProjectLoadStageErrorCode::AllocationFailed,
            std::nullopt,
            std::nullopt,
            std::nullopt,
        });
    } catch (const std::length_error &) {
        return ProjectLoadStageResult::failure(ProjectLoadStageError{
            ProjectLoadStageErrorCode::AllocationFailed,
            std::nullopt,
            std::nullopt,
            std::nullopt,
        });
    }
}
#endif

// Schedules the built-in scene as one transaction of node creations applied at
// `apply_at`. Every edit is an ordinary command, so it replicates, journals and
// undoes through the paths everything else does - there is no second mechanism
// for 3D. Returns false if the scheduler refuses any edit, which is a
// programming error here rather than a runtime condition.
[[nodiscard]] doc::Node default_compositor_output_node(doc::NodeId id,
                                                       doc::NodeId scene) {
    doc::Node node{};
    node.id = id;
    node.type = std::string{compositor::schema::output_type};
    node.properties.emplace(
        std::string{compositor::schema::scene_id},
        doc::PropertyValue{static_cast<std::int64_t>(scene.value())});
    node.properties.emplace(std::string{compositor::schema::label},
                            doc::PropertyValue{std::string{"Output"}});
    node.properties.emplace(std::string{compositor::schema::editor_x},
                            doc::PropertyValue{420.0});
    node.properties.emplace(std::string{compositor::schema::editor_y},
                            doc::PropertyValue{120.0});
    return node;
}

[[nodiscard]] bool author_scene_document(cmd::DirectorScheduler &scheduler,
                                         cmd::FrameNumber apply_at) {
    const cmd::UndoContextId undo_context{1U};
    const cmd::TransactionId transaction{1U};
    for (const auto &node : show::demo_scene()) {
        const auto node_id = node.id;
        auto scheduled =
            scheduler.schedule_edit(cmd::PutNode{node}, cmd::EraseNode{node_id},
                                    undo_context, transaction, apply_at);
        if (!scheduled) {
            return false;
        }
    }
    const doc::Node output = default_compositor_output_node(
        doc::NodeId{show::demo_scene_node_base + 8U},
        doc::NodeId{show::demo_scene_node_base});
    if (!scheduler.schedule_edit(cmd::PutNode{output},
                                 cmd::EraseNode{output.id}, undo_context,
                                 transaction, apply_at)) {
        return false;
    }
    return true;
}

[[nodiscard]] doc::Node editor_canvas_feed_map_node(core::u32 width,
                                                    core::u32 height) {
    namespace schema = output::schema;

    doc::Node node{};
    node.id = doc::NodeId{show::demo_feed_map_node_base};
    node.type = std::string{schema::map_type};
    node.properties.emplace(std::string{schema::node_index},
                            doc::PropertyValue{std::int64_t{0}});
    node.properties.emplace(std::string{schema::source_x},
                            doc::PropertyValue{std::int64_t{0}});
    node.properties.emplace(std::string{schema::source_y},
                            doc::PropertyValue{std::int64_t{0}});
    node.properties.emplace(
        std::string{schema::source_width},
        doc::PropertyValue{static_cast<std::int64_t>(width)});
    node.properties.emplace(
        std::string{schema::source_height},
        doc::PropertyValue{static_cast<std::int64_t>(height)});
    node.properties.emplace(std::string{schema::viewport_x},
                            doc::PropertyValue{std::int64_t{0}});
    node.properties.emplace(std::string{schema::viewport_y},
                            doc::PropertyValue{std::int64_t{0}});
    node.properties.emplace(
        std::string{schema::viewport_width},
        doc::PropertyValue{static_cast<std::int64_t>(width)});
    node.properties.emplace(
        std::string{schema::viewport_height},
        doc::PropertyValue{static_cast<std::int64_t>(height)});
    return node;
}

// The initial feed map, scheduled the same way as the scene: ordinary
// node-creation commands in one transaction, so it replicates, journals, and
// undoes through the same paths. Explicit output demos keep their two slices;
// a fresh editor gets one full-canvas node-0 slice for its supervised output.
[[nodiscard]] bool author_feed_map_document(cmd::DirectorScheduler &scheduler,
                                            cmd::FrameNumber apply_at,
                                            InitialFeedMapPlan plan) {
    std::vector<doc::Node> nodes;
    switch (plan.kind) {
    case InitialFeedMapKind::None:
        return true;
    case InitialFeedMapKind::OutputDemo:
        nodes = show::demo_feed_map();
        break;
    case InitialFeedMapKind::EditorCanvas:
        nodes.push_back(
            editor_canvas_feed_map_node(plan.source_width, plan.source_height));
        break;
    }

    const cmd::UndoContextId undo_context{1U};
    const cmd::TransactionId transaction{1U};
    for (const auto &node : nodes) {
        const auto node_id = node.id;
        auto scheduled =
            scheduler.schedule_edit(cmd::PutNode{node}, cmd::EraseNode{node_id},
                                    undo_context, transaction, apply_at);
        if (!scheduled) {
            return false;
        }
    }
    return true;
}

#if defined(MEDIASERVER_HAS_GUI)
// Writes the demo clip as a short image sequence of TGAs to a temp directory
// and returns a '#'-run template the renderer resolves per frame. A stand-in
// for real media the operator would drop in: enough to have something on the
// wall to drag while the show plays. TGA because it needs no encoder library.
[[nodiscard]] std::string write_demo_clip() {
    namespace fs = std::filesystem;
    const auto directory = fs::temp_directory_path() / "mediaserver-editclip";
    std::error_code ec;
    fs::create_directories(directory, ec);

    constexpr core::u32 width = 320U;
    constexpr core::u32 height = 180U;
    std::array<core::u8, 18U> header{};
    header[2U] = 2U; // uncompressed true-colour
    header[12U] = static_cast<core::u8>(width & 0xffU);
    header[13U] = static_cast<core::u8>((width >> 8U) & 0xffU);
    header[14U] = static_cast<core::u8>(height & 0xffU);
    header[15U] = static_cast<core::u8>((height >> 8U) & 0xffU);
    header[16U] = 32U;
    header[17U] = 0x28U; // top-left origin, 8 alpha bits

    std::vector<core::u8> pixels(static_cast<std::size_t>(width) * height * 4U,
                                 0U);
    for (core::u32 frame = 0U; frame < show::demo_clip_frames; ++frame) {
        const auto bar = (frame * width) / show::demo_clip_frames;
        for (core::u32 y = 0U; y < height; ++y) {
            for (core::u32 x = 0U; x < width; ++x) {
                const auto index =
                    (static_cast<std::size_t>(y) * width + x) * 4U;
                auto r = static_cast<core::u8>((x * 255U) / (width - 1U));
                auto g = static_cast<core::u8>((y * 255U) / (height - 1U));
                auto b = static_cast<core::u8>((frame * 255U) /
                                               (show::demo_clip_frames - 1U));
                if (x >= bar && x < bar + 14U) {
                    r = 255U;
                    g = 255U;
                    b = 255U;
                }
                pixels[index + 0U] = b; // TGA is BGRA
                pixels[index + 1U] = g;
                pixels[index + 2U] = r;
                pixels[index + 3U] = 255U;
            }
        }
        char name[32]{};
        std::snprintf(name, sizeof(name), "editclip_%04u.tga", frame);
        std::ofstream out{directory / name, std::ios::binary | std::ios::trunc};
        out.write(reinterpret_cast<const char *>(header.data()),
                  static_cast<std::streamsize>(header.size()));
        out.write(reinterpret_cast<const char *>(pixels.data()),
                  static_cast<std::streamsize>(pixels.size()));
    }
    return (directory / "editclip_####.tga").string();
}

// Authors the edit scene the same way the other demos are authored: one
// transaction of node creations, replicating and undoing through the ordinary
// paths.
[[nodiscard]] bool author_edit_document(cmd::DirectorScheduler &scheduler,
                                        cmd::FrameNumber apply_at,
                                        const std::string &clip_source) {
    for (const auto &node : show::demo_edit_scene(clip_source)) {
        const auto node_id = node.id;
        auto scheduled = scheduler.schedule_edit(
            cmd::PutNode{node}, cmd::EraseNode{node_id}, cmd::UndoContextId{1U},
            cmd::TransactionId{1U}, apply_at);
        if (!scheduled) {
            return false;
        }
    }
    const doc::Node output = default_compositor_output_node(
        doc::NodeId{show::demo_edit_node_base + 7U},
        doc::NodeId{show::demo_edit_node_base});
    if (!scheduler.schedule_edit(
            cmd::PutNode{output}, cmd::EraseNode{output.id},
            cmd::UndoContextId{1U}, cmd::TransactionId{1U}, apply_at)) {
        return false;
    }
    return true;
}

[[nodiscard]] std::string_view
transform_property(scene::TransformChannel component) noexcept;

// A deterministic node id for the channel driving one typed property of
// `target`. Deterministic so keying finds-or-creates a channel without scanning
// the document. Preserve the original low-nibble address space for the first
// sixteen channels, while assigning later channels to a disjoint high-bit
// prefix with five component bits.
[[nodiscard]] doc::NodeId channel_node_id(doc::NodeId target,
                                          core::u64 component_index) noexcept {
    constexpr core::u64 legacy_channel_id_base = 0x7C00'0000'0000'0000ULL;
    constexpr core::u64 extended_channel_id_base = 0x7D00'0000'0000'0000ULL;
    if (component_index < 16U) {
        return doc::NodeId{legacy_channel_id_base | (target.value() << 4U) |
                           component_index};
    }
    return doc::NodeId{extended_channel_id_base | (target.value() << 5U) |
                       component_index};
}

[[nodiscard]] doc::Node make_channel_node(doc::NodeId id, doc::NodeId target,
                                          std::string_view component) {
    doc::Node node{};
    node.id = id;
    node.type = std::string{scene::schema::channel_type};
    node.properties.insert_or_assign(
        std::string{scene::schema::channel_target},
        doc::PropertyValue{static_cast<std::int64_t>(target.value())});
    node.properties.insert_or_assign(
        std::string{scene::schema::channel_component},
        doc::PropertyValue{std::string{component}});
    return node;
}

// Inserts or updates the key at `frame` in a channel node's indexed key
// properties. Keys are kept contiguous, so the first missing index is the next
// free slot and a matching frame just has its value replaced.
void upsert_channel_key(doc::Node &node, core::u64 frame, double value) {
    const std::string prefix{scene::schema::channel_key_prefix};
    const std::string frame_suffix{scene::schema::channel_key_frame_suffix};
    const std::string value_suffix{scene::schema::channel_key_value_suffix};
    core::u64 next_index = 0U;
    for (core::u64 i = 0U;; ++i) {
        const auto found =
            node.properties.find(prefix + std::to_string(i) + frame_suffix);
        if (found == node.properties.end()) {
            next_index = i;
            break;
        }
        const auto *const held = std::get_if<std::int64_t>(&found->second);
        if (held != nullptr && *held == static_cast<std::int64_t>(frame)) {
            node.properties.insert_or_assign(prefix + std::to_string(i) +
                                                 value_suffix,
                                             doc::PropertyValue{value});
            return;
        }
    }
    const auto index = std::to_string(next_index);
    node.properties.insert_or_assign(
        prefix + index + frame_suffix,
        doc::PropertyValue{static_cast<std::int64_t>(frame)});
    node.properties.insert_or_assign(prefix + index + value_suffix,
                                     doc::PropertyValue{value});
    node.properties.insert_or_assign(
        prefix + index + std::string{scene::schema::channel_key_interp_suffix},
        doc::PropertyValue{std::string{scene::schema::interp_linear}});
}

// Removes one indexed key and compacts every later key down by one slot. The
// channel schema stops parsing at its first missing index, so leaving a hole
// would silently hide all later animation from every node.
[[nodiscard]] bool erase_channel_key(doc::Node &node, core::u64 frame) {
    constexpr core::u64 maximum =
        static_cast<core::u64>(std::numeric_limits<core::i64>::max());
    if (frame > maximum) {
        return false;
    }
    const auto wanted = static_cast<std::int64_t>(frame);
    const std::string prefix{scene::schema::channel_key_prefix};
    const std::string frame_suffix{scene::schema::channel_key_frame_suffix};
    const std::string value_suffix{scene::schema::channel_key_value_suffix};
    const std::string interp_suffix{scene::schema::channel_key_interp_suffix};

    core::u64 count = 0U;
    core::u64 remove_at = std::numeric_limits<core::u64>::max();
    for (core::u64 index = 0U;; ++index) {
        const auto found =
            node.properties.find(prefix + std::to_string(index) + frame_suffix);
        if (found == node.properties.end()) {
            count = index;
            break;
        }
        const auto *const held = std::get_if<std::int64_t>(&found->second);
        if (held != nullptr && *held == wanted &&
            remove_at == std::numeric_limits<core::u64>::max()) {
            remove_at = index;
        }
    }
    if (remove_at == std::numeric_limits<core::u64>::max()) {
        return false;
    }

    const std::array<std::string_view, 3U> suffixes{frame_suffix, value_suffix,
                                                    interp_suffix};
    for (core::u64 index = remove_at; index + 1U < count; ++index) {
        for (const std::string_view suffix : suffixes) {
            const std::string destination =
                prefix + std::to_string(index) + std::string{suffix};
            const std::string source =
                prefix + std::to_string(index + 1U) + std::string{suffix};
            const auto found = node.properties.find(source);
            if (found != node.properties.end()) {
                node.properties.insert_or_assign(destination, found->second);
            } else {
                node.properties.erase(destination);
            }
        }
    }
    const std::string last_index = std::to_string(count - 1U);
    for (const std::string_view suffix : suffixes) {
        node.properties.erase(prefix + last_index + std::string{suffix});
    }
    return true;
}

// Finds the key at `from` in a channel node's indexed properties and sets its
// frame to `to`. Returns false when there is no key at that frame. Value and
// interp stay; only the frame moves, which the model re-sorts on parse.
[[nodiscard]] bool retime_channel_key(doc::Node &node, core::i64 from,
                                      core::i64 to) {
    const std::string prefix{scene::schema::channel_key_prefix};
    const std::string frame_suffix{scene::schema::channel_key_frame_suffix};
    for (core::u64 i = 0U;; ++i) {
        const auto name = prefix + std::to_string(i) + frame_suffix;
        const auto found = node.properties.find(name);
        if (found == node.properties.end()) {
            return false;
        }
        const auto *const held = std::get_if<std::int64_t>(&found->second);
        if (held != nullptr && *held == from) {
            node.properties.insert_or_assign(
                name, doc::PropertyValue{static_cast<std::int64_t>(to)});
            return true;
        }
    }
}

// Authors one keyframe on a single transform component: create-or-update the
// component's channel with a key at the frame, or clear the channel. On the
// same scene undo context as moves; the gesture groups a multi-component key.
[[nodiscard]] bool schedule_key_edit(cmd::DirectorScheduler &scheduler,
                                     const editor::KeyIntent &key,
                                     cmd::FrameNumber apply_at,
                                     EditCoalescingPlan *coalescing = nullptr) {
    const auto schedule = [&](cmd::Payload forward, cmd::Payload inverse) {
        if (coalescing != nullptr) {
            return schedule_editor_coalesced_edit(scheduler, std::move(forward),
                                                  std::move(inverse),
                                                  *coalescing, apply_at);
        }
        return scheduler.schedule_edit(
            std::move(forward), std::move(inverse), cmd::UndoContextId{2U},
            cmd::TransactionId{key.gesture}, apply_at);
    };
    const auto id =
        channel_node_id(key.target, static_cast<core::u64>(key.component));
    const auto *const existing = scheduler.projected_document().get(id);
    if (key.clear) {
        if (existing == nullptr) {
            return true;
        }
        auto scheduled = schedule(cmd::EraseNode{id}, cmd::PutNode{*existing});
        return static_cast<bool>(scheduled);
    }
    if (key.erase) {
        if (existing == nullptr) {
            return true;
        }
        doc::Node node = *existing;
        if (!erase_channel_key(node, key.frame)) {
            return true;
        }
        const std::string first_frame =
            std::string{scene::schema::channel_key_prefix} + "0" +
            std::string{scene::schema::channel_key_frame_suffix};
        cmd::Payload payload = node.properties.contains(first_frame)
                                   ? cmd::Payload{cmd::PutNode{std::move(node)}}
                                   : cmd::Payload{cmd::EraseNode{id}};
        auto scheduled = schedule(std::move(payload), cmd::PutNode{*existing});
        return static_cast<bool>(scheduled);
    }
    doc::Node node = existing != nullptr
                         ? *existing
                         : make_channel_node(id, key.target,
                                             transform_property(key.component));
    upsert_channel_key(node, key.frame, key.value);
    cmd::Payload inverse = existing != nullptr
                               ? cmd::Payload{cmd::PutNode{*existing}}
                               : cmd::Payload{cmd::EraseNode{id}};
    auto scheduled =
        schedule(cmd::PutNode{std::move(node)}, std::move(inverse));
    return static_cast<bool>(scheduled);
}

// Authors a retime: move the key at `from_frame` on (target, component) to
// `to_frame`. A missing channel or key is a no-op, not a failure.
[[nodiscard]] bool schedule_key_move(cmd::DirectorScheduler &scheduler,
                                     const editor::KeyMove &move,
                                     cmd::FrameNumber apply_at) {
    const auto id =
        channel_node_id(move.target, static_cast<core::u64>(move.component));
    const auto *const existing = scheduler.projected_document().get(id);
    if (existing == nullptr) {
        return true;
    }
    doc::Node node = *existing;
    if (!retime_channel_key(node, move.from_frame, move.to_frame)) {
        return true;
    }
    auto scheduled = scheduler.schedule_edit(
        cmd::PutNode{std::move(node)}, cmd::PutNode{*existing},
        cmd::UndoContextId{2U}, cmd::TransactionId{move.gesture}, apply_at);
    return static_cast<bool>(scheduled);
}

// Maps an editor transport request to the operator command the transport
// already understands, so timeline buttons and keyboard controls share one
// path.
[[nodiscard]] show::PresenterCommand
map_transport(editor::TransportRequest request) noexcept {
    switch (request) {
    case editor::TransportRequest::TogglePlay:
        return show::PresenterCommand::TogglePlay;
    case editor::TransportRequest::StepBack:
        return show::PresenterCommand::StepBack;
    case editor::TransportRequest::StepForward:
        return show::PresenterCommand::StepForward;
    case editor::TransportRequest::JumpBack:
        return show::PresenterCommand::JumpBack;
    case editor::TransportRequest::JumpForward:
        return show::PresenterCommand::JumpForward;
    case editor::TransportRequest::GoToStart:
        return show::PresenterCommand::GoToStart;
    case editor::TransportRequest::None:
        break;
    }
    return show::PresenterCommand::None;
}

// The document property a transform channel maps to. Position/rotation/scale
// reuse the object transform names, so a channel and a base edit name the same
// property.
[[nodiscard]] std::string_view
transform_property(scene::TransformChannel component) noexcept {
    switch (component) {
    case scene::TransformChannel::PositionX:
        return scene::schema::position_x;
    case scene::TransformChannel::PositionY:
        return scene::schema::position_y;
    case scene::TransformChannel::PositionZ:
        return scene::schema::position_z;
    case scene::TransformChannel::RotationYaw:
        return scene::schema::rotation_yaw;
    case scene::TransformChannel::RotationPitch:
        return scene::schema::rotation_pitch;
    case scene::TransformChannel::RotationRoll:
        return scene::schema::rotation_roll;
    case scene::TransformChannel::ScaleX:
        return scene::schema::scale_x;
    case scene::TransformChannel::ScaleY:
        return scene::schema::scale_y;
    case scene::TransformChannel::ScaleZ:
        return scene::schema::scale_z;
    case scene::PropertyChannel::Opacity:
        return scene::schema::opacity;
    case scene::PropertyChannel::Brightness:
        return scene::schema::brightness;
    case scene::PropertyChannel::Saturation:
        return scene::schema::saturation;
    case scene::PropertyChannel::BlurRadius:
        return scene::schema::blur_radius;
    case scene::PropertyChannel::FilmGrainAmount:
        return scene::schema::film_grain_amount;
    case scene::PropertyChannel::BoxBlurRadius:
        return scene::schema::box_blur_radius;
    case scene::PropertyChannel::DirectionalBlurLength:
        return scene::schema::directional_blur_length;
    case scene::PropertyChannel::ChromaKeyMix:
        return scene::schema::chroma_key_mix;
    case scene::PropertyChannel::LumaKeyMix:
        return scene::schema::luma_key_mix;
    case scene::TransformChannel::Count:
        break;
    }
    return {};
}

[[nodiscard]] double read_transform(const doc::Node &node,
                                    std::string_view name,
                                    double fallback) noexcept {
    const auto found = node.properties.find(name);
    if (found == node.properties.end()) {
        return fallback;
    }
    if (const auto *const real = std::get_if<double>(&found->second)) {
        return *real;
    }
    if (const auto *const whole = std::get_if<std::int64_t>(&found->second)) {
        return static_cast<double>(*whole);
    }
    return fallback;
}

// The authored transform of each scene element, for the editor's Inspector.
// Read from the projected document (the base values, before channels), so the
// Inspector shows and edits what an edit changes rather than the animated
// result.
[[nodiscard]] std::vector<editor::ElementInfo> scene_elements(
    const cmd::DirectorScheduler &scheduler, const scene::Scene &scene,
    std::span<const scene::ClipPlacement> clips = {}, core::u64 show_frame = 0U,
    std::span<const scene::TimelineLane> lanes = {}) {
    std::vector<editor::ElementInfo> elements;
    const auto read = [&scheduler](doc::NodeId node, editor::ElementKind kind) {
        editor::ElementInfo info{};
        info.node = node;
        info.kind = kind;
        const auto *const doc_node = scheduler.projected_document().get(node);
        if (doc_node == nullptr) {
            return info;
        }
        const auto number = [doc_node](std::string_view name, double fallback) {
            return static_cast<float>(
                read_transform(*doc_node, name, fallback));
        };
        const auto boolean = [doc_node](std::string_view name, bool fallback) {
            const auto found = doc_node->properties.find(name);
            if (found == doc_node->properties.end()) {
                return fallback;
            }
            if (const auto *const value = std::get_if<bool>(&found->second)) {
                return *value;
            }
            if (const auto *const value = std::get_if<double>(&found->second)) {
                return std::isfinite(*value) ? *value >= 0.5 : fallback;
            }
            if (const auto *const value =
                    std::get_if<std::int64_t>(&found->second)) {
                return *value != 0;
            }
            return fallback;
        };
        info.locked = boolean(scene::schema::locked, false);
        info.position = scene::Vec3{number(scene::schema::position_x, 0.0),
                                    number(scene::schema::position_y, 0.0),
                                    number(scene::schema::position_z, 0.0)};
        info.rotation = scene::Vec3{number(scene::schema::rotation_yaw, 0.0),
                                    number(scene::schema::rotation_pitch, 0.0),
                                    number(scene::schema::rotation_roll, 0.0)};
        info.scale = scene::Vec3{number(scene::schema::scale_x, 1.0),
                                 number(scene::schema::scale_y, 1.0),
                                 number(scene::schema::scale_z, 1.0)};
        info.color = scene::Vec3{number(scene::schema::color_r, 1.0),
                                 number(scene::schema::color_g, 1.0),
                                 number(scene::schema::color_b, 1.0)};
        info.intensity = number(scene::schema::intensity, 1.0);
        info.direction = scene::Vec3{number(scene::schema::direction_x, 0.0),
                                     number(scene::schema::direction_y, -1.0),
                                     number(scene::schema::direction_z, 0.0)};
        info.range = number(scene::schema::range, 10.0);
        info.material.base_color = info.color;
        info.material.roughness = number(scene::schema::roughness, 0.5);
        info.material.metallic = number(scene::schema::metallic, 0.0);
        info.material.emissive =
            scene::Vec3{number(scene::schema::emissive_r, 0.0),
                        number(scene::schema::emissive_g, 0.0),
                        number(scene::schema::emissive_b, 0.0)};
        info.style.opacity =
            std::clamp(number(scene::schema::opacity, 1.0), 0.0F, 1.0F);
        const int blend =
            static_cast<int>(number(scene::schema::blend_mode, 0.0));
        if (blend >= 0 && blend <= 3) {
            info.style.blend = static_cast<scene::BlendMode>(blend);
        }
        info.style.effects.brightness_enabled =
            boolean(scene::schema::brightness_enabled, false);
        info.style.effects.brightness = number(scene::schema::brightness, 1.0);
        info.style.effects.blur_enabled =
            boolean(scene::schema::blur_enabled, false);
        info.style.effects.blur_radius =
            number(scene::schema::blur_radius, 0.0);
        const auto text = [doc_node](std::string_view name) {
            const auto found = doc_node->properties.find(name);
            if (found == doc_node->properties.end()) {
                return std::string{};
            }
            if (const auto *const value =
                    std::get_if<std::string>(&found->second)) {
                return *value;
            }
            return std::string{};
        };
        const auto integer = [doc_node](std::string_view name,
                                        core::u32 fallback) {
            const auto found = doc_node->properties.find(name);
            if (found == doc_node->properties.end()) {
                return fallback;
            }
            if (const auto *const value =
                    std::get_if<std::int64_t>(&found->second);
                value != nullptr && *value >= 0 &&
                *value <= static_cast<std::int64_t>(UINT32_MAX)) {
                return static_cast<core::u32>(*value);
            }
            return fallback;
        };
        const auto signed_integer = [doc_node](std::string_view name,
                                               core::i64 fallback) {
            const auto found = doc_node->properties.find(name);
            if (found == doc_node->properties.end()) {
                return fallback;
            }
            if (const auto *const value =
                    std::get_if<std::int64_t>(&found->second)) {
                return static_cast<core::i64>(*value);
            }
            return fallback;
        };
        const auto optional_reference = [doc_node](std::string_view name) {
            const auto found = doc_node->properties.find(name);
            if (found == doc_node->properties.end()) {
                return std::optional<doc::NodeId>{};
            }
            if (const auto *const value =
                    std::get_if<std::int64_t>(&found->second);
                value != nullptr && *value > 0) {
                return std::optional<doc::NodeId>{
                    doc::NodeId{static_cast<core::u64>(*value)}};
            }
            return std::optional<doc::NodeId>{};
        };
        info.source = text(scene::schema::source);
        info.sequence_start = integer(scene::schema::sequence_start, 0U);
        info.sequence_count = integer(scene::schema::sequence_count, 1U);
        info.display_name = text(scene::schema::group_name);
        if (info.display_name.empty() && !info.source.empty() &&
            (kind == editor::ElementKind::Media ||
             kind == editor::ElementKind::Model)) {
            info.display_name = native_path_to_utf8(
                native_path_from_utf8(info.source).filename());
        }
        info.parent_group = optional_reference(scene::schema::parent_group);
        info.group_order = signed_integer(scene::schema::group_order, 0);
        info.visible = boolean(scene::schema::visible, true);
        info.effective_visible = info.visible;
        return info;
    };
    const auto apply_parent_state = [&scene](editor::ElementInfo &info) {
        if (!info.parent_group.has_value()) {
            return;
        }
        const auto parent =
            std::find_if(scene.groups.begin(), scene.groups.end(),
                         [&](const scene::SceneGroup &group) noexcept {
                             return group.node == *info.parent_group;
                         });
        if (parent == scene.groups.end()) {
            return;
        }
        info.locked = info.locked || parent->effective_locked;
        info.effective_visible =
            info.effective_visible && parent->effective_visible;
    };
    // Every authored clip placement, on air or not. A clip the playhead has
    // moved past is still the operator's to select, drag, and trim, so the
    // authored list rather than the evaluated Scene is what the panels get.
    // `clips` is empty for callers with no timeline, which keeps the original
    // evaluated-media behaviour.
    for (const auto &clip : clips) {
        auto info = read(clip.node, editor::ElementKind::Media);
        info.timing = clip.timing;
        info.source_frame_rate = clip.frame_rate;
        info.timeline_lane = clip.lane;
        info.clip_order = clip.order;
        info.parent_group = clip.parent_group;
        info.group_order = clip.group_order;
        info.locked = info.locked || clip.locked;
        const auto lane = std::find_if(lanes.begin(), lanes.end(),
                                       [&](const scene::TimelineLane &value) {
                                           return value.id == clip.lane;
                                       });
        if (lane != lanes.end()) {
            info.locked = info.locked || lane->locked;
            if (!lane->implicit) {
                info.screen_name = lane->name;
            }
        }
        const auto evaluated =
            std::find_if(scene.media.begin(), scene.media.end(),
                         [&clip](const scene::MediaInstance &value) noexcept {
                             return value.source_node == clip.node;
                         });
        info.on_air = evaluated != scene.media.end();
        if (info.on_air) {
            const float authored = info.style.opacity;
            info.style = evaluated->style;
            // A head or tail ramp belongs to the placement, not to the layer.
            // Leaving it folded into the opacity the Inspector shows would let
            // one nudge during a dissolve bake that dissolve into the authored
            // value, so it is divided back out. Where the ramp is fully out
            // there is nothing to divide by and the document's own value
            // stands.
            const float fade =
                clip.timing.placed()
                    ? scene::clip_fade(
                          clip.timing,
                          static_cast<core::i64>(std::min(
                              show_frame,
                              static_cast<core::u64>(
                                  std::numeric_limits<std::int64_t>::max()))))
                    : 1.0F;
            info.style.opacity =
                fade > 0.0F ? std::min(evaluated->style.opacity / fade, 1.0F)
                            : authored;
            info.locked = info.locked || evaluated->locked;
        }
        apply_parent_state(info);
        elements.push_back(std::move(info));
    }
    if (clips.empty()) {
        for (const auto &clip : scene.media) {
            auto info = read(clip.source_node, editor::ElementKind::Media);
            info.style = clip.style;
            info.locked = info.locked || clip.locked;
            info.parent_group = clip.parent_group;
            info.group_order = clip.group_order;
            apply_parent_state(info);
            elements.push_back(std::move(info));
        }
    }
    for (const auto &object : scene.instances) {
        auto info = read(object.source, editor::ElementKind::Object);
        info.style = object.style;
        info.locked = info.locked || object.locked;
        info.parent_group = object.parent_group;
        info.group_order = object.group_order;
        apply_parent_state(info);
        elements.push_back(std::move(info));
    }
    for (const auto &model : scene.models) {
        auto info = read(model.source_node, editor::ElementKind::Model);
        info.style = model.style;
        info.locked = info.locked || model.locked;
        info.parent_group = model.parent_group;
        info.group_order = model.group_order;
        apply_parent_state(info);
        elements.push_back(std::move(info));
    }
    for (const auto &light : scene.directional_lights) {
        elements.push_back(
            read(light.source, editor::ElementKind::DirectionalLight));
    }
    for (const auto &light : scene.point_lights) {
        elements.push_back(read(light.source, editor::ElementKind::PointLight));
    }
    // Groups remain editor-visible even when they (and therefore their
    // drawables) are hidden. Their authored local transform is read from the
    // document; evaluated metadata supplies inherited lock/visibility truth.
    for (const scene::SceneGroup &group : scene.groups) {
        auto info = read(group.node, editor::ElementKind::Group);
        info.display_name = group.name;
        info.parent_group = group.parent_group;
        info.group_order = group.group_order;
        info.locked = group.effective_locked;
        info.visible = group.visible;
        info.effective_visible = group.effective_visible;
        elements.push_back(std::move(info));
    }
    // Tracks are elements because a track is a screen: it has a transform to
    // inspect, a format to set, and a look to grade. The implicit lane is not
    // one - it owns no plane, and a row that cannot be selected is better than
    // a selection that turns out to control nothing.
    for (const scene::TimelineLane &lane : lanes) {
        if (lane.implicit) {
            continue;
        }
        auto info = read(lane.id, editor::ElementKind::Track);
        info.display_name = lane.name;
        // The projection already resolved the aspect lock, so this is the
        // extent the screen renders with rather than the pair of numbers the
        // document happens to hold.
        info.scale = lane.scale;
        info.style.opacity = lane.opacity;
        info.style.blend = lane.compositing;
        info.raster_width = lane.raster_width;
        info.raster_height = lane.raster_height;
        info.aspect_locked = lane.aspect_locked;
        info.locked = lane.locked;
        // Muting a track is what hiding it means, so one control reads and
        // writes one state rather than the timeline and the Inspector each
        // owning half of it.
        info.visible = !lane.muted;
        info.effective_visible = info.visible;
        elements.push_back(std::move(info));
    }
    return elements;
}

// Authors one Inspector edit of a named property (a light field). Reads the
// node, sets the property, PutNode with the restoring inverse.
[[nodiscard]] bool schedule_generic_edit(
    cmd::DirectorScheduler &scheduler, const editor::GenericEdit &edit,
    cmd::FrameNumber apply_at, EditCoalescingPlan *coalescing = nullptr) {
    if (edit.property.empty()) {
        return true;
    }
    const auto *const existing = scheduler.projected_document().get(edit.node);
    if (existing == nullptr) {
        return true;
    }
    doc::Node updated = *existing;
    if (edit.property == scene::schema::box_blur_radius) {
        if (!std::isfinite(edit.value)) {
            return true;
        }
        const auto radius = static_cast<std::int64_t>(std::llround(
            std::clamp(edit.value, 0.0,
                       static_cast<double>(scene::max_box_blur_radius_px))));
        updated.properties.insert_or_assign(edit.property,
                                            doc::PropertyValue{radius});
    } else if (edit.property == scene::schema::lane_raster_width ||
               edit.property == scene::schema::lane_raster_height) {
        if (!std::isfinite(edit.value)) {
            return true;
        }
        // A raster is whole pixels, and the scene model reads it as an
        // integer: writing 1920.0 as a double would make the track node
        // unreadable and the screen disappear.
        const auto extent = static_cast<std::int64_t>(std::llround(std::clamp(
            edit.value, 1.0, static_cast<double>(scene::max_raster_extent))));
        updated.properties.insert_or_assign(edit.property,
                                            doc::PropertyValue{extent});
    } else if (edit.property == scene::schema::blend_mode &&
               existing->type == scene::schema::timeline_lane_type) {
        // A track spells its blend as a mode name, not as the numeric
        // blend_mode every drawable uses. Translating here keeps the
        // Inspector generic and keeps the track with one authored blend
        // rather than two properties that can disagree.
        if (!std::isfinite(edit.value)) {
            return true;
        }
        const auto index = static_cast<int>(std::llround(edit.value));
        std::string_view mode = scene::schema::lane_normal_mode;
        if (index == 1) {
            mode = scene::schema::lane_add_mode;
        } else if (index == 2) {
            mode = scene::schema::lane_multiply_mode;
        } else if (index == 3) {
            mode = scene::schema::lane_screen_mode;
        } else if (index != 0) {
            return true;
        }
        updated.properties.insert_or_assign(
            std::string{scene::schema::lane_compositing_mode},
            doc::PropertyValue{std::string{mode}});
    } else if (edit.property == scene::schema::lane_aspect_locked) {
        updated.properties.insert_or_assign(
            edit.property, doc::PropertyValue{edit.value >= 0.5});
    } else if (edit.property == scene::schema::effect_enabled) {
        // Switching an effect off is not deleting it: the node, its parameters,
        // and its key channels all stay, and the scene model reads this as a
        // flag rather than as the double every scalar property uses.
        if (existing->type != scene::schema::effect_type) {
            return true;
        }
        updated.properties.insert_or_assign(
            edit.property, doc::PropertyValue{edit.value >= 0.5});
    } else if (edit.property == scene::schema::box_blur_passes) {
        if (!std::isfinite(edit.value)) {
            return true;
        }
        const auto passes = static_cast<std::int64_t>(std::llround(std::clamp(
            edit.value, 1.0, static_cast<double>(scene::max_box_blur_passes))));
        updated.properties.insert_or_assign(edit.property,
                                            doc::PropertyValue{passes});
    } else if (edit.property == scene::schema::chroma_key_morphology_px) {
        if (!std::isfinite(edit.value)) {
            return true;
        }
        const auto radius = static_cast<std::int64_t>(std::llround(std::clamp(
            edit.value,
            -static_cast<double>(scene::max_key_morphology_radius_px),
            static_cast<double>(scene::max_key_morphology_radius_px))));
        updated.properties.insert_or_assign(edit.property,
                                            doc::PropertyValue{radius});
    } else if (edit.property == scene::schema::luma_key_invert) {
        updated.properties.insert_or_assign(
            edit.property, doc::PropertyValue{edit.value >= 0.5});
    } else if (edit.property == scene::schema::chroma_key_color_r ||
               edit.property == scene::schema::chroma_key_color_g ||
               edit.property == scene::schema::chroma_key_color_b ||
               edit.property == scene::schema::chroma_key_tolerance ||
               edit.property == scene::schema::chroma_key_softness ||
               edit.property == scene::schema::chroma_key_spill_suppression ||
               edit.property == scene::schema::luma_key_low_threshold ||
               edit.property == scene::schema::luma_key_high_threshold ||
               edit.property == scene::schema::luma_key_low_softness ||
               edit.property == scene::schema::luma_key_high_softness) {
        if (!std::isfinite(edit.value)) {
            return true;
        }
        double value = std::clamp(edit.value, 0.0, 1.0);
        const auto number_property = [&](std::string_view property,
                                         double fallback) {
            const auto found = existing->properties.find(std::string{property});
            if (found == existing->properties.end()) {
                return fallback;
            }
            const auto *const number = std::get_if<double>(&found->second);
            return number != nullptr && std::isfinite(*number)
                       ? std::clamp(*number, 0.0, 1.0)
                       : fallback;
        };
        if (edit.property == scene::schema::luma_key_low_threshold) {
            value = std::min(
                value,
                number_property(scene::schema::luma_key_high_threshold, 1.0));
        } else if (edit.property == scene::schema::luma_key_high_threshold) {
            value = std::max(
                value,
                number_property(scene::schema::luma_key_low_threshold, 0.0));
        }
        updated.properties.insert_or_assign(edit.property,
                                            doc::PropertyValue{value});
    } else {
        updated.properties.insert_or_assign(edit.property,
                                            doc::PropertyValue{edit.value});
    }
    auto scheduled = coalescing != nullptr
                         ? schedule_editor_coalesced_edit(
                               scheduler, cmd::PutNode{std::move(updated)},
                               cmd::PutNode{*existing}, *coalescing, apply_at)
                         : scheduler.schedule_edit(
                               cmd::PutNode{std::move(updated)},
                               cmd::PutNode{*existing}, cmd::UndoContextId{2U},
                               cmd::TransactionId{edit.gesture}, apply_at);
    return static_cast<bool>(scheduled);
}

[[nodiscard]] bool schedule_lock_edit(cmd::DirectorScheduler &scheduler,
                                      const editor::LockEdit &edit,
                                      cmd::FrameNumber apply_at) {
    const auto *const existing = scheduler.projected_document().get(edit.node);
    if (existing == nullptr) {
        return true;
    }
    const bool surface = existing->type == output::schema::surface_type;
    const bool track = existing->type == scene::schema::timeline_lane_type;
    const bool scene_element = existing->type == scene::schema::object_type ||
                               existing->type == scene::schema::media_type ||
                               existing->type == scene::schema::model_type ||
                               existing->type == scene::schema::light_type;
    if (!surface && !track && !scene_element) {
        return true;
    }
    doc::Node updated = *existing;
    // A track keeps its lock under its own name, so locking a screen from the
    // Inspector and locking it from the timeline are one state.
    const std::string_view property =
        surface ? output::schema::locked
                : (track ? scene::schema::lane_locked : scene::schema::locked);
    updated.properties.insert_or_assign(std::string{property},
                                        doc::PropertyValue{edit.locked});
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, cmd::PutNode{*existing},
        cmd::UndoContextId{surface ? 4U : 2U}, cmd::TransactionId{edit.gesture},
        apply_at));
}

// Authors one Inspector edit: set a single transform component's base property,
// PutNode with the inverse that restores it. The gesture groups a drag's edits.
[[nodiscard]] bool schedule_property_edit(
    cmd::DirectorScheduler &scheduler, const editor::PropertyEdit &edit,
    cmd::FrameNumber apply_at, EditCoalescingPlan *coalescing = nullptr) {
    const auto name = edit.component == scene::PropertyChannel::FilmGrainAmount
                          ? scene::schema::amount
                          : transform_property(edit.component);
    if (name.empty()) {
        return true;
    }
    const auto *const existing = scheduler.projected_document().get(edit.node);
    if (existing == nullptr) {
        return true;
    }
    doc::Node updated = *existing;
    if (edit.component == scene::PropertyChannel::BoxBlurRadius) {
        if (!std::isfinite(edit.value)) {
            return true;
        }
        const auto rounded = std::round(edit.value);
        const auto radius = static_cast<std::int64_t>(std::clamp(
            rounded, 0.0, static_cast<double>(scene::max_box_blur_radius_px)));
        updated.properties.insert_or_assign(std::string{name},
                                            doc::PropertyValue{radius});
    } else {
        updated.properties.insert_or_assign(std::string{name},
                                            doc::PropertyValue{edit.value});
    }
    auto scheduled = coalescing != nullptr
                         ? schedule_editor_coalesced_edit(
                               scheduler, cmd::PutNode{std::move(updated)},
                               cmd::PutNode{*existing}, *coalescing, apply_at)
                         : scheduler.schedule_edit(
                               cmd::PutNode{std::move(updated)},
                               cmd::PutNode{*existing}, cmd::UndoContextId{2U},
                               cmd::TransactionId{edit.gesture}, apply_at);
    return static_cast<bool>(scheduled);
}

// Writes a clip placement into a media node. One PutNode carries the whole
// ClipTiming because it is one indivisible edit: a move that landed its start
// without its duration would be a clip of a different length for a frame.
void write_clip_timing(doc::Node &node, const scene::ClipTiming &timing) {
    const auto whole = [](std::int64_t value) {
        return doc::PropertyValue{value};
    };
    node.properties.insert_or_assign(
        std::string{scene::schema::clip_start},
        whole(static_cast<std::int64_t>(timing.start)));
    node.properties.insert_or_assign(
        std::string{scene::schema::clip_duration},
        whole(static_cast<std::int64_t>(timing.duration)));
    node.properties.insert_or_assign(
        std::string{scene::schema::source_in},
        whole(static_cast<std::int64_t>(timing.source_in)));
    node.properties.insert_or_assign(
        std::string{scene::schema::fade_in},
        whole(static_cast<std::int64_t>(timing.fade_in)));
    node.properties.insert_or_assign(
        std::string{scene::schema::fade_out},
        whole(static_cast<std::int64_t>(timing.fade_out)));
}

// A placement the scene model will accept: nothing negative, at least one
// frame long, and ramps that fit inside the clip. The editor already clamps
// these, but the authoring boundary is what the document has to trust.
[[nodiscard]] scene::ClipTiming
sane_clip_timing(const scene::ClipTiming &timing) noexcept {
    scene::ClipTiming result{};
    result.start = std::max(timing.start, std::int64_t{0});
    result.duration = std::max(timing.duration, std::int64_t{1});
    result.source_in = std::max(timing.source_in, std::int64_t{0});
    result.fade_in =
        std::clamp(timing.fade_in, std::int64_t{0}, result.duration);
    result.fade_out =
        std::clamp(timing.fade_out, std::int64_t{0}, result.duration);
    return result;
}

[[nodiscard]] bool schedule_clip_timing(cmd::DirectorScheduler &scheduler,
                                        const editor::ClipTimingEdit &edit,
                                        cmd::FrameNumber apply_at) {
    const auto *const existing = scheduler.projected_document().get(edit.clip);
    if (existing == nullptr || existing->type != scene::schema::media_type) {
        // A stale request from a clip that has since been deleted. Harmless.
        return true;
    }
    doc::Node updated = *existing;
    write_clip_timing(updated, sane_clip_timing(edit.timing));
    if (edit.lane.has_value()) {
        // The implicit lane is the absence of the property, not a zero value:
        // the scene model rejects a lane_id of zero as malformed.
        if (*edit.lane == doc::NodeId{}) {
            updated.properties.erase(std::string{scene::schema::lane_id});
        } else {
            updated.properties.insert_or_assign(
                std::string{scene::schema::lane_id},
                doc::PropertyValue{
                    static_cast<std::int64_t>(edit.lane->value())});
        }
    }
    if (updated == *existing) {
        return true;
    }
    auto scheduled = scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, cmd::PutNode{*existing},
        cmd::UndoContextId{2U}, cmd::TransactionId{edit.gesture}, apply_at);
    return static_cast<bool>(scheduled);
}

// Cuts one placement in two at `frame`. The left half keeps the node and loses
// its tail; the right half is a new node with the same source, the same
// transform, and its trim advanced by exactly the frames the left half kept,
// so the two together show what the one clip showed. The tail ramp travels to
// the right half and the head ramp stays with the left, because those are the
// ends that still exist.
[[nodiscard]] bool schedule_clip_split(cmd::DirectorScheduler &scheduler,
                                       const editor::ClipSplitIntent &request,
                                       doc::NodeId id,
                                       cmd::FrameNumber apply_at) {
    const auto *const existing =
        scheduler.projected_document().get(request.clip);
    if (existing == nullptr || existing->type != scene::schema::media_type) {
        return true;
    }
    const auto read = [existing](std::string_view name) -> std::int64_t {
        const auto found = existing->properties.find(name);
        if (found == existing->properties.end()) {
            return 0;
        }
        const auto *const value = std::get_if<std::int64_t>(&found->second);
        return value != nullptr ? *value : 0;
    };
    scene::ClipTiming timing{};
    timing.start = read(scene::schema::clip_start);
    timing.duration = read(scene::schema::clip_duration);
    timing.source_in = read(scene::schema::source_in);
    timing.fade_in = read(scene::schema::fade_in);
    timing.fade_out = read(scene::schema::fade_out);
    const auto cut = static_cast<std::int64_t>(std::min(
        request.frame,
        static_cast<core::u64>(std::numeric_limits<std::int64_t>::max())));
    if (!timing.placed() || cut <= timing.start || cut > timing.last_frame()) {
        // Nothing to cut, or a cut that would produce an empty half. A split
        // is never allowed to become a delete.
        return true;
    }

    const std::int64_t left_length = cut - timing.start;
    scene::ClipTiming left = timing;
    left.duration = left_length;
    left.fade_in = std::min(timing.fade_in, left_length);
    left.fade_out = 0;

    scene::ClipTiming right = timing;
    right.start = cut;
    right.duration = timing.duration - left_length;
    right.source_in = timing.source_in + left_length;
    right.fade_in = 0;
    right.fade_out = std::min(timing.fade_out, right.duration);

    doc::Node head = *existing;
    write_clip_timing(head, left);
    doc::Node tail = *existing;
    tail.id = id;
    write_clip_timing(tail, right);

    const cmd::UndoContextId context{2U};
    const cmd::TransactionId transaction{request.gesture};
    auto shortened = scheduler.schedule_edit(cmd::PutNode{std::move(head)},
                                             cmd::PutNode{*existing}, context,
                                             transaction, apply_at);
    if (!shortened) {
        return false;
    }
    auto added = scheduler.schedule_edit(cmd::PutNode{std::move(tail)},
                                         cmd::EraseNode{id}, context,
                                         transaction, apply_at);
    return static_cast<bool>(added);
}

// Authors the project-wide effect settings. Editing an existing node keeps its
// id so the undo restores exactly what was there; a project that never opened
// the dialog has no node at all, and one is created rather than assumed - the
// same provenance rule the synchronization policy follows.
[[nodiscard]] bool
schedule_project_settings(cmd::DirectorScheduler &scheduler,
                          const editor::ProjectSettingsEdit &edit,
                          doc::NodeId new_id, cmd::FrameNumber apply_at) {
    const scene::EffectSettings settings = scene::clamped(edit.settings);
    doc::Node node{};
    node.type = std::string{scene::schema::settings_type};
    node.properties.emplace(
        std::string{scene::schema::blur_taps},
        doc::PropertyValue{static_cast<std::int64_t>(settings.blur_taps)});
    node.properties.emplace(
        std::string{scene::schema::blur_sigma_ratio},
        doc::PropertyValue{static_cast<double>(settings.blur_sigma_ratio)});
    node.properties.emplace(
        std::string{scene::schema::blur_passes},
        doc::PropertyValue{static_cast<std::int64_t>(settings.blur_passes)});
    node.properties.emplace(std::string{scene::schema::directional_blur_taps},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                settings.directional_blur_taps)});
    node.properties.emplace(
        std::string{scene::schema::brightness_curve},
        doc::PropertyValue{std::string{settings.brightness_curve ==
                                               scene::BrightnessCurve::Gain
                                           ? scene::schema::gain_curve
                                           : scene::schema::exposure_curve}});
    node.properties.emplace(
        std::string{scene::schema::brightness_gamma},
        doc::PropertyValue{static_cast<double>(settings.brightness_gamma)});

    const doc::Node *const existing =
        edit.node == doc::NodeId{}
            ? nullptr
            : scheduler.projected_document().get(edit.node);
    node.id = existing != nullptr ? edit.node : new_id;
    if (existing != nullptr && node == *existing) {
        return true;
    }
    const cmd::Payload inverse = existing != nullptr
                                     ? cmd::Payload{cmd::PutNode{*existing}}
                                     : cmd::Payload{cmd::EraseNode{new_id}};
    auto scheduled = scheduler.schedule_edit(
        cmd::PutNode{std::move(node)}, inverse, cmd::UndoContextId{2U},
        cmd::TransactionId{edit.gesture}, apply_at);
    return static_cast<bool>(scheduled);
}

// A default node for a newly-added element. Objects sit at the origin with a
// neutral material; lights carry the same defaults the demo scene uses. The
// operator refines them through the Inspector afterwards.
// How many show frames a newly-placed clip occupies. A sequence or a video
// gets its own length converted into show frames exactly - a 24 fps clip on a
// 50 fps show is longer in show frames than it is in its own - and a still,
// which has no length of its own, gets the five seconds an editor conventional-
// ly gives one. Rounded up so the last source frame is always reachable.
[[nodiscard]] std::int64_t
placed_clip_duration(const editor::AddIntent &add,
                     show::FrameRate show_rate) noexcept {
    const auto count = static_cast<std::int64_t>(std::max(add.count, 1U));
    const auto show_numerator =
        static_cast<std::int64_t>(std::max(show_rate.numerator, core::u32{1}));
    const auto show_denominator = static_cast<std::int64_t>(
        std::max(show_rate.denominator, core::u32{1}));
    if (count <= 1) {
        constexpr std::int64_t still_seconds = 5;
        return std::max(
            (still_seconds * show_numerator + show_denominator - 1) /
                show_denominator,
            std::int64_t{1});
    }
    if (!add.frame_rate.has_value() || add.frame_rate->numerator == 0U ||
        add.frame_rate->denominator == 0U) {
        return count;
    }
    const auto source_numerator =
        static_cast<std::int64_t>(add.frame_rate->numerator);
    const auto source_denominator =
        static_cast<std::int64_t>(add.frame_rate->denominator);
    const std::int64_t numerator = count * show_numerator * source_denominator;
    const std::int64_t denominator = show_denominator * source_numerator;
    return std::max((numerator + denominator - 1) / denominator,
                    std::int64_t{1});
}

[[nodiscard]] doc::Node make_added_node(doc::NodeId id,
                                        const editor::AddIntent &add,
                                        show::FrameRate show_rate) {
    doc::Node node{};
    node.id = id;
    const auto kind = add.kind;
    const auto real = [](double value) { return doc::PropertyValue{value}; };
    const auto whole = [](std::int64_t value) {
        return doc::PropertyValue{value};
    };
    const auto text = [](std::string_view value) {
        return doc::PropertyValue{std::string{value}};
    };
    const auto add_links = [&]() {
        if (add.scene != scene::SceneId{}) {
            node.properties.emplace(
                std::string{scene::schema::scene_id},
                whole(static_cast<std::int64_t>(add.scene.value())));
        }
        if (add.asset != doc::NodeId{}) {
            node.properties.emplace(
                std::string{scene::schema::asset_id},
                whole(static_cast<std::int64_t>(add.asset.value())));
        }
        if (add.position.has_value()) {
            node.properties.insert_or_assign(
                std::string{scene::schema::position_x},
                real(static_cast<double>(add.position->x)));
            node.properties.insert_or_assign(
                std::string{scene::schema::position_y},
                real(static_cast<double>(add.position->y)));
            node.properties.insert_or_assign(
                std::string{scene::schema::position_z},
                real(static_cast<double>(add.position->z)));
        }
    };
    if (kind == editor::AddKind::Model) {
        node.type = std::string{scene::schema::model_type};
        node.properties.emplace(std::string{scene::schema::source},
                                text(add.source));
        add_links();
        return node;
    }
    if (kind == editor::AddKind::Media) {
        node.type = std::string{scene::schema::media_type};
        node.properties.emplace(std::string{scene::schema::source},
                                text(add.source));
        node.properties.emplace(std::string{scene::schema::sequence_start},
                                whole(0));
        node.properties.emplace(
            std::string{scene::schema::sequence_count},
            whole(static_cast<std::int64_t>(std::max(add.count, 1U))));
        if (add.frame_rate.has_value()) {
            node.properties.emplace(
                std::string{scene::schema::source_rate_numerator},
                whole(static_cast<std::int64_t>(add.frame_rate->numerator)));
            node.properties.emplace(
                std::string{scene::schema::source_rate_denominator},
                whole(static_cast<std::int64_t>(add.frame_rate->denominator)));
        }
        // Two metres tall, and as wide as the source's aspect ratio, so a clip
        // is never stretched.
        const double aspect =
            add.aspect > 0.0F ? static_cast<double>(add.aspect) : (16.0 / 9.0);
        node.properties.emplace(std::string{scene::schema::scale_x},
                                real(2.0 * aspect));
        node.properties.emplace(std::string{scene::schema::scale_y}, real(2.0));
        // A new clip lands placed rather than looping forever, so it appears on
        // the timeline as a bar the operator can move, trim, and cut. Its
        // trim, its ramps, and its start are all zero: an untouched placement
        // of the whole source at the head of the show.
        node.properties.emplace(std::string{scene::schema::clip_start},
                                whole(0));
        node.properties.emplace(std::string{scene::schema::clip_duration},
                                whole(placed_clip_duration(add, show_rate)));
        add_links();
        return node;
    }
    if (kind == editor::AddKind::DirectionalLight ||
        kind == editor::AddKind::PointLight) {
        node.type = std::string{scene::schema::light_type};
        node.properties.emplace(std::string{scene::schema::color_r}, real(1.0));
        node.properties.emplace(std::string{scene::schema::color_g}, real(1.0));
        node.properties.emplace(std::string{scene::schema::color_b}, real(1.0));
        if (kind == editor::AddKind::PointLight) {
            node.properties.emplace(std::string{scene::schema::light_kind},
                                    text("point"));
            node.properties.emplace(std::string{scene::schema::position_y},
                                    real(3.0));
            node.properties.emplace(std::string{scene::schema::range},
                                    real(25.0));
            node.properties.emplace(std::string{scene::schema::intensity},
                                    real(12.0));
        } else {
            node.properties.emplace(std::string{scene::schema::light_kind},
                                    text("directional"));
            node.properties.emplace(std::string{scene::schema::direction_x},
                                    real(-0.3));
            node.properties.emplace(std::string{scene::schema::direction_y},
                                    real(-1.0));
            node.properties.emplace(std::string{scene::schema::direction_z},
                                    real(-0.4));
            node.properties.emplace(std::string{scene::schema::intensity},
                                    real(2.5));
        }
        add_links();
        return node;
    }

    node.type = std::string{scene::schema::object_type};
    const std::string_view primitive =
        kind == editor::AddKind::Plane
            ? "plane"
            : (kind == editor::AddKind::Sphere ? "sphere" : "box");
    node.properties.emplace(std::string{scene::schema::primitive},
                            text(primitive));
    node.properties.emplace(std::string{scene::schema::color_r}, real(0.60));
    node.properties.emplace(std::string{scene::schema::color_g}, real(0.60));
    node.properties.emplace(std::string{scene::schema::color_b}, real(0.63));
    add_links();
    return node;
}

// Creates one element: a PutNode whose inverse erases it, on the scene undo
// context, so an add undoes cleanly and replicates like any other edit.
[[nodiscard]] bool schedule_add(cmd::DirectorScheduler &scheduler,
                                const editor::AddIntent &add, doc::NodeId id,
                                cmd::FrameNumber apply_at,
                                show::FrameRate show_rate) {
    auto scheduled = scheduler.schedule_edit(
        cmd::PutNode{make_added_node(id, add, show_rate)}, cmd::EraseNode{id},
        cmd::UndoContextId{2U}, cmd::TransactionId{add.gesture}, apply_at);
    return static_cast<bool>(scheduled);
}

[[nodiscard]] bool
schedule_asset_import(cmd::DirectorScheduler &scheduler,
                      const editor::AssetImportIntent &request, doc::NodeId id,
                      cmd::FrameNumber apply_at) {
    ProjectAssetNodeUpsert upsert = make_project_asset_upsert(
        scheduler.projected_document(), request.asset, id);
    if (upsert.before.has_value()) {
        return static_cast<bool>(scheduler.schedule_edit(
            cmd::PutNode{std::move(upsert.after)},
            cmd::PutNode{std::move(upsert.before.value())},
            cmd::UndoContextId{4U}, cmd::TransactionId{request.gesture},
            apply_at));
    }
    const doc::NodeId target = upsert.after.id;
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(upsert.after)}, cmd::EraseNode{target},
        cmd::UndoContextId{4U}, cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] std::string_view
project_asset_import_error_label(ProjectAssetImportErrorCode code) noexcept {
    switch (code) {
    case ProjectAssetImportErrorCode::InvalidSource:
        return "SOURCE IS NOT AN ABSOLUTE PATH";
    case ProjectAssetImportErrorCode::SourceMissing:
        return "SOURCE COULD NOT BE OPENED";
    case ProjectAssetImportErrorCode::FingerprintFailed:
        return "SOURCE METADATA COULD NOT BE READ";
    }
    return "UNKNOWN ERROR";
}

struct PendingAssetImportAuthoring final {
    core::u64 gesture{};
    std::vector<editor::FeedSurfaceAddRequest> surface_adds;
    std::vector<editor::FeedSurfaceEditRequest> surface_edits;
};

template <typename Request>
void extract_gesture_requests(std::vector<Request> &pending, core::u64 gesture,
                              std::vector<Request> &extracted) {
    const std::size_t count = static_cast<std::size_t>(std::count_if(
        pending.begin(), pending.end(), [gesture](const Request &request) {
            return request.gesture == gesture;
        }));
    extracted.reserve(extracted.size() + count);
    std::size_t retained{};
    for (std::size_t index = 0U; index < pending.size(); ++index) {
        if (pending[index].gesture == gesture) {
            extracted.push_back(std::move(pending[index]));
        } else {
            if (retained != index) {
                pending[retained] = std::move(pending[index]);
            }
            ++retained;
        }
    }
    pending.resize(retained);
}

template <typename Request>
void discard_gesture_requests(std::vector<Request> &pending,
                              core::u64 gesture) {
    std::erase_if(pending, [gesture](const Request &request) {
        return request.gesture == gesture;
    });
}

[[nodiscard]] bool
schedule_asset_delete(cmd::DirectorScheduler &scheduler,
                      const editor::AssetDeleteIntent &request,
                      cmd::FrameNumber apply_at) {
    const doc::Node *const existing =
        scheduler.projected_document().get(request.asset);
    if (existing == nullptr) {
        return true;
    }
    if (existing->type != assets::schema::media_type &&
        existing->type != assets::schema::model_type &&
        existing->type != assets::schema::projection_mesh_type) {
        // A stale or malformed UI request must never erase a non-asset node.
        return true;
    }
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::EraseNode{request.asset}, cmd::PutNode{*existing},
        // Library removal is a direct destructive UI action, so keep it in
        // the editor's globally reachable Undo context. Undo restores the
        // complete asset node without touching any scene placement.
        cmd::UndoContextId{2U}, cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] bool
schedule_asset_proxy_edit(cmd::DirectorScheduler &scheduler,
                          const editor::AssetProxyEditIntent &request,
                          cmd::FrameNumber apply_at) {
    const doc::Node *const existing =
        scheduler.projected_document().get(request.asset);
    if (existing == nullptr || existing->type != assets::schema::media_type ||
        !assets::valid_proxy_playback_policy(request.policy)) {
        return true;
    }
    doc::Node changed = *existing;
    if (request.policy == assets::ProxyPlaybackPolicy::Source) {
        changed.properties.erase(
            std::string{assets::schema::requested_proxy_profile});
    } else {
        if (!request.requested_profile.has_value() ||
            !assets::valid_requested_proxy_profile(
                *request.requested_profile)) {
            return true;
        }
        const assets::DerivativeProfile *const profile =
            assets::derivative_profile(*request.requested_profile);
        if (profile == nullptr) {
            return true;
        }
        changed.properties.insert_or_assign(
            std::string{assets::schema::requested_proxy_profile},
            doc::PropertyValue{std::string{profile->name}});
    }
    changed.properties.insert_or_assign(
        std::string{assets::schema::proxy_playback_policy},
        doc::PropertyValue{
            std::string{assets::proxy_playback_policy_name(request.policy)}});
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(changed)}, cmd::PutNode{*existing},
        cmd::UndoContextId{2U}, cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] std::string_view
compositor_type(compositor::NodeKind kind) noexcept {
    switch (kind) {
    case compositor::NodeKind::SceneLayer:
        return compositor::schema::scene_layer_type;
    case compositor::NodeKind::Brightness:
        return compositor::schema::brightness_type;
    case compositor::NodeKind::Saturation:
        return compositor::schema::saturation_type;
    case compositor::NodeKind::Levels:
        return compositor::schema::levels_type;
    case compositor::NodeKind::LiftGammaGain:
        return compositor::schema::lift_gamma_gain_type;
    case compositor::NodeKind::ContrastPivot:
        return compositor::schema::contrast_pivot_type;
    case compositor::NodeKind::ColorMatrix:
        return compositor::schema::color_matrix_type;
    case compositor::NodeKind::WhiteBalance:
        return compositor::schema::white_balance_type;
    case compositor::NodeKind::Curves:
        return compositor::schema::curves_type;
    case compositor::NodeKind::FilmGrain:
        return compositor::schema::film_grain_type;
    case compositor::NodeKind::Blur:
        return compositor::schema::blur_type;
    case compositor::NodeKind::BoxBlur:
        return compositor::schema::box_blur_type;
    case compositor::NodeKind::DirectionalBlur:
        return compositor::schema::directional_blur_type;
    case compositor::NodeKind::ChromaKey:
        return compositor::schema::chroma_key_type;
    case compositor::NodeKind::LumaKey:
        return compositor::schema::luma_key_type;
    case compositor::NodeKind::Matte:
        return compositor::schema::matte_type;
    case compositor::NodeKind::RadialArray:
        return compositor::schema::radial_array_type;
    case compositor::NodeKind::LedCarProcess:
        return compositor::schema::led_car_process_type;
    case compositor::NodeKind::Composite:
        return compositor::schema::composite_type;
    case compositor::NodeKind::Output:
        return compositor::schema::output_type;
    }
    return compositor::schema::brightness_type;
}

[[nodiscard]] doc::Node
make_compositor_node(doc::NodeId id, const editor::CompositorAddIntent &request,
                     doc::NodeId scene) {
    doc::Node node{};
    node.id = id;
    node.type = std::string{compositor_type(request.kind)};
    // Graph objects are owned by the scene they were authored in, so a node
    // added while rehearsing composites Preview and leaves Program alone.
    node.properties.emplace(
        std::string{compositor::schema::scene_id},
        doc::PropertyValue{static_cast<std::int64_t>(scene.value())});
    node.properties.emplace(std::string{compositor::schema::label},
                            doc::PropertyValue{std::string{
                                compositor::node_kind_name(request.kind)}});
    node.properties.emplace(
        std::string{compositor::schema::editor_x},
        doc::PropertyValue{static_cast<double>(request.editor_x)});
    node.properties.emplace(
        std::string{compositor::schema::editor_y},
        doc::PropertyValue{static_cast<double>(request.editor_y)});
    switch (request.kind) {
    case compositor::NodeKind::SceneLayer:
        node.properties.emplace(std::string{compositor::schema::scene_node},
                                doc::PropertyValue{static_cast<std::int64_t>(
                                    request.scene_node.value())});
        break;
    case compositor::NodeKind::Brightness:
        node.properties.emplace(std::string{compositor::schema::amount},
                                doc::PropertyValue{1.0});
        break;
    case compositor::NodeKind::Saturation:
        node.properties.emplace(std::string{compositor::schema::amount},
                                doc::PropertyValue{1.0});
        break;
    case compositor::NodeKind::Levels: {
        const auto add_channel =
            [&](std::string_view input_black, std::string_view input_white,
                std::string_view gamma, std::string_view output_black,
                std::string_view output_white) {
                node.properties.emplace(std::string{input_black},
                                        doc::PropertyValue{0.0});
                node.properties.emplace(std::string{input_white},
                                        doc::PropertyValue{1.0});
                node.properties.emplace(std::string{gamma},
                                        doc::PropertyValue{1.0});
                node.properties.emplace(std::string{output_black},
                                        doc::PropertyValue{0.0});
                node.properties.emplace(std::string{output_white},
                                        doc::PropertyValue{1.0});
            };
        add_channel(compositor::schema::levels_master_input_black,
                    compositor::schema::levels_master_input_white,
                    compositor::schema::levels_master_gamma,
                    compositor::schema::levels_master_output_black,
                    compositor::schema::levels_master_output_white);
        add_channel(compositor::schema::levels_red_input_black,
                    compositor::schema::levels_red_input_white,
                    compositor::schema::levels_red_gamma,
                    compositor::schema::levels_red_output_black,
                    compositor::schema::levels_red_output_white);
        add_channel(compositor::schema::levels_green_input_black,
                    compositor::schema::levels_green_input_white,
                    compositor::schema::levels_green_gamma,
                    compositor::schema::levels_green_output_black,
                    compositor::schema::levels_green_output_white);
        add_channel(compositor::schema::levels_blue_input_black,
                    compositor::schema::levels_blue_input_white,
                    compositor::schema::levels_blue_gamma,
                    compositor::schema::levels_blue_output_black,
                    compositor::schema::levels_blue_output_white);
        break;
    }
    case compositor::NodeKind::LiftGammaGain:
        node.properties.emplace(std::string{compositor::schema::lift_r},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::lift_g},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::lift_b},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::gamma_r},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::gamma_g},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::gamma_b},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::gain_r},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::gain_g},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::gain_b},
                                doc::PropertyValue{1.0});
        break;
    case compositor::NodeKind::ContrastPivot:
        node.properties.emplace(std::string{compositor::schema::contrast},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::pivot},
                                doc::PropertyValue{0.5});
        break;
    case compositor::NodeKind::ColorMatrix:
        node.properties.emplace(std::string{compositor::schema::m00},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::m01},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::m02},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::m10},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::m11},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::m12},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::m20},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::m21},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::m22},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::offset_r},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::offset_g},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::offset_b},
                                doc::PropertyValue{0.0});
        break;
    case compositor::NodeKind::WhiteBalance:
        node.properties.emplace(
            std::string{compositor::schema::temperature_kelvin},
            doc::PropertyValue{static_cast<double>(
                scene::d65_white_balance_temperature_kelvin)});
        node.properties.emplace(std::string{compositor::schema::tint},
                                doc::PropertyValue{0.0});
        break;
    case compositor::NodeKind::Curves:
        node.properties.emplace(
            std::string{compositor::schema::curve_master_prefix} + "0" +
                std::string{compositor::schema::curve_x_suffix},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{compositor::schema::curve_master_prefix} + "0" +
                std::string{compositor::schema::curve_y_suffix},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{compositor::schema::curve_master_prefix} + "1" +
                std::string{compositor::schema::curve_x_suffix},
            doc::PropertyValue{1.0});
        node.properties.emplace(
            std::string{compositor::schema::curve_master_prefix} + "1" +
                std::string{compositor::schema::curve_y_suffix},
            doc::PropertyValue{1.0});
        break;
    case compositor::NodeKind::FilmGrain:
        node.properties.emplace(std::string{compositor::schema::amount},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::pixel_scale},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::chroma_ratio},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::seed},
                                doc::PropertyValue{std::int64_t{0}});
        node.properties.emplace(std::string{compositor::schema::mode},
                                doc::PropertyValue{std::string{
                                    compositor::schema::monochrome_mode}});
        break;
    case compositor::NodeKind::Blur:
        node.properties.emplace(std::string{compositor::schema::radius},
                                doc::PropertyValue{8.0});
        break;
    case compositor::NodeKind::BoxBlur:
        node.properties.emplace(
            std::string{compositor::schema::box_blur_radius},
            doc::PropertyValue{std::int64_t{8}});
        node.properties.emplace(
            std::string{compositor::schema::box_blur_passes},
            doc::PropertyValue{std::int64_t{1}});
        break;
    case compositor::NodeKind::DirectionalBlur:
        node.properties.emplace(
            std::string{compositor::schema::directional_blur_length},
            doc::PropertyValue{16.0});
        node.properties.emplace(
            std::string{compositor::schema::directional_blur_angle_turns},
            doc::PropertyValue{0.0});
        break;
    case compositor::NodeKind::ChromaKey:
        node.properties.emplace(
            std::string{compositor::schema::chroma_key_color_r},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{compositor::schema::chroma_key_color_g},
            doc::PropertyValue{1.0});
        node.properties.emplace(
            std::string{compositor::schema::chroma_key_color_b},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{compositor::schema::chroma_key_tolerance},
            doc::PropertyValue{0.1});
        node.properties.emplace(
            std::string{compositor::schema::chroma_key_softness},
            doc::PropertyValue{0.1});
        node.properties.emplace(
            std::string{compositor::schema::chroma_key_morphology_px},
            doc::PropertyValue{std::int64_t{0}});
        node.properties.emplace(
            std::string{compositor::schema::chroma_key_spill_suppression},
            doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::chroma_key_mix},
                                doc::PropertyValue{1.0});
        break;
    case compositor::NodeKind::LumaKey:
        node.properties.emplace(
            std::string{compositor::schema::luma_key_low_threshold},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{compositor::schema::luma_key_high_threshold},
            doc::PropertyValue{1.0});
        node.properties.emplace(
            std::string{compositor::schema::luma_key_low_softness},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{compositor::schema::luma_key_high_softness},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{compositor::schema::luma_key_invert},
            doc::PropertyValue{false});
        node.properties.emplace(std::string{compositor::schema::luma_key_mix},
                                doc::PropertyValue{1.0});
        break;
    case compositor::NodeKind::Matte:
        node.properties.emplace(std::string{compositor::schema::matte_mode},
                                doc::PropertyValue{std::string{
                                    compositor::schema::matte_alpha_mode}});
        node.properties.emplace(std::string{compositor::schema::matte_invert},
                                doc::PropertyValue{false});
        node.properties.emplace(
            std::string{compositor::schema::matte_clamp_input},
            doc::PropertyValue{true});
        break;
    case compositor::NodeKind::RadialArray:
        node.properties.emplace(std::string{compositor::schema::count},
                                doc::PropertyValue{std::int64_t{9}});
        node.properties.emplace(std::string{compositor::schema::radius},
                                doc::PropertyValue{4.0});
        break;
    case compositor::NodeKind::LedCarProcess:
        node.properties.emplace(std::string{compositor::schema::position_x},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::position_y},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::position_z},
                                doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{compositor::schema::rotation_yaw_turns},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{compositor::schema::rotation_pitch_turns},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{compositor::schema::rotation_roll_turns},
            doc::PropertyValue{0.0});
        node.properties.emplace(std::string{compositor::schema::plate_scale},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{compositor::schema::array_scale},
                                doc::PropertyValue{4.0});
        node.properties.emplace(
            std::string{compositor::schema::reflection_height},
            doc::PropertyValue{4.0});
        node.properties.emplace(
            std::string{compositor::schema::reflection_scale},
            doc::PropertyValue{1.0});
        node.properties.emplace(
            std::string{compositor::schema::horizontal_blend},
            doc::PropertyValue{0.05});
        break;
    case compositor::NodeKind::Composite:
    case compositor::NodeKind::Output:
        break;
    }
    return node;
}

[[nodiscard]] bool
schedule_compositor_add(cmd::DirectorScheduler &scheduler,
                        const editor::CompositorAddIntent &request,
                        doc::NodeId id, doc::NodeId scene, core::u64 gesture,
                        cmd::FrameNumber apply_at) {
    if (request.kind == compositor::NodeKind::SceneLayer &&
        request.scene_node == doc::NodeId{}) {
        return true;
    }
    if (scene == doc::NodeId{}) {
        return true;
    }
    doc::Node added = make_compositor_node(id, request, scene);
    if (request.kind == compositor::NodeKind::SceneLayer) {
        const doc::Node *const source =
            scheduler.projected_document().get(request.scene_node);
        if (source != nullptr &&
            source->type == mediaserver::scene::schema::timeline_lane_type) {
            std::string label{"Track"};
            const auto name =
                source->properties.find(mediaserver::scene::schema::lane_name);
            if (name != source->properties.end()) {
                if (const auto *const text =
                        std::get_if<std::string>(&name->second);
                    text != nullptr && !text->empty()) {
                    label += ": ";
                    label += *text;
                }
            }
            added.properties.insert_or_assign(
                std::string{compositor::schema::label},
                doc::PropertyValue{std::move(label)});
        }
    }
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(added)}, cmd::EraseNode{id},
        cmd::UndoContextId{6U}, cmd::TransactionId{gesture}, apply_at));
}

[[nodiscard]] bool
schedule_compositor_move(cmd::DirectorScheduler &scheduler,
                         const editor::CompositorMoveIntent &request,
                         core::u64 gesture, cmd::FrameNumber apply_at) {
    const doc::Node *const existing =
        scheduler.projected_document().get(request.node);
    if (existing == nullptr) {
        return true;
    }
    doc::Node moved = *existing;
    moved.properties.insert_or_assign(
        std::string{compositor::schema::editor_x},
        doc::PropertyValue{static_cast<double>(request.editor_x)});
    moved.properties.insert_or_assign(
        std::string{compositor::schema::editor_y},
        doc::PropertyValue{static_cast<double>(request.editor_y)});
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(moved)}, cmd::PutNode{*existing},
        cmd::UndoContextId{6U}, cmd::TransactionId{gesture}, apply_at));
}

[[nodiscard]] bool schedule_compositor_property(
    cmd::DirectorScheduler &scheduler,
    const editor::CompositorPropertyEditIntent &request, core::u64 gesture,
    cmd::FrameNumber apply_at, EditCoalescingPlan *coalescing = nullptr) {
    const doc::Node *const existing =
        scheduler.projected_document().get(request.node);
    if (existing == nullptr) {
        return true;
    }

    if (request.property == editor::CompositorProperty::CurvesParameters) {
        const auto *const requested =
            std::get_if<compositor::CurvesParameters>(&request.value);
        const auto normalized = requested != nullptr
                                    ? scene::normalized_curves(*requested)
                                    : std::optional<scene::CurvesParameters>{};
        if (!normalized.has_value() ||
            existing->type != compositor::schema::curves_type) {
            return true;
        }
        doc::Node changed = *existing;
        constexpr std::array<std::string_view, 4U> prefixes{{
            compositor::schema::curve_master_prefix,
            compositor::schema::curve_red_prefix,
            compositor::schema::curve_green_prefix,
            compositor::schema::curve_blue_prefix,
        }};
        const auto indexed_property = [](std::string_view prefix,
                                         std::size_t index,
                                         std::string_view suffix) {
            return std::string{prefix} + std::to_string(index) +
                   std::string{suffix};
        };
        for (const std::string_view prefix : prefixes) {
            for (std::size_t index = 0U;
                 index < scene::max_curve_control_points; ++index) {
                changed.properties.erase(indexed_property(
                    prefix, index, compositor::schema::curve_x_suffix));
                changed.properties.erase(indexed_property(
                    prefix, index, compositor::schema::curve_y_suffix));
            }
        }
        const auto write_channel =
            [&](std::string_view prefix,
                const scene::CurveChannelParameters &channel) {
                for (std::size_t index = 0U; index < channel.points.size();
                     ++index) {
                    changed.properties.insert_or_assign(
                        indexed_property(prefix, index,
                                         compositor::schema::curve_x_suffix),
                        doc::PropertyValue{
                            static_cast<double>(channel.points[index].x)});
                    changed.properties.insert_or_assign(
                        indexed_property(prefix, index,
                                         compositor::schema::curve_y_suffix),
                        doc::PropertyValue{
                            static_cast<double>(channel.points[index].y)});
                }
            };
        write_channel(compositor::schema::curve_master_prefix,
                      normalized->master);
        if (normalized->red.has_value()) {
            write_channel(compositor::schema::curve_red_prefix,
                          *normalized->red);
        }
        if (normalized->green.has_value()) {
            write_channel(compositor::schema::curve_green_prefix,
                          *normalized->green);
        }
        if (normalized->blue.has_value()) {
            write_channel(compositor::schema::curve_blue_prefix,
                          *normalized->blue);
        }
        if (coalescing != nullptr) {
            return static_cast<bool>(schedule_editor_coalesced_edit(
                scheduler, cmd::PutNode{std::move(changed)},
                cmd::PutNode{*existing}, *coalescing, apply_at));
        }
        return static_cast<bool>(scheduler.schedule_edit(
            cmd::PutNode{std::move(changed)}, cmd::PutNode{*existing},
            cmd::UndoContextId{6U}, cmd::TransactionId{gesture}, apply_at));
    }

    std::string_view property;
    bool integer_value = false;
    bool film_grain_mode_value = false;
    switch (request.property) {
    case editor::CompositorProperty::BrightnessAmount:
    case editor::CompositorProperty::SaturationAmount:
        property = compositor::schema::amount;
        break;
    case editor::CompositorProperty::LevelsMasterInputBlack:
        property = compositor::schema::levels_master_input_black;
        break;
    case editor::CompositorProperty::LevelsMasterInputWhite:
        property = compositor::schema::levels_master_input_white;
        break;
    case editor::CompositorProperty::LevelsMasterGamma:
        property = compositor::schema::levels_master_gamma;
        break;
    case editor::CompositorProperty::LevelsMasterOutputBlack:
        property = compositor::schema::levels_master_output_black;
        break;
    case editor::CompositorProperty::LevelsMasterOutputWhite:
        property = compositor::schema::levels_master_output_white;
        break;
    case editor::CompositorProperty::LevelsRedInputBlack:
        property = compositor::schema::levels_red_input_black;
        break;
    case editor::CompositorProperty::LevelsRedInputWhite:
        property = compositor::schema::levels_red_input_white;
        break;
    case editor::CompositorProperty::LevelsRedGamma:
        property = compositor::schema::levels_red_gamma;
        break;
    case editor::CompositorProperty::LevelsRedOutputBlack:
        property = compositor::schema::levels_red_output_black;
        break;
    case editor::CompositorProperty::LevelsRedOutputWhite:
        property = compositor::schema::levels_red_output_white;
        break;
    case editor::CompositorProperty::LevelsGreenInputBlack:
        property = compositor::schema::levels_green_input_black;
        break;
    case editor::CompositorProperty::LevelsGreenInputWhite:
        property = compositor::schema::levels_green_input_white;
        break;
    case editor::CompositorProperty::LevelsGreenGamma:
        property = compositor::schema::levels_green_gamma;
        break;
    case editor::CompositorProperty::LevelsGreenOutputBlack:
        property = compositor::schema::levels_green_output_black;
        break;
    case editor::CompositorProperty::LevelsGreenOutputWhite:
        property = compositor::schema::levels_green_output_white;
        break;
    case editor::CompositorProperty::LevelsBlueInputBlack:
        property = compositor::schema::levels_blue_input_black;
        break;
    case editor::CompositorProperty::LevelsBlueInputWhite:
        property = compositor::schema::levels_blue_input_white;
        break;
    case editor::CompositorProperty::LevelsBlueGamma:
        property = compositor::schema::levels_blue_gamma;
        break;
    case editor::CompositorProperty::LevelsBlueOutputBlack:
        property = compositor::schema::levels_blue_output_black;
        break;
    case editor::CompositorProperty::LevelsBlueOutputWhite:
        property = compositor::schema::levels_blue_output_white;
        break;
    case editor::CompositorProperty::LiftGammaGainLiftR:
        property = compositor::schema::lift_r;
        break;
    case editor::CompositorProperty::LiftGammaGainLiftG:
        property = compositor::schema::lift_g;
        break;
    case editor::CompositorProperty::LiftGammaGainLiftB:
        property = compositor::schema::lift_b;
        break;
    case editor::CompositorProperty::LiftGammaGainGammaR:
        property = compositor::schema::gamma_r;
        break;
    case editor::CompositorProperty::LiftGammaGainGammaG:
        property = compositor::schema::gamma_g;
        break;
    case editor::CompositorProperty::LiftGammaGainGammaB:
        property = compositor::schema::gamma_b;
        break;
    case editor::CompositorProperty::LiftGammaGainGainR:
        property = compositor::schema::gain_r;
        break;
    case editor::CompositorProperty::LiftGammaGainGainG:
        property = compositor::schema::gain_g;
        break;
    case editor::CompositorProperty::LiftGammaGainGainB:
        property = compositor::schema::gain_b;
        break;
    case editor::CompositorProperty::ContrastPivotContrast:
        property = compositor::schema::contrast;
        break;
    case editor::CompositorProperty::ContrastPivotPivot:
        property = compositor::schema::pivot;
        break;
    case editor::CompositorProperty::ColorMatrixM00:
        property = compositor::schema::m00;
        break;
    case editor::CompositorProperty::ColorMatrixM01:
        property = compositor::schema::m01;
        break;
    case editor::CompositorProperty::ColorMatrixM02:
        property = compositor::schema::m02;
        break;
    case editor::CompositorProperty::ColorMatrixM10:
        property = compositor::schema::m10;
        break;
    case editor::CompositorProperty::ColorMatrixM11:
        property = compositor::schema::m11;
        break;
    case editor::CompositorProperty::ColorMatrixM12:
        property = compositor::schema::m12;
        break;
    case editor::CompositorProperty::ColorMatrixM20:
        property = compositor::schema::m20;
        break;
    case editor::CompositorProperty::ColorMatrixM21:
        property = compositor::schema::m21;
        break;
    case editor::CompositorProperty::ColorMatrixM22:
        property = compositor::schema::m22;
        break;
    case editor::CompositorProperty::ColorMatrixOffsetR:
        property = compositor::schema::offset_r;
        break;
    case editor::CompositorProperty::ColorMatrixOffsetG:
        property = compositor::schema::offset_g;
        break;
    case editor::CompositorProperty::ColorMatrixOffsetB:
        property = compositor::schema::offset_b;
        break;
    case editor::CompositorProperty::WhiteBalanceTemperatureKelvin:
        property = compositor::schema::temperature_kelvin;
        break;
    case editor::CompositorProperty::WhiteBalanceTint:
        property = compositor::schema::tint;
        break;
    case editor::CompositorProperty::CurvesParameters:
        // Handled as one typed payload above.
        return true;
    case editor::CompositorProperty::FilmGrainAmount:
        property = compositor::schema::amount;
        break;
    case editor::CompositorProperty::FilmGrainPixelScale:
        property = compositor::schema::pixel_scale;
        break;
    case editor::CompositorProperty::FilmGrainChromaRatio:
        property = compositor::schema::chroma_ratio;
        break;
    case editor::CompositorProperty::FilmGrainSeed:
        property = compositor::schema::seed;
        integer_value = true;
        break;
    case editor::CompositorProperty::FilmGrainMode:
        property = compositor::schema::mode;
        film_grain_mode_value = true;
        break;
    case editor::CompositorProperty::BlurRadius:
    case editor::CompositorProperty::RadialRadius:
        property = compositor::schema::radius;
        break;
    case editor::CompositorProperty::BoxBlurRadius:
        property = compositor::schema::box_blur_radius;
        integer_value = true;
        break;
    case editor::CompositorProperty::BoxBlurPasses:
        property = compositor::schema::box_blur_passes;
        integer_value = true;
        break;
    case editor::CompositorProperty::DirectionalBlurLength:
        property = compositor::schema::directional_blur_length;
        break;
    case editor::CompositorProperty::DirectionalBlurAngleTurns:
        property = compositor::schema::directional_blur_angle_turns;
        break;
    case editor::CompositorProperty::RadialCount:
        property = compositor::schema::count;
        integer_value = true;
        break;
    case editor::CompositorProperty::RadialStartAngleTurns:
        property = compositor::schema::start_angle_turns;
        break;
    case editor::CompositorProperty::RadialCenterX:
        property = compositor::schema::center_x;
        break;
    case editor::CompositorProperty::RadialCenterY:
        property = compositor::schema::center_y;
        break;
    case editor::CompositorProperty::LedCarPositionX:
        property = compositor::schema::position_x;
        break;
    case editor::CompositorProperty::LedCarPositionY:
        property = compositor::schema::position_y;
        break;
    case editor::CompositorProperty::LedCarPositionZ:
        property = compositor::schema::position_z;
        break;
    case editor::CompositorProperty::LedCarYawTurns:
        property = compositor::schema::rotation_yaw_turns;
        break;
    case editor::CompositorProperty::LedCarPitchTurns:
        property = compositor::schema::rotation_pitch_turns;
        break;
    case editor::CompositorProperty::LedCarRollTurns:
        property = compositor::schema::rotation_roll_turns;
        break;
    case editor::CompositorProperty::LedCarPlateScale:
        property = compositor::schema::plate_scale;
        break;
    case editor::CompositorProperty::LedCarArrayScale:
        property = compositor::schema::array_scale;
        break;
    case editor::CompositorProperty::LedCarReflectionHeight:
        property = compositor::schema::reflection_height;
        break;
    case editor::CompositorProperty::LedCarReflectionScale:
        property = compositor::schema::reflection_scale;
        break;
    case editor::CompositorProperty::LedCarHorizontalBlend:
        property = compositor::schema::horizontal_blend;
        break;
    }

    doc::PropertyValue value;
    if (film_grain_mode_value) {
        const auto *const number = std::get_if<core::u32>(&request.value);
        if (number == nullptr || *number > 1U) {
            return true;
        }
        value = std::string{*number == 0U ? compositor::schema::monochrome_mode
                                          : compositor::schema::rgb_mode};
    } else if (integer_value) {
        const auto *const number = std::get_if<core::u32>(&request.value);
        if (number == nullptr) {
            return true;
        }
        value = static_cast<std::int64_t>(*number);
    } else {
        const auto *const number = std::get_if<float>(&request.value);
        if (number == nullptr || !std::isfinite(*number)) {
            return true;
        }
        value = static_cast<double>(*number);
    }

    doc::Node changed = *existing;
    changed.properties.insert_or_assign(std::string{property},
                                        std::move(value));
    if (coalescing != nullptr) {
        return static_cast<bool>(schedule_editor_coalesced_edit(
            scheduler, cmd::PutNode{std::move(changed)},
            cmd::PutNode{*existing}, *coalescing, apply_at));
    }
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(changed)}, cmd::PutNode{*existing},
        cmd::UndoContextId{6U}, cmd::TransactionId{gesture}, apply_at));
}

[[nodiscard]] bool
schedule_compositor_connect(cmd::DirectorScheduler &scheduler,
                            const editor::CompositorConnectIntent &request,
                            doc::NodeId id, core::u64 gesture,
                            cmd::FrameNumber apply_at) {
    constexpr core::u64 max_reference =
        static_cast<core::u64>(std::numeric_limits<std::int64_t>::max());
    if (request.from.node.value() > max_reference ||
        request.to.node.value() > max_reference) {
        return true;
    }
    const cmd::UndoContextId context{6U};
    const cmd::TransactionId transaction{gesture};
    if (request.replace_edge.has_value()) {
        const doc::Node *const replaced =
            scheduler.projected_document().get(*request.replace_edge);
        if (replaced != nullptr) {
            const auto target =
                replaced->properties.find(compositor::schema::to_node);
            const auto port =
                replaced->properties.find(compositor::schema::to_port);
            const auto *const target_value =
                target == replaced->properties.end()
                    ? nullptr
                    : std::get_if<std::int64_t>(&target->second);
            const auto *const port_value =
                port == replaced->properties.end()
                    ? nullptr
                    : std::get_if<std::int64_t>(&port->second);
            const bool matches_target =
                replaced->type == compositor::schema::edge_type &&
                target_value != nullptr && *target_value > 0 &&
                static_cast<core::u64>(*target_value) ==
                    request.to.node.value() &&
                port_value != nullptr && *port_value >= 0 &&
                static_cast<core::u64>(*port_value) == request.to.port;
            if (!matches_target) {
                return true;
            }
            if (!scheduler.schedule_edit(cmd::EraseNode{replaced->id},
                                         cmd::PutNode{*replaced}, context,
                                         transaction, apply_at)) {
                return false;
            }
        }
    }
    doc::Node edge{};
    edge.id = id;
    edge.type = std::string{compositor::schema::edge_type};
    edge.properties.emplace(std::string{compositor::schema::from_node},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                request.from.node.value())});
    edge.properties.emplace(
        std::string{compositor::schema::from_port},
        doc::PropertyValue{static_cast<std::int64_t>(request.from.port)});
    edge.properties.emplace(
        std::string{compositor::schema::to_node},
        doc::PropertyValue{static_cast<std::int64_t>(request.to.node.value())});
    edge.properties.emplace(
        std::string{compositor::schema::to_port},
        doc::PropertyValue{static_cast<std::int64_t>(request.to.port)});
    edge.properties.emplace(std::string{compositor::schema::order},
                            doc::PropertyValue{std::int64_t{0}});
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(edge)}, cmd::EraseNode{id}, context, transaction,
        apply_at));
}

[[nodiscard]] std::optional<doc::NodeId>
node_reference(const doc::Node &node, std::string_view property) noexcept {
    const auto found = node.properties.find(property);
    if (found == node.properties.end()) {
        return std::nullopt;
    }
    const auto *const value = std::get_if<std::int64_t>(&found->second);
    if (value == nullptr || *value <= 0) {
        return std::nullopt;
    }
    return doc::NodeId{static_cast<core::u64>(*value)};
}

[[nodiscard]] bool schedule_erase_node(cmd::DirectorScheduler &scheduler,
                                       doc::NodeId id,
                                       cmd::UndoContextId context,
                                       cmd::TransactionId transaction,
                                       cmd::FrameNumber apply_at) {
    const doc::Node *const existing = scheduler.projected_document().get(id);
    if (existing == nullptr) {
        return true;
    }
    return static_cast<bool>(
        scheduler.schedule_edit(cmd::EraseNode{id}, cmd::PutNode{*existing},
                                context, transaction, apply_at));
}

[[nodiscard]] bool
schedule_compositor_delete(cmd::DirectorScheduler &scheduler,
                           const editor::CompositorDeleteIntent &request,
                           core::u64 gesture, cmd::FrameNumber apply_at) {
    if (request.target.kind == editor::CompositorSelectionKind::None ||
        request.target.node == doc::NodeId{}) {
        return true;
    }
    const doc::Root &root = scheduler.projected_document();
    const doc::Node *const target = root.get(request.target.node);
    if (target == nullptr) {
        return true;
    }
    const doc::NodeId target_id = target->id;
    const std::string target_type = target->type;
    const cmd::UndoContextId context{6U};
    const cmd::TransactionId transaction{gesture};
    if (request.target.kind == editor::CompositorSelectionKind::Edge) {
        return target_type != compositor::schema::edge_type ||
               schedule_erase_node(scheduler, target_id, context, transaction,
                                   apply_at);
    }
    // Output is structural: every newly-authored composition receives one and
    // the editor presents it as required rather than as a disposable effect.
    if (target_type == compositor::schema::output_type) {
        return true;
    }
    if (!compositor::schema::is_node_type(target_type)) {
        return true;
    }

    try {
        std::vector<doc::NodeId> incident;
        const doc::Delta contents = doc::diff(doc::Root{}, root);
        for (const doc::NodeChange &change : contents.changes) {
            if (!change.after ||
                change.after->type != compositor::schema::edge_type) {
                continue;
            }
            const auto from =
                node_reference(*change.after, compositor::schema::from_node);
            const auto to =
                node_reference(*change.after, compositor::schema::to_node);
            if ((from && *from == target_id) || (to && *to == target_id)) {
                incident.push_back(change.after->id);
            }
        }
        for (const doc::NodeId edge : incident) {
            if (!schedule_erase_node(scheduler, edge, context, transaction,
                                     apply_at)) {
                return false;
            }
        }
        return schedule_erase_node(scheduler, target_id, context, transaction,
                                   apply_at);
    } catch (const std::bad_alloc &) {
        return false;
    }
}

[[nodiscard]] doc::Node make_effect_node(doc::NodeId id,
                                         const editor::EffectIntent &effect) {
    doc::Node node{};
    node.id = id;
    node.type = std::string{scene::schema::effect_type};
    node.properties.emplace(
        std::string{scene::schema::effect_target},
        doc::PropertyValue{static_cast<std::int64_t>(effect.target.value())});
    std::string_view kind = scene::schema::brightness_kind;
    switch (effect.kind) {
    case scene::EffectKind::Brightness:
        kind = scene::schema::brightness_kind;
        break;
    case scene::EffectKind::Saturation:
        kind = scene::schema::saturation_kind;
        break;
    case scene::EffectKind::Blur:
        kind = scene::schema::blur_kind;
        break;
    case scene::EffectKind::BoxBlur:
        kind = scene::schema::box_blur_kind;
        break;
    case scene::EffectKind::DirectionalBlur:
        kind = scene::schema::directional_blur_kind;
        break;
    case scene::EffectKind::ChromaKey:
        kind = scene::schema::chroma_key_kind;
        break;
    case scene::EffectKind::LumaKey:
        kind = scene::schema::luma_key_kind;
        break;
    case scene::EffectKind::Levels:
        kind = scene::schema::levels_kind;
        break;
    case scene::EffectKind::LiftGammaGain:
        kind = scene::schema::lift_gamma_gain_kind;
        break;
    case scene::EffectKind::ContrastPivot:
        kind = scene::schema::contrast_pivot_kind;
        break;
    case scene::EffectKind::ColorMatrix:
        kind = scene::schema::color_matrix_kind;
        break;
    case scene::EffectKind::WhiteBalance:
        kind = scene::schema::white_balance_kind;
        break;
    case scene::EffectKind::Curves:
        kind = scene::schema::curves_kind;
        break;
    case scene::EffectKind::FilmGrain:
        kind = scene::schema::film_grain_kind;
        break;
    case scene::EffectKind::Count:
        break;
    }
    node.properties.emplace(std::string{scene::schema::effect_kind},
                            doc::PropertyValue{std::string{kind}});
    node.properties.emplace(std::string{scene::schema::effect_order},
                            doc::PropertyValue{std::int64_t{0}});
    node.properties.emplace(std::string{scene::schema::effect_enabled},
                            doc::PropertyValue{true});
    if (effect.kind == scene::EffectKind::Brightness) {
        node.properties.emplace(std::string{scene::schema::brightness},
                                doc::PropertyValue{1.0});
    } else if (effect.kind == scene::EffectKind::Saturation) {
        node.properties.emplace(std::string{scene::schema::saturation},
                                doc::PropertyValue{1.0});
    } else if (effect.kind == scene::EffectKind::Blur) {
        node.properties.emplace(std::string{scene::schema::blur_radius},
                                doc::PropertyValue{8.0});
    } else if (effect.kind == scene::EffectKind::BoxBlur) {
        node.properties.emplace(std::string{scene::schema::box_blur_radius},
                                doc::PropertyValue{std::int64_t{8}});
        node.properties.emplace(std::string{scene::schema::box_blur_passes},
                                doc::PropertyValue{std::int64_t{1}});
    } else if (effect.kind == scene::EffectKind::DirectionalBlur) {
        node.properties.emplace(
            std::string{scene::schema::directional_blur_length},
            doc::PropertyValue{16.0});
        node.properties.emplace(
            std::string{scene::schema::directional_blur_angle_turns},
            doc::PropertyValue{0.0});
    } else if (effect.kind == scene::EffectKind::ChromaKey) {
        node.properties.emplace(std::string{scene::schema::chroma_key_color_r},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::chroma_key_color_g},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{scene::schema::chroma_key_color_b},
                                doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{scene::schema::chroma_key_tolerance},
            doc::PropertyValue{0.1});
        node.properties.emplace(std::string{scene::schema::chroma_key_softness},
                                doc::PropertyValue{0.1});
        node.properties.emplace(
            std::string{scene::schema::chroma_key_morphology_px},
            doc::PropertyValue{std::int64_t{0}});
        node.properties.emplace(
            std::string{scene::schema::chroma_key_spill_suppression},
            doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::chroma_key_mix},
                                doc::PropertyValue{1.0});
    } else if (effect.kind == scene::EffectKind::LumaKey) {
        node.properties.emplace(
            std::string{scene::schema::luma_key_low_threshold},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{scene::schema::luma_key_high_threshold},
            doc::PropertyValue{1.0});
        node.properties.emplace(
            std::string{scene::schema::luma_key_low_softness},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{scene::schema::luma_key_high_softness},
            doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::luma_key_invert},
                                doc::PropertyValue{false});
        node.properties.emplace(std::string{scene::schema::luma_key_mix},
                                doc::PropertyValue{1.0});
    } else if (effect.kind == scene::EffectKind::Levels) {
        const auto add_channel =
            [&](std::string_view input_black, std::string_view input_white,
                std::string_view gamma, std::string_view output_black,
                std::string_view output_white) {
                node.properties.emplace(std::string{input_black},
                                        doc::PropertyValue{0.0});
                node.properties.emplace(std::string{input_white},
                                        doc::PropertyValue{1.0});
                node.properties.emplace(std::string{gamma},
                                        doc::PropertyValue{1.0});
                node.properties.emplace(std::string{output_black},
                                        doc::PropertyValue{0.0});
                node.properties.emplace(std::string{output_white},
                                        doc::PropertyValue{1.0});
            };
        add_channel(scene::schema::levels_master_input_black,
                    scene::schema::levels_master_input_white,
                    scene::schema::levels_master_gamma,
                    scene::schema::levels_master_output_black,
                    scene::schema::levels_master_output_white);
        add_channel(scene::schema::levels_red_input_black,
                    scene::schema::levels_red_input_white,
                    scene::schema::levels_red_gamma,
                    scene::schema::levels_red_output_black,
                    scene::schema::levels_red_output_white);
        add_channel(scene::schema::levels_green_input_black,
                    scene::schema::levels_green_input_white,
                    scene::schema::levels_green_gamma,
                    scene::schema::levels_green_output_black,
                    scene::schema::levels_green_output_white);
        add_channel(scene::schema::levels_blue_input_black,
                    scene::schema::levels_blue_input_white,
                    scene::schema::levels_blue_gamma,
                    scene::schema::levels_blue_output_black,
                    scene::schema::levels_blue_output_white);
    } else if (effect.kind == scene::EffectKind::LiftGammaGain) {
        node.properties.emplace(std::string{scene::schema::lift_r},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::lift_g},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::lift_b},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::gamma_r},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{scene::schema::gamma_g},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{scene::schema::gamma_b},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{scene::schema::gain_r},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{scene::schema::gain_g},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{scene::schema::gain_b},
                                doc::PropertyValue{1.0});
    } else if (effect.kind == scene::EffectKind::ContrastPivot) {
        node.properties.emplace(std::string{scene::schema::contrast},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{scene::schema::pivot},
                                doc::PropertyValue{0.5});
    } else if (effect.kind == scene::EffectKind::ColorMatrix) {
        node.properties.emplace(std::string{scene::schema::m00},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{scene::schema::m01},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::m02},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::m10},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::m11},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{scene::schema::m12},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::m20},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::m21},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::m22},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{scene::schema::offset_r},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::offset_g},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::offset_b},
                                doc::PropertyValue{0.0});
    } else if (effect.kind == scene::EffectKind::WhiteBalance) {
        node.properties.emplace(
            std::string{scene::schema::temperature_kelvin},
            doc::PropertyValue{static_cast<double>(
                scene::d65_white_balance_temperature_kelvin)});
        node.properties.emplace(std::string{scene::schema::tint},
                                doc::PropertyValue{0.0});
    } else if (effect.kind == scene::EffectKind::Curves) {
        node.properties.emplace(
            std::string{scene::schema::curve_master_prefix} + "0" +
                std::string{scene::schema::curve_x_suffix},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{scene::schema::curve_master_prefix} + "0" +
                std::string{scene::schema::curve_y_suffix},
            doc::PropertyValue{0.0});
        node.properties.emplace(
            std::string{scene::schema::curve_master_prefix} + "1" +
                std::string{scene::schema::curve_x_suffix},
            doc::PropertyValue{1.0});
        node.properties.emplace(
            std::string{scene::schema::curve_master_prefix} + "1" +
                std::string{scene::schema::curve_y_suffix},
            doc::PropertyValue{1.0});
    } else if (effect.kind == scene::EffectKind::FilmGrain) {
        node.properties.emplace(std::string{scene::schema::amount},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::pixel_scale},
                                doc::PropertyValue{1.0});
        node.properties.emplace(std::string{scene::schema::chroma_ratio},
                                doc::PropertyValue{0.0});
        node.properties.emplace(std::string{scene::schema::seed},
                                doc::PropertyValue{std::int64_t{0}});
        node.properties.emplace(
            std::string{scene::schema::mode},
            doc::PropertyValue{std::string{scene::schema::monochrome_mode}});
    }
    return node;
}

[[nodiscard]] bool schedule_effect(cmd::DirectorScheduler &scheduler,
                                   const editor::EffectIntent &effect,
                                   doc::NodeId id, cmd::FrameNumber apply_at) {
    if (effect.kind == scene::EffectKind::Count ||
        !scheduler.projected_document().contains(effect.target) ||
        effect.target.value() >
            static_cast<core::u64>(std::numeric_limits<std::int64_t>::max())) {
        return true;
    }
    auto scheduled = scheduler.schedule_edit(
        cmd::PutNode{make_effect_node(id, effect)}, cmd::EraseNode{id},
        cmd::UndoContextId{2U}, cmd::TransactionId{effect.gesture}, apply_at);
    return static_cast<bool>(scheduled);
}

[[nodiscard]] bool schedule_film_grain_effect_edit(
    cmd::DirectorScheduler &scheduler,
    const editor::FilmGrainEffectEditIntent &request, cmd::FrameNumber apply_at,
    EditCoalescingPlan *coalescing = nullptr) {
    const doc::Node *const existing =
        scheduler.projected_document().get(request.effect);
    const auto normalized = scene::normalized_film_grain(request.parameters);
    if (existing == nullptr || existing->type != scene::schema::effect_type ||
        !normalized.has_value()) {
        return true;
    }
    const auto kind = existing->properties.find(scene::schema::effect_kind);
    if (kind == existing->properties.end() ||
        std::get_if<std::string>(&kind->second) == nullptr ||
        *std::get_if<std::string>(&kind->second) !=
            scene::schema::film_grain_kind) {
        return true;
    }

    doc::Node changed = *existing;
    changed.properties.insert_or_assign(
        std::string{scene::schema::amount},
        doc::PropertyValue{static_cast<double>(normalized->amount)});
    changed.properties.insert_or_assign(
        std::string{scene::schema::pixel_scale},
        doc::PropertyValue{static_cast<double>(normalized->pixel_scale)});
    changed.properties.insert_or_assign(
        std::string{scene::schema::chroma_ratio},
        doc::PropertyValue{static_cast<double>(normalized->chroma_ratio)});
    changed.properties.insert_or_assign(
        std::string{scene::schema::seed},
        doc::PropertyValue{static_cast<std::int64_t>(normalized->seed)});
    changed.properties.insert_or_assign(
        std::string{scene::schema::mode},
        doc::PropertyValue{
            std::string{normalized->mode == scene::FilmGrainMode::RGB
                            ? scene::schema::rgb_mode
                            : scene::schema::monochrome_mode}});

    if (coalescing != nullptr) {
        return static_cast<bool>(schedule_editor_coalesced_edit(
            scheduler, cmd::PutNode{std::move(changed)},
            cmd::PutNode{*existing}, *coalescing, apply_at));
    }
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(changed)}, cmd::PutNode{*existing},
        cmd::UndoContextId{2U}, cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] bool
schedule_effect_delete(cmd::DirectorScheduler &scheduler,
                       const editor::EffectDeleteIntent &request,
                       cmd::FrameNumber apply_at) {
    const auto planned = scene::plan_delete_effect(
        scheduler.projected_document(), request.effect);
    if (!planned) {
        // A stale UI request is harmless. Allocation failure is different: do
        // not partially author a cascade whose complete dependency list could
        // not be produced.
        return planned.error() !=
               scene::EffectDeletePlanError::AllocationFailed;
    }

    const cmd::UndoContextId context{2U};
    const cmd::TransactionId transaction{request.gesture};
    for (const doc::NodeId node : planned.value().erases) {
        if (!schedule_erase_node(scheduler, node, context, transaction,
                                 apply_at)) {
            return false;
        }
    }
    return true;
}

// Deletes one element: an EraseNode whose inverse restores it. A missing node
// is a no-op. All of a multi-select delete share the gesture, so it undoes at
// once.
[[nodiscard]] bool schedule_delete(cmd::DirectorScheduler &scheduler,
                                   doc::NodeId node, core::u64 gesture,
                                   cmd::FrameNumber apply_at) {
    const auto *const existing = scheduler.projected_document().get(node);
    if (existing == nullptr) {
        return true;
    }
    auto scheduled = scheduler.schedule_edit(
        cmd::EraseNode{node}, cmd::PutNode{*existing}, cmd::UndoContextId{2U},
        cmd::TransactionId{gesture}, apply_at);
    return static_cast<bool>(scheduled);
}

// Authors one operator move as an ordinary command: read the node's current
// authored state, set its position, and schedule a PutNode whose inverse
// restores the old one. A separate undo context from the transport's, so
// undoing a move never touches playback. The editor produced the position; this
// is the only place a document changes.
[[nodiscard]] bool schedule_scene_edit(cmd::DirectorScheduler &scheduler,
                                       const editor::EditIntent &edit,
                                       cmd::FrameNumber apply_at) {
    const auto *const existing = scheduler.projected_document().get(edit.node);
    if (existing == nullptr) {
        // The node is gone; there is nothing to move and nothing to fail.
        return true;
    }
    doc::Node updated = *existing;
    updated.properties.insert_or_assign(
        std::string{scene::schema::position_x},
        doc::PropertyValue{static_cast<double>(edit.position.x)});
    updated.properties.insert_or_assign(
        std::string{scene::schema::position_y},
        doc::PropertyValue{static_cast<double>(edit.position.y)});
    updated.properties.insert_or_assign(
        std::string{scene::schema::position_z},
        doc::PropertyValue{static_cast<double>(edit.position.z)});

    // The transaction is the drag gesture, not the frame: every sealed frame of
    // one continuous drag appends to the same transaction, so the clip moves on
    // the nodes each frame but the whole drag undoes as a single step. A fresh
    // gesture (the next drag) starts a new transaction and becomes the undo
    // top.
    auto scheduled = scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, cmd::PutNode{*existing},
        cmd::UndoContextId{2U}, cmd::TransactionId{edit.gesture}, apply_at);
    return static_cast<bool>(scheduled);
}

[[nodiscard]] std::string_view
hardware_kind_property(topology::HardwareDeviceKind kind) noexcept {
    switch (kind) {
    case topology::HardwareDeviceKind::RenderServer:
        return topology::schema::render_server_kind;
    case topology::HardwareDeviceKind::OperatorMachine:
        return topology::schema::operator_machine_kind;
    case topology::HardwareDeviceKind::LedProcessor:
        return topology::schema::led_processor_kind;
    case topology::HardwareDeviceKind::LedWall:
        return topology::schema::led_wall_kind;
    }
    return topology::schema::render_server_kind;
}

[[nodiscard]] doc::Node
make_hardware_device(doc::NodeId id,
                     const editor::HardwarePlaceRequest &request) {
    doc::Node node{};
    node.id = id;
    node.type = std::string{topology::schema::device_type};
    node.properties.emplace(
        std::string{topology::schema::kind},
        doc::PropertyValue{std::string{hardware_kind_property(request.kind)}});
    node.properties.emplace(std::string{topology::schema::name},
                            doc::PropertyValue{std::string{
                                topology::device_kind_name(request.kind)}});
    node.properties.emplace(
        std::string{topology::schema::grid_x},
        doc::PropertyValue{static_cast<std::int64_t>(request.grid_x)});
    node.properties.emplace(
        std::string{topology::schema::grid_y},
        doc::PropertyValue{static_cast<std::int64_t>(request.grid_y)});
    node.properties.emplace(std::string{topology::schema::address},
                            doc::PropertyValue{std::string{}});
    if (request.kind == topology::HardwareDeviceKind::RenderServer) {
        node.properties.emplace(
            std::string{topology::schema::lifecycle_endpoint},
            doc::PropertyValue{std::string{}});
        node.properties.emplace(std::string{topology::schema::node_index},
                                doc::PropertyValue{std::int64_t{0}});
    }
    node.properties.emplace(std::string{topology::schema::width},
                            doc::PropertyValue{std::int64_t{1920}});
    node.properties.emplace(std::string{topology::schema::height},
                            doc::PropertyValue{std::int64_t{1080}});
    node.properties.emplace(std::string{topology::schema::refresh_numerator},
                            doc::PropertyValue{std::int64_t{60}});
    node.properties.emplace(std::string{topology::schema::refresh_denominator},
                            doc::PropertyValue{std::int64_t{1}});
    if (request.kind == topology::HardwareDeviceKind::RenderServer) {
        node.properties.emplace(std::string{topology::schema::window_mode},
                                doc::PropertyValue{std::string{
                                    topology::schema::inherit_window_mode}});
        node.properties.emplace(std::string{topology::schema::desktop_x},
                                doc::PropertyValue{std::int64_t{0}});
        node.properties.emplace(std::string{topology::schema::desktop_y},
                                doc::PropertyValue{std::int64_t{0}});
    }
    return node;
}

[[nodiscard]] bool
schedule_hardware_place(cmd::DirectorScheduler &scheduler,
                        const editor::HardwarePlaceRequest &request,
                        doc::NodeId id, cmd::FrameNumber apply_at) {
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{make_hardware_device(id, request)}, cmd::EraseNode{id},
        cmd::UndoContextId{3U}, cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] bool
schedule_hardware_move(cmd::DirectorScheduler &scheduler,
                       const editor::HardwareMoveRequest &request,
                       cmd::FrameNumber apply_at) {
    const doc::Node *const existing =
        scheduler.projected_document().get(request.device);
    if (existing == nullptr ||
        existing->type != topology::schema::device_type) {
        return true;
    }
    doc::Node updated = *existing;
    updated.properties.insert_or_assign(
        std::string{topology::schema::grid_x},
        doc::PropertyValue{static_cast<std::int64_t>(request.grid_x)});
    updated.properties.insert_or_assign(
        std::string{topology::schema::grid_y},
        doc::PropertyValue{static_cast<std::int64_t>(request.grid_y)});
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, cmd::PutNode{*existing},
        cmd::UndoContextId{3U}, cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] bool
schedule_hardware_link(cmd::DirectorScheduler &scheduler,
                       const editor::HardwareLinkRequest &request,
                       doc::NodeId id, cmd::FrameNumber apply_at) {
    constexpr core::u64 max_document_node =
        static_cast<core::u64>(std::numeric_limits<std::int64_t>::max());
    if (request.from.device.value() > max_document_node ||
        request.to.device.value() > max_document_node) {
        return true;
    }
    if (request.output_binding.has_value() &&
        (request.output_binding->render_server != request.from.device ||
         request.output_binding->logical_port != request.from.port ||
         request.output_region.width == 0U ||
         request.output_region.height == 0U)) {
        return true;
    }
    doc::Node node{};
    node.id = id;
    node.type = std::string{topology::schema::link_type};
    node.properties.emplace(std::string{topology::schema::from_device},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                request.from.device.value())});
    node.properties.emplace(
        std::string{topology::schema::from_port},
        doc::PropertyValue{static_cast<std::int64_t>(request.from.port)});
    node.properties.emplace(std::string{topology::schema::to_device},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                request.to.device.value())});
    node.properties.emplace(
        std::string{topology::schema::to_port},
        doc::PropertyValue{static_cast<std::int64_t>(request.to.port)});
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(node)}, cmd::EraseNode{id},
        cmd::UndoContextId{3U}, cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] bool
schedule_hardware_property(cmd::DirectorScheduler &scheduler,
                           const editor::HardwarePropertyEdit &edit,
                           cmd::FrameNumber apply_at) {
    const bool allowed =
        edit.property == topology::schema::name ||
        edit.property == topology::schema::address ||
        edit.property == topology::schema::lifecycle_endpoint ||
        edit.property == topology::schema::node_index ||
        edit.property == topology::schema::width ||
        edit.property == topology::schema::height ||
        edit.property == topology::schema::refresh_numerator ||
        edit.property == topology::schema::refresh_denominator ||
        edit.property == topology::schema::window_mode ||
        edit.property == topology::schema::desktop_x ||
        edit.property == topology::schema::desktop_y;
    const doc::Node *const existing =
        scheduler.projected_document().get(edit.device);
    if (!allowed || existing == nullptr ||
        existing->type != topology::schema::device_type) {
        return true;
    }
    const bool lifecycle_property =
        edit.property == topology::schema::lifecycle_endpoint ||
        edit.property == topology::schema::node_index ||
        edit.property == topology::schema::window_mode ||
        edit.property == topology::schema::desktop_x ||
        edit.property == topology::schema::desktop_y;
    const auto kind =
        existing->properties.find(std::string{topology::schema::kind});
    const auto *const kind_text = kind != existing->properties.end()
                                      ? std::get_if<std::string>(&kind->second)
                                      : nullptr;
    if (lifecycle_property &&
        (kind_text == nullptr ||
         *kind_text != topology::schema::render_server_kind)) {
        return true;
    }
    doc::Node updated = *existing;
    updated.properties.insert_or_assign(edit.property, edit.value);
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, cmd::PutNode{*existing},
        cmd::UndoContextId{3U}, cmd::TransactionId{edit.gesture}, apply_at));
}

[[nodiscard]] bool
schedule_hardware_control_port(cmd::DirectorScheduler &scheduler,
                               const editor::HardwareControlPortEdit &edit,
                               doc::NodeId allocated_id,
                               cmd::FrameNumber apply_at) {
    if (edit.base_port == 0U ||
        edit.base_port > topology::max_hardware_control_base_port) {
        return true;
    }

    const doc::Root &root = scheduler.projected_document();
    const doc::Node *existing{};
    if (edit.settings != doc::NodeId{}) {
        existing = root.get(edit.settings);
        // A stale request from a replaced project must not create a second
        // settings object or edit a node whose identity was reused.
        if (existing == nullptr ||
            existing->type != topology::schema::settings_type) {
            return true;
        }
    } else {
        // The panel asks to create when its evaluated map has no provenance.
        // Re-check the authoritative projected document at the authoring
        // boundary and update the lowest-id settings node if one arrived in
        // the meantime, preserving the model's deterministic winner.
        const doc::Delta contents = doc::diff(doc::Root{}, root);
        for (const doc::NodeChange &change : contents.changes) {
            if (!change.after ||
                change.after->type != topology::schema::settings_type) {
                continue;
            }
            if (existing == nullptr || change.after->id < existing->id) {
                existing = change.after.get();
            }
        }
    }

    doc::Node updated{};
    updated.id = existing != nullptr ? existing->id : allocated_id;
    updated.type = std::string{topology::schema::settings_type};
    if (existing != nullptr) {
        updated = *existing;
    }
    updated.properties.insert_or_assign(
        std::string{topology::schema::control_base_port},
        doc::PropertyValue{static_cast<std::int64_t>(edit.base_port)});
    cmd::Payload inverse = existing != nullptr
                               ? cmd::Payload{cmd::PutNode{*existing}}
                               : cmd::Payload{cmd::EraseNode{allocated_id}};
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, std::move(inverse),
        cmd::UndoContextId{3U}, cmd::TransactionId{edit.gesture}, apply_at));
}

// Authors the project-wide synchronization policy exactly like every other
// document edit, so it replicates, journals, and undoes through the same
// paths. A zero policy provenance creates sync.policy; an existing node is
// replaced whole, which keeps a half-edited raster from ever being visible.
[[nodiscard]] bool
schedule_sync_policy(cmd::DirectorScheduler &scheduler,
                     const editor::HardwareSyncPolicyEdit &edit,
                     doc::NodeId allocated_id, cmd::FrameNumber apply_at) {
    if (!sync::valid_policy(edit.value)) {
        return true;
    }

    const doc::Root &root = scheduler.projected_document();
    const doc::Node *existing{};
    if (edit.policy != doc::NodeId{}) {
        existing = root.get(edit.policy);
        // A stale request from a replaced project must not create a second
        // policy object or edit a node whose identity was reused.
        if (existing == nullptr ||
            existing->type != sync::schema::policy_type) {
            return true;
        }
    } else {
        // Re-check the authoritative projected document at the authoring
        // boundary and update the lowest-id policy node if one arrived in the
        // meantime, preserving the model deterministic winner.
        const doc::Delta contents = doc::diff(doc::Root{}, root);
        for (const doc::NodeChange &change : contents.changes) {
            if (!change.after ||
                change.after->type != sync::schema::policy_type) {
                continue;
            }
            if (existing == nullptr || change.after->id < existing->id) {
                existing = change.after.get();
            }
        }
    }

    doc::Node updated{};
    updated.id = existing != nullptr ? existing->id : allocated_id;
    sync::write_sync_policy_properties(updated, edit.value);
    cmd::Payload inverse = existing != nullptr
                               ? cmd::Payload{cmd::PutNode{*existing}}
                               : cmd::Payload{cmd::EraseNode{allocated_id}};
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, std::move(inverse),
        cmd::UndoContextId{3U}, cmd::TransactionId{edit.gesture}, apply_at));
}

[[nodiscard]] std::string_view
hardware_window_mode_property(topology::HardwareWindowMode mode) noexcept {
    switch (mode) {
    case topology::HardwareWindowMode::Inherit:
        return topology::schema::inherit_window_mode;
    case topology::HardwareWindowMode::Windowed:
        return topology::schema::windowed_window_mode;
    case topology::HardwareWindowMode::Borderless:
        return topology::schema::borderless_window_mode;
    case topology::HardwareWindowMode::Fullscreen:
        return topology::schema::fullscreen_window_mode;
    }
    return topology::schema::fullscreen_window_mode;
}

[[nodiscard]] doc::Node
make_hardware_output_binding(doc::NodeId id,
                             const topology::HardwareOutputBinding &binding) {
    doc::Node node{};
    node.id = id;
    node.type = std::string{topology::schema::output_binding_type};
    node.properties.emplace(std::string{topology::schema::render_server},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                binding.render_server.value())});
    node.properties.emplace(
        std::string{topology::schema::logical_port},
        doc::PropertyValue{static_cast<std::int64_t>(binding.logical_port)});
    node.properties.emplace(
        std::string{topology::schema::feed_index},
        doc::PropertyValue{static_cast<std::int64_t>(binding.feed_index)});
    node.properties.emplace(
        std::string{topology::schema::display_adapter_id},
        doc::PropertyValue{
            topology::format_display_adapter_id(binding.target.adapter_id)});
    node.properties.emplace(std::string{topology::schema::display_target_id},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                binding.target.target_id)});
    node.properties.emplace(std::string{topology::schema::connector_instance},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                binding.target.connector_instance)});
    node.properties.emplace(
        std::string{topology::schema::width},
        doc::PropertyValue{static_cast<std::int64_t>(binding.width)});
    node.properties.emplace(
        std::string{topology::schema::height},
        doc::PropertyValue{static_cast<std::int64_t>(binding.height)});
    node.properties.emplace(std::string{topology::schema::refresh_numerator},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                binding.refresh_numerator)});
    node.properties.emplace(std::string{topology::schema::refresh_denominator},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                binding.refresh_denominator)});
    node.properties.emplace(
        std::string{topology::schema::window_mode},
        doc::PropertyValue{
            std::string{hardware_window_mode_property(binding.window_mode)}});
    return node;
}

[[nodiscard]] bool
schedule_hardware_output_binding(cmd::DirectorScheduler &scheduler,
                                 const editor::HardwareOutputBindingEdit &edit,
                                 doc::NodeId allocated_id,
                                 cmd::FrameNumber apply_at) {
    const topology::HardwareOutputBinding &binding = edit.binding;
    if (binding.render_server == doc::NodeId{} ||
        binding.logical_port < topology::first_render_server_video_port ||
        binding.logical_port > topology::last_render_server_video_port ||
        binding.feed_index > topology::max_hardware_node_index ||
        !binding.target.valid() || binding.width == 0U ||
        binding.width > topology::max_hardware_output_width ||
        binding.height == 0U ||
        binding.height > topology::max_hardware_output_height ||
        binding.refresh_numerator == 0U || binding.refresh_denominator == 0U ||
        binding.window_mode == topology::HardwareWindowMode::Inherit) {
        return true;
    }

    const doc::Root &root = scheduler.projected_document();
    topology::HardwareMapModel model;
    model.reset(root);
    const topology::HardwareMap map = model.evaluate();
    const topology::HardwareDevice *const server =
        map.device(binding.render_server);
    if (server == nullptr ||
        server->kind != topology::HardwareDeviceKind::RenderServer) {
        return true;
    }
    const doc::NodeId id =
        binding.node != doc::NodeId{} ? binding.node : allocated_id;
    const doc::Node *const existing = root.get(id);
    if (existing != nullptr &&
        existing->type != topology::schema::output_binding_type) {
        return true;
    }
    for (const topology::HardwareOutputBinding &other : map.output_bindings) {
        if (other.node == id || other.render_server != binding.render_server) {
            continue;
        }
        if (other.logical_port == binding.logical_port ||
            other.feed_index == binding.feed_index ||
            other.target == binding.target) {
            return true;
        }
    }
    cmd::Payload inverse = existing != nullptr
                               ? cmd::Payload{cmd::PutNode{*existing}}
                               : cmd::Payload{cmd::EraseNode{id}};
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{make_hardware_output_binding(id, binding)},
        std::move(inverse), cmd::UndoContextId{3U},
        cmd::TransactionId{edit.gesture}, apply_at));
}

[[nodiscard]] const doc::Node *find_feed_map_node(const doc::Root &root,
                                                  core::u32 node_index);

[[nodiscard]] std::vector<const doc::Node *>
find_feed_map_nodes(const doc::Root &root, core::u32 node_index);

[[nodiscard]] bool schedule_hardware_output_binding_delete(
    cmd::DirectorScheduler &scheduler,
    const editor::HardwareOutputBindingDelete &request,
    cmd::FrameNumber apply_at) {
    const doc::Node *const existing =
        scheduler.projected_document().get(request.binding);
    if (existing == nullptr ||
        existing->type != topology::schema::output_binding_type) {
        return true;
    }
    topology::HardwareMapModel model;
    model.reset(scheduler.projected_document());
    const topology::HardwareMap map = model.evaluate();
    const topology::HardwareOutputBinding *const binding =
        map.output_binding(request.binding);
    if (binding == nullptr) {
        return true;
    }
    const bool feed_is_shared =
        std::any_of(map.output_bindings.begin(), map.output_bindings.end(),
                    [&](const topology::HardwareOutputBinding &other) {
                        return other.node != binding->node &&
                               other.feed_index == binding->feed_index;
                    });
    if (!feed_is_shared) {
        for (const doc::Node *const feed : find_feed_map_nodes(
                 scheduler.projected_document(), binding->feed_index)) {
            if (!schedule_erase_node(
                    scheduler, feed->id, cmd::UndoContextId{3U},
                    cmd::TransactionId{request.gesture}, apply_at)) {
                return false;
            }
        }
    }
    return schedule_erase_node(scheduler, request.binding,
                               cmd::UndoContextId{3U},
                               cmd::TransactionId{request.gesture}, apply_at);
}

[[nodiscard]] bool
schedule_hardware_delete(cmd::DirectorScheduler &scheduler,
                         const editor::HardwareDeleteRequest &request,
                         cmd::FrameNumber apply_at) {
    if (request.target.kind == editor::HardwareSelectionKind::None ||
        request.target.node == doc::NodeId{}) {
        return true;
    }
    const doc::Root &root = scheduler.projected_document();
    const doc::Node *const target = root.get(request.target.node);
    if (target == nullptr) {
        return true;
    }
    const doc::NodeId target_id = target->id;
    const std::string target_type = target->type;
    const cmd::UndoContextId context{3U};
    const cmd::TransactionId transaction{request.gesture};
    if (request.target.kind == editor::HardwareSelectionKind::Link) {
        if (target_type != topology::schema::link_type) {
            return true;
        }
        topology::HardwareMapModel model;
        model.reset(root);
        const topology::HardwareMap map = model.evaluate();
        if (const topology::HardwareLink *const link = map.link(target_id)) {
            if (const topology::HardwareOutputBinding *const binding =
                    map.output_binding_for(link->from.device,
                                           link->from.port)) {
                if (!schedule_hardware_output_binding_delete(
                        scheduler,
                        editor::HardwareOutputBindingDelete{binding->node,
                                                            request.gesture},
                        apply_at)) {
                    return false;
                }
            }
        }
        return schedule_erase_node(scheduler, target_id, context, transaction,
                                   apply_at);
    }
    if (target_type != topology::schema::device_type) {
        return true;
    }

    try {
        std::vector<doc::NodeId> incident;
        std::vector<core::u32> feeds;
        topology::HardwareMapModel model;
        model.reset(root);
        const topology::HardwareMap hardware = model.evaluate();
        for (const topology::HardwareOutputBinding &binding :
             hardware.output_bindings) {
            if (binding.render_server == target_id) {
                feeds.push_back(binding.feed_index);
            }
        }
        const doc::Delta contents = doc::diff(doc::Root{}, root);
        for (const doc::NodeChange &change : contents.changes) {
            if (!change.after) {
                continue;
            }
            if (change.after->type == topology::schema::output_binding_type) {
                const auto server = node_reference(
                    *change.after, topology::schema::render_server);
                if (server && *server == target_id) {
                    incident.push_back(change.after->id);
                }
                continue;
            }
            if (change.after->type != topology::schema::link_type) {
                continue;
            }
            const auto from =
                node_reference(*change.after, topology::schema::from_device);
            const auto to =
                node_reference(*change.after, topology::schema::to_device);
            if ((from && *from == target_id) || (to && *to == target_id)) {
                incident.push_back(change.after->id);
            }
        }
        for (const core::u32 feed_index : feeds) {
            const bool shared = std::any_of(
                hardware.output_bindings.begin(),
                hardware.output_bindings.end(),
                [&](const topology::HardwareOutputBinding &binding) {
                    return binding.render_server != target_id &&
                           binding.feed_index == feed_index;
                });
            if (!shared) {
                for (const doc::Node *const feed : find_feed_map_nodes(
                         scheduler.projected_document(), feed_index)) {
                    if (!schedule_erase_node(scheduler, feed->id, context,
                                             transaction, apply_at)) {
                        return false;
                    }
                }
            }
        }
        for (const doc::NodeId link : incident) {
            if (!schedule_erase_node(scheduler, link, context, transaction,
                                     apply_at)) {
                return false;
            }
        }
        return schedule_erase_node(scheduler, target_id, context, transaction,
                                   apply_at);
    } catch (const std::bad_alloc &) {
        return false;
    }
}

[[nodiscard]] const doc::Node *find_feed_map_node(const doc::Root &root,
                                                  core::u32 node_index) {
    const auto nodes = find_feed_map_nodes(root, node_index);
    return nodes.empty() ? nullptr : nodes.front();
}

[[nodiscard]] std::vector<const doc::Node *>
find_feed_map_nodes(const doc::Root &root, core::u32 node_index) {
    std::vector<const doc::Node *> matches;
    const doc::Delta contents = doc::diff(doc::Root{}, root);
    for (const doc::NodeChange &change : contents.changes) {
        if (!change.after || change.after->type != output::schema::map_type) {
            continue;
        }
        const auto found =
            change.after->properties.find(output::schema::node_index);
        if (found == change.after->properties.end()) {
            continue;
        }
        const auto *const value = std::get_if<std::int64_t>(&found->second);
        if (value != nullptr && *value >= 0 &&
            static_cast<core::u64>(*value) == node_index) {
            matches.push_back(change.after.get());
        }
    }
    return matches;
}

[[nodiscard]] const doc::Node *find_feed_map_node(const doc::Root &root,
                                                  doc::NodeId node) noexcept {
    const doc::Node *const found = root.get(node);
    return found != nullptr && found->type == output::schema::map_type
               ? found
               : nullptr;
}

[[nodiscard]] const doc::Node *find_feed_surface_node(const doc::Root &root,
                                                      doc::NodeId id) noexcept {
    const doc::Node *const node = root.get(id);
    return node != nullptr && node->type == output::schema::surface_type
               ? node
               : nullptr;
}

[[nodiscard]] bool
feed_surface_reference_fits_document(doc::NodeId id) noexcept {
    return id.value() <=
           static_cast<core::u64>(std::numeric_limits<std::int64_t>::max());
}

[[nodiscard]] doc::NodeId projection_asset_for_source(const doc::Root &root,
                                                      std::string_view source) {
    if (source.empty()) {
        return {};
    }
    assets::AssetModel model;
    model.reset(root);
    const assets::AssetRegistry registry = model.evaluate();
    const assets::Asset *const asset =
        registry.asset_for_source(source, assets::AssetKind::ProjectionMesh);
    return asset != nullptr ? assets::asset_id(*asset) : doc::NodeId{};
}

void write_feed_surface_properties(doc::Node &node, std::string_view name,
                                   const output::PhysicalSurface &surface,
                                   output::ProjectionSurfaceShape shape,
                                   float horizontal_arc_turns,
                                   core::u32 mesh_columns,
                                   std::string_view mesh_source,
                                   float mesh_scale, doc::NodeId mesh_asset) {
    std::string_view shape_property = output::schema::shape_planar;
    switch (shape) {
    case output::ProjectionSurfaceShape::Planar:
        break;
    case output::ProjectionSurfaceShape::HorizontalArc:
        shape_property = output::schema::shape_horizontal_arc;
        break;
    case output::ProjectionSurfaceShape::ImportedMesh:
        shape_property = output::schema::shape_imported_mesh;
        break;
    }
    node.properties.insert_or_assign(std::string{output::schema::surface_name},
                                     doc::PropertyValue{std::string{name}});
    node.properties.insert_or_assign(
        std::string{output::schema::surface_shape},
        doc::PropertyValue{std::string{shape_property}});
    node.properties.insert_or_assign(
        std::string{output::schema::center_x},
        doc::PropertyValue{static_cast<double>(surface.center.x)});
    node.properties.insert_or_assign(
        std::string{output::schema::center_y},
        doc::PropertyValue{static_cast<double>(surface.center.y)});
    node.properties.insert_or_assign(
        std::string{output::schema::center_z},
        doc::PropertyValue{static_cast<double>(surface.center.z)});
    node.properties.insert_or_assign(
        std::string{output::schema::right_x},
        doc::PropertyValue{static_cast<double>(surface.right.x)});
    node.properties.insert_or_assign(
        std::string{output::schema::right_y},
        doc::PropertyValue{static_cast<double>(surface.right.y)});
    node.properties.insert_or_assign(
        std::string{output::schema::right_z},
        doc::PropertyValue{static_cast<double>(surface.right.z)});
    node.properties.insert_or_assign(
        std::string{output::schema::up_x},
        doc::PropertyValue{static_cast<double>(surface.up.x)});
    node.properties.insert_or_assign(
        std::string{output::schema::up_y},
        doc::PropertyValue{static_cast<double>(surface.up.y)});
    node.properties.insert_or_assign(
        std::string{output::schema::up_z},
        doc::PropertyValue{static_cast<double>(surface.up.z)});
    node.properties.insert_or_assign(
        std::string{output::schema::physical_width},
        doc::PropertyValue{static_cast<double>(surface.width)});
    node.properties.insert_or_assign(
        std::string{output::schema::physical_height},
        doc::PropertyValue{static_cast<double>(surface.height)});
    node.properties.insert_or_assign(
        std::string{output::schema::raster_width},
        doc::PropertyValue{static_cast<std::int64_t>(surface.raster.width)});
    node.properties.insert_or_assign(
        std::string{output::schema::raster_height},
        doc::PropertyValue{static_cast<std::int64_t>(surface.raster.height)});
    node.properties.insert_or_assign(
        std::string{output::schema::horizontal_arc_turns},
        doc::PropertyValue{static_cast<double>(horizontal_arc_turns)});
    node.properties.insert_or_assign(
        std::string{output::schema::mesh_columns},
        doc::PropertyValue{static_cast<std::int64_t>(mesh_columns)});
    node.properties.insert_or_assign(
        std::string{output::schema::mesh_source},
        doc::PropertyValue{std::string{mesh_source}});
    if (shape == output::ProjectionSurfaceShape::ImportedMesh && mesh_asset &&
        feed_surface_reference_fits_document(mesh_asset)) {
        node.properties.insert_or_assign(
            std::string{output::schema::mesh_asset_id},
            doc::PropertyValue{static_cast<std::int64_t>(mesh_asset.value())});
    } else {
        node.properties.erase(std::string{output::schema::mesh_asset_id});
    }
    node.properties.insert_or_assign(
        std::string{output::schema::mesh_scale},
        doc::PropertyValue{static_cast<double>(mesh_scale)});
}

[[nodiscard]] bool
valid_feed_surface_request(const output::PhysicalSurface &surface,
                           output::ProjectionSurfaceShape shape,
                           float horizontal_arc_turns, core::u32 mesh_columns,
                           std::string_view mesh_source,
                           float mesh_scale) noexcept {
    // The model parses every persisted scalar even when its shape does not
    // currently use it. Refuse dormant non-finite shape parameters here too,
    // so a node accepted for authoring cannot be rejected when it is evaluated
    // or loaded from a saved project.
    if (!output::valid(surface) || !std::isfinite(horizontal_arc_turns) ||
        !std::isfinite(mesh_scale)) {
        return false;
    }
    switch (shape) {
    case output::ProjectionSurfaceShape::Planar:
        return true;
    case output::ProjectionSurfaceShape::HorizontalArc:
        return std::abs(horizontal_arc_turns) > 1.0e-5F &&
               std::abs(horizontal_arc_turns) < 0.5F &&
               mesh_columns >= output::min_horizontal_arc_columns &&
               mesh_columns <= output::max_horizontal_arc_columns;
    case output::ProjectionSurfaceShape::ImportedMesh:
        return !mesh_source.empty() && mesh_scale > 0.0F &&
               mesh_scale <= output::max_imported_mesh_scale;
    }
    return false;
}

[[nodiscard]] doc::Node
make_feed_surface_node(doc::NodeId id,
                       const editor::FeedSurfaceAddRequest &request,
                       doc::NodeId mesh_asset) {
    doc::Node node{};
    node.id = id;
    node.type = std::string{output::schema::surface_type};
    node.properties.insert_or_assign(std::string{output::schema::locked},
                                     doc::PropertyValue{true});
    write_feed_surface_properties(node, request.name, request.surface,
                                  request.shape, request.horizontal_arc_turns,
                                  request.mesh_columns, request.mesh_source,
                                  request.mesh_scale, mesh_asset);
    return node;
}

[[nodiscard]] bool
schedule_feed_surface_add(cmd::DirectorScheduler &scheduler,
                          const editor::FeedSurfaceAddRequest &request,
                          doc::NodeId allocated_id, cmd::FrameNumber apply_at) {
    if (!valid_feed_surface_request(
            request.surface, request.shape, request.horizontal_arc_turns,
            request.mesh_columns, request.mesh_source, request.mesh_scale)) {
        return true;
    }
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{make_feed_surface_node(
            allocated_id, request,
            projection_asset_for_source(scheduler.projected_document(),
                                        request.mesh_source))},
        cmd::EraseNode{allocated_id}, cmd::UndoContextId{4U},
        cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] bool
schedule_feed_surface_edit(cmd::DirectorScheduler &scheduler,
                           const editor::FeedSurfaceEditRequest &request,
                           cmd::FrameNumber apply_at) {
    const doc::Node *const existing =
        find_feed_surface_node(scheduler.projected_document(), request.node);
    if (existing == nullptr ||
        !valid_feed_surface_request(
            request.surface, request.shape, request.horizontal_arc_turns,
            request.mesh_columns, request.mesh_source, request.mesh_scale)) {
        return true;
    }
    doc::Node updated = *existing;
    write_feed_surface_properties(
        updated, request.name, request.surface, request.shape,
        request.horizontal_arc_turns, request.mesh_columns, request.mesh_source,
        request.mesh_scale,
        projection_asset_for_source(scheduler.projected_document(),
                                    request.mesh_source));

    // Surface views receive evaluated geometry. Preserve every transform
    // property the originating control did not actually change, otherwise an
    // edit to an unanimated axis (or to mesh calibration) would bake another
    // axis's animated value into the authored base beneath its channel.
    const auto preserve = [&](std::string_view property) {
        const auto authored = existing->properties.find(property);
        if (authored != existing->properties.end()) {
            updated.properties.insert_or_assign(std::string{property},
                                                authored->second);
            return;
        }
        const auto generated = updated.properties.find(property);
        if (generated != updated.properties.end()) {
            updated.properties.erase(generated);
        }
    };
    if (!editor::has_transform(request.transform_components,
                               editor::FeedSurfaceTransform::PositionX)) {
        preserve(output::schema::center_x);
    }
    if (!editor::has_transform(request.transform_components,
                               editor::FeedSurfaceTransform::PositionY)) {
        preserve(output::schema::center_y);
    }
    if (!editor::has_transform(request.transform_components,
                               editor::FeedSurfaceTransform::PositionZ)) {
        preserve(output::schema::center_z);
    }
    if (!editor::has_transform(request.transform_components,
                               editor::FeedSurfaceTransform::Yaw)) {
        preserve(output::schema::right_x);
        preserve(output::schema::right_y);
        preserve(output::schema::right_z);
        preserve(output::schema::up_x);
        preserve(output::schema::up_y);
        preserve(output::schema::up_z);
    }
    const cmd::UndoContextId context{request.transform_components ==
                                             editor::FeedSurfaceTransform::None
                                         ? 4U
                                         : 2U};
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, cmd::PutNode{*existing}, context,
        cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] bool
schedule_feed_surface_delete(cmd::DirectorScheduler &scheduler,
                             const editor::FeedSurfaceDeleteRequest &request,
                             cmd::FrameNumber apply_at) {
    const doc::Node *const existing =
        find_feed_surface_node(scheduler.projected_document(), request.node);
    if (existing == nullptr) {
        return true;
    }
    // Output-map references deliberately survive this erase. Their unresolved
    // surface id is a visible, repairable project error and undo restores the
    // exact surface without having to reconstruct assignments.
    return schedule_erase_node(scheduler, existing->id, cmd::UndoContextId{4U},
                               cmd::TransactionId{request.gesture}, apply_at);
}

[[nodiscard]] bool
schedule_feed_projection_edit(cmd::DirectorScheduler &scheduler,
                              const editor::FeedProjectionEdit &edit,
                              cmd::FrameNumber apply_at) {
    const doc::Root &root = scheduler.projected_document();
    const doc::Node *const existing = find_feed_map_node(root, edit.node);
    if (existing == nullptr || edit.source.width == 0U ||
        edit.source.height == 0U) {
        return true;
    }

    std::string_view projection;
    doc::NodeId surface{};
    switch (edit.projection) {
    case output::FeedProjection::CanvasSlice:
        projection = output::schema::projection_canvas;
        break;
    case output::FeedProjection::PlanarSurface: {
        projection = output::schema::projection_planar_surface;
        surface = edit.surface;
        if (!surface || !feed_surface_reference_fits_document(surface) ||
            find_feed_surface_node(root, surface) == nullptr) {
            return true;
        }
        output::FeedMapModel model;
        model.reset(root);
        const output::FeedMap evaluated = model.evaluate();
        const output::ProjectionSurface *const resolved =
            evaluated.surface_for(surface);
        if (resolved == nullptr ||
            !scene::contains(resolved->geometry.raster, edit.source)) {
            return true;
        }
        break;
    }
    default:
        return true;
    }

    doc::Node updated = *existing;
    updated.properties.insert_or_assign(
        std::string{output::schema::projection},
        doc::PropertyValue{std::string{projection}});
    updated.properties.insert_or_assign(
        std::string{output::schema::surface_id},
        doc::PropertyValue{static_cast<std::int64_t>(surface.value())});
    updated.properties.insert_or_assign(
        std::string{output::schema::source_x},
        doc::PropertyValue{static_cast<std::int64_t>(edit.source.x)});
    updated.properties.insert_or_assign(
        std::string{output::schema::source_y},
        doc::PropertyValue{static_cast<std::int64_t>(edit.source.y)});
    updated.properties.insert_or_assign(
        std::string{output::schema::source_width},
        doc::PropertyValue{static_cast<std::int64_t>(edit.source.width)});
    updated.properties.insert_or_assign(
        std::string{output::schema::source_height},
        doc::PropertyValue{static_cast<std::int64_t>(edit.source.height)});
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, cmd::PutNode{*existing},
        cmd::UndoContextId{4U}, cmd::TransactionId{edit.gesture}, apply_at));
}

[[nodiscard]] bool schedule_feed_map_edit(cmd::DirectorScheduler &scheduler,
                                          const editor::FeedMapRectEdit &edit,
                                          doc::NodeId allocated_id,
                                          cmd::FrameNumber apply_at) {
    static_cast<void>(allocated_id);
    if (edit.viewport.width == 0U || edit.viewport.height == 0U) {
        return true;
    }
    const doc::Node *const existing =
        find_feed_map_node(scheduler.projected_document(), edit.node);
    if (existing == nullptr) {
        return true;
    }
    doc::Node updated{};
    updated.id = existing->id;
    updated.type = std::string{output::schema::map_type};
    updated = *existing;
    updated.properties.insert_or_assign(
        std::string{output::schema::viewport_x},
        doc::PropertyValue{static_cast<std::int64_t>(edit.viewport.x)});
    updated.properties.insert_or_assign(
        std::string{output::schema::viewport_y},
        doc::PropertyValue{static_cast<std::int64_t>(edit.viewport.y)});
    updated.properties.insert_or_assign(
        std::string{output::schema::viewport_width},
        doc::PropertyValue{static_cast<std::int64_t>(edit.viewport.width)});
    updated.properties.insert_or_assign(
        std::string{output::schema::viewport_height},
        doc::PropertyValue{static_cast<std::int64_t>(edit.viewport.height)});
    cmd::Payload inverse = cmd::PutNode{*existing};
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, std::move(inverse),
        cmd::UndoContextId{4U}, cmd::TransactionId{edit.gesture}, apply_at));
}

[[nodiscard]] bool
schedule_feed_map_add(cmd::DirectorScheduler &scheduler,
                      const editor::FeedMapAddRequest &request,
                      doc::NodeId allocated_id, cmd::FrameNumber apply_at,
                      cmd::UndoContextId context = cmd::UndoContextId{4U}) {
    if (request.source.width == 0U || request.source.height == 0U ||
        request.viewport.width == 0U || request.viewport.height == 0U) {
        return true;
    }
    doc::Node node{};
    node.id = allocated_id;
    node.type = std::string{output::schema::map_type};
    node.properties.emplace(
        std::string{output::schema::node_index},
        doc::PropertyValue{static_cast<std::int64_t>(request.node_index)});
    node.properties.emplace(
        std::string{output::schema::source_x},
        doc::PropertyValue{static_cast<std::int64_t>(request.source.x)});
    node.properties.emplace(
        std::string{output::schema::source_y},
        doc::PropertyValue{static_cast<std::int64_t>(request.source.y)});
    node.properties.emplace(
        std::string{output::schema::source_width},
        doc::PropertyValue{static_cast<std::int64_t>(request.source.width)});
    node.properties.emplace(
        std::string{output::schema::source_height},
        doc::PropertyValue{static_cast<std::int64_t>(request.source.height)});
    node.properties.emplace(
        std::string{output::schema::viewport_x},
        doc::PropertyValue{static_cast<std::int64_t>(request.viewport.x)});
    node.properties.emplace(
        std::string{output::schema::viewport_y},
        doc::PropertyValue{static_cast<std::int64_t>(request.viewport.y)});
    node.properties.emplace(
        std::string{output::schema::viewport_width},
        doc::PropertyValue{static_cast<std::int64_t>(request.viewport.width)});
    node.properties.emplace(
        std::string{output::schema::viewport_height},
        doc::PropertyValue{static_cast<std::int64_t>(request.viewport.height)});
    node.properties.emplace(
        std::string{output::schema::projection},
        doc::PropertyValue{std::string{output::schema::projection_canvas}});
    node.properties.emplace(std::string{output::schema::surface_id},
                            doc::PropertyValue{std::int64_t{0}});
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(node)}, cmd::EraseNode{allocated_id}, context,
        cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] bool
schedule_feed_map_delete(cmd::DirectorScheduler &scheduler,
                         const editor::FeedMapDeleteRequest &request,
                         cmd::FrameNumber apply_at) {
    const doc::Node *const existing =
        find_feed_map_node(scheduler.projected_document(), request.node);
    if (existing == nullptr) {
        return true;
    }
    return schedule_erase_node(scheduler, existing->id, cmd::UndoContextId{4U},
                               cmd::TransactionId{request.gesture}, apply_at);
}
#endif

[[nodiscard]] bool parse_u64(std::string_view value,
                             core::u64 &parsed) noexcept {
    if (value.empty()) {
        return false;
    }
    const auto *const begin = value.data();
    const auto *const end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    return result.ec == std::errc{} && result.ptr == end;
}

void sleep_until_boundary(Clock::time_point boundary) noexcept {
    const auto now = Clock::now();
    if (boundary <= now) {
        std::this_thread::yield();
        return;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(boundary - now);
    std::this_thread::sleep_for(std::min(remaining, max_sleep));
}

[[nodiscard]] const char *transport_label(show::TransportState state) noexcept {
    switch (state) {
    case show::TransportState::Playing:
        return "playing";
    case show::TransportState::Paused:
        return "paused";
    case show::TransportState::Stopped:
        break;
    }
    return "stopped";
}

[[nodiscard]] const char *
outcome_label(show::FrameCommitOutcome outcome) noexcept {
    switch (outcome) {
    case show::FrameCommitOutcome::Idle:
        return "idle";
    case show::FrameCommitOutcome::Ready:
        return "ready";
    case show::FrameCommitOutcome::Committed:
        return "committed";
    case show::FrameCommitOutcome::NotApproved:
        return "waiting";
    case show::FrameCommitOutcome::CheckpointPending:
        return "checkpoint";
    case show::FrameCommitOutcome::NeedsResync:
        return "resync";
    case show::FrameCommitOutcome::Unavailable:
        break;
    }
    return "disconnected";
}

[[nodiscard]] const char *sync_mode_label(sync::SyncMode mode) noexcept {
    switch (mode) {
    case sync::SyncMode::Unsynced:
        return "unsynced";
    case sync::SyncMode::Framelock:
        return "framelock";
    case sync::SyncMode::ExternalGenlock:
        return "external-genlock";
    }
    return "unsynced";
}

[[nodiscard]] const char *
runtime_barrier_label(net::RuntimeBarrierState state) noexcept {
    switch (state) {
    case net::RuntimeBarrierState::Unknown:
        return "unknown";
    case net::RuntimeBarrierState::Unsupported:
        return "unsupported";
    case net::RuntimeBarrierState::NotJoined:
        return "not-joined";
    case net::RuntimeBarrierState::Joining:
        return "joining";
    case net::RuntimeBarrierState::Joined:
        return "joined";
    case net::RuntimeBarrierState::Left:
        return "left";
    case net::RuntimeBarrierState::Failed:
        return "failed";
    }
    return "unknown";
}

[[nodiscard]] const char *
presenter_error_text(show::PresenterError error) noexcept {
    switch (error) {
    case show::PresenterError::InvalidConfiguration:
        return "invalid surface configuration";
    case show::PresenterError::AllocationFailed:
        return "allocation failed";
    case show::PresenterError::ExactDisplayModeUnavailable:
        return "the selected display does not support the exact saved "
               "resolution and refresh rate";
    case show::PresenterError::DisplayUnavailable:
        break;
    }
    return "the platform refused a window";
}

[[nodiscard]] show::ShowClockConfig
clock_config(const PlaybackOptions &options) noexcept {
    return show::ShowClockConfig{options.rate, options.first_frame,
                                 options.last_frame, options.loop};
}

// Per-frame presentation cost, reported only where there is one to report.
// The CPU path has no submission to time and says nothing rather than zero.
[[nodiscard]] std::string gpu_timing_text(const show::PresenterTiming &timing) {
    if (!timing.gpu) {
        return {};
    }
    return " present-us=" + std::to_string(timing.last_present_ns / 1'000U) +
           " present-peak-us=" +
           std::to_string(timing.peak_present_ns / 1'000U) +
           " gpu-observed-us=" +
           std::to_string(timing.last_submit_to_observed_ns / 1'000U) +
           " gpu-skipped=" + std::to_string(timing.skipped_presents) +
           " media-buffered=" + std::to_string(timing.media_deferred_frames) +
           " gpu-present=" + (timing.vsync_limited ? "vsync" : "unthrottled");
}

// Control frames the wall clock says are owed but the seal loop has not
// produced. Under an anchored transport a Director that cannot keep up makes
// the show run slow rather than skip positions, so this is the number that
// says so instead of the old skipped-position count.
[[nodiscard]] core::u64 pacer_lag(const show::ShowClock &pacer,
                                  core::u64 sealed) noexcept {
    const auto due = pacer.frame().value();
    return due > sealed ? due - sealed : 0U;
}

[[nodiscard]] core::u64 status_interval(const PlaybackOptions &options,
                                        show::FrameRate rate) noexcept {
    if (options.status_interval != 0U) {
        return options.status_interval;
    }
    const auto per_second =
        static_cast<core::u64>(show::frames_per_second(rate));
    return per_second == 0U ? 1U : per_second;
}

// Builds the output for one role. A windowed request in a build without the
// shell is refused rather than silently downgraded: an operator who asked for a
// window and got a headless run would have no way to tell.
[[nodiscard]] std::unique_ptr<show::Presenter>
make_presenter(const PlaybackOptions &options, const std::string &title,
               bool accepts_commands,
               const PresenterWindowSettings *applied_window = nullptr) {
    if (options.windowed) {
#if defined(MEDIASERVER_HAS_GUI)
        ui::SdlPresenterConfig config{};
        config.title = title;
        config.width = options.surface_width;
        config.height = options.surface_height;
        config.position_x = options.window_x;
        config.position_y = options.window_y;
        config.position_explicit = options.window_position_explicit;
        config.fullscreen = options.fullscreen;
        config.borderless = options.borderless;
        if (applied_window != nullptr) {
            config.refresh_numerator = applied_window->refresh_numerator;
            config.refresh_denominator = applied_window->refresh_denominator;
            config.require_exact_display_mode =
                applied_window->require_exact_display_mode;
        }
        config.accepts_commands = accepts_commands;
        config.gpu = options.gpu;
        config.scene = options.scene;
        config.output = options.output;
        config.buffered_media = options.output && !accepts_commands;
        config.media_read_ahead_frames =
            options.active_preferences.output_read_ahead_frames;
        config.media_cache_mebibytes =
            options.active_preferences.playback_cache_mebibytes;
        config.media_trail_frames =
            options.active_preferences.playback_trail_frames;

        auto created = ui::SdlPresenter::create(config);
        if (!created) {
            std::cerr << "could not open the output window: "
                      << presenter_error_text(created.error()) << '\n';
            return nullptr;
        }
        return std::move(created).value();
#else
        static_cast<void>(title);
        static_cast<void>(accepts_commands);
        static_cast<void>(applied_window);
        std::cerr << "this build has no window output; configure with "
                     "MEDIASERVER_ENABLE_GUI=ON and the vcpkg gui feature\n";
        return nullptr;
#endif
    }

    auto created = show::HeadlessPresenter::create(options.surface_width,
                                                   options.surface_height);
    if (!created) {
        std::cerr << "could not allocate the presentation surface: "
                  << presenter_error_text(created.error()) << '\n';
        return nullptr;
    }
    return std::move(created).value();
}

// Applies one operator command to the Director transport, re-anchoring it at
// the control frame the result will be sealed into. Every command is exact
// integer state, and the nodes learn about it through the ordinary
// sealed-frame path.
void apply_command(show::TransportAuthority &transport,
                   show::PresenterCommand command,
                   cmd::FrameNumber at) noexcept {
    const auto per_second = static_cast<core::u64>(
        show::frames_per_second(transport.anchor().rate));
    // The live range from the transport, so a range change from the editor is
    // respected by the step and jump clamps immediately.
    const auto first = transport.anchor().first_frame.value();
    const auto last = transport.anchor().last_frame.value();
    const auto current = transport.frame_at(at).value();

    const auto clamped = [first, last](core::u64 target) noexcept {
        return show::ShowFrame{std::min(std::max(target, first), last)};
    };
    const auto back = [current, first](core::u64 delta) noexcept {
        return current - first >= delta ? current - delta : first;
    };
    const auto forward = [current, last](core::u64 delta) noexcept {
        return last - current >= delta ? current + delta : last;
    };

    switch (command) {
    case show::PresenterCommand::TogglePlay:
        if (transport.state() == show::TransportState::Playing) {
            static_cast<void>(transport.pause(at));
        } else {
            static_cast<void>(transport.play(at));
        }
        return;
    case show::PresenterCommand::StepBack:
    case show::PresenterCommand::StepForward:
    case show::PresenterCommand::JumpBack:
    case show::PresenterCommand::JumpForward:
    case show::PresenterCommand::GoToStart:
        break;
    case show::PresenterCommand::None:
    case show::PresenterCommand::Quit:
        return;
    }

    // Scrubbing is a paused operation: stepping while playing would be
    // immediately overwritten by the next boundary.
    if (transport.state() == show::TransportState::Playing) {
        static_cast<void>(transport.pause(at));
    }

    switch (command) {
    case show::PresenterCommand::StepBack:
        static_cast<void>(transport.seek(clamped(back(1U)), at));
        return;
    case show::PresenterCommand::StepForward:
        static_cast<void>(transport.seek(clamped(forward(1U)), at));
        return;
    case show::PresenterCommand::JumpBack:
        static_cast<void>(transport.seek(clamped(back(per_second)), at));
        return;
    case show::PresenterCommand::JumpForward:
        static_cast<void>(transport.seek(clamped(forward(per_second)), at));
        return;
    case show::PresenterCommand::GoToStart:
        static_cast<void>(transport.seek(transport.anchor().first_frame, at));
        return;
    default:
        return;
    }
}

#if defined(MEDIASERVER_HAS_GUI)
// Fills the per-output physical-timing row of Output Monitor.
//
// Each row answers for one supervised render process: how many of its outputs
// the authored sync groups expect, how many actually report locked, whether
// its swapchains joined the expected barrier, and what its frame accounting
// looks like. A row with nothing to go on reports SYNC UNVERIFIED rather than
// an absence that could be mistaken for health.
void apply_node_sync_rows(std::vector<editor::OutputMonitorState::Node> &rows,
                          const sync::SyncPolicy &policy,
                          const sync::SyncTopology &topology,
                          const sync::SyncStatusSnapshot &status) {
    using RowSync = editor::OutputMonitorState::Node::SyncStatus;
    for (editor::OutputMonitorState::Node &row : rows) {
        row.sync = RowSync::Unsynced;
        row.sync_locked_outputs = 0U;
        row.sync_expected_outputs = 0U;
        row.barrier_joined_outputs = 0U;
        row.barrier_expected_outputs = 0U;
        row.missed_frames = 0U;
        row.late_frames = 0U;
        row.held_frames = 0U;
        row.repeated_frames = 0U;
        row.loss_of_sync_events = 0U;
        row.sync_detail.clear();
        if (!policy.requires_physical_sync()) {
            continue;
        }

        const sync::NodeSyncStatus *const observed =
            status.node(row.node_index);
        for (const sync::SyncGroup &group : topology.groups) {
            for (const sync::SyncGroupMember &member : group.members) {
                if (member.node_index != row.node_index) {
                    continue;
                }
                ++row.sync_expected_outputs;
                if (observed == nullptr) {
                    continue;
                }
                for (const sync::SyncOutputStatus &output : observed->outputs) {
                    if (output.target == member.target &&
                        output.lock == sync::SyncLockState::Locked) {
                        ++row.sync_locked_outputs;
                        break;
                    }
                }
            }
        }

        if (observed == nullptr ||
            (!observed->machine_reported && !observed->runtime_reported)) {
            row.sync = RowSync::Unverified;
            continue;
        }
        row.sync_detail = observed->detail;
        row.barrier_joined_outputs = observed->barrier_joined_outputs;
        row.barrier_expected_outputs = observed->barrier_expected_outputs;
        row.missed_frames = observed->counters.missed;
        row.late_frames = observed->counters.late;
        row.held_frames = observed->counters.held;
        row.repeated_frames = observed->counters.repeated;
        row.loss_of_sync_events = observed->counters.loss_of_sync_events;

        // Ordered from the most fundamental missing fact to the most
        // specific, so the row names the first thing an operator must fix.
        switch (observed->board) {
        case sync::SyncDeviceAvailability::Absent:
        case sync::SyncDeviceAvailability::Failed:
            row.sync = RowSync::SyncDeviceMissing;
            continue;
        case sync::SyncDeviceAvailability::Unknown:
        case sync::SyncDeviceAvailability::Unsupported:
            row.sync = RowSync::Unverified;
            continue;
        case sync::SyncDeviceAvailability::Present:
            break;
        }
        if (policy.requires_external_reference()) {
            switch (observed->reference) {
            case sync::SyncReferenceState::Absent:
            case sync::SyncReferenceState::Unstable:
                row.sync = RowSync::ReferenceLost;
                continue;
            case sync::SyncReferenceState::Present:
                row.sync = RowSync::ReferenceNotInUse;
                continue;
            case sync::SyncReferenceState::Unknown:
            case sync::SyncReferenceState::Unsupported:
                row.sync = RowSync::Unverified;
                continue;
            case sync::SyncReferenceState::InUse:
                break;
            }
        }
        if (row.sync_locked_outputs < row.sync_expected_outputs) {
            row.sync = RowSync::Unlocked;
            continue;
        }
        if (observed->barrier != sync::PresentBarrierState::Joined ||
            observed->barrier_group != policy.group ||
            observed->barrier_id != policy.barrier ||
            observed->barrier_joined_outputs <
                observed->barrier_expected_outputs) {
            row.sync = observed->barrier == sync::PresentBarrierState::Unknown
                           ? RowSync::Unverified
                           : RowSync::BarrierIncomplete;
            continue;
        }
        row.sync = observed->simulated ? RowSync::Simulated : RowSync::Locked;
    }
}
#endif

} // namespace

bool window_output_available() noexcept {
#if defined(MEDIASERVER_HAS_GUI)
    return true;
#else
    return false;
#endif
}

bool gpu_output_available() noexcept {
#if defined(MEDIASERVER_HAS_GUI)
    return ui::gpu_output_available();
#else
    return false;
#endif
}

bool scene_output_available() noexcept {
#if defined(MEDIASERVER_HAS_GUI)
    return ui::scene_output_available();
#else
    return false;
#endif
}

bool output_output_available() noexcept {
#if defined(MEDIASERVER_HAS_GUI)
    return ui::output_output_available();
#else
    return false;
#endif
}

bool editor_available() noexcept {
#if defined(MEDIASERVER_HAS_GUI)
    return editor::editor_available();
#else
    return false;
#endif
}

bool parse_frame_rate(std::string_view value, show::FrameRate &rate) noexcept {
    const auto separator = value.find('/');
    core::u64 numerator{};
    core::u64 denominator{1U};
    if (separator == std::string_view::npos) {
        if (!parse_u64(value, numerator)) {
            return false;
        }
    } else if (!parse_u64(value.substr(0U, separator), numerator) ||
               !parse_u64(value.substr(separator + 1U), denominator)) {
        return false;
    }

    if (numerator == 0U || numerator > show::max_rate_numerator ||
        denominator == 0U || denominator > show::max_rate_denominator) {
        return false;
    }
    rate = show::FrameRate{static_cast<core::u32>(numerator),
                           static_cast<core::u32>(denominator)};
    return true;
}

bool parse_show_range(std::string_view value, show::ShowFrame &first,
                      show::ShowFrame &last) noexcept {
    const auto separator = value.find(':');
    if (separator == std::string_view::npos) {
        return false;
    }
    core::u64 begin{};
    core::u64 end{};
    if (!parse_u64(value.substr(0U, separator), begin) ||
        !parse_u64(value.substr(separator + 1U), end) || end < begin ||
        end - begin == std::numeric_limits<core::u64>::max()) {
        return false;
    }
    first = show::ShowFrame{begin};
    last = show::ShowFrame{end};
    return true;
}

bool valid_playback_options(const PlaybackOptions &options) noexcept {
    return show::ShowClock::valid_configuration(clock_config(options)) &&
           options.surface_width != 0U && options.surface_height != 0U &&
           options.surface_width <= 7'680U && options.surface_height <= 4'320U;
}

int run_node_playback(net::NodeSessionService &service,
                      cmd::NodeCommandBus &node, const PlaybackOptions &options,
                      const volatile std::sig_atomic_t &stop_requested) {
    // A node output window is input-inert on purpose: what it shows is
    // replicated state, so keyboard transport there would be a lie.
    auto presenter = make_presenter(
        options, "media-server node " + std::to_string(options.node_index),
        false);
    if (presenter == nullptr) {
        return 1;
    }
    PresenterWindowSettings active_presenter_settings{
        options.surface_width,
        options.surface_height,
        options.window_x,
        options.window_y,
        options.window_position_explicit,
        options.windowed,
        options.fullscreen,
        options.borderless};
    struct NodeOutputPresenter final {
        DeployedOutput output;
        std::unique_ptr<show::Presenter> presenter;
    };
    std::vector<NodeOutputPresenter> output_presenters;
    output_presenters.push_back(NodeOutputPresenter{
        DeployedOutput{{},
                       topology::first_render_server_video_port,
                       options.node_index,
                       {},
                       active_presenter_settings,
                       "launch output"},
        std::move(presenter)});
    OutputDeployment active_deployment{};
    active_deployment.node_index = options.node_index;
    active_deployment.legacy_single_output = true;
    active_deployment.outputs.push_back(output_presenters.front().output);
    bool node_deployment_runtime_ready = true;
    bool node_feed_bindings_runtime_ready = true;
    bool node_asset_runtime_ready = false;
    bool node_proxy_runtime_ready = true;

    std::unique_ptr<server::ProxyGenerationService> node_proxy_artifacts;
    if (options.scene || options.output) {
        node_proxy_artifacts = open_proxy_playback_artifacts(options, "node");
    }
    std::unique_ptr<ProxyGenerationWorker> node_proxy_generator;
    if (options.output) {
        auto generation_artifacts =
            open_proxy_playback_artifacts(options, "node proxy generator");
        if (generation_artifacts != nullptr) {
            node_proxy_generator = std::make_unique<ProxyGenerationWorker>(
                std::move(generation_artifacts));
        }
    }
    MediaDecodeSupportWorker node_decode_support;
    ProxyPlaybackService node_proxy_playback{node_proxy_artifacts.get()};
    std::vector<ProxyPlacementObservation> node_proxy_observations;
    std::array<net::RuntimeMediaAssetStatus, net::max_runtime_media_assets>
        node_media_status{};
    std::size_t node_media_status_count{};
    auto next_node_media_status_refresh = Clock::time_point::min();

    net::NodeServiceFrameCommitSink sink{service, node};
    show::FrameBoundaryDriverConfig config{};
    config.clock = clock_config(options);
    config.mailbox_capacity = 16U;
    config.max_commits_per_tick = 8U;

    auto created =
        show::FrameBoundaryDriver::create(config, sink, Clock::now());
    if (!created) {
        std::cerr << "invalid node frame-boundary driver configuration\n";
        return 1;
    }
    auto driver = std::move(created).value();

    auto retirement = show::RootRetirementRing::create(32U);
    if (!retirement) {
        std::cerr << "could not allocate the node root retirement ring\n";
        return 1;
    }
    auto retired = std::move(retirement).value();

    // The node clock is a pacer, not the transport: it decides when a boundary
    // occurs, while what is presented comes from the committed document.
    if (!driver.play(Clock::now())) {
        std::cerr << "could not start the node frame pacer\n";
        return 1;
    }

    auto interval = status_interval(options, driver.config().clock.rate);
    show::PublishedFrame presented{};
    core::u64 boundaries = 0U;
    core::u64 presented_frames = 0U;
    core::u64 since_status = 0U;
    core::u64 present_failures = 0U;
    auto last_outcome = show::FrameCommitOutcome::Idle;
    auto closed = false;

    // 3D scene state, used only when the presenter renders scenes. The model is
    // reset when the committed document changes and evaluated every frame, so a
    // playing show re-parses nothing and two nodes evaluate byte-identical
    // scenes from the same show frame.
    scene::SceneModel scene_model;
    core::u64 scene_version = 0U;
    bool scene_loaded = false;
    compositor::CompositorModel compositor_model;
    core::u64 compositor_version = 0U;
    bool compositor_loaded = false;

    // Feed-map state, used only on the output path. Reset on a document change
    // like the scene, and evaluated the same way, so two nodes derive the same
    // map from the same committed revision.
    output::FeedMapModel feed_model;
    core::u64 feed_version = 0U;
    bool feed_loaded = false;

    assets::AssetModel node_asset_model;
    assets::AssetRegistry node_asset_registry;
    core::u64 node_asset_version = 0U;
    core::u64 node_asset_registry_digest = 0U;
    core::u64 node_asset_manifest_digest = 0U;
    bool node_assets_loaded = false;
    ProjectAssetAvailability node_asset_availability;

    // Output-window placement is authored in the replicated Hardware Mapping
    // document. It is evaluated on version changes and only rebuilds the
    // presenter when the effective settings for this exact node index change.
    topology::HardwareMapModel node_hardware_model;
    core::u64 node_hardware_version{};
    bool node_hardware_loaded{};
    topology::HardwareMap node_hardware;
    SystemHardwareProbe local_hardware_probe;
    core::u64 local_inventory_generation{};
    auto next_inventory_check = Clock::time_point::min();

    // The project-wide synchronization policy is replicated document state
    // like everything else, so a node adopts a changed policy at a frame
    // boundary rather than being told out of band.
    sync::SyncPolicyModel node_sync_model;
    core::u64 node_sync_version{};
    bool node_sync_loaded{};
    sync::SyncPolicy node_sync_policy{};
    show::PresenterSyncRequest node_sync_request{};
    bool node_sync_request_pending{true};
    net::RuntimeOutputSync node_output_sync{};
    net::RuntimeOutputSync published_output_sync{};
    // Boundaries where the group late-frame policy repeated the previous
    // complete frame instead of advancing part of a wall.
    core::u64 policy_held_boundaries{};
    core::u64 late_boundaries{};
    core::u64 missed_boundaries{};
    OutputPlaybackBuffer output_playback_buffer{
        options.active_preferences.output_buffer_frames};
    // Transitions out of an aggregate Joined state observed here. Kept apart
    // from the presenter leave counts, which are already cumulative totals:
    // folding the two into one running sum would count each loss again on
    // every refresh.
    core::u64 barrier_loss_transitions{};

    const auto publish_runtime_adoption = [&]() noexcept {
        try {
            net::RuntimeAssetAdoption adoption{
                node_asset_manifest_digest,
                node_asset_runtime_ready && node_proxy_runtime_ready &&
                    node_deployment_runtime_ready &&
                    node_feed_bindings_runtime_ready};
            adoption.media_assets.assign(
                node_media_status.begin(),
                node_media_status.begin() +
                    static_cast<std::ptrdiff_t>(node_media_status_count));
            service.publish_runtime_asset_adoption(std::move(adoption));
        } catch (const std::bad_alloc &) {
        }
    };

    // The worst state across this process's outputs. A group is only joined
    // when every one of its members is, so any output that is not joined
    // decides the report; the ordering below is by how badly it fails.
    const auto aggregate_barrier =
        [](net::RuntimeBarrierState current,
           show::PresenterBarrierState observed) noexcept {
            const auto mapped = [observed]() noexcept {
                switch (observed) {
                case show::PresenterBarrierState::Unknown:
                    return net::RuntimeBarrierState::Unknown;
                case show::PresenterBarrierState::Unsupported:
                    return net::RuntimeBarrierState::Unsupported;
                case show::PresenterBarrierState::NotJoined:
                    return net::RuntimeBarrierState::NotJoined;
                case show::PresenterBarrierState::Joining:
                    return net::RuntimeBarrierState::Joining;
                case show::PresenterBarrierState::Joined:
                    return net::RuntimeBarrierState::Joined;
                case show::PresenterBarrierState::Left:
                    return net::RuntimeBarrierState::Left;
                case show::PresenterBarrierState::Failed:
                    return net::RuntimeBarrierState::Failed;
                }
                return net::RuntimeBarrierState::Unknown;
            }();
            const auto severity =
                [](net::RuntimeBarrierState state) noexcept -> int {
                switch (state) {
                case net::RuntimeBarrierState::Failed:
                    return 6;
                case net::RuntimeBarrierState::Left:
                    return 5;
                case net::RuntimeBarrierState::Unsupported:
                    return 4;
                case net::RuntimeBarrierState::NotJoined:
                    return 3;
                case net::RuntimeBarrierState::Joining:
                    return 2;
                case net::RuntimeBarrierState::Unknown:
                    return 1;
                case net::RuntimeBarrierState::Joined:
                    return 0;
                }
                return 1;
            };
            return severity(mapped) > severity(current) ? mapped : current;
        };

    const auto refresh_output_sync = [&]() noexcept {
        const auto previous = node_output_sync.barrier;
        net::RuntimeOutputSync sync{};
        sync.barrier = net::RuntimeBarrierState::Joined;
        sync.group = node_sync_policy.group;
        sync.barrier_id = node_sync_policy.barrier;
        sync.expected_outputs =
            static_cast<core::u32>(output_presenters.size());
        if (output_presenters.empty()) {
            sync.barrier = net::RuntimeBarrierState::Unknown;
        }
        for (const NodeOutputPresenter &slot : output_presenters) {
            const show::PresenterSyncReport report =
                slot.presenter->sync_report();
            sync.barrier = aggregate_barrier(sync.barrier, report.barrier);
            if (report.barrier == show::PresenterBarrierState::Joined) {
                ++sync.joined_outputs;
            }
            sync.repeated_frames += report.repeated;
            sync.loss_of_sync_events += report.leave_events;
        }
        sync.presented_frames = presented_frames;
        sync.missed_frames = missed_boundaries;
        sync.late_frames = late_boundaries;
        sync.held_frames = policy_held_boundaries;
        if (previous == net::RuntimeBarrierState::Joined &&
            sync.barrier != net::RuntimeBarrierState::Joined) {
            ++barrier_loss_transitions;
        }
        sync.loss_of_sync_events += barrier_loss_transitions;
        node_output_sync = sync;
    };

    const auto publish_output_sync = [&](bool force) noexcept {
        refresh_output_sync();
        // Publishing takes the session resource lock, so the per-frame path
        // only does it when the state an operator acts on actually changed.
        const bool changed =
            node_output_sync.barrier != published_output_sync.barrier ||
            node_output_sync.group != published_output_sync.group ||
            node_output_sync.barrier_id != published_output_sync.barrier_id ||
            node_output_sync.joined_outputs !=
                published_output_sync.joined_outputs ||
            node_output_sync.expected_outputs !=
                published_output_sync.expected_outputs ||
            node_output_sync.loss_of_sync_events !=
                published_output_sync.loss_of_sync_events;
        if (!force && !changed) {
            return;
        }
        published_output_sync = node_output_sync;
        service.publish_runtime_output_sync(node_output_sync);
    };

    const auto apply_sync_policy = [&]() noexcept {
        node_sync_request = show::PresenterSyncRequest{
            node_sync_policy.requires_physical_sync(), node_sync_policy.group,
            node_sync_policy.barrier};
        for (NodeOutputPresenter &slot : output_presenters) {
            static_cast<void>(slot.presenter->apply_sync(node_sync_request));
        }
        node_sync_request_pending = false;
        publish_output_sync(true);
    };

    while (stop_requested == 0 && !closed) {
        const auto now = Clock::now();
        auto ticked = driver.tick(now);
        if (!ticked) {
            // A stall long enough to invalidate the pacer is a local timing
            // fault, never a reason to present unapproved state. Re-anchor and
            // keep holding whatever the Director last approved.
            if (ticked.error() == show::ShowClockError::TimeOverflow) {
                static_cast<void>(driver.seek(driver.clock().frame(), now));
                continue;
            }
            std::cerr << "node frame pacer refused a time observation (code "
                      << static_cast<unsigned>(ticked.error()) << ")\n";
            return 1;
        }

        const auto report = std::move(ticked).value();
        boundaries += report.boundaries;
        last_outcome = report.last_outcome;
        // The loop wakes more often than the show rate so shutdown stays
        // responsive, so frame accounting only counts on an actual boundary.
        // Counting per iteration would report thousands of missed frames a
        // second on a perfectly healthy node.
        const bool at_boundary = report.boundaries != 0U;
        // More than one boundary crossed by a single wake-up means this
        // process missed a raster deadline it should have hit. That is the
        // one late-frame signal available without a scan-out timestamp, and
        // it is reported as such rather than being folded into "held".
        if (report.boundaries > 1U) {
            late_boundaries += report.boundaries - 1U;
        }

        std::size_t skipped = 0U;
        show::PublishedFrame incoming{};
        if (driver.mailbox().consume_latest(incoming, skipped, &retired)) {
            // The outgoing root is parked rather than released here; the ring
            // is drained below, away from the boundary.
            static_cast<void>(retired.retire(std::move(presented.root)));
            presented = std::move(incoming);
            ++presented_frames;
        }

        if (!node_sync_loaded ||
            presented.version.value() != node_sync_version) {
            node_sync_model.reset(presented.root);
            node_sync_version = presented.version.value();
            node_sync_loaded = true;
            const sync::SyncPolicy updated = node_sync_model.evaluate();
            if (updated != node_sync_policy) {
                node_sync_policy = updated;
                node_sync_request_pending = true;
            }
        }

        const bool hardware_changed =
            !node_hardware_loaded ||
            presented.version.value() != node_hardware_version;
        if (hardware_changed) {
            node_hardware_model.reset(presented.root);
            node_hardware_version = presented.version.value();
            node_hardware_loaded = true;
            node_hardware = node_hardware_model.evaluate();
        }

        std::shared_ptr<const net::ClusterMachineInfo> local_inventory;
        bool inventory_changed{};
        if (options.output && now >= next_inventory_check) {
            next_inventory_check = now + std::chrono::seconds{1};
            local_inventory = local_hardware_probe.snapshot();
            const core::u64 generation =
                local_inventory != nullptr ? local_inventory->sample_generation
                                           : 0U;
            inventory_changed = generation != local_inventory_generation;
            local_inventory_generation = generation;
        }
        if (hardware_changed || inventory_changed) {
            if (options.output) {
                if (local_inventory == nullptr) {
                    local_inventory = local_hardware_probe.snapshot();
                }
                OutputDeployment wanted = resolve_output_deployment(
                    options, node_hardware, local_inventory.get());
                if (!wanted.ready()) {
                    for (const auto &problem : wanted.issues) {
                        std::cerr << "output deployment for node "
                                  << options.node_index << ": "
                                  << problem.message << '\n';
                    }
                    output_presenters.clear();
                    active_deployment = std::move(wanted);
                    node_deployment_runtime_ready = false;
                    node_feed_bindings_runtime_ready = false;
                    node_sync_request_pending = true;
                    publish_runtime_adoption();
                } else if (wanted != active_deployment) {
                    std::vector<NodeOutputPresenter> replacements;
                    bool created_all = true;
                    try {
                        replacements.reserve(wanted.outputs.size());
                    } catch (const std::bad_alloc &) {
                        created_all = false;
                    }
                    for (const DeployedOutput &deployed : wanted.outputs) {
                        if (!created_all) {
                            break;
                        }
                        const PlaybackOptions applied =
                            presenter_options(options, deployed.window);
                        auto replacement = make_presenter(
                            applied,
                            "media-server node " +
                                std::to_string(options.node_index) +
                                " output " +
                                std::to_string(deployed.logical_port) +
                                " feed " + std::to_string(deployed.feed_index),
                            false, &deployed.window);
                        if (replacement == nullptr) {
                            created_all = false;
                            break;
                        }
                        replacements.push_back(NodeOutputPresenter{
                            deployed, std::move(replacement)});
                    }
                    if (!created_all) {
                        // Never retain an old physical binding after the
                        // project requests another target or mode. A black/
                        // absent output is safer than driving the wrong wall.
                        output_presenters.clear();
                        node_deployment_runtime_ready = false;
                        node_feed_bindings_runtime_ready = false;
                        node_sync_request_pending = true;
                        std::cerr << "could not apply the complete output "
                                     "deployment for node "
                                  << options.node_index << '\n';
                    } else {
                        output_presenters = std::move(replacements);
                        active_deployment = std::move(wanted);
                        node_deployment_runtime_ready = true;
                        // A new swapchain is not a barrier member. Rejoin on
                        // the next tick rather than assuming the replacement
                        // inherited the old window's membership.
                        node_sync_request_pending = true;
                        // Explicit feed references are verified against the
                        // evaluated Feed Map later in this same render tick.
                        node_feed_bindings_runtime_ready =
                            active_deployment.legacy_single_output;
                    }
                    publish_runtime_adoption();
                }
            } else {
                const auto wanted = presenter_window_update(
                    options, node_hardware,
                    node_hardware_model.counters().rejected);
                if (!wanted.has_value()) {
                    std::cerr << "retained output window because authored "
                                 "hardware for node "
                              << options.node_index
                              << " is rejected or ambiguous\n";
                } else if (*wanted != active_presenter_settings) {
                    auto replacement =
                        make_presenter(presenter_options(options, *wanted),
                                       "media-server node " +
                                           std::to_string(options.node_index),
                                       false, &*wanted);
                    if (replacement == nullptr) {
                        std::cerr << "could not apply authored output window "
                                     "configuration for node "
                                  << options.node_index << '\n';
                    } else {
                        output_presenters.front().presenter =
                            std::move(replacement);
                        output_presenters.front().output.window = *wanted;
                        active_presenter_settings = *wanted;
                        node_sync_request_pending = true;
                    }
                }
            }
        }

        const auto transport = presented.transport;
        show::TestPatternInput input{};
        // The position was derived from the anchor at the exact control frame
        // that carried it, so the two can never be a mismatched pair.
        input.show_frame =
            transport ? presented.show_frame : options.first_frame;
        input.control_frame = presented.control_frame;
        input.rate = transport ? transport->rate : options.rate;
        input.state =
            transport ? transport->state : show::TransportState::Stopped;
        input.node_index = options.node_index;
        input.held = report.held;
        input.needs_resync =
            report.last_outcome == show::FrameCommitOutcome::NeedsResync;
        if (transport && driver.config().clock.rate != transport->rate) {
            if (!driver.reconfigure_rate(transport->rate, now)) {
                std::cerr << "node frame pacer refused the replicated show "
                             "rate\n";
                return 1;
            }
            interval = status_interval(options, transport->rate);
            since_status = 0U;
        }
        if (options.output && transport.has_value()) {
            const OutputPlaybackBuffer::Selection buffered =
                output_playback_buffer.select(
                    *transport, presented.control_frame, at_boundary);
            input.control_frame = buffered.control_frame;
            input.show_frame = buffered.show_frame;
        }

        // A node draws its slice of the replicated scene through its feed map,
        // or draws the raw slice, or shows the test pattern - decided once at
        // start-up by which renderers the presenter was built with. A node that
        // could not build the path it was asked for never reaches here:
        // make_presenter refuses it. The output path implies the scene path,
        // which is the source it transforms.
        const bool render_output = options.output;
        const bool render_scene = options.scene;
        // Source readiness is checked against the exact authored bytes. A
        // later proxy resolution may replace only the evaluated runtime path;
        // the document path and persistent asset id are never rewritten.
        if (render_scene || render_output) {
            const bool document_changed =
                !node_assets_loaded ||
                presented.version.value() != node_asset_version;
            bool digest_changed = false;
            bool rechecked = false;
            if (document_changed) {
                node_asset_model.reset(presented.root);
                node_asset_registry = node_asset_model.evaluate();
                const core::u64 registry_digest =
                    assets::digest(node_asset_registry);
                if (!node_assets_loaded ||
                    registry_digest != node_asset_registry_digest) {
                    // Proxy policy and requested profile are authored asset
                    // state but intentionally do not change the content
                    // manifest. Clear only session materializations whenever
                    // that complete registry identity changes.
                    node_proxy_playback.reset();
                    node_asset_registry_digest = registry_digest;
                    if (options.output && node_proxy_generator != nullptr) {
                        for (const assets::Asset &entry :
                             node_asset_registry.assets) {
                            const auto *const media =
                                std::get_if<assets::MediaAsset>(&entry);
                            if (media != nullptr) {
                                node_proxy_generator->request(*media);
                            }
                        }
                    }
                    next_node_media_status_refresh = Clock::time_point::min();
                }
                const ProjectAssetManifest manifest =
                    collect_project_asset_manifest(presented.root,
                                                   node_asset_registry);
                digest_changed = !node_assets_loaded ||
                                 manifest.digest != node_asset_manifest_digest;
                if (digest_changed) {
                    node_asset_manifest_digest = manifest.digest;
                    node_asset_availability = scan_project_assets(manifest);
                    node_media_status_count = 0U;
                    if (options.output) {
                        node_decode_support.replace(manifest.digest,
                                                    node_asset_registry);
                    }
                    next_node_media_status_refresh = Clock::time_point::min();
                    rechecked = true;
                }
                node_asset_version = presented.version.value();
                node_assets_loaded = true;
            }
            if (rechecked) {
                const bool ready = node_asset_availability.ready();
                const bool readiness_changed =
                    ready != node_asset_runtime_ready;
                node_asset_runtime_ready = ready;
                // The report carries the digest it answers, so a new manifest
                // has to be republished even when this node was already ready:
                // otherwise the Director keeps comparing against the digest
                // this node last named and the Program gate never opens.
                if (digest_changed || readiness_changed) {
                    publish_runtime_adoption();
                }
                // One line per actual change, not per retry. The ongoing state
                // belongs on the Output Monitor, not in a five-second log.
                if (!ready && (digest_changed || readiness_changed)) {
                    for (const ProjectAssetPresence &presence :
                         node_asset_availability.assets) {
                        if (!presence.present) {
                            std::cerr << "node " << options.node_index
                                      << " cannot open asset "
                                      << presence.source << '\n';
                        }
                    }
                }
            }
            if (options.output && node_assets_loaded &&
                now >= next_node_media_status_refresh) {
                std::array<net::RuntimeMediaAssetStatus,
                           net::max_runtime_media_assets>
                    refreshed{};
                std::size_t refreshed_count{};
                if (build_node_media_status(
                        node_asset_registry, node_asset_availability,
                        node_proxy_artifacts.get(), node_proxy_generator.get(),
                        node_decode_support, node_asset_manifest_digest,
                        refreshed, refreshed_count)) {
                    const bool changed =
                        refreshed_count != node_media_status_count ||
                        !std::equal(
                            refreshed.begin(),
                            refreshed.begin() +
                                static_cast<std::ptrdiff_t>(refreshed_count),
                            node_media_status.begin());
                    if (changed) {
                        node_media_status = refreshed;
                        node_media_status_count = refreshed_count;
                        publish_runtime_adoption();
                    }
                }
                next_node_media_status_refresh =
                    now + library_proxy_retry_interval;
            }
        }
        scene::Scene scene_frame;
        core::u64 authored_scene_digest{};
        scene::Scene transition_from_frame;
        scene::Scene transition_to_frame;
        // Compositor graphs are scene-scoped, so a dissolve mixes two pictures
        // that were each composited through their own graph.
        compositor::CompositorGraph compositor_graph;
        compositor::CompositorGraph transition_from_graph;
        compositor::CompositorGraph transition_to_graph;
        show::SwitcherProgress switcher_progress{};
        output::FeedMap feed_map;
        show::PresenterStatus status{show::PresenterStatus::Skipped};
        bool output_frame_attempted{};
        bool output_frame_complete{};
        // Barrier membership is applied before this boundary presents, so a
        // policy change or a rebuilt swapchain never spends a frame silently
        // outside the group it is supposed to be in.
        if (node_sync_request_pending) {
            apply_sync_policy();
        }
        if (render_scene) {
            if (!scene_loaded || presented.version.value() != scene_version) {
                scene_model.reset(presented.root);
                scene_version = presented.version.value();
                scene_loaded = true;
            }
            if (!compositor_loaded ||
                presented.version.value() != compositor_version) {
                compositor_model.reset(presented.root);
                compositor_version = presented.version.value();
                compositor_loaded = true;
            }
            const scene::SceneTime time{input.show_frame.value(),
                                        input.rate.numerator,
                                        input.rate.denominator};
            // The show's repeat, so a clip still on air across the wrap has the
            // frames it lands on decoded before it gets there. The range rides
            // the transport anchor, so this node warms exactly what the
            // Director does, on the same frame.
            const scene::ShowLoop show_loop =
                transport ? scene::ShowLoop{transport->first_frame.value(),
                                            transport->last_frame.value(),
                                            transport->loop}
                          : scene::ShowLoop{};
            // A graph object authored before graphs were scene-scoped carries
            // no owner. It used to apply to whatever Program was showing, so
            // Program is the scene it is adopted into. Both the Director and
            // every node resolve it the same way, from the same document.
            const auto node_scenes = scene_model.scenes();
            const auto switcher = show::read_switcher(presented.root);
            const scene::SceneId legacy_graph_owner =
                switcher.has_value()
                    ? switcher->program_scene
                    : (node_scenes.empty() ? scene::SceneId{}
                                           : node_scenes.front().id);
            if (switcher.has_value()) {
                switcher_progress = show::derive_switcher_progress(
                    *switcher, input.control_frame);
                scene_frame = scene_model.evaluate(switcher->program_scene,
                                                   time, show_loop);
                compositor_graph = compositor_model.evaluate(
                    switcher->program_scene, legacy_graph_owner);
                if (switcher_progress.active) {
                    transition_from_frame = scene_model.evaluate(
                        switcher_progress.from, time, show_loop);
                    transition_to_frame = scene_model.evaluate(
                        switcher_progress.to, time, show_loop);
                    transition_from_graph = compositor_model.evaluate(
                        switcher_progress.from, legacy_graph_owner);
                    transition_to_graph = compositor_model.evaluate(
                        switcher_progress.to, legacy_graph_owner);
                    // Older documents may contain a dissolve authored before
                    // canvas compatibility was enforced. Never hand an
                    // impossible pair to Presenter; promote Program as a cut.
                    if (!show::dissolve_canvases_compatible(
                            transition_from_frame.canvas,
                            transition_to_frame.canvas)) {
                        switcher_progress.active = false;
                    }
                }
            } else {
                scene_frame = scene_model.evaluate(time, show_loop);
                compositor_graph = compositor_model.evaluate(
                    scene_frame.id, legacy_graph_owner);
            }

            authored_scene_digest = scene::digest(scene_frame);

            std::vector<ProxyPlacementObservation> observations;
            bool proxies_ready = resolve_scene_proxies(
                node_proxy_playback, scene_frame, node_asset_registry,
                node_asset_availability, observations);
            if (switcher_progress.active) {
                proxies_ready = resolve_scene_proxies(
                                    node_proxy_playback, transition_from_frame,
                                    node_asset_registry,
                                    node_asset_availability, observations) &&
                                proxies_ready;
                proxies_ready = resolve_scene_proxies(
                                    node_proxy_playback, transition_to_frame,
                                    node_asset_registry,
                                    node_asset_availability, observations) &&
                                proxies_ready;
            }
            report_proxy_status_changes("node", observations,
                                        node_proxy_observations);
            node_proxy_observations = std::move(observations);
            if (proxies_ready != node_proxy_runtime_ready) {
                node_proxy_runtime_ready = proxies_ready;
                publish_runtime_adoption();
            }
        }

        if (render_output) {
            if (!feed_loaded || presented.version.value() != feed_version) {
                feed_model.reset(presented.root);
                feed_version = presented.version.value();
                feed_loaded = true;
            }
            feed_map = feed_model.evaluate(
                scene::SceneTime{input.show_frame.value(), input.rate.numerator,
                                 input.rate.denominator});
            const bool feeds_ready =
                active_deployment.legacy_single_output ||
                std::all_of(
                    active_deployment.outputs.begin(),
                    active_deployment.outputs.end(),
                    [&](const DeployedOutput &deployed) {
                        return !feed_map.maps_for(deployed.feed_index).empty();
                    });
            if (feeds_ready != node_feed_bindings_runtime_ready) {
                node_feed_bindings_runtime_ready = feeds_ready;
                if (!feeds_ready) {
                    std::cerr << "output deployment for node "
                              << options.node_index
                              << " references a missing Feed Map index\n";
                }
                publish_runtime_adoption();
            }
            // The group late-frame rule. When one output of this process has
            // no complete frame, HOLD GROUP repeats the previous complete
            // frame on every output instead of advancing the rest: on an LED
            // wall one repeated frame everywhere is far less visible than one
            // segment moving while its neighbour stands still.
            //
            // This holds the outputs one render process owns. Holding across
            // machines is what the present barrier and the existing frame
            // barrier are for; neither is replaced by this rule.
            const bool outputs_complete = std::all_of(
                output_presenters.begin(), output_presenters.end(),
                [&](const NodeOutputPresenter &slot) {
                    return !feed_map.maps_for(slot.output.feed_index).empty();
                });
            const bool hold_group =
                !outputs_complete &&
                node_sync_policy.late_frame == sync::LateFramePolicy::HoldGroup;
            status = show::PresenterStatus::Skipped;
            if (hold_group) {
                if (at_boundary) {
                    policy_held_boundaries += report.boundaries;
                    missed_boundaries += report.boundaries;
                }
                for (NodeOutputPresenter &slot : output_presenters) {
                    if (slot.presenter->hold() ==
                        show::PresenterStatus::Closed) {
                        status = show::PresenterStatus::Closed;
                        break;
                    }
                }
            } else {
                output_frame_attempted = !output_presenters.empty();
                bool media_deferred = false;
                for (NodeOutputPresenter &slot : output_presenters) {
                    const auto maps = feed_map.maps_for(slot.output.feed_index);
                    show::PresenterStatus slot_status =
                        show::PresenterStatus::Skipped;
                    if (!maps.empty() && switcher_progress.active) {
                        slot_status = slot.presenter->present(
                            transition_from_frame, transition_to_frame,
                            static_cast<float>(switcher_progress.mix()),
                            transition_from_graph, transition_to_graph, maps);
                    } else if (!maps.empty()) {
                        slot_status = slot.presenter->present(
                            scene_frame, compositor_graph, maps);
                    }
                    if (slot_status == show::PresenterStatus::Closed) {
                        status = slot_status;
                        break;
                    }
                    if (slot_status == show::PresenterStatus::Deferred) {
                        media_deferred = true;
                        continue;
                    }
                    if (slot_status == show::PresenterStatus::Failed ||
                        (slot_status == show::PresenterStatus::Presented &&
                         status == show::PresenterStatus::Skipped)) {
                        status = slot_status;
                    }
                }
                if (media_deferred && status != show::PresenterStatus::Closed &&
                    status != show::PresenterStatus::Failed) {
                    status = show::PresenterStatus::Deferred;
                }
                output_frame_complete =
                    output_frame_attempted &&
                    (status == show::PresenterStatus::Presented ||
                     status == show::PresenterStatus::Skipped);
            }
        } else if (render_scene) {
            // The node's own rectangle of the canvas, from the document. A node
            // the feed map does not bind draws the whole canvas rather than
            // guessing.
            const auto *const bound = scene_frame.slice_for(options.node_index);
            const auto slice =
                bound != nullptr ? *bound : scene::whole(scene_frame.canvas);
            show::Presenter &scene_presenter =
                *output_presenters.front().presenter;
            status =
                switcher_progress.active
                    ? scene_presenter.present(
                          transition_from_frame, transition_to_frame,
                          static_cast<float>(switcher_progress.mix()),
                          transition_from_graph, transition_to_graph, slice)
                    : scene_presenter.present(scene_frame, compositor_graph,
                                              slice);
        } else {
            status = output_presenters.front().presenter->present(input);
        }

        switch (status) {
        case show::PresenterStatus::Closed:
            node_deployment_runtime_ready = false;
            publish_runtime_adoption();
            closed = true;
            break;
        case show::PresenterStatus::Failed:
            ++present_failures;
            if (at_boundary) {
                missed_boundaries += report.boundaries;
            }
            break;
        case show::PresenterStatus::Deferred:
            if (at_boundary) {
                missed_boundaries += report.boundaries;
            }
            break;
        case show::PresenterStatus::Presented:
        case show::PresenterStatus::Skipped:
            break;
        }
        if (render_output && transport.has_value()) {
            output_playback_buffer.observe(output_frame_complete);
        }

        // Only on a real boundary: a vendor barrier query is a driver call,
        // and this loop wakes far more often than the show rate. Only a
        // changed barrier state then takes the session lock; the counters
        // ride along at the status cadence below.
        if (at_boundary) {
            publish_output_sync(false);
        }

        since_status += report.boundaries;
        if (since_status >= interval) {
            since_status = 0U;
            publish_output_sync(true);
            std::cout
                << "node " << options.node_index
                << " show-frame=" << input.show_frame.value()
                << " control-frame=" << presented.control_frame.value()
                << " output-control-frame=" << input.control_frame.value()
                << " output-buffer-lag="
                << (render_output ? output_playback_buffer.latency_frames(
                                        presented.control_frame)
                                  : 0U)
                << " version=" << presented.version.value()
                << " transport=" << transport_label(input.state)
                << " state=" << outcome_label(report.last_outcome)
                << " boundaries=" << boundaries
                << " presented=" << presented_frames
                << " held=" << driver.counters().held_boundaries
                << " skipped=" << skipped << " overflows="
                << driver.counters().mailbox_overflows
                // The scene digest compares the replicated state itself,
                // not a rendering of it, so it agrees across nodes drawing
                // different slices - which the frame-buffer digest cannot.
                << " digest="
                << (render_scene
                        ? authored_scene_digest
                        : show::frame_buffer_digest(
                              output_presenters.front().presenter->surface()));
            // The feed-map digest agrees across nodes for the same reason the
            // scene one does: it is the replicated parameters, not a rendering.
            if (render_output) {
                std::cout << " feed=" << output::digest(feed_map);
            }
            if (!output_presenters.empty()) {
                std::cout << gpu_timing_text(
                    output_presenters.front().presenter->timing());
            }
            std::cout << " sync=" << sync_mode_label(node_sync_policy.mode)
                      << " barrier="
                      << runtime_barrier_label(node_output_sync.barrier) << '/'
                      << node_output_sync.joined_outputs << 'x'
                      << node_output_sync.expected_outputs
                      << " missed=" << node_output_sync.missed_frames
                      << " late=" << node_output_sync.late_frames
                      << " policy-held=" << node_output_sync.held_frames
                      << " repeated=" << node_output_sync.repeated_frames
                      << " sync-loss=" << node_output_sync.loss_of_sync_events;
            std::cout << '\n' << std::flush;
        }

        // Off-boundary housekeeping: this is the only place a superseded trie
        // is actually released.
        static_cast<void>(retired.drain());

        if (options.run_frames != 0U && boundaries >= options.run_frames) {
            break;
        }
        sleep_until_boundary(driver.clock().next_boundary_at());
    }

    std::cout << "node " << options.node_index << " stopping"
              << " show-frame="
              << (presented.transport ? presented.show_frame.value()
                                      : options.first_frame.value())
              << " control-frame=" << presented.control_frame.value()
              << " version=" << presented.version.value()
              << " boundaries=" << boundaries
              << " presented=" << presented_frames
              << " committed=" << driver.counters().committed_frames
              << " held=" << driver.counters().held_boundaries
              << " checkpoints=" << driver.counters().checkpoint_activations
              << " present-failures=" << present_failures
              << " outputs=" << output_presenters.size();
    if (!output_presenters.empty()) {
        std::cout << " window="
                  << (output_presenters.front().presenter->windowed()
                          ? "closed"
                          : "headless")
                  << gpu_timing_text(
                         output_presenters.front().presenter->timing());
    }
    std::cout << " last=" << outcome_label(last_outcome) << '\n' << std::flush;
    return 0;
}

int run_director_show(cmd::DirectorScheduler &scheduler,
                      persist::CommandLog &command_log,
                      persist::FrameDurability frame_durability,
                      net::DirectorSessionHub &hub,
                      net::DirectorFrameBarrier &barrier,
                      const net::DirectorSessionService *session_service,
                      const PlaybackOptions &options,
                      const volatile std::sig_atomic_t &stop_requested) {
    // Recovery affects whether either build may author demo fallback content;
    // it is not a GUI-only project-loading concern.
    const bool recovered_session =
        command_log.recovery_report().entry_count != 0U;
#if !defined(MEDIASERVER_HAS_GUI)
    // Project loading is a GUI-only authoring path, but keeping these seams in
    // the common entry point lets both builds use the same durable coordinator.
    static_cast<void>(frame_durability);
#endif
    // The editor replaces the preview presenter when asked for: it is the
    // operator's surface, not an output window. Everything else - the seal
    // loop, the transport, replication - is identical either way.
    std::unique_ptr<show::Presenter> presenter;
    std::string clip_source;
    std::unique_ptr<server::ProxyGenerationService> director_proxy_artifacts;
    if (options.scene || options.output || options.edit) {
        director_proxy_artifacts =
            open_proxy_playback_artifacts(options, "Director");
    }
    std::unique_ptr<ProxyGenerationWorker> director_proxy_generator;
    if (options.edit) {
        auto generation_artifacts =
            open_proxy_playback_artifacts(options, "proxy generator");
        if (generation_artifacts != nullptr) {
            director_proxy_generator = std::make_unique<ProxyGenerationWorker>(
                std::move(generation_artifacts));
        }
    }
    ProxyPlaybackService director_proxy_playback{
        director_proxy_artifacts.get()};
    std::vector<ProxyPlacementObservation> director_proxy_observations;
#if defined(MEDIASERVER_HAS_GUI)
    std::unique_ptr<editor::EditorShell> editor;
    std::unique_ptr<ClusterLifecycleController> lifecycle_controller;
    std::unique_ptr<server::ThumbnailService> thumbnail_service;
    std::vector<assets::DerivativeKey> retained_thumbnail_keys;
    std::vector<assets::DerivativeKey> retained_filmstrip_keys;
    std::vector<editor::LibraryProxyRuntimeObservation> library_proxy_runtime;
    auto next_output_media_projection = Clock::time_point::min();
    std::optional<std::filesystem::path> workspace_layout_file;
    persist::WorkspaceLayoutBytes workspace_layout;
    if (options.edit) {
        const auto located_layout = persist::default_workspace_layout_file();
        if (!located_layout) {
            std::cerr << "could not locate workspace layout settings (code "
                      << static_cast<unsigned>(located_layout.error().code)
                      << ")\n";
        } else {
            workspace_layout_file = located_layout.value();
            auto loaded_layout =
                persist::load_workspace_layout(*workspace_layout_file);
            if (!loaded_layout) {
                std::cerr << "could not load workspace layout (code "
                          << static_cast<unsigned>(loaded_layout.error().code)
                          << ')';
                if (loaded_layout.error().system_error) {
                    std::cerr << ": "
                              << loaded_layout.error().system_error.message();
                }
                std::cerr << '\n';
            } else {
                workspace_layout = std::move(loaded_layout).value();
            }
        }
        editor::EditorConfig editor_config{};
        editor_config.title = "media-server editor";
        // Honour an explicit --surface, but keep the editor usably large when
        // the caller left the small test-pattern default in place.
        editor_config.width =
            options.surface_width >= 1024U ? options.surface_width : 1400U;
        editor_config.height =
            options.surface_height >= 640U ? options.surface_height : 900U;
        editor_config.position_x = options.window_x;
        editor_config.position_y = options.window_y;
        editor_config.workspace_layout = workspace_layout;
        auto created_editor = editor::EditorShell::create(editor_config);
        if (!created_editor) {
            std::cerr << "could not open the operator editor window\n";
            return 1;
        }
        editor = std::move(created_editor).value();
        {
            std::error_code cache_path_error;
            const std::filesystem::path cache_root =
                derivative_cache_root(options, cache_path_error) / "thumbnails";
            constexpr core::u64 thumbnail_cache_bytes = 512U * 1'024U * 1'024U;
            if (cache_path_error) {
                std::cerr << "could not locate the thumbnail cache root: "
                          << cache_path_error.message() << '\n';
            } else {
                auto cache =
                    assets::DerivativeCache::open(assets::DerivativeCacheConfig{
                        cache_root, thumbnail_cache_bytes});
                if (!cache) {
                    std::cerr << "could not open the thumbnail cache (code "
                              << static_cast<unsigned>(cache.error().code)
                              << ")\n";
                } else {
                    try {
                        thumbnail_service =
                            std::make_unique<server::ThumbnailService>(
                                std::move(cache).value());
                    } catch (const std::bad_alloc &) {
                        std::cerr << "could not initialize thumbnail workers: "
                                     "allocation failed\n";
                    } catch (const std::system_error &error) {
                        std::cerr << "could not initialize thumbnail workers: "
                                  << error.what() << '\n';
                    }
                }
            }
        }
        try {
            ClusterLifecycleControllerConfig lifecycle_config{};
            lifecycle_config.shared_secret = options.lifecycle_secret;
            lifecycle_controller = std::make_unique<ClusterLifecycleController>(
                std::move(lifecycle_config));
        } catch (const std::bad_alloc &) {
            std::cerr << "could not initialize cluster lifecycle controller: "
                         "allocation failed\n";
            return 1;
        } catch (const std::system_error &error) {
            std::cerr << "could not initialize cluster lifecycle controller: "
                      << error.what() << '\n';
            return 1;
        }
        clip_source = write_demo_clip();
    }
#endif
    if (!options.edit) {
        presenter = make_presenter(options, "media-server director", true);
        if (presenter == nullptr) {
            return 1;
        }
    }

    const auto start = Clock::now();

    // One clock, and it is a pacer. It free-runs regardless of transport state
    // and decides only when the next control frame is due, so pausing never
    // stops the frame stream that carries transport changes and keeps the
    // cohort acknowledging.
    //
    // Where the show is, is not a clock reading at all. It is the replicated
    // anchor evaluated at the control-frame ordinal, which is what makes the
    // Director and every node compute the same position from the same state.
    auto paced = show::ShowClock::create(
        show::ShowClockConfig{
            options.rate, show::ShowFrame{0U},
            show::ShowFrame{std::numeric_limits<core::u64>::max() - 1U}, false},
        start);
    auto created = show::TransportAuthority::create(clock_config(options),
                                                    scheduler.sealed_through());
    if (!paced || !created) {
        std::cerr << "invalid Director show clock configuration\n";
        return 1;
    }
    auto pacer = std::move(paced).value();
    auto transport = std::move(created).value();
    bool restored_recovered_transport = false;

    if (!pacer.play(start)) {
        std::cerr << "could not start the Director frame pacer\n";
        return 1;
    }
#if defined(MEDIASERVER_HAS_GUI)
    if (options.edit && recovered_session) {
        const doc::Node *const raw_transport =
            scheduler.projected_document().get(show::transport_node_id);
        const auto recovered_transport =
            show::read_transport(scheduler.projected_document());
        if (raw_transport != nullptr &&
            std::string_view{raw_transport->type} ==
                show::transport_node_type &&
            !recovered_transport.has_value()) {
            std::cerr << "recovered project transport state is invalid\n";
            return 1;
        }
        if (recovered_transport.has_value()) {
            const cmd::FrameNumber through = scheduler.sealed_through();
            if (!restore_project_transport(transport, *recovered_transport,
                                           through) ||
                !pacer.seek(show::ShowFrame{through.value()}, start) ||
                !pacer.reconfigure_rate(recovered_transport->rate, start)) {
                std::cerr << "could not restore recovered project timing\n";
                return 1;
            }
            restored_recovered_transport = true;
        }
    }
#endif

    // The built-in scene and feed map are authored once, as ordinary
    // node-creation commands on the first frame this Director will seal. From
    // there they are document state like any other: they replicate to the
    // cohort, and a playing show adds no further edits because every motion is
    // a rate the nodes integrate against the show frame themselves, and the
    // feed map does not move at all.
    if (options.scene || options.output || options.edit) {
        const cmd::FrameNumber first{scheduler.sealed_through().value() + 1U};
        if (options.scene && !author_scene_document(scheduler, first)) {
            std::cerr << "could not author the built-in scene document\n";
            return 1;
        }
        const InitialFeedMapPlan feed_map_plan =
            initial_feed_map_plan(options, recovered_session);
        if (feed_map_plan.kind != InitialFeedMapKind::None &&
            !author_feed_map_document(scheduler, first, feed_map_plan)) {
            std::cerr << "could not author the built-in feed map document\n";
            return 1;
        }
#if defined(MEDIASERVER_HAS_GUI)
        if (options.edit && !recovered_session &&
            !author_edit_document(scheduler, first, clip_source)) {
            std::cerr << "could not author the edit scene document\n";
            return 1;
        }
#endif
    }

    std::deque<net::DirectorFrameBarrierId> tracked;
    std::vector<show::PresenterCommand> operator_commands;
#if defined(MEDIASERVER_HAS_GUI)
    // Operator moves from the editor, coalesced to the latest per node,
    // authored at the next frame boundary like the transport commands.
    std::vector<editor::EditIntent> pending_edits;
    // A pending scene-history step, authored at the boundary after the edits so
    // it navigates from exactly the state those edits just left behind.
    std::optional<editor::HistoryRequest> pending_history;
    // A pending timeline scrub: the show frame the operator dragged the
    // playhead to, applied to the transport (paused) at the boundary.
    std::optional<core::u64> pending_seek;
    // The same, for Preview's local playhead. Kept separate all the way to the
    // boundary so a scrub authored while rehearsing can never reach the show.
    std::optional<core::u64> pending_preview_seek;
    // Transport actions aimed at Preview rather than at the live show.
    std::vector<show::PresenterCommand> preview_commands;
    // A pending show range/loop change from the timeline.
    std::optional<editor::RangeRequest> pending_range;
    // A pending exact project cadence. The transport anchor and the Director
    // pacer change on the same sealed boundary.
    std::optional<editor::ShowRateRequest> pending_show_rate;
    // Pending keyframe requests, authored as scene.channel edits at the
    // boundary.
    std::vector<editor::KeyIntent> pending_keys;
    // Pending keyframe retimes (a diamond dragged to a new time).
    std::vector<editor::KeyMove> pending_key_moves;
    // Alt-drag duplicates are complete key-set operations, planned together at
    // the boundary so every destination shares one transaction and delta.
    std::optional<editor::KeyDuplicateRequest> pending_key_duplicate;
    std::optional<editor::KeyBatchRequest> pending_key_batch;
    std::optional<editor::TimelineKeyScalePreview> key_scale_preview;
    // Clip placements and cuts from the timeline, authored at the boundary
    // like every other edit.
    std::optional<editor::ProjectSettingsEdit> pending_project_settings;
    std::vector<editor::ClipTimingEdit> pending_clip_timings;
    std::vector<editor::ClipSplitIntent> pending_clip_splits;
    std::vector<editor::TimelineLaneEditIntent> pending_lane_edits;
    std::vector<editor::TimelineLayerOrderIntent> pending_layer_orders;
    std::vector<editor::ElementFlagIntent> pending_element_flags;
    // Set by the Library's RESCAN control. Nothing rescans on a timer: on a
    // stage the files move under you constantly and a periodic disk read is a
    // periodic stall, so looking again is something the operator asks for.
    bool pending_asset_rescan = false;
    std::vector<editor::TimelineEditIntent> pending_timeline_edits;
    std::optional<editor::GroupEditIntent> pending_group_edit;
    std::optional<editor::SelectionSetEditIntent> pending_selection_set_edit;
    std::optional<editor::MaskEditIntent> pending_mask_edit;
    struct PendingClipPaste final {
        editor::ClipClipboardRequest request;
        ClipClipboard snapshot;
    };
    ClipClipboard clip_clipboard;
    std::optional<PendingClipPaste> pending_clip_paste;
    struct PendingKeyPaste final {
        editor::KeyClipboardRequest request;
        KeyClipboard snapshot;
    };
    KeyClipboard key_clipboard;
    std::optional<PendingKeyPaste> pending_key_paste;
    // Pending Inspector edits, authored as base-property edits at the boundary.
    std::vector<editor::PropertyEdit> pending_properties;
    // Pending Inspector edits of named properties (light fields).
    std::vector<editor::GenericEdit> pending_generics;
    // Selection locks stay authorable during a dissolve because they do not
    // alter either rendered scene.
    std::vector<editor::LockEdit> pending_locks;
    // Pending element creations and deletions from the editor's Add/Edit menus.
    std::vector<editor::AddIntent> pending_adds;
    std::vector<editor::AssetImportIntent> pending_asset_imports;
    std::vector<editor::AssetDeleteIntent> pending_asset_deletes;
    std::vector<editor::AssetProxyEditIntent> pending_asset_proxy_edits;
    std::vector<editor::EffectIntent> pending_effects;
    std::vector<editor::FilmGrainEffectEditIntent>
        pending_film_grain_effect_edits;
    std::vector<editor::EffectDeleteIntent> pending_effect_deletes;
    std::optional<editor::SwitcherRequest> pending_switcher;
    std::vector<editor::HardwarePlaceRequest> pending_hardware_places;
    std::vector<editor::HardwareMoveRequest> pending_hardware_moves;
    std::vector<editor::HardwareLinkRequest> pending_hardware_links;
    std::vector<editor::HardwarePropertyEdit> pending_hardware_properties;
    std::optional<editor::HardwareControlPortEdit>
        pending_hardware_control_port;
    std::optional<editor::HardwareSyncPolicyEdit> pending_sync_policy;
    std::vector<editor::HardwareOutputBindingEdit>
        pending_hardware_output_binding_edits;
    std::vector<editor::HardwareOutputBindingDelete>
        pending_hardware_output_binding_deletes;
    std::vector<editor::HardwareDeleteRequest> pending_hardware_deletes;
    std::vector<editor::FeedMapAddRequest> pending_feed_map_adds;
    std::vector<editor::FeedMapRectEdit> pending_feed_map_edits;
    std::vector<editor::FeedMapDeleteRequest> pending_feed_map_deletes;
    std::vector<editor::FeedProjectionEdit> pending_feed_projection_edits;
    std::vector<editor::FeedSurfaceAddRequest> pending_feed_surface_adds;
    std::vector<editor::FeedSurfaceEditRequest> pending_feed_surface_edits;
    std::vector<editor::FeedSurfaceDeleteRequest> pending_feed_surface_deletes;
    std::optional<editor::CompositorAddIntent> pending_compositor_add;
    std::optional<editor::CompositorMoveIntent> pending_compositor_move;
    std::optional<editor::CompositorConnectIntent> pending_compositor_connect;
    std::optional<editor::CompositorPropertyEditIntent>
        pending_compositor_property;
    std::optional<editor::CompositorDeleteIntent> pending_compositor_delete;
    core::u64 pending_compositor_gesture{};
    std::vector<doc::NodeId> pending_deletes;
    core::u64 pending_delete_gesture{};
    std::optional<editor::ProjectFileRequest> pending_project_save;
    std::optional<editor::ProjectFileRequest> pending_project_load;
    EditCoalescingPolicy editor_coalescing_policy;
    std::map<core::u64, EditCoalescingPlan> pending_editor_coalescing;
    bool pending_editor_coalescing_close{};
    const auto coalescing_for = [&](core::u64 gesture) -> EditCoalescingPlan * {
        const auto found = pending_editor_coalescing.find(gesture);
        return found == pending_editor_coalescing.end() ? nullptr
                                                        : &found->second;
    };
    // Startup validates the recent project before the frame loop, then hands
    // the already-decoded root to the ordinary frame-boundary staging path.
    // Manual Open requests leave this empty and continue to read at the
    // boundary, so both paths share transport/switcher/WAL validation.
    std::optional<doc::Root> pending_preloaded_project;
    std::optional<std::filesystem::path> recent_project_settings_file;
    std::string project_path;
    std::string project_message = recovered_session
                                      ? std::string{"RECOVERED UNSAVED CHANGES"}
                                      : std::string{};
    doc::Root saved_project_snapshot = scheduler.projected_document();
    // Without a verified saved-file baseline, a recovered WAL is an unsaved
    // untitled session.
    bool project_explicitly_dirty{recovered_session};
    core::u64 project_generation{};
    const auto report_recent_project_error =
        [](std::string_view operation, const persist::SettingsError &error) {
            std::cerr << operation << " recent-project setting (code "
                      << static_cast<unsigned>(error.code) << ')';
            if (error.system_error) {
                std::cerr << ": " << error.system_error.message();
            }
            std::cerr << '\n';
        };
    // A recovered Director WAL can contain edits made after the last explicit
    // project save. It is the newer, lossless source of truth, so never replace
    // it merely because a recent-project preference exists. The default WAL
    // may recover its path only when the saved content is identical; an
    // explicit WAL never borrows the global recent path. A fresh log may safely
    // stage the remembered project.
    if (options.edit) {
        const auto settings_file =
            persist::default_recent_project_settings_file();
        if (!settings_file) {
            report_recent_project_error("could not locate",
                                        settings_file.error());
        } else {
            recent_project_settings_file = settings_file.value();
            auto recent =
                persist::open_recent_project(*recent_project_settings_file);
            if (!recent) {
                if (recent.error().settings_error.has_value()) {
                    report_recent_project_error("could not read",
                                                *recent.error().settings_error);
                } else if (recent.error().project_error.has_value()) {
                    const auto &error = *recent.error().project_error;
                    std::cerr << "recent project is unavailable or invalid "
                                 "(code "
                              << static_cast<unsigned>(error.code) << ')';
                    if (error.system_error) {
                        std::cerr << ": " << error.system_error.message();
                    }
                    std::cerr << '\n';
                }
            } else if (recent.value().has_value()) {
                auto candidate = std::move(*recent.value());
                if (!recovered_session) {
                    pending_project_load = editor::ProjectFileRequest{
                        native_path_to_utf8(candidate.path)};
                    pending_preloaded_project = std::move(candidate.project);
                    project_message = "REOPENING PROJECT...";
                } else if (options.associate_recent_project_after_recovery &&
                           recovered_project_matches_saved_baseline(
                               candidate.project,
                               scheduler.projected_document())) {
                    // The recovered document agrees with the complete saved
                    // project content (apart from transient transport state),
                    // so this is a safe baseline and ordinary Ctrl+S target.
                    project_path = native_path_to_utf8(candidate.path);
                    saved_project_snapshot = std::move(candidate.project);
                    project_explicitly_dirty = false;
                    project_message = "RECOVERED PROJECT";
                    std::cerr << "recovered Director session matched the "
                                 "recent saved-project baseline\n";
                } else {
                    // A global recent pointer is not proof that a recovered WAL
                    // belongs to that file: the last recent-setting update may
                    // have failed after a successful save of another project.
                    // Keep recovery untitled and dirty so Ctrl+S cannot
                    // overwrite an unrelated project.
                    std::cerr << "recovered WAL left untitled; recent-project "
                                 "content/ownership was not verified\n";
                }
            }
        }
    }
    if (options.edit && recovered_session) {
        try {
            MediaRateEnrichment enrichment =
                enrich_media_frame_rates(scheduler.projected_document());
            if (enrichment.updated_nodes != 0U) {
                const cmd::FrameNumber at{scheduler.sealed_through().value() +
                                          1U};
                auto scheduled = scheduler.schedule_document_replace(
                    enrichment.project, cmd::UndoContextId{7U},
                    cmd::TransactionId{at.value()}, at);
                if (scheduled) {
                    project_explicitly_dirty = true;
                    project_message =
                        "RECOVERED PROJECT - MEDIA FPS UPDATED (" +
                        std::to_string(enrichment.updated_nodes) + ')';
                    std::cerr << "enriched " << enrichment.updated_nodes
                              << " recovered media node(s) with native FPS; "
                                 "save the project to persist metadata\n";
                } else {
                    std::cerr << "could not schedule recovered media FPS "
                                 "metadata enrichment\n";
                }
            }
        } catch (const std::bad_alloc &) {
            std::cerr << "could not enrich recovered media FPS metadata: "
                         "allocation failed\n";
        } catch (const std::length_error &) {
            std::cerr << "could not enrich recovered media FPS metadata: "
                         "project limits exceeded\n";
        }
    }
    const auto remember_recent_project = [&recent_project_settings_file,
                                          &report_recent_project_error](
                                             std::string_view path) {
        if (!recent_project_settings_file.has_value()) {
            return;
        }
        auto remembered = persist::save_recent_project_path(
            *recent_project_settings_file, native_path_from_utf8(path));
        if (!remembered && !remembered.error().committed) {
            report_recent_project_error("could not update", remembered.error());
        } else if (!remembered) {
            report_recent_project_error("updated without confirmed durability",
                                        remembered.error());
        }
    };
    // The next id to try for a created element; advanced past any occupied id
    // so an add never collides with an existing node (content, channel, or
    // demo).
    core::u64 next_new_node{100'000U};
    const auto clear_pending_project_authoring = [&]() {
        pending_edits.clear();
        pending_history.reset();
        pending_seek.reset();
        pending_preview_seek.reset();
        preview_commands.clear();
        pending_range.reset();
        pending_show_rate.reset();
        pending_keys.clear();
        pending_key_moves.clear();
        pending_key_duplicate.reset();
        pending_key_batch.reset();
        key_scale_preview.reset();
        pending_clip_timings.clear();
        pending_clip_splits.clear();
        pending_lane_edits.clear();
        pending_layer_orders.clear();
        pending_timeline_edits.clear();
        pending_group_edit.reset();
        pending_selection_set_edit.reset();
        pending_mask_edit.reset();
        pending_clip_paste.reset();
        clip_clipboard.clear();
        pending_key_paste.reset();
        key_clipboard.clear();
        pending_project_settings.reset();
        pending_properties.clear();
        pending_generics.clear();
        pending_locks.clear();
        pending_adds.clear();
        pending_asset_imports.clear();
        pending_asset_deletes.clear();
        pending_asset_proxy_edits.clear();
        pending_effects.clear();
        pending_film_grain_effect_edits.clear();
        pending_effect_deletes.clear();
        pending_switcher.reset();
        pending_hardware_places.clear();
        pending_hardware_moves.clear();
        pending_hardware_links.clear();
        pending_hardware_properties.clear();
        pending_hardware_control_port.reset();
        pending_sync_policy.reset();
        pending_hardware_output_binding_edits.clear();
        pending_hardware_output_binding_deletes.clear();
        pending_hardware_deletes.clear();
        pending_feed_map_adds.clear();
        pending_feed_map_edits.clear();
        pending_feed_map_deletes.clear();
        pending_feed_projection_edits.clear();
        pending_feed_surface_adds.clear();
        pending_feed_surface_edits.clear();
        pending_feed_surface_deletes.clear();
        pending_compositor_add.reset();
        pending_compositor_move.reset();
        pending_compositor_connect.reset();
        pending_compositor_property.reset();
        pending_compositor_delete.reset();
        pending_compositor_gesture = 0U;
        pending_deletes.clear();
        pending_delete_gesture = 0U;
        pending_editor_coalescing.clear();
        pending_editor_coalescing_close = false;
        editor_coalescing_policy.close();
        scheduler.end_coalescing();
    };
#endif
    bool rehearsal_mode{};
    // Preview's playhead. Program's transport is replicated document state
    // every render node derives its frame from; Preview's is Director-local
    // and authored nowhere, which is the whole point: an operator scrubbing,
    // playing, or stepping through the scene they are rehearsing must not move
    // the show. It shares the live range, loop, and rate, because those are one
    // show-wide setting rather than a property of the scene being edited.
    std::optional<show::TransportAuthority> preview_transport;
#if defined(MEDIASERVER_HAS_GUI)
    // Entering Preview seeds its playhead from wherever the show currently is
    // and freezes it there, so the operator starts from the picture they were
    // just looking at and the show carries on without them.
    const auto enter_preview = [&](cmd::FrameNumber at) {
        rehearsal_mode = true;
        show::TransportAuthority seeded = transport;
        static_cast<void>(seeded.pause(at));
        preview_transport = seeded;
    };
    const auto leave_preview = [&]() noexcept {
        rehearsal_mode = false;
        preview_transport.reset();
    };
#endif
    std::optional<show::TransportAnchor> authored;
    auto interval = status_interval(options, transport.anchor().rate);
    core::u64 sealed = 0U;
    core::u64 since_status = 0U;
    core::u64 anchors = 0U;
    core::u64 present_failures = 0U;
    core::u32 configured_cluster_nodes = 0U;
    bool cluster_configuration_authored = false;
    ClusterDiagnosticLog cluster_diagnostics{
        cluster_diagnostics_path(command_log.path())};
    if (!cluster_diagnostics.available()) {
        std::cerr << "cluster diagnostics unavailable at "
                  << cluster_diagnostics.path().string() << '\n';
    }
    // The latest evaluated physical-timing picture. It is refreshed once per
    // Director tick and read by both the operator surface and the diagnostic
    // stream, so those two can never disagree about what was proven.
    struct DirectorSyncView final {
        sync::SyncPolicy policy{};
        sync::SyncTopology topology{};
        sync::SyncStatusSnapshot status{};
        sync::SyncReadiness readiness{};
    } director_sync;
    sync::SyncPolicyModel director_sync_model;
    core::u64 director_sync_version{};
    bool director_sync_loaded{};

    // One place where every physical-timing condition is combined, so the
    // Program gate, the operator surface, and the diagnostic stream can never
    // disagree about why an output is or is not ready.
    const auto evaluate_sync_gate = [&](bool assets_adopted) {
        sync::SyncReadinessInput gate{};
        gate.policy = &director_sync.policy;
        gate.topology = &director_sync.topology;
        gate.nodes = director_sync.status.nodes;
        gate.exact_show_frame_ready = true;
        gate.assets_adopted = assets_adopted;
        gate.render_nodes_live =
            std::all_of(director_sync.topology.node_indices.begin(),
                        director_sync.topology.node_indices.end(),
                        [&](core::u32 node_index) {
                            const sync::NodeSyncStatus *const node =
                                director_sync.status.node(node_index);
                            return node != nullptr && node->runtime_reported;
                        });
        return sync::evaluate_sync_readiness(gate);
    };

    // A STRICT policy that asks for physical output timing refuses to promote
    // Program until that timing is proven. This is the whole of the promotion
    // gate: everything the asset and lifecycle layers know is reported on the
    // Output Monitor instead, because distributing content to render nodes is
    // an external concern in this build.
    const auto program_sync_held = [&]() noexcept {
        return director_sync.policy.requires_physical_sync() &&
               !director_sync.readiness.satisfied;
    };

    const auto observe_cluster = [&](core::u64 show_frame) {
        ClusterObservation observation{};
        const auto barrier_diagnostics = barrier.observer_diagnostics();
        observation.unix_time_ms = static_cast<core::u64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        observation.control_frame = scheduler.sealed_through().value();
        observation.show_frame = show_frame;
        const std::size_t observed_sessions =
            barrier_diagnostics.live_sessions +
            barrier_diagnostics.catching_up_sessions;
        const core::u32 observed_node_count =
            static_cast<core::u32>(std::min<std::size_t>(
                observed_sessions, std::numeric_limits<core::u32>::max()));
        if (!cluster_configuration_authored) {
            configured_cluster_nodes =
                std::max(configured_cluster_nodes, observed_node_count);
        }
        observation.configured_nodes = configured_cluster_nodes;
        observation.live_sessions = static_cast<core::u32>(
            std::min<std::size_t>(barrier_diagnostics.live_sessions,
                                  std::numeric_limits<core::u32>::max()));
        observation.catching_up_sessions = static_cast<core::u32>(
            std::min<std::size_t>(barrier_diagnostics.catching_up_sessions,
                                  std::numeric_limits<core::u32>::max()));
        const core::u64 released =
            barrier.released_through().through_frame.value();
        observation.release_lag_frames =
            observation.control_frame > released
                ? observation.control_frame - released
                : 0U;
        observation.pacer_lag_frames =
            pacer_lag(pacer, observation.control_frame);
        observation.open_barriers =
            static_cast<core::u64>(barrier.open_frames());
        observation.present_failures = present_failures;
        if (session_service != nullptr) {
            observation.sessions = session_service->counters();
        }
        observation.barrier = barrier_diagnostics;
        observation.diagnostics_recording = cluster_diagnostics.available();
        observation.sync_mode =
            std::string{sync::sync_mode_name(director_sync.policy.mode)};
        observation.sync_state =
            std::string{director_sync.readiness.physical_headline()};
        observation.program_state =
            std::string{director_sync.readiness.headline()};
        observation.sync_required =
            director_sync.policy.requires_physical_sync();
        observation.sync_satisfied = director_sync.readiness.satisfied;
        observation.sync_degraded = director_sync.readiness.degraded;
        observation.sync_groups = director_sync.readiness.groups;
        observation.sync_expected_outputs =
            director_sync.readiness.expected_outputs;
        observation.sync_locked_outputs =
            director_sync.readiness.locked_outputs;
        observation.sync_barrier_joined_outputs =
            director_sync.readiness.barrier_joined_outputs;
        observation.sync_configuration_issues =
            static_cast<core::u32>(director_sync.topology.diagnostics.size());
        for (const sync::NodeSyncStatus &node : director_sync.status.nodes) {
            observation.sync_simulated =
                observation.sync_simulated || node.simulated;
            observation.sync_missed_frames += node.counters.missed;
            observation.sync_late_frames += node.counters.late;
            observation.sync_held_frames += node.counters.held;
            observation.sync_repeated_frames += node.counters.repeated;
            observation.sync_loss_events += node.counters.loss_of_sync_events;
        }
        // Physical timing is proven, never assumed. Only a non-simulated
        // verdict that satisfied a policy demanding it may set these.
        observation.hardware_frame_lock_verified =
            director_sync.readiness.verified;
        observation.external_time_sync_verified =
            director_sync.readiness.verified &&
            director_sync.policy.requires_external_reference();
        observation.health = cluster_control_health(observation);
        return observation;
    };
    auto autoplay_pending = options.autoplay && !restored_recovered_transport;
    auto closed = false;

    // The Director previews the whole canvas of the same replicated scene. It
    // evaluates from its own committed document, so it needs no mailbox; the
    // model is reset when that document's version changes.
    scene::SceneModel scene_model;
    core::u64 scene_version = 0U;
    bool scene_loaded = false;

    // The feed map, evaluated only to report its digest: the Director previews
    // the whole canvas, not a node's slice, so it does not transform. Reported
    // so an operator can confirm the Director and every node agree on the map.
    output::FeedMapModel feed_model;
    core::u64 feed_version = 0U;
    bool feed_loaded = false;

    topology::HardwareMapModel hardware_model;
    core::u64 hardware_version = 0U;
    bool hardware_loaded = false;

    assets::AssetModel asset_model;
    assets::AssetRegistry director_asset_registry;
    assets::AssetRegistry director_playback_asset_registry;
    core::u64 asset_version = 0U;
    core::u64 asset_registry_digest = 0U;
    core::u64 asset_manifest_digest = 0U;
    bool assets_loaded = false;
    ProjectAssetAvailability director_asset_availability;
#if defined(MEDIASERVER_HAS_GUI)
    editor::EditorPreferencesState preferences_view{};
    {
        const auto &stored = options.stored_preferences;
        const auto &active = options.active_preferences;
        const auto editor_priority = [](persist::ProxyTranscodePriority value) {
            switch (value) {
            case persist::ProxyTranscodePriority::Background:
                return editor::EditorPreferences::TranscodePriority::Background;
            case persist::ProxyTranscodePriority::Balanced:
                return editor::EditorPreferences::TranscodePriority::Balanced;
            case persist::ProxyTranscodePriority::Fast:
                return editor::EditorPreferences::TranscodePriority::Fast;
            }
            return editor::EditorPreferences::TranscodePriority::Background;
        };
        preferences_view.stored = editor::EditorPreferences{
            stored.director_listen,
            stored.show_id,
            stored.director_epoch,
            stored.lifecycle_secret_file,
            stored.wal_path,
            stored.proxy_cache_directory,
            stored.proxy_cache_next_to_source,
            stored.proxy_cache_gibibytes,
            stored.output_buffer_frames,
            stored.output_read_ahead_frames,
            stored.playback_cache_mebibytes,
            stored.playback_trail_frames,
            editor_priority(stored.proxy_transcode_priority)};
        preferences_view.active = editor::EditorPreferences{
            active.director_listen,
            active.show_id,
            active.director_epoch,
            active.lifecycle_secret_file,
            active.wal_path,
            active.proxy_cache_directory,
            active.proxy_cache_next_to_source,
            active.proxy_cache_gibibytes,
            active.output_buffer_frames,
            active.output_read_ahead_frames,
            active.playback_cache_mebibytes,
            active.playback_trail_frames,
            editor_priority(active.proxy_transcode_priority)};
        // A field is only "overridden" when an argument actually displaced a
        // stored value; an argument matching the setting is not a conflict.
        const auto displaced = [](bool from_command_line,
                                  std::string_view stored_value,
                                  std::string_view active_value) {
            return from_command_line && !stored_value.empty() &&
                   stored_value != active_value;
        };
        preferences_view.director_listen_overridden =
            displaced(options.director_listen_from_command_line,
                      stored.director_listen, active.director_listen);
        preferences_view.show_id_overridden = displaced(
            options.show_id_from_command_line, stored.show_id, active.show_id);
        preferences_view.director_epoch_overridden =
            displaced(options.director_epoch_from_command_line,
                      stored.director_epoch, active.director_epoch);
        preferences_view.lifecycle_secret_file_overridden = displaced(
            options.lifecycle_secret_file_from_command_line,
            stored.lifecycle_secret_file, active.lifecycle_secret_file);
        preferences_view.wal_path_overridden =
            displaced(options.wal_path_from_command_line, stored.wal_path,
                      active.wal_path);
    }
#endif

    compositor::CompositorModel compositor_model;
    core::u64 compositor_version = 0U;
    bool compositor_loaded = false;

    while (stop_requested == 0 && !closed) {
        const auto now = Clock::now();
#if defined(MEDIASERVER_HAS_GUI)
        if (editor_coalescing_policy.expire(now)) {
            scheduler.end_coalescing();
            pending_editor_coalescing_close = true;
        }
#endif

        auto paced_tick = pacer.advance_to(now);
        if (!paced_tick) {
            if (paced_tick.error() == show::ShowClockError::TimeOverflow) {
                static_cast<void>(pacer.seek(pacer.frame(), now));
                continue;
            }
            std::cerr << "Director frame pacer refused a time observation\n";
            return 1;
        }

        const auto due = std::min(std::move(paced_tick).value().advanced,
                                  max_director_catch_up);
        core::u64 sealed_this_iteration = 0U;
        std::optional<show::FrameRate> repace_after_seal;
        for (core::u64 index = 0U; index < due; ++index) {
            if (tracked.size() >= max_tracked_frames) {
                std::cerr << "Director cohort backlog reached "
                          << tracked.size()
                          << " unretired frames; refusing to seal further\n";
                return 1;
            }

            const auto through = scheduler.sealed_through().value();
            if (through == std::numeric_limits<core::u64>::max()) {
                std::cerr << "Director control frame numbering is exhausted\n";
                return 1;
            }
            const cmd::FrameNumber frame{through + 1U};

            // Operator input is applied to the first frame of this iteration,
            // because an anchor has to name the exact control frame it takes
            // effect on.
            if (index == 0U) {
                if (autoplay_pending) {
                    autoplay_pending = false;
                    if (!transport.play(frame)) {
                        std::cerr << "could not start the Director transport\n";
                        return 1;
                    }
                }
                bool project_loaded_this_frame = false;
#if defined(MEDIASERVER_HAS_GUI)
                if (pending_project_load.has_value()) {
                    const editor::ProjectFileRequest request =
                        std::move(*pending_project_load);
                    pending_project_load.reset();

                    auto loaded =
                        pending_preloaded_project.has_value()
                            ? persist::ProjectLoadResult::success(
                                  std::move(*pending_preloaded_project))
                            : persist::load_project_file(
                                  native_path_from_utf8(request.path));
                    pending_preloaded_project.reset();
                    if (!loaded) {
                        project_message =
                            project_file_failure("LOADING", loaded.error());
                        std::cerr << "could not load project '" << request.path
                                  << "' (code "
                                  << static_cast<unsigned>(loaded.error().code)
                                  << ')';
                        if (loaded.error().system_error) {
                            std::cerr << ": "
                                      << loaded.error().system_error.message();
                        }
                        std::cerr << '\n';
                    } else {
                        doc::Root replacement = std::move(loaded).value();
                        std::size_t enriched_media_nodes{};
                        try {
                            MediaRateEnrichment enrichment =
                                enrich_media_frame_rates(replacement);
                            replacement = std::move(enrichment.project);
                            enriched_media_nodes = enrichment.updated_nodes;
                        } catch (const std::bad_alloc &) {
                            std::cerr << "could not enrich loaded media FPS "
                                         "metadata: "
                                         "allocation failed\n";
                        } catch (const std::length_error &) {
                            std::cerr << "could not enrich loaded media FPS "
                                         "metadata: "
                                         "project limits exceeded\n";
                        }
                        show::TransportAuthority replacement_transport =
                            transport;
                        const doc::Node *const raw_transport =
                            replacement.get(show::transport_node_id);
                        const auto loaded_transport =
                            show::read_transport(replacement);
                        const bool malformed_transport =
                            raw_transport != nullptr &&
                            std::string_view{raw_transport->type} ==
                                show::transport_node_type &&
                            !loaded_transport.has_value();
                        bool transport_valid = !malformed_transport;
                        if (transport_valid && loaded_transport.has_value()) {
                            transport_valid = restore_project_transport(
                                replacement_transport, *loaded_transport,
                                frame);
                        } else if (transport_valid) {
                            // A project predating persisted transport settings
                            // gets the launch/default cadence (50 fps unless an
                            // explicit --show-rate was supplied), never the
                            // settings of whichever project happened to be
                            // open.
                            auto legacy_transport =
                                show::TransportAuthority::create(
                                    clock_config(options), frame);
                            if (legacy_transport) {
                                replacement_transport =
                                    std::move(legacy_transport).value();
                            } else {
                                transport_valid = false;
                            }
                        }
                        if (!transport_valid) {
                            project_message = "ERROR LOADING PROJECT TRANSPORT";
                            std::cerr << "project transport state is invalid\n";
                        } else {
                            // Settle before staging so the preflight batch is
                            // still exact when the ordinary end-of-frame
                            // transport settlement runs below.
                            static_cast<void>(
                                replacement_transport.settle(frame));
                            // Re-anchor the saved playhead to this Director's
                            // current control frame. This preserves range,
                            // direction, state, and position without importing
                            // a stale control-frame ordinal from another run.
                            replacement =
                                replacement.set(show::make_transport_node(
                                    replacement_transport.anchor()));
                            if (!normalize_project_switcher(replacement,
                                                            frame)) {
                                project_message =
                                    "ERROR LOADING PROJECT SWITCHER";
                                std::cerr
                                    << "project switcher state is invalid\n";
                            } else {
                                auto staged = stage_project_load(
                                    scheduler, replacement, frame, command_log,
                                    frame_durability);
                                if (!staged) {
                                    const auto &error = staged.error();
                                    switch (error.code) {
                                    case ProjectLoadStageErrorCode::
                                        ScheduleFailed:
                                        project_message =
                                            "ERROR LOADING PROJECT: "
                                            "SCHEDULER "
                                            "REJECTED REPLACEMENT";
                                        break;
                                    case ProjectLoadStageErrorCode::
                                        PrepareFailed:
                                        project_message =
                                            "ERROR LOADING PROJECT: "
                                            "FRAME "
                                            "COULD NOT BE PREPARED";
                                        break;
                                    case ProjectLoadStageErrorCode::
                                        WirePreflightFailed:
                                        project_message =
                                            error.command_log_error
                                                        .has_value() &&
                                                    command_log_limit_error(
                                                        *error
                                                             .command_log_error)
                                                ? "ERROR LOADING "
                                                  "PROJECT: "
                                                  "PROJECT "
                                                  "EXCEEDS FRAME "
                                                  "TRANSFER "
                                                  "LIMITS"
                                                : "ERROR LOADING "
                                                  "PROJECT: "
                                                  "FRAME "
                                                  "TRANSFER "
                                                  "PREFLIGHT "
                                                  "REJECTED";
                                        break;
                                    case ProjectLoadStageErrorCode::
                                        WalPreflightFailed:
                                        project_message =
                                            error.command_log_error
                                                        .has_value() &&
                                                    command_log_limit_error(
                                                        *error
                                                             .command_log_error)
                                                ? "ERROR LOADING "
                                                  "PROJECT: "
                                                  "PROJECT EXCEEDS "
                                                  "WAL "
                                                  "LIMITS"
                                                : "ERROR LOADING "
                                                  "PROJECT: WAL "
                                                  "PREFLIGHT "
                                                  "REJECTED";
                                        break;
                                    case ProjectLoadStageErrorCode::AbortFailed:
                                        project_message =
                                            "ERROR LOADING PROJECT: "
                                            "FRAME "
                                            "PREFLIGHT FAILED";
                                        break;
                                    case ProjectLoadStageErrorCode::
                                        AllocationFailed:
                                        project_message =
                                            "ERROR LOADING PROJECT: "
                                            "NOT ENOUGH "
                                            "MEMORY";
                                        break;
                                    }
                                    std::cerr
                                        << "could not stage loaded project "
                                           "for "
                                           "frame "
                                        << frame.value() << " (code "
                                        << static_cast<unsigned>(error.code)
                                        << ")\n";
                                } else {
                                    static_assert(
                                        std::is_nothrow_move_assignable_v<
                                            cmd::DirectorScheduler>);
                                    scheduler = std::move(staged).value();
                                    transport = replacement_transport;
                                    repace_after_seal = transport.anchor().rate;
                                    authored = transport.anchor();
                                    saved_project_snapshot =
                                        scheduler.projected_document();
                                    project_explicitly_dirty =
                                        enriched_media_nodes != 0U;
                                    project_path = request.path;
                                    remember_recent_project(project_path);
                                    project_message =
                                        enriched_media_nodes == 0U
                                            ? "PROJECT LOADED"
                                            : "PROJECT LOADED - MEDIA "
                                              "FPS "
                                              "UPDATED (" +
                                                  std::to_string(
                                                      enriched_media_nodes) +
                                                  ')';
                                    if (enriched_media_nodes != 0U) {
                                        std::cerr
                                            << "enriched "
                                            << enriched_media_nodes
                                            << " loaded media node(s) with "
                                               "native FPS; save the project "
                                               "to persist metadata\n";
                                    }
                                    leave_preview();
                                    next_new_node = 100'000U;
                                    ++project_generation;
                                    // A replacement project may carry the
                                    // same document version as the previous
                                    // one, while project-relative asset
                                    // resolution depends on the newly chosen
                                    // project path. Force the runtime asset
                                    // overlay to be rebuilt on the next draw.
                                    assets_loaded = false;
                                    clear_pending_project_authoring();
                                    project_loaded_this_frame = true;
                                }
                            }
                        }
                    }
                }
#endif
                if (!project_loaded_this_frame) {
                    for (const auto command : operator_commands) {
                        apply_command(transport, command, frame);
                    }
                }
                operator_commands.clear();
#if defined(MEDIASERVER_HAS_GUI)
                if (preview_transport.has_value()) {
                    // Range, loop, and cadence are one show-wide setting, so
                    // Preview follows the live transport for those. Everything
                    // else about where Preview sits is local and authored
                    // nowhere: no command, no anchor, no replication.
                    const show::TransportAnchor &live = transport.anchor();
                    const show::TransportAnchor &previewed =
                        preview_transport->anchor();
                    if (previewed.rate != live.rate) {
                        static_cast<void>(
                            preview_transport->set_rate(live.rate, frame));
                    }
                    if (previewed.first_frame != live.first_frame ||
                        previewed.last_frame != live.last_frame ||
                        previewed.loop != live.loop) {
                        static_cast<void>(preview_transport->set_range(
                            live.first_frame, live.last_frame, live.loop,
                            frame));
                    }
                    if (!project_loaded_this_frame) {
                        for (const auto command : preview_commands) {
                            apply_command(*preview_transport, command, frame);
                        }
                        if (pending_preview_seek.has_value()) {
                            if (preview_transport->state() ==
                                show::TransportState::Playing) {
                                static_cast<void>(
                                    preview_transport->pause(frame));
                            }
                            static_cast<void>(preview_transport->seek(
                                show::ShowFrame{
                                    std::min(std::max(*pending_preview_seek,
                                                      live.first_frame.value()),
                                             live.last_frame.value())},
                                frame));
                        }
                    }
                    static_cast<void>(preview_transport->settle(frame));
                }
                preview_commands.clear();
                pending_preview_seek.reset();
                const auto projected_switcher =
                    show::read_switcher(scheduler.projected_document());
                bool scene_edits_locked = false;
                if (projected_switcher.has_value() &&
                    show::derive_switcher_progress(*projected_switcher, frame)
                        .active) {
                    scene::SceneModel projected_scenes;
                    projected_scenes.reset(scheduler.projected_document());
                    const auto infos = projected_scenes.scenes();
                    const auto from = std::find_if(
                        infos.begin(), infos.end(),
                        [&](const scene::SceneInfo &info) {
                            return info.id ==
                                   projected_switcher->transition_from;
                        });
                    const auto to = std::find_if(
                        infos.begin(), infos.end(),
                        [&](const scene::SceneInfo &info) {
                            return info.id == projected_switcher->transition_to;
                        });
                    scene_edits_locked =
                        from != infos.end() && to != infos.end() &&
                        show::dissolve_canvases_compatible(from->canvas,
                                                           to->canvas);
                }
                // Which scene an authoring gesture belongs to. Preview edits
                // the rehearsal scene; otherwise the operator is editing
                // Program directly. A document with no switcher yet has one
                // scene, and it is the lowest-id stage.
                const auto authoring_scene = [&]() -> doc::NodeId {
                    if (projected_switcher.has_value()) {
                        return rehearsal_mode
                                   ? projected_switcher->rehearsal_scene
                                   : projected_switcher->program_scene;
                    }
                    scene::SceneModel authored_scenes;
                    authored_scenes.reset(scheduler.projected_document());
                    const auto infos = authored_scenes.scenes();
                    return infos.empty() ? doc::NodeId{} : infos.front().id;
                };
                if (scene_edits_locked) {
                    pending_edits.clear();
                    pending_keys.clear();
                    pending_key_moves.clear();
                    pending_key_duplicate.reset();
                    pending_key_batch.reset();
                    key_scale_preview.reset();
                    pending_clip_timings.clear();
                    pending_clip_splits.clear();
                    pending_lane_edits.clear();
                    pending_layer_orders.clear();
                    pending_element_flags.clear();
                    pending_timeline_edits.clear();
                    pending_group_edit.reset();
                    pending_selection_set_edit.reset();
                    pending_mask_edit.reset();
                    pending_clip_paste.reset();
                    pending_key_paste.reset();
                    pending_project_settings.reset();
                    pending_properties.clear();
                    pending_generics.clear();
                    pending_adds.clear();
                    pending_effects.clear();
                    pending_film_grain_effect_edits.clear();
                    pending_effect_deletes.clear();
                    pending_deletes.clear();
                    pending_compositor_add.reset();
                    pending_compositor_move.reset();
                    pending_compositor_connect.reset();
                    pending_compositor_property.reset();
                    pending_compositor_delete.reset();
                    pending_history.reset();
                }
                for (const auto &edit : pending_locks) {
                    if (!schedule_lock_edit(scheduler, edit, frame)) {
                        std::cerr
                            << "could not author an element lock for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_locks.clear();
                for (const auto &edit : pending_edits) {
                    if (!schedule_scene_edit(scheduler, edit, frame)) {
                        std::cerr << "could not author a scene edit for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_edits.clear();
                for (const auto &key : pending_keys) {
                    if (!schedule_key_edit(scheduler, key, frame,
                                           coalescing_for(key.gesture))) {
                        std::cerr << "could not author a keyframe for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_keys.clear();
                // Import is a live path reference plus a metadata-only source
                // revision. No media payload bytes are read on this loop.
                for (auto &imported : pending_asset_imports) {
                    auto prepared =
                        prepare_project_asset_import(std::move(imported.asset));
                    if (!prepared) {
                        discard_gesture_requests(pending_feed_surface_adds,
                                                 imported.gesture);
                        discard_gesture_requests(pending_feed_surface_edits,
                                                 imported.gesture);
                        const std::string_view label =
                            project_asset_import_error_label(
                                prepared.error().code);
                        project_message = "ASSET IMPORT FAILED - ";
                        project_message.append(label);
                        std::cerr << "asset import rejected: " << label << '\n';
                        continue;
                    }

                    PendingAssetImportAuthoring authoring{};
                    authoring.gesture = imported.gesture;
                    try {
                        extract_gesture_requests(pending_feed_surface_adds,
                                                 imported.gesture,
                                                 authoring.surface_adds);
                        extract_gesture_requests(pending_feed_surface_edits,
                                                 imported.gesture,
                                                 authoring.surface_edits);
                    } catch (const std::bad_alloc &) {
                        discard_gesture_requests(pending_feed_surface_adds,
                                                 imported.gesture);
                        discard_gesture_requests(pending_feed_surface_edits,
                                                 imported.gesture);
                        project_message =
                            "ASSET IMPORT FAILED - NOT ENOUGH MEMORY";
                        std::cerr << "could not queue an asset import: "
                                     "allocation failed\n";
                        continue;
                    }

                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId asset_id{next_new_node++};
                    const editor::AssetImportIntent verified{
                        std::move(prepared).value(), imported.gesture};
                    if (!schedule_asset_import(scheduler, verified, asset_id,
                                               frame)) {
                        project_message =
                            "ASSET IMPORT FAILED - SCHEDULER REJECTED ASSET";
                        std::cerr
                            << "could not author a completed asset import "
                               "for frame "
                            << frame.value() << '\n';
                        continue;
                    }

                    bool companion_failed = false;
                    for (const auto &request : authoring.surface_edits) {
                        if (!schedule_feed_surface_edit(scheduler, request,
                                                        frame)) {
                            companion_failed = true;
                            break;
                        }
                    }
                    if (!companion_failed) {
                        for (const auto &request : authoring.surface_adds) {
                            while (scheduler.projected_document().contains(
                                doc::NodeId{next_new_node})) {
                                ++next_new_node;
                            }
                            const doc::NodeId surface_id{next_new_node++};
                            if (!schedule_feed_surface_add(scheduler, request,
                                                           surface_id, frame)) {
                                companion_failed = true;
                                break;
                            }
                            if (request.assign_map_node.has_value()) {
                                const editor::FeedProjectionEdit assignment{
                                    *request.assign_map_node,
                                    output::FeedProjection::PlanarSurface,
                                    surface_id,
                                    scene::whole(request.surface.raster),
                                    request.gesture};
                                if (!schedule_feed_projection_edit(
                                        scheduler, assignment, frame)) {
                                    companion_failed = true;
                                    break;
                                }
                            }
                        }
                    }
                    if (companion_failed) {
                        project_message =
                            "ASSET IMPORTED - SURFACE UPDATE FAILED";
                        std::cerr << "could not author the projection surface "
                                     "paired with an asset import for frame "
                                  << frame.value() << '\n';
                    } else {
                        project_message = "ASSET IMPORTED";
                    }
                }
                pending_asset_imports.clear();
                // A mixed surface drag can emit animated-axis keys and one
                // masked base edit for the remaining axes. Schedule that base
                // immediately after its keys so their shared scene transaction
                // stays on top and undoes atomically.
                for (const auto &request : pending_feed_surface_edits) {
                    if (!schedule_feed_surface_edit(scheduler, request,
                                                    frame)) {
                        std::cerr
                            << "could not edit a projection surface for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_feed_surface_edits.clear();
                for (const auto &moved : pending_key_moves) {
                    if (!schedule_key_move(scheduler, moved, frame)) {
                        std::cerr
                            << "could not author a keyframe move for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_key_moves.clear();
                if (pending_key_batch.has_value()) {
                    const editor::KeyBatchRequest request =
                        std::move(*pending_key_batch);
                    pending_key_batch.reset();
                    const std::vector<scene::KeyAddress> selection =
                        scene_key_addresses(request.edit.keys);
                    scene::KeyEditResult planned = [&]() {
                        switch (request.edit.action) {
                        case editor::TimelineKeyBatchAction::Move:
                            return scene::plan_key_move(
                                scheduler.projected_document(), selection,
                                request.edit.delta);
                        case editor::TimelineKeyBatchAction::Duplicate:
                            return scene::plan_key_duplicate(
                                scheduler.projected_document(), selection,
                                request.edit.delta);
                        case editor::TimelineKeyBatchAction::Delete:
                            return scene::plan_key_delete(
                                scheduler.projected_document(), selection);
                        case editor::TimelineKeyBatchAction::Scale:
                            return scene::plan_key_scale(
                                scheduler.projected_document(), selection,
                                request.edit.scale);
                        }
                        return scene::KeyEditResult::failure(
                            scene::KeyEditError::InvalidScale);
                    }();
                    if (!planned) {
                        switch (planned.error()) {
                        case scene::KeyEditError::InvalidScale:
                            project_message = "KEY SCALE REJECTED - "
                                              "DENOMINATOR MUST BE POSITIVE";
                            break;
                        case scene::KeyEditError::IntegerOverflow:
                            project_message = "KEY EDIT REJECTED - DESTINATION "
                                              "FRAME OVERFLOW";
                            break;
                        case scene::KeyEditError::AllocationFailed:
                            std::cerr << "could not allocate an atomic key "
                                         "edit plan\n";
                            return 1;
                        }
                    } else {
                        scene::KeyEditPlan plan = std::move(planned).value();
                        const doc::Delta delta = doc::diff(
                            scheduler.projected_document(), plan.after);
                        if (!delta.changes.empty()) {
                            auto scheduled =
                                scheduler.schedule_document_replace(
                                    plan.after, cmd::UndoContextId{2U},
                                    cmd::TransactionId{request.transaction},
                                    frame);
                            if (!scheduled) {
                                std::cerr << "could not author an atomic key "
                                             "edit for frame "
                                          << frame.value() << '\n';
                                return 1;
                            }
                        }
                        std::vector<editor::TimelineKeyAddress> selection_after;
                        selection_after.reserve(plan.selection_after.size());
                        for (const scene::KeyRecord &key :
                             plan.selection_after) {
                            selection_after.push_back(
                                editor::TimelineKeyAddress{key.address.target,
                                                           key.address.property,
                                                           key.address.frame});
                        }
                        if (!editor->apply_key_edit_selection(
                                request.scene, selection_after)) {
                            project_message = "KEY EDIT APPLIED - LOCAL "
                                              "SELECTION COULD NOT BE UPDATED";
                        } else {
                            project_message = key_batch_message(
                                request.edit.action, plan.report);
                        }
                    }
                }
                if (pending_key_duplicate.has_value()) {
                    try {
                        const std::vector<KeyAddress> selection =
                            key_addresses(pending_key_duplicate->selection);
                        auto planned = duplicate_keys(
                            scheduler.projected_document(),
                            pending_key_duplicate->scene, selection,
                            pending_key_duplicate->delta);
                        if (planned) {
                            const KeyEditReport &report =
                                planned.value().report;
                            if (report.applied != 0U) {
                                auto scheduled =
                                    scheduler.schedule_document_replace(
                                        planned.value().document,
                                        cmd::UndoContextId{2U},
                                        cmd::TransactionId{
                                            pending_key_duplicate->transaction},
                                        frame);
                                if (!scheduled) {
                                    std::cerr
                                        << "could not author a key duplicate "
                                           "for frame "
                                        << frame.value() << '\n';
                                    return 1;
                                }
                            }
                            project_message = key_operation_message(
                                "DUPLICATE", report.applied, "APPLIED",
                                report.locked_targets, report.incompatible);
                        } else {
                            project_message =
                                "KEY DUPLICATE REJECTED - FRAME OUT OF RANGE";
                        }
                    } catch (const std::bad_alloc &) {
                        std::cerr << "could not plan a key duplicate\n";
                        return 1;
                    }
                    pending_key_duplicate.reset();
                }
                if (pending_key_paste.has_value()) {
                    try {
                        auto planned = pending_key_paste->snapshot.paste(
                            scheduler.projected_document(),
                            pending_key_paste->request.scene,
                            pending_key_paste->request.playhead);
                        if (planned) {
                            const KeyEditReport &report =
                                planned.value().report;
                            if (report.applied != 0U) {
                                auto scheduled =
                                    scheduler.schedule_document_replace(
                                        planned.value().document,
                                        cmd::UndoContextId{2U},
                                        cmd::TransactionId{
                                            pending_key_paste->request
                                                .transaction},
                                        frame);
                                if (!scheduled) {
                                    std::cerr
                                        << "could not author a key paste for "
                                           "frame "
                                        << frame.value() << '\n';
                                    return 1;
                                }
                            }
                            project_message = key_operation_message(
                                "PASTE", report.applied, "APPLIED",
                                report.locked_targets, report.incompatible);
                        } else if (planned.error() == KeyPasteError::Empty) {
                            project_message = "KEY PASTE - CLIPBOARD EMPTY";
                        } else {
                            project_message =
                                "KEY PASTE REJECTED - FRAME OUT OF RANGE";
                        }
                    } catch (const std::bad_alloc &) {
                        std::cerr << "could not plan a key paste\n";
                        return 1;
                    }
                    pending_key_paste.reset();
                }
                for (const auto &edit : pending_clip_timings) {
                    if (!schedule_clip_timing(scheduler, edit, frame)) {
                        std::cerr
                            << "could not author a clip placement for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_clip_timings.clear();
                for (const auto &edit : pending_layer_orders) {
                    const scene::LayerOrderPlacement placement =
                        edit.placement == editor::TimelineLayerPlacement::Before
                            ? scene::LayerOrderPlacement::Before
                            : scene::LayerOrderPlacement::After;
                    auto planned = scene::plan_reorder_layer(
                        scheduler.projected_document(), edit.scene, edit.layer,
                        edit.destination, placement);
                    if (!planned) {
                        if (planned.error() ==
                            scene::LayerOrderError::AllocationFailed) {
                            std::cerr
                                << "could not allocate a layer reorder plan\n";
                            return 1;
                        }
                        project_message = "LAYER REORDER REJECTED";
                        continue;
                    }
                    const doc::Delta delta = doc::diff(
                        scheduler.projected_document(), planned.value().after);
                    if (!delta.changes.empty()) {
                        auto scheduled = scheduler.schedule_document_replace(
                            planned.value().after, cmd::UndoContextId{2U},
                            cmd::TransactionId{edit.gesture}, frame);
                        if (!scheduled) {
                            std::cerr
                                << "could not author a layer reorder for frame "
                                << frame.value() << '\n';
                            return 1;
                        }
                    }
                }
                pending_layer_orders.clear();
                for (const auto &split : pending_clip_splits) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_clip_split(scheduler, split, id, frame)) {
                        std::cerr << "could not split a clip for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_clip_splits.clear();
                // Mute and lock are one boolean on the element's own node,
                // so they need no planner - but they do need the same undo
                // grouping, which the shared gesture id gives them.
                for (const auto &edit : pending_element_flags) {
                    const auto *const existing =
                        scheduler.projected_document().get(edit.element);
                    if (existing == nullptr) {
                        // Deleted under the click. Benign.
                        continue;
                    }
                    doc::Node updated = *existing;
                    const bool track =
                        existing->type == scene::schema::timeline_lane_type;
                    const bool showing =
                        edit.flag == editor::TimelineElementFlag::Visible;
                    // A track spells both of these differently and spells
                    // visibility inside out - it is muted rather than hidden.
                    // Translating here keeps one control in the timeline and
                    // one in the Inspector writing the same state instead of
                    // each owning half of it.
                    updated.properties.insert_or_assign(
                        std::string{track
                                        ? (showing ? scene::schema::lane_muted
                                                   : scene::schema::lane_locked)
                                        : (showing ? scene::schema::visible
                                                   : scene::schema::locked)},
                        doc::PropertyValue{track && showing ? !edit.value
                                                            : edit.value});
                    if (updated == *existing) {
                        continue;
                    }
                    auto scheduled = scheduler.schedule_edit(
                        cmd::PutNode{std::move(updated)},
                        cmd::PutNode{*existing}, cmd::UndoContextId{2U},
                        cmd::TransactionId{edit.gesture}, frame);
                    if (!scheduled) {
                        std::cerr
                            << "could not author an element flag for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_element_flags.clear();
                for (const auto &edit : pending_lane_edits) {
                    LaneAuthorAction action{};
                    switch (edit.action) {
                    case editor::TimelineLaneAction::Create:
                        action = LaneAuthorAction::Create;
                        break;
                    case editor::TimelineLaneAction::Rename:
                        action = LaneAuthorAction::Rename;
                        break;
                    case editor::TimelineLaneAction::MoveEarlier:
                        action = LaneAuthorAction::MoveEarlier;
                        break;
                    case editor::TimelineLaneAction::MoveLater:
                        action = LaneAuthorAction::MoveLater;
                        break;
                    case editor::TimelineLaneAction::MoveTo:
                        action = LaneAuthorAction::MoveTo;
                        break;
                    case editor::TimelineLaneAction::ToggleMute:
                        action = LaneAuthorAction::ToggleMute;
                        break;
                    case editor::TimelineLaneAction::ToggleLock:
                        action = LaneAuthorAction::ToggleLock;
                        break;
                    case editor::TimelineLaneAction::DeleteEmpty:
                        action = LaneAuthorAction::DeleteEmpty;
                        break;
                    case editor::TimelineLaneAction::DeleteClips:
                        action = LaneAuthorAction::DeleteClips;
                        break;
                    case editor::TimelineLaneAction::DeleteMoveClips:
                        action = LaneAuthorAction::DeleteMoveClips;
                        break;
                    }
                    auto planned = plan_lane_authoring(
                        scheduler.projected_document(),
                        LaneAuthorRequest{action, edit.scene, edit.lane,
                                          edit.destination, edit.name},
                        next_new_node);
                    if (!planned) {
                        // Stale selection and a lane becoming non-empty are
                        // benign operator races. The planner has not authored
                        // anything, so history and redo remain untouched.
                        project_message =
                            planned.error() == LaneAuthorError::NonEmptyLane
                                ? "TRACK DELETE REJECTED - TRACK IS NOT EMPTY"
                                : "TRACK EDIT REJECTED";
                        continue;
                    }
                    const doc::Delta delta =
                        doc::diff(scheduler.projected_document(),
                                  planned.value().document);
                    if (!delta.changes.empty()) {
                        auto scheduled = scheduler.schedule_document_replace(
                            planned.value().document, cmd::UndoContextId{2U},
                            cmd::TransactionId{edit.gesture}, frame);
                        if (!scheduled) {
                            std::cerr
                                << "could not author a timeline lane for frame "
                                << frame.value() << '\n';
                            return 1;
                        }
                    }
                    next_new_node = planned.value().next_node;
                }
                pending_lane_edits.clear();
                if (pending_group_edit.has_value()) {
                    const editor::GroupEditIntent request =
                        std::move(*pending_group_edit);
                    pending_group_edit.reset();
                    core::Result<scene::GroupEditPlan, scene::GroupEditError>
                        planned =
                            request.action == editor::GroupEditAction::Group
                                ? scene::plan_group_nodes(
                                      scheduler.projected_document(),
                                      request.scene, request.selection,
                                      doc::NodeId{next_new_node}, "Group")
                                : scene::plan_ungroup_nodes(
                                      scheduler.projected_document(),
                                      request.scene, request.group);
                    if (!planned) {
                        project_message = group_edit_error_message(
                            request.action, planned.error());
                    } else {
                        const doc::Delta delta =
                            doc::diff(scheduler.projected_document(),
                                      planned.value().after);
                        if (!delta.changes.empty()) {
                            auto scheduled =
                                scheduler.schedule_document_replace(
                                    planned.value().after,
                                    cmd::UndoContextId{2U},
                                    cmd::TransactionId{request.gesture}, frame);
                            if (!scheduled) {
                                std::cerr << "could not author a Scene group "
                                             "edit for frame "
                                          << frame.value() << '\n';
                                return 1;
                            }
                        }
                        if (request.action == editor::GroupEditAction::Group) {
                            next_new_node =
                                std::max(next_new_node,
                                         planned.value().group.value() + 1U);
                            project_message = "GROUP CREATED";
                        } else {
                            project_message =
                                "GROUP REMOVED; CHILDREN PRESERVED";
                        }
                    }
                }
                if (pending_selection_set_edit.has_value()) {
                    const editor::SelectionSetEditIntent request =
                        std::move(*pending_selection_set_edit);
                    pending_selection_set_edit.reset();
                    const scene::SelectionSetPlanResult planned = [&]() {
                        switch (request.action) {
                        case editor::SelectionSetEditAction::Create:
                            return scene::plan_create_selection_set(
                                scheduler.projected_document(), request.scene,
                                request.name, request.members,
                                doc::NodeId{next_new_node});
                        case editor::SelectionSetEditAction::ReplaceMembers:
                            return scene::plan_replace_selection_set(
                                scheduler.projected_document(), request.scene,
                                request.set, request.members,
                                doc::NodeId{next_new_node});
                        case editor::SelectionSetEditAction::Rename:
                            return scene::plan_rename_selection_set(
                                scheduler.projected_document(), request.scene,
                                request.set, request.name);
                        case editor::SelectionSetEditAction::Delete:
                            return scene::plan_delete_selection_set(
                                scheduler.projected_document(), request.scene,
                                request.set);
                        case editor::SelectionSetEditAction::Recall:
                            return scene::SelectionSetPlanResult::failure(
                                scene::SelectionSetError{
                                    scene::SelectionSetErrorCode::InvalidSet,
                                    request.set});
                        }
                        return scene::SelectionSetPlanResult::failure(
                            scene::SelectionSetError{
                                scene::SelectionSetErrorCode::InvalidSet,
                                request.set});
                    }();
                    if (!planned) {
                        project_message = selection_set_error_message(
                            request.action, planned.error());
                    } else {
                        const doc::Delta delta =
                            doc::diff(scheduler.projected_document(),
                                      planned.value().after);
                        if (!delta.changes.empty()) {
                            auto scheduled =
                                scheduler.schedule_document_replace(
                                    planned.value().after,
                                    cmd::UndoContextId{2U},
                                    cmd::TransactionId{request.gesture}, frame);
                            if (!scheduled) {
                                std::cerr << "could not author a Selection Set "
                                             "edit for frame "
                                          << frame.value() << '\n';
                                return 1;
                            }
                        }
                        if (request.action ==
                                editor::SelectionSetEditAction::Create ||
                            request.action == editor::SelectionSetEditAction::
                                                  ReplaceMembers) {
                            for (const doc::Node &node : planned.value().puts) {
                                next_new_node = std::max(next_new_node,
                                                         node.id.value() + 1U);
                            }
                        }
                        switch (request.action) {
                        case editor::SelectionSetEditAction::Create:
                            project_message =
                                "SELECTION SET SAVED - " +
                                std::to_string(planned.value().members.size()) +
                                " MEMBERS";
                            break;
                        case editor::SelectionSetEditAction::ReplaceMembers:
                            project_message =
                                "SELECTION SET REPLACED - " +
                                std::to_string(planned.value().members.size()) +
                                " MEMBERS";
                            break;
                        case editor::SelectionSetEditAction::Rename:
                            project_message = "SELECTION SET RENAMED";
                            break;
                        case editor::SelectionSetEditAction::Delete:
                            project_message = "SELECTION SET DELETED";
                            break;
                        case editor::SelectionSetEditAction::Recall:
                            break;
                        }
                    }
                }
                if (pending_mask_edit.has_value()) {
                    const editor::MaskEditIntent request =
                        std::move(*pending_mask_edit);
                    pending_mask_edit.reset();
                    auto planned = plan_mask_edit_integration(
                        scheduler.projected_document(), request, next_new_node);
                    if (!planned) {
                        if (planned.error().code ==
                            scene::MaskAuthoringErrorCode::AllocationFailed) {
                            std::cerr
                                << "could not allocate a mask authoring plan\n";
                            return 1;
                        }
                        project_message =
                            mask_edit_error_message(planned.error().code);
                    } else {
                        const doc::Delta delta =
                            doc::diff(scheduler.projected_document(),
                                      planned.value().document);
                        if (!delta.changes.empty()) {
                            auto scheduled =
                                scheduler.schedule_document_replace(
                                    planned.value().document,
                                    cmd::UndoContextId{2U},
                                    cmd::TransactionId{request.transaction},
                                    frame);
                            if (!scheduled) {
                                std::cerr
                                    << "could not author a mask edit for frame "
                                    << frame.value() << '\n';
                                return 1;
                            }
                        }
                        next_new_node = planned.value().next_node;
                        project_message = "MASK EDIT APPLIED";
                    }
                }
                for (const editor::TimelineEditIntent &request :
                     pending_timeline_edits) {
                    scene::SceneModel current_model;
                    current_model.reset(scheduler.projected_document());
                    const std::vector<scene::ClipPlacement> current_clips =
                        current_model.clips(request.scene);
                    auto planned = plan_timeline_edit_integration(
                        scheduler.projected_document(), current_clips, request,
                        clip_clipboard, next_new_node);
                    if (!planned) {
                        switch (planned.error()) {
                        case TimelineEditIntegrationError::
                            ClipboardIncompatible:
                            project_message =
                                "TIMELINE EDIT REJECTED - CLIPBOARD "
                                "MUST "
                                "CONTAIN ONE SOURCE TRACK";
                            break;
                        case TimelineEditIntegrationError::
                            TargetLaneUnavailable:
                            project_message = "TIMELINE EDIT REJECTED - TARGET "
                                              "TRACK IS LOCKED OR STALE";
                            break;
                        case TimelineEditIntegrationError::TimelineRejected:
                        case TimelineEditIntegrationError::AuthoringRejected:
                            project_message = "TIMELINE EDIT REJECTED - CLIPS "
                                              "OR HANDLES CHANGED";
                            break;
                        case TimelineEditIntegrationError::PasteRejected:
                            project_message = "TIMELINE EDIT REJECTED - "
                                              "CLIPBOARD PASTE FAILED";
                            break;
                        case TimelineEditIntegrationError::AllocationFailed:
                            std::cerr << "could not allocate an advanced "
                                         "timeline edit plan\n";
                            return 1;
                        }
                        continue;
                    }
                    const doc::Delta delta =
                        doc::diff(scheduler.projected_document(),
                                  planned.value().document);
                    if (!delta.changes.empty()) {
                        auto scheduled = scheduler.schedule_document_replace(
                            planned.value().document, cmd::UndoContextId{2U},
                            cmd::TransactionId{request.transaction}, frame);
                        if (!scheduled) {
                            std::cerr << "could not author an advanced "
                                         "timeline edit for frame "
                                      << frame.value() << '\n';
                            return 1;
                        }
                    }
                    next_new_node = planned.value().next_node;
                    project_message = "TIMELINE EDIT APPLIED";
                }
                pending_timeline_edits.clear();
                if (pending_clip_paste.has_value()) {
                    auto planned = pending_clip_paste->snapshot.paste(
                        scheduler.projected_document(),
                        pending_clip_paste->request.scene,
                        pending_clip_paste->request.playhead, next_new_node);
                    if (planned) {
                        auto scheduled = scheduler.schedule_document_replace(
                            planned.value().document, cmd::UndoContextId{2U},
                            cmd::TransactionId{
                                pending_clip_paste->request.transaction},
                            frame);
                        if (!scheduled) {
                            std::cerr
                                << "could not author a clip paste for frame "
                                << frame.value() << '\n';
                            return 1;
                        }
                        next_new_node = planned.value().next_node;
                    } else if (planned.error() != ClipPasteError::Empty &&
                               planned.error() !=
                                   ClipPasteError::InvalidTargetScene) {
                        std::cerr
                            << "ignored invalid clip clipboard paste (code "
                            << static_cast<unsigned>(planned.error()) << ")\n";
                    }
                    pending_clip_paste.reset();
                }
                if (pending_project_settings.has_value()) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node};
                    if (!schedule_project_settings(
                            scheduler, *pending_project_settings, id, frame)) {
                        std::cerr
                            << "could not author project settings for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                    if (pending_project_settings->node == doc::NodeId{}) {
                        ++next_new_node;
                    }
                    pending_project_settings.reset();
                }
                for (const auto &edit : pending_properties) {
                    if (!schedule_property_edit(scheduler, edit, frame,
                                                coalescing_for(edit.gesture))) {
                        std::cerr
                            << "could not author an inspector edit for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_properties.clear();
                for (const auto &edit : pending_generics) {
                    if (!schedule_generic_edit(scheduler, edit, frame,
                                               coalescing_for(edit.gesture))) {
                        std::cerr
                            << "could not author an inspector edit for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_generics.clear();
                for (const auto &add : pending_adds) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_add(scheduler, add, id, frame,
                                      transport.anchor().rate)) {
                        std::cerr
                            << "could not author an element add for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_adds.clear();
                for (const auto &request : pending_asset_deletes) {
                    if (!schedule_asset_delete(scheduler, request, frame)) {
                        std::cerr << "could not delete an asset for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_asset_deletes.clear();
                for (const auto &request : pending_asset_proxy_edits) {
                    if (!schedule_asset_proxy_edit(scheduler, request, frame)) {
                        std::cerr << "could not author an asset proxy policy "
                                     "for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_asset_proxy_edits.clear();
                for (const auto &effect : pending_effects) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_effect(scheduler, effect, id, frame)) {
                        std::cerr << "could not author an effect for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_effects.clear();
                for (const auto &request : pending_film_grain_effect_edits) {
                    if (!schedule_film_grain_effect_edit(
                            scheduler, request, frame,
                            coalescing_for(request.gesture))) {
                        std::cerr << "could not author Film Grain parameters "
                                     "for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_film_grain_effect_edits.clear();
                for (const auto &request : pending_effect_deletes) {
                    if (!schedule_effect_delete(scheduler, request, frame)) {
                        std::cerr << "could not delete an effect for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_effect_deletes.clear();
                for (const auto &request : pending_hardware_places) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_hardware_place(scheduler, request, id,
                                                 frame)) {
                        std::cerr
                            << "could not author a hardware device for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_hardware_places.clear();
                for (const auto &request : pending_hardware_moves) {
                    if (!schedule_hardware_move(scheduler, request, frame)) {
                        std::cerr
                            << "could not author a hardware move for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_hardware_moves.clear();
                for (const auto &edit : pending_hardware_properties) {
                    if (!schedule_hardware_property(scheduler, edit, frame)) {
                        std::cerr
                            << "could not author hardware properties for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_hardware_properties.clear();
                if (pending_hardware_control_port.has_value()) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_hardware_control_port(
                            scheduler, *pending_hardware_control_port, id,
                            frame)) {
                        std::cerr << "could not author hardware control "
                                     "settings for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                    pending_hardware_control_port.reset();
                }
                if (pending_sync_policy.has_value()) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_sync_policy(scheduler, *pending_sync_policy,
                                              id, frame)) {
                        std::cerr << "could not author the sync policy for "
                                     "frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                    pending_sync_policy.reset();
                }
                for (const auto &edit : pending_hardware_output_binding_edits) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_hardware_output_binding(scheduler, edit, id,
                                                          frame)) {
                        std::cerr << "could not author an output binding for "
                                     "frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_hardware_output_binding_edits.clear();
                for (const auto &request :
                     pending_hardware_output_binding_deletes) {
                    if (!schedule_hardware_output_binding_delete(
                            scheduler, request, frame)) {
                        std::cerr << "could not remove an output binding for "
                                     "frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_hardware_output_binding_deletes.clear();
                for (const auto &request : pending_hardware_links) {
                    if (request.output_binding.has_value()) {
                        const topology::HardwareOutputBinding &binding =
                            *request.output_binding;
                        if (binding.node == doc::NodeId{}) {
                            while (scheduler.projected_document().contains(
                                doc::NodeId{next_new_node})) {
                                ++next_new_node;
                            }
                            const doc::NodeId binding_id{next_new_node++};
                            if (!schedule_hardware_output_binding(
                                    scheduler,
                                    editor::HardwareOutputBindingEdit{
                                        binding, request.gesture},
                                    binding_id, frame)) {
                                std::cerr << "could not bind a wired hardware "
                                             "output for frame "
                                          << frame.value() << '\n';
                                return 1;
                            }
                        }
                        if (find_feed_map_node(scheduler.projected_document(),
                                               binding.feed_index) == nullptr) {
                            while (scheduler.projected_document().contains(
                                doc::NodeId{next_new_node})) {
                                ++next_new_node;
                            }
                            const doc::NodeId feed_id{next_new_node++};
                            const editor::FeedMapAddRequest feed{
                                binding.feed_index, request.output_region,
                                scene::whole(scene::Canvas{binding.width,
                                                           binding.height}),
                                request.gesture};
                            if (!schedule_feed_map_add(
                                    scheduler, feed, feed_id, frame,
                                    cmd::UndoContextId{3U})) {
                                std::cerr << "could not create a region for a "
                                             "wired hardware "
                                             "output at frame "
                                          << frame.value() << '\n';
                                return 1;
                            }
                        }
                    }
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_hardware_link(scheduler, request, id,
                                                frame)) {
                        std::cerr
                            << "could not author a hardware link for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_hardware_links.clear();
                for (const auto &request : pending_hardware_deletes) {
                    if (!schedule_hardware_delete(scheduler, request, frame)) {
                        std::cerr << "could not delete hardware for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_hardware_deletes.clear();
                for (const auto &request : pending_feed_surface_adds) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_feed_surface_add(scheduler, request, id,
                                                   frame)) {
                        std::cerr
                            << "could not add a projection surface for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                    if (request.assign_map_node.has_value()) {
                        const editor::FeedProjectionEdit assignment{
                            *request.assign_map_node,
                            output::FeedProjection::PlanarSurface, id,
                            scene::whole(request.surface.raster),
                            request.gesture};
                        if (!schedule_feed_projection_edit(scheduler,
                                                           assignment, frame)) {
                            std::cerr << "could not assign a UV projection "
                                         "mesh for frame "
                                      << frame.value() << '\n';
                            return 1;
                        }
                    }
                }
                pending_feed_surface_adds.clear();
                for (const auto &request : pending_feed_map_adds) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_feed_map_add(scheduler, request, id, frame)) {
                        std::cerr << "could not add a feed map for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_feed_map_adds.clear();
                for (const auto &edit : pending_feed_map_edits) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_feed_map_edit(scheduler, edit, id, frame)) {
                        std::cerr
                            << "could not author a feed map edit for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_feed_map_edits.clear();
                for (const auto &edit : pending_feed_projection_edits) {
                    if (!schedule_feed_projection_edit(scheduler, edit,
                                                       frame)) {
                        std::cerr
                            << "could not edit a feed projection for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_feed_projection_edits.clear();
                for (const auto &request : pending_feed_surface_deletes) {
                    if (!schedule_feed_surface_delete(scheduler, request,
                                                      frame)) {
                        std::cerr << "could not delete a projection surface "
                                     "for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_feed_surface_deletes.clear();
                for (const auto &request : pending_feed_map_deletes) {
                    if (!schedule_feed_map_delete(scheduler, request, frame)) {
                        std::cerr << "could not delete a feed map for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_feed_map_deletes.clear();
                if (pending_compositor_add.has_value()) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_compositor_add(
                            scheduler, *pending_compositor_add, id,
                            authoring_scene(), pending_compositor_gesture,
                            frame)) {
                        std::cerr
                            << "could not add a compositor node for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                    pending_compositor_add.reset();
                }
                if (pending_compositor_move.has_value()) {
                    if (!schedule_compositor_move(
                            scheduler, *pending_compositor_move,
                            pending_compositor_gesture, frame)) {
                        std::cerr
                            << "could not move a compositor node for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                    pending_compositor_move.reset();
                }
                if (pending_compositor_property.has_value()) {
                    if (!schedule_compositor_property(
                            scheduler, *pending_compositor_property,
                            pending_compositor_gesture, frame,
                            coalescing_for(pending_compositor_gesture))) {
                        std::cerr << "could not edit a compositor parameter "
                                     "for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                    pending_compositor_property.reset();
                }
                if (pending_compositor_connect.has_value()) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_compositor_connect(
                            scheduler, *pending_compositor_connect, id,
                            pending_compositor_gesture, frame)) {
                        std::cerr
                            << "could not connect compositor nodes for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                    pending_compositor_connect.reset();
                }
                if (pending_compositor_delete.has_value()) {
                    if (!schedule_compositor_delete(
                            scheduler, *pending_compositor_delete,
                            pending_compositor_gesture, frame)) {
                        std::cerr
                            << "could not delete a compositor item for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                    pending_compositor_delete.reset();
                }
                pending_compositor_gesture = 0U;
                for (const auto node : pending_deletes) {
                    if (!schedule_delete(scheduler, node,
                                         pending_delete_gesture, frame)) {
                        std::cerr
                            << "could not author an element delete for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_deletes.clear();
                if (pending_switcher.has_value()) {
                    const editor::SwitcherRequest request = *pending_switcher;
                    pending_switcher.reset();
                    const cmd::UndoContextId switcher_context{5U};
                    const cmd::TransactionId switcher_transaction{
                        frame.value()};

                    scene::SceneModel project_scenes;
                    project_scenes.reset(scheduler.projected_document());
                    const std::vector<scene::SceneInfo> scenes =
                        project_scenes.scenes();
                    std::optional<show::SwitcherAnchor> anchor =
                        show::read_switcher(scheduler.projected_document());

                    const auto scene_for =
                        [&](scene::SceneId id) -> const scene::SceneInfo * {
                        const auto found =
                            std::find_if(scenes.begin(), scenes.end(),
                                         [id](const scene::SceneInfo &info) {
                                             return info.id == id;
                                         });
                        return found == scenes.end() ? nullptr : &*found;
                    };
                    const auto dissolve_pair_compatible =
                        [&](scene::SceneId program, scene::SceneId rehearsal) {
                            const scene::SceneInfo *const program_info =
                                scene_for(program);
                            const scene::SceneInfo *const rehearsal_info =
                                scene_for(rehearsal);
                            return program_info != nullptr &&
                                   rehearsal_info != nullptr &&
                                   show::dissolve_canvases_compatible(
                                       program_info->canvas,
                                       rehearsal_info->canvas);
                        };

                    const auto author_switcher =
                        [&](const show::SwitcherAnchor &next) {
                            return static_cast<bool>(
                                show::schedule_switcher_anchor(
                                    scheduler, next, frame, switcher_context,
                                    switcher_transaction));
                        };

                    // A scene copy has to bring its composite with it. The
                    // scene layer owns the drawables; the compositor graph owns
                    // how they are stacked and treated, and a copy that lost
                    // the graph would look nothing like the scene it came from
                    // the moment it was promoted.
                    const scene::SceneId legacy_graph_owner =
                        anchor.has_value()
                            ? anchor->program_scene
                            : (!scenes.empty() ? scenes.front().id
                                               : scene::SceneId{});
                    const auto author_graph_clone =
                        [&](const scene::SceneClonePlan &plan) -> bool {
                        if (legacy_graph_owner == scene::SceneId{}) {
                            return true;
                        }
                        auto planned = compositor::plan_clone_graph(
                            scheduler.projected_document(), plan.source,
                            plan.destination, legacy_graph_owner, plan.remap,
                            doc::NodeId{next_new_node});
                        if (!planned) {
                            return false;
                        }
                        const compositor::GraphClonePlan graph =
                            std::move(planned).value();
                        const bool cloned_output = std::any_of(
                            graph.puts.begin(), graph.puts.end(),
                            [](const doc::Node &node) {
                                return node.type ==
                                       compositor::schema::output_type;
                            });
                        for (const doc::Node &node : graph.puts) {
                            if (!scheduler.schedule_edit(
                                    cmd::PutNode{node}, cmd::EraseNode{node.id},
                                    switcher_context, switcher_transaction,
                                    frame)) {
                                return false;
                            }
                        }
                        for (const auto &[source, destination] : graph.remap) {
                            static_cast<void>(source);
                            next_new_node = std::max(next_new_node,
                                                     destination.value() + 1U);
                        }
                        if (!cloned_output) {
                            while (scheduler.projected_document().contains(
                                doc::NodeId{next_new_node})) {
                                ++next_new_node;
                            }
                            const doc::Node output =
                                default_compositor_output_node(
                                    doc::NodeId{next_new_node++},
                                    plan.destination);
                            if (!scheduler.schedule_edit(
                                    cmd::PutNode{output},
                                    cmd::EraseNode{output.id}, switcher_context,
                                    switcher_transaction, frame)) {
                                return false;
                            }
                        }
                        return true;
                    };

                    if (request.action ==
                        editor::SwitcherAction::ToggleRehearse) {
                        if (rehearsal_mode) {
                            leave_preview();
                        } else if (anchor.has_value()) {
                            enter_preview(frame);
                        } else if (!scenes.empty()) {
                            auto clone = scene::plan_clone_scene(
                                scheduler.projected_document(),
                                scenes.front().id, doc::NodeId{next_new_node});
                            if (!clone) {
                                std::cerr << "could not clone Program into "
                                             "Rehearsal\n";
                                return 1;
                            }
                            scene::SceneClonePlan plan =
                                std::move(clone).value();
                            for (doc::Node &node : plan.puts) {
                                if (node.id == plan.destination) {
                                    const std::string rehearsal_name =
                                        scenes.front().name.empty()
                                            ? "Rehearsal"
                                            : scenes.front().name +
                                                  " Rehearsal";
                                    node.properties.insert_or_assign(
                                        std::string{scene::schema::scene_name},
                                        doc::PropertyValue{rehearsal_name});
                                }
                                if (!scheduler.schedule_edit(
                                        cmd::PutNode{node},
                                        cmd::EraseNode{node.id},
                                        switcher_context, switcher_transaction,
                                        frame)) {
                                    std::cerr
                                        << "could not author Rehearsal clone\n";
                                    return 1;
                                }
                            }
                            for (const auto &[source, destination] :
                                 plan.remap) {
                                static_cast<void>(source);
                                next_new_node = std::max(
                                    next_new_node, destination.value() + 1U);
                            }
                            if (!author_graph_clone(plan)) {
                                std::cerr << "could not author the Rehearsal "
                                             "compositor graph\n";
                                return 1;
                            }
                            auto switcher_created =
                                show::SwitcherAuthority::create(
                                    plan.source, plan.destination, frame);
                            if (!switcher_created ||
                                !author_switcher(std::move(switcher_created)
                                                     .value()
                                                     .anchor())) {
                                std::cerr << "could not author "
                                             "Program/Rehearsal slots\n";
                                return 1;
                            }
                            enter_preview(frame);
                        }
                    } else if (request.action == editor::SwitcherAction::Swap) {
                        // Swap promotes the scene the operator has been
                        // rehearsing. Its conditions belong to the switcher
                        // itself - a Preview scene, no transition already
                        // running, and for a dissolve one shared canvas - plus
                        // the STRICT synchronization hold the surface reports.
                        // Whether render nodes hold the media is reported on
                        // the Output Monitor, not enforced here: distributing
                        // content to them is an external concern in this build.
                        if (program_sync_held()) {
                            project_message =
                                std::string{director_sync.readiness.headline()};
                        } else if (anchor.has_value() &&
                                   !show::derive_switcher_progress(*anchor,
                                                                   frame)
                                        .active &&
                                   (request.transition_control_frames == 0U ||
                                    dissolve_pair_compatible(
                                        anchor->program_scene,
                                        anchor->rehearsal_scene))) {
                            auto authority = show::SwitcherAuthority::create(
                                anchor->program_scene, anchor->rehearsal_scene,
                                frame);
                            auto swapped =
                                authority
                                    ? std::move(authority).value().swap(
                                          frame,
                                          request.transition_control_frames)
                                    : core::Result<show::SwitcherAnchor,
                                                   show::SwitcherCommandError>::
                                          failure(show::SwitcherCommandError::
                                                      InvalidScene);
                            if (!swapped ||
                                !author_switcher(std::move(swapped).value())) {
                                std::cerr << "could not swap Program and "
                                             "Rehearsal\n";
                                return 1;
                            }
                            // Preview now holds what Program was showing.
                            // Re-seed its playhead so the operator picks up
                            // from the picture that just came off air.
                            if (rehearsal_mode) {
                                enter_preview(frame);
                            }
                        }
                    } else if (request.action ==
                               editor::SwitcherAction::SelectRehearsal) {
                        const scene::SceneId program =
                            anchor.has_value()
                                ? anchor->program_scene
                                : (!scenes.empty() ? scenes.front().id
                                                   : scene::SceneId{});
                        // Preview accepts any scene. Whether the two canvases
                        // allow a dissolve is decided when the operator asks
                        // for one, not when they choose what to work on.
                        if (request.scene != program &&
                            (!anchor.has_value() ||
                             !show::derive_switcher_progress(*anchor, frame)
                                  .active)) {
                            auto authority = show::SwitcherAuthority::create(
                                program,
                                anchor.has_value() ? anchor->rehearsal_scene
                                                   : request.scene,
                                frame);
                            std::optional<show::SwitcherAnchor> selected;
                            if (authority) {
                                auto state = std::move(authority).value();
                                if (!anchor.has_value() ||
                                    request.scene == anchor->rehearsal_scene) {
                                    selected = state.anchor();
                                } else {
                                    auto changed = state.set_rehearsal(
                                        request.scene, frame);
                                    if (changed) {
                                        selected = std::move(changed).value();
                                    }
                                }
                            }
                            if (!selected.has_value() ||
                                !author_switcher(*selected)) {
                                std::cerr
                                    << "could not select Rehearsal scene\n";
                                return 1;
                            }
                            enter_preview(frame);
                        }
                    } else if (request.action ==
                                   editor::SwitcherAction::DuplicateScene &&
                               (!anchor.has_value() ||
                                !show::derive_switcher_progress(*anchor, frame)
                                     .active)) {
                        const scene::SceneInfo *const source_info =
                            scene_for(request.scene);
                        if (source_info != nullptr) {
                            auto cloned = scene::plan_clone_scene(
                                scheduler.projected_document(), request.scene,
                                doc::NodeId{next_new_node});
                            if (!cloned) {
                                std::cerr << "could not plan a scene copy\n";
                                return 1;
                            }
                            scene::SceneClonePlan plan =
                                std::move(cloned).value();
                            for (doc::Node &node : plan.puts) {
                                if (node.id == plan.destination) {
                                    const std::string copy_name =
                                        source_info->name.empty()
                                            ? "Scene Copy"
                                            : source_info->name + " Copy";
                                    node.properties.insert_or_assign(
                                        std::string{scene::schema::scene_name},
                                        doc::PropertyValue{copy_name});
                                }
                                if (!scheduler.schedule_edit(
                                        cmd::PutNode{node},
                                        cmd::EraseNode{node.id},
                                        switcher_context, switcher_transaction,
                                        frame)) {
                                    std::cerr
                                        << "could not author a scene copy\n";
                                    return 1;
                                }
                            }
                            for (const auto &[source, destination] :
                                 plan.remap) {
                                static_cast<void>(source);
                                next_new_node = std::max(
                                    next_new_node, destination.value() + 1U);
                            }
                            if (!author_graph_clone(plan)) {
                                std::cerr << "could not author the scene copy "
                                             "compositor graph\n";
                                return 1;
                            }
                            const scene::SceneId program =
                                anchor.has_value()
                                    ? anchor->program_scene
                                    : (!scenes.empty() ? scenes.front().id
                                                       : request.scene);
                            auto copied_switcher =
                                show::SwitcherAuthority::create(
                                    program, plan.destination, frame);
                            if (!copied_switcher ||
                                !author_switcher(std::move(copied_switcher)
                                                     .value()
                                                     .anchor())) {
                                std::cerr
                                    << "could not assign the scene copy to "
                                       "Rehearsal\n";
                                return 1;
                            }
                            enter_preview(frame);
                        }
                    } else if (request.action ==
                                   editor::SwitcherAction::CreateScene &&
                               (!anchor.has_value() ||
                                !show::derive_switcher_progress(*anchor, frame)
                                     .active)) {
                        const scene::SceneId existing_program =
                            anchor.has_value()
                                ? anchor->program_scene
                                : (!scenes.empty() ? scenes.front().id
                                                   : scene::SceneId{});
                        const scene::SceneInfo *const program_info =
                            scene_for(existing_program);
                        const scene::Canvas created_canvas =
                            program_info != nullptr
                                ? program_info->canvas
                                : scene::Canvas{1'920U, 1'080U};
                        while (scheduler.projected_document().contains(
                            doc::NodeId{next_new_node})) {
                            ++next_new_node;
                        }
                        const scene::SceneId created_scene{next_new_node++};
                        while (scheduler.projected_document().contains(
                            doc::NodeId{next_new_node})) {
                            ++next_new_node;
                        }
                        const doc::NodeId camera_id{next_new_node++};
                        while (scheduler.projected_document().contains(
                            doc::NodeId{next_new_node})) {
                            ++next_new_node;
                        }
                        const doc::NodeId output_id{next_new_node++};
                        doc::Node stage{};
                        stage.id = created_scene;
                        stage.type = std::string{scene::schema::stage_type};
                        stage.properties.emplace(
                            std::string{scene::schema::scene_name},
                            doc::PropertyValue{
                                "Scene " + std::to_string(scenes.size() + 1U)});
                        stage.properties.emplace(
                            std::string{scene::schema::canvas_width},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                created_canvas.width)});
                        stage.properties.emplace(
                            std::string{scene::schema::canvas_height},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                created_canvas.height)});
                        doc::Node camera{};
                        camera.id = camera_id;
                        camera.type = std::string{scene::schema::camera_type};
                        camera.properties.emplace(
                            std::string{scene::schema::scene_id},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                created_scene.value())});
                        camera.properties.emplace(
                            std::string{scene::schema::eye_z},
                            doc::PropertyValue{6.0});
                        const doc::Node output = default_compositor_output_node(
                            output_id, created_scene);
                        for (const doc::Node &node : {stage, camera, output}) {
                            if (!scheduler.schedule_edit(
                                    cmd::PutNode{node}, cmd::EraseNode{node.id},
                                    switcher_context, switcher_transaction,
                                    frame)) {
                                std::cerr << "could not create a scene\n";
                                return 1;
                            }
                        }
                        const scene::SceneId program =
                            existing_program != scene::SceneId{}
                                ? existing_program
                                : created_scene;
                        if (program != created_scene) {
                            auto scene_switcher =
                                show::SwitcherAuthority::create(
                                    program, created_scene, frame);
                            if (!scene_switcher ||
                                !author_switcher(std::move(scene_switcher)
                                                     .value()
                                                     .anchor())) {
                                std::cerr << "could not assign the new scene "
                                             "to Rehearsal\n";
                                return 1;
                            }
                            enter_preview(frame);
                        }
                    } else if (request.action ==
                                   editor::SwitcherAction::DeleteScene &&
                               (!anchor.has_value() ||
                                !show::derive_switcher_progress(*anchor, frame)
                                     .active)) {
                        const scene::SceneId program =
                            anchor.has_value()
                                ? anchor->program_scene
                                : (!scenes.empty() ? scenes.front().id
                                                   : scene::SceneId{});
                        // Program is the protected live scene. All other
                        // scenes may be removed, including Rehearsal; in that
                        // case select a remaining off-air scene or remove the
                        // two-slot switcher when Program will be the only one
                        // left.
                        if (request.scene != program &&
                            scene_for(request.scene) != nullptr) {
                            auto planned = scene::plan_delete_scene(
                                scheduler.projected_document(), request.scene);
                            if (!planned) {
                                std::cerr << "could not plan scene deletion\n";
                                return 1;
                            }
                            if (anchor.has_value() &&
                                anchor->rehearsal_scene == request.scene) {
                                const auto fallback = std::find_if(
                                    scenes.begin(), scenes.end(),
                                    [&](const scene::SceneInfo &info) {
                                        return info.id != program &&
                                               info.id != request.scene;
                                    });
                                if (fallback != scenes.end()) {
                                    auto replacement =
                                        show::SwitcherAuthority::create(
                                            program, fallback->id, frame);
                                    if (!replacement ||
                                        !author_switcher(std::move(replacement)
                                                             .value()
                                                             .anchor())) {
                                        std::cerr
                                            << "could not replace the "
                                               "deleted Rehearsal scene\n";
                                        return 1;
                                    }
                                    if (rehearsal_mode) {
                                        enter_preview(frame);
                                    }
                                } else {
                                    if (!schedule_erase_node(
                                            scheduler, show::switcher_node_id,
                                            switcher_context,
                                            switcher_transaction, frame)) {
                                        std::cerr << "could not remove the "
                                                     "empty Rehearsal slot\n";
                                        return 1;
                                    }
                                    leave_preview();
                                }
                            }
                            for (const doc::NodeId id :
                                 planned.value().erases) {
                                if (!schedule_erase_node(
                                        scheduler, id, switcher_context,
                                        switcher_transaction, frame)) {
                                    std::cerr << "could not delete scene\n";
                                    return 1;
                                }
                            }
                        }
                    }
                }
                if (pending_range) {
                    // Set the show length and loop, then let the ordinary
                    // anchor authoring below replicate the new bounds.
                    static_cast<void>(transport.set_range(
                        show::ShowFrame{pending_range->first},
                        show::ShowFrame{pending_range->last},
                        pending_range->loop, frame));
                    project_explicitly_dirty = true;
                    pending_range.reset();
                }
                if (pending_show_rate.has_value()) {
                    const show::FrameRate requested{
                        pending_show_rate->numerator,
                        pending_show_rate->denominator};
                    if (!transport.set_rate(requested, frame)) {
                        std::cerr
                            << "could not apply the requested show rate\n";
                        return 1;
                    }
                    repace_after_seal = requested;
                    project_explicitly_dirty = true;
                    pending_show_rate.reset();
                }
                if (pending_seek) {
                    // Scrubbing is a paused operation, like the keyboard step
                    // and jump commands: pause, then seek to the dragged frame,
                    // clamped to the live show range. The new anchor replicates
                    // like any other transport change.
                    if (transport.state() == show::TransportState::Playing) {
                        static_cast<void>(transport.pause(frame));
                    }
                    const auto target = std::min(
                        std::max(*pending_seek,
                                 transport.anchor().first_frame.value()),
                        transport.anchor().last_frame.value());
                    static_cast<void>(
                        transport.seek(show::ShowFrame{target}, frame));
                    pending_seek.reset();
                }
                if (pending_history.has_value()) {
                    // Navigate the editor's most recently authored scene or
                    // compositor context one whole gesture at a time. Both
                    // actions author fresh commands; NoHistory only means that
                    // the requested direction has no next step.
                    const bool undo =
                        pending_history->action == editor::HistoryAction::Undo;
                    const cmd::UndoContextId history_context{
                        pending_history->context ==
                                editor::HistoryContext::Compositor
                            ? 6U
                            : 2U};
                    auto navigated =
                        undo ? scheduler.schedule_undo(
                                   history_context,
                                   cmd::TransactionId{
                                       pending_history->transaction},
                                   frame)
                             : scheduler.schedule_redo(
                                   history_context,
                                   cmd::TransactionId{
                                       pending_history->transaction},
                                   frame);
                    if (!navigated && navigated.error() !=
                                          cmd::DirectorUndoError::NoHistory) {
                        std::cerr << "could not author a scene-edit "
                                  << (undo ? "undo" : "redo") << " for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                    pending_history.reset();
                }
                if (pending_project_save.has_value()) {
                    const editor::ProjectFileRequest request =
                        std::move(*pending_project_save);
                    pending_project_save.reset();
                    doc::Root snapshot = scheduler.projected_document();
                    show::TransportAnchor saved_transport = transport.anchor();
                    saved_transport.anchor_show_frame =
                        transport.frame_at(frame);
                    saved_transport.anchor_control_frame = frame;
                    snapshot = snapshot.set(
                        show::make_transport_node(saved_transport));
                    auto saved = persist::save_project_file(
                        native_path_from_utf8(request.path), snapshot);
                    const bool installed = !saved && saved.error().committed;
                    if (!saved && !installed) {
                        project_message =
                            project_file_failure("SAVING", saved.error());
                        std::cerr << "could not save project '" << request.path
                                  << "' (code "
                                  << static_cast<unsigned>(saved.error().code)
                                  << ')';
                        if (saved.error().system_error) {
                            std::cerr << ": "
                                      << saved.error().system_error.message();
                        }
                        std::cerr << '\n';
                    } else {
                        saved_project_snapshot = scheduler.projected_document();
                        project_explicitly_dirty = false;
                        const bool asset_base_changed =
                            project_path != request.path;
                        project_path = request.path;
                        // Save As changes the base of project-relative asset
                        // paths even though authored content is unchanged.
                        if (asset_base_changed) {
                            assets_loaded = false;
                        }
                        remember_recent_project(project_path);
                        project_message =
                            installed ? "ERROR SAVE: DURABILITY NOT CONFIRMED"
                                      : "PROJECT SAVED";
                        if (installed) {
                            std::cerr
                                << "project '" << request.path
                                << "' was replaced, but durable directory "
                                   "sync failed";
                            if (saved.error().system_error) {
                                std::cerr
                                    << ": "
                                    << saved.error().system_error.message();
                            }
                            std::cerr << '\n';
                        }
                    }
                }
                pending_editor_coalescing.clear();
                if (pending_editor_coalescing_close) {
                    scheduler.end_coalescing();
                    pending_editor_coalescing_close = false;
                }
#endif
            }
            static_cast<void>(transport.settle(frame));

            // The anchor travels as an ordinary command inside a sealed frame,
            // so every node derives the position from the same revision. An
            // unchanged anchor authors nothing, so a playing show costs the
            // document no edits at all -- which is what removes the per-frame
            // durable write behind it.
            if (!authored || *authored != transport.anchor()) {
                const auto command = show::schedule_transport_anchor(
                    scheduler, transport.anchor(), frame,
                    cmd::UndoContextId{1U}, cmd::TransactionId{frame.value()});
                if (!command) {
                    std::cerr
                        << "could not author the transport anchor for frame "
                        << frame.value() << " (code "
                        << static_cast<unsigned>(command.error()) << ")\n";
                    return 1;
                }
                authored = transport.anchor();
                ++anchors;
            }

            auto sealed_frame = barrier.seal_and_track(hub, frame);
            if (!sealed_frame) {
                std::cerr << "Director could not seal frame " << frame.value()
                          << " (code "
                          << static_cast<unsigned>(sealed_frame.error().code)
                          << ")\n";
                return 1;
            }
            tracked.push_back(sealed_frame.value().barrier);
            ++sealed;
            ++sealed_this_iteration;
            if (repace_after_seal.has_value()) {
                // `due` was calculated at the old cadence. Once the boundary
                // carrying the new rate is sealed, discard the remaining old-
                // rate debt and make the next boundary one complete period at
                // the new exact rational rate after this same observation.
                if (!pacer.seek(show::ShowFrame{frame.value()}, now) ||
                    !pacer.reconfigure_rate(*repace_after_seal, now)) {
                    std::cerr << "could not re-anchor the Director show rate\n";
                    return 1;
                }
                interval = status_interval(options, *repace_after_seal);
                since_status = 0U;
                break;
            }
        }

        // Retire in order. A frame whose cohort has not finished stays, and so
        // does everything behind it, which is what bounds the backlog above.
        while (!tracked.empty() && barrier.retire(tracked.front())) {
            tracked.pop_front();
        }

        const auto control_frame = scheduler.sealed_through();
        show::TestPatternInput input{};
        input.show_frame = transport.frame_at(control_frame);
        input.control_frame = control_frame;
        input.rate = transport.anchor().rate;
        input.state = transport.state();
        input.node_index = options.node_index;
        input.held = sealed_this_iteration == 0U;
        // Where the Stage is looking. In Preview that is the operator's own
        // playhead, derived from the same control-frame stream but from a
        // Director-local anchor; otherwise it is exactly the show frame.
        const show::ShowFrame stage_show_frame =
            preview_transport.has_value()
                ? preview_transport->frame_at(control_frame)
                : input.show_frame;
        [[maybe_unused]] const bool stage_playing =
            preview_transport.has_value()
                ? preview_transport->state() == show::TransportState::Playing
                : transport.state() == show::TransportState::Playing;

        // The Director previews the whole canvas of the replicated scene, so it
        // shows what the wall shows between its nodes. It evaluates from its
        // own committed document rather than a mailbox.
        const bool render_scene =
            presenter != nullptr && presenter->renders_scenes();
        // The scene is evaluated for the preview or the editor: both work on
        // the same committed scene at the show frame.
        const bool needs_scene = render_scene || options.edit;
        scene::Scene scene_frame;
        core::u64 authored_scene_digest{};
        scene::Scene stage_scene_frame;
        scene::Scene transition_from_frame;
        scene::Scene transition_to_frame;
        std::vector<scene::SceneInfo> scene_infos;
        std::optional<show::SwitcherAnchor> switcher_anchor;
        show::SwitcherProgress switcher_progress{};
        output::FeedMap feed_map;
        topology::HardwareMap hardware_map;
        assets::AssetRegistry &asset_registry = director_asset_registry;
        ProjectAssetManifest asset_manifest;
        // The graph of the scene on the Stage - the one the Compositor panel
        // edits. Program renders its own, which is the same object except while
        // Preview is showing a different scene.
        compositor::CompositorGraph compositor_graph;
        compositor::CompositorGraph program_compositor_graph;
        compositor::CompositorGraph transition_from_graph;
        compositor::CompositorGraph transition_to_graph;
        scene::SceneId stage_scene{};
        show::PresenterStatus status{show::PresenterStatus::Skipped};
        if (needs_scene) {
            const auto &document = scheduler.sealed_document();
            if (!scene_loaded || document.version().value() != scene_version) {
                scene_model.reset(document);
                scene_version = document.version().value();
                scene_loaded = true;
            }
            if (!compositor_loaded ||
                document.version().value() != compositor_version) {
                compositor_model.reset(document);
                compositor_version = document.version().value();
                compositor_loaded = true;
            }
            if (!assets_loaded || document.version().value() != asset_version) {
                asset_model.reset(document);
                asset_registry = asset_model.evaluate();
                director_playback_asset_registry =
                    options.edit ? editor_playback_registry(asset_registry)
                                 : asset_registry;
                if (director_proxy_generator != nullptr) {
                    // Only the editor's working derivatives belong on this
                    // machine. Authored full-resolution output proxies are
                    // generated independently by each output node.
                    for (const assets::Asset &entry :
                         director_playback_asset_registry.assets) {
                        if (const auto *const media =
                                std::get_if<assets::MediaAsset>(&entry);
                            media != nullptr &&
                            media->requested_proxy_profile ==
                                assets::DerivativeProfileId::Proxy720pCfrH264) {
                            director_proxy_generator->request(*media);
                        }
                    }
                }
                const core::u64 registry_digest =
                    assets::digest(asset_registry);
                if (!assets_loaded ||
                    registry_digest != asset_registry_digest) {
                    director_proxy_playback.reset();
                    asset_registry_digest = registry_digest;
                }
                const ProjectAssetManifest manifest =
                    collect_project_asset_manifest(document, asset_registry);
                // The Director is one more machine that has to be able to
                // open these paths, and it answers the question the same way
                // a render node does.
                if (!assets_loaded ||
                    manifest.digest != asset_manifest_digest) {
                    asset_manifest_digest = manifest.digest;
                    director_asset_availability = scan_project_assets(manifest);
                }
                asset_version = document.version().value();
                assets_loaded = true;
            } else if (pending_asset_rescan) {
                // The operator asked. This is the only path that reads a
                // source's bytes, and the only one that can report a content
                // mismatch: a show is rebuilt constantly and nothing on the
                // frame path is allowed to wait on a disk read.
                asset_model.reset(document);
                asset_registry = asset_model.evaluate();
                director_playback_asset_registry =
                    options.edit ? editor_playback_registry(asset_registry)
                                 : asset_registry;
                if (director_proxy_generator != nullptr) {
                    for (const assets::Asset &entry :
                         director_playback_asset_registry.assets) {
                        if (const auto *const media =
                                std::get_if<assets::MediaAsset>(&entry);
                            media != nullptr &&
                            media->requested_proxy_profile ==
                                assets::DerivativeProfileId::Proxy720pCfrH264) {
                            director_proxy_generator->request(*media);
                        }
                    }
                }
                director_asset_availability = check_project_assets(
                    collect_project_asset_manifest(document, asset_registry));
                project_message = director_asset_availability.ready()
                                      ? "ASSETS VERIFIED"
                                      : "ASSETS INCOMPLETE - SEE LIBRARY";
            }
            pending_asset_rescan = false;
            const scene::SceneTime time{input.show_frame.value(),
                                        input.rate.numerator,
                                        input.rate.denominator};
            // The show's repeat, for the warm-up across its wrap. Program only:
            // Preview runs its own playhead over its own range, and warming it
            // against the show's would name frames it is not heading for.
            const scene::ShowLoop show_loop{
                transport.anchor().first_frame.value(),
                transport.anchor().last_frame.value(), transport.anchor().loop};
            // Preview evaluates the scene it is rehearsing at its own playhead.
            // Program is evaluated at the show frame in the same iteration, so
            // scrubbing one never disturbs the other.
            const scene::SceneTime stage_time{stage_show_frame.value(),
                                              input.rate.numerator,
                                              input.rate.denominator};
            scene_infos = scene_model.scenes();
            switcher_anchor = show::read_switcher(document);
            // A graph object authored before graphs were scene-scoped carries
            // no owner. It used to apply to whatever Program was showing, so
            // that is the scene it is adopted into - which keeps the wall
            // looking exactly as it did when the project was saved.
            const scene::SceneId legacy_graph_owner =
                switcher_anchor.has_value()
                    ? switcher_anchor->program_scene
                    : (scene_infos.empty() ? scene::SceneId{}
                                           : scene_infos.front().id);
            if (switcher_anchor.has_value()) {
                switcher_progress = show::derive_switcher_progress(
                    *switcher_anchor, control_frame);
                scene_frame = scene_model.evaluate(
                    switcher_anchor->program_scene, time, show_loop);
                stage_scene = rehearsal_mode ? switcher_anchor->rehearsal_scene
                                             : switcher_anchor->program_scene;
                stage_scene_frame =
                    scene_model.evaluate(stage_scene, stage_time);
                program_compositor_graph = compositor_model.evaluate(
                    switcher_anchor->program_scene, legacy_graph_owner);
                compositor_graph = rehearsal_mode
                                       ? compositor_model.evaluate(
                                             stage_scene, legacy_graph_owner)
                                       : program_compositor_graph;
                if (switcher_progress.active) {
                    transition_from_frame = scene_model.evaluate(
                        switcher_progress.from, time, show_loop);
                    transition_to_frame = scene_model.evaluate(
                        switcher_progress.to, time, show_loop);
                    transition_from_graph = compositor_model.evaluate(
                        switcher_progress.from, legacy_graph_owner);
                    transition_to_graph = compositor_model.evaluate(
                        switcher_progress.to, legacy_graph_owner);
                    if (!show::dissolve_canvases_compatible(
                            transition_from_frame.canvas,
                            transition_to_frame.canvas)) {
                        switcher_progress.active = false;
                    }
                }
            } else {
                scene_frame = scene_model.evaluate(time, show_loop);
                stage_scene_frame = scene_frame;
                stage_scene = scene_frame.id;
                compositor_graph = compositor_model.evaluate(
                    scene_frame.id, legacy_graph_owner);
                program_compositor_graph = compositor_graph;
            }

            authored_scene_digest = scene::digest(scene_frame);

            std::vector<ProxyPlacementObservation> observations;
            bool proxies_ready = resolve_scene_proxies(
                director_proxy_playback, scene_frame,
                director_playback_asset_registry, director_asset_availability,
                observations);
            proxies_ready = resolve_scene_proxies(
                                director_proxy_playback, stage_scene_frame,
                                director_playback_asset_registry,
                                director_asset_availability, observations) &&
                            proxies_ready;
            if (switcher_progress.active) {
                proxies_ready =
                    resolve_scene_proxies(
                        director_proxy_playback, transition_from_frame,
                        director_playback_asset_registry,
                        director_asset_availability, observations) &&
                    proxies_ready;
                proxies_ready =
                    resolve_scene_proxies(
                        director_proxy_playback, transition_to_frame,
                        director_playback_asset_registry,
                        director_asset_availability, observations) &&
                    proxies_ready;
            }
            report_proxy_status_changes("Director", observations,
                                        director_proxy_observations);
            director_proxy_observations = std::move(observations);
            static_cast<void>(proxies_ready);
        }

        if (options.edit) {
            const auto &document = scheduler.sealed_document();
            if (!feed_loaded || document.version().value() != feed_version) {
                feed_model.reset(document);
                feed_version = document.version().value();
                feed_loaded = true;
            }
            feed_map = feed_model.evaluate(
                scene::SceneTime{input.show_frame.value(), input.rate.numerator,
                                 input.rate.denominator});
            if (!hardware_loaded ||
                document.version().value() != hardware_version) {
                hardware_model.reset(document);
                hardware_version = document.version().value();
                hardware_loaded = true;
            }
            hardware_map = hardware_model.evaluate();
            asset_manifest =
                collect_project_asset_manifest(document, asset_registry);
        }

        // The synchronization policy and the groups derived from the authored
        // cabling are pure document projections, so they are refreshed even on
        // a Director with no editor: the diagnostic stream should record what
        // the project asked for whether or not anyone is watching.
        {
            const doc::Root &sync_document = scheduler.sealed_document();
            if (!director_sync_loaded ||
                sync_document.version().value() != director_sync_version) {
                director_sync_model.reset(sync_document);
                director_sync_version = sync_document.version().value();
                director_sync_loaded = true;
                director_sync.policy = director_sync_model.evaluate();
            }
            director_sync.topology =
                sync::derive_sync_topology(hardware_map, director_sync.policy);
            director_sync.readiness = evaluate_sync_gate(true);
        }

        bool handled = false;
#if defined(MEDIASERVER_HAS_GUI)
        if (options.edit && editor) {
            handled = true;
            // The timeline authors against the Stage playhead and reports the
            // Program one beside it, so an operator rehearsing at frame 900
            // can still read where the live show is.
            const editor::EditorTimeline timeline{
                stage_show_frame.value(),
                input.show_frame.value(),
                transport.anchor().first_frame.value(),
                transport.anchor().last_frame.value(),
                stage_playing,
                transport.state() == show::TransportState::Playing,
                transport.anchor().loop,
                transport.anchor().rate.numerator,
                transport.anchor().rate.denominator};
            const auto stage_clips = scene_model.clips(stage_scene_frame.id);
            const auto stage_lanes = scene_model.lanes(stage_scene_frame.id);
            std::vector<scene::SelectionSetSummary> selection_set_summaries;
            auto selection_set_catalog = scene::selection_sets(
                scheduler.sealed_document(), stage_scene_frame.id);
            if (selection_set_catalog) {
                selection_set_summaries =
                    std::move(selection_set_catalog).value().sets;
            } else if (selection_set_catalog.error().code ==
                       scene::SelectionSetErrorCode::AllocationFailed) {
                std::cerr
                    << "could not project the Stage Selection Sets catalog\n";
                return 1;
            }
            const auto elements =
                scene_elements(scheduler, stage_scene_frame, stage_clips,
                               stage_show_frame.value(), stage_lanes);
            const auto all_effects = scene_model.effects();
            std::vector<scene::EffectInstance> effects;
            for (const scene::EffectInstance &effect : all_effects) {
                const bool in_stage =
                    std::any_of(elements.begin(), elements.end(),
                                [&](const editor::ElementInfo &element) {
                                    return element.node == effect.target;
                                });
                if (in_stage) {
                    effects.push_back(effect);
                }
            }
            const auto all_keyframes = scene_model.channel_keyframes();
            std::vector<scene::ChannelKeyframes> keyframes;
            for (const scene::ChannelKeyframes &channel : all_keyframes) {
                const bool element_channel =
                    std::any_of(elements.begin(), elements.end(),
                                [&](const editor::ElementInfo &element) {
                                    return element.node == channel.target;
                                });
                const bool effect_channel =
                    std::any_of(effects.begin(), effects.end(),
                                [&](const scene::EffectInstance &effect) {
                                    return effect.source == channel.target;
                                });
                const bool surface_channel =
                    feed_map.surface_for(channel.target) != nullptr;
                if (element_channel || effect_channel || surface_channel) {
                    keyframes.push_back(channel);
                }
            }
            editor::EditorSwitcherState switcher_view{};
            switcher_view.scenes = scene_infos;
            switcher_view.program_scene = switcher_anchor.has_value()
                                              ? switcher_anchor->program_scene
                                              : scene_frame.id;
            switcher_view.rehearsal_scene =
                switcher_anchor.has_value() ? switcher_anchor->rehearsal_scene
                                            : scene::SceneId{};
            switcher_view.stage_scene = &stage_scene_frame;
            switcher_view.program_graph = &program_compositor_graph;
            switcher_view.transition_from =
                switcher_progress.active ? &transition_from_frame : nullptr;
            switcher_view.transition_to =
                switcher_progress.active ? &transition_to_frame : nullptr;
            switcher_view.transition_from_graph =
                switcher_progress.active ? &transition_from_graph : nullptr;
            switcher_view.transition_to_graph =
                switcher_progress.active ? &transition_to_graph : nullptr;
            switcher_view.transition_mix =
                static_cast<float>(switcher_progress.mix());
            switcher_view.transition_active = switcher_progress.active;
            switcher_view.rehearsal_mode = rehearsal_mode;
            switcher_view.rehearsal_locked = switcher_progress.active;
            switcher_view.dissolve_compatible = false;
            if (switcher_anchor.has_value()) {
                const auto program_info = std::find_if(
                    scene_infos.begin(), scene_infos.end(),
                    [&](const scene::SceneInfo &info) {
                        return info.id == switcher_anchor->program_scene;
                    });
                const auto rehearsal_info = std::find_if(
                    scene_infos.begin(), scene_infos.end(),
                    [&](const scene::SceneInfo &info) {
                        return info.id == switcher_anchor->rehearsal_scene;
                    });
                switcher_view.dissolve_compatible =
                    program_info != scene_infos.end() &&
                    rehearsal_info != scene_infos.end() &&
                    show::dissolve_canvases_compatible(program_info->canvas,
                                                       rehearsal_info->canvas);
            }
            const editor::EditorProjectState project_view{
                project_path, project_message,
                project_explicitly_dirty ||
                    project_content_changed(saved_project_snapshot,
                                            scheduler.projected_document()),
                project_generation};
            std::vector<editor::OutputMonitorState::Node> output_nodes;
            topology::HardwareRuntimeSnapshot hardware_runtime;
            if (lifecycle_controller) {
                cluster_configuration_authored = true;
                try {
                    lifecycle_controller->sync(hardware_map, asset_manifest,
                                               project_generation);
                    output_nodes = lifecycle_controller->snapshot();
                    hardware_runtime =
                        lifecycle_controller->hardware_snapshot();
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not build hardware runtime snapshot\n";
                    return 1;
                }
            }
            configured_cluster_nodes = static_cast<core::u32>(
                std::min<std::size_t>(output_nodes.size(),
                                      std::numeric_limits<core::u32>::max()));
            // Asset adoption is reported, not enforced. Distributing media to
            // render nodes is an external concern in this build, so a node that
            // cannot yet prove it holds every file is a warning on the Output
            // Monitor rather than a hold on the switcher.
            bool assets_adopted = director_asset_availability.ready();
            if (assets_adopted && lifecycle_controller) {
                assets_adopted = lifecycle_controller->program_ready(
                    asset_manifest, hardware_map, project_generation, barrier);
            }
            if (lifecycle_controller) {
                director_sync.status =
                    lifecycle_controller->sync_snapshot(&barrier);
            }
            director_sync.readiness = evaluate_sync_gate(assets_adopted);
            // The one condition that still holds a promotion, and only when
            // the project asked for it. A policy that makes no physical timing
            // claim, or that declares its failures Degraded, never blocks.
            switcher_view.program_held = program_sync_held();
            editor::OutputMonitorState output_monitor{
                timeline.program_frame,
                scene_frame.canvas.width,
                scene_frame.canvas.height,
                static_cast<float>(timeline.rate_numerator) /
                    static_cast<float>(std::max(timeline.rate_denominator, 1U)),
                timeline.program_playing,
                output_nodes};
            const ClusterObservation cluster =
                observe_cluster(timeline.program_frame);
            using UiHealth = editor::OutputMonitorState::Cluster::Health;
            switch (cluster.health) {
            case ClusterControlHealth::Unmonitored:
                output_monitor.cluster.health = UiHealth::Unmonitored;
                break;
            case ClusterControlHealth::Healthy:
                output_monitor.cluster.health = UiHealth::Healthy;
                break;
            case ClusterControlHealth::Degraded:
                output_monitor.cluster.health = UiHealth::Degraded;
                break;
            case ClusterControlHealth::Faulted:
                output_monitor.cluster.health = UiHealth::Faulted;
                break;
            }
            output_monitor.cluster.configured_nodes = cluster.configured_nodes;
            output_monitor.cluster.live_sessions = cluster.live_sessions;
            output_monitor.cluster.catching_up_sessions =
                cluster.catching_up_sessions;
            output_monitor.cluster.release_lag_frames =
                cluster.release_lag_frames;
            output_monitor.cluster.pacer_lag_frames = cluster.pacer_lag_frames;
            output_monitor.cluster.heartbeat_timeouts =
                cluster.sessions.heartbeat_timeouts;
            output_monitor.cluster.session_failures =
                cluster.sessions.session_failures;
            output_monitor.cluster.barrier_rejections =
                cluster.barrier.rejected_registrations +
                cluster.barrier.rejected_acknowledgements;
            output_monitor.cluster.present_failures = cluster.present_failures;
            output_monitor.cluster.diagnostics_recording =
                cluster.diagnostics_recording;
            output_monitor.cluster.external_time_sync_verified =
                cluster.external_time_sync_verified;
            output_monitor.cluster.hardware_frame_lock_verified =
                cluster.hardware_frame_lock_verified;
            output_monitor.cluster.sync_mode =
                sync::sync_mode_name(director_sync.policy.mode);
            // The timing line reports the timing fact. An unrelated hold -
            // assets not adopted, a node still catching up - has its own
            // indicators and must not be dressed up as a raster problem.
            output_monitor.cluster.sync_headline =
                director_sync.readiness.physical_headline();
            output_monitor.cluster.sync_required =
                director_sync.policy.requires_physical_sync();
            output_monitor.cluster.sync_satisfied =
                director_sync.readiness.satisfied;
            output_monitor.cluster.sync_degraded =
                director_sync.readiness.degraded;
            output_monitor.cluster.sync_groups = director_sync.readiness.groups;
            output_monitor.cluster.sync_locked_outputs =
                director_sync.readiness.locked_outputs;
            output_monitor.cluster.sync_expected_outputs =
                director_sync.readiness.expected_outputs;
            output_monitor.cluster.barrier_joined_outputs =
                director_sync.readiness.barrier_joined_outputs;
            output_monitor.cluster.sync_configuration_issues =
                static_cast<core::u32>(
                    director_sync.topology.diagnostics.size());
            apply_node_sync_rows(output_nodes, director_sync.policy,
                                 director_sync.topology, director_sync.status);
            const editor::ProjectSettingsState project_settings_view{
                scene_model.effect_settings(),
                scene_model.effect_settings_node()};
            std::vector<editor::LibraryThumbnailObservation>
                thumbnail_observations;
            if (thumbnail_service) {
                editor->reset_library_thumbnail_textures(
                    project_view.generation);
                const assets::DerivativeProfile *const profile =
                    assets::derivative_profile(
                        assets::DerivativeProfileId::Thumbnail320x180Srgb);
                if (profile != nullptr) {
                    try {
                        thumbnail_observations.reserve(
                            asset_registry.assets.size());
                        for (const assets::Asset &asset :
                             asset_registry.assets) {
                            const auto *const media =
                                std::get_if<assets::MediaAsset>(&asset);
                            if (media == nullptr ||
                                !media->fingerprint.has_value()) {
                                continue;
                            }
                            const assets::DerivativeKey key =
                                assets::make_derivative_key(
                                    media->id, *media->fingerprint, *profile);
                            editor::LibraryThumbnailState state =
                                editor::LibraryThumbnailState::Placeholder;
                            const rhi::Texture *texture =
                                editor->library_thumbnail_texture(key);
                            if (const auto snapshot =
                                    thumbnail_service->snapshot(key);
                                snapshot.has_value()) {
                                switch (snapshot->state) {
                                case assets::DerivativeState::Queued:
                                    state =
                                        editor::LibraryThumbnailState::Queued;
                                    break;
                                case assets::DerivativeState::Generating:
                                    state = editor::LibraryThumbnailState::
                                        Generating;
                                    break;
                                case assets::DerivativeState::Ready:
                                    state =
                                        editor::LibraryThumbnailState::Ready;
                                    if (texture == nullptr &&
                                        std::find(
                                            retained_thumbnail_keys.begin(),
                                            retained_thumbnail_keys.end(),
                                            key) !=
                                            retained_thumbnail_keys.end()) {
                                        auto loaded =
                                            thumbnail_service->load(key);
                                        if (loaded) {
                                            assets::DerivativeCacheLoad data =
                                                std::move(loaded).value();
                                            if (editor
                                                    ->upload_library_thumbnail(
                                                        key, data.payload)) {
                                                texture =
                                                    editor
                                                        ->library_thumbnail_texture(
                                                            key);
                                            } else {
                                                state = editor::
                                                    LibraryThumbnailState::
                                                        Failed;
                                            }
                                        } else {
                                            state = editor::
                                                LibraryThumbnailState::Failed;
                                        }
                                    }
                                    break;
                                case assets::DerivativeState::Stale:
                                    state =
                                        editor::LibraryThumbnailState::Stale;
                                    break;
                                case assets::DerivativeState::Failed:
                                    state =
                                        editor::LibraryThumbnailState::Failed;
                                    break;
                                case assets::DerivativeState::Missing:
                                case assets::DerivativeState::Cancelled:
                                    break;
                                }
                            }
                            thumbnail_observations.push_back(
                                editor::LibraryThumbnailObservation{key, state,
                                                                    texture});
                        }
                    } catch (const std::bad_alloc &) {
                        std::cerr << "could not project Library thumbnail "
                                     "observations\n";
                        return 1;
                    }
                }
            }
            std::vector<editor::TimelineFilmstripObservation>
                filmstrip_observations;
            if (thumbnail_service) {
                editor->reset_timeline_filmstrip_textures(
                    project_view.generation);
                const assets::DerivativeProfile *const profile =
                    assets::derivative_profile(
                        assets::DerivativeProfileId::Filmstrip8x160x90Srgb);
                if (profile != nullptr) {
                    try {
                        filmstrip_observations.reserve(stage_clips.size());
                        for (const scene::ClipPlacement &clip : stage_clips) {
                            const doc::Node *const clip_node =
                                scheduler.projected_document().get(clip.node);
                            if (clip_node == nullptr) {
                                continue;
                            }
                            const auto asset_property =
                                clip_node->properties.find(
                                    scene::schema::asset_id);
                            const auto *const asset_reference =
                                asset_property != clip_node->properties.end()
                                    ? std::get_if<std::int64_t>(
                                          &asset_property->second)
                                    : nullptr;
                            if (asset_reference == nullptr ||
                                *asset_reference <= 0) {
                                continue;
                            }
                            const doc::NodeId asset_id{
                                static_cast<core::u64>(*asset_reference)};
                            const assets::Asset *const asset =
                                asset_registry.asset(asset_id);
                            const auto *const media =
                                asset != nullptr
                                    ? std::get_if<assets::MediaAsset>(asset)
                                    : nullptr;
                            if (media == nullptr ||
                                !media->fingerprint.has_value()) {
                                continue;
                            }
                            const assets::DerivativeKey key =
                                assets::make_derivative_key(
                                    media->id, *media->fingerprint, *profile);
                            assets::DerivativeState state =
                                assets::DerivativeState::Missing;
                            const rhi::Texture *texture =
                                editor->timeline_filmstrip_texture(key);
                            if (const auto snapshot =
                                    thumbnail_service->snapshot(key);
                                snapshot.has_value()) {
                                state = snapshot->state;
                                if (state == assets::DerivativeState::Ready &&
                                    texture == nullptr &&
                                    std::find(retained_filmstrip_keys.begin(),
                                              retained_filmstrip_keys.end(),
                                              key) !=
                                        retained_filmstrip_keys.end()) {
                                    auto loaded = thumbnail_service->load(key);
                                    if (loaded &&
                                        editor->upload_timeline_filmstrip(
                                            key, loaded.value().payload)) {
                                        texture =
                                            editor->timeline_filmstrip_texture(
                                                key);
                                    } else {
                                        state = assets::DerivativeState::Failed;
                                    }
                                }
                            }
                            filmstrip_observations.push_back(
                                editor::TimelineFilmstripObservation{
                                    clip.node, key, state, texture});
                        }
                    } catch (const std::bad_alloc &) {
                        std::cerr << "could not project Timeline filmstrip "
                                     "observations\n";
                        return 1;
                    }
                }
            }
            if (now >= next_output_media_projection) {
                try {
                    project_output_node_media_runtime(
                        asset_registry, asset_manifest.digest, hardware_map,
                        barrier, library_proxy_runtime);
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not project output-node media runtime "
                                 "observations\n";
                    return 1;
                }
                next_output_media_projection =
                    now + library_proxy_retry_interval;
            }
            auto editor_frame = editor->frame(
                scene_frame, timeline, keyframes, elements, effects,
                hardware_map, feed_map, switcher_view, asset_registry,
                thumbnail_observations, compositor_graph, project_view,
                output_monitor, hardware_runtime, director_sync.policy,
                preferences_view, project_settings_view, library_proxy_runtime,
                stage_lanes, clip_clipboard.span_duration(),
                selection_set_summaries, key_scale_preview,
                filmstrip_observations);
            if (editor_frame.save_workspace_layout.has_value() &&
                workspace_layout_file.has_value()) {
                const auto saved_layout = persist::save_workspace_layout(
                    *workspace_layout_file,
                    *editor_frame.save_workspace_layout);
                if (!saved_layout) {
                    std::cerr
                        << "could not save workspace layout (code "
                        << static_cast<unsigned>(saved_layout.error().code)
                        << ')';
                    if (saved_layout.error().system_error) {
                        std::cerr
                            << ": "
                            << saved_layout.error().system_error.message();
                    }
                    std::cerr << '\n';
                }
            }
            if (editor_frame.close_semantic_coalescing) {
                editor_coalescing_policy.close();
                scheduler.end_coalescing();
                pending_editor_coalescing_close = true;
            }
            if (editor_frame.semantic_coalescing.has_value()) {
                const editor::SemanticCoalescingRequest &request =
                    *editor_frame.semantic_coalescing;
                const EditCoalescingIdentity identity{
                    cmd::UndoContextId{
                        request.context == editor::HistoryContext::Compositor
                            ? 6U
                            : 2U},
                    cmd::CoalescingKey{request.key.focus, request.key.selection,
                                       request.key.property,
                                       request.key.direction,
                                       request.key.modifiers}};
                try {
                    EditCoalescingPlan plan = editor_coalescing_policy.observe(
                        identity, cmd::TransactionId{request.gesture},
                        Clock::now());
                    if (plan.action == EditCoalescingAction::Begin) {
                        // Close the previous semantic stream even when every
                        // payload in this new repeat later proves to be a
                        // no-op.
                        scheduler.end_coalescing();
                    }
                    pending_editor_coalescing.insert_or_assign(request.gesture,
                                                               std::move(plan));
                    // A same-frame semantic repeat follows (rather than
                    // precedes) any focus/pointer boundary reported with it.
                    pending_editor_coalescing_close = false;
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue numeric edit coalescing\n";
                    return 1;
                }
            }
            if (thumbnail_service) {
                for (const editor::LibraryThumbnailRequestIntent &request :
                     editor_frame.thumbnail_requests) {
                    const assets::Asset *const asset =
                        asset_registry.asset(request.key.asset);
                    const auto *const media =
                        asset != nullptr
                            ? std::get_if<assets::MediaAsset>(asset)
                            : nullptr;
                    if (media == nullptr || !media->fingerprint.has_value()) {
                        continue;
                    }
                    const assets::DerivativeProfile *const profile =
                        assets::derivative_profile(
                            assets::DerivativeProfileId::Thumbnail320x180Srgb);
                    if (profile == nullptr ||
                        assets::make_derivative_key(media->id,
                                                    *media->fingerprint,
                                                    *profile) != request.key) {
                        continue;
                    }
                    const server::ThumbnailRequestPriority priority =
                        request.priority ==
                                editor::LibraryThumbnailRequestPriority::Visible
                            ? server::ThumbnailRequestPriority::Visible
                            : server::ThumbnailRequestPriority::Prefetch;
                    const auto scheduled =
                        thumbnail_service->request(*media, priority);
                    if (!scheduled) {
                        std::cerr
                            << "could not request Library thumbnail for "
                            << media->id.value() << " (code "
                            << static_cast<unsigned>(scheduled.error().code)
                            << ")\n";
                    }
                }
                for (const editor::TimelineFilmstripRequestIntent &request :
                     editor_frame.filmstrip_requests) {
                    const assets::Asset *const asset =
                        asset_registry.asset(request.key.asset);
                    const auto *const media =
                        asset != nullptr
                            ? std::get_if<assets::MediaAsset>(asset)
                            : nullptr;
                    if (media == nullptr || !media->fingerprint.has_value()) {
                        continue;
                    }
                    const assets::DerivativeProfile *const profile =
                        assets::derivative_profile(
                            assets::DerivativeProfileId::Filmstrip8x160x90Srgb);
                    if (profile == nullptr ||
                        assets::make_derivative_key(media->id,
                                                    *media->fingerprint,
                                                    *profile) != request.key) {
                        continue;
                    }
                    const server::ThumbnailRequestPriority priority =
                        request.priority ==
                                editor::TimelineFilmstripRequestPriority::
                                    Visible
                            ? server::ThumbnailRequestPriority::Visible
                            : server::ThumbnailRequestPriority::Prefetch;
                    const auto scheduled =
                        thumbnail_service->request_filmstrip(*media, priority);
                    if (!scheduled) {
                        std::cerr
                            << "could not request Timeline filmstrip for "
                            << media->id.value() << " (code "
                            << static_cast<unsigned>(scheduled.error().code)
                            << ")\n";
                    }
                }
                if (editor_frame.thumbnail_retain.has_value()) {
                    try {
                        retained_thumbnail_keys =
                            editor_frame.thumbnail_retain->keys;
                    } catch (const std::bad_alloc &) {
                        std::cerr
                            << "could not retain Library thumbnail keys\n";
                        return 1;
                    }
                    editor->retain_library_thumbnail_textures(
                        retained_thumbnail_keys);
                }
                if (editor_frame.filmstrip_retain.has_value()) {
                    try {
                        retained_filmstrip_keys =
                            editor_frame.filmstrip_retain->keys;
                    } catch (const std::bad_alloc &) {
                        std::cerr
                            << "could not retain Timeline filmstrip keys\n";
                        return 1;
                    }
                    editor->retain_timeline_filmstrip_textures(
                        retained_filmstrip_keys);
                }
                try {
                    std::vector<assets::DerivativeKey> retained_derivatives =
                        retained_thumbnail_keys;
                    for (const assets::DerivativeKey &key :
                         retained_filmstrip_keys) {
                        if (std::find(retained_derivatives.begin(),
                                      retained_derivatives.end(),
                                      key) == retained_derivatives.end()) {
                            retained_derivatives.push_back(key);
                        }
                    }
                    thumbnail_service->retain(retained_derivatives);
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not retain thumbnail derivative keys\n";
                    return 1;
                }
            }
            if (editor_frame.save_preferences.has_value()) {
                const editor::EditorPreferences &wanted =
                    *editor_frame.save_preferences;
                persist::Preferences next{};
                next.director_listen = wanted.director_listen;
                next.show_id = wanted.show_id;
                next.director_epoch = wanted.director_epoch;
                next.lifecycle_secret_file = wanted.lifecycle_secret_file;
                next.wal_path = wanted.wal_path;
                next.proxy_cache_directory = wanted.proxy_cache_directory;
                next.proxy_cache_next_to_source =
                    wanted.proxy_cache_next_to_source;
                next.proxy_cache_gibibytes = wanted.proxy_cache_gibibytes;
                next.output_buffer_frames = wanted.output_buffer_frames;
                next.output_read_ahead_frames = wanted.output_read_ahead_frames;
                next.playback_cache_mebibytes = wanted.playback_cache_mebibytes;
                next.playback_trail_frames = wanted.playback_trail_frames;
                switch (wanted.proxy_transcode_priority) {
                case editor::EditorPreferences::TranscodePriority::Background:
                    next.proxy_transcode_priority =
                        persist::ProxyTranscodePriority::Background;
                    break;
                case editor::EditorPreferences::TranscodePriority::Balanced:
                    next.proxy_transcode_priority =
                        persist::ProxyTranscodePriority::Balanced;
                    break;
                case editor::EditorPreferences::TranscodePriority::Fast:
                    next.proxy_transcode_priority =
                        persist::ProxyTranscodePriority::Fast;
                    break;
                }
                if (!valid_proxy_cache_preference(
                        next.proxy_cache_directory,
                        next.proxy_cache_next_to_source)) {
                    preferences_view.message =
                        "PROXY CACHE FOLDER MUST BE AN ABSOLUTE FOLDER";
                } else if (!next.valid_tuning()) {
                    preferences_view.message =
                        "PLAYBACK SETTINGS ARE OUTSIDE SUPPORTED LIMITS";
                } else if (options.preferences_file.empty()) {
                    preferences_view.message =
                        "NO PER-USER SETTINGS LOCATION ON THIS MACHINE";
                } else if (auto saved = persist::save_preferences(
                               options.preferences_file, next)) {
                    // The panel keeps editing the stored record, so adopt it
                    // immediately; the running process keeps its own values
                    // until it is restarted, which the panel already says.
                    preferences_view.stored = wanted;
                    preferences_view.message = "SAVED - APPLIES AT NEXT LAUNCH";
                } else {
                    preferences_view.message = "COULD NOT SAVE PREFERENCES";
                    std::cerr << "could not save preferences (code "
                              << static_cast<unsigned>(saved.error().code)
                              << ')';
                    if (saved.error().system_error) {
                        std::cerr << ": "
                                  << saved.error().system_error.message();
                    }
                    std::cerr << '\n';
                }
            }
            if (editor_frame.clear_proxy_cache) {
                // Stop the only writer and join any materialization read before
                // removing service-owned cache children. The long-lived
                // playback handle reconciles its index on the next inspect.
                director_proxy_generator.reset();
                director_proxy_playback.reset();
                const ProxyCacheClearResult cleared =
                    clear_proxy_cache(options, asset_registry);
                if (cleared) {
                    preferences_view.message = "PROXY CACHE CLEARED";
                    if (cleared.roots_cleared > 1U) {
                        preferences_view.message += " - ";
                        preferences_view.message +=
                            std::to_string(cleared.roots_cleared);
                        preferences_view.message += " LOCATIONS";
                    }
                } else {
                    preferences_view.message = "COULD NOT CLEAR PROXY CACHE";
                    std::cerr << "could not clear proxy cache: "
                              << cleared.error.message() << '\n';
                }

                auto generation_artifacts =
                    open_proxy_playback_artifacts(options, "proxy generator");
                if (generation_artifacts != nullptr) {
                    director_proxy_generator =
                        std::make_unique<ProxyGenerationWorker>(
                            std::move(generation_artifacts));
                    for (const assets::Asset &entry :
                         director_playback_asset_registry.assets) {
                        const auto *const media =
                            std::get_if<assets::MediaAsset>(&entry);
                        if (media != nullptr &&
                            media->requested_proxy_profile ==
                                assets::DerivativeProfileId::Proxy720pCfrH264) {
                            director_proxy_generator->request(*media);
                        }
                    }
                }
            }
            if (editor_frame.output_lifecycle.has_value() &&
                lifecycle_controller) {
                lifecycle_controller->submit(*editor_frame.output_lifecycle);
            }
            if (editor_frame.status == editor::EditorStatus::Closed) {
                closed = true;
            }
            // Coalesce to the latest edit per node before the next boundary
            // authors them, so a drag is one command per frame, not per event.
            for (const auto &edit : editor_frame.edits) {
                bool merged = false;
                for (auto &queued : pending_edits) {
                    if (queued.node == edit.node) {
                        queued = edit;
                        merged = true;
                        break;
                    }
                }
                if (!merged) {
                    try {
                        pending_edits.push_back(edit);
                    } catch (const std::bad_alloc &) {
                        std::cerr << "could not queue a scene edit\n";
                        return 1;
                    }
                }
            }
            if (editor_frame.history.has_value()) {
                pending_history = editor_frame.history;
            }
            if (editor_frame.clip_clipboard.has_value()) {
                const editor::ClipClipboardRequest &request =
                    *editor_frame.clip_clipboard;
                if (request.action == editor::ClipClipboardAction::Copy) {
                    try {
                        static_cast<void>(clip_clipboard.copy(
                            scheduler.projected_document(), request.selection));
                    } catch (const std::bad_alloc &) {
                        std::cerr << "could not snapshot the clip clipboard\n";
                        return 1;
                    }
                } else {
                    try {
                        pending_clip_paste =
                            PendingClipPaste{request, clip_clipboard};
                    } catch (const std::bad_alloc &) {
                        std::cerr << "could not queue a clip paste\n";
                        return 1;
                    }
                }
            }
            if (editor_frame.key_clipboard.has_value()) {
                const editor::KeyClipboardRequest &request =
                    *editor_frame.key_clipboard;
                try {
                    if (request.action ==
                        editor::TimelineKeyClipboardAction::Copy) {
                        const std::vector<KeyAddress> selection =
                            key_addresses(request.selection);
                        const KeyCopyReport report =
                            key_clipboard.copy(scheduler.projected_document(),
                                               request.scene, selection);
                        project_message = key_operation_message(
                            "COPY", report.captured, "CAPTURED",
                            report.locked_targets, report.incompatible);
                    } else {
                        pending_key_paste =
                            PendingKeyPaste{request, key_clipboard};
                    }
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not snapshot the key clipboard\n";
                    return 1;
                }
            }
            if (editor_frame.key_duplicate.has_value()) {
                try {
                    pending_key_duplicate = editor_frame.key_duplicate;
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue a key duplicate\n";
                    return 1;
                }
            }
            if (editor_frame.key_scale_preview.has_value()) {
                const editor::KeyScalePreviewRequest &request =
                    *editor_frame.key_scale_preview;
                const std::vector<scene::KeyAddress> selection =
                    scene_key_addresses(request.preview.keys);
                auto planned =
                    scene::plan_key_scale(scheduler.projected_document(),
                                          selection, request.preview.scale);
                editor::TimelineKeyScalePreview preview{};
                preview.signature = request.preview.signature;
                if (planned) {
                    preview.valid = true;
                    preview.selected = planned.value().report.selected;
                    preview.applied = planned.value().report.applied;
                    preview.skips = planned.value().report.skips.size();
                    preview.collisions =
                        planned.value().report.collisions.size();
                } else if (planned.error() ==
                           scene::KeyEditError::AllocationFailed) {
                    std::cerr << "could not allocate a key scale preview\n";
                    return 1;
                }
                key_scale_preview = preview;
            }
            if (editor_frame.key_batch.has_value()) {
                try {
                    pending_key_batch = std::move(*editor_frame.key_batch);
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue an atomic key edit\n";
                    return 1;
                }
            }
            if (editor_frame.seek_to_frame) {
                (rehearsal_mode ? pending_preview_seek : pending_seek) =
                    editor_frame.seek_to_frame;
            }
            if (editor_frame.range) {
                pending_range = editor_frame.range;
            }
            if (editor_frame.show_rate) {
                pending_show_rate = editor_frame.show_rate;
            }
            for (const auto &key : editor_frame.keys) {
                try {
                    pending_keys.push_back(key);
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue a keyframe\n";
                    return 1;
                }
            }
            for (const auto &moved : editor_frame.key_moves) {
                try {
                    pending_key_moves.push_back(moved);
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue a keyframe move\n";
                    return 1;
                }
            }
            if (editor_frame.project_settings.has_value()) {
                pending_project_settings = editor_frame.project_settings;
            }
            for (const auto &edit : editor_frame.clip_timings) {
                try {
                    pending_clip_timings.push_back(edit);
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue a clip placement\n";
                    return 1;
                }
            }
            for (const auto &split : editor_frame.clip_splits) {
                try {
                    pending_clip_splits.push_back(split);
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue a clip split\n";
                    return 1;
                }
            }
            pending_asset_rescan =
                pending_asset_rescan || editor_frame.rescan_assets;
            for (const auto &edit : editor_frame.element_flags) {
                try {
                    pending_element_flags.push_back(edit);
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue an element flag edit\n";
                    return 1;
                }
            }
            for (const auto &edit : editor_frame.lane_edits) {
                try {
                    pending_lane_edits.push_back(edit);
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue a timeline lane edit\n";
                    return 1;
                }
            }
            for (const auto &edit : editor_frame.layer_orders) {
                try {
                    pending_layer_orders.push_back(edit);
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue a layer reorder\n";
                    return 1;
                }
            }
            for (const auto &edit : editor_frame.timeline_edits) {
                try {
                    pending_timeline_edits.push_back(edit);
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue an advanced timeline edit\n";
                    return 1;
                }
            }
            if (editor_frame.group_edit.has_value()) {
                try {
                    pending_group_edit = editor_frame.group_edit;
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue a Scene group edit\n";
                    return 1;
                }
            }
            if (editor_frame.selection_set_edit.has_value()) {
                editor::SelectionSetEditIntent request =
                    std::move(*editor_frame.selection_set_edit);
                if (request.action == editor::SelectionSetEditAction::Recall) {
                    auto recalled = scene::recall_selection_set(
                        scheduler.projected_document(), request.scene,
                        request.set);
                    if (!recalled) {
                        project_message = selection_set_error_message(
                            request.action, recalled.error());
                    } else if (!editor->apply_selection_set_recall(
                                   request.scene, recalled.value().targets,
                                   recalled.value().primary)) {
                        project_message =
                            "SELECTION SET RECALL REJECTED - SCENE CHANGED "
                            "OR LOCAL MEMORY IS FULL";
                    } else {
                        project_message =
                            "SELECTION SET RECALLED - " +
                            std::to_string(recalled.value().targets.size()) +
                            " SELECTED";
                        if (!recalled.value().skips.empty()) {
                            project_message +=
                                " / " +
                                std::to_string(recalled.value().skips.size()) +
                                " STALE OR INCOMPATIBLE SKIPPED";
                        }
                    }
                } else {
                    try {
                        pending_selection_set_edit = std::move(request);
                    } catch (const std::bad_alloc &) {
                        std::cerr << "could not queue a Selection Set edit\n";
                        return 1;
                    }
                }
            }
            if (editor_frame.mask_edit.has_value()) {
                try {
                    pending_mask_edit = std::move(*editor_frame.mask_edit);
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue a mask edit\n";
                    return 1;
                }
            }
            for (const auto &edit : editor_frame.property_edits) {
                try {
                    pending_properties.push_back(edit);
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue an inspector edit\n";
                    return 1;
                }
            }
            for (const auto &edit : editor_frame.generic_edits) {
                try {
                    pending_generics.push_back(edit);
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue an inspector edit\n";
                    return 1;
                }
            }
            for (const auto &edit : editor_frame.lock_edits) {
                try {
                    pending_locks.push_back(edit);
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue an element lock\n";
                    return 1;
                }
            }
            for (const auto &add : editor_frame.adds) {
                try {
                    pending_adds.push_back(add);
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue an element add\n";
                    return 1;
                }
            }
            for (const auto &imported : editor_frame.asset_imports) {
                try {
                    pending_asset_imports.push_back(imported);
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue an asset import\n";
                    return 1;
                }
            }
            for (const auto &request : editor_frame.asset_deletes) {
                try {
                    pending_asset_deletes.push_back(request);
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue an asset removal\n";
                    return 1;
                }
            }
            for (const auto &request : editor_frame.asset_proxy_edits) {
                try {
                    pending_asset_proxy_edits.push_back(request);
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue an asset proxy policy\n";
                    return 1;
                }
            }
            for (const auto &effect : editor_frame.effects) {
                try {
                    pending_effects.push_back(effect);
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue an effect\n";
                    return 1;
                }
            }
            for (const auto &edit : editor_frame.film_grain_effect_edits) {
                try {
                    pending_film_grain_effect_edits.push_back(edit);
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue Film Grain parameters\n";
                    return 1;
                }
            }
            for (const auto &effect : editor_frame.effect_deletes) {
                try {
                    pending_effect_deletes.push_back(effect);
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue an effect removal\n";
                    return 1;
                }
            }
            if (editor_frame.switcher.has_value()) {
                pending_switcher = editor_frame.switcher;
            }
            try {
                pending_hardware_places.insert(
                    pending_hardware_places.end(),
                    editor_frame.hardware_places.begin(),
                    editor_frame.hardware_places.end());
                pending_hardware_moves.insert(
                    pending_hardware_moves.end(),
                    editor_frame.hardware_moves.begin(),
                    editor_frame.hardware_moves.end());
                pending_hardware_links.insert(
                    pending_hardware_links.end(),
                    editor_frame.hardware_links.begin(),
                    editor_frame.hardware_links.end());
                pending_hardware_deletes.insert(
                    pending_hardware_deletes.end(),
                    editor_frame.hardware_deletes.begin(),
                    editor_frame.hardware_deletes.end());
                pending_hardware_properties.insert(
                    pending_hardware_properties.end(),
                    editor_frame.hardware_property_edits.begin(),
                    editor_frame.hardware_property_edits.end());
                pending_hardware_output_binding_edits.insert(
                    pending_hardware_output_binding_edits.end(),
                    editor_frame.hardware_output_binding_edits.begin(),
                    editor_frame.hardware_output_binding_edits.end());
                pending_hardware_output_binding_deletes.insert(
                    pending_hardware_output_binding_deletes.end(),
                    editor_frame.hardware_output_binding_deletes.begin(),
                    editor_frame.hardware_output_binding_deletes.end());
                pending_feed_map_adds.insert(pending_feed_map_adds.end(),
                                             editor_frame.feed_map_adds.begin(),
                                             editor_frame.feed_map_adds.end());
                pending_feed_map_edits.insert(
                    pending_feed_map_edits.end(),
                    editor_frame.feed_map_edits.begin(),
                    editor_frame.feed_map_edits.end());
                pending_feed_map_deletes.insert(
                    pending_feed_map_deletes.end(),
                    editor_frame.feed_map_deletes.begin(),
                    editor_frame.feed_map_deletes.end());
                pending_feed_projection_edits.insert(
                    pending_feed_projection_edits.end(),
                    editor_frame.feed_projection_edits.begin(),
                    editor_frame.feed_projection_edits.end());
                pending_feed_surface_adds.insert(
                    pending_feed_surface_adds.end(),
                    editor_frame.feed_surface_adds.begin(),
                    editor_frame.feed_surface_adds.end());
                pending_feed_surface_edits.insert(
                    pending_feed_surface_edits.end(),
                    editor_frame.feed_surface_edits.begin(),
                    editor_frame.feed_surface_edits.end());
                pending_feed_surface_deletes.insert(
                    pending_feed_surface_deletes.end(),
                    editor_frame.feed_surface_deletes.begin(),
                    editor_frame.feed_surface_deletes.end());
            } catch (const std::bad_alloc &) {
                std::cerr << "could not queue a mapping edit\n";
                return 1;
            }
            if (editor_frame.hardware_control_port_edit.has_value()) {
                pending_hardware_control_port =
                    editor_frame.hardware_control_port_edit;
            }
            if (editor_frame.sync_policy_edit.has_value()) {
                pending_sync_policy = editor_frame.sync_policy_edit;
            }
            if (editor_frame.compositor_add.has_value()) {
                pending_compositor_add = editor_frame.compositor_add;
                pending_compositor_gesture = editor_frame.compositor_gesture;
            }
            if (editor_frame.compositor_move.has_value()) {
                pending_compositor_move = editor_frame.compositor_move;
                pending_compositor_gesture = editor_frame.compositor_gesture;
            }
            if (editor_frame.compositor_connect.has_value()) {
                pending_compositor_connect = editor_frame.compositor_connect;
                pending_compositor_gesture = editor_frame.compositor_gesture;
            }
            if (editor_frame.compositor_property_edit.has_value()) {
                pending_compositor_property =
                    editor_frame.compositor_property_edit;
                pending_compositor_gesture = editor_frame.compositor_gesture;
            }
            if (editor_frame.compositor_delete.has_value()) {
                pending_compositor_delete = editor_frame.compositor_delete;
                pending_compositor_gesture = editor_frame.compositor_gesture;
            }
            if (!editor_frame.deletes.empty()) {
                try {
                    pending_deletes.insert(pending_deletes.end(),
                                           editor_frame.deletes.begin(),
                                           editor_frame.deletes.end());
                    pending_delete_gesture = editor_frame.delete_gesture;
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue an element delete\n";
                    return 1;
                }
            }
            if (editor_frame.transport != editor::TransportRequest::None) {
                try {
                    // In Preview the timeline drives Preview's own playhead.
                    // The show keeps running underneath, untouched.
                    (rehearsal_mode ? preview_commands : operator_commands)
                        .push_back(map_transport(editor_frame.transport));
                } catch (const std::bad_alloc &) {
                    std::cerr << "could not queue a transport command\n";
                    return 1;
                }
            }
            if (editor_frame.load_project.has_value()) {
                pending_project_save.reset();
                // An explicit Open supersedes the startup recent-project
                // candidate, including its already-decoded Root. Never pair a
                // newly chosen path with stale preloaded project contents.
                pending_preloaded_project.reset();
                pending_project_load = std::move(editor_frame.load_project);
                project_message = "LOADING PROJECT...";
            } else if (editor_frame.save_project.has_value()) {
                // Explicit Save means "save this current project", not "load
                // the remembered project at the next boundary first".
                pending_project_load.reset();
                pending_preloaded_project.reset();
                pending_project_save = std::move(editor_frame.save_project);
                project_message = "SAVING PROJECT...";
            }
        }
#endif
        if (!handled) {
            if (render_scene && switcher_progress.active) {
                status = presenter->present(
                    transition_from_frame, transition_to_frame,
                    static_cast<float>(switcher_progress.mix()),
                    transition_from_graph, transition_to_graph,
                    scene::whole(scene_frame.canvas));
            } else {
                status = render_scene
                             ? presenter->present(
                                   scene_frame, program_compositor_graph,
                                   scene::whole(scene_frame.canvas))
                             : presenter->present(input);
            }
        }

        // Evaluated for its digest only, so the Director's status line can show
        // it agrees with the nodes' feed maps.
        if (options.output) {
            const auto &document = scheduler.sealed_document();
            if (!feed_loaded || document.version().value() != feed_version) {
                feed_model.reset(document);
                feed_version = document.version().value();
                feed_loaded = true;
            }
            feed_map = feed_model.evaluate(
                scene::SceneTime{input.show_frame.value(), input.rate.numerator,
                                 input.rate.denominator});
        }

        switch (status) {
        case show::PresenterStatus::Closed:
            closed = true;
            break;
        case show::PresenterStatus::Failed:
            ++present_failures;
            break;
        case show::PresenterStatus::Deferred:
        case show::PresenterStatus::Presented:
        case show::PresenterStatus::Skipped:
            break;
        }

        if (presenter != nullptr) {
            for (auto command = presenter->take_command();
                 command != show::PresenterCommand::None;
                 command = presenter->take_command()) {
                try {
                    operator_commands.push_back(command);
                } catch (const std::bad_alloc &) {
                    std::cerr
                        << "could not queue an operator transport command\n";
                    return 1;
                }
            }
        }

        since_status += sealed_this_iteration;
        if (sealed_this_iteration != 0U && since_status >= interval) {
            since_status = 0U;
            ClusterObservation cluster =
                observe_cluster(input.show_frame.value());
            cluster.diagnostics_recording = cluster_diagnostics.available();
            static_cast<void>(cluster_diagnostics.append(cluster));
            std::cout << "director show-frame=" << input.show_frame.value()
                      << " control-frame=" << control_frame.value()
                      << " transport=" << transport_label(transport.state())
                      << " sealed=" << sealed << " anchors=" << anchors
                      << " pacer-lag=" << cluster.pacer_lag_frames
                      << " tracked=" << tracked.size()
                      << " sessions=" << hub.active_subscriptions()
                      << " live=" << cluster.live_sessions
                      << " catching-up=" << cluster.catching_up_sessions
                      << " release-lag=" << cluster.release_lag_frames
                      << " heartbeat-timeouts="
                      << cluster.sessions.heartbeat_timeouts
                      << " released-through="
                      << barrier.released_through().through_frame.value();
            // The same scene digest the nodes report. Printed here so an
            // operator can confirm the Director and every node agree on the
            // replicated scene at the same show frame.
            if (needs_scene) {
                std::cout << " digest=" << authored_scene_digest;
            }
            // And the same feed-map digest, so the operator can confirm the map
            // itself is replicated, not just the scene.
            if (options.output) {
                std::cout << " feed=" << output::digest(feed_map);
            }
            if (presenter != nullptr) {
                std::cout << gpu_timing_text(presenter->timing());
            }
            std::cout << '\n' << std::flush;
        }

        if (options.run_frames != 0U && sealed >= options.run_frames) {
            break;
        }
        sleep_until_boundary(pacer.next_boundary_at());
    }

    ClusterObservation final_cluster =
        observe_cluster(transport.frame_at(scheduler.sealed_through()).value());
    final_cluster.diagnostics_recording = cluster_diagnostics.available();
    static_cast<void>(cluster_diagnostics.append(final_cluster));
    std::cout << "director stopping"
              << " show-frame="
              << transport.frame_at(scheduler.sealed_through()).value()
              << " control-frame=" << scheduler.sealed_through().value()
              << " transport=" << transport_label(transport.state())
              << " sealed=" << sealed << " anchors=" << anchors
              << " pacer-lag=" << final_cluster.pacer_lag_frames
              << " tracked=" << tracked.size()
              << " present-failures=" << present_failures << " surface="
              << (options.edit
                      ? "editor"
                      : (presenter && presenter->windowed() ? "window"
                                                            : "headless"))
              << " released-through="
              << barrier.released_through().through_frame.value() << '\n'
              << std::flush;
    return 0;
}

} // namespace mediaserver::app
