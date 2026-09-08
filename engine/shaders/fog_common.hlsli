#ifndef MEDIASERVER_FOG_COMMON_HLSLI
#define MEDIASERVER_FOG_COMMON_HLSLI

// Shared froxel geometry. One definition, because the scatter pass, the
// integration and the per-pixel apply all have to agree about where a cell is -
// and a cell computed one way here and another way there is the same class of
// fault as two nodes disagreeing, only harder to see because it happens inside
// one frame.

cbuffer FogConstants : register(b0, space1) {
    // Canvas-wide clip-to-world. Canvas-wide and not the slice's: the froxel
    // grid partitions the authored full-canvas frustum, so a world point falls
    // in the same cell whichever node is asking. This is the whole contract.
    column_major float4x4 fog_inverse_view_projection;
    // xyz: grid dimensions in froxels. w: the world height the air is densest
    // at and below - see `fog_extinction_at`.
    float4 fog_grid;
    // x: near plane. y: far plane. z: extinction per world unit at and below
    // the base height. w: Henyey-Greenstein asymmetry.
    float4 fog_planes;
    // rgb: scattering albedo. a: height falloff per world unit above the base
    // height; zero is uniform air.
    float4 fog_albedo;
    // xy: the canvas extent in pixels, for turning an absolute canvas pixel
    // into a froxel column. zw: this render's origin in absolute canvas pixels,
    // so the apply pass keys the lookup to the wall rather than to whichever
    // part of it this node owns - the same discipline the occlusion kernel's
    // rotation follows.
    float4 fog_canvas;
    // x: shadow bias in tile texels. A froxel has no normal, so there is no
    // slope term - this is the constant part of what the lit draw applies.
    // y: spare. z: how many rows of the light buffer this frame filled.
    // w: spare.
    float4 fog_shadow;
    // x: resident environment intensity. y: background fog view depth in world
    // units; zero retains the far-clip background. zw: reserved.
    float4 fog_environment;
};

// Where depth slice `index` begins, in view-space units.
//
// Quadratic in the slice fraction rather than the exponential every reference
// uses, and the reason is the one that has decided every other filter here: an
// exponential needs a transcendental evaluated identically on two machines, and
// a quadratic is one multiply.
//
// The same warp as `render::froxel_slice_depth`, and the two are held together
// by inspection rather than by a test - one is HLSL and one is C++, and nothing
// here can call both. The C++ side is the tested one: its inverse is asserted
// on the slice boundaries, which is where an off-by-one would live. This side
// takes a *fractional* index, because the scatter stage asks for slice centres
// and the apply stage interpolates between them; the C++ side takes a whole
// slice index because a partition boundary is all it ever needs.
float fog_slice_depth(float index) {
    const float fraction = index / max(fog_grid.z, 1.0);
    return fog_planes.x + (fog_planes.y - fog_planes.x) * fraction * fraction;
}

// The world position at the centre of a froxel.
//
// Unprojects the cell's canvas UV at the near and far planes and walks along
// that ray, which keeps the interpolation planar - a froxel boundary is a plane
// at a constant view depth, not a shell at a constant distance from the eye.
float3 fog_froxel_world(float2 canvas_uv, float view_depth) {
    const float4 near_clip =
        float4(canvas_uv.x * 2.0 - 1.0, 1.0 - canvas_uv.y * 2.0, 1.0, 1.0);
    const float4 far_clip =
        float4(canvas_uv.x * 2.0 - 1.0, 1.0 - canvas_uv.y * 2.0, 0.0, 1.0);
    const float4 near_world = mul(fog_inverse_view_projection, near_clip);
    const float4 far_world = mul(fog_inverse_view_projection, far_clip);
    const float3 near_point =
        near_world.xyz / (abs(near_world.w) > 1.0e-12 ? near_world.w : 1.0e-12);
    const float3 far_point =
        far_world.xyz / (abs(far_world.w) > 1.0e-12 ? far_world.w : 1.0e-12);
    const float span = max(fog_planes.y - fog_planes.x, 1.0e-6);
    const float t = saturate((view_depth - fog_planes.x) / span);
    return near_point + (far_point - near_point) * t;
}

// The extinction at a world position: full at and below the base height and
// falling off exponentially above it. Uniform when the falloff is zero, which
// is the default and reproduces the previous atmosphere exactly.
//
// This is what lets fog settle on a floor rather than fill the room. `exp` is
// allowed here for the reason the integration stage gives: it is a defined
// operation on the instruction set every node in a cohort runs, and the rule
// this renderer holds is about filtering and CPU-side transcendentals.
float fog_extinction_at(float3 world_position) {
    const float above = max(world_position.y - fog_grid.w, 0.0);
    return fog_planes.z * exp(-above * fog_albedo.a);
}

// Henyey-Greenstein. `cosine` is between the view direction and the direction
// the light arrives from.
//
// This is what makes volumetric fog worth rendering rather than a depth-faded
// tint: forward scattering puts a halo around a practical aimed at the camera,
// which is the single most recognisable thing about real air.
float fog_phase(float cosine, float g) {
    const float gg = g * g;
    const float denominator = 1.0 + gg - 2.0 * g * cosine;
    // The 1/4pi normalisation, so the phase function integrates to one over the
    // sphere and an authored density means the same thing at any anisotropy.
    return (1.0 - gg) / (4.0 * 3.14159265 * pow(max(denominator, 1.0e-4), 1.5));
}

// Slice `z` of the volume lives at columns `z * grid.x` of the atlas, exactly
// as `pack_display_lut_atlas` lays out the display table. A 2D atlas rather
// than a 3D texture because the interpolation below is written out by hand for
// determinism, and once it is manual a 3D texture buys nothing and costs an RHI
// change.
int2 fog_atlas_texel(int3 cell) {
    const int3 clamped =
        clamp(cell, int3(0, 0, 0),
              int3(int(fog_grid.x) - 1, int(fog_grid.y) - 1,
                   int(fog_grid.z) - 1));
    return int2(clamped.z * int(fog_grid.x) + clamped.x, clamped.y);
}

#endif
