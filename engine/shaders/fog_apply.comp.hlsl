// Volumetric fog, apply stage. The integrated volume, resolved at each pixel's
// own depth and composited onto the lit frame.
//
// The froxel this pixel reads is keyed to its **absolute canvas coordinate**,
// not to its position in this node's target. That is the same discipline the
// occlusion kernel's rotation follows and it is the reason two nodes agree: the
// wall pixel decides the cell, never the node that happens to own it.

#include "fog_common.hlsli"

Texture2D<float4> source : register(t0);
SamplerState source_sampler : register(s0);
Texture2D<float> scene_depth : register(t1);
SamplerState depth_sampler : register(s1);
Texture2D<float4> integrated : register(t2);
SamplerState integrated_sampler : register(s2);
RWTexture2D<float4> destination : register(u0);

// Trilinear by hand, in a fixed order, for the reason every filter here is
// written out: a hardware sampler's precision is not guaranteed identical
// between implementations and this value reaches a wall two machines each draw
// half of.
float4 sample_volume(float3 cell) {
    const int3 grid = int3(max(fog_grid.xyz, float3(1.0, 1.0, 1.0)));
    const float3 position = float3(
        clamp(cell.x, 0.0, float(grid.x) - 1.0),
        clamp(cell.y, 0.0, float(grid.y) - 1.0),
        clamp(cell.z, 0.0, float(grid.z) - 1.0));
    const int3 base = int3(floor(position));
    const float3 weight = position - float3(base);

    const float4 c000 = integrated.Load(int3(fog_atlas_texel(base), 0));
    const float4 c100 =
        integrated.Load(int3(fog_atlas_texel(base + int3(1, 0, 0)), 0));
    const float4 c010 =
        integrated.Load(int3(fog_atlas_texel(base + int3(0, 1, 0)), 0));
    const float4 c110 =
        integrated.Load(int3(fog_atlas_texel(base + int3(1, 1, 0)), 0));
    const float4 c001 =
        integrated.Load(int3(fog_atlas_texel(base + int3(0, 0, 1)), 0));
    const float4 c101 =
        integrated.Load(int3(fog_atlas_texel(base + int3(1, 0, 1)), 0));
    const float4 c011 =
        integrated.Load(int3(fog_atlas_texel(base + int3(0, 1, 1)), 0));
    const float4 c111 =
        integrated.Load(int3(fog_atlas_texel(base + int3(1, 1, 1)), 0));

    const float4 near_row = lerp(lerp(c000, c100, weight.x),
                                 lerp(c010, c110, weight.x), weight.y);
    const float4 far_row = lerp(lerp(c001, c101, weight.x),
                                lerp(c011, c111, weight.x), weight.y);
    return lerp(near_row, far_row, weight.z);
}

// The inverse of `fog_slice_depth`, in slices, fractional so the interpolation
// above has something to work with. Square root rather than a general inverse
// power: IEEE 754 requires `sqrt` correctly rounded, which is exactly why the
// forward warp was chosen quadratic.
float fog_slice_of(float view_depth) {
    const float span = max(fog_planes.y - fog_planes.x, 1.0e-6);
    const float fraction = saturate((view_depth - fog_planes.x) / span);
    return sqrt(fraction) * max(fog_grid.z, 1.0);
}

