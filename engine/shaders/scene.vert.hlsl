// Scene geometry, vertex stage. Fully pulled: there is no vertex buffer, no
// index buffer, and no vertex input layout anywhere in the pipeline.
//
// The stage reads its own index out of a storage buffer, then its own vertex,
// then its own instance. That costs one extra dependent load per vertex and
// buys three things worth more than the load:
//
//  * the RHI seam needs no vertex input state, so adding a second kind of
//    geometry later changes a shader rather than an interface;
//  * every mesh in the scene lives in one buffer, so switching mesh means
//    changing an offset rather than rebinding;
//  * it is the shape a bindless, GPU-driven pipeline already wants, which is
//    where this is going.
//
// Register spaces follow the SDL_GPU DXIL convention: a vertex stage's storage
// buffers live at (t[n], space0) and its uniform buffers at (b[n], space1).
//
// Matrices are stored as explicit columns rather than as float4x4 because the
// CPU side stores them column-major and a silent transpose is the kind of bug
// that produces a plausible-looking wrong picture. Written out, the contract
// is visible in both files.

struct Vertex {
    float3 position;
    float3 normal;
    float2 uv;
};

struct Instance {
    // Object to world, four columns.
    float4 world0;
    float4 world1;
    float4 world2;
    float4 world3;
    // The inverse transpose of the world basis, three columns. Correct under
    // non-uniform scale, which the world basis alone is not.
    float4 normal0;
    float4 normal1;
    float4 normal2;
    float4 base_color_roughness;
    float4 emissive_metallic;
    // x: opacity, y: horizontal edge blend, z: BlendMode, w: depth-only.
    float4 layer_style;
    // x: first operation, y: operation count. zw are reserved.
    uint4 effect_range;
};

StructuredBuffer<Vertex> vertices : register(t0, space0);
StructuredBuffer<uint> indices : register(t1, space0);
StructuredBuffer<Instance> instances : register(t2, space0);

cbuffer ViewConstants : register(b0, space1) {
    column_major float4x4 view_projection;
};

// Where this batch's slice of the pulled buffers begins.
//
// Passed explicitly rather than as the draw call's first-vertex and
// first-instance arguments because those do not mean the same thing on every
// backend: D3D's SV_InstanceID excludes the draw's first instance while
// Vulkan's gl_InstanceIndex includes it. One shader, correct on both.
cbuffer BatchConstants : register(b1, space1) {
    uint4 batch_offsets; // x: first index, y: first instance
};

// How far a depth-only instance is pushed away from the camera, in normalised
// device depth. Such an instance is drawn on behalf of another compositor
// layer purely to occlude this one, and coplanar content is a layering
// question rather than a depth one: two quads on the same plane must still be
// ordered by the composite, not silently cancelled by a reversed-depth test
// that neither of them can win. The bias makes the occluder lose that tie in
// both directions - it neither hides coplanar geometry nor is hidden by it -
// while remaining thousands of float32 steps below anything an operator could
// place deliberately.
static const float depth_only_bias = 1.0e-5;

struct Varyings {
    float4 position : SV_Position;
    float3 world_position : TEXCOORD0;
    float3 world_normal : TEXCOORD1;
    // Constant across the triangle: the material belongs to the instance, not
    // to the vertex, and interpolating it would be both wasteful and wrong at
    // a shared edge.
    nointerpolation float4 base_color_roughness : TEXCOORD2;
    nointerpolation float4 emissive_metallic : TEXCOORD3;
    // The vertex's primary texture coordinate, consumed by both imported PBR
    // material maps and the media fragment stage.
    float2 uv : TEXCOORD4;
    nointerpolation float4 layer_style : TEXCOORD5;
    nointerpolation uint4 effect_range : TEXCOORD6;
};

Varyings main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    const uint index = indices[batch_offsets.x + vertex_id];
    const Vertex vertex = vertices[index];
    const Instance instance = instances[batch_offsets.y + instance_id];

    const float4 world = instance.world0 * vertex.position.x
        + instance.world1 * vertex.position.y
        + instance.world2 * vertex.position.z + instance.world3;

    const float3 normal = instance.normal0.xyz * vertex.normal.x
        + instance.normal1.xyz * vertex.normal.y
        + instance.normal2.xyz * vertex.normal.z;

    Varyings result;
    result.position = mul(view_projection, world);
    if (instance.layer_style.w > 0.5) {
        // Reversed depth: the near plane is one, so subtracting recedes.
        result.position.z -= depth_only_bias * result.position.w;
    }
    result.world_position = world.xyz;
    // Left unnormalised here and normalised per pixel, so that the
    // interpolation across a curved surface does not shorten it.
    result.world_normal = normal;
    result.base_color_roughness = instance.base_color_roughness;
    result.emissive_metallic = instance.emissive_metallic;
    result.uv = vertex.uv;
    result.layer_style = instance.layer_style;
    result.effect_range = instance.effect_range;
    return result;
}
