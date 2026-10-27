#pragma once

#include <mediaserver/render/model_loader.hpp>
#include <mediaserver/scene/raycast.hpp>

#include <cstddef>
#include <memory>

// Ray-cast hierarchies for decoded models, so the Stage can say exactly where
// on a set piece the pointer is. The model cache already holds each model's
// triangles in memory; what is missing is a hierarchy over them, and building
// one for a large set is far too slow for the UI thread.
//
// So `observe` never builds. The first sight of a model queues its hierarchy
// for a background worker and returns nothing; the caller uses the model's
// bounds until a later frame finds the hierarchy ready. A hierarchy belongs to
// one decoded generation: a refreshed model is a new asset and is queued anew,
// while the old one's hierarchy is released once that asset is gone.
namespace mediaserver::editor {

struct ModelSurfaceCacheConfig final {
    // Hierarchies queued or building at once. A busy queue makes a cold model
    // wait for a later frame rather than growing it.
    std::size_t max_pending{4U};
    // Roughly 20 bytes a triangle: the default holds about 25 million.
    std::size_t max_bytes{512ULL * 1024ULL * 1024ULL};
};

struct ModelSurfaceCacheStats final {
    std::size_t ready{};
    std::size_t pending{};
    std::size_t bytes{};
    std::size_t builds{};
    std::size_t evictions{};
};

class ModelSurfaceCache;

namespace detail {
// Test seam: blocks until no hierarchy is queued or building.
[[nodiscard]] bool wait_for_model_surface_cache_for_test(
    ModelSurfaceCache& cache) noexcept;
} // namespace detail

class ModelSurfaceCache final {
public:
    explicit ModelSurfaceCache(ModelSurfaceCacheConfig config = {});
    ModelSurfaceCache(const ModelSurfaceCache&) = delete;
    ModelSurfaceCache& operator=(const ModelSurfaceCache&) = delete;
    ModelSurfaceCache(ModelSurfaceCache&&) noexcept;
    ModelSurfaceCache& operator=(ModelSurfaceCache&&) noexcept;
    // Asks a building worker to stop and joins it.
    ~ModelSurfaceCache();

    // UI-thread lookup. Never builds, frees or waits: a contended lock reads
    // as not ready. Null until the hierarchy for exactly this asset is built,
    // and for a model with no usable triangles.
    [[nodiscard]] std::shared_ptr<const scene::MeshBvh>
    observe(const std::shared_ptr<const render::ModelAsset>& asset);

    // Project boundary: the worker drops every hierarchy and queued build.
    void request_reset() noexcept;

    [[nodiscard]] ModelSurfaceCacheStats stats() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;

    friend bool detail::wait_for_model_surface_cache_for_test(
        ModelSurfaceCache&) noexcept;
};

} // namespace mediaserver::editor
