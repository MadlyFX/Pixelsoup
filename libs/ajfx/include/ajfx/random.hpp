#pragma once

#include <cstdint>

// Deterministic per-particle randomness.
//
// Every random value a particle ever uses is a pure function of three
// integers: the emitter's seed, the particle's spawn index, and a stream that
// names what the value is for (initial speed, lifetime, colour...). Nothing
// depends on which GPU thread ran the particle, the order threads finished, a
// clock, or how many frames have been simulated - which is what lets two
// machines agree, and lets a seek land on the same particles as playing through.
//
// Integer-only for the same reason: integer arithmetic has no contraction or
// reassociation freedom, so the CPU reference and every GPU driver produce the
// same bits. shaders/ajfx_random.hlsli is the HLSL twin of this file and must
// stay identical in arithmetic; the tests compare the two.
namespace ajfx {

[[nodiscard]] constexpr std::uint32_t hash_mix(std::uint32_t state, std::uint32_t value) noexcept {
    state ^= value + 0x9e3779b9U + (state << 6U) + (state >> 2U);
    state ^= state >> 16U;
    state *= 0x7feb352dU;
    state ^= state >> 15U;
    state *= 0x846ca68bU;
    state ^= state >> 16U;
    return state;
}

// A value in [0, 1). Twenty-four bits is exactly a binary32 significand, so
// the conversion is exact everywhere.
[[nodiscard]] constexpr float hash_unit(std::uint32_t state) noexcept {
    return static_cast<float>(state & 0x00ffffffU) * (1.0F / 16777216.0F);
}

[[nodiscard]] constexpr std::uint32_t particle_hash(
    std::uint32_t emitter_seed, std::uint32_t spawn_index, std::uint32_t stream) noexcept {
    return hash_mix(hash_mix(hash_mix(0U, emitter_seed), spawn_index), stream);
}

[[nodiscard]] constexpr float particle_unit(
    std::uint32_t emitter_seed, std::uint32_t spawn_index, std::uint32_t stream) noexcept {
    return hash_unit(particle_hash(emitter_seed, spawn_index, stream));
}

} // namespace ajfx
