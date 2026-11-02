#pragma once

#include "compile.hpp"

#include <ajfx/effect.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The page's solid layers merged into one physics world (SolidWorld): each
// layer compiles to a rigid physics emitter of its own, and these become one
// emitter whose bodies all knock into each other, each layer's init modules
// and looks scoped to its own body kinds. Internal to the compiler.
namespace ajfx::studio {

// One solid layer as it compiled on its own.
struct SolidBuild final {
    // Its place on the page, and its uid.
    std::size_t index{0U};
    Uid layer{};
    Emitter emitter;
    // The block each spawn module came from, in the emitter's stack order.
    std::vector<Uid> spawn_origins;
    // Room it needs for what its bodies break into (LayerBuilder).
    std::uint64_t extra{0U};
    // The cuts it asked for, named against its own emitter and kinds.
    std::vector<FractureCut> cuts;
};

struct MergedSolids final {
    // Named, seeded and drawn as the lowest-uid layer's; its capacity is
    // left for the compiler to size.
    Emitter emitter;
    std::uint64_t extra{0U};
    // Every member's cuts, naming the world and its kinds.
    std::vector<FractureCut> cuts;
    // Each member's particles in the world, in the order of the builds:
    // its body kinds. Empty where the world is one layer alone.
    std::vector<Scope> scopes;
};

// Merges `builds`, in their layers' uid order, into one world:
// - the first is the world's base: its name, seed, renderer, own kind (0),
//   world settings, kinds and props stay as they are;
// - every other layer's own kind and kinds follow as kinds of the world, in
//   uid order, made explicit where they leaned on their layer's renderer
//   (an Auto shape, a hull of "the layer's model", a body drawn as the
//   layer's own), and its props after the base's;
// - the spawn stack is every layer's spawn modules in the order of the
//   blocks they came from, so a spawn source stays where it was when a
//   layer is added or moved;
// - each layer's init modules, colour and size over life and heat are
//   scoped to its kinds, and its breaks name the world's kinds;
// - forces and colliders are the world's: another layer's that the world
//   already holds (the page's gravity and ground, say) are not added twice.
// Nothing, with `why`, when the world would hold more than one may (kinds,
// spawn modules or props).
[[nodiscard]] std::optional<MergedSolids> merge_solids(const std::vector<SolidBuild>& builds, std::string& why);

// Widens every scope naming body kind `parent` of `emitter` to the `count`
// kinds from `first`, which a cut made of it: a piece is in every scope its
// parent was. Particle emitters' readers of that emitter (SpawnFromContacts,
// SpawnFromBodies) and the reports and world that name it follow too.
void widen_scopes(Compiled& compiled, const std::string& emitter, std::uint32_t parent, std::uint32_t first,
                  std::uint32_t count);

} // namespace ajfx::studio
