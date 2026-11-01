// ajfx particle sprites, fragment stage: the sprite's shade (see
// particle_sprite_shade.hlsli), faded where it nears the scene behind it when
// the emitter asks for soft edges. Blended by the pipeline (premultiplied
// over, or additive) and depth-tested against the scene without writing
// depth.

#include "particle_sprite_shade.hlsli"

// The scene's velocity product: z is its reversed device depth, written by
// the auxiliary pass on every node from canvas-wide matrices.
Texture2D<float4> scene_motion : register(t1, space2);
SamplerState scene_motion_sampler : register(s1, space2);

// View distance from reversed device depth, through the projection's depth
// rows: depth = (p22 z + p23) / (p32 z + p33) for view-space z, which looks
// down -Z.
float view_distance(float depth, float4 terms) {
    return -(terms.y - depth * terms.w) / (depth * terms.z - terms.x);
}

float4 main(SpriteInput input) : SV_Target {
    float4 shade = sprite_shade(input);
    if ((input.flags & 16U) != 0U) {
        const float scene = scene_motion.Load(int3(int2(input.position.xy), 0)).z;
        // Nothing drawn behind (cleared depth) leaves the sprite whole.
        if (scene > 0.0F) {
            const float gap = view_distance(scene, input.depth_terms)
                - view_distance(input.position.z, input.depth_terms);
            shade *= saturate(gap / input.soft_distance);
        }
    }
    return shade;
}
