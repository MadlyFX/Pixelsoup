// ajfx particle sprites, vertex stage. One quad per live particle, instanced
// by the count the simulation wrote (an indirect draw), pulled from the
// emitter's particle records.
//
// The quad is built in world space from the camera's own axes, never from the
// slice's projection, so every node builds the same geometry and only the
// rasterising projection differs between slices.
//
// Register spaces follow the scene stage: storage at (t[n], space0), uniforms
// at (b[n], space1). Matrices are explicit columns, as in scene.vert.hlsl.

#include "light.hlsli"
#include "scene_motion_vector.hlsli"

ByteAddressBuffer particles : register(t0, space0);
// Drawing order from the depth sort: instance i draws record order[i] when
// the sorted flag is set (otherwise the records are bound here too).
ByteAddressBuffer order : register(t1, space0);
// The emitter's counters, the live count first, for ribbons.
ByteAddressBuffer counters : register(t2, space0);
// The emitter's mesh: eight floats a vertex (position, normal, uv), then the
// triangle list's indices.
ByteAddressBuffer mesh_vertices : register(t3, space0);
ByteAddressBuffer mesh_indices : register(t4, space0);
// The scene's lights, lighting.x of them, for lit emitters.
StructuredBuffer<Light> scene_lights : register(t5, space0);

cbuffer ParticleView : register(b0, space1) {
    // This slice's projection. Rasterisation only.
    column_major float4x4 view_projection;
    // Effect to world, four columns.
    float4 world0;
    float4 world1;
    float4 world2;
    float4 world3;
    // World-space camera axes. xyz only.
    float4 camera_right;
    float4 camera_up;
    float4 camera_forward;
    // The record layout: byte offsets, 0xffffffff for an attribute the
    // emitter does not store.
    uint stride;
    uint position_offset;
    uint velocity_offset;
    uint color_offset;
    uint size_offset;
    uint rotation_offset;
    // 1 to align quads with velocity.
    uint facing_velocity;
    // 1 when the emitter's colours are already premultiplied.
    uint premultiplied;
    // x: seconds of motion a velocity-aligned quad spans. y: 1 for additive
    // light. z: the bits of a mesh particle's orientation offset (asuint),
    // absent when it has none. w: the body kind's own size scale for this
    // mesh draw, 1 where the size is the record's alone.
    float4 sprite_extra;
    uint age_offset;
    uint lifetime_offset;
    // Columns in the low 16 bits, rows in the high.
    uint flipbook;
    // 1 textured, 2 cross-fade flipbook cells, 4 sRGB texture, 8 sorted,
    // 16 soft, 32 one ribbon through the particles in birth order, 64 a mesh
    // each.
    uint flags;
    // The canvas-wide view-projection now and at the previous frame, without
    // jitter: the velocity product's two endpoints, as the scene's own.
    column_major float4x4 canvas_view_projection;
    column_major float4x4 previous_view_projection;
    // x: seconds back to the previous frame. y: 1 when there is a previous
    // frame to move from. z: the soft-particle fade distance.
    float4 motion;
    // The projection's (2,2), (2,3), (3,2) and (3,3): how reversed device
    // depth maps back to view distance.
    float4 depth_terms;
    float4 draw_tint;
    // The scene's light, for lit emitters. rgb: the sky half of the ambient
    // hemisphere; a: overall ambient strength.
    float4 sky_ambient;
    // rgb: the ground half; a: the exposure the scene applies to what it
    // lights.
    float4 ground_exposure;
    // The emitter's look: curvature, wrap, ambient, shadow bias in texels.
    float4 look;
    // x: lights in scene_lights. y: shadow cascades. z: the emitter's
    // lighting (0 unlit, 1 lit, 2 six-way). w: 1 when it takes shadows.
    uint4 lighting;
    column_major float4x4 shadow_view_projection[4];
    float4 shadow_texel_depth;
};

static const uint absent = 0xffffffffU;
static const float2 corners[6] = {
    float2(-1.0F, -1.0F), float2(1.0F, -1.0F), float2(1.0F, 1.0F),
    float2(-1.0F, -1.0F), float2(1.0F, 1.0F), float2(-1.0F, 1.0F),
};

