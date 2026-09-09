#include <mediaserver/render/model_cache.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace mediaserver::render {
namespace {

class FileModelAssetLoader final : public ModelAssetLoader {
public:
    [[nodiscard]] bool load(
        const std::string& source,
        ModelAsset& into) override {
        return load_model(source, into);
    }
};

[[nodiscard]] bool add_bytes(
    std::size_t& total,
    std::size_t count,
    std::size_t element) noexcept {
    if (element != 0U && count > std::numeric_limits<std::size_t>::max()
            / element) {
        return false;
    }
    const std::size_t bytes = count * element;
    if (total > std::numeric_limits<std::size_t>::max() - bytes) {
        return false;
    }
    total += bytes;
    return true;
}

[[nodiscard]] std::optional<std::size_t> decoded_bytes(
    const ModelAsset& asset) noexcept {
    std::size_t bytes{};
    if (!add_bytes(
            bytes, asset.mesh.vertices.size(), sizeof(scene::MeshVertex))
        || !add_bytes(
            bytes, asset.mesh.indices.size(), sizeof(core::u32))
        || !add_bytes(
            bytes, asset.vertex_metadata.size(),
            sizeof(ModelVertexMetadata))
        || !add_bytes(bytes, asset.lods.size(), sizeof(ModelLod))
        || !add_bytes(bytes, asset.parts.size(), sizeof(ModelPart))
        || !add_bytes(bytes, asset.textures.size(), sizeof(ModelTexture))) {
        return std::nullopt;
    }
    for (const ModelTexture& texture : asset.textures) {
        if (!add_bytes(bytes, texture.rgba.size(), sizeof(core::u8)) ||
            !add_bytes(bytes, texture.lower_mips.size(), sizeof(ModelTextureMip))) {
            return std::nullopt;
        }
        for (const auto& mip : texture.lower_mips) {
            if (!add_bytes(bytes, mip.rgba.size(), sizeof(core::u8)))
                return std::nullopt;
        }
    }
    if (!add_bytes(bytes,asset.animation_nodes.size(),sizeof(ModelAnimationNode))
        || !add_bytes(bytes,asset.pose_bindings.size(),sizeof(ModelPoseBinding))
        || !add_bytes(bytes,asset.animations.size(),sizeof(ModelAnimationClip))) return std::nullopt;
    for (const auto& clip:asset.animations) {
        if (!add_bytes(bytes,clip.name.size(),1) || !add_bytes(bytes,clip.channels.size(),sizeof(ModelAnimationChannel))) return std::nullopt;
        for (const auto& channel:clip.channels)
            if (!add_bytes(bytes,channel.times.size(),sizeof(float)) || !add_bytes(bytes,channel.values.size(),sizeof(scene::Vec4))) return std::nullopt;
    }
    return bytes;
}

} // namespace

