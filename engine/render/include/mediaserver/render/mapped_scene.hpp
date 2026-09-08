#pragma once

#include <mediaserver/compositor/compositor_model.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/render/compositor_scene_executor.hpp>
#include <mediaserver/render/projection_mesh_warp.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/scene.hpp>

#include <array>
#include <functional>
#include <memory>
#include <optional>

// Shared output-mapping render path.
//
// Both a render node and the editor's Program Output must resolve physical
// surfaces, imported UV meshes, and compositor graphs identically. Keeping
// that work here prevents the operator preview from drifting into a plain
// camera view while the connector is rendering an off-axis/warped image.
namespace mediaserver::render {

struct MappedProjection final {
  SceneRenderView render_view{};
  // Retains asynchronously published and procedurally cached geometry through
  // command recording. A refresh can publish a replacement concurrently
  // without invalidating this frame's mesh pointer.
  std::shared_ptr<const output::ProjectionMesh> retained_mesh;
  std::optional<output::MeshPerspectiveView> mesh_view;
  // Present only for imported geometry, whose retained vertices remain in
  // immutable file-local coordinates across live calibration animation.
  std::optional<output::ProjectionMeshPlacement> mesh_placement;

  [[nodiscard]] const output::ProjectionMesh *projection_mesh() const noexcept;
};

enum class OutputProjectionCacheState : core::u8 {
  Empty,
  Loading,
  Ready,
  Failed,
};

struct OutputProjectionCacheStatus final {
  OutputProjectionCacheState state{OutputProjectionCacheState::Empty};
  // True while an explicit same-source refresh is loading (or failed) and the
  // last ready local mesh remains safe to draw for that imported source.
  bool stale_ready{};
  core::u64 generation{};

  bool operator==(const OutputProjectionCacheStatus &) const = default;
};

class OutputProjectionCache;

namespace detail {

class OutputProjectionCacheLoadGate final {
public:
  OutputProjectionCacheLoadGate();
  OutputProjectionCacheLoadGate(const OutputProjectionCacheLoadGate &) = delete;
  OutputProjectionCacheLoadGate &
  operator=(const OutputProjectionCacheLoadGate &) = delete;
  ~OutputProjectionCacheLoadGate();

  [[nodiscard]] bool wait_until_blocked_for_test() noexcept;
  [[nodiscard]] bool wait_until_finished_for_test() noexcept;
  void release_for_test() noexcept;

  // Called only by the cache's background executor.
  void block_worker_for_test() noexcept;
  void finish_worker_for_test() noexcept;

private:
  struct State;
  std::shared_ptr<State> state_;
};

// Deterministic synchronization seams used only by the CPU cache tests. They
// live here rather than changing the production loader interface or relying on
// scheduler timing to prove that resolve() never waits behind publication.
void hold_output_projection_cache_lock_for_test(
    OutputProjectionCache &cache, const std::function<void()> &while_locked);
[[nodiscard]] bool wait_for_output_projection_cache_for_test(
    OutputProjectionCache &cache) noexcept;
[[nodiscard]] std::shared_ptr<OutputProjectionCacheLoadGate>
block_next_output_projection_cache_load_for_test(OutputProjectionCache &cache);

} // namespace detail

class OutputProjectionCache final {
public:
  OutputProjectionCache();
  OutputProjectionCache(const OutputProjectionCache &) = delete;
  OutputProjectionCache &operator=(const OutputProjectionCache &) = delete;
  OutputProjectionCache(OutputProjectionCache &&) noexcept;
  OutputProjectionCache &operator=(OutputProjectionCache &&) noexcept;
  ~OutputProjectionCache();

  // Render-facing resolution never opens, stats, or reads an imported file.
  // A cold authored key is admitted to bounded background work with try-locks;
  // while publication/admission is busy this immediately returns the retained
  // ready mesh for the exact same key, or null.
  [[nodiscard]] std::optional<MappedProjection>
  resolve(const scene::Scene &frame, const output::OutputMap &map) noexcept;

  // Production recording form. The first observation made by an outer
  // command list freezes the exact authored key and retained mesh, including
  // a stale or null answer. Later resolves on that command cannot adopt a
  // worker publication that completed between mapped views. If the bounded
  // command-token ledger is unavailable, the last exact observation is held
  // without admitting or adopting new work.
  [[nodiscard]] std::optional<MappedProjection>
  resolve(rhi::CommandList &commands, const scene::Scene &frame,
          const output::OutputMap &map) noexcept;

  // Geometry-only form for editor previews and other callers that do not own
  // a scene camera. Imported geometry is returned in immutable file-local
  // coordinates; callers apply the current surface calibration and mesh scale
  // when they need world positions. It has the same exact-source, nonblocking
  // observation/admission semantics as resolve(); planar mappings return null.
  [[nodiscard]] std::shared_ptr<const output::ProjectionMesh>
  resolve_mesh(const output::OutputMap &map) noexcept;

  // Operator/control-path request to re-read the currently authored imported
  // mesh. Failures otherwise remain sticky forever. This records one new
  // generation without doing filesystem work. It may synchronize only with
  // the cache's short publication critical section; false means there is no
  // imported key to refresh or that generation is already loading.
  [[nodiscard]] bool refresh_imported_mesh() noexcept;

