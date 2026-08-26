#include <mediaserver/render/mapped_scene.hpp>

#include <mediaserver/render/projection_mesh_loader.hpp>

#include <new>
#include <system_error>
#include <utility>

namespace mediaserver::render {
namespace {

[[nodiscard]] std::optional<ProjectionMeshFileFingerprint>
projection_mesh_file_fingerprint(const std::string &source) noexcept {
  std::error_code error{};
  const std::filesystem::file_status status =
      std::filesystem::status(source, error);
  if (error || !std::filesystem::is_regular_file(status)) {
    return std::nullopt;
  }
  const std::uintmax_t size = std::filesystem::file_size(source, error);
  if (error) {
    return std::nullopt;
  }
  const std::filesystem::file_time_type modified =
      std::filesystem::last_write_time(source, error);
  if (error) {
    return std::nullopt;
  }
  return ProjectionMeshFileFingerprint{size, modified};
}

[[nodiscard]] const rhi::Texture *
record_scene(CompositorSceneExecutor &executor, rhi::CommandList &commands,
             const scene::Scene &frame, scene::CanvasRect slice) noexcept {
  auto rendered = executor.execute(commands, frame, slice);
  return rendered ? rendered.value() : nullptr;
}

[[nodiscard]] const rhi::Texture *
record_scene(CompositorSceneExecutor &executor, rhi::CommandList &commands,
             const compositor::CompositorGraph &graph,
             const scene::Scene &frame, scene::CanvasRect slice) noexcept {
  if (!graph.valid() || graph.plan.steps.empty()) {
    return record_scene(executor, commands, frame, slice);
  }
  const auto planned = plan_compositor_scene(graph, frame);
  if (!planned) {
    // Project-global graphs can contain layers from another switcher scene.
    // Preserve that scene's intrinsic output rather than blanking it.
    if (planned.error() == CompositorScenePlanError::MissingSceneLayer) {
      return record_scene(executor, commands, frame, slice);
    }
    return nullptr;
  }
  auto rendered = executor.execute(commands, graph, frame, slice);
  return rendered ? rendered.value() : nullptr;
}

} // namespace

const output::ProjectionMesh *
MappedProjection::projection_mesh() const noexcept {
  return borrowed_mesh != nullptr
             ? borrowed_mesh
             : (owned_mesh.has_value() ? &*owned_mesh : nullptr);
}

void OutputProjectionCache::reset() noexcept {
  key_.reset();
  file_.reset();
  mesh_.reset();
  failed_ = false;
  retry_countdown_ = 0U;
}

std::optional<MappedProjection>
OutputProjectionCache::resolve(const scene::Scene &frame,
                               const output::OutputMap &map) noexcept {
  if (map.projection != output::FeedProjection::PlanarSurface ||
      !map.resolved_surface.has_value()) {
    return std::nullopt;
  }
  const output::ResolvedProjectionSurface &surface = *map.resolved_surface;
  switch (surface.shape) {
  case output::ProjectionSurfaceShape::Planar: {
    const auto built = output::perspective_view_for(
        surface.geometry, map.source, frame.camera.eye, frame.camera.near_plane,
        frame.camera.far_plane);
    if (!built) {
      return std::nullopt;
    }
    return MappedProjection{SceneRenderView{built.value().matrices,
                                            built.value().eye,
                                            built.value().forward},
                            std::nullopt, nullptr, std::nullopt};
  }
  case output::ProjectionSurfaceShape::HorizontalArc: {
    auto mesh = output::horizontal_arc_mesh(surface.geometry,
                                            surface.horizontal_arc_turns,
                                            surface.mesh_columns, map.source);
    if (!mesh) {
      return std::nullopt;
    }
    auto mesh_view = output::mesh_perspective_view_for(
        mesh.value(), frame.camera.eye, frame.camera.near_plane,
        frame.camera.far_plane);
    if (!mesh_view) {
      return std::nullopt;
    }
    MappedProjection projection{};
    projection.render_view =
        SceneRenderView{mesh_view.value().matrices, mesh_view.value().eye,
                        mesh_view.value().forward};
    projection.owned_mesh.emplace(std::move(mesh).value());
    projection.mesh_view.emplace(std::move(mesh_view).value());
    return projection;
  }
  case output::ProjectionSurfaceShape::ImportedMesh: {
    constexpr core::u32 failed_retry_frames = 120U;
    const auto file = projection_mesh_file_fingerprint(surface.mesh_source);
    if (!key_.has_value() || *key_ != surface || file_ != file) {
      reset();
    }
    if (failed_ && retry_countdown_ > 0U) {
      --retry_countdown_;
      return std::nullopt;
    }
    if (!mesh_.has_value()) {
      auto loaded = load_projection_mesh(surface.mesh_source, surface.geometry,
                                         surface.mesh_scale);
      if (!loaded) {
        try {
          key_.emplace(surface);
          file_ = file;
          failed_ = true;
          retry_countdown_ = failed_retry_frames;
        } catch (const std::bad_alloc &) {
          reset();
        }
        return std::nullopt;
      }
      try {
        mesh_.emplace(std::move(loaded).value());
        key_.emplace(surface);
        file_ = file;
        failed_ = false;
        retry_countdown_ = 0U;
      } catch (const std::bad_alloc &) {
        reset();
        return std::nullopt;
      }
    }
    if (failed_ || !mesh_.has_value()) {
      return std::nullopt;
    }
    auto mesh_view = output::mesh_perspective_view_for(*mesh_, frame.camera.eye,
                                                       frame.camera.near_plane,
                                                       frame.camera.far_plane);
    if (!mesh_view) {
      return std::nullopt;
    }
    MappedProjection projection{};
    projection.render_view =
        SceneRenderView{mesh_view.value().matrices, mesh_view.value().eye,
                        mesh_view.value().forward};
    projection.borrowed_mesh = &*mesh_;
    projection.mesh_view.emplace(std::move(mesh_view).value());
    return projection;
  }
  }
  return std::nullopt;
}

const rhi::Texture *record_output_mapped_scene(
    CompositorSceneExecutor &executor, ProjectionMeshWarp &mesh_warp,
    OutputProjectionCache &projection_cache, rhi::CommandList &commands,
    const scene::Scene &frame, const output::OutputMap &map,
    bool &mapping_unavailable) noexcept {
  if (map.projection == output::FeedProjection::CanvasSlice) {
    return record_scene(executor, commands, frame, map.source);
  }
  auto projection = projection_cache.resolve(frame, map);
  if (!projection.has_value()) {
    mapping_unavailable = true;
    return nullptr;
  }
  const auto rendered =
      executor.execute(commands, frame, projection->render_view);
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
                        *projection->mesh_view)) {
    return nullptr;
  }
  return &mesh_warp.target();
}

