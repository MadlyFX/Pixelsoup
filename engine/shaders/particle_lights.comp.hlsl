// ajfx particle lights: one emitter's light records for the scene's lit pass,
// written into reserved slots of the particle light buffer.
//
// Slot k of n takes particle k when n or fewer are alive, else the particle
// k * alive / n: spread evenly through the live particles in birth order, in
// integers, so every node picks the same ones. A slot with no particle gets a
// light of zero intensity, which the lit pass skips. Each thread writes only
// its own slot.

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
    float intensity;
    float range;
    uint padding;
};

ByteAddressBuffer particles : register(t0);
ByteAddressBuffer counters : register(t1); // the live count first
RWByteAddressBuffer lights : register(u0);   // light.hlsli Light records, 128 bytes each

static const uint absent = 0xffffffffU;
static const uint light_point = 1U;

[numthreads(64, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    const uint k = thread.x;
    if (k >= light_count) {
        return;
    }
    const uint alive = counters.Load(0U);
    const uint at = (first_slot + k) * 128U;
    float3 position = float3(0.0F, 0.0F, 0.0F);
    float4 color = float4(0.0F, 0.0F, 0.0F, 0.0F);
    float strength = 0.0F;
    if (k < alive) {
        const uint record = alive <= light_count ? k : (k * alive) / light_count;
        const float3 p = asfloat(particles.Load3(record * stride + position_offset));
        position = world0.xyz * p.x + world1.xyz * p.y + world2.xyz * p.z + world3.xyz;
        color = color_offset != absent ? asfloat(particles.Load4(record * stride + color_offset))
                                       : float4(1.0F, 1.0F, 1.0F, 1.0F);
        strength = intensity;
    }
    // vector, kind
    lights.Store3(at, asuint(position));
    lights.Store(at + 12U, light_point);
    // color_intensity: the particle's colour, by its alpha
    lights.Store4(at + 16U, asuint(float4(color.rgb * color.a, strength)));
    // params: range, no cone, no shadow
    lights.Store4(at + 32U, asuint(float4(range, 0.0F, 0.0F, -1.0F)));
    // spot, ies (no profile), ies_axis, area_u, area_v
    lights.Store4(at + 48U, uint4(0U, 0U, 0U, 0U));
    lights.Store4(at + 64U, asuint(float4(0.0F, 0.0F, 0.0F, -1.0F)));
    lights.Store4(at + 80U, uint4(0U, 0U, 0U, 0U));
    lights.Store4(at + 96U, uint4(0U, 0U, 0U, 0U));
    lights.Store4(at + 112U, uint4(0U, 0U, 0U, 0U));
}
