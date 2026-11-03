// One grid volume's step (src/volume.cpp step_volume), a dispatch of one
// thread per cell for each pass: inject the step's deposits with buoyancy,
// the curl and its size, the confinement push, semi-Lagrangian advection
// from one buffer into the other, the divergence (and a zero pressure), one
// Jacobi iteration, and the pressure gradient taken off with dissipation.
// Each pass reads buffers no thread of it writes, so the result does not
// depend on scheduling, and every operation is the reference's in its order.

cbuffer AjfxVolume : register(b0, space1) {
    uint vol_nx;
    uint vol_ny;
    uint vol_nz;
    uint vol_cells;
    float vol_buoyancy_dt;
    float vol_weight_dt;
    float vol_confinement;   // vorticity * cell * dt
    float vol_inv_two_cells;
    float vol_cell_squared;
    float vol_advect_scale;  // dt / cell
    float vol_keep_density;
    float vol_keep_temperature;
    float vol_keep_velocity;
    uint vol_base;           // the volume's region in the buffer, in words (volume_region_words)
    uint vol_current;        // which of its two cell buffers holds the grid as the step began
    uint vol_parity;         // jacobi: which pressure buffer it reads; project: which it reads
};

RWByteAddressBuffer ajfx_volumes : register(u0);

#include "ajfx_volume.hlsli"

// The region (gpu/simulation.cpp volume_region_words): two cell buffers of
// five floats a cell (density, temperature, velocity), the deposits (ten
// words a cell), the curl (three floats), its size, the divergence and two
// pressures.
uint vol_grid(uint which) { return vol_base + which * 5u * vol_cells; }
uint vol_density(uint which) { return vol_grid(which); }
uint vol_temperature(uint which) { return vol_grid(which) + vol_cells; }
uint vol_velocity(uint which) { return vol_grid(which) + 2u * vol_cells; }
uint vol_deposits() { return vol_base + 10u * vol_cells; }
uint vol_curl() { return vol_base + 20u * vol_cells; }
uint vol_magnitude() { return vol_base + 23u * vol_cells; }
uint vol_divergence() { return vol_base + 24u * vol_cells; }
uint vol_pressure(uint which) { return vol_base + (25u + which) * vol_cells; }

void vol_store(uint o, float v) { ajfx_volumes.Store(o * 4u, asuint(v)); }
void vol_store3(uint o, float3 v) { ajfx_volumes.Store3(o * 4u, asuint(v)); }

uint3 vol_xyz(uint c) {
    uint x = c % vol_nx;
    uint y = (c / vol_nx) % vol_ny;
    uint z = c / (vol_nx * vol_ny);
    return uint3(x, y, z);
}

// The velocity `d` cells along one axis, or still outside the grid.
float3 vol_velocity_or_still(uint which, int x, int y, int z) {
    if (x < 0 || y < 0 || z < 0 || x >= (int)vol_nx || y >= (int)vol_ny || z >= (int)vol_nz) {
        return float3(0.0f, 0.0f, 0.0f);
    }
    return ajfx_vf3(vol_velocity(which) + 3u * ajfx_volume_at(vol_nx, vol_ny, (uint)x, (uint)y, (uint)z));
}

// A scalar of the cell along one axis, clamped to the grid's edge.
float vol_scalar_clamped(uint field, int x, int y, int z) {
    uint cx = (uint)clamp(x, 0, (int)vol_nx - 1);
    uint cy = (uint)clamp(y, 0, (int)vol_ny - 1);
    uint cz = (uint)clamp(z, 0, (int)vol_nz - 1);
    return ajfx_vf(field + ajfx_volume_at(vol_nx, vol_ny, cx, cy, cz));
}

