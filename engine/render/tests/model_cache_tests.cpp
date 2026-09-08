#include <mediaserver/render/model_cache.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

namespace core = mediaserver::core;
namespace render = mediaserver::render;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define CACHE_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

struct LoaderState final {
    std::mutex mutex;
    std::condition_variable changed;
    core::u32 calls{};
    bool available{true};
    bool block_first{};
    core::u32 block_call{};
    bool first_entered{};
    bool release_first{};
    bool first_exited{};
    bool cache_destroyed{};
    bool loader_destroyed{};
};

[[nodiscard]] render::ModelAsset asset_for(core::u32 identity) {
    render::ModelAsset asset{};
    asset.mesh.vertices = {
        scene::MeshVertex{scene::Vec3{0.0F, 0.0F, 0.0F}},
        scene::MeshVertex{scene::Vec3{1.0F, 0.0F, 0.0F}},
        scene::MeshVertex{scene::Vec3{0.0F, 1.0F, 0.0F}}};
    asset.mesh.indices = {0U, 1U, 2U};
    asset.vertex_metadata.resize(asset.mesh.vertices.size());
    asset.parts.push_back(render::ModelPart{0U, 3U});
    asset.textures.push_back(render::ModelTexture{2U, 2U, std::vector<core::u8>(16U, static_cast<core::u8>(identity)), {}});
    asset.bounds.minimum.x = static_cast<float>(identity);
    asset.bounds.maximum.x = static_cast<float>(identity);
    return asset;
}

class InjectedLoader final : public render::ModelAssetLoader {
public:
    explicit InjectedLoader(std::shared_ptr<LoaderState> state)
        : state_(std::move(state)) {}

    ~InjectedLoader() override {
        const std::lock_guard lock{state_->mutex};
        state_->loader_destroyed = true;
        state_->changed.notify_all();
    }

    [[nodiscard]] bool load(
        const std::string&,
        render::ModelAsset& into) override {
        core::u32 call{};
        bool available{};
        {
            std::unique_lock lock{state_->mutex};
            call = ++state_->calls;
            if ((call == 1U && state_->block_first)
                || call == state_->block_call) {
                state_->first_entered = true;
                state_->changed.notify_all();
                state_->changed.wait(
                    lock, [&] { return state_->release_first; });
                state_->first_exited = true;
                state_->changed.notify_all();
            }
            available = state_->available;
            state_->changed.notify_all();
        }
        if (!available) {
            return false;
        }
        into = asset_for(call);
        return true;
    }

private:
    std::shared_ptr<LoaderState> state_;
};

[[nodiscard]] render::ModelCache make_cache(
    const std::shared_ptr<LoaderState>& state,
    render::ModelCacheConfig config = {}) {
    return render::ModelCache{
        std::make_unique<InjectedLoader>(state), config};
}

// Cold requests may lose nonblocking queue admission. Tests that require a
// worker to run retry the request explicitly; an idle queue alone cannot prove
// that a request was accepted. Capacity/contention assertions still observe the
// raw API once, so this helper does not conceal those outcomes.
[[nodiscard]] render::ModelCacheObservation
admit_for_test(render::ModelCache &cache, const std::string &source,
               core::u64 fence, core::u64 authored_generation = 0) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  for (;;) {
    const auto observed = cache.observe(source, fence, authored_generation);
    if (observed.state != render::ModelCacheState::Unseen &&
        observed.state != render::ModelCacheState::Busy)
      return observed;
    CACHE_CHECK(std::chrono::steady_clock::now() < deadline);
    std::this_thread::yield();
  }
}

void wait_for_first_entry(const std::shared_ptr<LoaderState> &state) {
  std::unique_lock lock{state->mutex};
  CACHE_CHECK(state->changed.wait_for(lock, std::chrono::seconds{2},
                                      [&] { return state->first_entered; }));
}

