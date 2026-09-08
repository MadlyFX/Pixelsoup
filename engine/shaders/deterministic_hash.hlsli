#ifndef MEDIASERVER_DETERMINISTIC_HASH_HLSLI
#define MEDIASERVER_DETERMINISTIC_HASH_HLSLI

// Integer-only avalanche steps, shared by every pass that needs a repeatable
// pseudo-random value per pixel.
//
// Integer-only because a float expression is at the mercy of whatever the
// driver decides to fuse or reassociate, and two nodes with different driver
// versions would then rotate their sample sets differently for the same wall
// pixel. Integers have no such freedom.
//
// The rule for callers is the same everywhere: seed from **absolute canvas
// coordinates and the exact show frame**, never from a node-local pixel index,
// a frame counter or a clock. A node rendering the right half of the wall must
// produce the same value for a given wall pixel as a node rendering the whole
// of it, or the two disagree along the seam.

uint deterministic_mix(uint state, uint value) {
    state ^= value + 0x9e3779b9u + (state << 6u) + (state >> 2u);
    state ^= state >> 16u;
    state *= 0x7feb352du;
    state ^= state >> 15u;
    state *= 0x846ca68bu;
    state ^= state >> 16u;
    return state;
}

// A value in [0, 1). Twenty-four bits is exactly what a binary32 significand
// holds, so the conversion is exact on every implementation.
float deterministic_unit(uint state) {
    return (float)(state & 0x00ffffffu) * (1.0 / 16777216.0);
}

#endif
