#pragma once

#include "document.hpp"
#include "user_assets.hpp"

#include <ajfx/effect.hpp>
#include <ajfx/field.hpp>
#include <ajfx/mesh.hpp>
#include <ajfx/package.hpp>
#include <ajfx/texture.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Turns a Studio page into an ajfx effect.
//
// The compiler is where Studio's hand-holding lives:
// - Each block's modules go to the stage and slot the runtime needs (forces,
//   then turning and collisions, then appearance over life), so block order
//   on the page never changes the result.
// - What several blocks say about one thing is merged: Color's "burns out"
//   and Fade's fade-out become one colour-over-life curve.
// - Every layer's capacity is the most particles it can ever have alive,
//   worked out from its emitters and life, so nobody sets a budget by hand
//   and none is exceeded.
// - Module ids and seeds come from block and layer uids, so editing one block
//   leaves the random streams of every other where they were.
// - Page scale multiplies distances, sizes, speeds and accelerations.
// A layer that cannot compile is reported and left out of the preview; the
// rest still play.
namespace ajfx::studio {

enum class Purpose : std::uint8_t {
    Preview, // honours solo
    Export,  // every enabled layer
};

// The page's solid layers, played as one physics world: one rigid physics
// emitter holding every playing solid layer's bodies, so loose pieces of one
// layer knock into another's.
// - **Each layer keeps its own look.** Its init modules, its colour and size
//   over life and its heat are scoped (ajfx::Scope) to its own body kinds,
//   so a layer's pieces start, live and look as they did on their own. A
//   layer's own kind 0 becomes a kind of the world's.
// - **Order-free and local.** The world is the lowest-uid layer's emitter
//   (its name, seed, renderer, world settings and kind 0); every other
//   layer's kinds and props follow in uid order, and the spawn stack is in
//   the order of the blocks' uids, so moving layers changes nothing and
//   adding a solid layer leaves the others' spawn sources and kinds where
//   they were.
// - **What is one per world** -- gravity, ground, damping, the material of
//   the world's own pieces, how it draws -- is the lowest-uid layer's;
//   another layer's forces and colliders that differ from it are added.
// Particle layers meet it (BodyCollision) when it is small enough to be met,
// and read its contacts and bodies (SpawnFromContacts, SpawnFromBodies).
struct SolidWorld final {
    // Empty when no solid layer plays.
    std::string emitter;
    std::uint32_t capacity{0U};
    // Whether anything in it moves: something is born in it, rather than
    // only standing there.
    bool moves{false};
    struct Member final {
        Uid layer{};
        // The world's particles that are this layer's: empty where the world
        // is this layer alone.
        Scope bodies;

        friend bool operator==(const Member&, const Member&) = default;
    };
    // In the order of their layers' uids.
    std::vector<Member> layers;

    // Whether particles can meet its bodies (ajfx::max_body_colliders).
    [[nodiscard]] bool meetable() const noexcept {
        return !emitter.empty() && capacity <= max_body_colliders;
    }
    [[nodiscard]] const Member* member(Uid layer) const noexcept {
        for (const Member& m : layers) {
            if (m.layer == layer) {
                return &m;
            }
        }
        return nullptr;
    }

    friend bool operator==(const SolidWorld&, const SolidWorld&) = default;
};

// One module of the compiled effect, and the block (or, for what the
// compiler made from the layer itself, the layer) it came from.
struct ModuleLine final {
    Uid origin{};
    RawModule module;
};

struct LayerReport final {
    Uid layer{};
    // The emitter it plays in. Every solid layer plays in the page's one
    // world (SolidWorld), so several reports may name one emitter; `scope`
    // says which of its particles are this layer's (empty: all of them).
    std::string emitter;
    Scope scope;
    // The capacity of that emitter, which is the layer's own unless it
    // shares one.
    std::uint32_t emitter_capacity{0U};
    // Played: enabled, not muted by another layer's solo, and compiled.
    bool included{false};
    std::uint32_t capacity{0U};
    // Bytes of particle state at that capacity.
    std::uint64_t bytes{0U};
    std::vector<ModuleLine> modules;
    SpriteRenderer renderer;
    // The parameters its blocks publish.
    std::vector<std::string> parameters;
    // Baked rather than live (Layer::baked).
    bool baked{false};
    // A published control a baked layer holds where it is set rather than
    // publishing: the simulation reads it, and a cache cannot follow a value
    // its simulation reads. Its playback-time controls stay published.
    struct Held final {
        Uid block{};
        std::string control;
        std::string name;
    };
    std::vector<Held> held;
    // A solid layer's props (RigidPhysics::props), by the Object each one
    // is, in the order its world lists them: what the preview picks a prop
    // by.
    std::vector<Uid> props;
    // The bursts of its Objects that appear part way through ("Appears
    // at"), by module id: each plays out as a thing let go, not as a burst
    // that lives its whole life (natural_loop_seconds).
    std::vector<ModuleId> appearing;
    // Why the layer could not compile, and the block at fault when known.
    std::string error;
    Uid error_block{};
};

// A file of the artist's the package needs, by the entry it goes in. The
// compiler only names it; the builder reads and prepares it off the
// interface thread, and fills `Compiled::textures`, `meshes` and `fields`.
struct AssetRequest final {
    enum class Kind : std::uint8_t {
        Texture, // a sprite picture
        Mesh,    // a debris model
        Field,   // a model to collide with, baked to a distance field
    };
    Kind kind{Kind::Texture};
    std::string entry;
    std::string path;
    Uid layer{};
    Uid block{};
    // A field's: the model's scale (page scale included), samples along its
    // longest side, and whether it is solid.
    float scale{1.0F};
    std::uint32_t cells{0U};
    ColliderSolid solid{ColliderSolid::Auto};

