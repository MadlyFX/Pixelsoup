#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/scene/scene.hpp>

#include <span>
#include <vector>

// Where each shadow-casting local light lives in the shadow atlas.
//
// One atlas, partitioned into tiles, rather than a map per light: sixty four
// separate targets is sixty four allocations and sixty four bindings, and the
// atlas is what makes a bounded budget expressible at all.
//
// **The allocation policy is the whole reason this is its own file.** The
// obvious policy is to give the biggest tiles to the lights nearest the
// camera. Every surface has a different camera - Program, Preview, Stage, each
// mapped view, and the inner and outer frusta of a tracked shot - so a
// distance-ranked atlas means two nodes assign different tiles to the same
// light and shadow it at two resolutions. That is a seam down the wall, and it
// moves whenever the camera does.
//
// So tiles are assigned by **stable authored identity**: sort by document node
// id, allocate in that order, refuse the overflow and count it. Node ids are
// replicated, so every node walks the same list and reaches the same answer
// without talking to anyone. An operator who wants a particular light
// prioritised raises its authored tier rather than hoping a heuristic agrees.
namespace mediaserver::render {

struct ShadowAtlasLimits final {
    // Square, and a power of two multiple of the smallest tile so the grid
    // below divides exactly.
    core::u32 extent{2048U};
    core::u32 small_tile{256U};
    core::u32 medium_tile{512U};
    core::u32 large_tile{1024U};

    bool operator==(const ShadowAtlasLimits&) const = default;
};

struct ShadowTile final {
    doc::NodeId light{};
    core::u32 x{};
    core::u32 y{};
    core::u32 extent{};

    bool operator==(const ShadowTile&) const = default;
};

struct ShadowAtlasPlan final {
    // Ordered by light node id, which is also the order they were allocated.
    std::vector<ShadowTile> tiles;
    // Lights that asked for a tile and did not get one. Counted rather than
    // hidden: the same lights are refused on every node, so the result is
    // consistent, but an operator still needs to know a practical stopped
    // casting because the atlas was full.
    core::u32 refused{};

    bool operator==(const ShadowAtlasPlan&) const = default;

    // The tile for a light, or nullptr. Linear over a list bounded by the
    // atlas's tile count, which is small.
    [[nodiscard]] const ShadowTile* find(doc::NodeId light) const noexcept;
};

[[nodiscard]] core::u32 tile_extent_for(
    scene::ShadowTier tier, const ShadowAtlasLimits& limits) noexcept;

// Deterministic, allocation order fixed by node id, no camera anywhere in it.
//
// Only spot lights are considered: a spot is one projection, a point is six,
// and doing spots first buys most of the benefit for a sixth of the cost. A
// point light that asks to cast is refused and counted rather than silently
// treated as a spot pointing somewhere arbitrary.
[[nodiscard]] ShadowAtlasPlan plan_shadow_atlas(
    std::span<const scene::PointLight> lights,
    const ShadowAtlasLimits& limits);

} // namespace mediaserver::render
