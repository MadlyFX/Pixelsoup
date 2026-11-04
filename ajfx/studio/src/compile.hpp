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
#include <string_view>
#include <vector>

// Turns a Studio scene into an ajfx effect (compile.cpp).
//
// - Every simulating particle system is an emitter of its own, named for its
//   item ("item_<uid>"). Its Motion stack, the forces that act on it, the
//   rules that feed it and the Static objects it meets become its modules,
//   each in the stage and slot the runtime needs, so the order of the tree
//   never changes the result.
// - Every simulating Object is part of the scene's one solid world: one
//   rigid physics emitter, named for its lowest-identity Object, holding
//   every Object's bodies (a body kind each) and standing things (a prop
//   each), so loose things of one Object knock into another's.
// - Smoke grids are the effect's volumes and blasts its blasts, which every
//   emitter that does not pass through answers.
// - Module ids and seeds come from the items' reserved identities (scene.cpp),
//   so editing one item leaves every other's random streams where they were.
// - Every emitter's capacity is the most it can ever have alive, worked out
//   from what bears into it and how long it lives, so no budget is set by
//   hand and none is exceeded. Scene scale multiplies distances, sizes,
//   speeds and accelerations.
// A scene that cannot play whole does not play: the first item at fault is
// named and there is no effect, so a preview is never replaced by a
// plausible incomplete one.
namespace ajfx::studio {

// The scene's Objects as the one physics world they play in. Particle
// systems meet it (BodyCollision) when it is small enough to be met, and are
// born of its contacts and bodies (SpawnFromContacts, SpawnFromBodies).
// - **Each Object keeps its own look and launch.** Its lifetime, launch and
//   size are scoped (ajfx::Scope) to its own body kinds.
// - **Order-free and local.** The world is the lowest-identity Object's
//   emitter (its name, seed, material and world settings); every other
//   Object's kinds and props follow in identity order, so adding an Object
//   leaves the others' spawn sources and kinds where they were.
struct SolidWorld final {
    // Empty when no Object plays.
    std::string emitter;
    std::uint32_t capacity{0U};
    // Whether anything in it moves: something is born in it, rather than
    // only standing there.
    bool moves{false};
    struct Member final {
        Uid item{};
        // The world's particles that are this Object's: empty where the
        // world is this Object alone.
        Scope bodies;

        friend bool operator==(const Member&, const Member&) = default;
    };
    // In identity order.
    std::vector<Member> items;
    // Where drops are born (ajfx::SpawnActions, ScenePhysics::max_dropped):
    // its place in the world's spawn stack, and the most dropped bodies it
    // holds at once. None where the scene takes no drops.
    std::optional<std::uint32_t> drops;
    std::uint32_t max_dropped{0U};
    // The dropped things one step may make: what a click a frame could ask.
    static constexpr std::uint32_t drops_per_step = 16U;

    // Whether particles can meet its bodies (ajfx::max_body_colliders).
    [[nodiscard]] bool meetable() const noexcept {
        return !emitter.empty() && capacity <= max_body_colliders;
    }
    [[nodiscard]] const Member* member(Uid item) const noexcept {
        for (const Member& m : items) {
            if (m.item == item) {
                return &m;
            }
        }
        return nullptr;
    }

