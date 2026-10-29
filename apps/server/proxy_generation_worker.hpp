#pragma once

// The editor's background proxy generator: one job at a time, made in
// runs of segments in the order the views need them.

#include "proxy_generation_service.hpp"

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/assets/derivative.hpp>
#include <mediaserver/assets/proxy_policy.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace mediaserver::app {

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

    // A worker that always yields encodes every job within the Background
    // budget; a render node's does, since its output may be live at any time.
    explicit ProxyGenerationWorker(
        std::unique_ptr<server::ProxyGenerationService> artifacts,
        bool always_yield = false)
        : artifacts_(std::move(artifacts)), playing_(always_yield),
          always_yield_(always_yield),
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
            // The editor's proxy is made in segments, which can be drawn as
            // each is published; any other profile, or an asset whose length
            // was never probed, is made as one file.
            core::u32 segments = 0U;
            if (asset.requested_proxy_profile ==
                    assets::DerivativeProfileId::Proxy720pCfrH264 &&
                asset.frame_count.has_value() &&
                asset.frame_rate.has_value()) {
                segments = assets::proxy_segment_count(*asset.frame_count,
                                                       *asset.frame_rate);
            }
            scheduled_.push_back(*key);
            pending_.push_back(Job{asset, *key, segments,
                                   std::vector<bool>(segments, false), 0U});
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

    // While anything plays, the next job to start takes the Background
    // budget; the configured priority applies again from the job after a
    // stop. Measured on 4K H.264 (Release, 5800X): Fast generates 3.2x faster
    // than Background, but a Fast or Balanced encode costs editor playback a
    // few exact frames in every 720, where Background costs none.
    void set_playing(bool playing) noexcept {
        playing_.store(playing || always_yield_, std::memory_order_relaxed);
    }

    // Where the editor's views are looking: each placement's asset and the
    // source frame it asks for. The segment under each is made first, then
    // the rest of that file from there, then the rest of the file from its
    // start. A run of segments is one encode; between segments the worker
    // looks again and leaves the run when a view needs something else.
    struct Need final {
        doc::NodeId asset{};
        core::u32 source_frame{};

        bool operator==(const Need &) const = default;
    };
    void set_needs(std::span<const Need> needs) {
        std::scoped_lock lock{mutex_};
        if (!std::equal(needs.begin(), needs.end(), needs_.begin(),
                        needs_.end())) {
            needs_.assign(needs.begin(), needs.end());
        }
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
    struct Job final {
        assets::MediaAsset asset;
        assets::DerivativeKey key;
        // Zero makes the whole file in one encode.
        core::u32 segments{};
        std::vector<bool> done;
        core::u64 done_frames{};
    };
    // One generate() call: a run of segments, the join once all are made,
    // or the whole file.
    struct Pick final {
        std::size_t job{};
        std::optional<core::u32> segment;
        bool join{};
        // The run: `segment` and the missing segments straight after it.
        core::u32 segments{1U};
    };
    // The missing segments from `segment` up to the next one already made
    // or the end of the file.
    [[nodiscard]] static core::u32 run_from(const Job &job,
                                            core::u32 segment) {
        core::u32 end = segment;
        while (end < job.segments && !job.done[end]) {
            ++end;
        }
        return end - segment;
    }
    // The segment a view needs next: the one under its frame or one of the
    // two after it, whichever is missing first.
    [[nodiscard]] std::optional<Pick> need_pick_locked() const {
        constexpr core::u32 look_ahead = 2U;
        for (const Need &need : needs_) {
            for (std::size_t index = 0U; index < pending_.size(); ++index) {
                const Job &job = pending_[index];
                if (job.asset.id != need.asset || job.segments == 0U) {
                    continue;
                }
                const auto under = assets::proxy_segment_of(
                    need.source_frame, *job.asset.frame_count,
                    *job.asset.frame_rate);
                if (!under.has_value()) {
                    continue;
                }
                for (core::u32 segment = *under;
                     segment <= *under + look_ahead && segment < job.segments;
                     ++segment) {
                    if (!job.done[segment]) {
                        return Pick{index, segment, false,
                                    run_from(job, segment)};
                    }
                }
            }
        }
        return std::nullopt;
    }
    // What the views need, or else the first job's first missing segments,
    // its join, or its whole file.
    [[nodiscard]] Pick pick_locked() const {
        if (const auto needed = need_pick_locked()) {
            return *needed;
        }
        const Job &job = pending_.front();
        for (core::u32 segment = 0U; segment < job.segments; ++segment) {
            if (!job.done[segment]) {
                return Pick{0U, segment, false, run_from(job, segment)};
            }
        }
        return Pick{0U, std::nullopt, job.segments != 0U};
    }
    // After `segment` of a run is published: carry on with the next one
    // unless a view needs another segment first, or play has started or
    // stopped since the run took its CPU budget.
    [[nodiscard]] bool continue_run(const assets::DerivativeKey &key,
                                    core::u32 segment, core::u64 frames,
                                    bool yield) {
        std::scoped_lock lock{mutex_};
        const auto job = std::find_if(
            pending_.begin(), pending_.end(),
            [&](const Job &value) { return value.key == key; });
        if (job == pending_.end() || segment >= job->segments) {
            return false;
        }
        if (!job->done[segment]) {
            job->done[segment] = true;
            job->done_frames += frames;
        }
        if (playing_.load(std::memory_order_relaxed) != yield) {
            return false;
        }
        const auto needed = need_pick_locked();
        return !needed.has_value() ||
               (pending_[needed->job].key == key &&
                needed->segment == segment + 1U);
    }

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

    void finish_locked(const assets::DerivativeKey &key) {
        const auto found = std::find_if(
            progress_.begin(), progress_.end(),
            [&](const ProgressRecord &record) { return record.key == key; });
        if (found != progress_.end()) {
            found->visible = false;
        }
    }

    void run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            Pick pick;
            assets::MediaAsset asset;
            assets::DerivativeKey key;
            core::u64 done_frames{};
            {
                std::unique_lock lock{mutex_};
                condition_.wait(lock, stop,
                                [this] { return !pending_.empty(); });
                if (stop.stop_requested()) {
                    return;
                }
                pick = pick_locked();
                asset = pending_[pick.job].asset;
                key = pending_[pick.job].key;
                done_frames = pending_[pick.job].done_frames;
            }
            begin(key);
            const bool yield = playing_.load(std::memory_order_relaxed);
            std::cerr << "proxy generation for asset " << asset.id.value();
            if (pick.segment.has_value() && pick.segments != 1U) {
                std::cerr << " segments " << *pick.segment << "-"
                          << *pick.segment + pick.segments - 1U;
            } else if (pick.segment.has_value()) {
                std::cerr << " segment " << *pick.segment;
            } else if (pick.join) {
                std::cerr << " joins its segments";
            }
            std::cerr << (yield ? " yielding to playback\n"
                                : " at the configured priority\n");
            const auto generated = artifacts_->generate(
                asset, stop,
                [this, &key, &pick, done_frames](core::u64 completed,
                                                 core::u64 total) {
                    if (pick.segment.has_value()) {
                        update(key, done_frames + completed,
                               asset_frames(key, total));
                    } else if (!pick.join) {
                        update(key, completed, total);
                    }
                },
                server::ProxyGenerationService::Step{
                    yield, pick.segment, pick.join, pick.segments,
                    [this, &key, yield](core::u32 segment,
                                        const server::ProxyGenerationReceipt
                                            &receipt) {
                        return continue_run(
                            key, segment,
                            receipt.encoded_metadata.frame_count, yield);
                    }});
            if (stop.stop_requested()) {
                return;
            }
            // A run the worker left for something more urgent is not a
            // failure: what it made is kept and the next pick goes on.
            const bool run = pick.segments != 1U;
            if (run && !generated &&
                generated.error().code ==
                    server::ProxyGenerationErrorCode::Cancelled) {
                continue;
            }
            if (!generated) {
                std::cerr << "proxy generation failed for asset "
                          << asset.id.value() << " (code "
                          << static_cast<unsigned>(generated.error().code)
                          << ")\n";
            }
            std::scoped_lock lock{mutex_};
            const auto job = std::find_if(
                pending_.begin(), pending_.end(),
                [&](const Job &value) { return value.key == key; });
            if (job == pending_.end()) {
                continue;
            }
            if (run && generated) {
                continue;
            }
            if (pick.segment.has_value() && generated) {
                job->done[*pick.segment] = true;
                job->done_frames +=
                    generated.value().encoded_metadata.frame_count;
                continue;
            }
            if (!generated && job->segments != 0U) {
                // A segment or the join failed: make the whole file instead.
                job->segments = 0U;
                job->done.clear();
                job->done_frames = 0U;
                continue;
            }
            finish_locked(key);
            pending_.erase(job);
        }
    }

    // The asset's frame count, which a segment's own progress total is not.
    [[nodiscard]] core::u64 asset_frames(const assets::DerivativeKey &key,
                                         core::u64 fallback) const {
        std::scoped_lock lock{mutex_};
        const auto found = std::find_if(
            progress_.begin(), progress_.end(),
            [&](const ProgressRecord &record) { return record.key == key; });
        return found != progress_.end() && found->total_frames != 0U
            ? found->total_frames : fallback;
    }

    std::unique_ptr<server::ProxyGenerationService> artifacts_;
    std::atomic<bool> playing_;
    bool always_yield_{};
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::deque<Job> pending_;
    std::vector<Need> needs_;
    std::vector<assets::DerivativeKey> scheduled_;
    std::vector<ProgressRecord> progress_;
    std::jthread worker_;
};

} // namespace mediaserver::app
