#include <mediaserver/editor/model_surface_cache.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mediaserver::editor {

struct ModelSurfaceCache::State final {
    struct Entry final {
        // Identity: the pointer alone could be reused by a later asset.
        std::weak_ptr<const render::ModelAsset> asset;
        std::shared_ptr<const scene::MeshBvh> bvh;
        bool pending{};
        core::u64 used{};
    };

    explicit State(ModelSurfaceCacheConfig settings) : config(settings) {}

    ModelSurfaceCacheConfig config;
    mutable std::mutex mutex;
    std::condition_variable_any wake;
    std::condition_variable idle;
    std::unordered_map<const render::ModelAsset*, Entry> entries;
    std::deque<std::shared_ptr<const render::ModelAsset>> queue;
    // Hierarchies the UI thread let go of, freed on the worker.
    std::vector<std::shared_ptr<const scene::MeshBvh>> retired;
    bool building{};
    bool reset_requested{};
    core::u64 clock{};
    std::size_t bytes{};
    std::atomic<std::size_t> ready_count{};
    std::atomic<std::size_t> pending_count{};
    std::atomic<std::size_t> byte_count{};
    std::atomic<std::size_t> builds{};
    std::atomic<std::size_t> evictions{};
    // Started on the first queued build: most viewports never need one.
    std::jthread worker;

    void start_worker() {
        if (!worker.joinable()) {
            worker = std::jthread{[this](std::stop_token stop) { run(stop); }};
        }
    }

    void publish_counts() noexcept {
        std::size_t ready = 0U;
        std::size_t pending = 0U;
        for (const auto& [unused, entry] : entries) {
            static_cast<void>(unused);
            ready += entry.bvh != nullptr ? 1U : 0U;
            pending += entry.pending ? 1U : 0U;
        }
        ready_count.store(ready, std::memory_order_relaxed);
        pending_count.store(pending, std::memory_order_relaxed);
        byte_count.store(bytes, std::memory_order_relaxed);
    }

    void run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::shared_ptr<const render::ModelAsset> job;
            std::vector<std::shared_ptr<const scene::MeshBvh>> released;
            {
                std::unique_lock lock{mutex};
                wake.wait(lock, stop, [this] {
                    return !queue.empty() || reset_requested || !retired.empty();
                });
                if (stop.stop_requested()) {
                    return;
                }
                released.swap(retired);
                if (reset_requested) {
                    reset_requested = false;
                    for (auto& [unused, entry] : entries) {
                        static_cast<void>(unused);
                        if (entry.bvh != nullptr) {
                            released.push_back(std::move(entry.bvh));
                        }
                    }
                    entries.clear();
                    queue.clear();
                    bytes = 0U;
                    publish_counts();
                }
                if (!queue.empty()) {
                    job = std::move(queue.front());
                    queue.pop_front();
                    building = true;
                }
            }
            released.clear();
            if (job == nullptr) {
                idle.notify_all();
                continue;
            }

            auto built = std::make_shared<scene::MeshBvh>(
                scene::build_mesh_bvh(job->mesh, stop));
            builds.fetch_add(1U, std::memory_order_relaxed);
            {
                std::lock_guard lock{mutex};
                building = false;
                const auto found = entries.find(job.get());
                if (found != entries.end() && found->second.pending &&
                    found->second.asset.lock() == job) {
                    found->second.pending = false;
                    // One hierarchy larger than the whole budget is not kept,
                    // or it would be rebuilt on every look. That model stays
                    // on its bounds.
                    if (!built->empty() && built->bytes() <= config.max_bytes) {
                        bytes += built->bytes();
                        found->second.bvh = std::move(built);
                    }
                }
                // Over budget, the least recently used go first; the one just
                // built is the most recently asked for and goes last.
                while (bytes > config.max_bytes) {
                    auto oldest = entries.end();
                    for (auto at = entries.begin(); at != entries.end(); ++at) {
                        if (at->second.bvh != nullptr &&
                            (oldest == entries.end() ||
                             at->second.used < oldest->second.used)) {
                            oldest = at;
                        }
                    }
                    if (oldest == entries.end()) {
                        break;
                    }
                    bytes -= std::min(bytes, oldest->second.bvh->bytes());
                    released.push_back(std::move(oldest->second.bvh));
                    entries.erase(oldest);
                    evictions.fetch_add(1U, std::memory_order_relaxed);
                }
                // A model released by the model cache has no use for its
                // hierarchy.
                for (auto at = entries.begin(); at != entries.end();) {
                    if (!at->second.pending && at->second.asset.expired()) {
                        if (at->second.bvh != nullptr) {
                            bytes -= std::min(bytes, at->second.bvh->bytes());
                            released.push_back(std::move(at->second.bvh));
                        }
                        at = entries.erase(at);
                    } else {
                        ++at;
                    }
                }
                publish_counts();
            }
            // Free the asset reference and anything evicted off the lock; the
            // last reference to a decoded model may be this one.
            job.reset();
            released.clear();
            idle.notify_all();
        }
    }
};

