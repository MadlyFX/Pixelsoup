// ajfx particle sprites, fragment stage: the sprite's shade (see
// particle_sprite_shade.hlsli). Blended by the pipeline (premultiplied over,
// or additive) and depth-tested against the scene without writing depth.

#include "particle_sprite_shade.hlsli"

float4 main(SpriteInput input) : SV_Target {
    return sprite_shade(input);
}
