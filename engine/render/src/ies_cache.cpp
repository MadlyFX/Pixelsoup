#include <mediaserver/render/ies_profile.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace mediaserver::render {
struct IesCache::State final {
    struct Entry {
        std::string source;
        core::u64 generation{}, epoch{}, authored_generation{};
        IesCacheState state{IesCacheState::Unseen};
        IesError error{IesError::None};
        std::shared_ptr<const IesProfile> profile;
        bool queued{}, working{};
    };
    mutable std::mutex mutex;
    mutable std::condition_variable changed;
    // Four independent scenes may remain pinned. No path allocation on observe.
    std::array<Entry, max_ies_profiles * 4> entries;
    core::u64 generation{};
    std::shared_ptr<const std::function<void()>> next_load_gate;
    std::atomic_uint64_t epoch{1}, admissions{}, failures{}, late{}, busy{}, refused{}, overwrites{};
    std::jthread worker;

    State() {
        for (auto& e : entries) e.source.reserve(max_ies_source_bytes);
        worker = std::jthread([this](std::stop_token stop) {
            for (;;) {
                std::string source;
                core::u64 work_generation{};
                std::size_t slot{};
                std::shared_ptr<const std::function<void()>> gate;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [&] {
                        if (stop.stop_requested()) return true;
                        for (const auto& e : entries) if (e.queued) return true;
                        return false;
                    });
                    if (stop.stop_requested()) return;
                    while (!entries[slot].queued) ++slot;
                    auto& e = entries[slot];
                    e.queued = false; // take the request; replacements are counted
                    e.working = true;
                    source = e.source;
                    work_generation = e.generation;
                    gate = std::move(next_load_gate);
                }
                if (gate) (*gate)();
                auto result = load_ies_profile(source);
                {
                    std::lock_guard lock(mutex);
                    auto& e = entries[slot];
                    e.working = false;
                    if (e.generation != work_generation) ++late;
                    else if (!result.profile) {
                        e.state = IesCacheState::Failed;
                        e.error = result.error;
                        ++failures;
                    } else {
                        e.profile = std::move(result.profile);
                        e.error = IesError::None;
                        e.state = IesCacheState::Ready;
                        e.epoch = epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
                    }
                }
                changed.notify_all();
            }
        });
    }
    ~State() { worker.request_stop(); changed.notify_all(); }
    void queue(Entry& e) {
        if (e.queued) ++overwrites;
        e.generation = ++generation;
        e.state = IesCacheState::Pending;
        e.error = IesError::None;
        e.queued = true;
        changed.notify_all();
    }
};

IesCache::IesCache() {
    static auto shared = std::make_shared<State>();
    state_ = shared;
}
IesObservation IesCache::observe(std::string_view source, core::u64 fence,
                                 core::u64 authored_generation) noexcept {
    if (source.empty()) return {};
    if (source.size() > max_ies_source_bytes) {
        ++state_->refused;
        return {{}, IesCacheState::Capacity, IesError::Capacity};
    }
    std::unique_lock lock(state_->mutex, std::try_to_lock);
    if (!lock) { ++state_->busy; return {{}, IesCacheState::Busy}; }
    State::Entry* candidate = nullptr;
    for (auto& e : state_->entries) {
        if (e.source == source) {
            if (authored_generation != 0U && e.authored_generation != authored_generation) {
                e.authored_generation = authored_generation;
                state_->queue(e);
            }
            const bool visible = e.state == IesCacheState::Ready && (fence == 0 || e.epoch <= fence);
            return {visible ? e.profile : nullptr,
                e.state == IesCacheState::Ready && !visible ? IesCacheState::Pending : e.state,
                e.error, e.generation, e.epoch};
        }
        if (e.state == IesCacheState::Unseen) candidate = &e;
        else if (!candidate && !e.working && !e.queued && e.state != IesCacheState::Failed
                 && (!e.profile || e.profile.use_count() == 1)) candidate = &e;
    }
    if (!candidate) { ++state_->refused; return {{}, IesCacheState::Capacity, IesError::Capacity}; }
    candidate->source.assign(source);
    candidate->authored_generation = authored_generation;
    candidate->profile.reset();
    candidate->epoch = 0;
    state_->queue(*candidate);
    ++state_->admissions;
    return {{}, IesCacheState::Pending, IesError::None, candidate->generation};
}
bool IesCache::refresh(std::string_view source) noexcept {
    std::unique_lock lock(state_->mutex, std::try_to_lock);
    if (!lock) return false;
    for (auto& e : state_->entries) if (e.source == source) {
        state_->queue(e);
        return true;
    }
    return false;
}
core::u64 IesCache::publication_epoch() const noexcept { return state_->epoch.load(std::memory_order_acquire); }
IesCacheStats IesCache::stats() const noexcept {
    return {state_->admissions.load(), state_->failures.load(), state_->late.load(),
            state_->busy.load(), state_->refused.load(), state_->overwrites.load()};
}
void IesCache::wait_for_test() const {
    std::unique_lock lock(state_->mutex);
    state_->changed.wait(lock, [&] {
        for (const auto& e : state_->entries) if (e.working || e.queued) return false;
        return true;
    });
}
void IesCache::set_next_load_gate_for_test(std::shared_ptr<const std::function<void()>> gate) {
    std::lock_guard lock(state_->mutex);
    state_->next_load_gate = std::move(gate);
}
} // namespace mediaserver::render
