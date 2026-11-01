#pragma once

#include <ajfx/math.hpp>

#include <cstdint>

namespace ajfx {

// The divergence-free field CurlNoise pushes particles along, at `q` in the
// noise's own space ((position - drift * time) * frequency, scrolled along z
// by time * evolution), for a module seed. The same numbers the simulation
// uses, for an editor that draws the field.
[[nodiscard]] Vec3 curl_noise(Vec3 q, std::uint32_t seed);

} // namespace ajfx