[numthreads(256, 1, 1)]
void volume_inject(uint3 id : SV_DispatchThreadID) {
    uint c = id.x;
    if (c >= vol_cells) {
        return;
    }
    uint sums = vol_deposits() + c * 10u;
    float d = ajfx_vf(vol_density(vol_current) + c) + ajfx_volume_decode(sums) * ajfx_volume_quantum;
    float t = ajfx_vf(vol_temperature(vol_current) + c) + ajfx_volume_decode(sums + 2u) * ajfx_volume_quantum;
    float3 v = ajfx_vf3(vol_velocity(vol_current) + 3u * c);
    v.x = v.x + ajfx_volume_decode(sums + 4u) * ajfx_volume_quantum;
    v.y = v.y + ajfx_volume_decode(sums + 6u) * ajfx_volume_quantum;
    v.z = v.z + ajfx_volume_decode(sums + 8u) * ajfx_volume_quantum;
    v.y = v.y + (t * vol_buoyancy_dt - d * vol_weight_dt);
    vol_store(vol_density(vol_current) + c, d);
    vol_store(vol_temperature(vol_current) + c, t);
    vol_store3(vol_velocity(vol_current) + 3u * c, v);
    // The deposits are taken: the next step's start from nothing.
    [unroll] for (uint k = 0u; k < 10u; ++k) {
        ajfx_volumes.Store((sums + k) * 4u, 0u);
    }
}

[numthreads(256, 1, 1)]
void volume_curl(uint3 id : SV_DispatchThreadID) {
    uint c = id.x;
    if (c >= vol_cells) {
        return;
    }
    int3 p = (int3)vol_xyz(c);
    float3 xp = vol_velocity_or_still(vol_current, p.x + 1, p.y, p.z);
    float3 xm = vol_velocity_or_still(vol_current, p.x - 1, p.y, p.z);
    float3 yp = vol_velocity_or_still(vol_current, p.x, p.y + 1, p.z);
    float3 ym = vol_velocity_or_still(vol_current, p.x, p.y - 1, p.z);
    float3 zp = vol_velocity_or_still(vol_current, p.x, p.y, p.z + 1);
    float3 zm = vol_velocity_or_still(vol_current, p.x, p.y, p.z - 1);
    float3 curl = float3((yp.z - ym.z) * vol_inv_two_cells - (zp.y - zm.y) * vol_inv_two_cells,
                         (zp.x - zm.x) * vol_inv_two_cells - (xp.z - xm.z) * vol_inv_two_cells,
                         (xp.y - xm.y) * vol_inv_two_cells - (yp.x - ym.x) * vol_inv_two_cells);
    vol_store3(vol_curl() + 3u * c, curl);
    vol_store(vol_magnitude() + c, sqrt(curl.x * curl.x + curl.y * curl.y + curl.z * curl.z));
}

[numthreads(256, 1, 1)]
void volume_confine(uint3 id : SV_DispatchThreadID) {
    uint c = id.x;
    if (c >= vol_cells) {
        return;
    }
    int3 p = (int3)vol_xyz(c);
    uint m = vol_magnitude();
    float3 grow = float3((vol_scalar_clamped(m, p.x + 1, p.y, p.z) - vol_scalar_clamped(m, p.x - 1, p.y, p.z)) *
                             vol_inv_two_cells,
                         (vol_scalar_clamped(m, p.x, p.y + 1, p.z) - vol_scalar_clamped(m, p.x, p.y - 1, p.z)) *
                             vol_inv_two_cells,
                         (vol_scalar_clamped(m, p.x, p.y, p.z + 1) - vol_scalar_clamped(m, p.x, p.y, p.z - 1)) *
                             vol_inv_two_cells);
    float len = sqrt(grow.x * grow.x + grow.y * grow.y + grow.z * grow.z);
    if (len > 1.0e-6f) {
        float3 towards = grow * (1.0f / len);
        float3 w = ajfx_vf3(vol_curl() + 3u * c);
        float3 push = float3(towards.y * w.z - towards.z * w.y, towards.z * w.x - towards.x * w.z,
                             towards.x * w.y - towards.y * w.x);
        uint at = vol_velocity(vol_current) + 3u * c;
        vol_store3(at, ajfx_vf3(at) + push * vol_confinement);
    }
}