struct ModelCache::State final
    : std::enable_shared_from_this<ModelCache::State> {
    struct Request final {
        std::string source;
        core::u64 generation{};
        core::u64 ticket{};
    };

    struct Entry final {
        std::shared_ptr<const ModelAsset> asset;
        std::size_t bytes{};
        core::u64 generation{1U};
        core::u64 authored_generation{};
        core::u64 asset_generation{};
        core::u64 asset_publication_epoch{};
        core::u64 asset_retirement_epoch{};
        // Latest visible state transition (refresh or completed answer).
        core::u64 publication_epoch{};
        // A completed answer may still be hidden behind a recording fence.
        // Retain the earlier refresh transition so that answer cannot make
        // the previous generation appear current again in the meantime.
        core::u64 refresh_epoch{};
        core::u64 pending_ticket{};
        core::u64 stamp{};
        ModelCacheState state{ModelCacheState::Unseen};
    };

    struct RetiredAsset final {
        // Points at the stable std::map key for this State. Safe reset erases
        // retired records before source entries.
        const std::string* source{};
        std::shared_ptr<const ModelAsset> asset;
        std::size_t bytes{};
        core::u64 generation{};
        core::u64 publication_epoch{};
        core::u64 retirement_epoch{};
    };

    // One process executor is shared even by injected test caches. A blocked
    // file/codec operation therefore cannot cause renderer teardown to join;
    // work holds only weak cache state and retains its loader solely while the
    // worker is actually inside load().
    struct Executor final {
        struct PendingKey final {
            core::u64 owner{};
            std::string source;

            [[nodiscard]] bool operator<(
                const PendingKey& other) const noexcept {
                if (owner != other.owner) {
                    return owner < other.owner;
                }
                return source < other.source;
            }
        };

        struct Work final {
            std::weak_ptr<State> completion;
            core::u64 owner{};
            Request request;
            bool reset{};
        };

        static constexpr std::size_t maximum_global_pending = 256U;

        [[nodiscard]] static Executor& instance() {
            static Executor executor;
            return executor;
        }

        Executor(const Executor&) = delete;
        Executor& operator=(const Executor&) = delete;

        [[nodiscard]] core::u64 next_owner() noexcept {
            core::u64 owner =
                next_owner_.fetch_add(1U, std::memory_order_relaxed);
            if (owner == 0U) {
                owner = next_owner_.fetch_add(1U, std::memory_order_relaxed);
            }
            return owner;
        }

        [[nodiscard]] core::u64 enqueue(
            std::weak_ptr<State> completion,
            core::u64 owner,
            std::size_t maximum_owner_pending,
            std::string source,
            core::u64 generation) noexcept {
            try {
                std::unique_lock lock{mutex_, std::try_to_lock};
                if (!lock.owns_lock()) {
                    return 0U;
                }
                const PendingKey key{owner, source};
                if (pending_.find(key) != pending_.end()) {
                    return 0U;
                }
                const std::size_t owner_pending = static_cast<std::size_t>(
                    std::count_if(
                        pending_.begin(), pending_.end(),
                        [owner](const auto& held) noexcept {
                            return held.first.owner == owner;
                        }));
                if (owner_pending >= maximum_owner_pending
                    || pending_.size() >= maximum_global_pending) {
                    return 0U;
                }

                core::u64 ticket = next_ticket_++;
                if (ticket == 0U) {
                    ticket = next_ticket_++;
                }
                const auto [held, inserted] =
                    pending_.try_emplace(key, ticket);
                if (!inserted) {
                    return 0U;
                }
                try {
                    queue_.push_back(Work{
                        std::move(completion), owner,
                        Request{std::move(source), generation, ticket},
                        false});
                } catch (...) {
                    pending_.erase(held);
                    throw;
                }
                changed_.notify_one();
                return ticket;
            } catch (...) {
                return 0U;
            }
        }

        [[nodiscard]] core::u64 enqueue_reset(
            std::weak_ptr<State> completion,
            core::u64 owner) noexcept {
            try {
                std::unique_lock lock{mutex_, std::try_to_lock};
                if (!lock.owns_lock()
                    || pending_.size() >= maximum_global_pending) {
                    return 0U;
                }
                const PendingKey key{owner, std::string{}};
                if (pending_.find(key) != pending_.end()) {
                    return 0U;
                }
                core::u64 ticket = next_ticket_++;
                if (ticket == 0U) {
                    ticket = next_ticket_++;
                }
                const auto [held, inserted] =
                    pending_.try_emplace(key, ticket);
                if (!inserted) {
                    return 0U;
                }
                try {
                    queue_.push_back(Work{
                        std::move(completion), owner,
                        Request{std::string{}, 0U, ticket}, true});
                } catch (...) {
                    pending_.erase(held);
                    throw;
                }
                changed_.notify_one();
                return ticket;
            } catch (...) {
                return 0U;
            }
        }

        // Operator path only. The executing call cannot be cancelled, but its
        // pending identity is retired and the State generation fence rejects
        // its result. No source/loader object is joined here.
        void invalidate(core::u64 owner, const std::string& source) noexcept {
            try {
                const std::lock_guard lock{mutex_};
                for (auto work = queue_.begin(); work != queue_.end();) {
                    if (work->owner == owner
                        && work->request.source == source) {
                        work = queue_.erase(work);
                    } else {
                        ++work;
                    }
                }
                pending_.erase(PendingKey{owner, source});
                changed_.notify_all();
            } catch (...) {
                // Source generation remains authoritative if opportunistic
                // queue removal cannot allocate its temporary key.
            }
        }

        [[nodiscard]] bool wait_idle(core::u64 owner) noexcept {
            try {
                std::unique_lock lock{mutex_};
                return changed_.wait_for(
                    lock, std::chrono::seconds{5}, [&] {
                        return std::none_of(
                            pending_.begin(), pending_.end(),
                            [owner](const auto& held) noexcept {
                                return held.first.owner == owner;
                            });
                    });
            } catch (...) {
                return false;
            }
        }

      private:
        Executor()
            : worker_([this](std::stop_token stop) { run(stop); }) {}

        ~Executor() {
            // Only process teardown may wait for an OS read/codec already in
            // flight. ModelCache and SceneRenderer teardown never owns this.
            worker_.request_stop();
            changed_.notify_all();
        }

        void complete(const Work& work) noexcept {
            try {
                const std::lock_guard lock{mutex_};
                const PendingKey key{work.owner, work.request.source};
                const auto pending = pending_.find(key);
                if (pending != pending_.end()
                    && pending->second == work.request.ticket) {
                    pending_.erase(pending);
                }
                changed_.notify_all();
            } catch (...) {
            }
        }

        void cancel_owner_queue_for_reset(const Work& reset) noexcept {
            try {
                const std::lock_guard lock{mutex_};
                for (auto work = queue_.begin(); work != queue_.end();) {
                    if (work->owner == reset.owner) {
                        pending_.erase(PendingKey{
                            work->owner, work->request.source});
                        work = queue_.erase(work);
                    } else {
                        ++work;
                    }
                }
                for (auto pending = pending_.begin();
                     pending != pending_.end();) {
                    if (pending->first.owner == reset.owner
                        && !(pending->first.source.empty()
                            && pending->second == reset.request.ticket)) {
                        pending = pending_.erase(pending);
                    } else {
                        ++pending;
                    }
                }
                changed_.notify_all();
            } catch (...) {
            }
        }

        void run(std::stop_token stop) noexcept {
            while (!stop.stop_requested()) {
                Work work{};
                {
                    std::unique_lock lock{mutex_};
                    changed_.wait(lock, [&] {
                        return stop.stop_requested() || !queue_.empty();
                    });
                    if (stop.stop_requested()) {
                        return;
                    }
                    work = std::move(queue_.front());
                    queue_.pop_front();
                }

                if (work.reset) {
                    cancel_owner_queue_for_reset(work);
                    if (auto state = work.completion.lock()) {
                        state->reset_on_worker();
                    }
                    complete(work);
                    continue;
                }

                std::shared_ptr<ModelAssetLoader> loader;
                if (auto state = work.completion.lock()) {
                    loader = state->loader_;
                }
                std::shared_ptr<const ModelAsset> asset;
                bool loaded{};
                if (loader != nullptr) {
                    try {
                        ModelAsset decoded{};
                        loaded = loader->load(work.request.source, decoded);
                        if (loaded) loaded = prepare_model_mips(decoded);
                        if (loaded) {
                            asset = std::make_shared<const ModelAsset>(
                                std::move(decoded));
                        }
                    } catch (...) {
                        asset.reset();
                        loaded = false;
                    }
                }
                // Drop the potentially cache-owned loader before publication;
                // a cache destroyed during load() does not wait here.
                loader.reset();
                if (auto state = work.completion.lock()) {
                    state->publish_completed(
                        work.request, loaded, std::move(asset));
                }
                complete(work);
            }
        }

        std::atomic<core::u64> next_owner_{1U};
        core::u64 next_ticket_{1U};
        std::mutex mutex_;
        std::condition_variable changed_;
        std::deque<Work> queue_;
        std::map<PendingKey, core::u64> pending_;
        std::jthread worker_;
    };

    State(
        std::shared_ptr<ModelAssetLoader> loader,
        ModelCacheConfig config)
        : loader_(std::move(loader)), config_(config),
          executor_(&Executor::instance()), owner_(executor_->next_owner()) {
        config_.max_pending = std::max(config_.max_pending, std::size_t{1U});
        config_.max_sources = std::max(config_.max_sources, std::size_t{1U});
        config_.max_ready_assets =
            std::max(config_.max_ready_assets, std::size_t{1U});
        config_.max_ready_bytes =
            std::max(config_.max_ready_bytes, std::size_t{1U});
        retired_.reserve(config_.max_ready_assets);
    }

    [[nodiscard]] core::u64 next_stamp() noexcept {
        if (stamp_ == std::numeric_limits<core::u64>::max()) {
            for (auto& [source, entry] : entries_) {
                static_cast<void>(source);
                entry.stamp >>= 1U;
            }
            stamp_ >>= 1U;
        }
        return ++stamp_;
    }

    [[nodiscard]] bool room_for_source() {
        // observe() is the render-facing caller. Erasing a ready entry here
        // could run the last shared_ptr<ModelAsset> destructor and free a very
        // large decoded mesh on the frame thread. Decoded eviction is worker-
        // publication work only; a full source table simply drops admission.
        return entries_.size() < config_.max_sources;
    }

    [[nodiscard]] core::u64 reserve_publication_epoch() noexcept {
        // Protected by mutex_. Saturating is preferable to ever reusing zero
        // or wrapping an identity; reaching this after 2^64 publications is
        // outside the process lifetime of any practical show.
        if (reserved_publication_epoch_
            != std::numeric_limits<core::u64>::max()) {
            ++reserved_publication_epoch_;
        }
        return reserved_publication_epoch_;
    }

    void commit_publication_epoch(core::u64 epoch) noexcept {
        // A refresh/control thread may reserve a later epoch after this
        // mutation unlocks and commit first. CAS-max prevents regression; the
        // common State mutex means that later reservation also acquired every
        // preceding entry mutation before publishing its release.
        core::u64 visible = publication_epoch_.load(
            std::memory_order_relaxed);
        while (visible < epoch
               && !publication_epoch_.compare_exchange_weak(
                   visible, epoch, std::memory_order_release,
                   std::memory_order_relaxed)) {
        }
    }

    [[nodiscard]] bool room_for_ready(
        const std::string& source,
        std::size_t bytes,
        Entry& replacing,
        core::u64 publication_epoch) noexcept {
        if (bytes > config_.max_ready_bytes) {
            return false;
        }
        // Every newly published generation remains resident until an explicit
        // safe reset. This is bounded CPU MVCC: a command that latched an older
        // epoch can still obtain its exact generation even if refresh finishes
        // before a later mapped view observes it. Distinct Ready sources are
        // never automatically evicted behind a recording's fence.
        if (ready_assets_ >= config_.max_ready_assets
            || ready_bytes_ > config_.max_ready_bytes - bytes) {
            return false;
        }
        if (replacing.asset != nullptr) {
            if (retired_.size() >= config_.max_ready_assets) {
                return false;
            }
            retired_.push_back(RetiredAsset{
                &source,
                std::move(replacing.asset),
                replacing.bytes,
                replacing.asset_generation,
                replacing.asset_publication_epoch,
                replacing.asset_retirement_epoch != 0U
                    ? replacing.asset_retirement_epoch
                    : publication_epoch});
            retired_bytes_ += replacing.bytes;
            replacing.bytes = 0U;
            replacing.asset_generation = 0U;
            replacing.asset_publication_epoch = 0U;
            replacing.asset_retirement_epoch = 0U;
        }
        publish_sizes();
        return true;
    }

    void publish_sizes() noexcept {
        sources_snapshot_.store(entries_.size(), std::memory_order_relaxed);
        ready_assets_snapshot_.store(
            ready_assets_, std::memory_order_relaxed);
        ready_bytes_snapshot_.store(ready_bytes_, std::memory_order_relaxed);
        retired_assets_snapshot_.store(
            retired_.size(), std::memory_order_relaxed);
        retired_bytes_snapshot_.store(
            retired_bytes_, std::memory_order_relaxed);
    }

    void publish_completed(
        const Request& request,
        bool loaded,
        std::shared_ptr<const ModelAsset> asset) noexcept {
        core::u64 committed_epoch{};
        std::shared_ptr<const std::function<void()>> publication_gate;
        try {
            {
                const std::lock_guard lock{mutex_};
                const auto found = entries_.find(request.source);
                if (found == entries_.end()
                    || found->second.generation != request.generation
                    || found->second.pending_ticket != request.ticket) {
                    ++late_results_discarded_;
                    return;
                }
                Entry& entry = found->second;
                entry.pending_ticket = 0U;
                pending_snapshot_.fetch_sub(1U, std::memory_order_relaxed);
                ++completions_;
                committed_epoch = reserve_publication_epoch();

                const auto bytes = asset == nullptr
                    ? std::optional<std::size_t>{}
                    : decoded_bytes(*asset);
                if (loaded && asset != nullptr && bytes.has_value()
                    && room_for_ready(
                        found->first, *bytes, entry, committed_epoch)) {
                    ++ready_assets_;
                    entry.asset = std::move(asset);
                    entry.bytes = *bytes;
                    entry.asset_generation = entry.generation;
                    entry.asset_publication_epoch = committed_epoch;
                    entry.asset_retirement_epoch = 0U;
                    entry.publication_epoch = committed_epoch;
                    entry.state = ModelCacheState::Ready;
                    entry.stamp = next_stamp();
                    ready_bytes_ += *bytes;
                    publish_sizes();
                } else {
                    // Invalid, oversized, or uncacheable results are one
                    // completed sticky answer. A retained older asset remains
                    // available only to renderers that already integrated it;
                    // no implicit retry occurs.
                    entry.state = ModelCacheState::Failed;
                    entry.publication_epoch = committed_epoch;
                    entry.stamp = next_stamp();
                    ++failures_;
                    failed_sources_snapshot_.fetch_add(
                        1U, std::memory_order_relaxed);
                    publish_sizes();
                }
                publication_gate = publication_gate_for_test_;
            }

            // Crucially, visible epoch advances only after the entry mutex is
            // released. A command that latches during mutation gets the old
            // fence and cannot reveal this answer in a later mapped view.
            if (publication_gate != nullptr) {
                try {
                    (*publication_gate)();
                } catch (...) {
                }
            }
            commit_publication_epoch(committed_epoch);
        } catch (...) {
            // A publication allocation failure is sticky as far as the
            // executor queue is concerned. The entry remains Pending only in
            // this exceptional bookkeeping case; render still never waits.
        }
    }

    [[nodiscard]] ModelCacheObservation observe(
        const std::string& source,
        core::u64 publication_fence, core::u64 authored_generation) noexcept {
        if (source.empty() || publication_fence == 0U) {
            return ModelCacheObservation{};
        }
        try {
            std::unique_lock lock{mutex_, std::try_to_lock};
            if (!lock.owns_lock()) {
                return ModelCacheObservation{
                    nullptr, ModelCacheState::Busy, 0U, 0U, 0U, false};
            }

            auto found = entries_.find(source);
            if (found == entries_.end()) {
                if (!room_for_source()) {
                    ++admission_drops_;
                    return ModelCacheObservation{};
                }
                found = entries_.try_emplace(source, Entry{}).first;
                publish_sizes();
            }
            Entry& entry = found->second;
            if (authored_generation != 0 && entry.authored_generation != authored_generation) {
                if(entry.state == ModelCacheState::Failed) failed_sources_snapshot_.fetch_sub(1U, std::memory_order_relaxed);
                if(entry.pending_ticket != 0) pending_snapshot_.fetch_sub(1U, std::memory_order_relaxed);
                entry.authored_generation = authored_generation;
                ++entry.generation;
                entry.pending_ticket = 0;
                entry.state = ModelCacheState::Unseen;
                entry.publication_epoch = reserve_publication_epoch();
                entry.refresh_epoch = entry.publication_epoch;
                commit_publication_epoch(entry.publication_epoch);
            }
            entry.stamp = next_stamp();

            if (entry.state == ModelCacheState::Unseen
                && entry.pending_ticket == 0U) {
                const core::u64 ticket = executor_->enqueue(
                    weak_from_this(), owner_, config_.max_pending,
                    source, entry.generation);
                if (ticket != 0U) {
                    entry.pending_ticket = ticket;
                    entry.state = ModelCacheState::Pending;
                    pending_snapshot_.fetch_add(1U, std::memory_order_relaxed);
                    ++admissions_;
                } else {
                    ++admission_drops_;
                }
            }

            std::shared_ptr<const ModelAsset> snapshot;
            core::u64 snapshot_generation{};
            core::u64 snapshot_epoch{};
            const auto consider = [&](const auto& candidate,
                                      core::u64 generation,
                                      core::u64 published,
                                      core::u64 retired) {
                if (candidate != nullptr && published != 0U
                    && published <= publication_fence
                    && (retired == 0U || publication_fence < retired)
                    && published >= snapshot_epoch) {
                    snapshot = candidate;
                    snapshot_generation = generation;
                    snapshot_epoch = published;
                }
            };
            consider(
                entry.asset, entry.asset_generation,
                entry.asset_publication_epoch,
                entry.asset_retirement_epoch);
            for (const RetiredAsset& retired : retired_) {
                if (retired.source != nullptr
                    && *retired.source == source) {
                    consider(
                        retired.asset, retired.generation,
                        retired.publication_epoch,
                        retired.retirement_epoch);
                }
            }
            if (snapshot != nullptr) {
                ++cache_hits_;
                const bool transition_visible =
                    entry.publication_epoch <= publication_fence ||
                    (entry.refresh_epoch != 0U && entry.refresh_epoch <= publication_fence);
                const bool stale = transition_visible
                    && snapshot_generation != entry.generation;
                const auto visible_state = entry.publication_epoch <= publication_fence
                    ? entry.state : ModelCacheState::Pending;
                return ModelCacheObservation{
                    std::move(snapshot),
                    stale ? visible_state : ModelCacheState::Ready,
                    snapshot_generation, entry.generation,
                    snapshot_epoch, stale};
            }
            // A completed generation newer than this command recording is
            // intentionally indistinguishable from pending until the next
            // recording latches a new epoch.
            const ModelCacheState state = entry.publication_epoch
                    > publication_fence
                ? ModelCacheState::Pending
                : entry.state;
            return ModelCacheObservation{
                nullptr, state, entry.generation,
                entry.generation, entry.publication_epoch,
                entry.asset != nullptr
                    && entry.asset_generation != entry.generation};
        } catch (...) {
            ++admission_drops_;
            return ModelCacheObservation{};
        }
    }

    [[nodiscard]] bool refresh(const std::string& source) noexcept {
        if (source.empty()) {
            return false;
        }
        core::u64 committed_epoch{};
        std::shared_ptr<const std::function<void()>> publication_gate;
        try {
            {
                const std::lock_guard lock{mutex_};
                const auto found = entries_.find(source);
                if (found == entries_.end()) {
                    return false;
                }
                Entry& entry = found->second;
                if (entry.state == ModelCacheState::Failed) {
                    failed_sources_snapshot_.fetch_sub(
                        1U, std::memory_order_relaxed);
                }
                if (entry.pending_ticket != 0U) {
                    pending_snapshot_.fetch_sub(
                        1U, std::memory_order_relaxed);
                }
                committed_epoch = reserve_publication_epoch();
                ++entry.generation;
                if (entry.generation == 0U) {
                    ++entry.generation;
                }
                entry.pending_ticket = 0U;
                entry.state = ModelCacheState::Unseen;
                entry.publication_epoch = committed_epoch;
                entry.refresh_epoch = committed_epoch;
                entry.stamp = next_stamp();
                executor_->invalidate(owner_, source);
                publication_gate = publication_gate_for_test_;
            }
            if (publication_gate != nullptr) {
                try {
                    (*publication_gate)();
                } catch (...) {
                }
            }
            commit_publication_epoch(committed_epoch);
            return true;
        } catch (...) {
            return false;
        }
    }

    void reset_on_worker() noexcept {
        core::u64 committed_epoch{};
        std::shared_ptr<const std::function<void()>> publication_gate;
        try {
            {
                const std::lock_guard lock{mutex_};
                committed_epoch = reserve_publication_epoch();

                // Retired records go first because they borrow stable source
                // key addresses from entries_. Only worker-owned final refs
                // are reclaimed; an external renderer/observation keeps both
                // its generation and key record alive.
                for (auto retired = retired_.begin();
                     retired != retired_.end();) {
                    if (retired->asset.use_count() == 1) {
                        retired = retired_.erase(retired);
                        ++evictions_;
                    } else {
                        ++retired;
                    }
                }
                for (auto entry = entries_.begin();
                     entry != entries_.end();) {
                    const bool retained_version = std::any_of(
                        retired_.begin(), retired_.end(),
                        [&](const RetiredAsset& retired) noexcept {
                            return retired.source == &entry->first;
                        });
                    const bool reclaimable_asset =
                        entry->second.asset == nullptr
                        || entry->second.asset.use_count() == 1;
                    if (!retained_version && reclaimable_asset) {
                        if (entry->second.asset != nullptr) {
                            ++evictions_;
                        }
                        entry = entries_.erase(entry);
                        continue;
                    }
                    entry->second.pending_ticket = 0U;
                    if (entry->second.state == ModelCacheState::Pending
                        || entry->second.state == ModelCacheState::Unseen) {
                        entry->second.state = entry->second.asset != nullptr
                                && entry->second.asset_generation
                                    == entry->second.generation
                            ? ModelCacheState::Ready
                            : ModelCacheState::Failed;
                        entry->second.publication_epoch = committed_epoch;
                    }
                    ++entry;
                }

                ready_assets_ = retired_.size();
                ready_bytes_ = 0U;
                retired_bytes_ = 0U;
                for (const RetiredAsset& retired : retired_) {
                    ready_bytes_ += retired.bytes;
                    retired_bytes_ += retired.bytes;
                }
                std::size_t failed_sources{};
                for (const auto& [source, entry] : entries_) {
                    static_cast<void>(source);
                    if (entry.asset != nullptr) {
                        ++ready_assets_;
                        ready_bytes_ += entry.bytes;
                    }
                    if (entry.state == ModelCacheState::Failed) {
                        ++failed_sources;
                    }
                }
                pending_snapshot_.store(0U, std::memory_order_relaxed);
                failed_sources_snapshot_.store(
                    failed_sources, std::memory_order_relaxed);
                publish_sizes();
                publication_gate = publication_gate_for_test_;
            }
            if (publication_gate != nullptr) {
                try {
                    (*publication_gate)();
                } catch (...) {
                }
            }
            commit_publication_epoch(committed_epoch);
        } catch (...) {
        }
    }

    [[nodiscard]] bool request_safe_reset() noexcept {
        return executor_->enqueue_reset(weak_from_this(), owner_) != 0U;
    }

    std::shared_ptr<ModelAssetLoader> loader_;
    ModelCacheConfig config_{};
    Executor* executor_{};
    core::u64 owner_{};
    std::mutex mutex_;
    std::map<std::string, Entry> entries_;
    std::vector<RetiredAsset> retired_;
    std::size_t ready_assets_{};
    std::size_t ready_bytes_{};
    std::size_t retired_bytes_{};
    core::u64 stamp_{};
    core::u64 reserved_publication_epoch_{1U};
    std::shared_ptr<const std::function<void()>> publication_gate_for_test_;

    std::atomic<core::u64> publication_epoch_{1U};
    std::atomic<std::size_t> sources_snapshot_{};
    std::atomic<std::size_t> pending_snapshot_{};
    std::atomic<std::size_t> ready_assets_snapshot_{};
    std::atomic<std::size_t> ready_bytes_snapshot_{};
    std::atomic<std::size_t> retired_assets_snapshot_{};
    std::atomic<std::size_t> retired_bytes_snapshot_{};
    std::atomic<std::size_t> failed_sources_snapshot_{};
    std::atomic<core::u64> admissions_{};
    std::atomic<core::u64> admission_drops_{};
    std::atomic<core::u64> cache_hits_{};
    std::atomic<core::u64> completions_{};
    std::atomic<core::u64> failures_{};
    std::atomic<core::u64> evictions_{};
    std::atomic<core::u64> late_results_discarded_{};
};

