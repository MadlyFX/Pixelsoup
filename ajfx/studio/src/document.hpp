#pragma once

#include <ajfx/modules.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// An ajfx Studio document: a page of layers, each a short list of blocks.
//
// Blocks say what an artist wants ("stream 40 a second", "rise", "glow
// orange, fading out") rather than how the runtime does it. The compiler
// (compile.hpp) turns a page into an ajfx::Effect: it places each block's
// modules in the stage and order the runtime needs, merges what several
// blocks contribute to one module, and sizes every emitter's budget. So the
// order of blocks on the page is for reading, not for correctness.
//
// Experts reach under the hood with the Module block, which holds one ajfx
// module verbatim, every field editable.
namespace ajfx::studio {

// Stable identity of a layer or block within its document. Module ids and
// random seeds are derived from it, so inserting, moving or deleting a block
// leaves every other block's particles where they were.
using Uid = std::uint32_t;

// A control's value. One shape for every control type keeps storage,
// undo and saving generic: a number uses x; a range x (low) and y (high);
// a vector x, y and z; a colour x, y, z and w; a choice `text`; a switch
// x (0 or 1).
struct Value final {
    float x{};
    float y{};
    float z{};
    float w{};
    std::string text;

    friend bool operator==(const Value&, const Value&) = default;
};

[[nodiscard]] inline Value number(float x) { return Value{x, 0.0F, 0.0F, 0.0F, {}}; }
[[nodiscard]] inline Value range(float low, float high) { return Value{low, high, 0.0F, 0.0F, {}}; }
[[nodiscard]] inline Value vec3(float x, float y, float z) { return Value{x, y, z, 0.0F, {}}; }
[[nodiscard]] inline Value color(float r, float g, float b, float a = 1.0F) { return Value{r, g, b, a, {}}; }
[[nodiscard]] inline Value choice(std::string id) { return Value{0.0F, 0.0F, 0.0F, 0.0F, std::move(id)}; }
[[nodiscard]] inline Value toggle(bool on) { return Value{on ? 1.0F : 0.0F, 0.0F, 0.0F, 0.0F, {}}; }

// Any built-in ajfx module, for the Module block.
using RawModule = std::variant<SpawnModule, InitModule, UpdateModule>;

// A control the effect publishes for its host to set and animate: the media
// server's Inspector shows it by `name`, held to `low`..`high` (in the
// control's own units, before page scale). The effect's budget is worked out
// for the whole range, so no value an operator picks can overrun it. A range
// control publishes two parameters, `name_min` and `name_max`.
struct Published final {
    std::string control;
    std::string name;
    float low{0.0F};
    float high{1.0F};

    friend bool operator==(const Published&, const Published&) = default;
};

// A tidy parameter name from any text ("Flames rate" -> "flames_rate"):
// lower-case letters, digits and single underscores, at most 48.
[[nodiscard]] std::string parameter_name(std::string_view text);
// What may be typed into a name: letters and digits lower-cased, spaces and
// dashes as underscores, everything else dropped.
[[nodiscard]] std::string typed_parameter_name(std::string_view text);
// Hosts key parameters by names of letters, digits and underscores.
[[nodiscard]] bool valid_parameter_name(std::string_view name) noexcept;

// What a Module block was detached from, so the modules a block became can
// be put back together into it. Every Module block of one detach carries the
// same record.
struct Detached final {
    // The block's own uid: the modules of one detach share it.
    Uid from{};
    // The block as it was, as page JSON (document_io.hpp).
    std::string original;
    // This module as the detach made it (id 0), to tell whether it has been
    // edited since.
    RawModule module;
    // How many Module blocks the detach made.
    std::uint32_t count{1U};

