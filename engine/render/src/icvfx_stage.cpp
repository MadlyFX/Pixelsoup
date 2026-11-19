#include <mediaserver/render/icvfx_stage.hpp>

#include <mediaserver/core/idle_report.hpp>

#include <algorithm>
#include <optional>
#include <utility>

namespace mediaserver::render {
namespace {

using Result = core::Result<IcvfxStage, RenderError>;

// The inner render target's extent, from the surface raster and the authored
// plate density. Deliberately *not* from the plate rectangle: the plate moves
// every frame, and a target that followed it would reallocate inside a command
// list.
//
// Sizing to the whole raster is the conservative choice - the plate is never
// larger than the wall it is found on - and it means the effective density
// never falls below what the operator authored, whatever the camera is looking
// at. The raster and not the scene canvas, because that is the space the plate
// rectangle is expressed in.
[[nodiscard]] core::u32 inner_extent(core::u32 raster_extent,
                                     core::u32 numerator,
                                     core::u32 denominator) noexcept {
    const auto scaled =
        (static_cast<core::u64>(raster_extent) * numerator + denominator - 1U) /
        denominator;
    return static_cast<core::u32>(std::clamp<core::u64>(
        scaled, 1U, max_inner_frustum_extent));
}

// The densest plate any camera in this scene asks for. One inner target serves
// every camera, so it has to satisfy the hungriest of them; a camera that asked
// for less is rendered into a corner of it at its own plate size.
[[nodiscard]] std::pair<core::u32, core::u32> inner_target_extent(
    const scene::Scene &frame, core::u32 base_width,
    core::u32 base_height) noexcept {
    core::u32 numerator = 1U;
    core::u32 denominator = 1U;
    for (const scene::TrackedCameraView &view : frame.tracked_cameras) {
        const scene::IcvfxCameraSettings camera = scene::clamped(view.icvfx);
        // Compare as a cross product so the choice never depends on rounding.
        if (static_cast<core::u64>(camera.resolution_numerator) * denominator >
            static_cast<core::u64>(numerator) * camera.resolution_denominator) {
            numerator = camera.resolution_numerator;
            denominator = camera.resolution_denominator;
        }
    }
    return {inner_extent(base_width, numerator, denominator),
            inner_extent(base_height, numerator, denominator)};
}

// The raster extent the plate render is sized from: the wall's raster, or -
// when this output shows its slice of that raster in fewer pixels than the
// raster has - the raster at the output's own density. A plate rendered at the
// full raster of a wall shown at a quarter of it spends four times the pixels
// of everything the output can show, and a full second renderer at an
// 11520-wide raster is gigabytes of video memory: enough, beside the outer
// render, to exhaust a render node's GPU and have the device removed.
//
// Never above the raster, so a flat wall shown at or above its raster keeps
// exactly the plate it had.
[[nodiscard]] core::u32 shown_extent(core::u32 raster, core::u32 slice,
                                     core::u32 shown) noexcept {
    if (shown == 0U || slice == 0U || shown >= slice) {
        return raster;
    }
    const core::u64 scaled =
        (static_cast<core::u64>(raster) * shown + slice - 1U) / slice;
    return static_cast<core::u32>(
        std::clamp<core::u64>(scaled, 1U, raster));
}

// Why a plate render failed, said once per cause. Every cause but media
// adoption fails the frame, so the log is the only place an operator learns
// which step it was.
void report_plate_render_failure(std::optional<RenderError> error) noexcept {
    const char *reason = "the plate render produced no image";
    if (error.has_value()) {
        switch (*error) {
        case RenderError::Unavailable:
            reason = "the plate render is waiting for its media to be "
                     "adopted";
            break;
        case RenderError::ShaderFormatUnsupported:
            reason = "the plate render has no usable shaders";
            break;
        case RenderError::CapabilityUnsupported:
            reason = "the plate render lacks a required renderer capability";
            break;
        case RenderError::InvalidConfiguration:
            reason = "the plate render refused its view as an invalid "
                     "configuration";
            break;
        case RenderError::AllocationFailed:
            reason = "the plate render could not allocate its targets";
            break;
        case RenderError::BackendFailure:
            reason = "the plate render failed in the GPU backend";
            break;
        }
    }
    static_cast<void>(
        core::report_idle_once("inner frustum: plate not recorded", reason));
}

} // namespace

IcvfxStage::IcvfxStage(rhi::Device &device, RendererLimits limits,
                       MediaCacheConfig media_cache,
                       rhi::TextureFormat working_format,
                       IcvfxCompositor first, IcvfxCompositor second) noexcept
    : device_(&device), limits_(limits), media_cache_(media_cache),
      working_format_(working_format),
      compositors_{std::move(first), std::move(second)} {}

Result IcvfxStage::create(rhi::Device &device, core::u32 width,
                          core::u32 height, const RendererLimits &limits,
                          MediaCacheConfig media_cache,
                          rhi::TextureFormat working_format) {
    auto first = IcvfxCompositor::create(device, width, height,
                                         working_format);
    if (!first) {
        return Result::failure(first.error());
    }
    auto second = IcvfxCompositor::create(device, width, height,
                                          working_format);
    if (!second) {
        return Result::failure(second.error());
    }
    return Result::success(IcvfxStage{device, limits, media_cache,
                                      working_format, std::move(first).value(),
                                      std::move(second).value()});
}

bool IcvfxStage::resize(rhi::Device &device, core::u32 width,
                        core::u32 height) noexcept {
    device_ = &device;
    bool resized = true;
    for (IcvfxCompositor &compositor : compositors_) {
        resized = compositor.resize(device, width, height) && resized;
    }
    return resized;
}

IcvfxStats IcvfxStage::stats() const noexcept {
    IcvfxStats total{};
    for (const IcvfxCompositor &compositor : compositors_) {
        const IcvfxStats &one = compositor.stats();
        total.frames_planned += one.frames_planned;
        total.inner_frames_rendered += one.inner_frames_rendered;
        total.inner_frames_skipped += one.inner_frames_skipped;
        total.composites += one.composites;
        total.refusals += one.refusals;
    }
    return total;
}

bool IcvfxStage::ensure_inner(
    const scene::Scene &frame, const output::PhysicalSurface &surface,
    scene::CanvasRect slice, scene::Canvas shown) noexcept {
    if (device_ == nullptr) {
        return false;
    }
    // A high-water mark, so outputs that show their walls at different
    // densities through one stage settle on the largest rather than
    // reallocating the plate renderer between them every frame.
    base_width_ = std::max(
        base_width_,
        shown_extent(surface.raster.width, slice.width, shown.width));
    base_height_ = std::max(
        base_height_,
        shown_extent(surface.raster.height, slice.height, shown.height));
    const auto [width, height] =
        inner_target_extent(frame, base_width_, base_height_);
    if (inner_ && inner_width_ == width && inner_height_ == height) {
        return true;
    }
    if (inner_) {
        // A canvas or density edit is an operator action, not a frame event,
        // so reallocating here is the same class of work as a window resize.
        if (!inner_->resize(*device_, width, height)) {
            return false;
        }
        inner_width_ = width;
        inner_height_ = height;
        return true;
    }
    auto created = CompositorSceneExecutor::create(
        *device_, width, height, limits_, media_cache_, working_format_);
    if (!created) {
        return false;
    }
    inner_.emplace(std::move(created).value());
    inner_width_ = width;
    inner_height_ = height;
    return true;
}

bool IcvfxStage::ensure_plate_warp() noexcept {
    if (device_ == nullptr || !inner_) {
        return false;
    }
    if (plate_warp_) {
        // Same class of work as the inner executor's resize: a canvas or
        // density edit, never a per-frame plate size.
        return plate_warp_->resize(*device_, inner_width_, inner_height_);
    }
    auto created = ProjectionMeshWarp::create(
        *device_, inner_width_, inner_height_, ProjectionMeshWarpLimits{},
        working_format_);
    if (!created) {
        static_cast<void>(core::report_idle_once(
            "inner frustum: plate warp",
            created.error() == RenderError::ShaderFormatUnsupported
                ? "the mesh warp shaders are unavailable, so no plate can be "
                  "drawn on a curved or UV-mesh wall"
                : "the plate warp target could not be allocated"));
        return false;
    }
    plate_warp_.emplace(std::move(created).value());
    return true;
}

const rhi::Texture *IcvfxStage::record(
    rhi::CommandList &commands, const scene::Scene &frame,
    const output::PhysicalSurface &surface, scene::CanvasRect slice,
    std::span<const scene::InnerFrustumRegion> regions,
    const rhi::Texture &outer, MediaCache *shared_media_cache,
    scene::Canvas shown) noexcept {
    return record_stack(commands, frame, surface, nullptr, slice, regions,
                        outer, shared_media_cache, shown);
}

const rhi::Texture *IcvfxStage::record(
    rhi::CommandList &commands, const scene::Scene &frame,
    const output::PhysicalSurface &surface, const IcvfxSurfaceMesh &mesh,
    scene::CanvasRect slice,
    std::span<const scene::InnerFrustumRegion> regions,
    const rhi::Texture &outer, MediaCache *shared_media_cache,
    scene::Canvas shown) noexcept {
    if (mesh.mesh == nullptr) {
        last_plans_.clear();
        static_cast<void>(core::report_idle_once(
            "inner frustum: no plate on this output",
            "the complete surface mesh is not available on this node"));
        return &outer;
    }
    return record_stack(commands, frame, surface, &mesh, slice, regions,
                        outer, shared_media_cache, shown);
}

const rhi::Texture *IcvfxStage::record_stack(
    rhi::CommandList &commands, const scene::Scene &frame,
    const output::PhysicalSurface &surface, const IcvfxSurfaceMesh *mesh,
    scene::CanvasRect slice,
    std::span<const scene::InnerFrustumRegion> regions,
    const rhi::Texture &outer, MediaCache *shared_media_cache,
    scene::Canvas shown) noexcept {
    last_plans_.clear();
    plate_pending_ = false;
    const std::size_t count =
        std::min(frame.tracked_cameras.size(), regions.size());
    last_plans_.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        last_plans_.push_back(plan_inner_frustum(
            frame, frame.tracked_cameras[index], surface, slice,
            regions[index]));
    }

