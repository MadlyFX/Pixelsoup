#ifndef MEDIASERVER_PARTICLE_SPRITE_SHADE_HLSLI
#define MEDIASERVER_PARTICLE_SPRITE_SHADE_HLSLI

// What a particle sprite puts at a pixel, shared by the colour pass and the
// motion pass so both agree on its coverage: the emitter's texture (or
// flipbook cell) times the particle's premultiplied colour, or a soft disc
// when the emitter has no texture; a lit emitter's colour times the light
// its pixel receives (see sprite_light).
//
// sRGB textures are decoded after filtering, as the scene's model textures
// are: this RHI samples eight-bit textures as stored.

Texture2D<float4> sprite_texture : register(t0, space2);
SamplerState sprite_sampler : register(s0, space2);
// The second six-way map (light from the left, from below and from behind),
// then the scene's directional shadow cascades, nearest first; the vertex
// stage says how many are bound.
Texture2D<float4> sprite_lightmap : register(t2, space2);
SamplerState sprite_lightmap_sampler : register(s2, space2);
Texture2D<float> sprite_cascade0 : register(t3, space2);
SamplerState sprite_cascade0_sampler : register(s3, space2);
Texture2D<float> sprite_cascade1 : register(t4, space2);
SamplerState sprite_cascade1_sampler : register(s4, space2);
Texture2D<float> sprite_cascade2 : register(t5, space2);
SamplerState sprite_cascade2_sampler : register(s5, space2);
Texture2D<float> sprite_cascade3 : register(t6, space2);
SamplerState sprite_cascade3_sampler : register(s6, space2);

float3 decode_srgb(float3 encoded) {
    const float3 low = encoded / 12.92;
    const float3 high = pow(max((encoded + 0.055) / 1.055, 0.0), 2.4);
    return lerp(low, high, step(0.04045, encoded));
}

struct SpriteInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 cells : TEXCOORD1;
    float blend : TEXCOORD2;
    nointerpolation uint flags : TEXCOORD3;
    // The sprite's motion product (see scene_motion_vector.hlsli), one value
    // for the whole sprite.
    nointerpolation float4 motion : TEXCOORD4;
    nointerpolation float4 depth_terms : TEXCOORD5;
    nointerpolation float soft_distance : TEXCOORD6;
    // The scene's light from six directions (see particle_sprite.vert.hlsl),
    // the directional lights apart in `sun`, and where this pixel falls in
    // each shadow cascade. Flags: 256 lit, 512 six-way, 1024 shadowed, and
    // bits 11 up the cascade count.
    float3 radiance[6] : TEXCOORD7;
    float3 sun[6] : TEXCOORD13;
    float3 normal : TEXCOORD19;
    float3 shadow[4] : TEXCOORD20;
    nointerpolation float4 shadow_bias : TEXCOORD24;
    // The look's curvature, for a sprite's rounded normal.
    nointerpolation float curvature : TEXCOORD25;
    float4 color : COLOR0;
};

// Whether the directional light reaches `coordinates` (uv, reversed depth)
// in one cascade: four taps compared and blended, missing taps lit. A sprite
// has no receiver plane, so a constant bias in the map's own depth.
float sprite_cascade_visibility(Texture2D<float> map, float3 coordinates, float bias) {
    uint w, h;
    map.GetDimensions(w, h);
    const float2 extent = float2(w, h);
    const float2 coord = coordinates.xy * extent - 0.5F;
    const int2 pixel = int2(floor(coord));
    const float2 fraction = frac(coord);
    const float receiver = min(coordinates.z + bias, 1.0F);
    float values[4];
    [unroll] for (uint i = 0U; i < 4U; ++i) {
        const int2 at = pixel + int2(i & 1U, i >> 1U);
        if (any(at < 0) || any(at >= int2(extent))) {
            values[i] = 1.0F;
            continue;
        }
        const float stored = map.Load(int3(at, 0));
        values[i] = stored == 0.0F || stored <= receiver ? 1.0F : 0.0F;
    }
    return lerp(lerp(values[0], values[1], fraction.x), lerp(values[2], values[3], fraction.x), fraction.y);
}

