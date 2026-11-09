#include "playback_media.hpp"

#include "cluster_observability.hpp"
#include "output_media_runtime.hpp"

#include <mediaserver/assets/asset_path.hpp>
#include <mediaserver/assets/derivative_cache.hpp>
#include <mediaserver/assets/derivative_profile_id.hpp>

#include <mediaserver/persist/settings.hpp>

#include <iterator>
#include <span>

namespace mediaserver::app {

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
        // Stills have nothing to transcode; a video larger than the 720p
        // proxy gets one as soon as it is in the project.
        const bool still =
            media->frame_count.has_value() && *media->frame_count <= 1U;
        if ((source_policy || full_resolution_output_proxy) && !still &&
            (media->dimensions->width > 1'280U ||
             media->dimensions->height > 720U)) {
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
    for (const topology::HardwareDevice *const device :
         topology::cluster_render_servers(hardware)) {
        node_indices.push_back(device->node_index);
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

} // namespace mediaserver::app
