#include <mediaserver/render/slice_apron.hpp>

#include <mediaserver/render/bloom_chain.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace mediaserver::render {
namespace {

[[nodiscard]] core::u32 ceil_to_u32(float value) noexcept {
    if (!std::isfinite(value) || value <= 0.0F) {
        return 0U;
    }
    const float maximum = static_cast<float>(
        std::numeric_limits<core::u32>::max());
    if (value >= maximum) {
        return std::numeric_limits<core::u32>::max();
    }
    return static_cast<core::u32>(std::ceil(value));
}

} // namespace

core::u32 SliceApronRequirements::maximum() const noexcept {
    // Bloom reads the shaded result, including screen-space occlusion and
    // transmission. Its input needs its own valid border: max(AO, bloom)
    // leaves AO-clamped pixels inside bloom's footprint at a slice edge.
    const core::u64 filters = static_cast<core::u64>(spatial_filter)
        + std::max(denoiser, reflection) + taau + std::max(occlusion, contact_shadow);
    return static_cast<core::u32>(std::min<core::u64>(
        filters + bloom + transmission + motion_blur + plate_dof, std::numeric_limits<core::u32>::max()));
}

// The apron one first-class layer effect stack needs.
//
// These are the effects an operator drops onto a layer, and they are what the
// compositor actually applies - `scene.effects` below is the older show-wide
// list. Both reach the same GPU processors, so both must be counted: a layer
// blur that was not counted is an apron of zero and a seam down the wall, with
// nothing anywhere reporting that the filter was wider than the pixels it had.
[[nodiscard]] core::u32 layer_style_apron(
    const scene::EffectStack& effects,
    const scene::EffectSettings& settings) noexcept {
    core::u32 apron = 0U;
    if (effects.blur_enabled) {
        apron = std::max(
            apron, ceil_to_u32(effects.blur_radius) * settings.blur_passes);
    }
    for (const scene::SpatialEffectOperation& operation :
         effects.spatial_operations) {
        switch (operation.kind) {
        case scene::EffectKind::Blur:
            apron = std::max(
                apron, ceil_to_u32(operation.value) * settings.blur_passes);
            break;
        case scene::EffectKind::BoxBlur:
            apron = std::max(
                apron, operation.box_blur.radius_px * operation.box_blur.passes);
            break;
        case scene::EffectKind::DirectionalBlur:
            // Half the length: a directional blur is centred on the texel, so
            // it reaches half its span in each direction.
            apron = std::max(
                apron,
                ceil_to_u32(operation.directional_blur.length_px * 0.5F));
            break;
        default:
            // Every other operation is per-pixel and reads no neighbour.
            break;
        }
    }
    return apron;
}

