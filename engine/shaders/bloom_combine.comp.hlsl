// Bloom, combine stage. The accumulated spill added back to the frame.
//
// Runs at the *padded* extent, before the crop and before the display
// transform, in linear light - which is the only place the addition means
// anything. Adding spill after a display transform adds it to encoded values,
// where a given number is a different amount of light depending on how bright
// the pixel already was, and the result is a bloom that changes character
// across the exposure range for no physical reason.
//
// Writes into level zero of the chain rather than over the frame, so the colour
// the temporal resolve published stays exactly as it was: bloom is not part of
// the history, and a frame that fed its own spill back in would grow one.

Texture2D<float4> source : register(t0);
SamplerState source_sampler : register(s0);
RWTexture2D<float4> destination : register(u0);

cbuffer BloomConstants : register(b0, space1) {
    // xy: destination extent in texels. zw: source extent, equal to it here.
    float4 bloom_extent;
    // x: authored strength. y: unused on this stage.
    float4 bloom_parameters;
};

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    const int2 extent = int2(max(bloom_extent.xy, 1.0));
    const int2 texel = int2(thread.xy);
    if (any(texel >= extent)) {
        return;
    }
    const float4 frame = source.Load(int3(texel, 0));
    const float4 spill = destination[texel];
    // Alpha is the frame's own. Spill carries the premultiplied colour of the
    // highlights that produced it and must not also add coverage - an isolated
    // compositor layer that bloomed its way to a higher alpha would start
    // occluding the layers behind it.
    destination[texel] = float4(
        frame.rgb + spill.rgb * bloom_parameters.x, frame.a);
}
