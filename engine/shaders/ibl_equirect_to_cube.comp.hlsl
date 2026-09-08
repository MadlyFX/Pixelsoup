// One face of the environment cubemap, from the authored equirectangular map.
//
// Dispatched once per face at each environment generation, never per frame. The
// authored rotation is applied here rather than at sampling time so every
// downstream pass - irradiance, prefilter, the sky - sees one already-oriented
// environment and cannot disagree about which way it faces.
#include "ibl_common.hlsli"

cbuffer FaceParameters : register(b0, space1) {
    uint2 face_extent;
    uint face_index;
    float map_rotation;
};

Texture2D<float4> equirect : register(t0);
SamplerState equirect_sampler : register(s0);
RWTexture2D<float4> face_target : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    if (thread.x >= face_extent.x || thread.y >= face_extent.y) {
        return;
    }
    const float2 uv =
        (float2(thread.xy) + 0.5F) / float2(face_extent);
    const float3 direction = ibl_face_direction(face_index, uv);
    const float2 source = ibl_equirect_uv(direction, map_rotation);
    // Level zero explicitly: the source has no mip chain, and letting the
    // hardware pick would make the result depend on the derivative of a
    // coordinate that has none in a compute dispatch.
    const float3 radiance =
        equirect.SampleLevel(equirect_sampler, source, 0.0F).rgb;
    face_target[thread.xy] = float4(radiance, 1.0F);
}
