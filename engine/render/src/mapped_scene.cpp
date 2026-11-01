#include <mediaserver/render/mapped_scene.hpp>

#include <mediaserver/render/projection_mesh_loader.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stop_token>
#include <thread>
#include <utility>

namespace mediaserver::render {
namespace {

struct ProjectionMeshKey final {
  doc::NodeId map_node{};
  doc::NodeId surface_node{};
  scene::CanvasRect region{};
  output::ResolvedProjectionSurface surface;
  core::u64 authored_generation{};

  [[nodiscard]] bool
  matches(const output::OutputMap &map,
          const output::ResolvedProjectionSurface &candidate, core::u64 requested_generation = 0U) const noexcept {
    if (authored_generation != requested_generation) return false;
    if (surface.shape != candidate.shape) {
      return false;
    }
    if (candidate.shape == output::ProjectionSurfaceShape::ImportedMesh) {
      // Imported file content is immutable local geometry. Map/surface node,
      // raster region, evaluated calibration, and mesh scale are placement
      // inputs, not asset identity, so animation cannot enqueue another read.
      return surface.mesh_source == candidate.mesh_source;
    }
    // A generated mesh is built in stage coordinates; the stage only places
    // it each frame, so it is not part of the mesh's identity.
    return map_node == map.node && surface_node == map.surface &&
           region == map.source && surface.geometry == candidate.geometry &&
           surface.horizontal_arc_turns == candidate.horizontal_arc_turns &&
           surface.mesh_columns == candidate.mesh_columns &&
           surface.mesh_source == candidate.mesh_source &&
           surface.mesh_scale == candidate.mesh_scale &&
           surface.mesh_asset == candidate.mesh_asset &&
           surface.color == candidate.color;
  }
};

enum class ProjectionMeshPhase : core::u8 {
  Empty,
  AwaitingAdmission,
  Loading,
  Ready,
  Failed,
};

constexpr core::u64 status_state_mask = 0x3U;
constexpr core::u64 status_stale_bit = 0x4U;
constexpr core::u64 status_generation_shift = 3U;
constexpr core::u64 status_generation_mask =
    std::numeric_limits<core::u64>::max() >> status_generation_shift;
static_assert(std::atomic<core::u64>::is_always_lock_free,
              "projection cache status must remain a lock-free snapshot");

[[nodiscard]] OutputProjectionCacheState
public_state(ProjectionMeshPhase phase) noexcept {
  switch (phase) {
  case ProjectionMeshPhase::Empty:
    return OutputProjectionCacheState::Empty;
  case ProjectionMeshPhase::AwaitingAdmission:
  case ProjectionMeshPhase::Loading:
    return OutputProjectionCacheState::Loading;
  case ProjectionMeshPhase::Ready:
    return OutputProjectionCacheState::Ready;
  case ProjectionMeshPhase::Failed:
    return OutputProjectionCacheState::Failed;
  }
  return OutputProjectionCacheState::Failed;
}

[[nodiscard]] core::u64 packed_status(ProjectionMeshPhase phase,
                                      bool stale_ready,
                                      core::u64 generation) noexcept {
  return ((generation & status_generation_mask) << status_generation_shift) |
         (stale_ready ? status_stale_bit : 0U) |
         static_cast<core::u64>(public_state(phase));
}

[[nodiscard]] core::u64 next_generation(core::u64 generation) noexcept {
  generation = (generation + 1U) & status_generation_mask;
  return generation == 0U ? 1U : generation;
}

[[nodiscard]] const rhi::Texture *
record_scene(CompositorSceneExecutor &executor, rhi::CommandList &commands,
             const scene::Scene &frame, scene::CanvasRect slice,
             MediaCache *shared_media_cache) noexcept {
  auto rendered = executor.execute(commands, frame, slice, shared_media_cache);
  return rendered ? rendered.value() : nullptr;
}

[[nodiscard]] const rhi::Texture *
record_scene(CompositorSceneExecutor &executor, rhi::CommandList &commands,
             const compositor::CompositorGraph &graph,
             const scene::Scene &frame, scene::CanvasRect slice,
             MediaCache *shared_media_cache) noexcept {
  if (!graph.valid() || graph.plan.steps.empty()) {
    return record_scene(executor, commands, frame, slice, shared_media_cache);
  }
  const auto planned = plan_compositor_scene(graph, frame);
  if (!planned) {
    // Project-global graphs can contain layers from another switcher scene.
    // Preserve that scene's intrinsic output rather than blanking it.
    if (planned.error() == CompositorScenePlanError::MissingSceneLayer) {
      return record_scene(executor, commands, frame, slice, shared_media_cache);
    }
    return nullptr;
  }
  auto rendered =
      executor.execute(commands, graph, frame, slice, shared_media_cache);
  return rendered ? rendered.value() : nullptr;
}

} // namespace

struct detail::OutputProjectionCacheLoadGate::State final {
  std::mutex mutex;
  std::condition_variable changed;
  bool blocked{};
  bool released{};
  bool finished{};
};

detail::OutputProjectionCacheLoadGate::OutputProjectionCacheLoadGate()
    : state_(std::make_shared<State>()) {}

detail::OutputProjectionCacheLoadGate::~OutputProjectionCacheLoadGate() {
  release_for_test();
}

bool detail::OutputProjectionCacheLoadGate::
    wait_until_blocked_for_test() noexcept {
  try {
    std::unique_lock lock{state_->mutex};
    return state_->changed.wait_for(lock, std::chrono::seconds{10},
                                    [&] { return state_->blocked; });
  } catch (...) {
    return false;
  }
}

bool detail::OutputProjectionCacheLoadGate::
    wait_until_finished_for_test() noexcept {
  try {
    std::unique_lock lock{state_->mutex};
    return state_->changed.wait_for(lock, std::chrono::seconds{10},
                                    [&] { return state_->finished; });
  } catch (...) {
    return false;
  }
}

void detail::OutputProjectionCacheLoadGate::release_for_test() noexcept {
  try {
    {
      std::lock_guard lock{state_->mutex};
      state_->released = true;
    }
    state_->changed.notify_all();
  } catch (...) {
  }
}

void detail::OutputProjectionCacheLoadGate::block_worker_for_test() noexcept {
  try {
    std::unique_lock lock{state_->mutex};
    state_->blocked = true;
    state_->changed.notify_all();
    state_->changed.wait(lock, [&] { return state_->released; });
  } catch (...) {
  }
}

void detail::OutputProjectionCacheLoadGate::finish_worker_for_test() noexcept {
  try {
    {
      std::lock_guard lock{state_->mutex};
      state_->finished = true;
    }
    state_->changed.notify_all();
  } catch (...) {
  }
}

struct OutputProjectionCache::AsyncState final
    : std::enable_shared_from_this<OutputProjectionCache::AsyncState> {
  struct MeshObservation final {
    std::shared_ptr<const ProjectionMeshKey> key;
    std::shared_ptr<const output::ProjectionMesh> mesh;
  };

  struct Executor final {
    struct Work final {
      std::weak_ptr<AsyncState> completion;
      std::shared_ptr<const ProjectionMeshKey> key;
      std::shared_ptr<detail::OutputProjectionCacheLoadGate> gate;
      core::u64 generation{};
    };

    static constexpr std::size_t maximum_pending = 64U;

    [[nodiscard]] static Executor &instance() {
      static Executor executor;
      return executor;
    }

    Executor(const Executor &) = delete;
    Executor &operator=(const Executor &) = delete;

    [[nodiscard]] bool try_enqueue(Work work) noexcept {
      return enqueue(std::move(work), false);
    }

    [[nodiscard]] bool enqueue_for_test(Work work) noexcept {
      return enqueue(std::move(work), true);
    }

  private:
    Executor() : worker_([this](std::stop_token stop) { run(stop); }) {}

    ~Executor() {
      // Only process teardown owns this join. Individual caches retain no
      // worker and can be reset or destroyed while a filesystem read runs.
      worker_.request_stop();
      changed_.notify_all();
    }

    [[nodiscard]] bool enqueue(Work work, bool wait_for_lock) noexcept {
      try {
        std::unique_lock lock{mutex_, std::defer_lock};
        if (wait_for_lock) {
          lock.lock();
        } else if (!lock.try_lock()) {
          return false;
        }
        if (pending_ >= maximum_pending) {
          return false;
        }
        queue_.push_back(std::move(work));
        ++pending_;
        changed_.notify_one();
        return true;
      } catch (...) {
        return false;
      }
    }

    void complete_one() noexcept {
      std::lock_guard lock{mutex_};
      if (pending_ != 0U) {
        --pending_;
      }
    }

    void run(std::stop_token stop) noexcept {
      while (!stop.stop_requested()) {
        Work work{};
        {
          std::unique_lock lock{mutex_};
          changed_.wait(
              lock, [&] { return stop.stop_requested() || !queue_.empty(); });
          if (stop.stop_requested()) {
            return;
          }
          work = std::move(queue_.front());
          queue_.pop_front();
        }

        if (auto completion = work.completion.lock()) {
          if (work.gate != nullptr) {
            work.gate->block_worker_for_test();
          }
          std::shared_ptr<const output::ProjectionMesh> mesh;
          bool loaded = false;
          try {
            auto result =
                load_projection_mesh_local(work.key->surface.mesh_source);
            if (result.has_value()) {
              mesh = std::make_shared<const output::ProjectionMesh>(
                  std::move(result).value());
              loaded = true;
            }
          } catch (...) {
            mesh.reset();
          }
          completion->publish(std::move(work.key), work.generation,
                              std::move(mesh), loaded);
        }
        if (work.gate != nullptr) {
          work.gate->finish_worker_for_test();
        }
        complete_one();
      }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Work> queue_;
    std::size_t pending_{};
    // Last so synchronization and queues outlive the worker's final access.
    std::jthread worker_;
  };

  AsyncState() : executor_(&Executor::instance()) { publish_status_locked(); }

  [[nodiscard]] MeshObservation
  observe_mesh(const output::OutputMap &map,
               const output::ResolvedProjectionSurface &surface, core::u64 requested_generation = 0U) noexcept {
    // Construct an owned key only on an authored change. Unchanged arc and
    // imported resolves compare in place and allocate nothing here.
    std::shared_ptr<const ProjectionMeshKey> replacement;
    if (observed_key_ == nullptr || !observed_key_->matches(map, surface, requested_generation)) {
      try {
        replacement = std::make_shared<const ProjectionMeshKey>(
            ProjectionMeshKey{map.node, map.surface, map.source, surface, requested_generation});
      } catch (...) {
        return MeshObservation{};
      }
    }

    std::unique_lock lock{mutex_, std::try_to_lock};
    if (lock.owns_lock()) {
      if (key_ == nullptr || !key_->matches(map, surface, requested_generation)) {
        if (replacement == nullptr) {
          try {
            replacement = std::make_shared<const ProjectionMeshKey>(
                ProjectionMeshKey{map.node, map.surface, map.source, surface, requested_generation});
          } catch (...) {
            return MeshObservation{};
          }
        }
        begin_key_locked(std::move(replacement));
      } else if (phase_ == ProjectionMeshPhase::AwaitingAdmission) {
        try_admit_locked(false);
      }
      observed_key_ = key_;
      observed_mesh_ = mesh_;
    }

    // The observation is render-thread-owned. If publication holds the state
    // lock, exact-source stale geometry remains retained; a newly authored
    // source can never receive geometry from the former source generation.
    if (observed_key_ == nullptr || !observed_key_->matches(map, surface, requested_generation)) {
      // If the try-lock lost to publication, retain the requested replacement
      // key with a null answer. A command-fenced caller must freeze that cold
      // observation instead of retrying and adopting midway through recording.
      return MeshObservation{std::move(replacement), {}};
    }
    return MeshObservation{observed_key_, observed_mesh_};
  }

  [[nodiscard]] std::shared_ptr<const output::ProjectionMesh>
  resolve_mesh(const output::OutputMap &map,
               const output::ResolvedProjectionSurface &surface, core::u64 requested_generation = 0U) noexcept {
    const MeshObservation observed = observe_mesh(map, surface, requested_generation);
    return observed.key != nullptr && observed.key->matches(map, surface, requested_generation)
               ? observed.mesh
               : nullptr;
  }

  void reconcile_recording() noexcept {
    if (!recording_) {
      recording_key_.reset();
      recording_mesh_.reset();
      return;
    }
    if (recording_.outcome() == rhi::CommandRecordingOutcome::Pending) {
      return;
    }
    recording_ = {};
    recording_key_.reset();
    recording_mesh_.reset();
  }

  [[nodiscard]] bool recording_pending() const noexcept {
    return recording_ &&
           recording_.outcome() == rhi::CommandRecordingOutcome::Pending;
  }

  [[nodiscard]] std::shared_ptr<const output::ProjectionMesh>
  resolve_mesh(rhi::CommandList &commands, const output::OutputMap &map,
               const output::ResolvedProjectionSurface &surface, core::u64 requested_generation = 0U) noexcept {
    reconcile_recording();
    const rhi::CommandRecordingToken current = commands.recording_token();

    if (recording_pending()) {
      if (current && current.same_recording(recording_)) {
        return recording_key_ != nullptr &&
                       recording_key_->matches(map, surface, requested_generation)
                   ? recording_mesh_
                   : nullptr;
      }
      if (!current) {
        // Ledger exhaustion is permanently latched by CommandList. Hold this
        // slot's exact frozen answer; never retry/adopt midway through it.
        return recording_key_ != nullptr &&
                       recording_key_->matches(map, surface, requested_generation)
                   ? recording_mesh_
                   : nullptr;
      }
      // Two live outer recordings may not share one mutable observation slot.
      // The caller refuses this recording and can retry after the owner ends.
      return {};
    }

    if (!current) {
      // No transaction identity means no admission or publication adoption.
      // The render-thread-owned observation is the only safe stale answer.
      return observed_key_ != nullptr && observed_key_->matches(map, surface, requested_generation)
                 ? observed_mesh_
                 : nullptr;
    }

    MeshObservation observed = observe_mesh(map, surface, requested_generation);
    recording_ = current;
    recording_key_ = std::move(observed.key);
    recording_mesh_ = std::move(observed.mesh);
    return recording_key_ != nullptr && recording_key_->matches(map, surface, requested_generation)
               ? recording_mesh_
               : nullptr;
  }

  [[nodiscard]] bool refresh_imported_mesh() noexcept {
    // An operator explicitly requested this mutation, so do not silently drop
    // it behind the worker's very short publication section. The lock never
    // covers loader I/O; only resolve() has the hard try-lock requirement.
    std::lock_guard lock{mutex_};
    if (key_ == nullptr ||
        key_->surface.shape != output::ProjectionSurfaceShape::ImportedMesh) {
      return false;
    }
    if (phase_ == ProjectionMeshPhase::AwaitingAdmission) {
      try_admit_locked(false);
      return true;
    }
    if (phase_ == ProjectionMeshPhase::Loading) {
      return false;
    }
    generation_ = next_generation(generation_);
    phase_ = ProjectionMeshPhase::AwaitingAdmission;
    publish_status_locked();
    try_admit_locked(false);
    return true;
  }

  [[nodiscard]] OutputProjectionCacheStatus status() const noexcept {
    const core::u64 value = status_.load(std::memory_order_acquire);
    return OutputProjectionCacheStatus{
        static_cast<OutputProjectionCacheState>(value & status_state_mask),
        (value & status_stale_bit) != 0U, value >> status_generation_shift};
  }

  void reset() noexcept {
    std::lock_guard lock{mutex_};
    reset_locked();
  }

  [[nodiscard]] bool try_reset() noexcept {
    std::unique_lock lock{mutex_, std::try_to_lock};
    if (!lock.owns_lock()) {
      return false;
    }
    reset_locked();
    return true;
  }

  void reset_locked() noexcept {
    generation_ = next_generation(generation_);
    key_.reset();
    mesh_.reset();
    phase_ = ProjectionMeshPhase::Empty;
    observed_key_.reset();
    observed_mesh_.reset();
    recording_ = {};
    recording_key_.reset();
    recording_mesh_.reset();
    next_gate_.reset();
    publish_status_locked();
    changed_.notify_all();
  }

  void hold_lock_for_test(const std::function<void()> &while_locked) {
    std::lock_guard lock{mutex_};
    while_locked();
  }

  [[nodiscard]] bool wait_for_completion_for_test() noexcept {
    try {
      std::unique_lock lock{mutex_};
      if (phase_ == ProjectionMeshPhase::AwaitingAdmission) {
        try_admit_locked(true);
      }
      if (phase_ != ProjectionMeshPhase::Loading) {
        return phase_ == ProjectionMeshPhase::Ready ||
               phase_ == ProjectionMeshPhase::Failed;
      }
      constexpr auto maximum_wait = std::chrono::seconds{10};
      if (!changed_.wait_for(lock, maximum_wait, [&] {
            return phase_ != ProjectionMeshPhase::Loading;
          })) {
        return false;
      }
      return phase_ == ProjectionMeshPhase::Ready ||
             phase_ == ProjectionMeshPhase::Failed;
    } catch (...) {
      return false;
    }
  }

  [[nodiscard]] std::shared_ptr<detail::OutputProjectionCacheLoadGate>
  block_next_load_for_test() {
    auto gate = std::make_shared<detail::OutputProjectionCacheLoadGate>();
    std::lock_guard lock{mutex_};
    next_gate_ = gate;
    return gate;
  }

private:
  void begin_key_locked(
      std::shared_ptr<const ProjectionMeshKey> replacement) noexcept {
    generation_ = next_generation(generation_);
    key_ = std::move(replacement);
    mesh_.reset();

    if (key_->surface.shape == output::ProjectionSurfaceShape::HorizontalArc) {
      phase_ = ProjectionMeshPhase::AwaitingAdmission;
      publish_status_locked();
      try {
        auto built = output::horizontal_arc_mesh(
            key_->surface.geometry, key_->surface.horizontal_arc_turns,
            key_->surface.mesh_columns, key_->region);
        if (built.has_value()) {
          mesh_ = std::make_shared<const output::ProjectionMesh>(
              std::move(built).value());
          phase_ = ProjectionMeshPhase::Ready;
        } else {
          phase_ = ProjectionMeshPhase::Failed;
        }
      } catch (...) {
        mesh_.reset();
        phase_ = ProjectionMeshPhase::Failed;
      }
      publish_status_locked();
      changed_.notify_all();
      return;
    }

    if (key_->surface.shape == output::ProjectionSurfaceShape::ImportedMesh) {
      if (key_->surface.mesh_source.empty()) {
        phase_ = ProjectionMeshPhase::Failed;
        publish_status_locked();
        changed_.notify_all();
        return;
      }
      phase_ = ProjectionMeshPhase::AwaitingAdmission;
      publish_status_locked();
      try_admit_locked(false);
      return;
    }

    phase_ = ProjectionMeshPhase::Failed;
    publish_status_locked();
    changed_.notify_all();
  }

  void try_admit_locked(bool wait_for_executor) noexcept {
    if (phase_ != ProjectionMeshPhase::AwaitingAdmission || key_ == nullptr ||
        key_->surface.shape != output::ProjectionSurfaceShape::ImportedMesh) {
      return;
    }
    const Executor::Work work{weak_from_this(), key_, next_gate_, generation_};
    const bool admitted = wait_for_executor ? executor_->enqueue_for_test(work)
                                            : executor_->try_enqueue(work);
    if (admitted) {
      next_gate_.reset();
      phase_ = ProjectionMeshPhase::Loading;
    }
    publish_status_locked();
  }

  void publish(std::shared_ptr<const ProjectionMeshKey> completed_key,
               core::u64 completed_generation,
               std::shared_ptr<const output::ProjectionMesh> completed_mesh,
               bool loaded) noexcept {
    std::lock_guard lock{mutex_};
    if (phase_ != ProjectionMeshPhase::Loading || key_ != completed_key ||
        generation_ != completed_generation) {
      changed_.notify_all();
      return;
    }
    if (loaded && completed_mesh != nullptr) {
      mesh_ = std::move(completed_mesh);
      phase_ = ProjectionMeshPhase::Ready;
    } else {
      // A same-key explicit refresh keeps the previously ready mesh. Its
      // failure is sticky and visible, but the wall need not blank.
      phase_ = ProjectionMeshPhase::Failed;
    }
    publish_status_locked();
    changed_.notify_all();
  }

  void publish_status_locked() noexcept {
    status_.store(
        packed_status(phase_,
                      mesh_ != nullptr && phase_ != ProjectionMeshPhase::Ready,
                      generation_),
        std::memory_order_release);
  }

  Executor *executor_{};
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::shared_ptr<const ProjectionMeshKey> key_;
  std::shared_ptr<const output::ProjectionMesh> mesh_;
  ProjectionMeshPhase phase_{ProjectionMeshPhase::Empty};
  core::u64 generation_{};
  std::atomic<core::u64> status_{};
  std::shared_ptr<detail::OutputProjectionCacheLoadGate> next_gate_;

  // Accessed by the one owning render thread only. Control-side refresh and
  // worker publication never mutate this retained observation.
  std::shared_ptr<const ProjectionMeshKey> observed_key_;
  std::shared_ptr<const output::ProjectionMesh> observed_mesh_;

  // Render-thread-owned snapshot for one outer command recording. Worker
  // publication changes mesh_, never these fields.
  rhi::CommandRecordingToken recording_;
  std::shared_ptr<const ProjectionMeshKey> recording_key_;
  std::shared_ptr<const output::ProjectionMesh> recording_mesh_;
};

const output::ProjectionMesh *
MappedProjection::projection_mesh() const noexcept {
  return retained_mesh.get();
}

OutputProjectionCache::OutputProjectionCache()
    : state_(std::make_shared<AsyncState>()) {}

OutputProjectionCache::OutputProjectionCache(
    OutputProjectionCache &&) noexcept = default;

OutputProjectionCache &
OutputProjectionCache::operator=(OutputProjectionCache &&) noexcept = default;

OutputProjectionCache::~OutputProjectionCache() = default;

void OutputProjectionCache::reset() noexcept {
  if (state_ != nullptr) {
    state_->reset();
  }
}

bool OutputProjectionCache::refresh_imported_mesh() noexcept {
  return state_ != nullptr && state_->refresh_imported_mesh();
}

OutputProjectionCacheStatus OutputProjectionCache::status() const noexcept {
  return state_ != nullptr ? state_->status() : OutputProjectionCacheStatus{};
}

void OutputProjectionCache::reconcile_recording() noexcept {
  if (state_ != nullptr) {
    state_->reconcile_recording();
  }
}

bool OutputProjectionCache::recording_pending() const noexcept {
  return state_ != nullptr && state_->recording_pending();
}

bool OutputProjectionCache::try_reset() noexcept {
  return state_ == nullptr || state_->try_reset();
}

std::shared_ptr<const output::ProjectionMesh>
OutputProjectionCache::resolve_mesh(const output::OutputMap &map) noexcept {
  if (state_ == nullptr ||
      map.projection != output::FeedProjection::PlanarSurface ||
      !map.resolved_surface.has_value()) {
    return {};
  }
  const auto &surface = *map.resolved_surface;
  if (surface.shape == output::ProjectionSurfaceShape::Planar) {
    return {};
  }
  return state_->resolve_mesh(map, surface);
}

namespace {

[[nodiscard]] std::optional<MappedProjection> build_mapped_projection(
    const scene::Scene &frame, const output::OutputMap &map,
    std::shared_ptr<const output::ProjectionMesh> mesh) noexcept {
  if (map.projection != output::FeedProjection::PlanarSurface ||
      !map.resolved_surface.has_value()) {
    return std::nullopt;
  }
  const output::ResolvedProjectionSurface &surface = *map.resolved_surface;
  // A camera-anchored stage is placed afresh every frame from the evaluated
  // scene camera. Only the surface's centre and axes move into the world; the
  // eye stays the camera's own, so a world-fixed stage and a carried one
  // differ in exactly one place. Generated meshes keep their cached
  // stage-space vertices and follow through a placement instead, which keeps
  // camera motion off the mesh cache entirely.
  std::optional<output::StagePose> stage;
  if (surface.stage.anchor == output::StageAnchor::Camera) {
    const auto pose =
        output::camera_stage_pose(frame.camera, surface.stage.eye);
    if (!pose) {
      return std::nullopt;
    }
    stage = pose.value();
  }
  const output::PhysicalSurface geometry =
      stage.has_value() ? output::place_on_stage(surface.geometry, *stage)
                        : surface.geometry;
  // The roll a fitted mesh view takes. On a carried stage it is the face's
  // own up edge, so a ceiling fits about the room rather than about whichever
  // world axis the fitter falls back to when it looks straight up.
  const scene::Vec3 stage_up =
      stage.has_value() ? geometry.up : scene::Vec3{0.0F, 1.0F, 0.0F};
  switch (surface.shape) {
  case output::ProjectionSurfaceShape::Planar: {
    const auto built = output::perspective_view_for(
        geometry, map.source, frame.camera.eye, frame.camera.near_plane,
        frame.camera.far_plane);
    if (!built) {
      return std::nullopt;
    }
    MappedProjection projection{
        SceneRenderView{built.value().matrices, built.value().eye,
                        built.value().forward, map.node.value()},
        {},
        std::nullopt};
    projection.surface_up = geometry.up;
    return projection;
  }
  case output::ProjectionSurfaceShape::HorizontalArc: {
    if (mesh == nullptr) {
      return std::nullopt;
    }
    std::optional<output::ProjectionMeshPlacement> placement;
    if (stage.has_value()) {
      placement = output::stage_mesh_placement(*stage);
    }
    auto mesh_view =
        placement.has_value()
            ? output::mesh_perspective_view_for(
                  *mesh, *placement, frame.camera.eye,
                  frame.camera.near_plane, frame.camera.far_plane, stage_up)
            : output::mesh_perspective_view_for(*mesh, frame.camera.eye,
                                                frame.camera.near_plane,
                                                frame.camera.far_plane);
    if (!mesh_view) {
      return std::nullopt;
    }
    MappedProjection projection{};
    projection.render_view =
        SceneRenderView{mesh_view.value().matrices, mesh_view.value().eye,
                        mesh_view.value().forward, map.node.value()};
    projection.retained_mesh = std::move(mesh);
    projection.mesh_view.emplace(std::move(mesh_view).value());
    projection.mesh_placement = placement;
    projection.surface_up = geometry.up;
    return projection;
  }
  case output::ProjectionSurfaceShape::ImportedMesh: {
    if (mesh == nullptr) {
      return std::nullopt;
    }
    const output::ProjectionMeshPlacement placement{geometry,
                                                     surface.mesh_scale};
    if (surface.mesh_scale > output::max_imported_mesh_scale ||
        !output::valid(placement)) {
      return std::nullopt;
    }
    auto mesh_view = output::mesh_perspective_view_for(
        *mesh, placement, frame.camera.eye, frame.camera.near_plane,
        frame.camera.far_plane, stage_up);
    if (!mesh_view) {
      return std::nullopt;
    }
    MappedProjection projection{};
    projection.render_view =
        SceneRenderView{mesh_view.value().matrices, mesh_view.value().eye,
                        mesh_view.value().forward, map.node.value()};
    projection.retained_mesh = std::move(mesh);
    projection.mesh_view.emplace(std::move(mesh_view).value());
    projection.mesh_placement.emplace(placement);
    projection.surface_up = geometry.up;
    return projection;
  }
  }
  return std::nullopt;
}

} // namespace

std::optional<MappedProjection>
OutputProjectionCache::resolve(const scene::Scene &frame,
                               const output::OutputMap &map) noexcept {
  if (map.projection != output::FeedProjection::PlanarSurface ||
      !map.resolved_surface.has_value()) {
    return std::nullopt;
  }
  const output::ResolvedProjectionSurface &surface = *map.resolved_surface;
  std::shared_ptr<const output::ProjectionMesh> mesh;
  if (surface.shape != output::ProjectionSurfaceShape::Planar) {
    if (state_ == nullptr) return std::nullopt;
    mesh = state_->resolve_mesh(map, surface, scene::asset_generation_key(frame));
  }
  return build_mapped_projection(frame, map, std::move(mesh));
}

std::optional<MappedProjection> OutputProjectionCache::resolve(
    rhi::CommandList &commands, const scene::Scene &frame,
    const output::OutputMap &map) noexcept {
  if (map.projection != output::FeedProjection::PlanarSurface ||
      !map.resolved_surface.has_value()) {
    return std::nullopt;
  }
  const output::ResolvedProjectionSurface &surface = *map.resolved_surface;
  std::shared_ptr<const output::ProjectionMesh> mesh;
  if (surface.shape != output::ProjectionSurfaceShape::Planar) {
    if (state_ == nullptr) {
      return std::nullopt;
    }
    mesh = state_->resolve_mesh(commands, map, surface, scene::asset_generation_key(frame));
  }
  return build_mapped_projection(frame, map, std::move(mesh));
}

void OutputProjectionCacheRegistry::prepare_recording(
    rhi::CommandList &commands) noexcept {
  const rhi::CommandRecordingToken current = commands.recording_token();
  if (prepared_recording_ && current &&
      prepared_recording_.outcome() ==
          rhi::CommandRecordingOutcome::Pending &&
      current.same_recording(prepared_recording_)) {
    return;
  }

  // A terminal observation can belong to a map that is absent this frame.
  // Reconcile the complete fixed registry once at every outer command
  // boundary so inactive slots never pin transaction receipts indefinitely.
  for (Slot &slot : slots_) {
    slot.cache.reconcile_recording();
  }
  prepared_recording_ = current;
}

void OutputProjectionCacheRegistry::touch(Slot &slot) noexcept {
  if (access_clock_ == std::numeric_limits<core::u64>::max()) {
    // The practical lifetime is unreachable, but deterministic rebasing keeps
    // overflow defined without allocating a sort scratch on the frame path.
    access_clock_ = 0U;
    for (Slot &candidate : slots_) {
      candidate.last_use = 0U;
    }
  }
  slot.last_use = ++access_clock_;
}

OutputProjectionCache *
OutputProjectionCacheRegistry::cache_for(doc::NodeId map_node) noexcept {
  if (!map_node) {
    return nullptr;
  }

  for (Slot &slot : slots_) {
    if (slot.occupied && slot.map_node == map_node) {
      touch(slot);
      return &slot.cache;
    }
  }

  for (Slot &slot : slots_) {
    if (!slot.occupied) {
      slot.occupied = true;
      slot.map_node = map_node;
      touch(slot);
      return &slot.cache;
    }
  }

  std::array<bool, output::max_output_maps> busy{};
  for (std::size_t attempt = 0U; attempt < slots_.size(); ++attempt) {
    Slot *reclaim = nullptr;
    std::size_t reclaim_index{};
    for (std::size_t index = 0U; index < slots_.size(); ++index) {
      Slot &slot = slots_[index];
      if (busy[index] || slot.cache.recording_pending()) {
        continue;
      }
      if (reclaim == nullptr || slot.last_use < reclaim->last_use) {
        reclaim = &slot;
        reclaim_index = index;
      }
    }
    if (reclaim == nullptr) {
      return nullptr;
    }
    // Re-keying is a render-path operation. Publication owns only a short
    // critical section, but even that may never be waited on here; try the
    // next deterministic LRU candidate when this one is busy.
    if (!reclaim->cache.try_reset()) {
      busy[reclaim_index] = true;
      continue;
    }
    reclaim->map_node = map_node;
    touch(*reclaim);
    return &reclaim->cache;
  }
  return nullptr;
}

std::optional<MappedProjection> OutputProjectionCacheRegistry::resolve(
    rhi::CommandList &commands, const scene::Scene &frame,
    const output::OutputMap &map) noexcept {
  prepare_recording(commands);
  OutputProjectionCache *const cache = cache_for(map.node);
  return cache != nullptr ? cache->resolve(commands, frame, map)
                          : std::nullopt;
}

bool OutputProjectionCacheRegistry::refresh_imported_meshes() noexcept {
  bool refreshed = false;
  for (Slot &slot : slots_) {
    if (slot.occupied && slot.cache.refresh_imported_mesh()) {
      refreshed = true;
    }
  }
  return refreshed;
}

OutputProjectionCacheStatus
OutputProjectionCacheRegistry::status() const noexcept {
  OutputProjectionCacheStatus aggregate{};
  core::u8 rank{};
  for (const Slot &slot : slots_) {
    if (!slot.occupied) {
      continue;
    }
    const OutputProjectionCacheStatus observed = slot.cache.status();
    aggregate.stale_ready = aggregate.stale_ready || observed.stale_ready;
    if (observed.generation > aggregate.generation) {
      aggregate.generation = observed.generation;
    }
    core::u8 observed_rank{};
    switch (observed.state) {
    case OutputProjectionCacheState::Empty:
      observed_rank = 0U;
      break;
    case OutputProjectionCacheState::Ready:
      observed_rank = 1U;
      break;
    case OutputProjectionCacheState::Failed:
      observed_rank = 2U;
      break;
    case OutputProjectionCacheState::Loading:
      observed_rank = 3U;
      break;
    }
    if (observed_rank > rank) {
      rank = observed_rank;
      aggregate.state = observed.state;
    }
  }
  return aggregate;
}

void OutputProjectionCacheRegistry::reset() noexcept {
  prepared_recording_ = {};
  access_clock_ = 0U;
  for (Slot &slot : slots_) {
    slot.cache.reset();
    slot.map_node = {};
    slot.last_use = 0U;
    slot.occupied = false;
  }
}

OutputProjectionCache *detail_output_projection_cache_for_test(
    OutputProjectionCacheRegistry &registry, doc::NodeId map_node) noexcept {
  for (OutputProjectionCacheRegistry::Slot &slot : registry.slots_) {
    if (slot.occupied && slot.map_node == map_node) {
      return &slot.cache;
    }
  }
  return nullptr;
}

namespace detail {

void hold_output_projection_cache_lock_for_test(
    OutputProjectionCache &cache, const std::function<void()> &while_locked) {
  if (cache.state_ != nullptr) {
    cache.state_->hold_lock_for_test(while_locked);
  } else {
    while_locked();
  }
}

bool wait_for_output_projection_cache_for_test(
    OutputProjectionCache &cache) noexcept {
  return cache.state_ != nullptr &&
         cache.state_->wait_for_completion_for_test();
}

std::shared_ptr<OutputProjectionCacheLoadGate>
block_next_output_projection_cache_load_for_test(OutputProjectionCache &cache) {
  return cache.state_ != nullptr ? cache.state_->block_next_load_for_test()
                                 : nullptr;
}

} // namespace detail

namespace {

template <typename ProjectionCaches>
const rhi::Texture *record_output_mapped_scene_impl(
    CompositorSceneExecutor &executor, ProjectionMeshWarp &mesh_warp,
    ProjectionCaches &projection_cache, rhi::CommandList &commands,
    const scene::Scene &frame, const output::OutputMap &map,
    bool &mapping_unavailable, MediaCache *shared_media_cache) noexcept {
  if (map.projection == output::FeedProjection::CanvasSlice) {
    return record_scene(executor, commands, frame, map.source,
                        shared_media_cache);
  }
  auto projection = projection_cache.resolve(commands, frame, map);
  if (!projection.has_value()) {
    mapping_unavailable = true;
    return nullptr;
  }
  const auto rendered =
      executor.execute(commands, frame, projection->render_view,
                       shared_media_cache);
  if (!rendered) {
    return nullptr;
  }
  const output::ProjectionMesh *const projection_mesh =
      projection->projection_mesh();
  if (projection_mesh == nullptr) {
    return rendered.value();
  }
  if (!projection->mesh_view.has_value() ||
      !mesh_warp.render(commands, *rendered.value(), *projection_mesh,
                        *projection->mesh_view,
                        projection->mesh_placement.has_value()
                            ? &*projection->mesh_placement
                            : nullptr)) {
    return nullptr;
  }
  return &mesh_warp.target();
}

template <typename ProjectionCaches>
const rhi::Texture *record_output_mapped_scene_impl(
    CompositorSceneExecutor &executor, ProjectionMeshWarp &mesh_warp,
    ProjectionCaches &projection_cache, rhi::CommandList &commands,
    const compositor::CompositorGraph &graph, const scene::Scene &frame,
    const output::OutputMap &map, bool &mapping_unavailable,
    MediaCache *shared_media_cache) noexcept {
  if (map.projection == output::FeedProjection::CanvasSlice) {
    return record_scene(executor, commands, graph, frame, map.source,
                        shared_media_cache);
  }
  auto projection = projection_cache.resolve(commands, frame, map);
  if (!projection.has_value()) {
    mapping_unavailable = true;
    return nullptr;
  }
  const auto finish =
      [&](const rhi::Texture *rendered) -> const rhi::Texture * {
    const output::ProjectionMesh *const projection_mesh =
        projection->projection_mesh();
    if (rendered == nullptr || projection_mesh == nullptr) {
      return rendered;
    }
    if (!projection->mesh_view.has_value() ||
        !mesh_warp.render(commands, *rendered, *projection_mesh,
                          *projection->mesh_view,
                          projection->mesh_placement.has_value()
                              ? &*projection->mesh_placement
                              : nullptr)) {
      return nullptr;
    }
    return &mesh_warp.target();
  };
  if (!graph.valid() || graph.plan.steps.empty()) {
    const auto rendered =
        executor.execute(commands, frame, projection->render_view,
                         shared_media_cache);
    return finish(rendered ? rendered.value() : nullptr);
  }
  const auto planned = plan_compositor_scene(graph, frame);
  if (!planned) {
    if (planned.error() == CompositorScenePlanError::MissingSceneLayer) {
      const auto rendered =
          executor.execute(commands, frame, projection->render_view,
                           shared_media_cache);
      return finish(rendered ? rendered.value() : nullptr);
    }
    return nullptr;
  }
  const auto rendered =
      executor.execute(commands, graph, frame, projection->render_view,
                       shared_media_cache);
  return finish(rendered ? rendered.value() : nullptr);
}

} // namespace

const rhi::Texture *record_output_mapped_scene(
    CompositorSceneExecutor &executor, ProjectionMeshWarp &mesh_warp,
    OutputProjectionCache &projection_cache, rhi::CommandList &commands,
    const scene::Scene &frame, const output::OutputMap &map,
    bool &mapping_unavailable, MediaCache *shared_media_cache) noexcept {
  return record_output_mapped_scene_impl(
      executor, mesh_warp, projection_cache, commands, frame, map,
      mapping_unavailable, shared_media_cache);
}

const rhi::Texture *record_output_mapped_scene(
    CompositorSceneExecutor &executor, ProjectionMeshWarp &mesh_warp,
    OutputProjectionCacheRegistry &projection_caches,
    rhi::CommandList &commands, const scene::Scene &frame,
    const output::OutputMap &map, bool &mapping_unavailable,
    MediaCache *shared_media_cache) noexcept {
  return record_output_mapped_scene_impl(
      executor, mesh_warp, projection_caches, commands, frame, map,
      mapping_unavailable, shared_media_cache);
}

const rhi::Texture *record_output_mapped_scene(
    CompositorSceneExecutor &executor, ProjectionMeshWarp &mesh_warp,
    OutputProjectionCache &projection_cache, rhi::CommandList &commands,
    const compositor::CompositorGraph &graph, const scene::Scene &frame,
    const output::OutputMap &map, bool &mapping_unavailable,
    MediaCache *shared_media_cache) noexcept {
  return record_output_mapped_scene_impl(
      executor, mesh_warp, projection_cache, commands, graph, frame, map,
      mapping_unavailable, shared_media_cache);
}

const rhi::Texture *record_output_mapped_scene(
    CompositorSceneExecutor &executor, ProjectionMeshWarp &mesh_warp,
    OutputProjectionCacheRegistry &projection_caches,
    rhi::CommandList &commands,
    const compositor::CompositorGraph &graph, const scene::Scene &frame,
    const output::OutputMap &map, bool &mapping_unavailable,
    MediaCache *shared_media_cache) noexcept {
  return record_output_mapped_scene_impl(
      executor, mesh_warp, projection_caches, commands, graph, frame, map,
      mapping_unavailable, shared_media_cache);
}

} // namespace mediaserver::render
