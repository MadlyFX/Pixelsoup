#include <mediaserver/render/media_cache.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <thread>
#include <utility>

namespace mediaserver::render {
namespace {

// Where `frame_index` sits inside the clip's loop, or nothing when the caller
// stated no loop range - or stated one the frame is not actually inside, which
// a caller mixing hand-built requests with real ones can do.
[[nodiscard]] std::optional<core::u32> offset_within(
    const MediaLoopWindow& window, core::u32 frame_index) noexcept {
    if (window.count == 0U || frame_index < window.first) {
        return std::nullopt;
    }
    const core::u32 offset = frame_index - window.first;
    return offset < window.count ? std::optional{offset} : std::nullopt;
}

// How far the show moved through the clip since the last request, measured the
// short way around the loop. A clip that wrapped from its last frame to its
// first advanced by one, not backwards across its whole length: without this
// every loop point reads as a reverse scrub and aims the read-ahead at exactly
// the frames the show has just finished with.
[[nodiscard]] core::i64 loop_delta(
    const MediaLoopWindow& window,
    core::u32 previous,
    core::u32 current) noexcept {
    const auto from = offset_within(window, previous);
    const auto to = offset_within(window, current);
    if (!from.has_value() || !to.has_value()) {
        return static_cast<core::i64>(current) - static_cast<core::i64>(previous);
    }
    const auto count = static_cast<core::i64>(window.count);
    const core::i64 forward =
        (static_cast<core::i64>(*to) - static_cast<core::i64>(*from) + count)
        % count;
    return forward * 2 <= count ? forward : forward - count;
}

// The frame `delta` steps along from `frame_index`, wrapped inside the loop.
// With no loop range it is plain arithmetic that refuses to run off either end
// of the frame numbering rather than wrapping around it.
[[nodiscard]] std::optional<core::u32> advance(
    const MediaLoopWindow& window,
    core::u32 frame_index,
    core::i64 delta) noexcept {
    if (const auto offset = offset_within(window, frame_index)) {
        const auto count = static_cast<core::i64>(window.count);
        core::i64 next = (static_cast<core::i64>(*offset) + delta) % count;
        if (next < 0) {
            next += count;
        }
        return window.first + static_cast<core::u32>(next);
    }
    const core::i64 next = static_cast<core::i64>(frame_index) + delta;
    constexpr auto highest =
        static_cast<core::i64>(std::numeric_limits<core::u32>::max());
    if (next < 0 || next > highest) {
        return std::nullopt;
    }
    return static_cast<core::u32>(next);
}

} // namespace

struct MediaCache::AsyncDecodeState final
    : std::enable_shared_from_this<MediaCache::AsyncDecodeState> {
    struct Request final {
        std::string key;
        std::string source;
        core::u32 frame_index{};
        bool urgent{};
        core::u64 generation{};
        std::shared_ptr<const std::vector<std::string>> refresh_sources;
    };

    struct Ready final {
        std::shared_ptr<const DecodedFrame> frame;
        std::string source;
        std::size_t bytes{};
        core::u64 stamp{};
    };

    // One executor for the process, constructed during the first MediaCache's
    // setup. A single worker gives request priority and FIFO order one stable
    // meaning across Program, Stage, mapped outputs, and compositor layers;
    // unused per-renderer caches do not create idle threads.
    struct Executor final {
        struct PendingKey final {
            core::u64 owner{};
            std::string key;

            [[nodiscard]] bool operator<(
                const PendingKey& other) const noexcept {
                if (owner != other.owner) {
                    return owner < other.owner;
                }
                return key < other.key;
            }
        };

        struct Work final {
            std::weak_ptr<AsyncDecodeState> completion;
            std::weak_ptr<MediaSource> source;
            core::u64 owner{};
            core::u64 ticket{};
            Request request;
        };

        struct Pending final {
            core::u64 ticket{};
        };

        // Includes the one in-flight decode as well as both queued lanes.
        // This is an implementation constant, never derived from node CPU
        // count, so admission and eviction policy match across output nodes.
        static constexpr std::size_t maximum_global_pending = 256U;

        [[nodiscard]] static Executor& instance() {
            static Executor executor;
            return executor;
        }

        Executor(const Executor&) = delete;
        Executor& operator=(const Executor&) = delete;

        [[nodiscard]] core::u64 next_identity() noexcept {
            core::u64 identity =
                next_identity_.fetch_add(1U, std::memory_order_relaxed);
            if (identity == 0U) {
                identity =
                    next_identity_.fetch_add(1U, std::memory_order_relaxed);
            }
            return identity;
        }

        void enqueue(
            std::weak_ptr<AsyncDecodeState> completion,
            std::weak_ptr<MediaSource> source,
            core::u64 owner,
            std::size_t maximum_owner_pending,
            Request request) noexcept {
            try {
                std::unique_lock lock{mutex_, std::try_to_lock};
                if (!lock.owns_lock()) {
                    // Render-facing admission never waits for worker queue
                    // bookkeeping. Visible work retries on its next resolve;
                    // speculative work is allowed to disappear.
                    return;
                }

                const PendingKey requested{owner, request.key};
                if (pending_.find(requested) != pending_.end()) {
                    if (request.urgent) {
                        const auto queued = std::find_if(
                            speculative_.begin(), speculative_.end(),
                            [&](const Work& work) {
                                return work.owner == owner
                                    && work.request.key == request.key;
                            });
                        if (queued != speculative_.end()) {
                            queued->request.urgent = true;
                            urgent_.push_back(std::move(*queued));
                            speculative_.erase(queued);
                            changed_.notify_one();
                        }
                    }
                    return;
                }

                const auto owner_pending = static_cast<std::size_t>(
                    std::count_if(
                        pending_.begin(), pending_.end(),
                        [&](const auto& pending) {
                            return pending.first.owner == owner;
                        }));
                if (owner_pending >= maximum_owner_pending) {
                    if (!request.urgent
                        || !evict_speculative(owner)) {
                        return;
                    }
                }
                if (pending_.size() >= maximum_global_pending) {
                    if (!request.urgent
                        || !evict_speculative(std::nullopt)) {
                        return;
                    }
                }

                core::u64 ticket = next_ticket_++;
                if (ticket == 0U) {
                    ticket = next_ticket_++;
                }
                const auto [held, inserted] =
                    pending_.try_emplace(
                        requested, Pending{ticket});
                if (!inserted) {
                    return;
                }
                try {
                    Work work{
                        std::move(completion), std::move(source), owner,
                        ticket, std::move(request)};
                    (work.request.urgent ? urgent_ : speculative_)
                        .push_back(std::move(work));
                } catch (...) {
                    pending_.erase(held);
                    throw;
                }
                changed_.notify_one();
            } catch (...) {
                // Queue allocation is best effort. A cold frame remains stale
                // and is retried; allocation pressure never terminates render.
            }
        }

        // Operator/control-path cancellation. This may wait for the short
        // executor queue critical section; render-side enqueue uses try_lock
        // and drops while invalidation is in progress. The in-flight job is no
        // longer represented by either lane, so its Pending record is removed
        // below and AsyncDecodeState's generation fence discards its late
        // publication.
        void invalidate(core::u64 owner) noexcept {
            try {
                const std::lock_guard lock{mutex_};
                const auto erase_lane = [&](auto& lane) {
                    for (auto work = lane.begin(); work != lane.end();) {
                        if (work->owner != owner) {
                            ++work;
                            continue;
                        }
                        const auto pending = pending_.find(PendingKey{
                            work->owner, work->request.key});
                        if (pending != pending_.end()
                            && pending->second.ticket == work->ticket) {
                            pending_.erase(pending);
                        }
                        work = lane.erase(work);
                    }
                };
                erase_lane(urgent_);
                erase_lane(speculative_);
                for (auto pending = pending_.begin();
                     pending != pending_.end();) {
                    if (pending->first.owner == owner) {
                        pending = pending_.erase(pending);
                    } else {
                        ++pending;
                    }
                }
            } catch (...) {
                // Generation fencing remains authoritative even if a standard
                // container cannot finish opportunistic queued-work removal.
            }
        }

      private:
        Executor()
            : worker_([this](std::stop_token stop) { run(stop); }) {}

        ~Executor() {
            // Process teardown may wait for an operating-system read already
            // in flight. MediaCache teardown never owns or joins this worker.
            worker_.request_stop();
            changed_.notify_all();
        }

        [[nodiscard]] bool evict_speculative(
            std::optional<core::u64> owner) {
            for (std::size_t remaining = speculative_.size();
                 remaining > 0U; --remaining) {
                const std::size_t index = remaining - 1U;
                const Work& candidate = speculative_[index];
                if (owner.has_value() && candidate.owner != *owner) {
                    continue;
                }
                const auto pending = pending_.find(PendingKey{
                    candidate.owner, candidate.request.key});
                if (pending != pending_.end()
                    && pending->second.ticket == candidate.ticket) {
                    pending_.erase(pending);
                }
                // Queued jobs hold the source weakly, so erasing one here can
                // never run an arbitrary MediaSource destructor on render.
                speculative_.erase(
                    speculative_.begin()
                    + static_cast<std::ptrdiff_t>(index));
                return true;
            }
            return false;
        }

        [[nodiscard]] Work take_work() {
            Work work{};
            if (!urgent_.empty()) {
                work = std::move(urgent_.front());
                urgent_.pop_front();
            } else {
                work = std::move(speculative_.front());
                speculative_.pop_front();
            }
            return work;
        }

        void complete(const Work& work) noexcept {
            std::lock_guard lock{mutex_};
            const auto pending = pending_.find(PendingKey{
                work.owner, work.request.key});
            if (pending != pending_.end()
                && pending->second.ticket == work.ticket) {
                pending_.erase(pending);
            }
        }

        void run(std::stop_token stop) noexcept {
            while (!stop.stop_requested()) {
                Work work{};
                {
                    std::unique_lock lock{mutex_};
                    changed_.wait(lock, [&] {
                        return stop.stop_requested() || !urgent_.empty()
                            || !speculative_.empty();
                    });
                    if (stop.stop_requested()) {
                        return;
                    }
                    work = take_work();
                }

                // Queued jobs refer weakly to both lifetime domains. A worker
                // takes the source strongly only while decode() is executing;
                // dead caches are skipped and receive no late publication.
                if (!work.completion.expired()) {
                    auto source = work.source.lock();
                    if (source != nullptr) {
                        std::shared_ptr<const DecodedFrame> frame;
                        try {
                            if(work.request.refresh_sources) {
                                for(const auto& path : *work.request.refresh_sources)
                                    static_cast<void>(source->refresh(path));
                            } else frame = std::make_shared<const DecodedFrame>(
                                source->decode(
                                    work.request.source,
                                    work.request.frame_index));
                        } catch (...) {
                            frame.reset();
                        }
                        // Only the executing job retains a source. Drop it
                        // before publication; queued jobs are weak and cache
                        // teardown never waits for this blocking decode.
                        source.reset();
                        if (auto completion = work.completion.lock()) {
                            completion->publish_completed(
                                work.request, std::move(frame));
                        }
                    }
                }
                complete(work);
            }
        }

        std::atomic<core::u64> next_identity_{1U};
        core::u64 next_ticket_{1U};
        std::mutex mutex_;
        std::condition_variable changed_;
        std::deque<Work> urgent_;
        std::deque<Work> speculative_;
        std::map<PendingKey, Pending> pending_;
        // Last so the process-global executor joins before its queues and
        // synchronization primitives are released at application shutdown.
        std::jthread worker_;
    };

    AsyncDecodeState(
        std::shared_ptr<MediaSource> source,
        std::size_t maximum_pending,
        std::size_t maximum_ready_bytes)
        : source_(std::move(source)),
          maximum_pending_(std::max(maximum_pending, std::size_t{1U})),
          maximum_ready_frames_(
              std::max(maximum_pending, std::size_t{1U})),
          maximum_ready_bytes_(
              std::max(maximum_ready_bytes, std::size_t{1U})),
          failed_frame_(std::make_shared<const DecodedFrame>()),
          executor_(&Executor::instance()),
          identity_(executor_->next_identity()) {}

    AsyncDecodeState(const AsyncDecodeState&) = delete;
    AsyncDecodeState& operator=(const AsyncDecodeState&) = delete;
    ~AsyncDecodeState() = default;

    bool adopt_generation(core::u64 generation, std::span<const scene::MediaInstance> media) noexcept {
        try {
            std::unique_lock lock{mutex_,std::try_to_lock};
            if(!lock.owns_lock()) return false;
            if(authored_generation_ != generation) {
                auto paths=std::make_shared<std::vector<std::string>>();
                paths->reserve(media.size());
                for(const auto& clip:media) if(!clip.source.empty() &&
                    std::find(paths->begin(),paths->end(),clip.source)==paths->end()) paths->push_back(clip.source);
                refresh_sources_=std::move(paths); authored_generation_=generation;
                ++generation_; ready_.clear(); ready_bytes_=0; refreshed_=refresh_sources_->empty();
            }
            if(refreshed_) return true;
            Request request{"revision:"+std::to_string(generation_),{},0,true,generation_,refresh_sources_};
            executor_->enqueue(weak_from_this(),std::weak_ptr<MediaSource>{source_},identity_,maximum_pending_,std::move(request));
            return false;
        } catch(...) { return false; }
    }

    void request(
        const std::string& key,
        const std::string& source,
        core::u32 frame_index,
        bool urgent) noexcept {
        if (key.empty()) {
            return;
        }
        try {
            std::unique_lock lock{mutex_, std::try_to_lock};
            if (!lock.owns_lock()) {
                // Publishing can allocate and retire CPU frames. The render
                // path never waits behind it: a visible request is retried on
                // the next resolve and speculative work may simply be lost.
                return;
            }
            if (ready_.contains(key)) {
                return;
            }
            Request request{key, source, frame_index, urgent, generation_};
            executor_->enqueue(
                weak_from_this(), std::weak_ptr<MediaSource>{source_},
                identity_, maximum_pending_,
                std::move(request));
        } catch (...) {
            // A cold asset request is best-effort. Allocation pressure must
            // leave the last complete frame on screen, not terminate render.
        }
    }

    [[nodiscard]] std::shared_ptr<const DecodedFrame> poll(
        const std::string& key) noexcept {
        std::unique_lock lock{mutex_, std::try_to_lock};
        if (!lock.owns_lock()) {
            // A contended residency lookup is stale, never late. The caller
            // keeps its placement's last GPU texture instead of waiting for
            // the worker to finish publishing another frame.
            return nullptr;
        }
        const auto found = ready_.find(key);
        if (found == ready_.end()) {
            return nullptr;
        }
        found->second.stamp = ++stamp_;
        return found->second.frame;
    }

    // Explicit operator/control-path invalidation for BackgroundRequired
    // media. It removes completed answers for this source, cancels this cache's
    // queued cold work, and moves its generation before render admission can
    // resume. An already executing disk decode is not joined; its late
    // publication is discarded.
    [[nodiscard]] bool refresh(const std::string& source) noexcept {
        if (source.empty()) {
            return false;
        }
        try {
            const std::lock_guard lock{mutex_};
            ++generation_;
            if (generation_ == 0U) {
                ++generation_;
            }
            // One bounded cache-wide epoch avoids unbounded per-path metadata.
            // Refresh is rare operator work, so dropping this cache's other
            // queued cold stills is preferable to executing work whose old
            // generation could no longer publish; visible demand re-admits
            // them at the next render boundary.
            executor_->invalidate(identity_);
            for (auto ready = ready_.begin(); ready != ready_.end();) {
                if (ready->second.source != source) {
                    ++ready;
                    continue;
                }
                ready_bytes_ -= ready->second.bytes;
                ready = ready_.erase(ready);
            }
            return true;
        } catch (...) {
            return false;
        }
    }

private:
    void publish(
        const Request& request,
        std::shared_ptr<const DecodedFrame> frame) {
        const std::size_t bytes = frame == nullptr ? 0U : frame->pixels.size();
        const core::u64 next_stamp = stamp_ + 1U;
        const auto [entry, inserted] = ready_.try_emplace(
            request.key,
            Ready{frame, request.source, bytes, next_stamp});
        if (!inserted) {
            ready_bytes_ -= entry->second.bytes;
            entry->second = Ready{
                std::move(frame), request.source, bytes, next_stamp};
        }
        stamp_ = next_stamp;
        while (!ready_.empty()
               && (ready_.size() > maximum_ready_frames_
                   || ready_bytes_ > maximum_ready_bytes_ - bytes)) {
            const auto oldest = std::min_element(
                ready_.begin(), ready_.end(),
                [](const auto& left, const auto& right) noexcept {
                    if (left.second.stamp != right.second.stamp) {
                        return left.second.stamp < right.second.stamp;
                    }
                    return left.first < right.first;
                });
            ready_bytes_ -= oldest->second.bytes;
            ready_.erase(oldest);
        }
        if (ready_.contains(request.key)) {
            ready_bytes_ += bytes;
        }
    }

    void publish_completed(
        const Request& request,
        std::shared_ptr<const DecodedFrame> frame) noexcept {
        try {
            std::lock_guard lock{mutex_};
            if (generation_ != request.generation) {
                return;
            }
            if(request.refresh_sources) { refreshed_=true; return; }
            if (frame == nullptr
                || frame->pixels.size() > maximum_ready_bytes_) {
                // Exceptions and hostile oversized assets become a completed
                // failure marker. This keeps both memory and disk retries
                // bounded until explicit refresh advances the generation.
                frame = failed_frame_;
            }
            publish(request, std::move(frame));
        } catch (...) {
            // Publishing can fail under allocation pressure. Executor pending
            // state is still retired; a later resolve may enqueue one retry.
        }
    }

    std::shared_ptr<MediaSource> source_;
    std::size_t maximum_pending_{};
    std::size_t maximum_ready_frames_{};
    std::size_t maximum_ready_bytes_{};
    std::mutex mutex_;
    std::map<std::string, Ready> ready_;
    std::size_t ready_bytes_{};
    core::u64 stamp_{};
    core::u64 generation_{1U};
    core::u64 authored_generation_{};
    bool refreshed_{};
    std::shared_ptr<const std::vector<std::string>> refresh_sources_;
    std::shared_ptr<const DecodedFrame> failed_frame_;
    Executor* executor_{};
    core::u64 identity_{};
};

MediaCache::MediaCache() : MediaCache(MediaCacheConfig{}) {}

MediaCache::MediaCache(MediaCacheConfig config)
    : MediaCache(make_media_source(), config) {}

MediaCache::MediaCache(
    std::unique_ptr<MediaSource> source,
    MediaCacheConfig config)
    : source_(source ? std::move(source) : make_media_source()),
      config_(config) {
    config_.read_ahead_frames =
        std::max(config_.read_ahead_frames, core::u32{1U});
    config_.max_read_ahead_bytes =
        std::max(config_.max_read_ahead_bytes, std::size_t{1U});
    config_.max_read_ahead_requests =
        std::max(config_.max_read_ahead_requests, std::size_t{1U});
    config_.unknown_frame_bytes =
        std::max(config_.unknown_frame_bytes, std::size_t{1U});
    async_decode_ = std::make_shared<AsyncDecodeState>(
        source_, config_.max_read_ahead_requests,
        config_.max_read_ahead_bytes);
}

MediaCache::~MediaCache() = default;

MediaCache::MediaCache(MediaCache&&) noexcept = default;
MediaCache& MediaCache::operator=(MediaCache&&) noexcept = default;

void MediaCache::begin_frame() noexcept {
    reconcile_recordings();
    for (auto& frame : resident_) {
        // Opening frames survive active playback's allocation reuse before
        // the renderer has had a chance to restate this pass's upcoming cuts.
        frame.pinned = preloaded_keys_.contains(frame.key);
    }
    preloaded_now_.clear();
    preload_bytes_ = preload_decode_bytes_ = preload_decode_requests_ = preload_uploads_ = 0U;
    on_air_.swap(on_air_now_);
    on_air_now_.clear();
    // A placement that left the timeline has no stale picture to preserve.
    // Retaining its hold would eventually make every texture ineligible for
    // reuse at the byte limit. Pending recording snapshots protect their own
    // resources independently through resident_is_held().
    for (auto held = last_by_clip_.begin(); held != last_by_clip_.end();) {
        if (on_air_.contains(held->first)) {
            ++held;
            continue;
        }
        last_frame_by_clip_.erase(held->first);
        last_request_by_clip_.erase(held->first);
        held = last_by_clip_.erase(held);
    }
    read_ahead_requests_.clear();
    stats_ = MediaCacheStats{};
    stats_.byte_budget = max_resident_bytes;
    refresh_resident_stats();
}

void MediaCache::end_frame() noexcept {
    submit_read_ahead();
    preloaded_keys_.swap(preloaded_now_);
    trim();
    refresh_resident_stats();
}

void MediaCache::remember_read_ahead(
    const ClipKey& clip_key, const std::string& source,
    core::u32 frame_index, MediaLoopWindow window, core::i64 stride,
    core::u64 clip) {
    const auto known_bytes = decoded_frame_bytes_.find(source);
    const std::size_t frame_bytes = known_bytes == decoded_frame_bytes_.end()
        ? config_.unknown_frame_bytes
        : known_bytes->second;
    read_ahead_requests_.insert_or_assign(
        clip_key,
        ReadAheadRequest{
            source, frame_index, window, stride, clip, frame_bytes});
}

void MediaCache::submit_read_ahead() noexcept {
    if (read_ahead_requests_.empty()) {
        return;
    }
    try {
        std::vector<const ReadAheadRequest*> requests;
        requests.reserve(read_ahead_requests_.size());
        for (const auto& [clip, request] : read_ahead_requests_) {
            static_cast<void>(clip);
            requests.push_back(&request);
        }

        // Keep a stopped playhead's admitted set stable. Playback changes the
        // frame ordinal and naturally rotates which clip starts a constrained
        // first round, without turning UI repaint cadence into decode work.
        const std::size_t start = static_cast<std::size_t>(
            requests.front()->frame_index) % requests.size();
        std::size_t scheduled{};
        std::size_t scheduled_bytes{};
        for (core::u32 ahead = 1U;
             ahead <= config_.read_ahead_frames;
             ++ahead) {
            std::size_t round_requests{};
            std::size_t round_bytes{};
            for (const ReadAheadRequest* const request : requests) {
                const auto target = advance(
                    request->window, request->frame_index,
                    request->stride * static_cast<core::i64>(ahead));
                if (!target.has_value()
                    || request->frame_bytes > config_.max_read_ahead_bytes
                    || round_bytes
                        > config_.max_read_ahead_bytes - request->frame_bytes) {
                    continue;
                }
                round_bytes += request->frame_bytes;
                ++round_requests;
            }
            if (ahead > 1U
                && (round_requests
                        > config_.max_read_ahead_requests - scheduled
                    || round_bytes
                        > config_.max_read_ahead_bytes - scheduled_bytes)) {
                break;
            }
            for (std::size_t offset = 0U; offset < requests.size(); ++offset) {
                if (scheduled >= config_.max_read_ahead_requests) {
                    read_ahead_requests_.clear();
                    return;
                }
                const ReadAheadRequest& request =
                    *requests[(start + offset) % requests.size()];
                if (request.frame_bytes > config_.max_read_ahead_bytes
                    || scheduled_bytes
                        > config_.max_read_ahead_bytes - request.frame_bytes) {
                    continue;
                }
                const auto target = advance(
                    request.window, request.frame_index,
                    request.stride * static_cast<core::i64>(ahead));
                if (!target.has_value()) {
                    continue;
                }
                try {
                    if (source_->scheduling(request.source)
                        == MediaDecodeScheduling::BackgroundRequired) {
                        const auto key = source_->key(request.source, *target);
                        async_decode_->request(
                            key, request.source, *target, false);
                    } else {
                        source_->prefetch(
                            request.source, *target, request.clip);
                    }
                } catch (...) {
                    // Read-ahead is a best-effort hint. A failed allocation or
                    // source-specific error must not terminate the frame.
                    continue;
                }
                scheduled_bytes += request.frame_bytes;
                ++scheduled;
            }
        }
    } catch (...) {
        // Building the bounded request view is likewise speculative work.
    }
    read_ahead_requests_.clear();
}

ResolvedMediaFrame MediaCache::held_frame(const ClipKey& clip) {
    const auto latest = last_by_clip_.find(clip);
    if (latest == last_by_clip_.end()) {
        return {};
    }
    const auto found = by_key_.find(latest->second);
    if (found == by_key_.end()) {
        last_by_clip_.erase(latest);
        last_frame_by_clip_.erase(clip);
        return {};
    }
    found->second->pinned = true;
    resident_.splice(resident_.end(), resident_, found->second);
    ++stats_.held_frames;
    return ResolvedMediaFrame{
        &found->second->texture,
        found->second->chroma_plane
            ? &found->second->chroma_plane
            : nullptr,
        found->second->layout};
}

bool MediaCache::resident_is_held(const std::string& key) const noexcept {
    if (std::any_of(
        last_by_clip_.begin(), last_by_clip_.end(),
        [&key](const auto& held) { return held.second == key; })) {
        return true;
    }
    const auto resident = by_key_.find(key);
    if (resident == by_key_.end()) {
        return false;
    }
    const rhi::Texture* const texture = &resident->second->texture;
    return std::any_of(
        deferred_recordings_.begin(), deferred_recordings_.end(),
        [texture](const DeferredRecording& deferred) {
            return deferred.recording
                && deferred.recording.outcome()
                    == rhi::CommandRecordingOutcome::Pending
                && std::any_of(
                    deferred.snapshots.begin(), deferred.snapshots.end(),
                    [texture](const DeferredSnapshot& snapshot) {
                        return snapshot.occupied
                            && snapshot.primary == texture;
                    });
        });
}

void MediaCache::remember_candidate_claim(
    UploadCandidate& candidate, const ClipKey& clip,
    core::u32 resolved_frame) noexcept {
    const auto existing = std::find_if(
        candidate.claims.begin(), candidate.claims.end(),
        [clip_id = clip.second](const CandidateClaim& claim) {
            return claim.clip == clip_id;
        });
    if (existing != candidate.claims.end()) {
        return;
    }
    try {
        CandidateClaim claim{};
        claim.clip = clip.second;
        claim.resolved_frame = resolved_frame;
        if (const auto previous = last_by_clip_.find(clip);
            previous != last_by_clip_.end()) {
            claim.previous_key = previous->second;
            claim.had_previous = true;
        }
        candidate.claims.push_back(std::move(claim));
    } catch (...) {
        // The frame can still be sampled by this recording. Failure to retain
        // a future hold claim only makes the next unresolved frame go blank;
        // it never publishes a provisional GPU answer.
    }
}

ResolvedMediaFrame MediaCache::candidate_frame(
    UploadCandidate& candidate, const ClipKey& clip,
    core::u32 resolved_frame) noexcept {
    // If one recording draws several ready frames for the same placement, its
    // last successful observation becomes the post-submit hold. Earlier
    // candidates remain alive for draws already recorded, but no longer claim
    // placement publication when the shared command is accepted.
    for (UploadCandidate& other : candidates_) {
        if (&other == &candidate || other.source != candidate.source ||
            !other.recording.same_recording(candidate.recording)) {
            continue;
        }
        other.claims.erase(
            std::remove_if(
                other.claims.begin(), other.claims.end(),
                [clip_id = clip.second](const CandidateClaim& claim) {
                    return claim.clip == clip_id;
                }),
            other.claims.end());
    }
    remember_candidate_claim(candidate, clip, resolved_frame);
    ++candidate_stamp_;
    if (candidate_stamp_ == 0U) {
        ++candidate_stamp_;
    }
    candidate.stamp = candidate_stamp_;
    return ResolvedMediaFrame{
        &candidate.texture,
        candidate.chroma_plane ? &candidate.chroma_plane : nullptr,
        candidate.layout};
}

const MediaCache::DeferredSnapshot* MediaCache::recording_snapshot(
    const rhi::CommandRecordingToken& recording,
    const std::string& key,
    core::u64 clip) const noexcept {
    if (!recording) {
        return nullptr;
    }
    const auto found = std::find_if(
        deferred_recordings_.begin(), deferred_recordings_.end(),
        [&recording](const DeferredRecording& candidate) {
            return candidate.recording.same_recording(recording);
        });
    if (found == deferred_recordings_.end()) {
        return nullptr;
    }
    const auto snapshot = std::find_if(
        found->snapshots.begin(), found->snapshots.end(),
        [&key, clip](const DeferredSnapshot& candidate) {
            return candidate.occupied && candidate.clip == clip
                && candidate.key == key;
        });
    if (snapshot == found->snapshots.end()) {
        return nullptr;
    }
    return &*snapshot;
}

bool MediaCache::defer_recording(
    const rhi::CommandRecordingToken& recording,
    const std::string& key,
    core::u64 clip,
    ResolvedMediaFrame frame, bool pending) noexcept {
    if (!recording) {
        return false;
    }
    auto found = std::find_if(
        deferred_recordings_.begin(), deferred_recordings_.end(),
        [&recording](const DeferredRecording& candidate) {
            return candidate.recording.same_recording(recording);
        });
    if (found == deferred_recordings_.end()) {
        found = std::find_if(
            deferred_recordings_.begin(), deferred_recordings_.end(),
            [](const DeferredRecording& candidate) {
                return !candidate.recording;
            });
        if (found == deferred_recordings_.end()) {
            return false;
        }
        found->recording = recording;
    }
    if (recording_snapshot(recording, key, clip)) {
        return true;
    }
    auto snapshot = std::find_if(
        found->snapshots.begin(), found->snapshots.end(),
        [](const DeferredSnapshot& candidate) {
            return !candidate.occupied;
        });
    if (snapshot == found->snapshots.end()) {
        return false;
    }
    try {
        snapshot->key = key;
    } catch (...) {
        return false;
    }
    snapshot->clip = clip;
    snapshot->primary = frame.primary;
    snapshot->chroma = frame.chroma;
    snapshot->layout = frame.layout;
    snapshot->occupied = true;
    snapshot->pending = pending;
    return true;
}

void MediaCache::reconcile_recordings() noexcept {
    for (auto& deferred : deferred_recordings_) {
        if (!deferred.recording) {
            continue;
        }
        const auto outcome = deferred.recording.outcome();
        if (outcome != rhi::CommandRecordingOutcome::Pending) {
            deferred.recording = {};
            for (auto& snapshot : deferred.snapshots) {
                snapshot = DeferredSnapshot{};
            }
        }
    }

    for (auto entry = candidates_.begin(); entry != candidates_.end();) {
        if (entry->state == CandidateUploadState::Retryable ||
            entry->state == CandidateUploadState::Quarantined) {
            ++entry;
            continue;
        }
        const auto outcome = entry->recording.outcome();
        if (outcome == rhi::CommandRecordingOutcome::Pending) {
            ++entry;
            continue;
        }
        if (outcome == rhi::CommandRecordingOutcome::Canceled) {
            entry->recording = {};
            entry->claims.clear();
            if (entry->discard) {
                candidate_bytes_ -= entry->bytes;
                entry = candidates_.erase(entry);
                continue;
            }
            entry->state = CandidateUploadState::Retryable;
            ++entry;
            continue;
        }
        if (outcome == rhi::CommandRecordingOutcome::Submitted &&
            entry->state == CandidateUploadState::RecordingPartial &&
            !entry->discard) {
            // The same queue orders the next row transfer after these rows.
            // Keep the allocation private and resume at the accepted offset;
            // a full image need not fit into one command's upload arena.
            entry->committed_rows = entry->recorded_rows;
            entry->committed_chroma_rows = entry->recorded_chroma_rows;
            entry->recording = {};
            entry->state = CandidateUploadState::Retryable;
            ++entry;
            continue;
        }
        if (outcome != rhi::CommandRecordingOutcome::Submitted ||
            entry->state == CandidateUploadState::RecordingPartial ||
            entry->discard) {
            // A submitted partial upload may still be executing. With no GPU
            // completion fence in the recording receipt, retaining it for the
            // cache lifetime is the only conservative destruction policy.
            entry->recording = {};
            entry->claims.clear();
            entry->state = CandidateUploadState::Quarantined;
            ++entry;
            continue;
        }

        // Complete recording accepted: allocate all throwing map/list storage
        // before moving either texture out of the candidate. On allocation
        // pressure the submitted resource remains retained and publication is
        // retried at a later explicit cache observation.
        try {
            const auto [mapped, inserted] =
                by_key_.try_emplace(entry->key, resident_.end());
            if (!inserted) {
                entry->recording = {};
                entry->claims.clear();
                entry->state = CandidateUploadState::Quarantined;
                ++entry;
                continue;
            }
            try {
                resident_.emplace_back();
            } catch (...) {
                by_key_.erase(mapped);
                throw;
            }
            auto resident = std::prev(resident_.end());
            try {
                resident->key = entry->key;
                resident->source = entry->source;
            } catch (...) {
                resident_.erase(resident);
                by_key_.erase(mapped);
                throw;
            }
            resident->layout = entry->layout;
            resident->bytes = entry->bytes;
            resident->texture = std::move(entry->texture);
            resident->chroma_plane = std::move(entry->chroma_plane);
            resident->pinned = false;
            mapped->second = resident;
            resident_bytes_ += resident->bytes;

            for (const CandidateClaim& claim : entry->claims) {
                try {
                    const ClipKey clip{entry->source, claim.clip};
                    const auto current = last_by_clip_.find(clip);
                    const bool unchanged = claim.had_previous
                        ? current != last_by_clip_.end() &&
                            current->second == claim.previous_key
                        : current == last_by_clip_.end();
                    if (unchanged ||
                        (current != last_by_clip_.end() &&
                         current->second == entry->key)) {
                        last_by_clip_[clip] = entry->key;
                        last_frame_by_clip_[clip] = claim.resolved_frame;
                    }
                } catch (...) {
                    // Publication by truthful source key is authoritative.
                    // A lost placement hold is a stale/blank policy downgrade,
                    // not a reason to roll back a submitted GPU resource.
                }
            }

            candidate_bytes_ -= entry->bytes;
            entry = candidates_.erase(entry);
        } catch (...) {
            ++entry;
        }
    }
    refresh_resident_stats();
}

void MediaCache::erase(ResidentList::iterator entry) noexcept {
    // Several placements can be holding this one texture - two clips cut from
    // the same file and sitting on the same frame share it - so every claim on
    // it is dropped, not just the first one found.
    for (auto held = last_by_clip_.begin(); held != last_by_clip_.end();) {
        if (held->second == entry->key) {
            last_frame_by_clip_.erase(held->first);
            held = last_by_clip_.erase(held);
        } else {
            ++held;
        }
    }
    by_key_.erase(entry->key);
    resident_bytes_ -= entry->bytes;
    resident_.erase(entry);
}

void MediaCache::trim() noexcept {
    while (resident_.size() > max_resident_frames
           || resident_bytes_ > max_resident_bytes) {
        const auto candidate = std::find_if(
            resident_.begin(), resident_.end(),
            [](const ResidentFrame& frame) { return !frame.pinned; });
        if (candidate == resident_.end()) {
            // Current-frame references are authoritative. A scene using more
            // than four independent clips may exceed the soft frame count, and
            // one texture larger than the byte budget must remain drawable.
            break;
        }
        erase(candidate);
    }
}

void MediaCache::refresh_resident_stats() noexcept {
    // Candidates own real texture/staging memory even though their cache keys
    // are not visible until submit. Count that pressure without making them
    // eligible for ordinary resident trimming or reuse.
    stats_.resident_frames = resident_.size() + candidates_.size();
    stats_.resident_bytes = resident_bytes_ + candidate_bytes_;
    stats_.byte_budget = max_resident_bytes;
}

void MediaCache::prewarm(
    const std::string& source_template, core::u32 frame_index, core::u64 clip) {
    if (source_->scheduling(source_template)
        == MediaDecodeScheduling::BackgroundRequired) {
        async_decode_->request(
            source_->key(source_template, frame_index),
            source_template, frame_index, false);
        return;
    }
    source_->prefetch(source_template, frame_index, clip);
}

bool MediaCache::adopt_generation(core::u64 generation,
    std::span<const scene::MediaInstance> media) noexcept {
    if(generation == 0) return true;
    if(authored_generation_ != generation) {
        preloaded_keys_.clear();
        preloaded_now_.clear();
        reconcile_recordings();
        for(auto entry=candidates_.begin();entry!=candidates_.end();) {
            if(entry->state==CandidateUploadState::RecordingComplete || entry->state==CandidateUploadState::RecordingPartial) {
                entry->discard=true; entry->claims.clear(); ++entry;
            } else { candidate_bytes_-=entry->bytes; entry=candidates_.erase(entry); }
        }
        while(!resident_.empty()) erase(resident_.begin());
        last_by_clip_.clear(); last_frame_by_clip_.clear(); last_request_by_clip_.clear();
        read_ahead_requests_.clear(); on_air_.clear(); on_air_now_.clear(); decoded_frame_bytes_.clear();
        authored_generation_=generation;
    }
    return async_decode_->adopt_generation(generation,media);
}

void MediaCache::prewarm(
    rhi::Device& device, rhi::CommandList& commands,
    const std::string& source, core::u32 first,
    MediaLoopWindow window, core::u64 clip) noexcept {
    try {
        const auto key = source_->key(source, first);
        if (key.empty() || preloaded_now_.contains(key)) return;
        // File/codec work is admitted only to speculative worker queues.
        prewarm(source, first, clip);
        const bool background = source_->scheduling(source)
            == MediaDecodeScheduling::BackgroundRequired;
        auto decoded = background ? async_decode_->poll(key)
                                  : source_->try_prefetched(source, first, clip);
        if (decoded && decoded->valid())
            decoded_frame_bytes_[source] = decoded->pixels.size();

        // Seed a short forward runway as well as the opening frame. Keep it
        // bounded independently of live read-ahead, and never walk the same
        // one-frame still or loop twice.
        const auto known = decoded_frame_bytes_.find(source);
        const std::size_t bytes = known == decoded_frame_bytes_.end()
            ? config_.unknown_frame_bytes : known->second;
        const auto runway_budget = std::min(
            config_.max_read_ahead_bytes, max_preloaded_bytes);
        for (core::u32 ahead = 1U; ahead <= config_.read_ahead_frames; ++ahead) {
            if ((window.count != 0U && ahead >= window.count)
                || preload_decode_requests_ >= config_.max_read_ahead_requests
                || bytes > runway_budget
                || preload_decode_bytes_ > runway_budget - bytes) break;
            const auto next = advance(window, first, ahead);
            if (!next || source_->key(source, *next) == key) break;
            prewarm(source, *next, clip);
            ++preload_decode_requests_;
            preload_decode_bytes_ += bytes;
        }

        reconcile_recordings();
        const auto resident = by_key_.find(key);
        const auto candidate = std::find_if(candidates_.begin(), candidates_.end(),
            [&key](const UploadCandidate& value) { return value.key == key; });
        const std::size_t slot_bytes = resident != by_key_.end()
            ? resident->second->bytes : candidate != candidates_.end()
            ? candidate->bytes : decoded && decoded->valid()
            ? (decoded->pixels.size() > std::numeric_limits<std::size_t>::max() / 2U
                ? std::numeric_limits<std::size_t>::max()
                : decoded->pixels.size() * 2U) : 0U;
        if (slot_bytes == 0U || slot_bytes > max_preloaded_bytes
            || preloaded_now_.size() >= max_preloaded_frames
            || preload_bytes_ > max_preloaded_bytes - slot_bytes) return;
        if (resident != by_key_.end()) {
            resident->second->pinned = true;
            resident_.splice(resident_.end(), resident_, resident->second);
        } else {
            if (candidate == candidates_.end()
                || candidate->state == CandidateUploadState::Retryable) {
                // Spread texture allocation/transfer across frames, even when
                // many network decodes complete at once.
                if (preload_uploads_ >= 1U) return;
                if (!decoded && candidate != candidates_.end()) decoded = candidate->decoded;
                if (!upload_decoded(device, commands, source, key, first, decoded)) return;
                ++preload_uploads_;
            } else if (candidate->state != CandidateUploadState::RecordingComplete) {
                return;
            }
        }
        preloaded_now_.insert(key);
        preload_bytes_ += slot_bytes;
    } catch (...) {
        // Speculation can be dropped; current-frame readiness is untouched.
    }
}

bool MediaCache::refresh_source(
    const std::string& source_template) noexcept {
    if (source_template.empty()) {
        return false;
    }
    const bool refreshed =
        source_->scheduling(source_template)
            == MediaDecodeScheduling::BackgroundRequired
        ? async_decode_->refresh(source_template)
        : source_->refresh(source_template);
    if (!refreshed) {
        return false;
    }

    reconcile_recordings();

    // A refresh is explicitly a safe cache boundary. Retryable and already
    // reconciled candidates can be destroyed here; a still-live recording is
    // only marked for discard and remains retained through its outcome.
    for (auto entry = candidates_.begin(); entry != candidates_.end();) {
        if (entry->source != source_template) {
            ++entry;
            continue;
        }
        if (entry->state == CandidateUploadState::RecordingComplete ||
            entry->state == CandidateUploadState::RecordingPartial) {
            entry->discard = true;
            entry->claims.clear();
            ++entry;
            continue;
        }
        candidate_bytes_ -= entry->bytes;
        entry = candidates_.erase(entry);
    }

    // This is an explicit safe-boundary operation, not render-side maintenance.
    // Retire every GPU answer derived from the source so the next resolve cannot
    // bypass the freshly invalidated CPU registry through an old resident hit.
    for (auto entry = resident_.begin(); entry != resident_.end();) {
        if (entry->source != source_template) {
            ++entry;
            continue;
        }
        const auto retired = entry++;
        erase(retired);
    }
    const auto erase_clip_state = [&](auto& states) {
        for (auto entry = states.begin(); entry != states.end();) {
            if (entry->first.first == source_template) {
                entry = states.erase(entry);
            } else {
                ++entry;
            }
        }
    };
    erase_clip_state(last_by_clip_);
    erase_clip_state(last_frame_by_clip_);
    erase_clip_state(last_request_by_clip_);
    erase_clip_state(read_ahead_requests_);
    for (auto entry = on_air_.begin(); entry != on_air_.end();) {
        if (entry->first == source_template) {
            entry = on_air_.erase(entry);
        } else {
            ++entry;
        }
    }
    for (auto entry = on_air_now_.begin(); entry != on_air_now_.end();) {
        if (entry->first == source_template) {
            entry = on_air_now_.erase(entry);
        } else {
            ++entry;
        }
    }
    decoded_frame_bytes_.erase(source_template);
    refresh_resident_stats();
    return true;
}

ResolvedMediaFrame MediaCache::resolve(
    rhi::Device& device,
    rhi::CommandList& commands,
    const std::string& source_template,
    core::u32 frame_index,
    MediaLoopWindow window,
    core::u64 clip) {
    reconcile_recordings();
    const auto key = source_->key(source_template, frame_index);
    if (key.empty()) {
        return {};
    }
    const ClipKey clip_key{source_template, clip};
    // A clip that was not on air last pass is starting, not continuing - the
    // show has cut to it, or looped back onto it. What it was doing when it
    // last left the air says nothing about now: the frame it ended on is
    // further into the file than the one it is about to open with, and the
    // direction it was travelling then is not the direction it is travelling
    // now. Holding either is how the end of a clip appears on the wall at the
    // moment it begins.
    //
    // Decided once per pass, on the placement's first resolve: a radially
    // expanded clip resolves several times against one identity, and the second
    // of those is continuing what the first just established, not re-entering.
    const bool first_resolve_this_pass = on_air_now_.insert(clip_key).second;
    if (first_resolve_this_pass && on_air_.find(clip_key) == on_air_.end()) {
        last_by_clip_.erase(clip_key);
        last_frame_by_clip_.erase(clip_key);
        last_request_by_clip_.erase(clip_key);
    }
    // How far the show moved this frame is how far it will move next frame, so
    // the stride is taken from what was actually asked for rather than assumed
    // to be one: a 24fps clip on a 60fps show advances by less than a frame at
    // a time, a 60fps clip on a 30fps show advances by two.
    const auto previous_request = last_request_by_clip_.find(clip_key);
    const core::i64 measured = previous_request == last_request_by_clip_.end()
        ? 1
        : loop_delta(window, previous_request->second, frame_index);
    last_request_by_clip_[clip_key] = frame_index;

    // A step larger than playback is the show moving, not the clip playing: a
    // cut, a scrub, or the show's own range looping under a clip that never
    // left the air. Whichever way it went, play resumes *forward* from where it
    // landed, so the read-ahead does too. Following the jump backwards instead
    // walks out of the clip towards the end of the file, which is the one place
    // the frames coming next certainly are not.
    const bool jumped = std::abs(measured) > max_playback_stride;
    const core::i64 stride = jumped || measured == 0 ? 1 : measured;
    // A jump backwards also retires the held frame. It sits further into the
    // clip than the one now being asked for, so holding it puts a picture the
    // clip has not reached yet on the wall - and where several clips are cut
    // from one file, that picture is recognisably a later clip's. Nothing is
    // the honest answer until the decode lands. A forward jump is left holding:
    // the frame behind is ordinary judder, not a glimpse of the future.
    if (jumped && measured < 0) {
        last_by_clip_.erase(clip_key);
        last_frame_by_clip_.erase(clip_key);
    }
    const rhi::CommandRecordingToken recording = commands.recording_token();
    const auto held_for_recording = [&]() -> ResolvedMediaFrame {
        // Every non-answer freezes the exact held texture (or exact null) for
        // this requested key and placement within the command. Merely marking
        // that it missed is insufficient: another command can publish a new
        // last_by_clip_ answer between mapped views.
        ResolvedMediaFrame result;
        if (recording) {
            const auto provisional = std::find_if(
                candidates_.begin(), candidates_.end(),
                [&recording, &source_template, clip](
                    const UploadCandidate& candidate) {
                    return candidate.state ==
                            CandidateUploadState::RecordingComplete &&
                        candidate.source == source_template &&
                        candidate.recording.same_recording(recording) &&
                        std::any_of(
                            candidate.claims.begin(), candidate.claims.end(),
                            [clip](const CandidateClaim& claim) {
                                return claim.clip == clip;
                            });
                });
            if (provisional != candidates_.end()) {
                ++stats_.held_frames;
                result = ResolvedMediaFrame{
                    &provisional->texture,
                    provisional->chroma_plane
                        ? &provisional->chroma_plane
                        : nullptr,
                    provisional->layout};
            }
        }
        // Work recorded by this same command is allowed to establish a newer
        // provisional hold for a placement. A publication from any other
        // command is not: in that case the exact earlier snapshot wins.
        if (!result) {
            if (const auto frozen = recording_snapshot(
                    recording, key, clip)) {
                if (frozen->primary) {
                    ++stats_.held_frames;
                }
                return {frozen->primary, frozen->chroma, frozen->layout};
            }
        }
        if (!result) {
            result = held_frame(clip_key);
        }
        if (recording
            && !defer_recording(recording, key, clip, result)) {
            // Without a retained exact snapshot, null is the only answer that
            // is guaranteed reproducible on every later view of this command.
            return {};
        }
        return result;
    };
    if (const auto frozen = recording_snapshot(recording, key, clip)) {
        if (!frozen->pending) {
            return {frozen->primary, frozen->chroma, frozen->layout};
        }
        ++stats_.pending_requests;
        remember_read_ahead(
            clip_key, source_template, frame_index, window, stride, clip);
        return held_for_recording();
    }
    const auto found = by_key_.find(key);
    if (found != by_key_.end()) {
        found->second->pinned = true;
        resident_.splice(resident_.end(), resident_, found->second);
        last_by_clip_[clip_key] = key;
        last_frame_by_clip_[clip_key] = frame_index;
        ++stats_.cache_hits;
        remember_read_ahead(
            clip_key, source_template, frame_index, window, stride, clip);
        return ResolvedMediaFrame{
            &found->second->texture,
            found->second->chroma_plane
                ? &found->second->chroma_plane
                : nullptr,
            found->second->layout};
    }

    // Finish a placement's admitted transfer before admitting another frame.
    // Otherwise a moving large video can fill every candidate slot with one
    // abandoned partial frame per tick and never publish any complete image.
    const auto unfinished = std::find_if(candidates_.begin(), candidates_.end(),
        [&](const UploadCandidate& value) {
            return value.key != key && value.source == source_template &&
                !value.discard && value.resolved_frame <= frame_index &&
                (value.state == CandidateUploadState::RecordingPartial ||
                 value.state == CandidateUploadState::Retryable) &&
                std::any_of(value.claims.begin(), value.claims.end(),
                    [clip](const CandidateClaim& claim) { return claim.clip == clip; });
        });
    if (unfinished != candidates_.end()) {
        remember_read_ahead(clip_key, source_template, frame_index, window, stride, clip);
        ++stats_.pending_requests;
        if (unfinished->state == CandidateUploadState::Retryable && recording) {
            const auto continued = upload_decoded(device, commands, source_template,
                unfinished->key, unfinished->resolved_frame, unfinished->decoded);
            remember_candidate_claim(*unfinished, clip_key, unfinished->resolved_frame);
            if (continued) {
                const auto ready = candidate_frame(*unfinished, clip_key, unfinished->resolved_frame);
                static_cast<void>(defer_recording(recording, key, clip, ready));
                return ready;
            }
        }
        return held_for_recording();
    }

    auto candidate = std::find_if(
        candidates_.begin(), candidates_.end(),
        [&key](const UploadCandidate& value) { return value.key == key; });
    if (candidate != candidates_.end() &&
        candidate->state == CandidateUploadState::RecordingComplete &&
        candidate->recording.same_recording(recording)) {
        ++stats_.cache_hits;
        remember_read_ahead(
            clip_key, source_template, frame_index, window, stride, clip);
        return candidate_frame(*candidate, clip_key, candidate->resolved_frame);
    }
    if (candidate != candidates_.end() &&
        candidate->state != CandidateUploadState::Retryable) {
        ++stats_.pending_requests;
        remember_read_ahead(
            clip_key, source_template, frame_index, window, stride, clip);
        return held_for_recording();
    }
    if (!recording) {
        ++stats_.pending_requests;
        remember_read_ahead(
            clip_key, source_template, frame_index, window, stride, clip);
        return held_for_recording();
    }

    std::shared_ptr<const DecodedFrame> frame;
    if (candidate != candidates_.end()) {
        frame = candidate->decoded;
    } else if (source_->scheduling(source_template)
        == MediaDecodeScheduling::BackgroundRequired) {
        frame = async_decode_->poll(key);
        if (frame == nullptr) {
            async_decode_->request(
                key, source_template, frame_index, true);
            // Poll once more so a tiny memory-backed decode that completed
            // during queue insertion can publish without adding a frame of
            // avoidable latency. Both polls are bounded map lookups.
            frame = async_decode_->poll(key);
        }
    } else {
        frame = source_->try_decode(source_template, frame_index, clip);
        if (frame == nullptr) {
            frame = source_->try_decode_latest(
                source_template, frame_index, clip);
        }
    }
    if (frame != nullptr && frame->valid()) {
        decoded_frame_bytes_[source_template] = frame->pixels.size();
    }
    remember_read_ahead(
        clip_key, source_template, frame_index, window, stride, clip);
    if (frame == nullptr) {
        ++stats_.pending_requests;
        return held_for_recording();
    }
    if (!frame->valid()) {
        // A completed failure is offline media, not a late decode. Retaining
        // the last image here would hide a deleted file or missing sequence
        // frame indefinitely. Freeze the null answer across mapped views.
        last_by_clip_.erase(clip_key);
        last_frame_by_clip_.erase(clip_key);
        if (recording && !defer_recording(recording, key, clip, {}, false)) {
            ++stats_.pending_requests;
        }
        return {};
    }

    const bool late = frame->source_frame.has_value() &&
        *frame->source_frame != frame_index;
    if (late) {
        ++stats_.pending_requests;
    }

    const core::u32 resolved_frame =
        frame->source_frame.value_or(frame_index);
    const auto previously_presented = last_frame_by_clip_.find(clip_key);
    if (late && measured >= 0 &&
        previously_presented != last_frame_by_clip_.end() &&
        resolved_frame < previously_presented->second) {
        // Exact/shared results and late stream-local results can complete in a
        // different order. Never replace a forward-playing clip's newer held
        // image with an older fallback; that visual reversal is rubberbanding.
        return held_for_recording();
    }
    if (late) {
        ++stats_.held_frames;
    }

    const std::string resolved_key = frame->source_frame.has_value()
        ? source_->key(source_template, *frame->source_frame)
        : key;
    if (resolved_key.empty()) {
        return held_for_recording();
    }
    if (const auto resident = by_key_.find(resolved_key);
        resident != by_key_.end()) {
        resident->second->pinned = true;
        resident_.splice(resident_.end(), resident_, resident->second);
        last_by_clip_[clip_key] = resolved_key;
        last_frame_by_clip_[clip_key] = resolved_frame;
        ++stats_.cache_hits;
        return ResolvedMediaFrame{
            &resident->second->texture,
            resident->second->chroma_plane
                ? &resident->second->chroma_plane
                : nullptr,
            resident->second->layout};
    }

    if (candidate == candidates_.end() || candidate->key != resolved_key) {
        candidate = std::find_if(
            candidates_.begin(), candidates_.end(),
            [&resolved_key](const UploadCandidate& value) {
                return value.key == resolved_key;
            });
    }
    if (candidate != candidates_.end() &&
        candidate->state == CandidateUploadState::RecordingComplete &&
        candidate->recording.same_recording(recording)) {
        ++stats_.cache_hits;
        return candidate_frame(
            *candidate, clip_key, candidate->resolved_frame);
    }
    if (candidate != candidates_.end() &&
        candidate->state != CandidateUploadState::Retryable) {
        ++stats_.pending_requests;
        return held_for_recording();
    }
    if (candidate != candidates_.end()) {
        frame = candidate->decoded;
    }

    const auto uploaded = upload_decoded(device, commands, source_template,
                                         resolved_key, resolved_frame, frame);
    candidate = std::find_if(candidates_.begin(), candidates_.end(),
        [&resolved_key](const UploadCandidate& value) { return value.key == resolved_key; });
    if (!uploaded) {
        if (candidate != candidates_.end())
            remember_candidate_claim(*candidate, clip_key, resolved_frame);
        ++stats_.pending_requests;
        return held_for_recording();
    }
    return candidate_frame(*candidate, clip_key, resolved_frame);
}

ResolvedMediaFrame MediaCache::upload_decoded(
    rhi::Device& device, rhi::CommandList& commands,
    const std::string& source_template, const std::string& resolved_key,
    core::u32 resolved_frame, std::shared_ptr<const DecodedFrame> frame) {
    const auto recording = commands.recording_token();
    if (!recording || !frame || !frame->valid()) return {};
    auto candidate = std::find_if(candidates_.begin(), candidates_.end(),
        [&resolved_key](const UploadCandidate& value) { return value.key == resolved_key; });
    if (candidate != candidates_.end()) {
        if (candidate->state == CandidateUploadState::RecordingComplete
            && candidate->recording.same_recording(recording)) {
            return {&candidate->texture,
                candidate->chroma_plane ? &candidate->chroma_plane : nullptr,
                candidate->layout};
        }
        if (candidate->state != CandidateUploadState::Retryable) return {};
        frame = candidate->decoded;
    }
    const bool p010 =
        frame->layout == DecodedPixelLayout::P010Rec709Limited ||
        frame->layout == DecodedPixelLayout::P010Rec709Full;
    const std::size_t luma_bytes = p010
        ? static_cast<std::size_t>(frame->width) * frame->height * 2U
        : 0U;
    const std::size_t chroma_bytes = p010 ? luma_bytes / 2U : 0U;
    if (p010 &&
        (frame->width % 2U != 0U || frame->height % 2U != 0U ||
         frame->pixels.size() != luma_bytes + chroma_bytes)) {
        return {};
    }

    rhi::TextureDesc desc{};
    desc.width = frame->width;
    desc.height = frame->height;
    desc.format = p010 ? rhi::TextureFormat::R16Unorm : frame->format;
    desc.sampled = true;
    desc.streaming_upload = true;
    const std::size_t pixel_bytes = frame->pixels.size();
    const std::size_t slot_bytes =
        pixel_bytes > std::numeric_limits<std::size_t>::max() / 2U
        ? std::numeric_limits<std::size_t>::max()
        : pixel_bytes * 2U;

    if (candidate == candidates_.end()) {
        rhi::Texture texture;
        rhi::Texture chroma_plane;

        // A canceled candidate is safe to reclaim because the recording token
        // publishes Canceled only after backend rollback. Reclaim only when the
        // dedicated candidate bound is full; an exact-key retry above always
        // keeps and re-records its own allocation.
        if (candidates_.size() >= max_upload_candidates) {
            const auto reusable_candidate = std::min_element(
                candidates_.begin(), candidates_.end(),
                [](const UploadCandidate& left,
                   const UploadCandidate& right) {
                    if (left.state != CandidateUploadState::Retryable || !left.texture.safe_to_release() ||
                        (left.chroma_plane && !left.chroma_plane.safe_to_release())) {
                        return false;
                    }
                    if (right.state != CandidateUploadState::Retryable || !right.texture.safe_to_release() ||
                        (right.chroma_plane && !right.chroma_plane.safe_to_release())) {
                        return true;
                    }
                    return left.stamp < right.stamp;
                });
            if (reusable_candidate == candidates_.end() ||
                reusable_candidate->state !=
                    CandidateUploadState::Retryable || !reusable_candidate->texture.safe_to_release() ||
                (reusable_candidate->chroma_plane && !reusable_candidate->chroma_plane.safe_to_release())) {
                return {};
            }
            if (reusable_candidate->texture.desc() == desc &&
                static_cast<bool>(reusable_candidate->chroma_plane) == p010) {
                texture = std::move(reusable_candidate->texture);
                chroma_plane =
                    std::move(reusable_candidate->chroma_plane);
            }
            candidate_bytes_ -= reusable_candidate->bytes;
            candidates_.erase(reusable_candidate);
        }

        // Prefer allocations no placement is relying on as its stale answer.
        // A canceled upload therefore never erases the picture it is meant to
        // hold while the next recording retries.
        if (!texture) {
            const auto reusable = std::find_if(
                resident_.begin(), resident_.end(),
                [this, &desc, p010](const ResidentFrame& value) {
                    return !value.pinned && !resident_is_held(value.key) &&
                        value.texture.desc() == desc &&
                        static_cast<bool>(value.chroma_plane) == p010;
                });
            if (reusable != resident_.end()) {
                texture = std::move(reusable->texture);
                chroma_plane = std::move(reusable->chroma_plane);
                erase(reusable);
            }
        }

        // Bound all actual GPU/staging ownership, including pending and
        // quarantined candidates. Only an unpinned published frame that no
        // placement holds is reclaimable; a candidate may still be captured
        // by a recording and is never an admission victim.
        if (slot_bytes > max_resident_bytes) {
            return {};
        }
        while (resident_bytes_ + candidate_bytes_ >
               max_resident_bytes - slot_bytes) {
            const auto unused = std::find_if(
                resident_.begin(), resident_.end(),
                [this](const ResidentFrame& value) {
                    return !value.pinned && !resident_is_held(value.key);
                });
            if (unused == resident_.end()) {
                // A scrub can abandon several incomplete large frames before
                // the candidate-count cap is reached. Their submitted rows
                // may be reclaimed once the GPU fence has completed; an open
                // recording or transfer still on the queue remains protected.
                const auto abandoned = std::find_if(candidates_.begin(), candidates_.end(),
                    [](const UploadCandidate& value) {
                        return value.state == CandidateUploadState::Retryable &&
                            value.texture.safe_to_release() &&
                            (!value.chroma_plane || value.chroma_plane.safe_to_release());
                    });
                if (abandoned == candidates_.end()) return {};
                candidate_bytes_ -= abandoned->bytes;
                candidates_.erase(abandoned);
                continue;
            }
            erase(unused);
        }

        if (!texture) {
            auto created = device.create_texture(desc);
            if (!created) {
                return {};
            }
            texture = std::move(created).value();
        }

        if (p010 && !chroma_plane) {
            rhi::TextureDesc plane{};
            plane.width = frame->width / 2U;
            plane.height = frame->height / 2U;
            plane.format = rhi::TextureFormat::Rg16Unorm;
            plane.sampled = true;
            plane.streaming_upload = true;
            auto created = device.create_texture(plane);
            if (!created) {
                return {};
            }
            chroma_plane = std::move(created).value();
        }

        try {
            candidates_.emplace_back();
            auto inserted = std::prev(candidates_.end());
            try {
                inserted->key = resolved_key;
                inserted->source = source_template;
            } catch (...) {
                candidates_.erase(inserted);
                throw;
            }
            inserted->resolved_frame = resolved_frame;
            inserted->layout = frame->layout;
            inserted->bytes = slot_bytes;
            inserted->decoded = frame;
            inserted->texture = std::move(texture);
            inserted->chroma_plane = std::move(chroma_plane);
            ++candidate_stamp_;
            if (candidate_stamp_ == 0U) {
                ++candidate_stamp_;
            }
            inserted->stamp = candidate_stamp_;
            candidate_bytes_ += inserted->bytes;
            candidate = inserted;
        } catch (...) {
            return {};
        }
    }

    if (!candidate->texture ||
        (p010 && !candidate->chroma_plane)) {
        candidate->state = CandidateUploadState::Quarantined;
        return {};
    }

    // The token is retained before the first upload. Every failure path below
    // keeps textures and decoded bytes in the candidate until this recording
    // is known canceled or submitted.
    candidate->recording = recording;
    candidate->claims.clear();
    candidate->discard = false;
    candidate->recorded_rows = candidate->committed_rows;
    candidate->recorded_chroma_rows = candidate->committed_chroma_rows;
    // Leave room for uniforms and geometry after media staging. Capacity comes
    // from the RHI, which accounts for row padding and fragmented upload pages.
    constexpr core::u64 draw_reserve_bytes = 1024U * 1024U;
    const auto upload_rows = [&](rhi::Texture& texture,
                                 std::span<const core::u8> pixels,
                                 core::u32& completed) {
        if (completed == texture.height()) return true;
        const core::u32 rows = std::min(texture.height() - completed,
            commands.texture_upload_row_capacity(texture, draw_reserve_bytes));
        if (rows == 0U) return false;
        const std::size_t row_bytes = pixels.size() / texture.height();
        if (!commands.upload_texture_rows(texture, 0U, 0U, completed, rows,
            pixels.subspan(static_cast<std::size_t>(completed) * row_bytes,
                           static_cast<std::size_t>(rows) * row_bytes))) return false;
        completed += rows;
        return completed == texture.height();
    };
    bool uploaded = false;
    if (p010) {
        const std::span<const core::u8> luma{
            frame->pixels.data(), luma_bytes};
        const std::span<const core::u8> chroma{
            frame->pixels.data() + luma_bytes, chroma_bytes};
        const bool luma_uploaded = upload_rows(candidate->texture, luma, candidate->recorded_rows);
        const bool chroma_uploaded = luma_uploaded &&
            upload_rows(candidate->chroma_plane, chroma, candidate->recorded_chroma_rows);
        uploaded = luma_uploaded && chroma_uploaded;
    } else {
        uploaded = upload_rows(
            candidate->texture,
            std::span<const core::u8>{
                frame->pixels.data(), frame->pixels.size()}, candidate->recorded_rows);
    }
    candidate->state = uploaded
        ? CandidateUploadState::RecordingComplete
        : CandidateUploadState::RecordingPartial;
    refresh_resident_stats();
    if (!uploaded) {
        return {};
    }

    ++stats_.uploads;
    if (p010) {
        ++stats_.planar_uploads;
    }
    return ResolvedMediaFrame{&candidate->texture,
        candidate->chroma_plane ? &candidate->chroma_plane : nullptr,
        candidate->layout};
}

} // namespace mediaserver::render
