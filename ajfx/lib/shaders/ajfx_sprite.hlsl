// Sprites: one camera-facing (or velocity-aligned) quad per live particle,
// drawn with an indirect instance count the simulation writes. Vertices are
// pulled from the particle records; the record layout arrives as offsets,
// with 0xffffffff for an attribute the emitter does not store. A textured
// sprite samples its texture, or the flipbook cell its life has reached;
// an untextured one is a soft disc. A ribbon emitter draws instead one strip
// through its live particles in birth order: instance i is the segment from
// particle i to particle i + 1.
//
// Lighting (SpriteRenderer::lighting): light reaches a particle through six
// directions of its own frame (right, left, up, down, towards the camera,
// away from it). The vertex stage sums what the scene's light gives each
// direction, the pixel stage weighs them by a normal (lit: rounded from the
// quad, or the mesh's own) or by six-way lightmaps. The reference scene is
// one directional light and a sky/ground hemisphere, in sprite_camera.

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
    uint sprite_sorted;        // 0: instance i draws record i; 1: the record sprite_order[i];
                               // 2 + w: a depth slice, the record sprite_order[sprite_order[w] + i]
    uint sprite_age;
    uint sprite_lifetime;
    uint sprite_flipbook;      // columns in the low 16 bits, rows in the high
    uint sprite_flags;         // 1: textured, 2: cross-fade flipbook cells; bits 4-5 lighting
                               // (0 unlit, 1 lit, 2 six-way); bits 8 up the emitter's index
    uint sprite_geometry;      // 0 sprites, 1 one ribbon through the particles, 2 a mesh each
    uint sprite_orientation;   // a mesh particle's quaternion, when it has one
};

RWByteAddressBuffer sprite_particles : register(u0);
// view_projection as four rows (clip = M * position), then the camera's
// right, up and forward axes, each padded to four floats; then the scene's
// light: the direction it travels, its colour, the sky colour and the
// ground colour, each padded to four floats.
RWByteAddressBuffer sprite_camera : register(u1);
// Drawing order from the depth sort, when sprite_sorted is set; then where
// each depth slice starts in it (ParticleSorter::depth_slice).
RWByteAddressBuffer sprite_order : register(u2);
// The live count first, for ribbons.
RWByteAddressBuffer sprite_counters : register(u3);
// The emitter's models, and which of them each body kind is drawn as:
//   0: the byte offset of a record's body kind, or sprite_absent for an
//      emitter whose bodies are all drawn alike;
//   1: how many kinds the table holds, at least one;
//   then seven words a kind: index start/count, size scale, and RGBA tint.
//   Several kinds may share
//   a model, and then share its range while keeping their own scale and tint.
// A particle's kind picks its model, so one draw covers every model the
// emitter has and the order between them is still the sort's.
RWByteAddressBuffer sprite_models : register(u6);
// The emitter's models: eight floats a vertex (position, normal, uv), then the
// triangle list's indices.
RWByteAddressBuffer sprite_mesh_vertices : register(u4);
RWByteAddressBuffer sprite_mesh_indices : register(u5);
// Each emitter's look, four floats: curvature, wrap, ambient, unused.
RWByteAddressBuffer sprite_looks : register(u7);
Texture2D<float4> sprite_texture : register(t0);
// The second six-way map: light from the left, from below and from behind.
Texture2D<float4> sprite_lightmap : register(t1);
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
    // Light arriving from the particle's right, left, up, down, front
    // (towards the camera) and behind; zero when unlit.
    float3 radiance[6] : TEXCOORD3;
    // A mesh vertex's normal in the sprite frame; a sprite rounds its own.
    float3 normal : TEXCOORD9;
    float4 color : COLOR0;
};

float4 sprite_camera_row(uint row) {
    return asfloat(sprite_camera.Load4(row * 16u));
}

uint sprite_lighting() {
    return (sprite_flags >> 4u) & 3u;
}

float3 sprite_look() {
    return asfloat(sprite_looks.Load3((sprite_flags >> 8u) * 16u));
}

