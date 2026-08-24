// The 2D UI, vertex stage. Fully pulled, like the scene stages: no vertex or
// index buffer, the stage reads its own vertex from a storage buffer by
// SV_VertexID plus this batch's base. Two triangles (six vertices) per quad are
// emitted directly, so there is no index buffer.
//
// Positions arrive in pixels and are converted to clip space here, so the CPU
// side of the UI works entirely in pixels. Y is flipped: pixel (0,0) is the top
// left, clip (-1,+1) is the top left.
//
// Register spaces follow the SDL_GPU DXIL convention: storage at (t[n], space0),
// uniforms at (b[n], space1).

struct UiVertex {
    float2 position; // pixels
    float2 uv;
    float4 color; // straight alpha
};

StructuredBuffer<UiVertex> vertices : register(t0, space0);

cbuffer ViewConstants : register(b0, space1) {
    float2 target_size; // pixels
    float2 view_pad;
};

// Where this batch's vertices begin, and the clip rectangle it is scissored to
// (minx, miny, maxx, maxy, in pixels). Carried per batch like the scene path's
// batch offsets; the clip rect rides along so the fragment stage can reject
// pixels outside it (the RHI exposes no scissor).
cbuffer BatchConstants : register(b1, space1) {
    uint4 batch_offsets; // x: first vertex
    float4 clip_rect;
};

struct Varyings {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
    nointerpolation float4 clip_rect : TEXCOORD2;
};

Varyings main(uint vertex_id : SV_VertexID) {
    const UiVertex vertex = vertices[batch_offsets.x + vertex_id];

    Varyings result;
    const float2 ndc = float2(
        vertex.position.x / target_size.x * 2.0 - 1.0,
        1.0 - vertex.position.y / target_size.y * 2.0);
    // z = 1.0 (the far value under the scene's reversed depth). draw_pulled
    // always binds a depth target and clears it to 0.0; a GREATER, non-writing
    // depth test then passes every UI fragment (1.0 > 0.0) while the pipeline's
    // depth-stencil target still matches the pass. Painter's order is the CPU
    // draw order.
    result.position = float4(ndc, 1.0, 1.0);
    result.uv = vertex.uv;
    result.color = vertex.color;
    result.clip_rect = clip_rect;
    return result;
}
