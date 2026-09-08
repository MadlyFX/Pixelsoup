// Writes one value per texel, derived from the texel's own coordinates.
//
// The first callable compute shader in this RHI, and it exists to be asserted
// against rather than looked at: every output byte is predictable from the
// dispatch parameters alone, so a test can compare the whole readback exactly
// instead of checking that something changed.
//
// Order-independent by construction. Each thread writes one texel it alone
// owns, so the result cannot depend on which group finishes first - which is
// the property every compute pass feeding pixels has to have, because two
// render nodes running this must produce the same bits.

// Uniform bytes land at b0 in space 1. See the compute root signature in
// d3d12_backend.cpp: spaces are separated by binding kind so a shader author
// need not count the bindings that came before.
cbuffer FillParameters : register(b0, space1) {
    uint2 extent;
    uint seed;
    uint fill_padding;
};

RWTexture2D<uint> target : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    // The dispatch rounds up to whole groups, so the last group along each
    // axis runs threads past the edge. They return rather than clamp: a
    // clamped thread would write a texel another thread owns, which is the
    // exact race this pass must not have.
    if (thread.x >= extent.x || thread.y >= extent.y) {
        return;
    }
    target[thread.xy] = seed + (thread.y * extent.x) + thread.x;
}