// What the scene's light gives each of the six directions of a frame with
// axes `right`, `up` and `front` (towards the camera): the directional light
// wrapped past the terminator by `wrap`, and the hemisphere by `ambient`.
void sprite_radiance(float3 right, float3 up, float3 front, out float3 radiance[6]) {
    const float3 look = sprite_look();
    const float wrap = look.y;
    const float ambient = look.z;
    const float3 to_light = -normalize(sprite_camera_row(7u).xyz);
    const float3 color = sprite_camera_row(8u).xyz;
    const float3 sky = sprite_camera_row(9u).xyz;
    const float3 ground = sprite_camera_row(10u).xyz;
    const float3 directions[6] = {right, -right, up, -up, front, -front};
    [unroll] for (uint i = 0u; i < 6u; ++i) {
        const float facing = dot(to_light, directions[i]);
        radiance[i] = color * saturate((facing + wrap) / (1.0f + wrap)) +
                      lerp(ground, sky, directions[i].y * 0.5f + 0.5f) * ambient;
    }
}

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

float3 sprite_particle_position(uint record) {
    return asfloat(sprite_particles.Load3(record * sprite_stride + sprite_position));
}

// Segment `instance` of the ribbon: corner.x picks its end (that particle or
// the next), corner.y its side. The side runs across the trail's direction
// at that end, as the camera sees it; the last instance has no next particle
// and draws nothing.
SpriteVertex sprite_ribbon_vertex(uint vertex, uint instance) {
    SpriteVertex output = (SpriteVertex)0;
    uint alive = sprite_counters.Load(0u);
    float2 corner = sprite_corners[vertex];
    if (instance + 1u >= alive) {
        return output;
    }
    uint end = instance + (corner.x > 0.0f ? 1u : 0u);
    uint base = end * sprite_stride;
    float3 position = sprite_particle_position(end);
    float3 tangent = sprite_particle_position(min(end + 1u, alive - 1u)) -
                     sprite_particle_position(end > 0u ? end - 1u : 0u);
    float3 side = cross(tangent, sprite_camera_row(6u).xyz);
    float len = length(side);
    side = len > 1.0e-6f ? side / len : sprite_camera_row(4u).xyz;
    float size = sprite_size != sprite_absent ? asfloat(sprite_particles.Load(base + sprite_size)) : 0.1f;
    float4 world = float4(position + side * (corner.y * size * 0.5f), 1.0f);
    output.position = float4(dot(sprite_camera_row(0u), world), dot(sprite_camera_row(1u), world),
                             dot(sprite_camera_row(2u), world), dot(sprite_camera_row(3u), world));
    // Soft across, like the disc; the texture along by life.
    output.uv = float2(0.0f, corner.y);
    float age = sprite_age != sprite_absent ? asfloat(sprite_particles.Load(base + sprite_age)) : 0.0f;
    float lifetime = sprite_lifetime != sprite_absent ? asfloat(sprite_particles.Load(base + sprite_lifetime)) : 1.0f;
    float life = lifetime > 0.0f ? saturate(age / lifetime) : 0.0f;
    output.cells = float2(life, 0.5f - corner.y * 0.5f).xyxy;
    output.blend = 0.0f;
    output.color = sprite_color != sprite_absent ? asfloat(sprite_particles.Load4(base + sprite_color))
                                                 : float4(1.0f, 1.0f, 1.0f, 1.0f);
    return output;
}

// The record a sprite instance draws.
uint sprite_record(uint instance) {
    if (sprite_sorted == 0u) {
        return instance;
    }
    uint first = sprite_sorted >= 2u ? sprite_order.Load((sprite_sorted - 2u) * 4u) : 0u;
    return sprite_order.Load((first + instance) * 4u);
}

