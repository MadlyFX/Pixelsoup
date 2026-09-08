// Nine spherical-harmonic coefficients for the diffuse environment.
//
// SH9 rather than a convolved cubemap: the diffuse term is low-frequency, nine
// coefficients reproduce it to well under a code value on an 8-bit wall, and
// evaluating it in the lit shader is a dot product rather than a cube sample.
//
// **One thread group, on purpose.** This is a reduction over the whole
// environment, and a reduction is where determinism goes wrong: float addition
// is not associative, so a result that depends on which group finished first
// differs between two nodes. Instead a single group of 64 threads each walks a
// fixed stride of the source in increasing order, and the tree reduction below
// runs a fixed number of fixed-width steps. Same bits everywhere, every time.
#include "ibl_common.hlsli"

cbuffer IrradianceParameters : register(b0, space1) {
    uint face_size;
    uint irradiance_padding0;
    uint irradiance_padding1;
    uint irradiance_padding2;
};

TextureCube<float4> environment : register(t0);
SamplerState environment_sampler : register(s0);
// Nine texels, one per coefficient, RGB in xyz. A storage texture rather than a
// buffer because a writable storage buffer does not exist in this RHI yet.
RWTexture2D<float4> coefficients : register(u0);

#define IBL_SH_THREADS 64U

groupshared float3 shared_sh[IBL_SH_THREADS][9];

[numthreads(IBL_SH_THREADS, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    const uint lane = thread.x;
    float3 accumulated[9];
    for (uint slot = 0U; slot < 9U; ++slot) {
        accumulated[slot] = float3(0.0F, 0.0F, 0.0F);
    }

    const uint per_face = face_size * face_size;
    const uint total = per_face * 6U;
    // Increasing index order within each lane's fixed stride.
    for (uint texel = lane; texel < total; texel += IBL_SH_THREADS) {
        const uint face = texel / per_face;
        const uint within = texel - (face * per_face);
        const uint2 coordinate = uint2(within % face_size, within / face_size);
        const float2 uv = (float2(coordinate) + 0.5F) / float(face_size);
        const float3 direction = ibl_face_direction(face, uv);
        const float weight = ibl_texel_solid_angle(uv, face_size);
        const float3 radiance =
            environment.SampleLevel(environment_sampler, direction, 0.0F).rgb;

        float basis[9];
        ibl_sh9(direction, basis);
        for (uint slot = 0U; slot < 9U; ++slot) {
            accumulated[slot] += radiance * (basis[slot] * weight);
        }
    }

    for (uint slot = 0U; slot < 9U; ++slot) {
        shared_sh[lane][slot] = accumulated[slot];
    }
    GroupMemoryBarrierWithGroupSync();

    // Fixed-width tree. Every node performs the same additions in the same
    // order, so the sum is reproducible rather than merely close.
    for (uint stride = IBL_SH_THREADS / 2U; stride > 0U; stride >>= 1U) {
        if (lane < stride) {
            for (uint slot = 0U; slot < 9U; ++slot) {
                shared_sh[lane][slot] += shared_sh[lane + stride][slot];
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (lane == 0U) {
        for (uint slot = 0U; slot < 9U; ++slot) {
            coefficients[uint2(slot, 0U)] =
                float4(shared_sh[0][slot], 1.0F);
        }
    }
}
