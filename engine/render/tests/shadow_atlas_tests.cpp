#include <mediaserver/render/shadow_atlas.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace render = mediaserver::render;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define ATLAS_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] scene::PointLight caster(
    core::u64 id, scene::ShadowTier tier = scene::ShadowTier::Medium) {
    scene::PointLight light{};
    light.source = doc::NodeId{id};
    light.kind = scene::LocalLightKind::Spot;
    light.casts_shadow = true;
    light.shadow_tier = tier;
    return light;
}

// A small atlas: four cells across at the default tile sizes.
[[nodiscard]] render::ShadowAtlasLimits small_atlas() {
    render::ShadowAtlasLimits limits{};
    limits.extent = 1024U;
    limits.small_tile = 256U;
    limits.medium_tile = 512U;
    limits.large_tile = 1024U;
    return limits;
}

// Two nodes given the same lights in different orders agree completely.
//
// This is the property the whole file exists for. The obvious allocation
// policy - biggest tiles to the lights nearest the camera - is wrong here
// because every surface has a different camera, so two nodes would assign the
// same light different tiles and shadow the same wall at two resolutions.
// Sorting by document node id removes the camera from the question entirely.
void assignment_does_not_depend_on_the_order_lights_arrive() {
    const render::ShadowAtlasLimits limits = small_atlas();
    std::vector<scene::PointLight> forward{
        caster(11U), caster(4U, scene::ShadowTier::Small), caster(27U),
        caster(9U, scene::ShadowTier::Small)};
    std::vector<scene::PointLight> reversed{forward.rbegin(), forward.rend()};

    const render::ShadowAtlasPlan a = render::plan_shadow_atlas(forward, limits);
    const render::ShadowAtlasPlan b =
        render::plan_shadow_atlas(reversed, limits);
    ATLAS_CHECK(a == b);
    ATLAS_CHECK(a.tiles.size() == 4U);
    ATLAS_CHECK(a.refused == 0U);

    // And the order really is by node id, not by arrival.
    ATLAS_CHECK(a.tiles[0U].light == doc::NodeId{4U});
    ATLAS_CHECK(a.tiles[1U].light == doc::NodeId{9U});
    ATLAS_CHECK(a.tiles[2U].light == doc::NodeId{11U});
    ATLAS_CHECK(a.tiles[3U].light == doc::NodeId{27U});
}

// Tiles are the authored size, aligned, and never overlap.
void tiles_are_authored_sized_aligned_and_disjoint() {
    const render::ShadowAtlasLimits limits = small_atlas();
    const std::vector<scene::PointLight> lights{
        caster(1U, scene::ShadowTier::Small),
        caster(2U, scene::ShadowTier::Medium),
        caster(3U, scene::ShadowTier::Small),
    };
    const render::ShadowAtlasPlan plan =
        render::plan_shadow_atlas(lights, limits);
    ATLAS_CHECK(plan.tiles.size() == 3U);

    ATLAS_CHECK(render::tile_extent_for(scene::ShadowTier::Small, limits)
                == 256U);
    ATLAS_CHECK(render::tile_extent_for(scene::ShadowTier::Medium, limits)
                == 512U);
    ATLAS_CHECK(render::tile_extent_for(scene::ShadowTier::Large, limits)
                == 1024U);

    for (const render::ShadowTile& tile : plan.tiles) {
        // Inside the atlas.
        ATLAS_CHECK(tile.x + tile.extent <= limits.extent);
        ATLAS_CHECK(tile.y + tile.extent <= limits.extent);
        // Aligned to its own size. Without this, where a tile lands depends on
        // the gaps earlier lights happened to leave, and the packing stops
        // being a pure function of the ordered request list.
        ATLAS_CHECK((tile.x % tile.extent) == 0U);
        ATLAS_CHECK((tile.y % tile.extent) == 0U);
    }

    // No two tiles share a texel. A shadow map written over by its neighbour
    // is not a subtle artifact - it is one light casting another's shadow.
    for (std::size_t i = 0U; i < plan.tiles.size(); ++i) {
        for (std::size_t j = i + 1U; j < plan.tiles.size(); ++j) {
            const render::ShadowTile& a = plan.tiles[i];
            const render::ShadowTile& b = plan.tiles[j];
            const bool apart = a.x + a.extent <= b.x || b.x + b.extent <= a.x
                || a.y + a.extent <= b.y || b.y + b.extent <= a.y;
            ATLAS_CHECK(apart);
        }
    }
}

