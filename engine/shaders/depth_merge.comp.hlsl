// Nearest-surface depth across compositor layers.
//
// The compositor renders each isolated layer with its own depth, then lays the
// layers over one another. Atmosphere is applied to the finished composite -
// it is light scattered by the air between the camera and whatever it can
// see, not a property of any one layer - and that needs one depth for the
// composite: the nearest surface any layer drew at each pixel.
//
// Depth is reversed - near is one, far is zero, nothing drawn is zero - so the
// nearest surface is the maximum, and a layer that drew nothing at a pixel
// contributes nothing. One dispatch per layer, folded into the running answer
// as each layer lands. A `Load` rather than a sample, for the reason every
// filter here is written out: two nodes must agree to the bit.

Texture2D<float> source : register(t0);
SamplerState source_sampler : register(s0);
RWTexture2D<float> destination : register(u0);

cbuffer DepthMergeConstants : register(b0, space1) {
    // xy: the extent in texels. zw: unused.
    float4 merge_extent;
};

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    const int2 extent = int2(max(merge_extent.xy, 1.0));
    const int2 texel = int2(thread.xy);
    if (any(texel >= extent)) {
        return;
    }
    const float layer = source.Load(int3(texel, 0));
    destination[texel] = max(destination[texel], layer);
}
