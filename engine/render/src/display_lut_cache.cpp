#include <mediaserver/render/display_lut_cache.hpp>

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <new>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace mediaserver::render {
namespace {

// A 128-point `.cube` is under twenty megabytes of text. Anything past this is
// not a look, it is a mistake, and refusing it keeps a typo in a path from
// pulling a multi-gigabyte file through the worker.
constexpr std::size_t max_cube_bytes = 64U * 1024U * 1024U;

[[nodiscard]] std::string read_text(const std::string& path) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file) {
        return {};
    }
    const std::streamoff size = file.tellg();
    if (size <= 0 || static_cast<std::uintmax_t>(size) > max_cube_bytes) {
        return {};
    }
    file.seekg(0, std::ios::beg);
    std::string text;
    try {
        text.resize(static_cast<std::size_t>(size));
    } catch (const std::bad_alloc&) {
        return {};
    }
    if (!file.read(text.data(), static_cast<std::streamsize>(text.size()))) {
        return {};
    }
    return text;
}

} // namespace

struct DisplayLutCache::State final {
    struct Request final {
        DisplayLutRequest request;
        core::u64 generation{};
    };

    mutable std::mutex mutex;
    std::condition_variable idle;

    DisplayLutRequest resident;
    bool has_resident{};
    core::u64 generation{1U};
    DisplayLutState state{DisplayLutState::Unseen};
    std::shared_ptr<const DisplayLut> table;
    bool working{};
    bool has_request{};
    Request request;

    std::atomic_uint64_t admissions{};
    std::atomic_uint64_t completions{};
    std::atomic_uint64_t failures{};
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

            // Outside the lock: a slow disk must never stall an observe, and
            // the bake itself is a hundred thousand transcendentals that have
            // no business on a frame path either.
            bool look_failed = false;
            DisplayLut look{};
            if (!work.request.show_lut_source.empty()) {
                const std::string text =
                    read_text(work.request.show_lut_source);
                look = text.empty() ? DisplayLut{} : parse_cube_lut(text);
                look_failed = !look.valid();
            }
            DisplayLut baked = bake_display_lut(
                work.request.transform, work.request.size,
                look.valid() ? &look : nullptr);

            {
                const std::lock_guard lock{mutex};
                working = false;
                if (work.generation != generation) {
                    // A refresh or a different request moved on while this
                    // baked. Adopting it would replace the table the operator
                    // just asked for with the one they replaced.
                    late_results_discarded.fetch_add(
                        1U, std::memory_order_relaxed);
                } else if (!baked.valid()) {
                    // The bake itself refused - a size out of range - which is
                    // a programming error rather than an authored one. Nothing
                    // is published; the previous table, if any, stays.
                    state = DisplayLutState::Failed;
                    failures.fetch_add(1U, std::memory_order_relaxed);
                } else {
                    table = std::make_shared<const DisplayLut>(
                        std::move(baked));
                    if (look_failed) {
                        // The transform alone, published under Failed so the
                        // wall shows a predictable picture and the operator is
                        // told the look did not reach it.
                        state = DisplayLutState::Failed;
                        failures.fetch_add(1U, std::memory_order_relaxed);
                    } else {
                        state = DisplayLutState::Ready;
                        completions.fetch_add(1U, std::memory_order_relaxed);
                    }
                }
            }
            idle.notify_all();
        }
    }
};

DisplayLutCache::DisplayLutCache() {
    // Process-shared, so every output on this machine grades from the same
    // baked bytes rather than each baking its own copy of the same table.
    static const std::shared_ptr<State> shared = std::make_shared<State>();
    state_ = shared;
}

DisplayLutObservation DisplayLutCache::observe(
    const DisplayLutRequest& request) noexcept {
    DisplayLutObservation result{};
    if (state_ == nullptr) {
        return result;
    }
    std::unique_lock lock{state_->mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
        state_->busy_observations.fetch_add(1U, std::memory_order_relaxed);
        result.state = DisplayLutState::Busy;
        return result;
    }

    if (!state_->has_resident || state_->resident != request) {
        // A different table than the one resident. Admit it and report
        // Pending; the old table is not offered, because it is not this one.
        state_->resident = request;
        state_->has_resident = true;
        ++state_->generation;
        state_->table.reset();
        state_->state = DisplayLutState::Pending;
        state_->request = State::Request{request, state_->generation};
        state_->has_request = true;
        state_->admissions.fetch_add(1U, std::memory_order_relaxed);
        result.state = DisplayLutState::Pending;
        result.generation = state_->generation;
        lock.unlock();
        state_->idle.notify_all();
        return result;
    }

    result.state = state_->state;
    result.generation = state_->generation;
    result.table = state_->table;
    return result;
}

bool DisplayLutCache::refresh(const DisplayLutRequest& request) noexcept {
    if (state_ == nullptr) {
        return false;
    }
    {
        const std::lock_guard lock{state_->mutex};
        state_->resident = request;
        state_->has_resident = true;
        ++state_->generation;
        state_->state = DisplayLutState::Pending;
        state_->request = State::Request{request, state_->generation};
        state_->has_request = true;
        state_->admissions.fetch_add(1U, std::memory_order_relaxed);
    }
    state_->idle.notify_all();
    return true;
}

DisplayLutCacheStats DisplayLutCache::stats() const noexcept {
    DisplayLutCacheStats result{};
    if (state_ == nullptr) {
        return result;
    }
    result.admissions = state_->admissions.load(std::memory_order_relaxed);
    result.completions = state_->completions.load(std::memory_order_relaxed);
    result.failures = state_->failures.load(std::memory_order_relaxed);
    result.busy_observations =
        state_->busy_observations.load(std::memory_order_relaxed);
    result.late_results_discarded =
        state_->late_results_discarded.load(std::memory_order_relaxed);
    return result;
}

bool DisplayLutCache::wait_for_test() noexcept {
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
