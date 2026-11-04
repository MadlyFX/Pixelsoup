#pragma once

#include "compile.hpp"
#include "document.hpp"

#include <ajfx/cache.hpp>
#include <ajfx/result.hpp>
#include <ajfx/rigid_solver.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

// Studio's bake: a scene's baked items worked out once and cached
// (ajfx/cache.hpp), so a host plays them back rather than simulating them.
//
// Everything but a smoke grid is baked (ItemReport::baked): the solid world
// and the particle systems, and a system that feeds smoke stays live, since
// a cache keeps particles and not what they deposit. Baking runs the whole
// effect from the start on the CPU, for as long as the loop, with the
// scene's performance, and keeps what the baked items did; the export
// writes it beside the package as `<name>.ajfxcache`. What Studio keeps of a
// bake (BakeRecord) is what it says the cache's state by: which baked items
// it holds as they are, which as they were and why, and what baking again
// would cost before anyone asks for it.
namespace ajfx::studio {

// The newest cache Studio took, and what it was taken from.
struct BakeRecord final {
    // The scene as it was baked, to say what has changed since.
    Document document;
    // Each baked item as it was baked: its emitter, what the cache holds of
    // it and its digest (ajfx::emitter_digest), worked out from the scene as
    // bake_status works it out, so the two compare like with like.
    struct Item final {
        Uid item{};
        std::string emitter;
        CacheMode mode{CacheMode::Live};
        std::uint64_t digest{0U};
    };
    std::vector<Item> items;
    std::uint32_t instance_seed{0U};
    std::uint64_t frames{0U};
    // Seconds the bake took, which is what the next one is estimated from.
    double seconds{0.0};
    BakeReport report;
    // The .ajfxcache itself.
    std::vector<std::uint8_t> bytes;

    [[nodiscard]] const Item* item(Uid uid) const;
};

enum class BakeState : std::uint8_t {
    Live,     // not baked: simulated wherever it plays
    Unbaked,  // baked, and no cache holds it yet
    Current,  // the cache holds it as it is
    Stale,    // the cache holds it as it was: something it depends on changed
    TooLarge, // baked, and too big for a cache to hold
};

struct ItemBake final {
    Uid item{};
    BakeState state{BakeState::Live};
    CacheMode mode{CacheMode::Live};
    // Why it is stale or cannot be baked, in the artist's words.
    std::string why;
};

// What baking the scene as it is now would come to, before anyone asks.
struct BakeCost final {
    std::uint64_t frames{0U};
    // Estimated from each baked item's most particles alive (the preview's
    // count where there is one, else its budget): bytes in all, and the most
    // read in one second of playback.
    std::uint64_t bytes{0U};
    std::uint64_t bytes_per_second{0U};
    // From the last bake's pace; negative when there has been none.
    double seconds{-1.0};
};

struct BakeStatus final {
    // In identity order.
    std::vector<ItemBake> items;
    BakeCost cost;
    bool any_baked{false};
    // A cache exists and holds every baked item as it is: export writes it.
    bool current{false};
    // Steps the cache holds, for the transport; 0 with none.
    std::uint64_t cached_frames{0U};
    // Set by the host when it bakes the scene by itself once the scene
    // settles (Studio does): an item waiting for its cache then needs no
    // pressing of Bake.
    bool automatic{false};

    [[nodiscard]] const ItemBake* item(Uid uid) const;
};

// What a cache holds of each of the compiled effect's emitters, in its
// order: the bodies of the solid world, the particles of a baked particle
// system, nothing of a live one.
[[nodiscard]] std::vector<CacheMode> bake_modes(const Document& document, const Compiled& compiled);
// Steps a bake covers: the loop the preview plays.
[[nodiscard]] std::uint64_t bake_frames(const Document& document, const Compiled& compiled);

// Each item's bake state, against `record` (none before the first bake).
// `compiled` is the scene compiled (compile), before its files are read;
// `peaks` the preview's most particles alive by item, for the estimate.
[[nodiscard]] BakeStatus bake_status(const Document& document, const Compiled& compiled, const BakeRecord* record,
                                     const std::map<Uid, std::uint64_t>* peaks = nullptr);

// Bakes the scene's baked items. `compiled` is the scene compiled with its
// files read and its cuts made (PackageBuilder::resolve_assets); `solver`
// runs its rigid physics. `progress` hears the share done, 0 to 1, and stops
// the bake by returning false. Fails, with a reason, when nothing is baked,
// an item is too big for a cache or the bake is over budget.
[[nodiscard]] Result<BakeRecord> bake_scene(const Document& document, const Compiled& compiled,
                                           std::uint32_t instance_seed, const RigidSolver* solver,
                                           const std::function<bool(double)>& progress = {});

// "640 KB", "12.5 MB".
[[nodiscard]] std::string bytes_text(std::uint64_t bytes);

} // namespace ajfx::studio
