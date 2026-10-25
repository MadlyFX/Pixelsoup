#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/scene/scene.hpp>

namespace mediaserver::render {

enum class SliceApronError : core::u8 {
    InvalidCanvas,
    InvalidSlice,
    LimitExceeded,
};

struct SliceApronRequirements final {
    core::u32 spatial_filter{};
    core::u32 denoiser{};
    core::u32 reflection{};
    core::u32 taau{};
    core::u32 bloom{};
    core::u32 occlusion{};
    core::u32 contact_shadow{};
    core::u32 motion_blur{};
    core::u32 plate_dof{};
    // Canvas-pixel granularity the padded rectangle must land on, or zero for
    // none. Bloom sets it, because its mip chain partitions the canvas into
    // blocks and a node whose rectangle straddles them averages a different set
    // of pixels from its neighbour.
    //
    // Applied to the *maximum* rather than folded into the bloom lane: any lane
    // can be the largest, and whichever one wins is what displaces the
    // rectangle.
    core::u32 alignment{};
    // Transmission reads an already shaded background; its support adds to
    // the downstream filter requirement rather than competing with it.
    core::u32 transmission{};

    [[nodiscard]] core::u32 maximum() const noexcept;
    bool operator==(const SliceApronRequirements&) const = default;
};

struct PaddedSlice final {
    // Canvas-space rectangle actually rendered by the node.
    scene::CanvasRect render{};
    // Rectangle of the authored slice inside `render`. Crop to this only after
    // every neighbourhood-dependent pass has completed.
    scene::CanvasRect crop{};
    core::u32 apron{};

    bool operator==(const PaddedSlice&) const = default;
};

// The identity padding: render exactly the authored slice, crop nothing.
//
// A named function rather than an inline aggregate at each site, because two
// sites constructing "no apron" by hand is two chances to get the crop
// rectangle wrong - and a wrong crop is a silently offset image, not a
// failure.
[[nodiscard]] constexpr PaddedSlice unpadded(scene::CanvasRect slice) noexcept {
    return PaddedSlice{
        slice, scene::CanvasRect{0U, 0U, slice.width, slice.height}, 0U};
}

// The renderer target extent a padded slice needs, in **target** pixels.
//
// `render` and `crop` above are canvas-space rectangles. The targets they are
// drawn into are not: a render whose output extent differs from its slice is a
// resample, and its target is the output extent, not the slice. The apron is
// the one part that is shared, and it is only ever non-zero at a one-to-one
// pitch (see padded_slice_for), where the two spaces coincide anyway.
//
// This exists for the same reason padded_slice_for does. The compositor sizes
// these targets and the renderer validates them, and when the two computed it
// separately they agreed only at one-to-one: every scaled render - every
// Program preview smaller than its output raster - was refused on the size
// check, every frame, with no picture and no asset digest to show for it.
struct PaddedTargetExtent final {
    core::u32 width{};
    core::u32 height{};

    bool operator==(const PaddedTargetExtent&) const = default;
};

[[nodiscard]] constexpr PaddedTargetExtent padded_target_extent(
    const PaddedSlice& padded, scene::CanvasRect frame,
    core::u32 output_width, core::u32 output_height) noexcept {
    return PaddedTargetExtent{
        output_width + (padded.render.width - frame.width),
        output_height + (padded.render.height - frame.height)};
}

// The rectangle to crop out of a padded target, in **target** pixels. The
// offset is the apron, which is canvas-space only where the two grids
// coincide; the extent is always the output's own, never the slice's.
[[nodiscard]] constexpr scene::CanvasRect padded_target_crop(
    const PaddedSlice& padded, core::u32 output_width,
    core::u32 output_height) noexcept {
    return scene::CanvasRect{padded.crop.x, padded.crop.y, output_width,
                             output_height};
}

[[nodiscard]] SliceApronRequirements apron_requirements(
    const scene::Scene& scene) noexcept;

[[nodiscard]] core::Result<PaddedSlice, SliceApronError> padded_slice(
    scene::Canvas canvas,
    scene::CanvasRect slice,
    const SliceApronRequirements& requirements,
    core::u32 limit) noexcept;

// The padded slice one renderer should actually use, decided from the scene
// rather than from a requirement the caller assembled itself.
//
// This exists so the two places that need the answer - the compositor sizing
// its targets, and the renderer recording the draw - cannot disagree. They are
// pure functions of the same replicated inputs, so calling this from both is
// one definition, not two implementations.
//
// **The apron is only applied at a one-to-one pixel pitch**, meaning the output
// extent equals the slice extent. That is the case the whole padded path exists
// for: a wall node rendering its own rectangle of the canvas at the canvas's
// own resolution. At any other scale the render is a resample, the target grid
// does not line up with the canvas grid, and an apron measured in canvas pixels
// cannot be converted to a whole number of output pixels without moving the
// interior by a fraction of one - which is precisely the error this path exists
// to eliminate. A scaled render is a preview or a mapped view; it has no
// neighbour to seam against, so it takes no apron and costs nothing.
//
// `advanced` drops the renderer-settings lanes (denoiser, reflection, TAAU)
// when Advanced was authored but not selected. It does not drop the
// spatial-filter lane: an authored blur is applied by the compositor on either
// backend, so a Basic wall needs the same apron an Advanced one does.
[[nodiscard]] core::Result<PaddedSlice, SliceApronError> padded_slice_for(
    const scene::Scene& scene,
    scene::CanvasRect slice,
    core::u32 output_width,
    core::u32 output_height,
    bool advanced) noexcept;

// A complete off-axis image has no canvas boundary to clamp against. Render
// outside its visible crop, then widen the supplied projection by this ratio.
// The visible extent remains the history key; this allocation is pose-invariant.
[[nodiscard]] core::Result<PaddedSlice, SliceApronError> padded_view_for(
    const scene::Scene& scene, core::u32 width, core::u32 height,
    bool advanced, bool plate_dof = false) noexcept;

} // namespace mediaserver::render
