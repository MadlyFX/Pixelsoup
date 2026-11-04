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
    uint ajfx_spawn_base;   // unused: each newborn's spawn index is in ajfx_data
    uint ajfx_seed;
    float ajfx_dt;
    uint ajfx_ages_offset;  // in floats, within ajfx_data; spawn indices follow capacity later
    uint ajfx_tick_lo;      // the step being taken (detail::step_key), for the shed kernel's rolls
    uint ajfx_tick_hi;
    uint ajfx_reserved0;
    uint ajfx_reserved1;
    uint ajfx_reserved2;
    uint4 ajfx_reserved3;
};

RWByteAddressBuffer ajfx_src : register(u0);      // particles at the start of the step
RWByteAddressBuffer ajfx_dst : register(u1);      // compacted survivors, then newborns
RWByteAddressBuffer ajfx_counters : register(u2); // alive, compacted, refused, births shed from particles this step
RWByteAddressBuffer ajfx_flags : register(u3);    // 1 per surviving particle
RWByteAddressBuffer ajfx_offsets : register(u4);  // survivor's index within its block
RWByteAddressBuffer ajfx_blocks : register(u5);   // block totals, then block offsets
RWByteAddressBuffer ajfx_data : register(u6);     // module data, birth ages, then birth spawn indices
RWByteAddressBuffer ajfx_args : register(u7);     // indirect dispatch sizes

static const uint ajfx_group_size = 256u;
static const uint ajfx_block_size = 4096u;
static const uint ajfx_max_groups_x = 65535u;
static const float ajfx_two_pi = asfloat(0x40c90fdbu); // 2 * float(pi), as the reference computes it
static const float ajfx_infinity = asfloat(0x7f800000u);

uint ajfx_index(uint3 group, uint thread) {
    return (group.y * ajfx_max_groups_x + group.x) * ajfx_group_size + thread;
}

// This step's births: the ones the CPU scheduled (ajfx_birth_count) and the
// ones the shed kernel worked out from other emitters' particles, which it
// counted into the counters' fourth word (zero for an emitter that sheds
// from none).
uint ajfx_births() {
    return ajfx_birth_count + ajfx_counters.Load(12u);
}

