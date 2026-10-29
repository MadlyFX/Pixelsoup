// ajfx particle sprites, fragment stage: a soft disc in the particle's
// premultiplied colour. Blended by the pipeline (premultiplied over, or
// additive) and depth-tested against the scene without writing depth.

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

float4 main(Input input) : SV_Target {
    const float coverage = saturate(1.0F - dot(input.uv, input.uv));
    return input.color * coverage;
}
