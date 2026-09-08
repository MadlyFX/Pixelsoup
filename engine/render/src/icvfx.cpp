#include <mediaserver/render/icvfx.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/icvfx_composite_frag_dxil.h>
#include <mediaserver/shaders/output_vert_dxil.h>
#endif

#include <algorithm>
#include <array>
#include <span>
#include <utility>

namespace mediaserver::render {
namespace {

using Result = core::Result<IcvfxCompositor, RenderError>;

struct ShaderBlobs final {
    std::span<const core::u8> vertex;
    std::span<const core::u8> fragment;
    rhi::ShaderFormat format{rhi::ShaderFormat::Dxil};
    bool present{};
};

[[nodiscard]] ShaderBlobs shader_blobs() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return ShaderBlobs{
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_output_vert_dxil),
            sizeof(mediaserver_output_vert_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_icvfx_composite_frag_dxil),
            sizeof(mediaserver_icvfx_composite_frag_dxil)},
        rhi::ShaderFormat::Dxil, true};
#else
    return ShaderBlobs{};
#endif
}

[[nodiscard]] rhi::TextureDesc color_desc(core::u32 width,
                                          core::u32 height,
                                          rhi::TextureFormat format) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.color_target = true;
    desc.sampled = true;
    return desc;
}

// Mirrors IcvfxConstants in icvfx_composite.frag.hlsl, field for field.
struct alignas(16) IcvfxUniform final {
    std::array<float, 2U> slice_origin{};
    std::array<float, 2U> canvas_per_pixel{};
    std::array<float, 2U> inner_origin{};
    std::array<float, 2U> inner_size{};
    float feather{};
    float enabled{};
    std::array<float, 2U> padding{};
};

static_assert(sizeof(IcvfxUniform) == 48U);

[[nodiscard]] std::span<const core::u8> bytes(
    const IcvfxUniform& value) noexcept {
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(&value), sizeof(value)};
}

[[nodiscard]] IcvfxUniform uniform_for(const IcvfxPlan& plan,
                                       scene::CanvasRect slice,
                                       core::u32 target_width,
                                       core::u32 target_height) noexcept {
    IcvfxUniform uniform{};
    uniform.slice_origin = {static_cast<float>(slice.x),
                            static_cast<float>(slice.y)};
    // Exactly one whenever a node renders its slice at its slice resolution,
    // which is what keeps the canvas coordinate in the shader an exact
    // reconstruction. The general form is here so an output at a different
    // resolution is still correct, just not bit-exact against a full-canvas
    // reference.
    uniform.canvas_per_pixel = {
        target_width == slice.width
            ? 1.0F
            : static_cast<float>(slice.width)
                / static_cast<float>(target_width),
        target_height == slice.height
            ? 1.0F
            : static_cast<float>(slice.height)
                / static_cast<float>(target_height),
    };
    uniform.inner_origin = {static_cast<float>(plan.inner_rect.x),
                            static_cast<float>(plan.inner_rect.y)};
    uniform.inner_size = {static_cast<float>(plan.inner_rect.width),
                          static_cast<float>(plan.inner_rect.height)};
    uniform.feather = static_cast<float>(plan.soft_edge_px);
    uniform.enabled = plan.active && !plan.skipped ? 1.0F : 0.0F;
    return uniform;
}

// The same edge test the shader performs, and deliberately the same shape:
// this is the seam two nodes have to agree on.
[[nodiscard]] float edge_distance(float value, float low,
                                  float high) noexcept {
    return std::min(value - low, high - value);
}

[[nodiscard]] float smoothstep_unit(float t) noexcept {
    const float clamped = std::clamp(t, 0.0F, 1.0F);
    return clamped * clamped * (3.0F - 2.0F * clamped);
}

} // namespace