struct Output {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    // This flipbook cell's texture coordinates, then the next's.
    float4 cells : TEXCOORD1;
    float blend : TEXCOORD2;
    nointerpolation uint flags : TEXCOORD3;
    // The sprite's velocity product, the same for all its vertices.
    nointerpolation float4 motion : TEXCOORD4;
    nointerpolation float4 depth_terms : TEXCOORD5;
    nointerpolation float soft_distance : TEXCOORD6;
    // Light arriving from the particle's right, left, up, down, front
    // (towards the camera) and behind, the directional lights apart so a
    // pixel can shadow them: `radiance` is the rest, `sun` the directional.
    float3 radiance[6] : TEXCOORD7;
    float3 sun[6] : TEXCOORD13;
    // A mesh vertex's normal in the sprite frame; a sprite rounds its own.
    float3 normal : TEXCOORD19;
    // Each cascade's shadow coordinates for this vertex (uv, reversed depth):
    // the cascades are orthographic, so they interpolate exactly.
    float3 shadow[4] : TEXCOORD20;
    // Each cascade's bias in its own depth.
    nointerpolation float4 shadow_bias : TEXCOORD24;
    nointerpolation float curvature : TEXCOORD25;
    // Premultiplied.
    float4 color : COLOR0;
};

// What the scene's light gives each of six directions of the frame with axes
// `right`, `up` and `front` (towards the camera) at `world`: the hemisphere
// by the look's ambient, every local light by its falloff and cone, and the
// directional lights apart in `sun`, each wrapped past the terminator by the
// look's wrap and all under the scene's exposure. Without a scene (no
// lights, no hemisphere) one white light at the camera, so a lit particle
// still reads.
void six_way_light(float3 world, float3 right, float3 up, float3 front, out float3 radiance[6], out float3 sun[6]) {
    const float wrap = look.y;
    const float exposure = ground_exposure.a;
    const float3 directions[6] = {right, -right, up, -up, front, -front};
    [unroll] for (uint d = 0U; d < 6U; ++d) {
        radiance[d] = lerp(ground_exposure.rgb, sky_ambient.rgb, directions[d].y * 0.5F + 0.5F) * sky_ambient.a
            * look.z * exposure;
        sun[d] = float3(0.0F, 0.0F, 0.0F);
    }
    const bool no_scene = lighting.x == 0U && sky_ambient.a <= 0.0F;
    const uint count = no_scene ? 1U : lighting.x;
    for (uint l = 0U; l < count; ++l) {
        float3 to_light;
        float3 light = float3(1.0F, 1.0F, 1.0F);
        bool directional = true;
        if (no_scene) {
            to_light = front;
        } else {
            const Light record = scene_lights[l];
            light = record.color_intensity.rgb * record.color_intensity.a * exposure;
            if (record.kind == LIGHT_DIRECTIONAL) {
                to_light = -normalize(record.vector);
            } else {
                directional = false;
                const float3 offset = record.vector - world;
                const float distance = length(offset);
                const float range = max(record.params.x, 1e-3F);
                if (distance >= range) {
                    continue;
                }
                to_light = offset / max(distance, 1e-5F);
                const float falloff = saturate(1.0F - distance / range);
                light *= falloff * falloff / max(distance * distance, 1e-4F);
                if (record.kind == LIGHT_SPOT) {
                    const float cone = light_cone_weight(record, dot(-to_light, record.spot.xyz));
                    if (cone <= 0.0F) {
                        continue;
                    }
                    light *= cone;
                }
            }
        }
        [unroll] for (uint d = 0U; d < 6U; ++d) {
            const float3 given = light * saturate((dot(to_light, directions[d]) + wrap) / (1.0F + wrap));
            if (directional) {
                sun[d] += given;
            } else {
                radiance[d] += given;
            }
        }
    }
}

// Where `world` falls in each shadow cascade, and each one's bias.
void shadow_coordinates(float3 world, out float3 shadow[4], out float4 shadow_bias) {
    shadow_bias = look.w * shadow_texel_depth;
    [unroll] for (uint c = 0U; c < 4U; ++c) {
        shadow[c] = float3(0.0F, 0.0F, 0.0F);
        if (c < lighting.y) {
            const float4 clip = mul(shadow_view_projection[c], float4(world, 1.0F));
            shadow[c] = clip.w > 0.0F ? float3((clip.xy / clip.w) * float2(0.5F, -0.5F) + 0.5F, clip.z / clip.w)
                                      : float3(0.0F, 0.0F, 0.0F);
        }
    }
}