    friend bool operator==(const AssetRequest&, const AssetRequest&) = default;
};

// A Break block set to cut its model rather than to use the Part blocks
// beside it. The compiler only names the cut, since the model's geometry is
// not read until the builder reads it; the builder cuts and puts the cells
// into the effect (see PackageBuilder::resolve_assets).
struct FractureCut final {
    Uid layer{};
    Uid block{};
    // The emitter whose bodies come apart, and the mesh entry to cut.
    std::string emitter;
    std::string model;
    // The body kind that comes apart: the Object's own, since a layer may
    // hold several and each cuts its own model.
    std::uint32_t body{0U};
    ModuleId id{};
    std::uint32_t cells{8U};
    std::uint32_t seed{0U};
    float spread{0.0F};
    float threshold{4.0F};
    // How hard a blast must throw it to shatter it (Break::blast_threshold);
    // 0 leaves blasts to push it.
    float blast_threshold{0.0F};
    float scatter{1.0F};
    float spin{180.0F};

    friend bool operator==(const FractureCut&, const FractureCut&) = default;
};

// An Explosion on the page, as the blast it compiles to (ajfx::Blast). It is
// the page's rather than its layer's: every layer that plays and does not
// pass through answers it with a Shockwave, so the dust, the sparks and the
// pieces all follow one place, time and power.
// - Named from its block's uid (`blast_name`), so moving or adding blocks
//   and layers renames nothing.
// - In the order of the blocks' uids, not the page's.
// - Page scale applies to its place, power and reach.
// - Present while the block and its layer are switched on. Soloing another
//   layer leaves it going off, since what is soloed is still blasted.
struct PageBlast final {
    Uid block{};
    Uid layer{};
    Blast blast;

    friend bool operator==(const PageBlast&, const PageBlast&) = default;
};

[[nodiscard]] std::string blast_name(Uid block);
// The page's blasts, at most ajfx::max_blasts of them.
[[nodiscard]] std::vector<PageBlast> page_blasts(const Document& document, float scale);

struct Compiled final {
    // Set when at least one layer compiled.
    std::optional<Effect> effect;
    std::vector<LayerReport> layers;
    // The library textures and meshes the effect names.
    TextureSet textures;
    MeshSet meshes;
    FieldSet fields;
    // The artist's files the played layers name, for the builder to read.
    std::vector<AssetRequest> requests;
    // The cuts the builder makes once it has the models they cut.
    std::vector<FractureCut> cuts;
    // The page's solid layers as the one world they play in.
    SolidWorld solids;
    // Empty when every included layer compiled; else the first problem.
    std::string error;

    [[nodiscard]] const LayerReport* layer(Uid uid) const;
    [[nodiscard]] std::uint32_t total_capacity() const;
};

[[nodiscard]] Compiled compile(const Document& document, Purpose purpose);

// Makes the cuts `compiled` asks for, which needs the models they cut: the
// caller reads those into `compiled.meshes` first, so this runs after the
// artist's files are resolved and not when the layers compile. Each cut adds
// a body kind and a mesh per cell and the Break module that scatters them. A
// cut that cannot be made leaves its layer playing unbroken and says why in
// `issue`, and what the cuts made is validated before it goes anywhere.
void make_cuts(Compiled& compiled, std::string& issue);

// The texture, mesh and field entries a package of the compiled effect needs.
[[nodiscard]] std::vector<PackageEntry> asset_entries(const Compiled& compiled);

// The Module blocks a block compiles to, for "Detach": its own modules with
// fresh uids. Empty for a block that has none or is not detachable. Each
// carries the block it came from (Block::detached), so it can be recombined.
[[nodiscard]] std::vector<Block> detach_block(Document& document, const Compiled& compiled, Uid block);

// What a Module block was detached from, for recombining it.
struct DetachedGroup final {
    Uid from{};
    std::string title;
    // Module blocks of the detach still in the layer.
    std::size_t modules{0U};
    // Some were edited or deleted since: recombining puts back the block as
    // it was, dropping those edits.
    bool changed{false};
};
[[nodiscard]] std::optional<DetachedGroup> detached_group(const Document& document, Uid block);
// Replaces every Module block detached with `block` by the block they came
// from, where the first of them stands. Returns the block's uid.
std::optional<Uid> recombine_block(Document& document, Uid block);

// The seconds the effect takes to play out, from its emitters: a burst
// until its last particle dies, a stream long enough to build up and show a
// whole generation at its full. At least a second, at most two minutes.
[[nodiscard]] float natural_loop_seconds(const Compiled& compiled);
// The preview's loop: the natural one, or the page's own when it sets one.
[[nodiscard]] float loop_seconds(const Document& document, const Compiled& compiled);

} // namespace ajfx::studio