ModelSurfaceCache::ModelSurfaceCache(ModelSurfaceCacheConfig config)
    : state_(std::make_unique<State>(config)) {}

ModelSurfaceCache::ModelSurfaceCache(ModelSurfaceCache&&) noexcept = default;
ModelSurfaceCache&
ModelSurfaceCache::operator=(ModelSurfaceCache&&) noexcept = default;

ModelSurfaceCache::~ModelSurfaceCache() {
    if (state_ != nullptr && state_->worker.joinable()) {
        state_->worker.request_stop();
        state_->worker.join();
    }
}

std::shared_ptr<const scene::MeshBvh> ModelSurfaceCache::observe(
    const std::shared_ptr<const render::ModelAsset>& asset) {
    if (state_ == nullptr || asset == nullptr) {
        return nullptr;
    }
    State& state = *state_;
    std::unique_lock lock{state.mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
        return nullptr;
    }
    const auto found = state.entries.find(asset.get());
    if (found != state.entries.end()) {
        if (found->second.asset.lock() == asset) {
            found->second.used = ++state.clock;
            return found->second.bvh;
        }
        // The address now belongs to a newer asset. The stale hierarchy is
        // released on the worker, not here.
        if (found->second.bvh != nullptr) {
            state.bytes -= std::min(state.bytes, found->second.bvh->bytes());
            state.retired.push_back(std::move(found->second.bvh));
        }
        state.entries.erase(found);
    }
    std::size_t pending = 0U;
    for (const auto& [unused, entry] : state.entries) {
        static_cast<void>(unused);
        pending += entry.pending ? 1U : 0U;
    }
    if (pending >= state.config.max_pending) {
        return nullptr;
    }
    state.entries.emplace(asset.get(),
                          State::Entry{asset, nullptr, true, ++state.clock});
    state.queue.push_back(asset);
    state.publish_counts();
    state.start_worker();
    lock.unlock();
    state.wake.notify_one();
    return nullptr;
}

void ModelSurfaceCache::request_reset() noexcept {
    if (state_ == nullptr) {
        return;
    }
    {
        std::lock_guard lock{state_->mutex};
        if (!state_->worker.joinable() && state_->entries.empty()) {
            return;
        }
        state_->reset_requested = true;
        state_->start_worker();
    }
    state_->wake.notify_one();
}

ModelSurfaceCacheStats ModelSurfaceCache::stats() const noexcept {
    if (state_ == nullptr) {
        return {};
    }
    return ModelSurfaceCacheStats{
        state_->ready_count.load(std::memory_order_relaxed),
        state_->pending_count.load(std::memory_order_relaxed),
        state_->byte_count.load(std::memory_order_relaxed),
        state_->builds.load(std::memory_order_relaxed),
        state_->evictions.load(std::memory_order_relaxed)};
}

namespace detail {

bool wait_for_model_surface_cache_for_test(ModelSurfaceCache& cache) noexcept {
    if (cache.state_ == nullptr) {
        return true;
    }
    ModelSurfaceCache::State& state = *cache.state_;
    std::unique_lock lock{state.mutex};
    state.idle.wait(lock, [&state] {
        return state.queue.empty() && !state.building &&
               !state.reset_requested && state.retired.empty();
    });
    return true;
}

} // namespace detail

} // namespace mediaserver::editor