IcvfxPlan plan_inner_frustum(
    const scene::Scene& scene,
    const scene::TrackedCameraView& view,
    const output::PhysicalSurface& surface,
    scene::CanvasRect slice,
    const scene::InnerFrustumRegion& region) noexcept {
    IcvfxPlan plan{};
    plan.camera = view.camera;
    // Two switches, and both have to be on. The show-wide one takes every
    // plate off the wall at once, which is what an operator reaches for
    // between setups; the camera's own decides whether this camera was ever
    // going to draw one.
    const scene::IcvfxSettings settings =
        scene::clamped(scene.renderer_settings.icvfx);
    const scene::IcvfxCameraSettings camera = scene::clamped(view.icvfx);
    // Bounded against the surface raster, which is the space the caller
    // derived `region` and `slice` in. Gating on the scene canvas instead
    // would refuse a perfectly good plate on any wall whose raster is larger
    // than the canvas, and admit an out-of-range one on any wall smaller.
    if (!settings.inner_frustum || !camera.inner_frustum || !view.bound()
        || !view.pose.valid || !region.valid || !output::valid(surface)
        || !scene::valid(surface.raster)
        || !scene::contains(surface.raster, region.rect)) {
        return plan;
    }

    plan.active = true;
    plan.inner_rect = region.rect;
    plan.clamped = region.clamped;
    // Half the shorter side is the widest feather that still leaves the middle
    // of the plate at full strength; a wider one would never reach weight 1.
    const core::u32 half_extent =
        std::min(region.rect.width, region.rect.height) / 2U;
    plan.soft_edge_px = std::min(camera.soft_edge_px, half_extent);

    plan.covered = scene::intersection(slice, region.rect);
    if (plan.covered.width == 0U || plan.covered.height == 0U) {
        // The whole performance argument for ICVFX: a node whose slice misses
        // the rectangle does the second render not at all.
        plan.skipped = true;
        return plan;
    }

    // The inner render covers the complete rectangle, not this node's share of
    // it. Two nodes that split the rectangle must sample one image, or the
    // seam between their halves would be a seam between two renders.
    const auto scaled = [&](core::u32 extent) noexcept {
        const auto numerator =
            static_cast<core::u64>(camera.resolution_numerator);
        const auto denominator =
            static_cast<core::u64>(camera.resolution_denominator);
        const core::u64 value =
            (static_cast<core::u64>(extent) * numerator + denominator - 1U)
            / denominator;
        return static_cast<core::u32>(
            std::min<core::u64>(value, max_inner_frustum_extent));
    };
    plan.inner_width = scaled(region.rect.width);
    plan.inner_height = scaled(region.rect.height);
    if (plan.inner_width == 0U || plan.inner_height == 0U) {
        plan.active = false;
        plan.skipped = false;
        plan.inner_width = 0U;
        plan.inner_height = 0U;
        return plan;
    }
    return plan;
}

scene::Camera tracked_stage_camera(
    const scene::TrackedCameraView& view) noexcept {
    return scene::tracked_view_camera(view.authored, view.pose, view.encoders);
}

