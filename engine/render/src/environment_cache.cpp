#include <mediaserver/render/environment_cache.hpp>

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace mediaserver::render {
namespace {

// An environment map far larger than this is a mistake rather than a
// requirement: the prefilter resamples into a bounded cube anyway, so the only
// thing extra source resolution buys is decode time and resident bytes.
constexpr std::size_t max_environment_bytes = 512U * 1024U * 1024U;

[[nodiscard]] std::vector<core::u8> read_file(const std::string& path) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file) {
        return {};
    }
    const std::streamoff size = file.tellg();
    if (size <= 0
        || static_cast<std::uintmax_t>(size) > max_environment_bytes) {
        return {};
    }
    file.seekg(0, std::ios::beg);
    std::vector<core::u8> bytes;
    try {
        bytes.resize(static_cast<std::size_t>(size));
    } catch (const std::bad_alloc&) {
        return {};
    }
    if (!file.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()))) {
        return {};
    }
    return bytes;
}

} // namespace

struct EnvironmentCache::State final {
    // One request in flight at a time. A scene has one environment, so a queue
    // would only ever hold a request the next one supersedes.
    struct Request final {
        std::string source;
        core::u64 generation{};
    };

    mutable std::mutex mutex;
    std::condition_variable idle;

    std::string source;
    core::u64 generation{1U};
    core::u64 authored_generation{};
    // The generation whose result is currently resident, which lags
    // `generation` while a refresh is in flight.
    core::u64 ready_generation{};
    EnvironmentCacheState state{EnvironmentCacheState::Unseen};
    std::shared_ptr<const DecodedFrame> frame;
    bool working{};
    bool has_request{};
    Request request;

    std::atomic_uint64_t publication_epoch{1U};
    std::atomic_uint64_t admissions{};
    std::atomic_uint64_t completions{};
    std::atomic_uint64_t failures{};
    std::atomic_uint64_t cache_hits{};
    std::atomic_uint64_t busy_observations{};
    std::atomic_uint64_t late_results_discarded{};

    std::jthread worker;

    State() : worker([this](std::stop_token stop) { run(stop); }) {}

    ~State() {
        worker.request_stop();
        {
            const std::lock_guard lock{mutex};
            has_request = true;
        }
        idle.notify_all();
    }

    void run(std::stop_token stop) {
        for (;;) {
            Request work{};
            {
                std::unique_lock lock{mutex};
                idle.wait(lock, [this, &stop] {
                    return has_request || stop.stop_requested();
                });
                if (stop.stop_requested() && !has_request) {
                    return;
                }
                if (!has_request) {
                    continue;
                }
                has_request = false;
                work = request;
                working = true;
            }

            // Outside the lock. A blocked read must never stall an observe.
            const std::vector<core::u8> bytes = read_file(work.source);
            DecodedFrame decoded =
                bytes.empty() ? DecodedFrame{}
                              : decode_radiance_bytes(bytes);

            {
                const std::lock_guard lock{mutex};
                working = false;
                if (work.generation != generation) {
                    // A refresh moved on while this was decoding. Adopting it
                    // would replace the map the operator just asked for with
                    // the one they replaced.
                    late_results_discarded.fetch_add(
                        1U, std::memory_order_relaxed);
                } else if (decoded.valid()) {
                    frame = std::make_shared<const DecodedFrame>(
                        std::move(decoded));
                    ready_generation = work.generation;
                    state = EnvironmentCacheState::Ready;
                    completions.fetch_add(1U, std::memory_order_relaxed);
                    publication_epoch.fetch_add(
                        1U, std::memory_order_release);
                } else {
                    // Sticky. The previous complete map is deliberately kept:
                    // a scene that had an environment and then got a bad path
                    // should keep lighting rather than go black while an
                    // operator reads the diagnostic.
                    state = EnvironmentCacheState::Failed;
                    failures.fetch_add(1U, std::memory_order_relaxed);
                }
            }
            idle.notify_all();
        }
    }
};