void ajfx_write_groups(uint at, uint threads, uint per_group) {
    uint groups = (threads + per_group - 1u) / per_group;
    uint x = min(groups, ajfx_max_groups_x);
    uint y = groups == 0u ? 1u : (groups + ajfx_max_groups_x - 1u) / ajfx_max_groups_x;
    ajfx_args.Store3(at, uint3(x, y, 1u));
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
    float spin;
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
    p.spin = 0.0f;
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

// Whether a module with a scope acts on this particle: [every particle, 8
// words of one bit per spawn source], packed by pack_scope. The source is the
// spawn index's top 8 bits (spawn_source).
bool ajfx_in_scope(Particle p, uint o) {
    if (ajfx_u32(o) != 0u) {
        return true;
    }
    uint source = p.spawn_index >> 24u;
    return ((ajfx_u32(o + 1u + (source >> 5u)) >> (source & 31u)) & 1u) != 0u;
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

float ajfx_normalized_age(Particle p) {
    return min(max(p.age / p.lifetime, 0.0f), 1.0f);
}

// A curve at t, from [count, (t, value) per key], as the reference samples it.
float ajfx_curve(uint o, float t) {
    uint count = ajfx_u32(o);
    uint next = 0u;
    while (next < count && ajfx_f(o + 1u + next * 2u) < t) {
        ++next;
    }
    if (next == 0u) {
        return ajfx_f(o + 2u);
    }
    if (next == count) {
        return ajfx_f(o + 2u + (count - 1u) * 2u);
    }
    uint a = o + 1u + (next - 1u) * 2u;
    uint b = a + 2u;
    float span = ajfx_f(b) - ajfx_f(a);
    float f = span > 0.0f ? (t - ajfx_f(a)) / span : 1.0f;
    return ajfx_mix(ajfx_f(a + 1u), ajfx_f(b + 1u), f);
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

// [shape, center.xyz, radius_min, radius_max, size.xyz, surface_only]
void ajfx_shape_location(inout Particle p, uint o, uint id) {
    uint shape = ajfx_u32(o);
    float3 center = ajfx_f3(o + 1u);
    float radius = ajfx_mix(ajfx_f(o + 4u), ajfx_f(o + 5u), ajfx_random(p, id, 5u));
    float3 size = ajfx_f3(o + 6u);
    bool surface = ajfx_u32(o + 9u) != 0u;
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

// [axis.xyz, t1.xyz, t2.xyz, cos_spread, speed_min, speed_max, cos_inner]
void ajfx_velocity(inout Particle p, uint o, uint id) {
    float3 axis = ajfx_f3(o);
    float3 t1 = ajfx_f3(o + 3u);
    float3 t2 = ajfx_f3(o + 6u);
    float cos_spread = ajfx_f(o + 9u);
    float cos_inner = ajfx_f(o + 12u);
    float cos_theta = cos_inner - ajfx_random(p, id, 0u) * (cos_inner - cos_spread);
    float sin_theta = sqrt(max(0.0f, 1.0f - cos_theta * cos_theta));
    float phi = ajfx_two_pi * ajfx_random(p, id, 1u);
    float3 direction = t1 * (sin_theta * cos(phi)) + t2 * (sin_theta * sin(phi)) + axis * cos_theta;
    p.velocity = direction * ajfx_mix(ajfx_f(o + 10u), ajfx_f(o + 11u), ajfx_random(p, id, 2u));
}

// [a.rgba, b.rgba, brightness_min, brightness_max]
void ajfx_color(inout Particle p, uint o, uint id) {
    float4 a = ajfx_f4(o);
    float4 b = ajfx_f4(o + 4u);
    float t = ajfx_random(p, id, 0u);
    float k = ajfx_mix(ajfx_f(o + 8u), ajfx_f(o + 9u), ajfx_random(p, id, 1u));
    p.base_color = float4(ajfx_mix(a.x, b.x, t) * k, ajfx_mix(a.y, b.y, t) * k, ajfx_mix(a.z, b.z, t) * k,
                          ajfx_mix(a.w, b.w, t));
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

// [speed_min, speed_max, radians per degree, both ways, 1 / (1 + drag * dt)]
void ajfx_spin(inout Particle p, uint o, uint id) {
    float speed = ajfx_mix(ajfx_f(o), ajfx_f(o + 1u), ajfx_random(p, id, 0u)) * ajfx_f(o + 2u);
    p.spin = ajfx_u32(o + 3u) != 0u && ajfx_random(p, id, 1u) < 0.5f ? -speed : speed;
}

// The update's turn by a spin module's data, where position is integrated.
void ajfx_spin_turn(inout Particle p, uint o) {
    p.spin = p.spin * ajfx_f(o + 4u);
    p.rotation += p.spin * ajfx_dt;
}

// ------------------------------------------------------------ forces

// A force's factor for this particle, from [scale_min, scale_max] at o.
float ajfx_force_scale(Particle p, uint o, uint id) {
    return ajfx_mix(ajfx_f(o), ajfx_f(o + 1u), ajfx_random(p, id, 0u));
}

// [acceleration * dt, scale_min, scale_max, curve]
void ajfx_gravity(inout Particle p, uint o, uint id) {
    p.velocity += ajfx_f3(o) * (ajfx_curve(o + 5u, ajfx_normalized_age(p)) * ajfx_force_scale(p, o + 3u, id));
}

// [1 / (1 + coefficient * dt), coefficient * dt, varied, scale_min, scale_max]
void ajfx_drag(inout Particle p, uint o, uint id) {
    float keep = ajfx_u32(o + 2u) != 0u ? 1.0f / (1.0f + ajfx_f(o + 1u) * ajfx_force_scale(p, o + 3u, id)) : ajfx_f(o);
    p.velocity = p.velocity * keep;
}

// [frequency, strength * dt, scroll, seed, drift * time.xyz, scale_min, scale_max, curve]
void ajfx_curl_noise(inout Particle p, uint o, uint id) {
    float3 q = (p.position - ajfx_f3(o + 4u)) * ajfx_f(o) + float3(0.0f, 0.0f, ajfx_f(o + 2u));
    p.velocity += ajfx_curl_field(q, ajfx_u32(o + 3u)) *
                  (ajfx_f(o + 1u) * ajfx_curve(o + 9u, ajfx_normalized_age(p)) * ajfx_force_scale(p, o + 7u, id));
}

// [axis.xyz, center.xyz, strength * dt, scale_min, scale_max]
void ajfx_vortex(inout Particle p, uint o, uint id) {
    float3 axis = ajfx_f3(o);
    float3 r = p.position - ajfx_f3(o + 3u);
    float3 radial = r - axis * ajfx_dot(r, axis);
    float len = sqrt(ajfx_dot(radial, radial));
    if (len > 1.0e-6f) {
        p.velocity += ajfx_cross(axis, radial) * (ajfx_f(o + 6u) * ajfx_force_scale(p, o + 7u, id) / len);
    }
}

// [position.xyz, strength, radius, dt, scale_min, scale_max]
void ajfx_attractor(inout Particle p, uint o, uint id) {
    float3 d = ajfx_f3(o) - p.position;
    float dist = sqrt(ajfx_dot(d, d));
    if (dist <= 1.0e-6f) {
        return;
    }
    float radius = ajfx_f(o + 4u);
    float falloff = radius > 0.0f ? max(0.0f, 1.0f - dist / radius) : 1.0f;
    p.velocity += d * (ajfx_f(o + 3u) * ajfx_force_scale(p, o + 6u, id) * falloff * ajfx_f(o + 5u) / dist);
}

// [axis.xyz, center.xyz, strength * dt, height, scale_min, scale_max]
void ajfx_converge(inout Particle p, uint o, uint id) {
    float3 axis = ajfx_f3(o);
    float3 r = p.position - ajfx_f3(o + 3u);
    float along = ajfx_dot(r, axis);
    float3 radial = r - axis * along;
    float height = ajfx_f(o + 7u);
    float falloff = height > 0.0f ? min(max(1.0f - along / height, 0.0f), 1.0f) : 1.0f;
    p.velocity = p.velocity - radial * (ajfx_f(o + 6u) * ajfx_force_scale(p, o + 8u, id) * falloff);
}

// [fires this step, center.xyz, power, radius, scale_min, scale_max]. Out
// from the blast's centre, fading linearly to nothing at its radius, as
// blast_push (src/effect.cpp) works it out.
void ajfx_shockwave(inout Particle p, uint o, uint id) {
    if (ajfx_u32(o) == 0u) {
        return;
    }
    float3 d = p.position - ajfx_f3(o + 1u);
    float dist = sqrt(ajfx_dot(d, d));
    float radius = ajfx_f(o + 5u);
    if (!(dist < radius)) {
        return;
    }
    float3 out_dir = dist > 1.0e-6f ? d * (1.0f / dist) : float3(0.0f, 1.0f, 0.0f);
    float speed = ajfx_f(o + 4u) * (1.0f - dist / radius);
    p.velocity += out_dir * (speed * ajfx_force_scale(p, o + 6u, id));
}

// ------------------------------------------------------------ constraints and appearance

#ifdef AJFX_RIGID_DATA
// An emitter with a RigidBody defines AJFX_RIGID_DATA as that module's data
// offset, and its particles meet colliders as balls, as in the reference.
void ajfx_respond_rigid(inout Particle p, float dist, float3 n, float restitution, float slide, bool kill) {
    float round = ajfx_f(AJFX_RIGID_DATA + 4u);
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
    float push = -into * (1.0f + bounce);
    p.velocity = p.velocity + n * push;
    p.angular_velocity = p.angular_velocity * round;
    float3 arm = n * -radius;
    float3 contact = p.velocity + ajfx_cross(p.angular_velocity, arm);
    float3 slip = contact - n * ajfx_dot(contact, n);
    float turn = 2.5f * round;
    float share = 1.0f / (1.0f + turn);
    float speed = sqrt(ajfx_dot(slip, slip));
    float grip = push * (1.0f - slide);
    if (share * speed * slide > grip) {
        share = grip / (speed * slide);
    }
    p.velocity = p.velocity - slip * share;
    if (radius > 1.0e-6f) {
        p.angular_velocity = p.angular_velocity - ajfx_cross(arm, slip) * (share * turn / (radius * radius));
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

// `v` turned by the unit quaternion `q`, in the order rotate (math.hpp)
// multiplies it, so the GPU and the reference agree to the bit.
float3 ajfx_turn(float4 q, float3 v) {
    float3 u = float3(q.x, q.y, q.z);
    float3 t = ajfx_cross(u, v) * 2.0f;
    return v + t * q.w + ajfx_cross(u, t);
}

// The update kernel's distance fields share u7 with the bookkeeping
// kernel's indirect arguments, which the update kernel never touches. So do
// the bodies its BodyCollision and Wake modules meet, after the fields.
#define ajfx_fields ajfx_args

float ajfx_wf(uint o) { return asfloat(ajfx_fields.Load(o * 4u)); }
float3 ajfx_wf3(uint o) { return float3(ajfx_wf(o), ajfx_wf(o + 1u), ajfx_wf(o + 2u)); }
uint ajfx_wu(uint o) { return ajfx_fields.Load(o * 4u); }

// ------------------------------------------------------------ bodies
//
// Another emitter's bodies, sorted into a grid each step (src/body_grid.hpp
// says how and why the result is that of meeting every body in turn). A
// module's grid data, packed by pack_grid, is [records at, order at, starts
// at, count, usable, origin.xyz, 1 / cell, nx, ny, nz, bound]; each body's
// record in the fields buffer is [position.xyz, orientation.xyzw, half size,
// box, velocity.xyz, spawn index, kind].

static const uint ajfx_no_body = 0xffffffffu;
static const uint ajfx_body_floats = 14u;

struct AjfxCellSpan {
    uint3 lo;
    uint3 hi;
    bool none;
};

// The cells a body whose surface comes within `reach` of `pos` can be in, or
// false to look at every body instead. As cell_span does it.
bool ajfx_cell_span(uint g, float3 pos, float reach, out AjfxCellSpan span) {
    span.lo = uint3(0u, 0u, 0u);
    span.hi = uint3(0u, 0u, 0u);
    span.none = false;
    if (ajfx_u32(g + 4u) == 0u) {
        return false;
    }
    float inv = ajfx_f(g + 8u);
    float k = ceil(reach * 1.01f * inv);
    if (!(k <= 3.0f)) {
        return false;
    }
    float3 origin = ajfx_f3(g + 5u);
    float3 at = float3(floor((pos.x - origin.x) * inv), floor((pos.y - origin.y) * inv),
                       floor((pos.z - origin.z) * inv));
    if (isnan(at.x) || isnan(at.y) || isnan(at.z)) {
        return false;
    }
    uint3 dims = uint3(ajfx_u32(g + 9u), ajfx_u32(g + 10u), ajfx_u32(g + 11u));
    float3 top = float3((float)(dims.x - 1u), (float)(dims.y - 1u), (float)(dims.z - 1u));
    float3 first = at - float3(k, k, k);
    float3 last = at + float3(k, k, k);
    if (last.x < 0.0f || last.y < 0.0f || last.z < 0.0f || first.x > top.x || first.y > top.y || first.z > top.z) {
        span.none = true;
        return true;
    }
    span.lo = uint3(first.x <= 0.0f ? 0u : (uint)first.x, first.y <= 0.0f ? 0u : (uint)first.y,
                    first.z <= 0.0f ? 0u : (uint)first.z);
    span.hi = uint3(last.x >= top.x ? dims.x - 1u : (uint)last.x, last.y >= top.y ? dims.y - 1u : (uint)last.y,
                    last.z >= top.z ? dims.z - 1u : (uint)last.z);
    return true;
}

// How far from its middle a body's surface may be: sqrt(3) half sizes for a
// box, as body_bound works it out.
float ajfx_body_bound(uint at) {
    float half_size = ajfx_wf(at + 7u);
    return ajfx_wu(at + 8u) != 0u ? half_size * 1.7320508f : half_size;
}

// The first body at or after `from`, in body order, among the span's cells,
// whose bounding ball grown by `extra` reaches `pos`. As next_body does it.
uint ajfx_next_body(uint g, AjfxCellSpan span, float3 pos, float extra, uint from) {
    uint best = ajfx_no_body;
    if (span.none) {
        return best;
    }
    uint records = ajfx_u32(g);
    uint order = ajfx_u32(g + 1u);
    uint starts = ajfx_u32(g + 2u);
    uint nx = ajfx_u32(g + 9u);
    uint ny = ajfx_u32(g + 10u);
    for (uint z = span.lo.z; z <= span.hi.z; ++z) {
        for (uint y = span.lo.y; y <= span.hi.y; ++y) {
            for (uint x = span.lo.x; x <= span.hi.x; ++x) {
                uint cell = (z * ny + y) * nx + x;
                uint end = ajfx_wu(starts + cell + 1u);
                for (uint j = ajfx_wu(starts + cell); j < end; ++j) {
                    uint b = ajfx_wu(order + j);
                    if (b >= best) {
                        break;
                    }
                    if (b < from) {
                        continue;
                    }
                    uint at = records + b * ajfx_body_floats;
                    float3 d = pos - ajfx_wf3(at);
                    float r = (ajfx_body_bound(at) + extra) * 1.01f;
                    if (ajfx_dot(d, d) <= r * r) {
                        best = b;
                        break;
                    }
                }
            }
        }
    }
    return best;
}

// The particle kept out of the one body whose record is at `at`, as
// meet_body (src/simulation.cpp) does it.
void ajfx_meet_body(inout Particle p, uint at, float restitution, float slide, bool kill) {
    float3 centre = ajfx_wf3(at);
    float4 q = float4(ajfx_wf(at + 3u), ajfx_wf(at + 4u), ajfx_wf(at + 5u), ajfx_wf(at + 6u));
    float half_size = ajfx_wf(at + 7u);
    bool box = ajfx_wu(at + 8u) != 0u;
    float3 d = p.position - centre;
    if (!box) {
        float len = sqrt(ajfx_dot(d, d));
        float3 out_dir = len > 1.0e-6f ? d * (1.0f / len) : float3(0.0f, 1.0f, 0.0f);
        ajfx_respond(p, len - half_size, out_dir, restitution, slide, kill);
        return;
    }
    // The box is axis aligned in the body's own frame: turn the offset back
    // by the conjugate, decide there, and turn the way out forward.
    float4 back = float4(-q.x, -q.y, -q.z, q.w);
    float3 local = ajfx_turn(back, d);
    float3 over = abs(local) - float3(half_size, half_size, half_size);
    if (over.x >= 0.0f || over.y >= 0.0f || over.z >= 0.0f) {
        float3 clamped = clamp(local, -half_size, half_size);
        float3 away = local - clamped;
        float len = sqrt(ajfx_dot(away, away));
        if (len > 1.0e-6f) {
            ajfx_respond(p, len, ajfx_turn(q, away * (1.0f / len)), restitution, slide, kill);
        }
        return;
    }
    // Inside: out through the nearest face, x before y before z on a tie, as
    // the box collider does.
    float3 out_dir = float3(local.x < 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f);
    float dist = over.x;
    if (over.y > over.x && over.y >= over.z) {
        out_dir = float3(0.0f, local.y < 0.0f ? -1.0f : 1.0f, 0.0f);
        dist = over.y;
    } else if (over.z > over.x && over.z > over.y) {
        out_dir = float3(0.0f, 0.0f, local.z < 0.0f ? -1.0f : 1.0f);
        dist = over.z;
    }
    ajfx_respond(p, dist, ajfx_turn(q, out_dir), restitution, slide, kill);
}

// [restitution, 1 - friction, kill, grid]. Each body the particle may be
// touching is met in body order, the search starting again from wherever
// the last one left it, so a particle pushed out of one is where the next
// finds it: meeting every body in turn, as the reference does it
// (meet_bodies).
void ajfx_body_collision(inout Particle p, uint o, uint id) {
    float restitution = ajfx_f(o);
    float slide = ajfx_f(o + 1u);
    bool kill = ajfx_u32(o + 2u) != 0u;
    uint g = o + 3u;
    uint records = ajfx_u32(g);
    uint count = ajfx_u32(g + 3u);
    float extra = 0.0f;
#ifdef AJFX_RIGID_DATA
    // A rigid particle reaches out by its contact radius.
    extra = max(p.size * ajfx_f(AJFX_RIGID_DATA + 2u), 0.0f);
#endif
    uint from = 0u;
    for (uint guard = 0u; guard < count && from < count; ++guard) {
        AjfxCellSpan span;
        if (!ajfx_cell_span(g, p.position, ajfx_f(g + 12u) + extra, span)) {
            for (uint rest = from; rest < count; ++rest) {
                ajfx_meet_body(p, records + rest * ajfx_body_floats, restitution, slide, kill);
            }
            return;
        }
        uint b = ajfx_next_body(g, span, p.position, extra, from);
        if (b == ajfx_no_body) {
            return;
        }
        ajfx_meet_body(p, records + b * ajfx_body_floats, restitution, slide, kill);
        from = b + 1u;
    }
}

// Whether a body is in a wake's bodies scope: [every source, 8 words of
// source bits, every kind, 8 words of kind bits], as pack_body_scope packs
// it and in_scope decides it.
bool ajfx_body_in_scope(uint o, uint spawn_index, uint kind) {
    uint source = spawn_index >> 24u;
    bool sources = ajfx_u32(o) != 0u || ((ajfx_u32(o + 1u + (source >> 5u)) >> (source & 31u)) & 1u) != 0u;
    bool kinds = ajfx_u32(o + 9u) != 0u ||
                 (kind < 256u && ((ajfx_u32(o + 10u + (kind >> 5u)) >> (kind & 31u)) & 1u) != 0u);
    return sources && kinds;
}

// Whether body `b`'s wake at `pos` is stronger than the best so far (the
// earlier body on a tie), and if so, it becomes the best.
void ajfx_wake_consider(uint o, uint records, uint b, float3 pos, inout uint best, inout float best_weight) {
    uint at = records + b * ajfx_body_floats;
    float3 v = ajfx_wf3(at + 9u);
    if (ajfx_dot(v, v) < ajfx_f(o + 2u)) {
        return;
    }
    if (!ajfx_body_in_scope(o + 6u, ajfx_wu(at + 12u), ajfx_wu(at + 13u))) {
        return;
    }
    float3 d = pos - ajfx_wf3(at);
    float dist = sqrt(ajfx_dot(d, d));
    float r = ajfx_f(o + 1u) * ajfx_wf(at + 7u);
    if (!(dist < r)) {
        return;
    }
    float w = 1.0f - dist / r;
    if (w > best_weight || (w == best_weight && b < best)) {
        best = b;
        best_weight = w;
    }
}

// [strength * dt, radius, min_speed squared, scale_min, scale_max, reach,
// bodies scope (18), grid]. The particle takes k / (1 + k) of the way to the
// velocity of the strongest wake at it, as the reference's Wake does.
void ajfx_wake(inout Particle p, uint o, uint id) {
    uint g = o + 24u;
    uint records = ajfx_u32(g);
    uint count = ajfx_u32(g + 3u);
    if (count == 0u) {
        return;
    }
    uint best = ajfx_no_body;
    float best_weight = 0.0f;
    AjfxCellSpan span;
    if (!ajfx_cell_span(g, p.position, ajfx_f(o + 5u), span)) {
        for (uint b = 0u; b < count; ++b) {
            ajfx_wake_consider(o, records, b, p.position, best, best_weight);
        }
    } else if (!span.none) {
        uint order = ajfx_u32(g + 1u);
        uint starts = ajfx_u32(g + 2u);
        uint nx = ajfx_u32(g + 9u);
        uint ny = ajfx_u32(g + 10u);
        for (uint z = span.lo.z; z <= span.hi.z; ++z) {
            for (uint y = span.lo.y; y <= span.hi.y; ++y) {
                for (uint x = span.lo.x; x <= span.hi.x; ++x) {
                    uint cell = (z * ny + y) * nx + x;
                    uint end = ajfx_wu(starts + cell + 1u);
                    for (uint j = ajfx_wu(starts + cell); j < end; ++j) {
                        ajfx_wake_consider(o, records, ajfx_wu(order + j), p.position, best, best_weight);
                    }
                }
            }
        }
    }
    if (best == ajfx_no_body) {
        return;
    }
    float k = ajfx_f(o) * ajfx_force_scale(p, o + 3u, id) * best_weight;
    float t = k / (1.0f + k);
    float3 vb = ajfx_wf3(records + best * ajfx_body_floats + 9u);
    p.velocity = p.velocity + (vb - p.velocity) * t;
}

// ------------------------------------------------------------ grid volumes
//
// The grid volumes (Volume) live in one buffer the update kernel is handed at
// u4 (the bookkeeping kernels' offsets, which the update kernel never
// touches). A module's grid data, packed by pack_volume, is [cells at,
// deposits at, nx, ny, nz, origin.xyz, 1 / cell]; a grid of no cells is one
// the step has no buffer for.

#define ajfx_volumes ajfx_offsets
#include "ajfx_volume.hlsli"

// [grid (9), strength * dt, scale_min, scale_max]. The particle takes at
// most all of the way to the velocity the grid holds at it, as the
// reference's VolumeAdvect does; nothing outside the grid's box.
void ajfx_volume_advect(inout Particle p, uint o, uint id) {
    uint nx = ajfx_u32(o + 2u);
    uint ny = ajfx_u32(o + 3u);
    uint nz = ajfx_u32(o + 4u);
    if (nx == 0u) {
        return;
    }
    float3 origin = ajfx_f3(o + 5u);
    float inv = ajfx_f(o + 8u);
    float ux = (p.position.x - origin.x) * inv;
    float uy = (p.position.y - origin.y) * inv;
    float uz = (p.position.z - origin.z) * inv;
    if (!(ux >= 0.0f && uy >= 0.0f && uz >= 0.0f && ux <= (float)nx && uy <= (float)ny && uz <= (float)nz)) {
        return;
    }
    AjfxVolumeTaps taps = ajfx_volume_taps(nx, ny, nz, float3(ux - 0.5f, uy - 0.5f, uz - 0.5f));
    uint cells = nx * ny * nz;
    float3 carried = ajfx_volume_sample_velocity(ajfx_u32(o) + 2u * cells, nx, ny, taps);
    float t = min(ajfx_f(o + 9u) * ajfx_force_scale(p, o + 10u, id), 1.0f);
    p.velocity = p.velocity + (carried - p.velocity) * t;
}

// [grid (9), density * dt, temperature * dt, velocity * dt, curve]. The
// particle's deposits go into the sums of the cell it is in, quantised and
// added as integers, as the reference's VolumeSource makes them.
void ajfx_volume_source(inout Particle p, uint o, uint id) {
    uint nx = ajfx_u32(o + 2u);
    uint ny = ajfx_u32(o + 3u);
    uint nz = ajfx_u32(o + 4u);
    if (nx == 0u) {
        return;
    }
    float3 origin = ajfx_f3(o + 5u);
    float inv = ajfx_f(o + 8u);
    float ux = (p.position.x - origin.x) * inv;
    float uy = (p.position.y - origin.y) * inv;
    float uz = (p.position.z - origin.z) * inv;
    if (!(ux >= 0.0f && uy >= 0.0f && uz >= 0.0f && ux < (float)nx && uy < (float)ny && uz < (float)nz)) {
        return;
    }
    uint cell = ajfx_volume_at(nx, ny, (uint)floor(ux), (uint)floor(uy), (uint)floor(uz));
    float over_life = ajfx_curve(o + 12u, ajfx_normalized_age(p));
    uint sums = ajfx_u32(o + 1u) + cell * 10u;
    float k = ajfx_f(o + 11u) * over_life;
    ajfx_volume_add(sums, ajfx_f(o + 9u) * over_life);
    ajfx_volume_add(sums + 2u, ajfx_f(o + 10u) * over_life);
    ajfx_volume_add(sums + 4u, p.velocity.x * k);
    ajfx_volume_add(sums + 6u, p.velocity.y * k);
    ajfx_volume_add(sums + 8u, p.velocity.z * k);
}

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

// [center.xyz, half_size.xyz, turn.xyzw, restitution, 1 - friction, inside, kill]
// Worked in the box's own frame, as the reference does.
void ajfx_box_collision(inout Particle p, uint o, uint id) {
    float4 turn = ajfx_f4(o + 6u);
    float4 back = float4(-turn.x, -turn.y, -turn.z, turn.w);
    float3 d = ajfx_turn(back, p.position - ajfx_f3(o));
    float3 h = ajfx_f3(o + 3u);
    float3 s = float3(d.x < 0.0f ? -1.0f : 1.0f, d.y < 0.0f ? -1.0f : 1.0f, d.z < 0.0f ? -1.0f : 1.0f);
    float3 q = float3(abs(d.x) - h.x, abs(d.y) - h.y, abs(d.z) - h.z);
    float3 over = float3(max(q.x, 0.0f), max(q.y, 0.0f), max(q.z, 0.0f));
    float outside = sqrt(ajfx_dot(over, over));
    float restitution = ajfx_f(o + 10u);
    float slide = ajfx_f(o + 11u);
    bool kill = ajfx_u32(o + 13u) != 0u;
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
    n = ajfx_turn(turn, n);
    if (ajfx_u32(o + 12u) != 0u) {
        ajfx_respond(p, -dist, n * -1.0f, restitution, slide, kill);
    } else {
        ajfx_respond(p, dist, n, restitution, slide, kill);
    }
}

// [1 / (1 + angular_drag * dt), dt / 2, contact_scale, rest_speed, roundness]
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
    p.size = p.base_size * ajfx_curve(o, ajfx_normalized_age(p));
}

// [start, 1 / half_life, glow]. The heat at this age, start * 2^(-age /
// half_life), through a black-body-like ramp, added to the colour, as
// detail::heat_glow (src/curves.hpp) works it out. Called after the colour
// is worked out afresh from the base and every colour over life.
void ajfx_heat(inout Particle p, uint o, uint id) {
    float heat = ajfx_f(o) * exp2(-(p.age * ajfx_f(o + 1u)));
    float t = min(max(heat, 0.0f), 1.0f);
    float glow = ajfx_f(o + 2u);
    p.color = float4(p.color.x + min(max(3.0f * t, 0.0f), 1.0f) * glow,
                     p.color.y + min(max(3.0f * t - 1.0f, 0.0f), 1.0f) * glow,
                     p.color.z + min(max(3.0f * t - 2.0f, 0.0f), 1.0f) * glow, p.color.w);
}

#endif
