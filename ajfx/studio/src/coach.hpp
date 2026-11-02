#pragma once

#include "catalog.hpp"
#include "compile.hpp"
#include "document.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The coach: plain-language notes on a page, each with the fix one click
// away. It knows the mistakes that make an effect look cheap - particles that
// pop at the end of their life, smoke slicing into the floor, glow too dim to
// bloom, sparks falling through the ground - and says what to do rather than
// what is wrong.
namespace ajfx::studio {

enum class AdviceLevel : std::uint8_t {
    Tip,     // would look better
    Warning, // probably not what was meant
    Error,   // cannot play
};

struct Advice final {
    Uid layer{};
    // The block the note is about, or 0 for the layer itself.
    Uid block{};
    AdviceLevel level{AdviceLevel::Tip};
    std::string message;
    // Empty when there is nothing to press.
    std::string fix_label;
    std::function<void(Document&)> fix;
};

struct BakeStatus;

// `bake`, where given, adds what the page's baked layers need: a cache to be
// taken, or taken again because a setting it depends on changed.
[[nodiscard]] std::vector<Advice> advise(const Document& document, const Compiled& compiled,
                                         const BakeStatus* bake = nullptr);

// Whether anything is born in the layer: a Stream, a Burst or an Object, or a
// raw spawn module. Disabled blocks do not count. A layer where nothing is
// born plays nothing, so the coach has nothing else to say about it and the
// page offers `starters` instead.
[[nodiscard]] bool has_births(const Layer& layer);

// One block a suggestion adds, with the values it is set to.
struct SuggestedBlock final {
    BlockKind kind;
    std::vector<std::pair<std::string, Value>> values;
};

// Something the page offers to add to a layer: a word the artist chooses by,
// named for the block it adds, and the blocks that make it so. The first block
// is the one the choice is about and the one the page opens; the rest are what
// it needs to play as described (a pour of solid pieces needs gravity and a
// ground, or it hangs in the air and nothing says why). Offered as a tile to
// begin a layer (`starters`), and as a chip once it has begun (`next_steps`).
// There is no sentence about it: the operator learns a block by using it, and
// the tooltip says only which blocks a click adds.
struct Suggestion final {
    std::string title;
    // The picture it is drawn with, as the block picker draws the same block.
    Category icon{Category::Birth};
    std::vector<SuggestedBlock> blocks;
};

// What the layer can start with, in the order to offer it. Empty once
// something is born in it. A rigid layer offers things that fall, and things
// that stand there for them to land on; a particle layer a flow or a burst.
[[nodiscard]] std::vector<Suggestion> starters(const Layer& layer);

// What would carry a layer on, once something is born in it: a look, a fade, a
// ground, and in a rigid layer what makes its pieces fall or what falls on
// what stands there. In the order to offer it, each only while it is missing.
// These are the coach's tips that add a block, offered as the way on rather
// than as faults, so `advise` no longer says them. Empty for a layer that is
// off or has nothing born (that is `starters`).
[[nodiscard]] std::vector<Suggestion> next_steps(const Document& document, const Layer& layer);

// Adds a suggestion's blocks to the end of the layer, as choosing it does, and
// returns the first one's uid (0 when the layer is not there or nothing was
// added). The blocks are conformed to the layer as any block put in it is.
Uid add_suggestion(Document& document, Uid layer, const Suggestion& suggestion);

} // namespace ajfx::studio