// The flipbook cells a particle shows: its life split evenly over the cells,
// left to right, top to bottom. Age is simulation state, so every view and
// node picks the same cell.
void flipbook_cells(uint base, float2 corner, out float4 cells, out float blend) {
    const uint columns = max(flipbook & 0xffffU, 1U);
    const uint rows = max(flipbook >> 16U, 1U);
    const uint frames = columns * rows;
    const float age = age_offset != absent ? asfloat(particles.Load(base + age_offset)) : 0.0F;
    const float lifetime = lifetime_offset != absent ? asfloat(particles.Load(base + lifetime_offset)) : 1.0F;
    const float life = lifetime > 0.0F ? saturate(age / lifetime) : 0.0F;
    const float at = life * (float)frames;
    const float whole = floor(at);
    const uint frame = min((uint)whole, frames - 1U);
    const uint next = min(frame + 1U, frames - 1U);
    const float2 local = float2(corner.x * 0.5F + 0.5F, 0.5F - corner.y * 0.5F);
    const float2 scale = float2(1.0F / (float)columns, 1.0F / (float)rows);
    cells.xy = (float2((float)(frame % columns), (float)(frame / columns)) + local) * scale;
    cells.zw = (float2((float)(next % columns), (float)(next / columns)) + local) * scale;
    blend = (flags & 2U) != 0U ? saturate(at - whole) : 0.0F;
}

float3 to_world_point(float3 p) {
    return world0.xyz * p.x + world1.xyz * p.y + world2.xyz * p.z + world3.xyz;
}

float3 to_world_direction(float3 d) {
    return world0.xyz * d.x + world1.xyz * d.y + world2.xyz * d.z;
}

// What every geometry's vertex carries for its particle (the record at
// `base`, at world `position`): flags, depth terms, the motion product, and
// the premultiplied colour, its light scaled by `shade`. A lit emitter's
// vertex (at world `vertex_world`, in the sprite frame `right`, `up`,
// `front`) also carries the scene's light, which the pixel stage weighs.
void finish_vertex(inout Output output, uint base, float3 position, float shade, float3 vertex_world, float3 right,
                   float3 up, float3 front, float3 local_normal) {
    output.flags = flags | (lighting.z != 0U ? 256U : 0U) | (lighting.z == 2U ? 512U : 0U)
        | (lighting.w != 0U ? 1024U : 0U) | (min(lighting.y, 4U) << 11U);
    output.normal = local_normal;
    output.curvature = look.x;
    if (lighting.z != 0U) {
        six_way_light(vertex_world, right, up, front, output.radiance, output.sun);
        shadow_coordinates(vertex_world, output.shadow, output.shadow_bias);
    } else {
        [unroll] for (uint d = 0U; d < 6U; ++d) {
            output.radiance[d] = float3(0.0F, 0.0F, 0.0F);
            output.sun[d] = float3(0.0F, 0.0F, 0.0F);
        }
        [unroll] for (uint c = 0U; c < 4U; ++c) {
            output.shadow[c] = float3(0.0F, 0.0F, 0.0F);
        }
        output.shadow_bias = float4(0.0F, 0.0F, 0.0F, 0.0F);
    }
    output.depth_terms = depth_terms;
    output.soft_distance = motion.z;
    // Where the particle was a frame ago, by its own velocity: one value per
    // particle, computed alike on every node from canvas-wide matrices, and
    // flat across what it draws.
    const float3 moved = velocity_offset != absent
        ? to_world_direction(asfloat(particles.Load3(base + velocity_offset))) * motion.x
        : float3(0.0F, 0.0F, 0.0F);
    output.motion = motion_vector(mul(canvas_view_projection, float4(position, 1.0F)),
                                  mul(previous_view_projection, float4(position - moved, 1.0F)), motion.y);
    float4 color = color_offset != absent ? asfloat(particles.Load4(base + color_offset))
                                          : float4(1.0F, 1.0F, 1.0F, 1.0F);
    color *= draw_tint;
    if (premultiplied != 0U) { color.rgb *= draw_tint.a; }
    color.rgb *= shade;
    output.color = premultiplied != 0U ? color : float4(color.rgb * color.a, color.a);
    if (sprite_extra.y > 0.5F) {
        // Light covers as much as it brightens: later stages divide colour by
        // alpha and composite over what lies below, so a faint spark barely
        // hides it and a white-hot one replaces it, keeping its hue.
        output.color.a = saturate(max(max(output.color.r, output.color.g), output.color.b));
    }
}