SliceApronRequirements apron_requirements(const scene::Scene& value) noexcept {
    SliceApronRequirements result{};
    // Imported material factors become resident asynchronously. Reserve the
    // fixed thin-transmission support for model scenes from authored state,
    // never from worker completion or this node's currently visible slice.
    if (!value.models.empty() || std::any_of(value.instances.begin(), value.instances.end(),
        [](const auto& instance) { return instance.material.transmission > 0.0F; })) {
        result.transmission = 8U;
    }
    const scene::EffectSettings settings = scene::clamped(value.effect_settings);
    for (const scene::SceneScreen& screen : value.screens) {
        result.spatial_filter = std::max(
            result.spatial_filter, layer_style_apron(screen.style.effects,
                                                     settings));
    }
    for (const scene::MeshInstance& instance : value.instances) {
        result.spatial_filter = std::max(
            result.spatial_filter, layer_style_apron(instance.style.effects,
                                                     settings));
    }
    for (const scene::MediaInstance& instance : value.media) {
        result.spatial_filter = std::max(
            result.spatial_filter, layer_style_apron(instance.style.effects,
                                                     settings));
    }
    for (const scene::ModelInstance& instance : value.models) {
        result.spatial_filter = std::max(
            result.spatial_filter, layer_style_apron(instance.style.effects,
                                                     settings));
    }
    for (const scene::EffectInstance& effect : value.effects) {
        if (!effect.enabled) {
            continue;
        }
        switch (effect.kind) {
        case scene::EffectKind::Blur:
            result.spatial_filter = std::max(
                result.spatial_filter, ceil_to_u32(effect.value) *
                    scene::clamped(value.effect_settings).blur_passes);
            break;
        case scene::EffectKind::BoxBlur:
            result.spatial_filter = std::max(
                result.spatial_filter,
                effect.box_blur.radius_px * effect.box_blur.passes);
            break;
        case scene::EffectKind::DirectionalBlur:
            result.spatial_filter = std::max(
                result.spatial_filter,
                ceil_to_u32(effect.directional_blur.length_px * 0.5F));
            break;
        case scene::EffectKind::Brightness:
        case scene::EffectKind::Saturation:
        case scene::EffectKind::ChromaKey:
        case scene::EffectKind::LumaKey:
        case scene::EffectKind::Levels:
        case scene::EffectKind::LiftGammaGain:
        case scene::EffectKind::ContrastPivot:
        case scene::EffectKind::ColorMatrix:
        case scene::EffectKind::WhiteBalance:
        case scene::EffectKind::Curves:
        case scene::EffectKind::FilmGrain:
        case scene::EffectKind::Count:
            break;
        }
    }
    if (value.renderer_settings.backend == scene::RendererBackend::Advanced) {
        // Geometric specular AA evaluates fine derivatives on 2x2 raster quads.
        // Align the padded origin, including for odd-width physical node slices.
        result.alignment = 2U;
        result.denoiser = value.renderer_settings.denoising ? 16U : 0U;
        result.reflection = value.renderer_settings.ray_tracing ? 32U : 0U;
        // Two pixels: one for the 3x3 neighbourhood clamp the resolve
        // reads around each pixel, and one for the sub-pixel jitter, which
        // reaches half a pixel and so can pull that neighbourhood a whole
        // pixel further out once rounded. A clamp that read outside the padded
        // region would sample the edge and pin the colour box to it, which
        // shows up as a bright seam exactly where two nodes meet.
        result.taau = value.renderer_settings.taau ? 2U : 0U;
        result.motion_blur = value.renderer_settings.motion_blur
            ? std::min(value.renderer_settings.motion_blur_radius, scene::max_motion_blur_radius) : 0U;
        // The occlusion search's screen-space reach, which the shader clamps
        // to this same number. One constant expressed in two languages is a
        // hazard; the shader names it in its own comment so a change to either
        // is visibly a change to both.
        result.contact_shadow = value.renderer_settings.contact_shadow_length > 0.0F ? 32U : 0U;
        result.occlusion =
            value.renderer_settings.occlusion_radius > 0.0F ? 32U : 0U;
        // The whole chain's reach, which is large and is meant to be: a
        // six-level chain needs 252 pixels on every side for the authored
        // region to hold the values one node would have produced. If a show
        // cannot afford that it gets a refusal from `padded_slice` rather than
        // a shorter chain, because a shorter chain on one node and a full one
        // on its neighbour is two different blurs meeting at a seam.
        if (value.renderer_settings.bloom_strength > 0.0F) {
            result.bloom = bloom_chain_apron(bloom_chain_depth(value.canvas));
            result.alignment = bloom_chain_alignment(value.canvas);
        }
    }
    return result;
}

core::Result<PaddedSlice, SliceApronError> padded_slice(
    scene::Canvas canvas,
    scene::CanvasRect slice,
    const SliceApronRequirements& requirements,
    core::u32 limit) noexcept {
    using Result = core::Result<PaddedSlice, SliceApronError>;
    if (!scene::valid(canvas)) {
        return Result::failure(SliceApronError::InvalidCanvas);
    }
    if (!scene::contains(canvas, slice)) {
        return Result::failure(SliceApronError::InvalidSlice);
    }
    core::u64 rounded_apron = requirements.maximum();
    if (requirements.alignment > 1U) {
        // Rounded up, never down. Down would be a smaller apron than a pass
        // asked for, which is the silent shortening every refusal in this file
        // exists to avoid.
        rounded_apron = ((rounded_apron + requirements.alignment - 1U)
                 / requirements.alignment) * requirements.alignment;
    }
    if (rounded_apron > limit) {
        return Result::failure(SliceApronError::LimitExceeded);
    }
    const core::u32 apron = static_cast<core::u32>(rounded_apron);

    const core::u32 alignment = std::max(requirements.alignment, 1U);
    const core::u32 start_x = (slice.x - std::min(slice.x, apron)) / alignment * alignment;
    const core::u32 start_y = (slice.y - std::min(slice.y, apron)) / alignment * alignment;
    const auto aligned_end = [alignment](core::u32 end, core::u32 extent, core::u32 guard) {
        const core::u64 expanded = std::min<core::u64>(static_cast<core::u64>(end) + guard, extent);
        return static_cast<core::u32>(std::min<core::u64>(
            (expanded + alignment - 1U) / alignment * alignment, extent));
    };
    const core::u32 end_x = aligned_end(slice.x + slice.width, canvas.width, apron);
    const core::u32 end_y = aligned_end(slice.y + slice.height, canvas.height, apron);
    const core::u32 left = slice.x - start_x;
    const core::u32 top = slice.y - start_y;
    const core::u32 right = end_x - (slice.x + slice.width);
    const core::u32 bottom = end_y - (slice.y + slice.height);
    if (std::max({left, top, right, bottom}) > limit)
        return Result::failure(SliceApronError::LimitExceeded);

    PaddedSlice result{};
    result.render = scene::CanvasRect{
        slice.x - left, slice.y - top, slice.width + left + right,
        slice.height + top + bottom};
    result.crop = scene::CanvasRect{left, top, slice.width, slice.height};
    result.apron = apron;
    return Result::success(result);
}

