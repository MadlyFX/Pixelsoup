#pragma once
#include <mediaserver/render/frame_graph.hpp>

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/output/perspective_mapping.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/rhi/device.hpp>

// nDisplay-style projective mesh warp.
//
// The scene is rendered once with output::mesh_perspective_view_for(). This
// pass then rasterizes the physical surface in its destination UV0 space and
// samples that scene render through the physical position's projected clip
// coordinate. A curved wall therefore needs one generalized view plus this
// warp, not a manually-aligned camera for every LED column.
namespace mediaserver::render {

struct ProjectionMeshWarpLimits final {
    core::u32 max_vertices{65'536U};
    core::u32 max_indices{196'608U};

    bool operator==(const ProjectionMeshWarpLimits&) const = default;
};

class ProjectionMeshWarp final {
public:
    [[nodiscard]] static bool shaders_available() noexcept;

    [[nodiscard]] static core::Result<ProjectionMeshWarp, RenderError> create(
        rhi::Device& device,
        core::u32 width,
        core::u32 height,
        ProjectionMeshWarpLimits limits = {},
        rhi::TextureFormat format = rhi::TextureFormat::Rgba8Unorm);

    ProjectionMeshWarp(const ProjectionMeshWarp&) = delete;
    ProjectionMeshWarp& operator=(const ProjectionMeshWarp&) = delete;
    ProjectionMeshWarp(ProjectionMeshWarp&&) = default;
    ProjectionMeshWarp& operator=(ProjectionMeshWarp&&) = default;
    ~ProjectionMeshWarp() = default;

    [[nodiscard]] rhi::Texture& target() noexcept { return color_; }
    [[nodiscard]] const rhi::Texture& target() const noexcept { return color_; }
    [[nodiscard]] core::u32 width() const noexcept { return color_.width(); }
    [[nodiscard]] core::u32 height() const noexcept { return color_.height(); }
    [[nodiscard]] const ProjectionMeshWarpLimits& limits() const noexcept {
        return limits_;
    }

    [[nodiscard]] bool resize(rhi::Device& device,
                              core::u32 width,
                              core::u32 height) noexcept;

    // Records a black-cleared connector image. The source is the one
    // asymmetric scene render described by `view`; uncovered destination UVs
    // intentionally stay black. `placement` is required only when `mesh`
    // retains imported file-local positions. `window`, when given, maps that
    // rectangle of the mesh's destination raster onto the whole target instead
    // of UV0 0..1 - how a tracked plate is warped into its own rectangle on a
    // curved wall. False is a validation/capacity/backend error.
    [[nodiscard]] bool render(rhi::CommandList& commands,
                              const rhi::Texture& source,
                              const output::ProjectionMesh& mesh,
                              const output::MeshPerspectiveView& view,
                              const output::ProjectionMeshPlacement* placement =
                                  nullptr,
                              const output::MeshDestinationWindow* window =
                                  nullptr) noexcept;

private:
    FrameGraph graph_;
    ProjectionMeshWarp(ProjectionMeshWarpLimits limits,
                       rhi::Texture color,
                       rhi::Texture depth,
                       rhi::GraphicsPipeline pipeline,
                       rhi::Sampler sampler,
                       rhi::StorageBuffer vertices,
                       rhi::StorageBuffer indices) noexcept;

    ProjectionMeshWarpLimits limits_{};
    rhi::Texture color_;
    rhi::Texture depth_;
    rhi::GraphicsPipeline pipeline_;
    rhi::Sampler sampler_;
    rhi::StorageBuffer vertices_;
    rhi::StorageBuffer indices_;
};

} // namespace mediaserver::render
