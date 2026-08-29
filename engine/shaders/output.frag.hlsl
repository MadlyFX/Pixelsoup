// Output transform, pixel stage.
//
// This is the OUTPUT stage of the architecture plan (§1.2): it samples the
// node's source - the region of the shared canvas this node is responsible for,
// already rendered into a texture - onto the output raster.
//
// Today that is a straight sample: this product drives LED walls, where
// adjacent nodes abut pixel-exact and there is no overlap to blend. The stage
// is the seam where the per-panel/per-wall calibration LUT and dither will land
// next; both are colour operations applied last, on the node that owns the
// output.
//
// Register spaces follow the SDL_GPU DXIL convention, the same as the other
// pixel stages here: sampled textures and samplers at (t[n], space2) / (s[n],
// space2).

Texture2D<float4> source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

cbuffer OutputConstants : register(b0, space3) {
    float2 viewport_origin;
    float2 viewport_size;
};

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(Input input) : SV_Target0 {
    const float2 local_uv = (input.uv - viewport_origin) / viewport_size;
    if (any(local_uv < 0.0) || any(local_uv >= 1.0)) {
        discard;
    }
    return source.Sample(source_sampler, local_uv);
}
