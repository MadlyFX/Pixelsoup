#ifndef AJFX_VOLUME_HLSLI
#define AJFX_VOLUME_HLSLI

// What particles and the grid step share of a grid volume (src/volume.cpp):
// a cell's place, trilinear sampling between cell centres, and the fixed
// point deposits are summed in. Whoever includes this defines ajfx_volumes
// as the RWByteAddressBuffer the grids live in. Every function here is the
// reference's, operation for operation.

static const float ajfx_volume_quantum = asfloat(0x35800000u); // 2^-20
static const float ajfx_volume_largest = 2147483520.0f;        // the largest float below 2^31

float ajfx_vf(uint o) { return asfloat(ajfx_volumes.Load(o * 4u)); }
float3 ajfx_vf3(uint o) { return asfloat(ajfx_volumes.Load3(o * 4u)); }
uint ajfx_vu(uint o) { return ajfx_volumes.Load(o * 4u); }

uint ajfx_volume_at(uint nx, uint ny, uint x, uint y, uint z) {
    return (z * ny + y) * nx + x;
}

// A deposit as the integer it is summed as (quantize_deposit).
int ajfx_volume_quantize(float amount) {
    float scaled = amount * 1048576.0f;
    scaled = min(max(scaled, -ajfx_volume_largest), ajfx_volume_largest);
    return (int)floor(scaled + 0.5f);
}

// Adds `amount` to the 64-bit sum at word `o` (low word, then high):
// integer adds, so the sum is the same whichever order the particles come
// in, with the carry taken from the low word's wrap.
void ajfx_volume_add(uint o, float amount) {
    int q = ajfx_volume_quantize(amount);
    if (q == 0) {
        return;
    }
    uint lo_add = asuint(q);
    uint hi_add = q < 0 ? 0xffffffffu : 0u;
    uint was;
    ajfx_volumes.InterlockedAdd(o * 4u, lo_add, was);
    uint carry = (was + lo_add) < was ? 1u : 0u;
    ajfx_volumes.InterlockedAdd((o + 1u) * 4u, hi_add + carry);
}

// A sum back as a float, before the quantum (decode_deposit).
float ajfx_volume_decode(uint o) {
    uint lo = ajfx_vu(o);
    int hi = asint(ajfx_vu(o + 1u));
    return ((float)hi * 4294967296.0f + (float)(lo >> 16u) * 65536.0f) + (float)(lo & 0xffffu);
}

// Where a point in grid units (the lattice of cell centres) samples from
// (volume.cpp taps): the lower cell each way and the fraction towards the
// next, clamped to the edge.
struct AjfxVolumeTaps {
    uint ix;
    uint iy;
    uint iz;
    float fx;
    float fy;
    float fz;
};

AjfxVolumeTaps ajfx_volume_taps(uint nx, uint ny, uint nz, float3 p) {
    float gx = min(max(p.x, 0.0f), (float)(nx - 1u));
    float gy = min(max(p.y, 0.0f), (float)(ny - 1u));
    float gz = min(max(p.z, 0.0f), (float)(nz - 1u));
    AjfxVolumeTaps t;
    t.ix = min((uint)floor(gx), nx - 2u);
    t.iy = min((uint)floor(gy), ny - 2u);
    t.iz = min((uint)floor(gz), nz - 2u);
    t.fx = gx - (float)t.ix;
    t.fy = gy - (float)t.iy;
    t.fz = gz - (float)t.iz;
    return t;
}

float ajfx_volume_trilinear(float c000, float c100, float c010, float c110, float c001, float c101, float c011,
                            float c111, AjfxVolumeTaps t) {
    float c00 = c000 + (c100 - c000) * t.fx;
    float c10 = c010 + (c110 - c010) * t.fx;
    float c01 = c001 + (c101 - c001) * t.fx;
    float c11 = c011 + (c111 - c011) * t.fx;
    float c0 = c00 + (c10 - c00) * t.fy;
    float c1 = c01 + (c11 - c01) * t.fy;
    return c0 + (c1 - c0) * t.fz;
}

// A scalar field at `field` (one float a cell), sampled at the taps.
float ajfx_volume_sample_scalar(uint field, uint nx, uint ny, AjfxVolumeTaps t) {
    uint c000 = field + ajfx_volume_at(nx, ny, t.ix, t.iy, t.iz);
    uint c010 = field + ajfx_volume_at(nx, ny, t.ix, t.iy + 1u, t.iz);
    uint c001 = field + ajfx_volume_at(nx, ny, t.ix, t.iy, t.iz + 1u);
    uint c011 = field + ajfx_volume_at(nx, ny, t.ix, t.iy + 1u, t.iz + 1u);
    return ajfx_volume_trilinear(ajfx_vf(c000), ajfx_vf(c000 + 1u), ajfx_vf(c010), ajfx_vf(c010 + 1u), ajfx_vf(c001),
                                 ajfx_vf(c001 + 1u), ajfx_vf(c011), ajfx_vf(c011 + 1u), t);
}

// The velocity field at `field` (three floats a cell), sampled at the taps.
float3 ajfx_volume_sample_velocity(uint field, uint nx, uint ny, AjfxVolumeTaps t) {
    float3 c000 = ajfx_vf3(field + 3u * ajfx_volume_at(nx, ny, t.ix, t.iy, t.iz));
    float3 c100 = ajfx_vf3(field + 3u * ajfx_volume_at(nx, ny, t.ix + 1u, t.iy, t.iz));
    float3 c010 = ajfx_vf3(field + 3u * ajfx_volume_at(nx, ny, t.ix, t.iy + 1u, t.iz));
    float3 c110 = ajfx_vf3(field + 3u * ajfx_volume_at(nx, ny, t.ix + 1u, t.iy + 1u, t.iz));
    float3 c001 = ajfx_vf3(field + 3u * ajfx_volume_at(nx, ny, t.ix, t.iy, t.iz + 1u));
    float3 c101 = ajfx_vf3(field + 3u * ajfx_volume_at(nx, ny, t.ix + 1u, t.iy, t.iz + 1u));
    float3 c011 = ajfx_vf3(field + 3u * ajfx_volume_at(nx, ny, t.ix, t.iy + 1u, t.iz + 1u));
    float3 c111 = ajfx_vf3(field + 3u * ajfx_volume_at(nx, ny, t.ix + 1u, t.iy + 1u, t.iz + 1u));
    return float3(ajfx_volume_trilinear(c000.x, c100.x, c010.x, c110.x, c001.x, c101.x, c011.x, c111.x, t),
                  ajfx_volume_trilinear(c000.y, c100.y, c010.y, c110.y, c001.y, c101.y, c011.y, c111.y, t),
                  ajfx_volume_trilinear(c000.z, c100.z, c010.z, c110.z, c001.z, c101.z, c011.z, c111.z, t));
}

#endif
