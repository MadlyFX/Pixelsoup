#pragma once

#include <mediaserver/compositor/compositor_model.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/render/compositor_scene_executor.hpp>
#include <mediaserver/render/projection_mesh_warp.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/scene.hpp>

#include <filesystem>
#include <optional>

// Shared output-mapping render path.
//
// Both a render node and the editor's Program Output must resolve physical
// surfaces, imported UV meshes, and compositor graphs identically. Keeping
// that work here prevents the operator preview from drifting into a plain
// camera view while the connector is rendering an off-axis/warped image.
namespace mediaserver::render {

struct ProjectionMeshFileFingerprint final {
  std::uintmax_t size{};
  std::filesystem::file_time_type modified{};

  bool operator==(const ProjectionMeshFileFingerprint &) const = default;
};

struct MappedProjection final {
  SceneRenderView render_view{};
  std::optional<output::ProjectionMesh> owned_mesh;
  const output::ProjectionMesh *borrowed_mesh{};
  std::optional<output::MeshPerspectiveView> mesh_view;

  [[nodiscard]] const output::ProjectionMesh *projection_mesh() const noexcept;
};

class OutputProjectionCache final {
public:
  [[nodiscard]] std::optional<MappedProjection>
  resolve(const scene::Scene &frame, const output::OutputMap &map) noexcept;

  void reset() noexcept;

private:
  std::optional<output::ResolvedProjectionSurface> key_;
  std::optional<ProjectionMeshFileFingerprint> file_;
  std::optional<output::ProjectionMesh> mesh_;
  bool failed_{};
  core::u32 retry_countdown_{};
};

// Records the exact image before the final window-sized copy. Canvas feeds
// render their authored crop; physical feeds render an asymmetric view and,
// when needed, project it through the generated/imported UV mesh.
[[nodiscard]] const rhi::Texture *record_output_mapped_scene(
    CompositorSceneExecutor &executor, ProjectionMeshWarp &mesh_warp,
    OutputProjectionCache &projection_cache, rhi::CommandList &commands,
    const scene::Scene &frame, const output::OutputMap &map,
    bool &mapping_unavailable) noexcept;

[[nodiscard]] const rhi::Texture *record_output_mapped_scene(
    CompositorSceneExecutor &executor, ProjectionMeshWarp &mesh_warp,
    OutputProjectionCache &projection_cache, rhi::CommandList &commands,
    const compositor::CompositorGraph &graph, const scene::Scene &frame,
    const output::OutputMap &map, bool &mapping_unavailable) noexcept;

} // namespace mediaserver::render