// The directional shadow at this pixel: the nearest cascade it falls inside,
// else lit. Every node picks the cascade from the same world point.
float sprite_shadow(SpriteInput input) {
    if ((input.flags & 1024U) == 0U) {
        return 1.0F;
    }
    const uint cascades = (input.flags >> 11U) & 7U;
    [unroll] for (uint c = 0U; c < 4U; ++c) {
        if (c >= cascades) {
            break;
        }
        const float3 at = input.shadow[c];
        if (all(at.xy > 0.0F) && all(at.xy < 1.0F) && at.z > 0.0F && at.z < 1.0F) {
            if (c == 0U) return sprite_cascade_visibility(sprite_cascade0, at, input.shadow_bias.x);
            if (c == 1U) return sprite_cascade_visibility(sprite_cascade1, at, input.shadow_bias.y);
            if (c == 2U) return sprite_cascade_visibility(sprite_cascade2, at, input.shadow_bias.z);
            return sprite_cascade_visibility(sprite_cascade3, at, input.shadow_bias.w);
        }
    }
    return 1.0F;
}

// The light a pixel of a lit particle receives: the six directions weighed
// by a normal (a sprite's, rounded from its corner by the look's curvature,
// a mesh's own) or by the six-way
// maps' texels, the directional part through the shadow. One when unlit.
float3 sprite_light(SpriteInput input) {
    if ((input.flags & 256U) == 0U) {
        return float3(1.0F, 1.0F, 1.0F);
    }
    float weights[6];
    if ((input.flags & 512U) != 0U) {
        const float3 a = lerp(sprite_texture.Sample(sprite_sampler, input.cells.xy),
                              sprite_texture.Sample(sprite_sampler, input.cells.zw), input.blend).rgb;
        const float3 b = lerp(sprite_lightmap.Sample(sprite_lightmap_sampler, input.cells.xy),
                              sprite_lightmap.Sample(sprite_lightmap_sampler, input.cells.zw), input.blend).rgb;
        weights[0] = a.r; weights[1] = b.r;
        weights[2] = a.g; weights[3] = b.g;
        weights[4] = a.b; weights[5] = b.b;
    } else {
        float3 n;
        if ((input.flags & 64U) != 0U) {
            n = normalize(input.normal);
        } else {
            const float2 turned = input.uv * input.curvature;
            n = normalize(float3(turned, sqrt(saturate(1.0F - dot(turned, turned)))));
        }
        weights[0] = max(n.x, 0.0F); weights[1] = max(-n.x, 0.0F);
        weights[2] = max(n.y, 0.0F); weights[3] = max(-n.y, 0.0F);
        weights[4] = max(n.z, 0.0F); weights[5] = max(-n.z, 0.0F);
    }
    const float shadow = sprite_shadow(input);
    float3 light = float3(0.0F, 0.0F, 0.0F);
    [unroll] for (uint d = 0U; d < 6U; ++d) {
        light += weights[d] * (input.radiance[d] + shadow * input.sun[d]);
    }
    return light;
}

// Premultiplied. Six-way maps are light, not colour: only their alpha
// covers. An additive emitter's light covers as much as it brightens, worked
// out again here from the lit colour (see finish_vertex).
float4 sprite_shade(SpriteInput input) {
    float4 color = input.color;
    if ((input.flags & 256U) != 0U) {
        color.rgb *= sprite_light(input);
        if ((input.flags & 128U) != 0U) {
            color.a = saturate(max(max(color.r, color.g), color.b));
        }
    }
    if ((input.flags & 1U) != 0U) {
        float4 texel = lerp(sprite_texture.Sample(sprite_sampler, input.cells.xy),
                            sprite_texture.Sample(sprite_sampler, input.cells.zw), input.blend);
        if ((input.flags & 4U) != 0U) {
            texel.rgb = decode_srgb(texel.rgb);
        }
        if ((input.flags & 512U) != 0U) {
            texel.rgb = float3(1.0F, 1.0F, 1.0F);
        }
        return color * float4(texel.rgb * texel.a, texel.a);
    }
    const float coverage = saturate(1.0F - dot(input.uv, input.uv));
    return color * coverage;
}

#endif
