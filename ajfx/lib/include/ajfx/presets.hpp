#pragma once

#include <ajfx/effect.hpp>
#include <ajfx/mesh.hpp>
#include <ajfx/texture.hpp>

// Ready-made effects: a starting point to tweak, and examples of the builder.
// Each publishes its most useful knobs as parameters.
namespace ajfx::presets {

// Hot sparks thrown upwards, falling and bouncing off the ground plane.
// Parameters: rate (sparks per second), color.
[[nodiscard]] Effect sparks();

// Slow, swirling, fading smoke rising from a disc: soft puffs from the
// "puff" flipbook, depth-sorted, fading where they meet the scene.
// Parameters: rate, wind (vec3 acceleration).
[[nodiscard]] Effect smoke();

// Repeating bursts of confetti in two colours, tumbling down.
// Parameters: color_a, color_b.
[[nodiscard]] Effect confetti();

// Bursts of stone chips thrown up from the ground plane, tumbling, bouncing
// and rolling to rest before they fade: rigid "chip" mesh particles.
// Parameters: force (the fastest throw, units per second).
[[nodiscard]] Effect debris();

// A burst of stone chips (the "chip" mesh) thrown up from the floor that
// land, pile on the floor and each other, and come to rest (every collider
// is something a viewer can see):
// rigid physics, so it needs ajfx::rigid to run. Parameters: force (the
// fastest throw, units per second).
[[nodiscard]] Effect rubble();

// The textures the presets name, generated (no image files): "puff", a 2x2
// flipbook of soft, lumpy puffs that spread out cell by cell.
[[nodiscard]] TextureSet textures();

// The meshes the presets name: "chip", a box half as tall as it is wide.
[[nodiscard]] MeshSet meshes();

} // namespace ajfx::presets