const rhi::Texture *record_output_mapped_scene(
    CompositorSceneExecutor &executor, ProjectionMeshWarp &mesh_warp,
    OutputProjectionCache &projection_cache, rhi::CommandList &commands,
    const compositor::CompositorGraph &graph, const scene::Scene &frame,
    const output::OutputMap &map, bool &mapping_unavailable) noexcept {
  if (map.projection == output::FeedProjection::CanvasSlice) {
    return record_scene(executor, commands, graph, frame, map.source);
  }
  auto projection = projection_cache.resolve(frame, map);
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
                          *projection->mesh_view)) {
      return nullptr;
    }
    return &mesh_warp.target();
  };
  if (!graph.valid() || graph.plan.steps.empty()) {
    const auto rendered =
        executor.execute(commands, frame, projection->render_view);
    return finish(rendered ? rendered.value() : nullptr);
  }
  const auto planned = plan_compositor_scene(graph, frame);
  if (!planned) {
    if (planned.error() == CompositorScenePlanError::MissingSceneLayer) {
      const auto rendered =
          executor.execute(commands, frame, projection->render_view);
      return finish(rendered ? rendered.value() : nullptr);
    }
    return nullptr;
  }
  const auto rendered =
      executor.execute(commands, graph, frame, projection->render_view);
  return finish(rendered ? rendered.value() : nullptr);
}

} // namespace mediaserver::render
