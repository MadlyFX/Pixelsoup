#pragma once

#include "document.hpp"

#include <array>
#include <optional>
#include <string_view>
#include <vector>

// Blowing things up: what a solid thing is made of, and "Explode this".
//
// An Explosion block is the page's blast (compile.hpp, page_blasts). What
// lives here is what an artist reaches for around it:
// - **Materials.** An Object's "Made of" is Concrete, Wood, Glass or Metal.
//   Picking one writes that material's weight, grip and bounce into the
//   Object, and how hard a hit or a blast must be to break it and how many
//   pieces it cuts into into the Break under it. They are ordinary values
//   afterwards, each with its reset arrow, so a material is a starting
//   point rather than a lock.
// - **Explode this.** One edit that takes a Solid layer's Object and blows
//   it up: the Object is cut into pieces that shatter when blasted, an
//   Explosion goes off at its middle, and a fireball, sparks, smoke and dust
//   are added as layers of their own, all sized to the Object.
namespace ajfx::studio {

// One material a solid thing can be made of. Speeds are in m/s, as the
// Break block's are; `pieces` is what its Break cuts it into.
struct Material final {
    std::string_view id;
    std::string_view name;
    float density{2400.0F};
    float friction{0.6F};
    float bounce{0.15F};
    // A hit at this speed breaks it (Break's "Comes apart when hit at").
    float breaks_at{4.0F};
    // A blast that throws it this hard shatters it (Break's "Breaks when
    // blasted at").
    float blasted_at{8.0F};
    float pieces{8.0F};
};

// Concrete, Wood, Glass and Metal, as the Object's "Made of" offers them.
[[nodiscard]] const std::array<Material, 4>& materials();
// The material an id names, or null (for "layer", the layer's own).
[[nodiscard]] const Material* find_material(std::string_view id);

// Writes the material the Object's "Made of" names into it (its own weight,
// grip and bounce) and into the Break blocks under it (how hard it must be
// hit or blasted to break, and how many pieces it cuts into). "The layer's"
// gives it the layer's material again. False when `object` is not an Object.
bool apply_material(Document& document, Uid object);

// What "Explode this" made.
struct Exploded final {
    // The Object blown up, and the Break that cuts it.
    Uid object{};
    Uid breaking{};
    // The Explosion block, in the Object's layer.
    Uid explosion{};
    // The Glows hot block that makes its broken pieces glow and cool, in the
    // Object's layer.
    Uid heat{};
    // The fireball, sparks, smoke and dust layers, then the dust where its
    // pieces land and the embers trailing them, in that order.
    std::vector<Uid> layers;
};

// The Object "Explode this" acts on in a Solid layer: `object` where that is
// one of the layer's Objects, else the layer's first. 0 where there is none,
// or the layer is not rigid.
[[nodiscard]] Uid explodable_object(const Document& document, Uid layer, Uid object = 0U);

// Blows up the Object (see explodable_object), as one edit:
// - it is let be knocked about, and solid where it was hollow, since only a
//   piece the world moves can come apart;
// - its Break cuts its model into its material's pieces (Concrete's where
//   it has none) and shatters when blasted; one is added under it where it
//   has none;
// - an Explosion goes off at its middle half a second after it is let go,
//   strong enough to shatter it and reaching four times its size;
// - Fireball, Sparks, Smoke and Dust layers are added after its layer, born
//   at its middle just before the blast so the blast throws them, all
//   sized to it;
// - and what its pieces make as they fly: they glow hot and cool (a Glows
//   hot block on the broken pieces), shed embers as they move (a Trail
//   layer) and kick up dust where they land (a Puffs where things hit
//   layer), both reading its layer.
// Nothing when there is no Object to blow up.
std::optional<Exploded> explode_this(Document& document, Uid layer, Uid object = 0U);

} // namespace ajfx::studio