void a_contended_render_observation_returns_immediately() {
  const auto state = std::make_shared<LoaderState>();
  auto cache = make_cache(state);
  std::mutex gate_mutex;
  std::condition_variable gate_changed;
  bool held{};
  bool release{};

  std::jthread holder{[&] {
    render::detail::hold_model_cache_lock_for_test(cache, [&] {
      std::unique_lock gate{gate_mutex};
      held = true;
      gate_changed.notify_all();
      gate_changed.wait(gate, [&] { return release; });
    });
  }};
  {
    std::unique_lock gate{gate_mutex};
    CACHE_CHECK(gate_changed.wait_for(gate, std::chrono::seconds{2},
                                      [&] { return held; }));
  }

  const auto observation =
      cache.observe("held-lock.obj", cache.publication_epoch());
  CACHE_CHECK(observation.state == render::ModelCacheState::Busy);
  CACHE_CHECK(observation.asset == nullptr);

  {
    const std::lock_guard gate{gate_mutex};
    release = true;
    gate_changed.notify_all();
  }
}

void authored_generation_invalidates_and_is_idempotent() {
  const auto state = std::make_shared<LoaderState>();
  auto cache = make_cache(state);
  static_cast<void>(
      admit_for_test(cache, "same-path.obj", cache.publication_epoch(), 81));
  CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
  const auto first =
      cache.observe("same-path.obj", cache.publication_epoch(), 81);
  CACHE_CHECK(first.ready() && !first.stale_ready);
  static_cast<void>(
      admit_for_test(cache, "same-path.obj", cache.publication_epoch(), 82));
  CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
  const auto second =
      cache.observe("same-path.obj", cache.publication_epoch(), 82);
  CACHE_CHECK(second.ready() && !second.stale_ready &&
              second.generation != first.generation);
  CACHE_CHECK(second.asset->bounds.minimum.x != first.asset->bounds.minimum.x);
  for (unsigned i = 0; i < 10; ++i)
    CACHE_CHECK(cache.observe("same-path.obj", cache.publication_epoch(), 82)
                    .generation == second.generation);
  CACHE_CHECK(state->calls == 2);
}

void destruction_does_not_join_a_blocked_decode() {
  const auto state = std::make_shared<LoaderState>();
  state->block_first = true;
  auto cache = std::make_unique<render::ModelCache>(make_cache(state));

  // Admission uses try-lock/drop, so retry until the worker has entered the
  // injected blocking loader.
  while (true) {
    static_cast<void>(
        admit_for_test(*cache, "blocked.obj", cache->publication_epoch()));
    std::unique_lock lock{state->mutex};
    if (state->changed.wait_for(lock, std::chrono::milliseconds{1},
                                [&] { return state->first_entered; })) {
      break;
    }
  }

  std::jthread destroyer{[state, owned = std::move(cache)]() mutable {
    owned.reset();
    const std::lock_guard lock{state->mutex};
    state->cache_destroyed = true;
    state->changed.notify_all();
  }};
  bool returned_before_release{};
  {
    std::unique_lock lock{state->mutex};
    returned_before_release = state->changed.wait_for(
        lock, std::chrono::seconds{2}, [&] { return state->cache_destroyed; });
    state->release_first = true;
    state->changed.notify_all();
  }
  destroyer.join();
  CACHE_CHECK(returned_before_release);
  {
    std::unique_lock lock{state->mutex};
    CACHE_CHECK(state->changed.wait_for(lock, std::chrono::seconds{2}, [&] {
      return state->first_exited && state->loader_destroyed;
    }));
  }
}

void failures_are_sticky_until_explicit_refresh() {
    const auto state = std::make_shared<LoaderState>();
    state->available = false;
    auto cache = make_cache(state);
    const std::string source = "repairable.obj";

    static_cast<void>(admit_for_test(cache, source, cache.publication_epoch()));
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    auto failed = cache.observe(source, cache.publication_epoch());
    CACHE_CHECK(failed.state == render::ModelCacheState::Failed);
    for (core::u32 retry = 0U; retry < 64U; ++retry) {
        static_cast<void>(admit_for_test(cache, source, cache.publication_epoch()));
    }
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    {
        const std::lock_guard lock{state->mutex};
        CACHE_CHECK(state->calls == 1U);
        state->available = true;
    }

    CACHE_CHECK(cache.refresh(source));
    static_cast<void>(admit_for_test(cache, source, cache.publication_epoch()));
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    const auto ready = cache.observe(source, cache.publication_epoch());
    CACHE_CHECK(ready.ready());
    CACHE_CHECK(ready.generation == 2U);
    {
        const std::lock_guard lock{state->mutex};
        CACHE_CHECK(state->calls == 2U);
    }
}

