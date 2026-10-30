#ifndef AJFX_PARTICLES_HLSLI
#define AJFX_PARTICLES_HLSLI

// The per-particle half of the GPU runtime: bindings, the particle record
// and one function per module. Generated emitter kernels call the module
// functions in stack order.
//
// Each function mirrors the CPU reference (src/simulation.cpp) operation for
// operation. Everything a module derives from its inputs once per step
// (normalized axes, drag factors, dt products) arrives precomputed in the
// module's data, packed by src/gpu/pack.cpp in the order read here. Dot and
// cross products are written out rather than using intrinsics so their
// evaluation order matches the reference. Compile with -Gis.

#include "ajfx_random.hlsli"

// b0 in space 1 is where the media server's RHI binds a dispatch's uniform bytes,
// and u0..u7 in space 0 where it binds writable buffers, so these kernels run
// unchanged on either backend.
cbuffer AjfxConstants : register(b0, space1) {
    uint ajfx_stride;       // bytes per particle record
    uint ajfx_capacity;
    uint ajfx_birth_count;
    uint ajfx_spawn_base;
    uint ajfx_seed;
    float ajfx_dt;
    uint ajfx_ages_offset;  // in floats, within ajfx_data
    uint ajfx_reserved0;
    uint4 ajfx_reserved1;
    uint4 ajfx_reserved2;
};

RWByteAddressBuffer ajfx_src : register(u0);      // particles at the start of the step
RWByteAddressBuffer ajfx_dst : register(u1);      // compacted survivors, then newborns
RWByteAddressBuffer ajfx_counters : register(u2); // alive, compacted, refused
RWByteAddressBuffer ajfx_flags : register(u3);    // 1 per surviving particle
RWByteAddressBuffer ajfx_offsets : register(u4);  // survivor's index within its block
RWByteAddressBuffer ajfx_blocks : register(u5);   // block totals, then block offsets
RWByteAddressBuffer ajfx_data : register(u6);     // module data, then birth ages
RWByteAddressBuffer ajfx_args : register(u7);     // indirect dispatch sizes

static const uint ajfx_group_size = 256u;
static const uint ajfx_block_size = 4096u;
static const uint ajfx_max_groups_x = 65535u;
static const float ajfx_two_pi = asfloat(0x40c90fdbu); // 2 * float(pi), as the reference computes it
static const float ajfx_infinity = asfloat(0x7f800000u);

uint ajfx_index(uint3 group, uint thread) {
    return (group.y * ajfx_max_groups_x + group.x) * ajfx_group_size + thread;
}

struct Particle {
    float3 position;
    float3 velocity;
    float age;
    float lifetime;
    float4 base_color;
    float4 color;
    float base_size;
    float size;
    float rotation;
    uint spawn_index;
    float4 orientation; // x, y, z, w
    float3 angular_velocity;
};

Particle ajfx_default_particle() {
    Particle p;
    p.position = float3(0.0f, 0.0f, 0.0f);
    p.velocity = float3(0.0f, 0.0f, 0.0f);
    p.age = 0.0f;
    p.lifetime = 1.0f;
    p.base_color = float4(1.0f, 1.0f, 1.0f, 1.0f);
    p.color = float4(1.0f, 1.0f, 1.0f, 1.0f);
    p.base_size = 0.1f;
    p.size = 0.1f;
    p.rotation = 0.0f;
    p.spawn_index = 0u;
    p.orientation = float4(0.0f, 0.0f, 0.0f, 1.0f);
    p.angular_velocity = float3(0.0f, 0.0f, 0.0f);
    return p;
}

float ajfx_f(uint o) { return asfloat(ajfx_data.Load(o * 4u)); }
float3 ajfx_f3(uint o) { return float3(ajfx_f(o), ajfx_f(o + 1u), ajfx_f(o + 2u)); }
float4 ajfx_f4(uint o) { return float4(ajfx_f(o), ajfx_f(o + 1u), ajfx_f(o + 2u), ajfx_f(o + 3u)); }
uint ajfx_u32(uint o) { return ajfx_data.Load(o * 4u); }