  // Lock-free informational snapshot suitable for UI display. Readiness is
  // deliberately not polled by resolve() to trigger work.
  [[nodiscard]] OutputProjectionCacheStatus status() const noexcept;

  // Safe-boundary invalidation. It never joins an in-flight loader; late
  // publication is discarded by generation/key identity.
  void reset() noexcept;

private:
  struct AsyncState;
  std::shared_ptr<AsyncState> state_;

  void reconcile_recording() noexcept;
  [[nodiscard]] bool recording_pending() const noexcept;
  [[nodiscard]] bool try_reset() noexcept;

  friend void detail::hold_output_projection_cache_lock_for_test(
      OutputProjectionCache &, const std::function<void()> &);
  friend bool detail::wait_for_output_projection_cache_for_test(
      OutputProjectionCache &) noexcept;
  friend std::shared_ptr<detail::OutputProjectionCacheLoadGate>
  detail::block_next_output_projection_cache_load_for_test(
      OutputProjectionCache &);
  friend class OutputProjectionCacheRegistry;
};

// Presenter-owned, fixed-capacity topology for mapped output geometry. Every
// cache state is constructed with the presenter, never on first use in the
// per-map loop. Stable non-zero map.node identity selects an exact slot, then
// the lowest empty slot, then a deterministic least-recently-used slot. A
// slot whose former command observation is still pending is never reclaimed.
class OutputProjectionCacheRegistry final {
public:
  OutputProjectionCacheRegistry() = default;
  OutputProjectionCacheRegistry(const OutputProjectionCacheRegistry &) =
      delete;
  OutputProjectionCacheRegistry &
  operator=(const OutputProjectionCacheRegistry &) = delete;

  [[nodiscard]] std::optional<MappedProjection>
  resolve(rhi::CommandList &commands, const scene::Scene &frame,
          const output::OutputMap &map) noexcept;

  // Explicit operator request. All resident imported-map slots are refreshed;
  // there is no polling or timer-driven work.
  [[nodiscard]] bool refresh_imported_meshes() noexcept;

  // Lock-free aggregate over occupied slots. Loading outranks Failed, which
  // outranks Ready; stale_ready is true if any retained resident is stale.
  [[nodiscard]] OutputProjectionCacheStatus status() const noexcept;

  // Safe-boundary invalidation of topology and all resident cache states.
  void reset() noexcept;

private:
  struct Slot final {
    OutputProjectionCache cache;
    doc::NodeId map_node{};
    core::u64 last_use{};
    bool occupied{};
  };

  [[nodiscard]] OutputProjectionCache *
  cache_for(doc::NodeId map_node) noexcept;
  void prepare_recording(rhi::CommandList &commands) noexcept;
  void touch(Slot &slot) noexcept;

  std::array<Slot, output::max_output_maps> slots_{};
  rhi::CommandRecordingToken prepared_recording_;
  core::u64 access_clock_{};

  friend OutputProjectionCache *detail_output_projection_cache_for_test(
      OutputProjectionCacheRegistry &, doc::NodeId) noexcept;
};

// Narrow test seam for deterministic worker gating and pointer/generation
// checks without exposing mutable registry slots to production callers.
[[nodiscard]] OutputProjectionCache *detail_output_projection_cache_for_test(
    OutputProjectionCacheRegistry &registry, doc::NodeId map_node) noexcept;

// Records the exact image before the final window-sized copy. Canvas feeds
// render their authored crop; physical feeds render an asymmetric view and,
// when needed, project it through the generated/imported UV mesh.
[[nodiscard]] const rhi::Texture *record_output_mapped_scene(
    CompositorSceneExecutor &executor, ProjectionMeshWarp &mesh_warp,
    OutputProjectionCache &projection_cache, rhi::CommandList &commands,
    const scene::Scene &frame, const output::OutputMap &map,
    bool &mapping_unavailable,
    MediaCache *shared_media_cache = nullptr) noexcept;

[[nodiscard]] const rhi::Texture *record_output_mapped_scene(
    CompositorSceneExecutor &executor, ProjectionMeshWarp &mesh_warp,
    OutputProjectionCacheRegistry &projection_caches,
    rhi::CommandList &commands, const scene::Scene &frame,
    const output::OutputMap &map, bool &mapping_unavailable,
    MediaCache *shared_media_cache = nullptr) noexcept;

[[nodiscard]] const rhi::Texture *record_output_mapped_scene(
    CompositorSceneExecutor &executor, ProjectionMeshWarp &mesh_warp,
    OutputProjectionCache &projection_cache, rhi::CommandList &commands,
    const compositor::CompositorGraph &graph, const scene::Scene &frame,
    const output::OutputMap &map, bool &mapping_unavailable,
    MediaCache *shared_media_cache = nullptr) noexcept;

[[nodiscard]] const rhi::Texture *record_output_mapped_scene(
    CompositorSceneExecutor &executor, ProjectionMeshWarp &mesh_warp,
    OutputProjectionCacheRegistry &projection_caches,
    rhi::CommandList &commands,
    const compositor::CompositorGraph &graph, const scene::Scene &frame,
    const output::OutputMap &map, bool &mapping_unavailable,
    MediaCache *shared_media_cache = nullptr) noexcept;

} // namespace mediaserver::render
