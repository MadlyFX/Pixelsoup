// ajfx grid volumes, fragment stage: the smoke a grid volume holds, marched
// along the view ray through the grid's box in a fixed number of steps, as
// ajfx's reference raymarch does (ajfx/lib/shaders/ajfx_volume_draw.hlsl):
// each step absorbs by the density times the volume's extinction and
// scatters its albedo of what it absorbed towards the camera, in the
// volume's colour, lit by the scene's first directional light through a
// fixed number of steps of its own back out of the box, through that light's
// shadow cascades, plus the hemisphere, all under the scene's exposure.
//
// Determinism: the step counts are fixed, nothing is jittered, and the ray
// comes from the world-space box the vertex stage built and the camera's
// own eye and forward axis, never from the slice's projection, so every node
// marches the same ray through the same cells. The march stops at the
// scene's depth, read from the velocity product as soft particles read it,
// so smoke behind a wall stays behind it; the pipeline itself tests no
// depth, since a box's back face would hide smoke in front of a surface.
//
// Only the box's back faces shade: a pixel whose surface point is the ray's
// way in is discarded, so each ray is marched once, and a camera inside the
// box still sees the smoke around it. Output is premultiplied for the
// pipeline's blend.

#include "light.hlsli"
#include "particle_sprite_shade.hlsli"

// The scene's velocity product: z is its reversed device depth.
Texture2D<float4> scene_motion : register(t1, space2);
SamplerState scene_motion_sampler : register(s1, space2);
// The grid's cells (ajfx::gpu::VolumeBuffers): from word `cells_at`, the
// densities, then the temperatures, then the velocities.
ByteAddressBuffer volume_cells : register(t7, space2);
// The scene's lights, lighting.x of them.
StructuredBuffer<Light> scene_lights : register(t8, space2);

cbuffer ParticleVolume : register(b0, space3) {
    // Effect to world, four columns, and world to effect, four columns.
    float4 world0;
    float4 world1;
    float4 world2;
    float4 world3;
    float4 inverse0;
    float4 inverse1;
    float4 inverse2;
    float4 inverse3;
    // The camera's eye and forward axis in world space. w of `eye`: 1 for a
    // perspective view (rays fan out from the eye), 0 for an orthographic
    // one (rays run along the forward axis).
    float4 eye;
    float4 camera_forward;
    // The projection's (2,2), (2,3), (3,2) and (3,3): how reversed device
    // depth maps back to view distance.
    float4 depth_terms;
    // The grid: its lowest corner in the effect's space, and a cell's size.
    float4 grid_origin;
    // x, y, z: cells along each axis. w: the first density's word.
    uint4 grid_cells;
    // x: extinction. y: albedo. z: steps along the ray. w: steps towards
    // the light from each of them.
    float4 march;
    float4 color;
    // The scene's light, as lit sprites take it (ParticleLighting).
    float4 sky_ambient;     // rgb sky, a: ambient strength
    float4 ground_exposure; // rgb ground, a: exposure
    // x: lights in scene_lights. y: shadow cascades. z: 1 to take shadows.
    uint4 lighting;
    column_major float4x4 shadow_view_projection[4];
    float4 shadow_texel_depth;
    // Shadow bias in texels.
    float4 shadow_bias;
};

struct Input {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
};

float3 to_effect_point(float3 p) {
    return inverse0.xyz * p.x + inverse1.xyz * p.y + inverse2.xyz * p.z + inverse3.xyz;
}

float3 to_effect_direction(float3 d) {
    return inverse0.xyz * d.x + inverse1.xyz * d.y + inverse2.xyz * d.z;
}

float3 to_world_point(float3 p) {
    return world0.xyz * p.x + world1.xyz * p.y + world2.xyz * p.z + world3.xyz;
}

// View distance from reversed device depth, through the projection's depth
// rows (see particle_sprite.frag.hlsl).
float view_distance(float depth, float4 terms) {
    return -(terms.y - depth * terms.w) / (depth * terms.z - terms.x);
}

float cell_density(uint x, uint y, uint z) {
    const uint at = (z * grid_cells.y + y) * grid_cells.x + x;
    return asfloat(volume_cells.Load((grid_cells.w + at) * 4u));
}

