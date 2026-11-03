// ajfx grid volumes, vertex stage: the grid's box, twelve triangles built in
// world space from the effect's placement, so every node rasterises the same
// box and only the slice's projection differs. Each pixel the box covers then
// marches the view ray through the grid (particle_volume.frag.hlsl).
//
// Both faces are drawn; the pixel stage keeps the back faces only, so a
// camera inside the box still sees the smoke around it.
//
// Register spaces follow the scene stage: uniforms at (b[n], space1).

cbuffer ParticleVolumeView : register(b0, space1) {
    // This slice's projection. Rasterisation only.
    column_major float4x4 view_projection;
    // Effect to world, four columns.
    float4 world0;
    float4 world1;
    float4 world2;
    float4 world3;
    // The grid's box in the effect's space: its lowest corner and its size.
    float4 box_origin;
    float4 box_size;
};

struct Output {
    float4 position : SV_Position;
    // The box's surface at this pixel, in world space.
    float3 world : TEXCOORD0;
};

// The unit cube's corners, round the bottom face then the top, and its
// triangles: each face two of them.
static const float3 corners[8] = {
    float3(0.0f, 0.0f, 0.0f), float3(1.0f, 0.0f, 0.0f), float3(1.0f, 1.0f, 0.0f), float3(0.0f, 1.0f, 0.0f),
    float3(0.0f, 0.0f, 1.0f), float3(1.0f, 0.0f, 1.0f), float3(1.0f, 1.0f, 1.0f), float3(0.0f, 1.0f, 1.0f),
};
static const uint3 faces[12] = {
    uint3(0u, 2u, 1u), uint3(0u, 3u, 2u), // -Z
    uint3(4u, 5u, 6u), uint3(4u, 6u, 7u), // +Z
    uint3(0u, 1u, 5u), uint3(0u, 5u, 4u), // -Y
    uint3(3u, 7u, 6u), uint3(3u, 6u, 2u), // +Y
    uint3(0u, 4u, 7u), uint3(0u, 7u, 3u), // -X
    uint3(1u, 2u, 6u), uint3(1u, 6u, 5u), // +X
};

Output main(uint vertex : SV_VertexID) {
    const uint3 triangle_corners = faces[min(vertex / 3u, 11u)];
    const uint which = vertex % 3u;
    const uint index = which == 0u ? triangle_corners.x : which == 1u ? triangle_corners.y : triangle_corners.z;
    const float3 local = box_origin.xyz + corners[min(index, 7u)] * box_size.xyz;
    const float3 world = world0.xyz * local.x + world1.xyz * local.y + world2.xyz * local.z + world3.xyz;
    Output output;
    output.position = mul(view_projection, float4(world, 1.0f));
    output.world = world;
    return output;
}