void a_late_pre_refresh_result_is_discarded() {
    const auto state = std::make_shared<LoaderState>();
    state->block_first = true;
    auto cache = make_cache(state);
    const std::string source = "changing.obj";

    static_cast<void>(admit_for_test(cache, source, cache.publication_epoch()));
    wait_for_first_entry(state);
    CACHE_CHECK(cache.refresh(source));
    // Admit generation two while generation one remains inside load().
    static_cast<void>(admit_for_test(cache, source, cache.publication_epoch()));
    {
        const std::lock_guard lock{state->mutex};
        state->release_first = true;
        state->changed.notify_all();
    }
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    const auto ready = cache.observe(source, cache.publication_epoch());
    CACHE_CHECK(ready.ready());
    CACHE_CHECK(ready.generation == 2U);
    CACHE_CHECK(ready.asset->bounds.minimum.x == 2.0F);
    CACHE_CHECK(cache.stats().late_results_discarded == 1U);
}

void one_ready_asset_is_shared_and_published_once() {
    const auto state = std::make_shared<LoaderState>();
    auto first = make_cache(state);
    render::ModelCache second = first;
    const std::string source = "shared.obj";

    static_cast<void>(admit_for_test(first, source, first.publication_epoch()));
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(first));
    const auto a = first.observe(source, first.publication_epoch());
    const auto b = second.observe(source, second.publication_epoch());
    CACHE_CHECK(a.ready());
    CACHE_CHECK(b.ready());
    CACHE_CHECK(a.asset == b.asset);
    for (core::u32 repeat = 0U; repeat < 32U; ++repeat) {
        CACHE_CHECK(first.observe(source, first.publication_epoch()).ready());
        CACHE_CHECK(second.observe(source, second.publication_epoch()).ready());
    }
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(first));
    CACHE_CHECK(first.stats().completions == 1U);
    {
        const std::lock_guard lock{state->mutex};
        CACHE_CHECK(state->calls == 1U);
    }
}

void publication_is_hidden_until_the_reserved_epoch_is_visible() {
    const auto state = std::make_shared<LoaderState>();
    auto cache = make_cache(state);
    const std::string source = "publication-gate.obj";
    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool mutation_unlocked{};
    bool release{};
    render::detail::set_model_cache_publication_gate_for_test(
        cache, [&] {
            std::unique_lock lock{gate_mutex};
            mutation_unlocked = true;
            gate_changed.notify_all();
            gate_changed.wait(lock, [&] { return release; });
        });

    const core::u64 recording_fence = cache.publication_epoch();
    static_cast<void>(admit_for_test(cache, source, recording_fence));
    {
        std::unique_lock lock{gate_mutex};
        CACHE_CHECK(gate_changed.wait_for(
            lock, std::chrono::seconds{2},
            [&] { return mutation_unlocked; }));
    }

    // The entry mutation is complete and unlocked, but the process-visible
    // epoch is intentionally still the one this recording latched.
    CACHE_CHECK(cache.publication_epoch() == recording_fence);
    const auto first_view = cache.observe(source, recording_fence);
    CACHE_CHECK(first_view.state == render::ModelCacheState::Pending);
    CACHE_CHECK(!first_view.ready());
    {
        const std::lock_guard lock{gate_mutex};
        release = true;
        gate_changed.notify_all();
    }
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));

    // The same outer command recording cannot reveal the completion only in
    // its second mapped view. A newly latched recording can.
    const auto second_view = cache.observe(source, recording_fence);
    CACHE_CHECK(second_view.state == render::ModelCacheState::Pending);
    CACHE_CHECK(!second_view.ready());
    auto next_recording =
        cache.observe(source, cache.publication_epoch());
    for (core::u32 attempt = 0U;
         next_recording.state == render::ModelCacheState::Unseen
             && attempt < 64U;
         ++attempt) {
        std::this_thread::yield();
        next_recording = cache.observe(
            source, cache.publication_epoch());
    }
    CACHE_CHECK(next_recording.ready());
}

