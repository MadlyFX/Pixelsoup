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

// Studio's bake: a page's baked layers worked out once and cached
// (ajfx/cache.hpp), so a host plays them back rather than simulating them.
//
// A layer is baked or live, and the page says which (Layer::baked). Baking
// runs the whole effect from the start on the CPU, for as long as the loop,
// at the page's own values, and keeps what the baked layers did; the export
// writes it beside the package as `<name>.ajfxcache`. What Studio keeps of a
// bake (BakeRecord) is what it says the cache's state by: which baked layers
// it holds as they are, which as they were and why, and what baking again
// would cost before anyone asks for it.
namespace ajfx::studio {

// The newest cache Studio took, and what it was taken from.
struct BakeRecord final {
    // The page as it was baked, to say what has changed since.
    Document page;
    // Each baked layer as it was baked: its emitter, what the cache holds of
    // it and its digest (ajfx::emitter_digest), worked out from the page as
    // bake_status works it out, so the two compare like with like.
    struct Layer final {
        Uid layer{};
        std::string emitter;
        CacheMode mode{CacheMode::Live};
        std::uint64_t digest{0U};
    };
    std::vector<Layer> layers;
    std::uint32_t instance_seed{0U};
    std::uint64_t frames{0U};
    // Seconds the bake took, which is what the next one is estimated from.
    double seconds{0.0};
    BakeReport report;
    // The .ajfxcache itself.
    std::vector<std::uint8_t> bytes;

    [[nodiscard]] const Layer* layer(Uid uid) const;
};

enum class BakeState : std::uint8_t {
    Live,     // not baked: simulated wherever it plays
    Unbaked,  // baked, and no cache holds it yet
    Current,  // the cache holds it as it is
    Stale,    // the cache holds it as it was: something it depends on changed
    TooLarge, // baked, and too big for a cache to hold
};

struct LayerBake final {
    Uid layer{};
    BakeState state{BakeState::Live};
    CacheMode mode{CacheMode::Live};
    // Why it is stale or cannot be baked, in the artist's words.
    std::string why;
    // The block whose change made it stale, when one did.
    Uid block{};
};

// What baking the page as it is now would come to, before anyone asks.
struct BakeCost final {
    std::uint64_t frames{0U};
    // Estimated from each baked layer's most particles alive (the preview's
    // count where there is one, else its budget): bytes in all, and the most
    // read in one second of playback.
    std::uint64_t bytes{0U};
    std::uint64_t bytes_per_second{0U};
    // From the last bake's pace; negative when there has been none.
    double seconds{-1.0};
};

struct BakeStatus final {
    std::vector<LayerBake> layers;
    BakeCost cost;
    bool any_baked{false};
    // A cache exists and holds every baked layer as it is: export writes it.
    bool current{false};
    // Steps the cache holds, for the transport; 0 with none.
    std::uint64_t cached_frames{0U};
    // Set by the host when it bakes the page by itself once the page
    // settles (Studio does): a layer waiting for its cache then needs no
    // pressing of Bake, and the coach says so quietly.
    bool automatic{false};

    [[nodiscard]] const LayerBake* layer(Uid uid) const;
};

// What a cache holds of each of the compiled effect's emitters, in its
// order: the bodies of a baked rigid layer, the particles of a baked
// particle layer, nothing of a live one.
[[nodiscard]] std::vector<CacheMode> bake_modes(const Document& document, const Compiled& compiled);
// Steps a bake covers: the loop the preview plays.
[[nodiscard]] std::uint64_t bake_frames(const Document& document, const Compiled& compiled);

// Each layer's bake state, against `record` (none before the first bake).
// `compiled` is the page compiled for export, before its files are read;
// `peaks` the preview's most particles alive by layer, for the estimate.
[[nodiscard]] BakeStatus bake_status(const Document& document, const Compiled& compiled, const BakeRecord* record,
                                     const std::map<Uid, std::uint64_t>* peaks = nullptr);

// Bakes the page's baked layers. `compiled` is the page compiled for export
// with its files read and its cuts made (PackageBuilder::resolve_assets);
// `solver` runs its rigid physics. `progress` hears the share done, 0 to 1,
// and stops the bake by returning false. Fails, with a reason, when no layer
// is baked, a layer is too big for a cache or the bake is over budget.
[[nodiscard]] Result<BakeRecord> bake_page(const Document& document, const Compiled& compiled,
                                           std::uint32_t instance_seed, const RigidSolver* solver,
                                           const std::function<bool(double)>& progress = {});

// "640 KB", "12.5 MB".
[[nodiscard]] std::string bytes_text(std::uint64_t bytes);

} // namespace ajfx::studio
