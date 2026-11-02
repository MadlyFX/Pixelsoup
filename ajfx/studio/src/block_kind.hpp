#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// Every kind of block a page can hold, named once.
//
// A page stores a block's kind as the id below, and the catalog, compiler,
// guides and coach all ask what kind a block is. Spelled as a string literal
// at each of those sites a typo compiles and silently matches nothing, so
// they spell it as a BlockKind instead: a misspelt enumerator does not
// build, and a `switch` over BlockKind that leaves a kind out is a warning,
// which the build holds as an error. The id is only for saving, loading and
// anything keyed by text (a dragged block, a search).
namespace ajfx::studio {

enum class BlockKind : std::uint8_t {
    // Birth
    Stream,
    Burst,
    Shape,
    Launch,
    // Born of the page's solid things: where they hit, and behind them as
    // they move. Particle layers only.
    HitPuffs,
    HitSparks,
    Trail,
    // Motion
    Gravity,
    Rise,
    Wind,
    Turbulence,
    Drag,
    Swirl,
    DrawIn,
    Attract,
    // A blast that goes off once and throws everything near it outward: the
    // page's, not the layer's, so it reaches every layer that does not pass
    // through.
    Explosion,
    // A short shove at one point, one way: a small blast set behind what it
    // pushes. The page's, as an Explosion is.
    Push,
    // Particles pushed along by the page's moving solid things.
    Stirred,
    // Look
    Appearance,
    Color,
    Size,
    Spin,
    Fade,
    // A glow that cools with age.
    Heat,
    // Contact
    Ground,
    // Opts a particle layer out of meeting the page's solid things (and, if
    // it says so, the page's ground), which it meets by default.
    PassesThrough,
    Sphere,
    Box,
    MeshCollider,
    Tumble,
    // Physics
    Object,
    Break,
    Part,
    Physics,
    // Light
    Light,
    // Expert: one raw ajfx module.
    Module,
};

inline constexpr std::size_t block_kind_count = static_cast<std::size_t>(BlockKind::Module) + 1U;

// The id a page stores, in enumerator order.
inline constexpr std::array<std::string_view, block_kind_count> block_kind_ids{
    "stream", "burst",  "shape",  "launch", "hit_puffs", "hit_sparks", "trail", "gravity",      "rise",   "wind",   "turbulence", "drag",
    "swirl",  "draw_in", "attract", "explosion", "push", "stirred", "appearance", "color",       "size",   "spin",   "fade", "heat", "ground",
    "passes_through", "sphere", "box",    "mesh_collider", "tumble",    "object", "break",  "part",       "physics",
    "light",  "module",
};

[[nodiscard]] constexpr std::string_view block_kind_id(BlockKind kind) noexcept {
    return block_kind_ids[static_cast<std::size_t>(kind)];
}

// The kind an id names, or nothing for an id no Studio has.
[[nodiscard]] constexpr std::optional<BlockKind> block_kind_from(std::string_view id) noexcept {
    for (std::size_t i = 0U; i < block_kind_ids.size(); ++i) {
        if (block_kind_ids[i] == id) {
            return static_cast<BlockKind>(i);
        }
    }
    return std::nullopt;
}

// So a block's stored kind reads against the enumerator, `block.kind ==
// BlockKind::Object`, with no string in sight at the call.
[[nodiscard]] constexpr bool operator==(std::string_view id, BlockKind kind) noexcept {
    return id == block_kind_id(kind);
}

} // namespace ajfx::studio