void a_full_cache_refresh_replaces_cache_only_generation() {
    const auto state = std::make_shared<LoaderState>();
    render::ModelCacheConfig config{};
    config.max_sources = 2U;
    config.max_ready_assets = 2U;
    config.max_ready_bytes = 4U * 1024U;
    auto cache = make_cache(state, config);
    const std::string source = "full-refresh.obj";

    static_cast<void>(admit_for_test(cache, source, cache.publication_epoch()));
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    auto first = cache.observe(source, cache.publication_epoch());
    CACHE_CHECK(first.ready());
    std::weak_ptr<const render::ModelAsset> old = first.asset;
    first = {};

    CACHE_CHECK(cache.refresh(source));
    static_cast<void>(admit_for_test(cache, source, cache.publication_epoch()));
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    const auto replacement =
        cache.observe(source, cache.publication_epoch());
    CACHE_CHECK(replacement.ready());
    CACHE_CHECK(replacement.generation == 2U);
    CACHE_CHECK(replacement.asset->bounds.minimum.x == 2.0F);
    // The prior generation is bounded MVCC state, not LRU garbage: an older
    // command-list fence may still need it in a later mapped view.
    CACHE_CHECK(!old.expired());
    const auto stats = cache.stats();
    CACHE_CHECK(stats.ready_assets == 2U);
    CACHE_CHECK(stats.retired_assets == 1U);
    CACHE_CHECK(stats.ready_bytes <= stats.ready_byte_capacity);
    CACHE_CHECK(stats.evictions == 0U);
}

void an_observed_generation_is_never_destroyed_by_its_observer() {
    const auto state = std::make_shared<LoaderState>();
    render::ModelCacheConfig config{};
    config.max_sources = 2U;
    config.max_ready_assets = 2U;
    config.max_ready_bytes = 4U * 1024U;
    auto cache = make_cache(state, config);
    const std::string source = "held-refresh.obj";

    static_cast<void>(admit_for_test(cache, source, cache.publication_epoch()));
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    auto held = cache.observe(source, cache.publication_epoch());
    CACHE_CHECK(held.ready());
    std::weak_ptr<const render::ModelAsset> old = held.asset;

    CACHE_CHECK(cache.refresh(source));
    static_cast<void>(admit_for_test(cache, source, cache.publication_epoch()));
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    CACHE_CHECK(
        cache.observe(source, cache.publication_epoch()).ready());
    CACHE_CHECK(cache.stats().retired_assets == 1U);
    held = {};
    // State-owned retirement remains the final owner after the render-facing
    // observation releases; reclamation is worker publication work only.
    CACHE_CHECK(!old.expired());

    CACHE_CHECK(!old.expired());
}

void full_residency_refusal_is_sticky_and_does_not_decode_oscillate() {
    const auto state = std::make_shared<LoaderState>();
    render::ModelCacheConfig config{};
    config.max_sources = 3U;
    config.max_ready_assets = 1U;
    config.max_ready_bytes = 4U * 1024U;
    auto cache = make_cache(state, config);
    const auto settled = [&](const std::string &source) {
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds{5};
      for (;;) {
        const auto observed = cache.observe(source, cache.publication_epoch());
        if (observed.state == render::ModelCacheState::Ready ||
            observed.state == render::ModelCacheState::Failed)
          return observed;
        // Queue admission uses try_lock too. An idle worker does not mean
        // that the previous observation successfully enqueued a request.
        CACHE_CHECK(std::chrono::steady_clock::now() < deadline);
        CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
        std::this_thread::yield();
      }
    };

    CACHE_CHECK(settled("a.obj").ready());
    // Release that short-lived observation before worker-only turnover.
    CACHE_CHECK(settled("b.obj").state == render::ModelCacheState::Failed);

    for (core::u32 repeat = 0U; repeat < 64U; ++repeat) {
      const auto a = cache.observe("a.obj", cache.publication_epoch());
      const auto b = cache.observe("b.obj", cache.publication_epoch());
      CACHE_CHECK(a.ready());
      CACHE_CHECK(b.state == render::ModelCacheState::Failed);
    }
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    {
      const std::lock_guard lock{state->mutex};
      CACHE_CHECK(state->calls == 2U);
    }
    CACHE_CHECK(cache.stats().evictions == 0U);

    // Refresh is explicit but cannot break a latched snapshot to steal the
    // sole residency slot. It produces one more sticky answer, not churn.
    CACHE_CHECK(cache.refresh("b.obj"));
    CACHE_CHECK(settled("b.obj").state == render::ModelCacheState::Failed);
    {
      const std::lock_guard lock{state->mutex};
      CACHE_CHECK(state->calls == 3U);
    }
}

