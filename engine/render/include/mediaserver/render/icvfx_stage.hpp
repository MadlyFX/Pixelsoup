#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/compositor_scene_executor.hpp>
#include <mediaserver/render/icvfx.hpp>
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
           MediaCache *shared_media_cache = nullptr) noexcept;

    // The plans this output produced last frame, one per tracked camera, in
    // composite order. Empty when the scene has no tracked cameras.
    [[nodiscard]] std::span<const IcvfxPlan> last_plans() const noexcept {
        return last_plans_;
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
        const scene::Scene &frame,
        const output::PhysicalSurface &surface) noexcept;

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
    core::u32 inner_width_{};
    core::u32 inner_height_{};
    std::vector<IcvfxPlan> last_plans_;
};

} // namespace mediaserver::render
