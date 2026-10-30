// Sprites: one camera-facing (or velocity-aligned) quad per live particle,
// drawn with an indirect instance count the simulation writes. Vertices are
// pulled from the particle records; the record layout arrives as offsets,
// with 0xffffffff for an attribute the emitter does not store.

cbuffer AjfxSprite : register(b0, space1) {
    uint sprite_stride;
    uint sprite_position;
    uint sprite_velocity;
    uint sprite_color;
    uint sprite_size;
    uint sprite_rotation;
    uint sprite_facing;        // 0 camera, 1 velocity
    uint sprite_premultiplied; // the particle colour is already premultiplied
    float sprite_stretch;      // seconds of motion a velocity-aligned sprite spans
    uint sprite_sorted;        // 1: instance i draws the record sprite_order[i]
    uint2 sprite_reserved0;
    uint4 sprite_reserved1;
};

RWByteAddressBuffer sprite_particles : register(u0);
// view_projection as four rows (clip = M * position), then the camera's
// right, up and forward axes, each padded to four floats.
RWByteAddressBuffer sprite_camera : register(u1);
// Drawing order from the depth sort, when sprite_sorted is set.
RWByteAddressBuffer sprite_order : register(u2);

static const uint sprite_absent = 0xffffffffu;
static const float2 sprite_corners[6] = {
    float2(-1.0f, -1.0f), float2(1.0f, -1.0f), float2(1.0f, 1.0f),
    float2(-1.0f, -1.0f), float2(1.0f, 1.0f), float2(-1.0f, 1.0f),
};

struct SpriteVertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

float4 sprite_camera_row(uint row) {
    return asfloat(sprite_camera.Load4(row * 16u));
}

SpriteVertex sprite_vertex(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    uint record = sprite_sorted != 0u ? sprite_order.Load(instance * 4u) : instance;
    uint base = record * sprite_stride;
    float3 position = asfloat(sprite_particles.Load3(base + sprite_position));
    float4 color = sprite_color != sprite_absent ? asfloat(sprite_particles.Load4(base + sprite_color))
                                                 : float4(1.0f, 1.0f, 1.0f, 1.0f);
    float size = sprite_size != sprite_absent ? asfloat(sprite_particles.Load(base + sprite_size)) : 0.1f;

    float3 right = sprite_camera_row(4u).xyz;
    float3 up = sprite_camera_row(5u).xyz;
    float3 forward = sprite_camera_row(6u).xyz;
    bool aligned = false;
    float2 extent = float2(size * 0.5f, size * 0.5f);
    if (sprite_facing == 1u && sprite_velocity != sprite_absent) {
        float3 velocity = asfloat(sprite_particles.Load3(base + sprite_velocity));
        float3 along = velocity - forward * dot(velocity, forward);
        float len = length(along);
        if (len > 1.0e-6f) {
            up = along / len;
            right = cross(up, forward);
            extent.y += len * sprite_stretch * 0.5f;
            aligned = true;
        }
    }
    if (!aligned && sprite_rotation != sprite_absent) {
        float angle = asfloat(sprite_particles.Load(base + sprite_rotation));
        float c = cos(angle);
        float s = sin(angle);
        float3 rotated = right * c + up * s;
        up = up * c - right * s;
        right = rotated;
    }

    float2 corner = sprite_corners[vertex];
    float4 world = float4(position + right * (corner.x * extent.x) + up * (corner.y * extent.y), 1.0f);
    SpriteVertex output;
    output.position = float4(dot(sprite_camera_row(0u), world), dot(sprite_camera_row(1u), world),
                             dot(sprite_camera_row(2u), world), dot(sprite_camera_row(3u), world));
    output.uv = corner;
    output.color = color;
    return output;
}

// A soft disc. Output is premultiplied for the backend's blend equations.
float4 sprite_pixel(SpriteVertex input) : SV_Target {
    float coverage = saturate(1.0f - dot(input.uv, input.uv));
    if (sprite_premultiplied != 0u) {
        return input.color * coverage;
    }
    float alpha = input.color.a * coverage;
    return float4(input.color.rgb * alpha, alpha);
}
