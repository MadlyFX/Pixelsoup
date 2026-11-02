#pragma once

#include "block_kind.hpp"
#include "scene.hpp"

#include <ajfx/modules.hpp>

#include <array>
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

// What moves a layer's particles, chosen when the layer is made. The runtime
// cannot mix them: in a rigid layer a physics engine owns every particle's
// motion, so the forces and per-particle curves a particle layer is built
// from cannot act (ajfx::check_rigid_physics). Rather than let an artist
// build something the runtime will refuse, the kind decides which blocks the
// palette and the picker offer at all, and `catalog.hpp` says which is which.
enum class LayerKind : std::uint8_t {
    Particles, // the GPU simulation: forces, collisions, curves over life
    Rigid,     // a physics engine: solid bodies that collide with each other
};

[[nodiscard]] std::string_view layer_kind_name(LayerKind kind) noexcept;
// One sentence: what a layer of this kind is, for the artist choosing one.
[[nodiscard]] std::string_view layer_kind_description(LayerKind kind) noexcept;
[[nodiscard]] std::string_view layer_kind_id(LayerKind kind) noexcept;
[[nodiscard]] LayerKind layer_kind_from(std::string_view id) noexcept;

// What a layer is, in the artist's words: what "Add layer" asks ("What is
// it?") in place of the kind. It decides the kind (a Solid is moved by a
// physics engine, everything else by forces) and how the page's world acts
// on the layer -- how much of the page's gravity and wind it feels, and what
// it does where it meets the ground or anything solid: sparks bounce, dust
// sticks, smoke slides (`world_share`). Something else is a particle layer
// built by hand: the page's gravity leaves it alone unless a block says so.
// The kind can still be changed on its own, under Advanced.
enum class LayerWhat : std::uint8_t {
    Other,
    Solid,
    Sparks,
    Debris,
    Fire,
    Smoke,
    Dust,
    Rain,
    Snow,
    Magic,
};

// In the order "What is it?" offers them.
inline constexpr std::array<LayerWhat, 10U> layer_whats{
    LayerWhat::Solid, LayerWhat::Sparks, LayerWhat::Fire, LayerWhat::Smoke, LayerWhat::Dust,
    LayerWhat::Debris, LayerWhat::Rain, LayerWhat::Snow, LayerWhat::Magic, LayerWhat::Other,
};

[[nodiscard]] std::string_view layer_what_name(LayerWhat what) noexcept;
// A few words on what it does, for a tooltip.
[[nodiscard]] std::string_view layer_what_description(LayerWhat what) noexcept;
[[nodiscard]] std::string_view layer_what_id(LayerWhat what) noexcept;
[[nodiscard]] std::optional<LayerWhat> layer_what_from(std::string_view id) noexcept;
// The kind a layer of this is made as.
[[nodiscard]] LayerKind layer_what_kind(LayerWhat what) noexcept;

// How the palette and the pickers name an empty layer of what it is, and how
// that name is read back. One pair, so the side that offers a layer and the
// side that makes one cannot drift apart: an id neither recognises would
// make a layer with no uid, and every widget on it would collide with every
// other such layer. "" is Something else.
[[nodiscard]] std::string empty_layer_id(LayerWhat what);
[[nodiscard]] std::optional<LayerWhat> empty_layer_what(std::string_view id) noexcept;

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
    LayerKind kind{LayerKind::Particles};
    // What the layer is, which sets how the page's world acts on it.
    LayerWhat what{LayerWhat::Other};
    // Baked: worked out once and played back from a cache (ajfx/cache.hpp)
    // rather than simulated wherever it plays -- a rigid layer's bodies to
    // the bit, a particle layer's particles quantised. Live otherwise. A
    // baked layer holds its bake-time published controls where they are
    // set, since a cache cannot follow a value its simulation reads. A
    // solid layer is made baked (make_layer, set_layer_kind) and a particle
    // layer live; a page read from a file keeps what it says.
    bool baked{false};
    // A rigid layer's physics world: one body shape, friction, bounciness,
    // density, spin damping and rest, held as a block of the catalog's
    // "physics" definition so that it stores, draws, publishes and undoes
    // exactly as any block does. Present only while `kind` is Rigid; a
    // particle layer has no world. It is the layer's, not a block on its
    // list, because there is one world per layer and no way to be without
    // it: that is what the kind means.
    std::optional<Block> world;
    std::vector<Block> blocks;

    friend bool operator==(const Layer&, const Layer&) = default;
};

// What happens where a layer meets the ground or something solid.
enum class Contact : std::uint8_t {
    Bounce, // springs back by `bounce`, and slides with `friction`
    Slide,  // stops falling and slides with `friction`
    Stick,  // stops where it lands
    Vanish, // is gone (particles only)
};