ModelCache::ModelCache() {
    static const std::shared_ptr<State> shared = [] {
        return std::make_shared<State>(
            std::make_shared<FileModelAssetLoader>(), ModelCacheConfig{});
    }();
    state_ = shared;
}

ModelCache::ModelCache(
    std::unique_ptr<ModelAssetLoader> loader,
    ModelCacheConfig config) {
    std::shared_ptr<ModelAssetLoader> shared_loader = loader
        ? std::shared_ptr<ModelAssetLoader>{std::move(loader)}
        : std::make_shared<FileModelAssetLoader>();
    state_ = std::make_shared<State>(std::move(shared_loader), config);
}

ModelCacheObservation ModelCache::observe(
    const std::string& source,
    core::u64 publication_fence, core::u64 authored_generation) noexcept {
    return state_ == nullptr
        ? ModelCacheObservation{}
        : state_->observe(source, publication_fence, authored_generation);
}

bool ModelCache::refresh(const std::string& source) noexcept {
    return state_ != nullptr && state_->refresh(source);
}

bool ModelCache::request_safe_reset() noexcept {
    return state_ != nullptr && state_->request_safe_reset();
}

core::u64 ModelCache::publication_epoch() const noexcept {
    return state_ == nullptr
        ? 0U
        : state_->publication_epoch_.load(std::memory_order_acquire);
}

