#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/render/model_loader.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

// Bounded, asynchronous CPU residency for imported 3D models.
//
// File reads and model/image decoding are never performed by observe(). A
// cold source is admitted to one process-wide worker with try-lock/drop
// semantics; the render path receives an immutable shared ModelAsset only
// after the worker has completed it. Default ModelCache handles share one
// process-wide decoded residency, so Basic, Advanced, compositor-layer, and
// mapped-output renderers do not parse the same file independently.
namespace mediaserver::render {

enum class ModelCacheState : core::u8 {
    Unseen,
    Pending,
    Ready,
    Failed,
    Busy,
};

struct ModelCacheConfig final {
    // Includes executing and queued work owned by this cache state.
    std::size_t max_pending{32U};
    // Source records include ready generations and sticky failures. They are
    // deliberately bounded metadata: a full table drops a new cold source
    // rather than growing the frame-path map.
    std::size_t max_sources{512U};
    // Includes live and retired decoded generations. The default covers the
    // residency of 256 distinct model assets plus its bounded 64
    // refresh-generation headroom; the byte cap remains authoritative for
    // unusually large authored assets.
    std::size_t max_ready_assets{320U};
    // CPU backing includes mip pyramids and is independent of GPU residency.
    std::size_t max_ready_bytes{2ULL * 1024ULL * 1024ULL * 1024ULL};

    bool operator==(const ModelCacheConfig&) const = default;
};

struct ModelCacheStats final {
    std::size_t sources{};
    std::size_t pending{};
    std::size_t ready_assets{};
    std::size_t ready_bytes{};
    std::size_t retired_assets{};
    std::size_t retired_bytes{};
    std::size_t failed_sources{};
    std::size_t source_capacity{};
    std::size_t pending_capacity{};
    std::size_t ready_asset_capacity{};
    std::size_t ready_byte_capacity{};
    core::u64 admissions{};
    core::u64 admission_drops{};
    core::u64 cache_hits{};
    core::u64 completions{};
    core::u64 failures{};
    core::u64 evictions{};
    core::u64 late_results_discarded{};

    bool operator==(const ModelCacheStats&) const = default;
};

struct ModelCacheObservation final {
    // The newest complete immutable snapshot valid at the caller's recording
    // fence. It may remain present while `state` says a newer generation is
    // Pending or Failed; ready() means the snapshot is safe to integrate.
    std::shared_ptr<const ModelAsset> asset;
    ModelCacheState state{ModelCacheState::Unseen};
    // Generation of asset when ready(), otherwise the requested generation.
    core::u64 generation{};
    // Latest requested source generation, even when an older stale snapshot
    // is returned for continuity during refresh.
    core::u64 current_generation{};
    core::u64 publication_epoch{};
    // A prior complete asset remains in CPU residency while an explicit
    // same-source refresh is pending or failed. Cold sibling renderers may
    // integrate this snapshot too, so refresh never blanks a mapped view.
    bool stale_ready{};

    [[nodiscard]] bool ready() const noexcept { return asset != nullptr; }
};

class ModelAssetLoader {
public:
    ModelAssetLoader() = default;
    ModelAssetLoader(const ModelAssetLoader&) = delete;
    ModelAssetLoader& operator=(const ModelAssetLoader&) = delete;
    virtual ~ModelAssetLoader() = default;

    // Worker-thread only. Implementations may read files and run codecs.
    [[nodiscard]] virtual bool load(
        const std::string& source,
        ModelAsset& into) = 0;
};

class ModelCache;

namespace detail {

// Deterministic CPU-test seams. Production code never waits for cache work or
// takes the publication lock on its render-facing path.
void hold_model_cache_lock_for_test(
    ModelCache& cache,
    const std::function<void()>& while_locked);
[[nodiscard]] bool wait_for_model_cache_for_test(
    ModelCache& cache) noexcept;
[[nodiscard]] bool wait_for_shared_model_cache_for_test() noexcept;
// Installs a deterministic test-only gate that runs after an entry mutation
// is unlocked but before its epoch becomes visible to a new recording.
void set_model_cache_publication_gate_for_test(
    ModelCache& cache,
    const std::function<void()>& while_uncommitted);

} // namespace detail

class ModelCache final {
public:
    // Default handles share one process-wide loader and decoded residency.
    ModelCache();
    explicit ModelCache(
        std::unique_ptr<ModelAssetLoader> loader,
        ModelCacheConfig config = ModelCacheConfig{});

    // Copying shares the same bounded cache state. This is intentional: an
    // immutable decoded asset should feed every renderer that needs it.
    ModelCache(const ModelCache&) noexcept = default;
    ModelCache& operator=(const ModelCache&) noexcept = default;
    ModelCache(ModelCache&&) noexcept = default;
    ModelCache& operator=(ModelCache&&) noexcept = default;
    ~ModelCache() = default;

    // Render-facing observation/admission. Never waits, stats, reads, decodes,
    // joins, or uses a timer. A contended lookup returns Busy immediately. A
    // completed asset newer than publication_fence is held for the next safe
    // command-list boundary rather than becoming visible midway through one.
    [[nodiscard]] ModelCacheObservation observe(
        const std::string& source,
        core::u64 publication_fence, core::u64 authored_generation = 0U) noexcept;

    // Operator/control-path generation change. Failure is sticky until this is
    // called. It may briefly take cache/executor bookkeeping locks, but it
    // performs no file/codec work and never joins an executing loader. A late
    // pre-refresh result is rejected by source generation and ticket.
    [[nodiscard]] bool refresh(const std::string& source) noexcept;

    // Explicit project-transition boundary. Enqueues bounded worker-side
    // reclamation; it does not join or destroy decoded assets on the caller.
    // Call only after every old-project command recording and renderer sharing
    // this cache state has been released. Default handles are process-shared,
    // so this is a process-wide project boundary, not a per-renderer clear.
    // Externally owned generations are retained.
    [[nodiscard]] bool request_safe_reset() noexcept;

    // Current process-state publication epoch. It is monotonic, nonzero, and
    // lock-free; CommandList latches it once for a recording.
    [[nodiscard]] core::u64 publication_epoch() const noexcept;

    // Lock-free, already-computed informational counters for operator UI.
    [[nodiscard]] ModelCacheStats stats() const noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;

    friend void detail::hold_model_cache_lock_for_test(
        ModelCache&, const std::function<void()>&);
    friend bool detail::wait_for_model_cache_for_test(ModelCache&) noexcept;
    friend void detail::set_model_cache_publication_gate_for_test(
        ModelCache&, const std::function<void()>&);
};

} // namespace mediaserver::render