void refresh_is_one_epoch_fenced_snapshot_transition() {
  const auto state = std::make_shared<LoaderState>();
  auto cache = make_cache(state);
  const std::string source = "refresh-fence.obj";
  static_cast<void>(admit_for_test(cache, source, cache.publication_epoch()));
  CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
  const core::u64 old_fence = cache.publication_epoch();
  CACHE_CHECK(cache.observe(source, old_fence).ready());

  std::mutex gate_mutex;
  std::condition_variable gate_changed;
  bool mutation_unlocked{};
  bool release{};
  render::detail::set_model_cache_publication_gate_for_test(cache, [&] {
    std::unique_lock lock{gate_mutex};
    mutation_unlocked = true;
    gate_changed.notify_all();
    gate_changed.wait(lock, [&] { return release; });
  });
  bool refreshed{};
  std::jthread refresher{[&] { refreshed = cache.refresh(source); }};
  {
    std::unique_lock lock{gate_mutex};
    CACHE_CHECK(gate_changed.wait_for(lock, std::chrono::seconds{2},
                                      [&] { return mutation_unlocked; }));
  }
  CACHE_CHECK(cache.publication_epoch() == old_fence);
  const auto during = cache.observe(source, old_fence);
  CACHE_CHECK(during.ready());
  CACHE_CHECK(during.generation == 1U);
  {
    const std::lock_guard lock{gate_mutex};
    release = true;
    gate_changed.notify_all();
  }
  refresher.join();
  CACHE_CHECK(refreshed);
  auto same_recording = cache.observe(source, old_fence);
  const auto observe_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (same_recording.state == render::ModelCacheState::Busy) {
    CACHE_CHECK(std::chrono::steady_clock::now() < observe_deadline);
    std::this_thread::yield();
    // Retain the original command fence while worker publication owns
    // the entry mutex. Busy never grants permission to latch a new epoch.
    same_recording = cache.observe(source, old_fence);
  }
  CACHE_CHECK(same_recording.ready());
  CACHE_CHECK(same_recording.generation == 1U);

  const auto next_recording = admit_for_test(cache, source, cache.publication_epoch());
  CACHE_CHECK(next_recording.ready());
  CACHE_CHECK(next_recording.generation == 1U ||
              next_recording.generation == 2U);
  if (next_recording.generation == 1U) {
    CACHE_CHECK(next_recording.state == render::ModelCacheState::Pending);
    CACHE_CHECK(next_recording.stale_ready);
  }
  CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
  const auto replacement = cache.observe(source, cache.publication_epoch());
  CACHE_CHECK(replacement.ready());
  CACHE_CHECK(replacement.generation == 2U);
}

void pending_and_failed_refreshes_keep_the_last_complete_asset() {
  const auto state = std::make_shared<LoaderState>();
  auto cache = make_cache(state);
  const std::string source = "stale-refresh.obj";
  static_cast<void>(admit_for_test(cache, source, cache.publication_epoch()));
  CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
  CACHE_CHECK(cache.observe(source, cache.publication_epoch()).ready());
  {
    const std::lock_guard lock{state->mutex};
    state->block_call = 2U;
    state->first_entered = false;
    state->release_first = false;
  }

  CACHE_CHECK(cache.refresh(source));
  const core::u64 refreshed_fence = cache.publication_epoch();
  auto during = cache.observe(source, refreshed_fence);
  for (core::u32 attempt = 0U;
       during.state == render::ModelCacheState::Unseen && attempt < 64U;
       ++attempt) {
    std::this_thread::yield();
    during = cache.observe(source, refreshed_fence);
  }
  wait_for_first_entry(state);
  during = cache.observe(source, refreshed_fence);
  CACHE_CHECK(during.ready());
  CACHE_CHECK(during.generation == 1U);
  CACHE_CHECK(during.state == render::ModelCacheState::Pending);
  CACHE_CHECK(during.stale_ready);
  {
    const std::lock_guard lock{state->mutex};
    state->available = false;
    state->release_first = true;
    state->changed.notify_all();
  }
  CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
  const auto failed = cache.observe(source, cache.publication_epoch());
  CACHE_CHECK(failed.ready());
  CACHE_CHECK(failed.generation == 1U);
  CACHE_CHECK(failed.state == render::ModelCacheState::Failed);
  CACHE_CHECK(failed.stale_ready);
  for (core::u32 repeat = 0U; repeat < 32U; ++repeat) {
    CACHE_CHECK(cache.observe(source, cache.publication_epoch()).ready());
  }
  {
    const std::lock_guard lock{state->mutex};
    CACHE_CHECK(state->calls == 2U);
  }
}