    // Nothing to draw. Hand the caller its own texture back rather than copying
    // it through a compositor: an output that sees no plate should pay nothing,
    // and on a wide wall with long lenses that is most outputs.
    const bool any_plate =
        std::any_of(last_plans_.begin(), last_plans_.end(),
                    [](const IcvfxPlan &plan) noexcept {
                        return plan.active && !plan.skipped;
                    });
    if (!any_plate) {
        // Silent until this existed, and it is the last place a plate can go
        // missing without anyone being told: the caller has already checked
        // everything it can see, so from outside this looks like a working
        // stage that simply drew nothing. The two ways it happens are worth
        // telling apart - a node whose slice misses the plate is the feature
        // working as designed and is most outputs on a wide wall, while a plan
        // that never became active is a rectangle the planner refused.
        const bool skipped_only =
            std::any_of(last_plans_.begin(), last_plans_.end(),
                        [](const IcvfxPlan &plan) noexcept {
                            return plan.active && plan.skipped;
                        });
        static_cast<void>(core::report_idle_once(
            "inner frustum: planned but not drawn",
            skipped_only
                ? "this node's slice misses every plate, so it skips the second render"
                : "the planner refused every plate for this frame"));
        return &outer;
    }
    if (!ensure_inner(frame, surface, slice, shown)) {
        static_cast<void>(core::report_idle_once(
            "inner frustum: plate not recorded",
            "the plate renderer could not be created at the plate density"));
        return nullptr;
    }
    if (mesh != nullptr && !ensure_plate_warp()) {
        return nullptr;
    }