// Overflow is refused, counted, and refuses the same lights everywhere.
void overflow_is_refused_and_counted_identically() {
    const render::ShadowAtlasLimits limits = small_atlas();
    // Sixteen small tiles fit; ask for twenty.
    std::vector<scene::PointLight> lights;
    for (core::u64 id = 1U; id <= 20U; ++id) {
        lights.push_back(caster(id, scene::ShadowTier::Small));
    }
    const render::ShadowAtlasPlan plan =
        render::plan_shadow_atlas(lights, limits);
    ATLAS_CHECK(plan.tiles.size() == 16U);
    ATLAS_CHECK(plan.refused == 4U);

    // The lowest node ids won, which is what makes the refusal identical on
    // every node rather than merely the same *number* of refusals.
    for (const render::ShadowTile& tile : plan.tiles) {
        ATLAS_CHECK(tile.light.value() <= 16U);
    }
    std::vector<scene::PointLight> shuffled{lights.rbegin(), lights.rend()};
    ATLAS_CHECK(render::plan_shadow_atlas(shuffled, limits) == plan);
}

// Only spots get a tile, and only lights that asked.
void only_spots_that_asked_get_a_tile() {
    const render::ShadowAtlasLimits limits = small_atlas();
    scene::PointLight quiet = caster(1U);
    quiet.casts_shadow = false;
    scene::PointLight omni = caster(2U);
    omni.kind = scene::LocalLightKind::Point;
    scene::PointLight area = caster(3U);
    area.kind = scene::LocalLightKind::Rectangle;
    const std::vector<scene::PointLight> lights{quiet, omni, area, caster(4U)};

    const render::ShadowAtlasPlan plan =
        render::plan_shadow_atlas(lights, limits);
    ATLAS_CHECK(plan.tiles.size() == 1U);
    ATLAS_CHECK(plan.tiles[0U].light == doc::NodeId{4U});
    // A light that did not ask is not a refusal. The two that asked and cannot
    // be served - a point needs six faces, a rectangle is not a projection -
    // are counted, because silently not casting is the failure an operator
    // spends an hour looking for.
    ATLAS_CHECK(plan.refused == 2U);
    ATLAS_CHECK(plan.find(doc::NodeId{4U}) != nullptr);
    ATLAS_CHECK(plan.find(doc::NodeId{1U}) == nullptr);
}

// A misconfigured atlas allocates nothing rather than half of something.
void an_indivisible_atlas_allocates_nothing() {
    render::ShadowAtlasLimits limits = small_atlas();
    limits.extent = 1000U; // not a multiple of the small tile
    const std::vector<scene::PointLight> lights{caster(1U)};
    const render::ShadowAtlasPlan plan =
        render::plan_shadow_atlas(lights, limits);
    ATLAS_CHECK(plan.tiles.empty());
    // Not reported as a refused light: the scene is fine, the configuration
    // is not, and conflating the two sends an operator to the wrong place.
    ATLAS_CHECK(plan.refused == 0U);

    render::ShadowAtlasLimits zero = small_atlas();
    zero.small_tile = 0U;
    ATLAS_CHECK(render::plan_shadow_atlas(lights, zero).tiles.empty());
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 5U> cases{{
    {"assignment does not depend on the order lights arrive",
     &assignment_does_not_depend_on_the_order_lights_arrive},
    {"tiles are authored sized aligned and disjoint",
     &tiles_are_authored_sized_aligned_and_disjoint},
    {"overflow is refused and counted identically",
     &overflow_is_refused_and_counted_identically},
    {"only spots that asked get a tile", &only_spots_that_asked_get_a_tile},
    {"an indivisible atlas allocates nothing",
     &an_indivisible_atlas_allocates_nothing},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const auto& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
