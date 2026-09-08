#include <mediaserver/render/shadow_atlas.hpp>

#include <algorithm>
#include <new>

namespace mediaserver::render {
namespace {

// The atlas is tracked as a grid of the smallest tile, and a larger tile is
// aligned to its own size. Alignment is what keeps the packing a pure function
// of the ordered request list: without it, where a large tile lands would
// depend on the gaps a different set of earlier lights happened to leave.
[[nodiscard]] bool fits(const std::vector<bool>& used, core::u32 cells,
                        core::u32 cell_x, core::u32 cell_y,
                        core::u32 span) noexcept {
    if (cell_x + span > cells || cell_y + span > cells) {
        return false;
    }
    for (core::u32 y = 0U; y < span; ++y) {
        for (core::u32 x = 0U; x < span; ++x) {
            if (used[static_cast<std::size_t>(cell_y + y) * cells
                     + (cell_x + x)]) {
                return false;
            }
        }
    }
    return true;
}

void occupy(std::vector<bool>& used, core::u32 cells, core::u32 cell_x,
            core::u32 cell_y, core::u32 span) noexcept {
    for (core::u32 y = 0U; y < span; ++y) {
        for (core::u32 x = 0U; x < span; ++x) {
            used[static_cast<std::size_t>(cell_y + y) * cells
                 + (cell_x + x)] = true;
        }
    }
}

} // namespace

const ShadowTile* ShadowAtlasPlan::find(doc::NodeId light) const noexcept {
    for (const ShadowTile& tile : tiles) {
        if (tile.light == light) {
            return &tile;
        }
    }
    return nullptr;
}

core::u32 tile_extent_for(
    scene::ShadowTier tier, const ShadowAtlasLimits& limits) noexcept {
    switch (tier) {
    case scene::ShadowTier::Small:
        return limits.small_tile;
    case scene::ShadowTier::Large:
        return limits.large_tile;
    case scene::ShadowTier::Medium:
        break;
    }
    return limits.medium_tile;
}

ShadowAtlasPlan plan_shadow_atlas(
    std::span<const scene::PointLight> lights,
    const ShadowAtlasLimits& limits) {
    ShadowAtlasPlan plan{};
    if (limits.small_tile == 0U || limits.extent < limits.small_tile
        || (limits.extent % limits.small_tile) != 0U) {
        // A grid that does not divide is a configuration error, not a scene
        // that happens to have too many lights, so nothing is allocated and
        // nothing is reported as refused.
        return plan;
    }
    const core::u32 cells = limits.extent / limits.small_tile;

    // Requests first, in node-id order. Sorting the *requests* rather than
    // walking the scene's light vector matters: the vector's order is whatever
    // the document parser produced, and two nodes with different local
    // document histories could hold the same lights in a different sequence.
    struct Request final {
        doc::NodeId light{};
        core::u32 extent{};
    };
    std::vector<Request> requests;
    try {
        requests.reserve(lights.size());
    } catch (const std::bad_alloc&) {
        return plan;
    }
    for (const scene::PointLight& light : lights) {
        if (!light.casts_shadow) {
            continue;
        }
        if (light.kind != scene::LocalLightKind::Spot) {
            // A point needs six faces and a rectangle is not a projection at
            // all. Refused explicitly rather than shadowed as if it were a
            // spot pointing down its authored direction, which would be a
            // shadow nobody asked for in a place nobody chose.
            ++plan.refused;
            continue;
        }
        requests.push_back(Request{light.source,
                                   tile_extent_for(light.shadow_tier, limits)});
    }
    std::sort(requests.begin(), requests.end(),
              [](const Request& a, const Request& b) noexcept {
                  return a.light.value() < b.light.value();
              });

    std::vector<bool> used;
    try {
        used.assign(static_cast<std::size_t>(cells) * cells, false);
        plan.tiles.reserve(requests.size());
    } catch (const std::bad_alloc&) {
        return ShadowAtlasPlan{};
    }

    for (const Request& request : requests) {
        const core::u32 extent =
            std::clamp(request.extent, limits.small_tile, limits.extent);
        const core::u32 span = std::max(extent / limits.small_tile, 1U);
        bool placed = false;
        // First fit in row-major order, on the tile's own alignment. Scanning
        // in a fixed order from a fixed list is what makes the result
        // reproducible; there is no attempt to pack tightly, because a
        // cleverer packer is a second thing two nodes could disagree about.
        for (core::u32 cell_y = 0U; !placed && cell_y + span <= cells;
             cell_y += span) {
            for (core::u32 cell_x = 0U; cell_x + span <= cells;
                 cell_x += span) {
                if (!fits(used, cells, cell_x, cell_y, span)) {
                    continue;
                }
                occupy(used, cells, cell_x, cell_y, span);
                plan.tiles.push_back(ShadowTile{
                    request.light, cell_x * limits.small_tile,
                    cell_y * limits.small_tile, span * limits.small_tile});
                placed = true;
                break;
            }
        }
        if (!placed) {
            ++plan.refused;
        }
    }
    return plan;
}

} // namespace mediaserver::render
