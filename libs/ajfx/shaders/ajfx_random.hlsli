#ifndef AJFX_RANDOM_HLSLI
#define AJFX_RANDOM_HLSLI

// HLSL twin of include/ajfx/random.hpp. The arithmetic must match it exactly;
// see that file for the rules. Seed from (emitter seed, spawn index, stream),
// never a thread index, frame counter or clock.

uint ajfx_hash_mix(uint state, uint value) {
    state ^= value + 0x9e3779b9u + (state << 6u) + (state >> 2u);
    state ^= state >> 16u;
    state *= 0x7feb352du;
    state ^= state >> 15u;
    state *= 0x846ca68bu;
    state ^= state >> 16u;
    return state;
}

float ajfx_hash_unit(uint state) {
    return (float)(state & 0x00ffffffu) * (1.0 / 16777216.0);
}

uint ajfx_particle_hash(uint emitter_seed, uint spawn_index, uint stream) {
    return ajfx_hash_mix(ajfx_hash_mix(ajfx_hash_mix(0u, emitter_seed), spawn_index), stream);
}

float ajfx_particle_unit(uint emitter_seed, uint spawn_index, uint stream) {
    return ajfx_hash_unit(ajfx_particle_hash(emitter_seed, spawn_index, stream));
}

#endif
