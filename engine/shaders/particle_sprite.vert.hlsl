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

#include "scene_motion_vector.hlsli"

ByteAddressBuffer particles : register(t0, space0);
// Drawing order from the depth sort: instance i draws record order[i] when
// the sorted flag is set (otherwise the records are bound here too).
ByteAddressBuffer order : register(t1, space0);

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
    // light.
    float4 sprite_extra;
    uint age_offset;
    uint lifetime_offset;
    // Columns in the low 16 bits, rows in the high.
    uint flipbook;
    // 1 textured, 2 cross-fade flipbook cells, 4 sRGB texture, 8 sorted.
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
    // Premultiplied.
    float4 color : COLOR0;
};

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

Output main(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    const uint record = (flags & 8U) != 0U ? order.Load(instance * 4U) : instance;
    const uint base = record * stride;
    const float3 position = to_world_point(asfloat(particles.Load3(base + position_offset)));
    float4 color = color_offset != absent ? asfloat(particles.Load4(base + color_offset)) : float4(1.0F, 1.0F, 1.0F, 1.0F);
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
    output.flags = flags;
    output.depth_terms = depth_terms;
    output.soft_distance = motion.z;
    // Where the particle was a frame ago, by its own velocity: one value for
    // the whole sprite, from its centre, so it needs no interpolation and
    // every node computes the same bits.
    const float3 moved = velocity_offset != absent
        ? to_world_direction(asfloat(particles.Load3(base + velocity_offset))) * motion.x
        : float3(0.0F, 0.0F, 0.0F);
    output.motion = motion_vector(mul(canvas_view_projection, float4(position, 1.0F)),
                                  mul(previous_view_projection, float4(position - moved, 1.0F)), motion.y);
    output.color = premultiplied != 0U ? color : float4(color.rgb * color.a, color.a);
    if (sprite_extra.y > 0.5F) {
        // Light covers as much as it brightens: later stages divide colour by
        // alpha and composite over what lies below, so a faint spark barely
        // hides it and a white-hot one replaces it, keeping its hue.
        output.color.a = saturate(max(max(output.color.r, output.color.g), output.color.b));
    }
    return output;
}