// Vertex `vertex` of the mesh drawn for particle `instance`: turned by the
// particle's orientation, or else so its +Z runs along its velocity
// (velocity facing) or spun about Y by its rotation; scaled by its size, and
// lit by a light at the camera.
SpriteVertex sprite_mesh_vertex(uint vertex, uint instance) {
    uint record = sprite_sorted != 0u ? sprite_order.Load(instance * 4u) : instance;
    uint base = record * sprite_stride;
    // Which model this particle is drawn as. The draw's vertex count is the
    // largest model's, so a particle drawn as a smaller one folds the
    // vertices past its own away.
    uint kinds = max(sprite_models.Load(4u), 1u);
    uint kind_offset = sprite_models.Load(0u);
    uint kind = 0u;
    if (kind_offset != sprite_absent) {
        kind = min(sprite_particles.Load(base + kind_offset), kinds - 1u);
    }
    uint first_index = sprite_models.Load(8u + kind * 28u);
    uint index_count = sprite_models.Load(12u + kind * 28u);
    float kind_scale = asfloat(sprite_models.Load(16u + kind * 28u));
    float4 kind_tint = asfloat(sprite_models.Load4(20u + kind * 28u));
    if (vertex >= index_count) {
        SpriteVertex folded = (SpriteVertex)0;
        return folded;
    }
    float3 position = asfloat(sprite_particles.Load3(base + sprite_position));
    float4 color = sprite_color != sprite_absent ? asfloat(sprite_particles.Load4(base + sprite_color))
                                                 : float4(1.0f, 1.0f, 1.0f, 1.0f);
    float size = sprite_size != sprite_absent ? asfloat(sprite_particles.Load(base + sprite_size)) : 0.1f;
    color *= kind_tint;
    if (sprite_premultiplied != 0u) { color.rgb *= kind_tint.a; }
    uint index = sprite_mesh_indices.Load((first_index + vertex) * 4u);
    float3 local = asfloat(sprite_mesh_vertices.Load3(index * 32u));
    float3 normal = asfloat(sprite_mesh_vertices.Load3(index * 32u + 12u));
    float2 uv = asfloat(sprite_mesh_vertices.Load2(index * 32u + 24u));
    float3 ax = float3(1.0f, 0.0f, 0.0f);
    float3 ay = float3(0.0f, 1.0f, 0.0f);
    float3 az = float3(0.0f, 0.0f, 1.0f);
    bool aligned = false;
    if (sprite_orientation != sprite_absent) {
        // The quaternion's rotation matrix, column by column.
        float4 q = asfloat(sprite_particles.Load4(base + sprite_orientation));
        ax = float3(1.0f - 2.0f * (q.y * q.y + q.z * q.z), 2.0f * (q.x * q.y + q.w * q.z),
                    2.0f * (q.x * q.z - q.w * q.y));
        ay = float3(2.0f * (q.x * q.y - q.w * q.z), 1.0f - 2.0f * (q.x * q.x + q.z * q.z),
                    2.0f * (q.y * q.z + q.w * q.x));
        az = float3(2.0f * (q.x * q.z + q.w * q.y), 2.0f * (q.y * q.z - q.w * q.x),
                    1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        aligned = true;
    } else if (sprite_facing == 1u && sprite_velocity != sprite_absent) {
        float3 velocity = asfloat(sprite_particles.Load3(base + sprite_velocity));
        float len = length(velocity);
        if (len > 1.0e-6f) {
            az = velocity / len;
            float3 reference = abs(az.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
            ax = normalize(cross(reference, az));
            ay = cross(az, ax);
            aligned = true;
        }
    }
    if (!aligned && sprite_rotation != sprite_absent) {
        float angle = asfloat(sprite_particles.Load(base + sprite_rotation));
        float c = cos(angle);
        float s = sin(angle);
        ax = float3(c, 0.0f, -s);
        az = float3(s, 0.0f, c);
    }
    float4 world = float4(position + (ax * local.x + ay * local.y + az * local.z) * (size * kind_scale), 1.0f);
    float3 n = normalize(ax * normal.x + ay * normal.y + az * normal.z);
    SpriteVertex output = (SpriteVertex)0;
    output.position = float4(dot(sprite_camera_row(0u), world), dot(sprite_camera_row(1u), world),
                             dot(sprite_camera_row(2u), world), dot(sprite_camera_row(3u), world));
    // Solid (no disc), textured by the mesh's own coordinates.
    output.uv = float2(0.0f, 0.0f);
    output.cells = uv.xyxy;
    output.blend = 0.0f;
    if (sprite_lighting() != 0u) {
        // Lit by the scene through its own normal, taken into the camera's
        // frame, where the pixel stage weighs the six directions.
        const float3 right = sprite_camera_row(4u).xyz;
        const float3 up = sprite_camera_row(5u).xyz;
        const float3 front = -sprite_camera_row(6u).xyz;
        sprite_radiance(right, up, front, output.radiance);
        output.normal = float3(dot(n, right), dot(n, up), dot(n, front));
        output.color = color;
    } else {
        // Unlit meshes keep a light at the camera, so their shape reads.
        float shade = 0.35f + 0.65f * saturate(dot(n, -sprite_camera_row(6u).xyz));
        output.color = float4(color.rgb * shade, color.a);
    }
    return output;
}

SpriteVertex sprite_vertex(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    if (sprite_geometry == 1u) {
        return sprite_ribbon_vertex(vertex, instance);
    }
    if (sprite_geometry == 2u) {
        return sprite_mesh_vertex(vertex, instance);
    }
    uint record = sprite_record(instance);
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
    SpriteVertex output = (SpriteVertex)0;
    output.position = float4(dot(sprite_camera_row(0u), world), dot(sprite_camera_row(1u), world),
                             dot(sprite_camera_row(2u), world), dot(sprite_camera_row(3u), world));
    output.uv = corner;
    sprite_flipbook_cells(base, corner, output.cells, output.blend);
    if (sprite_lighting() != 0u) {
        // The frame the quad was built in, turned with it, so the rounded
        // normal and the six-way maps follow the picture.
        sprite_radiance(normalize(right), normalize(up), -forward, output.radiance);
    }
    output.color = color;
    return output;
}

// The light a pixel of a lit particle receives: the six directions weighed
// by a normal (a sprite's, rounded from its corner by the look's curvature;
// a mesh's own) or by the six-way maps' texels. One for an unlit particle.
float3 sprite_light(SpriteVertex input) {
    const uint mode = sprite_lighting();
    if (mode == 0u) {
        return float3(1.0f, 1.0f, 1.0f);
    }
    float weights[6];
    if (mode == 2u) {
        const float3 a = lerp(sprite_texture.Sample(sprite_sampler, input.cells.xy),
                              sprite_texture.Sample(sprite_sampler, input.cells.zw), input.blend).rgb;
        const float3 b = lerp(sprite_lightmap.Sample(sprite_sampler, input.cells.xy),
                              sprite_lightmap.Sample(sprite_sampler, input.cells.zw), input.blend).rgb;
        weights[0] = a.r; weights[1] = b.r;
        weights[2] = a.g; weights[3] = b.g;
        weights[4] = a.b; weights[5] = b.b;
    } else {
        float3 n;
        if (sprite_geometry == 2u) {
            n = normalize(input.normal);
        } else {
            const float2 turned = input.uv * sprite_look().x;
            n = normalize(float3(turned, sqrt(saturate(1.0f - dot(turned, turned)))));
        }
        weights[0] = max(n.x, 0.0f); weights[1] = max(-n.x, 0.0f);
        weights[2] = max(n.y, 0.0f); weights[3] = max(-n.y, 0.0f);
        weights[4] = max(n.z, 0.0f); weights[5] = max(-n.z, 0.0f);
    }
    float3 light = float3(0.0f, 0.0f, 0.0f);
    [unroll] for (uint i = 0u; i < 6u; ++i) {
        light += weights[i] * input.radiance[i];
    }
    return light;
}

// The texture times the particle colour, or a soft disc, times the light
// the particle receives. Output is premultiplied for the backend's blend
// equations. Six-way maps are light, not colour: only their alpha covers.
float4 sprite_pixel(SpriteVertex input) : SV_Target {
    input.color.rgb *= sprite_light(input);
    if ((sprite_flags & 1u) != 0u) {
        float4 texel = lerp(sprite_texture.Sample(sprite_sampler, input.cells.xy),
                            sprite_texture.Sample(sprite_sampler, input.cells.zw), input.blend);
        if (sprite_lighting() == 2u) {
            texel.rgb = float3(1.0f, 1.0f, 1.0f);
        }
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
