// Bloom, downsample stage. One level of the chain into the next.
//
// A separable [1,3,3,1]/8 tent over a 4x4 block, and not the 13-tap kernel the
// usual references describe. The 13-tap is defined in terms of *bilinear*
// samples taken at half-texel offsets - that is what makes thirteen fetches
// cover thirty-six texels - and a bilinear tap is exactly what nothing in this
// renderer is allowed to depend on, because filtering precision is not
// guaranteed identical between implementations and this value feeds a wall two
// machines each render half of. Written as explicit taps with fixed weights it
// is the same idea: wide, smooth, and energy conserving, since the weights sum
// to one by construction.
//
// Reads one source texel beyond the 2x2 block it averages, which is what
// `bloom_chain_apron` pays for on every level.

Texture2D<float4> source : register(t0);
SamplerState source_sampler : register(s0);
RWTexture2D<float4> destination : register(u0);

cbuffer BloomConstants : register(b0, space1) {
    // xy: destination extent in texels. zw: source extent in texels.
    float4 bloom_extent;
    // x: strength, unused here. y: whether the destination is accumulated into
    // rather than replaced, unused here. zw: spare.
    float4 bloom_parameters;
};

int2 clamp_texel(int2 texel, int2 extent) {
    return clamp(texel, int2(0, 0), extent - int2(1, 1));
}

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    const int2 destination_extent = int2(max(bloom_extent.xy, 1.0));
    const int2 source_extent = int2(max(bloom_extent.zw, 1.0));
    const int2 texel = int2(thread.xy);
    if (any(texel >= destination_extent)) {
        return;
    }

    // The 4x4 block whose centre is the 2x2 this destination texel covers.
    // Clamped at the target edge, which is correct for both a node holding the
    // canvas edge and a node holding the whole canvas: they clamp in the same
    // place, because the padded rectangle either reaches the canvas boundary on
    // both or reaches neither.
    const int2 base = texel * 2 - int2(1, 1);
    const float weights[4] = {0.125, 0.375, 0.375, 0.125};

    float4 total = float4(0.0, 0.0, 0.0, 0.0);
    [unroll] for (int y = 0; y < 4; ++y) {
        [unroll] for (int x = 0; x < 4; ++x) {
            const int2 tap = clamp_texel(base + int2(x, y), source_extent);
            total += source.Load(int3(tap, 0)) * (weights[x] * weights[y]);
        }
    }
    destination[texel] = total;
}