std::optional<SceneRenderView> inner_frustum_view(
    const scene::Scene& scene, const scene::TrackedCameraView& tracked_view,
    const output::PhysicalSurface& surface, const IcvfxPlan& plan) noexcept {
    if (!plan.active || plan.skipped) {
        return std::nullopt;
    }
    const scene::Camera tracked = tracked_stage_camera(tracked_view);
    // Through the wall, from the tracked eye. The same call the outer frustum
    // is built with, given this camera's eye and the plate's rectangle instead
    // of the authored eye and this node's slice - so the two renders agree
    // about where every wall pixel is in the world, which is the only reason
    // they can meet in a feather without a step.
    //
    // The lens does not enter here. It has already moved the eye to the
    // entrance pupil in `tracked_stage_camera`, and the projection itself is
    // fixed by the wall rectangle: what the camera sees of the plate is
    // decided by where the plate is, not by the camera's own field of view.
    // The lens sizes the rectangle; the wall builds the frustum.
    const auto built = output::perspective_view_for(
        surface, plan.inner_rect, tracked.eye, tracked.near_plane,
        tracked.far_plane);
    if (!built) {
        // An eye behind the wall, or a degenerate rectangle. Refusing is
        // correct: there is no honest plate for this frame, and every node
        // given the same replicated pose refuses identically.
        return std::nullopt;
    }
    SceneRenderView view{};
    view.matrices = built.value().matrices;
    view.eye = built.value().eye;
    view.forward = built.value().forward;
    // Stable by construction. See the header: a pose-derived identity would
    // reset temporal history on every frame the camera moved, which is every
    // frame that matters.
    core::u64 identity = 0x4943'5646'5849'4E4EULL; // "ICVFXINN"
    identity ^= scene.id.value() + 0x9E37'79B9'7F4A'7C15ULL
        + (identity << 6U) + (identity >> 2U);
    // The tracked camera node, so each camera keeps its own temporal history
    // and swapping which physical camera is live is the cut it actually is.
    identity ^= tracked_view.camera.value() + 0x9E37'79B9'7F4A'7C15ULL
        + (identity << 6U) + (identity >> 2U);
    view.history_identity = identity;
    view.clip_planes = SceneClipPlanes{tracked.near_plane, tracked.far_plane};
    view.guard_band = true;
    view.shutter_seconds = tracked.shutter_seconds;
    PlateLens lens{};
    lens.focal_length_m = tracked_view.pose.focal_length_mm * 0.001F;
    if (lens.focal_length_m == 0.0F && tracked.lens_profile)
        lens.focal_length_m = scene::evaluate_lens(*tracked.lens_profile, tracked_view.encoders).focal_length_mm * 0.001F;
    lens.focus_distance_m = tracked_view.pose.focus_distance_m > 0.0F
        ? tracked_view.pose.focus_distance_m : tracked.focus_distance_m;
    lens.aperture_f_number = tracked_view.pose.aperture_f_number > 0.0F
        ? tracked_view.pose.aperture_f_number : tracked.aperture_f_number;
    lens.eye = tracked.eye;
    lens.forward = scene::normalize(tracked.target - tracked.eye);
    lens.right = scene::normalize(scene::cross(lens.forward, tracked.up));
    lens.up = scene::normalize(scene::cross(lens.right, lens.forward));
    lens.wall_right = scene::normalize(surface.right);
    lens.wall_normal = scene::normalize(scene::cross(lens.wall_right, surface.up));
    lens.wall_up = scene::normalize(scene::cross(lens.wall_normal, lens.wall_right));
    lens.wall_plane_distance = scene::dot(lens.wall_normal, surface.center - lens.eye);
    lens.region_width_m = surface.width * static_cast<float>(plan.inner_rect.width) / surface.raster.width;
    lens.region_height_m = surface.height * static_cast<float>(plan.inner_rect.height) / surface.raster.height;
    view.plate_lens = lens;
    return view;
}

float icvfx_composite_weight(
    const IcvfxPlan& plan, float canvas_x, float canvas_y) noexcept {
    if (!plan.active || plan.skipped) {
        return 0.0F;
    }
    const float left = static_cast<float>(plan.inner_rect.x);
    const float top = static_cast<float>(plan.inner_rect.y);
    const float right = left + static_cast<float>(plan.inner_rect.width);
    const float bottom = top + static_cast<float>(plan.inner_rect.height);
    const float inside = std::min(edge_distance(canvas_x, left, right),
                                  edge_distance(canvas_y, top, bottom));
    if (!(inside > 0.0F)) {
        return 0.0F;
    }
    if (plan.soft_edge_px == 0U) {
        return 1.0F;
    }
    return smoothstep_unit(inside / static_cast<float>(plan.soft_edge_px));
}

bool IcvfxCompositor::shaders_available() noexcept {
    return shader_blobs().present;
}

