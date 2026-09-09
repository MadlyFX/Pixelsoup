#include <mediaserver/render/baked_lighting.hpp>

#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace mediaserver::render {

struct BakeJob::Executor final {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<std::shared_ptr<BakeJob>> jobs;
  std::shared_ptr<BakeJob> active;
  unsigned outstanding{};
  std::jthread worker;
  Executor()
      : worker([this](std::stop_token stop) {
          for (;;) {
            std::shared_ptr<BakeJob> job;
            {
              std::unique_lock lock(mutex);
              changed.wait(
                  lock, [&] { return stop.stop_requested() || !jobs.empty(); });
              if (stop.stop_requested())
                return;
              job = std::move(jobs.front());
              jobs.pop_front();
              active = job;
            }
            job->result_ = bake_lighting(job->scene_, &job->progress_);
            if (job->result_.ready() && !job->progress_.cancel.load()) {
              job->progress_.stage.store(BakeStage::Writing);
              job->result_ =
                  write_probe_asset(*job->result_.asset, job->destination_);
            }
            job->progress_.stage.store(
                job->progress_.cancel.load() ? BakeStage::Cancelled
                : job->result_.ready()       ? BakeStage::Ready
                                             : BakeStage::Failed,
                std::memory_order_release);
            {
              std::lock_guard lock(mutex);
              active.reset();
              --outstanding;
            }
          }
        }) {}
  ~Executor() {
    {
      std::lock_guard lock(mutex);
      if (active)
        active->cancel();
      for (auto &job : jobs)
        job->cancel();
    }
    worker.request_stop();
    changed.notify_all();
  }
};

std::shared_ptr<BakeJob> BakeJob::start(scene::Scene scene,
                                        std::string destination) {
  static Executor executor;
  std::unique_lock lock(executor.mutex, std::try_to_lock);
  if (!lock || executor.outstanding >= 2 || destination.empty())
    return {};
  auto job = std::make_shared<BakeJob>();
  job->scene_ = std::move(scene);
  job->destination_ = std::move(destination);
  executor.jobs.push_back(job);
  ++executor.outstanding;
  lock.unlock();
  executor.changed.notify_one();
  return job;
}
const BakeResult *BakeJob::result() const noexcept {
  const auto stage = progress_.stage.load(std::memory_order_acquire);
  return stage == BakeStage::Ready || stage == BakeStage::Failed ||
                 stage == BakeStage::Cancelled
             ? &result_
             : nullptr;
}