    friend bool operator==(const SolidWorld&, const SolidWorld&) = default;
};

// What one scene item became: every Object, particle system and smoke grid
// has one, playing or not.
struct ItemReport final {
    Uid item{};
    // The emitter it plays in. Every Object plays in the scene's one world
    // (SolidWorld), so several reports may name one emitter; `scope` says
    // which of its particles are this item's (empty: all of them). Empty for
    // a smoke grid, which builds no emitter.
    std::string emitter;
    Scope scope;
    // The capacity of that emitter, which is the item's own unless it
    // shares one.
    std::uint32_t emitter_capacity{0U};
    // Played: it simulates and compiled.
    bool included{false};
    // Its own share of that capacity, and the bytes of particle state at it.
    std::uint32_t capacity{0U};
    std::uint64_t bytes{0U};
    // Baked rather than live: worked out once and played back from a cache.
    // Everything but a smoke grid and the system a preview plays live.
    bool baked{false};
    // The ids of the spawn modules it made in its emitter, in their order.
    std::vector<ModuleId> spawns;
    // Whether it stands in the world as a prop (RigidPhysics::props) rather
    // than as bodies. The world's props are its Objects' in identity order.
    bool prop{false};
    // Why it could not compile.
    std::string error;
};

// A file of the artist's the package needs, by the entry it goes in. The
// compiler only names it; the builder reads and prepares it off the
// interface thread, and fills `Compiled::meshes` and `fields`.
struct AssetRequest final {
    enum class Kind : std::uint8_t {
        Mesh,  // an Object's model
        Field, // a Static object's model to collide with, baked to a distance field
    };
    Kind kind{Kind::Mesh};
    std::string entry;
    std::string path;
    // The item whose file it is.
    Uid item{};
    // A field's: the model's scale (scene scale included), samples along its
    // longest side, and whether it is solid.
    float scale{1.0F};
    std::uint32_t cells{0U};
    ColliderSolid solid{ColliderSolid::Auto};
    // A field's model centred on its bounds with its longest side 1 before
    // `scale`, as an object's model is drawn, and turned by `turn` (degrees,
    // prop_orientation): the field stands where the object does.
    bool fit{false};
    Vec3 turn{};

    friend bool operator==(const AssetRequest&, const AssetRequest&) = default;
};

// An Object that comes apart (Object::fracture), cut into pieces of its
// model. The compiler only names the cut, since the model's geometry is not
// read until the builder reads it; make_cuts makes it.
struct FractureCut final {
    Uid item{};
    // The emitter whose bodies come apart, and the mesh entry to cut.
    std::string emitter;
    std::string model;
    // The body kind that comes apart: the Object's own.
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

// The name a blast's identity has in the effect (ajfx::Blast::name).
[[nodiscard]] std::string blast_name(Uid identity);
// A smoke grid's volume is named from its identity (smoke_grid_volume):
// the identity a name came from, or 0 for a name that is not one.
[[nodiscard]] std::string volume_name(Uid identity);
[[nodiscard]] Uid volume_identity(std::string_view name);
// The identity whose volume a runtime message names, as `volume 'grid_<n>'`,
// or 0 when it names none.
[[nodiscard]] Uid volume_named_in(std::string_view message);

struct Compiled final {
    // Set when the scene compiled whole and something plays.
    std::optional<Effect> effect;
    // In identity order.
    std::vector<ItemReport> items;
    // The library textures and meshes the effect names.
    TextureSet textures;
    MeshSet meshes;
    FieldSet fields;
    // The artist's files the effect names, for the builder to read.
    std::vector<AssetRequest> requests;
    // The cuts the builder makes once it has the models they cut.
    std::vector<FractureCut> cuts;
    SolidWorld solids;
    // Empty when the scene compiled; else the first problem, naming its item.
    std::string error;

    [[nodiscard]] const ItemReport* item(Uid uid) const;
    [[nodiscard]] std::uint32_t total_capacity() const;
};

// `live` names a particle system whose Motion controls are compiled as
// published parameters (live_parameters, scene.hpp), for a preview that
// changes them without a new package; 0 for none.
[[nodiscard]] Compiled compile(const Document& document, std::uint32_t live = 0U);

// Makes the cuts `compiled` asks for, which needs the models they cut: the
// caller reads those into `compiled.meshes` first, so this runs after the
// artist's files are resolved and not when the scene compiles. Each cut adds
// a body kind and a mesh per cell and the Break module that scatters them. A
// cut that cannot be made leaves its Object playing unbroken and says why in
// `issue`, and what the cuts made is validated before it goes anywhere.
void make_cuts(Compiled& compiled, std::string& issue);

// The texture, mesh and field entries a package of the compiled effect needs.
[[nodiscard]] std::vector<PackageEntry> asset_entries(const Compiled& compiled);

// The seconds the preview plays before it loops: the scene's range.
[[nodiscard]] float loop_seconds(const Document& document);

} // namespace ajfx::studio