// The density at `p` (the effect's space): trilinear between cell centres
// and clamped to the edge, zero outside the box. The reference's taps and
// lerps in its order (ajfx_volume.hlsli), so both see the same smoke.
float density_at(float3 p) {
    const float3 u = (p - grid_origin.xyz) * (1.0f / grid_origin.w);
    const float3 n = float3(grid_cells.xyz);
    if (!(u.x >= 0.0f && u.y >= 0.0f && u.z >= 0.0f && u.x <= n.x && u.y <= n.y && u.z <= n.z)) {
        return 0.0f;
    }
    const float3 g = min(max(u - 0.5f, 0.0f), n - 1.0f);
    const uint ix = min((uint)floor(g.x), grid_cells.x - 2u);
    const uint iy = min((uint)floor(g.y), grid_cells.y - 2u);
    const uint iz = min((uint)floor(g.z), grid_cells.z - 2u);
    const float fx = g.x - (float)ix;
    const float fy = g.y - (float)iy;
    const float fz = g.z - (float)iz;
    const float c000 = cell_density(ix, iy, iz);
    const float c100 = cell_density(ix + 1u, iy, iz);
    const float c010 = cell_density(ix, iy + 1u, iz);
    const float c110 = cell_density(ix + 1u, iy + 1u, iz);
    const float c001 = cell_density(ix, iy, iz + 1u);
    const float c101 = cell_density(ix + 1u, iy, iz + 1u);
    const float c011 = cell_density(ix, iy + 1u, iz + 1u);
    const float c111 = cell_density(ix + 1u, iy + 1u, iz + 1u);
    const float c00 = c000 + (c100 - c000) * fx;
    const float c10 = c010 + (c110 - c010) * fx;
    const float c01 = c001 + (c101 - c001) * fx;
    const float c11 = c011 + (c111 - c011) * fx;
    const float c0 = c00 + (c10 - c00) * fy;
    const float c1 = c01 + (c11 - c01) * fy;
    return max(c0 + (c1 - c0) * fz, 0.0f);
}

// Where the ray from `o` along `d` (the effect's space) enters and leaves
// the grid's box; false when it misses. Entry is clamped to the ray's start.
bool box_span(float3 o, float3 d, out float enter, out float leave) {
    const float3 lo = grid_origin.xyz;
    const float3 hi = lo + float3(grid_cells.xyz) * grid_origin.w;
    enter = 0.0f;
    leave = 3.4e38f;
    [unroll] for (uint axis = 0u; axis < 3u; ++axis) {
        const float dir = d[axis];
        if (abs(dir) < 1.0e-12f) {
            if (o[axis] < lo[axis] || o[axis] > hi[axis]) {
                return false;
            }
            continue;
        }
        const float inv = 1.0f / dir;
        const float t0 = (lo[axis] - o[axis]) * inv;
        const float t1 = (hi[axis] - o[axis]) * inv;
        enter = max(enter, min(t0, t1));
        leave = min(leave, max(t0, t1));
    }
    return leave > enter;
}

// Whether the directional light reaches world point `p`: the nearest
// cascade it falls inside, else lit.
float shadow_at(float3 p) {
    if (lighting.z == 0u) {
        return 1.0f;
    }
    const uint cascades = min(lighting.y, 4u);
    [unroll] for (uint c = 0u; c < 4u; ++c) {
        if (c >= cascades) {
            break;
        }
        const float4 clip = mul(shadow_view_projection[c], float4(p, 1.0f));
        if (clip.w <= 0.0f) {
            continue;
        }
        const float3 at = float3((clip.xy / clip.w) * float2(0.5f, -0.5f) + 0.5f, clip.z / clip.w);
        if (all(at.xy > 0.0f) && all(at.xy < 1.0f) && at.z > 0.0f && at.z < 1.0f) {
            const float bias = shadow_bias.x * shadow_texel_depth[c];
            if (c == 0u) return sprite_cascade_visibility(sprite_cascade0, at, bias);
            if (c == 1u) return sprite_cascade_visibility(sprite_cascade1, at, bias);
            if (c == 2u) return sprite_cascade_visibility(sprite_cascade2, at, bias);
            return sprite_cascade_visibility(sprite_cascade3, at, bias);
        }
    }
    return 1.0f;
}

