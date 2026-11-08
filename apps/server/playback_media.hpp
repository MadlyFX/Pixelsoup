#pragma once

// Proxy cache, proxy generation and media-runtime observation shared by the
// Director and node playback loops.

#include "ffmpeg_proxy_encoder.hpp"
#include "playback.hpp"
#include "project_assets.hpp"
#include "proxy_generation_service.hpp"
#include "proxy_generation_worker.hpp"
#include "proxy_playback.hpp"

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/assets/derivative.hpp>
#include <mediaserver/assets/proxy_policy.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/net/director_frame_barrier.hpp>
#include <mediaserver/net/session_protocol.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/topology/hardware_map.hpp>

#include <mediaserver/editor/workspace_panels.hpp>
#include <mediaserver/render/media_source.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <span>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace mediaserver::app {

[[nodiscard]] core::u64
proxy_cache_bytes(const PlaybackOptions &options) noexcept;

[[nodiscard]] server::ProxyTranscodePriority
proxy_transcode_priority(const PlaybackOptions &options) noexcept;

[[nodiscard]] std::filesystem::path
derivative_cache_root(const PlaybackOptions &options, std::error_code &error);

[[nodiscard]] std::filesystem::path
fixed_proxy_cache_root(const PlaybackOptions &options, std::error_code &error);

[[nodiscard]] bool valid_proxy_cache_preference(std::string_view directory,
                                                bool next_to_source) noexcept;

[[nodiscard]] std::unique_ptr<server::ProxyGenerationService>
open_proxy_playback_artifacts(const PlaybackOptions &options,
                              std::string_view owner);

struct ProxyCacheClearResult final {
    std::size_t roots_cleared{};
    std::error_code error;

    [[nodiscard]] explicit operator bool() const noexcept { return !error; }
};

[[nodiscard]] ProxyCacheClearResult
clear_proxy_cache(const PlaybackOptions &options,
                  const assets::AssetRegistry &registry) noexcept;

[[nodiscard]] assets::AssetRegistry
editor_playback_registry(const assets::AssetRegistry &authored);

[[nodiscard]] const char *
proxy_selection_name(assets::ProxyPlaybackSelection selection) noexcept;

[[nodiscard]] bool
same_proxy_status(const ProxyPlacementObservation &left,
                  const ProxyPlacementObservation &right) noexcept;

void report_proxy_status_changes(
    std::string_view owner,
    const std::vector<ProxyPlacementObservation> &current,
    const std::vector<ProxyPlacementObservation> &previous);

[[nodiscard]] bool
resolve_scene_proxies(ProxyPlaybackService &playback, scene::Scene &frame,
                      const assets::AssetRegistry &registry,
                      const ProjectAssetAvailability &availability,
                      std::vector<ProxyPlacementObservation> &observations);

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
runtime_proxy_state(ProxyAssetRuntimeState state) noexcept;

[[nodiscard]] net::RuntimeProxyReason
runtime_proxy_reason(assets::ProxyPlaybackReason reason) noexcept;

[[nodiscard]] net::RuntimeProxyIssue
runtime_proxy_issue(ProxyPlacementIssue issue) noexcept;

[[nodiscard]] bool build_node_media_status(
    const assets::AssetRegistry &registry,
    const ProjectAssetAvailability &availability,
    server::ProxyGenerationService *artifacts,
    const ProxyGenerationWorker *generator,
    const MediaDecodeSupportWorker &decode_support, core::u64 manifest_digest,
    std::array<net::RuntimeMediaAssetStatus, net::max_runtime_media_assets>
        &output,
    std::size_t &output_count) noexcept;

[[nodiscard]] editor::LibraryProxyRuntimeState
editor_proxy_runtime_state(net::RuntimeProxyState state) noexcept;

[[nodiscard]] editor::LibraryProxyRuntimeIssue
editor_proxy_runtime_issue(net::RuntimeProxyIssue issue) noexcept;

[[nodiscard]] const net::RuntimeMediaAssetStatus *
runtime_media_status(const net::RuntimeAssetAdoption &adoption,
                     doc::NodeId asset) noexcept;

void project_output_node_media_runtime(
    const assets::AssetRegistry &registry, core::u64 manifest_digest,
    const topology::HardwareMap &hardware,
    const net::DirectorFrameBarrier &barrier,
    std::vector<editor::LibraryProxyRuntimeObservation> &destination);

} // namespace mediaserver::app