float3 particle_position(uint record) {
    return to_world_point(asfloat(particles.Load3(record * stride + position_offset)));
}

// Segment `instance` of the ribbon: corner.x picks its end (that particle or
// the next), corner.y its side. The side runs across the trail's direction
// at that end as the camera sees it; the last instance has no next particle
// and draws nothing.
Output ribbon_vertex(float2 corner, uint instance) {
    Output output = (Output)0;
    const uint alive = counters.Load(0U);
    if (instance + 1U >= alive) {
        return output;
    }
    const uint end = instance + (corner.x > 0.0F ? 1U : 0U);
    const uint base = end * stride;
    const float3 position = particle_position(end);
    const float3 tangent = particle_position(min(end + 1U, alive - 1U)) - particle_position(end > 0U ? end - 1U : 0U);
    float3 side = cross(tangent, camera_forward.xyz);
    const float span = length(side);
    side = span > 1.0e-6F ? side / span : camera_right.xyz;
    const float size = size_offset != absent ? asfloat(particles.Load(base + size_offset)) : 0.1F;
    output.position = mul(view_projection, float4(position + side * (corner.y * size * 0.5F), 1.0F));
    // Soft across, like the disc; the texture along it by life.
    output.uv = float2(0.0F, corner.y);
    const float age = age_offset != absent ? asfloat(particles.Load(base + age_offset)) : 0.0F;
    const float lifetime = lifetime_offset != absent ? asfloat(particles.Load(base + lifetime_offset)) : 1.0F;
    const float life = lifetime > 0.0F ? saturate(age / lifetime) : 0.0F;
    output.cells = float2(life, 0.5F - corner.y * 0.5F).xyxy;
    output.blend = 0.0F;
    finish_vertex(output, base, position, 1.0F, position, camera_right.xyz, camera_up.xyz, -camera_forward.xyz,
                  float3(0.0F, 0.0F, 1.0F));
    return output;
}

