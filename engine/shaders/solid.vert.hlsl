// A fullscreen triangle from SV_VertexID, with no inputs of any kind.
//
// The minimal pulled draw: no vertex buffer, no storage buffer, no uniform.
// It exists so the RHI can be exercised without also depending on a production
// shader's resource layout - a viewport test that broke because the scene
// shader gained a constant would be testing the wrong thing.
//
// Three vertices covering the whole target. The standard trick: a triangle
// twice the size of the viewport, clipped down to it, which avoids the seam a
// two-triangle quad puts across the diagonal.
float4 main(uint vertex : SV_VertexID) : SV_Position {
    const float2 uv = float2((vertex << 1) & 2, vertex & 2);
    return float4((uv * float2(2.0F, -2.0F)) + float2(-1.0F, 1.0F), 0.0F, 1.0F);
}