void a_full_cache_cannot_evict_a_preobserved_command_snapshot() {
    const auto state = std::make_shared<LoaderState>();
    render::ModelCacheConfig config{};
    config.max_sources = 2U;
    config.max_ready_assets = 1U;
    config.max_ready_bytes = 4U * 1024U;
    auto cache = make_cache(state, config);

    static_cast<void>(admit_for_test(cache, "a.obj", cache.publication_epoch()));
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    const core::u64 command_fence = cache.publication_epoch();
    // B finishes after this command latched, but before its first observation
    // of A. Refusal preserves A's exact snapshot for that later view.
    static_cast<void>(admit_for_test(cache, "b.obj", command_fence));
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    const auto a = cache.observe("a.obj", command_fence);
    CACHE_CHECK(a.ready());
    CACHE_CHECK(a.generation == 1U);
    CACHE_CHECK(
        cache.observe("b.obj", cache.publication_epoch()).state
        == render::ModelCacheState::Failed);
}

void three_generation_fences_select_their_exact_mvcc_asset() {
    const auto state = std::make_shared<LoaderState>();
    render::ModelCacheConfig config{};
    config.max_ready_assets = 4U;
    config.max_ready_bytes = 16U * 1024U;
    auto cache = make_cache(state, config);
    const std::string source = "three-generations.obj";
    const auto admit_and_wait = [&] {
        for (core::u32 attempt = 0U; attempt < 256U; ++attempt) {
            const auto observed = cache.observe(
                source, cache.publication_epoch());
            if (observed.state == render::ModelCacheState::Pending) {
                break;
            }
            std::this_thread::yield();
        }
        CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    };

    admit_and_wait();
    const core::u64 first_fence = cache.publication_epoch();
    CACHE_CHECK(cache.observe(source, first_fence).generation == 1U);
    CACHE_CHECK(cache.refresh(source));
    admit_and_wait();
    const core::u64 second_fence = cache.publication_epoch();
    CACHE_CHECK(cache.observe(source, second_fence).generation == 2U);
    CACHE_CHECK(cache.refresh(source));
    admit_and_wait();
    const core::u64 third_fence = cache.publication_epoch();

    const auto first = cache.observe(source, first_fence);
    const auto second = cache.observe(source, second_fence);
    const auto third = cache.observe(source, third_fence);
    CACHE_CHECK(first.ready());
    CACHE_CHECK(second.ready());
    CACHE_CHECK(third.ready());
    CACHE_CHECK(first.generation == 1U);
    CACHE_CHECK(second.generation == 2U);
    CACHE_CHECK(third.generation == 3U);
    CACHE_CHECK(first.asset->bounds.minimum.x == 1.0F);
    CACHE_CHECK(second.asset->bounds.minimum.x == 2.0F);
    CACHE_CHECK(third.asset->bounds.minimum.x == 3.0F);
    CACHE_CHECK(cache.stats().retired_assets == 2U);
}

void explicit_safe_reset_reclaims_historical_sources_on_the_worker() {
    const auto state = std::make_shared<LoaderState>();
    render::ModelCacheConfig config{};
    config.max_sources = 2U;
    config.max_ready_assets = 1U;
    config.max_ready_bytes = 4U * 1024U;
    auto cache = make_cache(state, config);

    static_cast<void>(admit_for_test(cache, "a.obj", cache.publication_epoch()));
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    CACHE_CHECK(cache.observe(
        "a.obj", cache.publication_epoch()).ready());
    static_cast<void>(admit_for_test(cache, "b.obj", cache.publication_epoch()));
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    CACHE_CHECK(cache.stats().sources == 2U);
    CACHE_CHECK(cache.stats().ready_assets == 1U);

    bool requested{};
    for (core::u32 attempt = 0U; attempt < 256U && !requested; ++attempt) {
        requested = cache.request_safe_reset();
        if (!requested) {
            std::this_thread::yield();
        }
    }
    CACHE_CHECK(requested);
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    CACHE_CHECK(cache.stats().sources == 0U);
    CACHE_CHECK(cache.stats().ready_assets == 0U);

    static_cast<void>(admit_for_test(cache, "c.obj", cache.publication_epoch()));
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    const auto c = cache.observe("c.obj", cache.publication_epoch());
    CACHE_CHECK(c.ready());
    CACHE_CHECK(c.asset->bounds.minimum.x == 3.0F);
}

