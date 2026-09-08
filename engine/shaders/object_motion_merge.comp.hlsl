Texture2D<float4> source : register(t0);
SamplerState source_sampler : register(s0);
RWTexture2D<float4> destination : register(u0);
cbuffer MotionConstants : register(b0, space1) { float4 extent; float4 motion; };
[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    const int2 at = int2(thread.xy);
    if (any(at >= int2(extent.xy))) return;
    const float4 layer = source.Load(int3(at, 0));
    // Depth-only occluders emit zero; their visible layer contributes the real
    // value. Nearer reversed depth wins; exact ties follow composite order.
    if (layer.z > 0.0 && layer.z >= destination[at].z) destination[at] = layer;
}