IcvfxCompositor::IcvfxCompositor(rhi::Texture color,
                                 rhi::GraphicsPipeline pipeline,
                                 rhi::Sampler sampler) noexcept
    : color_(std::move(color)), pipeline_(std::move(pipeline)),
      sampler_(std::move(sampler)) {}

Result IcvfxCompositor::create(rhi::Device& device, core::u32 width,
                               core::u32 height, rhi::TextureFormat format) {
    const ShaderBlobs blobs = shader_blobs();
    if (!blobs.present || !device.supports(blobs.format)) {
        return Result::failure(RenderError::ShaderFormatUnsupported);
    }
    if (width == 0U || height == 0U) {
        return Result::failure(RenderError::InvalidConfiguration);
    }
    auto color = device.create_texture(color_desc(width, height, format));
    if (!color) {
        return Result::failure(RenderError::AllocationFailed);
    }

    rhi::ShaderDesc vertex{};
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.format = blobs.format;
    vertex.bytecode = blobs.vertex;
    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = blobs.format;
    fragment.bytecode = blobs.fragment;
    fragment.sampler_count = 2U;
    fragment.uniform_buffer_count = 1U;
    auto pipeline = device.create_pipeline(
        vertex, fragment, format, false, rhi::DepthState{});
    if (!pipeline) {
        return Result::failure(
            pipeline.error() == rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }
    // Linear, because the inner render is usually at a different pixel density
    // from the wall and a nearest sample would alias along the feather - which
    // is exactly the edge a camera is pointed at.
    auto sampler = device.create_sampler(rhi::Filter::Linear);
    if (!sampler) {
        return Result::failure(RenderError::AllocationFailed);
    }
    return Result::success(IcvfxCompositor{std::move(color).value(),
                                           std::move(pipeline).value(),
                                           std::move(sampler).value()});
}

bool IcvfxCompositor::resize(rhi::Device& device, core::u32 width,
                             core::u32 height) noexcept {
    if (width == 0U || height == 0U) {
        return false;
    }
    if (color_.width() == width && color_.height() == height) {
        return true;
    }
    auto color =
        device.create_texture(color_desc(width, height, color_.desc().format));
    if (!color) {
        return false;
    }
    color_ = std::move(color).value();
    return true;
}

bool IcvfxCompositor::render(rhi::CommandList& commands,
                             const IcvfxPlan& plan,
                             scene::CanvasRect slice,
                             const rhi::Texture& outer,
                             const rhi::Texture* inner) noexcept {
    ++stats_.frames_planned;
    const bool needs_inner = plan.active && !plan.skipped;
    if (needs_inner && inner == nullptr) {
        // A plan that says it needs an inner render and a caller that supplied
        // none is a bug, not a frame to render approximately.
        ++stats_.refusals;
        return false;
    }
    if (slice.width == 0U || slice.height == 0U) {
        ++stats_.refusals;
        return false;
    }

    const IcvfxUniform uniform =
        uniform_for(plan, slice, color_.width(), color_.height());
    // The inactive path binds the outer texture twice rather than branching to
    // a second pipeline: one instruction sequence produces the shared pixels
    // whether or not this node drew an inner frustum.
    const rhi::Texture& inner_source = needs_inner ? *inner : outer;
    const std::array<rhi::SampledTexture, 2U> sources{{
        rhi::SampledTexture{&outer, &sampler_},
        rhi::SampledTexture{&inner_source, &sampler_},
    }};
    if (!commands.draw_output_transform(
            color_, rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}, pipeline_, sources,
            bytes(uniform))) {
        ++stats_.refusals;
        return false;
    }
    ++stats_.composites;
    if (needs_inner) {
        ++stats_.inner_frames_rendered;
    } else if (plan.skipped) {
        ++stats_.inner_frames_skipped;
    }
    return true;
}

} // namespace mediaserver::render