// How the page's world acts on a layer, from what it is (Layer::what):
// - `gravity` and `wind` are the shares of the page's gravity and wind it
//   feels. Smoke and flames rise by their own blocks, so they feel none of
//   the page's gravity; dust and snow drift, so they feel a little.
// - `contact`, `bounce` and `friction` are what it does at the page's ground
//   and at every solid thing on the page. A layer's own Ground block says
//   both instead.
// A rigid layer always feels the whole of the page's gravity: a physics
// engine has no lighter body.
struct WorldShare final {
    float gravity{0.0F};
    float wind{0.0F};
    Contact contact{Contact::Slide};
    float bounce{0.0F};
    float friction{0.2F};
    // How hard an Explosion throws it, as a share of the blast's own speed,
    // each particle picking its own between the two: sparks fly, smoke and
    // flame are pushed less. A solid piece takes the blast as it is, so
    // whether it breaks reads straight off the blast.
    float blast_min{1.0F};
    float blast_max{1.0F};

    friend bool operator==(const WorldShare&, const WorldShare&) = default;
};

[[nodiscard]] WorldShare world_share(const Layer& layer) noexcept;

// The world every layer of the page plays in: a ground, gravity and wind,
// the page's rather than each layer's. The compiler gives them to every layer
// by its share (world_share); a layer's own Ground or Gravity block stands
// in for the page's, and a Passes through block opts it out of the solids
// (and, if it says so, the ground).
struct PageWorld final {
    // A ground at `ground_height` metres.
    bool ground{true};
    float ground_height{0.0F};
    // In g: 1 is Earth's.
    float gravity{1.0F};
    // A steady push, m/s^2, toward `wind_heading` degrees (0 is +X, 90 is
    // -Z, as the Wind block's heading).
    float wind{0.0F};
    float wind_heading{0.0F};

    friend bool operator==(const PageWorld&, const PageWorld&) = default;
};

// What the preview stands the effect on. Never exported.
enum class Backdrop : std::uint8_t {
    Studio, // a lit grey floor and a 1.8 m figure for scale
    Night,  // the same floor under a dim moonlit rig, for glowing effects
    Void,   // nothing: black, no floor
};

// The page's frame-time budget (Document::budget_ms): a share of a 60 Hz
// frame left for the effect once the show around it has drawn.
inline constexpr float default_budget_ms = 4.0F;
inline constexpr float min_budget_ms = 0.1F;
inline constexpr float max_budget_ms = 100.0F;

struct Document final {
    // New scenes have one typed authority. `layers` is only for the explicit
    // legacy importer and the compiler's transient IR, never a second editor.
    std::optional<Scene> scene;
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
    // Milliseconds of GPU a frame the effect may take, simulation and
    // drawing together, as the preview measures it. "Maximize budget"
    // (budget.hpp) scales the page's amounts to fill it. Never exported:
    // it is the artist's target, not part of the effect.
    float budget_ms{default_budget_ms};
    // The ground, gravity and wind every layer plays in.
    PageWorld world;
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
    // Any block on the page, a rigid layer's world among them.
    [[nodiscard]] const Block* block(Uid uid) const;
    [[nodiscard]] Block* block(Uid uid);
    // The layer whose world block this is, or null.
    [[nodiscard]] Layer* world_of(Uid block);

    friend bool operator==(const Document&, const Document&) = default;
};

// ------------------------------------------------------------ editing

// A block of `kind` with fresh uid and default values, or nothing for a kind
// the catalog does not have.
[[nodiscard]] std::optional<Block> make_block(Document& document, std::string_view kind);
[[nodiscard]] std::optional<Block> make_block(Document& document, BlockKind kind);
// An empty layer of `kind`, with its world where the kind has one. It is
// Something else, unless rigid, when it is a Solid.
[[nodiscard]] Layer make_layer(Document& document, LayerKind kind, std::string name);
// An empty layer of what it is, of the kind that is.
[[nodiscard]] Layer make_layer(Document& document, LayerWhat what, std::string name);
// Says what the layer is, and gives it the kind that goes with it. Refused,
// changing nothing, where a block the kind cannot hold is in the way
// (`blocks_against_kind` names them).
bool set_layer_what(Document& document, Layer& layer, LayerWhat what);
// Gives `layer` the kind: adds or drops its world and conforms every block,
// so nothing is left holding a setting its new kind hides. Blocks the kind
// does not have at all are refused rather than dropped, so nothing an artist
// authored is lost: `blocks_against_kind` names them first.
bool set_layer_kind(Document& document, Layer& layer, LayerKind kind);
// The blocks of `layer` that a layer of `kind` cannot hold, by title, in
// page order. Empty when the layer may become that kind.
[[nodiscard]] std::vector<std::string> blocks_against_kind(const Layer& layer, LayerKind kind);
// A Module block holding `module`.
[[nodiscard]] Block make_raw_block(Document& document, RawModule module);

// Whether a block of this kind belongs to the Object above it rather than to
// the layer: breaking is a thing's own, where births, looks and the world are
// the layer's.
[[nodiscard]] bool owned_by_object(std::string_view block_kind) noexcept;
// The Object block governing the block at `index`: the nearest "object" block
// before it, or nothing where none stands above it. An Object owns the
// breaking blocks that follow it until the next Object, which is how a Break
// says which thing comes apart without naming it -- the same page-order idiom
// that decides which layer's pieces a particle lands on.
[[nodiscard]] std::optional<std::size_t> object_of(const Layer& layer, std::size_t index);

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
