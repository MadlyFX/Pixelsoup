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

ByteAddressBuffer particles : register(t0, space0);

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
    // x: seconds of motion a velocity-aligned quad spans.
    float4 sprite_extra;
};

static const uint absent = 0xffffffffU;
static const float2 corners[6] = {
    float2(-1.0F, -1.0F), float2(1.0F, -1.0F), float2(1.0F, 1.0F),
    float2(-1.0F, -1.0F), float2(1.0F, 1.0F), float2(-1.0F, 1.0F),
};

struct Output {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    // Premultiplied.
    float4 color : COLOR0;
};

float3 to_world_point(float3 p) {
    return world0.xyz * p.x + world1.xyz * p.y + world2.xyz * p.z + world3.xyz;
}

float3 to_world_direction(float3 d) {
    return world0.xyz * d.x + world1.xyz * d.y + world2.xyz * d.z;
}

Output main(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    const uint base = instance * stride;
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
    output.color = premultiplied != 0U ? color : float4(color.rgb * color.a, color.a);
    return output;
}
