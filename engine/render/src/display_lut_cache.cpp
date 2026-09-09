#include <mediaserver/render/display_lut_cache.hpp>

#include <atomic>
#include <array>
#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <iostream>
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
    struct Entry final {
        DisplayLutRequest request;
        core::u64 generation{};
        core::u64 touched{};
        bool used{};
        bool pending{};
        DisplayLutObservation result;
    };
    // Bounded residency for a show look and multiple calibrated walls. Old
    // completed requests may be replaced; an in-flight publication is fenced.
    std::array<Entry, 256U> entries;
    mutable std::mutex mutex;
    std::condition_variable idle;
    core::u64 generation{1U};
    core::u64 clock{};
    bool working{};
    std::atomic_uint64_t admissions{}, completions{}, failures{}, busy_observations{}, late_results_discarded{};
    std::jthread worker;

    State() : worker([this](std::stop_token stop) { run(stop); }) {}
    ~State() {
        worker.request_stop();
        { const std::lock_guard lock{mutex}; }
        idle.notify_all();
        if (worker.joinable()) worker.join();
    }
    bool pending() const {
        return std::any_of(entries.begin(), entries.end(), [](const Entry& e) { return e.pending; });
    }
    Entry* find(const DisplayLutRequest& request) {
        for (auto& e : entries) if (e.used && e.request == request) return &e;
        return nullptr;
    }
    Entry* admit(const DisplayLutRequest& request) {
        Entry* target = nullptr;
        for (auto& e : entries) {
            if (!e.used) { target = &e; break; }
            if (!e.pending && (target == nullptr || e.touched < target->touched)) target = &e;
        }
        if (target == nullptr) return nullptr;
        target->request = request;
        target->generation = ++generation;
        target->touched = ++clock;
        target->used = true;
        target->pending = true;
        target->result = {};
        target->result.generation = target->generation;
        target->result.state = DisplayLutState::Pending;
        ++admissions;
        return target;
    }
    void run(std::stop_token stop) {
        for (;;) {
            DisplayLutRequest request;
            core::u64 work_generation{};
            std::size_t index{};
            {
                std::unique_lock lock{mutex};
                idle.wait(lock, [&] { return stop.stop_requested() || pending(); });
                if (stop.stop_requested()) return;
                for (; index < entries.size(); ++index) if (entries[index].pending) break;
                auto& e = entries[index];
                request = e.request;
                work_generation = e.generation;
                e.pending = false;
                working = true;
            }
            DisplayLutObservation result;
            result.generation = work_generation;
            result.state = DisplayLutState::Failed;
            try {
                DisplayLut look, camera;
                bool failed = false;
                if (!request.show_lut_source.empty()) {
                    look = parse_cube_lut(read_text(request.show_lut_source));
                    failed = !look.valid();
                }
                if (request.wall.enabled && !request.wall.camera_inverse_lut_source.empty()) {
                    camera = parse_cube_lut(read_text(request.wall.camera_inverse_lut_source));
                    failed = failed || !camera.valid();
                }
                // Failed wall compensation must never claim readiness. Publish
                // an uncalibrated fallback for diagnostics; Program holds it.
                auto baked = bake_display_lut(request.transform, request.size,
                    look.valid() ? &look : nullptr, request.wall,
                    camera.valid() ? &camera : nullptr);
                if (baked.valid()) {
                    result.atlas = std::make_shared<const std::vector<core::u8>>(pack_display_lut_atlas(baked));
                    result.table = std::make_shared<const DisplayLut>(std::move(baked));
                    if (!failed && !result.atlas->empty()) result.state = DisplayLutState::Ready;
                }
            } catch (...) { result.state = DisplayLutState::Failed; }
            if (result.state == DisplayLutState::Failed) {
                std::cerr << "Colour LUT bake failed: show look [" << request.show_lut_source
                          << "], camera inverse [" << request.wall.camera_inverse_lut_source
                          << "]. Check readable unit-domain RGB .cube files and the display transform.\n";
            }
            {
                const std::lock_guard lock{mutex};
                working = false;
                auto& e = entries[index];
                if (e.generation != work_generation) ++late_results_discarded;
                else {
                    e.result = std::move(result);
                    if (e.result.state == DisplayLutState::Ready) ++completions;
                    else ++failures;
                }
            }
            idle.notify_all();
        }
    }
};

DisplayLutCache::DisplayLutCache() {
    static const std::shared_ptr<State> shared = std::make_shared<State>();
    state_ = shared;
}

DisplayLutObservation DisplayLutCache::observe(const DisplayLutRequest& request) noexcept {
    if (!state_) return {};
    std::unique_lock lock{state_->mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
        ++state_->busy_observations;
        DisplayLutObservation result; result.state = DisplayLutState::Busy; return result;
    }
    auto* e = state_->find(request);
    if (e == nullptr) {
        try { e = state_->admit(request); } catch (...) { e = nullptr; }
        if (e == nullptr) {
            ++state_->failures;
            DisplayLutObservation result; result.state = DisplayLutState::Failed; return result;
        }
        state_->idle.notify_all();
    }
    e->touched = ++state_->clock;
    return e->result;
}

bool DisplayLutCache::refresh(const DisplayLutRequest& request) noexcept {
    if (!state_) return false;
    try {
        const std::lock_guard lock{state_->mutex};
        auto* e = state_->find(request);
        if (e == nullptr) e = state_->admit(request);
        else {
            e->generation = ++state_->generation;
            e->result = {};
            e->result.generation = e->generation;
            e->result.state = DisplayLutState::Pending;
            e->pending = true;
            ++state_->admissions;
        }
        state_->idle.notify_all();
        return e != nullptr;
    } catch (...) { return false; }
}

DisplayLutCacheStats DisplayLutCache::stats() const noexcept {
    if (!state_) return {};
    return {state_->admissions.load(), state_->completions.load(), state_->failures.load(),
        state_->busy_observations.load(), state_->late_results_discarded.load()};
}
bool DisplayLutCache::wait_for_test() noexcept {
    if (!state_) return false;
    std::unique_lock lock{state_->mutex};
    state_->idle.wait(lock, [&] { return !state_->working && !state_->pending(); });
    return true;
}
} // namespace mediaserver::render
