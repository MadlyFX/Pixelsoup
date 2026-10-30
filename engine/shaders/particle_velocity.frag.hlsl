// ajfx particle sprites, motion stage: writes the sprite's motion product
// (canvas-relative UV motion, current and expected previous reversed depth)
// into the velocity target wherever the sprite covers at least half the
// pixel, so temporal antialiasing reprojects the particle rather than the
// background behind it. Fainter fringes keep the background's motion.
//
// The product is one flat value per sprite, computed in the vertex stage from
// the particle's centre with the canvas-wide matrices: no interpolation, so
// every node writes the same bits for the same wall pixel.

#include "particle_sprite_shade.hlsli"

float4 main(SpriteInput input) : SV_Target {
    if (sprite_shade(input).a < 0.5F) {
        discard;
    }
    return input.motion;
}
