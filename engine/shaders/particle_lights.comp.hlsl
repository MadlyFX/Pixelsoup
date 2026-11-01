// ajfx particle lights: one emitter's light records for the scene's lit pass,
// written into reserved slots of the particle light buffer.
//
// A particle carries a light for its whole life or not at all: those whose
// spawn index is a multiple of `spacing` do (ajfx::light_spacing), and the
// carrier with spawn index s = q * spacing owns slot q mod light_count. Live
// particles are kept in birth order, so slot k finds its newest live carrier
// by binary search on the spawn index; integers throughout, so every node
// picks the same ones. A light's intensity and reach are picked for its
// particle, and its colour and intensity follow the particle's life. A slot
// with no carrier gets a light of zero intensity, which the lit pass skips.
// Each thread writes only its own slot.

cbuffer ParticleLightParameters : register(b0, space1) {
    // Effect to world, four columns.
    float4 world0;
    float4 world1;
    float4 world2;
    float4 world3;
    uint stride;
    uint position_offset;
    uint color_offset; // 0xffffffff: white
    uint light_count;
    uint first_slot;
    uint spacing;
    uint age_offset;
    uint lifetime_offset;
    uint spawn_offset;
    uint salt;           // tells emitters' picks apart
    uint color_keys;     // 0: the particle's own colour
    uint curve_keys;
    float intensity_min;
    float intensity_max;
    float range_min;
    float range_max;
    float4 color_times[2];  // eight key times, four a vector
    float4 color_values[8];
    float4 curve_times[2];
    float4 curve_values[2];
};

ByteAddressBuffer particles : register(t0);
ByteAddressBuffer counters : register(t1); // the live count first
RWByteAddressBuffer lights : register(u0);   // light.hlsli Light records, 128 bytes each

static const uint absent = 0xffffffffU;
static const uint light_point = 1U;
// Carriers tried per slot, newest first, when a newer one has died.
static const uint tries = 4U;

uint mix(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// A number in [0, 1) for the particle born `spawn`, one per stream.
float pick(uint spawn, uint stream) {
    return float(mix(mix(spawn ^ salt) + stream) >> 8) * (1.0F / 16777216.0F);
}

uint spawn_of(uint record) {
    return particles.Load(record * stride + spawn_offset);
}

float key_time(float4 times[2], uint i) {
    return times[i >> 2][i & 3U];
}

float4 colour_at(float t) {
    float4 value = color_values[0];
    [loop] for (uint i = 1U; i < color_keys; ++i) {
        const float t0 = key_time(color_times, i - 1U);
        const float t1 = key_time(color_times, i);
        if (t >= t0) {
            value = t1 > t0 ? lerp(color_values[i - 1U], color_values[i], saturate((t - t0) / (t1 - t0)))
                            : color_values[i];
        }
    }
    return value;
}

float curve_at(float t) {
    float value = curve_values[0][0];
    [loop] for (uint i = 1U; i < curve_keys; ++i) {
        const float t0 = key_time(curve_times, i - 1U);
        const float t1 = key_time(curve_times, i);
        const float v0 = curve_values[(i - 1U) >> 2][(i - 1U) & 3U];
        const float v1 = curve_values[i >> 2][i & 3U];
        if (t >= t0) {
            value = t1 > t0 ? lerp(v0, v1, saturate((t - t0) / (t1 - t0))) : v1;
        }
    }
    return value;
}

// The record holding spawn index `wanted` among the first `alive`, or absent.
uint find(uint wanted, uint alive) {
    uint low = 0U;
    uint high = alive;
    [loop] while (low < high) {
        const uint middle = low + (high - low) / 2U;
        const uint at = spawn_of(middle);
        if (at == wanted) {
            return middle;
        }
        if (at < wanted) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    return absent;
}

[numthreads(64, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    const uint k = thread.x;
    if (k >= light_count) {
        return;
    }
    const uint alive = counters.Load(0U);
    const uint at = (first_slot + k) * 128U;
    float3 position = float3(0.0F, 0.0F, 0.0F);
    float3 color = float3(0.0F, 0.0F, 0.0F);
    float strength = 0.0F;
    float reach = range_min;

    uint record = absent;
    uint spawn = 0U;
    if (alive != 0U) {
        const uint oldest = spawn_of(0U);
        const uint newest = spawn_of(alive - 1U);
        const uint c = newest / spacing;
        if (c >= k) {
            // The newest carrier index at or below c that owns slot k.
            uint q = c - (c - k) % light_count;
            [loop] for (uint attempt = 0U; attempt < tries && record == absent; ++attempt) {
                const uint s = q * spacing;
                if (s < oldest) {
                    break;
                }
                record = find(s, alive);
                spawn = s;
                if (q < light_count) {
                    break;
                }
                q -= light_count;
            }
        }
    }
    if (record != absent) {
        const uint base = record * stride;
        const float3 p = asfloat(particles.Load3(base + position_offset));
        position = world0.xyz * p.x + world1.xyz * p.y + world2.xyz * p.z + world3.xyz;
        const float age = asfloat(particles.Load(base + age_offset));
        const float lifetime = asfloat(particles.Load(base + lifetime_offset));
        const float t = lifetime > 0.0F ? saturate(age / lifetime) : 1.0F;
        float4 tint = float4(1.0F, 1.0F, 1.0F, 1.0F);
        if (color_keys != 0U) {
            tint = colour_at(t);
        } else if (color_offset != absent) {
            tint = asfloat(particles.Load4(base + color_offset));
        }
        color = tint.rgb * tint.a;
        strength = lerp(intensity_min, intensity_max, pick(spawn, 0U)) * max(curve_at(t), 0.0F);
        reach = lerp(range_min, range_max, pick(spawn, 1U));
    }
    // vector, kind
    lights.Store3(at, asuint(position));
    lights.Store(at + 12U, light_point);
    // color_intensity
    lights.Store4(at + 16U, asuint(float4(color, strength)));
    // params: range, no cone, no shadow
    lights.Store4(at + 32U, asuint(float4(reach, 0.0F, 0.0F, -1.0F)));
    // spot, ies (no profile), ies_axis, area_u, area_v
    lights.Store4(at + 48U, uint4(0U, 0U, 0U, 0U));
    lights.Store4(at + 64U, asuint(float4(0.0F, 0.0F, 0.0F, -1.0F)));
    lights.Store4(at + 80U, uint4(0U, 0U, 0U, 0U));
    lights.Store4(at + 96U, uint4(0U, 0U, 0U, 0U));
    lights.Store4(at + 112U, uint4(0U, 0U, 0U, 0U));
}