// Vertex `vertex` of the mesh drawn for particle `instance`: turned by the
// particle's orientation, or else so its +Z runs along its velocity (velocity
// facing) or spun about Y by its rotation, in the effect's space; scaled by
// its size; lit by a light at the camera.
Output mesh_vertex(uint vertex, uint instance) {
    Output output = (Output)0;
    const uint record = (flags & 8U) != 0U ? order.Load(instance * 4U) : instance;
    const uint base = record * stride;
    const float3 position = to_world_point(asfloat(particles.Load3(base + position_offset)));
    // The body kind's own size as well, so the model drawn is the body the
    // solver built rather than the one the emitter's size describes.
    const float size = (size_offset != absent ? asfloat(particles.Load(base + size_offset)) : 0.1F) * sprite_extra.w;
    const uint index = mesh_indices.Load(vertex * 4U);
    const float3 local = asfloat(mesh_vertices.Load3(index * 32U));
    const float3 normal = asfloat(mesh_vertices.Load3(index * 32U + 12U));
    const float2 uv = asfloat(mesh_vertices.Load2(index * 32U + 24U));
    float3 ax = float3(1.0F, 0.0F, 0.0F);
    float3 ay = float3(0.0F, 1.0F, 0.0F);
    float3 az = float3(0.0F, 0.0F, 1.0F);
    bool aligned = false;
    const uint orientation_offset = asuint(sprite_extra.z);
    if (orientation_offset != absent) {
        // The quaternion's rotation matrix, column by column.
        const float4 q = asfloat(particles.Load4(base + orientation_offset));
        ax = float3(1.0F - 2.0F * (q.y * q.y + q.z * q.z), 2.0F * (q.x * q.y + q.w * q.z),
                    2.0F * (q.x * q.z - q.w * q.y));
        ay = float3(2.0F * (q.x * q.y - q.w * q.z), 1.0F - 2.0F * (q.x * q.x + q.z * q.z),
                    2.0F * (q.y * q.z + q.w * q.x));
        az = float3(2.0F * (q.x * q.z + q.w * q.y), 2.0F * (q.y * q.z - q.w * q.x),
                    1.0F - 2.0F * (q.x * q.x + q.y * q.y));
        aligned = true;
    } else if (facing_velocity != 0U && velocity_offset != absent) {
        const float3 velocity = asfloat(particles.Load3(base + velocity_offset));
        const float speed = length(velocity);
        if (speed > 1.0e-6F) {
            az = velocity / speed;
            const float3 reference = abs(az.y) < 0.99F ? float3(0.0F, 1.0F, 0.0F) : float3(1.0F, 0.0F, 0.0F);
            ax = normalize(cross(reference, az));
            ay = cross(az, ax);
            aligned = true;
        }
    }
    if (!aligned && rotation_offset != absent) {
        const float angle = asfloat(particles.Load(base + rotation_offset));
        const float c = cos(angle);
        const float s = sin(angle);
        ax = float3(c, 0.0F, -s);
        az = float3(s, 0.0F, c);
    }
    const float3 offset = to_world_direction((ax * local.x + ay * local.y + az * local.z) * size);
    const float3 n = normalize(to_world_direction(ax * normal.x + ay * normal.y + az * normal.z));
    output.position = mul(view_projection, float4(position + offset, 1.0F));
    // Solid (no disc), textured by the mesh's own coordinates.
    output.cells = uv.xyxy;
    // Lit by the scene through its own normal, taken into the camera's
    // frame; unlit, by a light at the camera so its shape still reads.
    const float3 right = camera_right.xyz;
    const float3 up = camera_up.xyz;
    const float3 front = -camera_forward.xyz;
    const float shade = lighting.z != 0U ? 1.0F : 0.35F + 0.65F * saturate(dot(n, front));
    finish_vertex(output, base, position, shade, position + offset, right, up, front,
                  float3(dot(n, right), dot(n, up), dot(n, front)));
    return output;
}

Output main(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    if ((flags & 32U) != 0U) {
        return ribbon_vertex(corners[vertex], instance);
    }
    if ((flags & 64U) != 0U) {
        return mesh_vertex(vertex, instance);
    }
    const uint record = (flags & 8U) != 0U ? order.Load(instance * 4U) : instance;
    const uint base = record * stride;
    const float3 position = to_world_point(asfloat(particles.Load3(base + position_offset)));
    const float size = size_offset != absent ? asfloat(particles.Load(base + size_offset)) : 0.1F;

    float3 right = camera_right.xyz;
    float3 up = camera_up.xyz;
    const float3 forward = camera_forward.xyz;
    float2 extent = float2(size * 0.5F, size * 0.5F);
    bool aligned = false;
    if (facing_velocity != 0U && velocity_offset != absent) {
        const float3 velocity = to_world_direction(asfloat(particles.Load3(base + velocity_offset)));
        const float3 along = velocity - forward * dot(velocity, forward);
        const float speed = length(along);
        if (speed > 1.0e-6F) {
            up = along / speed;
            right = cross(up, forward);
            extent.y += speed * sprite_extra.x * 0.5F;
            aligned = true;
        }
    }
    if (!aligned && rotation_offset != absent) {
        const float angle = asfloat(particles.Load(base + rotation_offset));
        const float c = cos(angle);
        const float s = sin(angle);
        const float3 rotated = right * c + up * s;
        up = up * c - right * s;
        right = rotated;
    }

    const float2 corner = corners[vertex];
    const float3 world = position + right * (corner.x * extent.x) + up * (corner.y * extent.y);
    Output output;
    output.position = mul(view_projection, float4(world, 1.0F));
    output.uv = corner;
    flipbook_cells(base, corner, output.cells, output.blend);
    // The frame the quad was built in, turned with it, so the rounded normal
    // and the six-way maps follow the picture.
    finish_vertex(output, base, position, 1.0F, world, normalize(right), normalize(up), -forward,
                  float3(0.0F, 0.0F, 1.0F));
    return output;
}
