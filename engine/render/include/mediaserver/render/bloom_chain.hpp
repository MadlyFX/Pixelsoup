#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/scene/scene.hpp>

#include <vector>

// The geometry of the bloom mip chain, decided before any of it is recorded.
//
// Bloom is a progressive downsample and upsample: halve the image a few times,
// blurring as it goes, then walk back up adding each level to the one below.
// The arithmetic is unremarkable. **Where the levels are** is not, and that is
// why this is its own file rather than a few lines inside the pass.
//
// A mip level is a partition of the image into blocks, and two nodes holding
// different rectangles of the same canvas will partition it differently unless
// they are made not to. A node whose slice starts at canvas x=1280 would put
// its level-6 block boundary at 1280, 1344, 1408; a node rendering the whole
// canvas puts them at 1280, 1344, 1408 as well - but only because 1280 happens
// to divide by 64. Start the slice at 1300 and every block boundary in that
// node's chain falls somewhere no other node has one, every level averages a
// different set of pixels, and the wall has a bloom seam that moves whenever
// the slice does. This is the same discipline the shadow cascades follow and
// the froxel grid in part B will have to: the grid belongs to the canvas, and a
// node evaluates the part of it that it covers.
//
// So the level count comes from the canvas, never from the local target, and a
// rendered rectangle that does not sit on the canvas's own grid is refused
// rather than quietly resampled.
namespace mediaserver::render {

enum class BloomChainError : core::u8 {
    InvalidCanvas,
    InvalidRect,
    // The rendered rectangle does not begin on the coarsest level's canvas
    // grid, so this node's blocks are not the canvas's blocks.
    Unaligned,
};

// The coarsest level any canvas takes, whatever its size.
//
// A cap rather than a budget: it is the same number on every node, so it does
// not reintroduce the node-dependent chain length this file exists to prevent.
// Six levels reach 252 canvas pixels (see `bloom_chain_apron`), which fits
// inside the default `slice_apron_limit` of 256 - the two numbers are related
// on purpose, because a chain whose apron a show cannot afford is refused
// outright and an operator deserves the default to work.
inline constexpr core::u32 max_bloom_levels = 6U;

// Below this the levels stop carrying anything a blur would spread.
inline constexpr core::u32 min_bloom_level_extent = 8U;

struct BloomLevel final {
    // Canvas-space rectangle of this level, in *canvas* pixels - so a level's
    // position on the wall is stated in the units every node shares, and two
    // nodes can be compared directly.
    scene::CanvasRect canvas{};
    // The level's own extent in its own texels.
    core::u32 width{};
    core::u32 height{};

    bool operator==(const BloomLevel&) const = default;
};

// The canvas-pixel grid a node's rendered rectangle must sit on, which is the
// coarsest level's texel size. Zero when there is no chain.
//
// An authored *slice* that does not start on this grid cannot be rescued by any
// apron - the padded rectangle is the slice displaced by a whole number of
// blocks, so it is off the grid exactly when the slice is - and it is refused.
// A wall whose panels are laid out on any sane boundary satisfies this; one
// whose slices start at arbitrary pixels does not, and should be told so rather
// than shown a bloom seam.
[[nodiscard]] core::u32 bloom_chain_alignment(scene::Canvas canvas) noexcept;

struct BloomChainPlan final {
    // Level zero is the rendered rectangle itself; each following level is half
    // its extent. Empty when the canvas is too small to halve usefully, which
    // is a chain of nothing rather than an error.
    std::vector<BloomLevel> levels;

    [[nodiscard]] core::u32 depth() const noexcept {
        return levels.empty() ? 0U
                              : static_cast<core::u32>(levels.size()) - 1U;
    }
    bool operator==(const BloomChainPlan&) const = default;
};

// How many times this canvas is halved. A pure function of the canvas, so every
// node holding any slice of it computes the same number.
[[nodiscard]] core::u32 bloom_chain_depth(scene::Canvas canvas) noexcept;

// The canvas pixels a chain of this depth reaches, and therefore the apron a
// node must render beyond its own slice for the authored region to hold the
// same values a single node would have produced.
//
// Roughly four times the coarsest level's texel size: two texels of downsample
// reach on the way up the chain and one texel of tent reach on the way back
// down, each doubling as the levels do. It is a large number by design - a
// six-level chain needs 256 pixels on every side - and that cost is the honest
// price of a bloom identical on two halves of a wall. The alternative is not a
// cheaper exact answer; it is an inexact one.
//
// **Rounded up to a whole number of coarsest-level blocks**, and that is not
// tidiness. The rectangle that has to sit on the canvas grid is the *padded*
// one, and a padded rectangle is the slice displaced by the apron - so an apron
// that is not a whole number of blocks moves an aligned slice off the grid and
// `plan_bloom_chain` refuses it. The unrounded reach of a six-level chain is
// 252, which is four short of four blocks, and 252 refuses every frame of a
// 2560x720 wall split down the middle. There is a test for exactly that
// geometry.
[[nodiscard]] core::u32 bloom_chain_apron(core::u32 depth) noexcept;

// The chain a node rendering `render` out of `canvas` should record.
//
// `render` is the *padded* rectangle - the slice plus its apron - because that
// is the region whose values are trustworthy, and the chain is built from all
// of it. The crop back to the authored slice happens afterwards, as it does for
// every other neighbourhood-dependent pass.
[[nodiscard]] core::Result<BloomChainPlan, BloomChainError> plan_bloom_chain(
    scene::Canvas canvas, scene::CanvasRect render) noexcept;

} // namespace mediaserver::render