struct ProbeCache::State final {
  // Sixteen immutable identities, each bounded to 4096 diffuse probes and
  // four fixed reflection captures (about 73 MiB of total pixel/probe data).
  // Entries pinned by a recorded renderer cannot be evicted or replaced.
  struct Entry {
    std::string source, content;
    core::u64 generation{}, epoch{}, authored_generation{};
    ProbeCacheState state{ProbeCacheState::Unseen};
    std::shared_ptr<const ProbeAsset> asset;
    bool queued{};
  };
  mutable std::mutex mutex;
  mutable std::condition_variable changed;
  std::array<Entry, 16> entries;
  core::u64 generation{};
  bool working{};
  std::shared_ptr<const std::function<void()>> next_load_gate;
  std::atomic_uint64_t epoch{1}, admissions{}, failures{}, late{}, busy{},
      refused{};
  std::jthread worker;
  State()
      : worker([this](std::stop_token stop) {
          for (;;) {
            std::string source, content;
            core::u64 work_generation{};
            std::size_t slot{};
            std::shared_ptr<const std::function<void()>> gate;
            {
              std::unique_lock lock(mutex);
              changed.wait(lock, [&] {
                if (stop.stop_requested())
                  return true;
                for (auto &e : entries)
                  if (e.queued)
                    return true;
                return false;
              });
              if (stop.stop_requested())
                return;
              while (slot < entries.size() && !entries[slot].queued)
                ++slot;
              auto &e = entries[slot];
              e.queued = false;
              source = e.source;
              content = e.content;
              work_generation = e.generation;
              working = true;
              gate = std::move(next_load_gate);
            }
            if (gate)
              (*gate)();
            auto result = load_probe_asset(source, content);
            {
              std::lock_guard lock(mutex);
              auto &e = entries[slot];
              working = false;
              if (e.generation != work_generation)
                ++late;
              else if (!result.ready()) {
                e.state = ProbeCacheState::Failed;
                ++failures;
              } else {
                e.asset = std::move(result.asset);
                e.state = ProbeCacheState::Ready;
                e.epoch = epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
              }
            }
            changed.notify_all();
          }
        }) {}
  ~State() {
    worker.request_stop();
    changed.notify_all();
  }
};
ProbeCache::ProbeCache() {
  static auto shared = std::make_shared<State>();
  state_ = shared;
}
ProbeObservation ProbeCache::observe(const std::string &source,
                                     const std::string &content,
                                     core::u64 fence,
                                     core::u64 authored_generation) noexcept {
  if (source.empty())
    return {};
  std::unique_lock lock(state_->mutex, std::try_to_lock);
  if (!lock) {
    ++state_->busy;
    return {{}, ProbeCacheState::Busy};
  }
  State::Entry *empty = nullptr;
  for (auto &e : state_->entries) {
    if (e.source == source && e.content == content) {
      if (authored_generation != 0 &&
          e.authored_generation != authored_generation) {
        e.authored_generation = authored_generation;
        e.generation = ++state_->generation;
        e.state = ProbeCacheState::Pending;
        e.queued = true;
        state_->changed.notify_all();
      }
      return {e.epoch <= fence || fence == 0 ? e.asset : nullptr, e.state,
              e.generation, e.epoch};
    }
    if (!empty && (e.state == ProbeCacheState::Unseen ||
                   ((!e.asset || e.asset.use_count() == 1) &&
                    e.state != ProbeCacheState::Pending)))
      empty = &e;
  }
  if (!empty) {
    ++state_->refused;
    return {{}, ProbeCacheState::Capacity};
  }
  try {
    empty->source = source;
    empty->content = content;
  } catch (...) {
    ++state_->refused;
    return {{}, ProbeCacheState::Capacity};
  }
  empty->authored_generation = authored_generation;
  empty->generation = ++state_->generation;
  empty->asset.reset();
  empty->epoch = 0;
  empty->state = ProbeCacheState::Pending;
  empty->queued = true;
  const auto generation = empty->generation;
  ++state_->admissions;
  lock.unlock();
  state_->changed.notify_all();
  return {{}, ProbeCacheState::Pending, generation};
}
bool ProbeCache::refresh(const std::string &source) noexcept {
  std::unique_lock lock(state_->mutex, std::try_to_lock);
  if (!lock)
    return false;
  bool found = false;
  for (auto &e : state_->entries)
    if (e.source == source) {
      e.generation = ++state_->generation;
      e.state = ProbeCacheState::Pending;
      e.queued = true;
      found = true;
    }
  lock.unlock();
  if (found)
    state_->changed.notify_all();
  return found;
}
core::u64 ProbeCache::publication_epoch() const noexcept {
  return state_->epoch.load(std::memory_order_acquire);
}
ProbeCacheStats ProbeCache::stats() const noexcept {
  return {state_->admissions.load(), state_->failures.load(),
          state_->late.load(), state_->busy.load(), state_->refused.load()};
}
void ProbeCache::wait_for_test() const {
  std::unique_lock lock(state_->mutex);
  state_->changed.wait(lock, [&] {
    if (state_->working)
      return false;
    for (auto &e : state_->entries)
      if (e.queued)
        return false;
    return true;
  });
}
void ProbeCache::set_next_load_gate_for_test(
    std::shared_ptr<const std::function<void()>> gate) {
  std::lock_guard lock(state_->mutex);
  state_->next_load_gate = std::move(gate);
}
} // namespace mediaserver::render