    // Bottom of the stack first. `tracked_cameras` is already in composite
    // order, so this loop is the whole of "bigger render order draws over" -
    // and because that order is part of the replicated scene, two nodes
    // building the same stack build it the same way round.
    const rhi::Texture *source = &outer;
    std::size_t next_compositor = 0U;
    for (std::size_t index = 0U; index < last_plans_.size(); ++index) {
        const IcvfxPlan &plan = last_plans_[index];
        if (!plan.active || plan.skipped) {
            continue;
        }
        // The plate texture the compositor reads: a render that covers the
        // plate rectangle exactly, in either wall shape.
        const rhi::Texture *plate = nullptr;
        if (mesh == nullptr) {
            // The plate: an off-axis perspective from this camera's tracked
            // eye through its rectangle on the wall, built by the same call
            // the outer frustum uses.
            //
            // A surface that refuses the eye - behind the wall, or coplanar
            // with it - leaves this camera out of the stack rather than
            // failing the frame. The refusal is derived entirely from
            // replicated state, so every node drops the same camera on the
            // same frame, and the plates below it still composite.
            const auto view = inner_frustum_view(
                frame, frame.tracked_cameras[index], surface, plan);
            if (!view.has_value()) {
                continue;
            }
            const auto rendered =
                inner_->execute(commands, frame, *view, shared_media_cache);
            if (!rendered && rendered.error() == RenderError::Unavailable) {
                // Still adopting this frame's media - see `plate_pending`.
                plate_pending_ = true;
                report_plate_render_failure(rendered.error());
                continue;
            }
            if (!rendered || rendered.value() == nullptr) {
                report_plate_render_failure(rendered ? std::nullopt
                                                     : std::optional{rendered.error()});
                return nullptr;
            }
            plate = rendered.value();
        } else {
            // A curved or UV-mesh wall: one render from the tracked eye fitted
            // to the wall under the rectangle, then warped through those same
            // triangles into the rectangle. Refused the same way a flat wall
            // refuses an eye behind it - dropped, reported, the rest drawn.
            const auto view = inner_frustum_mesh_view(
                frame, frame.tracked_cameras[index], surface, *mesh, plan);
            if (!view) {
                const char *reason =
                    "no plate view fits the wall under the plate";
                switch (view.error()) {
                case output::PerspectiveMappingError::MeshBehindEye:
                    reason = "part of the wall under the plate is behind the "
                             "tracked camera";
                    break;
                case output::PerspectiveMappingError::DegenerateFrustum:
                    reason = "no part of the surface mesh lies under the "
                             "plate rectangle";
                    break;
                case output::PerspectiveMappingError::InvalidClipRange:
                    reason = "the tracked camera's clip range is not valid";
                    break;
                case output::PerspectiveMappingError::InvalidEye:
                    reason = "the tracked camera's eye is not valid";
                    break;
                case output::PerspectiveMappingError::InvalidMesh:
                    reason = "the surface mesh has unusable vertices";
                    break;
                case output::PerspectiveMappingError::InvalidSurface:
                    reason = "the surface placement or mesh scale is not "
                             "valid";
                    break;
                default:
                    break;
                }
                static_cast<void>(core::report_idle_once(
                    "inner frustum: plate dropped on a mesh wall", reason));
                continue;
            }
            if (frame.renderer_settings.plate_depth_of_field
                && frame.renderer_settings.plate_dof_radius > 0U) {
                static_cast<void>(core::report_idle_once(
                    "plate depth of field",
                    "not applied on a curved or UV-mesh wall, which has no "
                    "single wall plane to subtract; the plate is drawn "
                    "without it"));
            }
            const auto rendered = inner_->execute(
                commands, frame, view.value().render, shared_media_cache);
            if (!rendered && rendered.error() == RenderError::Unavailable) {
                plate_pending_ = true;
                report_plate_render_failure(rendered.error());
                continue;
            }
            if (!rendered || rendered.value() == nullptr) {
                report_plate_render_failure(rendered ? std::nullopt
                                                     : std::optional{rendered.error()});
                return nullptr;
            }
            const output::MeshDestinationWindow window{surface.raster,
                                                       plan.inner_rect};
            if (!plate_warp_->render(commands, *rendered.value(), *mesh->mesh,
                                     view.value().mesh_view, mesh->placement,
                                     &window)) {
                return nullptr;
            }
            plate = &plate_warp_->target();
        }
        // Alternating, so this composite never samples the target it writes.
        IcvfxCompositor &compositor = compositors_[next_compositor];
        next_compositor = 1U - next_compositor;
        if (!compositor.render(commands, plan, slice, *source, plate)) {
            static_cast<void>(core::report_idle_once(
                "inner frustum: plate not recorded",
                "the plate composite was refused"));
            return nullptr;
        }
        source = &compositor.target();
    }
    return source;
}

} // namespace mediaserver::render