// Integrated texel z contains the answer at the END of slice z. Resolve a
// finite backdrop between those endpoints, including clear air at the near
// plane, so even a distance inside the first cell cannot inherit a full slab.
float4 sample_background_volume(float2 column, float view_depth) {
    const float slice = min(floor(fog_slice_of(view_depth)), fog_grid.z - 1.0);
    const float near_edge = fog_slice_depth(slice);
    const float far_edge = fog_slice_depth(slice + 1.0);
    const float weight = saturate((view_depth - near_edge)
                                  / max(far_edge - near_edge, 1.0e-6));
    if (slice > 0.0) {
        return sample_volume(float3(column, slice - 1.0 + weight));
    }
    return lerp(float4(0.0, 0.0, 0.0, 1.0),
                sample_volume(float3(column, 0.0)), weight);
}

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    uint width = 0U;
    uint height = 0U;
    destination.GetDimensions(width, height);
    const int2 pixel = int2(thread.xy);
    if (pixel.x >= int(width) || pixel.y >= int(height)) {
        return;
    }

    const float4 lit = source.Load(int3(pixel, 0));
    const float depth = scene_depth.Load(int3(pixel, 0));

    // Where this pixel is on the wall, which is what selects the froxel.
    const float2 canvas_pixel = float2(pixel) + fog_canvas.zw;
    const float2 canvas_uv = (canvas_pixel + 0.5) / max(fog_canvas.xy, 1.0);
    const float2 column = canvas_uv * fog_grid.xy - 0.5;

    // Reversed depth: the near plane is one and the far plane is zero, so
    // nothing drawn leaves a zero here and the fog should be integrated all the
    // way to the far plane rather than stopping at the camera.
    float view_depth = fog_planes.y;
    if (depth > 0.0) {
        const float3 world = fog_froxel_world(canvas_uv, fog_planes.x);
        const float4 near_clip = float4(canvas_uv.x * 2.0 - 1.0,
                                        1.0 - canvas_uv.y * 2.0, depth, 1.0);
        const float4 hit = mul(fog_inverse_view_projection, near_clip);
        const float3 hit_world =
            hit.xyz / (abs(hit.w) > 1.0e-12 ? hit.w : 1.0e-12);
        // Planar view depth, recovered by projecting onto the ray the froxel
        // planes are perpendicular to - not the distance to the eye, which
        // would bend the slabs into shells.
        const float3 far_point = fog_froxel_world(canvas_uv, fog_planes.y);
        const float3 axis = far_point - world;
        const float length_squared = max(dot(axis, axis), 1.0e-12);
        const float along = dot(hit_world - world, axis) / length_squared;
        view_depth = fog_planes.x + along * (fog_planes.y - fog_planes.x);
    }

    const bool finite_background = depth <= 0.0 && fog_environment.y > 0.0;
    float4 volume = finite_background
        ? sample_background_volume(column, fog_environment.y)
        : sample_volume(float3(column, fog_slice_of(view_depth)));

    // An HDRI is a backdrop, with no measured surface distance. Integrating
    // to the far clip must not erase it. Limit only legacy background optical
    // coverage; surfaces still receive the full depth-dependent atmosphere.
    // Scale scattering with opacity so the cap does not add excess light.
    // A finite backdrop uses its authored depth and full optical coverage.
    if (depth <= 0.0 && !finite_background && volume.a < 0.15) {
        volume.rgb *= 0.85 / max(1.0 - volume.a, 1.0e-6);
        volume.a = 0.15;
    }

    // Premultiplied throughout, and the alpha is the frame's own: fog adds
    // light and removes light, and does neither to coverage. A layer that
    // fogged its way to a higher alpha would start occluding what is behind it.
    //
    // The in-scattered term is added at full strength, **not** scaled by that
    // alpha, and the reason is worth writing down because the tidier-looking
    // alternative is wrong in practice.
    //
    // `lit.rgb` arrives already multiplied by its own coverage, so attenuating
    // it needs nothing extra. Scaling the in-scatter the same way looks
    // consistent - and on paper it is, for one isolated layer considered alone.
    // But `CompositorSceneExecutor::execute_plan` renders *every* layer with
    // `transparent_background`, including the first, and composites them over a
    // sky the accumulator was cleared to. Alpha-weighted in-scatter is
    // therefore zero everywhere no geometry was drawn, no layer ever owns the
    // background, and the sky comes out of the composite with no atmosphere on
    // it at all - fog visible only where it happens to overlap an object, which
    // is exactly what it looks like.
    //
    // Added raw, a single-layer scene - the common case - gets the background
    // fogged exactly once and correctly. The cost is that a scene composited
    // from several layers fogs the shared background once per layer. That is a
    // real defect and it is recorded in step 11; the honest fix is to apply
    // atmosphere once to the composite rather than inside each layer's render,
    // which needs a combined depth the executor does not have yet.
    destination[pixel] =
        float4(lit.rgb * volume.a + volume.rgb, lit.a);
}