ModelCacheStats ModelCache::stats() const noexcept {
    if (state_ == nullptr) {
        return ModelCacheStats{};
    }
    ModelCacheStats result{};
    result.sources =
        state_->sources_snapshot_.load(std::memory_order_relaxed);
    result.pending =
        state_->pending_snapshot_.load(std::memory_order_relaxed);
    result.ready_assets =
        state_->ready_assets_snapshot_.load(std::memory_order_relaxed);
    result.ready_bytes =
        state_->ready_bytes_snapshot_.load(std::memory_order_relaxed);
    result.retired_assets =
        state_->retired_assets_snapshot_.load(std::memory_order_relaxed);
    result.retired_bytes =
        state_->retired_bytes_snapshot_.load(std::memory_order_relaxed);
    result.failed_sources =
        state_->failed_sources_snapshot_.load(std::memory_order_relaxed);
    result.source_capacity = state_->config_.max_sources;
    result.pending_capacity = state_->config_.max_pending;
    result.ready_asset_capacity = state_->config_.max_ready_assets;
    result.ready_byte_capacity = state_->config_.max_ready_bytes;
    result.admissions = state_->admissions_.load(std::memory_order_relaxed);
    result.admission_drops =
        state_->admission_drops_.load(std::memory_order_relaxed);
    result.cache_hits = state_->cache_hits_.load(std::memory_order_relaxed);
    result.completions =
        state_->completions_.load(std::memory_order_relaxed);
    result.failures = state_->failures_.load(std::memory_order_relaxed);
    result.evictions = state_->evictions_.load(std::memory_order_relaxed);
    result.late_results_discarded =
        state_->late_results_discarded_.load(std::memory_order_relaxed);
    return result;
}

void detail::hold_model_cache_lock_for_test(
    ModelCache& cache,
    const std::function<void()>& while_locked) {
    if (cache.state_ == nullptr) {
        while_locked();
        return;
    }
    const std::lock_guard lock{cache.state_->mutex_};
    while_locked();
}

bool detail::wait_for_model_cache_for_test(ModelCache& cache) noexcept {
    return cache.state_ != nullptr
        && cache.state_->executor_->wait_idle(cache.state_->owner_);
}

bool detail::wait_for_shared_model_cache_for_test() noexcept {
    ModelCache shared;
    return wait_for_model_cache_for_test(shared);
}

void detail::set_model_cache_publication_gate_for_test(
    ModelCache& cache,
    const std::function<void()>& while_uncommitted) {
    if (cache.state_ == nullptr) {
        return;
    }
    auto gate = std::make_shared<const std::function<void()>>(
        while_uncommitted);
    const std::lock_guard lock{cache.state_->mutex_};
    cache.state_->publication_gate_for_test_ = std::move(gate);
}

} // namespace mediaserver::render
