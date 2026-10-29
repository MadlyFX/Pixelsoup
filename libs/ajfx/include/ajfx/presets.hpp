#pragma once

#include <ajfx/effect.hpp>

// Ready-made effects: a starting point to tweak, and examples of the builder.
// Each publishes its most useful knobs as parameters.
namespace ajfx::presets {

// Hot sparks thrown upwards, falling and bouncing off the ground plane.
// Parameters: rate (sparks per second), color.
[[nodiscard]] Effect sparks();

// Slow, swirling, fading smoke rising from a disc.
// Parameters: rate, wind (vec3 acceleration).
[[nodiscard]] Effect smoke();

// Repeating bursts of confetti in two colours, tumbling down.
// Parameters: color_a, color_b.
[[nodiscard]] Effect confetti();

} // namespace ajfx::presets