void safe_reset_retains_external_snapshots_until_a_later_boundary() {
    const auto state = std::make_shared<LoaderState>();
    render::ModelCacheConfig config{};
    config.max_sources = 2U;
    config.max_ready_assets = 2U;
    config.max_ready_bytes = 8U * 1024U;
    auto cache = make_cache(state, config);

    static_cast<void>(admit_for_test(cache, "held.obj", cache.publication_epoch()));
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    auto held = cache.observe("held.obj", cache.publication_epoch());
    CACHE_CHECK(held.ready());
    std::weak_ptr<const render::ModelAsset> lifetime = held.asset;

    bool requested{};
    for (core::u32 attempt = 0U; attempt < 256U && !requested; ++attempt) {
        requested = cache.request_safe_reset();
        if (!requested) {
            std::this_thread::yield();
        }
    }
    CACHE_CHECK(requested);
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    CACHE_CHECK(cache.stats().sources == 1U);
    CACHE_CHECK(cache.stats().ready_assets == 1U);
    CACHE_CHECK(!lifetime.expired());

    held = {};
    requested = false;
    for (core::u32 attempt = 0U; attempt < 256U && !requested; ++attempt) {
        requested = cache.request_safe_reset();
        if (!requested) {
            std::this_thread::yield();
        }
    }
    CACHE_CHECK(requested);
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    CACHE_CHECK(cache.stats().sources == 0U);
    CACHE_CHECK(cache.stats().ready_assets == 0U);
    CACHE_CHECK(lifetime.expired());
}

void cache_publishes_complete_mips_with_the_asset_generation() {
    const auto state = std::make_shared<LoaderState>();
    auto cache = make_cache(state);
    static_cast<void>(admit_for_test(cache, "mips", cache.publication_epoch()));
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    const auto first = cache.observe("mips", cache.publication_epoch());
    CACHE_CHECK(first.ready());
    CACHE_CHECK(first.asset->textures[0].lower_mips.size() == 1U);
    CACHE_CHECK(first.asset->textures[0].lower_mips[0].rgba.size() == 4U);
    CACHE_CHECK(first.asset->textures[0].lower_mips[0].rgba[0] == 1U);
    const auto epoch = cache.publication_epoch();
    CACHE_CHECK(cache.refresh("mips"));
    static_cast<void>(admit_for_test(cache, "mips", cache.publication_epoch()));
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    const auto frozen = cache.observe("mips", epoch);
    CACHE_CHECK(frozen.ready() && frozen.asset->textures[0].lower_mips[0].rgba[0] == 1U);
    const auto replaced = cache.observe("mips", cache.publication_epoch());
    CACHE_CHECK(replaced.ready() && replaced.asset->textures[0].lower_mips[0].rgba[0] == 2U);
    CACHE_CHECK(replaced.generation != first.generation);
}

void pending_and_decoded_residency_stay_bounded() {
    const auto state = std::make_shared<LoaderState>();
    state->block_first = true;
    render::ModelCacheConfig config{};
    config.max_pending = 1U;
    config.max_sources = 3U;
    config.max_ready_assets = 1U;
    config.max_ready_bytes = 4U * 1024U;
    auto cache = make_cache(state, config);

    static_cast<void>(admit_for_test(cache, "first.obj", cache.publication_epoch()));
    wait_for_first_entry(state);
    const auto dropped =
        cache.observe("second.obj", cache.publication_epoch());
    CACHE_CHECK(dropped.state == render::ModelCacheState::Unseen);
    CACHE_CHECK(cache.stats().pending == 1U);
    {
        const std::lock_guard lock{state->mutex};
        state->release_first = true;
        state->changed.notify_all();
    }
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));

    static_cast<void>(admit_for_test(cache, "second.obj", cache.publication_epoch()));
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    const auto stats = cache.stats();
    CACHE_CHECK(stats.pending <= stats.pending_capacity);
    CACHE_CHECK(stats.ready_assets <= stats.ready_asset_capacity);
    CACHE_CHECK(stats.ready_bytes <= stats.ready_byte_capacity);
    CACHE_CHECK(stats.admission_drops > 0U);
}