    friend bool operator==(const Detached&, const Detached&) = default;
};

struct Block final {
    Uid uid{};
    // The catalog entry this block is (catalog.hpp), e.g. "stream".
    std::string kind;
    bool enabled{true};
    // Values the artist changed, sorted by control; every other control
    // takes its default. Change them through set() and clear().
    std::vector<std::pair<std::string, Value>> values;
    // The Module block's module.
    std::optional<RawModule> raw;
    // Controls it publishes, sorted by control.
    std::vector<Published> published;
    // Set on a Module block made by detaching another block.
    std::optional<Detached> detached;

    // The stored value, or nothing for a control at its default.
    [[nodiscard]] const Value* find(std::string_view control) const;
    void set(std::string_view control, Value value);
    void clear(std::string_view control);

    [[nodiscard]] const Published* publication(std::string_view control) const;
    void publish(Published publication);
    void unpublish(std::string_view control);

    friend bool operator==(const Block&, const Block&) = default;
};

struct Layer final {
    Uid uid{};
    std::string name;
    bool enabled{true};
    // Preview only: while any layer is soloed, only soloed layers play.
    // Export ignores it.
    bool solo{false};
    // Seconds each particle lives, between the two.
    float life_min{1.0F};
    float life_max{2.0F};
    // Deals the whole layer's random values afresh; 0 keeps the layer's own.
    std::uint32_t seed{0U};
    std::vector<Block> blocks;

    friend bool operator==(const Layer&, const Layer&) = default;
};

// What the preview stands the effect on. Never exported.
enum class Backdrop : std::uint8_t {
    Studio, // a lit grey floor and a 1.8 m figure for scale
    Night,  // the same floor under a dim moonlit rig, for glowing effects
    Void,   // nothing: black, no floor
};

struct Document final {
    std::string name{"Untitled effect"};
    // Multiplies every distance, size, speed and acceleration in the blocks,
    // so the whole effect grows or shrinks and keeps its look. Module blocks
    // are taken literally.
    float scale{1.0F};
    // Seconds the preview plays before it loops, when `loop_auto` is off.
    // With it on (the default) the loop is as long as the effect takes to
    // play out, worked out from its emitters (compile.hpp).
    float loop_seconds{6.0F};
    bool loop_auto{true};
    Backdrop backdrop{Backdrop::Night};
    // Bloom strength in the preview (scene::RendererSettings::bloom_strength).
    float glow{0.6F};
    std::vector<Layer> layers;
    Uid next_uid{1U};

    [[nodiscard]] Uid allocate() { return next_uid++; }

    [[nodiscard]] Layer* layer(Uid uid);
    [[nodiscard]] const Layer* layer(Uid uid) const;
    // The layer holding the block, and the block's index in it.
    struct BlockPlace final {
        Layer* layer{};
        std::size_t index{};
    };
    [[nodiscard]] std::optional<BlockPlace> find_block(Uid uid);
    [[nodiscard]] const Block* block(Uid uid) const;

    friend bool operator==(const Document&, const Document&) = default;
};

// ------------------------------------------------------------ editing

// A block of `kind` with fresh uid and default values, or nothing for a kind
// the catalog does not have.
[[nodiscard]] std::optional<Block> make_block(Document& document, std::string_view kind);
// A Module block holding `module`.
[[nodiscard]] Block make_raw_block(Document& document, RawModule module);

// Inserts `block` into `layer` at `index` (clamped). Returns its uid.
Uid insert_block(Layer& layer, std::size_t index, Block block);
// Moves a block to `index` in `to` (which may be its own layer). The index is
// where it lands after it is taken out of its old place.
bool move_block(Document& document, Uid block, Uid to_layer, std::size_t index);
bool remove_block(Document& document, Uid block);
// A copy below the original, with fresh uids (so different random streams).
std::optional<Uid> duplicate_block(Document& document, Uid block);

// A copy of `layer` with fresh uids for it and its blocks.
[[nodiscard]] Layer copy_layer(Document& document, const Layer& layer);
bool remove_layer(Document& document, Uid layer);
bool move_layer(Document& document, Uid layer, std::size_t index);

} // namespace ajfx::studio