[numthreads(256, 1, 1)]
void volume_advect(uint3 id : SV_DispatchThreadID) {
    uint c = id.x;
    if (c >= vol_cells) {
        return;
    }
    uint3 p = vol_xyz(c);
    uint from = vol_current;
    uint to = 1u - vol_current;
    float3 v = ajfx_vf3(vol_velocity(from) + 3u * c);
    AjfxVolumeTaps t = ajfx_volume_taps(vol_nx, vol_ny, vol_nz,
                                        float3((float)p.x - v.x * vol_advect_scale, (float)p.y - v.y * vol_advect_scale,
                                               (float)p.z - v.z * vol_advect_scale));
    vol_store(vol_density(to) + c, ajfx_volume_sample_scalar(vol_density(from), vol_nx, vol_ny, t));
    vol_store(vol_temperature(to) + c, ajfx_volume_sample_scalar(vol_temperature(from), vol_nx, vol_ny, t));
    vol_store3(vol_velocity(to) + 3u * c, ajfx_volume_sample_velocity(vol_velocity(from), vol_nx, vol_ny, t));
}

// From here the grid is in the other buffer (1 - vol_current).
[numthreads(256, 1, 1)]
void volume_divergence(uint3 id : SV_DispatchThreadID) {
    uint c = id.x;
    if (c >= vol_cells) {
        return;
    }
    uint which = 1u - vol_current;
    int3 p = (int3)vol_xyz(c);
    float3 xp = vol_velocity_or_still(which, p.x + 1, p.y, p.z);
    float3 xm = vol_velocity_or_still(which, p.x - 1, p.y, p.z);
    float3 yp = vol_velocity_or_still(which, p.x, p.y + 1, p.z);
    float3 ym = vol_velocity_or_still(which, p.x, p.y - 1, p.z);
    float3 zp = vol_velocity_or_still(which, p.x, p.y, p.z + 1);
    float3 zm = vol_velocity_or_still(which, p.x, p.y, p.z - 1);
    vol_store(vol_divergence() + c, (((xp.x - xm.x) + (yp.y - ym.y)) + (zp.z - zm.z)) * vol_inv_two_cells);
    vol_store(vol_pressure(0u) + c, 0.0f);
}

[numthreads(256, 1, 1)]
void volume_jacobi(uint3 id : SV_DispatchThreadID) {
    uint c = id.x;
    if (c >= vol_cells) {
        return;
    }
    int3 p = (int3)vol_xyz(c);
    uint in_at = vol_pressure(vol_parity);
    float sum = ((((vol_scalar_clamped(in_at, p.x - 1, p.y, p.z) + vol_scalar_clamped(in_at, p.x + 1, p.y, p.z)) +
                   vol_scalar_clamped(in_at, p.x, p.y - 1, p.z)) +
                  vol_scalar_clamped(in_at, p.x, p.y + 1, p.z)) +
                 vol_scalar_clamped(in_at, p.x, p.y, p.z - 1)) +
                vol_scalar_clamped(in_at, p.x, p.y, p.z + 1);
    vol_store(vol_pressure(1u - vol_parity) + c, (sum - ajfx_vf(vol_divergence() + c) * vol_cell_squared) * (1.0f / 6.0f));
}

[numthreads(256, 1, 1)]
void volume_project(uint3 id : SV_DispatchThreadID) {
    uint c = id.x;
    if (c >= vol_cells) {
        return;
    }
    uint which = 1u - vol_current;
    int3 p = (int3)vol_xyz(c);
    uint pr = vol_pressure(vol_parity);
    uint at = vol_velocity(which) + 3u * c;
    float3 v = ajfx_vf3(at);
    v.x = v.x - (vol_scalar_clamped(pr, p.x + 1, p.y, p.z) - vol_scalar_clamped(pr, p.x - 1, p.y, p.z)) * vol_inv_two_cells;
    v.y = v.y - (vol_scalar_clamped(pr, p.x, p.y + 1, p.z) - vol_scalar_clamped(pr, p.x, p.y - 1, p.z)) * vol_inv_two_cells;
    v.z = v.z - (vol_scalar_clamped(pr, p.x, p.y, p.z + 1) - vol_scalar_clamped(pr, p.x, p.y, p.z - 1)) * vol_inv_two_cells;
    vol_store3(at, v * vol_keep_velocity);
    uint d = vol_density(which) + c;
    vol_store(d, ajfx_vf(d) * vol_keep_density);
    uint t = vol_temperature(which) + c;
    vol_store(t, ajfx_vf(t) * vol_keep_temperature);
}
