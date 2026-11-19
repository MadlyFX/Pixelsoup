#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/compositor_scene_executor.hpp>
#include <mediaserver/render/icvfx.hpp>
#include <mediaserver/render/projection_mesh_warp.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/scene.hpp>

#include <array>
#include <optional>
#include <span>
#include <vector>

// The extra renders per output, and the composites that join them.
//
// `IcvfxPlan` says what to draw for one camera and `IcvfxCompositor` joins two
// finished textures; this is the piece in between that actually produces the
// plates. It owns a complete second scene executor so a plate is drawn with the
// same effects, masks, and colour path as the wall around it - a plate that
// skipped the show's grade would be worse than no plate at all.
//
// That second executor is the honest cost of the feature: one extra full render
// per tracked camera whose plate this output can see.
//
// A stage with several cameras composites their plates in the scene's order,
// lowest render order first, so the last one listed ends up on top. The two
// compositors alternate: each reads the previous one's result and writes its
// own, which is what lets a stack of any depth be built without a compositor
// ever sampling the target it is writing.
namespace mediaserver::render {

class IcvfxStage final {
public:
    [[nodiscard]] static bool shaders_available() noexcept {
        return IcvfxCompositor::shaders_available() &&
            CompositorSceneExecutor::shaders_available();
    }

    // `width` and `height` are the composite target: the same extent as the
    // outer image this stage will be handed. The inner render target is sized
    // separately, from the authored canvas and plate density, the first time a
    // plate is actually drawn.
    [[nodiscard]] static core::Result<IcvfxStage, RenderError>
    create(rhi::Device &device, core::u32 width, core::u32 height,
           const RendererLimits &limits = RendererLimits{},
           MediaCacheConfig media_cache = MediaCacheConfig{},
           rhi::TextureFormat working_format = rhi::TextureFormat::Rgba8Unorm);

    IcvfxStage(const IcvfxStage &) = delete;
    IcvfxStage &operator=(const IcvfxStage &) = delete;
    IcvfxStage(IcvfxStage &&) = default;
    IcvfxStage &operator=(IcvfxStage &&) = default;
    ~IcvfxStage() = default;

    [[nodiscard]] bool resize(rhi::Device &device, core::u32 width,
                              core::u32 height) noexcept;

    // Draws the inner frustum and composites it over `outer`.
    //
    // `shown` is the extent in output pixels the caller displays `slice` at
    // (an output map's viewport). The plate renderer is sized from the wall
    // raster at that density, never above the raster itself; zero means the
    // raster. See `shown_extent` in icvfx_stage.cpp for why it matters.
    //
    // Returns `&outer` unchanged - not a copy - whenever there is no plate for
    // this output to draw: no tracking, no authored inner frustum, or a slice
    // that misses the rectangle entirely. That last case is the whole
    // performance argument for ICVFX, and returning the caller's own texture
    // means a node that skips pays nothing at all, not even a blit.
    //
    // `surface` is the wall this output is showing. It fixes the frame of
    // reference for everything here: `surface.raster` is the space `slice` and
    // `regions` are expressed in, and its geometry is what each plate's view is
    // built through. One surface per call, because one output shows one wall.
    //
    // `regions` carries one inner-frustum rectangle per entry in the scene's
    // `tracked_cameras`, in the same order and in `surface.raster` pixels. The
    // caller derives them, because only the caller knows which wall this output
    // is showing. A shorter span draws only the cameras it covers.
    //
    // A null return is a real failure - a refused command, an exhausted target
    // - and the caller must not present a frame for it. A camera whose eye the
    // surface refuses is dropped from the stack instead, which is not a
    // failure: the remaining plates still composite.
    [[nodiscard]] const rhi::Texture *
    record(rhi::CommandList &commands, const scene::Scene &frame,
           const output::PhysicalSurface &surface, scene::CanvasRect slice,
           std::span<const scene::InnerFrustumRegion> regions,
           const rhi::Texture &outer,
           MediaCache *shared_media_cache = nullptr,
           scene::Canvas shown = scene::Canvas{0U, 0U}) noexcept;

