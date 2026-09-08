#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/froxel_grid.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/render/slice_apron.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

// Volumetric fog: the air between the camera and the set, and the light in it.
//
// On a volume this earns its cost twice. It gives a set depth and scale, which
// is what a flat backdrop most obviously lacks - and it softens the far
// distance, which hides the resolution limit of background content exactly
// where a static plate would otherwise betray itself.
//
// Three stages over one froxel volume: scatter per cell, integrate front to
// back along each ray, resolve at each pixel's own depth. Where the cells are
// is `froxel_grid.hpp`'s problem and it is the part that had to be right first.
//
// **Every node computes the whole grid.** 160x90x64 is under a million cells;
// computing the identical volume everywhere removes the coverage bookkeeping
// from the frame path and leaves no arithmetic for two nodes to disagree about.
// `plan_froxel_grid` still reports coverage and restricting the dispatch to it
// remains available as a pure optimisation, but correctness does not depend on
// it and this class does not take it.
namespace mediaserver::render {

struct VolumetricFogStats final {
    core::u64 volumes{};
    core::u64 dispatches{};
    // Frames that authored fog and did not get it, because the grid or the
    // targets did not describe the same thing. Counted rather than skipped:
    // fog missing on one node and present on its neighbour is a seam down the
    // middle of every deep shot.
    core::u64 refusals{};

    bool operator==(const VolumetricFogStats&) const = default;
};

// Which matrix the volume is keyed to for one frame, and which raster.
//
// The one derivation of it, shared by the renderer and the compositor so the
// two cannot drift. An ordinary wall slice takes the authored canvas, the
// canvas-wide matrix and the padded rectangle: the grid partitions the
// full-canvas frustum and a world point falls in the same cell on every node.
//
// A frame rendered from an **explicit view** - the ICVFX inner frustum, which
// is off-axis from the tracked camera's eye, or a projection-mapped surface -
// takes that view's own matrix and its own raster. The depth this pass
// unprojects was written by that projection, and unprojecting it with the
// authored camera's matrix recovers a distance belonging to no camera at all:
// the plate would carry fog computed for a view it was not rendered from, and
// the compositor would feather that against a correctly fogged outer frustum.
// Refusing is not an option either - it would fail every ICVFX frame that
// authors atmosphere - so the view is what decides.
struct FogFrame final {
    scene::Mat4 view_projection{};
    scene::Canvas grid_canvas{};
    scene::CanvasRect render{};
    SceneClipPlanes clip_planes{};

    bool operator==(const FogFrame&) const = default;
};

[[nodiscard]] FogFrame fog_frame_for(const scene::Scene& scene,
                                     const SceneRenderView* view,
                                     const PaddedSlice& padded,
                                     core::u32 target_width,
                                     core::u32 target_height) noexcept;

class VolumetricFog final {
public:
    [[nodiscard]] static bool shaders_available() noexcept;

    // `target_format` is the format of the fogged frame this pass publishes.
    // Scene-linear RGBA16F everywhere it matters; a compositor working in
    // another format asks for its own so the frame can be cropped out of the
    // result without a conversion pass. The volume itself is always RGBA16F.
    [[nodiscard]] static core::Result<VolumetricFog, RenderError> create(
        rhi::Device& device, core::u32 width, core::u32 height,
        const FroxelGridLimits& limits,
        rhi::TextureFormat target_format =
            rhi::TextureFormat::Rgba16Float) noexcept;

    VolumetricFog(const VolumetricFog&) = delete;
    VolumetricFog& operator=(const VolumetricFog&) = delete;
    VolumetricFog(VolumetricFog&&) = default;
    VolumetricFog& operator=(VolumetricFog&&) = default;
    ~VolumetricFog() = default;

    [[nodiscard]] core::u32 width() const noexcept { return target_.width(); }
    [[nodiscard]] core::u32 height() const noexcept { return target_.height(); }

    // The lit frame with the atmosphere composited onto it.
    [[nodiscard]] const rhi::Texture& target() const noexcept {
        return target_;
    }

    [[nodiscard]] bool resize(rhi::Device& device, core::u32 width,
                              core::u32 height) noexcept;

    // Records all three stages. An explicit `clip_planes` supplies the depth
    // range of a projection that differs from the authored camera's range.
    //
    // `view_projection` is the matrix the frame was actually rendered from,
    // covering the whole of `grid_canvas` - and `grid_canvas` is the raster
    // that frustum fills, with `render` this node's rectangle inside it.
    //
    // For an ordinary wall slice that is the authored canvas, the canvas-wide
    // matrix, and the padded rectangle: the grid partitions the full-canvas
    // frustum and a world point falls in the same cell on every node.
    //
    // For a frame rendered from an **explicit view** - the ICVFX inner frustum,
    // which is off-axis from the tracked camera's eye, or a projection-mapped
    // surface - it is that view's own matrix and its own raster. Passing the
    // authored camera's matrix there unprojects a depth that was written by a
    // different frustum, and the recovered distance is meaningless; the plate
    // would carry fog computed for a camera it was not rendered from, and the
    // compositor would feather that against a correctly fogged outer frustum.
    // Refusing instead is not an option - it would fail every ICVFX frame that
    // authors atmosphere.
    // `lights` and `light_shadows` are the buffers the lit draw uploaded, and
    // `shadow_atlas` the tiles the casting ones got. The air is lit by exactly
    // the lights that lit the surfaces, with the same falloff and the same
    // occlusion - a second implementation of either would drift, and the drift
    // would read as fog that does not belong to the lamp it came from.
    [[nodiscard]] bool record(rhi::CommandList& commands,
                              const rhi::Texture& color,
                              const rhi::Texture& depth,
                              const rhi::Texture& shadow_atlas,
                              const rhi::StorageBuffer& lights,
                              const rhi::StorageBuffer& light_shadows,
                              core::u32 light_count,
                              const scene::Scene& scene,
                              const scene::Mat4& view_projection,
                              scene::Canvas grid_canvas,
                              scene::CanvasRect render,
                              std::optional<SceneClipPlanes> clip_planes =
                                  std::nullopt,
                              const rhi::Texture* environment = nullptr) noexcept;

    [[nodiscard]] const VolumetricFogStats& stats() const noexcept {
        return stats_;
    }
    void reset_stats() noexcept { stats_ = {}; }

private:
    VolumetricFog(FroxelGridLimits limits, rhi::Texture scatter,
                  rhi::Texture integrated, rhi::Texture target,
                  rhi::ComputePipeline scatter_pipeline,
                  rhi::ComputePipeline integrate_pipeline,
                  rhi::ComputePipeline apply_pipeline,
                  rhi::Sampler sampler,
                  rhi::TextureFormat target_format) noexcept;

    FroxelGridLimits limits_{};
    rhi::TextureFormat target_format_{rhi::TextureFormat::Rgba16Float};
    // The volume, as a 2D atlas of depth slices. Not a 3D texture: the
    // interpolation is manual for determinism, and once it is manual a 3D
    // texture buys nothing and costs an RHI change - the same conclusion step
    // 10 reached about the display table.
    rhi::Texture scatter_;
    rhi::Texture integrated_;
    rhi::Texture target_;
    rhi::ComputePipeline scatter_pipeline_;
    rhi::ComputePipeline integrate_pipeline_;
    rhi::ComputePipeline apply_pipeline_;
    rhi::Sampler sampler_;
    VolumetricFogStats stats_{};
};

} // namespace mediaserver::render
