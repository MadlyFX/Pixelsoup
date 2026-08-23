// Instanced quad, vertex stage.
//
// The whole output path is one draw of this: every rectangle the test pattern
// needs - borders, sweep bar, tint bars, and each glyph cell - is one instance
// record. There is no vertex buffer and no uniform buffer, so the per-frame
// upload is the instance array and nothing else.
//
// Bounds arrive already in clip space. Converting on the CPU costs two
// multiplies per rectangle and removes the target-size uniform entirely, which
// keeps the RHI seam free of uniform plumbing it would otherwise need.
//
// Register spaces follow the SDL_GPU DXIL convention: a vertex stage's storage
// buffers live at (t[n], space0).

struct Instance {
    float4 bounds; // x0, y0, x1, y1 in clip space
    float4 color;  // straight alpha
    float4 uv;     // u0, v0, u1, v1 in atlas texture coordinates
};

StructuredBuffer<Instance> instances : register(t0, space0);

struct Output {
    float4 position : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

Output main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    // Two triangles per quad, wound counter-clockwise to match the pipeline's
    // declared front face.
    const float2 corners[6] = {
        float2(0.0, 0.0), float2(0.0, 1.0), float2(1.0, 0.0),
        float2(1.0, 0.0), float2(0.0, 1.0), float2(1.0, 1.0),
    };

    const float2 corner = corners[vertex_id];
    const Instance record = instances[instance_id];

    Output result;
    result.position = float4(
        lerp(record.bounds.x, record.bounds.z, corner.x),
        lerp(record.bounds.y, record.bounds.w, corner.y),
        0.0,
        1.0);
    result.color = record.color;
    result.uv = float2(
        lerp(record.uv.x, record.uv.z, corner.x),
        lerp(record.uv.y, record.uv.w, corner.y));
    return result;
}
