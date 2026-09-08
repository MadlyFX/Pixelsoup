// Bloom, upsample stage. A coarser level added back into the level below it.
//
// The interpolation is a two-tap-per-axis tent whose weights come from the
// destination texel's *parity*, which makes them exactly 1/4 and 3/4 - binary
// fractions, identical on every machine, with none of the rounding a hardware
// bilinear unit is free to do differently. A fine texel `d` sits at coarse
// coordinate `d/2 - 1/4`: an even texel therefore leans three quarters onto the
// coarse texel above it and an odd texel three quarters onto the one below.
// That is the whole derivation, and writing it out by hand is what keeps two
// nodes agreeing.
//
// Reads one coarse texel beyond the destination's own footprint, which is the
// other half of what `bloom_chain_apron` pays for.

Texture2D<float4> source : register(t0);
SamplerState source_sampler : register(s0);
RWTexture2D<float4> destination : register(u0);

cbuffer BloomConstants : register(b0, space1) {
    // xy: destination extent in texels. zw: source extent in texels.
    float4 bloom_extent;
    // x: unused here. y: 1 when this level already holds a downsampled image to
    // add to, 0 when it does not and this write establishes it. The last
    // upsample writes the full-resolution level, which the downsample chain
    // never filled - reading it there would accumulate whatever the target
    // happened to contain.
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

    // Parity picks the pair and the weights. The destination index is never
    // negative, so the halving is an exact shift rather than a rounding
    // decision; `base` may be -1 at the first texel, which clamps to the edge
    // and is the correct behaviour there.
    const int2 parity = texel & int2(1, 1);
    const int2 base = int2(parity.x == 0 ? texel.x / 2 - 1 : texel.x / 2,
                           parity.y == 0 ? texel.y / 2 - 1 : texel.y / 2);
    const float2 base_weight = float2(parity.x == 0 ? 0.25 : 0.75,
                                      parity.y == 0 ? 0.25 : 0.75);

    float4 total = float4(0.0, 0.0, 0.0, 0.0);
    [unroll] for (int y = 0; y < 2; ++y) {
        [unroll] for (int x = 0; x < 2; ++x) {
            const float weight =
                (x == 0 ? base_weight.x : 1.0 - base_weight.x)
                * (y == 0 ? base_weight.y : 1.0 - base_weight.y);
            const int2 tap = clamp_texel(base + int2(x, y), source_extent);
            total += source.Load(int3(tap, 0)) * weight;
        }
    }

    // Averaged with what the downsample left here, not added to it.
    //
    // Adding would sum every scale in the chain and hand the combine stage
    // several times the light the frame actually emitted, so an authored
    // strength of one would mean something different on a six-level canvas than
    // on a three-level one. Averaging at each step weights the scales
    // geometrically and leaves the whole chain carrying the energy of the frame
    // it came from, which is what makes the strength a property of the look
    // rather than of the canvas size.
    if (bloom_parameters.y > 0.5) {
        total = 0.5 * (total + destination[texel]);
    }
    destination[texel] = total;
}
