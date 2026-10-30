// Sprites: one camera-facing (or velocity-aligned) quad per live particle,
// drawn with an indirect instance count the simulation writes. Vertices are
// pulled from the particle records; the record layout arrives as offsets,
// with 0xffffffff for an attribute the emitter does not store. A textured
// sprite samples its texture, or the flipbook cell its life has reached;
// an untextured one is a soft disc.

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
    uint sprite_age;
    uint sprite_lifetime;
    uint sprite_flipbook;      // columns in the low 16 bits, rows in the high
    uint sprite_flags;         // 1: textured, 2: cross-fade flipbook cells
    uint2 sprite_reserved0;
};

RWByteAddressBuffer sprite_particles : register(u0);
// view_projection as four rows (clip = M * position), then the camera's
// right, up and forward axes, each padded to four floats.
RWByteAddressBuffer sprite_camera : register(u1);
// Drawing order from the depth sort, when sprite_sorted is set.
RWByteAddressBuffer sprite_order : register(u2);
Texture2D<float4> sprite_texture : register(t0);
SamplerState sprite_sampler : register(s0);

static const uint sprite_absent = 0xffffffffu;
static const float2 sprite_corners[6] = {
    float2(-1.0f, -1.0f), float2(1.0f, -1.0f), float2(1.0f, 1.0f),
    float2(-1.0f, -1.0f), float2(1.0f, 1.0f), float2(-1.0f, 1.0f),
};

struct SpriteVertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;      // the quad's corner, -1 to 1
    float4 cells : TEXCOORD1;   // this flipbook cell's texture coordinates, then the next's
    float blend : TEXCOORD2;    // how far into the next cell
    float4 color : COLOR0;
};

// The flipbook cells a particle shows: its life split evenly over the
// cells, left to right, top to bottom. Age is simulation state, so every
// view and node picks the same cell.
void sprite_flipbook_cells(uint base, float2 corner, out float4 cells, out float blend) {
    uint columns = max(sprite_flipbook & 0xffffu, 1u);
    uint rows = max(sprite_flipbook >> 16u, 1u);
    uint frames = columns * rows;
    float age = sprite_age != sprite_absent ? asfloat(sprite_particles.Load(base + sprite_age)) : 0.0f;
    float lifetime = sprite_lifetime != sprite_absent ? asfloat(sprite_particles.Load(base + sprite_lifetime)) : 1.0f;
    float life = lifetime > 0.0f ? saturate(age / lifetime) : 0.0f;
    float at = life * (float)frames;
    float whole = floor(at);
    uint frame = min((uint)whole, frames - 1u);
    uint next = min(frame + 1u, frames - 1u);
    float2 local = float2(corner.x * 0.5f + 0.5f, 0.5f - corner.y * 0.5f);
    float2 scale = float2(1.0f / (float)columns, 1.0f / (float)rows);
    cells.xy = (float2((float)(frame % columns), (float)(frame / columns)) + local) * scale;
    cells.zw = (float2((float)(next % columns), (float)(next / columns)) + local) * scale;
    blend = (sprite_flags & 2u) != 0u ? saturate(at - whole) : 0.0f;
}

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
    sprite_flipbook_cells(base, corner, output.cells, output.blend);
    output.color = color;
    return output;
}

// The texture times the particle colour, or a soft disc. Output is
// premultiplied for the backend's blend equations.
float4 sprite_pixel(SpriteVertex input) : SV_Target {
    if ((sprite_flags & 1u) != 0u) {
        float4 texel = lerp(sprite_texture.Sample(sprite_sampler, input.cells.xy),
                            sprite_texture.Sample(sprite_sampler, input.cells.zw), input.blend);
        if (sprite_premultiplied != 0u) {
            return input.color * float4(texel.rgb * texel.a, texel.a);
        }
        float textured_alpha = input.color.a * texel.a;
        return float4(input.color.rgb * texel.rgb * textured_alpha, textured_alpha);
    }
    float coverage = saturate(1.0f - dot(input.uv, input.uv));
    if (sprite_premultiplied != 0u) {
        return input.color * coverage;
    }
    float alpha = input.color.a * coverage;
    return float4(input.color.rgb * alpha, alpha);
}
