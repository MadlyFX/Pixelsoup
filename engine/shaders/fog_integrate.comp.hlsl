// Volumetric fog, integration stage. Front to back along each froxel ray.
//
// One thread per column and row, walking the depth axis in order, because the
// integration is a running product and a parallel scan over 64 slices would
// cost more to synchronise than the loop costs to run.
//
// Beer-Lambert needs an exponential, and that is allowed here. The determinism
// rule this renderer holds is about *filtering* precision - a hardware sampler
// is free to interpolate differently, so every tap is a Load - and about
// transcendentals evaluated on the CPU, where `deterministic::` exists. An HLSL
// `exp` is a defined operation on the same instruction set every node in a
// cohort runs; `gaussian_blur.frag.hlsl` has used one since it was written.

#include "fog_common.hlsli"

Texture2D<float4> scatter : register(t0);
SamplerState scatter_sampler : register(s0);
RWTexture2D<float4> integrated : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    const int3 grid = int3(max(fog_grid.xyz, float3(1.0, 1.0, 1.0)));
    const int2 cell = int2(thread.xy);
    if (cell.x >= grid.x || cell.y >= grid.y) {
        return;
    }

    float3 accumulated = float3(0.0, 0.0, 0.0);
    float transmittance = 1.0;
    float near_edge = fog_slice_depth(0.0);

    for (int slice = 0; slice < grid.z; ++slice) {
        const float far_edge = fog_slice_depth(float(slice) + 1.0);
        const float thickness = max(far_edge - near_edge, 0.0);
        const float4 sample =
            scatter.Load(int3(fog_atlas_texel(int3(cell, slice)), 0));
        const float extinction = max(sample.a, 0.0);

        // The analytic integral of in-scattering across a slab of constant
        // density, rather than a midpoint sample of it. The difference is
        // visible as banding at low slice counts, which is exactly where a
        // 64-slice grid lives.
        const float slab = exp(-extinction * thickness);
        const float3 integral = extinction > 1.0e-6
            ? sample.rgb * (1.0 - slab) / extinction
            : sample.rgb * thickness;

        accumulated += transmittance * integral;
        transmittance *= slab;
        // Written per slice rather than only at the end, because the apply pass
        // reads the volume at the *pixel's* depth - it needs the running answer
        // at every slice, not the total.
        integrated[fog_atlas_texel(int3(cell, slice))] =
            float4(accumulated, transmittance);
        near_edge = far_edge;
    }
}
