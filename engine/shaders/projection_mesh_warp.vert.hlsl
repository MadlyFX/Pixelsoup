// Projective physical-mesh warp, vertex stage.
//
// Destination UV0 places the LED mesh directly into connector clip space.
// source_clip is the same physical vertex seen by the one asymmetric scene
// render. Interpolating the homogeneous value and dividing in the fragment
// stage performs the projective texture lookup over each triangle.

struct WarpVertex {
    float4 destination_clip;
    float4 source_clip;
};

StructuredBuffer<WarpVertex> vertices : register(t0, space0);
StructuredBuffer<uint> indices : register(t1, space0);

cbuffer BatchConstants : register(b1, space1) {
    uint4 batch_offsets; // x: first index
};

struct Varyings {
    float4 position : SV_Position;
    float4 source_clip : TEXCOORD0;
};

Varyings main(uint vertex_id : SV_VertexID) {
    const uint index = indices[batch_offsets.x + vertex_id];
    const WarpVertex vertex = vertices[index];
    Varyings result;
    result.position = vertex.destination_clip;
    result.source_clip = vertex.source_clip;
    return result;
}