struct Case final {
    const char* name;
    void (*function)();
};

void unpublished_completion_cannot_hide_a_visible_refresh() {
    const auto state = std::make_shared<LoaderState>();
    auto cache = make_cache(state);
    const std::string source = "refresh-completion-fence.obj";
    static_cast<void>(admit_for_test(cache, source, cache.publication_epoch(), 15));
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    const auto old_fence = cache.publication_epoch();
    const auto original = cache.observe(source, old_fence, 15);
    CACHE_CHECK(original.ready());
    CACHE_CHECK(cache.refresh(source));
    const auto refreshed_fence = cache.publication_epoch();
    std::promise<void> mutation, release;
    const auto released = release.get_future().share();
    render::detail::set_model_cache_publication_gate_for_test(cache, [&] {
        mutation.set_value();
        released.wait();
    });
    static_cast<void>(admit_for_test(cache, source, refreshed_fence, 15));
    const bool entered = mutation.get_future().wait_for(std::chrono::seconds{2}) == std::future_status::ready;
    const auto previous_recording = cache.observe(source, old_fence, 15);
    const auto current_recording = cache.observe(source, refreshed_fence, 15);
    release.set_value();
    CACHE_CHECK(render::detail::wait_for_model_cache_for_test(cache));
    CACHE_CHECK(entered);
    CACHE_CHECK(previous_recording.ready() && !previous_recording.stale_ready);
    CACHE_CHECK(current_recording.asset == original.asset);
    CACHE_CHECK(current_recording.stale_ready);
    CACHE_CHECK(current_recording.state == render::ModelCacheState::Pending);
    const auto replacement = cache.observe(source, cache.publication_epoch(), 15);
    CACHE_CHECK(replacement.ready() && !replacement.stale_ready);
    CACHE_CHECK(replacement.generation != original.generation);
}

constexpr std::array<Case, 19U> cases{{
    {"unpublished completion retains the visible refresh", &unpublished_completion_cannot_hide_a_visible_refresh},
    {"a contended render observation returns immediately",
     &a_contended_render_observation_returns_immediately},
    {"authored generation invalidation", &authored_generation_invalidates_and_is_idempotent},
    {"destruction does not join a blocked decode",
     &destruction_does_not_join_a_blocked_decode},
    {"failures are sticky until explicit refresh",
     &failures_are_sticky_until_explicit_refresh},
    {"a late pre-refresh result is discarded",
     &a_late_pre_refresh_result_is_discarded},
    {"one ready asset is shared and published once",
     &one_ready_asset_is_shared_and_published_once},
    {"publication is hidden until reserved epoch is visible",
     &publication_is_hidden_until_the_reserved_epoch_is_visible},
    {"a full cache refresh replaces cache-only generation",
     &a_full_cache_refresh_replaces_cache_only_generation},
    {"an observed generation is never destroyed by its observer",
     &an_observed_generation_is_never_destroyed_by_its_observer},
    {"full residency refusal is sticky and does not decode oscillate",
     &full_residency_refusal_is_sticky_and_does_not_decode_oscillate},
    {"refresh is one epoch-fenced snapshot transition",
     &refresh_is_one_epoch_fenced_snapshot_transition},
    {"pending and failed refreshes keep the last complete asset",
     &pending_and_failed_refreshes_keep_the_last_complete_asset},
    {"a full cache cannot evict a preobserved command snapshot",
     &a_full_cache_cannot_evict_a_preobserved_command_snapshot},
    {"three generation fences select their exact MVCC asset",
     &three_generation_fences_select_their_exact_mvcc_asset},
    {"explicit safe reset reclaims historical sources on worker",
     &explicit_safe_reset_reclaims_historical_sources_on_the_worker},
    {"safe reset retains external snapshots until later boundary",
     &safe_reset_retains_external_snapshots_until_a_later_boundary},
    {"cache publishes complete mips with the asset generation", &cache_publishes_complete_mips_with_the_asset_generation},
    {"pending and decoded residency stay bounded",
     &pending_and_decoded_residency_stay_bounded},
}};

} // namespace

int main() {
    std::size_t failures{};
    for (const Case& test : cases) {
        try {
            test.function();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": "
                      << error.what() << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