float ajfx_mix(float a, float b, float t) { return a + (b - a) * t; }
float ajfx_dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float3 ajfx_cross(float3 a, float3 b) {
    return float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

float ajfx_random(Particle p, uint module_id, uint draw) {
    return ajfx_particle_unit(ajfx_seed, p.spawn_index, ajfx_hash_mix(module_id, draw));
}

// ------------------------------------------------------------ noise

float ajfx_fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

float ajfx_corner(uint hash, float x, float y, float z) {
    switch (hash % 12u) {
    case 0u: return x + y;
    case 1u: return -x + y;
    case 2u: return x - y;
    case 3u: return -x - y;
    case 4u: return x + z;
    case 5u: return -x + z;
    case 6u: return x - z;
    case 7u: return -x - z;
    case 8u: return y + z;
    case 9u: return -y + z;
    case 10u: return y - z;
    default: return -y - z;
    }
}

uint ajfx_lattice(uint seed, uint ix, uint iy, uint iz) {
    return ajfx_hash_mix(ajfx_hash_mix(ajfx_hash_mix(seed, ix), iy), iz);
}

float ajfx_gradient_noise(float3 q, uint seed) {
    float fx = floor(q.x);
    float fy = floor(q.y);
    float fz = floor(q.z);
    uint ix = asuint(int(fx));
    uint iy = asuint(int(fy));
    uint iz = asuint(int(fz));
    float x = q.x - fx;
    float y = q.y - fy;
    float z = q.z - fz;
    float c000 = ajfx_corner(ajfx_lattice(seed, ix, iy, iz), x, y, z);
    float c100 = ajfx_corner(ajfx_lattice(seed, ix + 1u, iy, iz), x - 1.0f, y, z);
    float c010 = ajfx_corner(ajfx_lattice(seed, ix, iy + 1u, iz), x, y - 1.0f, z);
    float c110 = ajfx_corner(ajfx_lattice(seed, ix + 1u, iy + 1u, iz), x - 1.0f, y - 1.0f, z);
    float c001 = ajfx_corner(ajfx_lattice(seed, ix, iy, iz + 1u), x, y, z - 1.0f);
    float c101 = ajfx_corner(ajfx_lattice(seed, ix + 1u, iy, iz + 1u), x - 1.0f, y, z - 1.0f);
    float c011 = ajfx_corner(ajfx_lattice(seed, ix, iy + 1u, iz + 1u), x, y - 1.0f, z - 1.0f);
    float c111 = ajfx_corner(ajfx_lattice(seed, ix + 1u, iy + 1u, iz + 1u), x - 1.0f, y - 1.0f, z - 1.0f);
    float u = ajfx_fade(x);
    float v = ajfx_fade(y);
    float w = ajfx_fade(z);
    return ajfx_mix(ajfx_mix(ajfx_mix(c000, c100, u), ajfx_mix(c010, c110, u), v),
                    ajfx_mix(ajfx_mix(c001, c101, u), ajfx_mix(c011, c111, u), v), w);
}

float3 ajfx_curl_field(float3 q, uint seed) {
    const float e = 1.0e-2f;
    const float inv = 1.0f / (2.0f * e);
    uint s1 = ajfx_hash_mix(seed, 1u);
    uint s2 = ajfx_hash_mix(seed, 2u);
    uint s3 = ajfx_hash_mix(seed, 3u);
    float3 dx = float3(e, 0.0f, 0.0f);
    float3 dy = float3(0.0f, e, 0.0f);
    float3 dz = float3(0.0f, 0.0f, e);
    float d3_dy = (ajfx_gradient_noise(q + dy, s3) - ajfx_gradient_noise(q - dy, s3)) * inv;
    float d2_dz = (ajfx_gradient_noise(q + dz, s2) - ajfx_gradient_noise(q - dz, s2)) * inv;
    float d1_dz = (ajfx_gradient_noise(q + dz, s1) - ajfx_gradient_noise(q - dz, s1)) * inv;
    float d3_dx = (ajfx_gradient_noise(q + dx, s3) - ajfx_gradient_noise(q - dx, s3)) * inv;
    float d2_dx = (ajfx_gradient_noise(q + dx, s2) - ajfx_gradient_noise(q - dx, s2)) * inv;
    float d1_dy = (ajfx_gradient_noise(q + dy, s1) - ajfx_gradient_noise(q - dy, s1)) * inv;
    return float3(d3_dy - d2_dz, d1_dz - d3_dx, d2_dx - d1_dy);
}

// ------------------------------------------------------------ init modules

// [min, max]
void ajfx_lifetime(inout Particle p, uint o, uint id) {
    p.lifetime = ajfx_mix(ajfx_f(o), ajfx_f(o + 1u), ajfx_random(p, id, 0u));
}

float3 ajfx_unit_sphere(float u, float v) {
    float z = 2.0f * u - 1.0f;
    float r = sqrt(max(0.0f, 1.0f - z * z));
    float phi = ajfx_two_pi * v;
    return float3(r * cos(phi), r * sin(phi), z);
}

// [shape, center.xyz, radius, size.xyz, surface_only]
void ajfx_shape_location(inout Particle p, uint o, uint id) {
    uint shape = ajfx_u32(o);
    float3 center = ajfx_f3(o + 1u);
    float radius = ajfx_f(o + 4u);
    float3 size = ajfx_f3(o + 5u);
    bool surface = ajfx_u32(o + 8u) != 0u;
    float3 offset = float3(0.0f, 0.0f, 0.0f);
    if (shape == 1u) { // sphere
        float r = surface ? radius : radius * pow(ajfx_random(p, id, 2u), 1.0f / 3.0f);
        offset = ajfx_unit_sphere(ajfx_random(p, id, 0u), ajfx_random(p, id, 1u)) * r;
    } else if (shape == 2u) { // box
        offset = float3((ajfx_random(p, id, 0u) - 0.5f) * size.x, (ajfx_random(p, id, 1u) - 0.5f) * size.y,
                        (ajfx_random(p, id, 2u) - 0.5f) * size.z);
        if (surface) {
            float yz = size.y * size.z;
            float xz = size.x * size.z;
            float xy = size.x * size.y;
            float pick = ajfx_random(p, id, 3u) * (yz + xz + xy);
            float side = ajfx_random(p, id, 4u) < 0.5f ? -0.5f : 0.5f;
            if (pick < yz) {
                offset.x = side * size.x;
            } else if (pick < yz + xz) {
                offset.y = side * size.y;
            } else {
                offset.z = side * size.z;
            }
        }
    } else if (shape == 3u) { // disc
        float r = surface ? radius : radius * sqrt(ajfx_random(p, id, 1u));
        float phi = ajfx_two_pi * ajfx_random(p, id, 0u);
        offset = float3(r * cos(phi), 0.0f, r * sin(phi));
    }
    p.position = center + offset;
}

// [axis.xyz, t1.xyz, t2.xyz, cos_spread, speed_min, speed_max]
void ajfx_velocity(inout Particle p, uint o, uint id) {
    float3 axis = ajfx_f3(o);
    float3 t1 = ajfx_f3(o + 3u);
    float3 t2 = ajfx_f3(o + 6u);
    float cos_spread = ajfx_f(o + 9u);
    float cos_theta = 1.0f - ajfx_random(p, id, 0u) * (1.0f - cos_spread);
    float sin_theta = sqrt(max(0.0f, 1.0f - cos_theta * cos_theta));
    float phi = ajfx_two_pi * ajfx_random(p, id, 1u);
    float3 direction = t1 * (sin_theta * cos(phi)) + t2 * (sin_theta * sin(phi)) + axis * cos_theta;
    p.velocity = direction * ajfx_mix(ajfx_f(o + 10u), ajfx_f(o + 11u), ajfx_random(p, id, 2u));
}

// [a.rgba, b.rgba]
void ajfx_color(inout Particle p, uint o, uint id) {
    float4 a = ajfx_f4(o);
    float4 b = ajfx_f4(o + 4u);
    float t = ajfx_random(p, id, 0u);
    p.base_color = float4(ajfx_mix(a.x, b.x, t), ajfx_mix(a.y, b.y, t), ajfx_mix(a.z, b.z, t), ajfx_mix(a.w, b.w, t));
}

// [min, max]
void ajfx_size(inout Particle p, uint o, uint id) {
    p.base_size = ajfx_mix(ajfx_f(o), ajfx_f(o + 1u), ajfx_random(p, id, 0u));
}

// [min, max, radians per degree]
void ajfx_rotation(inout Particle p, uint o, uint id) {
    p.rotation = ajfx_mix(ajfx_f(o), ajfx_f(o + 1u), ajfx_random(p, id, 0u)) * ajfx_f(o + 2u);
}

// [speed_min, speed_max, radians per degree, random orientation]
void ajfx_tumble(inout Particle p, uint o, uint id) {
    if (ajfx_u32(o + 3u) != 0u) {
        // Shoemake's uniform rotation from three draws.
        float u1 = ajfx_random(p, id, 0u);
        float a = sqrt(1.0f - u1);
        float b = sqrt(u1);
        float t2 = ajfx_two_pi * ajfx_random(p, id, 1u);
        float t3 = ajfx_two_pi * ajfx_random(p, id, 2u);
        p.orientation = float4(a * sin(t2), a * cos(t2), b * sin(t3), b * cos(t3));
    }
    p.angular_velocity = ajfx_unit_sphere(ajfx_random(p, id, 3u), ajfx_random(p, id, 4u)) *
                         (ajfx_mix(ajfx_f(o), ajfx_f(o + 1u), ajfx_random(p, id, 5u)) * ajfx_f(o + 2u));
}

// ------------------------------------------------------------ forces

// [acceleration * dt]
void ajfx_gravity(inout Particle p, uint o, uint id) {
    p.velocity += ajfx_f3(o);
}

// [1 / (1 + coefficient * dt)]
void ajfx_drag(inout Particle p, uint o, uint id) {
    p.velocity = p.velocity * ajfx_f(o);
}

// [frequency, strength * dt, scroll, seed]
void ajfx_curl_noise(inout Particle p, uint o, uint id) {
    float3 q = p.position * ajfx_f(o) + float3(0.0f, 0.0f, ajfx_f(o + 2u));
    p.velocity += ajfx_curl_field(q, ajfx_u32(o + 3u)) * ajfx_f(o + 1u);
}

// [axis.xyz, center.xyz, strength * dt]
void ajfx_vortex(inout Particle p, uint o, uint id) {
    float3 axis = ajfx_f3(o);
    float3 r = p.position - ajfx_f3(o + 3u);
    float3 radial = r - axis * ajfx_dot(r, axis);
    float len = sqrt(ajfx_dot(radial, radial));
    if (len > 1.0e-6f) {
        p.velocity += ajfx_cross(axis, radial) * (ajfx_f(o + 6u) / len);
    }
}

// [position.xyz, strength, radius, dt]
void ajfx_attractor(inout Particle p, uint o, uint id) {
    float3 d = ajfx_f3(o) - p.position;
    float dist = sqrt(ajfx_dot(d, d));
    if (dist <= 1.0e-6f) {
        return;
    }
    float radius = ajfx_f(o + 4u);
    float falloff = radius > 0.0f ? max(0.0f, 1.0f - dist / radius) : 1.0f;
    p.velocity += d * (ajfx_f(o + 3u) * falloff * ajfx_f(o + 5u) / dist);
}

// ------------------------------------------------------------ constraints and appearance

#ifdef AJFX_RIGID_DATA
// An emitter with a RigidBody defines AJFX_RIGID_DATA as that module's data
// offset, and its particles meet colliders as solid balls. The impulse
// factors are the reference's decimals.
void ajfx_respond_rigid(inout Particle p, float dist, float3 n, float restitution, float slide, bool kill) {
    float radius = p.size * ajfx_f(AJFX_RIGID_DATA + 2u);
    float gap = dist - radius;
    if (gap >= 0.0f) {
        return;
    }
    if (kill) {
        p.age = ajfx_infinity;
        return;
    }
    p.position = p.position - n * gap;
    float into = ajfx_dot(p.velocity, n);
    if (!(into < 0.0f)) {
        return;
    }
    float bounce = -into < ajfx_f(AJFX_RIGID_DATA + 3u) ? 0.0f : restitution;
    p.velocity = p.velocity - n * (into * (1.0f + bounce));
    float3 arm = n * -radius;
    float3 contact = p.velocity + ajfx_cross(p.angular_velocity, arm);
    float3 slip = contact - n * ajfx_dot(contact, n);
    float grip = 1.0f - slide;
    p.velocity = p.velocity - slip * (grip * 0.2857142857f);
    if (radius > 1.0e-6f) {
        p.angular_velocity = p.angular_velocity - ajfx_cross(arm, slip) * (grip * 0.7142857143f / (radius * radius));
    }
}
#endif

// Every collider's response, given the particle's signed distance (negative
// where it may not be) and the unit direction back to where it may.
void ajfx_respond(inout Particle p, float dist, float3 n, float restitution, float slide, bool kill) {
#ifdef AJFX_RIGID_DATA
    ajfx_respond_rigid(p, dist, n, restitution, slide, kill);
#else
    if (dist >= 0.0f) {
        return;
    }
    if (kill) {
        p.age = ajfx_infinity;
        return;
    }
    p.position = p.position - n * dist;
    float into = ajfx_dot(p.velocity, n);
    if (into < 0.0f) {
        float3 normal = n * into;
        p.velocity = (p.velocity - normal) * slide - normal * restitution;
    }
#endif
}

// [normal.xyz, offset, restitution, 1 - friction, kill]
void ajfx_plane_collision(inout Particle p, uint o, uint id) {
    float3 n = ajfx_f3(o);
    ajfx_respond(p, ajfx_dot(n, p.position) - ajfx_f(o + 3u), n, ajfx_f(o + 4u), ajfx_f(o + 5u),
                 ajfx_u32(o + 6u) != 0u);
}

// [center.xyz, radius, restitution, 1 - friction, inside, kill]
void ajfx_sphere_collision(inout Particle p, uint o, uint id) {
    float3 d = p.position - ajfx_f3(o);
    float len = sqrt(ajfx_dot(d, d));
    float3 out_dir = len > 1.0e-6f ? d * (1.0f / len) : float3(0.0f, 1.0f, 0.0f);
    float radius = ajfx_f(o + 3u);
    bool kill = ajfx_u32(o + 7u) != 0u;
    if (ajfx_u32(o + 6u) != 0u) {
        ajfx_respond(p, radius - len, out_dir * -1.0f, ajfx_f(o + 4u), ajfx_f(o + 5u), kill);
    } else {
        ajfx_respond(p, len - radius, out_dir, ajfx_f(o + 4u), ajfx_f(o + 5u), kill);
    }
}

// The update kernel's distance fields share u7 with the bookkeeping
// kernel's indirect arguments, which the update kernel never touches.
#define ajfx_fields ajfx_args

float ajfx_field_at(uint base, uint nx, uint ny, uint x, uint y, uint z) {
    return asfloat(ajfx_fields.Load((base + (z * ny + y) * nx + x) * 4u));
}

// [field offset, nx, ny, nz, origin.xyz, spacing, offset.xyz, restitution,
//  1 - friction, inside, kill]. Trilinear distance and its exact gradient,
// as sample_field (src/field.cpp) computes them; nothing outside the grid.
void ajfx_sdf_collision(inout Particle p, uint o, uint id) {
    uint base = ajfx_u32(o);
    uint nx = ajfx_u32(o + 1u);
    uint ny = ajfx_u32(o + 2u);
    uint nz = ajfx_u32(o + 3u);
    float3 origin = ajfx_f3(o + 4u);
    float spacing = ajfx_f(o + 7u);
    float3 q = p.position - ajfx_f3(o + 8u);
    float gx = (q.x - origin.x) / spacing;
    float gy = (q.y - origin.y) / spacing;
    float gz = (q.z - origin.z) / spacing;
    if (!(gx >= 0.0f && gy >= 0.0f && gz >= 0.0f && gx <= (float)(nx - 1u) && gy <= (float)(ny - 1u) &&
          gz <= (float)(nz - 1u))) {
        return;
    }
    uint ix = min((uint)floor(gx), nx - 2u);
    uint iy = min((uint)floor(gy), ny - 2u);
    uint iz = min((uint)floor(gz), nz - 2u);
    float fx = gx - (float)ix;
    float fy = gy - (float)iy;
    float fz = gz - (float)iz;
    float c000 = ajfx_field_at(base, nx, ny, ix, iy, iz);
    float c100 = ajfx_field_at(base, nx, ny, ix + 1u, iy, iz);
    float c010 = ajfx_field_at(base, nx, ny, ix, iy + 1u, iz);
    float c110 = ajfx_field_at(base, nx, ny, ix + 1u, iy + 1u, iz);
    float c001 = ajfx_field_at(base, nx, ny, ix, iy, iz + 1u);
    float c101 = ajfx_field_at(base, nx, ny, ix + 1u, iy, iz + 1u);
    float c011 = ajfx_field_at(base, nx, ny, ix, iy + 1u, iz + 1u);
    float c111 = ajfx_field_at(base, nx, ny, ix + 1u, iy + 1u, iz + 1u);
    float c00 = c000 + (c100 - c000) * fx;
    float c10 = c010 + (c110 - c010) * fx;
    float c01 = c001 + (c101 - c001) * fx;
    float c11 = c011 + (c111 - c011) * fx;
    float c0 = c00 + (c10 - c00) * fy;
    float c1 = c01 + (c11 - c01) * fy;
    float e00 = c100 - c000;
    float e10 = c110 - c010;
    float e01 = c101 - c001;
    float e11 = c111 - c011;
    float ex0 = e00 + (e10 - e00) * fy;
    float ex1 = e01 + (e11 - e01) * fy;
    float dy0 = c10 - c00;
    float dy1 = c11 - c01;
    float dist = c0 + (c1 - c0) * fz;
    float3 g = float3((ex0 + (ex1 - ex0) * fz) / spacing, (dy0 + (dy1 - dy0) * fz) / spacing, (c1 - c0) / spacing);
    float len = sqrt(ajfx_dot(g, g));
    float3 out_dir = len > 1.0e-6f ? g * (1.0f / len) : float3(0.0f, 1.0f, 0.0f);
    bool kill = ajfx_u32(o + 14u) != 0u;
    if (ajfx_u32(o + 13u) != 0u) {
        ajfx_respond(p, -dist, out_dir * -1.0f, ajfx_f(o + 11u), ajfx_f(o + 12u), kill);
    } else {
        ajfx_respond(p, dist, out_dir, ajfx_f(o + 11u), ajfx_f(o + 12u), kill);
    }
}

// [center.xyz, half_size.xyz, restitution, 1 - friction, inside, kill]
void ajfx_box_collision(inout Particle p, uint o, uint id) {
    float3 d = p.position - ajfx_f3(o);
    float3 h = ajfx_f3(o + 3u);
    float3 s = float3(d.x < 0.0f ? -1.0f : 1.0f, d.y < 0.0f ? -1.0f : 1.0f, d.z < 0.0f ? -1.0f : 1.0f);
    float3 q = float3(abs(d.x) - h.x, abs(d.y) - h.y, abs(d.z) - h.z);
    float3 over = float3(max(q.x, 0.0f), max(q.y, 0.0f), max(q.z, 0.0f));
    float outside = sqrt(ajfx_dot(over, over));
    float restitution = ajfx_f(o + 6u);
    float slide = ajfx_f(o + 7u);
    bool kill = ajfx_u32(o + 9u) != 0u;
    float3 n;
    float dist = outside;
    if (outside > 0.0f) {
        n = float3(s.x * over.x / outside, s.y * over.y / outside, s.z * over.z / outside);
    } else {
        n = float3(s.x, 0.0f, 0.0f);
        dist = q.x;
        if (q.y > dist) {
            n = float3(0.0f, s.y, 0.0f);
            dist = q.y;
        }
        if (q.z > dist) {
            n = float3(0.0f, 0.0f, s.z);
            dist = q.z;
        }
    }
    if (ajfx_u32(o + 8u) != 0u) {
        ajfx_respond(p, -dist, n * -1.0f, restitution, slide, kill);
    } else {
        ajfx_respond(p, dist, n, restitution, slide, kill);
    }
}

// [1 / (1 + angular_drag * dt), dt / 2, contact_scale, rest_speed]
void ajfx_rigid_body(inout Particle p, uint o, uint id) {
    float3 w = p.angular_velocity * ajfx_f(o);
    p.angular_velocity = w;
    float h = ajfx_f(o + 1u);
    float4 q = p.orientation;
    float x = q.x + h * (w.x * q.w + w.y * q.z - w.z * q.y);
    float y = q.y + h * (w.y * q.w + w.z * q.x - w.x * q.z);
    float z = q.z + h * (w.z * q.w + w.x * q.y - w.y * q.x);
    float qw = q.w - h * (w.x * q.x + w.y * q.y + w.z * q.z);
    float inverse = 1.0f / sqrt(x * x + y * y + z * z + qw * qw);
    p.orientation = float4(x * inverse, y * inverse, z * inverse, qw * inverse);
}

float ajfx_normalized_age(Particle p) {
    return min(max(p.age / p.lifetime, 0.0f), 1.0f);
}

// [count, (t, r, g, b, a) per key]
void ajfx_color_over_life(inout Particle p, uint o, uint id) {
    uint count = ajfx_u32(o);
    float t = ajfx_normalized_age(p);
    uint next = 0u;
    while (next < count && ajfx_f(o + 1u + next * 5u) < t) {
        ++next;
    }
    float4 value;
    if (next == 0u) {
        value = ajfx_f4(o + 2u);
    } else if (next == count) {
        value = ajfx_f4(o + 2u + (count - 1u) * 5u);
    } else {
        uint a = o + 1u + (next - 1u) * 5u;
        uint b = a + 5u;
        float span = ajfx_f(b) - ajfx_f(a);
        float f = span > 0.0f ? (t - ajfx_f(a)) / span : 1.0f;
        float4 va = ajfx_f4(a + 1u);
        float4 vb = ajfx_f4(b + 1u);
        value = float4(ajfx_mix(va.x, vb.x, f), ajfx_mix(va.y, vb.y, f), ajfx_mix(va.z, vb.z, f),
                       ajfx_mix(va.w, vb.w, f));
    }
    p.color = p.base_color * value;
}

// [count, (t, value) per key]
void ajfx_size_over_life(inout Particle p, uint o, uint id) {
    uint count = ajfx_u32(o);
    float t = ajfx_normalized_age(p);
    uint next = 0u;
    while (next < count && ajfx_f(o + 1u + next * 2u) < t) {
        ++next;
    }
    float value;
    if (next == 0u) {
        value = ajfx_f(o + 2u);
    } else if (next == count) {
        value = ajfx_f(o + 2u + (count - 1u) * 2u);
    } else {
        uint a = o + 1u + (next - 1u) * 2u;
        uint b = a + 2u;
        float span = ajfx_f(b) - ajfx_f(a);
        float f = span > 0.0f ? (t - ajfx_f(a)) / span : 1.0f;
        value = ajfx_mix(ajfx_f(a + 1u), ajfx_f(b + 1u), f);
    }
    p.size = p.base_size * value;
}

#endif
