#include <mediaserver/render/bloom_chain.hpp>

#include <algorithm>

namespace mediaserver::render {
namespace {

// Ceiling division, which is what a partial block at the canvas edge needs: the
// last level-k block of a canvas that does not divide evenly is short, and both
// a node holding that edge and a node holding the whole canvas must agree that
// it exists.
[[nodiscard]] core::u32 halve(core::u32 value) noexcept {
    return value == 0U ? 0U : (value + 1U) / 2U;
}

} // namespace

core::u32 bloom_chain_depth(scene::Canvas canvas) noexcept {
    if (!scene::valid(canvas)) {
        return 0U;
    }
    core::u32 extent = std::min(canvas.width, canvas.height);
    core::u32 depth = 0U;
    while (depth < max_bloom_levels && halve(extent) >= min_bloom_level_extent) {
        extent = halve(extent);
        ++depth;
    }
    return depth;
}

core::u32 bloom_chain_apron(core::u32 depth) noexcept {
    if (depth == 0U) {
        return 0U;
    }
    const core::u32 capped = std::min(depth, max_bloom_levels);
    const core::u32 stride = 1U << capped;
    const core::u32 reach = 4U * (stride - 1U);
    // Up to the next whole block. See the header: the padded rectangle is the
    // slice displaced by this, and displacing an aligned slice by a partial
    // block lands it off the grid.
    return ((reach + stride - 1U) / stride) * stride;
}

core::u32 bloom_chain_alignment(scene::Canvas canvas) noexcept {
    const core::u32 depth = bloom_chain_depth(canvas);
    return depth == 0U ? 0U : 1U << depth;
}

core::Result<BloomChainPlan, BloomChainError> plan_bloom_chain(
    scene::Canvas canvas, scene::CanvasRect render) noexcept {
    using Result = core::Result<BloomChainPlan, BloomChainError>;
    if (!scene::valid(canvas)) {
        return Result::failure(BloomChainError::InvalidCanvas);
    }
    if (!scene::contains(canvas, render) || render.width == 0U
        || render.height == 0U) {
        return Result::failure(BloomChainError::InvalidRect);
    }

    const core::u32 depth = bloom_chain_depth(canvas);
    if (depth == 0U) {
        return Result::success(BloomChainPlan{});
    }

    // The rectangle must sit on the coarsest level's canvas grid, or this
    // node's blocks are not the canvas's blocks and no amount of apron will
    // make its levels agree with a neighbour's.
    //
    // An edge that reaches the canvas boundary is aligned by definition: the
    // last block there is short for every node, including one rendering the
    // whole canvas, and they are all short in the same place.
    const core::u32 stride = 1U << depth;
    const bool left_aligned = render.x % stride == 0U;
    const bool top_aligned = render.y % stride == 0U;
    const bool right_aligned = (render.x + render.width) % stride == 0U
        || render.x + render.width == canvas.width;
    const bool bottom_aligned = (render.y + render.height) % stride == 0U
        || render.y + render.height == canvas.height;
    if (!left_aligned || !top_aligned || !right_aligned || !bottom_aligned) {
        return Result::failure(BloomChainError::Unaligned);
    }

    BloomChainPlan plan{};
    plan.levels.reserve(static_cast<std::size_t>(depth) + 1U);
    // Level zero is the rendered rectangle at its own resolution. Every level
    // after it keeps the same canvas footprint and halves its texel count, so
    // `canvas` below always states where the level sits on the wall rather than
    // where it sits in this node's target.
    core::u32 width = render.width;
    core::u32 height = render.height;
    for (core::u32 level = 0U; level <= depth; ++level) {
        plan.levels.push_back(BloomLevel{render, width, height});
        width = halve(width);
        height = halve(height);
    }
    return Result::success(plan);
}

} // namespace mediaserver::render