    // The same stack on a wall that is not flat.
    //
    // `outer` is the mesh-warped wall image this output already drew, which
    // is in `surface.raster` pixels over `slice` exactly as a flat wall's is -
    // so the composite, the feather and the skip decision are unchanged. What
    // differs is how each plate is made: `inner_frustum_mesh_view` fits one
    // render from the tracked eye to the triangles of `mesh` under the plate
    // rectangle, and the stage's own warp projects that render through the
    // same triangles into the rectangle before it is composited.
    //
    // `regions` must come from the mesh form of `output::inner_frustum_region`
    // over the same complete `mesh`. A camera whose plate view the mesh
    // refuses - wall behind the tracked eye under the plate - is dropped from
    // the stack and reported, as a flat wall drops an eye behind it. Null only
    // for a real failure: a refused command, a target that could not be made.
    [[nodiscard]] const rhi::Texture *
    record(rhi::CommandList &commands, const scene::Scene &frame,
           const output::PhysicalSurface &surface, const IcvfxSurfaceMesh &mesh,
           scene::CanvasRect slice,
           std::span<const scene::InnerFrustumRegion> regions,
           const rhi::Texture &outer,
           MediaCache *shared_media_cache = nullptr,
           scene::Canvas shown = scene::Canvas{0U, 0U}) noexcept;

    // The plans this output produced last frame, one per tracked camera, in
    // composite order. Empty when the scene has no tracked cameras.
    [[nodiscard]] std::span<const IcvfxPlan> last_plans() const noexcept {
        return last_plans_;
    }
    // A plate this stage owed last call but could not render because its own
    // renderer was still adopting the frame's media (a new asset generation:
    // every media source is re-opened before anything is drawn). That plate
    // was left off and the rest of the stack drawn, rather than failing the
    // output - and the presenter reports the frame incomplete, so a buffered
    // output holds its last complete frame instead of showing one without the
    // plate. Any other refusal of a plate render still fails the call.
    [[nodiscard]] bool plate_pending() const noexcept {
        return plate_pending_;
    }
    // Summed across the compositors, so a stack of plates reports the work it
    // actually did rather than whichever half landed on one of them.
    [[nodiscard]] IcvfxStats stats() const noexcept;
    // Present only once a plate has actually been drawn. Exposed so a presenter
    // can report the plate's media residency beside the outer image's.
    [[nodiscard]] const CompositorSceneExecutor *inner() const noexcept {
        return inner_ ? &*inner_ : nullptr;
    }

private:
    IcvfxStage(rhi::Device &device, RendererLimits limits,
               MediaCacheConfig media_cache, rhi::TextureFormat working_format,
               IcvfxCompositor first, IcvfxCompositor second) noexcept;

    // Allocates or resizes the inner executor for this wall's raster and the
    // authored density. Never called with a per-frame plate size: the plate
    // moves every frame, and a target that reallocated with it would be an
    // allocation in the middle of a command list.
    [[nodiscard]] bool ensure_inner(
        const scene::Scene &frame, const output::PhysicalSurface &surface,
        scene::CanvasRect slice, scene::Canvas shown) noexcept;
    // The plate warp, at the inner target's extent. Made the first time a
    // plate is drawn on a mesh wall, alongside the inner executor, so a show
    // on flat walls never pays for it.
    [[nodiscard]] bool ensure_plate_warp() noexcept;

    [[nodiscard]] const rhi::Texture *
    record_stack(rhi::CommandList &commands, const scene::Scene &frame,
                 const output::PhysicalSurface &surface,
                 const IcvfxSurfaceMesh *mesh, scene::CanvasRect slice,
                 std::span<const scene::InnerFrustumRegion> regions,
                 const rhi::Texture &outer,
                 MediaCache *shared_media_cache,
                 scene::Canvas shown) noexcept;

    rhi::Device *device_{};
    RendererLimits limits_{};
    MediaCacheConfig media_cache_{};
    rhi::TextureFormat working_format_{rhi::TextureFormat::Rgba8Unorm};
    // Two, alternating. A compositor cannot read the texture it writes, so a
    // stack deeper than one plate needs somewhere to have put the last result.
    std::array<IcvfxCompositor, 2U> compositors_;
    // Constructed the first time a plate is drawn rather than at create, so a
    // show with no tracking never pays for a second renderer.
    std::optional<CompositorSceneExecutor> inner_;
    std::optional<ProjectionMeshWarp> plate_warp_;
    core::u32 inner_width_{};
    core::u32 inner_height_{};
    // The raster extent the plate renderer is sized from; see ensure_inner.
    core::u32 base_width_{};
    core::u32 base_height_{};
    std::vector<IcvfxPlan> last_plans_;
    bool plate_pending_{};
};

} // namespace mediaserver::render
