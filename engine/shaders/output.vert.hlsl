// Output transform, vertex stage.
//
// The output stage draws one thing: a single triangle large enough to cover the
// whole target, with texture coordinates running 0..1 across the target (and
// past it, where the triangle is clipped away). There is no vertex buffer, no
// instance buffer, and no uniform here - the three corners come from
// SV_VertexID alone, which is why this seam needs no vertex input state.
//
// This is the fullscreen-triangle trick rather than a two-triangle quad: one
// primitive instead of two, no shared edge down the middle, and the same
// coverage. The pixel stage does the actual work of feed-map cropping and the
// soft-edge blend; see output.frag.hlsl.

struct Output {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Output main(uint vertex_id : SV_VertexID) {
    // vertex 0 -> uv (0,0), vertex 1 -> (2,0), vertex 2 -> (0,2). The triangle
    // through those, mapped to clip space, covers [-1,1] in both axes.
    const float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);

    Output result;
    // uv.y grows downward, so it is flipped into clip space where y is up. This
    // makes uv (0,0) the top-left of the target, the texture convention the
    // pixel stage and the source texture both assume.
    result.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    result.uv = uv;
    return result;
}