EnvironmentCache::EnvironmentCache() {
    // Process-shared, so every renderer on this machine lights from the same
    // decoded bytes rather than each holding its own copy of the same file.
    static const std::shared_ptr<State> shared = std::make_shared<State>();
    state_ = shared;
}

EnvironmentCacheObservation EnvironmentCache::observe(
    const std::string& source, core::u64 publication_fence, core::u64 authored_generation) noexcept {
    EnvironmentCacheObservation result{};
    if (state_ == nullptr) {
        return result;
    }
    if (source.empty()) {
        // Not an error, and it must not read as one: a scene with no authored
        // environment is the ordinary case, and the renderer falls back to its
        // hemisphere without complaint.
        return result;
    }

    std::unique_lock lock{state_->mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
        state_->busy_observations.fetch_add(1U, std::memory_order_relaxed);
        result.state = EnvironmentCacheState::Busy;
        return result;
    }

    if (state_->source != source || (authored_generation != 0 && state_->authored_generation != authored_generation)) {
        state_->authored_generation = authored_generation;
        // A different map than the one resident. Admit it and report Pending;
        // the old frame is not offered, because it is not this environment.
        state_->source = source;
        ++state_->generation;
        state_->ready_generation = 0U;
        state_->frame.reset();
        state_->state = EnvironmentCacheState::Pending;
        state_->request = State::Request{source, state_->generation};
        state_->has_request = true;
        state_->admissions.fetch_add(1U, std::memory_order_relaxed);
        lock.unlock();
        state_->idle.notify_all();
        result.state = EnvironmentCacheState::Pending;
        result.generation = state_->generation;
        return result;
    }

    result.state = state_->state;
    result.generation = state_->generation;
    result.publication_epoch =
        state_->publication_epoch.load(std::memory_order_acquire);
    if (state_->frame != nullptr
        && (publication_fence == 0U
            || result.publication_epoch <= publication_fence)) {
        // Withheld past the fence rather than appearing midway through a
        // recording, which would light half a frame from a different map.
        result.frame = state_->frame;
        state_->cache_hits.fetch_add(1U, std::memory_order_relaxed);
    }
    return result;
}

bool EnvironmentCache::refresh(const std::string& source) noexcept {
    if (state_ == nullptr || source.empty()) {
        return false;
    }
    {
        const std::lock_guard lock{state_->mutex};
        state_->source = source;
        ++state_->generation;
        state_->state = EnvironmentCacheState::Pending;
        state_->request = State::Request{source, state_->generation};
        state_->has_request = true;
        state_->admissions.fetch_add(1U, std::memory_order_relaxed);
    }
    state_->idle.notify_all();
    return true;
}

core::u64 EnvironmentCache::publication_epoch() const noexcept {
    return state_ == nullptr
        ? 0U
        : state_->publication_epoch.load(std::memory_order_acquire);
}

EnvironmentCacheStats EnvironmentCache::stats() const noexcept {
    EnvironmentCacheStats result{};
    if (state_ == nullptr) {
        return result;
    }
    result.admissions = state_->admissions.load(std::memory_order_relaxed);
    result.completions = state_->completions.load(std::memory_order_relaxed);
    result.failures = state_->failures.load(std::memory_order_relaxed);
    result.cache_hits = state_->cache_hits.load(std::memory_order_relaxed);
    result.busy_observations =
        state_->busy_observations.load(std::memory_order_relaxed);
    result.late_results_discarded =
        state_->late_results_discarded.load(std::memory_order_relaxed);
    return result;
}

bool EnvironmentCache::wait_for_test() noexcept {
    if (state_ == nullptr) {
        return false;
    }
    std::unique_lock lock{state_->mutex};
    state_->idle.wait(lock, [this] {
        return !state_->working && !state_->has_request;
    });
    return true;
}

} // namespace mediaserver::render