float4 main(Input input) : SV_Target {
    // The view ray in world space: from the eye through this point of the
    // box, or along the forward axis through it, from the camera's plane.
    const float3 forward = normalize(camera_forward.xyz);
    float3 origin_world;
    float3 direction_world;
    if (eye.w > 0.5f) {
        origin_world = eye.xyz;
        direction_world = input.world - eye.xyz;
        const float span = length(direction_world);
        if (!(span > 0.0f)) {
            discard;
        }
        direction_world /= span;
    } else {
        origin_world = input.world - forward * dot(input.world - eye.xyz, forward);
        direction_world = forward;
    }
    const float along = dot(direction_world, forward);
    if (!(along > 1.0e-6f)) {
        discard;
    }
    // Into the effect's space, where the grid is axis-aligned. The ray's
    // length unit stays the world's, so the step is the same step whatever
    // the placement's scale.
    const float3 o = to_effect_point(origin_world);
    const float3 d_scaled = to_effect_direction(direction_world);
    const float scale = length(d_scaled);
    if (!(scale > 0.0f)) {
        discard;
    }
    const float3 d = d_scaled / scale;
    float enter;
    float leave;
    if (!box_span(o, d, enter, leave)) {
        discard;
    }
    // This pixel is the ray's way in or its way out; only the way out
    // marches, so the ray is marched once.
    const float here = dot(to_effect_point(input.world) - o, d);
    if (abs(here - enter) < abs(here - leave)) {
        discard;
    }
    // Nothing before the near plane, nothing past the far plane, and nothing
    // behind the scene.
    const float near_distance = view_distance(1.0f, depth_terms);
    enter = max(enter, near_distance * scale / along);
    const float far_distance = view_distance(0.0f, depth_terms);
    if (isfinite(far_distance) && far_distance > 0.0f) {
        leave = min(leave, far_distance * scale / along);
    }
    const float scene = scene_motion.Load(int3(int2(input.position.xy), 0)).z;
    if (scene > 0.0f) {
        leave = min(leave, view_distance(scene, depth_terms) * scale / along);
    }
    if (!(leave > enter)) {
        discard;
    }

    // The light: the scene's first directional light through its shadows
    // and the hemisphere, under the exposure; without a scene, one white
    // light at the camera, as lit sprites take.
    const float exposure = ground_exposure.a;
    const bool no_scene = lighting.x == 0u && sky_ambient.a <= 0.0f;
    float3 to_light_world = -forward;
    float3 light_color = float3(1.0f, 1.0f, 1.0f);
    bool lit = no_scene;
    for (uint l = 0u; l < lighting.x; ++l) {
        const Light record = scene_lights[l];
        if (record.kind == LIGHT_DIRECTIONAL) {
            to_light_world = -normalize(record.vector);
            light_color = record.color_intensity.rgb * record.color_intensity.a * exposure;
            lit = true;
            break;
        }
    }
    if (!lit) {
        light_color = float3(0.0f, 0.0f, 0.0f);
    }
    const float3 ambient = (sky_ambient.rgb + ground_exposure.rgb) * 0.5f * sky_ambient.a * exposure;
    const float3 to_light_scaled = to_effect_direction(to_light_world);
    const float light_scale = length(to_light_scaled);
    const float3 to_light = light_scale > 0.0f ? to_light_scaled / light_scale : float3(0.0f, 1.0f, 0.0f);
    const bool shadowed = lit && !no_scene && lighting.z != 0u && lighting.y != 0u;

    const float extinction = march.x;
    const float albedo = march.y;
    const uint steps = max((uint)march.z, 1u);
    const uint light_steps = max((uint)march.w, 1u);
    // Steps are in the effect's space; the extinction is per world unit.
    const float ds = (leave - enter) / (float)steps;
    const float ds_world = ds / scale;
    const float dl_world_per = light_scale > 0.0f ? 1.0f / light_scale : 1.0f;
    float transmittance = 1.0f;
    float3 scattered = float3(0.0f, 0.0f, 0.0f);
    for (uint k = 0u; k < steps; ++k) {
        const float t = enter + ((float)k + 0.5f) * ds;
        const float3 p = o + d * t;
        const float density = density_at(p);
        if (density <= 0.0f) {
            continue;
        }
        const float through = exp(-density * extinction * ds_world);
        float reaching = 1.0f;
        if (lit) {
            float light_enter;
            float light_leave;
            if (box_span(p, to_light, light_enter, light_leave)) {
                const float dl = light_leave / (float)light_steps;
                float depth = 0.0f;
                for (uint m = 0u; m < light_steps; ++m) {
                    depth += density_at(p + to_light * (((float)m + 0.5f) * dl)) * extinction * dl * dl_world_per;
                }
                reaching = exp(-depth);
            }
            if (shadowed) {
                reaching *= shadow_at(to_world_point(p));
            }
        }
        const float3 light = light_color * reaching + ambient;
        scattered += transmittance * (1.0f - through) * albedo * color.rgb * light;
        transmittance *= through;
    }
    return float4(scattered, 1.0f - transmittance);
}
