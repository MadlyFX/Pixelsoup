// Reads every binding kind a dispatch can carry, and writes one derived from
// all of them.
//
// One shader rather than four because the thing worth proving is that the four
// tables coexist in a single root signature with the right registers: a shader
// that bound only one would pass with three of them silently misplaced.
//
// It also reads a storage texture written by an earlier dispatch, which is what
// makes it a dependency rather than an independent second pass - the backend
// has to order the two with a UAV barrier, and if it does not, the values read
// here are whatever the earlier dispatch had reached when this one started.

cbuffer GatherParameters : register(b0, space1) {
    uint2 extent;
    uint factor;
    uint gather_padding;
};

// t0 in space 0, with its sampler at s0.
Texture2D<float4> source : register(t0);
SamplerState source_sampler : register(s0);

// Storage buffers follow the sampled textures in the same space, so with one
// sampled texture declared the first buffer is t1. That ordering is the root
// signature's, not a convention this shader chose.
StructuredBuffer<uint> weights : register(t1);

// u0 is the texture the previous dispatch wrote; u1 is this one's output.
RWTexture2D<uint> previous : register(u0);
RWTexture2D<uint> target : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    if (thread.x >= extent.x || thread.y >= extent.y) {
        return;
    }
    const uint index = (thread.y * extent.x) + thread.x;
    // Point-sampled at the texel centre. A filtered sample would make the
    // expected value depend on the sampler's own rounding, which is not what
    // this is testing.
    const float2 uv =
        (float2(thread.xy) + 0.5F) / float2(extent);
    const uint sampled = uint(source.SampleLevel(source_sampler, uv, 0.0F).r
                              * 255.0F + 0.5F);
    target[thread.xy] =
        (previous[thread.xy] * factor) + weights[index] + sampled;
}