core::Result<PaddedSlice, SliceApronError> padded_slice_for(
    const scene::Scene& scene,
    scene::CanvasRect slice,
    core::u32 output_width,
    core::u32 output_height,
    bool advanced) noexcept {
    // A scaled render takes no apron. See the note in the header: an apron in
    // canvas pixels has no exact expression in output pixels unless the two
    // grids coincide, and a fractional interior offset is a worse error than
    // the seam an apron prevents.
    const bool one_to_one =
        output_width == slice.width && output_height == slice.height;
    SliceApronRequirements requirements =
        one_to_one ? apron_requirements(scene) : SliceApronRequirements{};
    // The Advanced-only lanes are dropped when Advanced was authored but not
    // selected. The spatial-filter lane is not one of them: an authored blur is
    // applied by the compositor on either backend, so a Basic wall seams
    // exactly as an Advanced one would without its apron.
    if (!advanced) {
        requirements.alignment = 0U;
        requirements.bloom = 0U;
        requirements.denoiser = 0U;
        requirements.reflection = 0U;
        requirements.taau = 0U;
        requirements.occlusion = 0U;
        requirements.contact_shadow = 0U;
        requirements.motion_blur = 0U;
    }
    return padded_slice(
        scene.canvas, slice, requirements,
        scene.renderer_settings.slice_apron_limit);
}

core::Result<PaddedSlice, SliceApronError> padded_view_for(
    const scene::Scene& scene, core::u32 width, core::u32 height,
    bool advanced, bool plate_dof) noexcept {
    using Result = core::Result<PaddedSlice, SliceApronError>;
    constexpr core::u64 max_extent = 16384U;
    if (width == 0U || height == 0U || width > max_extent || height > max_extent)
        return Result::failure(SliceApronError::InvalidSlice);
    auto requirements = apron_requirements(scene);
    if (advanced && plate_dof && scene.renderer_settings.plate_depth_of_field)
        requirements.plate_dof = std::min(scene.renderer_settings.plate_dof_radius, scene::max_plate_dof_radius);
    if (!advanced) {
        requirements.bloom = requirements.denoiser = requirements.reflection = 0U;
        requirements.taau = requirements.occlusion = requirements.alignment = 0U;
        requirements.motion_blur = requirements.contact_shadow = 0U;
    } else if (scene.renderer_settings.bloom_strength > 0.0F) {
        // Padding can deepen bloom's pyramid. Reserve its bounded maximum so
        // growing the target cannot silently increase the filter beyond its guard.
        requirements.bloom = bloom_chain_apron(max_bloom_levels);
        requirements.alignment = 1U << max_bloom_levels;
    }
    const core::u64 alignment = std::max(2U, requirements.alignment);
    const core::u64 apron = (static_cast<core::u64>(requirements.maximum())
        + alignment - 1U) / alignment * alignment;
    if (apron > scene.renderer_settings.slice_apron_limit
        || width + 2U * apron > max_extent || height + 2U * apron > max_extent)
        return Result::failure(SliceApronError::LimitExceeded);
    const auto guard = static_cast<core::u32>(apron);
    return Result::success(PaddedSlice{
        {0U, 0U, width + 2U * guard, height + 2U * guard},
        {guard, guard, width, height}, guard});
}

} // namespace mediaserver::render
