// Halves one plane of a decoded video frame, for a clip held at half
// resolution.
//
// Each destination texel is the mean of the 2x2 source block it covers, read
// with explicit loads rather than a filtered fetch: filtering precision is not
// guaranteed identical between implementations, and every node holding this
// clip has to produce the same bytes. The mean is taken on integer codes and
// rounded half up, exactly as the system-memory path does it
// (halve_decoded_frame), so a frame halved on the GPU and one halved on the
// CPU are the same picture. An odd source extent averages its last row or
// column with itself.
//
// Run once for luma and once for the interleaved chroma plane; the unused
// channels of a single-channel plane are carried through and dropped by the
// destination format.

Texture2D<float4> source : register(t0);
RWTexture2D<float4> destination : register(u0);

cbuffer HalveConstants : register(b0) {
    // xy: destination extent in texels. zw: source extent in texels.
    uint4 halve_extent;
    // x: the largest code a sample holds - 255 for eight-bit planes, 65535
    // for sixteen-bit ones. yzw: spare.
    uint4 halve_format;
};

uint4 load_code(uint2 texel, float scale) {
    return (uint4)round(source.Load(int3(texel, 0)) * scale);
}

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    const uint2 destination_extent = halve_extent.xy;
    const uint2 source_extent = halve_extent.zw;
    if (any(thread.xy >= destination_extent)) {
        return;
    }
    const float scale = (float)halve_format.x;
    const uint2 last = source_extent - uint2(1u, 1u);
    const uint2 top_left = min(thread.xy * 2u, last);
    const uint2 bottom_right = min(thread.xy * 2u + uint2(1u, 1u), last);
    const uint4 sum =
        load_code(top_left, scale)
        + load_code(uint2(bottom_right.x, top_left.y), scale)
        + load_code(uint2(top_left.x, bottom_right.y), scale)
        + load_code(bottom_right, scale);
    destination[thread.xy] = (float4)((sum + 2u) / 4u) / scale;
}
