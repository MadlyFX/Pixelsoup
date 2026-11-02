#pragma once

#include "document.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The blocks an artist builds a layer from. Each entry says what the block is
// for in a sentence, which controls it has and what they default to. The
// controls an artist needs first are Essential; the rest sit under "More".
// What a block turns into is the compiler's business (compile.cpp).
namespace ajfx::studio {

// The kind of layer a block, a control or one of a choice's options belongs
// in, as a set. A particle layer and a rigid layer are moved by different
// runtimes and accept different things, so the catalog says once which is
// which and the palette, the picker, the page and the compiler all read it
// from here. What is not offered cannot be authored, so a refusal the
// runtime would raise never reaches an artist.
inline constexpr std::uint8_t in_particles = 1U << 0U;
inline constexpr std::uint8_t in_rigid = 1U << 1U;
inline constexpr std::uint8_t in_any_layer = in_particles | in_rigid;
// Never offered: a definition the catalog holds for something other than a
// block on a layer's list (today, a rigid layer's own world settings).
inline constexpr std::uint8_t in_no_layer = 0U;

[[nodiscard]] constexpr std::uint8_t layer_bit(LayerKind kind) noexcept {
    return static_cast<std::uint8_t>(1U << static_cast<std::uint8_t>(kind));
}
[[nodiscard]] constexpr bool fits(std::uint8_t layers, LayerKind kind) noexcept {
    return (layers & layer_bit(kind)) != 0U;
}

// BlockKind::Module is the Module block, one raw ajfx module. BlockKind::Physics
// is the catalog entry a rigid layer's world settings are drawn and stored
// from (Layer::world); it is never offered as a block.

enum class Category : std::uint8_t {
    Birth,   // how many particles, where they start and how they leave
    Motion,  // the forces on them
    Look,    // how they are drawn
    Contact, // what they hit
    Physics, // solid pieces that collide with each other
    Light,   // light they give the scene
    Expert,  // raw modules
};

[[nodiscard]] std::string_view category_name(Category category) noexcept;

// Which part of a rigid physics layer a block acts on.
//
// A rigid layer is one physics world holding the layer's own pieces, the
// objects an artist stood in it, and the parts a piece comes apart into.
// Blocks for all of them sit in one list, so a flat page says nothing about
// which a block is for: Bodies is the pieces' material while an Object three
// rows down can carry its own, Break acts on the Object above it and not on
// the pieces, and Gravity is the whole world's. The page draws a rigid layer's blocks
// under these headings, so what a block acts on is read rather than worked
// out from what the compiler does with it. A particle layer has one scope
// and keeps its flat list.
enum class RigidScope : std::uint8_t {
    Objects,  // each thing standing in the world, owning the blocks under it
    Pieces,   // the layer's own pieces: how many, where, how big, how drawn
    Breaking, // when a piece comes apart, and what into
    World,    // the world every piece shares, and what they are made of
    Hood,     // a raw module, which may act on any of them
};

// The scopes the page draws, in the order it reads them: the things in the
// layer, then what the layer's own pieces are and what becomes of them, then
// the world it all happens in. A scope with nothing in it draws no heading.
inline constexpr std::array<RigidScope, 5U> rigid_scopes{RigidScope::Objects, RigidScope::Pieces,
                                                         RigidScope::Breaking, RigidScope::World,
                                                         RigidScope::Hood};

[[nodiscard]] std::string_view rigid_scope_name(RigidScope scope) noexcept;
// One sentence: what this part of a rigid layer is, for the heading's
// tooltip.
[[nodiscard]] std::string_view rigid_scope_help(RigidScope scope) noexcept;
// What a block of this kind acts on in a rigid physics layer, the world
// block among them. Every kind has an answer, a kind a rigid layer cannot
// hold included, since the catalog is asked before the layer is known.
[[nodiscard]] RigidScope rigid_scope(BlockKind kind) noexcept;
[[nodiscard]] RigidScope rigid_scope(std::string_view block_kind) noexcept;

enum class ControlType : std::uint8_t {
    Number,
    Range,
    Vec3,
    Color,
    Choice,
    Toggle,
    // A file on disk, its path in `text`: an image or a 3D model (`files`).
    File,
    // One of the page's solid layers, by its uid in `x`; 0 is every one.
    // The page offers its solid layers by name.
    SolidLayer,
};

// What a File control picks.
enum class FileKind : std::uint8_t {
    Image, // a picture for sprites: PNG, JPEG, TGA, BMP
    Model, // a 3D model: OBJ, glTF, GLB
};

enum class Level : std::uint8_t {
    Essential,
    Detail,
};

struct ChoiceDef final {
    std::string_view id;
    std::string_view label;
    // The layer kinds this option is offered in: a response the rigid
    // runtime has no answer for is not on the list there.
    std::uint8_t layers{in_any_layer};
};

// Shown only while another control of the block holds (or, with `negate`,
// does not hold) one of `values`. A switch reads as "on" or "off".
struct Condition final {
    std::string_view control;
    std::array<std::string_view, 3> values{};
    bool negate{false};
};

struct ControlDef final {
    std::string_view id;
    std::string_view label;
    ControlType type{ControlType::Number};
    Value fallback{};
    // The range a slider covers, and the range a typed value is held to.
    float min{0.0F};
    float max{1.0F};
    float hard_min{-1.0e9F};
    float hard_max{1.0e9F};
    std::string_view unit;
    int decimals{2};
    std::vector<ChoiceDef> choices;
    Level level{Level::Essential};
    std::string_view help;
    std::optional<Condition> shown_if;
    // Can be published for a host to set (see Published): the control maps
    // onto one runtime input, so a host's value means what the control does.
    bool publishable{false};
    // When a published value takes effect (ajfx::ParameterTiming). Bake-time
    // controls are read by the simulation -- an amount, a speed, a force --
    // so a cache is taken with one value of each and a baked layer holds
    // them where they are set. Playback-time controls (Appearance's tint,
    // brightness, opacity and size scale) are applied as particles are
    // drawn, baked or not, so an operator may always change them.
    ParameterTiming timing{ParameterTiming::Bake};
    // A range that was once one number: a page saved then gives the number
    // as its high end, from 0 (a spread of 20 is 0 to 20), rather than as
    // both ends.
    bool number_is_high{false};
    // For a File control.
    FileKind files{FileKind::Image};
    // The layer kinds this control is shown and stored in. A control the
    // kind leaves out is hidden on the page and cleared from the block
    // (conform_block), so a block never carries a setting an artist cannot
    // see and the compiler cannot act on one.
    std::uint8_t layers{in_any_layer};
    // The layer kinds a Range control shows as one number in, both ends
    // together: where a runtime takes one value for the whole layer, a
    // spread would promise a variation it does not deliver.
    std::uint8_t one_number{in_no_layer};
};

struct BlockDef final {
    BlockKind kind{BlockKind::Stream};
    std::string_view title;
    // One sentence: what the block does, in the artist's words.
    std::string_view description;
    Category category{Category::Birth};
    std::vector<ControlDef> controls;
    // Extra words the block picker matches.
    std::string_view keywords;
    // At most one of these in a layer.
    bool unique{false};
    // Can be turned into the Module blocks it compiles to.
    bool detachable{true};
    // The layer kinds this block may be used in. A block outside the
    // working layer's kind is not offered by the palette or the picker and
    // cannot be dropped there, so the layer's kind alone decides what a
    // page can be built from.
    std::uint8_t layers{in_any_layer};
    // Why this block is not in a layer of a kind it leaves out, in the
    // artist's words, for the palette's tooltip. Empty for a block that is
    // in every kind.
    std::string_view only_in;
};

[[nodiscard]] const std::vector<BlockDef>& block_catalog();
[[nodiscard]] const BlockDef* find_block_def(std::string_view kind);
[[nodiscard]] const BlockDef* find_block_def(BlockKind kind);
[[nodiscard]] const ControlDef* find_control(const BlockDef& def, std::string_view id);
// When a control of a block of this kind takes effect, if published: bake
// time for anything the catalog does not know.
[[nodiscard]] ParameterTiming control_timing(std::string_view block_kind, std::string_view control);

// Whether a block of this kind belongs in a layer of that kind.
[[nodiscard]] bool block_fits(BlockKind block_kind, LayerKind layer) noexcept;
[[nodiscard]] bool block_fits(std::string_view block_kind, LayerKind layer) noexcept;
// The options a choice offers in a layer of this kind, in catalog order.
[[nodiscard]] std::vector<ChoiceDef> choices_in(const ControlDef& control, LayerKind layer);
// Clears every value a layer of this kind hides, and squares a range the
// kind shows as one number, so a block carries only what its layer can act
// on. Called wherever a block arrives in a layer or a layer changes kind.
void conform_block(Block& block, LayerKind layer);

// A control's value: what the block stores, else its default.
[[nodiscard]] Value value_of(const Block& block, const ControlDef& control);
[[nodiscard]] float number_of(const Block& block, std::string_view control);
[[nodiscard]] Value get(const Block& block, std::string_view control);
[[nodiscard]] std::string choice_of(const Block& block, std::string_view control);
[[nodiscard]] bool toggle_of(const Block& block, std::string_view control);
// A File control's path, empty when none is picked.
[[nodiscard]] std::string file_of(const Block& block, std::string_view control);
// The file name a path ends in, for reading on the page: "rock.glb".
[[nodiscard]] std::string file_label(std::string_view path);

// Clamps to the control's hard range; a choice not on the list falls back.
[[nodiscard]] Value sanitize(const ControlDef& control, Value value);

// Whether the page draws this control: its condition holds, and the layer's
// kind has it at all.
[[nodiscard]] bool control_visible(const Block& block, const BlockDef& def, const ControlDef& control, LayerKind layer);

// A short reading of the block's values, shown beside its title.
[[nodiscard]] std::string block_summary(const Block& block);
[[nodiscard]] std::string block_title(const Block& block);

// "0.25", "12", "1.5 m".
[[nodiscard]] std::string format_number(float value, int decimals);
// A range as words: "2 m/s" when both ends agree, else "1 to 2 m/s".
[[nodiscard]] std::string format_range(const Value& range, int decimals, std::string_view unit);

} // namespace ajfx::studio
